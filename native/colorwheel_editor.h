/* C ABI between obs-colorwheel.c (filter) and the editor UI.
 *
 * Deliberately shaped like obs-curves/native/curves_editor.h: same host
 * struct idea, same open/on-filter-destroyed pair, same live-query
 * callbacks.  When the two plugins are merged later, this becomes a second
 * tab in the existing editor and nothing else changes.
 */
#ifndef OBS_COLORWHEEL_EDITOR_H
#define OBS_COLORWHEEL_EDITOR_H

#include "grade_engine.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
	/* Opaque filter pointer; only ever passed back to the callbacks. */
	void *filter;

	/* Live working copy the editor mutates in place. */
	grade_params_t *params;

	/* Target source of this filter (UTF-8, may be NULL). */
	const char *source_name;
	int target_width;
	int target_height;

	/* Live queries: the target may not be attached yet when the editor is
	 * opened from a filter-add callback. */
	const char *(*get_source_name)(void *filter);
	void (*get_target_size)(void *filter, int *w, int *h);

	/* Commit params -> OBS settings -> obs_source_update(). */
	void (*apply)(void *filter);
	/* Params changed but not committed yet; just means "redraw". */
	void (*mark_dirty)(void *filter);

	/* Where to remember the window rect / always-on-top state (UTF-8).
	 * The editor copies it during open, so the caller may free it after. */
	const char *config_path;
} colorwheel_editor_host;

/* Open (or focus) the editor window.  Copies the host struct. */
void obs_colorwheel_editor_open(const colorwheel_editor_host *host);

/* Called by the filter when it is being destroyed.  Returns false when the
 * editor still holds a reference, in which case the filter must leak rather
 * than free memory a UI thread may still paint (same rule as obs-curves). */
bool obs_colorwheel_editor_on_filter_destroyed(void *filter);

#ifdef __cplusplus
}
#endif

#endif
