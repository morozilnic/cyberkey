#include "CyberTyping.h"
#include <BleKeyboard.h>
#include <NimBLEDevice.h>
#include <NimBLEServer.h>

// Forward declaration from USB HID (defined in main.cpp / USBHIDKeyboard)
extern void usbTypePrint(const char* str);
extern void usbTypeKey(uint8_t key);
extern bool isUSBActive();

static BleKeyboard bleKeyboard("CyberKey BLE", "M5Stack", 100);
static bool bleEnabled = false;

bool initBLEKeyboard() {
    return setBLEKeyboardEnabled(true);
}

bool setBLEKeyboardEnabled(bool enabled) {
    if (enabled == bleEnabled) return true;
    if (enabled) {
        if (NimBLEDevice::getInitialized()) {
            if (!NimBLEDevice::startAdvertising()) return false;
        } else {
            bleKeyboard.begin();
        }
        bleEnabled = true;
    } else {
        NimBLEServer* server = NimBLEDevice::getServer();
        if (!server || !NimBLEDevice::stopAdvertising()) return false;
        for (uint16_t connectionId : server->getPeerDevices()) {
            if (server->disconnect(connectionId) != 0) return false;
        }
        bleEnabled = false;
    }
    return true;
}

bool isBLEKeyboardEnabled() {
    return bleEnabled;
}

bool isBLEConnected() {
    return bleEnabled && bleKeyboard.isConnected();
}

void typeString(const String& str) {
    if (isBLEConnected()) {
        bleKeyboard.print(str.c_str());
    } else {
        usbTypePrint(str.c_str());
    }
}

void typeStringWithEnter(const String& str) {
    if (isBLEConnected()) {
        bleKeyboard.print(str.c_str());
        bleKeyboard.write(KEY_RETURN);
    } else {
        usbTypePrint(str.c_str());
        usbTypeKey(0xB0); // KEY_RETURN in USB HID
    }
}

void typeKey(uint8_t key) {
    if (isBLEConnected()) {
        bleKeyboard.write(key);
    } else {
        usbTypeKey(key);
    }
}
