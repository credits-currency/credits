Credits Core version 0.9.1.73 is now available from:

  http://credits-currency.org

How to Upgrade
--------------

NOTE! With version 0.9.1.73 some of the performance improvements added in version .70 and .71 
has been (temporarily) removed. This means that you can run this version directly if you are coming
from version 0.9.1.60, which the major part of all users have.

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

0.9.1.73 Release notes
=======================

0.9.1.73 is a minor release, aiming to correct the syncing speed problems that have been presented
since earlier versions, but became more pronounced in version 0.9.1.70.
- Removing the joining of the two chainstates for claiming and bitcoin 
  to get rid of performance problems on windows.
- Joining the threads that handles messages for Credits
  and Bitcoin.
- Minor bug fixes related to syncing speed.
