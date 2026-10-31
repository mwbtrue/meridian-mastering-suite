#include "TruePeakLimiter.h"

#include <algorithm>
#include <cmath>

namespace meridian
{

namespace
{
    constexpr int kControlInterval = 32;
    constexpr double kLn10Over20 = 0.11512925464970228;   // ln(10) / 20

    inline double dbToLin (double db) noexcept   { return std::exp (db * kLn10Over20); }
    inline double linToDb (double g) noexcept    { return 20.0 * std::log10 (std::max (g, 1.0e-12)); }
}

const TruePeakLimiter::StyleSettings& TruePeakLimiter::getStyle (int s) noexcept
{
    static const StyleSettings styles[4] =
    {
        //  st2 att  rel    relMin  relMax  st1 att  st1 rel  warm
        {   0.5,     200.0, 100.0,  400.0,  0.01,    5.0,     false },   // Transparent
        {   0.3,      80.0,  50.0,  160.0,  0.01,    3.0,     false },   // Punchy
        {   1.0,     150.0, 100.0,  300.0,  0.02,    8.0,     true  },   // Warm
        {   0.05,     20.0,  10.0,   60.0,  0.005,   2.0,     false },   // Aggressive
    };

    return styles[std::clamp (s, 0, 3)];
}

TruePeakLimiter::TruePeakLimiter (juce::AudioProcessorValueTreeState& state) : DSPModuleBase (state)
{
    ceilingParam      = rawParam (pid::limCeiling);
    lookaheadParam    = rawParam (pid::limLookahead);
    releaseParam      = rawParam (pid::limRelease);
    autoReleaseParam  = rawParam (pid::limAutoRelease);
    styleParam        = rawParam (pid::limStyle);
    linkParam         = rawParam (pid::limLink);
    oversamplingParam = rawParam (pid::limOversampling);
    stage1Param       = rawParam (pid::limStage1Enabled);
    lowLatencyParam   = rawParam (pid::lowLatencyMode);
}

double TruePeakLimiter::releaseCoeff (double ms) const noexcept
{
    return 1.0 - std::exp (-1.0 / (std::max (0.001, ms) * 1.0e-3 * currentSampleRate));
}

void TruePeakLimiter::prepareModule (double sampleRate, int)
{
    const int maxLookahead = (int) std::ceil (kMaxLookaheadMs * 1.0e-3 * sampleRate);
    const int maxStage1 = (int) std::ceil (kStage1LookaheadMs * 1.0e-3 * sampleRate);

    detector.prepare (16, kDetectorHalfSpan, kDetectorBeta);
    backstopDetector.prepare (kBackstopFactor, kBackstopHalfSpan, kBackstopBeta);
    backstopDetector.setFactor (kBackstopFactor);

    stage1Smoother.prepare (2 * maxStage1 + 4, maxStage1 + 2);

    for (int ch = 0; ch < 2; ++ch)
    {
        stage2Smoother[ch].prepare (2 * maxLookahead + 2 * kDetectorHalfSpan + 4, maxLookahead + 2);
        backstopSmoother[ch].prepare (2 * kBackstopHalfSpan + 4, 2);
        stage1Line[ch].prepare (maxStage1 + 2);
        stage2Line[ch].prepare (maxLookahead + 2 * kDetectorHalfSpan + 2);
        backstopLine[ch].prepare (2 * kBackstopHalfSpan + 2);

        for (auto& bq : lfLowPass[(size_t) ch])
            bq.setCoefficients (Biquad::makeLPF (kLinkCrossoverHz, 0.70710678118654752, sampleRate));
    }

    rmsCoeff = 1.0 - std::exp (-1.0 / (0.2 * sampleRate));
    followerCoeff = 1.0 - std::exp (-double (kControlInterval) / (1.0 * sampleRate));
    lfEnvRelease = std::exp (-1.0 / (0.010 * sampleRate));
    backstopRel = releaseCoeff (50.0);

    cfgStyle = cfgLookahead = cfgStage1 = -1;
    updateParameters();
    configure (true);
}

void TruePeakLimiter::resetModule() noexcept
{
    detector.reset();
    backstopDetector.reset();

    for (int ch = 0; ch < 2; ++ch)
    {
        stage1Line[ch].reset();
        stage2Line[ch].reset();
        backstopLine[ch].reset();
        for (auto& bq : lfLowPass[(size_t) ch])
            bq.reset();
        lfEnv[ch] = 0.0;
        backstopSmoother[ch].reset (1.0);
    }

    rmsPower = 0.0;
    followerDb = -120.0;
    lastGain = 1.0;
    configure (true);
    stage2GrDb.store (0.0f, std::memory_order_relaxed);
    setGainReductionDb (0.0f);
}

void TruePeakLimiter::configure (bool resetState) noexcept
{
    const auto& st = getStyle (style);
    const double fs = currentSampleRate;
    const double initial = resetState ? 1.0 : lastGain;

    // Stage 1: ramp = attack, plateau pads the smoother delay to exactly stage1Lookahead.
    {
        const int k = std::clamp ((int) std::lround (st.stage1AttackMs * 1.0e-3 * fs), 1, stage1Lookahead + 1);
        const int plateau = 2 * (stage1Lookahead - k + 1);
        stage1Smoother.configure (k, plateau, initial);
        stage1Delay = stage1Smoother.getDelay();
        stage1Rel = releaseCoeff (st.stage1ReleaseMs);
    }

    // Stage 2: ramp = attack (<= lookahead + 1), smoother delay = lookahead + halfSpan.
    {
        const int k = std::clamp ((int) std::lround (st.stage2AttackMs * 1.0e-3 * fs), 1, lookaheadSamples + 1);
        const int plateau = 2 * kDetectorHalfSpan + 2 * (lookaheadSamples - k + 1);

        for (auto& s : stage2Smoother)
            s.configure (k, plateau, initial);

        stage2Delay = detector.getLatencySamples() + stage2Smoother[0].getDelay();
    }

    // Backstop: fixed structure, ramp 1, plateau covering the 16x kernel.
    if (resetState)
        for (auto& s : backstopSmoother)
            s.configure (1, 2 * kBackstopHalfSpan, 1.0);

    backstopDelay = backstopDetector.getLatencySamples() + backstopSmoother[0].getDelay();

    cfgStyle = style;
    cfgLookahead = lookaheadSamples;
    cfgStage1 = stage1Lookahead;
}

int TruePeakLimiter::computeLatency() const noexcept
{
    return stage1Delay + stage2Delay + backstopDelay;
}

void TruePeakLimiter::updateParameters() noexcept
{
    const double fs = currentSampleRate;

    ceilingLin = dbToGain (std::clamp ((double) load (ceilingParam), -3.0, 0.0));
    link = std::clamp ((double) load (linkParam) * 0.01, 0.0, 1.0);
    manualReleaseMs = std::clamp ((double) load (releaseParam), 1.0, 2000.0);
    autoRelease = loadBool (autoReleaseParam);
    stage1Enabled = loadBool (stage1Param);
    style = std::clamp (loadInt (styleParam), 0, 3);
    lowLatency = loadBool (lowLatencyParam);

    const int osIndex = std::clamp (loadInt (oversamplingParam), 0, (int) choices::limOversampling.size() - 1);
    detector.setFactor (choices::limOversampling[(size_t) osIndex]);

    const double laMs = std::clamp ((double) load (lookaheadParam), 0.0, kMaxLookaheadMs);
    lookaheadSamples = lowLatency ? 0 : (int) std::lround (laMs * 1.0e-3 * fs);
    stage1Lookahead = lowLatency ? 0 : (int) std::lround (kStage1LookaheadMs * 1.0e-3 * fs);

    if (style != cfgStyle || lookaheadSamples != cfgLookahead || stage1Lookahead != cfgStage1)
        configure (false);

    setLatencySamples (computeLatency());
}

void TruePeakLimiter::processBlock (double* L, double* R, int numSamples) noexcept
{
    const auto& st = getStyle (style);

    const double ceilLin = ceilingLin;
    const double c2 = ceilLin * kStage2Margin;
    const double cb = ceilLin * kBackstopMargin;
    const double thr1Db = linToDb (ceilLin) + kStage1ThresholdOffsetDb;
    const double thr1 = dbToLin (thr1Db);
    constexpr double ratioFactor = 1.0 - 1.0 / 100.0;

    double minG2 = 1.0, minTotal = 1.0;
    double rel2 = releaseCoeff (autoRelease ? currentReleaseMs : manualReleaseMs);
    int control = 0;

    for (int i = 0; i < numSamples; ++i)
    {
        const double xL = L[i], xR = R[i];

        // ---------------- Stage 1: linked transient catcher ----------------
        double r1 = 1.0;
        const double d = std::max (std::abs (xL), std::abs (xR));

        if (stage1Enabled && d > thr1)
        {
            double gr = ratioFactor * (linToDb (d) - thr1Db);
            if (st.warmGr)
                gr = std::tanh (gr * 1.1) / 1.1;
            r1 = dbToLin (-gr);
        }

        const double g1 = stage1Smoother.process (r1, stage1Rel);
        const double sL = stage1Line[0].processSample (xL, stage1Delay) * g1;
        const double sR = stage1Line[1].processSample (xR, stage1Delay) * g1;

        // ---------------- Auto-release programme tracking ----------------
        rmsPower += rmsCoeff * (std::max (sL * sL, sR * sR) - rmsPower);

        if (++control >= kControlInterval)
        {
            control = 0;

            if (autoRelease)
            {
                const double rmsDb = 10.0 * std::log10 (rmsPower + 1.0e-24);
                followerDb += followerCoeff * (rmsDb - followerDb);
                const double trend = std::clamp ((rmsDb - followerDb) / 6.0, -1.0, 1.0);
                currentReleaseMs = std::clamp (st.releaseMs * std::exp2 (-trend), st.releaseMinMs, st.releaseMaxMs);
                rel2 = releaseCoeff (currentReleaseMs);
            }
        }

        // ---------------- Stage 2: true-peak requirement ----------------
        const double tpL = detector.process (0, sL);
        const double tpR = detector.process (1, sR);

        const double lfL = std::abs (lfLowPass[0][1].process (lfLowPass[0][0].process (sL)));
        const double lfR = std::abs (lfLowPass[1][1].process (lfLowPass[1][0].process (sR)));
        lfEnv[0] = std::max (lfL, lfEnv[0] * lfEnvRelease);
        lfEnv[1] = std::max (lfR, lfEnv[1] * lfEnvRelease);

        double rL = tpL > c2 ? c2 / tpL : 1.0;
        double rR = tpR > c2 ? c2 / tpR : 1.0;

        if (rL < 1.0 || rR < 1.0)
        {
            const double rMin = std::min (rL, rR);
            const double bL = link * rMin + (1.0 - link) * rL;
            const double bR = link * rMin + (1.0 - link) * rR;

            // LF share of each channel's reduction (dB), always linked.
            const double grL = -linToDb (rL), grR = -linToDb (rR);
            const double shareL = tpL > 0.0 ? std::clamp (lfEnv[0] / tpL, 0.0, 1.0) : 0.0;
            const double shareR = tpR > 0.0 ? std::clamp (lfEnv[1] / tpR, 0.0, 1.0) : 0.0;
            const double lfLinked = dbToLin (-std::max (grL * shareL, grR * shareR));

            rL = std::min ({ bL, lfLinked, rL });
            rR = std::min ({ bR, lfLinked, rR });
        }

        const double g2L = stage2Smoother[0].process (rL, rel2);
        const double g2R = stage2Smoother[1].process (rR, rel2);
        const double yL = stage2Line[0].processSample (sL, stage2Delay) * g2L;
        const double yR = stage2Line[1].processSample (sR, stage2Delay) * g2R;

        // ---------------- Backstop: independent 16x guarantee ----------------
        const double bpL = backstopDetector.process (0, yL);
        const double bpR = backstopDetector.process (1, yR);
        const double gbL = backstopSmoother[0].process (bpL > cb ? cb / bpL : 1.0, backstopRel);
        const double gbR = backstopSmoother[1].process (bpR > cb ? cb / bpR : 1.0, backstopRel);

        L[i] = std::clamp (backstopLine[0].processSample (yL, backstopDelay) * gbL, -ceilLin, ceilLin);
        R[i] = std::clamp (backstopLine[1].processSample (yR, backstopDelay) * gbR, -ceilLin, ceilLin);

        minG2 = std::min ({ minG2, g2L, g2R });
        minTotal = std::min (minTotal, g1 * std::min (g2L * gbL, g2R * gbR));
    }

    for (auto& ch : lfLowPass)
        for (auto& bq : ch)
            bq.snapToZero();

    lastGain = minG2;
    stage2GrDb.store ((float) std::min (0.0, linToDb (minG2)), std::memory_order_relaxed);
    setGainReductionDb ((float) std::max (0.0, -linToDb (minTotal)));
}

} // namespace meridian
