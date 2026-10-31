#pragma once

#include <juce_core/juce_core.h>

#include <array>
#include <string_view>

/**
    Central registry of every APVTS parameter ID, plus the choice tables that
    map choice indices to physical values.

    Scalar IDs are static constexpr std::string_view constants. Indexed IDs
    follow two patterns, built with the helpers at the bottom:
        band parameters:  <prefix><index>_<field>   e.g. "ceq_band3_gain"
        slot parameters:  <prefix><index>           e.g. "module_bypass7"

    This header is deliberately free of processor/module dependencies so that
    every DSP module, the processor, the assistant and the UI can include it.
*/
namespace meridian
{

// ----------------------------------------------------------------------------
// Counts
// ----------------------------------------------------------------------------
inline constexpr int kNumModules      = 12;
inline constexpr int kNumSnapshots    = 4;
inline constexpr int kNumCeqBands     = 8;
inline constexpr int kNumDeqBands     = 6;
inline constexpr int kNumMbdBands     = 4;
inline constexpr int kNumMbdXovers    = kNumMbdBands - 1;
inline constexpr int kNumSatBands     = 3;
inline constexpr int kNumSatXovers    = kNumSatBands - 1;
inline constexpr int kNumImgBands     = 4;
inline constexpr int kNumImgXovers    = kNumImgBands - 1;

namespace pid
{
    // ---- InputModule --------------------------------------------------------
    inline constexpr std::string_view inputTrim        = "input_trim";
    inline constexpr std::string_view inputPolarity    = "input_polarity";
    inline constexpr std::string_view inputDcBlock     = "input_dcblock";

    // ---- CorrectiveEQ (band prefix + fields) --------------------------------
    inline constexpr std::string_view ceqBandPrefix    = "ceq_band";
    inline constexpr std::string_view ceqPhaseMode     = "ceq_phase_mode";

    // ---- LowEndControl ------------------------------------------------------
    inline constexpr std::string_view lecMonoFreq      = "lec_mono_freq";
    inline constexpr std::string_view lecMonoEnabled   = "lec_mono_enabled";
    inline constexpr std::string_view lecDynEnabled    = "lec_dyn_enabled";
    inline constexpr std::string_view lecDynThreshold  = "lec_dyn_threshold";
    inline constexpr std::string_view lecDynMaxGr      = "lec_dyn_maxgr";

    // ---- DynamicEQ ----------------------------------------------------------
    inline constexpr std::string_view deqBandPrefix    = "deq_band";
    inline constexpr std::string_view deqResonEnabled  = "deq_reson_enabled";

    // ---- GlueCompressor -----------------------------------------------------
    inline constexpr std::string_view glueRatio        = "glue_ratio";
    inline constexpr std::string_view glueThreshold    = "glue_threshold";
    inline constexpr std::string_view glueAttack       = "glue_attack";
    inline constexpr std::string_view glueRelease      = "glue_release";
    inline constexpr std::string_view glueAutoRelease  = "glue_auto_release";
    inline constexpr std::string_view glueKnee         = "glue_knee";
    inline constexpr std::string_view glueMakeup       = "glue_makeup";
    inline constexpr std::string_view glueMix          = "glue_mix";
    inline constexpr std::string_view glueScHpf        = "glue_sc_hpf";
    inline constexpr std::string_view glueLookahead    = "glue_lookahead";
    inline constexpr std::string_view glueScChannel    = "glue_sc_channel";

    // ---- MultibandDynamics --------------------------------------------------
    inline constexpr std::string_view mbdXoverPrefix   = "mbd_xover";
    inline constexpr std::string_view mbdBandPrefix    = "mbd_band";

    // ---- ToneEQ -------------------------------------------------------------
    inline constexpr std::string_view teqPultecBassFreq     = "teq_pultec_bass_freq";
    inline constexpr std::string_view teqPultecBassBoost    = "teq_pultec_bass_boost";
    inline constexpr std::string_view teqPultecBassAtten    = "teq_pultec_bass_atten";
    inline constexpr std::string_view teqPultecAttenFreq    = "teq_pultec_atten_freq";
    inline constexpr std::string_view teqPultecAttenBw      = "teq_pultec_atten_bw";
    inline constexpr std::string_view teqPultecMidBoost     = "teq_pultec_mid_boost";
    inline constexpr std::string_view teqPultecMidFreq      = "teq_pultec_mid_freq";
    inline constexpr std::string_view teqPultecMidBw        = "teq_pultec_mid_bw";
    inline constexpr std::string_view teqPultecTrebleBoost  = "teq_pultec_treble_boost";
    inline constexpr std::string_view teqPultecTrebleFreq   = "teq_pultec_treble_freq";
    inline constexpr std::string_view teqPultecTrebleBw     = "teq_pultec_treble_bw";
    inline constexpr std::string_view teqBaxandallBass      = "teq_baxandall_bass";
    inline constexpr std::string_view teqBaxandallBassFreq  = "teq_baxandall_bass_freq";
    inline constexpr std::string_view teqBaxandallTreble    = "teq_baxandall_treble";
    inline constexpr std::string_view teqBaxandallTrebleFreq= "teq_baxandall_treble_freq";
    inline constexpr std::string_view teqPassiveMidGain     = "teq_passive_mid_gain";
    inline constexpr std::string_view teqPassiveMidFreq     = "teq_passive_mid_freq";
    inline constexpr std::string_view teqPassiveMidQ        = "teq_passive_mid_q";
    inline constexpr std::string_view teqSideAirGain        = "teq_side_air_gain";
    inline constexpr std::string_view teqSideAirFreq        = "teq_side_air_freq";
    inline constexpr std::string_view teqColour             = "teq_colour";

    // ---- Saturation ---------------------------------------------------------
    inline constexpr std::string_view satModel             = "sat_model";
    inline constexpr std::string_view satDrive             = "sat_drive";
    inline constexpr std::string_view satBias              = "sat_bias";
    inline constexpr std::string_view satMix               = "sat_mix";
    inline constexpr std::string_view satOversampling      = "sat_oversampling";
    inline constexpr std::string_view satMultibandEnabled  = "sat_multiband_enabled";
    inline constexpr std::string_view satBandPrefix        = "sat_band";   // _freq: crossovers 0..1, _drive: bands 0..2
    inline constexpr std::string_view satAutoGain          = "sat_autogain";

    // ---- StereoImager -------------------------------------------------------
    inline constexpr std::string_view imgBandPrefix        = "img_band";
    inline constexpr std::string_view imgXoverPrefix       = "img_xover";
    inline constexpr std::string_view imgCorrGuard         = "img_corr_guard";

    // ---- Clipper ------------------------------------------------------------
    inline constexpr std::string_view clipperMode          = "clipper_mode";
    inline constexpr std::string_view clipperCeiling       = "clipper_ceiling";
    inline constexpr std::string_view clipperDrive         = "clipper_drive";
    inline constexpr std::string_view clipperEnabled       = "clipper_enabled";
    inline constexpr std::string_view clipperLinkCeiling   = "clipper_link_ceiling";

    // ---- TruePeakLimiter ----------------------------------------------------
    inline constexpr std::string_view limCeiling           = "lim_ceiling";
    inline constexpr std::string_view limLookahead         = "lim_lookahead";
    inline constexpr std::string_view limRelease           = "lim_release";
    inline constexpr std::string_view limAutoRelease       = "lim_auto_release";
    inline constexpr std::string_view limStyle             = "lim_style";
    inline constexpr std::string_view limLink              = "lim_link";
    inline constexpr std::string_view limOversampling      = "lim_oversampling";
    inline constexpr std::string_view limStage1Enabled     = "lim_stage1_enabled";

    // ---- Dither -------------------------------------------------------------
    inline constexpr std::string_view ditherEnabled        = "dither_enabled";
    inline constexpr std::string_view ditherBits           = "dither_bits";
    inline constexpr std::string_view ditherShape          = "dither_shape";

    // ---- Global -------------------------------------------------------------
    inline constexpr std::string_view moduleOrderPrefix    = "module_order";
    inline constexpr std::string_view moduleBypassPrefix   = "module_bypass";
    inline constexpr std::string_view moduleChannelPrefix  = "module_channel";
    inline constexpr std::string_view moduleWetDryPrefix   = "module_wetdry";
    inline constexpr std::string_view moduleDeltaPrefix    = "module_delta";
    inline constexpr std::string_view abSlot               = "ab_slot";
    inline constexpr std::string_view outputGain           = "output_gain";
    inline constexpr std::string_view lowLatencyMode       = "low_latency_mode";
    inline constexpr std::string_view lufsTarget           = "lufs_target";
    inline constexpr std::string_view lmBypass             = "lm_bypass";

    // ---- Non-parameter state properties (stored on the APVTS ValueTree) ------
    inline constexpr std::string_view targetProfileProperty = "target_profile";

    // ---- Band field names ---------------------------------------------------
    namespace field
    {
        inline constexpr std::string_view freq          = "freq";
        inline constexpr std::string_view gain          = "gain";
        inline constexpr std::string_view q             = "q";
        inline constexpr std::string_view type          = "type";
        inline constexpr std::string_view slope         = "slope";
        inline constexpr std::string_view enabled       = "enabled";
        inline constexpr std::string_view channel       = "channel";
        inline constexpr std::string_view threshold     = "threshold";
        inline constexpr std::string_view maxGain       = "maxgain";
        inline constexpr std::string_view attack        = "attack";
        inline constexpr std::string_view release       = "release";
        inline constexpr std::string_view ratio         = "ratio";
        inline constexpr std::string_view knee          = "knee";
        inline constexpr std::string_view makeup        = "makeup";
        inline constexpr std::string_view upwardThresh  = "upward_thresh";
        inline constexpr std::string_view upwardRatio   = "upward_ratio";
        inline constexpr std::string_view drive         = "drive";
        inline constexpr std::string_view width         = "width";
    }

    // ---- ID builders --------------------------------------------------------
    /** "<prefix><index>_<field>", e.g. bandParam (ceqBandPrefix, 3, field::gain) -> "ceq_band3_gain" */
    inline juce::String bandParam (std::string_view prefix, int index, std::string_view fieldName)
    {
        return juce::String (prefix.data(), prefix.size()) + juce::String (index) + "_"
             + juce::String (fieldName.data(), fieldName.size());
    }

    /** "<prefix><index>_freq" for crossover parameters, e.g. "mbd_xover1_freq" */
    inline juce::String xoverParam (std::string_view prefix, int index)
    {
        return bandParam (prefix, index, field::freq);
    }

    /** "<prefix><index>", e.g. slotParam (moduleBypassPrefix, 7) -> "module_bypass7" */
    inline juce::String slotParam (std::string_view prefix, int index)
    {
        return juce::String (prefix.data(), prefix.size()) + juce::String (index);
    }

    /** Converts a scalar string_view ID to a juce::String. */
    inline juce::String str (std::string_view id)
    {
        return juce::String (id.data(), id.size());
    }
} // namespace pid

// ----------------------------------------------------------------------------
// Choice tables (choice index -> physical value)
// ----------------------------------------------------------------------------
namespace choices
{
    inline constexpr std::array<int, 8>    ceqSlopesDbPerOct  { 6, 12, 18, 24, 36, 48, 72, 96 };
    inline constexpr int                   ceqDefaultSlopeIndex = 3;   // 24 dB/oct

    inline constexpr std::array<double, 4> pultecBassFreqs    { 20.0, 30.0, 60.0, 100.0 };
    inline constexpr std::array<double, 4> pultecAttenFreqs   { 100.0, 200.0, 300.0, 400.0 };
    inline constexpr std::array<double, 7> pultecMidFreqs     { 3000.0, 4000.0, 5000.0, 8000.0, 10000.0, 12000.0, 16000.0 };
    inline constexpr std::array<double, 5> pultecTrebleFreqs  { 8000.0, 10000.0, 12000.0, 14000.0, 16000.0 };

    inline constexpr std::array<int, 5>    satOversampling    { 1, 2, 4, 8, 16 };
    inline constexpr std::array<int, 3>    limOversampling    { 4, 8, 16 };
    inline constexpr std::array<int, 3>    ditherBits         { 16, 24, 32 };   // 32 = float target: dither off

    enum class ChannelMode : int { Stereo = 0, Mid, Side, Left, Right };
    enum class CeqFilterType : int { Bell = 0, LowShelf, HighShelf, Notch, Tilt, HighPass, LowPass, EllipticHighPass };
    enum class PhaseMode : int { Minimum = 0, Linear, Natural };
    enum class SatModel : int { Tape = 0, Tube, Transformer };
    enum class ClipperMode : int { Soft = 0, Hard, Knee };
    enum class LimiterStyle : int { Transparent = 0, Punchy, Warm, Aggressive };
    enum class DitherShape : int { TPDF = 0, HighShibata, EWeighted };
    enum class GlueSidechain : int { LeftPlusRight = 0, Mid };
} // namespace choices

} // namespace meridian
