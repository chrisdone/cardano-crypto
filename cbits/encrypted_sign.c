#include <assert.h>
#include <stdint.h>
#include <string.h>

#include <ed25519.h>
#include <hmac.h>

#include "cryptonite_pbkdf2.h"

typedef uint8_t cryptonite_chacha_context[131];

extern void cryptonite_chacha_init(cryptonite_chacha_context *ctx, uint8_t nb_rounds, uint32_t keylen, const uint8_t *key, uint32_t ivlen, const uint8_t *iv);
extern void cryptonite_chacha_combine(uint8_t *dst, cryptonite_chacha_context *st, const uint8_t *src, uint32_t bytes);

void clear(void *buf, uint32_t const sz)
{
	/* FIXME - HERE we need to make sure the compiler is not going to remove the call */
	memset(buf, 0, sz);
}

#define NB_ITERATIONS 15000

static
void stretch(uint8_t *buf, uint32_t const buf_len, uint8_t const *pass, uint32_t const pass_len)
{
	const uint8_t salt[] = "encrypted wallet salt";
	assert(pass_len > 0);
	cryptonite_fastpbkdf2_hmac_sha512(pass, pass_len, salt, sizeof(salt), NB_ITERATIONS, buf, buf_len);
}

#define SYM_KEY_SIZE     32
#define SYM_NONCE_SIZE   8
#define SYM_BUF_SIZE     (SYM_KEY_SIZE+SYM_NONCE_SIZE)

#define ENCRYPTED_KEY_SIZE 32
#define PUBLIC_KEY_SIZE    32
#define CHAIN_CODE_SIZE    32

#define FULL_KEY_SIZE      (ENCRYPTED_KEY_SIZE + PUBLIC_KEY_SIZE + CHAIN_CODE_SIZE)

typedef struct {
	uint8_t ekey[ENCRYPTED_KEY_SIZE];
	uint8_t pkey[PUBLIC_KEY_SIZE];
	uint8_t cc[CHAIN_CODE_SIZE];
} encrypted_key;

static void memory_combine(uint8_t const *pass, uint32_t const pass_len, uint8_t const *source, uint8_t *dest, uint32_t sz)
{
	uint8_t buf[SYM_BUF_SIZE];
	cryptonite_chacha_context ctx;
	static uint8_t const CHACHA_NB_ROUNDS = 20;

	if (pass_len) {
		memset(&ctx, 0, sizeof(cryptonite_chacha_context));

		/* generate BUF_SIZE bytes where first KEY_SIZE bytes is the key and NONCE_SIZE remaining bytes the nonce */
		stretch(buf, SYM_BUF_SIZE, pass, pass_len);
		cryptonite_chacha_init(&ctx, CHACHA_NB_ROUNDS, SYM_KEY_SIZE, buf, SYM_NONCE_SIZE, buf + SYM_KEY_SIZE);
		clear(buf, SYM_BUF_SIZE);
		cryptonite_chacha_combine(dest, &ctx, source, sz);
		clear(&ctx, sizeof(cryptonite_chacha_context));
	} else {
		memcpy(dest, source, sz);
	}
}

static void unencrypt_start
    (uint8_t const*  pass,
     uint32_t const  pass_len,
     encrypted_key const *encrypted_key /* in */,
     ed25519_secret_key  decrypted_key /* out */)
{
	memory_combine(pass, pass_len, encrypted_key->ekey, decrypted_key, ENCRYPTED_KEY_SIZE);
}

static void unencrypt_stop(ed25519_secret_key decrypted_key)
{
	clear(decrypted_key, sizeof(ed25519_secret_key));
}

void wallet_encrypted_to_public
    (encrypted_key const *encrypted_key,
     uint8_t const *pass, uint32_t const pass_len,
     ed25519_public_key public_key)
{
	ed25519_secret_key priv_key;

	unencrypt_start(pass, pass_len, encrypted_key, priv_key);
	cardano_crypto_ed25519_publickey(priv_key, public_key);
	unencrypt_stop(priv_key);
}

void wallet_encrypted_from_secret
    (uint8_t const *pass, uint32_t const pass_len,
     ed25519_secret_key secret_key,
     const uint8_t cc[CHAIN_CODE_SIZE],
     encrypted_key *encrypted_key)
{
	ed25519_public_key pub_key;

	cardano_crypto_ed25519_publickey(secret_key, pub_key);

	memory_combine(pass, pass_len, secret_key, encrypted_key->ekey, ENCRYPTED_KEY_SIZE);
	memcpy(encrypted_key->pkey, pub_key, PUBLIC_KEY_SIZE);
	memcpy(encrypted_key->cc, cc, CHAIN_CODE_SIZE);
}

void wallet_encrypted_sign
    (encrypted_key const *encrypted_key, uint8_t const* pass, uint32_t const pass_len,
     uint8_t const *data, uint32_t const data_len,
     ed25519_signature signature)
{
	ed25519_secret_key priv_key;
	ed25519_public_key pub_key;

	unencrypt_start(pass, pass_len, encrypted_key, priv_key);
	cardano_crypto_ed25519_publickey(priv_key, pub_key);
	cardano_crypto_ed25519_sign(data, data_len, encrypted_key->cc, CHAIN_CODE_SIZE, priv_key, pub_key, signature);
	unencrypt_stop(priv_key);
}

DECL_HMAC(sha512,
          SHA512_BLOCK_SIZE,
          SHA512_DIGEST_SIZE,
          struct sha512_ctx,
          cryptonite_sha512_init,
          cryptonite_sha512_update,
          cryptonite_sha512_finalize);

void wallet_encrypted_derive_normal
    (encrypted_key const *in,
     uint8_t const *pass, uint32_t const pass_len,
     uint32_t index,
     encrypted_key *out)
{
	ed25519_secret_key priv_key;
	ed25519_secret_key res_key;
	HMAC_sha512_ctx hmac_ctx;
	uint8_t idxBuf[4];
	uint8_t hmac_out[64];

	idxBuf[0] = index >> 24;
	idxBuf[1] = index >> 16;
	idxBuf[2] = index >> 8;
	idxBuf[3] = index;

	HMAC_sha512_init(&hmac_ctx, in->cc, CHAIN_CODE_SIZE);
	HMAC_sha512_update(&hmac_ctx, "NORM", 4);
	HMAC_sha512_update(&hmac_ctx, in->pkey, PUBLIC_KEY_SIZE);
	HMAC_sha512_update(&hmac_ctx, idxBuf, 4);
	HMAC_sha512_final(&hmac_ctx, hmac_out);

	/* TODO : refactor to not have to 'stretch' twice in unencrypt and wallet_create_.._from_secret */
	unencrypt_start(pass, pass_len, in, priv_key);
	cardano_crypto_ed25519_scalar_add(priv_key, hmac_out, res_key);
	unencrypt_stop(priv_key);

	wallet_encrypted_from_secret(pass, pass_len, res_key, hmac_out + 32, out);
	clear(res_key, ENCRYPTED_KEY_SIZE);
	clear(hmac_out, 64);
}

void wallet_encrypted_derive_hardened
    (encrypted_key const *in,
     uint8_t const *pass, uint32_t const pass_len,
	 uint32_t index,
     encrypted_key *out)
{
	ed25519_secret_key priv_key;
	HMAC_sha512_ctx hmac_ctx;
	uint8_t idxBuf[4];
	uint8_t hmac_out[64];

	idxBuf[0] = index >> 24;
	idxBuf[1] = index >> 16;
	idxBuf[2] = index >> 8;
	idxBuf[3] = index;

	HMAC_sha512_init(&hmac_ctx, in->cc, CHAIN_CODE_SIZE);
	HMAC_sha512_update(&hmac_ctx, "HARD", 4);

	unencrypt_start(pass, pass_len, in, priv_key);
	HMAC_sha512_update(&hmac_ctx, priv_key, ENCRYPTED_KEY_SIZE);
	unencrypt_stop(priv_key);

	HMAC_sha512_update(&hmac_ctx, idxBuf, 4);
	HMAC_sha512_final(&hmac_ctx, hmac_out);

	wallet_encrypted_from_secret(pass, pass_len, hmac_out, hmac_out + 32, out);
	clear(hmac_out, 64);
}
