// Checks ReadFeatureSwitch (src/feature_list.h) against real config.ini files, outside the game.
// Build and run (x86 Developer prompt):  build_featureswitch.cmd
// The older [config] crew_window key follows the same on/off/true/false/yes/no/number rules as [features].
#include "feature_list.h"
#include <cstdio>
#include <string>

static int g_failures = 0;
#define CHECK(cond)                                                    \
    do {                                                               \
        if (!(cond)) {                                                 \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                              \
        }                                                              \
    } while (0)

static std::string g_path;

static void WriteIni(const char* sBody) {
    FILE* fp = nullptr;
    if (fopen_s(&fp, g_path.c_str(), "w") == 0 && fp) {
        std::fputs(sBody, fp);
        std::fclose(fp);
    }
}

static bool Crew(bool bDefault = true) {
    return ReadFeatureSwitch(g_path.c_str(), "crew_window", bDefault);
}

static void ParsesEveryWord() {
    CHECK(ParseFeatureSwitch("1") == 1);
    CHECK(ParseFeatureSwitch("0") == 0);
    CHECK(ParseFeatureSwitch("2") == 1);
    CHECK(ParseFeatureSwitch("on") == 1);
    CHECK(ParseFeatureSwitch("ON") == 1);
    CHECK(ParseFeatureSwitch("true") == 1);
    CHECK(ParseFeatureSwitch("Yes") == 1);
    CHECK(ParseFeatureSwitch("off") == 0);
    CHECK(ParseFeatureSwitch("False") == 0);
    CHECK(ParseFeatureSwitch("no") == 0);
    CHECK(ParseFeatureSwitch("maybe") == -1);
    CHECK(ParseFeatureSwitch("") == -1);
}

static void FeaturesLineWins() {
    WriteIni("[features]\ncrew_window=off\n[config]\ncrew_window=on\n");
    CHECK(!Crew());
    WriteIni("[features]\ncrew_window=yes\n[config]\ncrew_window=0\n");
    CHECK(Crew(false));
}

static void AliasReadsWords() {
    WriteIni("[config]\ncrew_window=on\n");
    CHECK(Crew(false)); // was off: atoi("on") is 0
    WriteIni("[config]\ncrew_window=true\n");
    CHECK(Crew(false));
    WriteIni("[config]\ncrew_window=yes\n");
    CHECK(Crew(false));
    WriteIni("[config]\ncrew_window=off\n");
    CHECK(!Crew(true));
    WriteIni("[config]\ncrew_window=no\n");
    CHECK(!Crew(true));
    WriteIni("[config]\ncrew_window=1\n");
    CHECK(Crew(false));
    WriteIni("[config]\ncrew_window=0\n");
    CHECK(!Crew(true));
}

static void UnreadableFallsThrough() {
    WriteIni("[features]\ncrew_window=maybe\n[config]\ncrew_window=on\n");
    CHECK(Crew(false)); // an unreadable [features] line falls back to the alias
    WriteIni("[config]\ncrew_window=maybe\n");
    CHECK(Crew(true));  // an unreadable alias falls back to the default
    CHECK(!Crew(false));
    WriteIni("");
    CHECK(Crew(true));
    CHECK(!Crew(false));
}

int main() {
    char dir[MAX_PATH] = {};
    GetTempPathA(MAX_PATH, dir);
    g_path = std::string(dir) + "featureswitch_test_config.ini";
    ParsesEveryWord();
    FeaturesLineWins();
    AliasReadsWords();
    UnreadableFallsThrough();
    DeleteFileA(g_path.c_str());
    if (g_failures == 0) std::printf("featureswitch_test: all passed\n");
    return g_failures == 0 ? 0 : 1;
}
