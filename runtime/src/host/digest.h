#pragma once

// MD5 and SHA-256: OpenSSL, or Windows CNG on Windows.

#include <cstddef>
#include <cstdint>

#ifdef _WIN32
#include <windows.h>
#include <bcrypt.h>
#else
#include <openssl/evp.h>
#endif

namespace cafe::host {

#ifdef _WIN32
inline void digest(LPCWSTR algorithm, const void* data, size_t size, uint8_t* out, ULONG out_size) {
    BCRYPT_ALG_HANDLE provider = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    BCryptOpenAlgorithmProvider(&provider, algorithm, nullptr, 0);
    BCryptCreateHash(provider, &hash, nullptr, 0, nullptr, 0, 0);
    BCryptHashData(hash, static_cast<PUCHAR>(const_cast<void*>(data)), static_cast<ULONG>(size), 0);
    BCryptFinishHash(hash, out, out_size, 0);
    BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(provider, 0);
}
inline void md5(const void* data, size_t size, uint8_t out[16]) { digest(BCRYPT_MD5_ALGORITHM, data, size, out, 16); }
inline void sha256(const void* data, size_t size, uint8_t out[32]) {
    digest(BCRYPT_SHA256_ALGORITHM, data, size, out, 32);
}
#else
inline void md5(const void* data, size_t size, uint8_t out[16]) {
    unsigned int length = 0;
    EVP_Digest(data, size, out, &length, EVP_md5(), nullptr);
}
inline void sha256(const void* data, size_t size, uint8_t out[32]) {
    unsigned int length = 0;
    EVP_Digest(data, size, out, &length, EVP_sha256(), nullptr);
}
#endif

} // namespace cafe::host
