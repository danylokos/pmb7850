#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "soc.h"
#include "synth.h"
#include "xbus_unknown1.h"

#define STREAM_BASE 0xEB80u
#define STREAM_COMMAND 0xE836u

static int failures;
#define CHECK(condition) do { \
    if (!(condition)) { \
        failures++; \
        printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
    } \
} while (0)

static const uint8_t WAV_PACKET[] = {
    0xBC, 0xAD, 0, 0, 0, 0, 0, 0, 0x11, 0x11,
};

static const uint8_t SI3_PACKET[] = {
    0x01,0x91,0x10,0x18,0x14,0x73,0x14,0x14,
    0x0a,0x4d,0x72,0x52,0xf8,0xff,0xa0,0xa0,
    0xcc,0xc1,
};

static uint8_t *make_flash(size_t size) {
    uint8_t *flash = malloc(size);
    if (!flash) return NULL;
    memset(flash, 0xFF, size);
    flash[0] = 0xFA; flash[1] = 0x80;
    flash[2] = 0xC4; flash[3] = 0x2F;
    return flash;
}

static size_t flash_size(const device_config_t *device) {
    size_t size = 0;
    for (int i = 0; i < device->flash.nchips; i++)
        size += device->flash.chips[i].chip_size;
    return size;
}

static int init_soc(soc_t *soc, const device_config_t *device,
                    uint8_t **flash) {
    size_t size = flash_size(device);
    *flash = make_flash(size);
    return *flash && cemu_soc_init(soc, *flash, size, device,
                                   synth_defaults(), 0).code ==
                         CEMU_STATUS_OK;
}

static uint16_t read_word(soc_t *soc, uint32_t addr) {
    return (uint16_t)(bus_read8(&soc->bus, addr) |
                      ((uint16_t)bus_read8(&soc->bus, addr + 1u) << 8));
}

static void write_packet(soc_t *soc, const uint8_t *data, size_t size) {
    size_t i = 0;
    for (; i + 1u < size; i += 2u)
        bus_write16(&soc->bus, STREAM_BASE + (uint32_t)i,
                    (uint16_t)(data[i] | ((uint16_t)data[i + 1u] << 8)));
    if (i < size) bus_write8(&soc->bus, STREAM_BASE + (uint32_t)i, data[i]);
}

static void command(soc_t *soc, uint16_t value) {
    bus_write16(&soc->bus, STREAM_COMMAND, value);
    bus_write16(&soc->bus, 0xEF3A, 4);
    CHECK(read_word(soc, STREAM_COMMAND) == (uint16_t)(value | 0x8000u));
}

static void test_transport_ordering(void) {
    soc_t soc;
    uint8_t *flash = NULL;
    CHECK(init_soc(&soc, cemu_device_by_name("c55"), &flash));
    if (!flash) return;
    xbus_audio_state_t *stream =
        &((xbus_unknown1_state_t *)soc.xbus_unknown1_periph->state)->audio;
    cemu_speaker_state_t *speaker = soc.audio.speaker;

    /* Cold-boot DPRAM clearing is pass-through, not an audio command or
     * packet. M55 reaches this sequence before interrupts are enabled. */
    bus_write16(&soc.bus, STREAM_COMMAND, 0u);
    CHECK(read_word(&soc, STREAM_COMMAND) == 0u);
    for (uint32_t addr = STREAM_BASE; addr <= 0xEBAFu; addr += 2u)
        bus_write16(&soc.bus, addr, 0u);
    cemu_soc_tick(&soc, 32);
    CHECK(stream->stream_command == 0 && stream->stream_packets == 0 &&
          !stream->unclassified_pending);

    /* A qualifying command persists across the observed short setup gap. */
    command(&soc, 0x16u);
    cemu_soc_tick(&soc, 143);
    write_packet(&soc, SI3_PACKET, sizeof SI3_PACKET);
    cemu_soc_tick(&soc, 31);
    CHECK(read_word(&soc, STREAM_BASE) == 0x9101u);
    cemu_soc_tick(&soc, 1);
    CHECK(read_word(&soc, STREAM_BASE) == 0x5555u);
    CHECK(stream->stream_packets == 1 && stream->stream_accepted == 1);
    CHECK(stream->stream_kind[0] == XBUS_AUDIO_STREAM_SI3);
    CHECK(speaker->pcm[CEMU_AUDIO_SOURCE_XBUS].count == 216 &&
          speaker->pcm[CEMU_AUDIO_SOURCE_XBUS].has_held_frame);
    cemu_soc_tick(&soc, 1);
    CHECK(stream->stream_count == 0 && stream->stream_drained == 1);

    /* A settled packet is retained with its producer token until a command
     * classifies it, even across the measured long command gap. */
    command(&soc, 0x17u);
    write_packet(&soc, SI3_PACKET, sizeof SI3_PACKET);
    cemu_soc_tick(&soc, 32);
    CHECK(read_word(&soc, STREAM_BASE) == 0x9101u);
    CHECK(stream->unclassified_pending && stream->stream_accepted == 1);
    cemu_soc_tick(&soc, 1200000);
    CHECK(read_word(&soc, STREAM_BASE) == 0x9101u);
    command(&soc, 0x16u);
    CHECK(read_word(&soc, STREAM_BASE) == 0x5555u);
    CHECK(!stream->unclassified_pending && stream->stream_accepted == 2);
    CHECK(speaker->pcm[CEMU_AUDIO_SOURCE_XBUS].count == 216 &&
          speaker->pcm[CEMU_AUDIO_SOURCE_XBUS].has_held_frame);

    /* A new base write deterministically replaces the older unclassified
     * packet, without acknowledging either before classification. */
    cemu_soc_tick(&soc, 1);
    command(&soc, 0x17u);
    const uint8_t old_packet[] = {0xBC,0xAD,1,2,3,4};
    write_packet(&soc, old_packet, sizeof old_packet);
    cemu_soc_tick(&soc, 32);
    write_packet(&soc, SI3_PACKET, sizeof SI3_PACKET);
    cemu_soc_tick(&soc, 32);
    CHECK(stream->stream_replaced == 1);
    CHECK(stream->unclassified_length == sizeof SI3_PACKET);
    CHECK(!memcmp(stream->unclassified_data, SI3_PACKET,
                  sizeof SI3_PACKET));
    command(&soc, 0x16u);
    CHECK(read_word(&soc, STREAM_BASE) == 0x5555u);

    /* Queue ownership wraps; a full queue returns retry without decoding. */
    cemu_soc_tick(&soc, 1);
    stream->stream_head = XBUS_AUDIO_QUEUE_CAP - 1u;
    command(&soc, 0x16u);
    write_packet(&soc, SI3_PACKET, sizeof SI3_PACKET);
    cemu_soc_tick(&soc, 32);
    CHECK(stream->stream_kind[XBUS_AUDIO_QUEUE_CAP - 1u] ==
          XBUS_AUDIO_STREAM_SI3);
    command(&soc, 0x20u); /* Accepted setup does not reclassify the stream. */
    CHECK(stream->active_stream_kind == XBUS_AUDIO_STREAM_SI3);
    cemu_soc_tick(&soc, 1);
    CHECK(stream->stream_head == 0);
    stream->stream_count = XBUS_AUDIO_QUEUE_CAP;
    stream->stream_drain_deadline = 0;
    write_packet(&soc, SI3_PACKET, sizeof SI3_PACKET);
    cemu_soc_tick(&soc, 32);
    CHECK(read_word(&soc, STREAM_BASE) == 0xAAAAu);
    CHECK(stream->stream_retried == 1);

    command(&soc, 0x17u);
    CHECK(stream->stream_count == 0 && !stream->si3.voices[0].active &&
          stream->active_stream_kind == XBUS_AUDIO_STREAM_UNCLASSIFIED);
    write_packet(&soc, SI3_PACKET, sizeof SI3_PACKET);
    cemu_soc_tick(&soc, 32);
    CHECK(stream->unclassified_pending);
    cemu_soc_audio_reset(&soc);
    CHECK(!stream->unclassified_pending && stream->stream_command == 0);
    cemu_soc_free(&soc);
    free(flash);
}

static void test_device_qualification(void) {
    static const char *const qualified[] = {"a52", "a55", "c55", "m55",
        "a60", "a62", "a65", "c60", "cf62", "mc60"};
    for (size_t i = 0; i < sizeof qualified / sizeof qualified[0]; i++) {
        const device_config_t *device = cemu_device_by_name(qualified[i]);
        soc_t soc;
        uint8_t *flash = NULL;
        CHECK(device && device->audio.stream_profile ==
                           DEVICE_AUDIO_STREAM_XBUS_UNKNOWN1_V1);
        CHECK(device && device->audio.clock_hz == 26000000u);
        CHECK(device->audio.capcom_ringer_present ==
              (i < 4 || !strcmp(qualified[i], "a60") ||
                        !strcmp(qualified[i], "c60")));
        CHECK(init_soc(&soc, device, &flash));
        if (!flash) continue;
        command(&soc, 0x16u);
        write_packet(&soc, SI3_PACKET, sizeof SI3_PACKET);
        cemu_soc_tick(&soc, 32);
        CHECK(read_word(&soc, STREAM_BASE) == 0x5555u);
        uint8_t full[128];
        for (unsigned n = 0; n < sizeof full; n += 2) { full[n] = 0xcc; full[n + 1] = 0xc1; }
        write_packet(&soc, full, sizeof full);
        cemu_soc_tick(&soc, 32);
        xbus_audio_state_t *audio = &((xbus_unknown1_state_t *)soc.xbus_unknown1_periph->state)->audio;
        CHECK(audio->stream_accepted == 2 && audio->si3.quanta == 65);
        CHECK(audio->si3.malformed_packets == 0);
        cemu_soc_free(&soc);
        free(flash);
    }

    /* A matching XBUS ID alone does not qualify the hardware profile. */
    device_config_t disabled = *cemu_device_by_name("c60");
    disabled.audio = (device_audio_config_t){0};
    const device_config_t *unsupported = &disabled;
    soc_t soc;
    uint8_t *flash = NULL;
    CHECK(unsupported && (unsupported->xbus_unknown1_id == 0x1202u ||
                          unsupported->xbus_unknown1_id == 0x1203u) &&
          unsupported->audio.stream_profile == DEVICE_AUDIO_STREAM_NONE);
    CHECK(init_soc(&soc, unsupported, &flash));
    if (!flash) return;
    bus_write16(&soc.bus, STREAM_COMMAND, 0x16u);
    write_packet(&soc, SI3_PACKET, sizeof SI3_PACKET);
    cemu_soc_tick(&soc, 1200000);
    CHECK(read_word(&soc, STREAM_COMMAND) == 0x16u);
    CHECK(read_word(&soc, STREAM_BASE) == 0x9101u);
    xbus_audio_state_t *stream =
        &((xbus_unknown1_state_t *)soc.xbus_unknown1_periph->state)->audio;
    CHECK(stream->stream_packets == 0);
    cemu_soc_free(&soc);
    free(flash);
}


/* No RIFF exists in RAM or NOR: only configuration and delivered bytes count. */
static void test_configured_sampled(void) {
    for (unsigned rate = 1; rate <= 3; rate++) {
        soc_t soc;
        uint8_t *flash = NULL;
        CHECK(init_soc(&soc, cemu_device_by_name("c55"), &flash));
        if (!flash) return;
        xbus_audio_state_t *st = &((xbus_unknown1_state_t *)soc.xbus_unknown1_periph->state)->audio;
        write_packet(&soc, WAV_PACKET, sizeof WAV_PACKET);
        cemu_soc_tick(&soc, 32);
        bus_write16(&soc.bus, STREAM_COMMAND, 0x14);
        CHECK(read_word(&soc, STREAM_COMMAND) == 0x14);
        CHECK(st->stream_accepted == 0);
        bus_write16(&soc.bus, STREAM_COMMAND + 2, rate);
        bus_write16(&soc.bus, 0xEF3A, 2); /* Other notification is not submission. */
        CHECK(st->stream_accepted == 0);
        bus_write16(&soc.bus, 0xEF3A, 4);
        CHECK(read_word(&soc, STREAM_COMMAND) == 0x8014);
        CHECK(st->stream_accepted == 1);
        CHECK(st->decoded_audio.sample_rate == (rate == 1 ? 8000u : rate == 2 ? 16000u : 0u));
        if (rate < 3) {
            CHECK(st->decoded_audio.sample_frames_emitted == 5);
            /* Identical bytes in a new submission are not deduplicated. */
            write_packet(&soc, WAV_PACKET, sizeof WAV_PACKET);
            cemu_soc_tick(&soc, 32);
            CHECK(st->decoded_audio.sample_frames_emitted == 10);
            const uint8_t continuation[] = {0xbc,0xad,0xff,0xff,0x11,0x11};
            write_packet(&soc, continuation, sizeof continuation);
            /* Stop is a submission boundary: commit this final payload. */
            command(&soc, 0x15);
            CHECK(st->active_stream_kind == XBUS_AUDIO_STREAM_UNCLASSIFIED);
            /* 14 delivered frames, no fabricated file tail. */
            CHECK(soc.audio.speaker->pcm[CEMU_AUDIO_SOURCE_XBUS].count == (rate == 1 ? 84u : 42u));
        } else {
            CHECK(st->decoded_audio.sample_frames_emitted == 0);
            command(&soc, 0x15);
        }
        cemu_soc_free(&soc);
        free(flash);
    }
}

static void test_packet_codec(void) {
    cemu_ima_adpcm_stream_t st = {0};
    int16_t pcm[64];
    CHECK(cemu_ima_adpcm_packet(&st, WAV_PACKET, sizeof WAV_PACKET, pcm, 64) == 5);
    for (int i = 0; i < 5; i++) CHECK(pcm[i] == i);
    /* Four old samples, then a header at word offset 1 and four new samples. */
    const uint8_t cross[] = {0xbc,0xad,1,0,0x11,0x11,100,0,0,0,0x99,0x99};
    CHECK(cemu_ima_adpcm_packet(&st, cross, sizeof cross, pcm, 64) == 9);
    CHECK(pcm[0] == 5 && pcm[3] == 8 && pcm[4] == 100 && pcm[8] == 96);
    cemu_ima_adpcm_stream_t before = st;
    uint8_t bad[sizeof cross]; memcpy(bad, cross, sizeof bad); bad[8] = 89;
    CHECK(cemu_ima_adpcm_packet(&st, bad, sizeof bad, pcm, 64) == -1);
    CHECK(!memcmp(&before, &st, sizeof st));
    CHECK(cemu_ima_adpcm_packet(&st, cross, sizeof cross, pcm, 8) == -1);
    CHECK(!memcmp(&before, &st, sizeof st));
    bad[8] = 0; bad[2] = 0xfe;
    CHECK(cemu_ima_adpcm_packet(&st, bad, sizeof bad, pcm, 64) == -1);
    cemu_ima_adpcm_stream_reset(&st);
    CHECK(cemu_ima_adpcm_packet(&st, cross, sizeof cross, pcm, 64) == -1);
}

static void test_full_packet(void) {
    soc_t soc; uint8_t *flash = NULL;
    CHECK(init_soc(&soc, cemu_device_by_name("c55"), &flash));
    if (!flash) return;
    xbus_audio_state_t *stream = &((xbus_unknown1_state_t *)soc.xbus_unknown1_periph->state)->audio;
    uint8_t packet[128];
    for (unsigned i = 0; i < 128; i += 2) { packet[i] = 0xcc; packet[i+1] = 0xc1; }
    command(&soc, 0x16);
    for (unsigned i = 0; i < 128; i += 2) {
        bus_write16(&soc.bus, STREAM_BASE + i, 0xc1cc);
        cemu_soc_tick(&soc, 1);
    }
    cemu_soc_tick(&soc, 30);
    CHECK(stream->stream_accepted == 0);
    cemu_soc_tick(&soc, 1);
    CHECK(stream->stream_accepted == 1 && stream->stream_length[0] == 128);
    CHECK(stream->si3.quanta == 64 && stream->si3.malformed_packets == 0);
    CHECK(!memcmp(stream->stream_data[0], packet, 128));
    CHECK(read_word(&soc, STREAM_BASE) == 0x5555);
    cemu_soc_free(&soc); free(flash);
}

int main(void) {
    test_full_packet();
    test_transport_ordering();
    test_device_qualification();
    test_configured_sampled();
    test_packet_codec();
    puts(failures ? "X55 XBUS audio transport: FAIL" : "X55 XBUS audio transport: PASS");
    return failures ? 1 : 0;
}
