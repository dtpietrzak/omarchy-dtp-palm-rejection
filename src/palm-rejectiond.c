/* omarchy-dtp-palm-rejectiond: Apple-style palm rejection for Linux trackpads.
 *
 * Grabs the built-in trackpad, copies every event to a virtual twin made with
 * uinput, and tags contacts that look like palms with
 * ABS_MT_TOOL_TYPE = MT_TOOL_PALM. libinput already ignores contacts tagged
 * that way, so gestures, scrolling and taps keep working with the fingers
 * that are left. The built-in keyboard is read only for the time of the last
 * typing key press; key codes are never stored or logged.
 *
 * When the process exits for any reason the kernel drops the grab and the
 * real trackpad works as before. */
#define _GNU_SOURCE

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <libgen.h>
#include <limits.h>
#include <linux/input.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/ioctl.h>
#include <sys/signalfd.h>
#include <time.h>
#include <unistd.h>

#include <libevdev/libevdev-uinput.h>
#include <libevdev/libevdev.h>

#include "classifier.h"
#include "config.h"

#define DEFAULT_CONF "/etc/omarchy-dtp-palm-rejection/palm-rejection.conf"
#define CLONE_SUFFIX " (palm rejection)"
#define MAX_KEYBOARDS 4
#define MAX_SLOTS 32

struct slot {
	struct pr_contact c;
	bool begin_pending;
	bool end_pending;
	enum pr_state logged_state;
};

static struct {
	const char *conf_path;
	struct pr_config cfg;
	struct pr_geom geom;

	int tp_fd;
	struct libevdev *tp;
	char tp_name[256];
	struct libevdev_uinput *ui;
	bool grabbed;

	int kb_fd[MAX_KEYBOARDS];
	int nkb;
	int mods_held;

	int nslots;
	int cur_slot;
	struct slot slots[MAX_SLOTS];
	int out_tool[MAX_SLOTS];

	uint64_t last_key_us;
	bool key_this_frame;
} S;

static uint64_t ev_us(const struct input_event *e)
{
	return (uint64_t)e->input_event_sec * 1000000ull + (uint64_t)e->input_event_usec;
}

static uint64_t now_us(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000;
}

static bool has_suffix(const char *s, const char *suffix)
{
	size_t a = strlen(s), b = strlen(suffix);

	return a >= b && !strcmp(s + a - b, suffix);
}

static bool is_touchpad(struct libevdev *d)
{
	return libevdev_has_event_code(d, EV_ABS, ABS_MT_POSITION_X) &&
	       libevdev_has_event_code(d, EV_ABS, ABS_MT_POSITION_Y) &&
	       libevdev_has_event_code(d, EV_ABS, ABS_MT_TRACKING_ID) &&
	       libevdev_has_event_code(d, EV_ABS, ABS_MT_TOUCH_MAJOR) &&
	       libevdev_has_event_code(d, EV_KEY, BTN_TOOL_FINGER) &&
	       !libevdev_has_property(d, INPUT_PROP_DIRECT) &&
	       !has_suffix(libevdev_get_name(d), CLONE_SUFFIX);
}

static bool is_keyboard(struct libevdev *d)
{
	return libevdev_has_event_code(d, EV_KEY, KEY_A) &&
	       libevdev_has_event_code(d, EV_KEY, KEY_Z) &&
	       libevdev_has_event_code(d, EV_KEY, KEY_SPACE) &&
	       libevdev_has_event_code(d, EV_KEY, KEY_ENTER);
}

static bool is_external_bus(int bus)
{
	return bus == BUS_USB || bus == BUS_BLUETOOTH || bus == BUS_VIRTUAL;
}

typedef bool (*visit_fn)(const char *path, int fd, struct libevdev *d, void *data);

static int nodes_denied;

/* Open each /dev/input/event* node in order. The visitor keeps the fd and
 * device by returning true; otherwise they are closed. */
static void for_each_event_node(visit_fn visit, void *data)
{
	struct dirent **names;
	int n = scandir("/dev/input", &names, NULL, versionsort);

	for (int i = 0; i < n; i++) {
		char path[PATH_MAX];
		struct libevdev *d = NULL;
		int fd;

		if (strncmp(names[i]->d_name, "event", 5)) {
			free(names[i]);
			continue;
		}
		snprintf(path, sizeof(path), "/dev/input/%s", names[i]->d_name);
		free(names[i]);
		fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
		if (fd < 0) {
			if (errno == EACCES || errno == EPERM)
				nodes_denied++;
			continue;
		}
		if (libevdev_new_from_fd(fd, &d) < 0) {
			close(fd);
			continue;
		}
		if (!visit(path, fd, d, data)) {
			libevdev_free(d);
			close(fd);
		}
	}
	if (n > 0)
		free(names);
}

static bool find_touchpad(const char *path, int fd, struct libevdev *d, void *data)
{
	(void)data;
	if (S.tp || !is_touchpad(d))
		return false;
	if (S.cfg.touchpad_name[0]) {
		if (strcmp(libevdev_get_name(d), S.cfg.touchpad_name))
			return false;
	} else if (is_external_bus(libevdev_get_id_bustype(d))) {
		return false;
	}
	S.tp = d;
	S.tp_fd = fd;
	snprintf(S.tp_name, sizeof(S.tp_name), "%s", libevdev_get_name(d));
	printf("trackpad: %s (%s)\n", S.tp_name, path);
	return true;
}

static bool find_keyboard(const char *path, int fd, struct libevdev *d, void *data)
{
	(void)data;
	if (S.nkb == MAX_KEYBOARDS || !is_keyboard(d))
		return false;
	if (S.cfg.keyboard_name[0]) {
		if (strcmp(libevdev_get_name(d), S.cfg.keyboard_name))
			return false;
	} else if (libevdev_get_id_bustype(d) != libevdev_get_id_bustype(S.tp) ||
		   libevdev_get_id_vendor(d) != libevdev_get_id_vendor(S.tp)) {
		/* The built-in keyboard sits on the trackpad's bus with its
		 * vendor. With an external keyboard your palms are not on
		 * the laptop. */
		return false;
	}
	printf("keyboard: %s (%s)\n", libevdev_get_name(d), path);
	/* Only the raw fd is needed for key press times. */
	libevdev_free(d);
	S.kb_fd[S.nkb++] = fd;
	return true;
}

static bool list_device(const char *path, int fd, struct libevdev *d, void *data)
{
	(void)fd;
	(void)data;
	printf("%-20s bus=%04x vendor=%04x product=%04x %s%s  %s\n", path,
	       libevdev_get_id_bustype(d), libevdev_get_id_vendor(d),
	       libevdev_get_id_product(d), is_touchpad(d) ? "[trackpad]" : "",
	       is_keyboard(d) ? "[keyboard]" : "", libevdev_get_name(d));
	return false;
}

static void setup_geometry(void)
{
	const struct input_absinfo *ax = libevdev_get_abs_info(S.tp, ABS_MT_POSITION_X);
	const struct input_absinfo *ay = libevdev_get_abs_info(S.tp, ABS_MT_POSITION_Y);

	S.geom.min_x = ax->minimum;
	S.geom.max_x = ax->maximum;
	S.geom.min_y = ay->minimum;
	S.geom.max_y = ay->maximum;
	/* Without a resolution, pretend the pad is 100 mm across. */
	S.geom.res_x = ax->resolution > 0 ? ax->resolution : (ax->maximum - ax->minimum) / 100.0;
	S.geom.res_y = ay->resolution > 0 ? ay->resolution : (ay->maximum - ay->minimum) / 100.0;
	if (S.geom.res_x <= 0)
		S.geom.res_x = 1;
	if (S.geom.res_y <= 0)
		S.geom.res_y = 1;
	printf("trackpad size: %.0f x %.0f mm\n", (S.geom.max_x - S.geom.min_x) / S.geom.res_x,
	       (S.geom.max_y - S.geom.min_y) / S.geom.res_y);
}

/* The virtual twin: the same device plus ABS_MT_TOOL_TYPE. The name keeps
 * the original as a prefix, so libinput's quirks for it still match. */
static int create_clone(void)
{
	struct libevdev *proto = libevdev_new();
	char name[300];
	int rc;

	snprintf(name, sizeof(name), "%s%s", S.tp_name, CLONE_SUFFIX);
	libevdev_set_name(proto, name);
	libevdev_set_id_bustype(proto, libevdev_get_id_bustype(S.tp));
	libevdev_set_id_vendor(proto, libevdev_get_id_vendor(S.tp));
	libevdev_set_id_product(proto, libevdev_get_id_product(S.tp));
	libevdev_set_id_version(proto, libevdev_get_id_version(S.tp));

	for (unsigned p = 0; p <= INPUT_PROP_MAX; p++)
		if (libevdev_has_property(S.tp, p))
			libevdev_enable_property(proto, p);

	for (unsigned type = EV_KEY; type < EV_MAX; type++) {
		int max;

		if (type == EV_REP || !libevdev_has_event_type(S.tp, type))
			continue;
		max = libevdev_event_type_get_max(type);
		for (int code = 0; code <= max; code++) {
			if (!libevdev_has_event_code(S.tp, type, code))
				continue;
			libevdev_enable_event_code(proto, type, code,
						   type == EV_ABS ? libevdev_get_abs_info(S.tp, code)
								  : NULL);
		}
	}

	struct input_absinfo tool = { .minimum = 0, .maximum = MT_TOOL_MAX };
	libevdev_enable_event_code(proto, EV_ABS, ABS_MT_TOOL_TYPE, &tool);

	rc = libevdev_uinput_create_from_device(proto, LIBEVDEV_UINPUT_OPEN_MANAGED, &S.ui);
	libevdev_free(proto);
	if (rc < 0) {
		fprintf(stderr, "cannot create the virtual trackpad: %s\n", strerror(-rc));
		return -1;
	}
	printf("virtual trackpad: %s (%s)\n", name, libevdev_uinput_get_devnode(S.ui));
	for (int s = 0; s < MAX_SLOTS; s++)
		S.out_tool[s] = MT_TOOL_FINGER;
	return 0;
}

static int active_contacts(void)
{
	int n = 0;

	for (int s = 0; s < S.nslots; s++)
		if (S.slots[s].c.active)
			n++;
	return n;
}

/* Grab and release only while nothing touches the pad, so no touch is left
 * half-seen by either device. */
static void update_grab(void)
{
	bool want = S.cfg.enabled;

	if (want == S.grabbed || active_contacts())
		return;
	if (libevdev_grab(S.tp, want ? LIBEVDEV_GRAB : LIBEVDEV_UNGRAB) < 0) {
		fprintf(stderr, "cannot %s the trackpad: %s\n", want ? "grab" : "release",
			strerror(errno));
		return;
	}
	S.grabbed = want;
	printf("palm rejection %s\n", want ? "on" : "off");
}

static void write_event(unsigned type, unsigned code, int value)
{
	if (S.grabbed)
		libevdev_uinput_write_event(S.ui, type, code, value);
}

static void log_contact(int s, const char *what)
{
	struct pr_contact *c = &S.slots[s].c;

	if (!S.cfg.debug)
		return;
	printf("%s slot=%d %s reason=%s major=%d minor=%d ratio=%.2f width=%d/%d "
	       "pressure=%d at=%.1f,%.1fmm start=%.1f,%.1fmm edge=%s typing=%s "
	       "travel=%.1fmm age=%llums max_major=%d\n",
	       what, s, pr_state_name(c->state), pr_reason_name(c->reason), c->major,
	       c->minor, c->minor > 0 ? (double)c->major / c->minor : 0.0, c->wmajor,
	       c->wminor, c->pressure, (c->x - S.geom.min_x) / S.geom.res_x,
	       (c->y - S.geom.min_y) / S.geom.res_y, (c->x0 - S.geom.min_x) / S.geom.res_x,
	       (c->y0 - S.geom.min_y) / S.geom.res_y, c->edge_at_start ? "yes" : "no",
	       c->typing_at_start ? "yes" : "no", c->travel_mm,
	       (unsigned long long)((now_us() - c->t0_us) / 1000), c->max_major);
	fflush(stdout);
}

static void end_frame(uint64_t t)
{
	bool injected = false;

	for (int s = 0; s < S.nslots; s++) {
		struct slot *sl = &S.slots[s];

		if (sl->end_pending) {
			if (sl->c.active)
				log_contact(s, "lift");
			sl->c.active = false;
			sl->end_pending = false;
		}
		if (sl->begin_pending) {
			pr_contact_begin(&sl->c, t);
			sl->begin_pending = false;
		}
		if (!sl->c.active)
			continue;
		pr_classify(&S.cfg, &S.geom, &sl->c, t, S.last_key_us, S.key_this_frame);
		if (sl->c.state != sl->logged_state) {
			log_contact(s, "change");
			sl->logged_state = sl->c.state;
		}
	}
	S.key_this_frame = false;

	if (S.grabbed) {
		for (int s = 0; s < S.nslots; s++) {
			int tool;

			if (!S.slots[s].c.active)
				continue;
			tool = S.slots[s].c.state == PR_FINGER ? MT_TOOL_FINGER : MT_TOOL_PALM;
			if (tool == S.out_tool[s])
				continue;
			write_event(EV_ABS, ABS_MT_SLOT, s);
			write_event(EV_ABS, ABS_MT_TOOL_TYPE, tool);
			S.out_tool[s] = tool;
			injected = true;
		}
		/* Readers track the current slot; put it back where the
		 * real device left it. */
		if (injected)
			write_event(EV_ABS, ABS_MT_SLOT, S.cur_slot);
		write_event(EV_SYN, SYN_REPORT, 0);
	}

	update_grab();
}

static void handle_tp_event(const struct input_event *e)
{
	struct slot *sl = S.cur_slot >= 0 && S.cur_slot < S.nslots ? &S.slots[S.cur_slot] : NULL;

	if (e->type == EV_SYN) {
		if (e->code == SYN_REPORT)
			end_frame(ev_us(e));
		/* SYN_DROPPED is handled by libevdev's sync. */
		return;
	}

	write_event(e->type, e->code, e->value);
	if (e->type != EV_ABS)
		return;

	switch (e->code) {
	case ABS_MT_SLOT:
		S.cur_slot = e->value;
		return;
	case ABS_MT_TRACKING_ID:
		if (!sl)
			return;
		if (e->value >= 0) {
			if (sl->c.active)
				log_contact(S.cur_slot, "lift");
			sl->c.active = false;
			sl->begin_pending = true;
			sl->end_pending = false;
			sl->logged_state = PR_FINGER;
		} else {
			sl->end_pending = true;
			sl->begin_pending = false;
		}
		return;
	}
	if (!sl)
		return;
	switch (e->code) {
	case ABS_MT_POSITION_X: sl->c.x = e->value; break;
	case ABS_MT_POSITION_Y: sl->c.y = e->value; break;
	case ABS_MT_TOUCH_MAJOR: sl->c.major = e->value; break;
	case ABS_MT_TOUCH_MINOR: sl->c.minor = e->value; break;
	case ABS_MT_WIDTH_MAJOR: sl->c.wmajor = e->value; break;
	case ABS_MT_WIDTH_MINOR: sl->c.wminor = e->value; break;
	case ABS_MT_PRESSURE: sl->c.pressure = e->value; break;
	}
}

/* Returns -1 when the trackpad went away. */
static int read_touchpad(void)
{
	struct input_event e;
	int rc;

	for (;;) {
		rc = libevdev_next_event(S.tp, LIBEVDEV_READ_FLAG_NORMAL, &e);
		if (rc == LIBEVDEV_READ_STATUS_SYNC) {
			while (rc == LIBEVDEV_READ_STATUS_SYNC) {
				handle_tp_event(&e);
				rc = libevdev_next_event(S.tp, LIBEVDEV_READ_FLAG_SYNC, &e);
			}
			continue;
		}
		if (rc == LIBEVDEV_READ_STATUS_SUCCESS) {
			handle_tp_event(&e);
			continue;
		}
		if (rc == -EAGAIN)
			return 0;
		fprintf(stderr, "trackpad read failed: %s\n", strerror(-rc));
		return -1;
	}
}

static bool is_modifier(unsigned code)
{
	switch (code) {
	case KEY_LEFTCTRL: case KEY_RIGHTCTRL:
	case KEY_LEFTALT: case KEY_RIGHTALT:
	case KEY_LEFTSHIFT: case KEY_RIGHTSHIFT:
	case KEY_LEFTMETA: case KEY_RIGHTMETA:
	case KEY_FN: case KEY_CAPSLOCK: case KEY_TAB: case KEY_COMPOSE:
		return true;
	}
	return false;
}

/* Same typing keys as libinput's disable-while-typing: not F keys, media
 * keys, the numpad or Escape, and nothing pressed with Ctrl, Alt or Super
 * held, so shortcuts never block the pointer. */
static void handle_key(const struct input_event *e)
{
	bool shortcut_mod;

	if (e->type != EV_KEY || e->value == 2)
		return;
	shortcut_mod = e->code == KEY_LEFTCTRL || e->code == KEY_RIGHTCTRL ||
		       e->code == KEY_LEFTALT || e->code == KEY_RIGHTALT ||
		       e->code == KEY_LEFTMETA || e->code == KEY_RIGHTMETA;
	if (shortcut_mod) {
		if (e->value == 1)
			S.mods_held++;
		else if (S.mods_held > 0)
			S.mods_held--;
		return;
	}
	if (e->value != 1 || is_modifier(e->code) || S.mods_held)
		return;
	if (e->code == KEY_ESC || e->code == KEY_KPASTERISK || e->code >= KEY_F1)
		return;
	S.last_key_us = ev_us(e);
	S.key_this_frame = true;
}

static void read_keyboard(int i)
{
	struct input_event ev[64];
	ssize_t n;

	while ((n = read(S.kb_fd[i], ev, sizeof(ev))) > 0)
		for (size_t k = 0; k < (size_t)n / sizeof(ev[0]); k++)
			handle_key(&ev[k]);
	if (n < 0 && errno == ENODEV) {
		close(S.kb_fd[i]);
		S.kb_fd[i] = -1;
		printf("keyboard went away\n");
	}
}

/* Returns 1 when the process should restart to pick up new device names. */
static int reload_config(void)
{
	struct pr_config next;
	int rc = pr_config_load(S.conf_path, &next, stderr);

	if (rc < 0) {
		fprintf(stderr, "cannot read %s: %s, keeping the old settings\n", S.conf_path,
			strerror(errno));
		return 0;
	}
	if (strcmp(next.touchpad_name, S.cfg.touchpad_name) ||
	    strcmp(next.keyboard_name, S.cfg.keyboard_name)) {
		printf("device names changed, restarting\n");
		return 1;
	}
	if (next.enabled != S.cfg.enabled || next.debug != S.cfg.debug)
		printf("settings: enabled=%s debug=%s\n", next.enabled ? "yes" : "no",
		       next.debug ? "yes" : "no");
	S.cfg = next;
	update_grab();
	return 0;
}

static void usage(FILE *out)
{
	fprintf(out,
		"Usage: omarchy-dtp-palm-rejectiond [--config FILE]\n"
		"       omarchy-dtp-palm-rejectiond --list\n"
		"       omarchy-dtp-palm-rejectiond --check-config FILE\n");
}

int main(int argc, char **argv)
{
	static const struct option opts[] = {
		{ "config", required_argument, NULL, 'c' },
		{ "list", no_argument, NULL, 'l' },
		{ "check-config", required_argument, NULL, 'k' },
		{ "help", no_argument, NULL, 'h' },
		{ 0 },
	};
	char conf_dir[PATH_MAX], conf_base[NAME_MAX + 1];
	struct pollfd pfd[3 + MAX_KEYBOARDS];
	int sfd, ifd, opt, rc;
	sigset_t mask;

	setvbuf(stdout, NULL, _IOLBF, 0);
	S.conf_path = DEFAULT_CONF;
	while ((opt = getopt_long(argc, argv, "c:lk:h", opts, NULL)) != -1) {
		switch (opt) {
		case 'c':
			S.conf_path = optarg;
			break;
		case 'l':
			for_each_event_node(list_device, NULL);
			if (nodes_denied)
				printf("%d input device(s) could not be opened; run with sudo\n",
				       nodes_denied);
			return 0;
		case 'k': {
			struct pr_config cfg;

			rc = pr_config_load(optarg, &cfg, stdout);
			if (rc < 0) {
				printf("cannot read %s: %s\n", optarg, strerror(errno));
				return 1;
			}
			return rc ? 1 : 0;
		}
		case 'h':
			usage(stdout);
			return 0;
		default:
			usage(stderr);
			return 2;
		}
	}

	rc = pr_config_load(S.conf_path, &S.cfg, stderr);
	if (rc < 0) {
		fprintf(stderr, "cannot read %s: %s, using defaults\n", S.conf_path,
			strerror(errno));
		pr_config_defaults(&S.cfg);
	}

	for_each_event_node(find_touchpad, NULL);
	if (!S.tp) {
		if (nodes_denied)
			fprintf(stderr, "%d input device(s) could not be opened; this needs root\n",
				nodes_denied);
		fprintf(stderr, "no built-in trackpad found; set touchpad_name in %s "
				"(see --list)\n", S.conf_path);
		return 1;
	}
	libevdev_set_clock_id(S.tp, CLOCK_MONOTONIC);
	for_each_event_node(find_keyboard, NULL);
	if (!S.nkb)
		printf("no built-in keyboard found; only size and edge checks will run\n");
	for (int i = 0; i < S.nkb; i++) {
		int clk = CLOCK_MONOTONIC;

		ioctl(S.kb_fd[i], EVIOCSCLOCKID, &clk);
	}

	setup_geometry();
	S.nslots = libevdev_get_num_slots(S.tp);
	if (S.nslots <= 0 || S.nslots > MAX_SLOTS) {
		fprintf(stderr, "unsupported slot count %d\n", S.nslots);
		return 1;
	}
	S.cur_slot = libevdev_get_current_slot(S.tp);
	for (int s = 0; s < S.nslots; s++) {
		/* Touches already down when we start are left alone; they
		 * only delay the grab until they lift. */
		S.slots[s].c.active = libevdev_get_slot_value(S.tp, s, ABS_MT_TRACKING_ID) >= 0;
		S.slots[s].c.state = PR_FINGER;
	}
	if (create_clone() < 0)
		return 1;

	sigemptyset(&mask);
	sigaddset(&mask, SIGTERM);
	sigaddset(&mask, SIGINT);
	sigaddset(&mask, SIGHUP);
	sigprocmask(SIG_BLOCK, &mask, NULL);
	sfd = signalfd(-1, &mask, SFD_CLOEXEC | SFD_NONBLOCK);

	snprintf(conf_dir, sizeof(conf_dir), "%s", S.conf_path);
	snprintf(conf_dir, sizeof(conf_dir), "%s", dirname(conf_dir));
	{
		char tmp[PATH_MAX];

		snprintf(tmp, sizeof(tmp), "%s", S.conf_path);
		snprintf(conf_base, sizeof(conf_base), "%s", basename(tmp));
	}
	ifd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
	if (ifd >= 0 && inotify_add_watch(ifd, conf_dir,
					  IN_CLOSE_WRITE | IN_MOVED_TO | IN_CREATE | IN_DELETE) < 0) {
		fprintf(stderr, "cannot watch %s: %s; restart to apply changes\n", conf_dir,
			strerror(errno));
	}

	printf("settings: enabled=%s debug=%s\n", S.cfg.enabled ? "yes" : "no",
	       S.cfg.debug ? "yes" : "no");
	update_grab();

	for (;;) {
		int n = 0;

		pfd[n++] = (struct pollfd){ .fd = S.tp_fd, .events = POLLIN };
		pfd[n++] = (struct pollfd){ .fd = sfd, .events = POLLIN };
		pfd[n++] = (struct pollfd){ .fd = ifd, .events = POLLIN };
		for (int i = 0; i < S.nkb; i++)
			pfd[n++] = (struct pollfd){ .fd = S.kb_fd[i], .events = POLLIN };

		if (poll(pfd, n, -1) < 0) {
			if (errno == EINTR)
				continue;
			perror("poll");
			return 1;
		}

		/* Keys first, so a key and a touch landing together see the
		 * key. */
		for (int i = 0; i < S.nkb; i++)
			if (S.kb_fd[i] >= 0 && pfd[3 + i].revents)
				read_keyboard(i);

		if (pfd[0].revents & (POLLERR | POLLHUP | POLLNVAL)) {
			fprintf(stderr, "trackpad went away\n");
			return 1;
		}
		if (pfd[0].revents & POLLIN && read_touchpad() < 0)
			return 1;

		if (pfd[1].revents & POLLIN) {
			struct signalfd_siginfo si;

			if (read(sfd, &si, sizeof(si)) == sizeof(si)) {
				if (si.ssi_signo == SIGHUP) {
					if (reload_config())
						break;
				} else {
					printf("stopping\n");
					break;
				}
			}
		}

		if (ifd >= 0 && pfd[2].revents & POLLIN) {
			char buf[4096] __attribute__((aligned(__alignof__(struct inotify_event))));
			bool ours = false;
			ssize_t len;

			while ((len = read(ifd, buf, sizeof(buf))) > 0) {
				for (char *p = buf; p < buf + len;) {
					struct inotify_event *ie = (struct inotify_event *)p;

					if (ie->len && !strcmp(ie->name, conf_base))
						ours = true;
					p += sizeof(*ie) + ie->len;
				}
			}
			if (ours && reload_config())
				break;
		}
	}

	/* Exiting drops the grab and removes the virtual trackpad. */
	libevdev_uinput_destroy(S.ui);
	return 0;
}
