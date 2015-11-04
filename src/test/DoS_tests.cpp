// Copyright (c) 2011-2014 The Bitcoin Core developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

//
// Unit tests for denial-of-service detection/prevention code
//



#include "keystore.h"
#include "main.h"
#include "net.h"
#include "script.h"
#include "serialize.h"

#include <stdint.h>

#include <boost/assign/list_of.hpp> // for 'map_list_of()'
#include <boost/date_time/posix_time/posix_time_types.hpp>
#include <boost/foreach.hpp>
#include <boost/test/unit_test.hpp>

// Tests this internal-to-main.cpp method:
extern bool Bitcredit_AddOrphanTx(const Credits_CTransaction& tx);
extern unsigned int Bitcredit_LimitOrphanTxSize(unsigned int nMaxOrphans);
extern std::map<uint256, Credits_CTransaction> bitcredit_mapOrphanTransactions;
extern std::map<uint256, std::set<uint256> > bitcredit_mapOrphanTransactionsByPrev;

CService ip(uint32_t i)
{
    struct in_addr s;
    s.s_addr = i;
    return CService(CNetAddr(s), Credits_NetParams()->GetDefaultPort());
}

BOOST_AUTO_TEST_SUITE(DoS_tests)

BOOST_AUTO_TEST_CASE(DoS_banning)
{
    CNode::ClearBanned();
    CAddress addr1(ip(0xa0b0c001));
    CNode dummyNode1(INVALID_SOCKET, addr1, "", true, Credits_NetParams());
    dummyNode1.nVersion = 1;
    Bitcredit_Misbehaving(dummyNode1.GetId(), 100); // Should get banned
    Bitcredit_SendMessages(&dummyNode1, false);
    BOOST_CHECK(CNode::IsBanned(addr1));
    BOOST_CHECK(!CNode::IsBanned(ip(0xa0b0c001|0x0000ff00))); // Different IP, not banned

    CAddress addr2(ip(0xa0b0c002));
    CNode dummyNode2(INVALID_SOCKET, addr2, "", true, Credits_NetParams());
    dummyNode2.nVersion = 1;
    Bitcredit_Misbehaving(dummyNode2.GetId(), 50);
    Bitcredit_SendMessages(&dummyNode2, false);
    BOOST_CHECK(!CNode::IsBanned(addr2)); // 2 not banned yet...
    BOOST_CHECK(CNode::IsBanned(addr1));  // ... but 1 still should be
    Bitcredit_Misbehaving(dummyNode2.GetId(), 50);
    Bitcredit_SendMessages(&dummyNode2, false);
    BOOST_CHECK(CNode::IsBanned(addr2));
}

BOOST_AUTO_TEST_CASE(DoS_banscore)
{
    CNode::ClearBanned();
    mapArgs["-banscore"] = "111"; // because 11 is my favorite number
    CAddress addr1(ip(0xa0b0c001));
    CNode dummyNode1(INVALID_SOCKET, addr1, "", true, Credits_NetParams());
    dummyNode1.nVersion = 1;
    Bitcredit_Misbehaving(dummyNode1.GetId(), 100);
    Bitcredit_SendMessages(&dummyNode1, false);
    BOOST_CHECK(!CNode::IsBanned(addr1));
    Bitcredit_Misbehaving(dummyNode1.GetId(), 10);
    Bitcredit_SendMessages(&dummyNode1, false);
    BOOST_CHECK(!CNode::IsBanned(addr1));
    Bitcredit_Misbehaving(dummyNode1.GetId(), 1);
    Bitcredit_SendMessages(&dummyNode1, false);
    BOOST_CHECK(CNode::IsBanned(addr1));
    mapArgs.erase("-banscore");
}

BOOST_AUTO_TEST_CASE(DoS_bantime)
{
    CNode::ClearBanned();
    int64_t nStartTime = GetTime();
    SetMockTime(nStartTime); // Overrides future calls to GetTime()

    CAddress addr(ip(0xa0b0c001));
    CNode dummyNode(INVALID_SOCKET, addr, "", true, Credits_NetParams());
    dummyNode.nVersion = 1;

    Bitcredit_Misbehaving(dummyNode.GetId(), 100);
    Bitcredit_SendMessages(&dummyNode, false);
    BOOST_CHECK(CNode::IsBanned(addr));

    SetMockTime(nStartTime+60*60);
    BOOST_CHECK(CNode::IsBanned(addr));

    SetMockTime(nStartTime+60*60*24+1);
    BOOST_CHECK(!CNode::IsBanned(addr));
}

static bool CheckNBits(int nHeight, unsigned int nbits1, int64_t time1, unsigned int nbits2, int64_t time2)\
{
    if (time1 > time2)
        return CheckNBits(nHeight, nbits2, time2, nbits1, time1);
    int64_t deltaTime = time2-time1;

    uint256 required;
    required.SetCompact(Bitcredit_ComputeMinWork(nHeight, nbits1, deltaTime));
    uint256 have;
    have.SetCompact(nbits2);
    return (have <= required);
}

BOOST_AUTO_TEST_CASE(DoS_checknbits_V1)
{
	int nHeight = 0;
    using namespace boost::assign; // for 'map_list_of()'

    // Timestamps,nBits from the bitcoin block chain.
    // These are the block-chain checkpoint blocks
    typedef std::map<int64_t, unsigned int> BlockData;
    BlockData chainData =
        map_list_of(1239852051,486604799)(1262749024,486594666)
        (1279305360,469854461)(1280200847,469830746)(1281678674,469809688)
        (1296207707,453179945)(1302624061,453036989)(1309640330,437004818)
        (1313172719,436789733);

    // Make sure CheckNBits considers every combination of block-chain-lock-in-points
    // "sane":
    BOOST_FOREACH(const BlockData::value_type& i, chainData)
    {
        BOOST_FOREACH(const BlockData::value_type& j, chainData)
        {
            BOOST_CHECK(CheckNBits(nHeight, i.second, i.first, j.second, j.first));
        }
    }

    // Test a couple of insane combinations:
    BlockData::value_type firstcheck = *(chainData.begin());
    BlockData::value_type lastcheck = *(chainData.rbegin());

    // First checkpoint difficulty at or a while after the last checkpoint time should fail when
    // compared to last checkpoint
    BOOST_CHECK(!CheckNBits(nHeight, firstcheck.second, lastcheck.first+60*10, lastcheck.second, lastcheck.first));
    BOOST_CHECK(!CheckNBits(nHeight, firstcheck.second, lastcheck.first+60*60*24*14, lastcheck.second, lastcheck.first));

    // ... but OK if enough time passed for difficulty to adjust downward:
    BOOST_CHECK(CheckNBits(nHeight, firstcheck.second, lastcheck.first+60*60*24*365*4, lastcheck.second, lastcheck.first));

    //Some default value testing
    const unsigned int diffBefore = 0x0A00FFFF;
    uint256 uint256DiffBefore;
    uint256DiffBefore.SetCompact(diffBefore);
    uint256 uint256DiffAfter;
    int64_t timeRange;

    timeRange = 14 * 24 * 60 * 60;
    uint256DiffAfter = uint256DiffBefore * 3;
    BOOST_CHECK(CheckNBits(nHeight, diffBefore, 0, uint256DiffAfter.GetCompact(), timeRange));
    uint256DiffAfter = uint256DiffBefore * 5;
    BOOST_CHECK(!CheckNBits(nHeight, diffBefore, 0, uint256DiffAfter.GetCompact(), timeRange));

    timeRange = 14 * 24 * 60 * 60 * 2;
    uint256DiffAfter = uint256DiffBefore * 3;
    BOOST_CHECK(CheckNBits(nHeight, diffBefore, 0, uint256DiffAfter.GetCompact(), timeRange));
    uint256DiffAfter = uint256DiffBefore * 5;
    BOOST_CHECK(!CheckNBits(nHeight, diffBefore, 0, uint256DiffAfter.GetCompact(), timeRange));

    timeRange = 14 * 24 * 60 * 60 * 4;
    uint256DiffAfter = uint256DiffBefore * 3;
    BOOST_CHECK(CheckNBits(nHeight, diffBefore, 0, uint256DiffAfter.GetCompact(), timeRange));
    uint256DiffAfter = uint256DiffBefore * 5;
    BOOST_CHECK(!CheckNBits(nHeight, diffBefore, 0, uint256DiffAfter.GetCompact(), timeRange));

    timeRange = 14 * 24 * 60 * 60 * 4 + 1;
    uint256DiffAfter = uint256DiffBefore * 15;
    BOOST_CHECK(CheckNBits(nHeight, diffBefore, 0, uint256DiffAfter.GetCompact(), timeRange));
    uint256DiffAfter = uint256DiffBefore * 17;
    BOOST_CHECK(!CheckNBits(nHeight, diffBefore, 0, uint256DiffAfter.GetCompact(), timeRange));
}

BOOST_AUTO_TEST_CASE(DoS_checknbits_V2)
{
	int nHeight = 50000;
    using namespace boost::assign; // for 'map_list_of()'

    // Timestamps,nBits from the bitcoin block chain.
    // These are the block-chain checkpoint blocks
    typedef std::map<int64_t, unsigned int> BlockData;
    BlockData chainData =
        map_list_of(1239852051,486604799)(1262749024,486594666)
        (1279305360,469854461)(1280200847,469830746)(1281678674,469809688)
        (1296207707,453179945)(1302624061,453036989)(1309640330,437004818)
        (1313172719,436789733);

    // Make sure CheckNBits considers every combination of block-chain-lock-in-points
    // "sane":
    BOOST_FOREACH(const BlockData::value_type& i, chainData)
    {
        BOOST_FOREACH(const BlockData::value_type& j, chainData)
        {
            BOOST_CHECK(CheckNBits(nHeight, i.second, i.first, j.second, j.first));
        }
    }

    // Test a couple of insane combinations:
    BlockData::value_type firstcheck = *(chainData.begin());
    BlockData::value_type lastcheck = *(chainData.rbegin());

    // First checkpoint difficulty at or a while after the last checkpoint time should fail when
    // compared to last checkpoint
    BOOST_CHECK(!CheckNBits(nHeight, firstcheck.second, lastcheck.first+60*10, lastcheck.second, lastcheck.first));
    BOOST_CHECK(!CheckNBits(nHeight, firstcheck.second, lastcheck.first+60*60*24*14, lastcheck.second, lastcheck.first));

    // ... but OK if enough time passed for difficulty to adjust downward:
    BOOST_CHECK(CheckNBits(nHeight, firstcheck.second, lastcheck.first+60*60*24*365*4, lastcheck.second, lastcheck.first));

    //Some default value testing
    const unsigned int diffBefore = 0x0A00FFFF;
    uint256 uint256DiffBefore;
    uint256DiffBefore.SetCompact(diffBefore);
    uint256 uint256DiffAfter;
    int64_t timeRange;

    timeRange = (14 * 24 * 60 * 60) / 8;
    uint256DiffAfter = uint256DiffBefore * 1;
    BOOST_CHECK(CheckNBits(nHeight, diffBefore, 0, uint256DiffAfter.GetCompact(), timeRange));
    uint256DiffAfter = uint256DiffBefore * 3;
    BOOST_CHECK(!CheckNBits(nHeight, diffBefore, 0, uint256DiffAfter.GetCompact(), timeRange));

    timeRange = ((14 * 24 * 60 * 60) / 8) * 2;
    uint256DiffAfter = uint256DiffBefore * 1;
    BOOST_CHECK(CheckNBits(nHeight, diffBefore, 0, uint256DiffAfter.GetCompact(), timeRange));
    uint256DiffAfter = uint256DiffBefore * 3;
    BOOST_CHECK(!CheckNBits(nHeight, diffBefore, 0, uint256DiffAfter.GetCompact(), timeRange));

    timeRange = ((14 * 24 * 60 * 60) / 8) * 4;
    uint256DiffAfter = uint256DiffBefore * 3;
    BOOST_CHECK(CheckNBits(nHeight, diffBefore, 0, uint256DiffAfter.GetCompact(), timeRange));
    uint256DiffAfter = uint256DiffBefore * 5;
    BOOST_CHECK(!CheckNBits(nHeight, diffBefore, 0, uint256DiffAfter.GetCompact(), timeRange));

    timeRange = (((14 * 24 * 60 * 60) / 8) * 4) + 1;
    uint256DiffAfter = uint256DiffBefore * 7;
    BOOST_CHECK(CheckNBits(nHeight, diffBefore, 0, uint256DiffAfter.GetCompact(), timeRange));
    uint256DiffAfter = uint256DiffBefore * 9;
    BOOST_CHECK(!CheckNBits(nHeight, diffBefore, 0, uint256DiffAfter.GetCompact(), timeRange));
}

Credits_CTransaction RandomOrphan()
{
    std::map<uint256, Credits_CTransaction>::iterator it;
    it = bitcredit_mapOrphanTransactions.lower_bound(GetRandHash());
    if (it == bitcredit_mapOrphanTransactions.end())
        it = bitcredit_mapOrphanTransactions.begin();
    return it->second;
}

BOOST_AUTO_TEST_CASE(DoS_mapOrphans)
{
    CKey key;
    key.MakeNewKey(true);
    CBasicKeyStore keystore;
    keystore.AddKey(key);

    // 50 orphan transactions:
    for (int i = 0; i < 50; i++)
    {
        Credits_CTransaction tx;
        tx.vin.resize(1);
        tx.vin[0].prevout.n = 0;
        tx.vin[0].prevout.hash = GetRandHash();
        tx.vin[0].scriptSig << OP_1;
        tx.vout.resize(1);
        tx.vout[0].nValue = 1*CENT;
        tx.vout[0].scriptPubKey.SetDestination(key.GetPubKey().GetID());

        Bitcredit_AddOrphanTx(tx);
    }

    // ... and 50 that depend on other orphans:
    for (int i = 0; i < 50; i++)
    {
        Credits_CTransaction txPrev = RandomOrphan();

        Credits_CTransaction tx;
        tx.vin.resize(1);
        tx.vin[0].prevout.n = 0;
        tx.vin[0].prevout.hash = txPrev.GetHash();
        tx.vout.resize(1);
        tx.vout[0].nValue = 1*CENT;
        tx.vout[0].scriptPubKey.SetDestination(key.GetPubKey().GetID());

        Credits_SignSignature(keystore, txPrev, tx, 0);

        Bitcredit_AddOrphanTx(tx);
    }

    // This really-big orphan should be ignored:
    for (int i = 0; i < 10; i++)
    {
        Credits_CTransaction txPrev = RandomOrphan();

        Credits_CTransaction tx;
        tx.vout.resize(1);
        tx.vout[0].nValue = 1*CENT;
        tx.vout[0].scriptPubKey.SetDestination(key.GetPubKey().GetID());
        tx.vin.resize(500);
        for (unsigned int j = 0; j < tx.vin.size(); j++)
        {
            tx.vin[j].prevout.n = j;
            tx.vin[j].prevout.hash = txPrev.GetHash();
        }

        Credits_SignSignature(keystore, txPrev, tx, 0);
        // Re-use same signature for other inputs
        // (they don't have to be valid for this test)
        for (unsigned int j = 1; j < tx.vin.size(); j++)
            tx.vin[j].scriptSig = tx.vin[0].scriptSig;

        BOOST_CHECK(!Bitcredit_AddOrphanTx(tx));
    }

    // Test LimitOrphanTxSize() function:
    Bitcredit_LimitOrphanTxSize(40);
    BOOST_CHECK(bitcredit_mapOrphanTransactions.size() <= 40);
    Bitcredit_LimitOrphanTxSize(10);
    BOOST_CHECK(bitcredit_mapOrphanTransactions.size() <= 10);
    Bitcredit_LimitOrphanTxSize(0);
    BOOST_CHECK(bitcredit_mapOrphanTransactions.empty());
    BOOST_CHECK(bitcredit_mapOrphanTransactionsByPrev.empty());
}

BOOST_AUTO_TEST_CASE(DoS_checkSig)
{
    // Test signature caching code (see key.cpp Verify() methods)

    CKey key;
    key.MakeNewKey(true);
    CBasicKeyStore keystore;
    keystore.AddKey(key);
    unsigned int flags = SCRIPT_VERIFY_P2SH | SCRIPT_VERIFY_STRICTENC;

    // 100 orphan transactions:
    static const int NPREV=100;
    Credits_CTransaction orphans[NPREV];
    for (int i = 0; i < NPREV; i++)
    {
        Credits_CTransaction& tx = orphans[i];
        tx.vin.resize(1);
        tx.vin[0].prevout.n = 0;
        tx.vin[0].prevout.hash = GetRandHash();
        tx.vin[0].scriptSig << OP_1;
        tx.vout.resize(1);
        tx.vout[0].nValue = 1*CENT;
        tx.vout[0].scriptPubKey.SetDestination(key.GetPubKey().GetID());

        Bitcredit_AddOrphanTx(tx);
    }

    // Create a transaction that depends on orphans:
    Credits_CTransaction tx;
    tx.vout.resize(1);
    tx.vout[0].nValue = 1*CENT;
    tx.vout[0].scriptPubKey.SetDestination(key.GetPubKey().GetID());
    tx.vin.resize(NPREV);
    for (unsigned int j = 0; j < tx.vin.size(); j++)
    {
        tx.vin[j].prevout.n = 0;
        tx.vin[j].prevout.hash = orphans[j].GetHash();
    }
    // Creating signatures primes the cache:
    boost::posix_time::ptime mst1 = boost::posix_time::microsec_clock::local_time();
    for (unsigned int j = 0; j < tx.vin.size(); j++) {
        BOOST_CHECK(Credits_SignSignature(keystore, orphans[j], tx, j));
    }
    boost::posix_time::ptime mst2 = boost::posix_time::microsec_clock::local_time();
    boost::posix_time::time_duration msdiff = mst2 - mst1;
    long nOneValidate = msdiff.total_milliseconds();
    if (fDebug) printf("DoS_Checksig sign: %ld\n", nOneValidate);

    // ... now validating repeatedly should be quick:
    // 2.8GHz machine, -g build: Sign takes ~760ms,
    // uncached Verify takes ~250ms, cached Verify takes ~50ms
    // (for 100 single-signature inputs)
    mst1 = boost::posix_time::microsec_clock::local_time();
    for (unsigned int i = 0; i < 5; i++)
        for (unsigned int j = 0; j < tx.vin.size(); j++)
            BOOST_CHECK(Bitcredit_VerifySignature(Credits_CCoins(orphans[j], BITCREDIT_MEMPOOL_HEIGHT), tx, j, flags, SIGHASH_ALL));
    mst2 = boost::posix_time::microsec_clock::local_time();
    msdiff = mst2 - mst1;
    long nManyValidate = msdiff.total_milliseconds();
    if (fDebug) printf("DoS_Checksig five: %ld\n", nManyValidate);

    BOOST_CHECK_MESSAGE(nManyValidate < nOneValidate, "Signature cache timing failed");

    // Empty a signature, validation should fail:
    CScript save = tx.vin[0].scriptSig;
    tx.vin[0].scriptSig = CScript();
    BOOST_CHECK(!Bitcredit_VerifySignature(Credits_CCoins(orphans[0], BITCREDIT_MEMPOOL_HEIGHT), tx, 0, flags, SIGHASH_ALL));
    tx.vin[0].scriptSig = save;

    // Swap signatures, validation should fail:
    std::swap(tx.vin[0].scriptSig, tx.vin[1].scriptSig);
    BOOST_CHECK(!Bitcredit_VerifySignature(Credits_CCoins(orphans[0], BITCREDIT_MEMPOOL_HEIGHT), tx, 0, flags, SIGHASH_ALL));
    BOOST_CHECK(!Bitcredit_VerifySignature(Credits_CCoins(orphans[1], BITCREDIT_MEMPOOL_HEIGHT), tx, 1, flags, SIGHASH_ALL));
    std::swap(tx.vin[0].scriptSig, tx.vin[1].scriptSig);

    // Exercise -maxsigcachesize code:
    mapArgs["-maxsigcachesize"] = "10";
    // Generate a new, different signature for vin[0] to trigger cache clear:
    CScript oldSig = tx.vin[0].scriptSig;

    BOOST_CHECK(Credits_SignSignature(keystore, orphans[0], tx, 0));
    BOOST_CHECK(tx.vin[0].scriptSig != oldSig);
    for (unsigned int j = 0; j < tx.vin.size(); j++)
        BOOST_CHECK(Bitcredit_VerifySignature(Credits_CCoins(orphans[j], BITCREDIT_MEMPOOL_HEIGHT), tx, j, flags, SIGHASH_ALL));
    mapArgs.erase("-maxsigcachesize");

    Bitcredit_LimitOrphanTxSize(0);
}

BOOST_AUTO_TEST_SUITE_END()
