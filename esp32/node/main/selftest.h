#pragma once

#include <stdbool.h>

// Bench commands: "selftest" runs the vectors generated from lywsd02.py, "heap"
// prints memory, "blecycle N" switches the radio on and off, "ntp [server]" asks a
// time server how far off our clock is. False when the line is none of them.
bool selftest_command(const char *line);
