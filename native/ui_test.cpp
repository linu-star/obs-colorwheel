/* Interaction test for the colour-wheel editor.
 *
 * Drives the real window with synthetic messages and asserts observable
 * behaviour.  This exists because three bugs reached the user that a compile
 * could never catch: the brightness strip not dragging (the timer only serviced
 * wheel drags), the close button being swallowed by HTCAPTION, and negative
 * gamma producing a zero exponent.  Build with -DCW_TEST.
 */
#include <windows.h>

#include <math.h>
#include <stdio.h>

#include "colorwheel_editor.h"
#include "grade_engine.h"

extern "C" void cw_test_layout(int w, int *sl, int *st, int *sw, int *sh,
			       int *wl, int *wt, int *ww, int *wh);
extern "C" int cw_test_knob(int w, float *ux, float *uy);
extern "C" void cw_test_readout(int w, int ch, float *cx, float *cy);
extern "C" void cw_test_reset(int w, float *cx, float *cy);
extern "C" int cw_test_editing(void);

static grade_params_t P;
static int g_apply;
static int g_fail;

static void check(const char *name, int ok, const char *detail)
{
	printf("%s  %s%s%s\n", ok ? "PASS" : "FAIL", name,
	       detail && detail[0] ? "  " : "", detail ? detail : "");
	if (!ok)
		g_fail++;
}

static void fake_apply(void *) { g_apply++; }
static void fake_dirty(void *) {}
static const char *fake_name(void *) { return "harness"; }
static void fake_size(void *, int *w, int *h) { *w = 1920; *h = 1080; }

static void pump(int ms)
{
	MSG msg;
	const DWORD end = GetTickCount() + (DWORD)ms;
	while (GetTickCount() < end) {
		while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
			TranslateMessage(&msg);
			DispatchMessageW(&msg);
		}
		Sleep(5);
	}
}

/* The brightness of a wheel is its luma, not the plain mean: the push is
 * luminance neutral but not mean neutral, so the mean moves when you push. */
static float master_lift(void)
{
	return 0.2126f * P.lift[0] + 0.7152f * P.lift[1] + 0.0722f * P.lift[2];
}

int main(void)
{
	grade_defaults(&P);

	colorwheel_editor_host host = {};
	host.filter = nullptr;
	host.params = &P;
	host.get_source_name = fake_name;
	host.get_target_size = fake_size;
	host.apply = fake_apply;
	host.mark_dirty = fake_dirty;

	obs_colorwheel_editor_open(&host);
	pump(250);

	HWND h = FindWindowW(L"OBSColorWheelEditor", nullptr);
	check("editor window created", h != nullptr, "");
	if (!h)
		return 1;

	int sl, st, sw, sh, wl, wt, ww, wh;
	cw_test_layout(0, &sl, &st, &sw, &sh, &wl, &wt, &ww, &wh);
	{
		char d[160];
		snprintf(d, sizeof(d), "slider=(%d,%d %dx%d) wheel=(%d,%d %dx%d)",
			 sl, st, sw, sh, wl, wt, ww, wh);
		check("layout resolved", sw > 10 && ww > 40 && sh > 2, d);
	}

	/* ---- A. brightness strip: click at the right end, then drag left.
	 *         The drag half is the regression for "slider cannot be dragged":
	 *         it only passes if WM_MOUSEMOVE + WM_TIMER actually re-apply. */
	grade_defaults(&P);
	SendMessageW(h, WM_LBUTTONDOWN, 0, MAKELPARAM(sl + sw - 1, st + sh / 2));
	const float m_click = master_lift();
	SendMessageW(h, WM_MOUSEMOVE, 0, MAKELPARAM(sl + 1, st + sh / 2));
	SendMessageW(h, WM_TIMER, 1, 0);
	const float m_drag = master_lift();
	SendMessageW(h, WM_LBUTTONUP, 0, MAKELPARAM(sl + 1, st + sh / 2));
	{
		char d[128];
		snprintf(d, sizeof(d), "click=%.3f drag=%.3f", m_click, m_drag);
		check("slider click at right end -> +0.5", fabsf(m_click - 0.5f) < 0.06f, d);
		check("slider DRAG to left end -> -0.5", fabsf(m_drag + 0.5f) < 0.06f, "");
		check("mouse up commits via apply()", g_apply > 0, "");
	}

	/* ---- B. wheel disc push must be luminance neutral (delta sum == 0) ---- */
	grade_defaults(&P);
	const int cx = wl + ww / 2, cy = wt + wh / 2;
	SendMessageW(h, WM_LBUTTONDOWN, 0, MAKELPARAM(cx, cy));
	SendMessageW(h, WM_MOUSEMOVE, 0, MAKELPARAM(wl + ww - 6, cy));
	SendMessageW(h, WM_TIMER, 1, 0);
	SendMessageW(h, WM_LBUTTONUP, 0, MAKELPARAM(wl + ww - 6, cy));
	{
		char d[128];
		snprintf(d, sizeof(d), "lift=%.3f/%.3f/%.3f", P.lift[0], P.lift[1], P.lift[2]);
		check("wheel drag right pushes blue", P.lift[2] > 0.05f, d);
		check("wheel drag is luminance neutral",
		      fabsf(0.2126f * P.lift[0] + 0.7152f * P.lift[1] +
			    0.0722f * P.lift[2]) < 0.01f,
		      "");
	}

	/* ---- B2. a click in the bounding-box corner (outside the disc) must NOT
	 *          start a wheel drag: that made the knob jump to the rim instead
	 *          of landing under the cursor. ---- */
	grade_defaults(&P);
	SendMessageW(h, WM_LBUTTONDOWN, 0, MAKELPARAM(wl + 2, wt + 2));
	SendMessageW(h, WM_LBUTTONUP, 0, MAKELPARAM(wl + 2, wt + 2));
	check("click outside the disc is ignored",
	      fabsf(P.lift[0]) < 1e-6f && fabsf(P.lift[1]) < 1e-6f &&
		      fabsf(P.lift[2]) < 1e-6f,
	      "");

	/* ---- B3. the knob must sit where the mouse pressed, and the colour must
	 *          match the disc at that spot.  Regression for "the wheel pushes
	 *          in the opposite direction": the knob used to be drawn mirrored
	 *          vertically, so dragging to green parked it on magenta. ---- */
	{
		const float discR = (ww * 0.5f) * 0.83f;
		const int ccx = wl + ww / 2, ccy = wt + wh / 2;

		/* press above the centre -> green goes up, blue goes down */
		grade_defaults(&P);
		SendMessageW(h, WM_LBUTTONDOWN, 0,
			     MAKELPARAM(ccx, ccy - (int)(discR * 0.6f)));
		SendMessageW(h, WM_LBUTTONUP, 0,
			     MAKELPARAM(ccx, ccy - (int)(discR * 0.6f)));
		float ux = 0, uy = 0;
		cw_test_knob(0, &ux, &uy);
		{
			char d[128];
			snprintf(d, sizeof(d), "knob=(%.2f,%.2f) lift=%.2f/%.2f/%.2f",
				 ux, uy, P.lift[0], P.lift[1], P.lift[2]);
			check("knob follows the mouse upward",
			      uy < -0.4f && fabsf(ux) < 0.12f, d);
			check("press above centre pushes red",
			      P.lift[0] > P.lift[1] && P.lift[0] > P.lift[2], "");
		}

		/* press to the right -> red */
		grade_defaults(&P);
		SendMessageW(h, WM_LBUTTONDOWN, 0,
			     MAKELPARAM(ccx + (int)(discR * 0.6f), ccy));
		SendMessageW(h, WM_LBUTTONUP, 0,
			     MAKELPARAM(ccx + (int)(discR * 0.6f), ccy));
		cw_test_knob(0, &ux, &uy);
		{
			char d[128];
			snprintf(d, sizeof(d), "knob=(%.2f,%.2f) lift=%.2f/%.2f/%.2f",
				 ux, uy, P.lift[0], P.lift[1], P.lift[2]);
			check("knob follows the mouse rightward",
			      ux > 0.4f && fabsf(uy) < 0.12f, d);
			check("press right pushes blue",
			      P.lift[2] > P.lift[0] && P.lift[2] > P.lift[1], "");
		}
	}

	/* ---- B4. Holding the mouse still must not drift the value.  The push is
	 *          luminance neutral but not mean neutral, so a mean-based master
	 *          added part of the push back into brightness on every mouse
	 *          event: values ran away and the knob never tracked the cursor. */
	{
		grade_defaults(&P);
		const float discR = (ww * 0.5f) * 0.83f;
		const int ccx = wl + ww / 2, ccy = wt + wh / 2;
		const int mx = ccx + (int)(discR * 0.55f);
		SendMessageW(h, WM_LBUTTONDOWN, 0, MAKELPARAM(mx, ccy));
		const float first = P.lift[2];
		for (int i = 0; i < 12; i++) {
			SendMessageW(h, WM_MOUSEMOVE, 0, MAKELPARAM(mx, ccy));
			SendMessageW(h, WM_TIMER, 1, 0);
		}
		SendMessageW(h, WM_LBUTTONUP, 0, MAKELPARAM(mx, ccy));
		char d[96];
		snprintf(d, sizeof(d), "first=%.3f after12=%.3f", first, P.lift[2]);
		check("holding the mouse still does not drift",
		      fabsf(P.lift[2] - first) < 0.01f, d);
	}

	/* ---- G. the per-wheel reset button clears only its own range ---- */
	{
		float rx, ry;
		grade_defaults(&P);
		P.lift[0] = 0.30f;
		P.lift[1] = 0.10f;
		P.lift[2] = 0.00f;
		P.gamma[0] = P.gamma[1] = P.gamma[2] = 0.20f;
		cw_test_reset(0, &rx, &ry);
		SendMessageW(h, WM_LBUTTONDOWN, 0, MAKELPARAM((int)rx, (int)ry));
		SendMessageW(h, WM_LBUTTONUP, 0, MAKELPARAM((int)rx, (int)ry));
		{
			char d[128];
			snprintf(d, sizeof(d),
				 "lift=%.2f/%.2f/%.2f gamma0=%.2f", P.lift[0],
				 P.lift[1], P.lift[2], P.gamma[0]);
			check("reset button clears its own wheel",
			      fabsf(P.lift[0]) < 1e-4f &&
				      fabsf(P.lift[1]) < 1e-4f &&
				      fabsf(P.lift[2]) < 1e-4f,
			      d);
			check("reset button leaves other wheels alone",
			      fabsf(P.gamma[0] - 0.20f) < 1e-4f, "");
		}
	}

	/* ---- C. double-click on the strip resets that wheel ---- */
	SendMessageW(h, WM_LBUTTONDBLCLK, 0, MAKELPARAM(sl + sw - 1, st + sh / 2));
	check("double-click resets brightness", fabsf(master_lift()) < 0.02f, "");

	/* ---- F. the numeric readouts are editable ---- */
	{
		float cx0, cy0;

		grade_defaults(&P);
		cw_test_readout(0, 0, &cx0, &cy0);
		SendMessageW(h, WM_LBUTTONDBLCLK, 0,
			     MAKELPARAM((int)cx0, (int)cy0));
		check("double-click opens the numeric editor",
		      cw_test_editing() != 0, "");
		SendMessageW(h, WM_CHAR, L'0', 0);
		SendMessageW(h, WM_CHAR, L'.', 0);
		SendMessageW(h, WM_CHAR, L'4', 0);
		SendMessageW(h, WM_CHAR, L'2', 0);
		SendMessageW(h, WM_KEYDOWN, VK_RETURN, 0);
		{
			char d[64];
			snprintf(d, sizeof(d), "liftR=%.3f want 0.420", P.lift[0]);
			check("typed value is committed",
			      fabsf(P.lift[0] - 0.42f) < 0.001f, d);
		}

		/* Out-of-range input must clamp to the channel range, not be taken
		 * literally (lift is +-1). */
		cw_test_readout(0, 0, &cx0, &cy0);
		SendMessageW(h, WM_LBUTTONDBLCLK, 0,
			     MAKELPARAM((int)cx0, (int)cy0));
		SendMessageW(h, WM_CHAR, L'9', 0);
		SendMessageW(h, WM_CHAR, L'9', 0);
		SendMessageW(h, WM_KEYDOWN, VK_RETURN, 0);
		{
			char d[64];
			snprintf(d, sizeof(d), "liftR=%.3f want 1.000", P.lift[0]);
			check("out-of-range input clamps",
			      fabsf(P.lift[0] - 1.0f) < 0.001f, d);
		}

		/* Dragging a readout scrubs that channel. */
		grade_defaults(&P);
		float gx, gy;
		cw_test_readout(0, 1, &gx, &gy);
		SendMessageW(h, WM_LBUTTONDOWN, 0,
			     MAKELPARAM((int)gx, (int)gy));
		SendMessageW(h, WM_MOUSEMOVE, 0,
			     MAKELPARAM((int)gx + 60, (int)gy));
		SendMessageW(h, WM_TIMER, 1, 0);
		SendMessageW(h, WM_LBUTTONUP, 0,
			     MAKELPARAM((int)gx + 60, (int)gy));
		{
			char d[64];
			snprintf(d, sizeof(d), "liftG=%.3f", P.lift[1]);
			check("dragging a readout scrubs it", P.lift[1] > 0.05f,
			      d);
		}
	}

	/* ---- E. keyboard: Tab focuses a wheel, arrows nudge its brightness ---- */
	grade_defaults(&P);
	SendMessageW(h, WM_KEYDOWN, VK_TAB, 0);
	SendMessageW(h, WM_KEYDOWN, VK_RIGHT, 0);
	{
		char d[64];
		snprintf(d, sizeof(d), "lift0=%.4f", P.lift[0]);
		check("Tab then Right nudges brightness", P.lift[0] > 0.005f, d);
	}
	SendMessageW(h, WM_KEYDOWN, VK_LEFT, 0);
	check("Left undoes the nudge", fabsf(P.lift[0]) < 1e-4f, "");

	/* ---- D. close button: hit test must be HTCLIENT, and the click must hide
	 *         the window.  Previously HTCAPTION swallowed it. ---- */
	RECT rc, wr;
	GetClientRect(h, &rc);
	GetWindowRect(h, &wr);
	const LRESULT ht = SendMessageW(h, WM_NCHITTEST, 0,
					MAKELPARAM(wr.right - 28, wr.top + 20));
	{
		char d[64];
		snprintf(d, sizeof(d), "nchittest=%ld want %d", (long)ht, (int)HTCLIENT);
		check("close button reports HTCLIENT", ht == HTCLIENT, d);
	}
	/* Buttons act on release now, like a normal desktop button. */
	SendMessageW(h, WM_LBUTTONDOWN, 0, MAKELPARAM(rc.right - 28, 20));
	SendMessageW(h, WM_LBUTTONUP, 0, MAKELPARAM(rc.right - 28, 20));
	pump(60);
	check("close button hides the window", !IsWindowVisible(h), "");

	printf("\n%s (%d failure%s)\n", g_fail ? "FAILED" : "ALL PASS", g_fail,
	       g_fail == 1 ? "" : "s");
	return g_fail ? 1 : 0;
}
