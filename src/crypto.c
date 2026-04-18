#include "config.h"
/* crypto.c - Modern cryptographic primitives for X3
 * Copyright 2024-2026 Cathexis/X3 Development Team
 *
 * Dependencies: libsodium (random, ct_memcmp), libargon2 (passwords),
 *               OpenSSL 3.x (HMAC, SHA, HKDF)
 */

#include "crypto.h"
#include "log.h"

#include <string.h>
#include <stdlib.h>
#include <sodium.h>
#include <argon2.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/kdf.h>
#include <openssl/rand.h>

/* Legacy MD5 check/crypt for migration - forward declare from md5.c */
extern int checkpass(const char *pass, const char *crypted);

/* ─── Initialization ─── */

static int crypto_initialized = 0;

int
crypto_init(void)
{
    if (crypto_initialized)
        return 0;

    if (sodium_init() < 0) {
        log_module(MAIN_LOG, LOG_ERROR, "crypto_init: sodium_init() failed");
        return -1;
    }

    crypto_initialized = 1;
    log_module(MAIN_LOG, LOG_INFO, "Crypto subsystem initialized (Argon2id + HMAC-SHA256 + libsodium)");
    return 0;
}

void
crypto_cleanup(void)
{
    crypto_initialized = 0;
}

/* ─── Secure Random ─── */

void
crypto_random_bytes(unsigned char *buf, size_t len)
{
    randombytes_buf(buf, len);
}

uint32_t
crypto_random_uint32(void)
{
    return randombytes_random();
}

/* ─── Constant-Time Operations ─── */

int
crypto_ct_memcmp(const void *a, const void *b, size_t len)
{
    return sodium_memcmp(a, b, len);
}

/* ─── SHA-256 / SHA-512 ─── */

int
crypto_sha256(const unsigned char *data, size_t len, unsigned char *out)
{
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    unsigned int md_len;
    int ret = -1;

    if (!ctx)
        return -1;

    if (EVP_DigestInit_ex(ctx, EVP_sha256(), NULL) == 1 &&
        EVP_DigestUpdate(ctx, data, len) == 1 &&
        EVP_DigestFinal_ex(ctx, out, &md_len) == 1)
        ret = 0;

    EVP_MD_CTX_free(ctx);
    return ret;
}

int
crypto_sha512(const unsigned char *data, size_t len, unsigned char *out)
{
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    unsigned int md_len;
    int ret = -1;

    if (!ctx)
        return -1;

    if (EVP_DigestInit_ex(ctx, EVP_sha512(), NULL) == 1 &&
        EVP_DigestUpdate(ctx, data, len) == 1 &&
        EVP_DigestFinal_ex(ctx, out, &md_len) == 1)
        ret = 0;

    EVP_MD_CTX_free(ctx);
    return ret;
}

/* ─── HMAC-SHA256 ─── */

int
crypto_hmac_sha256(const unsigned char *key, size_t key_len,
                   const unsigned char *msg, size_t msg_len,
                   unsigned char *out)
{
    unsigned int out_len = CRYPTO_HMAC_LEN;
    unsigned char *result;

    result = HMAC(EVP_sha256(), key, (int)key_len, msg, msg_len, out, &out_len);
    return result ? 0 : -1;
}

/* ─── Base64 ─── */

static const char b64_table[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

size_t
crypto_b64_encode(const unsigned char *in, size_t len, char *out)
{
    size_t i, o = 0;

    for (i = 0; i + 2 < len; i += 3) {
        out[o++] = b64_table[(in[i] >> 2) & 0x3f];
        out[o++] = b64_table[((in[i] & 0x3) << 4) | ((in[i+1] >> 4) & 0xf)];
        out[o++] = b64_table[((in[i+1] & 0xf) << 2) | ((in[i+2] >> 6) & 0x3)];
        out[o++] = b64_table[in[i+2] & 0x3f];
    }
    if (i < len) {
        out[o++] = b64_table[(in[i] >> 2) & 0x3f];
        if (i + 1 < len) {
            out[o++] = b64_table[((in[i] & 0x3) << 4) | ((in[i+1] >> 4) & 0xf)];
            out[o++] = b64_table[((in[i+1] & 0xf) << 2)];
        } else {
            out[o++] = b64_table[((in[i] & 0x3) << 4)];
            out[o++] = '=';
        }
        out[o++] = '=';
    }
    out[o] = '\0';
    return o;
}

static int
b64_val(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

size_t
crypto_b64_decode(const char *in, unsigned char *out, size_t out_len)
{
    size_t i, o = 0, len = strlen(in);
    int vals[4];

    for (i = 0; i + 3 < len && o < out_len; i += 4) {
        vals[0] = b64_val(in[i]);
        vals[1] = b64_val(in[i+1]);
        if (vals[0] < 0 || vals[1] < 0)
            return (size_t)-1;
        out[o++] = (vals[0] << 2) | (vals[1] >> 4);
        if (in[i+2] == '=') break;
        vals[2] = b64_val(in[i+2]);
        if (vals[2] < 0) return (size_t)-1;
        if (o < out_len) out[o++] = ((vals[1] & 0xf) << 4) | (vals[2] >> 2);
        if (in[i+3] == '=') break;
        vals[3] = b64_val(in[i+3]);
        if (vals[3] < 0) return (size_t)-1;
        if (o < out_len) out[o++] = ((vals[2] & 0x3) << 6) | vals[3];
    }
    return o;
}

/* ─── HKDF-SHA256 ─── */

int
crypto_hkdf_sha256(const unsigned char *ikm, size_t ikm_len,
                   const unsigned char *salt, size_t salt_len,
                   const unsigned char *info, size_t info_len,
                   unsigned char *out, size_t out_len)
{
    EVP_PKEY_CTX *pctx;
    int ret = -1;

    pctx = EVP_PKEY_CTX_new_id(EVP_PKEY_HKDF, NULL);
    if (!pctx)
        return -1;

    if (EVP_PKEY_derive_init(pctx) <= 0)
        goto done;
    if (EVP_PKEY_CTX_set_hkdf_md(pctx, EVP_sha256()) <= 0)
        goto done;
    if (EVP_PKEY_CTX_set1_hkdf_salt(pctx, salt, (int)salt_len) <= 0)
        goto done;
    if (EVP_PKEY_CTX_set1_hkdf_key(pctx, ikm, (int)ikm_len) <= 0)
        goto done;
    if (EVP_PKEY_CTX_add1_hkdf_info(pctx, info, (int)info_len) <= 0)
        goto done;
    if (EVP_PKEY_derive(pctx, out, &out_len) <= 0)
        goto done;

    ret = 0;
done:
    EVP_PKEY_CTX_free(pctx);
    return ret;
}

/* ─── Password Hashing (Argon2id) ─── */

int
crypto_hash_password(const char *password, char *encoded, size_t encoded_len)
{
    unsigned char salt[CRYPTO_ARGON2_SALTLEN];
    int ret;

    crypto_random_bytes(salt, sizeof(salt));

    ret = argon2id_hash_encoded(
        CRYPTO_ARGON2_TIMECOST,
        CRYPTO_ARGON2_MEMCOST,
        CRYPTO_ARGON2_PARALLEL,
        password, strlen(password),
        salt, sizeof(salt),
        CRYPTO_ARGON2_HASHLEN,
        encoded, encoded_len
    );

    if (ret != ARGON2_OK) {
        log_module(MAIN_LOG, LOG_ERROR, "crypto_hash_password: argon2id_hash_encoded failed: %s",
                   argon2_error_message(ret));
        return -1;
    }
    return 0;
}

int
crypto_is_legacy_hash(const char *encoded)
{
    /* Legacy X3 MD5 hashes are NOT prefixed with $argon2 */
    if (!encoded || !*encoded)
        return 0;
    if (strncmp(encoded, "$argon2", 7) == 0)
        return 0; /* It's a modern hash */
    return 1; /* Legacy format */
}

int
crypto_verify_password(const char *password, const char *encoded)
{
    int ret;

    if (!password || !encoded)
        return -1;

    /* Modern Argon2id hash */
    if (strncmp(encoded, "$argon2", 7) == 0) {
        ret = argon2id_verify(encoded, password, strlen(password));
        if (ret == ARGON2_OK)
            return 1; /* match */
        if (ret == ARGON2_VERIFY_MISMATCH)
            return 0; /* no match */
        return -1;    /* error */
    }

    /* Legacy MD5 hash — delegate to old checkpass() for migration */
    return checkpass(password, encoded) ? 1 : 0;
}

/* ─── AES-256-GCM Database Encryption ─── */

int
crypto_db_derive_key(const char *passphrase, unsigned char *key_out)
{
    if (!passphrase || !*passphrase)
        return -1;
    return crypto_hmac_sha256(
        (const unsigned char *)passphrase, strlen(passphrase),
        (const unsigned char *)"cathexis-saxdb-encrypt-v1", 24,
        key_out);
}

int
crypto_db_is_encrypted(const char *path)
{
    FILE *f;
    char magic[4];
    f = fopen(path, "rb");
    if (!f) return 0;
    if (fread(magic, 1, 4, f) != 4) { fclose(f); return 0; }
    fclose(f);
    return memcmp(magic, CRYPTO_DB_MAGIC, 4) == 0;
}

int
crypto_db_encrypt_file(const char *src_path, const char *dst_path,
                       const unsigned char *key)
{
    FILE *fin, *fout;
    EVP_CIPHER_CTX *ctx = NULL;
    unsigned char iv[CRYPTO_DB_IV_LEN];
    unsigned char tag[CRYPTO_DB_TAG_LEN];
    unsigned char *plaintext = NULL;
    unsigned char *ciphertext = NULL;
    long file_size;
    int outlen, tmplen;
    uint32_t version = CRYPTO_DB_VERSION;
    int ret = -1;

    fin = fopen(src_path, "rb");
    if (!fin) return -1;

    fseek(fin, 0, SEEK_END);
    file_size = ftell(fin);
    fseek(fin, 0, SEEK_SET);

    plaintext = malloc(file_size);
    if (!plaintext) { fclose(fin); return -1; }
    if ((long)fread(plaintext, 1, file_size, fin) != file_size) {
        free(plaintext); fclose(fin); return -1;
    }
    fclose(fin);

    ciphertext = malloc(file_size + 16);
    if (!ciphertext) { free(plaintext); return -1; }

    /* Generate random IV */
    if (RAND_bytes(iv, CRYPTO_DB_IV_LEN) != 1) goto cleanup;

    ctx = EVP_CIPHER_CTX_new();
    if (!ctx) goto cleanup;
    if (EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), NULL, NULL, NULL) != 1) goto cleanup;
    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, CRYPTO_DB_IV_LEN, NULL) != 1) goto cleanup;
    if (EVP_EncryptInit_ex(ctx, NULL, NULL, key, iv) != 1) goto cleanup;
    if (EVP_EncryptUpdate(ctx, ciphertext, &outlen, plaintext, (int)file_size) != 1) goto cleanup;
    if (EVP_EncryptFinal_ex(ctx, ciphertext + outlen, &tmplen) != 1) goto cleanup;
    outlen += tmplen;
    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, CRYPTO_DB_TAG_LEN, tag) != 1) goto cleanup;

    /* Write: magic + version + iv + ciphertext + tag */
    fout = fopen(dst_path, "wb");
    if (!fout) goto cleanup;
    fwrite(CRYPTO_DB_MAGIC, 1, 4, fout);
    fwrite(&version, 1, 4, fout);
    fwrite(iv, 1, CRYPTO_DB_IV_LEN, fout);
    fwrite(ciphertext, 1, outlen, fout);
    fwrite(tag, 1, CRYPTO_DB_TAG_LEN, fout);
    fclose(fout);
    ret = 0;

cleanup:
    if (ctx) EVP_CIPHER_CTX_free(ctx);
    if (plaintext) { OPENSSL_cleanse(plaintext, file_size); free(plaintext); }
    if (ciphertext) free(ciphertext);
    return ret;
}

int
crypto_db_decrypt_file(const char *src_path, const char *dst_path,
                       const unsigned char *key)
{
    FILE *fin, *fout;
    EVP_CIPHER_CTX *ctx = NULL;
    unsigned char header[CRYPTO_DB_HDR_LEN];
    unsigned char iv[CRYPTO_DB_IV_LEN];
    unsigned char tag[CRYPTO_DB_TAG_LEN];
    unsigned char *ciphertext = NULL;
    unsigned char *plaintext = NULL;
    long file_size, ct_size;
    int outlen, tmplen;
    int ret = -1;

    fin = fopen(src_path, "rb");
    if (!fin) return -1;

    fseek(fin, 0, SEEK_END);
    file_size = ftell(fin);
    fseek(fin, 0, SEEK_SET);

    /* Minimum: header(8) + iv(12) + tag(16) = 36 bytes */
    if (file_size < 36) { fclose(fin); return -1; }

    if (fread(header, 1, CRYPTO_DB_HDR_LEN, fin) != CRYPTO_DB_HDR_LEN) { fclose(fin); return -1; }
    if (memcmp(header, CRYPTO_DB_MAGIC, 4) != 0) { fclose(fin); return -1; }

    if (fread(iv, 1, CRYPTO_DB_IV_LEN, fin) != CRYPTO_DB_IV_LEN) { fclose(fin); return -1; }

    ct_size = file_size - CRYPTO_DB_HDR_LEN - CRYPTO_DB_IV_LEN - CRYPTO_DB_TAG_LEN;
    if (ct_size < 0) { fclose(fin); return -1; }

    ciphertext = malloc(ct_size);
    plaintext = malloc(ct_size + 1);
    if (!ciphertext || !plaintext) { fclose(fin); goto cleanup; }

    if ((long)fread(ciphertext, 1, ct_size, fin) != ct_size) { fclose(fin); goto cleanup; }
    if (fread(tag, 1, CRYPTO_DB_TAG_LEN, fin) != CRYPTO_DB_TAG_LEN) { fclose(fin); goto cleanup; }
    fclose(fin); fin = NULL;

    ctx = EVP_CIPHER_CTX_new();
    if (!ctx) goto cleanup;
    if (EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), NULL, NULL, NULL) != 1) goto cleanup;
    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, CRYPTO_DB_IV_LEN, NULL) != 1) goto cleanup;
    if (EVP_DecryptInit_ex(ctx, NULL, NULL, key, iv) != 1) goto cleanup;
    if (EVP_DecryptUpdate(ctx, plaintext, &outlen, ciphertext, (int)ct_size) != 1) goto cleanup;
    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, CRYPTO_DB_TAG_LEN, tag) != 1) goto cleanup;
    if (EVP_DecryptFinal_ex(ctx, plaintext + outlen, &tmplen) != 1) { ret = -2; goto cleanup; }
    outlen += tmplen;

    fout = fopen(dst_path, "wb");
    if (!fout) goto cleanup;
    fwrite(plaintext, 1, outlen, fout);
    fclose(fout);
    ret = 0;

cleanup:
    if (ctx) EVP_CIPHER_CTX_free(ctx);
    if (fin) fclose(fin);
    if (plaintext) { OPENSSL_cleanse(plaintext, ct_size + 1); free(plaintext); }
    if (ciphertext) free(ciphertext);
    return ret;
}
