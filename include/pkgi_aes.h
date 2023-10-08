#pragma once

#include "pkgi_utils.h"
#include <mbedtls/aes.h>


void aes128_init(mbedtls_aes_context* ctx, const uint8_t* key);
void aes128_init_dec(mbedtls_aes_context* ctx, const uint8_t* key);

void aes128_ecb_encrypt(mbedtls_aes_context* ctx, const uint8_t* input, uint8_t* output);
void aes128_ecb_decrypt(mbedtls_aes_context* ctx, const uint8_t* input, uint8_t* output);

void aes128_ctr_xor(mbedtls_aes_context* ctx, const uint8_t* iv, uint64_t block, uint8_t* buffer, size_t size);

void aes128_cmac(const uint8_t* key, const uint8_t* buffer, uint32_t size, uint8_t* mac);

void aes128_psp_decrypt(mbedtls_aes_context* ctx, const uint8_t* iv, uint32_t index, uint8_t* buffer, uint32_t size);
