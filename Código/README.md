# ASE Final Project: ESP-Arcade — Retro Mini-Game Console (ESP32-C6)

This repository contains an Embedded Systems Architecture (ASE) project for 2025/26:
a hardware retro **arcade console** built on the ESP32-C6. It runs three classic
mini-games on a tiny TFT, keeps high scores on an SD card, gives LED feedback on
every point scored, sleeps to save power, and reports an **online leaderboard** to a secure
web dashboard over MQTT/TLS.

> The application, firmware and dashboard were written from scratch for this
> project, on a standard breadboard wiring for this hardware kit: ESP32-C6,
> ST7735 + SD sharing one SPI bus, potentiometer, push button and LED
> (see `circuit.png`).

## Games

| Game | Goal | Controls |
|------|------|----------|
| **Flappy** | Fly through the gaps between the pipes | Button A = flap |
| **Pong** | Survival rally vs. an AI paddle; 5 misses ends the run | Potentiometer = move paddle |
| **Dino Run** | Endless runner, jump the obstacles, distance = score | Button A = jump |

**One-button design:** the whole console is playable with just Button A plus the
potentiometer. **Hold Button A for ~5 s** to quit back to the menu from anywhere
(a quit run still counts toward the record). Buttons B (give up) and C (menu on
game over) are optional extras if wired.

## Hardware Map

- ESP32-C6 DevKitC-1
- ST7735 TFT display (160x80) on SPI2
- SD card on the same SPI2 bus
- Potentiometer on ADC1
- Three buttons and one status LED

Main pins (unchanged from the original board wiring — see `circuit.png`):

- TFT/SD SPI: MOSI GPIO 19, MISO GPIO 20, CLK GPIO 21
- TFT: CS GPIO 22, DC GPIO 2, RST GPIO 3, BL GPIO 15
- SD card: CS GPIO 18
- Potentiometer: GPIO 1
- Button A: GPIO 23, Button B: GPIO 0, Button C: GPIO 4
- Status LED: GPIO 5

## Firmware Architecture

The firmware is split into focused FreeRTOS tasks that share one `arcade_state_t`
guarded by `state_mutex`:

- `game_task` (`games.c`): the brain — menu state machine + Flappy/Pong/Dino
  simulation. Reads the potentiometer and the button event queue, detects the
  Button A hold gesture, persists high scores, and manages light sleep.
- `display_task` (`display_arcade.c`): the only task that drives the SPI display,
  using a dirty-rectangle strategy so the 160x80 panel does not flicker.
- `net_task` (`network_mqtt.c`): connects to secure MQTT, publishes `arcade/status`,
  and reacts to `arcade/command`.

Button interrupts push the GPIO id into `button_evt_queue` from the ISR; remote MQTT
commands push synthetic `REMOTE_*` events into the same queue, so the game task has a
single, uniform input path.

## Controls Summary

- **Menu:** turn the potentiometer to highlight a game, Button A to start it.
- **Flappy:** A = flap (the run starts on the first flap).
- **Pong:** potentiometer moves your paddle.
- **Dino:** A = jump.
- **Anywhere:** hold A for ~5 s = quit to the menu (the score still counts).
- **Game over:** A = play again; hold A = menu (B/C also go to the menu if wired).
- **Sleep:** after 30 s idle on the menu the console enters Light Sleep; press
  Button A to wake it.

## Score LED

The status LED gives instant physical feedback while playing: it pulses for
~120 ms every time the score increases — one blink per pipe passed in Flappy,
per rally survived in Pong, and a near-continuous glow as distance accumulates
in Dino Run.

## High Score Persistence

High scores are stored on the SD card in `/sdcard/scores.txt` using a tagged text
format (`ARC1`). They are loaded at boot and rewritten whenever a record is beaten or
the scores are reset from the dashboard.

## Secure MQTT Setup

Run the setup script from the repository root while the laptop is on the same AP the
ESP32 will use:

```bash
./scripts/setup_mosquitto_tls.sh
```

The script detects the laptop IP, generates a local CA + server certificate, installs
a Mosquitto TLS listener on port `8883` with username/password auth, copies the CA to
`main/mqtt_broker_ca.pem`, and updates `main/generated_mqtt_config.h` and
`arcade-dashboard/mqtt-config.json`.

Static MQTT credentials:

- Username: `arcade`
- Password: `arcade-local-2026`

Test the broker locally:

```bash
mosquitto_sub -h <laptop-ip> -p 8883 --cafile .local/mqtt-certs/arcade-ca.crt -u arcade -P arcade-local-2026 -t arcade/status -v
mosquitto_pub -h <laptop-ip> -p 8883 --cafile .local/mqtt-certs/arcade-ca.crt -u arcade -P arcade-local-2026 -t arcade/command -m start_flappy
```

## Wi-Fi Configuration

Defaults live in `main/Kconfig.projbuild`, `sdkconfig`, and `sdkconfig.defaults`:

- SSID: `Mi 11 Lite 5G`
- Password: `1234567890`

The ESP32-C6 only joins **2.4 GHz** networks. If the hotspot uses 5 GHz the Wi-Fi
disconnect reason is `201` (`NO_AP_FOUND`); force the AP to 2.4 GHz and rerun the TLS
script so the IP and certificates stay current. The games run fine even with no
network — only the leaderboard/remote-control features need MQTT.

## Build and Flash

```bash
idf.py build
idf.py flash
idf.py monitor
```

## Web Dashboard (online leaderboard + remote control)

A Node.js/Express server bridges secure MQTT to Socket.IO for the browser UI.

```bash
cd arcade-dashboard
npm install
npm start
```

Open `http://localhost:3000`. The dashboard shows the live screen/score, the three
high scores, and remote buttons that publish
to `arcade/command`:

- `start_flappy` / `start_pong` / `start_dino` — start a game on the device.
- `select` — acts like Button A (start / replay).
- `menu` — return to the menu.
- `reset_scores` — clear all high scores on the device.

## MQTT Status Payload

`arcade/status` publishes JSON:

```json
{"screen":"play","game":"flappy","score":12,
 "high_flappy":40,"high_pong":18,"high_dino":233,
 "is_sleeping":false}
```
