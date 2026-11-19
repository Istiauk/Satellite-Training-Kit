/*
 * Satellite Training Kit - Base station, Nano 2: OBC link
 * Board : Arduino Nano (TB2 on the base station PCB) + SX1278 (Ra-02), USB to laptop
 *
 * The OBC carries an SX1278, so the ground side of the OBC link must be an
 * SX1278 too (an E32 cannot reliably decode raw SX1278 packets).
 * Chosen fix: the SX1278 is connected to Nano 2 with jumper wires and the
 * board's second E32 socket (P2) is left empty. See docs/design-review.md #5.
 *
 * Laptop -> Arduino (115200 baud, one line per command):
 *   CMD <text>   send "STK1 <text>" to the OBC      e.g.  CMD CAM ON
 *
 * Arduino -> laptop (CSV lines, parsed by ground_station/ground_station.py):
 *   TLM,seq,uptime_ms,lm35_cC,bmp_cC,pressure_Pa,alt_cm,ax,ay,az,gx,gy,gz,
 *       vbat_mV,ibat_mA,flags,cmd_rssi,cmd_count,rssi,snr
 *   BAD,<reason>,<len>,<rssi>      frame failed the size/CRC check
 *   INFO,<text>
 *
 * Library: "LoRa" by Sandeep Mistry
 *
 * Wiring (jumper wires, Nano 2 / TB2)
 *   D10 -> SX1278 NSS     D9 -> SX1278 RESET
 *   D11 -> MOSI   D12 <- MISO   D13 -> SCK
 *   3V3 -> SX1278 3.3V    GND -> SX1278 GND
 *   DIO0 not needed (polling). The SX1278 is a 3.3 V part: level-shift
 *   NSS / SCK / MOSI / RESET from the 5 V Nano.
 */
#include <SPI.h>
#include <LoRa.h>
#include "stk_protocol.h"

const uint8_t PIN_LORA_SS = 10, PIN_LORA_RST = 9;
const int     PIN_LORA_DIO0 = -1;     // polling mode

char hostLine[96];
uint8_t hostLen = 0;

// ---------------- Laptop -> OBC ----------------
void handleHostLine(char *line) {
  if (!strncmp(line, "CMD ", 4)) {
    LoRa.beginPacket();
    LoRa.print(STK_CMD_PREFIX);
    LoRa.print(line + 4);
    LoRa.endPacket();
    Serial.print(F("INFO,sent to OBC: ")); Serial.println(line + 4);
  } else if (line[0]) {
    Serial.println(F("INFO,unknown host command (use CMD ...)"));
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

// ---------------- OBC telemetry -> laptop ----------------
void pollLoRa() {
  int size = LoRa.parsePacket();
  if (size <= 0) return;
  int rssi = LoRa.packetRssi();
  float snr = LoRa.packetSnr();

  if (size != (int)sizeof(StkTelemetry)) {
    while (LoRa.available()) LoRa.read();
    Serial.print(F("BAD,size,")); Serial.print(size); Serial.print(','); Serial.println(rssi);
    return;
  }
  StkTelemetry t;
  uint8_t *p = (uint8_t *)&t;
  for (uint8_t i = 0; i < sizeof(t); i++) p[i] = LoRa.read();
  if (!stk_valid(&t)) {
    Serial.print(F("BAD,crc,")); Serial.print(size); Serial.print(','); Serial.println(rssi);
    return;
  }
  Serial.print(F("TLM,"));
  Serial.print(t.seq); Serial.print(',');
  Serial.print(t.uptime_ms); Serial.print(',');
  Serial.print(t.temp_lm35_cC); Serial.print(',');
  Serial.print(t.temp_bmp_cC); Serial.print(',');
  Serial.print(t.pressure_Pa); Serial.print(',');
  Serial.print(t.altitude_cm); Serial.print(',');
  for (uint8_t i = 0; i < 3; i++) { Serial.print(t.accel_raw[i]); Serial.print(','); }
  for (uint8_t i = 0; i < 3; i++) { Serial.print(t.gyro_raw[i]);  Serial.print(','); }
  Serial.print(t.vbat_mV); Serial.print(',');
  Serial.print(t.ibat_mA); Serial.print(',');
  Serial.print(t.flags); Serial.print(',');
  Serial.print(t.last_cmd_rssi); Serial.print(',');
  Serial.print(t.cmd_count); Serial.print(',');
  Serial.print(rssi); Serial.print(',');
  Serial.println(snr, 1);
}

void setup() {
  Serial.begin(115200);
  LoRa.setPins(PIN_LORA_SS, PIN_LORA_RST, PIN_LORA_DIO0);
  if (!LoRa.begin(STK_LORA_FREQ_HZ)) {
    Serial.println(F("INFO,LoRa init FAILED - check wiring"));
    while (true) {}
  }
  LoRa.setSpreadingFactor(STK_LORA_SF);
  LoRa.setSignalBandwidth(STK_LORA_BW_HZ);
  LoRa.setCodingRate4(STK_LORA_CR_DENOM);
  LoRa.setSyncWord(STK_LORA_SYNC_WORD);
  LoRa.setTxPower(STK_LORA_TX_DBM);
  LoRa.enableCrc();
  Serial.println(F("INFO,OBC link ready"));
}

void loop() {
  pollHost();
  pollLoRa();
}
