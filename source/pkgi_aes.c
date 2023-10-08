#include "pkgi_aes.h"
#include "pkgi_utils.h"

#include <string.h>


void aes128_init(mbedtls_aes_context* ctx, const uint8_t* key)
{
    mbedtls_aes_init(ctx);
    mbedtls_aes_setkey_enc(ctx, key, 128);
}

void aes128_init_dec(mbedtls_aes_context* ctx, const uint8_t* key)
{
    mbedtls_aes_init(ctx);
    mbedtls_aes_setkey_dec(ctx, key, 128);
/*
    mbedtls_aes_context enc;
    aes128_init(&enc, key);
*/
}

static void aes128_encrypt(mbedtls_aes_context* ctx, const uint8_t* input, uint8_t* output)
{
    mbedtls_aes_crypt_ecb(ctx, MBEDTLS_AES_ENCRYPT, input, output);
}

static void aes128_decrypt(mbedtls_aes_context* ctx, const uint8_t* input, uint8_t* output)
{
    mbedtls_aes_crypt_ecb(ctx, MBEDTLS_AES_DECRYPT, input, output);
}

void aes128_ecb_encrypt(mbedtls_aes_context* ctx, const uint8_t* input, uint8_t* output)
{
    aes128_encrypt(ctx, input, output);
}

void aes128_ecb_decrypt(mbedtls_aes_context* ctx, const uint8_t* input, uint8_t* output)
{
    aes128_decrypt(ctx, input, output);
}

static void ctr_add(uint8_t* counter, uint64_t n)
{
    for (int i=15; i>=0; i--)
    {
        n = n + counter[i];
        counter[i] = (uint8_t)n;
        n >>= 8;
    }
}

void aes128_ctr_xor(mbedtls_aes_context* context, const uint8_t* iv, uint64_t block, uint8_t* buffer, size_t size)
{
    uint8_t tmp[16];
    uint8_t counter[16];
    for (uint32_t i=0; i<16; i++)
    {
        counter[i] = iv[i];
    }
    ctr_add(counter, block);

    while (size >= 16)
    {
        aes128_encrypt(context, counter, tmp);
        for (uint32_t i=0; i<16; i++)
        {
            *buffer++ ^= tmp[i];
        }
        ctr_add(counter, 1);
        size -= 16;
    }

    if (size != 0)
    {
        aes128_encrypt(context, counter, tmp);
        for (size_t i=0; i<size; i++)
        {
            *buffer++ ^= tmp[i];
        }
    }
}

// https://tools.ietf.org/rfc/rfc4493.txt

typedef struct {
    mbedtls_aes_context key;
    uint8_t last[16];
    uint8_t block[16];
    uint32_t size;
} aes128_cmac_ctx;

static void aes128_cmac_process(mbedtls_aes_context* ctx, uint8_t* block, const uint8_t *buffer, uint32_t size)
{
    if(size % 16 != 0)
        return;

    for (uint32_t i = 0; i < size; i += 16)
    {
        for (size_t k = 0; k < 16; k++)
        {
            block[k] ^= *buffer++;
        }
        aes128_ecb_encrypt(ctx, block, block);
    }
}

static void aes128_cmac_init(aes128_cmac_ctx* ctx, const uint8_t* key)
{
    aes128_init(&ctx->key, key);
    memset(ctx->last, 0, 16);
    ctx->size = 0;
}

static void aes128_cmac_update(aes128_cmac_ctx* ctx, const uint8_t* buffer, uint32_t size)
{
    if (ctx->size + size <= 16)
    {
        memcpy(ctx->block + ctx->size, buffer, size);
        ctx->size += size;
        return;
    }

    if (ctx->size != 0)
    {
        uint32_t avail = 16 - ctx->size;
        memcpy(ctx->block + ctx->size, buffer, avail < size ? avail : size);
        buffer += avail;
        size -= avail;

        aes128_cmac_process(&ctx->key, ctx->last, ctx->block, 16);
    }

    if (size >= 16)
    {
        uint32_t full = (size - 1) & ~15;
        aes128_cmac_process(&ctx->key, ctx->last, buffer, full);
        buffer += full;
        size -= full;
    }

    memcpy(ctx->block, buffer, size);
    ctx->size = size;
}

static void cmac_gfmul(uint8_t* block)
{
    uint8_t carry = 0;
    for (int i = 15; i >= 0; i--)
    {
        uint8_t x = block[i];
        block[i] = (block[i] << 1) | (carry >> 7);
        carry = x;
    }

    block[15] ^= (carry & 0x80 ? 0x87 : 0);
}

static void aes128_cmac_done(aes128_cmac_ctx* ctx, uint8_t* mac)
{
    uint8_t zero[16] = { 0 };
    aes128_ecb_encrypt(&ctx->key, zero, mac);

    cmac_gfmul(mac);

    if (ctx->size != 16)
    {
        cmac_gfmul(mac);

        ctx->block[ctx->size] = 0x80;
        memset(ctx->block + ctx->size + 1, 0, 16 - (ctx->size + 1));
    }

    for (size_t i = 0; i < 16; i++)
    {
        mac[i] ^= ctx->block[i];
    }

    aes128_cmac_process(&ctx->key, mac, ctx->last, 16);
}

void aes128_cmac(const uint8_t* key, const uint8_t* buffer, uint32_t size, uint8_t* mac)
{
    aes128_cmac_ctx ctx;
    aes128_cmac_init(&ctx, key);
    aes128_cmac_update(&ctx, buffer, size);
    aes128_cmac_done(&ctx, mac);
}

void aes128_psp_decrypt(mbedtls_aes_context* ctx, const uint8_t* iv, uint32_t index, uint8_t* buffer, uint32_t size)
{
    if(size % 16 != 0)
        return;

    uint8_t GCC_ALIGN(16) prev[16];
    uint8_t GCC_ALIGN(16) block[16];

    if (index == 0)
    {
        memset(prev, 0, 16);
    }
    else
    {
        memcpy(prev, iv, 12);
        set32le(prev + 12, index);
    }

    memcpy(block, iv, 16);
    set32le(block + 12, index);

    for (uint32_t i = 0; i < size; i += 16)
    {
        set32le(block + 12, get32le(block + 12) + 1);

        uint8_t out[16];
        aes128_ecb_decrypt(ctx, block, out);

        for (size_t k = 0; k < 16; k++)
        {
            *buffer++ ^= prev[k] ^ out[k];
        }
        memcpy(prev, block, 16);
    }
}
