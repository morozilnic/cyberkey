#pragma once
#include <Arduino.h>
#include <mbedtls/md.h>

class CyberTOTP {
public:
    static int base32Decode(const char* input, uint8_t* output) {
        int buffer = 0;
        int bitsLeft = 0;
        int count = 0;

        for (const char* ptr = input; *ptr; ++ptr) {
            char ch = *ptr;
            int val = -1;
            if (ch >= 'A' && ch <= 'Z') val = ch - 'A';
            else if (ch >= 'a' && ch <= 'z') val = ch - 'a';
            else if (ch >= '2' && ch <= '7') val = ch - '2' + 26;
            else if (ch == ' ' || ch == '-' || ch == '=') continue;
            else continue;

            buffer = (buffer << 5) | val;
            bitsLeft += 5;
            if (bitsLeft >= 8) {
                bitsLeft -= 8;
                output[count++] = (buffer >> bitsLeft) & 0xFF;
            }
        }
        return count;
    }

    static uint32_t generateCode(const char* base32Secret, uint64_t epochSeconds, uint32_t step = 30) {
        uint8_t key[64];
        int keyLen = base32Decode(base32Secret, key);
        if (keyLen <= 0) return 0;

        uint64_t timeStep = epochSeconds / step;
        uint8_t msg[8];
        for (int i = 7; i >= 0; --i) {
            msg[i] = timeStep & 0xFF;
            timeStep >>= 8;
        }

        uint8_t hmacResult[32];
        mbedtls_md_context_t ctx;
        mbedtls_md_init(&ctx);
        mbedtls_md_setup(&ctx, mbedtls_md_info_from_type(MBEDTLS_MD_SHA1), 1);
        mbedtls_md_hmac_starts(&ctx, key, keyLen);
        mbedtls_md_hmac_update(&ctx, msg, 8);
        mbedtls_md_hmac_finish(&ctx, hmacResult);
        mbedtls_md_free(&ctx);

        int offset = hmacResult[19] & 0x0F;
        uint32_t truncatedHash = (hmacResult[offset] & 0x7F) << 24 |
                                 (hmacResult[offset + 1] & 0xFF) << 16 |
                                 (hmacResult[offset + 2] & 0xFF) << 8 |
                                 (hmacResult[offset + 3] & 0xFF);

        return truncatedHash % 1000000;
    }
};
