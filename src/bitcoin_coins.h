// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2013 The Bitcoin developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
#ifndef BITCOIN_BITCOIN_COINS_H
#define BITCOIN_BITCOIN_COINS_H

#include "bitcoin_core.h"
#include "serialize.h"
#include "uint256.h"

#include <assert.h>
#include <stdint.h>

#include <boost/foreach.hpp>

/** pruned version of CTransaction: only retains metadata and unspent transaction outputs
 *
 * Serialized format:
 * - VARINT(nVersion)
 * - VARINT(nCode)
 * - unspentness bitvector, for vout[2] and further; least significant byte first
 * - the non-spent CTxOuts (via CTxOutCompressor)
 * - VARINT(nHeight)
 *
 * The nCode value consists of:
 * - bit 1: IsCoinBase()
 * - bit 2: vout[0] is not spent
 * - bit 4: vout[1] is not spent
 * - The higher bits encode N, the number of non-zero bytes in the following bitvector.
 *   - In case both bit 2 and bit 4 are unset, they encode N-1, as there must be at
 *     least one non-spent output).
 *
 * Example: 0104835800816115944e077fe7c803cfa57f29b36bf87c1d358bb85e
 *          <><><--------------------------------------------><---->
 *          |  \                  |                             /
 *    version   code             vout[1]                  height
 *
 *    - version = 1
 *    - code = 4 (vout[1] is not spent, and 0 non-zero bytes of bitvector follow)
 *    - unspentness bitvector: as 0 non-zero bytes follow, it has length 0
 *    - vout[1]: 835800816115944e077fe7c803cfa57f29b36bf87c1d35
 *               * 8358: compact amount representation for 60000000000 (600 CRE)
 *               * 00: special txout type pay-to-pubkey-hash
 *               * 816115944e077fe7c803cfa57f29b36bf87c1d35: address uint160
 *    - height = 203998
 *
 *
 * Example: 0109044086ef97d5790061b01caab50f1b8e9c50a5057eb43c2d9563a4eebbd123008c988f1a4a4de2161e0f50aac7f17e7f9555caa486af3b
 *          <><><--><--------------------------------------------------><----------------------------------------------><---->
 *         /  \   \                     |                                                           |                     /
 *  version  code  unspentness       vout[4]                                                     vout[16]           height
 *
 *  - version = 1
 *  - code = 9 (coinbase, neither vout[0] or vout[1] are unspent,
 *                2 (1, +1 because both bit 2 and bit 4 are unset) non-zero bitvector bytes follow)
 *  - unspentness bitvector: bits 2 (0x04) and 14 (0x4000) are set, so vout[2+2] and vout[14+2] are unspent
 *  - vout[4]: 86ef97d5790061b01caab50f1b8e9c50a5057eb43c2d9563a4ee
 *             * 86ef97d579: compact amount representation for 234925952 (2.35 CRE)
 *             * 00: special txout type pay-to-pubkey-hash
 *             * 61b01caab50f1b8e9c50a5057eb43c2d9563a4ee: address uint160
 *  - vout[16]: bbd123008c988f1a4a4de2161e0f50aac7f17e7f9555caa4
 *              * bbd123: compact amount representation for 110397 (0.001 CRE)
 *              * 00: special txout type pay-to-pubkey-hash
 *              * 8c988f1a4a4de2161e0f50aac7f17e7f9555caa4: address uint160
 *  - height = 120891
 */
class Bitcoin_CCoins
{
public:
    // whether transaction is a coinbase
    bool fCoinBase;

    // unspent transaction outputs; spent outputs are .IsNull(); spent outputs at the end of the array are dropped
   std::vector<Bitcoin_CTxOut> vout;

    // at which height this transaction was included in the active block chain
    int nHeight;

    // version of the CTransaction; accesses to this value should probably check for nHeight as well,
    // as new tx version will probably only be introduced at certain heights
    int nVersion;

    //This constructor has two usages
    //1. It can and must only be done by the Bitcoin coin tip, not the claim coin tip
    //2. To create an object that can be compared with equalsForBlockAttributes
    Bitcoin_CCoins(const Bitcoin_CTransaction &tx, int nHeightIn) : fCoinBase(tx.IsCoinBase()), nHeight(nHeightIn), nVersion(tx.nVersion) {
    	BOOST_FOREACH(const CTxOut & txout, tx.vout) {
    		vout.push_back(Bitcoin_CTxOut(txout.nValue, 0, txout.scriptPubKey, 0));
    	}

    	ClearUnspendable();
    }

    //This constructor has only one usage. NB it must NOT be used for anything else!
    //1. To create an object that can be compared with equalsForBlockCompressedAttributes
    Bitcoin_CCoins(const Bitcoin_CTransactionCompressed &tx, int nHeightIn) : fCoinBase(tx.IsCoinBase()), nHeight(nHeightIn), nVersion(tx.nVersion) {
        if(tx.IsCreatedFromBlock()) {
        	BOOST_FOREACH(const CTxOut & txout, tx.vout) {
        		vout.push_back(Bitcoin_CTxOut(txout.nValue, 0, txout.scriptPubKey, 0));
        	}
        } else {
        	//Add dummy Bitcoin_CTxOut to use for comparison
			for (unsigned int i = 0; i < tx.voutSpendable.size(); i++) {
				CScript dummyScript;
				if(tx.voutSpendable[i]) {
					dummyScript << OP_NOP;
				} else {
					dummyScript << OP_RETURN;
				}
				vout.push_back(Bitcoin_CTxOut(0, 0, dummyScript, -1));
			}
        }

        ClearUnspendable();
    }

    bool equalsForBlockAttributes(const Bitcoin_CCoins &b) {
         // Empty Bitcoin_CCoins objects are always equal.
         if (IsPruned() && b.IsPruned()) {
             return true;
         }

         if (fCoinBase != b.fCoinBase ||
                nHeight != b.nHeight ||
                nVersion != b.nVersion) {
        	 return false;
         }

         if(vout.size() != b.vout.size()) {
        	 return false;
         }

     	for (unsigned int i = 0; i < vout.size(); i++) {
     		const Bitcoin_CTxOut &txout = vout[i];
     		const Bitcoin_CTxOut &btxout = b.vout[i];
             if (txout.nValueOriginal != btxout.nValueOriginal ||
            	txout.scriptPubKey != btxout.scriptPubKey) {
                 return false;
             }
     	}
     	return true;
    }

    bool equalsForBlockCompressedAttributes(const Bitcoin_CCoins &b) {
        // Empty Bitcoin_CCoins objects are always equal.
        if (IsPruned() && b.IsPruned()) {
            return true;
        }

        if (fCoinBase != b.fCoinBase ||
               nHeight != b.nHeight ||
               nVersion != b.nVersion) {
       	 return false;
        }

         if(vout.size() != b.vout.size()) {
        	 return false;
         }

     	return true;
    }

    // empty constructor
    Bitcoin_CCoins() : fCoinBase(false), vout(0), nHeight(0), nVersion(0) { }

    //TODO - test these three fractional calculation.
    // different types of claim activating methods
    void ActivateClaimable(const Bitcoin_CTransaction& tx) {
     	for (unsigned int i = 0; i < vout.size(); i++) {
     		if(!tx.vout[i].scriptPubKey.IsUnspendable()) {
				Bitcoin_CTxOut &txout = vout[i];
				assert(txout.nValueOriginal >= 0);

				txout.nValueClaimable = txout.nValueOriginal;
     		}
     	}

    	ClearUnspendable();
    }
    void ActivateClaimable(const Bitcoin_CTransactionCompressed& tx, const ClaimSum& claimSum) {
		for (unsigned int i = 0; i < vout.size(); i++) {
			if(tx.voutSpendable[i]) {
				Bitcoin_CTxOut & txout = vout[i];

				const int64_t nValue = txout.nValueOriginal;
				const int64_t nValueClaimable = ReduceByFraction(nValue, claimSum.nValueClaimableSum, claimSum.nValueOriginalSum);
				assert((nValueClaimable >= 0 && nValueClaimable <= nValue));

				txout.nValueClaimable = nValueClaimable;
			}
		}

    	ClearUnspendable();
    }
    void ActivateClaimableCoinbase(const Bitcoin_CTransactionCompressed& tx, const int64_t nTotalReduceFees, const int64_t& nTotalValueOut) {
		for (unsigned int i = 0; i < vout.size(); i++) {
			if(tx.voutSpendable[i]) {
				Bitcoin_CTxOut & txout = vout[i];

				const int64_t nValue = txout.nValueOriginal;
				const int64_t nFractionReducedFee = ReduceByFraction(nTotalReduceFees, nValue, nTotalValueOut);
				int64_t nValueClaimable = 0;
				if(nValue > nFractionReducedFee) {
					nValueClaimable = nValue - nFractionReducedFee;
				}
				assert((nValueClaimable >= 0 && nValueClaimable <= nValue));

				txout.nValueClaimable = nValueClaimable;
			}
		}

    	ClearUnspendable();
    }

    //Used when reverting a coin from claim* functions
    void DeactivateClaimable();

    // remove spent outputs at the end of vout
    void Cleanup() {
        while (vout.size() > 0 && vout.back().IsNull())
        	vout.pop_back();
        if (vout.empty())
            std::vector<Bitcoin_CTxOut>().swap(vout);
    }

    void ClearUnspendable() {
    	for (unsigned int i = 0; i < vout.size(); i++) {
    		Bitcoin_CTxOut &txout = vout[i];
            if (txout.scriptPubKey.IsUnspendable()) {
                txout.SetNull();
            }
    	}
        Cleanup();
    }

    void swap(Bitcoin_CCoins &to) {
        std::swap(to.fCoinBase, fCoinBase);
        to.vout.swap(vout);
        std::swap(to.nHeight, nHeight);
        std::swap(to.nVersion, nVersion);
    }

    void CalcMaskSize(unsigned int &nBytes, unsigned int &nNonzeroBytes) const;

    bool IsCoinBase() const {
        return fCoinBase;
    }

    unsigned int GetSerializeSize(int nType, int nVersion) const {
        unsigned int nSize = 0;
        // version
        nSize += ::GetSerializeSize(VARINT(this->nVersion), nType, nVersion);

        unsigned int nMaskSize = 0, nMaskCode = 0;
        CalcMaskSize(nMaskSize, nMaskCode);

        bool fFirst = vout.size() > 0 && !vout[0].IsNull();
        bool fSecond = vout.size() > 1 && !vout[1].IsNull();
        assert(fFirst || fSecond || nMaskCode);
        unsigned int nCode = 8*(nMaskCode - (fFirst || fSecond ? 0 : 1)) + (fCoinBase ? 1 : 0) + (fFirst ? 2 : 0) + (fSecond ? 4 : 0);
        // size of header code
        nSize += ::GetSerializeSize(VARINT(nCode), nType, nVersion);
        // spentness bitmask
        nSize += nMaskSize;
        // txouts themself
        for (unsigned int i = 0; i < vout.size(); i++)
            if (!vout[i].IsNull())
                nSize += ::GetSerializeSize(Bitcoin_CTxOutCompressor(REF(vout[i])), nType, nVersion);

        // height
        nSize += ::GetSerializeSize(VARINT(nHeight), nType, nVersion);
        return nSize;
    }

    template<typename Stream>
    void Serialize(Stream &s, int nType, int nVersion) const {
        // version
        ::Serialize(s, VARINT(this->nVersion), nType, nVersion);

        unsigned int nMaskSize = 0, nMaskCode = 0;
        CalcMaskSize(nMaskSize, nMaskCode);

        bool fFirst = vout.size() > 0 && !vout[0].IsNull();
        bool fSecond = vout.size() > 1 && !vout[1].IsNull();
        assert(fFirst || fSecond || nMaskCode);
        unsigned int nCode = 8*(nMaskCode - (fFirst || fSecond ? 0 : 1)) + (fCoinBase ? 1 : 0) + (fFirst ? 2 : 0) + (fSecond ? 4 : 0);
        // header code
        ::Serialize(s, VARINT(nCode), nType, nVersion);
        // spentness bitmask
        for (unsigned int b = 0; b<nMaskSize; b++) {
            unsigned char chAvail = 0;
            for (unsigned int i = 0; i < 8 && 2+b*8+i < vout.size(); i++)
                if (!vout[2+b*8+i].IsNull())
                    chAvail |= (1 << i);
            ::Serialize(s, chAvail, nType, nVersion);
        }
        // txouts themself
        for (unsigned int i = 0; i < vout.size(); i++) {
            if (!vout[i].IsNull())
                ::Serialize(s, Bitcoin_CTxOutCompressor(REF(vout[i])), nType, nVersion);
        }

        // coinbase height
        ::Serialize(s, VARINT(nHeight), nType, nVersion);
    }

    template<typename Stream>
    void Unserialize(Stream &s, int nType, int nVersion) {
        // version
        ::Unserialize(s, VARINT(this->nVersion), nType, nVersion);

        unsigned int nCode = 0;
        // header code
        ::Unserialize(s, VARINT(nCode), nType, nVersion);
        fCoinBase = nCode & 1;
        std::vector<bool> vAvail(2, false);
        vAvail[0] = nCode & 2;
        vAvail[1] = nCode & 4;
        unsigned int nMaskCode = (nCode / 8) + ((nCode & 6) != 0 ? 0 : 1);
        // spentness bitmask
        while (nMaskCode > 0) {
            unsigned char chAvail = 0;
            ::Unserialize(s, chAvail, nType, nVersion);
            for (unsigned int p = 0; p < 8; p++) {
                bool f = (chAvail & (1 << p)) != 0;
                vAvail.push_back(f);
            }
            if (chAvail != 0)
                nMaskCode--;
        }
        // txouts themself
        vout.assign(vAvail.size(), Bitcoin_CTxOut());
        for (unsigned int i = 0; i < vAvail.size(); i++) {
            if (vAvail[i])
                ::Unserialize(s, REF(Bitcoin_CTxOutCompressor(vout[i])), nType, nVersion);
        }

        // coinbase height
        ::Unserialize(s, VARINT(nHeight), nType, nVersion);

        Cleanup();
    }

    // mark an outpoint spent, and construct undo information
    bool Bitcoin_Spend(const COutPoint &out);
    //This function is NOT the claim spending function, it merely moves the txout set forward
    bool Claim_Spend(const COutPoint &out, Bitcoin_CTxInUndo &undo);
    //This is the claim spending function
    bool Credits_Spend(const COutPoint &out, Credits_CTxInUndo &undo);

    // check whether a particular output is still available
    bool IsAvailable(unsigned int nPos) const {
        return nPos < vout.size() && !vout[nPos].IsNull();
    }
    // check whether the original bitcoin is available for spending
    bool Original_IsAvailable(unsigned int nPos) const {
        return IsAvailable(nPos) && !vout[nPos].IsOriginalSpent();
    }
    // check whether there are any bitcoins left to claim
    bool Claim_IsAvailable(unsigned int nPos) const {
        return IsAvailable(nPos) && vout[nPos].nValueClaimable > 0;
    }

    // check whether the entire Bitcoin_CCoins is spent
    // note that only !IsPruned() Bitcoin_CCoins can be serialized
    bool IsPruned() const {
        BOOST_FOREACH(const Bitcoin_CTxOut &out, vout)
            if (!out.IsNull())
                return false;
        return true;
    }

    std::string ToString() const;
    void print() const;
};

struct Bitcoin_CCoinsStats
{
    int nHeight;
    uint256 bitcoin_hashBlock;
    uint256 claim_hashBlock;
    uint256 hashCreditsClaimTip;
    int64_t totalClaimedCoins;
    uint64_t nTransactions;
    uint64_t nTransactionOutputsOriginal;
    uint64_t nTransactionOutputsClaimable;
    uint64_t nSerializedSize;
    uint256 hashSerialized;
    int64_t nTotalAmountOriginal;
    int64_t nTotalAmountClaimable;

    Bitcoin_CCoinsStats() : nHeight(0), bitcoin_hashBlock(0), claim_hashBlock(0), hashCreditsClaimTip(0), totalClaimedCoins(0), nTransactions(0), nTransactionOutputsOriginal(0), nTransactionOutputsClaimable(0), nSerializedSize(0), hashSerialized(0), nTotalAmountOriginal(0), nTotalAmountClaimable(0){}
};

/** Abstract view on the open txout dataset. */
class Bitcoin_CCoinsView
{
public:
    // Retrieve the Bitcoin_CCoins (unspent transaction outputs) for a given txid
    virtual bool GetCoins(const uint256 &txid, Bitcoin_CCoins &coins);

    // Modify the Bitcoin_CCoins for a given txid
    virtual bool SetCoins(const uint256 &txid, const Bitcoin_CCoins &coins);

    // Just check whether we have data for a given txid.
    // This may (but cannot always) return true for fully spent transactions
    virtual bool HaveCoins(const uint256 &txid);

    // Retrieve the block hash whose state this Bitcoin_CCoinsView currently represents
    virtual uint256 Bitcoin_GetBestBlock();
    virtual uint256 Claim_GetBestBlock();

    // Modify the currently active block hash
    virtual bool Bitcoin_SetBestBlock(const uint256 &hashBlock);
    virtual bool Claim_SetBestBlock(const uint256 &hashBlock);

    // Retrieve the block hash whose state this view currently represents
    virtual uint256 Claim_GetCreditsClaimTip();
    // Modify the currently active block hash
    virtual bool Claim_SetCreditsClaimTip(const uint256 &hashCreditsClaimTip);

    // Get the total (sum) number of bitcoins that have been claimed
    virtual int64_t Claim_GetTotalClaimedCoins();
    // Modify the currently total claimed coins value
    virtual bool Claim_SetTotalClaimedCoins(const int64_t &totalClaimedCoins);

    // Do a bulk modification (multiple SetCoins + one SetBestBlock)
    virtual bool BatchWrite(const std::map<uint256, Bitcoin_CCoins> &mapCoins, const uint256 &bitcoin_hashBlock, const uint256 &claim_hashBlock, const uint256 &claim_hashCreditsClaimTip, const int64_t &claim_totalClaimedCoins);

    // Calculate statistics about the unspent transaction output set
    virtual bool GetStats(Bitcoin_CCoinsStats &stats);

    // As we use Bitcoin_CCoinsViews polymorphically, have a virtual destructor
    virtual ~Bitcoin_CCoinsView() {}
};


/** Bitcoin_CCoinsView backed by another Bitcoin_CCoinsView */
class Bitcoin_CCoinsViewBacked : public Bitcoin_CCoinsView
{
protected:
    Bitcoin_CCoinsView *base;

public:
    Bitcoin_CCoinsViewBacked(Bitcoin_CCoinsView &viewIn);
    bool GetCoins(const uint256 &txid, Bitcoin_CCoins &coins);
    bool SetCoins(const uint256 &txid, const Bitcoin_CCoins &coins);
    bool HaveCoins(const uint256 &txid);
    uint256 Bitcoin_GetBestBlock();
    uint256 Claim_GetBestBlock();
    bool Bitcoin_SetBestBlock(const uint256 &hashBlock);
    bool Claim_SetBestBlock(const uint256 &hashBlock);
    uint256 Claim_GetCreditsClaimTip();
    bool Claim_SetCreditsClaimTip(const uint256 &hashCreditsClaimTip);
    int64_t Claim_GetTotalClaimedCoins();
    bool Claim_SetTotalClaimedCoins(const int64_t &totalClaimedCoins);
    void SetBackend(Bitcoin_CCoinsView &viewIn);
    Bitcoin_CCoinsView *GetBackend();
    bool BatchWrite(const std::map<uint256, Bitcoin_CCoins> &mapCoins, const uint256 &bitcoin_hashBlock, const uint256 &claim_hashBlock, const uint256 &claim_hashCreditsClaimTip, const int64_t &claim_totalClaimedCoins);
    bool GetStats(Bitcoin_CCoinsStats &stats);
};


/** Bitcoin_CCoinsView that adds a memory cache for transactions to another Bitcoin_CCoinsView */
class Bitcoin_CCoinsViewCache : public Bitcoin_CCoinsViewBacked
{
protected:
    std::map<uint256,Bitcoin_CCoins> cacheCoins;

    uint256 bitcoin_hashBlock;

    uint256 claim_hashBlock;
    uint256 claim_hashCreditsClaimTip;
    int64_t claim_totalClaimedCoins;

public:
    Bitcoin_CCoinsViewCache(Bitcoin_CCoinsView &baseIn, bool fDummy = false);

    // Standard Bitcoin_CCoinsView methods
    bool GetCoins(const uint256 &txid, Bitcoin_CCoins &coins);
    bool SetCoins(const uint256 &txid, const Bitcoin_CCoins &coins);
    bool HaveCoins(const uint256 &txid);
    uint256 Bitcoin_GetBestBlock();
    uint256 Claim_GetBestBlock();
    bool Bitcoin_SetBestBlock(const uint256 &hashBlock);
    bool Claim_SetBestBlock(const uint256 &hashBlock);
    uint256 Claim_GetCreditsClaimTip();
    bool Claim_SetCreditsClaimTip(const uint256 &hashCreditsClaimTip);
    int64_t Claim_GetTotalClaimedCoins();
    bool Claim_SetTotalClaimedCoins(const int64_t &totalClaimedCoins);
    bool BatchWrite(const std::map<uint256, Bitcoin_CCoins> &mapCoins, const uint256 &bitcoin_hashBlock, const uint256 &claim_hashBlock, const uint256 &claim_hashCreditsClaimTip, const int64_t &claim_totalClaimedCoins);

    // Return a modifiable reference to a Bitcoin_CCoins. Check HaveCoins first.
    // Many methods explicitly require a Bitcoin_CCoinsViewCache because of this method, to reduce
    // copying.
    Bitcoin_CCoins &GetCoins(const uint256 &txid);

    // Push the modifications applied to this cache to its base.
    // Failure to call this method before destruction will cause the changes to be forgotten.
    bool Flush();

    // Calculate the size of the cache (in number of transactions)
    unsigned int GetCacheSize();

    /** Amount of bitcoins coming in to a transaction
        Note that lightweight clients may not know anything besides the hash of previous transactions,
        so may not be able to calculate this.

        @param[in] tx	transaction for which we are checking input total
        @return	Sum of value of all inputs (scriptSigs)
     */
    int64_t Bitcoin_GetValueIn(const Bitcoin_CTransaction& tx);
    int64_t Claim_GetValueIn(const Credits_CTransaction& tx);

    // Check whether all prevouts of the transaction are present in the UTXO set represented by this view
    bool Bitcoin_HaveInputs(const Bitcoin_CTransaction& tx);
    bool Claim_HaveInputs(const Credits_CTransaction& tx);

    // Return priority of tx at height nHeight
    double Bitcoin_GetPriority(const Bitcoin_CTransaction &tx, int nHeight);
    double Claim_GetPriority(const Credits_CTransaction &tx, int nHeight);

    const Bitcoin_CTxOut &GetOutputFor(const COutPoint& outPoint, int validationType);
private:
    std::map<uint256,Bitcoin_CCoins>::iterator FetchCoins(const uint256 &txid);
};

#endif
