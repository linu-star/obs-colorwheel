/* Primary-grade maths.  See grade_engine.h.  No libobs, no allocation. */
#include "grade_engine.h"

#include <math.h>

/* BT.709 luma, coefficients derived so the Y'CbCr pair below is exactly
 * inverse (the rounded 1.5748 / 0.4681 forms drift on a round trip). */
#define KR 0.2126f
#define KG 0.7152f
#define KB 0.0722f
#define CR_R 1.5748f          /* 2*(1-KR) */
#define CB_B 1.8556f          /* 2*(1-KB) */
#define CR_G 0.4680823f       /* 2*(1-KR)*KR/KG */
#define CB_G 0.1873413f       /* 2*(1-KB)*KB/KG */

/* Offset is scaled so UI 50 / 0 give +-0.25: (v - 25) * 0.01. */
#define OFFSET_UNIT 0.01f

static float clampf(float v, float lo, float hi)
{
	return v < lo ? lo : (v > hi ? hi : v);
}

/* pow() is NaN for a negative base; graded data goes negative easily. */
static float spow(float v, float e)
{
	return v < 0.0f ? -powf(-v, e) : powf(v, e);
}

static float luma709(const float c[3])
{
	return KR * c[0] + KG * c[1] + KB * c[2];
}

void grade_defaults(grade_params_t *p)
{
	if (!p)
		return;
	for (int i = 0; i < 3; i++) {
		p->lift[i] = 0.0f;
		p->gamma[i] = 0.0f;
		p->gain[i] = 1.0f;
		p->offset[i] = 25.0f;
	}
	p->contrast = 1.0f;
	p->pivot = 0.435f;
	p->sat = 50.0f;
	p->hue = 50.0f;
	p->lum_mix = 100.0f;
	p->boost = 0.0f;
	p->shadows = 0.0f;
	p->highlights = 0.0f;
}

bool grade_is_neutral(const grade_params_t *p)
{
	if (!p)
		return true;
	for (int i = 0; i < 3; i++) {
		if (p->lift[i] != 0.0f || p->gamma[i] != 0.0f ||
		    p->gain[i] != 1.0f || p->offset[i] != 25.0f)
			return false;
	}
	return p->contrast == 1.0f && p->pivot == 0.435f && p->sat == 50.0f &&
	       p->hue == 50.0f && p->lum_mix == 100.0f && p->boost == 0.0f &&
	       p->shadows == 0.0f && p->highlights == 0.0f;
}

/* Tone chain for a single channel: offset -> lift/gain -> gamma -> contrast.
 * Shared by the shader (same order) and by grade_eval_channel(). */
static float tone_channel(const grade_params_t *p, int ch, float x)
{
	const float off = (p->offset[ch] - 25.0f) * OFFSET_UNIT;
	const float g = 1.0f + p->gamma[ch];
	const float k = p->gain[ch];
	const float l = p->lift[ch];

	float v = x + off;
	v = v * (k - l) + l;
	if (g != 1.0f)
		v = spow(v, 1.0f / g);
	if (p->contrast != 1.0f || p->pivot != 0.435f)
		v = spow(v / p->pivot, p->contrast) * p->pivot;
	return v;
}

float grade_eval_channel(const grade_params_t *p, int ch, float x)
{
	if (!p || ch < 0 || ch > 2)
		return x;
	return tone_channel(p, ch, x);
}

void grade_eval_rgb(const grade_params_t *p, const float in[3], float out[3])
{
	if (!p) {
		out[0] = in[0];
		out[1] = in[1];
		out[2] = in[2];
		return;
	}

	float c[3];
	for (int i = 0; i < 3; i++)
		c[i] = tone_channel(p, i, in[i]);

	const float y = luma709(c);

	/* Approximation tier (neutral 0.0 == identity). */
	if (p->shadows != 0.0f) {
		const float w = (1.0f - clampf(y, 0.0f, 1.0f)) * p->shadows * 0.1f;
		for (int i = 0; i < 3; i++)
			c[i] += w;
	}
	if (p->highlights != 0.0f) {
		const float w = clampf(y, 0.0f, 1.0f) * p->highlights * 0.1f;
		for (int i = 0; i < 3; i++)
			c[i] *= 1.0f + w;
	}

	if (p->sat != 50.0f || p->hue != 50.0f || p->boost != 0.0f) {
		float s = p->sat / 50.0f;
		if (p->boost != 0.0f) {
			/* strongest in the mid-tones, zero at both ends */
			const float w = 1.0f - fabsf(2.0f * clampf(y, 0.0f, 1.0f) - 1.0f);
			s *= 1.0f + p->boost * w;
		}
		if (s < 0.0f)
			s = 0.0f;

		float cb = (c[2] - y) / CB_B;
		float cr = (c[0] - y) / CR_R;

		if (p->hue != 50.0f) {
			const float h = (p->hue - 50.0f) / 50.0f * GRADE_PI;
			const float ca = cosf(h), sa = sinf(h);
			const float cb2 = cb * ca - cr * sa;
			const float cr2 = cb * sa + cr * ca;
			cb = cb2;
			cr = cr2;
		}

		c[0] = y + CR_R * cr;
		c[1] = y - CR_G * cr - CB_G * cb;
		c[2] = y + CB_B * cb;

		/* Luma-preserving chroma gain: a neutral pixel has c[i] == y, so it
		 * can never be tinted, and full desaturation lands on luma. */
		for (int i = 0; i < 3; i++)
			c[i] = y + s * (c[i] - y);
	}

	if (p->lum_mix != 100.0f) {
		const float m = clampf(p->lum_mix / 100.0f, 0.0f, 1.0f);
		const float yin = luma709(in);
		const float yout = luma709(c);
		for (int i = 0; i < 3; i++)
			c[i] += (yin - yout) * (1.0f - m);
	}

	out[0] = c[0];
	out[1] = c[1];
	out[2] = c[2];
}

/* ------------------------------------------------------- UI -> internal --- */

float grade_gamma_exponent(float ui)
{
	if (ui >= 0.0f)
		return 1.0f + ui * 4.0f;
	const float d = 1.0f - ui * 4.0f;
	return d > 1e-4f ? 1.0f / d : 1e4f;
}

void grade_contrast_uniforms(float ui, float pivot, float out[4])
{
	const float d = ui - 1.0f;
	float k = 0.0f;
	if (d > 0.0f) {
		if (d <= 0.5f)
			k = d / 0.5f * 9.48f;
		else if (ui >= 2.0f)
			k = 528.0f;
		else
			k = 9.48f * powf(528.0f / 9.48f, (d - 0.5f) / 0.5f);
	} else if (d < 0.0f) {
		const float a = -d;
		k = (a > 1.0f) ? -528.0f : -a / 0.5f * 9.48f;
	}

	out[1] = pivot;
	if (k > -1e-3f && k < 1e-3f) {
		out[0] = 0.0f;
		out[2] = 0.5f;
		out[3] = 1.0f;
		return;
	}
	const float s0 = 1.0f / (1.0f + expf(-k * (0.0f - pivot)));
	const float s1 = 1.0f / (1.0f + expf(-k * (1.0f - pivot)));
	const float dd = s1 - s0;
	out[0] = k;
	out[2] = s0;
	out[3] = (dd > 1e-6f || dd < -1e-6f) ? 1.0f / dd : 1.0f;
}

/* Coefficients live in grade_engine.h so the editor's bitmap uses the same
 * numbers; see there for why they are derived rather than rounded. */
#define CW_CR_R GRADE_CW_CR_R
#define CW_CR_G GRADE_CW_CR_G
#define CW_CB_G GRADE_CW_CB_G
#define CW_CB_B GRADE_CW_CB_B

/* Unit RGB-offset direction for a wheel vector, ignoring radius.
 *
 * Screen space is x right / y down; the chroma plane is Cb right / Cr up.  This
 * mapping is what places red at the top, blue at the right and green at the
 * lower left -- DaVinci Resolve's layout -- instead of the even 120 degree
 * spacing of an HSV ring.  It also reproduces Resolve's uneven wedge widths:
 * yellow lands in a narrow gap while green and cyan spread out.
 *
 * Luma is preserved: 0.2126*dR + 0.7152*dG + 0.0722*dB == 0 for every
 * direction, because the coefficients are a scaled YCbCr inverse with Y = 0.
 */
void grade_wheel_direction(float dx, float dy, float out[3])
{
	const float r = sqrtf(dx * dx + dy * dy);
	if (r <= 1e-9f) {
		out[0] = out[1] = out[2] = 0.0f;
		return;
	}
	const float cb = dx / r;
	const float cr = -dy / r;
	out[0] = CW_CR_R * cr;
	out[1] = -(CW_CR_G * cr + CW_CB_G * cb);
	out[2] = CW_CB_B * cb;
}

void grade_wheel_to_rgb(float dx, float dy, float radius_scale, float out[3])
{
	const float r = sqrtf(dx * dx + dy * dy);
	if (r <= 1e-9f) {
		out[0] = out[1] = out[2] = 0.0f;
		return;
	}
	const float rr = r > 1.0f ? 1.0f : r;
	float dir[3];
	grade_wheel_direction(dx, dy, dir);
	const float k = rr * radius_scale;
	out[0] = dir[0] * k;
	out[1] = dir[1] * k;
	out[2] = dir[2] * k;
}
