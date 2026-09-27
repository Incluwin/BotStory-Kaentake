#include "pch.h"
#include "v83_version_guard.h"
#include "v83_offsets.h"
#include "debug.h"

#include <windows.h>

namespace v83 {

namespace {

struct ClientFingerprint {
    bool     valid;
    uint32_t size_of_image;
    uint32_t timestamp;
};

ClientFingerprint ReadClientFingerprint() {
    ClientFingerprint fp{};

    HMODULE hModule = GetModuleHandleA(nullptr);  // MapleStory.exe (the host process)
    if (!hModule) {
        return fp;
    }

    auto* base = reinterpret_cast<const unsigned char*>(hModule);
    auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
        return fp;
    }

    auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) {
        return fp;
    }

    fp.valid         = true;
    fp.size_of_image = nt->OptionalHeader.SizeOfImage;
    fp.timestamp     = nt->FileHeader.TimeDateStamp;
    return fp;
}

}  // namespace

bool CheckClientVersion() {
    const ClientFingerprint fp = ReadClientFingerprint();
    if (!fp.valid) {
        DEBUG_MESSAGE("[v83] Could not read MapleStory.exe PE headers; skipping version check.");
        return true;  // Don't block on our own failure to introspect.
    }

    DEBUG_MESSAGE("[v83] MapleStory.exe SizeOfImage=0x%08X TimeDateStamp=0x%08X",
                  fp.size_of_image, fp.timestamp);

    const bool size_configured = kExpectedImageSize != 0;
    const bool ts_configured   = kExpectedTimeDateStamp != 0;

    if (!size_configured && !ts_configured) {
        return true;  // Guard disabled -- informational log only.
    }

    const bool size_matches = !size_configured || fp.size_of_image == kExpectedImageSize;
    const bool ts_matches   = !ts_configured   || fp.timestamp     == kExpectedTimeDateStamp;

    // Require both configured checks to match. If only one is populated, a
    // single mismatch is enough to fail -- that's the user's only signal.
    return size_matches && ts_matches;
}

void EnsureClientVersionOrExit() {
    if (CheckClientVersion()) {
        return;
    }

    const ClientFingerprint fp = ReadClientFingerprint();
    char msg[512];
    wsprintfA(msg,
              "Kaentake refuses to attach: MapleStory.exe does not match the\n"
              "expected v83 fingerprint compiled into v83_offsets.h.\n\n"
              "Expected SizeOfImage=0x%08X, TimeDateStamp=0x%08X\n"
              "Actual   SizeOfImage=0x%08X, TimeDateStamp=0x%08X\n\n"
              "Running with wrong offsets will corrupt client memory.\n"
              "Update kExpectedImageSize / kExpectedTimeDateStamp if this\n"
              "client is the one the mod is intended for.",
              kExpectedImageSize, kExpectedTimeDateStamp,
              fp.valid ? fp.size_of_image : 0u,
              fp.valid ? fp.timestamp     : 0u);
    MessageBoxA(nullptr, msg, "Kaentake: client version mismatch", MB_ICONERROR | MB_OK);
    ExitProcess(1);
}

}  // namespace v83
