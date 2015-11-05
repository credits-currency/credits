Credits Core version 0.9.1.80 is now available from:

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
   make a temporary backup copy of the working directory (named *_old or similar). 
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

0.9.1.80 Release notes
=======================

0.9.1.80 is a minor release, no updates to the working directory is neccessary when installing.
- NOTE!! Hard fork will occur at block 50000.
- Version number bumped to .80 to indicate hard fork version
- Deposit transactions will be allowed to have more than one input. This will enable the wallet (in the release after this one) to automatically create deposits from any content that you have in a wallet.
- The number of deposit transactions in a block will be allowed up to a maximum of 1000, updated from previously 10. This will make it easier to enable pooled mining, with externally provided deposits.
- Changed difficulty adjustment algorithm at block 50 000. The new diff adjustment algorithm will be much faster. It will update every 252 blocks with a max adjustment factor of 2 up or down. Adjustment speed will be 64 times higher than current diff adjustment. The reason for this is twofold:
   1. To prevent block generation from being stuck if a sudden drop in mining power occurs. 
   2. In the future when deposit mining will be more and more important, preventing the block generation from being stuck if deposits are being withheld.
   
