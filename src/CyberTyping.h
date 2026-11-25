#pragma once
#include <Arduino.h>

// Initializes BLE HID Keyboard (Advertises as "CyberKey BLE")
bool initBLEKeyboard();
bool setBLEKeyboardEnabled(bool enabled);
bool isBLEKeyboardEnabled();

// Returns true if a host (phone, laptop, iPad) is paired and connected over BLE
bool isBLEConnected();

// Types a string using active transport (BLE if connected, else USB HID)
void typeString(const String& str);

// Types a string followed by Enter using active transport
void typeStringWithEnter(const String& str);

// Types a single keycode (e.g. KEY_RETURN or '\t')
void typeKey(uint8_t key);
