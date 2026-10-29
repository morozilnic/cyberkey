#include "CyberTyping.h"
#include <BleKeyboard.h>

// Forward declaration from USB HID (defined in main.cpp / USBHIDKeyboard)
extern void usbTypePrint(const char* str);
extern void usbTypeKey(uint8_t key);
extern bool isUSBActive();

static BleKeyboard bleKeyboard("CyberKey BLE", "M5Stack", 100);

void initBLEKeyboard() {
    bleKeyboard.begin();
}

bool isBLEConnected() {
    return bleKeyboard.isConnected();
}

void typeString(const String& str) {
    if (bleKeyboard.isConnected()) {
        bleKeyboard.print(str.c_str());
    } else {
        usbTypePrint(str.c_str());
    }
}

void typeStringWithEnter(const String& str) {
    if (bleKeyboard.isConnected()) {
        bleKeyboard.print(str.c_str());
        bleKeyboard.write(KEY_RETURN);
    } else {
        usbTypePrint(str.c_str());
        usbTypeKey(0xB0); // KEY_RETURN in USB HID
    }
}

void typeKey(uint8_t key) {
    if (bleKeyboard.isConnected()) {
        bleKeyboard.write(key);
    } else {
        usbTypeKey(key);
    }
}
