#include "display_arcade.h"
#include "arcade_state.h"
#include "st7735.h"
#include "graphics.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <string.h>

#define CHAR_W(size)  (6 * (size))   /* ST7735 font is 5px + 1px spacing */

static int center_x(const char *str, uint8_t size)
{
    int w = (int)strlen(str) * CHAR_W(size);
    int x = (SCR_W - w) / 2;
    return x < 0 ? 0 : x;
}

static void draw_centered(uint16_t y, const char *str, uint16_t color, uint16_t bg, uint8_t size)
{
    st7735_draw_string(center_x(str, size), y, str, color, bg, size);
}

/* ============================ MENU ============================ */

static void draw_menu_row(int i, bool selected, const arcade_state_t *s)
{
    int y = 18 + i * 16;
    uint16_t bg   = selected ? ST7735_BLUE  : ST7735_BLACK;
    uint16_t fg   = selected ? ST7735_WHITE : ST7735_GRAY;
    st7735_fill_rect(4, y - 2, SCR_W - 8, 14, bg);

    char line[48];
    snprintf(line, sizeof(line), "%-8s HI:%lu",
             game_name((game_id_t)i), (unsigned long)s->high[i]);
    st7735_draw_string(8, y, line, fg, bg, 1);
}

static void render_menu(const arcade_state_t *s)
{
    static int prev_index = -1;

    if (s->dirty_full) {
        st7735_fill_screen(ST7735_BLACK);
        draw_centered(2, "ESP-ARCADE", ST7735_YELLOW, ST7735_BLACK, 1);
        for (int i = 0; i < NUM_GAMES; i++) draw_menu_row(i, i == s->menu_index, s);
        draw_centered(70, "TURN=PICK  A=START", ST7735_GREEN, ST7735_BLACK, 1);
        prev_index = s->menu_index;
    } else if (s->menu_index != prev_index) {
        draw_menu_row(prev_index, false, s);
        draw_menu_row(s->menu_index, true, s);
        prev_index = s->menu_index;
    }
}

/* ============================ FLAPPY ============================ */

/* Draws (or erases, with black) one pipe pair, clipped to the screen. */
static void flappy_pipe_pair(int x, int gap_y, uint16_t color)
{
    int x0 = x, w = FLAPPY_PIPE_W;
    if (x0 < 0) { w += x0; x0 = 0; }
    if (x0 + w > SCR_W) w = SCR_W - x0;
    if (w <= 0) return;
    st7735_fill_rect(x0, FLAPPY_TOP, w, gap_y - FLAPPY_TOP, color);          /* top pipe */
    st7735_fill_rect(x0, gap_y + FLAPPY_GAP_H, w,
                     SCR_H - gap_y - FLAPPY_GAP_H, color);                    /* bottom pipe */
}

static void render_flappy(const arcade_state_t *s)
{
    static int  prev_by = -1;
    static int  prev_px[FLAPPY_NUM_PIPES];
    static int  prev_gy[FLAPPY_NUM_PIPES];
    static bool prev_pipes_valid = false;
    static bool prev_waiting = false;
    static uint32_t prev_score = 0xFFFFFFFFu;

    if (s->dirty_full) {
        st7735_fill_screen(ST7735_BLACK);
        prev_by = -1;
        prev_pipes_valid = false;
        prev_waiting = false;
        prev_score = 0xFFFFFFFFu;
    }

    /* erase previous pipes and bird */
    if (prev_pipes_valid) {
        for (int i = 0; i < FLAPPY_NUM_PIPES; i++)
            flappy_pipe_pair(prev_px[i], prev_gy[i], ST7735_BLACK);
    }
    if (prev_by >= 0)
        st7735_fill_rect(FLAPPY_BIRD_X, prev_by, FLAPPY_BIRD_W, FLAPPY_BIRD_H, ST7735_BLACK);

    /* draw pipes then the bird on top */
    for (int i = 0; i < FLAPPY_NUM_PIPES; i++) {
        flappy_pipe_pair(s->flappy.pipe_x[i], s->flappy.gap_y[i], ST7735_GREEN);
        prev_px[i] = s->flappy.pipe_x[i];
        prev_gy[i] = s->flappy.gap_y[i];
    }
    prev_pipes_valid = true;

    int by = s->flappy.y_fp >> 3;
    st7735_fill_rect(FLAPPY_BIRD_X, by, FLAPPY_BIRD_W, FLAPPY_BIRD_H, ST7735_YELLOW);
    prev_by = by;

    /* "press A" hint while the run has not started yet */
    if (!s->flappy.started) {
        draw_centered(64, "PRESS A", ST7735_CYAN, ST7735_BLACK, 1);
        prev_waiting = true;
    } else if (prev_waiting) {
        st7735_fill_rect(0, 64, SCR_W, 8, ST7735_BLACK);
        prev_waiting = false;
    }

    if (s->score != prev_score) {
        char h[48];
        snprintf(h, sizeof(h), "FLAPPY %lu", (unsigned long)s->score);
        st7735_fill_rect(0, 0, SCR_W, FLAPPY_TOP, ST7735_BLACK);
        st7735_draw_string(2, 1, h, ST7735_YELLOW, ST7735_BLACK, 1);
        prev_score = s->score;
    }
}

/* ============================ PONG ============================ */

static void render_pong(const arcade_state_t *s)
{
    static int prev_ball_x = -1, prev_ball_y = -1;
    static int prev_py = -1, prev_ay = -1;
    static int prev_score = -1, prev_lives = -1;

    int lives = PONG_WIN_SCORE - s->pong.ai_score;
    int rx = SCR_W - 4 - PONG_PADDLE_W;

    if (s->dirty_full) {
        st7735_fill_screen(ST7735_BLACK);
        prev_ball_x = prev_ball_y = prev_py = prev_ay = -1;
        prev_score = prev_lives = -1;
    }

    /* erase old sprites */
    if (prev_ball_x >= 0)
        st7735_fill_rect(prev_ball_x, prev_ball_y, PONG_BALL, PONG_BALL, ST7735_BLACK);
    if (prev_py >= 0)
        st7735_fill_rect(4, prev_py, PONG_PADDLE_W, PONG_PADDLE_H, ST7735_BLACK);
    if (prev_ay >= 0)
        st7735_fill_rect(rx, prev_ay, PONG_PADDLE_W, PONG_PADDLE_H, ST7735_BLACK);

    /* draw new sprites */
    st7735_fill_rect(4,  s->pong.player_y, PONG_PADDLE_W, PONG_PADDLE_H, ST7735_WHITE);
    st7735_fill_rect(rx, s->pong.ai_y,     PONG_PADDLE_W, PONG_PADDLE_H, ST7735_WHITE);
    st7735_fill_rect(s->pong.ball_x, s->pong.ball_y, PONG_BALL, PONG_BALL, ST7735_YELLOW);

    if (s->score != (uint32_t)prev_score || lives != prev_lives) {
        char h[48];
        snprintf(h, sizeof(h), "RALLY %lu  LIVES %d", (unsigned long)s->score, lives);
        st7735_fill_rect(0, 0, SCR_W, 9, ST7735_BLACK);
        st7735_draw_string(2, 1, h, ST7735_CYAN, ST7735_BLACK, 1);
        prev_score = (int)s->score;
        prev_lives = lives;
    }

    prev_ball_x = s->pong.ball_x; prev_ball_y = s->pong.ball_y;
    prev_py = s->pong.player_y;   prev_ay = s->pong.ai_y;
}

/* ============================ DINO RUN ============================ */

static void render_dino(const arcade_state_t *s)
{
    static int prev_dino_y = -1;
    static int prev_ox[DINO_NUM_OBS] = {-99, -99, -99};
    static int prev_oh[DINO_NUM_OBS] = {0, 0, 0};
    static uint32_t prev_score = 0xFFFFFFFFu;

    if (s->dirty_full) {
        st7735_fill_screen(ST7735_BLACK);
        prev_dino_y = -1;
        for (int i = 0; i < DINO_NUM_OBS; i++) { prev_ox[i] = -99; prev_oh[i] = 0; }
        prev_score = 0xFFFFFFFFu;
    }

    /* erase previous obstacles + dino */
    for (int i = 0; i < DINO_NUM_OBS; i++) {
        if (prev_ox[i] > -DINO_OBS_W)
            st7735_fill_rect(prev_ox[i] < 0 ? 0 : prev_ox[i],
                             DINO_GROUND_Y - prev_oh[i],
                             DINO_OBS_W, prev_oh[i], ST7735_BLACK);
    }
    if (prev_dino_y >= 0)
        st7735_fill_rect(DINO_X, prev_dino_y, DINO_W, DINO_H, ST7735_BLACK);

    /* ground line (redraw fully: cheap and avoids erase artifacts) */
    draw_hline(0, DINO_GROUND_Y, SCR_W, ST7735_WHITE);

    /* draw obstacles + dino */
    for (int i = 0; i < DINO_NUM_OBS; i++) {
        int ox = s->dino.obs_x[i];
        if (ox > -DINO_OBS_W && ox < SCR_W)
            st7735_fill_rect(ox < 0 ? 0 : ox, DINO_GROUND_Y - s->dino.obs_h[i],
                             DINO_OBS_W, s->dino.obs_h[i], ST7735_GREEN);
        prev_ox[i] = ox;
        prev_oh[i] = s->dino.obs_h[i];
    }
    st7735_fill_rect(DINO_X, s->dino.y, DINO_W, DINO_H, ST7735_CYAN);
    prev_dino_y = s->dino.y;

    if (s->score != prev_score) {
        char h[48];
        snprintf(h, sizeof(h), "DIST %lu", (unsigned long)s->score);
        st7735_fill_rect(0, 0, SCR_W, 9, ST7735_BLACK);
        st7735_draw_string(2, 1, h, ST7735_WHITE, ST7735_BLACK, 1);
        prev_score = s->score;
    }
}

/* ============================ GAME OVER / SLEEP ============================ */

static void render_gameover(const arcade_state_t *s)
{
    if (!s->dirty_full) return;   /* static screen */
    st7735_fill_screen(ST7735_BLACK);
    draw_centered(8, "GAME OVER", ST7735_RED, ST7735_BLACK, 2);
    draw_centered(30, game_name(s->current_game), ST7735_GRAY, ST7735_BLACK, 1);

    char l[48];
    snprintf(l, sizeof(l), "SCORE %lu", (unsigned long)s->score);
    draw_centered(42, l, ST7735_WHITE, ST7735_BLACK, 1);
    snprintf(l, sizeof(l), "BEST  %lu", (unsigned long)s->high[s->current_game]);
    draw_centered(52, l, ST7735_YELLOW, ST7735_BLACK, 1);

    if (s->new_high) draw_centered(60, "NEW HIGH!", ST7735_GREEN, ST7735_BLACK, 1);
    draw_centered(70, "A=RETRY  HOLD A=MENU", ST7735_CYAN, ST7735_BLACK, 1);
}

static void render_sleep(const arcade_state_t *s)
{
    if (!s->dirty_full) return;
    st7735_fill_screen(ST7735_BLACK);
    draw_centered(18, "Zzz", ST7735_BLUE, ST7735_BLACK, 2);
    draw_centered(42, "SLEEPING", ST7735_GRAY, ST7735_BLACK, 1);
    draw_centered(56, "PRESS A", ST7735_WHITE, ST7735_BLACK, 1);
    draw_centered(66, "TO WAKE", ST7735_WHITE, ST7735_BLACK, 1);
}

/* ============================ Task ============================ */

void display_task(void *pvParameters)
{
    arcade_state_t snap;

    while (1) {
        xSemaphoreTake(state_mutex, portMAX_DELAY);
        snap = g_state;                 /* copy snapshot */
        bool full = g_state.dirty_full;
        g_state.dirty_full = false;     /* consume the full-redraw request */
        xSemaphoreGive(state_mutex);
        snap.dirty_full = full;

        switch (snap.screen) {
            case SCREEN_MENU:     render_menu(&snap);     break;
            case SCREEN_PLAY:
                switch (snap.current_game) {
                    case GAME_FLAPPY: render_flappy(&snap); break;
                    case GAME_PONG:   render_pong(&snap);   break;
                    case GAME_DINO:   render_dino(&snap);   break;
                }
                break;
            case SCREEN_GAMEOVER: render_gameover(&snap); break;
            case SCREEN_SLEEP:    render_sleep(&snap);    break;
        }

        vTaskDelay(pdMS_TO_TICKS(33));
    }
}
