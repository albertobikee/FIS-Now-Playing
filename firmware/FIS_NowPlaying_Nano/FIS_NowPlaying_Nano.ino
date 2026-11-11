/*
  FIS Now Playing - Arduino Nano (FIS/DIS radio text over 3LB)
  https://github.com/albertobikee/FIS-Now-Playing

  Copyright (C) 2026 Alberto Hernández López

  This program is free software: you can redistribute it and/or modify it
  under the terms of the GNU General Public License as published by the Free
  Software Foundation, either version 3 of the License, or (at your option)
  any later version. This program is distributed WITHOUT ANY WARRANTY.
  See the LICENSE file or <https://www.gnu.org/licenses/>.

  Credits:
    Uses TLBFISLib by domnulvlad (GPL-3.0) and is based on its example
    09.Radio_text: https://github.com/domnulvlad/TLBFISLib
*/

/*
  FIS_NowPlaying_Nano.ino

  Recibe por el puerto serie (D0/RX, desde el ESP32) líneas con el formato
      TITULO|ARTISTA\n
  y las muestra en la zona de radio del cuadro (2 líneas x 8 caracteres).
  Los textos de más de 8 caracteres se leen UNA vez de principio a fin y luego
  vuelven al principio y se quedan quietos hasta la siguiente canción.

    Línea 0 -> título
    Línea 1 -> artista

  Una línea vacía ("|") muestra el mensaje por defecto (HRNDZ'S / TOLEDO).
  El mismo mensaje sale al arrancar y cuando no llegan datos del ESP32 (15 s).
  Basado en el ejemplo 09.Radio_text de TLBFISLib.

  Cableado:
    SCK (D13) -> CLK, MOSI (D11) -> DATA, D9 -> ENA, GND común.
    ESP32 TX (GPIO17) -> Nano/Uno RX (D0), GND común.
  OJO: desconecta el cable de D0 al cargar el sketch por USB.
*/

#include <TLBFISLib.h>
#include <SPI.h>

// ---------- Hardware ----------
#define SPI_INSTANCE SPI
#define ENA_PIN      9

// ---------- Ajustes ----------
#define LINK_BAUD          115200
#define VISIBLE            8       // caracteres que caben por línea
#define MAX_TEXT           60      // longitud máxima guardada por línea
#define SCROLL_STEP_MS     450     // velocidad del scroll (ms por carácter)
#define SCROLL_HOLD_MS     1800    // pausa con el principio visible, antes de mover
#define SCROLL_END_HOLD_MS 1500    // pausa con el final visible, antes de volver
#define STALE_MS           15000UL // sin datos del ESP32 -> mensaje por defecto

// Mensaje por defecto (máx. 8 caracteres por línea para que no haga scroll)
#define IDLE_LINE0         "HRNDZ'S"
#define IDLE_LINE1         "TOLEDO"

void sendFunction(uint8_t data)
{
  SPI_INSTANCE.beginTransaction(SPISettings(125000, MSBFIRST, SPI_MODE3));
  SPI_INSTANCE.transfer(data);
  SPI_INSTANCE.endTransaction();
}

void beginFunction()
{
  SPI_INSTANCE.begin();
}

TLBFISLib FIS(ENA_PIN, sendFunction, beginFunction);

// ---------- Estado de las dos líneas ----------
struct Line {
  char          text[MAX_TEXT + 1];
  uint8_t       len;
  uint8_t       pos;        // posición actual del scroll
  unsigned long nextStep;   // cuándo avanzar
  bool          dirty;      // hay que reescribirla
  bool          valid;      // ya se ha escrito algún texto
  bool          done;       // el scroll ya dio su única pasada
};

Line lines[2];
bool radioOn = false;
bool idleShown = false;       // true mientras se muestra el mensaje por defecto
bool forceRedraw = false;     // lo activa la función de error de la librería
unsigned long lastRx = 0;

// ---------- Limpieza de texto ----------
// Tabla para UTF-8 0xC3 0x80..0xBF (letras acentuadas latinas) -> ASCII
static const char LATIN1_MAP[65] =
  "AAAAAAACEEEEIIIIDNOOOOOXOUUUUYTS"   // 0x80-0x9F (mayúsculas)
  "AAAAAAACEEEEIIIIDNOOOOO/OUUUUYTY";  // 0xA0-0xBF (minúsculas)

// Convierte UTF-8 a ASCII imprimible en mayúsculas. Lo que no conoce -> '?'
void sanitize(const char *in, size_t inLen, char *out, size_t outSize)
{
  size_t o = 0;
  size_t i = 0;
  while (i < inLen && o < outSize - 1) {
    uint8_t c = (uint8_t)in[i];
    char r = 0;

    if (c < 0x80) {                       // ASCII normal
      if (c >= 'a' && c <= 'z') c -= 32;  // mayúsculas
      r = (c >= 0x20 && c <= 0x7E) ? (char)c : ' ';
      i++;
    } else {                              // secuencia UTF-8
      uint8_t extra = (c >= 0xF0) ? 3 : (c >= 0xE0) ? 2 : (c >= 0xC0) ? 1 : 0;
      uint8_t c1 = (i + 1 < inLen) ? (uint8_t)in[i + 1] : 0;
      uint8_t c2 = (i + 2 < inLen) ? (uint8_t)in[i + 2] : 0;

      if (c == 0xC3 && c1 >= 0x80 && c1 <= 0xBF) {
        r = LATIN1_MAP[c1 - 0x80];
      } else if (c == 0xE2 && c1 == 0x80 && (c2 == 0x98 || c2 == 0x99)) {
        r = '\'';
      } else if (c == 0xE2 && c1 == 0x80 && (c2 == 0x9C || c2 == 0x9D)) {
        r = '"';
      } else if (c == 0xE2 && c1 == 0x80 && (c2 == 0x93 || c2 == 0x94)) {
        r = '-';
      } else {
        r = '?';
      }
      i += 1 + extra;
    }
    out[o++] = r;
  }
  out[o] = 0;

  // Quitar espacios del final
  while (o > 0 && out[o - 1] == ' ') out[--o] = 0;
}

// ---------- Gestión de líneas ----------
void setLine(uint8_t idx, const char *raw, size_t rawLen)
{
  Line &l = lines[idx];
  char clean[MAX_TEXT + 1];
  sanitize(raw, rawLen, clean, sizeof(clean));

  // El ESP32 reenvía la misma canción cada pocos segundos: si el texto no ha
  // cambiado, no se reinicia el scroll ni se reescribe la línea.
  if (l.valid && strcmp(clean, l.text) == 0) return;

  strcpy(l.text, clean);
  l.len = strlen(l.text);
  l.pos = 0;
  l.done = false;
  l.valid = true;
  l.nextStep = millis() + SCROLL_HOLD_MS;
  l.dirty = true;
}

void drawLine(uint8_t idx)
{
  Line &l = lines[idx];
  char win[VISIBLE + 1];

  // Siempre se escriben 8 caracteres (rellenando con espacios): así una línea
  // vacía o más corta borra de verdad lo que hubiera antes.
  memset(win, ' ', VISIBLE);
  win[VISIBLE] = 0;

  if (l.len <= VISIBLE) {
    memcpy(win, l.text, l.len);
  } else {
    memcpy(win, l.text + l.pos, VISIBLE);
  }
  FIS.writeRadioText(idx, win);
  l.dirty = false;
}

// Mensaje por defecto: la pantalla nunca se queda apagada
void showIdle()
{
  setLine(0, IDLE_LINE0, strlen(IDLE_LINE0));
  setLine(1, IDLE_LINE1, strlen(IDLE_LINE1));
  radioOn = true;
  idleShown = true;
}

// Una sola pasada: principio -> final -> vuelta al principio y se queda quieto.
void stepScroll(uint8_t idx, unsigned long now)
{
  Line &l = lines[idx];
  if (l.len <= VISIBLE || l.done) return;
  if ((long)(now - l.nextStep) < 0) return;

  uint8_t maxPos = l.len - VISIBLE;     // posición en la que se ve el final
  if (l.pos < maxPos) {
    l.pos++;
    l.nextStep = now + ((l.pos == maxPos) ? SCROLL_END_HOLD_MS : SCROLL_STEP_MS);
    l.dirty = true;
  } else {
    l.pos = 0;                          // ya se ha leído todo: volver al principio
    l.done = true;                      // y no volver a moverse
    l.dirty = true;
  }
}

// ---------- Recepción por serie ----------
// Formato: TITULO|ARTISTA
void handleLine(const char *buf, size_t len)
{
  const char *sep = (const char *)memchr(buf, '|', len);
  size_t titleLen  = sep ? (size_t)(sep - buf) : len;
  const char *art  = sep ? sep + 1 : buf + len;
  size_t artistLen = sep ? (size_t)(buf + len - art) : 0;

  lastRx = millis();

  if (titleLen == 0 && artistLen == 0) {   // línea vacía -> mensaje por defecto
    if (!idleShown) showIdle();
    return;
  }

  setLine(0, buf, titleLen);
  setLine(1, art, artistLen);
  radioOn = true;
  idleShown = false;
}

void readSerial()
{
  static char rx[160];
  static size_t n = 0;

  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\r') continue;
    if (c == '\n') {
      if (n > 0) handleLine(rx, n);
      n = 0;
    } else if (n < sizeof(rx) - 1) {
      rx[n++] = c;
    } else {
      n = 0;  // línea demasiado larga: descartar
    }
  }
}

// ---------- Arduino ----------
void setup()
{
  Serial.begin(LINK_BAUD);

  // Si el cuadro muestra un aviso en esa zona, hay que redibujar después.
  FIS.errorFunction(
    [](unsigned long duration) {
      (void) duration;
      forceRedraw = true;
    }
  );

  FIS.begin();
  FIS.clearRadioText();
  showIdle();     // al arrancar, mensaje por defecto
}

void loop()
{
  FIS.update();   // mantiene la conexión con el cuadro
  readSerial();

  unsigned long now = millis();

  // Sin datos desde hace tiempo (ESP32 apagado o sin iPhone): mensaje por defecto.
  if (!idleShown && now - lastRx > STALE_MS) {
    showIdle();
  }

  if (radioOn) {
    if (forceRedraw) {
      lines[0].dirty = true;
      lines[1].dirty = true;
      forceRedraw = false;
    }
    stepScroll(0, now);
    stepScroll(1, now);
    if (lines[0].dirty) drawLine(0);
    if (lines[1].dirty) drawLine(1);
  } else {
    forceRedraw = false;
  }
}
