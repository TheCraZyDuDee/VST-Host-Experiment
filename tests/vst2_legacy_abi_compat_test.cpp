// Minimal, independently authored stand-in for the legacy VST2 SDK's
// callback typedef syntax. No proprietary SDK headers are copied or used.
#include "audio/vst2_legacy_abi_compat.hpp"

#include <cstdint>
#include <type_traits>

// Some legacy aeffect.h versions define this macro even on Linux.
#define VSTCALLBACK __cdecl

struct AEffect;
using VstInt32 = std::int32_t;
using VstIntPtr = std::intptr_t;

typedef VstIntPtr (VSTCALLBACK *audioMasterCallback)(
    AEffect*, VstInt32, VstInt32, VstIntPtr, void*, float);
typedef VstIntPtr (VSTCALLBACK *AEffectDispatcherProc)(
    AEffect*, VstInt32, VstInt32, VstIntPtr, void*, float);

struct AEffect {
    AEffectDispatcherProc dispatcher{};
};

static VstIntPtr VSTCALLBACK host(
    AEffect*, VstInt32, VstInt32, VstIntPtr, void*, float) {
    return 2400;
}

// The same function-pointer declaration used by our VST2 host.
using Entry = AEffect* (VSTCALLBACK*)(audioMasterCallback);

static_assert(std::is_pointer_v<audioMasterCallback>);
static_assert(std::is_pointer_v<Entry>);

int main() {
    audioMasterCallback callback = &host;
    AEffect effect{};
    effect.dispatcher = callback;
    return effect.dispatcher(nullptr, 0, 0, 0, nullptr, 0.0F) == 2400 ? 0 : 1;
}
