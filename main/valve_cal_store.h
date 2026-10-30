/**
 * @file valve_cal_store.h
 * @brief Persisted per-valve calibration, NVS namespace "hvac_cal" (versioned via settings_ns_open).
 *
 * INTERFACE (lead). Implemented by WP-D in valve_cal_store.c. Used by the valve
 * driver at boot: load -> plausible(current ADC) -> use stored, else recalibrate
 * that valve only. The record carries its own CRC.
 */

#ifndef VALVE_CAL_STORE_H
#define VALVE_CAL_STORE_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "hvac_hw_config.h"

#ifdef __cplusplus
extern "C" {
#endif

#define VALVE_CAL_NAMESPACE     "hvac_cal"
#define VALVE_CAL_SCHEMA        1

typedef struct {
    uint8_t adc_min;            /* ADC % at the lower end of the calibrated range */
    uint8_t adc_max;
    uint8_t adc_center;         /* butterfly: fully open; linear: unused */
    uint8_t low_deadzone;       /* ADC % where the lower deadzone ends */
    uint8_t high_deadzone;      /* ADC % where the upper deadzone starts */
    bool is_inverted;           /* FWD increases ADC */
    uint8_t speed;              /* PWM used when calibrated (0 = default) */
    uint32_t boot_counter;      /* boot number when this record was written */
    uint16_t crc;               /* CRC16 over the fields above; set by save */
} valve_cal_record_t;

/** Open the namespace, bump and remember the boot counter. Call once at boot after settings_init(). */
esp_err_t valve_cal_store_init(void);

/** ESP_OK, ESP_ERR_NOT_FOUND (no record) or ESP_ERR_INVALID_CRC (corrupt record). */
esp_err_t valve_cal_store_load(hvac_valve_id_t valve, valve_cal_record_t *out);

/** Compute the CRC, stamp the boot counter and write. */
esp_err_t valve_cal_store_save(hvac_valve_id_t valve, const valve_cal_record_t *rec);

esp_err_t valve_cal_store_erase(hvac_valve_id_t valve);
esp_err_t valve_cal_store_erase_all(void);

/** Plausibility at boot: current ADC within [adc_min - 3, adc_max + 3]. Both ends mean
 *  closed, so a reading in a deadzone is fine. */
bool valve_cal_store_plausible(const valve_cal_record_t *rec, uint8_t current_adc);

/** The butterfly pot turns continuously; in its dead gap the wiper is off the track and the
 *  reading parks at the top or bottom (98 % / 0 % on the bench) for several seconds at
 *  calibration speed. True for such a reading: calibration does not count it as a stall. */
#define VALVE_POT_GAP_LOW_PCT   2
#define VALVE_POT_GAP_HIGH_PCT  96
bool valve_cal_in_pot_gap(uint8_t adc);

#ifdef __cplusplus
}
#endif

#endif /* VALVE_CAL_STORE_H */
