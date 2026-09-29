/* Byte naming authority: C166S V1 PDF pp.82-85; M166 pp.33,83-85. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "disasm.h"
#include "synth.h"

static soc_t soc;
static cpu_t cpu;

static void format(const uint8_t *code, int length, const char *expected)
{
    char text[128];
    for (int i = 0; i < length; i++)
        cemu_memory_controller_poke8(&soc.memory, 0xf600 + i, code[i]);
    int actual = cpu_disasm_at(&cpu, &soc, 0xf600, text, sizeof text);
    if (actual != length || strcmp(text, expected)) {
        fprintf(stderr, "expected %s (%d), got %s (%d)\n",
                expected, length, text, actual);
        abort();
    }
}

int main(void)
{
    uint8_t *flash = calloc(8*1024*1024, 1);
    assert(flash);
    device_config_t cfg = *cemu_device_by_name("c55");
    cemu_soc_init(&soc, flash, 8*1024*1024, &cfg, synth_defaults(), 0);
    cemu_cpu_init(&cpu, &soc.bus);
    cemu_soc_attach_cpu(&soc, &cpu);
    cemu_cpu_reset(&cpu);
    for (int i = 0; i < 16; i++) {
        char name[8], expected[80];
        snprintf(name, sizeof name, "r%c%d", i & 1 ? 'h' : 'l', i/2);
        uint8_t immediate[] = {0xe7, 0xf0+i, 0x5a, 0};
        snprintf(expected, sizeof expected, "movb %s,#0x5a", name);
        format(immediate, 4, expected);
        for (int j = 0; j < 16; j++) cemu_cpu_set_gpr(&cpu, j, 0xa1b2);
        cpu.csp = 0; cpu.ip = 0xf600;
        cemu_cpu_step(&cpu);
        for (int j = 0; j < 16; j++)
            assert(cemu_cpu_gpr(&cpu, j) == (j == i/2 ?
                   (i & 1 ? 0x5ab2 : 0xa15a) : 0xa1b2));
        const uint8_t forms[][4] = {
            {0xe1, 0x50+i}, {0xf1, i*16+i}, {0xf3, 0xf0+i, 0x34, 0x12},
            {0xf7, 0xf0+i, 0x34, 0x12}, {0xc5, 0xf0+i, 0x34, 0x12},
            {0xd5, 0xf0+i, 0x34, 0x12}, {0xc0, i*16+15}, {0xd0, i*16+15},
            {0xa9, i*16+15}, {0x99, i*16+15}, {0xb9, i*16+15},
            {0x89, i*16+15}, {0xf4, i*16+15, 0x34, 0x12},
            {0xe4, i*16+15, 0x34, 0x12}, {0xa1, i*16}, {0xb1, i*16},
        };
        const char *formats[] = {
            "movb %s,#0x5", "movb %s,%s", "movb %s,0x1234",
            "movb 0x1234,%s", "movbz 0x1234,%s", "movbs 0x1234,%s",
            "movbz r15,%s", "movbs r15,%s", "movb %s,[r15]",
            "movb %s,[r15+]", "movb [r15],%s", "movb [-r15],%s",
            "movb %s,[r15+#0x1234]", "movb [r15+#0x1234],%s",
            "negb %s", "cplb %s",
        };
        for (unsigned j = 0; j < sizeof forms / sizeof forms[0]; j++) {
            snprintf(expected, sizeof expected, formats[j], name, name);
            int len = (j >= 2 && j <= 5) || j == 12 || j == 13 ? 4 : 2;
            format(forms[j], len, expected);
        }
        const char *alu[] = {"addb","addcb","subb","subcb","cmpb","xorb","andb","orb"};
        for (int j = 0; j < 8; j++) {
            uint8_t rr[] = {j*16+1, i*16+i};
            snprintf(expected, sizeof expected, "%s %s,%s", alu[j], name, name);
            format(rr, 2, expected);
            uint8_t ri[] = {j*16+7, 0xf0+i, 0x5a, 0xaa};
            snprintf(expected, sizeof expected, "%s %s,#0x5a", alu[j], name);
            format(ri, 4, expected);
            uint8_t ind[] = {j*16+9, i*16+15};
            snprintf(expected, sizeof expected, "%s %s,[r3+]", alu[j], name);
            format(ind, 2, expected);
        }
    }
    uint8_t word[] = {0xd2, 0xf1, 0x34, 0x12};
    format(word, 4, "movbs r1,0x1234");
    uint8_t sfr[] = {0xe7, 0x88, 0x5a, 0};
    cpu.extr = 0; format(sfr, 4, "movb PSW,#0x5a");
    cpu.extr = 1; format(sfr, 4, "movb 0xf110,#0x5a");
    cemu_soc_free(&soc);
    free(flash);
    puts("PASS CEMU byte formatting and neighbor preservation (16 indices)");
    return 0;
}
