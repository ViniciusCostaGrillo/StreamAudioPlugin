#include <Protocol.h>

#include <cassert>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

void test_encode_layout_matches_mrsr() {
    std::cout << "[test_protocol] Running test_encode_layout_matches_mrsr..." << std::endl;

    const float ch0[4] = { 0.1f, 0.2f, 0.3f, 0.4f };
    const float ch1[4] = { 0.5f, 0.6f, 0.7f, 0.8f };
    const float* const channelPtrs[2] = { ch0, ch1 };

    uint8_t buf[256];
    std::memset(buf, 0xCC, sizeof(buf));

    size_t written = StreamAudio::Protocol::encodeAudioPacket(
        buf, sizeof(buf), "obs", 2, 48000, 4, channelPtrs, 0
    );

    // Expected size: 47 header + (2 channels * 4 frames * 4 bytes) = 79 bytes
    assert(written == 79);

    // Bytes 0..3: magic "MRSR"
    assert(buf[0] == 'M');
    assert(buf[1] == 'R');
    assert(buf[2] == 'S');
    assert(buf[3] == 'R');

    // Bytes 4..7: uint32_t packet_size in little-endian == 79
    uint32_t packetSize = 0;
    std::memcpy(&packetSize, buf + 4, sizeof(uint32_t));
    assert(packetSize == 79);

    // Bytes 8..39: identifier "obs\0..." (zero padded up to 32 bytes)
    assert(std::string(reinterpret_cast<const char*>(buf + 8)) == "obs");
    for (size_t i = 8 + 3; i < 40; ++i) {
        assert(buf[i] == 0);
    }

    // Byte 40: channel count == 2
    assert(buf[40] == 2);

    // Bytes 41..44: uint32_t sample_rate in little-endian == 48000
    uint32_t sampleRate = 0;
    std::memcpy(&sampleRate, buf + 41, sizeof(uint32_t));
    assert(sampleRate == 48000);

    // Bytes 45..46: uint16_t data_bytes in little-endian == 32
    uint16_t dataBytes = 0;
    std::memcpy(&dataBytes, buf + 45, sizeof(uint16_t));
    assert(dataBytes == 32);

    // Bytes 47..78: planar float32 samples (ch0[0..3] then ch1[0..3])
    float readCh0[4];
    float readCh1[4];
    std::memcpy(readCh0, buf + 47, 4 * sizeof(float));
    std::memcpy(readCh1, buf + 47 + 4 * sizeof(float), 4 * sizeof(float));

    for (int i = 0; i < 4; ++i) {
        assert(readCh0[i] == ch0[i]);
        assert(readCh1[i] == ch1[i]);
    }

    // Round-trip decode verification
    StreamAudio::Protocol::DecodedAudioPacket decoded{};
    bool ok = StreamAudio::Protocol::decodeAudioPacket(buf, written, decoded);
    assert(ok);
    assert(std::string(decoded.identifier) == "obs");
    assert(decoded.channels == 2);
    assert(decoded.sampleRate == 48000);
    assert(decoded.frameCount == 4);
    assert(decoded.planarSampleBytes != nullptr);
    assert(decoded.planarSamples != nullptr);
    float decodedSamples[8]{};
    std::memcpy(decodedSamples, decoded.planarSampleBytes, sizeof(decodedSamples));
    for (int i = 0; i < 4; ++i) {
        assert(decodedSamples[i] == ch0[i]);
        assert(decodedSamples[4 + i] == ch1[i]);
    }

    // Also verify non-zero frameOffset
    const float ch0_long[8] = { 0.f, 0.f, 0.f, 0.f, 1.1f, 1.2f, 1.3f, 1.4f };
    const float ch1_long[8] = { 0.f, 0.f, 0.f, 0.f, 2.1f, 2.2f, 2.3f, 2.4f };
    const float* const channelPtrsLong[2] = { ch0_long, ch1_long };

    size_t writtenOffset = StreamAudio::Protocol::encodeAudioPacket(
        buf, sizeof(buf), "offset_test", 2, 44100, 4, channelPtrsLong, 4
    );
    assert(writtenOffset == 79);
    ok = StreamAudio::Protocol::decodeAudioPacket(buf, writtenOffset, decoded);
    assert(ok);
    assert(std::string(decoded.identifier) == "offset_test");
    std::memcpy(decodedSamples, decoded.planarSampleBytes, sizeof(decodedSamples));
    assert(decodedSamples[0] == 1.1f);
    assert(decodedSamples[3] == 1.4f);
    assert(decodedSamples[4] == 2.1f);
    assert(decodedSamples[7] == 2.4f);

    std::cout << "[test_protocol] test_encode_layout_matches_mrsr passed!" << std::endl;
}

void test_chunk_frame_count() {
    std::cout << "[test_protocol] Running test_chunk_frame_count..." << std::endl;

    assert(StreamAudio::Protocol::maxFramesPerPacket(2) == 150);
    assert(StreamAudio::Protocol::maxFramesPerPacket(8) == 37);
    assert(StreamAudio::Protocol::maxFramesPerPacket(0) == 1);

    // Additional boundary and intermediate channel counts
    assert(StreamAudio::Protocol::maxFramesPerPacket(1) == 300);
    assert(StreamAudio::Protocol::maxFramesPerPacket(3) == 100);
    assert(StreamAudio::Protocol::maxFramesPerPacket(4) == 75);
    assert(StreamAudio::Protocol::maxFramesPerPacket(6) == 50);

    // Verify frames * channels <= kMaxAudioFloatsPerPacket (300) for all valid channels
    for (uint8_t ch = 1; ch <= StreamAudio::Protocol::kMaxChannels; ++ch) {
        uint32_t mf = StreamAudio::Protocol::maxFramesPerPacket(ch);
        assert(mf * ch <= StreamAudio::Protocol::kMaxAudioFloatsPerPacket);
        assert(mf >= 1);
    }

    std::cout << "[test_protocol] test_chunk_frame_count passed!" << std::endl;
}

void test_decode_rejects_malformed() {
    std::cout << "[test_protocol] Running test_decode_rejects_malformed..." << std::endl;

    // Create a valid baseline packet
    const float ch0[4] = { 0.1f, 0.2f, 0.3f, 0.4f };
    const float ch1[4] = { 0.5f, 0.6f, 0.7f, 0.8f };
    const float* const channelPtrs[2] = { ch0, ch1 };

    uint8_t validBuf[128];
    size_t validLen = StreamAudio::Protocol::encodeAudioPacket(
        validBuf, sizeof(validBuf), "valid", 2, 48000, 4, channelPtrs, 0
    );
    assert(validLen == 79);

    StreamAudio::Protocol::DecodedAudioPacket out{};

    // Null buffer
    assert(!StreamAudio::Protocol::decodeAudioPacket(nullptr, validLen, out));

    // Length < 47 bytes
    assert(!StreamAudio::Protocol::decodeAudioPacket(validBuf, 0, out));
    assert(!StreamAudio::Protocol::decodeAudioPacket(validBuf, 46, out));
    assert(!StreamAudio::Protocol::decodeAudioPacket(validBuf, 20, out));

    // Wrong magic
    {
        uint8_t badMagic[79];
        std::memcpy(badMagic, validBuf, 79);

        // Lowercase "mRSR"
        badMagic[0] = 'm';
        assert(!StreamAudio::Protocol::decodeAudioPacket(badMagic, 79, out));

        // Totally wrong "XXXX"
        badMagic[0] = 'X'; badMagic[1] = 'X'; badMagic[2] = 'X'; badMagic[3] = 'X';
        assert(!StreamAudio::Protocol::decodeAudioPacket(badMagic, 79, out));
    }

    // packet_size != len
    {
        uint8_t badSize[79];
        std::memcpy(badSize, validBuf, 79);

        // Header says 80, but actual len is 79
        badSize[4] = 80;
        assert(!StreamAudio::Protocol::decodeAudioPacket(badSize, 79, out));

        // Header says 78, but actual len is 79
        badSize[4] = 78;
        assert(!StreamAudio::Protocol::decodeAudioPacket(badSize, 79, out));

        // Header says 0
        badSize[4] = 0; badSize[5] = 0; badSize[6] = 0; badSize[7] = 0;
        assert(!StreamAudio::Protocol::decodeAudioPacket(badSize, 79, out));
    }

    // channels == 0
    {
        uint8_t badCh[79];
        std::memcpy(badCh, validBuf, 79);
        badCh[40] = 0;
        assert(!StreamAudio::Protocol::decodeAudioPacket(badCh, 79, out));
    }

    // channels > 8
    {
        uint8_t badCh[79];
        std::memcpy(badCh, validBuf, 79);
        badCh[40] = 9;
        assert(!StreamAudio::Protocol::decodeAudioPacket(badCh, 79, out));
        badCh[40] = 255;
        assert(!StreamAudio::Protocol::decodeAudioPacket(badCh, 79, out));
    }

    // sampleRate == 0
    {
        uint8_t badRate[79];
        std::memcpy(badRate, validBuf, 79);
        badRate[41] = 0; badRate[42] = 0; badRate[43] = 0; badRate[44] = 0;
        assert(!StreamAudio::Protocol::decodeAudioPacket(badRate, 79, out));
    }

    // data_bytes != (len - 47)
    {
        uint8_t badDataBytes[79];
        std::memcpy(badDataBytes, validBuf, 79);
        badDataBytes[45] = 30; // 30 != (79 - 47 = 32)
        assert(!StreamAudio::Protocol::decodeAudioPacket(badDataBytes, 79, out));
    }

    // data_bytes % (channels * 4) != 0
    // channels = 2 -> 8 bytes per frame. If data_bytes = 28, 28 % 8 != 0.
    {
        uint8_t unaligned[75]; // 47 + 28 = 75
        std::memcpy(unaligned, validBuf, 47);
        // set packet_size to 75
        unaligned[4] = 75; unaligned[5] = 0; unaligned[6] = 0; unaligned[7] = 0;
        unaligned[40] = 2; // channels = 2
        // set data_bytes to 28
        unaligned[45] = 28; unaligned[46] = 0;
        assert(!StreamAudio::Protocol::decodeAudioPacket(unaligned, 75, out));
    }

    // Payload exceeding kMaxAudioFloatsPerPacket (300 floats)
    // 301 floats * 4 bytes = 1204 bytes payload -> total packet size = 1251
    {
        std::vector<uint8_t> largePkt(StreamAudio::Protocol::kHeaderSize + 1204, 0);
        largePkt[0] = 'M'; largePkt[1] = 'R'; largePkt[2] = 'S'; largePkt[3] = 'R';
        uint32_t sz = static_cast<uint32_t>(largePkt.size());
        std::memcpy(&largePkt[4], &sz, 4);
        largePkt[40] = 1; // 1 channel -> 301 frames
        uint32_t sr = 48000;
        std::memcpy(&largePkt[41], &sr, 4);
        uint16_t db = 1204;
        std::memcpy(&largePkt[45], &db, 2);

        assert(!StreamAudio::Protocol::decodeAudioPacket(largePkt.data(), largePkt.size(), out));
    }

    std::cout << "[test_protocol] test_decode_rejects_malformed passed!" << std::endl;
}

void test_destination_resolution() {
    std::cout << "[test_protocol] Running test_destination_resolution..." << std::endl;

    StreamAudio::Protocol::DestinationKind kind;
    uint32_t ipNet = 0;

    // "* local broadcast" -> LocalBroadcast (127.255.255.255)
    assert(StreamAudio::Protocol::resolveDestination("* local broadcast", kind, ipNet));
    assert(kind == StreamAudio::Protocol::DestinationKind::LocalBroadcast);
    {
        const uint8_t* b = reinterpret_cast<const uint8_t*>(&ipNet);
        assert(b[0] == 127 && b[1] == 255 && b[2] == 255 && b[3] == 255);
    }

    // Leading and trailing whitespace on "* local broadcast"
    assert(StreamAudio::Protocol::resolveDestination("  * local broadcast \t ", kind, ipNet));
    assert(kind == StreamAudio::Protocol::DestinationKind::LocalBroadcast);

    // Case-insensitivity
    assert(StreamAudio::Protocol::resolveDestination("* Local Broadcast", kind, ipNet));
    assert(kind == StreamAudio::Protocol::DestinationKind::LocalBroadcast);

    // "* local" -> LocalBroadcast
    assert(StreamAudio::Protocol::resolveDestination("* local", kind, ipNet));
    assert(kind == StreamAudio::Protocol::DestinationKind::LocalBroadcast);
    assert(StreamAudio::Protocol::resolveDestination("  * local  ", kind, ipNet));
    assert(kind == StreamAudio::Protocol::DestinationKind::LocalBroadcast);

    // "* broadcast" -> GlobalBroadcast (255.255.255.255)
    assert(StreamAudio::Protocol::resolveDestination("* broadcast", kind, ipNet));
    assert(kind == StreamAudio::Protocol::DestinationKind::GlobalBroadcast);
    {
        const uint8_t* b = reinterpret_cast<const uint8_t*>(&ipNet);
        assert(b[0] == 255 && b[1] == 255 && b[2] == 255 && b[3] == 255);
    }
    assert(StreamAudio::Protocol::resolveDestination(" \r\n * Broadcast \t", kind, ipNet));
    assert(kind == StreamAudio::Protocol::DestinationKind::GlobalBroadcast);

    // Custom IPv4 "192.168.1.50"
    assert(StreamAudio::Protocol::resolveDestination("192.168.1.50", kind, ipNet));
    assert(kind == StreamAudio::Protocol::DestinationKind::CustomIp);
    {
        const uint8_t* b = reinterpret_cast<const uint8_t*>(&ipNet);
        assert(b[0] == 192 && b[1] == 168 && b[2] == 1 && b[3] == 50);
    }

    // Whitespace trimming on custom IPv4
    assert(StreamAudio::Protocol::resolveDestination("  10.0.0.1  \n", kind, ipNet));
    assert(kind == StreamAudio::Protocol::DestinationKind::CustomIp);
    {
        const uint8_t* b = reinterpret_cast<const uint8_t*>(&ipNet);
        assert(b[0] == 10 && b[1] == 0 && b[2] == 0 && b[3] == 1);
    }

    // Boundaries of IPv4
    assert(StreamAudio::Protocol::resolveDestination("0.0.0.0", kind, ipNet));
    assert(kind == StreamAudio::Protocol::DestinationKind::CustomIp);
    {
        const uint8_t* b = reinterpret_cast<const uint8_t*>(&ipNet);
        assert(b[0] == 0 && b[1] == 0 && b[2] == 0 && b[3] == 0);
    }

    assert(StreamAudio::Protocol::resolveDestination("255.255.255.255", kind, ipNet));
    assert(kind == StreamAudio::Protocol::DestinationKind::CustomIp);
    {
        const uint8_t* b = reinterpret_cast<const uint8_t*>(&ipNet);
        assert(b[0] == 255 && b[1] == 255 && b[2] == 255 && b[3] == 255);
    }

    // Invalid IP rejections
    assert(!StreamAudio::Protocol::resolveDestination("invalid-ip", kind, ipNet));
    assert(!StreamAudio::Protocol::resolveDestination("", kind, ipNet));
    assert(!StreamAudio::Protocol::resolveDestination("   ", kind, ipNet));
    assert(!StreamAudio::Protocol::resolveDestination("256.0.0.1", kind, ipNet));
    assert(!StreamAudio::Protocol::resolveDestination("1.2.3", kind, ipNet));
    assert(!StreamAudio::Protocol::resolveDestination("1.2.3.4.5", kind, ipNet));
    assert(!StreamAudio::Protocol::resolveDestination("1.2.3.4abc", kind, ipNet));
    assert(!StreamAudio::Protocol::resolveDestination("1.2.3.", kind, ipNet));
    assert(!StreamAudio::Protocol::resolveDestination(".1.2.3", kind, ipNet));
    assert(!StreamAudio::Protocol::resolveDestination("1..2.3", kind, ipNet));
    assert(!StreamAudio::Protocol::resolveDestination("01.2.3.4", kind, ipNet));
    assert(!StreamAudio::Protocol::resolveDestination("1.02.3.4", kind, ipNet));
    assert(!StreamAudio::Protocol::resolveDestination("-1.2.3.4", kind, ipNet));
    assert(!StreamAudio::Protocol::resolveDestination("+1.2.3.4", kind, ipNet));
    assert(!StreamAudio::Protocol::resolveDestination("1.2.3.256", kind, ipNet));
    assert(!StreamAudio::Protocol::resolveDestination("192.168.1. 1", kind, ipNet));

    std::cout << "[test_protocol] test_destination_resolution passed!" << std::endl;
}

void test_encode_edge_cases() {
    std::cout << "[test_protocol] Running test_encode_edge_cases..." << std::endl;

    const float ch0[4] = { 1.f, 2.f, 3.f, 4.f };
    const float* const channelPtrs[2] = { ch0, nullptr }; // ch1 is null

    uint8_t buf[256];

    // Identifier > 31 chars should truncate cleanly to 31 chars with NUL terminator
    const char* longId = "abcdefghijklmnopqrstuvwxyz0123456789"; // 36 chars
    size_t written = StreamAudio::Protocol::encodeAudioPacket(
        buf, sizeof(buf), longId, 2, 48000, 4, channelPtrs, 0
    );
    assert(written == 79);

    // Byte 39 must be 0
    assert(buf[39] == 0);

    StreamAudio::Protocol::DecodedAudioPacket decoded{};
    bool ok = StreamAudio::Protocol::decodeAudioPacket(buf, written, decoded);
    assert(ok);
    assert(std::string(decoded.identifier) == "abcdefghijklmnopqrstuvwxyz01234"); // 31 chars
    assert(decoded.identifier[31] == '\0');

    // Null channelPtrs[1] should be zero-filled
    float edgeSamples[8]{};
    std::memcpy(edgeSamples, decoded.planarSampleBytes, sizeof(edgeSamples));
    assert(edgeSamples[0] == 1.f);
    assert(edgeSamples[1] == 2.f);
    assert(edgeSamples[4] == 0.f); // ch1 frame 0
    assert(edgeSamples[5] == 0.f); // ch1 frame 1

    // Capacity too small
    assert(StreamAudio::Protocol::encodeAudioPacket(buf, 46, "test", 2, 48000, 4, channelPtrs, 0) == 0);
    assert(StreamAudio::Protocol::encodeAudioPacket(buf, 78, "test", 2, 48000, 4, channelPtrs, 0) == 0);

    // Invalid parameters
    assert(StreamAudio::Protocol::encodeAudioPacket(nullptr, sizeof(buf), "test", 2, 48000, 4, channelPtrs, 0) == 0);
    assert(StreamAudio::Protocol::encodeAudioPacket(buf, sizeof(buf), "test", 0, 48000, 4, channelPtrs, 0) == 0);
    assert(StreamAudio::Protocol::encodeAudioPacket(buf, sizeof(buf), "test", 9, 48000, 4, channelPtrs, 0) == 0);
    assert(StreamAudio::Protocol::encodeAudioPacket(buf, sizeof(buf), "test", 2, 0, 4, channelPtrs, 0) == 0);
    assert(StreamAudio::Protocol::encodeAudioPacket(buf, sizeof(buf), "test", 2, 48000, 4, nullptr, 0) == 0);

    // Null identifier should encode as empty string without crash
    written = StreamAudio::Protocol::encodeAudioPacket(
        buf, sizeof(buf), nullptr, 1, 48000, 4, channelPtrs, 0
    );
    assert(written == 47 + 16);
    ok = StreamAudio::Protocol::decodeAudioPacket(buf, written, decoded);
    assert(ok);
    assert(std::string(decoded.identifier) == "");

    // Frames overflow (> 300 floats)
    // 2 channels * 151 frames = 302 floats > 300
    assert(StreamAudio::Protocol::encodeAudioPacket(buf, sizeof(buf), "test", 2, 48000, 151, channelPtrs, 0) == 0);

    // Max frames (2 channels * 150 frames = 300 floats) fits
    std::vector<float> bigCh0(150, 0.5f);
    const float* const bigPtrs[1] = { bigCh0.data() };
    std::vector<uint8_t> bigBuf(StreamAudio::Protocol::kHeaderSize + 300 * sizeof(float));
    written = StreamAudio::Protocol::encodeAudioPacket(
        bigBuf.data(), bigBuf.size(), "max_test", 1, 48000, 300, bigPtrs, 0
    );
    assert(written == bigBuf.size());
    ok = StreamAudio::Protocol::decodeAudioPacket(bigBuf.data(), written, decoded);
    assert(ok);
    assert(decoded.frameCount == 300);

    std::cout << "[test_protocol] test_encode_edge_cases passed!" << std::endl;
}

} // namespace

int main() {
    std::cout << "=== Running Protocol Tests ===" << std::endl;
    test_encode_layout_matches_mrsr();
    test_chunk_frame_count();
    test_decode_rejects_malformed();
    test_destination_resolution();
    test_encode_edge_cases();
    std::cout << "=== All Protocol Tests Passed! ===" << std::endl;
    return 0;
}
