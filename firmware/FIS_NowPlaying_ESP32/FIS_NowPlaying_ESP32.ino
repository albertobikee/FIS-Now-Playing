/*
  FIS Now Playing - ESP32 (AMS -> UART bridge)
  https://github.com/albertobikee/FIS-Now-Playing

  Copyright (C) 2026 Alberto Hernández López

  This program is free software: you can redistribute it and/or modify it
  under the terms of the GNU General Public License as published by the Free
  Software Foundation, either version 3 of the License, or (at your option)
  any later version. This program is distributed WITHOUT ANY WARRANTY.
  See the LICENSE file or <https://www.gnu.org/licenses/>.

  Credits:
    The Apple Media Service (AMS) client, iOS pairing and HID setup are based
    on MusicStation by Roman Likhachev (ralihachev):
    https://github.com/ralihachev/MusicStation
    Those portions are also covered by the MIT License:

      MIT License - Copyright (c) 2026 Roman Likhachev
      Permission is hereby granted, free of charge, to any person obtaining a
      copy of this software and associated documentation files (the
      "Software"), to deal in the Software without restriction, including
      without limitation the rights to use, copy, modify, merge, publish,
      distribute, sublicense, and/or sell copies of the Software, and to
      permit persons to whom the Software is furnished to do so, subject to
      the following conditions:
      The above copyright notice and this permission notice shall be included
      in all copies or substantial portions of the Software.
      THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
      OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
      MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
      IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
      CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
      TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
      SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
*/

/*
  FIS_NowPlaying_ESP32.ino

  Lee del iPhone, por Bluetooth LE (Apple Media Service), el título y el artista
  de lo que esté sonando (Spotify incluido) y los envía por UART al Arduino Nano
  con el formato   TITULO|ARTISTA\n

  Basado en el proyecto MusicStation (ralihachev/MusicStation), que ya lee AMS
  en un ESP32 con Arduino. Aquí se ha quitado la pantalla, el WiFi y las letras.

  Requisitos:
    - Placa: ESP32 Dev Module.
    - Versión PARA EL CORE 3.2.1 de "esp32 by Espressif Systems"
      (la misma que usa MusicStation). No compila con la 3.3.x ni con la 2.0.x.
    - Sin librerías extra: usa el BLE que trae el core.

  Cableado:
    ESP32 GPIO17 (TX2) -> Nano RX (D0)
    GND con GND
    (UART0 = USB queda libre para depurar por el monitor serie a 115200)

  Primer emparejamiento:
    1. Abre el monitor serie. Verás "PIN: xxxxxx".
    2. En el iPhone: Ajustes > Bluetooth > FIS-NowPlaying.
    3. Escribe el PIN que aparece (también se manda al Nano, que lo mostrará
       en el cuadro como "PIN" / "xxxxxx").
    4. Reproduce algo en Spotify. Debe salir en el monitor serie.
  Si el emparejamiento se estropea: "Omitir este dispositivo" en el iPhone y
  vuelve a emparejar (y, si hace falta, borra la flash del ESP32).
*/

#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEClient.h>
#include <BLEHIDDevice.h>
#include <BLESecurity.h>
#include <esp_gap_ble_api.h>
#include <atomic>

#define DEVICE_NAME            "FIS-NowPlaying"

// UART hacia el Nano
#define LINK_RX                16
#define LINK_TX                17
#define LINK_BAUD              115200

// Tiempos
#define SETTLE_MS              400      // espera a que lleguen título y artista
#define RESEND_MS              5000     // reenvío periódico (por si el Nano arranca tarde)
#define PAUSE_CLEAR_MS         45000    // tras tanto tiempo en pausa vuelve HRNDZ'S / TOLEDO
#define NOW_PLAYING_MS         3000     // cuánto se muestra "NOW PLAYING" al reanudar

// Apple Media Service
#define AMS_SERVICE_UUID       "89D3502B-0F36-433A-8EF4-C502AD55F8DC"
#define AMS_ENTITY_UPDATE_UUID "2F7CABCE-808D-411F-9A0C-BB92BA96C102"
#define HID_SERVICE_UUID       0x1812
#define HID_GENERIC_APPEARANCE 0x03C0

#define ENTITY_PLAYER              0
#define ENTITY_TRACK               2
#define PLAYER_ATTR_PLAYBACK_INFO  1
#define TRACK_ATTR_ARTIST          0
#define TRACK_ATTR_TITLE           2

// ---------- Estado compartido entre callbacks BLE y loop() ----------
constexpr size_t TEXT_CAP = 128;

struct Inbox {
  char          artist[TEXT_CAP];
  char          title[TEXT_CAP];
  int           state;        // 0 pausa, 1 reproduciendo, 2 rebobinando, 3 avance rápido
  bool          dirty;
  unsigned long changedAt;
};

Inbox inbox = {};
portMUX_TYPE inboxMux = portMUX_INITIALIZER_UNLOCKED;

BLEServer       *server = nullptr;
BLEClient       *client = nullptr;
BLEHIDDevice    *hidDevice = nullptr;
BLECharacteristic *mediaInput = nullptr;
BLEAddress       phoneAddress("00:00:00:00:00:00");

std::atomic<bool>     phoneConnected{false};
std::atomic<bool>     authComplete{false};
std::atomic<unsigned long> authenticatedAt{0};
std::atomic<uint32_t> pairingPasskey{0};
std::atomic<bool>     failedBondPending{false};
esp_bd_addr_t         failedBondAddress = {0};

bool wasConnected = false;
bool amsReady = false;

// Un mando HID "Consumer Control" real hace que iOS trate el dispositivo como
// accesorio del sistema y lo muestre en Ajustes > Bluetooth (como en MusicStation).
static const uint8_t mediaReportMap[] = {
  0x05, 0x0C, 0x09, 0x01, 0xA1, 0x01, 0x85, 0x01,
  0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x03,
  0x09, 0xCD, 0x09, 0xB5, 0x09, 0xB6, 0x81, 0x02,
  0x75, 0x05, 0x95, 0x01, 0x81, 0x03, 0xC0
};

// ---------- Envío al Nano ----------
void sendLine(const char *title, const char *artist)
{
  Serial2.print(title);
  Serial2.print('|');
  Serial2.print(artist);
  Serial2.print('\n');
  Serial.printf("-> Nano: %s | %s\n", title, artist);
}

// ---------- Callbacks BLE ----------
class ServerCallbacks : public BLEServerCallbacks
{
  void onConnect(BLEServer *, esp_ble_gatts_cb_param_t *param) override
  {
    phoneAddress = BLEAddress(param->connect.remote_bda);
    phoneConnected = true;
    Serial.println("[BLE] onConnect");
  }

  void onDisconnect(BLEServer *, esp_ble_gatts_cb_param_t *) override
  {
    phoneConnected = false;
    authComplete = false;
    authenticatedAt = 0;
    pairingPasskey = 0;
    Serial.println("[BLE] onDisconnect");
  }
};

class SecurityCallbacks : public BLESecurityCallbacks
{
  uint32_t onPassKeyRequest() override { return 123456; }

  void onPassKeyNotify(uint32_t key) override
  {
    Serial.printf("[SEC] onPassKeyNotify: %06lu\n", (unsigned long)key);
    pairingPasskey = key;
  }

  bool onSecurityRequest() override
  {
    Serial.println("[SEC] onSecurityRequest");
    return true;
  }
  bool onConfirmPIN(uint32_t) override { return true; }

  void onAuthenticationComplete(esp_ble_auth_cmpl_t cmpl) override
  {
    Serial.printf("[SEC] onAuthenticationComplete: exito=%d\n", cmpl.success);
    pairingPasskey = 0;
    if (cmpl.success) {
      authenticatedAt = millis();
      authComplete = true;
    } else {
      authComplete = false;
      authenticatedAt = 0;
      memcpy(failedBondAddress, cmpl.bd_addr, sizeof(esp_bd_addr_t));
      failedBondPending = true;
      Serial.printf("Fallo de emparejamiento, motivo %u\n", cmpl.fail_reason);
    }
  }
};

// iOS solo expone AMS a los periféricos que "solicitan" su UUID.
void addAmsSolicitation(BLEAdvertisementData &adv)
{
  BLEUUID uuid(AMS_SERVICE_UUID);
  uint8_t raw[18];
  raw[0] = 17;                                  // longitud: tipo + 16 bytes de UUID
  raw[1] = ESP_BLE_AD_TYPE_128SOL_SRV_UUID;
  memcpy(raw + 2, uuid.getNative()->uuid.uuid128, 16);

  String data;
  data.concat((const char *)raw, sizeof(raw));
  adv.addData(data);
}

// Notificaciones AMS. Se ejecuta fuera del loop(): solo copia a un buzón.
void onEntityUpdate(BLERemoteCharacteristic *, uint8_t *data, size_t len, bool)
{
  if (len < 3) return;
  uint8_t entity = data[0];
  uint8_t attr   = data[1];
  // data[2] = flags (bit 0 = valor truncado por el MTU)

  char value[TEXT_CAP];
  size_t count = min(len - 3, sizeof(value) - 1);
  memcpy(value, data + 3, count);
  value[count] = 0;

  if (entity == ENTITY_PLAYER && attr == PLAYER_ATTR_PLAYBACK_INFO) {
    int state = 0;
    float rate = 0, elapsed = 0;
    if (sscanf(value, "%d,%f,%f", &state, &rate, &elapsed) >= 1) {
      portENTER_CRITICAL(&inboxMux);
      inbox.state = state;
      portEXIT_CRITICAL(&inboxMux);
    }
  } else if (entity == ENTITY_TRACK) {
    char *dest = nullptr;
    if (attr == TRACK_ATTR_ARTIST)     dest = inbox.artist;
    else if (attr == TRACK_ATTR_TITLE) dest = inbox.title;
    if (dest) {
      portENTER_CRITICAL(&inboxMux);
      if (strcmp(dest, value) != 0) {
        strlcpy(dest, value, TEXT_CAP);
        inbox.dirty = true;
        inbox.changedAt = millis();
      }
      portEXIT_CRITICAL(&inboxMux);
    }
  }
}

// ---------- Cliente AMS (el ESP32 lee los datos del iPhone) ----------
bool startAmsClient()
{
  // Se llama solo cuando el emparejamiento y el cifrado ya han terminado.
  if (client && !client->isConnected()) {
    delete client;
    client = nullptr;
  }
  if (!client) {
    client = BLEDevice::createClient();
    if (!client->connect(phoneAddress)) {
      Serial.println("No se pudo crear el cliente GATT");
      return false;
    }
  }
  if (!client->isConnected()) return false;

  client->getServices();   // forzar descubrimiento nuevo
  BLERemoteService *ams = client->getService(AMS_SERVICE_UUID);
  if (!ams) {
    Serial.println("Servicio AMS no encontrado (¿emparejamiento rechazado?)");
    return false;
  }
  BLERemoteCharacteristic *entityUpdate = ams->getCharacteristic(AMS_ENTITY_UPDATE_UUID);
  if (!entityUpdate) {
    Serial.println("Característica Entity Update no encontrada");
    return false;
  }

  entityUpdate->registerForNotify(onEntityUpdate);
  uint8_t playerSub[] = {ENTITY_PLAYER, PLAYER_ATTR_PLAYBACK_INFO};
  uint8_t trackSub[]  = {ENTITY_TRACK, TRACK_ATTR_ARTIST, TRACK_ATTR_TITLE};
  entityUpdate->writeValue(playerSub, sizeof(playerSub), true);
  entityUpdate->writeValue(trackSub, sizeof(trackSub), true);
  Serial.println("AMS listo");
  return true;
}

void cleanupClient()
{
  amsReady = false;
  authComplete = false;
  authenticatedAt = 0;
  portENTER_CRITICAL(&inboxMux);
  inbox = {};
  portEXIT_CRITICAL(&inboxMux);
  if (client) {
    if (client->isConnected()) client->disconnect();
    delete client;
    client = nullptr;
  }
}

// ---------- Setup ----------
void setup()
{
  Serial.begin(115200);
  Serial2.begin(LINK_BAUD, SERIAL_8N1, LINK_RX, LINK_TX);
  Serial.println("Core ESP32: " ESP_ARDUINO_VERSION_STR);

  BLEDevice::init(DEVICE_NAME);
  // Un MTU mayor evita que los títulos largos lleguen cortados.
  // Si da problemas de emparejamiento, comenta esta línea.
  BLEDevice::setMTU(185);
  BLEDevice::setEncryptionLevel(ESP_BLE_SEC_ENCRYPT_MITM);
  BLEDevice::setSecurityCallbacks(new SecurityCallbacks());
  static BLESecurity security;
  security.setAuthenticationMode(ESP_LE_AUTH_REQ_SC_MITM_BOND);
  security.setCapability(ESP_IO_CAP_OUT);
  security.setKeySize(16);
  security.setInitEncryptionKey(ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK);
  security.setRespEncryptionKey(ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK);

  server = BLEDevice::createServer();
  server->setCallbacks(new ServerCallbacks());

  hidDevice = new BLEHIDDevice(server);
  hidDevice->manufacturer()->setValue(DEVICE_NAME);
  hidDevice->pnp(0x02, 0x303A, 0x4001, 0x0100);
  hidDevice->hidInfo(0x00, 0x01);
  hidDevice->reportMap(const_cast<uint8_t *>(mediaReportMap), sizeof(mediaReportMap));
  mediaInput = hidDevice->inputReport(1);
  uint8_t released = 0;
  mediaInput->setValue(&released, 1);
  hidDevice->startServices();
  hidDevice->setBatteryLevel(100);

  BLEAdvertising *advertising = server->getAdvertising();
  BLEAdvertisementData advertisement;
  advertisement.setFlags(ESP_BLE_ADV_FLAG_GEN_DISC | ESP_BLE_ADV_FLAG_BREDR_NOT_SPT);
  advertisement.setCompleteServices(BLEUUID((uint16_t)HID_SERVICE_UUID));
  addAmsSolicitation(advertisement);
  advertising->setAdvertisementData(advertisement);

  BLEAdvertisementData scanResponse;
  scanResponse.setAppearance(HID_GENERIC_APPEARANCE);
  scanResponse.setName(DEVICE_NAME);
  advertising->setScanResponse(true);
  advertising->setScanResponseData(scanResponse);

  delay(250);   // deja que Bluedroid termine de configurar el anuncio
  advertising->start();
  Serial.println("Anunciando como " DEVICE_NAME);
}

// ---------- Loop ----------
void loop()
{
  static unsigned long lastAmsTry = 0;
  static unsigned long lastSend = 0;
  static unsigned long pausedSince = 0;
  static int lastState = -1;
  static bool cleared = true;
  static unsigned long holdUntil = 0;   // mientras dure, se muestra NOW PLAYING
  static uint32_t shownPin = 0;

  unsigned long now = millis();

  // --- Gestión de la conexión ---
  if (phoneConnected && !wasConnected) {
    wasConnected = true;
    lastAmsTry = 0;
    Serial.println("iPhone conectado");
  } else if (phoneConnected && wasConnected && authComplete && !amsReady &&
             now - authenticatedAt >= 500 &&
             (lastAmsTry == 0 || now - lastAmsTry > 10000)) {
    lastAmsTry = now;
    amsReady = startAmsClient();
    if (!amsReady) Serial.println("AMS falló; reintento en 10 s");
  } else if (!phoneConnected && wasConnected) {
    wasConnected = false;
    lastAmsTry = 0;
    cleanupClient();
    sendLine("", "");               // apagar el texto del cuadro
    cleared = true;
    lastState = -1;
    delay(500);
    BLEDevice::startAdvertising();
    Serial.println("iPhone desconectado; anunciando de nuevo");
  }

  // Un emparejamiento fallido deja una entrada obsoleta en el ESP32
  if (!phoneConnected && failedBondPending.exchange(false)) {
    esp_bd_addr_t address;
    memcpy(address, failedBondAddress, sizeof(esp_bd_addr_t));
    esp_ble_remove_bond_device(address);
    Serial.println("Emparejamiento fallido: olvida el dispositivo en el iPhone y repite");
  }

  // --- Diagnóstico: estado cada 5 s mientras no esté AMS listo ---
  static unsigned long lastStatus = 0;
  if (phoneConnected && !amsReady && now - lastStatus >= 5000) {
    lastStatus = now;
    Serial.printf("[estado] conectado=%d auth=%d ams=%d\n",
                  (int)phoneConnected, (int)authComplete, (int)amsReady);
  }

  // --- PIN de emparejamiento: monitor serie + cuadro ---
  uint32_t pin = pairingPasskey;
  if (pin != 0 && pin != shownPin) {
    shownPin = pin;
    char digits[8];
    snprintf(digits, sizeof(digits), "%06lu", (unsigned long)pin);
    Serial.printf("PIN: %s\n", digits);
    sendLine("PIN", digits);
  } else if (pin == 0) {
    shownPin = 0;
  }

  // --- Publicar título y artista ---
  if (amsReady) {
    char title[TEXT_CAP], artist[TEXT_CAP];
    int state;
    bool dirty;
    unsigned long changedAt;

    portENTER_CRITICAL(&inboxMux);
    strlcpy(title, inbox.title, sizeof(title));
    strlcpy(artist, inbox.artist, sizeof(artist));
    state = inbox.state;
    dirty = inbox.dirty;
    changedAt = inbox.changedAt;
    portEXIT_CRITICAL(&inboxMux);

    // Spotify a veces añade " • Video disponible" al artista: se recorta.
    char *bullet = strstr(artist, " \xE2\x80\xA2 ");
    if (bullet) *bullet = 0;

    // Cambios de estado reproducción/pausa
    if (state != lastState) {
      int previous = lastState;
      lastState = state;
      cleared = false;
      holdUntil = 0;
      if (state == 0) {                       // se ha pausado
        pausedSince = now;
        sendLine("PAUSE", "");
        lastSend = now;
      } else if (previous == 0) {             // se ha reanudado
        sendLine("NOW", "PLAYING");
        holdUntil = now + NOW_PLAYING_MS;
        lastSend = now;
      } else {
        lastSend = 0;                         // primer estado: enviar el título
      }
    }

    // Terminado el "NOW PLAYING", volver al título y artista
    bool holding = holdUntil != 0 && (long)(holdUntil - now) > 0;
    if (holdUntil != 0 && !holding) {
      holdUntil = 0;
      lastSend = 0;
    }

    if (!holding && !cleared) {
      if (state == 0) {
        // En pausa: refrescar "PAUSE" periódicamente
        if (now - lastSend >= RESEND_MS) {
          sendLine("PAUSE", "");
          lastSend = now;
        }
      } else if (dirty && now - changedAt >= SETTLE_MS) {
        portENTER_CRITICAL(&inboxMux);
        inbox.dirty = false;
        portEXIT_CRITICAL(&inboxMux);
        sendLine(title, artist);
        lastSend = now;
      } else if ((title[0] || artist[0]) &&
                 (lastSend == 0 || now - lastSend >= RESEND_MS)) {
        sendLine(title, artist);
        lastSend = now;
      }
    }

    // Pausa muy larga -> apagar la zona de radio
    if (state == 0 && !cleared && now - pausedSince >= PAUSE_CLEAR_MS) {
      sendLine("", "");
      cleared = true;
    }
  }

  delay(20);
}
