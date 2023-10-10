/*
* based on pkg2zip by mmozeiko
* https://github.com/mmozeiko/pkg2zip/
*/

#include "pkgi.h"
#include "pkgi_aes.h"
#include "pkgi_download.h"
#include <zlib.h>

#include <stdio.h>
#include <string.h>

#define ISO_SECTOR_SIZE 2048
#define CSO_HEADER_SIZE 24

#define PKG_HEADER_SIZE 192
#define PKG_HEADER_EXT_SIZE 64

#define PKG_TYPE_PSP 0x0001
#define PKG_TYPE_PSX 0x0002
#define PKG_TYPE_PTF 0x0003

#define Z_WBITS_DEFLATE (-15)

// https://vitadevwiki.com/vita/Keys_NonVita#PSPAESKirk4.2F7
static const uint8_t kirk7_key38[] = { 0x12, 0x46, 0x8d, 0x7e, 0x1c, 0x42, 0x20, 0x9b, 0xba, 0x54, 0x26, 0x83, 0x5e, 0xb0, 0x33, 0x03 };
static const uint8_t kirk7_key39[] = { 0xc4, 0x3b, 0xb6, 0xd6, 0x53, 0xee, 0x67, 0x49, 0x3e, 0xa9, 0x5f, 0xbc, 0x0c, 0xed, 0x6f, 0x8a };
static const uint8_t kirk7_key63[] = { 0x9c, 0x9b, 0x13, 0x72, 0xf8, 0xc6, 0x40, 0xcf, 0x1c, 0x62, 0xf5, 0xd5, 0x92, 0xdd, 0xb5, 0x82 };

// https://vitadevwiki.com/vita/Keys_NonVita#PSPAMHashKey
static const uint8_t amctl_hashkey_3[] = { 0xe3, 0x50, 0xed, 0x1d, 0x91, 0x0a, 0x1f, 0xd0, 0x29, 0xbb, 0x1c, 0x3e, 0xf3, 0x40, 0x77, 0xfb };
static const uint8_t amctl_hashkey_4[] = { 0x13, 0x5f, 0xa4, 0x7c, 0xab, 0x39, 0x5b, 0xa4, 0x76, 0xb8, 0xcc, 0xa9, 0x8f, 0x3a, 0x04, 0x45 };
static const uint8_t amctl_hashkey_5[] = { 0x67, 0x8d, 0x7f, 0xa3, 0x2a, 0x9c, 0xa0, 0xd1, 0x50, 0x8a, 0xd8, 0x38, 0x5e, 0x4b, 0x01, 0x7e };

// https://wiki.henkaku.xyz/vita/Packages#AES_Keys
static const uint8_t pkg_ps3_key[] = { 0x2e, 0x7b, 0x71, 0xd7, 0xc9, 0xc9, 0xa1, 0x4e, 0xa3, 0x22, 0x1f, 0x18, 0x88, 0x28, 0xb8, 0xf8 };
static const uint8_t pkg_psp_key[] = { 0x07, 0xf2, 0xc6, 0x82, 0x90, 0xb5, 0x0d, 0x2c, 0x33, 0x81, 0x8d, 0x70, 0x9b, 0x60, 0xe6, 0x2b };

// lzrc decompression code from libkirk by tpu
typedef struct {
    // input stream
    const uint8_t* input;
    uint32_t in_ptr;
    uint32_t in_len;

    // output stream
    uint8_t* output;
    uint32_t out_ptr;
    uint32_t out_len;

    // range decode
    uint32_t range;
    uint32_t code;
    uint32_t out_code;
    uint8_t lc;

    uint8_t bm_literal[8][256];
    uint8_t bm_dist_bits[8][39];
    uint8_t bm_dist[18][8];
    uint8_t bm_match[8][8];
    uint8_t bm_len[8][31];
} lzrc_decode;


static uint32_t zlib_deflate_cso(FILE *outfile, uint8_t *in, uint32_t* cso_block)
{
    uint8_t GCC_ALIGN(16) out[ISO_SECTOR_SIZE];
    z_stream z;
    uint32_t ret;

    memset(&z, 0, sizeof(z));
    if(deflateInit2(&z, Z_BEST_COMPRESSION, Z_DEFLATED, Z_WBITS_DEFLATE, 8, Z_DEFAULT_STRATEGY))
    {
        LOG("Error: zlib initialization error");
        return(0);
    }

    z.next_in   = in;
    z.avail_in  = ISO_SECTOR_SIZE;
    z.next_out  = out;
    z.avail_out = sizeof(out);

    if (deflate(&z, Z_FINISH) == Z_STREAM_END)
    {
        pkgi_write(outfile, out, (uint32_t)z.total_out);
        ret = z.total_out;
    }
    else
    {
        *cso_block |= 0x80000000;
        pkgi_write(outfile, in, ISO_SECTOR_SIZE);
        ret = ISO_SECTOR_SIZE;
    }

    deflateEnd(&z);
    return(ret);
}

static void sys_read(FILE* file, uint64_t offset, void* buffer, uint32_t size)
{
    fseek(file, offset, SEEK_SET);
    fread(buffer, size, 1, file);
}

static void out_write_at(FILE* file, long offset, const void* buffer, uint32_t size)
{
    long pos = ftell(file);

    fseek(file, offset, SEEK_SET);
    fwrite(buffer, size, 1, file);
    fseek(file, pos, SEEK_SET);
}

static void rc_init(lzrc_decode* rc, void* out, int out_len, const void* in, int in_len)
{
    if (in_len < 5)
    {
        LOG("ERROR: internal error - lzrc input underflow! pkg may be corrupted?\n");
        return;
    }

    rc->input = in;
    rc->in_len = in_len;
    rc->in_ptr = 5;

    rc->output = out;
    rc->out_len = out_len;
    rc->out_ptr = 0;

    rc->range = 0xffffffff;
    rc->lc = rc->input[0];
    rc->code = get32be(rc->input + 1);
    rc->out_code = 0xffffffff;

    memset(rc->bm_literal, 0x80, sizeof(rc->bm_literal));
    memset(rc->bm_dist_bits, 0x80, sizeof(rc->bm_dist_bits));
    memset(rc->bm_dist, 0x80, sizeof(rc->bm_dist));
    memset(rc->bm_match, 0x80, sizeof(rc->bm_match));
    memset(rc->bm_len, 0x80, sizeof(rc->bm_len));
}

static void normalize(lzrc_decode* rc)
{
    if (rc->range < 0x01000000)
    {
        rc->range <<= 8;
        rc->code = (rc->code << 8) + rc->input[rc->in_ptr];
        rc->in_ptr++;
    }
}

static int rc_bit(lzrc_decode* rc, uint8_t *prob)
{
    uint32_t bound;

    normalize(rc);

    bound = (rc->range >> 8) * (*prob);
    *prob -= *prob >> 3;

    if (rc->code < bound)
    {
        rc->range = bound;
        *prob += 31;
        return 1;
    }
    else
    {
        rc->code -= bound;
        rc->range -= bound;
        return 0;
    }
}

static int rc_bittree(lzrc_decode* rc, uint8_t *probs, int limit)
{
    int number = 1;

    do
    {
        number = (number << 1) + rc_bit(rc, probs + number);
    }
    while (number < limit);

    return number;
}

static int rc_number(lzrc_decode* rc, uint8_t *prob, uint32_t n)
{
    int number = 1;

    if (n > 3)
    {
        number = (number << 1) + rc_bit(rc, prob + 3);
        if (n > 4)
        {
            number = (number << 1) + rc_bit(rc, prob + 3);
            if (n > 5)
            {
                // direct bits
                normalize(rc);

                for (uint32_t i = 0; i < n - 5; i++)
                {
                    rc->range >>= 1;
                    number <<= 1;
                    if (rc->code < rc->range)
                    {
                        number += 1;
                    }
                    else
                    {
                        rc->code -= rc->range;
                    }
                }
            }
        }
    }

    if (n > 0)
    {
        number = (number << 1) + rc_bit(rc, prob);
        if (n > 1)
        {
            number = (number << 1) + rc_bit(rc, prob + 1);
            if (n > 2)
            {
                number = (number << 1) + rc_bit(rc, prob + 2);
            }
        }
    }

    return number;
}

static int lzrc_decompress(void* out, int out_len, const void* in, int in_len)
{
    lzrc_decode rc;
    rc_init(&rc, out, out_len, in, in_len);

    if (rc.lc & 0x80)
    {
        // plain text
        memcpy(rc.output, rc.input + 5, rc.code);
        return rc.code;
    }

    int rc_state = 0;
    uint8_t last_byte = 0;

    for (;;)
    {
        uint32_t match_step = 0;

        int bit = rc_bit(&rc, &rc.bm_match[rc_state][match_step]);
        if (bit == 0) // literal
        {
            if (rc_state > 0)
            {
                rc_state -= 1;
            }

            int byte = rc_bittree(&rc, &rc.bm_literal[((last_byte >> rc.lc) & 0x07)][0], 0x100);
            byte -= 0x100;

            if (rc.out_ptr == rc.out_len)
            {
                LOG("ERROR: internal error - lzrc output overflow! pkg may be corrupted?\n");
                return -1;
            }
            rc.output[rc.out_ptr++] = (uint8_t)byte;
            last_byte = (uint8_t)byte;
        }
        else // match
        {
            // find bits of match length
            uint32_t len_bits = 0;
            for (int i = 0; i < 7; i++)
            {
                match_step += 1;
                bit = rc_bit(&rc, &rc.bm_match[rc_state][match_step]);
                if (bit == 0)
                {
                    break;
                }
                len_bits += 1;
            }

            // find match length
            uint32_t match_len;
            if (len_bits == 0)
            {
                match_len = 1;
            }
            else
            {
                uint32_t len_state = ((len_bits - 1) << 2) + ((rc.out_ptr << (len_bits - 1)) & 0x03);
                match_len = rc_number(&rc, &rc.bm_len[rc_state][len_state], len_bits);
                if (match_len == 0xFF)
                {
                    // end of stream
                    return rc.out_ptr;
                }
            }

            // find number of bits of match distance
            uint32_t dist_state = 0;
            uint32_t limit = 8;
            if (match_len > 2)
            {
                dist_state += 7;
                limit = 44;
            }
            int dist_bits = rc_bittree(&rc, &rc.bm_dist_bits[len_bits][dist_state], limit);
            dist_bits -= limit;

            // find match distance
            uint32_t match_dist;
            if (dist_bits > 0)
            {
                match_dist = rc_number(&rc, &rc.bm_dist[dist_bits][0], dist_bits);
            }
            else
            {
                match_dist = 1;
            }

            // copy match bytes
            if (match_dist > rc.out_ptr)
            {
                LOG("ERROR: internal error - lzrc match_dist out of range! pkg may be corrupted?\n");
                return -1;
            }

            if (rc.out_ptr + match_len + 1 > rc.out_len)
            {
                LOG("ERROR: internal error - lzrc output overflow! pkg may be corrupted?\n");
                return -1;
            }

            const uint8_t* match_src = rc.output + rc.out_ptr - match_dist;
            for (uint32_t i = 0; i <= match_len; i++)
            {
                rc.output[rc.out_ptr++] = *match_src++;
            }
            last_byte = match_src[-1];

            rc_state = 6 + ((rc.out_ptr + 1) & 1);
        }
    }
}

static void init_psp_decrypt(mbedtls_aes_context* key, uint8_t* iv, int eboot, const uint8_t* mac, const uint8_t* header, uint32_t offset1, uint32_t offset2)
{
    uint8_t tmp[16];
    aes128_init_dec(key, kirk7_key63);
    if (eboot)
    {
        aes128_ecb_decrypt(key, header + offset1, tmp);
    }
    else
    {
        memcpy(tmp, header + offset1, 16);
    }

    mbedtls_aes_context aes;
    aes128_init_dec(&aes, kirk7_key38);
    aes128_ecb_decrypt(&aes, tmp, tmp);

    for (size_t i = 0; i < 16; i++)
    {
        iv[i] = mac[i] ^ tmp[i] ^ header[offset2 + i] ^ amctl_hashkey_3[i] ^ amctl_hashkey_5[i];
    }
    aes128_init_dec(&aes, kirk7_key39);
    aes128_ecb_decrypt(&aes, iv, iv);

    for (size_t i = 0; i < 16; i++)
    {
        iv[i] ^= amctl_hashkey_4[i];
    }
}

static void unpack_psp_eboot(const char* path, mbedtls_aes_context* pkg_key, const uint8_t* pkg_iv, FILE* pkg, uint64_t enc_offset, uint64_t item_offset, uint64_t item_size, int cso)
{
    if (item_size < 0x28)
    {
        LOG("ERROR: eboot.pbp file is too short!\n");
        return;
    }

    uint8_t eboot_header[0x28];
    sys_read(pkg, enc_offset + item_offset, eboot_header, sizeof(eboot_header));
    aes128_ctr_xor(pkg_key, pkg_iv, item_offset / 16, eboot_header, sizeof(eboot_header));

    if (memcmp(eboot_header, "\x00PBP", 4) != 0)
    {
        LOG("ERROR: wrong eboot.pbp header signature!\n");
        return;
    }

    uint32_t psar_offset = get32le(eboot_header + 0x24);
    if (psar_offset + 256 > item_size)
    {
        LOG("ERROR: eboot.pbp file is too short!\n");
        return;
    }
    if (psar_offset % 16 != 0)
    {
        LOG("ERROR: invalid psar offset!\n");
        return;
    }

    uint8_t psar_header[256];
    sys_read(pkg, enc_offset + item_offset + psar_offset, psar_header, sizeof(psar_header));
    aes128_ctr_xor(pkg_key, pkg_iv, (item_offset + psar_offset) / 16, psar_header, sizeof(psar_header));

    if (memcmp(psar_header, "NPUMDIMG", 8) != 0)
    {
        LOG("ERROR: wrong data.psar header signature!\n");
        return;
    }

    uint32_t iso_block = get32le(psar_header + 0x0c);
    if (iso_block > 16)
    {
        LOG("ERROR: unsupported data.psar block size %u, max %u supported!\b", iso_block, 16);
        return;
    }

    uint8_t mac[16];
    aes128_cmac(kirk7_key38, psar_header, 0xc0, mac);

    mbedtls_aes_context psp_key;
    uint8_t psp_iv[16];
    init_psp_decrypt(&psp_key, psp_iv, 1, mac, psar_header, 0xc0, 0xa0);
    aes128_psp_decrypt(&psp_key, psp_iv, 0, psar_header + 0x40, 0x60);

    uint32_t iso_start = get32le(psar_header + 0x54);
    uint32_t iso_end = get32le(psar_header + 0x64);
    uint32_t iso_total = iso_end - iso_start - 1;
    uint32_t block_count = (iso_total + iso_block - 1) / iso_block;

    uint32_t iso_table = get32le(psar_header + 0x6c);

    if (iso_table + block_count * 32 > item_size)
    {
        LOG("ERROR: offset table in data.psar file is too large!\n");
        return;
    }

    uint32_t cso_index = 0;
    uint32_t cso_offset = 0;
    uint32_t* cso_block = NULL;

    void* outfile = pkgi_create(path);

    if (cso)
    {
        uint64_t cso_size = block_count * iso_block * ISO_SECTOR_SIZE;

        uint32_t cso_block_count = (uint32_t)(1 + (cso_size + ISO_SECTOR_SIZE - 1) / ISO_SECTOR_SIZE);
        cso_block = pkgi_malloc(cso_block_count * sizeof(uint32_t));

        uint8_t cso_header[CSO_HEADER_SIZE] = { 0x43, 0x49, 0x53, 0x4f };
        // header size
        set32le(cso_header + 4, sizeof(cso_header));
        // original size
        set64le(cso_header + 8, cso_size);
        // block size
        set32le(cso_header + 16, ISO_SECTOR_SIZE);
        // version
        cso_header[20] = 1;

        pkgi_write(outfile, cso_header, sizeof(cso_header));
        pkgi_write(outfile, cso_block, cso_block_count * sizeof(uint32_t));

        cso_offset = CSO_HEADER_SIZE + cso_block_count * sizeof(uint32_t);
    }

    for (uint32_t i = 0; i < block_count; i++)
    {
        uint64_t table_offset = item_offset + psar_offset + iso_table + 32 * i;

        uint8_t table[32];
        sys_read(pkg, enc_offset + table_offset, table, sizeof(table));
        aes128_ctr_xor(pkg_key, pkg_iv, table_offset / 16, table, sizeof(table));

        uint32_t t[8];
        for (size_t k = 0; k < 8; k++)
        {
            t[k] = get32le(table + k * 4);
        }

        uint32_t block_offset = t[4] ^ t[2] ^ t[3];
        uint32_t block_size = t[5] ^ t[1] ^ t[2];
        uint32_t block_flags = t[6] ^ t[0] ^ t[3];

        if (psar_offset + block_size > item_size)
        {
            LOG("ERROR: iso block size/offset is too large!\n");
            return;
        }

        uint8_t GCC_ALIGN(16) data[16 * ISO_SECTOR_SIZE];

        uint64_t abs_offset = item_offset + psar_offset + block_offset;
        update_install_progress(NULL, enc_offset + abs_offset);
        sys_read(pkg, enc_offset + abs_offset, data, block_size);
        aes128_ctr_xor(pkg_key, pkg_iv, abs_offset / 16, data, block_size);

        if ((block_flags & 4) == 0)
        {
            aes128_psp_decrypt(&psp_key, psp_iv, block_offset / 16, data, block_size);
        }

        if (block_size == iso_block * ISO_SECTOR_SIZE)
        {
            if (cso)
            {
                for (size_t n = 0; n < iso_block * ISO_SECTOR_SIZE; n += ISO_SECTOR_SIZE)
                {
                    cso_block[cso_index] = cso_offset;
                    cso_offset += zlib_deflate_cso(outfile, data + n, &cso_block[cso_index]);
                    cso_index++;
                }
            }
            else
            {
                pkgi_write(outfile, data, block_size);
            }
        }
        else
        {
            uint8_t GCC_ALIGN(16) uncompressed[16 * ISO_SECTOR_SIZE];
            uint32_t out_size = lzrc_decompress(uncompressed, sizeof(uncompressed), data, block_size);
            if (out_size != iso_block * ISO_SECTOR_SIZE)
            {
                LOG("ERROR: internal error - lzrc decompression failed! pkg may be corrupted?\n");
                return;
            }
            if (cso)
            {
                for (size_t n = 0; n < iso_block * ISO_SECTOR_SIZE; n += ISO_SECTOR_SIZE)
                {
                    cso_block[cso_index] = cso_offset;
                    cso_offset += zlib_deflate_cso(outfile, uncompressed + n, &cso_block[cso_index]);
                    cso_index++;
                }
            }
            else
            {
                pkgi_write(outfile, uncompressed, out_size);
            }
        }
    }

    if (cso)
    {
        cso_block[cso_index++] = cso_offset;
        out_write_at(outfile, CSO_HEADER_SIZE, cso_block, cso_index * sizeof(uint32_t));
        pkgi_free(cso_block);
    }

    pkgi_close(outfile);
}

static void unpack_psp_edat(const char* path, mbedtls_aes_context* pkg_key, const uint8_t* pkg_iv, FILE* pkg, uint64_t enc_offset, uint64_t item_offset, uint64_t item_size)
{
    if (item_size < 0x90 + 0xa0)
    {
        LOG("ERROR: EDAT file is to short!\n");
        return;
    }

    uint8_t item_header[90];
    sys_read(pkg, enc_offset + item_offset, item_header, sizeof(item_header));
    aes128_ctr_xor(pkg_key, pkg_iv, (item_offset) / 16, item_header, sizeof(item_header));
    uint8_t key_header_offset = item_header[0xC];

    uint8_t key_header[0xa0];
    sys_read(pkg, enc_offset + item_offset + key_header_offset, key_header, sizeof(key_header));
    aes128_ctr_xor(pkg_key, pkg_iv, (item_offset + key_header_offset) / 16, key_header, sizeof(key_header));

    if (memcmp(key_header, "\x00PGD", 4) != 0)
    {
        LOG("ERROR: wrong EDAT header signature!\n");
        return;
    }

    uint32_t key_index = get32le(key_header + 4);
    uint32_t drm_type = get32le(key_header + 8);
    if (key_index != 1 || drm_type != 1)
    {
        LOG("ERROR: unsupported EDAT file, key/drm type is wrong!\n");
        return;
    }

    uint8_t mac[16];
    aes128_cmac(kirk7_key38, key_header, 0x70, mac);

    mbedtls_aes_context psp_key;
    uint8_t psp_iv[16];
    init_psp_decrypt(&psp_key, psp_iv, 0, mac, key_header, 0x70, 0x10);
    aes128_psp_decrypt(&psp_key, psp_iv, 0, key_header + 0x30, 0x30);

    uint32_t data_size = get32le(key_header + 0x44);
    uint32_t data_offset = get32le(key_header + 0x4c);

    if (data_offset != 0x90)
    {
        LOG("ERROR: unsupported EDAT file, data offset is wrong!\n");
        return;
    }

    init_psp_decrypt(&psp_key, psp_iv, 0, mac, key_header, 0x70, 0x30);

    uint32_t block_size = 0x10;
    uint32_t block_count = ((data_size + (block_size - 1)) / block_size );

    void* outfile = pkgi_create(path);
    for (uint32_t i = 0; i < block_count; i++)
    {
        uint8_t block[0x10];
        uint32_t block_offset = (data_offset + (i * block_size)); 

        // update progress bar every 128Kb
        if (i % 0x2000 == 0)
            update_install_progress(NULL, enc_offset + item_offset + key_header_offset + block_offset);

        sys_read(pkg, enc_offset + item_offset + key_header_offset + block_offset, block, block_size);
        aes128_ctr_xor(pkg_key, pkg_iv, (item_offset + key_header_offset + block_offset)  / 16, block, block_size);
        aes128_psp_decrypt(&psp_key, psp_iv, i * block_size / 16, block, block_size);

        uint32_t out_size = 0x10;
        if ( ((i + 1) * block_size) > data_size )
        {
            out_size = data_size - (i * block_size);
        }
        pkgi_write(outfile, block, out_size);
    }

    pkgi_close(outfile);
}

int convert_psp_pkg_iso(const char* pkg_arg, int cso)
{
    LOG("pkg2zip v1.8");
    LOG("[*] loading %s...", pkg_arg);

    uint64_t pkg_size = pkgi_get_size(pkg_arg);
    void* pkg = pkgi_open(pkg_arg);
    if (!pkg)
    {
        LOG("ERROR: could not open pkg file");
        return(0);
    }

    uint8_t pkg_header[PKG_HEADER_SIZE + PKG_HEADER_EXT_SIZE];
    sys_read(pkg, 0, pkg_header, sizeof(pkg_header));

    if (get32be(pkg_header) != 0x7f504b47 || get32be(pkg_header + PKG_HEADER_SIZE) != 0x7F657874)
    {
        LOG("ERROR: not a pkg file\n");
        return(0);
    }

    // http://www.psdevwiki.com/ps3/PKG_files
    uint64_t meta_offset = get32be(pkg_header + 8);
    uint32_t meta_count = get32be(pkg_header + 12);
    uint32_t item_count = get32be(pkg_header + 20);
    uint64_t total_size = get64be(pkg_header + 24);
    uint64_t enc_offset = get64be(pkg_header + 32);
    uint64_t enc_size = get64be(pkg_header + 40);
    const uint8_t* iv = pkg_header + 0x70;
    int key_type = pkg_header[0xe7] & 7;

    if (pkg_size < total_size)
    {
        LOG("ERROR: pkg file is too small\n");
        return(0);
    }
    if (pkg_size < enc_offset + item_count * 32)
    {
        LOG("ERROR: pkg file is too small\n");
        return(0);
    }

    uint32_t content_type = 0;
    uint32_t items_offset = 0;
    uint32_t items_size = 0;

    for (uint32_t i = 0; i < meta_count; i++)
    {
        uint8_t block[16];
        sys_read(pkg, meta_offset, block, sizeof(block));

        uint32_t type = get32be(block + 0);
        uint32_t size = get32be(block + 4);

        if (type == 2)
        {
            content_type = get32be(block + 8);
        }
        else if (type == 13)
        {
            items_offset = get32be(block + 8);
            items_size = get32be(block + 12);
        }
/*
        else if (type == 10)
        {
            // DLC
            sys_read(pkg, meta_offset + 8 + 8, dlc_install_dir, sizeof(dlc_install_dir));
        }
        else if (type == 14)
        {
            sfo_offset = get32be(block + 8);
            sfo_size = get32be(block + 12);
        }
*/

        meta_offset += 2 * sizeof(uint32_t) + size;
    }

    int type;

    // http://www.psdevwiki.com/ps3/PKG_files
    if (content_type == 6)
    {
        type = PKG_TYPE_PSX;
        LOG("[*] unpacking PSX");
    }
    else if (content_type == 7 || content_type == 0xe || content_type == 0xf || content_type == 0x10)
    {
        // PSP & PSP-PCEngine / PSP-Go / PSP-Mini / PSP-NeoGeo
        type = PKG_TYPE_PSP;
        LOG("[*] unpacking PSP");
    }
    else if (content_type == 9)
    {
        // PSP Theme
        type = PKG_TYPE_PTF;
        LOG("[*] unpacking PSP Theme");
    }
    else
    {
        LOG("ERROR: unsupported content type 0x%x", content_type);
        return(0);
    }

    mbedtls_aes_context ps3_key;
    uint8_t main_key[16];
    if (key_type == 1)
    {
        memcpy(main_key, pkg_psp_key, sizeof(main_key));
        aes128_init(&ps3_key, pkg_ps3_key);
    }
    else
    {
        LOG("ERROR: unsupported key type 0x%x", key_type);
        return(0);
    }

    mbedtls_aes_context key;
    aes128_init(&key, main_key);

    const char* title = (char*)pkg_header + 0x44;
    const char* id = (char*)pkg_header + 0x37;

    char root[1024];

    if (type == PKG_TYPE_PSP)
    {
        snprintf(root, sizeof(root), "%s/ISO", pkgi_get_storage_device());
        pkgi_mkdirs(root);

        if (content_type == 7) // && strcmp(category, "HG") == 0)
        {
            snprintf(root, sizeof(root), "%s/PSP/GAME/%.9s", pkgi_get_storage_device(), id);
        }
    }
    else if (type == PKG_TYPE_PTF)
    {
        snprintf(root, sizeof(root), "%s/PSP/THEME", pkgi_get_storage_device());
        pkgi_mkdirs(root);
    }
    else if (type == PKG_TYPE_PSX)
    {
        snprintf(root, sizeof(root), "%s/PSP/GAME/%.9s", pkgi_get_storage_device(), id);
        pkgi_mkdirs(root);
    }
    else
    {
        LOG("ERROR: unsupported type\n");
        return(0);
    }

    char path[1024];

    for (uint32_t item_index = 0; item_index < item_count; item_index++)
    {
        uint8_t item[32];
        uint64_t item_offset = items_offset + item_index * 32;
        sys_read(pkg, enc_offset + item_offset, item, sizeof(item));
        aes128_ctr_xor(&key, iv, item_offset / 16, item, sizeof(item));

        uint32_t name_offset = get32be(item + 0);
        uint32_t name_size = get32be(item + 4);
        uint64_t data_offset = get64be(item + 8);
        uint64_t data_size = get64be(item + 16);
        uint8_t psp_type = item[24];
        uint8_t flags = item[27];

        if ((name_offset % 16 != 0) || (data_offset % 16 != 0))
        {
            LOG("ERROR: pkg file is corrupted");
            return(0);
        }

        if (pkg_size < enc_offset + name_offset + name_size ||
            pkg_size < enc_offset + data_offset + data_size)
        {
            LOG("ERROR: pkg file is too short, possibly corrupted\n");
            return(0);
        }

        if (name_size >= FILENAME_MAX)
        {
            LOG("ERROR: pkg file contains file with very long name\n");
            return(0);
        }

        mbedtls_aes_context* item_key;
        if (type == PKG_TYPE_PSP || type == PKG_TYPE_PSX)
        {
            item_key = psp_type == 0x90 ? &key : &ps3_key;
        }
        else
        {
            item_key = &key;
        }

        char name[FILENAME_MAX];
        sys_read(pkg, enc_offset + name_offset, name, name_size);
        aes128_ctr_xor(item_key, iv, name_offset / 16, (uint8_t*)name, name_size);
        name[name_size] = 0;

        // LOG("[%u/%u] %s\n", item_index + 1, item_count, name);

        if (flags != 4 && flags != 18)
        {
            if (type == PKG_TYPE_PSX)
            {
                if (strcmp("USRDIR/CONTENT/DOCUMENT.DAT", name) == 0)
                {
                    snprintf(path, sizeof(path), "%s/DOCUMENT.DAT", root);
                }
                else if (strcmp("USRDIR/CONTENT/EBOOT.PBP", name) == 0)
                {
                    snprintf(path, sizeof(path), "%s/EBOOT.PBP", root);
                }
                else
                {
                    continue;
                }
            }
            else if (type == PKG_TYPE_PTF)
            {
                snprintf(path, sizeof(path), "%s/PSP/THEME/%s", pkgi_get_storage_device(), name);
                update_install_progress(path + 15, 0);
                unpack_psp_edat(path, item_key, iv, pkg, enc_offset, data_offset, data_size);
                continue;
            }
            else if (type == PKG_TYPE_PSP)
            {
                if (strcmp("USRDIR/CONTENT/EBOOT.PBP", name) == 0)
                {
                    snprintf(path, sizeof(path), "%s/ISO/%s [%.9s].%s", pkgi_get_storage_device(), title, id, cso ? "cso" : "iso");
                    update_install_progress(path + 4, 0);
                    unpack_psp_eboot(path, item_key, iv, pkg, enc_offset, data_offset, data_size, cso);
                    continue;
                }
/*
                else if (strcmp("USRDIR/CONTENT/PSP-KEY.EDAT", name) == 0)
                {
                    snprintf(path, sizeof(path), "%s/PSP/GAME/%.9s/PSP-KEY.EDAT", pkgi_get_storage_device(), id);
                    unpack_psp_key(path, item_key, iv, pkg, enc_offset, data_offset, data_size);
                    continue;
                }
                else if (strcmp("USRDIR/CONTENT/CONTENT.DAT", name) == 0)
                {
                    snprintf(path, sizeof(path), "%s/PSP/GAME/%.9s/CONTENT.DAT", pkgi_get_storage_device(), id);
                }
*/
                else
                {
                    continue;
                }
            }
            else
            {
                snprintf(path, sizeof(path), "%s/%s", root, name);
            }

            uint64_t offset = data_offset;

            void* outfile = pkgi_create(path);
            while (data_size != 0)
            {
                uint8_t GCC_ALIGN(16) buffer[1 << 16];
                uint32_t size = (uint32_t)min64(data_size, sizeof(buffer));
                update_install_progress(path + 14, enc_offset + offset);

                sys_read(pkg, enc_offset + offset, buffer, size);
                aes128_ctr_xor(item_key, iv, offset / 16, buffer, size);
                pkgi_write(outfile, buffer, size);

                offset += size;
                data_size -= size;
            }

            pkgi_close(outfile);
        }
    }
    pkgi_close(pkg);
    update_install_progress(NULL, pkg_size);

    LOG("[*] unpacking completed");
    return 1;
}
