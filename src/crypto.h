/* crypto.h - Modern cryptographic primitives for X3
 * Copyright 2024-2026 Cathexis/X3 Development Team
 *
 * Replaces legacy MD5-based password hashing with Argon2id.
 * Provides HMAC-SHA256, secure random, and constant-time operations.
 *
 * Dependencies: libsodium, libargon2, OpenSSL 3.x
 */

#ifndef X3_CRYPTO_H
#define X3_CRYPTO_H

#include <stddef.h>
#include <stdint.h>
#include <time.h>

/* ─── Password Hashing (Argon2id) ─── */

/* Argon2id parameters - OWASP 2024 recommendations */
#define CRYPTO_ARGON2_TIMECOST   3        /* iterations */
#define CRYPTO_ARGON2_MEMCOST    65536    /* 64 MiB */
#define CRYPTO_ARGON2_PARALLEL   1        /* threads */
#define CRYPTO_ARGON2_HASHLEN    32       /* output bytes */
#define CRYPTO_ARGON2_SALTLEN    16       /* salt bytes */
#define CRYPTO_ARGON2_ENCODED_LEN 256     /* max encoded string length */

/* Hash a password with Argon2id.  Returns an encoded string like:
 *   $argon2id$v=19$m=65536,t=3,p=1$<salt>$<hash>
 * Caller provides buffer of at least CRYPTO_ARGON2_ENCODED_LEN bytes.
 * Returns 0 on success, -1 on failure. */
int crypto_hash_password(const char *password, char *encoded, size_t encoded_len);

/* Verify a password against an Argon2id encoded string.
 * Returns 1 if password matches, 0 if not, -1 on error.
 * Also accepts legacy MD5 hashes for migration (auto-detected by prefix). */
int crypto_verify_password(const char *password, const char *encoded);

/* Check if an encoded hash is a legacy MD5 format that should be upgraded. */
int crypto_is_legacy_hash(const char *encoded);

/* ─── HMAC-SHA256 ─── */

#define CRYPTO_HMAC_LEN     32    /* HMAC-SHA256 output length */
#define CRYPTO_HMAC_B64_LEN 44    /* base64-encoded HMAC length */

/* Compute HMAC-SHA256.  key_len and msg_len in bytes.
 * out must be at least CRYPTO_HMAC_LEN bytes.
 * Returns 0 on success. */
int crypto_hmac_sha256(const unsigned char *key, size_t key_len,
                       const unsigned char *msg, size_t msg_len,
                       unsigned char *out);

/* ─── SHA-256 / SHA-512 ─── */

#define CRYPTO_SHA256_LEN  32
#define CRYPTO_SHA512_LEN  64

int crypto_sha256(const unsigned char *data, size_t len, unsigned char *out);
int crypto_sha512(const unsigned char *data, size_t len, unsigned char *out);

/* ─── Secure Random ─── */

/* Fill buffer with cryptographically secure random bytes. */
void crypto_random_bytes(unsigned char *buf, size_t len);

/* Generate a random 32-bit integer. */
uint32_t crypto_random_uint32(void);

/* ─── Constant-Time Operations ─── */

/* Constant-time memory comparison.  Returns 0 if equal. */
int crypto_ct_memcmp(const void *a, const void *b, size_t len);

/* ─── Base64 ─── */

/* Standard base64 encode/decode.
 * crypto_b64_encode: out must be at least ((len+2)/3)*4 + 1 bytes.
 * Returns length of encoded string (not counting NUL). */
size_t crypto_b64_encode(const unsigned char *in, size_t len, char *out);

/* crypto_b64_decode: out must be at least (len*3)/4 bytes.
 * Returns number of decoded bytes, or (size_t)-1 on error. */
size_t crypto_b64_decode(const char *in, unsigned char *out, size_t out_len);

/* ─── Key Derivation ─── */

#define CRYPTO_KDF_KEYLEN  32  /* derived key length */

/* HKDF-SHA256 key derivation.
 * Derives a key from input keying material (ikm), optional salt, and info string.
 * out must be at least out_len bytes. */
int crypto_hkdf_sha256(const unsigned char *ikm, size_t ikm_len,
                       const unsigned char *salt, size_t salt_len,
                       const unsigned char *info, size_t info_len,
                       unsigned char *out, size_t out_len);

/* ─── AES-256-GCM Database Encryption ─── */

#define CRYPTO_DB_MAGIC     "SX3E"
#define CRYPTO_DB_VERSION   1
#define CRYPTO_DB_IV_LEN    12
#define CRYPTO_DB_TAG_LEN   16
#define CRYPTO_DB_KEY_LEN   32
#define CRYPTO_DB_HDR_LEN   8   /* magic + version */

/* Derive a 32-byte AES key from a passphrase.
 * key = HMAC-SHA256(passphrase, "cathexis-saxdb-encrypt-v1") */
int crypto_db_derive_key(const char *passphrase, unsigned char *key_out);

/* Encrypt a file in-place with AES-256-GCM.
 * Reads plaintext from src_path, writes encrypted data to dst_path.
 * Returns 0 on success. */
int crypto_db_encrypt_file(const char *src_path, const char *dst_path,
                           const unsigned char *key);

/* Decrypt a file with AES-256-GCM.
 * Reads encrypted data from src_path, writes plaintext to dst_path.
 * Returns 0 on success, -1 on error, -2 on auth failure. */
int crypto_db_decrypt_file(const char *src_path, const char *dst_path,
                           const unsigned char *key);

/* Check if a file is encrypted (has SX3E magic header). */
int crypto_db_is_encrypted(const char *path);

/* ─── Initialization ─── */

/* Initialize the crypto subsystem.  Call once at startup.
 * Returns 0 on success. */
int crypto_init(void);

/* Clean up the crypto subsystem.  Call at shutdown. */
void crypto_cleanup(void);

#endif /* X3_CRYPTO_H */
