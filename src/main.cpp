// ============================================================================
//  ARGOS IoT — Estação Argos de Encosta (ESP32)
//  Monitoramento de risco de deslizamento na borda (edge).
//
//  Sensores : DHT22 (umidade/temp) + MPU6050 (inclinação do terreno)
//  Saídas   : 4 LEDs (verde/amarelo/laranja/vermelho) + OLED SSD1306
//  Rede     : Wi-Fi (Wokwi-GUEST) + MQTT sobre TLS (HiveMQ Cloud)
//
//  Ver ARGOS_IOT_SPEC.md para a especificação completa.
// ============================================================================

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <Wire.h>
#include "DHTesp.h"
#include <Adafruit_MPU6050.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <ArduinoJson.h>
#include <time.h>
#include <math.h>

// ── Identidade da estação ─────────────────────────────────────────
// Troque apenas esta constante para implantar outra estação da frota.
const char* ESTACAO_ID = "sp-mboi-mirim-01";

// ── WiFi ──────────────────────────────────────────────────────────
const char* WIFI_SSID     = "Wokwi-GUEST";
const char* WIFI_PASSWORD = "";

// ── MQTT HiveMQ Cloud (TLS) ───────────────────────────────────────
const char* MQTT_BROKER   = "c175cd285334458eab19178117bdbc91.s1.eu.hivemq.cloud";
const int   MQTT_PORT     = 8883;
const char* MQTT_USER     = "FelipeRM564723";
const char* MQTT_PASSWORD = "Fiap@20072006";

// ── Pinos (ESP32 DevKit v1) — ver §4 do SPEC ──────────────────────
#define DHT_PIN     15
#define I2C_SDA     21
#define I2C_SCL     22
#define LED_VERDE    2
#define LED_AMARELO  4
#define LED_LARANJA  5
#define LED_VERMELHO 18

// ── OLED SSD1306 ──────────────────────────────────────────────────
#define OLED_W      128
#define OLED_H       64
#define OLED_ADDR  0x3C

// ── Intervalos de publicação (§6 / §7) ────────────────────────────
#define INTERVALO_LEITURAS_MS  5000UL
#define INTERVALO_STATUS_MS    30000UL

// ── Limiares do cálculo de risco (§5) ─────────────────────────────
#define HUM_MIN          40.0f   // 40% umidade → humNorm 0
#define HUM_MAX         100.0f   // 100% umidade → humNorm 1
#define TILT_MAX         30.0f   // 30° = encosta em movimento → tiltNorm 1
#define VARIACAO_GATILHO  5.0f   // variação brusca de inclinação força CRÍTICO
#define PESO_UMIDADE      0.4f
#define PESO_INCLINACAO   0.6f

// ── NTP (timestamp ISO-8601) ──────────────────────────────────────
const char* NTP_SERVER = "pool.ntp.org";

// ── Objetos globais ───────────────────────────────────────────────
DHTesp dht;
Adafruit_MPU6050 mpu;
Adafruit_SSD1306 oled(OLED_W, OLED_H, &Wire, -1);
WiFiClientSecure wifiClient;
PubSubClient mqtt(wifiClient);

// ── Estado ────────────────────────────────────────────────────────
unsigned long ultimaLeitura = 0;
unsigned long ultimoStatus  = 0;
unsigned long bootMs        = 0;

bool  mpuOk      = false;
bool  oledOk     = false;
float anguloAnterior = 0.0f;
bool  temAnguloAnterior = false;
String nivelAnterior = "";

// Última leitura válida (para o OLED e publicações)
float ultUmidade   = NAN;
float ultTemp      = NAN;
float ultAngulo    = 0.0f;
float ultScore     = 0.0f;
String ultNivel    = "baixo";
String ultCor      = "#22C55E";

// ────────────────────────────────────────────────────────────────
// Utilitários
// ────────────────────────────────────────────────────────────────
static float clamp01(float v) {
  if (v < 0.0f) return 0.0f;
  if (v > 1.0f) return 1.0f;
  return v;
}

// Monta tópico "argos/estacao/{id}/{sufixo}"
String topico(const char* sufixo) {
  return String("argos/estacao/") + ESTACAO_ID + "/" + sufixo;
}

// Timestamp ISO-8601 UTC (ex.: 2026-06-04T14:05:00Z).
// Cai para string vazia se o NTP ainda não sincronizou.
String timestampISO() {
  time_t agora = time(nullptr);
  if (agora < 100000) return "";        // relógio ainda não sincronizado
  struct tm tmInfo;
  gmtime_r(&agora, &tmInfo);
  char buf[25];
  strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tmInfo);
  return String(buf);
}

// ────────────────────────────────────────────────────────────────
// Conectividade
// ────────────────────────────────────────────────────────────────
void conectarWiFi() {
  if (WiFi.status() == WL_CONNECTED) return;
  Serial.print("[WiFi] Conectando");
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\n[WiFi] OK — IP: " + WiFi.localIP().toString());

  // Sincroniza relógio para timestamps ISO-8601 (UTC).
  configTime(0, 0, NTP_SERVER);
}

void conectarMQTT() {
  wifiClient.setInsecure();                 // Wokwi não valida cadeia TLS
  mqtt.setServer(MQTT_BROKER, MQTT_PORT);
  mqtt.setKeepAlive(60);
  mqtt.setBufferSize(512);

  while (!mqtt.connected()) {
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("[MQTT] WiFi caiu — abortando tentativa.");
      return;
    }
    Serial.print("[MQTT] Conectando...");
    String clientId = String("argos-") + ESTACAO_ID;
    if (mqtt.connect(clientId.c_str(), MQTT_USER, MQTT_PASSWORD)) {
      Serial.println("OK");
    } else {
      Serial.printf("falhou (rc=%d), nova tentativa em 3s...\n", mqtt.state());
      wifiClient.stop();
      delay(3000);
    }
  }
}

// ────────────────────────────────────────────────────────────────
// Leitura de sensores
// ────────────────────────────────────────────────────────────────
// Ângulo de inclinação (graus) entre o eixo do dispositivo e a gravidade.
// 0° = plano (encosta estável) → cresce conforme o terreno se inclina.
float lerInclinacao() {
  if (!mpuOk) return 0.0f;
  sensors_event_t a, g, temp;
  mpu.getEvent(&a, &g, &temp);
  float ax = a.acceleration.x;
  float ay = a.acceleration.y;
  float az = a.acceleration.z;
  float ang = atan2f(sqrtf(ax * ax + ay * ay), fabsf(az)) * 180.0f / PI;
  return ang;
}

// ────────────────────────────────────────────────────────────────
// Lógica de risco na borda (§5)
// ────────────────────────────────────────────────────────────────
struct Risco {
  float  score;
  String nivel;
  String cor;
};

Risco calcularRisco(float umidade, float angulo, float variacaoAngulo) {
  float humNorm  = clamp01((umidade - HUM_MIN) / (HUM_MAX - HUM_MIN));
  float tiltNorm = clamp01(angulo / TILT_MAX);
  float score    = humNorm * PESO_UMIDADE + tiltNorm * PESO_INCLINACAO;

  // Gatilho mecânico: variação brusca de inclinação força CRÍTICO.
  if (variacaoAngulo > VARIACAO_GATILHO) score = 1.0f;

  Risco r;
  r.score = score;
  if (score < 0.25f)      { r.nivel = "baixo";   r.cor = "#22C55E"; }
  else if (score < 0.50f) { r.nivel = "medio";   r.cor = "#EAB308"; }
  else if (score < 0.75f) { r.nivel = "alto";    r.cor = "#F97316"; }
  else                    { r.nivel = "critico"; r.cor = "#EF4444"; }
  return r;
}

// ────────────────────────────────────────────────────────────────
// Atuadores
// ────────────────────────────────────────────────────────────────
void apagarLeds() {
  digitalWrite(LED_VERDE,    LOW);
  digitalWrite(LED_AMARELO,  LOW);
  digitalWrite(LED_LARANJA,  LOW);
  digitalWrite(LED_VERMELHO, LOW);
}

// Acende o LED do nível atual. No nível crítico, o vermelho pisca
// (alterna conforme millis); por isso é chamado a cada loop().
void atualizarLeds(const String& nivel) {
  apagarLeds();
  if (nivel == "baixo")   digitalWrite(LED_VERDE,   HIGH);
  else if (nivel == "medio")   digitalWrite(LED_AMARELO, HIGH);
  else if (nivel == "alto")    digitalWrite(LED_LARANJA, HIGH);
  else if (nivel == "critico") {
    bool aceso = (millis() / 400) % 2;     // pisca ~1,25 Hz
    digitalWrite(LED_VERMELHO, aceso ? HIGH : LOW);
  }
}

void atualizarOLED() {
  if (!oledOk) return;
  oled.clearDisplay();
  oled.setTextColor(SSD1306_WHITE);

  // Cabeçalho
  oled.setTextSize(1);
  oled.setCursor(0, 0);
  oled.print("ARGOS ");
  oled.print(ESTACAO_ID);

  oled.drawFastHLine(0, 10, OLED_W, SSD1306_WHITE);

  // Nível de risco em destaque
  oled.setTextSize(2);
  oled.setCursor(0, 14);
  String nv = ultNivel;
  nv.toUpperCase();
  oled.print(nv);

  // Leituras
  oled.setTextSize(1);
  oled.setCursor(0, 36);
  if (isnan(ultUmidade)) oled.print("Umid: --   ");
  else { oled.print("Umid: "); oled.print(ultUmidade, 0); oled.print("%  "); }
  oled.print("T:");
  if (isnan(ultTemp)) oled.print("--");
  else oled.print(ultTemp, 0);
  oled.print((char)247);                  // símbolo de grau
  oled.print("C");

  oled.setCursor(0, 46);
  oled.print("Inclin: ");
  oled.print(ultAngulo, 1);
  oled.print((char)247);

  oled.setCursor(0, 56);
  oled.print("WiFi: ");
  if (WiFi.status() == WL_CONNECTED) {
    oled.print("OK ");
    oled.print(WiFi.RSSI());
    oled.print("dBm");
  } else {
    oled.print("OFFLINE");
  }

  oled.display();
}

// ────────────────────────────────────────────────────────────────
// Publicações MQTT (§6)
// ────────────────────────────────────────────────────────────────
void publicarLeituras(float umidade, float inclinacao) {
  JsonDocument doc;
  doc["id"]         = ESTACAO_ID;
  doc["umidade"]    = round(umidade * 10) / 10.0;
  doc["inclinacao"] = round(inclinacao * 10) / 10.0;
  doc["ts"]         = timestampISO();

  char payload[256];
  size_t n = serializeJson(doc, payload);
  mqtt.publish(topico("leituras").c_str(), (const uint8_t*)payload, n, false);
  Serial.println("[MQTT] leituras → " + String(payload));
}

void publicarRisco(const Risco& r) {
  JsonDocument doc;
  doc["id"]    = ESTACAO_ID;
  doc["nivel"] = r.nivel;
  doc["score"] = round(r.score * 100) / 100.0;
  doc["ts"]    = timestampISO();

  char payload[256];
  size_t n = serializeJson(doc, payload);
  mqtt.publish(topico("risco").c_str(), (const uint8_t*)payload, n, true);  // retido
  Serial.println("[MQTT] risco → " + String(payload));
}

void publicarStatus() {
  JsonDocument doc;
  doc["id"]        = ESTACAO_ID;
  doc["online"]    = true;
  doc["wifi_rssi"] = WiFi.RSSI();
  doc["uptime_s"]  = (millis() - bootMs) / 1000;
  doc["ts"]        = timestampISO();

  char payload[256];
  size_t n = serializeJson(doc, payload);
  mqtt.publish(topico("status").c_str(), (const uint8_t*)payload, n, false);
  Serial.println("[MQTT] status → " + String(payload));
}

// ────────────────────────────────────────────────────────────────
// Ciclo principal de medição
// ────────────────────────────────────────────────────────────────
void cicloLeitura() {
  TempAndHumidity d = dht.getTempAndHumidity();
  float umidade     = d.humidity;
  float temperatura = d.temperature;
  float angulo      = lerInclinacao();

  if (isnan(umidade) || isnan(temperatura)) {
    Serial.printf("[DHT22] erro de leitura (%s) — usando ultima valida.\n",
                  dht.getStatusString());
    umidade     = isnan(ultUmidade) ? 0.0f : ultUmidade;
    temperatura = isnan(ultTemp)    ? 0.0f : ultTemp;
  }

  float variacao = temAnguloAnterior ? fabsf(angulo - anguloAnterior) : 0.0f;
  anguloAnterior = angulo;
  temAnguloAnterior = true;

  Risco r = calcularRisco(umidade, angulo, variacao);

  // Guarda para OLED/atuadores
  ultUmidade = umidade;
  ultTemp    = temperatura;
  ultAngulo  = angulo;
  ultScore   = r.score;
  ultNivel   = r.nivel;
  ultCor     = r.cor;

  Serial.printf("[LEITURA] umid=%.1f%% temp=%.1fC incl=%.1f° var=%.1f° score=%.2f nivel=%s\n",
                umidade, temperatura, angulo, variacao, r.score, r.nivel.c_str());

  // Publica leituras (sempre) e risco (só quando o nível muda)
  if (mqtt.connected()) {
    publicarLeituras(umidade, angulo);
    if (r.nivel != nivelAnterior) {
      publicarRisco(r);
      nivelAnterior = r.nivel;
    }
  }
}

// ────────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  delay(500);
  bootMs = millis();

  Serial.println("\n\n=== ARGOS IoT — Estacao de Encosta ===");
  Serial.print("Estacao: "); Serial.println(ESTACAO_ID);

  // LEDs
  pinMode(LED_VERDE,    OUTPUT);
  pinMode(LED_AMARELO,  OUTPUT);
  pinMode(LED_LARANJA,  OUTPUT);
  pinMode(LED_VERMELHO, OUTPUT);
  apagarLeds();

  // Barramento I²C compartilhado (MPU6050 + OLED)
  Wire.begin(I2C_SDA, I2C_SCL);

  // DHT22
  dht.setup(DHT_PIN, DHTesp::DHT22);
  Serial.println("[DHT22] inicializado (GPIO15).");

  // MPU6050 (0x68)
  if (mpu.begin(0x68, &Wire)) {
    mpu.setAccelerometerRange(MPU6050_RANGE_8_G);
    mpu.setGyroRange(MPU6050_RANGE_500_DEG);
    mpu.setFilterBandwidth(MPU6050_BAND_21_HZ);
    mpuOk = true;
    Serial.println("[MPU6050] OK (0x68).");
  } else {
    Serial.println("[MPU6050] NAO encontrado — inclinacao = 0.");
  }

  // OLED (0x3C)
  if (oled.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
    oledOk = true;
    oled.clearDisplay();
    oled.setTextSize(1);
    oled.setTextColor(SSD1306_WHITE);
    oled.setCursor(0, 0);
    oled.println("ARGOS IoT");
    oled.println("Inicializando...");
    oled.display();
    Serial.println("[OLED] OK (0x3C).");
  } else {
    Serial.println("[OLED] NAO encontrado.");
  }

  conectarWiFi();
  conectarMQTT();

  Serial.println("Setup completo.\n");
}

void loop() {
  // Mantém conectividade
  if (WiFi.status() != WL_CONNECTED) conectarWiFi();
  if (!mqtt.connected())            conectarMQTT();
  mqtt.loop();

  unsigned long agora = millis();

  // Ciclo de leitura a cada 5s
  if (agora - ultimaLeitura >= INTERVALO_LEITURAS_MS) {
    ultimaLeitura = agora;
    cicloLeitura();
    atualizarOLED();
  }

  // Heartbeat a cada 30s
  if (agora - ultimoStatus >= INTERVALO_STATUS_MS) {
    ultimoStatus = agora;
    if (mqtt.connected()) publicarStatus();
  }

  // LEDs atualizados todo loop (crítico precisa piscar)
  atualizarLeds(ultNivel);
}
