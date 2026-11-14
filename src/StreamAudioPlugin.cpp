#include "StreamAudioPlugin.h"
#include "Win32EditorView.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace StreamAudio {

using namespace Steinberg;
using namespace Steinberg::Vst;

StreamAudioPlugin::StreamAudioPlugin() {
    for (size_t i = 0; i < 8; ++i) {
        rxJitterBuffers_[i] = std::make_unique<JitterChannelBuffer>();
    }
    setIdentifier("default");
    setDestination("* local broadcast");
    enabled_.store(true, std::memory_order_relaxed);
    mode_.store(1, std::memory_order_relaxed); // 1 = Send
    channels_.store(2, std::memory_order_relaxed);
    hostSampleRate_.store(44100.0, std::memory_order_relaxed);
    lastSampleRate_.store(44100, std::memory_order_relaxed);
}

StreamAudioPlugin::~StreamAudioPlugin() {
    if (active_) {
        setActive(false);
    }
}

tresult PLUGIN_API StreamAudioPlugin::initialize(FUnknown* context) {
    tresult res = SingleComponentEffect::initialize(context);
    if (res != kResultOk) {
        return res;
    }

    addAudioInput(STR16("Audio Input"), SpeakerArr::kStereo);
    addAudioOutput(STR16("Audio Output"), SpeakerArr::kStereo);

    parameters.addParameter(
        STR16("Enabled"),
        nullptr,
        1,
        1.0,
        ParameterInfo::kCanAutomate,
        kParamEnabled
    );

    parameters.addParameter(
        STR16("Mode"),
        nullptr,
        1,
        1.0,
        ParameterInfo::kCanAutomate,
        kParamMode
    );

    parameters.addParameter(new RangeParameter(
        STR16("Channels"),
        kParamChannels,
        nullptr,
        1.0,
        8.0,
        2.0,
        7,
        ParameterInfo::kCanAutomate
    ));

    return kResultOk;
}

tresult PLUGIN_API StreamAudioPlugin::terminate() {
    if (active_) {
        setActive(false);
    }
    return SingleComponentEffect::terminate();
}

tresult PLUGIN_API StreamAudioPlugin::setActive(TBool state) {
    const bool requestedActive = (state != 0);
    if (requestedActive == active_) {
        return kResultOk;
    }

    tresult res = SingleComponentEffect::setActive(state);
    if (res != kResultOk) {
        return res;
    }

    if (requestedActive) {
        if (!UdpService::instance().acquire()) {
            return kResultFalse;
        }
        txSlot_ = UdpService::instance().createSender();
        rxSlot_ = UdpService::instance().createReceiver(identifier_);

        const double hostSr = hostSampleRate_.load(std::memory_order_relaxed);
        const double effectiveHostSr = hostSr > 0.0 ? hostSr : 48000.0;
        for (size_t ch = 0; ch < 8; ++ch) {
            rxResamplers_[ch].reset(effectiveHostSr, effectiveHostSr);
            if (rxJitterBuffers_[ch]) {
                rxJitterBuffers_[ch]->reset();
            }
        }
        active_ = true;
    } else {
        if (txSlot_) {
            UdpService::instance().destroySender(txSlot_);
            txSlot_ = nullptr;
        }
        if (rxSlot_) {
            UdpService::instance().destroyReceiver(rxSlot_);
            rxSlot_ = nullptr;
        }
        UdpService::instance().release();

        for (size_t ch = 0; ch < 8; ++ch) {
            if (rxJitterBuffers_[ch]) {
                rxJitterBuffers_[ch]->reset();
            }
        }
        active_ = false;
    }
    return kResultOk;
}

tresult PLUGIN_API StreamAudioPlugin::setBusArrangements(
    SpeakerArrangement* inputs,
    int32 numIns,
    SpeakerArrangement* outputs,
    int32 numOuts
) {
    if (numIns != 1 || numOuts != 1 || !inputs || !outputs) {
        return kResultFalse;
    }

    int32 inChannels = SpeakerArr::getChannelCount(inputs[0]);
    int32 outChannels = SpeakerArr::getChannelCount(outputs[0]);

    if (inChannels < 1 || inChannels > 8 || outChannels < 1 || outChannels > 8) {
        return kResultFalse;
    }

    return SingleComponentEffect::setBusArrangements(inputs, numIns, outputs, numOuts);
}

tresult PLUGIN_API StreamAudioPlugin::canProcessSampleSize(int32 symbolicSampleSize) {
    return symbolicSampleSize == kSample32 ? kResultTrue : kResultFalse;
}

tresult PLUGIN_API StreamAudioPlugin::setupProcessing(ProcessSetup& newSetup) {
    tresult res = SingleComponentEffect::setupProcessing(newSetup);
    if (res != kResultOk) {
        return res;
    }

    hostSampleRate_.store(newSetup.sampleRate, std::memory_order_release);
    const double hostSr = newSetup.sampleRate > 0.0 ? newSetup.sampleRate : 48000.0;
    const double srcSr = lastPktSampleRate_ > 0 ? static_cast<double>(lastPktSampleRate_) : hostSr;

    for (size_t ch = 0; ch < 8; ++ch) {
        rxResamplers_[ch].reset(srcSr, hostSr);
        if (rxJitterBuffers_[ch]) {
            rxJitterBuffers_[ch]->reset();
        }
    }

    return kResultOk;
}

tresult PLUGIN_API StreamAudioPlugin::setParamNormalized(ParamID tag, ParamValue value) {
    tresult res = EditController::setParamNormalized(tag, value);
    if (res != kResultOk) {
        return res;
    }

    switch (tag) {
        case kParamEnabled:
            enabled_.store(value >= 0.5, std::memory_order_release);
            break;
        case kParamMode:
            mode_.store(value >= 0.5 ? 1 : 0, std::memory_order_release);
            break;
        case kParamChannels: {
            int ch = std::clamp(static_cast<int>(std::round(value * 7.0)) + 1, 1, 8);
            channels_.store(ch, std::memory_order_release);
            break;
        }
        default:
            break;
    }

    return kResultOk;
}

IPlugView* PLUGIN_API StreamAudioPlugin::createView(FIDString name) {
    if (name && std::strcmp(name, ViewType::kEditor) == 0) {
        return new Win32EditorView(this);
    }
    return nullptr;
}

void StreamAudioPlugin::setIdentifier(const char* id) noexcept {
    if (!id) {
        id = "default";
    }
    char temp[32] = {0};
    std::strncpy(temp, id, 31);
    temp[31] = '\0';

    std::memcpy(identifier_, temp, 32);

    uint32_t seq = idSeq_.load(std::memory_order_relaxed);
    idSeq_.store(seq + 1, std::memory_order_release);
    std::memcpy(identifierStorage_, temp, 32);
    idSeq_.store(seq + 2, std::memory_order_release);

    if (rxSlot_) {
        UdpService::instance().setReceiverIdentifier(rxSlot_, temp);
    }
}

void StreamAudioPlugin::setDestination(const char* dest) noexcept {
    if (!dest) {
        dest = "* local broadcast";
    }
    char temp[64] = {0};
    std::strncpy(temp, dest, 63);
    temp[63] = '\0';

    std::memcpy(destination_, temp, 64);

    Protocol::DestinationKind kind;
    uint32_t netIp = 0;
    if (Protocol::resolveDestination(temp, kind, netIp)) {
        destIpNet_.store(netIp, std::memory_order_release);
    } else {
        const uint8_t fb[4] = { 127, 255, 255, 255 };
        std::memcpy(&netIp, fb, 4);
        destIpNet_.store(netIp, std::memory_order_release);
    }
}

void StreamAudioPlugin::readIdentifierLockFree(char out[32]) const noexcept {
    for (;;) {
        uint32_t s1 = idSeq_.load(std::memory_order_acquire);
        if (s1 & 1) {
            continue;
        }
        std::memcpy(out, identifierStorage_, 32);
        uint32_t s2 = idSeq_.load(std::memory_order_acquire);
        if (s1 == s2) {
            break;
        }
    }
    out[31] = '\0';
}

void StreamAudioPlugin::updatePeaks(float** channelBuffers, int32 numChannels, int32 numSamples) noexcept {
    if (!channelBuffers || numChannels <= 0 || numSamples <= 0) {
        return;
    }

    float pL = 0.0f;
    float pR = 0.0f;

    if (channelBuffers[0]) {
        const float* buf0 = channelBuffers[0];
        for (int32 i = 0; i < numSamples; ++i) {
            float a = std::abs(buf0[i]);
            if (a > pL) pL = a;
        }
    }

    if (numChannels > 1 && channelBuffers[1]) {
        const float* buf1 = channelBuffers[1];
        for (int32 i = 0; i < numSamples; ++i) {
            float a = std::abs(buf1[i]);
            if (a > pR) pR = a;
        }
    } else {
        pR = pL;
    }

    peakL_.store(pL, std::memory_order_relaxed);
    peakR_.store(pR, std::memory_order_relaxed);
}

bool StreamAudioPlugin::isTxActive() const noexcept {
    return txSlot_ ? UdpService::instance().isTxActive(txSlot_) : false;
}

bool StreamAudioPlugin::isRxActive() const noexcept {
    return rxSlot_ ? UdpService::instance().isRxActive(rxSlot_) : false;
}

tresult PLUGIN_API StreamAudioPlugin::getState(IBStream* state) {
    if (!state) {
        return kInvalidArgument;
    }

    PluginStateHeader hdr{};
    hdr.magic = 0x54534153; // 'SAST'
    hdr.version = 1;
    hdr.enabled = enabled_.load(std::memory_order_relaxed) ? 1 : 0;
    hdr.mode = mode_.load(std::memory_order_relaxed);
    hdr.channels = static_cast<uint8_t>(channels_.load(std::memory_order_relaxed));
    hdr.reserved = 0;
    std::memcpy(hdr.identifier, identifier_, 32);
    std::memcpy(hdr.destination, destination_, 64);

    int32 written = 0;
    tresult res = state->write(&hdr, static_cast<int32>(sizeof(hdr)), &written);
    if (res != kResultOk || written != static_cast<int32>(sizeof(hdr))) {
        return kResultFalse;
    }

    return kResultOk;
}

tresult PLUGIN_API StreamAudioPlugin::setState(IBStream* state) {
    if (!state) {
        return kInvalidArgument;
    }

    PluginStateHeader hdr{};
    int32 bytesRead = 0;
    tresult res = state->read(&hdr, static_cast<int32>(sizeof(hdr)), &bytesRead);
    if (res != kResultOk || bytesRead != static_cast<int32>(sizeof(hdr))) {
        return kResultFalse;
    }

    if (hdr.magic != 0x54534153 || hdr.version != 1) {
        return kResultFalse;
    }

    if (hdr.mode > 1 || hdr.channels < 1 || hdr.channels > 8) {
        return kResultFalse;
    }

    hdr.identifier[31] = '\0';
    hdr.destination[63] = '\0';

    setParamNormalized(kParamEnabled, hdr.enabled ? 1.0 : 0.0);
    setParamNormalized(kParamMode, hdr.mode ? 1.0 : 0.0);
    setParamNormalized(kParamChannels, (hdr.channels - 1) / 7.0);
    setIdentifier(hdr.identifier);
    setDestination(hdr.destination);

    return kResultOk;
}

tresult PLUGIN_API StreamAudioPlugin::process(ProcessData& data) {
    // 1. Process parameter changes
    if (data.inputParameterChanges) {
        int32 numParamsChanged = data.inputParameterChanges->getParameterCount();
        for (int32 i = 0; i < numParamsChanged; ++i) {
            IParamValueQueue* queue = data.inputParameterChanges->getParameterData(i);
            if (queue && queue->getPointCount() > 0) {
                ParamValue val = 0.0;
                int32 sampleOffset = 0;
                if (queue->getPoint(queue->getPointCount() - 1, sampleOffset, val) == kResultTrue) {
                    ParamID tag = queue->getParameterId();
                    switch (tag) {
                        case kParamEnabled:
                            enabled_.store(val >= 0.5, std::memory_order_relaxed);
                            break;
                        case kParamMode:
                            mode_.store(val >= 0.5 ? 1 : 0, std::memory_order_relaxed);
                            break;
                        case kParamChannels: {
                            int ch = std::clamp(static_cast<int>(std::round(val * 7.0)) + 1, 1, 8);
                            channels_.store(ch, std::memory_order_relaxed);
                            break;
                        }
                        default:
                            break;
                    }
                }
            }
        }
    }

    if (data.numSamples <= 0) {
        return kResultOk;
    }

    if (data.numOutputs == 0 || !data.outputs) {
        return kResultOk;
    }

    int32 numOutChannels = data.outputs[0].numChannels;
    if (numOutChannels <= 0 || !data.outputs[0].channelBuffers32) {
        return kResultOk;
    }

    // 2. Pass-through invariant:
    // Copy input to output, zeroing excess output channels or all outputs if numInputs == 0.
    int32 numInChannels = (data.numInputs > 0 && data.inputs && data.inputs[0].channelBuffers32)
        ? data.inputs[0].numChannels
        : 0;

    for (int32 ch = 0; ch < numOutChannels; ++ch) {
        float* outBuf = data.outputs[0].channelBuffers32[ch];
        if (!outBuf) continue;

        if (ch < numInChannels && data.inputs[0].channelBuffers32[ch]) {
            const float* inBuf = data.inputs[0].channelBuffers32[ch];
            if (inBuf != outBuf) {
                std::memcpy(outBuf, inBuf, data.numSamples * sizeof(float));
            }
        } else {
            std::memset(outBuf, 0, data.numSamples * sizeof(float));
        }
    }

    const bool isEnabled = enabled_.load(std::memory_order_relaxed);
    if (!isEnabled) {
        updatePeaks(data.outputs[0].channelBuffers32, numOutChannels, data.numSamples);
        return kResultOk;
    }

    const uint8_t currentMode = mode_.load(std::memory_order_relaxed);
    const int32 totalSamples = data.numSamples;
    constexpr int32 kProcessChunkMax = 2048;

    if (currentMode == 1) {
        // Send mode
        if (txSlot_) {
            char idBuf[32];
            readIdentifierLockFree(idBuf);
            const uint32_t destIpNet = destIpNet_.load(std::memory_order_relaxed);
            const uint8_t configuredCh = static_cast<uint8_t>(std::clamp(channels_.load(std::memory_order_relaxed), 1, 8));
            const uint8_t sendChannels = static_cast<uint8_t>(std::clamp(std::min(static_cast<int32>(configuredCh), numInChannels), 0, 8));

            if (sendChannels > 0) {
                const uint32_t hostSr = static_cast<uint32_t>(hostSampleRate_.load(std::memory_order_relaxed));
                const uint32_t sr = hostSr > 0 ? hostSr : 48000;
                lastSampleRate_.store(sr, std::memory_order_relaxed);

                for (int32 offset = 0; offset < totalSamples; offset += kProcessChunkMax) {
                    const int32 chunkFrames = std::min(kProcessChunkMax, totalSamples - offset);
                    const float* chunkPtrs[8] = {nullptr};
                    bool hasNullChannel = false;
                    for (uint8_t ch = 0; ch < sendChannels; ++ch) {
                        const float* inBuf = (ch < numInChannels && data.inputs[0].channelBuffers32)
                            ? data.inputs[0].channelBuffers32[ch]
                            : nullptr;
                        if (!inBuf) {
                            hasNullChannel = true;
                            chunkPtrs[ch] = nullptr;
                        } else {
                            chunkPtrs[ch] = inBuf + offset;
                        }
                    }
                    if (!hasNullChannel) {
                        UdpService::instance().tryPushSendAudio(
                            txSlot_,
                            idBuf,
                            destIpNet,
                            sendChannels,
                            sr,
                            static_cast<uint32_t>(chunkFrames),
                            chunkPtrs
                        );
                    }
                }
            }
        }
    } else {
        // Receive mode (currentMode == 0)
        if (rxSlot_) {
            ReceivedPacketView pkt{};
            const double hostSr = hostSampleRate_.load(std::memory_order_relaxed);
            const double effectiveHostSr = hostSr > 0.0 ? hostSr : 48000.0;

            while (UdpService::instance().tryPopReceivedPacket(rxSlot_, pkt)) {
                if (pkt.sampleRate > 0 && pkt.channels > 0 && pkt.planarSamples && pkt.frameCount > 0) {
                    lastSampleRate_.store(pkt.sampleRate, std::memory_order_relaxed);

                    if (pkt.sampleRate != lastPktSampleRate_ || pkt.channels != lastPktChannels_) {
                        lastPktSampleRate_ = pkt.sampleRate;
                        lastPktChannels_ = pkt.channels;
                        for (size_t ch = 0; ch < 8; ++ch) {
                            rxResamplers_[ch].reset(static_cast<double>(pkt.sampleRate), effectiveHostSr);
                        }
                    }

                    const uint8_t channelsToResample = std::min(static_cast<uint8_t>(8), pkt.channels);
                    for (uint8_t ch = 0; ch < channelsToResample; ++ch) {
                        const float* inSamples = pkt.planarSamples + (ch * pkt.frameCount);
                        size_t inConsumed = 0;
                        size_t produced = rxResamplers_[ch].process(
                            inSamples,
                            pkt.frameCount,
                            resampleScratch_.data(),
                            resampleScratch_.size(),
                            inConsumed
                        );
                        if (produced > 0 && rxJitterBuffers_[ch]) {
                            rxJitterBuffers_[ch]->push(resampleScratch_.data(), produced);
                        }
                    }
                }
            }

            const int32 channelsToSum = std::min(numOutChannels, 8);
            for (int32 offset = 0; offset < totalSamples; offset += kProcessChunkMax) {
                const int32 chunkFrames = std::min(kProcessChunkMax, totalSamples - offset);
                for (int32 ch = 0; ch < channelsToSum; ++ch) {
                    if (rxJitterBuffers_[ch] && data.outputs[0].channelBuffers32[ch]) {
                        size_t popped = rxJitterBuffers_[ch]->popForBlock(
                            popScratch_.data(),
                            static_cast<size_t>(chunkFrames)
                        );
                        float* outBuf = data.outputs[0].channelBuffers32[ch] + offset;
                        for (size_t i = 0; i < popped; ++i) {
                            outBuf[i] += popScratch_[i];
                        }
                    }
                }
            }
        }
    }

    updatePeaks(data.outputs[0].channelBuffers32, numOutChannels, data.numSamples);
    return kResultOk;
}

} // namespace StreamAudio
