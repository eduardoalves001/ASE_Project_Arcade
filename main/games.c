#include "games.h"
#include "arcade_state.h"
#include "board_pins.h"
#include "storage_sd.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_sleep.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "Game";

#define TICK_MS            30        /* ~33 Hz game loop */
#define IDLE_SLEEP_MS      30000     /* menu idle before light sleep */
#define BTN_DEBOUNCE_MS    120
#define SNAKE_STEP_NORMAL  6         /* ticks per snake move */
#define SNAKE_STEP_HARD    4

/* Forward declarations (functions mutate g_state; caller already holds the mutex). */
static void end_game(void);
static void start_game(game_id_t g);
static void go_menu(void);

/* ----- ask the network task to publish a status update right away ----- */
static void publish_now(void)
{
    if (net_task_handle != NULL) {
        xTaskNotifyGive(net_task_handle);
    }
}

/* ============================ SNAKE ============================ */

static void snake_place_food(snake_t *s)
{
    for (int tries = 0; tries < 256; tries++) {
        int fx = rng_next(&s->rng) % SNAKE_COLS;
        int fy = rng_next(&s->rng) % SNAKE_ROWS;
        bool occupied = false;
        for (int i = 0; i < s->len; i++) {
            if (s->x[i] == fx && s->y[i] == fy) { occupied = true; break; }
        }
        if (!occupied) { s->food_x = fx; s->food_y = fy; return; }
    }
    s->food_x = 0; s->food_y = 0;
}

static void reset_snake(void)
{
    snake_t *s = &g_state.snake;
    s->rng      = now_ms() * 2654435761u + 1u;
    s->len      = 3;
    int cx = SNAKE_COLS / 2, cy = SNAKE_ROWS / 2;
    for (int i = 0; i < s->len; i++) { s->x[i] = cx - i; s->y[i] = cy; }
    s->dir      = 1;  /* moving right */
    s->next_dir = 1;
    s->grew     = false;
    s->step_id  = 0;
    s->tick_acc = 0;
    s->tail_x   = s->x[s->len - 1];
    s->tail_y   = s->y[s->len - 1];
    snake_place_food(s);
}

/* Advance the snake one cell. Returns false if it died. */
static bool snake_step(void)
{
    snake_t *s = &g_state.snake;

    /* apply queued direction unless it is a 180-degree reversal */
    if ((s->next_dir + 2) % 4 != s->dir) s->dir = s->next_dir;

    int nx = s->x[0], ny = s->y[0];
    switch (s->dir) {
        case 0: ny--; break;
        case 1: nx++; break;
        case 2: ny++; break;
        case 3: nx--; break;
    }

    if (nx < 0 || nx >= SNAKE_COLS || ny < 0 || ny >= SNAKE_ROWS) return false;

    bool eat = (nx == s->food_x && ny == s->food_y);

    /* the tail cell moves away unless we are growing this step */
    int check_len = s->len - (eat ? 0 : 1);
    for (int i = 0; i < check_len; i++) {
        if (s->x[i] == nx && s->y[i] == ny) return false;  /* hit own body */
    }

    s->tail_x = s->x[s->len - 1];
    s->tail_y = s->y[s->len - 1];

    if (eat) {
        if (s->len < SNAKE_MAX_LEN) s->len++;
        s->grew = true;
    } else {
        s->grew = false;
    }

    for (int i = s->len - 1; i > 0; i--) { s->x[i] = s->x[i - 1]; s->y[i] = s->y[i - 1]; }
    s->x[0] = nx; s->y[0] = ny;

    if (eat) { g_state.score++; snake_place_food(s); }
    s->step_id++;
    return true;
}

/* ============================ PONG ============================ */

static void pong_serve(pong_t *p, int dir)
{
    p->ball_x  = SCR_W / 2;
    p->ball_y  = SCR_H / 2;
    p->ball_vx = dir;   /* -1 or +1 */
    p->ball_vy = 1;
}

static void reset_pong(void)
{
    pong_t *p = &g_state.pong;
    p->player_y     = (SCR_H - PONG_PADDLE_H) / 2;
    p->ai_y         = p->player_y;
    p->player_score = 0;
    p->ai_score     = 0;     /* counts player misses (= lost lives) */
    pong_serve(p, 1);
}

/* Survival pong: rally as long as you can, 5 misses ends the run.
 * g_state.score = successful returns. */
static void pong_update(int pot)
{
    pong_t *p = &g_state.pong;
    int speed    = g_state.hard_mode ? 3 : 2;
    int ai_speed = 2;

    p->player_y = (pot * (SCR_H - PONG_PADDLE_H)) / 100;

    int target = p->ball_y - PONG_PADDLE_H / 2;
    if (p->ai_y < target) p->ai_y += ai_speed;
    else if (p->ai_y > target) p->ai_y -= ai_speed;
    if (p->ai_y < 0) p->ai_y = 0;
    if (p->ai_y > SCR_H - PONG_PADDLE_H) p->ai_y = SCR_H - PONG_PADDLE_H;

    p->ball_x += p->ball_vx * speed;
    p->ball_y += p->ball_vy * speed;

    if (p->ball_y <= 0)                { p->ball_y = 0;               p->ball_vy = 1;  }
    if (p->ball_y >= SCR_H - PONG_BALL){ p->ball_y = SCR_H - PONG_BALL; p->ball_vy = -1; }

    /* player paddle on the left */
    if (p->ball_vx < 0 && p->ball_x <= 4 + PONG_PADDLE_W && p->ball_x > 0) {
        if (p->ball_y + PONG_BALL >= p->player_y &&
            p->ball_y <= p->player_y + PONG_PADDLE_H) {
            p->ball_vx = 1;
            p->ball_x  = 4 + PONG_PADDLE_W;
            g_state.score++;
            int rel = p->ball_y - p->player_y;
            if (rel < PONG_PADDLE_H / 3)        p->ball_vy = -1;
            else if (rel > 2 * PONG_PADDLE_H / 3) p->ball_vy = 1;
        }
    }

    /* AI paddle on the right */
    int rx = SCR_W - 4 - PONG_PADDLE_W;
    if (p->ball_vx > 0 && p->ball_x + PONG_BALL >= rx && p->ball_x < SCR_W) {
        if (p->ball_y + PONG_BALL >= p->ai_y &&
            p->ball_y <= p->ai_y + PONG_PADDLE_H) {
            p->ball_vx = -1;
            p->ball_x  = rx - PONG_BALL;
        }
    }

    if (p->ball_x < 0) {                    /* player missed */
        p->ai_score++;
        if (p->ai_score >= PONG_WIN_SCORE) { end_game(); return; }
        pong_serve(p, 1);
    } else if (p->ball_x > SCR_W) {         /* AI missed: keep rallying */
        pong_serve(p, -1);
    }
}

/* ============================ DINO RUN ============================ */

static void dino_spawn(dino_t *d, int idx, int from_x)
{
    d->obs_x[idx] = from_x;
    d->obs_h[idx] = 8 + (rng_next(&d->rng) % 10);   /* 8..17 px tall */
}

static void reset_dino(void)
{
    dino_t *d = &g_state.dino;
    d->rng       = now_ms() * 2246822519u + 1u;
    d->y         = DINO_GROUND_Y - DINO_H;
    d->vy        = 0;
    d->on_ground = true;
    d->speed     = 3;
    d->dist      = 0;
    int sx = SCR_W + 10;
    for (int i = 0; i < DINO_NUM_OBS; i++) {
        dino_spawn(d, i, sx);
        sx = d->obs_x[i] + 50 + (rng_next(&d->rng) % 40);
    }
}

static void dino_update(void)
{
    dino_t *d = &g_state.dino;
    int spd = d->speed + (g_state.hard_mode ? 2 : 0) + (int)(d->dist / 600);
    if (spd > 9) spd = 9;

    /* gravity / jump arc */
    d->vy += 1;
    d->y  += d->vy;
    if (d->y >= DINO_GROUND_Y - DINO_H) {
        d->y = DINO_GROUND_Y - DINO_H;
        d->vy = 0;
        d->on_ground = true;
    } else {
        d->on_ground = false;
    }

    for (int i = 0; i < DINO_NUM_OBS; i++) {
        d->obs_x[i] -= spd;
        if (d->obs_x[i] < -DINO_OBS_W) {
            int maxx = 0;
            for (int j = 0; j < DINO_NUM_OBS; j++) if (d->obs_x[j] > maxx) maxx = d->obs_x[j];
            dino_spawn(d, i, maxx + 45 + (int)(rng_next(&d->rng) % 55));
        }
        int ox = d->obs_x[i];
        int oy = DINO_GROUND_Y - d->obs_h[i];
        if (DINO_X < ox + DINO_OBS_W && DINO_X + DINO_W > ox && d->y + DINO_H > oy) {
            end_game();
            return;
        }
    }

    d->dist += spd;
    g_state.score = d->dist / 5;
}

/* ============================ State transitions ============================ */

static void end_game(void)
{
    game_id_t g = g_state.current_game;
    g_state.new_high = false;
    if (g_state.score > g_state.high[g]) {
        g_state.high[g] = g_state.score;
        g_state.new_high = true;
        save_high_scores(g_state.high);
    }
    g_state.screen     = SCREEN_GAMEOVER;
    g_state.dirty_full = true;
    publish_now();
}

static void start_game(game_id_t g)
{
    g_state.current_game = g;
    g_state.menu_index   = g;
    g_state.score        = 0;
    g_state.new_high     = false;
    switch (g) {
        case GAME_SNAKE: reset_snake(); break;
        case GAME_PONG:  reset_pong();  break;
        case GAME_DINO:  reset_dino();  break;
    }
    g_state.screen     = SCREEN_PLAY;
    g_state.dirty_full = true;
    publish_now();
}

static void go_menu(void)
{
    g_state.screen     = SCREEN_MENU;
    g_state.dirty_full = true;
    publish_now();
}

/* ============================ Input handling ============================ */

static void handle_event(uint32_t evt)
{
    screen_t s = g_state.screen;

    if (evt >= EVT_REMOTE_BASE) {
        switch (evt) {
            case REMOTE_MENU:         go_menu();               break;
            case REMOTE_START_SNAKE:  start_game(GAME_SNAKE);  break;
            case REMOTE_START_PONG:   start_game(GAME_PONG);   break;
            case REMOTE_START_DINO:   start_game(GAME_DINO);   break;
            case REMOTE_RESET_SCORES:
                for (int i = 0; i < NUM_GAMES; i++) g_state.high[i] = 0;
                save_high_scores(g_state.high);
                publish_now();
                break;
            case REMOTE_SELECT:
                if (s == SCREEN_MENU)          start_game((game_id_t)g_state.menu_index);
                else if (s == SCREEN_GAMEOVER) start_game(g_state.current_game);
                break;
        }
        return;
    }

    if (s == SCREEN_MENU) {
        if (evt == BUTTON_A_GPIO) start_game((game_id_t)g_state.menu_index);
    } else if (s == SCREEN_PLAY) {
        if (evt == BUTTON_B_GPIO) { end_game(); return; }   /* B = give up */
        if (g_state.current_game == GAME_SNAKE) {
            if (evt == BUTTON_A_GPIO)      g_state.snake.next_dir = (g_state.snake.dir + 3) % 4; /* left */
            else if (evt == BUTTON_C_GPIO) g_state.snake.next_dir = (g_state.snake.dir + 1) % 4; /* right */
        } else if (g_state.current_game == GAME_DINO) {
            if (evt == BUTTON_A_GPIO && g_state.dino.on_ground) {
                g_state.dino.vy = -8;
                g_state.dino.on_ground = false;
            }
        }
        /* Pong is controlled by the potentiometer only */
    } else if (s == SCREEN_GAMEOVER) {
        if (evt == BUTTON_A_GPIO)                                  start_game(g_state.current_game);
        else if (evt == BUTTON_B_GPIO || evt == BUTTON_C_GPIO)     go_menu();
    }
}

/* ============================ Light sleep ============================ */

static void enter_light_sleep(uint32_t *last_input_ms)
{
    ESP_LOGI(TAG, "Idle on menu -> light sleep. Press button C to wake.");

    xSemaphoreTake(state_mutex, portMAX_DELAY);
    g_state.is_sleeping = true;
    g_state.screen      = SCREEN_SLEEP;
    g_state.dirty_full  = true;
    xSemaphoreGive(state_mutex);
    publish_now();

    vTaskDelay(pdMS_TO_TICKS(150));   /* let the display paint the sleep screen */

    gpio_hold_en(PIN_BL);             /* freeze backlight so it does not flicker */
    gpio_wakeup_enable(BUTTON_C_GPIO, GPIO_INTR_LOW_LEVEL);
    esp_sleep_enable_gpio_wakeup();
    esp_light_sleep_start();

    gpio_wakeup_disable(BUTTON_C_GPIO);
    gpio_hold_dis(PIN_BL);

    xSemaphoreTake(state_mutex, portMAX_DELAY);
    g_state.is_sleeping = false;
    g_state.screen      = SCREEN_MENU;
    g_state.dirty_full  = true;
    xSemaphoreGive(state_mutex);

    *last_input_ms = now_ms();
    publish_now();

    /* discard the wake button press so it does not start a game */
    uint32_t e;
    while (xQueueReceive(button_evt_queue, &e, 0)) { }
}

/* ============================ Main game task ============================ */

void game_task(void *pvParameters)
{
    static uint32_t last_btn_ms[40] = {0};
    uint32_t last_input_ms = now_ms();

    xSemaphoreTake(state_mutex, portMAX_DELAY);
    g_state.screen     = SCREEN_MENU;
    g_state.dirty_full = true;
    xSemaphoreGive(state_mutex);

    while (1) {
        int pot = adc_read_percent();
        bool sleepy = false;

        xSemaphoreTake(state_mutex, portMAX_DELAY);

        /* 1. drain and debounce input events */
        uint32_t evt;
        while (xQueueReceive(button_evt_queue, &evt, 0)) {
            if (evt < EVT_REMOTE_BASE && evt < 40) {
                uint32_t t = now_ms();
                if (t - last_btn_ms[evt] < BTN_DEBOUNCE_MS) continue;
                last_btn_ms[evt] = t;
            }
            last_input_ms = now_ms();
            handle_event(evt);
        }

        /* 2. advance simulation */
        g_state.frame++;
        g_state.hard_mode = (g_state.temp >= HARD_TEMP_C);

        if (g_state.screen == SCREEN_MENU) {
            int idx = (pot * NUM_GAMES) / 101;
            if (idx < 0) idx = 0;
            if (idx >= NUM_GAMES) idx = NUM_GAMES - 1;
            g_state.menu_index   = idx;
            g_state.current_game = (game_id_t)idx;
        } else if (g_state.screen == SCREEN_PLAY) {
            switch (g_state.current_game) {
                case GAME_SNAKE: {
                    int step = g_state.hard_mode ? SNAKE_STEP_HARD : SNAKE_STEP_NORMAL;
                    if (++g_state.snake.tick_acc >= step) {
                        g_state.snake.tick_acc = 0;
                        if (!snake_step()) end_game();
                    }
                    break;
                }
                case GAME_PONG: pong_update(pot); break;
                case GAME_DINO: dino_update();    break;
            }
        }

        if (g_state.screen == SCREEN_MENU && !g_state.is_sleeping &&
            (now_ms() - last_input_ms) > IDLE_SLEEP_MS) {
            sleepy = true;
        }

        /* status LED warns when ambient heat has enabled hard mode */
        gpio_set_level(LED_GPIO, g_state.hard_mode ? 1 : 0);

        xSemaphoreGive(state_mutex);

        if (sleepy) enter_light_sleep(&last_input_ms);

        vTaskDelay(pdMS_TO_TICKS(TICK_MS));
    }
}
