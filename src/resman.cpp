#include "pch.h"
#include "hook.h"
#include "features.h"
#include "debug.h"
#include "wvs/wvsapp.h"
#include "wvs/util.h"
#include "ztl/ztl.h"
#include <algorithm>
#include <vector>
#include <tuple>
#include <cstdio>
#include <cwchar>


// Append a line to kaentake_diag.txt in the game's working directory.
// Used to verify which WZ paths are flowing through our hooks at runtime.
static void diag_log(const char* fmt, ...) {
    FILE* fp = nullptr;
    if (fopen_s(&fp, "kaentake_diag.txt", "a") != 0 || !fp) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    vfprintf(fp, fmt, ap);
    va_end(ap);
    fclose(fp);
}


// Pick the quickSlot variant name that best fits the current screen width.
static const wchar_t* pick_quickslot_variant() {
    int nWidth = get_screen_width();
    if      (nWidth >= 1600) return L"quickSlot3";
    else if (nWidth >= 1366) return L"quickSlot2";
    else if (nWidth >= 1024) return L"quickSlot1";
    else                     return L"quickSlot";
}


// Core swap: given a loaded base IWzProperty, replace its "quickSlot" child
// with the current-resolution variant from Custom.wz.
//
// DO NOT call get_rm() on paths that encompass the current Serialize context
// from this function's callers -- doing so re-enters Serialize on the same
// property and recurses infinitely (see history in this file). The variant
// path we fetch lives under Custom.wz, which is a separate namespace, so
// that one is safe.
void apply_quickslot_swap_on(IWzPropertyPtr pBase, const char* caller_tag) {
    const wchar_t* sVariant = pick_quickslot_variant();
    int nWidth = get_screen_width();

    Ztl_bstr_t sVariantPath = Ztl_bstr_t(L"Custom/UI/StatusBar.img/base/") + Ztl_bstr_t(sVariant);
    Ztl_variant_t vCanvas = get_rm()->GetObjectA(sVariantPath);
    if (!vCanvas.GetUnknown()) {
        diag_log("[%s] width=%d variant=%ls VARIANT_MISSING\n", caller_tag, nWidth, sVariant);
        return;
    }
    if (!pBase) {
        diag_log("[%s] width=%d variant=%ls BASE_NULL\n", caller_tag, nWidth, sVariant);
        return;
    }
    pBase->Remove(L"quickSlot");
    pBase->Add(L"quickSlot", vCanvas, false);
    diag_log("[%s] width=%d variant=%ls OK\n", caller_tag, nWidth, sVariant);
}


// Runtime entry: navigate to base via resource manager, then swap. Only safe
// to call when we are NOT already inside the Serialize hook of StatusBar.img
// or one of its ancestors -- caller must be something like
// set_screen_resolution that runs outside resource-loading callbacks.
void ApplyQuickslotOverride(const char* caller_tag) {
    Ztl_variant_t vBase = get_rm()->GetObjectA(L"UI/StatusBar.img/base");
    IUnknownPtr pBaseUnk = vBase.GetUnknown();
    IWzPropertyPtr pBase;
    if (!pBaseUnk || FAILED(pBaseUnk->QueryInterface(&pBase))) {
        diag_log("[%s] BASE_LOOKUP_FAILED\n", caller_tag);
        return;
    }
    apply_quickslot_swap_on(pBase, caller_tag);
}


static auto& g_pCustomNameSpace = *new IWzNameSpacePtr();
static std::vector<Ztl_bstr_t> g_vecOverrides;

class IWzNameSpaceImpl {
public:
    typedef HRESULT(__stdcall* raw__OnGetLocalObject_t)(IWzNameSpaceImpl*, int, BSTR, int*, VARIANT*);
    inline static raw__OnGetLocalObject_t raw__OnGetLocalObject_orig;

    HRESULT __stdcall raw__OnGetLocalObject_hook(int nIndex, BSTR sPath, int* pnPathUsed, VARIANT* pvRet) {
        HRESULT hr = raw__OnGetLocalObject_orig(this, nIndex, sPath, pnPathUsed, pvRet);
        if (SUCCEEDED(hr)) {
            return hr;
        }
        if (!std::binary_search(g_vecOverrides.begin(), g_vecOverrides.end(), Ztl_bstr_t(sPath))) {
            return hr;
        }
        return g_pCustomNameSpace->raw__OnGetLocalObject(nIndex, sPath, pnPathUsed, pvRet);
    }
};

class CWzProperty : public IWzProperty {
public:
    typedef HRESULT(__stdcall* raw_Serialize_t)(CWzProperty*, IWzArchive*);
    inline static raw_Serialize_t raw_Serialize_orig;

    HRESULT __stdcall raw_Serialize_hook(IWzArchive* pArchive) {
        HRESULT hr = raw_Serialize_orig(this, pArchive);
        if (FAILED(hr)) {
            return hr;
        }
        // Diagnostic: log Serialize events for StatusBar/quickSlot paths so we
        // can see in kaentake_diag.txt which absoluteUOL values actually flow
        // through this hook (the path MapleStory uses internally may differ
        // from what we assume). IWzArchive::absoluteUOL returns _bstr_t BY
        // VALUE, so we must cache in a local _bstr_t (not a raw wchar_t*) to
        // keep the backing BSTR alive across subsequent accessor calls.
        Ztl_bstr_t sAbsUol = pArchive->absoluteUOL;
        const wchar_t* sAbsUolC = static_cast<const wchar_t*>(sAbsUol);
        if (sAbsUolC && (wcsstr(sAbsUolC, L"StatusBar") || wcsstr(sAbsUolC, L"uickSlot"))) {
            diag_log("[ser] %ls\n", sAbsUolC);
        }

        bool bIsOverride = std::binary_search(g_vecOverrides.begin(), g_vecOverrides.end(), sAbsUol);

        IWzPropertyPtr pProperty;
        if (bIsOverride) {
            pProperty = get_rm()->GetObjectA(L"Custom/" + sAbsUol).GetUnknown();
            IEnumVARIANTPtr pEnum = pProperty->_NewEnum;
            while (true) {
                Ztl_variant_t vNext;
                ULONG uCeltFetched;
                if (FAILED(pEnum->Next(1, &vNext, &uCeltFetched)) || uCeltFetched == 0) {
                    break;
                }
                Ztl_bstr_t sNext = V_BSTR(&vNext);
                IUnknownPtr pUnk = pProperty->item[sNext].GetUnknown();
                IWzPropertyPtr pSub;
                if (!pUnk || FAILED(pUnk->QueryInterface(&pSub))) {
                    this->Add(sNext, pProperty->item[sNext], false);
                }
            }
        }

        // Resolution-aware quickslot bar override. Custom.wz provides four
        // canvases under UI/StatusBar.img/base of increasing width:
        //   quickSlot  151x80  default (8-slot layout)
        //   quickSlot1 220x80
        //   quickSlot2 326x80
        //   quickSlot3 466x80  widest (matches the 26-slot layout patched by
        //                      enlargequickslot.cpp -- rightmost slot at x=427)
        // The generic override loop above skips IWzProperty children, so
        // canvases never reach the parent property by that path. We also
        // don't know a priori whether CWzProperty::Serialize fires per
        // sub-property or only once per .img, so handle both: match at
        // "UI/StatusBar.img/base" (sub-property level) and at "UI/StatusBar.img"
        // (img root, with `this->item[L"base"]` navigated manually).
        // When StatusBar.img/base finishes loading, apply the resolution-aware
        // quickSlot override. We do NOT go through get_rm() here -- navigating
        // back to a path we're currently inside of re-enters Serialize and
        // recurses. Use `this` or `this->item[L"base"]` directly.
        if (sAbsUolC) {
            if (sAbsUol == Ztl_bstr_t(L"UI/StatusBar.img/base")) {
                IWzPropertyPtr pThisBase(static_cast<IWzProperty*>(this));
                apply_quickslot_swap_on(pThisBase, "override @base");
            } else if (sAbsUol == Ztl_bstr_t(L"UI/StatusBar.img")) {
                Ztl_variant_t vBase = this->item[L"base"];
                IUnknownPtr pBaseUnk = vBase.GetUnknown();
                IWzPropertyPtr pBase;
                if (pBaseUnk && SUCCEEDED(pBaseUnk->QueryInterface(&pBase))) {
                    apply_quickslot_swap_on(pBase, "override @img");
                } else {
                    diag_log("[override @img] base_child_missing\n");
                }
            }
        }
        return S_OK;
    }
};


void CWvsApp::InitializeResMan_hook() {
    DEBUG_MESSAGE("CWvsApp::InitializeResMan");
    features::AttachDeferred(FeatureId::wz_map_limit); // before the original opens the archives
    CWvsApp::InitializeResMan(this);

    // add custom namespace to root
    IWzWritableNameSpacePtr pWritableRoot;
    if (FAILED(get_root()->QueryInterface(&pWritableRoot))) {
        ErrorMessage("Failed to cast root namespace");
        return;
    }
    IWzNameSpacePtr pNameSpace;
    PcCreateObject<IWzNameSpacePtr>(L"NameSpace", pNameSpace, nullptr);
    Ztl_variant_t vResult;
    pWritableRoot->AddObject(L"Custom", static_cast<IUnknown*>(pNameSpace), &vResult);
    g_pCustomNameSpace = vResult.GetUnknown();

    // load Custom.wz from file system
    IWzFileSystemPtr fs;
    PcCreateObject<IWzFileSystemPtr>(L"NameSpace#FileSystem", fs, nullptr);
    char sStartPath[MAX_PATH];
    GetModuleFileNameA(nullptr, sStartPath, MAX_PATH);
    Dir_BackSlashToSlash(sStartPath);
    Dir_upDir(sStartPath);
    fs->Init(sStartPath);

    IWzPackagePtr pPackage;
    PcCreateObject<IWzPackagePtr>(L"NameSpace#Package", pPackage, nullptr);
    IWzSeekableArchivePtr pArchive = fs->item[L"Custom.wz"].GetUnknown();
    pPackage->Init(L"83", L"Custom", pArchive);
    g_pCustomNameSpace->Mount(L"/", pPackage, 1);

    // iterate custom namespace
    std::vector<std::tuple<Ztl_bstr_t, IEnumVARIANTPtr>> stack;
    stack.emplace_back(L"", g_pCustomNameSpace->_NewEnum);
    while (!stack.empty()) {
        auto [sPath, pEnum] = stack.back();
        stack.pop_back();

        while (true) {
            Ztl_variant_t vNext;
            ULONG uCeltFetched;
            if (FAILED(pEnum->Next(1, &vNext, &uCeltFetched)) || uCeltFetched == 0) {
                break;
            }
            Ztl_bstr_t sUOL = (sPath.length() > 0 ? sPath + L"/" : L"") + V_BSTR(&vNext);
            Ztl_variant_t vObj = get_rm()->GetObjectA(L"Custom/" + sUOL);
            IUnknownPtr pUnk = vObj.GetUnknown();
            if (pUnk) {
                IWzNameSpacePtr pSub;
                if (SUCCEEDED(pUnk->QueryInterface(&pSub))) {
                    stack.emplace_back(sUOL, pSub->_NewEnum);
                    continue;
                }
                IWzPropertyPtr pProp;
                if (SUCCEEDED(pUnk->QueryInterface(&pProp))) {
                    stack.emplace_back(sUOL, pProp->_NewEnum);
                }
            }
            g_vecOverrides.push_back(sUOL);
        }
    }
    std::sort(g_vecOverrides.begin(), g_vecOverrides.end()); // uses operator<

    // res_man's own hooks, made now that the archives are open
    features::AttachScope scope(FeatureId::res_man);

    // NameSpace.dll - try resolving from g_pCustomNameSpace
    IWzNameSpaceImpl::raw__OnGetLocalObject_orig = static_cast<IWzNameSpaceImpl::raw__OnGetLocalObject_t>(GetAddressByPattern("NAMESPACE.DLL", "B8 ?? ?? ?? ?? E8 ?? ?? ?? ?? 81 EC 80"));
    ATTACH_HOOK(IWzNameSpaceImpl::raw__OnGetLocalObject_orig, IWzNameSpaceImpl::raw__OnGetLocalObject_hook);

    // PCOM.dll - patch CWzProperty objects during serialization
    CWzProperty::raw_Serialize_orig = static_cast<CWzProperty::raw_Serialize_t>(GetAddressByPattern("PCOM.DLL", "B8 ?? ?? ?? ?? E8 ?? ?? ?? ?? 83 EC 68"));
    ATTACH_HOOK(CWzProperty::raw_Serialize_orig, CWzProperty::raw_Serialize_hook);
}

void CWvsApp::CleanUp_hook() {
    CWvsApp::CleanUp(this);
    g_pCustomNameSpace.Detach(); // the original CleanUp has unloaded PCOM; do not Release
}


void AttachResManMod() {
    ATTACH_HOOK(CWvsApp::InitializeResMan, CWvsApp::InitializeResMan_hook);
    ATTACH_HOOK(CWvsApp::CleanUp, CWvsApp::CleanUp_hook);
}