// stk_protocol.h - shared radio protocol for the Satellite Training Kit
//
// KEEP IDENTICAL COPIES in firmware/obc/ and firmware/base_station_obc_link/
// (Arduino only compiles files that sit inside the sketch folder).
//
// Link layout
//   OBC (SX1278)  <--raw LoRa-->  base station Nano 2 (SX1278)
//   COM digipeater (E32)  <--E32 transparent mode-->  base station Nano 1 (E32)
//
// Uplink  (ground -> OBC): ASCII text  "STK1 <COMMAND>"
// Downlink(OBC -> ground): binary 44-byte StkTelemetry frame (below)
#pragma once
#include <stdint.h>
#include <stddef.h>

// ---------------- LoRa radio settings (must match on both ends) -----------
#define STK_LORA_FREQ_HZ     433E6
#define STK_LORA_SF          9        // spreading factor 7..12 (higher = longer range, slower)
#define STK_LORA_BW_HZ       125E3    // bandwidth
#define STK_LORA_CR_DENOM    5        // coding rate 4/5
#define STK_LORA_SYNC_WORD   0x5A     // private network id (avoids other LoRa traffic)
#define STK_LORA_TX_DBM      17

// ---------------- Uplink commands ----------------------------------------
#define STK_CMD_PREFIX       "STK1 "  // a command without this prefix is ignored
#define STK_CMD_MAX_LEN      48
// Recognised commands (after the prefix):
//   TLM        request telemetry
//   PING       same as TLM (link test)
//   CAM ON / CAM OFF     camera + video transmitter power (EPS switch)
//   COM ON / COM OFF     digipeater / COM power            (EPS switch)
//   ALL OFF    switch off every switchable load (safe mode)

// ---------------- Downlink telemetry frame -------------------------------
#define STK_SYNC             0xA5
#define STK_TYPE_TELEMETRY   0x01

// status flag bits
#define STK_FLAG_CAMERA_ON   0x01
#define STK_FLAG_COM_ON      0x02
#define STK_FLAG_BMP_OK      0x04
#define STK_FLAG_IMU_OK      0x08
#define STK_FLAG_CMD_REJECT  0x10     // last command was not recognised

// 44 bytes, little-endian (native AVR byte order), no padding.
typedef struct __attribute__((packed)) {
  uint8_t  sync;            //  0  always STK_SYNC
  uint8_t  type;            //  1  STK_TYPE_TELEMETRY
  uint16_t seq;             //  2  frame counter
  uint32_t uptime_ms;       //  4  OBC uptime
  int16_t  temp_lm35_cC;    //  8  LM35 temperature, 0.01 degC
  int16_t  temp_bmp_cC;     // 10  BMP180 temperature, 0.01 degC
  int32_t  pressure_Pa;     // 12  BMP180 pressure, Pa
  int32_t  altitude_cm;     // 16  barometric altitude, cm
  int16_t  accel_raw[3];    // 20  MPU6050 X/Y/Z, +-2 g range -> 16384 LSB/g
  int16_t  gyro_raw[3];     // 26  MPU6050 X/Y/Z, +-250 dps range -> 131 LSB/(deg/s)
  uint16_t vbat_mV;         // 32  battery voltage, mV
  int16_t  ibat_mA;         // 34  battery current (ACS712), mA
  uint8_t  flags;           // 36  STK_FLAG_* bits
  int8_t   last_cmd_rssi;   // 37  RSSI of last received command, dBm
  uint16_t cmd_count;       // 38  commands accepted since boot
  uint8_t  reserved[2];     // 40  spare for future sensors
  uint16_t crc;             // 42  CRC-16/CCITT-FALSE over bytes 0..41
} StkTelemetry;

static_assert(sizeof(StkTelemetry) == 44, "telemetry frame must be 44 bytes");

// CRC-16/CCITT-FALSE: poly 0x1021, init 0xFFFF ("123456789" -> 0x29B1)
static inline uint16_t stk_crc16(const uint8_t *data, size_t len) {
  uint16_t crc = 0xFFFF;
  while (len--) {
    crc ^= (uint16_t)(*data++) << 8;
    for (uint8_t i = 0; i < 8; i++)
      crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
  }
  return crc;
}

static inline void stk_seal(StkTelemetry *t) {
  t->crc = stk_crc16((const uint8_t *)t, sizeof(StkTelemetry) - 2);
}

static inline bool stk_valid(const StkTelemetry *t) {
  return t->sync == STK_SYNC && t->type == STK_TYPE_TELEMETRY &&
         t->crc == stk_crc16((const uint8_t *)t, sizeof(StkTelemetry) - 2);
}
