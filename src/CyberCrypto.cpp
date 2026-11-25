#include "CyberCrypto.h"

#include <M5Cardputer.h>
#include <M5UnitUnified.h>
#include <M5UnitUnifiedCRYPTO.h>
#include <wiring/m5_unit_unified_wiring.hpp>
#include <mbedtls/ecp.h>
#include <mbedtls/gcm.h>
#include <mbedtls/md.h>
#include <mbedtls/platform_util.h>
#include <mbedtls/sha256.h>
#include <mbedtls/version.h>

namespace {

constexpr size_t PUBLIC_KEY_SIZE = 64;
constexpr size_t MASTER_KEY_SIZE = 32;
constexpr size_t NONCE_SIZE = 12;
constexpr size_t TAG_SIZE = 16;
constexpr size_t SALT_SIZE = 16;
constexpr size_t LEGACY_HEADER_SIZE = 4 + 1 + PUBLIC_KEY_SIZE + NONCE_SIZE + 4;
constexpr size_t MASTER_HEADER_SIZE = 4 + 1 + NONCE_SIZE + 4;
constexpr size_t WRAP_HEADER_SIZE = 4 + 1 + PUBLIC_KEY_SIZE + SALT_SIZE + NONCE_SIZE + 4;
constexpr uint8_t FILE_MAGIC[] = {'C', 'K', 'E', '1'};
constexpr uint8_t WRAP_MAGIC[] = {'C', 'K', 'W', '1'};
constexpr uint8_t LEGACY_FILE_VERSION = 1;
constexpr uint8_t MASTER_FILE_VERSION = 2;
constexpr uint8_t WRAP_VERSION = 1;
constexpr uint32_t PASSWORD_KDF_ITERATIONS = 50000;

m5::unit::UnitUnified unitManager;
m5::unit::UnitID cryptoUnit;
bool unitRegistered = false;
bool unitReady = false;
bool masterKeyReady = false;
bool legacyMigrationAllowed = false;
uint8_t masterKey[MASTER_KEY_SIZE]{};

int randomBytes(void*, unsigned char* output, size_t length) {
    uint8_t random[32]{};
    while (length > 0) {
        if (!unitReady || !cryptoUnit.readRandomArray(random)) {
            mbedtls_platform_zeroize(random, sizeof(random));
            return -1;
        }
        const size_t chunk = length < sizeof(random) ? length : sizeof(random);
        memcpy(output, random, chunk);
        output += chunk;
        length -= chunk;
    }
    mbedtls_platform_zeroize(random, sizeof(random));
    return 0;
}

bool makeEphemeralPublicKey(uint8_t publicKey[PUBLIC_KEY_SIZE]) {
    mbedtls_ecp_group group;
    mbedtls_mpi privateKey;
    mbedtls_ecp_point publicPoint;
    mbedtls_ecp_group_init(&group);
    mbedtls_mpi_init(&privateKey);
    mbedtls_ecp_point_init(&publicPoint);

    uint8_t uncompressed[1 + PUBLIC_KEY_SIZE]{};
    size_t written = 0;
    bool ok = mbedtls_ecp_group_load(&group, MBEDTLS_ECP_DP_SECP256R1) == 0 &&
              mbedtls_ecp_gen_keypair(&group, &privateKey, &publicPoint, randomBytes, nullptr) == 0 &&
              mbedtls_ecp_point_write_binary(&group, &publicPoint, MBEDTLS_ECP_PF_UNCOMPRESSED, &written,
                                             uncompressed, sizeof(uncompressed)) == 0 &&
              written == sizeof(uncompressed) && uncompressed[0] == 0x04;
    if (ok) {
        memcpy(publicKey, uncompressed + 1, PUBLIC_KEY_SIZE);
    }

    mbedtls_platform_zeroize(uncompressed, sizeof(uncompressed));
    mbedtls_ecp_point_free(&publicPoint);
    mbedtls_mpi_free(&privateKey);
    mbedtls_ecp_group_free(&group);
    return ok;
}

bool deriveSharedSecret(const uint8_t peerPublicKey[PUBLIC_KEY_SIZE], uint8_t sharedSecret[MASTER_KEY_SIZE]) {
    using m5::unit::atecc608::Slot;
    if (!unitReady || !cryptoUnit.ECDHStoredKey(sharedSecret, Slot::PrimaryPrivateKey, peerPublicKey)) {
        mbedtls_platform_zeroize(sharedSecret, MASTER_KEY_SIZE);
        return false;
    }
    return true;
}

bool deriveLegacyKey(const uint8_t peerPublicKey[PUBLIC_KEY_SIZE], const char* purpose, uint8_t key[MASTER_KEY_SIZE]) {
    uint8_t sharedSecret[MASTER_KEY_SIZE]{};
    if (!purpose || !deriveSharedSecret(peerPublicKey, sharedSecret)) {
        mbedtls_platform_zeroize(sharedSecret, sizeof(sharedSecret));
        return false;
    }

    const size_t purposeLength = strlen(purpose);
    std::vector<uint8_t> kdfInput(sizeof(sharedSecret) + purposeLength);
    memcpy(kdfInput.data(), sharedSecret, sizeof(sharedSecret));
    memcpy(kdfInput.data() + sizeof(sharedSecret), purpose, purposeLength);
#if MBEDTLS_VERSION_MAJOR >= 3
    const int result = mbedtls_sha256(kdfInput.data(), kdfInput.size(), key, 0);
#else
    const int result = mbedtls_sha256_ret(kdfInput.data(), kdfInput.size(), key, 0);
#endif

    mbedtls_platform_zeroize(sharedSecret, sizeof(sharedSecret));
    mbedtls_platform_zeroize(kdfInput.data(), kdfInput.size());
    return result == 0;
}

bool hmacSha256(const uint8_t* key, size_t keyLength, const uint8_t* input, size_t inputLength,
                uint8_t output[MASTER_KEY_SIZE]) {
    const mbedtls_md_info_t* info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    return info && mbedtls_md_hmac(info, key, keyLength, input, inputLength, output) == 0;
}

bool derivePasswordKey(const String& credential, const uint8_t salt[SALT_SIZE],
                       const uint8_t peerPublicKey[PUBLIC_KEY_SIZE], uint8_t key[MASTER_KEY_SIZE]) {
    if (credential.length() == 0 || !unitReady) return false;

    uint8_t sharedSecret[MASTER_KEY_SIZE]{};
    uint8_t saltMaterial[MASTER_KEY_SIZE + SALT_SIZE]{};
    if (!deriveSharedSecret(peerPublicKey, sharedSecret)) {
        mbedtls_platform_zeroize(sharedSecret, sizeof(sharedSecret));
        return false;
    }
    memcpy(saltMaterial, sharedSecret, sizeof(sharedSecret));
    memcpy(saltMaterial + sizeof(sharedSecret), salt, SALT_SIZE);

    std::vector<uint8_t> firstBlock(saltMaterial, saltMaterial + sizeof(saltMaterial));
    firstBlock.push_back(0);
    firstBlock.push_back(0);
    firstBlock.push_back(0);
    firstBlock.push_back(1);

    uint8_t current[MASTER_KEY_SIZE]{};
    uint8_t next[MASTER_KEY_SIZE]{};
    bool ok = hmacSha256(reinterpret_cast<const uint8_t*>(credential.c_str()), credential.length(),
                         firstBlock.data(), firstBlock.size(), current);
    if (ok) {
        memcpy(key, current, sizeof(current));
        for (uint32_t iteration = 1; iteration < PASSWORD_KDF_ITERATIONS; ++iteration) {
            ok = hmacSha256(reinterpret_cast<const uint8_t*>(credential.c_str()), credential.length(),
                            current, sizeof(current), next);
            if (!ok) break;
            memcpy(current, next, sizeof(current));
            for (size_t i = 0; i < sizeof(current); ++i) key[i] ^= current[i];
        }
    }

    mbedtls_platform_zeroize(sharedSecret, sizeof(sharedSecret));
    mbedtls_platform_zeroize(saltMaterial, sizeof(saltMaterial));
    mbedtls_platform_zeroize(firstBlock.data(), firstBlock.size());
    mbedtls_platform_zeroize(current, sizeof(current));
    mbedtls_platform_zeroize(next, sizeof(next));
    if (!ok) mbedtls_platform_zeroize(key, MASTER_KEY_SIZE);
    return ok;
}

void makeAdditionalData(const uint8_t* header, size_t headerLength, const char* purpose,
                       std::vector<uint8_t>& aad) {
    const size_t purposeLength = strlen(purpose);
    aad.resize(headerLength + purposeLength);
    memcpy(aad.data(), header, headerLength);
    memcpy(aad.data() + headerLength, purpose, purposeLength);
}

bool deriveMasterDataKey(const char* purpose, uint8_t key[MASTER_KEY_SIZE]) {
    if (!masterKeyReady || !purpose) return false;
    return hmacSha256(masterKey, sizeof(masterKey),
                      reinterpret_cast<const uint8_t*>(purpose), strlen(purpose), key);
}

bool normalizeRecoveryCode(const String& input, String& normalized) {
    normalized = "";
    for (size_t i = 0; i < input.length(); ++i) {
        const char c = input[i];
        if (c == '-' || c == ' ') continue;
        const char upper = static_cast<char>(toupper(static_cast<unsigned char>(c)));
        if (!((upper >= 'A' && upper <= 'H') || (upper >= 'J' && upper <= 'N') ||
              (upper >= 'P' && upper <= 'Z') || (upper >= '2' && upper <= '9'))) {
            return false;
        }
        normalized += upper;
    }
    return normalized.length() == 26;
}

bool wrapMasterKey(const uint8_t dataKey[MASTER_KEY_SIZE], const String& credential, const char* purpose,
                   std::vector<uint8_t>& output) {
    if (!unitReady || credential.length() == 0 || !purpose) return false;

    uint8_t ephemeralPublicKey[PUBLIC_KEY_SIZE]{};
    uint8_t salt[SALT_SIZE]{};
    uint8_t nonce[NONCE_SIZE]{};
    uint8_t wrappingKey[MASTER_KEY_SIZE]{};
    if (!makeEphemeralPublicKey(ephemeralPublicKey) || randomBytes(nullptr, salt, sizeof(salt)) != 0 ||
        randomBytes(nullptr, nonce, sizeof(nonce)) != 0 ||
        !derivePasswordKey(credential, salt, ephemeralPublicKey, wrappingKey)) {
        mbedtls_platform_zeroize(ephemeralPublicKey, sizeof(ephemeralPublicKey));
        mbedtls_platform_zeroize(salt, sizeof(salt));
        mbedtls_platform_zeroize(nonce, sizeof(nonce));
        mbedtls_platform_zeroize(wrappingKey, sizeof(wrappingKey));
        return false;
    }

    output.assign(WRAP_HEADER_SIZE + MASTER_KEY_SIZE + TAG_SIZE, 0);
    uint8_t* header = output.data();
    memcpy(header, WRAP_MAGIC, sizeof(WRAP_MAGIC));
    header[4] = WRAP_VERSION;
    memcpy(header + 5, ephemeralPublicKey, sizeof(ephemeralPublicKey));
    memcpy(header + 5 + sizeof(ephemeralPublicKey), salt, sizeof(salt));
    memcpy(header + 5 + sizeof(ephemeralPublicKey) + sizeof(salt), nonce, sizeof(nonce));
    const uint32_t payloadLength = MASTER_KEY_SIZE;
    const size_t lengthOffset = WRAP_HEADER_SIZE - 4;
    header[lengthOffset] = static_cast<uint8_t>(payloadLength >> 24);
    header[lengthOffset + 1] = static_cast<uint8_t>(payloadLength >> 16);
    header[lengthOffset + 2] = static_cast<uint8_t>(payloadLength >> 8);
    header[lengthOffset + 3] = static_cast<uint8_t>(payloadLength);

    std::vector<uint8_t> aad;
    makeAdditionalData(header, WRAP_HEADER_SIZE, purpose, aad);
    mbedtls_gcm_context gcm;
    mbedtls_gcm_init(&gcm);
    bool ok = mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, wrappingKey, 256) == 0 &&
              mbedtls_gcm_crypt_and_tag(&gcm, MBEDTLS_GCM_ENCRYPT, MASTER_KEY_SIZE,
                                        nonce, sizeof(nonce), aad.data(), aad.size(),
                                        dataKey, output.data() + WRAP_HEADER_SIZE,
                                        TAG_SIZE, output.data() + WRAP_HEADER_SIZE + MASTER_KEY_SIZE) == 0;
    mbedtls_gcm_free(&gcm);

    mbedtls_platform_zeroize(ephemeralPublicKey, sizeof(ephemeralPublicKey));
    mbedtls_platform_zeroize(salt, sizeof(salt));
    mbedtls_platform_zeroize(nonce, sizeof(nonce));
    mbedtls_platform_zeroize(wrappingKey, sizeof(wrappingKey));
    mbedtls_platform_zeroize(aad.data(), aad.size());
    if (!ok) output.clear();
    return ok;
}

bool unwrapMasterKey(const uint8_t* wrapper, size_t length, const String& credential, const char* purpose,
                     uint8_t dataKey[MASTER_KEY_SIZE]) {
    if (!unitReady || !wrapper || !purpose ||
        length != WRAP_HEADER_SIZE + MASTER_KEY_SIZE + TAG_SIZE ||
        memcmp(wrapper, WRAP_MAGIC, sizeof(WRAP_MAGIC)) != 0 || wrapper[4] != WRAP_VERSION) {
        return false;
    }

    const size_t lengthOffset = WRAP_HEADER_SIZE - 4;
    const uint32_t payloadLength = (static_cast<uint32_t>(wrapper[lengthOffset]) << 24) |
                                   (static_cast<uint32_t>(wrapper[lengthOffset + 1]) << 16) |
                                   (static_cast<uint32_t>(wrapper[lengthOffset + 2]) << 8) |
                                   static_cast<uint32_t>(wrapper[lengthOffset + 3]);
    if (payloadLength != MASTER_KEY_SIZE) return false;

    const uint8_t* ephemeralPublicKey = wrapper + 5;
    const uint8_t* salt = wrapper + 5 + PUBLIC_KEY_SIZE;
    const uint8_t* nonce = salt + SALT_SIZE;
    const uint8_t* ciphertext = wrapper + WRAP_HEADER_SIZE;
    const uint8_t* tag = ciphertext + MASTER_KEY_SIZE;
    uint8_t wrappingKey[MASTER_KEY_SIZE]{};
    if (!derivePasswordKey(credential, salt, ephemeralPublicKey, wrappingKey)) {
        mbedtls_platform_zeroize(wrappingKey, sizeof(wrappingKey));
        return false;
    }

    std::vector<uint8_t> aad;
    makeAdditionalData(wrapper, WRAP_HEADER_SIZE, purpose, aad);
    mbedtls_gcm_context gcm;
    mbedtls_gcm_init(&gcm);
    const bool ok = mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, wrappingKey, 256) == 0 &&
                    mbedtls_gcm_auth_decrypt(&gcm, MASTER_KEY_SIZE, nonce, NONCE_SIZE,
                                             aad.data(), aad.size(), tag, TAG_SIZE,
                                             ciphertext, dataKey) == 0;
    mbedtls_gcm_free(&gcm);

    mbedtls_platform_zeroize(wrappingKey, sizeof(wrappingKey));
    mbedtls_platform_zeroize(aad.data(), aad.size());
    if (!ok) mbedtls_platform_zeroize(dataKey, MASTER_KEY_SIZE);
    return ok;
}

bool openLegacy(const uint8_t* data, size_t length, const char* purpose, String& plaintext) {
    if (!legacyMigrationAllowed || length < LEGACY_HEADER_SIZE + TAG_SIZE) return false;

    const uint32_t payloadLength = (static_cast<uint32_t>(data[LEGACY_HEADER_SIZE - 4]) << 24) |
                                   (static_cast<uint32_t>(data[LEGACY_HEADER_SIZE - 3]) << 16) |
                                   (static_cast<uint32_t>(data[LEGACY_HEADER_SIZE - 2]) << 8) |
                                   static_cast<uint32_t>(data[LEGACY_HEADER_SIZE - 1]);
    if (length - LEGACY_HEADER_SIZE - TAG_SIZE != payloadLength) return false;

    const uint8_t* ephemeralPublicKey = data + 5;
    const uint8_t* nonce = data + 5 + PUBLIC_KEY_SIZE;
    const uint8_t* ciphertext = data + LEGACY_HEADER_SIZE;
    const uint8_t* tag = ciphertext + payloadLength;
    uint8_t key[MASTER_KEY_SIZE]{};
    if (!deriveLegacyKey(ephemeralPublicKey, purpose, key)) {
        mbedtls_platform_zeroize(key, sizeof(key));
        return false;
    }

    std::vector<uint8_t> aad;
    makeAdditionalData(data, LEGACY_HEADER_SIZE, purpose, aad);
    std::vector<uint8_t> decrypted(payloadLength + 1, 0);
    mbedtls_gcm_context gcm;
    mbedtls_gcm_init(&gcm);
    const bool ok = mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, key, 256) == 0 &&
                    mbedtls_gcm_auth_decrypt(&gcm, payloadLength, nonce, NONCE_SIZE, aad.data(), aad.size(),
                                             tag, TAG_SIZE, ciphertext, decrypted.data()) == 0;
    mbedtls_gcm_free(&gcm);

    if (ok) plaintext = String(reinterpret_cast<const char*>(decrypted.data()));
    mbedtls_platform_zeroize(key, sizeof(key));
    mbedtls_platform_zeroize(aad.data(), aad.size());
    mbedtls_platform_zeroize(decrypted.data(), decrypted.size());
    return ok;
}

}

namespace CyberCrypto {

bool begin() {
    if (!unitRegistered) {
        unitRegistered = m5::unit::wiring::addI2C(unitManager, cryptoUnit);
        if (!unitRegistered) {
            unitReady = false;
            return false;
        }
    }

    unitReady = unitManager.begin();
    if (unitReady) {
        uint8_t probe[32]{};
        unitReady = cryptoUnit.readRandomArray(probe);
        mbedtls_platform_zeroize(probe, sizeof(probe));
    }
    return unitReady;
}

bool refresh() {
    if (unitReady) {
        uint8_t probe[32]{};
        const bool available = cryptoUnit.readRandomArray(probe);
        mbedtls_platform_zeroize(probe, sizeof(probe));
        if (available) return true;
        unitReady = false;
    }
    return begin();
}

bool isReady() {
    return unitReady;
}

bool isUnlocked() {
    return masterKeyReady;
}

void lock() {
    mbedtls_platform_zeroize(masterKey, sizeof(masterKey));
    masterKeyReady = false;
}

void allowLegacyMigration(bool allowed) {
    legacyMigrationAllowed = allowed;
}

bool generateRecoveryCode(String& recoveryCode) {
    uint8_t random[16]{};
    if (randomBytes(nullptr, random, sizeof(random)) != 0) {
        mbedtls_platform_zeroize(random, sizeof(random));
        return false;
    }

    static constexpr char alphabet[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
    recoveryCode = "";
    uint32_t accumulator = 0;
    uint8_t bitCount = 0;
    for (uint8_t byte : random) {
        accumulator = (accumulator << 8) | byte;
        bitCount = static_cast<uint8_t>(bitCount + 8);
        while (bitCount >= 5) {
            bitCount = static_cast<uint8_t>(bitCount - 5);
            recoveryCode += alphabet[(accumulator >> bitCount) & 0x1F];
            accumulator &= bitCount == 0 ? 0 : (1U << bitCount) - 1U;
        }
    }
    if (bitCount > 0) {
        recoveryCode += alphabet[(accumulator << (5 - bitCount)) & 0x1F];
    }
    mbedtls_platform_zeroize(random, sizeof(random));
    return recoveryCode.length() == 26;
}

bool createMasterKey(const String& password, const String& recoveryCode,
                     std::vector<uint8_t>& passwordWrapper,
                     std::vector<uint8_t>& recoveryWrapper) {
    String normalizedRecovery;
    if (!unitReady || password.length() < 8 || !normalizeRecoveryCode(recoveryCode, normalizedRecovery)) {
        return false;
    }

    uint8_t generatedKey[MASTER_KEY_SIZE]{};
    if (randomBytes(nullptr, generatedKey, sizeof(generatedKey)) != 0 ||
        !wrapMasterKey(generatedKey, password, "master-password", passwordWrapper) ||
        !wrapMasterKey(generatedKey, normalizedRecovery, "master-recovery", recoveryWrapper)) {
        passwordWrapper.clear();
        recoveryWrapper.clear();
        mbedtls_platform_zeroize(generatedKey, sizeof(generatedKey));
        return false;
    }

    memcpy(masterKey, generatedKey, sizeof(masterKey));
    masterKeyReady = true;
    mbedtls_platform_zeroize(generatedKey, sizeof(generatedKey));
    return true;
}

bool unlockMasterKey(const String& password, const uint8_t* wrapper, size_t length) {
    uint8_t unlockedKey[MASTER_KEY_SIZE]{};
    const bool ok = unwrapMasterKey(wrapper, length, password, "master-password", unlockedKey);
    if (ok) {
        memcpy(masterKey, unlockedKey, sizeof(masterKey));
        masterKeyReady = true;
    }
    mbedtls_platform_zeroize(unlockedKey, sizeof(unlockedKey));
    return ok;
}

bool unlockMasterKeyWithRecovery(const String& recoveryCode, const uint8_t* wrapper, size_t length) {
    String normalizedRecovery;
    if (!normalizeRecoveryCode(recoveryCode, normalizedRecovery)) return false;

    uint8_t unlockedKey[MASTER_KEY_SIZE]{};
    const bool ok = unwrapMasterKey(wrapper, length, normalizedRecovery, "master-recovery", unlockedKey);
    if (ok) {
        memcpy(masterKey, unlockedKey, sizeof(masterKey));
        masterKeyReady = true;
    }
    mbedtls_platform_zeroize(unlockedKey, sizeof(unlockedKey));
    return ok;
}

bool rewrapMasterKey(const String& password, const String& recoveryCode,
                     std::vector<uint8_t>& passwordWrapper,
                     std::vector<uint8_t>& recoveryWrapper) {
    String normalizedRecovery;
    if (!unitReady || !masterKeyReady || password.length() < 8 ||
        !normalizeRecoveryCode(recoveryCode, normalizedRecovery)) {
        return false;
    }
    if (!wrapMasterKey(masterKey, password, "master-password", passwordWrapper) ||
        !wrapMasterKey(masterKey, normalizedRecovery, "master-recovery", recoveryWrapper)) {
        passwordWrapper.clear();
        recoveryWrapper.clear();
        return false;
    }
    return true;
}

bool isSealed(const uint8_t* data, size_t length) {
    if (!data || length < 5 || memcmp(data, FILE_MAGIC, sizeof(FILE_MAGIC)) != 0) return false;
    if (data[4] == LEGACY_FILE_VERSION) return length >= LEGACY_HEADER_SIZE + TAG_SIZE;
    if (data[4] == MASTER_FILE_VERSION) return length >= MASTER_HEADER_SIZE + TAG_SIZE;
    return false;
}

bool isMasterSealed(const uint8_t* data, size_t length) {
    if (!data || length < MASTER_HEADER_SIZE + TAG_SIZE ||
        memcmp(data, FILE_MAGIC, sizeof(FILE_MAGIC)) != 0 ||
        data[4] != MASTER_FILE_VERSION) {
        return false;
    }
    const size_t lengthOffset = MASTER_HEADER_SIZE - 4;
    const uint32_t payloadLength = (static_cast<uint32_t>(data[lengthOffset]) << 24) |
                                   (static_cast<uint32_t>(data[lengthOffset + 1]) << 16) |
                                   (static_cast<uint32_t>(data[lengthOffset + 2]) << 8) |
                                   static_cast<uint32_t>(data[lengthOffset + 3]);
    return length - MASTER_HEADER_SIZE - TAG_SIZE == payloadLength;
}

bool seal(const String& plaintext, const char* purpose, std::vector<uint8_t>& output) {
    if (!unitReady || !masterKeyReady || !purpose || plaintext.length() > UINT32_MAX) {
        return false;
    }

    uint8_t key[MASTER_KEY_SIZE]{};
    uint8_t nonce[NONCE_SIZE]{};
    if (!deriveMasterDataKey(purpose, key) || randomBytes(nullptr, nonce, sizeof(nonce)) != 0) {
        mbedtls_platform_zeroize(key, sizeof(key));
        mbedtls_platform_zeroize(nonce, sizeof(nonce));
        return false;
    }

    output.assign(MASTER_HEADER_SIZE + plaintext.length() + TAG_SIZE, 0);
    uint8_t* header = output.data();
    memcpy(header, FILE_MAGIC, sizeof(FILE_MAGIC));
    header[4] = MASTER_FILE_VERSION;
    memcpy(header + 5, nonce, sizeof(nonce));
    const uint32_t payloadLength = plaintext.length();
    const size_t lengthOffset = MASTER_HEADER_SIZE - 4;
    header[lengthOffset] = static_cast<uint8_t>(payloadLength >> 24);
    header[lengthOffset + 1] = static_cast<uint8_t>(payloadLength >> 16);
    header[lengthOffset + 2] = static_cast<uint8_t>(payloadLength >> 8);
    header[lengthOffset + 3] = static_cast<uint8_t>(payloadLength);

    std::vector<uint8_t> aad;
    makeAdditionalData(header, MASTER_HEADER_SIZE, purpose, aad);
    mbedtls_gcm_context gcm;
    mbedtls_gcm_init(&gcm);
    const bool ok = mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, key, 256) == 0 &&
                    mbedtls_gcm_crypt_and_tag(&gcm, MBEDTLS_GCM_ENCRYPT, plaintext.length(),
                                              nonce, sizeof(nonce), aad.data(), aad.size(),
                                              reinterpret_cast<const uint8_t*>(plaintext.c_str()),
                                              output.data() + MASTER_HEADER_SIZE,
                                              TAG_SIZE, output.data() + MASTER_HEADER_SIZE + plaintext.length()) == 0;
    mbedtls_gcm_free(&gcm);

    mbedtls_platform_zeroize(key, sizeof(key));
    mbedtls_platform_zeroize(nonce, sizeof(nonce));
    mbedtls_platform_zeroize(aad.data(), aad.size());
    if (!ok) output.clear();
    return ok;
}

bool open(const uint8_t* data, size_t length, const char* purpose, String& plaintext) {
    if (!unitReady || !purpose || !isSealed(data, length)) return false;
    if (data[4] == LEGACY_FILE_VERSION) {
        return openLegacy(data, length, purpose, plaintext);
    }
    if (!masterKeyReady) return false;

    const uint32_t payloadLength = (static_cast<uint32_t>(data[MASTER_HEADER_SIZE - 4]) << 24) |
                                   (static_cast<uint32_t>(data[MASTER_HEADER_SIZE - 3]) << 16) |
                                   (static_cast<uint32_t>(data[MASTER_HEADER_SIZE - 2]) << 8) |
                                   static_cast<uint32_t>(data[MASTER_HEADER_SIZE - 1]);
    if (length - MASTER_HEADER_SIZE - TAG_SIZE != payloadLength) return false;

    const uint8_t* nonce = data + 5;
    const uint8_t* ciphertext = data + MASTER_HEADER_SIZE;
    const uint8_t* tag = ciphertext + payloadLength;
    uint8_t key[MASTER_KEY_SIZE]{};
    if (!deriveMasterDataKey(purpose, key)) {
        mbedtls_platform_zeroize(key, sizeof(key));
        return false;
    }

    std::vector<uint8_t> aad;
    makeAdditionalData(data, MASTER_HEADER_SIZE, purpose, aad);
    std::vector<uint8_t> decrypted(payloadLength + 1, 0);
    mbedtls_gcm_context gcm;
    mbedtls_gcm_init(&gcm);
    const bool ok = mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, key, 256) == 0 &&
                    mbedtls_gcm_auth_decrypt(&gcm, payloadLength, nonce, NONCE_SIZE, aad.data(), aad.size(),
                                             tag, TAG_SIZE, ciphertext, decrypted.data()) == 0;
    mbedtls_gcm_free(&gcm);

    if (ok) plaintext = String(reinterpret_cast<const char*>(decrypted.data()));
    mbedtls_platform_zeroize(key, sizeof(key));
    mbedtls_platform_zeroize(aad.data(), aad.size());
    mbedtls_platform_zeroize(decrypted.data(), decrypted.size());
    return ok;
}

}
