#pragma once

#include <stdint.h>
#include "pkgi_db.h"

#define PKGI_RAP_SIZE 16

int pkgi_download(const DbItem* item);
char * pkgi_http_download_buffer(const char* url, uint32_t* buf_size);
