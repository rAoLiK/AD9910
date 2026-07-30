#ifndef PHASE_COMPENSATION_H
#define PHASE_COMPENSATION_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Return the correction added to the firmware feedback-phase target.
 *
 * The calibration workbook records phase as CH1 - output, whereas the phase
 * detector uses feedback - N * CH1 and the feedback path is inverted by
 * 180 degrees.  Therefore:
 *
 *   correction = measured_scope_phase - theoretical_scope_phase
 *
 * Calibration points are linearly interpolated by reference-input frequency.
 * Values outside the measured 1..100 kHz range are clamped to the nearest
 * endpoint.
 */
float PhaseCompensation_GetDeg(float reference_frequency_hz,
                               uint8_t multiplier);

#ifdef __cplusplus
}
#endif

#endif /* PHASE_COMPENSATION_H */
