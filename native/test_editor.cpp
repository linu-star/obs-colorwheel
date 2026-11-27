/* Offline UI harness: opens the editor with a fake host, so the window can be
 * smoke-tested and screenshotted without OBS.  Same idea as
 * obs-curves/native/test_editor.cpp.  Not part of the shipped DLL. */
#include <windows.h>

#include <math.h>
#include <stdio.h>

#include "colorwheel_editor.h"

static grade_params_t g_params;

static const char *fake_name(void *)
{
	/* ASCII on purpose: a Chinese narrow literal gets transcoded to the system
	 * code page by mingw, which would then be decoded as UTF-8 and look like
	 * mojibake.  The real plugin receives UTF-8 from obs_source_get_name(). */
	return "Test Source 1920x1080";
}

static void fake_size(void *, int *w, int *h)
{
	*w = 1920;
	*h = 1080;
}

static void fake_apply(void *) {}
static void fake_dirty(void *) {}

int main(int argc, char **argv)
{
	grade_defaults(&g_params);

	/* Plausible grade: a small colour push plus a brightness offset per wheel. */
	g_params.lift[0] = 0.06f;
	g_params.lift[1] = 0.02f;
	g_params.lift[2] = 0.04f;
	g_params.gamma[0] = 0.10f;
	g_params.gamma[1] = 0.01f;
	g_params.gamma[2] = -0.03f;
	g_params.gain[0] = 1.08f;
	g_params.gain[1] = 1.00f;
	g_params.gain[2] = 0.97f;
	g_params.offset[0] = 25.0f;
	g_params.offset[1] = 25.0f;
	g_params.offset[2] = 27.0f;

	colorwheel_editor_host host = {};
	host.filter = nullptr;
	host.params = &g_params;
	host.source_name = fake_name(nullptr);
	host.get_source_name = fake_name;
	host.get_target_size = fake_size;
	host.apply = fake_apply;
	host.mark_dirty = fake_dirty;

	/* Deterministic geometry: seed the window-state file the editor reads, so
	 * an external screenshot can target a fixed rectangle. */
	const wchar_t *cfg = L"C:\\Users\\qiusen\\.dsh\\dsh_wechat\\obs-colorwheel\\native\\tmp\\harness_window.ini";
	WritePrivateProfileStringW(L"window", L"x", L"150", cfg);
	WritePrivateProfileStringW(L"window", L"y", L"150", cfg);
	WritePrivateProfileStringW(L"window", L"w", L"1280", cfg);
	WritePrivateProfileStringW(L"window", L"h", L"460", cfg);
	WritePrivateProfileStringW(L"window", L"pinned", L"1", cfg);
	host.config_path =
		"C:\\Users\\qiusen\\.dsh\\dsh_wechat\\obs-colorwheel\\native\\tmp\\harness_window.ini";

	obs_colorwheel_editor_open(&host);

	HWND h = FindWindowW(L"OBSColorWheelEditor", nullptr);
	printf("editor_hwnd=%p  %s\n", (void *)h,
	       h ? "window created" : "D3D11 init failed, no window");
	if (h) {
		RECT wr;
		GetWindowRect(h, &wr);
		printf("window=%ld,%ld,%ld,%ld\n", (long)wr.left, (long)wr.top,
		       (long)(wr.right - wr.left), (long)(wr.bottom - wr.top));
	}
	fflush(stdout);

	/* Regression probe for the custom-chrome hit test: the close button must
	 * resolve to HTCLIENT, otherwise HTCAPTION swallows the click and the
	 * button never receives WM_LBUTTONDOWN. */
	if (h) {
		RECT wr;
		GetWindowRect(h, &wr);
		LRESULT over_close = SendMessageW(
			h, WM_NCHITTEST, 0,
			MAKELPARAM(wr.right - 28, wr.top + 20));
		LRESULT over_title = SendMessageW(
			h, WM_NCHITTEST, 0,
			MAKELPARAM(wr.left + 200, wr.top + 20));
		printf("nchittest close=%ld (want %d)  title=%ld (want %d)\n",
		       (long)over_close, HTCLIENT, (long)over_title, HTCAPTION);
		fflush(stdout);
	}

	const int ms = argc > 1 ? atoi(argv[1]) : 6000;
	const DWORD end = GetTickCount() + (DWORD)ms;
	MSG msg;
	while (GetTickCount() < end) {
		while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
			TranslateMessage(&msg);
			DispatchMessageW(&msg);
		}
		Sleep(10);
	}
	return 0;
}
