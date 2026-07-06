#ifndef GAMES_H
#define GAMES_H

/*
 * The arcade "brain": a single FreeRTOS task that runs the menu state machine
 * and the per-game simulation (Flappy, Pong, Dino Run). It reads the
 * potentiometer + button queue, mutates g_state under state_mutex, persists
 * high scores to the SD card, and asks the network task to publish on changes.
 */
void game_task(void *pvParameters);

#endif /* GAMES_H */
