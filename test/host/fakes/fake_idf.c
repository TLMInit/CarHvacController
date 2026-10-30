/* Host fakes for the few ESP-IDF calls the tested firmware files make. */
#include <string.h>
#include "nvs.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "bootloader_random.h"

#define MAX_NS 8
#define MAX_ENT 128
enum { T_U16 = 1, T_U32, T_I32, T_STR, T_BLOB };
typedef struct { char ns[16]; } ns_t;
typedef struct { int used, ns, type; char key[16]; uint32_t v; char s[64]; } ent_t;
static ns_t s_ns[MAX_NS];
static int s_nns;
static ent_t s_ent[MAX_ENT];
static esp_err_t s_fail_open;
int fake_restart_count;

void fake_nvs_reset(void) { memset(s_ns, 0, sizeof(s_ns)); memset(s_ent, 0, sizeof(s_ent)); s_nns = 0; s_fail_open = ESP_OK; fake_restart_count = 0; }
void fake_nvs_fail_open(esp_err_t err) { s_fail_open = err; }
int fake_nvs_count(const char *ns)
{
    int n = 0;
    for (int i = 0; i < s_nns; i++) {
        if (strcmp(s_ns[i].ns, ns) == 0) {
            for (int j = 0; j < MAX_ENT; j++) n += s_ent[j].used && s_ent[j].ns == i;
        }
    }
    return n;
}
esp_err_t nvs_open(const char *ns, nvs_open_mode_t mode, nvs_handle_t *out)
{
    (void)mode;
    if (s_fail_open != ESP_OK) { esp_err_t e = s_fail_open; s_fail_open = ESP_OK; return e; }
    for (int i = 0; i < s_nns; i++) if (strcmp(s_ns[i].ns, ns) == 0) { *out = (nvs_handle_t)(i + 1); return ESP_OK; }
    if (s_nns >= MAX_NS || strlen(ns) > 15) return ESP_ERR_NO_MEM;
    strcpy(s_ns[s_nns].ns, ns);
    *out = (nvs_handle_t)(++s_nns);
    return ESP_OK;
}
void nvs_close(nvs_handle_t h) { (void)h; }
esp_err_t nvs_commit(nvs_handle_t h) { (void)h; return ESP_OK; }
esp_err_t nvs_erase_all(nvs_handle_t h)
{
    for (int j = 0; j < MAX_ENT; j++) if (s_ent[j].used && s_ent[j].ns == (int)h - 1) s_ent[j].used = 0;
    return ESP_OK;
}
static ent_t *find(nvs_handle_t h, const char *key)
{
    for (int j = 0; j < MAX_ENT; j++) if (s_ent[j].used && s_ent[j].ns == (int)h - 1 && strcmp(s_ent[j].key, key) == 0) return &s_ent[j];
    return NULL;
}
esp_err_t nvs_erase_key(nvs_handle_t h, const char *key)
{
    ent_t *e = find(h, key);
    if (!e) return ESP_ERR_NVS_NOT_FOUND;
    e->used = 0;
    return ESP_OK;
}
static esp_err_t put(nvs_handle_t h, const char *key, int type, uint32_t v, const char *s)
{
    if (strlen(key) > 15) return ESP_ERR_INVALID_ARG;      /* real NVS key limit */
    ent_t *e = find(h, key);
    if (!e) {
        for (int j = 0; j < MAX_ENT && !e; j++) if (!s_ent[j].used) e = &s_ent[j];
        if (!e) return ESP_ERR_NO_MEM;
        memset(e, 0, sizeof(*e));
        e->used = 1; e->ns = (int)h - 1; strcpy(e->key, key);
    }
    e->type = type; e->v = v;
    if (s) { strncpy(e->s, s, sizeof(e->s) - 1); e->s[sizeof(e->s) - 1] = 0; }
    return ESP_OK;
}
static esp_err_t getv(nvs_handle_t h, const char *key, int type, uint32_t *v)
{
    ent_t *e = find(h, key);
    if (!e) return ESP_ERR_NVS_NOT_FOUND;
    if (e->type != type) return ESP_ERR_NVS_TYPE_MISMATCH;
    *v = e->v;
    return ESP_OK;
}
esp_err_t nvs_get_u16(nvs_handle_t h, const char *k, uint16_t *v) { uint32_t t; esp_err_t e = getv(h, k, T_U16, &t); if (e == ESP_OK) *v = (uint16_t)t; return e; }
esp_err_t nvs_set_u16(nvs_handle_t h, const char *k, uint16_t v) { return put(h, k, T_U16, v, NULL); }
esp_err_t nvs_get_u32(nvs_handle_t h, const char *k, uint32_t *v) { return getv(h, k, T_U32, v); }
esp_err_t nvs_set_u32(nvs_handle_t h, const char *k, uint32_t v) { return put(h, k, T_U32, v, NULL); }
esp_err_t nvs_get_i32(nvs_handle_t h, const char *k, int32_t *v) { uint32_t t; esp_err_t e = getv(h, k, T_I32, &t); if (e == ESP_OK) *v = (int32_t)t; return e; }
esp_err_t nvs_set_i32(nvs_handle_t h, const char *k, int32_t v) { return put(h, k, T_I32, (uint32_t)v, NULL); }
esp_err_t nvs_set_str(nvs_handle_t h, const char *k, const char *v) { return put(h, k, T_STR, 0, v); }
esp_err_t nvs_get_str(nvs_handle_t h, const char *k, char *out, size_t *len)
{
    ent_t *e = find(h, k);
    if (!e) return ESP_ERR_NVS_NOT_FOUND;
    if (e->type != T_STR) return ESP_ERR_NVS_TYPE_MISMATCH;
    size_t n = strlen(e->s) + 1;
    if (out && *len < n) { *len = n; return ESP_ERR_INVALID_SIZE; }
    if (out) memcpy(out, e->s, n);
    *len = n;
    return ESP_OK;
}

/* Blobs up to 64 bytes: bytes in s, length in v */
esp_err_t nvs_set_blob(nvs_handle_t h, const char *k, const void *v, size_t len)
{
    if (len > sizeof(s_ent[0].s)) return ESP_ERR_NVS_VALUE_TOO_LONG;
    esp_err_t err = put(h, k, T_BLOB, (uint32_t)len, NULL);
    if (err == ESP_OK) memcpy(find(h, k)->s, v, len);
    return err;
}
esp_err_t nvs_get_blob(nvs_handle_t h, const char *k, void *out, size_t *len)
{
    ent_t *e = find(h, k);
    if (!e) return ESP_ERR_NVS_NOT_FOUND;
    if (e->type != T_BLOB) return ESP_ERR_NVS_TYPE_MISMATCH;
    if (out && *len < e->v) { *len = e->v; return ESP_ERR_INVALID_SIZE; }
    if (out) memcpy(out, e->s, e->v);
    *len = e->v;
    return ESP_OK;
}

void esp_restart(void) { fake_restart_count++; }
int64_t esp_timer_get_time(void) { return 0; }
void esp_fill_random(void *buf, size_t len) { memset(buf, 0x55, len); }
void bootloader_random_enable(void) {}
void bootloader_random_disable(void) {}
