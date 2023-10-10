#include "pkgi.h"
#include "pkgi_style.h"

#include <sys/stat.h>
#include <SDL2/SDL.h>

#include <pspgu.h>
#include <pspdisplay.h>
#include <pspnet.h>
#include <pspnet_inet.h>
#include <pspnet_apctl.h>
#include <psputility.h>
#include <psppower.h>
#include <pspctrl.h>
#include <pspthreadman.h>
#include <pspiofilemgr.h>

#include <unistd.h>
#include <string.h>
#include <stdio.h>

#include <curl/curl.h>

#include "libfont.h"
#include "ttf_render.h"
#include "font-8x16.h"

// Audio handle
static int32_t audio = 0;
extern const uint8_t _binary_data_haiku_s3m_start;
extern const uint8_t _binary_data_haiku_s3m_size;

static uint32_t * texture_mem;      // Pointers to texture memory
static uint32_t * free_mem;         // Pointer after last texture


#define YA2D_DEFAULT_Z 1
#define AUDIO_SAMPLES 256

#define PKGI_OSK_INPUT_LENGTH 128

#define SCE_IME_DIALOG_MAX_TITLE_LENGTH	(128)
#define SCE_IME_DIALOG_MAX_TEXT_LENGTH	(512)

#define ANALOG_CENTER       0x78
#define ANALOG_THRESHOLD    0x68
#define ANALOG_MIN          (ANALOG_CENTER - ANALOG_THRESHOLD)
#define ANALOG_MAX          (ANALOG_CENTER + ANALOG_THRESHOLD)

#define PKGI_USER_AGENT "Mozilla/5.0 (PLAYSTATION PORTABLE; 1.00)"


struct pkgi_http
{
    int used;
    uint64_t size;
    uint64_t offset;
    CURL *curl;
};

typedef struct 
{
    pkgi_texture circle;
    pkgi_texture cross;
    pkgi_texture triangle;
    pkgi_texture square;
} t_tex_buttons;

typedef struct
{
    char *memory;
    size_t size;
} curl_memory_t;


static SceLwMutexWorkarea g_dialog_lock;

static int g_ok_button;
static int g_cancel_button;
static uint32_t g_button_frame_count;
static uint64_t g_time;

static int osk_level = 0;
static uint16_t g_ime_title[SCE_IME_DIALOG_MAX_TITLE_LENGTH];
static uint16_t g_ime_text[SCE_IME_DIALOG_MAX_TEXT_LENGTH];
static uint16_t g_ime_input[SCE_IME_DIALOG_MAX_TEXT_LENGTH + 1];

static pkgi_http g_http[4];
static t_tex_buttons tex_buttons;

//static MREADER *mem_reader;
//static MODULE *module;

SDL_Window* window;                         // SDL window
SDL_Renderer* renderer;                     // SDL software renderer

char * basename (const char *filename)
{
	char *p = strrchr (filename, '/');
	return p ? p + 1 : (char *) filename;
}

int pkgi_snprintf(char* buffer, uint32_t size, const char* msg, ...)
{
    va_list args;
    va_start(args, msg);
    // TODO: why sceClibVsnprintf doesn't work here?
    int len = vsnprintf(buffer, size - 1, msg, args);
    va_end(args);
    buffer[len] = 0;
    return len;
}

void pkgi_vsnprintf(char* buffer, uint32_t size, const char* msg, va_list args)
{
    // TODO: why sceClibVsnprintf doesn't work here?
    int len = vsnprintf(buffer, size - 1, msg, args);
    buffer[len] = 0;
}

char* pkgi_strstr(const char* str, const char* sub)
{
    return strstr(str, sub);
}

int pkgi_stricontains(const char* str, const char* sub)
{
    return pkgi_strstr(str, sub) != NULL;
//    return strcasestr(str, sub) != NULL;
}

int pkgi_stricmp(const char* a, const char* b)
{
    return strcasecmp(a, b);
}

void pkgi_strncpy(char* dst, uint32_t size, const char* src)
{
    strncpy(dst, src, size);
}

char* pkgi_strrchr(const char* str, char ch)
{
    return strrchr(str, ch);
}

uint32_t pkgi_strlen(const char *str)
{
    return strlen(str);
}

int64_t pkgi_strtoll(const char* str)
{
    int64_t res = 0;
    const char* s = str;
    if (*s && *s == '-')
    {
        s++;
    }
    while (*s)
    {
        res = res * 10 + (*s - '0');
        s++;
    }

    return str[0] == '-' ? -res : res;
}

void *pkgi_malloc(uint32_t size)
{
    return malloc(size);
}

void pkgi_free(void *ptr)
{
    free(ptr);
}

void pkgi_memcpy(void* dst, const void* src, uint32_t size)
{
    memcpy(dst, src, size);
}

void pkgi_memmove(void* dst, const void* src, uint32_t size)
{
    memmove(dst, src, size);
}

int pkgi_memequ(const void* a, const void* b, uint32_t size)
{
    return memcmp(a, b, size) == 0;
}

static void pkgi_start_debug_log(void)
{
#ifdef PKGI_ENABLE_LOGGING
    dbglogger_init_mode(FILE_LOGGER, "ms0:/pkgi-psp.log", 1);
    LOG("PKGi PSP logging initialized");
#endif
}

static void pkgi_stop_debug_log(void)
{
#ifdef PKGI_ENABLE_LOGGING
    dbglogger_stop();
#endif
}

int pkgi_ok_button(void)
{
    return g_ok_button;
}

int pkgi_cancel_button(void)
{
    return g_cancel_button;
}

int pkgi_dialog_lock(void)
{
    int res = sceKernelLockLwMutex(&g_dialog_lock, 1, NULL);
    if (res != 0)
    {
        LOG("dialog lock failed error=0x%08x", res);
    }
    return (res == 0);
}

int pkgi_dialog_unlock(void)
{
    int res = sceKernelUnlockLwMutex(&g_dialog_lock, 1);
    if (res != 0)
    {
        LOG("dialog unlock failed error=0x%08x", res);
    }
    return (res == 0);
}

static int convert_to_utf16(const char* utf8, uint16_t* utf16, uint32_t available)
{
    int count = 0;
    while (*utf8)
    {
        uint8_t ch = (uint8_t)*utf8++;
        uint32_t code;
        uint32_t extra;

        if (ch < 0x80)
        {
            code = ch;
            extra = 0;
        }
        else if ((ch & 0xe0) == 0xc0)
        {
            code = ch & 31;
            extra = 1;
        }
        else if ((ch & 0xf0) == 0xe0)
        {
            code = ch & 15;
            extra = 2;
        }
        else
        {
            // TODO: this assumes there won't be invalid utf8 codepoints
            code = ch & 7;
            extra = 3;
        }

        for (uint32_t i=0; i<extra; i++)
        {
            uint8_t next = (uint8_t)*utf8++;
            if (next == 0 || (next & 0xc0) != 0x80)
            {
                goto utf16_end;
            }
            code = (code << 6) | (next & 0x3f);
        }

        if (code < 0xd800 || code >= 0xe000)
        {
            if (available < 1) goto utf16_end;
            utf16[count++] = (uint16_t)code;
            available--;
        }
        else // surrogate pair
        {
            if (available < 2) goto utf16_end;
            code -= 0x10000;
            utf16[count++] = 0xd800 | (code >> 10);
            utf16[count++] = 0xdc00 | (code & 0x3ff);
            available -= 2;
        }
    }

utf16_end:
    utf16[count]=0;
    return count;
}

static int convert_from_utf16(const uint16_t* utf16, char* utf8, uint32_t size)
{
    int count = 0;
    while (*utf16)
    {
        uint32_t code;
        uint16_t ch = *utf16++;
        if (ch < 0xd800 || ch >= 0xe000)
        {
            code = ch;
        }
        else // surrogate pair
        {
            uint16_t ch2 = *utf16++;
            if (ch < 0xdc00 || ch > 0xe000 || ch2 < 0xd800 || ch2 > 0xdc00)
            {
                goto utf8_end;
            }
            code = 0x10000 + ((ch & 0x03FF) << 10) + (ch2 & 0x03FF);
        }

        if (code < 0x80)
        {
            if (size < 1) goto utf8_end;
            utf8[count++] = (char)code;
            size--;
        }
        else if (code < 0x800)
        {
            if (size < 2) goto utf8_end;
            utf8[count++] = (char)(0xc0 | (code >> 6));
            utf8[count++] = (char)(0x80 | (code & 0x3f));
            size -= 2;
        }
        else if (code < 0x10000)
        {
            if (size < 3) goto utf8_end;
            utf8[count++] = (char)(0xe0 | (code >> 12));
            utf8[count++] = (char)(0x80 | ((code >> 6) & 0x3f));
            utf8[count++] = (char)(0x80 | (code & 0x3f));
            size -= 3;
        }
        else
        {
            if (size < 4) goto utf8_end;
            utf8[count++] = (char)(0xf0 | (code >> 18));
            utf8[count++] = (char)(0x80 | ((code >> 12) & 0x3f));
            utf8[count++] = (char)(0x80 | ((code >> 6) & 0x3f));
            utf8[count++] = (char)(0x80 | (code & 0x3f));
            size -= 4;
        }
    }

utf8_end:
    utf8[count]=0;
    return count;
}

static void ConfigureDialog(pspUtilityDialogCommon *dialog, size_t dialog_size)
{
    memset(dialog, 0, sizeof(pspUtilityDialogCommon));

    dialog->size = dialog_size;
    sceUtilityGetSystemParamInt(PSP_SYSTEMPARAM_ID_INT_LANGUAGE, &dialog->language); // Prompt language
    sceUtilityGetSystemParamInt(PSP_SYSTEMPARAM_ID_INT_UNKNOWN, &dialog->buttonSwap); // X/O button swap
    dialog->graphicsThread = 0x11;
    dialog->accessThread = 0x13;
    dialog->fontThread = 0x12;
    dialog->soundThread = 0x10;
}

void pkgi_dialog_input_text(const char* title, const char* text)
{
    int done;
    SceUtilityOskData data;
    SceUtilityOskParams params;

    osk_level = 0;
    memset(&g_ime_input, 0, sizeof(g_ime_input));
    memset(&g_ime_text, 0, sizeof(g_ime_text));
    memset(&g_ime_title, 0, sizeof(g_ime_title));

    convert_to_utf16(title, g_ime_title, PKGI_COUNTOF(g_ime_title) - 1);
    convert_to_utf16(text, g_ime_input, PKGI_COUNTOF(g_ime_input) - 1);

    memset(&data, 0, sizeof(SceUtilityOskData));
    data.language = PSP_UTILITY_OSK_LANGUAGE_DEFAULT; // Use system default for text input
    data.lines = 1;
    data.unk_24 = 1;
    data.inputtype = PSP_UTILITY_OSK_INPUTTYPE_ALL; // Allow all input types
    data.desc = g_ime_title;
    data.intext = g_ime_input;
    data.outtextlength = SCE_IME_DIALOG_MAX_TEXT_LENGTH;
    data.outtextlimit = PKGI_OSK_INPUT_LENGTH; // Limit input to 128 characters
    data.outtext = g_ime_text;

    memset(&params, 0, sizeof(SceUtilityOskParams));
    ConfigureDialog(&params.base, sizeof(SceUtilityOskParams));
    params.datacount = 1;
    params.data = &data;

    if (sceUtilityOskInitStart(&params) < 0)
        return;

    void* list = aligned_alloc(16, 0x100000);
    if (!list)
        return;

    do {
        sceGuStart(GU_DIRECT, list);
        sceGuClearColor(0xFF68260D);
        sceGuClearDepth(0);
        sceGuClear(GU_COLOR_BUFFER_BIT|GU_DEPTH_BUFFER_BIT);
        sceGuFinish();
        sceGuSync(0, 0);

        done = sceUtilityOskGetStatus();
        switch(done)
        {
            case PSP_UTILITY_DIALOG_VISIBLE:
                sceUtilityOskUpdate(1);
                break;
            
            case PSP_UTILITY_DIALOG_QUIT:
                sceUtilityOskShutdownStart();
                break;
        }

        sceDisplayWaitVblankStart();
        sceGuSwapBuffers();
    } while (done != PSP_UTILITY_DIALOG_FINISHED);

    free(list);

    if (data.result == PSP_UTILITY_OSK_RESULT_CANCELLED)
        return;

    osk_level = 1;
    return;
}

int pkgi_dialog_input_update(void)
{
    return (osk_level == 1);
}

void pkgi_dialog_input_get_text(char* text, uint32_t size)
{
    osk_level = 2;
    convert_from_utf16(g_ime_text, text, size - 1);
    LOG("input: %s", text);
}

void load_ttf_fonts(void)
{
    LOG("loading TTF fonts");
    texture_mem = malloc(256 * 8 * 4);

    if(!texture_mem)
        return; // fail!

    ResetFont();
    free_mem = (uint32_t *) AddFontFromBitmapArray((uint8_t *) console_font_8x16, (uint8_t *) texture_mem, 0, 0xFF, PKGI_FONT_WIDTH, PKGI_FONT_HEIGHT, 1, BIT7_FIRST_PIXEL);

    if (TTFLoadFont(0, PKGI_APP_FOLDER "/FONT.OTF", NULL, 0) < 0)
    {
        LOG("[ERROR] Failed to load font!");
        return;
    }

    free_mem = (uint32_t*) init_ttf_table((uint8_t*) free_mem);
}

static int Net_DisplayNetDialog(void)
{
    int ret = 0, done = 0;
    pspUtilityNetconfData data;
    struct pspUtilityNetconfAdhoc adhocparam;

    memset(&adhocparam, 0, sizeof(adhocparam));
    memset(&data, 0, sizeof(pspUtilityNetconfData));

    ConfigureDialog(&data.base, sizeof(pspUtilityNetconfData));
    data.action = PSP_NETCONF_ACTION_CONNECTAP;
    data.hotspot = 0;
    data.adhocparam = &adhocparam;

    if ((ret = sceUtilityNetconfInitStart(&data)) < 0) {
        LOG("sceUtilityNetconfInitStart() failed: 0x%08x", ret);
        return 0;
    }

    void* list = aligned_alloc(16, 0x100000);
    if (!list)
        return 0;

    while(!done)
    {
        sceGuStart(GU_DIRECT, list);
        sceGuClearColor(0xFF68260D);
        sceGuClearDepth(0);
        sceGuClear(GU_COLOR_BUFFER_BIT|GU_DEPTH_BUFFER_BIT);
        sceGuFinish();
        sceGuSync(0, 0);

        switch(sceUtilityNetconfGetStatus()) {
            case PSP_UTILITY_DIALOG_NONE:
                done = 1;
                break;

            case PSP_UTILITY_DIALOG_VISIBLE:
                if ((ret = sceUtilityNetconfUpdate(1)) < 0) {
                    LOG("sceUtilityNetconfUpdate(1) failed: 0x%08x", ret);
                    done = 1;
                }
                break;

            case PSP_UTILITY_DIALOG_QUIT:
                if ((ret = sceUtilityNetconfShutdownStart()) < 0) {
                    LOG("sceUtilityNetconfShutdownStart() failed: 0x%08x", ret);
                    done = 1;
                }
                break;

            case PSP_UTILITY_DIALOG_FINISHED:
                done = 1;
                break;

            default:
                break;
        }

        sceDisplayWaitVblankStart();
        sceGuSwapBuffers();
    }
    free(list);

    done = PSP_NET_APCTL_STATE_DISCONNECTED;
    if ((ret = sceNetApctlGetState(&done)) < 0) {
        LOG("sceNetApctlGetState() failed: 0x%08x", ret);
        return 0;
    }

    return (done == PSP_NET_APCTL_STATE_GOT_IP);
}

int psp_network_up(void)
{
    static int net_up = 0;

    if (net_up)
        return 1;

    net_up = Net_DisplayNetDialog();

    return net_up;
}

static int http_init(void)
{
	int ret = 0;

	sceUtilityLoadNetModule(PSP_NET_MODULE_COMMON);
	sceUtilityLoadNetModule(PSP_NET_MODULE_INET);
	
	if ((ret = sceNetInit(128 * 1024, 42, 4 * 1024, 42, 4 * 1024)) < 0) {
		LOG("sceNetInit() failed: 0x%08x", ret);
		return -1;
	}

	if ((ret = sceNetInetInit()) < 0) {
		LOG("sceNetInetInit() failed: 0x%08x", ret);
		return -1;
	}

	if ((ret = sceNetApctlInit(0x8000, 48)) < 0) {
		LOG("sceNetApctlInit() failed: 0x%08x", ret);
		return -1;
	}

	curl_global_init(CURL_GLOBAL_ALL);

	return 0;
}

static void http_end(void)
{
	curl_global_cleanup();

	sceNetApctlTerm();
	sceNetInetTerm();
	sceNetTerm();

	sceUtilityUnloadNetModule(PSP_NET_MODULE_INET);
	sceUtilityUnloadNetModule(PSP_NET_MODULE_COMMON);
}

static int init_video(void)
{
    // Initialize SDL functions
    LOG("Initializing SDL");
    if (SDL_Init(SDL_INIT_VIDEO) < 0)
    {
        LOG("Failed to initialize SDL: %s", SDL_GetError());
        return (-1);
    }

    // Create a window context
    LOG("Creating a window");
    window = SDL_CreateWindow("main", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, PKGI_SCREEN_WIDTH, PKGI_SCREEN_HEIGHT, SDL_WINDOW_SHOWN);
    if (!window) {
        LOG("SDL_CreateWindow: %s", SDL_GetError());
        return (-1);
    }

    // Create a renderer (OpenGL ES2)
    renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!renderer) {
        LOG("SDL_CreateRenderer: %s", SDL_GetError());
        return (-1);
    }

    return 0;
}

static int pspPadInit(void)
{
    int ret;

    sceCtrlSetSamplingCycle(0);
    ret=sceCtrlSetSamplingMode(PSP_CTRL_MODE_ANALOG);
    if (ret < 0)
    {
        LOG("sceCtrlSetSamplingMode Error 0x%8X", ret);
        return -1;
    }

    return ret;
}

void pkgi_start(void)
{
    int ret = 0;
    char temp[32];

    pkgi_start_debug_log();

    ret = sceKernelCreateLwMutex(&g_dialog_lock, "dialog_mutex", 0, 0, NULL);
    if (ret != 0) {
        LOG("mutex create error (%x)", ret);
    }

    sceUtilityGetSystemParamInt(PSP_SYSTEMPARAM_ID_INT_UNKNOWN, &ret); // X/O button swap
    if (ret == 0)
    {
        g_ok_button = PKGI_BUTTON_O;
        g_cancel_button = PKGI_BUTTON_X;
    }
    else
    {
        g_ok_button = PKGI_BUTTON_X;
        g_cancel_button = PKGI_BUTTON_O;
    }
    
    if (init_video() < 0)
    {
        LOG("[ERROR] Failed to init video!");
//        return (-1);
    }

    // initialize pad
    if (pspPadInit() < 0)
    {
        LOG("[ERROR] Failed to open pad!");
//        return (-1);
    }

    LOG("initializing Network");
    if (http_init() < 0)
    {
        LOG("[ERROR] Failed to init network!");
//        return (-1);
    }

    tex_buttons.circle   = pkgi_load_image_buffer(CIRCLE, png);
    tex_buttons.cross    = pkgi_load_image_buffer(CROSS, png);
    tex_buttons.triangle = pkgi_load_image_buffer(TRIANGLE, png);
    tex_buttons.square   = pkgi_load_image_buffer(SQUARE, png);

    load_ttf_fonts();

    SetCurrentFont(PKGI_FONT_8x16);
    SetFontSize(PKGI_FONT_WIDTH, PKGI_FONT_HEIGHT);
    SetFontZ(PKGI_FONT_Z);

    pkgi_snprintf(temp, sizeof(temp), "%s%s", pkgi_get_storage_device(), PKGI_RAP_FOLDER);
    pkgi_mkdirs(temp);

    g_time = pkgi_time_msec();
}

int pkgi_update(pkgi_input* input)
{
    SceCtrlData padData;
    uint32_t previous = input->down;

    if(sceCtrlPeekBufferPositive(&padData, 1) > 0)
    {
        if (padData.Ly < ANALOG_MIN)
            padData.Buttons |= PSP_CTRL_UP;

        if (padData.Ly > ANALOG_MAX)
            padData.Buttons |= PSP_CTRL_DOWN;

        if (padData.Lx < ANALOG_MIN)
            padData.Buttons |= PSP_CTRL_LEFT;

        if (padData.Lx > ANALOG_MAX)
            padData.Buttons |= PSP_CTRL_RIGHT;

        input->down = padData.Buttons;
    }

    input->pressed = input->down & ~previous;
    input->active = input->pressed;

    if (input->down == previous)
    {
        if (g_button_frame_count >= 10)
        {
            input->active = input->down;
        }
        g_button_frame_count++;
    }
    else
    {
        g_button_frame_count = 0;
    }

    // Clear the canvas
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 0xFF);
    SDL_RenderClear(renderer);

    uint64_t time = pkgi_time_msec();
    input->delta = time - g_time;
    g_time = time;

    return 1;
}

void pkgi_swap(void)
{
    // Propagate the updated window to the screen
    SDL_RenderPresent(renderer);
}

void pkgi_end(void)
{
    curl_global_cleanup();
    pkgi_stop_debug_log();

    pkgi_free_texture(tex_buttons.circle);
    pkgi_free_texture(tex_buttons.cross);
    pkgi_free_texture(tex_buttons.triangle);
    pkgi_free_texture(tex_buttons.square);

    // Cleanup resources
    sceKernelDeleteLwMutex(&g_dialog_lock);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    // Stop all SDL sub-systems
    SDL_Quit();
    http_end();

    //sysProcessExit(0);
}

static int is_psp_go(void)
{
    static int res = -1;

    if (res >= 0)
        return res;

    SceDevInf inf;
    SceDevctlCmd cmd;

    cmd.dev_inf = &inf;
    memset(&inf, 0, sizeof(SceDevInf));
    res = (sceIoDevctl("ef0:", SCE_PR_GETDEV, &cmd, sizeof(SceDevctlCmd), NULL, 0) < 0) ? 0 : 1;

    return res;
}

const char* pkgi_get_storage_device(void)
{
    const char* dev[2] = {"ms0:", "ef0:"};

    return dev[is_psp_go()];
}

uint64_t pkgi_get_free_space(void)
{
    SceDevInf inf;
    SceDevctlCmd cmd;
    static uint32_t t = 0;
    static uint64_t freeSize = 0;

    if (t++ % 0x1000 == 0)
    {
        cmd.dev_inf = &inf;
        memset(&inf, 0, sizeof(SceDevInf));
        sceIoDevctl(pkgi_get_storage_device(), SCE_PR_GETDEV, &cmd, sizeof(SceDevctlCmd), NULL, 0);
        freeSize = ((uint64_t) inf.freeClusters) * ((uint64_t) inf.sectorCount) * ((uint64_t) inf.sectorSize);
    }

    return (freeSize);
}

const char* pkgi_get_config_folder(void)
{
    return PKGI_APP_FOLDER;
}

const char* pkgi_get_temp_folder(void)
{
    return PKGI_TMP_FOLDER;
}

const char* pkgi_get_app_folder(void)
{
    return PKGI_APP_FOLDER;
}

int pkgi_is_incomplete(const char* titleid)
{
    char path[256];
    pkgi_snprintf(path, sizeof(path), "%s%s/%s.resume", pkgi_get_storage_device(), pkgi_get_temp_folder(), titleid);

    struct stat st;
    int res = stat(path, &st);
    return (res == 0);
}

int pkgi_dir_exists(const char* path)
{
    LOG("checking if folder %s exists", path);

    SceUID id = sceIoDopen(path);
    if (id < 0) {
        return 0;
    }
    sceIoDclose(id);
    return 1;
}

int pkgi_is_installed(const char* titleid)
{    
    char path[128];
    snprintf(path, sizeof(path), "%s%s/%s", pkgi_get_storage_device(), PKGI_INSTALL_FOLDER, titleid);

    return (pkgi_dir_exists(path));
}

uint32_t pkgi_time_msec()
{
    struct timeval tv;

    gettimeofday(&tv,NULL);
    return (((uint32_t)tv.tv_sec)*1000)+(tv.tv_usec/1000);
}

void pkgi_thread_exit()
{
    sceKernelExitThread(0);
}

void pkgi_start_thread(const char* name, pkgi_thread_entry* start)
{
    SceUID id;

    id = sceKernelCreateThread(name, (SceKernelThreadEntry) start, 0x18, 0x10000, PSP_THREAD_ATTR_USER, NULL);
	LOG("sysThreadCreate: %s (0x%08x)", name, id);

    if (id < 0)
    {
        LOG("failed to start %s thread", name);
    }
    else
    {
        sceKernelStartThread(id, 0, NULL);
    }
}

void pkgi_sleep(uint32_t msec)
{
    usleep(msec * 1000);
}

int pkgi_load(const char* name, void* data, uint32_t max)
{
    FILE* fd = fopen(name, "rb");
    if (!fd)
    {
        return -1;
    }
    
    char* data8 = data;

    int total = 0;
    
    while (max != 0)
    {
        int read = fread(data8 + total, 1, max, fd);
        if (read < 0)
        {
            total = -1;
            break;
        }
        else if (read == 0)
        {
            break;
        }
        total += read;
        max -= read;
    }

    fclose(fd);
    return total;
}

int pkgi_save(const char* name, const void* data, uint32_t size)
{
    FILE* fd = fopen(name, "wb");
    if (!fd)
    {
        return 0;
    }

    int ret = 1;
    const char* data8 = data;
    while (size != 0)
    {
        int written = fwrite(data8, 1, size, fd);
        if (written <= 0)
        {
            ret = 0;
            break;
        }
        data8 += written;
        size -= written;
    }

    fclose(fd);
    return ret;
}

void pkgi_lock_process(void)
{
    LOG("locking power button");
    if (scePowerLock(0) < 0)
    {
        LOG("scePowerLock failed");
    }
}

void pkgi_unlock_process(void)
{
    LOG("unlocking power button");
    if (scePowerUnlock(0) < 0)
    {
        LOG("scePowerUnlock failed");
    }
}

pkgi_texture pkgi_load_png_raw(const void* data, uint32_t size)
{
    pkgi_texture tex = loadPngTexture(NULL, data);

    if (!tex)
    {
        LOG("failed to load texture");
    }
    return tex;
}

pkgi_texture pkgi_load_png_file(const char* filename)
{
    pkgi_texture tex = loadPngTexture(filename, NULL);

    if (!tex)
    {
        LOG("failed to load texture file %s", filename);
    }
    return tex;
}

void pkgi_draw_texture(pkgi_texture texture, int x, int y)
{
    DrawTexture(texture, x, y, YA2D_DEFAULT_Z, texture->width, texture->height, SDL_ALPHA_OPAQUE);
}

void pkgi_draw_background(pkgi_texture texture)
{
    DrawTexture(texture, 0, 0, YA2D_DEFAULT_Z, PKGI_SCREEN_WIDTH, PKGI_SCREEN_HEIGHT, SDL_ALPHA_OPAQUE);
}

void pkgi_draw_texture_z(pkgi_texture texture, int x, int y, int z, float scale)
{
    DrawTexture(texture, x, y, z, texture->width * scale, texture->height * scale, SDL_ALPHA_OPAQUE);
}

void pkgi_free_texture(pkgi_texture texture)
{
    SDL_DestroyTexture(texture->texture);
    free(texture);
}

void pkgi_clip_set(int x, int y, int w, int h)
{
    set_ttf_window(x, y, w, h*2, 0);
}

void pkgi_clip_remove(void)
{
    set_ttf_window(0, 0, PKGI_SCREEN_WIDTH, PKGI_SCREEN_HEIGHT, 0);
}

void pkgi_draw_fill_rect(int x, int y, int w, int h, uint32_t color)
{
    SDL_Rect dialog_rect = { x, y, w, h };

    SDL_SetRenderDrawColor(renderer, RGBA_R(color), RGBA_G(color), RGBA_B(color), SDL_ALPHA_OPAQUE);
    SDL_RenderFillRect(renderer, &dialog_rect);
}

void pkgi_draw_fill_rect_z(int x, int y, int z, int w, int h, uint32_t color)
{
    pkgi_draw_fill_rect(x, y, w, h, color);
}

void pkgi_draw_rect(int x, int y, int w, int h, uint32_t color)
{
    SDL_Rect dialog_rect = { x, y, w, h };

    SDL_SetRenderDrawColor(renderer, RGBA_R(color), RGBA_G(color), RGBA_B(color), SDL_ALPHA_OPAQUE);
    SDL_RenderDrawRect(renderer, &dialog_rect);
}

void pkgi_draw_rect_z(int x, int y, int z, int w, int h, uint32_t color)
{
    pkgi_draw_rect(x, y, w, h, color);
}

void pkgi_draw_text_z(int x, int y, int z, uint32_t color, const char* text)
{
    int i=x, j=y;
    SetFontColor(RGBA_COLOR(color, 255), 0);
    while (*text) {
        switch(*text) {
            case '\n':
                i = x;
                j += PKGI_FONT_HEIGHT;
                text++;
                continue;
            case '\xfa':
                pkgi_draw_texture(tex_buttons.circle, i, j);
                i += PKGI_FONT_WIDTH;
                text++;
                continue;
            case '\xfb':
                pkgi_draw_texture(tex_buttons.cross, i, j);
                i += PKGI_FONT_WIDTH;
                text++;
                continue;
            case '\xfc':
                pkgi_draw_texture(tex_buttons.triangle, i, j);
                i += PKGI_FONT_WIDTH;
                text++;
                continue;
            case '\xfd':
                pkgi_draw_texture(tex_buttons.square, i, j);
                i += PKGI_FONT_WIDTH;
                text++;
                continue;
        }
        
        DrawChar(i, j, z, (uint8_t) *text);
        i += PKGI_FONT_WIDTH;
        text++; 
    }    
}


void pkgi_draw_text_ttf(int x, int y, int z, uint32_t color, const char* text)
{
    Z_ttf = z;
    display_ttf_string(x+PKGI_FONT_SHADOW, y+PKGI_FONT_SHADOW, text, RGBA_COLOR(PKGI_COLOR_TEXT_SHADOW, 128), 0, PKGI_FONT_WIDTH+6, PKGI_FONT_HEIGHT+2, NULL);
    display_ttf_string(x, y, text, RGBA_COLOR(color, 255), 0, PKGI_FONT_WIDTH+6, PKGI_FONT_HEIGHT+2, NULL);
}

int pkgi_text_width_ttf(const char* text)
{
    return (display_ttf_string(0, 0, text, 0, 0, PKGI_FONT_WIDTH+6, PKGI_FONT_HEIGHT+2, NULL));
}


void pkgi_draw_text(int x, int y, uint32_t color, const char* text)
{
    SetFontColor(RGBA_COLOR(PKGI_COLOR_TEXT_SHADOW, 200), 0);
    DrawStringMono((float)x+PKGI_FONT_SHADOW, (float)y+PKGI_FONT_SHADOW, (char *)text);

    SetFontColor(RGBA_COLOR(color, 255), 0);
    DrawStringMono((float)x, (float)y, (char *)text);
}


int pkgi_text_width(const char* text)
{
    return (strlen(text) * PKGI_FONT_WIDTH) + PKGI_FONT_SHADOW;
}

int pkgi_text_height(const char* text)
{
    return PKGI_FONT_HEIGHT + PKGI_FONT_SHADOW+1;
}

int pkgi_validate_url(const char* url)
{
    if (url[0] == 0)
    {
        return 0;
    }
    if ((pkgi_strstr(url, "http://") == url) || (pkgi_strstr(url, "https://") == url) ||
        (pkgi_strstr(url, "ftp://") == url)  || (pkgi_strstr(url, "ftps://") == url))
    {
        return 1;
    }
    return 0;
}

static void pkgi_curl_init(CURL *curl)
{
    union SceNetApctlInfo proxy_info;

    // Set user agent string
    curl_easy_setopt(curl, CURLOPT_USERAGENT, PKGI_USER_AGENT);
    // don't verify the certificate's name against host
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    // don't verify the peer's SSL certificate
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    // Set SSL VERSION to TLS 1.2
    curl_easy_setopt(curl, CURLOPT_SSLVERSION, CURL_SSLVERSION_TLSv1_2);
    // Set timeout for the connection to build
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 20L);
    // Follow redirects
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    // maximum number of redirects allowed
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 20L);
    // Fail the request if the HTTP code returned is equal to or larger than 400
    curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
    // request using SSL for the FTP transfer if available
    curl_easy_setopt(curl, CURLOPT_USE_SSL, CURLUSESSL_TRY);

    // check for proxy settings
    memset(&proxy_info, 0, sizeof(proxy_info));
    sceNetApctlGetInfo(PSP_NET_APCTL_INFO_USE_PROXY, &proxy_info);
    
    if (proxy_info.useProxy)
    {
        memset(&proxy_info, 0, sizeof(proxy_info));
        sceNetApctlGetInfo(PSP_NET_APCTL_INFO_PROXY_URL, &proxy_info);
        curl_easy_setopt(curl, CURLOPT_PROXY, proxy_info.proxyUrl);
        curl_easy_setopt(curl, CURLOPT_PROXYTYPE, CURLPROXY_HTTP);

        memset(&proxy_info, 0, sizeof(proxy_info));
        sceNetApctlGetInfo(PSP_NET_APCTL_INFO_PROXY_PORT, &proxy_info);
        curl_easy_setopt(curl, CURLOPT_PROXYPORT, proxy_info.proxyPort);
    }
}

pkgi_http* pkgi_http_get(const char* url, const char* content, uint64_t offset)
{
    LOG("http get");

    if (!pkgi_validate_url(url))
    {
        LOG("unsupported URL (%s)", url);
        return NULL;
    }

    pkgi_http* http = NULL;
    for (size_t i = 0; i < 4; i++)
    {
        if (g_http[i].used == 0)
        {
            http = &g_http[i];
            break;
        }
    }

    if (!http)
    {
        LOG("too many simultaneous http requests");
        return NULL;
    }

    http->curl = curl_easy_init();
    if (!http->curl)
    {
        LOG("curl init error");
        return NULL;
    }

    pkgi_curl_init(http->curl);
    curl_easy_setopt(http->curl, CURLOPT_URL, url);

    LOG("starting http GET request for %s", url);

    if (offset != 0)
    {
        LOG("setting http offset %ld", offset);
        /* resuming upload at this position */
        curl_easy_setopt(http->curl, CURLOPT_RESUME_FROM_LARGE, (curl_off_t) offset);
    }

    http->used = 1;
    return(http);
}

int pkgi_http_response_length(pkgi_http* http, int64_t* length)
{
    CURLcode res;

    // do the download request without getting the body
    curl_easy_setopt(http->curl, CURLOPT_NOBODY, 1L);
    curl_easy_setopt(http->curl, CURLOPT_NOPROGRESS, 1L);

    // Perform the request
    res = curl_easy_perform(http->curl);

    if(res != CURLE_OK)
    {
        LOG("curl_easy_perform() failed: %s", curl_easy_strerror(res));
        return 0;
    }

    long status = 0;
    curl_easy_getinfo(http->curl, CURLINFO_RESPONSE_CODE, &status);
    LOG("http status code = %d", status);

    curl_easy_getinfo(http->curl, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, length);
    LOG("http response length = %llu", *length);
    http->size = *length;

    return 1;
}

int pkgi_http_read(pkgi_http* http, void* write_func, void* xferinfo_func)
{
    CURLcode res;

    curl_easy_setopt(http->curl, CURLOPT_NOBODY, 0L);
    // The function that will be used to write the data
    curl_easy_setopt(http->curl, CURLOPT_WRITEFUNCTION, write_func);
    // The data file descriptor which will be written to
    curl_easy_setopt(http->curl, CURLOPT_WRITEDATA, NULL);

    if (xferinfo_func)
    {
        /* pass the struct pointer into the xferinfo function */
        curl_easy_setopt(http->curl, CURLOPT_XFERINFOFUNCTION, xferinfo_func);
        curl_easy_setopt(http->curl, CURLOPT_XFERINFODATA, NULL);
        curl_easy_setopt(http->curl, CURLOPT_NOPROGRESS, 0L);
    }

    // Perform the request
    res = curl_easy_perform(http->curl);

    if(res != CURLE_OK)
    {
        LOG("curl_easy_perform() failed: %s", curl_easy_strerror(res));
        return 0;
    }

    return 1;
}

void pkgi_http_close(pkgi_http* http)
{
    LOG("http close");
    curl_easy_cleanup(http->curl);

    http->used = 0;
}

int pkgi_mkdirs(const char* dir)
{
    char path[256];
    pkgi_snprintf(path, sizeof(path), "%s", dir);
    LOG("pkgi_mkdirs for %s", path);
    char* ptr = path;
    ptr++;
    while (*ptr)
    {
        while (*ptr && *ptr != '/')
        {
            ptr++;
        }
        char last = *ptr;
        *ptr = 0;

        if (!pkgi_dir_exists(path))
        {
            LOG("mkdir %s", path);
            int err = mkdir(path, 0777);
            if (err < 0)
            {
                LOG("mkdir %s err=0x%08x", path, (uint32_t)err);
                return 0;
            }
        }
        
        *ptr++ = last;
        if (last == 0)
        {
            break;
        }
    }

    return 1;
}

void pkgi_rm(const char* file)
{
    struct stat sb;
    if (stat(file, &sb) == 0) {
        LOG("removing file %s", file);

        int err = sceIoRemove(file);
        if (err < 0)
        {
            LOG("error removing %s file, err=0x%08x", err);
        }
    }
}

int64_t pkgi_get_size(const char* path)
{
    struct stat st;
    int err = stat(path, &st);
    if (err < 0)
    {
        LOG("cannot get size of %s, err=0x%08x", path, err);
        return -1;
    }
    return st.st_size;
}

void* pkgi_create(const char* path)
{
    LOG("fopen create on %s", path);
    FILE* fd = fopen(path, "wb");
    if (!fd)
    {
        LOG("cannot create %s, err=0x%08x", path, fd);
        return NULL;
    }
    LOG("fopen returned fd=%d", fd);

    return (void*)fd;
}

void* pkgi_open(const char* path)
{
    LOG("fopen open rb on %s", path);
    FILE* fd = fopen(path, "rb");
    if (!fd)
    {
        LOG("cannot open %s, err=0x%08x", path, fd);
        return NULL;
    }
    LOG("fopen returned fd=%d", fd);

    return (void*)fd;
}

void* pkgi_append(const char* path)
{
    LOG("fopen append on %s", path);
    FILE* fd = fopen(path, "ab");
    if (!fd)
    {
        LOG("cannot append %s, err=0x%08x", path, fd);
        return NULL;
    }
    LOG("fopen returned fd=%d", fd);

    return (void*)fd;
}

int pkgi_read(void* f, void* buffer, uint32_t size)
{
    LOG("asking to read %u bytes", size);
    size_t read = fread(buffer, 1, size, (FILE*)f);
    if (read < 0)
    {
        LOG("fread error 0x%08x", read);
    }
    else
    {
        LOG("read %d bytes", read);
    }
    return read;
}

int pkgi_write(void* f, const void* buffer, uint32_t size)
{
//    LOG("asking to write %u bytes", size);
    size_t write = fwrite(buffer, size, 1, (FILE*)f);
    if (write < 0)
    {
        LOG("fwrite error 0x%08x", write);
    }
    else
    {
//        LOG("wrote %d bytes", write);
    }
    return (write == 1);
}

void pkgi_close(void* f)
{
    FILE *fd = (FILE*)f;
    LOG("closing file %d", fd);
    int err = fclose(fd);
    if (err < 0)
    {
        LOG("close error 0x%08x", err);
    }
}

static size_t curl_write_memory(void *contents, size_t size, size_t nmemb, void *userp)
{
    size_t realsize = size * nmemb;
    curl_memory_t *mem = (curl_memory_t *)userp;

    char *ptr = realloc(mem->memory, mem->size + realsize + 1);
    if(!ptr)
    {
        /* out of memory! */
        LOG("not enough memory (realloc)");
        return 0;
    }

    mem->memory = ptr;
    memcpy(&(mem->memory[mem->size]), contents, realsize);
    mem->size += realsize;
    mem->memory[mem->size] = 0;

    return realsize;
}

char * pkgi_http_download_buffer(const char* url, uint32_t* buf_size)
{
    CURL *curl;
    CURLcode res;
    curl_memory_t chunk;

    curl = curl_easy_init();
    if(!curl)
    {
        LOG("cURL init error");
        return NULL;
    }
    
    chunk.memory = malloc(1);   /* will be grown as needed by the realloc above */
    chunk.size = 0;             /* no data at this point */

    pkgi_curl_init(curl);
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 1L);
    // The function that will be used to write the data
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_memory);
    // The data file descriptor which will be written to
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&chunk);

    // Perform the request
    res = curl_easy_perform(curl);

    if(res != CURLE_OK)
    {
        LOG("curl_easy_perform() failed: %s", curl_easy_strerror(res));
        curl_easy_cleanup(curl);
        free(chunk.memory);
        return NULL;
    }

    LOG("%lu bytes retrieved", (unsigned long)chunk.size);
    // clean-up
    curl_easy_cleanup(curl);

    *buf_size = chunk.size;
    return (chunk.memory);
}

const char * pkgi_get_user_language(void)
{
    int language;

    // Prompt language
    if(sceUtilityGetSystemParamInt(PSP_SYSTEMPARAM_ID_INT_LANGUAGE, &language) < 0)
        return "en";

    switch (language)
    {
    case PSP_SYSTEMPARAM_LANGUAGE_JAPANESE:             //  0   Japanese
        return "ja";

    case PSP_SYSTEMPARAM_LANGUAGE_ENGLISH:              //  1   English (United States)
        return "en";

    case PSP_SYSTEMPARAM_LANGUAGE_FRENCH:               //  2   French
        return "fr";

    case PSP_SYSTEMPARAM_LANGUAGE_SPANISH:              //  3   Spanish
        return "es";

    case PSP_SYSTEMPARAM_LANGUAGE_GERMAN:               //  4   German
        return "de";

    case PSP_SYSTEMPARAM_LANGUAGE_ITALIAN:              //  5   Italian
        return "it";

    case PSP_SYSTEMPARAM_LANGUAGE_DUTCH:                //  6   Dutch
        return "nl";

    case PSP_SYSTEMPARAM_LANGUAGE_RUSSIAN:              //  8   Russian
        return "ru";

    case PSP_SYSTEMPARAM_LANGUAGE_KOREAN:               //  9   Korean
        return "ko";

    case PSP_SYSTEMPARAM_LANGUAGE_CHINESE_TRADITIONAL:  // 10   Chinese (traditional)
    case PSP_SYSTEMPARAM_LANGUAGE_CHINESE_SIMPLIFIED:   // 11   Chinese (simplified)
        return "ch";

    case PSP_SYSTEMPARAM_LANGUAGE_PORTUGUESE:           //  7   Portuguese (Portugal)
        return "pt";

    default:
        break;
    }

    return "en";
}
