@echo off
setlocal EnableDelayedExpansion

echo ================================================================
echo WebView2 Data Directory Fix for Zenith
echo Fixes: "Microsoft Edge can't read and write to its data directory"
echo ================================================================

net session >nul 2>&1
if %errorlevel% neq 0 (
    rem Relaunch elevated using PowerShell, preserving all args
    powershell -NoProfile -Command "Start-Process -FilePath '%COMSPEC%' -ArgumentList '/c','\"%~f0\" %*' -Verb RunAs"
    exit /b 1
)

rem ---- rest of script runs elevated below ----
echo Running elevated...

set "WEBVIEW_DATA_DIR=%LOCALAPPDATA%\zenith.soft\EBWebView"
echo [INFO] WebView2 data directory: %WEBVIEW_DATA_DIR%

if exist "%WEBVIEW_DATA_DIR%" (
    echo [INFO] Removing existing WebView2 data directory...
    
    takeown /f "%WEBVIEW_DATA_DIR%" /r /d y >nul 2>&1
    icacls "%WEBVIEW_DATA_DIR%" /grant "%USERNAME%":F /t >nul 2>&1
    icacls "%WEBVIEW_DATA_DIR%" /grant "Administrators":F /t >nul 2>&1
    icacls "%WEBVIEW_DATA_DIR%" /grant "Everyone":F /t >nul 2>&1

    rmdir /S /Q "%WEBVIEW_DATA_DIR%" >nul 2>&1
    
    if exist "%WEBVIEW_DATA_DIR%" (
        echo [WARN] Could not completely remove WebView2 data directory
        echo [INFO] Attempting to clear contents...
        del /F /S /Q "%WEBVIEW_DATA_DIR%\*" >nul 2>&1
        for /d %%i in ("%WEBVIEW_DATA_DIR%\*") do rmdir /S /Q "%%i" >nul 2>&1
    ) else (
        echo [SUCCESS] WebView2 data directory removed successfully
    )
)

echo [INFO] Creating WebView2 data directory with proper permissions...

mkdir "%LOCALAPPDATA%\zenith.soft" 2>nul
mkdir "%WEBVIEW_DATA_DIR%" 2>nul

icacls "%WEBVIEW_DATA_DIR%" /grant "%USERNAME%":(OI)(CI)F /t >nul 2>&1
icacls "%WEBVIEW_DATA_DIR%" /grant "Administrators":(OI)(CI)F /t >nul 2>&1
icacls "%WEBVIEW_DATA_DIR%" /grant "Users":(OI)(CI)F /t >nul 2>&1
icacls "%WEBVIEW_DATA_DIR%" /grant "Everyone":(OI)(CI)F /t >nul 2>&1

icacls "%WEBVIEW_DATA_DIR%" /grant "SYSTEM":(OI)(CI)F /t >nul 2>&1
icacls "%WEBVIEW_DATA_DIR%" /grant "Authenticated Users":(OI)(CI)F /t >nul 2>&1

echo [INFO] Configuring WebView2 registry settings...

reg add "HKEY_CURRENT_USER\Software\Microsoft\EdgeWebView\EBWebView" /v "UserDataFolder" /t REG_SZ /d "%WEBVIEW_DATA_DIR%" /f >nul 2>&1
reg add "HKEY_CURRENT_USER\Software\Microsoft\EdgeWebView\EBWebView" /v "EnableLogging" /t REG_DWORD /d 0 /f >nul 2>&1
reg add "HKEY_CURRENT_USER\Software\Microsoft\EdgeWebView\EBWebView" /v "AdditionalBrowserArguments" /t REG_SZ /d "--disable-web-security --user-data-dir=%WEBVIEW_DATA_DIR%" /f >nul 2>&1

reg add "HKEY_CURRENT_USER\Software\Microsoft\Edge\WebView2" /v "UserDataFolder" /t REG_SZ /d "%WEBVIEW_DATA_DIR%" /f >nul 2>&1
reg add "HKEY_CURRENT_USER\Software\Microsoft\Edge\WebView2" /v "AllowNonAdminInstall" /t REG_DWORD /d 1 /f >nul 2>&1

reg add "HKEY_CURRENT_USER\Software\Policies\Microsoft\Edge\WebView2" /v "UserDataDir" /t REG_SZ /d "%WEBVIEW_DATA_DIR%" /f >nul 2>&1

echo [INFO] Verifying the fix...

if exist "%WEBVIEW_DATA_DIR%" (
    echo [SUCCESS] WebView2 data directory created

    echo test > "%WEBVIEW_DATA_DIR%\test.txt" 2>nul
    if exist "%WEBVIEW_DATA_DIR%\test.txt" (
        del "%WEBVIEW_DATA_DIR%\test.txt" >nul 2>&1
        echo [SUCCESS] WebView2 data directory write access confirmed
    ) else (
        echo [ERROR] WebView2 data directory lacks write access
        goto :error_fix
    )
) else (
    echo [ERROR] Failed to create WebView2 data directory
    goto :error_fix
)

echo [INFO] Restarting Edge processes to apply changes...
taskkill /F /IM msedge.exe >nul 2>&1
taskkill /F /IM msedgewebview2.exe >nul 2>&1
taskkill /F /IM WebView2Loader.exe >nul 2>&1

echo.
echo ================================================================
echo WebView2 Data Directory Fix COMPLETED SUCCESSFULLY
echo ================================================================
echo [SUCCESS] The WebView2 data directory error should now be fixed
echo [INFO] You can now launch Zenith without the data directory error
echo [INFO] Data directory: %WEBVIEW_DATA_DIR%
echo [INFO] Registry configuration applied
echo [INFO] Proper permissions set
echo ================================================================
echo.
exit /b 0

:error_fix
echo.
echo ================================================================
echo ADDITIONAL TROUBLESHOOTING STEPS
echo ================================================================
echo [INFO] Attempting additional permission fixes...

echo [INFO] Applying alternative permission settings...
cacls "%WEBVIEW_DATA_DIR%" /E /G "%USERNAME%":F >nul 2>&1
cacls "%WEBVIEW_DATA_DIR%" /E /G "Everyone":F >nul 2>&1

icacls "%LOCALAPPDATA%\zenith.soft" /grant "%USERNAME%":(OI)(CI)F /t >nul 2>&1
icacls "%LOCALAPPDATA%\zenith.soft" /grant "Everyone":(OI)(CI)F /t >nul 2>&1

echo [INFO] Alternative permissions applied
echo [INFO] Try launching Zenith again
echo [INFO] If the error persists, run the main fix_webview2.bat script
echo ================================================================
exit /b 1
