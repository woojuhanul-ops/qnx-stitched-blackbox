@echo off
call D:\QNX\sdp8\qnxsdp-env.bat
cd /d D:\Chronica\qnx\blackbox
echo === BUILD ledgerd ===
qcc -Vgcc_ntox86_64 -Wall -O2 -o ledgerd ledgerd.c retriever.c
echo QCC_EXIT=%ERRORLEVEL%
echo === BUILD sensord ===
qcc -Vgcc_ntox86_64 -Wall -O2 -o sensord sensord.c
echo QCC_EXIT=%ERRORLEVEL%
echo === BUILD watchdog ===
qcc -Vgcc_ntox86_64 -Wall -O2 -o watchdog watchdog.c
echo QCC_EXIT=%ERRORLEVEL%
echo === BUILD verifier ===
qcc -Vgcc_ntox86_64 -Wall -O2 -o verifier verifier.c
echo QCC_EXIT=%ERRORLEVEL%
echo === MKIFS ===
mkifs blackbox.build blackbox.ifs
echo MKIFS_EXIT=%ERRORLEVEL%
dir ledgerd sensord watchdog verifier blackbox.ifs
