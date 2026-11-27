/* Offline self-test for the grade engine.  Mirrors obs-curves/native/curve_test.c:
 * build with gcc, run, expect exit 0.  The properties checked here are the ones
 * that actually broke in obs-curves (neutral != identity, wrong offset scaling),
 * so they are asserted rather than eyeballed. */
#include "grade_engine.h"

#include <math.h>
#include <stdio.h>

static int g_fail;

static void check(const char *name, int pass, const char *detail)
{
	printf("%s  %s%s%s\n", pass ? "PASS" : "FAIL", name,
	       detail && detail[0] ? "  " : "", detail ? detail : "");
	if (!pass)
		g_fail++;
}

static int near(float a, float b, float eps)
{
	return fabsf(a - b) <= eps;
}

int main(void)
{
	grade_params_t p;
	float in[3], out[3];

	/* P1: every control at neutral is a bit-exact identity. */
	grade_defaults(&p);
	{
		float probes[5][3] = {{0.0f, 0.0f, 0.0f},
				      {1.0f, 1.0f, 1.0f},
				      {0.5f, 0.5f, 0.5f},
				      {0.18f, 0.18f, 0.18f},
				      {0.8f, 0.2f, 0.1f}};
		int ok = 1;
		for (int i = 0; i < 5; i++) {
			grade_eval_rgb(&p, probes[i], out);
			for (int c = 0; c < 3; c++)
				if (!near(out[c], probes[i][c], 1e-6f))
					ok = 0;
		}
		check("P1 neutral == identity", ok, "");
		check("P1b grade_is_neutral", grade_is_neutral(&p) == 1, "");
	}

	/* P2: lift moves the black point and leaves white alone. */
	grade_defaults(&p);
	p.lift[0] = p.lift[1] = p.lift[2] = 0.1f;
	in[0] = in[1] = in[2] = 0.0f;
	grade_eval_rgb(&p, in, out);
	check("P2 lift: black -> 0.1", near(out[0], 0.1f, 1e-5f), "");
	in[0] = in[1] = in[2] = 1.0f;
	grade_eval_rgb(&p, in, out);
	check("P2 lift: white stays 1", near(out[0], 1.0f, 1e-5f), "");

	/* P3: gain 2 doubles mid grey, leaves black. */
	grade_defaults(&p);
	p.gain[0] = p.gain[1] = p.gain[2] = 2.0f;
	in[0] = in[1] = in[2] = 0.5f;
	grade_eval_rgb(&p, in, out);
	check("P3 gain 2: 0.5 -> 1.0", near(out[0], 1.0f, 1e-5f), "");
	in[0] = in[1] = in[2] = 0.0f;
	grade_eval_rgb(&p, in, out);
	check("P3 gain 2: black stays 0", near(out[0], 0.0f, 1e-6f), "");

	/* P4: gamma +1 means exponent 2 -> sqrt in the midtones. */
	grade_defaults(&p);
	p.gamma[0] = p.gamma[1] = p.gamma[2] = 1.0f;
	in[0] = in[1] = in[2] = 0.5f;
	grade_eval_rgb(&p, in, out);
	check("P4 gamma exp2: 0.5 -> 0.7071", near(out[0], 0.70710678f, 1e-4f),
	      "");

	/* P5: contrast pins the pivot exactly. */
	{
		int ok = 1;
		const float cs[4] = {0.5f, 1.0f, 1.7f, 3.0f};
		for (int i = 0; i < 4; i++) {
			grade_defaults(&p);
			p.contrast = cs[i];
			in[0] = in[1] = in[2] = 0.435f;
			grade_eval_rgb(&p, in, out);
			if (!near(out[0], 0.435f, 1e-5f))
				ok = 0;
		}
		check("P5 contrast pins pivot 0.435", ok, "");
	}

	/* P6: a wheel push is luminance neutral.  With the chroma-plane mapping the
	 * deltas no longer sum to zero (blue alone is worth more than red alone),
	 * so the property has to be stated with Rec.709 weights, which is the
	 * correct one anyway. */
	{
		int ok = 1;
		float worst = 0.0f;
		for (int a = 0; a < 360; a += 5) {
			const float th = (float)a * 3.14159265f / 180.0f;
			float d[3];
			grade_wheel_to_rgb(cosf(th), sinf(th), 0.35f, d);
			const float y = 0.2126f * d[0] + 0.7152f * d[1] +
					0.0722f * d[2];
			if (fabsf(y) > worst)
				worst = fabsf(y);
			if (fabsf(y) > 1e-5f)
				ok = 0;
		}
		char d[64];
		snprintf(d, sizeof(d), "max |dY| = %.2e", worst);
		check("P6 wheel push is luminance neutral", ok, d);
	}

	/* P6b: hue placement.  Red at the top, blue at the right, green at the
	 * lower left -- Resolve's layout, not an even HSV ring. */
	{
		float dr[3], db[3], dg[3];
		grade_wheel_to_rgb(0.0f, -1.0f, 1.0f, dr);  /* up */
		grade_wheel_to_rgb(1.0f, 0.0f, 1.0f, db);   /* right */
		grade_wheel_to_rgb(-0.6f, 0.8f, 1.0f, dg);  /* lower left */
		int ok = dr[0] > dr[1] && dr[0] > dr[2];    /* up = red */
		ok = ok && db[2] > db[0] && db[2] > db[1];  /* right = blue */
		ok = ok && dg[1] > dg[0] && dg[1] > dg[2];  /* lower-left = green */
		char d[128];
		snprintf(d, sizeof(d), "up=(%.2f,%.2f,%.2f) right=(%.2f,%.2f,%.2f)",
			 dr[0], dr[1], dr[2], db[0], db[1], db[2]);
		check("P6b red up / blue right / green lower-left", ok, d);
	}

	/* P7: saturation 0 collapses to Rec.709 luma; neutral stays put. */
	grade_defaults(&p);
	p.sat = 0.0f;
	in[0] = 0.8f; in[1] = 0.2f; in[2] = 0.1f;
	grade_eval_rgb(&p, in, out);
	{
		const float y = 0.2126f * 0.8f + 0.7152f * 0.2f + 0.0722f * 0.1f;
		check("P7 sat 0 -> luma grey",
		      near(out[0], y, 1e-4f) && near(out[1], y, 1e-4f) &&
			      near(out[2], y, 1e-4f),
		      "");
	}
	grade_defaults(&p);
	in[0] = 0.8f; in[1] = 0.2f; in[2] = 0.1f;
	grade_eval_rgb(&p, in, out);
	check("P7b sat 50 -> identity",
	      near(out[0], in[0], 1e-6f) && near(out[1], in[1], 1e-6f) &&
		      near(out[2], in[2], 1e-6f),
	      "");

	/* P8: hue +25 degrees then -25 degrees round trips. */
	grade_defaults(&p);
	in[0] = 0.8f; in[1] = 0.2f; in[2] = 0.1f;
	p.hue = 75.0f;
	grade_eval_rgb(&p, in, out);
	p.hue = 25.0f;
	grade_eval_rgb(&p, out, out);
	/* float32 keeps ~7 digits: a +90/-90 degree round trip lands within ~5e-5,
	 * which is far below one 8-bit LSB (3.9e-3). */
	check("P8 hue round trip",
	      near(out[0], in[0], 2e-3f) && near(out[1], in[1], 2e-3f) &&
		      near(out[2], in[2], 2e-3f),
	      "");

	/* ---- P9: UI -> internal mapping anchors (from the Resolve LUTs) ---- */
	check("P9 gamma exp(0)=1", near(grade_gamma_exponent(0.0f), 1.0f, 1e-6f), "");
	check("P9 gamma exp(0.25)=2", near(grade_gamma_exponent(0.25f), 2.0f, 1e-5f), "");
	check("P9 gamma exp(0.5)=3", near(grade_gamma_exponent(0.5f), 3.0f, 1e-5f), "");
	check("P9 gamma exp(-0.25)=0.5", near(grade_gamma_exponent(-0.25f), 0.5f, 1e-5f), "");
	check("P9 gamma exp(-1)=0.2", near(grade_gamma_exponent(-1.0f), 0.2f, 1e-5f), "");

	/* ---- P10: the exponent must stay positive over the whole slider.
	 *          A linear extrapolation to the negative side hits zero at -0.25
	 *          and goes negative below that, and pow() then explodes. ---- */
	{
		int ok = 1;
		float worst = 1e9f;
		for (float u = -1.0f; u <= 1.0001f; u += 0.01f) {
			const float e = grade_gamma_exponent(u);
			if (e < worst)
				worst = e;
			if (!(e > 0.0f) || e > 1e4f)
				ok = 0;
		}
		char d[64];
		snprintf(d, sizeof(d), "min exponent %.4f", worst);
		check("P10 gamma exponent stays positive", ok, d);
	}

	/* ---- P11: contrast uniforms finite everywhere, identity at ui = 1 ---- */
	{
		float cu[4];
		int ok = 1;
		grade_contrast_uniforms(1.0f, 0.435f, cu);
		if (cu[0] != 0.0f)
			ok = 0;
		for (float u = 0.0f; u <= 2.0001f; u += 0.01f) {
			grade_contrast_uniforms(u, 0.435f, cu);
			for (int i = 0; i < 4; i++)
				if (!isfinite(cu[i]))
					ok = 0;
		}
		check("P11 contrast uniforms finite, identity at 1.0", ok, "");
	}

	/* ---- P12: every control, over its whole slider, must produce a finite
	 *          and monotone transfer.  This is the regression net for the
	 *          "negative gamma explodes" class of bug. ---- */
	{
		struct {
			const char *name;
			float lo, hi;
		} ranges[5] = {
			{"lift", -0.5f, 0.5f},
			{"gamma", -1.0f, 1.0f},
			{"gain", 0.0f, 2.0f},
			{"offset", 0.0f, 50.0f},
			{"contrast", 0.0f, 2.0f},
		};
		int ok = 1;
		char why[128] = "";
		for (int c = 0; c < 5 && ok; c++) {
			for (float u = ranges[c].lo; u <= ranges[c].hi + 1e-4f; u += 0.05f) {
				grade_defaults(&p);
				switch (c) {
				case 0: p.lift[0] = p.lift[1] = p.lift[2] = u; break;
				case 1: p.gamma[0] = p.gamma[1] = p.gamma[2] = u; break;
				case 2: p.gain[0] = p.gain[1] = p.gain[2] = u; break;
				case 3: p.offset[0] = p.offset[1] = p.offset[2] = u; break;
				default: p.contrast = u; break;
				}
				float prev = -1e9f;
				for (float x = 0.0f; x <= 1.0001f; x += 0.02f) {
					float in[3] = {x, x, x}, outp[3];
					grade_eval_rgb(&p, in, outp);
					for (int k = 0; k < 3; k++) {
						if (!isfinite(outp[k])) {
							ok = 0;
							snprintf(why, sizeof(why), "%s ui=%.2f NaN/Inf",
								 ranges[c].name, u);
						}
					}
					if (outp[0] < prev - 1e-4f) {
						ok = 0;
						snprintf(why, sizeof(why), "%s ui=%.2f non-monotone",
							 ranges[c].name, u);
					}
					prev = outp[0];
				}
				if (!ok)
					break;
			}
		}
		check("P12 all controls finite + monotone", ok, why);
	}

	printf("\n%s (%d failure%s)\n", g_fail ? "FAILED" : "ALL PASS", g_fail,
	       g_fail == 1 ? "" : "s");
	return g_fail ? 1 : 0;
}
