$ErrorActionPreference = "Stop"

& "C:\Espressif\Initialize-Idf.ps1" -IdfId "esp-idf-20ee62e792ea89630ac6a777ab3ebc57"

Set-Location "C:\Users\andrew\OneDrive - Green O365\Documents\GitHub\dashcdg\platform\espidf\projects\dashcdg_badge"

idf.py --version
idf.py -p COM6 build flash
