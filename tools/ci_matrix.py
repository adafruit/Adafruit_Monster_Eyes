#!/usr/bin/env python3
"""GitHub Actions build matrix.

Every board this project builds for. The workflow asks
for either the full release set or a small CI test set
"""

import json
import sys

# usbstack=tinyusb is needed on RP2
RP2 = "rp2040:rp2040:{board}:flash={flash}_0,usbstack=tinyusb"
ESP = "esp32:esp32:{board}:PartitionScheme=tinyuf2"
ESP_S3 = ESP + ",USBMode=default"

MB8, MB16, MB2 = 8388608, 16777216, 2097152

GENERAL = [
    # bundle,             fqbn
    ("feather_rp2040",   RP2.format(board="adafruit_feather", flash=MB8)),
    ("metro_rp2040",     RP2.format(board="adafruit_metro", flash=MB16)),
    ("feather_rp2350",   RP2.format(board="adafruit_feather_rp2350_hstx",
                                    flash=MB8)),
    ("metro_rp2350",     RP2.format(board="adafruit_metro_rp2350", flash=MB16)),
    ("feather_esp32s2",  ESP.format(board="adafruit_feather_esp32s2")),
    ("feather_esp32s3",  ESP_S3.format(board="adafruit_feather_esp32s3")),
    ("metro_esp32s2",    ESP.format(board="adafruit_metro_esp32s2")),
    ("metro_esp32s3",    ESP_S3.format(board="adafruit_metro_esp32s3")),
]
PANELS = [("st7789", "EYE_PANEL_ST7789"), ("gc9a01a", "EYE_PANEL_GC9A01A")]

# --- PicoDVI carriers ------------------------------------------------------
DVI = [
    ("feather_rp2040_dvi", RP2.format(board="adafruit_feather_dvi", flash=MB8),
     "adafruit_feather_dvi_cfg"),
    ("pico_picowbell", RP2.format(board="rpipico", flash=MB2),
     "adafruit_dvibell_cfg"),
    ("pico_dvisock", RP2.format(board="rpipico", flash=MB2), "pico_sock_cfg"),
]

# --- Qualia round displays -------------------------------------------------
QUALIA = [
    ("round-2.1in-480", "QUALIA_PANEL_21_480"),
    ("round-2.8in-480", "QUALIA_PANEL_28_480"),
    ("round-4.0in-720", "QUALIA_PANEL_40_720"),
]
QUALIA_FQBN = ESP_S3.format(board="adafruit_qualia_s3_rgb666") + ",PSRAM=opi"

# --- QT Py with the EYESPI BFF --------------------------------------------
QTPY = [
    ("qtpy_rp2040_eyespi_bff",
     RP2.format(board="adafruit_qtpy", flash=MB8), "rp2040"),
    ("qtpy_esp32s2_eyespi_bff",
     ESP.format(board="adafruit_qtpy_esp32s2"), "esp32"),
]


def core_of(fqbn):
    """Which core installs this board."""
    return "rp2040" if fqbn.startswith("rp2040:") else "esp32"


def family_of(fqbn):
    """UF2 family ID for an ESP32 board, or "" for RP2."""
    if not fqbn.startswith("esp32:"):
        return ""
    board = fqbn.split(":")[2].split(",")[0]
    if "esp32s3" in board or "s3_rgb666" in board:
        return "0xc47e5767"  # ESP32-S3
    if "esp32s2" in board:
        return "0xbfdd4eee"  # ESP32-S2
    raise SystemExit(f"No UF2 family known for {board}; add it to family_of()")


def entry(name, fqbn, bundle, defines, asset=None):
    e = {"name": name, "fqbn": fqbn, "core": core_of(fqbn),
         "family": family_of(fqbn), "bundle": bundle, "defines": defines}
    if asset:
        e["asset"] = asset
    return e


def full():
    out = []

    # Eight boards, both eye counts, both panels.
    for bundle, fqbn in GENERAL:
        for panel_name, panel in PANELS:
            for eyes in (1, 2):
                out.append(entry(
                    f"{bundle}-{panel_name}-{eyes}eye", fqbn, bundle,
                    f"-DEYE_PANEL={panel} -DNUM_EYES={eyes}",
                    asset=f"{panel_name}-{eyes}eye"))

    # One eye per board
    for bundle, fqbn, cfg in DVI:
        out.append(entry(
            f"picodvi-solo-{bundle}", fqbn, "picodvi_solo",
            f"-DEYE_PANEL=EYE_PANEL_DVI -DNUM_EYES=1 -DDVI_PIN_CONFIG={cfg}",
            asset=bundle))

    # Two boards, one eye each, synced over STEMMA QT.
    for bundle, fqbn, cfg in DVI:
        for role in ("primary", "secondary"):
            out.append(entry(
                f"picodvi-dual-{bundle}-{role}", fqbn, "dual_picodvi",
                f"-DEYE_PANEL=EYE_PANEL_DVI -DNUM_EYES=1 "
                f"-DDVI_PIN_CONFIG={cfg} -DEYE_SYNC=EYE_SYNC_{role.upper()}",
                asset=f"{bundle}-{role}"))

    for asset, panel in QUALIA:
        out.append(entry(
            f"qualia-{asset}", QUALIA_FQBN, "qualia_s3_rgb666",
            f"-DEYE_PANEL=EYE_PANEL_RGB666 -DNUM_EYES=1 "
            f"-DQUALIA_PANEL={panel}",
            asset=asset))

    for bundle, fqbn, _core in QTPY:
        out.append(entry(
            f"{bundle}-gc9a01a-1eye", fqbn, bundle,
            "-DEYE_PANEL=EYE_PANEL_GC9A01A -DNUM_EYES=1",
            asset="gc9a01a-1eye"))

    return out


def smoke():
    """One build per architecture and per display type.

    Enough to catch a break in any backend without spending a runner on all
    forty-odd release builds for every push.
    """
    want = {
        "feather_rp2040-st7789-2eye",
        "feather_esp32s3-gc9a01a-2eye",
        "picodvi-solo-pico_picowbell",
        "qualia-round-2.1in-480",
    }
    return [e for e in full() if e["name"] in want]


if __name__ == "__main__":
    mode = sys.argv[1] if len(sys.argv) > 1 else None
    rows = smoke() if mode == "smoke" else full()
    if mode in ("smoke", "release"):
        print("include=" + json.dumps(rows, separators=(",", ":")))
    else:
        from collections import defaultdict
        g = defaultdict(list)
        for e in rows:
            g[e["bundle"]].append(e)
        print(f"{len(rows)} builds in {len(g)} bundles\n")
        for b in sorted(g):
            print(f"  {b}.zip")
            for e in g[b]:
                print(f"      {e.get('asset', e['name'])}")
