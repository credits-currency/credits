// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2014 The Bitcoin developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "main.h"

#include "addrman.h"
#include "alert.h"
#include "chainparams.h"
#include "checkpoints.h"
#include "checkqueue.h"
#include "init.h"
#include "net.h"
#include "txdb.h"
#include "txmempool.h"
#include "ui_interface.h"
#include "util.h"

#include <sstream>
#include <vector>

#include <boost/algorithm/string/replace.hpp>
#include <boost/filesystem.hpp>
#include <boost/filesystem/fstream.hpp>

#include "subsidylevels.h"

using namespace std;
using namespace boost;

#if defined(NDEBUG)
# error "Credits cannot be compiled without assertions."
#endif

//
// Global state
//

CCriticalSection cs_main;
CCriticalSection cs_LastBlockFile;
MainState credits_mainState;

Bitcredit_CTxMemPool credits_mempool;

map<uint256, Credits_CBlockIndex*> credits_mapBlockIndex;
Bitcredit_CChain credits_chainActive;
Bitcredit_CChain bitcredit_chainMostWork;
int64_t credits_nTimeBestReceived = 0;
int bitcredit_nScriptCheckThreads = 0;
bool bitcredit_fBenchmark = false;
bool bitcredit_fTxIndex = false;
unsigned int bitcredit_nCoinCacheSize = 5000;

/** Fees smaller than this (in satoshi) are considered zero fee (for transaction creation) */
CFeeRate Credits_CTransaction::minTxFee = CFeeRate(10000);  // Override with -mintxfee
/** Fees smaller than this (in satoshi) are considered zero fee (for relaying and mining) */
CFeeRate Credits_CTransaction::minRelayTxFee = CFeeRate(1000);

COrphanIndex credits_orphanIndex("credits_orphans");

map<uint256, Credits_CTransaction> bitcredit_mapOrphanTransactions;
map<uint256, set<uint256> > bitcredit_mapOrphanTransactionsByPrev;

// Constant stuff for coinbase transactions we create:
CScript BITCREDIT_COINBASE_FLAGS;

const string bitcredit_strMessageMagic = "Credits Signed Message:\n";

// Internal stuff
namespace {
    struct Credits_CBlockIndexWorkComparator
    {
        bool operator()(Credits_CBlockIndex *pa, Credits_CBlockIndex *pb) {
            // First sort by most total work, ...
            if (pa->nChainWork > pb->nChainWork) return false;
            if (pa->nChainWork < pb->nChainWork) return true;

            // ... then by earliest time received, ...
            if (pa->nSequenceId < pb->nSequenceId) return false;
            if (pa->nSequenceId > pb->nSequenceId) return true;

            // Use pointer address as tie breaker (should only happen with blocks
            // loaded from disk, as those all have id 0).
            if (pa < pb) return false;
            if (pa > pb) return true;

            // Identical blocks.
            return false;
        }
    };

    Credits_CBlockIndex *bitcredit_pindexBestInvalid;
    // may contain all CBlockIndex*'s that have validness >=BLOCK_VALID_TRANSACTIONS, and must contain those who aren't failed
    set<Credits_CBlockIndex*, Credits_CBlockIndexWorkComparator> bitcredit_setBlockIndexValid;

    // Every received block is assigned a unique and increasing identifier, so we
    // know which one to give priority in case of a fork.
    CCriticalSection bitcredit_cs_nBlockSequenceId;
    // Blocks loaded from disk are assigned id 0, so start the counter at 1.
    uint32_t bitcredit_nBlockSequenceId = 1;

    // Sources of received blocks, to be able to send them reject messages or ban
    // them, if processing happens afterwards. Protected by cs_main.
    map<uint256, NodeId> bitcredit_mapBlockSource;

    map<uint256, pair<NodeId, list<QueuedBlock>::iterator> > bitcredit_mapBlocksInFlight;
    map<uint256, pair<NodeId, list<uint256>::iterator> > bitcredit_mapBlocksToDownload;
}

//////////////////////////////////////////////////////////////////////////////
//
// dispatching functions
//

// These functions dispatch to one or all registered wallets

namespace {
struct Bitcredit_CMainSignals {
    // Notifies listeners of updated transaction data (passing hash, transaction, and optionally the block it is found in.
    boost::signals2::signal<void (const Bitcoin_CWallet *bitcoin_wallet, const uint256 &, const Credits_CTransaction &, const Credits_CBlock *)> SyncTransaction;
    // Notifies listeners of an erased transaction (currently disabled, requires transaction replacement).
    boost::signals2::signal<void (Credits_CWallet *credits_wallet, const uint256 &)> EraseTransaction;
    // Notifies listeners of an updated transaction without new data (for now: a coinbase potentially becoming visible).
    boost::signals2::signal<void (const uint256 &)> UpdatedTransaction;
    // Notifies listeners of a new active block chain.
    boost::signals2::signal<void (const CBlockLocator &)> SetBestChain;
    // Notifies listeners about an inventory item being seen on the network.
    boost::signals2::signal<void (const uint256 &)> Inventory;
    // Tells listeners to broadcast their data.
    boost::signals2::signal<void ()> Broadcast;
} bitcredit_g_signals;
}

void Bitcredit_RegisterWallet(Credits_CWalletInterface* pwalletIn) {
    bitcredit_g_signals.SyncTransaction.connect(boost::bind(&Credits_CWalletInterface::SyncTransaction, pwalletIn, _1, _2, _3, _4));
    bitcredit_g_signals.EraseTransaction.connect(boost::bind(&Credits_CWalletInterface::EraseFromWallet, pwalletIn, _1, _2));
    bitcredit_g_signals.UpdatedTransaction.connect(boost::bind(&Credits_CWalletInterface::UpdatedTransaction, pwalletIn, _1));
    bitcredit_g_signals.SetBestChain.connect(boost::bind(&Credits_CWalletInterface::SetBestChain, pwalletIn, _1));
    bitcredit_g_signals.Inventory.connect(boost::bind(&Credits_CWalletInterface::Inventory, pwalletIn, _1));
    bitcredit_g_signals.Broadcast.connect(boost::bind(&Credits_CWalletInterface::ResendWalletTransactions, pwalletIn));
}

void Bitcredit_UnregisterWallet(Credits_CWalletInterface* pwalletIn) {
    bitcredit_g_signals.Broadcast.disconnect(boost::bind(&Credits_CWalletInterface::ResendWalletTransactions, pwalletIn));
    bitcredit_g_signals.Inventory.disconnect(boost::bind(&Credits_CWalletInterface::Inventory, pwalletIn, _1));
    bitcredit_g_signals.SetBestChain.disconnect(boost::bind(&Credits_CWalletInterface::SetBestChain, pwalletIn, _1));
    bitcredit_g_signals.UpdatedTransaction.disconnect(boost::bind(&Credits_CWalletInterface::UpdatedTransaction, pwalletIn, _1));
    bitcredit_g_signals.EraseTransaction.disconnect(boost::bind(&Credits_CWalletInterface::EraseFromWallet, pwalletIn, _1, _2));
    bitcredit_g_signals.SyncTransaction.disconnect(boost::bind(&Credits_CWalletInterface::SyncTransaction, pwalletIn, _1, _2, _3, _4));
}

void Bitcredit_UnregisterAllWallets() {
    bitcredit_g_signals.Broadcast.disconnect_all_slots();
    bitcredit_g_signals.Inventory.disconnect_all_slots();
    bitcredit_g_signals.SetBestChain.disconnect_all_slots();
    bitcredit_g_signals.UpdatedTransaction.disconnect_all_slots();
    bitcredit_g_signals.EraseTransaction.disconnect_all_slots();
    bitcredit_g_signals.SyncTransaction.disconnect_all_slots();
}

void Bitcredit_SyncWithWallets(const Bitcoin_CWallet *bitcoin_wallet, const uint256 &hash, const Credits_CTransaction &tx, const Credits_CBlock *pblock) {
    bitcredit_g_signals.SyncTransaction(bitcoin_wallet, hash, tx, pblock);
}

//////////////////////////////////////////////////////////////////////////////
//
// Registration of network node signals.
//

namespace {

// Map maintaining per-node state. Requires cs_main.
map<NodeId, CNodeState> bitcredit_mapNodeState;

// Requires cs_main.
CNodeState *Bitcredit_State(NodeId pnode) {
    map<NodeId, CNodeState>::iterator it = bitcredit_mapNodeState.find(pnode);
    if (it == bitcredit_mapNodeState.end())
        return NULL;
    return &it->second;
}

int Bitcredit_GetHeight()
{
    LOCK(cs_main);
    return credits_chainActive.Height();
}

void Bitcredit_InitializeNode(NodeId nodeid, const CNode *pnode) {
    LOCK(cs_main);
    CNodeState &state = bitcredit_mapNodeState.insert(std::make_pair(nodeid, CNodeState())).first->second;
    state.name = pnode->addrName;
}

void Bitcredit_FinalizeNode(NodeId nodeid) {
    LOCK(cs_main);
    CNodeState *state = Bitcredit_State(nodeid);

    BOOST_FOREACH(const QueuedBlock& entry, state->vBlocksInFlight)
        bitcredit_mapBlocksInFlight.erase(entry.hash);
    BOOST_FOREACH(const uint256& hash, state->vBlocksToDownload)
        bitcredit_mapBlocksToDownload.erase(hash);

    bitcredit_mapNodeState.erase(nodeid);
}

// Requires cs_main.
void Bitcredit_MarkBlockAsReceived(const uint256 &hash, NodeId nodeFrom = -1) {
    map<uint256, pair<NodeId, list<uint256>::iterator> >::iterator itToDownload = bitcredit_mapBlocksToDownload.find(hash);
    if (itToDownload != bitcredit_mapBlocksToDownload.end()) {
        CNodeState *state = Bitcredit_State(itToDownload->second.first);
        state->vBlocksToDownload.erase(itToDownload->second.second);
        state->nBlocksToDownload--;
        bitcredit_mapBlocksToDownload.erase(itToDownload);
    }

    map<uint256, pair<NodeId, list<QueuedBlock>::iterator> >::iterator itInFlight = bitcredit_mapBlocksInFlight.find(hash);
    if (itInFlight != bitcredit_mapBlocksInFlight.end()) {
        CNodeState *state = Bitcredit_State(itInFlight->second.first);
        state->vBlocksInFlight.erase(itInFlight->second.second);
        state->nBlocksInFlight--;
        if (itInFlight->second.first == nodeFrom)
            state->nLastBlockReceive = GetTimeMicros();
        bitcredit_mapBlocksInFlight.erase(itInFlight);
    }

}

// Requires cs_main.
bool Bitcredit_AddBlockToQueue(NodeId nodeid, const uint256 &hash) {
    if (bitcredit_mapBlocksToDownload.count(hash) || bitcredit_mapBlocksInFlight.count(hash))
        return false;

    CNodeState *state = Bitcredit_State(nodeid);
    if (state == NULL)
        return false;

    list<uint256>::iterator it = state->vBlocksToDownload.insert(state->vBlocksToDownload.end(), hash);
    state->nBlocksToDownload++;
    if (state->nBlocksToDownload > 5000)
        Bitcredit_Misbehaving(nodeid, 10);
    bitcredit_mapBlocksToDownload[hash] = std::make_pair(nodeid, it);
    return true;
}

// Requires cs_main.
void Bitcredit_MarkBlockAsInFlight(NodeId nodeid, const uint256 &hash) {
    CNodeState *state = Bitcredit_State(nodeid);
    assert(state != NULL);

    // Make sure it's not listed somewhere already.
    Bitcredit_MarkBlockAsReceived(hash);

    QueuedBlock newentry = {hash, GetTimeMicros(), state->nBlocksInFlight};
    if (state->nBlocksInFlight == 0)
        state->nLastBlockReceive = newentry.nTime; // Reset when a first request is sent.
    list<QueuedBlock>::iterator it = state->vBlocksInFlight.insert(state->vBlocksInFlight.end(), newentry);
    state->nBlocksInFlight++;
    bitcredit_mapBlocksInFlight[hash] = std::make_pair(nodeid, it);
}

}

bool Bitcredit_GetNodeStateStats(NodeId nodeid, CNodeStateStats &stats) {
    LOCK(cs_main);
    CNodeState *state = Bitcredit_State(nodeid);
    if (state == NULL)
        return false;
    stats.nMisbehavior = state->nMisbehavior;
    return true;
}

void Bitcredit_RegisterNodeSignals(CNodeSignals& nodeSignals)
{
    nodeSignals.GetHeight.connect(&Bitcredit_GetHeight);
    nodeSignals.ProcessMessages.connect(&Bitcredit_ProcessMessages);
    nodeSignals.SendMessages.connect(&Bitcredit_SendMessages);
    nodeSignals.InitializeNode.connect(&Bitcredit_InitializeNode);
    nodeSignals.FinalizeNode.connect(&Bitcredit_FinalizeNode);
}

void Bitcredit_UnregisterNodeSignals(CNodeSignals& nodeSignals)
{
    nodeSignals.GetHeight.disconnect(&Bitcredit_GetHeight);
    nodeSignals.ProcessMessages.disconnect(&Bitcredit_ProcessMessages);
    nodeSignals.SendMessages.disconnect(&Bitcredit_SendMessages);
    nodeSignals.InitializeNode.disconnect(&Bitcredit_InitializeNode);
    nodeSignals.FinalizeNode.disconnect(&Bitcredit_FinalizeNode);
}

//////////////////////////////////////////////////////////////////////////////
//
// CChain implementation
//

CBlockIndexBase *Bitcredit_CChain::SetTip(CBlockIndexBase *pindex) {
    if (pindex == NULL) {
        vChain.clear();
        return NULL;
    }
    vChain.resize(pindex->nHeight + 1);
    while (pindex && vChain[pindex->nHeight] != pindex) {
        vChain[pindex->nHeight] = pindex;
        pindex = pindex->pprev;
    }
    return pindex;
}

CBlockLocator Bitcredit_CChain::GetLocator(const CBlockIndexBase *pindex) const {
    int nStep = 1;
    std::vector<uint256> vHave;
    vHave.reserve(32);

    if (!pindex)
        pindex = Tip();
    while (pindex) {
        vHave.push_back(pindex->GetBlockHash());
        // Stop when we have added the genesis block.
        if (pindex->nHeight == 0)
            break;
        // Exponentially larger steps back, plus the genesis block.
        int nHeight = std::max(pindex->nHeight - nStep, 0);
        // In case pindex is not in this chain, iterate pindex->pprev to find blocks.
        while (pindex->nHeight > nHeight && !Contains(pindex))
            pindex = pindex->pprev;
        // If pindex is in this chain, use direct height-based access.
        if (pindex->nHeight > nHeight)
            pindex = (*this)[nHeight];
        if (vHave.size() > 10)
            nStep *= 2;
    }

    return CBlockLocator(vHave);
}

Credits_CBlockIndex *Bitcredit_CChain::FindFork(const CBlockLocator &locator) const {
    // Find the first block the caller has in the main chain
    BOOST_FOREACH(const uint256& hash, locator.vHave) {
        std::map<uint256, Credits_CBlockIndex*>::iterator mi = credits_mapBlockIndex.find(hash);
        if (mi != credits_mapBlockIndex.end())
        {
            Credits_CBlockIndex* pindex = (*mi).second;
            if (Contains(pindex))
                return pindex;
        }
    }
    return Genesis();
}

Credits_CCoinsViewCache *credits_pcoinsTip = NULL;
Credits_CBlockTreeDB *bitcredit_pblocktree = NULL;

//////////////////////////////////////////////////////////////////////////////
//
// mapOrphanTransactions
//

bool Bitcredit_AddOrphanTx(const Credits_CTransaction& tx)
{
    uint256 hash = tx.GetHash();
    if (bitcredit_mapOrphanTransactions.count(hash))
        return false;

    // Ignore big transactions, to avoid a
    // send-big-orphans memory exhaustion attack. If a peer has a legitimate
    // large transaction with a missing parent then we assume
    // it will rebroadcast it later, after the parent transaction(s)
    // have been mined or received.
    // 10,000 orphans, each of which is at most 5,000 bytes big is
    // at most 500 megabytes of orphans:
    unsigned int sz = tx.GetSerializeSize(SER_NETWORK, Credits_CTransaction::CURRENT_VERSION);
    if (sz > 5000)
    {
        LogPrint("mempool", "ignoring large orphan tx (size: %u, hash: %s)\n", sz, hash.ToString());
        return false;
    }

    bitcredit_mapOrphanTransactions[hash] = tx;
    BOOST_FOREACH(const Credits_CTxIn& txin, tx.vin)
        bitcredit_mapOrphanTransactionsByPrev[txin.prevout.hash].insert(hash);

    LogPrint("mempool", "stored orphan tx %s (mapsz %u)\n", hash.ToString(),
        bitcredit_mapOrphanTransactions.size());
    return true;
}

void static Bitcredit_EraseOrphanTx(uint256 hash)
{
    if (!bitcredit_mapOrphanTransactions.count(hash))
        return;
    const Credits_CTransaction& tx = bitcredit_mapOrphanTransactions[hash];
    BOOST_FOREACH(const Credits_CTxIn& txin, tx.vin)
    {
        bitcredit_mapOrphanTransactionsByPrev[txin.prevout.hash].erase(hash);
        if (bitcredit_mapOrphanTransactionsByPrev[txin.prevout.hash].empty())
            bitcredit_mapOrphanTransactionsByPrev.erase(txin.prevout.hash);
    }
    bitcredit_mapOrphanTransactions.erase(hash);
}

unsigned int Bitcredit_LimitOrphanTxSize(unsigned int nMaxOrphans)
{
    unsigned int nEvicted = 0;
    while (bitcredit_mapOrphanTransactions.size() > nMaxOrphans)
    {
        // Evict a random orphan:
        uint256 randomhash = GetRandHash();
        map<uint256, Credits_CTransaction>::iterator it = bitcredit_mapOrphanTransactions.lower_bound(randomhash);
        if (it == bitcredit_mapOrphanTransactions.end())
            it = bitcredit_mapOrphanTransactions.begin();
        Bitcredit_EraseOrphanTx(it->first);
        ++nEvicted;
    }
    return nEvicted;
}







bool Bitcredit_IsStandardTx(const Credits_CTransaction& tx, string& reason)
{
    AssertLockHeld(cs_main);
    if (tx.nVersion > Credits_CTransaction::CURRENT_VERSION || tx.nVersion < 1) {
        reason = "version";
        return false;
    }

    // Treat non-final transactions as non-standard to prevent a specific type
    // of double-spend attack, as well as DoS attacks. (if the transaction
    // can't be mined, the attacker isn't expending resources broadcasting it)
    // Basically we don't want to propagate transactions that can't included in
    // the next block.
    //
    // However, IsFinalTx() is confusing... Without arguments, it uses
    // chainActive.Height() to evaluate nLockTime; when a block is accepted, chainActive.Height()
    // is set to the value of nHeight in the block. However, when IsFinalTx()
    // is called within CBlock::AcceptBlock(), the height of the block *being*
    // evaluated is what is used. Thus if we want to know if a transaction can
    // be part of the *next* block, we need to call IsFinalTx() with one more
    // than chainActive.Height().
    //
    // Timestamps on the other hand don't get any special treatment, because we
    // can't know what timestamp the next block will have, and there aren't
    // timestamp applications where it matters.
    if (!Credits_IsFinalTx(tx, credits_chainActive.Height() + 1)) {
        reason = "non-final";
        return false;
    }

    // Extremely large transactions with lots of inputs can cost the network
    // almost as much to process as they cost the sender in fees, because
    // computing signature hashes is O(ninputs*txsize). Limiting transactions
    // to MAX_STANDARD_TX_SIZE mitigates CPU exhaustion attacks.
    unsigned int sz = tx.GetSerializeSize(SER_NETWORK, Credits_CTransaction::CURRENT_VERSION);
    if (sz >= CREDITS_MAX_STANDARD_TX_SIZE) {
        reason = "tx-size";
        return false;
    }

    BOOST_FOREACH(const Credits_CTxIn& txin, tx.vin)
    {
        // Biggest 'standard' txin is a 15-of-15 P2SH multisig with compressed
        // keys. (remember the 520 byte limit on redeemScript size) That works
        // out to a (15*(33+1))+3=513 byte redeemScript, 513+1+15*(73+1)=1624
        // bytes of scriptSig, which we round off to 1650 bytes for some minor
        // future-proofing. That's also enough to spend a 20-of-20
        // CHECKMULTISIG scriptPubKey, though such a scriptPubKey is not
        // considered standard)
        if (txin.scriptSig.size() > 1650) {
            reason = "scriptsig-size";
            return false;
        }
        if (!txin.scriptSig.IsPushOnly()) {
            reason = "scriptsig-not-pushonly";
            return false;
        }
        if (!txin.scriptSig.HasCanonicalPushes()) {
            reason = "scriptsig-non-canonical-push";
            return false;
        }
    }

    unsigned int nDataOut = 0;
    txnouttype whichType;
    BOOST_FOREACH(const CTxOut& txout, tx.vout) {
        if (!::IsStandard(txout.scriptPubKey, whichType)) {
            reason = "scriptpubkey";
            return false;
        }
        if (whichType == TX_NULL_DATA)
            nDataOut++;
        else if (txout.IsDust(Credits_CTransaction::minRelayTxFee)) {
            reason = "dust";
            return false;
        }
    }

    // only one OP_RETURN txout is permitted
    if (nDataOut > 1) {
        reason = "multi-op-return";
        return false;
    }

    return true;
}

bool Credits_IsFinalTx(const Credits_CTransaction &tx, int nBlockHeight, int64_t nBlockTime)
{
    AssertLockHeld(cs_main);
    // Time based nLockTime implemented in 0.1.6
    if (tx.nLockTime == 0)
        return true;
    if (nBlockHeight == 0)
        nBlockHeight = credits_chainActive.Height();
    if (nBlockTime == 0)
        nBlockTime = GetAdjustedTime();
    if ((int64_t)tx.nLockTime < ((int64_t)tx.nLockTime < BITCREDIT_LOCKTIME_THRESHOLD ? (int64_t)nBlockHeight : nBlockTime))
        return true;
    return false;
}

//
// Check transaction inputs, and make sure any
// pay-to-script-hash transactions are evaluating IsStandard scripts
//
// Why bother? To avoid denial-of-service attacks; an attacker
// can submit a standard HASH... OP_EQUAL transaction,
// which will get accepted into blocks. The redemption
// script can be anything; an attacker could use a very
// expensive-to-check-upon-redemption script like:
//   DUP CHECKSIG DROP ... repeated 100 times... OP_1
//
bool Bitcredit_AreInputsStandard(const Credits_CTransaction& tx, Credits_CCoinsViewCache& credits_view)
{
    if (tx.IsCoinBase())
        return true; // Coinbases don't use vin normally

	for (unsigned int i = 0; i < tx.vin.size(); i++) {
		const CScript& prevScript = tx.IsClaim() ? credits_view.Claim_GetOutputScriptFor(tx.vin[i]): credits_view.Credits_GetOutputFor(tx.vin[i]).scriptPubKey;

		vector<vector<unsigned char> > vSolutions;
		txnouttype whichType;
		if (!Solver(prevScript, whichType, vSolutions))
			return false;
		int nArgsExpected = ScriptSigArgsExpected(whichType, vSolutions);
		if (nArgsExpected < 0)
			return false;

		// Transactions with extra stuff in their scriptSigs are
		// non-standard. Note that this EvalScript() call will
		// be quick, because if there are any operations
		// beside "push data" in the scriptSig the
		// IsStandard() call returns false
		vector<vector<unsigned char> > stack;
		if (!Bitcredit_EvalScript(stack, tx.vin[i].scriptSig, tx, i, false, 0))
			return false;

		if (whichType == TX_SCRIPTHASH)
		{
			if (stack.empty())
				return false;
			CScript subscript(stack.back().begin(), stack.back().end());
			vector<vector<unsigned char> > vSolutions2;
			txnouttype whichType2;
			if (!Solver(subscript, whichType2, vSolutions2))
				return false;
			if (whichType2 == TX_SCRIPTHASH)
				return false;

			int tmpExpected;
			tmpExpected = ScriptSigArgsExpected(whichType2, vSolutions2);
			if (tmpExpected < 0)
				return false;
			nArgsExpected += tmpExpected;
		}

		if (stack.size() != (unsigned int)nArgsExpected)
			return false;
	}

    return true;
}

unsigned int Bitcredit_GetLegacySigOpCount(const Credits_CTransaction& tx)
{
    unsigned int nSigOps = 0;
    BOOST_FOREACH(const Credits_CTxIn& txin, tx.vin)
    {
        nSigOps += txin.scriptSig.GetSigOpCount(false);
    }
    BOOST_FOREACH(const CTxOut& txout, tx.vout)
    {
        nSigOps += txout.scriptPubKey.GetSigOpCount(false);
    }
    return nSigOps;
}

unsigned int Bitcredit_GetP2SHSigOpCount(const Credits_CTransaction& tx, Credits_CCoinsViewCache& bitcredit_inputs)
{
    if (tx.IsCoinBase())
        return 0;

    unsigned int nSigOps = 0;
	if(tx.IsClaim()) {
	    for (unsigned int i = 0; i < tx.vin.size(); i++) {
			const CScript &prevoutScript = bitcredit_inputs.Claim_GetOutputScriptFor(tx.vin[i]);
			if (prevoutScript.IsPayToScriptHash())
				nSigOps += prevoutScript.GetSigOpCount(tx.vin[i].scriptSig);
	    }
	} else {
	    for (unsigned int i = 0; i < tx.vin.size(); i++) {
			const CScript &prevoutScript = bitcredit_inputs.Credits_GetOutputFor(tx.vin[i]).scriptPubKey;
			if (prevoutScript.IsPayToScriptHash())
				nSigOps += prevoutScript.GetSigOpCount(tx.vin[i].scriptSig);
	    }
	}
    return nSigOps;
}

int Credits_CMerkleTx::SetMerkleBranch(const Credits_CBlock* pblock)
{
    AssertLockHeld(cs_main);
    Credits_CBlock blockTmp;

    if (pblock == NULL) {
    	Credits_CCoins coins;
        if (credits_pcoinsTip->Credits_GetCoins(GetHash(), coins)) {
            Credits_CBlockIndex *pindex = credits_chainActive[coins.nHeight];
            if (pindex) {
                if (!Credits_ReadBlockFromDisk(blockTmp, pindex))
                    return 0;
                pblock = &blockTmp;
            }
        }
    }

    if (pblock) {
        // Update the tx's hashBlock
        hashBlock = pblock->GetHash();

        // Locate the transaction
        for (nIndex = 0; nIndex < (int)pblock->vtx.size(); nIndex++)
            if (pblock->vtx[nIndex] == *(Credits_CTransaction*)this)
                break;
        if (nIndex == (int)pblock->vtx.size())
        {
            vMerkleBranch.clear();
            nIndex = -1;
            LogPrintf("Credits: ERROR: SetMerkleBranch() : couldn't find tx in block\n");
            return 0;
        }

        // Fill in merkle branch
        vMerkleBranch = pblock->GetMerkleBranch(nIndex);
    }

    // Is the tx in a block that's in the main chain
    map<uint256, Credits_CBlockIndex*>::iterator mi = credits_mapBlockIndex.find(hashBlock);
    if (mi == credits_mapBlockIndex.end())
        return 0;
    Credits_CBlockIndex* pindex = (*mi).second;
    if (!pindex || !credits_chainActive.Contains(pindex))
        return 0;

    return credits_chainActive.Height() - pindex->nHeight + 1;
}







bool Bitcredit_CheckTransaction(const Credits_CTransaction& tx, CValidationState &state)
{
    // Basic checks that don't depend on any context
    if (tx.vin.empty())
        return state.DoS(10, error("Credits: CheckTransaction() : vin empty"),
                         BITCREDIT_REJECT_INVALID, "bad-txns-vin-empty");
    if (tx.vout.empty())
        return state.DoS(10, error("Credits: CheckTransaction() : vout empty"),
                         BITCREDIT_REJECT_INVALID, "bad-txns-vout-empty");
    // Size limits
    if (::GetSerializeSize(tx, SER_NETWORK, CREDITS_PROTOCOL_VERSION) > BITCREDIT_MAX_BLOCK_SIZE)
        return state.DoS(100, error("Credits: CheckTransaction() : size limits failed"),
                         BITCREDIT_REJECT_INVALID, "bad-txns-oversize");

    // Check for negative or overflow output values
    int64_t nValueOut = 0;
    BOOST_FOREACH(const CTxOut& txout, tx.vout)
    {
        if (txout.nValue == 0)
            return state.DoS(100, error("Credits: CheckTransaction() : txout.nValue is zero"),
                             BITCREDIT_REJECT_INVALID, "bad-txns-vout-zero");
        if (txout.nValue < 0)
            return state.DoS(100, error("Credits: CheckTransaction() : txout.nValue negative"),
                             BITCREDIT_REJECT_INVALID, "bad-txns-vout-negative");
        if (txout.nValue > BITCREDIT_MAX_MONEY)
            return state.DoS(100, error("Credits: CheckTransaction() : txout.nValue too high"),
                             BITCREDIT_REJECT_INVALID, "bad-txns-vout-toolarge");
        nValueOut += txout.nValue;
        if (!Credits_MoneyRange(nValueOut))
            return state.DoS(100, error("Credits: CheckTransaction() : txout total out of range"),
                             BITCREDIT_REJECT_INVALID, "bad-txns-txouttotal-toolarge");
    }

    // Check for duplicate inputs
    set<COutPoint> vInOutPoints;
    BOOST_FOREACH(const Credits_CTxIn& txin, tx.vin)
    {
        if (vInOutPoints.count(txin.prevout))
            return state.DoS(100, error("Credits: CheckTransaction() : duplicate inputs"),
                             BITCREDIT_REJECT_INVALID, "bad-txns-inputs-duplicate");
        vInOutPoints.insert(txin.prevout);
    }

    if (tx.IsCoinBase())
    {
        if (tx.vin[0].scriptSig.size() < 2 || tx.vin[0].scriptSig.size() > 100)
            return state.DoS(100, error("Credits: CheckTransaction() : coinbase script size"),
                             BITCREDIT_REJECT_INVALID, "bad-cb-length");
    }
    else
    {
        BOOST_FOREACH(const Credits_CTxIn& txin, tx.vin)
            if (txin.prevout.IsNull())
                return state.DoS(10, error("Credits: CheckTransaction() : prevout is null"),
                                 BITCREDIT_REJECT_INVALID, "bad-txns-prevout-null");
    }

    if(tx.IsDeposit() || tx.IsClaim()) {
        if (tx.nLockTime != 0)
            return state.DoS(10, error("Credits: CheckTransaction() : claim and deposits can not have a locktime set"),
                             BITCREDIT_REJECT_INVALID, "bad-txns-locktime-set");
    }

    return true;
}

int64_t Credits_GetMinFee(const Credits_CTransaction& tx, unsigned int nBytes, bool fAllowFree, enum GetMinFee_mode mode)
{
    // Base fee is either minTxFee or minRelayTxFee
    CFeeRate baseFeeRate = (mode == GMF_RELAY) ? tx.minRelayTxFee : tx.minTxFee;

    int64_t nMinFee = baseFeeRate.GetFee(nBytes);

    if (fAllowFree)
    {
        // There is a free transaction area in blocks created by most miners,
        // * If we are relaying we allow transactions up to DEFAULT_BLOCK_PRIORITY_SIZE - 1000
        //   to be considered to fall into this category. We don't want to encourage sending
        //   multiple transactions instead of one big transaction to avoid fees.
        // * If we are creating a transaction we allow transactions up to 1,000 bytes
        //   to be considered safe and assume they can likely make it into this section.
        if (nBytes < (mode == GMF_SEND ? 1000 : (BITCREDIT_DEFAULT_BLOCK_PRIORITY_SIZE - 1000)))
            nMinFee = 0;
    }

    if (!Credits_MoneyRange(nMinFee))
        nMinFee = BITCREDIT_MAX_MONEY;
    return nMinFee;
}


bool Bitcredit_AcceptToMemoryPool(Bitcredit_CTxMemPool& pool, CValidationState &state, const Credits_CTransaction &tx, bool fLimitFree,
                        bool* pfMissingInputs, bool fRejectInsaneFee)
{
    AssertLockHeld(cs_main);
    if (pfMissingInputs)
        *pfMissingInputs = false;

    if (!Bitcredit_CheckTransaction(tx, state))
        return error("Credits: AcceptToMemoryPool: : CheckTransaction failed");

    // Coinbase is only valid in a block, not as a loose transaction
    if (tx.IsCoinBase())
        return state.DoS(100, error("Credits: AcceptToMemoryPool: : coinbase as individual tx"),
                         BITCREDIT_REJECT_INVALID, "coinbase");

    // Deposit is only valid in a block, not as a loose transaction
    if (tx.IsDeposit())
        return state.DoS(100, error("Credits: AcceptToMemoryPool: : deposit as individual tx"),
                         BITCREDIT_REJECT_INVALID, "deposit");

    // Rather not work on nonstandard transactions (unless -testnet/-regtest)
    string reason;
    if (Credits_Params().NetworkID() == CChainParams::MAIN && !Bitcredit_IsStandardTx(tx, reason))
        return state.DoS(0,
                         error("Credits: AcceptToMemoryPool : nonstandard transaction: %s", reason),
                         BITCREDIT_REJECT_NONSTANDARD, reason);

    // is it already in the memory pool?
    uint256 hash = tx.GetHash();
    if (pool.exists(hash))
        return false;

    // Check for conflicts with in-memory transactions
    {
    LOCK(pool.cs); // protect pool.mapNextTx
    if(!tx.IsClaim()) {
    	for (unsigned int i = 0; i < tx.vin.size(); i++) {
			COutPoint outpoint = tx.vin[i].prevout;
			if (pool.mapNextTx.count(outpoint))
			{
				// Disable replacement feature for now
				return false;
			}
    	}
    }
    }

    {
        uint256 bestBlockHash;

    	Credits_CCoinsView bitcredit_dummy;
        Credits_CCoinsViewCache credits_view(bitcredit_dummy);

        int64_t nValueIn = 0;
        {
        LOCK(pool.cs);
        Credits_CCoinsViewMemPool credits_viewMemPool(*credits_pcoinsTip, pool);
        credits_view.Credits_SetBackend(credits_viewMemPool);

        // do we already have it?
        if (credits_view.Credits_HaveCoins(hash))
            return false;

        // do all inputs exist?
        // Note that this does not check for the presence of actual outputs (see the next check for that),
        // only helps filling in pfMissingInputs (to determine missing vs spent).
        if(!tx.IsClaim()) {
        	BOOST_FOREACH(const Credits_CTxIn txin, tx.vin) {
				if (!credits_view.Credits_HaveCoins(txin.prevout.hash)) {
					if (pfMissingInputs)
						*pfMissingInputs = true;
					return false;
				}
        	}

			// are the actual inputs available?
			if (!credits_view.Credits_HaveInputs(tx))
				return state.Invalid(error("Credits: AcceptToMemoryPool : credits inputs already spent"),
									 BITCREDIT_REJECT_DUPLICATE, "bad-txns-bitcredit-inputs-spent");
        }

        // do all inputs exist?
        // Note that this does not check for the presence of actual outputs (see the next check for that),
        // only helps filling in pfMissingInputs (to determine missing vs spent).
        if(tx.IsClaim()) {
        	BOOST_FOREACH(const Credits_CTxIn txin, tx.vin) {
				if (!credits_view.Claim_HaveCoins(txin.prevout.hash)) {
					if (pfMissingInputs)
						*pfMissingInputs = true;
					return false;
				}
        	}

        	if (!credits_view.Claim_HaveInputs(tx))
        		return state.Invalid(error("Credits: AcceptToMemoryPool : bitcoin inputs already spent"),
        				BITCREDIT_REJECT_DUPLICATE, "bad-txns-bitcoin-inputs-spent");
        }

        // Bring the best block into scope
        bestBlockHash = credits_view.Claim_GetBestBlock();

        // Bring the best block into scope
        credits_view.Credits_GetBestBlock();

        if(tx.IsClaim()) {
        	nValueIn = credits_view.Claim_GetValueIn(tx);
        } else {
        	nValueIn = credits_view.Credits_GetValueIn(tx);
        }

        // we have all inputs cached now, so switch back to dummy, so we don't need to keep lock on mempool
        credits_view.Credits_SetBackend(bitcredit_dummy);
        }

        // Check for non-standard pay-to-script-hash in inputs
        if (Credits_Params().NetworkID() == CChainParams::MAIN && !Bitcredit_AreInputsStandard(tx, credits_view))
            return error("Credits: AcceptToMemoryPool: : nonstandard transaction input");

        // Note: if you modify this code to accept non-standard transactions, then
        // you should add code here to check that the transaction does a
        // reasonable number of ECDSA signature verifications.

        int64_t nValueOut = tx.GetValueOut();
        int64_t nFees = nValueIn-nValueOut;

        Bitcoin_CBlockIndex *bitcoin_pindexPrev = bitcoin_mapBlockIndex.find(bestBlockHash)->second;
        double dPriority = 0;
        if(tx.IsClaim()) {
        	dPriority = credits_view.Claim_GetPriority(tx, bitcoin_pindexPrev->nHeight);
        } else {
        	dPriority = credits_view.Credits_GetPriority(tx, credits_chainActive.Height());
        }

        Bitcredit_CTxMemPoolEntry entry(tx, nFees, GetTime(), dPriority, credits_chainActive.Height());
        unsigned int nSize = entry.GetTxSize();

        // Don't accept it if it can't get into a block
        int64_t txMinFee = Credits_GetMinFee(tx, nSize, true, GMF_RELAY);
        if (fLimitFree && nFees < txMinFee)
            return state.DoS(0, error("Credits: AcceptToMemoryPool : not enough fees %s, %d < %d",
                                      hash.ToString(), nFees, txMinFee),
                             BITCREDIT_REJECT_INSUFFICIENTFEE, "insufficient fee");

        // Continuously rate-limit free (really, very-low-fee)transactions
        // This mitigates 'penny-flooding' -- sending thousands of free transactions just to
        // be annoying or make others' transactions take longer to confirm.
        if (fLimitFree && nFees < Credits_CTransaction::minRelayTxFee.GetFee(nSize))
        {
            static CCriticalSection csFreeLimiter;
            static double dFreeCount;
            static int64_t nLastTime;
            int64_t nNow = GetTime();

            LOCK(csFreeLimiter);

            // Use an exponentially decaying ~10-minute window:
            dFreeCount *= pow(1.0 - 1.0/600.0, (double)(nNow - nLastTime));
            nLastTime = nNow;
            // -limitfreerelay unit is thousand-bytes-per-minute
            // At default rate it would take over a month to fill 1GB
            if (dFreeCount >= GetArg("-limitfreerelay", 15)*10*1000)
                return state.DoS(0, error("Credits: AcceptToMemoryPool : free transaction rejected by rate limiter"),
                                 BITCREDIT_REJECT_INSUFFICIENTFEE, "insufficient priority");
            LogPrint("mempool", "Rate limit dFreeCount: %g => %g\n", dFreeCount, dFreeCount+nSize);
            dFreeCount += nSize;
        }

        if (fRejectInsaneFee && nFees > Credits_CTransaction::minRelayTxFee.GetFee(nSize) * 10000)
            return error("Credits: AcceptToMemoryPool: : insane fees %s, %d > %d",
                         hash.ToString(),
                         nFees, Credits_CTransaction::minRelayTxFee.GetFee(nSize) * 10000);

        int64_t nClaimCoins = 0;
        // Check against previous transactions
        // This is done last to help prevent CPU exhaustion denial-of-service attacks.
        if (!Credits_CheckInputs(tx, state, credits_view, nClaimCoins, true, STANDARD_SCRIPT_VERIFY_FLAGS))
        {
            return error("Credits: AcceptToMemoryPool: : ConnectInputs failed %s", hash.ToString());
        }
        if(!Bitcredit_FindBestBlockAndCheckClaims(credits_view, nClaimCoins)) {
        	return false;
        }

        // Store transaction in memory
        pool.addUnchecked(hash, entry);
    }

    bitcredit_g_signals.SyncTransaction(bitcoin_pwalletMain, hash, tx, NULL);

    return true;
}


int Credits_CMerkleTx::GetDepthInMainChainINTERNAL(Credits_CBlockIndex* &pindexRet) const
{
    if (hashBlock == 0 || nIndex == -1)
        return 0;
    AssertLockHeld(cs_main);

    // Find the block it claims to be in
    map<uint256, Credits_CBlockIndex*>::iterator mi = credits_mapBlockIndex.find(hashBlock);
    if (mi == credits_mapBlockIndex.end())
        return 0;
    Credits_CBlockIndex* pindex = (*mi).second;
    if (!pindex || !credits_chainActive.Contains(pindex))
        return 0;

    // Make sure the merkle branch connects to this block
    if (!fMerkleVerified)
    {
        if (Credits_CBlock::CheckMerkleBranch(GetHash(), vMerkleBranch, nIndex) != pindex->hashMerkleRoot)
            return 0;
        fMerkleVerified = true;
    }

    pindexRet = pindex;
    return credits_chainActive.Height() - pindex->nHeight + 1;
}

int Credits_CMerkleTx::GetDepthInMainChain(Credits_CBlockIndex* &pindexRet) const
{
    AssertLockHeld(cs_main);
    int nResult = GetDepthInMainChainINTERNAL(pindexRet);
    if (nResult == 0 && !credits_mempool.exists(GetHash()))
        return -1; // Not in chain, not in mempool

    return nResult;
}

int Credits_CMerkleTx::GetBlocksToMaturity() const
{
    if (!IsCoinBase())
        return 0;
    return max(0, (BITCREDIT_COINBASE_MATURITY+1) - GetDepthInMainChain());
}

int Credits_CMerkleTx::GetFirstDepositOutBlocksToMaturity() const
{
    if (!IsDeposit())
        return 0;
    return max(0, (Credits_Params().DepositLockDepth()+1) - GetDepthInMainChain());
}
int Credits_CMerkleTx::GetSecondDepositOutBlocksToMaturity() const
{
    if (!IsDeposit())
        return 0;
    return max(0, (BITCREDIT_COINBASE_MATURITY+1) - GetDepthInMainChain());
}


bool Credits_CMerkleTx::AcceptToMemoryPool(bool fLimitFree)
{
    CValidationState state;
    return ::Bitcredit_AcceptToMemoryPool(credits_mempool, state, *this, fLimitFree, NULL);
}


// Return transaction in tx, and if it was found inside a block, its hash is placed in hashBlock
bool Bitcredit_GetTransaction(const uint256 &hash, Credits_CTransaction &txOut, uint256 &hashBlock, bool fAllowSlow)
{
    Credits_CBlockIndex *pindexSlow = NULL;
    {
        LOCK(cs_main);
        {
            if (credits_mempool.lookup(hash, txOut))
            {
                return true;
            }
        }

        if (bitcredit_fTxIndex) {
            CDiskTxPos postx;
            if (bitcredit_pblocktree->ReadTxIndex(hash, postx)) {
                CAutoFile file(Credits_OpenBlockFile(postx, true), SER_DISK, CREDITS_CLIENT_VERSION);
                Credits_CBlockHeader header;
                try {
                    file >> header;
                    fseek(file, postx.nTxOffset, SEEK_CUR);
                    file >> txOut;
                } catch (std::exception &e) {
                    return error("Credits: Bitcredit_GetTransaction: %s : Deserialize or I/O error - %s", __func__, e.what());
                }
                hashBlock = header.GetHash();
                if (txOut.GetHash() != hash)
                    return error("Credits: Bitcredit_GetTransaction: %s : txid mismatch", __func__);
                return true;
            }
        }

        if (fAllowSlow) { // use coin database to locate block that contains transaction, and scan it
            int nHeight = -1;
            {
                Credits_CCoinsViewCache &view = *credits_pcoinsTip;
                Credits_CCoins coins;
                if (view.Credits_GetCoins(hash, coins))
                    nHeight = coins.nHeight;
            }
            if (nHeight > 0)
                pindexSlow = credits_chainActive[nHeight];
        }
    }

    if (pindexSlow) {
        Credits_CBlock block;
        if (Credits_ReadBlockFromDisk(block, pindexSlow)) {
            BOOST_FOREACH(const Credits_CTransaction &tx, block.vtx) {
                if (tx.GetHash() == hash) {
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

bool Bitcredit_WriteBlockToDisk(Credits_CBlock& block, CDiskBlockPos& pos)
{
    // Open history file to append
    CAutoFile fileout = CAutoFile(Credits_OpenBlockFile(pos), SER_DISK, CREDITS_CLIENT_VERSION);
    if (!fileout)
        return error("Credits: WriteBlockToDisk : OpenBlockFile failed");

    // Write index header
    unsigned int nSize = fileout.GetSerializeSize(block);
    fileout << FLATDATA(Credits_Params().MessageStart()) << nSize;

    // Write block
    long fileOutPos = ftell(fileout);
    if (fileOutPos < 0)
        return error("Credits: WriteBlockToDisk : ftell failed");
    pos.nPos = (unsigned int)fileOutPos;
    fileout << block;

    // Flush stdio buffers and commit to disk before returning
    fflush(fileout);
    if (!Bitcredit_IsInitialBlockDownload())
        FileCommit(fileout);

    return true;
}

bool Credits_ReadBlockFromDisk(Credits_CBlock& block, const CDiskBlockPos& pos)
{
    block.SetNull();

    // Open history file to read
    CAutoFile filein = CAutoFile(Credits_OpenBlockFile(pos, true), SER_DISK, CREDITS_CLIENT_VERSION);
    if (!filein)
        return error("Credits: ReadBlockFromDisk : OpenBlockFile failed");

    // Read block
    try {
        filein >> block;
    }
    catch (std::exception &e) {
        return error("Credits_ReadBlockFromDisk: %s : Deserialize or I/O error - %s", __func__, e.what());
    }

    // Check the header
    if (!Bitcredit_CheckProofOfWork(block.GetHash(), block.nBits, block.nTotalDepositBase, block.nDepositAmount))
        return error("Credits: ReadBlockFromDisk : Errors in block header");

    return true;
}

bool Credits_ReadBlockFromDisk(Credits_CBlock& block, const Credits_CBlockIndex* pindex)
{
    if (!Credits_ReadBlockFromDisk(block, pindex->GetBlockPos()))
        return false;
    if (block.GetHash() != pindex->GetBlockHash())
        return error("Credits: ReadBlockFromDisk(CBlock&, CBlockIndex*) : GetHash() doesn't match index");
    return true;
}

bool Credits_WriteOrphanToDisk(Credits_CBlock& pblock, CNode* pfrom) {
	const uint256 hash = pblock.GetHash();
	const std::string hashHex = hash.GetHex();
	const std::string lastTwo = hashHex.substr(hashHex.size() - 2);

	try {
		//Open block file (and create if necessary)
		FILE* blockFile = OpenTmpDiskFile("credits_orphans", lastTwo.c_str(), hashHex.c_str(), false);
		if (!blockFile)
			return error("Credits_WriteOrphanToDisk : OpenBlockFile failed");

		// Open history file to append
		CAutoFile fileout = CAutoFile(blockFile, SER_DISK, pfrom->GetNetParams()->ClientVersion());
		if (!fileout)
			return error("Bitcoin: Bitcoin_CBlockCompressed::WriteToDisk : OpenFile failed");

		//Write hashes of interest
		fileout << hash;
		fileout << pblock.hashPrevBlock;
		fileout << pblock.hashLinkedBitcoinBlock;

		// write block
		fileout << pblock;

		// Flush stdio buffers and commit to disk before returning
		fflush(fileout);
		FileCommit(fileout);
	}
	catch (std::exception &e) {
		return error("Credits_WriteOrphanToDisk: %s : Deserialize or I/O error - %s", __func__, e.what());
	}
    return true;
}

bool Credits_ReadOrphanFromDisk(const uint256 &hash, Credits_CBlock& block) {
	const std::string hashHex = hash.GetHex();
	const std::string lastTwo = hashHex.substr(hashHex.size() - 2);

    block.SetNull();

    try {
		// Open history file to read
		CAutoFile filein = CAutoFile(OpenTmpDiskFile("credits_orphans", lastTwo.c_str(), hashHex.c_str(), true), SER_DISK, CREDITS_CLIENT_VERSION);
		if (!filein)
			return error("Credits: Credits_ReadOrphanFromDisk : OpenBlockFile failed");

		//Read hashes of interest
		uint256 tmpHash;
		filein >> tmpHash; // hash
		uint256 tmpPrevHash;
		filein >> tmpPrevHash; //pblock->hashPrevBlock;
		uint256 hashLinkedBitcoinBlock;
		filein >> hashLinkedBitcoinBlock;

		// Read block
        filein >> block;
    }
    catch (std::exception &e) {
        return error("Credits_ReadOrphanFromDisk: %s : Deserialize or I/O error - %s", __func__, e.what());
    }

    // Check the header
    if (!Bitcredit_CheckProofOfWork(block.GetHash(), block.nBits, block.nTotalDepositBase, block.nDepositAmount))
        return error("Credits: Credits_ReadOrphanFromDisk : Errors in block header");

    return true;
}

bool Credits_IndexOrphansFromDisk() {
    const int64_t nStart = GetTimeMillis();

    credits_orphanIndex.SetNull();

    unsigned int nOrphansLoaded = 0;
	//Load index for all orphans
	const boost::filesystem::path dirPath = GetTmpDataDir() / "credits_orphans";
	boost::filesystem::directory_iterator it(dirPath), eod;
	BOOST_FOREACH(const boost::filesystem::path& subDirPath, std::make_pair(it, eod)) {
	    if(boost::filesystem::is_directory(subDirPath)) {
	    	boost::filesystem::directory_iterator it2(subDirPath), eod2;
        	BOOST_FOREACH(const boost::filesystem::path& orphanedFilePath, std::make_pair(it2, eod2)) {
        	    if(boost::filesystem::is_regular_file(orphanedFilePath)) {
        	        // Open orphan file to read
        	        CAutoFile filein = CAutoFile(fopen(orphanedFilePath.string().c_str(), "rb+"), SER_DISK, CREDITS_CLIENT_VERSION);
        	        if (!filein)
        	            return error("Credits_IndexOrphansFromDisk : OpenBlockFile failed");

        	        try {
        	    		//Read hashes of interest
        	    		uint256 hash;
        	    		filein >> hash;
        	    		uint256 hashPrevBlock;
        	    		filein >> hashPrevBlock;
        	    		uint256 hashLinkedBitcoinBlock;
        	    		filein >> hashLinkedBitcoinBlock;

        	            COrphanBlock* pblock2 = new COrphanBlock();
        	            pblock2->hashBlock = hash;
        	            pblock2->hashPrev = hashPrevBlock;
        	            pblock2->hashLinkedBitcoinBlock = hashLinkedBitcoinBlock;
        	            pblock2->fStoredInMemory = false;

        	            credits_orphanIndex.mapOrphanBlocks.insert(make_pair(hash, pblock2));
        	            credits_orphanIndex.mapOrphanBlocksByPrev.insert(make_pair(pblock2->hashPrev, pblock2));
        	            credits_orphanIndex.mapOrphanBlocksByLinkedBitcoinBlock.insert(make_pair(pblock2->hashLinkedBitcoinBlock, pblock2));

        	            nOrphansLoaded++;
        	        }
        	        catch (std::exception &e) {
        	            return error("Credits_IndexOrphansFromDisk: %s : Deserialize or I/O error - %s", __func__, e.what());
        	        }
        	    }
        	}
	    }
	}

    LogPrintf("%d orphaned Credits blocks loaded from disk in %15dms\n", nOrphansLoaded, GetTimeMillis() - nStart);

	return true;
}

uint64_t Bitcredit_GetRequiredDeposit(const uint64_t nTotalDepositBase) {
	return nTotalDepositBase / (2 * Credits_Params().DepositLockDepth());
}

uint256 Bitcredit_ReduceByReqDepositLevel(const uint256 nValue, const uint64_t nDepositAmount, const uint64_t nTotalDepositBase) {
	const uint64_t reqDeposit = Bitcredit_GetRequiredDeposit(nTotalDepositBase);
	const uint256 linear = ReduceByFraction(nValue, nDepositAmount, reqDeposit);
	uint256 result = linear;

	//Exponential enforcement of deposits is not applied unitl approx block 140000
	if(nTotalDepositBase >= BITCREDIT_EXPONENTIAL_DEPOSIT_ENFORCE_AT) {
		const uint256 exponential = ReduceByFraction(linear, nDepositAmount, reqDeposit);
		result = exponential;
	}
	return result;
}

uint64_t Bitcredit_GetMaxBlockSubsidy(const uint64_t nTotalMonetaryBase) {
	uint64_t nSubsidy = 0;

	//Loop will find correct position in vector of subsidyLevels by
	//comparing the total monetary base of each level to the nTotalMonetaryBase of the previous block
	const vector<SubsidyLevel> subsidyLevels = Credits_Params().getSubsidyLevels();
	SubsidyLevel current = subsidyLevels.at(0);
	for (unsigned int i = 1; i < subsidyLevels.size(); i++) {
		const SubsidyLevel nextSubsidyLevel = subsidyLevels.at(i);
		if (nTotalMonetaryBase >= nextSubsidyLevel.nTotalMonetaryBase) {
			current = nextSubsidyLevel;
		} else {
			break;
		}
	}

	nSubsidy = current.nSubsidyUpdateTo;

    return nSubsidy;
}

uint64_t Bitcredit_GetAllowedBlockSubsidy(const uint64_t nTotalMonetaryBase, uint64_t nDepositAmount, const uint64_t nTotalDepositBase) {
	uint64_t nSubsidy = Bitcredit_GetMaxBlockSubsidy(nTotalMonetaryBase);
	if(nTotalDepositBase > BITCREDIT_ENFORCE_SUBSIDY_REDUCTION_AFTER) {
		nSubsidy = Bitcredit_ReduceByReqDepositLevel(uint256(nSubsidy), nDepositAmount, nTotalDepositBase).GetLow64();
	}
	return nSubsidy;
}

static const int64_t bitcredit_nTargetTimespan_V1 = 14 * 24 * 60 * 60; // two weeks
static const int64_t bitcredit_nTargetSpacing_V1 = 10 * 60;
static const int64_t bitcredit_nInterval_V1 = bitcredit_nTargetTimespan_V1 / bitcredit_nTargetSpacing_V1;
static const int64_t bitcredit_nMaxAdjustment_V1 = 4;

static const int64_t bitcredit_nTargetTimespan_V2 = bitcredit_nTargetTimespan_V1 / 8; // approximately 1.75 days
static const int64_t bitcredit_nTargetSpacing_V2 = 10 * 60;
static const int64_t bitcredit_nInterval_V2 = bitcredit_nTargetTimespan_V2 / bitcredit_nTargetSpacing_V2;
static const int64_t bitcredit_nMaxAdjustment_V2 = 2; // Combined with nTargetTimespan this will result in 64 times faster adjustments

//
// minimum amount of work that could possibly be required nTime after
// minimum work required was nBase
//
unsigned int Bitcredit_Internal_ComputeMinWork(unsigned int nBase, int64_t nTime, const int64_t &nTargetTimespan, const int64_t &nTargetSpacing, const int64_t &nMaxAdjustment)
{
    const uint256 &bnLimit = Credits_Params().ProofOfWorkLimit();
    // Testnet has min-difficulty blocks
    // after nTargetSpacing*2 time between blocks:
    if (Bitcredit_TestNet() && nTime > nTargetSpacing*2)
        return bnLimit.GetCompact();

    uint256 bnResult;
    bnResult.SetCompact(nBase);
    while (nTime > 0 && bnResult < bnLimit)
    {
        // Maximum nMaxAdjustment% adjustment...
        bnResult *= nMaxAdjustment;
        // ... in best-case exactly nMaxAdjustment-times-normal target time
        nTime -= nTargetTimespan*nMaxAdjustment;
    }
    if (bnResult > bnLimit)
        bnResult = bnLimit;
    return bnResult.GetCompact();
}

unsigned int Bitcredit_ComputeMinWork(int nHeight, unsigned int nBase, int64_t nTime) {
	if(nHeight < CREDITS_DIFF_ALGO_V2_AT_CHAIN_HEIGHT) {
		return Bitcredit_Internal_ComputeMinWork(nBase, nTime, bitcredit_nTargetTimespan_V1, bitcredit_nTargetSpacing_V1, bitcredit_nMaxAdjustment_V1);
	} else {
		return Bitcredit_Internal_ComputeMinWork(nBase, nTime, bitcredit_nTargetTimespan_V2, bitcredit_nTargetSpacing_V2, bitcredit_nMaxAdjustment_V2);
	}
}

unsigned int Bitcredit_Internal_GetNextWorkRequired(const Credits_CBlockIndex* pindexLast, const Credits_CBlockHeader *pblock, const int64_t &nTargetTimespan, const int64_t &nTargetSpacing, const int64_t &nInterval, const int64_t &nMaxAdjustment)
{
    unsigned int nProofOfWorkLimit = Credits_Params().ProofOfWorkLimit().GetCompact();

    // Genesis block
    if (pindexLast == NULL)
        return nProofOfWorkLimit;

    // Only change once per interval
    if ((pindexLast->nHeight+1) % nInterval != 0)
    {
        if (Bitcredit_TestNet())
        {
            // Special difficulty rule for testnet:
            // If the new block's timestamp is more than 2* 10 minutes
            // then allow mining of a min-difficulty block.
            if (pblock->nTime > pindexLast->nTime + nTargetSpacing*2)
                return nProofOfWorkLimit;
            else
            {
                // Return the last non-special-min-difficulty-rules-block
                const CBlockIndexBase* pindex = pindexLast;
                while (pindex->pprev && pindex->nHeight % nInterval != 0 && pindex->nBits == nProofOfWorkLimit)
                    pindex = pindex->pprev;
                return pindex->nBits;
            }
        }
        return pindexLast->nBits;
    }

    // Go back by what we want to be x days worth of blocks
    const CBlockIndexBase* pindexFirst = pindexLast;
    for (int i = 0; pindexFirst && i < nInterval-1; i++)
        pindexFirst = pindexFirst->pprev;
    assert(pindexFirst);

    // Limit adjustment step
    int64_t nActualTimespan = pindexLast->GetBlockTime() - pindexFirst->GetBlockTime();
    LogPrintf("Credits:   nActualTimespan = %d  before bounds\n", nActualTimespan);
    if (nActualTimespan < nTargetTimespan/nMaxAdjustment)
        nActualTimespan = nTargetTimespan/nMaxAdjustment;
    if (nActualTimespan > nTargetTimespan*nMaxAdjustment)
        nActualTimespan = nTargetTimespan*nMaxAdjustment;

    // Retarget
    uint256 bnNew;
    uint256 bnOld;
    bnNew.SetCompact(pindexLast->nBits);
    bnOld = bnNew;
    bnNew *= nActualTimespan;
    bnNew /= nTargetTimespan;

    if (bnNew > Credits_Params().ProofOfWorkLimit())
        bnNew = Credits_Params().ProofOfWorkLimit();

    /// debug print
    LogPrintf("Credits: GetNextWorkRequired RETARGET\n");
    LogPrintf("Credits: nTargetTimespan = %d    nActualTimespan = %d\n", nTargetTimespan, nActualTimespan);
    LogPrintf("Credits: nMaxAdjustment = %d\n", nMaxAdjustment);
    LogPrintf("Credits: Before: %08x  %s\n", pindexLast->nBits, bnOld.ToString());
    LogPrintf("Credits: After:  %08x  %s\n", bnNew.GetCompact(), bnNew.ToString());

    return bnNew.GetCompact();
}

unsigned int Bitcredit_GetNextWorkRequired(const Credits_CBlockIndex* pindexLast, const Credits_CBlockHeader *pblock) {
	if(pindexLast->nHeight < CREDITS_DIFF_ALGO_V2_AT_CHAIN_HEIGHT) {
		return Bitcredit_Internal_GetNextWorkRequired(pindexLast, pblock, bitcredit_nTargetTimespan_V1, bitcredit_nTargetSpacing_V1, bitcredit_nInterval_V1, bitcredit_nMaxAdjustment_V1);
	} else {
		return Bitcredit_Internal_GetNextWorkRequired(pindexLast, pblock, bitcredit_nTargetTimespan_V2, bitcredit_nTargetSpacing_V2, bitcredit_nInterval_V2, bitcredit_nMaxAdjustment_V2);
	}
}

bool Bitcredit_CheckProofOfWork(uint256 hash, unsigned int nBits, uint64_t nTotalDepositBase, uint64_t nDepositAmount)
{
    bool fNegative;
    bool fOverflow;
    uint256 bnTarget;
    bnTarget.SetCompact(nBits, &fNegative, &fOverflow);

    // Check range
    if (fNegative || bnTarget == 0 || fOverflow || bnTarget > Credits_Params().ProofOfWorkLimit())
        return error("Credits: CheckProofOfWork() : nBits below minimum work");

    //Adjust the target depending on how much miner has added as deposit
    bnTarget = Bitcredit_ReduceByReqDepositLevel(bnTarget, nDepositAmount, nTotalDepositBase);

    // Check proof of work matches claimed amount
    if (hash > bnTarget)
        return error(strprintf("CheckProofOfWork() : hash doesn't match nBits. nBits is %u, hash is %s", bnTarget.GetCompact(), hash.GetHex()));

    return true;
}

bool Bitcredit_IsInitialBlockDownload()
{
    LOCK(cs_main);
    if (credits_mainState.ImportingOrReindexing() || credits_chainActive.Height() < Checkpoints::Bitcredit_GetTotalBlocksEstimate())
        return true;
    static int64_t nLastUpdate;
    static CBlockIndexBase* pindexLastBest;
    if (credits_chainActive.Tip() != pindexLastBest)
    {
        pindexLastBest = credits_chainActive.Tip();
        nLastUpdate = GetTime();
    }
    return (GetTime() - nLastUpdate < 10 &&
    		credits_chainActive.Tip()->GetBlockTime() < GetTime() - 24 * 60 * 60);
}

bool bitcredit_fLargeWorkForkFound = false;
bool bitcredit_fLargeWorkInvalidChainFound = false;
Credits_CBlockIndex *bitcredit_pindexBestForkTip = NULL, *bitcredit_pindexBestForkBase = NULL;

void Bitcredit_CheckForkWarningConditions()
{
    AssertLockHeld(cs_main);
    // Before we get past initial download, we cannot reliably alert about forks
    // (we assume we don't get stuck on a fork before the last checkpoint)
    if (Bitcredit_IsInitialBlockDownload())
        return;

    // If our best fork is no longer within 72 blocks (+/- 12 hours if no one mines it)
    // of our head, drop it
    if (bitcredit_pindexBestForkTip && credits_chainActive.Height() - bitcredit_pindexBestForkTip->nHeight >= 72)
        bitcredit_pindexBestForkTip = NULL;

    if (bitcredit_pindexBestForkTip || (bitcredit_pindexBestInvalid && bitcredit_pindexBestInvalid->nChainWork > credits_chainActive.Tip()->nChainWork + (credits_chainActive.Tip()->GetBlockWork() * 6)))
    {
        if (!bitcredit_fLargeWorkForkFound)
        {
            std::string strCmd = GetArg("-alertnotify", "");
            if (!strCmd.empty())
            {
                std::string warning = std::string("'Warning: Large-work fork detected, forking after block ") +
                                      bitcredit_pindexBestForkBase->phashBlock->ToString() + std::string("'");
                boost::replace_all(strCmd, "%s", warning);
                boost::thread t(runCommand, strCmd); // thread runs free
            }
        }
        if (bitcredit_pindexBestForkTip)
        {
            LogPrintf("Credits: CheckForkWarningConditions: Warning: Large valid fork found\n  forking the chain at height %d (%s)\n  lasting to height %d (%s).\nChain state database corruption likely.\n",
                   bitcredit_pindexBestForkBase->nHeight, bitcredit_pindexBestForkBase->phashBlock->ToString(),
                   bitcredit_pindexBestForkTip->nHeight, bitcredit_pindexBestForkTip->phashBlock->ToString());
            bitcredit_fLargeWorkForkFound = true;
        }
        else
        {
            LogPrintf("Credits: CheckForkWarningConditions: Warning: Found invalid chain at least ~6 blocks longer than our best chain.\nChain state database corruption likely.\n");
            bitcredit_fLargeWorkInvalidChainFound = true;
        }
    }
    else
    {
        bitcredit_fLargeWorkForkFound = false;
        bitcredit_fLargeWorkInvalidChainFound = false;
    }
}

void Bitcredit_CheckForkWarningConditionsOnNewFork(Credits_CBlockIndex* pindexNewForkTip)
{
    AssertLockHeld(cs_main);
    // If we are on a fork that is sufficiently large, set a warning flag
    Credits_CBlockIndex* pfork = pindexNewForkTip;
    Credits_CBlockIndex* plonger = (Credits_CBlockIndex*)credits_chainActive.Tip();
    while (pfork && pfork != plonger)
    {
        while (plonger && plonger->nHeight > pfork->nHeight)
            plonger = (Credits_CBlockIndex*)plonger->pprev;
        if (pfork == plonger)
            break;
        pfork = (Credits_CBlockIndex*)pfork->pprev;
    }

    // We define a condition which we should warn the user about as a fork of at least 7 blocks
    // who's tip is within 72 blocks (+/- 12 hours if no one mines it) of ours
    // We use 7 blocks rather arbitrarily as it represents just under 10% of sustained network
    // hash rate operating on the fork.
    // or a chain that is entirely longer than ours and invalid (note that this should be detected by both)
    // We define it this way because it allows us to only store the highest fork tip (+ base) which meets
    // the 7-block condition and from this always have the most-likely-to-cause-warning fork
    if (pfork && (!bitcredit_pindexBestForkTip || (bitcredit_pindexBestForkTip && pindexNewForkTip->nHeight > bitcredit_pindexBestForkTip->nHeight)) &&
            pindexNewForkTip->nChainWork - pfork->nChainWork > (pfork->GetBlockWork() * 7) &&
            credits_chainActive.Height() - pindexNewForkTip->nHeight < 72)
    {
        bitcredit_pindexBestForkTip = pindexNewForkTip;
        bitcredit_pindexBestForkBase = pfork;
    }

    Bitcredit_CheckForkWarningConditions();
}

// Requires cs_main.
void Bitcredit_Misbehaving(NodeId pnode, int howmuch)
{
    if (howmuch == 0)
        return;

    CNodeState *state = Bitcredit_State(pnode);
    if (state == NULL)
        return;

    state->nMisbehavior += howmuch;
    if (state->nMisbehavior >= GetArg("-banscore", 100))
    {
        LogPrintf("Credits: Misbehaving: %s (%d -> %d) BAN THRESHOLD EXCEEDED\n", state->name, state->nMisbehavior-howmuch, state->nMisbehavior);
        state->fShouldBan = true;
    } else
        LogPrintf("Credits: Misbehaving: %s (%d -> %d)\n", state->name, state->nMisbehavior-howmuch, state->nMisbehavior);
}

void static Bitcredit_InvalidChainFound(Credits_CBlockIndex* pindexNew)
{
    if (!bitcredit_pindexBestInvalid || pindexNew->nChainWork > bitcredit_pindexBestInvalid->nChainWork)
    {
        bitcredit_pindexBestInvalid = pindexNew;
        uiInterface.NotifyBlocksChanged();
    }
    LogPrintf("Credits: InvalidChainFound: invalid block=%s  height=%d  log2_work=%.8g  date=%s\n",
      pindexNew->GetBlockHash().ToString(), pindexNew->nHeight,
      log(pindexNew->nChainWork.getdouble())/log(2.0), DateTimeStrFormat("%Y-%m-%d %H:%M:%S",
      pindexNew->GetBlockTime()));
    LogPrintf("Credits: InvalidChainFound:  current best=%s  height=%d  log2_work=%.8g  date=%s\n",
      credits_chainActive.Tip()->GetBlockHash().ToString(), credits_chainActive.Height(), log(credits_chainActive.Tip()->nChainWork.getdouble())/log(2.0),
      DateTimeStrFormat("%Y-%m-%d %H:%M:%S", credits_chainActive.Tip()->GetBlockTime()));
    Bitcredit_CheckForkWarningConditions();
}

void static Bitcredit_InvalidBlockFound(Credits_CBlockIndex *pindex, const CValidationState &state) {
    int nDoS = 0;
    if (state.IsInvalid(nDoS)) {
        std::map<uint256, NodeId>::iterator it = bitcredit_mapBlockSource.find(pindex->GetBlockHash());
        if (it != bitcredit_mapBlockSource.end() && Bitcredit_State(it->second)) {
            CBlockReject reject = {state.GetRejectCode(), state.GetRejectReason(), pindex->GetBlockHash()};
            Bitcredit_State(it->second)->rejects.push_back(reject);
            if (nDoS > 0)
                Bitcredit_Misbehaving(it->second, nDoS);
        }
    }
    if (!state.CorruptionPossible()) {
        pindex->nStatus |= BLOCK_FAILED_VALID;
        bitcredit_pblocktree->WriteBlockIndex(Credits_CDiskBlockIndex(pindex));
        bitcredit_setBlockIndexValid.erase(pindex);
        Bitcredit_InvalidChainFound(pindex);
    }
}

void Bitcredit_UpdateTime(Credits_CBlockHeader& block, const Credits_CBlockIndex* pindexPrev)
{
    block.nTime = max(pindexPrev->GetMedianTimePast()+1, GetAdjustedTime());

    // Updating time can change work required on testnet:
    if (Bitcredit_TestNet())
        block.nBits = Bitcredit_GetNextWorkRequired(pindexPrev, &block);
}











void Bitcredit_UpdateCoins(const Credits_CTransaction& tx, CValidationState &state, Credits_CCoinsViewCache &bitcredit_inputs, Credits_CTxUndo &txundo, int nHeight, const uint256 &txhash)
{
    bool ret;
    // mark inputs spent
    if (!tx.IsCoinBase()) {
    	if(tx.IsClaim()) {
            BOOST_FOREACH(const Credits_CTxIn &txin, tx.vin) {
            	Credits_CTxInUndo undo;
    			Claim_CCoins &coins = bitcredit_inputs.Claim_GetCoins(txin.prevout.hash);
    			ret = coins.SpendByClaiming(txin.prevout, undo);
    			assert(ret);
    			txundo.vprevout.push_back(undo);
            }
    	} else {
            BOOST_FOREACH(const Credits_CTxIn &txin, tx.vin) {
            	Credits_CTxInUndo undo;
				Credits_CCoins &coins = bitcredit_inputs.Credits_GetCoins(txin.prevout.hash);
				ret = coins.Spend(txin.prevout, undo);
				assert(ret);
				txundo.vprevout.push_back(undo);
            }
    	}
    }

    // add outputs
    ret = bitcredit_inputs.Credits_SetCoins(txhash, Credits_CCoins(tx, nHeight));
    assert(ret);
}

bool Bitcredit_CScriptCheck::operator()() const {
    const CScript &scriptSig = ptxTo->vin[nIn].scriptSig;
    if (!Bitcredit_VerifyScript(scriptSig, scriptPubKey, *ptxTo, nIn, nFlags, nHashType))
        return error("Credits: CScriptCheck() : %s VerifySignature failed", ptxTo->GetHash().ToString());
    return true;
}

bool Bitcredit_VerifySignature(const Credits_CCoins& txFrom, const Credits_CTransaction& txTo, unsigned int nIn, unsigned int flags, int nHashType)
{
    return Bitcredit_CScriptCheck(txFrom, txTo, nIn, flags, nHashType)();
}

bool Bitcredit_FindBestBlockAndCheckClaims(Credits_CCoinsViewCache &claim_view, const int64_t nClaimedCoins)
{
	//Verify that clams are in bounds
	const map<uint256, Bitcoin_CBlockIndex*>::iterator mi = bitcoin_mapBlockIndex.find(claim_view.Claim_GetBestBlock());
	if (mi == bitcoin_mapBlockIndex.end()) {
		return error("Credits: Bitcoin block to check for claiming %s not found in bitcoin blockchain!\n\n", claim_view.Claim_GetBestBlock().GetHex());
	}
	Bitcoin_CBlockIndex* pBestBlock = (*mi).second;
	if(!Bitcredit_CheckClaimsAreInBounds(claim_view, nClaimedCoins, pBestBlock->nHeight)) {
		return error("Credits: Claimed number of coins too high: %d!\n\n", nClaimedCoins);
	}
	return true;
}

bool Bitcredit_CheckClaimsAreInBounds(Credits_CCoinsViewCache &claim_inputs, const int64_t nClaimedCoins, const int nBitcoinBlockHeight)
{
	const int64_t nTotalClaimedCoins = claim_inputs.Claim_GetTotalClaimedCoins();
	//Max allowed claim level is 90% of the present bitcoin monetary base
	const int64_t limit = ReduceByFraction(Bitcoin_GetTotalMonetaryBase(nBitcoinBlockHeight), 9, 10);

    if(nTotalClaimedCoins + nClaimedCoins > limit) {
    	return error("Credits: Too high claim attempt, allowed: %d, already claimed: %s, claiming attempt: %d", limit, nTotalClaimedCoins, nClaimedCoins);
    }
    if(nTotalClaimedCoins + nClaimedCoins > BITCREDIT_MAX_BITCOIN_CLAIM) {
    	return error("Credits: The claim limit has been reached, already claimed: %s, claiming attempt: %d", nTotalClaimedCoins, nClaimedCoins);
    }
    return true;
}

bool Credits_CheckInputs(const Credits_CTransaction& tx, CValidationState &state, Credits_CCoinsViewCache &credits_inputs, int64_t &nTotalClaimedCoins, bool fScriptChecks, unsigned int flags, std::vector<Bitcredit_CScriptCheck> *pvChecks)
{
    if (!tx.IsCoinBase())
    {
        if (pvChecks)
            pvChecks->reserve(tx.vin.size());

        // This doesn't trigger the DoS code on purpose; if it did, it would make it easier
        // for an attacker to attempt to split the network.
    	if(tx.IsClaim()) {
			if (!credits_inputs.Claim_HaveInputs(tx))
				return state.Invalid(error("Credits: CheckInputs() : %s external bitcoin inputs unavailable", tx.GetHash().ToString()));
    	} else {
			if (!credits_inputs.Credits_HaveInputs(tx))
				return state.Invalid(error("Credits: CheckInputs() : %s credits inputs unavailable", tx.GetHash().ToString()));
    	}


        // While checking, GetBestBlock() refers to the parent block.
        // This is also true for mempool checks.
        Credits_CBlockIndex *bitcredit_pindexPrev = credits_mapBlockIndex.find(credits_inputs.Credits_GetBestBlock())->second;
        int bitcredit_nSpendHeight = bitcredit_pindexPrev->nHeight + 1;

		//The reason for that is that Credits_CheckInputs uses it for verification before it has a chance to be initalized
        //Using claim_inputs here would cause some problem with updating the best block index here. Not sure why.
		uint256 bitcoinBestBlock = credits_inputs.Claim_GetBestBlock() ;
		if(bitcoinBestBlock == uint256(0)) {
			bitcoinBestBlock = Bitcoin_Params().GenesisBlock().GetHash();
		}
        Bitcoin_CBlockIndex *bitcoin_pindexPrev = bitcoin_mapBlockIndex.find(bitcoinBestBlock)->second;
        int bitcoin_nSpendHeight = bitcoin_pindexPrev->nHeight + 1;

		int64_t nTxValueOut = tx.GetValueOut();
        int64_t nValueIn = 0;
        int64_t nFees = 0;
    	if(tx.IsClaim()) {
			for (unsigned int i = 0; i < tx.vin.size(); i++) {
            	const COutPoint &prevout = tx.vin[i].prevout;

				const Claim_CCoins &coins = credits_inputs.Claim_GetCoins(prevout.hash);

				if (coins.IsCoinBase()) {
					if (bitcoin_nSpendHeight - coins.nHeight < BITCOIN_COINBASE_MATURITY)
						return state.Invalid(
							error("Credits: CheckInputs() : tried to spend external bitcoin coinbase at depth %d", bitcoin_nSpendHeight - coins.nHeight),
							BITCREDIT_REJECT_INVALID, "bad-txns-premature-spend-of-coinbase");
				}

				if(coins.vout.size() > prevout.n) {
					// Check for negative or overflow input values
					nValueIn += coins.vout[prevout.n].nValueClaimable;
					if (!Credits_MoneyRange(coins.vout[prevout.n].nValueClaimable) || !Credits_MoneyRange(nValueIn))
						return state.DoS(100, error("Credits: CheckInputs() : external bitcoin txin values out of range"),
										 BITCREDIT_REJECT_INVALID, "bad-txns-inputvalues-outofrange");

				}
            }
			nTotalClaimedCoins += nValueIn;
    	} else {
            for (unsigned int i = 0; i < tx.vin.size(); i++) {
            	const COutPoint &prevout = tx.vin[i].prevout;

				const Credits_CCoins &coins = credits_inputs.Credits_GetCoins(prevout.hash);

				// If prev is coinbase, check that it's matured
				//An exception is made where the deposit can spend the coinbase IF the coinbase
				//and deposit are within the same block. Without this rule there would be no initial
				//deposit inputs to reference. When the monetary base grows, mining on coinbases
				//will probably phase out.
				//Note that the deposit in turn will be locked for 15000 blocks.
				if (coins.IsCoinBase()) {
					if(!tx.IsDeposit() || bitcredit_nSpendHeight != coins.nHeight) {
						if (bitcredit_nSpendHeight - coins.nHeight < BITCREDIT_COINBASE_MATURITY)
							return state.Invalid(
								error("Credits: CheckInputs() : tried to spend coinbase at depth %d", bitcredit_nSpendHeight - coins.nHeight),
								BITCREDIT_REJECT_INVALID, "bad-txns-premature-spend-of-coinbase");
					}
				}

				if(coins.IsDeposit()) {
					if(prevout.n == 0) {
						if (bitcredit_nSpendHeight - coins.nHeight < Credits_Params().DepositLockDepth())
							return state.Invalid(
								error("Credits: CheckInputs() : tried to spend deposit at depth %d, depth %d is required", bitcredit_nSpendHeight - coins.nHeight, Credits_Params().DepositLockDepth()),
								BITCREDIT_REJECT_INVALID, "bad-txns-premature-spend-of-deposit");
					} else {
						//A deposit change cannot be spent until coinbase maturity UNLESS the spending tx is a deposit itself
						//This is to make it possible to chain deposits, with each following deposit directly spending a previous deposit change.
						if(!tx.IsDeposit())
							if (bitcredit_nSpendHeight - coins.nHeight < BITCREDIT_COINBASE_MATURITY)
								return state.Invalid(
									error("Credits: CheckInputs() : tried to spend deposit change at depth %d, depth %d is required", bitcredit_nSpendHeight - coins.nHeight, BITCREDIT_COINBASE_MATURITY),
									BITCREDIT_REJECT_INVALID, "bad-txns-premature-spend-of-deposit-change");
					}
				}

				// Check for negative or overflow input values
				nValueIn += coins.vout[prevout.n].nValue;
				if (!Credits_MoneyRange(coins.vout[prevout.n].nValue) || !Credits_MoneyRange(nValueIn))
					return state.DoS(100, error("Credits: CheckInputs() : txin values out of range"),
									 BITCREDIT_REJECT_INVALID, "bad-txns-inputvalues-outofrange");
            }

			if(tx.IsDeposit() && nTxValueOut != nValueIn) {
				return state.Invalid(
					error("Credits: CheckInputs() :  deposit tx inputs does not match outputs  %s", tx.GetHash().ToString()),
					BITCREDIT_REJECT_INVALID, "bad-deposit-tx-in-out-differs");
			}
    	}

        if (nValueIn < nTxValueOut)
            return state.DoS(100, error("Credits: CheckInputs() : %s value in < value out", tx.GetHash().ToString()),
                             BITCREDIT_REJECT_INVALID, "bad-txns-in-belowout");

        // Tally transaction fees
        int64_t nTxFee = nValueIn - nTxValueOut;
        if (nTxFee < 0)
            return state.DoS(100, error("Credits: CheckInputs() : %s nTxFee < 0", tx.GetHash().ToString()),
                             BITCREDIT_REJECT_INVALID, "bad-txns-fee-negative");
        nFees += nTxFee;
        if (!Credits_MoneyRange(nFees))
            return state.DoS(100, error("Credits: CheckInputs() : nFees out of range"),
                             BITCREDIT_REJECT_INVALID, "bad-txns-fee-outofrange");

        // The first loop above does all the inexpensive checks.
        // Only if ALL inputs pass do we perform expensive ECDSA signature checks.
        // Helps prevent CPU exhaustion attacks.

        // Skip ECDSA signature verification when connecting blocks
        // before the last block chain checkpoint. This is safe because block merkle hashes are
        // still computed and checked, and any change will be caught at the next checkpoint.
        if (fScriptChecks) {
          if(tx.IsClaim()) {
              for (unsigned int i = 0; i < tx.vin.size(); i++) {
              	const COutPoint &prevout = tx.vin[i].prevout;

				const Claim_CCoins &coins = credits_inputs.Claim_GetCoins(prevout.hash);

				// Verify signature
				Bitcredit_CScriptCheck check(coins, tx, i, flags, 0);
				if (pvChecks) {
					pvChecks->push_back(Bitcredit_CScriptCheck());
					check.swap(pvChecks->back());
				} else if (!check()) {
					if (flags & STANDARD_NOT_MANDATORY_VERIFY_FLAGS) {
						// Check whether the failure was caused by a
						// non-mandatory script verification check, such as
						// non-standard DER encodings or non-null dummy
						// arguments; if so, don't trigger DoS protection to
						// avoid splitting the network between upgraded and
						// non-upgraded nodes.
						Bitcredit_CScriptCheck check(coins, tx, i,
								flags & ~STANDARD_NOT_MANDATORY_VERIFY_FLAGS, 0);
						if (check())
							return state.Invalid(false, BITCREDIT_REJECT_NONSTANDARD, "non-mandatory-script-verify-flag");
					}
					// Failures of other flags indicate a transaction that is
					// invalid in new blocks, e.g. a invalid P2SH. We DoS ban
					// such nodes as they are not following the protocol. That
					// said during an upgrade careful thought should be taken
					// as to the correct behavior - we may want to continue
					// peering with non-upgraded nodes even after a soft-fork
					// super-majority vote has passed.
					return state.DoS(100,false, BITCREDIT_REJECT_INVALID, "mandatory-script-verify-flag-failed");
				}
              }
          } else {
              for (unsigned int i = 0; i < tx.vin.size(); i++) {
              	const COutPoint &prevout = tx.vin[i].prevout;

				const Credits_CCoins &coins = credits_inputs.Credits_GetCoins(prevout.hash);

				// Verify signature
				Bitcredit_CScriptCheck check(coins, tx, i, flags, 0);
				if (pvChecks) {
					pvChecks->push_back(Bitcredit_CScriptCheck());
					check.swap(pvChecks->back());
				} else if (!check()) {
					if (flags & STANDARD_NOT_MANDATORY_VERIFY_FLAGS) {
						// Check whether the failure was caused by a
						// non-mandatory script verification check, such as
						// non-standard DER encodings or non-null dummy
						// arguments; if so, don't trigger DoS protection to
						// avoid splitting the network between upgraded and
						// non-upgraded nodes.
						Bitcredit_CScriptCheck check(coins, tx, i,
								flags & ~STANDARD_NOT_MANDATORY_VERIFY_FLAGS, 0);
						if (check())
							return state.Invalid(false, BITCREDIT_REJECT_NONSTANDARD, "non-mandatory-script-verify-flag");
					}
					// Failures of other flags indicate a transaction that is
					// invalid in new blocks, e.g. a invalid P2SH. We DoS ban
					// such nodes as they are not following the protocol. That
					// said during an upgrade careful thought should be taken
					// as to the correct behavior - we may want to continue
					// peering with non-upgraded nodes even after a soft-fork
					// super-majority vote has passed.
					return state.DoS(100,false, BITCREDIT_REJECT_INVALID, "mandatory-script-verify-flag-failed");
				}
              }
          }
        }
    }

    return true;
}



bool Bitcredit_DisconnectBlock(Credits_CBlock& block, CValidationState& state, Credits_CBlockIndex* pindex, Credits_CCoinsViewCache& credits_view, bool updateBitcoinUndo, std::vector<pair<Bitcoin_CBlockIndex*, Bitcoin_CBlockUndoClaim> > &vBlockUndoClaims, bool* pfClean)
{
    assert(pindex->GetBlockHash() == credits_view.Credits_GetBestBlock());

    if (pfClean)
        *pfClean = false;

    bool fClean = true;

    Credits_CBlockUndo blockUndo;
    CDiskBlockPos pos = pindex->GetUndoPos();
    if (pos.IsNull())
        return error("Credits: DisconnectBlock() : no undo data available");
    if (!blockUndo.ReadFromDisk(pos, pindex->pprev->GetBlockHash(), Credits_NetParams()))
        return error("Credits: DisconnectBlock() : failure reading undo data");

    if (blockUndo.vtxundo.size() + 1 != block.vtx.size())
        return error("Credits: DisconnectBlock() : block and undo data inconsistent");

    int64_t nTotalClaimedCoinsForBlock = 0;

    // undo transactions in reverse order
    for (int i = block.vtx.size() - 1; i >= 0; i--) {
        const Credits_CTransaction &tx = block.vtx[i];
        uint256 hash = tx.GetHash();

        // Check that all outputs are available and match the outputs in the block itself
        // exactly. Note that transactions with only provably unspendable outputs won't
        // have outputs available even in the block itself, so we handle that case
        // specially with outsEmpty.
        Credits_CCoins outsEmpty;
        Credits_CCoins &outs = credits_view.Credits_HaveCoins(hash) ? credits_view.Credits_GetCoins(hash) : outsEmpty;
        outs.ClearUnspendable();

        Credits_CCoins outsBlock = Credits_CCoins(tx, pindex->nHeight);
        // The CCoins serialization does not serialize negative numbers.
        // No network rules currently depend on the version here, so an inconsistency is harmless
        // but it must be corrected before txout nversion ever influences a network rule.
        if (outsBlock.nVersion < 0)
            outs.nVersion = outsBlock.nVersion;
        if (outs != outsBlock)
            fClean = fClean && error("Credits: DisconnectBlock() : added transaction mismatch? database corrupted. \nExpected:\n%s\nFound:\n%s\n", outs.ToString(), outsBlock.ToString());

        // remove outputs
        outs = Credits_CCoins();

        // restore inputs
        if (i > 0) { // not coinbases
            const Credits_CTxUndo &txundo = blockUndo.vtxundo[i-1];
            if (txundo.vprevout.size() != tx.vin.size())
                return error("Credits: DisconnectBlock() : transaction and undo data inconsistent");
            if(tx.IsClaim()) {
                for (unsigned int j = tx.vin.size(); j-- > 0;) {
					const COutPoint &out = tx.vin[j].prevout;
					const Credits_CTxInUndo &undo = txundo.vprevout[j];
					Claim_CCoins coins;
					credits_view.Claim_GetCoins(out.hash, coins); // this can fail if the prevout was already entirely spent
					if (coins.IsPruned())
						fClean = fClean && error("Credits: DisconnectBlock() : undo claim data adding output to missing transaction");

					if (coins.HasClaimable(out.n))
						fClean = fClean && error("Credits: DisconnectBlock() : undo claim data overwriting existing output");

					assert (coins.vout.size() >= out.n+1);
					coins.vout[out.n].nValueClaimable = undo.txout.nValue;
					if (!credits_view.Claim_SetCoins(out.hash, coins))
						return error("Credits: DisconnectBlock() : cannot restore claim coin inputs");

					nTotalClaimedCoinsForBlock += undo.txout.nValue;
                }
            } else {
                for (unsigned int j = tx.vin.size(); j-- > 0;) {
					const COutPoint &out = tx.vin[j].prevout;
					const Credits_CTxInUndo &undo = txundo.vprevout[j];
					Credits_CCoins coins;
					credits_view.Credits_GetCoins(out.hash, coins); // this can fail if the prevout was already entirely spent
					if (undo.nHeight != 0) {
						// undo data contains height: this is the last output of the prevout tx being spent
						if (!coins.IsPruned())
							fClean = fClean && error("Credits: DisconnectBlock() : undo data overwriting existing transaction");
						coins = Credits_CCoins();
						coins.fCoinBase = undo.fCoinBase;
						coins.nHeight = undo.nHeight;
						coins.nMetaData = undo.nMetaData;
						coins.nVersion = undo.nVersion;
					} else {
						if (coins.IsPruned())
							fClean = fClean && error("Credits: DisconnectBlock() : undo data adding output to missing transaction");
					}
					if (coins.IsAvailable(out.n))
						fClean = fClean && error("Credits: DisconnectBlock() : undo data overwriting existing output");
					if (coins.vout.size() < out.n+1)
						coins.vout.resize(out.n+1);
					coins.vout[out.n] = undo.txout;
					if (!credits_view.Credits_SetCoins(out.hash, coins))
						return error("Credits: DisconnectBlock() : cannot restore coin inputs");
                }
            }
        }
    }

    if(nTotalClaimedCoinsForBlock > 0) {
		const int64_t nTotalClaimedCoinsBefore = credits_view.Claim_GetTotalClaimedCoins();
		if(!credits_view.Claim_SetTotalClaimedCoins(nTotalClaimedCoinsBefore - nTotalClaimedCoinsForBlock)) {
			return state.Abort(_("Credits total claimed coins for block could not be set while disconnecting block."));
		}
		assert(credits_view.Claim_GetTotalClaimedCoins() > 0);
    }

    assert(pindex->pprev != NULL);
    //Moving claim pointer here. Note that this invokes the bitcoin *Claim methods,
    //a separate chain of methods to just update the claim state
    if(!Bitcoin_AlignClaimTip(pindex, (Credits_CBlockIndex*)pindex->pprev, credits_view, state, updateBitcoinUndo, vBlockUndoClaims)) {
        return state.Abort(strprintf(_("ERROR: Credits: DisconnectBlock: Failed to move claim tip from bitcoin block %s"), block.hashLinkedBitcoinBlock.GetHex()));
    }

    // move best block pointer to prevout block
    credits_view.Credits_SetBestBlock(pindex->pprev->GetBlockHash());


    if (pfClean) {
        *pfClean = fClean;
        return true;
    } else {
        return fClean;
    }
}

void static Bitcredit_FlushBlockFile(bool fFinalize = false)
{
    LOCK(cs_LastBlockFile);

    CDiskBlockPos posOld(credits_mainState.nLastBlockFile, 0);

    FILE *fileOld = Credits_OpenBlockFile(posOld);
    if (fileOld) {
        if (fFinalize)
            TruncateFile(fileOld, credits_mainState.infoLastBlockFile.nSize);
        FileCommit(fileOld);
        fclose(fileOld);
    }

    fileOld = Credits_OpenUndoFile(posOld);
    if (fileOld) {
        if (fFinalize)
            TruncateFile(fileOld, credits_mainState.infoLastBlockFile.nUndoSize);
        FileCommit(fileOld);
        fclose(fileOld);
    }
}

bool Bitcredit_FindUndoPos(MainState& mainState, CValidationState &state, int nFile, CDiskBlockPos &pos, unsigned int nAddSize);

static CCheckQueue<Bitcredit_CScriptCheck> bitcredit_scriptcheckqueue(128);

void Bitcredit_ThreadScriptCheck() {
    RenameThread("bitcredit_bitcoin-scriptch");
    bitcredit_scriptcheckqueue.Thread();
}

void UpdateResurrectedDepositBase(const Credits_CBlockIndex* pBlockToTrim, const Credits_CTransaction &tx, int64_t &nResurrectedDepositBase, Credits_CCoinsViewCache& credits_view) {
    if(pBlockToTrim != NULL) {
		if (!tx.IsClaim()) {
			BOOST_FOREACH(const Credits_CTxIn &txin, tx.vin) {
				const Credits_CCoins &coins = credits_view.Credits_GetCoins(txin.prevout.hash);

				if(coins.nHeight < pBlockToTrim->nHeight) {
					nResurrectedDepositBase += coins.vout[txin.prevout.n].nValue;
				}
			}
		}
    }
}

void UpdateTrimmedDepositBase(const Credits_CBlockIndex* pBlockToTrim, Credits_CBlock &trimBlock, int64_t &nTrimmedDepositBase, Credits_CCoinsViewCache& credits_view) {
    if(pBlockToTrim != NULL) {
		for (unsigned int i = 0; i < trimBlock.vtx.size(); i++) {
			const Credits_CTransaction &tx = trimBlock.vtx[i];
			const uint256 txHash = tx.GetHash();

			if(credits_view.Credits_HaveCoins(txHash)) {
				const Credits_CCoins &coins = credits_view.Credits_GetCoins(txHash);

				BOOST_FOREACH(const CTxOut &txout, coins.vout) {
					if(txout.nValue > 0) {
						nTrimmedDepositBase += txout.nValue;
					}
				}
			}
		}
    }
}

bool Bitcredit_ConnectBlock(Credits_CBlock& block, CValidationState& state, Credits_CBlockIndex* pindex, Credits_CCoinsViewCache& credits_view, bool updateBitcoinUndo, std::vector<pair<Bitcoin_CBlockIndex*, Bitcoin_CBlockUndoClaim> > &vBlockUndoClaims, bool fJustCheck)
{
    AssertLockHeld(cs_main);
    // Check it again in case a previous version let a bad block in
    if (!Bitcredit_CheckBlock(block, state, !fJustCheck, !fJustCheck, !fJustCheck))
        return false;

    // verify that the view's current state corresponds to the previous block
    uint256 hashPrevBlock = pindex->pprev == NULL ? uint256(0) : pindex->pprev->GetBlockHash();
    assert(hashPrevBlock == credits_view.Credits_GetBestBlock());

    // Special case for the genesis block, skipping connection of its transactions
    // (its coinbase is unspendable)
    if (block.GetHash() == Credits_Params().HashGenesisBlock()) {
        credits_view.Credits_SetBestBlock(pindex->GetBlockHash());
        return true;
    }

    //Moving claim pointer here. Note that this invokes the bitcoin *Claim functions,
    //a separate chain of methods to just update the claim state
    if(!Bitcoin_AlignClaimTip((Credits_CBlockIndex*)pindex->pprev, pindex, credits_view, state, updateBitcoinUndo, vBlockUndoClaims)) {
        return state.Abort(strprintf(_("ERROR: Credits: ConnectBlock: Failed to move claim tip to %s\n"), block.hashLinkedBitcoinBlock.GetHex()));
    }

    bool fScriptChecks = pindex->nHeight >= Checkpoints::Bitcredit_GetTotalBlocksEstimate();

    // Do not allow blocks that contain transactions which 'overwrite' older transactions,
    // unless those are already completely spent.
    // If such overwrites are allowed, coinbases and transactions depending upon those
    // can be duplicated to remove the ability to spend the first instance -- even after
    // being sent to another address.
    // See BIP30 and http://r6.ca/blog/20120206T005236Z.html for more information.
    // This logic is not necessary for memory pool transactions, as AcceptToMemoryPool
    // already refuses previously-known transaction ids entirely.
    // This rule was originally applied all blocks whose timestamp was after March 15, 2012, 0:00 UTC.
    // Now that the whole chain is irreversibly beyond that time it is applied to all blocks except the
    // two in the chain that violate it. This prevents exploiting the issue against nodes in their
    // initial block download.
    bool fEnforceBIP30 = !pindex->phashBlock; // Enforce on CreateNewBlock invocations which don't have a hash.
    if (fEnforceBIP30) {
        for (unsigned int i = 0; i < block.vtx.size(); i++) {
            uint256 hash = block.GetTxHash(i);
            if (credits_view.Credits_HaveCoins(hash) && !credits_view.Credits_GetCoins(hash).IsPruned())
                return state.DoS(100, error("Credits: ConnectBlock() : tried to overwrite transaction"),
                                 BITCREDIT_REJECT_INVALID, "bad-txns-BIP30");
        }
    }

    //Setup the block used for updating the deposit base due to long enough block chain
    Credits_CBlockIndex* pBlockToTrim = NULL;
    Credits_CBlock trimBlock;
    if(pindex->nHeight > BITCREDIT_DEPOSIT_CUTOFF_DEPTH) {
    	//Find trim block in active chain
		pBlockToTrim = credits_chainActive[pindex->nHeight - BITCREDIT_DEPOSIT_CUTOFF_DEPTH - 1];
	    // Read block from disk.
	    if (!Credits_ReadBlockFromDisk(trimBlock, pBlockToTrim)) {
	        return state.Abort(_("Failed to read trim block"));
	    }
    }
    //These two updates the deposit base
    int64_t nResurrectedDepositBase = 0;
    int64_t nTrimmedDepositBase = 0;

    unsigned int flags = SCRIPT_VERIFY_NOCACHE | SCRIPT_VERIFY_P2SH;

    Credits_CBlockUndo blockundo;

    CCheckQueueControl<Bitcredit_CScriptCheck> control(fScriptChecks && bitcredit_nScriptCheckThreads ? &bitcredit_scriptcheckqueue : NULL);

    int64_t nStart = GetTimeMicros();
    int64_t nClaimedCoinsForBlock = 0;
    int64_t nFees = 0;
    int nInputs = 0;
    unsigned int nSigOps = 0;
    //We must move the nTxOffset past the signature vector here, not required in Bitcoin.
    //It's sigvectorsize + sigarraysize + txvectorsize
    unsigned int sigSerSize = 0;
    for (unsigned int i = 0; i < block.vsig.size(); i++) {
    	sigSerSize += block.vsig[i].GetSerializeSize(SER_DISK, Credits_Params().ClientVersion());
    }
    CDiskTxPos pos(pindex->GetBlockPos(), GetSizeOfCompactSize(block.vsig.size()) + sigSerSize + GetSizeOfCompactSize(block.vtx.size()));
    std::vector<std::pair<uint256, CDiskTxPos> > vPos;
    vPos.reserve(block.vtx.size());
    for (unsigned int i = 0; i < block.vtx.size(); i++)
    {
        const Credits_CTransaction &tx = block.vtx[i];

        nInputs += tx.vin.size();
        nSigOps += Bitcredit_GetLegacySigOpCount(tx);
        if (nSigOps > BITCREDIT_MAX_BLOCK_SIGOPS)
            return state.DoS(100, error("Credits: ConnectBlock() : too many sigops"),
                             BITCREDIT_REJECT_INVALID, "bad-blk-sigops");

        if (!tx.IsCoinBase())
        {
        	if(tx.IsClaim()) {
				if (!credits_view.Claim_HaveInputs(tx))
					return state.DoS(100, error("Credits: ConnectBlock() : external bitcoin inputs missing/spent"),
									 BITCREDIT_REJECT_INVALID, "bad-txns-inputs-missingorspent");
        	} else {
				if (!credits_view.Credits_HaveInputs(tx))
					return state.DoS(100, error("Credits: ConnectBlock() : inputs missing/spent"),
									 BITCREDIT_REJECT_INVALID, "bad-txns-inputs-missingorspent");
        	}

			// Add in sigops done by pay-to-script-hash inputs;
			// this is to prevent a "rogue miner" from creating
			// an incredibly-expensive-to-validate block.
			nSigOps += Bitcredit_GetP2SHSigOpCount(tx, credits_view);
			if (nSigOps > BITCREDIT_MAX_BLOCK_SIGOPS)
				return state.DoS(100, error("Credits: ConnectBlock() : too many sigops"),
								 BITCREDIT_REJECT_INVALID, "bad-blk-sigops");

            if(tx.IsClaim()) {
            	nFees += credits_view.Claim_GetValueIn(tx) - tx.GetValueOut();
            } else {
            	nFees += credits_view.Credits_GetValueIn(tx) - tx.GetValueOut();
            }

            std::vector<Bitcredit_CScriptCheck> vChecks;
            if (!Credits_CheckInputs(tx, state, credits_view, nClaimedCoinsForBlock, fScriptChecks, flags, bitcredit_nScriptCheckThreads ? &vChecks : NULL))
                return false;
            control.Add(vChecks);

            UpdateResurrectedDepositBase(pBlockToTrim, tx, nResurrectedDepositBase, credits_view);
        }

        Credits_CTxUndo ctxundo;
        Bitcredit_UpdateCoins(tx, state, credits_view, ctxundo, pindex->nHeight, block.GetTxHash(i));
        if (!tx.IsCoinBase()) {
            blockundo.vtxundo.push_back(ctxundo);
        }

        vPos.push_back(std::make_pair(block.GetTxHash(i), pos));
        pos.nTxOffset += ::GetSerializeSize(tx, SER_DISK, CREDITS_CLIENT_VERSION);
    }

    UpdateTrimmedDepositBase(pBlockToTrim, trimBlock, nTrimmedDepositBase, credits_view);

    if(!Bitcredit_FindBestBlockAndCheckClaims(credits_view, nClaimedCoinsForBlock)) {
    	return false;
    }

    if(nClaimedCoinsForBlock > 0) {
		const int64_t nTotalClaimedCoinsBefore = credits_view.Claim_GetTotalClaimedCoins();
		if(!credits_view.Claim_SetTotalClaimedCoins(nTotalClaimedCoinsBefore + nClaimedCoinsForBlock)) {
			return state.Abort(_("Credits total claimed coins for block could not be set while connecting block."));
		}
    }

    int64_t nTime = GetTimeMicros() - nStart;
    if (bitcredit_fBenchmark)
        LogPrintf("Credits: - Connect %u transactions: %.2fms (%.3fms/tx, %.3fms/txin)\n", (unsigned)block.vtx.size(), 0.001 * nTime, 0.001 * nTime / block.vtx.size(), nInputs <= 1 ? 0 : 0.001 * nTime / (nInputs-1));

    const int64_t nDepositAmount = block.GetDepositAmount();

    //Check that coinbase out is in range
    Credits_CBlockIndex* prevBlockIndex = (Credits_CBlockIndex*)pindex->pprev;
    const uint64_t allowedSubsidy = Bitcredit_GetAllowedBlockSubsidy(prevBlockIndex->nTotalMonetaryBase, nDepositAmount, prevBlockIndex->nTotalDepositBase);
    const uint64_t allowedSubsidyIncFee = allowedSubsidy + nFees;
    const int64_t coinbaseValueOut = block.vtx[0].GetValueOut();
    if (coinbaseValueOut > allowedSubsidyIncFee)
        return state.DoS(100,
                         error("Credits: ConnectBlock() : coinbase pays too much (actual=%d vs limit=%d)",
                        		coinbaseValueOut, allowedSubsidyIncFee),
                               BITCREDIT_REJECT_INVALID, "bad-cb-amount");

	if(allowedSubsidy < Bitcredit_GetMaxBlockSubsidy(prevBlockIndex->nTotalMonetaryBase)) {
		//Check that deposit is above allowed if coinbase is used as deposit
		const uint256 coinbaseHash = block.vtx[0].GetHash();
		for (unsigned int i = 1; i < block.vtx.size(); i++) {
			const Credits_CTransaction& tx = block.vtx[i];
			if(tx.IsDeposit()) {
				if(tx.vin[0].prevout.hash == coinbaseHash) {
					uint64_t nRequiredDeposit = Bitcredit_GetRequiredDeposit(prevBlockIndex->nTotalDepositBase);
					if (nDepositAmount < nRequiredDeposit)
						return state.DoS(100,
										 error("Credits: ConnectBlock() : deposit is too low (actual=%d vs required=%d)",
												 nDepositAmount, nRequiredDeposit),
											   BITCREDIT_REJECT_INVALID, "bad-deposit-amount");
				}
			} else {
				break;
			}
		}
	}

    //Check that total monetary base is exactly previous total monetary base plus coinbase out minus fees
    const int64_t nMonetaryBaseChange = (coinbaseValueOut - nFees) + nClaimedCoinsForBlock;
    uint64_t nExpectedTotalMonetaryBase = prevBlockIndex->nTotalMonetaryBase + nMonetaryBaseChange;
    if (block.nTotalMonetaryBase != nExpectedTotalMonetaryBase)
        return state.DoS(100,
                         error("Credits: ConnectBlock() : total monetary base is wrong (was=%d should be=%d)",
                        		 block.nTotalMonetaryBase, nExpectedTotalMonetaryBase),
                               BITCREDIT_REJECT_INVALID, "bad-mb-amount");

    uint64_t nExpectedTotalDepositBase = prevBlockIndex->nTotalDepositBase + nMonetaryBaseChange - nTrimmedDepositBase + nResurrectedDepositBase;
    if (block.nTotalDepositBase != nExpectedTotalDepositBase)
        return state.DoS(100,
                         error("Credits: ConnectBlock() : total deposit base is wrong (was=%d should be=%d)",
                        		 block.nTotalDepositBase, nExpectedTotalDepositBase),
                               BITCREDIT_REJECT_INVALID, "bad-db-amount");

    if (!control.Wait())
        return state.DoS(100, false);
    int64_t nTime2 = GetTimeMicros() - nStart;
    if (bitcredit_fBenchmark)
        LogPrintf("Credits: - Verify %u txins: %.2fms (%.3fms/txin)\n", nInputs - 1, 0.001 * nTime2, nInputs <= 1 ? 0 : 0.001 * nTime2 / (nInputs-1));

    if (fJustCheck)
        return true;

    // Correct transaction counts.
    pindex->nTx = block.vtx.size();
    if (pindex->pprev)
        pindex->nChainTx = pindex->pprev->nChainTx + block.vtx.size();

    // Write undo information to disk
    if (pindex->GetUndoPos().IsNull() || !pindex->IsValid(BLOCK_VALID_SCRIPTS))
    {
        if (pindex->GetUndoPos().IsNull()) {
            CDiskBlockPos pos;
            if (!Bitcredit_FindUndoPos(credits_mainState, state, pindex->nFile, pos, ::GetSerializeSize(blockundo, SER_DISK, CREDITS_CLIENT_VERSION) + 40))
                return error("Credits: ConnectBlock() : FindUndoPos failed");
            if (!blockundo.WriteToDisk(pos, pindex->pprev->GetBlockHash(), Credits_NetParams()))
                return state.Abort(_("Credits: Failed to write undo data"));

            // update nUndoPos in block index
            pindex->nUndoPos = pos.nPos;
            pindex->nStatus |= BLOCK_HAVE_UNDO;
        }

        pindex->RaiseValidity(BLOCK_VALID_SCRIPTS);

        Credits_CDiskBlockIndex blockindex(pindex);
        if (!bitcredit_pblocktree->WriteBlockIndex(blockindex))
            return state.Abort(_("Credits: Failed to write block index"));
    }

    if (bitcredit_fTxIndex)
        if (!bitcredit_pblocktree->WriteTxIndex(vPos))
            return state.Abort(_("Credits: Failed to write transaction index"));

    // add this block to the view's block chain
    bool ret;
    ret = credits_view.Credits_SetBestBlock(pindex->GetBlockHash());
    assert(ret);

    // Watch for transactions paying to me
    for (unsigned int i = 0; i < block.vtx.size(); i++)
        bitcredit_g_signals.SyncTransaction(bitcoin_pwalletMain, block.GetTxHash(i), block.vtx[i], &block);

    return true;
}

// Update the on-disk chain state.
bool static Bitcredit_WriteChainState(CValidationState &state) {
    static int64_t nLastWrite = 0;
    if (!Bitcredit_IsInitialBlockDownload() || credits_pcoinsTip->GetCacheSize() > bitcredit_nCoinCacheSize || GetTimeMicros() > nLastWrite + 600*1000000) {
        // Typical CCoins structures on disk are around 100 bytes in size.
        // Pushing a new one to the database can cause it to be written
        // twice (once in the log, and once in the tables). This is already
        // an overestimation, as most will delete an existing entry or
        // overwrite one. Still, use a conservative safety factor of 2.
        if (!CheckDiskSpace(100 * 2 * 2 * credits_pcoinsTip->GetCacheSize()))
            return state.Error("out of disk space");
        Bitcredit_FlushBlockFile();
        bitcredit_pblocktree->Sync();
        if (!credits_pcoinsTip->All_Flush())
            return state.Abort(_("Failed to write to coin database"));
        nLastWrite = GetTimeMicros();
    }
    return true;
}

/**
 * Returns the number of bitcoin blocks that we have to move the tip to get to the requested position
 */
bool GetBitcoinBlockSteps(int& bitcoinBlockSteps, uint256& alignToBitcoinBlockHash, CValidationState &state, Credits_CCoinsViewCache& credits_view) {
	std::map<uint256, Bitcoin_CBlockIndex*>::iterator mi = bitcoin_mapBlockIndex.find(alignToBitcoinBlockHash);
	if (mi == bitcoin_mapBlockIndex.end()) {
		return state.Abort(strprintf(_("Referenced claim block %s can not be found"), alignToBitcoinBlockHash.ToString()));
	}
	const Bitcoin_CBlockIndex* pmoveToIndex = (*mi).second;
	if (!bitcoin_chainActive.Contains(pmoveToIndex)) {
		return state.Abort(strprintf(_("Referenced claim block %s is not in active bitcoin block chain"), pmoveToIndex->phashBlock->ToString()));
	}

	// If credits_view hasn't moved yet, the height will be the number of blocks we have to move
	const uint256 claimBestBlockHash = credits_view.Claim_GetBestBlock();
	if (claimBestBlockHash == uint256(0)) {
		bitcoinBlockSteps = pmoveToIndex->nHeight;
		return true;
	}
	//Set starting point for movement
	Bitcoin_CBlockIndex* pBestBlockIndex;
	std::map<uint256, Bitcoin_CBlockIndex*>::iterator mi2 = bitcoin_mapBlockIndex.find(claimBestBlockHash);
	if (mi2 == bitcoin_mapBlockIndex.end()) {
		return state.Abort(strprintf(_("Referenced claim block %s can not be found"), claimBestBlockHash.ToString()));
	}
	pBestBlockIndex = (*mi2).second;

	bitcoinBlockSteps = pmoveToIndex->nHeight  - pBestBlockIndex->nHeight;
	return true;
}

// Update chainActive and related internal data structures.
void static Bitcredit_UpdateTip(Credits_CBlockIndex *pindexNew) {
    credits_chainActive.SetTip(pindexNew);

    // Update best block in wallet (so we can detect restored wallets)
    bool fIsInitialDownload = Bitcredit_IsInitialBlockDownload();
    if ((credits_chainActive.Height() % 20160) == 0 || (!fIsInitialDownload && (credits_chainActive.Height() % 144) == 0))
        bitcredit_g_signals.SetBestChain(credits_chainActive.GetLocator());

    // New best block
    credits_nTimeBestReceived = GetTime();
    credits_mempool.AddTransactionsUpdated(1);
		LogPrintf("Credits: UpdateTip: new best=%s  height=%d  log2_work=%.8g  tx=%lu  date=%s progress=%f\n",
      credits_chainActive.Tip()->GetBlockHash().ToString(), credits_chainActive.Height(), log(credits_chainActive.Tip()->nChainWork.getdouble())/log(2.0), (unsigned long)credits_chainActive.Tip()->nChainTx,
      DateTimeStrFormat("%Y-%m-%d %H:%M:%S", credits_chainActive.Tip()->GetBlockTime()),
      Checkpoints::Credits_GuessVerificationProgress((Credits_CBlockIndex*)credits_chainActive.Tip()));

    // Check the version of the last 100 blocks to see if we need to upgrade:
    if (!fIsInitialDownload)
    {
        int nUpgraded = 0;
        const Credits_CBlockIndex* pindex = (Credits_CBlockIndex*)credits_chainActive.Tip();
        for (int i = 0; i < 100 && pindex != NULL; i++)
        {
            if (pindex->nVersion > Credits_CBlock::CURRENT_VERSION)
                ++nUpgraded;
            pindex = (Credits_CBlockIndex*)pindex->pprev;
        }
        if (nUpgraded > 0)
            LogPrintf("Credits: SetBestChain: %d of last 100 blocks above version %d\n", nUpgraded, (int)Credits_CBlock::CURRENT_VERSION);
        if (nUpgraded > 100/2)
            // strMiscWarning is read by GetWarnings(), called by Qt and the JSON-RPC code to warn the user:
            strMiscWarning = _("Credits: Warning: This version is obsolete, upgrade required!");
    }
}

// Disconnect chainActive's tip.
bool static Bitcredit_DisconnectTip(CValidationState &state) {
    Credits_CBlockIndex *pindexDelete = (Credits_CBlockIndex*)credits_chainActive.Tip();
    assert(pindexDelete);
    credits_mempool.check(credits_pcoinsTip);
    // Read block from disk.
    Credits_CBlock block;
    if (!Credits_ReadBlockFromDisk(block, pindexDelete))
        return state.Abort(_("Failed to read block"));
    // Apply the block atomically to the chain state.
    int64_t nStart = GetTimeMicros();

    Credits_CBlockIndex* pIndexPrev = (Credits_CBlockIndex*)pindexDelete->pprev;

    Credits_CCoinsViewCache credits_view(*credits_pcoinsTip, true);

    int bitcoinBlockSteps;
    if(!GetBitcoinBlockSteps(bitcoinBlockSteps, pIndexPrev->hashLinkedBitcoinBlock, state, *credits_pcoinsTip)) {
		return state.Abort(_("Problem when calculating bitcoin block steps"));
    }

	uint256 hashMoveFromBitcoinBlock = pindexDelete->hashLinkedBitcoinBlock;
	std::map<uint256, Bitcoin_CBlockIndex*>::iterator mi = bitcoin_mapBlockIndex.find(hashMoveFromBitcoinBlock);
	if (mi == bitcoin_mapBlockIndex.end()) {
        return state.Abort(strprintf(_("Referenced claim bitcoin block %s can not be found"), hashMoveFromBitcoinBlock.ToString()));
    }
	const Bitcoin_CBlockIndex* pmoveFromBitcoinIndex = (*mi).second;

	const bool fastForwardClaimState = FastForwardClaimStateFor(pmoveFromBitcoinIndex->nHeight, pmoveFromBitcoinIndex->GetBlockHash());
    if (bitcredit_fBenchmark) {
		if(fastForwardClaimState) {
			LogPrintf("Credits: DisconnectTip() : No tmp db created, in fast forward state, claim tip is %d bitcoin blocks ahead\n", -bitcoinBlockSteps);
		} else {
			LogPrintf("Credits: DisconnectTip() : No tmp db created, only moving claim tip %d bitcoin blocks\n", bitcoinBlockSteps);
		}
    }
	std::vector<pair<Bitcoin_CBlockIndex*, Bitcoin_CBlockUndoClaim> > vBlockUndoClaims;
	if (!Bitcredit_DisconnectBlock(block, state, pindexDelete, credits_view, true, vBlockUndoClaims))
		return error("Credits: DisconnectTip() : DisconnectBlock %s failed", pindexDelete->GetBlockHash().ToString());
	if(fastForwardClaimState) {
		assert(credits_view.Credits_Flush());
	} else {
		assert(Bitcoin_DeleteBlockUndoClaimsFromDisk(state, vBlockUndoClaims));
		assert(credits_view.All_Flush());
	}

    if (bitcredit_fBenchmark)
        LogPrintf("Credits: - Disconnect: %.2fms\n", (GetTimeMicros() - nStart) * 0.001);
    // Write the chain state to disk, if necessary.
    if (!Bitcredit_WriteChainState(state))
        return false;
    // Resurrect mempool transactions from the disconnected block.
    BOOST_FOREACH(const Credits_CTransaction &tx, block.vtx) {
        // ignore validation errors in resurrected transactions
        list<Credits_CTransaction> removed;
        CValidationState stateDummy;
        if (!tx.IsCoinBase() && !tx.IsDeposit())
            if (!Bitcredit_AcceptToMemoryPool(credits_mempool, stateDummy, tx, false, NULL))
                credits_mempool.remove(tx, removed, true);
    }
    credits_mempool.check(credits_pcoinsTip);
    // Update chainActive and related variables.
    Bitcredit_UpdateTip(pIndexPrev);
    // Let wallets know transactions went from 1-confirmed to
    // 0-confirmed or conflicted:
    BOOST_FOREACH(const Credits_CTransaction &tx, block.vtx) {
        Bitcredit_SyncWithWallets(bitcoin_pwalletMain, tx.GetHash(), tx, NULL);
    }
    return true;
}

// Connect a new block to chainActive.
bool static Bitcredit_ConnectTip(CValidationState &state, Credits_CBlockIndex *pindexNew) {
    assert(pindexNew->pprev == credits_chainActive.Tip());
    credits_mempool.check(credits_pcoinsTip);
    // Read block from disk.
    Credits_CBlock block;
    if (!Credits_ReadBlockFromDisk(block, pindexNew))
        return state.Abort(_("Credits: ConnectTip() : Failed to read block"));
    // Apply the block atomically to the chain state.
    int64_t nStart = GetTimeMicros();

    Credits_CCoinsViewCache credits_view(*credits_pcoinsTip, true);


	uint256 hashNewTipBitcoinBlock = pindexNew->hashLinkedBitcoinBlock;
	std::map<uint256, Bitcoin_CBlockIndex*>::iterator mi = bitcoin_mapBlockIndex.find(hashNewTipBitcoinBlock);
	if (mi == bitcoin_mapBlockIndex.end()) {
        return state.Abort(strprintf(_("Credits: ConnectTip() : Referenced claim bitcoin block %s can not be found"), hashNewTipBitcoinBlock.ToString()));
    }
	const Bitcoin_CBlockIndex* palignToBitcoinIndex = (*mi).second;

	const bool fastForwardClaimState = FastForwardClaimStateFor(palignToBitcoinIndex->nHeight, palignToBitcoinIndex->GetBlockHash());
    if (bitcredit_fBenchmark) {
		int bitcoinBlockSteps;
		if(!GetBitcoinBlockSteps(bitcoinBlockSteps, block.hashLinkedBitcoinBlock, state, *credits_pcoinsTip)) {
			return state.Abort(_("Credits: ConnectTip() : Problem when calculating bitcoin block steps"));
		}
		if(fastForwardClaimState) {
			LogPrintf("Credits: ConnectTip() : In fast forward state, claim tip is %d bitcoin blocks ahead\n", -bitcoinBlockSteps);
		} else {
			LogPrintf("Credits: ConnectTip() : Moving claim tip %d bitcoin blocks\n", bitcoinBlockSteps);
		}
    }
	std::vector<pair<Bitcoin_CBlockIndex*, Bitcoin_CBlockUndoClaim> > vBlockUndoClaims;
	CInv inv(MSG_BLOCK, pindexNew->GetBlockHash());
	if (!Bitcredit_ConnectBlock(block, state, pindexNew, credits_view, true, vBlockUndoClaims, false)) {
		if (state.IsInvalid())
			Bitcredit_InvalidBlockFound(pindexNew, state);
		return error("Credits: ConnectTip() : ConnectBlock %s failed", pindexNew->GetBlockHash().ToString());
	}
	bitcredit_mapBlockSource.erase(inv.hash);
	if(fastForwardClaimState) {
		assert(credits_view.Credits_Flush());
	} else {
		assert(Bitcoin_WriteBlockUndoClaimsToDisk(state, vBlockUndoClaims));
		assert(credits_view.All_Flush());
	}

    if (bitcredit_fBenchmark)
        LogPrintf("Credits: - Connect: %.2fms\n", (GetTimeMicros() - nStart) * 0.001);
    // Write the chain state to disk, if necessary.
    if (!Bitcredit_WriteChainState(state))
        return false;
    // Remove conflicting transactions from the mempool.
    list<Credits_CTransaction> txConflicted;
    credits_mempool.removeForBlock(block.vtx, pindexNew->nHeight, txConflicted);
    credits_mempool.check(credits_pcoinsTip);
    // Update chainActive & related variables.
    Bitcredit_UpdateTip(pindexNew);

	if (!Bitcoin_TrimCompressedBlockHistory(state)) {
		return error("Bitcoin: ConnectTip() : Could not trim block history!");
	}

    // Tell wallet about transactions that went from mempool
    // to conflicted:
    BOOST_FOREACH(const Credits_CTransaction &tx, txConflicted) {
        Bitcredit_SyncWithWallets(bitcoin_pwalletMain, tx.GetHash(), tx, NULL);
    }
    // ... and about transactions that got confirmed:
    BOOST_FOREACH(const Credits_CTransaction &tx, block.vtx) {
        Bitcredit_SyncWithWallets(bitcoin_pwalletMain, tx.GetHash(), tx, &block);
    }
    return true;
}

// Make chainMostWork correspond to the chain with the most work in it, that isn't
// known to be invalid (it's however far from certain to be valid).
void static Bitcredit_FindMostWorkChain() {
    Credits_CBlockIndex *pindexNew = NULL;

    // In case the current best is invalid, do not consider it.
    while (bitcredit_chainMostWork.Tip() && (bitcredit_chainMostWork.Tip()->nStatus & BLOCK_FAILED_MASK)) {
        bitcredit_setBlockIndexValid.erase((Credits_CBlockIndex*)bitcredit_chainMostWork.Tip());
        bitcredit_chainMostWork.SetTip(bitcredit_chainMostWork.Tip()->pprev);
    }

    do {
        // Find the best candidate header.
        {
            std::set<Credits_CBlockIndex*, Credits_CBlockIndexWorkComparator>::reverse_iterator it = bitcredit_setBlockIndexValid.rbegin();
            if (it == bitcredit_setBlockIndexValid.rend())
                return;
            pindexNew = *it;
        }

        // Check whether all blocks on the path between the currently active chain and the candidate are valid.
        // Just going until the active chain is an optimization, as we know all blocks in it are valid already.
        Credits_CBlockIndex *pindexTest = pindexNew;
        bool fInvalidAncestor = false;
        while (pindexTest && !credits_chainActive.Contains(pindexTest)) {
            if (!pindexTest->IsValid(BLOCK_VALID_TRANSACTIONS) || !(pindexTest->nStatus & BLOCK_HAVE_DATA)) {
                // Candidate has an invalid ancestor, remove entire chain from the set.
                if (bitcredit_pindexBestInvalid == NULL || pindexNew->nChainWork > bitcredit_pindexBestInvalid->nChainWork)
                    bitcredit_pindexBestInvalid = pindexNew;
                Credits_CBlockIndex *pindexFailed = pindexNew;
                while (pindexTest != pindexFailed) {
                    pindexFailed->nStatus |= BLOCK_FAILED_CHILD;
                    bitcredit_setBlockIndexValid.erase(pindexFailed);
                    pindexFailed = (Credits_CBlockIndex*)pindexFailed->pprev;
                }
                fInvalidAncestor = true;
                break;
            }
            pindexTest = (Credits_CBlockIndex*)pindexTest->pprev;
        }
        if (fInvalidAncestor)
            continue;

        break;
    } while(true);

    // Check whether it's actually an improvement.
    if ((Credits_CBlockIndex*)bitcredit_chainMostWork.Tip() && !Credits_CBlockIndexWorkComparator()((Credits_CBlockIndex*)bitcredit_chainMostWork.Tip(), pindexNew))
        return;

    // We have a new best.
    bitcredit_chainMostWork.SetTip(pindexNew);
}

// Try to activate to the most-work chain (thereby connecting it).
bool Bitcredit_ActivateBestChain(CValidationState &state) {
    LOCK(cs_main);
    Credits_CBlockIndex *pindexOldTip = (Credits_CBlockIndex*)credits_chainActive.Tip();
    bool fComplete = false;
    while (!fComplete) {
        Bitcredit_FindMostWorkChain();
        fComplete = true;

        // Check whether we have something to do.
        if (bitcredit_chainMostWork.Tip() == NULL) break;

        // Disconnect active blocks which are no longer in the best chain.
        while (credits_chainActive.Tip() && !bitcredit_chainMostWork.Contains(credits_chainActive.Tip())) {
            if (!Bitcredit_DisconnectTip(state))
                return false;
        }

        // Connect new blocks.
        while (!credits_chainActive.Contains(bitcredit_chainMostWork.Tip())) {
            Credits_CBlockIndex *pindexConnect = bitcredit_chainMostWork[credits_chainActive.Height() + 1];
            if (!Bitcredit_ConnectTip(state, pindexConnect)) {
                if (state.IsInvalid()) {
                    // The block violates a consensus rule.
                    if (!state.CorruptionPossible())
                        Bitcredit_InvalidChainFound((Credits_CBlockIndex*)bitcredit_chainMostWork.Tip());
                    fComplete = false;
                    state = CValidationState();
                    break;
                } else {
                    // A system error occurred (disk space, database error, ...).
                    return false;
                }
            }
        }
    }

    if (credits_chainActive.Tip() != pindexOldTip) {
        std::string strCmd = GetArg("-blocknotify", "");
        if (!Bitcredit_IsInitialBlockDownload() && !strCmd.empty())
        {
            boost::replace_all(strCmd, "%s", credits_chainActive.Tip()->GetBlockHash().GetHex());
            boost::thread t(runCommand, strCmd); // thread runs free
        }
    }

    return true;
}


Credits_CBlockIndex* Bitcredit_AddToBlockIndex(Credits_CBlockHeader& block)
{
    // Check for duplicate
    uint256 hash = block.GetHash();
    std::map<uint256, Credits_CBlockIndex*>::iterator it = credits_mapBlockIndex.find(hash);
    if (it != credits_mapBlockIndex.end())
        return it->second;

    // Construct new block index object
    Credits_CBlockIndex* pindexNew = new Credits_CBlockIndex(block);
    assert(pindexNew);
    {
         LOCK(bitcredit_cs_nBlockSequenceId);
         pindexNew->nSequenceId = bitcredit_nBlockSequenceId++;
    }
    map<uint256, Credits_CBlockIndex*>::iterator mi = credits_mapBlockIndex.insert(make_pair(hash, pindexNew)).first;
    pindexNew->phashBlock = &((*mi).first);
    map<uint256, Credits_CBlockIndex*>::iterator miPrev = credits_mapBlockIndex.find(block.hashPrevBlock);
    if (miPrev != credits_mapBlockIndex.end())
    {
        pindexNew->pprev = (*miPrev).second;
        pindexNew->nHeight = pindexNew->pprev->nHeight + 1;
    }
    pindexNew->nChainWork = (pindexNew->pprev ? pindexNew->pprev->nChainWork : 0) + pindexNew->GetBlockWork();
    pindexNew->RaiseValidity(BLOCK_VALID_TREE);

    return pindexNew;
}


// Mark a block as having its data received and checked (up to BLOCK_VALID_TRANSACTIONS).
bool Bitcredit_ReceivedBlockTransactions(const Credits_CBlock &block, CValidationState& state, Credits_CBlockIndex *pindexNew, const CDiskBlockPos& pos)
{
    pindexNew->nTx = block.vtx.size();
    if (pindexNew->pprev) {
        // Not the genesis block.
        if (pindexNew->pprev->nChainTx) {
            // This parent's block's total number transactions is known, so compute outs.
            pindexNew->nChainTx = pindexNew->pprev->nChainTx + pindexNew->nTx;
        } else {
            // The total number of transactions isn't known yet.
            // We will compute it when the block is connected.
            pindexNew->nChainTx = 0;
        }
    } else {
        // Genesis block.
        pindexNew->nChainTx = pindexNew->nTx;
    }
    pindexNew->nFile = pos.nFile;
    pindexNew->nDataPos = pos.nPos;
    pindexNew->nUndoPos = 0;
    pindexNew->nStatus |= BLOCK_HAVE_DATA;

    if (pindexNew->RaiseValidity(BLOCK_VALID_TRANSACTIONS))
        bitcredit_setBlockIndexValid.insert(pindexNew);

    if (!bitcredit_pblocktree->WriteBlockIndex(Credits_CDiskBlockIndex(pindexNew)))
        return state.Abort(_("Credits: Failed to write block index"));

    // New best?
    if (!Bitcredit_ActivateBestChain(state))
        return false;

    LOCK(cs_main);
    if (pindexNew == credits_chainActive.Tip())
    {
        // Clear fork warning if its no longer applicable
        Bitcredit_CheckForkWarningConditions();
        // Notify UI to display prev block's coinbase if it was ours
        static uint256 hashPrevBestCoinBase;
        bitcredit_g_signals.UpdatedTransaction(hashPrevBestCoinBase);
        hashPrevBestCoinBase = block.GetTxHash(0);
    } else
        Bitcredit_CheckForkWarningConditionsOnNewFork(pindexNew);

    if (!bitcredit_pblocktree->Flush())
        return state.Abort(_("Credits: Failed to sync block index"));

    uiInterface.NotifyBlocksChanged();
    return true;
}


bool Bitcredit_FindBlockPos(CValidationState &state, CDiskBlockPos &pos, unsigned int nAddSize, unsigned int nHeight, uint64_t nTime, bool fKnown = false)
{
    bool fUpdatedLast = false;

    LOCK(cs_LastBlockFile);

    if (fKnown) {
        if (credits_mainState.nLastBlockFile != pos.nFile) {
            credits_mainState.nLastBlockFile = pos.nFile;
            credits_mainState.infoLastBlockFile.SetNull();
            bitcredit_pblocktree->ReadBlockFileInfo(credits_mainState.nLastBlockFile, credits_mainState.infoLastBlockFile);
            fUpdatedLast = true;
        }
    } else {
        while (credits_mainState.infoLastBlockFile.nSize + nAddSize >= BITCREDIT_MAX_BLOCKFILE_SIZE) {
            LogPrintf("Credits: Leaving block file %i: %s\n", credits_mainState.nLastBlockFile, credits_mainState.infoLastBlockFile.ToString());
            Bitcredit_FlushBlockFile(true);
            credits_mainState.nLastBlockFile++;
            credits_mainState.infoLastBlockFile.SetNull();
            bitcredit_pblocktree->ReadBlockFileInfo(credits_mainState.nLastBlockFile, credits_mainState.infoLastBlockFile); // check whether data for the new file somehow already exist; can fail just fine
            fUpdatedLast = true;
        }
        pos.nFile = credits_mainState.nLastBlockFile;
        pos.nPos = credits_mainState.infoLastBlockFile.nSize;
    }

    credits_mainState.infoLastBlockFile.nSize += nAddSize;
    credits_mainState.infoLastBlockFile.AddBlock(nHeight, nTime);

    if (!fKnown) {
        unsigned int nOldChunks = (pos.nPos + BITCREDIT_BLOCKFILE_CHUNK_SIZE - 1) / BITCREDIT_BLOCKFILE_CHUNK_SIZE;
        unsigned int nNewChunks = (credits_mainState.infoLastBlockFile.nSize + BITCREDIT_BLOCKFILE_CHUNK_SIZE - 1) / BITCREDIT_BLOCKFILE_CHUNK_SIZE;
        if (nNewChunks > nOldChunks) {
            if (CheckDiskSpace(nNewChunks * BITCREDIT_BLOCKFILE_CHUNK_SIZE - pos.nPos)) {
                FILE *file = Credits_OpenBlockFile(pos);
                if (file) {
                    LogPrintf("Credits: Pre-allocating up to position 0x%x in blk%05u.dat\n", nNewChunks * BITCREDIT_BLOCKFILE_CHUNK_SIZE, pos.nFile);
                    AllocateFileRange(file, pos.nPos, nNewChunks * BITCREDIT_BLOCKFILE_CHUNK_SIZE - pos.nPos);
                    fclose(file);
                }
            }
            else
                return state.Error("out of disk space");
        }
    }

    if (!bitcredit_pblocktree->WriteBlockFileInfo(credits_mainState.nLastBlockFile, credits_mainState.infoLastBlockFile))
        return state.Abort(_("Credits: Failed to write file info"));
    if (fUpdatedLast)
        bitcredit_pblocktree->WriteLastBlockFile(credits_mainState.nLastBlockFile);

    return true;
}

bool Bitcredit_FindUndoPos(MainState& mainState, CValidationState &state, int nFile, CDiskBlockPos &pos, unsigned int nAddSize)
{
    pos.nFile = nFile;

    LOCK(cs_LastBlockFile);

    unsigned int nNewSize;
    if (nFile == mainState.nLastBlockFile) {
        pos.nPos = mainState.infoLastBlockFile.nUndoSize;
        nNewSize = (mainState.infoLastBlockFile.nUndoSize += nAddSize);
        if (!bitcredit_pblocktree->WriteBlockFileInfo(mainState.nLastBlockFile, mainState.infoLastBlockFile))
            return state.Abort(_("Credits: Failed to write block info"));
    } else {
    	CBlockFileInfo info;
        if (!bitcredit_pblocktree->ReadBlockFileInfo(nFile, info))
            return state.Abort(_("Credits: Failed to read block info"));
        pos.nPos = info.nUndoSize;
        nNewSize = (info.nUndoSize += nAddSize);
        if (!bitcredit_pblocktree->WriteBlockFileInfo(nFile, info))
            return state.Abort(_("Credits: Failed to write block info"));
    }

    unsigned int nOldChunks = (pos.nPos + BITCREDIT_UNDOFILE_CHUNK_SIZE - 1) / BITCREDIT_UNDOFILE_CHUNK_SIZE;
    unsigned int nNewChunks = (nNewSize + BITCREDIT_UNDOFILE_CHUNK_SIZE - 1) / BITCREDIT_UNDOFILE_CHUNK_SIZE;
    if (nNewChunks > nOldChunks) {
        if (CheckDiskSpace(nNewChunks * BITCREDIT_UNDOFILE_CHUNK_SIZE - pos.nPos)) {
            FILE *file = Credits_OpenUndoFile(pos);
            if (file) {
                LogPrintf("Credits: Pre-allocating up to position 0x%x in rev%05u.dat\n", nNewChunks * BITCREDIT_UNDOFILE_CHUNK_SIZE, pos.nFile);
                AllocateFileRange(file, pos.nPos, nNewChunks * BITCREDIT_UNDOFILE_CHUNK_SIZE - pos.nPos);
                fclose(file);
            }
        }
        else
            return state.Error("out of disk space");
    }

    return true;
}

bool Credits_CheckLinkedBitcoinBlock(const Credits_CBlockHeader& block, CValidationState& state)
{
	//Check that the reference to -->bitcoin<-- block  is buried deep enough in the bitcoin blockchain
	std::map<uint256, Bitcoin_CBlockIndex*>::iterator mi = bitcoin_mapBlockIndex.find(block.hashLinkedBitcoinBlock);
	if (mi == bitcoin_mapBlockIndex.end()) {
		return state.DoS(100, error(strprintf("Credits: CheckBlockHeader() : referenced bitcoin block could not be found: %s", block.hashLinkedBitcoinBlock.GetHex())),
				BITCREDIT_REJECT_INVALID_BITCOIN_BLOCK_LINK, "invalid-bitcoin-link");
	} else {
		const Bitcoin_CBlockIndex* pindex = (*mi).second;

		if(!bitcoin_chainActive.Contains(pindex)) {
			return state.DoS(100, error(strprintf("Credits: CheckBlockHeader() : referenced bitcoin block is not in active chain: %s", block.hashLinkedBitcoinBlock.GetHex())),
					BITCREDIT_REJECT_INVALID_BITCOIN_BLOCK_LINK, "bitcoin-link-not-active");
		}

		//Link depth not checked for genesis block. This is to prevent test setup from failing
		if(block.GetHash() != Credits_Params().GenesisBlock().GetHash()) {
			if(bitcoin_chainActive.Height() - pindex->nHeight < Credits_Params().AcceptDepthLinkedBitcoinBlock()) {
				return state.DoS(100, error(strprintf("Credits: CheckBlockHeader() : referenced bitcoin block is not deep enough in active chain: %s", block.hashLinkedBitcoinBlock.GetHex())),
						BITCREDIT_REJECT_INVALID_BITCOIN_BLOCK_LINK, "bitcoin-link-not-deep-enough");
			}
		}

		if(pindex->nHeight > BITCREDIT_MAX_BITCOIN_LINK_HEIGHT) {
			return state.DoS(100, error(strprintf("Credits: CheckBlockHeader() : referenced bitcoin block is too high in chain. Max allowed: %s, current height: %s", BITCREDIT_MAX_BITCOIN_LINK_HEIGHT, pindex->nHeight)),
					BITCREDIT_REJECT_INVALID_BITCOIN_BLOCK_LINK, "bitcoin-link-too-high");
		}
	}

    return true;
}

bool Bitcredit_CheckBlockHeader(const Credits_CBlockHeader& block, CValidationState& state, bool fCheckPOW, bool fCheckLinkedBitcoinBlock)
{
    // Check proof of work matches claimed amount
    if (fCheckPOW && !Bitcredit_CheckProofOfWork(block.GetHash(), block.nBits, block.nTotalDepositBase, block.nDepositAmount))
        return state.DoS(50, error("Credits: CheckBlockHeader() : proof of work failed"),
                         BITCREDIT_REJECT_INVALID, "high-hash");

    // Check timestamp
    if (block.GetBlockTime() > GetAdjustedTime() + 2 * 60 * 60)
        return state.Invalid(error("Credits: CheckBlockHeader() : block timestamp too far in the future"),
                             BITCREDIT_REJECT_INVALID, "time-too-new");

    Credits_CBlockIndex* pcheckpoint = Checkpoints::Bitcredit_GetLastCheckpoint(credits_mapBlockIndex);
    if (pcheckpoint && block.hashPrevBlock != (credits_chainActive.Tip() ? credits_chainActive.Tip()->GetBlockHash() : uint256(0)))
    {
        // Extra checks to prevent "fill up memory by spamming with bogus blocks"
        int64_t deltaTime = block.GetBlockTime() - pcheckpoint->nTime;
        if (deltaTime < 0)
        {
            return state.DoS(100, error("Credits: CheckBlockHeader() : block with timestamp before last checkpoint"),
                             BITCREDIT_REJECT_CHECKPOINT, "time-too-old");
        }
        bool fOverflow = false;
        uint256 bnNewBlock;
        bnNewBlock.SetCompact(block.nBits, NULL, &fOverflow);
        uint256 bnRequired;
        bnRequired.SetCompact(Bitcredit_ComputeMinWork(pcheckpoint->nHeight, pcheckpoint->nBits, deltaTime));
        if (fOverflow || bnNewBlock > bnRequired)
        {
            return state.DoS(100, error("Credits: CheckBlockHeader() : block with too little proof-of-work"),
                             BITCREDIT_REJECT_INVALID, "bad-diffbits");
        }
    }

    if(fCheckLinkedBitcoinBlock) {
    	if(!Credits_CheckLinkedBitcoinBlock(block, state)) {
            return state.DoS(100, error("Credits: CheckBlockHeader() : error when checking linked bitcoin block"),
                             BITCREDIT_REJECT_INVALID, "bad-linked-bitcoin-block");
    	}
    }

    return true;
}

bool Bitcredit_CheckBlock(const Credits_CBlock& block, CValidationState& state, bool fCheckPOW, bool fCheckMerkleRoot, bool fCheckLinkedBitcoinBlock)
{
    // These are checks that are independent of context
    // that can be verified before saving an orphan block.

    if (!Bitcredit_CheckBlockHeader(block, state, fCheckPOW, fCheckLinkedBitcoinBlock))
        return false;

    // Size limits
    if (block.vtx.empty() || block.vtx.size() > BITCREDIT_MAX_BLOCK_SIZE || ::GetSerializeSize(block, SER_NETWORK, CREDITS_PROTOCOL_VERSION) > BITCREDIT_MAX_BLOCK_SIZE)
        return state.DoS(100, error("Credits: CheckBlock() : size limits failed"),
                         BITCREDIT_REJECT_INVALID, "bad-blk-length");

    // First transaction must be coinbase, second deposit
    if (block.vtx.empty() || !block.vtx[0].IsValidCoinBase())
        return state.DoS(100, error("Credits: CheckBlock() : first tx is not valid coinbase"),
                         BITCREDIT_REJECT_INVALID, "bad-cb-missing");

    if (block.vtx.size() < 2 || !block.vtx[1].IsValidDeposit())
        return state.DoS(100, error("Credits: CheckBlock() : second tx is not valid deposit"),
                         BITCREDIT_REJECT_INVALID, "bad-dp-missing");

    //A maximum of ten deposits can be included in a block
    unsigned int i = 1;
    for (; i < block.vtx.size() && block.vtx[i].IsDeposit(); i++) {
    	unsigned int coinbaseRefCount = 0;

		if(!block.vtx[i].IsValidDeposit()) {
			return state.DoS(100, error("Credits: CheckBlock() : deposit at pos %d is not a valid deposit", i),
							 BITCREDIT_REJECT_INVALID, "bad-dp");
		}

		if(block.vtx[i].vin[0].prevout.hash == coinbaseRefCount) {
			coinbaseRefCount++;

			if(coinbaseRefCount > 1) {
				return state.DoS(100, error("Credits: CheckBlock() : only one deposit can spend coinbase, deposit at pos %d tries to double spend it", i),
								 BITCREDIT_REJECT_INVALID, "too-many-dp-cb-ref");
			}
		}

		if(i > CREDITS_MAX_NO_OF_DEPOSITS) {
			return state.DoS(100, error("Credits: CheckBlock() : max allowed number of deposits are %d", CREDITS_MAX_NO_OF_DEPOSITS),
							 BITCREDIT_REJECT_INVALID, "bad-to-many-dp");
		}
    }

    for (; i < block.vtx.size(); i++)
        if (!(block.vtx[i].IsStandard() || block.vtx[i].IsClaim()))
            return state.DoS(100, error("Credits: CheckBlock() : only tx type standard or type external bitcoin allowed"),
                             BITCREDIT_REJECT_INVALID, "bad-tx-type");

    // First vin of coinbase must have the block coinbase link hash set as it's prevout hash
    if (block.vtx[0].vin[0].prevout.hash != block.GetLockHash())
        return state.DoS(100, error("Credits: CheckBlock() : coinbase block link hash does not match link hash of block"),
                         BITCREDIT_REJECT_INVALID, "bad-block-link-hash");

    if (block.nDepositAmount != block.GetDepositAmount())
        return state.DoS(100, error("Credits: CheckBlock() : header total deposit in header is not same as amount from deposits"),
                         BITCREDIT_REJECT_INVALID, "bad-deposit-amount");

    // Check transactions
    BOOST_FOREACH(const Credits_CTransaction& tx, block.vtx)
        if (!Bitcredit_CheckTransaction(tx, state))
            return error("Credits: CheckBlock() : CheckTransaction failed");

    // Build the merkle tree already. We need it anyway later, and it makes the
    // block cache the transaction hashes, which means they don't need to be
    // recalculated many times during this block's validation.
    block.BuildMerkleTree();
    block.BuildSigMerkleTree();

    // Check for duplicate txids. This is caught by ConnectInputs(),
    // but catching it earlier avoids a potential DoS attack:
    set<uint256> uniqueTx;
    for (unsigned int i = 0; i < block.vtx.size(); i++) {
        uniqueTx.insert(block.GetTxHash(i));
    }
    if (uniqueTx.size() != block.vtx.size())
        return state.DoS(100, error("Credits: CheckBlock() : duplicate transaction"),
                         BITCREDIT_REJECT_INVALID, "bad-txns-duplicate", true);

    unsigned int nSigOps = 0;
    BOOST_FOREACH(const Credits_CTransaction& tx, block.vtx)
    {
        nSigOps += Bitcredit_GetLegacySigOpCount(tx);
    }
    if (nSigOps > BITCREDIT_MAX_BLOCK_SIGOPS)
        return state.DoS(100, error("Credits: CheckBlock() : out-of-bounds SigOpCount"),
                         BITCREDIT_REJECT_INVALID, "bad-blk-sigops", true);

    // Check merkle root
    if (fCheckMerkleRoot && block.hashMerkleRoot != block.vMerkleTree.back())
        return state.DoS(100, error("Credits: CheckBlock() : hashMerkleRoot mismatch"),
                         BITCREDIT_REJECT_INVALID, "bad-txnmrklroot", true);

    // Check sig merkle root
    if (fCheckMerkleRoot && block.hashSigMerkleRoot != block.vSigMerkleTree.back())
        return state.DoS(100, error("Credits: CheckBlock() : hashSigMerkleRoot mismatch"),
                         BITCREDIT_REJECT_INVALID, "bad-txnsigmrklroot", true);

    //Check that all deposit signatures are valid
    for (unsigned int i = 1; i < block.vtx.size(); i++) {
        if (!block.vtx[i].IsDeposit()) {
        	break;
        }

		//Verify signature against txMerkleRoot
		const CCompactSignature & sigDeposit = block.vsig[i-1];
		std::vector<unsigned char> vchsignature(sigDeposit.begin(), sigDeposit.end());
		CPubKey pubkey;
		const bool compactRecovered = pubkey.RecoverCompact(block.hashMerkleRoot, vchsignature);
		if(!compactRecovered) {
			return state.DoS(100, error("Credits: CheckBlock() : sig for deposit at position %d could not be recovered", i),
							 BITCREDIT_REJECT_INVALID, "bad-deposit-sig-unrecoverable", true);
		}

		if(pubkey.GetID() != block.vtx[i].signingKeyId) {
			return state.DoS(100, error("Credits: CheckBlock() : sig for deposit at position %d is not done by correct key", i),
							 BITCREDIT_REJECT_INVALID, "bad-deposit-sig-wrong-key", true);
		}
    }

    return true;
}

bool Bitcredit_AcceptBitcoinBlockLinkage(Credits_CBlockHeader& block, CValidationState& state) {
    LOCK(cs_main);
    unsigned int nLinkedBitcoinHeight;
    {
    	//Check that the referenced bitcoin block is deep enough in the bitcoin block chain
    	uint256 hashLinkedBitcoinBlock = block.hashLinkedBitcoinBlock;
    	const map<uint256, Bitcoin_CBlockIndex*>::iterator mi = bitcoin_mapBlockIndex.find(hashLinkedBitcoinBlock);
    	if (mi == bitcoin_mapBlockIndex.end()) {
    		return state.Invalid(error("Credits: AcceptBitcoinBlockLinkage() : Linked bitcoin block %s not found in bitcoin blockchain!", hashLinkedBitcoinBlock.GetHex()), 0, "invalidlink");
    	}
    	const Bitcoin_CBlockIndex* pLinkedBitcoinIndex = (*mi).second;
    	nLinkedBitcoinHeight = pLinkedBitcoinIndex->nHeight;
    	const Bitcoin_CBlockIndex * activeBlockAtHeight = bitcoin_chainActive[nLinkedBitcoinHeight];
    	if(*activeBlockAtHeight->phashBlock != *pLinkedBitcoinIndex->phashBlock) {
    		return state.Invalid(error("Credits: AcceptBitcoinBlockLinkage() : Referenced bitcoin block is not the same as block in active chain. Active chain has probably changed. Hashes are\n%s\n%s", activeBlockAtHeight->phashBlock->GetHex(), pLinkedBitcoinIndex->phashBlock->GetHex()), 0, "invalidlink");
    	}
    	const int nDepth = bitcoin_chainActive.Tip()->nHeight - nLinkedBitcoinHeight;
    	if(nDepth < Credits_Params().AcceptDepthLinkedBitcoinBlock()) {
    		return state.Invalid(error("Credits: AcceptBitcoinBlockLinkage() : Linked bitcoin block %s is not deep enough in the bitcoin blockchain! Depth must be at least %d and is %d", hashLinkedBitcoinBlock.GetHex(), Credits_Params().AcceptDepthLinkedBitcoinBlock(), nDepth), 0, "invalidlink");
    	}
    }

	//Check that this referenced bitcoin block isn't lower in the chain than the previous referenced bitcoin block (genesis block does not have any previous block)
    if(block.GetHash() != Credits_Params().GenesisBlock().GetHash()) {
		unsigned int nPrevLinkedBitcoinHeight;
		{
			const map<uint256, Credits_CBlockIndex*>::iterator mi = credits_mapBlockIndex.find(block.hashPrevBlock);
			if (mi == credits_mapBlockIndex.end()) {
				return state.Invalid(error("Credits: AcceptBitcoinBlockLinkage() : Previous credits block %s not found in credits blockchain", block.hashPrevBlock.GetHex()), 0, "invalidlink");
			}
			const Credits_CBlockIndex* pPrevBlock = (*mi).second;

			uint256 hashLinkedBitcoinBlock = pPrevBlock->hashLinkedBitcoinBlock;
			const map<uint256, Bitcoin_CBlockIndex*>::iterator miPrev = bitcoin_mapBlockIndex.find(hashLinkedBitcoinBlock);
			if (miPrev == bitcoin_mapBlockIndex.end()) {
				return state.Invalid(error("Credits: AcceptBitcoinBlockLinkage() : Linked bitcoin block %s not found in bitcoin blockchain", hashLinkedBitcoinBlock.GetHex()), 0, "invalidlink");
			}
			const Bitcoin_CBlockIndex* pPrevLinkedBitcoinIndex = (*miPrev).second;
			nPrevLinkedBitcoinHeight = pPrevLinkedBitcoinIndex->nHeight;
		}

		if(nLinkedBitcoinHeight < nPrevLinkedBitcoinHeight) {
			return state.Invalid(error("Credits: AcceptBitcoinBlockLinkage() : Linked bitcoin block height %d is lower than previous linked bitcoin block height %d", nLinkedBitcoinHeight, nPrevLinkedBitcoinHeight), 0, "invalidlink");
		}
    }

	return true;
}

bool Bitcredit_AcceptBlockHeader(Credits_CBlockHeader& block, CValidationState& state, Credits_CBlockIndex** ppindex)
{
    AssertLockHeld(cs_main);

    // Check for duplicate
    uint256 hash = block.GetHash();
    std::map<uint256, Credits_CBlockIndex*>::iterator miSelf = credits_mapBlockIndex.find(hash);
    Credits_CBlockIndex *pindex = NULL;
    if (miSelf != credits_mapBlockIndex.end()) {
        pindex = miSelf->second;
        if (pindex->nStatus & BLOCK_FAILED_MASK)
            return state.Invalid(error("Credits: AcceptBlock() : block is marked invalid"), 0, "duplicate");
    }

    if(!Bitcredit_AcceptBitcoinBlockLinkage(block, state)) {
    	return false;
    }

    // Get prev block index
    Credits_CBlockIndex* pindexPrev = NULL;
    int nHeight = 0;
    if (hash != Credits_Params().HashGenesisBlock()) {
        map<uint256, Credits_CBlockIndex*>::iterator mi = credits_mapBlockIndex.find(block.hashPrevBlock);
        if (mi == credits_mapBlockIndex.end())
            return state.DoS(10, error("Credits: AcceptBlock() : prev block not found"), 0, "bad-prevblk");
        pindexPrev = (*mi).second;
        nHeight = pindexPrev->nHeight+1;

        // Check proof of work
        if (block.nBits != Bitcredit_GetNextWorkRequired(pindexPrev, &block))
            return state.DoS(100, error("Credits: AcceptBlock() : incorrect proof of work"),
                             BITCREDIT_REJECT_INVALID, "bad-diffbits");

        // Check timestamp against prev
        if (block.GetBlockTime() <= pindexPrev->GetMedianTimePast())
            return state.Invalid(error("Credits: AcceptBlock() : block's timestamp is too early"),
                                 BITCREDIT_REJECT_INVALID, "time-too-old");

        // Check that the block chain matches the known block chain up to a checkpoint
        if (!Checkpoints::Bitcredit_CheckBlock(nHeight, hash))
            return state.DoS(100, error("Credits: AcceptBlock() : rejected by checkpoint lock-in at %d", nHeight),
                             BITCREDIT_REJECT_CHECKPOINT, "checkpoint mismatch");

        // Don't accept any forks from the main chain prior to last checkpoint
        Credits_CBlockIndex* pcheckpoint = Checkpoints::Bitcredit_GetLastCheckpoint(credits_mapBlockIndex);
        if (pcheckpoint && nHeight < pcheckpoint->nHeight)
            return state.DoS(100, error("Credits: AcceptBlock() : forked chain older than last checkpoint (height %d)", nHeight));

        // Reject block.nVersion=1 blocks when 95% (75% on testnet) of the network has upgraded:
//        if (block.nVersion < 2)
//        {
//            if ((!Bitcredit_TestNet() && Credits_CBlockIndex::IsSuperMajority(2, pindexPrev, 950, 1000)) ||
//                (Bitcredit_TestNet() && Credits_CBlockIndex::IsSuperMajority(2, pindexPrev, 75, 100)))
//            {
//                return state.Invalid(error("AcceptBlock() : rejected nVersion=1 block"),
//                                     BITCREDIT_REJECT_OBSOLETE, "bad-version");
//            }
//        }
    }

    if (pindex == NULL)
        pindex = Bitcredit_AddToBlockIndex(block);

    if (ppindex)
        *ppindex = pindex;

    return true;
}

bool Bitcredit_AcceptBlock(Credits_CBlock& block, CValidationState& state, Credits_CBlockIndex** ppindex, CDiskBlockPos* dbp, CNetParams * netParams)
{
    AssertLockHeld(cs_main);

    Credits_CBlockIndex *&pindex = *ppindex;

    if (!Bitcredit_AcceptBlockHeader(block, state, &pindex))
        return false;

    if (!Bitcredit_CheckBlock(block, state)) {
        if (state.Invalid() && !state.CorruptionPossible()) {
            pindex->nStatus |= BLOCK_FAILED_VALID;
        }
        return false;
    }

    int nHeight = pindex->nHeight;
    uint256 hash = pindex->GetBlockHash();

    // Check that all transactions are finalized
    BOOST_FOREACH(const Credits_CTransaction& tx, block.vtx)
        if (!Credits_IsFinalTx(tx, nHeight, block.GetBlockTime())) {
            pindex->nStatus |= BLOCK_FAILED_VALID;
            return state.DoS(10, error("Credits: AcceptBlock() : contains a non-final transaction"),
                             BITCREDIT_REJECT_INVALID, "bad-txns-nonfinal");
        }

    // Write block to history file
    try {
        unsigned int nBlockSize = ::GetSerializeSize(block, SER_DISK, netParams->ClientVersion());
        CDiskBlockPos blockPos;
        if (dbp != NULL)
            blockPos = *dbp;
        if (!Bitcredit_FindBlockPos(state, blockPos, nBlockSize+8, nHeight, block.nTime, dbp != NULL))
            return error("Credits: AcceptBlock() : Bitcredit_FindBlockPos failed");
        if (dbp == NULL)
            if (!Bitcredit_WriteBlockToDisk(block, blockPos))
                return state.Abort(_("Failed to write block"));
        if (!Bitcredit_ReceivedBlockTransactions(block, state, pindex, blockPos))
            return error("Credits: AcceptBlock() : ReceivedBlockTransactions failed");
    } catch(std::runtime_error &e) {
        return state.Abort(_("System error: ") + e.what());
    }

    // Relay inventory, but don't relay old inventory during initial block download
    int nBlockEstimate = Checkpoints::Bitcredit_GetTotalBlocksEstimate();
    if (credits_chainActive.Tip()->GetBlockHash() == hash)
    {
        LOCK(netParams->cs_vNodes);
        BOOST_FOREACH(CNode* pnode, netParams->vNodes)
            if (credits_chainActive.Height() > (pnode->nStartingHeight != -1 ? pnode->nStartingHeight - 2000 : nBlockEstimate))
                pnode->PushInventory(CInv(MSG_BLOCK, hash));
    }

    return true;
}

bool Credits_CBlockIndex::IsSuperMajority(int minVersion, const Credits_CBlockIndex* pstart, unsigned int nRequired, unsigned int nToCheck)
{
    unsigned int nFound = 0;
    for (unsigned int i = 0; i < nToCheck && nFound < nRequired && pstart != NULL; i++)
    {
        if (pstart->nVersion >= minVersion)
            ++nFound;
        pstart = (Credits_CBlockIndex*)pstart->pprev;
    }
    return (nFound >= nRequired);
}

void Bitcredit_PushGetBlocks(CNode* pnode, Credits_CBlockIndex* pindexBegin, uint256 hashEnd)
{
    AssertLockHeld(cs_main);
    // Filter out duplicate requests
    if (pindexBegin == pnode->pindexLastGetBlocksBegin && hashEnd == pnode->hashLastGetBlocksEnd)
        return;
    pnode->pindexLastGetBlocksBegin = pindexBegin;
    pnode->hashLastGetBlocksEnd = hashEnd;

    pnode->PushMessage("getblocks", credits_chainActive.GetLocator(pindexBegin), hashEnd);
}

bool Credits_ProcessOrphans(const uint256 &hashInit)
{
    // Recursively process any orphan blocks that depended on this one
    vector<uint256> vWorkQueue;
    vWorkQueue.push_back(hashInit);
    for (unsigned int i = 0; i < vWorkQueue.size(); i++)
    {
        uint256 hashPrev = vWorkQueue[i];
        std::vector <COrphanBlock*>deleteOrphans;
        for (multimap<uint256, COrphanBlock*>::iterator mi = credits_orphanIndex.mapOrphanBlocksByPrev.lower_bound(hashPrev);
             mi != credits_orphanIndex.mapOrphanBlocksByPrev.upper_bound(hashPrev);
             ++mi)
        {
            Credits_CBlock block;
            {
            	if(mi->second->fStoredInMemory) {
					CDataStream ss(mi->second->vchBlock, SER_DISK, CREDITS_CLIENT_VERSION);
					ss >> block;
            	} else {
					if(!Credits_ReadOrphanFromDisk(mi->second->hashBlock, block)) {
						LogPrintf("Credits_ProcessOrphans() : Read orphaned block from disk FAILED for %s!\n", mi->second->hashBlock.GetHex());

						credits_orphanIndex.RemoveOrpan(mi->second);
						//Gather all connected orphans for later deletion
						deleteOrphans.push_back(mi->second);
						delete mi->second;

						continue;
					}
            	}
            }
            block.BuildMerkleTree();
            block.BuildSigMerkleTree();

            CValidationState stateDummy;
            //If we have a correctly linked bitcoin block
            if(Credits_CheckLinkedBitcoinBlock(block, stateDummy)) {
				// Use a dummy CValidationState so someone can't setup nodes to counter-DoS based on orphan resolution (that is, feeding people an invalid block based on LegitBlockX in order to get anyone relaying LegitBlockX banned)
				Credits_CBlockIndex *pindexChild = NULL;
				if (Bitcredit_AcceptBlock(block, stateDummy, &pindexChild, NULL, Credits_NetParams()))
					vWorkQueue.push_back(mi->second->hashBlock);

				credits_orphanIndex.RemoveOrpan(mi->second);
				//Gather all connected orphans for later deletion
				deleteOrphans.push_back(mi->second);
				delete mi->second;
            }
        }

		//Delete all gathered orphans
        credits_orphanIndex.DeletePrevPartial(hashPrev, deleteOrphans);
    }
    return true;
}

void Credits_ProcessBitcoinLinkedOprhans(vector<Bitcoin_CBlockIndex*> &linkedBitcoinBlocks) {
	if(credits_orphanIndex.mapOrphanBlocksByLinkedBitcoinBlock.size() == 0) {
		return;
	}

	//Used to stop when no more orphan blocks are removed by processing them
	BOOST_FOREACH(const Bitcoin_CBlockIndex* pLinkedBitcoinBlock, linkedBitcoinBlocks) {
		//Find all orphan roots and if their previous block exists in the block index
		const uint256 hashLinkedBlock = pLinkedBitcoinBlock->GetBlockHash();
		if(credits_orphanIndex.mapOrphanBlocksByLinkedBitcoinBlock.count(hashLinkedBlock) > 0) {
			std::set<uint256> hashOrphanRootPrevs;

			//Find the orphan roots for any loose chains that may exist, index on those roots previous block
			for (multimap<uint256, COrphanBlock*>::iterator mi = credits_orphanIndex.mapOrphanBlocksByLinkedBitcoinBlock.lower_bound(hashLinkedBlock);
				 mi != credits_orphanIndex.mapOrphanBlocksByLinkedBitcoinBlock.upper_bound(hashLinkedBlock); ++mi) {
				const uint256 hashPrev = credits_orphanIndex.mapOrphanBlocks[credits_orphanIndex.GetOrphanRoot(mi->second->hashBlock)]->hashPrev;
				if(credits_mapBlockIndex.count(hashPrev) && hashOrphanRootPrevs.count(hashPrev) == 0) {
					hashOrphanRootPrevs.insert(hashPrev);
				}
			}

			//Trigger orphan processing for all blocks that have been identified as having non-processed blocks
			BOOST_FOREACH(const uint256& hash, hashOrphanRootPrevs) {
				if(!Credits_ProcessOrphans(hash)) {
					//Do not fail for credits linking, only error log
					LogPrintf("Credits_ProcessByLinkedBitcoinBlock() : ERROR: Processing of orphan block FAILED!\nBlock hash is %s\n", hash.GetHex());
				}
			}
		}
	}
}

bool Bitcredit_ProcessBlock(CValidationState &state, CNode* pfrom, Credits_CBlock* pblock, CDiskBlockPos *dbp)
{
    AssertLockHeld(cs_main);

    // Check for duplicate
    uint256 hash = pblock->GetHash();
    if (credits_mapBlockIndex.count(hash))
        return state.Invalid(error("Credits: ProcessBlock() : already have block %d %s", credits_mapBlockIndex[hash]->nHeight, hash.ToString()), 0, "duplicate");
    if (credits_orphanIndex.mapOrphanBlocks.count(hash)) {
        //If the block passed in validates with regards to bitcoin link and previous accepted, try accepting it
        if(Credits_CheckLinkedBitcoinBlock(*pblock, state) && credits_mapBlockIndex.count(pblock->hashPrevBlock) > 0) {
        	//Change the hash to refer to previous block instead
        	hash = pblock->hashPrevBlock;
        } else {
        	return state.Invalid(error("Credits: ProcessBlock() : already have block (orphan) %s", hash.ToString()), 0, "duplicate");
        }
    }

    //If the block we should process from hasn't changed up until here, proceed with block accept
    //If it has changed, hash refers to the previously already accepted parent block
    if(hash == pblock->GetHash()) {
		// Preliminary checks
		if (!Bitcredit_CheckBlock(*pblock, state, true, true, false))
			return error("Credits: ProcessBlock() : CheckBlock FAILED");

		//If we have an incorrectly linked bitcoin block
		const bool fIncorrectLinkedBitcoinBlock = !Credits_CheckLinkedBitcoinBlock(*pblock, state);
		// If we don't already have its previous block (with full data), shunt it off to holding area until we get it
		std::map<uint256, Credits_CBlockIndex*>::iterator it = credits_mapBlockIndex.find(pblock->hashPrevBlock);
		const bool fPreviousMissing = pblock->hashPrevBlock != 0 && (it == credits_mapBlockIndex.end() || !(it->second->nStatus & BLOCK_HAVE_DATA));
		if (fIncorrectLinkedBitcoinBlock || fPreviousMissing)
		{
			LogPrintf("Credits: ProcessBlock: ORPHAN BLOCK %lu, prev=%s\n", (unsigned long)credits_orphanIndex.mapOrphanBlocks.size(), pblock->hashPrevBlock.ToString());

			// Accept orphans as long as there is a node to request its parents from
			if (pfrom) {
				const int64_t nMaxOrphansMemory = GetArg("-maxorphanblocksmemory", BITCREDIT_DEFAULT_MAX_ORPHAN_BLOCKS_MEMORY);
				const int64_t nMaxOrphansDisk = GetArg("-maxorphanblocksdisk", BITCREDIT_DEFAULT_MAX_ORPHAN_BLOCKS_DISK);
				credits_orphanIndex.PruneOrphanBlocks(nMaxOrphansMemory, nMaxOrphansDisk);
				COrphanBlock* pblock2 = new COrphanBlock();
				{
					if(credits_orphanIndex.nStoredInMemory < nMaxOrphansMemory) {
						CDataStream ss(SER_DISK, CREDITS_CLIENT_VERSION);
						ss << *pblock;
						pblock2->vchBlock = std::vector<unsigned char>(ss.begin(), ss.end());
						pblock2->fStoredInMemory = true;
						credits_orphanIndex.nStoredInMemory++;
					} else {
						if(!Credits_WriteOrphanToDisk(*pblock, pfrom)) {
							return error(strprintf("Credits: ProcessBlock() : FAILED to write orphan %s to disk", pblock->GetHash().GetHex()));
						}
						pblock2->fStoredInMemory = false;
					}
				}
				pblock2->hashBlock = hash;
				pblock2->hashPrev = pblock->hashPrevBlock;
				pblock2->hashLinkedBitcoinBlock = pblock->hashLinkedBitcoinBlock;

				credits_orphanIndex.mapOrphanBlocks.insert(make_pair(hash, pblock2));
				credits_orphanIndex.mapOrphanBlocksByPrev.insert(make_pair(pblock2->hashPrev, pblock2));
				credits_orphanIndex.mapOrphanBlocksByLinkedBitcoinBlock.insert(make_pair(pblock2->hashLinkedBitcoinBlock, pblock2));

				if(fPreviousMissing) {
					// Ask this guy to fill in what we're missing
					Bitcredit_PushGetBlocks(pfrom, (Credits_CBlockIndex*)credits_chainActive.Tip(), credits_orphanIndex.GetOrphanRoot(hash));
				}
			}
			return true;
		}

		// Store to disk
		Credits_CBlockIndex *pindex = NULL;
		if(!Bitcredit_AcceptBlock(*pblock, state, &pindex, dbp, Credits_NetParams()))
			return error("Credits: ProcessBlock() : AcceptBlock FAILED");
    }

    if(!Credits_ProcessOrphans(hash))
        return error("Credits: ProcessBlock() : ProcessOrphans FAILED");

    LogPrintf("Credits: ProcessBlock: ACCEPTED\n");
    return true;
}








Credits_CMerkleBlock::Credits_CMerkleBlock(const Credits_CBlock& block, CBloomFilter& filter)
{
    header = block.GetBlockHeader();

    vector<bool> vMatch;
    vector<uint256> vHashes;

    vMatch.reserve(block.vtx.size());
    vHashes.reserve(block.vtx.size());

    for (unsigned int i = 0; i < block.vtx.size(); i++)
    {
        uint256 hash = block.vtx[i].GetHash();
        if (filter.bitcredit_IsRelevantAndUpdate(block.vtx[i], hash))
        {
            vMatch.push_back(true);
            vMatchedTxn.push_back(make_pair(i, hash));
        }
        else
            vMatch.push_back(false);
        vHashes.push_back(hash);
    }

    txn = CPartialMerkleTree(vHashes, vMatch);
}






FILE* Credits_OpenBlockFile(const CDiskBlockPos &pos, bool fReadOnly) {
    return OpenDiskFile(pos, "credits_blocks", "blk", fReadOnly);
}

FILE* Credits_OpenUndoFile(const CDiskBlockPos &pos, bool fReadOnly) {
    return OpenDiskFile(pos, "credits_blocks", "rev", fReadOnly);
}

Credits_CBlockIndex * Credits_InsertBlockIndex(uint256 hash)
{
    if (hash == 0)
        return NULL;

    // Return existing
    map<uint256, Credits_CBlockIndex*>::iterator mi = credits_mapBlockIndex.find(hash);
    if (mi != credits_mapBlockIndex.end())
        return (*mi).second;

    // Create new
    Credits_CBlockIndex* pindexNew = new Credits_CBlockIndex();
    if (!pindexNew)
        throw runtime_error("Credits: LoadBlockIndex() : new CBlockIndex failed");
    mi = credits_mapBlockIndex.insert(make_pair(hash, pindexNew)).first;
    pindexNew->phashBlock = &((*mi).first);

    return pindexNew;
}

bool static Credits_LoadBlockIndexDB()
{
    if (!bitcredit_pblocktree->LoadBlockIndexGuts())
        return false;

    boost::this_thread::interruption_point();

    // Calculate nChainWork
    vector<pair<int, Credits_CBlockIndex*> > vSortedByHeight;
    vSortedByHeight.reserve(credits_mapBlockIndex.size());
    BOOST_FOREACH(const PAIRTYPE(uint256, Credits_CBlockIndex*)& item, credits_mapBlockIndex)
    {
        Credits_CBlockIndex* pindex = item.second;
        vSortedByHeight.push_back(make_pair(pindex->nHeight, pindex));
    }
    sort(vSortedByHeight.begin(), vSortedByHeight.end());
    BOOST_FOREACH(const PAIRTYPE(int, Credits_CBlockIndex*)& item, vSortedByHeight)
    {
        Credits_CBlockIndex* pindex = item.second;
        pindex->nChainWork = (pindex->pprev ? pindex->pprev->nChainWork : 0) + pindex->GetBlockWork();
        pindex->nChainTx = (pindex->pprev ? pindex->pprev->nChainTx : 0) + pindex->nTx;
        if (pindex->IsValid(BLOCK_VALID_TRANSACTIONS))
            bitcredit_setBlockIndexValid.insert(pindex);
        if (pindex->nStatus & BLOCK_FAILED_MASK && (!bitcredit_pindexBestInvalid || pindex->nChainWork > bitcredit_pindexBestInvalid->nChainWork))
            bitcredit_pindexBestInvalid = pindex;
    }

    // Load block file info
    bitcredit_pblocktree->ReadLastBlockFile(credits_mainState.nLastBlockFile);
    LogPrintf("Credits: LoadBlockIndexDB(): last block file = %i\n", credits_mainState.nLastBlockFile);
    if (bitcredit_pblocktree->ReadBlockFileInfo(credits_mainState.nLastBlockFile, credits_mainState.infoLastBlockFile))
        LogPrintf("Credits: LoadBlockIndexDB(): last block file info: %s\n", credits_mainState.infoLastBlockFile.ToString());

    // Check presence of blk files
    LogPrintf("Credits: Checking all blk files are present...\n");
    set<int> setBlkDataFiles;
    BOOST_FOREACH(const PAIRTYPE(uint256, Credits_CBlockIndex*)& item, credits_mapBlockIndex)
    {
    	Credits_CBlockIndex* pindex = item.second;
        if (pindex->nStatus & BLOCK_HAVE_DATA) {
            setBlkDataFiles.insert(pindex->nFile);
        }
    }
    for (std::set<int>::iterator it = setBlkDataFiles.begin(); it != setBlkDataFiles.end(); it++)
    {
        CDiskBlockPos pos(*it, 0);
        if (!CAutoFile(Credits_OpenBlockFile(pos, true), SER_DISK, Credits_Params().ClientVersion())) {
            return false;
        }
    }

    // Check whether we need to continue reindexing
    bool fReindexing = false;
    bitcredit_pblocktree->ReadReindexing(fReindexing);
    credits_mainState.fReindex |= fReindexing;

    // Check whether we have a transaction index
    bitcredit_pblocktree->ReadFlag("txindex", bitcredit_fTxIndex);
    LogPrintf("Credits: LoadBlockIndexDB(): transaction index %s\n", bitcredit_fTxIndex ? "enabled" : "disabled");

    // Load pointer to end of best chain
    std::map<uint256, Credits_CBlockIndex*>::iterator it = credits_mapBlockIndex.find(credits_pcoinsTip->Credits_GetBestBlock());
    if (it == credits_mapBlockIndex.end())
        return true;
    credits_chainActive.SetTip(it->second);
    LogPrintf("Credits: LoadBlockIndexDB(): hashBestChain=%s height=%d date=%s progress=%f\n",
        credits_chainActive.Tip()->GetBlockHash().ToString(), credits_chainActive.Height(),
        DateTimeStrFormat("%Y-%m-%d %H:%M:%S", credits_chainActive.Tip()->GetBlockTime()),
        Checkpoints::Credits_GuessVerificationProgress((Credits_CBlockIndex*)credits_chainActive.Tip()));

    return true;
}

Credits_CVerifyDB::Credits_CVerifyDB()
{
    uiInterface.ShowProgress(_("Credits: Verifying blocks..."), 0);
}

Credits_CVerifyDB::~Credits_CVerifyDB()
{
    uiInterface.ShowProgress("", 100);
}

bool Credits_CVerifyDB::VerifyDB(int nCheckLevel, int nCheckDepth)
{
    LOCK(cs_main);
    if (credits_chainActive.Tip() == NULL || credits_chainActive.Tip()->pprev == NULL)
        return true;

    // Verify blocks in the best chain
    if (nCheckDepth <= 0)
        nCheckDepth = 1000000000; // suffices until the year 19000
    if (nCheckDepth > credits_chainActive.Height())
        nCheckDepth = credits_chainActive.Height();
    nCheckLevel = std::max(0, std::min(4, nCheckLevel));
    LogPrintf("Credits: Verifying last %i blocks at level %i\n", nCheckDepth, nCheckLevel);

	//This is a throw away chainstate db + undo vector
	Credits_CCoinsViewCache bitcredit_coins(*credits_pcoinsTip, true);
	std::vector<pair<Bitcoin_CBlockIndex*, Bitcoin_CBlockUndoClaim> > vBlockUndoClaims;
	Credits_CBlockIndex* pindexState = (Credits_CBlockIndex*)credits_chainActive.Tip();
	Credits_CBlockIndex* pindexFailure = NULL;
	int nGoodTransactions = 0;
	CValidationState state;
	for (Credits_CBlockIndex* pindex = (Credits_CBlockIndex*)credits_chainActive.Tip(); pindex && pindex->pprev; pindex = (Credits_CBlockIndex*)pindex->pprev)
	{
		boost::this_thread::interruption_point();
		uiInterface.ShowProgress(_("Credits: Verifying blocks..."), std::max(1, std::min(99, (int)(((double)(credits_chainActive.Height() - pindex->nHeight)) / (double)nCheckDepth * (nCheckLevel >= 4 ? 50 : 100)))));
		if (pindex->nHeight < credits_chainActive.Height()-nCheckDepth)
			break;
		Credits_CBlock block;
		// check level 0: read from disk
		if (!Credits_ReadBlockFromDisk(block, pindex))
			return error("Credits: VerifyDB() : *** ReadBlockFromDisk failed at %d, hash=%s", pindex->nHeight, pindex->GetBlockHash().ToString());
		// check level 1: verify block validity
		if (nCheckLevel >= 1 && !Bitcredit_CheckBlock(block, state))
			return error("Credits: VerifyDB() : *** found bad block at %d, hash=%s\n", pindex->nHeight, pindex->GetBlockHash().ToString());
		// check level 2: verify undo validity
		if (nCheckLevel >= 2 && pindex) {
			Credits_CBlockUndo undo;
			CDiskBlockPos pos = pindex->GetUndoPos();
			if (!pos.IsNull()) {
				if (!undo.ReadFromDisk(pos, pindex->pprev->GetBlockHash(), Credits_NetParams()))
					return error("Credits: VerifyDB() : *** found bad undo data at %d, hash=%s\n", pindex->nHeight, pindex->GetBlockHash().ToString());
			}
		}
		// check level 3: check for inconsistencies during memory-only disconnect of tip blocks
		if (nCheckLevel >= 3 &&
				pindex == pindexState &&
				((bitcredit_coins.GetCacheSize() + credits_pcoinsTip->GetCacheSize()) <= 2*bitcredit_nCoinCacheSize + 32000)) {
			bool fClean = true;
			if (!Bitcredit_DisconnectBlock(block, state, pindex, bitcredit_coins, false, vBlockUndoClaims, &fClean))
				return error("Credits: VerifyDB() : *** irrecoverable inconsistency in block data at %d, hash=%s", pindex->nHeight, pindex->GetBlockHash().ToString());
			pindexState = (Credits_CBlockIndex*)pindex->pprev;
			if (!fClean) {
				nGoodTransactions = 0;
				pindexFailure = pindex;
			} else
				nGoodTransactions += block.vtx.size();
		}
	}
	if (pindexFailure)
		return error("Credits: VerifyDB() : *** coin database inconsistencies found (last %i blocks, %i good transactions before that)\n", credits_chainActive.Height() - pindexFailure->nHeight + 1, nGoodTransactions);

	// check level 4: try reconnecting blocks
	if (nCheckLevel >= 4) {
		Credits_CBlockIndex *pindex = pindexState;
		while (pindex != credits_chainActive.Tip()) {
			boost::this_thread::interruption_point();
			uiInterface.ShowProgress(_("Credits: Verifying blocks..."), std::max(1, std::min(99, 100 - (int)(((double)(credits_chainActive.Height() - pindex->nHeight)) / (double)nCheckDepth * 50))));
			pindex = credits_chainActive.Next(pindex);
			Credits_CBlock block;
			if (!Credits_ReadBlockFromDisk(block, pindex))
				return error("Credits: VerifyDB() : *** ReadBlockFromDisk failed at %d, hash=%s", pindex->nHeight, pindex->GetBlockHash().ToString());
			if (!Bitcredit_ConnectBlock(block, state, pindex, bitcredit_coins, false, vBlockUndoClaims, false))
				return error("Credits: VerifyDB() : *** found unconnectable block at %d, hash=%s", pindex->nHeight, pindex->GetBlockHash().ToString());
		}
	}
	LogPrintf("Credits: No coin database inconsistencies in last %i blocks (%i transactions)\n", credits_chainActive.Height() - pindexState->nHeight, nGoodTransactions);

    return true;
}

void Credits_UnloadBlockIndex()
{
    credits_mapBlockIndex.clear();
    bitcredit_setBlockIndexValid.clear();
    credits_chainActive.SetTip(NULL);
    bitcredit_pindexBestInvalid = NULL;
}

bool Credits_LoadBlockIndex()
{
    // Load block index from databases
    if (!credits_mainState.fReindex && !Credits_LoadBlockIndexDB())
        return false;
    return true;
}


bool Credits_InitBlockIndex() {
    LOCK(cs_main);
    // Check whether we're already initialized
    if (credits_chainActive.Genesis() != NULL)
        return true;

    // Use the provided setting for -txindex in the new database
    bitcredit_fTxIndex = GetBoolArg("-txindex", false);
    bitcredit_pblocktree->WriteFlag("txindex", bitcredit_fTxIndex);
    LogPrintf("Credits: Initializing databases...\n");

    // Only add the genesis block if not reindexing (in which case we reuse the one already on disk)
    if (!credits_mainState.fReindex) {
        try {
            Credits_CBlock &block = const_cast<Credits_CBlock&>(Credits_Params().GenesisBlock());
            // Start new block file
            unsigned int nBlockSize = ::GetSerializeSize(block, SER_DISK, CREDITS_CLIENT_VERSION);
            CDiskBlockPos blockPos;
            CValidationState state;
            if (!Bitcredit_FindBlockPos(state, blockPos, nBlockSize+8, 0, block.nTime))
                return error("Credits: LoadBlockIndex() : Bitcredit_FindBlockPos failed");
            if (!Bitcredit_WriteBlockToDisk(block, blockPos))
                return error("Credits: LoadBlockIndex() : writing genesis block to disk failed");
            Credits_CBlockIndex *pindex = Bitcredit_AddToBlockIndex(block);
            if (!Bitcredit_ReceivedBlockTransactions(block, state, pindex, blockPos))
                return error("Credits: LoadBlockIndex() : genesis block not accepted");
        } catch(std::runtime_error &e) {
            return error("Credits: LoadBlockIndex() : failed to initialize block database: %s", e.what());
        }
    }

    return true;
}



void Bitcredit_PrintBlockTree()
{
    AssertLockHeld(cs_main);
    // pre-compute tree structure
    map<Credits_CBlockIndex*, vector<Credits_CBlockIndex*> > mapNext;
    for (map<uint256, Credits_CBlockIndex*>::iterator mi = credits_mapBlockIndex.begin(); mi != credits_mapBlockIndex.end(); ++mi)
    {
        Credits_CBlockIndex* pindex = (*mi).second;
        mapNext[(Credits_CBlockIndex*)pindex->pprev].push_back(pindex);
        // test
        //while (rand() % 3 == 0)
        //    mapNext[pindex->pprev].push_back(pindex);
    }

    vector<pair<int, Credits_CBlockIndex*> > vStack;
    vStack.push_back(make_pair(0, credits_chainActive.Genesis()));

    int nPrevCol = 0;
    while (!vStack.empty())
    {
        int nCol = vStack.back().first;
        Credits_CBlockIndex* pindex = vStack.back().second;
        vStack.pop_back();

        // print split or gap
        if (nCol > nPrevCol)
        {
            for (int i = 0; i < nCol-1; i++)
                LogPrintf("| ");
            LogPrintf("|\\\n");
        }
        else if (nCol < nPrevCol)
        {
            for (int i = 0; i < nCol; i++)
                LogPrintf("| ");
            LogPrintf("|\n");
       }
        nPrevCol = nCol;

        // print columns
        for (int i = 0; i < nCol; i++)
            LogPrintf("| ");

        // print item
        Credits_CBlock block;
        Credits_ReadBlockFromDisk(block, pindex);
        LogPrintf("%d (blk%05u.dat:0x%x)  %s  tx %u\n",
            pindex->nHeight,
            pindex->GetBlockPos().nFile, pindex->GetBlockPos().nPos,
            DateTimeStrFormat("%Y-%m-%d %H:%M:%S", block.GetBlockTime()),
            block.vtx.size());

        // put the main time-chain first
        vector<Credits_CBlockIndex*>& vNext = mapNext[pindex];
        for (unsigned int i = 0; i < vNext.size(); i++)
        {
            if (credits_chainActive.Next(vNext[i]))
            {
                swap(vNext[0], vNext[i]);
                break;
            }
        }

        // iterate children
        for (unsigned int i = 0; i < vNext.size(); i++)
            vStack.push_back(make_pair(nCol+i, vNext[i]));
    }
}

bool Bitcredit_LoadExternalBlockFile(FILE* fileIn, CDiskBlockPos *dbp)
{
    int64_t nStart = GetTimeMillis();

    int nLoaded = 0;
    try {
        CBufferedFile blkdat(fileIn, 2*BITCREDIT_MAX_BLOCK_SIZE, BITCREDIT_MAX_BLOCK_SIZE+8, SER_DISK, CREDITS_CLIENT_VERSION);
        uint64_t nStartByte = 0;
        if (dbp) {
            // (try to) skip already indexed part
        	CBlockFileInfo info;
            if (bitcredit_pblocktree->ReadBlockFileInfo(dbp->nFile, info)) {
                nStartByte = info.nSize;
                blkdat.Seek(info.nSize);
            }
        }
        uint64_t nRewind = blkdat.GetPos();
        while (blkdat.good() && !blkdat.eof()) {
            boost::this_thread::interruption_point();

            blkdat.SetPos(nRewind);
            nRewind++; // start one byte further next time, in case of failure
            blkdat.SetLimit(); // remove former limit
            unsigned int nSize = 0;
            try {
                // locate a header
                unsigned char buf[MESSAGE_START_SIZE];
                blkdat.FindByte(Credits_Params().MessageStart()[0]);
                nRewind = blkdat.GetPos()+1;
                blkdat >> FLATDATA(buf);
                if (memcmp(buf, Credits_Params().MessageStart(), MESSAGE_START_SIZE))
                    continue;
                // read size
                blkdat >> nSize;
                if (nSize < 80 || nSize > BITCREDIT_MAX_BLOCK_SIZE)
                    continue;
            } catch (std::exception &e) {
                // no valid block header found; don't complain
                break;
            }
            try {
                // read block
                uint64_t nBlockPos = blkdat.GetPos();
                blkdat.SetLimit(nBlockPos + nSize);
                Credits_CBlock block;
                blkdat >> block;
                nRewind = blkdat.GetPos();

                // process block
                if (nBlockPos >= nStartByte) {
                    LOCK(cs_main);
                    if (dbp)
                        dbp->nPos = nBlockPos;
                    CValidationState state;
                    if (Bitcredit_ProcessBlock(state, NULL, &block, dbp))
                        nLoaded++;
                    if (state.IsError())
                        break;
                }
            } catch (std::exception &e) {
                LogPrintf("ERROR: Bitcredit_LoadExternalBlockFile: %s : Deserialize or I/O error - %s\n", __func__, e.what());
            }
        }
        fclose(fileIn);
    } catch(std::runtime_error &e) {
        AbortNode(_("Error: system error: ") + e.what());
    }
    if (nLoaded > 0)
        LogPrintf("Credits: Loaded %i blocks from external file in %dms\n", nLoaded, GetTimeMillis() - nStart);
    return nLoaded > 0;
}










//////////////////////////////////////////////////////////////////////////////
//
// CAlert
//

string Bitcredit_GetWarnings(string strFor)
{
    int nPriority = 0;
    string strStatusBar;
    string strRPC;

    if (GetBoolArg("-testsafemode", false))
        strRPC = "test";

    if (!CLIENT_VERSION_IS_RELEASE)
        strStatusBar = _("This is a pre-release test build - use at your own risk - do not use for mining or merchant applications");

    // Misc warnings like out of disk space and clock is wrong
    if (strMiscWarning != "")
    {
        nPriority = 1000;
        strStatusBar = strMiscWarning;
    }

    if (bitcredit_fLargeWorkForkFound)
    {
        nPriority = 2000;
        strStatusBar = strRPC = _("Warning: The network does not appear to fully agree! Some miners appear to be experiencing issues.");
    }
    else if (bitcredit_fLargeWorkInvalidChainFound)
    {
        nPriority = 2000;
        strStatusBar = strRPC = _("Warning: We do not appear to fully agree with our peers! You may need to upgrade, or other nodes may need to upgrade.");
    }

    // Alerts
    {
        LOCK(cs_mapAlerts);
        BOOST_FOREACH(PAIRTYPE(const uint256, CAlert)& item, mapAlerts)
        {
            const CAlert& alert = item.second;
            if (alert.AppliesToMe() && alert.nPriority > nPriority)
            {
                nPriority = alert.nPriority;
                strStatusBar = alert.strStatusBar;
            }
        }
    }

    if (strFor == "statusbar")
        return strStatusBar;
    else if (strFor == "rpc")
        return strRPC;
    assert(!"GetWarnings() : invalid parameter");
    return "error";
}








//////////////////////////////////////////////////////////////////////////////
//
// Messages
//


bool static Bitcredit_AlreadyHave(const CInv& inv)
{
    switch (inv.type)
    {
    case MSG_TX:
        {
            bool txInMap = false;
            txInMap = credits_mempool.exists(inv.hash);
            return txInMap || bitcredit_mapOrphanTransactions.count(inv.hash) ||
                credits_pcoinsTip->Credits_HaveCoins(inv.hash);
        }
    case MSG_BLOCK:
        return credits_mapBlockIndex.count(inv.hash) ||
               credits_orphanIndex.mapOrphanBlocks.count(inv.hash);
    }
    // Don't know what it is, just say we already got one
    return true;
}


void static Bitcredit_ProcessGetData(CNode* pfrom)
{
    std::deque<CInv>::iterator it = pfrom->vRecvGetData.begin();

    vector<CInv> vNotFound;

    LOCK(cs_main);

    while (it != pfrom->vRecvGetData.end()) {
        // Don't bother if send buffer is too full to respond anyway
        if (pfrom->nSendSize >= SendBufferSize())
            break;

        const CInv &inv = *it;
        {
            boost::this_thread::interruption_point();
            it++;

            if (inv.type == MSG_BLOCK || inv.type == MSG_FILTERED_BLOCK)
            {
                bool send = false;
                map<uint256, Credits_CBlockIndex*>::iterator mi = credits_mapBlockIndex.find(inv.hash);
                if (mi != credits_mapBlockIndex.end())
                {
                    // If the requested block is at a height below our last
                    // checkpoint, only serve it if it's in the checkpointed chain
                    int nHeight = mi->second->nHeight;
                    Credits_CBlockIndex* pcheckpoint = Checkpoints::Bitcredit_GetLastCheckpoint(credits_mapBlockIndex);
                    if (pcheckpoint && nHeight < pcheckpoint->nHeight) {
                        if (!credits_chainActive.Contains(mi->second))
                        {
                            LogPrintf("Credits: ProcessGetData(): ignoring request for old block that isn't in the main chain\n");
                        } else {
                            send = true;
                        }
                    } else {
                        send = true;
                    }
                }
                if (send)
                {
                    // Send block from disk
                    Credits_CBlock block;
                    if (!Credits_ReadBlockFromDisk(block, (*mi).second))
                        assert(!"cannot load block from disk");
                    if (inv.type == MSG_BLOCK)
                        pfrom->PushMessage("block", block);
                    else // MSG_FILTERED_BLOCK)
                    {
                        LOCK(pfrom->cs_filter);
                        if (pfrom->pfilter)
                        {
                        	Credits_CMerkleBlock merkleBlock(block, *pfrom->pfilter);
                            pfrom->PushMessage("merkleblock", merkleBlock);
                            // CMerkleBlock just contains hashes, so also push any transactions in the block the client did not see
                            // This avoids hurting performance by pointlessly requiring a round-trip
                            // Note that there is currently no way for a node to request any single transactions we didnt send here -
                            // they must either disconnect and retry or request the full block.
                            // Thus, the protocol spec specified allows for us to provide duplicate txn here,
                            // however we MUST always provide at least what the remote peer needs
                            typedef std::pair<unsigned int, uint256> PairType;
                            BOOST_FOREACH(PairType& pair, merkleBlock.vMatchedTxn)
                                if (!pfrom->setInventoryKnown.count(CInv(MSG_TX, pair.second)))
                                    pfrom->PushMessage("tx", block.vtx[pair.first]);
                        }
                        // else
                            // no response
                    }

                    // Trigger them to send a getblocks request for the next batch of inventory
                    if (inv.hash == pfrom->hashContinue)
                    {
                        // Bypass PushInventory, this must send even if redundant,
                        // and we want it right after the last block so they don't
                        // wait for other stuff first.
                        vector<CInv> vInv;
                        vInv.push_back(CInv(MSG_BLOCK, credits_chainActive.Tip()->GetBlockHash()));
                        pfrom->PushMessage("inv", vInv);
                        pfrom->hashContinue = 0;
                    }
                }
            }
            else if (inv.IsKnownType())
            {
                // Send stream from relay memory
                bool pushed = false;
                {
                	CNetParams * netParams = pfrom->netParams;
                    LOCK(netParams->cs_mapRelay);
                    map<CInv, CDataStream>::iterator mi = netParams->mapRelay.find(inv);
                    if (mi != netParams->mapRelay.end()) {
                        pfrom->PushMessage(inv.GetCommand(), (*mi).second);
                        pushed = true;
                    }
                }
                if (!pushed && inv.type == MSG_TX) {
                    Credits_CTransaction tx;
                    if (credits_mempool.lookup(inv.hash, tx)) {
                        CDataStream ss(SER_NETWORK, CREDITS_PROTOCOL_VERSION);
                        ss.reserve(1000);
                        ss << tx;
                        pfrom->PushMessage("tx", ss);
                        pushed = true;
                    }
                }
                if (!pushed) {
                    vNotFound.push_back(inv);
                }
            }

            // Track requests for our stuff.
            bitcredit_g_signals.Inventory(inv.hash);

            if (inv.type == MSG_BLOCK || inv.type == MSG_FILTERED_BLOCK)
                break;
        }
    }

    pfrom->vRecvGetData.erase(pfrom->vRecvGetData.begin(), it);

    if (!vNotFound.empty()) {
        // Let the peer know that we didn't find what it asked for, so it doesn't
        // have to wait around forever. Currently only SPV clients actually care
        // about this message: it's needed when they are recursively walking the
        // dependencies of relevant unconfirmed transactions. SPV clients want to
        // do that because they want to know about (and store and rebroadcast and
        // risk analyze) the dependencies of transactions relevant to them, without
        // having to download the entire memory pool.
        pfrom->PushMessage("notfound", vNotFound);
    }
}

bool static Bitcredit_ProcessMessage(CMessageHeader& hdr, CNode* pfrom, string strCommand, CDataStream& vRecv, CNetParams * netParams)
{
    RandAddSeedPerfmon();
    LogPrint(netParams->DebugCategory(), "Credits: received: %s (%u bytes)\n", strCommand, vRecv.size());
    if (mapArgs.count("-dropmessagestest") && GetRand(atoi(mapArgs["-dropmessagestest"])) == 0)
    {
        LogPrintf("Credits: dropmessagestest DROPPING RECV MESSAGE\n");
        return true;
    }

    Bitcredit_State(pfrom->GetId())->nLastBlockProcess = GetTimeMicros();



    if (strCommand == "version")
    {
        // Each connection can only send one version message
        if (pfrom->nVersion != 0)
        {
            pfrom->PushMessage("reject", strCommand, BITCREDIT_REJECT_DUPLICATE, string("Duplicate version message"));
            Bitcredit_Misbehaving(pfrom->GetId(), 1);
            return false;
        }

        int64_t nTime;
        CAddress addrMe;
        CAddress addrFrom;
        uint64_t nNonce = 1;
        vRecv >> pfrom->nVersion >> pfrom->nServices >> nTime >> addrMe;
        if (pfrom->nVersion < MIN_PEER_PROTO_VERSION)
        {
            // disconnect from peers older than this proto version
            LogPrintf("Credits: partner %s using obsolete version %i; disconnecting\n", pfrom->addr.ToString(), pfrom->nVersion);
            pfrom->PushMessage("reject", strCommand, BITCREDIT_REJECT_OBSOLETE,
                               strprintf("Version must be %d or greater", MIN_PEER_PROTO_VERSION));
            pfrom->fDisconnect = true;
            return false;
        }

        if (pfrom->nVersion == 10300)
            pfrom->nVersion = 300;
        if (!vRecv.empty())
            vRecv >> addrFrom >> nNonce;
        if (!vRecv.empty()) {
            vRecv >> pfrom->strSubVer;
            pfrom->cleanSubVer = SanitizeString(pfrom->strSubVer);
        }
        if (!vRecv.empty())
            vRecv >> pfrom->nStartingHeight;
        if (!vRecv.empty())
            vRecv >> pfrom->fRelayTxes; // set to true after we get the first filter* message
        else
            pfrom->fRelayTxes = true;

        if (pfrom->fInbound && addrMe.IsRoutable())
        {
            pfrom->addrLocal = addrMe;
            SeenLocal(addrMe, netParams);
        }

        // Disconnect if we connected to ourself
        if (nNonce == netParams->nLocalHostNonce && nNonce > 1)
        {
            LogPrintf("Credits: connected to self at %s, disconnecting\n", pfrom->addr.ToString());
            pfrom->fDisconnect = true;
            return true;
        }

        // Be shy and don't send version until we hear
        if (pfrom->fInbound)
            pfrom->PushVersion();

        pfrom->fClient = !(pfrom->nServices & NODE_NETWORK);


        // Change version
        pfrom->PushMessage("verack");
        pfrom->ssSend.SetVersion(min(pfrom->nVersion, CREDITS_PROTOCOL_VERSION));

        if (!pfrom->fInbound)
        {
            // Advertise our address
            if (netParams->fListen && !Bitcredit_IsInitialBlockDownload())
            {
                CAddress addr = GetLocalAddress(&pfrom->addr, netParams);
                if (addr.IsRoutable())
                    pfrom->PushAddress(addr);
            }

            // Get recent addresses
            if (pfrom->fOneShot || pfrom->nVersion >= CADDR_TIME_VERSION || netParams->addrman.size() < 1000)
            {
                pfrom->PushMessage("getaddr");
                pfrom->fGetAddr = true;
            }
            netParams->addrman.Good(pfrom->addr);
        } else {
            if (((CNetAddr)pfrom->addr) == (CNetAddr)addrFrom)
            {
            	netParams->addrman.Add(addrFrom, addrFrom);
            	netParams->addrman.Good(addrFrom);
            }
        }

        // Relay alerts
        {
            LOCK(cs_mapAlerts);
            BOOST_FOREACH(PAIRTYPE(const uint256, CAlert)& item, mapAlerts)
                item.second.RelayTo(pfrom);
        }

        pfrom->fSuccessfullyConnected = true;

        LogPrintf("Credits: receive version message: %s: version %d, blocks=%d, us=%s, them=%s, peer=%s\n", pfrom->cleanSubVer, pfrom->nVersion, pfrom->nStartingHeight, addrMe.ToString(), addrFrom.ToString(), pfrom->addr.ToString());

        AddTimeData(pfrom->addr, nTime);
    }


    else if (pfrom->nVersion == 0)
    {
        // Must have a version message before anything else
        Bitcredit_Misbehaving(pfrom->GetId(), 1);
        return false;
    }


    else if (strCommand == "verack")
    {
        pfrom->SetRecvVersion(min(pfrom->nVersion, CREDITS_PROTOCOL_VERSION));
    }


    else if (strCommand == "addr")
    {
        vector<CAddress> vAddr;
        vRecv >> vAddr;

        // Don't want addr from older versions unless seeding
        if (pfrom->nVersion < CADDR_TIME_VERSION && netParams->addrman.size() > 1000)
            return true;
        if (vAddr.size() > 1000)
        {
            Bitcredit_Misbehaving(pfrom->GetId(), 20);
            return error("Credits: message addr size() = %u", vAddr.size());
        }

        // Store the new addresses
        vector<CAddress> vAddrOk;
        int64_t nNow = GetAdjustedTime();
        int64_t nSince = nNow - 10 * 60;
        BOOST_FOREACH(CAddress& addr, vAddr)
        {
            boost::this_thread::interruption_point();

            if (addr.nTime <= 100000000 || addr.nTime > nNow + 10 * 60)
                addr.nTime = nNow - 5 * 24 * 60 * 60;
            pfrom->AddAddressKnown(addr);
            bool fReachable = IsReachable(addr, netParams);
            if (addr.nTime > nSince && !pfrom->fGetAddr && vAddr.size() <= 10 && addr.IsRoutable())
            {
                // Relay to a limited number of other nodes
                {
                    LOCK(netParams->cs_vNodes);
                    // Use deterministic randomness to send to the same nodes for 24 hours
                    // at a time so the setAddrKnowns of the chosen nodes prevent repeats
                    static uint256 hashSalt;
                    if (hashSalt == 0)
                        hashSalt = GetRandHash();
                    uint64_t hashAddr = addr.GetHash();
                    uint256 hashRand = hashSalt ^ (hashAddr<<32) ^ ((GetTime()+hashAddr)/(24*60*60));
                    hashRand = Hash(BEGIN(hashRand), END(hashRand));
                    multimap<uint256, CNode*> mapMix;
                    BOOST_FOREACH(CNode* pnode, netParams->vNodes)
                    {
                        if (pnode->nVersion < CADDR_TIME_VERSION)
                            continue;
                        unsigned int nPointer;
                        memcpy(&nPointer, &pnode, sizeof(nPointer));
                        uint256 hashKey = hashRand ^ nPointer;
                        hashKey = Hash(BEGIN(hashKey), END(hashKey));
                        mapMix.insert(make_pair(hashKey, pnode));
                    }
                    int nRelayNodes = fReachable ? 2 : 1; // limited relaying of addresses outside our network(s)
                    for (multimap<uint256, CNode*>::iterator mi = mapMix.begin(); mi != mapMix.end() && nRelayNodes-- > 0; ++mi)
                        ((*mi).second)->PushAddress(addr);
                }
            }
            // Do not store addresses outside our network
            if (fReachable)
                vAddrOk.push_back(addr);
        }
        netParams->addrman.Add(vAddrOk, pfrom->addr, 2 * 60 * 60);
        if (vAddr.size() < 1000)
            pfrom->fGetAddr = false;
        if (pfrom->fOneShot)
            pfrom->fDisconnect = true;
    }


    else if (strCommand == "inv")
    {
        vector<CInv> vInv;
        vRecv >> vInv;
        if (vInv.size() > MAX_INV_SZ)
        {
            Bitcredit_Misbehaving(pfrom->GetId(), 20);
            return error("Credits: message inv size() = %u", vInv.size());
        }

        LOCK(cs_main);

        for (unsigned int nInv = 0; nInv < vInv.size(); nInv++)
        {
            const CInv &inv = vInv[nInv];

            boost::this_thread::interruption_point();
            pfrom->AddInventoryKnown(inv);

            bool fAlreadyHave = Bitcredit_AlreadyHave(inv);
            LogPrint(netParams->DebugCategory(), "Credits:   got inventory: %s  %s\n", inv.ToString(), fAlreadyHave ? "have" : "new");

            if (!fAlreadyHave) {
                if (!credits_mainState.ImportingOrReindexing()) {
                    if (inv.type == MSG_BLOCK)
                        Bitcredit_AddBlockToQueue(pfrom->GetId(), inv.hash);
                    else
                        pfrom->AskFor(inv);
                }
            } else if (inv.type == MSG_BLOCK && credits_orphanIndex.mapOrphanBlocks.count(inv.hash)) {
                Bitcredit_PushGetBlocks(pfrom, (Credits_CBlockIndex*)credits_chainActive.Tip(), credits_orphanIndex.GetOrphanRoot(inv.hash));
            }

            // Track requests for our stuff
            bitcredit_g_signals.Inventory(inv.hash);
        }
    }


    else if (strCommand == "getdata")
    {
        vector<CInv> vInv;
        vRecv >> vInv;
        if (vInv.size() > MAX_INV_SZ)
        {
            Bitcredit_Misbehaving(pfrom->GetId(), 20);
            return error("Credits: message getdata size() = %u", vInv.size());
        }

        if (fDebug || (vInv.size() != 1))
            LogPrint(netParams->DebugCategory(), "Credits: received getdata (%u invsz)\n", vInv.size());

        if ((fDebug && vInv.size() > 0) || (vInv.size() == 1))
            LogPrint(netParams->DebugCategory(), "Credits: received getdata for: %s\n", vInv[0].ToString());

        pfrom->vRecvGetData.insert(pfrom->vRecvGetData.end(), vInv.begin(), vInv.end());
        Bitcredit_ProcessGetData(pfrom);
    }


    else if (strCommand == "getblocks")
    {
        CBlockLocator locator;
        uint256 hashStop;
        vRecv >> locator >> hashStop;

        LOCK(cs_main);

        // Find the last block the caller has in the main chain
        Credits_CBlockIndex* pindex = credits_chainActive.FindFork(locator);

        // Send the rest of the chain
        if (pindex)
            pindex = credits_chainActive.Next(pindex);
        int nLimit = 500;
        LogPrint(netParams->DebugCategory(), "Credits: getblocks %d to %s limit %d\n", (pindex ? pindex->nHeight : -1), hashStop.ToString(), nLimit);
        for (; pindex; pindex = credits_chainActive.Next(pindex))
        {
            if (pindex->GetBlockHash() == hashStop)
            {
                LogPrint(netParams->DebugCategory(), "Credits:   getblocks stopping at %d %s\n", pindex->nHeight, pindex->GetBlockHash().ToString());
                break;
            }
            pfrom->PushInventory(CInv(MSG_BLOCK, pindex->GetBlockHash()));
            if (--nLimit <= 0)
            {
                // When this block is requested, we'll send an inv that'll make them
                // getblocks the next batch of inventory.
                LogPrint(netParams->DebugCategory(), "Credits:   getblocks stopping at limit %d %s\n", pindex->nHeight, pindex->GetBlockHash().ToString());
                pfrom->hashContinue = pindex->GetBlockHash();
                break;
            }
        }
    }


    else if (strCommand == "getheaders")
    {
        CBlockLocator locator;
        uint256 hashStop;
        vRecv >> locator >> hashStop;

        LOCK(cs_main);

        Credits_CBlockIndex* pindex = NULL;
        if (locator.IsNull())
        {
            // If locator is null, return the hashStop block
            map<uint256, Credits_CBlockIndex*>::iterator mi = credits_mapBlockIndex.find(hashStop);
            if (mi == credits_mapBlockIndex.end())
                return true;
            pindex = (*mi).second;
        }
        else
        {
            // Find the last block the caller has in the main chain
            pindex = credits_chainActive.FindFork(locator);
            if (pindex)
                pindex = credits_chainActive.Next(pindex);
        }

        // we must use CBlocks, as CBlockHeaders won't include the 0x00 nTx count at the end
        vector<Credits_CBlock> vHeaders;
        int nLimit = 2000;
        LogPrint(netParams->DebugCategory(), "Credits: getheaders %d to %s\n", (pindex ? pindex->nHeight : -1), hashStop.ToString());
        for (; pindex; pindex = credits_chainActive.Next(pindex))
        {
            vHeaders.push_back(pindex->GetBlockHeader());
            if (--nLimit <= 0 || pindex->GetBlockHash() == hashStop)
                break;
        }
        pfrom->PushMessage("headers", vHeaders);
    }


    else if (strCommand == "tx")
    {
        vector<uint256> vWorkQueue;
        vector<uint256> vEraseQueue;
        Credits_CTransaction tx;
        vRecv >> tx;

        CInv inv(MSG_TX, tx.GetHash());
        pfrom->AddInventoryKnown(inv);

        LOCK(cs_main);

        bool fMissingInputs = false;
        CValidationState state;
        if (Bitcredit_AcceptToMemoryPool(credits_mempool, state, tx, true, &fMissingInputs))
        {
            credits_mempool.check(credits_pcoinsTip);
            Credits_RelayTransaction(tx, inv.hash, netParams);
            pfrom->netParams->mapAlreadyAskedFor.erase(inv);
            vWorkQueue.push_back(inv.hash);
            vEraseQueue.push_back(inv.hash);


            LogPrint("mempool", "AcceptToMemoryPool: %s %s : accepted %s (poolsz %u)\n",
                pfrom->addr.ToString(), pfrom->cleanSubVer,
                tx.GetHash().ToString(),
                credits_mempool.mapTx.size());

            // Recursively process any orphan transactions that depended on this one
            for (unsigned int i = 0; i < vWorkQueue.size(); i++)
            {
                uint256 hashPrev = vWorkQueue[i];
                for (set<uint256>::iterator mi = bitcredit_mapOrphanTransactionsByPrev[hashPrev].begin();
                     mi != bitcredit_mapOrphanTransactionsByPrev[hashPrev].end();
                     ++mi)
                {
                    const uint256& orphanHash = *mi;
                    const Credits_CTransaction& orphanTx = bitcredit_mapOrphanTransactions[orphanHash];
                    bool fMissingInputs2 = false;
                    // Use a dummy CValidationState so someone can't setup nodes to counter-DoS based on orphan
                    // resolution (that is, feeding people an invalid transaction based on LegitTxX in order to get
                    // anyone relaying LegitTxX banned)
                    CValidationState stateDummy;

                    if (Bitcredit_AcceptToMemoryPool(credits_mempool, stateDummy, orphanTx, true, &fMissingInputs2))
                    {
                        LogPrint("mempool", "   accepted orphan tx %s\n", orphanHash.ToString());
                        Credits_RelayTransaction(orphanTx, orphanHash, netParams);
                        pfrom->netParams->mapAlreadyAskedFor.erase(CInv(MSG_TX, orphanHash));
                        vWorkQueue.push_back(orphanHash);
                        vEraseQueue.push_back(orphanHash);
                    }
                    else if (!fMissingInputs2)
                    {
                        // invalid or too-little-fee orphan
                        vEraseQueue.push_back(orphanHash);
                        LogPrint("mempool", "   removed orphan tx %s\n", orphanHash.ToString());
                    }
                    credits_mempool.check(credits_pcoinsTip);
                }
            }

            BOOST_FOREACH(uint256 hash, vEraseQueue)
                Bitcredit_EraseOrphanTx(hash);
        }
        else if (fMissingInputs)
        {
        	Bitcredit_AddOrphanTx(tx);

            // DoS prevention: do not allow mapOrphanTransactions to grow unbounded
            unsigned int nEvicted = Bitcredit_LimitOrphanTxSize(BITCREDIT_MAX_ORPHAN_TRANSACTIONS);
            if (nEvicted > 0)
                LogPrint("mempool", "mapOrphan overflow, removed %u tx\n", nEvicted);
        }
        int nDoS = 0;
        if (state.IsInvalid(nDoS))
        {
            LogPrint("mempool", "%s from %s %s was not accepted into the memory pool: %s\n", tx.GetHash().ToString(),
                pfrom->addr.ToString(), pfrom->cleanSubVer,
                state.GetRejectReason());
            pfrom->PushMessage("reject", strCommand, state.GetRejectCode(),
                               state.GetRejectReason(), inv.hash);
            if (nDoS > 0)
                Bitcredit_Misbehaving(pfrom->GetId(), nDoS);
        }
    }


    else if (strCommand == "block" && !credits_mainState.ImportingOrReindexing()) // Ignore blocks received while importing
    {
        Credits_CBlock block;
        vRecv >> block;

        LogPrint(netParams->DebugCategory(), "Credits: received block %s\n", block.GetHash().ToString());
        // block.print();

        CInv inv(MSG_BLOCK, block.GetHash());
        pfrom->AddInventoryKnown(inv);

        LOCK(cs_main);
        // Remember who we got this block from.
        bitcredit_mapBlockSource[inv.hash] = pfrom->GetId();
        Bitcredit_MarkBlockAsReceived(inv.hash, pfrom->GetId());

        CValidationState state;
        Bitcredit_ProcessBlock(state, pfrom, &block);
    }


    else if (strCommand == "getaddr")
    {
        pfrom->vAddrToSend.clear();
        vector<CAddress> vAddr = netParams->addrman.GetAddr();
        BOOST_FOREACH(const CAddress &addr, vAddr)
            pfrom->PushAddress(addr);
    }


    else if (strCommand == "mempool")
    {
        LOCK2(cs_main, pfrom->cs_filter);

        std::vector<uint256> vtxid;
        credits_mempool.queryHashes(vtxid);
        vector<CInv> vInv;
        BOOST_FOREACH(uint256& hash, vtxid) {
            CInv inv(MSG_TX, hash);
            Credits_CTransaction tx;
            bool fInMemPool = credits_mempool.lookup(hash, tx);
            if (!fInMemPool) continue; // another thread removed since queryHashes, maybe...

            if ((pfrom->pfilter && pfrom->pfilter->bitcredit_IsRelevantAndUpdate(tx, hash)) ||
               (!pfrom->pfilter))
                vInv.push_back(inv);
            if (vInv.size() == MAX_INV_SZ) {
                pfrom->PushMessage("inv", vInv);
                vInv.clear();
            }
        }
        if (vInv.size() > 0)
            pfrom->PushMessage("inv", vInv);
    }


    else if (strCommand == "ping")
    {
        if (pfrom->nVersion > BIP0031_VERSION)
        {
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
            pfrom->PushMessage("pong", nonce);
        }
    }


    else if (strCommand == "pong")
    {
        int64_t pingUsecEnd = GetTimeMicros();
        uint64_t nonce = 0;
        size_t nAvail = vRecv.in_avail();
        bool bPingFinished = false;
        std::string sProblem;

        if (nAvail >= sizeof(nonce)) {
            vRecv >> nonce;

            // Only process pong message if there is an outstanding ping (old ping without nonce should never pong)
            if (pfrom->nPingNonceSent != 0) {
                if (nonce == pfrom->nPingNonceSent) {
                    // Matching pong received, this ping is no longer outstanding
                    bPingFinished = true;
                    int64_t pingUsecTime = pingUsecEnd - pfrom->nPingUsecStart;
                    if (pingUsecTime > 0) {
                        // Successful ping time measurement, replace previous
                        pfrom->nPingUsecTime = pingUsecTime;
                    } else {
                        // This should never happen
                        sProblem = "Timing mishap";
                    }
                } else {
                    // Nonce mismatches are normal when pings are overlapping
                    sProblem = "Nonce mismatch";
                    if (nonce == 0) {
                        // This is most likely a bug in another implementation somewhere, cancel this ping
                        bPingFinished = true;
                        sProblem = "Nonce zero";
                    }
                }
            } else {
                sProblem = "Unsolicited pong without ping";
            }
        } else {
            // This is most likely a bug in another implementation somewhere, cancel this ping
            bPingFinished = true;
            sProblem = "Short payload";
        }

        if (!(sProblem.empty())) {
            LogPrint(netParams->DebugCategory(), "Credits: pong %s %s: %s, %x expected, %x received, %u bytes\n",
                pfrom->addr.ToString(),
                pfrom->cleanSubVer,
                sProblem,
                pfrom->nPingNonceSent,
                nonce,
                nAvail);
        }
        if (bPingFinished) {
            pfrom->nPingNonceSent = 0;
        }
    }


    else if (strCommand == "alert")
    {
        CAlert alert;
        vRecv >> alert;

        uint256 alertHash = alert.GetHash();
        if (pfrom->setKnown.count(alertHash) == 0)
        {
            if (alert.ProcessAlert())
            {
                // Relay
                pfrom->setKnown.insert(alertHash);
                {
                    LOCK(netParams->cs_vNodes);
                    BOOST_FOREACH(CNode* pnode, netParams->vNodes)
                        alert.RelayTo(pnode);
                }
            }
            else {
                // Small DoS penalty so peers that send us lots of
                // duplicate/expired/invalid-signature/whatever alerts
                // eventually get banned.
                // This isn't a Misbehaving(100) (immediate ban) because the
                // peer might be an older or different implementation with
                // a different signature key, etc.
                Bitcredit_Misbehaving(pfrom->GetId(), 10);
            }
        }
    }


    else if (strCommand == "filterload")
    {
        CBloomFilter filter;
        vRecv >> filter;

        if (!filter.IsWithinSizeConstraints())
            // There is no excuse for sending a too-large filter
            Bitcredit_Misbehaving(pfrom->GetId(), 100);
        else
        {
            LOCK(pfrom->cs_filter);
            delete pfrom->pfilter;
            pfrom->pfilter = new CBloomFilter(filter);
            pfrom->pfilter->UpdateEmptyFull();
        }
        pfrom->fRelayTxes = true;
    }


    else if (strCommand == "filteradd")
    {
        vector<unsigned char> vData;
        vRecv >> vData;

        // Nodes must NEVER send a data item > 520 bytes (the max size for a script data object,
        // and thus, the maximum size any matched object can have) in a filteradd message
        if (vData.size() > MAX_SCRIPT_ELEMENT_SIZE)
        {
            Bitcredit_Misbehaving(pfrom->GetId(), 100);
        } else {
            LOCK(pfrom->cs_filter);
            if (pfrom->pfilter)
                pfrom->pfilter->insert(vData);
            else
                Bitcredit_Misbehaving(pfrom->GetId(), 100);
        }
    }


    else if (strCommand == "filterclear")
    {
        LOCK(pfrom->cs_filter);
        delete pfrom->pfilter;
        pfrom->pfilter = new CBloomFilter();
        pfrom->fRelayTxes = true;
    }


    else if (strCommand == "reject")
    {
        if (fDebug)
        {
            string strMsg; unsigned char ccode; string strReason;
            vRecv >> strMsg >> ccode >> strReason;

            ostringstream ss;
            ss << strMsg << " code " << itostr(ccode) << ": " << strReason;

            if (strMsg == "block" || strMsg == "tx")
            {
                uint256 hash;
                vRecv >> hash;
                ss << ": hash " << hash.ToString();
            }
            // Truncate to reasonable length and sanitize before printing:
            string s = ss.str();
            if (s.size() > 111) s.erase(111, string::npos);
            LogPrint(netParams->DebugCategory(), "Credits: Reject %s\n", SanitizeString(s));
        }
    }

	//Do nothing, possible to extend
    else
    {
    }


    // Update the last seen time for this node's address
    if (pfrom->fNetworkNode)
        if (strCommand == "version" || strCommand == "addr" || strCommand == "inv" || strCommand == "getdata" || strCommand == "ping")
            netParams->addrman.AddressCurrentlyConnected(pfrom->addr);


    return true;
}

// requires LOCK(cs_vRecvMsg)
bool Bitcredit_ProcessMessages(CNode* pfrom)
{
	CNetParams * netParams = pfrom->netParams;

    //if (fDebug)
    //    LogPrintf("Credits: ProcessMessages(%u messages)\n", pfrom->vRecvMsg.size());

    //
    // Message format
    //  (4) message start
    //  (12) command
    //  (4) size
    //  (4) checksum
    //  (x) data
    //
    bool fOk = true;

    if (!pfrom->vRecvGetData.empty())
        Bitcredit_ProcessGetData(pfrom);

    // this maintains the order of responses
    if (!pfrom->vRecvGetData.empty()) return fOk;

    std::deque<CNetMessage>::iterator it = pfrom->vRecvMsg.begin();
    while (!pfrom->fDisconnect && it != pfrom->vRecvMsg.end()) {
        // Don't bother if send buffer is too full to respond anyway
        if (pfrom->nSendSize >= SendBufferSize())
            break;

        // get next message
        CNetMessage& msg = *it;

        //if (fDebug)
        //    LogPrintf("Credits: ProcessMessages(message %u msgsz, %u bytes, complete:%s)\n",
        //            msg.hdr.nMessageSize, msg.vRecv.size(),
        //            msg.complete() ? "Y" : "N");

        // end, if an incomplete message is found
        if (!msg.complete())
            break;

        // at this point, any failure means we can delete the current message
        it++;

        // Scan for message start
        if (memcmp(msg.hdr.pchMessageStart, netParams->MessageStart(), MESSAGE_START_SIZE) != 0) {
            LogPrintf("\n\nBITCREDIT: PROCESSMESSAGE: INVALID MESSAGESTART\n\n");
            fOk = false;
            break;
        }

        // Read header
        CMessageHeader& hdr = msg.hdr;
        if (!hdr.IsValid())
        {
            LogPrintf("\n\nBITCREDIT: PROCESSMESSAGE: ERRORS IN HEADER %s\n\n\n", hdr.GetCommand());
            continue;
        }
        string strCommand = hdr.GetCommand();

        // Message size
        unsigned int nMessageSize = hdr.nMessageSize;

        // Checksum
        CDataStream& vRecv = msg.vRecv;
        uint256 hash = Hash(vRecv.begin(), vRecv.begin() + nMessageSize);
        unsigned int nChecksum = 0;
        memcpy(&nChecksum, &hash, sizeof(nChecksum));
        if (nChecksum != hdr.nChecksum)
        {
            LogPrintf("Credits: ProcessMessages(%s, %u bytes) : CHECKSUM ERROR nChecksum=%08x hdr.nChecksum=%08x\n",
               strCommand, nMessageSize, nChecksum, hdr.nChecksum);
            continue;
        }

        // Process message
        bool fRet = false;
        try
        {
            fRet = Bitcredit_ProcessMessage(hdr, pfrom, strCommand, vRecv, pfrom->GetNetParams());
            boost::this_thread::interruption_point();
        }
        catch (std::ios_base::failure& e)
        {
            pfrom->PushMessage("reject", strCommand, BITCREDIT_REJECT_MALFORMED, string("error parsing message"));
            if (strstr(e.what(), "end of data"))
            {
                // Allow exceptions from under-length message on vRecv
                LogPrintf("Credits: ProcessMessages(%s, %u bytes) : Exception '%s' caught, normally caused by a message being shorter than its stated length\n", strCommand, nMessageSize, e.what());
            }
            else if (strstr(e.what(), "size too large"))
            {
                // Allow exceptions from over-long size
                LogPrintf("Credits: ProcessMessages(%s, %u bytes) : Exception '%s' caught\n", strCommand, nMessageSize, e.what());
            }
            else
            {
                PrintExceptionContinue(&e, "ProcessMessages()");
            }
        }
        catch (boost::thread_interrupted) {
            throw;
        }
        catch (std::exception& e) {
            PrintExceptionContinue(&e, "ProcessMessages()");
        } catch (...) {
            PrintExceptionContinue(NULL, "ProcessMessages()");
        }

        if (!fRet)
            LogPrintf("Credits: ProcessMessage(%s, %u bytes) FAILED\n", strCommand, nMessageSize);

        break;
    }

    // In case the connection got shut down, its receive buffer was wiped
    if (!pfrom->fDisconnect)
        pfrom->vRecvMsg.erase(pfrom->vRecvMsg.begin(), it);

    return fOk;
}


bool Bitcredit_SendMessages(CNode* pto, bool fSendTrickle)
{
	CNetParams * netParams = pto->netParams;

    {
        // Don't send anything until we get their version message
        if (pto->nVersion == 0)
            return true;

        //
        // Message: ping
        //
        bool pingSend = false;
        if (pto->fPingQueued) {
            // RPC ping request by user
            pingSend = true;
        }
        if (pto->nLastSend && GetTime() - pto->nLastSend > 30 * 60 && pto->vSendMsg.empty()) {
            // Ping automatically sent as a keepalive
            pingSend = true;
        }
        if (pingSend) {
            uint64_t nonce = 0;
            while (nonce == 0) {
                RAND_bytes((unsigned char*)&nonce, sizeof(nonce));
            }
            pto->nPingNonceSent = nonce;
            pto->fPingQueued = false;
            if (pto->nVersion > BIP0031_VERSION) {
                // Take timestamp as close as possible before transmitting ping
                pto->nPingUsecStart = GetTimeMicros();
                pto->PushMessage("ping", nonce);
            } else {
                // Peer is too old to support ping command with nonce, pong will never arrive, disable timing
                pto->nPingUsecStart = 0;
                pto->PushMessage("ping");
            }
        }

        TRY_LOCK(cs_main, lockMain); // Acquire cs_main for IsInitialBlockDownload() and CNodeState()
        if (!lockMain)
            return true;

        // Address refresh broadcast
        static int64_t nLastRebroadcast;
        if (!Bitcredit_IsInitialBlockDownload() && (GetTime() - nLastRebroadcast > 24 * 60 * 60))
        {
            {
                LOCK(netParams->cs_vNodes);
                BOOST_FOREACH(CNode* pnode, netParams->vNodes)
                {
                    // Periodically clear setAddrKnown to allow refresh broadcasts
                    if (nLastRebroadcast)
                        pnode->setAddrKnown.clear();

                    // Rebroadcast our address
                    if (netParams->fListen)
                    {
                        CAddress addr = GetLocalAddress(&pnode->addr, netParams);
                        if (addr.IsRoutable())
                            pnode->PushAddress(addr);
                    }
                }
            }
            nLastRebroadcast = GetTime();
        }

        //
        // Message: addr
        //
        if (fSendTrickle)
        {
            vector<CAddress> vAddr;
            vAddr.reserve(pto->vAddrToSend.size());
            BOOST_FOREACH(const CAddress& addr, pto->vAddrToSend)
            {
                // returns true if wasn't already contained in the set
                if (pto->setAddrKnown.insert(addr).second)
                {
                    vAddr.push_back(addr);
                    // receiver rejects addr messages larger than 1000
                    if (vAddr.size() >= 1000)
                    {
                        pto->PushMessage("addr", vAddr);
                        vAddr.clear();
                    }
                }
            }
            pto->vAddrToSend.clear();
            if (!vAddr.empty())
                pto->PushMessage("addr", vAddr);
        }

        CNodeState &state = *Bitcredit_State(pto->GetId());
        if (state.fShouldBan) {
            if (pto->addr.IsLocal())
                LogPrintf("Credits: Warning: not banning local node %s!\n", pto->addr.ToString());
            else {
                pto->fDisconnect = true;
                CNode::Ban(pto->addr);
            }
            state.fShouldBan = false;
        }

        BOOST_FOREACH(const CBlockReject& reject, state.rejects)
            pto->PushMessage("reject", (string)"block", reject.chRejectCode, reject.strRejectReason, reject.hashBlock);
        state.rejects.clear();

        // Start block sync
        if (pto->fStartSync && !credits_mainState.ImportingOrReindexing()) {
            pto->fStartSync = false;
            Bitcredit_PushGetBlocks(pto, (Credits_CBlockIndex*)credits_chainActive.Tip(), uint256(0));
        }

        // Resend wallet transactions that haven't gotten in a block yet
        // Except during reindex, importing and IBD, when old wallet
        // transactions become unconfirmed and spams other nodes.
        if (!credits_mainState.ImportingOrReindexing() && !Bitcredit_IsInitialBlockDownload())
        {
            bitcredit_g_signals.Broadcast();
        }

        //
        // Message: inventory
        //
        vector<CInv> vInv;
        vector<CInv> vInvWait;
        {
            LOCK(pto->cs_inventory);
            vInv.reserve(pto->vInventoryToSend.size());
            vInvWait.reserve(pto->vInventoryToSend.size());
            BOOST_FOREACH(const CInv& inv, pto->vInventoryToSend)
            {
                if (pto->setInventoryKnown.count(inv))
                    continue;

                // trickle out tx inv to protect privacy
                if (inv.type == MSG_TX && !fSendTrickle)
                {
                    // 1/4 of tx invs blast to all immediately
                    static uint256 hashSalt;
                    if (hashSalt == 0)
                        hashSalt = GetRandHash();
                    uint256 hashRand = inv.hash ^ hashSalt;
                    hashRand = Hash(BEGIN(hashRand), END(hashRand));
                    bool fTrickleWait = ((hashRand & 3) != 0);

                    if (fTrickleWait)
                    {
                        vInvWait.push_back(inv);
                        continue;
                    }
                }

                // returns true if wasn't already contained in the set
                if (pto->setInventoryKnown.insert(inv).second)
                {
                    vInv.push_back(inv);
                    if (vInv.size() >= 1000)
                    {
                        pto->PushMessage("inv", vInv);
                        vInv.clear();
                    }
                }
            }
            pto->vInventoryToSend = vInvWait;
        }
        if (!vInv.empty())
            pto->PushMessage("inv", vInv);


        // Detect stalled peers. Require that blocks are in flight, we haven't
        // received a (requested) block in one minute, and that all blocks are
        // in flight for over two minutes, since we first had a chance to
        // process an incoming block.
        int64_t nNow = GetTimeMicros();
        if (!pto->fDisconnect && state.nBlocksInFlight &&
            state.nLastBlockReceive < state.nLastBlockProcess - BITCREDIT_BLOCK_DOWNLOAD_TIMEOUT*1000000 &&
            state.vBlocksInFlight.front().nTime < state.nLastBlockProcess - 2*BITCREDIT_BLOCK_DOWNLOAD_TIMEOUT*1000000) {
            LogPrintf("Credits: Peer %s is stalling block download, disconnecting\n", state.name.c_str());
            pto->fDisconnect = true;
        }

        //
        // Message: getdata (blocks)
        //
        vector<CInv> vGetData;
        while (!pto->fDisconnect && state.nBlocksToDownload && state.nBlocksInFlight < BITCREDIT_MAX_BLOCKS_IN_TRANSIT_PER_PEER) {
            uint256 hash = state.vBlocksToDownload.front();
            vGetData.push_back(CInv(MSG_BLOCK, hash));
            Bitcredit_MarkBlockAsInFlight(pto->GetId(), hash);
            LogPrint(netParams->DebugCategory(), "Credits: Requesting block %s from %s\n", hash.ToString(), state.name);
            if (vGetData.size() >= 1000)
            {
                pto->PushMessage("getdata", vGetData);
                vGetData.clear();
            }
        }

        //
        // Message: getdata (non-blocks)
        //
        while (!pto->fDisconnect && !pto->mapAskFor.empty() && (*pto->mapAskFor.begin()).first <= nNow)
        {
            const CInv& inv = (*pto->mapAskFor.begin()).second;
            if (!Bitcredit_AlreadyHave(inv))
            {
                if (fDebug)
                    LogPrint(netParams->DebugCategory(), "Credits: sending getdata: %s\n", inv.ToString());
                vGetData.push_back(inv);
                if (vGetData.size() >= 1000)
                {
                    pto->PushMessage("getdata", vGetData);
                    vGetData.clear();
                }
            }
            pto->mapAskFor.erase(pto->mapAskFor.begin());
        }
        if (!vGetData.empty())
            pto->PushMessage("getdata", vGetData);

    }
    return true;
}






class Bitcredit_CMainCleanup
{
public:
    Bitcredit_CMainCleanup() {}
    ~Bitcredit_CMainCleanup() {
        // block headers
        std::map<uint256, Credits_CBlockIndex*>::iterator it1 = credits_mapBlockIndex.begin();
        for (; it1 != credits_mapBlockIndex.end(); it1++)
            delete (*it1).second;
        credits_mapBlockIndex.clear();

        // orphan blocks
        std::map<uint256, COrphanBlock*>::iterator it2 = credits_orphanIndex.mapOrphanBlocks.begin();
        for (; it2 != credits_orphanIndex.mapOrphanBlocks.end(); it2++)
            delete (*it2).second;
        credits_orphanIndex.SetNull();

        // orphan transactions
        bitcredit_mapOrphanTransactions.clear();
    }
} bitcredit_instance_of_cmaincleanup;
