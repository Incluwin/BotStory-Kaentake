#include "pch.h"
#include "paths.h"
#include "debug.h"
#include <windows.h>
#include <strsafe.h>
#include <cstdio>


void DebugMessage(const char* pszFormat, ...) {
    char pszDest[1024];
    size_t cbDest = 1024 * sizeof(char);
    va_list argList;
    va_start(argList, pszFormat);
    StringCbVPrintfA(pszDest, cbDest, pszFormat, argList);
    OutputDebugStringA(pszDest);
    va_end(argList);
}

void AppendLogFileHint(char* sDest, size_t cbDest) {
    StringCbCatA(sDest, cbDest, "\n\nDetails are saved in " KAENTAKE_LOG_NAME " in this folder:\n");
    StringCbCatA(sDest, cbDest, kpaths::ExeDir());
}

void ErrorMessage(const char* pszFormat, ...) {
    char pszDest[1024];
    size_t cbDest = 1024 * sizeof(char);
    va_list argList;
    va_start(argList, pszFormat);
    StringCbVPrintfA(pszDest, cbDest, pszFormat, argList);
    va_end(argList);
    LogMessage("[error] %s", pszDest);
    char pszShown[1536];
    StringCbCopyA(pszShown, sizeof(pszShown), pszDest);
    AppendLogFileHint(pszShown, sizeof(pszShown));
    MessageBoxA(nullptr, pszShown, "BotStory", MB_ICONERROR);
}

void LogMessage(const char* pszFormat, ...) {
    char pszDest[1024];
    size_t cbDest = 1024 * sizeof(char);
    va_list argList;
    va_start(argList, pszFormat);
    StringCbVPrintfA(pszDest, cbDest, pszFormat, argList);
    va_end(argList);
    OutputDebugStringA(pszDest);
    FILE* fp = nullptr;
    if (fopen_s(&fp, kpaths::LogPath(), "a") == 0 && fp) {
        fprintf(fp, "%s\n", pszDest);
        fclose(fp);
    }
}
