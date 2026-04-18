#include "config.h"
/* pqcrypto.c - Post-quantum cryptographic primitives for X3
 * Copyright 2024-2026 Cathexis/X3 Development Team
 *
 * Implements hybrid X25519 + ML-KEM-768 key exchange and
 * ML-DSA-65 digital signatures using liboqs.
 */

#include "pqcrypto.h"
#include "crypto.h"
#include "log.h"

#include <string.h>
#include <stdlib.h>
#include <sodium.h>

#ifdef HAVE_LIBOQS
#include <oqs/oqs.h>
#else
/* Stubs when liboqs is not available */
#endif

static int pq_initialized = 0;

int
pqcrypto_init(void)
{
    if (pq_initialized)
        return 0;

#ifdef HAVE_LIBOQS
    OQS_init();
    pq_initialized = 1;
    log_module(MAIN_LOG, LOG_INFO, "Post-quantum crypto initialized: %s", pq_description());
    return 0;
#else
    log_module(MAIN_LOG, LOG_WARNING, "Post-quantum crypto: not compiled (liboqs unavailable)");
    return -1;
#endif
}

void
pqcrypto_cleanup(void)
{
#ifdef HAVE_LIBOQS
    if (pq_initialized) {
        OQS_destroy();
        pq_initialized = 0;
    }
#endif
}

int
pq_available(void)
{
#ifdef HAVE_LIBOQS
    return pq_initialized && OQS_KEM_alg_is_enabled(OQS_KEM_alg_kyber_768);
#else
    return 0;
#endif
}

const char *
pq_description(void)
{
#ifdef HAVE_LIBOQS
    static char desc[256];
    snprintf(desc, sizeof(desc), "ML-KEM-768 (Kyber) + X25519 hybrid KEX, ML-DSA-65 (Dilithium3) signatures [liboqs %s]",
             OQS_version());
    return desc;
#else
    return "Post-quantum crypto not available (compiled without liboqs)";
#endif
}

/* ─── Hybrid Key Exchange (X25519 + ML-KEM-768) ─── */

#ifdef HAVE_LIBOQS

int
pq_kex_init_initiator(struct pq_kex_state *state)
{
    OQS_KEM *kem;

    memset(state, 0, sizeof(*state));
    state->role = 0; /* initiator */

    /* Generate X25519 keypair */
    crypto_box_keypair(state->x25519_pk, state->x25519_sk);

    /* Generate ML-KEM-768 keypair */
    kem = OQS_KEM_new(OQS_KEM_alg_kyber_768);
    if (!kem) {
        log_module(MAIN_LOG, LOG_ERROR, "pq_kex_init: OQS_KEM_new(kyber_768) failed");
        return -1;
    }

    state->mlkem_pk_len = kem->length_public_key;
    state->mlkem_sk_len = kem->length_secret_key;
    state->mlkem_pk = malloc(state->mlkem_pk_len);
    state->mlkem_sk = malloc(state->mlkem_sk_len);

    if (!state->mlkem_pk || !state->mlkem_sk) {
        OQS_KEM_free(kem);
        pq_kex_free(state);
        return -1;
    }

    if (OQS_KEM_keypair(kem, state->mlkem_pk, state->mlkem_sk) != OQS_SUCCESS) {
        log_module(MAIN_LOG, LOG_ERROR, "pq_kex_init: OQS_KEM_keypair failed");
        OQS_KEM_free(kem);
        pq_kex_free(state);
        return -1;
    }

    OQS_KEM_free(kem);
    return 0;
}

int
pq_kex_respond(const unsigned char *peer_x25519_pk,
               const unsigned char *peer_mlkem_pk, size_t peer_mlkem_pk_len,
               unsigned char **ct_out, size_t *ct_len,
               unsigned char *x25519_pk_out,
               unsigned char *shared_out)
{
    OQS_KEM *kem;
    unsigned char x25519_sk[32], x25519_shared[32];
    unsigned char *mlkem_shared = NULL;
    unsigned char combined[64]; /* x25519_shared || mlkem_shared */

    /* Generate our X25519 keypair */
    crypto_box_keypair(x25519_pk_out, x25519_sk);

    /* X25519 key agreement */
    if (crypto_scalarmult(x25519_shared, x25519_sk, peer_x25519_pk) != 0) {
        sodium_memzero(x25519_sk, sizeof(x25519_sk));
        return -1;
    }
    sodium_memzero(x25519_sk, sizeof(x25519_sk));

    /* ML-KEM encapsulation */
    kem = OQS_KEM_new(OQS_KEM_alg_kyber_768);
    if (!kem)
        return -1;

    if (peer_mlkem_pk_len != kem->length_public_key) {
        OQS_KEM_free(kem);
        return -1;
    }

    *ct_len = kem->length_ciphertext;
    *ct_out = malloc(*ct_len);
    mlkem_shared = malloc(kem->length_shared_secret);
    if (!*ct_out || !mlkem_shared) {
        free(*ct_out);
        free(mlkem_shared);
        OQS_KEM_free(kem);
        return -1;
    }

    if (OQS_KEM_encaps(kem, *ct_out, mlkem_shared, peer_mlkem_pk) != OQS_SUCCESS) {
        free(*ct_out);
        free(mlkem_shared);
        OQS_KEM_free(kem);
        return -1;
    }

    /* Combine both shared secrets and derive final key via HKDF */
    memcpy(combined, x25519_shared, 32);
    memcpy(combined + 32, mlkem_shared, 32); /* Kyber SS is 32 bytes */
    sodium_memzero(x25519_shared, sizeof(x25519_shared));
    sodium_memzero(mlkem_shared, kem->length_shared_secret);
    free(mlkem_shared);
    OQS_KEM_free(kem);

    if (crypto_hkdf_sha256(combined, sizeof(combined),
                           (const unsigned char *)"x3-pq-s2s", 9,
                           (const unsigned char *)"hybrid-kex-v1", 13,
                           shared_out, PQ_SHARED_SECRET_LEN) != 0) {
        sodium_memzero(combined, sizeof(combined));
        return -1;
    }

    sodium_memzero(combined, sizeof(combined));
    return 0;
}

int
pq_kex_finalize(struct pq_kex_state *state,
                const unsigned char *peer_x25519_pk,
                const unsigned char *ct, size_t ct_len,
                unsigned char *shared_out)
{
    OQS_KEM *kem;
    unsigned char x25519_shared[32];
    unsigned char *mlkem_shared = NULL;
    unsigned char combined[64];

    if (state->role != 0)
        return -1; /* must be initiator */

    /* X25519 key agreement */
    if (crypto_scalarmult(x25519_shared, state->x25519_sk, peer_x25519_pk) != 0)
        return -1;

    /* ML-KEM decapsulation */
    kem = OQS_KEM_new(OQS_KEM_alg_kyber_768);
    if (!kem)
        return -1;

    if (ct_len != kem->length_ciphertext) {
        OQS_KEM_free(kem);
        return -1;
    }

    mlkem_shared = malloc(kem->length_shared_secret);
    if (!mlkem_shared) {
        OQS_KEM_free(kem);
        return -1;
    }

    if (OQS_KEM_decaps(kem, mlkem_shared, ct, state->mlkem_sk) != OQS_SUCCESS) {
        free(mlkem_shared);
        OQS_KEM_free(kem);
        return -1;
    }

    /* Combine and derive via HKDF — same as responder */
    memcpy(combined, x25519_shared, 32);
    memcpy(combined + 32, mlkem_shared, 32);
    sodium_memzero(x25519_shared, sizeof(x25519_shared));
    sodium_memzero(mlkem_shared, kem->length_shared_secret);
    free(mlkem_shared);
    OQS_KEM_free(kem);

    if (crypto_hkdf_sha256(combined, sizeof(combined),
                           (const unsigned char *)"x3-pq-s2s", 9,
                           (const unsigned char *)"hybrid-kex-v1", 13,
                           shared_out, PQ_SHARED_SECRET_LEN) != 0) {
        sodium_memzero(combined, sizeof(combined));
        return -1;
    }

    sodium_memzero(combined, sizeof(combined));
    return 0;
}

void
pq_kex_free(struct pq_kex_state *state)
{
    if (!state) return;
    sodium_memzero(state->x25519_sk, sizeof(state->x25519_sk));
    if (state->mlkem_sk) {
        sodium_memzero(state->mlkem_sk, state->mlkem_sk_len);
        free(state->mlkem_sk);
    }
    if (state->mlkem_pk)
        free(state->mlkem_pk);
    memset(state, 0, sizeof(*state));
}

/* ─── ML-DSA-65 (Dilithium3) Signatures ─── */

int
pq_sig_keygen(unsigned char **pk_out, size_t *pk_len,
              unsigned char **sk_out, size_t *sk_len)
{
    OQS_SIG *sig = OQS_SIG_new(OQS_SIG_alg_ml_dsa_65);
    if (!sig)
        return -1;

    *pk_len = sig->length_public_key;
    *sk_len = sig->length_secret_key;
    *pk_out = malloc(*pk_len);
    *sk_out = malloc(*sk_len);

    if (!*pk_out || !*sk_out) {
        free(*pk_out); free(*sk_out);
        OQS_SIG_free(sig);
        return -1;
    }

    if (OQS_SIG_keypair(sig, *pk_out, *sk_out) != OQS_SUCCESS) {
        free(*pk_out); free(*sk_out);
        OQS_SIG_free(sig);
        return -1;
    }

    OQS_SIG_free(sig);
    return 0;
}

int
pq_sign(const unsigned char *sk, size_t sk_len,
        const unsigned char *msg, size_t msg_len,
        unsigned char **sig_out, size_t *sig_len)
{
    OQS_SIG *sig = OQS_SIG_new(OQS_SIG_alg_ml_dsa_65);
    if (!sig)
        return -1;

    if (sk_len != sig->length_secret_key) {
        OQS_SIG_free(sig);
        return -1;
    }

    *sig_out = malloc(sig->length_signature);
    if (!*sig_out) {
        OQS_SIG_free(sig);
        return -1;
    }

    if (OQS_SIG_sign(sig, *sig_out, sig_len, msg, msg_len, sk) != OQS_SUCCESS) {
        free(*sig_out);
        OQS_SIG_free(sig);
        return -1;
    }

    OQS_SIG_free(sig);
    return 0;
}

int
pq_verify(const unsigned char *pk, size_t pk_len,
          const unsigned char *msg, size_t msg_len,
          const unsigned char *sig_data, size_t sig_len)
{
    OQS_SIG *sig = OQS_SIG_new(OQS_SIG_alg_ml_dsa_65);
    OQS_STATUS result;
    if (!sig)
        return -1;

    if (pk_len != sig->length_public_key) {
        OQS_SIG_free(sig);
        return -1;
    }

    result = OQS_SIG_verify(sig, msg, msg_len, sig_data, sig_len, pk);
    OQS_SIG_free(sig);
    return (result == OQS_SUCCESS) ? 1 : 0;
}

/* ─── S2S Session Key Derivation ─── */

int
pq_s2s_derive_keys(struct pq_s2s_session *session,
                   const unsigned char *shared_secret,
                   int is_initiator)
{
    unsigned char keys[64]; /* 32 send + 32 recv */

    memset(session, 0, sizeof(*session));

    if (crypto_hkdf_sha256(shared_secret, PQ_SHARED_SECRET_LEN,
                           (const unsigned char *)"x3-pq-session", 13,
                           (const unsigned char *)"s2s-hmac-keys-v1", 16,
                           keys, sizeof(keys)) != 0)
        return -1;

    /* Initiator uses first 32 for send, responder uses first 32 for recv */
    if (is_initiator) {
        memcpy(session->send_key, keys, 32);
        memcpy(session->recv_key, keys + 32, 32);
    } else {
        memcpy(session->recv_key, keys, 32);
        memcpy(session->send_key, keys + 32, 32);
    }

    sodium_memzero(keys, sizeof(keys));
    session->send_seq = 0;
    session->recv_seq = 0;
    session->established = 1;
    return 0;
}

int
pq_s2s_sign_message(struct pq_s2s_session *session,
                    const char *message,
                    unsigned char *hmac_out)
{
    unsigned char buf[8 + 512]; /* seq + message */
    size_t msg_len = strlen(message);
    size_t total;

    if (!session->established)
        return -1;

    /* Include sequence number to prevent replay */
    for (int i = 0; i < 8; i++)
        buf[i] = (session->send_seq >> (56 - 8*i)) & 0xff;
    total = 8;
    if (msg_len > sizeof(buf) - 8)
        msg_len = sizeof(buf) - 8;
    memcpy(buf + 8, message, msg_len);
    total += msg_len;

    if (crypto_hmac_sha256(session->send_key, 32, buf, total, hmac_out) != 0)
        return -1;

    session->send_seq++;
    return 0;
}

int
pq_s2s_verify_message(struct pq_s2s_session *session,
                      const char *message,
                      const unsigned char *hmac)
{
    unsigned char expected[32];
    unsigned char buf[8 + 512];
    size_t msg_len = strlen(message);
    size_t total;

    if (!session->established)
        return -1;

    for (int i = 0; i < 8; i++)
        buf[i] = (session->recv_seq >> (56 - 8*i)) & 0xff;
    total = 8;
    if (msg_len > sizeof(buf) - 8)
        msg_len = sizeof(buf) - 8;
    memcpy(buf + 8, message, msg_len);
    total += msg_len;

    if (crypto_hmac_sha256(session->recv_key, 32, buf, total, expected) != 0)
        return -1;

    if (crypto_ct_memcmp(expected, hmac, 32) != 0)
        return 0; /* mismatch */

    session->recv_seq++;
    return 1; /* valid */
}

#else /* !HAVE_LIBOQS — stub implementations */

int pq_kex_init_initiator(struct pq_kex_state *state) { (void)state; return -1; }
int pq_kex_respond(const unsigned char *a, const unsigned char *b, size_t c,
                   unsigned char **d, size_t *e, unsigned char *f, unsigned char *g)
{ (void)a;(void)b;(void)c;(void)d;(void)e;(void)f;(void)g; return -1; }
int pq_kex_finalize(struct pq_kex_state *s, const unsigned char *a, const unsigned char *b,
                    size_t c, unsigned char *d) { (void)s;(void)a;(void)b;(void)c;(void)d; return -1; }
void pq_kex_free(struct pq_kex_state *s) { if(s) memset(s,0,sizeof(*s)); }

int pq_sig_keygen(unsigned char **a, size_t *b, unsigned char **c, size_t *d)
{ (void)a;(void)b;(void)c;(void)d; return -1; }
int pq_sign(const unsigned char *a, size_t b, const unsigned char *c, size_t d,
            unsigned char **e, size_t *f) { (void)a;(void)b;(void)c;(void)d;(void)e;(void)f; return -1; }
int pq_verify(const unsigned char *a, size_t b, const unsigned char *c, size_t d,
              const unsigned char *e, size_t f) { (void)a;(void)b;(void)c;(void)d;(void)e;(void)f; return -1; }

int pq_s2s_derive_keys(struct pq_s2s_session *s, const unsigned char *a, int b)
{ (void)s;(void)a;(void)b; return -1; }
int pq_s2s_sign_message(struct pq_s2s_session *s, const char *a, unsigned char *b)
{ (void)s;(void)a;(void)b; return -1; }
int pq_s2s_verify_message(struct pq_s2s_session *s, const char *a, const unsigned char *b)
{ (void)s;(void)a;(void)b; return -1; }

#endif /* HAVE_LIBOQS */
