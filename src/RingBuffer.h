#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace StreamAudio {

/**
 * @brief Lock-free Single-Producer Single-Consumer (SPSC) ring buffer for float samples.
 *
 * Real-time audio safe:
 * - Zero heap allocations (fixed capacity via std::array).
 * - Zero mutexes or locks (lock-free atomic indices with acquire/release ordering).
 * - Uses power-of-two capacity for fast bitwise modulo operations and full capacity utilization.
 *
 * @note When instantiated with large capacities (such as Capacity >= 16384), instances
 * should be allocated on the heap (e.g. via std::unique_ptr or as a member of a heap-allocated
 * class) rather than as a local stack variable to avoid thread stack overflow.
 */
template<size_t Capacity>
class SpscFloatRing {
    static_assert(Capacity > 0, "Capacity must be greater than zero");
    static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be a power of two");

public:
    SpscFloatRing() noexcept = default;
    ~SpscFloatRing() noexcept = default;

    // SPSC buffers are non-copyable and non-movable
    SpscFloatRing(const SpscFloatRing&) = delete;
    SpscFloatRing& operator=(const SpscFloatRing&) = delete;
    SpscFloatRing(SpscFloatRing&&) = delete;
    SpscFloatRing& operator=(SpscFloatRing&&) = delete;

    /**
     * @brief Pushes up to count floats into the ring buffer.
     * @param src Pointer to source float buffer.
     * @param count Number of floats to push.
     * @return Actual number of floats pushed.
     */
    size_t push(const float* src, size_t count) noexcept {
        if (!src || count == 0) return 0;

        const size_t w = writeIndex_.load(std::memory_order_relaxed);
        const size_t r = readIndex_.load(std::memory_order_acquire);
        const size_t occupied = w - r;

        if (occupied >= Capacity) return 0;
        const size_t toWrite = std::min(count, Capacity - occupied);

        const size_t mask = Capacity - 1;
        const size_t idx = w & mask;
        const size_t firstChunk = std::min(toWrite, Capacity - idx);

        std::memcpy(&buffer_[idx], src, firstChunk * sizeof(float));
        if (toWrite > firstChunk) {
            std::memcpy(&buffer_[0], src + firstChunk, (toWrite - firstChunk) * sizeof(float));
        }

        writeIndex_.store(w + toWrite, std::memory_order_release);
        return toWrite;
    }

    /**
     * @brief Pops up to count floats from the ring buffer.
     * @param dst Pointer to destination float buffer.
     * @param count Number of floats to pop.
     * @return Actual number of floats popped.
     */
    size_t pop(float* dst, size_t count) noexcept {
        if (!dst || count == 0) return 0;

        const size_t r = readIndex_.load(std::memory_order_relaxed);
        const size_t w = writeIndex_.load(std::memory_order_acquire);
        const size_t occupied = w - r;

        if (occupied == 0) return 0;
        const size_t toRead = std::min(count, occupied);

        const size_t mask = Capacity - 1;
        const size_t idx = r & mask;
        const size_t firstChunk = std::min(toRead, Capacity - idx);

        std::memcpy(dst, &buffer_[idx], firstChunk * sizeof(float));
        if (toRead > firstChunk) {
            std::memcpy(dst + firstChunk, &buffer_[0], (toRead - firstChunk) * sizeof(float));
        }

        readIndex_.store(r + toRead, std::memory_order_release);
        return toRead;
    }

    /**
     * @brief Discards up to count floats without copying.
     * @param count Number of floats to discard from the read head.
     */
    void discard(size_t count) noexcept {
        if (count == 0) return;

        const size_t r = readIndex_.load(std::memory_order_relaxed);
        const size_t w = writeIndex_.load(std::memory_order_acquire);
        const size_t occupied = w - r;
        const size_t toDiscard = std::min(count, occupied);

        readIndex_.store(r + toDiscard, std::memory_order_release);
    }

    /**
     * @brief Returns the number of floats available to read.
     */
    size_t availableRead() const noexcept {
        const size_t w = writeIndex_.load(std::memory_order_acquire);
        const size_t r = readIndex_.load(std::memory_order_acquire);
        const size_t diff = w - r;
        return (diff <= Capacity) ? diff : 0;
    }

    /**
     * @brief Returns the free space available to write in floats.
     */
    size_t availableWrite() const noexcept {
        const size_t w = writeIndex_.load(std::memory_order_acquire);
        const size_t r = readIndex_.load(std::memory_order_acquire);
        const size_t diff = w - r;
        return (diff < Capacity) ? (Capacity - diff) : 0;
    }

    /**
     * @brief Resets the buffer to empty state.
     */
    void reset() noexcept {
        readIndex_.store(0, std::memory_order_release);
        writeIndex_.store(0, std::memory_order_release);
    }

private:
    alignas(64) std::atomic<size_t> writeIndex_{0};
    alignas(64) std::atomic<size_t> readIndex_{0};
    std::array<float, Capacity> buffer_{};
};

/**
 * @brief Jitter channel buffer wrapping an SpscFloatRing<131072> (~2.7s @ 48kHz).
 *
 * WARNING: sizeof(JitterChannelBuffer) is ~512 KB.
 * This class must NEVER be allocated as a local variable on the thread stack, as it can
 * cause stack overflow (especially on Windows where default thread stacks are 1 MB).
 * Always heap-allocate instances (e.g. via std::make_unique<JitterChannelBuffer>() or as a
 * member of a heap-allocated service/processor object).
 *
 * Enforces:
 * 1. 2-block initial prebuffer before releasing audio (mitigates network jitter).
 * 2. Automatic trimming of oldest frames on consumer side when buffered frames exceed 64 * blockFrames.
 * 3. Underrun detection: resets prebuffered state on complete underrun so recovery
 *    re-accumulates a 2-block cushion before resuming playback.
 * 4. Strict SPSC thread safety: producer thread calls push() and only mutates writeIndex;
 *    consumer thread calls popForBlock() / discard() and only mutates readIndex.
 */
class JitterChannelBuffer {
public:
    static constexpr size_t RING_CAPACITY = 131072; // Power of two (2^17)

    JitterChannelBuffer() noexcept = default;
    ~JitterChannelBuffer() noexcept = default;

    JitterChannelBuffer(const JitterChannelBuffer&) = delete;
    JitterChannelBuffer& operator=(const JitterChannelBuffer&) = delete;
    JitterChannelBuffer(JitterChannelBuffer&&) = delete;
    JitterChannelBuffer& operator=(JitterChannelBuffer&&) = delete;

    /**
     * @brief Pushes incoming audio frames into the jitter buffer.
     * Producer thread only. Safely pushes up to available capacity without
     * modifying the consumer's readIndex.
     */
    void push(const float* src, size_t count) noexcept {
        if (!src || count == 0) return;
        ring_.push(src, count);
    }

    /**
     * @brief Pops up to blockFrames frames for a DAW/OBS audio block.
     *
     * - Before reaching 2 * blockFrames initially, returns 0.
     * - Once prebuffered, trims buffered samples exceeding 64 * blockFrames.
     * - On complete underrun (0 frames available), resets prebuffered to false.
     *
     * @param dst Destination buffer.
     * @param blockFrames Number of frames requested for the audio block.
     * @return Number of frames actually popped into dst.
     */
    size_t popForBlock(float* dst, size_t blockFrames) noexcept {
        if (!dst || blockFrames == 0) return 0;

        size_t avail = ring_.availableRead();

        // Check if prebuffering condition is met
        if (!prebuffered_) {
            if (avail < 2 * blockFrames) {
                return 0; // Still waiting for 2-block cushion
            }
            prebuffered_ = true;
        }

        // Clamp buffered frames to max 64 * blockFrames by discarding oldest excess
        const size_t maxBuffered = 64 * blockFrames;
        if (avail > maxBuffered) {
            const size_t excess = avail - maxBuffered;
            ring_.discard(excess);
            avail = maxBuffered;
        }

        // Complete underrun check
        if (avail == 0) {
            prebuffered_ = false;
            return 0;
        }

        const size_t toPop = std::min(blockFrames, avail);
        const size_t popped = ring_.pop(dst, toPop);

        // If we drained completely or had a partial underrun, re-arm prebuffering
        if (popped < blockFrames) {
            prebuffered_ = false;
        }

        return popped;
    }

    /**
     * @brief Resets the jitter buffer to empty state and clears prebuffered flag.
     */
    void reset() noexcept {
        ring_.reset();
        prebuffered_ = false;
    }

    /**
     * @brief Returns the number of frames currently buffered in the ring.
     */
    size_t bufferedFrames() const noexcept {
        return ring_.availableRead();
    }

    /**
     * @brief Returns true if the buffer has achieved its initial 2-block prebuffer.
     */
    bool isPrebuffered() const noexcept {
        return prebuffered_;
    }

private:
    SpscFloatRing<RING_CAPACITY> ring_;
    bool prebuffered_{false};
};

} // namespace StreamAudio
