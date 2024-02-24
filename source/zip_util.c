#include <zip.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <sys/stat.h>
#include <dirent.h>

#include "pkgi.h"
#include "pkgi_download.h"

#define UNZIP_BUF_SIZE 0x20000

static inline uint64_t min64(uint64_t a, uint64_t b)
{
    return a < b ? a : b;
}

int extract_zip(const char* zip_file)
{
	char path[256];
	uint8_t* buffer;
	int64_t zsize = pkgi_get_size(zip_file);
	struct zip* archive = zip_open(zip_file, ZIP_RDONLY | ZIP_CHECKCONS, NULL);
	int files = zip_get_num_files(archive);

	if (files <= 0) {
		LOG("Empty ZIP file.");
		zip_close(archive);
		return 0;
	}

	buffer = malloc(UNZIP_BUF_SIZE);
	if (!buffer)
		return 0;

	LOG("Extracting %s to <%s>...", zip_file, dest_path);

	for (int i = 0; i < files; i++) {
		const char* filename = zip_get_name(archive, i, 0);

		update_install_progress(filename, (zsize * i)/files);
		LOG("Unzip [%d/%d] '%s'...", i+1, files, filename);

		if (!filename)
			continue;

		if (filename[0] == '/')
			filename++;

		if (strncasecmp(filename, "PSP/GAME/", 9) == 0)
			filename += 9;

		snprintf(path, sizeof(path)-1, "%s/PSP/GAME/%s", pkgi_get_storage_device(), filename);
		char* slash = strrchr(path, '/');
		*slash = 0;
		pkgi_mkdirs(path);
		*slash = '/';

		if (filename[strlen(filename) - 1] == '/')
			continue;

		struct zip_stat st;
		if (zip_stat_index(archive, i, 0, &st)) {
			LOG("Unable to access file %s in zip.", filename);
			continue;
		}
		struct zip_file* zfd = zip_fopen_index(archive, i, 0);
		if (!zfd) {
			LOG("Unable to open file %s in zip.", filename);
			continue;
		}

		FILE* tfd = fopen(path, "wb");
		if(!tfd) {
			free(buffer);
			zip_fclose(zfd);
			zip_close(archive);
			LOG("Error opening temporary file '%s'.", path);
			return 0;
		}

		uint64_t pos = 0, count;
		while (pos < st.size) {
			count = min64(UNZIP_BUF_SIZE, st.size - pos);
			if (zip_fread(zfd, buffer, count) != count) {
				free(buffer);
				fclose(tfd);
				zip_fclose(zfd);
				zip_close(archive);
				LOG("Error reading from zip.");
				return 0;
			}

			fwrite(buffer, count, 1, tfd);
			pos += count;
		}

		zip_fclose(zfd);
		fclose(tfd);

		update_install_progress(NULL, zsize * (i+1)/files);
	}

	if (archive) {
		zip_close(archive);
	}

	update_install_progress(NULL, zsize);
	free(buffer);

	return files;
}
