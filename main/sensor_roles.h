/**
 * @file sensor_roles.h
 * @brief DS18B20 sensors on the node's 1-Wire bus (GPIO7), each assigned a role.
 *
 * INTERFACE (lead). Implemented by WP-C inside hvac_sensors.c (the sensors task
 * owns the bus traffic). Readers never block: everything here returns cached data.
 *
 * Per temperature cycle (1 s): one broadcast Convert T (Skip ROM), then on the next
 * cycle one Read Scratchpad (Match ROM) per assigned ROM. Readings are float degC,
 * NaN when the ROM is unassigned, absent, implausible (SN-02 rules) or older than 5 s.
 *
 * Assignments persist in NVS namespace "hvac_cfg", key "rom_roles" (blob of
 * SENSOR_ROLES_MAX_ROMS x {rom[8], role}). Migration from 2.0: if no assignment
 * exists and exactly one ROM is found, it becomes DUCT (what the single sensor was).
 * Roles are append-only: the blob stores the role number (one byte), so an older blob
 * loads unchanged, and older firmware skips a role it does not know (>= its COUNT).
 */

#ifndef SENSOR_ROLES_H
#define SENSOR_ROLES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SENSOR_ROLES_MAX_ROMS   4       /* = ONEWIRE_SEARCH_MAX_ROMS (cobs_protocol.h), asserted in hvac_sensors.c */

typedef enum {
    SENSOR_ROLE_UNUSED = 0,
    SENSOR_ROLE_DUCT,           /* duct outlet air: what 2.0 called "duct" */
    SENSOR_ROLE_COOLANT,        /* heater core / coolant line: heater delay */
    SENSOR_ROLE_OUTSIDE,        /* outside air: defog glass estimate */
    SENSOR_ROLE_EVAPORATOR,     /* evaporator fins / outlet (v2.4): freeze limits, electric compressor loop */
    SENSOR_ROLE_COUNT
} sensor_role_t;

typedef struct {
    uint8_t rom[8];
    sensor_role_t role;
    bool present;               /* answered the last search or read */
    float temp_c;               /* last plausible reading, NaN if none */
    uint32_t age_ms;            /* since that reading; UINT32_MAX if none */
} sensor_rom_info_t;

/** Ask the sensors task to run a 1-Wire search on its next cycle (non-blocking). */
void sensor_roles_request_search(void);

/** True while a requested search has not completed yet. */
bool sensor_roles_search_pending(void);

/** Snapshot of known ROMs (assigned ones and the last search's results). Returns count. */
size_t sensor_roles_get_found(sensor_rom_info_t *out, size_t max);

/** Assign a role to a ROM (UNUSED clears it); one ROM per role, persisted at once. */
esp_err_t sensor_roles_assign(const uint8_t rom[8], sensor_role_t role);

/** Temperature of the sensor with this role, NaN if none. */
float sensor_roles_get_temp(sensor_role_t role);

/** "Duct", "Coolant", "Outside", "Evaporator", "Unused" (English; the UI translates). */
const char *sensor_role_name(sensor_role_t role);

#ifdef __cplusplus
}
#endif

#endif /* SENSOR_ROLES_H */
