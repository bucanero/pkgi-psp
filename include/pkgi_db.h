#pragma once

#include <stdint.h>


typedef enum {
    PresenceUnknown,
    PresenceIncomplete,
    PresenceInstalled,
    PresenceMissing,
} DbPresence;

typedef enum {
    SortByTitle,
    SortByRegion,
    SortByName,
    SortBySize,
} DbSort;

typedef enum {
    SortAscending,
    SortDescending,
} DbSortOrder;

typedef enum {
    DbFilterRegionASA = 0x01,
    DbFilterRegionEUR = 0x02,
    DbFilterRegionJPN = 0x04,
    DbFilterRegionUSA = 0x08,

    // TODO: implement these two
    DbFilterInstalled = 0x10,
    DbFilterMissing   = 0x20,

    DbFilterContentGame     = 0x000100,
    DbFilterContentDLC      = 0x000200,
    DbFilterContentTheme    = 0x000400,
    DbFilterContentPSX      = 0x000800,
    DbFilterContentDemo     = 0x001000,
    DbFilterContentUpdate   = 0x002000,
    DbFilterContentEmulator = 0x004000,
    DbFilterContentApp      = 0x008000,
    DbFilterContentTool     = 0x010000,

    DbFilterAllRegions = DbFilterRegionUSA | DbFilterRegionEUR | DbFilterRegionJPN | DbFilterRegionASA,
    DbFilterAllContent = DbFilterContentGame | DbFilterContentDLC | DbFilterContentTheme | DbFilterContentPSX | 
                         DbFilterContentDemo | DbFilterContentUpdate | DbFilterContentEmulator | DbFilterContentApp | DbFilterContentTool,
    DbFilterAll = DbFilterAllRegions | DbFilterAllContent | DbFilterInstalled | DbFilterMissing,
} DbFilter;

typedef enum {
    ContentUnknown,
    ContentGame,
    ContentDLC,
    ContentTheme,
    ContentPSX,
    ContentDemo,
    ContentUpdate,
    ContentEmulator,
    ContentApp,
    ContentTool,
    MAX_CONTENT_TYPES
} ContentType;

typedef struct {
    DbPresence presence;
    const char* content;
    ContentType type;
    const char* name;
    const char* description;
    const uint8_t* rap;
    const char* url;
    const uint8_t* digest;
    int64_t size;
} DbItem;

typedef enum {
    RegionASA,
    RegionEUR,
    RegionJPN,
    RegionUSA,
    RegionUnknown,
} GameRegion;

typedef struct Config {
    DbSort sort;
    DbSortOrder order;
    uint32_t filter;
    uint8_t content;
    uint8_t version_check;
    uint8_t install_mode_iso;
    uint8_t keep_pkg;
    uint8_t allow_refresh;
    uint8_t storage;
    char language[3];
} Config;


int pkgi_db_reload(char* error, uint32_t error_size);
int pkgi_db_update(const char* update_url, uint32_t update_len, char* error, uint32_t error_size);
void pkgi_db_get_update_status(uint32_t* updated, uint32_t* total);
int pkgi_db_load_xml_updates(const char* content_id, const char* name);

void pkgi_db_configure(const char* search, const Config* config);

uint32_t pkgi_db_count(void);
uint32_t pkgi_db_total(void);
DbItem* pkgi_db_get(uint32_t index);

GameRegion pkgi_get_region(const char* content);
ContentType pkgi_get_content_type(uint32_t content);
