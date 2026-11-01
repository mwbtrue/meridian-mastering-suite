// Accuracy test for meridian::fastmath against the C runtime.
#include "common/FastMath.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

int main()
{
    namespace fm = meridian::fastmath;
    double expRel = 0.0, logRel = 0.0, tanhAbs = 0.0;

    for (int i = 0; i <= 2000000; ++i)
    {
        const double x = -60.0 + 120.0 * i / 2000000.0;
        expRel = std::max (expRel, std::abs (fm::exp (x) - std::exp (x)) / std::exp (x));
        tanhAbs = std::max (tanhAbs, std::abs (fm::tanh (x) - std::tanh (x)));
        const double small = x * 1.0e-3;
        tanhAbs = std::max (tanhAbs, std::abs (fm::tanh (small) - std::tanh (small)));
    }

    for (int i = 0; i <= 2000000; ++i)
    {
        const double x = std::pow (10.0, -30.0 + 34.0 * i / 2000000.0);
        logRel = std::max (logRel, std::abs (fm::log (x) - std::log (x)) / std::max (1.0, std::abs (std::log (x))));
    }

    const bool exact = fm::exp (0.0) == 1.0 && fm::tanh (0.0) == 0.0 && fm::log (1.0) == 0.0
                       && fm::dbToAmp (0.0) == 1.0;

    std::printf ("fastmath: exp max rel err %.3g, log max err %.3g, tanh max abs err %.3g, exact identities %s\n",
                 expRel, logRel, tanhAbs, exact ? "yes" : "no");

    const bool pass = expRel < 1.0e-14 && logRel < 1.0e-14 && tanhAbs < 1.0e-14 && exact;
    std::printf ("RESULT: %s\n", pass ? "PASS" : "FAIL");
    return pass ? 0 : 1;
}
