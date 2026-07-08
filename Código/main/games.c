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

#define TICK_MS 30          /* ~33 Hz game loop */
#define IDLE_SLEEP_MS 30000 /* menu idle before light sleep */
#define BTN_DEBOUNCE_MS 120
#define HOLD_MENU_MS 5000 /* hold Button A this long to exit to the menu */

/* Forward declarations (functions mutate g_state; caller already holds the mutex). */
static void end_game(void);
static void start_game(game_id_t g);
static void go_menu(void);

/* ----- ask the network task to publish a status update right away ----- */
static void publish_now(void)
{
    if (net_task_handle != NULL)
    {
        xTaskNotifyGive(net_task_handle);
    }
}

/* ============================ FLAPPY ============================ */

static int flappy_random_gap(flappy_t *f)
{
    /* keep the whole gap inside the play area with a small margin */
    return FLAPPY_TOP + 4 +
           (int)(rng_next(&f->rng) % (SCR_H - FLAPPY_GAP_H - FLAPPY_TOP - 10));
}

static void reset_flappy(void)
{
    flappy_t *f = &g_state.flappy;
    f->rng = now_ms() * 2654435761u + 1u;
    f->y_fp = ((SCR_H / 2) - FLAPPY_BIRD_H) << 3;
    f->vy = 0;
    f->started = false; /* bird hovers until the first flap */
    for (int i = 0; i < FLAPPY_NUM_PIPES; i++)
    {
        f->pipe_x[i] = SCR_W + 20 + i * 90;
        f->gap_y[i] = flappy_random_gap(f);
        f->passed[i] = false;
    }
}

static void flappy_update(void)
{
    flappy_t *f = &g_state.flappy;
    if (!f->started)
        return;

    int spd = 2;

    f->vy += 3;
    if (f->vy > 28)
        f->vy = 28;
    f->y_fp += f->vy;

    if ((f->y_fp >> 3) < FLAPPY_TOP)
    {
        f->y_fp = FLAPPY_TOP << 3;
        f->vy = 0;
    }
    int by = f->y_fp >> 3;
    if (by + FLAPPY_BIRD_H >= SCR_H)
    {
        end_game();
        return;
    }

    for (int i = 0; i < FLAPPY_NUM_PIPES; i++)
    {
        f->pipe_x[i] -= spd;
        if (f->pipe_x[i] < -FLAPPY_PIPE_W)
        {
            int maxx = 0;
            for (int j = 0; j < FLAPPY_NUM_PIPES; j++)
                if (f->pipe_x[j] > maxx)
                    maxx = f->pipe_x[j];
            f->pipe_x[i] = maxx + 90;
            f->gap_y[i] = flappy_random_gap(f);
            f->passed[i] = false;
        }

        if (!f->passed[i] && f->pipe_x[i] + FLAPPY_PIPE_W < FLAPPY_BIRD_X)
        {
            f->passed[i] = true;
            g_state.score++;
        }

        int px = f->pipe_x[i];
        if (FLAPPY_BIRD_X < px + FLAPPY_PIPE_W && FLAPPY_BIRD_X + FLAPPY_BIRD_W > px &&
            (by < f->gap_y[i] || by + FLAPPY_BIRD_H > f->gap_y[i] + FLAPPY_GAP_H))
        {
            end_game();
            return;
        }
    }
}

/* ============================ PONG ============================ */

static void pong_serve(pong_t *p, int dir)
{
    p->ball_x = SCR_W / 2;
    p->ball_y = SCR_H / 2;
    p->ball_vx = dir;
    p->ball_vy = 1;
}

static void reset_pong(void)
{
    pong_t *p = &g_state.pong;
    p->player_y = (SCR_H - PONG_PADDLE_H) / 2;
    p->ai_y = p->player_y;
    p->player_score = 0;
    p->ai_score = 0;
    pong_serve(p, 1);
}

/*Caso morra 5 vezes, dá game over */
static void pong_update(int pot)
{
    pong_t *p = &g_state.pong;
    int speed = 2;
    int ai_speed = 2;

    p->player_y = (pot * (SCR_H - PONG_PADDLE_H)) / 100;

    int target = p->ball_y - PONG_PADDLE_H / 2;
    if (p->ai_y < target)
        p->ai_y += ai_speed;
    else if (p->ai_y > target)
        p->ai_y -= ai_speed;
    if (p->ai_y < 0)
        p->ai_y = 0;
    if (p->ai_y > SCR_H - PONG_PADDLE_H)
        p->ai_y = SCR_H - PONG_PADDLE_H;

    p->ball_x += p->ball_vx * speed;
    p->ball_y += p->ball_vy * speed;

    if (p->ball_y <= 0)
    {
        p->ball_y = 0;
        p->ball_vy = 1;
    }
    if (p->ball_y >= SCR_H - PONG_BALL)
    {
        p->ball_y = SCR_H - PONG_BALL;
        p->ball_vy = -1;
    }

    if (p->ball_vx < 0 && p->ball_x <= 4 + PONG_PADDLE_W && p->ball_x > 0)
    {
        if (p->ball_y + PONG_BALL >= p->player_y &&
            p->ball_y <= p->player_y + PONG_PADDLE_H)
        {
            p->ball_vx = 1;
            p->ball_x = 4 + PONG_PADDLE_W;
            g_state.score++;
            int rel = p->ball_y - p->player_y;
            if (rel < PONG_PADDLE_H / 3)
                p->ball_vy = -1;
            else if (rel > 2 * PONG_PADDLE_H / 3)
                p->ball_vy = 1;
        }
    }

    int rx = SCR_W - 4 - PONG_PADDLE_W;
    if (p->ball_vx > 0 && p->ball_x + PONG_BALL >= rx && p->ball_x < SCR_W)
    {
        if (p->ball_y + PONG_BALL >= p->ai_y &&
            p->ball_y <= p->ai_y + PONG_PADDLE_H)
        {
            p->ball_vx = -1;
            p->ball_x = rx - PONG_BALL;
        }
    }

    if (p->ball_x < 0)
    {
        p->ai_score++;
        if (p->ai_score >= PONG_WIN_SCORE)
        {
            end_game();
            return;
        }
        pong_serve(p, 1);
    }
    else if (p->ball_x > SCR_W)
    {
        pong_serve(p, -1);
    }
}

/* ============================ DINO RUN ============================ */

static void dino_spawn(dino_t *d, int idx, int from_x)
{
    d->obs_x[idx] = from_x;
    d->obs_h[idx] = 8 + (rng_next(&d->rng) % 10);
}

static void reset_dino(void)
{
    dino_t *d = &g_state.dino;
    d->rng = now_ms() * 2246822519u + 1u;
    d->y = DINO_GROUND_Y - DINO_H;
    d->vy = 0;
    d->on_ground = true;
    d->speed = 3;
    d->dist = 0;
    int sx = SCR_W + 10;
    for (int i = 0; i < DINO_NUM_OBS; i++)
    {
        dino_spawn(d, i, sx);
        sx = d->obs_x[i] + 50 + (rng_next(&d->rng) % 40);
    }
}

static void dino_update(void)
{
    dino_t *d = &g_state.dino;
    int spd = d->speed + (int)(d->dist / 600);
    if (spd > 9)
        spd = 9;

    d->vy += 1;
    d->y += d->vy;
    if (d->y >= DINO_GROUND_Y - DINO_H)
    {
        d->y = DINO_GROUND_Y - DINO_H;
        d->vy = 0;
        d->on_ground = true;
    }
    else
    {
        d->on_ground = false;
    }

    for (int i = 0; i < DINO_NUM_OBS; i++)
    {
        d->obs_x[i] -= spd;
        if (d->obs_x[i] < -DINO_OBS_W)
        {
            int maxx = 0;
            for (int j = 0; j < DINO_NUM_OBS; j++)
                if (d->obs_x[j] > maxx)
                    maxx = d->obs_x[j];
            dino_spawn(d, i, maxx + 45 + (int)(rng_next(&d->rng) % 55));
        }
        int ox = d->obs_x[i];
        int oy = DINO_GROUND_Y - d->obs_h[i];
        if (DINO_X < ox + DINO_OBS_W && DINO_X + DINO_W > ox && d->y + DINO_H > oy)
        {
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
    if (g_state.score > g_state.high[g])
    {
        g_state.high[g] = g_state.score;
        g_state.new_high = true;
        save_high_scores(g_state.high);
    }
    g_state.screen = SCREEN_GAMEOVER;
    g_state.dirty_full = true;
    publish_now();
}

static void start_game(game_id_t g)
{
    g_state.current_game = g;
    g_state.menu_index = g;
    g_state.score = 0;
    g_state.new_high = false;
    switch (g)
    {
    case GAME_FLAPPY:
        reset_flappy();
        break;
    case GAME_PONG:
        reset_pong();
        break;
    case GAME_DINO:
        reset_dino();
        break;
    }
    g_state.screen = SCREEN_PLAY;
    g_state.dirty_full = true;
    publish_now();
}

static void go_menu(void)
{
    g_state.screen = SCREEN_MENU;
    g_state.dirty_full = true;
    publish_now();
}

/* ============================ Input handling ============================ */

static void play_primary_action(void)
{
    if (g_state.current_game == GAME_FLAPPY)
    {
        g_state.flappy.started = true;
        g_state.flappy.vy = -22;
    }
    else if (g_state.current_game == GAME_DINO)
    {
        if (g_state.dino.on_ground)
        {
            g_state.dino.vy = -8;
            g_state.dino.on_ground = false;
        }
    }
}

static void handle_event(uint32_t evt)
{
    screen_t s = g_state.screen;

    if (evt >= EVT_REMOTE_BASE)
    {
        switch (evt)
        {
        case REMOTE_MENU:
            go_menu();
            break;
        case REMOTE_START_FLAPPY:
            start_game(GAME_FLAPPY);
            break;
        case REMOTE_START_PONG:
            start_game(GAME_PONG);
            break;
        case REMOTE_START_DINO:
            start_game(GAME_DINO);
            break;
        case REMOTE_RESET_SCORES:
            for (int i = 0; i < NUM_GAMES; i++)
                g_state.high[i] = 0;
            save_high_scores(g_state.high);
            publish_now();
            break;
        case REMOTE_SELECT:
            if (s == SCREEN_MENU)
                start_game((game_id_t)g_state.menu_index);
            else if (s == SCREEN_PLAY)
                play_primary_action();
            else if (s == SCREEN_GAMEOVER)
                start_game(g_state.current_game);
            break;
        }
        return;
    }

    if (s == SCREEN_MENU)
    {
        if (evt == BUTTON_A_GPIO)
            start_game((game_id_t)g_state.menu_index);
    }
    else if (s == SCREEN_PLAY)
    {
        if (evt == BUTTON_A_GPIO)
            play_primary_action();
    }
    else if (s == SCREEN_GAMEOVER)
    {
        if (evt == BUTTON_A_GPIO)
            start_game(g_state.current_game);
    }
}

/* ============================ Light sleep ============================ */

static void enter_light_sleep(uint32_t *last_input_ms)
{
    ESP_LOGI(TAG, "Idle on menu -> light sleep. Press button A to wake.");

    xSemaphoreTake(state_mutex, portMAX_DELAY);
    g_state.is_sleeping = true;
    g_state.screen = SCREEN_SLEEP;
    g_state.dirty_full = true;
    xSemaphoreGive(state_mutex);
    publish_now();

    vTaskDelay(pdMS_TO_TICKS(150));

    gpio_hold_en(PIN_BL);
    gpio_wakeup_enable(BUTTON_A_GPIO, GPIO_INTR_LOW_LEVEL);
    esp_sleep_enable_gpio_wakeup();
    esp_light_sleep_start();

    gpio_wakeup_disable(BUTTON_A_GPIO);
    gpio_hold_dis(PIN_BL);

    xSemaphoreTake(state_mutex, portMAX_DELAY);
    g_state.is_sleeping = false;
    g_state.screen = SCREEN_MENU;
    g_state.dirty_full = true;
    xSemaphoreGive(state_mutex);

    *last_input_ms = now_ms();
    publish_now();

    uint32_t e;
    while (xQueueReceive(button_evt_queue, &e, 0))
    {
    }
}

/* ============================ Main game task ============================ */

void game_task(void *pvParameters)
{
    static uint32_t last_btn_ms[40] = {0};
    uint32_t last_input_ms = now_ms();
    uint32_t hold_acc = 0;
    bool hold_done = false;
    uint32_t led_prev_score = 0;
    uint32_t led_pulse_until = 0;

    xSemaphoreTake(state_mutex, portMAX_DELAY);
    g_state.screen = SCREEN_MENU;
    g_state.dirty_full = true;
    xSemaphoreGive(state_mutex);

    while (1)
    {
        int pot = adc_read_percent();
        bool sleepy = false;

        xSemaphoreTake(state_mutex, portMAX_DELAY);

        if (gpio_get_level(BUTTON_A_GPIO) == 0)
        {
            last_input_ms = now_ms();
            hold_acc += TICK_MS;
        }
        else
        {
            hold_acc = (hold_acc > 3 * TICK_MS) ? hold_acc - 3 * TICK_MS : 0;
            if (hold_acc == 0)
                hold_done = false;
        }
        if (!hold_done && hold_acc >= HOLD_MENU_MS)
        {
            hold_done = true;
            ESP_LOGI(TAG, "Button hold detected -> back to menu");
            if (g_state.screen == SCREEN_PLAY)
            {
                game_id_t g = g_state.current_game;
                if (g_state.score > g_state.high[g])
                {
                    g_state.high[g] = g_state.score;
                    save_high_scores(g_state.high);
                }
            }
            if (g_state.screen != SCREEN_MENU)
                go_menu();
        }

        uint32_t evt;
        while (xQueueReceive(button_evt_queue, &evt, 0))
        {
            if (evt < EVT_REMOTE_BASE && evt < 40)
            {
                uint32_t t = now_ms();
                if (t - last_btn_ms[evt] < BTN_DEBOUNCE_MS)
                    continue;
                last_btn_ms[evt] = t;
            }
            last_input_ms = now_ms();
            handle_event(evt);
        }

        g_state.frame++;

        if (g_state.screen == SCREEN_MENU)
        {
            int idx = (pot * NUM_GAMES) / 101;
            if (idx < 0)
                idx = 0;
            if (idx >= NUM_GAMES)
                idx = NUM_GAMES - 1;
            g_state.menu_index = idx;
            g_state.current_game = (game_id_t)idx;
        }
        else if (g_state.screen == SCREEN_PLAY)
        {
            switch (g_state.current_game)
            {
            case GAME_FLAPPY:
                flappy_update();
                break;
            case GAME_PONG:
                pong_update(pot);
                break;
            case GAME_DINO:
                dino_update();
                break;
            }
        }

        if (g_state.screen == SCREEN_MENU && !g_state.is_sleeping &&
            (now_ms() - last_input_ms) > IDLE_SLEEP_MS)
        {
            sleepy = true;
        }

        if (g_state.screen == SCREEN_PLAY && g_state.score > led_prev_score)
        {
            led_pulse_until = now_ms() + 120;
        }
        led_prev_score = g_state.score;
        gpio_set_level(LED_GPIO, (now_ms() < led_pulse_until) ? 1 : 0);

        xSemaphoreGive(state_mutex);

        if (sleepy)
            enter_light_sleep(&last_input_ms);

        vTaskDelay(pdMS_TO_TICKS(TICK_MS));
    }
}
