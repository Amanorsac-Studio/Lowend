// Antialias.h — anti-aliasing and DC-management primitives for the JM Signature
// DSP, per `pedal-style-effect-plugin-development-guide.md` §1.3 and §2.
//
// §1.3: "Any nonlinear waveshaper ... generates harmonics above Nyquist that
// fold back as audible aliasing artifacts if you don't manage it." Two accepted
// professional answers: oversampling, and Antiderivative Antialiasing (ADAA).
// This header supplies the ADAA half, plus the DC blocker §2's distortion row
// asks for and the soft ceiling the delay/limiter rows want.
//
// WHY ADAA CARRIES MOST OF THE LOAD HERE (and where 4x joins it)
// --------------------------------------------------------------
// Client review round 6 fixed the plugin's reported latency at exactly the
// limiter's 96-sample lookahead and recorded, as a decision, that the reported
// latency must not change at runtime (hosts handle that badly, and the FX rack
// is user-reorderable, so a per-slot oversampler would make latency a function
// of the rack's contents). ADAA's cost is a HALF-SAMPLE group delay and a mild
// high-frequency softening — nothing a host has to compensate for — which is
// exactly why the guide names it "mostly used for memoryless waveshaping
// stages". Every nonlinearity we anti-alias here IS memoryless:
//
//   * EP Preamp `tanh`               -> 4x oversampling + ADAA (guide's belt
//                                       and braces; the unit is a single
//                                       stereo instance, so 4x is affordable.
//                                       Measured: -7.7 dB -> -66.3 dB alias
//                                       floor at full drive)
//   * Sampler per-voice drive `tanh` -> ADAA only (up to 64 concurrent voices;
//                                       4x on every voice is not affordable,
//                                       and ADAA is the trade the guide names
//                                       for exactly this case)
//   * Limiter ceiling clamp          -> C1 soft ceiling (a hard clamp is a
//                                       DISCONTINUOUS DERIVATIVE, the worst
//                                       possible aliaser; a C1 curve's
//                                       harmonics fall away as 1/n^3 instead
//                                       of 1/n^2, and it stays a wire below
//                                       its knee)
//   * Delay feedback loops           -> soft ceiling (bounded loop, §2 delay row)
//
// First-order ADAA (Parker/Zavalishin/Le Bivic; see the guide's references):
// given f() with antiderivative F(),
//
//      y[n] = ( F(x[n]) - F(x[n-1]) ) / ( x[n] - x[n-1] )
//
// which is the average of f over the segment the signal traversed during the
// sample period — i.e. a band-limited evaluation — degenerating to f(x) when
// the signal is not moving. The difference quotient is ill-conditioned for tiny
// steps, so below a threshold we fall back to the midpoint f((x+x1)/2); the
// error there is O(d^2) and the signal is barely moving, so it cannot alias.
//
// ONE REFINEMENT, and it matters a lot here. Applied literally, first-order
// ADAA of f(x) = x is (x + x[n-1])/2 — a two-tap moving average. That is the
// "mild low-pass coloration" the guide mentions, and it applies to the WHOLE
// signal, including the perfectly linear part that generates no harmonics and
// therefore needs no band-limiting at all. On the master limiter that showed up
// as a -6 dBFS null residual against the bypassed path: the safety wall was
// quietly low-passing everything that passed through it.
//
// On a DRIVE stage that low-pass is acceptable — it is part of the sound of
// every anti-aliased saturator, and the alternatives are worse (see the note on
// AdaaTanh). On the master LIMITER's brick wall it is not: a safety stage must
// be a wire when it is not working. The wall therefore does not use ADAA at
// all; it uses a plain C1-continuous soft ceiling that is bit-exact below its
// knee. See `SoftCeiling`.
//
// Everything here is allocation-free and branch-cheap: usable on the audio
// thread inside the sample loop.
#pragma once
#include <juce_audio_basics/juce_audio_basics.h>
#include <cmath>

namespace jm {

//==============================================================================
/// log(cosh(x)), the antiderivative of tanh, evaluated without overflowing.
/// cosh(x) overflows a float around |x| = 89; log(cosh x) = |x| - ln2 +
/// log1p(exp(-2|x|)) is exact and finite for every input we can produce.
inline double logCosh (double x) noexcept
{
    const double a = std::abs (x);
    return a + std::log1p (std::exp (-2.0 * a)) - 0.6931471805599453;
}

//==============================================================================
/// First-order antiderivative-antialiased `tanh`. One instance per channel (or
/// per voice per channel); state is two doubles.
///
/// This is the textbook form, `y = (F(x) - F(x1)) / (x - x1)`, and it is the
/// textbook form on purpose. The tempting "residual" variant
/// `y = x + ADAA(tanh(x) - x)` — which avoids ADAA's half-sample low-pass on
/// the linear part of the curve — was implemented and measured, and it is wrong
/// for a saturator: in the flat region of tanh the residual antiderivative
/// grows like -x^2/2, so its difference quotient degenerates to
/// `y = 1 + (x - x1)/2`. That is a DIFFERENTIATOR, not a limiter: it overshoots
/// tanh's own bound by half the per-sample slew (measured: up to 3.6 at drive
/// 100 with a 14.5 kHz probe) and the stage stops saturating. Clamping the
/// overshoot just puts a hard clipper back in. The half-sample low-pass the
/// textbook form costs — the "mild low-pass coloration" the guide names — is
/// the honest price, and on a drive stage it reads as slightly softer top end,
/// not as a defect.
struct AdaaTanh
{
    /// Consistent "the previous sample was silence" state.
    void reset() noexcept { x1_ = 0.0; f1_ = 0.0; }

    float process (float xin) noexcept
    {
        const double x = (double) xin;
        const double d = x - x1_;
        const double f = logCosh (x);
        const double y = std::abs (d) < kMinStep ? std::tanh (0.5 * (x + x1_))   // midpoint, O(d^2)
                                                 : (f - f1_) / d;
        f1_ = f;
        x1_ = x;
        return (float) y;
    }

private:
    /// Below this step the difference quotient loses more precision than the
    /// midpoint fallback costs in accuracy (see the header comment).
    static constexpr double kMinStep = 1.0e-5;
    double x1_ = 0.0, f1_ = 0.0;
};

//==============================================================================
/// C1-continuous soft ceiling: identity below `knee`, then a tanh bend that
/// approaches `ceiling` asymptotically and never reaches it.
///
///     |x| <= T           : y = x                      (bit-exact passthrough)
///     |x| >  T           : y = sgn(x) * (T + k*tanh((|x|-T)/k)),  k = C - T
///
/// The derivative is 1 on both sides of T, so there is no corner to radiate
/// harmonics, and |y| < C strictly — still a brick wall, but a differentiable
/// one. Odd-symmetric, so its antiderivative is even and ADAA applies.
struct SoftCeiling
{
    void set (float ceiling, float kneeFraction) noexcept
    {
        c_ = ceiling;
        t_ = ceiling * kneeFraction;
        k_ = juce::jmax (1.0e-6f, c_ - t_);
    }
    float ceiling() const noexcept { return c_; }
    float knee() const noexcept { return t_; }

    float shape (float x) const noexcept
    {
        const float a = std::abs (x);
        if (a <= t_) return x;
        const float y = t_ + k_ * std::tanh ((a - t_) / k_);
        return x < 0.0f ? -y : y;
    }

    /// Antiderivative of `shape` (even, zero at x = 0).
    double antiderivative (double x) const noexcept
    {
        const double a = std::abs (x);
        const double T = (double) t_, K = (double) k_;
        if (a <= T) return 0.5 * a * a;
        return 0.5 * T * T + T * (a - T) + K * K * logCosh ((a - T) / K);
    }

private:
    float c_ = 1.0f, t_ = 0.9f, k_ = 0.1f;
};

/// WHY THE LIMITER'S WALL IS NOT ADAA'd
/// ------------------------------------
/// ADAA was tried on the ceiling and measured, and it is the wrong tool for a
/// safety stage. First-order ADAA replaces f(x) with the AVERAGE of f over the
/// segment the signal traversed since the last sample. At the frequencies where
/// aliasing is actually a problem (the regression probe is a 14.5 kHz sine —
/// 109 degrees of phase per sample) that average spans most of a half-cycle, so
/// the wall stopped passing peaks at all: measured peak output fell to exactly
/// the knee and the "alias floor" got 30 dB WORSE, because a brutal low-pass
/// looks like distortion to any measurement of it. The variant that avoids the
/// low-pass is unbounded, which a brick wall may not be.
///
/// `SoftCeiling` on its own already delivers what this stage needs:
///   * bit-exact below the knee — a wire when the limiter is not working, so
///     the plugin still nulls against its own bypassed path;
///   * C1-continuous, so unlike a hard clip there is no derivative
///     discontinuity and the harmonics it does make fall away far faster;
///   * strictly bounded by the ceiling, with no clamp and no state;
///   * zero added latency, zero added CPU beyond one comparison.

//==============================================================================
/// One-pole DC blocker, cutoff derived from the host rate (guide §4.1).
///
/// §2's distortion row: "add a small amount of DC-blocking after asymmetric
/// clipping". 8 Hz is deliberately far below the lowest musical fundamental the
/// instrument produces (A0 = 27.5 Hz, where this costs 0.35 dB) so it removes
/// offset and subsonic rumble without thinning the low end — measured in
/// `tests/DspStandards.inc`.
struct DcBlocker
{
    void prepare (double sampleRate, float cutoffHz = 8.0f) noexcept
    {
        const double sr = sampleRate > 0.0 ? sampleRate : 48000.0;
        r_ = (float) std::exp (-2.0 * juce::MathConstants<double>::pi * (double) cutoffHz / sr);
        reset();
    }
    void reset() noexcept { x1_ = y1_ = 0.0f; }
    float process (float x) noexcept
    {
        const float y = x - x1_ + r_ * y1_;
        x1_ = x;
        y1_ = y;
        return y;
    }
private:
    float r_ = 0.999f, x1_ = 0.0f, y1_ = 0.0f;
};

//==============================================================================
/// 4-point, 3rd-order Lagrange (Catmull-Rom) fractional-delay interpolation.
///
/// Guide §2, modulation row: "linear is cheap but adds high-frequency loss;
/// allpass or Lagrange interpolation sounds cleaner". Linear interpolation is a
/// 2-tap FIR whose magnitude response at a half-sample delay is
/// cos(pi*f/fs) — **-3.0 dB at 0.25*fs and -10.2 dB at 0.4*fs**, i.e. every
/// modulated tap in the chorus, flanger, both delays and the reverb pre-delay
/// was quietly losing its top octave, and losing a DIFFERENT amount of it every
/// sample as the LFO moved the tap (that wobble is the "swishy" artefact). The
/// cubic form below costs three extra multiply-adds and recovers 1.9 dB and
/// 3.2 dB of that (measured: -1.07 dB and -6.96 dB at the same two points).
///
/// x0..x3 are consecutive samples; `f` in [0,1) is the position between x1 and
/// x2 (the classic interpolation interval).
inline float lagrange3 (float x0, float x1, float x2, float x3, float f) noexcept
{
    const float c0 = x1;
    const float c1 = 0.5f * (x2 - x0);
    const float c2 = x0 - 2.5f * x1 + 2.0f * x2 - 0.5f * x3;
    const float c3 = 0.5f * (x3 - x0) + 1.5f * (x1 - x2);
    return ((c3 * f + c2) * f + c1) * f + c0;
}

} // namespace jm
