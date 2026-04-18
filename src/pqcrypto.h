/* pqcrypto.h - Post-quantum cryptographic primitives for X3
 * Copyright 2024-2026 Cathexis/X3 Development Team
 *
 * Provides hybrid classical+post-quantum key exchange for S2S links.
 * Uses X25519 + ML-KEM-768 (Kyber) for key encapsulation.
 * Uses ML-DSA-65 (Dilithium3) for digital signatures (optional).
 *
 * Dependencies: liboqs, libsodium
 */

#ifndef X3_PQCRYPTO_H
#define X3_PQCRYPTO_H

#include <stddef.h>
#include <stdint.h>

/* ─── Hybrid Key Exchange (X25519 + ML-KEM-768) ─── */

/* Combined shared secret length: 32 (X25519) + 32 (ML-KEM derived) = 64 → HKDF to 32 */
#define PQ_SHARED_SECRET_LEN    32

/* Key exchange state for one side of the handshake */
struct pq_kex_state {
    unsigned char x25519_pk[32];        /* X25519 public key */
    unsigned char x25519_sk[32];        /* X25519 secret key */
    unsigned char *mlkem_pk;            /* ML-KEM-768 public key (heap allocated) */
    unsigned char *mlkem_sk;            /* ML-KEM-768 secret key (heap allocated) */
    size_t mlkem_pk_len;
    size_t mlkem_sk_len;
    int role;                           /* 0=initiator, 1=responder */
};

/* Initialize key exchange as initiator.
 * Generates X25519 + ML-KEM keypairs.
 * The public keys must be sent to the peer.
 * Returns 0 on success. */
int pq_kex_init_initiator(struct pq_kex_state *state);

/* Initialize key exchange as responder.
 * Receives the initiator's public keys, generates a ciphertext,
 * and derives the shared secret.
 * peer_x25519_pk: 32 bytes, peer_mlkem_pk: variable length.
 * ct_out: receives ML-KEM ciphertext (caller must free).
 * x25519_pk_out: receives our X25519 public key (32 bytes).
 * shared_out: receives 32-byte shared secret.
 * Returns 0 on success. */
int pq_kex_respond(const unsigned char *peer_x25519_pk,
                   const unsigned char *peer_mlkem_pk, size_t peer_mlkem_pk_len,
                   unsigned char **ct_out, size_t *ct_len,
                   unsigned char *x25519_pk_out,
                   unsigned char *shared_out);

/* Finalize key exchange as initiator.
 * Receives responder's X25519 public key and ML-KEM ciphertext.
 * Decapsulates and derives the same shared secret.
 * Returns 0 on success. */
int pq_kex_finalize(struct pq_kex_state *state,
                    const unsigned char *peer_x25519_pk,
                    const unsigned char *ct, size_t ct_len,
                    unsigned char *shared_out);

/* Free key exchange state. Securely zeroes secret material. */
void pq_kex_free(struct pq_kex_state *state);

/* ─── ML-DSA-65 (Dilithium3) Signatures ─── */

/* Sign a message.  sig_out is allocated by the function (caller must free).
 * Returns 0 on success. */
int pq_sign(const unsigned char *sk, size_t sk_len,
            const unsigned char *msg, size_t msg_len,
            unsigned char **sig_out, size_t *sig_len);

/* Verify a signature.  Returns 1 if valid, 0 if invalid, -1 on error. */
int pq_verify(const unsigned char *pk, size_t pk_len,
              const unsigned char *msg, size_t msg_len,
              const unsigned char *sig, size_t sig_len);

/* Generate a Dilithium3 keypair.
 * pk_out and sk_out are allocated by the function (caller must free).
 * Returns 0 on success. */
int pq_sig_keygen(unsigned char **pk_out, size_t *pk_len,
                  unsigned char **sk_out, size_t *sk_len);

/* ─── S2S Link Security ─── */

/* Negotiate a post-quantum secured S2S link.
 * This is called after the classical PASS/SERVER handshake succeeds.
 * It performs a hybrid key exchange over the existing link and
 * derives encryption/MAC keys for the session.
 *
 * The protocol is:
 *   1. Initiator sends:  PQKEX <x25519_pk_b64> <mlkem_pk_b64>
 *   2. Responder sends:  PQKEX <x25519_pk_b64> <mlkem_ct_b64>
 *   3. Both derive shared secret → HKDF → session keys
 *
 * Returns 0 on success, fills key_out with PQ_SHARED_SECRET_LEN bytes.
 */

struct pq_s2s_session {
    unsigned char send_key[32];     /* key for outgoing HMAC */
    unsigned char recv_key[32];     /* key for incoming HMAC */
    uint64_t send_seq;              /* outgoing sequence number */
    uint64_t recv_seq;              /* incoming sequence number */
    int established;                /* 1 if PQ session is active */
};

/* Initialize S2S PQ session from shared secret. */
int pq_s2s_derive_keys(struct pq_s2s_session *session,
                       const unsigned char *shared_secret,
                       int is_initiator);

/* Compute HMAC for an outgoing S2S message. */
int pq_s2s_sign_message(struct pq_s2s_session *session,
                        const char *message,
                        unsigned char *hmac_out);

/* Verify HMAC on an incoming S2S message. */
int pq_s2s_verify_message(struct pq_s2s_session *session,
                          const char *message,
                          const unsigned char *hmac);

/* ─── Initialization ─── */

int pqcrypto_init(void);
void pqcrypto_cleanup(void);

/* ─── Feature query ─── */

/* Returns 1 if post-quantum support is compiled in and available. */
int pq_available(void);

/* Returns human-readable description of PQ algorithms in use. */
const char *pq_description(void);

#endif /* X3_PQCRYPTO_H */
