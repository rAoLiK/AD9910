#ifndef PLL_CONFIG_H
#define PLL_CONFIG_H

#define PLL_MIN_SAMPLE_RATE_HZ          (10000UL)
#define PLL_MAX_SAMPLE_RATE_HZ          (2400000UL)
#define PLL_TIM2_CLOCK_HZ               (72000000UL)
#define PLL_SAMPLES_PER_DDS_CYCLE       (24.0f)

/*
 * Enter fixed-FTW hold below the reported 60 kHz reference-input problem
 * boundary so normal estimator tolerance cannot leave a nominal 60 kHz input
 * on the dithering PI path.  MUL 2 can reach the same DDS-side sensitivity at
 * a lower reference frequency, so it has a separate output-frequency gate.
 * Both gates use a lower exit threshold for hysteresis.
 */
#define PLL_HIGH_HOLD_ENTER_HZ          (59000.0f)
#define PLL_HIGH_HOLD_EXIT_HZ           (57000.0f)
#define PLL_HIGH_OUTPUT_HOLD_ENTER_HZ   (89000.0f)
#define PLL_HIGH_OUTPUT_HOLD_EXIT_HZ    (87000.0f)

#endif /* PLL_CONFIG_H */
