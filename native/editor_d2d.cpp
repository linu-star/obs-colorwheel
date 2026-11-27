// obs-colorwheel editor: full-window D3D11 swapchain + Direct2D + DirectWrite.
// Same shape as obs-curves/native/editor_d2d.cpp:
//   WS_POPUP | WS_CAPTION | WS_THICKFRAME with a custom title bar
//   IDXGISwapChain1 (FLIP_DISCARD) -> ID2D1DeviceContext -> self-drawn controls
//   no COMBOBOX, no child popups, no native controls
//
// Colour wheels are rendered from a CPU-generated BGRA bitmap that is rebuilt
// only when the size or DPI changes, so there is no per-frame gradient work.
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
/* Must be defined before windows.h: mingw's d2d1.h spells the ID2D1RenderTarget
 * text method as plain DrawText, and the winuser.h macro renames that
 * declaration.  Without UNICODE the member ends up called DrawTextA. */
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#include <windows.h>
#include <windowsx.h>

#include <d2d1.h>
#include <d2d1_1.h>
#include <d2d1_1helper.h>
#include <d2d1helper.h>
#include <d3d11.h>
#include <dwrite.h>
#include <dxgi1_2.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "colorwheel_editor.h"

/* winuser.h turns DrawText into DrawTextW, which breaks the D2D method of the
 * same name. */
#undef DrawText

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dwrite.lib")

namespace {

const wchar_t *kClassName = L"OBSColorWheelEditor";
const int kTitleH = 40;
const int kPad = 16;

/* Semantic colour tokens.  Every pair below was measured with
 * tools/contrast_audit.py against WCAG AA: 4.5:1 for text, 3:1 for component
 * graphics.  Ratios in the comments are the measured values.
 *
 * One deliberate deviation: kColTrack is 1.60:1 against the window background
 * rather than 3:1.  A 3:1 groove would need to be about #6A707C, which reads as
 * a bright bar on a dark panel.  The slider's value is carried by the fill and
 * the knob, not by the empty groove. */
enum : unsigned {
	kColBg = 0x1B1D21,       /* window background */
	kColPanel = 0x24262B,    /* title bar */
	kColTrack = 0x3A3F49,    /* slider groove, 1.60:1 vs bg */
	kColAccent = 0x3D5FC8,   /* button surface, white text 4.79:1 */
	kColAccentHi = 0x6D8FEE, /* slider fill / data accent, 3.43:1 vs groove */
	kColText = 0xE6E8EC,     /* primary text, 12.34:1 on panel */
	kColTextDim = 0xB9BEC6,  /* channel readouts, 9.04:1 on bg */
	kColTextValue = 0xE1E4EA,/* master readout / knob, 13.25:1 on bg */
	kColHint = 0x8A909B,     /* footer hint, 5.26:1 on bg */
	kColButton = 0x3A3E46,   /* secondary button surface */
	kColButtonHi = 0x474C56, /* secondary button hover */
	kColButtonDown = 0x2E323A,
	kColOnAccent = 0xE8EBF0, /* text on accent surface */
	kColOnButton = 0xC8CCD4, /* glyph on secondary button */
	kColBorder = 0x33373E,   /* card hairline */
	kColTick = 0x8A909B,     /* slider neutral tick */
};

/* 8px spacing grid. */
const float kSpace1 = 8.0f;
const float kSpace2 = 16.0f;
const float kSpace3 = 24.0f;
const float kSpace4 = 32.0f;
const float kSpace6 = 48.0f;
const float kButtonSize = 32.0f;
const float kSliderH = 14.0f;   /* visual height */
const float kSliderHit = 32.0f; /* pointer target height */

enum WheelId { W_LIFT = 0, W_GAMMA, W_GAIN, W_OFFSET, W_COUNT };

// How much one full-deflection wheel push moves the underlying parameter.
// Tuned so a full push is strong but not destructive.
const float kWheelScale[W_COUNT] = {0.35f, 1.0f, 1.0f, 25.0f};

// Brightness slider range per wheel, always centred on the neutral value so
// the knob sits in the middle at neutral -- same as Resolve's strip slider.
// Lift is +-0.5: the internal value is 2*ui, so +-0.5 already reaches full
// white; allowing +-1 would make the control unusable over most of its travel.
const float kMasterLo[W_COUNT] = {-0.5f, -1.0f, 0.0f, 0.0f};
const float kMasterHi[W_COUNT] = {0.5f, 1.0f, 2.0f, 50.0f};

/* Disc radius as a fraction of the wheel's bounding half-size.  The hit test,
 * the drag normalisation and the knob position must all use this one value,
 * otherwise the knob cannot sit under the cursor. */
const float kDiscRatio = 0.83f;

const wchar_t *kWheelName[W_COUNT] = {L"暗部", L"中灰", L"亮部", L"偏移"};
const float kNeutral[W_COUNT] = {0.0f, 0.0f, 1.0f, 25.0f};

struct App {
	HWND hwnd = nullptr;

	ID3D11Device *d3d = nullptr;
	ID3D11DeviceContext *ctx = nullptr;
	IDXGISwapChain1 *swap = nullptr;
	ID2D1Factory1 *d2d = nullptr;
	ID2D1Device *d2d_dev = nullptr;
	ID2D1DeviceContext *dc = nullptr;
	ID2D1Bitmap1 *target = nullptr;

	IDWriteFactory *dwrite = nullptr;
	IDWriteTextFormat *f_title = nullptr;
	IDWriteTextFormat *f_label = nullptr;
	IDWriteTextFormat *f_value = nullptr;

	ID2D1Bitmap *wheel_bmp = nullptr;
	int wheel_bmp_size = 0;
	float dpi = 1.0f;

	colorwheel_editor_host host{};
	bool host_valid = false;

	// drag state
	int drag_wheel = -1;
	int drag_slider = -1;
	bool drag_ring = false;
	POINT drag_pt{};

	/* The brightness strip is only ~150 px wide, so an absolute mapping gives
	 * at best 150 distinct values (offset would step by 0.33).  Drag relative
	 * to an anchor instead, and let Shift scale the motion down for fine
	 * adjustment. */
	int slider_anchor_x = 0;
	float slider_anchor_val = 0.0f;
	bool dirty = false;
	UINT_PTR timer = 0;

	// window state, persisted next to the plugin so the editor does not keep
	// landing on top of the OBS preview
	bool pinned = true;
	int saved_x = -1, saved_y = -1, saved_w = 0, saved_h = 0;
	wchar_t cfg[MAX_PATH] = {};

	// interaction states
	int focus = -1;     // -1 none, 0..3 wheels, 4 pin, 5 close
	int hover_btn = -1; // 4 pin, 5 close, -1 none
	int press_btn = -1;

	/* Inline numeric editing.  Resolve lets you type into the R/G/B/Y readouts
	 * under each wheel; without it the only way to hit an exact value is to
	 * nudge the strip a pixel at a time. */
	int edit_wheel = -1;
	int edit_ch = -1; // 0..2 = R/G/B, 3 = the master value
	wchar_t edit_buf[24] = {};
	int edit_len = 0;
	/* The buffer starts holding the current value; the first character typed
	 * replaces it, like a selected field. */
	bool edit_replace = false;

	/* Drag-to-scrub on a readout. */
	int scrub_wheel = -1;
	int scrub_ch = -1;
	int scrub_anchor_x = 0;
	float scrub_anchor_val = 0.0f;

	// per-wheel reset button state
	int hover_reset = -1;
	int press_reset = -1;
};

App g;

/* ------------------------------------------------------------------ colour */

void hsv_to_rgb(float h, float s, float v, float *r, float *g, float *b)
{
	/* atan2 returns a negative angle for half the circle.  Normalise before
	 * indexing, otherwise the negative modulo lands on the default branch and
	 * the disc renders as six hard wedges instead of a hue gradient. */
	float hh = fmodf(h, 1.0f);
	if (hh < 0.0f)
		hh += 1.0f;
	hh *= 6.0f;
	int i = (int)hh;
	if (i < 0)
		i = 0;
	if (i > 5)
		i = 5;
	const float f = hh - (float)i;
	const float p = v * (1.0f - s), q = v * (1.0f - s * f),
		    t = v * (1.0f - s * (1.0f - f));
	switch (i) {
	case 0: *r = v; *g = t; *b = p; break;
	case 1: *r = q; *g = v; *b = p; break;
	case 2: *r = p; *g = v; *b = t; break;
	case 3: *r = p; *g = q; *b = v; break;
	case 4: *r = t; *g = p; *b = v; break;
	default: *r = v; *g = p; *b = q; break;
	}
}

/* Recover the disc position from three channel deltas: the exact inverse of
 * grade_wheel_to_rgb.  R and B carry Cr and Cb directly; G follows from the
 * luma-preserving constraint, so it is not needed. */
void wheel_deltas_to_uv(const float d[3], float *dx, float *dy)
{
	/* Strip the brightness component first: d carries the master offset as an
	 * equal shift on all three channels, and the Cb/Cr inverse would otherwise
	 * read that shift as chroma -- which put the knob off-centre by however
	 * far the brightness slider had been moved. */
	const float y = 0.2126f * d[0] + 0.7152f * d[1] + 0.0722f * d[2];
	const float cr = (d[0] - y) / GRADE_CW_CR_R;
	const float cb = (d[2] - y) / GRADE_CW_CB_B;
	*dx = cb;
	*dy = -cr;
}

/* Current per-channel delta from neutral for one wheel. */
void wheel_delta(const grade_params_t *p, int w, float out[3])
{
	for (int i = 0; i < 3; i++) {
		switch (w) {
		case W_LIFT: out[i] = p->lift[i] - kNeutral[W_LIFT]; break;
		case W_GAMMA: out[i] = p->gamma[i] - kNeutral[W_GAMMA]; break;
		case W_GAIN: out[i] = p->gain[i] - kNeutral[W_GAIN]; break;
		default: out[i] = p->offset[i] - kNeutral[W_OFFSET]; break;
		}
	}
}

/* The three channel values are "master + a zero-sum colour offset".  The
 * master is therefore recoverable as their mean, which keeps the brightness
 * slider and the wheel in sync without storing a fourth parameter. */
float wheel_master(const grade_params_t *p, int w)
{
	float d[3];
	wheel_delta(p, w, d);
	/* Luma, not the plain mean.
	 *
	 * The chroma-plane push is luminance neutral but NOT mean neutral: a push
	 * toward blue gives (0, -0.10, 0.35), whose mean is +0.083.  With a
	 * mean-based master every drag event added that mean back into the
	 * brightness, so values climbed while the mouse stood still and the knob
	 * could never sit under the cursor.  Luma is invariant under a
	 * luminance-neutral push, which is exactly what we need here. */
	const float m = 0.2126f * d[0] + 0.7152f * d[1] + 0.0722f * d[2];
	return kNeutral[w] + m;
}

void wheel_set_master(grade_params_t *p, int w, float new_master)
{
	const float cur = wheel_master(p, w);
	const float delta = new_master - cur;
	for (int i = 0; i < 3; i++) {
		switch (w) {
		case W_LIFT: p->lift[i] += delta; break;
		case W_GAMMA: p->gamma[i] += delta; break;
		case W_GAIN: p->gain[i] += delta; break;
		default: p->offset[i] += delta; break;
		}
	}
}

/* Colour-only push: the deltas are relative to the current master, so moving
 * the wheel never changes brightness -- exactly the behaviour the mean-based
 * master above relies on. */
void wheel_write(grade_params_t *p, int w, const float d[3])
{
	const float m = wheel_master(p, w);
	for (int i = 0; i < 3; i++) {
		const float v = m + d[i];
		switch (w) {
		case W_LIFT: p->lift[i] = v; break;
		case W_GAMMA: p->gamma[i] = v; break;
		case W_GAIN: p->gain[i] = v; break;
		default: p->offset[i] = v; break;
		}
	}
}

/* ------------------------------------------------------- wheel bitmap ---- */

/* Hue of an RGB-offset direction, so the disc shows the colour that actually
 * gets added when you push in that direction.  With the chroma-plane mapping
 * this is what makes red sit at the top and the wedges uneven, like Resolve. */
float direction_hue(float dx, float dy)
{
	float d[3];
	grade_wheel_direction(dx, dy, d);
	const float mx = fmaxf(d[0], fmaxf(d[1], d[2]));
	const float mn = fminf(d[0], fminf(d[1], d[2]));
	const float span = mx - mn;
	if (span < 1e-6f)
		return 0.0f;
	float h;
	if (mx == d[0])
		h = fmodf((d[1] - d[2]) / span, 6.0f);
	else if (mx == d[1])
		h = (d[2] - d[0]) / span + 2.0f;
	else
		h = (d[0] - d[1]) / span + 4.0f;
	h /= 6.0f;
	if (h < 0.0f)
		h += 1.0f;
	return h;
}

// One CPU pass: hue ring + saturation disc.  Cached, rebuilt on size change.
ID2D1Bitmap *build_wheel_bitmap(int size)
{
	if (!g.dc || size <= 0)
		return nullptr;

	const int n = size;
	const float c = n * 0.5f;
	const float r_out = c - 1.0f;
	const float ringW = r_out * 0.17f;
	const float r_ring_in = r_out - ringW;
	const float gap = ringW * 0.45f;
	const float r_disc = r_ring_in - gap;

	/* calloc, not malloc: pixels outside the wheel are skipped with `continue`,
	 * so any uninitialised byte would show up as garbage in the square corners. */
	unsigned char *px = (unsigned char *)calloc((size_t)n * n, 4);
	if (!px)
		return nullptr;

	for (int y = 0; y < n; y++) {
		for (int x = 0; x < n; x++) {
			const float dx = (float)x - c + 0.5f;
			const float dy = (float)y - c + 0.5f;
			const float r = sqrtf(dx * dx + dy * dy);
			unsigned char *o = px + ((size_t)y * n + x) * 4;

			float rr, gg, bb;
			float a = 0.0f;

			if (r <= r_disc) {
				// inner disc: hue by chroma direction, saturation by radius
				const float h = direction_hue(dx, dy);
				float s = r / (r_disc > 0.0f ? r_disc : 1.0f);
				hsv_to_rgb(h, s < 1.0f ? s : 1.0f, 0.96f, &rr,
					   &gg, &bb);
				const float edge = (r_disc - r) / 1.5f;
				a = edge < 1.0f ? edge : 1.0f;
			} else if (r < r_ring_in) {
				// groove: a structural gap reads as "two parts", not one decal
				rr = gg = bb = 0.0f;
				a = 0.55f;
			} else if (r <= r_out) {
				const float h = direction_hue(dx, dy);
				hsv_to_rgb(h, 0.82f, 0.95f, &rr, &gg, &bb);
				const float a0 = (r - r_ring_in) / 1.5f;
				const float a1 = (r_out - r) / 1.5f;
				a = fminf(1.0f, fminf(a0, a1));
			} else {
				continue;
			}

			if (a < 0.0f)
				a = 0.0f;
			if (a > 1.0f)
				a = 1.0f;

			o[0] = (unsigned char)(bb * 255.0f * a + 0.5f);
			o[1] = (unsigned char)(gg * 255.0f * a + 0.5f);
			o[2] = (unsigned char)(rr * 255.0f * a + 0.5f);
			o[3] = (unsigned char)(a * 255.0f + 0.5f);
		}
	}

	// The device context stays at its default 96 dpi (our layout is in pixels),
	// so the bitmap must declare 96 dpi too.  Mismatched dpi here makes
	// DrawBitmap resample and the wheel comes out soft.
	D2D1_BITMAP_PROPERTIES props = D2D1::BitmapProperties(
		D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
				  D2D1_ALPHA_MODE_PREMULTIPLIED),
		96.0f, 96.0f);
	ID2D1Bitmap *bmp = nullptr;
	g.dc->CreateBitmap(D2D1::SizeU((UINT32)n, (UINT32)n), px,
			   (UINT32)n * 4, &props, &bmp);
	free(px);
	return bmp;
}

void ensure_wheel_bitmap(int size)
{
	if (g.wheel_bmp && g.wheel_bmp_size == size)
		return;
	if (g.wheel_bmp) {
		g.wheel_bmp->Release();
		g.wheel_bmp = nullptr;
	}
	g.wheel_bmp = build_wheel_bitmap(size);
	g.wheel_bmp_size = size;
}

/* ------------------------------------------------------------- d3d plumbing */

IDXGIDevice *dxgi_dev_get(ID3D11Device *dev)
{
	IDXGIDevice *p = nullptr;
	dev->QueryInterface(__uuidof(IDXGIDevice), (void **)&p);
	return p;
}

bool create_device()
{
	UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
	/* Asking for 11_1 makes D3D11CreateDevice fail with E_INVALIDARG on any
	 * machine without the D3D11.1 runtime -- including plenty of Windows 10
	 * boxes.  Always retry without it. */
	D3D_FEATURE_LEVEL levels_11_1[] = {D3D_FEATURE_LEVEL_11_1,
					   D3D_FEATURE_LEVEL_11_0,
					   D3D_FEATURE_LEVEL_10_1};
	D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0,
				      D3D_FEATURE_LEVEL_10_1};
	D3D_FEATURE_LEVEL got;
	HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
				       flags, levels_11_1, 3, D3D11_SDK_VERSION,
				       &g.d3d, &got, &g.ctx);
	if (hr == E_INVALIDARG) {
		hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
				       flags, levels, 2, D3D11_SDK_VERSION,
				       &g.d3d, &got, &g.ctx);
	}
	if (FAILED(hr))
		return false;

	IDXGIDevice *dxgi_dev = nullptr;
	if (FAILED(g.d3d->QueryInterface(__uuidof(IDXGIDevice),
					 (void **)&dxgi_dev)))
		return false;
	IDXGIAdapter *adapter = nullptr;
	dxgi_dev->GetAdapter(&adapter);
	dxgi_dev->Release();
	IDXGIFactory2 *factory = nullptr;
	adapter->GetParent(__uuidof(IDXGIFactory2), (void **)&factory);
	adapter->Release();
	if (!factory)
		return false;

	RECT rc;
	GetClientRect(g.hwnd, &rc);

	DXGI_SWAP_CHAIN_DESC1 sd = {};
	sd.Width = (UINT)(rc.right - rc.left);
	sd.Height = (UINT)(rc.bottom - rc.top);
	sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
	sd.SampleDesc.Count = 1;
	sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	sd.BufferCount = 2;
	// FLIP_SEQUENTIAL keeps the presented contents in the other buffer.
	// FLIP_DISCARD leaves it undefined, so a Present that is not preceded by a
	// full repaint can show a stale/uninitialised buffer -- which read as
	// stray fragments around the wheels.
	sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
	sd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
	sd.Scaling = DXGI_SCALING_STRETCH;

	hr = factory->CreateSwapChainForHwnd(g.d3d, g.hwnd, &sd, nullptr,
					     nullptr, &g.swap);
	factory->Release();
	if (FAILED(hr))
		return false;

	// CreateDevice lives on ID2D1Factory1, not on ID2D1Factory.
	D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1),
			  nullptr, (void **)&g.d2d);
	if (!g.d2d)
		return false;
	IDXGIDevice *d2d_dxgi = dxgi_dev_get(g.d3d);
	if (!d2d_dxgi)
		return false;
	hr = g.d2d->CreateDevice(d2d_dxgi, &g.d2d_dev);
	d2d_dxgi->Release();
	if (FAILED(hr))
		return false;
	if (FAILED(g.d2d_dev->CreateDeviceContext(
		    D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &g.dc)))
		return false;

	DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,
			    __uuidof(IDWriteFactory),
			    (IUnknown **)&g.dwrite);
	if (!g.dwrite)
		return false;

	g.dwrite->CreateTextFormat(L"Microsoft YaHei UI", nullptr,
				   DWRITE_FONT_WEIGHT_SEMI_BOLD,
				   DWRITE_FONT_STYLE_NORMAL,
				   DWRITE_FONT_STRETCH_NORMAL, 15.0f * g.dpi,
				   L"zh-cn", &g.f_title);
	g.dwrite->CreateTextFormat(L"Microsoft YaHei UI", nullptr,
				   DWRITE_FONT_WEIGHT_NORMAL,
				   DWRITE_FONT_STYLE_NORMAL,
				   DWRITE_FONT_STRETCH_NORMAL, 13.0f * g.dpi,
				   L"zh-cn", &g.f_label);
	g.dwrite->CreateTextFormat(L"Consolas", nullptr,
				   DWRITE_FONT_WEIGHT_NORMAL,
				   DWRITE_FONT_STYLE_NORMAL,
				   DWRITE_FONT_STRETCH_NORMAL, 12.0f * g.dpi,
				   L"en-us", &g.f_value);
	return true;
}

bool create_target()
{
	if (!g.swap || !g.dc)
		return false;
	IDXGISurface *surf = nullptr;
	if (FAILED(g.swap->GetBuffer(0, __uuidof(IDXGISurface),
				     (void **)&surf)))
		return false;

	D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(
		D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
		D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
				  D2D1_ALPHA_MODE_IGNORE),
		96.0f * g.dpi, 96.0f * g.dpi);

	HRESULT hr = g.dc->CreateBitmapFromDxgiSurface(surf, &props, &g.target);
	surf->Release();
	if (FAILED(hr))
		return false;
	g.dc->SetTarget(g.target);
	return true;
}

void discard_target()
{
	if (g.dc)
		g.dc->SetTarget(nullptr);
	if (g.target) {
		g.target->Release();
		g.target = nullptr;
	}
}

void resize()
{
	if (!g.swap)
		return;
	discard_target();
	RECT rc;
	GetClientRect(g.hwnd, &rc);
	UINT w = (UINT)(rc.right - rc.left), h = (UINT)(rc.bottom - rc.top);
	if (w == 0 || h == 0)
		return;
	g.swap->ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN, 0);
	create_target();
}

void release_all()
{
	if (g.wheel_bmp) { g.wheel_bmp->Release(); g.wheel_bmp = nullptr; }
	if (g.f_title) { g.f_title->Release(); g.f_title = nullptr; }
	if (g.f_label) { g.f_label->Release(); g.f_label = nullptr; }
	if (g.f_value) { g.f_value->Release(); g.f_value = nullptr; }
	if (g.dwrite) { g.dwrite->Release(); g.dwrite = nullptr; }
	discard_target();
	if (g.dc) { g.dc->Release(); g.dc = nullptr; }
	if (g.d2d_dev) { g.d2d_dev->Release(); g.d2d_dev = nullptr; }
	if (g.d2d) { g.d2d->Release(); g.d2d = nullptr; }
	if (g.swap) { g.swap->Release(); g.swap = nullptr; }
	if (g.ctx) { g.ctx->Release(); g.ctx = nullptr; }
	if (g.d3d) { g.d3d->Release(); g.d3d = nullptr; }
	if (g.timer) { KillTimer(g.hwnd, g.timer); g.timer = 0; }
}

/* --------------------------------------------------------------- drawing */

D2D1_COLOR_F col(UINT32 rgb, float a = 1.0f)
{
	return D2D1::ColorF(((rgb >> 16) & 0xFF) / 255.0f,
			    ((rgb >> 8) & 0xFF) / 255.0f,
			    (rgb & 0xFF) / 255.0f, a);
}

D2D1_RECT_F inset(float x, float y, float w, float h)
{
	return D2D1::RectF(x, y, x + w, y + h);
}

void draw_text(const wchar_t *s, IDWriteTextFormat *fmt, D2D1_RECT_F r,
	       D2D1_COLOR_F color)
{
	if (!s || !fmt)
		return;
	ID2D1SolidColorBrush *br = nullptr;
	g.dc->CreateSolidColorBrush(color, &br);
	if (!br)
		return;
	g.dc->DrawTextW(s, (UINT32)wcslen(s), fmt, r, br,
			D2D1_DRAW_TEXT_OPTIONS_CLIP);
	br->Release();
}

/* Focus ring: 2px accent outline offset outside the control, so keyboard focus
 * is visible on every interactive element. */
void draw_focus_ring(D2D1_RECT_F r, float radius)
{
	ID2D1SolidColorBrush *br = nullptr;
	g.dc->CreateSolidColorBrush(col(kColAccentHi), &br);
	if (!br)
		return;
	g.dc->DrawRoundedRectangle(D2D1::RoundedRect(r, radius, radius), br,
				   2.0f);
	br->Release();
}

/* ---- Layout ---------------------------------------------------------------
 * Design direction: Adobe Spectrum-like professional creative tool (the skill's
 * style domain names it directly for "creative tools, media workflows ...
 * tokenized colour, precise hierarchy, dense but legible controls, strong
 * focus states").  Consequences applied here:
 *   - four wheels read as four cards on a canvas, so each tonal range is a
 *     visual unit instead of four things floating on one background;
 *   - everything sits on the 8px grid;
 *   - type scale 15 / 13 / 13 with monospaced numerals so live values do not
 *     jitter horizontally while dragging.
 */
const float kCardGap = 8.0f;
const float kCardPad = 16.0f;
const float kLabelH = 22.0f;

float wheel_size(float cw)
{
	const float slot = cw / 4.0f;
	const float avail = slot - kCardGap * 2.0f - kCardPad * 2.0f;
	float s = avail < 230.0f * g.dpi ? avail : 230.0f * g.dpi;
	if (s < 60.0f)
		s = 60.0f;
	return s;
}

D2D1_RECT_F card_rect(int w, float cw)
{
	const float slot = cw / 4.0f;
	const float size = wheel_size(cw);
	const float h = kCardPad * 2.0f + kLabelH + kSpace1 + size + kSpace1 +
			18.0f + kSpace1 + kSliderH;
	return inset(kCardGap + slot * w, (float)kTitleH + kSpace2,
		     slot - kCardGap * 2.0f, h);
}

// Geometry of wheel w, in client coordinates.
D2D1_RECT_F wheel_rect(int w, float cw)
{
	D2D1_RECT_F c = card_rect(w, cw);
	const float size = wheel_size(cw);
	return inset(c.left + (c.right - c.left - size) * 0.5f,
		     c.top + kCardPad + kLabelH + kSpace1, size, size);
}

D2D1_RECT_F label_rect(int w, float cw)
{
	D2D1_RECT_F c = card_rect(w, cw);
	return inset(c.left + kCardPad, c.top + kCardPad, c.right - c.left - kCardPad * 2.0f,
		     kLabelH);
}

// Horizontal brightness strip under each wheel, centred on the neutral value.
D2D1_RECT_F slider_rect(int w, float cw)
{
	D2D1_RECT_F r = wheel_rect(w, cw);
	return inset(r.left, r.bottom + kSpace1 + 18.0f + kSpace1,
		     r.right - r.left, kSliderH);
}

// One of the four numeric readouts (R, G, B, master).
D2D1_RECT_F readout_rect(int w, int ch, float cw)
{
	D2D1_RECT_F r = wheel_rect(w, cw);
	const float bw = (r.right - r.left) / 4.0f;
	return inset(r.left + bw * ch, r.bottom + kSpace1, bw, 18.0f);
}

/* Per-wheel reset button, top-right inside the card.  26x26 clears the 24px
 * pointer minimum; it only ever resets its own tonal range. */
const float kResetSize = 26.0f;

D2D1_RECT_F reset_rect(int w, float cw)
{
	D2D1_RECT_F c = card_rect(w, cw);
	return inset(c.right - 12.0f - kResetSize, c.top + 12.0f, kResetSize,
		     kResetSize);
}

/* Defined further down, next to the message handlers that drive them. */
float readout_value(int w, int ch);
void readout_range(int w, int ch, float *lo, float *hi);
void readout_set(int w, int ch, float v);

/* Knob position for wheel w, in disc-normalised screen coordinates
 * (x right, y down).  Returns the radius clamped to 1.
 *
 * The sign here is subtle and was wrong once: wheel_deltas_to_uv already
 * converts back into SCREEN space (it negates internally to undo the
 * atan2(-dy, dx) convention), so no further flip is allowed.  Negating dy here
 * drew the knob at the opposite side of the disc from the drag that created it
 * -- drag to green, knob sits on magenta.
 */
float knob_uv(int w, float *ux, float *uy)
{
	float d[3];
	wheel_delta(g.host.params, w, d);
	float dx, dy;
	wheel_deltas_to_uv(d, &dx, &dy);
	const float scale = kWheelScale[w];
	float x = dx / scale, y = dy / scale;
	float rad = sqrtf(x * x + y * y);
	if (rad > 1.0f) {
		x /= rad;
		y /= rad;
		rad = 1.0f;
	}
	*ux = x;
	*uy = y;
	return rad;
}

void draw_wheel(int w, float cw)
{
	D2D1_RECT_F card = card_rect(w, cw);
	D2D1_RECT_F r = wheel_rect(w, cw);
	const float size = r.right - r.left;
	const float cx = (r.left + r.right) * 0.5f;
	const float cy = (r.top + r.bottom) * 0.5f;

	/* Card surface: gives each tonal range a visual boundary so the four
	 * wheels read as four controls rather than one busy canvas. */
	ID2D1SolidColorBrush *cb = nullptr;
	g.dc->CreateSolidColorBrush(col(kColPanel), &cb);
	if (cb) {
		g.dc->FillRoundedRectangle(D2D1::RoundedRect(card, 10.0f, 10.0f),
					   cb);
		cb->Release();
	}
	ID2D1SolidColorBrush *cbd = nullptr;
	g.dc->CreateSolidColorBrush(col(kColBorder), &cbd);
	if (cbd) {
		g.dc->DrawRoundedRectangle(
			D2D1::RoundedRect(card, 10.0f, 10.0f), cbd, 1.0f);
		cbd->Release();
	}

	ensure_wheel_bitmap((int)(size + 0.5f));
	if (g.wheel_bmp) {
		g.dc->DrawBitmap(g.wheel_bmp,
				 D2D1::RectF(r.left, r.top, r.right, r.bottom),
				 1.0f,
				 D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR);
	}

	// label inside the card
	draw_text(kWheelName[w], g.f_title, label_rect(w, cw), col(kColText));

	/* Per-wheel reset.  A symbol, not text, so it reads the same in any
	 * language; it resets only this tonal range. */
	{
		D2D1_RECT_F rb = reset_rect(w, cw);
		unsigned bg = kColButton;
		if (g.press_reset == w)
			bg = kColButtonDown;
		else if (g.hover_reset == w)
			bg = kColButtonHi;
		ID2D1SolidColorBrush *bb = nullptr;
		g.dc->CreateSolidColorBrush(col(bg), &bb);
		if (bb) {
			g.dc->FillRoundedRectangle(
				D2D1::RoundedRect(rb, 6.0f, 6.0f), bb);
			bb->Release();
		}
		draw_text(L"\u21BA", g.f_title,
			  inset(rb.left + 7.0f, rb.top + 3.0f, kResetSize, 22.0f),
			  col(g.hover_reset == w ? kColText : kColOnButton));
	}

	if (!g.host_valid || !g.host.params)
		return;

	float ux, uy;
	const float rad = knob_uv(w, &ux, &uy);
	const float knobR = size * 0.5f * kDiscRatio - 5.0f;
	const float kx = cx + ux * rad * knobR;
	const float ky = cy + uy * rad * knobR;

	// centre crosshair
	ID2D1SolidColorBrush *br = nullptr;
	g.dc->CreateSolidColorBrush(col(0xFFFFFF, 0.55f), &br);
	if (br) {
		g.dc->DrawLine(D2D1::Point2F(cx - 6, cy),
			       D2D1::Point2F(cx + 6, cy), br, 1.0f);
		g.dc->DrawLine(D2D1::Point2F(cx, cy - 6),
			       D2D1::Point2F(cx, cy + 6), br, 1.0f);
		br->Release();
	}

	// knob
	g.dc->CreateSolidColorBrush(col(0xFFFFFF, 0.95f), &br);
	if (br) {
		g.dc->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(kx, ky), 6.0f, 6.0f),
				  br, 2.0f);
		br->Release();
	}

	/* Four readouts: R, G, B and the brightness (master), like Resolve.  Each
	 * is editable: double-click to type, drag to scrub. */
	{
		wchar_t buf[32];
		for (int i = 0; i < 4; i++) {
			D2D1_RECT_F rr = readout_rect(w, i, cw);

			if (g.edit_wheel == w && g.edit_ch == i) {
				g.edit_buf[g.edit_len] = 0;
				ID2D1SolidColorBrush *eb = nullptr;
				g.dc->CreateSolidColorBrush(col(kColAccent), &eb);
				if (eb) {
					g.dc->FillRoundedRectangle(
						D2D1::RoundedRect(
							inset(rr.left - 2.0f,
							      rr.top - 1.0f,
							      (rr.right - rr.left) + 4.0f,
							      20.0f),
							3.0f, 3.0f),
						eb);
					eb->Release();
				}
				draw_text(g.edit_buf, g.f_value, rr,
					  col(kColOnAccent));
				continue;
			}

			swprintf(buf, 32, L"%.2f", readout_value(w, i));
			draw_text(buf, g.f_value, rr,
				  col(i == 3 ? kColTextValue : kColTextDim));
		}
	}

	// Brightness strip.  Dragging it moves all three channel values together,
	// i.e. it changes the level of this tonal range without touching hue.
	D2D1_RECT_F sr = slider_rect(w, cw);
	ID2D1SolidColorBrush *tb = nullptr;
	g.dc->CreateSolidColorBrush(col(kColTrack), &tb);
	if (tb) {
		g.dc->FillRoundedRectangle(D2D1::RoundedRect(sr, 4.0f, 4.0f),
					   tb);
		tb->Release();
	}
	const float lo = kMasterLo[w], hi = kMasterHi[w];
	float t = (wheel_master(g.host.params, w) - lo) / (hi - lo);
	if (t < 0.0f)
		t = 0.0f;
	if (t > 1.0f)
		t = 1.0f;
	const float mx = (sr.left + sr.right) * 0.5f;
	const float skx = sr.left + t * (sr.right - sr.left);

	ID2D1SolidColorBrush *fb = nullptr;
	g.dc->CreateSolidColorBrush(col(kColAccentHi), &fb);
	if (fb) {
		const float a = mx < skx ? mx : skx;
		const float b = mx < skx ? skx : mx;
		if (b - a > 1.0f)
			g.dc->FillRoundedRectangle(
				D2D1::RoundedRect(
					inset(a, sr.top, b - a,
					      sr.bottom - sr.top),
					4.0f, 4.0f),
				fb);
		fb->Release();
	}
	/* Neutral tick, drawn on top of the fill so the zero position stays
	 * readable no matter where the value sits. */
	{
		const float nt = (kNeutral[w] - lo) / (hi - lo);
		const float ntx = sr.left + nt * (sr.right - sr.left);
		ID2D1SolidColorBrush *tk = nullptr;
		g.dc->CreateSolidColorBrush(col(kColTextValue, 0.75f), &tk);
		if (tk) {
			g.dc->FillRectangle(
				inset(ntx - 1.0f, sr.top + 3.0f, 2.0f,
				      (sr.bottom - sr.top) - 6.0f),
				tk);
			tk->Release();
		}
	}

	ID2D1SolidColorBrush *kb = nullptr;
	g.dc->CreateSolidColorBrush(col(kColTextValue), &kb);
	if (kb) {
		g.dc->FillEllipse(
			D2D1::Ellipse(
				D2D1::Point2F(skx,
					      (sr.top + sr.bottom) * 0.5f),
				6.0f, 6.0f),
			kb);
		kb->Release();
	}

	/* Focus ring: keyboard focus must be visible on every control. */
	if (g.focus == w)
		draw_focus_ring(inset(sr.left - 3, sr.top - 3, sr.right - sr.left + 6,
				      sr.bottom - sr.top + 6),
				6.0f);
}

/* Keyboard hint chip: makes the keyboard path discoverable instead of hiding it
 * in a paragraph.  Width is derived from the label so no text measuring is
 * needed. */
float draw_keycap(float x, float y, const wchar_t *s, float w)
{
	ID2D1SolidColorBrush *br = nullptr;
	g.dc->CreateSolidColorBrush(col(kColButton), &br);
	if (br) {
		g.dc->FillRoundedRectangle(
			D2D1::RoundedRect(inset(x, y, w, 18.0f), 4.0f, 4.0f),
			br);
		br->Release();
	}
	draw_text(s, g.f_value, inset(x, y + 1.0f, w, 16.0f), col(kColOnButton));
	return w;
}

HRESULT draw()
{
	if (!g.dc || !g.target)
		return S_OK;

	RECT rc;
	GetClientRect(g.hwnd, &rc);
	const float cw = (float)(rc.right - rc.left);
	const float ch = (float)(rc.bottom - rc.top);
	if (cw <= 0 || ch <= 0)
		return S_OK;

	g.dc->BeginDraw();
	g.dc->Clear(col(kColBg));

	// title bar
	ID2D1SolidColorBrush *br = nullptr;
	g.dc->CreateSolidColorBrush(col(kColPanel), &br);
	if (br) {
		g.dc->FillRectangle(inset(0, 0, cw, (float)kTitleH), br);
		br->Release();
	}
	/* hairline under the title bar: separates chrome from content without a
	 * boxed border */
	ID2D1SolidColorBrush *hd = nullptr;
	g.dc->CreateSolidColorBrush(col(kColBorder), &hd);
	if (hd) {
		g.dc->FillRectangle(
			inset(0, (float)kTitleH - 1.0f, cw, 1.0f), hd);
		hd->Release();
	}

	// mingw's narrow/wide printf treats %s in a wide format as a *narrow*
	// string, so the title truncated to its first character.  %ls is the
	// portable spelling for a wide argument.
	const char *sn = (g.host_valid && g.host.get_source_name)
				 ? g.host.get_source_name(g.host.filter)
				 : nullptr;
	wchar_t title[256];
	if (sn) {
		if (MultiByteToWideChar(CP_UTF8, 0, sn, -1, title, 128) <= 0)
			wcscpy(title, L"未连接");
	} else {
		wcscpy(title, L"未连接");
	}
	wchar_t head[320];
	swprintf(head, 320, L"一级校色轮  ·  %ls", title);
	draw_text(head, g.f_title, inset(16, 10, cw - 120, 24), col(kColText));

	/* Title-bar buttons: pin (always-on-top) and close.  32x32 so the pointer
	 * target clears the 24px WCAG minimum with room to spare.
	 *
	 * Hover darkens rather than lightens: the accent surface holds white text
	 * at 4.79:1, and any lightening drops it below the 4.5:1 requirement. */
	{
		const float by = 4.0f;
		const float pinX = cw - 84.0f;
		const float closeX = cw - 44.0f;

		unsigned pinBg = g.pinned ? kColAccent : kColButton;
		if (g.press_btn == 4)
			pinBg = g.pinned ? 0x2E4699 : kColButtonDown;
		else if (g.hover_btn == 4)
			pinBg = g.pinned ? 0x3550AE : kColButtonHi;

		unsigned closeBg = kColButton;
		if (g.press_btn == 5)
			closeBg = kColButtonDown;
		else if (g.hover_btn == 5)
			closeBg = kColButtonHi;

		ID2D1SolidColorBrush *pb = nullptr;
		g.dc->CreateSolidColorBrush(col(pinBg), &pb);
		if (pb) {
			g.dc->FillRoundedRectangle(
				D2D1::RoundedRect(
					inset(pinX, by, kButtonSize, kButtonSize),
					6.0f, 6.0f),
				pb);
			pb->Release();
		}
		draw_text(g.pinned ? L"顶" : L"浮", g.f_label,
			  inset(pinX, by + 5.0f, kButtonSize, 22.0f),
			  col(kColOnAccent));

		ID2D1SolidColorBrush *b2 = nullptr;
		g.dc->CreateSolidColorBrush(col(closeBg), &b2);
		if (b2) {
			g.dc->FillRoundedRectangle(
				D2D1::RoundedRect(
					inset(closeX, by, kButtonSize,
					      kButtonSize),
					6.0f, 6.0f),
				b2);
			b2->Release();
		}
		draw_text(L"×", g.f_title,
			  inset(closeX + 8.0f, by + 4.0f, 24.0f, 24.0f),
			  col(kColOnButton));

		if (g.focus == 4)
			draw_focus_ring(inset(pinX - 3, by - 3, kButtonSize + 6,
					      kButtonSize + 6),
					8.0f);
		if (g.focus == 5)
			draw_focus_ring(inset(closeX - 3, by - 3, kButtonSize + 6,
					      kButtonSize + 6),
					8.0f);
	}

	for (int w = 0; w < W_COUNT; w++)
		draw_wheel(w, cw);

	// footer: key caps + hint
	{
		const float y = ch - 30.0f;
		float x = kSpace2;
		x += draw_keycap(x, y, L"Tab", 44.0f) + 6.0f;
		draw_text(L"切换", g.f_label, inset(x, y + 1.0f, 60.0f, 18.0f),
			  col(kColHint));
		x += 34.0f;
		x += draw_keycap(x, y, L"←→", 48.0f) + 6.0f;
		draw_text(L"微调亮度", g.f_label,
			  inset(x, y + 1.0f, 90.0f, 18.0f), col(kColHint));
		x += 68.0f;
		draw_text(L"· 拖动圆盘推色 · 双击复位 · 松手写入设置", g.f_label,
			  inset(x, y + 1.0f, cw - x - kSpace2, 18.0f),
			  col(kColHint));
	}

	HRESULT hr = g.dc->EndDraw();
	return hr;
}

void present()
{
	if (g.swap)
		g.swap->Present(1, 0);
}

void invalidate()
{
	InvalidateRect(g.hwnd, nullptr, FALSE);
}

/* ------------------------------------------------- window state persistence */

/* Win32 profile API instead of fscanf/fprintf: the CRT scan machinery alone
 * added ~65 KB to a 133 KB DLL. */
void load_state()
{
	if (!g.cfg[0])
		return;
	g.saved_x = GetPrivateProfileIntW(L"window", L"x", -1, g.cfg);
	g.saved_y = GetPrivateProfileIntW(L"window", L"y", -1, g.cfg);
	g.saved_w = GetPrivateProfileIntW(L"window", L"w", 0, g.cfg);
	g.saved_h = GetPrivateProfileIntW(L"window", L"h", 0, g.cfg);
	g.pinned = GetPrivateProfileIntW(L"window", L"pinned", 1, g.cfg) != 0;
}

void put_int(const wchar_t *key, long v)
{
	wchar_t buf[32];
	swprintf(buf, 32, L"%ld", v);
	WritePrivateProfileStringW(L"window", key, buf, g.cfg);
}

void save_state()
{
	if (!g.cfg[0] || !g.hwnd)
		return;
	RECT wr;
	GetWindowRect(g.hwnd, &wr);
	put_int(L"x", (long)wr.left);
	put_int(L"y", (long)wr.top);
	put_int(L"w", (long)(wr.right - wr.left));
	put_int(L"h", (long)(wr.bottom - wr.top));
	put_int(L"pinned", g.pinned ? 1 : 0);
}

/* Open flush to the bottom of the work area and as wide as is comfortable:
 * OBS keeps its preview in the upper half, so the editor belongs below it.
 * Only the SIZE is remembered; the position is recomputed every time so the
 * window always comes back to the same place. */
void initial_rect(RECT *out)
{
	RECT work;
	SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
	const int workW = work.right - work.left;
	const int workH = work.bottom - work.top;

	/* Ignore a remembered size that predates the current layout: earlier builds
	 * saved 900x380 or 1280x420, neither of which fits the card layout. */
	int w = g.saved_w >= 1000 ? g.saved_w : 1280;
	int h = g.saved_h >= 440 ? g.saved_h : 460;
	if (w > workW - 40)
		w = workW - 40;
	if (h > workH - 80)
		h = workH - 80;

	out->left = work.left + (workW - w) / 2;
	out->top = work.bottom - h;
	out->right = out->left + w;
	out->bottom = work.bottom;
}

/* --------------------------------------------------------------- input */

bool hit_ring(D2D1_RECT_F r, POINT pt)
{
	const float cx = (r.left + r.right) * 0.5f;
	const float cy = (r.top + r.bottom) * 0.5f;
	const float rad = (r.right - r.left) * 0.5f;
	const float dx = pt.x - cx, dy = pt.y - cy;
	const float d = sqrtf(dx * dx + dy * dy);
	return d > rad * 0.83f && d <= rad + 2.0f;
}

bool inside(D2D1_RECT_F r, POINT pt)
{
	return pt.x >= r.left && pt.x <= r.right && pt.y >= r.top &&
	       pt.y <= r.bottom;
}

int hit_wheel(POINT pt)
{
	RECT rc;
	GetClientRect(g.hwnd, &rc);
	for (int w = 0; w < W_COUNT; w++) {
		D2D1_RECT_F r = wheel_rect(w, (float)rc.right);
		const float cx = (r.left + r.right) * 0.5f;
		const float cy = (r.top + r.bottom) * 0.5f;
		const float rad = (r.right - r.left) * 0.5f;
		const float dx = (float)pt.x - cx, dy = (float)pt.y - cy;
		/* Only the disc counts.  Using the bounding square meant a click in a
		 * corner (outside the circle) still started a drag whose radius was
		 * normalised to the rim, so the knob landed somewhere other than under
		 * the cursor and the colour did not match the spot that was clicked. */
		if (dx * dx + dy * dy <= (rad * kDiscRatio) * (rad * kDiscRatio))
			return w;
	}
	return -1;
}

int hit_slider(POINT pt)
{
	RECT rc;
	GetClientRect(g.hwnd, &rc);
	for (int w = 0; w < W_COUNT; w++) {
		D2D1_RECT_F sr = slider_rect(w, (float)rc.right);
		/* The visible groove is kSliderH tall; the pointer target is
		 * kSliderHit tall so it clears the 24px WCAG minimum. */
		const float pad = (kSliderHit - kSliderH) * 0.5f;
		if (pt.x >= sr.left - 4.0f && pt.x <= sr.right + 4.0f &&
		    pt.y >= sr.top - pad && pt.y <= sr.bottom + pad)
			return w;
	}
	return -1;
}

/* ---- numeric readouts: type into them, or drag them to scrub ------------- */

float readout_value(int w, int ch)
{
	const grade_params_t *p = g.host.params;
	if (ch == 3)
		return wheel_master(p, w);
	switch (w) {
	case W_LIFT:
		return p->lift[ch];
	case W_GAMMA:
		return p->gamma[ch];
	case W_GAIN:
		return p->gain[ch];
	default:
		return p->offset[ch];
	}
}

void readout_range(int w, int ch, float *lo, float *hi)
{
	if (ch == 3) {
		*lo = kMasterLo[w];
		*hi = kMasterHi[w];
		return;
	}
	switch (w) {
	case W_LIFT:
		*lo = -1.0f;
		*hi = 1.0f;
		break;
	case W_GAMMA:
		*lo = -1.0f;
		*hi = 1.0f;
		break;
	case W_GAIN:
		*lo = 0.0f;
		*hi = 4.0f;
		break;
	default:
		*lo = 0.0f;
		*hi = 100.0f;
		break;
	}
}

void readout_set(int w, int ch, float v)
{
	grade_params_t *p = g.host.params;
	if (ch == 3) {
		wheel_set_master(p, w, v);
		return;
	}
	switch (w) {
	case W_LIFT:
		p->lift[ch] = v;
		break;
	case W_GAMMA:
		p->gamma[ch] = v;
		break;
	case W_GAIN:
		p->gain[ch] = v;
		break;
	default:
		p->offset[ch] = v;
		break;
	}
}

int hit_readout(POINT pt, int *out_ch)
{
	RECT rc;
	GetClientRect(g.hwnd, &rc);
	for (int w = 0; w < W_COUNT; w++)
		for (int ch = 0; ch < 4; ch++) {
			D2D1_RECT_F rr = readout_rect(w, ch, (float)rc.right);
			if (pt.x >= rr.left && pt.x <= rr.right &&
			    pt.y >= rr.top - 2 && pt.y <= rr.bottom + 2) {
				if (out_ch)
					*out_ch = ch;
				return w;
			}
		}
	return -1;
}

int hit_reset(POINT pt)
{
	RECT rc;
	GetClientRect(g.hwnd, &rc);
	for (int w = 0; w < W_COUNT; w++) {
		D2D1_RECT_F r = reset_rect(w, (float)rc.right);
		if (pt.x >= r.left - 2 && pt.x <= r.right + 2 &&
		    pt.y >= r.top - 2 && pt.y <= r.bottom + 2)
			return w;
	}
	return -1;
}

void begin_edit(int w, int ch)
{
	if (!g.host_valid || !g.host.params)
		return;
	g.edit_wheel = w;
	g.edit_ch = ch;
	swprintf(g.edit_buf, 24, L"%.2f", readout_value(w, ch));
	g.edit_len = (int)wcslen(g.edit_buf);
	g.edit_replace = true;
	invalidate();
}

void cancel_edit()
{
	g.edit_wheel = -1;
	g.edit_ch = -1;
	invalidate();
}

void commit_edit()
{
	if (g.edit_wheel < 0)
		return;
	g.edit_buf[g.edit_len] = 0;
	float v = (float)wcstod(g.edit_buf, nullptr);
	float lo, hi;
	readout_range(g.edit_wheel, g.edit_ch, &lo, &hi);
	if (v < lo)
		v = lo;
	if (v > hi)
		v = hi;
	readout_set(g.edit_wheel, g.edit_ch, v);
	if (g.host_valid && g.host.apply)
		g.host.apply(g.host.filter);
	g.edit_wheel = -1;
	g.edit_ch = -1;
	invalidate();
}

void apply_drag()
{
	if (!g.host_valid || !g.host.params)
		return;

	/* Readout scrub: dragging horizontally over a number changes that channel.
	 * The whole range takes ~500 px of travel, so a 230 px wheel is enough to
	 * cover it while leaving room for fine control under Shift. */
	if (g.scrub_wheel >= 0) {
		float lo, hi;
		readout_range(g.scrub_wheel, g.scrub_ch, &lo, &hi);
		const float fine = (GetKeyState(VK_SHIFT) & 0x8000) ? 0.1f : 1.0f;
		const float step = (hi - lo) / 500.0f * fine;
		float v = g.scrub_anchor_val +
			  ((float)g.drag_pt.x - (float)g.scrub_anchor_x) * step;
		if (v < lo)
			v = lo;
		if (v > hi)
			v = hi;
		readout_set(g.scrub_wheel, g.scrub_ch, v);
		if (g.host.mark_dirty)
			g.host.mark_dirty(g.host.filter);
		invalidate();
		return;
	}

	// Brightness strip: level of this tonal range only.
	if (g.drag_slider >= 0) {
		RECT rc;
		GetClientRect(g.hwnd, &rc);
		const int w = g.drag_slider;
		D2D1_RECT_F sr = slider_rect(w, (float)rc.right);
		const float width = sr.right - sr.left;
		const float range = kMasterHi[w] - kMasterLo[w];
		/* Shift = 1/6 speed for fine adjustment. */
		const float fine =
			(GetKeyState(VK_SHIFT) & 0x8000) ? 0.15f : 1.0f;
		float v = g.slider_anchor_val +
			  ((float)g.drag_pt.x - (float)g.slider_anchor_x) /
				  width * range * fine;
		if (v < kMasterLo[w])
			v = kMasterLo[w];
		if (v > kMasterHi[w])
			v = kMasterHi[w];
		wheel_set_master(g.host.params, w, v);
		if (g.host.mark_dirty)
			g.host.mark_dirty(g.host.filter);
		invalidate();
		return;
	}

	if (g.drag_wheel < 0)
		return;

	RECT rc;
	GetClientRect(g.hwnd, &rc);
	D2D1_RECT_F r = wheel_rect(g.drag_wheel, (float)rc.right);
	const float cx = (r.left + r.right) * 0.5f;
	const float cy = (r.top + r.bottom) * 0.5f;
	const float half = (r.right - r.left) * 0.5f;
	/* Normalise by the DISC radius, not the bounding box: the knob is drawn on
	 * the disc, so this is what makes it sit under the cursor. */
	const float discR = half * kDiscRatio;

	float dx = (g.drag_pt.x - cx) / discR;
	float dy = (g.drag_pt.y - cy) / discR;
	float rad = sqrtf(dx * dx + dy * dy);
	if (rad > 1.0f) {
		dx /= rad;
		dy /= rad;
	}

	float d[3];
	if (g.drag_ring) {
		// ring = master (luminance) for this wheel
		grade_wheel_to_rgb(dx, dy, 0.0f, d);
		const float master = -dy; // up = brighter
		for (int i = 0; i < 3; i++)
			d[i] = master * kWheelScale[g.drag_wheel];
	} else {
		grade_wheel_to_rgb(dx, dy, kWheelScale[g.drag_wheel], d);
	}
	wheel_write(g.host.params, g.drag_wheel, d);

	if (g.host.mark_dirty)
		g.host.mark_dirty(g.host.filter);
	invalidate();
}

void reset_wheel(int w)
{
	if (!g.host_valid || !g.host.params)
		return;
	// reset both the colour push and the brightness for this range
	wheel_set_master(g.host.params, w, kNeutral[w]);
	float zero[3] = {0.0f, 0.0f, 0.0f};
	wheel_write(g.host.params, w, zero);
	if (g.host.mark_dirty)
		g.host.mark_dirty(g.host.filter);
	invalidate();
}

/* --------------------------------------------------------------- window */

LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
	switch (msg) {
	case WM_NCCALCSIZE:
		if (wp)
			return 0;
		break;

	case WM_NCHITTEST: {
		POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
		RECT rc;
		GetClientRect(hwnd, &rc);
		POINT c = pt;
		ScreenToClient(hwnd, &c);
		const int b = 6;
		if (c.y < kTitleH) {
			/* Anything interactive inside the title bar must report
			 * HTCLIENT.  Returning HTCAPTION here makes the system treat the
			 * click as a window drag, so WM_LBUTTONDOWN never arrives and the
			 * close button looks dead. */
			if (c.x >= rc.right - 84 && c.y >= 2 && c.y <= kTitleH)
				return HTCLIENT;
			if (c.x < b && c.y < b)
				return HTTOPLEFT;
			if (c.x > rc.right - b && c.y < b)
				return HTTOPRIGHT;
			return HTCAPTION;
		}
		if (c.x < b) return HTLEFT;
		if (c.x > rc.right - b) return HTRIGHT;
		if (c.y > rc.bottom - b) return HTBOTTOM;
		return HTCLIENT;
	}

	case WM_GETMINMAXINFO: {
		MINMAXINFO *mmi = (MINMAXINFO *)lp;
		mmi->ptMinTrackSize.x = 760;
		mmi->ptMinTrackSize.y = 300;
		return 0;
	}

	case WM_DPICHANGED: {
		g.dpi = (float)HIWORD(wp) / 96.0f;
		RECT *r = (RECT *)lp;
		SetWindowPos(hwnd, nullptr, r->left, r->top,
			     r->right - r->left, r->bottom - r->top,
			     SWP_NOZORDER | SWP_NOACTIVATE);
		if (g.wheel_bmp) {
			g.wheel_bmp->Release();
			g.wheel_bmp = nullptr;
			g.wheel_bmp_size = 0;
		}
		if (g.f_title) { g.f_title->Release(); g.f_title = nullptr; }
		if (g.f_label) { g.f_label->Release(); g.f_label = nullptr; }
		if (g.f_value) { g.f_value->Release(); g.f_value = nullptr; }
		if (g.dwrite) {
			g.dwrite->CreateTextFormat(
				L"Microsoft YaHei UI", nullptr,
				DWRITE_FONT_WEIGHT_SEMI_BOLD,
				DWRITE_FONT_STYLE_NORMAL,
				DWRITE_FONT_STRETCH_NORMAL, 15.0f * g.dpi,
				L"zh-cn", &g.f_title);
			g.dwrite->CreateTextFormat(
				L"Microsoft YaHei UI", nullptr,
				DWRITE_FONT_WEIGHT_NORMAL,
				DWRITE_FONT_STYLE_NORMAL,
				DWRITE_FONT_STRETCH_NORMAL, 13.0f * g.dpi,
				L"zh-cn", &g.f_label);
			g.dwrite->CreateTextFormat(
				L"Consolas", nullptr,
				DWRITE_FONT_WEIGHT_NORMAL,
				DWRITE_FONT_STYLE_NORMAL,
				DWRITE_FONT_STRETCH_NORMAL, 13.0f * g.dpi,
				L"en-us", &g.f_value);
		}
		return 0;
	}

	case WM_SIZE:
		if (wp != SIZE_MINIMIZED) {
			resize();
			invalidate();
		}
		return 0;

	case WM_PAINT: {
		PAINTSTRUCT ps;
		BeginPaint(hwnd, &ps);
		HRESULT hr = draw();
		if (hr == D2DERR_RECREATE_TARGET) {
			// Driver update / GPU reset / remote-desktop switch.  Without this
			// the window simply goes black and looks like a crash.
			discard_target();
			create_target();
			hr = draw();
		}
		if (SUCCEEDED(hr))
			present();
		EndPaint(hwnd, &ps);
		return 0;
	}

	case WM_TIMER:
		if (wp == 1) {
			KillTimer(hwnd, 1);
			g.timer = 0;
			/* Every drag kind must be serviced.  Checking only drag_wheel
			 * left the brightness strip applying just the initial click. */
			if (g.drag_wheel >= 0 || g.drag_slider >= 0 ||
			    g.scrub_wheel >= 0)
				apply_drag();
		}
		return 0;

	case WM_LBUTTONDOWN: {
		POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
		RECT crc;
		GetClientRect(hwnd, &crc);
		if (pt.x >= crc.right - 84 && pt.y >= 2 && pt.y <= 40) {
			/* Highlight on press, act on release over the button -- the
			 * usual desktop button contract. */
			g.press_btn = pt.x < crc.right - 48 ? 4 : 5;
			invalidate();
			return 0;
		}
		/* Per-wheel reset buttons sit above the disc, so they must be tested
		 * before the readouts and the slider. */
		{
			const int rbtn = hit_reset(pt);
			if (rbtn >= 0) {
				g.press_reset = rbtn;
				invalidate();
				return 0;
			}
		}
		/* Commit an open edit when the click lands anywhere else. */
		if (g.edit_wheel >= 0) {
			int ech = -1;
			const int ew = hit_readout(pt, &ech);
			if (!(ew == g.edit_wheel && ech == g.edit_ch))
				commit_edit();
		}
		int rch = -1;
		const int rw = hit_readout(pt, &rch);
		if (rw >= 0) {
			g.scrub_wheel = rw;
			g.scrub_ch = rch;
			g.scrub_anchor_x = pt.x;
			g.scrub_anchor_val = readout_value(rw, rch);
			SetCapture(hwnd);
			return 0;
		}
		int sw = hit_slider(pt);
		if (sw >= 0) {
			RECT rc;
			GetClientRect(hwnd, &rc);
			D2D1_RECT_F sr = slider_rect(sw, (float)rc.right);
			float t = ((float)pt.x - sr.left) / (sr.right - sr.left);
			if (t < 0.0f)
				t = 0.0f;
			if (t > 1.0f)
				t = 1.0f;
			const float v =
				kMasterLo[sw] + t * (kMasterHi[sw] - kMasterLo[sw]);
			/* Click jumps to the spot, then the drag is relative to this
			 * anchor so Shift can give sub-pixel precision. */
			wheel_set_master(g.host.params, sw, v);
			g.slider_anchor_x = pt.x;
			g.slider_anchor_val = v;
			g.drag_slider = sw;
			g.drag_wheel = -1;
			g.drag_pt = pt;
			SetCapture(hwnd);
			apply_drag();
			return 0;
		}
		int w = hit_wheel(pt);
		if (w >= 0) {
			RECT rc;
			GetClientRect(hwnd, &rc);
			g.drag_wheel = w;
			g.drag_slider = -1;
			// the outer ring is an indicator only; brightness lives on the
			// strip, matching Resolve
			g.drag_ring = false;
			g.drag_pt = pt;
			SetCapture(hwnd);
			apply_drag();
		}
		return 0;
	}

	case WM_MOUSEMOVE: {
		const int mx = GET_X_LPARAM(lp), my = GET_Y_LPARAM(lp);
		/* Track the latest position here (message coordinates work under
		 * SetCapture and are driveable from a test harness), then coalesce the
		 * expensive work into a 16 ms timer. */
		if (g.drag_wheel >= 0 || g.drag_slider >= 0 ||
		    g.scrub_wheel >= 0) {
			g.drag_pt.x = mx;
			g.drag_pt.y = my;
			if (!g.timer)
				g.timer = SetTimer(hwnd, 1, 16, nullptr);
			return 0;
		}
		/* Hover feedback for the title-bar buttons. */
		RECT crc;
		GetClientRect(hwnd, &crc);
		int hb = -1;
		if (my >= 2 && my <= 40 && mx >= crc.right - 84)
			hb = mx < crc.right - 48 ? 4 : 5;
		if (hb != g.hover_btn) {
			g.hover_btn = hb;
			invalidate();
		}
		{
			POINT hp = {mx, my};
			const int hr = hit_reset(hp);
			if (hr != g.hover_reset) {
				g.hover_reset = hr;
				invalidate();
			}
		}
		TRACKMOUSEEVENT tme = {};
		tme.cbSize = sizeof(tme);
		tme.dwFlags = TME_LEAVE;
		tme.hwndTrack = hwnd;
		TrackMouseEvent(&tme);
		return 0;
	}

	case WM_MOUSELEAVE:
		if (g.hover_btn != -1 || g.hover_reset != -1) {
			g.hover_btn = -1;
			g.hover_reset = -1;
			invalidate();
		}
		return 0;

	case WM_LBUTTONUP: {
		POINT up = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
		if (g.press_reset >= 0) {
			const int rw = g.press_reset;
			g.press_reset = -1;
			if (hit_reset(up) == rw) {
				reset_wheel(rw);
				if (g.host_valid && g.host.apply)
					g.host.apply(g.host.filter);
			}
			invalidate();
			return 0;
		}
		if (g.scrub_wheel >= 0) {
			g.scrub_wheel = -1;
			g.scrub_ch = -1;
			ReleaseCapture();
			if (g.timer) { KillTimer(hwnd, 1); g.timer = 0; }
			if (g.host_valid && g.host.apply)
				g.host.apply(g.host.filter);
			invalidate();
			return 0;
		}
		if (g.press_btn >= 0) {
			RECT crc;
			GetClientRect(hwnd, &crc);
			const bool inTitle = up.y >= 2 && up.y <= 40;
			const bool overPin =
				inTitle && up.x >= crc.right - 84 &&
				up.x < crc.right - 48;
			const bool overClose = inTitle && up.x >= crc.right - 48 &&
					       up.x < crc.right - 12;
			if (g.press_btn == 4 && overPin) {
				g.pinned = !g.pinned;
				SetWindowPos(hwnd,
					     g.pinned ? HWND_TOPMOST
						      : HWND_NOTOPMOST,
					     0, 0, 0, 0,
					     SWP_NOMOVE | SWP_NOSIZE);
				save_state();
				SetForegroundWindow(hwnd);
			} else if (g.press_btn == 5 && overClose) {
				save_state();
				ShowWindow(hwnd, SW_HIDE);
			}
			g.press_btn = -1;
			invalidate();
			return 0;
		}
		if (g.drag_wheel >= 0 || g.drag_slider >= 0) {
			g.drag_wheel = -1;
			g.drag_slider = -1;
			ReleaseCapture();
			if (g.timer) { KillTimer(hwnd, 1); g.timer = 0; }
			if (g.host_valid && g.host.apply)
				g.host.apply(g.host.filter);
			invalidate();
		}
		return 0;
	}

	case WM_LBUTTONDBLCLK: {
		POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
		int ech = -1;
		const int ew = hit_readout(pt, &ech);
		if (ew >= 0) {
			g.scrub_wheel = -1;
			g.scrub_ch = -1;
			begin_edit(ew, ech);
			return 0;
		}
		int sw = hit_slider(pt);
		if (sw >= 0) {
			wheel_set_master(g.host.params, sw, kNeutral[sw]);
			if (g.host_valid && g.host.apply)
				g.host.apply(g.host.filter);
			invalidate();
			return 0;
		}
		int w = hit_wheel(pt);
		if (w >= 0) {
			reset_wheel(w);
			if (g.host_valid && g.host.apply)
				g.host.apply(g.host.filter);
		}
		return 0;
	}

	case WM_KEYDOWN:
		/* While a readout is being edited the keyboard belongs to it. */
		if (g.edit_wheel >= 0) {
			switch (wp) {
			case VK_BACK:
				if (g.edit_len > 0)
					g.edit_len--;
				invalidate();
				return 0;
			case VK_RETURN:
				commit_edit();
				return 0;
			case VK_ESCAPE:
				cancel_edit();
				return 0;
			case VK_TAB:
				commit_edit();
				break; /* fall through to the focus cycle */
			default:
				return 0;
			}
		}
		/* Keyboard operation: Tab walks the controls, arrows nudge the
		 * focused wheel's brightness, Enter/Space presses the focused button.
		 * Without this the editor is mouse-only. */
		switch (wp) {
		case VK_TAB:
			if (GetKeyState(VK_SHIFT) & 0x8000)
				g.focus = g.focus <= 0 ? 5 : g.focus - 1;
			else
				g.focus = g.focus >= 5 ? -1 : g.focus + 1;
			invalidate();
			return 0;
		case VK_LEFT:
		case VK_RIGHT:
			if (g.focus >= 0 && g.focus < W_COUNT && g.host_valid &&
			    g.host.params) {
				const int w = g.focus;
				const float step =
					(GetKeyState(VK_SHIFT) & 0x8000) ? 0.001f
									 : 0.01f;
				float v = wheel_master(g.host.params, w) +
					  (wp == VK_RIGHT ? step : -step);
				if (v < kMasterLo[w])
					v = kMasterLo[w];
				if (v > kMasterHi[w])
					v = kMasterHi[w];
				wheel_set_master(g.host.params, w, v);
				if (g.host.apply)
					g.host.apply(g.host.filter);
				invalidate();
			}
			return 0;
		case VK_RETURN:
		case VK_SPACE:
			if (g.focus == 4) {
				g.pinned = !g.pinned;
				SetWindowPos(hwnd,
					     g.pinned ? HWND_TOPMOST
						      : HWND_NOTOPMOST,
					     0, 0, 0, 0,
					     SWP_NOMOVE | SWP_NOSIZE);
				save_state();
				invalidate();
			} else if (g.focus == 5) {
				save_state();
				ShowWindow(hwnd, SW_HIDE);
			}
			return 0;
		}
		return 0;

	case WM_CHAR:
		if (g.edit_wheel >= 0) {
			const wchar_t c = (wchar_t)wp;
			if ((c >= L'0' && c <= L'9') || c == L'.' || c == L'-' ||
			    c == L'+') {
				if (g.edit_replace) {
					g.edit_len = 0;
					g.edit_replace = false;
				}
				if (g.edit_len < 20) {
					g.edit_buf[g.edit_len++] = c;
					invalidate();
				}
			}
			return 0;
		}
		return 0;

	case WM_CLOSE:
		save_state();
		ShowWindow(hwnd, SW_HIDE);
		return 0;

	case WM_EXITSIZEMOVE:
		save_state();
		return 0;

	default:
		break;
	}
	return DefWindowProcW(hwnd, msg, wp, lp);
}

} // namespace

/* ------------------------------------------------------------------ ABI */

extern "C" void obs_colorwheel_editor_open(const colorwheel_editor_host *host)
{
	if (!host)
		return;

	g.host = *host;
	g.host_valid = true;

	/* Re-opening while the window exists: restore whichever z-order the user
	 * last chose instead of forcing topmost again. */
	if (g.hwnd) {
		SetWindowPos(g.hwnd, g.pinned ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0,
			     0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
		SetForegroundWindow(g.hwnd);
		invalidate();
		return;
	}

	g.dpi = 1.0f;

	if (host->config_path && host->config_path[0])
		MultiByteToWideChar(CP_UTF8, 0, host->config_path, -1, g.cfg,
				    MAX_PATH);
	load_state();

	WNDCLASSEXW wc = {};
	wc.cbSize = sizeof(wc);
	wc.style = CS_DBLCLKS;
	wc.lpfnWndProc = wnd_proc;
	wc.hInstance = GetModuleHandleW(nullptr);
	wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
	wc.lpszClassName = kClassName;
	RegisterClassExW(&wc);

	RECT place;
	initial_rect(&place);
	RECT rc = {0, 0, place.right - place.left, place.bottom - place.top};
	AdjustWindowRectEx(&rc, WS_POPUP | WS_THICKFRAME, FALSE,
			   (g.pinned ? WS_EX_TOPMOST : 0) | WS_EX_TOOLWINDOW);
	g.hwnd = CreateWindowExW((g.pinned ? WS_EX_TOPMOST : 0) | WS_EX_TOOLWINDOW,
				 kClassName, L"一级校色轮",
				 WS_POPUP | WS_THICKFRAME, place.left, place.top,
				 rc.right - rc.left, rc.bottom - rc.top, nullptr,
				 nullptr, wc.hInstance, nullptr);
	if (!g.hwnd)
		return;

	g.dpi = (float)GetDpiForWindow(g.hwnd) / 96.0f;

	if (!create_device() || !create_target()) {
		/* Never put a modal message box here.  This runs on OBS's UI thread,
		 * so a modal dialog would freeze the whole application when D3D is
		 * unavailable (remote desktop, driver reset, VM).  Log and leave the
		 * filter usable through the properties panel instead. */
		OutputDebugStringW(L"[obs-colorwheel] D3D11 init failed; "
				   L"editor unavailable, properties still usable\n");
		release_all();
		DestroyWindow(g.hwnd);
		g.hwnd = nullptr;
		return;
	}

	ShowWindow(g.hwnd, SW_SHOW);
	SetForegroundWindow(g.hwnd);
	SetFocus(g.hwnd);
	invalidate();
}

extern "C" bool obs_colorwheel_editor_on_filter_destroyed(void *filter)
{
	if (!g.host_valid || g.host.filter != filter)
		return true;
	/* Keep the host alive but mark it disconnected; the filter will leak and
	 * we must never dereference its params again. */
	g.host_valid = false;
	g.host.params = nullptr;
	g.host.filter = nullptr;
	g.drag_wheel = -1;
	invalidate();
	return false;
}

#ifdef CW_TEST
extern "C" int cw_test_knob(int w, float *ux, float *uy)
{
	if (!g.host_valid || !g.host.params)
		return 0;
	knob_uv(w, ux, uy);
	return 1;
}

extern "C" void cw_test_readout(int w, int ch, float *cx, float *cy)
{
	RECT rc;
	GetClientRect(g.hwnd, &rc);
	D2D1_RECT_F r = readout_rect(w, ch, (float)rc.right);
	*cx = (r.left + r.right) * 0.5f;
	*cy = (r.top + r.bottom) * 0.5f;
}

extern "C" void cw_test_reset(int w, float *cx, float *cy)
{
	RECT rc;
	GetClientRect(g.hwnd, &rc);
	D2D1_RECT_F r = reset_rect(w, (float)rc.right);
	*cx = (r.left + r.right) * 0.5f;
	*cy = (r.top + r.bottom) * 0.5f;
}

extern "C" int cw_test_editing(void)
{
	return g.edit_wheel >= 0 && g.edit_ch >= 0;
}
#endif

#ifdef CW_TEST
/* Test-only: expose the computed layout so the harness can click exactly on
 * the brightness strip and the wheel disc instead of hard-coding coordinates. */
extern "C" void cw_test_layout(int w, int *sl, int *st, int *sw_, int *sh_,
			       int *wl, int *wt, int *ww_, int *wh_)
{
	if (!g.hwnd)
		return;
	RECT rc;
	GetClientRect(g.hwnd, &rc);
	const D2D1_RECT_F s = slider_rect(w, (float)rc.right);
	const D2D1_RECT_F q = wheel_rect(w, (float)rc.right);
	if (sl)
		*sl = (int)s.left;
	if (st)
		*st = (int)s.top;
	if (sw_)
		*sw_ = (int)(s.right - s.left);
	if (sh_)
		*sh_ = (int)(s.bottom - s.top);
	if (wl)
		*wl = (int)q.left;
	if (wt)
		*wt = (int)q.top;
	if (ww_)
		*ww_ = (int)(q.right - q.left);
	if (wh_)
		*wh_ = (int)(q.bottom - q.top);
}
#endif
