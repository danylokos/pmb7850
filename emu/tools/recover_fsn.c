/* recover_fsn — brute-force the 32-bit Siemens x55 handset FSN from a dump.
 *
 * The EEPROM security blocks 5008/5077 are stream-encrypted with a key derived
 * from FSN + IMEI-BCD + phone model (Freia's C45/C55 cipher, ported here from
 * refs/freia/siemens_source/SEC.C). For the correct FSN, the decrypted block's
 * own 8-bit sum/XOR check bytes (CRCBuffer) match the values stored inside the
 * decrypted block — a self-consistency oracle that needs no externally-known
 * answer. This enumerates the whole 2^32 FSN space with a fixed IMEI, gating
 * on that oracle in three stages (header -> 5008 body -> 5077 body), and prints
 * every candidate that passes all three (expected: exactly one).
 *
 * The Python tool emu/tools/siemens_tools/eeprom implements the identical cipher
 * and can verify a recovered FSN; this C tool exists because a pure-Python
 * 2^32 search is infeasible (hours-to-days) whereas this runs in minutes.
 *
 * Build:   make bin/recover_fsn       (from emu/; adds -fopenmp when available)
 * Usage:   recover_fsn <flash> --imei <14 digits>
 *              [--off-5008 0x..] [--off-5077 0x..]   (linear addresses)
 *              [--file-off-5008 0x..] [--file-off-5077 0x..]
 *          IMEI is normally decoded first with `python -m tools.siemens_tools eeprom extract --block 5009`.
 *
 * Examples:
 *   # Regression: rediscover the customized C55 FSN (0xA35F2F28)
 *   recover_fsn ../fw/C55/v.24/ff/C55lg1Sw24_мой.bin --imei 35335000894548
 *   # A55 dump (records at different offsets):
 *   recover_fsn "../fw/A55/v.09/ff/SIEMENS A55 LG7 V9 - 00800000.FLS" \
 *       --imei 35283800375615 --off-5008 0xFDEAC2 --off-5077 0xFDEBA2
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MASK32 0xFFFFFFFFu

#define B5008_LEN 0xE0
#define B5077_LEN 0xE8
#define STRANGE_HEADER_LEN 0x18
#define MAINBODY5008_CRC_LEN 0xB0
#define MAINBODY5077_CRC_LEN 0xD8

#define FLASH_LINEAR_BASE 0x00800000u
#define DEFAULT_OFF_5008 0x00FD7690u
#define DEFAULT_OFF_5077 0x00FD7770u

/* UnknownKey1 "A50/C55" row (SECLOC.H:280-283). Words 12,14,15 are runtime-patched
 * with the FSN (12) and IMEI-BCD (14..15) before the key schedule. */
static const uint32_t UNKNOWN_KEY1_C55[16] = {
    0xE5EE0A14u, 0xF8FFFBD7u, 0x03C64D76u, 0xB2F9B2FDu,
    0xA266CDB4u, 0xA410F726u, 0x43C2113Eu, 0xAFF651F4u,
    0x8C986DDCu, 0x8E588F08u, 0x8EB84FDCu, 0x4128E208u,
    0x0u, 0x08555555u, 0x0u, 0x0u,
};
static const uint32_t UNKNOWN_KEY2[6] = {
    0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0x200u, 0x0u,
};

static inline uint32_t rol32(uint32_t v, unsigned c) {
    return (v << c) | (v >> (32 - c));
}

/* SEC.C:146 ConvertToBCD: 14 IMEI digits -> 8 BCD bytes (byte0 low nibble = 0x0A). */
static void convert_to_bcd(const char *imei, uint8_t bcd[8]) {
    memset(bcd, 0, 8);
    bcd[0] = 0x0A;
    int i = 0, src = 0;
    while (i < 7) {
        bcd[i] |= (uint8_t)((imei[src++] - '0') << 4);
        i++;
        bcd[i] = (uint8_t)(imei[src++] - '0');
    }
}

/* SEC.C:357 ModifyinputArray1 — in-place mix of arr64[0..3]. */
static void modify_input_array1(uint32_t arr64[6], const uint32_t arr4c[16]) {
    uint32_t a0 = arr64[0], a1 = arr64[1], a2 = arr64[2], a3 = arr64[3];
    static const int reorder[4] = {0, 2, 1, 3};
    for (int i = 0; i < 4; i++) {
        a0 = rol32(a0 + (arr4c[i * 4] + (((a2 ^ a3) & a1) ^ a3)), 3);
        a3 = rol32(a3 + (arr4c[i * 4 + 1] + (((a1 ^ a2) & a0) ^ a2)), 7);
        a2 = rol32(a2 + (arr4c[i * 4 + 2] + (((a0 ^ a1) & a3) ^ a1)), 11);
        a1 = rol32(a1 + (arr4c[i * 4 + 3] + (((a3 ^ a0) & a2) ^ a0)), 19);
    }
    for (int i = 0; i < 4; i++) {
        a0 = rol32(a0 + (((a1 & a2) | (a1 & a3) | (a2 & a3)) + arr4c[i] + 0x5A827999u), 3);
        a3 = rol32(a3 + (((a0 & a1) | (a0 & a2) | (a1 & a2)) + arr4c[i + 4] + 0x5A827999u), 5);
        a2 = rol32(a2 + (((a3 & a0) | (a3 & a1) | (a0 & a1)) + arr4c[i + 8] + 0x5A827999u), 9);
        a1 = rol32(a1 + (((a2 & a3) | (a2 & a0) | (a3 & a0)) + arr4c[i + 12] + 0x5A827999u), 13);
    }
    for (int i = 0; i < 4; i++) {
        int j = reorder[i];
        a0 = rol32(a0 + (arr4c[j] + (a1 ^ a2 ^ a3) + 0x6ED9EBA1u), 3);
        a3 = rol32(a3 + (arr4c[j + 8] + (a0 ^ a1 ^ a2) + 0x6ED9EBA1u), 9);
        a2 = rol32(a2 + (arr4c[j + 4] + (a3 ^ a0 ^ a1) + 0x6ED9EBA1u), 11);
        a1 = rol32(a1 + (arr4c[j + 12] + (a2 ^ a3 ^ a0) + 0x6ED9EBA1u), 15);
    }
    arr64[0] += a0;
    arr64[1] += a1;
    arr64[2] += a2;
    arr64[3] += a3;
}

/* SEC.C:453 ModifyinputArray2 -> CodE2[0..3]. */
static void modify_input_array2(uint32_t arr64[6], uint32_t cod_e2[4]) {
    uint32_t arr50[16];
    memset(arr50, 0, sizeof(arr50));
    arr50[0] = 0x80;
    arr50[14] = arr64[4];
    arr50[15] = arr64[5];
    modify_input_array1(arr64, arr50);
    for (int i = 0; i < 4; i++) cod_e2[i] = arr64[i];
}

/* SEC.C CreateCodE2 with SEC.C:2503-2505 key injection. */
static void create_cod_e2(uint32_t fsn, const uint8_t bcd[8], uint32_t cod_e2[4]) {
    uint32_t arr4c[16];
    memcpy(arr4c, UNKNOWN_KEY1_C55, sizeof(arr4c));
    arr4c[12] = fsn;
    arr4c[14] = (uint32_t)bcd[0] | (uint32_t)bcd[1] << 8 | (uint32_t)bcd[2] << 16 | (uint32_t)bcd[3] << 24;
    arr4c[15] = (uint32_t)bcd[4] | (uint32_t)bcd[5] << 8 | (uint32_t)bcd[6] << 16 | (uint32_t)bcd[7] << 24;
    uint32_t arr64[6];
    memcpy(arr64, UNKNOWN_KEY2, sizeof(arr64));
    modify_input_array1(arr64, arr4c);
    modify_input_array2(arr64, cod_e2);
}

/* SEC.C:761 GosubC7D524 (RC4-style KSA), Num=0x80. cods08[0..2] are PRGA counters. */
static void gosub_c7d524(const uint32_t cod_e2[4], uint8_t cods08[259]) {
    const int num = 0x80 >> 3; /* 16 */
    cods08[0] = cods08[1] = cods08[2] = 0;
    for (int i = 0; i < 256; i++) cods08[i + 3] = (uint8_t)i;
    uint8_t cd01[16];
    for (int i = 0; i < 4; i++) {
        cd01[i * 4 + 0] = (uint8_t)cod_e2[i];
        cd01[i * 4 + 1] = (uint8_t)(cod_e2[i] >> 8);
        cd01[i * 4 + 2] = (uint8_t)(cod_e2[i] >> 16);
        cd01[i * 4 + 3] = (uint8_t)(cod_e2[i] >> 24);
    }
    uint8_t nc01 = 0;
    for (int i = 0; i < 256; i++) {
        nc01 = (uint8_t)(nc01 + cd01[i % num] + cods08[i + 3]);
        uint8_t t = cods08[i + 3];
        cods08[i + 3] = cods08[nc01 + 3];
        cods08[nc01 + 3] = t;
    }
}

/* SEC.C:816 GosubC7D5CE decrypt: out[i] = src[i] ^ keystream; carried counter
 * holds ciphertext (src) in decrypt direction. Counters persist across calls. */
static void gosub_c7d5ce_dec(uint8_t cods08[259], uint8_t *out, int idx,
                             const uint8_t *src, int num) {
    uint8_t rl6 = cods08[0], rl3 = cods08[1], rl1 = cods08[2];
    for (int i = 0; i < num; i++) {
        rl6 = (uint8_t)(rl6 + 1);
        rl1 = (uint8_t)(rl1 + cods08[rl6 + 3]);
        rl3 = (uint8_t)(rl3 + rl1);
        uint8_t rl7 = (uint8_t)(rl1 + cods08[rl3 + 3]);
        cods08[rl6 + 3] = cods08[rl3 + 3];
        cods08[rl3 + 3] = rl1;
        rl1 = src[i];
        out[idx + i] = rl1 ^ cods08[rl7 + 3];
    }
    cods08[0] = rl6;
    cods08[1] = rl3;
    cods08[2] = rl1;
}

/* SEC.C:870 CRCBuffer: 8-bit running sum, then running XOR, over len bytes. */
static void crc_check(const uint8_t *buf, int len, uint8_t *sum, uint8_t *xorv) {
    uint8_t s = 0, x = 0;
    for (int i = 0; i < len; i++) {
        s = (uint8_t)(s + buf[i]);
        x ^= buf[i];
    }
    *sum = s;
    *xorv = x;
}

/* Decrypt 5008 in the RecreateIMEISpecificBlocksC45 segment/re-seed schedule. */
static void decrypt_5008(const uint8_t *cipher, const uint32_t cod_e2[4], uint8_t out[B5008_LEN]) {
    const int h = STRANGE_HEADER_LEN;
    uint8_t s[259];
    gosub_c7d524(cod_e2, s);
    gosub_c7d5ce_dec(s, out, 0x00, cipher + 0x00, 0x08);
    gosub_c7d5ce_dec(s, out, 0x08, cipher + 0x08, h);
    gosub_c7d524(cod_e2, s);
    gosub_c7d5ce_dec(s, out, h + 8, cipher + h + 8, 0x08);
    gosub_c7d5ce_dec(s, out, h + 16, cipher + h + 16, B5008_LEN - (h + 16));
}

static void decrypt_5077(const uint8_t *cipher, const uint32_t cod_e2[4], uint8_t out[B5077_LEN]) {
    uint8_t s[259];
    gosub_c7d524(cod_e2, s);
    gosub_c7d5ce_dec(s, out, 0x00, cipher + 0x00, 0x08);
    gosub_c7d5ce_dec(s, out, 0x08, cipher + 0x08, B5077_LEN - 8);
}

/* Stage-1 oracle: decrypt only the 5008 header and check its embedded sum/XOR.
 * This is the cheap filter run over all 2^32 candidates. */
static int header_ok(const uint8_t *cipher5008, uint32_t fsn, const uint8_t bcd[8]) {
    const int h = STRANGE_HEADER_LEN;
    uint32_t cod_e2[4];
    create_cod_e2(fsn, bcd, cod_e2);
    uint8_t s[259], hdr[STRANGE_HEADER_LEN];
    gosub_c7d524(cod_e2, s);
    /* seed (8) then header (0x18); we only need the header bytes. */
    uint8_t seed[8];
    gosub_c7d5ce_dec(s, seed, 0, cipher5008, 0x08);
    gosub_c7d5ce_dec(s, hdr, 0, cipher5008 + 0x08, h);
    uint8_t sum, xorv;
    crc_check(hdr, h - 2, &sum, &xorv);
    return sum == hdr[h - 2] && xorv == hdr[h - 1];
}

/* Full oracle: all three DD2476 regions self-consistent. */
static int full_ok(const uint8_t *c5008, const uint8_t *c5077, uint32_t fsn,
                   const uint8_t bcd[8]) {
    const int h = STRANGE_HEADER_LEN;
    uint32_t cod_e2[4];
    create_cod_e2(fsn, bcd, cod_e2);
    uint8_t d5008[B5008_LEN], d5077[B5077_LEN];
    decrypt_5008(c5008, cod_e2, d5008);
    decrypt_5077(c5077, cod_e2, d5077);
    uint8_t sum, xorv;
    crc_check(d5008 + 8, h - 2, &sum, &xorv);
    if (sum != d5008[8 + h - 2] || xorv != d5008[8 + h - 1]) return 0;
    crc_check(d5008 + h + 16, MAINBODY5008_CRC_LEN, &sum, &xorv);
    if (sum != d5008[h + 16 + MAINBODY5008_CRC_LEN] ||
        xorv != d5008[h + 16 + MAINBODY5008_CRC_LEN + 1]) return 0;
    crc_check(d5077 + 8, MAINBODY5077_CRC_LEN, &sum, &xorv);
    if (sum != d5077[8 + MAINBODY5077_CRC_LEN] ||
        xorv != d5077[8 + MAINBODY5077_CRC_LEN + 1]) return 0;
    return 1;
}

static uint8_t *read_flash(const char *path, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); return NULL; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0) { fclose(f); return NULL; }
    uint8_t *buf = malloc((size_t)n);
    if (!buf || fread(buf, 1, (size_t)n, f) != (size_t)n) {
        free(buf); fclose(f); return NULL;
    }
    fclose(f);
    *out_len = (size_t)n;
    return buf;
}

int main(int argc, char **argv) {
    const char *flash_path = NULL, *imei = NULL;
    uint32_t off5008 = DEFAULT_OFF_5008, off5077 = DEFAULT_OFF_5077;
    size_t file_off5008 = 0, file_off5077 = 0;
    int have_file_off5008 = 0, have_file_off5077 = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--imei") && i + 1 < argc) imei = argv[++i];
        else if (!strcmp(argv[i], "--off-5008") && i + 1 < argc) off5008 = (uint32_t)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--off-5077") && i + 1 < argc) off5077 = (uint32_t)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--file-off-5008") && i + 1 < argc) {
            file_off5008 = (size_t)strtoull(argv[++i], NULL, 0);
            have_file_off5008 = 1;
        }
        else if (!strcmp(argv[i], "--file-off-5077") && i + 1 < argc) {
            file_off5077 = (size_t)strtoull(argv[++i], NULL, 0);
            have_file_off5077 = 1;
        }
        else if (argv[i][0] != '-') flash_path = argv[i];
        else { fprintf(stderr, "unknown arg: %s\n", argv[i]); return 2; }
    }
    if (!flash_path || !imei) {
        fprintf(stderr,
                "usage: %s <flash> --imei <14 digits> "
                "[--off-5008 0x..] [--off-5077 0x..] "
                "[--file-off-5008 0x..] [--file-off-5077 0x..]\n",
                argv[0]);
        return 2;
    }
    if (have_file_off5008 != have_file_off5077) {
        fprintf(stderr, "both file offsets must be supplied together\n");
        return 2;
    }
    size_t dlen = strlen(imei);
    if (dlen < 14) { fprintf(stderr, "IMEI needs >=14 digits\n"); return 2; }

    size_t flen = 0;
    uint8_t *flash = read_flash(flash_path, &flen);
    if (!flash) return 1;

    size_t fo5008 = have_file_off5008
        ? file_off5008 : (size_t)(off5008 - FLASH_LINEAR_BASE);
    size_t fo5077 = have_file_off5077
        ? file_off5077 : (size_t)(off5077 - FLASH_LINEAR_BASE);
    if (fo5008 + B5008_LEN > flen || fo5077 + B5077_LEN > flen) {
        fprintf(stderr, "block offset out of range for a %zu-byte image\n", flen);
        free(flash); return 1;
    }
    uint8_t c5008[B5008_LEN], c5077[B5077_LEN];
    memcpy(c5008, flash + fo5008, B5008_LEN);
    memcpy(c5077, flash + fo5077, B5077_LEN);
    free(flash);

    uint8_t bcd[8];
    convert_to_bcd(imei, bcd);

    fprintf(stderr, "flash   : %s\n", flash_path);
    fprintf(stderr, "imei    : %s\n", imei);
    if (have_file_off5008) {
        fprintf(stderr, "5008    : file 0x%zX\n", fo5008);
        fprintf(stderr, "5077    : file 0x%zX\n", fo5077);
    } else {
        fprintf(stderr, "5008    : linear 0x%X (file 0x%zX)\n",
                off5008, fo5008);
        fprintf(stderr, "5077    : linear 0x%X (file 0x%zX)\n",
                off5077, fo5077);
    }
    fprintf(stderr, "searching 2^32 FSN space...\n");

    unsigned long long stage1 = 0, found = 0;
    /* Enumerate the whole 32-bit space. header_ok is the cheap per-candidate
     * filter; only its survivors pay for the full three-region decrypt. */
#ifdef _OPENMP
#pragma omp parallel for schedule(dynamic, 0x100000) reduction(+ : stage1, found)
#endif
    for (long long p = 0; p <= 0xFFFFFFFFll; p++) {
        uint32_t fsn = (uint32_t)p;
        if (!header_ok(c5008, fsn, bcd)) continue;
        stage1++;
        if (full_ok(c5008, c5077, fsn, bcd)) {
            found++;
            printf("FSN 0x%08X\n", fsn);
            fflush(stdout);
        }
    }

    fprintf(stderr, "stage-1 (header) survivors: %llu\n", stage1);
    fprintf(stderr, "full (all 3 regions) survivors: %llu\n", found);
    return found ? 0 : 3;
}
