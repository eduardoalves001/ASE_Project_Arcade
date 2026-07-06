#ifndef ARCADE_STATE_H
#define ARCADE_STATE_H

/*
 * Shared application state for the ESP-Arcade console.
 *
 * One arcade_state_t instance (g_state) is the single source of truth that the
 * game, display and network FreeRTOS tasks read and write. Every access
 * must be wrapped by state_mutex. Button presses arrive as GPIO numbers (or
 * REMOTE_* sentinels) through button_evt_queue from the ISR / MQTT layer.
 */

#include <stdint.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "esp_adc/adc_oneshot.h"

/* ---- Display geometry (ST7735 landscape) ---- */
#define SCR_W            160
#define SCR_H            80

/* ---- Games ---- */
#define NUM_GAMES        3
typedef enum {
    GAME_FLAPPY = 0,
    GAME_PONG   = 1,
    GAME_DINO   = 2,
} game_id_t;

typedef enum {
    SCREEN_MENU     = 0,
    SCREEN_PLAY     = 1,
    SCREEN_GAMEOVER = 2,
    SCREEN_SLEEP    = 3,
} screen_t;

/* ---- Button / remote events pushed into button_evt_queue ----
 * Physical buttons are pushed as their GPIO numbers by the ISR.
 * Remote (MQTT) commands use sentinels >= EVT_REMOTE_BASE. */
#define EVT_REMOTE_BASE 1000
enum {
    REMOTE_MENU         = EVT_REMOTE_BASE + 0,
    REMOTE_START_FLAPPY = EVT_REMOTE_BASE + 1,
    REMOTE_START_PONG   = EVT_REMOTE_BASE + 2,
    REMOTE_START_DINO   = EVT_REMOTE_BASE + 3,
    REMOTE_RESET_SCORES = EVT_REMOTE_BASE + 4,
    REMOTE_SELECT       = EVT_REMOTE_BASE + 5,  /* behaves like Button A */
};

/* ===================== Flappy ===================== */
#define FLAPPY_BIRD_X    30
#define FLAPPY_BIRD_W    8
#define FLAPPY_BIRD_H    6
#define FLAPPY_TOP       10          /* play area starts below the score header */
#define FLAPPY_NUM_PIPES 2
#define FLAPPY_PIPE_W    10
#define FLAPPY_GAP_H     28

typedef struct {
    int      y_fp;                      /* bird top y, fixed point (x8) */
    int      vy;                        /* vertical velocity, fixed point (x8) */
    int      pipe_x[FLAPPY_NUM_PIPES];
    int      gap_y[FLAPPY_NUM_PIPES];   /* top of the gap opening */
    bool     passed[FLAPPY_NUM_PIPES];  /* pipe already scored */
    bool     started;                   /* physics frozen until the first flap */
    uint32_t rng;                       /* xorshift state for gap placement */
} flappy_t;

/* ===================== Pong ===================== */
#define PONG_PADDLE_H    18
#define PONG_PADDLE_W    3
#define PONG_BALL        3
#define PONG_WIN_SCORE   5

typedef struct {
    int ball_x, ball_y;
    int ball_vx, ball_vy;
    int player_y;           /* left paddle top, driven by potentiometer */
    int ai_y;               /* right paddle top */
    int player_score, ai_score;
} pong_t;

/* ===================== Dino runner ===================== */
#define DINO_GROUND_Y    66          /* y of the ground line */
#define DINO_X           18
#define DINO_W           10
#define DINO_H           12
#define DINO_NUM_OBS     3
#define DINO_OBS_W       6

typedef struct {
    int      y;             /* top of the dino sprite */
    int      vy;            /* vertical velocity (fixed point /10) */
    bool     on_ground;
    int      obs_x[DINO_NUM_OBS];
    int      obs_h[DINO_NUM_OBS];
    int      speed;         /* horizontal scroll px per tick */
    uint32_t dist;          /* score (distance run) */
    uint32_t rng;
} dino_t;

/* ===================== Whole-console state ===================== */
typedef struct {
    screen_t  screen;
    game_id_t current_game;     /* selected in menu / active in play */
    int       menu_index;       /* 0..NUM_GAMES-1 */
    uint32_t  score;            /* score of the current run */
    uint32_t  high[NUM_GAMES];  /* persisted high scores */
    bool      new_high;         /* the run that just ended beat the record */

    bool      is_sleeping;
    uint32_t  frame;            /* free-running game tick counter */
    bool      dirty_full;       /* renderer must repaint the whole screen */

    flappy_t  flappy;
    pong_t    pong;
    dino_t    dino;
} arcade_state_t;

/* ---- Globals (defined in arcade_state.c) ---- */
extern arcade_state_t        g_state;
extern SemaphoreHandle_t     state_mutex;
extern QueueHandle_t         button_evt_queue;
extern adc_oneshot_unit_handle_t adc1_handle;
extern TaskHandle_t          net_task_handle;

/* ---- Small helpers ---- */
uint32_t    now_ms(void);
int         adc_read_percent(void);            /* potentiometer 0..100 */
uint32_t    rng_next(uint32_t *state);         /* xorshift32 */
const char *game_name(game_id_t g);

#endif /* ARCADE_STATE_H */
