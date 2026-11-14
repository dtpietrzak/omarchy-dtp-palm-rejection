/* Per-contact palm classification. Pure logic, no device I/O, so it can be
 * unit tested with synthetic contacts. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

enum pr_state {
	PR_FINGER,    /* forwarded as a finger */
	PR_SOFT_PALM, /* held back as a palm, released if it moves on purpose */
	PR_PALM,      /* a palm until it lifts */
};

enum pr_reason {
	PR_R_NONE,
	PR_R_SIZE,         /* bigger than palm_size, at any time */
	PR_R_TYPING_SHAPE, /* landed while typing, too big or too long */
	PR_R_TYPING_EDGE,  /* landed while typing, in an edge zone */
	PR_R_TAP_GUARD,    /* landed right after a key press */
	PR_R_EDGE_SHAPE,   /* in an edge zone, too big or too long, not moving */
	PR_R_RESTING,      /* resting in an edge zone or big when a key went down */
	PR_R_RELEASED,     /* was a soft palm, moved like a finger */
};

struct pr_config {
	bool enabled;
	bool debug;
	int palm_size;           /* raw ABS_MT_TOUCH_MAJOR, always a palm above */
	int finger_size;         /* raw ABS_MT_TOUCH_MAJOR, bigger is suspicious */
	double max_ratio;        /* major / minor, longer is suspicious */
	int typing_ms;           /* how long after a key press counts as typing */
	int tap_guard_ms;        /* hold back every new touch this long after a key */
	int settle_ms;           /* keep checking a new touch's shape this long */
	double side_edge_mm;     /* left and right edge zones */
	double bottom_edge_mm;   /* edge zone along the front, away from the keyboard */
	double edge_release_mm;  /* travel that releases an edge soft palm */
	double center_release_mm;/* travel that releases a tap-guard soft palm */
	double rest_mm;          /* less travel than this counts as resting */
	char touchpad_name[128]; /* empty: detect */
	char keyboard_name[128]; /* empty: detect */
};

/* Touchpad geometry in device units; res is units per mm. */
struct pr_geom {
	int min_x, max_x, min_y, max_y;
	double res_x, res_y;
};

struct pr_contact {
	bool active;
	bool fresh; /* began in the current frame */
	int x, y, major, minor, wmajor, wminor, pressure;
	int x0, y0;
	uint64_t t0_us;
	bool typing_at_start;
	bool guard_at_start;
	bool edge_at_start;
	double travel_mm;
	int max_size;
	double max_ratio;
	int max_pressure;
	enum pr_state state;
	enum pr_reason reason;
};

void pr_config_defaults(struct pr_config *cfg);

/* Start tracking a contact. Call after its first frame's axes are set. */
void pr_contact_begin(struct pr_contact *c, uint64_t now_us);

/* Decide the contact's state for this frame. last_key_us is the time of the
 * last typing key press (0: none yet); key_this_frame is true when a typing
 * key went down since the previous frame. */
void pr_classify(const struct pr_config *cfg, const struct pr_geom *geom,
		 struct pr_contact *c, uint64_t now_us, uint64_t last_key_us,
		 bool key_this_frame);

/* Touch size: the bigger ellipse axis. Ratio: bigger over smaller, >= 1. */
int pr_contact_size(const struct pr_contact *c);
double pr_contact_ratio(const struct pr_contact *c);

bool pr_in_edge(const struct pr_config *cfg, const struct pr_geom *geom, int x, int y);

const char *pr_reason_name(enum pr_reason r);
const char *pr_state_name(enum pr_state s);
