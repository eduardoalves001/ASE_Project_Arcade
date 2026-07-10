#ifndef DISPLAY_ARCADE_H
#define DISPLAY_ARCADE_H

/*
 * Rendering task. It is the only task that talks to the ST7735 over SPI.
 * Each frame it snapshots g_state under the mutex and repaints, using a
 * dirty-rectangle strategy so the small 160x80 panel does not flicker.
 */
void display_task(void *pvParameters);

#endif /* DISPLAY_ARCADE_H */
