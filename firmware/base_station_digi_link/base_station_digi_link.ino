/*
 * Satellite Training Kit - Base station, Nano 1: digipeater link
 * Board : Arduino Nano (TB1 on the base station PCB) + EBYTE E32 (socket P1), USB to laptop
 *
 * Sends text messages to the COM digipeater and shows everything heard on the
 * E32 link, including the digipeater's re-broadcasts.
 *
 * Laptop -> Arduino (115200 baud):   MSG <text>
 * Arduino -> laptop:                 DIGI,<text>   |   INFO,<text>
 *
 * Wiring - matches the base station PCB (hardware/base-station/base_station_connections.csv)
 *   A3 <- E32 TXD   (SoftwareSerial RX)
 *   A4 -> E32 RXD   (SoftwareSerial TX)
 *   A5 -> E32 M0    (driven LOW = normal mode)
 *   A6 -- E32 M1    analog-input only: REWORK - tie M1 to GND
 *   A7 <- E32 AUX   analog-input only: read with analogRead()
 *   E32 VCC/GND from the MINI360 (U1). Make sure the Nano GND is joined to the
 *   radio GND (docs/design-review.md #7).
 */
#include <SoftwareSerial.h>

const uint8_t PIN_E32_RX  = A3;
const uint8_t PIN_E32_TX  = A4;
const uint8_t PIN_E32_M0  = A5;
const uint8_t PIN_E32_AUX = A7;
const uint32_t E32_BAUD = 9600;

SoftwareSerial e32(PIN_E32_RX, PIN_E32_TX);

char hostLine[96];   uint8_t hostLen = 0;
char e32Line[110];   uint8_t e32Len = 0;
unsigned long e32LastByte = 0;

void waitE32Ready(unsigned long timeoutMs = 1000) {
  unsigned long t0 = millis();
  while (analogRead(PIN_E32_AUX) <= 512 && millis() - t0 < timeoutMs) {}
}

// ---------------- Laptop -> digipeater ----------------
void handleHostLine(char *line) {
  if (!strncmp(line, "MSG ", 4)) {
    waitE32Ready();
    e32.print(line + 4);
    e32.print('\n');
    Serial.print(F("INFO,sent to digipeater: ")); Serial.println(line + 4);
  } else if (line[0]) {
    Serial.println(F("INFO,unknown host command (use MSG ...)"));
  }
}

void pollHost() {
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\r') continue;
    if (c == '\n') { hostLine[hostLen] = 0; handleHostLine(hostLine); hostLen = 0; }
    else if (hostLen < sizeof(hostLine) - 1) hostLine[hostLen++] = c;
  }
}

// ---------------- E32 link -> laptop ----------------
void flushE32Line() {
  e32Line[e32Len] = 0;
  if (e32Len) { Serial.print(F("DIGI,")); Serial.println(e32Line); }
  e32Len = 0;
}

void pollE32() {
  while (e32.available()) {
    char c = e32.read();
    e32LastByte = millis();
    if (c == '\r') continue;
    if (c == '\n') flushE32Line();
    else if (e32Len < sizeof(e32Line) - 1) e32Line[e32Len++] = c;
  }
  if (e32Len && millis() - e32LastByte > 100) flushE32Line();   // message without newline
}

void setup() {
  Serial.begin(115200);
  pinMode(PIN_E32_M0, OUTPUT);
  digitalWrite(PIN_E32_M0, LOW);
  e32.begin(E32_BAUD);
  waitE32Ready();
  Serial.println(F("INFO,digipeater link ready"));
}

void loop() {
  pollHost();
  pollE32();
}
