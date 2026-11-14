#include <ConfigStore.h>
#include <Protocol.h>
#include <UdpService.h>

#include <atomic>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace {

void test_config_store_roundtrip() {
    std::cout << "[test_udp_service] Running test_config_store_roundtrip..." << std::endl;

    std::filesystem::path tempDir = std::filesystem::temp_directory_path() / "stream_audio_test_config_store";
    std::error_code ec;
    std::filesystem::remove_all(tempDir, ec);

    const std::string baseDir = tempDir.string();

    // Verify initial load from empty directory returns empty vector
    auto initialIps = StreamAudio::ConfigStore::loadRecentIps(baseDir);
    assert(initialIps.empty());

    // Verify getIniPath contains expected directory and file name
    std::string iniPath = StreamAudio::ConfigStore::getIniPath(baseDir);
    assert(iniPath.find("StreamAudioPlugin") != std::string::npos);
    assert(iniPath.find("recent.ini") != std::string::npos);

    // Save 12 entries: valid IPs, an invalid "bad", a broadcast "* broadcast", and a duplicate
    StreamAudio::ConfigStore::saveRecentIp("192.168.1.10", baseDir);
    StreamAudio::ConfigStore::saveRecentIp("192.168.1.11", baseDir);
    StreamAudio::ConfigStore::saveRecentIp("bad_ip", baseDir);             // Invalid: should be ignored
    StreamAudio::ConfigStore::saveRecentIp("* broadcast", baseDir);        // Broadcast: not a CustomIp, ignored
    StreamAudio::ConfigStore::saveRecentIp("192.168.1.12", baseDir);
    StreamAudio::ConfigStore::saveRecentIp("192.168.1.13", baseDir);
    StreamAudio::ConfigStore::saveRecentIp("192.168.1.14", baseDir);
    StreamAudio::ConfigStore::saveRecentIp("192.168.1.15", baseDir);
    StreamAudio::ConfigStore::saveRecentIp("192.168.1.16", baseDir);
    StreamAudio::ConfigStore::saveRecentIp("192.168.1.17", baseDir);
    StreamAudio::ConfigStore::saveRecentIp("192.168.1.18", baseDir);
    StreamAudio::ConfigStore::saveRecentIp("192.168.1.19", baseDir);
    StreamAudio::ConfigStore::saveRecentIp("192.168.1.20", baseDir);
    // Duplicate of 192.168.1.10: should move to front (MRU order)
    StreamAudio::ConfigStore::saveRecentIp("192.168.1.10", baseDir);

    auto loadedIps = StreamAudio::ConfigStore::loadRecentIps(baseDir);

    // Exactly 10 IPs retained
    assert(loadedIps.size() == 10);

    // Most recent is the duplicated IP moved to front
    assert(loadedIps[0] == "192.168.1.10");
    assert(loadedIps[1] == "192.168.1.20");
    assert(loadedIps[2] == "192.168.1.19");
    assert(loadedIps[3] == "192.168.1.18");
    assert(loadedIps[4] == "192.168.1.17");
    assert(loadedIps[5] == "192.168.1.16");
    assert(loadedIps[6] == "192.168.1.15");
    assert(loadedIps[7] == "192.168.1.14");
    assert(loadedIps[8] == "192.168.1.13");
    assert(loadedIps[9] == "192.168.1.12");

    // Verify 192.168.1.11 was truncated out
    for (const auto& ip : loadedIps) {
        assert(ip != "192.168.1.11");
        assert(ip != "bad_ip");
        assert(ip != "* broadcast");
    }

    // Clean up temp directory
    std::filesystem::remove_all(tempDir, ec);
    std::cout << "  -> test_config_store_roundtrip passed!" << std::endl;
}

void test_udp_loopback_send_and_receive_routing() {
    std::cout << "[test_udp_service] Running test_udp_loopback_send_and_receive_routing..." << std::endl;

    // Verify default protocol port constant is 58710
    static_assert(StreamAudio::Protocol::kPort == 58710, "Protocol port must be 58710");

    auto& service = StreamAudio::UdpService::instance();

    // Use ephemeral port (0) for unit test isolation
    service.setBindPortForTest(0);

    bool acquired = service.acquire();
    assert(acquired);
    assert(service.refCount() == 1);

    uint16_t activePort = service.activePort();
    assert(activePort > 0);
    std::cout << "  (bound to ephemeral test port: " << activePort << ")" << std::endl;

    // Create 1 sender and 2 receivers with different identifiers
    auto* sender = service.createSender();
    assert(sender != nullptr);

    auto* rxA = service.createReceiver("busA");
    auto* rxB = service.createReceiver("busB");
    assert(rxA != nullptr);
    assert(rxB != nullptr);

    // Initial activity checks
    assert(!service.isTxActive(sender));
    assert(!service.isRxActive(rxA));
    assert(!service.isRxActive(rxB));

    // Target loopback: 127.0.0.1
    StreamAudio::Protocol::DestinationKind kind;
    uint32_t destIpNet = 0;
    bool destResolved = StreamAudio::Protocol::resolveDestination("127.0.0.1", kind, destIpNet);
    assert(destResolved);
    assert(kind == StreamAudio::Protocol::DestinationKind::CustomIp);

    // Prepare 2-channel, 256-frame audio block
    constexpr uint32_t kFrames = 256;
    constexpr uint8_t kChannels = 2;
    constexpr uint32_t kSampleRate = 48000;

    std::vector<float> ch0(kFrames);
    std::vector<float> ch1(kFrames);
    for (uint32_t i = 0; i < kFrames; ++i) {
        ch0[i] = static_cast<float>(i) * 0.001f;
        ch1[i] = -static_cast<float>(i) * 0.001f;
    }
    const float* const channelPtrs[2] = { ch0.data(), ch1.data() };

    // Push audio directed to "busA"
    bool pushed = service.tryPushSendAudio(
        sender, "busA", destIpNet, kChannels, kSampleRate, kFrames, channelPtrs
    );
    assert(pushed);

    // Wait up to 500 ms for reception and verify chunking
    // Note: 2 channels -> maxFramesPerPacket = 150 frames.
    // 256 frames are chunked into 150 + 106 frames across 2 MRSR UDP packets.
    uint32_t totalFramesPoppedA = 0;
    std::vector<float> receivedCh0;
    std::vector<float> receivedCh1;
    receivedCh0.reserve(kFrames);
    receivedCh1.reserve(kFrames);

    const auto startTime = std::chrono::steady_clock::now();
    while (std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now() - startTime).count() < 500) {
        StreamAudio::ReceivedPacketView packetView{};
        while (service.tryPopReceivedPacket(rxA, packetView)) {
            assert(packetView.sampleRate == kSampleRate);
            assert(packetView.channels == kChannels);
            assert(packetView.frameCount > 0);
            assert(packetView.planarSamples != nullptr);

            // Verify planar layout: [ch0 frames][ch1 frames]
            const float* p0 = packetView.planarSamples;
            const float* p1 = packetView.planarSamples + packetView.frameCount;

            for (uint32_t f = 0; f < packetView.frameCount; ++f) {
                receivedCh0.push_back(p0[f]);
                receivedCh1.push_back(p1[f]);
            }
            totalFramesPoppedA += packetView.frameCount;
        }

        if (totalFramesPoppedA >= kFrames) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    assert(totalFramesPoppedA == kFrames);
    assert(receivedCh0.size() == kFrames);
    assert(receivedCh1.size() == kFrames);

    for (uint32_t i = 0; i < kFrames; ++i) {
        assert(std::fabs(receivedCh0[i] - ch0[i]) < 1e-6f);
        assert(std::fabs(receivedCh1[i] - ch1[i]) < 1e-6f);
    }

    // Verify rxB received zero packets (isolated identifier)
    StreamAudio::ReceivedPacketView packetViewB{};
    bool poppedB = service.tryPopReceivedPacket(rxB, packetViewB);
    assert(!poppedB);

    // Verify activity indicators
    assert(service.isTxActive(sender));
    assert(service.isRxActive(rxA));
    assert(!service.isRxActive(rxB));

    // Cleanup slots and release service
    service.destroyReceiver(rxA);
    service.destroyReceiver(rxB);
    service.destroySender(sender);

    service.release();
    assert(service.refCount() == 0);

    std::cout << "  -> test_udp_loopback_send_and_receive_routing passed!" << std::endl;
}

void test_default_port_and_8ch_loopback() {
    std::cout << "[test_udp_service] Running test_default_port_and_8ch_loopback..." << std::endl;

    auto& service = StreamAudio::UdpService::instance();
    // Default activePort is 58710 before any override
    assert(service.activePort() == StreamAudio::Protocol::kPort);

    // Acquire on ephemeral port for multi-channel test
    service.setBindPortForTest(0);
    assert(service.acquire());

    auto* sender = service.createSender();
    auto* rx = service.createReceiver("surround8");
    assert(sender && rx);

    StreamAudio::Protocol::DestinationKind kind;
    uint32_t destIpNet = 0;
    StreamAudio::Protocol::resolveDestination("127.0.0.1", kind, destIpNet);

    constexpr uint8_t kChannels = 8;
    constexpr uint32_t kFrames = 512;
    constexpr uint32_t kSampleRate = 96000;

    std::vector<std::vector<float>> channels(kChannels, std::vector<float>(kFrames));
    std::vector<const float*> ptrs(kChannels);
    for (uint8_t c = 0; c < kChannels; ++c) {
        for (uint32_t f = 0; f < kFrames; ++f) {
            channels[c][f] = static_cast<float>(c * 1000 + f) * 0.0001f;
        }
        ptrs[c] = channels[c].data();
    }

    bool pushed = service.tryPushSendAudio(
        sender, "surround8", destIpNet, kChannels, kSampleRate, kFrames, ptrs.data()
    );
    assert(pushed);

    uint32_t totalFrames = 0;
    std::vector<std::vector<float>> receivedCh(kChannels);
    for (uint8_t c = 0; c < kChannels; ++c) {
        receivedCh[c].reserve(kFrames);
    }

    const auto start = std::chrono::steady_clock::now();
    while (std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now() - start).count() < 500) {
        StreamAudio::ReceivedPacketView packet{};
        while (service.tryPopReceivedPacket(rx, packet)) {
            assert(packet.sampleRate == kSampleRate);
            assert(packet.channels == kChannels);
            for (uint8_t c = 0; c < kChannels; ++c) {
                const float* p = packet.planarSamples + c * packet.frameCount;
                for (uint32_t f = 0; f < packet.frameCount; ++f) {
                    receivedCh[c].push_back(p[f]);
                }
            }
            totalFrames += packet.frameCount;
        }
        if (totalFrames >= kFrames) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    assert(totalFrames == kFrames);
    for (uint8_t c = 0; c < kChannels; ++c) {
        assert(receivedCh[c].size() == kFrames);
        for (uint32_t f = 0; f < kFrames; ++f) {
            assert(std::fabs(receivedCh[c][f] - channels[c][f]) < 1e-6f);
        }
    }

    service.destroyReceiver(rx);
    service.destroySender(sender);
    service.release();

    // Verify reset to default port 58710
    assert(service.activePort() == StreamAudio::Protocol::kPort);

    std::cout << "  -> test_default_port_and_8ch_loopback passed!" << std::endl;
}

void test_multi_acquire_release_and_rapid_id_change() {
    std::cout << "[test_udp_service] Running test_multi_acquire_release_and_rapid_id_change..." << std::endl;

    auto& service = StreamAudio::UdpService::instance();
    service.setBindPortForTest(0);

    // Acquire twice: verifies reference count management
    assert(service.acquire());
    assert(service.refCount() == 1);
    assert(service.acquire());
    assert(service.refCount() == 2);

    auto* rx = service.createReceiver("initId");
    auto* tx = service.createSender();
    assert(rx != nullptr && tx != nullptr);

    StreamAudio::Protocol::DestinationKind kind;
    uint32_t loopbackNet = 0;
    StreamAudio::Protocol::resolveDestination("127.0.0.1", kind, loopbackNet);

    std::atomic<bool> stopWorker{false};
    std::atomic<uint64_t> sentBlocks{0};
    std::atomic<uint64_t> poppedBlocks{0};

    // Sender thread pushing traffic under various IDs
    std::thread senderWorker([&]() {
        const float s0[32] = { 0.1f };
        const float s1[32] = { 0.2f };
        const float* const ptrs[2] = { s0, s1 };

        uint64_t counter = 0;
        while (!stopWorker.load(std::memory_order_relaxed)) {
            const char* targetId = (counter % 3 == 0) ? "idA" : (counter % 3 == 1) ? "idB" : "idC";
            service.tryPushSendAudio(tx, targetId, loopbackNet, 2, 48000, 32, ptrs);
            sentBlocks.fetch_add(1, std::memory_order_relaxed);
            counter++;
            std::this_thread::yield();
        }
    });

    // Consumer thread popping from rx
    std::thread receiverWorker([&]() {
        while (!stopWorker.load(std::memory_order_relaxed)) {
            StreamAudio::ReceivedPacketView view{};
            if (service.tryPopReceivedPacket(rx, view)) {
                poppedBlocks.fetch_add(1, std::memory_order_relaxed);
            }
            std::this_thread::yield();
        }
    });

    // Main thread rapidly alternates receiver identifier across IDs
    const auto start = std::chrono::steady_clock::now();
    uint64_t idChanges = 0;
    while (std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now() - start).count() < 150) {
        service.setReceiverIdentifier(rx, (idChanges % 2 == 0) ? "idA" : "idB");
        idChanges++;
        std::this_thread::yield();
    }

    stopWorker.store(true, std::memory_order_release);
    senderWorker.join();
    receiverWorker.join();

    std::cout << "  (completed " << idChanges << " ID changes, "
              << sentBlocks.load() << " sent blocks, "
              << poppedBlocks.load() << " received blocks with zero deadlocks/crashes)"
              << std::endl;

    service.destroyReceiver(rx);
    service.destroySender(tx);

    // Release twice
    service.release();
    assert(service.refCount() == 1);
    service.release();
    assert(service.refCount() == 0);

    std::cout << "  -> test_multi_acquire_release_and_rapid_id_change passed!" << std::endl;
}

} // namespace

int main() {
    std::cout << "=== Running UDP Service & Config Store Tests ===" << std::endl;
    test_config_store_roundtrip();
    test_udp_loopback_send_and_receive_routing();
    test_default_port_and_8ch_loopback();
    test_multi_acquire_release_and_rapid_id_change();
    std::cout << "=== All UDP Service & Config Store Tests Passed! ===" << std::endl;
    return 0;
}
