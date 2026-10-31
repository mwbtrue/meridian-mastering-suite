#pragma once

#include <cstdint>

namespace meridian
{

/** Snapshot of all output meters, produced once per processed audio chunk and
    passed audio thread -> UI thread through LockFreeQueue<MeterData, 512>.
    Loudness values follow ITU-R BS.1770-4 / EBU R128. Silence is reported as
    kSilenceDb. gainReductionPerModule is indexed by ModuleId (not slot) and
    holds positive dB of reduction (0 = none). */
struct MeterData
{
    static constexpr int numModules = 12;
    static constexpr float kSilenceDb = -150.0f;

    float momentaryLUFS  = kSilenceDb;   // 400 ms window
    float shortTermLUFS  = kSilenceDb;   // 3 s window
    float integratedLUFS = kSilenceDb;   // gated programme loudness
    float LRA            = 0.0f;         // loudness range, LU
    float truePeak       = kSilenceDb;   // dBTP, max of both channels (4x oversampled)
    float samplePeak     = kSilenceDb;   // dBFS, max of both channels
    float PLR            = 0.0f;         // peak-to-loudness ratio: truePeak - integratedLUFS
    float PSR            = 0.0f;         // peak-to-short-term ratio: truePeak - shortTermLUFS
    float correlation    = 1.0f;         // -1..+1, full band
    float correlationLow = 1.0f;         // -1..+1, below 150 Hz
    float gainReductionPerModule[numModules] {};

    std::int64_t samplePosition = 0;     // running sample counter at end of the chunk
};

/** One magnitude spectrum frame for the display (2048-point FFT, Hann window,
    75 % overlap, bins 0 .. Nyquist). Values in dBFS (a full-scale sine reads
    0 dB), floored at -120 dB.
        magnitude      stereo power average  10 log10((|L|^2 + |R|^2) / 2)
        magnitudeMid   M = (L + R) / 2
        magnitudeSide  S = (L - R) / 2
        peakHold       running max of magnitude, exponential decay (tau = 2 s) */
struct SpectrumFrame
{
    static constexpr int fftOrder = 11;
    static constexpr int fftSize  = 1 << fftOrder;
    static constexpr int numBins  = fftSize / 2 + 1;
    static constexpr float kFloorDb = -120.0f;

    float magnitude[numBins] {};
    float magnitudeMid[numBins] {};
    float magnitudeSide[numBins] {};
    float peakHold[numBins] {};
    double sampleRate = 48000.0;
    std::int64_t samplePosition = 0;
};

} // namespace meridian
