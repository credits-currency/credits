// Copyright (c) 2009-2013 The Bitcoin developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_CHECKPOINT_H
#define BITCOIN_CHECKPOINT_H

#include <map>

class Bitcredit_CBlockIndex;
class uint256;

/** Block-chain checkpoints are compiled-in sanity checks.
 * They are updated every release or three.
 */
namespace Checkpoints
{
    // Returns true if block passes checkpoint checks
    bool Bitcredit_CheckBlock(int nHeight, const uint256& hash);

    // Return conservative estimate of total number of blocks, 0 if unknown
    int Bitcredit_GetTotalBlocksEstimate();

    // Returns last CBlockIndex* in mapBlockIndex that is a checkpoint
    Bitcredit_CBlockIndex* Bitcredit_GetLastCheckpoint(const std::map<uint256, Bitcredit_CBlockIndex*>& mapBlockIndex);

    double Bitcredit_GuessVerificationProgress(Bitcredit_CBlockIndex *pindex, bool fSigchecks = true);

    extern bool bitcredit_fEnabled;
}

#endif
