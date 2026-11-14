#pragma once

#include <Protocol.h>

#include <cstddef>
#include <cstdint>
#include <memory>

namespace StreamAudio {

struct ReceiverSlot;
struct SenderSlot;

struct ReceivedPacketView {
    uint32_t sampleRate = 0;
    uint8_t channels = 0;
    uint32_t frameCount = 0;
    const float* planarSamples = nullptr;
};

class UdpService {
public:
    static UdpService& instance();

    bool acquire();
    void release();
    size_t refCount() const noexcept;

    ReceiverSlot* createReceiver(const char* identifier);
    void destroyReceiver(ReceiverSlot* slot);
    void setReceiverIdentifier(ReceiverSlot* slot, const char* identifier);

    SenderSlot* createSender();
    void destroySender(SenderSlot* slot);

    bool tryPushSendAudio(
        SenderSlot* slot,
        const char* identifier,
        uint32_t destIpNetOrder,
        uint8_t channels,
        uint32_t sampleRate,
        uint32_t frames,
        const float* const* channelPtrs
    );

    bool tryPopReceivedPacket(ReceiverSlot* slot, ReceivedPacketView& out);

    bool isRxActive(const ReceiverSlot* slot) const noexcept;
    bool isTxActive(const SenderSlot* slot) const noexcept;

    void setBindPortForTest(uint16_t port) noexcept;
    uint16_t activePort() const noexcept;

private:
    UdpService();
    ~UdpService();
    UdpService(const UdpService&) = delete;
    UdpService& operator=(const UdpService&) = delete;

    struct Impl;
    std::unique_ptr<Impl> pImpl_;
};

} // namespace StreamAudio
