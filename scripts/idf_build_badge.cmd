@echo off
setlocal
call "C:\Espressif\idf_cmd_init.bat" esp-idf-20ee62e792ea89630ac6a777ab3ebc57
if errorlevel 1 exit /b %errorlevel%
cd /d "C:\Users\andrew\OneDrive - Green O365\Documents\GitHub\dashcdg\platform\espidf\projects\dashcdg_badge"
idf.py --version
if errorlevel 1 exit /b %errorlevel%
idf.py build
exit /b %errorlevel%
