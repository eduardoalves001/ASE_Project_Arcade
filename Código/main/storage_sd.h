#ifndef STORAGE_SD_H
#define STORAGE_SD_H

#include <stdint.h>
#include "arcade_state.h"

void load_high_scores(void);

void save_high_scores(const uint32_t high[NUM_GAMES]);

#endif
