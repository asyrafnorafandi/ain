// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2019 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <miner.h>

#include <amount.h>
#include <chain.h>
#include <chainparams.h>
#include <coins.h>
#include <consensus/tx_check.h>
#include <consensus/consensus.h>
#include <consensus/merkle.h>
#include <consensus/tx_verify.h>
#include <consensus/validation.h>
#include <masternodes/anchors.h>
#include <masternodes/criminals.h>
#include <masternodes/masternodes.h>
#include <masternodes/mn_checks.h>
#include <net.h>
#include <policy/feerate.h>
#include <policy/policy.h>
#include <pos.h>
#include <pos_kernel.h>
#include <primitives/transaction.h>
#include <script/standard.h>
#include <util/moneystr.h>
#include <util/system.h>
#include <util/validation.h>
#include <wallet/wallet.h>

#include <algorithm>
#include <queue>
#include <utility>

int64_t UpdateTime(CBlockHeader* pblock, const Consensus::Params& consensusParams, const CBlockIndex* pindexPrev)
{
    int64_t nOldTime = pblock->nTime;
    int64_t nNewTime = std::max(pindexPrev->GetMedianTimePast()+1, GetAdjustedTime());

    if (nOldTime < nNewTime)
        pblock->nTime = nNewTime;

    // Updating time can change work required on testnet:
    if (consensusParams.pos.fAllowMinDifficultyBlocks)
        pblock->nBits = pos::GetNextWorkRequired(pindexPrev, pblock, consensusParams.pos);

    return nNewTime - nOldTime;
}

BlockAssembler::Options::Options() {
    blockMinFeeRate = CFeeRate(DEFAULT_BLOCK_MIN_TX_FEE);
    nBlockMaxWeight = DEFAULT_BLOCK_MAX_WEIGHT;
}

BlockAssembler::BlockAssembler(const CChainParams& params, const Options& options) : chainparams(params)
{
    blockMinFeeRate = options.blockMinFeeRate;
    // Limit weight to between 4K and MAX_BLOCK_WEIGHT-4K for sanity:
    nBlockMaxWeight = std::max<size_t>(4000, std::min<size_t>(MAX_BLOCK_WEIGHT - 4000, options.nBlockMaxWeight));
}

static BlockAssembler::Options DefaultOptions()
{
    // Block resource limits
    // If -blockmaxweight is not given, limit to DEFAULT_BLOCK_MAX_WEIGHT
    BlockAssembler::Options options;
    options.nBlockMaxWeight = gArgs.GetArg("-blockmaxweight", DEFAULT_BLOCK_MAX_WEIGHT);
    CAmount n = 0;
    if (gArgs.IsArgSet("-blockmintxfee") && ParseMoney(gArgs.GetArg("-blockmintxfee", ""), n)) {
        options.blockMinFeeRate = CFeeRate(n);
    } else {
        options.blockMinFeeRate = CFeeRate(DEFAULT_BLOCK_MIN_TX_FEE);
    }
    return options;
}

BlockAssembler::BlockAssembler(const CChainParams& params) : BlockAssembler(params, DefaultOptions()) {}

void BlockAssembler::resetBlock()
{
    inBlock.clear();

    // Reserve space for coinbase tx
    nBlockWeight = 4000;
    nBlockSigOpsCost = 400;
    fIncludeWitness = false;

    // These counters do not include coinbase tx
    nBlockTx = 0;
    nFees = 0;
}

Optional<int64_t> BlockAssembler::m_last_block_num_txs{nullopt};
Optional<int64_t> BlockAssembler::m_last_block_weight{nullopt};

std::unique_ptr<CBlockTemplate> BlockAssembler::CreateNewBlock(const CScript& scriptPubKeyIn)
{
    int64_t nTimeStart = GetTimeMicros();

    resetBlock();

    pblocktemplate.reset(new CBlockTemplate());

    if(!pblocktemplate.get())
        return nullptr;
    pblock = &pblocktemplate->block; // pointer for convenience

    // Add dummy coinbase tx as first transaction
    pblock->vtx.emplace_back();
    pblocktemplate->vTxFees.push_back(-1); // updated at end
    pblocktemplate->vTxSigOpsCost.push_back(-1); // updated at end

    LOCK2(cs_main, mempool.cs);
    // in fact, this may be redundant cause it was checked upthere in the miner
    auto myIDs = pcustomcsview->AmIOperator();
    if (!myIDs)
        return nullptr;
    auto nodePtr = pcustomcsview->GetMasternode(myIDs->second);
    if (!nodePtr || !nodePtr->IsActive())
        return nullptr;

    CBlockIndex* pindexPrev = ::ChainActive().Tip();
    assert(pindexPrev != nullptr);
    nHeight = pindexPrev->nHeight + 1;

    pblock->nVersion = ComputeBlockVersion(pindexPrev, chainparams.GetConsensus());
    // -regtest only: allow overriding block.nVersion with
    // -blockversion=N to test forking scenarios
    if (chainparams.MineBlocksOnDemand())
        pblock->nVersion = gArgs.GetArg("-blockversion", pblock->nVersion);

    pblock->nTime = GetAdjustedTime();
    const int64_t nMedianTimePast = pindexPrev->GetMedianTimePast();

    nLockTimeCutoff = (STANDARD_LOCKTIME_VERIFY_FLAGS & LOCKTIME_MEDIAN_TIME_PAST)
                       ? nMedianTimePast
                       : pblock->GetBlockTime();

    // Decide whether to include witness transactions
    // This is only needed in case the witness softfork activation is reverted
    // (which would require a very deep reorganization).
    // Note that the mempool would accept transactions with witness data before
    // IsWitnessEnabled, but we would only ever mine blocks after IsWitnessEnabled
    // unless there is a massive block reorganization with the witness softfork
    // not activated.
    // TODO: replace this with a call to main to assess validity of a mempool
    // transaction (which in most cases can be a no-op).
    fIncludeWitness = IsWitnessEnabled(pindexPrev, chainparams.GetConsensus());

    const auto txVersion = GetTransactionVersion(nHeight);

    auto currentTeam = pcustomcsview->GetCurrentTeam();
    auto confirms = panchorAwaitingConfirms->GetQuorumFor(currentTeam);
    if (confirms.size() > 0) { // quorum or zero

        CAnchorFinalizationMessage finMsg{confirms[0]};
        finMsg.nextTeam = pcustomcsview->CalcNextTeam(pindexPrev->stakeModifier);
        finMsg.currentTeam = currentTeam;
        for (auto const & msg : confirms) {
            finMsg.sigs.push_back(msg.signature);
        }

        CDataStream metadata(DfAnchorFinalizeTxMarker, SER_NETWORK, PROTOCOL_VERSION);
        metadata << finMsg;

        CTxDestination destination = finMsg.rewardKeyType == 1 ? CTxDestination(PKHash(finMsg.rewardKeyID)) : CTxDestination(WitnessV0KeyHash(finMsg.rewardKeyID));

        CMutableTransaction mTx(txVersion);
        mTx.vin.resize(1);
        mTx.vin[0].prevout.SetNull();
        mTx.vin[0].scriptSig = CScript() << nHeight << OP_0;
        mTx.vout.resize(2);
        mTx.vout[0].scriptPubKey = CScript() << OP_RETURN << ToByteVector(metadata);
        mTx.vout[0].nValue = 0;
        mTx.vout[1].scriptPubKey = GetScriptForDestination(destination);

        if (nHeight >= chainparams.GetConsensus().AMKHeight) {
            mTx.vout[1].nValue = pcustomcsview->GetCommunityBalance(CommunityAccountType::AnchorReward); // do not reset it, so it will occure on connectblock
        }
        else { // pre-AMK logic:
            mTx.vout[1].nValue = GetAnchorSubsidy(finMsg.anchorHeight, finMsg.prevAnchorHeight, chainparams.GetConsensus());
        }

        auto rewardTx = pcustomcsview->GetRewardForAnchor(finMsg.btcTxHash);
        if (!rewardTx) {
            LogPrintf("AnchorConfirms::CreateNewBlock(): create finalization tx: %s block: %d\n", mTx.GetHash().GetHex(), nHeight);
            pblock->vtx.push_back(MakeTransactionRef(std::move(mTx)));
            pblocktemplate->vTxFees.push_back(0);
            pblocktemplate->vTxSigOpsCost.push_back(WITNESS_SCALE_FACTOR * GetLegacySigOpCount(*pblock->vtx.back()));
        }else {
            LogPrintf("AnchorConfirms::CreateNewBlock(): reward for anchor %s already exists (tx: %s), skip reward again\n",
                finMsg.btcTxHash.ToString(), (*rewardTx).ToString());
        }

        // DO NOT erase votes here! they'll be cleaned after block connection (ONLY!)
//        panchorAwaitingConfirms->EraseAnchor(confirmsForAnchor.first);
    }

    CTransactionRef criminalTx = nullptr;
    if (fCriminals) {
        CCriminalProofsView::CMnCriminals criminals = pcriminals->GetUnpunishedCriminals();
        if (criminals.size() != 0) {
            CCriminalProofsView::CMnCriminals::iterator itCriminalMN = criminals.begin();
            auto const & proof = itCriminalMN->second;
            CKeyID minter;
            assert(IsDoubleSigned(proof.blockHeader, proof.conflictBlockHeader, minter));
            // not necessary - checked by GetUnpunishedCriminals()
//            auto itFirstMN = penhancedview->GetMasternodeIdByOperator(minter);
//            assert(itFirstMN && (*itFirstMN) == itCriminalMN->first);

            CDataStream metadata(DfCriminalTxMarker, SER_NETWORK, PROTOCOL_VERSION);
            metadata << proof.blockHeader << proof.conflictBlockHeader << itCriminalMN->first;

            CMutableTransaction newCriminalTx(txVersion);
            newCriminalTx.vin.resize(1);
            newCriminalTx.vin[0].prevout.SetNull();
            newCriminalTx.vin[0].scriptSig = CScript() << nHeight << OP_0;
            newCriminalTx.vout.resize(1);
            newCriminalTx.vout[0].scriptPubKey = CScript() << OP_RETURN << ToByteVector(metadata);
            newCriminalTx.vout[0].nValue = 0;

            pblock->vtx.push_back(MakeTransactionRef(std::move(newCriminalTx)));
            criminalTx = pblock->vtx.back();

            pblocktemplate->vTxFees.push_back(0);
            pblocktemplate->vTxSigOpsCost.push_back(WITNESS_SCALE_FACTOR * GetLegacySigOpCount(*pblock->vtx.back()));
        }
    }

    int nPackagesSelected = 0;
    int nDescendantsUpdated = 0;
    addPackageTxs(nPackagesSelected, nDescendantsUpdated, nHeight);

    int64_t nTime1 = GetTimeMicros();

    m_last_block_num_txs = nBlockTx;
    m_last_block_weight = nBlockWeight;

    // Create coinbase transaction.
    CMutableTransaction coinbaseTx(txVersion);
    coinbaseTx.vin.resize(1);
    coinbaseTx.vin[0].prevout.SetNull();
    coinbaseTx.vin[0].scriptSig = CScript() << nHeight << OP_0;
    coinbaseTx.vout.resize(1);
    coinbaseTx.vout[0].scriptPubKey = scriptPubKeyIn;
    CAmount blockReward = GetBlockSubsidy(nHeight, chainparams.GetConsensus());
    coinbaseTx.vout[0].nValue = nFees + blockReward;

    if (nHeight >= chainparams.GetConsensus().AMKHeight) {
        // assume community non-utxo funding:
        for (auto kv : chainparams.GetConsensus().nonUtxoBlockSubsidies) {
            coinbaseTx.vout[0].nValue -= blockReward * kv.second / COIN;
        }
        // Pinch off foundation share
        if (!chainparams.GetConsensus().foundationShareScript.empty() && chainparams.GetConsensus().foundationShareDFIP1 != 0) {
            coinbaseTx.vout.resize(2);
            coinbaseTx.vout[1].scriptPubKey = chainparams.GetConsensus().foundationShareScript;
            coinbaseTx.vout[1].nValue = blockReward * chainparams.GetConsensus().foundationShareDFIP1 / COIN; // the main difference is that new FS is a %% from "base" block reward and no fees involved
            coinbaseTx.vout[0].nValue -= coinbaseTx.vout[1].nValue;

            LogPrintf("CreateNewBlock(): post AMK logic, foundation share %d\n", coinbaseTx.vout[1].nValue);
        }
    }
    else { // pre-AMK logic:
        // Pinch off foundation share
        CAmount foundationsReward = coinbaseTx.vout[0].nValue * chainparams.GetConsensus().foundationShare / 100;
        if (!chainparams.GetConsensus().foundationShareScript.empty() && chainparams.GetConsensus().foundationShare != 0) {
            if (pcustomcsview->GetFoundationsDebt() < foundationsReward) {
                coinbaseTx.vout.resize(2);
                coinbaseTx.vout[1].scriptPubKey = chainparams.GetConsensus().foundationShareScript;
                coinbaseTx.vout[1].nValue = foundationsReward - pcustomcsview->GetFoundationsDebt();
                coinbaseTx.vout[0].nValue -= coinbaseTx.vout[1].nValue;

                LogPrintf("CreateNewBlock(): pre AMK logic, foundation share %d\n", coinbaseTx.vout[1].nValue);
            } else {
                pcustomcsview->SetFoundationsDebt(pcustomcsview->GetFoundationsDebt() - foundationsReward);
            }
        }
    }

    pblock->vtx[0] = MakeTransactionRef(std::move(coinbaseTx));

    pblocktemplate->vchCoinbaseCommitment = GenerateCoinbaseCommitment(*pblock, pindexPrev, chainparams.GetConsensus());
    pblocktemplate->vTxFees[0] = -nFees;

    LogPrintf("CreateNewBlock(): block weight: %u txs: %u fees: %ld sigops %d\n", GetBlockWeight(*pblock), nBlockTx, nFees, nBlockSigOpsCost);

    // Fill in header
    pblock->hashPrevBlock  = pindexPrev->GetBlockHash();
    UpdateTime(pblock, chainparams.GetConsensus(), pindexPrev);
    pblock->nBits          = pos::GetNextWorkRequired(pindexPrev, pblock, chainparams.GetConsensus().pos);
    pblock->stakeModifier  = pos::ComputeStakeModifier(pindexPrev->stakeModifier, myIDs->first);

    pblocktemplate->vTxSigOpsCost[0] = WITNESS_SCALE_FACTOR * GetLegacySigOpCount(*pblock->vtx[0]);

    CValidationState state;
    if (!TestBlockValidity(state, chainparams, *pblock, pindexPrev, false)) {
        throw std::runtime_error(strprintf("%s: TestBlockValidity failed: %s", __func__, FormatStateMessage(state)));
    }
    int64_t nTime2 = GetTimeMicros();

    LogPrint(BCLog::BENCH, "CreateNewBlock() packages: %.2fms (%d packages, %d updated descendants), validity: %.2fms (total %.2fms)\n", 0.001 * (nTime1 - nTimeStart), nPackagesSelected, nDescendantsUpdated, 0.001 * (nTime2 - nTime1), 0.001 * (nTime2 - nTimeStart));

    return std::move(pblocktemplate);
}

void BlockAssembler::onlyUnconfirmed(CTxMemPool::setEntries& testSet)
{
    for (CTxMemPool::setEntries::iterator iit = testSet.begin(); iit != testSet.end(); ) {
        // Only test txs not already in the block
        if (inBlock.count(*iit)) {
            testSet.erase(iit++);
        }
        else {
            iit++;
        }
    }
}

bool BlockAssembler::TestPackage(uint64_t packageSize, int64_t packageSigOpsCost) const
{
    // TODO: switch to weight-based accounting for packages instead of vsize-based accounting.
    if (nBlockWeight + WITNESS_SCALE_FACTOR * packageSize >= nBlockMaxWeight)
        return false;
    if (nBlockSigOpsCost + packageSigOpsCost >= MAX_BLOCK_SIGOPS_COST)
        return false;
    return true;
}

// Perform transaction-level checks before adding to block:
// - transaction finality (locktime)
// - premature witness (in case segwit transactions are added to mempool before
//   segwit activation)
bool BlockAssembler::TestPackageTransactions(const CTxMemPool::setEntries& package)
{
    for (CTxMemPool::txiter it : package) {
        if (!IsFinalTx(it->GetTx(), nHeight, nLockTimeCutoff))
            return false;
        if (!fIncludeWitness && it->GetTx().HasWitness())
            return false;
    }
    return true;
}

void BlockAssembler::AddToBlock(CTxMemPool::txiter iter)
{
    pblock->vtx.emplace_back(iter->GetSharedTx());
    pblocktemplate->vTxFees.push_back(iter->GetFee());
    pblocktemplate->vTxSigOpsCost.push_back(iter->GetSigOpCost());
    nBlockWeight += iter->GetTxWeight();
    ++nBlockTx;
    nBlockSigOpsCost += iter->GetSigOpCost();
    nFees += iter->GetFee();
    inBlock.insert(iter);

    bool fPrintPriority = gArgs.GetBoolArg("-printpriority", DEFAULT_PRINTPRIORITY);
    if (fPrintPriority) {
        LogPrintf("fee %s txid %s\n",
                  CFeeRate(iter->GetModifiedFee(), iter->GetTxSize()).ToString(),
                  iter->GetTx().GetHash().ToString());
    }
}

int BlockAssembler::UpdatePackagesForAdded(const CTxMemPool::setEntries& alreadyAdded,
        indexed_modified_transaction_set &mapModifiedTx)
{
    int nDescendantsUpdated = 0;
    for (CTxMemPool::txiter it : alreadyAdded) {
        CTxMemPool::setEntries descendants;
        mempool.CalculateDescendants(it, descendants);
        // Insert all descendants (not yet in block) into the modified set
        for (CTxMemPool::txiter desc : descendants) {
            if (alreadyAdded.count(desc))
                continue;
            ++nDescendantsUpdated;
            modtxiter mit = mapModifiedTx.find(desc);
            if (mit == mapModifiedTx.end()) {
                CTxMemPoolModifiedEntry modEntry(desc);
                modEntry.nSizeWithAncestors -= it->GetTxSize();
                modEntry.nModFeesWithAncestors -= it->GetModifiedFee();
                modEntry.nSigOpCostWithAncestors -= it->GetSigOpCost();
                mapModifiedTx.insert(modEntry);
            } else {
                mapModifiedTx.modify(mit, update_for_parent_inclusion(it));
            }
        }
    }
    return nDescendantsUpdated;
}

// Skip entries in mapTx that are already in a block or are present
// in mapModifiedTx (which implies that the mapTx ancestor state is
// stale due to ancestor inclusion in the block)
// Also skip transactions that we've already failed to add. This can happen if
// we consider a transaction in mapModifiedTx and it fails: we can then
// potentially consider it again while walking mapTx.  It's currently
// guaranteed to fail again, but as a belt-and-suspenders check we put it in
// failedTx and avoid re-evaluation, since the re-evaluation would be using
// cached size/sigops/fee values that are not actually correct.
bool BlockAssembler::SkipMapTxEntry(CTxMemPool::txiter it, indexed_modified_transaction_set &mapModifiedTx, CTxMemPool::setEntries &failedTx)
{
    assert (it != mempool.mapTx.end());
    return mapModifiedTx.count(it) || inBlock.count(it) || failedTx.count(it);
}

void BlockAssembler::SortForBlock(const CTxMemPool::setEntries& package, std::vector<CTxMemPool::txiter>& sortedEntries)
{
    // Sort package by ancestor count
    // If a transaction A depends on transaction B, then A's ancestor count
    // must be greater than B's.  So this is sufficient to validly order the
    // transactions for block inclusion.
    sortedEntries.clear();
    sortedEntries.insert(sortedEntries.begin(), package.begin(), package.end());
    std::sort(sortedEntries.begin(), sortedEntries.end(), CompareTxIterByAncestorCount());
}

// This transaction selection algorithm orders the mempool based
// on feerate of a transaction including all unconfirmed ancestors.
// Since we don't remove transactions from the mempool as we select them
// for block inclusion, we need an alternate method of updating the feerate
// of a transaction with its not-yet-selected ancestors as we go.
// This is accomplished by walking the in-mempool descendants of selected
// transactions and storing a temporary modified state in mapModifiedTxs.
// Each time through the loop, we compare the best transaction in
// mapModifiedTxs with the next transaction in the mempool to decide what
// transaction package to work on next.
void BlockAssembler::addPackageTxs(int &nPackagesSelected, int &nDescendantsUpdated, int nHeight)
{
    // mapModifiedTx will store sorted packages after they are modified
    // because some of their txs are already in the block
    indexed_modified_transaction_set mapModifiedTx;
    // Keep track of entries that failed inclusion, to avoid duplicate work
    CTxMemPool::setEntries failedTx;

    // Start by adding all descendants of previously added txs to mapModifiedTx
    // and modifying them for their already included ancestors
    UpdatePackagesForAdded(inBlock, mapModifiedTx);

    CTxMemPool::indexed_transaction_set::index<ancestor_score>::type::iterator mi = mempool.mapTx.get<ancestor_score>().begin();
    CTxMemPool::txiter iter;

    // Limit the number of attempts to add transactions to the block when it is
    // close to full; this is just a simple heuristic to finish quickly if the
    // mempool has a lot of entries.
    const int64_t MAX_CONSECUTIVE_FAILURES = 1000;
    int64_t nConsecutiveFailed = 0;

    // Custom TXs to undo at the end
    std::set<uint256> checkedTX;

    // Copy of the view
    CCustomCSView view(*pcustomcsview);

    while (mi != mempool.mapTx.get<ancestor_score>().end() || !mapModifiedTx.empty())
    {
        // First try to find a new transaction in mapTx to evaluate.
        if (mi != mempool.mapTx.get<ancestor_score>().end() &&
                SkipMapTxEntry(mempool.mapTx.project<0>(mi), mapModifiedTx, failedTx)) {
            ++mi;
            continue;
        }

        // Now that mi is not stale, determine which transaction to evaluate:
        // the next entry from mapTx, or the best from mapModifiedTx?
        bool fUsingModified = false;

        modtxscoreiter modit = mapModifiedTx.get<ancestor_score>().begin();
        if (mi == mempool.mapTx.get<ancestor_score>().end()) {
            // We're out of entries in mapTx; use the entry from mapModifiedTx
            iter = modit->iter;
            fUsingModified = true;
        } else {
            // Try to compare the mapTx entry to the mapModifiedTx entry
            iter = mempool.mapTx.project<0>(mi);
            if (modit != mapModifiedTx.get<ancestor_score>().end() &&
                    CompareTxMemPoolEntryByAncestorFee()(*modit, CTxMemPoolModifiedEntry(iter))) {
                // The best entry in mapModifiedTx has higher score
                // than the one from mapTx.
                // Switch which transaction (package) to consider
                iter = modit->iter;
                fUsingModified = true;
            } else {
                // Either no entry in mapModifiedTx, or it's worse than mapTx.
                // Increment mi for the next loop iteration.
                ++mi;
            }
        }

        // We skip mapTx entries that are inBlock, and mapModifiedTx shouldn't
        // contain anything that is inBlock.
        assert(!inBlock.count(iter));

        uint64_t packageSize = iter->GetSizeWithAncestors();
        CAmount packageFees = iter->GetModFeesWithAncestors();
        int64_t packageSigOpsCost = iter->GetSigOpCostWithAncestors();
        if (fUsingModified) {
            packageSize = modit->nSizeWithAncestors;
            packageFees = modit->nModFeesWithAncestors;
            packageSigOpsCost = modit->nSigOpCostWithAncestors;
        }

        if (packageFees < blockMinFeeRate.GetFee(packageSize)) {
            // Everything else we might consider has a lower fee rate
            break;
        }

        if (!TestPackage(packageSize, packageSigOpsCost)) {
            if (fUsingModified) {
                // Since we always look at the best entry in mapModifiedTx,
                // we must erase failed entries so that we can consider the
                // next best entry on the next loop iteration
                mapModifiedTx.get<ancestor_score>().erase(modit);
                failedTx.insert(iter);
            }

            ++nConsecutiveFailed;

            if (nConsecutiveFailed > MAX_CONSECUTIVE_FAILURES && nBlockWeight >
                    nBlockMaxWeight - 4000) {
                // Give up if we're close to full and haven't succeeded in a while
                break;
            }
            continue;
        }

        CTxMemPool::setEntries ancestors;
        uint64_t nNoLimit = std::numeric_limits<uint64_t>::max();
        std::string dummy;
        mempool.CalculateMemPoolAncestors(*iter, ancestors, nNoLimit, nNoLimit, nNoLimit, nNoLimit, dummy, false);

        onlyUnconfirmed(ancestors);
        ancestors.insert(iter);

        // Test if all tx's are Final
        if (!TestPackageTransactions(ancestors)) {
            if (fUsingModified) {
                mapModifiedTx.get<ancestor_score>().erase(modit);
                failedTx.insert(iter);
            }
            continue;
        }

        // This transaction will make it in; reset the failed counter.
        nConsecutiveFailed = 0;

        // Package can be added. Sort the entries in a valid order.
        std::vector<CTxMemPool::txiter> sortedEntries;
        SortForBlock(ancestors, sortedEntries);

        // Account check
        bool customTxPassed{true};

        // Apply and check custom TXs in order
        for (size_t i = 0; i < sortedEntries.size(); ++i) {
            const CTransaction& tx = sortedEntries[i]->GetTx();

            // Do not double check already checked custom TX. This will be an ancestor of current TX.
            if (checkedTX.find(tx.GetHash()) != checkedTX.end()) {
                continue;
            }

            std::vector<unsigned char> metadata;
            CustomTxType txType = GuessCustomTxType(tx, metadata);

            // Only check custom TXs
            if (txType != CustomTxType::None) {
                auto res = ApplyCustomTx(view, ::ChainstateActive().CoinsTip(), tx, chainparams.GetConsensus(), nHeight, 0, false, true);

                // Not okay invalidate, undo and skip
                if (!res.ok) {
                    customTxPassed = false;

                    LogPrintf("%s: Failed %s TX %s: %s\n", __func__, ToString(txType), tx.GetHash().GetHex(), res.msg);

                    break;
                }

                // Track checked TXs to avoid double applying
                checkedTX.insert(tx.GetHash());
            }
        }

        // Failed, let's move on!
        if (!customTxPassed) {
            if (fUsingModified) {
                mapModifiedTx.get<ancestor_score>().erase(modit);
                failedTx.insert(iter);
            }
            continue;
        }

        for (size_t i=0; i<sortedEntries.size(); ++i) {
            AddToBlock(sortedEntries[i]);
            // Erase from the modified set, if present
            mapModifiedTx.erase(sortedEntries[i]);
        }

        ++nPackagesSelected;

        // Update transactions that depend on each of these
        nDescendantsUpdated += UpdatePackagesForAdded(ancestors, mapModifiedTx);
    }
}

void IncrementExtraNonce(CBlock* pblock, const CBlockIndex* pindexPrev, unsigned int& nExtraNonce)
{
    // Update nExtraNonce
    static uint256 hashPrevBlock;
    if (hashPrevBlock != pblock->hashPrevBlock)
    {
        nExtraNonce = 0;
        hashPrevBlock = pblock->hashPrevBlock;
    }
    ++nExtraNonce;
    unsigned int nHeight = pindexPrev->nHeight+1; // Height first in coinbase required for block.version=2
    CMutableTransaction txCoinbase(*pblock->vtx[0]);
    txCoinbase.vin[0].scriptSig = (CScript() << nHeight << CScriptNum(nExtraNonce)) + COINBASE_FLAGS;
    assert(txCoinbase.vin[0].scriptSig.size() <= 100);

    pblock->vtx[0] = MakeTransactionRef(std::move(txCoinbase));
    pblock->hashMerkleRoot = BlockMerkleRoot(*pblock);
}

namespace pos {
    Staker::Status Staker::stake(CChainParams chainparams, const ThreadStaker::Args& args) {
        if (!chainparams.GetConsensus().pos.allowMintingWithoutPeers) {
            if(!g_connman)
                throw std::runtime_error("Error: Peer-to-peer functionality missing or disabled");

            if (!chainparams.GetConsensus().pos.allowMintingWithoutPeers && g_connman->GetNodeCount(CConnman::CONNECTIONS_ALL) == 0)
                return Status::initWaiting;
        }

        if (nLastSystemTime.time_since_epoch().count() != 0 && nLastSteadyTime.time_since_epoch().count() != 0) {
            using namespace std::chrono;
            const auto systemNow = system_clock::now();
            const auto steadyNow = steady_clock::now();
            const auto nSystemClockDifference = duration_cast<milliseconds>(systemNow - nLastSystemTime);
            const auto nSteadyClockDifference = duration_cast<milliseconds>(steadyNow - nLastSteadyTime);

            if(std::abs((nSystemClockDifference - nSteadyClockDifference).count()) > 1000) { //1s
                //LogPrintf("*** System clock change detected. Staking will be paused until the clock is synced again.\n");
                // TODO: call a NTP syncing routine
                //return Status::initWaiting;
            }
        }

        nLastSystemTime = std::chrono::system_clock::now();
        nLastSteadyTime = std::chrono::steady_clock::now();

        bool minted = false;
        bool potentialCriminalBlock = false;

        CBlockIndex* tip = getTip();

        // this part of code stay valid until tip got changed
        /// @todo is 'tip' can be changed here? is it possible to pull 'getTip()' and mnview access to the upper (calling 'stake()') block?
        uint32_t mintedBlocks(0);
        uint256 masternodeID{};
        CScript defaultScript;
        {
            LOCK(cs_main);
            auto optMasternodeID = pcustomcsview->GetMasternodeIdByOperator(args.operatorID);
            if (!optMasternodeID)
            {
                return Status::initWaiting;
            }
            masternodeID = *optMasternodeID;
            auto nodePtr = pcustomcsview->GetMasternode(masternodeID);
            if (!nodePtr || !nodePtr->IsActive(tip->height)) /// @todo miner: height+1 or nHeight+1 ???
            {
                /// @todo may be new status for not activated (or already resigned) MN??
                return Status::initWaiting;
            }
            mintedBlocks = nodePtr->mintedBlocks;
            if (args.coinbaseScript.empty()) {
                // this is safe cause MN was found
                defaultScript = GetScriptForDestination(nodePtr->ownerType == 1 ? CTxDestination(PKHash(nodePtr->ownerAuthAddress)) : CTxDestination(WitnessV0KeyHash(nodePtr->ownerAuthAddress)));
            }
        }

        withSearchInterval([&](int64_t coinstakeTime, int64_t nSearchInterval) {
            if (fCriminals) {
                std::map <uint256, CBlockHeader> blockHeaders{};
                {
                    LOCK(cs_main);
                    pcriminals->FetchMintedHeaders(masternodeID, mintedBlocks + 1, blockHeaders, fIsFakeNet);
                }
                for (std::pair <uint256, CBlockHeader> const & blockHeader : blockHeaders) {
                    if (IsDoubleSignRestricted(blockHeader.second.height, tip->nHeight + (uint64_t)1)) {
                        potentialCriminalBlock = true;
                        return;
                    }
                }
            }
            //
            // Create block template
            //
            std::unique_ptr<CBlockTemplate> pblocktemplate(BlockAssembler(chainparams).CreateNewBlock(args.coinbaseScript.empty() ? defaultScript : args.coinbaseScript));
            if (!pblocktemplate.get()) {
                throw std::runtime_error("Error in WalletStaker: Keypool ran out, please call keypoolrefill before restarting the staking thread");
            }
            std::shared_ptr<CBlock> pblock = std::make_shared<CBlock>(pblocktemplate->block);

            LogPrint(BCLog::STAKING, "Running Staker with %u common transactions in block (%u bytes)\n", pblock->vtx.size() - 1,
                     ::GetSerializeSize(*pblock, PROTOCOL_VERSION));

            // find matching Hash
            pblock->height = tip->nHeight + 1;
            pblock->mintedBlocks = mintedBlocks + 1;
            pblock->stakeModifier = pos::ComputeStakeModifier(tip->stakeModifier, args.minterKey.GetPubKey().GetID());

            bool found = false;
            for (uint32_t t = 0; t < nSearchInterval; t++) {
                boost::this_thread::interruption_point();

                pblock->nTime = ((uint32_t)coinstakeTime - t);

                if (pos::CheckKernelHash(pblock->stakeModifier, pblock->nBits,  (int64_t) pblock->nTime, chainparams.GetConsensus(), masternodeID).hashOk) {
                    LogPrint(BCLog::STAKING, "MakeStake: kernel found\n");

                    found = true;
                    break;
                }
            }

            if (!found) {
                return;
            }

            //
            // Trying to sign a block
            //
            auto err = pos::SignPosBlock(pblock, args.minterKey);
            if (err) {
                LogPrint(BCLog::STAKING, "SignPosBlock(): %s \n", *err);
                return;
            }

            //
            // Final checks
            //
            {
                LOCK(cs_main);
                err = pos::CheckSignedBlock(pblock, tip, chainparams);
                if (err) {
                    LogPrint(BCLog::STAKING, "CheckSignedBlock(): %s \n", *err);
                    return;
                }
            }

            if (!ProcessNewBlock(chainparams, pblock, true, nullptr)) {
                LogPrintf("PoS block was checked, but wasn't accepted by ProcessNewBlock\n");
                return;
            }

            minted = true;
        });

        return minted ? Status::minted : (potentialCriminalBlock? Status::criminalWaiting : Status::stakeWaiting);
    }

    CBlockIndex* Staker::getTip() {
        LOCK(cs_main);
        return ::ChainActive().Tip();
    }

    template <typename F>
    bool Staker::withSearchInterval(F&& f) {
        const int64_t nTime = GetAdjustedTime(); // TODO: SS GetAdjustedTime() + period minting block

        if (nTime > nLastCoinStakeSearchTime) {
            f(nTime, nTime - nLastCoinStakeSearchTime);
            nLastCoinStakeSearchTime = nTime;
            return true;
        }
        return false;
    }

int32_t ThreadStaker::operator()(ThreadStaker::Args args, CChainParams chainparams) {
    pos::Staker staker{};
    int32_t nMinted = 0;
    int32_t nTried = 0;

    auto trying = [&]() {
        return args.nMaxTries == -1 || nTried < args.nMaxTries;
    };

    auto notDone = [&]() {
        return args.nMint == -1 || nMinted < args.nMint;
    };

    while (true) {
        boost::this_thread::interruption_point();

        while (fImporting || fReindex) {
            boost::this_thread::interruption_point();

            LogPrintf("ThreadStaker: reindex waiting\n");

            std::this_thread::sleep_for(std::chrono::milliseconds(900));
        }

        try {
            Staker::Status status = staker.stake(chainparams, args);
            if (status == Staker::Status::error) {
                LogPrintf("ThreadStaker terminated due to a staking error\n");
                return nMinted;
            }
            if (status == Staker::Status::minted) {
                LogPrintf("ThreadStaker minted a block!\n");
                nMinted++;
            }
            if (status == Staker::Status::initWaiting) {
                LogPrintf("ThreadStaker: initial waiting\n");
            }
            if (status == Staker::Status::stakeWaiting) {
                LogPrint(BCLog::STAKING, "Staked, but no kernel found yet\n");
            }
            if (status == Staker::Status::criminalWaiting) {
                LogPrint(BCLog::STAKING, "Potential criminal block tried to create\n");
            }
        }
        catch (const std::runtime_error &e) {
            LogPrintf("ThreadStaker runtime error: %s\n", e.what());
            return nMinted;
        }

        nTried++;
        if (trying() && notDone()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(900));
        } else {
            break;
        }
    }

    return nMinted;
}

}
