"""Draw the backplane pinout map from hardware/backplane/backplane_pinout.csv.

Usage (from the repository root):
    pip install matplotlib
    python tools/plot_pinout.py

Writes figures/backplane/backplane_pinout_map.png
"""
import csv
from pathlib import Path

import matplotlib.pyplot as plt
from matplotlib.patches import Patch, Rectangle

ROOT = Path(__file__).resolve().parents[1]
CSV = ROOT / "hardware" / "backplane" / "backplane_pinout.csv"
OUT = ROOT / "figures" / "backplane" / "backplane_pinout_map.png"

COLORS = {
    "ground": "#4a4a4a",
    "power": "#d1495b",
    "analog signal": "#2e86ab",
    "digital control": "#3bb273",
    "not connected": "#ececec",
}
TITLES = {"J1_OBC": "OBC", "J2_EPS": "EPS", "J3_PAYLOAD_COM": "Payload & COM"}


def load():
    pins = {}
    with open(CSV, newline="") as f:
        for r in csv.DictReader(f):
            pins.setdefault(r["header"], []).append(r)
    return pins


def main():
    pins = load()
    fig, axes = plt.subplots(1, 3, figsize=(15, 11))
    for ax, (hdr, rows) in zip(axes, pins.items()):
        for r in rows:
            p = int(r["pin"])
            row = (p + 1) // 2
            col = 0 if p % 2 else 1
            y = 21 - row
            ax.add_patch(Rectangle((col, y), 0.9, 0.85,
                                   color=COLORS[r["signal_type"]], ec="white"))
            dark = r["signal_type"] != "not connected"
            ax.text(col + 0.45, y + 0.42, str(p), ha="center", va="center",
                    fontsize=8, color="white" if dark else "#999")
            if r["net"] != "NC":
                x = -0.1 if col == 0 else 2.0
                ax.text(x, y + 0.42, r["net"], ha="right" if col == 0 else "left",
                        va="center", fontsize=8.5)
        ax.set_xlim(-2.6, 4.6)
        ax.set_ylim(0.5, 21.5)
        ax.set_aspect("equal")
        ax.axis("off")
        ax.set_title(f"{hdr}  ({TITLES[hdr]})", fontsize=12, fontweight="bold")
    fig.legend(handles=[Patch(color=c, label=k) for k, c in COLORS.items()],
               loc="lower center", ncol=5, frameon=False, fontsize=10)
    fig.suptitle("Backplane 2x20 header pinout (2.54 mm pitch) - viewed from schematic",
                 fontsize=14)
    fig.tight_layout(rect=(0, 0.04, 1, 0.96))
    OUT.parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(OUT, dpi=200)
    print(f"Saved {OUT}")


if __name__ == "__main__":
    main()
