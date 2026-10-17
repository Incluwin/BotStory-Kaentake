#pragma once
// The module registry: which of feature_list.h's modules are attached, the hooks and patches each one made, and a
// runtime switch per module (see SetActive).
#include "feature_list.h"

#include <cstddef>

enum class FeatureId : int {
    none = -1,
#define FEATURE_ENUM(id, title, def, flags, req, attach) id,
    KAENTAKE_FEATURES(FEATURE_ENUM)
#undef FEATURE_ENUM
    count
};

namespace features {

// DllMain: the FEATURE_EARLY modules (the version guard).
void AttachEarly();

// ZExceptionHandler::ZExceptionHandler (system.cpp): every other module that config.ini [features] leaves on, in
// list order, then the shared-bytes check.
void AttachClient();

// A FEATURE_DEFERRED module, attached by the module that needs it at that point. Attaches once.
void AttachDeferred(FeatureId id);

bool IsAttached(FeatureId id);
bool IsActive(FeatureId id);

// Make an attached FEATURE_LIVE module inert, or active again, without a restart (a server veto): its hooks fall
// through to the game's own code and its byte patches go back to the original bytes. Call on the game's thread.
// False when the module is not attached, not FEATURE_LIVE, or shares bytes with another attached module.
bool SetActive(FeatureId id, bool bActive);

const char* Name(FeatureId id);
FeatureId Find(const char* sId); // FeatureId::none when unknown

// For hooks a module installs after its attach function returned (from inside one of its own hooks): records them
// under that module, and checks them for shared bytes when the scope ends.
class AttachScope {
public:
    explicit AttachScope(FeatureId id);
    ~AttachScope();
    AttachScope(const AttachScope&) = delete;
    AttachScope& operator=(const AttachScope&) = delete;

private:
    int m_nOuter;
    size_t m_uFrom;
};

}  // namespace features


// hook.cpp's side: AttachHook and PatchMemory report every hook and patch here, under the module being attached.
namespace features::detail {

// For a hook made by a FEATURE_LIVE module: a small thunk that jumps to pDetour while the module is active and to
// the original function otherwise. pDetour itself for any other hook.
void* WrapDetour(void* pDetour);
void SetWrappedOriginal(void* pThunk, void* pOriginal);

void RecordHook(void* pRequested, void* pRealTarget, size_t uSize);

// Called by PatchMemory once the bytes are writable and before they change.
void RecordPatch(void* pAddress, const void* pValue, size_t uSize);

}  // namespace features::detail
