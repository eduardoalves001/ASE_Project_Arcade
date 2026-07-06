# Report Writing Plan: ESP-Arcade Retro Console

A section-by-section plan for a ~5-page academic report (Overleaf/LaTeX) on the
"ESP-Arcade: a FreeRTOS retro mini-game console on the ESP32-C6" project.

## General Guidelines
* **Format:** IEEE conference style or standard single/two-column university format.
* **Length:** ~5 pages of core content (excluding cover and references).
* **Tone:** Academic, technical, objective ("The system implements...").
* **Visuals:** Include `circuit.png` (the wiring is shared with the board) and at
  least one screen-state diagram of the menu/game/game-over/sleep state machine.

---

## Page 1: Introduction and Hardware Architecture

### 1. Title and Abstract (~0.5 pages)
* **Title:** e.g. *ESP-Arcade: A FreeRTOS-based Retro Game Console with IoT
  Leaderboard on the ESP32-C6*.
* **Abstract (150-200 words):** state the problem (a multi-game handheld console),
  the hardware (ESP32-C6 + ST7735 + SD + ADC + GPIO), the software paradigm
  (FreeRTOS tasks, mutex, queues, ISRs), the protocols (SPI, ADC, MQTT/TLS), and
  the results (three playable games, SD-persistent high scores, per-point LED
  feedback, light sleep, and a secure web leaderboard).
* **Keywords:** ESP32, FreeRTOS, real-time game loop, MQTT over TLS, SPI, Low Power.

### 2. Introduction (~0.5 pages)
* Objective: a hardware arcade console running several classic mini-games.
* Requirements: responsive analog/digital input, a flicker-free real-time renderer,
  non-volatile high scores, power efficiency, and remote
  telemetry/control.

### 3. Hardware Architecture & Peripherals (~1 page)
* Introduce the ESP32-C6 DevKitC-1. Insert `circuit.png` (Figure 1).
* **SPI (ST7735 + SD):** MOSI 19, MISO 20, CLK 21. Explain how the two Chip Selects
  (TFT GPIO 22, SD GPIO 18) multiplex one `SPI2_HOST` bus without collisions.
* **ADC (Potentiometer):** GPIO 1 — analog control (menu selection, Pong paddle).
* **GPIO & Interrupts (Buttons + LED):** Buttons GPIO 23/0/4 (pull-up, falling-edge
  interrupts); LED GPIO 5 pulses on every score increment.

---

## Page 2-3: Software Architecture and FreeRTOS Paradigm

### 4. Software Architecture & Concurrency
* Motivate FreeRTOS over a single `while(1)` loop. Describe the three tasks:
  1. `game_task` — the menu + game state machine ("the brain"), ~33 Hz.
  2. `display_task` — the sole SPI renderer (dirty-rectangle drawing).
  3. `net_task` — MQTT publish/subscribe independent of game timing.

### 5. Synchronization and Resource Management
* **Mutex (`state_mutex`):** the single `arcade_state_t` is the shared resource.
  Show how `game_task` writes it and `display_task`/`net_task` snapshot it under the
  mutex (copy-then-render / copy-then-publish to minimise the critical section).
* **Queue (`button_evt_queue`) + ISR:** the GPIO ISR only pushes the GPIO id; the
  game task does the heavy work. MQTT commands push synthetic `REMOTE_*` ids into the
  *same* queue, unifying local and remote input.

### 6. Game Logic and the State Machine (include pseudo-code)
* Screens: `MENU → PLAY → GAMEOVER → MENU`, plus `SLEEP`.
* Per-game mechanics:
  * **Flappy** — fixed-point vertical physics (gravity + flap impulse), scrolling
    pipe pairs recycled via a deterministic xorshift PRNG, pass-detection scoring.
  * **Pong** — analog paddle from the ADC, AI paddle tracking, survival scoring
    (rallies survived; 5 misses ends the run).
  * **Dino Run** — integer jump physics (gravity + impulse), scrolling obstacles,
    distance score, speed ramping with distance.
* **Score LED:** the status LED pulses (~120 ms) on every score increment,
  giving immediate physical feedback tied to the game state.
* **One-button UX:** short presses arrive through the GPIO ISR queue (tap =
  flap/jump/start), while a polled level check in the game loop detects a ~5 s
  hold that exits to the menu — the entire console is operable with a single
  button, combining edge-triggered interrupts with level polling.
* **Pseudo-code (game loop):**
  ```text
  Algorithm 1: game_task
  BEGIN
    screen <- MENU
    WHILE True DO
      pot <- read ADC percent
      LOCK state_mutex
        FOR each queued button/remote event DO debounce + handle_event
        IF screen = MENU THEN menu_index <- map(pot)
        ELSE IF screen = PLAY THEN update(current_game, pot)   // may end the game
        IF screen = MENU AND idle > 30s THEN mark_for_sleep
        IF score increased THEN pulse LED
      UNLOCK state_mutex
      IF mark_for_sleep THEN enter_light_sleep()
      Delay(33 ms)
    END WHILE
  END
  ```

### 6.1 Flicker-Free Rendering
* The naive approach (clear + redraw the whole 160x80 panel each frame) flickers.
* Dirty-rectangle strategy: on a screen change (`dirty_full`) draw the static
  background once; each frame erase only the cells/sprites that moved and redraw them.
* Flappy/Pong/Dino erase the previous bird/ball/paddle/pipe/obstacle rectangles then
  redraw them at the new positions; score text is repainted only when the value
  changes.

---

## Page 4: Advanced Features (SPI Multiplexing, Sleep, IoT)

### 7. Peripheral Deep-Dive: SPI Multiplexing and FatFs
* One `SPI2_HOST` shared by the SD card (`sdspi_host`) and the TFT
  (`st7735_add_device`, `skip_bus_init = true`).
* FatFs persistence of high scores to a tagged `ARC1` text file (`scores.txt`).

### 8. Power Management (Light Sleep)
* Transition from active to Light Sleep after menu inactivity.
* Contrast Light vs Deep Sleep (RAM retained, CPU halted, peripherals clock-gated).
* Hardware specifics: `gpio_hold_en()` freezes the backlight pin during sleep, and
  `gpio_wakeup_enable()` lets Button A (GPIO 23) wake the device. Wake button presses
  are drained from the queue so they do not start a game.

### 9. Connectivity (Wi-Fi & Secure MQTT)
* Wi-Fi Station with NVS, secure MQTT (`mqtts://`) with an embedded CA, and
  username/password auth.
* Two topics: `arcade/status` (publish) and `arcade/command` (subscribe).
* `xTaskNotifyGive` forces an immediate publish on state changes (start, game over,
  score, sleep) instead of waiting for the periodic timer.

---

## Page 5: Web Dashboard and Conclusion

### 10. Web Dashboard & Two-Way Communication
* Node.js/Express bridge: MQTT ↔ Socket.IO.
* Frontend renders the live screen/score, the three high scores (leaderboard), the
  and the live score.
* Remote control: buttons publish `start_flappy|start_pong|start_dino|select|menu|
  reset_scores`; the firmware injects these as `REMOTE_*` events.

### 11. Conclusion
* Achievements: 5+ peripherals, a clean RTOS architecture, a flicker-free real-time
  renderer, SD persistence, per-point LED feedback, light sleep, and bidirectional
  IoT control.
* Future work: I2S buzzer for sound, a second BLE gamepad, or networked 2-player Pong
  between two boards.

### 12. References
* ESP-IDF Programming Guide; FreeRTOS Reference Manual; ST7735 datasheet.
