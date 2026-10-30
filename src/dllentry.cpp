// The DLL's entry point (link /ENTRY:KaentakeDllEntry, src/CMakeLists.txt), in front of the CRT's.
//
// By the time ExitProcess detaches us, the game has torn down what our statics point into:
// CWvsApp::CleanUp unloaded PCOM and the COM servers behind every IWz*Ptr, and the exe's CRT
// destroyed the ZAllocEx pools behind Ztl_bstr_t / ZXString. The CRT's process detach would then
// run every static destructor in this DLL (resman's g_vecOverrides frees its strings through
// ZtlTaskMemFree, the weather layers and the mods' canvases and fonts Release through vtables of
// unloaded DLLs), and the first one faults: Kaentake.dll+0x4B18 -> MapleStory.exe+0x31FE, then the
// game's filter ends the process with TerminateProcess(0xC0000005) on every quit.
//
// So when the process is terminating (lpvReserved != NULL) the CRT detach is skipped and the
// statics are leaked; Windows frees the memory. A FreeLibrary (lpvReserved == NULL) still gets the
// full CRT detach. DllMain is called either way.
#include <windows.h>

extern "C" BOOL WINAPI _DllMainCRTStartup(HINSTANCE hModule, DWORD fdwReason, LPVOID lpvReserved);
BOOL WINAPI DllMain(HINSTANCE hModule, DWORD fdwReason, LPVOID lpvReserved);

extern "C" BOOL WINAPI KaentakeDllEntry(HINSTANCE hModule, DWORD fdwReason, LPVOID lpvReserved) {
    if (fdwReason == DLL_PROCESS_DETACH && lpvReserved != nullptr) {
        return DllMain(hModule, fdwReason, lpvReserved);
    }
    return _DllMainCRTStartup(hModule, fdwReason, lpvReserved);
}
