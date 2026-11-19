"""Satellite Training Kit - ground station console.

The base station has two Arduino Nanos, each on its own USB port:
  Nano 2 (SX1278) - OBC link: telemetry + commands      -> --obc-port
  Nano 1 (E32)    - digipeater link: text messages      -> --digi-port
Either port can be left out. Telemetry is shown live and logged to CSV.

    pip install pyserial
    python ground_station.py --obc-port COM5 --digi-port COM6          (Windows)
    python ground_station.py --obc-port /dev/ttyUSB0 --digi-port /dev/ttyUSB1
    python ground_station.py --selftest             (no hardware needed)

Console commands
    tlm                 request one telemetry frame
    cam on | cam off    camera + video transmitter power
    com on | com off    digipeater / COM power
    all off             safe mode: every switchable load off
    poll <seconds>      request telemetry automatically (poll 0 = stop)
    msg <text>          send a text message through the digipeater
    help | quit
"""
import argparse
import csv
import struct
import sys
import threading
import time
from datetime import datetime
from pathlib import Path

# ---- Telemetry frame: must match firmware/obc/stk_protocol.h -------------
FRAME_FMT = "<BBHIhhii3h3hHhBbH2sH"          # 44 bytes, little-endian
FRAME_FIELDS = ["sync", "type", "seq", "uptime_ms", "temp_lm35_cC", "temp_bmp_cC",
                "pressure_Pa", "altitude_cm", "ax", "ay", "az", "gx", "gy", "gz",
                "vbat_mV", "ibat_mA", "flags", "last_cmd_rssi", "cmd_count",
                "reserved", "crc"]
assert struct.calcsize(FRAME_FMT) == 44

# CSV columns printed by the base-station Arduino after "TLM,"
TLM_FIELDS = ["seq", "uptime_ms", "temp_lm35_cC", "temp_bmp_cC", "pressure_Pa",
              "altitude_cm", "ax", "ay", "az", "gx", "gy", "gz", "vbat_mV",
              "ibat_mA", "flags", "last_cmd_rssi", "cmd_count", "rssi", "snr"]

FLAGS = {0x01: "CAM", 0x02: "COM", 0x04: "BMP", 0x08: "IMU", 0x10: "CMD-REJECTED"}
ACCEL_LSB_PER_G = 16384.0     # MPU6050 at +-2 g
GYRO_LSB_PER_DPS = 131.0      # MPU6050 at +-250 deg/s


def crc16_ccitt(data: bytes) -> int:
    """CRC-16/CCITT-FALSE, identical to stk_crc16() in the firmware."""
    crc = 0xFFFF
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) if crc & 0x8000 else (crc << 1)
            crc &= 0xFFFF
    return crc


def decode_frame(raw: bytes) -> dict:
    """Decode a raw 44-byte frame (useful with an SDR or another receiver)."""
    if len(raw) != 44:
        raise ValueError(f"frame must be 44 bytes, got {len(raw)}")
    vals = struct.unpack(FRAME_FMT, raw)
    d = dict(zip(FRAME_FIELDS, vals))
    if d["sync"] != 0xA5 or crc16_ccitt(raw[:42]) != d["crc"]:
        raise ValueError("bad sync byte or CRC")
    return d


def to_engineering(d: dict) -> dict:
    """Convert raw integer fields to engineering units."""
    return {
        "seq": int(d["seq"]),
        "uptime_s": int(d["uptime_ms"]) / 1000,
        "temp_lm35_C": int(d["temp_lm35_cC"]) / 100,
        "temp_bmp_C": int(d["temp_bmp_cC"]) / 100,
        "pressure_hPa": int(d["pressure_Pa"]) / 100,
        "altitude_m": int(d["altitude_cm"]) / 100,
        "accel_g": tuple(round(int(d[k]) / ACCEL_LSB_PER_G, 3) for k in ("ax", "ay", "az")),
        "gyro_dps": tuple(round(int(d[k]) / GYRO_LSB_PER_DPS, 2) for k in ("gx", "gy", "gz")),
        "vbat_V": int(d["vbat_mV"]) / 1000,
        "ibat_A": int(d["ibat_mA"]) / 1000,
        "flags": [name for bit, name in FLAGS.items() if int(d["flags"]) & bit],
        "cmd_rssi_dBm": int(d["last_cmd_rssi"]),
        "cmd_count": int(d["cmd_count"]),
        "rssi_dBm": int(d["rssi"]) if "rssi" in d else None,
        "snr_dB": float(d["snr"]) if "snr" in d else None,
    }


def show(t: dict) -> None:
    link = f"  link {t['rssi_dBm']} dBm / {t['snr_dB']} dB" if t["rssi_dBm"] is not None else ""
    print(f"\n── Telemetry #{t['seq']}  (OBC up {t['uptime_s']:.0f} s){link}")
    print(f"   Temp   LM35 {t['temp_lm35_C']:6.2f} °C   BMP180 {t['temp_bmp_C']:6.2f} °C")
    print(f"   Baro   {t['pressure_hPa']:8.2f} hPa   altitude {t['altitude_m']:8.2f} m")
    print(f"   Accel  {t['accel_g']} g    Gyro {t['gyro_dps']} °/s")
    print(f"   Power  {t['vbat_V']:.2f} V   {t['ibat_A']:+.3f} A")
    print(f"   Status {', '.join(t['flags']) or '-'}   commands accepted {t['cmd_count']}")


class GroundStation:
    def __init__(self, obc_port, digi_port, baud: int, log_path: Path):
        import serial  # imported here so --selftest works without pyserial
        self.links = {}
        if obc_port:
            self.links["obc"] = serial.Serial(obc_port, baud, timeout=0.2)
        if digi_port:
            self.links["digi"] = serial.Serial(digi_port, baud, timeout=0.2)
        time.sleep(2)                      # each Arduino resets when its port opens
        self.log_path = log_path
        self.poll_s = 0.0
        self.running = True
        new = not log_path.exists()
        self.log = open(log_path, "a", newline="")
        self.writer = csv.writer(self.log)
        if new:
            self.writer.writerow(["timestamp"] + TLM_FIELDS)

    def send(self, link: str, line: str) -> None:
        if link not in self.links:
            print(f"[!] no {link} port given - start with --{link}-port")
            return
        self.links[link].write((line + "\n").encode())

    def reader(self, link: str) -> None:
        ser = self.links[link]
        while self.running:
            try:
                raw = ser.readline().decode(errors="replace").strip()
            except Exception:
                break
            if not raw:
                continue
            kind, _, rest = raw.partition(",")
            if kind == "TLM":
                vals = rest.split(",")
                if len(vals) != len(TLM_FIELDS):
                    print(f"[!] malformed telemetry line: {raw}")
                    continue
                self.writer.writerow([datetime.now().isoformat(timespec="seconds")] + vals)
                self.log.flush()
                show(to_engineering(dict(zip(TLM_FIELDS, vals))))
            elif kind == "DIGI":
                print(f"\n📡 digipeater: {rest}")
            elif kind == "BAD":
                print(f"\n[!] rejected frame ({rest})")
            else:
                print(f"\n· [{link}] {rest or raw}")

    def poller(self) -> None:
        last = 0.0
        while self.running:
            if self.poll_s > 0 and time.time() - last >= self.poll_s:
                self.send("obc", "CMD TLM")
                last = time.time()
            time.sleep(0.1)

    def run(self) -> None:
        for link in self.links:
            threading.Thread(target=self.reader, args=(link,), daemon=True).start()
        threading.Thread(target=self.poller, daemon=True).start()
        ports = ", ".join(f"{k}={v.port}" for k, v in self.links.items())
        print(f"Connected ({ports}). Logging telemetry to {self.log_path}. Type 'help'.")
        simple = {"tlm": "TLM", "ping": "PING", "cam on": "CAM ON", "cam off": "CAM OFF",
                  "com on": "COM ON", "com off": "COM OFF", "all off": "ALL OFF"}
        try:
            while True:
                cmd = input("> ").strip()
                low = cmd.lower()
                if low in ("quit", "exit"):
                    break
                if low == "help":
                    print(__doc__.split("Console commands")[1])
                elif low in simple:
                    self.send("obc", "CMD " + simple[low])
                elif low.startswith("poll"):
                    try:
                        self.poll_s = float(low.split()[1])
                        print(f"auto-poll every {self.poll_s} s" if self.poll_s else "auto-poll off")
                    except (IndexError, ValueError):
                        print("usage: poll <seconds>")
                elif low.startswith("msg "):
                    self.send("digi", "MSG " + cmd[4:])
                elif cmd:
                    print("unknown command - type 'help'")
        except (KeyboardInterrupt, EOFError):
            pass
        self.running = False
        self.log.close()
        for ser in self.links.values():
            ser.close()


def selftest() -> None:
    """Decode a frame produced by the C firmware code (see firmware/README.md)."""
    sample = bytes.fromhex(
        "A501070040E20100E609320AF2890100820F00000000000000407DFF00000000"
        "CC3D64FE0DB903000000112F")
    assert crc16_ccitt(b"123456789") == 0x29B1
    d = decode_frame(sample)
    t = to_engineering(d)
    assert t["seq"] == 7 and t["vbat_V"] == 15.82 and t["temp_lm35_C"] == 25.34
    assert t["accel_g"] == (0.0, 0.0, 1.0) and t["flags"] == ["CAM", "BMP", "IMU"]
    tampered = bytearray(sample); tampered[2] ^= 1
    try:
        decode_frame(bytes(tampered)); raise AssertionError("tampered frame accepted")
    except ValueError:
        pass
    show(t)
    print("\nSelf-test passed: Python decoder matches the firmware frame format.")


def main() -> None:
    ap = argparse.ArgumentParser(description="Satellite Training Kit ground station")
    ap.add_argument("--obc-port", help="serial port of base station Nano 2 (SX1278, OBC link)")
    ap.add_argument("--digi-port", help="serial port of base station Nano 1 (E32, digipeater link)")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--log", type=Path, default=Path("telemetry_log.csv"))
    ap.add_argument("--selftest", action="store_true", help="run offline decoder test")
    a = ap.parse_args()
    if a.selftest:
        selftest()
        return
    if not (a.obc_port or a.digi_port):
        ap.error("give --obc-port and/or --digi-port (or use --selftest)")
    GroundStation(a.obc_port, a.digi_port, a.baud, a.log).run()


if __name__ == "__main__":
    sys.exit(main())
