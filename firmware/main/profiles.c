/* Vehicle profiles: built into the firmware, with optional extras and
 * overrides from the SD card (/sdcard/canary/profiles/<id>.json).
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "app.h"

#include "cJSON.h"

#include <dirent.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    const char *id;
    const char *json;
} fw_profile;
extern const fw_profile g_fw_profiles[];
extern const int g_fw_profile_count;

#define SD_DIR "/sdcard/canary/profiles"

static bool valid_id(const char *id)
{
    if (!*id || strlen(id) > 39) return false;
    for (const char *c = id; *c; c++)
        if (!((*c >= 'a' && *c <= 'z') || (*c >= '0' && *c <= '9') || *c == '-' || *c == '_')) return false;
    return true;
}

static void index_entry(cn_jw *w, const char *id, const char *json, bool user)
{
    cJSON *d = cJSON_Parse(json);
    const cJSON *name = cJSON_GetObjectItemCaseSensitive(d, "name"), *desc = cJSON_GetObjectItemCaseSensitive(d, "description");
    cn_jw_obj(w);
    cn_jw_kstr(w, "id", id);
    cn_jw_kstr(w, "name", cJSON_IsString(name) ? name->valuestring : id);
    cn_jw_kstr(w, "description", cJSON_IsString(desc) ? desc->valuestring : "");
    cn_jw_kbool(w, "user", user);
    cn_jw_end_obj(w);
    cJSON_Delete(d);
}

char *fw_profile_loader(void *ctx, const char *id)
{
    (void)ctx;
    if (!strcmp(id, CN_PROFILE_INDEX)) {
        cn_buf b;
        cn_buf_init(&b);
        cn_jw w;
        cn_jw_init(&w, &b);
        cn_jw_arr(&w);
        for (int i = 0; i < g_fw_profile_count; i++) index_entry(&w, g_fw_profiles[i].id, g_fw_profiles[i].json, false);
        DIR *d = sd_mounted() ? opendir(SD_DIR) : NULL;
        struct dirent *de;
        while (d && (de = readdir(d))) {
            char pid[40];
            size_t n = strlen(de->d_name);
            if (n < 6 || n - 5 >= sizeof(pid) || strcmp(de->d_name + n - 5, ".json") != 0) continue;
            memcpy(pid, de->d_name, n - 5);
            pid[n - 5] = 0;
            bool builtin = false;
            for (int i = 0; i < g_fw_profile_count; i++)
                if (!strcmp(g_fw_profiles[i].id, pid)) builtin = true;
            if (builtin || !valid_id(pid)) continue; /* an override of a built-in keeps its place in the list */
            char path[320];
            snprintf(path, sizeof(path), SD_DIR "/%s", de->d_name);
            char *json = cn_read_file(path, NULL);
            if (json) index_entry(&w, pid, json, true);
            free(json);
        }
        if (d) closedir(d);
        cn_jw_end_arr(&w);
        return cn_buf_take(&b);
    }
    if (!valid_id(id)) return NULL;
    if (sd_mounted()) {
        char path[120];
        snprintf(path, sizeof(path), SD_DIR "/%s.json", id);
        char *json = cn_read_file(path, NULL);
        if (json) return json;
    }
    for (int i = 0; i < g_fw_profile_count; i++)
        if (!strcmp(g_fw_profiles[i].id, id)) return strdup(g_fw_profiles[i].json);
    return NULL;
}
