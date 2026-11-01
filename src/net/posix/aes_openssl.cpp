// AES-128-CBC decryption (see aes.h) with the system's OpenSSL, on air/OS
// where the transport uses it for TLS (tls_openssl.cpp).
#include "net/aes.h"

#include <openssl/evp.h>

namespace net {

bool aes128CbcDecrypt(
    std::string_view key, std::string_view iv, std::string_view in, std::string *out
) {
    out->clear();
    if (key.size() != 16 || iv.size() != 16 || in.empty() || in.size() % 16 != 0)
        return false;
    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (!ctx)
        return false;
    std::string plain(in.size() + 16, '\0');
    int         n = 0, tail = 0;
    // PKCS#7 padding is checked and stripped by EVP_DecryptFinal_ex.
    const bool ok =
        EVP_DecryptInit_ex(
            ctx,
            EVP_aes_128_cbc(),
            nullptr,
            reinterpret_cast<const unsigned char *>(key.data()),
            reinterpret_cast<const unsigned char *>(iv.data())
        ) == 1 &&
        EVP_DecryptUpdate(
            ctx,
            reinterpret_cast<unsigned char *>(plain.data()),
            &n,
            reinterpret_cast<const unsigned char *>(in.data()),
            int(in.size())
        ) == 1 &&
        EVP_DecryptFinal_ex(ctx, reinterpret_cast<unsigned char *>(plain.data()) + n, &tail) == 1;
    EVP_CIPHER_CTX_free(ctx);
    if (!ok)
        return false;
    plain.resize(size_t(n + tail));
    *out = std::move(plain);
    return true;
}

} // namespace net
