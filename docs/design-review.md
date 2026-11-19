# Schematic design review (2026)

In 2026, while rewriting the firmware and transcribing the schematics into the [`hardware/`](../hardware/) tables, I reviewed the original V2 schematics (Fall 2023) line by line. This page lists what I found, why it matters, and how to fix it. The firmware in [`firmware/`](../firmware/) is written for the **fixed** hardware.

## Notation

| Term | Meaning |
|---|---|
| **Net clash** | Two signals routed to the same microcontroller pin |
| **Rework** | A small fix on a built board: cutting a trace, adding a jumper wire, swapping a resistor |
| **ADC** | Analog-to-digital converter input. On the Nano it reads 0–5 V and must never see more than about 5.5 V. |
| **A6 / A7** | Nano pins that are **analog-input only**. They can't be used as digital inputs or outputs. |
| **M0 / M1** | Mode pins of the E32 LoRa module. Both must be LOW for normal transmit/receive. |

## Summary

| # | Board | Issue | Severity | Fix |
|---|---|---|---|---|
| 1 | OBC | D2 used by both LoRa DIO0 and DIGI_COM_SW | 🔴 High | Leave the radio's DIO0 unconnected; firmware polls instead |
| 2 | OBC | A1 used by both the LM35 and the battery divider | 🔴 High | Move the divider to A2 |
| 3 | OBC | Battery divider ratio 1/2 → up to 8.4 V on the ADC | 🔴 High | Change RVD0 to 30 kΩ (ratio 1/4) |
| 4 | Payload & COM | Nano 5 V and GND wired to D12 and D11 | 🔴 High | Re-route to the +5V and GND pins |
| 5 | OBC ↔ Base station | SX1278 on the OBC, E32 modules at the base station | 🔴 High | Base station Nano 2 uses an SX1278 |
| 6 | Payload & COM, Base station | E32 M1 on A6, AUX on A7 (analog-only pins) | 🟠 Medium | Tie M1 to GND; read AUX with `analogRead()` |
| 7 | OBC, Base station | Nano power/GND/RESET pins appear unconnected | 🟠 Medium | Check against the PCB; add wires if missing |
| 8 | Payload & COM | Unlabelled wire stub on VTX1 pin 2 | 🟢 Low | Remove the stub, or label it NC |

## Details

### 1. OBC: D2 net clash
- **What:** the net label `D2` appears on the SX1278's DIO0 pin (LoRa1.5) and on backplane pin 36 (`DIGI_COM_SW`).
- **Why it matters:** DIO0 is an **output** from the radio, and DIGI_COM_SW is an **output** from the Arduino. Two outputs fighting on one wire can damage both, and the COM power would switch on and off whenever a packet arrives.
- **Fix:** don't connect LoRa1 pin 5 (leave the header pin unsoldered or cut the trace). The radio library only needs DIO0 for interrupt mode, so [`obc.ino`](../firmware/obc/obc.ino) checks for packets by polling. D2 is then used only for DIGI_COM_SW, which matches the backplane.

### 2. OBC: A1 net clash
- **What:** `A1` is on backplane pin 12 (LM35 temperature output) and on the battery voltage divider.
- **Why it matters:** both sources drive the same ADC pin, so neither reading is correct.
- **Fix:** cut the divider's connection to A1 and run a wire to **A2**, which is free. The firmware reads current on A0, temperature on A1 and battery voltage on A2.

### 3. OBC: divider overvoltage
- **What:** RVD0 and RVD1 are both 10 kΩ, so the ADC sees V_BAT ÷ 2.
- **Why it matters:** a full 4-cell pack is 4 × 4.2 V = 16.8 V, which puts **8.4 V** on the ADC. That's above the Nano's 5 V limit and can destroy the input.
- **Fix:** RVD0 = 30 kΩ, RVD1 = 10 kΩ gives a ratio of ¼, so 16.8 V reads as 4.2 V. Set `VBAT_DIVIDER = 4.0` in the firmware.

### 4. Payload & COM: Nano power miswire
- **What:** net `5V2` (PAY_5V) goes to Nano pin 15 (**D12**) and `GND2` goes to pin 14 (**D11**). The real supply pins (+5V = 27, GND = 4/29) are left unconnected.
- **Why it matters:** the Nano would only be "powered" backwards through the protection diode of a data pin. That gives an unreliable supply and can damage the chip. It may also explain part of the COM reception problems mentioned in the report.
- **Fix:** re-route 5V2 to pin 27 (+5V) and GND2 to pin 4 or 29 (GND).

### 5. Radio mismatch between the OBC and the base station
- **What:** the OBC carries an **SX1278 (Ra-02) on SPI**. The base station PCB has two 7-pin **EBYTE E32** sockets.
- **Why it matters:** the E32 wraps LoRa in its own built-in packet format, so a raw SX1278 and an E32 don't reliably understand each other. This was probably a cause of the "LoRa reception issues" in the final report.
- **Fix (chosen):** base station **Nano 1 + E32** handles the digipeater link exactly as the PCB shows. **Nano 2 uses an SX1278** connected with jumper wires to its free SPI pins (D9–D13, 3.3 V, GND) to talk to the OBC, and its E32 socket (P2) is left empty. See [`firmware/README.md`](../firmware/README.md).

### 6. E32 mode pins on analog-only pins
- **What:** on both the Payload & COM board and base station Nano 1, M1 → A6 and AUX → A7.
- **Why it matters:** A6 and A7 can't be digital outputs, so the Arduino can't drive M1. A floating mode pin may leave the module out of normal mode.
- **Fix:** connect M1 permanently to GND with a jumper (normal mode needs M0 = M1 = LOW). M0 on A5 is driven LOW by the firmware. AUX is read with `analogRead(A7) > 512`.

### 7. Nano supply, ground and reset pins
- **What:** in the OBC and base station schematic images, the Nano's +5V, GND and RESET pins have no net labels. The OBC's reset button network (S1, RSW0) drives a `RESET` net that isn't on the Nano symbol.
- **Why it matters:** a serial link (Nano ↔ E32) needs a **common ground**, and the OBC Nano needs a supply.
- **Fix:** check on the actual PCB. If they're missing, add wires: OBC +5 net → Nano +5V, all GNDs together, RESET net → Nano RESET.

### 8. VTX1 pin 2 stub
An unlabelled wire is attached to VTX1 pin 2 (Altium flags it with the orange wavy line). It's harmless, but it should be removed or marked NC (no connection).

## Checked and OK
- **EPS high-side switches:** the IRF9540 gate–source voltage is −V<sub>CC</sub> when on. At 12–16.8 V that's within its ±20 V rating. The IRF540 only has to sink the ~1.7 mA through the 10 kΩ pull-up, which a 5 V gate drive handles easily. The pull-down resistors make the switches fail-safe (off).
- **OBC I2C sensors:** MPU6050 and BMP180 share SDA (A4) and SCL (A5), each at its own I2C address (0x68 and 0x77). No conflict.
- **SX1278 SPI wiring:** NSS D10, RST D9, MOSI/MISO/SCK D11–D13 are correct for the Nano's hardware SPI.
- **Backplane alignment:** every board's P1 header net matches the backplane pinout row for row.

## Report corrections
- Battery maximum: **16.8 V** (4 × 4.2 V), not 15.7 V.
- EPS cost: the line items sum to **2,969.33 BDT**; the report states 3,024.33 BDT.
