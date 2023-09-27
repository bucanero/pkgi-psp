#include <pspkernel.h>
#include <psputilsforkernel.h>
#include <psploadexec_kernel.h>
#include <pspctrl.h>
#include <pspdebug.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <mbedtls/aes.h>

#include "pkgi.h"

#define DEPACKAGER_VER 3

/*
typedef struct {
	u32 magic;
	u16 pkg_revision;
	u16 pkg_type;
	u32 pkg_metadata_offset;
	u32 pkg_metadata_count;
	u32 pkg_metadata_size;
	u32 item_count;
	u64 total_size;
	u64 data_offset;
	u64 data_size;
	u8 contentid[0x30];
	u8 digest[0x10];
	u8 pkg_data_riv[0x10];
	u8 pkg_header_digest[0x40];
} PKG_HEADER;

typedef struct {
	u32 magic;                             // 0x7F657874 (".ext")
	u32 unknown_1;                         // Maybe version. Always 1.
	u32 ext_hdr_size;                      // Extended header size. ex: 0x40
	u32 ext_data_size;                     // ex: 0x180
	u32 main_and_ext_headers_hmac_offset;  // ex: 0x100
	u32 metadata_header_hmac_offset;       // ex: 0x360, 0x390, 0x490 
	u64 tail_offset;                       // Tail size seems to be always 0x1A0
	u32 padding1;
	u32 pkg_key_id;                        // Id of the AES key used for decryption. PSP = 0x1, PS Vita = 0xC0000002, PSM = 0xC0000004
	u32 full_header_hmac_offset;           // ex: none (old pkg): 0, 0x930
	u8 padding2[0x14];
} PKG_EXT_HEADER;
*/

void install_update_progress(const char *filename, int64_t progress);

static u8 public_key[16], static_public_key[16], xor_key[16], gameid[10];

static const u8 PSPAESKey[16] __attribute__((aligned(16))) = {
	0x07, 0xF2, 0xC6, 0x82, 0x90, 0xB5, 0x0D, 0x2C, 0x33, 0x81, 0x8D, 0x70, 0x9B, 0x60, 0xE6, 0x2B
};


static void AES128_ECB_encrypt(uint8_t* input, const uint8_t* key, uint8_t *output)
{
	mbedtls_aes_context ctx;

	mbedtls_aes_init(&ctx);
	mbedtls_aes_setkey_enc(&ctx, key, 128);
	mbedtls_aes_crypt_ecb(&ctx, MBEDTLS_AES_ENCRYPT, input, output);
}

static void AES128_ECB_decrypt(uint8_t* input, const uint8_t* key, uint8_t *output)
{
	mbedtls_aes_context ctx;

	mbedtls_aes_init(&ctx);
	mbedtls_aes_setkey_dec(&ctx, key, 128);
	mbedtls_aes_crypt_ecb(&ctx, MBEDTLS_AES_DECRYPT, input, output);
}

static u32 toU32(char *_buf)
{
	u8 *buf = (u8 *)_buf;
	u32 b1 = buf[0] << 24;
	u32 b2 = buf[1] << 16;
	u32 b3 = buf[2] << 8;
	u32 size = b1 | b2 | b3 | buf[3];

	return size;
}

static int is_pkg_supported(const char *file)
{
	char buf[16];

	SceUID fd = sceIoOpen(file, PSP_O_RDONLY, 0777);
	if (fd >= 0) {
		sceIoRead(fd, buf, 4);
		sceIoClose(fd);

		return memcmp(buf, "\x7FPKG", 4) == 0;
	} else
		return 0;
}

static int is_pkg_type_supported(const char *file)
{
	char buf[16];

	SceUID fd = sceIoOpen(file, PSP_O_RDONLY, 0777);
	if (fd >= 0) {
		sceIoRead(fd, buf, 8);
		sceIoClose(fd);

		return memcmp(buf, "\x7FPKG\x80\x00\x00\x02", 8) == 0;
	} else
		return 0;
}

static void view_pkg_info(const char *file)
{
	char buf[256];

	LOG("PKG info viewer");

	SceUID fd = sceIoOpen(file, PSP_O_RDONLY, 0777);
	sceIoRead(fd, buf, sizeof(buf));
	sceIoClose(fd);

	u8 version = (u8)buf[4];

	LOG("PKG type:       %s", buf[7] == 1 ? "PS3 (currently unsupported)" : buf[7] == 2 ? "PSP" : "unknown");
	LOG("PKG version:    %s", (version == 0x80) ? "retail" : (version == 0x90) ? "debug (currently unsupported)" : "unknown");
	LOG("Content ID:     %s", buf + 0x30);
	LOG("PKG file count: %d", toU32(buf + 0x14));
	LOG("PKG size:       %d bytes", toU32(buf + 0x1C));
	LOG("Encrypted size: %d bytes", toU32(buf + 0x2C));
}

static void xor128(u8 *dst, u8 *xor1, u8 *xor2)
{
	int i;

	for (i = 0; i < 16; i++)
		dst[i] = xor1[i] ^ xor2[i];
}

static void iter128(u8 *buf)
{
	int i;

	for (i = 15; i >= 0; i--) {
		buf[i]++;

		if (buf[i])
			break;
	}
}

static void setiter128(u8 *dst, int size)
{
	memcpy(dst, static_public_key, 16);

	int i;

	for (i = 0; i < size; i++)
		iter128(dst);
}

int install_psp_pkg(const char *file)
{
	char *tmpBuf;
	char pkgBuf[256];
	SceIoStat stat;
	SceOff progress = 0;

	LOG("PSP depackager v%d", DEPACKAGER_VER);
	LOG("PSP PKG installer");

	if(is_pkg_supported(file))
		view_pkg_info(file);

	if (!is_pkg_type_supported(file)) {
		LOG("Unsupported PKG type detected.");
		return 0;
	}

	//1 MiB buffer
	tmpBuf = (char *)malloc(1024 * 1024);
	if (!tmpBuf) {
		LOG("Error allocating memory: 0x%08X", 1024 * 1024);
		return 0;
	}

	SceUID fd = sceIoOpen(file, PSP_O_RDONLY, 0777);
	SceSize fdsize = sceIoLseek(fd, 0, PSP_SEEK_END);
	sceIoLseek(fd, 0, PSP_SEEK_SET);

	sceIoRead(fd, pkgBuf, 256);

	if (toU32(pkgBuf + 0x1C) != fdsize) {
		sceIoClose(fd);
		LOG("Corrupt PKG detected");
		LOG("detected size: %d bytes", fdsize);
		LOG("expected size: %d bytes", toU32(pkgBuf + 0x1C));

		free(tmpBuf);
		return 0;
	}

	sceIoLseek(fd, toU32(pkgBuf + 8) + 0x48, PSP_SEEK_SET);
	sceIoRead(fd, gameid, 9);
	gameid[9] = 0;

	memcpy(public_key, pkgBuf + 0x70, 16);
	memcpy(static_public_key, pkgBuf + 0x70, 16);

	u32 enc_start = toU32(pkgBuf + 0x24);
	u32 files = toU32(pkgBuf + 0x14);

	sceIoLseek(fd, enc_start, PSP_SEEK_SET);
	sceIoRead(fd, tmpBuf, files * 32);

#ifdef MAGICGATE
	sceMgrAESInit();
#endif

	int i, j, pspcount = 0;
	u32 file_name[files], file_name_len[files], file_offset[files], file_size[files], is_file[files];

	for (i = 0; i < (int)(files * 2); i++) {
#ifdef MAGICGATE
		sceMgrAESEncrypt(xor_key, public_key, 16, PSPAESKey, NULL);
#else
		AES128_ECB_encrypt(public_key, PSPAESKey, xor_key);
#endif
		xor128((u8 *)(tmpBuf + (i * 16)), (u8 *)(tmpBuf + (i * 16)), xor_key);
		iter128(public_key);
	}

	for (i = 0; i < (int)files; i++) {
		if (((u8 *)tmpBuf)[(i * 32) + 24] == 0x90) {
			file_name[pspcount] = toU32(tmpBuf + i * 32);
			file_name_len[pspcount] = toU32(tmpBuf + i * 32 + 4);
			file_offset[pspcount] = toU32(tmpBuf + i * 32 + 12);
			file_size[pspcount] = toU32(tmpBuf + i * 32 + 20);
			is_file[pspcount] = ((tmpBuf[i * 32 + 27] != 4) && file_size[pspcount]);
			pspcount++;
		}
	}

	int files_extracted = 0;

	for (i = 0; i < pspcount; i++) {
		sceIoLseek(fd, enc_start + file_name[i], PSP_SEEK_SET);
		int namesize = (file_name_len[i] + 15) & -16;
		sceIoRead(fd, tmpBuf, namesize);
		memcpy(public_key, pkgBuf + 0x70, 16);

		setiter128(public_key, file_name[i] >> 4);

		for (j = 0; j < (namesize >> 4); j++) {
#ifdef MAGICGATE
			sceMgrAESEncrypt(xor_key, public_key, 16, PSPAESKey, NULL);
#else
			AES128_ECB_encrypt(public_key, PSPAESKey, xor_key);
#endif
			xor128((u8 *)(tmpBuf + (j * 16)), (u8 *)(tmpBuf + (j * 16)), xor_key);
			iter128(public_key);
		}

		char path[256], tmp[256];
		snprintf(path, sizeof(path), PKGI_INSTALL_FOLDER "/%s/%s", gameid, tmpBuf + 15);

		char *Path = path;
		int pl = 0;

		while (Path[pl]) {
			if (Path[pl] == '/') {
				memcpy(tmp, path, pl);
				tmp[pl] = 0;

				memset(&stat, 0, sizeof(SceIoStat));

				if (sceIoGetstat(tmp, &stat) < 0)
					sceIoMkdir(tmp, 0777);
			}

			pl++;
		}

		if (is_file[i]) {
			LOG("Currently extracting: %s", path);
			files_extracted++;

			progress = sceIoLseek(fd, enc_start + file_offset[i], PSP_SEEK_SET);
			sceIoRead(fd, tmpBuf, 1024 * 1024);
			install_update_progress(path + 14, progress);

			setiter128(public_key, file_offset[i] >> 4);

			SceUID dstfd = sceIoOpen(path, 0x602, 0777);

			u32 szcheck = 0, mincheck = 0;

			LOG("%d/%d bytes", mincheck, file_size[i]);

			for (j = 0; j < (int)(file_size[i] >> 4); j++) {
				if (szcheck == 1024 * 1024) {
					szcheck = 0;
					mincheck += 1024 * 1024;

					sceIoWrite(dstfd, tmpBuf, 1024 * 1024);
					sceIoRead(fd, tmpBuf, 1024 * 1024);
					install_update_progress(path + 14, progress + mincheck);

					LOG("%d/%d bytes", mincheck, file_size[i]);
				}

#ifdef MAGICGATE
				sceMgrAESEncrypt(xor_key, public_key, 16, PSPAESKey, NULL);
#else
				AES128_ECB_encrypt(public_key, PSPAESKey, xor_key);
#endif
				xor128((u8 *)((tmpBuf + (j * 16)) - mincheck), (u8 *)((tmpBuf + (j * 16)) - mincheck), xor_key);
				iter128(public_key);

				szcheck += 16;
			}

			if (mincheck < file_size[i])
			{
				sceIoWrite(dstfd, tmpBuf, file_size[i] - mincheck);
				install_update_progress(path + 14, progress + file_size[i]);
			}

			sceIoClose(dstfd);
/*
			int pathlen = strlen(path);

			if (!strcmp(path + pathlen - 9, "EBOOT.PBP")) {
				dstfd = sceIoOpen(path, PSP_O_RDONLY, 0777);
				sceIoLseek(dstfd, 0x24, PSP_SEEK_SET);
				u32 psar;
				sceIoRead(dstfd, &psar, 4);
				sceIoLseek(dstfd, psar, PSP_SEEK_SET);
				u8 block[16];
				sceIoRead(dstfd, block, 16);

				if (!memcmp(block, "PSTITLE", 7))
					sceIoLseek(dstfd, psar + 0x200, PSP_SEEK_SET);
				else if (!memcmp(block, "PSISO", 5))
					sceIoLseek(dstfd, psar + 0x400, PSP_SEEK_SET);

				sceIoRead(dstfd, block, 4);

				if (!memcmp(block, "\x00PGD", 4)) {
//					dumpPS1key(path);
					keysbin = 1;
					LOG("PS1 KEYS.BIN dumped");
				}

				sceIoClose(dstfd);
			}
*/
		}
	}

	install_update_progress(file + 9, fdsize);
	sceIoClose(fd);
	free(tmpBuf);

	LOG("files extracted: %d", files_extracted);
	LOG("Installation complete");

	return 1;
}
