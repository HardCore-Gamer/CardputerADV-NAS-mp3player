
# Cardputer-Adv NAS MP3 Player

Firmware for M5Stack Cardputer-Adv that connects to WiFi, opens an HTTP/WebDAV NAS directory, lists folders and MP3 files, and streams selected MP3 tracks.

The player keeps the last track URL, byte position, volume, and eco-mode setting in NVS. If the saved WiFi and NAS settings are still valid, boot reconnects to WiFi and returns to the last track screen. Playback is resumed manually from the player screen.

## Player UI

- Top-right status area shows time, WiFi signal strength, and battery state.
- Playback screen uses a compact three-band layout with track title, state, timer, progress bar, elapsed/total time, percentage, and volume bar.
- The player screen no longer shows the local IP address.
- The playback screen uses partial redraws instead of full-screen refreshes to reduce visible flicker, and the help panel blocks background progress updates cleanly.
- File list and WiFi list use the same dark panel layout with simpler list-first presentation.
- Shortcut details are hidden by default; press `h` to open the shortcut panel.
<img width="660" height="980" alt="photo_2026-05-24_09-58-29" src="https://github.com/user-attachments/assets/117fe2d7-cfd6-44c6-84a8-8d68fafa0457" />
## Power Saving

- Eco mode is enabled by default.
- While music is playing, 15 seconds without input dims the display and reduces UI refresh frequency.
- After a longer idle period, the screen turns fully off while audio playback continues.
- Press any key once to wake the display, then press again for the intended action.
- Press `m` on the player screen to toggle eco mode manually.

## NAS URL

Use an HTTP/WebDAV URL, for example:

```text
http://192.168.1.20:5005/music/
http://user:password@192.168.1.20:5005/music/
```

SMB/CIFS shares such as `\\NAS\music` are not supported by this ESP32 firmware. Enable WebDAV, a simple HTTP directory listing, or another local HTTP service on the NAS.

## Controls

- WiFi list / file list: `w` and `s` move, `Enter` selects.
- Text input: type normally, `Backspace` deletes, `Enter` confirms, `Tab` cancels.
- File list: `f` searches within the current folder.
- File list: `q` opens WiFi selection, `r` reloads, `n` edits NAS URL, `t` opens sleep timer.
- File list / player: `h` opens the shortcut panel.
- Player: time, WiFi icon, battery icon, timer state, progress bar, elapsed/total time, percentage, volume bar, and ECO state are on screen.
- Player: `Space` starts or resumes playback from the saved position.
- Player: `Fn + ,` seek backward, `Fn + .` seek forward.
- Player: `<` and `>` also trigger backward/forward seek.
- Player: `+` and `-` change volume.
- Player: `q` stops playback and opens WiFi selection.
- Player: `n` next, `p` previous, `b` file list, `t` timer.
- Player: `m` toggles eco mode.
- WiFi list: `Tab` returns to the previous browser screen when opened from the library/player.

## Build

```powershell
python -m platformio run
```

The M5Launcher app binary is:

```text
.pio/build/cardputer_adv_nas_mp3/firmware.bin
```

A ready-to-copy build is also written to:

```text
cardputer_adv_nas_mp3.bin
```
