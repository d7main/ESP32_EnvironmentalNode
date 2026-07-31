# Web Flasher

Flash the latest pre-built firmware directly from your browser — no local toolchain, Python, or `esptool.py` required.

!!! warning "Browser & Driver Requirements"
    **Supported browsers:** Google Chrome 89+ or Microsoft Edge 89+.
    The [Web Serial API](https://caniuse.com/web-serial) is **not** available in Firefox or Safari.

    **USB-to-UART driver:** The ESP32-C3-DevKitM-1 uses a **CH340/CH341** USB-UART bridge.
    If your operating system does not enumerate the board as a serial port:

    - **Windows:** Install the [CH34x driver from WCH](https://www.wch-ic.com/downloads/CH341SER_EXE.html).
    - **macOS / Linux:** The CH340 kernel module is included by default since macOS 11 / Linux 3.x — no additional driver needed.

---

## Flashing Instructions

1. **Connect** the ESP32-C3-DevKitM-1 board to your computer via USB.
2. Click **Install** below and select the correct COM / serial port when prompted.
3. The tool will erase flash, then write bootloader, partition table, and application in sequence.
4. After flashing completes, the board resets automatically and launches the SoftAP configuration portal.

!!! note "First flash — full erase"
    Selecting **Erase device** in the installer dialog wipes any existing NVS configuration.
    After a fresh flash, the node will start in unconfigured mode (SoftAP portal launches automatically).
    If you are updating firmware only and want to keep your existing settings, **uncheck** the erase option.

---

## Installer

<div style="display:flex; align-items:center; gap:1rem; flex-wrap:wrap; padding:1.5rem; border:1px solid var(--md-default-fg-color--lightest); border-radius:0.5rem; background:var(--md-code-bg-color);">
  <esp-web-install-button manifest="manifest.json">
    <button slot="activate" style="
      background:#009485;
      color:#fff;
      border:none;
      border-radius:0.4rem;
      padding:0.65rem 1.4rem;
      font-size:1rem;
      font-weight:600;
      cursor:pointer;
      transition:background 0.2s;
    " onmouseover="this.style.background='#00695c'"
       onmouseout="this.style.background='#009485'">
      ⚡ Flash Firmware
    </button>
    <span slot="unsupported" style="color:var(--md-typeset-color);">
      ⚠️ Your browser does not support Web Serial.
      Please use <strong>Chrome</strong> or <strong>Edge</strong>.
    </span>
    <span slot="not-allowed" style="color:var(--md-typeset-color);">
      ⚠️ Web Serial is not available on this page.
      Ensure the site is served over <strong>HTTPS</strong>.
    </span>
  </esp-web-install-button>
  <span style="font-size:0.9rem; color:var(--md-default-fg-color--light);">
    Firmware v2.0 &nbsp;|&nbsp; ESP32-C3 &nbsp;|&nbsp; ESP-IDF v5.5.0
  </span>
</div>

<script
  type="module"
  src="https://unpkg.com/esp-web-tools@9.0.3/dist/web/install-button.js?module">
</script>

---

## What Gets Flashed

The manifest targets three binary regions on the ESP32-C3 flash:

| Binary | Offset | Source |
|---|---|---|
| `bootloader.bin` | `0x0000` | PlatformIO build output |
| `partitions.bin` | `0x8000` | Custom 2 MB partition table |
| `firmware.bin` | `0x10000` | Application (ESP-IDF + sensors + portal) |

!!! tip "Hosting your own binaries"
    The `manifest.json` on this site references files under `firmware/`.
    To publish a new release:

    1. Build with `pio run --environment esp32_c3`.
    2. Copy `.pio/build/esp32_c3/bootloader.bin`, `partitions.bin`, and `firmware.bin`
       into `docs/firmware/`.
    3. Commit and push — the GitHub Actions workflow redeploys the site automatically.

---

## Manual Flashing (CLI)

If you prefer `esptool.py`:

```bash
esptool.py --chip esp32c3 --port /dev/ttyUSB0 --baud 460800 \
  write_flash \
  0x0000  .pio/build/esp32_c3/bootloader.bin \
  0x8000  .pio/build/esp32_c3/partitions.bin \
  0x10000 .pio/build/esp32_c3/firmware.bin
```

Or via PlatformIO:

```bash
pio run --environment esp32_c3 --target upload
```
