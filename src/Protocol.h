#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string_view>

namespace StreamAudio::Protocol {

// Wire protocol UDP port for ReaStream communication
inline constexpr uint16_t kPort = 58710;

// Header size in bytes: 4 (magic) + 4 (packet_size) + 32 (identifier) + 1 (channels) + 4 (sample_rate) + 2 (data_bytes)
inline constexpr size_t kHeaderSize = 47;

// Maximum audio floats transmitted per UDP packet (<= 1200 bytes payload)
inline constexpr size_t kMaxAudioFloatsPerPacket = 300;

// Identifier buffer size in bytes (31 ASCII characters max + NUL terminator)
inline constexpr size_t kMaxIdentifierBytes = 32;

// Maximum audio channels supported on wire
inline constexpr uint8_t kMaxChannels = 8;

// Decoded representation of an incoming MRSR audio packet
struct DecodedAudioPacket {
    char identifier[32];
    uint8_t channels;
    uint32_t sampleRate;
    uint32_t frameCount;
    // Unaligned payload byte pointer at data + 47; copy via std::memcpy into aligned float buffer.
    const uint8_t* planarSampleBytes;
    // Typed alias to data + 47 matching wire header offset; do not dereference directly when data+47 is unaligned.
    const float* planarSamples;
};

// Target destination category
enum class DestinationKind {
    LocalBroadcast,
    GlobalBroadcast,
    CustomIp
};

/**
 * Calculates the maximum audio frames that can be transmitted in a single UDP packet
 * for the given channel count, adhering to max(1, 300 / channels).
 */
inline uint32_t maxFramesPerPacket(uint8_t channels) noexcept {
    if (channels == 0) {
        return 1;
    }
    uint32_t maxF = static_cast<uint32_t>(kMaxAudioFloatsPerPacket / channels);
    return (maxF < 1) ? 1 : maxF;
}

/**
 * Encodes audio frames into an MRSR packet buffer.
 *
 * Header byte layout (all multi-byte integers in little-endian):
 *   0..3:   "MRSR"
 *   4..7:   uint32_t packet_size (47 + data_bytes)
 *   8..39:  char identifier[32] (zero-padded, <= 31 chars + NUL)
 *   40:     uint8_t channels (1..8)
 *   41..44: uint32_t sample_rate
 *   45..46: uint16_t data_bytes (frames * channels * 4)
 *   47..:   planar IEEE-754 float32 samples (ch0[0..frames-1], ch1[0..frames-1], ...)
 *
 * Returns total bytes written, or 0 on parameter validation failure or insufficient capacity.
 */
inline size_t encodeAudioPacket(
    uint8_t* outBuf,
    size_t outCapacity,
    const char* identifier,
    uint8_t channels,
    uint32_t sampleRate,
    uint32_t frames,
    const float* const* channelPtrs,
    uint32_t frameOffset = 0
) noexcept {
    if (outBuf == nullptr || channelPtrs == nullptr) {
        return 0;
    }
    if (channels == 0 || channels > kMaxChannels) {
        return 0;
    }
    if (sampleRate == 0) {
        return 0;
    }

    const uint64_t totalFloats = static_cast<uint64_t>(frames) * static_cast<uint64_t>(channels);
    if (totalFloats > kMaxAudioFloatsPerPacket) {
        return 0;
    }

    const size_t dataBytes = static_cast<size_t>(totalFloats) * sizeof(float);
    const size_t requiredSize = kHeaderSize + dataBytes;
    if (outCapacity < requiredSize) {
        return 0;
    }

    // 0..3: Magic "MRSR"
    outBuf[0] = 'M';
    outBuf[1] = 'R';
    outBuf[2] = 'S';
    outBuf[3] = 'R';

    // 4..7: uint32_t packet_size (little-endian)
    const uint32_t packetSize = static_cast<uint32_t>(requiredSize);
    outBuf[4] = static_cast<uint8_t>(packetSize & 0xFF);
    outBuf[5] = static_cast<uint8_t>((packetSize >> 8) & 0xFF);
    outBuf[6] = static_cast<uint8_t>((packetSize >> 16) & 0xFF);
    outBuf[7] = static_cast<uint8_t>((packetSize >> 24) & 0xFF);

    // 8..39: char identifier[32] (zero-padded, guaranteed NUL-terminated at index 31)
    std::memset(outBuf + 8, 0, kMaxIdentifierBytes);
    if (identifier != nullptr) {
        size_t idLen = std::strlen(identifier);
        if (idLen > kMaxIdentifierBytes - 1) {
            idLen = kMaxIdentifierBytes - 1;
        }
        std::memcpy(outBuf + 8, identifier, idLen);
    }

    // 40: uint8_t channels
    outBuf[40] = channels;

    // 41..44: uint32_t sample_rate (little-endian)
    outBuf[41] = static_cast<uint8_t>(sampleRate & 0xFF);
    outBuf[42] = static_cast<uint8_t>((sampleRate >> 8) & 0xFF);
    outBuf[43] = static_cast<uint8_t>((sampleRate >> 16) & 0xFF);
    outBuf[44] = static_cast<uint8_t>((sampleRate >> 24) & 0xFF);

    // 45..46: uint16_t data_bytes (little-endian)
    const uint16_t payloadBytes = static_cast<uint16_t>(dataBytes);
    outBuf[45] = static_cast<uint8_t>(payloadBytes & 0xFF);
    outBuf[46] = static_cast<uint8_t>((payloadBytes >> 8) & 0xFF);

    // 47..: Planar float32 samples
    if (frames > 0) {
        const size_t channelBytes = static_cast<size_t>(frames) * sizeof(float);
        for (uint8_t c = 0; c < channels; ++c) {
            uint8_t* chDest = outBuf + kHeaderSize + static_cast<size_t>(c) * channelBytes;
            if (channelPtrs[c] != nullptr) {
                std::memcpy(chDest, channelPtrs[c] + frameOffset, channelBytes);
            } else {
                std::memset(chDest, 0, channelBytes);
            }
        }
    }

    return requiredSize;
}

/**
 * Decodes and validates an incoming MRSR UDP packet.
 *
 * Verifies magic, total length, channel range (1..8), sample rate, data payload
 * byte alignment, and float count limits. Guaranteed safe against out-of-bounds reads.
 */
inline bool decodeAudioPacket(const uint8_t* data, size_t len, DecodedAudioPacket& out) noexcept {
    if (data == nullptr || len < kHeaderSize) {
        return false;
    }

    // Verify magic "MRSR"
    if (data[0] != 'M' || data[1] != 'R' || data[2] != 'S' || data[3] != 'R') {
        return false;
    }

    // Verify packet_size field matches actual datagram length
    const uint32_t packetSize = static_cast<uint32_t>(data[4])
                              | (static_cast<uint32_t>(data[5]) << 8)
                              | (static_cast<uint32_t>(data[6]) << 16)
                              | (static_cast<uint32_t>(data[7]) << 24);
    if (static_cast<size_t>(packetSize) != len) {
        return false;
    }

    // Verify channels within supported range 1..8
    const uint8_t channels = data[40];
    if (channels == 0 || channels > kMaxChannels) {
        return false;
    }

    // Verify positive sample rate
    const uint32_t sampleRate = static_cast<uint32_t>(data[41])
                              | (static_cast<uint32_t>(data[42]) << 8)
                              | (static_cast<uint32_t>(data[43]) << 16)
                              | (static_cast<uint32_t>(data[44]) << 24);
    if (sampleRate == 0) {
        return false;
    }

    // Verify data_bytes equals (len - 47)
    const uint16_t dataBytes = static_cast<uint16_t>(data[45] | (static_cast<uint16_t>(data[46]) << 8));
    if (static_cast<size_t>(dataBytes) != (len - kHeaderSize)) {
        return false;
    }

    // Verify audio payload matches full frame boundary across all channels
    const size_t bytesPerFrame = static_cast<size_t>(channels) * sizeof(float);
    if ((dataBytes % bytesPerFrame) != 0) {
        return false;
    }

    const uint32_t frameCount = static_cast<uint32_t>(dataBytes / bytesPerFrame);
    if (static_cast<uint64_t>(frameCount) * channels > kMaxAudioFloatsPerPacket) {
        return false;
    }

    // Populate output struct
    std::memcpy(out.identifier, data + 8, kMaxIdentifierBytes);
    out.identifier[kMaxIdentifierBytes - 1] = '\0'; // Guarantee NUL termination
    out.channels = channels;
    out.sampleRate = sampleRate;
    out.frameCount = frameCount;
    out.planarSampleBytes = (frameCount > 0) ? (data + kHeaderSize) : nullptr;
    out.planarSamples = (frameCount > 0) ? reinterpret_cast<const float*>(data + kHeaderSize) : nullptr;

    return true;
}

namespace detail {

inline std::string_view trimAsciiWhitespace(std::string_view sv) noexcept {
    while (!sv.empty() && (sv.front() == ' ' || sv.front() == '\t' || sv.front() == '\r' || sv.front() == '\n')) {
        sv.remove_prefix(1);
    }
    while (!sv.empty() && (sv.back() == ' ' || sv.back() == '\t' || sv.back() == '\r' || sv.back() == '\n')) {
        sv.remove_suffix(1);
    }
    return sv;
}

inline bool equalsCaseInsensitive(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) {
        return false;
    }
    for (size_t i = 0; i < a.size(); ++i) {
        char ca = a[i];
        char cb = b[i];
        if (ca >= 'A' && ca <= 'Z') ca = static_cast<char>(ca + ('a' - 'A'));
        if (cb >= 'A' && cb <= 'Z') cb = static_cast<char>(cb + ('a' - 'A'));
        if (ca != cb) {
            return false;
        }
    }
    return true;
}

} // namespace detail

/**
 * Resolves destination text string into DestinationKind and IPv4 address in network byte order.
 *
 * Supported patterns:
 *   - "* local broadcast", "* local" -> LocalBroadcast (127.255.255.255)
 *   - "* broadcast"                  -> GlobalBroadcast (255.255.255.255)
 *   - Strict dotted-quad IPv4        -> CustomIp (a.b.c.d, octets 0..255, no leading zeros unless single '0')
 *
 * Leading and trailing whitespace are trimmed. Returns true if valid, false otherwise.
 */
inline bool resolveDestination(
    std::string_view text,
    DestinationKind& kind,
    uint32_t& ipv4NetworkOrder
) noexcept {
    const std::string_view trimmed = detail::trimAsciiWhitespace(text);
    if (trimmed.empty()) {
        return false;
    }

    if (detail::equalsCaseInsensitive(trimmed, "* local broadcast") ||
        detail::equalsCaseInsensitive(trimmed, "* local")) {
        kind = DestinationKind::LocalBroadcast;
        const uint8_t ip[4] = { 127, 255, 255, 255 };
        std::memcpy(&ipv4NetworkOrder, ip, 4);
        return true;
    }

    if (detail::equalsCaseInsensitive(trimmed, "* broadcast")) {
        kind = DestinationKind::GlobalBroadcast;
        const uint8_t ip[4] = { 255, 255, 255, 255 };
        std::memcpy(&ipv4NetworkOrder, ip, 4);
        return true;
    }

    // Strict IPv4 dotted-quad parsing: a.b.c.d
    uint8_t octets[4] = { 0, 0, 0, 0 };
    size_t octetIdx = 0;
    size_t segStart = 0;

    for (size_t i = 0; i <= trimmed.size(); ++i) {
        if (i == trimmed.size() || trimmed[i] == '.') {
            if (octetIdx >= 4) {
                return false; // More than 4 octets
            }

            const std::string_view part = trimmed.substr(segStart, i - segStart);
            if (part.empty()) {
                return false; // Empty segment (e.g. "..")
            }

            // Disallow leading zeros unless the segment is exactly "0"
            if (part.size() > 1 && part[0] == '0') {
                return false;
            }

            uint32_t val = 0;
            for (char ch : part) {
                if (ch < '0' || ch > '9') {
                    return false; // Non-digit character
                }
                val = val * 10 + static_cast<uint32_t>(ch - '0');
                if (val > 255) {
                    return false; // Octet exceeds 255
                }
            }

            octets[octetIdx++] = static_cast<uint8_t>(val);
            segStart = i + 1;
        }
    }

    if (octetIdx != 4) {
        return false; // Incomplete dotted-quad (fewer than 4 octets)
    }

    kind = DestinationKind::CustomIp;
    std::memcpy(&ipv4NetworkOrder, octets, 4);
    return true;
}

} // namespace StreamAudio::Protocol
