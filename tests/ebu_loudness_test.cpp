// EBU Tech 3341 (loudness meter, "EBU Mode") and EBU Tech 3342 (Loudness Range)
// conformance cases, synthesised in code from the published descriptions.
//
// Tech 3341 tolerances: M / S / I +/-0.1 LU, true peak +0.2/-0.4 dB.
// Tech 3342 tolerance:  LRA +/-1 LU.
// All tone cases use a 1 kHz sine of the stated level on BOTH channels
// (stereo), exactly as the EBU test files are specified.
#include "analysis/MeteringEngine.h"

#include <cmath>
#include <cstdio>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace
{
constexpr double kPi = 3.14159265358979323846;

struct Segment { double dbfs; double seconds; double freq = 1000.0; };

struct Signal { std::vector<double> L, R; };

Signal makeTones (const std::vector<Segment>& segs, double fs)
{
    Signal s;
    double phase = 0.0;
    for (const auto& seg : segs)
    {
        const double a = std::pow (10.0, seg.dbfs / 20.0);
        const auto n = (size_t) std::llround (seg.seconds * fs);
        const double inc = 2.0 * kPi * seg.freq / fs;
        for (size_t i = 0; i < n; ++i)
        {
            const double v = a * std::sin (phase);
            phase += inc;
            if (phase > 2.0 * kPi) phase -= 2.0 * kPi;
            s.L.push_back (v);
            s.R.push_back (v);
        }
    }
    return s;
}

/** Feeds the signal in 512-sample blocks; `probe` is called after every block
    with the elapsed time and the current meter snapshot. */
meridian::MeterData run (const Signal& s, double fs,
                         const std::function<void (double, const meridian::MeterData&)>& probe = {})
{
    meridian::MeteringEngine m;
    m.prepare (fs, 512);
    meridian::MeterData d;
    for (size_t o = 0; o < s.L.size(); o += 512)
    {
        const int len = (int) std::min<size_t> (512, s.L.size() - o);
        m.process (s.L.data() + o, s.R.data() + o, len);
        if (probe)
        {
            m.fillMeterData (d);
            probe (double (o + (size_t) len) / fs, d);
        }
    }
    m.fillMeterData (d);
    return d;
}

int failures = 0;

void check (const std::string& name, double measured, double expected, double tolLo, double tolHi, const char* unit)
{
    const bool pass = measured >= expected - tolLo && measured <= expected + tolHi;
    if (! pass) ++failures;
    std::printf ("  %-58s %8.3f %s (expected %.1f, tol -%.1f/+%.1f) %s\n",
                 name.c_str(), measured, unit, expected, tolLo, tolHi, pass ? "PASS" : "FAIL");
}
} // namespace

int main()
{
    for (double fs : { 48000.0, 44100.0 })
    {
        std::printf ("EBU Tech 3341 / 3342 @ %.0f Hz\n", fs);

        // ---- Tech 3341 case 1: -23 dBFS, 20 s -> M, S, I = -23.0 LUFS --------
        {
            const auto d = run (makeTones ({ { -23.0, 20.0 } }, fs), fs);
            check ("3341-1 -23 dBFS 20 s: Momentary", d.momentaryLUFS, -23.0, 0.1, 0.1, "LUFS");
            check ("3341-1 -23 dBFS 20 s: Short-term", d.shortTermLUFS, -23.0, 0.1, 0.1, "LUFS");
            check ("3341-1 -23 dBFS 20 s: Integrated", d.integratedLUFS, -23.0, 0.1, 0.1, "LUFS");
        }
        // ---- Tech 3341 case 2: -33 dBFS, 20 s -> -33.0 LUFS -----------------
        {
            const auto d = run (makeTones ({ { -33.0, 20.0 } }, fs), fs);
            check ("3341-2 -33 dBFS 20 s: Momentary", d.momentaryLUFS, -33.0, 0.1, 0.1, "LUFS");
            check ("3341-2 -33 dBFS 20 s: Short-term", d.shortTermLUFS, -33.0, 0.1, 0.1, "LUFS");
            check ("3341-2 -33 dBFS 20 s: Integrated", d.integratedLUFS, -33.0, 0.1, 0.1, "LUFS");
        }
        // ---- Tech 3341 case 3: -36 / -23 / -36 dBFS, 10/60/10 s -> I = -23 --
        {
            const auto d = run (makeTones ({ { -36.0, 10.0 }, { -23.0, 60.0 }, { -36.0, 10.0 } }, fs), fs);
            check ("3341-3 -36/-23/-36 dBFS 10/60/10 s: Integrated (rel. gate)", d.integratedLUFS, -23.0, 0.1, 0.1, "LUFS");
        }
        // ---- Tech 3341 case 4: -72/-36/-23/-36/-72 dBFS 10/10/60/10/10 s ----
        {
            const auto d = run (makeTones ({ { -72.0, 10.0 }, { -36.0, 10.0 }, { -23.0, 60.0 },
                                             { -36.0, 10.0 }, { -72.0, 10.0 } }, fs), fs);
            check ("3341-4 -72/-36/-23/-36/-72 dBFS: Integrated (abs. gate)", d.integratedLUFS, -23.0, 0.1, 0.1, "LUFS");
        }
        // ---- Tech 3341 case 5: -26 / -20 / -26 dBFS, 20/20.1/20 s -> I = -23 -
        {
            const auto d = run (makeTones ({ { -26.0, 20.0 }, { -20.0, 20.1 }, { -26.0, 20.0 } }, fs), fs);
            check ("3341-5 -26/-20/-26 dBFS 20/20.1/20 s: Integrated", d.integratedLUFS, -23.0, 0.1, 0.1, "LUFS");
        }
        // ---- Tech 3341 case 9 (short-term constancy): alternating -20 / -30 dBFS
        //      tone bursts of 1.34 s / 1.66 s (3 s period) -> S stays -23.0 once
        //      the 3 s window is full. Checked over 3.1..20 s.
        {
            std::vector<Segment> segs;
            for (int i = 0; i < 7; ++i) { segs.push_back ({ -20.0, 1.34 }); segs.push_back ({ -30.0, 1.66 }); }
            double worst = 0.0;
            run (makeTones (segs, fs), fs, [&] (double t, const meridian::MeterData& d)
            {
                if (t > 3.1 && t < 20.0) worst = std::max (worst, std::abs (d.shortTermLUFS + 23.0));
            });
            check ("3341-9 S constancy (max |S+23|, 3 s bursts)", worst, 0.0, 0.0, 0.1, "LU ");
        }
        // ---- Tech 3341 case 10-ish (momentary constancy): -20 / -30 dBFS
        //      bursts of 0.18 s / 0.22 s (0.4 s period) -> M stays near -23.
        //      The 3341 window position test allows +/-0.1 LU; evaluated over
        //      the steady region.
        {
            std::vector<Segment> segs;
            for (int i = 0; i < 50; ++i) { segs.push_back ({ -20.0, 0.18 }); segs.push_back ({ -30.0, 0.22 }); }
            double sum = 0.0; int count = 0;
            run (makeTones (segs, fs), fs, [&] (double t, const meridian::MeterData& d)
            {
                if (t > 1.0 && t < 19.0) { sum += d.momentaryLUFS; ++count; }
            });
            const double meanM = count > 0 ? sum / count : 0.0;
            // Energy average of the burst pattern is exactly -23.0 LUFS +/- K-weighting at 1 kHz (0 dB).
            const double expected = 10.0 * std::log10 ((0.18 * std::pow (10.0, -2.0) + 0.22 * std::pow (10.0, -3.0)) / 0.4);
            check ("3341-10 M mean over 0.4 s bursts", meanM, expected, 0.1, 0.1, "LUFS");
        }
        // ---- Tech 3341 case 15/16 (true peak): 0 dBFS-peak 1 kHz-ish sines whose
        //      samples avoid the peak. A sine at fs/4 with 45 deg phase has samples
        //      at 0.7071 of its peak: amplitude 1.0 -> TP 0.0 dBTP, sample peak -3.01.
        {
            Signal s;
            const auto n = (size_t) fs;
            for (size_t i = 0; i < n; ++i)
            {
                const double v = 0.5 * std::sin (2.0 * kPi * 0.25 * double (i) + kPi / 4.0);  // TP -6.02 dBTP
                s.L.push_back (v); s.R.push_back (v);
            }
            const auto d = run (s, fs);
            check ("3341-15 fs/4 @ 45 deg, peak -6.02 dBTP: True peak", d.truePeak, -6.02, 0.4, 0.2, "dBTP");
        }

        // ---- Tech 3342 LRA cases -------------------------------------------
        {
            const auto d = run (makeTones ({ { -20.0, 20.0 }, { -30.0, 20.0 } }, fs), fs);
            check ("3342-1 -20/-30 dBFS 20/20 s: LRA", d.LRA, 10.0, 1.0, 1.0, "LU ");
        }
        {
            const auto d = run (makeTones ({ { -20.0, 20.0 }, { -15.0, 20.0 } }, fs), fs);
            check ("3342-2 -20/-15 dBFS 20/20 s: LRA", d.LRA, 5.0, 1.0, 1.0, "LU ");
        }
        {
            const auto d = run (makeTones ({ { -40.0, 20.0 }, { -20.0, 20.0 } }, fs), fs);
            check ("3342-3 -40/-20 dBFS 20/20 s: LRA", d.LRA, 20.0, 1.0, 1.0, "LU ");
        }
        {
            const auto d = run (makeTones ({ { -50.0, 20.0 }, { -35.0, 20.0 }, { -20.0, 20.0 },
                                             { -35.0, 20.0 }, { -50.0, 20.0 } }, fs), fs);
            check ("3342-4 -50/-35/-20/-35/-50 dBFS 20 s each: LRA", d.LRA, 15.0, 1.0, 1.0, "LU ");
        }
    }

    std::printf ("ebu_loudness_test: %s (%d failure%s)\n", failures == 0 ? "PASS" : "FAIL", failures, failures == 1 ? "" : "s");
    return failures == 0 ? 0 : 1;
}
