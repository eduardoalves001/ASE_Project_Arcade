#ifndef STORAGE_SD_H
#define STORAGE_SD_H

#include <stdint.h>
#include "arcade_state.h"

/* Loads scores.txt from the SD card into g_state.high (takes state_mutex). */
void load_high_scores(void);

/* Serializes the three high scores to /sdcard/scores.txt. Caller may hold
 * state_mutex: this function never takes it. */
void save_high_scores(const uint32_t high[NUM_GAMES]);

#endif /* STORAGE_SD_H */
