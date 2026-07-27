// SPDX-License-Identifier: BSD-3-Clause
/* Copyright (c) 2026 RealtekRTL8822C contributors. */

#include "RtwWpaCrypto.hpp"

#include <string.h>

#ifdef RTW_WPA_USERSPACE
#include <CommonCrypto/CommonCryptor.h>
#include <CommonCrypto/CommonDigest.h>
typedef CC_SHA1_CTX SHA1_CTX;
#define SHA1Init CC_SHA1_Init
#define SHA1Update CC_SHA1_Update
#define SHA1Final CC_SHA1_Final
struct aes_decrypt_ctx { uint8_t key[16]; };
static const int aes_good = 0;
static int aes_decrypt_key128(const uint8_t* key, aes_decrypt_ctx* context) {
    memcpy(context->key, key, 16);
    return aes_good;
}
static int aes_decrypt(const uint8_t* input, uint8_t* output, aes_decrypt_ctx* context) {
    size_t moved = 0;
    CCCryptorStatus status = CCCrypt(kCCDecrypt, kCCAlgorithmAES, kCCOptionECBMode,
                                     context->key, 16, nullptr, input, 16,
                                     output, 16, &moved);
    return status == kCCSuccess && moved == 16 ? aes_good : -1;
}
#else
#include <libkern/crypto/aes.h>
#include <libkern/crypto/sha1.h>
#endif

namespace RtwWpaCrypto {

void secureZero(void* data, size_t len) {
    volatile uint8_t* p = static_cast<volatile uint8_t*>(data);
    while (len--) *p++ = 0;
}

bool constantTimeEqual(const uint8_t* a, const uint8_t* b, size_t len) {
    uint8_t difference = 0;
    for (size_t i = 0; i < len; i++) difference |= a[i] ^ b[i];
    return difference == 0;
}

void hmacSha1(const uint8_t* key, size_t keyLen,
              const uint8_t* data, size_t dataLen, uint8_t out[20]) {
    uint8_t keyBlock[64];
    uint8_t innerDigest[20];
    memset(keyBlock, 0, sizeof(keyBlock));
    if (keyLen > sizeof(keyBlock)) {
        SHA1_CTX keyContext;
        SHA1Init(&keyContext);
        SHA1Update(&keyContext, key, keyLen);
        SHA1Final(keyBlock, &keyContext);
    } else if (keyLen != 0) {
        memcpy(keyBlock, key, keyLen);
    }

    uint8_t innerPad[64], outerPad[64];
    for (size_t i = 0; i < sizeof(keyBlock); i++) {
        innerPad[i] = keyBlock[i] ^ 0x36;
        outerPad[i] = keyBlock[i] ^ 0x5c;
    }

    SHA1_CTX context;
    SHA1Init(&context);
    SHA1Update(&context, innerPad, sizeof(innerPad));
    SHA1Update(&context, data, dataLen);
    SHA1Final(innerDigest, &context);
    SHA1Init(&context);
    SHA1Update(&context, outerPad, sizeof(outerPad));
    SHA1Update(&context, innerDigest, sizeof(innerDigest));
    SHA1Final(out, &context);

    secureZero(&context, sizeof(context));
    secureZero(keyBlock, sizeof(keyBlock));
    secureZero(innerPad, sizeof(innerPad));
    secureZero(outerPad, sizeof(outerPad));
    secureZero(innerDigest, sizeof(innerDigest));
}

bool derivePmk(const char* passphrase, const uint8_t* ssid, size_t ssidLen,
               uint8_t pmk[32]) {
    if (!passphrase || !ssid || !pmk || ssidLen > 32) return false;
    size_t passLen = strlen(passphrase);
    if (passLen < 8 || passLen > 63) return false;

    uint8_t salt[36];
    uint8_t u[20], digest[20];
    memcpy(salt, ssid, ssidLen);
    size_t produced = 0;
    for (uint32_t block = 1; produced < 32; block++) {
        salt[ssidLen + 0] = static_cast<uint8_t>(block >> 24);
        salt[ssidLen + 1] = static_cast<uint8_t>(block >> 16);
        salt[ssidLen + 2] = static_cast<uint8_t>(block >> 8);
        salt[ssidLen + 3] = static_cast<uint8_t>(block);
        hmacSha1(reinterpret_cast<const uint8_t*>(passphrase), passLen,
                 salt, ssidLen + 4, u);
        memcpy(digest, u, sizeof(digest));
        for (unsigned iteration = 1; iteration < 4096; iteration++) {
            uint8_t next[20];
            hmacSha1(reinterpret_cast<const uint8_t*>(passphrase), passLen,
                     u, sizeof(u), next);
            memcpy(u, next, sizeof(u));
            for (size_t i = 0; i < sizeof(digest); i++) digest[i] ^= next[i];
            secureZero(next, sizeof(next));
        }
        size_t copy = (32 - produced) < sizeof(digest) ? 32 - produced : sizeof(digest);
        memcpy(pmk + produced, digest, copy);
        produced += copy;
    }
    secureZero(salt, sizeof(salt));
    secureZero(u, sizeof(u));
    secureZero(digest, sizeof(digest));
    return true;
}

void derivePtk(const uint8_t pmk[32], const uint8_t authenticator[6],
               const uint8_t supplicant[6], const uint8_t anonce[32],
               const uint8_t snonce[32], uint8_t ptk[64]) {
    static const uint8_t label[] = "Pairwise key expansion";
    uint8_t context[76];
    bool authFirst = memcmp(authenticator, supplicant, 6) < 0;
    memcpy(context, authFirst ? authenticator : supplicant, 6);
    memcpy(context + 6, authFirst ? supplicant : authenticator, 6);
    bool anonceFirst = memcmp(anonce, snonce, 32) < 0;
    memcpy(context + 12, anonceFirst ? anonce : snonce, 32);
    memcpy(context + 44, anonceFirst ? snonce : anonce, 32);

    uint8_t input[sizeof(label) + sizeof(context) + 1];
    memcpy(input, label, sizeof(label) - 1);
    input[sizeof(label) - 1] = 0;
    memcpy(input + sizeof(label), context, sizeof(context));
    size_t produced = 0;
    for (uint8_t counter = 0; produced < 64; counter++) {
        input[sizeof(label) + sizeof(context)] = counter;
        uint8_t digest[20];
        hmacSha1(pmk, 32, input, sizeof(input), digest);
        size_t copy = (64 - produced) < sizeof(digest) ? 64 - produced : sizeof(digest);
        memcpy(ptk + produced, digest, copy);
        produced += copy;
        secureZero(digest, sizeof(digest));
    }
    secureZero(context, sizeof(context));
    secureZero(input, sizeof(input));
}

bool aesKeyUnwrap(const uint8_t kek[16], const uint8_t* wrapped,
                  size_t wrappedLen, uint8_t* plain, size_t* plainLen) {
    if (!kek || !wrapped || !plain || !plainLen || wrappedLen < 24 ||
        (wrappedLen & 7) != 0 || *plainLen < wrappedLen - 8) return false;
    size_t blocks = wrappedLen / 8 - 1;
    if (blocks > 31) return false;

    uint8_t a[8];
    memcpy(a, wrapped, sizeof(a));
    memcpy(plain, wrapped + 8, wrappedLen - 8);
    aes_decrypt_ctx context;
    if (aes_decrypt_key128(kek, &context) != aes_good) return false;
    for (int round = 5; round >= 0; round--) {
        for (size_t index = blocks; index > 0; index--) {
            uint64_t t = static_cast<uint64_t>(blocks * static_cast<size_t>(round) + index);
            uint8_t input[16], output[16];
            memcpy(input, a, 8);
            for (unsigned byte = 0; byte < 8; byte++) {
                input[7 - byte] ^= static_cast<uint8_t>(t >> (byte * 8));
            }
            memcpy(input + 8, plain + (index - 1) * 8, 8);
            if (aes_decrypt(input, output, &context) != aes_good) {
                secureZero(&context, sizeof(context));
                return false;
            }
            memcpy(a, output, 8);
            memcpy(plain + (index - 1) * 8, output + 8, 8);
            secureZero(input, sizeof(input));
            secureZero(output, sizeof(output));
        }
    }
    static const uint8_t integrity[8] = { 0xa6, 0xa6, 0xa6, 0xa6, 0xa6, 0xa6, 0xa6, 0xa6 };
    bool valid = constantTimeEqual(a, integrity, sizeof(a));
    *plainLen = valid ? wrappedLen - 8 : 0;
    if (!valid) secureZero(plain, wrappedLen - 8);
    secureZero(a, sizeof(a));
    secureZero(&context, sizeof(context));
    return valid;
}

bool selfTest() {
    static const uint8_t expectedPmk[32] = {
        0xf4,0x2c,0x6f,0xc5,0x2d,0xf0,0xeb,0xef,0x9e,0xbb,0x4b,0x90,0xb3,0x8a,0x5f,0x90,
        0x2e,0x83,0xfe,0x1b,0x13,0x5a,0x70,0xe2,0x3a,0xed,0x76,0x2e,0x97,0x10,0xa1,0x2e
    };
    uint8_t pmk[32];
    if (!derivePmk("password", reinterpret_cast<const uint8_t*>("IEEE"), 4, pmk) ||
        !constantTimeEqual(pmk, expectedPmk, sizeof(pmk))) {
        secureZero(pmk, sizeof(pmk));
        return false;
    }
    secureZero(pmk, sizeof(pmk));

    static const uint8_t expectedPtk[64] = {
        0x79,0xc3,0x15,0x81,0x7d,0xa2,0xce,0x91,0x7e,0x32,0x64,0xee,0xac,0x39,0xf9,0x08,
        0xed,0xaf,0xe7,0x82,0x05,0xab,0x10,0xe5,0xfa,0x4d,0x85,0x50,0x52,0x35,0xf6,0x96,
        0xad,0x89,0x09,0x20,0x24,0xc3,0x45,0x24,0xf0,0x7b,0x21,0xe1,0xf7,0xba,0xf3,0xb4,
        0x76,0xe2,0xb3,0xf0,0x64,0xf8,0x10,0xeb,0x14,0x07,0x08,0xbe,0x96,0x8d,0x02,0xf3
    };
    uint8_t testPmk[32], authenticator[6], supplicant[6], anonce[32], snonce[32], ptk[64];
    for (size_t i = 0; i < sizeof(testPmk); i++) testPmk[i] = static_cast<uint8_t>(i);
    const uint8_t aa[6] = { 0x00,0x11,0x22,0x33,0x44,0x55 };
    const uint8_t spa[6] = { 0x66,0x77,0x88,0x99,0xaa,0xbb };
    memcpy(authenticator, aa, sizeof(aa));
    memcpy(supplicant, spa, sizeof(spa));
    for (size_t i = 0; i < 32; i++) { anonce[i] = static_cast<uint8_t>(i); snonce[i] = static_cast<uint8_t>(i + 32); }
    derivePtk(testPmk, authenticator, supplicant, anonce, snonce, ptk);
    if (!constantTimeEqual(ptk, expectedPtk, sizeof(ptk))) {
        secureZero(testPmk, sizeof(testPmk)); secureZero(ptk, sizeof(ptk));
        return false;
    }
    secureZero(testPmk, sizeof(testPmk)); secureZero(ptk, sizeof(ptk));
    secureZero(anonce, sizeof(anonce)); secureZero(snonce, sizeof(snonce));

    static const uint8_t kek[16] = {
        0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08,0x09,0x0a,0x0b,0x0c,0x0d,0x0e,0x0f
    };
    static const uint8_t wrapped[24] = {
        0x1f,0xa6,0x8b,0x0a,0x81,0x12,0xb4,0x47,0xae,0xf3,0x4b,0xd8,0xfb,0x5a,0x7b,0x82,
        0x9d,0x3e,0x86,0x23,0x71,0xd2,0xcf,0xe5
    };
    static const uint8_t expectedPlain[16] = {
        0x00,0x11,0x22,0x33,0x44,0x55,0x66,0x77,0x88,0x99,0xaa,0xbb,0xcc,0xdd,0xee,0xff
    };
    uint8_t plain[16];
    size_t plainLen = sizeof(plain);
    bool unwrap = aesKeyUnwrap(kek, wrapped, sizeof(wrapped), plain, &plainLen);
    bool valid = unwrap && plainLen == sizeof(plain) &&
                 constantTimeEqual(plain, expectedPlain, sizeof(plain));
    secureZero(plain, sizeof(plain));
    return valid;
}

} // namespace RtwWpaCrypto
