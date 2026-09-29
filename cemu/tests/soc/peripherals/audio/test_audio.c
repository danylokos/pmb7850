#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "soc.h"
#include "synth.h"

static int failures;
#define CHECK(condition) do { \
    if (!(condition)) { \
        failures++; \
        printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
    } \
} while (0)

static void set_cc_mode(soc_t *soc, int channel, int mode) {
    uint16_t current = bus_read16(&soc->bus, 0xFF52u);
    int shift = (channel & 3) * 4;
    uint16_t mask = (uint16_t)(0xFu << shift);
    bus_write16(&soc->bus, 0xFF52u,
                (uint16_t)((current & ~mask) | ((mode & 7) << shift)));
}

int main(void) {
    static const char *const qualified[] = {"a52", "a55", "c55", "m55", "a60", "c60"};
    for (size_t device_index = 0;
         device_index < sizeof qualified / sizeof qualified[0];
         device_index++) {
        const device_config_t *device = cemu_device_by_name(
            qualified[device_index]);
        size_t flash_size = 0;
        for (int i = 0; device && i < device->flash.nchips; i++)
            flash_size += device->flash.chips[i].chip_size;
        uint8_t *flash = malloc(flash_size);
        CHECK(device != NULL && flash != NULL);
        if (!device || !flash) continue;
        memset(flash, 0xFF, flash_size);
        flash[0] = 0xFA; flash[1] = 0x80;
        flash[2] = 0xC4; flash[3] = 0x2F;

        soc_t soc;
        CHECK(cemu_soc_init(&soc, flash, flash_size, device,
                            synth_defaults(), 0).code == CEMU_STATUS_OK);
        CHECK(cemu_soc_audio_available(&soc));
        CHECK(soc.speaker_periph ==
              soc.peripherals[soc.n_peripherals - 1]);
        cemu_speaker_state_t *speaker = soc.speaker_periph->state;

        bus_write16(&soc.bus, 0xFF50u, 0x0000); /* stop T0 */
        bus_write16(&soc.bus, 0xFE50u, 0xFFFF); /* seed */
        bus_write16(&soc.bus, 0xFE54u, 0xFFFC); /* reload */
        bus_write16(&soc.bus, 0xFE84u, 0xFFFE); /* CC2 compare */
        set_cc_mode(&soc, 2, 7);                /* compare mode 3 */
        bus_write16(&soc.bus, 0xFF50u, 0x0040); /* start, divider 8 */
        cemu_soc_tick(&soc, 40);

        CHECK(speaker->ringer_active && !speaker->ringer_level);
        CHECK(speaker->ringer_last_transition == 40);

        /* A valid CAPCOM edge on another channel is not RINGIN. */
        bus_write16(&soc.bus, 0xFF50u, 0x0000);
        set_cc_mode(&soc, 2, 0);
        bus_write16(&soc.bus, 0xFE50u, 0xFFFF);
        bus_write16(&soc.bus, 0xFE82u, 0xFFFD);
        set_cc_mode(&soc, 1, 7);
        bus_write16(&soc.bus, 0xFF50u, 0x0040);
        cemu_soc_tick(&soc, 32);
        CHECK(speaker->ringer_last_transition == 40);

        cemu_soc_free(&soc);
        free(flash);
    }

    const device_config_t *unsupported = cemu_device_by_name("a62");
    CHECK(unsupported && !unsupported->audio.capcom_ringer_present &&
          unsupported->audio.stream_profile == DEVICE_AUDIO_STREAM_XBUS_UNKNOWN1_V1);
    puts(failures ? "X55 audio routes: FAIL" : "X55 audio routes: PASS");
    return failures ? 1 : 0;
}
