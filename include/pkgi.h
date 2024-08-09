#pragma once

#include <stdint.h>
#include <stdarg.h>
#include "pkgi_dialog.h"

#define PKGI_UPDATE_URL     "https://api.github.com/repos/bucanero/pkgi-psp/releases/latest"
#define PKGI_VERSION        "1.1.0"

#define PKGI_BUTTON_SELECT 0x000001
#define PKGI_BUTTON_START  0x000008
#define PKGI_BUTTON_UP     0x000010
#define PKGI_BUTTON_RIGHT  0x000020
#define PKGI_BUTTON_DOWN   0x000040
#define PKGI_BUTTON_LEFT   0x000080

#define PKGI_BUTTON_LT     0x000100 // L1
#define PKGI_BUTTON_RT     0x000200 // R1

#define PKGI_BUTTON_X      0x004000 // cross
#define PKGI_BUTTON_O      0x002000 // circle
#define PKGI_BUTTON_T      0x001000 // triangle
#define PKGI_BUTTON_S      0x008000 // square

#define PKGI_UNUSED(x) (void)(x)

#define PKGI_APP_FOLDER "."
#define PKGI_RAP_FOLDER "/PKG/RAP"
#define PKGI_TMP_FOLDER "/PKG"
#define PKGI_INSTALL_FOLDER "/PSP/GAME"


#define PKGI_COUNTOF(arr) (sizeof(arr)/sizeof(0[arr]))

#ifdef PKGI_ENABLE_LOGGING
#include <dbglogger.h>
#define LOG dbglogger_log
#else
#define LOG(...)
#endif

int pkgi_snprintf(char* buffer, uint32_t size, const char* msg, ...);
void pkgi_vsnprintf(char* buffer, uint32_t size, const char* msg, va_list args);
char* pkgi_strstr(const char* str, const char* sub);
int pkgi_stricontains(const char* str, const char* sub);
int pkgi_stricmp(const char* a, const char* b);
void pkgi_strncpy(char* dst, uint32_t size, const char* src);
char* pkgi_strrchr(const char* str, char ch);
uint32_t pkgi_strlen(const char *str);
int64_t pkgi_strtoll(const char* str);
void pkgi_memcpy(void* dst, const void* src, uint32_t size);
void pkgi_memmove(void* dst, const void* src, uint32_t size);
int pkgi_memequ(const void* a, const void* b, uint32_t size);
void* pkgi_malloc(uint32_t size);
void pkgi_free(void* ptr);

int pkgi_ok_button(void);
int pkgi_cancel_button(void);

void pkgi_start(void);
int pkgi_update(pkgi_input* input);
void pkgi_swap(void);
void pkgi_end(void);

int pkgi_get_battery_charge(int* status);
const char* pkgi_get_storage_device(void);
int pkgi_is_psp_go(uint8_t dev);

uint64_t pkgi_get_free_space(void);
const char* pkgi_get_config_folder(void);
const char* pkgi_get_temp_folder(void);
const char* pkgi_get_app_folder(void);
int pkgi_is_incomplete(const char* titleid);
int pkgi_is_installed(const char* titleid);
int pkgi_install(int iso_mode, int remove_pkg);

void pkgi_play_audio(void);
uint32_t pkgi_time_msec(void);

typedef void pkgi_thread_entry(void);
void pkgi_start_thread(const char* name, pkgi_thread_entry* start);
void pkgi_thread_exit(void);
void pkgi_sleep(uint32_t msec);

int pkgi_load(const char* name, void* data, uint32_t max);
int pkgi_save(const char* name, const void* data, uint32_t size);

void pkgi_lock_process(void);
void pkgi_unlock_process(void);

int pkgi_dialog_lock(void);
int pkgi_dialog_unlock(void);

void pkgi_dialog_input_text(const char* title, const char* text);
int pkgi_dialog_input_update(void);
void pkgi_dialog_input_get_text(char* text, uint32_t size);

int pkgi_check_free_space(uint64_t http_length);

typedef struct pkgi_http pkgi_http;

int pkgi_validate_url(const char* url);
pkgi_http* pkgi_http_get(const char* url, const char* content, uint64_t offset);
int pkgi_http_response_length(pkgi_http* http, int64_t* length);
int pkgi_http_read(pkgi_http* http, void* write_func, void* xferinfo_func);
void pkgi_http_close(pkgi_http* http);

int pkgi_mkdirs(const char* path);
void pkgi_rm(const char* file);
int64_t pkgi_get_size(const char* path);

// creates file (if it exists, truncates size to 0)
void* pkgi_create(const char* path);
// open existing file in read mode, fails if file does not exist
void* pkgi_open(const char* path);
// open file for writing, next write will append data to end of it
void* pkgi_append(const char* path);
// close file
void pkgi_close(void* f);

int pkgi_read(void* f, void* buffer, uint32_t size);
int pkgi_write(void* f, const void* buffer, uint32_t size);

// UI stuff
struct pkgi_texture_s
{
	void *texture;
	unsigned short width;
	unsigned short height;
};

typedef struct pkgi_texture_s* pkgi_texture;

#define pkgi_load_image_buffer(name, type) \
    ({ extern const uint8_t _binary_data_##name##_##type##_start; \
       extern const uint8_t _binary_data_##name##_##type##_size; \
       pkgi_load_##type##_raw((void*) &_binary_data_##name##_##type##_start , (int) &_binary_data_##name##_##type##_size); \
    })

pkgi_texture loadPngTexture(const char* path, const void* buffer);
pkgi_texture pkgi_load_png_raw(const void* data, uint32_t size);
pkgi_texture pkgi_load_png_file(const char* filename);
void pkgi_draw_background(pkgi_texture texture);
void pkgi_draw_texture(pkgi_texture texture, int x, int y);
void pkgi_draw_texture_z(pkgi_texture texture, int x, int y, int z, float scale);
void pkgi_free_texture(pkgi_texture texture);

void pkgi_clip_set(int x, int y, int w, int h);
void pkgi_clip_remove(void);
void pkgi_draw_rect(int x, int y, int w, int h, uint32_t color);
void pkgi_draw_rect_z(int x, int y, int z, int w, int h, uint32_t color);
void pkgi_draw_fill_rect(int x, int y, int w, int h, uint32_t color);
void pkgi_draw_fill_rect_z(int x, int y, int z, int w, int h, uint32_t color);
void pkgi_draw_text(int x, int y, uint32_t color, const char* text);
void pkgi_draw_text_z(int x, int y, int z, uint32_t color, const char* text);
void pkgi_draw_text_ttf(int x, int y, int z, uint32_t color, const char* text);
int pkgi_text_width(const char* text);
int pkgi_text_width_ttf(const char* text);
int pkgi_text_height(const char* text);
