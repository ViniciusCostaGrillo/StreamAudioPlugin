#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>

namespace StreamAudio {

/**
 * @brief High-performance, lock-free, zero-allocation linear resampler.
 *
 * Real-time audio safe:
 * - Zero heap allocations (all state stored inline).
 * - Zero mutexes or locks.
 * - Handles arbitrary sample rates (e.g. 44.1k -> 48k, 48k -> 96k, 48k -> 44.1k).
 * - Stateful across arbitrary block sizes (64, 128, 512, odd sizes):
 *   maintains sub-sample phase continuity with zero clicks across block boundaries.
 * - Exact 1:1 passthrough when srcRate == dstRate (zero 1-sample lag).
 */
class LinearResampler {
public:
    LinearResampler(double srcRate = 48000.0, double dstRate = 48000.0) noexcept {
        reset(srcRate, dstRate);
    }

    ~LinearResampler() noexcept = default;

    /**
     * @brief Resets resampler state with new source and destination sample rates.
     */
    void reset(double srcRate, double dstRate) noexcept {
        srcRate_ = srcRate;
        dstRate_ = dstRate;
        if (srcRate > 0.0 && dstRate > 0.0) {
            step_ = srcRate / dstRate;
        } else {
            step_ = 1.0;
        }
        s0_ = 0.0f;
        s1_ = 0.0f;
        primed_ = 0;
        phase_ = 0.0;
    }

    /**
     * @brief Resets resampler phase and sliding window while retaining configured rates.
     */
    void reset() noexcept {
        reset(srcRate_, dstRate_);
    }

    /**
     * @brief Processes input audio frames and produces resampled output frames.
     *
     * @param in Pointer to input float samples.
     * @param inFrames Number of available input frames.
     * @param out Pointer to output float buffer.
     * @param outFrames Maximum number of output frames requested.
     * @param[out] inConsumed Set to the actual number of input frames consumed.
     * @return Number of output frames generated into out.
     */
    size_t process(const float* in, size_t inFrames,
                   float* out, size_t outFrames,
                   size_t& inConsumed) noexcept {
        inConsumed = 0;
        if (!in || !out || inFrames == 0 || outFrames == 0 || srcRate_ <= 0.0 || dstRate_ <= 0.0) {
            return 0;
        }

        // Exact 1:1 passthrough fast path: zero lag, bitwise exact copying
        if (srcRate_ == dstRate_ && phase_ == 0.0) {
            const size_t count = std::min(inFrames, outFrames);
            std::memcpy(out, in, count * sizeof(float));
            inConsumed = count;
            if (count > 0) {
                s0_ = in[count - 1];
                primed_ = 1;
            }
            return count;
        }

        size_t outProduced = 0;

        while (outProduced < outFrames) {
            // 1. Advance input window if phase_ >= 1.0
            while (phase_ >= 1.0) {
                if (primed_ == 2) {
                    s0_ = s1_;
                    primed_ = 1;
                    phase_ -= 1.0;
                } else if (primed_ == 1) {
                    if (inConsumed < inFrames) {
                        s0_ = in[inConsumed++];
                        primed_ = 1;
                        phase_ -= 1.0;
                    } else {
                        // Need more input frames to advance phase
                        return outProduced;
                    }
                } else { // primed_ == 0
                    if (inConsumed < inFrames) {
                        s0_ = in[inConsumed++];
                        primed_ = 1;
                    } else {
                        return outProduced;
                    }
                }
            }

            // 2. Ensure s0_ is loaded
            if (primed_ == 0) {
                if (inConsumed < inFrames) {
                    s0_ = in[inConsumed++];
                    primed_ = 1;
                } else {
                    return outProduced;
                }
            }

            // 3. Ensure s1_ is loaded if phase_ > 1e-9
            if (phase_ > 1e-9) {
                if (primed_ < 2) {
                    if (inConsumed < inFrames) {
                        s1_ = in[inConsumed++];
                        primed_ = 2;
                    } else {
                        // Need the next sample to interpolate
                        return outProduced;
                    }
                }
            }

            // 4. Emit the interpolated output sample
            const float sampleVal = (primed_ == 2 && phase_ > 1e-9)
                ? (s0_ + static_cast<float>(phase_) * (s1_ - s0_))
                : s0_;
            out[outProduced++] = sampleVal;

            // 5. Advance phase for next output sample
            phase_ += step_;
        }

        return outProduced;
    }

    double sourceRate() const noexcept { return srcRate_; }
    double destinationRate() const noexcept { return dstRate_; }

private:
    double srcRate_{0.0};
    double dstRate_{0.0};
    double step_{1.0};
    float s0_{0.0f};
    float s1_{0.0f};
    int primed_{0};
    double phase_{0.0};
};

} // namespace StreamAudio
