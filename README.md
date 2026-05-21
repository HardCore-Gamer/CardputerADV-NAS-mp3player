# Cardputer-Adv NAS MP3 Player

Firmware for M5Stack Cardputer-Adv that connects to WiFi, opens an HTTP/WebDAV NAS directory, lists folders and MP3 files, and streams selected MP3 tracks.

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
- File list: `r` reloads, `n` edits NAS URL, `t` opens sleep timer.
- Player: progress bar + volume bar on screen.
- Player: `Space` stops/resumes from the saved position.
- Player: `Fn + ,` seek backward, `Fn + .` seek forward.
- Player: `<` and `>` also trigger backward/forward seek.
- Player: `n` next, `p` previous, `b` file list, `t` timer, `+`/`-` volume.

## Build

```powershell
pio run
```

The M5Launcher app binary is:

```text
.pio/build/cardputer_adv_nas_mp3/firmware.bin
```
