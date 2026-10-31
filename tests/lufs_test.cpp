// BS.1770-4 accuracy: a 1 kHz sine at -23 dBFS on both channels reads -23.0 LUFS.
#include "analysis/MeteringEngine.h"

#include <cmath>
#include <cstdio>
#include <vector>

int main()
{
    const double fs = 48000.0, pi = 3.14159265358979323846;
    const int n = 480000;
    const double a = std::pow (10.0, -23.0 / 20.0);

    std::vector<double> x ((size_t) n);
    for (int i = 0; i < n; ++i)
        x[(size_t) i] = a * std::sin (2.0 * pi * 1000.0 * i / fs);

    bool allPass = true;
    for (double rate : { 48000.0, 44100.0, 96000.0 })
    {
        std::vector<double> y ((size_t) (n * rate / fs));
        for (size_t i = 0; i < y.size(); ++i)
            y[i] = a * std::sin (2.0 * pi * 1000.0 * double (i) / rate);

        meridian::MeteringEngine m;
        m.prepare (rate, 512);
        for (size_t o = 0; o < y.size(); o += 512)
            m.process (y.data() + o, y.data() + o, (int) std::min<size_t> (512, y.size() - o));

        meridian::MeterData d;
        m.fillMeterData (d);
        const bool pass = std::abs (d.integratedLUFS + 23.0f) <= 0.2f;
        allPass = allPass && pass;
        std::printf ("lufs_test @ %.0f Hz: integrated %.3f LUFS, momentary %.3f, short-term %.3f, TP %.3f dBTP, LRA %.2f LU -> %s\n",
                     rate, d.integratedLUFS, d.momentaryLUFS, d.shortTermLUFS, d.truePeak, d.LRA, pass ? "PASS" : "FAIL");
    }
    return allPass ? 0 : 1;
}
