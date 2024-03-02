#include "pkgi_db.h"
#include "pkgi_config.h"
#include "pkgi_utils.h"
#include "pkgi_sha256.h"
#include "pkgi.h"
#include "pkgi_download.h"

#include <stddef.h>
#include <mini18n.h>
#include <stdlib.h>
#include <dirent.h>
#include <string.h>

#define MAX_DB_SIZE (4*1024*1024)
#define MAX_DB_ITEMS 0x4000
#define MAX_DB_COLUMNS 32

#define EXTDB_ID_LENGTH  110
#define EXTDB_ID_SHA256  "\x7c\xb2\xf4\x8c\x8f\x8b\x4e\xf0\xfa\x1b\x8e\x7c\x03\x82\xc4\x33\xf9\xe9\x5c\x85\x21\xd3\xac\x6f\xad\x5c\x1c\x9f\x33\xf7\xcb\xc8"
#define EXTDB2_ID_SHA256 "\x72\x55\xb4\xce\x97\x59\x5a\xb6\x66\x6a\xc9\x80\x58\xd3\x22\x95\x8d\x9c\x33\x6a\xbd\x25\x21\x43\x79\x10\xb7\x98\x06\x0e\x40\x85"
#define EXTDB3_ID_SHA256 "\x56\x1a\x55\x30\xd5\xad\xfd\x00\x3c\x40\x42\x6a\xe2\x79\x30\xd6\xcc\xc0\x93\xbd\x1c\xf6\x43\xe7\x9c\x74\xf1\xfb\x8e\xf9\xf4\x7c"

static char* db_data = NULL;
static uint32_t db_total;
static uint32_t db_size;

static DbItem db[MAX_DB_ITEMS];
static uint32_t db_count;

static DbItem* db_item[MAX_DB_ITEMS];
static uint32_t db_item_count;

typedef enum {
    ColumnContentId,
    ColumnContentType,
    ColumnName,
    ColumnDescription,
    ColumnRap,
    ColumnUrl,
    ColumnSize,
    ColumnChecksum,
    ColumnUnknown
} ColumnType;

typedef struct {
    ColumnType type;
    const char* text_id;
    const char* data;
} ColumnEntry;

typedef struct {
    char delimiter;
    uint8_t total_columns;
    ColumnType* type;
    ColumnEntry* data;
} dbFormat;

static ColumnEntry entries[] =
{
    { ColumnContentId, "contentid", "" },
    { ColumnContentType, "type", "" },
    { ColumnName, "name", "" },
    { ColumnDescription, "description", "" },
    { ColumnRap, "rap", "" },
    { ColumnUrl, "url", "" },
    { ColumnSize, "size", "" },
    { ColumnChecksum, "checksum", "" },
};

static const ColumnType default_format[] =
{
    ColumnContentId,
    ColumnContentType,
    ColumnName,
    ColumnDescription,
    ColumnRap,
    ColumnUrl,
    ColumnSize,
    ColumnChecksum
};

static const ColumnType external_format[] =
{
    ColumnUnknown,
    ColumnUnknown,
    ColumnUnknown,
    ColumnName,
    ColumnUrl,
    ColumnContentId,
    ColumnUnknown,
    ColumnRap,
    ColumnUnknown,
    ColumnSize,
    ColumnChecksum
};

static const ColumnType external_format2[] =
{
    ColumnUnknown,
    ColumnUnknown,
    ColumnName,
    ColumnUrl,
    ColumnContentId,
    ColumnUnknown,
    ColumnRap,
    ColumnUnknown,
    ColumnSize,
    ColumnChecksum
};

static const ColumnType external_format3[] =
{
    ColumnUnknown,
    ColumnUnknown,
    ColumnName,
    ColumnUrl,
    ColumnContentId,
    ColumnUnknown,
    ColumnUnknown,
    ColumnSize,
    ColumnChecksum
};

static uint8_t hexvalue(char ch)
{
    if (ch >= '0' && ch <= '9')
    {
        return ch - '0';
    }
    else if (ch >= 'a' && ch <= 'f')
    {
        return ch - 'a' + 10;
    }
    else if (ch >= 'A' && ch <= 'F')
    {
        return ch - 'A' + 10;
    }
    return 0;
}

static uint8_t* pkgi_hexbytes(const char* digest, uint32_t length)
{
    uint8_t* result = (uint8_t*)digest;

    for (uint32_t i = 0; i < length; i++)
    {
        char ch1 = digest[2 * i];
        char ch2 = digest[2 * i + 1];
        if (ch1 == 0 || ch2 == 0)
        {
            return NULL;
        }

        result[i] = hexvalue(ch1) * 16 + hexvalue(ch2);
    }

    return result;
}

static char* generate_contentid(void)
{
    char* cid = (char*)pkgi_malloc(37);
    pkgi_snprintf(cid, 36, "X00000-X%08d_00-0000000000000000", db_count);
    return cid;
}

static size_t write_update_data(void *buffer, size_t size, size_t nmemb, void *stream)
{
    size_t realsize = size * nmemb;

    pkgi_memcpy(db_data + db_size, buffer, realsize);
    db_size += realsize;

    return (realsize);
}

static int update_database(const char* update_url, const char* path, char* error, uint32_t error_size)
{
    db_total = 0;
    db_size = 0;
    LOG("downloading update from %s", update_url);

    pkgi_http* http = pkgi_http_get(update_url, NULL, 0);
    if (!http)
    {
        pkgi_snprintf(error, error_size, "%s\n%s", _("failed to download list from"), update_url);
        return 0;
    }
    else
    {
        int64_t length;
        if (!pkgi_http_response_length(http, &length))
        {
            pkgi_snprintf(error, error_size, "%s\n%s", _("failed to download list from"), update_url);
        }
        else
        {
            if (length > (int64_t)(MAX_DB_SIZE - 1))
            {
                pkgi_snprintf(error, error_size, _("list is too large... check for newer pkgi version!"));
            }
            else if (length != 0)
            {
                db_total = (uint32_t)length;
                error[0] = 0;

                if (!pkgi_http_read(http, &write_update_data, NULL))
                {
                    pkgi_snprintf(error, error_size, "%s", _("HTTP download error"));
                    db_size = 0;
                }
            }

            if (error[0] == 0 && db_size == 0)
            {
                pkgi_snprintf(error, error_size, _("list is empty... check the DB server"));
            }
        }

        pkgi_http_close(http);

        if (db_size == 0)
        {
            return 0;
        }
        else
        {
            pkgi_save(path, db_data, db_size);
        }
    }
    return 1;
}

static int load_database(uint8_t db_id)
{
    uint8_t column = 0;
    dbFormat dbf = { ',', 8, (ColumnType*)default_format, entries };

    char path[256];
    pkgi_snprintf(path, sizeof(path), "%s/dbformat.txt", pkgi_get_config_folder());

    int loaded = pkgi_load(path, db_data, MAX_DB_SIZE - 1);
    if (loaded > 0)
    {
        char* ptr = db_data;
        char* end = db_data + loaded + 1;
        column = 0;
        ColumnType types[MAX_DB_COLUMNS];

        LOG("loading format from %s", path);

        dbf.delimiter = *ptr++;

        if (ptr < end && *ptr == '\r')
        {
            ptr++;
        }
        if (ptr < end && *ptr == '\n')
        {
            ptr++;
        }

        while (ptr < end && *ptr)
        {
            const char* column_name = ptr;
            types[column] = ColumnUnknown;

            while (ptr < end && *ptr != dbf.delimiter && *ptr != '\n' && *ptr != '\r' && column < MAX_DB_COLUMNS)
            {
                ptr++;
            }
            *ptr++ = 0;

            int j;
            for (j = 0; j < 8; j++) {
                if (pkgi_stricmp(entries[j].text_id, column_name) == 0) {
                    types[column] = entries[j].type;
                }
            }
        
            column++;
        }
        dbf.total_columns = column;
        dbf.type = types;
    }

    pkgi_snprintf(path, sizeof(path), "%s/pkgi%s.txt", pkgi_get_config_folder(), pkgi_content_tag(db_id));

    LOG("loading database from %s", path);
    
    loaded = pkgi_load(path, db_data+db_size, MAX_DB_SIZE - 1);
    if (loaded > 0)
    {
        uint8_t check[SHA256_DIGEST_SIZE];

        mbedtls_sha256((uint8_t*)db_data+db_size, EXTDB_ID_LENGTH, check, 0);

        if (pkgi_memequ(EXTDB_ID_SHA256, check, SHA256_DIGEST_SIZE))
        {
            dbf.delimiter = '\t';
            dbf.total_columns = PKGI_COUNTOF(external_format);
            dbf.type = (ColumnType*) external_format;
        }
        else if (pkgi_memequ(EXTDB2_ID_SHA256, check, SHA256_DIGEST_SIZE))
        {
            dbf.delimiter = '\t';
            dbf.total_columns = PKGI_COUNTOF(external_format2);
            dbf.type = (ColumnType*) external_format2;
        }
        else
        {
            mbedtls_sha256((uint8_t*)db_data+db_size, EXTDB_ID_LENGTH - 9, check, 0);
            if (pkgi_memequ(EXTDB3_ID_SHA256, check, SHA256_DIGEST_SIZE))
            {
                dbf.delimiter = '\t';
                dbf.total_columns = PKGI_COUNTOF(external_format3);
                dbf.type = (ColumnType*) external_format3;
            }
        }
    }
    else
    {
        return 0;
    }

    LOG("parsing items (%d bytes)", loaded);

    db_size += loaded;
    db_data[db_size] = '\n';
    char* ptr = db_data + db_size - loaded;
    char* end = db_data + db_size + 1;

    if (db_size > 3 && (uint8_t)ptr[0] == 0xef && (uint8_t)ptr[1] == 0xbb && (uint8_t)ptr[2] == 0xbf)
    {
        ptr += 3;
    }

    while (ptr < end && *ptr)
    {
        column = 0;
        while (ptr < end && column < dbf.total_columns)
        {
            const char* content = ptr;

            while (ptr < end && *ptr != dbf.delimiter && *ptr != '\n' && *ptr != '\r')
            {
                ptr++;
            }
            *ptr++ = 0;

            dbf.data[dbf.type[column]].data = content;
            column++;
        }

        if (column == dbf.total_columns && pkgi_validate_url(dbf.data[ColumnUrl].data))
        {
            uint32_t ctype = (uint32_t)pkgi_strtoll(dbf.data[ColumnContentType].data);
            // contentid can't be empty, let's generate one
            db[db_count].content = (dbf.data[ColumnContentId].data[0] == 0 ? generate_contentid() : dbf.data[ColumnContentId].data);
            db[db_count].type = pkgi_get_content_type(ctype == 0 ? db_id : ctype);
            db[db_count].name = dbf.data[ColumnName].data;
            db[db_count].description = dbf.data[ColumnDescription].data;
            db[db_count].rap = pkgi_hexbytes(dbf.data[ColumnRap].data, PKGI_RAP_SIZE);
            db[db_count].url = dbf.data[ColumnUrl].data;
            db[db_count].size = pkgi_strtoll(dbf.data[ColumnSize].data);
            db[db_count].digest = pkgi_hexbytes(dbf.data[ColumnChecksum].data, SHA256_DIGEST_SIZE);
            db_item[db_count] = db + db_count;
            db_count++;
        }

        if (db_count == MAX_DB_ITEMS)
        {
            break;
        }

        if (ptr < end && *ptr == '\r')
        {
            ptr++;
        }
        if (ptr < end && *ptr == '\n')
        {
            ptr++;
        }
    }

    LOG("finished parsing, %u total items", (db_count - db_item_count));

    db_item_count = db_count;

    return 1;
}

static void scan_local_packages(void)
{
    DIR* d;
    void* fp;
    struct dirent *dirp;
    int64_t fsize;
    char buf[256];

    pkgi_snprintf(buf, sizeof(buf), "%s%s", pkgi_get_storage_device(), pkgi_get_temp_folder());
    if ((d = opendir(buf)) == NULL)
        return;

	while ((dirp = readdir(d)) != NULL)
	{
        pkgi_snprintf(buf, sizeof(buf), "%s%s/%s", pkgi_get_storage_device(), pkgi_get_temp_folder(), dirp->d_name);
        fsize = pkgi_get_size(buf);
        if ((fp = pkgi_open(buf)) == NULL)
            continue;

        pkgi_read(fp, buf, 0x80);
        pkgi_close(fp);

        if (!pkgi_memequ(buf, "\x7FPKG\x80\x00\x00\x02", 8) || fsize != get32be(buf + 0x1C))
            continue;

        memset(&db[db_count], 0, sizeof(DbItem));
        db[db_count].content = strdup(buf + 0x30);
        db[db_count].type = ContentLocal;
        db[db_count].name = strdup(dirp->d_name);
        db[db_count].size = fsize;
        db[db_count].url = db[db_count].name;
        db[db_count].description = db[db_count].name + pkgi_strlen(dirp->d_name);
        db_item[db_count] = &db[db_count];
        db_count++;
    }
    closedir(d);
}

int pkgi_db_update(const char* update_url, uint32_t update_len, char* error, uint32_t error_size)
{
    char path[256];

    if (!db_data)
    {
        return 0;
    }

    for (int i = 0; i < MAX_CONTENT_TYPES; i++)
    {
        const char* tmp_url = update_url + update_len*i;

        if (tmp_url[0] != 0)
        {
            pkgi_snprintf(path, sizeof(path), "%s/pkgi%s.txt", pkgi_get_config_folder(), pkgi_content_tag(i));
            update_database(tmp_url, path, error, error_size);
        }
    }

    return 1;
}

int pkgi_db_reload(char* error, uint32_t error_size)
{
    char path[256];

    db_total = 0;
    db_size = 0;
    db_count = 0;
    db_item_count = 0;

    if (!db_data && (db_data = malloc(MAX_DB_SIZE)) == NULL)
    {
        pkgi_snprintf(error, error_size, "failed to allocate memory for database");
        return 0;
    }

    for (int i = 0; i < ContentLocal; i++)
    {
        pkgi_snprintf(path, sizeof(path), "%s/pkgi%s.txt", pkgi_get_config_folder(), pkgi_content_tag(i));

        if (pkgi_get_size(path) > 0)
        {
            load_database(i);
        }
    }

    scan_local_packages();
    LOG("finished db update, %u total items", db_count);

    if (db_count == 0)
    {
        pkgi_snprintf(error, error_size, _("ERROR: pkgi.txt file(s) missing or bad config.txt file"));
        return 0;
    }
    return 1;
}

static void swap(uint32_t a, uint32_t b)
{
    DbItem* temp = db_item[a];
    db_item[a] = db_item[b];
    db_item[b] = temp;
}

static int matches(GameRegion region, ContentType content, uint32_t filter)
{
    return ((region == RegionASA && (filter & DbFilterRegionASA))
        || (region == RegionEUR && (filter & DbFilterRegionEUR))
        || (region == RegionJPN && (filter & DbFilterRegionJPN))
        || (region == RegionUSA && (filter & DbFilterRegionUSA))
        || (region == RegionUnknown))

        && ((content == ContentGame && (filter & DbFilterContentGame))
        || (content == ContentDLC && (filter & DbFilterContentDLC))
        || (content == ContentTheme && (filter & DbFilterContentTheme))
        || (content == ContentPSX && (filter & DbFilterContentPSX))
        || (content == ContentDemo && (filter & DbFilterContentDemo))
        || (content == ContentUpdate && (filter & DbFilterContentUpdate))
        || (content == ContentEmulator && (filter & DbFilterContentEmulator))
        || (content == ContentApp && (filter & DbFilterContentApp))
        || (content == ContentLocal && (filter & DbFilterContentLocal))
        || (content == ContentUnknown));
}

static int lower(const DbItem* a, const DbItem* b, DbSort sort, DbSortOrder order, uint32_t filter)
{
    GameRegion reg_a = pkgi_get_region(a->content);
    GameRegion reg_b = pkgi_get_region(b->content);

    int cmp = 0;
    if (sort == SortByTitle)
    {
        cmp = pkgi_stricmp(a->content + 7, b->content + 7) < 0;
    }
    else if (sort == SortByRegion)
    {
        cmp = reg_a == reg_b ? pkgi_stricmp(a->content + 7, b->content + 7) < 0 : reg_a < reg_b;
    }
    else if (sort == SortByName)
    {
        cmp = pkgi_stricmp(a->name, b->name) < 0;
    }
    else if (sort == SortBySize)
    {
        cmp = a->size < b->size;
    }

    int matches_a = matches(reg_a, a->type, filter);
    int matches_b = matches(reg_b, b->type, filter);

    if (matches_a == matches_b)
    {
        return order == SortAscending ? cmp : !cmp;
    }
    else if (matches_a)
    {
        return 1;
    }
    else
    {
        return 0;
    }
}

static void heapify(uint32_t n, uint32_t index, DbSort sort, DbSortOrder order, uint32_t filter)
{
    uint32_t largest = index;
    uint32_t left = 2 * index + 1;
    uint32_t right = 2 * index + 2;

    if (left < n && lower(db_item[largest], db_item[left], sort, order, filter))
    {
        largest = left;
    }

    if (right < n && lower(db_item[largest], db_item[right], sort, order, filter))
    {
        largest = right;
    }

    if (largest != index)
    {
        swap(index, largest);
        heapify(n, largest, sort, order, filter);
    }
}

void pkgi_db_configure(const char* search, const Config* config)
{
    uint32_t search_count;
    if (!search)
    {
        search_count = db_count;
    }
    else
    {
        uint32_t write = 0;
        for (uint32_t read = 0; read < db_count; read++)
        {
            if (pkgi_stricontains(db_item[read]->name, search))
            {
                if (write < read)
                {
                    swap(read, write);
                }
                write++;
            }
        }
        search_count = write;
    }

    if (search_count == 0)
    {
        db_item_count = 0;
        return;
    }

    for (int i = search_count / 2 - 1; i >= 0; i--)
    {
        heapify(search_count, i, config->sort, config->order, config->filter);
    }

    for (int i = search_count - 1; i >= 0; i--)
    {
        swap(i, 0);
        heapify(i, 0, config->sort, config->order, config->filter);
    }

    if (config->filter == DbFilterAll)
    {
        db_item_count = search_count;
    }
    else
    {
        uint32_t low = 0;
        uint32_t high = search_count - 1;
        while (low <= high)
        {
            // this never overflows because of MAX_DB_ITEMS
            uint32_t middle = (low + high) / 2;

            GameRegion region = pkgi_get_region(db_item[middle]->content);
            if (matches(region, db_item[middle]->type, config->filter))
            {
                low = middle + 1;
            }
            else
            {
                if (middle == 0)
                {
                    break;
                }
                high = middle - 1;
            }
        }
        db_item_count = low;
    }
}

void pkgi_db_get_update_status(uint32_t* updated, uint32_t* total)
{
    *updated = db_size;
    *total = db_total;
}

uint32_t pkgi_db_count(void)
{
    return db_item_count;
}

uint32_t pkgi_db_total(void)
{
    return db_count;
}

DbItem* pkgi_db_get(uint32_t index)
{
    return index < db_item_count ? db_item[index] : NULL;
}

GameRegion pkgi_get_region(const char* content)
{
    switch (content[0])
    {
    case 'K':
    case 'H':
        return RegionASA;

    case 'E':
        return RegionEUR;

    case 'J':
        return RegionJPN;

    case 'U':
        return RegionUSA;

    default:
        return RegionUnknown;
    }
}

ContentType pkgi_get_content_type(uint32_t content)
{
    if (content < MAX_CONTENT_TYPES)
        return (ContentType)content;

    return ContentUnknown;
}

int pkgi_db_load_xml_updates(const char* content_id, const char* name)
{
/*
    xmlDoc *doc = NULL;
    xmlNode *root_element = NULL;
    xmlNode *cur_node = NULL;
    char *value;
    char updUrl[256];
    uint32_t size, updates = 0;

    pkgi_snprintf(updUrl, sizeof(updUrl), "https://a0.ww.np.dl.playstation.net/tpl/np/%.9s/%.9s-ver.xml", content_id + 7, content_id + 7);
    LOG("Loading update xml (%s)...", updUrl);

    char * buffer = pkgi_http_download_buffer(updUrl, &size);

    if (!buffer)
        return (-1);

    /*parse the file and get the DOM *
    doc = xmlParseMemory(buffer, size);

    if (!doc)
    {
        LOG("XML: could not parse file %s", updUrl);
        free(buffer);
        return 0;
    }

    /*Get the root element node *
    root_element = xmlDocGetRootElement(doc);
    cur_node = root_element->children;

    if (xmlStrcasecmp(cur_node->name, BAD_CAST "tag") == 0)
        cur_node = cur_node->children;

    for (; cur_node; cur_node = cur_node->next)
    {
        if (cur_node->type != XML_ELEMENT_NODE)
            continue;

        if ((xmlStrcasecmp(cur_node->name, BAD_CAST "package") == 0) && (db_count < MAX_DB_ITEMS))
        {
            memset(&db[db_count], 0, sizeof(DbItem));
            db[db_count].type = ContentUpdate;

            value = (char*) xmlGetProp(cur_node, BAD_CAST "version");
            size = pkgi_strlen(value) + 1;
            pkgi_memcpy(db_data + db_size, value, size);
            db[db_count].description = db_data + db_size;
            db_size += size;

            // append the version to content-id
            pkgi_snprintf(db_data + db_size, 1024, "%s_%s", content_id, value);
            db[db_count].content = db_data + db_size;
            db_size += (pkgi_strlen(db[db_count].content) + 1);

            pkgi_snprintf(db_data + db_size, 1024, "%s (%s)", name, value);
            db[db_count].name = db_data + db_size;
            db_size += (pkgi_strlen(db[db_count].name) + 1);

            value = (char*) xmlGetProp(cur_node, BAD_CAST "url");
            size = pkgi_strlen(value) + 1;
            pkgi_memcpy(db_data + db_size, value, size);
            db[db_count].url = db_data + db_size;
            db_size += size;

            value = (char*) xmlGetProp(cur_node, BAD_CAST "size");
            db[db_count].size = pkgi_strtoll(value);

//            value = (char*) xmlGetProp(cur_node, BAD_CAST "ps3_system_ver");
//            value = (char*) xmlGetProp(cur_node, BAD_CAST "sha1sum");
//            LOG("SHA1 (%s)", value);
//            db[db_count].digest = pkgi_hexbytes(value, SHA1_DIGEST_SIZE);

            LOG("Update: '%s' [%d] %s", db[db_count].name, db[db_count].size, db[db_count].url);

            db_item[db_count] = db + db_count;
            db_count++;
            updates++;
        }
    }

    /*free the document *
    xmlFreeDoc(doc);
    xmlCleanupParser();
    free(buffer);

    return updates;
    */
    return 0;
}
