// Module registry: attaches feature_list.h's modules in list order, records every hook and patch under the module
// that made it, reports two modules changing the same bytes, and switches LIVE modules inert and back at runtime.
//
// config.ini [features] (next to MapleStory.exe):
//   <id>=1|0    one line per module; a missing line = the module's default (feature_list.h)
//   log=1       write kaentake_features.txt: the attach order and every hook and patch, rewritten each start
//               (also when the environment has KAENTAKE_FEATURE_LOG=1)
//   inert=a,b   attach these LIVE modules but start them inert, as a server veto at login would leave them
#include "pch.h"
#include "features.h"
#include "hook.h"
#include "debug.h"
#include "constants.h"
#include "v83_version_guard.h"

#include <windows.h>
#include <strsafe.h>
#include <cstdarg>
#include <cstdio>
#include <share.h>
#include <string>
#include <vector>


namespace {

struct Feature {
    const char* id;
    const char* title;
    bool bDefault;
    int nFlags;
    FeatureId eRequires;
    void (*pAttach)();
};

const Feature kFeatures[] = {
#define FEATURE_ROW(id, title, def, flags, req, attach) { #id, title, (def) != 0, (flags), FeatureId::req, &attach },
    KAENTAKE_FEATURES(FEATURE_ROW)
#undef FEATURE_ROW
};

constexpr size_t kCount = static_cast<size_t>(FeatureId::count);
static_assert(sizeof(kFeatures) / sizeof(kFeatures[0]) == kCount, "one row per FeatureId");

constexpr bool SameText(const char* a, const char* b) {
    return *a == *b && (*a == '\0' || SameText(a + 1, b + 1));
}

// Build-time checks on the list (a duplicate id already fails as a duplicate enumerator).
#define FEATURE_CHECK(id, title, def, flags, req, attach) \
    static_assert(FeatureId::req < FeatureId::id, #id ": the module it requires must come earlier in the list"); \
    static_assert(!((flags) & FEATURE_CORE) || (def), #id ": a core module is always on"); \
    static_assert(!((flags) & FEATURE_CORE) || !((flags) & (FEATURE_LIVE | FEATURE_SERVER_GATED)), \
                  #id ": a core module has no switch"); \
    static_assert(!SameText(#id, "log") && !SameText(#id, "inert"), #id ": reserved [features] key");
KAENTAKE_FEATURES(FEATURE_CHECK)
#undef FEATURE_CHECK


struct State {
    bool bAttached = false;
    bool bActive = true; // read by the module's hook thunks (plain bool keeps g_aState constant-initialized)
    bool bSharesBytes = false;
};
State g_aState[kCount];

// One hook or patch, in the order they were made.
struct Record {
    int nModule;        // kFeatures index; -1 outside any module (DllMain's system hooks)
    bool bHook;
    uintptr_t uAddress; // hook: the address asked for; patch: the first byte written
    uintptr_t uStart;   // the first byte written (a hook's real target)
    size_t uSize;
    std::vector<unsigned char> abOriginal; // patches: the bytes before and the bytes written
    std::vector<unsigned char> abPatched;
};

std::vector<Record>& Records() {
    static auto& v = *new std::vector<Record>(); // never destroyed: nothing to free at DLL detach
    return v;
}

int g_nCurrent = -1; // the module being attached
FILE* g_pLog = nullptr;


std::string GameDir() {
    char path[MAX_PATH] = {};
    DWORD n = GetModuleFileNameA(nullptr, path, MAX_PATH);
    std::string s(path, n);
    size_t slash = s.find_last_of("\\/");
    return slash == std::string::npos ? std::string(".") : s.substr(0, slash);
}

// config.ini next to MapleStory.exe (the working directory is not always the game folder)
std::string ConfigPath() {
    return GameDir() + "\\" CONSTANTS_CONFIG_NAME;
}

void Log(const char* sFormat, ...) {
    if (!g_pLog) {
        return;
    }
    va_list ap;
    va_start(ap, sFormat);
    vfprintf(g_pLog, sFormat, ap);
    va_end(ap);
    fputc('\n', g_pLog);
    fflush(g_pLog);
}

// kaentake_diag.txt (appended, like the other modules' lines), the features log and the debugger.
void Diag(const char* sFormat, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, sFormat);
    StringCbVPrintfA(buf, sizeof(buf), sFormat, ap);
    va_end(ap);
    FILE* fp = nullptr;
    if (fopen_s(&fp, "kaentake_diag.txt", "a") == 0 && fp) {
        fprintf(fp, "[features] %s\n", buf);
        fclose(fp);
    }
    Log("%s", buf);
    DEBUG_MESSAGE("[features] %s", buf);
}

const char* ModuleName(int nModule) {
    return nModule < 0 ? "-" : kFeatures[nModule].id;
}

void LogRecord(const Record& r) {
    if (!g_pLog) {
        return;
    }
    if (r.bHook) {
        Log("%s hook 0x%08X %u at 0x%08X", ModuleName(r.nModule), r.uAddress, static_cast<unsigned>(r.uSize), r.uStart);
        return;
    }
    char hex[80] = {};
    for (size_t i = 0; i < r.abPatched.size() && i < 32; ++i) {
        StringCbPrintfA(hex + i * 2, sizeof(hex) - i * 2, "%02X", r.abPatched[i]);
    }
    Log("%s patch 0x%08X %u %s%s", ModuleName(r.nModule), r.uAddress, static_cast<unsigned>(r.uSize), hex,
        r.abPatched.size() > 32 ? ".." : "");
}

void OpenLog() {
    std::string path = GameDir() + "\\kaentake_features.txt";
    g_pLog = _fsopen(path.c_str(), "w", _SH_DENYNO); // readable while the game runs
    if (!g_pLog) {
        return;
    }
    Log("# Kaentake modules: attach order, then each hook and patch under the module that made it");
    for (size_t i = 0; i < kCount; ++i) {
        if ((kFeatures[i].nFlags & FEATURE_EARLY) && g_aState[i].bAttached) {
            Log("attach %s", kFeatures[i].id);
        }
    }
    for (const auto& r : Records()) {
        LogRecord(r);
    }
}

void WriteBytes(uintptr_t uAddress, const std::vector<unsigned char>& ab) {
    DWORD flOldProtect;
    VirtualProtect(reinterpret_cast<void*>(uAddress), ab.size(), PAGE_EXECUTE_READWRITE, &flOldProtect);
    memcpy(reinterpret_cast<void*>(uAddress), ab.data(), ab.size());
    VirtualProtect(reinterpret_cast<void*>(uAddress), ab.size(), flOldProtect, &flOldProtect);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(uAddress), ab.size());
}

void AttachOne(size_t i) {
    int nOuter = g_nCurrent;
    g_nCurrent = static_cast<int>(i);
    Log("attach %s", kFeatures[i].id);
    kFeatures[i].pAttach();
    g_nCurrent = nOuter;
    g_aState[i].bAttached = true;
}

// Whether config.ini and the module's requirement let a module attach.
bool MayAttach(size_t i, const std::string& config) {
    const Feature& f = kFeatures[i];
    if (!(f.nFlags & FEATURE_CORE) && !ReadFeatureSwitch(config.c_str(), f.id, f.bDefault)) {
        if (f.bDefault) {
            Diag("%s off (config.ini)", f.id);
        } else {
            Log("skip %s (off by default)", f.id);
        }
        return false;
    }
    if (f.eRequires != FeatureId::none && !g_aState[static_cast<size_t>(f.eRequires)].bAttached) {
        Diag("%s off: it needs %s", f.id, kFeatures[static_cast<size_t>(f.eRequires)].id);
        return false;
    }
    return true;
}

// Reports every pair of records from different modules that change the same bytes. Only records from uFrom on are
// checked against the rest, so a deferred attach checks just what it added.
void CheckSharedBytes(size_t uFrom) {
    const auto& v = Records();
    int nReported = 0;
    for (size_t i = uFrom; i < v.size(); ++i) {
        const Record& b = v[i];
        if (b.nModule < 0) {
            continue;
        }
        for (size_t j = 0; j < i; ++j) {
            const Record& a = v[j];
            if (a.nModule < 0 || a.nModule == b.nModule
                    || a.uStart >= b.uStart + b.uSize || b.uStart >= a.uStart + a.uSize) {
                continue;
            }
            g_aState[a.nModule].bSharesBytes = true;
            g_aState[b.nModule].bSharesBytes = true;
            if (++nReported <= 20) {
                Diag("%s and %s change the same bytes: %s 0x%08X (%u bytes), %s 0x%08X (%u bytes)",
                     ModuleName(a.nModule), ModuleName(b.nModule),
                     a.bHook ? "hook" : "patch", a.uStart, static_cast<unsigned>(a.uSize),
                     b.bHook ? "hook" : "patch", b.uStart, static_cast<unsigned>(b.uSize));
            }
        }
    }
    if (nReported > 20) {
        Diag("... %d more shared-byte pairs", nReported - 20);
    }
#ifdef _DEBUG
    if (nReported) {
        ErrorMessage("%d hook/patch pairs from different modules change the same bytes; see kaentake_diag.txt.", nReported);
    }
#endif
}

void ApplyInertList(const std::string& config) {
    char buf[512];
    if (GetPrivateProfileStringA("features", "inert", "", buf, sizeof(buf), config.c_str()) == 0) {
        return;
    }
    char* pContext = nullptr;
    for (char* sId = strtok_s(buf, ", \t", &pContext); sId; sId = strtok_s(nullptr, ", \t", &pContext)) {
        FeatureId id = features::Find(sId);
        if (id == FeatureId::none) {
            Diag("inert=%s: no such module", sId);
        } else if (features::SetActive(id, false)) {
            Diag("%s attached inert (config.ini inert=)", sId);
        }
    }
}

unsigned char* AllocThunk(size_t uSize) {
    static unsigned char* pPage = nullptr;
    static size_t uUsed = 0;
    constexpr size_t kPage = 4096;
    if (!pPage || uUsed + uSize > kPage) {
        pPage = static_cast<unsigned char*>(VirtualAlloc(nullptr, kPage, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
        uUsed = 0;
        if (!pPage) {
            return nullptr;
        }
    }
    unsigned char* p = pPage + uUsed;
    uUsed += uSize;
    return p;
}

}  // namespace


namespace features {

void AttachEarly() {
    for (size_t i = 0; i < kCount; ++i) {
        if (kFeatures[i].nFlags & FEATURE_EARLY) {
            AttachOne(i);
        }
    }
}

void AttachClient() {
    std::string config = ConfigPath();
    char sEnv[4];
    if (ReadFeatureSwitch(config.c_str(), "log", false)
            || (GetEnvironmentVariableA("KAENTAKE_FEATURE_LOG", sEnv, sizeof(sEnv)) == 1 && sEnv[0] == '1')) {
        OpenLog();
    }
    for (size_t i = 0; i < kCount; ++i) {
        if (!(kFeatures[i].nFlags & (FEATURE_EARLY | FEATURE_DEFERRED)) && MayAttach(i, config)) {
            AttachOne(i);
        }
    }
    CheckSharedBytes(0);
    ApplyInertList(config);
}

void AttachDeferred(FeatureId id) {
    size_t i = static_cast<size_t>(id);
    if (i >= kCount || g_aState[i].bAttached || !MayAttach(i, ConfigPath())) {
        return;
    }
    size_t uFrom = Records().size();
    AttachOne(i);
    CheckSharedBytes(uFrom);
}

bool IsAttached(FeatureId id) {
    size_t i = static_cast<size_t>(id);
    return i < kCount && g_aState[i].bAttached;
}

bool IsActive(FeatureId id) {
    size_t i = static_cast<size_t>(id);
    return i < kCount && g_aState[i].bAttached && g_aState[i].bActive;
}

bool SetActive(FeatureId id, bool bActive) {
    size_t i = static_cast<size_t>(id);
    if (i >= kCount || !g_aState[i].bAttached) {
        return false;
    }
    const Feature& f = kFeatures[i];
    State& s = g_aState[i];
    if (s.bActive == bActive) {
        return true;
    }
    if (!(f.nFlags & FEATURE_LIVE)) {
        Diag("%s cannot be switched while the game runs; it stays %s", f.id, s.bActive ? "on" : "off");
        return false;
    }
    if (s.bSharesBytes) {
        Diag("%s shares bytes with another module; it stays %s", f.id, s.bActive ? "on" : "off");
        return false;
    }
    auto& v = Records();
    if (!bActive) {
        s.bActive = false; // hooks fall through first, then the patched bytes go back
        for (auto it = v.rbegin(); it != v.rend(); ++it) {
            if (it->nModule == static_cast<int>(i) && !it->bHook) {
                WriteBytes(it->uStart, it->abOriginal);
            }
        }
    } else {
        for (const auto& r : v) {
            if (r.nModule == static_cast<int>(i) && !r.bHook) {
                WriteBytes(r.uStart, r.abPatched);
            }
        }
        s.bActive = true;
    }
    Log("%s %s", f.id, bActive ? "active" : "inert");
    return true;
}

const char* Name(FeatureId id) {
    size_t i = static_cast<size_t>(id);
    return i < kCount ? kFeatures[i].id : "none";
}

AttachScope::AttachScope(FeatureId id) : m_nOuter(g_nCurrent), m_uFrom(Records().size()) {
    g_nCurrent = static_cast<int>(id);
}

AttachScope::~AttachScope() {
    g_nCurrent = m_nOuter;
    CheckSharedBytes(m_uFrom);
}

FeatureId Find(const char* sId) {
    for (size_t i = 0; i < kCount; ++i) {
        if (_stricmp(kFeatures[i].id, sId) == 0) {
            return static_cast<FeatureId>(i);
        }
    }
    return FeatureId::none;
}

}  // namespace features


namespace features::detail {

// cmp byte ptr [bActive], 0 / jne detour / jmp dword ptr [original] / detour: jmp pDetour / original: dd 0
// Registers and stack are untouched, so it suits any calling convention.
void* WrapDetour(void* pDetour) {
    if (g_nCurrent < 0 || !(kFeatures[g_nCurrent].nFlags & FEATURE_LIVE)) {
        return pDetour;
    }
    constexpr size_t kThunk = 24;
    unsigned char* p = AllocThunk(kThunk);
    if (!p) {
        return pDetour;
    }
    const auto uFlag = reinterpret_cast<uintptr_t>(&g_aState[g_nCurrent].bActive);
    p[0] = 0x80;
    p[1] = 0x3D;
    memcpy(p + 2, &uFlag, 4);
    p[6] = 0x00;
    p[7] = 0x75; // jne +6
    p[8] = 0x06;
    p[9] = 0xFF; // jmp dword ptr [p + 20]
    p[10] = 0x25;
    const auto uSlot = reinterpret_cast<uintptr_t>(p + 20);
    memcpy(p + 11, &uSlot, 4);
    p[15] = 0xE9; // jmp pDetour
    const auto nRel = static_cast<int32_t>(reinterpret_cast<uintptr_t>(pDetour) - reinterpret_cast<uintptr_t>(p + 20));
    memcpy(p + 16, &nRel, 4);
    memset(p + 20, 0, 4);
    FlushInstructionCache(GetCurrentProcess(), p, kThunk);
    return p;
}

void SetWrappedOriginal(void* pThunk, void* pOriginal) {
    memcpy(static_cast<unsigned char*>(pThunk) + 20, &pOriginal, 4);
}

void RecordHook(void* pRequested, void* pRealTarget, size_t uSize) {
    Record r{};
    r.nModule = g_nCurrent;
    r.bHook = true;
    r.uAddress = reinterpret_cast<uintptr_t>(pRequested);
    r.uStart = reinterpret_cast<uintptr_t>(pRealTarget ? pRealTarget : pRequested);
    r.uSize = uSize;
    Records().push_back(std::move(r));
    LogRecord(Records().back());
}

void RecordPatch(void* pAddress, const void* pValue, size_t uSize) {
    Record r{};
    r.nModule = g_nCurrent;
    r.bHook = false;
    r.uAddress = reinterpret_cast<uintptr_t>(pAddress);
    r.uStart = r.uAddress;
    r.uSize = uSize;
    const auto* pOld = static_cast<const unsigned char*>(pAddress);
    const auto* pNew = static_cast<const unsigned char*>(pValue);
    r.abOriginal.assign(pOld, pOld + uSize);
    r.abPatched.assign(pNew, pNew + uSize);
    Records().push_back(std::move(r));
    LogRecord(Records().back());
}

}  // namespace features::detail
