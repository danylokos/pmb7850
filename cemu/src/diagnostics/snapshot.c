/* Full-state snapshot writer — see snapshot.h. */
#define _POSIX_C_SOURCE 200809L
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <sys/stat.h>
#include "snapshot.h"
#include "memory_regions.h"
#include "serial.h"
#include "ssc0.h"
#include "lcd.h"
#include "xbus_unknown1.h"
#include "battery.h"
#include "keypad.h"
#include "twi_gpio.h"
#include "sim.h"
#include "gsm_stub.h"
#include "gsm_legacy_adapter.h"

/* Schema 34 adds the SIM protocol session, baseband transport, virtual-cell
 * state, and temporary legacy-adapter continuation. Schema 35 appends atomic
 * LCD presentation state. Schema 36 requires explicit state for every
 * configured battery input. Schema 37 appends complete mutable keypad state.
 * Schema 38 appends address-sweep presentation state for every LCD model.
 * Schema 39 adds the latched XBUS mailbox submission mode. Older snapshots
 * can restore only an idle mailbox: its in-flight mode cannot be recovered.
 * Schemas 32/33 restore empty SIM/radio sessions and still require explicit
 * attachment flags. */
#define SNAPSHOT_SCHEMA 39
#define SNAPSHOT_SCHEMA_LCD_SWEEP 38
#define SNAPSHOT_SCHEMA_KEYPAD 37
#define SNAPSHOT_SCHEMA_BATTERY 36
#define SNAPSHOT_SCHEMA_LCD_PRESENTED 35
#define SNAPSHOT_SCHEMA_SIM_BASEBAND 34
#define SNAPSHOT_SCHEMA_ASC0_TX 33
#define SNAPSHOT_SCHEMA_ASC0_RX_ONLY 32

typedef struct {
    uint8_t imsi[9], iccid[10], lp[1], phase_ef[1], sst[2], ad[4], kc[9];
    uint8_t hpplmn[1], plmn[24], loci[11], bcch[16], acc[2], fplmn[12];
    uint8_t cphs_info[3], operator_name[16];
} sim_profile_bytes_t;

#if CEMU_INSTRUMENTED
/* base64 of `n` bytes into `out` (out must hold 4*ceil(n/3)+1). */
static void b64(const uint8_t *in, size_t n, char *out) {
    static const char T[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t o = 0;
    for (size_t i = 0; i < n; i += 3) {
        uint32_t x = in[i] << 16;
        if (i + 1 < n) x |= in[i + 1] << 8;
        if (i + 2 < n) x |= in[i + 2];
        out[o++] = T[(x >> 18) & 0x3F];
        out[o++] = T[(x >> 12) & 0x3F];
        out[o++] = (i + 1 < n) ? T[(x >> 6) & 0x3F] : '=';
        out[o++] = (i + 2 < n) ? T[x & 0x3F] : '=';
    }
    out[o] = 0;
}

static flash_state_t *snapshot_flash(soc_t *soc, int chip_index) {
    return cemu_memory_controller_flash_state(&soc->memory, chip_index);
}

static void sim_profile_export(const sim_state_t *st,
                               sim_profile_bytes_t *out) {
#define COPY_PROFILE(field) memcpy(out->field, st->field, sizeof out->field)
    COPY_PROFILE(imsi); COPY_PROFILE(iccid); COPY_PROFILE(lp);
    COPY_PROFILE(phase_ef); COPY_PROFILE(sst); COPY_PROFILE(ad);
    COPY_PROFILE(kc); COPY_PROFILE(hpplmn); COPY_PROFILE(plmn);
    COPY_PROFILE(loci); COPY_PROFILE(bcch); COPY_PROFILE(acc);
    COPY_PROFILE(fplmn); COPY_PROFILE(cphs_info); COPY_PROFILE(operator_name);
#undef COPY_PROFILE
}

static int write_b64_field(FILE *f, const char *name, const uint8_t *data,
                           size_t size, int comma) {
    char *encoded = malloc(4u * ((size + 2u) / 3u) + 1u);
    if (!encoded) return -1;
    b64(data, size, encoded);
    int failed = fprintf(f, "    \"%s\": \"%s\"%s\n", name, encoded,
                         comma ? "," : "") < 0;
    free(encoded);
    return failed ? -1 : 0;
}

static int write_sim_json(FILE *f, const soc_t *soc) {
    const sim_state_t *st = soc->sim_periph->state;
    sim_profile_bytes_t profile;
    sim_profile_export(st, &profile);
    fprintf(f, "  \"sim_session\": {\n");
    fprintf(f, "    \"phase\": %u,\n", (unsigned)st->phase);
    fprintf(f, "    \"due_tick\": %llu,\n",
            (unsigned long long)st->due_tick);
    fprintf(f, "    \"byte_seq\": %llu,\n",
            (unsigned long long)st->byte_seq);
    fprintf(f, "    \"output_head\": %u,\n", st->output_head);
    fprintf(f, "    \"output_len\": %u,\n", st->output_len);
    fprintf(f, "    \"input_len\": %u,\n", st->input_len);
    fprintf(f, "    \"input_need\": %u,\n", st->input_need);
    fprintf(f, "    \"input_kind\": %u,\n", st->input_kind);
    fprintf(f, "    \"selected_df\": %u,\n", st->selected_df);
    fprintf(f, "    \"selected_ef\": %u,\n", st->selected_ef);
    fprintf(f, "    \"response_len\": %u,\n", st->response_len);
    fprintf(f, "    \"tx_waiting\": %u,\n", st->tx_waiting);
    fprintf(f, "    \"completion_pending\": %u,\n",
            st->completion_pending);
    fprintf(f, "    \"rx_loaded\": %u,\n", st->rx_loaded);
    fprintf(f, "    \"initial_raised\": %u,\n", st->initial_raised);
    fprintf(f, "    \"completion_sw\": %u,\n", st->completion_sw);
    fprintf(f, "    \"last_ctrl\": %u,\n", st->last_ctrl);
    if (write_b64_field(f, "output_b64", st->output,
                        sizeof st->output, 1) != 0 ||
        write_b64_field(f, "output_requires_rx_b64",
                        st->output_requires_rx,
                        sizeof st->output_requires_rx, 1) != 0 ||
        write_b64_field(f, "output_internal_b64", st->output_internal,
                        sizeof st->output_internal, 1) != 0 ||
        write_b64_field(f, "input_b64", st->input,
                        sizeof st->input, 1) != 0 ||
        write_b64_field(f, "response_b64", st->response,
                        sizeof st->response, 1) != 0 ||
        write_b64_field(f, "profile_b64", (const uint8_t *)&profile,
                        sizeof profile, 0) != 0)
        return -1;
    fprintf(f, "  },\n");
    return ferror(f) ? -1 : 0;
}

static void write_keypad_json(FILE *f, const soc_t *soc) {
    keypad_mutable_state_t st;
    cemu_keypad_capture_mutable(soc->keypad_periph->state, &st);
    fprintf(f, "  \"keypad\": {\n");
    fprintf(f, "    \"scans\": %llu,\n", (unsigned long long)st.scans);
    fprintf(f, "    \"handled_scans\": %llu,\n",
            (unsigned long long)st.handled_scans);
    fprintf(f, "    \"results\": %llu,\n", (unsigned long long)st.results);
    fprintf(f, "    \"startup_releases\": %llu,\n",
            (unsigned long long)st.startup_releases);
    fprintf(f, "    \"pressed\": %u,\n", st.pressed);
    fprintf(f, "    \"sampled_pressed\": %u,\n", st.sampled_pressed);
    fprintf(f, "    \"last_command\": %u,\n", st.last_command);
    fprintf(f, "    \"last_result\": %u,\n", st.last_result);
    fprintf(f, "    \"startup_power_pending\": %u,\n",
            st.startup_power_pending);
    fprintf(f, "    \"startup_result_tagged\": %u,\n",
            st.startup_result_tagged);
    fprintf(f, "    \"startup_result_read_mask\": %u,\n",
            st.startup_result_read_mask);
    fprintf(f, "    \"matrix_scan_phase\": %u,\n", st.matrix_scan_phase);
    fprintf(f, "    \"release_activity_pending\": %u\n",
            st.release_activity_pending);
    fprintf(f, "  },\n");
}

static int write_asc0_json(FILE *f, soc_t *soc) {
    const serial_state_t *serial = soc->serial_periph->state;
    size_t pending = soc->serial_rx_len - soc->serial_rx_head;
    char *enc = malloc(4 * ((pending + 2u) / 3u) + 1u);
    if (!enc) return -1;
    b64(pending ? soc->serial_rx + soc->serial_rx_head : NULL,
        pending, enc);
    fprintf(f, "  \"asc0\": {\n");
    fprintf(f, "    \"tx_active\": %d, \"tx_tir_raised\": %d, \"tx_byte\": %u,\n",
            serial->tx_active, serial->tx_tir_raised, serial->tx_byte);
    fprintf(f, "    \"tx_con\": %u, \"tx_bg\": %u, \"tx_fdv\": %u,\n",
            serial->tx_con, serial->tx_bg, serial->tx_fdv);
    fprintf(f, "    \"tx_frame_bits\": %u, \"tx_start_tick\": %llu,\n",
            serial->tx_frame_bits,
            (unsigned long long)serial->tx_start_tick);
    fprintf(f, "    \"tx_tir_tick\": %llu, \"tx_completion_tick\": %llu,\n",
            (unsigned long long)serial->tx_tir_tick,
            (unsigned long long)serial->tx_completion_tick);
    fprintf(f, "    \"tx_buffer_full\": %d, \"tx_buffer_byte\": %u,\n",
            serial->tx_buffer_full, serial->tx_buffer_byte);
    fprintf(f, "    \"rx_pending_b64\": \"%s\",\n", enc);
    fprintf(f, "    \"rx_full\": %d, \"rx_active\": %d, \"rx_byte\": %u,\n",
            serial->rx_full, serial->rx_active, serial->rx_byte);
    fprintf(f, "    \"rx_con\": %u, \"rx_bg\": %u, \"rx_fdv\": %u,\n",
            serial->rx_con, serial->rx_bg, serial->rx_fdv);
    fprintf(f, "    \"rx_frame_bits\": %u, \"rx_start_tick\": %llu, \"rx_completion_tick\": %llu\n",
            serial->rx_frame_bits,
            (unsigned long long)serial->rx_start_tick,
            (unsigned long long)serial->rx_completion_tick);
    fprintf(f, "  },\n");
    free(enc);
    return 0;
}

static int write_ssc_lcd_json(FILE *f, soc_t *soc) {
    ssc0_state_t *ssc = (ssc0_state_t *)soc->ssc0_periph->state;
    fprintf(f, "  \"ssc0\": {\n");
    fprintf(f, "    \"config\": %u, \"status\": %u, \"rb_full\": %d,\n",
            ssc->config, ssc->status, ssc->rb_full);
    fprintf(f, "    \"shift_active\": %d, \"shift_tx\": %u, \"shift_bits\": %u,\n",
            ssc->shift_active, ssc->shift_tx, ssc->shift_bits);
    fprintf(f, "    \"shift_msb_first\": %d, \"shift_start_tick\": %llu, \"completion_tick\": %llu,\n",
            ssc->shift_msb_first,
            (unsigned long long)ssc->shift_start_tick,
            (unsigned long long)ssc->completion_tick);
    fprintf(f, "    \"tb_full\": %d, \"tb\": %u\n", ssc->tb_full, ssc->tb);
    fprintf(f, "  },\n");

    if (!soc->lcd_periph) {
        fprintf(f, "  \"lcd\": null,\n");
        return 0;
    }

    size_t n = cemu_lcd_state_size(soc->lcd_periph);
    char *enc = malloc(4 * ((n + 2) / 3) + 1);
    if (!enc) return -1;
    b64((const uint8_t *)soc->lcd_periph->state, n, enc);
    fprintf(f, "  \"lcd\": {\n");
    fprintf(f, "    \"model\": \"%s\",\n", soc->lcd_periph->model);
    fprintf(f, "    \"state_size\": %zu,\n", n);
    fprintf(f, "    \"state_b64\": \"%s\"\n", enc);
    fprintf(f, "  },\n");
    free(enc);
    return 0;
}

static int write_twi_json(FILE *f, soc_t *soc) {
    if (!soc->twi_gpio_periph) {
        fprintf(f, "  \"twi_gpio\": null,\n");
        return 0;
    }
    const twi_gpio_state_t *st = soc->twi_gpio_periph->state;
    size_t n = sizeof *st;
    char *enc = malloc(4 * ((n + 2) / 3) + 1);
    if (!enc) return -1;
    b64((const uint8_t *)st, n, enc);
    fprintf(f, "  \"twi_gpio\": {\n");
    fprintf(f, "    \"model\": \"%s\",\n", soc->twi_gpio_periph->model);
    fprintf(f, "    \"state_size\": %zu,\n", n);
    fprintf(f, "    \"state_b64\": \"%s\"\n", enc);
    fprintf(f, "  },\n");
    free(enc);
    return 0;
}

/* Write one contiguous region [start,start+len) to <dir>/<prefix>_<start>.bin. */
static int write_region_bin(const char *dir, const char *prefix,
                            uint32_t start, const uint8_t *bytes, uint32_t len) {
    char path[512];
    snprintf(path, sizeof path, "%s/%s_%06x.bin", dir, prefix, start);
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    size_t w = fwrite(bytes, 1, len, f);
    fclose(f);
    return w == len ? 0 : -1;
}

static int write_full_bin_payload(const char *full_dir, const cemu_memory_region_t *r,
                                  soc_t *soc) {
    char path[1024];
    snprintf(path, sizeof path, "%s/%s", full_dir, r->file);
    FILE *f = fopen(path, "wb");
    if (!f) return -1;

    enum { CHUNK = 64 * 1024 };
    uint8_t *buf = malloc(CHUNK);
    if (!buf) { fclose(f); return -1; }

    uint32_t addr = r->start;
    while (addr <= r->end) {
        uint32_t remain = r->end - addr + 1u;
        uint32_t n = remain < CHUNK ? remain : CHUNK;
        for (uint32_t i = 0; i < n; i++) buf[i] = cemu_memory_controller_peek8(&soc->memory, addr + i);
        if (fwrite(buf, 1, n, f) != n) {
            free(buf);
            fclose(f);
            return -1;
        }
        addr += n;
    }

    free(buf);
    fclose(f);
    return 0;
}

static int write_full_bins(const char *dir, soc_t *soc) {
    char full_dir[1024];
    snprintf(full_dir, sizeof full_dir, "%s/full-bins", dir);
    mkdir(full_dir, 0777);

    size_t nregions = CEMU_MEMORY_REGION_COUNT;
    for (size_t i = 0; i < nregions; i++) {
        if (write_full_bin_payload(full_dir, &CEMU_MEMORY_REGIONS[i], soc) != 0) return -1;
    }

    char manifest[1024];
    snprintf(manifest, sizeof manifest, "%s/manifest.json", full_dir);
    FILE *f = fopen(manifest, "w");
    if (!f) return -1;
    fprintf(f, "{\n");
    fprintf(f, "  \"schema\": 1,\n");
    fprintf(f, "  \"view\": \"cpu-visible\",\n");
    fprintf(f, "  \"fill_policy\": \"memory_controller_peek8; unmapped returns 0xff\",\n");
    fprintf(f, "  \"regions\": [");
    for (size_t i = 0; i < nregions; i++) {
        const cemu_memory_region_t *r = &CEMU_MEMORY_REGIONS[i];
        uint32_t size = r->end - r->start + 1u;
        fprintf(f,
                "%s\n    {\"name\": \"%s\", \"start\": \"0x%06x\", "
                "\"end\": \"0x%06x\", \"size\": %u, \"file\": \"%s\"}",
                i ? "," : "", r->name, r->start, r->end, size, r->file);
    }
    fprintf(f, "\n  ]\n");
    fprintf(f, "}\n");
    fclose(f);
    return 0;
}

static int write_nor_regions_json(FILE *f, const char *dir,
                                  const char *prefix,
                                  const nor_patch_t *patches,
                                  size_t patch_count) {
    fprintf(f, "    \"regions\": [");
    int first = 1;
    for (size_t i = 0; i < patch_count;) {
        uint32_t start = patches[i].off;
        size_t j = i + 1u;
        while (j < patch_count &&
               patches[j].off == patches[j - 1u].off + 1u)
            j++;
        uint32_t len = (uint32_t)(j - i);
        uint8_t *buf = malloc(len ? len : 1u);
        if (!buf) return -1;
        for (size_t k = i; k < j; k++) buf[k - i] = patches[k].value;
        if (write_region_bin(dir, prefix, start, buf, len) != 0) {
            free(buf);
            return -1;
        }
        fprintf(f, "%s\n      {\"start\": \"%#08x\", \"end\": \"%#08x\", "
                "\"size\": %u, \"file\": \"%s_%06x.bin\"}",
                first ? "" : ",", start, start + len - 1u, len,
                prefix, start);
        first = 0;
        free(buf);
        i = j;
    }
    fprintf(f, "%s]\n", first ? "" : "\n    ");
    return 0;
}

static int write_m58_flash_json(FILE *f, const char *dir,
                                const m58lw064d_state_t *st,
                                const char *header, const char *prefix,
                                const char *footer) {
    size_t wb_count = 0;
    const m58lw064d_write_buffer_entry_t *wb = cemu_m58lw064d_state_write_buffer_entries(st, &wb_count);
    size_t patch_count = 0;
    const m58lw064d_patch_t *patches = cemu_m58lw064d_state_patches(st, &patch_count);

    fputs(header, f);
    fprintf(f, "    \"model\": \"m58lw064d\",\n");
    fprintf(f, "    \"read_mode\": \"%s\",\n", cemu_m58lw064d_mode_str(st));
    fprintf(f, "    \"command_phase\": \"%s\",\n",
            cemu_m58lw064d_command_phase_str(st->command_phase));
    fprintf(f, "    \"status\": %u,\n", st->status);
    fprintf(f, "    \"cmd_writes\": %u,\n", st->cmd_writes);
    fprintf(f, "    \"erased_blocks\": \"%#018llx\",\n",
            (unsigned long long)st->erased_blocks);
    fprintf(f, "    \"factory_uid_set\": %s,\n",
            st->factory_uid_set ? "true" : "false");
    fprintf(f, "    \"factory_uid\": \"");
    for (size_t i = 0; i < sizeof st->factory_uid; i++)
        fprintf(f, "%02X", st->factory_uid[i]);
    fprintf(f, "\",\n");
    fprintf(f, "    \"write_buffer_state\": \"%s\",\n",
            cemu_m58lw064d_write_buffer_state_str(st->write_buffer_state));
    fprintf(f, "    \"write_buffer_words_left\": %u,\n", st->write_buffer_words_left);
    fprintf(f, "    \"write_buffer_block\": \"%#08x\",\n",
            st->write_buffer_block);
    fprintf(f, "    \"write_buffer_page\": \"%#08x\",\n",
            st->write_buffer_page);

    fprintf(f, "    \"write_buffer\": [");
    for (size_t i = 0; i < wb_count; i++) {
        fprintf(f, "%s\n      {\"off\": \"%#08x\", \"value\": \"%#x\", \"size\": %u}",
                i ? "," : "", wb[i].off, wb[i].value, wb[i].size);
    }
    fprintf(f, "%s],\n", wb_count ? "\n    " : "");

    if (write_nor_regions_json(f, dir, prefix, patches, patch_count) != 0)
        return -1;
    fputs(footer, f);
    return 0;
}

static int write_am29_flash_json(FILE *f, const char *dir,
                                 const am29lv_state_t *st,
                                 const char *header, const char *prefix,
                                 const char *footer) {
    size_t patch_count = 0;
    const am29lv_patch_t *patches =
        cemu_am29lv_state_patches(st, &patch_count);
    char secsi_prefix[64];
    snprintf(secsi_prefix, sizeof secsi_prefix, "%s_secsi", prefix);
    if (write_region_bin(dir, secsi_prefix, 0, st->secsi,
                         AM29LV_SECSI_SIZE) != 0)
        return -1;

    fputs(header, f);
    fprintf(f, "    \"model\": \"%s\",\n", cemu_am29lv_model_str(st));
    fprintf(f, "    \"read_mode\": \"%s\",\n", cemu_am29lv_mode_str(st));
    fprintf(f, "    \"command_phase\": \"%s\",\n",
            cemu_am29lv_phase_str(st->phase));
    fprintf(f, "    \"cmd_writes\": %u,\n", st->cmd_writes);
    fprintf(f, "    \"erase_active\": %s,\n",
            st->erase_active ? "true" : "false");
    fprintf(f, "    \"erase_suspended\": %s,\n",
            st->erase_suspended ? "true" : "false");
    fprintf(f, "    \"erase_sector\": \"%#08x\",\n", st->erase_sector);
    fprintf(f, "    \"erase_timer_ticks_remaining\": %u,\n",
            st->erase_timer_ticks_remaining);
    fprintf(f, "    \"erase_ticks_remaining\": %u,\n",
            st->erase_ticks_remaining);
    fprintf(f, "    \"erase_toggle\": %s,\n",
            st->erase_toggle ? "true" : "false");
    for (size_t i = 0; i < AM29LV_MAX_SECTOR_COUNT / 64u; i++)
        fprintf(f, "    \"erased_sector_bits%zu\": \"0x%llx\",\n", i,
                (unsigned long long)st->erased_sectors[i]);
    fprintf(f, "    \"secsi_esn_set\": %s,\n",
            st->secsi_esn_set ? "true" : "false");
    fprintf(f, "    \"secsi_locked\": %s,\n",
            st->secsi_locked ? "true" : "false");
    fprintf(f, "    \"secsi_file\": \"%s_secsi_000000.bin\",\n", prefix);
    fprintf(f, "    \"write_buffer_sector\": \"%#08x\",\n",
            st->write_buffer_sector);
    fprintf(f, "    \"write_buffer_page\": \"%#08x\",\n",
            st->write_buffer_page);
    fprintf(f, "    \"write_buffer_words_left\": %u,\n",
            st->write_buffer_words_left);
    fprintf(f, "    \"write_buffer\": [");
    for (size_t i = 0; i < st->write_buffer_len; i++) {
        const am29lv_write_buffer_entry_t *entry =
            &st->write_buffer[i];
        fprintf(f, "%s\n      "
                "{\"off\": \"%#08x\", \"value\": \"%#x\", \"size\": %u}",
                i ? "," : "", entry->off, entry->value, entry->size);
    }
    fprintf(f, "%s],\n", st->write_buffer_len ? "\n    " : "");
    if (write_nor_regions_json(f, dir, prefix,
                               (const nor_patch_t *)patches,
                               patch_count) != 0)
        return -1;
    fputs(footer, f);
    return 0;
}

static int write_w30_flash_json(FILE *f, const char *dir,
                                const w30_state_t *st,
                                const char *header, const char *prefix,
                                const char *footer) {
    fputs(header, f);
    fprintf(f, "    \"model\": \"%s\",\n", cemu_w30_model_str(st));
    fprintf(f, "    \"command_phase\": \"%s\",\n",
            cemu_w30_phase_str(st->phase));
    fprintf(f, "    \"setup_partition\": %u,\n", st->setup_partition);
    fprintf(f, "    \"setup_off\": \"%#08x\",\n", st->setup_off);
    fprintf(f, "    \"cmd_writes\": %u,\n", st->cmd_writes);
    for (unsigned i = 0; i < st->config->partition_count; i++)
        fprintf(f, "    \"mode%u\": %u, \"status%u\": %u, "
                "\"configuration%u\": %u,\n",
                i, st->mode[i], i, st->status[i], i, st->configuration[i]);
    for (unsigned i = 0; i < cemu_w30_block_bitmap_words(st); i++)
        fprintf(f, "    \"erased_bits%u\": \"%#llx\", "
                "\"locked_bits%u\": \"%#llx\", "
                "\"lockdown_bits%u\": \"%#llx\",\n",
                i, (unsigned long long)st->erased_blocks[i],
                i, (unsigned long long)st->locked_blocks[i],
                i, (unsigned long long)st->lockdown_blocks[i]);
    fprintf(f, "    \"factory_uid_set\": %s,\n",
            st->factory_uid_set ? "true" : "false");
    fprintf(f, "    \"factory_uid\": \"");
    for (unsigned i = 0; i < sizeof st->factory_uid; i++)
        fprintf(f, "%02X", st->factory_uid[i]);
    fprintf(f, "\",\n    \"customer_locked\": %s,\n",
            st->customer_locked ? "true" : "false");
    fprintf(f, "    \"customer\": \"");
    for (unsigned i = 0; i < sizeof st->customer; i++)
        fprintf(f, "%02X", st->customer[i]);
    fprintf(f, "\",\n");
    const w30_operation_t *ops[] = {
        &st->active, &st->suspended_erase, &st->suspended_program
    };
    const char *names[] = {"active", "suspended_erase", "suspended_program"};
    for (unsigned i = 0; i < 3; i++) {
        const w30_operation_t *op = ops[i];
        fprintf(f, "    \"%s_kind\": %u, \"%s_partition\": %u, "
                "\"%s_block\": %u, \"%s_off\": \"%#08x\", "
                "\"%s_value\": %u, \"%s_ticks\": %u, "
                "\"%s_program_error\": %u,\n",
                names[i], op->kind, names[i], op->partition,
                names[i], op->block, names[i], op->off,
                names[i], op->value, names[i], op->ticks_remaining,
                names[i], op->program_error);
    }
    if (write_nor_regions_json(f, dir, prefix, st->store.patches,
                               st->store.count) != 0)
        return -1;
    fputs(footer, f);
    return 0;
}

static int write_flash_json(FILE *f, const char *dir,
                            const flash_state_t *st,
                            const char *header, const char *prefix,
                            const char *footer) {
    if (st->kind == FLASH_MODEL_M58LW064D)
        return write_m58_flash_json(f, dir, &st->u.m58,
                                    header, prefix, footer);
    if (st->kind == FLASH_MODEL_AM29LV)
        return write_am29_flash_json(f, dir, &st->u.am29,
                                     header, prefix, footer);
    if (st->kind == FLASH_MODEL_W30)
        return write_w30_flash_json(f, dir, &st->u.w30,
                                    header, prefix, footer);
    return -1;
}

static int write_flash_states_json(FILE *f, const char *dir, soc_t *soc) {
    if (soc->memory.n_flash_chips == 1)
        return write_flash_json(f, dir, snapshot_flash(soc, 0),
                                "  \"flash\": {\n", "flash", "  },\n");
    fprintf(f, "  \"flash_chips\": [");
    for (int i = 0; i < soc->memory.n_flash_chips; i++) {
        char header[256], prefix[32];
        snprintf(prefix, sizeof prefix, "flash%d", i);
        snprintf(header, sizeof header,
                 "%s\n    {\"index\": %d, \"name\": \"%s\",\n",
                 i ? "," : "", i, soc->memory.flash_chips[i].name);
        if (write_flash_json(f, dir, snapshot_flash(soc, i), header, prefix,
                             "    }") != 0)
            return -1;
    }
    fprintf(f, "\n  ],\n");
    return 0;
}

int snapshot_write_dir_ex(const char *dir, cpu_t *cpu, soc_t *soc, const char *flash_path,
                          snapshot_write_options_t opts) {
    mkdir(dir, 0777);   /* create if absent; ignore EEXIST */

    char json[512];
    snprintf(json, sizeof json, "%s/snapshot.json", dir);
    FILE *f = fopen(json, "w");
    if (!f) { fprintf(stderr, "error: cannot write snapshot %s\n", json); return -1; }

    fprintf(f, "{\n");
    fprintf(f, "  \"schema\": %d,\n", SNAPSHOT_SCHEMA);

    /* Startup metadata includes the immutable mapping contract. A restore into a
     * differently configured machine is rejected before state is mutated. */
    fprintf(f, "  \"prove" "nance\": {\n");
    if (opts.host_json) {
        fputs(opts.host_json, f);
    } else {
        if (flash_path)
            fprintf(f, "    \"flash\": \"%s\",\n", flash_path);
        fputs("    \"firmware_" "patches\": [],\n", f);
    }
    fprintf(f, "    \"flash_chips\": [");
    for (int i = 0; i < soc->memory.n_flash_chips; i++) {
        const flash_chip_config_t *chip = &soc->memory.flash_chips[i];
        fprintf(f, "%s\n      {\"index\": %d, \"name\": \"%s\", "
                "\"model\": \"%s\", \"file_offset\": %zu, "
                "\"chip_size\": %u}",
                i ? "," : "", i, chip->name, chip->model,
                soc->memory.flash_file_offsets[i], chip->chip_size);
    }
    fprintf(f, "%s],\n", soc->memory.n_flash_chips ? "\n    " : "");
    fprintf(f, "    \"lm_size\": %u,\n", soc->memory.lm_size);
    xbus_unknown1_state_t *unknown1 =
        (xbus_unknown1_state_t *)soc->xbus_unknown1_periph->state;
    fprintf(f, "    \"xbus_unknown1_id\": %u,\n", unknown1->id);
    if (soc->twi_gpio_periph) {
        const twi_gpio_state_t *twi = soc->twi_gpio_periph->state;
        fprintf(f, "    \"twi_gpio\": {\"model\": \"%s\", "
                "\"address\": %u, \"port\": %u, \"scl_bit\": %u, "
                "\"sda_bit\": %u, \"register_count\": %u},\n",
                soc->twi_gpio_periph->model, twi->address, twi->port,
                twi->scl_bit, twi->sda_bit, twi->register_count);
    } else {
        fprintf(f, "    \"twi_gpio\": null,\n");
    }
    fprintf(f, "    \"windows\": [");
    for (int i = 0; i < soc->memory.n_flash_windows; i++) {
        const flash_window_config_t *w = &soc->memory.flash_windows[i];
        fprintf(f, "%s\n      {\"cpu_base\": %u, \"cpu_size\": %u, "
                "\"chip_index\": %d, \"chip_base\": %u, "
                "\"mirror_period\": %u, \"command_visible\": %s}",
                i ? "," : "", w->cpu_base, w->cpu_size, w->chip_index, w->chip_base,
                w->mirror_period, w->command_visible ? "true" : "false");
    }
    fprintf(f, "%s],\n", soc->memory.n_flash_windows ? "\n    " : "");
    fprintf(f, "    \"external_ram_config\": [");
    for (int i = 0; i < soc->memory.n_external_ram; i++) {
        external_ram_state_t *ram = cemu_memory_controller_external_ram_state(&soc->memory, i);
        fprintf(f, "%s\n      {\"model\": \"%s\", \"chip_size\": %u, "
                "\"addrsel_index\": %d}",
                i ? "," : "", ram->model, ram->chip_size,
                ram->addrsel_index);
    }
    fprintf(f, "%s]\n", soc->memory.n_external_ram ? "\n    " : "");
    fprintf(f, "  },\n");

    /* cpu: addresses hex, counters decimal (mirror _jsonable_cpu). */
    fprintf(f, "  \"cpu\": {\n");
    fprintf(f, "    \"csp\": \"%#06x\",\n", cpu->csp);
    fprintf(f, "    \"ip\": \"%#06x\",\n", cpu->ip);
    fprintf(f, "    \"pc\": \"%#08x\",\n", cpu_pc(cpu));
    fprintf(f, "    \"ext_count\": %d,\n", cpu->ext_count);
    fprintf(f, "    \"extr\": %d,\n", cpu->extr);
    fprintf(f, "    \"halted\": %s,\n", cpu->halted ? "true" : "false");
    fprintf(f, "    \"idle\": %s,\n", cpu->idle ? "true" : "false");
    fprintf(f, "    \"icount\": %llu,\n", (unsigned long long)cpu->icount);
    fprintf(f, "    \"interrupts_delivered\": %llu,\n", (unsigned long long)cpu->interrupts_delivered);
    fprintf(f, "    \"traps_taken\": %llu\n", (unsigned long long)cpu->traps_taken);
    fprintf(f, "  },\n");

    /* soc scalars (mirror _SOC_STATE_ATTRS subset the C models). */
    fprintf(f, "  \"soc\": {\n");
    fprintf(f, "    \"ticks\": %llu,\n", (unsigned long long)soc->ticks);
    fprintf(f, "    \"init_locked\": %s,\n",
            soc->init_locked ? "true" : "false");
    fprintf(f, "    \"rstout\": %s,\n", soc->rstout ? "true" : "false");
    fprintf(f, "    \"mem_write_seq\": %llu\n", (unsigned long long)soc->memory.mem_write_seq);
    fprintf(f, "  },\n");

    xbus_unknown1_state_t *mailbox =
        (xbus_unknown1_state_t *)soc->xbus_unknown1_periph->state;
    fprintf(f, "  \"xbus_unknown1\": {\n");
    fprintf(f, "    \"transaction_seq\": %llu,\n",
            (unsigned long long)mailbox->transaction_seq);
    fprintf(f, "    \"active_transaction_id\": %llu,\n",
            (unsigned long long)mailbox->active_transaction_id);
    fprintf(f, "    \"phase\": %u,\n", (unsigned)mailbox->phase);
    fprintf(f, "    \"control\": %u,\n", mailbox->control);
    fprintf(f, "    \"deadline\": %llu\n",
            (unsigned long long)mailbox->deadline);
    fprintf(f, "  },\n");

    if (cemu_battery_available(soc->battery_periph)) {
        fprintf(f, "  \"battery\": {\"level\": %u, \"charging\": %s},\n",
                cemu_battery_level(soc->battery_periph),
                cemu_battery_charging(soc->battery_periph) ? "true" : "false");
    } else {
        fprintf(f, "  \"battery\": null,\n");
    }

    write_keypad_json(f, soc);

    if (write_sim_json(f, soc) != 0) {
        fclose(f);
        fprintf(stderr, "error: cannot serialize SIM snapshot state\n");
        return -1;
    }

    const gsm_stub_state_t *bb = soc->gsm_stub_periph->state;
    const gsm_legacy_adapter_state_t *legacy =
        soc->gsm_legacy_adapter_periph->state;
    fprintf(f, "  \"baseband\": {\n");
    fprintf(f, "    \"pending\": %u,\n", bb->pending != 0);
    fprintf(f, "    \"command\": %u,\n", bb->command);
    fprintf(f, "    \"pending_command\": %u,\n", bb->pending_command);
    fprintf(f, "    \"due_tick\": %llu,\n",
            (unsigned long long)bb->due_tick);
    fprintf(f, "    \"request_sequence\": %llu,\n",
            (unsigned long long)bb->request_sequence);
    fprintf(f, "    \"pending_sequence\": %llu,\n",
            (unsigned long long)bb->pending_sequence);
    fprintf(f, "    \"synchronized_responses\": %llu,\n",
            (unsigned long long)bb->synchronized_responses);
    fprintf(f, "    \"result_sequence\": %llu,\n",
            (unsigned long long)bb->result_sequence);
    fprintf(f, "    \"last_result_source\": %u,\n",
            (unsigned)bb->last_result_source);
    fprintf(f, "    \"last_result_command\": %u,\n",
            bb->last_result_command);
    fprintf(f, "    \"last_result_word0\": %u,\n",
            bb->last_result_word0);
    fprintf(f, "    \"last_result_word1\": %u,\n",
            bb->last_result_word1);
    fprintf(f, "    \"cell_synchronized\": %u,\n",
            bb->cell.synchronized != 0);
    fprintf(f, "    \"cell_available\": %u,\n",
            bb->cell.available != 0);
    fprintf(f, "    \"cell_service_allowed\": %u,\n",
            bb->cell.service_allowed != 0);
    fprintf(f, "    \"cell_plmn_bcd\": %u,\n",
            bb->cell.plmn[0] | ((unsigned)bb->cell.plmn[1] << 8) |
            ((unsigned)bb->cell.plmn[2] << 16));
    fprintf(f, "    \"cell_arfcn\": %u,\n", bb->cell.arfcn);
    fprintf(f, "    \"cell_quality\": %u,\n", bb->cell.quality);
    fprintf(f, "    \"cell_level\": %u,\n", bb->cell.level);
    fprintf(f, "    \"measurement_index\": %u,\n",
            bb->cell.measurement_index);
    fprintf(f, "    \"legacy_registration_events\": %llu,\n",
            (unsigned long long)legacy->registration_events);
    fprintf(f, "    \"legacy_signal_events\": %llu,\n",
            (unsigned long long)legacy->signal_events);
    fprintf(f, "    \"legacy_return_pc\": %u\n", legacy->return_pc);
    fprintf(f, "  },\n");

    if (write_twi_json(f, soc) != 0) {
        fclose(f);
        fprintf(stderr, "error: cannot serialize GPIO-TWI snapshot state\n");
        return -1;
    }
    if (write_ssc_lcd_json(f, soc) != 0) {
        fclose(f);
        fprintf(stderr, "error: cannot serialize SSC0/LCD snapshot state\n");
        return -1;
    }
    if (write_asc0_json(f, soc) != 0) {
        fclose(f);
        fprintf(stderr, "error: cannot serialize ASC0 snapshot state\n");
        return -1;
    }

    /* sfr: nonzero words, {"0xADDR": "0xVAL"} sorted by address. */
    fprintf(f, "  \"sfr\": {");
    int first = 1;
    for (uint32_t i = 0; i < SFR_WORDS; i++) {
        if (soc->memory.sfr[i] == 0) continue;
        uint32_t addr = SFR_BASE + 2u * i;
        fprintf(f, "%s\n    \"%#06x\": \"%#x\"", first ? "" : ",", addr, soc->memory.sfr[i]);
        first = 0;
    }
    fprintf(f, "%s},\n", first ? "" : "\n  ");

    /* serial console output, base64. */
    {
        size_t n = soc->serial_tx_len;
        char *enc = malloc(4 * ((n + 2) / 3) + 1);
        b64(soc->serial_tx, n, enc);
        fprintf(f, "  \"serial_tx_b64\": \"%s\",\n", enc);
        free(enc);
    }

    if (write_flash_states_json(f, dir, soc) != 0) {
        fclose(f);
        fprintf(stderr, "error: cannot write flash snapshot blobs\n");
        return -1;
    }

    /* Local memory is physical state independent of its current SYSCON alias. */
    if (write_region_bin(dir, "lm", 0, soc->memory.lm, soc->memory.lm_size) != 0) {
        fprintf(stderr, "error: cannot write local memory\n");
        fclose(f);
        return -1;
    }
    fprintf(f, "  \"lm\": {\"size\": %u, \"file\": \"lm_000000.bin\"},\n",
            soc->memory.lm_size);

    /* Physical external RAM is independent of its current CPU aperture. */
    fprintf(f, "  \"external_ram\": [");
    for (int i = 0; i < soc->memory.n_external_ram; i++) {
        external_ram_state_t *ram = cemu_memory_controller_external_ram_state(&soc->memory, i);
        char prefix[32];
        snprintf(prefix, sizeof prefix, "xram%d", i);
        if (write_region_bin(dir, prefix, 0, ram->bytes,
                             ram->chip_size) != 0) {
            fprintf(stderr, "error: cannot write external RAM %d\n", i);
            fclose(f);
            return -1;
        }
        fprintf(f, "%s\n    {\"index\": %d, \"size\": %u, "
                "\"file\": \"xram%d_000000.bin\"}",
                i ? "," : "", i, ram->chip_size, i);
    }
    fprintf(f, "%s],\n", soc->memory.n_external_ram ? "\n  " : "");

    /* regions: one ram_<start>.bin per contiguous written run (gap=0, keep zeros
     * — byte-exact), indexed here by {start, end, size, file}. */
    fprintf(f, "  \"regions\": [");
    first = 1;
    uint32_t a = 0;
    while (a < ADDR_SPACE) {
        if (!soc->memory.present[a]) { a++; continue; }
        uint32_t start = a;
        while (a < ADDR_SPACE && soc->memory.present[a]) a++;
        uint32_t len = a - start;
        if (write_region_bin(dir, "ram", start, &soc->memory.ram[start], len) != 0) {
            fprintf(stderr, "error: cannot write region ram_%06x.bin\n", start);
            fclose(f);
            return -1;
        }
        fprintf(f, "%s\n    {\"start\": \"%#08x\", \"end\": \"%#08x\", \"size\": %u, \"file\": \"ram_%06x.bin\"}",
                first ? "" : ",", start, start + len - 1, len, start);
        first = 0;
    }
    fprintf(f, "%s]\n", first ? "" : "\n  ");
    fprintf(f, "}\n");
    fclose(f);

    if (opts.full_bins && write_full_bins(dir, soc) != 0) {
        fprintf(stderr, "error: cannot write full-bin snapshot export\n");
        return -1;
    }
    return 0;
}

int snapshot_write_dir(const char *dir, cpu_t *cpu, soc_t *soc, const char *flash_path) {
    snapshot_write_options_t opts = {0};
    return snapshot_write_dir_ex(dir, cpu, soc, flash_path, opts);
}

/* ======================================================================= */
/* Snapshot reader (inverse of the writer). The snapshot.json shape is our    */
/* own fixed json.dumps(indent=2) layout, so a small purpose-built scanner    */
/* (key-substring + strtoull) suffices — no general JSON parser needed.       */
/* ======================================================================= */
#else
static flash_state_t *snapshot_flash(soc_t *soc, int chip_index) {
    return cemu_memory_controller_flash_state(&soc->memory, chip_index);
}
#endif

/* Slurp a whole file into a NUL-terminated buffer (caller frees). NULL on error. */
static char *slurp_text(const char *path, long *len_out) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    char *buf = malloc((n > 0 ? n : 0) + 1);
    if (!buf) { fclose(f); return NULL; }
    if (n > 0 && fread(buf, 1, n, f) != (size_t)n) { fclose(f); free(buf); return NULL; }
    buf[n > 0 ? n : 0] = 0;
    fclose(f);
    if (len_out) *len_out = n;
    return buf;
}

/* Base64 decode into `out`; returns SIZE_MAX if the decoded payload is too large. */
static size_t b64_decode(const char *in, uint8_t *out, size_t out_cap) {
    static int8_t D[256]; static int init = 0;
    if (!init) {
        for (int i = 0; i < 256; i++) D[i] = -1;
        const char *T = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        for (int i = 0; i < 64; i++) D[(unsigned char)T[i]] = (int8_t)i;
        init = 1;
    }
    size_t o = 0; int acc = 0, bits = 0;
    for (const char *p = in; *p; p++) {
        if (*p == '=') break;
        int v = D[(unsigned char)*p];
        if (v < 0) continue;
        acc = (acc << 6) | v; bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (o >= out_cap) return SIZE_MAX;
            out[o++] = (uint8_t)((acc >> bits) & 0xFF);
        }
    }
    return o;
}

/* Find `"key"` in json and return a pointer just past the following ':', or NULL.
 * `from` lets the caller scope the search (e.g. inside a sub-object). */
static const char *find_key(const char *from, const char *key) {
    char pat[64]; snprintf(pat, sizeof pat, "\"%s\"", key);
    size_t pat_len = strlen(pat);
    const char *p = from;
    while ((p = strstr(p, pat)) != NULL) {
        const char *after = p + pat_len;
        while (*after == ' ' || *after == '\t' ||
               *after == '\r' || *after == '\n')
            after++;
        if (*after == ':') return after + 1;
        p += pat_len;
    }
    return NULL;
}

static const char *json_object_end(const char *object) {
    if (!object) return NULL;
    const char *p = object;
    while (*p && *p != '{') p++;
    if (*p != '{') return NULL;
    int depth = 0, in_string = 0, escaped = 0;
    for (; *p; p++) {
        if (in_string) {
            if (escaped) escaped = 0;
            else if (*p == '\\') escaped = 1;
            else if (*p == '"') in_string = 0;
            continue;
        }
        if (*p == '"') in_string = 1;
        else if (*p == '{') depth++;
        else if (*p == '}' && --depth == 0) return p;
    }
    return NULL;
}

static const char *json_array_end(const char *array) {
    if (!array) return NULL;
    const char *p = array;
    while (*p && *p != '[') p++;
    if (*p != '[') return NULL;
    int depth = 0, in_string = 0, escaped = 0;
    for (; *p; p++) {
        if (in_string) {
            if (escaped) escaped = 0;
            else if (*p == '\\') escaped = 1;
            else if (*p == '"') in_string = 0;
            continue;
        }
        if (*p == '"') in_string = 1;
        else if (*p == '[') depth++;
        else if (*p == ']' && --depth == 0) return p;
    }
    return NULL;
}

static uint64_t parse_uint_value(const char *p, int *ok) {
    if (!p) { if (ok) *ok = 0; return 0; }
    while (*p == ' ' || *p == '	') p++;
    if (*p == '"') p++;                       /* skip opening quote of hex string */
    if (ok) *ok = 1;
    return strtoull(p, NULL, 0);
}

/* Parse the scalar after a key as an unsigned value (handles "0x.." strings or
 * bare decimals). Returns 0 and leaves *ok=0 if the key is absent. */
static uint64_t key_uint(const char *from, const char *key, int *ok) {
    return parse_uint_value(find_key(from, key), ok);
}

static int key_string(const char *from, const char *key, char *out, size_t cap) {
    const char *p = find_key(from, key);
    if (!p) return 0;
    while (*p == ' ' || *p == '\t') p++;
    if (*p != '"') return 0;
    p++;
    const char *end = strchr(p, '"');
    if (!end || (size_t)(end - p) >= cap) return 0;
    memcpy(out, p, end - p);
    out[end - p] = 0;
    return 1;
}

static size_t decode_b64_key(const char *obj, const char *key,
                             uint8_t *out, size_t out_cap) {
    const char *p = find_key(obj, key);
    if (!p) return SIZE_MAX;
    while (*p == ' ' || *p == '\t') p++;
    if (*p++ != '"') return SIZE_MAX;
    const char *end = strchr(p, '"');
    if (!end) return SIZE_MAX;
    size_t enc_len = (size_t)(end - p);
    char *enc = malloc(enc_len + 1u);
    if (!enc) return SIZE_MAX;
    memcpy(enc, p, enc_len);
    enc[enc_len] = 0;
    size_t got = b64_decode(enc, out, out_cap);
    free(enc);
    return got;
}

static void sim_profile_import(sim_state_t *st,
                               const sim_profile_bytes_t *in) {
#define COPY_PROFILE(field) memcpy(st->field, in->field, sizeof st->field)
    COPY_PROFILE(imsi); COPY_PROFILE(iccid); COPY_PROFILE(lp);
    COPY_PROFILE(phase_ef); COPY_PROFILE(sst); COPY_PROFILE(ad);
    COPY_PROFILE(kc); COPY_PROFILE(hpplmn); COPY_PROFILE(plmn);
    COPY_PROFILE(loci); COPY_PROFILE(bcch); COPY_PROFILE(acc);
    COPY_PROFILE(fplmn); COPY_PROFILE(cphs_info); COPY_PROFILE(operator_name);
#undef COPY_PROFILE
}

static int restore_sim(const char *doc, soc_t *soc, uint64_t schema) {
    if (schema < SNAPSHOT_SCHEMA_SIM_BASEBAND) return 0;
    const char *obj = find_key(doc, "sim_session");
    sim_state_t *st = soc->sim_periph->state;
    int ok = 0;
#define SIM_UINT(name) key_uint(obj, (name), &ok)
    uint64_t phase = SIM_UINT("phase");
    int valid = obj && ok && phase <= SIM_PHASE_READY;
    uint64_t due_tick = SIM_UINT("due_tick"); valid &= ok;
    uint64_t byte_seq = SIM_UINT("byte_seq"); valid &= ok;
    uint64_t output_head = SIM_UINT("output_head");
    valid &= ok && output_head < SIM_QUEUE_CAP;
    uint64_t output_len = SIM_UINT("output_len");
    valid &= ok && output_len <= SIM_QUEUE_CAP;
    uint64_t input_len = SIM_UINT("input_len");
    valid &= ok && input_len <= SIM_APDU_CAP;
    uint64_t input_need = SIM_UINT("input_need");
    valid &= ok && input_need <= SIM_APDU_CAP;
    uint64_t input_kind = SIM_UINT("input_kind");
    valid &= ok && input_kind <= 3;
    uint64_t selected_df = SIM_UINT("selected_df"); valid &= ok && selected_df <= 0xFFFFu;
    uint64_t selected_ef = SIM_UINT("selected_ef"); valid &= ok && selected_ef <= 0xFFFFu;
    uint64_t response_len = SIM_UINT("response_len");
    valid &= ok && response_len <= sizeof st->response;
    uint64_t tx_waiting = SIM_UINT("tx_waiting"); valid &= ok && tx_waiting <= 1;
    uint64_t completion_pending = SIM_UINT("completion_pending");
    valid &= ok && completion_pending <= 1;
    uint64_t rx_loaded = SIM_UINT("rx_loaded"); valid &= ok && rx_loaded <= 1;
    uint64_t initial_raised = SIM_UINT("initial_raised"); valid &= ok && initial_raised <= 1;
    uint64_t completion_sw = SIM_UINT("completion_sw"); valid &= ok && completion_sw <= 0xFFFFu;
    uint64_t last_ctrl = SIM_UINT("last_ctrl"); valid &= ok && last_ctrl <= 0xFFFFu;
#undef SIM_UINT
    sim_profile_bytes_t profile;
    valid &= decode_b64_key(obj, "output_b64", st->output,
                            sizeof st->output) == sizeof st->output;
    valid &= decode_b64_key(obj, "output_requires_rx_b64",
                            st->output_requires_rx,
                            sizeof st->output_requires_rx) ==
             sizeof st->output_requires_rx;
    valid &= decode_b64_key(obj, "output_internal_b64", st->output_internal,
                            sizeof st->output_internal) ==
             sizeof st->output_internal;
    valid &= decode_b64_key(obj, "input_b64", st->input,
                            sizeof st->input) == sizeof st->input;
    valid &= decode_b64_key(obj, "response_b64", st->response,
                            sizeof st->response) == sizeof st->response;
    valid &= decode_b64_key(obj, "profile_b64", (uint8_t *)&profile,
                            sizeof profile) == sizeof profile;
    if (!valid) return -1;
    st->phase = (sim_phase_t)phase;
    st->due_tick = due_tick;
    st->byte_seq = byte_seq;
    st->output_head = (uint16_t)output_head;
    st->output_len = (uint16_t)output_len;
    st->input_len = (uint16_t)input_len;
    st->input_need = (uint16_t)input_need;
    st->input_kind = (uint8_t)input_kind;
    st->selected_df = (uint16_t)selected_df;
    st->selected_ef = (uint16_t)selected_ef;
    st->response_len = (uint8_t)response_len;
    st->tx_waiting = (uint8_t)tx_waiting;
    st->completion_pending = (uint8_t)completion_pending;
    st->rx_loaded = (uint8_t)rx_loaded;
    st->initial_raised = (uint8_t)initial_raised;
    st->completion_sw = (uint16_t)completion_sw;
    st->last_ctrl = (uint16_t)last_ctrl;
    sim_profile_import(st, &profile);
    st->mode = SIM_MODE_NONE;
    st->restored = 1;
    return 0;
}

static int restore_asc0(const char *doc, soc_t *soc, uint64_t schema) {
    const char *top_level = strstr(doc, "\n  \"asc0\"");
    const char *obj = top_level ? find_key(top_level, "asc0") : NULL;
    const char *object_end = json_object_end(obj);
    if (!obj || !object_end) return -1;

    const char *encoded = find_key(obj, "rx_pending_b64");
    if (!encoded || encoded >= object_end) return -1;
    while (*encoded == ' ' || *encoded == '\t') encoded++;
    if (*encoded++ != '"') return -1;
    const char *encoded_end = strchr(encoded, '"');
    if (!encoded_end || encoded_end >= object_end) return -1;
    size_t encoded_len = (size_t)(encoded_end - encoded);
    char *encoded_copy = malloc(encoded_len + 1u);
    uint8_t *pending = malloc(encoded_len ? encoded_len : 1u);
    if (!encoded_copy || !pending) {
        free(encoded_copy); free(pending);
        return -1;
    }
    memcpy(encoded_copy, encoded, encoded_len);
    encoded_copy[encoded_len] = 0;
    size_t pending_len = b64_decode(encoded_copy, pending, encoded_len);
    free(encoded_copy);

    int field_ok = 0;
    uint64_t tx_active = 0, tx_tir_raised = 0, tx_byte = 0;
    uint64_t tx_con = 0, tx_bg = 0, tx_fdv = 0, tx_frame_bits = 0;
    uint64_t tx_start_tick = 0, tx_tir_tick = 0, tx_completion_tick = 0;
    uint64_t tx_buffer_full = 0, tx_buffer_byte = 0;
    int valid = 1;
    if (schema >= SNAPSHOT_SCHEMA_ASC0_TX) {
        tx_active = key_uint(obj, "tx_active", &field_ok);
        valid &= field_ok && tx_active <= 1u;
        tx_tir_raised = key_uint(obj, "tx_tir_raised", &field_ok);
        valid &= field_ok && tx_tir_raised <= 1u;
        tx_byte = key_uint(obj, "tx_byte", &field_ok);
        valid &= field_ok && tx_byte <= 0xFFu;
        tx_con = key_uint(obj, "tx_con", &field_ok);
        valid &= field_ok && tx_con <= 0xFFFFu;
        tx_bg = key_uint(obj, "tx_bg", &field_ok);
        valid &= field_ok && tx_bg <= 0x1FFFu;
        tx_fdv = key_uint(obj, "tx_fdv", &field_ok);
        valid &= field_ok && tx_fdv <= 0x01FFu;
        tx_frame_bits = key_uint(obj, "tx_frame_bits", &field_ok);
        valid &= field_ok && tx_frame_bits <= 12u;
        tx_start_tick = key_uint(obj, "tx_start_tick", &field_ok);
        valid &= field_ok;
        tx_tir_tick = key_uint(obj, "tx_tir_tick", &field_ok);
        valid &= field_ok;
        tx_completion_tick = key_uint(obj, "tx_completion_tick", &field_ok);
        valid &= field_ok;
        tx_buffer_full = key_uint(obj, "tx_buffer_full", &field_ok);
        valid &= field_ok && tx_buffer_full <= 1u;
        tx_buffer_byte = key_uint(obj, "tx_buffer_byte", &field_ok);
        valid &= field_ok && tx_buffer_byte <= 0xFFu;
        if (tx_active) {
            valid &= tx_frame_bits > 0 && tx_start_tick <= soc->ticks &&
                     tx_tir_tick >= tx_start_tick &&
                     tx_completion_tick >= tx_tir_tick &&
                     tx_completion_tick > soc->ticks;
            if (tx_tir_raised) valid &= tx_tir_tick <= soc->ticks;
            else valid &= tx_tir_tick > soc->ticks;
        }
    }
    uint64_t rx_full = key_uint(obj, "rx_full", &field_ok);
    valid &= field_ok && rx_full <= 1u;
    uint64_t rx_active = key_uint(obj, "rx_active", &field_ok);
    valid &= field_ok && rx_active <= 1u;
    uint64_t rx_byte = key_uint(obj, "rx_byte", &field_ok);
    valid &= field_ok && rx_byte <= 0xFFu;
    uint64_t rx_con = key_uint(obj, "rx_con", &field_ok);
    valid &= field_ok && rx_con <= 0xFFFFu;
    uint64_t rx_bg = key_uint(obj, "rx_bg", &field_ok);
    valid &= field_ok && rx_bg <= 0x1FFFu;
    uint64_t rx_fdv = key_uint(obj, "rx_fdv", &field_ok);
    valid &= field_ok && rx_fdv <= 0x01FFu;
    uint64_t rx_frame_bits = key_uint(obj, "rx_frame_bits", &field_ok);
    valid &= field_ok && rx_frame_bits <= 12u;
    uint64_t rx_start_tick = key_uint(obj, "rx_start_tick", &field_ok);
    valid &= field_ok;
    uint64_t rx_completion_tick =
        key_uint(obj, "rx_completion_tick", &field_ok);
    valid &= field_ok;
    if (rx_active)
        valid &= pending_len > 0 && pending[0] == rx_byte &&
                 rx_frame_bits > 0 && rx_start_tick <= soc->ticks &&
                 rx_completion_tick > soc->ticks;
    if (!valid) {
        free(pending);
        return -1;
    }

    if (pending_len > soc->serial_rx_cap) {
        soc->serial_rx_cap = pending_len;
        soc->serial_rx = realloc(soc->serial_rx,
                                 pending_len ? pending_len : 1u);
    }
    if (pending_len) memcpy(soc->serial_rx, pending, pending_len);
    free(pending);
    soc->serial_rx_head = 0;
    soc->serial_rx_len = pending_len;

    serial_state_t *serial = soc->serial_periph->state;
    serial->tx_active = (int)tx_active;
    serial->tx_tir_raised = (int)tx_tir_raised;
    serial->tx_byte = (uint8_t)tx_byte;
    serial->tx_con = (uint16_t)tx_con;
    serial->tx_bg = (uint16_t)tx_bg;
    serial->tx_fdv = (uint16_t)tx_fdv;
    serial->tx_frame_bits = (unsigned)tx_frame_bits;
    serial->tx_start_tick = tx_start_tick;
    serial->tx_tir_tick = tx_tir_tick;
    serial->tx_completion_tick = tx_completion_tick;
    serial->tx_buffer_full = (int)tx_buffer_full;
    serial->tx_buffer_byte = (uint8_t)tx_buffer_byte;
    serial->rx_full = (int)rx_full;
    serial->rx_active = (int)rx_active;
    serial->rx_byte = (uint8_t)rx_byte;
    serial->rx_con = (uint16_t)rx_con;
    serial->rx_bg = (uint16_t)rx_bg;
    serial->rx_fdv = (uint16_t)rx_fdv;
    serial->rx_frame_bits = (unsigned)rx_frame_bits;
    serial->rx_start_tick = rx_start_tick;
    serial->rx_completion_tick = rx_completion_tick;
    return 0;
}

static int key_bool(const char *from, const char *key, int *ok) {
    const char *p = find_key(from, key);
    if (!p) { if (ok) *ok = 0; return 0; }
    while (*p == ' ' || *p == '\t') p++;
    if (!strncmp(p, "true", 4)) { if (ok) *ok = 1; return 1; }
    if (!strncmp(p, "false", 5)) { if (ok) *ok = 1; return 0; }
    if (ok) *ok = 0;
    return 0;
}

static int flash_chip_index_by_name(const memory_controller_t *memory,
                                    const char *name) {
    for (int i = 0; i < memory->n_flash_chips; i++)
        if (!strcmp(memory->flash_chips[i].name, name)) return i;
    return -1;
}

static int snapshot_flash_chip_index(const char *object,
                                     const char *object_end,
                                     const memory_controller_t *memory,
                                     unsigned seen_indices,
                                     unsigned seen_chips,
                                     int *snapshot_index,
                                     int *chip_index) {
    const char *indexp = object ? find_key(object, "index") : NULL;
    char name[64];
    int index_ok = 0;
    uint64_t index = indexp ? parse_uint_value(indexp, &index_ok) : 0;
    if (!object || !object_end || !indexp || indexp >= object_end ||
        index >= (uint64_t)memory->n_flash_chips ||
        !index_ok || !key_string(object, "name", name, sizeof name))
        return 0;
    int resolved = flash_chip_index_by_name(memory, name);
    if ((seen_indices & (1u << index)) ||
        resolved < 0 || (seen_chips & (1u << resolved)))
        return 0;
    if (snapshot_index) *snapshot_index = (int)index;
    *chip_index = resolved;
    return 1;
}

static int snapshot_mapping_matches(const char *doc, const soc_t *soc,
                                    int *chip_index_map) {
    const char *startup = find_key(doc, "prove" "nance");
    if (!startup) return 0;
    int ok = 0;
    {
        const char *chips = find_key(startup, "flash_chips");
        const char *chips_end = chips ? strchr(chips, ']') : NULL;
        if (!chips || !chips_end) return 0;
        const char *p = chips;
        int count = 0;
        unsigned seen_indices = 0;
        unsigned seen_chips = 0;
        for (int i = 0; i < MAX_FLASH_CHIPS; i++) chip_index_map[i] = -1;
        while (p < chips_end) {
            const char *object = strchr(p, '{');
            if (!object || object >= chips_end) break;
            if (count >= soc->memory.n_flash_chips) return 0;
            const char *close = json_object_end(object);
            int snapshot_index = -1;
            int chip_index = -1;
            if (!close || close >= chips_end ||
                !snapshot_flash_chip_index(
                    object, close, &soc->memory,
                    seen_indices, seen_chips,
                    &snapshot_index, &chip_index))
                return 0;
            char model[64];
            const flash_chip_config_t *chip =
                &soc->memory.flash_chips[chip_index];
            if (!key_string(object, "model", model, sizeof model) ||
                strcmp(model, chip->model) != 0)
                return 0;
            uint64_t file_offset = key_uint(object, "file_offset", &ok);
            if (!ok ||
                file_offset != soc->memory.flash_file_offsets[chip_index])
                return 0;
            uint64_t chip_size = key_uint(object, "chip_size", &ok);
            if (!ok || chip_size != chip->chip_size) return 0;
            chip_index_map[snapshot_index] = chip_index;
            seen_indices |= 1u << snapshot_index;
            seen_chips |= 1u << chip_index;
            count++;
            p = close + 1;
        }
        if (count != soc->memory.n_flash_chips ||
            seen_indices != (1u << soc->memory.n_flash_chips) - 1u ||
            seen_chips != (1u << soc->memory.n_flash_chips) - 1u)
            return 0;

        const char *windows = find_key(startup, "windows");
        const char *windows_end = windows ? strchr(windows, ']') : NULL;
        if (!windows || !windows_end) return 0;
        p = windows;
        count = 0;
        while (p < windows_end) {
            const char *basep = find_key(p, "cpu_base");
            if (!basep || basep >= windows_end) break;
            if (count >= soc->memory.n_flash_windows) return 0;
            const flash_window_config_t *w =
                &soc->memory.flash_windows[count];
            uint64_t cpu_base = parse_uint_value(basep, &ok);
            if (!ok || cpu_base != w->cpu_base) return 0;
            uint64_t cpu_size = key_uint(basep, "cpu_size", &ok);
            if (!ok || cpu_size != w->cpu_size) return 0;
            uint64_t chip_index = key_uint(basep, "chip_index", &ok);
            if (!ok || chip_index >=
                           (uint64_t)soc->memory.n_flash_chips ||
                chip_index_map[chip_index] != w->chip_index)
                return 0;
            uint64_t chip_base = key_uint(basep, "chip_base", &ok);
            if (!ok || chip_base != w->chip_base) return 0;
            uint64_t period = key_uint(basep, "mirror_period", &ok);
            if (!ok || period != w->mirror_period) return 0;
            int commands = key_bool(basep, "command_visible", &ok);
            if (!ok || commands != !!w->command_visible) return 0;
            count++;
            const char *close = strchr(basep, '}');
            if (!close || close >= windows_end) return 0;
            p = close + 1;
        }
        if (count != soc->memory.n_flash_windows) return 0;
    }

    uint64_t lm_size = key_uint(startup, "lm_size", &ok);
    if (!ok || lm_size != soc->memory.lm_size) return 0;
    uint64_t unknown1_id = key_uint(startup, "xbus_unknown1_id", &ok);
    xbus_unknown1_state_t *unknown1 =
        (xbus_unknown1_state_t *)soc->xbus_unknown1_periph->state;
    if (!ok || unknown1_id != unknown1->id) return 0;

    const char *twi_obj = find_key(startup, "twi_gpio");
    if (!twi_obj) return 0;
    while (*twi_obj == ' ' || *twi_obj == '\t' ||
           *twi_obj == '\r' || *twi_obj == '\n')
        twi_obj++;
    if (!soc->twi_gpio_periph) {
        if (strncmp(twi_obj, "null", 4) != 0) return 0;
    } else {
        const twi_gpio_state_t *twi = soc->twi_gpio_periph->state;
        char model[64];
        if (*twi_obj != '{' ||
            !key_string(twi_obj, "model", model, sizeof model) ||
            strcmp(model, soc->twi_gpio_periph->model) != 0 ||
            key_uint(twi_obj, "address", &ok) != twi->address || !ok ||
            key_uint(twi_obj, "port", &ok) != twi->port || !ok ||
            key_uint(twi_obj, "scl_bit", &ok) != twi->scl_bit || !ok ||
            key_uint(twi_obj, "sda_bit", &ok) != twi->sda_bit || !ok ||
            key_uint(twi_obj, "register_count", &ok) !=
                twi->register_count || !ok)
            return 0;
    }

    const char *ram_cfg = find_key(startup, "external_ram_config");
    const char *ram_end = ram_cfg ? strchr(ram_cfg, ']') : NULL;
    if (!ram_cfg || !ram_end) return 0;
    const char *p = ram_cfg;
    int count = 0;
    while (p < ram_end) {
        const char *modelp = find_key(p, "model");
        if (!modelp || modelp >= ram_end) break;
        if (count >= soc->memory.n_external_ram) return 0;
        external_ram_state_t *ram = cemu_memory_controller_external_ram_state(&soc->memory, count);
        char ram_model[64];
        const char *value = modelp;
        while (*value == ' ' || *value == '\t') value++;
        if (*value++ != '"') return 0;
        const char *value_end = strchr(value, '"');
        if (!value_end || (size_t)(value_end - value) >= sizeof ram_model)
            return 0;
        memcpy(ram_model, value, (size_t)(value_end - value));
        ram_model[value_end - value] = 0;
        if (strcmp(ram_model, ram->model) != 0) return 0;
        uint64_t ram_size = key_uint(modelp, "chip_size", &ok);
        if (!ok || ram_size != ram->chip_size) return 0;
        uint64_t addrsel = key_uint(modelp, "addrsel_index", &ok);
        if (!ok || addrsel != (uint64_t)ram->addrsel_index) return 0;
        count++;
        const char *end = strchr(modelp, '}');
        if (!end || end >= ram_end) return 0;
        p = end + 1;
    }
    return count == soc->memory.n_external_ram;
}

static int restore_ssc_lcd(const char *doc, soc_t *soc, uint64_t schema) {
    int ok = 0;
    const char *ssc_obj = find_key(doc, "ssc0");
    if (!ssc_obj) return -1;
    ssc0_state_t *ssc = (ssc0_state_t *)soc->ssc0_periph->state;
    ssc->config = (uint16_t)key_uint(ssc_obj, "config", &ok);
    ssc->status = (uint16_t)key_uint(ssc_obj, "status", &ok);
    ssc->rb_full = (int)key_uint(ssc_obj, "rb_full", &ok);
    ssc->shift_active = (int)key_uint(ssc_obj, "shift_active", &ok);
    ssc->shift_tx = (uint16_t)key_uint(ssc_obj, "shift_tx", &ok);
    ssc->shift_bits = (unsigned)key_uint(ssc_obj, "shift_bits", &ok);
    ssc->shift_msb_first = (int)key_uint(ssc_obj, "shift_msb_first", &ok);
    ssc->shift_start_tick = key_uint(ssc_obj, "shift_start_tick", &ok);
    ssc->completion_tick = key_uint(ssc_obj, "completion_tick", &ok);
    ssc->tb_full = (int)key_uint(ssc_obj, "tb_full", &ok);
    ssc->tb = (uint16_t)key_uint(ssc_obj, "tb", &ok);

    if (!soc->lcd_periph) return 0;
    const char *lcd_obj = find_key(doc, "lcd");
    if (!lcd_obj) return -1;
    char model[64];
    size_t stored = (size_t)key_uint(lcd_obj, "state_size", &ok);
    size_t accepted = cemu_lcd_snapshot_state_size(soc->lcd_periph,
                                                    (unsigned)schema);
    if (!key_string(lcd_obj, "model", model, sizeof model) ||
        strcmp(model, soc->lcd_periph->model) != 0 ||
        !ok || stored != accepted)
        return -1;
    uint8_t *restored = malloc(stored ? stored : 1u);
    if (!restored) return -1;
    size_t decoded = decode_b64_key(lcd_obj, "state_b64", restored, stored);
    if (decoded != stored) {
        free(restored);
        return -1;
    }
    int restored_ok = cemu_lcd_restore_snapshot_state(
        soc->lcd_periph, soc, restored, stored, (unsigned)schema);
    free(restored);
    return restored_ok ? 0 : -1;
}

static int restore_twi(const char *doc, soc_t *soc) {
    const char *top_level = strstr(doc, "\n  \"twi_gpio\"");
    const char *obj = top_level ? find_key(top_level, "twi_gpio") : NULL;
    if (!obj) return -1;
    while (*obj == ' ' || *obj == '\t' || *obj == '\r' || *obj == '\n')
        obj++;
    if (!soc->twi_gpio_periph)
        return strncmp(obj, "null", 4) == 0 ? 0 : -1;
    if (*obj != '{') return -1;

    int ok = 0;
    char model[64];
    size_t stored = (size_t)key_uint(obj, "state_size", &ok);
    if (!key_string(obj, "model", model, sizeof model) ||
        strcmp(model, soc->twi_gpio_periph->model) != 0 ||
        !ok || stored != sizeof(twi_gpio_state_t))
        return -1;

    twi_gpio_state_t restored;
    if (decode_b64_key(obj, "state_b64", (uint8_t *)&restored,
                       sizeof restored) != sizeof restored ||
        restored.phase > TWI_GPIO_MASTER_ACK_RELEASE ||
        restored.rx_kind > TWI_GPIO_RX_DATA ||
        restored.after_ack > TWI_GPIO_AFTER_TX ||
        restored.register_count > MAX_TWI_REGISTERS ||
        (restored.port != SOC_PORT_P3 && restored.port != SOC_PORT_P6 &&
         restored.port != SOC_PORT_P7 && restored.port != SOC_PORT_P8) ||
        restored.scl_bit > 15 || restored.sda_bit > 15)
        return -1;

    const twi_gpio_state_t *configured = soc->twi_gpio_periph->state;
    if (restored.address != configured->address ||
        restored.port != configured->port ||
        restored.scl_bit != configured->scl_bit ||
        restored.sda_bit != configured->sda_bit ||
        restored.register_count != configured->register_count)
        return -1;
    *(twi_gpio_state_t *)soc->twi_gpio_periph->state = restored;
    cemu_twi_gpio_finish_restore(soc->twi_gpio_periph, soc);
    return 0;
}

static m58lw064d_mode_t flash_mode_parse(const char *name) {
    if (!strcmp(name, "id")) return FLASH_ID;
    if (!strcmp(name, "status")) return FLASH_STATUS;
    if (!strcmp(name, "cfi")) return FLASH_CFI;
    return FLASH_ARRAY;
}

static m58lw064d_command_phase_t flash_command_phase_parse(const char *name) {
    if (!strcmp(name, "program-data")) return FLASH_PHASE_PROGRAM_DATA;
    if (!strcmp(name, "erase-confirm")) return FLASH_PHASE_ERASE_CONFIRM;
    return FLASH_PHASE_IDLE;
}

static m58lw064d_write_buffer_state_t flash_wb_state_parse(const char *name) {
    if (!strcmp(name, "expect-count")) return FLASH_WB_EXPECT_COUNT;
    if (!strcmp(name, "loading")) return FLASH_WB_LOADING;
    if (!strcmp(name, "expect-confirm")) return FLASH_WB_EXPECT_CONFIRM;
    return FLASH_WB_IDLE;
}

static am29lv_mode_t am29_mode_parse(const char *name) {
    if (!strcmp(name, "autoselect")) return AM29_MODE_AUTOSELECT;
    if (!strcmp(name, "secsi")) return AM29_MODE_SECSI;
    return AM29_MODE_ARRAY;
}

static am29lv_phase_t am29_phase_parse(const char *name) {
    if (!strcmp(name, "expect-55")) return AM29_PHASE_EXPECT_55;
    if (!strcmp(name, "expect-command")) return AM29_PHASE_EXPECT_COMMAND;
    if (!strcmp(name, "program")) return AM29_PHASE_PROGRAM;
    if (!strcmp(name, "secsi-exit-zero"))
        return AM29_PHASE_SECSI_EXIT_ZERO;
    if (!strcmp(name, "secsi-protect-40"))
        return AM29_PHASE_SECSI_PROTECT_40;
    if (!strcmp(name, "write-buffer-count"))
        return AM29_PHASE_WRITE_BUFFER_COUNT;
    if (!strcmp(name, "write-buffer-data"))
        return AM29_PHASE_WRITE_BUFFER_DATA;
    if (!strcmp(name, "write-buffer-confirm"))
        return AM29_PHASE_WRITE_BUFFER_CONFIRM;
    if (!strcmp(name, "erase-unlock-aa"))
        return AM29_PHASE_ERASE_UNLOCK_AA;
    if (!strcmp(name, "erase-unlock-55"))
        return AM29_PHASE_ERASE_UNLOCK_55;
    if (!strcmp(name, "erase-confirm"))
        return AM29_PHASE_ERASE_CONFIRM;
    return AM29_PHASE_IDLE;
}

static w30_phase_t w30_phase_parse(const char *name) {
    if (!strcmp(name, "program-data")) return W30_PHASE_PROGRAM_DATA;
    if (!strcmp(name, "erase-confirm")) return W30_PHASE_ERASE_CONFIRM;
    if (!strcmp(name, "lock-confirm")) return W30_PHASE_LOCK_CONFIRM;
    if (!strcmp(name, "protection-data"))
        return W30_PHASE_PROTECTION_DATA;
    if (!strcmp(name, "efp-confirm")) return W30_PHASE_EFP_CONFIRM;
    return W30_PHASE_IDLE;
}

static void parse_w30_operation(const char *obj, const char *name,
                                w30_operation_t *op, int *ok) {
    char key[64];
#define OP_FIELD(field, member, type) do { \
    snprintf(key, sizeof key, "%s_" field, name); \
    op->member = (type)key_uint(obj, key, ok); \
} while (0)
    OP_FIELD("kind", kind, w30_operation_kind_t);
    OP_FIELD("partition", partition, uint8_t);
    OP_FIELD("block", block, uint16_t);
    OP_FIELD("off", off, uint32_t);
    OP_FIELD("value", value, uint16_t);
    OP_FIELD("ticks", ticks_remaining, uint32_t);
    OP_FIELD("program_error", program_error, uint8_t);
#undef OP_FIELD
}

static int grow_array(void **ptr, size_t *cap, size_t need, size_t elem_size) {
    if (*cap >= need) return 1;
    size_t new_cap = *cap ? *cap : 8u;
    while (new_cap < need) new_cap *= 2u;
    void *new_ptr = realloc(*ptr, new_cap * elem_size);
    if (!new_ptr) return 0;
    *ptr = new_ptr;
    *cap = new_cap;
    return 1;
}

static int parse_flash_write_buffer(const char *flash_obj,
                                    m58lw064d_write_buffer_entry_t **out,
                                    size_t *count_out) {
    const char *wb = find_key(flash_obj, "write_buffer");
    const char *wb_end = wb ? strstr(wb, "\"regions\"") : NULL;
    m58lw064d_write_buffer_entry_t *entries = NULL;
    size_t count = 0, cap = 0;
    const char *p = wb;
    while (p && wb_end && p < wb_end) {
        const char *addrp = find_key(p, "off");
        int ok = 0;
        if (!addrp || addrp >= wb_end) break;
        uint32_t off = (uint32_t)parse_uint_value(addrp, &ok);
        if (!ok) break;
        uint16_t value = (uint16_t)key_uint(addrp, "value", &ok);
        if (!ok) break;
        uint8_t size = (uint8_t)key_uint(addrp, "size", &ok);
        if (!ok) break;
        if (!grow_array((void **)&entries, &cap, count + 1u, sizeof(*entries))) {
            free(entries);
            return -1;
        }
        entries[count].off = off;
        entries[count].value = value;
        entries[count].size = size;
        count++;
        p = strchr(addrp, '}');
        if (p) p++;
    }
    *out = entries;
    *count_out = count;
    return 0;
}

static int parse_flash_regions(const char *dir, const char *flash_obj,
                               m58lw064d_patch_t **out, size_t *count_out) {
    const char *regions = find_key(flash_obj, "regions");
    const char *flash_end = regions ? json_object_end(flash_obj) : NULL;
    m58lw064d_patch_t *patches = NULL;
    size_t count = 0, cap = 0;
    const char *p = regions;
    while (p && flash_end && p < flash_end) {
        const char *startp = find_key(p, "start");
        int ok = 0;
        if (!startp || startp >= flash_end) break;
        uint32_t start = (uint32_t)parse_uint_value(startp, &ok);
        if (!ok) break;
        const char *filep = find_key(startp, "file");
        if (!filep || filep >= flash_end) break;
        while (*filep == ' ' || *filep == '\t') filep++;
        if (*filep != '"') break;
        filep++;
        const char *file_end = strchr(filep, '"');
        if (!file_end) break;
        char fname[64];
        size_t fl = (size_t)(file_end - filep);
        if (fl >= sizeof fname) fl = sizeof fname - 1;
        memcpy(fname, filep, fl);
        fname[fl] = 0;
        char path[600];
        snprintf(path, sizeof path, "%s/%s", dir, fname);
        long blen = 0;
        char *blob = slurp_text(path, &blen);
        if (!blob) { free(patches); return -1; }
        if (blen > 0) {
            if (!grow_array((void **)&patches, &cap, count + (size_t)blen, sizeof(*patches))) {
                free(blob);
                free(patches);
                return -1;
            }
            for (long i = 0; i < blen; i++) {
                patches[count + (size_t)i].off = start + (uint32_t)i;
                patches[count + (size_t)i].value = (uint8_t)blob[i];
            }
            count += (size_t)blen;
        }
        free(blob);
        p = strchr(file_end, '}');
        if (p) p++;
    }
    *out = patches;
    *count_out = count;
    return 0;
}

static int restore_flash_object(const char *dir, const char *flash_obj,
                                flash_state_t *flash_state) {
    char path[512];
    int ok = 0;
    if (flash_obj && flash_state->kind == FLASH_MODEL_M58LW064D) {
        m58lw064d_state_t *m58 = cemu_flash_m58_state(flash_state);
        if (!cemu_m58lw064d_state_restore(m58, FLASH_ARRAY, FLASH_PHASE_IDLE,
                                     0x80, 0, 0, FLASH_WB_IDLE, 0,
                                     0, UINT32_MAX, NULL, 0, NULL, 0)) {
            fprintf(stderr, "error: cannot reset M58 snapshot state\n");
            return -1;
        }
        char mode[32] = "array";
        char command_phase[32] = "idle";
        char wb_state[32] = "idle";
        key_string(flash_obj, "read_mode", mode, sizeof mode);
        key_string(flash_obj, "command_phase", command_phase,
                   sizeof command_phase);
        key_string(flash_obj, "write_buffer_state", wb_state, sizeof wb_state);
        uint8_t status = (uint8_t)key_uint(flash_obj, "status", &ok);
        uint32_t cmd_writes = (uint32_t)key_uint(flash_obj, "cmd_writes", &ok);
        uint64_t erased_blocks =
            key_uint(flash_obj, "erased_blocks", &ok);
        int uid_field_ok = 0;
        int factory_uid_set = key_bool(flash_obj, "factory_uid_set", &uid_field_ok);
        char factory_uid_hex[17] = "";
        if (uid_field_ok &&
            !key_string(flash_obj, "factory_uid", factory_uid_hex,
                        sizeof factory_uid_hex)) {
            fprintf(stderr, "error: invalid M58 snapshot factory UID\n");
            return -1;
        }
        uint8_t words_left = (uint8_t)key_uint(flash_obj, "write_buffer_words_left", &ok);
        uint32_t write_buffer_block =
            (uint32_t)key_uint(flash_obj, "write_buffer_block", &ok);
        uint32_t write_buffer_page =
            (uint32_t)key_uint(flash_obj, "write_buffer_page", &ok);
        m58lw064d_write_buffer_entry_t *wb = NULL;
        size_t wb_count = 0;
        m58lw064d_patch_t *patches = NULL;
        size_t patch_count = 0;
        if (parse_flash_write_buffer(flash_obj, &wb, &wb_count) != 0
                || parse_flash_regions(dir, flash_obj, &patches, &patch_count) != 0) {
            free(wb); free(patches); return -1;
        }
        if (!cemu_m58lw064d_state_restore(
                                     m58, flash_mode_parse(mode),
                                     flash_command_phase_parse(command_phase),
                                     status, cmd_writes, erased_blocks,
                                     flash_wb_state_parse(wb_state), words_left,
                                     write_buffer_block, write_buffer_page,
                                     wb, wb_count, patches, patch_count)) {
            fprintf(stderr,
                    "error: invalid M58 snapshot state "
                    "(phase=%s wb=%s left=%u entries=%zu patches=%zu "
                    "block=%#x page=%#x status=%#x)\n",
                    command_phase, wb_state, words_left, wb_count, patch_count,
                    write_buffer_block, write_buffer_page, status);
            free(wb); free(patches); return -1;
        }
        free(wb);
        free(patches);
        if (uid_field_ok) {
            if (factory_uid_set) {
                uint8_t factory_uid[8];
                if (cemu_m58lw064d_factory_uid_parse(factory_uid_hex, factory_uid) != 0) {
                    fprintf(stderr, "error: invalid M58 snapshot factory UID\n");
                    return -1;
                }
                cemu_m58lw064d_factory_uid_set(m58, factory_uid);
            } else {
                memset(m58->factory_uid, 0, sizeof m58->factory_uid);
                m58->factory_uid_set = 0;
            }
        }
    } else if (flash_obj && flash_state->kind == FLASH_MODEL_AM29LV) {
        char mode[32] = "array";
        char phase[32] = "idle";
        key_string(flash_obj, "read_mode", mode, sizeof mode);
        key_string(flash_obj, "command_phase", phase, sizeof phase);
        uint32_t cmd_writes =
            (uint32_t)key_uint(flash_obj, "cmd_writes", &ok);
        int erase_active = key_bool(flash_obj, "erase_active", &ok);
        int erase_suspended = key_bool(flash_obj, "erase_suspended", &ok);
        uint32_t erase_sector =
            (uint32_t)key_uint(flash_obj, "erase_sector", &ok);
        uint32_t erase_timer_ticks_remaining = (uint32_t)key_uint(
            flash_obj, "erase_timer_ticks_remaining", &ok);
        uint32_t erase_ticks_remaining = (uint32_t)key_uint(
            flash_obj, "erase_ticks_remaining", &ok);
        int erase_toggle = key_bool(flash_obj, "erase_toggle", &ok);
        uint64_t erased_sectors[AM29LV_MAX_SECTOR_COUNT / 64u];
        for (size_t i = 0; i < AM29LV_MAX_SECTOR_COUNT / 64u; i++) {
            char key[32];
            snprintf(key, sizeof key, "erased_sector_bits%zu", i);
            erased_sectors[i] = key_uint(flash_obj, key, &ok);
        }
        int secsi_esn_set = key_bool(flash_obj, "secsi_esn_set", &ok);
        int secsi_locked = key_bool(flash_obj, "secsi_locked", &ok);
        uint32_t write_buffer_sector =
            (uint32_t)key_uint(flash_obj, "write_buffer_sector", &ok);
        uint32_t write_buffer_page =
            (uint32_t)key_uint(flash_obj, "write_buffer_page", &ok);
        uint8_t write_buffer_words_left =
            (uint8_t)key_uint(flash_obj, "write_buffer_words_left", &ok);
        uint8_t secsi[AM29LV_SECSI_SIZE];
        char secsi_file[96];
        if (!key_string(flash_obj, "secsi_file", secsi_file,
                        sizeof secsi_file))
            return -1;
        snprintf(path, sizeof path, "%s/%s", dir, secsi_file);
        long secsi_len = 0;
        char *secsi_blob = slurp_text(path, &secsi_len);
        if (!secsi_blob || secsi_len != AM29LV_SECSI_SIZE) {
            free(secsi_blob); return -1;
        }
        memcpy(secsi, secsi_blob, sizeof secsi);
        free(secsi_blob);

        m58lw064d_patch_t *generic_patches = NULL;
        size_t patch_count = 0;
        if (parse_flash_regions(dir, flash_obj, &generic_patches,
                                &patch_count) != 0) {
            return -1;
        }
        am29lv_patch_t *patches = NULL;
        if (patch_count) {
            patches = malloc(patch_count * sizeof(*patches));
            if (!patches) {
                free(generic_patches); return -1;
            }
            for (size_t i = 0; i < patch_count; i++) {
                patches[i].off = generic_patches[i].off;
                patches[i].value = generic_patches[i].value;
            }
        }
        free(generic_patches);
        m58lw064d_write_buffer_entry_t *generic_write_buffer = NULL;
        size_t write_buffer_len = 0;
        if (parse_flash_write_buffer(flash_obj, &generic_write_buffer,
                                     &write_buffer_len) != 0 ||
            write_buffer_len > AM29LV_WRITE_BUFFER_WORDS) {
            free(generic_write_buffer);
            free(patches); return -1;
        }
        am29lv_write_buffer_entry_t
            write_buffer[AM29LV_WRITE_BUFFER_WORDS];
        for (size_t i = 0; i < write_buffer_len; i++) {
            write_buffer[i].off = generic_write_buffer[i].off;
            write_buffer[i].value = generic_write_buffer[i].value;
            write_buffer[i].size = generic_write_buffer[i].size;
        }
        free(generic_write_buffer);
        if (!cemu_am29lv_state_restore(cemu_flash_am29_state(flash_state),
                                       am29_mode_parse(mode),
                                       am29_phase_parse(phase), cmd_writes,
                                       secsi, secsi_esn_set, secsi_locked,
                                       write_buffer_sector,
                                       write_buffer_page,
                                       write_buffer_words_left,
                                       write_buffer, write_buffer_len,
                                       erased_sectors, erase_active,
                                       erase_suspended,
                                       erase_sector,
                                       erase_timer_ticks_remaining,
                                       erase_ticks_remaining, erase_toggle,
                                       patches, patch_count)) {
            free(patches); return -1;
        }
        free(patches);
    } else if (flash_obj &&
               flash_state->kind == FLASH_MODEL_W30) {
        w30_state_t restored;
        peripheral_t dummy;
        char model[32] = "";
        if (!key_string(flash_obj, "model", model, sizeof model) ||
            strcmp(model, cemu_flash_model_str(flash_state)) != 0)
            return -1;
        if (flash_state->u.w30.config == &cemu_W30_128MBIT_TOP)
            cemu_w30_128mbit_top_periph_init(&dummy, &restored);
        else
            cemu_w30_64mbit_top_periph_init(&dummy, &restored);
        char phase[32] = "idle";
        key_string(flash_obj, "command_phase", phase, sizeof phase);
        restored.phase = w30_phase_parse(phase);
        restored.setup_partition =
            (uint8_t)key_uint(flash_obj, "setup_partition", &ok);
        restored.setup_off =
            (uint32_t)key_uint(flash_obj, "setup_off", &ok);
        restored.cmd_writes =
            (uint32_t)key_uint(flash_obj, "cmd_writes", &ok);
        for (unsigned i = 0; i < restored.config->partition_count; i++) {
            char key[32];
            snprintf(key, sizeof key, "mode%u", i);
            restored.mode[i] = (w30_mode_t)key_uint(flash_obj, key, &ok);
            snprintf(key, sizeof key, "status%u", i);
            restored.status[i] = (uint8_t)key_uint(flash_obj, key, &ok);
            snprintf(key, sizeof key, "configuration%u", i);
            restored.configuration[i] =
                (uint16_t)key_uint(flash_obj, key, &ok);
        }
        for (unsigned i = 0; i < cemu_w30_block_bitmap_words(&restored); i++) {
            char key[32];
            snprintf(key, sizeof key, "erased_bits%u", i);
            restored.erased_blocks[i] = key_uint(flash_obj, key, &ok);
            snprintf(key, sizeof key, "locked_bits%u", i);
            restored.locked_blocks[i] = key_uint(flash_obj, key, &ok);
            snprintf(key, sizeof key, "lockdown_bits%u", i);
            restored.lockdown_blocks[i] = key_uint(flash_obj, key, &ok);
        }
        restored.factory_uid_set =
            key_bool(flash_obj, "factory_uid_set", &ok);
        restored.customer_locked =
            key_bool(flash_obj, "customer_locked", &ok);
        char factory_uid[17], customer[17];
        if (!key_string(flash_obj, "factory_uid", factory_uid,
                        sizeof factory_uid) ||
            !key_string(flash_obj, "customer", customer, sizeof customer) ||
            cemu_flash_hex_parse(factory_uid, restored.factory_uid, 8) != 0 ||
            cemu_flash_hex_parse(customer, restored.customer, 8) != 0) {
            cemu_w30_state_free(&restored);
            return -1;
        }
        parse_w30_operation(flash_obj, "active", &restored.active, &ok);
        parse_w30_operation(flash_obj, "suspended_erase",
                            &restored.suspended_erase, &ok);
        parse_w30_operation(flash_obj, "suspended_program",
                            &restored.suspended_program, &ok);
        m58lw064d_patch_t *patches = NULL;
        size_t patch_count = 0;
        if (!ok || parse_flash_regions(dir, flash_obj, &patches,
                                       &patch_count) != 0 ||
            !cemu_w30_state_restore(
                cemu_flash_w30_state(flash_state), &restored,
                (const nor_patch_t *)patches, patch_count)) {
            free(patches);
            cemu_w30_state_free(&restored);
            return -1;
        }
        free(patches);
        /* Ownership transferred by state_restore through a deep copy. */
        cemu_w30_state_free(&restored);
    }
    else {
        return -1;
    }
    return 0;
}

int snapshot_read_dir(const char *dir, cpu_t *cpu, soc_t *soc) {
    char path[512]; snprintf(path, sizeof path, "%s/snapshot.json", dir);
    char *doc = slurp_text(path, NULL);
    if (!doc) return -1;

    int ok = 0;
    uint64_t schema = key_uint(doc, "schema", &ok);
    if (!ok || (schema != SNAPSHOT_SCHEMA &&
                schema != SNAPSHOT_SCHEMA_LCD_SWEEP &&
                schema != SNAPSHOT_SCHEMA_KEYPAD &&
                schema != SNAPSHOT_SCHEMA_BATTERY &&
                schema != SNAPSHOT_SCHEMA_LCD_PRESENTED &&
                schema != SNAPSHOT_SCHEMA_SIM_BASEBAND &&
                schema != SNAPSHOT_SCHEMA_ASC0_TX &&
                schema != SNAPSHOT_SCHEMA_ASC0_RX_ONLY)) {
        fprintf(stderr, "error: unsupported snapshot schema %llu\n",
                (unsigned long long)schema);
        free(doc); return -1;
    }
    int snapshot_chip_indices[MAX_FLASH_CHIPS];
    if (!snapshot_mapping_matches(doc, soc, snapshot_chip_indices)) {
        fprintf(stderr, "error: snapshot flash mapping differs from configured machine\n");
        free(doc); return -1;
    }

    /* cpu scalars: scope to the "cpu" object so keys like icount aren't matched
     * against a same-named field elsewhere. */
    const char *cpu_obj = find_key(doc, "cpu");
    if (cpu_obj) {
        cpu->csp = (uint8_t)key_uint(cpu_obj, "csp", &ok);
        cpu->ip = (uint16_t)key_uint(cpu_obj, "ip", &ok);
        cpu->ext_count = (int)key_uint(cpu_obj, "ext_count", &ok);
        cpu->extr = (int)key_uint(cpu_obj, "extr", &ok);
        cpu->ext_kind = EXT_NONE;   /* writer persists only count/extr (mirror it) */
        const char *hp = find_key(cpu_obj, "halted");
        if (hp) { while (*hp==' '||*hp=='\t') hp++; cpu->halted = strncmp(hp, "true", 4) == 0; }
        const char *ip = find_key(cpu_obj, "idle");
        if (ip) { while (*ip==' '||*ip=='\t') ip++; cpu->idle = strncmp(ip, "true", 4) == 0; }
        cpu->icount = key_uint(cpu_obj, "icount", &ok);
        cpu->interrupts_delivered = key_uint(cpu_obj, "interrupts_delivered", &ok);
        cpu->traps_taken = key_uint(cpu_obj, "traps_taken", &ok);
    }

    /* soc scalars. */
    const char *soc_obj = find_key(doc, "soc");
    if (soc_obj) {
        soc->ticks = key_uint(soc_obj, "ticks", &ok);
        soc->init_locked = key_bool(soc_obj, "init_locked", &ok);
        soc->rstout = key_bool(soc_obj, "rstout", &ok);
        soc->memory.mem_write_seq = key_uint(soc_obj, "mem_write_seq", &ok);
    }

    const char *mailbox_obj = find_key(doc, "xbus_unknown1");
    int transaction_seq_ok = 0, active_transaction_id_ok = 0;
    int phase_ok = 0, deadline_ok = 0;
    xbus_unknown1_state_t *mailbox =
        (xbus_unknown1_state_t *)soc->xbus_unknown1_periph->state;
    uint64_t transaction_seq = key_uint(
        mailbox_obj, "transaction_seq", &transaction_seq_ok);
    uint64_t active_transaction_id = key_uint(
        mailbox_obj, "active_transaction_id", &active_transaction_id_ok);
    uint64_t phase = key_uint(mailbox_obj, "phase", &phase_ok);
    uint64_t deadline = key_uint(mailbox_obj, "deadline", &deadline_ok);
    int control_ok = 0;
    uint64_t control = schema >= 39
                     ? key_uint(mailbox_obj, "control", &control_ok) : 0;
    if (schema < 39) {
        if (phase != XBUS_MAILBOX_IDLE) {
            fprintf(stderr, "error: active legacy XBUS mailbox snapshot lacks submission mode\n");
            free(doc); return -1;
        }
        control_ok = 1;
    }
    int idle_valid = phase == XBUS_MAILBOX_IDLE && deadline == 0;
    int grace_valid = phase == XBUS_MAILBOX_GRACE &&
                      (control & 5u) != 0 &&
                      active_transaction_id != 0 && deadline > soc->ticks;
    int sync_valid = phase == XBUS_MAILBOX_SYNC &&
                     (control & 5u) == 0 &&
                     active_transaction_id != 0 && deadline == 0;
    if (!mailbox_obj || !transaction_seq_ok || !active_transaction_id_ok ||
        !phase_ok || !deadline_ok || !control_ok || control > UINT16_MAX ||
        active_transaction_id != transaction_seq ||
        (!idle_valid && !grace_valid && !sync_valid)) {
        fprintf(stderr, "error: invalid XBUS mailbox snapshot state\n");
        free(doc); return -1;
    }
    mailbox->transaction_seq = transaction_seq;
    mailbox->active_transaction_id = active_transaction_id;
    mailbox->phase = (xbus_mailbox_phase_t)phase;
    mailbox->control = (uint16_t)control;
    mailbox->deadline = deadline;

    if (cemu_battery_available(soc->battery_periph)) {
        const char *battery_obj = find_key(doc, "battery");
        while (battery_obj && (*battery_obj == ' ' || *battery_obj == '\t' ||
                               *battery_obj == '\r' || *battery_obj == '\n'))
            battery_obj++;
        if (battery_obj && strncmp(battery_obj, "null", 4) == 0) {
            /* Schemas 32--35 may have been written before a device acquired
             * a calibrated battery route.  Preserve its fresh configured
             * default; schema 36 snapshots must carry explicit state. */
            if (schema >= SNAPSHOT_SCHEMA_BATTERY) {
                fprintf(stderr, "error: invalid battery snapshot state\n");
                free(doc); return -1;
            }
        } else {
            int level_ok = 0, charging_ok = 0;
            uint64_t level = key_uint(battery_obj, "level", &level_ok);
            int charging = key_bool(battery_obj, "charging", &charging_ok);
            if (!battery_obj || !level_ok || level > 100 || !charging_ok) {
                fprintf(stderr, "error: invalid battery snapshot state\n");
                free(doc); return -1;
            }
            cemu_battery_restore_state(soc->battery_periph,
                                       (unsigned)level, charging);
        }
    }

    keypad_state_t *keypad = soc->keypad_periph->state;
    if (schema >= SNAPSHOT_SCHEMA_KEYPAD) {
        const char *obj = find_key(doc, "keypad");
        int field_ok = 0;
        keypad_mutable_state_t restored = {0};
#define KEYPAD_UINT(field) do { \
    restored.field = key_uint(obj, #field, &field_ok); \
    if (!field_ok) { \
        fprintf(stderr, "error: invalid keypad snapshot state\n"); \
        free(doc); return -1; \
    } \
} while (0)
        if (!obj) {
            fprintf(stderr, "error: invalid keypad snapshot state\n");
            free(doc); return -1;
        }
        KEYPAD_UINT(scans);
        KEYPAD_UINT(handled_scans);
        KEYPAD_UINT(results);
        KEYPAD_UINT(startup_releases);
        KEYPAD_UINT(pressed);
        KEYPAD_UINT(sampled_pressed);
        KEYPAD_UINT(last_command);
        KEYPAD_UINT(last_result);
        KEYPAD_UINT(startup_power_pending);
        KEYPAD_UINT(startup_result_tagged);
        KEYPAD_UINT(startup_result_read_mask);
        KEYPAD_UINT(matrix_scan_phase);
        KEYPAD_UINT(release_activity_pending);
#undef KEYPAD_UINT
        if (!cemu_keypad_restore_mutable(keypad, &restored)) {
            fprintf(stderr, "error: invalid keypad snapshot state\n");
            free(doc); return -1;
        }
    } else {
        cemu_keypad_restore_legacy(keypad);
    }

    if (restore_sim(doc, soc, schema) != 0) {
        fprintf(stderr, "error: invalid SIM snapshot state\n");
        free(doc); return -1;
    }

    if (schema >= SNAPSHOT_SCHEMA_SIM_BASEBAND) {
        const char *bb_obj = find_key(doc, "baseband");
        int field_ok = 0;
        uint64_t pending = key_uint(bb_obj, "pending", &field_ok);
        int valid = bb_obj && field_ok && pending <= 1;
#define BB_UINT(name) key_uint(bb_obj, (name), &field_ok)
        uint64_t command = BB_UINT("command");
        valid &= field_ok && command <= 0xFFFFu;
        uint64_t pending_command = BB_UINT("pending_command");
        valid &= field_ok && pending_command <= 0xFFFFu;
        uint64_t due_tick = BB_UINT("due_tick");
        valid &= field_ok;
        uint64_t request_sequence = BB_UINT("request_sequence");
        valid &= field_ok;
        uint64_t pending_sequence = BB_UINT("pending_sequence");
        valid &= field_ok;
        uint64_t synchronized_responses =
            BB_UINT("synchronized_responses");
        valid &= field_ok;
        uint64_t result_sequence = BB_UINT("result_sequence");
        valid &= field_ok;
        uint64_t last_result_source = BB_UINT("last_result_source");
        valid &= field_ok && last_result_source <= BASEBAND_RESULT_ADC;
        uint64_t last_result_command = BB_UINT("last_result_command");
        valid &= field_ok && last_result_command <= 0xFFFFu;
        uint64_t last_result_word0 = BB_UINT("last_result_word0");
        valid &= field_ok && last_result_word0 <= 0xFFFFu;
        uint64_t last_result_word1 = BB_UINT("last_result_word1");
        valid &= field_ok && last_result_word1 <= 0xFFFFu;
        uint64_t cell_synchronized = BB_UINT("cell_synchronized");
        valid &= field_ok && cell_synchronized <= 1;
        uint64_t cell_available = BB_UINT("cell_available");
        valid &= field_ok && cell_available <= 1;
        uint64_t cell_service_allowed = BB_UINT("cell_service_allowed");
        valid &= field_ok && cell_service_allowed <= 1;
        uint64_t cell_plmn_bcd = BB_UINT("cell_plmn_bcd");
        valid &= field_ok && cell_plmn_bcd <= 0xFFFFFFu;
        uint64_t cell_arfcn = BB_UINT("cell_arfcn");
        valid &= field_ok && cell_arfcn <= 0xFFFFu;
        uint64_t cell_quality = BB_UINT("cell_quality");
        valid &= field_ok && cell_quality <= 0xFFFFu;
        uint64_t cell_level = BB_UINT("cell_level");
        valid &= field_ok && cell_level <= 0xFFFFu;
        uint64_t measurement_index = BB_UINT("measurement_index");
        valid &= field_ok && measurement_index < 102;
        uint64_t registration_events =
            BB_UINT("legacy_registration_events");
        valid &= field_ok && registration_events <= 1;
        uint64_t signal_events = BB_UINT("legacy_signal_events");
        valid &= field_ok && signal_events <= registration_events;
        uint64_t return_pc = BB_UINT("legacy_return_pc");
        valid &= field_ok && return_pc <= 0xFFFFFFu;
#undef BB_UINT
        valid &= !pending || (pending_sequence != 0 &&
                              pending_sequence <= request_sequence &&
                              due_tick != 0);
        valid &= (result_sequence == 0) ==
                 (last_result_source == BASEBAND_RESULT_NONE);
        if (!valid) {
            fprintf(stderr, "error: invalid baseband snapshot state\n");
            free(doc); return -1;
        }
        gsm_stub_state_t *bb = soc->gsm_stub_periph->state;
        bb->pending = (int)pending;
        bb->command = (uint16_t)command;
        bb->pending_command = (uint16_t)pending_command;
        bb->due_tick = due_tick;
        bb->request_sequence = request_sequence;
        bb->pending_sequence = pending_sequence;
        bb->synchronized_responses = synchronized_responses;
        bb->result_sequence = result_sequence;
        bb->last_result_source =
            (baseband_result_source_t)last_result_source;
        bb->last_result_command = (uint16_t)last_result_command;
        bb->last_result_word0 = (uint16_t)last_result_word0;
        bb->last_result_word1 = (uint16_t)last_result_word1;
        bb->cell.synchronized = (int)cell_synchronized;
        bb->cell.available = (int)cell_available;
        bb->cell.service_allowed = (int)cell_service_allowed;
        bb->cell.plmn[0] = (uint8_t)cell_plmn_bcd;
        bb->cell.plmn[1] = (uint8_t)(cell_plmn_bcd >> 8);
        bb->cell.plmn[2] = (uint8_t)(cell_plmn_bcd >> 16);
        bb->cell.arfcn = (uint16_t)cell_arfcn;
        bb->cell.quality = (uint16_t)cell_quality;
        bb->cell.level = (uint16_t)cell_level;
        bb->cell.measurement_index = (unsigned)measurement_index;
        bb->enabled = 0;
        bb->restored = 1;

        gsm_legacy_adapter_state_t *legacy =
            soc->gsm_legacy_adapter_periph->state;
        legacy->registration_events = registration_events;
        legacy->signal_events = signal_events;
        legacy->return_pc = (uint32_t)return_pc;
        legacy->enabled = 0;
        legacy->restored = 1;
    }

    /* sfr: reset the store, then set each {"0xADDR": "0xVAL"} pair. Scan the
     * "sfr" object span (up to the closing brace before "serial_tx_b64"). */
    memset(soc->memory.sfr, 0, sizeof soc->memory.sfr);
    const char *sfr_obj = find_key(doc, "sfr");
    if (sfr_obj) {
        const char *sfr_end = strstr(sfr_obj, "serial_tx_b64");
        const char *p = sfr_obj;
        while (p && (!sfr_end || p < sfr_end)) {
            const char *q = strchr(p, '"');
            if (!q || (sfr_end && q >= sfr_end)) break;
            uint32_t addr = (uint32_t)strtoul(q + 1, NULL, 0);
            const char *colon = strchr(q + 1, ':');
            if (!colon || (sfr_end && colon >= sfr_end)) break;
            const char *vq = strchr(colon, '"');
            if (!vq || (sfr_end && vq >= sfr_end)) break;
            uint32_t val = (uint32_t)strtoul(vq + 1, NULL, 0);
            if (addr >= SFR_BASE && addr < SFR_BASE + 2u * SFR_WORDS)
                cemu_memory_controller_sfr_put(&soc->memory, addr, (uint16_t)val);
            const char *vend = strchr(vq + 1, '"');
            p = vend ? vend + 1 : NULL;
        }
    }
    cemu_interrupt_subsystem_resync(&soc->interrupts);

    if (restore_asc0(doc, soc, schema) != 0) {
        fprintf(stderr, "error: cannot restore ASC0 snapshot state\n");
        free(doc); return -1;
    }

    if (restore_twi(doc, soc) != 0) {
        fprintf(stderr, "error: cannot restore GPIO-TWI snapshot state\n");
        free(doc); return -1;
    }

    if (restore_ssc_lcd(doc, soc, schema) != 0) {
        fprintf(stderr, "error: cannot restore SSC0/LCD snapshot state\n");
        free(doc); return -1;
    }

    /* serial console (base64). */
    const char *sp = find_key(doc, "serial_tx_b64");
    if (sp) {
        while (*sp == ' ' || *sp == '\t') sp++;
        if (*sp == '"') {
            sp++;
            const char *end = strchr(sp, '"');
            if (end) {
                size_t enc_len = end - sp;
                char *enc = malloc(enc_len + 1);
                memcpy(enc, sp, enc_len); enc[enc_len] = 0;
                uint8_t *dec = malloc(enc_len);   /* decoded <= encoded */
                size_t n = b64_decode(enc, dec, enc_len);
                if (n > soc->serial_tx_cap) {
                    soc->serial_tx_cap = n;
                    soc->serial_tx = realloc(soc->serial_tx, n ? n : 1u);
                }
                memcpy(soc->serial_tx, dec, n);
                soc->serial_tx_len = n;
                free(enc); free(dec);
            }
        }
    }

    /* Flash entries remain chip-relative. One-chip snapshots use `flash`;
     * multi-chip snapshots use the indexed `flash_chips` array. */
    const char *serial_obj = strstr(doc, "\"serial_tx_b64\"");
    if (soc->memory.n_flash_chips == 1) {
        const char *flash_obj = serial_obj ? find_key(serial_obj, "flash") : NULL;
        if (!flash_obj || restore_flash_object(
                dir, flash_obj, snapshot_flash(soc, 0)) != 0) {
            fprintf(stderr, "error: cannot restore flash snapshot state\n");
            free(doc); return -1;
        }
    } else {
        const char *chips = serial_obj ? find_key(serial_obj, "flash_chips") : NULL;
        const char *chips_end = json_array_end(chips);
        const char *pchip = chips;
        unsigned seen_indices = 0;
        unsigned seen_chips = 0;
        for (int i = 0; i < soc->memory.n_flash_chips; i++) {
            const char *object = pchip ? strchr(pchip, '{') : NULL;
            const char *close = json_object_end(object);
            int snapshot_index = -1;
            int chip_index = -1;
            if (!chips_end || !object || object >= chips_end || !close ||
                close >= chips_end ||
                !snapshot_flash_chip_index(
                    object, close, &soc->memory,
                    seen_indices, seen_chips,
                    &snapshot_index, &chip_index) ||
                snapshot_chip_indices[snapshot_index] != chip_index ||
                restore_flash_object(
                    dir, object, snapshot_flash(soc, chip_index)) != 0) {
                fprintf(stderr, "error: cannot restore flash chip %d state\n", i);
                free(doc); return -1;
            }
            seen_indices |= 1u << snapshot_index;
            seen_chips |= 1u << chip_index;
            pchip = close + 1;
        }
        if (seen_indices != (1u << soc->memory.n_flash_chips) - 1u ||
            seen_chips != (1u << soc->memory.n_flash_chips) - 1u) {
            fprintf(stderr, "error: incomplete flash chip snapshot state\n");
            free(doc); return -1;
        }
    }

    /* Restore physical LM before CPU-visible sparse RAM. */
    memset(soc->memory.lm, 0, soc->memory.lm_size);
    snprintf(path, sizeof path, "%s/lm_000000.bin", dir);
    long lm_len = 0;
    char *lm_blob = slurp_text(path, &lm_len);
    if (!lm_blob || lm_len != (long)soc->memory.lm_size) {
        fprintf(stderr, "error: invalid local-memory snapshot blob\n");
        free(lm_blob);
        free(doc);
        return -1;
    }
    memcpy(soc->memory.lm, lm_blob, soc->memory.lm_size);
    free(lm_blob);

    /* Restore each physical external-RAM blob before CPU-visible sparse RAM. */
    for (int i = 0; i < soc->memory.n_external_ram; i++) {
        external_ram_state_t *ram = cemu_memory_controller_external_ram_state(&soc->memory, i);
        memset(ram->bytes, 0, ram->chip_size);
        char bpath[600];
        snprintf(bpath, sizeof bpath, "%s/xram%d_000000.bin", dir, i);
        long blen = 0;
        char *blob = slurp_text(bpath, &blen);
        if (!blob || blen != (long)ram->chip_size) {
            fprintf(stderr, "error: invalid external RAM snapshot blob %d\n", i);
            free(blob);
            free(doc);
            return -1;
        }
        memcpy(ram->bytes, blob, ram->chip_size);
        free(blob);
    }

    /* Regions: clear RAM presence, then load each top-level ram_<start>.bin.
     * flash.regions is nested earlier, so don't take the first regions key. */
    memset(soc->memory.present, 0, ADDR_SPACE);
    const char *ram_regions = strstr(doc, "\n  \"regions\"");
    const char *rp = NULL;
    if (ram_regions) {
        const char *colon = strchr(ram_regions, ':');
        if (colon) rp = colon + 1;
    }
    if (!rp) rp = find_key(doc, "regions");
    if (rp) {
        const char *p = rp;
        for (;;) {
            const char *sk = find_key(p, "start");
            if (!sk) break;
            while (*sk == ' ' || *sk == '\t') sk++;
            if (*sk == '"') sk++;
            uint32_t start = (uint32_t)strtoul(sk, NULL, 0);
            const char *fk = strstr(p, "\"file\"");
            if (!fk) break;
            const char *fq = strchr(fk + 6, '"');       /* opening quote of value */
            if (!fq) break;
            const char *fend = strchr(fq + 1, '"');
            if (!fend) break;
            char fname[64]; size_t fl = fend - (fq + 1);
            if (fl >= sizeof fname) fl = sizeof fname - 1;
            memcpy(fname, fq + 1, fl); fname[fl] = 0;
            char bpath[600]; snprintf(bpath, sizeof bpath, "%s/%s", dir, fname);
            long blen = 0; char *blob = slurp_text(bpath, &blen);
            if (blob && blen > 0) {
                for (long i = 0; i < blen; i++)
                    memory_controller_ram_set(&soc->memory, (start + (uint32_t)i) & 0xFFFFFF, (uint8_t)blob[i]);
            }
            free(blob);
            p = fend + 1;
        }
    }

    free(doc);
    return 0;
}
