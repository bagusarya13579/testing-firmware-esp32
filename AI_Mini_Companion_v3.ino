#include <Wire.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Adafruit_MPU6050.h>
#include <driver/i2s_std.h>
#include "Audio.h"
#include <WiFiManager.h>

// ================= KONFIGURASI GROQ API =================
const char* GROQ_API_KEY  = "isi sendiri";
const char* GROQ_CHAT_URL = "isi sendiri";

// ================= DEFINISI PIN HARDWARE ESP32-C3 =================
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET    -1
#define OLED_ADDRESS  0x3C

// PERBAIKAN: Menggunakan GPIO 0 & GPIO 10 (Aman dari Strapping Pin GPIO 9 & 8)
#define PIN_SDA   0
#define PIN_SCL   10
#define PIN_TOUCH 4

// Pin I2S INMP441 Microphone (Input)
#define MIC_I2S_WS   2
#define MIC_I2S_SCK  1
#define MIC_I2S_SD   3

// Pin I2S MAX98357A Speaker (Output)
#define SPK_I2S_LRC  5
#define SPK_I2S_BCLK 6
#define SPK_I2S_DIN  7

#define SAMPLE_RATE      16000
#define RECORD_TIME_SEC  3     // Disesuaikan ke 3 detik agar RAM sangat lega
#define WAV_HEADER_SIZE  44
#define PCM_DATA_SIZE    (SAMPLE_RATE * RECORD_TIME_SEC * 2)
#define TOTAL_AUDIO_SIZE (WAV_HEADER_SIZE + PCM_DATA_SIZE)

// Inisialisasi Objek Hardware
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
Adafruit_MPU6050 mpu;
Audio audio; 

i2s_chan_handle_t rx_handle = NULL;
uint8_t* wavBuffer = NULL;

enum EmotionState { NORMAL, HAPPY, SURPRISED, THINKING, RECORDING, TALKING };
EmotionState currentEmotion = NORMAL;
unsigned long lastStateChange = 0;

float eyeHeightCurrent = 32.0f;
float gazeXCurrent = 0.0f;
float gazeYCurrent = 0.0f;
float gazeXTarget = 0.0f;
float gazeYTarget = 0.0f;
unsigned long lastEyeFrame = 0;
unsigned long nextGazeChange = 0;
unsigned long nextBlinkAt = 0;
unsigned long blinkStartedAt = 0;
int animatedEmotion = -1;
bool blinkActive = false;

// ================= 1. KONFIGURASI WIFI DINAMIS (STABIL NO-BROWNOUT) =================
void setupWiFi() {
  WiFiManager wm;

  Serial.println("\n--- Memulai WiFiManager ---");

  // Hapus tanda komentar '//' pada baris di bawah jika Anda ingin MEMAKSA Access Point selalu muncul
  // (Mengabaikan/menghapus memori Wi-Fi rumah yang tersimpan sebelumnya):
  // wm.resetSettings();

  // Mode AP+STA dan atur daya pancar ke 11 dBm (Mencegah Brownout Reset di ESP32-C3 SuperMini)
  WiFi.mode(WIFI_AP_STA);
  WiFi.setTxPower(WIFI_POWER_11dBm);

  // Tampilan petunjuk di layar OLED
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.println("=== WIFI SETUP ===");
  display.println("Connect HP ke AP:");
  display.println("SSID: AI-Companion-AP");
  display.println("IP  : 192.168.4.1");
  display.display();

  // Timeout Portal 180 detik (3 menit). Jika tidak ada koneksi, sistem lanjut tanpa blokir program
  wm.setConfigPortalTimeout(180);

  // Coba terhubung ke Wi-Fi tersimpan, jika tidak ada/gagal baru buka AP "AI-Companion-AP"
  bool res = wm.autoConnect("AI-Companion-AP");

  if (!res) {
    Serial.println("Wi-Fi tidak terhubung / Timeout Portal. Lanjut mode offline.");
    display.clearDisplay();
    display.setCursor(0, 20);
    display.println("WiFi Timeout!");
    display.println("Running Offline...");
    display.display();
    delay(2000);
  } else {
    Serial.println("Wi-Fi Berhasil Terhubung!");
    display.clearDisplay();
    display.setCursor(0, 20);
    display.println("WiFi Connected!");
    display.println(WiFi.localIP().toString());
    display.display();
    delay(1500);
  }
}

// ================= 2. PEMBUATAN 44-BYTE WAV HEADER =================
void createWavHeader(uint8_t* header, uint32_t pcmDataLen) {
  uint32_t totalDataLen = pcmDataLen + 36;
  uint32_t byteRate = SAMPLE_RATE * 1 * 16 / 8;

  header[0] ='R'; header[1] ='I'; header[2] ='F'; header[3] ='F';
  header[4] = (uint8_t)(totalDataLen & 0xFF);
  header[5] = (uint8_t)((totalDataLen >> 8) & 0xFF);
  header[6] = (uint8_t)((totalDataLen >> 16) & 0xFF);
  header[7] = (uint8_t)((totalDataLen >> 24) & 0xFF);
  header[8] ='W'; header[9] ='A'; header[10]='V'; header[11]='E';
  header[12]='f'; header[13]='m'; header[14]='t'; header[15]=' ';
  header[16]= 16; header[17]= 0; header[18]= 0; header[19]= 0;
  header[20]= 1;  header[21]= 0;                               
  header[22]= 1;  header[23]= 0;                               
  header[24]= (uint8_t)(SAMPLE_RATE & 0xFF);
  header[25]= (uint8_t)((SAMPLE_RATE >> 8) & 0xFF);
  header[26]= (uint8_t)((SAMPLE_RATE >> 16) & 0xFF);
  header[27]= (uint8_t)((SAMPLE_RATE >> 24) & 0xFF);
  header[28]= (uint8_t)(byteRate & 0xFF);
  header[29]= (uint8_t)((byteRate >> 8) & 0xFF);
  header[30]= (uint8_t)((byteRate >> 16) & 0xFF);
  header[31]= (uint8_t)((byteRate >> 24) & 0xFF);
  header[32]= 2;  header[33]= 0;                               
  header[34]= 16; header[35]= 0;                               
  header[36]='d'; header[37]='a'; header[38]='t'; header[39]='a';
  header[40]= (uint8_t)(pcmDataLen & 0xFF);
  header[41]= (uint8_t)((pcmDataLen >> 8) & 0xFF);
  header[42]= (uint8_t)((pcmDataLen >> 16) & 0xFF);
  header[43]= (uint8_t)((pcmDataLen >> 24) & 0xFF);
}

// ================= 3. ANIMASI EKSPRESI MATA OLED =================
void drawEyes(EmotionState state) {
  unsigned long now = millis();
  if (now - lastEyeFrame < 25) return;
  lastEyeFrame = now;

  float targetHeight = 32.0f;
  float targetGazeX = 0.0f;
  float targetGazeY = 0.0f;

  switch (state) {
    case HAPPY:
      targetHeight = 27.0f;
      break;
    case SURPRISED:
      targetHeight = 40.0f;
      break;
    case THINKING:
      targetHeight = 29.0f;
      targetGazeX = 4.0f;
      targetGazeY = -2.0f;
      break;
    case RECORDING:
      targetHeight = 18.0f;
      break;
    case TALKING:
      targetHeight = 30.0f + 4.0f * sinf(now / 150.0f);
      break;
    case NORMAL:
      break;
  }

  if (animatedEmotion != (int)state) {
    animatedEmotion = (int)state;
    gazeXTarget = targetGazeX;
    gazeYTarget = targetGazeY;
    nextGazeChange = now + (unsigned long)random(900, 2200);
  }

  if (state == NORMAL || state == HAPPY || state == SURPRISED) {
    if ((long)(now - nextGazeChange) >= 0) {
      gazeXTarget = (float)random(-5, 6);
      gazeYTarget = (float)random(-3, 4);
      nextGazeChange = now + (unsigned long)random(900, 2200);
    }
  } else {
    gazeXTarget = targetGazeX;
    gazeYTarget = targetGazeY;
  }

  eyeHeightCurrent += (targetHeight - eyeHeightCurrent) * 0.24f;
  gazeXCurrent += (gazeXTarget - gazeXCurrent) * 0.22f;
  gazeYCurrent += (gazeYTarget - gazeYCurrent) * 0.22f;

  if (!blinkActive && (long)(now - nextBlinkAt) >= 0) {
    blinkActive = true;
    blinkStartedAt = now;
    nextBlinkAt = now + (unsigned long)random(2400, 5200);
  }

  float blinkScale = 1.0f;
  if (blinkActive) {
    unsigned long blinkElapsed = now - blinkStartedAt;
    if (blinkElapsed < 70) {
      blinkScale = 1.0f - (float)blinkElapsed / 70.0f;
    } else if (blinkElapsed < 100) {
      blinkScale = 0.08f;
    } else if (blinkElapsed < 190) {
      blinkScale = 0.08f + 0.92f * (float)(blinkElapsed - 100) / 90.0f;
    } else {
      blinkActive = false;
    }
  }

  display.clearDisplay();
  int eyeWidth = 30;
  int eyeHeight = (int)(eyeHeightCurrent * blinkScale);
  if (eyeHeight < 3) eyeHeight = 3;
  int eyeY = (SCREEN_HEIGHT - eyeHeight) / 2;
  int leftX = 22, rightX = 76;
  int pupilRadius = eyeHeight < 12 ? 2 : 5;
  int pupilOffsetX = (int)gazeXCurrent;
  int pupilOffsetY = (int)gazeYCurrent;
  int pupilY = eyeY + eyeHeight / 2 + pupilOffsetY;
  if (state == HAPPY) pupilY = eyeY + eyeHeight / 4 + pupilOffsetY;

  switch (state) {
    case NORMAL:
    case HAPPY:
      display.fillRoundRect(leftX, eyeY, eyeWidth, eyeHeight, 8, SSD1306_WHITE);
      display.fillRoundRect(rightX, eyeY, eyeWidth, eyeHeight, 8, SSD1306_WHITE);
      if (state == HAPPY) {
        display.fillRect(leftX, eyeY + eyeHeight / 2, eyeWidth, eyeHeight / 2, SSD1306_BLACK);
        display.fillRect(rightX, eyeY + eyeHeight / 2, eyeWidth, eyeHeight / 2, SSD1306_BLACK);
      }
      if (eyeHeight > 10) {
        display.fillCircle(leftX + 15 + pupilOffsetX, pupilY,
                           pupilRadius, SSD1306_BLACK);
        display.fillCircle(rightX + 15 + pupilOffsetX, pupilY,
                           pupilRadius, SSD1306_BLACK);
      }
      break;

    case SURPRISED:
      display.fillRoundRect(leftX, eyeY, eyeWidth, eyeHeight, 10, SSD1306_WHITE);
      display.fillRoundRect(rightX, eyeY, eyeWidth, eyeHeight, 10, SSD1306_WHITE);
      display.fillCircle(leftX + 15 + pupilOffsetX, eyeY + eyeHeight / 2 + pupilOffsetY,
                         pupilRadius, SSD1306_BLACK);
      display.fillCircle(rightX + 15 + pupilOffsetX, eyeY + eyeHeight / 2 + pupilOffsetY,
                         pupilRadius, SSD1306_BLACK);
      break;

    case RECORDING:
      display.fillRoundRect(leftX, eyeY, eyeWidth, eyeHeight, 4, SSD1306_WHITE);
      display.fillRoundRect(rightX, eyeY, eyeWidth, eyeHeight, 4, SSD1306_WHITE);
      display.fillCircle(64, 52, 6, SSD1306_WHITE);
      break;

    case THINKING:
      display.fillRoundRect(leftX, eyeY, eyeWidth, eyeHeight, 6, SSD1306_WHITE);
      display.fillRoundRect(rightX, eyeY, eyeWidth, eyeHeight, 6, SSD1306_WHITE);
      display.fillCircle(leftX + 15 + pupilOffsetX, eyeY + eyeHeight / 2 + pupilOffsetY,
                         pupilRadius, SSD1306_BLACK);
      display.fillCircle(rightX + 15 + pupilOffsetX, eyeY + eyeHeight / 2 + pupilOffsetY,
                         pupilRadius, SSD1306_BLACK);
      break;

    case TALKING:
      display.fillRoundRect(leftX, eyeY, eyeWidth, eyeHeight, 7, SSD1306_WHITE);
      display.fillRoundRect(rightX, eyeY, eyeWidth, eyeHeight, 7, SSD1306_WHITE);
      if (eyeHeight > 10) {
        display.fillCircle(leftX + 15 + pupilOffsetX, eyeY + eyeHeight / 2 + pupilOffsetY,
                           pupilRadius, SSD1306_BLACK);
        display.fillCircle(rightX + 15 + pupilOffsetX, eyeY + eyeHeight / 2 + pupilOffsetY,
                           pupilRadius, SSD1306_BLACK);
      }
      break;
  }
  display.display();
}

// ================= 4. MIKROFON I2S =================
void initI2S_Mic_New() {
  if (rx_handle != NULL) {
    i2s_channel_disable(rx_handle);
    i2s_del_channel(rx_handle);
    rx_handle = NULL;
  }

  i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
  i2s_new_channel(&chan_cfg, NULL, &rx_handle);

  i2s_std_config_t std_cfg = {
    .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
    .slot_cfg = I2S_STD_MSB_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
    .gpio_cfg = {
      .mclk = I2S_GPIO_UNUSED,
      .bclk = (gpio_num_t)MIC_I2S_SCK,
      .ws = (gpio_num_t)MIC_I2S_WS,
      .dout = I2S_GPIO_UNUSED,
      .din = (gpio_num_t)MIC_I2S_SD,
      .invert_flags = {
        .mclk_inv = false,
        .bclk_inv = false,
        .ws_inv = false,
      },
    },
  };
  i2s_channel_init_std_mode(rx_handle, &std_cfg);
  i2s_channel_enable(rx_handle);
}

void recordAudioToWav() {
  initI2S_Mic_New();
  currentEmotion = RECORDING;
  drawEyes(currentEmotion);

  createWavHeader(wavBuffer, PCM_DATA_SIZE);

  size_t bytesRead = 0;
  size_t totalRead = 0;

  Serial.println("Mulai merekam suara...");
  while (totalRead < PCM_DATA_SIZE) {
    i2s_channel_read(rx_handle, wavBuffer + WAV_HEADER_SIZE + totalRead, 1024, &bytesRead, 1000);
    totalRead += bytesRead;
  }
  Serial.println("Rekaman selesai.");

  if (rx_handle != NULL) {
    i2s_channel_disable(rx_handle);
    i2s_del_channel(rx_handle);
    rx_handle = NULL;
  }
}

// ================= 5. GROQ WHISPER STT (STREAMING UPLOAD NO-CRASH) =================
String sendToGroqWhisper() {
  if (WiFi.status() != WL_CONNECTED) return "";

  WiFiClientSecure client;
  client.setInsecure();

  if (!client.connect("api.groq.com", 443)) {
    Serial.println("Gagal koneksi ke Groq STT Server");
    return "";
  }

  String boundary = "----ESP32Boundary7MA4YWxkTrZu0gW";
  String head = "--" + boundary + "\r\n" +
                "Content-Disposition: form-data; name=\"file\"; filename=\"speech.wav\"\r\n" +
                "Content-Type: audio/wav\r\n\r\n";

  String tail = "\r\n--" + boundary + "\r\n" +
                "Content-Disposition: form-data; name=\"model\"\r\n\r\n" +
                "whisper-large-v3-turbo\r\n" +
                "--" + boundary + "\r\n" +
                "Content-Disposition: form-data; name=\"language\"\r\n\r\n" +
                "id\r\n" +
                "--" + boundary + "--\r\n";

  uint32_t totalContentLength = head.length() + TOTAL_AUDIO_SIZE + tail.length();

  // Kirim Header HTTP
  client.println("POST /openai/v1/audio/transcriptions HTTP/1.1");
  client.println("Host: api.groq.com");
  client.println("Authorization: Bearer " + String(GROQ_API_KEY));
  client.println("Content-Type: multipart/form-data; boundary=" + boundary);
  client.print("Content-Length: ");
  client.println(totalContentLength);
  client.println("Connection: close");
  client.println();

  // Kirim Body Multipart Secara Streaming (Hemat RAM)
  client.print(head);

  size_t bytesSent = 0;
  while (bytesSent < TOTAL_AUDIO_SIZE) {
    size_t chunkSize = (TOTAL_AUDIO_SIZE - bytesSent > 1024) ? 1024 : (TOTAL_AUDIO_SIZE - bytesSent);
    client.write(wavBuffer + bytesSent, chunkSize);
    bytesSent += chunkSize;
  }

  client.print(tail);

  // Baca Respon HTTP
  String responseBody = "";
  bool isBody = false;
  unsigned long timeout = millis();

  while (client.connected() || client.available()) {
    if (millis() - timeout > 10000) break; // Timeout 10 detik
    
    if (client.available()) {
      String line = client.readStringUntil('\n');
      if (line == "\r") {
        isBody = true;
        continue;
      }
      if (isBody) {
        responseBody += line;
      }
    }
  }
  client.stop();

  String transcribedText = "";
  if (responseBody.length() > 0) {
    JsonDocument doc;
    deserializeJson(doc, responseBody);
    if (doc.containsKey("text")) {
      transcribedText = doc["text"].as<String>();
    }
  }

  return transcribedText;
}

// ================= 6. GROQ LLM CHAT =================
String askGroqLLM(String promptText) {
  if (WiFi.status() != WL_CONNECTED || promptText.length() == 0) return "Maaf, suara tidak terdengar.";

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  http.begin(client, GROQ_CHAT_URL);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Authorization", String("Bearer ") + GROQ_API_KEY);

  JsonDocument doc;
  doc["model"] = "llama-3.3-70b-versatile";
  
  JsonArray messages = doc["messages"].to<JsonArray>();
  JsonObject sysMsg = messages.add<JsonObject>();
  sysMsg["role"] = "system";
  sysMsg["content"] = "Kamu adalah AI Companion mini yang ramah dan lucu. Jawab singkat maksimal 15 kata dalam Bahasa Indonesia tanpa karakter khusus.";

  JsonObject userMsg = messages.add<JsonObject>();
  userMsg["role"] = "user";
  userMsg["content"] = promptText;

  String body;
  serializeJson(doc, body);

  int httpCode = http.POST(body);
  String replyText = "Maaf, terjadi masalah koneksi.";

  if (httpCode == 200) {
    String res = http.getString();
    JsonDocument resDoc;
    deserializeJson(resDoc, res);
    replyText = resDoc["choices"][0]["message"]["content"].as<String>();
  }

  http.end();
  return replyText;
}

// ================= 7. GOOGLE TTS & SPEAKER PLAYBACK =================
void playGoogleTTS(String text) {
  text.replace(" ", "%20");
  String ttsUrl = "http://translate.google.com/translate_tts?ie=UTF-8&q=" + text + "&tl=id&client=tw-ob";

  audio.setPinout(SPK_I2S_BCLK, SPK_I2S_LRC, SPK_I2S_DIN);
  audio.setVolume(21);
  audio.connecttohost(ttsUrl.c_str());

  currentEmotion = TALKING;
  while (audio.isRunning()) {
    audio.loop();
    drawEyes(TALKING);
  }
  
  currentEmotion = NORMAL;
}

// ================= 8. SETUP UTAMA =================
void setup() {
  Serial.begin(115200);
  delay(1000); // Beri jeda stabilisasi daya saat boot

  pinMode(PIN_TOUCH, INPUT);

  // Inisialisasi Bus I2C khusus di GPIO 0 (SDA) dan GPIO 10 (SCL)
  Wire.begin(PIN_SDA, PIN_SCL);

  // 1. Inisialisasi OLED
  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDRESS)) {
    Serial.println("OLED Warning: Tidak terdeteksi/alamat berbeda.");
  } else {
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(0, 10);
    display.println("Starting AI Companion...");
    display.display();
  }

  // 2. Jalankan Wi-Fi Setup dengan WiFiManager
  setupWiFi();

  // 3. Sensor MPU6050
  if (!mpu.begin(0x68, &Wire)) {
    Serial.println("MPU6050 Warning: Tidak terdeteksi.");
  }

  // 4. Alokasi Buffer Rekaman Suara
  wavBuffer = (uint8_t*) malloc(TOTAL_AUDIO_SIZE);
  if (wavBuffer == NULL) {
    Serial.println("Gagal alokasi RAM!");
  }

  currentEmotion = NORMAL;
  randomSeed(esp_random());
  nextBlinkAt = millis() + (unsigned long)random(1200, 2800);
  Serial.println("System Ready!");
}

// ================= 9. LOOP UTAMA =================
void loop() {
  bool isTouched = digitalRead(PIN_TOUCH);
  
  sensors_event_t a, g, temp;
  mpu.getEvent(&a, &g, &temp);
  float totalAccel = sqrt(a.acceleration.x * a.acceleration.x +
                          a.acceleration.y * a.acceleration.y +
                          a.acceleration.z * a.acceleration.z);

  if (isTouched) {
    recordAudioToWav();

    currentEmotion = THINKING;
    drawEyes(currentEmotion);
    
    String userQuery = sendToGroqWhisper();
    Serial.println("User Said (Whisper STT): " + userQuery);

    String aiReply = askGroqLLM(userQuery);
    Serial.println("AI Reply (Groq LLM): " + aiReply);

    playGoogleTTS(aiReply);
  }

  if (totalAccel > 18.0) {
    currentEmotion = SURPRISED;
    lastStateChange = millis();
  }

  if (currentEmotion != NORMAL && currentEmotion != TALKING && millis() - lastStateChange > 3000) {
    currentEmotion = NORMAL;
  }

  drawEyes(currentEmotion);
  delay(50);
}