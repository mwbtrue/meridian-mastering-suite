#include "IntelligentAssistant.h"

#include "ParameterIDs.h"
#include "analysis/MeteringEngine.h"
#include "common/Biquad.h"

#include <MeridianProfileData.h>
#include <juce_dsp/juce_dsp.h>

#include <algorithm>
#include <cmath>
#include <limits>

namespace meridian
{

namespace
{
    constexpr int kFftOrder = 11;
    constexpr int kFftSize = 1 << kFftOrder;
    constexpr float kGrDecayPerCall = 0.995f;   // held GR decays ~40 % per second at 512-sample chunks
    constexpr float kLimiterBudgetDb = 4.0f;
    constexpr float kDominanceMinTotalDb = 3.0f;
    constexpr float kDominanceShare = 0.5f;
    constexpr int kLimiterModule = 10;           // ModuleId::TruePeakLimiter

    float toDb (double linear) noexcept
    {
        return linear > 1.0e-15 ? static_cast<float> (20.0 * std::log10 (linear)) : -150.0f;
    }

    juce::String clip80 (const juce::String& s) { return s.length() <= 80 ? s : s.substring (0, 79) + "~"; }
    juce::String f1 (float v) { return juce::String (v, 1); }

    double parseChoiceNumber (const juce::String& text)
    {
        const auto t = text.trim();
        double v = t.getDoubleValue();
        if (t.containsIgnoreCase ("khz"))
            v *= 1000.0;
        return v;
    }
}

//==============================================================================
class IntelligentAssistant::AnalysisThread final : public juce::Thread
{
public:
    explicit AnalysisThread (IntelligentAssistant& o) : juce::Thread ("Meridian Assistant Analysis"), owner (o) {}

    void run() override
    {
        owner.runAnalysis (owner.pendingBuffer_, owner.pendingRate_);
        owner.analyzing_.store (false, std::memory_order_release);
    }

private:
    IntelligentAssistant& owner;
};

//==============================================================================
IntelligentAssistant::IntelligentAssistant()
{
    loadFactoryProfiles();
}

IntelligentAssistant::~IntelligentAssistant()
{
    if (analysisThread_ != nullptr)
        analysisThread_->stopThread (30000);
}

void IntelligentAssistant::prepare (double sampleRate, int)
{
    sampleRate_ = sampleRate > 0.0 ? sampleRate : 48000.0;
}

//==============================================================================
// Profiles
//==============================================================================
void IntelligentAssistant::loadFactoryProfiles()
{
    const juce::String previous = activeProfile_ != nullptr ? activeProfile_->name : juce::String();
    profiles_.clear();

    for (int i = 0; i < MeridianProfileData::namedResourceListSize; ++i)
    {
        int size = 0;
        if (const char* data = MeridianProfileData::getNamedResource (MeridianProfileData::namedResourceList[i], size))
        {
            auto p = TargetProfile::loadFromJSONString (juce::String::fromUTF8 (data, size));
            if (p.isValid())
                profiles_.push_back (std::move (p));
        }
    }

    activeProfile_ = nullptr;
    activeIndex_ = -1;
    setActiveProfile (previous.isNotEmpty() ? previous : juce::String ("R&B Major Label"));
}

void IntelligentAssistant::loadProfilesFromDirectory (const juce::File& dir)
{
    const juce::String previous = activeProfile_ != nullptr ? activeProfile_->name : juce::String();
    loadFactoryProfiles();

    if (dir.isDirectory())
    {
        for (const auto& entry : juce::RangedDirectoryIterator (dir, false, "*.json", juce::File::findFiles))
        {
            auto p = TargetProfile::loadFromJSON (entry.getFile());
            if (! p.isValid())
                continue;

            auto existing = std::find_if (profiles_.begin(), profiles_.end(),
                                          [&] (const TargetProfile& q) { return q.name == p.name; });
            if (existing != profiles_.end())
                *existing = std::move (p);
            else
                profiles_.push_back (std::move (p));
        }
    }

    activeProfile_ = nullptr;
    activeIndex_ = -1;
    setActiveProfile (previous.isNotEmpty() ? previous : juce::String ("R&B Major Label"));
}

void IntelligentAssistant::setActiveProfile (const juce::String& name)
{
    int found = -1;
    for (int i = 0; i < (int) profiles_.size() && found < 0; ++i)
        if (profiles_[(size_t) i].name == name)
            found = i;

    for (int i = 0; i < (int) profiles_.size() && found < 0; ++i)
        if (name.isNotEmpty() && profiles_[(size_t) i].name.startsWithIgnoreCase (name))
            found = i;

    if (found < 0 && activeIndex_ < 0 && ! profiles_.empty())
        found = 0;

    if (found >= 0)
    {
        activeIndex_ = found;
        activeProfile_ = &profiles_[(size_t) found];
    }
}

const TargetProfile* IntelligentAssistant::getActiveProfile() const
{
    return activeProfile_;
}

//==============================================================================
// Analysis
//==============================================================================
void IntelligentAssistant::analyzeBuffer (const juce::AudioBuffer<float>& buf, double sampleRate,
                                          const juce::String& profileName)
{
    if (isAnalyzing() || buf.getNumSamples() == 0 || buf.getNumChannels() == 0)
        return;

    if (analysisThread_ != nullptr)
    {
        analysisThread_->stopThread (5000);
        analysisThread_.reset();
    }

    setActiveProfile (profileName);

    pendingBuffer_.setSize (2, buf.getNumSamples(), false, false, true);
    pendingBuffer_.copyFrom (0, 0, buf, 0, 0, buf.getNumSamples());
    pendingBuffer_.copyFrom (1, 0, buf, std::min (1, buf.getNumChannels() - 1), 0, buf.getNumSamples());
    pendingRate_ = sampleRate > 0.0 ? sampleRate : sampleRate_;
    pendingProfile_ = activeProfile_ != nullptr ? *activeProfile_ : TargetProfile();

    analyzing_.store (true, std::memory_order_release);
    analysisThread_ = std::make_unique<AnalysisThread> (*this);
    analysisThread_->startThread (juce::Thread::Priority::low);
}

IntelligentAssistant::AnalysisResult IntelligentAssistant::getAnalysisResult() const
{
    const std::lock_guard<std::mutex> lock (resultMutex_);
    return result_;
}

std::vector<IntelligentAssistant::ParameterProposal> IntelligentAssistant::getProposals() const
{
    const std::lock_guard<std::mutex> lock (resultMutex_);
    return proposals_;
}

float IntelligentAssistant::bandRmsDb (const juce::AudioBuffer<float>& buf, double sr, double lowHz, double highHz)
{
    constexpr double q = 0.70710678118654752;
    const double nyq = 0.49 * sr;
    Biquad hp1 (Biquad::makeHPF (std::min (lowHz, nyq), q, sr)), hp2 (Biquad::makeHPF (std::min (lowHz, nyq), q, sr));
    Biquad lp1 (Biquad::makeLPF (std::min (highHz, nyq), q, sr)), lp2 (Biquad::makeLPF (std::min (highHz, nyq), q, sr));

    const int n = buf.getNumSamples();
    const float* L = buf.getReadPointer (0);
    const float* R = buf.getReadPointer (std::min (1, buf.getNumChannels() - 1));

    double sum = 0.0;
    for (int i = 0; i < n; ++i)
    {
        const double mid = 0.5 * (static_cast<double> (L[i]) + static_cast<double> (R[i]));
        const double y = lp2.process (lp1.process (hp2.process (hp1.process (mid))));
        sum += y * y;
    }

    return n > 0 ? toDb (std::sqrt (sum / n)) : -150.0f;
}

float IntelligentAssistant::lowBandCorrelation (const juce::AudioBuffer<float>& buf, double sr)
{
    constexpr double q = 0.70710678118654752;
    Biquad l1 (Biquad::makeLPF (150.0, q, sr)), l2 (Biquad::makeLPF (150.0, q, sr));
    Biquad r1 (Biquad::makeLPF (150.0, q, sr)), r2 (Biquad::makeLPF (150.0, q, sr));

    const int n = buf.getNumSamples();
    const float* L = buf.getReadPointer (0);
    const float* R = buf.getReadPointer (std::min (1, buf.getNumChannels() - 1));

    double lr = 0.0, ll = 0.0, rr = 0.0;
    for (int i = 0; i < n; ++i)
    {
        const double a = l2.process (l1.process (L[i]));
        const double b = r2.process (r1.process (R[i]));
        lr += a * b;
        ll += a * a;
        rr += b * b;
    }

    const double denom = std::sqrt (ll * rr);
    return denom > 1.0e-20 ? static_cast<float> (juce::jlimit (-1.0, 1.0, lr / denom)) : 1.0f;
}

float IntelligentAssistant::measureSibilance (const juce::AudioBuffer<float>& buf, double sr)
{
    return bandRmsDb (buf, sr, 5500.0, 9000.0);
}

float IntelligentAssistant::measureSpectralTilt (const std::array<float, kNumBins>& spectrum, double sr)
{
    const double binHz = sr / kFftSize;

    auto meanDb = [&] (double lo, double hi)
    {
        double sum = 0.0;
        int count = 0;
        for (int k = 1; k < kNumBins; ++k)
        {
            const double f = k * binHz;
            if (f >= lo && f <= hi)
            {
                sum += spectrum[(size_t) k];
                ++count;
            }
        }
        return count > 0 ? sum / count : static_cast<double> (SpectrumFrame::kFloorDb);
    };

    const double high = meanDb (8000.0, std::min (16000.0, 0.49 * sr));
    const double low = meanDb (100.0, 800.0);
    return static_cast<float> (high - low) - kReferenceTilt;
}

void IntelligentAssistant::runAnalysis (const juce::AudioBuffer<float>& buf, double sampleRate)
{
    AnalysisResult r;
    const int n = buf.getNumSamples();
    const float* L = buf.getReadPointer (0);
    const float* R = buf.getReadPointer (std::min (1, buf.getNumChannels() - 1));
    r.durationSeconds = static_cast<float> (n / sampleRate);

    // ---- 1. Loudness -------------------------------------------------------------------
    {
        const int block = std::max (1, juce::roundToInt (sampleRate * 0.1));
        MeteringEngine meter;
        meter.prepare (sampleRate, block);
        meter.reset();

        std::vector<double> bl ((size_t) block), br ((size_t) block);
        MeterData md;
        const int warmup = juce::roundToInt (sampleRate * 3.0);

        for (int pos = 0; pos < n; pos += block)
        {
            const int len = std::min (block, n - pos);
            for (int i = 0; i < len; ++i)
            {
                bl[(size_t) i] = L[pos + i];
                br[(size_t) i] = R[pos + i];
            }
            meter.process (bl.data(), br.data(), len);
            meter.fillMeterData (md);

            if (pos + len >= warmup || n < warmup)
                r.shortTermPeakLUFS = std::max (r.shortTermPeakLUFS, md.shortTermLUFS);
        }

        r.integratedLUFS = md.integratedLUFS;
        r.truePeak_dBTP = md.truePeak;
        r.LRA = md.LRA;
        r.PLR = md.integratedLUFS > -70.0f ? md.truePeak - md.integratedLUFS : 0.0f;
    }

    // ---- 2. Average spectrum -------------------------------------------------------------
    {
        juce::dsp::FFT fft (kFftOrder);
        std::vector<float> window ((size_t) kFftSize), work ((size_t) kFftSize * 2);
        for (int i = 0; i < kFftSize; ++i)
            window[(size_t) i] = static_cast<float> (0.5 - 0.5 * std::cos (2.0 * juce::MathConstants<double>::pi * i / kFftSize));

        std::vector<double> power ((size_t) kNumBins, 0.0);
        int frames = 0;
        const double scale = 4.0 / kFftSize;

        for (int start = 0; start + kFftSize <= n; start += kFftSize / 2)
        {
            for (int ch = 0; ch < 2; ++ch)
            {
                const float* x = ch == 0 ? L : R;
                std::fill (work.begin(), work.end(), 0.0f);
                for (int i = 0; i < kFftSize; ++i)
                    work[(size_t) i] = x[start + i] * window[(size_t) i];
                fft.performFrequencyOnlyForwardTransform (work.data(), true);
                for (int k = 0; k < kNumBins; ++k)
                {
                    const double m = static_cast<double> (work[(size_t) k]) * scale;
                    power[(size_t) k] += 0.5 * m * m;
                }
            }
            ++frames;
        }

        for (int k = 0; k < kNumBins; ++k)
        {
            const double p = frames > 0 ? power[(size_t) k] / frames : 0.0;
            r.avgSpectrum[(size_t) k] = p > 1.0e-12 ? std::max (SpectrumFrame::kFloorDb, static_cast<float> (10.0 * std::log10 (p)))
                                                    : SpectrumFrame::kFloorDb;
        }
    }

    // ---- 3-7. Band energies, tilt, low correlation ---------------------------------------
    r.sibilanceEnergy_dB = measureSibilance (buf, sampleRate);
    r.muddyEnergy_dB = bandRmsDb (buf, sampleRate, 200.0, 400.0);
    r.subEnergy_dB = bandRmsDb (buf, sampleRate, 20.0, 80.0);
    r.spectralTilt_dB = measureSpectralTilt (r.avgSpectrum, sampleRate);
    r.lowCorrelation = lowBandCorrelation (buf, sampleRate);
    r.valid = n > 0;

    {
        const std::lock_guard<std::mutex> lock (resultMutex_);
        result_ = r;
    }

    generateProposals (r, pendingProfile_, apvts);
}

//==============================================================================
// Proposals
//==============================================================================
float IntelligentAssistant::resolveValue (juce::RangedAudioParameter& param, float value)
{
    if (auto* choice = dynamic_cast<juce::AudioParameterChoice*> (&param))
    {
        const int count = choice->choices.size();
        const int idx = juce::roundToInt (value);
        if (std::abs (value - static_cast<float> (idx)) < 1.0e-4f && idx >= 0 && idx < count)
            return static_cast<float> (idx);

        int best = 0;
        double bestDist = std::numeric_limits<double>::max();
        for (int i = 0; i < count; ++i)
        {
            const double d = std::abs (parseChoiceNumber (choice->choices[i]) - value);
            if (d < bestDist)
            {
                bestDist = d;
                best = i;
            }
        }
        return static_cast<float> (best);
    }

    if (dynamic_cast<juce::AudioParameterBool*> (&param) != nullptr)
        return value >= 0.5f ? 1.0f : 0.0f;

    const auto& range = param.getNormalisableRange();
    return range.snapToLegalValue (juce::jlimit (range.start, range.end, value));
}

void IntelligentAssistant::setParameter (juce::AudioProcessorValueTreeState& state, const juce::String& id, float value)
{
    if (auto* p = state.getParameter (id))
    {
        const float v = resolveValue (*p, value);
        p->beginChangeGesture();
        p->setValueNotifyingHost (p->convertTo0to1 (v));
        p->endChangeGesture();
    }
}

juce::String IntelligentAssistant::describeParameter (const juce::AudioProcessorValueTreeState& state, const juce::String& id)
{
    if (auto* p = state.getParameter (id))
        return p->getName (48);
    return id;
}

void IntelligentAssistant::generateProposals (const AnalysisResult& r, const TargetProfile& profile,
                                              juce::AudioProcessorValueTreeState* state)
{
    std::vector<ParameterProposal> out;
    if (state == nullptr)
    {
        const std::lock_guard<std::mutex> lock (resultMutex_);
        proposals_.clear();
        return;
    }

    auto param = [state] (const juce::String& id) { return state->getParameter (id); };

    auto current = [&] (const juce::String& id) -> float
    {
        if (auto* p = param (id))
            return p->convertFrom0to1 (p->getValue());
        return std::numeric_limits<float>::quiet_NaN();
    };

    // Value a later rule should build on: an earlier proposal if present, else the live value.
    auto base = [&] (const juce::String& id) -> float
    {
        for (const auto& pr : out)
            if (pr.paramId == id)
                return pr.proposedValue;
        return current (id);
    };

    auto propose = [&] (const juce::String& id, float value, const juce::String& reason)
    {
        auto* p = param (id);
        if (p == nullptr)
            return;

        const float target = resolveValue (*p, value);
        const float cur = current (id);
        const auto& range = p->getNormalisableRange();
        const float tol = 1.0e-3f * std::max (1.0f, range.end - range.start);

        auto existing = std::find_if (out.begin(), out.end(), [&] (const ParameterProposal& q) { return q.paramId == id; });

        if (std::abs (target - cur) <= tol)
        {
            if (existing != out.end())
                out.erase (existing);
            return;
        }

        ParameterProposal pr;
        pr.paramId = id;
        pr.currentValue = cur;
        pr.proposedValue = target;
        pr.reason = clip80 (reason);

        if (existing != out.end())
            *existing = pr;
        else
            out.push_back (pr);
    };

    const juce::String shortName = profile.name.upToFirstOccurrenceOf ("(", false, false).trim().substring (0, 30);

    // ---- Base: profile suggestions ----------------------------------------------------
    for (const auto& [id, value] : profile.paramSuggestions)
        propose (id, value, "Profile " + shortName + ": " + describeParameter (*state, id));

    propose (pid::str (pid::lufsTarget), profile.targetLUFS_I, "Profile loudness target " + f1 (profile.targetLUFS_I) + " LUFS");

    if (! r.valid)
    {
        const std::lock_guard<std::mutex> lock (resultMutex_);
        proposals_ = std::move (out);
        return;
    }

    const bool loudnessValid = r.integratedLUFS > -70.0f;
    const juce::String trim = pid::str (pid::inputTrim);

    // ---- Loudness -------------------------------------------------------------------------
    bool pushedLouder = false;
    if (loudnessValid && r.integratedLUFS < profile.targetLUFS_I - 1.0f)
    {
        const float d = profile.targetLUFS_I - r.integratedLUFS;
        const float push = std::min (d, 6.0f);
        propose (trim, base (trim) + push,
                 f1 (r.integratedLUFS) + " LUFS vs " + f1 (profile.targetLUFS_I) + ": drive limiter +" + f1 (push) + " dB");
        const juce::String glue = pid::str (pid::glueThreshold);
        propose (glue, base (glue) - std::min (0.5f * d, 3.0f), "Lower glue threshold for density at +" + f1 (push) + " dB");
        pushedLouder = true;
    }
    else if (loudnessValid && r.integratedLUFS > profile.targetLUFS_I + 1.0f)
    {
        const float d = std::min (r.integratedLUFS - profile.targetLUFS_I, 6.0f);
        propose (trim, base (trim) - d,
                 f1 (r.integratedLUFS) + " LUFS vs " + f1 (profile.targetLUFS_I) + ": back off drive " + f1 (d) + " dB");
    }

    // ---- True peak ------------------------------------------------------------------------
    if (r.truePeak_dBTP > profile.targetTP_dBTP + 0.05f)
    {
        const juce::String ceil = pid::str (pid::limCeiling);
        if (base (ceil) > profile.targetTP_dBTP)
        {
            propose (ceil, profile.targetTP_dBTP, "True peak " + juce::String (r.truePeak_dBTP, 2) + " dBTP: lower ceiling");
        }
        else
        {
            const juce::String outGain = pid::str (pid::outputGain);
            const float over = r.truePeak_dBTP - profile.targetTP_dBTP;
            propose (outGain, base (outGain) - over - 0.1f, "TP over target after limiter: trim output " + juce::String (over + 0.1f, 2) + " dB");
        }
    }

    // ---- Low-end correlation ---------------------------------------------------------------
    if (r.lowCorrelation < profile.targetLowCorr)
    {
        const juce::String monoOn = pid::str (pid::lecMonoEnabled);
        const juce::String monoFreq = pid::str (pid::lecMonoFreq);
        const juce::String corr = juce::String (r.lowCorrelation, 2) + " < " + juce::String (profile.targetLowCorr, 2);

        if (base (monoOn) < 0.5f)
            propose (monoOn, 1.0f, "Low-band correlation " + corr + ": enable mono-maker");
        else
            propose (monoFreq, std::min (200.0f, base (monoFreq) + 30.0f), "Low-band correlation " + corr + ": raise mono crossover");
    }

    // ---- Sibilance ------------------------------------------------------------------------
    if (r.sibilanceEnergy_dB > kSibilanceLimitDb)
    {
        using namespace pid;
        const auto band = [] (std::string_view f) { return bandParam (deqBandPrefix, 2, f); };
        const juce::String why = "Sibilance " + f1 (r.sibilanceEnergy_dB) + " dB (5.5-9 kHz Mid): de-ess";
        propose (band (field::enabled), 1.0f, why);
        propose (band (field::freq), 7000.0f, why);
        propose (band (field::q), 2.0f, why);
        propose (band (field::maxGain), -2.0f, why);
        propose (band (field::channel), 1.0f, why);
        propose (band (field::threshold), std::round (r.sibilanceEnergy_dB - 3.0f), why);
    }

    // ---- Loudness range ---------------------------------------------------------------------
    if (r.LRA > 0.0f && r.LRA < profile.targetLRA_min)
    {
        for (int b = 0; b < kNumMbdBands; ++b)
        {
            const auto id = pid::bandParam (pid::mbdBandPrefix, b, pid::field::ratio);
            propose (id, std::max (1.0f, base (id) * 0.8f), "LRA " + f1 (r.LRA) + " LU < " + f1 (profile.targetLRA_min) + ": ease multiband ratio");
        }
    }
    else if (r.LRA > profile.targetLRA_max)
    {
        for (int b = 0; b < kNumMbdBands; ++b)
        {
            const auto id = pid::bandParam (pid::mbdBandPrefix, b, pid::field::threshold);
            propose (id, std::max (-40.0f, base (id) - 3.0f), "LRA " + f1 (r.LRA) + " LU > " + f1 (profile.targetLRA_max) + ": lower MB threshold 3 dB");
        }
    }

    // ---- Peak-to-loudness ratio --------------------------------------------------------------
    if (loudnessValid && ! pushedLouder && r.PLR < profile.targetPLR_min)
    {
        const juce::String drive = pid::str (pid::clipperDrive);
        propose (drive, std::max (0.0f, base (drive) - 1.0f), "PLR " + f1 (r.PLR) + " dB < " + f1 (profile.targetPLR_min) + ": less clipping");
    }

    // ---- Spectral tilt (relative to the reference master tilt) ------------------------------
    {
        const juce::String treble = pid::str (pid::teqPultecTrebleBoost);
        if (r.spectralTilt_dB < -2.0f)
        {
            const float d = juce::jlimit (0.5f, 2.0f, -0.5f * r.spectralTilt_dB);
            propose (treble, std::min (10.0f, base (treble) + d), "Spectrum " + f1 (-r.spectralTilt_dB) + " dB dull: more Pultec treble");
        }
        else if (r.spectralTilt_dB > 2.0f)
        {
            const float d = juce::jlimit (0.5f, 2.0f, 0.5f * r.spectralTilt_dB);
            propose (treble, std::max (0.0f, base (treble) - d), "Spectrum " + f1 (r.spectralTilt_dB) + " dB bright: less Pultec treble");
        }
    }

    const std::lock_guard<std::mutex> lock (resultMutex_);
    proposals_ = std::move (out);
}

void IntelligentAssistant::acceptProposal (int index, juce::AudioProcessorValueTreeState& state)
{
    ParameterProposal pr;
    {
        const std::lock_guard<std::mutex> lock (resultMutex_);
        if (index < 0 || index >= (int) proposals_.size())
            return;
        proposals_[(size_t) index].accepted = true;
        pr = proposals_[(size_t) index];
    }
    setParameter (state, pr.paramId, pr.proposedValue);
}

void IntelligentAssistant::acceptAllProposals (juce::AudioProcessorValueTreeState& state)
{
    const int count = (int) getProposals().size();
    for (int i = 0; i < count; ++i)
        acceptProposal (i, state);
}

void IntelligentAssistant::rejectProposal (int index, juce::AudioProcessorValueTreeState& state)
{
    ParameterProposal pr;
    {
        const std::lock_guard<std::mutex> lock (resultMutex_);
        if (index < 0 || index >= (int) proposals_.size())
            return;
        pr = proposals_[(size_t) index];
        proposals_[(size_t) index].accepted = false;
    }
    if (pr.accepted)
        setParameter (state, pr.paramId, pr.currentValue);
}

void IntelligentAssistant::rejectAllProposals (juce::AudioProcessorValueTreeState& state)
{
    const int count = (int) getProposals().size();
    for (int i = count - 1; i >= 0; --i)
        rejectProposal (i, state);
}

//==============================================================================
// Gain-reduction budget
//==============================================================================
void IntelligentAssistant::updateGRBudget (float limiterGR_dB, const float* perModuleGR) noexcept
{
    limiterHeld_ = std::max (std::abs (limiterGR_dB), limiterHeld_ * kGrDecayPerCall);

    float total = 0.0f, maxGr = 0.0f;
    int dominant = -1;

    for (int m = 0; m < kNumModules; ++m)
    {
        const float gr = perModuleGR != nullptr ? std::abs (perModuleGR[m]) : 0.0f;
        grHeld_[(size_t) m] = std::max (gr, grHeld_[(size_t) m] * kGrDecayPerCall);
        total += grHeld_[(size_t) m];
        if (grHeld_[(size_t) m] > maxGr)
        {
            maxGr = grHeld_[(size_t) m];
            dominant = m;
        }
    }

    int flags = 0;
    if (limiterHeld_ > kLimiterBudgetDb)
        flags |= 1;

    const float share = total > 0.0f ? maxGr / total : 0.0f;
    if (total > kDominanceMinTotalDb && share > kDominanceShare)
        flags |= 2;

    grLimiterDb.store (limiterHeld_, std::memory_order_relaxed);
    grTotalDb.store (total, std::memory_order_relaxed);
    grDominantShare.store (share, std::memory_order_relaxed);
    grDominantModule.store (dominant, std::memory_order_relaxed);
    grWarningFlags.store (flags, std::memory_order_relaxed);
}

float IntelligentAssistant::getGRWarningMessage (juce::String& msg) const
{
    msg.clear();
    const int flags = grWarningFlags.load (std::memory_order_relaxed);
    const float lim = grLimiterDb.load (std::memory_order_relaxed);

    if ((flags & 1) != 0)
        msg << "Limiter working > 4 dB (" << juce::String (lim, 1) << " dB) - reduce upstream gain";

    if ((flags & 2) != 0)
    {
        const int m = grDominantModule.load (std::memory_order_relaxed);
        const bool limiterAlreadyReported = (flags & 1) != 0 && m == kLimiterModule;

        if (m >= 0 && m < kNumModules && ! limiterAlreadyReported)
        {
            static constexpr const char* names[kNumModules] { "Input", "Corrective EQ", "Low End", "Dynamic EQ", "Glue Comp",
                                                              "Multiband", "Tone EQ", "Saturation", "Imager", "Clipper",
                                                              "Limiter", "Dither" };
            if (msg.isNotEmpty())
                msg << "  |  ";
            msg << names[m] << " does " << juce::roundToInt (100.0f * grDominantShare.load()) << "% of "
                << juce::String (grTotalDb.load(), 1) << " dB total GR - spread the work";
        }
    }

    return lim;
}

} // namespace meridian
