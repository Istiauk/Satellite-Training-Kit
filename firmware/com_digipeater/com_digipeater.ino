/*
 * Satellite Training Kit - COM subsystem: LoRa digipeater
 * Board : Arduino Nano + EBYTE E32-433T30D (1 W LoRa, UART transparent mode)
 * Power : COM_5V from the EPS, switched on/off by the OBC (DIGI_COM_SW)
 *
 * A digipeater ("digital repeater") listens for a text message, appends its
 * own tag, and broadcasts it again so the message reaches a wider area.
 *   in : Hello from ground
 *   out: Hello from ground |via STK-DIGI #12 up=345s
 * Messages that already contain " |via " are never repeated (stops loops).
 *
 * Wiring - matches the Payload & COM PCB (hardware/payload-com/payload_com_connections.csv)
 *   A3 <- E32 TXD   (SoftwareSerial RX)
 *   A4 -> E32 RXD   (SoftwareSerial TX)
 *   A5 -> E32 M0    (driven LOW = normal mode)
 *   A6 -- E32 M1    A6 is analog-input only and cannot drive M1:
 *                   REWORK - tie E32 M1 to GND with a jumper.
 *   A7 <- E32 AUX   analog-input only: read with analogRead()
 *   REWORK - the V2 PCB feeds the Nano's 5 V and GND into D12/D11.
 *   Re-route 5V2 to the +5V pin and GND2 to a GND pin (docs/design-review.md).
 */
#include <SoftwareSerial.h>

const uint8_t PIN_E32_RX  = A3;   // from E32 TXD
const uint8_t PIN_E32_TX  = A4;   // to   E32 RXD
const uint8_t PIN_E32_M0  = A5;
const uint8_t PIN_E32_AUX = A7;   // analog-only pin
const uint32_t E32_BAUD = 9600;   // E32 factory default
const char CALLSIGN[] = "STK-DIGI";
const char VIA_TAG[]  = " |via ";

SoftwareSerial e32(PIN_E32_RX, PIN_E32_TX);
char line[100];
uint8_t len = 0;
unsigned long lastByte = 0;
uint16_t relayCount = 0;

bool e32Ready() {
  return analogRead(PIN_E32_AUX) > 512;     // AUX HIGH = module idle
}

void waitE32Ready(unsigned long timeoutMs = 1000) {
  unsigned long t0 = millis();
  while (!e32Ready() && millis() - t0 < timeoutMs) {}
}

void relay() {
  line[len] = 0;
  len = 0;
  if (line[0] == 0) return;
  if (strstr(line, VIA_TAG)) {                 // already repeated once: ignore
    Serial.print(F("skip (already relayed): ")); Serial.println(line);
    return;
  }
  relayCount++;
  delay(random(150, 500));                     // random back-off so we don't talk over the sender
  waitE32Ready();
  e32.print(line);
  e32.print(VIA_TAG);
  e32.print(CALLSIGN);
  e32.print(F(" #"));
  e32.print(relayCount);
  e32.print(F(" up="));
  e32.print(millis() / 1000);
  e32.print(F("s\n"));
  waitE32Ready();
  Serial.print(F("relayed #")); Serial.print(relayCount); Serial.print(F(": ")); Serial.println(line);
}

void setup() {
  Serial.begin(115200);
  pinMode(PIN_E32_M0, OUTPUT);
  digitalWrite(PIN_E32_M0, LOW);               // M1 is tied LOW by rework -> normal mode
  e32.begin(E32_BAUD);
  randomSeed(analogRead(A2));                  // floating pin -> different back-off per board
  waitE32Ready();
  Serial.println(F("Digipeater ready"));
}

void loop() {
  while (e32.available()) {
    char c = e32.read();
    lastByte = millis();
    if (c == '\r') continue;
    if (c == '\n') relay();
    else if (len < sizeof(line) - 1) line[len++] = c;
  }
  if (len && millis() - lastByte > 100) relay();   // message ended without newline
}
