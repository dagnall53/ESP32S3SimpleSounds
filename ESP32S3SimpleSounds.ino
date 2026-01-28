#include <WiFi.h>
#include <FS.h>
// Remove the file systems that are not needed.
#include <SD.h>
#include <LittleFS.h>
#include <SD_MMC.h>
#include <FFat.h>
#include <SPI.h>
// the thing.
// ---------------- JSON ----------------
#include <ArduinoJson.h>

// ---------------- FILE MANAGER ----------------
#include <WebServer.h>
#include <ESPFMfGK.h>

#include "ESPFMfGKdropin.h"
#include <Arduino.h>
#include <ArduinoJson.h>
#include <WebServer.h>
#include <vector>

#include "AudioFileSourceFATFS.h"
#include "AudioFileSourceFS.h"
#include "AudioFileSourceID3.h"
#include "AudioGeneratorMP3.h"
#include "AudioGeneratorWAV.h"
#include "AudioOutputI2SNoDAC.h"
#include "AudioOutputMixer.h"

// ---------------- CONFIG ----------------
#define TRIGGER_PIN 0
#define AP_SSID "ESP32S3-Audio"
#define AP_PASS "12345678"

// Choose two unused pins for BCLK and LRCLK.
// They are not used electrically in the one‑transistor circuit used by  AudioOutputI2SNoDAC.
#define I2S_BCLK 4
#define I2S_LRCLK 5
#define AUDIOOUT 6  // your defined output pin

const int ledPins[] = { 13, 12, 11, 10, 9 };
const int ledCount = sizeof(ledPins) / sizeof(ledPins[0]);

// ---------------- PATHS ----------------
#define WIFI_JSON_PATH "/wifi.json"
#define SEQUENCE_JSON_PATH "/sequence.json"

// ---------------- GLOBALS ----------------
WebServer server(80);
String ssid, pass;

bool audioReady = false;
bool setwifidefault = false;

uint32_t lastLedStep = 0;
int ledIndex = 0;
const uint32_t ledInterval = 200;  // ms per step (tune this)

// ---------------- AUDIO (ESP8266Audio) ----------------
// Dual‑Channel Audio Engine using AudioOutputMixer
// Channel A = Sequence
// Channel B = Playlist

bool playRequested = false;
bool audioIsActive = false;
uint32_t rate;  // sample rate for wav

// Sequence items
struct SequenceItem {
  String filename;
  uint32_t delayMs;
};
std::vector<SequenceItem> playSequence;

// Playlist
std::vector<String> playlist;

// Decoders + sources (dual‑channel)
AudioGeneratorMP3 *mp3A = nullptr;
AudioGeneratorMP3 *mp3B = nullptr;
AudioGeneratorWAV *wavA = nullptr;
AudioGeneratorWAV *wavB = nullptr;

AudioFileSourceFS *srcA = nullptr;
AudioFileSourceFS *srcB = nullptr;

// Legacy single‑channel pointers (if still used elsewhere)
AudioGeneratorMP3 *mp3 = nullptr;
AudioGeneratorWAV *wav = nullptr;
AudioFileSourceFATFS *file = nullptr;
AudioFileSourceID3 *id3 = nullptr;

// State
bool sequenceActive = false;
bool playlistActive = false;
bool audioStopRequested = false;
int sequenceIndex = 0;
int playlistIndex = 0;
unsigned long sequenceDelayUntil = 0;
bool sequenceWaitingForDelay = false;

// Mixer
AudioOutputMixer *mixer = nullptr;
AudioOutputMixerStub *mixA = nullptr;  // Sequence
AudioOutputMixerStub *mixB = nullptr;  // Playlist

AudioOutputI2SNoDAC *out = nullptr;

uint32_t loadWavSpeed() {
  const char *path = "/wavspeed.txt";
  if (!FFat.exists(path)) {
    Serial.println("wavspeed.txt missing — using default 11025");
    return 11025;
  }
  File f = FFat.open(path, "r");
  if (!f) {
    Serial.println("Failed to open wavspeed.txt — using default 11025");
    return 11025;
  }
  String line = f.readStringUntil('\n');
  f.close();
  line.trim();
  uint32_t rate = line.toInt();
  if (rate < 1000 || rate > 48000) {
    Serial.printf("Invalid wavspeed '%s' — using default 11025\n", line.c_str());
    return 11025;
  }
  Serial.printf("Loaded WAV speed: %u Hz\n", rate);
  return rate;
}

void setupAudioMixer() {
  out = new AudioOutputI2SNoDAC();
  out->SetPinout(I2S_BCLK, I2S_LRCLK, AUDIOOUT);
  out->SetChannels(1);
  rate = loadWavSpeed();
  Serial.printf(" Setting WAV rate at %u", rate);
  out->SetRate(rate);
  mixer = new AudioOutputMixer(32, out);
  mixA = mixer->NewInput();
  mixB = mixer->NewInput();
  audioReady = true;
  // startPlaylistFile("/F1.wav");
  // playlistActive = false;   // don’t advance

  startFile("/F1.wav", 1);

  unsigned long t0 = millis();
  while (millis() - t0 < 50) {  // 50 ms of real audio pumping
    audioLoop();                // THIS is what actually warms the mixer
  }
  if (wavB) wavB->stop();
  delete wavB;
  wavB = nullptr;
  delete srcB;
  srcB = nullptr;
  Serial.println("Audio mixer initialized.");
}

// ======================================================
// Helper to start a file on each channel
// ======================================================
bool startFile(const String &filenameIn, int channel) {
  bool ok = false;

  // Select channel pointers
  AudioGeneratorMP3 *&mp3 = (channel == 0 ? mp3A : mp3B);
  AudioGeneratorWAV *&wav = (channel == 0 ? wavA : wavB);
  AudioFileSourceFS *&src = (channel == 0 ? srcA : srcB);
  AudioOutputMixerStub *&mix = (channel == 0 ? mixA : mixB);

  const char *tag = (channel == 0 ? "[SEQ]" : "[PL]");

  // Normalize filename
  String filename = filenameIn;
  if (!filename.startsWith("/")) filename = "/" + filename;

  // Cleanup previous decoders
  if (mp3) {
    mp3->stop();
    delete mp3;
    mp3 = nullptr;
  }
  if (wav) {
    wav->stop();
    delete wav;
    wav = nullptr;
  }
  if (src) {
    delete src;
    src = nullptr;
  }

  Serial.printf("%s Opening '%s'\n", tag, filename.c_str());
  Serial.printf("%s FFat.exists: %d\n", tag, FFat.exists(filename));

  // Create new file source
  src = new AudioFileSourceFS(FFat, filename.c_str());
  if (!src) {
    Serial.printf("%s Failed to alloc AudioFileSourceFS\n", tag);
    return false;
  }

  // --- MP3 ---
  if (filename.endsWith(".mp3") || filename.endsWith(".MP3")) {
    mp3 = new AudioGeneratorMP3();
    ok = mp3->begin(src, mix);
    return ok;
  }

  // --- WAV ---
  wav = new AudioGeneratorWAV();
  ok = wav->begin(src, mix);

  // Apply rate AFTER begin()
  out->SetRate(rate);
  Serial.printf("%s SetRate: %d\n", tag, rate);

  return ok;
}

// ======================================================
// Public API: Start Sequence (non‑blocking)
// ======================================================
void startSequence() {
  if (!loadSequence()) {
    Serial.println("No valid sequence.");
    return;
  }
  sequenceIndex = 0;
  sequenceActive = true;
  sequenceWaitingForDelay = false;
  auto &item = playSequence[0];
  if (item.delayMs > 0) {
    sequenceDelayUntil = millis() + item.delayMs;
    sequenceWaitingForDelay = true;
    return;
  }

  startFile(item.filename, 0);
}

// ======================================================
// Public API: Start Playlist (non‑blocking)
// ======================================================
void startPlaylist() {
  if (!loadPlaylist()) {
    Serial.println("Playlist missing or empty");
    return;
  }
  playlistIndex = 0;
  playlistActive = true;
  startFile(playlist[0], 1);
}

// ======================================================
// Public API: Stop everything
// ======================================================
void stopAllAudio() {
  if (mp3A) {
    mp3A->stop();
    delete mp3A;
    mp3A = nullptr;
  }
  if (mp3B) {
    mp3B->stop();
    delete mp3B;
    mp3B = nullptr;
  }
  if (wavA) {
    wavA->stop();
    delete wavA;
    wavA = nullptr;
  }
  if (wavB) {
    wavB->stop();
    delete wavB;
    wavB = nullptr;
  }

  sequenceActive = false;
  playlistActive = false;

  Serial.println("All audio stopped.");
}

// ======================================================
// Unified Non‑Blocking Audio Loop
// Call this from loop()
// ======================================================
void audioLoop() {
  if (!audioReady) return;
  if (!mixer || !out) return;

  // ============================
  // SEQUENCE DELAY HANDLING
  // ============================
  if (sequenceActive && sequenceWaitingForDelay) {
    if (millis() >= sequenceDelayUntil) {
      sequenceWaitingForDelay = false;

      if (sequenceIndex >= 0 && sequenceIndex < (int)playSequence.size()) {
        auto &item = playSequence[sequenceIndex];
        startFile(item.filename, 0);
      } else {
        sequenceActive = false;
        Serial.println("[SEQ] Sequence index out of range after delay");
      }
    }
  }

  // ============================
  // CHANNEL A (Sequence)
  // ============================
  if (mp3A) {
    mp3A->loop();

    if (srcA && srcA->getPos() >= srcA->getSize()) {
      Serial.println("[AL] MP3 A finished");
      mp3A->stop();
      delete mp3A;
      mp3A = nullptr;

      if (sequenceActive) {
        sequenceIndex++;
        if (sequenceIndex < (int)playSequence.size()) {
          auto &item = playSequence[sequenceIndex];
          if (item.delayMs > 0) {
            sequenceDelayUntil = millis() + item.delayMs;
            sequenceWaitingForDelay = true;
          } else {
            startFile(item.filename, 0);
          }
        } else {
          sequenceActive = false;
          Serial.println("[SEQ] Sequence finished");
        }
      }
    }
  } else if (wavA) {
    wavA->loop();

    if (srcA && srcA->getPos() >= srcA->getSize()) {
      Serial.println("[AL] WAV A finished");
      wavA->stop();
      delete wavA;
      wavA = nullptr;

      if (sequenceActive) {
        sequenceIndex++;
        if (sequenceIndex < (int)playSequence.size()) {
          auto &item = playSequence[sequenceIndex];
          if (item.delayMs > 0) {
            sequenceDelayUntil = millis() + item.delayMs;
            sequenceWaitingForDelay = true;
          } else {
            startFile(item.filename, 0);
          }
        } else {
          sequenceActive = false;
          Serial.println("[SEQ] Sequence finished");
        }
      }
    }
  }

  // ============================
  // CHANNEL B (Playlist)
  // ============================
  if (mp3B) {
    mp3B->loop();

    if (srcB && srcB->getPos() >= srcB->getSize()) {
      Serial.println("[AL] MP3 B finished");
      mp3B->stop();
      delete mp3B;
      mp3B = nullptr;

      if (playlistActive) {
        playlistIndex++;
        if (playlistIndex < (int)playlist.size()) {
          startFile(playlist[playlistIndex], 1);
        } else {
          playlistActive = false;
          Serial.println("[PL] Playlist finished");
        }
      }
    }
  } else if (wavB) {
    wavB->loop();

    if (srcB && srcB->getPos() >= srcB->getSize()) {
      Serial.println("[AL] WAV B finished");
      wavB->stop();
      delete wavB;
      wavB = nullptr;

      if (playlistActive) {
        playlistIndex++;
        if (playlistIndex < (int)playlist.size()) {
          startFile(playlist[playlistIndex], 1);
        } else {
          playlistActive = false;
          Serial.println("[PL] Playlist finished");
        }
      }
    }
  }

  // ============================
  // MIXER
  // ============================
  if (mixer && (mp3A || wavA || mp3B || wavB)) {
    mixer->loop();
  }

  // ============================
  // AUDIO ACTIVE FLAG
  // ============================
  audioIsActive = (mp3A || wavA || mp3B || wavB);
}

// ---------------- SEQUENCE LOADING ----------------
bool loadPlaylist() {
  playlist.clear();

  File f = FFat.open("/playlist.txt", "r");
  if (!f) return false;

  while (f.available()) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (line.length() > 0) playlist.push_back(line);
  }

  f.close();
  return !playlist.empty();
}

bool loadSequence() {
  playSequence.clear();

  if (!FFat.exists(SEQUENCE_JSON_PATH)) {
    Serial.println("sequence.json missing — creating default");
    createDefaultSequence();
  }

  File f = FFat.open(SEQUENCE_JSON_PATH, "r");
  if (!f) return false;

  StaticJsonDocument<4096> doc;
  DeserializationError err = deserializeJson(doc, f);
  f.close();
  if (err) return false;

  // ============================
  // NEW FORMAT: { "blocks": [ ... ] }
  // ============================
  if (doc.containsKey("blocks") && doc["blocks"].is<JsonArray>()) {
    JsonArray blocks = doc["blocks"].as<JsonArray>();

    for (JsonObject block : blocks) {
      uint32_t repeat = block["repeat"] | 1;

      if (!block.containsKey("steps")) continue;
      JsonArray steps = block["steps"].as<JsonArray>();

      for (uint32_t r = 0; r < repeat; r++) {
        for (JsonVariant v : steps) {
          SequenceItem item;

          if (v.is<const char *>()) {
            item.filename = v.as<const char *>();
            item.delayMs = 0;
          } else if (v.is<JsonObject>()) {
            JsonObject obj = v.as<JsonObject>();
            item.filename = obj["file"] | "";
            item.delayMs = obj["delay_ms"] | 0;
          }

          if (item.filename.length()) {
            playSequence.push_back(item);
          }
        }
      }
    }

    return !playSequence.empty();
  }

  // ============================
  // OLD FORMAT (backward compatible)
  // ============================
  if (!doc.containsKey("steps") || !doc["steps"].is<JsonArray>()) {
    return false;
  }

  uint32_t repeat = doc["repeat"] | 1;
  JsonArray steps = doc["steps"].as<JsonArray>();

  for (uint32_t r = 0; r < repeat; r++) {
    for (JsonObject obj : steps) {
      SequenceItem item;
      item.filename = obj["file"] | "";
      item.delayMs = obj["delay_ms"] | 0;

      if (item.filename.length()) {
        playSequence.push_back(item);
      }
    }
  }

  return !playSequence.empty();
}

void Setupffat() {
  if (!FFat.begin()) {
    Serial.println("FFat mount failed — attempting format");
    if (FFat.format()) {
      Serial.println("FFat format succeeded — retrying mount");
      if (!FFat.begin()) {
        Serial.println("FFat mount still failed after format");
        //return;
      }
    } else {
      Serial.println("FFat format failed");
      //return;
    }
  } else {
    Serial.println("FFat Installed");
  }
}

// ---------------- ID3 CALLBACK to shwo Mp3 data etc----------------
void MDCallback(void *cbData, const char *type, bool isUnicode, const char *string) {
  Serial.printf("ID3: %s = '", type);
  if (isUnicode) string += 2;
  while (*string) {
    char c = *string++;
    if (isUnicode) string++;
    Serial.printf("%c", c);
  }
  Serial.println("'");
}

void createDefaultSequence() {
  StaticJsonDocument<512> doc;

  doc["repeat"] = 1;
  JsonArray steps = doc.createNestedArray("steps");

  JsonObject s1 = steps.createNestedObject();
  s1["file"] = "/test.wav";
  s1["delay_ms"] = 0;

  File f = FFat.open(SEQUENCE_JSON_PATH, "w");
  if (!f) {
    Serial.println("Failed to create default sequence.json");
    return;
  }

  serializeJsonPretty(doc, f);
  f.close();

  Serial.println("Default sequence.json created");
}
void stopAudioNow() {
  audioStopRequested = true;
}

//end audio ...
//LEDS
bool audioRunning() {
  return audioIsActive;
}

void allLedsOff() {
  for (int i = 0; i < ledCount; i++) {
    digitalWrite(ledPins[i], HIGH);  // OFF (active-low)
  }
}

void ledLoop() {
  static uint32_t lastPrint = 0;
  uint32_t now = millis();
  // Print every 200ms so we can see state clearly
  // if (now - lastPrint >= 200) {  lastPrint = now;  Serial.printf("[LED] audioIsActive=%d  ledIndex=%d  now=%u\n", audioIsActive, ledIndex, now); // }
  // If audio is not running, turn LEDs off
  if (!audioRunning()) {
    // Only print when entering this state
    static bool wasRunning = false;
    if (wasRunning) {
      //  Serial.println("[LED] AUDIO STOPPED → turning all LEDs OFF");
      wasRunning = false;
    }
    allLedsOff();
    return;
  }
  // Audio is running
  static bool wasStopped = true;
  if (wasStopped) {
    //  Serial.println("[LED] AUDIO RUNNING → starting LED chase");
    wasStopped = false;
  }
  // LED chase timing
  if (now - lastLedStep >= ledInterval) {
    lastLedStep = now;
    // Serial.printf("[LED] STEP → ledIndex=%d\n", ledIndex);
    // turn all off
    for (int i = 0; i < ledCount; i++) {
      digitalWrite(ledPins[i], HIGH);
    }
    // turn one on
    digitalWrite(ledPins[ledIndex], LOW);
    ledIndex++;
    if (ledIndex >= ledCount) ledIndex = 0;
  }
}

void ledTask(void *param) {
  for (;;) {
    ledLoop();       // your existing LED logic
    vTaskDelay(10);  // 10ms tick (adjust as needed)
  }
}
// ---------------- WIFI CONFIG ----------------
bool loadWifiConfig(String &ssid, String &password) {
  if (!FFat.exists(WIFI_JSON_PATH)) return false;
  File f = FFat.open(WIFI_JSON_PATH, "r");
  if (!f) return false;
  StaticJsonDocument<256> doc;
  if (deserializeJson(doc, f)) return false;
  ssid = doc["ssid"] | "";
  password = doc["password"] | "";
  return ssid.length() > 0;
}
bool saveWifiConfig(const String &ssid, const String &password) {
  StaticJsonDocument<256> doc;
  doc["ssid"] = ssid;
  doc["password"] = password;

  File f = FFat.open(WIFI_JSON_PATH, "w");
  if (!f) return false;

  if (serializeJson(doc, f) == 0) {
    f.close();
    return false;
  }

  f.close();
  return true;
}

bool connectSTA() {

  // Try loading JSON config
  if (loadWifiConfig(ssid, pass)) {
    Serial.printf("Loaded STA config: %s\n", ssid.c_str());
  } else {
    Serial.println("No wifi.json found, using defaults.");
    ssid = "GUESTBOAT";
    pass = "12345678";
    setwifidefault = true;
  }
WiFi.setAutoReconnect(false);
WiFi.persistent(false);

  WiFi.begin(ssid.c_str(), pass.c_str());
  Serial.printf("Connecting to STA: %s\n", ssid.c_str());

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("STA connected: ");
    Serial.println(WiFi.localIP());
    return true;
  }

  Serial.println("STA connect failed.");
  return false;
}


void sendHomePage() {
  // Determine STA status
  bool staConnected = (WiFi.status() == WL_CONNECTED);

  // Always valid: AP IP
  String apIP = WiFi.softAPIP().toString();

  // STA IP only valid if connected
  String staIP = staConnected ? WiFi.localIP().toString() : "Not connected";

  // File manager should ALWAYS use AP IP if STA is not connected
  String fileMgrIP = staConnected ? staIP : apIP;

  String html =
    "<!DOCTYPE html>"
    "<html>"
    "<head><title>ESP32S3 Audio Player</title></head>"
    "<body style='font-family: sans-serif;'>"
    "<h2>ESP32S3 Audio Player</h2>"

    "<p><b>AP Address:</b> "
    + apIP + "</p>"
             "<p><b>STA Address:</b> "
    + staIP + "</p>"
              "<p>"
              "<a href='/playsequence' "
              "style='display:inline-block;padding:10px 16px;"
              "background:#28a745;color:white;text-decoration:none;"
              "border-radius:6px;margin-right:10px;'>"
              "Play Sequence"
              "</a>"

              "<a href='/playplaylist' "
              "style='display:inline-block;padding:10px 16px;"
              "background:#17a2b8;color:white;text-decoration:none;"
              "border-radius:6px;'>"
              "Play Playlist"
              "</a>"
              "</p>"

              "<p>"
              "<a href='http://"
    + fileMgrIP + ":8080/' "
                  "style='display:inline-block;padding:10px 16px;"
                  "background:#007bff;color:white;text-decoration:none;"
                  "border-radius:6px;'>"
                  "Open File Manager"
                  "</a>"
                  "</p>"
                  "<p>Uses 'ESPFMfGK' to manage files.</p>"

                  "</body>"
                  "</html>";

  server.send(200, "text/html", html);
}

void ServerStart() {
  server.on("/playplaylist", HTTP_GET, []() {
    if (!loadPlaylist()) {
      server.send(500, "text/plain", "Playlist missing or empty");
      return;
    }
    startPlaylist();
    server.send(200, "text/html",
                "<html><head><meta http-equiv='refresh' content='0; url=/' /></head>"
                "<body>Playlist started.</body></html>");
  });


  server.on("/playsequence", HTTP_GET, []() {
    startSequence();
    server.send(200, "text/html",
                "<html><head><meta http-equiv='refresh' content='0; url=/' /></head>"
                "<body>Sequence finished.</body></html>");
  });

  server.on("/playplaylist", HTTP_GET, []() {
    if (!loadPlaylist()) {
      server.send(500, "text/plain", "Playlist missing or empty");
      return;
    }
    startPlaylist();
    server.send(200, "text/html",
                "<html><head><meta http-equiv='refresh' content='0; url=/' /></head>"
                "<body>Playlist started.</body></html>");
  });


  server.on("/", HTTP_GET, sendHomePage);
  server.on("/HOME", HTTP_GET, sendHomePage);

  server.begin();
}



void listFFatDir(const char *dirname, uint8_t levels = 5) {
  Serial.printf("Listing FFat directory: %s\n", dirname);

  File root = FFat.open(dirname);
  if (!root) {
    Serial.println("Failed to open directory");
    return;
  }
  if (!root.isDirectory()) {
    Serial.println("Not a directory");
    return;
  }

  File file = root.openNextFile();
  while (file) {
    if (file.isDirectory()) {
      Serial.printf("  DIR : %s\n", file.name());
      if (levels) {
        listFFatDir(file.name(), levels - 1);
      }
    } else {
      Serial.printf("  FILE: %s  SIZE: %u\n", file.name(), (unsigned)file.size());
    }
    file = root.openNextFile();
  }
}

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("=== SETUP START ===");

  setwifidefault = false;

  // ---------------- LED STARTUP SEQUENCE ----------------
  for (int i = 0; i < ledCount; i++) {
    pinMode(ledPins[i], OUTPUT);
    digitalWrite(ledPins[i], HIGH);  // OFF
  }
  pinMode(TRIGGER_PIN, INPUT_PULLUP);

  for (int i = 0; i < ledCount; i++) {
    digitalWrite(ledPins[i], LOW);   // ON
    delay(120);
    digitalWrite(ledPins[i], HIGH);  // OFF
  }

  // ---------------- 0) MOUNT FFAT FIRST ----------------
  addFileSystems();   // FFat.begin() + filemgr.addFS(FFat, "/") happens here
  delay(100);

  // ---------------- 1) WIFI AP+STA MODE ----------------
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(AP_SSID, AP_PASS);
  delay(500);

  Serial.print("AP IP before file manager: ");
  Serial.println(WiFi.softAPIP());

  // ---------------- 2) AUDIO MIXER (SAFE AFTER FFAT) ----------------
  setupAudioMixer();  // calls loadWavSpeed() from FFat

  // ---------------- 3) TRY STA CONNECTION ONCE ----------------
  // Make STA non‑intrusive: no auto‑reconnect, no persistence
  WiFi.setAutoReconnect(false);
  WiFi.persistent(false);

  bool staOK = connectSTA();
  if (!staOK) {
    Serial.println("Continuing with AP only.");
  }

  // ---------------- 4) START FILE MANAGER (AS BEFORE) ----------------
  // This is the ordering that used to work for you:
  //   FFat mounted -> AP up -> STA attempted -> file manager started
  setupFilemanager();

  // ---------------- 5) CREATE DEFAULT wifi.json IF NEEDED ----------------
  if (setwifidefault) {
    if (saveWifiConfig(ssid, pass)) {
      Serial.println("Created default wifi.json");
    } else {
      Serial.println("Failed to create default wifi.json");
    }
    setwifidefault = false;
  }

  // ---------------- 6) START LED TASK ----------------
  xTaskCreatePinnedToCore(
    ledTask,
    "LED Task",
    4096,
    NULL,
    1,
    NULL,
    0);

  // ---------------- 7) START WEB SERVER ----------------
  ServerStart();

  // ---------------- 8) START SEQUENCE ----------------
  startSequence();

  Serial.println("=== SETUP END - will now play sequence once ===");
}


// void setup() {
//   Serial.begin(115200);
//   delay(500);
//   Serial.println("=== SETUP START ===");
//   setwifidefault = false;
//   // LEDs
//   for (int i = 0; i < ledCount; i++) {
//     pinMode(ledPins[i], OUTPUT);
//     digitalWrite(ledPins[i], HIGH);  // OFF
//   }
//   pinMode(TRIGGER_PIN, INPUT_PULLUP);

//   for (int i = 0; i < ledCount; i++) {
//     digitalWrite(ledPins[i], LOW);  // ON
//     delay(120);
//     digitalWrite(ledPins[i], HIGH);  // OFF
//   }
//   // 0) MOUNT FFat BEFORE ANY FILE ACCESS
//   addFileSystems();  // FFat.begin() happens here
//   delay(100);
//   //optional  listFFatDir("/");
//   // 1) WIFI AP
//   WiFi.mode(WIFI_AP_STA);
//   WiFi.softAP(AP_SSID, AP_PASS);
//   delay(500);
//   // 2) NOW AUDIO CAN SAFELY READ wavspeed.txt
//   setupAudioMixer();  // calls loadWavSpeed()
//   // 3) CONNECT STA (reads wifi.json from FFat)
//   bool staOK = connectSTA();
//   if (!staOK) {
//     Serial.println("Continuing with AP only.");
//   }
//   // 4) START FILEMANAGER AFTER STA IS UP (so WiFi.status() == WL_CONNECTED)
//   Serial.print("AP IP before file manager: ");
//   Serial.println(WiFi.softAPIP());
//   IPAddress bindIP;

//   if (WiFi.status() == WL_CONNECTED) {
//     bindIP = WiFi.localIP();  // STA connected
//     Serial.print("Filemanager binding to STA IP: ");
//   } else {
//     bindIP = WiFi.softAPIP();  // AP only
//     Serial.print("Filemanager binding to AP IP: ");
//   }

//   Serial.println(bindIP);

//   filemgr.setIP(bindIP);
//   setupFilemanager();

//   // 5) ONLY NOW, IF NEEDED, CREATE DEFAULT wifi.json
//   if (setwifidefault) {
//     if (saveWifiConfig(ssid, pass)) {
//       Serial.println("Created default wifi.json");
//     } else {
//       Serial.println("Failed to create default wifi.json");
//     }
//     setwifidefault = false;
//   }
//   // LED task
//   xTaskCreatePinnedToCore(
//     ledTask,
//     "LED Task",
//     4096,
//     NULL,
//     1,
//     NULL,
//     0);

//   ServerStart();
//   startSequence();
//   Serial.println("=== SETUP END - will now play sequence once ===");
// }
// ---------------- LOOP ----------------
void loop() {
  filemgr.handleClient();
  server.handleClient();
  audioLoop();
  if (digitalRead(TRIGGER_PIN) == LOW) {
    static uint32_t last = 0;
    if (millis() - last > 500) {
      last = millis();
      playRequested = true;
    }
  }
  if (playRequested) {
    playRequested = false;
    startPlaylist();
  }
}
/* 
PROMPT I would like to write a new sketch for ESP32S3, with 4M memory. 
It should use the libraries "ESP32 File Manager for Generation Klick" to access FFAT, And ESP8266Audio . 
It should play sound files from the FFAT when triggered. 
It should provide an AP but also allow connecting to a STA, and 
the STA and Passowrd should be stored in a JSON in the FFAT. (so they can be accessed and edited  via the filemanger). 
A file in the FFAT should specify which sound file to play, and a sequence and timing. I will need a copy paste .ino..
.. this "created" an initial mono result that largely worked. 
then later I added ...
I would like two channel sound (- not stereo ).
so that if I request (eg) a playlist, whilst sequence is playing, a second channel will be opened and the playlist played as well as the sequence.
.. this took a lot more prompting to cure faults and get to the working state above.


*/
