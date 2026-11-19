#include "featurespage.h"
#include "constants.h"
#include "debug.h"
#include "feature_list.h"
#include "paths.h"

#include <windows.h>
#include <strsafe.h>
#include <string>


namespace {

struct Row {
    const char* id;
    const char* title;
    bool bDefault;
    int nFlags;
    FeatureSection eSection;
    const char* sRequires; // "none" when it needs no other module
};

// The same list Kaentake.dll attaches from; the attach functions are not referenced here.
const Row kRows[] = {
#define LAUNCHER_ROW(id, title, def, flags, section, req, version, attach) \
    { #id, title, (def) != 0, (flags), FeatureSection::section, #req },
    KAENTAKE_FEATURES(LAUNCHER_ROW)
#undef LAUNCHER_ROW
};
constexpr int kRowCount = sizeof(kRows) / sizeof(kRows[0]);

const char* const kSectionTitles[] = {
#define LAUNCHER_SECTION(id, title) title,
    KAENTAKE_SECTIONS(LAUNCHER_SECTION)
#undef LAUNCHER_SECTION
};
constexpr int kSectionCount = static_cast<int>(FeatureSection::count);

// Not a module: the launcher's own switch for opening this page, kept in [features] next to the modules' lines.
constexpr const char* kAtLaunchKey = "page_at_launch";

constexpr int kIdFirstBox = 1000;
constexpr int kIdSave = 101;
constexpr int kIdDefaults = 102;
constexpr int kIdAtLaunch = 103;

// 96-dpi pixels (the launcher is not DPI aware; Windows scales the window)
constexpr int kMargin = 12;
constexpr int kColumnWidth = 280;
constexpr int kRowHeight = 22;
constexpr int kTextHeight = 18;
constexpr int kIndent = 10; // a section's boxes, under its heading
constexpr int kButtonWidth = 88;
constexpr int kButtonHeight = 26;
constexpr int kGap = 8;

std::string g_sConfig;
HFONT g_hFont = nullptr;
HFONT g_hHeadingFont = nullptr;
HWND g_aBox[kRowCount] = {};
HWND g_hAtLaunch = nullptr;
HWND g_hStatus = nullptr;
bool g_bPlay = false;
bool g_bDone = false;

// A module the server turned off at the last login (config.ini [features] inert=, written by the feature handshake).
// Its box shows it off and greyed; the player's own switch is kept in g_aStored and saved unchanged.
bool g_aVetoed[kRowCount] = {};
bool g_aStored[kRowCount] = {};
// What config.ini holds now, to tell whether there are unsaved changes.
bool g_aSaved[kRowCount] = {};
bool g_bSavedAtLaunch = true;


std::string ConfigPath() {
    return kpaths::ConfigPath();
}

bool InInertList(const char* sId) {
    char buf[512];
    if (GetPrivateProfileStringA("features", "inert", "", buf, sizeof(buf), g_sConfig.c_str()) == 0) {
        return false;
    }
    char* pContext = nullptr;
    for (char* s = strtok_s(buf, ", \t", &pContext); s; s = strtok_s(nullptr, ", \t", &pContext)) {
        if (_stricmp(s, sId) == 0) {
            return true;
        }
    }
    return false;
}

int RowIndex(const char* sId) {
    for (int i = 0; i < kRowCount; ++i) {
        if (strcmp(kRows[i].id, sId) == 0) {
            return i;
        }
    }
    return -1;
}

bool IsChecked(HWND hBox) {
    return SendMessageA(hBox, BM_GETCHECK, 0, 0) == BST_CHECKED;
}

// The player's switch for a row: the box, or for a row the server turned off, the value kept from config.ini.
bool SwitchOn(int i) {
    return g_aVetoed[i] ? g_aStored[i] : IsChecked(g_aBox[i]);
}

// A module whose required module is unchecked cannot attach: its box is greyed out until the other one is on.
void UpdateRequirements() {
    for (int i = 0; i < kRowCount; ++i) {
        if (!g_aBox[i] || g_aVetoed[i]) {
            continue;
        }
        int nRequired = RowIndex(kRows[i].sRequires);
        if (nRequired >= 0 && g_aBox[nRequired]) {
            EnableWindow(g_aBox[i], SwitchOn(nRequired));
        }
    }
}

void SetStatus(const char* sText) {
    if (g_hStatus) {
        SetWindowTextA(g_hStatus, sText);
    }
}

bool IsDirty() {
    for (int i = 0; i < kRowCount; ++i) {
        if (g_aBox[i] && SwitchOn(i) != g_aSaved[i]) {
            return true;
        }
    }
    return g_hAtLaunch && IsChecked(g_hAtLaunch) != g_bSavedAtLaunch;
}

void UpdateStatus() {
    SetStatus(IsDirty() ? "Changes not saved yet." : "");
}

bool Save() {
    for (int i = 0; i < kRowCount; ++i) {
        if (g_aBox[i] && !WritePrivateProfileStringA("features", kRows[i].id, SwitchOn(i) ? "1" : "0",
                                                     g_sConfig.c_str())) {
            return false;
        }
    }
    if (g_hAtLaunch && !WritePrivateProfileStringA("features", kAtLaunchKey, IsChecked(g_hAtLaunch) ? "1" : "0",
                                                   g_sConfig.c_str())) {
        return false;
    }
    for (int i = 0; i < kRowCount; ++i) {
        if (g_aBox[i]) {
            g_aSaved[i] = SwitchOn(i);
        }
    }
    g_bSavedAtLaunch = !g_hAtLaunch || IsChecked(g_hAtLaunch);
    return true;
}

void ShowSaveError(HWND hWnd) {
    LogMessage("[features_page] could not write %s (error %lu)", g_sConfig.c_str(), GetLastError());
    char msg[1024];
    StringCbPrintfA(msg, sizeof(msg),
                    "Your choices could not be saved to config.ini. Is the game folder read-only?\n\n%s",
                    g_sConfig.c_str());
    AppendLogFileHint(msg, sizeof(msg));
    MessageBoxA(hWnd, msg, "BotStory", MB_ICONERROR);
}

// Closing (Cancel or the window's X): asks first when there are unsaved changes. False keeps the page open.
bool ConfirmClose(HWND hWnd) {
    if (!IsDirty()) {
        return true;
    }
    int r = MessageBoxA(hWnd, "You changed some switches but did not save them.\n\nSave them before closing?",
                        "BotStory", MB_YESNOCANCEL | MB_ICONQUESTION);
    if (r == IDCANCEL) {
        return false;
    }
    if (r == IDYES && !Save()) {
        ShowSaveError(hWnd);
        return false;
    }
    return true;
}

HWND AddControl(HWND hParent, const char* sClass, const char* sText, DWORD dwStyle, int x, int y, int w, int h, int nId) {
    HWND hWnd = CreateWindowExA(0, sClass, sText, WS_CHILD | WS_VISIBLE | dwStyle, x, y, w, h, hParent,
                                reinterpret_cast<HMENU>(static_cast<INT_PTR>(nId)), GetModuleHandleA(nullptr), nullptr);
    SendMessageA(hWnd, WM_SETFONT, reinterpret_cast<WPARAM>(g_hFont), FALSE);
    return hWnd;
}

bool IsShownIn(const Row& r, int nSection) {
    return !(r.nFlags & FEATURE_CORE) && static_cast<int>(r.eSection) == nSection;
}

// Lines of boxes under a section's heading (two columns); 0 for a section with nothing to switch.
int SectionLines(int nSection) {
    int n = 0;
    for (const auto& r : kRows) {
        n += IsShownIn(r, nSection) ? 1 : 0;
    }
    return (n + 1) / 2;
}

int ListHeight() {
    int h = 0;
    for (int s = 0; s < kSectionCount; ++s) {
        int nLines = SectionLines(s);
        if (nLines > 0) {
            h += kTextHeight + nLines * kRowHeight + kGap;
        }
    }
    return h;
}

int ClientWidth() {
    return kMargin * 2 + kColumnWidth * 2;
}

int ClientHeight() {
    return kMargin + kTextHeight + kGap + ListHeight() + kTextHeight + kGap + kRowHeight + kGap + kButtonHeight
           + kMargin;
}

void CreateControls(HWND hWnd) {
    int y = kMargin;
    AddControl(hWnd, "STATIC", "Switch client features on or off. Changes apply the next time the game starts.",
               SS_LEFT, kMargin, y, kColumnWidth * 2, kTextHeight, -1);
    y += kTextHeight + kGap;

    // Each section: its heading, then its boxes in two columns (the first half on the left), in list order.
    for (int s = 0; s < kSectionCount; ++s) {
        int nLines = SectionLines(s);
        if (nLines == 0) {
            continue;
        }
        HWND hHeading = AddControl(hWnd, "STATIC", kSectionTitles[s], SS_LEFT, kMargin, y, kColumnWidth * 2,
                                   kTextHeight, -1);
        SendMessageA(hHeading, WM_SETFONT, reinterpret_cast<WPARAM>(g_hHeadingFont), FALSE);
        y += kTextHeight;

        int nShown = 0;
        for (int i = 0; i < kRowCount; ++i) {
            const Row& r = kRows[i];
            if (!IsShownIn(r, s)) {
                continue;
            }
            int nColumn = nShown / nLines;
            int nLine = nShown % nLines;
            ++nShown;
            std::string sLabel = r.title;
            if (r.nFlags & FEATURE_SERVER_GATED) {
                sLabel += " *";
            }
            g_aStored[i] = ReadFeatureSwitch(g_sConfig.c_str(), r.id, r.bDefault);
            g_aSaved[i] = g_aStored[i];
            g_aVetoed[i] = InInertList(r.id);
            if (g_aVetoed[i]) {
                sLabel += " (off on this server)";
            }
            g_aBox[i] = AddControl(hWnd, "BUTTON", sLabel.c_str(), BS_AUTOCHECKBOX | WS_TABSTOP,
                                   kMargin + kIndent + nColumn * kColumnWidth, y + nLine * kRowHeight,
                                   kColumnWidth - kGap - kIndent, kRowHeight, kIdFirstBox + i);
            SendMessageA(g_aBox[i], BM_SETCHECK, g_aStored[i] && !g_aVetoed[i] ? BST_CHECKED : BST_UNCHECKED, 0);
            if (g_aVetoed[i]) {
                EnableWindow(g_aBox[i], FALSE);
            }
        }
        y += nLines * kRowHeight + kGap;
    }

    AddControl(hWnd, "STATIC", "* Changes gameplay: the server may switch it off or require it.",
               SS_LEFT, kMargin, y, kColumnWidth * 2, kTextHeight, -1);
    y += kTextHeight + kGap;

    g_hAtLaunch = AddControl(hWnd, "BUTTON", "Show this page every time (hold Shift while starting to see it anyway)",
                             BS_AUTOCHECKBOX | WS_TABSTOP, kMargin, y, kColumnWidth * 2, kRowHeight, kIdAtLaunch);
    g_bSavedAtLaunch = FeaturesPageAtLaunch();
    SendMessageA(g_hAtLaunch, BM_SETCHECK, g_bSavedAtLaunch ? BST_CHECKED : BST_UNCHECKED, 0);
    y += kRowHeight + kGap;

    AddControl(hWnd, "BUTTON", "Defaults", BS_PUSHBUTTON | WS_TABSTOP, kMargin, y, kButtonWidth, kButtonHeight, kIdDefaults);
    int x = ClientWidth() - kMargin - kButtonWidth * 3 - kGap * 2;
    int nStatusX = kMargin + kButtonWidth + kGap;
    g_hStatus = AddControl(hWnd, "STATIC", "", SS_LEFT | SS_CENTERIMAGE, nStatusX, y, x - kGap - nStatusX,
                           kButtonHeight, -1);
    AddControl(hWnd, "BUTTON", "Play", BS_DEFPUSHBUTTON | WS_TABSTOP, x, y, kButtonWidth, kButtonHeight, IDOK);
    AddControl(hWnd, "BUTTON", "Save", BS_PUSHBUTTON | WS_TABSTOP, x + kButtonWidth + kGap, y, kButtonWidth,
               kButtonHeight, kIdSave);
    AddControl(hWnd, "BUTTON", "Cancel", BS_PUSHBUTTON | WS_TABSTOP, x + (kButtonWidth + kGap) * 2, y, kButtonWidth,
               kButtonHeight, IDCANCEL);
    UpdateRequirements();
}

LRESULT CALLBACK FeaturesWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE:
        CreateControls(hWnd);
        return 0;
    case WM_COMMAND: {
        int nId = LOWORD(wParam);
        if ((nId >= kIdFirstBox && nId < kIdFirstBox + kRowCount) || nId == kIdAtLaunch) {
            UpdateRequirements();
            UpdateStatus();
            return 0;
        }
        if (nId == kIdDefaults) {
            for (int i = 0; i < kRowCount; ++i) {
                if (g_aBox[i] && !g_aVetoed[i]) {
                    SendMessageA(g_aBox[i], BM_SETCHECK, kRows[i].bDefault ? BST_CHECKED : BST_UNCHECKED, 0);
                }
            }
            SendMessageA(g_hAtLaunch, BM_SETCHECK, BST_CHECKED, 0);
            UpdateRequirements();
            UpdateStatus();
            return 0;
        }
        if (nId == IDOK || nId == kIdSave) {
            if (!Save()) {
                ShowSaveError(hWnd);
                return 0;
            }
            if (nId == kIdSave) { // stays open: Play starts the game, Cancel or X closes
                SetStatus("Saved. Changes apply when the game starts.");
                return 0;
            }
            g_bPlay = true;
            DestroyWindow(hWnd);
            return 0;
        }
        if (nId == IDCANCEL) {
            if (ConfirmClose(hWnd)) {
                DestroyWindow(hWnd);
            }
            return 0;
        }
        break;
    }
    case WM_CLOSE:
        if (ConfirmClose(hWnd)) {
            DestroyWindow(hWnd);
        }
        return 0;
    case WM_DESTROY:
        g_bDone = true; // no WM_QUIT: the launcher goes on after the page
        return 0;
    }
    return DefWindowProcA(hWnd, msg, wParam, lParam);
}

}  // namespace


bool FeaturesPageAtLaunch() {
    return ReadFeatureSwitch(ConfigPath().c_str(), kAtLaunchKey, true);
}

bool FeaturesPageRun() {
    g_sConfig = ConfigPath();
    HINSTANCE hInstance = GetModuleHandleA(nullptr);

    NONCLIENTMETRICSA ncm = {};
    ncm.cbSize = sizeof(ncm);
    if (SystemParametersInfoA(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0)) {
        g_hFont = CreateFontIndirectA(&ncm.lfMessageFont);
        LOGFONTA lfHeading = ncm.lfMessageFont;
        lfHeading.lfWeight = FW_BOLD;
        g_hHeadingFont = CreateFontIndirectA(&lfHeading);
    }
    if (!g_hFont) {
        g_hFont = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
    }
    if (!g_hHeadingFont) {
        g_hHeadingFont = g_hFont;
    }

    WNDCLASSA wc = {};
    wc.lpfnWndProc = FeaturesWndProc;
    wc.hInstance = hInstance;
    wc.hIcon = LoadIconA(hInstance, "IDI_ICON1");
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    wc.lpszClassName = "BotStoryFeatures";
    RegisterClassA(&wc);

    DWORD dwStyle = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    RECT rc = { 0, 0, ClientWidth(), ClientHeight() };
    AdjustWindowRect(&rc, dwStyle, FALSE);
    int w = rc.right - rc.left;
    int h = rc.bottom - rc.top;
    HWND hWnd = CreateWindowExA(0, wc.lpszClassName, "BotStory Features", dwStyle,
                                (GetSystemMetrics(SM_CXSCREEN) - w) / 2, (GetSystemMetrics(SM_CYSCREEN) - h) / 2, w, h,
                                nullptr, nullptr, hInstance, nullptr);
    if (!hWnd) {
        return true; // no page, but the game still starts
    }
    ShowWindow(hWnd, SW_SHOW);
    SetForegroundWindow(hWnd);

    MSG msg;
    while (!g_bDone && GetMessageA(&msg, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageA(hWnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
    }
    return g_bPlay;
}
