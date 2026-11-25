#pragma once

#include <Arduino.h>
#include <vector>

namespace CyberCrypto {

bool begin();
bool refresh();
bool isReady();
bool isUnlocked();
void lock();
void allowLegacyMigration(bool allowed);
bool generateRecoveryCode(String& recoveryCode);
bool createMasterKey(const String& password, const String& recoveryCode,
                     std::vector<uint8_t>& passwordWrapper,
                     std::vector<uint8_t>& recoveryWrapper);
bool unlockMasterKey(const String& password, const uint8_t* wrapper, size_t length);
bool unlockMasterKeyWithRecovery(const String& recoveryCode, const uint8_t* wrapper, size_t length);
bool rewrapMasterKey(const String& password, const String& recoveryCode,
                     std::vector<uint8_t>& passwordWrapper,
                     std::vector<uint8_t>& recoveryWrapper);
bool isSealed(const uint8_t* data, size_t length);
bool isMasterSealed(const uint8_t* data, size_t length);
bool seal(const String& plaintext, const char* purpose, std::vector<uint8_t>& output);
bool open(const uint8_t* data, size_t length, const char* purpose, String& plaintext);

}
