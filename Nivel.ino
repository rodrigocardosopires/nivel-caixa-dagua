#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <Preferences.h>

#if __has_include("secrets.h")
#include "secrets.h"
#else
#warning "secrets.h nao encontrado: usando secrets.example.h (copie e preencha suas credenciais)"
#include "secrets.example.h"
#endif

// Descomente para ver as leituras no Monitor Serial (115200)
#define DEBUG

// Configurações do Display OLED
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET    -1
const uint32_t I2C_CLOCK = 400000UL;
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET, I2C_CLOCK, I2C_CLOCK);

// Definição dos pinos do Sensor HC-SR04
// Sensor alimentado em 3,3 V: o ECHO sai em 3,3 V e pode ir direto ao GPIO.
// Se alimentar o sensor em 5 V, use um divisor no ECHO (ex.: 1k em série + 2k para o GND).
// GPIO12 é pino de strapping do ESP32; se o boot falhar, mova o TRIG para 25, 26 ou 27.
const int PIN_TRIG = 12;
const int PIN_ECHO = 14;

// Calibração padrão (cm). Pode ser alterada pelo Home Assistant e fica salva na flash.
const int DISTANCIA_VAZIA_PADRAO = 100; // Sensor até o fundo (Caixa vazia = 0cm de água)
const int DISTANCIA_CHEIA_PADRAO = 10;  // Sensor até a água (Caixa cheia = 90cm de água)

// Limites aceitos na calibração (faixa útil do HC-SR04)
const int DISTANCIA_MIN_CM = 2;
const int DISTANCIA_MAX_CM = 400;
const int DIFERENCA_MIN_CM = 10;

const long TOLERANCIA_MM = 20; // Aceita até 2cm além dos limites calibrados

int distanciaVaziaCm = DISTANCIA_VAZIA_PADRAO;
int distanciaCheiaCm = DISTANCIA_CHEIA_PADRAO;
long distanciaVaziaMm;
long distanciaCheiaMm;
long alturaMaxMm;
unsigned long timeoutEcoUs; // fundo + 20cm de folga (~58us por cm, ida e volta)

Preferences preferencias;

// Temporização (ms). Abaixo de ~60ms o eco anterior pode contaminar a leitura.
const unsigned long INTERVALO_MEDIDA_MS = 70;
const unsigned long INTERVALO_PISCA_MS = 500;

// Leituras inválidas seguidas até exibir erro (~20 x 70ms = 1,4s)
const int MAX_FALHAS_SEGUIDAS = 20;

// Alerta com histerese para não ficar ligando/desligando perto do limite
const int ALERTA_ENTRA_PCT = 15;
const int ALERTA_SAI_PCT = 18;

// Filtro: mediana das últimas leituras + média exponencial (EMA)
const int TAM_MEDIANA = 5;
const float ALFA_EMA = 0.2f; // 0..1: maior = responde mais rápido, menor = mais estável

long bufferMediana[TAM_MEDIANA];
int indiceMediana = 0;
int qtdMediana = 0;
float distanciaFiltradaMm = 0;
bool filtroIniciado = false;
int falhasSeguidas = 0;

// Estado de exibição
int nivelCm = 0;
int porcentagem = 0;
bool sensorComErro = false;
bool emAlerta = false;
bool exibirTextoAlerta = false;
bool precisaRedesenhar = false;

unsigned long ultimaMedida = 0;
unsigned long ultimoPisca = 0;

// Home Assistant via MQTT (descoberta automática)
const unsigned long INTERVALO_RECONEXAO_MQTT_MS = 10000;
const unsigned long INTERVALO_MIN_PUBLICACAO_MS = 2000;
const unsigned long INTERVALO_HEARTBEAT_MS = 60000;
const uint32_t TIMEOUT_CONEXAO_MQTT_MS = 1000; // conexão bloqueia o loop; manter curto

WiFiClient wifiClient;
PubSubClient mqtt(wifiClient);

char idDispositivo[20];   // ex.: nivel_caixa_a1b2c3
char topicoEstado[48];
char topicoStatus[48];
char topicoCalibracao[48];
char topicoCmdVazia[56];
char topicoCmdCheia[56];

bool calibracaoPendente = false;

bool publicacaoPendente = false;
bool mqttConectadoNaTela = false;
bool wifiConectadoNaTela = false;

// Ícones 16x16 da faixa superior (1 bit por pixel, 2 bytes por linha)
const int ICONE_TAM = 16;
const int ICONE_WIFI_X = 0;
const int ICONE_HA_X = 18;
const int TEXTO_TOPO_X = 36; // início da área de texto, à direita dos ícones

const uint8_t ICONE_WIFI[] PROGMEM = {
  0x00, 0x00, 0x07, 0xE0, 0x1F, 0xF8, 0x78, 0x1E,
  0xE0, 0x07, 0x87, 0xE1, 0x1F, 0xF8, 0x38, 0x1C,
  0x20, 0x04, 0x03, 0xC0, 0x07, 0xE0, 0x04, 0x20,
  0x00, 0x00, 0x01, 0x80, 0x03, 0xC0, 0x01, 0x80
};

const uint8_t ICONE_HOME_ASSISTANT[] PROGMEM = {
  0x01, 0x80, 0x03, 0xC0, 0x06, 0x60, 0x0C, 0x30,
  0x1B, 0xD8, 0x33, 0xCC, 0x6D, 0xB6, 0xCD, 0xB3,
  0x45, 0xA2, 0x43, 0xC2, 0x41, 0x82, 0x41, 0x82,
  0x41, 0x82, 0x41, 0x82, 0x7F, 0xFE, 0x00, 0x00
};
unsigned long ultimaTentativaMqtt = 0;
unsigned long ultimaPublicacao = 0;

long lerDistanciaMm() {
  digitalWrite(PIN_TRIG, LOW);
  delayMicroseconds(2);
  digitalWrite(PIN_TRIG, HIGH);
  delayMicroseconds(10);
  digitalWrite(PIN_TRIG, LOW);

  unsigned long duracao = pulseIn(PIN_ECHO, HIGH, timeoutEcoUs);
  if (duracao == 0) return -1;

  // Velocidade do som a 20°C: 0,343 mm/us, dividido por 2 (ida e volta)
  return (long)(duracao * 343UL / 2000UL);
}

bool leituraValida(long distanciaMm) {
  return distanciaMm >= distanciaCheiaMm - TOLERANCIA_MM &&
         distanciaMm <= distanciaVaziaMm + TOLERANCIA_MM;
}

void recalcularLimites() {
  distanciaVaziaMm = distanciaVaziaCm * 10L;
  distanciaCheiaMm = distanciaCheiaCm * 10L;
  alturaMaxMm = distanciaVaziaMm - distanciaCheiaMm;
  timeoutEcoUs = (distanciaVaziaCm + 20) * 58UL;
}

bool calibracaoValida(int vaziaCm, int cheiaCm) {
  return cheiaCm >= DISTANCIA_MIN_CM && vaziaCm <= DISTANCIA_MAX_CM &&
         vaziaCm - cheiaCm >= DIFERENCA_MIN_CM;
}

void carregarCalibracao() {
  preferencias.begin("nivel", false);
  int vazia = preferencias.getInt("vazia", DISTANCIA_VAZIA_PADRAO);
  int cheia = preferencias.getInt("cheia", DISTANCIA_CHEIA_PADRAO);
  if (calibracaoValida(vazia, cheia)) {
    distanciaVaziaCm = vazia;
    distanciaCheiaCm = cheia;
  }
  recalcularLimites();
}

long calcularMediana() {
  long ordenado[TAM_MEDIANA];
  for (int i = 0; i < qtdMediana; i++) {
    long valor = bufferMediana[i];
    int j = i - 1;
    while (j >= 0 && ordenado[j] > valor) {
      ordenado[j + 1] = ordenado[j];
      j--;
    }
    ordenado[j + 1] = valor;
  }
  return ordenado[qtdMediana / 2];
}

void processarMedida() {
  long distanciaMm = lerDistanciaMm();

  if (!leituraValida(distanciaMm)) {
    if (falhasSeguidas < MAX_FALHAS_SEGUIDAS) falhasSeguidas++;
  } else {
    falhasSeguidas = 0;

    bufferMediana[indiceMediana] = distanciaMm;
    indiceMediana = (indiceMediana + 1) % TAM_MEDIANA;
    if (qtdMediana < TAM_MEDIANA) qtdMediana++;

    long mediana = calcularMediana();
    if (!filtroIniciado) {
      distanciaFiltradaMm = mediana;
      filtroIniciado = true;
    } else {
      distanciaFiltradaMm += (mediana - distanciaFiltradaMm) * ALFA_EMA;
    }

#ifdef DEBUG
    Serial.print(F("bruto(mm): "));     Serial.print(distanciaMm);
    Serial.print(F("  mediana(mm): ")); Serial.print(mediana);
    Serial.print(F("  filtrado(mm): ")); Serial.println(distanciaFiltradaMm, 1);
#endif
  }

  bool erro = falhasSeguidas >= MAX_FALHAS_SEGUIDAS;
  if (erro != sensorComErro) {
    sensorComErro = erro;
    precisaRedesenhar = true;
    publicacaoPendente = true;
#ifdef DEBUG
    Serial.println(erro ? F("Sensor sem resposta") : F("Sensor normalizado"));
#endif
  }

  atualizarNivel();
}

void atualizarNivel() {
  if (!filtroIniciado) return;

  long nivelMm = constrain(distanciaVaziaMm - (long)(distanciaFiltradaMm + 0.5f), 0L, alturaMaxMm);
  int novoNivelCm = (nivelMm + 5) / 10;
  int novaPorcentagem = (nivelMm * 100 + alturaMaxMm / 2) / alturaMaxMm;

  if (novoNivelCm != nivelCm || novaPorcentagem != porcentagem) {
    nivelCm = novoNivelCm;
    porcentagem = novaPorcentagem;
    precisaRedesenhar = true;
    publicacaoPendente = true;
  }

  if (!emAlerta && porcentagem <= ALERTA_ENTRA_PCT) {
    emAlerta = true;
    exibirTextoAlerta = true;
    ultimoPisca = millis();
    precisaRedesenhar = true;
    publicacaoPendente = true;
  } else if (emAlerta && porcentagem >= ALERTA_SAI_PCT) {
    emAlerta = false;
    exibirTextoAlerta = false;
    precisaRedesenhar = true;
    publicacaoPendente = true;
  }
}

void aplicarCalibracao(int vaziaCm, int cheiaCm) {
  calibracaoPendente = true; // republica sempre: em caso de recusa, o HA volta ao valor atual
  if (!calibracaoValida(vaziaCm, cheiaCm)) {
#ifdef DEBUG
    Serial.print(F("Calibracao recusada: vazia=")); Serial.print(vaziaCm);
    Serial.print(F(" cheia=")); Serial.println(cheiaCm);
#endif
    return;
  }
  if (vaziaCm == distanciaVaziaCm && cheiaCm == distanciaCheiaCm) return;

  distanciaVaziaCm = vaziaCm;
  distanciaCheiaCm = cheiaCm;
  preferencias.putInt("vazia", vaziaCm);
  preferencias.putInt("cheia", cheiaCm);
  recalcularLimites();
  atualizarNivel();
}

void aoReceberMqtt(char* topico, byte* payload, unsigned int tamanho) {
  char texto[16];
  unsigned int n = min(tamanho, (unsigned int)sizeof(texto) - 1);
  memcpy(texto, payload, n);
  texto[n] = '\0';
  int valorCm = (int)lroundf(atof(texto));

  if (strcmp(topico, topicoCmdVazia) == 0) {
    aplicarCalibracao(valorCm, distanciaCheiaCm);
  } else if (strcmp(topico, topicoCmdCheia) == 0) {
    aplicarCalibracao(distanciaVaziaCm, valorCm);
  }
}

void publicarConfig(const char* componente, const char* chave, const char* topicoDeEstado, const char* campos) {
  char topico[96];
  char payload[640];
  snprintf(topico, sizeof(topico), "homeassistant/%s/%s/%s/config", componente, idDispositivo, chave);
  snprintf(payload, sizeof(payload),
           "{\"uniq_id\":\"%s_%s\",\"stat_t\":\"%s\",\"avty_t\":\"%s\",%s,"
           "\"dev\":{\"ids\":[\"%s\"],\"name\":\"Nível Caixa d'Água\",\"mf\":\"DIY\",\"mdl\":\"ESP32 DevKit V1 + HC-SR04\"}}",
           idDispositivo, chave, topicoDeEstado, topicoStatus, campos, idDispositivo);
  mqtt.publish(topico, payload, true);
}

void publicarConfigCalibracao(const char* chave, const char* nome, const char* icone, const char* topicoCmd) {
  char campos[320];
  snprintf(campos, sizeof(campos),
           "\"name\":\"%s\",\"cmd_t\":\"%s\",\"min\":%d,\"max\":%d,\"step\":1,\"mode\":\"box\","
           "\"unit_of_meas\":\"cm\",\"ent_cat\":\"config\",\"ic\":\"%s\",\"val_tpl\":\"{{ value_json.%s }}\"",
           nome, topicoCmd, DISTANCIA_MIN_CM, DISTANCIA_MAX_CM, icone, chave);
  publicarConfig("number", chave, topicoCalibracao, campos);
}

void publicarCalibracao() {
  char payload[48];
  snprintf(payload, sizeof(payload), "{\"vazia\":%d,\"cheia\":%d}", distanciaVaziaCm, distanciaCheiaCm);
  if (mqtt.publish(topicoCalibracao, payload, true)) {
    calibracaoPendente = false;
  }
}

void publicarDescoberta() {
  publicarConfigCalibracao("vazia", "Distância caixa vazia", "mdi:arrow-collapse-down", topicoCmdVazia);
  publicarConfigCalibracao("cheia", "Distância caixa cheia", "mdi:arrow-collapse-up", topicoCmdCheia);
  publicarConfig("sensor", "nivel", topicoEstado,
    "\"name\":\"Nível\",\"unit_of_meas\":\"cm\",\"dev_cla\":\"distance\",\"stat_cla\":\"measurement\","
    "\"val_tpl\":\"{{ value_json.nivel }}\"");
  publicarConfig("sensor", "porcentagem", topicoEstado,
    "\"name\":\"Porcentagem\",\"unit_of_meas\":\"%\",\"stat_cla\":\"measurement\",\"ic\":\"mdi:water-percent\","
    "\"val_tpl\":\"{{ value_json.porcentagem }}\"");
  publicarConfig("binary_sensor", "alerta", topicoEstado,
    "\"name\":\"Nível baixo\",\"dev_cla\":\"problem\",\"ic\":\"mdi:water-alert\","
    "\"val_tpl\":\"{{ 'ON' if value_json.alerta else 'OFF' }}\"");
  publicarConfig("binary_sensor", "erro", topicoEstado,
    "\"name\":\"Erro do sensor\",\"dev_cla\":\"problem\",\"ent_cat\":\"diagnostic\","
    "\"val_tpl\":\"{{ 'ON' if value_json.erro else 'OFF' }}\"");
}

void publicarEstado() {
  char payload[96];
  snprintf(payload, sizeof(payload), "{\"nivel\":%d,\"porcentagem\":%d,\"alerta\":%s,\"erro\":%s}",
           nivelCm, porcentagem, emAlerta ? "true" : "false", sensorComErro ? "true" : "false");
  if (mqtt.publish(topicoEstado, payload, true)) {
    publicacaoPendente = false;
    ultimaPublicacao = millis();
  }
}

void iniciarRede() {
  uint64_t mac = ESP.getEfuseMac();
  snprintf(idDispositivo, sizeof(idDispositivo), "nivel_caixa_%02x%02x%02x",
           (uint8_t)(mac >> 24), (uint8_t)(mac >> 32), (uint8_t)(mac >> 40));
  snprintf(topicoEstado, sizeof(topicoEstado), "%s/estado", idDispositivo);
  snprintf(topicoStatus, sizeof(topicoStatus), "%s/status", idDispositivo);
  snprintf(topicoCalibracao, sizeof(topicoCalibracao), "%s/calibracao", idDispositivo);
  snprintf(topicoCmdVazia, sizeof(topicoCmdVazia), "%s/calibracao/vazia/set", idDispositivo);
  snprintf(topicoCmdCheia, sizeof(topicoCmdCheia), "%s/calibracao/cheia/set", idDispositivo);

  WiFi.mode(WIFI_STA);
  WiFi.setHostname(idDispositivo);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_SENHA);

  wifiClient.setConnectionTimeout(TIMEOUT_CONEXAO_MQTT_MS);
  mqtt.setServer(MQTT_HOST, MQTT_PORTA);
  mqtt.setBufferSize(768);
  mqtt.setSocketTimeout(2);
  mqtt.setCallback(aoReceberMqtt);
}

void manterMqtt(unsigned long agora) {
  if (WiFi.status() != WL_CONNECTED) return;

  if (!mqtt.connected()) {
    if (ultimaTentativaMqtt != 0 && agora - ultimaTentativaMqtt < INTERVALO_RECONEXAO_MQTT_MS) return;
    ultimaTentativaMqtt = agora;

    if (!mqtt.connect(idDispositivo, MQTT_USUARIO, MQTT_SENHA, topicoStatus, 0, true, "offline")) {
#ifdef DEBUG
      Serial.print(F("MQTT falhou, rc=")); Serial.println(mqtt.state());
#endif
      return;
    }
    mqtt.publish(topicoStatus, "online", true);
    publicarDescoberta();
    mqtt.subscribe(topicoCmdVazia);
    mqtt.subscribe(topicoCmdCheia);
    calibracaoPendente = true;
    publicacaoPendente = true;
    ultimaPublicacao = agora - INTERVALO_MIN_PUBLICACAO_MS;
  }

  mqtt.loop();

  if (calibracaoPendente) publicarCalibracao();

  if (!filtroIniciado && !sensorComErro) return;
  bool podePublicar = agora - ultimaPublicacao >= INTERVALO_MIN_PUBLICACAO_MS;
  if ((publicacaoPendente && podePublicar) || agora - ultimaPublicacao >= INTERVALO_HEARTBEAT_MS) {
    publicarEstado();
  }
}

void desenharErro() {
  display.setTextSize(2);
  display.setCursor(40, 16); display.print(F("ERRO"));
  display.setCursor(28, 38); display.print(F("SENSOR"));
}

void desenharNivel() {
  // ZONA AMARELA (Y: 0 a 15): texto centralizado à direita dos ícones
  char texto[12];
  if (exibirTextoAlerta) {
    strcpy(texto, "ALERTA!");
  } else {
    snprintf(texto, sizeof(texto), "%d cm", nivelCm);
  }
  const int larguraCaractere = 12; // fonte padrão 6px x tamanho 2
  int larguraTexto = strlen(texto) * larguraCaractere;
  display.setTextSize(2);
  display.setCursor(TEXTO_TOPO_X + (SCREEN_WIDTH - TEXTO_TOPO_X - larguraTexto) / 2, 0);
  display.print(texto);

  // ZONA AZUL (Y: 16 a 63)
  display.drawRect(10, 20, 30, 40, SSD1306_WHITE);
  display.fillRect(15, 17, 20, 3, SSD1306_WHITE);

  int alturaAguaGrafica = (porcentagem * 36 + 50) / 100;
  if (alturaAguaGrafica > 0) {
    display.fillRect(12, 58 - alturaAguaGrafica, 26, alturaAguaGrafica, SSD1306_WHITE);
  }

  display.setCursor(60, 32);
  display.print(porcentagem);
  display.print(F("%"));
}

void desenharTela() {
  display.clearDisplay();
  if (sensorComErro) {
    desenharErro();
  } else {
    desenharNivel();
  }
  if (wifiConectadoNaTela) {
    display.drawBitmap(ICONE_WIFI_X, 0, ICONE_WIFI, ICONE_TAM, ICONE_TAM, SSD1306_WHITE);
  }
  if (mqttConectadoNaTela) {
    display.drawBitmap(ICONE_HA_X, 0, ICONE_HOME_ASSISTANT, ICONE_TAM, ICONE_TAM, SSD1306_WHITE);
  }
  display.display();
}

void setup() {
  Serial.begin(115200);

  pinMode(PIN_TRIG, OUTPUT);
  pinMode(PIN_ECHO, INPUT);
  digitalWrite(PIN_TRIG, LOW);

  Wire.begin(21, 22);
  Wire.setClock(I2C_CLOCK);

  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C) &&
      !display.begin(SSD1306_SWITCHCAPVCC, 0x3D)) {
    Serial.println(F("Display SSD1306 nao encontrado (0x3C/0x3D)"));
  }

  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(2);
  display.setCursor(7, 0);  display.println(F("SISTEMA"));
  display.setCursor(30, 32); display.println(F("ESTAVEL"));
  display.display();

  carregarCalibracao();
  iniciarRede();
  delay(1500);
}

void loop() {
  unsigned long agora = millis();

  if (agora - ultimaMedida >= INTERVALO_MEDIDA_MS) {
    ultimaMedida = agora;
    processarMedida();
  }

  if (emAlerta && !sensorComErro && agora - ultimoPisca >= INTERVALO_PISCA_MS) {
    ultimoPisca = agora;
    exibirTextoAlerta = !exibirTextoAlerta;
    precisaRedesenhar = true;
  }

  manterMqtt(agora);
  bool wifiConectado = WiFi.status() == WL_CONNECTED;
  if (wifiConectado != wifiConectadoNaTela) {
    wifiConectadoNaTela = wifiConectado;
    precisaRedesenhar = true;
  }
  if (mqtt.connected() != mqttConectadoNaTela) {
    mqttConectadoNaTela = mqtt.connected();
    precisaRedesenhar = true;
  }

  if (precisaRedesenhar && (filtroIniciado || sensorComErro)) {
    precisaRedesenhar = false;
    desenharTela();
  }
}
