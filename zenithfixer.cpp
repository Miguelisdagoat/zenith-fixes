#include <windows.h>
#include <shlobj.h>
#include <shellapi.h>
#include <string>
#include <vector>
#include <filesystem>
#include <thread>
#include <chrono>
#include <algorithm>
#include <atlbase.h>
#include <exdisp.h>
#include <shobjidl.h>
#include <commctrl.h>
#undef ShellExecute
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "comctl32.lib")


static const UINT WM_APP_PROGRESS = WM_APP + 1;

static void AppendLog(HWND hEdit, const std::wstring& line) {
    std::wstring withNL = line + L"\r\n";
    SendMessageW(hEdit, EM_SETSEL, (WPARAM)-1, (LPARAM)-1);
    SendMessageW(hEdit, EM_REPLACESEL, FALSE, (LPARAM)withNL.c_str());
}

static void PostLogAndProgress(HWND hwnd, HWND hEdit, const std::wstring& line, int progress) {

    PostMessageW(hwnd, WM_APP_PROGRESS, (WPARAM)progress, 0);

    AppendLog(hEdit, line);
}

static std::wstring Trim(const std::wstring& s) {
    size_t start = s.find_first_not_of(L" \t\r\n");
    size_t end = s.find_last_not_of(L" \t\r\n");
    if (start == std::wstring::npos) return L"";
    return s.substr(start, end - start + 1);
}

static bool CaseInsensitiveEquals(const std::wstring& a, const std::wstring& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (towlower(a[i]) != towlower(b[i])) return false;
    }
    return true;
}

static bool ContainsCaseInsensitive(const std::wstring& hay, const std::wstring& needle) {
    std::wstring H = hay, N = needle;
    std::transform(H.begin(), H.end(), H.begin(), ::towlower);
    std::transform(N.begin(), N.end(), N.begin(), ::towlower);
    return H.find(N) != std::wstring::npos;
}

static bool IsProcessElevated() {
    HANDLE hToken = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &hToken)) return false;
    TOKEN_ELEVATION elevation{};
    DWORD size = 0;
    bool elevated = false;
    if (GetTokenInformation(hToken, TokenElevation, &elevation, sizeof(elevation), &size)) {
        elevated = elevation.TokenIsElevated != 0;
    }
    CloseHandle(hToken);
    return elevated;
}

static bool RelaunchElevated(HWND owner) {
    wchar_t path[MAX_PATH];
    if (!GetModuleFileNameW(nullptr, path, MAX_PATH)) return false;
    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    sei.hwnd = owner;
    sei.lpVerb = L"runas";
    sei.lpFile = path;
    sei.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&sei)) return false;
    return true;
}

static DWORD RunProcessWait(const std::wstring& app, const std::wstring& args, bool hidden = true) {
    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    sei.lpFile = app.c_str();
    sei.lpParameters = args.empty() ? nullptr : args.c_str();
    sei.nShow = hidden ? SW_HIDE : SW_SHOWNORMAL;
    if (!ShellExecuteExW(&sei) || !sei.hProcess) return (DWORD)-1;
    WaitForSingleObject(sei.hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(sei.hProcess, &code);
    CloseHandle(sei.hProcess);
    return code;
}

static std::wstring GetEnv(const wchar_t* name) {
    DWORD len = GetEnvironmentVariableW(name, nullptr, 0);
    if (len == 0) return L"";
    std::wstring v(len, L'\0');
    GetEnvironmentVariableW(name, &v[0], len);
    if (!v.empty() && v.back() == L'\0') v.pop_back();
    return v;
}

static std::wstring GetKnownFolder(REFKNOWNFOLDERID id) {
    PWSTR p = nullptr;
    std::wstring result;
    if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &p))) {
        result = p;
        CoTaskMemFree(p);
    }
    return result;
}

static std::wstring GetDownloadsPath() {
    std::wstring dl = GetKnownFolder(FOLDERID_Downloads);
    if (!dl.empty()) return dl;
    std::wstring user = GetEnv(L"USERPROFILE");
    if (!user.empty()) {
        std::filesystem::path p = std::filesystem::path(user) / L"Downloads";
        return p.wstring();
    }
    return L"";
}

static std::wstring GetLocalTemp() {
    std::wstring temp = GetEnv(L"LOCALAPPDATA");
    if (!temp.empty()) {
        std::filesystem::path p = std::filesystem::path(temp) / L"Temp";
        return p.wstring();
    }
    temp = GetEnv(L"TEMP");
    if (!temp.empty()) return temp;
    return L"C:\\Windows\\Temp";
}

static std::filesystem::path GetBackupRoot() {
    std::filesystem::path root = std::filesystem::path(GetLocalTemp()) / L"ZenithFixerBackup";
    std::error_code ec;
    std::filesystem::create_directories(root, ec);
    return root;
}

static bool DownloadWithInvokeWebRequest(const std::wstring& url, const std::wstring& dest, HWND log = nullptr) {
    std::wstring psCmd =
        L"-NoProfile -Command \"$u='" + url + L"'; $o='" + dest +
        L"'; Invoke-WebRequest -Uri $u -OutFile $o -UseBasicParsing\"";
    if (log) AppendLog(log, L" PowerShell: " + psCmd);
    DWORD code = RunProcessWait(L"powershell.exe", psCmd, true);
    return code == 0;
}

static void CloseRobloxBrowserTabs(HWND log) {
    AppendLog(log, L"Attempting to close Roblox browser tabs/windows...");
    std::wstring psCmd =
        L"-NoProfile -WindowStyle Hidden -Command \"$p=Get-Process; "
        L"$w=$p | Where-Object { $_.MainWindowTitle -like '*Roblox*' }; "
        L"$w | ForEach-Object { $_.CloseMainWindow() | Out-Null }\"";
    RunProcessWait(L"powershell.exe", psCmd, true);
}

static bool Cleanup(HWND log) {
    AppendLog(log, L"DISM /Online /Cleanup-Image /RestoreHealth, this may take long depending on your pc...");
    DWORD code = RunProcessWait(L"powershell.exe", L"-NoProfile -Command DISM /Online /Cleanup-Image /RestoreHealth");
    AppendLog(log, L" DISM completed with exit code " + std::to_wstring(code));
    return code == 0 || code == 1;
}

static bool Synctime(HWND log) {
    AppendLog(log, L"Syncing time and date settings (w32tm /resync /rediscover)...");
    DWORD code = RunProcessWait(L"powershell.exe", L"-NoProfile -Command w32tm /resync /rediscover");
    AppendLog(log, L" w32tm completed with exit code " + std::to_wstring(code));
    return code == 0 || code == 1;
}

static bool RunSFC(HWND log) {
    AppendLog(log, L"Running system file check, this may take long depending on your pc (sfc /scannow)...");
    DWORD code = RunProcessWait(L"powershell.exe", L"-NoProfile -Command sfc /scannow");
    AppendLog(log, L" SFC completed with exit code " + std::to_wstring(code));
    return code == 0 || code == 1;
}

static bool EnableDEP(HWND log) {
    AppendLog(log, L"Enabling system DEP via Set-ProcessMitigation...");
    DWORD code = RunProcessWait(L"powershell.exe",
                                L"-NoProfile -Command Set-ProcessMitigation -System -Enable Dep");
    AppendLog(log, L" DEP command exit code " + std::to_wstring(code));
    return code == 0;
}

static bool InstallOrRepairVCRedist(HWND log) {
    AppendLog(log, L"Downloading VC++ Redistributable (x64), this may take long depending on your pc...");
    std::wstring temp = GetLocalTemp();
    std::filesystem::path dest = std::filesystem::path(temp) / L"vc_redist.x64.exe";
    if (!DownloadWithInvokeWebRequest(L"https://aka.ms/vs/17/release/vc_redist.x64.exe", dest.wstring(), log)) {
        AppendLog(log, L" Failed to download VC++ redistributable.");
        return false;
    }
    AppendLog(log, L" Running VC++ redistributable installer, this may take long depending on your pc (silent)...");
    DWORD code = RunProcessWait(dest.wstring(), L"/install /quiet /norestart", true);
    AppendLog(log, L" VC++ installer exit code " + std::to_wstring(code));
    return code == 0;
}

static void AddZenithDefenderExclusions(HWND log) {
    AppendLog(log, L"Scanning Downloads for 'zenith' folders to add Defender exclusions...");
    std::wstring downloads = GetDownloadsPath();
    if (downloads.empty()) {
        AppendLog(log, L" Unable to resolve Downloads folder.");
        return;
    }
    try {
        for (const auto& entry : std::filesystem::directory_iterator(downloads)) {
            if (!entry.is_directory()) continue;
            std::wstring name = entry.path().filename().wstring();
            if (CaseInsensitiveEquals(name, L"zenith")) {
                std::wstring path = entry.path().wstring();
                AppendLog(log, L" Adding Defender exclusion: " + path);
                std::wstring cmd = L"-NoProfile -Command Add-MpPreference -ExclusionPath '" + path + L"'";
                RunProcessWait(L"powershell.exe", cmd, true);
            }
        }
    } catch (...) {
        AppendLog(log, L" Error scanning Downloads.");
    }
}

static void BackupRobloxData(HWND log) {
    std::wstring local = GetEnv(L"LOCALAPPDATA");
    if (local.empty()) local = GetKnownFolder(FOLDERID_LocalAppData);
    if (local.empty()) {
        AppendLog(log, L" Unable to resolve LocalAppData for backup.");
        return;
    }
    std::filesystem::path roblox = std::filesystem::path(local) / L"Roblox";
    std::filesystem::path srcLS = roblox / L"LocalStorage";
    std::filesystem::path srcRS = roblox / L"rbx-storage";
    std::filesystem::path backupRoot = GetBackupRoot();
    std::filesystem::path dstLS = backupRoot / L"LocalStorage";
    std::filesystem::path dstRS = backupRoot / L"rbx-storage";
    std::error_code ec;
    if (std::filesystem::exists(srcLS)) {
        AppendLog(log, L"Backing up LocalStorage...");
        std::filesystem::create_directories(dstLS, ec);
        std::filesystem::copy(srcLS, dstLS, std::filesystem::copy_options::recursive | std::filesystem::copy_options::overwrite_existing, ec);
    }
    if (std::filesystem::exists(srcRS)) {
        AppendLog(log, L"Backing up rbx-storage...");
        std::filesystem::create_directories(dstRS, ec);
        std::filesystem::copy(srcRS, dstRS, std::filesystem::copy_options::recursive | std::filesystem::copy_options::overwrite_existing, ec);
    }
}

static void DeleteAppDataDirs(HWND log) {
    AppendLog(log, L"Deleting LocalAppData folders: Roblox, fishstrap, bloxstrap...");
    std::wstring local = GetEnv(L"LOCALAPPDATA");
    if (local.empty()) local = GetKnownFolder(FOLDERID_LocalAppData);
    if (local.empty()) {
        AppendLog(log, L" Unable to resolve LocalAppData.");
        return;
    }
    std::vector<std::wstring> targets = {
        (std::filesystem::path(local) / L"Roblox").wstring(),
        (std::filesystem::path(local) / L"fishstrap").wstring(),
        (std::filesystem::path(local) / L"bloxstrap").wstring()
    };
    for (const auto& t : targets) {
        try {
            if (std::filesystem::exists(t)) {
                AppendLog(log, L" Removing: " + t);
                std::filesystem::remove_all(t);
            } else {
                AppendLog(log, L" Not found: " + t);
            }
        } catch (...) {
            AppendLog(log, L" Failed to remove: " + t);
        }
    }
}

static void RestoreRobloxData(HWND log) {
    std::wstring local = GetEnv(L"LOCALAPPDATA");
    if (local.empty()) local = GetKnownFolder(FOLDERID_LocalAppData);
    if (local.empty()) {
        AppendLog(log, L" Unable to resolve LocalAppData for restore.");
        return;
    }
    std::filesystem::path roblox = std::filesystem::path(local) / L"Roblox";
    std::filesystem::path backupRoot = GetBackupRoot();
    std::filesystem::path srcLS = backupRoot / L"LocalStorage";
    std::filesystem::path srcRS = backupRoot / L"rbx-storage";
    std::filesystem::path dstLS = roblox / L"LocalStorage";
    std::filesystem::path dstRS = roblox / L"rbx-storage";
    std::error_code ec;
    std::filesystem::create_directories(roblox, ec);
    if (std::filesystem::exists(srcLS)) {
        AppendLog(log, L"Restoring LocalStorage...");
        std::filesystem::create_directories(dstLS, ec);
        std::filesystem::copy(srcLS, dstLS, std::filesystem::copy_options::recursive | std::filesystem::copy_options::overwrite_existing, ec);
    }
    if (std::filesystem::exists(srcRS)) {
        AppendLog(log, L"Restoring rbx-storage...");
        std::filesystem::create_directories(dstRS, ec);
        std::filesystem::copy(srcRS, dstRS, std::filesystem::copy_options::recursive | std::filesystem::copy_options::overwrite_existing, ec);
    }
}

static bool ShellExecuteUnelevated(const std::wstring& app, const std::wstring& params) {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    CComPtr<IShellWindows> spShellWindows;
    if (SUCCEEDED(hr)) hr = spShellWindows.CoCreateInstance(CLSID_ShellWindows);
    if (FAILED(hr)) { CoUninitialize(); return false; }
    long hwndLoc = 0;
    CComVariant vEmpty;
    CComPtr<IDispatch> spDisp;
    hr = spShellWindows->FindWindowSW(&vEmpty, &vEmpty, SWC_DESKTOP, &hwndLoc, SWFO_NEEDDISPATCH, &spDisp);
    if (FAILED(hr) || !spDisp) { CoUninitialize(); return false; }
    CComPtr<IShellDispatch2> spShellDisp;
    hr = spDisp->QueryInterface(IID_PPV_ARGS(&spShellDisp));
    if (FAILED(hr) || !spShellDisp) { CoUninitialize(); return false; }
    CComBSTR bFile(app.c_str());
    CComVariant vArgs(params.empty() ? L"" : params.c_str());
    CComVariant vDir(L"");
    CComVariant vOp(L"open");
    CComVariant vShow(SW_SHOWNORMAL);
    hr = spShellDisp->ShellExecute(bFile, vArgs, vDir, vOp, vShow);
    CoUninitialize();
    return SUCCEEDED(hr);
}

static DWORD RunProcessRunAsInvoker(const std::wstring& app, const std::wstring& args) {
    SetEnvironmentVariableW(L"__COMPAT_LAYER", L"RunAsInvoker");
    std::wstring cmd = L"\"" + app + L"\"" + (args.empty() ? L"" : L" " + args);
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    DWORD code = (DWORD)-1;
    if (CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
        WaitForSingleObject(pi.hProcess, INFINITE);
        GetExitCodeProcess(pi.hProcess, &code);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
    SetEnvironmentVariableW(L"__COMPAT_LAYER", nullptr);
    return code;
}

static bool InstallRobloxToLocalAppData(HWND log) {
    AppendLog(log, L"Downloading Roblox installer to LocalAppData, this may take long depending on your pc...");
    std::filesystem::path dest = std::filesystem::path(GetLocalTemp()) / L"RobloxPlayerInstaller.exe";
    if (!DownloadWithInvokeWebRequest(L"https://www.roblox.com/download/client?os=win", dest.wstring(), log)) {
        AppendLog(log, L" Failed to download Roblox installer.");
        return false;
    }
    AppendLog(log, L"Launching Roblox installer unelevated (per-user)...");
    bool launched = ShellExecuteUnelevated(dest.wstring(), L"");
    DWORD exitCode = 0;
    if (!launched) {
        AppendLog(log, L" Shell launch failed; retrying with RunAsInvoker...");
        exitCode = RunProcessRunAsInvoker(dest.wstring(), L"");
        launched = (exitCode != (DWORD)-1);
    }
    if (!launched) {
        AppendLog(log, L" Could not start the installer. Please run it manually from LocalAppData\\Temp.");
        return false;
    }
    AppendLog(log, L"Roblox installer started. Complete the UI to finish the per-user install.");
    return true;
}

static void OpenRobloxDownloadPage(HWND log) {
    AppendLog(log, L"Opening Roblox client download page in default browser...");
    ShellExecuteW(nullptr, L"open", L"https://www.roblox.com/download/client?os=win", nullptr, nullptr, SW_SHOWNORMAL);
}

static std::wstring SnapshotDownloads(const std::wstring& downloads) {
    std::wstring list;
    for (const auto& e : std::filesystem::directory_iterator(downloads)) {
        if (!e.is_regular_file()) continue;
        list += e.path().filename().wstring();
        list += L"\n";
    }
    return list;
}

static bool SnapshotContains(const std::wstring& snapshot, const std::wstring& filename) {
    return snapshot.find(filename + L"\n") != std::wstring::npos;
}

static std::wstring WaitForRobloxExeInDownloads(HWND log, int maxSeconds = 300) {
    std::wstring downloads = GetDownloadsPath();
    if (downloads.empty()) {
        AppendLog(log, L" Downloads folder not resolved; cannot watch for installer.");
        return L"";
    }
    AppendLog(log, L"Waiting up to " + std::to_wstring(maxSeconds) + L"s for a new 'roblox*.exe' in Downloads...");
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(maxSeconds);
    std::wstring initial = SnapshotDownloads(downloads);
    while (std::chrono::steady_clock::now() < deadline) {
        try {
            for (const auto& e : std::filesystem::directory_iterator(downloads)) {
                if (!e.is_regular_file()) continue;
                std::wstring fname = e.path().filename().wstring();
                if (ContainsCaseInsensitive(fname, L".crdownload") ||
                    ContainsCaseInsensitive(fname, L".download") ||
                    ContainsCaseInsensitive(fname, L".part")) {
                    continue;
                }
                if (!ContainsCaseInsensitive(fname, L"roblox")) continue;
                if (!ContainsCaseInsensitive(fname, L".exe")) continue;
                if (!SnapshotContains(initial, fname)) {
                    AppendLog(log, L" Found new installer: " + fname);
                    return e.path().wstring();
                }
            }
        } catch (...) {
            AppendLog(log, L" Error scanning Downloads.");
        }
        std::this_thread::sleep_for(std::chrono::seconds(2));
    }
    AppendLog(log, L" Timed out waiting for Roblox installer in Downloads.");
    return L"";
}

static void RunRobloxInstaller(HWND log, const std::wstring& path) {
    AppendLog(log, L"Launching Roblox installer: " + path);
    DWORD code = RunProcessWait(path, L"", false);
    AppendLog(log, L" Roblox installer exit code " + std::to_wstring(code));
}

static void AttemptCloseRobloxTab(HWND log) {
    CloseRobloxBrowserTabs(log);
}

static void MoveRobloxVersionsToLocalAppData(HWND log) {
    std::filesystem::path src = L"C:\\Program Files (x86)\\Roblox\\Versions";
    if (!std::filesystem::exists(src)) {
        AppendLog(log, L"Source Versions folder not found in Program Files (x86).");
        return;
    }
    std::wstring local = GetEnv(L"LOCALAPPDATA");
    if (local.empty()) local = GetKnownFolder(FOLDERID_LocalAppData);
    if (local.empty()) {
        AppendLog(log, L"Unable to resolve LocalAppData for move.");
        return;
    }
    std::filesystem::path dstRoot = std::filesystem::path(local) / L"Roblox";
    std::filesystem::path dst = dstRoot / L"Versions";
    std::error_code ec;
    std::filesystem::create_directories(dstRoot, ec);
    std::filesystem::create_directories(dst, ec);
    AppendLog(log, L"Moving Versions to LocalAppData\\Roblox...");
    std::filesystem::copy(src, dst, std::filesystem::copy_options::recursive | std::filesystem::copy_options::overwrite_existing, ec);
    if (ec) {
        AppendLog(log, L"Copy failed: " + std::wstring(ec.message().begin(), ec.message().end()));
        return;
    }
    std::filesystem::remove_all(src, ec);
    if (ec) {
        AppendLog(log, L"Delete source failed: " + std::wstring(ec.message().begin(), ec.message().end()));
        return;
    }
    AppendLog(log, L"Move complete.");
}

struct AppState {
    HWND hButton{};
    HWND hEdit{};
    HWND hProgress{};
    bool running{false};
};

static void DoFixWorkflow(HWND hwnd, AppState* state) {
    if (!IsProcessElevated()) {
        int r = MessageBoxW(hwnd,
            L"This will make system-level changes.\n\nAdministrator rights are required. Continue?",
            L"Elevation required", MB_ICONWARNING | MB_OKCANCEL);
        if (r != IDOK) return;
        if (RelaunchElevated(hwnd)) {
            PostQuitMessage(0);
            return;
        } else {
            MessageBoxW(hwnd, L"Failed to relaunch elevated.", L"Error", MB_ICONERROR);
            return;
        }
    }
    int confirm = MessageBoxW(hwnd,
        L"This will:\n"
        L" - Add Windows Defender exclusions for 'zenith' folders in Downloads (folders only)\n"
        L" - Delete LocalAppData folders: Roblox, fishstrap, bloxstrap\n\nProceed?",
        L"Confirm actions", MB_ICONWARNING | MB_OKCANCEL);
    if (confirm != IDOK) return;
    state->running = true;
    EnableWindow(state->hButton, FALSE);

    PostMessageW(hwnd, WM_APP_PROGRESS, (WPARAM)0, 0);

    std::thread([hwnd, state]() {
        HWND log = state->hEdit;

        PostLogAndProgress(hwnd, log, L"Running DISM...", 5);
        Cleanup(log);

        PostLogAndProgress(hwnd, log, L"Syncing date and time...", 10);
        Synctime(log);

        PostLogAndProgress(hwnd, log, L"Starting SFC...", 5);
        RunSFC(log);

        PostLogAndProgress(hwnd, log, L"Installing/repairing VC++ redistributable...", 20);
        bool vcOk = InstallOrRepairVCRedist(log);


        PostLogAndProgress(hwnd, log, L"Prompting about DNS change...", 30);
        int dnsChoice = MessageBoxW(hwnd,
            L"Changing your DNS to 1.1.1.1.\n\nIf you are on a school or work computer/laptop please decline this DNS change as it may get you in trouble from your school or work company.",
            L"Change DNS to 1.1.1.1?", MB_ICONWARNING | MB_YESNO);
        if (dnsChoice == IDYES) {
            AppendLog(log, L"User accepted DNS change. Attempting to set DNS to 1.1.1.1 for active adapters...");

            std::wstring psCmd =
                L"-NoProfile -Command \"Get-NetAdapter -Physical | Where-Object {$_.Status -eq 'Up'} | ForEach-Object { Set-DnsClientServerAddress -InterfaceIndex $_.ifIndex -ServerAddresses '1.1.1.1' -ErrorAction SilentlyContinue }\"";
            DWORD dnsCode = RunProcessWait(L"powershell.exe", psCmd, true);
            AppendLog(log, L" DNS change command exit code " + std::to_wstring(dnsCode));
        } else {
            AppendLog(log, L"User declined DNS change. Skipping DNS modification.");
        }

        PostLogAndProgress(hwnd, log, L"Enabling DEP...", 35);
        EnableDEP(log);

        PostLogAndProgress(hwnd, log, L"Adding Defender exclusions for zenith...", 50);
        AddZenithDefenderExclusions(log);

        PostLogAndProgress(hwnd, log, L"Backing up Roblox data...", 60);
        BackupRobloxData(log);

        PostLogAndProgress(hwnd, log, L"Deleting LocalAppData Roblox/fishstrap/bloxstrap...", 70);
        DeleteAppDataDirs(log);

        PostLogAndProgress(hwnd, log, L"Attempting per-user Roblox install...", 80);
        bool robloxStarted = InstallRobloxToLocalAppData(log);

        PostLogAndProgress(hwnd, log, L"Moving Versions to LocalAppData...", 85);
        MoveRobloxVersionsToLocalAppData(log);

        PostLogAndProgress(hwnd, log, L"Restoring Roblox data...", 92);
        RestoreRobloxData(log);

        if (!robloxStarted) {
            AppendLog(log, L"Roblox installer not started automatically. You can run it manually from LocalAppData\\Temp.");
        }

        PostLogAndProgress(hwnd, log, L"All steps complete. Please restart your PC.", 100);
        MessageBoxW(hwnd, L"Fix completed.\n\nPlease restart your PC.", L"Done", MB_ICONINFORMATION);

        EnableWindow(state->hButton, TRUE);
        state->running = false;
    }).detach();
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    static AppState* state = nullptr;
    switch (msg) {
        case WM_CREATE: {
            state = new AppState();
            HFONT hFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);

            state->hButton = CreateWindowW(L"BUTTON", L"Fix",
                                           WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
                                           20, 20, 100, 32, hwnd, (HMENU)1001, GetModuleHandleW(nullptr), nullptr);

            state->hEdit = CreateWindowW(L"EDIT", L"",
                                         WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY,
                                         20, 110, 560, 280, hwnd, (HMENU)1002, GetModuleHandleW(nullptr), nullptr);


            state->hProgress = CreateWindowExW(0, PROGRESS_CLASS, nullptr,
                                               WS_CHILD | WS_VISIBLE,
                                               20, 60, 560, 20, hwnd, (HMENU)1003, GetModuleHandleW(nullptr), nullptr);

            SendMessageW(state->hButton, WM_SETFONT, (WPARAM)hFont, TRUE);
            SendMessageW(state->hEdit, WM_SETFONT, (WPARAM)hFont, TRUE);

            SendMessageW(state->hProgress, PBM_SETRANGE32, 0, 100);
            SendMessageW(state->hProgress, PBM_SETPOS, 0, 0);

            AppendLog(state->hEdit, L"Ready. Click Fix to start.");
            break;
        }
        case WM_COMMAND: {
            if (LOWORD(wParam) == 1001) {
                if (state && !state->running) {
                    DoFixWorkflow(hwnd, state);
                }
            }
            break;
        }
        case WM_APP_PROGRESS: {

            int p = (int)wParam;
            SendMessageW(state->hProgress, PBM_SETPOS, p, 0);
            break;
        }
        case WM_DESTROY: {
            delete state;
            PostQuitMessage(0);
            break;
        }
        default:
            return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
    return 0;
}

int APIENTRY wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR, int nCmdShow) {

    INITCOMMONCONTROLSEX iccex{};
    iccex.dwSize = sizeof(iccex);
    iccex.dwICC = ICC_PROGRESS_CLASS;
    InitCommonControlsEx(&iccex);

    const wchar_t* CLASS_NAME = L"ZenithFixerWinClass";
    WNDCLASSW wc{};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = CLASS_NAME;
    if (!RegisterClassW(&wc)) return 0;
    HWND hwnd = CreateWindowExW(0, CLASS_NAME, L"Zenith Fixer",
                                WS_OVERLAPPED | WS_CAPTION | WS_MINIMIZEBOX | WS_SYSMENU,
                                CW_USEDEFAULT, CW_USEDEFAULT, 610, 450,
                                nullptr, nullptr, hInst, nullptr);
    if (!hwnd) return 0;
    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);
    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return (int)msg.wParam;
}
