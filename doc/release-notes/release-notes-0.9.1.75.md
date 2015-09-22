Credits Core version 0.9.1.75 is now available from:

  http://credits-currency.org

How to Upgrade
--------------

If you are running an older version, shut it down. Wait until it has completely
shut down (which might take a few minutes for older versions), then run the
installer (on Windows) or just copy over /Applications/Credits-Qt (on Mac) or
creditsd/credits-qt (on Linux).

!!!BEFORE YOU START THE WALLET, MAKE SURE TO MAKE BACKUP COPIES OF THE FILES BITCREDIT_WALLET.DAT
AND BITCOIN_WALLET.DAT, IF YOU HAVE A PREVIOUS INSTALLATION.!!!

Once you have security copies of the wallets, do the following:
1. If you want the be really certain that no problems occur, 
   make a temporary backup copy of the working directory (named *_060 or similar). 
2. Start credits-qt or creditsd with the command:
    credits-qt -checklevel=2
    or
    creditsd -checklevel=2
    
Once you can verify that the new working directory is in sync (can take a day depending on your network
connection), you can delete the old working directory.

NOTE!!!!
If you are getting errors when trying to use a 0.9.1.60 working directory, you will either have to:
a) Sync the blockchain from start or
b) Initialize from the torrent file that can be found at http://credits-currency.org/viewtopic.php?f=18&t=517.
NOTE!!!!

0.9.1.75 Release notes
=======================

0.9.1.75 is a minor release, improving on block download and parallell chain sync.
- Removed restriction to wait for Bitcoin blockchain download until Credits download starts.
- Added on disk storage for orphaned Bitcoin and Credits blocks. This improves download times and 
  decreases bandwidth usage. 
- Orphaned Credits blocks parsed as soon as the corresponding Bitcoin block arrives. This helps both 
  blockchain progress in parallel.
- Improved ui, showing both chains download progress from start, instead of Credits blockchain 
  showing "No block source available" info.
- Hanging ui improvements, due to parallelisation of Bitcoin and Credits blockchain download.
