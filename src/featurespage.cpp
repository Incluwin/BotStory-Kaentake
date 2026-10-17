#include "featurespage.h"
#include "constants.h"
#include "feature_list.h"

#include <windows.h>
#include <string>


namespace {

struct Row {
    const char* id;
    const char* title;
    bool bDefault;
    int nFlags;
    const char* sRequires; // "none" when it needs no other module
};

// The same list Kaentake.dll attaches from; the attach functions are not referenced here.
const Row kRows[] = {
#define LAUNCHER_ROW(id, title, def, flags, req, version, attach) { #id, title, (def) != 0, (flags), #req },
    KAENTAKE_FEATURES(LAUNCHER_ROW)
#undef LAUNCHER_ROW
};
constexpr int kRowCount = sizeof(kRows) / sizeof(kRows[0]);

constexpr int kIdFirstBox = 1000;
constexpr int kIdSave = 101;
constexpr int kIdDefaults = 102;

// 96-dpi pixels (the launcher is not DPI aware; Windows scales the window)
constexpr int kMargin = 12;
constexpr int kColumnWidth = 280;
constexpr int kRowHeight = 22;
constexpr int kTextHeight = 18;
constexpr int kButtonWidth = 88;
constexpr int kButtonHeight = 26;
constexpr int kGap = 8;

std::string g_sConfig;
HFONT g_hFont = nullptr;
HWND g_aBox[kRowCount] = {};
bool g_bPlay = false;
bool g_bDone = false;


std::string ExeDir() {
    char path[MAX_PATH] = {};
    DWORD n = GetModuleFileNameA(nullptr, path, MAX_PATH);
    std::string s(path, n);
    size_t slash = s.find_last_of("\\/");
    return slash == std::string::npos ? std::string(".") : s.substr(0, slash);
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

// A module whose required module is unchecked cannot attach: its box is greyed out until the other one is on.
void UpdateRequirements() {
    for (int i = 0; i < kRowCount; ++i) {
        int nRequired = RowIndex(kRows[i].sRequires);
        if (g_aBox[i] && nRequired >= 0 && g_aBox[nRequired]) {
            EnableWindow(g_aBox[i], IsChecked(g_aBox[nRequired]));
        }
    }
}

bool Save() {
    for (int i = 0; i < kRowCount; ++i) {
        if (g_aBox[i] && !WritePrivateProfileStringA("features", kRows[i].id, IsChecked(g_aBox[i]) ? "1" : "0",
                                                     g_sConfig.c_str())) {
            return false;
        }
    }
    return true;
}

HWND AddControl(HWND hParent, const char* sClass, const char* sText, DWORD dwStyle, int x, int y, int w, int h, int nId) {
    HWND hWnd = CreateWindowExA(0, sClass, sText, WS_CHILD | WS_VISIBLE | dwStyle, x, y, w, h, hParent,
                                reinterpret_cast<HMENU>(static_cast<INT_PTR>(nId)), GetModuleHandleA(nullptr), nullptr);
    SendMessageA(hWnd, WM_SETFONT, reinterpret_cast<WPARAM>(g_hFont), FALSE);
    return hWnd;
}

int SwitchableCount() {
    int n = 0;
    for (const auto& r : kRows) {
        n += (r.nFlags & FEATURE_CORE) ? 0 : 1;
    }
    return n;
}

int ClientWidth() {
    return kMargin * 2 + kColumnWidth * 2;
}

int ClientHeight() {
    int nLines = (SwitchableCount() + 1) / 2;
    return kMargin + kTextHeight + kGap + nLines * kRowHeight + kGap + kTextHeight + kGap + kButtonHeight + kMargin;
}

void CreateControls(HWND hWnd) {
    int y = kMargin;
    AddControl(hWnd, "STATIC", "Switch client features on or off. Changes apply the next time the game starts.",
               SS_LEFT, kMargin, y, kColumnWidth * 2, kTextHeight, -1);
    y += kTextHeight + kGap;

    int nLines = (SwitchableCount() + 1) / 2;
    int nShown = 0;
    for (int i = 0; i < kRowCount; ++i) {
        const Row& r = kRows[i];
        if (r.nFlags & FEATURE_CORE) {
            continue;
        }
        int nColumn = nShown / nLines;
        int nLine = nShown % nLines;
        ++nShown;
        std::string sLabel = r.title;
        if (r.nFlags & FEATURE_SERVER_GATED) {
            sLabel += " *";
        }
        g_aBox[i] = AddControl(hWnd, "BUTTON", sLabel.c_str(), BS_AUTOCHECKBOX | WS_TABSTOP,
                               kMargin + nColumn * kColumnWidth, y + nLine * kRowHeight, kColumnWidth - kGap, kRowHeight,
                               kIdFirstBox + i);
        SendMessageA(g_aBox[i], BM_SETCHECK, ReadFeatureSwitch(g_sConfig.c_str(), r.id, r.bDefault) ? BST_CHECKED : BST_UNCHECKED, 0);
    }
    y += nLines * kRowHeight + kGap;

    AddControl(hWnd, "STATIC", "* Changes gameplay: the server may switch it off or require it.",
               SS_LEFT, kMargin, y, kColumnWidth * 2, kTextHeight, -1);
    y += kTextHeight + kGap;

    AddControl(hWnd, "BUTTON", "Defaults", BS_PUSHBUTTON | WS_TABSTOP, kMargin, y, kButtonWidth, kButtonHeight, kIdDefaults);
    int x = ClientWidth() - kMargin - kButtonWidth * 3 - kGap * 2;
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
        if (nId >= kIdFirstBox && nId < kIdFirstBox + kRowCount) {
            UpdateRequirements();
            return 0;
        }
        if (nId == kIdDefaults) {
            for (int i = 0; i < kRowCount; ++i) {
                if (g_aBox[i]) {
                    SendMessageA(g_aBox[i], BM_SETCHECK, kRows[i].bDefault ? BST_CHECKED : BST_UNCHECKED, 0);
                }
            }
            UpdateRequirements();
            return 0;
        }
        if (nId == IDOK || nId == kIdSave) {
            if (!Save()) {
                MessageBoxA(hWnd, ("Could not write " + g_sConfig).c_str(), "BotStory Features", MB_ICONERROR);
                return 0;
            }
            g_bPlay = nId == IDOK;
            DestroyWindow(hWnd);
            return 0;
        }
        if (nId == IDCANCEL) {
            DestroyWindow(hWnd);
            return 0;
        }
        break;
    }
    case WM_CLOSE:
        DestroyWindow(hWnd);
        return 0;
    case WM_DESTROY:
        g_bDone = true; // no WM_QUIT: the launcher goes on after the page
        return 0;
    }
    return DefWindowProcA(hWnd, msg, wParam, lParam);
}

}  // namespace


bool FeaturesPageRun() {
    g_sConfig = ExeDir() + "\\" CONSTANTS_CONFIG_NAME;
    HINSTANCE hInstance = GetModuleHandleA(nullptr);

    NONCLIENTMETRICSA ncm = {};
    ncm.cbSize = sizeof(ncm);
    if (SystemParametersInfoA(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0)) {
        g_hFont = CreateFontIndirectA(&ncm.lfMessageFont);
    }
    if (!g_hFont) {
        g_hFont = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
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
