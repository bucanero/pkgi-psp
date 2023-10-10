#pragma once

#include <stdint.h>
#include "pkgi_db.h"

#define PKGI_RAP_SIZE 16

int pkgi_download(const DbItem* item);
char * pkgi_http_download_buffer(const char* url, uint32_t* buf_size);

void progress_screen_refresh(void);
void update_install_progress(const char *filename, int64_t progress);
int install_psp_pkg(const char *file);
int convert_psp_pkg_iso(const char* pkg_arg, int cso);
