#pragma once

// Real-input FFT for the partitioned convolution engines (single precision).
//
// A real transform of size N is computed as one complex FFT of size N/2
// (even samples in the real part, odd samples in the imaginary part) followed
// by the standard split step, so it costs about half of a full complex FFT.
// The complex FFT is an iterative radix-2 decimation-in-time transform with
// precomputed bit-reversal and twiddle tables.
//
// Spectrum layout ("split"): re[0..N/2] followed by im[0..N/2], i.e. 2 * (N/2 + 1)
// floats. Keeping real and imaginary parts in separate runs lets the
// per-bin complex multiply-accumulate loops vectorise on every compiler.
//
// inverse() is the exact inverse of forward() (it includes the 1/N scale).
// prepare() allocates; forward() / inverse() are allocation-free and
// real-time safe.

#include <cmath>
#include <vector>

namespace meridian
{
class RealFFT
{
public:
    void prepare (int sizeN)
    {
        N = sizeN;
        M = N / 2;
        log2M = 0;
        while ((1 << log2M) < M)
            ++log2M;

        bitRev.assign ((size_t) M, 0);
        for (int i = 0; i < M; ++i)
        {
            int r = 0;
            for (int b = 0; b < log2M; ++b)
                if ((i >> b) & 1)
                    r |= 1 << (log2M - 1 - b);
            bitRev[(size_t) i] = r;
        }

        // Complex-FFT twiddles exp(-2 pi i j / M), j < M/2.
        twRe.assign ((size_t) std::max (1, M / 2), 0.0f);
        twIm.assign ((size_t) std::max (1, M / 2), 0.0f);
        for (int j = 0; j < M / 2; ++j)
        {
            const double a = -2.0 * 3.14159265358979323846 * j / M;
            twRe[(size_t) j] = (float) std::cos (a);
            twIm[(size_t) j] = (float) std::sin (a);
        }

        // Split-step twiddles exp(-2 pi i k / N), k <= M.
        wRe.assign ((size_t) M + 1, 0.0f);
        wIm.assign ((size_t) M + 1, 0.0f);
        for (int k = 0; k <= M; ++k)
        {
            const double a = -2.0 * 3.14159265358979323846 * k / N;
            wRe[(size_t) k] = (float) std::cos (a);
            wIm[(size_t) k] = (float) std::sin (a);
        }

        zRe.assign ((size_t) M, 0.0f);
        zIm.assign ((size_t) M, 0.0f);
    }

    int getSize() const noexcept     { return N; }
    int getNumBins() const noexcept  { return M + 1; }

    /** in: N real samples. out: split spectrum, 2 * (N/2 + 1) floats. */
    void forward (const float* in, float* out) noexcept
    {
        for (int n = 0; n < M; ++n)
        {
            const int r = bitRev[(size_t) n];
            zRe[(size_t) r] = in[2 * n];
            zIm[(size_t) r] = in[2 * n + 1];
        }

        complexFFT();

        float* Xr = out;
        float* Xi = out + (M + 1);

        Xr[0] = zRe[0] + zIm[0];  Xi[0] = 0.0f;
        Xr[M] = zRe[0] - zIm[0];  Xi[M] = 0.0f;

        for (int k = 1; k < M; ++k)
        {
            const float ar = zRe[(size_t) k],     ai = zIm[(size_t) k];
            const float br = zRe[(size_t) (M - k)], bi = -zIm[(size_t) (M - k)];   // conj(Z[M-k])
            const float er = 0.5f * (ar + br), ei = 0.5f * (ai + bi);              // Fe
            const float dr = 0.5f * (ar - br), di = 0.5f * (ai - bi);              // (Z - conj) / 2
            // Fo = -i * d ; X = Fe + W^k * Fo
            const float or_ = di, oi = -dr;
            const float wr = wRe[(size_t) k], wi = wIm[(size_t) k];
            Xr[k] = er + (wr * or_ - wi * oi);
            Xi[k] = ei + (wr * oi + wi * or_);
        }
    }

    /** in: split spectrum, 2 * (N/2 + 1) floats. out: N real samples. */
    void inverse (const float* in, float* out) noexcept
    {
        const float* Xr = in;
        const float* Xi = in + (M + 1);

        // Z[k] = Fe + i Fo, Fe = (X[k] + conj X[M-k]) / 2, Fo = (X[k] - conj X[M-k]) / 2 * conj(W^k).
        // The complex inverse is done as conj(FFT(conj(Z))) / M, so conj(Z) is
        // written straight into bit-reversed order.
        for (int k = 0; k < M; ++k)
        {
            const float ar = Xr[k],       ai = Xi[k];
            const float br = Xr[M - k],   bi = -Xi[M - k];
            const float er = 0.5f * (ar + br), ei = 0.5f * (ai + bi);
            const float dr = 0.5f * (ar - br), di = 0.5f * (ai - bi);
            const float wr = wRe[(size_t) k], wi = -wIm[(size_t) k];   // conj(W^k)
            const float fr = dr * wr - di * wi, fi = dr * wi + di * wr;
            // Z = Fe + i * Fo
            const float zr = er - fi, zi = ei + fr;
            const int r = bitRev[(size_t) k];
            zRe[(size_t) r] = zr;
            zIm[(size_t) r] = -zi;
        }

        complexFFT();

        const float scale = 1.0f / (float) M;
        for (int n = 0; n < M; ++n)
        {
            out[2 * n]     = zRe[(size_t) n] * scale;
            out[2 * n + 1] = -zIm[(size_t) n] * scale;
        }
    }

private:
    /** In-place forward FFT of zRe/zIm, input already in bit-reversed order. */
    void complexFFT() noexcept
    {
        float* re = zRe.data();
        float* im = zIm.data();

        // Stage 1 (len 2): twiddle 1.
        for (int i = 0; i < M; i += 2)
        {
            const float tr = re[i + 1], ti = im[i + 1];
            re[i + 1] = re[i] - tr;  im[i + 1] = im[i] - ti;
            re[i] += tr;             im[i] += ti;
        }

        for (int len = 4, step = M / 4; len <= M; len <<= 1, step >>= 1)
        {
            const int half = len >> 1;

            for (int i = 0; i < M; i += len)
            {
                float* ar = re + i;         float* ai = im + i;
                float* br = re + i + half;  float* bi = im + i + half;

                for (int j = 0; j < half; ++j)
                {
                    const float wr = twRe[(size_t) (j * step)], wi = twIm[(size_t) (j * step)];
                    const float tr = br[j] * wr - bi[j] * wi;
                    const float ti = br[j] * wi + bi[j] * wr;
                    br[j] = ar[j] - tr;  bi[j] = ai[j] - ti;
                    ar[j] += tr;         ai[j] += ti;
                }
            }
        }
    }

    int N = 0, M = 0, log2M = 0;
    std::vector<int> bitRev;
    std::vector<float> twRe, twIm, wRe, wIm, zRe, zIm;
};
} // namespace meridian
