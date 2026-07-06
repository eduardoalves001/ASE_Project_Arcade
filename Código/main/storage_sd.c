#include "storage_sd.h"
#include "arcade_state.h"
#include <stdio.h>
#include <string.h>
#include "esp_log.h"

static const char *TAG = "StorageSD";
#define SCORES_PATH "/sdcard/scores.txt"

void load_high_scores(void)
{
    uint32_t hs[NUM_GAMES] = {0, 0, 0};

    FILE *f = fopen(SCORES_PATH, "r");
    if (f) {
        char line[64];
        if (fgets(line, sizeof(line), f) && strncmp(line, "ARC1", 4) == 0) {
            while (fgets(line, sizeof(line), f)) {
                char key[16];
                unsigned long val;
                if (sscanf(line, "%15[^=]=%lu", key, &val) != 2) continue;
                if      (strcmp(key, "flappy") == 0) hs[GAME_FLAPPY] = (uint32_t)val;
                else if (strcmp(key, "pong")   == 0) hs[GAME_PONG]   = (uint32_t)val;
                else if (strcmp(key, "dino")   == 0) hs[GAME_DINO]   = (uint32_t)val;
            }
            ESP_LOGI(TAG, "High scores loaded: flappy=%lu pong=%lu dino=%lu",
                     (unsigned long)hs[0], (unsigned long)hs[1], (unsigned long)hs[2]);
        } else {
            ESP_LOGW(TAG, "scores.txt has no ARC1 header; starting fresh.");
        }
        fclose(f);
    } else {
        ESP_LOGW(TAG, "No scores.txt on the SD card yet; a new file will be created.");
    }

    if (xSemaphoreTake(state_mutex, portMAX_DELAY)) {
        for (int i = 0; i < NUM_GAMES; i++) g_state.high[i] = hs[i];
        xSemaphoreGive(state_mutex);
    }
}

void save_high_scores(const uint32_t high[NUM_GAMES])
{
    FILE *f = fopen(SCORES_PATH, "w");
    if (f) {
        fprintf(f, "ARC1\n");
        fprintf(f, "flappy=%lu\n", (unsigned long)high[GAME_FLAPPY]);
        fprintf(f, "pong=%lu\n",   (unsigned long)high[GAME_PONG]);
        fprintf(f, "dino=%lu\n",   (unsigned long)high[GAME_DINO]);
        fclose(f);
        ESP_LOGI(TAG, "High scores saved: flappy=%lu pong=%lu dino=%lu",
                 (unsigned long)high[0], (unsigned long)high[1], (unsigned long)high[2]);
    } else {
        ESP_LOGE(TAG, "Failed to open %s for writing.", SCORES_PATH);
    }
}
