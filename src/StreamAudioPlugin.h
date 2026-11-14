#pragma once

#include <public.sdk/source/vst/vstsinglecomponenteffect.h>
#include <pluginterfaces/base/ibstream.h>
#include <pluginterfaces/vst/ivstparameterchanges.h>

#include <PluginIds.h>
#include <Protocol.h>
#include <Resampler.h>
#include <RingBuffer.h>
#include <UdpService.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>

namespace StreamAudio {

enum ParamId : Steinberg::Vst::ParamID {
    kParamEnabled = 0,
    kParamMode = 1,
    kParamChannels = 2
};

#pragma pack(push, 1)
struct PluginStateHeader {
    uint32_t magic;       // 0x54534153 ('SAST')
    uint32_t version;     // 1
    uint8_t enabled;      // 0 or 1
    uint8_t mode;         // 0 = Receive, 1 = Send
    uint8_t channels;     // 1..8
    uint8_t reserved;     // 0
    char identifier[32];  // null-terminated
    char destination[64]; // null-terminated
};
#pragma pack(pop)
static_assert(sizeof(PluginStateHeader) == 108, "PluginStateHeader size must be exactly 108 bytes");

class StreamAudioPlugin : public Steinberg::Vst::SingleComponentEffect {
public:
    StreamAudioPlugin();
    ~StreamAudioPlugin() override;

    static Steinberg::FUnknown* createInstance(void*) {
        return static_cast<Steinberg::Vst::IAudioProcessor*>(new StreamAudioPlugin());
    }

    //---from IPluginBase---------
    Steinberg::tresult PLUGIN_API initialize(Steinberg::FUnknown* context) override;
    Steinberg::tresult PLUGIN_API terminate() override;

    //---from IComponent---------
    Steinberg::tresult PLUGIN_API setActive(Steinberg::TBool state) override;
    Steinberg::tresult PLUGIN_API setState(Steinberg::IBStream* state) override;
    Steinberg::tresult PLUGIN_API getState(Steinberg::IBStream* state) override;

    //---from IAudioProcessor---------
    Steinberg::tresult PLUGIN_API setBusArrangements(
        Steinberg::Vst::SpeakerArrangement* inputs,
        Steinberg::int32 numIns,
        Steinberg::Vst::SpeakerArrangement* outputs,
        Steinberg::int32 numOuts
    ) override;

    Steinberg::tresult PLUGIN_API canProcessSampleSize(Steinberg::int32 symbolicSampleSize) override;
    Steinberg::tresult PLUGIN_API setupProcessing(Steinberg::Vst::ProcessSetup& setup) override;
    Steinberg::tresult PLUGIN_API process(Steinberg::Vst::ProcessData& data) override;

    //---from IEditController---------
    Steinberg::tresult PLUGIN_API setParamNormalized(
        Steinberg::Vst::ParamID tag,
        Steinberg::Vst::ParamValue value
    ) override;

    Steinberg::IPlugView* PLUGIN_API createView(Steinberg::FIDString name) override;

    // Non-parameter config accessors
    const char* getIdentifier() const noexcept { return identifier_; }
    void setIdentifier(const char* id) noexcept;

    const char* getDestination() const noexcept { return destination_; }
    void setDestination(const char* dest) noexcept;

    // Atomic telemetry accessors
    float getPeakL() const noexcept { return peakL_.load(std::memory_order_relaxed); }
    float getPeakR() const noexcept { return peakR_.load(std::memory_order_relaxed); }
    bool isTxActive() const noexcept;
    bool isRxActive() const noexcept;
    uint32_t getLastSampleRate() const noexcept { return lastSampleRate_.load(std::memory_order_relaxed); }
    double getHostSampleRate() const noexcept { return hostSampleRate_.load(std::memory_order_relaxed); }

private:
    void readIdentifierLockFree(char out[32]) const noexcept;
    void updatePeaks(float** channelBuffers, Steinberg::int32 numChannels, Steinberg::int32 numSamples) noexcept;

    // Config fields (persisted)
    char identifier_[32]{"default"};
    char destination_[64]{"* local broadcast"};

    // Lock-free config shadows for audio thread
    std::atomic<bool> enabled_{true};
    std::atomic<uint8_t> mode_{1};     // 0 = Receive, 1 = Send
    std::atomic<int> channels_{2};     // 1..8
    std::atomic<uint32_t> destIpNet_{0};

    // Tiny seqlock for identifier copying on audio thread
    std::atomic<uint32_t> idSeq_{0};
    char identifierStorage_[32]{"default"};

    // Telemetry atomics
    std::atomic<float> peakL_{0.0f};
    std::atomic<float> peakR_{0.0f};
    std::atomic<uint32_t> lastSampleRate_{48000};
    std::atomic<double> hostSampleRate_{48000.0};

    // Slots from UdpService
    SenderSlot* txSlot_{nullptr};
    ReceiverSlot* rxSlot_{nullptr};

    // Audio DSP components (preallocated heap-owned buffers)
    std::array<std::unique_ptr<JitterChannelBuffer>, 8> rxJitterBuffers_;
    std::array<LinearResampler, 8> rxResamplers_;

    // Resampler tracking
    uint32_t lastPktSampleRate_{0};
    uint8_t lastPktChannels_{0};

    // Fixed scratch arrays for audio thread (<= 2048 floats)
    std::array<float, 2048> resampleScratch_{};
    std::array<float, 2048> popScratch_{};
};

} // namespace StreamAudio
