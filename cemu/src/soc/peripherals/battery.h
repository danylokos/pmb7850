/* PMB7850 battery/analog input model. */
#ifndef CEMU_PERIPH_BATTERY_H
#define CEMU_PERIPH_BATTERY_H

#include <stddef.h>
#include <stdint.h>
#include "devices.h"
#include "peripheral.h"

typedef struct {
    int available;
    uint8_t level;
    uint8_t charging;
    uint16_t millivolts;
    int16_t adc_raw;
    uint8_t battery_channel;
    uint8_t reference_channel;
    uint8_t adc_result_word;
    const battery_curve_point_t *curve;
    size_t n_curve;
    int16_t adc_raw_high;
    uint16_t adc_mv_high;
    int16_t adc_raw_low;
    uint16_t adc_mv_low;
    uint64_t samples;
    uint64_t state_changes;
} battery_state_t;

void cemu_battery_periph_init(peripheral_t *p, battery_state_t *st,
                         const device_battery_config_t *cfg);
int cemu_battery_available(const peripheral_t *p);
unsigned cemu_battery_level(const peripheral_t *p);
int cemu_battery_charging(const peripheral_t *p);
uint16_t cemu_battery_millivolts(const peripheral_t *p);
int16_t cemu_battery_adc_raw(const peripheral_t *p);
int cemu_battery_set_state(peripheral_t *p, struct soc *s, unsigned level,
                      int charging, const char *source);
void cemu_battery_restore_state(peripheral_t *p, unsigned level, int charging);

#endif /* CEMU_PERIPH_BATTERY_H */
