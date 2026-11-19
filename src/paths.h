#pragma once
// Where config.ini and kaentake_diag.txt live: beside the running exe (MapleStory.exe in the DLL, Launch Game.exe in
// the launcher, which ships in the same folder), never the working directory, which a shortcut can point anywhere.
// Header-only so the DLL and the launcher share it. Plain char buffers, so no static destructor runs at exit
// (see dllentry.cpp).
#include <windows.h>
#include <cstring>

#define KAENTAKE_LOG_NAME "kaentake_diag.txt"

namespace kpaths {

namespace detail {
inline void Build(char* sOut, const char* sFile) {
    char path[MAX_PATH] = {};
    DWORD n = GetModuleFileNameA(nullptr, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) {
        strcpy_s(path, ".\\");
        n = 2;
    }
    while (n > 0 && path[n - 1] != '\\' && path[n - 1] != '/') {
        --n;
    }
    path[n] = '\0';
    if (n == 0) {
        strcpy_s(path, ".\\");
    }
    strcpy_s(sOut, MAX_PATH, path);
    if (sFile) {
        strcat_s(sOut, MAX_PATH, sFile);
    }
}
}  // namespace detail

// The exe's folder, with a trailing backslash.
inline const char* ExeDir() {
    static char s[MAX_PATH];
    if (!s[0]) detail::Build(s, nullptr);
    return s;
}

inline const char* ConfigPath() {
    static char s[MAX_PATH];
    if (!s[0]) detail::Build(s, "config.ini");
    return s;
}

inline const char* LogPath() {
    static char s[MAX_PATH];
    if (!s[0]) detail::Build(s, KAENTAKE_LOG_NAME);
    return s;
}

}  // namespace kpaths
