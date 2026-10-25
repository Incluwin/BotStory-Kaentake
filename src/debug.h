#pragma once

#ifdef _DEBUG
#define DEBUG_MESSAGE(FORMAT, ...) DebugMessage(FORMAT, __VA_ARGS__)
#else
#define DEBUG_MESSAGE(FORMAT, ...)
#endif


void DebugMessage(const char* sFormat, ...);

void ErrorMessage(const char* sFormat, ...);

// One line appended to kaentake_diag.txt (beside the other modules' lines), in Release builds too; DEBUG_MESSAGE
// compiles away there. Start the line with the module's "[id]" like the rest of the file.
void LogMessage(const char* sFormat, ...);

// LogMessage the first time this call site runs only, for per-frame or per-entity paths.
#define LOG_ONCE(FORMAT, ...)                \
    do {                                     \
        static bool bLogged__ = false;       \
        if (!bLogged__) {                    \
            bLogged__ = true;                \
            LogMessage(FORMAT, __VA_ARGS__); \
        }                                    \
    } while (0)

// LogMessage once per distinct integer key at this call site, up to kLogKeyCap keys (the cap is logged once).
// Game-thread only, like LOG_ONCE.
constexpr int kLogKeyCap = 64;
#define LOG_ONCE_PER_ID(KEY, FORMAT, ...)                                                          \
    do {                                                                                           \
        static int aKeys__[kLogKeyCap] = {};                                                       \
        static int nKeys__ = 0;                                                                    \
        static bool bCapped__ = false;                                                             \
        const int nKey__ = (KEY);                                                                  \
        bool bSeen__ = false;                                                                      \
        for (int i__ = 0; i__ < nKeys__; ++i__) {                                                  \
            if (aKeys__[i__] == nKey__) {                                                          \
                bSeen__ = true;                                                                    \
                break;                                                                             \
            }                                                                                      \
        }                                                                                          \
        if (!bSeen__) {                                                                            \
            if (nKeys__ < kLogKeyCap) {                                                            \
                aKeys__[nKeys__++] = nKey__;                                                       \
                LogMessage(FORMAT, __VA_ARGS__);                                                   \
            } else if (!bCapped__) {                                                               \
                bCapped__ = true;                                                                  \
                LogMessage("    (%d distinct ids logged at this site; the rest are not)", kLogKeyCap); \
            }                                                                                      \
        }                                                                                          \
    } while (0)