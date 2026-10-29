# 🔐 CyberKey — Standalone Hardware 2FA Vault for M5Stack Cardputer

<p align="center">
  <b>A sleek, air-gapped, open-source hardware TOTP authenticator with USB auto-typing, MicroSD backup, and zero master-password delays.</b>
</p>

<p align="center">
  <img src="https://img.shields.io/badge/Platform-ESP32--S3-blue?style=flat-square" alt="Platform">
  <img src="https://img.shields.io/badge/Device-M5Stack%20Cardputer-red?style=flat-square" alt="Device">
  <img src="https://img.shields.io/badge/TOTP-RFC%206238-green?style=flat-square" alt="RFC 6238">
  <img src="https://img.shields.io/badge/License-MIT-purple?style=flat-square" alt="License">
</p>

---

## 🌟 Overview

**CyberKey** turns your [M5Stack Cardputer](https://docs.m5stack.com/en/core/cardputer) into a dedicated, standalone **Hardware Two-Factor Authenticator (2FA)**. 

No waiting for your phone to unlock, no cloud subscriptions, and no reliance on mobile apps. CyberKey computes standard RFC 6238 TOTP codes locally on the ESP32-S3 microcontroller using hardware-accelerated SHA1 crypto.

Whether you're authenticating into GitHub, Google, AWS, or your home server, simply pick your account on the Cardputer screen and let CyberKey do the work.

---

## ✨ Features

- ⚡ **Instant Boot & Unlock-Free Access**: Boots directly to your active 2FA codes. No mandatory PINs, biometric scans, or tedious boot delays.
- 🏷️ **Dual-Field Account Identification**:
  - **Line 1 (Large Cyan)**: Primary Service / Site name (e.g. `GitHub`, `Google`, `Proton`, `AWS`).
  - **Line 2 (Muted Gray)**: Account username or email (e.g. `user@example.com`, `admin`).
- ⌨️ **Dual USB HID & Bluetooth BLE Auto-Type ("YubiKey" Mode)**:
  - Plug your Cardputer into any PC, Mac, Linux machine, or iPad using USB-C, **or** pair it wirelessly over Bluetooth Low Energy (**`CyberKey BLE`**).
  - Press **`Enter`**: automatically types the active 6-digit TOTP code into your focused cursor and presses return!
  - Press **`U`**: automatically types your **Username** for the selected account.
  - Press **`P`**: automatically types your **Password** from the MicroSD password vault.
- 🔑 **Google Password Manager MicroSD Import**:
  - Drag-and-drop or upload your exported `passwords.csv` in the CyberKey web interface (or drop it onto the root of the MicroSD card).
  - Automatically parses service names, URLs, usernames, and passwords directly into `/passwords.json` on the MicroSD card.
  - Zero consumption of internal flash memory, leaving flash reserved exclusively for high-speed TOTP crypto.
  - Smart fuzzy matching automatically links your passwords to your 2FA accounts by service name or username.
- 🔍 **First-Letter Quick Search**:
  - Jump directly to any service across dozens of stored keys by pressing the first letter (`G` for Google/GitHub, `P` for Proton, `A` for AWS).
- 🌐 **Web Management & Captive Portal**:
  - Add, edit, or delete 2FA secrets from any phone or computer browser.
  - **Smart QR Screenshot Decoder**: Drag-and-drop or upload a 2FA QR code screenshot in the browser; CyberKey decodes the secret, parses the issuer and username, and populates the fields automatically.
  - **Captive Portal Fallback**: If no saved Wi-Fi is reachable, CyberKey spins up a `CyberKey-Setup` hotspot at `http://192.168.4.1`.
- 💾 **MicroSD Vault Backup & Restore**:
  - Hit **`B`** on the keyboard to save an encrypted JSON backup to your MicroSD card (`/cyberkey_backup.json`).
  - Hit **`R`** to restore all accounts and Wi-Fi credentials onto any new device.
- 📶 **Multi-Wi-Fi Profile Manager**:
  - Stores up to 5 Wi-Fi profiles.
  - Automatically scans and connects to the strongest available network on boot to synchronize NTP time.
- 🔋 **Battery Eco-Mode & Auto-Dim**:
  - Screen auto-dims from 80% to 15% brightness after 45 seconds of idle inactivity (restores instantly on any keypress).
  - Press **`W`** to toggle Wi-Fi into sleep mode on-demand to maximize battery life.
- 🔔 **Audio Countdown Ticks**:
  - Optional subtle audio chirp during the final 3 seconds of a TOTP cycle to alert you when a code is about to expire (toggle with `M`).
- 🚨 **Emergency Panic Wipe**:
  - Double-tap `Del` to instantly wipe all keys and Wi-Fi secrets from onboard flash memory.

---

## ⌨️ Cardputer Controls

| Key | Function |
|:---:|---|
| <kbd>Enter</kbd> | **Auto-Type TOTP**: Types active 6-digit TOTP code and presses Enter (BLE / USB) |
| <kbd>U</kbd> | **Auto-Type Username**: Types the account username/email into focused cursor |
| <kbd>P</kbd> | **Auto-Type Password**: Types the linked password stored on MicroSD |
| <kbd>Space</kbd> / <kbd>D</kbd> / <kbd>.</kbd> / <kbd>/</kbd> | Next Account |
| <kbd>A</kbd> / <kbd>,</kbd> / <kbd>;</kbd> | Previous Account |
| <kbd>A</kbd> – <kbd>Z</kbd> | **Quick Jump**: Jump directly to first account starting with that letter |
| <kbd>W</kbd> | **Wi-Fi Toggle**: Switch Wi-Fi radio ON / SLEEP (Eco-Mode) |
| <kbd>M</kbd> | Toggle Expiration Audio Chirp (ON / OFF) |
| <kbd>B</kbd> | Quick Backup to MicroSD Card (`/cyberkey_backup.json`) |
| <kbd>R</kbd> | Quick Restore from MicroSD Card |
| <kbd>Del</kbd> | Emergency Panic Wipe (press twice to wipe flash & SD passwords) |

---

## 🚀 Quick Start / Flashing

### Option A: M5Launcher (Easiest / Over-the-Air)
1. Copy `CyberKey-v1.1-complete.bin` to your Cardputer's MicroSD card.
2. Boot into **M5Launcher**.
3. Select `CyberKey-v1.1-complete.bin` and flash at offset `0x00000`.

### Option B: Web Flasher / esptool (USB-C)
1. Hold the `G0` (Boot) button on your Cardputer and connect the USB-C cable to your computer.
2. Run:
```bash
esptool.py --chip esp32s3 write_flash 0x0000 CyberKey-v1.1-complete.bin
```

---

## 🛠️ Building from Source

This project is built using [PlatformIO](https://platformio.org/).

1. Clone this repository:
```bash
git clone https://github.com/<your-username>/cardputer-cyberkey.git
cd cardputer-cyberkey
```

2. Build firmware:
```bash
pio run
```

3. Flash directly to Cardputer:
```bash
pio run -t upload
```

---

## 🔒 Security Architecture

- **Air-Gapped Operation**: After initial NTP time sync, Wi-Fi can be completely shut down (`W` key). The device functions 100% offline.
- **On-Chip Hardware Crypto**: TOTP codes are calculated on-the-fly using ESP32-S3's hardware-accelerated `mbedtls` cryptographic engine.
- **MicroSD Data Portability**: Backups are written in clean JSON format directly to the SD card root for simple offline archiving.

---

## 📄 License

Distributed under the **MIT License**. See `LICENSE` for more information.
EOF