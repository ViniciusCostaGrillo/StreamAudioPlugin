#include <RingBuffer.h>
#include <Resampler.h>

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <thread>
#include <vector>

namespace {

constexpr double PI = 3.14159265358979323846;

// ---------------------------------------------------------------------------
// 1. SpscFloatRing Tests
// ---------------------------------------------------------------------------

void test_spsc_basic_push_pop_order() {
    std::cout << "[test_dsp_buffers] test_spsc_basic_push_pop_order..." << std::endl;

    StreamAudio::SpscFloatRing<16> ring;
    assert(ring.availableRead() == 0);
    assert(ring.availableWrite() == 16);

    // Push 4 floats
    const float input1[4] = { 1.0f, 2.0f, 3.0f, 4.0f };
    size_t pushed = ring.push(input1, 4);
    assert(pushed == 4);
    assert(ring.availableRead() == 4);
    assert(ring.availableWrite() == 12);

    // Pop 2 floats
    float output[8] = { 0 };
    size_t popped = ring.pop(output, 2);
    assert(popped == 2);
    assert(output[0] == 1.0f);
    assert(output[1] == 2.0f);
    assert(ring.availableRead() == 2);
    assert(ring.availableWrite() == 14);

    // Push 10 floats (2 currently in buffer, 14 free)
    const float input2[10] = { 5.0f, 6.0f, 7.0f, 8.0f, 9.0f, 10.0f, 11.0f, 12.0f, 13.0f, 14.0f };
    pushed = ring.push(input2, 10);
    assert(pushed == 10);
    assert(ring.availableRead() == 12);
    assert(ring.availableWrite() == 4);

    // Pop all 12
    float output2[16] = { 0 };
    popped = ring.pop(output2, 16);
    assert(popped == 12);
    // Elements should be 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14
    for (size_t i = 0; i < 12; ++i) {
        assert(output2[i] == static_cast<float>(i + 3));
    }
    assert(ring.availableRead() == 0);
    assert(ring.availableWrite() == 16);

    std::cout << "  -> passed!" << std::endl;
}

void test_spsc_wraparound_and_order() {
    std::cout << "[test_dsp_buffers] test_spsc_wraparound_and_order..." << std::endl;

    // Use small power-of-two ring to force wraparound many times
    StreamAudio::SpscFloatRing<8> ring;
    assert(ring.availableWrite() == 8);
    assert(ring.availableRead() == 0);

    // Push 6, pop 4 in a loop for 1000 iterations
    float curWrite = 0.0f;
    float curRead = 0.0f;

    for (int iter = 0; iter < 1000; ++iter) {
        float inBuf[6];
        for (int i = 0; i < 6; ++i) {
            inBuf[i] = curWrite++;
        }
        size_t pushed = ring.push(inBuf, 6);
        assert(pushed == 6);

        float outBuf[4];
        size_t popped = ring.pop(outBuf, 4);
        assert(popped == 4);
        for (int i = 0; i < 4; ++i) {
            assert(outBuf[i] == curRead++);
        }

        // Net change per iteration is +2. If occupied > 2, next push of 6 would exceed capacity 8.
        // Drain when availableRead > 2 to keep space for next iteration's push of 6.
        if (ring.availableRead() > 2) {
            float drainBuf[8];
            size_t drained = ring.pop(drainBuf, ring.availableRead());
            for (size_t i = 0; i < drained; ++i) {
                assert(drainBuf[i] == curRead++);
            }
        }
    }

    // Drain remainder
    float drainRem[8];
    size_t remCount = ring.pop(drainRem, ring.availableRead());
    for (size_t i = 0; i < remCount; ++i) {
        assert(drainRem[i] == curRead++);
    }
    assert(ring.availableRead() == 0);
    assert(ring.availableWrite() == 8);

    // Discard test
    float inBuf[8] = { 10, 20, 30, 40, 50, 60, 70, 80 };
    ring.push(inBuf, 8);
    assert(ring.availableRead() == 8);
    assert(ring.availableWrite() == 0);

    ring.discard(3);
    assert(ring.availableRead() == 5);
    assert(ring.availableWrite() == 3);

    float outBuf[5] = { 0 };
    size_t popped = ring.pop(outBuf, 5);
    assert(popped == 5);
    assert(outBuf[0] == 40);
    assert(outBuf[1] == 50);
    assert(outBuf[2] == 60);
    assert(outBuf[3] == 70);
    assert(outBuf[4] == 80);

    // Reset test
    ring.push(inBuf, 4);
    assert(ring.availableRead() == 4);
    ring.reset();
    assert(ring.availableRead() == 0);
    assert(ring.availableWrite() == 8);

    // Edge cases: null pointers and zero counts
    assert(ring.push(nullptr, 4) == 0);
    assert(ring.push(inBuf, 0) == 0);
    assert(ring.pop(nullptr, 4) == 0);
    assert(ring.pop(outBuf, 0) == 0);

    std::cout << "  -> passed!" << std::endl;
}

void test_spsc_threaded_producer_consumer() {
    std::cout << "[test_dsp_buffers] test_spsc_threaded_producer_consumer..." << std::endl;

    constexpr size_t TOTAL_SAMPLES = 500000;
    StreamAudio::SpscFloatRing<1024> ring;

    std::thread producer([&]() {
        size_t sent = 0;
        float chunk[64];
        while (sent < TOTAL_SAMPLES) {
            size_t toSend = std::min<size_t>(64, TOTAL_SAMPLES - sent);
            for (size_t i = 0; i < toSend; ++i) {
                chunk[i] = static_cast<float>(sent + i);
            }
            size_t pushed = ring.push(chunk, toSend);
            sent += pushed;
            if (pushed == 0) {
                std::this_thread::yield();
            }
        }
    });

    std::vector<float> received;
    received.reserve(TOTAL_SAMPLES);

    std::thread consumer([&]() {
        float chunk[64];
        while (received.size() < TOTAL_SAMPLES) {
            size_t popped = ring.pop(chunk, 64);
            for (size_t i = 0; i < popped; ++i) {
                received.push_back(chunk[i]);
            }
            if (popped == 0) {
                std::this_thread::yield();
            }
        }
    });

    producer.join();
    consumer.join();

    assert(received.size() == TOTAL_SAMPLES);
    for (size_t i = 0; i < TOTAL_SAMPLES; ++i) {
        assert(received[i] == static_cast<float>(i));
    }

    std::cout << "  -> passed (" << TOTAL_SAMPLES << " samples verified)!" << std::endl;
}

// ---------------------------------------------------------------------------
// 2. JitterChannelBuffer Tests
// ---------------------------------------------------------------------------

void test_jitter_prebuffer_and_overflow_trim() {
    std::cout << "[test_dsp_buffers] test_jitter_prebuffer_and_overflow_trim..." << std::endl;

    auto jitter = std::make_unique<StreamAudio::JitterChannelBuffer>();
    assert(!jitter->isPrebuffered());
    assert(jitter->bufferedFrames() == 0);

    constexpr size_t BLOCK_FRAMES = 128;
    float block[BLOCK_FRAMES];

    // Push 1st block of 128 frames [0..127]
    for (size_t i = 0; i < BLOCK_FRAMES; ++i) block[i] = static_cast<float>(i);
    jitter->push(block, BLOCK_FRAMES);

    assert(jitter->bufferedFrames() == BLOCK_FRAMES);
    assert(!jitter->isPrebuffered());

    // popForBlock should return 0 (not prebuffered yet, needs 2 blocks)
    float out[BLOCK_FRAMES] = { 0 };
    size_t popped = jitter->popForBlock(out, BLOCK_FRAMES);
    assert(popped == 0);
    assert(!jitter->isPrebuffered());
    assert(jitter->bufferedFrames() == BLOCK_FRAMES);

    // Push 2nd block of 128 frames [128..255]
    for (size_t i = 0; i < BLOCK_FRAMES; ++i) block[i] = static_cast<float>(128 + i);
    jitter->push(block, BLOCK_FRAMES);

    assert(jitter->bufferedFrames() == 2 * BLOCK_FRAMES);
    // Now buffered >= 2 * BLOCK_FRAMES (256 frames)

    // popForBlock should now succeed, set isPrebuffered() = true, and return 1st block [0..127]
    popped = jitter->popForBlock(out, BLOCK_FRAMES);
    assert(popped == BLOCK_FRAMES);
    assert(jitter->isPrebuffered());
    assert(jitter->bufferedFrames() == BLOCK_FRAMES);
    for (size_t i = 0; i < BLOCK_FRAMES; ++i) {
        assert(out[i] == static_cast<float>(i));
    }

    // Next popForBlock returns 2nd block [128..255]
    popped = jitter->popForBlock(out, BLOCK_FRAMES);
    assert(popped == BLOCK_FRAMES);
    assert(jitter->bufferedFrames() == 0);
    for (size_t i = 0; i < BLOCK_FRAMES; ++i) {
        assert(out[i] == static_cast<float>(128 + i));
    }

    // Now buffer is empty (complete underrun)
    // Next popForBlock should return 0 and reset prebuffered_ = false!
    popped = jitter->popForBlock(out, BLOCK_FRAMES);
    assert(popped == 0);
    assert(!jitter->isPrebuffered());

    // Verify underrun recovery re-arms 2-block prebuffer
    jitter->push(block, BLOCK_FRAMES);
    assert(!jitter->isPrebuffered());
    popped = jitter->popForBlock(out, BLOCK_FRAMES);
    assert(popped == 0); // Still needs 2nd block

    jitter->push(block, BLOCK_FRAMES);
    popped = jitter->popForBlock(out, BLOCK_FRAMES);
    assert(popped == BLOCK_FRAMES);
    assert(jitter->isPrebuffered());

    // Drain remaining block so buffer is clean
    popped = jitter->popForBlock(out, BLOCK_FRAMES);
    assert(popped == BLOCK_FRAMES);
    jitter->reset();
    assert(!jitter->isPrebuffered());
    assert(jitter->bufferedFrames() == 0);

    // --- Overflow Trim Test ---
    // Push > 64 * BLOCK_FRAMES (e.g. 10,000 frames)
    // 64 * 128 = 8192 frames.
    // 10,000 frames: values 0 to 9999.
    constexpr size_t OVERFLOW_COUNT = 10000;
    std::vector<float> overflowData(OVERFLOW_COUNT);
    for (size_t i = 0; i < OVERFLOW_COUNT; ++i) {
        overflowData[i] = static_cast<float>(i);
    }
    jitter->push(overflowData.data(), OVERFLOW_COUNT);
    assert(jitter->bufferedFrames() == OVERFLOW_COUNT);

    // On popForBlock(out, BLOCK_FRAMES):
    // Buffer has 10,000 > 64 * 128 (8192).
    // Should discard oldest (10,000 - 8192 = 1808) frames.
    // Retained range is [1808..9999] (8192 frames).
    // Then it pops BLOCK_FRAMES (128 frames): [1808..1935].
    // Remaining in buffer: 8192 - 128 = 8064 frames.
    popped = jitter->popForBlock(out, BLOCK_FRAMES);
    assert(popped == BLOCK_FRAMES);
    assert(jitter->isPrebuffered());
    assert(jitter->bufferedFrames() == 8192 - BLOCK_FRAMES);
    assert(jitter->bufferedFrames() <= 64 * BLOCK_FRAMES);

    for (size_t i = 0; i < BLOCK_FRAMES; ++i) {
        assert(out[i] == static_cast<float>(1808 + i));
    }

    // Safety checks: null dst or 0 frames
    assert(jitter->popForBlock(nullptr, BLOCK_FRAMES) == 0);
    assert(jitter->popForBlock(out, 0) == 0);
    jitter->push(nullptr, 100);
    jitter->push(block, 0);

    std::cout << "  -> passed!" << std::endl;
}

// ---------------------------------------------------------------------------
// 3. LinearResampler Tests
// ---------------------------------------------------------------------------

void test_resampler_exact_passthrough() {
    std::cout << "[test_dsp_buffers] test_resampler_exact_passthrough (48k -> 48k)..." << std::endl;

    StreamAudio::LinearResampler resampler;
    resampler.reset(48000.0, 48000.0);

    constexpr size_t N = 128;
    float in[N];
    for (size_t i = 0; i < N; ++i) in[i] = std::sin(2.0 * PI * 440.0 * i / 48000.0);

    float out[N] = { 0 };
    size_t inConsumed = 0;
    size_t outProduced = resampler.process(in, N, out, N, inConsumed);

    // Exact 1:1 passthrough: inConsumed == N, outProduced == N, exact values, zero 1-sample lag
    assert(inConsumed == N);
    assert(outProduced == N);
    for (size_t i = 0; i < N; ++i) {
        assert(out[i] == in[i]);
    }

    // Second block across boundary
    float in2[N];
    for (size_t i = 0; i < N; ++i) in2[i] = std::sin(2.0 * PI * 440.0 * (N + i) / 48000.0);
    float out2[N] = { 0 };
    outProduced = resampler.process(in2, N, out2, N, inConsumed);
    assert(inConsumed == N);
    assert(outProduced == N);
    for (size_t i = 0; i < N; ++i) {
        assert(out2[i] == in2[i]);
    }

    std::cout << "  -> passed!" << std::endl;
}

void test_resampler_rate_conversion() {
    std::cout << "[test_dsp_buffers] test_resampler_rate_conversion (44.1k -> 48k)..." << std::endl;

    StreamAudio::LinearResampler resampler;
    resampler.reset(44100.0, 48000.0);

    // 1 kHz sine wave at 44.1 kHz input
    // Generate 44100 input samples (1 second)
    constexpr size_t TOTAL_IN = 44100;
    constexpr size_t TOTAL_OUT = 48000;
    std::vector<float> inData(TOTAL_IN);
    for (size_t i = 0; i < TOTAL_IN; ++i) {
        inData[i] = static_cast<float>(std::sin(2.0 * PI * 1000.0 * static_cast<double>(i) / 44100.0));
    }

    // Process through varying block sizes: 64, 128, 512, 73, 117
    const size_t inBlockSizes[] = { 64, 128, 512, 73, 117, 256 };
    const size_t outBlockSizes[] = { 128, 64, 256, 117, 73, 512 };
    constexpr size_t NUM_PATTERNS = 6;

    std::vector<float> resampledOut;
    resampledOut.reserve(TOTAL_OUT + 1024);

    size_t inPos = 0;
    size_t patternIdx = 0;

    while (inPos < TOTAL_IN) {
        size_t inBlock = inBlockSizes[patternIdx % NUM_PATTERNS];
        size_t outBlock = outBlockSizes[patternIdx % NUM_PATTERNS];
        patternIdx++;

        size_t availIn = std::min(inBlock, TOTAL_IN - inPos);
        std::vector<float> tempOut(outBlock);

        size_t inConsumed = 0;
        size_t produced = resampler.process(
            inData.data() + inPos, availIn,
            tempOut.data(), outBlock,
            inConsumed
        );

        inPos += inConsumed;
        for (size_t i = 0; i < produced; ++i) {
            resampledOut.push_back(tempOut[i]);
        }
    }

    std::cout << "  Resampled " << inPos << " inFrames to " << resampledOut.size() << " outFrames." << std::endl;

    // Verify output length is close to theoretical 48000 (within 2 frames due to fractional end)
    assert(resampledOut.size() >= TOTAL_OUT - 2 && resampledOut.size() <= TOTAL_OUT + 2);

    // Calculate Peak Error and RMS Error vs ideal 1 kHz sine wave at 48 kHz
    double peakError = 0.0;
    double sumSqError = 0.0;
    // Skip first 10 samples to let resampler settle if needed
    size_t compareCount = std::min(resampledOut.size(), TOTAL_OUT);
    for (size_t i = 10; i < compareCount; ++i) {
        double ideal = std::sin(2.0 * PI * 1000.0 * static_cast<double>(i) / 48000.0);
        double err = std::abs(static_cast<double>(resampledOut[i]) - ideal);
        if (err > peakError) peakError = err;
        sumSqError += err * err;
    }
    double rmsError = std::sqrt(sumSqError / (compareCount - 10));

    std::cout << "  Peak Error: " << peakError << " (assert < 0.05)" << std::endl;
    std::cout << "  RMS Error:  " << rmsError << " (assert < 0.05)" << std::endl;

    assert(peakError < 0.05);
    assert(rmsError < 0.05);

    std::cout << "  -> passed!" << std::endl;
}

void test_resampler_partial_input_stall_resume() {
    std::cout << "[test_dsp_buffers] test_resampler_partial_input_stall_resume..." << std::endl;

    StreamAudio::LinearResampler resampler;
    resampler.reset(44100.0, 48000.0);

    // Provide 50 frames when 128 requested
    constexpr size_t IN_FRAMES = 50;
    constexpr size_t OUT_REQUESTED = 128;
    float in1[IN_FRAMES];
    for (size_t i = 0; i < IN_FRAMES; ++i) in1[i] = static_cast<float>(i);

    float out1[OUT_REQUESTED] = { 0 };
    size_t inConsumed = 0;
    size_t produced = resampler.process(in1, IN_FRAMES, out1, OUT_REQUESTED, inConsumed);

    // Should consume all 50 frames, producing ~ 50 * (48000/44100) = ~54 samples
    assert(inConsumed == IN_FRAMES);
    assert(produced > 0 && produced < OUT_REQUESTED);

    // Next provide 200 frames (sufficient to satisfy remaining output requested)
    constexpr size_t IN_FRAMES_2 = 200;
    float in2[IN_FRAMES_2];
    for (size_t i = 0; i < IN_FRAMES_2; ++i) in2[i] = static_cast<float>(IN_FRAMES + i);

    float out2[OUT_REQUESTED] = { 0 };
    size_t inConsumed2 = 0;
    size_t produced2 = resampler.process(in2, IN_FRAMES_2, out2, OUT_REQUESTED, inConsumed2);

    assert(produced2 == OUT_REQUESTED);
    assert(inConsumed2 > 0 && inConsumed2 < IN_FRAMES_2);

    // Verify continuity: the transition between out1[produced-1] and out2[0] must be smooth
    float lastVal = out1[produced - 1];
    float firstVal = out2[0];
    float diff = std::abs(firstVal - lastVal);
    // Slope is ~ (44100/48000) ~ 0.92 per sample.
    assert(diff < 1.5f);

    // Invalid rate / safety checks
    resampler.reset(0, 48000);
    assert(resampler.process(in1, 10, out1, 10, inConsumed) == 0);
    resampler.reset(44100, -100);
    assert(resampler.process(in1, 10, out1, 10, inConsumed) == 0);
    resampler.reset(44100, 48000);
    assert(resampler.process(nullptr, 10, out1, 10, inConsumed) == 0);
    assert(resampler.process(in1, 10, nullptr, 10, inConsumed) == 0);
    assert(resampler.process(in1, 0, out1, 10, inConsumed) == 0);
    assert(resampler.process(in1, 10, out1, 0, inConsumed) == 0);

    std::cout << "  -> passed!" << std::endl;
}

} // anonymous namespace

int main() {
    std::cout << "=== Running DSP Buffer and Resampler Tests ===" << std::endl;

    test_spsc_basic_push_pop_order();
    test_spsc_wraparound_and_order();
    test_spsc_threaded_producer_consumer();
    test_jitter_prebuffer_and_overflow_trim();
    test_resampler_exact_passthrough();
    test_resampler_rate_conversion();
    test_resampler_partial_input_stall_resume();

    std::cout << "=== All DSP Buffer and Resampler Tests Passed! ===" << std::endl;
    return 0;
}
