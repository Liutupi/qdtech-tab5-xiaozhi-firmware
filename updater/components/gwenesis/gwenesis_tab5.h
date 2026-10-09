#pragma once
// Tab5 port glue for the Gwenesis core.
#include <stddef.h>

// Large tables the core builds at start-up: internal RAM when available (fast), PSRAM
// otherwise. Never returns NULL (aborts instead).
void* gwenesis_tab5_alloc(size_t size);
