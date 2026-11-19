/*
 * Satellite Training Kit - On-Board Computer (OBC) firmware
 * Board : Arduino Nano (ATmega328P)
 *
 * What it does
 *   1. Reads MPU6050 (I2C), BMP180 (I2C), LM35, ACS712 and battery divider (ADC).
 *   2. Waits for ground commands over LoRa (SX1278).
 *   3. Executes "STK1 ..." commands: switches camera / COM power through the
 *      EPS high-side MOSFET switches, and replies with a 44-byte telemetry frame.
 *
 * Libraries (Arduino Library Manager)
 *   - "LoRa" by Sandeep Mistry
 *   - "Adafruit BMP085 Library" (works with BMP180)
 *   MPU6050 is read directly with Wire (no extra library, saves RAM).
 *
 * Wiring - matches the OBC V2 PCB (hardware/obc/obc_connections.csv)
 *   D13 SCK, D12 MISO, D11 MOSI -> SX1278 SCK/MISO/MOSI (LoRa2 header)
 *   D10 -> SX1278 NSS, D9 -> SX1278 RESET
 *   SX1278 DIO0 : NOT USED. On the V2 PCB it shares D2 with DIGI_COM_SW, so
 *                 leave LoRa1 pin 5 unconnected. This firmware polls instead.
 *   A4 SDA, A5 SCL -> MPU6050 + BMP180
 *   A0 -> ACS712 current sensor   (backplane J1 pin 10)
 *   A1 -> LM35 temperature sensor (backplane J1 pin 12)
 *   A2 -> battery voltage divider  (REWORK: the V2 PCB puts it on A1 with a
 *         10k/10k divider; move it to A2 and change RVD0 to 30k)
 *   D2 -> DIGI_COM_SW (backplane J1 pin 36 -> EPS Q2 gate)
 *   D3 -> CAMERA_SW   (backplane J1 pin 40 -> EPS Q4 gate)
 *   D4 -> optional status LED (not on the V2 PCB)
 * See docs/design-review.md for the reasons behind each rework.
 *
 * NOTE: the SX1278 is a 3.3 V device, powered from the EPS 3.3 V rail
 * (J1 pin 4). Its SPI inputs should be level-shifted from the 5 V Nano.
 */
#include <SPI.h>
#include <Wire.h>
#include <LoRa.h>
#include <Adafruit_BMP085.h>
#include "stk_protocol.h"

// ---------------- Pins ----------------
const uint8_t PIN_LORA_SS     = 10;
const uint8_t PIN_LORA_RST    = 9;
const int     PIN_LORA_DIO0   = -1;   // not used: polling mode (see wiring notes)
const uint8_t PIN_DIGI_COM_SW = 2;
const uint8_t PIN_CAMERA_SW   = 3;
const uint8_t PIN_STATUS_LED  = 4;    // optional
const uint8_t PIN_ACS712      = A0;
const uint8_t PIN_LM35        = A1;
const uint8_t PIN_VBAT        = A2;   // after rework

// ---------------- Calibration (adjust to your hardware) ----------------
const float ADC_REF_V        = 5.0;                  // Nano analog reference (AVcc)
const float VBAT_DIVIDER     = (30.0 + 10.0) / 10.0; // RVD0 30k / RVD1 10k: 16.8 V -> 4.2 V
const float ACS712_V_PER_A   = 0.185;                // ACS712-05B sensitivity
const float SEA_LEVEL_PA     = 101325.0;             // for barometric altitude
#define BEACON_MS 0UL   // >0 = also send telemetry every BEACON_MS milliseconds
const uint8_t MPU_ADDR       = 0x68;

Adafruit_BMP085 bmp;
bool bmpOk = false, imuOk = false;
bool cameraOn = false, comOn = false, lastRejected = false;
uint16_t seq = 0, cmdCount = 0;
int8_t lastCmdRssi = 0;
float acsZeroV = 2.5;
unsigned long lastBeacon = 0;

// ---------------- Helpers ----------------
float readVolts(uint8_t pin, uint8_t samples = 16) {
  uint32_t sum = 0;
  for (uint8_t i = 0; i < samples; i++) sum += analogRead(pin);
  return (sum / (float)samples) * ADC_REF_V / 1023.0;
}

bool mpuInit() {
  Wire.beginTransmission(MPU_ADDR);
  Wire.write(0x6B);                 // PWR_MGMT_1
  Wire.write(0x00);                 // wake up
  if (Wire.endTransmission() != 0) return false;
  Wire.beginTransmission(MPU_ADDR);
  Wire.write(0x75);                 // WHO_AM_I
  Wire.endTransmission(false);
  Wire.requestFrom(MPU_ADDR, (uint8_t)1);
  return Wire.available() && Wire.read() == 0x68;
}

bool mpuRead(int16_t accel[3], int16_t gyro[3]) {
  Wire.beginTransmission(MPU_ADDR);
  Wire.write(0x3B);                 // ACCEL_XOUT_H
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom(MPU_ADDR, (uint8_t)14) != 14) return false;
  int16_t v[7];
  for (uint8_t i = 0; i < 7; i++) v[i] = (Wire.read() << 8) | Wire.read();
  for (uint8_t i = 0; i < 3; i++) { accel[i] = v[i]; gyro[i] = v[i + 4]; } // v[3] = die temp
  return true;
}

void setLoad(uint8_t pin, bool on, bool &state) {
  digitalWrite(pin, on ? HIGH : LOW);   // HIGH -> EPS N-MOSFET on -> P-MOSFET on -> load powered
  state = on;
}

void blink(uint8_t n) {
  for (uint8_t i = 0; i < n; i++) {
    digitalWrite(PIN_STATUS_LED, HIGH); delay(40);
    digitalWrite(PIN_STATUS_LED, LOW);  delay(60);
  }
}

// ---------------- Telemetry ----------------
void sendTelemetry() {
  StkTelemetry t;
  memset(&t, 0, sizeof(t));
  t.sync = STK_SYNC;
  t.type = STK_TYPE_TELEMETRY;
  t.seq = seq++;
  t.uptime_ms = millis();

  t.temp_lm35_cC = (int16_t)(readVolts(PIN_LM35) * 10000.0);   // 10 mV/degC -> 0.01 degC

  if (bmpOk) {
    t.temp_bmp_cC = (int16_t)(bmp.readTemperature() * 100.0);
    t.pressure_Pa = bmp.readPressure();
    t.altitude_cm = (int32_t)(bmp.readAltitude(SEA_LEVEL_PA) * 100.0);
  }
  if (imuOk) imuOk = mpuRead(t.accel_raw, t.gyro_raw);

  t.vbat_mV = (uint16_t)(readVolts(PIN_VBAT) * VBAT_DIVIDER * 1000.0);
  t.ibat_mA = (int16_t)((readVolts(PIN_ACS712) - acsZeroV) / ACS712_V_PER_A * 1000.0);

  t.flags = (cameraOn ? STK_FLAG_CAMERA_ON : 0) | (comOn ? STK_FLAG_COM_ON : 0) |
            (bmpOk ? STK_FLAG_BMP_OK : 0)     | (imuOk ? STK_FLAG_IMU_OK : 0) |
            (lastRejected ? STK_FLAG_CMD_REJECT : 0);
  t.last_cmd_rssi = lastCmdRssi;
  t.cmd_count = cmdCount;
  stk_seal(&t);

  LoRa.beginPacket();
  LoRa.write((const uint8_t *)&t, sizeof(t));
  LoRa.endPacket();               // blocking until sent
  blink(1);

  Serial.print(F("TX telemetry #")); Serial.print(t.seq);
  Serial.print(F("  Vbat=")); Serial.print(t.vbat_mV); Serial.print(F(" mV  T=")); Serial.print(t.temp_lm35_cC / 100.0);
  Serial.print(F(" C  flags=0x")); Serial.println(t.flags, HEX);
}

// ---------------- Commands ----------------
void handleCommand(char *msg, int rssi) {
  size_t plen = strlen(STK_CMD_PREFIX);
  if (strncmp(msg, STK_CMD_PREFIX, plen) != 0) return;   // not for us: stay silent

  char *cmd = msg + plen;
  for (int i = strlen(cmd) - 1; i >= 0 && (cmd[i] == '\r' || cmd[i] == '\n' || cmd[i] == ' '); i--) cmd[i] = 0;
  for (char *p = cmd; *p; p++) *p = toupper(*p);

  lastCmdRssi = (int8_t)constrain(rssi, -128, 127);
  lastRejected = false;

  if      (!strcmp(cmd, "TLM") || !strcmp(cmd, "PING")) { /* reply only */ }
  else if (!strcmp(cmd, "CAM ON"))  setLoad(PIN_CAMERA_SW, true, cameraOn);
  else if (!strcmp(cmd, "CAM OFF")) setLoad(PIN_CAMERA_SW, false, cameraOn);
  else if (!strcmp(cmd, "COM ON"))  setLoad(PIN_DIGI_COM_SW, true, comOn);
  else if (!strcmp(cmd, "COM OFF")) setLoad(PIN_DIGI_COM_SW, false, comOn);
  else if (!strcmp(cmd, "ALL OFF")) { setLoad(PIN_CAMERA_SW, false, cameraOn); setLoad(PIN_DIGI_COM_SW, false, comOn); }
  else lastRejected = true;

  if (!lastRejected) cmdCount++;
  Serial.print(F("RX cmd \"")); Serial.print(cmd); Serial.print(F("\" rssi="));
  Serial.print(rssi); Serial.println(lastRejected ? F(" -> REJECTED") : F(" -> OK"));

  delay(60);          // let the ground station switch back to receive
  sendTelemetry();    // every command is acknowledged with fresh telemetry
}

// ---------------- Setup / loop ----------------
void setup() {
  Serial.begin(115200);
  pinMode(PIN_CAMERA_SW, OUTPUT);
  pinMode(PIN_DIGI_COM_SW, OUTPUT);
  pinMode(PIN_STATUS_LED, OUTPUT);
  setLoad(PIN_CAMERA_SW, false, cameraOn);     // fail-safe: loads off at boot
  setLoad(PIN_DIGI_COM_SW, false, comOn);

  Wire.begin();
  imuOk = mpuInit();
  bmpOk = bmp.begin();
  acsZeroV = readVolts(PIN_ACS712, 64);        // calibrate current zero with loads off

  LoRa.setPins(PIN_LORA_SS, PIN_LORA_RST, PIN_LORA_DIO0);
  if (!LoRa.begin(STK_LORA_FREQ_HZ)) {
    Serial.println(F("LoRa init FAILED - check wiring"));
    while (true) blink(3);
  }
  LoRa.setSpreadingFactor(STK_LORA_SF);
  LoRa.setSignalBandwidth(STK_LORA_BW_HZ);
  LoRa.setCodingRate4(STK_LORA_CR_DENOM);
  LoRa.setSyncWord(STK_LORA_SYNC_WORD);
  LoRa.setTxPower(STK_LORA_TX_DBM);
  LoRa.enableCrc();

  Serial.print(F("OBC ready. IMU=")); Serial.print(imuOk);
  Serial.print(F(" BMP=")); Serial.print(bmpOk);
  Serial.print(F(" ACS zero=")); Serial.print(acsZeroV, 3); Serial.println(F(" V"));
  blink(2);
}

void loop() {
  int size = LoRa.parsePacket();
  if (size > 0) {
    char buf[STK_CMD_MAX_LEN + 1];
    int n = 0;
    while (LoRa.available()) {
      char c = (char)LoRa.read();
      if (n < STK_CMD_MAX_LEN) buf[n++] = c;
    }
    buf[n] = 0;
    handleCommand(buf, LoRa.packetRssi());
  }

#if BEACON_MS > 0
  if (millis() - lastBeacon >= BEACON_MS) {
    lastBeacon = millis();
    sendTelemetry();
  }
#endif
}
