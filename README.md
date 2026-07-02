# ASE Final Project: ESP-Arcade — Retro Mini-Game Console (ESP32-C6)

This repository contains an Embedded Systems Architecture (ASE) project for 2025/26:
a hardware retro **arcade console** built on the ESP32-C6. It runs three classic
mini-games on a tiny TFT, keeps high scores on an SD card, reacts to the ambient
temperature, sleeps to save power, and reports an **online leaderboard** to a secure
web dashboard over MQTT/TLS.

> This is a different application built on the *same breadboard wiring* used by the
> "Interactive IoT Virtual Pet" project: same ESP32-C6, same ST7735 + SD on a shared
> SPI bus, same DHT20, same potentiometer, same three buttons and LED. Only the
> firmware application and the dashboard are new.

## Games

| Game | Goal | Controls |
|------|------|----------|
| **Snake** | Eat food, grow, don't hit a wall or yourself | Button A = turn left, Button C = turn right |
| **Pong** | Survival rally vs. an AI paddle; 5 misses ends the run | Potentiometer = move paddle |
| **Dino Run** | Endless runner, jump the obstacles, distance = score | Button A = jump |

Button **B** quits the current game (the score still counts toward the record).

## Hardware Map

- ESP32-C6 DevKitC-1
- ST7735 TFT display (160x80) on SPI2
- SD card on the same SPI2 bus
- DHT20 temperature/humidity sensor on I2C
- Potentiometer on ADC1
- Three buttons and one status LED

Main pins (unchanged from the original board wiring — see `circuit.png`):

- TFT/SD SPI: MOSI GPIO 19, MISO GPIO 20, CLK GPIO 21
- TFT: CS GPIO 22, DC GPIO 2, RST GPIO 3, BL GPIO 15
- SD card: CS GPIO 18
- DHT20: SDA GPIO 6, SCL GPIO 7
- Potentiometer: GPIO 1
- Button A: GPIO 23, Button B: GPIO 0, Button C: GPIO 4
- Status LED: GPIO 5

## Firmware Architecture

The firmware is split into focused FreeRTOS tasks that share one `arcade_state_t`
guarded by `state_mutex`:

- `sensor_task` (`sensors.c`): reads the DHT20 every second into `temp`/`hum`.
- `game_task` (`games.c`): the brain — menu state machine + Snake/Pong/Dino
  simulation. Reads the potentiometer and the button event queue, persists high
  scores, and manages light sleep.
- `display_task` (`display_arcade.c`): the only task that drives the SPI display,
  using a dirty-rectangle strategy so the 160x80 panel does not flicker.
- `net_task` (`network_mqtt.c`): connects to secure MQTT, publishes `arcade/status`,
  and reacts to `arcade/command`.

Button interrupts push the GPIO id into `button_evt_queue` from the ISR; remote MQTT
commands push synthetic `REMOTE_*` events into the same queue, so the game task has a
single, uniform input path.

## Controls Summary

- **Menu:** turn the potentiometer to highlight a game, Button A to start it.
- **Snake:** A = turn left, C = turn right.
- **Pong:** potentiometer moves your paddle.
- **Dino:** A = jump.
- **Any game:** B = give up and see the score.
- **Game over:** A = play again, B/C = back to menu.
- **Sleep:** after 30 s idle on the menu the console enters Light Sleep; press
  Button C to wake it.

## Ambient "Hard Mode"

The DHT20 temperature feeds the difficulty: at or above `HARD_TEMP_C` (28 °C) the
console switches to **hard mode** — games run faster and the status **LED turns on**.
The dashboard shows a red `HARD!` badge. This demonstrates a real sensor influencing
game logic.

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
mosquitto_pub -h <laptop-ip> -p 8883 --cafile .local/mqtt-certs/arcade-ca.crt -u arcade -P arcade-local-2026 -t arcade/command -m start_snake
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
high scores, the ambient temperature/hard-mode state, and remote buttons that publish
to `arcade/command`:

- `start_snake` / `start_pong` / `start_dino` — start a game on the device.
- `select` — acts like Button A (start / replay).
- `menu` — return to the menu.
- `reset_scores` — clear all high scores on the device.

## MQTT Status Payload

`arcade/status` publishes JSON:

```json
{"screen":"play","game":"snake","score":12,
 "high_snake":40,"high_pong":18,"high_dino":233,
 "temp":24.6,"hum":51.0,"hard_mode":false,"is_sleeping":false}
```
