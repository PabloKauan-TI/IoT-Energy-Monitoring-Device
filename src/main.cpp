#include <PZEM004Tv30.h>
#include <HardwareSerial.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <time.h>


// NVS (memória persistente)
Preferences prefs;

// VARIÁVEIS DE CONFIGURAÇÃO
String wifiSSID = "Engine-IOT";
String wifiPASS = "IOT@engine";
int terminal_id = 1;
int aparelho_id = 2;
String user = "admin";
String pass = "P4bl01234";
String mac = "10:20:30:40:50:60";

// MQTT
const char* mqtt_server = "200.129.62.134";

WiFiClient espClient;
PubSubClient client(espClient);

// PZEM CONFIG
#define PZEM_RX_PIN 16
#define PZEM_TX_PIN 17
#define PZEM_SERIAL Serial2
PZEM004Tv30 pzem(PZEM_SERIAL, PZEM_RX_PIN, PZEM_TX_PIN); // Endereço 0xF8 (padrão)

// TIMER PARA ENVIO
unsigned long ultimoEnvio = 0;
unsigned long intervalo = 10UL * 60UL * 1000UL; // 2 min em ms

// PEGAR DATA E HORA BRASIL
String getDateTime() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) {
    return "01/01/1970 00:00:00";
  }
  char buffer[25];
  strftime(buffer, sizeof(buffer), "%d/%m/%Y %H:%M:%S", &timeinfo);
  return String(buffer);
}

// WIFI + NTP BRASIL
void connectWiFi() {
  Serial.print("Conectando ao WiFi: ");
  Serial.println(wifiSSID);

  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(wifiSSID.c_str(), wifiPASS.c_str());

  unsigned long ultimoRetry = millis();
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");

    if (millis() - ultimoRetry >= 10000) {
      ultimoRetry = millis();
      Serial.println();
      Serial.println("Ainda sem WiFi. Tentando novamente...");
      WiFi.disconnect();
      WiFi.begin(wifiSSID.c_str(), wifiPASS.c_str());
    }
  }

  Serial.println();
  Serial.println("WiFi conectado!");
  Serial.print("IP: ");
  Serial.println(WiFi.localIP());

  // NTP Brasil
  Serial.println("Sincronizando data/hora via NTP...");
  configTime(-3 * 3600, 0, "pool.ntp.org", "time.google.com");

  delay(1000);

  Serial.print("Horário atual: ");
  Serial.println(getDateTime());
}

// MQTT CALLBACK (CONFIGURAÇÃO)
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  Serial.println("==== RECEBEU JSON DE CONFIG ====");

  String jsonStr;
  for (int i = 0; i < length; i++) jsonStr += (char)payload[i];
  Serial.println(jsonStr);

  JsonDocument doc;
  if (deserializeJson(doc, jsonStr)) {
    Serial.println("Erro ao parsear JSON!");
    return;
  }

  if (doc.containsKey("ssid"))  wifiSSID = doc["ssid"].as<String>();
  if (doc.containsKey("senha")) wifiPASS = doc["senha"].as<String>();
  if (doc.containsKey("mac"))   mac = doc["mac"].as<String>();
  if (doc.containsKey("terminal_id")) terminal_id = doc["terminal_id"].as<int>();
  if (doc.containsKey("aparelho_id")) aparelho_id = doc["aparelho_id"].as<int>();

  Serial.println("Nova configuração aplicada!");

  // SALVAR NA NVS
  prefs.begin("config", false);
  prefs.putString("ssid", wifiSSID);
  prefs.putString("senha", wifiPASS);
  prefs.putString("mac", mac);
  prefs.putInt("terminal_id", terminal_id);
  prefs.putInt("aparelho_id", aparelho_id);
  prefs.end();

  Serial.println("Configurações salvas na NVS!");

  connectWiFi();
}

// MQTT CONNECT
void reconnectMQTT() {
  if (WiFi.status() != WL_CONNECTED || client.connected()) {
    return;
  }

  Serial.println("Conectando ao MQTT...");
  if (client.connect(mac.c_str(), user.c_str(), pass.c_str())) {
    Serial.println("MQTT conectado!");
    client.subscribe(mac.c_str());
    Serial.print("Inscrito no tópico: ");
    Serial.println(mac);
  } else {
    Serial.print("Falha MQTT, rc=");
    Serial.println(client.state());
  }
}

// SETUP
void setup() {
  Serial.begin(115200);
  delay(1500);

  // CARREGAR CONFIG
  prefs.begin("config", true);
  wifiSSID = prefs.getString("ssid", wifiSSID);
  wifiPASS = prefs.getString("senha", wifiPASS);
  aparelho_id = prefs.getInt("aparelho_id", aparelho_id);
  mac      = prefs.getString("mac", mac);
  terminal_id = prefs.getInt("terminal_id", terminal_id);
  prefs.end();

  Serial.println("Configurações carregadas:");
  Serial.println("SSID: " + wifiSSID);
  Serial.println("Senha: " + wifiPASS);
  Serial.println("Aparelho ID: " + String(aparelho_id));
  Serial.println("MAC sub: " + mac);
  Serial.println("Terminal ID: " + String(terminal_id));

  connectWiFi();

  client.setServer(mqtt_server, 1883);
  client.setCallback(mqttCallback);

  Serial.println("Sistema iniciado.");
}

// LOOP
void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi desconectado. Reconectando antes de continuar...");
    connectWiFi();
  }

  if (!client.connected()) 
    reconnectMQTT();

  if (client.connected()) {
    client.loop();  // mantém recebimento MQTT SEM PARAR
  }

  unsigned long agora = millis();

  // ENVIO A CADA 10 MINUTOS
  if (ultimoEnvio == 0 || agora - ultimoEnvio >= intervalo) {
    ultimoEnvio = agora;

    float voltage = pzem.voltage();
    float current = pzem.current();
    float power   = pzem.power();
    float energy  = pzem.energy();
    float freq    = pzem.frequency();
    float pf      = pzem.pf();

    if (isnan(voltage) || isnan(current) || isnan(power) || isnan(energy) || isnan(freq) || isnan(pf)) {
      Serial.println("Falha ao ler o PZEM. Confira TX/RX cruzados, GND comum e alimentacao 5V.");
    }

    JsonDocument doc;

    doc["mac"] = pzem.readAddress();
    doc["terminal_id"] = terminal_id;
    doc["aparelho_id"] = aparelho_id;
    doc["data"] = getDateTime();
    doc["tensao"] = voltage;
    doc["corrente"] = current;
    doc["potencia"] = power;
    doc["consumo"] = energy;
    doc["frequencia"] = freq;
    doc["fator_potencia"] = pf;

    String json;
    serializeJson(doc, json);

    Serial.println("==== ENVIANDO DADOS (10 MIN) ====");
    Serial.println(json);
    String topico = "energy." + mac;
    client.publish(topico.c_str(), json.c_str());
    Serial.println("-----------------------------------");
  }

  delay(1000); // evita 100% CPU
}
