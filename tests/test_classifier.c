/* Synthetic-contact tests for the classifier and config parser. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/classifier.h"
#include "../src/config.h"

static int failures;

#define CHECK(cond, ...)                                                   \
	do {                                                               \
		if (!(cond)) {                                             \
			failures++;                                        \
			fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
			fprintf(stderr, __VA_ARGS__);                      \
			fputc('\n', stderr);                               \
		}                                                          \
	} while (0)

/* A 128 x 78 mm pad, 10 units per mm, origin top-left, Y grows toward you. */
static const struct pr_geom G = { 0, 1280, 0, 780, 10.0, 10.0 };
static struct pr_config cfg;

#define MS(n) ((uint64_t)(n) * 1000)
#define T0 MS(100000)

struct sim {
	struct pr_contact c;
	uint64_t t;
};

static void land(struct sim *s, double x_mm, double y_mm, int major, int minor, uint64_t t,
		 uint64_t last_key)
{
	memset(&s->c, 0, sizeof(s->c));
	s->c.x = (int)(x_mm * G.res_x);
	s->c.y = (int)(y_mm * G.res_y);
	s->c.major = major;
	s->c.minor = minor;
	s->t = t;
	pr_contact_begin(&s->c, t);
	pr_classify(&cfg, &G, &s->c, t, last_key, false);
}

/* Like land(), with the approach width the trackpad reports. */
static void land_w(struct sim *s, double x_mm, double y_mm, int major, int minor, int wmajor,
		   uint64_t t, uint64_t last_key)
{
	memset(&s->c, 0, sizeof(s->c));
	s->c.x = (int)(x_mm * G.res_x);
	s->c.y = (int)(y_mm * G.res_y);
	s->c.major = major;
	s->c.minor = minor;
	s->c.wmajor = wmajor;
	s->t = t;
	pr_contact_begin(&s->c, t);
	pr_classify(&cfg, &G, &s->c, t, last_key, false);
}

static void step(struct sim *s, double dx_mm, double dy_mm, int major, int minor,
		 uint64_t dt, uint64_t last_key, bool key)
{
	s->c.x += (int)(dx_mm * G.res_x);
	s->c.y += (int)(dy_mm * G.res_y);
	s->c.major = major;
	s->c.minor = minor;
	s->t += dt;
	pr_classify(&cfg, &G, &s->c, s->t, last_key, key);
}

static void test_idle_fingertip_is_finger(void)
{
	struct sim s;

	land(&s, 64, 40, 500, 450, T0, 0);
	CHECK(s.c.state == PR_FINGER, "idle fingertip: %s", pr_state_name(s.c.state));
}

static void test_huge_contact_is_palm_anytime(void)
{
	struct sim s;

	land(&s, 64, 40, 500, 450, T0, 0);
	step(&s, 0, 0, 1700, 1200, MS(8), 0, false);
	CHECK(s.c.state == PR_PALM && s.c.reason == PR_R_SIZE, "huge: %s/%s",
	      pr_state_name(s.c.state), pr_reason_name(s.c.reason));
	step(&s, 0, 0, 500, 450, MS(8), 0, false);
	CHECK(s.c.state == PR_PALM, "size palm must stick until lift");
}

static void test_typing_big_contact_is_palm(void)
{
	struct sim s;

	land(&s, 64, 40, 1200, 800, T0, T0 - MS(100));
	CHECK(s.c.state == PR_PALM && s.c.reason == PR_R_TYPING_SHAPE, "typing big: %s/%s",
	      pr_state_name(s.c.state), pr_reason_name(s.c.reason));
}

static void test_typing_palm_growing_while_landing(void)
{
	struct sim s;

	/* Lands small, grows to palm-like within the settle window. */
	land(&s, 64, 40, 400, 380, T0, T0 - MS(300));
	CHECK(s.c.state == PR_FINGER, "lands small after tap guard: %s", pr_state_name(s.c.state));
	step(&s, 0, 0, 1100, 600, MS(30), T0 - MS(300), false);
	CHECK(s.c.state == PR_PALM, "grew while landing: %s", pr_state_name(s.c.state));
}

static void test_typing_fingertip_in_center_moves_cursor(void)
{
	struct sim s;
	uint64_t key = T0 - MS(100);

	/* Right after a key: held back, but released after a little travel,
	 * which is the difference from all-or-nothing disable-while-typing. */
	land(&s, 64, 40, 500, 450, T0, key);
	CHECK(s.c.state == PR_SOFT_PALM && s.c.reason == PR_R_TAP_GUARD, "tap guard: %s/%s",
	      pr_state_name(s.c.state), pr_reason_name(s.c.reason));
	step(&s, 1.0, 0, 500, 450, MS(8), key, false);
	CHECK(s.c.state == PR_SOFT_PALM, "1 mm is not enough yet");
	step(&s, 1.0, 0, 500, 450, MS(8), key, false);
	CHECK(s.c.state == PR_FINGER && s.c.reason == PR_R_RELEASED, "released: %s",
	      pr_state_name(s.c.state));
}

static void test_typing_fingertip_after_guard_is_finger(void)
{
	struct sim s;

	land(&s, 64, 40, 500, 450, T0, T0 - MS(300));
	CHECK(s.c.state == PR_FINGER, "300 ms after a key, centre fingertip: %s",
	      pr_state_name(s.c.state));
}

static void test_typing_edge_touch_held_until_deliberate(void)
{
	struct sim s;
	uint64_t key = T0 - MS(50);

	land(&s, 5, 40, 500, 450, T0, key);
	CHECK(s.c.state == PR_SOFT_PALM && s.c.reason == PR_R_TYPING_EDGE, "edge: %s/%s",
	      pr_state_name(s.c.state), pr_reason_name(s.c.reason));
	/* Moving while still typing does not release it. */
	step(&s, 10, 0, 500, 450, MS(10), key, false);
	CHECK(s.c.state == PR_SOFT_PALM, "still typing: %s", pr_state_name(s.c.state));
	/* Once typing has stopped, a long move does. */
	step(&s, 2, 0, 500, 450, MS(600), key, false);
	CHECK(s.c.state == PR_FINGER, "typing stopped and moved: %s", pr_state_name(s.c.state));
}

static void test_idle_edge_palm_shape(void)
{
	struct sim s;

	/* Hand resting on the pad while reading, no typing. */
	land(&s, 4, 6, 1100, 700, T0, 0);
	CHECK(s.c.state == PR_PALM && s.c.reason == PR_R_EDGE_SHAPE, "idle edge palm: %s/%s",
	      pr_state_name(s.c.state), pr_reason_name(s.c.reason));
}

static void test_idle_edge_fingertip_allowed(void)
{
	struct sim s;

	land(&s, 4, 40, 450, 420, T0, 0);
	CHECK(s.c.state == PR_FINGER, "idle edge fingertip: %s", pr_state_name(s.c.state));
}

static void test_four_finger_swipe_from_right_edge(void)
{
	struct sim s;

	/* From a real log: the fourth finger of a swipe landed at the right
	 * edge reading 318 x 676 and was taken for a palm, turning a four
	 * finger swipe into a three finger one. */
	land(&s, 127.6, 20.2, 318, 676, T0, 0);
	CHECK(s.c.state == PR_FINGER, "long fingertip at the edge: %s/%s",
	      pr_state_name(s.c.state), pr_reason_name(s.c.reason));
	step(&s, -0.3, 0, 300, 650, MS(7), 0, false);
	step(&s, -10, 0, 200, 300, MS(200), 0, false);
	CHECK(s.c.state == PR_FINGER, "and stays a finger as it swipes: %s/%s",
	      pr_state_name(s.c.state), pr_reason_name(s.c.reason));
}

static void test_resting_edge_contact_becomes_palm_on_key(void)
{
	struct sim s;

	land(&s, 6, 30, 600, 550, T0, 0);
	CHECK(s.c.state == PR_FINGER, "resting edge contact before typing");
	step(&s, 0.2, 0, 600, 550, MS(500), T0 + MS(500), true);
	CHECK(s.c.state == PR_PALM && s.c.reason == PR_R_RESTING, "after a key: %s/%s",
	      pr_state_name(s.c.state), pr_reason_name(s.c.reason));
}

static void test_moving_finger_trusted_when_typing(void)
{
	struct sim s;

	land(&s, 6, 30, 600, 550, T0, 0);
	step(&s, 5, 0, 600, 550, MS(50), 0, false);
	step(&s, 0, 0, 600, 550, MS(10), T0 + MS(60), true);
	CHECK(s.c.state == PR_FINGER, "moved finger stays a finger: %s", pr_state_name(s.c.state));
}

static void test_center_resting_finger_not_palm_on_key(void)
{
	struct sim s;

	land(&s, 64, 40, 500, 450, T0, 0);
	step(&s, 0, 0, 500, 450, MS(300), T0 + MS(300), true);
	CHECK(s.c.state == PR_FINGER, "centre resting fingertip: %s", pr_state_name(s.c.state));
}

static void test_edge_zones_are_sides_and_front(void)
{
	/* Pad is 128 x 78 mm; Y grows toward you. */
	CHECK(pr_in_edge(&cfg, &G, 50, 400), "left side is an edge");
	CHECK(pr_in_edge(&cfg, &G, 1230, 400), "right side is an edge");
	CHECK(pr_in_edge(&cfg, &G, 640, 740), "front edge is an edge");
	CHECK(!pr_in_edge(&cfg, &G, 640, 30), "keyboard edge is not an edge");
	CHECK(!pr_in_edge(&cfg, &G, 640, 400), "middle is not an edge");
}

static void test_typing_near_keyboard_edge_is_finger(void)
{
	struct sim s;

	/* Reaching up to point right after typing, past the tap guard. */
	land(&s, 64, 3, 500, 450, T0, T0 - MS(300));
	CHECK(s.c.state == PR_FINGER, "fingertip at the keyboard edge: %s", pr_state_name(s.c.state));
}

static void test_typing_front_edge_palm(void)
{
	struct sim s;

	land(&s, 64, 75, 500, 450, T0, T0 - MS(300));
	CHECK(s.c.state == PR_SOFT_PALM && s.c.reason == PR_R_TYPING_EDGE, "front edge while typing: %s/%s",
	      pr_state_name(s.c.state), pr_reason_name(s.c.reason));
}

static void test_minor_bigger_than_major(void)
{
	struct sim s;

	/* Seen on the M2 trackpad: minor reported bigger than major. */
	land(&s, 64, 40, 70, 94, T0, 0);
	CHECK(s.c.state == PR_FINGER, "small swapped axes: %s", pr_state_name(s.c.state));
	step(&s, 0, 0, 600, 1700, MS(8), 0, false);
	CHECK(s.c.state == PR_PALM && s.c.reason == PR_R_SIZE, "big minor axis is a palm: %s/%s",
	      pr_state_name(s.c.state), pr_reason_name(s.c.reason));
	CHECK(s.c.max_size == 1700, "max size uses the bigger axis: %d", s.c.max_size);

	land(&s, 5, 40, 400, 1100, T0, 0);
	CHECK(s.c.state == PR_PALM && s.c.reason == PR_R_EDGE_SHAPE, "big minor axis at the edge: %s/%s",
	      pr_state_name(s.c.state), pr_reason_name(s.c.reason));
}

static void test_typing_long_fingertip_in_middle_is_finger(void)
{
	struct sim s;
	uint64_t key = T0 - MS(100);

	/* From a real log: a finger put down right after typing to move the
	 * pointer read 318 x 612 (ratio 1.9) for its first frames. */
	land(&s, 66.9, 34.2, 188, 446, T0, key);
	CHECK(s.c.state == PR_SOFT_PALM && s.c.reason == PR_R_TAP_GUARD, "lands tap-guarded: %s/%s",
	      pr_state_name(s.c.state), pr_reason_name(s.c.reason));
	step(&s, 0.4, 0, 318, 612, MS(8), key, false);
	CHECK(s.c.state == PR_SOFT_PALM, "long fingertip is not a palm: %s/%s",
	      pr_state_name(s.c.state), pr_reason_name(s.c.reason));
	step(&s, 2.0, 0, 450, 500, MS(16), key, false);
	CHECK(s.c.state == PR_FINGER, "moves the pointer: %s", pr_state_name(s.c.state));
}

static void test_typing_palm_growing_at_side(void)
{
	struct sim s;
	uint64_t key = T0 - MS(100);

	/* From a real log: palms land small at the side and pass 1000 within
	 * 40-300 ms. */
	land(&s, 1.8, 33.8, 68, 60, T0, key);
	CHECK(s.c.state == PR_SOFT_PALM && s.c.reason == PR_R_TYPING_EDGE, "lands: %s/%s",
	      pr_state_name(s.c.state), pr_reason_name(s.c.reason));
	step(&s, 0.6, 0, 1030, 718, MS(40), key, false);
	CHECK(s.c.state == PR_PALM && s.c.reason == PR_R_TYPING_SHAPE, "grows: %s/%s",
	      pr_state_name(s.c.state), pr_reason_name(s.c.reason));
}

static void test_gaming_flick_from_edge_while_typing(void)
{
	struct sim s;
	uint64_t key = T0 - MS(100);

	/* From a real log: holding keys in a game and flicking a finger in
	 * from the left edge. Narrow at landing (1676), so not a palm; the tap
	 * guard lets it go after 1.5 mm. */
	land_w(&s, 7.8, 33.6, 202, 404, 1676, T0, key);
	CHECK(s.c.reason != PR_R_TYPING_EDGE, "narrow edge touch is not an edge palm: %s/%s",
	      pr_state_name(s.c.state), pr_reason_name(s.c.reason));
	step(&s, 2, 0, 300, 450, MS(10), T0, true);
	CHECK(s.c.state == PR_FINGER, "flick moves the pointer: %s/%s",
	      pr_state_name(s.c.state), pr_reason_name(s.c.reason));
	step(&s, 15, 0, 650, 670, MS(200), T0 + MS(200), true);
	CHECK(s.c.state == PR_FINGER, "and keeps moving it: %s", pr_state_name(s.c.state));
}

static void test_gaming_finger_held_still_at_edge(void)
{
	struct sim s;

	/* Aiming: a narrow finger resting at the edge while keys go down. */
	land_w(&s, 125, 30, 500, 600, 1600, T0, 0);
	CHECK(s.c.state == PR_FINGER, "lands: %s", pr_state_name(s.c.state));
	step(&s, 0.5, 0, 600, 650, MS(400), T0 + MS(400), true);
	step(&s, 0.3, 0, 600, 650, MS(100), T0 + MS(500), true);
	CHECK(s.c.state == PR_FINGER, "narrow resting finger survives key presses: %s/%s",
	      pr_state_name(s.c.state), pr_reason_name(s.c.reason));
}

static void test_wide_edge_touch_while_typing_held(void)
{
	struct sim s;
	uint64_t key = T0 - MS(100);

	/* From a real log: a palm at the left edge, width 2260 at landing. */
	land_w(&s, 0.4, 28.2, 54, 66, 2260, T0, key);
	CHECK(s.c.state == PR_SOFT_PALM && s.c.reason == PR_R_TYPING_EDGE, "wide edge touch: %s/%s",
	      pr_state_name(s.c.state), pr_reason_name(s.c.reason));
	step(&s, 1.7, 0, 400, 504, MS(160), key, false);
	CHECK(s.c.state == PR_SOFT_PALM, "small drift stays held: %s", pr_state_name(s.c.state));
}

static void test_wide_edge_touch_released_while_typing(void)
{
	struct sim s;
	uint64_t key = T0 - MS(100);

	land_w(&s, 3, 30, 60, 60, 2200, T0, key);
	CHECK(s.c.state == PR_SOFT_PALM, "held: %s", pr_state_name(s.c.state));
	step(&s, 9, 0, 600, 650, MS(100), T0 + MS(100), true);
	CHECK(s.c.state == PR_SOFT_PALM, "not inside the landing window: %s", pr_state_name(s.c.state));
	step(&s, 1, 0, 600, 650, MS(250), T0 + MS(350), true);
	CHECK(s.c.state == PR_FINGER && s.c.reason == PR_R_RELEASED,
	      "fingertip-sized and moved 8 mm, released while typing: %s/%s",
	      pr_state_name(s.c.state), pr_reason_name(s.c.reason));
}

static void test_narrow_palm_growing_while_moving(void)
{
	struct sim s;
	uint64_t key = T0 - MS(300);

	/* A palm that slips under palm_width still has to grow past
	 * finger_size within the landing window, and gets caught. */
	land_w(&s, 4, 35, 60, 60, 1900, T0, key);
	step(&s, 3, 0, 400, 400, MS(80), key, false);
	CHECK(s.c.state == PR_FINGER, "looks like a finger at first: %s", pr_state_name(s.c.state));
	step(&s, 0.5, 0, 1100, 700, MS(150), key, false);
	CHECK(s.c.state == PR_PALM, "caught when it grows: %s/%s", pr_state_name(s.c.state),
	      pr_reason_name(s.c.reason));
}

static void test_shortcut_window_expired(void)
{
	struct sim s;

	land(&s, 5, 40, 500, 450, T0, T0 - MS(2000));
	CHECK(s.c.state == PR_FINGER, "edge fingertip 2 s after typing: %s", pr_state_name(s.c.state));
}

static void test_config_parse(void)
{
	const char *text = "# comment\n"
			   "enabled = no\n"
			   "  finger_size=1200  \n"
			   "max_ratio = 2.5\n"  /* removed setting: an unknown key now */
			   "typing_ms = banana\n"
			   "nope = 1\n"
			   "touchpad_name = Apple MTP multi-touch\n"
			   "side_edge_mm = 500\n";
	FILE *in = fmemopen((void *)text, strlen(text), "r");
	FILE *err = fopen("/dev/null", "w");
	struct pr_config c;
	int problems;

	pr_config_defaults(&c);
	problems = pr_config_parse(in, &c, err);
	fclose(in);
	fclose(err);
	CHECK(problems == 4, "problems: %d", problems);
	CHECK(!c.enabled, "enabled");
	CHECK(c.finger_size == 1200, "finger_size %d", c.finger_size);
	CHECK(c.typing_ms == 500, "bad value keeps default");
	CHECK(c.side_edge_mm == 12.0, "out of range keeps default");
	CHECK(!strcmp(c.touchpad_name, "Apple MTP multi-touch"), "name '%s'", c.touchpad_name);
}

static void test_shipped_config_is_valid(void)
{
	struct pr_config c, d;
	FILE *err = fopen("/dev/null", "w");
	int problems = pr_config_load("config/palm-rejection.conf", &c, err);

	fclose(err);
	pr_config_defaults(&d);
	CHECK(problems == 0, "shipped config problems: %d", problems);
	CHECK(c.palm_size == d.palm_size && c.finger_size == d.finger_size &&
		      c.typing_ms == d.typing_ms && c.side_edge_mm == d.side_edge_mm &&
		      c.enabled == d.enabled && c.debug == d.debug,
	      "shipped config should match the built-in defaults");
}

int main(void)
{
	pr_config_defaults(&cfg);

	test_idle_fingertip_is_finger();
	test_huge_contact_is_palm_anytime();
	test_typing_big_contact_is_palm();
	test_typing_palm_growing_while_landing();
	test_typing_fingertip_in_center_moves_cursor();
	test_typing_fingertip_after_guard_is_finger();
	test_typing_edge_touch_held_until_deliberate();
	test_idle_edge_palm_shape();
	test_idle_edge_fingertip_allowed();
	test_four_finger_swipe_from_right_edge();
	test_resting_edge_contact_becomes_palm_on_key();
	test_moving_finger_trusted_when_typing();
	test_center_resting_finger_not_palm_on_key();
	test_edge_zones_are_sides_and_front();
	test_typing_near_keyboard_edge_is_finger();
	test_typing_front_edge_palm();
	test_minor_bigger_than_major();
	test_typing_long_fingertip_in_middle_is_finger();
	test_typing_palm_growing_at_side();
	test_gaming_flick_from_edge_while_typing();
	test_gaming_finger_held_still_at_edge();
	test_wide_edge_touch_while_typing_held();
	test_wide_edge_touch_released_while_typing();
	test_narrow_palm_growing_while_moving();
	test_shortcut_window_expired();
	test_config_parse();
	test_shipped_config_is_valid();

	if (failures) {
		fprintf(stderr, "%d failure(s)\n", failures);
		return 1;
	}
	printf("all classifier tests passed\n");
	return 0;
}
