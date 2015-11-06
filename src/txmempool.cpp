// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2013 The Bitcoin developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "core.h"
#include "txmempool.h"

#include <boost/circular_buffer.hpp>

using namespace std;

Bitcredit_CTxMemPoolEntry::Bitcredit_CTxMemPoolEntry()
{
    nHeight = BITCREDIT_MEMPOOL_HEIGHT;
}

Bitcredit_CTxMemPoolEntry::Bitcredit_CTxMemPoolEntry(const Credits_CTransaction& _tx, int64_t _nFee,
                                 int64_t _nTime, double _dPriority,
                                 unsigned int _nHeight):
    tx(_tx), nFee(_nFee), nTime(_nTime), dPriority(_dPriority), nHeight(_nHeight)
{
    nTxSize = ::GetSerializeSize(tx, SER_NETWORK, CREDITS_PROTOCOL_VERSION);
}

Bitcredit_CTxMemPoolEntry::Bitcredit_CTxMemPoolEntry(const Bitcredit_CTxMemPoolEntry& other)
{
    *this = other;
}

double
Bitcredit_CTxMemPoolEntry::GetPriority(unsigned int currentHeight) const
{
    int64_t nValueIn = tx.GetValueOut()+nFee;
    double deltaPriority = ((double)(currentHeight-nHeight)*nValueIn)/nTxSize;
    double dResult = dPriority + deltaPriority;
    return dResult;
}

//
// Keep track of fee/priority for transactions confirmed within N blocks
//
class Bitcredit_CBlockAverage
{
private:
    boost::circular_buffer<CFeeRate> feeSamples;
    boost::circular_buffer<double> prioritySamples;

    template<typename T> std::vector<T> buf2vec(boost::circular_buffer<T> buf) const
    {
        std::vector<T> vec(buf.begin(), buf.end());
        return vec;
    }

public:
    Bitcredit_CBlockAverage() : feeSamples(100), prioritySamples(100) { }

    void RecordFee(const CFeeRate& feeRate) {
        feeSamples.push_back(feeRate);
    }

    void RecordPriority(double priority) {
        prioritySamples.push_back(priority);
    }

    size_t FeeSamples() const { return feeSamples.size(); }
    size_t GetFeeSamples(std::vector<CFeeRate>& insertInto) const
    {
        BOOST_FOREACH(const CFeeRate& f, feeSamples)
            insertInto.push_back(f);
        return feeSamples.size();
    }
    size_t PrioritySamples() const { return prioritySamples.size(); }
    size_t GetPrioritySamples(std::vector<double>& insertInto) const
    {
        BOOST_FOREACH(double d, prioritySamples)
            insertInto.push_back(d);
        return prioritySamples.size();
    }

    // Used as belt-and-suspenders check when reading to detect
    // file corruption
    bool AreSane(const std::vector<CFeeRate>& vecFee)
    {
        BOOST_FOREACH(CFeeRate fee, vecFee)
        {
            if (fee < CFeeRate(0))
                return false;
            if (fee.GetFee(1000) > Credits_CTransaction::minRelayTxFee.GetFee(1000) * 10000)
                return false;
        }
        return true;
    }
    bool AreSane(const std::vector<double> vecPriority)
    {
        BOOST_FOREACH(double priority, vecPriority)
        {
            if (priority < 0)
                return false;
        }
        return true;
    }

    void Write(CAutoFile& fileout) const
    {
        std::vector<CFeeRate> vecFee = buf2vec(feeSamples);
        fileout << vecFee;
        std::vector<double> vecPriority = buf2vec(prioritySamples);
        fileout << vecPriority;
    }

    void Read(CAutoFile& filein) {
        std::vector<CFeeRate> vecFee;
        filein >> vecFee;
        if (AreSane(vecFee))
            feeSamples.insert(feeSamples.end(), vecFee.begin(), vecFee.end());
        else
            throw runtime_error("Corrupt fee value in estimates file.");
        std::vector<double> vecPriority;
        filein >> vecPriority;
        if (AreSane(vecPriority))
            prioritySamples.insert(prioritySamples.end(), vecPriority.begin(), vecPriority.end());
        else
            throw runtime_error("Corrupt priority value in estimates file.");
        if (feeSamples.size() + prioritySamples.size() > 0)
            LogPrint("estimatefee", "Read %d fee samples and %d priority samples\n",
                     feeSamples.size(), prioritySamples.size());
    }
};

class Bitcredit_CMinerPolicyEstimator
{
private:
    // Records observed averages transactions that confirmed within one block, two blocks,
    // three blocks etc.
    std::vector<Bitcredit_CBlockAverage> history;
    std::vector<CFeeRate> sortedFeeSamples;
    std::vector<double> sortedPrioritySamples;

    int nBestSeenHeight;

    // nBlocksAgo is 0 based, i.e. transactions that confirmed in the highest seen block are
    // nBlocksAgo == 0, transactions in the block before that are nBlocksAgo == 1 etc.
    void seenTxConfirm(CFeeRate feeRate, double dPriority, int nBlocksAgo)
    {
        // Last entry records "everything else".
        int nBlocksTruncated = min(nBlocksAgo, (int) history.size() - 1);
        assert(nBlocksTruncated >= 0);

        // We need to guess why the transaction was included in a block-- either
        // because it is high-priority or because it has sufficient fees.
        bool sufficientFee = (feeRate > Credits_CTransaction::minRelayTxFee);
        bool sufficientPriority = Credits_AllowFree(dPriority);
        const char* assignedTo = "unassigned";
        if (sufficientFee && !sufficientPriority)
        {
            history[nBlocksTruncated].RecordFee(feeRate);
            assignedTo = "fee";
        }
        else if (sufficientPriority && !sufficientFee)
        {
            history[nBlocksTruncated].RecordPriority(dPriority);
            assignedTo = "priority";
        }
        else
        {
            // Neither or both fee and priority sufficient to get confirmed:
            // don't know why they got confirmed.
        }
        LogPrint("estimatefee", "Seen TX confirm: %s : %s fee/%g priority, took %d blocks\n",
                 assignedTo, feeRate.ToString(), dPriority, nBlocksAgo);
    }

public:
    Bitcredit_CMinerPolicyEstimator(int nEntries) : nBestSeenHeight(0)
    {
        history.resize(nEntries);
    }

    void seenBlock(const std::vector<Bitcredit_CTxMemPoolEntry>& entries, int nBlockHeight)
    {
        if (nBlockHeight <= nBestSeenHeight)
        {
            // Ignore side chains and re-orgs; assuming they are random
            // they don't affect the estimate.
            // And if an attacker can re-org the chain at will, then
            // you've got much bigger problems than "attacker can influence
            // transaction fees."
            return;
        }
        nBestSeenHeight = nBlockHeight;

        // Fill up the history buckets based on how long transactions took
        // to confirm.
        std::vector<std::vector<const Bitcredit_CTxMemPoolEntry*> > entriesByConfirmations;
        entriesByConfirmations.resize(history.size());
        BOOST_FOREACH(const Bitcredit_CTxMemPoolEntry& entry, entries)
        {
            // How many blocks did it take for miners to include this transaction?
            int delta = nBlockHeight - entry.GetHeight();
            if (delta <= 0)
            {
                // Re-org made us lose height, this should only happen if we happen
                // to re-org on a difficulty transition point: very rare!
                continue;
            }
            if ((delta-1) >= (int)history.size())
                delta = history.size(); // Last bucket is catch-all
            entriesByConfirmations[delta-1].push_back(&entry);
        }
        for (size_t i = 0; i < entriesByConfirmations.size(); i++)
        {
            std::vector<const Bitcredit_CTxMemPoolEntry*> &e = entriesByConfirmations.at(i);
            // Insert at most 10 random entries per bucket, otherwise a single block
            // can dominate an estimate:
            if (e.size() > 10) {
                std::random_shuffle(e.begin(), e.end());
                e.resize(10);
            }
            BOOST_FOREACH(const Bitcredit_CTxMemPoolEntry* entry, e)
            {
                // Fees are stored and reported as BTC-per-kb:
                CFeeRate feeRate(entry->GetFee(), entry->GetTxSize());
                double dPriority = entry->GetPriority(entry->GetHeight()); // Want priority when it went IN
                seenTxConfirm(feeRate, dPriority, i);
            }
        }
        for (size_t i = 0; i < history.size(); i++) {
            if (history[i].FeeSamples() + history[i].PrioritySamples() > 0)
                LogPrint("estimatefee", "estimates: for confirming within %d blocks based on %d/%d samples, fee=%s, prio=%g\n",
                         i,
                         history[i].FeeSamples(), history[i].PrioritySamples(),
                         estimateFee(i+1).ToString(), estimatePriority(i+1));
        }
        sortedFeeSamples.clear();
        sortedPrioritySamples.clear();
    }

    // Can return CFeeRate(0) if we don't have any data for that many blocks back. nBlocksToConfirm is 1 based.
    CFeeRate estimateFee(int nBlocksToConfirm)
    {
        nBlocksToConfirm--;

        if (nBlocksToConfirm < 0 || nBlocksToConfirm >= (int)history.size())
            return CFeeRate(0);

        if (sortedFeeSamples.size() == 0)
        {
            for (size_t i = 0; i < history.size(); i++)
                history.at(i).GetFeeSamples(sortedFeeSamples);
            std::sort(sortedFeeSamples.begin(), sortedFeeSamples.end(),
                      std::greater<CFeeRate>());
        }
        if (sortedFeeSamples.size() == 0)
            return CFeeRate(0);

        int nBucketSize = history.at(nBlocksToConfirm).FeeSamples();

        // Estimates should not increase as number of confirmations goes up,
        // but the estimates are noisy because confirmations happen discretely
        // in blocks. To smooth out the estimates, use all samples in the history
        // and use the nth highest where n is (number of samples in previous bucket +
        // half the samples in nBlocksToConfirm bucket):
        size_t nPrevSize = 0;
        for (int i = 0; i < nBlocksToConfirm; i++)
            nPrevSize += history.at(i).FeeSamples();
        size_t index = min(nPrevSize + nBucketSize/2, sortedFeeSamples.size()-1);
        return sortedFeeSamples[index];
    }
    double estimatePriority(int nBlocksToConfirm)
    {
        nBlocksToConfirm--;

        if (nBlocksToConfirm < 0 || nBlocksToConfirm >= (int)history.size())
            return -1;

        if (sortedPrioritySamples.size() == 0)
        {
            for (size_t i = 0; i < history.size(); i++)
                history.at(i).GetPrioritySamples(sortedPrioritySamples);
            std::sort(sortedPrioritySamples.begin(), sortedPrioritySamples.end(),
                      std::greater<double>());
        }
        if (sortedPrioritySamples.size() == 0)
            return -1.0;

        int nBucketSize = history.at(nBlocksToConfirm).PrioritySamples();

        // Estimates should not increase as number of confirmations needed goes up,
        // but the estimates are noisy because confirmations happen discretely
        // in blocks. To smooth out the estimates, use all samples in the history
        // and use the nth highest where n is (number of samples in previous buckets +
        // half the samples in nBlocksToConfirm bucket).
        size_t nPrevSize = 0;
        for (int i = 0; i < nBlocksToConfirm; i++)
            nPrevSize += history.at(i).PrioritySamples();
        size_t index = min(nPrevSize + nBucketSize/2, sortedFeeSamples.size()-1);
        return sortedPrioritySamples[index];
    }

    void Write(CAutoFile& fileout) const
    {
        fileout << nBestSeenHeight;
        fileout << history.size();
        BOOST_FOREACH(const Bitcredit_CBlockAverage& entry, history)
        {
            entry.Write(fileout);
        }
    }

    void Read(CAutoFile& filein)
    {
        filein >> nBestSeenHeight;
        size_t numEntries;
        filein >> numEntries;
        history.clear();
        for (size_t i = 0; i < numEntries; i++)
        {
        	Bitcredit_CBlockAverage entry;
            entry.Read(filein);
            history.push_back(entry);
        }
    }
};

Bitcredit_CTxMemPool::Bitcredit_CTxMemPool()
{
    // Sanity checks off by default for performance, because otherwise
    // accepting transactions becomes O(N^2) where N is the number
    // of transactions in the pool
    fSanityCheck = false;

    // 25 blocks is a compromise between using a lot of disk/memory and
    // trying to give accurate estimates to people who might be willing
    // to wait a day or two to save a fraction of a penny in fees.
    // Confirmation times for very-low-fee transactions that take more
    // than an hour or three to confirm are highly variable.
    minerPolicyEstimator = new Bitcredit_CMinerPolicyEstimator(25);
}

Bitcredit_CTxMemPool::~Bitcredit_CTxMemPool()
{
    delete minerPolicyEstimator;
}

void Bitcredit_CTxMemPool::pruneSpent(const uint256 &hashTx, Credits_CCoins &coins)
{
    LOCK(cs);

    std::map<COutPoint, Credits_CInPoint>::iterator it = mapNextTx.lower_bound(COutPoint(hashTx, 0));

    // iterate over all COutPoints in mapNextTx whose hash equals the provided hashTx
    while (it != mapNextTx.end() && it->first.hash == hashTx) {
        coins.Spend(it->first.n); // and remove those outputs from coins
        it++;
    }
}

unsigned int Bitcredit_CTxMemPool::GetTransactionsUpdated() const
{
    LOCK(cs);
    return nTransactionsUpdated;
}

void Bitcredit_CTxMemPool::AddTransactionsUpdated(unsigned int n)
{
    LOCK(cs);
    nTransactionsUpdated += n;
}


bool Bitcredit_CTxMemPool::addUnchecked(const uint256& hash, const Bitcredit_CTxMemPoolEntry &entry)
{
    // Add to memory pool without checking anything.
    // Used by main.cpp AcceptToMemoryPool(), which DOES do
    // all the appropriate checks.
    LOCK(cs);
    {
        mapTx[hash] = entry;
        const Credits_CTransaction& tx = mapTx[hash].GetTx();
        for (unsigned int i = 0; i < tx.vin.size(); i++)
            mapNextTx[tx.vin[i].prevout] = Credits_CInPoint(&tx, i);
        nTransactionsUpdated++;
    }
    return true;
}


void Bitcredit_CTxMemPool::remove(const Credits_CTransaction &tx, std::list<Credits_CTransaction>& removed, bool fRecursive)
{
    // Remove transaction from memory pool
    {
        LOCK(cs);
        uint256 hash = tx.GetHash();
        if (fRecursive) {
            for (unsigned int i = 0; i < tx.vout.size(); i++) {
                std::map<COutPoint, Credits_CInPoint>::iterator it = mapNextTx.find(COutPoint(hash, i));
                if (it == mapNextTx.end())
                    continue;
                remove(*it->second.ptx, removed, true);
            }
        }
        if (mapTx.count(hash))
        {
            removed.push_front(tx);
            BOOST_FOREACH(const Credits_CTxIn& txin, tx.vin)
                mapNextTx.erase(txin.prevout);
            mapTx.erase(hash);
            nTransactionsUpdated++;
        }
    }
}

void Bitcredit_CTxMemPool::removeConflicts(const Credits_CTransaction &tx, std::list<Credits_CTransaction>& removed)
{
    // Remove transactions which depend on inputs of tx, recursively
    list<Credits_CTransaction> result;
    LOCK(cs);
    BOOST_FOREACH(const Credits_CTxIn &txin, tx.vin) {
        std::map<COutPoint, Credits_CInPoint>::iterator it = mapNextTx.find(txin.prevout);
        if (it != mapNextTx.end()) {
            const Credits_CTransaction &txConflict = *it->second.ptx;
            if (txConflict != tx)
            {
                remove(txConflict, removed, true);
            }
        }
    }
}

// Called when a block is connected. Removes from mempool and updates the miner fee estimator.
void Bitcredit_CTxMemPool::removeForBlock(const std::vector<Credits_CTransaction>& vtx, unsigned int nBlockHeight,
                                std::list<Credits_CTransaction>& conflicts)
{
    LOCK(cs);
    std::vector<Bitcredit_CTxMemPoolEntry> entries;
    BOOST_FOREACH(const Credits_CTransaction& tx, vtx)
    {
        uint256 hash = tx.GetHash();
        if (mapTx.count(hash))
            entries.push_back(mapTx[hash]);
    }
    minerPolicyEstimator->seenBlock(entries, nBlockHeight);
    BOOST_FOREACH(const Credits_CTransaction& tx, vtx)
    {
        std::list<Credits_CTransaction> dummy;
        remove(tx, dummy, false);
        removeConflicts(tx, conflicts);
    }
}


void Bitcredit_CTxMemPool::clear()
{
    LOCK(cs);
    mapTx.clear();
    mapNextTx.clear();
    ++nTransactionsUpdated;
}

void Bitcredit_CTxMemPool::check(Credits_CCoinsViewCache *pcoins) const
{
    if (!fSanityCheck)
        return;

    LogPrint("mempool", "Checking mempool with %u transactions and %u inputs\n", (unsigned int)mapTx.size(), (unsigned int)mapNextTx.size());

    LOCK(cs);
    for (std::map<uint256, Bitcredit_CTxMemPoolEntry>::const_iterator it = mapTx.begin(); it != mapTx.end(); it++) {
        unsigned int i = 0;
        const Credits_CTransaction& tx = it->second.GetTx();
        BOOST_FOREACH(const Credits_CTxIn &txin, tx.vin) {
            // Check that every mempool transaction's inputs refer to available coins, or other mempool tx's.
            std::map<uint256, Bitcredit_CTxMemPoolEntry>::const_iterator it2 = mapTx.find(txin.prevout.hash);
            if (it2 != mapTx.end()) {
                const Credits_CTransaction& tx2 = it2->second.GetTx();
                assert(tx2.vout.size() > txin.prevout.n && !tx2.vout[txin.prevout.n].IsNull());
            } else {
            	if(tx.IsClaim()) {
            		Claim_CCoins &coins = pcoins->Claim_GetCoins(txin.prevout.hash);
            		assert(coins.HasClaimable(txin.prevout.n));
            	} else {
            		Credits_CCoins &coins = pcoins->Credits_GetCoins(txin.prevout.hash);
            		assert(coins.IsAvailable(txin.prevout.n));
                }
            }
            // Check whether its inputs are marked in mapNextTx.
            std::map<COutPoint, Credits_CInPoint>::const_iterator it3 = mapNextTx.find(txin.prevout);
            assert(it3 != mapNextTx.end());
            assert(it3->second.ptx == &tx);
            assert(it3->second.n == i);
            i++;
        }
    }
    for (std::map<COutPoint, Credits_CInPoint>::const_iterator it = mapNextTx.begin(); it != mapNextTx.end(); it++) {
        uint256 hash = it->second.ptx->GetHash();
        map<uint256, Bitcredit_CTxMemPoolEntry>::const_iterator it2 = mapTx.find(hash);
        const Credits_CTransaction& tx = it2->second.GetTx();
        assert(it2 != mapTx.end());
        assert(&tx == it->second.ptx);
        assert(tx.vin.size() > it->second.n);
        assert(it->first == it->second.ptx->vin[it->second.n].prevout);
    }
}

void Bitcredit_CTxMemPool::queryHashes(vector<uint256>& vtxid)
{
    vtxid.clear();

    LOCK(cs);
    vtxid.reserve(mapTx.size());
    for (map<uint256, Bitcredit_CTxMemPoolEntry>::iterator mi = mapTx.begin(); mi != mapTx.end(); ++mi)
        vtxid.push_back((*mi).first);
}

bool Bitcredit_CTxMemPool::lookup(uint256 hash, Credits_CTransaction& result) const
{
    LOCK(cs);
    map<uint256, Bitcredit_CTxMemPoolEntry>::const_iterator i = mapTx.find(hash);
    if (i == mapTx.end()) return false;
    result = i->second.GetTx();
    return true;
}

CFeeRate Bitcredit_CTxMemPool::estimateFee(int nBlocks) const
{
    LOCK(cs);
    return minerPolicyEstimator->estimateFee(nBlocks);
}
double Bitcredit_CTxMemPool::estimatePriority(int nBlocks) const
{
    LOCK(cs);
    return minerPolicyEstimator->estimatePriority(nBlocks);
}

bool
Bitcredit_CTxMemPool::WriteFeeEstimates(CAutoFile& fileout) const
{
    try {
        LOCK(cs);
        fileout << 99900; // version required to read: 0.9.99 or later
        fileout << CREDITS_CLIENT_VERSION; // version that wrote the file
        minerPolicyEstimator->Write(fileout);
    }
    catch (std::exception &e) {
        LogPrintf("CTxMemPool::WriteFeeEstimates() : unable to write policy estimator data (non-fatal)");
        return false;
    }
    return true;
}

bool
Bitcredit_CTxMemPool::ReadFeeEstimates(CAutoFile& filein)
{
    try {
        int nVersionRequired, nVersionThatWrote;
        filein >> nVersionRequired >> nVersionThatWrote;
        if (nVersionRequired > CREDITS_CLIENT_VERSION)
            return error("CTxMemPool::ReadFeeEstimates() : up-version (%d) fee estimate file", nVersionRequired);

        LOCK(cs);
        minerPolicyEstimator->Read(filein);
    }
    catch (std::exception &e) {
        LogPrintf("CTxMemPool::ReadFeeEstimates() : unable to read policy estimator data (non-fatal)");
        return false;
    }
    return true;
}


Credits_CCoinsViewMemPool::Credits_CCoinsViewMemPool(Credits_CCoinsView &baseIn, Bitcredit_CTxMemPool &mempoolIn) : Credits_CCoinsViewBacked(baseIn), mempool(mempoolIn) { }

bool Credits_CCoinsViewMemPool::GetCoins(const uint256 &txid, Credits_CCoins &coins) {
    if (base->Credits_GetCoins(txid, coins))
        return true;
    Credits_CTransaction tx;
    if (mempool.lookup(txid, tx)) {
        coins = Credits_CCoins(tx, BITCREDIT_MEMPOOL_HEIGHT);
        return true;
    }
    return false;
}

bool Credits_CCoinsViewMemPool::HaveCoins(const uint256 &txid) {
    return mempool.exists(txid) || base->Credits_HaveCoins(txid);
}

