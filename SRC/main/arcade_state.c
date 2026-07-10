#include "arcade_state.h"
#include "board_pins.h"
#include "esp_timer.h"

#define POT_CAL_LO 80   /* raw at the low stop  -> 0%   */
#define POT_CAL_HI 3030 /* raw at the high stop -> 100% (~74% of 4095) */

/* Single source of truth shared by every task, guarded by state_mutex. */
arcade_state_t g_state;
SemaphoreHandle_t state_mutex = NULL;
QueueHandle_t button_evt_queue = NULL;
adc_oneshot_unit_handle_t adc1_handle = NULL;
TaskHandle_t net_task_handle = NULL;

uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

int adc_read_percent(void)
{
    int raw = 0;
    if (adc1_handle != NULL &&
        adc_oneshot_read(adc1_handle, ADC_CHANNEL, &raw) == ESP_OK)
    {
        /* stretch the usable pot travel (raw POT_CAL_LO..POT_CAL_HI) onto 0..100 */
        int pct = (raw - POT_CAL_LO) * 100 / (POT_CAL_HI - POT_CAL_LO);
        if (pct < 0)
            pct = 0;
        if (pct > 100)
            pct = 100;
        return pct;
    }
    return 0;
}

uint32_t rng_next(uint32_t *state)
{
    uint32_t x = *state ? *state : 0x1234567u;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return x;
}

const char *game_name(game_id_t g)
{
    switch (g)
    {
    case GAME_FLAPPY:
        return "FLAPPY";
    case GAME_PONG:
        return "PONG";
    case GAME_DINO:
        return "DINO RUN";
    default:
        return "?";
    }
}
