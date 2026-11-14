#include "classifier.h"

#include <math.h>
#include <string.h>

void pr_config_defaults(struct pr_config *cfg)
{
	memset(cfg, 0, sizeof(*cfg));
	cfg->enabled = true;
	cfg->debug = false;
	cfg->palm_size = 1600;
	cfg->finger_size = 1000;
	cfg->max_ratio = 1.8;
	cfg->typing_ms = 500;
	cfg->tap_guard_ms = 250;
	cfg->settle_ms = 60;
	cfg->side_edge_mm = 12.0;
	cfg->bottom_edge_mm = 10.0;
	cfg->edge_release_mm = 8.0;
	cfg->center_release_mm = 1.5;
	cfg->rest_mm = 2.0;
}

bool pr_in_edge(const struct pr_config *cfg, const struct pr_geom *g, int x, int y)
{
	double left = (x - g->min_x) / g->res_x;
	double right = (g->max_x - x) / g->res_x;
	/* evdev Y grows toward you, so max_y is the front edge. Palms rest
	 * on the sides and the front while typing; the keyboard edge is
	 * left alone because fingers reach up there to point. */
	double bottom = (g->max_y - y) / g->res_y;

	return left < cfg->side_edge_mm || right < cfg->side_edge_mm ||
	       bottom < cfg->bottom_edge_mm;
}

/* The Apple driver sometimes reports minor > major, especially while a
 * finger lifts, so use whichever axis is bigger. */
int pr_contact_size(const struct pr_contact *c)
{
	return c->major > c->minor ? c->major : c->minor;
}

double pr_contact_ratio(const struct pr_contact *c)
{
	int big = pr_contact_size(c);
	int small = c->major > c->minor ? c->minor : c->major;

	if (small <= 0)
		return 1.0;
	return (double)big / (double)small;
}

/* Too big, or long like the side of a hand. Tiny contacts have noisy
 * ellipses, so the length test only counts from half a finger up.
 *
 * The length test only applies in the edge zones: on the M2 trackpad a
 * fingertip landing in the middle often reads twice as long as it is wide
 * for its first few milliseconds, and palms can't land in the middle while
 * your hands are on the keyboard anyway. */
static bool shape_bad(const struct pr_config *cfg, const struct pr_contact *c, bool in_edge)
{
	int size = pr_contact_size(c);

	if (size > cfg->finger_size)
		return true;
	return in_edge && size > cfg->finger_size / 2 && pr_contact_ratio(c) > cfg->max_ratio;
}

static void set_state(struct pr_contact *c, enum pr_state s, enum pr_reason r)
{
	c->state = s;
	c->reason = r;
}

void pr_contact_begin(struct pr_contact *c, uint64_t now_us)
{
	c->active = true;
	c->fresh = true;
	c->x0 = c->x;
	c->y0 = c->y;
	c->t0_us = now_us;
	c->typing_at_start = false;
	c->guard_at_start = false;
	c->edge_at_start = false;
	c->travel_mm = 0.0;
	c->max_size = 0;
	c->max_ratio = 0.0;
	c->max_pressure = 0;
	c->state = PR_FINGER;
	c->reason = PR_R_NONE;
}

void pr_classify(const struct pr_config *cfg, const struct pr_geom *g,
		 struct pr_contact *c, uint64_t now_us, uint64_t last_key_us,
		 bool key_this_frame)
{
	uint64_t since_key = last_key_us && now_us >= last_key_us ? now_us - last_key_us
								  : UINT64_MAX;
	bool typing_now = since_key < (uint64_t)cfg->typing_ms * 1000;
	double dx = (c->x - c->x0) / g->res_x;
	double dy = (c->y - c->y0) / g->res_y;
	double dist = sqrt(dx * dx + dy * dy);
	uint64_t age_ms = (now_us - c->t0_us) / 1000;
	bool bad;
	double ratio = pr_contact_ratio(c);

	if (dist > c->travel_mm)
		c->travel_mm = dist;
	if (pr_contact_size(c) > c->max_size)
		c->max_size = pr_contact_size(c);
	if (ratio > c->max_ratio)
		c->max_ratio = ratio;
	if (c->pressure > c->max_pressure)
		c->max_pressure = c->pressure;

	if (c->fresh)
		c->edge_at_start = pr_in_edge(cfg, g, c->x, c->y);
	bad = shape_bad(cfg, c, c->edge_at_start);

	if (pr_contact_size(c) >= cfg->palm_size) {
		set_state(c, PR_PALM, PR_R_SIZE);
		return;
	}
	if (c->state == PR_PALM)
		return;

	if (c->fresh) {
		c->fresh = false;
		c->typing_at_start = typing_now;
		c->guard_at_start = since_key < (uint64_t)cfg->tap_guard_ms * 1000;

		if (c->typing_at_start && bad)
			set_state(c, PR_PALM, PR_R_TYPING_SHAPE);
		else if (c->typing_at_start && c->edge_at_start)
			set_state(c, PR_SOFT_PALM, PR_R_TYPING_EDGE);
		else if (c->guard_at_start)
			set_state(c, PR_SOFT_PALM, PR_R_TAP_GUARD);
		else if (c->edge_at_start && bad)
			set_state(c, PR_PALM, PR_R_EDGE_SHAPE);
		return;
	}

	/* A touch is still landing for the first few frames and grows while
	 * it does, so check its shape again. */
	if (age_ms <= (uint64_t)cfg->settle_ms && c->typing_at_start && bad) {
		set_state(c, PR_PALM, PR_R_TYPING_SHAPE);
		return;
	}

	switch (c->state) {
	case PR_SOFT_PALM:
		if (bad && c->typing_at_start) {
			set_state(c, PR_PALM, PR_R_TYPING_SHAPE);
		} else if (bad && c->edge_at_start) {
			set_state(c, PR_PALM, PR_R_EDGE_SHAPE);
		} else if (c->reason == PR_R_TAP_GUARD) {
			if (c->travel_mm >= cfg->center_release_mm)
				set_state(c, PR_FINGER, PR_R_RELEASED);
		} else if (c->reason == PR_R_TYPING_EDGE) {
			if (!typing_now && c->travel_mm >= cfg->edge_release_mm)
				set_state(c, PR_FINGER, PR_R_RELEASED);
		}
		break;
	case PR_FINGER:
		/* A finger that has moved is trusted from then on. A resting
		 * one is a palm once it looks like one, or once you type
		 * with it sitting in an edge zone. */
		if (c->travel_mm >= cfg->rest_mm)
			break;
		if (c->edge_at_start && bad)
			set_state(c, PR_PALM, PR_R_EDGE_SHAPE);
		else if (key_this_frame && (c->edge_at_start || bad))
			set_state(c, PR_PALM, PR_R_RESTING);
		break;
	case PR_PALM:
		break;
	}
}

const char *pr_reason_name(enum pr_reason r)
{
	switch (r) {
	case PR_R_NONE: return "-";
	case PR_R_SIZE: return "size";
	case PR_R_TYPING_SHAPE: return "typing-shape";
	case PR_R_TYPING_EDGE: return "typing-edge";
	case PR_R_TAP_GUARD: return "tap-guard";
	case PR_R_EDGE_SHAPE: return "edge-shape";
	case PR_R_RESTING: return "resting";
	case PR_R_RELEASED: return "released";
	}
	return "?";
}

const char *pr_state_name(enum pr_state s)
{
	switch (s) {
	case PR_FINGER: return "finger";
	case PR_SOFT_PALM: return "soft-palm";
	case PR_PALM: return "palm";
	}
	return "?";
}
