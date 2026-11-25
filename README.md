# 🔐 CyberKey with CRYPTO unit — Standalone Hardware 2FA & Password Vault for M5Stack Cardputer

> **How this version differs from the original:** Adds Crypto Unit-backed encryption with a master-password/recovery lock, a MicroSD password vault and web password management, expanded time-zone support, Wi-Fi/NTP recovery, and Cardputer-specific UI/OTA improvements. BLE HID is off by default; long on-screen labels scroll instead of being cut off. Original CyberKey code by **sndjy**.

<p align="center">
  <b>A standalone TOTP authenticator with a Crypto Unit-backed master-password lock, USB/BLE auto-typing, an encrypted MicroSD password vault, and OTA updates.</b>
</p>

<p align="center">
  <img src="https://img.shields.io/badge/Platform-ESP32--S3-blue?style=flat-square" alt="Platform">
  <img src="https://img.shields.io/badge/Device-M5Stack%20Cardputer-red?style=flat-square" alt="Device">
  <img src="https://img.shields.io/badge/TOTP-RFC%206238-green?style=flat-square" alt="RFC 6238">
  <img src="https://img.shields.io/badge/Version-v1.3.1-brightgreen?style=flat-square" alt="Version">
  <img src="https://img.shields.io/badge/License-MIT-purple?style=flat-square" alt="License">
</p>

---

## 🌟 Overview

**CyberKey** turns your [M5Stack Cardputer](https://docs.m5stack.com/en/core/cardputer) into a dedicated, standalone **Hardware Two-Factor Authenticator (2FA) & Password Autotyper**. 

No waiting for your phone to unlock, no cloud subscriptions, and no reliance on mobile apps. CyberKey computes standard RFC 6238 TOTP codes locally on the ESP32-S3 microcontroller using hardware-accelerated SHA1 crypto.

Whether you're authenticating into GitHub, Google, AWS, or your home server, simply pick your account on the Cardputer screen and let CyberKey do the work.

---

## ✨ Features

- 🔐 **Master Password Startup Lock**: Enter the master password on the Cardputer at every boot before CyberKey decrypts data or starts USB HID, Bluetooth, Wi-Fi, NTP, or the web portal.
- 🗝️ **Offline Recovery Code**: First-time setup generates one recovery code shown once on the display. Store it offline; it can unlock the data and set a replacement master password.
- 🏷️ **Dual-Field Account Identification**:
  - **Line 1 (Large Cyan)**: Primary Service / Site name (e.g. `GitHub`, `Google`, `Proton`, `AWS`).
  - **Line 2 (Muted Gray)**: Account username or email (e.g. `user@example.com`, `admin`).
- ⌨️ **Dual USB HID & Bluetooth BLE Auto-Type ("YubiKey" Mode)**:
  - Plug your Cardputer into any PC, Mac, Linux machine, or iPad using USB-C, **or** pair it wirelessly over Bluetooth Low Energy (**`CyberKey BLE`**).
  - BLE HID is off by default; press **`L`** to enable it for the current session.
  - Press **`Enter`**: automatically types the active 6-digit TOTP code into your focused cursor and presses return!
  - Press **`U`**: automatically types your **Username** for the selected account.
  - Press **`P`**: automatically types your **Password** from the MicroSD password vault.
  - Press **`L`**: start BLE HID advertising, or stop advertising and disconnect the host; the default is restored after reboot.
- 🚀 **GitHub Over-The-Air (OTA) Updates**:
  - Update your firmware wirelessly with a single keypress (<kbd>O</kbd>) or via the web portal.
  - Checks `version.json` in the `morozilnic/cyberkey` fork and downloads `CyberKey-app.bin` from its latest GitHub Release.
  - Low-footprint chunked flash streaming with real-time on-screen progress indicator.
- 🔑 **Google Password Manager MicroSD Import & Encrypted Vault**:
  - Add or update password entries directly from the web portal, with service name, URL, username, password, and optional note; adding requires the Crypto Unit and MicroSD card.
  - Drag-and-drop or upload your exported `passwords.csv` in the CyberKey web interface (or drop it onto the root of the MicroSD card).
  - Automatically parses service names, URLs, usernames, and passwords into encrypted `/passwords.json` on the MicroSD card.
  - Password records require both the Crypto Unit and the master password and are decrypted only in device memory.
  - Smart fuzzy matching automatically links your passwords to your 2FA accounts by service name or username.
- 🔍 **First-Letter Quick Search**:
  - Jump directly to any service across dozens of stored keys by pressing the first letter (`G` for Google/GitHub, `P` for Proton, `A` for AWS).
- 🌐 **Web Management & Captive Portal**:
  - Add, edit, or delete 2FA secrets from any phone or computer browser.
  - **Smart QR Screenshot Decoder**: Drag-and-drop or upload a 2FA QR code screenshot in the browser; CyberKey decodes the secret, parses the issuer and username, and populates the fields automatically.
  - **Captive Portal Fallback**: If no saved Wi-Fi is reachable, CyberKey spins up a `CyberKey-Setup` hotspot at `http://192.168.4.1`.
- 💾 **MicroSD Vault Backup & Restore**:
  - Hit **`B`** on the keyboard to save a backup to your MicroSD card (`/cyberkey_backup.json`).
  - Hit **`R`** to restore accounts and Wi-Fi credentials when the same Crypto Unit is connected.
- 📶 **Multi-Wi-Fi Profile Manager**:
  - Stores up to 5 Wi-Fi profiles.
  - Automatically scans and connects to the strongest available network on boot to synchronize NTP time.
  - Retries Crypto Unit initialization after reconnection, reloads encrypted Wi-Fi profiles, and retries Wi-Fi/NTP.
- 🔋 **Battery Eco-Mode & Auto-Dim**:
  - Screen auto-dims from 80% to 15% brightness after 45 seconds of idle inactivity (restores instantly on any keypress).
  - Press **`W`** to toggle Wi-Fi into sleep mode on-demand to maximize battery life.
- 🔔 **Audio Countdown Ticks**:
  - Optional subtle audio chirp during the final 3 seconds of a TOTP cycle to alert you when a code is about to expire (toggle with `M`).
- 🚨 **Emergency Panic Wipe**:
  - Double-tap `Del` to wipe keys, Wi-Fi profiles, master-password configuration, MicroSD password vault, and SD backups; setup must be completed again.

### 🔐 Crypto Unit-backed secret storage

The M5Stack Unit CRYPTO / Unit ID (ATECC608B-TNGTLS) must be connected to Cardputer Port A. On first setup, CyberKey generates a random data-encryption key. That key is wrapped separately by the master password and by the one-time-display recovery code; each wrapper is also bound to the Crypto Unit's protected ECDH key. Password-derived wrapping keys use PBKDF2-HMAC-SHA256. Account data, Wi-Fi credentials, SD backups, and `/passwords.json` are encrypted with AES-GCM under the data key. The unit's private key and the master data key are never written to storage.

Enter the master password on the Cardputer at each boot. Until it is accepted, the normal interface, USB/BLE HID, Wi-Fi, NTP, and web portal remain stopped. The Crypto Unit can be reinitialized after a disconnect; CyberKey then reloads protected Wi-Fi data and retries connection and time synchronization. Existing records from earlier firmware are migrated after the first successful password setup; keep the Crypto Unit and any MicroSD card with vault/backup files connected until migration finishes.

The recovery code can be entered after a failed password attempt by pressing `R`. It unlocks the existing data and lets you set a new password and recovery code. If both the master password and recovery code are lost, the data cannot be recovered; the emergency wipe erases the stored master wrappers and starts setup again.

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
| <kbd>O</kbd> | **OTA Update**: Check for updates and flash new firmware over Wi-Fi |
| <kbd>W</kbd> | **Wi-Fi Toggle**: Switch Wi-Fi radio ON / SLEEP (Eco-Mode) |
| <kbd>L</kbd> | **BLE HID Toggle**: Start advertising / stop advertising and disconnect the host; off by default |
| <kbd>M</kbd> | Toggle Expiration Audio Chirp (ON / OFF) |
| <kbd>B</kbd> | Quick Backup to MicroSD Card (`/cyberkey_backup.json`) |
| <kbd>R</kbd> | Quick Restore from MicroSD Card |
| <kbd>Del</kbd> | Emergency Panic Wipe (press twice to wipe flash & SD passwords) |

---

## 🚀 Quick Start / Flashing

### Option A: Over-The-Air (OTA) Update (If already running CyberKey)
1. Ensure your Cardputer is connected to Wi-Fi.
2. Press uppercase <kbd>O</kbd> on the Cardputer keyboard (or visit the web portal and click **Check for Updates**).
3. The device will automatically stream the latest firmware and reboot.

OTA checks `https://raw.githubusercontent.com/morozilnic/cyberkey/main/version.json` and downloads the application image from the latest GitHub Release. Publish the release with the asset named exactly `CyberKey-app.bin`.

### Option B: M5Launcher (MicroSD Card)
1. Download `CyberKey-complete.bin`.
2. Copy it to your Cardputer's MicroSD card.
3. Boot into **M5Launcher**, select `CyberKey-complete.bin`, and flash at offset `0x00000`.

### Option C: Web Flasher / esptool (USB-C)
1. Hold the `G0` (Boot) button on your Cardputer and connect the USB-C cable to your computer.
2. Run:
```bash
esptool.py --chip esp32s3 write_flash 0x0000 CyberKey-complete.bin
```

---

## 🛠️ Building from Source

This project is built using [PlatformIO](https://platformio.org/).

1. Clone this repository:
```bash
git clone https://github.com/your-username/cardputer-cyberkey.git
cd cardputer-cyberkey
```

2. Build firmware:
```bash
pio run
```

3. Merge complete 8MB flash binary:
```bash
esptool.py --chip esp32s3 merge_bin -o CyberKey-complete.bin --flash_mode keep --flash_size 8MB 0x0000 .pio/build/m5stack-cardputer/bootloader.bin 0x8000 .pio/build/m5stack-cardputer/partitions.bin 0x10000 .pio/build/m5stack-cardputer/firmware.bin
```

4. Flash directly via USB:
```bash
pio run -t upload
```

---

## 🔒 Security Architecture

- **Air-Gapped Operation**: After initial NTP time sync, Wi-Fi can be completely shut down (`W` key). The device functions 100% offline.
- **Master-password encryption**: AES-GCM protects every secret payload with a random data key. The master password and recovery code each wrap that key using a key derived from PBKDF2-HMAC-SHA256 and the Crypto Unit's protected ECDH secret.
- **Migration and recovery**: Legacy records are readable only during password-gated migration. Backups and vault files are migrated when available; the recovery code permits password replacement without losing the data key.
- **Unit-bound backups**: Encrypted backups require the same Crypto Unit, master password, and data key to restore. Losing both the password and recovery code makes the protected data unrecoverable.

---

## Original Author

The original CyberKey code was created by **sndjy**. This repository builds on that original work.

---

## 📄 License

Distributed under the **MIT License**. See `LICENSE` for more information.
