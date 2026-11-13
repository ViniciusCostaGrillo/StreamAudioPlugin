#pragma once

#include <pluginterfaces/base/funknown.h>

namespace StreamAudio {

// Unique 128-bit class identifier for StreamAudio processor:
// {7B4265F0-5C6D-4A2D-A2BF-18507E7822D1}
inline const Steinberg::FUID kProcessorUID(0x7B4265F0, 0x5C6D4A2D, 0xA2BF1850, 0x7E7822D1);

constexpr const char* kPluginName = "StreamAudioPlugin";
constexpr const char* kPluginCategory = "Fx|Network";
constexpr const char* kVendorName = "StreamAudio";
constexpr const char* kVersionStr = "1.0.0";

} // namespace StreamAudio
