Credits Core version 0.9.1.71 is now available from:

  http://credits-currency.org

How to Upgrade
--------------

NOTE! Starting with version 0.9.1.70 a more efficient storage and compression algorithm is
applied to the working directory. THIS MEANS THAT WHEN UPGRADING YOU CAN NOT USE THE OLD 
WORKING DIRECTORY AND HAVE TO RESYNC FROM AN EMPTY WORKING DIRECTORY.

The default working directories are:
For Windows: C:/Users/<user home directory>/AppData/Roaming/Credits
For Ubuntu/Linux: <user home directory>/.credits
For Mac: /Users/<user home directory>/Library/Application Support/Credits

If you are running an older version, shut it down. Wait until it has completely
shut down (which might take a few minutes for older versions), then run the
installer (on Windows) or just copy over /Applications/Credits-Qt (on Mac) or
creditsd/credits-qt (on Linux).

!!!BEFORE YOU START THE WALLET, MAKE SURE TO MAKE BACKUP COPIES OF THE FILES BITCREDIT_WALLET.DAT
AND BITCOIN_WALLET.DAT, IF YOU HAVE A PREVIOUS INSTALLATION.!!!

Once you have security copies of the wallets, do the following:
1. Rename the old working directory from Credits to Credits_old
2. Create a new directory called Credits
3. Copy the following files from Credits_old to Credits:
    - credits_wallet.dat
    - bitcoin_wallet.dat
    - credits.conf
4. Start credits-qt or creditsd with the command:
    credits-qt -checklevel=2
    or
    creditsd -checklevel=2
    
Once you can verify that the new working directory is in sync (can take a day depending on your network
connection), you can delete the old working directory.

0.9.1.71 Release notes
=======================

0.9.1.71 is a minor release, aiming to correct the syncing speed problems that have been presented
since earlier versions, but became more pronounced in version 0.9.1.70.
- Joining the threads that handles messages for Credits
  and Bitcoin.
- Minor bug fixes related to syncing speed.
