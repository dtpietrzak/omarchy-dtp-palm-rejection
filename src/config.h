#pragma once

#include <stdio.h>

#include "classifier.h"

/* Parse "key = value" lines into cfg, starting from its current values.
 * Unknown keys and bad values are reported to err and skipped, so one typo
 * never turns the filter off. Returns the number of problems found. */
int pr_config_parse(FILE *in, struct pr_config *cfg, FILE *err);

/* Defaults, then the file at path if it exists. Returns problems found, or
 * -1 when the file exists but cannot be read. */
int pr_config_load(const char *path, struct pr_config *cfg, FILE *err);
