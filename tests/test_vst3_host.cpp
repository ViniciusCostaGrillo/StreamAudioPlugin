#include <PluginIds.h>
#include <Protocol.h>
#include <StreamAudioPlugin.h>

#include <pluginterfaces/base/coreiids.cpp>
#include <pluginterfaces/base/funknown.cpp>
#include <public.sdk/source/vst/vstinitiids.cpp>
#include <base/source/baseiids.cpp>

#include <pluginterfaces/base/ipluginbase.h>
#include <pluginterfaces/vst/ivstaudioprocessor.h>
#include <pluginterfaces/vst/ivstcomponent.h>
#include <pluginterfaces/vst/ivsteditcontroller.h>
#include <pluginterfaces/vst/ivstparameterchanges.h>
#include <pluginterfaces/vst/vstspeaker.h>
#include <public.sdk/source/vst/utility/memoryibstream.h>

#include <windows.h>

#include <cassert>
#include <cmath>
#include <cstring>
#include <iostream>
#include <vector>

using namespace Steinberg;
using namespace Steinberg::Vst;

typedef bool (*InitDllFunc)();
typedef bool (*ExitDllFunc)();
typedef IPluginFactory* (PLUGIN_API *GetPluginFactoryFunc)();

int main() {
    std::cout << "=== [test_vst3_host] Running VST3 host harness tests ===" << std::endl;

    const char* dllPath = "build/StreamAudioPlugin.vst3/Contents/x86_64-win/StreamAudioPlugin.vst3";
    std::cout << "[Step 1] Loading plugin DLL: " << dllPath << std::endl;

    HMODULE hMod = LoadLibraryA(dllPath);
    if (!hMod) {
        std::cerr << "FAILED: Could not load " << dllPath << " (GetLastError=" << GetLastError() << ")" << std::endl;
        return 1;
    }

    auto initDll = reinterpret_cast<InitDllFunc>(GetProcAddress(hMod, "InitDll"));
    auto exitDll = reinterpret_cast<ExitDllFunc>(GetProcAddress(hMod, "ExitDll"));
    auto getPluginFactory = reinterpret_cast<GetPluginFactoryFunc>(GetProcAddress(hMod, "GetPluginFactory"));

    assert(initDll != nullptr && "InitDll export must exist");
    assert(exitDll != nullptr && "ExitDll export must exist");
    assert(getPluginFactory != nullptr && "GetPluginFactory export must exist");

    bool inited = initDll();
    assert(inited && "InitDll() must return true");

    IPluginFactory* factory = getPluginFactory();
    assert(factory != nullptr && "GetPluginFactory() must return valid factory pointer");

    // 1. Verify factory info and class info
    PFactoryInfo factoryInfo{};
    tresult fRes = factory->getFactoryInfo(&factoryInfo);
    assert(fRes == kResultOk);
    std::cout << "  Factory vendor: " << factoryInfo.vendor << std::endl;
    assert(std::string(factoryInfo.vendor) == StreamAudio::kVendorName);

    int32 numClasses = factory->countClasses();
    std::cout << "  Exported classes count: " << numClasses << std::endl;
    assert(numClasses >= 1);

    IPluginFactory2* factory2 = nullptr;
    tresult qf2 = factory->queryInterface(IPluginFactory2::iid, reinterpret_cast<void**>(&factory2));
    assert(qf2 == kResultOk && factory2 != nullptr);

    PClassInfo2 classInfo2{};
    tresult ciRes = factory2->getClassInfo2(0, &classInfo2);
    assert(ciRes == kResultOk);
    std::cout << "  Class name: " << classInfo2.name << std::endl;
    std::cout << "  Category: " << classInfo2.category << std::endl;
    std::cout << "  SubCategories: " << classInfo2.subCategories << std::endl;
    assert(std::string(classInfo2.name) == StreamAudio::kPluginName);
    assert(std::string(classInfo2.subCategories) == StreamAudio::kPluginCategory);
    factory2->release();

    // Create IComponent instance via kProcessorUID
    IComponent* component = nullptr;
    tresult cRes = factory->createInstance(
        StreamAudio::kProcessorUID,
        IComponent::iid,
        reinterpret_cast<void**>(&component)
    );
    assert(cRes == kResultOk && component != nullptr);

    IAudioProcessor* processor = nullptr;
    tresult qpRes = component->queryInterface(IAudioProcessor::iid, reinterpret_cast<void**>(&processor));
    assert(qpRes == kResultOk && processor != nullptr);

    IEditController* controller = nullptr;
    tresult qcRes = component->queryInterface(IEditController::iid, reinterpret_cast<void**>(&controller));
    assert(qcRes == kResultOk && controller != nullptr);

    auto* plugin1 = static_cast<StreamAudio::StreamAudioPlugin*>(component);

    std::cout << "[Step 2] Initializing component and bus arrangements" << std::endl;
    tresult initRes = component->initialize(nullptr);
    assert(initRes == kResultOk);

    SpeakerArrangement inArr = SpeakerArr::kStereo;
    SpeakerArrangement outArr = SpeakerArr::kStereo;
    tresult busRes = processor->setBusArrangements(&inArr, 1, &outArr, 1);
    assert(busRes == kResultOk);

    ProcessSetup setup{};
    setup.processMode = kRealtime;
    setup.symbolicSampleSize = kSample32;
    setup.maxSamplesPerBlock = 512;
    setup.sampleRate = 48000.0;
    tresult setRes = processor->setupProcessing(setup);
    assert(setRes == kResultOk);

    tresult actRes = component->setActive(true);
    assert(actRes == kResultOk);

    // Verify parameter defaults
    ParamValue valEnabled = controller->getParamNormalized(StreamAudio::kParamEnabled);
    ParamValue valMode = controller->getParamNormalized(StreamAudio::kParamMode);
    ParamValue valCh = controller->getParamNormalized(StreamAudio::kParamChannels);
    std::cout << "  Default params: Enabled=" << valEnabled << ", Mode=" << valMode << ", ChNorm=" << valCh << std::endl;
    assert(valEnabled == 1.0);
    assert(valMode == 1.0); // Send mode
    assert(std::abs(valCh - (1.0 / 7.0)) < 1e-4); // 2 channels normalized

    // Process known stereo sine wave and verify pass-through
    constexpr int32 kNumSamples = 512;
    std::vector<float> inL(kNumSamples);
    std::vector<float> inR(kNumSamples);
    std::vector<float> outL(kNumSamples, 0.0f);
    std::vector<float> outR(kNumSamples, 0.0f);
    for (int32 i = 0; i < kNumSamples; ++i) {
        inL[i] = std::sin(2.0f * 3.14159265f * 440.0f * static_cast<float>(i) / 48000.0f) * 0.7f;
        inR[i] = std::sin(2.0f * 3.14159265f * 880.0f * static_cast<float>(i) / 48000.0f) * 0.5f;
    }

    float* inChannelPtrs[2] = { inL.data(), inR.data() };
    float* outChannelPtrs[2] = { outL.data(), outR.data() };
    AudioBusBuffers inBus{};
    inBus.numChannels = 2;
    inBus.channelBuffers32 = inChannelPtrs;
    AudioBusBuffers outBus{};
    outBus.numChannels = 2;
    outBus.channelBuffers32 = outChannelPtrs;

    ProcessData data{};
    data.processMode = kRealtime;
    data.symbolicSampleSize = kSample32;
    data.numSamples = kNumSamples;
    data.numInputs = 1;
    data.inputs = &inBus;
    data.numOutputs = 1;
    data.outputs = &outBus;

    tresult procRes = processor->process(data);
    assert(procRes == kResultOk);

    // Verify exact pass-through
    for (int32 i = 0; i < kNumSamples; ++i) {
        assert(outL[i] == inL[i] && "Left channel pass-through must be bitwise exact");
        assert(outR[i] == inR[i] && "Right channel pass-through must be bitwise exact");
    }

    // Telemetry checks
    float peakL = plugin1->getPeakL();
    float peakR = plugin1->getPeakR();
    std::cout << "  Telemetry peaks: L=" << peakL << ", R=" << peakR << std::endl;
    assert(peakL > 0.0f && "Peak L must be non-zero after sine wave process");
    assert(peakR > 0.0f && "Peak R must be non-zero after sine wave process");
    assert(plugin1->getHostSampleRate() == 48000.0);
    assert(plugin1->getLastSampleRate() == 48000);

    std::cout << "  Testing idempotent setActive calls..." << std::endl;
    // Instance 1 is currently active: calling setActive(true) repeatedly must be idempotent
    assert(component->setActive(true) == kResultOk);
    assert(component->setActive(true) == kResultOk);
    assert(plugin1->isActive() == true);

    // Create a fresh third instance to test inactive setActive(false)
    IComponent* comp3 = nullptr;
    assert(factory->createInstance(StreamAudio::kProcessorUID, IComponent::iid, reinterpret_cast<void**>(&comp3)) == kResultOk);
    assert(comp3->initialize(nullptr) == kResultOk);
    auto* plugin3 = static_cast<StreamAudio::StreamAudioPlugin*>(comp3);
    assert(plugin3->isActive() == false);

    // Repeated setActive(false) on fresh inactive instance while instance 1 is active
    assert(comp3->setActive(false) == kResultOk);
    assert(comp3->setActive(false) == kResultOk);
    assert(plugin3->isActive() == false);
    assert(plugin1->isActive() == true);

    // Activate comp3, repeated setActive(true), then deactivate comp3
    assert(comp3->setActive(true) == kResultOk);
    assert(comp3->setActive(true) == kResultOk);
    assert(plugin3->isActive() == true);
    assert(comp3->setActive(false) == kResultOk);
    assert(comp3->setActive(false) == kResultOk);
    assert(plugin3->isActive() == false);
    assert(comp3->terminate() == kResultOk);
    comp3->release();

    // Verify instance 1 remains active and unaffected
    assert(plugin1->isActive() == true);

    std::cout << "  Testing large block chunking (numSamples = 2560 > 2048)..." << std::endl;
    constexpr int32 kBigNumSamples = 2560;
    std::vector<float> bigInL(kBigNumSamples, 0.42f);
    std::vector<float> bigInR(kBigNumSamples, 0.84f);
    std::vector<float> bigOutL(kBigNumSamples, 0.0f);
    std::vector<float> bigOutR(kBigNumSamples, 0.0f);
    float* bigInPtrs[2] = { bigInL.data(), bigInR.data() };
    float* bigOutPtrs[2] = { bigOutL.data(), bigOutR.data() };
    AudioBusBuffers bigInBus{};
    bigInBus.numChannels = 2;
    bigInBus.channelBuffers32 = bigInPtrs;
    AudioBusBuffers bigOutBus{};
    bigOutBus.numChannels = 2;
    bigOutBus.channelBuffers32 = bigOutPtrs;

    ProcessData bigData{};
    bigData.processMode = kRealtime;
    bigData.symbolicSampleSize = kSample32;
    bigData.numSamples = kBigNumSamples;
    bigData.numInputs = 1;
    bigData.inputs = &bigInBus;
    bigData.numOutputs = 1;
    bigData.outputs = &bigOutBus;

    assert(processor->process(bigData) == kResultOk);
    for (int32 i = 0; i < kBigNumSamples; ++i) {
        assert(bigOutL[i] == bigInL[i]);
        assert(bigOutR[i] == bigInR[i]);
    }

    // Test with null channel pointer when numSamples > 2048 to exercise chunk offset null check
    std::cout << "  Testing Send mode chunking with null channel buffer (numSamples = 2560)..." << std::endl;
    float* nullChannelPtrs[2] = { bigInL.data(), nullptr };
    bigInBus.channelBuffers32 = nullChannelPtrs;
    assert(processor->process(bigData) == kResultOk);
    bigInBus.channelBuffers32 = bigInPtrs; // restore

    std::cout << "[Step 3] Testing host edge cases" << std::endl;
    // 3a. numSamples == 0
    data.numSamples = 0;
    assert(processor->process(data) == kResultOk);

    // 3b. processContext == nullptr
    data.numSamples = 256;
    data.processContext = nullptr;
    assert(processor->process(data) == kResultOk);

    // 3c. numInputs == 0 -> outputs must be zeroed
    data.numInputs = 0;
    data.inputs = nullptr;
    outL.assign(kNumSamples, 1.0f);
    outR.assign(kNumSamples, 1.0f);
    assert(processor->process(data) == kResultOk);
    for (int32 i = 0; i < 256; ++i) {
        assert(outL[i] == 0.0f && "Outputs must be zeroed when numInputs == 0");
        assert(outR[i] == 0.0f && "Outputs must be zeroed when numInputs == 0");
    }

    // 3d. numOutputs == 0
    data.numOutputs = 0;
    data.outputs = nullptr;
    assert(processor->process(data) == kResultOk);

    // 3e. Null channel buffers
    data.numInputs = 1;
    inBus.channelBuffers32 = nullptr;
    data.inputs = &inBus;
    data.numOutputs = 1;
    outBus.channelBuffers32 = nullptr;
    data.outputs = &outBus;
    assert(processor->process(data) == kResultOk);

    std::cout << "[Step 4] State persistence roundtrip and corrupt input testing" << std::endl;
    // Restore valid data buffers
    inBus.channelBuffers32 = inChannelPtrs;
    outBus.channelBuffers32 = outChannelPtrs;
    data.inputs = &inBus;
    data.outputs = &outBus;

    // Mutate state on instance 1 via IEditController
    controller->setParamNormalized(StreamAudio::kParamEnabled, 0.0);
    controller->setParamNormalized(StreamAudio::kParamMode, 0.0); // Receive
    controller->setParamNormalized(StreamAudio::kParamChannels, 4.0 / 7.0); // 5 channels

    // Verify getState serialization
    ResizableMemoryIBStream stream;
    tresult gsRes = component->getState(&stream);
    assert(gsRes == kResultOk);
    std::cout << "  getState serialized " << stream.getCursor() << " bytes (expected 108)" << std::endl;
    assert(stream.getCursor() == 108);

    // Verify serialized state content
    StreamAudio::PluginStateHeader initHdr{};
    std::memcpy(&initHdr, stream.getData(), sizeof(initHdr));
    assert(initHdr.magic == 0x54534153);
    assert(initHdr.version == 1);
    assert(initHdr.enabled == 0);
    assert(initHdr.mode == 0);
    assert(initHdr.channels == 5);
    assert(std::string(initHdr.identifier) == "default");
    assert(std::string(initHdr.destination) == "* local broadcast");

    // Now create a custom state payload with custom identifier and destination
    StreamAudio::PluginStateHeader customHdr{};
    customHdr.magic = 0x54534153;
    customHdr.version = 1;
    customHdr.enabled = 0;
    customHdr.mode = 0;
    customHdr.channels = 5;
    customHdr.reserved = 0;
    std::strncpy(customHdr.identifier, "vocal_lead", 31);
    std::strncpy(customHdr.destination, "192.168.1.120", 63);

    ResizableMemoryIBStream customStream;
    customStream.write(&customHdr, sizeof(customHdr), nullptr);
    customStream.seek(0, IBStream::kIBSeekSet, nullptr);

    // Create instance 2 and restore custom state
    IComponent* comp2 = nullptr;
    tresult c2Res = factory->createInstance(
        StreamAudio::kProcessorUID,
        IComponent::iid,
        reinterpret_cast<void**>(&comp2)
    );
    assert(c2Res == kResultOk && comp2 != nullptr);
    assert(comp2->initialize(nullptr) == kResultOk);

    IEditController* ctrl2 = nullptr;
    assert(comp2->queryInterface(IEditController::iid, reinterpret_cast<void**>(&ctrl2)) == kResultOk);
    auto* plugin2 = static_cast<StreamAudio::StreamAudioPlugin*>(comp2);

    tresult ssRes = comp2->setState(&customStream);
    assert(ssRes == kResultOk);

    // Verify roundtrip on instance 2 via IEditController and getters
    assert(ctrl2->getParamNormalized(StreamAudio::kParamEnabled) == 0.0);
    assert(ctrl2->getParamNormalized(StreamAudio::kParamMode) == 0.0);
    assert(std::abs(ctrl2->getParamNormalized(StreamAudio::kParamChannels) - (4.0 / 7.0)) < 1e-4);
    assert(std::string(plugin2->getIdentifier()) == "vocal_lead");
    assert(std::string(plugin2->getDestination()) == "192.168.1.120");

    // Verify that getState on instance 2 also produces identical custom state
    ResizableMemoryIBStream stateOut2;
    assert(comp2->getState(&stateOut2) == kResultOk);
    assert(stateOut2.getCursor() == 108);
    StreamAudio::PluginStateHeader readBackHdr{};
    std::memcpy(&readBackHdr, stateOut2.getData(), sizeof(readBackHdr));
    assert(readBackHdr.magic == 0x54534153);
    assert(readBackHdr.version == 1);
    assert(readBackHdr.enabled == 0);
    assert(readBackHdr.mode == 0);
    assert(readBackHdr.channels == 5);
    assert(std::string(readBackHdr.identifier) == "vocal_lead");
    assert(std::string(readBackHdr.destination) == "192.168.1.120");

    // Corrupt stream testing
    ResizableMemoryIBStream emptyStream;
    assert(comp2->setState(&emptyStream) == kResultFalse && "Empty stream must be rejected");

    ResizableMemoryIBStream truncStream;
    std::vector<uint8_t> truncBuf(40, 0x55);
    truncStream.write(truncBuf.data(), static_cast<int32>(truncBuf.size()), nullptr);
    truncStream.seek(0, IBStream::kIBSeekSet, nullptr);
    assert(comp2->setState(&truncStream) == kResultFalse && "Truncated stream must be rejected");

    ResizableMemoryIBStream badMagicStream;
    std::vector<uint8_t> badMagicBuf(108, 0);
    badMagicStream.write(badMagicBuf.data(), static_cast<int32>(badMagicBuf.size()), nullptr);
    badMagicStream.seek(0, IBStream::kIBSeekSet, nullptr);
    assert(comp2->setState(&badMagicStream) == kResultFalse && "Bad magic stream must be rejected");

    // Verify instance 2 retained its valid state after rejected attempts
    assert(std::string(plugin2->getIdentifier()) == "vocal_lead");
    assert(ctrl2->getParamNormalized(StreamAudio::kParamMode) == 0.0);

    std::cout << "[Step 5] Receive mode process verification" << std::endl;
    IAudioProcessor* proc2 = nullptr;
    assert(comp2->queryInterface(IAudioProcessor::iid, reinterpret_cast<void**>(&proc2)) == kResultOk);
    assert(proc2->setBusArrangements(&inArr, 1, &outArr, 1) == kResultOk);
    assert(proc2->setupProcessing(setup) == kResultOk);
    assert(comp2->setActive(true) == kResultOk);

    // When disabled, verify pass-through still happens without crash
    ctrl2->setParamNormalized(StreamAudio::kParamEnabled, 0.0);
    data.numSamples = 256;
    for (int32 i = 0; i < 256; ++i) {
        inL[i] = 0.33f;
        inR[i] = 0.44f;
    }
    assert(proc2->process(data) == kResultOk);
    for (int32 i = 0; i < 256; ++i) {
        assert(outL[i] == inL[i]);
        assert(outR[i] == inR[i]);
    }

    // Enable in Receive mode (empty jitter buffers: pass-through should hold)
    ctrl2->setParamNormalized(StreamAudio::kParamEnabled, 1.0);
    assert(proc2->process(data) == kResultOk);
    for (int32 i = 0; i < 256; ++i) {
        assert(outL[i] == inL[i]);
        assert(outR[i] == inR[i]);
    }

    std::cout << "[Step 6] Clean teardown" << std::endl;
    assert(comp2->setActive(false) == kResultOk);
    assert(comp2->terminate() == kResultOk);
    ctrl2->release();
    proc2->release();
    comp2->release();

    assert(component->setActive(false) == kResultOk);
    assert(component->terminate() == kResultOk);
    controller->release();
    processor->release();
    component->release();

    bool deinitRes = exitDll();
    assert(deinitRes && "ExitDll() must return true");

    BOOL freeRes = FreeLibrary(hMod);
    assert(freeRes && "FreeLibrary must succeed");

    std::cout << "=== [test_vst3_host] ALL HOST TESTS PASSED SUCCESSFULLY! ===" << std::endl;
    return 0;
}
