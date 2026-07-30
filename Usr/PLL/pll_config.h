#ifndef PLL_CONFIG_H
#define PLL_CONFIG_H

#define PLL_MIN_SAMPLE_RATE_HZ          (10000UL)
#define PLL_MAX_SAMPLE_RATE_HZ          (2400000UL)
#define PLL_TIM2_CLOCK_HZ               (72000000UL)
#define PLL_SAMPLES_PER_DDS_CYCLE       (24.0f)

/*
 * Enter fixed-FTW hold below the reported 60 kHz problem boundary so normal
 * estimator tolerance cannot leave a nominal 60 kHz input on the dithering PI
 * path. The lower exit threshold provides hysteresis.
 */
#define PLL_HIGH_HOLD_ENTER_HZ          (59000.0f)
#define PLL_HIGH_HOLD_EXIT_HZ           (57000.0f)

#endif /* PLL_CONFIG_H */
