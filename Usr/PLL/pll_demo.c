#include "pll_demo.h"

#include "adc.h"
#include "dual_adc.h"
#include "lock_controller.h"
#include "phase_detector.h"
#include "tim.h"
#include "usart.h"

#include <ctype.h>
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#define PLL_DMA_MIN_HALF_PAIRS        (128U)
#define PLL_DMA_MAX_HALF_PAIRS        (2048U)
#define PLL_DMA_HALF_ALIGNMENT        (64U)
#define PLL_DMA_TARGET_BLOCK_HZ       (1000U)
#define PLL_DMA_PROVEN_HIGH_RATE_HZ   (960000U)
#define PLL_DMA_PAIR_COUNT            (2U * PLL_DMA_MAX_HALF_PAIRS)
#define PLL_RX_RING_SIZE              (128U)
#define PLL_RX_RING_MASK              (PLL_RX_RING_SIZE - 1U)
#define PLL_TX_RING_SIZE              (1024U)
#define PLL_TX_RING_MASK              (PLL_TX_RING_SIZE - 1U)
#define PLL_COMMAND_LINE_SIZE         (64U)
#define PLL_INITIAL_DDS_FREQUENCY_HZ  (10000.0f)
#define PLL_SEARCH_HIGH_RATE_HZ       DUAL_ADC_MAX_SAMPLE_RATE_HZ
#define PLL_RATE_CHANGE_HOLDOFF_MS    (100UL)
#define PLL_DDS_UPDATE_INTERVAL_MS    (1UL)
#define PLL_ACQUIRE_RESTART_MS        (1000UL)
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
  uint32_t last_ftw;
  uint16_t last_pow;
  double ftw_fraction_accumulator;
  float dds_frequency_hz;
  float dds_phase_offset_rad;
  bool initialized;
  bool running;
  bool dds_output_enabled;
  bool have_ftw;
} pll_demo_context_t;

static pll_demo_context_t s_context;
static pll_demo_status_t s_status;

static uint32_t s_adc_buffer[PLL_DMA_PAIR_COUNT]
    __attribute__((aligned(4)));

static volatile uint8_t s_dma_ready_mask;
static volatile bool s_adc_error_pending;
static volatile uint32_t s_dma_overrun_count;
static volatile uint32_t s_adc_error_count;

static uint8_t s_uart_rx_byte;
static uint8_t s_uart_rx_ring[PLL_RX_RING_SIZE];
static volatile uint16_t s_uart_rx_head;
static volatile uint16_t s_uart_rx_tail;
static volatile uint32_t s_uart_overflow_count;
static uint8_t s_uart_tx_ring[PLL_TX_RING_SIZE];
static volatile uint16_t s_uart_tx_head;
static volatile uint16_t s_uart_tx_tail;
static volatile bool s_uart_tx_busy;
static char s_command_line[PLL_COMMAND_LINE_SIZE];
static uint32_t s_command_length;

static const char *PLL_Demo_StateName(pll_demo_state_t state)
{
  switch (state) {
    case PLL_DEMO_STOPPED:
      return "STOPPED";
    case PLL_DEMO_SEARCHING:
      return "SEARCH";
    case PLL_DEMO_ACQUIRING:
      return "ACQUIRE";
    case PLL_DEMO_LOCKED:
      return "LOCKED";
    case PLL_DEMO_ERROR:
    default:
      return "ERROR";
  }
}

static const char *PLL_Demo_BandName(uint8_t band)
{
  switch ((lock_band_t)band) {
    case LOCK_BAND_LOW:
      return "LOW";
    case LOCK_BAND_HIGH:
      return "HIGH";
    case LOCK_BAND_MID:
    default:
      return "MID";
  }
}

static void PLL_Demo_TxKick(void)
{
  uint32_t primask;
  uint16_t tail;
  bool start = false;

  primask = __get_PRIMASK();
  __disable_irq();
  __DMB();
  tail = s_uart_tx_tail;
  if (!s_uart_tx_busy && (tail != s_uart_tx_head)) {
    s_uart_tx_busy = true;
    start = true;
  }
  __DMB();
  __set_PRIMASK(primask);

  if (start &&
      (HAL_UART_Transmit_IT(
           &huart1, &s_uart_tx_ring[tail], 1U) != HAL_OK)) {
    primask = __get_PRIMASK();
    __disable_irq();
    s_uart_tx_busy = false;
    __DMB();
    __set_PRIMASK(primask);
  }
}

static void PLL_Demo_Send(const char *text)
{
  if (text == NULL) {
    return;
  }

  while (*text != '\0') {
    uint16_t head = s_uart_tx_head;
    uint16_t next =
        (uint16_t)((head + 1U) & PLL_TX_RING_MASK);

    if (next == s_uart_tx_tail) {
      s_uart_overflow_count++;
      break;
    }
    s_uart_tx_ring[head] = (uint8_t)*text++;
    __DMB();
    s_uart_tx_head = next;
  }
  PLL_Demo_TxKick();
}

static void PLL_Demo_SendHelp(void)
{
  PLL_Demo_Send(
      "Commands: MUL 1|2, PHASE <deg>, START, STOP, STATUS, HELP\r\n"
      "Phase definition: phi_DDS - MUL*phi_REF, normalized to [-180,180).\r\n");
}

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
  profile.asf = output_enabled ? AD9910_ASF_MAX : 0U;
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
  (void)DualADC_Stop();
  s_context.running = false;
  s_status.state = PLL_DEMO_ERROR;
  (void)PLL_Demo_ApplyDDS(
      (s_context.dds_frequency_hz > 0.0f)
          ? s_context.dds_frequency_hz
          : PLL_INITIAL_DDS_FREQUENCY_HZ,
      s_context.dds_phase_offset_rad,
      false, true);
  PLL_Demo_Send("ERR ");
  PLL_Demo_Send((reason != NULL) ? reason : "UNKNOWN");
  PLL_Demo_Send("\r\n");
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
  if (((uint64_t)difference * 100ULL) <
      ((uint64_t)current * 12ULL)) {
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
  pll_demo_state_t next_state;

  PhaseDetector_Process(
      &s_context.detector, samples,
      s_context.active_half_pair_count,
      LockController_GetMultiplier(&s_context.controller),
      &s_context.measurement);

  elapsed_seconds =
      (float)s_context.active_half_pair_count /
      (float)s_context.actual_sample_rate_hz;
  LockController_Step(&s_context.controller,
                      &s_context.measurement,
                      elapsed_seconds,
                      &s_context.control);

  if (!s_context.measurement.frequency_valid) {
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
      /* Do not time out while the external generator is still slewing. */
      s_context.acquire_start_tick = now;
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

static bool PLL_Demo_ParseDecimal(const char *text, float *value)
{
  bool negative = false;
  bool have_digit = false;
  float integer = 0.0f;
  float fraction = 0.0f;
  float scale = 1.0f;

  if ((text == NULL) || (value == NULL)) {
    return false;
  }
  while (*text == ' ') {
    text++;
  }
  if ((*text == '+') || (*text == '-')) {
    negative = (*text == '-');
    text++;
  }
  while (isdigit((unsigned char)*text) != 0) {
    integer = integer * 10.0f + (float)(*text - '0');
    have_digit = true;
    text++;
  }
  if (*text == '.') {
    text++;
    while (isdigit((unsigned char)*text) != 0) {
      scale *= 0.1f;
      fraction += (float)(*text - '0') * scale;
      have_digit = true;
      text++;
    }
  }
  while (*text == ' ') {
    text++;
  }
  if (!have_digit || (*text != '\0')) {
    return false;
  }

  *value = integer + fraction;
  if (negative) {
    *value = -*value;
  }
  return true;
}

static void PLL_Demo_SendStatus(void)
{
  static char line[336];
  int32_t phase_mdeg =
      (int32_t)lroundf(s_status.measured_phase_deg * 1000.0f);
  int32_t error_mdeg =
      (int32_t)lroundf(s_status.phase_error_deg * 1000.0f);
  uint32_t phase_abs =
      (uint32_t)((phase_mdeg < 0) ? -phase_mdeg : phase_mdeg);
  uint32_t error_abs =
      (uint32_t)((error_mdeg < 0) ? -error_mdeg : error_mdeg);
  uint32_t ref_centi =
      (uint32_t)lroundf(s_status.reference_frequency_hz * 100.0f);
  uint32_t dds_centi =
      (uint32_t)lroundf(s_status.dds_frequency_hz * 100.0f);
  uint32_t quality_milli =
      (uint32_t)lroundf(s_status.phase_quality * 1000.0f);
  int32_t step_millihz =
      (int32_t)lroundf(s_status.frequency_step_hz * 1000.0f);
  uint32_t step_abs =
      (uint32_t)((step_millihz < 0) ? -step_millihz :
                                        step_millihz);

  (void)snprintf(
      line, sizeof(line),
      "STATE=%s MUL=%u REF=%lu.%02luHz DDS=%lu.%02luHz "
      "PH=%c%lu.%03lu ERR=%c%lu.%03lu Q=%lu.%03lu FS=%lu "
      "STEP=%c%lu.%03luHz MODE=%s CTRL=%s BAND=%s "
      "CHG=%u STRIDE=%u WIN=%u BLK=%u "
      "ANCHOR=%lu RST=%lu "
      "OVR=%lu ADCERR=%lu RXOVR=%lu DDSERR=%lu\r\n",
      PLL_Demo_StateName(s_status.state),
      (unsigned int)s_status.multiplier,
      (unsigned long)(ref_centi / 100U),
      (unsigned long)(ref_centi % 100U),
      (unsigned long)(dds_centi / 100U),
      (unsigned long)(dds_centi % 100U),
      (phase_mdeg < 0) ? '-' : '+',
      (unsigned long)(phase_abs / 1000U),
      (unsigned long)(phase_abs % 1000U),
      (error_mdeg < 0) ? '-' : '+',
      (unsigned long)(error_abs / 1000U),
      (unsigned long)(error_abs % 1000U),
      (unsigned long)(quality_milli / 1000U),
      (unsigned long)(quality_milli % 1000U),
      (unsigned long)s_status.sample_rate_hz,
      (step_millihz < 0) ? '-' : '+',
      (unsigned long)(step_abs / 1000U),
      (unsigned long)(step_abs % 1000U),
      s_status.fine_mode ? "FINE" : "COARSE",
      s_status.direct_phase_mode ? "POW" : "FTW",
      PLL_Demo_BandName(s_status.lock_band),
      s_status.frequency_change_pending ? 1U : 0U,
      (unsigned int)s_status.analysis_stride,
      (unsigned int)s_status.phase_pair_count,
      (unsigned int)s_status.block_pair_count,
      (unsigned long)s_status.frequency_reanchor_count,
      (unsigned long)s_status.search_restart_count,
      (unsigned long)s_status.dma_overrun_count,
      (unsigned long)s_status.adc_error_count,
      (unsigned long)s_status.uart_overflow_count,
      (unsigned long)s_status.dds_error_count);
  PLL_Demo_Send(line);
}

static void PLL_Demo_StartCommand(void)
{
  if (s_context.running) {
    PLL_Demo_Send("OK START\r\n");
    return;
  }

  LockController_ResetLoop(&s_context.controller);
  PhaseDetector_ResetFrequency(&s_context.detector);
  if (!PLL_Demo_ApplyDDS(
          (s_context.dds_frequency_hz > 0.0f)
              ? s_context.dds_frequency_hz
              : PLL_INITIAL_DDS_FREQUENCY_HZ,
          s_context.dds_phase_offset_rad,
          true, true) ||
      !PLL_Demo_StartSampling(PLL_SEARCH_HIGH_RATE_HZ, false)) {
    PLL_Demo_EnterError("START");
    return;
  }

  s_context.running = true;
  s_status.state = PLL_DEMO_SEARCHING;
  PLL_Demo_Send("OK START\r\n");
}

static void PLL_Demo_StopCommand(void)
{
  (void)DualADC_Stop();
  PLL_Demo_ClearPendingDMA();
  s_context.running = false;
  s_status.state = PLL_DEMO_STOPPED;
  if (!PLL_Demo_ApplyDDS(
          (s_context.dds_frequency_hz > 0.0f)
              ? s_context.dds_frequency_hz
              : PLL_INITIAL_DDS_FREQUENCY_HZ,
          s_context.dds_phase_offset_rad,
          false, true)) {
    s_status.state = PLL_DEMO_ERROR;
  }
  PLL_Demo_Send("OK STOP\r\n");
}

static void PLL_Demo_HandleCommand(char *line)
{
  uint32_t i;

  if (line == NULL) {
    return;
  }
  for (i = 0U; line[i] != '\0'; ++i) {
    line[i] = (char)toupper((unsigned char)line[i]);
  }

  if (strcmp(line, "HELP") == 0) {
    PLL_Demo_SendHelp();
  } else if (strcmp(line, "STATUS") == 0) {
    PLL_Demo_SendStatus();
  } else if (strcmp(line, "START") == 0) {
    PLL_Demo_StartCommand();
  } else if (strcmp(line, "STOP") == 0) {
    PLL_Demo_StopCommand();
  } else if (strncmp(line, "MUL ", 4U) == 0) {
    uint8_t multiplier =
        (line[4] == '1' && line[5] == '\0') ? 1U :
        (line[4] == '2' && line[5] == '\0') ? 2U : 0U;
    if ((multiplier == 0U) ||
        !LockController_SetMultiplier(
            &s_context.controller, multiplier)) {
      PLL_Demo_Send("ERR MUL expects 1 or 2\r\n");
    } else {
      PhaseDetector_ResetFrequency(&s_context.detector);
      PLL_Demo_Send("OK MUL\r\n");
    }
  } else if (strncmp(line, "PHASE ", 6U) == 0) {
    float phase_deg;
    if (!PLL_Demo_ParseDecimal(&line[6], &phase_deg)) {
      PLL_Demo_Send("ERR PHASE expects degrees\r\n");
    } else {
      LockController_SetTargetPhaseDeg(
          &s_context.controller, phase_deg);
      PLL_Demo_Send("OK PHASE\r\n");
    }
  } else if (line[0] != '\0') {
    PLL_Demo_Send("ERR UNKNOWN; use HELP\r\n");
  }
}

static bool PLL_Demo_PopRxByte(uint8_t *byte)
{
  uint16_t tail;

  if (byte == NULL) {
    return false;
  }

  tail = s_uart_rx_tail;
  __DMB();
  if (tail == s_uart_rx_head) {
    return false;
  }

  *byte = s_uart_rx_ring[tail];
  s_uart_rx_tail = (uint16_t)((tail + 1U) & PLL_RX_RING_MASK);
  __DMB();
  return true;
}

static void PLL_Demo_ProcessSerial(void)
{
  uint8_t byte;

  while (PLL_Demo_PopRxByte(&byte)) {
    if ((byte == '\r') || (byte == '\n')) {
      if (s_command_length != 0U) {
        s_command_line[s_command_length] = '\0';
        PLL_Demo_HandleCommand(s_command_line);
        s_command_length = 0U;
      }
    } else if ((byte == '\b') || (byte == 0x7FU)) {
      if (s_command_length != 0U) {
        s_command_length--;
      }
    } else if ((byte >= 0x20U) && (byte <= 0x7EU)) {
      if (s_command_length < (PLL_COMMAND_LINE_SIZE - 1U)) {
        s_command_line[s_command_length++] = (char)byte;
      } else {
        s_command_length = 0U;
        PLL_Demo_Send("ERR LINE TOO LONG\r\n");
      }
    }
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

  if (!s_context.running ||
      (s_status.state != PLL_DEMO_ACQUIRING) ||
      ((uint32_t)(now - s_context.acquire_start_tick) <
       PLL_ACQUIRE_RESTART_MS)) {
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
  s_status.fine_mode = s_context.control.fine_mode;
  s_status.direct_phase_mode =
      s_context.control.direct_phase_mode;
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
  s_status.uart_overflow_count = s_uart_overflow_count;
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

  if (HAL_UART_Receive_IT(&huart1, &s_uart_rx_byte, 1U) != HAL_OK) {
    s_status.state = PLL_DEMO_ERROR;
    return HAL_ERROR;
  }
  PLL_Demo_Send("\r\nAD9910 1x/2x phase-lock demo ready\r\n");
  PLL_Demo_SendHelp();

  if (!PLL_Demo_ApplyDDS(PLL_INITIAL_DDS_FREQUENCY_HZ,
                         0.0f,
                         true, true) ||
      !PLL_Demo_StartSampling(PLL_SEARCH_HIGH_RATE_HZ, false)) {
    PLL_Demo_EnterError("INIT");
    return HAL_ERROR;
  }

  s_context.running = true;
  s_context.initialized = true;
  s_context.last_valid_tick = HAL_GetTick();
  s_status.state = PLL_DEMO_SEARCHING;
  PLL_Demo_UpdateStatus();
  return HAL_OK;
}

void PLL_Demo_Process(void)
{
  uint8_t half;
  uint32_t processed = 0U;

  if (!s_context.initialized) {
    return;
  }

  PLL_Demo_ProcessSerial();

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

void PLL_Demo_UartRxCpltISR(UART_HandleTypeDef *huart)
{
  if ((huart != NULL) && (huart->Instance == USART1)) {
    uint16_t head = s_uart_rx_head;
    uint16_t next =
        (uint16_t)((head + 1U) & PLL_RX_RING_MASK);

    if (next == s_uart_rx_tail) {
      s_uart_overflow_count++;
    } else {
      s_uart_rx_ring[head] = s_uart_rx_byte;
      __DMB();
      s_uart_rx_head = next;
    }
    (void)HAL_UART_Receive_IT(&huart1, &s_uart_rx_byte, 1U);
  }
}

void PLL_Demo_UartTxCpltISR(UART_HandleTypeDef *huart)
{
  if ((huart != NULL) && (huart->Instance == USART1)) {
    s_uart_tx_tail =
        (uint16_t)((s_uart_tx_tail + 1U) & PLL_TX_RING_MASK);
    s_uart_tx_busy = false;
    __DMB();
    PLL_Demo_TxKick();
  }
}

void PLL_Demo_UartErrorISR(UART_HandleTypeDef *huart)
{
  if ((huart != NULL) && (huart->Instance == USART1)) {
    s_uart_overflow_count++;
    __HAL_UART_CLEAR_OREFLAG(huart);
    (void)HAL_UART_Receive_IT(&huart1, &s_uart_rx_byte, 1U);
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

void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
  PLL_Demo_UartRxCpltISR(huart);
}

void HAL_UART_TxCpltCallback(UART_HandleTypeDef *huart)
{
  PLL_Demo_UartTxCpltISR(huart);
}

void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart)
{
  PLL_Demo_UartErrorISR(huart);
}
