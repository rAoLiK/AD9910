#include "pll_demo.h"

#include "adc.h"
#include "dual_adc.h"
#include "lock_controller.h"
#include "phase_compensation.h"
#include "phase_detector.h"
#include "pll_config.h"
#include "tim.h"

#include <math.h>
#include <stddef.h>
#include <string.h>

#define PLL_DMA_MIN_HALF_PAIRS        (128U)
#define PLL_DMA_MAX_HALF_PAIRS        (2048U)
#define PLL_DMA_HALF_ALIGNMENT        (64U)
#define PLL_DMA_TARGET_BLOCK_HZ       (1000U)
#define PLL_DMA_PROVEN_HIGH_RATE_HZ   (960000U)
#define PLL_DMA_PAIR_COUNT            (2U * PLL_DMA_MAX_HALF_PAIRS)
#define PLL_INITIAL_DDS_FREQUENCY_HZ  (10000.0f)
#define PLL_SEARCH_HIGH_RATE_HZ       DUAL_ADC_MAX_SAMPLE_RATE_HZ
#define PLL_RATE_CHANGE_HOLDOFF_MS    (100UL)
#define PLL_DDS_UPDATE_INTERVAL_MS    (1UL)
#define PLL_ACQUIRE_RESTART_MS        (1000UL)
#define PLL_LOW_ACQUIRE_RESTART_MS    (3000UL)
#define PLL_CHANGE_PENDING_RESTART_MS (600UL)
#define PLL_HIGH_NEAR_LOCK_ERROR_DEG  (30.0f)
#define PLL_DEG_PER_RAD               (57.29577951308232f)

typedef struct {
  ad9910_t *dds;
  phase_detector_t detector;
  lock_controller_t controller;
  phase_measurement_t measurement;
  lock_controller_output_t control;
  uint32_t actual_sample_rate_hz;
  uint32_t active_half_pair_count;
  uint32_t last_rate_change_tick;
  uint32_t last_valid_tick;
  uint32_t last_dds_update_tick;
  uint32_t acquire_start_tick;
  uint32_t frequency_change_start_tick;
  uint32_t last_ftw;
  uint16_t last_pow;
  double ftw_fraction_accumulator;
  float dds_frequency_hz;
  float dds_phase_offset_rad;
  float nominal_target_phase_deg;
  float phase_compensation_deg;
  float output_scale;
  bool initialized;
  bool running;
  bool external_frequency_seeded;
  bool dds_output_enabled;
  bool have_ftw;
  bool frequency_change_pending_seen;
  bool frequency_hold_observed;
} pll_demo_context_t;

static pll_demo_context_t s_context;
static pll_demo_status_t s_status;

static uint32_t s_adc_buffer[PLL_DMA_PAIR_COUNT]
    __attribute__((aligned(4)));

static volatile uint8_t s_dma_ready_mask;
static volatile bool s_adc_error_pending;
static volatile uint32_t s_dma_overrun_count;
static volatile uint32_t s_adc_error_count;

static bool PLL_Demo_ApplyDDS(float frequency_hz,
                              float phase_offset_rad,
                              bool output_enabled,
                              bool force)
{
  ad9910_profile_word_t profile;
  double exact_ftw;
  double fractional_ftw;
  uint32_t ftw;
  uint16_t pow;
  ad9910_status_t result;

  if ((s_context.dds == NULL) || (frequency_hz <= 0.0f)) {
    return false;
  }
  pow = AD9910_PhaseDegToPOW(
      (double)phase_offset_rad * (double)PLL_DEG_PER_RAD);

  /*
   * At 1 GHz SYSCLK one FTW LSB is about 0.233 Hz.  First-order fractional
   * FTW modulation at the 1 kHz service rate gives a sub-0.1 Hz average
   * command without asking the AD9910 for an impossible fractional word.
   */
  exact_ftw =
      ((double)frequency_hz * 4294967296.0) /
      (double)s_context.dds->cfg.sysclk.sys_clk_hz;
  if ((exact_ftw < 0.0) || (exact_ftw > 4294967295.0)) {
    return false;
  }
  if (!force && output_enabled &&
      s_context.control.frequency_hold_mode) {
    /*
     * A locked high-frequency output must use one fixed FTW.  The normal
     * fractional accumulator alternates adjacent words at 1 kHz; that improves
     * average frequency resolution but is directly visible as output jitter.
     * Residual sub-FTW phase drift is handled by the controller through POW.
     */
    ftw = (exact_ftw >= 4294967294.5)
              ? UINT32_MAX
              : (uint32_t)(exact_ftw + 0.5);
    s_context.ftw_fraction_accumulator = 0.0;
  } else {
    ftw = (uint32_t)exact_ftw;
    fractional_ftw = exact_ftw - (double)ftw;
    if (force) {
      s_context.ftw_fraction_accumulator = 0.0;
    }
    s_context.ftw_fraction_accumulator += fractional_ftw;
    if ((s_context.ftw_fraction_accumulator >= 1.0) &&
        (ftw != UINT32_MAX)) {
      ftw++;
      s_context.ftw_fraction_accumulator -= 1.0;
    }
  }

  /*
   * Advance the modulator only at the scheduled service interval, even when
   * the selected integer FTW happens to be unchanged.
   */
  s_context.last_dds_update_tick = HAL_GetTick();
  if (!force && s_context.have_ftw &&
      (ftw == s_context.last_ftw) &&
      (pow == s_context.last_pow) &&
      (output_enabled == s_context.dds_output_enabled)) {
    s_context.dds_frequency_hz = frequency_hz;
    s_context.dds_phase_offset_rad = phase_offset_rad;
    return true;
  }

  profile.ftw = ftw;
  profile.pow = pow;
  profile.asf = output_enabled
                    ? AD9910_AmplitudeScaleToASF(
                          (double)s_context.output_scale)
                    : 0U;
  result = AD9910_ProgramProfile(
      s_context.dds, AD9910_PROFILE_0, &profile, 1U);
  if (result != AD9910_STATUS_OK) {
    s_status.dds_error_count++;
    return false;
  }

  s_context.last_ftw = ftw;
  s_context.last_pow = pow;
  s_context.have_ftw = true;
  s_context.dds_output_enabled = output_enabled;
  s_context.dds_frequency_hz = frequency_hz;
  s_context.dds_phase_offset_rad = phase_offset_rad;
  return true;
}

static void PLL_Demo_ClearPendingDMA(void)
{
  uint32_t primask = __get_PRIMASK();

  __disable_irq();
  s_dma_ready_mask = 0U;
  s_adc_error_pending = false;
  __DMB();
  __set_PRIMASK(primask);
}

static bool PLL_Demo_StartSampling(uint32_t requested_rate_hz,
                                   bool retain_frequency)
{
  uint32_t actual_rate_hz = 0U;
  uint32_t half_pair_count;

  /*
   * Keep analysis events near 1 kHz.  The 128-pair floor still spans more
   * than five cycles at the normal 24-samples/cycle low-frequency rate.
   */
  if (requested_rate_hz >= PLL_DMA_PROVEN_HIGH_RATE_HZ) {
    /*
     * Keep the fixed 2048-pair high-frequency cadence that was already
     * verified on the board.  Dynamic short blocks are only needed to remove
     * the tens-of-milliseconds latency below 10 kHz.
     */
    half_pair_count = PLL_DMA_MAX_HALF_PAIRS;
  } else {
    half_pair_count =
        (requested_rate_hz + (PLL_DMA_TARGET_BLOCK_HZ / 2U)) /
        PLL_DMA_TARGET_BLOCK_HZ;
    half_pair_count =
        (half_pair_count + (PLL_DMA_HALF_ALIGNMENT - 1U)) &
        ~(PLL_DMA_HALF_ALIGNMENT - 1U);
    if (half_pair_count < PLL_DMA_MIN_HALF_PAIRS) {
      half_pair_count = PLL_DMA_MIN_HALF_PAIRS;
    } else if (half_pair_count > PLL_DMA_MAX_HALF_PAIRS) {
      half_pair_count = PLL_DMA_MAX_HALF_PAIRS;
    }
  }

  (void)DualADC_Stop();
  PLL_Demo_ClearPendingDMA();
  if (DualADC_Start(s_adc_buffer, 2U * half_pair_count,
                    requested_rate_hz, &actual_rate_hz) != HAL_OK) {
    return false;
  }

  s_context.actual_sample_rate_hz = actual_rate_hz;
  s_context.active_half_pair_count = half_pair_count;
  s_context.last_rate_change_tick = HAL_GetTick();
  PhaseDetector_SetSampleRate(&s_context.detector,
                              (float)actual_rate_hz,
                              retain_frequency);
  return true;
}

static void PLL_Demo_EnterError(const char *reason)
{
  (void)reason;
  (void)DualADC_Stop();
  s_context.running = false;
  s_status.state = PLL_DEMO_ERROR;
  (void)PLL_Demo_ApplyDDS(
      (s_context.dds_frequency_hz > 0.0f)
          ? s_context.dds_frequency_hz
          : PLL_INITIAL_DDS_FREQUENCY_HZ,
      s_context.dds_phase_offset_rad,
      false, true);
}

static bool PLL_Demo_ChangeRateIfNeeded(uint32_t requested_rate_hz,
                                        bool retain_frequency)
{
  uint32_t current = s_context.actual_sample_rate_hz;
  uint32_t difference;
  uint32_t now = HAL_GetTick();

  if (current == 0U) {
    return PLL_Demo_StartSampling(requested_rate_hz, retain_frequency);
  }

  difference = (requested_rate_hz > current)
                   ? (requested_rate_hz - current)
                   : (current - requested_rate_hz);
  if (difference == 0U) {
    return true;
  }
  if ((s_status.state != PLL_DEMO_SEARCHING) &&
      (((uint64_t)difference * 100ULL) <
       ((uint64_t)current * 12ULL))) {
    return true;
  }
  /*
   * The first trustworthy frequency estimate is still measured at the
   * alias-safe 2.4 MSPS search rate.  Apply its large rate reduction
   * immediately; otherwise a 1 kHz input waits 100 ms before phase
   * demodulation has a long enough time aperture.
   */
  if ((s_status.state != PLL_DEMO_SEARCHING) &&
      ((uint32_t)(now - s_context.last_rate_change_tick) <
       PLL_RATE_CHANGE_HOLDOFF_MS)) {
    return true;
  }

  return PLL_Demo_StartSampling(requested_rate_hz, retain_frequency);
}

static bool PLL_Demo_PopReadyHalf(uint8_t *half)
{
  uint32_t primask;
  uint8_t mask;

  if (half == NULL) {
    return false;
  }

  primask = __get_PRIMASK();
  __disable_irq();
  __DMB();
  mask = s_dma_ready_mask;
  if ((mask & 0x01U) != 0U) {
    *half = 0U;
    s_dma_ready_mask = (uint8_t)(mask & ~0x01U);
  } else if ((mask & 0x02U) != 0U) {
    *half = 1U;
    s_dma_ready_mask = (uint8_t)(mask & ~0x02U);
  } else {
    __DMB();
    __set_PRIMASK(primask);
    return false;
  }
  __DMB();
  __set_PRIMASK(primask);
  return true;
}

static void PLL_Demo_ProcessSamples(const uint32_t *samples)
{
  float elapsed_seconds;
  uint32_t now = HAL_GetTick();
  pll_demo_state_t previous_state = s_status.state;
  pll_demo_state_t next_state;

  PhaseDetector_Process(
      &s_context.detector, samples,
      s_context.active_half_pair_count,
      LockController_GetMultiplier(&s_context.controller),
      &s_context.measurement);

  if (s_context.measurement.frequency_valid) {
    s_context.phase_compensation_deg =
        PhaseCompensation_GetDeg(
            s_context.measurement.reference_frequency_hz,
            LockController_GetMultiplier(&s_context.controller));
    LockController_TrackTargetPhaseDeg(
        &s_context.controller,
        s_context.nominal_target_phase_deg +
            s_context.phase_compensation_deg);
  }

  elapsed_seconds =
      (float)s_context.active_half_pair_count /
      (float)s_context.actual_sample_rate_hz;
  LockController_Step(&s_context.controller,
                      &s_context.measurement,
                      elapsed_seconds,
                      &s_context.control);

  if (s_context.control.frequency_hold_mode !=
      s_context.frequency_hold_observed) {
    if (s_context.control.frequency_hold_mode) {
      s_status.frequency_hold_enter_count++;
    } else {
      s_status.frequency_hold_exit_count++;
    }
    s_context.frequency_hold_observed =
        s_context.control.frequency_hold_mode;
  }

  if (!s_context.measurement.frequency_valid) {
    if (previous_state == PLL_DEMO_LOCKED) {
      s_status.lock_loss_count++;
    }
    s_status.state = PLL_DEMO_SEARCHING;
    return;
  }

  s_context.last_valid_tick = now;
  if (s_context.control.command_valid) {
    if (s_context.control.frequency_reanchored) {
      s_status.frequency_reanchor_count++;
      s_context.acquire_start_tick = now;
    }
    if (s_context.control.frequency_change_pending) {
      if (!s_context.frequency_change_pending_seen) {
        s_context.frequency_change_start_tick = now;
        s_context.frequency_change_pending_seen = true;
      }
    } else {
      s_context.frequency_change_pending_seen = false;
    }
    if (!PLL_Demo_ChangeRateIfNeeded(
            s_context.control.requested_sample_rate_hz, true)) {
      PLL_Demo_EnterError("ADC_RATE");
      return;
    }

  }

  next_state = s_context.control.phase_locked
                   ? PLL_DEMO_LOCKED
                   : PLL_DEMO_ACQUIRING;
  if ((previous_state == PLL_DEMO_LOCKED) &&
      (next_state != PLL_DEMO_LOCKED)) {
    s_status.lock_loss_count++;
  }
  if ((next_state == PLL_DEMO_ACQUIRING) &&
      (s_status.state != PLL_DEMO_ACQUIRING)) {
    s_context.acquire_start_tick = now;
  }
  s_status.state = next_state;
}

static void PLL_Demo_ServiceDDS(void)
{
  if (!s_context.running ||
      !s_context.control.command_valid ||
      (s_status.state == PLL_DEMO_ERROR) ||
      ((uint32_t)(HAL_GetTick() -
                  s_context.last_dds_update_tick) <
       PLL_DDS_UPDATE_INTERVAL_MS)) {
    return;
  }

  if (!PLL_Demo_ApplyDDS(s_context.control.dds_frequency_hz,
                         s_context.control.dds_phase_offset_rad,
                         true, false)) {
    PLL_Demo_EnterError("DDS");
  }
}

static void PLL_Demo_ProcessSearchRate(void)
{
  if (!s_context.running ||
      s_context.measurement.frequency_valid ||
      (s_status.state == PLL_DEMO_ERROR)) {
    return;
  }

  /*
   * Always reacquire at the maximum rate. A low search rate can alias a newly
   * connected high-frequency source into a plausible but incorrect result.
   */
  if ((s_context.actual_sample_rate_hz != PLL_SEARCH_HIGH_RATE_HZ) &&
      ((uint32_t)(HAL_GetTick() -
                  s_context.last_rate_change_tick) >=
       PLL_RATE_CHANGE_HOLDOFF_MS)) {
    if (!PLL_Demo_StartSampling(PLL_SEARCH_HIGH_RATE_HZ, false)) {
      PLL_Demo_EnterError("ADC_SEARCH");
    }
  }
}

static void PLL_Demo_ProcessAcquireWatchdog(void)
{
  uint32_t now = HAL_GetTick();
  float reference_frequency_hz =
      s_context.measurement.reference_frequency_hz;
  float observed_dds_frequency_hz =
      reference_frequency_hz *
      (float)LockController_GetMultiplier(&s_context.controller);
  uint32_t timeout_ms =
      s_context.control.direct_phase_mode
          ? PLL_LOW_ACQUIRE_RESTART_MS
          : PLL_ACQUIRE_RESTART_MS;
  bool change_timed_out =
      s_context.frequency_change_pending_seen &&
      ((uint32_t)(now - s_context.frequency_change_start_tick) >=
       PLL_CHANGE_PENDING_RESTART_MS);
  bool acquire_timed_out =
      (s_status.state == PLL_DEMO_ACQUIRING) &&
      ((uint32_t)(now - s_context.acquire_start_tick) >= timeout_ms);
  bool high_frequency_near_lock =
      s_context.measurement.frequency_valid &&
      s_context.measurement.phase_valid &&
      ((reference_frequency_hz >= PLL_HIGH_HOLD_ENTER_HZ) ||
       (observed_dds_frequency_hz >=
        PLL_HIGH_OUTPUT_HOLD_ENTER_HZ)) &&
      (fabsf(s_context.control.phase_error_rad * PLL_DEG_PER_RAD) <=
       PLL_HIGH_NEAR_LOCK_ERROR_DEG);

  /*
   * At high reference or DDS-output frequencies, requiring 50 ms continuously
   * inside the 3-degree lock window can keep a genuinely converging loop in
   * ACQUIRE until the old one-second watchdog expires.  A full estimator reset
   * at that point is the large periodic jerk seen on the board.  If frequency
   * and phase are both valid and the filtered error is already near lock, keep
   * the PI state and extend only this watchdog window. Task5 supplies an
   * independently confirmed absolute-frequency seed, so its acquisition must
   * also preserve the controller state instead of periodically restarting.
   * Source-change timeout remains authoritative.
   */
  if (acquire_timed_out &&
      (high_frequency_near_lock ||
       s_context.external_frequency_seeded)) {
    s_context.acquire_start_tick = now;
    s_status.acquire_restart_suppressed_count++;
    acquire_timed_out = false;
  }

  if (!s_context.running ||
      (!change_timed_out && !acquire_timed_out)) {
    return;
  }

  /*
   * A large upward source jump can alias at the old dynamic sample rate, and
   * a rare noisy capture can otherwise remain in ACQUIRE indefinitely.
   * Restart the same deterministic workflow used at power-up: clear both
   * estimators, return to the alias-safe rate, then derive the new rate.
   */
  LockController_ResetLoop(&s_context.controller);
  memset(&s_context.measurement, 0, sizeof(s_context.measurement));
  memset(&s_context.control, 0, sizeof(s_context.control));
  s_context.frequency_change_pending_seen = false;
  if (!PLL_Demo_StartSampling(PLL_SEARCH_HIGH_RATE_HZ, false)) {
    PLL_Demo_EnterError("REACQUIRE");
    return;
  }
  s_status.search_restart_count++;
  s_status.state = PLL_DEMO_SEARCHING;
}

static void PLL_Demo_UpdateStatus(void)
{
  s_status.multiplier =
      LockController_GetMultiplier(&s_context.controller);
  s_status.target_phase_deg =
      LockController_GetTargetPhaseDeg(&s_context.controller);
  s_status.nominal_target_phase_deg =
      s_context.nominal_target_phase_deg;
  s_status.phase_compensation_deg =
      s_context.phase_compensation_deg;
  s_status.reference_frequency_hz =
      s_context.measurement.reference_frequency_hz;
  s_status.dds_frequency_hz = s_context.dds_frequency_hz;
  s_status.measured_phase_deg =
      s_context.measurement.generalized_phase_rad *
      PLL_DEG_PER_RAD;
  s_status.phase_error_deg =
      s_context.control.phase_error_rad * PLL_DEG_PER_RAD;
  s_status.phase_quality = s_context.measurement.phase_quality;
  s_status.frequency_step_hz =
      s_context.control.frequency_step_hz;
  s_status.dds_phase_offset_deg =
      s_context.control.dds_phase_offset_rad * PLL_DEG_PER_RAD;
  s_status.phase_step_deg =
      s_context.control.phase_step_rad * PLL_DEG_PER_RAD;
  s_status.output_scale = s_context.output_scale;
  s_status.fine_mode = s_context.control.fine_mode;
  s_status.direct_phase_mode =
      s_context.control.direct_phase_mode;
  s_status.frequency_hold_mode =
      s_context.control.frequency_hold_mode;
  s_status.frequency_change_pending =
      s_context.control.frequency_change_pending;
  s_status.lock_band = (uint8_t)s_context.control.band;
  s_status.phase_pair_count =
      s_context.measurement.phase_pair_count;
  s_status.block_pair_count =
      (uint16_t)s_context.active_half_pair_count;
  s_status.analysis_stride =
      s_context.measurement.analysis_stride;
  s_status.sample_rate_hz = s_context.actual_sample_rate_hz;
  s_status.dma_overrun_count = s_dma_overrun_count;
  s_status.adc_error_count = s_adc_error_count;
  s_status.uart_overflow_count = 0U;
}

HAL_StatusTypeDef PLL_Demo_Init(ad9910_t *dds)
{
  memset(&s_context, 0, sizeof(s_context));
  memset(&s_status, 0, sizeof(s_status));
  s_context.dds = dds;

  if ((dds == NULL) || (dds->initialized == 0U)) {
    s_status.state = PLL_DEMO_ERROR;
    return HAL_ERROR;
  }

  PhaseDetector_Init(&s_context.detector,
                     (float)PLL_SEARCH_HIGH_RATE_HZ);
  LockController_Init(&s_context.controller);
  s_context.output_scale = 1.0f;
  if (!PLL_Demo_ApplyDDS(PLL_INITIAL_DDS_FREQUENCY_HZ,
                         0.0f,
                         false, true)) {
    PLL_Demo_EnterError("INIT");
    return HAL_ERROR;
  }

  s_context.running = false;
  s_context.initialized = true;
  s_context.last_valid_tick = HAL_GetTick();
  s_status.state = PLL_DEMO_STOPPED;
  PLL_Demo_UpdateStatus();
  return HAL_OK;
}

HAL_StatusTypeDef PLL_Demo_Configure(uint8_t multiplier,
                                     float target_phase_deg,
                                     float output_scale)
{
  uint8_t old_multiplier;

  if (!s_context.initialized ||
      ((multiplier != 1U) && (multiplier != 2U)) ||
      !isfinite(target_phase_deg) ||
      !isfinite(output_scale) ||
      (output_scale < 0.0f) ||
      (output_scale > 1.0f)) {
    return HAL_ERROR;
  }

  old_multiplier =
      LockController_GetMultiplier(&s_context.controller);
  if (!LockController_SetMultiplier(
          &s_context.controller, multiplier)) {
    return HAL_ERROR;
  }
  if ((old_multiplier != multiplier) ||
      (fabsf(s_context.nominal_target_phase_deg -
             target_phase_deg) > 0.0001f)) {
    s_context.nominal_target_phase_deg = target_phase_deg;
    s_context.phase_compensation_deg = 0.0f;
    LockController_SetTargetPhaseDeg(
        &s_context.controller, target_phase_deg);
  }
  s_context.output_scale = output_scale;

  if (old_multiplier != multiplier) {
    PhaseDetector_ResetFrequency(&s_context.detector);
    memset(&s_context.measurement, 0, sizeof(s_context.measurement));
    memset(&s_context.control, 0, sizeof(s_context.control));
    s_context.frequency_change_pending_seen = false;
    s_context.frequency_hold_observed = false;
    if (s_context.running &&
        !PLL_Demo_StartSampling(PLL_SEARCH_HIGH_RATE_HZ, false)) {
      PLL_Demo_EnterError("CONFIG_RATE");
      PLL_Demo_UpdateStatus();
      return HAL_ERROR;
    }
    if (s_context.running) {
      s_status.state = PLL_DEMO_SEARCHING;
    }
  }

  if (!PLL_Demo_ApplyDDS(
          (s_context.dds_frequency_hz > 0.0f)
              ? s_context.dds_frequency_hz
              : PLL_INITIAL_DDS_FREQUENCY_HZ,
          s_context.dds_phase_offset_rad,
          s_context.running, true)) {
    PLL_Demo_EnterError("CONFIG_DDS");
    PLL_Demo_UpdateStatus();
    return HAL_ERROR;
  }
  PLL_Demo_UpdateStatus();
  return HAL_OK;
}

HAL_StatusTypeDef PLL_Demo_SeedFrequency(float frequency_hz)
{
  if (!s_context.initialized || s_context.running ||
      !isfinite(frequency_hz) ||
      (frequency_hz < 1.0f) ||
      (frequency_hz > 400000000.0f)) {
    return HAL_ERROR;
  }

  if (!PLL_Demo_ApplyDDS(
          frequency_hz, 0.0f, false, true)) {
    PLL_Demo_EnterError("SEED_DDS");
    PLL_Demo_UpdateStatus();
    return HAL_ERROR;
  }
  s_context.external_frequency_seeded = true;
  PLL_Demo_UpdateStatus();
  return HAL_OK;
}

HAL_StatusTypeDef PLL_Demo_Start(void)
{
  if (!s_context.initialized) {
    return HAL_ERROR;
  }
  if (s_context.running) {
    return HAL_OK;
  }

  LockController_ResetLoop(&s_context.controller);
  PhaseDetector_ResetFrequency(&s_context.detector);
  memset(&s_context.measurement, 0, sizeof(s_context.measurement));
  memset(&s_context.control, 0, sizeof(s_context.control));
  s_context.dds_phase_offset_rad = 0.0f;
  s_context.frequency_change_pending_seen = false;
  s_context.frequency_hold_observed = false;
  if (!PLL_Demo_ApplyDDS(
          (s_context.dds_frequency_hz > 0.0f)
              ? s_context.dds_frequency_hz
              : PLL_INITIAL_DDS_FREQUENCY_HZ,
          s_context.dds_phase_offset_rad,
          true, true) ||
      !PLL_Demo_StartSampling(PLL_SEARCH_HIGH_RATE_HZ, false)) {
    PLL_Demo_EnterError("START");
    PLL_Demo_UpdateStatus();
    return HAL_ERROR;
  }

  s_context.running = true;
  s_context.last_valid_tick = HAL_GetTick();
  s_context.acquire_start_tick = s_context.last_valid_tick;
  s_status.state = PLL_DEMO_SEARCHING;
  PLL_Demo_UpdateStatus();
  return HAL_OK;
}

HAL_StatusTypeDef PLL_Demo_Stop(void)
{
  HAL_StatusTypeDef result;
  bool dds_ok;

  if (!s_context.initialized) {
    return HAL_ERROR;
  }

  result = DualADC_Stop();
  PLL_Demo_ClearPendingDMA();
  s_context.running = false;
  s_context.external_frequency_seeded = false;
  s_context.actual_sample_rate_hz = 0U;
  s_context.active_half_pair_count = 0U;
  s_context.frequency_change_pending_seen = false;
  s_context.frequency_hold_observed = false;
  memset(&s_context.measurement, 0, sizeof(s_context.measurement));
  memset(&s_context.control, 0, sizeof(s_context.control));
  dds_ok = PLL_Demo_ApplyDDS(
      (s_context.dds_frequency_hz > 0.0f)
          ? s_context.dds_frequency_hz
          : PLL_INITIAL_DDS_FREQUENCY_HZ,
      s_context.dds_phase_offset_rad,
      false, true);
  s_status.state =
      ((result == HAL_OK) && dds_ok)
          ? PLL_DEMO_STOPPED
          : PLL_DEMO_ERROR;
  PLL_Demo_UpdateStatus();
  return (s_status.state == PLL_DEMO_STOPPED) ? HAL_OK : HAL_ERROR;
}

void PLL_Demo_Process(void)
{
  uint8_t half;
  uint32_t processed = 0U;

  if (!s_context.initialized) {
    return;
  }

  if (s_adc_error_pending && s_context.running) {
    s_adc_error_pending = false;
    if (!PLL_Demo_StartSampling(
            s_context.actual_sample_rate_hz, true)) {
      PLL_Demo_EnterError("ADC");
      PLL_Demo_UpdateStatus();
      return;
    }
  }

  while ((processed < 4U) && PLL_Demo_PopReadyHalf(&half)) {
    const uint32_t *samples =
        &s_adc_buffer[(uint32_t)half *
                      s_context.active_half_pair_count];
    PLL_Demo_ProcessSamples(samples);
    processed++;
    if (!s_context.running) {
      break;
    }
  }

  PLL_Demo_ProcessSearchRate();
  PLL_Demo_ProcessAcquireWatchdog();
  PLL_Demo_ServiceDDS();
  PLL_Demo_UpdateStatus();
}

const pll_demo_status_t *PLL_Demo_GetStatus(void)
{
  return &s_status;
}

static void PLL_Demo_PublishDMAHalf(uint8_t bit)
{
  uint8_t mask = s_dma_ready_mask;

  if ((mask & bit) != 0U) {
    s_dma_overrun_count++;
  } else {
    s_dma_ready_mask = (uint8_t)(mask | bit);
    __DMB();
  }
}

void PLL_Demo_AdcHalfCpltISR(ADC_HandleTypeDef *hadc)
{
  if ((hadc != NULL) && (hadc->Instance == ADC1) &&
      s_context.running) {
    PLL_Demo_PublishDMAHalf(0x01U);
  }
}

void PLL_Demo_AdcCpltISR(ADC_HandleTypeDef *hadc)
{
  if ((hadc != NULL) && (hadc->Instance == ADC1) &&
      s_context.running) {
    PLL_Demo_PublishDMAHalf(0x02U);
  }
}

void PLL_Demo_AdcErrorISR(ADC_HandleTypeDef *hadc)
{
  if ((hadc != NULL) &&
      ((hadc->Instance == ADC1) || (hadc->Instance == ADC2))) {
    s_adc_error_count++;
    s_adc_error_pending = true;
    __HAL_TIM_DISABLE(&htim2);
  }
}

void HAL_ADC_ConvHalfCpltCallback(ADC_HandleTypeDef *hadc)
{
  PLL_Demo_AdcHalfCpltISR(hadc);
}

void HAL_ADC_ConvCpltCallback(ADC_HandleTypeDef *hadc)
{
  PLL_Demo_AdcCpltISR(hadc);
}

void HAL_ADC_ErrorCallback(ADC_HandleTypeDef *hadc)
{
  PLL_Demo_AdcErrorISR(hadc);
}
