#include <WiFi.h>
#include <FS.h>
// Remove the file systems that are not needed.
#include <SD.h>
#include <LittleFS.h>
#include <SD_MMC.h>
#include <FFat.h>
#include <SPI.h>
// the thing.
#include <ESPFMfGK.h>

// ---------------- AUDIO (ESP8266Audio) ----------------
#include "AudioFileSourceFATFS.h"
#include "AudioFileSourceID3.h"
#include "AudioGeneratorMP3.h"
#include "AudioOutputI2SNoDAC.h"
#include "AudioGeneratorWAV.h"


// ---------------- JSON ----------------
#include <ArduinoJson.h>

// ---------------- FILE MANAGER ----------------
#include <WebServer.h>
#include <ESPFMfGK.h>

// ---------------- CONFIG ----------------
#define TRIGGER_PIN 0
#define AP_SSID "ESP32S3-Audio"
#define AP_PASS "12345678"
// Choose two unused pins for BCLK and LRCLK.
// They are not used electrically in the one‑transistor circuit.
#define I2S_BCLK 4
#define I2S_LRCLK 5
#define AUDIOOUT 6  // your defined output pin
const int ledPins[] = { 13, 12, 11, 10, 9 };
const int ledCount = sizeof(ledPins) / sizeof(ledPins[0]);



#define WIFI_JSON_PATH "/wifi.json"
#define SEQUENCE_JSON_PATH "/sequence.json"

// ---------------- GLOBALS ----------------
WebServer server(80);
const word filemanagerport = 8080;
// we want a different port than the webserver
ESPFMfGK filemgr(filemanagerport);

AudioGeneratorMP3 *mp3;
AudioFileSourceFATFS *file;
AudioOutputI2SNoDAC *out;
AudioFileSourceID3 *id3;
AudioGeneratorWAV *wav = nullptr;


bool playRequested = false;
bool audioIsActive = false;

struct PlayItem {
  String filename;
  uint32_t delayMs;
};

uint32_t lastLedStep = 0;
int ledIndex = 0;
const uint32_t ledInterval = 200;  // ms per step (tune this)


std::vector<PlayItem> playSequence;

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
  // if (now - lastPrint >= 200) {
  //   lastPrint = now;
  //   Serial.printf("[LED] audioIsActive=%d  ledIndex=%d  now=%u\n",
  //                 audioIsActive, ledIndex, now);
  // }

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

// ---------------- ID3 CALLBACK ----------------
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
bool connectSTA() {
  String ssid, pass;

  // Try loading JSON config
  if (loadWifiConfig(ssid, pass)) {
    Serial.printf("Loaded STA config: %s\n", ssid.c_str());
  } else {
    Serial.println("No wifi.json found, using defaults.");

    ssid = "GUESTBOAT";
    pass = "12345678";
  }

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


// ---------------- SEQUENCE LOADING ----------------
#include <FFat.h>

std::vector<String> playlist;

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

bool loadSequence() {
  playSequence.clear();

  if (!FFat.exists(SEQUENCE_JSON_PATH)) {
    Serial.println("sequence.json missing — creating default");
    createDefaultSequence();
  }

  File f = FFat.open(SEQUENCE_JSON_PATH, "r");
  if (!f) return false;

  StaticJsonDocument<2048> doc;
  DeserializationError err = deserializeJson(doc, f);
  f.close();
  if (err) return false;

  // Expect: { "repeat": N, "steps": [ ... ] }
  if (!doc.containsKey("steps") || !doc["steps"].is<JsonArray>()) {
    return false;
  }

  uint32_t repeat = doc["repeat"] | 1;
  JsonArray steps = doc["steps"].as<JsonArray>();

  for (uint32_t r = 0; r < repeat; r++) {
    for (JsonObject obj : steps) {
      PlayItem item;
      item.filename = obj["file"] | "";
      item.delayMs = obj["delay_ms"] | 0;

      if (item.filename.length()) {
        playSequence.push_back(item);
      }
    }
  }

  return !playSequence.empty();
}



// ---------------- AUDIO ----------------
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



void stopAudio() {
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

  if (id3) {
    delete id3;
    id3 = nullptr;
  }

  if (file) {
    delete file;
    file = nullptr;
  }

  if (out) {
    out->stop();  // important: stop I2S / DMA
    delete out;
    out = nullptr;
  }

  audioIsActive = false;
}






bool playFile(const String &path) {
  stopAudio();  // clears mp3, id3, file, etc.

  if (!FFat.exists(path)) {
    Serial.printf("File not found: %s\n", path.c_str());
    return false;
  }

  file = new AudioFileSourceFATFS(path.c_str());

  // Determine file type by extension
  String ext = path.substring(path.lastIndexOf('.') + 1);
  ext.toLowerCase();

  if (out) {
    delete out;
    out = nullptr;
  }

  out = new AudioOutputI2SNoDAC();
  out->SetPinout(4, 5, AUDIOOUT);

  if (ext == "mp3") {
    id3 = new AudioFileSourceID3(file);
    id3->RegisterMetadataCB(MDCallback, (void *)"ID3");

    mp3 = new AudioGeneratorMP3();
    if (!mp3->begin(id3, out)) {
      Serial.println("MP3 begin failed.");
      stopAudio();
      return false;
    }
    audioIsActive = true;  // <-- ADD THIS
    Serial.printf("Playing MP3: %s\n", path.c_str());
    return true;
  }

  else if (ext == "wav") {
    wav = new AudioGeneratorWAV();
    if (!wav->begin(file, out)) {
      Serial.println("WAV begin failed.");
      stopAudio();
      return false;
    }
    // Apply WAV header (11.025Khz etc sampiling) parameters to I2S
    uint32_t rate = loadWavSpeed();
    out->SetRate(rate);    // my wavs are all 11.025 khz for compactness
    out->SetChannels(1);   //my wavs are MONO
    audioIsActive = true;  // <-- ADD THIS
    Serial.printf("Playing WAV: %s @ %ukHz\n", path.c_str(), rate);
    return true;
  }

  Serial.printf("Unsupported file type: %s\n", ext.c_str());
  return false;
}


void audioLoop() {
  bool running = false;

  if (mp3 && mp3->isRunning()) {
    running = mp3->loop();
  }

  if (wav && wav->isRunning()) {
    running = wav->loop();
  }
  audioIsActive = running;
}



// ---------------- PLAY SEQUENCE ----------------
void playSequenceNow() {
  if (!loadSequence()) {
    Serial.println("No valid sequence.");
    return;
  }

  for (auto &item : playSequence) {

    if (item.delayMs)
      delay(item.delayMs);

    if (!playFile(item.filename)) {
      Serial.printf("Failed to play: %s\n", item.filename.c_str());
      continue;
    }

    // Drive decoder until it actually finishes
    while (true) {
      bool running = false;

      if (mp3) running = mp3->loop();
      if (wav) running = wav->loop();

      if (!running) {
        Serial.println("[AUDIO] Decoder finished cleanly");
        break;
      }

      delay(5);
    }
  }
  audioIsActive = false;
  stopAudio();  // <-- make sure this really stops I2S
  Serial.println("Sequence finished.");
}


void playPlaylistNow() {
  if (!loadPlaylist()) {
    Serial.println("Playlist missing or empty");
    return;
  }

  for (auto &filename : playlist) {

    if (!FFat.exists(filename)) {
      Serial.printf("File not found: %s\n", filename.c_str());
      continue;
    }

    if (!playFile(filename)) {
      Serial.printf("Failed to play: %s\n", filename.c_str());
      continue;
    }

    // Drive decoder until it actually finishes
    while (true) {
      bool running = false;

      if (mp3) running = mp3->loop();
      if (wav) running = wav->loop();

      if (!running) {
        Serial.println("[AUDIO] Decoder finished cleanly");
        break;
      }

      delay(5);
    }
  }
  audioIsActive = false;
  stopAudio();  // <-- make sure this really stops I2S
  Serial.println("Playlist finished.");

  server.send(200, "text/html",
              "<html><head>"
              "<meta http-equiv='refresh' content='0; url=/' />"
              "</head><body>Playlist finished.</body></html>");
}


void sendHomePage() {
  String html =
    "<!DOCTYPE html>"
    "<html>"
    "<head><title>ESP32S3 Audio Player</title></head>"
    "<body style='font-family: sans-serif;'>"
    "<h2>ESP32S3 Audio Player</h2>"

    "<p>Use ESPFMfGK to manage files.</p>"

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
    + WiFi.localIP().toString() + ":8080/' "
                                  "style='display:inline-block;padding:10px 16px;"
                                  "background:#007bff;color:white;text-decoration:none;"
                                  "border-radius:6px;'>"
                                  "Open File Manager"
                                  "</a>"
                                  "</p>"

                                  "</body>"
                                  "</html>";

  server.send(200, "text/html", html);
}




void ServerStart() {

  server.on("/play", HTTP_GET, []() {
    if (!loadPlaylist()) {
      server.send(500, "text/plain", "Playlist missing or empty");
      return;
    }
    playPlaylistNow();
    server.send(200, "text/plain", "Playlist started");
  });

  server.on("/playsequence", HTTP_GET, []() {
    playSequenceNow();
    server.send(200, "text/html",
                "<html><head><meta http-equiv='refresh' content='0; url=/' /></head>"
                "<body>Sequence finished.</body></html>");
  });

  server.on("/playplaylist", HTTP_GET, []() {
    if (!loadPlaylist()) {
      server.send(500, "text/plain", "Playlist missing or empty");
      return;
    }
    playPlaylistNow();
  });

  server.on("/", HTTP_GET, sendHomePage);
  server.on("/HOME", HTTP_GET, sendHomePage);

  server.begin();
}

void ledTask(void *param) {
  for (;;) {
    ledLoop();       // your existing LED logic
    vTaskDelay(10);  // 10ms tick (adjust as needed)
  }
}



void setup() {
  Serial.begin(115200);
  delay(500);
  for (int i = 0; i < ledCount; i++) {
    pinMode(ledPins[i], OUTPUT);
    digitalWrite(ledPins[i], HIGH);  // LEDs OFF (wired to VCC)
  }

  pinMode(TRIGGER_PIN, INPUT_PULLUP);
  // Startup LED chase (active‑LOW)
  for (int i = 0; i < ledCount; i++) {
    digitalWrite(ledPins[i], LOW);  // LED ON
    delay(120);
    digitalWrite(ledPins[i], HIGH);  // LED OFF
  }

  // // AUDIO
  out = new AudioOutputI2SNoDAC();
  out->SetPinout(4, 5, AUDIOOUT);

  // WIFI
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(AP_SSID, AP_PASS);

  delay(500);  // allow AP to stabilise

  // Try STA connection (JSON or fallback)
  bool staOK = connectSTA();

  if (!staOK) {
    Serial.println("Continuing with AP only.");
  }

  delay(500);  // ensure IP stack ready
  Serial.print("AP IP before file manager: ");
  Serial.println(WiFi.softAPIP());

  addFileSystems();
  setupFilemanager();
  xTaskCreatePinnedToCore(
    ledTask,     // task function
    "LED Task",  // name
    4096,        // stack size
    NULL,        // parameter
    1,           // priority
    NULL,        // task handle
    0            // run on core 0
  );



  ServerStart();
}

// ---------------- LOOP ----------------
void loop() {
  // your existing loop content
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
    playSequenceNow();
  }
}
/* 
PROMPT I would like to write a new sketch for ESP32S3, with 4M memory. 
It should use the libraries "ESP32 File Manager for Generation Klick" to access FFAT, And ESP8266Audio . 
It should play sound files from the FFAT when triggered. 
It should provide an AP but also allow connecting to a STA, and 
the STA and Passowrd should be stored in a JSON in the FFAT. (so they can be accessed and edited  via the filemanger). 
A file in the FFAT should specify which sound file to play, and a sequence and timing. I will need a copy paste .ino
*/
