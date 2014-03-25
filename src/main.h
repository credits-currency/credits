// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2014 The Bitcoin developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_MAIN_H
#define BITCOIN_MAIN_H

#if defined(HAVE_CONFIG_H)
#include "bitcredit-config.h"
#endif

#include "chainparams.h"
#include "coins.h"
#include "core.h"
#include "net.h"
#include "script.h"
#include "sync.h"
#include "txmempool.h"
#include "uint256.h"

#include "main_common.h"
#include "bitcoin_main.h"

#include <algorithm>
#include <exception>
#include <map>
#include <set>
#include <stdint.h>
#include <string>
#include <utility>
#include <vector>

class Bitcredit_CBlockIndex;
class CBloomFilter;
class CInv;
class Bitcredit_CWallet;
class Bitcoin_CWallet;

/** The maximum allowed size for a serialized block, in bytes (network rule) */
static const unsigned int BITCREDIT_MAX_BLOCK_SIZE = 4 * 1000000;
/** Default for -blockmaxsize and -blockminsize, which control the range of sizes the mining code will create **/
static const unsigned int BITCREDIT_DEFAULT_BLOCK_MAX_SIZE = 4 * 750000;
static const unsigned int BITCREDIT_DEFAULT_BLOCK_MIN_SIZE = 0;
/** Default for -blockprioritysize, maximum space for zero/low-fee transactions **/
static const unsigned int BITCREDIT_DEFAULT_BLOCK_PRIORITY_SIZE = 50000;
/** The maximum size for transactions we're willing to relay/mine */
static const unsigned int BITCREDIT_MAX_STANDARD_TX_SIZE = 100000;
/** The maximum allowed number of signature check operations in a block (network rule) */
static const unsigned int BITCREDIT_MAX_BLOCK_SIGOPS = BITCREDIT_MAX_BLOCK_SIZE/50;
/** The maximum number of orphan transactions kept in memory */
static const unsigned int BITCREDIT_MAX_ORPHAN_TRANSACTIONS = BITCREDIT_MAX_BLOCK_SIZE/100;
/** Default for -maxorphanblocks, maximum number of orphan blocks kept in memory */
static const unsigned int BITCREDIT_DEFAULT_MAX_ORPHAN_BLOCKS = 750;
/** The maximum size of a blk?????.dat file (since 0.8) */
static const unsigned int BITCREDIT_MAX_BLOCKFILE_SIZE = 0x8000000; // 128 MiB
/** The pre-allocation chunk size for blk?????.dat files (since 0.8) */
static const unsigned int BITCREDIT_BLOCKFILE_CHUNK_SIZE = 0x1000000; // 16 MiB
/** The pre-allocation chunk size for rev?????.dat files (since 0.8) */
static const unsigned int BITCREDIT_UNDOFILE_CHUNK_SIZE = 0x100000; // 1 MiB
/** Coinbase transaction outputs can only be spent after this number of new blocks (network rule) */
static const int BITCREDIT_COINBASE_MATURITY = 100;
/** Threshold for nLockTime: below this value it is interpreted as block number, otherwise as UNIX timestamp. */
static const unsigned int BITCREDIT_LOCKTIME_THRESHOLD = 500000000; // Tue Nov  5 00:53:20 1985 UTC
/** Maximum number of script-checking threads allowed */
static const int BITCREDIT_MAX_SCRIPTCHECK_THREADS = 16;
/** -par default (number of script-checking threads, 0 = auto) */
static const int BITCREDIT_DEFAULT_SCRIPTCHECK_THREADS = 0;
/** Number of blocks that can be requested at any given time from a single peer. */
static const int BITCREDIT_MAX_BLOCKS_IN_TRANSIT_PER_PEER = 128;
/** Timeout in seconds before considering a block download peer unresponsive. */
static const unsigned int BITCREDIT_BLOCK_DOWNLOAD_TIMEOUT = 60;
/** The highest a bitcoin block can be in the blockchain to be referenced from a bitcredit block.
 * 	Once that threshold has been passed bitcredit can run without a reference to the bitcoin blockchain.
 * 	When that bitcoin block is mined, the total monetary base for bitcoin will be 18 000 000 BTC
 *  Block 600000 is approximately year 2020. */
static const int BITCREDIT_MAX_BITCOIN_LINK_HEIGHT = 600000;
/** The maximum amount of bitcoins that can be claimed. */
static const int64_t BITCREDIT_MAX_BITCOIN_CLAIM = 15000000 * COIN;
/** At this depth block values are deducted from the deposit calculation amount.*/
static const int BITCREDIT_DEPOSIT_CUTOFF_DEPTH = 400000;
/** The totalDepositBase level when a lowered reward will be enforced if deposit is not large enough.
 *  Up until this level the only thing a too low deposit will lead to is a higher difficulty. */
static const int64_t BITCREDIT_ENFORCE_SUBSIDY_REDUCTION_AFTER = 15000000 * COIN;

#ifdef USE_UPNP
static const int bitcredit_fHaveUPnP = true;
#else
static const int bitcredit_fHaveUPnP = false;
#endif

/** "reject" message codes **/
static const unsigned char BITCREDIT_REJECT_MALFORMED = 0x01;
static const unsigned char BITCREDIT_REJECT_INVALID = 0x10;
static const unsigned char BITCREDIT_REJECT_OBSOLETE = 0x11;
static const unsigned char BITCREDIT_REJECT_DUPLICATE = 0x12;
static const unsigned char BITCREDIT_REJECT_NONSTANDARD = 0x40;
static const unsigned char BITCREDIT_REJECT_DUST = 0x41;
static const unsigned char BITCREDIT_REJECT_INSUFFICIENTFEE = 0x42;
static const unsigned char BITCREDIT_REJECT_CHECKPOINT = 0x43;
static const unsigned char BITCREDIT_REJECT_INVALID_BITCOIN_BLOCK_LINK = 0x44;

extern MainState bitcredit_mainState;

extern CScript BITCREDIT_COINBASE_FLAGS;
extern Bitcredit_CTxMemPool bitcredit_mempool;
extern std::map<uint256, Bitcredit_CBlockIndex*> bitcredit_mapBlockIndex;
extern uint64_t bitcredit_nLastBlockTx;
extern uint64_t bitcredit_nLastBlockSize;
extern const std::string bitcredit_strMessageMagic;
extern int64_t bitcredit_nTimeBestReceived;
extern bool bitcredit_fBenchmark;
extern int bitcredit_nScriptCheckThreads;
extern bool bitcredit_fTxIndex;
extern unsigned int bitcredit_nCoinCacheSize;

// Minimum disk space required - used in CheckDiskSpace()
static const uint64_t bitcredit_nMinDiskSpace = 52428800;


class Bitcredit_CBlockTreeDB;
class Bitcredit_CTxUndo;
class Bitcredit_CScriptCheck;
class CValidationState;
class Bitcredit_CWalletInterface;

struct Bitcredit_CBlockTemplate;

/** Register a wallet to receive updates from core */
void Bitcredit_RegisterWallet(Bitcredit_CWalletInterface* pwalletIn);
/** Unregister a wallet from core */
void Bitcredit_UnregisterWallet(Bitcredit_CWalletInterface* pwalletIn);
/** Unregister all wallets from core */
void Bitcredit_UnregisterAllWallets();
/** Push an updated transaction to all registered wallets */
void Bitcredit_SyncWithWallets(const Bitcoin_CWallet *bitcoin_wallet, Bitcoin_CClaimCoinsViewCache &claim_view, const uint256 &hash, const Bitcredit_CTransaction& tx, const Bitcredit_CBlock* pblock = NULL);

/** Register with a network node to receive its signals */
void Bitcredit_RegisterNodeSignals(CNodeSignals& nodeSignals);
/** Unregister a network node */
void Bitcredit_UnregisterNodeSignals(CNodeSignals& nodeSignals);

void Bitcredit_PushGetBlocks(CNode* pnode, Bitcredit_CBlockIndex* pindexBegin, uint256 hashEnd);

/** Process an incoming block */
bool Bitcredit_ProcessBlock(CValidationState &state, CNode* pfrom, Bitcredit_CBlock* pblock, CDiskBlockPos *dbp = NULL);
/** Check whether enough disk space is available for an incoming block */
bool Bitcredit_CheckDiskSpace(uint64_t nAdditionalBytes = 0);
/** Open a block file (blk?????.dat) */
FILE* Bitcredit_OpenBlockFile(const CDiskBlockPos &pos, bool fReadOnly = false);
/** Open an undo file (rev?????.dat) */
FILE* Bitcredit_OpenUndoFile(const CDiskBlockPos &pos, bool fReadOnly = false);
/** Import blocks from an external file */
bool Bitcredit_LoadExternalBlockFile(FILE* fileIn, CDiskBlockPos *dbp = NULL);
/** Initialize a new block tree database + block data on disk */
bool Bitcredit_InitBlockIndex();
/** Load the block tree and coins database from disk */
bool Bitcredit_LoadBlockIndex();
/** Unload database information */
void Bitcredit_UnloadBlockIndex();
/** Print the loaded block tree */
void Bitcredit_PrintBlockTree();
/** Process protocol messages received from a given node */
bool Bitcredit_ProcessMessages(CNode* pfrom);
/** Send queued protocol messages to be sent to a give node */
bool Bitcredit_SendMessages(CNode* pto, bool fSendTrickle);
/** Run an instance of the script checking thread */
void Bitcredit_ThreadScriptCheck();
/** Check whether a block hash satisfies the proof-of-work requirement specified by nBits */
bool Bitcredit_CheckProofOfWork(uint256 hash, unsigned int nBits, uint64_t nTotalDepositBase, uint64_t nDepositAmount);
/** Calculate the minimum amount of work a received block needs, without knowing its direct parent */
unsigned int Bitcredit_ComputeMinWork(unsigned int nBase, int64_t nTime);
/** Check whether we are doing an initial block download (synchronizing from disk or network) */
bool Bitcredit_IsInitialBlockDownload();
/** Format a string that describes several potential problems detected by the core */
std::string Bitcredit_GetWarnings(std::string strFor);
/** Retrieve a transaction (from memory pool, or from disk, if possible) */
bool Bitcredit_GetTransaction(const uint256 &hash, Bitcredit_CTransaction &tx, uint256 &hashBlock, bool fAllowSlow = false);
/** Find the best known block, and make it the tip of the block chain */
bool Bitcredit_ActivateBestChain(CValidationState &state);
/** The max subsidy a block can give if the deposit is large enough */
uint64_t Bitcredit_GetMaxBlockSubsidy(const uint64_t nTotalMonetaryBase);
/** Gets the coinbase value with fees and deposit amount taken into account */
uint64_t Bitcredit_GetAllowedBlockSubsidy(const uint64_t nTotalMonetaryBase, const uint64_t nTotalDepositBase, uint64_t nDepositAmount);
unsigned int Bitcredit_GetNextWorkRequired(const Bitcredit_CBlockIndex* pindexLast, const Bitcredit_CBlockHeader *pblock);

/**
 * To be able to claim full reward a deposit of 1/15 000 of half the total deposit base
 * must be put as deposit in the block. All values are rounded down by normal integer rounding.
 * The requirement to put 1/15000 of half the total monetary base will cause each deposit
 * to be locked for approximately 100 days.
 * Ex: 5 000 000 / (2 * 15 000) = 167 bitcredits.
 * 167 bitcredits * 6 *24 (blocks each day) * 100 (days) = 2 400 000.
 */
uint64_t Bitcredit_GetRequiredDeposit(const uint64_t nTotalDepositBase);

//ONLY exposed here for testability
uint256 Bitcredit_ReduceByReqDepositLevel(uint256 nValue, const uint64_t nTotalDepositBase, const uint64_t nDepositAmount);


void Bitcredit_UpdateTime(Bitcredit_CBlockHeader& block, const Bitcredit_CBlockIndex* pindexPrev);

/** Create a new block index entry for a given block hash */
Bitcredit_CBlockIndex * Bitcredit_InsertBlockIndex(uint256 hash);
/** Verify a signature */
bool Bitcredit_VerifySignature(const Bitcredit_CCoins& txFrom, const Bitcredit_CTransaction& txTo, unsigned int nIn, unsigned int flags, int nHashType);
/** Get statistics from node state */
bool Bitcredit_GetNodeStateStats(NodeId nodeid, CNodeStateStats &stats);
/** Increase a node's misbehavior score. */
void Bitcredit_Misbehaving(NodeId nodeid, int howmuch);


/** (try to) add transaction to memory pool **/
bool Bitcredit_AcceptToMemoryPool(Bitcredit_CTxMemPool& pool, CValidationState &state, const Bitcredit_CTransaction &tx, bool fLimitFree,
                        bool* pfMissingInputs, bool fRejectInsaneFee=false);









int64_t Bitcredit_GetMinFee(const Bitcredit_CTransaction& tx, unsigned int nBytes, bool fAllowFree, enum GetMinFee_mode mode);

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

/** Check for standard transaction types
    @param[in] mapInputs    Map of previous transactions that have outputs we're spending
    @return True if all inputs (scriptSigs) use only standard transaction forms
*/
bool Bitcredit_AreInputsStandard(const Bitcredit_CTransaction& tx, Bitcredit_CCoinsViewCache& bitcredit_view, Bitcoin_CClaimCoinsViewCache& claim_view);

/** Count ECDSA signature operations the old-fashioned (pre-0.6) way
    @return number of sigops this transaction's outputs will produce when spent
    @see Bitcredit_CTransaction::FetchInputs
*/
unsigned int Bitcredit_GetLegacySigOpCount(const Bitcredit_CTransaction& tx);

/** Count ECDSA signature operations in pay-to-script-hash inputs.

    @param[in] mapInputs	Map of previous transactions that have outputs we're spending
    @return maximum number of sigops required to validate this transaction's inputs
    @see Bitcredit_CTransaction::FetchInputs
 */
unsigned int Bitcredit_GetP2SHSigOpCount(const Bitcredit_CTransaction& tx, Bitcredit_CCoinsViewCache& bitcredit_inputs, Bitcoin_CClaimCoinsViewCache& claim_inputs);


inline bool Bitcredit_AllowFree(double dPriority)
{
    // Large (in bytes) low-priority (new, small-coin) transactions
    // need a fee.
    return dPriority > COIN * 144 / 250;
}

bool Bitcredit_FindBestBlockAndCheckClaims(Bitcoin_CClaimCoinsViewCache &claim_view, const int64_t nClaimedCoins);
//Check that the claiming attempts being done are within the limits (90% of total monetary base and max 15 000 000  coins). Max bitcoin block height (600 000) tested in Bitcredit_CheckBlockHeader
bool Bitcredit_CheckClaimsAreInBounds(Bitcoin_CClaimCoinsViewCache &bitcoin_inputs, const int64_t nTryClaimedCoins, const int nBitcoinBlockHeight);

// Check whether all inputs of this transaction are valid (no double spends, scripts & sigs, amounts)
// This does not modify the UTXO set. If pvChecks is not NULL, script checks are pushed onto it
// instead of being performed inline.
bool Bitcredit_CheckInputs(const Bitcredit_CTransaction& tx, CValidationState &state, Bitcredit_CCoinsViewCache &bitcredit_inputs, Bitcoin_CClaimCoinsViewCache &claim_inputs, int64_t &nTotalClaimedCoins, bool fScriptChecks = true,
                 unsigned int flags = STANDARD_SCRIPT_VERIFY_FLAGS,
                 std::vector<Bitcredit_CScriptCheck> *pvChecks = NULL);

// Apply the effects of this transaction on the UTXO set represented by view
void Bitcredit_UpdateCoins(const Bitcredit_CTransaction& tx, CValidationState &state, Bitcredit_CCoinsViewCache &bitcredit_inputs, Bitcoin_CClaimCoinsViewCache &claim_inputs, Bitcredit_CTxUndo &txundo,  int nHeight, const uint256 &txhash);

// Context-independent validity checks
bool Bitcredit_CheckTransaction(const Bitcredit_CTransaction& tx, CValidationState& state);

/** Check for standard transaction types
    @return True if all outputs (scriptPubKeys) use only standard transaction forms
*/
bool Bitcredit_IsStandardTx(const Bitcredit_CTransaction& tx, std::string& reason);

bool Bitcredit_IsFinalTx(const Bitcredit_CTransaction &tx, int nBlockHeight = 0, int64_t nBlockTime = 0);

/** Undo information for a CBlock */
class Bitcredit_CBlockUndo
{
public:
    std::vector<Bitcredit_CTxUndo> vtxundo; // for all but the coinbase

    IMPLEMENT_SERIALIZE(
        READWRITE(vtxundo);
    )

    bool WriteToDisk(CDiskBlockPos &pos, const uint256 &hashBlock, CNetParams * netParams)
    {
        // Open history file to append
        CAutoFile fileout = CAutoFile(Bitcredit_OpenUndoFile(pos), SER_DISK, netParams->ClientVersion());
        if (!fileout)
            return error("Bitcredit: CBlockUndo::WriteToDisk : OpenUndoFile failed");

        // Write index header
        unsigned int nSize = fileout.GetSerializeSize(*this);
        fileout << FLATDATA(netParams->MessageStart()) << nSize;

        // Write undo data
        long fileOutPos = ftell(fileout);
        if (fileOutPos < 0)
            return error("Bitcredit: CBlockUndo::WriteToDisk : ftell failed");
        pos.nPos = (unsigned int)fileOutPos;
        fileout << *this;

        // calculate & write checksum
        CHashWriter hasher(SER_GETHASH, BITCREDIT_PROTOCOL_VERSION);
        hasher << hashBlock;
        hasher << *this;
        fileout << hasher.GetHash();

        // Flush stdio buffers and commit to disk before returning
        fflush(fileout);
        if (!Bitcredit_IsInitialBlockDownload())
            FileCommit(fileout);

        return true;
    }

    bool ReadFromDisk(const CDiskBlockPos &pos, const uint256 &hashBlock, CNetParams * netParams)
    {
        // Open history file to read
        CAutoFile filein = CAutoFile(Bitcredit_OpenUndoFile(pos, true), SER_DISK, netParams->ClientVersion());
        if (!filein)
            return error("Bitcredit: CBlockUndo::ReadFromDisk : OpenBlockFile failed");

        // Read block
        uint256 hashChecksum;
        try {
            filein >> *this;
            filein >> hashChecksum;
        }
        catch (std::exception &e) {
            return error("Bitcredit: %s : Deserialize or I/O error - %s", __func__, e.what());
        }

        // Verify checksum
        CHashWriter hasher(SER_GETHASH, BITCREDIT_PROTOCOL_VERSION);
        hasher << hashBlock;
        hasher << *this;
        if (hashChecksum != hasher.GetHash())
            return error("Bitcredit: CBlockUndo::ReadFromDisk : Checksum mismatch");

        return true;
    }
};


/** Closure representing one script verification
 *  Note that this stores references to the spending transaction */
class Bitcredit_CScriptCheck
{
private:
    CScript scriptPubKey;
    const Bitcredit_CTransaction *ptxTo;
    unsigned int nIn;
    unsigned int nFlags;
    int nHashType;

public:
    Bitcredit_CScriptCheck() {}
    Bitcredit_CScriptCheck(const Bitcredit_CCoins& txFromIn, const Bitcredit_CTransaction& txToIn, unsigned int nInIn, unsigned int nFlagsIn, int nHashTypeIn) :
        scriptPubKey(txFromIn.vout[txToIn.vin[nInIn].prevout.n].scriptPubKey),
        ptxTo(&txToIn), nIn(nInIn), nFlags(nFlagsIn), nHashType(nHashTypeIn) { }
    Bitcredit_CScriptCheck(const Bitcoin_CClaimCoins& txFromIn, const Bitcredit_CTransaction& txToIn, unsigned int nInIn, unsigned int nFlagsIn, int nHashTypeIn) :
        scriptPubKey(txFromIn.vout[txToIn.vin[nInIn].prevout.n].scriptPubKey),
        ptxTo(&txToIn), nIn(nInIn), nFlags(nFlagsIn), nHashType(nHashTypeIn) { }

    bool operator()() const;

    void swap(Bitcredit_CScriptCheck &check) {
        scriptPubKey.swap(check.scriptPubKey);
        std::swap(ptxTo, check.ptxTo);
        std::swap(nIn, check.nIn);
        std::swap(nFlags, check.nFlags);
        std::swap(nHashType, check.nHashType);
    }
};

/** A transaction with a merkle branch linking it to the block chain. */
class Bitcredit_CMerkleTx : public Bitcredit_CTransaction
{
private:
    int GetDepthInMainChainINTERNAL(Bitcredit_CBlockIndex* &pindexRet) const;

public:
    uint256 hashBlock;
    std::vector<uint256> vMerkleBranch;
    int nIndex;

    // memory only
    mutable bool fMerkleVerified;


    Bitcredit_CMerkleTx()
    {
        Init();
    }

    Bitcredit_CMerkleTx(const Bitcredit_CTransaction& txIn) : Bitcredit_CTransaction(txIn)
    {
        Init();
    }

    void Init()
    {
        hashBlock = 0;
        nIndex = -1;
        fMerkleVerified = false;
    }


    IMPLEMENT_SERIALIZE
    (
        nSerSize += SerReadWrite(s, *(Bitcredit_CTransaction*)this, nType, nVersion, ser_action);
        nVersion = this->nVersion;
        READWRITE(hashBlock);
        READWRITE(vMerkleBranch);
        READWRITE(nIndex);
    )


    int SetMerkleBranch(const Bitcredit_CBlock* pblock=NULL);

    // Return depth of transaction in blockchain:
    // -1  : not in blockchain, and not in memory pool (conflicted transaction)
    //  0  : in memory pool, waiting to be included in a block
    // >=1 : this many blocks deep in the main chain
    int GetDepthInMainChain(Bitcredit_CBlockIndex* &pindexRet) const;
    int GetDepthInMainChain() const { Bitcredit_CBlockIndex *pindexRet; return GetDepthInMainChain(pindexRet); }
    bool IsInMainChain() const { Bitcredit_CBlockIndex *pindexRet; return GetDepthInMainChainINTERNAL(pindexRet) > 0; }
    int GetBlocksToMaturity() const;
    int GetFirstDepositOutBlocksToMaturity() const;
    int GetSecondDepositOutBlocksToMaturity() const;
    bool AcceptToMemoryPool(bool fLimitFree=true);
};







/** Functions for disk access for blocks */
bool Bitcredit_WriteBlockToDisk(Bitcredit_CBlock& block, CDiskBlockPos& pos);
bool Bitcredit_ReadBlockFromDisk(Bitcredit_CBlock& block, const CDiskBlockPos& pos);
bool Bitcredit_ReadBlockFromDisk(Bitcredit_CBlock& block, const Bitcredit_CBlockIndex* pindex);


/** Functions for validating blocks and updating the block tree */

/** Undo the effects of this block (with given index) on the UTXO set represented by coins.
 *  In case pfClean is provided, operation will try to be tolerant about errors, and *pfClean
 *  will be true if no problems were found. Otherwise, the return value will be false in case
 *  of problems. Note that in any case, coins may be modified. */
bool Bitcredit_DisconnectBlock(Bitcredit_CBlock& block, CValidationState& state, Bitcredit_CBlockIndex* pindex, Bitcredit_CCoinsViewCache& bitcredit_view, Bitcoin_CClaimCoinsViewCache& claim_view, bool updateBitcoinUndo, bool* pfClean = NULL);

/** Calculates resurrection of deposit base. Should be used just before coins are updated */
void UpdateResurrectedDepositBase(const Bitcredit_CBlockIndex* pBlockToTrim, const Bitcredit_CTransaction &tx, int64_t &nResurrectedDepositBase, Bitcredit_CCoinsViewCache& bitcredit_view);
/** Calculates trimming of deposit base. Should be used just after coins are updated */
void UpdateTrimmedDepositBase(const Bitcredit_CBlockIndex* pBlockToTrim, Bitcredit_CBlock &trimBlock, int64_t &nTrimmedDepositBase, Bitcredit_CCoinsViewCache& bitcredit_view);
// Apply the effects of this block (with given index) on the UTXO set represented by coins
bool Bitcredit_ConnectBlock(Bitcredit_CBlock& block, CValidationState& state, Bitcredit_CBlockIndex* pindex, Bitcredit_CCoinsViewCache& bitcredit_view, Bitcoin_CClaimCoinsViewCache& claim_view, bool updateBitcoinUndo, bool fJustCheck);

// Add this block to the block index, and if necessary, switch the active block chain to this
bool Bitcredit_AddToBlockIndex(Bitcredit_CBlock& block, CValidationState& state, const CDiskBlockPos& pos);

// Context-independent validity checks
bool Bitcredit_CheckBlockHeader(const Bitcredit_CBlockHeader& block, CValidationState& state, bool fCheckPOW = true);
bool Bitcredit_CheckBlock(const Bitcredit_CBlock& block, CValidationState& state, bool fCheckPOW = true, bool fCheckMerkleRoot = true);

// Store block on disk
// if dbp is provided, the file is known to already reside on disk
bool Bitcredit_AcceptBlock(Bitcredit_CBlock& block, CValidationState& state, Bitcredit_CBlockIndex **pindex, CDiskBlockPos* dbp, CNetParams * netParams);
bool Bitcredit_AcceptBlockHeader(Bitcredit_CBlockHeader& block, CValidationState& state, Bitcredit_CBlockIndex **ppindex= NULL);


/** The block chain is a tree shaped structure starting with the
 * genesis block at the root, with each block potentially having multiple
 * candidates to be the next block. A blockindex may have multiple pprev pointing
 * to it, but at most one of them can be part of the currently active branch.
 */
class Bitcredit_CBlockIndex : public CBlockIndexBase
{
public:
    // block header
    int nVersion;
    uint256 hashMerkleRoot;
    uint256 hashLinkedBitcoinBlock;
    uint256 hashSigMerkleRoot;
    unsigned int nNonce;
    uint64_t nTotalMonetaryBase;
    uint64_t nTotalDepositBase;
    uint64_t nDepositAmount;

    // (memory only) Sequencial id assigned to distinguish order in which blocks are received.
    uint32_t nSequenceId;

    Bitcredit_CBlockIndex()
    {
        phashBlock = NULL;
        pprev = NULL;
        nHeight = 0;
        nFile = 0;
        nDataPos = 0;
        nUndoPos = 0;
        nChainWork = 0;
        nTx = 0;
        nChainTx = 0;
        nStatus = 0;
        nSequenceId = 0;

        nVersion       = 0;
        hashMerkleRoot = 0;
        hashLinkedBitcoinBlock         = 0;
        hashSigMerkleRoot         = 0;
        nTime          = 0;
        nBits          = 0;
        nNonce         = 0;
        nTotalMonetaryBase         = 0;
        nTotalDepositBase         = 0;
        nDepositAmount         = 0;
    }

    Bitcredit_CBlockIndex(Bitcredit_CBlockHeader& block)
    {
        phashBlock = NULL;
        pprev = NULL;
        nHeight = 0;
        nFile = 0;
        nDataPos = 0;
        nUndoPos = 0;
        nChainWork = 0;
        nTx = 0;
        nChainTx = 0;
        nStatus = 0;
        nSequenceId = 0;

        nVersion       = block.nVersion;
        hashMerkleRoot = block.hashMerkleRoot;
        hashLinkedBitcoinBlock         = block.hashLinkedBitcoinBlock;
        hashSigMerkleRoot         = block.hashSigMerkleRoot;
        nTime          = block.nTime;
        nBits          = block.nBits;
        nNonce         = block.nNonce;
        nTotalMonetaryBase         = block.nTotalMonetaryBase;
        nTotalDepositBase         = block.nTotalDepositBase;
        nDepositAmount         = block.nDepositAmount;
    }

    CDiskBlockPos GetBlockPos() const {
        CDiskBlockPos ret;
        if (nStatus & BLOCK_HAVE_DATA) {
            ret.nFile = nFile;
            ret.nPos  = nDataPos;
        }
        return ret;
    }

    CDiskBlockPos GetUndoPos() const {
        CDiskBlockPos ret;
        if (nStatus & BLOCK_HAVE_UNDO) {
            ret.nFile = nFile;
            ret.nPos  = nUndoPos;
        }
        return ret;
    }

    Bitcredit_CBlockHeader GetBlockHeader() const
    {
        Bitcredit_CBlockHeader block;
        block.nVersion       = nVersion;
        if (pprev)
            block.hashPrevBlock = pprev->GetBlockHash();
        block.hashMerkleRoot = hashMerkleRoot;
        block.hashLinkedBitcoinBlock         = hashLinkedBitcoinBlock;
        block.hashSigMerkleRoot         = hashSigMerkleRoot;
        block.nTime          = nTime;
        block.nBits          = nBits;
        block.nNonce         = nNonce;
        block.nTotalMonetaryBase = nTotalMonetaryBase;
        block.nTotalDepositBase = nTotalDepositBase;
        block.nDepositAmount = nDepositAmount;
        return block;
    }

    bool CheckIndex() const
    {
        return Bitcredit_CheckProofOfWork(GetBlockHash(), nBits, nTotalDepositBase, nDepositAmount);
    }

    enum { nMedianTimeSpan=11 };

    int64_t GetMedianTimePast() const
    {
        int64_t pmedian[nMedianTimeSpan];
        int64_t* pbegin = &pmedian[nMedianTimeSpan];
        int64_t* pend = &pmedian[nMedianTimeSpan];

        const CBlockIndexBase* pindex = this;
        for (int i = 0; i < nMedianTimeSpan && pindex; i++, pindex = pindex->pprev)
            *(--pbegin) = pindex->GetBlockTime();

        std::sort(pbegin, pend);
        return pbegin[(pend - pbegin)/2];
    }

    /**
     * Returns true if there are nRequired or more blocks of minVersion or above
     * in the last nToCheck blocks, starting at pstart and going backwards.
     */
    static bool IsSuperMajority(int minVersion, const Bitcredit_CBlockIndex* pstart,
                                unsigned int nRequired, unsigned int nToCheck);

    std::string ToString() const
    {
        return strprintf("CBlockIndex(pprev=%p, nHeight=%d, merkle=%s, sigmerkle=%s, hashBlock=%s)",
            pprev, nHeight,
            hashMerkleRoot.ToString(),
            hashSigMerkleRoot.ToString(),
            GetBlockHash().ToString());
    }

    void print() const
    {
        LogPrintf("%s\n", ToString());
    }

    // Check whether this block index entry is valid up to the passed validity level.
    bool IsValid(enum BlockStatus nUpTo = BLOCK_VALID_TRANSACTIONS) const
    {
        assert(!(nUpTo & ~BLOCK_VALID_MASK)); // Only validity flags allowed.
        if (nStatus & BLOCK_FAILED_MASK)
            return false;
        return ((nStatus & BLOCK_VALID_MASK) >= nUpTo);
    }

    // Raise the validity level of this block index entry.
    // Returns true if the validity was changed.
    bool RaiseValidity(enum BlockStatus nUpTo)
    {
        assert(!(nUpTo & ~BLOCK_VALID_MASK)); // Only validity flags allowed.
        if (nStatus & BLOCK_FAILED_MASK)
            return false;
        if ((nStatus & BLOCK_VALID_MASK) < nUpTo) {
            nStatus = (nStatus & ~BLOCK_VALID_MASK) | nUpTo;
            return true;
        }
        return false;
    }
};

/** Used to marshal pointers into hashes for db storage. */
class Bitcredit_CDiskBlockIndex : public Bitcredit_CBlockIndex
{
public:
    uint256 hashPrev;

    Bitcredit_CDiskBlockIndex() {
        hashPrev = 0;
    }

    explicit Bitcredit_CDiskBlockIndex(Bitcredit_CBlockIndex* pindex) : Bitcredit_CBlockIndex(*pindex) {
        hashPrev = (pprev ? pprev->GetBlockHash() : 0);
    }

    IMPLEMENT_SERIALIZE
    (
        if (!(nType & SER_GETHASH))
            READWRITE(VARINT(nVersion));

        READWRITE(VARINT(nHeight));
        READWRITE(VARINT(nStatus));
        READWRITE(VARINT(nTx));
        if (nStatus & (BLOCK_HAVE_DATA | BLOCK_HAVE_UNDO))
            READWRITE(VARINT(nFile));
        if (nStatus & BLOCK_HAVE_DATA)
            READWRITE(VARINT(nDataPos));
        if (nStatus & BLOCK_HAVE_UNDO)
            READWRITE(VARINT(nUndoPos));

        // block header
        READWRITE(this->nVersion);
        READWRITE(hashPrev);
        READWRITE(hashMerkleRoot);
        READWRITE(hashLinkedBitcoinBlock);
        READWRITE(hashSigMerkleRoot);
        READWRITE(nTime);
        READWRITE(nBits);
        READWRITE(nNonce);
        READWRITE(nTotalMonetaryBase);
        READWRITE(nTotalDepositBase);
        READWRITE(nDepositAmount);
    )

    uint256 GetBlockHash() const
    {
        Bitcredit_CBlockHeader block;
        block.nVersion        = nVersion;
        block.hashPrevBlock   = hashPrev;
        block.hashMerkleRoot  = hashMerkleRoot;
        block.hashLinkedBitcoinBlock          = hashLinkedBitcoinBlock;
        block.hashSigMerkleRoot          = hashSigMerkleRoot;
        block.nTime           = nTime;
        block.nBits           = nBits;
        block.nNonce          = nNonce;
        block.nTotalMonetaryBase = nTotalMonetaryBase;
        block.nTotalDepositBase = nTotalDepositBase;
        block.nDepositAmount = nDepositAmount;
        return block.GetHash();
    }


    std::string ToString() const
    {
        std::string str = "CDiskBlockIndex(";
        str += Bitcredit_CBlockIndex::ToString();
        str += strprintf("\n                hashBlock=%s, hashPrev=%s)",
            GetBlockHash().ToString(),
            hashPrev.ToString());
        return str;
    }

    void print() const
    {
        LogPrintf("%s\n", ToString());
    }
};


/** RAII wrapper for VerifyDB: Verify consistency of the block and coin databases */
class Bitcredit_CVerifyDB {
public:

	Bitcredit_CVerifyDB();
    ~Bitcredit_CVerifyDB();
    bool VerifyDB(int nCheckLevel, int nCheckDepth);
};

/** An in-memory indexed chain of blocks. */
class Bitcredit_CChain : public CChain {
public:
    /** Returns the index entry for the genesis block of this chain, or NULL if none. */
    Bitcredit_CBlockIndex *Genesis() const {
        return (Bitcredit_CBlockIndex*)(vChain.size() > 0 ? vChain[0] : NULL);
    }

    /** Returns the index entry at a particular height in this chain, or NULL if no such height exists. */
    Bitcredit_CBlockIndex *operator[](int nHeight) const {
        if (nHeight < 0 || nHeight >= (int)vChain.size())
            return NULL;
        return (Bitcredit_CBlockIndex*)vChain[nHeight];
    }

    /** Compare two chains efficiently. */
    friend bool operator==(const Bitcredit_CChain &a, const Bitcredit_CChain &b) {
        return a.vChain.size() == b.vChain.size() &&
               a.vChain[a.vChain.size() - 1] == b.vChain[b.vChain.size() - 1];
    }

    /** Efficiently check whether a block is present in this chain. */
    bool Contains(const CBlockIndexBase *pindex) const {
        return (*this)[pindex->nHeight] == pindex;
    }

    /** Find the successor of a block in this chain, or NULL if the given index is not found or is the tip. */
    Bitcredit_CBlockIndex *Next(const Bitcredit_CBlockIndex *pindex) const {
        if (Contains(pindex))
            return (*this)[pindex->nHeight + 1];
        else
            return NULL;
    }

    /** Set/initialize a chain with a given tip. Returns the forking point. */
    CBlockIndexBase *SetTip(CBlockIndexBase *pindex);

    /** Return a CBlockLocator that refers to a block in this chain (by default the tip). */
    CBlockLocator GetLocator(const CBlockIndexBase *pindex = NULL) const;

    /** Find the last common block between this chain and a locator. */
    Bitcredit_CBlockIndex *FindFork(const CBlockLocator &locator) const;
};

/** The currently-connected chain of blocks. */
extern Bitcredit_CChain bitcredit_chainActive;

/** The currently best known chain of headers (some of which may be invalid). */
extern Bitcredit_CChain bitcredit_chainMostWork;

/** Global variable that points to the active Bitcredit_CCoinsView (protected by cs_main) */
extern Bitcredit_CCoinsViewCache *bitcredit_pcoinsTip;

/** Global variable that points to the active block tree (protected by cs_main) */
extern Bitcredit_CBlockTreeDB *bitcredit_pblocktree;

struct Bitcredit_CBlockTemplate
{
	Bitcredit_CBlock block;
    std::vector<int64_t> vTxFees;
    std::vector<int64_t> vTxSigOps;
};






/** Used to relay blocks as header + vector<merkle branch>
 * to filtered nodes.
 */
class Bitcredit_CMerkleBlock
{
public:
    // Public only for unit testing
    Bitcredit_CBlockHeader header;
    CPartialMerkleTree txn;

public:
    // Public only for unit testing and relay testing
    // (not relayed)
    std::vector<std::pair<unsigned int, uint256> > vMatchedTxn;

    // Create from a CBlock, filtering transactions according to filter
    // Note that this will call IsRelevantAndUpdate on the filter for each transaction,
    // thus the filter will likely be modified.
    Bitcredit_CMerkleBlock(const Bitcredit_CBlock& block, CBloomFilter& filter);

    IMPLEMENT_SERIALIZE
    (
        READWRITE(header);
        READWRITE(txn);
    )
};


class Bitcredit_CWalletInterface {
protected:
    virtual void SyncTransaction(const Bitcoin_CWallet *bitcoin_wallet, Bitcoin_CClaimCoinsViewCache &claim_view, const uint256 &hash, const Bitcredit_CTransaction &tx, const Bitcredit_CBlock *pblock) =0;
    virtual void EraseFromWallet(Bitcredit_CWallet *bitcredit_wallet, const uint256 &hash) =0;
    virtual void SetBestChain(const CBlockLocator &locator) =0;
    virtual void UpdatedTransaction(const uint256 &hash) =0;
    virtual void Inventory(const uint256 &hash) =0;
    virtual void ResendWalletTransactions() =0;
    friend void ::Bitcredit_RegisterWallet(Bitcredit_CWalletInterface*);
    friend void ::Bitcredit_UnregisterWallet(Bitcredit_CWalletInterface*);
    friend void ::Bitcredit_UnregisterAllWallets();
};

#endif
