// Checks src/dllentry.cpp outside the game. Build and run (x86 Developer prompt): dllentry_test.cmd
//
//   dllentry_test exit <dll>    arms the DLL's static with a page, frees the page (the game's teardown
//                               of PCOM and its allocators), then ExitProcess(7). The static's
//                               destructor faults if it runs; the filter then ends the process with
//                               the fault code, as the game's does.
//   dllentry_test unload <dll>  arms the static with a counter and FreeLibrary's the DLL: the
//                               destructor must still run there. Exit code 0 = it ran once.
#include <windows.h>
#include <cstdio>
#include <cstring>

static LONG WINAPI GameLikeFilter(EXCEPTION_POINTERS* pInfo) {
    TerminateProcess(GetCurrentProcess(), pInfo->ExceptionRecord->ExceptionCode);
    return EXCEPTION_EXECUTE_HANDLER;
}

int main(int argc, char** argv) {
    if (argc != 3) {
        std::printf("usage: dllentry_test exit|unload <dll>\n");
        return 2;
    }
    SetUnhandledExceptionFilter(&GameLikeFilter);
    HMODULE hDll = LoadLibraryA(argv[2]);
    auto pArm = hDll ? reinterpret_cast<void(*)(volatile LONG*)>(GetProcAddress(hDll, "Arm")) : nullptr;
    if (!pArm) {
        std::printf("cannot load %s\n", argv[2]);
        return 2;
    }
    if (std::strcmp(argv[1], "exit") == 0) {
        void* pPage = VirtualAlloc(nullptr, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        pArm(static_cast<volatile LONG*>(pPage));
        VirtualFree(pPage, 0, MEM_RELEASE);
        ExitProcess(7);
    }
    if (std::strcmp(argv[1], "unload") == 0) {
        static volatile LONG nDestroyed = 0;
        pArm(&nDestroyed);
        FreeLibrary(hDll);
        return nDestroyed == 1 ? 0 : 3;
    }
    return 2;
}
