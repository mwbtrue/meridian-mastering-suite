#include "MeteringEngine.h"

#include <algorithm>
#include <cmath>

namespace meridian
{

namespace
{
    constexpr double kPi = 3.14159265358979323846;

    float toDb (double lin) noexcept
    {
        return lin > 0.0 ? (float) std::max (-150.0, 20.0 * std::log10 (lin)) : MeterData::kSilenceDb;
    }
}

std::array<BiquadCoeffs, 2> MeteringEngine::kWeighting (double sampleRate) noexcept
{
    if (std::abs (sampleRate - 48000.0) < 1.0e-6)
    {
        return { BiquadCoeffs { 1.53512485958697, -2.69169618940638, 1.19839281085285, -1.69065929318241, 0.73248077421585 },
                 BiquadCoeffs { 1.0, -2.0, 1.0, -1.99004745483398, 0.99007225036459 } };
    }

    std::array<BiquadCoeffs, 2> c;

    {   // Pre-filter: 2nd-order high shelf.
        const double f0 = 1681.974450955533, G = 3.999843853973347, Q = 0.7071752369554196;
        const double K = std::tan (kPi * f0 / sampleRate);
        const double Vh = std::pow (10.0, G / 20.0);
        const double Vb = std::pow (Vh, 0.4996667741545416);
        const double a0 = 1.0 + K / Q + K * K;
        c[0] = { (Vh + Vb * K / Q + K * K) / a0, 2.0 * (K * K - Vh) / a0, (Vh - Vb * K / Q + K * K) / a0,
                 2.0 * (K * K - 1.0) / a0, (1.0 - K / Q + K * K) / a0 };
    }

    {   // RLB high-pass (numerator 1, -2, 1 as in BS.1770).
        const double f0 = 38.13547087602444, Q = 0.5003270373238773;
        const double K = std::tan (kPi * f0 / sampleRate);
        const double a0 = 1.0 + K / Q + K * K;
        c[1] = { 1.0, -2.0, 1.0, 2.0 * (K * K - 1.0) / a0, (1.0 - K / Q + K * K) / a0 };
    }

    return c;
}

double MeteringEngine::energyToLufs (double e) noexcept
{
    return e > 0.0 ? -0.691 + 10.0 * std::log10 (e) : -1000.0;
}

void MeteringEngine::prepare (double sampleRate, int)
{
    fs = sampleRate > 0.0 ? sampleRate : 48000.0;
    subBlockLength = std::max (1, (int) std::lround (0.1 * fs));

    const auto kw = kWeighting (fs);
    for (int s = 0; s < 2; ++s)
    {
        kwL[(size_t) s].setCoefficients (kw[(size_t) s]);
        kwR[(size_t) s].setCoefficients (kw[(size_t) s]);
    }

    lowL.setCoefficients (Biquad::makeLPF (150.0, 0.70710678118654752, fs));
    lowR.setCoefficients (Biquad::makeLPF (150.0, 0.70710678118654752, fs));
    corrCoeff = 1.0 - std::exp (-1.0 / (0.3 * fs));

    const auto intBins = (size_t) std::lround ((kHistMax - kAbsoluteGate) / kIntegratedBinLu);
    const auto lraBins = (size_t) std::lround ((kHistMax - kAbsoluteGate) / kLraBinLu);
    intEnergy.assign (intBins, 0.0);
    intCount.assign (intBins, 0);
    lraEnergy.assign (lraBins, 0.0);
    lraCount.assign (lraBins, 0);

    tpDetector.prepare (4, 12, 8.0);
    tpDetector.setFactor (4);

    reset();
}

void MeteringEngine::clearIntegrated() noexcept
{
    std::fill (intEnergy.begin(), intEnergy.end(), 0.0);
    std::fill (intCount.begin(), intCount.end(), 0u);
    std::fill (lraEnergy.begin(), lraEnergy.end(), 0.0);
    std::fill (lraCount.begin(), lraCount.end(), 0u);
    intTotalEnergy = 0.0;
    intTotalCount = 0;
    lraCounter = 0;
    integrated.store (MeterData::kSilenceDb, std::memory_order_relaxed);
    lra.store (0.0f, std::memory_order_relaxed);
    plr.store (0.0f, std::memory_order_relaxed);
}

void MeteringEngine::clearPeaks() noexcept
{
    truePeakHold = samplePeakHold = 0.0;
    truePeakDb.store (MeterData::kSilenceDb, std::memory_order_relaxed);
    samplePeakDb.store (MeterData::kSilenceDb, std::memory_order_relaxed);
}

void MeteringEngine::reset() noexcept
{
    for (auto* b : { &kwL[0], &kwL[1], &kwR[0], &kwR[1], &lowL, &lowR })
        b->reset();

    tpDetector.reset();
    subEnergy.fill (0.0);
    subTruePeak.fill (0.0);
    subWrite = subFilled = subBlockCount = 0;
    subSum = subPeak = 0.0;
    sLR = sLL = sRR = lLR = lLL = lRR = 0.0;

    clearIntegrated();
    clearPeaks();
    momentary.store (MeterData::kSilenceDb, std::memory_order_relaxed);
    shortTerm.store (MeterData::kSilenceDb, std::memory_order_relaxed);
    psr.store (0.0f, std::memory_order_relaxed);
    correlation.store (1.0f, std::memory_order_relaxed);
    correlationLow.store (1.0f, std::memory_order_relaxed);
    integratedResetFlag.store (false);
    peakResetFlag.store (false);
}

void MeteringEngine::process (const double* left, const double* right, int numSamples) noexcept
{
    if (integratedResetFlag.exchange (false, std::memory_order_acq_rel))
    {
        clearIntegrated();
        clearPeaks();
    }

    if (peakResetFlag.exchange (false, std::memory_order_acq_rel))
        clearPeaks();

    for (int i = 0; i < numSamples; ++i)
    {
        const double l = left[i], r = right[i];

        // K-weighted energy (sum of channel mean squares).
        const double kl = kwL[1].process (kwL[0].process (l));
        const double kr = kwR[1].process (kwR[0].process (r));
        subSum += kl * kl + kr * kr;

        // Peaks.
        samplePeakHold = std::max ({ samplePeakHold, std::abs (l), std::abs (r) });
        const double tp = std::max (tpDetector.process (0, l), tpDetector.process (1, r));
        subPeak = std::max (subPeak, tp);

        // Correlation (full band and < 150 Hz).
        sLR += corrCoeff * (l * r - sLR);
        sLL += corrCoeff * (l * l - sLL);
        sRR += corrCoeff * (r * r - sRR);
        const double ll = lowL.process (l), lr = lowR.process (r);
        lLR += corrCoeff * (ll * lr - lLR);
        lLL += corrCoeff * (ll * ll - lLL);
        lRR += corrCoeff * (lr * lr - lRR);

        if (++subBlockCount >= subBlockLength)
            finishSubBlock();
    }

    for (auto* b : { &kwL[0], &kwL[1], &kwR[0], &kwR[1], &lowL, &lowR })
        b->snapToZero();

    auto corr = [] (double xy, double xx, double yy)
    {
        const double den = xx * yy;
        return den > 1.0e-24 ? (float) std::clamp (xy / std::sqrt (den), -1.0, 1.0) : 1.0f;
    };

    correlation.store (corr (sLR, sLL, sRR), std::memory_order_relaxed);
    correlationLow.store (corr (lLR, lLL, lRR), std::memory_order_relaxed);
    samplePeakDb.store (toDb (samplePeakHold), std::memory_order_relaxed);
    truePeakDb.store (toDb (std::max (truePeakHold, subPeak)), std::memory_order_relaxed);
}

void MeteringEngine::finishSubBlock() noexcept
{
    subEnergy[(size_t) subWrite] = subSum / double (subBlockCount);
    subTruePeak[(size_t) subWrite] = subPeak;
    truePeakHold = std::max (truePeakHold, subPeak);
    subWrite = (subWrite + 1) % kSubBlocks;
    subFilled = std::min (subFilled + 1, kSubBlocks);
    subSum = 0.0;
    subPeak = 0.0;
    subBlockCount = 0;

    auto meanOfLast = [this] (int count)
    {
        double e = 0.0;
        for (int k = 1; k <= count; ++k)
            e += subEnergy[(size_t) ((subWrite - k + kSubBlocks) % kSubBlocks)];
        return e / double (count);
    };

    // Momentary + gating block (400 ms, 75 % overlap).
    if (subFilled >= 4)
    {
        const double e = meanOfLast (4);
        const double lufs = energyToLufs (e);
        momentary.store ((float) std::max (-150.0, lufs), std::memory_order_relaxed);

        if (lufs >= kAbsoluteGate)
        {
            const int bin = std::clamp ((int) ((lufs - kAbsoluteGate) / kIntegratedBinLu), 0, (int) intCount.size() - 1);
            intEnergy[(size_t) bin] += e;
            ++intCount[(size_t) bin];
            intTotalEnergy += e;
            ++intTotalCount;
        }

        computeIntegrated();
    }

    // Short-term (3 s; reported over the available history until 3 s have elapsed).
    const double eShort = meanOfLast (subFilled);
    const double stLufs = energyToLufs (eShort);
    shortTerm.store ((float) std::max (-150.0, stLufs), std::memory_order_relaxed);

    double tp3 = 0.0;
    for (int k = 0; k < subFilled; ++k)
        tp3 = std::max (tp3, subTruePeak[(size_t) k]);
    psr.store (stLufs > -150.0 && tp3 > 0.0 ? (float) (20.0 * std::log10 (tp3) - stLufs) : 0.0f, std::memory_order_relaxed);

    // LRA uses complete 3 s windows only.
    if (subFilled >= kSubBlocks && stLufs >= kAbsoluteGate)
    {
        const int bin = std::clamp ((int) ((stLufs - kAbsoluteGate) / kLraBinLu), 0, (int) lraCount.size() - 1);
        lraEnergy[(size_t) bin] += eShort;
        ++lraCount[(size_t) bin];
    }

    if (++lraCounter >= 10)
    {
        lraCounter = 0;
        computeLra();
    }
}

void MeteringEngine::computeIntegrated() noexcept
{
    if (intTotalCount == 0)
        return;

    const double relGate = energyToLufs (intTotalEnergy / double (intTotalCount)) - 10.0;
    const int firstBin = std::max (0, (int) std::ceil ((relGate - kAbsoluteGate) / kIntegratedBinLu - 1.0e-9));

    double e = 0.0;
    std::uint64_t n = 0;
    for (size_t b = (size_t) firstBin; b < intCount.size(); ++b)
    {
        e += intEnergy[b];
        n += intCount[b];
    }

    if (n == 0)
        return;

    const double value = energyToLufs (e / double (n));
    integrated.store ((float) value, std::memory_order_relaxed);
    plr.store (truePeakHold > 0.0 ? (float) (20.0 * std::log10 (truePeakHold) - value) : 0.0f, std::memory_order_relaxed);
}

void MeteringEngine::computeLra() noexcept
{
    double eSum = 0.0;
    std::uint64_t nSum = 0;
    for (size_t b = 0; b < lraCount.size(); ++b)
    {
        eSum += lraEnergy[b];
        nSum += lraCount[b];
    }

    if (nSum == 0)
        return;

    const double relGate = energyToLufs (eSum / double (nSum)) - 20.0;
    const int firstBin = std::max (0, (int) std::ceil ((relGate - kAbsoluteGate) / kLraBinLu - 1.0e-9));

    std::uint64_t n = 0;
    for (size_t b = (size_t) firstBin; b < lraCount.size(); ++b)
        n += lraCount[b];

    if (n == 0)
        return;

    auto percentile = [&] (double p)
    {
        const auto target = (std::uint64_t) std::ceil (p * double (n));
        std::uint64_t acc = 0;
        for (size_t b = (size_t) firstBin; b < lraCount.size(); ++b)
        {
            acc += lraCount[b];
            if (acc >= std::max<std::uint64_t> (1, target))
                return kAbsoluteGate + (double (b) + 0.5) * kLraBinLu;
        }
        return kHistMax;
    };

    lra.store ((float) std::max (0.0, percentile (0.95) - percentile (0.10)), std::memory_order_relaxed);
}

void MeteringEngine::fillMeterData (MeterData& out) const noexcept
{
    out.momentaryLUFS  = momentary.load (std::memory_order_relaxed);
    out.shortTermLUFS  = shortTerm.load (std::memory_order_relaxed);
    out.integratedLUFS = integrated.load (std::memory_order_relaxed);
    out.LRA            = lra.load (std::memory_order_relaxed);
    out.truePeak       = truePeakDb.load (std::memory_order_relaxed);
    out.samplePeak     = samplePeakDb.load (std::memory_order_relaxed);
    out.PLR            = plr.load (std::memory_order_relaxed);
    out.PSR            = psr.load (std::memory_order_relaxed);
    out.correlation    = correlation.load (std::memory_order_relaxed);
    out.correlationLow = correlationLow.load (std::memory_order_relaxed);
}

} // namespace meridian
