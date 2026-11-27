/* Primary-grade (DaVinci-style colour wheels) maths -- pure C, no libobs.
 *
 * Mirrors the conventions of obs-curves/native/curve_engine.* so the two
 * engines can later live in one translation unit.
 *
 * All values are the *neutral-anchored* UI values, exactly as displayed:
 *   lift 0.00   gamma 0.00   gain 1.00   offset 25.00
 *   contrast 1.000  pivot 0.435  sat 50.00  hue 50.00  lumMix 100.00
 * Every control at its neutral value is a bit-exact identity.
 */
#ifndef GRADE_ENGINE_H
#define GRADE_ENGINE_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GRADE_PI 3.14159265358979f

typedef struct {
	/* Neutral 0.00 -> 1.00; neutral 25.00 -> 0.00; neutral 1.00 -> 1.00 */
	float lift[3];   /* additive, pivoted at black */
	float gamma[3];  /* deviation from 1.0; exponent is (1 + gamma) */
	float gain[3];   /* multiplier */
	float offset[3]; /* UI 25.00 is neutral */

	float contrast;  /* 1.000 */
	float pivot;     /* 0.435 */

	float sat;       /* 50.00 -> 1.0 */
	float hue;       /* 50.00 -> 0 rad */
	float lum_mix;   /* 100.00 -> 1.0 */

	/* Approximation tier: 0.00 is neutral for all three. */
	float boost;     /* colour boost, strongest in mid-tones */
	float shadows;   /* shadow-only lift */
	float highlights;/* highlight-only gain */
} grade_params_t;

void grade_defaults(grade_params_t *p);
bool grade_is_neutral(const grade_params_t *p);

/* Full chain. Safe for out-of-range values (signed pow, no NaN). */
void grade_eval_rgb(const grade_params_t *p, const float in[3], float out[3]);

/* Tone-only chain for one channel, used by the editor's response graph and
 * by the unit test.  Excludes saturation / hue / luminance mix. */
float grade_eval_channel(const grade_params_t *p, int ch, float x);

/* Wheel pad (dx,dy in -1..1, +y up) -> per-channel delta.
 * The three lobes sum to zero, so a hue push never changes luminance. */
void grade_wheel_to_rgb(float dx, float dy, float radius_scale, float out[3]);

/* Chroma-plane wheel coefficients, derived from the Rec.709 luma weights so a
 * push cancels exactly under 0.2126/0.7152/0.0722 instead of leaving a residue.
 * Shared by the engine and the editor's wheel bitmap. */
#define GRADE_CW_CR_R ((1.0f - 0.2126f) / (1.0f - 0.0722f))
#define GRADE_CW_CR_G \
	(((1.0f - 0.2126f) * 0.2126f / 0.7152f) / (1.0f - 0.0722f))
#define GRADE_CW_CB_G (0.0722f / 0.7152f)
#define GRADE_CW_CB_B 1.0f

/* Unit RGB-offset direction for a wheel vector, ignoring radius.  Red lands at
 * the top of the disc, blue at the right and green at the lower left, matching
 * Resolve's chroma-plane layout.  Luma-preserving. */
void grade_wheel_direction(float dx, float dy, float out[3]);

/* ---- UI value -> internal parameter, calibrated against DaVinci Resolve ----
 * These live here (not in the filter) so the test suite can exercise them.
 */
#define GRADE_LIFT_UI_TO_INTERNAL 2.0f   /* lift 0.50 -> internal 1.00 */
#define GRADE_OFFSET_UI_UNIT 0.0073327f  /* (ui-25) * this; ends give +-0.1833 */

/* Exponent for the gamma wheel.  Positive side is 1 + 4*ui (0.25 -> 2.0,
 * 0.50 -> 3.0); the negative side is the reciprocal so the exponent never
 * reaches zero or goes negative. */
float grade_gamma_exponent(float ui);

/* Contrast is a normalised logistic around the pivot.  Fills
 * out = (k, pivot, s0, 1/(s1-s0)); k == 0 means identity. */
void grade_contrast_uniforms(float ui, float pivot, float out[4]);

#ifdef __cplusplus
}
#endif

#endif
