// obs-colorwheel native filter (MinGW-w64 / C ABI).
//
// Architecture mirrors obs-curves/native/obs-curves.c on purpose, so the two
// plugins can share an editor shell later:
//   * one obs_source_info filter, video_render does begin/params/end
//   * parameters live in a plain struct (grade_params_t), parsed in update()
//   * the UI is a separate translation unit behind a small C ABI
//
// Differences, both deliberate:
//   * No LUT.  LGG is uniform maths in the shader (see colorwheel.effect).
//   * GS_RGBA16F intermediate + OBS_NO_DIRECT_RENDERING.  Under
//     OBS_ALLOW_DIRECT_RENDERING the bypass path ignores `format` entirely, so
//     a single-filter chain would silently run on the source texture.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <obs-module.h>
#include <graphics/graphics.h>
#include <graphics/vec4.h>
#include <util/bmem.h>

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <windows.h>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("obs-colorwheel", "en-US")

#include "colorwheel_editor.h"
#include "grade_engine.h"

struct colorwheel_data {
	obs_source_t *source;
	gs_effect_t *effect;

	gs_eparam_t *p_lift;
	gs_eparam_t *p_gamma;
	gs_eparam_t *p_gain;
	gs_eparam_t *p_offset;
	gs_eparam_t *p_contrast;
	gs_eparam_t *p_sat;
	gs_eparam_t *p_hue;
	gs_eparam_t *p_lumMix;
	gs_eparam_t *p_extra;

	grade_params_t cur;     /* render-side copy, guarded by lock */
	grade_params_t pending; /* editor-side copy */
	CRITICAL_SECTION lock;

	bool active;          /* false => skip the filter, straight passthrough */
	bool graphics_failed;
};

/* ---------------------------------------------------------------- settings */

static const char *k_chan_keys[4][3] = {
	{"pg_liftR", "pg_liftG", "pg_liftB"},
	{"pg_gammaR", "pg_gammaG", "pg_gammaB"},
	{"pg_gainR", "pg_gainG", "pg_gainB"},
	{"pg_offR", "pg_offG", "pg_offB"},
};

static void read_params(obs_data_t *s, grade_params_t *p)
{
	grade_defaults(p);
	for (int i = 0; i < 3; i++) {
		p->lift[i] = (float)obs_data_get_double(s, k_chan_keys[0][i]);
		p->gamma[i] = (float)obs_data_get_double(s, k_chan_keys[1][i]);
		p->gain[i] = (float)obs_data_get_double(s, k_chan_keys[2][i]);
		p->offset[i] = (float)obs_data_get_double(s, k_chan_keys[3][i]);
	}
	p->contrast = (float)obs_data_get_double(s, "pg_contrast");
	p->pivot = (float)obs_data_get_double(s, "pg_pivot");
	p->sat = (float)obs_data_get_double(s, "pg_sat");
	p->hue = (float)obs_data_get_double(s, "pg_hue");
	p->lum_mix = (float)obs_data_get_double(s, "pg_lummix");
	p->boost = (float)obs_data_get_double(s, "pg_boost");
	p->shadows = (float)obs_data_get_double(s, "pg_shadows");
	p->highlights = (float)obs_data_get_double(s, "pg_highlights");
}

static void write_params(obs_data_t *s, const grade_params_t *p)
{
	for (int i = 0; i < 3; i++) {
		obs_data_set_double(s, k_chan_keys[0][i], p->lift[i]);
		obs_data_set_double(s, k_chan_keys[1][i], p->gamma[i]);
		obs_data_set_double(s, k_chan_keys[2][i], p->gain[i]);
		obs_data_set_double(s, k_chan_keys[3][i], p->offset[i]);
	}
	obs_data_set_double(s, "pg_contrast", p->contrast);
	obs_data_set_double(s, "pg_pivot", p->pivot);
	obs_data_set_double(s, "pg_sat", p->sat);
	obs_data_set_double(s, "pg_hue", p->hue);
	obs_data_set_double(s, "pg_lummix", p->lum_mix);
	obs_data_set_double(s, "pg_boost", p->boost);
	obs_data_set_double(s, "pg_shadows", p->shadows);
	obs_data_set_double(s, "pg_highlights", p->highlights);
}

/* -------------------------------------------------------------- resources */

static void ensure_effect(struct colorwheel_data *d)
{
	if (d->graphics_failed || d->effect)
		return;

	char *path = obs_module_file("effects/colorwheel.effect");
	char *errors = NULL;
	if (path)
		d->effect = gs_effect_create_from_file(path, &errors);
	if (!d->effect) {
		blog(LOG_ERROR, "[obs-colorwheel] effect load failed: %s",
		     errors ? errors : "(no error)");
		bfree(errors);
		bfree(path);
		d->graphics_failed = true;
		return;
	}
	bfree(errors);
	bfree(path);

	d->p_lift = gs_effect_get_param_by_name(d->effect, "u_lift");
	d->p_gamma = gs_effect_get_param_by_name(d->effect, "u_gamma");
	d->p_gain = gs_effect_get_param_by_name(d->effect, "u_gain");
	d->p_offset = gs_effect_get_param_by_name(d->effect, "u_offset");
	d->p_contrast =
		gs_effect_get_param_by_name(d->effect, "u_contrast");
	d->p_sat = gs_effect_get_param_by_name(d->effect, "u_sat");
	d->p_hue = gs_effect_get_param_by_name(d->effect, "u_hue");
	d->p_lumMix = gs_effect_get_param_by_name(d->effect, "u_lumMix");
	d->p_extra = gs_effect_get_param_by_name(d->effect, "u_extra");
}

static void set_vec(gs_eparam_t *p, float x, float y, float z, float w)
{
	struct vec4 v;
	if (!p)
		return;
	vec4_set(&v, x, y, z, w);
	gs_effect_set_vec4(p, &v);
}

/* ------------------------------------------------------------ source info */

static const char *cw_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("ColorWheel");
}

static void *cw_create(obs_data_t *settings, obs_source_t *source)
{
	struct colorwheel_data *d = bzalloc(sizeof(*d));
	InitializeCriticalSection(&d->lock);
	d->source = source;
	grade_defaults(&d->cur);
	grade_defaults(&d->pending);
	d->active = false;

	/* OBS does not call update() on create; without this the panel values and
	 * the rendered result disagree until the first slider move. */
	obs_data_t *s = obs_source_get_settings(source);
	if (s) {
		read_params(s, &d->cur);
		d->pending = d->cur;
		d->active = !grade_is_neutral(&d->cur);
		obs_data_release(s);
	}
	UNUSED_PARAMETER(settings);
	return d;
}

static void cw_destroy(void *data)
{
	struct colorwheel_data *d = data;
	if (!obs_colorwheel_editor_on_filter_destroyed(d)) {
		/* The editor still holds a reference.  Leaking beats freeing memory a
		 * UI thread may still paint (same rule as obs-curves). */
		blog(LOG_ERROR,
		     "[obs-colorwheel] editor did not close; leaking filter");
		return;
	}
	obs_enter_graphics();
	if (d->effect)
		gs_effect_destroy(d->effect);
	obs_leave_graphics();
	DeleteCriticalSection(&d->lock);
	bfree(d);
}

static void cw_update(void *data, obs_data_t *settings)
{
	struct colorwheel_data *d = data;
	grade_params_t p;
	read_params(settings, &p);
	EnterCriticalSection(&d->lock);
	d->cur = p;
	d->pending = p;
	d->active = !grade_is_neutral(&p);
	LeaveCriticalSection(&d->lock);
}

static uint32_t cw_width(void *data)
{
	struct colorwheel_data *d = data;
	obs_source_t *t = obs_filter_get_target(d->source);
	return t ? obs_source_get_base_width(t) : 0;
}

static uint32_t cw_height(void *data)
{
	struct colorwheel_data *d = data;
	obs_source_t *t = obs_filter_get_target(d->source);
	return t ? obs_source_get_base_height(t) : 0;
}

/* The UI -> internal conversions (lift x2, offset 0.0073327/unit, gamma
 * exponent, contrast logistic) live in grade_engine.c so the test suite can
 * exercise them directly.  See grade_engine.h for the calibration notes. */

static void cw_render(void *data, gs_effect_t *effect)
{
	UNUSED_PARAMETER(effect);
	struct colorwheel_data *d = data;
	obs_source_t *target = obs_filter_get_target(d->source);
	if (!target) {
		obs_source_skip_video_filter(d->source);
		return;
	}

	grade_params_t p;
	bool active;
	EnterCriticalSection(&d->lock);
	p = d->cur;
	active = d->active;
	LeaveCriticalSection(&d->lock);

	/* Neutral grade: skip entirely rather than run a pass that is the
	 * identity.  Saves a full-screen pass and guarantees pixel-exact
	 * passthrough, which is also how the "neutral == identity" check is
	 * verified. */
	if (!active) {
		obs_source_skip_video_filter(d->source);
		return;
	}

	uint32_t w = obs_source_get_base_width(target);
	uint32_t h = obs_source_get_base_height(target);

	EnterCriticalSection(&d->lock);
	ensure_effect(d);
	gs_effect_t *fx = d->effect;
	LeaveCriticalSection(&d->lock);
	if (!fx) {
		obs_source_skip_video_filter(d->source);
		return;
	}

	/* Render in the source's OWN colour space.  Declaring a different space
	 * (e.g. GS_CS_SRGB_16F for a plain sRGB source) makes OBS decode into that
	 * space when drawing the source into the target while nothing encodes the
	 * result back, so the grade silently runs on linear values -- every
	 * control ends up wrong and a double gamma is applied.  Following the
	 * source space keeps the pipeline in one domain, which is also what the
	 * calibration against Resolve's LUTs assumed. */
	const enum gs_color_space space = obs_source_get_color_space(target, 0, NULL);
	const enum gs_color_format format = gs_get_format_from_space(space);
	if (!obs_source_process_filter_begin_with_color_space(
		    d->source, format, space, OBS_NO_DIRECT_RENDERING))
		return;

	set_vec(d->p_lift, p.lift[0] * GRADE_LIFT_UI_TO_INTERNAL,
		p.lift[1] * GRADE_LIFT_UI_TO_INTERNAL,
		p.lift[2] * GRADE_LIFT_UI_TO_INTERNAL, 0.0f);
	set_vec(d->p_gamma, grade_gamma_exponent(p.gamma[0]),
		grade_gamma_exponent(p.gamma[1]),
		grade_gamma_exponent(p.gamma[2]), 0.0f);
	set_vec(d->p_gain, p.gain[0], p.gain[1], p.gain[2], 0.0f);
	set_vec(d->p_offset, (p.offset[0] - 25.0f) * GRADE_OFFSET_UI_UNIT,
		(p.offset[1] - 25.0f) * GRADE_OFFSET_UI_UNIT,
		(p.offset[2] - 25.0f) * GRADE_OFFSET_UI_UNIT, 0.0f);
	{
		float cu[4];
		grade_contrast_uniforms(p.contrast, p.pivot, cu);
		set_vec(d->p_contrast, cu[0], cu[1], cu[2], cu[3]);
	}
	set_vec(d->p_sat, p.sat / 50.0f, 0.0f, 0.0f, 0.0f);
	set_vec(d->p_hue, (p.hue - 50.0f) / 50.0f * GRADE_PI, 0.0f, 0.0f,
		0.0f);
	set_vec(d->p_lumMix, p.lum_mix / 100.0f, 0.0f, 0.0f, 0.0f);
	set_vec(d->p_extra, p.boost, p.shadows, p.highlights, 0.0f);

	obs_source_process_filter_end(d->source, fx, w, h);
}

/* ------------------------------------------------------------- editor glue */

static void filter_apply(void *filter)
{
	struct colorwheel_data *d = filter;
	if (!d || !d->source)
		return;

	grade_params_t p;
	EnterCriticalSection(&d->lock);
	p = d->pending;
	d->cur = p;
	d->active = !grade_is_neutral(&p);
	LeaveCriticalSection(&d->lock);

	/* Commit to settings.  obs_source_get_settings() returns the live object,
	 * but OBS still needs an explicit obs_source_update() to run update() on
	 * the next tick -- this is NOT a copy. */
	obs_data_t *s = obs_source_get_settings(d->source);
	if (!s)
		return;
	write_params(s, &p);
	obs_source_update(d->source, s);
	obs_data_release(s);
}

static void filter_mark_dirty(void *filter)
{
	struct colorwheel_data *d = filter;
	if (!d)
		return;
	/* Live preview while dragging: push the working copy into the render
	 * struct without touching OBS settings.  Settings are only written on
	 * mouse-up via filter_apply(), so a drag never spams obs_source_update. */
	EnterCriticalSection(&d->lock);
	d->cur = d->pending;
	d->active = !grade_is_neutral(&d->pending);
	LeaveCriticalSection(&d->lock);
}

static obs_source_t *filter_parent(void *filter)
{
	struct colorwheel_data *d = filter;
	/* obs_filter_get_target() is the previous filter in the chain; the parent
	 * is the actual source whose name/size we want to show. */
	return (d && d->source) ? obs_filter_get_parent(d->source) : NULL;
}

static const char *host_get_source_name(void *filter)
{
	obs_source_t *t = filter_parent(filter);
	return t ? obs_source_get_name(t) : NULL;
}

static void host_get_target_size(void *filter, int *w, int *h)
{
	obs_source_t *t = filter_parent(filter);
	if (w)
		*w = t ? (int)obs_source_get_width(t) : 0;
	if (h)
		*h = t ? (int)obs_source_get_height(t) : 0;
}

static void open_editor_for(struct colorwheel_data *d)
{
	if (!d)
		return;

	colorwheel_editor_host host;
	memset(&host, 0, sizeof(host));

	EnterCriticalSection(&d->lock);
	d->pending = d->cur;
	LeaveCriticalSection(&d->lock);

	host.filter = d;
	host.params = &d->pending;
	host.source_name = host_get_source_name(d);
	host.target_width = 0;
	host.target_height = 0;
	host_get_target_size(d, &host.target_width, &host.target_height);
	host.get_source_name = host_get_source_name;
	host.get_target_size = host_get_target_size;
	host.apply = filter_apply;
	host.mark_dirty = filter_mark_dirty;
	host.config_path = obs_module_config_path("window.ini");

	blog(LOG_INFO, "[obs-colorwheel] editor target='%s' %dx%d",
	     host.source_name ? host.source_name : "?", host.target_width,
	     host.target_height);
	obs_colorwheel_editor_open(&host);
	bfree((void *)host.config_path);
	host.config_path = NULL;
}

static bool open_editor_clicked(obs_properties_t *props, obs_property_t *prop,
				void *data)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(prop);
	open_editor_for((struct colorwheel_data *)data);
	/* false: do not rebuild the properties panel.  Returning true here would
	 * deleteLater() the widget that is still inside this callback. */
	return false;
}

static bool reset_clicked(obs_properties_t *props, obs_property_t *prop,
			  void *data)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(prop);
	struct colorwheel_data *d = data;
	if (!d || !d->source)
		return false;
	grade_params_t p;
	grade_defaults(&p);
	obs_data_t *s = obs_source_get_settings(d->source);
	if (!s)
		return false;
	write_params(s, &p);
	obs_source_update(d->source, s);
	obs_data_release(s);
	return false;
}

static obs_properties_t *cw_properties(void *data)
{
	obs_properties_t *props = obs_properties_create();
	obs_properties_add_button2(props, "open_editor", "打开色轮编辑器",
				   open_editor_clicked, data);
	obs_properties_add_button2(props, "reset", "全部重置", reset_clicked,
				   data);
	obs_properties_add_text(
		props, "hint",
		"色轮的 Lift/Gamma/Gain/Offset 由编辑器控制；下方滑杆用于快速调整",
		OBS_TEXT_INFO);
	obs_properties_add_float_slider(props, "pg_contrast", "对比度", 0.0,
					4.0, 0.001);
	obs_properties_add_float_slider(props, "pg_sat", "饱和度", 0.0, 200.0,
					0.1);
	obs_properties_add_float_slider(props, "pg_hue", "色相", 0.0, 100.0,
					0.1);
	obs_properties_add_float_slider(props, "pg_lummix", "亮度混合", 0.0,
					100.0, 0.1);
	return props;
}

static void cw_defaults(obs_data_t *settings)
{
	grade_params_t p;
	grade_defaults(&p);
	write_params(settings, &p);
}

struct obs_source_info obs_colorwheel_filter = {
	.id = "obs_colorwheel_filter",
	.type = OBS_SOURCE_TYPE_FILTER,
	.output_flags = OBS_SOURCE_VIDEO,
	.get_name = cw_name,
	.create = cw_create,
	.destroy = cw_destroy,
	.get_width = cw_width,
	.get_height = cw_height,
	.get_defaults = cw_defaults,
	.get_properties = cw_properties,
	.update = cw_update,
	.video_render = cw_render,
};

bool obs_module_load(void)
{
	obs_register_source(&obs_colorwheel_filter);
	return true;
}

void obs_module_unload(void)
{
}
