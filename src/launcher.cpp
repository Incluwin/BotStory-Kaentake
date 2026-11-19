#include "debug.h"
#include "paths.h"
#include "constants.h"
#include "patcher.h"
#include "featurespage.h"
#include <windows.h>
#include <detours.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>


// Read the optional [config] resolution= key from config.ini and, if present
// and valid, propagate it to the injected DLL via KAENTAKE_RESOLUTION (which
// MapleStory.exe inherits through CreateProcess's default environment).
// Accepted values:
//   0..4           -- direct index matching CConfig::soScreenResolution
//   800x600        -- index 0
//   1024x768       -- index 1
//   1366x768       -- index 2
//   1600x900       -- index 3
//   1920x1080      -- index 4
// Unset values are ignored; an invalid one is ignored with a line in
// kaentake_diag.txt. resolution.cpp then falls back to the user's saved
// in-game preference (GLOBAL_OPT/soScreenResolution).
static void PropagateResolutionFromConfig() {
    char buf[32];
    DWORD n = GetPrivateProfileStringA(
        "config", "resolution", "",
        buf, sizeof(buf), kpaths::ConfigPath());
    if (n == 0) return;

    int resIdx = -1;
    if      (strcmp(buf, "800x600")   == 0) resIdx = 0;
    else if (strcmp(buf, "1024x768")  == 0) resIdx = 1;
    else if (strcmp(buf, "1366x768")  == 0) resIdx = 2;
    else if (strcmp(buf, "1600x900")  == 0) resIdx = 3;
    else if (strcmp(buf, "1920x1080") == 0) resIdx = 4;
    else if (n == 1 && buf[0] >= '0' && buf[0] <= '4') resIdx = buf[0] - '0';

    if (resIdx >= 0 && resIdx <= 4) {
        char sVal[2] = { static_cast<char>('0' + resIdx), '\0' };
        SetEnvironmentVariableA("KAENTAKE_RESOLUTION", sVal);
    } else {
        LogMessage("[launcher] config.ini [config] resolution=\"%s\" is not 800x600, 1024x768, 1366x768, 1600x900, "
                   "1920x1080 or 0-4; using the resolution chosen in the game instead", buf);
    }
}


// Propagate the integer-valued [config] sysopt keys (bgm_volume, etc.) that
// client_prefs.cpp knows how to override. Mirror of the table in
// client_prefs.cpp — keep in sync. Absent values are skipped; an invalid one
// is skipped or clamped with a line in kaentake_diag.txt. The DLL's
// ApplyPrefsAfterLoadGlobal only touches opt keys when the env var parsed.
static void PropagateIntFromConfig(const char* sKey, const char* sEnvName,
                                   int nLow, int nHigh) {
    char buf[16];
    DWORD n = GetPrivateProfileStringA(
        "config", sKey, "", buf, sizeof(buf), kpaths::ConfigPath());
    if (n == 0) return;
    char* endp = nullptr;
    long val = strtol(buf, &endp, 10);
    if (endp == buf) {
        LogMessage("[launcher] config.ini [config] %s=\"%s\" is not a number; using the in-game setting instead",
                   sKey, buf);
        return;
    }
    while (*endp == ' ' || *endp == '\t') ++endp;
    if (*endp != '\0') {
        LogMessage("[launcher] config.ini [config] %s=\"%s\" has extra characters after the number; using %ld",
                   sKey, buf, val);
    }
    long clamped = val < nLow ? nLow : val > nHigh ? nHigh : val;
    if (clamped != val) {
        LogMessage("[launcher] config.ini [config] %s=%ld is outside %d-%d; using %ld", sKey, val, nLow, nHigh, clamped);
        val = clamped;
    }
    char sVal[16];
    sprintf_s(sVal, sizeof(sVal), "%ld", val);
    SetEnvironmentVariableA(sEnvName, sVal);
}

static void PropagateSysOptsFromConfig() {
    PropagateIntFromConfig("bgm_volume", "KAENTAKE_BGM_VOLUME", 0, 100);
    PropagateIntFromConfig("bgm_mute",   "KAENTAKE_BGM_MUTE",   0,   1);
    PropagateIntFromConfig("sfx_volume", "KAENTAKE_SFX_VOLUME", 0, 100);
    PropagateIntFromConfig("sfx_mute",   "KAENTAKE_SFX_MUTE",   0,   1);
    PropagateIntFromConfig("fullscreen", "KAENTAKE_FULLSCREEN", 0,   1);
    PropagateIntFromConfig("hp_alert",   "KAENTAKE_HP_ALERT",   0, 100);
    PropagateIntFromConfig("mp_alert",   "KAENTAKE_MP_ALERT",   0, 100);
}


// Removes a "--features" or "/features" argument; true if there was one.
static bool TakeFeaturesFlag(std::string& sArgs) {
    for (const char* sFlag : { "--features", "/features" }) {
        size_t n = strlen(sFlag);
        for (size_t i = sArgs.find(sFlag); i != std::string::npos; i = sArgs.find(sFlag, i + 1)) {
            bool bStart = i == 0 || sArgs[i - 1] == ' ';
            bool bEnd = i + n == sArgs.size() || sArgs[i + n] == ' ';
            if (bStart && bEnd) {
                sArgs.erase(i, n);
                size_t first = sArgs.find_first_not_of(' ');
                size_t last = sArgs.find_last_not_of(' ');
                sArgs = first == std::string::npos ? std::string() : sArgs.substr(first, last - first + 1);
                return true;
            }
        }
    }
    return false;
}


// config.ini, kaentake_diag.txt and the game's own files all live beside Launch Game.exe (paths.h), and the game is
// started as MapleStory.exe in the working folder. A shortcut whose "Start in" folder is somewhere else used to make
// the patcher and the game start look there; when MapleStory.exe sits beside the launcher, that folder is used.
static void UseLauncherFolder() {
    char sCwd[MAX_PATH] = {};
    GetCurrentDirectoryA(MAX_PATH, sCwd);
    std::string sDir = kpaths::ExeDir();
    std::string sCur = sCwd;
    if (!sCur.empty() && sCur.back() != '\\') sCur.push_back('\\');
    if (_stricmp(sCur.c_str(), sDir.c_str()) == 0) return;
    if (GetFileAttributesA((sDir + "MapleStory.exe").c_str()) == INVALID_FILE_ATTRIBUTES) {
        LogMessage("[launcher] started in %s, but MapleStory.exe is not beside Launch Game.exe in %s; "
                   "staying in the start folder", sCwd, sDir.c_str());
        return;
    }
    LogMessage("[launcher] started in %s; using the game folder %s instead", sCwd, sDir.c_str());
    SetCurrentDirectoryA(sDir.c_str());
}


int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nShowCmd) {
    UseLauncherFolder();
    // The Features page opens first unless it was switched off (page_at_launch=0); "Launch Game.exe --features", or
    // Shift held while it starts, opens it anyway. The flag is not passed on to the game.
    std::string sArgs = lpCmdLine ? lpCmdLine : "";
    bool bFeatures = TakeFeaturesFlag(sArgs);
    if ((bFeatures || (GetAsyncKeyState(VK_SHIFT) & 0x8000) || FeaturesPageAtLaunch()) && !FeaturesPageRun()) {
        return 0;
    }

    if (!PatcherRun()) {
        return 1;
    }

    PropagateResolutionFromConfig();
    PropagateSysOptsFromConfig();

    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof(si));
    ZeroMemory(&pi, sizeof(pi));
    si.cb = sizeof(STARTUPINFOA);

    if (!DetourCreateProcessWithDllExA("MapleStory.exe", sArgs.data(), NULL, NULL, FALSE, CREATE_SUSPENDED, NULL, NULL, &si, &pi, "kaentake.dll", NULL)) {
        DWORD dwError = GetLastError();
        LPSTR sErrorMessage = nullptr;
        FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, dwError, 0, (LPSTR)&sErrorMessage, 0, nullptr);
        LogMessage("[launcher] CreateProcess MapleStory.exe failed [%lu]", dwError);
        ErrorMessage("The game (MapleStory.exe) could not be started.\n\n%s",
                     sErrorMessage ? sErrorMessage : "Windows did not say why.");
        LocalFree(sErrorMessage);
        return 1;
    }
    ResumeThread(pi.hThread);
    WaitForSingleObject(pi.hProcess, INFINITE);

    DWORD dwExitCode;
    if (!GetExitCodeProcess(pi.hProcess, &dwExitCode)) {
        LogMessage("[launcher] GetExitCodeProcess failed [%lu]", GetLastError()); // the game ran; nothing to tell the player
        return 1;
    }
    return 0;
}