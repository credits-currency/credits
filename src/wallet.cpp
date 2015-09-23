// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2014 The Bitcoin developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "wallet.h"
#include "bitcoin_wallet.h"

#include "base58.h"
#include "checkpoints.h"
#include "coincontrol.h"
#include "net.h"

#include <boost/algorithm/string/replace.hpp>
#include <openssl/rand.h>

using namespace std;

// Settings
int64_t credits_nTransactionFee = CREDITS_DEFAULT_TRANSACTION_FEE;
bool credits_bSpendZeroConfChange = true;

//////////////////////////////////////////////////////////////////////////////
//
// mapWallet
//

struct Credits_CompareValueOnly
{
    bool operator()(const pair<int64_t, pair<const Credits_CWalletTx*, unsigned int> >& t1,
                    const pair<int64_t, pair<const Credits_CWalletTx*, unsigned int> >& t2) const
    {
        return t1.first < t2.first;
    }
};

const Credits_CWalletTx* Credits_CWallet::GetWalletTx(const uint256& hash) const
{
    LOCK(cs_wallet);
    std::map<uint256, Credits_CWalletTx>::const_iterator it = mapWallet.find(hash);
    if (it == mapWallet.end())
        return NULL;
    return &(it->second);
}
void Credits_CWallet::GetAllWalletTxs(vector<const Credits_CWalletTx*> &vWalletTxs) const
{
    LOCK(cs_wallet);
    for (std::map<uint256, Credits_CWalletTx>::const_iterator it = mapWallet.begin(); it != mapWallet.end(); ++it) {
    	vWalletTxs.push_back(&(it->second));
    }
}

CPubKey Credits_CWallet::GenerateNewKey()
{
    AssertLockHeld(cs_wallet); // mapKeyMetadata
    bool fCompressed = CanSupportFeature(CREDITS_FEATURE_COMPRPUBKEY); // default to compressed public keys if we want 0.6.0 wallets

    RandAddSeedPerfmon();
    CKey secret;
    secret.MakeNewKey(fCompressed);

    // Compressed public keys were introduced in version 0.6.0
    if (fCompressed)
        SetMinVersion(CREDITS_FEATURE_COMPRPUBKEY);

    CPubKey pubkey = secret.GetPubKey();

    // Create new metadata
    int64_t nCreationTime = GetTime();
    mapKeyMetadata[pubkey.GetID()] = Credits_CKeyMetadata(nCreationTime);
    if (!nTimeFirstKey || nCreationTime < nTimeFirstKey)
        nTimeFirstKey = nCreationTime;

    bool added = AddKeyPubKey(secret, pubkey);
    assert_with_stacktrace(added, "CWallet::GenerateNewKey() : AddKey failed");
    return pubkey;
}

bool Credits_CWallet::AddKeyPubKey(const CKey& secret, const CPubKey &pubkey)
{
    AssertLockHeld(cs_wallet); // mapKeyMetadata
    if (!CCryptoKeyStore::AddKeyPubKey(secret, pubkey))
        return false;
    if (!fFileBacked)
        return true;
    if (!IsCrypted()) {
        return Credits_CWalletDB(strWalletFile, pbitDb).WriteKey(pubkey,
                                                 secret.GetPrivKey(),
                                                 mapKeyMetadata[pubkey.GetID()]);
    }
    return true;
}

bool Credits_CWallet::AddCryptedKey(const CPubKey &vchPubKey,
                            const vector<unsigned char> &vchCryptedSecret)
{
    if (!CCryptoKeyStore::AddCryptedKey(vchPubKey, vchCryptedSecret))
        return false;
    if (!fFileBacked)
        return true;
    {
        LOCK(cs_wallet);
        if (pwalletdbEncryption)
            return pwalletdbEncryption->WriteCryptedKey(vchPubKey,
                                                        vchCryptedSecret,
                                                        mapKeyMetadata[vchPubKey.GetID()]);
        else
            return Credits_CWalletDB(strWalletFile, pbitDb).WriteCryptedKey(vchPubKey,
                                                            vchCryptedSecret,
                                                            mapKeyMetadata[vchPubKey.GetID()]);
    }
    return false;
}

bool Credits_CWallet::LoadKeyMetadata(const CPubKey &pubkey, const Credits_CKeyMetadata &meta)
{
    AssertLockHeld(cs_wallet); // mapKeyMetadata
    if (meta.nCreateTime && (!nTimeFirstKey || meta.nCreateTime < nTimeFirstKey))
        nTimeFirstKey = meta.nCreateTime;

    mapKeyMetadata[pubkey.GetID()] = meta;
    return true;
}

bool Credits_CWallet::LoadCryptedKey(const CPubKey &vchPubKey, const std::vector<unsigned char> &vchCryptedSecret)
{
    return CCryptoKeyStore::AddCryptedKey(vchPubKey, vchCryptedSecret);
}

bool Credits_CWallet::AddCScript(const CScript& redeemScript)
{
    if (!CCryptoKeyStore::AddCScript(redeemScript))
        return false;
    if (!fFileBacked)
        return true;
    return Credits_CWalletDB(strWalletFile, pbitDb).WriteCScript(Hash160(redeemScript), redeemScript);
}

bool Credits_CWallet::Unlock(const SecureString& strWalletPassphrase)
{
    CCrypter crypter;
    CKeyingMaterial vMasterKey;

    {
        LOCK(cs_wallet);
        BOOST_FOREACH(const MasterKeyMap::value_type& pMasterKey, mapMasterKeys)
        {
            if(!crypter.SetKeyFromPassphrase(strWalletPassphrase, pMasterKey.second.vchSalt, pMasterKey.second.nDeriveIterations, pMasterKey.second.nDerivationMethod))
                return false;
            if (!crypter.Decrypt(pMasterKey.second.vchCryptedKey, vMasterKey))
                continue; // try another master key
            if (CCryptoKeyStore::Unlock(vMasterKey))
                return true;
        }
    }
    return false;
}

bool Credits_CWallet::ChangeWalletPassphrase(const SecureString& strOldWalletPassphrase, const SecureString& strNewWalletPassphrase)
{
    bool fWasLocked = IsLocked();

    {
        LOCK(cs_wallet);
        Lock();

        CCrypter crypter;
        CKeyingMaterial vMasterKey;
        BOOST_FOREACH(MasterKeyMap::value_type& pMasterKey, mapMasterKeys)
        {
            if(!crypter.SetKeyFromPassphrase(strOldWalletPassphrase, pMasterKey.second.vchSalt, pMasterKey.second.nDeriveIterations, pMasterKey.second.nDerivationMethod))
                return false;
            if (!crypter.Decrypt(pMasterKey.second.vchCryptedKey, vMasterKey))
                return false;
            if (CCryptoKeyStore::Unlock(vMasterKey))
            {
                int64_t nStartTime = GetTimeMillis();
                crypter.SetKeyFromPassphrase(strNewWalletPassphrase, pMasterKey.second.vchSalt, pMasterKey.second.nDeriveIterations, pMasterKey.second.nDerivationMethod);
                pMasterKey.second.nDeriveIterations = pMasterKey.second.nDeriveIterations * (100 / ((double)(GetTimeMillis() - nStartTime)));

                nStartTime = GetTimeMillis();
                crypter.SetKeyFromPassphrase(strNewWalletPassphrase, pMasterKey.second.vchSalt, pMasterKey.second.nDeriveIterations, pMasterKey.second.nDerivationMethod);
                pMasterKey.second.nDeriveIterations = (pMasterKey.second.nDeriveIterations + pMasterKey.second.nDeriveIterations * 100 / ((double)(GetTimeMillis() - nStartTime))) / 2;

                if (pMasterKey.second.nDeriveIterations < 25000)
                    pMasterKey.second.nDeriveIterations = 25000;

                LogPrintf("Wallet passphrase changed to an nDeriveIterations of %i\n", pMasterKey.second.nDeriveIterations);

                if (!crypter.SetKeyFromPassphrase(strNewWalletPassphrase, pMasterKey.second.vchSalt, pMasterKey.second.nDeriveIterations, pMasterKey.second.nDerivationMethod))
                    return false;
                if (!crypter.Encrypt(vMasterKey, pMasterKey.second.vchCryptedKey))
                    return false;
                Credits_CWalletDB(strWalletFile, pbitDb).WriteMasterKey(pMasterKey.first, pMasterKey.second);
                if (fWasLocked)
                    Lock();
                return true;
            }
        }
    }

    return false;
}

void Credits_CWallet::SetBestChain(const CBlockLocator& loc)
{
    Credits_CWalletDB walletdb(strWalletFile, pbitDb);
    walletdb.WriteBestBlock(loc);
}

bool Credits_CWallet::SetMinVersion(enum Credits_WalletFeature nVersion, Credits_CWalletDB* pwalletdbIn, bool fExplicit)
{
    LOCK(cs_wallet); // nWalletVersion
    if (nWalletVersion >= nVersion)
        return true;

    // when doing an explicit upgrade, if we pass the max version permitted, upgrade all the way
    if (fExplicit && nVersion > nWalletMaxVersion)
            nVersion = CREDITS_FEATURE_LATEST;

    nWalletVersion = nVersion;

    if (nVersion > nWalletMaxVersion)
        nWalletMaxVersion = nVersion;

    if (fFileBacked)
    {
        Credits_CWalletDB* pwalletdb = pwalletdbIn ? pwalletdbIn : new Credits_CWalletDB(strWalletFile, pbitDb);
        if (nWalletVersion > 40000)
            pwalletdb->WriteMinVersion(nWalletVersion);
        if (!pwalletdbIn)
            delete pwalletdb;
    }

    return true;
}

bool Credits_CWallet::SetMaxVersion(int nVersion)
{
    LOCK(cs_wallet); // nWalletVersion, nWalletMaxVersion
    // cannot downgrade below current version
    if (nWalletVersion > nVersion)
        return false;

    nWalletMaxVersion = nVersion;

    return true;
}

set<uint256> Credits_CWallet::GetConflicts(const uint256& txid) const
{
    set<uint256> result;
    AssertLockHeld(cs_wallet);

    std::map<uint256, Credits_CWalletTx>::const_iterator it = mapWallet.find(txid);
    if (it == mapWallet.end())
        return result;
    const Credits_CWalletTx& wtx = it->second;

    std::pair<TxSpends::const_iterator, TxSpends::const_iterator> range;

    BOOST_FOREACH(const Credits_CTxIn& txin, wtx.vin)
    {
        if (mapTxSpends.count(txin.prevout) <= 1)
            continue;  // No conflict if zero or one spends
        range = mapTxSpends.equal_range(txin.prevout);
        for (TxSpends::const_iterator it = range.first; it != range.second; ++it)
            result.insert(it->second);
    }
    return result;
}

void Credits_CWallet::SyncMetaData(pair<TxSpends::iterator, TxSpends::iterator> range)
{
    // We want all the wallet transactions in range to have the same metadata as
    // the oldest (smallest nOrderPos).
    // So: find smallest nOrderPos:

    int nMinOrderPos = std::numeric_limits<int>::max();
    const Credits_CWalletTx* copyFrom = NULL;
    for (TxSpends::iterator it = range.first; it != range.second; ++it)
    {
        const uint256& hash = it->second;
        int n = mapWallet[hash].nOrderPos;
        if (n < nMinOrderPos)
        {
            nMinOrderPos = n;
            copyFrom = &mapWallet[hash];
        }
    }
    // Now copy data from copyFrom to rest:
    for (TxSpends::iterator it = range.first; it != range.second; ++it)
    {
        const uint256& hash = it->second;
        Credits_CWalletTx* copyTo = &mapWallet[hash];
        if (copyFrom == copyTo) continue;
        copyTo->mapValue = copyFrom->mapValue;
        copyTo->vOrderForm = copyFrom->vOrderForm;
        // fTimeReceivedIsTxTime not copied on purpose
        // nTimeReceived not copied on purpose
        copyTo->nTimeSmart = copyFrom->nTimeSmart;
        copyTo->fFromMe = copyFrom->fFromMe;
        copyTo->strFromAccount = copyFrom->strFromAccount;
        // nOrderPos not copied on purpose
        // cached members not copied on purpose
    }
}

// Outpoint is spent if any non-conflicted transaction
// spends it:
bool Credits_CWallet::IsSpent(const uint256& hash, unsigned int n) const
{
    const COutPoint outpoint(hash, n);
    pair<TxSpends::const_iterator, TxSpends::const_iterator> range;
    range = mapTxSpends.equal_range(outpoint);

    for (TxSpends::const_iterator it = range.first; it != range.second; ++it)
    {
        const uint256& wtxid = it->second;
        std::map<uint256, Credits_CWalletTx>::const_iterator mit = mapWallet.find(wtxid);
        if (mit != mapWallet.end() && mit->second.GetDepthInMainChain() >= 0)
            return true; // Spent
    }
    return false;
}

void Credits_CWallet::AddToSpends(const COutPoint& outpoint, const uint256& wtxid)
{
    mapTxSpends.insert(make_pair(outpoint, wtxid));

    pair<TxSpends::iterator, TxSpends::iterator> range;
    range = mapTxSpends.equal_range(outpoint);
    SyncMetaData(range);
}


void Credits_CWallet::AddToSpends(const uint256& wtxid)
{
    assert(mapWallet.count(wtxid));
    Credits_CWalletTx& thisTx = mapWallet[wtxid];
    if (thisTx.IsCoinBase()) // Coinbases don't spend anything!
        return;

    BOOST_FOREACH(const Credits_CTxIn& txin, thisTx.vin)
        AddToSpends(txin.prevout, wtxid);
}

bool Credits_CWallet::EncryptWallet(const SecureString& strWalletPassphrase)
{
    if (IsCrypted())
        return false;

    CKeyingMaterial vMasterKey;
    RandAddSeedPerfmon();

    vMasterKey.resize(WALLET_CRYPTO_KEY_SIZE);
    RAND_bytes(&vMasterKey[0], WALLET_CRYPTO_KEY_SIZE);

    CMasterKey kMasterKey;

    RandAddSeedPerfmon();
    kMasterKey.vchSalt.resize(WALLET_CRYPTO_SALT_SIZE);
    RAND_bytes(&kMasterKey.vchSalt[0], WALLET_CRYPTO_SALT_SIZE);

    CCrypter crypter;
    int64_t nStartTime = GetTimeMillis();
    crypter.SetKeyFromPassphrase(strWalletPassphrase, kMasterKey.vchSalt, 25000, kMasterKey.nDerivationMethod);
    kMasterKey.nDeriveIterations = 2500000 / ((double)(GetTimeMillis() - nStartTime));

    nStartTime = GetTimeMillis();
    crypter.SetKeyFromPassphrase(strWalletPassphrase, kMasterKey.vchSalt, kMasterKey.nDeriveIterations, kMasterKey.nDerivationMethod);
    kMasterKey.nDeriveIterations = (kMasterKey.nDeriveIterations + kMasterKey.nDeriveIterations * 100 / ((double)(GetTimeMillis() - nStartTime))) / 2;

    if (kMasterKey.nDeriveIterations < 25000)
        kMasterKey.nDeriveIterations = 25000;

    LogPrintf("Encrypting Wallet with an nDeriveIterations of %i\n", kMasterKey.nDeriveIterations);

    if (!crypter.SetKeyFromPassphrase(strWalletPassphrase, kMasterKey.vchSalt, kMasterKey.nDeriveIterations, kMasterKey.nDerivationMethod))
        return false;
    if (!crypter.Encrypt(vMasterKey, kMasterKey.vchCryptedKey))
        return false;

    {
        LOCK(cs_wallet);
        mapMasterKeys[++nMasterKeyMaxID] = kMasterKey;
        if (fFileBacked)
        {
            pwalletdbEncryption = new Credits_CWalletDB(strWalletFile, pbitDb);
            if (!pwalletdbEncryption->TxnBegin())
                return false;
            pwalletdbEncryption->WriteMasterKey(nMasterKeyMaxID, kMasterKey);
        }

        if (!EncryptKeys(vMasterKey))
        {
            if (fFileBacked)
                pwalletdbEncryption->TxnAbort();
            exit(1); //We now probably have half of our keys encrypted in memory, and half not...die and let the user reload their unencrypted wallet.
        }

        // Encryption was introduced in version 0.4.0
        SetMinVersion(CREDITS_FEATURE_WALLETCRYPT, pwalletdbEncryption, true);

        if (fFileBacked)
        {
            if (!pwalletdbEncryption->TxnCommit())
                exit(1); //We now have keys encrypted in memory, but no on disk...die to avoid confusion and let the user reload their unencrypted wallet.

            delete pwalletdbEncryption;
            pwalletdbEncryption = NULL;
        }

        Lock();
        Unlock(strWalletPassphrase);
        NewKeyPool();
        Lock();

        // Need to completely rewrite the wallet file; if we don't, bdb might keep
        // bits of the unencrypted private key in slack space in the database file.
        Credits_CDB::Rewrite(strWalletFile, pbitDb);

    }
    NotifyStatusChanged(this);

    return true;
}

int64_t Credits_CWallet::IncOrderPosNext(Credits_CWalletDB *pwalletdb)
{
    AssertLockHeld(cs_wallet); // nOrderPosNext
    int64_t nRet = nOrderPosNext++;
    if (pwalletdb) {
        pwalletdb->WriteOrderPosNext(nOrderPosNext);
    } else {
        Credits_CWalletDB(strWalletFile, pbitDb).WriteOrderPosNext(nOrderPosNext);
    }
    return nRet;
}

Credits_CWallet::TxItems Credits_CWallet::OrderedTxItems(std::list<CAccountingEntry>& acentries, std::string strAccount)
{
    AssertLockHeld(cs_wallet); // mapWallet
    Credits_CWalletDB walletdb(strWalletFile, pbitDb);

    // First: get all CWalletTx and CAccountingEntry into a sorted-by-order multimap.
    TxItems txOrdered;

    // Note: maintaining indices in the database of (account,time) --> txid and (account, time) --> acentry
    // would make this much faster for applications that do this a lot.
    for (map<uint256, Credits_CWalletTx>::iterator it = mapWallet.begin(); it != mapWallet.end(); ++it)
    {
        Credits_CWalletTx* wtx = &((*it).second);
        txOrdered.insert(make_pair(wtx->nOrderPos, TxPair(wtx, (CAccountingEntry*)0)));
    }
    acentries.clear();
    walletdb.ListAccountCreditDebit(strAccount, acentries);
    BOOST_FOREACH(CAccountingEntry& entry, acentries)
    {
        txOrdered.insert(make_pair(entry.nOrderPos, TxPair((Credits_CWalletTx*)0, &entry)));
    }

    return txOrdered;
}

void Credits_CWallet::MarkDirty()
{
    {
        LOCK(cs_wallet);
        BOOST_FOREACH(PAIRTYPE(const uint256, Credits_CWalletTx)& item, mapWallet)
            item.second.MarkDirty();
    }
}

bool Credits_CWallet::AddToWallet(const Credits_CWalletTx& wtxIn, bool fFromLoadWallet)
{
    uint256 hash = wtxIn.GetHash();

    if (fFromLoadWallet)
    {
        mapWallet[hash] = wtxIn;
        mapWallet[hash].BindWallet(this);
        AddToSpends(hash);
    }
    else
    {
        LOCK(cs_wallet);
        // Inserts only if not already there, returns tx inserted or tx found
        pair<map<uint256, Credits_CWalletTx>::iterator, bool> ret = mapWallet.insert(make_pair(hash, wtxIn));
        Credits_CWalletTx& wtx = (*ret.first).second;
        wtx.BindWallet(this);
        bool fInsertedNew = ret.second;
        if (fInsertedNew)
        {
            wtx.nTimeReceived = GetAdjustedTime();
            wtx.nOrderPos = IncOrderPosNext();

            wtx.nTimeSmart = wtx.nTimeReceived;
            if (wtxIn.hashBlock != 0)
            {
                if (credits_mapBlockIndex.count(wtxIn.hashBlock))
                {
                    unsigned int latestNow = wtx.nTimeReceived;
                    unsigned int latestEntry = 0;
                    {
                        // Tolerate times up to the last timestamp in the wallet not more than 5 minutes into the future
                        int64_t latestTolerated = latestNow + 300;
                        std::list<CAccountingEntry> acentries;
                        TxItems txOrdered = OrderedTxItems(acentries);
                        for (TxItems::reverse_iterator it = txOrdered.rbegin(); it != txOrdered.rend(); ++it)
                        {
                            Credits_CWalletTx *const pwtx = (*it).second.first;
                            if (pwtx == &wtx)
                                continue;
                            CAccountingEntry *const pacentry = (*it).second.second;
                            int64_t nSmartTime;
                            if (pwtx)
                            {
                                nSmartTime = pwtx->nTimeSmart;
                                if (!nSmartTime)
                                    nSmartTime = pwtx->nTimeReceived;
                            }
                            else
                                nSmartTime = pacentry->nTime;
                            if (nSmartTime <= latestTolerated)
                            {
                                latestEntry = nSmartTime;
                                if (nSmartTime > latestNow)
                                    latestNow = nSmartTime;
                                break;
                            }
                        }
                    }

                    unsigned int& blocktime = credits_mapBlockIndex[wtxIn.hashBlock]->nTime;
                    wtx.nTimeSmart = std::max(latestEntry, std::min(blocktime, latestNow));
                }
                else
                    LogPrintf("Credits: AddToWallet() : found %s in block %s not in index\n",
                             wtxIn.GetHash().ToString(),
                             wtxIn.hashBlock.ToString());
            }
            AddToSpends(hash);
        }

        bool fUpdated = false;
        if (!fInsertedNew)
        {
            // Merge
            if (wtxIn.hashBlock != 0 && wtxIn.hashBlock != wtx.hashBlock)
            {
                wtx.hashBlock = wtxIn.hashBlock;
                fUpdated = true;
            }
            if (wtxIn.nIndex != -1 && (wtxIn.vMerkleBranch != wtx.vMerkleBranch || wtxIn.nIndex != wtx.nIndex))
            {
                wtx.vMerkleBranch = wtxIn.vMerkleBranch;
                wtx.nIndex = wtxIn.nIndex;
                fUpdated = true;
            }
            if (wtxIn.fFromMe && wtxIn.fFromMe != wtx.fFromMe)
            {
                wtx.fFromMe = wtxIn.fFromMe;
                fUpdated = true;
            }
        }

        //// debug print
        LogPrintf("Credits: AddToWallet %s: %s  %s%s\n", pbitDb->dbName, wtxIn.GetHash().ToString(), (fInsertedNew ? "new" : ""), (fUpdated ? "update" : ""));

        // Write to disk
        if (fInsertedNew || fUpdated)
            if (!wtx.WriteToDisk())
                return false;

        // Break debit/credit balance caches:
        wtx.MarkDirty();

        // Notify UI of new or updated transaction
        NotifyTransactionChanged(this, hash, fInsertedNew ? CT_NEW : CT_UPDATED);

        // notify an external script when a wallet transaction comes in or is updated
        std::string strCmd = GetArg("-walletnotify", "");

        if ( !strCmd.empty())
        {
            boost::replace_all(strCmd, "%s", wtxIn.GetHash().GetHex());
            boost::thread t(runCommand, strCmd); // thread runs free
        }

    }
    return true;
}

// Add a transaction to the wallet, or update it.
// pblock is optional, but should be provided if the transaction is known to be in a block.
// If fUpdate is true, existing transactions will be updated.
bool Credits_CWallet::AddToWalletIfInvolvingMe(const Bitcoin_CWallet *bitcoin_wallet,  const uint256 &hash, const Credits_CTransaction& tx, const Credits_CBlock* pblock, bool fUpdate)
{
    {
        AssertLockHeld(cs_wallet);
        bool fExisted = mapWallet.count(hash);
        if (fExisted && !fUpdate) return false;
        if (fExisted || IsMine(tx) || IsFromMe(tx) || (tx.IsClaim() && bitcoin_wallet->IsFromMe(tx)))
        {
            Credits_CWalletTx wtx(this,tx);
            // Get merkle branch if transaction was found in a block
            if (pblock)
                wtx.SetMerkleBranch(pblock);
            return AddToWallet(wtx);
        }
    }
    return false;
}

void Credits_CWallet::SyncTransaction(const Bitcoin_CWallet *bitcoin_wallet, const uint256 &hash, const Credits_CTransaction& tx, const Credits_CBlock* pblock)
{
    LOCK2(cs_main, cs_wallet);
    if (!AddToWalletIfInvolvingMe(bitcoin_wallet, hash, tx, pblock, true))
        return; // Not one of ours

    // If a transaction changes 'conflicted' state, that changes the balance
    // available of the outputs it spends. So force those to be
    // recomputed, also:
    BOOST_FOREACH(const Credits_CTxIn& txin, tx.vin)
    {
        if (mapWallet.count(txin.prevout.hash))
            mapWallet[txin.prevout.hash].MarkDirty();
    }
}

void Credits_CWallet::NotifyTxPrevInsBitcoin(Bitcoin_CWallet *bitcoin_wallet, Credits_CWalletTx &wtx) {
	//NOTE! This part operates on bitoin_wallet
    if(bitcoin_wallet) {
		BOOST_FOREACH(const Credits_CTxIn& txin, wtx.vin) {
			Bitcoin_CWalletTx &coin = bitcoin_wallet->mapWallet[txin.prevout.hash];
			coin.BindWallet(bitcoin_wallet);
			bitcoin_wallet->NotifyTransactionChanged(bitcoin_wallet, coin.GetHash(), CT_UPDATED);
		}
    }
}

void Credits_CWallet::NotifyTxPrevInsCredits(Credits_CWallet *credits_wallet, Credits_CWalletTx &wtx) {
	//NOTE! This part operates on credits_wallet
    if(credits_wallet) {
		BOOST_FOREACH(const Credits_CTxIn& txin, wtx.vin) {
			Credits_CWalletTx &coin = credits_wallet->mapWallet[txin.prevout.hash];
			coin.BindWallet(credits_wallet);
			credits_wallet->NotifyTransactionChanged(credits_wallet, coin.GetHash(), CT_UPDATED);
        }
    }
}

void Credits_CWallet::EraseFromWallet(Credits_CWallet *credits_wallet, const uint256 &hash)
{
    if (!fFileBacked)
        return;
    {
        LOCK(cs_wallet);
        Credits_CWalletTx &wtxOld = mapWallet[hash];

        if (mapWallet.erase(hash)) {
            Credits_CWalletDB(strWalletFile, pbitDb).EraseTx(hash);

            // Notify UI of deleted transaction
            NotifyTransactionChanged(this, hash, CT_DELETED);

            NotifyTxPrevInsCredits(credits_wallet, wtxOld);
        }
    }
    return;
}


bool Credits_CWallet::IsMine(const Credits_CTxIn &txin) const
{
    {
        LOCK(cs_wallet);
        map<uint256, Credits_CWalletTx>::const_iterator mi = mapWallet.find(txin.prevout.hash);
        if (mi != mapWallet.end())
        {
            const Credits_CWalletTx& prev = (*mi).second;
            if (txin.prevout.n < prev.vout.size())
                if (IsMine(prev.vout[txin.prevout.n]))
                    return true;
        }
    }
    return false;
}

int64_t Credits_CWallet::GetDebit(const Credits_CTxIn &txin) const
{
    {
        LOCK(cs_wallet);
        map<uint256, Credits_CWalletTx>::const_iterator mi = mapWallet.find(txin.prevout.hash);
        if (mi != mapWallet.end())
        {
            const Credits_CWalletTx& prev = (*mi).second;
            if (txin.prevout.n < prev.vout.size())
                if (IsMine(prev.vout[txin.prevout.n]))
                    return prev.vout[txin.prevout.n].nValue;
        }
    }
    return 0;
}

bool Credits_CWallet::IsChange(const CTxOut& txout) const
{
    CTxDestination address;

    // TODO: fix handling of 'change' outputs. The assumption is that any
    // payment to a TX_PUBKEYHASH that is mine but isn't in the address book
    // is change. That assumption is likely to break when we implement multisignature
    // wallets that return change back into a multi-signature-protected address;
    // a better way of identifying which outputs are 'the send' and which are
    // 'the change' will need to be implemented (maybe extend CWalletTx to remember
    // which output, if any, was change).
    if (ExtractDestination(txout.scriptPubKey, address) && ::IsMine(*this, address))
    {
        LOCK(cs_wallet);
        if (!mapAddressBook.count(address))
            return true;
    }
    return false;
}

int64_t Credits_CWalletTx::GetTxTime() const
{
    int64_t n = nTimeSmart;
    return n ? n : nTimeReceived;
}

int Credits_CWalletTx::GetRequestCount() const
{
    // Returns -1 if it wasn't being tracked
    int nRequests = -1;
    {
        LOCK(pwallet->cs_wallet);
        if (IsCoinBase())
        {
            // Generated block
            if (hashBlock != 0)
            {
                map<uint256, int>::const_iterator mi = pwallet->mapRequestCount.find(hashBlock);
                if (mi != pwallet->mapRequestCount.end())
                    nRequests = (*mi).second;
            }
        }
        else
        {
            // Did anyone request this transaction?
            map<uint256, int>::const_iterator mi = pwallet->mapRequestCount.find(GetHash());
            if (mi != pwallet->mapRequestCount.end())
            {
                nRequests = (*mi).second;

                // How about the block it's in?
                if (nRequests == 0 && hashBlock != 0)
                {
                    map<uint256, int>::const_iterator mi = pwallet->mapRequestCount.find(hashBlock);
                    if (mi != pwallet->mapRequestCount.end())
                        nRequests = (*mi).second;
                    else
                        nRequests = 1; // If it's in someone else's block it must have got out
                }
            }
        }
    }
    return nRequests;
}

void Credits_CWalletTx::GetAmounts(list<pair<CTxDestination, int64_t> >& listReceived,
                           list<pair<CTxDestination, int64_t> >& listSent, int64_t& nFee, string& strSentAccount) const
{
    nFee = 0;
    listReceived.clear();
    listSent.clear();
    strSentAccount = strFromAccount;

    // Compute fee:
    int64_t nDebit = GetDebit();
    if (nDebit > 0) // debit>0 means we signed/sent this transaction
    {
        int64_t nValueOut = GetValueOut();
        nFee = nDebit - nValueOut;
    }

    // Sent/received.
    BOOST_FOREACH(const CTxOut& txout, vout)
    {
        bool fIsMine;
        // Only need to handle txouts if AT LEAST one of these is true:
        //   1) they debit from us (sent)
        //   2) the output is to us (received)
        if (nDebit > 0)
        {
            // Don't report 'change' txouts
            if (pwallet->IsChange(txout))
                continue;
            fIsMine = pwallet->IsMine(txout);
        }
        else if (!(fIsMine = pwallet->IsMine(txout)))
            continue;

        // In either case, we need to get the destination address
        CTxDestination address;
        if (!ExtractDestination(txout.scriptPubKey, address))
        {
            LogPrintf("CWalletTx::GetAmounts: Unknown transaction type found, txid %s\n",
                     this->GetHash().ToString());
            address = CNoDestination();
        }

        // If we are debited by the transaction, add the output as a "sent" entry
        if (nDebit > 0)
            listSent.push_back(make_pair(address, txout.nValue));

        // If we are receiving the output, add it as a "received" entry
        if (fIsMine)
            listReceived.push_back(make_pair(address, txout.nValue));
    }

}

void Credits_CWalletTx::GetAccountAmounts(const string& strAccount, int64_t& nReceived,
                                  int64_t& nSent, int64_t& nFee) const
{
    nReceived = nSent = nFee = 0;

    int64_t allFee;
    string strSentAccount;
    list<pair<CTxDestination, int64_t> > listReceived;
    list<pair<CTxDestination, int64_t> > listSent;
    GetAmounts(listReceived, listSent, allFee, strSentAccount);

    if (strAccount == strSentAccount)
    {
        BOOST_FOREACH(const PAIRTYPE(CTxDestination,int64_t)& s, listSent)
            nSent += s.second;
        nFee = allFee;
    }
    {
        LOCK(pwallet->cs_wallet);
        BOOST_FOREACH(const PAIRTYPE(CTxDestination,int64_t)& r, listReceived)
        {
            if (pwallet->mapAddressBook.count(r.first))
            {
                map<CTxDestination, CAddressBookData>::const_iterator mi = pwallet->mapAddressBook.find(r.first);
                if (mi != pwallet->mapAddressBook.end() && (*mi).second.name == strAccount)
                    nReceived += r.second;
            }
            else if (strAccount.empty())
            {
                nReceived += r.second;
            }
        }
    }
}


bool Credits_CWalletTx::WriteToDisk()
{
    return Credits_CWalletDB(pwallet->strWalletFile, pwallet->pbitDb).WriteTx(GetHash(), *this);
}

// Scan the block chain (starting in pindexStart) for transactions
// from or to us. If fUpdate is true, found transactions that already
// exist in the wallet will be updated.
int Credits_CWallet::ScanForWalletTransactions(const Bitcoin_CWallet *bitcoin_wallet, Credits_CBlockIndex* pindexStart, bool fUpdate)
{
    int ret = 0;
    int64_t nNow = GetTime();

    Credits_CBlockIndex* pindex = pindexStart;
    {
        LOCK2(cs_main, cs_wallet);

        // no need to read and scan block, if block was created before
        // our wallet birthday (as adjusted for block time variability)
        while (pindex && nTimeFirstKey && (pindex->nTime < (nTimeFirstKey - 7200)))
            pindex = credits_chainActive.Next(pindex);

        ShowProgress(_("Rescanning credits wallet..."), 0); // show rescan progress in GUI as dialog or on splashscreen, if -credits_rescan on startup
        double dProgressStart = Checkpoints::Credits_GuessVerificationProgress(pindex, false);
        double dProgressTip = Checkpoints::Credits_GuessVerificationProgress((Credits_CBlockIndex*)credits_chainActive.Tip(), false);
        while (pindex)
        {
            if (pindex->nHeight % 100 == 0 && dProgressTip - dProgressStart > 0.0)
                ShowProgress(_("Rescanning credits wallet..."), std::max(1, std::min(99, (int)((Checkpoints::Credits_GuessVerificationProgress(pindex, false) - dProgressStart) / (dProgressTip - dProgressStart) * 100))));

            Credits_CBlock block;
            Credits_ReadBlockFromDisk(block, pindex);
            BOOST_FOREACH(Credits_CTransaction& tx, block.vtx)
            {
                if (AddToWalletIfInvolvingMe(bitcoin_wallet,  tx.GetHash(), tx, &block, fUpdate))
                    ret++;
            }
            pindex = credits_chainActive.Next(pindex);
            if (GetTime() >= nNow + 60) {
                nNow = GetTime();
                LogPrintf("Still rescanning credits wallet. At block %d. Progress=%f\n", pindex->nHeight, Checkpoints::Credits_GuessVerificationProgress(pindex));
            }
        }
        ShowProgress(_("Rescanning credits wallet..."), 100); // hide progress dialog in GUI
    }
    return ret;
}

void Credits_CWallet::ReacceptWalletTransactions()
{
    LOCK2(cs_main, cs_wallet);
    BOOST_FOREACH(PAIRTYPE(const uint256, Credits_CWalletTx)& item, mapWallet)
    {
        const uint256& wtxid = item.first;
        Credits_CWalletTx& wtx = item.second;
        assert(wtx.GetHash() == wtxid);

        int nDepth = wtx.GetDepthInMainChain();

        if (!wtx.IsCoinBase() && !wtx.IsDeposit() && nDepth < 0)
        {
            // Try to add to memory pool
            LOCK(credits_mempool.cs);
            wtx.AcceptToMemoryPool(false);
        }
    }
}

void Credits_CWalletTx::RelayWalletTransaction()
{
    if (!IsCoinBase())
    {
        if (GetDepthInMainChain() == 0) {
            uint256 hash = GetHash();
            LogPrintf("Credits: Relaying wtx %s\n", hash.ToString());
            Credits_RelayTransaction((Credits_CTransaction)*this, hash, Credits_NetParams());
        }
    }
}

set<uint256> Credits_CWalletTx::GetConflicts() const
{
    set<uint256> result;
    if (pwallet != NULL)
    {
        uint256 myHash = GetHash();
        result = pwallet->GetConflicts(myHash);
        result.erase(myHash);
    }
    return result;
}

void Credits_CWallet::ResendWalletTransactions()
{
    // Do this infrequently and randomly to avoid giving away
    // that these are our transactions.
    if (GetTime() < nNextResend)
        return;
    bool fFirst = (nNextResend == 0);
    nNextResend = GetTime() + GetRand(30 * 60);
    if (fFirst)
        return;

    // Only do it if there's been a new block since last time
    if (credits_nTimeBestReceived < nLastResend)
        return;
    nLastResend = GetTime();

    // Rebroadcast any of our txes that aren't in a block yet
    LogPrintf("Credits: ResendWalletTransactions()\n");
    {
        LOCK(cs_wallet);
        // Sort them in chronological order
        multimap<unsigned int, Credits_CWalletTx*> mapSorted;
        BOOST_FOREACH(PAIRTYPE(const uint256, Credits_CWalletTx)& item, mapWallet)
        {
            Credits_CWalletTx& wtx = item.second;
            // Don't rebroadcast until it's had plenty of time that
            // it should have gotten in already by now.
            if (credits_nTimeBestReceived - (int64_t)wtx.nTimeReceived > 5 * 60)
                mapSorted.insert(make_pair(wtx.nTimeReceived, &wtx));
        }
        BOOST_FOREACH(PAIRTYPE(const unsigned int, Credits_CWalletTx*)& item, mapSorted)
        {
            Credits_CWalletTx& wtx = *item.second;
            wtx.RelayWalletTransaction();
        }
    }
}






//////////////////////////////////////////////////////////////////////////////
//
// Actions
//


int64_t Credits_CWallet::GetBalance(map<uint256, set<int> >& mapFilterTxInPoints) const
{
    int64_t nTotal = 0;
    {
        LOCK2(cs_main, cs_wallet);
        for (map<uint256, Credits_CWalletTx>::const_iterator it = mapWallet.begin(); it != mapWallet.end(); ++it)
        {
            const Credits_CWalletTx* pcoin = &(*it).second;
            if (pcoin->IsTrusted())
                nTotal += pcoin->GetAvailableCredit(mapFilterTxInPoints);
        }
    }

    return nTotal;
}

int64_t Credits_CWallet::GetUnconfirmedBalance(map<uint256, set<int> >& mapFilterTxInPoints) const
{
    int64_t nTotal = 0;
    {
        LOCK2(cs_main, cs_wallet);
        for (map<uint256, Credits_CWalletTx>::const_iterator it = mapWallet.begin(); it != mapWallet.end(); ++it)
        {
            const Credits_CWalletTx* pcoin = &(*it).second;
            if (!Credits_IsFinalTx(*pcoin) || (!pcoin->IsTrusted() && pcoin->GetDepthInMainChain() == 0))
                nTotal += pcoin->GetAvailableCredit(mapFilterTxInPoints);
        }
    }
    return nTotal;
}

int64_t Credits_CWallet::GetImmatureBalance(map<uint256, set<int> >& mapFilterTxInPoints) const
{
    int64_t nTotal = 0;
    {
        LOCK2(cs_main, cs_wallet);
        for (map<uint256, Credits_CWalletTx>::const_iterator it = mapWallet.begin(); it != mapWallet.end(); ++it)
        {
            const Credits_CWalletTx* pcoin = &(*it).second;
            nTotal += pcoin->GetImmatureCredit(mapFilterTxInPoints);
        }
    }
    return nTotal;
}

int64_t Credits_CWallet::GetPreparedDepositBalance() const
{
    int64_t nTotal = 0;
    {
        LOCK2(cs_main, cs_wallet);
        for (map<uint256, Credits_CWalletTx>::const_iterator it = mapWallet.begin(); it != mapWallet.end(); ++it)
        {
            const Credits_CWalletTx* pcoin = &(*it).second;
//            if (pcoin->IsTrusted())
                nTotal += pcoin->GetPreparedDepositCredit();
        }
    }

    return nTotal;
}
int64_t Credits_CWallet::GetInDepositBalance() const
{
    int64_t nTotal = 0;
    {
        LOCK2(cs_main, cs_wallet);
        for (map<uint256, Credits_CWalletTx>::const_iterator it = mapWallet.begin(); it != mapWallet.end(); ++it)
        {
            const Credits_CWalletTx* pcoin = &(*it).second;
            if (pcoin->IsTrusted())
                nTotal += pcoin->GetInDepositCredit();
        }
    }

    return nTotal;
}

// populate vCoins with vector of spendable COutputs
void Credits_CWallet::AvailableCoins(vector<Credits_COutput>& vCoins, map<uint256, set<int> >& mapFilterTxInPoints, bool fOnlyConfirmed, const CCoinControl *coinControl) const
{
    vCoins.clear();

    {
        LOCK(cs_wallet);
        for (map<uint256, Credits_CWalletTx>::const_iterator it = mapWallet.begin(); it != mapWallet.end(); ++it)
        {
            const uint256& wtxid = it->first;
            const Credits_CWalletTx* pcoin = &(*it).second;

            if (!Credits_IsFinalTx(*pcoin))
                continue;

            if (fOnlyConfirmed && !pcoin->IsTrusted())
                continue;

            if (pcoin->IsCoinBase() && pcoin->GetBlocksToMaturity() > 0)
                continue;
            if(pcoin->IsDeposit() && pcoin->vout.size() == 1 && pcoin->GetFirstDepositOutBlocksToMaturity() > 0)
            	continue;
            if(pcoin->IsDeposit() && pcoin->vout.size() == 2 && pcoin->GetSecondDepositOutBlocksToMaturity() > 0)
            	continue;

            int nDepth = pcoin->GetDepthInMainChain();
            if (nDepth < 0)
                continue;

    		unsigned int start = 0;
            if(pcoin->IsDeposit() && pcoin->GetFirstDepositOutBlocksToMaturity() > 0) {
    			start = 1;
            }
            for (unsigned int i = start; i < pcoin->vout.size(); i++) {
                if (!(IsSpent(wtxid, i)) && IsMine(pcoin->vout[i]) &&
                    !IsLockedCoin((*it).first, i) && pcoin->vout[i].nValue > 0 &&
                    (!coinControl || !coinControl->HasSelected() || coinControl->IsSelected((*it).first, i))) {
						if(!IsInFilterPoints(wtxid, i, mapFilterTxInPoints)) {
							vCoins.push_back(Credits_COutput(pcoin, i, nDepth));
						}
                }
            }
        }
    }
}

void Credits_CWallet::ClaimTxInPoints(map<uint256, set<int> >& mapClaimTxPoints) const {
    std::vector<COutPoint> vClaimTxs;

    {
        LOCK(cs_wallet);
        for (map<uint256, Credits_CWalletTx>::const_iterator it = mapWallet.begin(); it != mapWallet.end(); ++it) {
            const Credits_CWalletTx* pcoin = &(*it).second;
            if (pcoin->IsClaim()) {
            	//pcoin->MarkDirty();
				for (unsigned int i = 0; i < pcoin->vin.size(); i++) {
					vClaimTxs.push_back(COutPoint(pcoin->vin[i].prevout.hash, pcoin->vin[i].prevout.n));
				}
            }
        }

        BOOST_FOREACH(const COutPoint& outpoint, vClaimTxs) {
        	mapClaimTxPoints[outpoint.hash].insert(outpoint.n);
        }
    }
}

void Credits_CWallet::PreparedDepositTxInPoints(map<uint256, set<int> >& mapDepositTxPoints) const {
    std::vector<COutPoint> vDepositTxs;

    {
        LOCK(cs_wallet);
        for (map<uint256, Credits_CWalletTx>::const_iterator it = mapWallet.begin(); it != mapWallet.end(); ++it) {
            const Credits_CWalletTx* pcoin = &(*it).second;
            if (pcoin->IsDeposit()) {
            	//pcoin->MarkDirty();
				for (unsigned int i = 0; i < pcoin->vin.size(); i++) {
					vDepositTxs.push_back(COutPoint(pcoin->vin[i].prevout.hash, pcoin->vin[i].prevout.n));
				}
            }
        }

        BOOST_FOREACH(const COutPoint& outpoint, vDepositTxs) {
        	mapDepositTxPoints[outpoint.hash].insert(outpoint.n);
        }
    }
}

static void Credits_ApproximateBestSubset(vector<pair<int64_t, pair<const Credits_CWalletTx*,unsigned int> > >vValue, int64_t nTotalLower, int64_t nTargetValue,
                                  vector<char>& vfBest, int64_t& nBest, int iterations = 1000)
{
    vector<char> vfIncluded;

    vfBest.assign(vValue.size(), true);
    nBest = nTotalLower;

    seed_insecure_rand();

    for (int nRep = 0; nRep < iterations && nBest != nTargetValue; nRep++)
    {
        vfIncluded.assign(vValue.size(), false);
        int64_t nTotal = 0;
        bool fReachedTarget = false;
        for (int nPass = 0; nPass < 2 && !fReachedTarget; nPass++)
        {
            for (unsigned int i = 0; i < vValue.size(); i++)
            {
                //The solver here uses a randomized algorithm,
                //the randomness serves no real security purpose but is just
                //needed to prevent degenerate behavior and it is important
                //that the rng fast. We do not use a constant random sequence,
                //because there may be some privacy improvement by making
                //the selection random.
                if (nPass == 0 ? insecure_rand()&1 : !vfIncluded[i])
                {
                    nTotal += vValue[i].first;
                    vfIncluded[i] = true;
                    if (nTotal >= nTargetValue)
                    {
                        fReachedTarget = true;
                        if (nTotal < nBest)
                        {
                            nBest = nTotal;
                            vfBest = vfIncluded;
                        }
                        nTotal -= vValue[i].first;
                        vfIncluded[i] = false;
                    }
                }
            }
        }
    }
}

bool Credits_CWallet::SelectCoinsMinConf(int64_t nTargetValue, int nConfMine, int nConfTheirs, vector<Credits_COutput> vCoins,
                                 set<pair<const Credits_CWalletTx*,unsigned int> >& setCoinsRet, int64_t& nValueRet) const
{
    setCoinsRet.clear();
    nValueRet = 0;

    // List of values less than target
    pair<int64_t, pair<const Credits_CWalletTx*,unsigned int> > coinLowestLarger;
    coinLowestLarger.first = std::numeric_limits<int64_t>::max();
    coinLowestLarger.second.first = NULL;
    vector<pair<int64_t, pair<const Credits_CWalletTx*,unsigned int> > > vValue;
    int64_t nTotalLower = 0;

    random_shuffle(vCoins.begin(), vCoins.end(), GetRandInt);

    BOOST_FOREACH(Credits_COutput output, vCoins)
    {
        const Credits_CWalletTx *pcoin = output.tx;

        if (output.nDepth < (pcoin->IsFromMe() ? nConfMine : nConfTheirs))
            continue;

        int i = output.i;
        int64_t n = pcoin->vout[i].nValue;

        pair<int64_t,pair<const Credits_CWalletTx*,unsigned int> > coin = make_pair(n,make_pair(pcoin, i));

        if (n == nTargetValue)
        {
            setCoinsRet.insert(coin.second);
            nValueRet += coin.first;
            return true;
        }
        else if (n < nTargetValue + CENT)
        {
            vValue.push_back(coin);
            nTotalLower += n;
        }
        else if (n < coinLowestLarger.first)
        {
            coinLowestLarger = coin;
        }
    }

    if (nTotalLower == nTargetValue)
    {
        for (unsigned int i = 0; i < vValue.size(); ++i)
        {
            setCoinsRet.insert(vValue[i].second);
            nValueRet += vValue[i].first;
        }
        return true;
    }

    if (nTotalLower < nTargetValue)
    {
        if (coinLowestLarger.second.first == NULL)
            return false;
        setCoinsRet.insert(coinLowestLarger.second);
        nValueRet += coinLowestLarger.first;
        return true;
    }

    // Solve subset sum by stochastic approximation
    sort(vValue.rbegin(), vValue.rend(), Credits_CompareValueOnly());
    vector<char> vfBest;
    int64_t nBest;

    Credits_ApproximateBestSubset(vValue, nTotalLower, nTargetValue, vfBest, nBest, 1000);
    if (nBest != nTargetValue && nTotalLower >= nTargetValue + CENT)
        Credits_ApproximateBestSubset(vValue, nTotalLower, nTargetValue + CENT, vfBest, nBest, 1000);

    // If we have a bigger coin and (either the stochastic approximation didn't find a good solution,
    //                                   or the next bigger coin is closer), return the bigger coin
    if (coinLowestLarger.second.first &&
        ((nBest != nTargetValue && nBest < nTargetValue + CENT) || coinLowestLarger.first <= nBest))
    {
        setCoinsRet.insert(coinLowestLarger.second);
        nValueRet += coinLowestLarger.first;
    }
    else {
        for (unsigned int i = 0; i < vValue.size(); i++)
            if (vfBest[i])
            {
                setCoinsRet.insert(vValue[i].second);
                nValueRet += vValue[i].first;
            }

        LogPrint("selectcoins", "SelectCoins() best subset: ");
        for (unsigned int i = 0; i < vValue.size(); i++)
            if (vfBest[i])
                LogPrint("selectcoins", "%s ", FormatMoney(vValue[i].first));
        LogPrint("selectcoins", "total %s\n", FormatMoney(nBest));
    }

    return true;
}

bool Credits_CWallet::SelectCoins(int64_t nTargetValue, set<pair<const Credits_CWalletTx*,unsigned int> >& setCoinsRet, int64_t& nValueRet, map<uint256, set<int> >& mapFilterTxInPoints, const CCoinControl* coinControl) const
{
    vector<Credits_COutput> vCoins;
    AvailableCoins(vCoins, mapFilterTxInPoints, true, coinControl);

    // coin control -> return all selected outputs (we want all selected to go into the transaction for sure)
    if (coinControl && coinControl->HasSelected())
    {
        BOOST_FOREACH(const Credits_COutput& out, vCoins)
        {
            nValueRet += out.tx->vout[out.i].nValue;
            setCoinsRet.insert(make_pair(out.tx, out.i));
        }
        return (nValueRet >= nTargetValue);
    }

    return (SelectCoinsMinConf(nTargetValue, 1, 6, vCoins, setCoinsRet, nValueRet) ||
            SelectCoinsMinConf(nTargetValue, 1, 1, vCoins, setCoinsRet, nValueRet) ||
            (credits_bSpendZeroConfChange && SelectCoinsMinConf(nTargetValue, 0, 1, vCoins, setCoinsRet, nValueRet)));
}




bool Credits_CWallet::CreateTransaction(Credits_CWallet *deposit_wallet, const vector<pair<CScript, int64_t> >& vecSend,
                                Credits_CWalletTx& wtxNew, Credits_CReserveKey& reservekey, int64_t& nFeeRet, std::string& strFailReason, const CCoinControl* coinControl)
{
	wtxNew.nTxType = TX_TYPE_STANDARD;

    int64_t nValue = 0;
    BOOST_FOREACH (const PAIRTYPE(CScript, int64_t)& s, vecSend)
    {
        if (nValue < 0)
        {
            strFailReason = _("Transaction amounts must be positive");
            return false;
        }
        nValue += s.second;
    }
    if (vecSend.empty() || nValue < 0)
    {
        strFailReason = _("Transaction amounts must be positive");
        return false;
    }

    wtxNew.BindWallet(this);

    {
        LOCK2(cs_main, cs_wallet);
        {
            nFeeRet = credits_nTransactionFee;
            while (true)
            {
                wtxNew.vin.clear();
                wtxNew.vout.clear();
                wtxNew.fFromMe = true;

                int64_t nTotalValue = nValue + nFeeRet;
                double dPriority = 0;
                // vouts to the payees
                BOOST_FOREACH (const PAIRTYPE(CScript, int64_t)& s, vecSend)
                {
                	CTxOut txout(s.second, s.first);
                    if (txout.IsDust(Credits_CTransaction::nMinRelayTxFee))
                    {
                        strFailReason = _("Transaction amount too small");
                        return false;
                    }
                    wtxNew.vout.push_back(txout);
                }

                //Find all prepared deposit transactions
                map<uint256, set<int> > mapPreparedDepositTxInPoints;
                deposit_wallet->PreparedDepositTxInPoints(mapPreparedDepositTxInPoints);

                // Choose coins to use
                set<pair<const Credits_CWalletTx*,unsigned int> > setCoins;
                int64_t nValueIn = 0;
                if (!SelectCoins(nTotalValue, setCoins, nValueIn, mapPreparedDepositTxInPoints, coinControl))
                {
                    strFailReason = _("Insufficient funds");
                    return false;
                }
                BOOST_FOREACH(PAIRTYPE(const Credits_CWalletTx*, unsigned int) pcoin, setCoins)
                {
                    int64_t nCredit = pcoin.first->vout[pcoin.second].nValue;
                    //The priority after the next block (depth+1) is used instead of the current,
                    //reflecting an assumption the user would accept a bit more delay for
                    //a chance at a free transaction.
                    dPriority += (double)nCredit * (pcoin.first->GetDepthInMainChain()+1);
                }

                int64_t nChange = nValueIn - nValue - nFeeRet;
                // The following if statement should be removed once enough miners
                // have upgraded to the 0.9 GetMinFee() rules. Until then, this avoids
                // creating free transactions that have change outputs less than
                // CENT bitcoins.
                if (nFeeRet < Credits_CTransaction::nMinTxFee && nChange > 0 && nChange < CENT)
                {
                    int64_t nMoveToFee = min(nChange, Credits_CTransaction::nMinTxFee - nFeeRet);
                    nChange -= nMoveToFee;
                    nFeeRet += nMoveToFee;
                }

                if (nChange > 0)
                {
                    // Fill a vout to ourself
                    // TODO: pass in scriptChange instead of reservekey so
                    // change transaction isn't always pay-to-bitcoin-address
                    CScript scriptChange;

                    // coin control: send change to custom address
                    if (coinControl && !boost::get<CNoDestination>(&coinControl->destChange))
                        scriptChange.SetDestination(coinControl->destChange);

                    // no coin control: send change to newly generated address
                    else
                    {
                        // Note: We use a new key here to keep it from being obvious which side is the change.
                        //  The drawback is that by not reusing a previous key, the change may be lost if a
                        //  backup is restored, if the backup doesn't have the new private key for the change.
                        //  If we reused the old key, it would be possible to add code to look for and
                        //  rediscover unknown transactions that were written with keys of ours to recover
                        //  post-backup change.

                        // Reserve a new key pair from key pool
                        CPubKey vchPubKey;
                        bool ret;
                        ret = reservekey.GetReservedKey(vchPubKey);
                        assert(ret); // should never fail, as we just unlocked

                        scriptChange.SetDestination(vchPubKey.GetID());
                    }

                    CTxOut newTxOut(nChange, scriptChange);

                    // Never create dust outputs; if we would, just
                    // add the dust to the fee.
                    if (newTxOut.IsDust(Credits_CTransaction::nMinRelayTxFee))
                    {
                        nFeeRet += nChange;
                        reservekey.ReturnKey();
                    }
                    else
                    {
                        // Insert change txn at random position:
                        vector<CTxOut>::iterator position = wtxNew.vout.begin()+GetRandInt(wtxNew.vout.size()+1);
                        wtxNew.vout.insert(position, newTxOut);
                    }
                }
                else
                    reservekey.ReturnKey();

                // Fill vin
                BOOST_FOREACH(const PAIRTYPE(const Credits_CWalletTx*,unsigned int)& coin, setCoins)
                    wtxNew.vin.push_back(Credits_CTxIn(coin.first->GetHash(),coin.second));

                // Sign
                int nIn = 0;
                BOOST_FOREACH(const PAIRTYPE(const Credits_CWalletTx*,unsigned int)& coin, setCoins) {
                    if (!Credits_SignSignature(*this, *coin.first, wtxNew, nIn++))
                    {
                        strFailReason = _("Signing transaction failed");
                        return false;
                    }
                }

                // Limit size
                unsigned int nBytes = ::GetSerializeSize(*(Credits_CTransaction*)&wtxNew, SER_NETWORK, CREDITS_PROTOCOL_VERSION);
                if (nBytes >= CREDITS_MAX_STANDARD_TX_SIZE)
                {
                    strFailReason = _("Transaction too large");
                    return false;
                }
                dPriority = wtxNew.ComputePriority(dPriority, nBytes);

                // Check that enough fee is included
                int64_t nPayFee = credits_nTransactionFee * (1 + (int64_t)nBytes / 1000);
                bool fAllowFree = Credits_AllowFree(dPriority);
                int64_t nMinFee = Credits_GetMinFee(wtxNew, nBytes, fAllowFree, GMF_SEND);
                if (nFeeRet < max(nPayFee, nMinFee))
                {
                    nFeeRet = max(nPayFee, nMinFee);
                    continue;
                }

                wtxNew.fTimeReceivedIsTxTime = true;

                break;
            }
        }
    }
    return true;
}

bool Credits_CWallet::CreateTransaction(Credits_CWallet *deposit_wallet, CScript scriptPubKey, int64_t nValue,
                                Credits_CWalletTx& wtxNew, Credits_CReserveKey& reservekey, int64_t& nFeeRet, std::string& strFailReason, const CCoinControl* coinControl)
{
    vector< pair<CScript, int64_t> > vecSend;
    vecSend.push_back(make_pair(scriptPubKey, nValue));
    return CreateTransaction(deposit_wallet, vecSend, wtxNew, reservekey, nFeeRet, strFailReason, coinControl);
}

bool Credits_CWallet::CreateDepositTransaction(Credits_CWallet *deposit_wallet, const pair<CScript, int64_t> & send,
                                Credits_CWalletTx& wtxNew, Credits_CReserveKey& depositSignatureKey, const int64_t & nTotalIn, std::string& strFailReason, const Credits_COutput& coin, Credits_CCoinsViewCache &credits_view)
{
	wtxNew.nTxType = TX_TYPE_DEPOSIT;

    int64_t nValue = send.second;

    if (nValue < 0)
    {
        strFailReason = _("Transaction amounts must be positive");
        return false;
    }

    wtxNew.BindWallet(deposit_wallet);

    {
        LOCK2(cs_main, cs_wallet);
        {
                // Reserve a new key pair from key pool
                CPubKey vchPubKey;
                bool ret;
                ret = depositSignatureKey.GetReservedKey(vchPubKey);
                assert(ret); // should never fail, as we just unlocked
                wtxNew.signingKeyId = vchPubKey.GetID();

                wtxNew.vin.clear();
                wtxNew.vout.clear();
                wtxNew.fFromMe = true;

                // vouts to the payees
				CTxOut txout(send.second, send.first);
				if (txout.IsDust(Credits_CTransaction::nMinRelayTxFee))
				{
					strFailReason = _("Transaction amount too small");
					return false;
				}
				wtxNew.vout.push_back(txout);

                // Fill vin
                wtxNew.vin.push_back(Credits_CTxIn(coin.tx->GetHash(),coin.i));

                // Sign
                int nIn = 0;
				if (!Credits_SignSignature(*this, *coin.tx, wtxNew, nIn++))
				{
					strFailReason = _("Signing transaction failed");
					return false;
				}

                // Limit size
                unsigned int nBytes = ::GetSerializeSize(*(Credits_CTransaction*)&wtxNew, SER_NETWORK, CREDITS_PROTOCOL_VERSION);
                if (nBytes >= CREDITS_MAX_STANDARD_TX_SIZE)
                {
                    strFailReason = _("Transaction too large");
                    return false;
                }

                wtxNew.fTimeReceivedIsTxTime = true;
        }
    }

	//Input verification
    int64_t nClaimedCoins = 0;
    CValidationState state;
    if (!Credits_CheckInputs(wtxNew, state, credits_view, nClaimedCoins, true, MANDATORY_SCRIPT_VERIFY_FLAGS)) {
        strFailReason = _("Deposit inputs are not valid");
        return false;
    }

    //Find all existing prepared deposits
    map<uint256, set<int> > mapDepositTxIns;
    deposit_wallet->PreparedDepositTxInPoints(mapDepositTxIns);

    if(IsAnyTxInInFilterPoints(wtxNew, mapDepositTxIns)) {
        strFailReason = _("Deposit spends same coins as other already prepared deposit");
        return false;
    }

    return true;
}

bool Credits_CWallet::ImportKeyFromBitcoinWallet (CTxDestination & address, Bitcoin_CWallet * bitcoinWallet) {
	//Verify and fetch secret key from bitcoin wallet
	CKeyID keyID;
	if (!CBitcoinAddress(address).GetKeyID(keyID)) {
		return false;
	}

	LogPrintf("Importinng key with address %s\n",  keyID.GetHex());

	CKey vchSecret;
	if (!bitcoinWallet->GetKey(keyID, vchSecret)) {
		return false;
	}

	//Add key to credits wallet.
	CPubKey pubkey = vchSecret.GetPubKey();
	CKeyID vchAddress = pubkey.GetID();
	{
		std::stringstream ss;
		ss << "bitcoin-claim " << DateTimeStrFormat("%Y-%m-%d %H:%M:%S", GetTime());
		std::string strLabel = ss.str();

		LOCK2(cs_main, cs_wallet);

		MarkDirty();
		SetAddressBook(vchAddress, strLabel, "receive");

		// Don't throw error in case a key is already there
		if (HaveKey(vchAddress)) {
			LogPrintf("Key with address %s already exists in credits wallet\n",  vchAddress.ToString());
			return true;
		}

		mapKeyMetadata[vchAddress].nCreateTime = 1;

		if (!AddKeyPubKey(vchSecret, pubkey)) {
			return false;
		}

		// whenever a key is imported, we need to scan the whole chain
		nTimeFirstKey = 1; // 0 would be considered 'no value'
//
//		 if (fRescan) {
//		     ScanForWalletTransactions(credits_chainActive.Genesis(), true);
//		 }

		LogPrintf("Key with address %s was imported into credits wallet\n",  vchAddress.ToString());
	}
	return true;
}

bool Credits_CWallet::CreateClaimTransaction(Bitcoin_CWallet *bitcoin_wallet, Credits_CCoinsViewCache *claim_view, const vector<pair<CScript, int64_t> >& vecSend,
                                Credits_CWalletTx& wtxNew, Credits_CReserveKey& reservekey, int64_t& nFeeRet, std::string& strFailReason, const CCoinControl* coinControl)
{
	wtxNew.nTxType = TX_TYPE_EXTERNAL_BITCOIN;

    int64_t nValue = 0;
    BOOST_FOREACH (const PAIRTYPE(CScript, int64_t)& s, vecSend)
    {
        if (nValue < 0)
        {
            strFailReason = _("Transaction amounts must be positive");
            return false;
        }
        nValue += s.second;
    }
    if (vecSend.empty() || nValue < 0)
    {
        strFailReason = _("Transaction amounts must be positive");
        return false;
    }

    wtxNew.BindWallet(this);

    {
        LOCK2(cs_main, cs_wallet);
        {
            nFeeRet = credits_nTransactionFee;
            while (true)
            {
                wtxNew.vin.clear();
                wtxNew.vout.clear();
                wtxNew.fFromMe = true;

                int64_t nTotalValue = nValue + nFeeRet;
                double dPriority = 0;
                // vouts to the payees
                BOOST_FOREACH (const PAIRTYPE(CScript, int64_t)& s, vecSend)
                {
                	CTxOut txout(s.second, s.first);
                    if (txout.IsDust(Credits_CTransaction::nMinRelayTxFee))
                    {
                        strFailReason = _("Transaction amount too small");
                        return false;
                    }
                    wtxNew.vout.push_back(txout);
                }

                //Find all claimed  transactions
                map<uint256, set<int> > mapClaimTxInPoints;
                ClaimTxInPoints(mapClaimTxInPoints);

                // Choose coins to use
                set<pair<const Bitcoin_CWalletTx*,unsigned int> > setCoins;
                int64_t nValueIn = 0;
                if (!bitcoin_wallet->SelectCoins(nTotalValue, setCoins, nValueIn, claim_view, mapClaimTxInPoints, coinControl))
                {
                    strFailReason = _("Insufficient funds");
                    return false;
                }
                BOOST_FOREACH(PAIRTYPE(const Bitcoin_CWalletTx*, unsigned int) pcoin, setCoins)
                {
                    CTxDestination address;
                    if(!ExtractDestination(pcoin.first->vout[pcoin.second].scriptPubKey, address)) {
                        strFailReason = _("Could not parse destination for output");
                        return false;
                    }

                    if(!ImportKeyFromBitcoinWallet(address, bitcoin_wallet)) {
                        strFailReason = _("Could not import key");
                        return false;
                    }

                    int64_t nCredit = pcoin.first->vout[pcoin.second].nValue;
                    //The priority after the next block (depth+1) is used instead of the current,
                    //reflecting an assumption the user would accept a bit more delay for
                    //a chance at a free transaction.
                    dPriority += (double)nCredit * (pcoin.first->GetDepthInMainChain()+1);
                }

                int64_t nChange = nValueIn - nValue - nFeeRet;
                // The following if statement should be removed once enough miners
                // have upgraded to the 0.9 GetMinFee() rules. Until then, this avoids
                // creating free transactions that have change outputs less than
                // CENT bitcoins.
                if (nFeeRet < Credits_CTransaction::nMinTxFee && nChange > 0 && nChange < CENT)
                {
                    int64_t nMoveToFee = min(nChange, Credits_CTransaction::nMinTxFee - nFeeRet);
                    nChange -= nMoveToFee;
                    nFeeRet += nMoveToFee;
                }

                if (nChange > 0)
                {
                    // Fill a vout to ourself
                    // TODO: pass in scriptChange instead of reservekey so
                    // change transaction isn't always pay-to-bitcoin-address
                    CScript scriptChange;

                    // coin control: send change to custom address
                    if (coinControl && !boost::get<CNoDestination>(&coinControl->destChange))
                        scriptChange.SetDestination(coinControl->destChange);

                    // no coin control: send change to newly generated address
                    else
                    {
                        // Note: We use a new key here to keep it from being obvious which side is the change.
                        //  The drawback is that by not reusing a previous key, the change may be lost if a
                        //  backup is restored, if the backup doesn't have the new private key for the change.
                        //  If we reused the old key, it would be possible to add code to look for and
                        //  rediscover unknown transactions that were written with keys of ours to recover
                        //  post-backup change.

                        // Reserve a new key pair from key pool
                        CPubKey vchPubKey;
                        bool ret;
                        ret = reservekey.GetReservedKey(vchPubKey);
                        assert(ret); // should never fail, as we just unlocked

                        scriptChange.SetDestination(vchPubKey.GetID());
                    }

                    CTxOut newTxOut(nChange, scriptChange);

                    // Never create dust outputs; if we would, just
                    // add the dust to the fee.
                    if (newTxOut.IsDust(Credits_CTransaction::nMinRelayTxFee))
                    {
                        nFeeRet += nChange;
                        reservekey.ReturnKey();
                    }
                    else
                    {
                        // Insert change txn at random position:
                        vector<CTxOut>::iterator position = wtxNew.vout.begin()+GetRandInt(wtxNew.vout.size()+1);
                        wtxNew.vout.insert(position, newTxOut);
                    }
                }
                else
                    reservekey.ReturnKey();

                // Fill vin
                BOOST_FOREACH(const PAIRTYPE(const Bitcoin_CWalletTx*,unsigned int)& coin, setCoins)
                    wtxNew.vin.push_back(Credits_CTxIn(coin.first->GetHash(),coin.second));

                // Sign
                int nIn = 0;
                BOOST_FOREACH(const PAIRTYPE(const Bitcoin_CWalletTx*,unsigned int)& coin, setCoins) {
                	const CScript &fromPubKey = coin.first->vout[coin.second].scriptPubKey;
                    if (!Credits_SignSignature(*this, fromPubKey, wtxNew, nIn)) {
                        strFailReason = _("Signing transaction failed");
                        return false;
                    }
                	nIn++;
                }

                // Limit size
                unsigned int nBytes = ::GetSerializeSize(*(Credits_CTransaction*)&wtxNew, SER_NETWORK, CREDITS_PROTOCOL_VERSION);
                if (nBytes >= CREDITS_MAX_STANDARD_TX_SIZE)
                {
                    strFailReason = _("Transaction too large");
                    return false;
                }
                dPriority = wtxNew.ComputePriority(dPriority, nBytes);

                // Check that enough fee is included
                int64_t nPayFee = credits_nTransactionFee * (1 + (int64_t)nBytes / 1000);
                bool fAllowFree = Credits_AllowFree(dPriority);
                int64_t nMinFee = Credits_GetMinFee(wtxNew, nBytes, fAllowFree, GMF_SEND);
                if (nFeeRet < max(nPayFee, nMinFee))
                {
                    nFeeRet = max(nPayFee, nMinFee);
                    continue;
                }

                wtxNew.fTimeReceivedIsTxTime = true;

                break;
            }
        }
    }
    return true;
}

// Call after CreateTransaction unless you want to abort
bool Credits_CWallet::CommitTransaction(Bitcoin_CWallet *bitcoin_wallet, Credits_CWalletTx& wtxNew, Credits_CReserveKey& reservekey, std::vector<Credits_CReserveKey *> &keyRecipients)
{
    {
        LOCK2(cs_main, cs_wallet);
        LogPrintf("CommitTransaction:\n%s", wtxNew.ToString());
        {
            // This is only to keep the database open to defeat the auto-flush for the
            // duration of this scope.  This is the only place where this optimization
            // maybe makes sense; please don't do it anywhere else.
            Credits_CWalletDB* pwalletdb = fFileBacked ? new Credits_CWalletDB(strWalletFile, pbitDb,"r") : NULL;

            // Take key pair from key pool so it won't be used again
            reservekey.KeepKey();
            BOOST_FOREACH(Credits_CReserveKey *keyRecipient, keyRecipients) {
            	keyRecipient->KeepKey();
            }

            // Add tx to wallet, because if it has change it's also ours,
            // otherwise just for transaction history.
            AddToWallet(wtxNew);

            // Notify that old coins are spent
            set<Credits_CWalletTx*> setCoins;
        	if(wtxNew.IsClaim()) {
                NotifyTxPrevInsBitcoin(bitcoin_wallet, wtxNew);
        	} else {
        		NotifyTxPrevInsCredits(this, wtxNew);
        	}

            if (fFileBacked)
                delete pwalletdb;
        }

        // Track how many getdata requests our transaction gets
        mapRequestCount[wtxNew.GetHash()] = 0;

        // Broadcast
        if (!wtxNew.AcceptToMemoryPool(false))
        {
            // This must not fail. The transaction has already been signed and recorded.
            LogPrintf("CommitTransaction() : Error: Transaction not valid\n");
            return false;
        }
        wtxNew.RelayWalletTransaction();
    }
    return true;
}

// Call after CreateTransaction unless you want to abort
bool Credits_CWallet::CommitDepositTransaction(Credits_CWallet *credits_wallet, Credits_CWalletTx& wtxNew, Credits_CReserveKey& depositSignaturekey, std::vector<Credits_CReserveKey *> &keyRecipients)
{
    {
        LOCK2(cs_main, cs_wallet);
        LogPrintf("CommitDepositTransaction:\n%s", wtxNew.ToString());
        {
        	if(!wtxNew.IsValidDeposit()) {
                LogPrintf("\n\nERROR! Transaction is not a valid deposit!\n\n");
        		return false;
        	}
            // This is only to keep the database open to defeat the auto-flush for the
            // duration of this scope.  This is the only place where this optimization
            // maybe makes sense; please don't do it anywhere else.
            Credits_CWalletDB* pwalletdb = fFileBacked ? new Credits_CWalletDB(strWalletFile, pbitDb,"r") : NULL;

//            // Take key pair from key pool so it won't be used again
//        	CPubKey depositSignaturePubKey;
//        	depositSignaturekey.GetReservedKey(depositSignaturePubKey);
//        	CTxDestination depositSignatureAddress = depositSignaturePubKey.GetID();
//            if(!ImportKeyFromBitcreditWallet(depositSignatureAddress, credits_wallet)) {
//                LogPrintf("\n\nERROR! Key could not be stored to deposit wallet!\n\n");
//                return false;
//            }
        	depositSignaturekey.KeepKey();

            BOOST_FOREACH(Credits_CReserveKey *keyRecipient, keyRecipients) {
            	keyRecipient->KeepKey();
            }

            // Add tx to wallet
            AddToWallet(wtxNew);

            NotifyTxPrevInsCredits(credits_wallet, wtxNew);

            if (fFileBacked)
                delete pwalletdb;
        }
    }
    return true;
}


string Credits_CWallet::SendMoney(Bitcoin_CWallet *bitcoin_wallet, Credits_CWallet *deposit_wallet, CScript scriptPubKey, int64_t nValue, Credits_CWalletTx& wtxNew)
{
    Credits_CReserveKey reservekey(this);
    int64_t nFeeRequired;

    if (IsLocked())
    {
        string strError = _("Error: Wallet locked, unable to create transaction!");
        LogPrintf("SendMoney() : %s", strError);
        return strError;
    }
    string strError;
    if (!CreateTransaction(deposit_wallet, scriptPubKey, nValue, wtxNew, reservekey, nFeeRequired, strError))
    {
        //Find all prepared deposit transactions
        map<uint256, set<int> > mapPreparedDepositTxInPoints;
        deposit_wallet->PreparedDepositTxInPoints(mapPreparedDepositTxInPoints);

        if (nValue + nFeeRequired > GetBalance(mapPreparedDepositTxInPoints))
            strError = strprintf(_("Error: This transaction requires a transaction fee of at least %s because of its amount, complexity, or use of recently received funds!"), FormatMoney(nFeeRequired));
        LogPrintf("SendMoney() : %s\n", strError);
        return strError;
    }

    //Just an empty vector
    std::vector<Credits_CReserveKey *> keyRecipients;
    if (!CommitTransaction(bitcoin_wallet, wtxNew, reservekey, keyRecipients))
        return _("Error: The transaction was rejected! This might happen if some of the coins in your wallet were already spent, such as if you used a copy of wallet.dat and coins were spent in the copy but not marked as spent here.");

    return "";
}



string Credits_CWallet::SendMoneyToDestination(Bitcoin_CWallet *bitcoin_wallet, Credits_CWallet *deposit_wallet, const CTxDestination& address, int64_t nValue, Credits_CWalletTx& wtxNew)
{
    // Check amount
    if (nValue <= 0)
        return _("Invalid amount");

    //Find all prepared deposit transactions
    map<uint256, set<int> > mapPreparedDepositTxInPoints;
    deposit_wallet->PreparedDepositTxInPoints(mapPreparedDepositTxInPoints);

    if (nValue + credits_nTransactionFee > GetBalance(mapPreparedDepositTxInPoints))
        return _("Insufficient funds");

    // Parse Credits address
    CScript scriptPubKey;
    scriptPubKey.SetDestination(address);

    return SendMoney(bitcoin_wallet, deposit_wallet, scriptPubKey, nValue, wtxNew);
}




Credits_DBErrors Credits_CWallet::LoadWallet(bool& fFirstRunRet, uint64_t &nAccountingEntryNumber)
{
    if (!fFileBacked)
        return CREDITS_DB_LOAD_OK;
    fFirstRunRet = false;
    Credits_DBErrors nLoadWalletRet = Credits_CWalletDB(strWalletFile, pbitDb,"cr+").LoadWallet(this, nAccountingEntryNumber);
    if (nLoadWalletRet == CREDITS_DB_NEED_REWRITE)
    {
        if (Credits_CDB::Rewrite(strWalletFile, pbitDb, "\x04pool"))
        {
            LOCK(cs_wallet);
            setKeyPool.clear();
            // Note: can't top-up keypool here, because wallet is locked.
            // User will be prompted to unlock wallet the next operation
            // the requires a new key.
        }
    }

    if (nLoadWalletRet != CREDITS_DB_LOAD_OK)
        return nLoadWalletRet;
    fFirstRunRet = !vchDefaultKey.IsValid();

    uiInterface.LoadWallet(this);

    return CREDITS_DB_LOAD_OK;
}


Credits_DBErrors Credits_CWallet::ZapWalletTx()
{
    if (!fFileBacked)
        return CREDITS_DB_LOAD_OK;
    Credits_DBErrors nZapWalletTxRet = Credits_CWalletDB(strWalletFile, pbitDb,"cr+").ZapWalletTx(this);
    if (nZapWalletTxRet == CREDITS_DB_NEED_REWRITE)
    {
        if (Credits_CDB::Rewrite(strWalletFile, pbitDb, "\x04pool"))
        {
            LOCK(cs_wallet);
            setKeyPool.clear();
            // Note: can't top-up keypool here, because wallet is locked.
            // User will be prompted to unlock wallet the next operation
            // the requires a new key.
        }
    }

    if (nZapWalletTxRet != CREDITS_DB_LOAD_OK)
        return nZapWalletTxRet;

    return CREDITS_DB_LOAD_OK;
}


bool Credits_CWallet::SetAddressBook(const CTxDestination& address, const string& strName, const string& strPurpose)
{
    bool fUpdated = false;
    {
        LOCK(cs_wallet); // mapAddressBook
        std::map<CTxDestination, CAddressBookData>::iterator mi = mapAddressBook.find(address);
        fUpdated = mi != mapAddressBook.end();
        mapAddressBook[address].name = strName;
        if (!strPurpose.empty()) /* update purpose only if requested */
            mapAddressBook[address].purpose = strPurpose;
    }
    NotifyAddressBookChanged(this, address, strName, ::IsMine(*this, address),
                             strPurpose, (fUpdated ? CT_UPDATED : CT_NEW) );
    if (!fFileBacked)
        return false;
    if (!strPurpose.empty() && !Credits_CWalletDB(strWalletFile, pbitDb).WritePurpose(CBitcoinAddress(address).ToString(), strPurpose))
        return false;
    return Credits_CWalletDB(strWalletFile, pbitDb).WriteName(CBitcoinAddress(address).ToString(), strName);
}

bool Credits_CWallet::DelAddressBook(const CTxDestination& address)
{
    {
        LOCK(cs_wallet); // mapAddressBook

        if(fFileBacked)
        {
            // Delete destdata tuples associated with address
            std::string strAddress = CBitcoinAddress(address).ToString();
            BOOST_FOREACH(const PAIRTYPE(string, string) &item, mapAddressBook[address].destdata)
            {
                Credits_CWalletDB(strWalletFile, pbitDb).EraseDestData(strAddress, item.first);
            }
        }
        mapAddressBook.erase(address);
    }

    NotifyAddressBookChanged(this, address, "", ::IsMine(*this, address), "", CT_DELETED);

    if (!fFileBacked)
        return false;
    Credits_CWalletDB(strWalletFile, pbitDb).ErasePurpose(CBitcoinAddress(address).ToString());
    return Credits_CWalletDB(strWalletFile, pbitDb).EraseName(CBitcoinAddress(address).ToString());
}

bool Credits_CWallet::SetDefaultKey(const CPubKey &vchPubKey)
{
    if (fFileBacked)
    {
        if (!Credits_CWalletDB(strWalletFile, pbitDb).WriteDefaultKey(vchPubKey))
            return false;
    }
    vchDefaultKey = vchPubKey;
    return true;
}

//
// Mark old keypool keys as used,
// and generate all new keys
//
bool Credits_CWallet::NewKeyPool()
{
    {
        LOCK(cs_wallet);
        Credits_CWalletDB walletdb(strWalletFile, pbitDb);
        BOOST_FOREACH(int64_t nIndex, setKeyPool)
            walletdb.ErasePool(nIndex);
        setKeyPool.clear();

        if (IsLocked())
            return false;

        int64_t nKeys = max(GetArg("-keypool", 100), (int64_t)0);
        for (int i = 0; i < nKeys; i++)
        {
            int64_t nIndex = i+1;
            walletdb.WritePool(nIndex, CKeyPool(GenerateNewKey()));
            setKeyPool.insert(nIndex);
        }
        LogPrintf("CWallet::NewKeyPool wrote %d new keys\n", nKeys);
    }
    return true;
}

bool Credits_CWallet::TopUpKeyPool(unsigned int kpSize)
{
    {
        LOCK(cs_wallet);

        if (IsLocked())
            return false;

        Credits_CWalletDB walletdb(strWalletFile, pbitDb);

        // Top up key pool
        unsigned int nTargetSize;
        if (kpSize > 0)
            nTargetSize = kpSize;
        else
            nTargetSize = max(GetArg("-keypool", 100), (int64_t) 0);

        while (setKeyPool.size() < (nTargetSize + 1))
        {
            int64_t nEnd = 1;
            if (!setKeyPool.empty())
                nEnd = *(--setKeyPool.end()) + 1;
            if (!walletdb.WritePool(nEnd, CKeyPool(GenerateNewKey())))
                throw runtime_error("TopUpKeyPool() : writing generated key failed");
            setKeyPool.insert(nEnd);
            LogPrintf("keypool added key %d, size=%u\n", nEnd, setKeyPool.size());
        }
    }
    return true;
}

void Credits_CWallet::ReserveKeyFromKeyPool(int64_t& nIndex, CKeyPool& keypool)
{
    nIndex = -1;
    keypool.vchPubKey = CPubKey();
    {
        LOCK(cs_wallet);

        if (!IsLocked())
            TopUpKeyPool();

        // Get the oldest key
        if(setKeyPool.empty())
            return;

        Credits_CWalletDB walletdb(strWalletFile, pbitDb);

        nIndex = *(setKeyPool.begin());
        setKeyPool.erase(setKeyPool.begin());
        if (!walletdb.ReadPool(nIndex, keypool))
            throw runtime_error("ReserveKeyFromKeyPool() : read failed");
        if (!HaveKey(keypool.vchPubKey.GetID()))
            throw runtime_error("ReserveKeyFromKeyPool() : unknown key in key pool");
        assert(keypool.vchPubKey.IsValid());
        LogPrintf("keypool reserve %d\n", nIndex);
    }
}

void Credits_CWallet::KeepKey(int64_t nIndex)
{
    // Remove from key pool
    if (fFileBacked)
    {
        Credits_CWalletDB walletdb(strWalletFile, pbitDb);
        walletdb.ErasePool(nIndex);
    }
    LogPrintf("keypool keep %d\n", nIndex);
}

void Credits_CWallet::ReturnKey(int64_t nIndex)
{
    // Return to key pool
    {
        LOCK(cs_wallet);
        setKeyPool.insert(nIndex);
    }
    LogPrintf("keypool return %d\n", nIndex);
}

bool Credits_CWallet::GetKeyFromPool(CPubKey& result)
{
    int64_t nIndex = 0;
    CKeyPool keypool;
    {
        LOCK(cs_wallet);
        ReserveKeyFromKeyPool(nIndex, keypool);
        if (nIndex == -1)
        {
            if (IsLocked()) return false;
            result = GenerateNewKey();
            return true;
        }
        KeepKey(nIndex);
        result = keypool.vchPubKey;
    }
    return true;
}

int64_t Credits_CWallet::GetOldestKeyPoolTime()
{
    int64_t nIndex = 0;
    CKeyPool keypool;
    ReserveKeyFromKeyPool(nIndex, keypool);
    if (nIndex == -1)
        return GetTime();
    ReturnKey(nIndex);
    return keypool.nTime;
}

std::map<CTxDestination, int64_t> Credits_CWallet::GetAddressBalances()
{
    map<CTxDestination, int64_t> balances;

    {
        LOCK(cs_wallet);
        BOOST_FOREACH(PAIRTYPE(uint256, Credits_CWalletTx) walletEntry, mapWallet)
        {
            Credits_CWalletTx *pcoin = &walletEntry.second;

            if (!Credits_IsFinalTx(*pcoin) || !pcoin->IsTrusted())
                continue;

            if (pcoin->IsCoinBase() && pcoin->GetBlocksToMaturity() > 0)
                continue;
            if(pcoin->IsDeposit() && pcoin->vout.size() == 1 && pcoin->GetFirstDepositOutBlocksToMaturity() > 0)
            	continue;
            if(pcoin->IsDeposit() && pcoin->vout.size() == 2 && pcoin->GetSecondDepositOutBlocksToMaturity() > 0)
            	continue;

            int nDepth = pcoin->GetDepthInMainChain();
            if (nDepth < (pcoin->IsFromMe() ? 0 : 1))
                continue;

            for (unsigned int i = 0; i < pcoin->vout.size(); i++)
            {
                CTxDestination addr;
                if (!IsMine(pcoin->vout[i]))
                    continue;
                if(!ExtractDestination(pcoin->vout[i].scriptPubKey, addr))
                    continue;

                int64_t n = IsSpent(walletEntry.first, i) ? 0 : pcoin->vout[i].nValue;

                if (!balances.count(addr))
                    balances[addr] = 0;
                balances[addr] += n;
            }
        }
    }

    return balances;
}

set< set<CTxDestination> > Credits_CWallet::GetAddressGroupings()
{
    AssertLockHeld(cs_wallet); // mapWallet
    set< set<CTxDestination> > groupings;
    set<CTxDestination> grouping;

    BOOST_FOREACH(PAIRTYPE(uint256, Credits_CWalletTx) walletEntry, mapWallet)
    {
        Credits_CWalletTx *pcoin = &walletEntry.second;

        if (pcoin->vin.size() > 0)
        {
            bool any_mine = false;
            // group all input addresses with each other
            BOOST_FOREACH(Credits_CTxIn txin, pcoin->vin)
            {
                CTxDestination address;
                if(!IsMine(txin)) /* If this input isn't mine, ignore it */
                    continue;
                if(!ExtractDestination(mapWallet[txin.prevout.hash].vout[txin.prevout.n].scriptPubKey, address))
                    continue;
                grouping.insert(address);
                any_mine = true;
            }

            // group change with input addresses
            if (any_mine)
            {
               BOOST_FOREACH(CTxOut txout, pcoin->vout)
                   if (IsChange(txout))
                   {
                       CTxDestination txoutAddr;
                       if(!ExtractDestination(txout.scriptPubKey, txoutAddr))
                           continue;
                       grouping.insert(txoutAddr);
                   }
            }
            if (grouping.size() > 0)
            {
                groupings.insert(grouping);
                grouping.clear();
            }
        }

        // group lone addrs by themselves
        for (unsigned int i = 0; i < pcoin->vout.size(); i++)
            if (IsMine(pcoin->vout[i]))
            {
                CTxDestination address;
                if(!ExtractDestination(pcoin->vout[i].scriptPubKey, address))
                    continue;
                grouping.insert(address);
                groupings.insert(grouping);
                grouping.clear();
            }
    }

    set< set<CTxDestination>* > uniqueGroupings; // a set of pointers to groups of addresses
    map< CTxDestination, set<CTxDestination>* > setmap;  // map addresses to the unique group containing it
    BOOST_FOREACH(set<CTxDestination> grouping, groupings)
    {
        // make a set of all the groups hit by this new group
        set< set<CTxDestination>* > hits;
        map< CTxDestination, set<CTxDestination>* >::iterator it;
        BOOST_FOREACH(CTxDestination address, grouping)
            if ((it = setmap.find(address)) != setmap.end())
                hits.insert((*it).second);

        // merge all hit groups into a new single group and delete old groups
        set<CTxDestination>* merged = new set<CTxDestination>(grouping);
        BOOST_FOREACH(set<CTxDestination>* hit, hits)
        {
            merged->insert(hit->begin(), hit->end());
            uniqueGroupings.erase(hit);
            delete hit;
        }
        uniqueGroupings.insert(merged);

        // update setmap
        BOOST_FOREACH(CTxDestination element, *merged)
            setmap[element] = merged;
    }

    set< set<CTxDestination> > ret;
    BOOST_FOREACH(set<CTxDestination>* uniqueGrouping, uniqueGroupings)
    {
        ret.insert(*uniqueGrouping);
        delete uniqueGrouping;
    }

    return ret;
}

set<CTxDestination> Credits_CWallet::GetAccountAddresses(string strAccount) const
{
    AssertLockHeld(cs_wallet); // mapWallet
    set<CTxDestination> result;
    BOOST_FOREACH(const PAIRTYPE(CTxDestination, CAddressBookData)& item, mapAddressBook)
    {
        const CTxDestination& address = item.first;
        const string& strName = item.second.name;
        if (strName == strAccount)
            result.insert(address);
    }
    return result;
}

bool Credits_CReserveKey::GetReservedKey(CPubKey& pubkey)
{
    if (nIndex == -1)
    {
        CKeyPool keypool;
        pwallet->ReserveKeyFromKeyPool(nIndex, keypool);
        if (nIndex != -1)
            vchPubKey = keypool.vchPubKey;
        else {
            if (pwallet->vchDefaultKey.IsValid()) {
                LogPrintf("CReserveKey::GetReservedKey(): Warning: Using default key instead of a new key, top up your keypool!");
                vchPubKey = pwallet->vchDefaultKey;
            } else
                return false;
        }
    }
    assert(vchPubKey.IsValid());
    pubkey = vchPubKey;
    return true;
}

void Credits_CReserveKey::KeepKey()
{
    if (nIndex != -1)
        pwallet->KeepKey(nIndex);
    nIndex = -1;
    vchPubKey = CPubKey();
}

void Credits_CReserveKey::ReturnKey()
{
    if (nIndex != -1)
        pwallet->ReturnKey(nIndex);
    nIndex = -1;
    vchPubKey = CPubKey();
}

void Credits_CWallet::GetAllReserveKeys(set<CKeyID>& setAddress) const
{
    setAddress.clear();

    Credits_CWalletDB walletdb(strWalletFile, pbitDb);

    LOCK2(cs_main, cs_wallet);
    BOOST_FOREACH(const int64_t& id, setKeyPool)
    {
        CKeyPool keypool;
        if (!walletdb.ReadPool(id, keypool))
            throw runtime_error("GetAllReserveKeyHashes() : read failed");
        assert(keypool.vchPubKey.IsValid());
        CKeyID keyID = keypool.vchPubKey.GetID();
        if (!HaveKey(keyID))
            throw runtime_error("GetAllReserveKeyHashes() : unknown key in key pool");
        setAddress.insert(keyID);
    }
}

void Credits_CWallet::UpdatedTransaction(const uint256 &hashTx)
{
    {
        LOCK(cs_wallet);
        // Only notify UI if this transaction is in this wallet
        map<uint256, Credits_CWalletTx>::const_iterator mi = mapWallet.find(hashTx);
        if (mi != mapWallet.end())
            NotifyTransactionChanged(this, hashTx, CT_UPDATED);
    }
}

void Credits_CWallet::LockCoin(COutPoint& output)
{
    AssertLockHeld(cs_wallet); // setLockedCoins
    setLockedCoins.insert(output);
}

void Credits_CWallet::UnlockCoin(COutPoint& output)
{
    AssertLockHeld(cs_wallet); // setLockedCoins
    setLockedCoins.erase(output);
}

void Credits_CWallet::UnlockAllCoins()
{
    AssertLockHeld(cs_wallet); // setLockedCoins
    setLockedCoins.clear();
}

bool Credits_CWallet::IsLockedCoin(uint256 hash, unsigned int n) const
{
    AssertLockHeld(cs_wallet); // setLockedCoins
    COutPoint outpt(hash, n);

    return (setLockedCoins.count(outpt) > 0);
}

void Credits_CWallet::ListLockedCoins(std::vector<COutPoint>& vOutpts)
{
    AssertLockHeld(cs_wallet); // setLockedCoins
    for (std::set<COutPoint>::iterator it = setLockedCoins.begin();
         it != setLockedCoins.end(); it++) {
    	COutPoint outpt = (*it);
        vOutpts.push_back(outpt);
    }
}

void Credits_CWallet::GetKeyBirthTimes(std::map<CKeyID, int64_t> &mapKeyBirth) const {
    AssertLockHeld(cs_wallet); // mapKeyMetadata
    mapKeyBirth.clear();

    // get birth times for keys with metadata
    for (std::map<CKeyID, Credits_CKeyMetadata>::const_iterator it = mapKeyMetadata.begin(); it != mapKeyMetadata.end(); it++)
        if (it->second.nCreateTime)
            mapKeyBirth[it->first] = it->second.nCreateTime;

    // map in which we'll infer heights of other keys
    Credits_CBlockIndex *pindexMax = credits_chainActive[std::max(0, credits_chainActive.Height() - 144)]; // the tip can be reorganised; use a 144-block safety margin
    std::map<CKeyID, Credits_CBlockIndex*> mapKeyFirstBlock;
    std::set<CKeyID> setKeys;
    GetKeys(setKeys);
    BOOST_FOREACH(const CKeyID &keyid, setKeys) {
        if (mapKeyBirth.count(keyid) == 0)
            mapKeyFirstBlock[keyid] = pindexMax;
    }
    setKeys.clear();

    // if there are no such keys, we're done
    if (mapKeyFirstBlock.empty())
        return;

    // find first block that affects those keys, if there are any left
    std::vector<CKeyID> vAffected;
    for (std::map<uint256, Credits_CWalletTx>::const_iterator it = mapWallet.begin(); it != mapWallet.end(); it++) {
        // iterate over all wallet transactions...
        const Credits_CWalletTx &wtx = (*it).second;
        std::map<uint256, Credits_CBlockIndex*>::const_iterator blit = credits_mapBlockIndex.find(wtx.hashBlock);
        if (blit != credits_mapBlockIndex.end() && credits_chainActive.Contains(blit->second)) {
            // ... which are already in a block
            int nHeight = blit->second->nHeight;
            BOOST_FOREACH(const CTxOut &txout, wtx.vout) {
                // iterate over all their outputs
                ::ExtractAffectedKeys(*this, txout.scriptPubKey, vAffected);
                BOOST_FOREACH(const CKeyID &keyid, vAffected) {
                    // ... and all their affected keys
                    std::map<CKeyID, Credits_CBlockIndex*>::iterator rit = mapKeyFirstBlock.find(keyid);
                    if (rit != mapKeyFirstBlock.end() && nHeight < rit->second->nHeight)
                        rit->second = blit->second;
                }
                vAffected.clear();
            }
        }
    }

    // Extract block timestamps for those keys
    for (std::map<CKeyID, Credits_CBlockIndex*>::const_iterator it = mapKeyFirstBlock.begin(); it != mapKeyFirstBlock.end(); it++)
        mapKeyBirth[it->first] = it->second->nTime - 7200; // block times can be 2h off
}

bool Credits_CWallet::AddDestData(const CTxDestination &dest, const std::string &key, const std::string &value)
{
    if (boost::get<CNoDestination>(&dest))
        return false;

    mapAddressBook[dest].destdata.insert(std::make_pair(key, value));
    if (!fFileBacked)
        return true;
    return Credits_CWalletDB(strWalletFile, pbitDb).WriteDestData(CBitcoinAddress(dest).ToString(), key, value);
}

bool Credits_CWallet::EraseDestData(const CTxDestination &dest, const std::string &key)
{
    if (!mapAddressBook[dest].destdata.erase(key))
        return false;
    if (!fFileBacked)
        return true;
    return Credits_CWalletDB(strWalletFile, pbitDb).EraseDestData(CBitcoinAddress(dest).ToString(), key);
}

bool Credits_CWallet::LoadDestData(const CTxDestination &dest, const std::string &key, const std::string &value)
{
    mapAddressBook[dest].destdata.insert(std::make_pair(key, value));
    return true;
}

bool Credits_CWallet::GetDestData(const CTxDestination &dest, const std::string &key, std::string *value) const
{
    std::map<CTxDestination, CAddressBookData>::const_iterator i = mapAddressBook.find(dest);
    if(i != mapAddressBook.end())
    {
        CAddressBookData::StringMap::const_iterator j = i->second.destdata.find(key);
        if(j != i->second.destdata.end())
        {
            if(value)
                *value = j->second;
            return true;
        }
    }
    return false;
}
