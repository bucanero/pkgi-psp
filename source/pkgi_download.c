#include "pkgi_download.h"
#include "pkgi_dialog.h"
#include "pkgi.h"
#include "pkgi_utils.h"
#include "pkgi_sha256.h"

#include <sys/stat.h>
#include <sys/file.h>
#include <stdio.h>
#include <stdlib.h>
#include <dirent.h>
#include <mini18n.h>


static char root[256];
static char resume_file[256];

static pkgi_http* http;
static const DbItem* db_item;
static int download_resume;

static uint64_t initial_offset;  // where http download resumes
static uint64_t download_offset; // pkg absolute offset
static uint64_t download_size;   // pkg total size (from http request)

static mbedtls_sha256_context sha;

static void* item_file;     // current file handle
static char item_name[256]; // current file name
static char item_path[256]; // current file path

void progress_screen_refresh(void);
int install_psp_pkg(const char *file);

// pkg header
static uint64_t total_size;

// UI stuff
static char dialog_extra[256];
static char dialog_eta[256];
static uint32_t info_start;
static uint32_t info_update;


static void calculate_eta(uint32_t speed)
{
    uint64_t seconds = (download_size - download_offset) / speed;
    if (seconds < 60)
    {
        pkgi_snprintf(dialog_eta, sizeof(dialog_eta), "%s: %us", _("ETA"), (uint32_t)seconds);
    }
    else if (seconds < 3600)
    {
        pkgi_snprintf(dialog_eta, sizeof(dialog_eta), "%s: %um %02us", _("ETA"), (uint32_t)(seconds / 60), (uint32_t)(seconds % 60));
    }
    else
    {
        uint32_t hours = (uint32_t)(seconds / 3600);
        uint32_t minutes = (uint32_t)((seconds - hours * 3600) / 60);
        pkgi_snprintf(dialog_eta, sizeof(dialog_eta), "%s: %uh %02um", _("ETA"), hours, minutes);
    }
}

/* follow the CURLOPT_XFERINFOFUNCTION callback definition */
static int update_progress(void *p, int64_t dltotal, int64_t dlnow, int64_t ultotal, int64_t ulnow)
{
    uint32_t info_now = pkgi_time_msec();

    if (info_now >= info_update)
    {
        char text[256];
        pkgi_snprintf(text, sizeof(text), "%s", item_name);

        if (download_resume)
        {
            // if resuming download, then there is no "download speed"
            dialog_extra[0] = 0;
        }
        else
        {
            // report download speed
            uint32_t speed = (uint32_t)(((download_offset - initial_offset) * 1000) / (info_now - info_start));
            if (speed > 10 * 1000 * 1024)
            {
                pkgi_snprintf(dialog_extra, sizeof(dialog_extra), "%u %s/s", speed / 1024 / 1024, _("MB"));
            }
            else if (speed > 1000)
            {
                pkgi_snprintf(dialog_extra, sizeof(dialog_extra), "%u %s/s", speed / 1024, _("KB"));
            }

            if (speed != 0)
            {
                // report ETA
                calculate_eta(speed);
            }
        }

        float percent;
        if (download_resume)
        {
            // if resuming, then we may not know download size yet, use total_size from pkg header
            percent = total_size ? (float)((double)download_offset / total_size) : 0.f;
        }
        else
        {
            // when downloading use content length from http response as download size
            percent = download_size ? (float)((double)download_offset / download_size) : 0.f;
        }

        pkgi_dialog_update_progress(text, dialog_extra, dialog_eta, percent);
        info_update = info_now + 500;
    }
    progress_screen_refresh();

    return (pkgi_dialog_is_cancelled());
}

static size_t write_verify_data(void *buffer, size_t size, size_t nmemb, void *stream)
{
    size_t realsize = size * nmemb;

    if (pkgi_write(item_file, buffer, realsize))
    {
        download_offset += realsize;
        mbedtls_sha256_update(&sha, buffer, realsize);
        return (realsize);
    }

    return 0;
}

static void download_start(void)
{
    LOG("resuming pkg download from %llu offset", initial_offset);
    download_offset = initial_offset;
    download_resume = 0;
    info_update = pkgi_time_msec() + 1000;
    pkgi_dialog_set_progress_title(_("Downloading..."));
}

static int download_data(void)
{
    if (!http)
    {
        LOG("requesting %s @ %llu", db_item->url, initial_offset);
        http = pkgi_http_get(db_item->url, db_item->content, initial_offset);
        if (!http)
        {
            pkgi_dialog_error(_("Could not send HTTP request"));
            return 0;
        }

        int64_t http_length;
        if (!pkgi_http_response_length(http, &http_length))
        {
            pkgi_dialog_error(_("HTTP request failed"));
            return 0;
        }
        if (http_length < 0)
        {
            pkgi_dialog_error(_("HTTP response has unknown length"));
            return 0;
        }

        download_size = http_length;
        total_size = initial_offset + download_size;

        if (!pkgi_check_free_space(http_length))
        {
            LOG("error! out of space");
            return 0;
        }

        LOG("http response length = %lld, total pkg size = %llu", http_length, total_size);
        info_start = pkgi_time_msec();
        info_update = pkgi_time_msec() + 500;
    }

    if (!pkgi_http_read(http, &write_verify_data, &update_progress))
    {
        pkgi_save(resume_file, &sha, sizeof(sha));

        if (!pkgi_dialog_is_cancelled())
        {
            pkgi_dialog_error(_("HTTP download error"));
        }
        return 0;
    }

    return 1;
}

// this includes creating of all the parent folders necessary to actually create file
static int create_file(void)
{
    char folder[256];
    pkgi_strncpy(folder, sizeof(folder), item_path);
    char* last = pkgi_strrchr(folder, '/');
    *last = 0;

    if (!pkgi_mkdirs(folder))
    {
        char error[256];
        pkgi_snprintf(error, sizeof(error), "%s %s", _("cannot create folder"), folder);
        pkgi_dialog_error(error);
        return 0;
    }

    LOG("creating %s file", item_name);
    item_file = pkgi_create(item_path);
    if (!item_file)
    {
        char error[256];
        pkgi_snprintf(error, sizeof(error), "%s %s", _("cannot create file"), item_name);
        pkgi_dialog_error(error);
        return 0;
    }

    return 1;
}

static int resume_partial_file(void)
{
    LOG("resuming %s file", item_name);
    item_file = pkgi_append(item_path);
    if (!item_file)
    {
        char error[256];
        pkgi_snprintf(error, sizeof(error), "%s %s", _("cannot resume file"), item_name);
        pkgi_dialog_error(error);
        return 0;
    }

    return 1;
}

static int download_pkg_file(void)
{
    int result = 0;

    pkgi_strncpy(item_name, sizeof(item_name), root);
    pkgi_snprintf(item_path, sizeof(item_path), "%s/%s", pkgi_get_temp_folder(), root);
    LOG("downloading %s", item_name);

    if (download_resume)
    {
        initial_offset = pkgi_get_size(item_path);
        if (!resume_partial_file()) goto bail;
        download_start();
    }
    else
    {
        if (!create_file()) goto bail;
    }

    if (!download_data()) goto bail;

    LOG("%s downloaded", item_path);
    result = 1;

bail:
    if (item_file != NULL)
    {
        pkgi_close(item_file);
        item_file = NULL;
    }
    return result;
}

static int check_integrity(const uint8_t* digest)
{
    if (!digest)
    {
        LOG("no integrity provided, skipping check");
        return 1;
    }

    uint8_t check[SHA256_DIGEST_SIZE];
    mbedtls_sha256_finish(&sha, check);

    LOG("checking integrity of pkg");
    if (!pkgi_memequ(digest, check, SHA256_DIGEST_SIZE))
    {
        LOG("pkg integrity is wrong, removing %s & resume data", item_path);

        pkgi_rm(item_path);
        pkgi_rm(resume_file);

        pkgi_dialog_error(_("pkg integrity failed, try downloading again"));
        return 0;
    }

    LOG("pkg integrity check succeeded");
    return 1;
}

static int create_rap(const char* contentid, const uint8_t* rap)
{
    LOG("creating %s.rap", contentid);
    pkgi_dialog_update_progress(_("Creating RAP file"), NULL, NULL, 1.f);

    char path[256];
    pkgi_snprintf(path, sizeof(path), "%s/%s.rap", PKGI_RAP_FOLDER, contentid);

    if (!pkgi_save(path, rap, PKGI_RAP_SIZE))
    {
        char error[256];
        pkgi_snprintf(error, sizeof(error), "%s %s.rap", _("Cannot save"), contentid);
        pkgi_dialog_error(error);
        return 0;
    }

    LOG("RAP file created");
    return 1;
}

int pkgi_download(const DbItem* item, const int background_dl)
{
    int result = 0;

    pkgi_snprintf(root, sizeof(root), "%s.pkg", item->content);
    LOG("package installation file: %s", root);

    pkgi_snprintf(resume_file, sizeof(resume_file), "%s/%s.resume", pkgi_get_temp_folder(), item->content);
    if (pkgi_load(resume_file, &sha, sizeof(sha)) == sizeof(sha))
    {
        LOG("resume file exists, trying to resume");
        pkgi_dialog_set_progress_title(_("Resuming..."));
        download_resume = 1;
    }
    else
    {
        LOG("cannot load resume file, starting download from scratch");
        pkgi_dialog_set_progress_title(background_dl ? _("Adding background task...") : _("Downloading..."));
        download_resume = 0;
        mbedtls_sha256_init(&sha);
        mbedtls_sha256_starts(&sha, 0);
    }

    http = NULL;
    item_file = NULL;
    download_size = 0;
    download_offset = 0;
    initial_offset = 0;
    db_item = item;

    dialog_extra[0] = 0;
    dialog_eta[0] = 0;
    info_start = pkgi_time_msec();
    info_update = info_start + 1000;

    if (item->rap)
    {
        if (!create_rap(item->content, item->rap)) goto finish;
//        if (!create_rif(item->content, item->rap)) goto finish;
    }

//    pkgi_dialog_update_progress(_("Downloading icon"), NULL, NULL, 1.f);
//    if (!pkgi_download_icon(item->content)) goto finish;

    if (!download_pkg_file()) goto finish;
    if (!check_integrity(item->digest)) goto finish;

    pkgi_rm(resume_file);
    result = 1;

finish:
    if (http)
    {
        pkgi_http_close(http);
    }

    return result;
}

void install_update_progress(const char *filename, int64_t progress)
{
    download_offset = progress;
    pkgi_strncpy(item_name, sizeof(item_name), filename);
    update_progress(NULL, 0, 0, 0, 0);
}

int pkgi_install(const char *titleid)
{
    char pkg_path[256];

    pkgi_snprintf(pkg_path, sizeof(pkg_path), "%s/%s", pkgi_get_temp_folder(), root);
    download_size = pkgi_get_size(pkg_path);
    info_start = pkgi_time_msec();

    initial_offset = 0;
    download_offset = 0;
    total_size = download_size;

    return (install_psp_pkg(pkg_path));
}
