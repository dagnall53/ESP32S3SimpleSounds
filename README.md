# ESP32S3 Simple Sounds

A lightweight, reliable audio playback system for the ESP32‑S3, supporting **MP3** and **WAV** playback from **FFat**, with a built‑in **Wi‑Fi AP**, optional **STA mode**, a simple **web UI**, and a full **file manager** powered by ESPFMfGK.

This project is designed for simple sound‑triggering applications, playlists, timed sequences, and remote control via HTTP.

---

## Features

### 🎵 Audio Playback
- Supports **MP3** via ESP8266Audio (`AudioGeneratorMP3`)
- Supports **WAV** via ESP8266Audio (`AudioGeneratorWAV`)
- Automatic format detection based on file extension
- Smooth playback using I2S NoDAC mode
- Adjustable WAV playback rate via `wavspeed.txt`

### 📁 FFat File Storage
- All audio files stored in FFat
- File manager accessible via web browser
- Upload, delete, rename, and manage files directly

### 🌐 Wi‑Fi Modes
- **AP Mode** always enabled  
  - Default AP: `ESP32S3-Audio` / `12345678`  
  - AP IP: `192.168.4.1`
- **STA Mode** optional  
  - Reads credentials from `/wifi.json`
  - If STA fails, AP remains active and stable
- Auto‑reconnect disabled for maximum AP stability

### 🖥️ Web Interface
- Home page with:
  - Play Sequence button  
  - Play Playlist button  
  - File Manager link  
  - AP and STA IP display
- File Manager on port **8080**

### 🔊 Playback Modes

#### 1. Playlist Mode
Plays the first file listed in `/playlist.txt`.

#### 2. Sequence Mode
Plays a timed sequence of audio files defined in `/sequence.json`.

#### 3. Trigger Button
The ESP32S3 boot button (GPIO0) triggers playlist playback.

---

## File Formats

### 📄 playlist.txt

A simple text file listing audio filenames, one per line.

Example:
```json
/F1.wav
/F2.mp3
/alert.wav
```
##Code

Only the **first entry** is used by `/playplaylist` or the trigger button.

---

### 📄 sequence.json

A JSON array describing a timed sequence of audio events.

Each entry contains:
- `"file"` — path to the audio file  
- `"delay_ms"` — delay before playing the file  

Example:

```json
[
  { "file": "/intro.mp3", "delay_ms": 0 },
  { "file": "/beep.wav", "delay_ms": 500 },
  { "file": "/message.mp3", "delay_ms": 2000 }
]
```
The sequence player:
Waits delay_ms
Plays the file
Waits until playback finishes
Moves to the next item

### Wi‑Fi Configuration
/wifi.json
If present, STA mode will attempt to connect using these credentials.

Example:

```json
{
  "ssid": "MyNetwork",
  "password": "MyPassword"
}
```
If missing, the device uses fallback credentials and remains in AP mode.

## HTTP Endpoints

| Endpoint | Description |
|---------|-------------|
| `/` | Home page |
| `/playplaylist` | Plays first entry in `playlist.txt` |
| `/playsequence` | Plays the full sequence from `sequence.json` |
| `/reconnectwifi` *(optional)* | Attempts STA reconnection |
| `http://<ip>:8080/` | File manager |

---

## Hardware

- ESP32‑S3 module  
- I2S NoDAC output (single‑transistor amplifier supported)  
- Boot button used as trigger input  
- Optional LEDs for status indication  

---

## Startup Flow

1. Mount FFat  
2. Start AP  
3. Load WAV speed  
4. Attempt STA connection (non‑blocking)  
5. Start file manager  
6. Start web server  
7. Optionally play startup sequence  

---

## Dependencies

- ESP8266Audio  
- ESPFMfGK  
- ArduinoJson  
- FFat  
- WebServer (ESP32 core)  

---

## License

This project is provided as‑is for personal and experimental use.

