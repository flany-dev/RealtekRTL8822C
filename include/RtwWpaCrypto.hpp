// SPDX-License-Identifier: BSD-3-Clause
/* Copyright (c) 2026 RealtekRTL8822C contributors. */

#ifndef RTW_WPA_CRYPTO_HPP
#define RTW_WPA_CRYPTO_HPP

#include <stddef.h>
#include <stdint.h>

namespace RtwWpaCrypto {

void secureZero(void* data, size_t len);
bool constantTimeEqual(const uint8_t* a, const uint8_t* b, size_t len);
void hmacSha1(const uint8_t* key, size_t keyLen,
              const uint8_t* data, size_t dataLen, uint8_t out[20]);
bool derivePmk(const char* passphrase, const uint8_t* ssid, size_t ssidLen,
               uint8_t pmk[32]);
void derivePtk(const uint8_t pmk[32], const uint8_t authenticator[6],
               const uint8_t supplicant[6], const uint8_t anonce[32],
               const uint8_t snonce[32], uint8_t ptk[64]);
bool aesKeyUnwrap(const uint8_t kek[16], const uint8_t* wrapped,
                  size_t wrappedLen, uint8_t* plain, size_t* plainLen);
bool selfTest();

} // namespace RtwWpaCrypto

#endif
