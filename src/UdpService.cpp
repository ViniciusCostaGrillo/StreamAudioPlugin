#include <UdpService.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <winsock2.h>
#include <windows.h>
#include <iphlpapi.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

namespace StreamAudio {

struct QueuedRxPacket {
    uint32_t sampleRate = 0;
    uint8_t channels = 0;
    uint32_t frameCount = 0;
    uint64_t timestampMs = 0;
    alignas(16) float samples[Protocol::kMaxAudioFloatsPerPacket]{};
};

struct ReceiverSlot {
    static constexpr size_t kMaxQueuedPackets = 256;
    static constexpr size_t kIndexMask = kMaxQueuedPackets - 1;

    mutable std::mutex idMutex;
    char identifier[Protocol::kMaxIdentifierBytes]{};

    // SPSC Queue: Producer (Receiver Thread) writes writeIndex, Consumer (Audio Thread) writes readIndex
    alignas(64) std::atomic<size_t> writeIndex{0};
    alignas(64) std::atomic<size_t> readIndex{0};
    QueuedRxPacket queue[kMaxQueuedPackets];

    std::atomic<uint64_t> lastRxTickMs{0};

    // Preallocated consumer scratch buffer for zero-alloc ReceivedPacketView
    alignas(16) float consumerScratchSamples[Protocol::kMaxAudioFloatsPerPacket]{};

    explicit ReceiverSlot(const char* initialId) {
        setIdentifier(initialId);
    }

    void setIdentifier(const char* id) {
        std::lock_guard<std::mutex> lock(idMutex);
        std::memset(identifier, 0, sizeof(identifier));
        if (id != nullptr) {
            size_t len = std::strlen(id);
            if (len >= sizeof(identifier)) {
                len = sizeof(identifier) - 1;
            }
            std::memcpy(identifier, id, len);
        }
    }

    bool matchesIdentifier(const char* packetId) const {
        if (packetId == nullptr) {
            return false;
        }
        std::lock_guard<std::mutex> lock(idMutex);
        return std::strncmp(identifier, packetId, sizeof(identifier)) == 0;
    }

    bool pushDecoded(const Protocol::DecodedAudioPacket& decoded, uint64_t now) {
        const size_t w = writeIndex.load(std::memory_order_relaxed);
        const size_t r = readIndex.load(std::memory_order_acquire);
        if (w - r >= kMaxQueuedPackets) {
            // SPSC invariant: Producer NEVER touches readIndex! Drop packet cleanly if full.
            return false;
        }

        auto& slot = queue[w & kIndexMask];
        slot.sampleRate = decoded.sampleRate;
        slot.channels = decoded.channels;
        slot.frameCount = decoded.frameCount;
        slot.timestampMs = now;

        const size_t sampleBytes = static_cast<size_t>(decoded.frameCount) * decoded.channels * sizeof(float);
        if (decoded.planarSampleBytes != nullptr && sampleBytes > 0) {
            std::memcpy(slot.samples, decoded.planarSampleBytes, sampleBytes);
        }

        writeIndex.store(w + 1, std::memory_order_release);
        lastRxTickMs.store(now, std::memory_order_release);
        return true;
    }

    bool popPacket(ReceivedPacketView& out, uint64_t now) {
        size_t r = readIndex.load(std::memory_order_relaxed);
        const size_t w = writeIndex.load(std::memory_order_acquire);

        while (r != w) {
            const auto& entry = queue[r & kIndexMask];
            // Drop stale packets (> 2000 ms) strictly on the consumer side
            if (now >= entry.timestampMs && (now - entry.timestampMs) > 2000) {
                r++;
                readIndex.store(r, std::memory_order_release);
                continue;
            }

            const size_t sampleBytes = static_cast<size_t>(entry.frameCount) * entry.channels * sizeof(float);
            if (sampleBytes > 0) {
                std::memcpy(consumerScratchSamples, entry.samples, sampleBytes);
            }

            out.sampleRate = entry.sampleRate;
            out.channels = entry.channels;
            out.frameCount = entry.frameCount;
            out.planarSamples = consumerScratchSamples;

            r++;
            readIndex.store(r, std::memory_order_release);
            return true;
        }

        return false;
    }
};

struct QueuedTxPacket {
    uint32_t destIpNetOrder = 0;
    uint16_t length = 0;
    alignas(4) uint8_t data[1280]{};
};

struct SenderSlot {
    static constexpr size_t kMaxQueuedPackets = 256;
    static constexpr size_t kIndexMask = kMaxQueuedPackets - 1;

    // SPSC Queue: Producer (Audio Thread) writes writeIndex, Consumer (Sender Thread) writes readIndex
    alignas(64) std::atomic<size_t> writeIndex{0};
    alignas(64) std::atomic<size_t> readIndex{0};
    QueuedTxPacket queue[kMaxQueuedPackets];

    std::atomic<uint64_t> lastTxTickMs{0};

    bool pushAudio(
        const char* identifier,
        uint32_t destIpNetOrder,
        uint8_t channels,
        uint32_t sampleRate,
        uint32_t frames,
        const float* const* channelPtrs,
        uint64_t now
    ) {
        if (identifier == nullptr || channels == 0 || channels > Protocol::kMaxChannels ||
            sampleRate == 0 || channelPtrs == nullptr) {
            return false;
        }
        if (frames == 0) {
            return true;
        }

        const uint32_t maxF = Protocol::maxFramesPerPacket(channels);
        const uint32_t numChunks = (frames + maxF - 1) / maxF;

        size_t w = writeIndex.load(std::memory_order_relaxed);
        const size_t r = readIndex.load(std::memory_order_acquire);
        if ((kMaxQueuedPackets - (w - r)) < numChunks) {
            return false; // Drop block to prevent partial delivery
        }

        for (uint32_t i = 0; i < numChunks; ++i) {
            const uint32_t frameOffset = i * maxF;
            const uint32_t chunkFrames = std::min(maxF, frames - frameOffset);

            auto& pkt = queue[w & kIndexMask];
            pkt.destIpNetOrder = destIpNetOrder;

            size_t written = Protocol::encodeAudioPacket(
                pkt.data, sizeof(pkt.data),
                identifier, channels, sampleRate, chunkFrames,
                channelPtrs, frameOffset
            );
            if (written == 0) {
                return false;
            }
            pkt.length = static_cast<uint16_t>(written);
            w++;
        }

        writeIndex.store(w, std::memory_order_release);
        lastTxTickMs.store(now, std::memory_order_release);
        return true;
    }
};

static void queryLocalNicIps(std::vector<uint32_t>& outIps) {
    outIps.clear();
    ULONG size = 0;
    DWORD res = GetIpAddrTable(nullptr, &size, FALSE);
    if (res == ERROR_INSUFFICIENT_BUFFER && size > 0) {
        std::vector<BYTE> buf(size);
        PMIB_IPADDRTABLE table = reinterpret_cast<PMIB_IPADDRTABLE>(buf.data());
        if (GetIpAddrTable(table, &size, FALSE) == NO_ERROR) {
            for (DWORD i = 0; i < table->dwNumEntries; ++i) {
                DWORD addr = table->table[i].dwAddr; // network byte order
                // Non-loopback check: first byte in network byte order is (addr & 0xFF)
                if ((addr & 0xFF) != 0x7F && addr != 0) {
                    outIps.push_back(static_cast<uint32_t>(addr));
                }
            }
        }
    }
}

struct UdpService::Impl {
    mutable std::mutex lifecycleMutex;
    size_t refCount = 0;

    std::mutex slotsMutex;
    std::vector<ReceiverSlot*> receiverSlots;
    std::vector<SenderSlot*> senderSlots;

    SOCKET socketFd = INVALID_SOCKET;
    HANDLE senderWakeupEvent = NULL;

    uint16_t activePort = Protocol::kPort;
    uint16_t bindPortOverride = 0;
    bool hasPortOverride = false;

    std::atomic<bool> running{false};
    std::thread senderThread;
    std::thread receiverThread;

    std::vector<uint32_t> localNicIps;

    bool startNetwork();
    void stopNetwork();
    void senderLoop();
    void drainSenderSlots();
    void receiverLoop();
};

bool UdpService::Impl::startNetwork() {
    WSADATA wsaData;
    int wsaRes = WSAStartup(MAKEWORD(2, 2), &wsaData);
    if (wsaRes != 0) {
        return false;
    }

    queryLocalNicIps(localNicIps);

    socketFd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (socketFd == INVALID_SOCKET) {
        WSACleanup();
        return false;
    }

    int reuse = 1;
    setsockopt(socketFd, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));

    int bcast = 1;
    setsockopt(socketFd, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&bcast), sizeof(bcast));

    u_long nonblock = 1;
    ioctlsocket(socketFd, FIONBIO, &nonblock);

    const uint16_t portToBind = hasPortOverride ? bindPortOverride : Protocol::kPort;
    sockaddr_in bindAddr{};
    bindAddr.sin_family = AF_INET;
    bindAddr.sin_addr.s_addr = htonl(INADDR_ANY);
    bindAddr.sin_port = htons(portToBind);

    if (bind(socketFd, reinterpret_cast<const sockaddr*>(&bindAddr), sizeof(bindAddr)) == SOCKET_ERROR) {
        closesocket(socketFd);
        socketFd = INVALID_SOCKET;
        WSACleanup();
        return false;
    }

    sockaddr_in boundAddr{};
    int addrLen = sizeof(boundAddr);
    if (getsockname(socketFd, reinterpret_cast<sockaddr*>(&boundAddr), &addrLen) == 0) {
        activePort = ntohs(boundAddr.sin_port);
    } else {
        activePort = (portToBind != 0) ? portToBind : Protocol::kPort;
    }

    senderWakeupEvent = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    if (senderWakeupEvent == NULL) {
        closesocket(socketFd);
        socketFd = INVALID_SOCKET;
        WSACleanup();
        return false;
    }

    running.store(true, std::memory_order_release);
    senderThread = std::thread(&UdpService::Impl::senderLoop, this);
    receiverThread = std::thread(&UdpService::Impl::receiverLoop, this);

    return true;
}

void UdpService::Impl::stopNetwork() {
    running.store(false, std::memory_order_release);

    if (senderWakeupEvent != NULL) {
        SetEvent(senderWakeupEvent);
    }

    if (senderThread.joinable()) {
        senderThread.join();
    }
    if (receiverThread.joinable()) {
        receiverThread.join();
    }

    if (senderWakeupEvent != NULL) {
        CloseHandle(senderWakeupEvent);
        senderWakeupEvent = NULL;
    }

    if (socketFd != INVALID_SOCKET) {
        closesocket(socketFd);
        socketFd = INVALID_SOCKET;
    }

    WSACleanup();

    activePort = Protocol::kPort;
    hasPortOverride = false;
    bindPortOverride = 0;
}

void UdpService::Impl::senderLoop() {
    while (running.load(std::memory_order_acquire)) {
        WaitForSingleObject(senderWakeupEvent, 20);
        if (!running.load(std::memory_order_acquire)) {
            break;
        }
        drainSenderSlots();
    }
    drainSenderSlots();
}

void UdpService::Impl::drainSenderSlots() {
    std::lock_guard<std::mutex> lock(slotsMutex);
    for (SenderSlot* slot : senderSlots) {
        if (slot == nullptr) {
            continue;
        }
        size_t r = slot->readIndex.load(std::memory_order_relaxed);
        const size_t w = slot->writeIndex.load(std::memory_order_acquire);
        while (r != w) {
            const auto& pkt = slot->queue[r & SenderSlot::kIndexMask];
            sockaddr_in destAddr{};
            destAddr.sin_family = AF_INET;
            destAddr.sin_port = htons(activePort);
            destAddr.sin_addr.s_addr = pkt.destIpNetOrder;

            sendto(
                socketFd,
                reinterpret_cast<const char*>(pkt.data),
                static_cast<int>(pkt.length),
                0,
                reinterpret_cast<const sockaddr*>(&destAddr),
                sizeof(destAddr)
            );

            r++;
            slot->readIndex.store(r, std::memory_order_release);
        }
    }
}

void UdpService::Impl::receiverLoop() {
    uint64_t lastTableRefresh = GetTickCount64();

    while (running.load(std::memory_order_acquire)) {
        const uint64_t now = GetTickCount64();
        if (now - lastTableRefresh > 10000) {
            queryLocalNicIps(localNicIps);
            lastTableRefresh = now;
        }

        fd_set readFds;
        FD_ZERO(&readFds);
        FD_SET(socketFd, &readFds);

        timeval tv{};
        tv.tv_sec = 0;
        tv.tv_usec = 50000; // 50 ms timeout

        int sel = select(0, &readFds, nullptr, nullptr, &tv);
        if (sel <= 0) {
            continue;
        }

        if (!FD_ISSET(socketFd, &readFds)) {
            continue;
        }

        alignas(4) uint8_t rawBuf[2048];
        while (running.load(std::memory_order_acquire)) {
            sockaddr_in srcAddr{};
            int fromLen = sizeof(srcAddr);
            int n = recvfrom(
                socketFd,
                reinterpret_cast<char*>(rawBuf),
                sizeof(rawBuf),
                0,
                reinterpret_cast<sockaddr*>(&srcAddr),
                &fromLen
            );

            if (n <= 0) {
                break; // EWOULDBLOCK or error
            }

            // Self-echo filter: drop if source IP matches any non-loopback local NIC IP
            const uint32_t srcIp = static_cast<uint32_t>(srcAddr.sin_addr.s_addr);
            bool isSelfEcho = false;
            for (uint32_t nicIp : localNicIps) {
                if (srcIp == nicIp) {
                    isSelfEcho = true;
                    break;
                }
            }
            if (isSelfEcho) {
                continue;
            }

            Protocol::DecodedAudioPacket decoded{};
            if (!Protocol::decodeAudioPacket(rawBuf, static_cast<size_t>(n), decoded)) {
                continue; // Malformed packet rejected safely
            }

            const uint64_t recvNow = GetTickCount64();
            {
                std::lock_guard<std::mutex> lock(slotsMutex);
                for (ReceiverSlot* slot : receiverSlots) {
                    if (slot != nullptr && slot->matchesIdentifier(decoded.identifier)) {
                        slot->pushDecoded(decoded, recvNow);
                    }
                }
            }
        }
    }
}

UdpService& UdpService::instance() {
    static UdpService sInstance;
    return sInstance;
}

UdpService::UdpService()
    : pImpl_(std::make_unique<Impl>()) {
}

UdpService::~UdpService() {
    if (pImpl_) {
        std::lock_guard<std::mutex> lock(pImpl_->lifecycleMutex);
        if (pImpl_->refCount > 0) {
            pImpl_->refCount = 0;
            pImpl_->stopNetwork();
        }
        std::lock_guard<std::mutex> slotsLock(pImpl_->slotsMutex);
        for (auto* r : pImpl_->receiverSlots) {
            delete r;
        }
        pImpl_->receiverSlots.clear();
        for (auto* s : pImpl_->senderSlots) {
            delete s;
        }
        pImpl_->senderSlots.clear();
    }
}

bool UdpService::acquire() {
    std::lock_guard<std::mutex> lock(pImpl_->lifecycleMutex);
    if (pImpl_->refCount > 0) {
        pImpl_->refCount++;
        return true;
    }

    if (!pImpl_->startNetwork()) {
        return false;
    }

    pImpl_->refCount = 1;
    return true;
}

void UdpService::release() {
    std::lock_guard<std::mutex> lock(pImpl_->lifecycleMutex);
    if (pImpl_->refCount == 0) {
        return;
    }

    pImpl_->refCount--;
    if (pImpl_->refCount == 0) {
        pImpl_->stopNetwork();
    }
}

size_t UdpService::refCount() const noexcept {
    std::lock_guard<std::mutex> lock(pImpl_->lifecycleMutex);
    return pImpl_->refCount;
}

ReceiverSlot* UdpService::createReceiver(const char* identifier) {
    auto* slot = new ReceiverSlot(identifier);
    std::lock_guard<std::mutex> lock(pImpl_->slotsMutex);
    pImpl_->receiverSlots.push_back(slot);
    return slot;
}

void UdpService::destroyReceiver(ReceiverSlot* slot) {
    if (slot == nullptr) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(pImpl_->slotsMutex);
        auto it = std::find(pImpl_->receiverSlots.begin(), pImpl_->receiverSlots.end(), slot);
        if (it != pImpl_->receiverSlots.end()) {
            pImpl_->receiverSlots.erase(it);
        }
    }
    delete slot;
}

void UdpService::setReceiverIdentifier(ReceiverSlot* slot, const char* identifier) {
    if (slot == nullptr) {
        return;
    }
    std::lock_guard<std::mutex> lock(pImpl_->slotsMutex);
    slot->setIdentifier(identifier);
}

SenderSlot* UdpService::createSender() {
    auto* slot = new SenderSlot();
    std::lock_guard<std::mutex> lock(pImpl_->slotsMutex);
    pImpl_->senderSlots.push_back(slot);
    return slot;
}

void UdpService::destroySender(SenderSlot* slot) {
    if (slot == nullptr) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(pImpl_->slotsMutex);
        auto it = std::find(pImpl_->senderSlots.begin(), pImpl_->senderSlots.end(), slot);
        if (it != pImpl_->senderSlots.end()) {
            pImpl_->senderSlots.erase(it);
        }
    }
    delete slot;
}

bool UdpService::tryPushSendAudio(
    SenderSlot* slot,
    const char* identifier,
    uint32_t destIpNetOrder,
    uint8_t channels,
    uint32_t sampleRate,
    uint32_t frames,
    const float* const* channelPtrs
) {
    if (slot == nullptr) {
        return false;
    }
    const uint64_t now = GetTickCount64();
    const bool pushed = slot->pushAudio(
        identifier, destIpNetOrder, channels, sampleRate, frames, channelPtrs, now
    );
    if (pushed && pImpl_->senderWakeupEvent != NULL) {
        SetEvent(pImpl_->senderWakeupEvent);
    }
    return pushed;
}

bool UdpService::tryPopReceivedPacket(ReceiverSlot* slot, ReceivedPacketView& out) {
    if (slot == nullptr) {
        return false;
    }
    return slot->popPacket(out, GetTickCount64());
}

bool UdpService::isRxActive(const ReceiverSlot* slot) const noexcept {
    if (slot == nullptr) {
        return false;
    }
    const uint64_t last = slot->lastRxTickMs.load(std::memory_order_relaxed);
    if (last == 0) {
        return false;
    }
    const uint64_t now = GetTickCount64();
    return (now >= last) && ((now - last) <= 500);
}

bool UdpService::isTxActive(const SenderSlot* slot) const noexcept {
    if (slot == nullptr) {
        return false;
    }
    const uint64_t last = slot->lastTxTickMs.load(std::memory_order_relaxed);
    if (last == 0) {
        return false;
    }
    const uint64_t now = GetTickCount64();
    return (now >= last) && ((now - last) <= 500);
}

void UdpService::setBindPortForTest(uint16_t port) noexcept {
    std::lock_guard<std::mutex> lock(pImpl_->lifecycleMutex);
    if (pImpl_->refCount == 0) {
        pImpl_->bindPortOverride = port;
        pImpl_->hasPortOverride = true;
    }
}

uint16_t UdpService::activePort() const noexcept {
    return pImpl_->activePort;
}

} // namespace StreamAudio
