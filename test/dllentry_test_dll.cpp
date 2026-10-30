// The DLL half of dllentry_test.cmd: one static whose destructor writes through a pointer the exe
// hands it, the way resman's g_vecOverrides frees its strings through the game's allocator.
#include <windows.h>

namespace {

struct Writer {
    volatile LONG* p = nullptr;
    ~Writer() {
        if (p) {
            InterlockedIncrement(p); // faults once the exe has freed the page
        }
    }
};

Writer g_writer;

} // namespace

extern "C" __declspec(dllexport) void Arm(volatile LONG* p) {
    g_writer.p = p;
}
