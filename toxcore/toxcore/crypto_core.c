/*
 * Functions for the core crypto.
 *
 * NOTE: This code has to be perfect. We don't mess around with encryption.
 */

/*
 * Copyright � 2016-2017 The TokTok team.
 * Copyright � 2013 Tox project.
 *
 * This file is part of Tox, the free peer to peer instant messenger.
 *
 * Tox is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Tox is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Tox.  If not, see <http://www.gnu.org/licenses/>.
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "ccompat.h"
#include "crypto_core.h"

#include <string.h>

#ifndef VANILLA_NACL
/* We use libsodium by default. */
#include <sodium.h>
#else
#include <crypto_box.h>
#include <crypto_hash_sha256.h>
#include <crypto_hash_sha512.h>
#include <crypto_scalarmult_curve25519.h>
#include <crypto_verify_16.h>
#include <crypto_verify_32.h>
#include <randombytes.h>
#define crypto_box_MACBYTES (crypto_box_ZEROBYTES - crypto_box_BOXZEROBYTES)
#endif

#if CRYPTO_PUBLIC_KEY_SIZE != crypto_box_PUBLICKEYBYTES
#error CRYPTO_PUBLIC_KEY_SIZE should be equal to crypto_box_PUBLICKEYBYTES
#endif

#if CRYPTO_SECRET_KEY_SIZE != crypto_box_SECRETKEYBYTES
#error CRYPTO_SECRET_KEY_SIZE should be equal to crypto_box_SECRETKEYBYTES
#endif

#if CRYPTO_SHARED_KEY_SIZE != crypto_box_BEFORENMBYTES
#error CRYPTO_SHARED_KEY_SIZE should be equal to crypto_box_BEFORENMBYTES
#endif

#if CRYPTO_SYMMETRIC_KEY_SIZE != crypto_box_BEFORENMBYTES
#error CRYPTO_SYMMETRIC_KEY_SIZE should be equal to crypto_box_BEFORENMBYTES
#endif

#if CRYPTO_MAC_SIZE != crypto_box_MACBYTES
#error CRYPTO_MAC_SIZE should be equal to crypto_box_MACBYTES
#endif

#if CRYPTO_NONCE_SIZE != crypto_box_NONCEBYTES
#error CRYPTO_NONCE_SIZE should be equal to crypto_box_NONCEBYTES
#endif

#if CRYPTO_SHA256_SIZE != crypto_hash_sha256_BYTES
#error CRYPTO_SHA256_SIZE should be equal to crypto_hash_sha256_BYTES
#endif

#if CRYPTO_SHA512_SIZE != crypto_hash_sha512_BYTES
#error CRYPTO_SHA512_SIZE should be equal to crypto_hash_sha512_BYTES
#endif

int32_t public_key_cmp(const uint8_t *pk1, const uint8_t *pk2)
{
#if CRYPTO_PUBLIC_KEY_SIZE != 32
#error CRYPTO_PUBLIC_KEY_SIZE is required to be 32 bytes for public_key_cmp to work,
#endif
    return crypto_verify_32(pk1, pk2);
}

uint16_t random_u16(void)
{
    uint16_t randnum;
    randombytes((uint8_t *)&randnum, sizeof(randnum));
    return randnum;
}

uint32_t random_u32(void)
{
    uint32_t randnum;
    randombytes((uint8_t *)&randnum, sizeof(randnum));
    return randnum;
}

uint64_t random_u64(void)
{
    uint64_t randnum;
    randombytes((uint8_t *)&randnum, sizeof(randnum));
    return randnum;
}

bool public_key_valid(const uint8_t *public_key)
{
    if (public_key[31] >= 128) { /* Last bit of key is always zero. */
        return 0;
    }

    return 1;
}

/* Precomputes the shared key from their public_key and our secret_key.
 * This way we can avoid an expensive elliptic curve scalar multiply for each
 * encrypt/decrypt operation.
 * shared_key has to be crypto_box_BEFORENMBYTES bytes long.
 */
int32_t encrypt_precompute(const uint8_t *public_key, const uint8_t *secret_key, uint8_t *shared_key)
{
    return crypto_box_beforenm(shared_key, public_key, secret_key);
}

int32_t encrypt_data_symmetric(const uint8_t *secret_key, const uint8_t *nonce, const uint8_t *plain, size_t length,
                               uint8_t *encrypted)
{
    if (length == 0 || !secret_key || !nonce || !plain || !encrypted) {
        return -1;
    }

    VLA(uint8_t, temp_plain, length + crypto_box_ZEROBYTES);
    VLA(uint8_t, temp_encrypted, length + crypto_box_MACBYTES + crypto_box_BOXZEROBYTES);

    memset(temp_plain, 0, crypto_box_ZEROBYTES);
    memcpy(temp_plain + crypto_box_ZEROBYTES, plain, length); // Pad the message with 32 0 bytes.

    if (crypto_box_afternm(temp_encrypted, temp_plain, length + crypto_box_ZEROBYTES, nonce, secret_key) != 0) {
        return -1;
    }

    /* Unpad the encrypted message. */
    memcpy(encrypted, temp_encrypted + crypto_box_BOXZEROBYTES, length + crypto_box_MACBYTES);
    return length + crypto_box_MACBYTES;
}

int32_t decrypt_data_symmetric(const uint8_t *secret_key, const uint8_t *nonce, const uint8_t *encrypted, size_t length,
                               uint8_t *plain)
{
    if (length <= crypto_box_BOXZEROBYTES || !secret_key || !nonce || !encrypted || !plain) {
        return -1;
    }

    VLA(uint8_t, temp_plain, length + crypto_box_ZEROBYTES);
    VLA(uint8_t, temp_encrypted, length + crypto_box_BOXZEROBYTES);

    memset(temp_encrypted, 0, crypto_box_BOXZEROBYTES);
    memcpy(temp_encrypted + crypto_box_BOXZEROBYTES, encrypted, length); // Pad the message with 16 0 bytes.

    if (crypto_box_open_afternm(temp_plain, temp_encrypted, length + crypto_box_BOXZEROBYTES, nonce, secret_key) != 0) {
        return -1;
    }

    memcpy(plain, temp_plain + crypto_box_ZEROBYTES, length - crypto_box_MACBYTES);
    return length - crypto_box_MACBYTES;
}

int32_t encrypt_data(const uint8_t *public_key, const uint8_t *secret_key, const uint8_t *nonce,
                     const uint8_t *plain, size_t length, uint8_t *encrypted)
{
    if (!public_key || !secret_key) {
        return -1;
    }

    uint8_t k[crypto_box_BEFORENMBYTES];
    encrypt_precompute(public_key, secret_key, k);
    int ret = encrypt_data_symmetric(k, nonce, plain, length, encrypted);
    crypto_memzero(k, sizeof k);
    return ret;
}

int32_t decrypt_data(const uint8_t *public_key, const uint8_t *secret_key, const uint8_t *nonce,
                     const uint8_t *encrypted, size_t length, uint8_t *plain)
{
    if (!public_key || !secret_key) {
        return -1;
    }

    uint8_t k[crypto_box_BEFORENMBYTES];
    encrypt_precompute(public_key, secret_key, k);
    int ret = decrypt_data_symmetric(k, nonce, encrypted, length, plain);
    crypto_memzero(k, sizeof k);
    return ret;
}


/* Increment the given nonce by 1. */
void increment_nonce(uint8_t *nonce)
{
    /* TODO(irungentoo): use increment_nonce_number(nonce, 1) or sodium_increment (change to little endian)
     * NOTE don't use breaks inside this loop
     * In particular, make sure, as far as possible,
     * that loop bounds and their potential underflow or overflow
     * are independent of user-controlled input (you may have heard of the Heartbleed bug).
     */
    uint32_t i = crypto_box_NONCEBYTES;
    uint_fast16_t carry = 1U;

    for (; i != 0; --i) {
        carry += (uint_fast16_t) nonce[i - 1];
        nonce[i - 1] = (uint8_t) carry;
        carry >>= 8;
    }
}

static uint32_t host_to_network(uint32_t x)
{
#if !defined(BYTE_ORDER) || BYTE_ORDER == LITTLE_ENDIAN
    return
        ((x >> 24) & 0x000000FF) | // move byte 3 to byte 0
        ((x >> 8)  & 0x0000FF00) | // move byte 2 to byte 1
        ((x << 8)  & 0x00FF0000) | // move byte 1 to byte 2
        ((x << 24) & 0xFF000000);  // move byte 0 to byte 3
#else
    return x;
#endif
}

/* increment the given nonce by num */
void increment_nonce_number(uint8_t *nonce, uint32_t host_order_num)
{
    /* NOTE don't use breaks inside this loop
     * In particular, make sure, as far as possible,
     * that loop bounds and their potential underflow or overflow
     * are independent of user-controlled input (you may have heard of the Heartbleed bug).
     */
    const uint32_t big_endian_num = host_to_network(host_order_num);
    const uint8_t *const num_vec = (const uint8_t *) &big_endian_num;
    uint8_t num_as_nonce[crypto_box_NONCEBYTES] = {0};
    num_as_nonce[crypto_box_NONCEBYTES - 4] = num_vec[0];
    num_as_nonce[crypto_box_NONCEBYTES - 3] = num_vec[1];
    num_as_nonce[crypto_box_NONCEBYTES - 2] = num_vec[2];
    num_as_nonce[crypto_box_NONCEBYTES - 1] = num_vec[3];

    uint32_t i = crypto_box_NONCEBYTES;
    uint_fast16_t carry = 0U;

    for (; i != 0; --i) {
        carry += (uint_fast16_t) nonce[i - 1] + (uint_fast16_t) num_as_nonce[i - 1];
        nonce[i - 1] = (unsigned char) carry;
        carry >>= 8;
    }
}

/* Fill the given nonce with random bytes. */
void random_nonce(uint8_t *nonce)
{
    randombytes(nonce, crypto_box_NONCEBYTES);
}

/* Fill a key CRYPTO_SYMMETRIC_KEY_SIZE big with random bytes */
void new_symmetric_key(uint8_t *key)
{
    randombytes(key, CRYPTO_SYMMETRIC_KEY_SIZE);
}

int32_t crypto_new_keypair(uint8_t *public_key, uint8_t *secret_key)
{
    return crypto_box_keypair(public_key, secret_key);
}

void crypto_derive_public_key(uint8_t *public_key, const uint8_t *secret_key)
{
    crypto_scalarmult_curve25519_base(public_key, secret_key);
}

void crypto_sha256(uint8_t *hash, const uint8_t *data, size_t length)
{
    crypto_hash_sha256(hash, data, length);
}

void crypto_sha512(uint8_t *hash, const uint8_t *data, size_t length)
{
    crypto_hash_sha512(hash, data, length);
}

void random_bytes(uint8_t *data, size_t length)
{
    randombytes(data, length);
}
