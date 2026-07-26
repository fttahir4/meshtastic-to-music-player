# DeMeshtify: Turning your Meshtastic node into a Spotify Music Player (Displays Only) 🎵

Turn a Meshtastic-flashed Heltec WiFi LoRa 32 (V3/V4) node into a live Spotify
"now playing" display. Wipes the mesh firmware and replaces it with a custom
build that shows a spinning disk icon, scrolling track info, a live progress
bar, playback status icons, and an idle clock with recent-play history.

Built and named **Faz FM** on the original hardware this project runs on.

## Features

- **Spinning disk icon** — a hand-drawn vinyl-record icon that spins while a
  track is playing, freezes mid-spin when paused, and resets to 12 o'clock
  when nothing's loaded
- **Song title + artist display** — scrolls as a looping ticker when the text
  is too wide for the screen, based on actual pixel width (not character
  count), so it works correctly across scripts of very different widths
- **Multi-language support** — automatically detects and switches fonts for
  Latin (with accents), Japanese, Korean, Cyrillic, Arabic, Urdu, Hebrew,
  Greek, Devanagari, and Vietnamese. Arabic/Urdu runs are re-ordered to read
  right-to-left, matching how mixed-language titles actually display
- **Live progress bar** — synced from Spotify every 5 seconds, interpolated
  smoothly between polls using the board's own clock
- **Playback status icons** — shuffle, play/pause, and repeat state, styled
  after Spotify's own icon language (dot indicators for active states)
- **Idle screen** — shows the current time (synced over NTP) and cycles
  through your last few played tracks when nothing's playing
- **Day/night auto-dimming** — screen brightness lowers automatically in the
  evening and returns to normal in the morning
- **Multi-network wifi** — supports a list of networks to try in order, with
  automatic reconnection if the connection drops

## Hardware

- Heltec WiFi LoRa 32 V4 (or V3 — same pinout/board profile)
- USB-C cable (must support data, not charge-only)

## Setup

### 1. Arduino IDE

1. Install [Arduino IDE](https://www.arduino.cc/en/software)
2. Add this to **Preferences → Additional Boards Manager URLs**:
   ```
   https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json
   ```
3. Install the **esp32 by Espressif Systems** package via Boards Manager
4. Install these libraries via **Sketch → Include Library → Manage Libraries**:
   - `U8g2` by olikraus
   - `ArduinoJson` by Benoit Blanchon

### 2. Spotify app

1. Create an app at the [Spotify Developer Dashboard](https://developer.spotify.com/dashboard)
2. Set the redirect URI to `http://127.0.0.1:8888/callback`
3. Grab your **Client ID** and **Client Secret** from the app's Settings page

### 3. Get a refresh token

Run `get_spotify_token.py` once on your computer:

```
pip3 install requests
python3 get_spotify_token.py
```

Fill in your Client ID and Secret in the script first. It'll open your
browser for a one-time login/approval, then print a refresh token, save it.

### 4. Confirm the mesh firmware is gone (optional but recommended)

Flash `heltec_v4_hello.ino` first — a tiny test sketch that prints a message
to the OLED. If you see it on the screen instead of the Meshtastic menu,
you've confirmed the board is fully out of mesh firmware and ready for the
main sketch. This step also verifies your board/port setup is correct before
moving on to the full project.

### 5. Flash the board

1. Open `spotify_now_playing.ino` in Arduino IDE
2. Fill in the placeholders near the top: wifi network(s), Spotify Client ID,
   Client Secret, and refresh token
3. Select **Tools → Board → esp32 → Heltec WiFi LoRa 32 (V4)**
4. Select the correct port under **Tools → Port**
5. Upload

If the upload fails to connect, hold the **PRG** button, plug in the USB
cable while still holding it, release after a couple seconds, then try
uploading again.

## Fully removing Meshtastic

Uploading any Arduino sketch (including the ones in this repo) already
overwrites the Meshtastic firmware — there's no separate "uninstall" step
needed. Once you've flashed `heltec_v4_hello.ino` or the main sketch, the
board is no longer running Meshtastic in any form.

If you ever want a completely blank slate before flashing something new
(not required, just extra-clean), you can do a full flash erase first using
[`esptool`](https://docs.espressif.com/projects/esptool/):

```
esptool.py erase_flash
```

### Reinstalling Meshtastic later

If you want to put Meshtastic back on the board:

1. Go to [flasher.meshtastic.org](https://flasher.meshtastic.org) in Chrome
   or Edge (requires WebSerial support)
2. Plug in the board
3. Select **Heltec WiFi LoRa 32 V3** from the board dropdown (same profile
   works for V4)
4. Pick the latest stable firmware release
5. Click **Flash**

## Notes on limitations

- Arabic/Urdu text displays with correct letter *order* but not letter
  *joining* — the display library doesn't support Arabic script shaping, so
  connected cursive letterforms aren't possible on this hardware
- Battery percentage isn't supported, since this runs on a plain USB power
  source with no battery-monitoring chip exposed
