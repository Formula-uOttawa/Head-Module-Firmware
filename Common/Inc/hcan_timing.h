/* hcan_timing.h - FDCAN bit timing from a target bit rate (portable C). */
#ifndef HCAN_TIMING_H
#define HCAN_TIMING_H

#include <stdbool.h>
#include <stdint.h>

typedef struct { uint32_t prescaler, seg1, seg2, sjw; } HcanTiming_t;

/*
 * Find prescaler/segments for `bitrate` from `kernel_hz`, aiming for `sample_permille` (e.g. 800 = 80%).
 * `data_phase` selects the FDCAN data-phase limits (smaller ranges) instead of the nominal ones.
 * Returns false if the rate cannot be hit exactly. seg1 includes the propagation segment.
 */
bool Hcan_ComputeTiming(uint32_t kernel_hz, uint32_t bitrate, uint32_t sample_permille, bool data_phase, HcanTiming_t *out);

#endif
