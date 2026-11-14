#include <PluginIds.h>
#include <StreamAudioPlugin.h>

#include <public.sdk/source/main/pluginfactory.h>

bool InitModule() {
    return true;
}

bool DeinitModule() {
    return true;
}

BEGIN_FACTORY_DEF(
    StreamAudio::kVendorName,
    "https://github.com",
    "info@streamaudio.local"
)

DEF_CLASS2(
    INLINE_UID(0x7B4265F0, 0x5C6D4A2D, 0xA2BF1850, 0x7E7822D1),
    Steinberg::PClassInfo::kManyInstances,
    kVstAudioEffectClass,
    StreamAudio::kPluginName,
    0,
    StreamAudio::kPluginCategory,
    StreamAudio::kVersionStr,
    kVstVersionString,
    StreamAudio::StreamAudioPlugin::createInstance
)

END_FACTORY
