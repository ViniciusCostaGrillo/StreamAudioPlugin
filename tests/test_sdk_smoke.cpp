#include <pluginterfaces/vst/ivstaudioprocessor.h>
#include <public.sdk/source/vst/vstsinglecomponenteffect.h>
#include "../src/PluginIds.h"

#include <cassert>
#include <iostream>
#include <string>

int main()
{
    std::cout << "[test_sdk_smoke] Checking PluginName: " << StreamAudio::kPluginName << std::endl;
    assert(std::string(StreamAudio::kPluginName) == "StreamAudioPlugin");

    std::cout << "[test_sdk_smoke] Checking ProcessorUID validity..." << std::endl;
    assert(StreamAudio::kProcessorUID.isValid());

    std::cout << "[test_sdk_smoke] Checking Category: " << StreamAudio::kPluginCategory << std::endl;
    assert(std::string(StreamAudio::kPluginCategory) == "Fx|Network");

    std::cout << "[test_sdk_smoke] Checking Vendor: " << StreamAudio::kVendorName << std::endl;
    assert(std::string(StreamAudio::kVendorName) == "StreamAudio");

    std::cout << "[test_sdk_smoke] Checking Version: " << StreamAudio::kVersionStr << std::endl;
    assert(std::string(StreamAudio::kVersionStr) == "1.0.0");

    std::cout << "[test_sdk_smoke] All assertions passed!" << std::endl;
    return 0;
}
