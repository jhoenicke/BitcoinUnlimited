// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2015 The Bitcoin Core developers
// Copyright (c) 2015-2018 The Bitcoin Unlimited developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "main.h"

#include "addrman.h"
#include "arith_uint256.h"
#include "blockrelay/graphene.h"
#include "blockrelay/thinblock.h"
#include "blockstorage/blockstorage.h"
#include "blockstorage/sequential_files.h"
#include "chainparams.h"
#include "checkpoints.h"
#include "checkqueue.h"
#include "connmgr.h"
#include "consensus/consensus.h"
#include "consensus/merkle.h"
#include "consensus/tx_verify.h"
#include "consensus/validation.h"
#include "dosman.h"
#include "expedited.h"
#include "hash.h"
#include "init.h"
#include "merkleblock.h"
#include "net.h"
#include "nodestate.h"
#include "parallel.h"
#include "policy/policy.h"
#include "pow.h"
#include "primitives/block.h"
#include "primitives/transaction.h"
#include "requestManager.h"
#include "respend/respenddetector.h"
#include "script/script.h"
#include "script/sigcache.h"
#include "script/standard.h"
#include "tinyformat.h"
#include "txadmission.h"
#include "txdb.h"
#include "txmempool.h"
#include "txorphanpool.h"
#include "uahf_fork.h"
#include "ui_interface.h"
#include "undo.h"
#include "util.h"
#include "utilmoneystr.h"
#include "utilstrencodings.h"
#include "validation/validation.h"
#include "validationinterface.h"
#include "versionbits.h"

#include <algorithm>
#include <boost/algorithm/hex.hpp>
#include <boost/algorithm/string/replace.hpp>
#include <boost/filesystem.hpp>
#include <boost/filesystem/fstream.hpp>
#include <boost/math/distributions/poisson.hpp>
#include <boost/scope_exit.hpp>
#include <boost/thread.hpp>
#include <sstream>

#if defined(NDEBUG)
#error "Bitcoin cannot be compiled without assertions."
#endif

/**
 * Global state
 */

extern std::atomic<int64_t> nTimeBestReceived;

// BU moved CWaitableCriticalSection csBestBlock;
// BU moved CConditionVariable cvBlockChange;
bool fImporting = false;
bool fReindex = false;
bool fTxIndex = false;
bool fHavePruned = false;
bool fPruneMode = false;
bool fIsBareMultisigStd = DEFAULT_PERMIT_BAREMULTISIG;
unsigned int nBytesPerSigOp = DEFAULT_BYTES_PER_SIGOP;
bool fCheckBlockIndex = false;
bool fCheckpointsEnabled = DEFAULT_CHECKPOINTS_ENABLED;
uint64_t nPruneTarget = 0;
uint64_t nDBUsedSpace = 0;
uint32_t nXthinBloomFilterSize = SMALLEST_MAX_BLOOM_FILTER_SIZE;

// BU: Move global objects to a single file
extern CTxMemPool mempool;

extern CTweak<unsigned int> maxBlocksInTransitPerPeer;
extern CTweak<unsigned int> blockDownloadWindow;
extern CTweak<uint64_t> reindexTypicalBlockSize;

extern std::map<CNetAddr, ConnectionHistory> mapInboundConnectionTracker;
extern CCriticalSection cs_mapInboundConnectionTracker;

extern CCriticalSection cs_LastBlockFile;

extern CBlockIndex *pindexBestInvalid;
extern std::map<uint256, NodeId> mapBlockSource;
extern std::set<int> setDirtyFileInfo;
extern std::map<uint256, std::pair<CBlockHeader, int64_t> > mapUnConnectedHeaders;
extern uint64_t nBlockSequenceId;


/** Number of nodes with fSyncStarted. */
int nSyncStarted = 0;

/** Number of preferable block download peers. */
std::atomic<int> nPreferredDownload{0};

/** All pairs A->B, where A (or one of its ancestors) misses transactions, but B has transactions.
 * Pruned nodes may have entries where B is missing data.
 */
std::multimap<CBlockIndex *, CBlockIndex *> mapBlocksUnlinked;

/** Global flag to indicate we should check to see if there are
 *  block/undo files that should be deleted.  Set on startup
 *  or if we allocate more file space when we're in prune mode
 */
bool fCheckForPruning = false;

std::vector<CBlockFileInfo> vinfoBlockFile;
int nLastBlockFile = 0;

//////////////////////////////////////////////////////////////////////////////
//
// Registration of network node signals.
//

namespace
{
int GetHeight()
{
    LOCK(cs_main);
    return chainActive.Height();
}

void UpdatePreferredDownload(CNode *node, CNodeState *state)
{
    nPreferredDownload.fetch_sub(state->fPreferredDownload);

    // Whether this node should be marked as a preferred download node.
    state->fPreferredDownload = !node->fOneShot && !node->fClient;
    // BU allow downloads from inbound nodes; this may have been limited to stop attackers from connecting
    // and offering a bad chain.  However, we are connecting to multiple nodes and so can choose the most work
    // chain on that basis.
    // state->fPreferredDownload = (!node->fInbound || node->fWhitelisted) && !node->fOneShot && !node->fClient;
    // LOG(NET, "node %s preferred DL: %d because (%d || %d) && %d && %d\n", node->GetLogName(),
    //   state->fPreferredDownload, !node->fInbound, node->fWhitelisted, !node->fOneShot, !node->fClient);

    nPreferredDownload.fetch_add(state->fPreferredDownload);
}

void InitializeNode(const CNode *pnode)
{
    // Add an entry to the nodestate map
    nodestate.InitializeNodeState(pnode);

    // Add an entry to requestmanager nodestate map
    requester.InitializeNodeState(pnode->GetId());
}

void FinalizeNode(NodeId nodeid)
{
    LOCK(cs_main);
    CNodeState *state = nodestate.State(nodeid);
    DbgAssert(state != nullptr, return );

    if (state->fSyncStarted)
        nSyncStarted--;

    std::vector<uint256> vBlocksInFlight;
    requester.GetBlocksInFlight(vBlocksInFlight, nodeid);
    for (const uint256 &hash : vBlocksInFlight)
    {
        // Erase mapblocksinflight entries for this node.
        requester.MapBlocksInFlightErase(hash, nodeid);

        // Reset all requests times to zero so that we can immediately re-request these blocks
        requester.ResetLastBlockRequestTime(hash);
    }
    nPreferredDownload.fetch_sub(state->fPreferredDownload);

    nodestate.RemoveNodeState(nodeid);
    requester.RemoveNodeState(nodeid);
    if (nodestate.Empty())
    {
        // Do a consistency check after the last peer is removed.  Force consistent state if production code
        DbgAssert(requester.MapBlocksInFlightEmpty(), requester.MapBlocksInFlightClear());
        DbgAssert(nPreferredDownload.load() == 0, nPreferredDownload.store(0));
    }
}

// Requires cs_main
bool PeerHasHeader(CNodeState *state, CBlockIndex *pindex)
{
    if (pindex == nullptr)
        return false;
    if (state->pindexBestKnownBlock && pindex == state->pindexBestKnownBlock->GetAncestor(pindex->nHeight))
        return true;
    if (state->pindexBestHeaderSent && pindex == state->pindexBestHeaderSent->GetAncestor(pindex->nHeight))
        return true;
    return false;
}
} // anon namespace


// Requires cs_main
bool CanDirectFetch(const Consensus::Params &consensusParams)
{
    return chainActive.Tip()->GetBlockTime() > GetAdjustedTime() - consensusParams.nPowTargetSpacing * 20;
}

bool GetNodeStateStats(NodeId nodeid, CNodeStateStats &stats)
{
    CNodeRef node(connmgr->FindNodeFromId(nodeid));
    if (!node)
        return false;

    LOCK(cs_main);
    CNodeState *state = nodestate.State(nodeid);
    DbgAssert(state != nullptr, return false);

    stats.nMisbehavior = node->nMisbehavior;
    stats.nSyncHeight = state->pindexBestKnownBlock ? state->pindexBestKnownBlock->nHeight : -1;
    stats.nCommonHeight = state->pindexLastCommonBlock ? state->pindexLastCommonBlock->nHeight : -1;

    std::vector<uint256> vBlocksInFlight;
    requester.GetBlocksInFlight(vBlocksInFlight, nodeid);
    for (const uint256 &hash : vBlocksInFlight)
    {
        // lookup block by hash to find height
        BlockMap::iterator mi = mapBlockIndex.find(hash);
        if (mi != mapBlockIndex.end())
        {
            CBlockIndex *pindex = (*mi).second;
            if (pindex)
                stats.vHeightInFlight.push_back(pindex->nHeight);
        }
    }
    return true;
}

void RegisterNodeSignals(CNodeSignals &nodeSignals)
{
    nodeSignals.GetHeight.connect(&GetHeight);
    nodeSignals.ProcessMessages.connect(&ProcessMessages);
    nodeSignals.SendMessages.connect(&SendMessages);
    nodeSignals.InitializeNode.connect(&InitializeNode);
    nodeSignals.FinalizeNode.connect(&FinalizeNode);
}

void UnregisterNodeSignals(CNodeSignals &nodeSignals)
{
    nodeSignals.GetHeight.disconnect(&GetHeight);
    nodeSignals.ProcessMessages.disconnect(&ProcessMessages);
    nodeSignals.SendMessages.disconnect(&SendMessages);
    nodeSignals.InitializeNode.disconnect(&InitializeNode);
    nodeSignals.FinalizeNode.disconnect(&FinalizeNode);
}

CBlockIndex *FindForkInGlobalIndex(const CChain &chain, const CBlockLocator &locator)
{
    // Find the first block the caller has in the main chain
    for (const uint256 &hash : locator.vHave)
    {
        BlockMap::iterator mi = mapBlockIndex.find(hash);
        if (mi != mapBlockIndex.end())
        {
            CBlockIndex *pindex = (*mi).second;
            if (chain.Contains(pindex))
                return pindex;
        }
    }
    return chain.Genesis();
}

CCoinsViewCache *pcoinsTip = nullptr;
CBlockTreeDB *pblocktree = nullptr;
CBlockTreeDB *pblocktreeother = nullptr;

bool TestLockPointValidity(const LockPoints *lp)
{
    AssertLockHeld(cs_main);
    assert(lp);
    // If there are relative lock times then the maxInputBlock will be set
    // If there are no relative lock times, the LockPoints don't depend on the chain
    if (lp->maxInputBlock)
    {
        // Check whether chainActive is an extension of the block at which the LockPoints
        // calculation was valid.  If not LockPoints are no longer valid
        if (!chainActive.Contains(lp->maxInputBlock))
        {
            return false;
        }
    }

    // LockPoints still valid
    return true;
}

/** Convert CValidationState to a human-readable message for logging */
std::string FormatStateMessage(const CValidationState &state)
{
    return strprintf("%s%s (code %i)", state.GetRejectReason(),
        state.GetDebugMessage().empty() ? "" : ", " + state.GetDebugMessage(), state.GetRejectCode());
}


bool AreFreeTxnsDisallowed()
{
    if (GetArg("-limitfreerelay", DEFAULT_LIMITFREERELAY) > 0)
        return false;

    return true;
}

bool GetTransaction(const uint256 &hash,
    CTransactionRef &txOut,
    const Consensus::Params &consensusParams,
    uint256 &hashBlock,
    bool fAllowSlow)
{
    CBlockIndex *pindexSlow = nullptr;

    LOCK(cs_main);

    CTransactionRef ptx = mempool.get(hash);
    if (ptx)
    {
        txOut = ptx;
        return true;
    }

    if (fTxIndex)
    {
        CDiskTxPos postx;
        if (pblocktree->ReadTxIndex(hash, postx))
        {
            CAutoFile file(OpenBlockFile(postx, true), SER_DISK, CLIENT_VERSION);
            if (file.IsNull())
                return error("%s: OpenBlockFile failed", __func__);
            CBlockHeader header;
            try
            {
                file >> header;
                fseek(file.Get(), postx.nTxOffset, SEEK_CUR);
                file >> txOut;
            }
            catch (const std::exception &e)
            {
                return error("%s: Deserialize or I/O error - %s", __func__, e.what());
            }
            hashBlock = header.GetHash();
            if (txOut->GetHash() != hash)
                return error("%s: txid mismatch", __func__);
            return true;
        }
    }

    // use coin database to locate block that contains transaction, and scan it
    if (fAllowSlow)
    {
        CoinAccessor coin(*pcoinsTip, hash);
        if (!coin->IsSpent())
            pindexSlow = chainActive[coin->nHeight];
    }

    if (pindexSlow)
    {
        CBlock block;
        if (ReadBlockFromDisk(block, pindexSlow, consensusParams))
        {
            for (const auto &tx : block.vtx)
            {
                if (tx->GetHash() == hash)
                {
                    txOut = tx;
                    hashBlock = pindexSlow->GetBlockHash();
                    return true;
                }
            }
        }
    }

    return false;
}


//////////////////////////////////////////////////////////////////////////////
//
// CBlock and CBlockIndex
//

bool fLargeWorkForkFound = false;
bool fLargeWorkInvalidChainFound = false;

// Execute a command, as given by -alertnotify, on certain events such as a long fork being seen
void AlertNotify(const std::string &strMessage)
{
    uiInterface.NotifyAlertChanged();
    std::string strCmd = GetArg("-alertnotify", "");
    if (strCmd.empty())
        return;

    // Alert text should be plain ascii coming from a trusted source, but to
    // be safe we first strip anything not in safeChars, then add single quotes around
    // the whole string before passing it to the shell:
    std::string singleQuote("'");
    std::string safeStatus = SanitizeString(strMessage);
    safeStatus = singleQuote + safeStatus + singleQuote;
    boost::replace_all(strCmd, "%s", safeStatus);

    boost::thread t(runCommand, strCmd); // thread runs free
}

/** Abort with a message */
bool AbortNode(const std::string &strMessage, const std::string &userMessage = "")
{
    strMiscWarning = strMessage;
    LOGA("*** %s\n", strMessage);
    uiInterface.ThreadSafeMessageBox(
        userMessage.empty() ? _("Error: A fatal internal error occurred, see debug.log for details") : userMessage, "",
        CClientUIInterface::MSG_ERROR);
    StartShutdown();
    return false;
}

bool AbortNode(CValidationState &state, const std::string &strMessage, const std::string &userMessage = "")
{
    AbortNode(strMessage, userMessage);
    return state.Error(strMessage);
}


//
// Called periodically asynchronously; alerts if it smells like
// we're being fed a bad chain (blocks being generated much
// too slowly or too quickly).
//
void PartitionCheck(bool (*initialDownloadCheck)(),
    CCriticalSection &cs,
    const CBlockIndex *const &bestHeader,
    int64_t nPowTargetSpacing)
{
    if (bestHeader == NULL || initialDownloadCheck())
        return;

    static int64_t lastAlertTime = 0;
    int64_t now = GetAdjustedTime();
    if (lastAlertTime > now - 60 * 60 * 24)
        return; // Alert at most once per day

    const int SPAN_HOURS = 4;
    const int SPAN_SECONDS = SPAN_HOURS * 60 * 60;
    int BLOCKS_EXPECTED = SPAN_SECONDS / nPowTargetSpacing;

    boost::math::poisson_distribution<double> poisson(BLOCKS_EXPECTED);

    std::string strWarning;
    int64_t startTime = GetAdjustedTime() - SPAN_SECONDS;

    LOCK(cs);
    const CBlockIndex *i = bestHeader;
    int nBlocks = 0;
    while (i->GetBlockTime() >= startTime)
    {
        ++nBlocks;
        i = i->pprev;
        if (i == NULL)
            return; // Ran out of chain, we must not be fully sync'ed
    }

    // How likely is it to find that many by chance?
    double p = boost::math::pdf(poisson, nBlocks);

    LOG(PARTITIONCHECK, "%s: Found %d blocks in the last %d hours\n", __func__, nBlocks, SPAN_HOURS);
    LOG(PARTITIONCHECK, "%s: likelihood: %g\n", __func__, p);

    // Aim for one false-positive about every fifty years of normal running:
    const int FIFTY_YEARS = 50 * 365 * 24 * 60 * 60;
    double alertThreshold = 1.0 / (FIFTY_YEARS / SPAN_SECONDS);

    if (p <= alertThreshold && nBlocks < BLOCKS_EXPECTED)
    {
        // Many fewer blocks than expected: alert!
        strWarning = strprintf(
            _("WARNING: check your network connection, %d blocks received in the last %d hours (%d expected)"), nBlocks,
            SPAN_HOURS, BLOCKS_EXPECTED);
    }
    else if (p <= alertThreshold && nBlocks > BLOCKS_EXPECTED)
    {
        // Many more blocks than expected: alert!
        strWarning = strprintf(_("WARNING: abnormally high number of blocks generated, %d blocks received in the last "
                                 "%d hours (%d expected)"),
            nBlocks, SPAN_HOURS, BLOCKS_EXPECTED);
    }
    if (!strWarning.empty())
    {
        strMiscWarning = strWarning;
        AlertNotify(strWarning);
        lastAlertTime = now;
    }
}

// Protected by cs_main
VersionBitsCache versionbitscache;

bool CheckAgainstCheckpoint(unsigned int height, const uint256 &hash, const CChainParams &chainparams)
{
    const CCheckpointData &ckpt = chainparams.Checkpoints();
    const auto &lkup = ckpt.mapCheckpoints.find(height);
    if (lkup != ckpt.mapCheckpoints.end()) // this block height is checkpointed
    {
        if (hash != lkup->second) // This block does not match the checkpoint
            return false;
    }
    return true;
}

bool CheckDiskSpace(uint64_t nAdditionalBytes)
{
    uint64_t nFreeBytesAvailable = fs::space(GetDataDir()).available;

    // Check for nMinDiskSpace bytes (currently 50MB)
    if (nFreeBytesAvailable < nMinDiskSpace + nAdditionalBytes)
        return AbortNode("Disk space is low!", _("Error: Disk space is low!"));

    return true;
}

bool LoadExternalBlockFile(const CChainParams &chainparams, FILE *fileIn, CDiskBlockPos *dbp)
{
    // Map of disk positions for blocks with unknown parent (only used for reindex)
    static std::multimap<uint256, CDiskBlockPos> mapBlocksUnknownParent;
    int64_t nStart = GetTimeMillis();

    int nLoaded = 0;
    try
    {
        // This takes over fileIn and calls fclose() on it in the CBufferedFile destructor
        CBufferedFile blkdat(fileIn, 2 * (reindexTypicalBlockSize.Value() + MESSAGE_START_SIZE + sizeof(unsigned int)),
            reindexTypicalBlockSize.Value() + MESSAGE_START_SIZE + sizeof(unsigned int), SER_DISK, CLIENT_VERSION);
        uint64_t nRewind = blkdat.GetPos();
        while (!blkdat.eof())
        {
            boost::this_thread::interruption_point();

            blkdat.SetPos(nRewind);
            nRewind++; // start one byte further next time, in case of failure
            blkdat.SetLimit(); // remove former limit
            unsigned int nSize = 0;
            try
            {
                // even if chainparams.MessageStart() is commonly used as network magic id
                // in this case is also used to separate blocks stored on disk on a block file.
                // locate a header
                unsigned char buf[MESSAGE_START_SIZE];
                blkdat.FindByte(chainparams.MessageStart()[0]);
                // FindByte peeks 1 ahead and locates the file pointer AT the byte, not at the next one as is typical
                // for file ops.  So if we rewind, we want to go one further.
                nRewind = blkdat.GetPos() + 1;
                blkdat >> FLATDATA(buf);
                if (memcmp(buf, chainparams.MessageStart(), MESSAGE_START_SIZE))
                    continue;
                // read size
                // BU NOTE: if we ever get to 4GB blocks the block size data structure will overflow since this is
                // defined as unsigned int (32 bits)
                blkdat >> nSize;
                if (nSize < 80) // BU allow variable block size || nSize > BU_MAX_BLOCK_SIZE)
                {
                    LOG(REINDEX, "Reindex error: Short block: %d\n", nSize);
                    continue;
                }
                if (nSize > 256 * 1024 * 1024)
                {
                    LOG(REINDEX, "Reindex warning: Gigantic block: %d\n", nSize);
                }
                blkdat.GrowTo(2 * (nSize + MESSAGE_START_SIZE + sizeof(unsigned int)));
            }
            catch (const std::exception &)
            {
                // no valid block header found; don't complain
                break;
            }
            try
            {
                // read block
                uint64_t nBlockPos = blkdat.GetPos();
                if (dbp)
                    dbp->nPos = nBlockPos;
                blkdat.SetLimit(nBlockPos + nSize);
                blkdat.SetPos(nBlockPos); // Unnecessary, I just got the position
                CBlock block;
                blkdat >> block;
                nRewind = blkdat.GetPos();

                // detect out of order blocks, and store them for later
                uint256 hash = block.GetHash();
                if (hash != chainparams.GetConsensus().hashGenesisBlock &&
                    mapBlockIndex.find(block.hashPrevBlock) == mapBlockIndex.end())
                {
                    LOG(REINDEX, "%s: Out of order block %s (created %s), parent %s not known\n", __func__,
                        hash.ToString(), DateTimeStrFormat("%Y-%m-%d", block.nTime), block.hashPrevBlock.ToString());
                    if (dbp)
                        mapBlocksUnknownParent.insert(std::make_pair(block.hashPrevBlock, *dbp));
                    continue;
                }

                // process in case the block isn't known yet
                if (mapBlockIndex.count(hash) == 0 || (mapBlockIndex[hash]->nStatus & BLOCK_HAVE_DATA) == 0)
                {
                    CValidationState state;
                    if (ProcessNewBlock(state, chainparams, NULL, &block, true, dbp, false))
                        nLoaded++;
                    if (state.IsError())
                        break;
                }
                else if (hash != chainparams.GetConsensus().hashGenesisBlock &&
                         mapBlockIndex[hash]->nHeight % 1000 == 0)
                {
                    LOG(REINDEX, "Block Import: already had block %s at height %d\n", hash.ToString(),
                        mapBlockIndex[hash]->nHeight);
                }

                // Recursively process earlier encountered successors of this block
                std::deque<uint256> queue;
                queue.push_back(hash);
                while (!queue.empty())
                {
                    uint256 head = queue.front();
                    queue.pop_front();
                    std::pair<std::multimap<uint256, CDiskBlockPos>::iterator,
                        std::multimap<uint256, CDiskBlockPos>::iterator>
                        range = mapBlocksUnknownParent.equal_range(head);
                    while (range.first != range.second)
                    {
                        std::multimap<uint256, CDiskBlockPos>::iterator it = range.first;
                        if (ReadBlockFromDiskSequential(block, it->second, chainparams.GetConsensus()))
                        {
                            LOGA("%s: Processing out of order child %s of %s\n", __func__, block.GetHash().ToString(),
                                head.ToString());
                            CValidationState dummy;
                            if (ProcessNewBlock(dummy, chainparams, NULL, &block, true, &it->second, false))
                            {
                                nLoaded++;
                                queue.push_back(block.GetHash());
                            }
                        }
                        range.first++;
                        mapBlocksUnknownParent.erase(it);
                    }
                }
            }
            catch (const std::exception &e)
            {
                LOGA("%s: Deserialize or I/O error - %s\n", __func__, e.what());
            }
        }
    }
    catch (const std::runtime_error &e)
    {
        AbortNode(std::string("System error: ") + e.what());
    }
    if (nLoaded > 0)
        LOGA("Loaded %i blocks from external file in %dms\n", nLoaded, GetTimeMillis() - nStart);
    return nLoaded > 0;
}

std::string GetWarnings(const std::string &strFor)
{
    std::string strStatusBar;
    std::string strRPC;
    std::string strGUI;

    if (!CLIENT_VERSION_IS_RELEASE)
    {
        strStatusBar =
            "This is a pre-release test build - use at your own risk - do not use for mining or merchant applications";
        strGUI = _(
            "This is a pre-release test build - use at your own risk - do not use for mining or merchant applications");
    }

    if (GetBoolArg("-testsafemode", DEFAULT_TESTSAFEMODE))
        strStatusBar = strRPC = strGUI = "testsafemode enabled";

    // Misc warnings like out of disk space and clock is wrong
    if (strMiscWarning != "")
    {
        strStatusBar = strGUI = strMiscWarning;
    }

    if (fLargeWorkForkFound)
    {
        strStatusBar = strRPC =
            "Warning: The network does not appear to fully agree! Some miners appear to be experiencing issues.";
        strGUI =
            _("Warning: The network does not appear to fully agree! Some miners appear to be experiencing issues.");
    }
    else if (fLargeWorkInvalidChainFound)
    {
        strStatusBar = strRPC = "Warning: We do not appear to fully agree with our peers! You may need to upgrade, or "
                                "other nodes may need to upgrade.";
        strGUI = _("Warning: We do not appear to fully agree with our peers! You may need to upgrade, or other nodes "
                   "may need to upgrade.");
    }

    if (strFor == "gui")
        return strGUI;
    else if (strFor == "statusbar")
        return strStatusBar;
    else if (strFor == "rpc")
        return strRPC;
    assert(!"GetWarnings(): invalid parameter");
    return "error";
}


//////////////////////////////////////////////////////////////////////////////
//
// Messages
//

bool AlreadyHaveBlock(const CInv &inv) EXCLUSIVE_LOCKS_REQUIRED(cs_main)
{
    // The Request Manager functionality requires that we return true only when we actually have received
    // the block and not when we have received the header only.  Otherwise the request manager may not
    // be able to update its block source in order to make re-requests.
    BlockMap::iterator mi = mapBlockIndex.find(inv.hash);
    if (mi == mapBlockIndex.end())
        return false;
    if (!(mi->second->nStatus & BLOCK_HAVE_DATA))
        return false;
    return true;
}

bool static ProcessGetData(CNode *pfrom, const Consensus::Params &consensusParams, std::deque<CInv> &vInv)
{
    bool gotWorkDone = false;

    std::vector<CInv> vNotFound;

    while (!vInv.empty())
    {
        // Don't bother if send buffer is too full to respond anyway
        if (pfrom->nSendSize >= SendBufferSize())
            break;

        const CInv inv = vInv.front();
        vInv.pop_front();
        {
            boost::this_thread::interruption_point();
            gotWorkDone = true;
            if (inv.type == MSG_BLOCK || inv.type == MSG_FILTERED_BLOCK || inv.type == MSG_THINBLOCK)
            {
                LOCK(cs_main);
                bool fSend = false;
                BlockMap::iterator mi = mapBlockIndex.find(inv.hash);
                if (mi != mapBlockIndex.end())
                {
                    if (chainActive.Contains(mi->second))
                    {
                        fSend = true;
                    }
                    else
                    {
                        static const int nOneMonth = 30 * 24 * 60 * 60;
                        // To prevent fingerprinting attacks, only send blocks outside of the active
                        // chain if they are valid, and no more than a month older (both in time, and in
                        // best equivalent proof of work) than the best header chain we know about.
                        fSend = mi->second->IsValid(BLOCK_VALID_SCRIPTS) && (pindexBestHeader != NULL) &&
                                (pindexBestHeader.load()->GetBlockTime() - mi->second->GetBlockTime() < nOneMonth) &&
                                (GetBlockProofEquivalentTime(
                                     *pindexBestHeader, *mi->second, *pindexBestHeader, consensusParams) < nOneMonth);
                        if (!fSend)
                        {
                            LOGA("%s: ignoring request from peer=%s for old block that isn't in the main chain\n",
                                __func__, pfrom->GetLogName());
                        }
                        else
                        { // BU: don't relay excessive blocks that are not on the active chain
                            if (mi->second->nStatus & BLOCK_EXCESSIVE)
                                fSend = false;
                            if (!fSend)
                                LOGA("%s: ignoring request from peer=%s for excessive block of height %d not on "
                                     "the main chain\n",
                                    __func__, pfrom->GetLogName(), mi->second->nHeight);
                        }
                        // BU: in the future we can throttle old block requests by setting send=false if we are out of
                        // bandwidth
                    }
                }
                // disconnect node in case we have reached the outbound limit for serving historical blocks
                // never disconnect whitelisted nodes
                static const int nOneWeek = 7 * 24 * 60 * 60; // assume > 1 week = historical
                if (fSend && CNode::OutboundTargetReached(true) &&
                    (((pindexBestHeader != nullptr) &&
                         (pindexBestHeader.load()->GetBlockTime() - mi->second->GetBlockTime() > nOneWeek)) ||
                        inv.type == MSG_FILTERED_BLOCK) &&
                    !pfrom->fWhitelisted)
                {
                    LOG(NET, "historical block serving limit reached, disconnect peer %s\n", pfrom->GetLogName());

                    // disconnect node
                    pfrom->fDisconnect = true;
                    fSend = false;
                }
                // Pruned nodes may have deleted the block, so check whether
                // it's available before trying to send.
                if (fSend && (mi->second->nStatus & BLOCK_HAVE_DATA))
                {
                    // Send block from disk
                    CBlock block;
                    if (!ReadBlockFromDisk(block, (*mi).second, consensusParams))
                    {
                        // its possible that I know about it but haven't stored it yet
                        LOG(THIN, "unable to load block %s from disk\n",
                            (*mi).second->phashBlock ? (*mi).second->phashBlock->ToString() : "");
                        // no response
                    }
                    else
                    {
                        if (inv.type == MSG_BLOCK)
                        {
                            pfrom->blocksSent += 1;
                            pfrom->PushMessage(NetMsgType::BLOCK, block);
                        }
                        else if (inv.type == MSG_THINBLOCK)
                        {
                            LOG(THIN, "Sending thinblock by INV queue getdata message\n");
                            SendXThinBlock(MakeBlockRef(block), pfrom, inv);
                        }
                        else // MSG_FILTERED_BLOCK)
                        {
                            LOCK(pfrom->cs_filter);
                            if (pfrom->pfilter)
                            {
                                CMerkleBlock merkleBlock(block, *pfrom->pfilter);
                                pfrom->PushMessage(NetMsgType::MERKLEBLOCK, merkleBlock);
                                pfrom->blocksSent += 1;
                                // CMerkleBlock just contains hashes, so also push any transactions in the block the
                                // client did not see
                                // This avoids hurting performance by pointlessly requiring a round-trip
                                // Note that there is currently no way for a node to request any single transactions we
                                // didn't send here -
                                // they must either disconnect and retry or request the full block.
                                // Thus, the protocol spec specified allows for us to provide duplicate txn here,
                                // however we MUST always provide at least what the remote peer needs
                                typedef std::pair<unsigned int, uint256> PairType;
                                for (PairType &pair : merkleBlock.vMatchedTxn)
                                {
                                    pfrom->txsSent += 1;
                                    pfrom->PushMessage(NetMsgType::TX, block.vtx[pair.first]);
                                }
                            }
                            // else
                            // no response
                        }

                        // Trigger the peer node to send a getblocks request for the next batch of inventory
                        if (inv.hash == pfrom->hashContinue)
                        {
                            // Bypass PushInventory, this must send even if redundant,
                            // and we want it right after the last block so they don't
                            // wait for other stuff first.
                            std::vector<CInv> oneInv;
                            oneInv.push_back(CInv(MSG_BLOCK, chainActive.Tip()->GetBlockHash()));
                            pfrom->PushMessage(NetMsgType::INV, oneInv);
                            pfrom->hashContinue.SetNull();
                        }
                    }
                }
            }
            else if (inv.IsKnownType())
            {
                // Send stream from relay memory
                bool fPushed = false;
                {
                    CTransactionRef ptx;

                    // We need to release this lock before push message. There is a potential deadlock because
                    // cs_vSend is often taken before cs_mapRelay
                    {
                        LOCK(cs_mapRelay);
                        std::map<CInv, CTransactionRef>::iterator mi = mapRelay.find(inv);
                        if (mi != mapRelay.end())
                        {
                            // Copy shared ptr to second because it may be deleted once lock is released
                            ptx = (*mi).second;
                            fPushed = true;
                        }
                    }

                    if (fPushed)
                    {
                        pfrom->PushMessage(inv.GetCommand(), ptx);
                        pfrom->txsSent += 1;
                    }
                }
                if (!fPushed && inv.type == MSG_TX)
                {
                    CTransactionRef ptx = nullptr;
                    ptx = CommitQGet(inv.hash);
                    if (!ptx)
                    {
                        ptx = mempool.get(inv.hash);
                    }
                    if (ptx)
                    {
                        pfrom->PushMessage(NetMsgType::TX, ptx);
                        fPushed = true;
                        pfrom->txsSent += 1;
                    }
                }
                if (!fPushed)
                {
                    vNotFound.push_back(inv);
                }
            }

            // Track requests for our stuff.
            GetMainSignals().Inventory(inv.hash);

            // We only want to process one of these message types before returning. These are high
            // priority messages and we don't want to sit here processing a large number of messages
            // while we hold the cs_main lock, but rather allow these messages to be sent first and
            // process the return message before potentially reading from the queue again.
            if (inv.type == MSG_BLOCK || inv.type == MSG_FILTERED_BLOCK || inv.type == MSG_THINBLOCK)
                break;
        }
    }

    if (!vNotFound.empty())
    {
        // Let the peer know that we didn't find what it asked for, so it doesn't
        // have to wait around forever. Currently only SPV clients actually care
        // about this message: it's needed when they are recursively walking the
        // dependencies of relevant unconfirmed transactions. SPV clients want to
        // do that because they want to know about (and store and rebroadcast and
        // risk analyze) the dependencies of transactions relevant to them, without
        // having to download the entire memory pool.
        pfrom->PushMessage(NetMsgType::NOTFOUND, vNotFound);
    }
    return gotWorkDone;
}


static bool BasicThinblockChecks(CNode *pfrom, const CChainParams &chainparams)
{
    if (!pfrom->ThinBlockCapable())
    {
        dosMan.Misbehaving(pfrom, 100);
        return error("Thinblock message received from a non thinblock node, peer=%d", pfrom->GetId());
    }

    // Check for Misbehaving and DOS
    // If they make more than 20 requests in 10 minutes then disconnect them
    if (Params().NetworkIDString() != "regtest")
    {
        if (pfrom->nGetXthinLastTime <= 0)
            pfrom->nGetXthinLastTime = GetTime();
        uint64_t nNow = GetTime();
        double tmp = pfrom->nGetXthinCount;
        while (!pfrom->nGetXthinCount.compare_exchange_weak(
            tmp, (tmp * std::pow(1.0 - 1.0 / 600.0, (double)(nNow - pfrom->nGetXthinLastTime)) + 1)))
            ;
        pfrom->nGetXthinLastTime = nNow;
        LOG(THIN, "nGetXthinCount is %f\n", pfrom->nGetXthinCount);
        if (chainparams.NetworkIDString() == "main") // other networks have variable mining rates
        {
            if (pfrom->nGetXthinCount >= 20)
            {
                dosMan.Misbehaving(pfrom, 50); // If they exceed the limit then disconnect them
                return error("requesting too many getdata thinblocks");
            }
        }
    }

    return true;
}

bool ProcessMessage(CNode *pfrom, std::string strCommand, CDataStream &vRecv, int64_t nTimeReceived)
{
    int64_t receiptTime = GetTime();
    const CChainParams &chainparams = Params();
    RandAddSeedPerfmon();
    unsigned int msgSize = vRecv.size(); // BU for statistics
    UpdateRecvStats(pfrom, strCommand, msgSize, nTimeReceived);
    LOG(NET, "received: %s (%u bytes) peer=%s\n", SanitizeString(strCommand), msgSize, pfrom->GetLogName());
    if (mapArgs.count("-dropmessagestest") && GetRand(atoi(mapArgs["-dropmessagestest"])) == 0)
    {
        LOGA("dropmessagestest DROPPING RECV MESSAGE\n");
        return true;
    }

    if (!(nLocalServices & NODE_BLOOM) &&
        (strCommand == NetMsgType::FILTERLOAD || strCommand == NetMsgType::FILTERADD ||
            strCommand == NetMsgType::FILTERCLEAR))
    {
        if (pfrom->nVersion >= NO_BLOOM_VERSION)
        {
            dosMan.Misbehaving(pfrom, 100);
            return false;
        }
        else
        {
            LOG(NET, "Inconsistent bloom filter settings peer %s\n", pfrom->GetLogName());
            pfrom->fDisconnect = true;
            return false;
        }
    }


    if (strCommand == NetMsgType::VERSION)
    {
        // Each connection can only send one version message
        if (pfrom->nVersion != 0)
        {
            pfrom->PushMessage(
                NetMsgType::REJECT, strCommand, REJECT_DUPLICATE, std::string("Duplicate version message"));
            pfrom->fDisconnect = true;
            return error("Duplicate version message received - disconnecting peer=%s version=%s", pfrom->GetLogName(),
                pfrom->cleanSubVer);
        }

        int64_t nTime;
        CAddress addrMe;
        CAddress addrFrom;
        uint64_t nNonce = 1;
        vRecv >> pfrom->nVersion >> pfrom->nServices >> nTime >> addrMe;

        if (pfrom->nVersion < MIN_PEER_PROTO_VERSION)
        {
            // ban peers older than this proto version
            pfrom->PushMessage(NetMsgType::REJECT, strCommand, REJECT_OBSOLETE,
                strprintf("Protocol Version must be %d or greater", MIN_PEER_PROTO_VERSION));
            dosMan.Misbehaving(pfrom, 100);
            return error("Using obsolete protocol version %i - banning peer=%s version=%s", pfrom->nVersion,
                pfrom->GetLogName(), pfrom->cleanSubVer);
        }

        if (pfrom->nVersion == 10300)
            pfrom->nVersion = 300;
        if (!vRecv.empty())
            vRecv >> addrFrom >> nNonce;
        if (!vRecv.empty())
        {
            vRecv >> LIMITED_STRING(pfrom->strSubVer, MAX_SUBVERSION_LENGTH);
            pfrom->cleanSubVer = SanitizeString(pfrom->strSubVer);
        }
        if (!vRecv.empty())
            vRecv >> pfrom->nStartingHeight;
        if (!vRecv.empty())
            vRecv >> pfrom->fRelayTxes; // set to true after we get the first filter* message
        else
            pfrom->fRelayTxes = true;

        // Disconnect if we connected to ourself
        if (nNonce == nLocalHostNonce && nNonce > 1)
        {
            LOGA("connected to self at %s, disconnecting\n", pfrom->addr.ToString());
            pfrom->fDisconnect = true;
            return true;
        }

        pfrom->addrLocal = addrMe;
        if (pfrom->fInbound && addrMe.IsRoutable())
        {
            SeenLocal(addrMe);
        }

        // Be shy and don't send version until we hear
        if (pfrom->fInbound)
            pfrom->PushVersion();

        pfrom->fClient = !(pfrom->nServices & NODE_NETWORK);

        // Potentially mark this peer as a preferred download peer.
        UpdatePreferredDownload(pfrom, nodestate.State(pfrom->GetId()));

        // Send VERACK handshake message
        pfrom->fVerackSent = true;
        pfrom->PushMessage(NetMsgType::VERACK);

        // Change version
        pfrom->ssSend.SetVersion(std::min(pfrom->nVersion, PROTOCOL_VERSION));

        if (!pfrom->fInbound)
        {
            // Advertise our address
            if (fListen && !IsInitialBlockDownload())
            {
                CAddress addr = GetLocalAddress(&pfrom->addr);
                FastRandomContext insecure_rand;
                if (addr.IsRoutable())
                {
                    LOG(NET, "ProcessMessages: advertising address %s\n", addr.ToString());
                    pfrom->PushAddress(addr, insecure_rand);
                }
                else if (IsPeerAddrLocalGood(pfrom))
                {
                    addr.SetIP(pfrom->addrLocal);
                    LOG(NET, "ProcessMessages: advertising address %s\n", addr.ToString());
                    pfrom->PushAddress(addr, insecure_rand);
                }
            }

            // Get recent addresses
            if (pfrom->fOneShot || pfrom->nVersion >= CADDR_TIME_VERSION || addrman.size() < 1000)
            {
                pfrom->PushMessage(NetMsgType::GETADDR);
                pfrom->fGetAddr = true;
            }
            addrman.Good(pfrom->addr);
        }
        else
        {
            if (((CNetAddr)pfrom->addr) == (CNetAddr)addrFrom)
            {
                addrman.Add(addrFrom, addrFrom);
                addrman.Good(addrFrom);
            }
        }

        LOG(NET, "receive version message: %s: version %d, blocks=%d, us=%s, peer=%s\n", pfrom->cleanSubVer,
            pfrom->nVersion, pfrom->nStartingHeight, addrMe.ToString(), pfrom->GetLogName());

        int64_t nTimeOffset = nTime - GetTime();
        pfrom->nTimeOffset = nTimeOffset;
        AddTimeData(pfrom->addr, nTimeOffset);

        // Feeler connections exist only to verify if address is online.
        if (pfrom->fFeeler)
        {
            // Should never occur but if it does correct the value.
            // We can't have an inbound "feeler" connection, so the value must be improperly set.
            DbgAssert(pfrom->fInbound == false, pfrom->fFeeler = false);
            if (pfrom->fInbound == false)
            {
                LOG(NET, "Disconnecting feeler to peer %s\n", pfrom->GetLogName());
                pfrom->fDisconnect = true;
            }
        }
    }

    /* Since we are processing messages in multiple threads, we may process them out of order.  Does enforcing this
       order actually matter?  Note we allow mis-order (or no version message at all) if whitelisted...
        else if (pfrom->nVersion == 0 && !pfrom->fWhitelisted)
        {
            // Must have version message before anything else (Although we may send our VERSION before
            // we receive theirs, it would not be possible to receive their VERACK before their VERSION).
            pfrom->fDisconnect = true;
            return error("%s receieved before VERSION message - disconnecting peer=%s", strCommand,
       pfrom->GetLogName());
        }
    */

    else if (strCommand == NetMsgType::VERACK)
    {
        // If we haven't sent a VERSION message yet then we should not get a VERACK message.
        if (pfrom->tVersionSent < 0)
        {
            pfrom->fDisconnect = true;
            return error("VERACK received but we never sent a VERSION message - disconnecting peer=%s version=%s",
                pfrom->GetLogName(), pfrom->cleanSubVer);
        }
        if (pfrom->fSuccessfullyConnected)
        {
            pfrom->fDisconnect = true;
            return error("duplicate VERACK received - disconnecting peer=%s version=%s", pfrom->GetLogName(),
                pfrom->cleanSubVer);
        }

        pfrom->fSuccessfullyConnected = true;
        pfrom->SetRecvVersion(std::min(pfrom->nVersion, PROTOCOL_VERSION));

        // Mark this node as currently connected, so we update its timestamp later.
        if (pfrom->fNetworkNode)
            pfrom->fCurrentlyConnected = true;

        if (pfrom->nVersion >= SENDHEADERS_VERSION)
        {
            // Tell our peer we prefer to receive headers rather than inv's
            // We send this to non-NODE NETWORK peers as well, because even
            // non-NODE NETWORK peers can announce blocks (such as pruning
            // nodes)

            pfrom->PushMessage(NetMsgType::SENDHEADERS);
        }

        // Tell the peer what maximum xthin bloom filter size we will consider acceptable.
        if (pfrom->ThinBlockCapable() && IsThinBlocksEnabled())
        {
            pfrom->PushMessage(NetMsgType::FILTERSIZEXTHIN, nXthinBloomFilterSize);
        }

        // BU expedited procecessing requires the exchange of the listening port id but we have to send it in a separate
        // version
        // message because we don't know if in the future Core will append more data to the end of the current VERSION
        // message.
        // The BUVERSION should be after the VERACK message otherwise Core may flag an error if another messaged shows
        // up before the VERACK is received.
        // The BUVERSION message is active from the protocol EXPEDITED_VERSION onwards.
        if (pfrom->nVersion >= EXPEDITED_VERSION)
        {
            pfrom->fBUVersionSent = true;
            pfrom->PushMessage(NetMsgType::BUVERSION, GetListenPort());
        }
    }


    else if (!pfrom->fSuccessfullyConnected && GetTime() - pfrom->tVersionSent > VERACK_TIMEOUT &&
             pfrom->tVersionSent >= 0)
    {
        // If verack is not received within timeout then disconnect.
        // The peer may be slow so disconnect them only, to give them another chance if they try to re-connect.
        // If they are a bad peer and keep trying to reconnect and still do not VERACK, they will eventually
        // get banned by the connection slot algorithm which tracks disconnects and reconnects.
        pfrom->fDisconnect = true;
        LOG(NET, "ERROR: disconnecting - VERACK not received within %d seconds for peer=%s version=%s\n",
            VERACK_TIMEOUT, pfrom->GetLogName(), pfrom->cleanSubVer);

        // update connection tracker which is used by the connection slot algorithm.
        LOCK(cs_mapInboundConnectionTracker);
        CNetAddr ipAddress = (CNetAddr)pfrom->addr;
        mapInboundConnectionTracker[ipAddress].nEvictions += 1;
        mapInboundConnectionTracker[ipAddress].nLastEvictionTime = GetTime();

        return true; // return true so we don't get any process message failures in the log.
    }


    else if (strCommand == NetMsgType::ADDR)
    {
        std::vector<CAddress> vAddr;
        vRecv >> vAddr;

        // Don't want addr from older versions unless seeding
        if (pfrom->nVersion < CADDR_TIME_VERSION && addrman.size() > 1000)
            return true;
        if (vAddr.size() > 1000)
        {
            dosMan.Misbehaving(pfrom, 20);
            return error("message addr size() = %u", vAddr.size());
        }

        // Store the new addresses
        std::vector<CAddress> vAddrOk;
        int64_t nNow = GetAdjustedTime();
        int64_t nSince = nNow - 10 * 60;
        FastRandomContext insecure_rand;
        for (CAddress &addr : vAddr)
        {
            boost::this_thread::interruption_point();

            if (addr.nTime <= 100000000 || addr.nTime > nNow + 10 * 60)
                addr.nTime = nNow - 5 * 24 * 60 * 60;
            pfrom->AddAddressKnown(addr);
            bool fReachable = IsReachable(addr);
            if (addr.nTime > nSince && !pfrom->fGetAddr && vAddr.size() <= 10 && addr.IsRoutable())
            {
                // Relay to a limited number of other nodes
                {
                    LOCK(cs_vNodes);
                    // Use deterministic randomness to send to the same nodes for 24 hours
                    // at a time so the addrKnowns of the chosen nodes prevent repeats
                    static uint256 hashSalt;
                    if (hashSalt.IsNull())
                        hashSalt = GetRandHash();
                    uint64_t hashAddr = addr.GetHash();
                    uint256 hashRand = ArithToUint256(
                        UintToArith256(hashSalt) ^ (hashAddr << 32) ^ ((GetTime() + hashAddr) / (24 * 60 * 60)));
                    hashRand = Hash(BEGIN(hashRand), END(hashRand));
                    std::multimap<uint256, CNode *> mapMix;
                    for (CNode *pnode : vNodes)
                    {
                        if (pnode->nVersion < CADDR_TIME_VERSION)
                            continue;
                        unsigned int nPointer;
                        memcpy(&nPointer, &pnode, sizeof(nPointer));
                        uint256 hashKey = ArithToUint256(UintToArith256(hashRand) ^ nPointer);
                        hashKey = Hash(BEGIN(hashKey), END(hashKey));
                        mapMix.insert(std::make_pair(hashKey, pnode));
                    }
                    int nRelayNodes = fReachable ? 2 : 1; // limited relaying of addresses outside our network(s)
                    for (std::multimap<uint256, CNode *>::iterator mi = mapMix.begin();
                         mi != mapMix.end() && nRelayNodes-- > 0; ++mi)
                        ((*mi).second)->PushAddress(addr, insecure_rand);
                }
            }
            // Do not store addresses outside our network
            if (fReachable)
                vAddrOk.push_back(addr);
        }
        addrman.Add(vAddrOk, pfrom->addr, 2 * 60 * 60);
        if (vAddr.size() < 1000)
            pfrom->fGetAddr = false;
        if (pfrom->fOneShot)
        {
            LOG(NET, "Disconnecting %s: one shot\n", pfrom->GetLogName());
            pfrom->fDisconnect = true;
        }
    }

    else if (strCommand == NetMsgType::SENDHEADERS)
    {
        LOCK(cs_main);
        nodestate.State(pfrom->GetId())->fPreferHeaders = true;
    }

    // Processing this message type for statistics purposes only, BU currently doesn't support CB protocol
    // Ignore this message if sent from a node advertising a version earlier than the first CB release (70014)
    else if (strCommand == NetMsgType::SENDCMPCT && pfrom->nVersion >= 70014)
    {
        bool fHighBandwidth = false;
        uint64_t nVersion = 0;
        vRecv >> fHighBandwidth >> nVersion;

        // BCH network currently only supports version 1 (v2 is segwit support on BTC)
        // May need to be updated in the future if other clients deploy a new version
        pfrom->fSupportsCompactBlocks = nVersion == 1;
    }

    else if (strCommand == NetMsgType::INV)
    {
        if (fImporting || fReindex)
            return true;

        std::vector<CInv> vInv;
        vRecv >> vInv;
        LOG(NET, "Received INV list of size %d\n", vInv.size());

        // Message Consistency Checking
        //   Check size == 0 to be intolerant of an empty and useless request.
        //   Validate that INVs are a valid type and not null.
        if (vInv.size() > MAX_INV_SZ || vInv.empty())
        {
            dosMan.Misbehaving(pfrom, 20);
            return error("message inv size() = %u", vInv.size());
        }

        bool fBlocksOnly = GetBoolArg("-blocksonly", DEFAULT_BLOCKSONLY);

        // Allow whitelisted peers to send data other than blocks in blocks only mode if whitelistrelay is true
        if (pfrom->fWhitelisted && GetBoolArg("-whitelistrelay", DEFAULT_WHITELISTRELAY))
            fBlocksOnly = false;

        for (unsigned int nInv = 0; nInv < vInv.size(); nInv++)
        {
            boost::this_thread::interruption_point();

            const CInv &inv = vInv[nInv];
            if (!((inv.type == MSG_TX) || (inv.type == MSG_BLOCK)) || inv.hash.IsNull())
            {
                dosMan.Misbehaving(pfrom, 20);
                return error("message inv invalid type = %u or is null hash %s", inv.type, inv.hash.ToString());
            }

            if (inv.type == MSG_BLOCK)
            {
                LOCK(cs_main);
                bool fAlreadyHaveBlock = AlreadyHaveBlock(inv);
                LOG(NET, "got inv: %s  %s peer=%d\n", inv.ToString(), fAlreadyHaveBlock ? "have" : "new", pfrom->id);

                requester.UpdateBlockAvailability(pfrom->GetId(), inv.hash);
                // RE !IsInitialBlockDownload(): We do not want to get the block if the system is executing the initial
                // block download because
                // blocks are stored in block files in the order of arrival.  So grabbing blocks "early" will cause new
                // blocks to be sprinkled
                // throughout older block files.  This will stop those files from being pruned.
                // !IsInitialBlockDownload() can be removed if
                // a better block storage system is devised.
                if ((!fAlreadyHaveBlock && !IsInitialBlockDownload()) ||
                    (!fAlreadyHaveBlock && Params().NetworkIDString() == "regtest"))
                {
                    // Since we now only rely on headers for block requests, if we get an INV from an older node or
                    // if there was a very large re-org which resulted in a revert to block announcements via INV,
                    // we will instead request the header rather than the block.  This is safer and prevents an
                    // attacker from sending us fake INV's for blocks that do not exist or try to get us to request
                    // and download fake blocks.
                    pfrom->PushMessage(NetMsgType::GETHEADERS, chainActive.GetLocator(pindexBestHeader), inv.hash);
                }
                else
                {
                    LOG(NET, "skipping request of block %s.  already have: %d  importing: %d  reindex: %d  "
                             "isChainNearlySyncd: %d\n",
                        inv.hash.ToString(), fAlreadyHaveBlock, fImporting, fReindex, IsChainNearlySyncd());
                }
            }
            else // If we get here then inv.type must == MSG_TX.
            {
                bool fAlreadyHaveTx = TxAlreadyHave(inv);
                // LOG(NET, "got inv: %s  %d peer=%s\n", inv.ToString(), fAlreadyHaveTx ? "have" : "new",
                // pfrom->GetLogName());
                LOG(NET, "got inv: %s  have: %d peer=%s\n", inv.ToString(), fAlreadyHaveTx, pfrom->GetLogName());

                pfrom->AddInventoryKnown(inv);
                if (fBlocksOnly)
                {
                    LOG(NET, "transaction (%s) inv sent in violation of protocol peer=%d\n", inv.hash.ToString(),
                        pfrom->id);
                }
                // RE !IsInitialBlockDownload(): during IBD, its a waste of bandwidth to grab transactions, they will
                // likely be included in blocks that we IBD download anyway.  This is especially important as
                // transaction volumes increase.
                else if (!fAlreadyHaveTx && !IsInitialBlockDownload())
                    requester.AskFor(inv, pfrom);
            }

            // Track requests for our stuff.
            GetMainSignals().Inventory(inv.hash);

            if (pfrom->nSendSize > (SendBufferSize() * 2))
            {
                dosMan.Misbehaving(pfrom, 50);
                return error("send buffer size() = %u", pfrom->nSendSize);
            }
        }
    }


    else if (strCommand == NetMsgType::GETDATA)
    {
        if (fImporting || fReindex)
        {
            LOG(NET, "received getdata from %s but importing\n", pfrom->GetLogName());
            return true;
        }

        std::vector<CInv> vInv;
        vRecv >> vInv;
        // BU check size == 0 to be intolerant of an empty and useless request
        if ((vInv.size() > MAX_INV_SZ) || (vInv.size() == 0))
        {
            dosMan.Misbehaving(pfrom, 20);
            return error("message getdata size() = %u", vInv.size());
        }

        // Validate that INVs are a valid type
        std::deque<CInv> invDeque;
        for (unsigned int nInv = 0; nInv < vInv.size(); nInv++)
        {
            const CInv &inv = vInv[nInv];
            if (!((inv.type == MSG_TX) || (inv.type == MSG_BLOCK) || (inv.type == MSG_FILTERED_BLOCK) ||
                    (inv.type == MSG_THINBLOCK)))
            {
                dosMan.Misbehaving(pfrom, 20);
                return error("message inv invalid type = %u", inv.type);
            }

            // Make basic checks
            if (inv.type == MSG_THINBLOCK)
            {
                if (!BasicThinblockChecks(pfrom, chainparams))
                    return false;
            }
            invDeque.push_back(inv);
        }

        if (fDebug || (invDeque.size() != 1))
            LOG(NET, "received getdata (%u invsz) peer=%s\n", invDeque.size(), pfrom->GetLogName());

        if ((fDebug && invDeque.size() > 0) || (invDeque.size() == 1))
            LOG(NET, "received getdata for: %s peer=%s\n", invDeque[0].ToString(), pfrom->GetLogName());

        ProcessGetData(pfrom, chainparams.GetConsensus(), invDeque);
        {
            LOCK(pfrom->csRecvGetData);
            pfrom->vRecvGetData.insert(pfrom->vRecvGetData.end(), invDeque.begin(), invDeque.end());
        }
    }


    else if (strCommand == NetMsgType::GETBLOCKS)
    {
        if (fImporting || fReindex)
            return true;

        CBlockLocator locator;
        uint256 hashStop;
        vRecv >> locator >> hashStop;

        LOCK(cs_main);

        // Find the last block the caller has in the main chain
        CBlockIndex *pindex = FindForkInGlobalIndex(chainActive, locator);

        // Send the rest of the chain
        if (pindex)
            pindex = chainActive.Next(pindex);
        int nLimit = 500;
        LOG(NET, "getblocks %d to %s limit %d from peer=%d\n", (pindex ? pindex->nHeight : -1),
            hashStop.IsNull() ? "end" : hashStop.ToString(), nLimit, pfrom->id);
        for (; pindex; pindex = chainActive.Next(pindex))
        {
            if (pindex->GetBlockHash() == hashStop)
            {
                LOG(NET, "  getblocks stopping at %d %s\n", pindex->nHeight, pindex->GetBlockHash().ToString());
                break;
            }
            // If pruning, don't inv blocks unless we have on disk and are likely to still have
            // for some reasonable time window (1 hour) that block relay might require.
            const int nPrunedBlocksLikelyToHave =
                MIN_BLOCKS_TO_KEEP - 3600 / chainparams.GetConsensus().nPowTargetSpacing;
            if (fPruneMode && (!(pindex->nStatus & BLOCK_HAVE_DATA) ||
                                  pindex->nHeight <= chainActive.Tip()->nHeight - nPrunedBlocksLikelyToHave))
            {
                LOG(NET, " getblocks stopping, pruned or too old block at %d %s\n", pindex->nHeight,
                    pindex->GetBlockHash().ToString());
                break;
            }
            pfrom->PushInventory(CInv(MSG_BLOCK, pindex->GetBlockHash()));
            if (--nLimit <= 0)
            {
                // When this block is requested, we'll send an inv that'll
                // trigger the peer to getblocks the next batch of inventory.
                LOG(NET, "  getblocks stopping at limit %d %s\n", pindex->nHeight, pindex->GetBlockHash().ToString());
                pfrom->hashContinue = pindex->GetBlockHash();
                break;
            }
        }
    }


    else if (strCommand == NetMsgType::GETHEADERS)
    {
        CBlockLocator locator;
        uint256 hashStop;
        vRecv >> locator >> hashStop;

        LOCK(cs_main);
        CNodeState *state = nodestate.State(pfrom->GetId());
        CBlockIndex *pindex = nullptr;
        if (locator.IsNull())
        {
            // If locator is null, return the hashStop block
            BlockMap::iterator mi = mapBlockIndex.find(hashStop);
            if (mi == mapBlockIndex.end())
                return true;
            pindex = (*mi).second;
        }
        else
        {
            // Find the last block the caller has in the main chain
            pindex = FindForkInGlobalIndex(chainActive, locator);
            if (pindex)
                pindex = chainActive.Next(pindex);
        }

        // we must use CBlocks, as CBlockHeaders won't include the 0x00 nTx count at the end
        std::vector<CBlock> vHeaders;
        int nLimit = MAX_HEADERS_RESULTS;
        LOG(NET, "getheaders height %d for block %s from peer %s\n", (pindex ? pindex->nHeight : -1),
            hashStop.ToString(), pfrom->GetLogName());
        for (; pindex; pindex = chainActive.Next(pindex))
        {
            vHeaders.push_back(pindex->GetBlockHeader());
            if (--nLimit <= 0 || pindex->GetBlockHash() == hashStop)
                break;
        }
        // pindex can be NULL either if we sent chainActive.Tip() OR
        // if our peer has chainActive.Tip() (and thus we are sending an empty
        // headers message). In both cases it's safe to update
        // pindexBestHeaderSent to be our tip.
        state->pindexBestHeaderSent = pindex ? pindex : chainActive.Tip();
        pfrom->PushMessage(NetMsgType::HEADERS, vHeaders);
    }


    else if (strCommand == NetMsgType::TX)
    {
        // Stop processing the transaction early if
        // We are in blocks only mode and peer is either not whitelisted or whitelistrelay is off
        if (GetBoolArg("-blocksonly", DEFAULT_BLOCKSONLY) &&
            (!pfrom->fWhitelisted || !GetBoolArg("-whitelistrelay", DEFAULT_WHITELISTRELAY)))
        {
            LOG(NET, "transaction sent in violation of protocol peer=%d\n", pfrom->id);
            return true;
        }

        // Put the tx on the tx admission queue for processing
        CTxInputData txd;
        vRecv >> txd.tx;

        // Indicate that the tx was received and is now in the commitQ but not necessarily in the mempool.
        CInv inv(MSG_TX, txd.tx->GetHash());
        requester.Processing(inv, pfrom);

        // Enqueue the transaction
        txd.nodeId = pfrom->id;
        txd.nodeName = pfrom->GetLogName();
        txd.whitelisted = pfrom->fWhitelisted;
        EnqueueTxForAdmission(txd);

        pfrom->AddInventoryKnown(inv);
        requester.UpdateTxnResponseTime(inv, pfrom);
    }


    else if (strCommand == NetMsgType::HEADERS) // Ignore headers received while importing
    {
        if (fImporting)
        {
            LOG(NET, "skipping processing of HEADERS because importing\n");
            return true;
        }
        if (fReindex)
        {
            LOG(NET, "skipping processing of HEADERS because reindexing\n");
            return true;
        }
        std::vector<CBlockHeader> headers;

        // Bypass the normal CBlock deserialization, as we don't want to risk deserializing 2000 full blocks.
        unsigned int nCount = ReadCompactSize(vRecv);
        if (nCount > MAX_HEADERS_RESULTS)
        {
            dosMan.Misbehaving(pfrom, 20);
            return error("headers message size = %u", nCount);
        }
        headers.resize(nCount);
        for (unsigned int n = 0; n < nCount; n++)
        {
            vRecv >> headers[n];
            ReadCompactSize(vRecv); // ignore tx count; assume it is 0.
        }

        LOCK(cs_main);

        // Nothing interesting. Stop asking this peers for more headers.
        if (nCount == 0)
            return true;

        // Check all headers to make sure they are continuous before attempting to accept them.
        // This prevents and attacker from keeping us from doing direct fetch by giving us out
        // of order headers.
        bool fNewUnconnectedHeaders = false;
        uint256 hashLastBlock;
        hashLastBlock.SetNull();
        for (const CBlockHeader &header : headers)
        {
            // check that the first header has a previous block in the blockindex.
            if (hashLastBlock.IsNull())
            {
                BlockMap::iterator mi = mapBlockIndex.find(header.hashPrevBlock);
                if (mi != mapBlockIndex.end())
                    hashLastBlock = header.hashPrevBlock;
            }

            // Add this header to the map if it doesn't connect to a previous header
            if (header.hashPrevBlock != hashLastBlock)
            {
                // If we still haven't finished downloading the initial headers during node sync and we get
                // an out of order header then we must disconnect the node so that we can finish downloading
                // initial headers from a diffeent peer. An out of order header at this point is likely an attack
                // to prevent the node from syncing.
                if (header.GetBlockTime() < GetAdjustedTime() - 24 * 60 * 60)
                {
                    pfrom->fDisconnect = true;
                    return error("non-continuous-headers sequence during node sync - disconnecting peer=%s",
                        pfrom->GetLogName());
                }
                fNewUnconnectedHeaders = true;
            }

            // if we have an unconnected header then add every following header to the unconnected headers cache.
            if (fNewUnconnectedHeaders)
            {
                uint256 hash = header.GetHash();
                if (mapUnConnectedHeaders.size() < MAX_UNCONNECTED_HEADERS)
                    mapUnConnectedHeaders[hash] = std::make_pair(header, GetTime());

                // update hashLastUnknownBlock so that we'll be able to download the block from this peer even
                // if we receive the headers, which will connect this one, from a different peer.
                requester.UpdateBlockAvailability(pfrom->GetId(), hash);
            }

            hashLastBlock = header.GetHash();
        }
        // return without error if we have an unconnected header.  This way we can try to connect it when the next
        // header arrives.
        if (fNewUnconnectedHeaders)
            return true;

        // If possible add any previously unconnected headers to the headers vector and remove any expired entries.
        std::map<uint256, std::pair<CBlockHeader, int64_t> >::iterator mi = mapUnConnectedHeaders.begin();
        while (mi != mapUnConnectedHeaders.end())
        {
            std::map<uint256, std::pair<CBlockHeader, int64_t> >::iterator toErase = mi;

            // Add the header if it connects to the previous header
            if (headers.back().GetHash() == (*mi).second.first.hashPrevBlock)
            {
                headers.push_back((*mi).second.first);
                mapUnConnectedHeaders.erase(toErase);

                // if you found one to connect then search from the beginning again in case there is another
                // that will connect to this new header that was added.
                mi = mapUnConnectedHeaders.begin();
                continue;
            }

            // Remove any entries that have been in the cache too long.  Unconnected headers should only exist
            // for a very short while, typically just a second or two.
            int64_t nTimeHeaderArrived = (*mi).second.second;
            uint256 headerHash = (*mi).first;
            mi++;
            if (GetTime() - nTimeHeaderArrived >= UNCONNECTED_HEADERS_TIMEOUT)
            {
                mapUnConnectedHeaders.erase(toErase);
            }
            // At this point we know the headers in the list received are known to be in order, therefore,
            // check if the header is equal to some other header in the list. If so then remove it from the cache.
            else
            {
                for (const CBlockHeader &header : headers)
                {
                    if (header.GetHash() == headerHash)
                    {
                        mapUnConnectedHeaders.erase(toErase);
                        break;
                    }
                }
            }
        }

        // Check and accept each header in dependency order (oldest block to most recent)
        CBlockIndex *pindexLast = nullptr;
        int i = 0;
        for (const CBlockHeader &header : headers)
        {
            CValidationState state;
            if (!AcceptBlockHeader(header, state, chainparams, &pindexLast))
            {
                int nDos;
                if (state.IsInvalid(nDos))
                {
                    if (nDos > 0)
                    {
                        dosMan.Misbehaving(pfrom, nDos);
                    }
                }
                // all headers from this one forward reference a fork that we don't follow, so erase them
                headers.erase(headers.begin() + i, headers.end());
                nCount = headers.size();
                break;
            }
            else
                PV->UpdateMostWorkOurFork(header);

            i++;
        }

        if (pindexLast)
            requester.UpdateBlockAvailability(pfrom->GetId(), pindexLast->GetBlockHash());

        if (nCount == MAX_HEADERS_RESULTS && pindexLast)
        {
            // Headers message had its maximum size; the peer may have more headers.
            // TODO: optimize: if pindexLast is an ancestor of chainActive.Tip or pindexBestHeader, continue
            // from there instead.
            LOG(NET, "more getheaders (%d) to end to peer=%s (startheight:%d)\n", pindexLast->nHeight,
                pfrom->GetLogName(), pfrom->nStartingHeight);
            pfrom->PushMessage(NetMsgType::GETHEADERS, chainActive.GetLocator(pindexLast), uint256());

            {
                CNodeState *state = nodestate.State(pfrom->GetId());
                DbgAssert(state != nullptr, );
                if (state)
                    state->nSyncStartTime = GetTime(); // reset the time because more headers needed
            }

            // During the process of IBD we need to update block availability for every connected peer. To do that we
            // request, from each NODE_NETWORK peer, a header that matches the last blockhash found in this recent set
            // of headers. Once the reqeusted header is received then the block availability for this peer will get
            // updated.
            if (IsInitialBlockDownload())
            {
                // To maintain locking order with cs_main we have to addrefs for each node and then release
                // the lock on cs_vNodes before aquiring cs_main further down.
                std::vector<CNode *> vNodesCopy;
                {
                    LOCK(cs_vNodes);
                    vNodesCopy = vNodes;
                    for (CNode *pnode : vNodes)
                    {
                        pnode->AddRef();
                    }
                }

                for (CNode *pnode : vNodesCopy)
                {
                    if (!pnode->fClient && pnode != pfrom)
                    {
                        LOCK(cs_main);
                        CNodeState *state = nodestate.State(pfrom->GetId());
                        DbgAssert(state != nullptr, ); // do not return, we need to release refs later.
                        if (state == nullptr)
                            continue;

                        if (state->pindexBestKnownBlock == nullptr ||
                            pindexLast->nChainWork > state->pindexBestKnownBlock->nChainWork)
                        {
                            // We only want one single header so we pass a null for CBlockLocator.
                            pnode->PushMessage(NetMsgType::GETHEADERS, CBlockLocator(), pindexLast->GetBlockHash());
                            LOG(NET | BLK, "Requesting header for blockavailability, peer=%s block=%s height=%d\n",
                                pnode->GetLogName(), pindexLast->GetBlockHash().ToString().c_str(),
                                pindexBestHeader.load()->nHeight);
                        }
                    }
                }

                // release refs
                for (CNode *pnode : vNodesCopy)
                    pnode->Release();
            }
        }

        bool fCanDirectFetch = CanDirectFetch(chainparams.GetConsensus());
        CNodeState *state = nodestate.State(pfrom->GetId());
        DbgAssert(state != nullptr, return false);

        // During the initial peer handshake we must receive the initial headers which should be greater
        // than or equal to our block height at the time of requesting GETHEADERS. This is because the peer has
        // advertised a height >= to our own. Furthermore, because the headers max returned is as much as 2000 this
        // could not be a mainnet re-org.
        if (!state->fFirstHeadersReceived)
        {
            // We want to make sure that the peer doesn't just send us any old valid header. The block height of the
            // last header they send us should be equal to our block height at the time we made the GETHEADERS request.
            if (pindexLast && state->nFirstHeadersExpectedHeight <= pindexLast->nHeight)
            {
                state->fFirstHeadersReceived = true;
                LOG(NET, "Initial headers received for peer=%s\n", pfrom->GetLogName());
            }

            // Allow for very large reorgs (> 2000 blocks) on the nol test chain or other test net.
            if (Params().NetworkIDString() != "main" && Params().NetworkIDString() != "regtest")
                state->fFirstHeadersReceived = true;
        }

        // update the syncd status.  This should come before we make calls to requester.AskFor().
        IsChainNearlySyncdInit();
        IsInitialBlockDownloadInit();

        // If this set of headers is valid and ends in a block with at least as
        // much work as our tip, download as much as possible.
        if (fCanDirectFetch && pindexLast && pindexLast->IsValid(BLOCK_VALID_TREE) &&
            chainActive.Tip()->nChainWork <= pindexLast->nChainWork)
        {
            // Set tweak value.  Mostly used in testing direct fetch.
            if (maxBlocksInTransitPerPeer.Value() != 0)
                pfrom->nMaxBlocksInTransit.store(maxBlocksInTransitPerPeer.Value());

            std::vector<CBlockIndex *> vToFetch;
            CBlockIndex *pindexWalk = pindexLast;
            // Calculate all the blocks we'd need to switch to pindexLast.
            while (pindexWalk && !chainActive.Contains(pindexWalk))
            {
                vToFetch.push_back(pindexWalk);
                pindexWalk = pindexWalk->pprev;
            }

            // Download as much as possible, from earliest to latest.
            unsigned int nAskFor = 0;
            for (auto pindex_iter = vToFetch.rbegin(); pindex_iter != vToFetch.rend(); pindex_iter++)
            {
                CBlockIndex *pindex = *pindex_iter;
                // pindex must be nonnull because we populated vToFetch a few lines above
                CInv inv(MSG_BLOCK, pindex->GetBlockHash());
                if (!AlreadyHaveBlock(inv))
                {
                    requester.AskFor(inv, pfrom);
                    LOG(REQ, "AskFor block via headers direct fetch %s (%d) peer=%d\n",
                        pindex->GetBlockHash().ToString(), pindex->nHeight, pfrom->id);
                    nAskFor++;
                }
                // We don't care about how many blocks are in flight.  We just need to make sure we don't
                // ask for more than the maximum allowed per peer because the request manager will take care
                // of any duplicate requests.
                if (nAskFor >= pfrom->nMaxBlocksInTransit.load())
                {
                    LOG(NET, "Large reorg, could only direct fetch %d blocks\n", nAskFor);
                    break;
                }
            }
            if (nAskFor > 1)
            {
                LOG(NET, "Downloading blocks toward %s (%d) via headers direct fetch\n",
                    pindexLast->GetBlockHash().ToString(), pindexLast->nHeight);
            }
        }

        CheckBlockIndex(chainparams.GetConsensus());
    }

    // BUIP010 Xtreme Thinblocks: begin section
    else if (strCommand == NetMsgType::GET_XTHIN && !fImporting && !fReindex && IsThinBlocksEnabled())
    {
        if (!BasicThinblockChecks(pfrom, chainparams))
            return false;

        CBloomFilter filterMemPool;
        CInv inv;
        vRecv >> inv >> filterMemPool;
        if (!((inv.type == MSG_XTHINBLOCK) || (inv.type == MSG_THINBLOCK)))
        {
            dosMan.Misbehaving(pfrom, 100);
            return error("message inv invalid type = %u", inv.type);
        }

        // Message consistency checking
        if (!((inv.type == MSG_XTHINBLOCK) || (inv.type == MSG_THINBLOCK)) || inv.hash.IsNull())
        {
            dosMan.Misbehaving(pfrom, 100);
            return error("invalid get_xthin type=%u hash=%s", inv.type, inv.hash.ToString());
        }


        // Validates that the filter is reasonably sized.
        LoadFilter(pfrom, &filterMemPool);
        {
            LOCK(cs_main);
            BlockMap::iterator mi = mapBlockIndex.find(inv.hash);
            if (mi == mapBlockIndex.end())
            {
                dosMan.Misbehaving(pfrom, 100);
                return error("Peer %srequested nonexistent block %s", pfrom->GetLogName(), inv.hash.ToString());
            }

            CBlock block;
            const Consensus::Params &consensusParams = Params().GetConsensus();
            if (!ReadBlockFromDisk(block, (*mi).second, consensusParams))
            {
                // We don't have the block yet, although we know about it.
                return error(
                    "Peer %s requested block %s that cannot be read", pfrom->GetLogName(), inv.hash.ToString());
            }
            else
            {
                SendXThinBlock(MakeBlockRef(block), pfrom, inv);
            }
        }
    }


    else if (strCommand == NetMsgType::XPEDITEDREQUEST)
    {
        return HandleExpeditedRequest(vRecv, pfrom);
    }
    else if (strCommand == NetMsgType::XPEDITEDBLK)
    {
        // ignore the expedited message unless we are at the chain tip...
        if (!fImporting && !fReindex && !IsInitialBlockDownload())
        {
            if (!HandleExpeditedBlock(vRecv, pfrom))
            {
                dosMan.Misbehaving(pfrom, 5);
                return false;
            }
        }
    }


    // BUVERSION is used to pass BU specific version information similar to NetMsgType::VERSION
    // and is exchanged after the VERSION and VERACK are both sent and received.
    else if (strCommand == NetMsgType::BUVERSION)
    {
        // If we never sent a VERACK message then we should not get a BUVERSION message.
        if (!pfrom->fVerackSent)
        {
            dosMan.Misbehaving(pfrom, 100);
            return error("BUVERSION received but we never sent a VERACK message - banning peer=%s version=%s",
                pfrom->GetLogName(), pfrom->cleanSubVer);
        }
        // Each connection can only send one version message
        if (pfrom->addrFromPort != 0)
        {
            pfrom->PushMessage(
                NetMsgType::REJECT, strCommand, REJECT_DUPLICATE, std::string("Duplicate BU version message"));
            dosMan.Misbehaving(pfrom, 100);
            return error("Duplicate BU version message received from peer=%s version=%s", pfrom->GetLogName(),
                pfrom->cleanSubVer);
        }

        // addrFromPort is needed for connecting and initializing Xpedited forwarding.
        vRecv >> pfrom->addrFromPort;
        pfrom->PushMessage(NetMsgType::BUVERACK);
    }
    // Final handshake for BU specific version information similar to NetMsgType::VERACK
    else if (strCommand == NetMsgType::BUVERACK)
    {
        // If we never sent a BUVERSION message then we should not get a VERACK message.
        if (!pfrom->fBUVersionSent)
        {
            dosMan.Misbehaving(pfrom, 100);
            return error("BUVERACK received but we never sent a BUVERSION message - banning peer=%s version=%s",
                pfrom->GetLogName(), pfrom->cleanSubVer);
        }

        // This step done after final handshake
        CheckAndRequestExpeditedBlocks(pfrom);
    }

    else if (strCommand == NetMsgType::XTHINBLOCK && !fImporting && !fReindex && !IsInitialBlockDownload() &&
             IsThinBlocksEnabled())
    {
        return CXThinBlock::HandleMessage(vRecv, pfrom, strCommand, 0);
    }


    else if (strCommand == NetMsgType::THINBLOCK && !fImporting && !fReindex && !IsInitialBlockDownload() &&
             IsThinBlocksEnabled())
    {
        return CThinBlock::HandleMessage(vRecv, pfrom);
    }


    else if (strCommand == NetMsgType::GET_XBLOCKTX && !fImporting && !fReindex && !IsInitialBlockDownload() &&
             IsThinBlocksEnabled())
    {
        return CXRequestThinBlockTx::HandleMessage(vRecv, pfrom);
    }


    else if (strCommand == NetMsgType::XBLOCKTX && !fImporting && !fReindex && !IsInitialBlockDownload() &&
             IsThinBlocksEnabled())
    {
        return CXThinBlockTx::HandleMessage(vRecv, pfrom);
    }
    // BUIP010 Xtreme Thinblocks: end section

    // BUIPXXX Graphene blocks: begin section
    else if (strCommand == NetMsgType::GET_GRAPHENE && !fImporting && !fReindex && IsGrapheneBlockEnabled())
    {
        return HandleGrapheneBlockRequest(vRecv, pfrom, chainparams);
    }

    else if (strCommand == NetMsgType::GRAPHENEBLOCK && !fImporting && !fReindex && !IsInitialBlockDownload() &&
             IsGrapheneBlockEnabled())
    {
        return CGrapheneBlock::HandleMessage(vRecv, pfrom, strCommand, 0);
    }


    else if (strCommand == NetMsgType::GET_GRAPHENETX && !fImporting && !fReindex && !IsInitialBlockDownload() &&
             IsGrapheneBlockEnabled())
    {
        return CRequestGrapheneBlockTx::HandleMessage(vRecv, pfrom);
    }


    else if (strCommand == NetMsgType::GRAPHENETX && !fImporting && !fReindex && !IsInitialBlockDownload() &&
             IsGrapheneBlockEnabled())
    {
        return CGrapheneBlockTx::HandleMessage(vRecv, pfrom);
    }
    // BUIPXXX Graphene blocks: end section


    else if (strCommand == NetMsgType::BLOCK && !fImporting && !fReindex) // Ignore blocks received while importing
    {
        CBlockRef pblock(new CBlock());
        {
            uint64_t nCheckBlockSize = vRecv.size();
            vRecv >> *pblock;

            // Sanity check. The serialized block size should match the size that is in our receive queue.  If not
            // this could be an attack block of some kind.
            DbgAssert(nCheckBlockSize == pblock->GetBlockSize(), return true);
        }

        CInv inv(MSG_BLOCK, pblock->GetHash());
        LOG(BLK, "received block %s peer=%d\n", inv.hash.ToString(), pfrom->id);
        UnlimitedLogBlock(*pblock, inv.hash.ToString(), receiptTime);

        if (IsChainNearlySyncd()) // BU send the received block out expedited channels quickly
        {
            CValidationState state;
            if (CheckBlockHeader(*pblock, state, true)) // block header is fine
                SendExpeditedBlock(*pblock, pfrom);
        }

        {
            LOCK(cs_main);
            CNodeState *state = nodestate.State(pfrom->GetId());
            DbgAssert(state != nullptr, );
            if (state)
                state->nSyncStartTime = GetTime(); // reset the getheaders time because block can consume all bandwidth
        }
        pfrom->nPingUsecStart = GetTimeMicros(); // Reset ping time because block can consume all bandwidth

        // Message consistency checking
        // NOTE: consistency checking is handled by checkblock() which is called during
        //       ProcessNewBlock() during HandleBlockMessage.
        PV->HandleBlockMessage(pfrom, strCommand, pblock, inv);
    }


    else if (strCommand == NetMsgType::GETADDR)
    {
        // This asymmetric behavior for inbound and outbound connections was introduced
        // to prevent a fingerprinting attack: an attacker can send specific fake addresses
        // to users' AddrMan and later request them by sending getaddr messages.
        // Making nodes which are behind NAT and can only make outgoing connections ignore
        // the getaddr message mitigates the attack.
        if (!pfrom->fInbound)
        {
            LOG(NET, "Ignoring \"getaddr\" from outbound connection. peer=%d\n", pfrom->id);
            return true;
        }

        // Only send one GetAddr response per connection to reduce resource waste
        //  and discourage addr stamping of INV announcements.
        if (pfrom->fSentAddr)
        {
            LOG(NET, "Ignoring repeated \"getaddr\". peer=%d\n", pfrom->id);
            return true;
        }
        pfrom->fSentAddr = true;
        LOCK(pfrom->cs_vSend);
        pfrom->vAddrToSend.clear();
        std::vector<CAddress> vAddr = addrman.GetAddr();
        FastRandomContext insecure_rand;
        for (const CAddress &addr : vAddr)
            pfrom->PushAddress(addr, insecure_rand);
    }


    else if (strCommand == NetMsgType::MEMPOOL)
    {
        if (CNode::OutboundTargetReached(false) && !pfrom->fWhitelisted)
        {
            LOG(NET, "mempool request with bandwidth limit reached, disconnect peer %s\n", pfrom->GetLogName());
            pfrom->fDisconnect = true;
            return true;
        }
        std::vector<uint256> vtxid;
        mempool.queryHashes(vtxid);
        std::vector<CInv> vInv;

        // Because we have to take cs_filter after mempool.cs, in order to maintain locking order, we
        // need find out if a filter is present first before later doing the mempool.get().
        bool fHaveFilter = false;
        {
            LOCK(pfrom->cs_filter);
            fHaveFilter = pfrom->pfilter ? true : false;
        }

        for (uint256 &hash : vtxid)
        {
            CInv inv(MSG_TX, hash);
            if (fHaveFilter)
            {
                CTransactionRef ptx = nullptr;
                ptx = mempool.get(inv.hash);
                if (ptx == nullptr)
                    continue; // another thread removed since queryHashes, maybe...

                LOCK(pfrom->cs_filter);
                if (!pfrom->pfilter->IsRelevantAndUpdate(*ptx))
                    continue;
            }
            vInv.push_back(inv);
            if (vInv.size() == MAX_INV_SZ)
            {
                pfrom->PushMessage(NetMsgType::INV, vInv);
                vInv.clear();
            }
        }
        if (vInv.size() > 0)
            pfrom->PushMessage(NetMsgType::INV, vInv);
    }


    else if (strCommand == NetMsgType::PING)
    {
        if (pfrom->nVersion > BIP0031_VERSION)
        {
            // take the lock exclusively to force a serialization point
            CSharedUnlocker unl(pfrom->csMsgSerializer);
            {
                WRITELOCK(pfrom->csMsgSerializer);
                uint64_t nonce = 0;
                vRecv >> nonce;
                // Echo the message back with the nonce. This allows for two useful features:
                //
                // 1) A remote node can quickly check if the connection is operational
                // 2) Remote nodes can measure the latency of the network thread. If this node
                //    is overloaded it won't respond to pings quickly and the remote node can
                //    avoid sending us more work, like chain download requests.
                //
                // The nonce stops the remote getting confused between different pings: without
                // it, if the remote node sends a ping once per second and this node takes 5
                // seconds to respond to each, the 5th ping the remote sends would appear to
                // return very quickly.
                pfrom->PushMessage(NetMsgType::PONG, nonce);
            }
        }
    }


    else if (strCommand == NetMsgType::PONG)
    {
        int64_t pingUsecEnd = nTimeReceived;
        uint64_t nonce = 0;
        size_t nAvail = vRecv.in_avail();
        bool bPingFinished = false;
        std::string sProblem;

        if (nAvail >= sizeof(nonce))
        {
            vRecv >> nonce;

            // Only process pong message if there is an outstanding ping (old ping without nonce should never pong)
            if (pfrom->nPingNonceSent != 0)
            {
                if (nonce == pfrom->nPingNonceSent)
                {
                    // Matching pong received, this ping is no longer outstanding
                    bPingFinished = true;
                    int64_t pingUsecTime = pingUsecEnd - pfrom->nPingUsecStart;
                    if (pingUsecTime > 0)
                    {
                        // Successful ping time measurement, replace previous
                        pfrom->nPingUsecTime = pingUsecTime;
                        pfrom->nMinPingUsecTime = std::min(pfrom->nMinPingUsecTime, pingUsecTime);
                    }
                    else
                    {
                        // This should never happen
                        sProblem = "Timing mishap";
                    }
                }
                else
                {
                    // Nonce mismatches are normal when pings are overlapping
                    sProblem = "Nonce mismatch";
                    if (nonce == 0)
                    {
                        // This is most likely a bug in another implementation somewhere; cancel this ping
                        bPingFinished = true;
                        sProblem = "Nonce zero";
                    }
                }
            }
            else
            {
                sProblem = "Unsolicited pong without ping";
            }
        }
        else
        {
            // This is most likely a bug in another implementation somewhere; cancel this ping
            bPingFinished = true;
            sProblem = "Short payload";
        }

        if (!(sProblem.empty()))
        {
            LOG(NET, "pong peer=%d: %s, %x expected, %x received, %u bytes\n", pfrom->id, sProblem,
                pfrom->nPingNonceSent, nonce, nAvail);
        }
        if (bPingFinished)
        {
            pfrom->nPingNonceSent = 0;
        }
    }


    else if (strCommand == NetMsgType::FILTERLOAD)
    {
        CBloomFilter filter;
        vRecv >> filter;

        if (!filter.IsWithinSizeConstraints())
        {
            // There is no excuse for sending a too-large filter
            dosMan.Misbehaving(pfrom, 100);
            return false;
        }
        else
        {
            LOCK(pfrom->cs_filter);
            delete pfrom->pfilter;
            pfrom->pfilter = new CBloomFilter(filter);
        }
        pfrom->fRelayTxes = true;
    }


    else if (strCommand == NetMsgType::FILTERADD)
    {
        std::vector<unsigned char> vData;
        vRecv >> vData;

        // Nodes must NEVER send a data item > 520 bytes (the max size for a script data object,
        // and thus, the maximum size any matched object can have) in a filteradd message
        if (vData.size() > MAX_SCRIPT_ELEMENT_SIZE)
        {
            dosMan.Misbehaving(pfrom, 100);
        }
        else
        {
            LOCK(pfrom->cs_filter);
            if (pfrom->pfilter)
                pfrom->pfilter->insert(vData);
            else
                dosMan.Misbehaving(pfrom, 100);
        }
    }


    else if (strCommand == NetMsgType::FILTERCLEAR)
    {
        LOCK(pfrom->cs_filter);
        delete pfrom->pfilter;
        pfrom->pfilter = new CBloomFilter();
        pfrom->fRelayTxes = true;
    }

    else if (strCommand == NetMsgType::FILTERSIZEXTHIN)
    {
        if (pfrom->ThinBlockCapable())
        {
            vRecv >> pfrom->nXthinBloomfilterSize;

            // As a safeguard don't allow a smaller max bloom filter size than the default max size.
            if (!pfrom->nXthinBloomfilterSize || (pfrom->nXthinBloomfilterSize < SMALLEST_MAX_BLOOM_FILTER_SIZE))
            {
                pfrom->PushMessage(
                    NetMsgType::REJECT, strCommand, REJECT_INVALID, std::string("filter size was too small"));
                LOG(NET, "Disconnecting %s: bloom filter size too small\n", pfrom->GetLogName());
                pfrom->fDisconnect = true;
                return false;
            }
        }
        else
        {
            pfrom->fDisconnect = true;
            return false;
        }
    }

    else if (strCommand == NetMsgType::REJECT)
    {
        // BU: Request manager: this was restructured to not just be active in fDebug mode so that the request manager
        // can be notified of request rejections.
        try
        {
            std::string strMsg;
            unsigned char ccode;
            std::string strReason;
            uint256 hash;

            vRecv >> LIMITED_STRING(strMsg, CMessageHeader::COMMAND_SIZE) >> ccode >>
                LIMITED_STRING(strReason, MAX_REJECT_MESSAGE_LENGTH);
            std::ostringstream ss;
            ss << strMsg << " code " << itostr(ccode) << ": " << strReason;

            // BU: Check request manager reject codes
            if (strMsg == NetMsgType::BLOCK || strMsg == NetMsgType::TX)
            {
                vRecv >> hash;
                ss << ": hash " << hash.ToString();

                // We need to see this reject message in either "req" or "net" debug mode
                LOG(REQ | NET, "Reject %s\n", SanitizeString(ss.str()));

                if (strMsg == NetMsgType::BLOCK)
                {
                    requester.Rejected(CInv(MSG_BLOCK, hash), pfrom, ccode);
                }
                else if (strMsg == NetMsgType::TX)
                {
                    requester.Rejected(CInv(MSG_TX, hash), pfrom, ccode);
                }
            }
            // if (fDebug) {
            // ostringstream ss;
            // ss << strMsg << " code " << itostr(ccode) << ": " << strReason;

            // if (strMsg == NetMsgType::BLOCK || strMsg == NetMsgType::TX)
            //  {
            //    ss << ": hash " << hash.ToString();
            //  }
            // LOG(NET, "Reject %s\n", SanitizeString(ss.str()));
            // }
        }
        catch (const std::ios_base::failure &)
        {
            // Avoid feedback loops by preventing reject messages from triggering a new reject message.
            LOG(NET, "Unparseable reject message received\n");
            LOG(REQ, "Unparseable reject message received\n");
        }
    }

    else
    {
        // Ignore unknown commands for extensibility
        LOG(NET, "Unknown command \"%s\" from peer=%d\n", SanitizeString(strCommand), pfrom->id);
    }

    return true;
}


bool ProcessMessages(CNode *pfrom)
{
    const CChainParams &chainparams = Params();
    // if (fDebug)
    //    LOGA("%s(%u messages)\n", __func__, pfrom->vRecvMsg.size());

    //
    // Message format
    //  (4) message start
    //  (12) command
    //  (4) size
    //  (4) checksum
    //  (x) data
    //
    bool fOk = true;
    bool gotWorkDone = false;

    {
        TRY_LOCK(pfrom->csRecvGetData, locked);
        if (locked && !pfrom->vRecvGetData.empty())
        {
            gotWorkDone |= ProcessGetData(pfrom, chainparams.GetConsensus(), pfrom->vRecvGetData);
        }
    }

    int msgsProcessed = 0;
    // Don't bother if send buffer is too full to respond anyway
    while ((!pfrom->fDisconnect) && (pfrom->nSendSize < SendBufferSize()))
    {
        READLOCK(pfrom->csMsgSerializer);
        CNetMessage msg;
        {
            TRY_LOCK(pfrom->cs_vRecvMsg, lockRecv);
            if (!lockRecv)
                break;

            if (pfrom->vRecvMsg.empty())
                break;
            CNetMessage &msgOnQ = pfrom->vRecvMsg.front();
            if (!msgOnQ.complete()) // end if an incomplete message is on the top
            {
                // LogPrintf("%s: partial message %d of size %d. Recvd bytes: %d\n", pfrom->GetLogName(),
                // msgOnQ.nDataPos, msgOnQ.size(), pfrom->currentRecvMsgSize.value);
                break;
            }
            msg = msgOnQ;
            // at this point, any failure means we can delete the current message
            pfrom->vRecvMsg.pop_front();
            pfrom->currentRecvMsgSize -= msg.size();
            msgsProcessed++;
            gotWorkDone = true;
        }

        // if (fDebug)
        //    LOGA("%s(message %u msgsz, %u bytes, complete:%s)\n", __func__,
        //            msg.hdr.nMessageSize, msg.vRecv.size(),
        //            msg.complete() ? "Y" : "N");

        // Scan for message start
        if (memcmp(msg.hdr.pchMessageStart, pfrom->GetMagic(chainparams), MESSAGE_START_SIZE) != 0)
        {
            LOG(NET, "PROCESSMESSAGE: INVALID MESSAGESTART %s peer=%s\n", SanitizeString(msg.hdr.GetCommand()),
                pfrom->GetLogName());
            if (!pfrom->fWhitelisted)
            {
                dosMan.Ban(pfrom->addr, BanReasonNodeMisbehaving, 4 * 60 * 60); // ban for 4 hours
            }
            fOk = false;
            break;
        }

        // Read header
        CMessageHeader &hdr = msg.hdr;
        if (!hdr.IsValid(pfrom->GetMagic(chainparams)))
        {
            LOGA(
                "PROCESSMESSAGE: ERRORS IN HEADER %s peer=%s\n", SanitizeString(hdr.GetCommand()), pfrom->GetLogName());
            continue;
        }
        std::string strCommand = hdr.GetCommand();

        // Message size
        unsigned int nMessageSize = hdr.nMessageSize;

        // Checksum
        CDataStream &vRecv = msg.vRecv;
        uint256 hash = Hash(vRecv.begin(), vRecv.begin() + nMessageSize);
        unsigned int nChecksum = ReadLE32((unsigned char *)&hash);
        if (nChecksum != hdr.nChecksum)
        {
            LOGA("%s(%s, %u bytes): CHECKSUM ERROR nChecksum=%08x hdr.nChecksum=%08x\n", __func__,
                SanitizeString(strCommand), nMessageSize, nChecksum, hdr.nChecksum);
            continue;
        }

        // Process message
        bool fRet = false;
        try
        {
            fRet = ProcessMessage(pfrom, strCommand, vRecv, msg.nTime);
            boost::this_thread::interruption_point();
        }
        catch (const std::ios_base::failure &e)
        {
            pfrom->PushMessage(NetMsgType::REJECT, strCommand, REJECT_MALFORMED, std::string("error parsing message"));
            if (strstr(e.what(), "end of data"))
            {
                // Allow exceptions from under-length message on vRecv
                LOGA("%s(%s, %u bytes): Exception '%s' caught, normally caused by a message being shorter than "
                     "its stated length\n",
                    __func__, SanitizeString(strCommand), nMessageSize, e.what());
            }
            else if (strstr(e.what(), "size too large"))
            {
                // Allow exceptions from over-long size
                LOGA("%s(%s, %u bytes): Exception '%s' caught\n", __func__, SanitizeString(strCommand), nMessageSize,
                    e.what());
            }
            else
            {
                PrintExceptionContinue(&e, "ProcessMessages()");
            }
        }
        catch (const boost::thread_interrupted &)
        {
            throw;
        }
        catch (const std::exception &e)
        {
            PrintExceptionContinue(&e, "ProcessMessages()");
        }
        catch (...)
        {
            PrintExceptionContinue(NULL, "ProcessMessages()");
        }

        if (!fRet)
            LOGA("%s(%s, %u bytes) FAILED peer %s\n", __func__, SanitizeString(strCommand), nMessageSize,
                pfrom->GetLogName());

        if (msgsProcessed > 2000)
            break; // let someone else do something periodically
    }

    return fOk;
}

static bool CheckForDownloadTimeout(CNode *pto, bool fReceived, int64_t &nRequestTime)
{
    // Use a timeout of 6 times the retry inverval before disconnecting.  This way only a max of 6
    // re-requested thinblocks or graphene blocks could be in memory at any one time.
    if (!fReceived && (GetTime() - nRequestTime) > 6 * blkReqRetryInterval / 1000000)
    {
        if (!pto->fWhitelisted && Params().NetworkIDString() != "regtest")
        {
            LOG(THIN, "ERROR: Disconnecting peer %s due to thinblock download timeout exceeded (%d secs)\n",
                pto->GetLogName(), (GetTime() - nRequestTime));
            pto->fDisconnect = true;
            return true;
        }
    }
    return false;
}

bool SendMessages(CNode *pto)
{
    const Consensus::Params &consensusParams = Params().GetConsensus();
    {
        // First set fDisconnect if appropriate.
        pto->DisconnectIfBanned();

        // Check for an internal disconnect request and if true then set fDisconnect. This would typically happen
        // during initial sync when a peer has a slow connection and we want to disconnect them.  We want to then
        // wait for any blocks that are still in flight before disconnecting, rather than re-requesting them again.
        if (pto->fDisconnectRequest)
        {
            NodeId nodeid = pto->GetId();
            int nInFlight = requester.GetNumBlocksInFlight(nodeid);
            LOG(IBD, "peer %s, checking disconnect request with %d in flight blocks\n", pto->GetLogName(), nInFlight);
            if (nInFlight == 0)
            {
                pto->fDisconnect = true;
                LOG(IBD, "peer %s, disconnect request was set, so disconnected\n", pto->GetLogName());
            }
        }

        // Now exit early if disconnecting or the version handshake is not complete.  We must not send PING or other
        // connection maintenance messages before the handshake is done.
        if (pto->fDisconnect || !pto->fSuccessfullyConnected)
            return true;

        //
        // Message: ping
        //
        bool pingSend = false;
        if (pto->fPingQueued)
        {
            // RPC ping request by user
            pingSend = true;
        }
        if (pto->nPingNonceSent == 0 && pto->nPingUsecStart + PING_INTERVAL * 1000000 < GetTimeMicros())
        {
            // Ping automatically sent as a latency probe & keepalive.
            pingSend = true;
        }
        if (pingSend)
        {
            uint64_t nonce = 0;
            while (nonce == 0)
            {
                GetRandBytes((unsigned char *)&nonce, sizeof(nonce));
            }
            pto->fPingQueued = false;
            pto->nPingUsecStart = GetTimeMicros();
            if (pto->nVersion > BIP0031_VERSION)
            {
                pto->nPingNonceSent = nonce;
                pto->PushMessage(NetMsgType::PING, nonce);
            }
            else
            {
                // Peer is too old to support ping command with nonce, pong will never arrive.
                pto->nPingNonceSent = 0;
                pto->PushMessage(NetMsgType::PING);
            }
        }

        // Check to see if there are any thinblocks or graphene blocks in flight that have gone beyond the
        // timeout interval. If so then we need to disconnect them so that the thinblock data is nullified.
        // We could null the associated data here but that would possibly cause a node to be banned later if
        // the thinblock or graphene block finally did show up, so instead we just disconnect this slow node.
        {
            LOCK(pto->cs_mapthinblocksinflight);
            if (!pto->mapThinBlocksInFlight.empty())
            {
                for (auto &item : pto->mapThinBlocksInFlight)
                {
                    if (CheckForDownloadTimeout(pto, item.second.fReceived, item.second.nRequestTime))
                        break;
                }
            }
        }
        {
            LOCK(pto->cs_mapgrapheneblocksinflight);
            if (!pto->mapGrapheneBlocksInFlight.empty())
            {
                for (auto &item : pto->mapGrapheneBlocksInFlight)
                {
                    if (CheckForDownloadTimeout(pto, item.second.fReceived, item.second.nRequestTime))
                        break;
                }
            }
        }

        // Check for block download timeout and disconnect node if necessary. Does not require cs_main.
        int64_t nNow = GetTimeMicros();
        requester.DisconnectOnDownloadTimeout(pto, consensusParams, nNow);

        // Address refresh broadcast
        if (!IsInitialBlockDownload() && pto->nNextLocalAddrSend < nNow)
        {
            AdvertiseLocal(pto);
            pto->nNextLocalAddrSend = PoissonNextSend(nNow, AVG_LOCAL_ADDRESS_BROADCAST_INTERVAL);
        }

        //
        // Message: addr
        //
        if (pto->nNextAddrSend < nNow)
        {
            LOCK(pto->cs_vSend);
            pto->nNextAddrSend = PoissonNextSend(nNow, AVG_ADDRESS_BROADCAST_INTERVAL);
            std::vector<CAddress> vAddr;
            vAddr.reserve(pto->vAddrToSend.size());
            for (const CAddress &addr : pto->vAddrToSend)
            {
                if (!pto->addrKnown.contains(addr.GetKey()))
                {
                    pto->addrKnown.insert(addr.GetKey());
                    vAddr.push_back(addr);
                    // receiver rejects addr messages larger than 1000
                    if (vAddr.size() >= 1000)
                    {
                        pto->PushMessage(NetMsgType::ADDR, vAddr);
                        vAddr.clear();
                    }
                }
            }
            pto->vAddrToSend.clear();
            if (!vAddr.empty())
                pto->PushMessage(NetMsgType::ADDR, vAddr);
        }

        CNodeState *state = nodestate.State(pto->GetId());
        if (state == nullptr)
        {
            return true;
        }

        // If a sync has been started check whether we received the first batch of headers requested within the timeout
        // period. If not then disconnect and ban the node and a new node will automatically be selected to start the
        // headers download.
        if ((state->fSyncStarted) && (state->nSyncStartTime < GetTime() - INITIAL_HEADERS_TIMEOUT) &&
            (!state->fFirstHeadersReceived) && !pto->fWhitelisted)
        {
            // pto->fDisconnect = true;
            LOGA("Initial headers were either not received or not received before the timeout\n", pto->GetLogName());
        }

        // Start block sync
        if (pindexBestHeader == nullptr)
            pindexBestHeader = chainActive.Tip();
        // Download if this is a nice peer, or we have no nice peers and this one might do.
        bool fFetch = state->fPreferredDownload || (nPreferredDownload.load() == 0 && !pto->fOneShot);
        if (!state->fSyncStarted && !fImporting && !fReindex)
        {
            // Only allow the downloading of headers from a single pruned peer.
            static int nSyncStartedPruned = 0;
            if (pto->fClient && nSyncStartedPruned >= 1)
                fFetch = false;

            // Only actively request headers from a single peer, unless we're close to today.
            if ((nSyncStarted < MAX_HEADER_REQS_DURING_IBD && fFetch) ||
                chainActive.Tip()->GetBlockTime() > GetAdjustedTime() - SINGLE_PEER_REQUEST_MODE_AGE)
            {
                const CBlockIndex *pindexStart = chainActive.Tip();
                /* If possible, start at the block preceding the currently
                   best known header.  This ensures that we always get a
                   non-empty list of headers back as long as the peer
                   is up-to-date.  With a non-empty response, we can initialise
                   the peer's known best block.  This wouldn't be possible
                   if we requested starting at pindexBestHeader and
                   got back an empty response.  */
                if (pindexStart->pprev)
                    pindexStart = pindexStart->pprev;
                // BU Bug fix for Core:  Don't start downloading headers unless our chain is shorter
                if (pindexStart->nHeight < pto->nStartingHeight)
                {
                    state->fSyncStarted = true;
                    state->nSyncStartTime = GetTime();
                    state->fRequestedInitialBlockAvailability = true;
                    state->nFirstHeadersExpectedHeight = pindexStart->nHeight;
                    nSyncStarted++;

                    if (pto->fClient)
                        nSyncStartedPruned++;

                    LOG(NET, "initial getheaders (%d) to peer=%s (startheight:%d)\n", pindexStart->nHeight,
                        pto->GetLogName(), pto->nStartingHeight);
                    pto->PushMessage(NetMsgType::GETHEADERS, chainActive.GetLocator(pindexStart), uint256());
                }
            }
        }

        // During IBD and when a new NODE_NETWORK peer connects we have to ask for if it has our best header in order
        // to update our block availability. We only want/need to do this only once per peer (if the initial batch of
        // headers has still not been etirely donwnloaded yet then the block availability will be updated during that
        // process rather than here).
        if (IsInitialBlockDownload() && !state->fRequestedInitialBlockAvailability &&
            state->pindexBestKnownBlock == nullptr && !fReindex && !fImporting)
        {
            if (!pto->fClient)
            {
                state->fRequestedInitialBlockAvailability = true;

                // We only want one single header so we pass a null CBlockLocator.
                pto->PushMessage(NetMsgType::GETHEADERS, CBlockLocator(), pindexBestHeader.load()->GetBlockHash());
                LOG(NET | BLK, "Requesting header for initial blockavailability, peer=%s block=%s height=%d\n",
                    pto->GetLogName(), pindexBestHeader.load()->GetBlockHash().ToString(),
                    pindexBestHeader.load()->nHeight);
            }
        }

        // Resend wallet transactions that haven't gotten in a block yet
        // Except during reindex, importing and IBD, when old wallet
        // transactions become unconfirmed and spams other nodes.
        if (!fReindex && !fImporting && !IsInitialBlockDownload())
        {
            GetMainSignals().Broadcast(nTimeBestReceived.load());
        }

        //
        // Try sending block announcements via headers
        //
        {
            // If we have less than MAX_BLOCKS_TO_ANNOUNCE in our
            // list of block hashes we're relaying, and our peer wants
            // headers announcements, then find the first header
            // not yet known to our peer but would connect, and send.
            // If no header would connect, or if we have too many
            // blocks, or if the peer doesn't want headers, just
            // add all to the inv queue.
            std::vector<uint256> blockHashesToAnnounce;
            {
                LOCK(pto->cs_inventory);
                // make a copy so that we do not need to keep cs_inventory which cannot be taken before cs_main
                blockHashesToAnnounce = pto->vBlockHashesToAnnounce; // TODO optimize
                pto->vBlockHashesToAnnounce.clear();
            }

            std::vector<CBlock> vHeaders;
            bool fRevertToInv = (!state->fPreferHeaders || pto->vBlockHashesToAnnounce.size() > MAX_BLOCKS_TO_ANNOUNCE);
            CBlockIndex *pBestIndex = nullptr; // last header queued for delivery
            requester.ProcessBlockAvailability(pto->id); // ensure pindexBestKnownBlock is up-to-date

            if (!fRevertToInv)
            {
                bool fFoundStartingHeader = false;
                // Try to find first header that our peer doesn't have, and
                // then send all headers past that one.  If we come across any
                // headers that aren't on chainActive, give up.
                for (const uint256 &hash : blockHashesToAnnounce)
                {
                    CBlockIndex *pindex = nullptr;
                    {
                        LOCK(cs_main);
                        BlockMap::iterator mi = mapBlockIndex.find(hash);
                        // BU skip blocks that we don't know about.  was: assert(mi != mapBlockIndex.end());
                        if (mi == mapBlockIndex.end())
                            continue;
                        pindex = mi->second;
                        if (chainActive[pindex->nHeight] != pindex)
                        {
                            // Bail out if we reorged away from this block
                            fRevertToInv = true;
                            break;
                        }
                    }


                    if (pBestIndex != nullptr && pindex->pprev != pBestIndex)
                    {
                        // This means that the list of blocks to announce don't
                        // connect to each other.
                        // This shouldn't really be possible to hit during
                        // regular operation (because reorgs should take us to
                        // a chain that has some block not on the prior chain,
                        // which should be caught by the prior check), but one
                        // way this could happen is by using invalidateblock /
                        // reconsiderblock repeatedly on the tip, causing it to
                        // be added multiple times to vBlockHashesToAnnounce.
                        // Robustly deal with this rare situation by reverting
                        // to an inv.
                        fRevertToInv = true;
                        break;
                    }
                    pBestIndex = pindex;
                    if (fFoundStartingHeader)
                    {
                        // add this to the headers message
                        vHeaders.push_back(pindex->GetBlockHeader());
                    }
                    else if (PeerHasHeader(state, pindex))
                    {
                        continue; // keep looking for the first new block
                    }
                    else if (pindex->pprev == NULL || PeerHasHeader(state, pindex->pprev))
                    {
                        // Peer doesn't have this header but they do have the prior one.
                        // Start sending headers.
                        fFoundStartingHeader = true;
                        vHeaders.push_back(pindex->GetBlockHeader());
                    }
                    else
                    {
                        // Peer doesn't have this header or the prior one -- nothing will
                        // connect, so bail out.
                        fRevertToInv = true;
                        break;
                    }
                }
            }
            if (fRevertToInv)
            {
                // If falling back to using an inv, just try to inv the tip.
                // The last entry in vBlockHashesToAnnounce was our tip at some point
                // in the past.
                if (!blockHashesToAnnounce.empty())
                {
                    for (const uint256 &hashToAnnounce : blockHashesToAnnounce)
                    {
                        CBlockIndex *pindex = nullptr;
                        {
                            LOCK(cs_main);
                            BlockMap::iterator mi = mapBlockIndex.find(hashToAnnounce);
                            if (mi == mapBlockIndex.end())
                                continue;
                            pindex = mi->second;

                            // Warn if we're announcing a block that is not on the main chain.
                            // This should be very rare and could be optimized out.
                            // Just log for now.
                            if (chainActive[pindex->nHeight] != pindex)
                            {
                                LOG(NET, "Announcing block %s not on main chain (tip=%s)\n", hashToAnnounce.ToString(),
                                    chainActive.Tip()->GetBlockHash().ToString());
                            }
                        }

                        // If the peer announced this block to us, don't inv it back.
                        // (Since block announcements may not be via inv's, we can't solely rely on
                        // setInventoryKnown to track this.)
                        if (!PeerHasHeader(state, pindex))
                        {
                            pto->PushInventory(CInv(MSG_BLOCK, hashToAnnounce));
                            LOG(NET, "%s: sending inv peer=%d hash=%s\n", __func__, pto->id, hashToAnnounce.ToString());
                        }
                    }
                }
            }
            else if (!vHeaders.empty())
            {
                if (vHeaders.size() > 1)
                {
                    LOG(NET, "%s: %u headers, range (%s, %s), to peer=%d\n", __func__, vHeaders.size(),
                        vHeaders.front().GetHash().ToString(), vHeaders.back().GetHash().ToString(), pto->id);
                }
                else
                {
                    LOG(NET, "%s: sending header %s to peer=%d\n", __func__, vHeaders.front().GetHash().ToString(),
                        pto->id);
                }
                LOCK(pto->cs_vSend);
                pto->PushMessage(NetMsgType::HEADERS, vHeaders);
                state->pindexBestHeaderSent = pBestIndex;
            }
        }

        //
        // Message: inventory
        //
        // We must send all INV's before returning otherwise, under very heavy transaction rates, we could end up
        // falling behind in sending INV's and vInventoryToSend could possibly get quite large.
        std::vector<CInv> vInvSend;
        while (!pto->vInventoryToSend.empty())
        {
            // Send message INV up to the MAX_INV_TO_SEND. Once we reach the max then send the INV message
            // and if there is any remaining it will be sent on the next iteration until vInventoryToSend is empty.
            int nToErase = 0;
            {
                // BU - here we only want to forward message inventory if our peer has actually been requesting
                // useful data or giving us useful data.  We give them 2 minutes to be useful but then choke off
                // their inventory.  This prevents fake peers from connecting and listening to our inventory
                // while providing no value to the network.
                // However we will still send them block inventory in the case they are a pruned node or wallet
                // waiting for block announcements, therefore we have to check each inv in pto->vInventoryToSend.
                bool fChokeTxInv = (pto->nActivityBytes == 0 && (nNow / 1000000 - pto->nTimeConnected) > 120);

                // Find INV's which should be sent, save them to vInvSend, and then erase from vInventoryToSend.
                int invsz = std::min((int)pto->vInventoryToSend.size(), MAX_INV_TO_SEND);
                vInvSend.reserve(invsz);

                LOCK(pto->cs_inventory);
                for (const CInv &inv : pto->vInventoryToSend)
                {
                    nToErase++;

                    if (inv.type == MSG_TX)
                    {
                        if (fChokeTxInv)
                            continue;
                        // skip if we already know about this one
                        if (pto->filterInventoryKnown.contains(inv.hash))
                            continue;
                    }
                    vInvSend.push_back(inv);
                    pto->filterInventoryKnown.insert(inv.hash);

                    if (vInvSend.size() >= MAX_INV_TO_SEND)
                        break;
                }

                if (nToErase > 0)
                {
                    pto->vInventoryToSend.erase(
                        pto->vInventoryToSend.begin(), pto->vInventoryToSend.begin() + nToErase);
                }
            }

            // To maintain proper locking order we have to push the message when we do not hold cs_inventory which
            // was held in the section above.
            if (nToErase > 0)
            {
                LOCK(pto->cs_vSend);
                if (!vInvSend.empty())
                    pto->PushMessage(NetMsgType::INV, vInvSend);
            }
        }

        // Request the next blocks. Mostly this will get exucuted during IBD but sometimes even
        // when the chain is syncd a block will get request via this method.
        requester.RequestNextBlocksToDownload(pto);
    }
    return true;
}

ThresholdState VersionBitsTipState(const Consensus::Params &params, Consensus::DeploymentPos pos)
{
    LOCK(cs_main);
    return VersionBitsState(chainActive.Tip(), params, pos, versionbitscache);
}

void MainCleanup()
{
    {
        LOCK(cs_main); // BU apply the appropriate lock so no contention during destruction
        // block headers
        BlockMap::iterator it1 = mapBlockIndex.begin();
        for (; it1 != mapBlockIndex.end(); it1++)
            delete (*it1).second;
        mapBlockIndex.clear();
    }

    {
        WRITELOCK(orphanpool.cs); // BU apply the appropriate lock so no contention during destruction
        // orphan transactions
        orphanpool.mapOrphanTransactions.clear();
        orphanpool.mapOrphanTransactionsByPrev.clear();
    }
}
