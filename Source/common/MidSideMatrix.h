#pragma once

namespace meridian::MidSide
{

/** M = (L + R) / 2,  S = (L - R) / 2  — unity-gain, perfectly invertible. */
constexpr inline void encodeMidSide (double L, double R, double& M, double& S) noexcept
{
    M = (L + R) * 0.5;
    S = (L - R) * 0.5;
}

/** L = M + S,  R = M - S */
constexpr inline void decodeMidSide (double M, double S, double& L, double& R) noexcept
{
    L = M + S;
    R = M - S;
}

/** In-place block encode: on return L holds Mid and R holds Side. */
constexpr inline void encodeStereoBlock (double* L, double* R, int numSamples) noexcept
{
    for (int i = 0; i < numSamples; ++i)
    {
        const double l = L[i];
        const double r = R[i];
        L[i] = (l + r) * 0.5;
        R[i] = (l - r) * 0.5;
    }
}

/** In-place block decode: on return M holds Left and S holds Right. */
constexpr inline void decodeStereoBlock (double* M, double* S, int numSamples) noexcept
{
    for (int i = 0; i < numSamples; ++i)
    {
        const double m = M[i];
        const double s = S[i];
        M[i] = m + s;
        S[i] = m - s;
    }
}

} // namespace meridian::MidSide
