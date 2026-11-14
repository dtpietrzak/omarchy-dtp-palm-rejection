#include "config.h"

#include <ctype.h>
#include <errno.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

enum kind { K_BOOL, K_INT, K_DOUBLE, K_NAME };

struct key {
	const char *name;
	enum kind kind;
	size_t offset;
	double min, max;
};

#define F(field) offsetof(struct pr_config, field)

static const struct key keys[] = {
	{ "enabled", K_BOOL, F(enabled), 0, 1 },
	{ "debug", K_BOOL, F(debug), 0, 1 },
	{ "palm_size", K_INT, F(palm_size), 1, 100000 },
	{ "finger_size", K_INT, F(finger_size), 1, 100000 },
	{ "max_ratio", K_DOUBLE, F(max_ratio), 1.0, 20.0 },
	{ "typing_ms", K_INT, F(typing_ms), 0, 5000 },
	{ "tap_guard_ms", K_INT, F(tap_guard_ms), 0, 5000 },
	{ "settle_ms", K_INT, F(settle_ms), 0, 1000 },
	{ "side_edge_mm", K_DOUBLE, F(side_edge_mm), 0, 100 },
	{ "top_edge_mm", K_DOUBLE, F(top_edge_mm), 0, 100 },
	{ "edge_release_mm", K_DOUBLE, F(edge_release_mm), 0, 200 },
	{ "center_release_mm", K_DOUBLE, F(center_release_mm), 0, 200 },
	{ "rest_mm", K_DOUBLE, F(rest_mm), 0, 200 },
	{ "touchpad_name", K_NAME, F(touchpad_name), 0, 0 },
	{ "keyboard_name", K_NAME, F(keyboard_name), 0, 0 },
};

static char *trim(char *s)
{
	char *end;

	while (isspace((unsigned char)*s))
		s++;
	end = s + strlen(s);
	while (end > s && isspace((unsigned char)end[-1]))
		*--end = '\0';
	return s;
}

static int set_value(const struct key *k, struct pr_config *cfg, const char *v)
{
	char *base = (char *)cfg + k->offset;
	char *end;

	switch (k->kind) {
	case K_BOOL:
		if (!strcmp(v, "yes") || !strcmp(v, "true") || !strcmp(v, "on") ||
		    !strcmp(v, "1"))
			*(bool *)base = true;
		else if (!strcmp(v, "no") || !strcmp(v, "false") || !strcmp(v, "off") ||
			 !strcmp(v, "0"))
			*(bool *)base = false;
		else
			return -1;
		return 0;
	case K_INT: {
		long n;

		errno = 0;
		n = strtol(v, &end, 10);
		if (errno || end == v || *end || n < k->min || n > k->max)
			return -1;
		*(int *)base = (int)n;
		return 0;
	}
	case K_DOUBLE: {
		double d;

		errno = 0;
		d = strtod(v, &end);
		if (errno || end == v || *end || !(d >= k->min && d <= k->max))
			return -1;
		*(double *)base = d;
		return 0;
	}
	case K_NAME: {
		size_t len = strlen(v);

		if (len >= sizeof(cfg->touchpad_name))
			return -1;
		for (size_t i = 0; i < len; i++)
			if (!isprint((unsigned char)v[i]))
				return -1;
		memcpy(base, v, len + 1);
		return 0;
	}
	}
	return -1;
}

int pr_config_parse(FILE *in, struct pr_config *cfg, FILE *err)
{
	char line[512];
	int lineno = 0, problems = 0;

	while (fgets(line, sizeof(line), in)) {
		char *s, *eq, *key, *value;
		size_t i;

		lineno++;
		if (!strchr(line, '\n') && !feof(in)) {
			int ch;

			while ((ch = fgetc(in)) != EOF && ch != '\n')
				;
			fprintf(err, "config line %d: too long, skipped\n", lineno);
			problems++;
			continue;
		}
		s = trim(line);
		if (!*s || *s == '#')
			continue;
		eq = strchr(s, '=');
		if (!eq) {
			fprintf(err, "config line %d: expected key = value\n", lineno);
			problems++;
			continue;
		}
		*eq = '\0';
		key = trim(s);
		value = trim(eq + 1);
		for (i = 0; i < sizeof(keys) / sizeof(keys[0]); i++)
			if (!strcmp(keys[i].name, key))
				break;
		if (i == sizeof(keys) / sizeof(keys[0])) {
			fprintf(err, "config line %d: unknown key \"%.64s\"\n", lineno, key);
			problems++;
			continue;
		}
		if (set_value(&keys[i], cfg, value)) {
			fprintf(err, "config line %d: bad value for %s\n", lineno, key);
			problems++;
		}
	}
	return problems;
}

int pr_config_load(const char *path, struct pr_config *cfg, FILE *err)
{
	FILE *f;
	int problems;

	pr_config_defaults(cfg);
	f = fopen(path, "re");
	if (!f)
		return errno == ENOENT ? 0 : -1;
	problems = pr_config_parse(f, cfg, err);
	fclose(f);
	return problems;
}
