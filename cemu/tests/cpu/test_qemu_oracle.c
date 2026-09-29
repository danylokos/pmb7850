/* Bounded external CPU-state oracle for the independent QEMU C166 tests. */
#include "test_harness.h"

#include <ctype.h>

static int hex_nibble(char ch)
{
    if (ch >= '0' && ch <= '9') {
        return ch - '0';
    }
    ch = (char)tolower((unsigned char)ch);
    return ch >= 'a' && ch <= 'f' ? ch - 'a' + 10 : -1;
}

static int write_hex(uint32_t address, const char *hex)
{
    size_t length = strlen(hex);

    if ((length & 1) || address + length / 2 > FAKE_SIZE) {
        return -1;
    }
    for (size_t i = 0; i < length; i += 2) {
        int high = hex_nibble(hex[i]);
        int low = hex_nibble(hex[i + 1]);

        if (high < 0 || low < 0) {
            return -1;
        }
        SET_MEM8(address + i / 2, (high << 4) | low);
    }
    return 0;
}

static int set_initial(const char *name, uint16_t value)
{
    if (name[0] == 'r' && isdigit((unsigned char)name[1])) {
        char *end;
        unsigned long reg = strtoul(name + 1, &end, 10);

        if (*end || reg > 15) {
            return -1;
        }
        SET_GPR(reg, value);
        return 0;
    }
    if (!strcmp(name, "dpp0") || !strcmp(name, "dpp1") ||
        !strcmp(name, "dpp2") || !strcmp(name, "dpp3")) {
        SET_MEM16(0xfe00 + 2 * (name[3] - '0'), value & 0x3ff);
    } else if (!strcmp(name, "cp")) {
        SET_MEM16(0xfe10, value & 0xfffe);
    } else if (!strcmp(name, "sp")) {
        SET_MEM16(0xfe12, value & 0xfffe);
    } else if (!strcmp(name, "stkov")) {
        SET_MEM16(0xfe14, value & 0xfffe);
    } else if (!strcmp(name, "stkun")) {
        SET_MEM16(0xfe16, value & 0xfffe);
    } else if (!strcmp(name, "mdh")) {
        SET_MEM16(0xfe0c, value);
    } else if (!strcmp(name, "mdl")) {
        SET_MEM16(0xfe0e, value);
    } else if (!strcmp(name, "mdc")) {
        SET_MEM16(0xff0e, value);
    } else if (!strcmp(name, "psw")) {
        SET_PSW(value);
    } else if (!strcmp(name, "csp")) {
        g_cpu.csp = value & 0xff;
    } else if (!strcmp(name, "ip")) {
        g_cpu.ip = value;
    } else {
        return -1;
    }
    return 0;
}

static void print_state(char **touches, int touch_count)
{
    printf("{\"pc\":%u,\"csp\":%u,\"ip\":%u,", PC(), CSP(), IP());
    printf("\"gpr\":[");
    for (int i = 0; i < 16; i++) {
        printf("%s%u", i ? "," : "", GPR(i));
    }
    printf("],\"dpp\":[%u,%u,%u,%u],",
           MEM16(0xfe00), MEM16(0xfe02),
           MEM16(0xfe04), MEM16(0xfe06));
    printf("\"cp\":%u,\"sp\":%u,\"psw\":%u,",
           MEM16(0xfe10), MEM16(0xfe12), PSW());
    printf("\"mdh\":%u,\"mdl\":%u,\"mdc\":%u,",
           MEM16(0xfe0c), MEM16(0xfe0e), MEM16(0xff0e));
    printf("\"ext_kind\":%u,\"ext_value\":%u,",
           g_cpu.ext_kind, g_cpu.ext_val);
    printf("\"ext_count\":%d,\"extr\":%d,\"guest_icount\":%llu,",
           g_cpu.ext_count, g_cpu.extr,
           (unsigned long long)g_cpu.icount);
    printf("\"memory\":{");
    for (int i = 0; i < touch_count; i++) {
        char *colon = strchr(touches[i], ':');
        unsigned long address;
        unsigned long length;

        *colon = '\0';
        address = strtoul(touches[i], NULL, 16);
        length = strtoul(colon + 1, NULL, 0);
        printf("%s\"%06lx\":\"", i ? "," : "", address);
        for (unsigned long j = 0; j < length; j++) {
            printf("%02x", MEM8(address + j));
        }
        printf("\"");
    }
    printf("}}\n");
}

int main(int argc, char **argv)
{
    char *touches[32];
    int touch_count = 0;
    unsigned long steps;

    if (argc == 1) {
        puts("qemu oracle: invoke through run_differential.py");
        return 0;
    }
    if (argc < 3) {
        return 2;
    }
    make_cpu();
    SET_MEM16(0xfe12, 0xfc00);
    if (write_hex(0, argv[1]) < 0) {
        return 2;
    }
    steps = strtoul(argv[2], NULL, 0);
    for (int i = 3; i < argc; i++) {
        char *equal = strchr(argv[i], '=');

        if (!strncmp(argv[i], "touch=", 6)) {
            if (touch_count == 32 || !strchr(argv[i] + 6, ':')) {
                return 2;
            }
            touches[touch_count++] = argv[i] + 6;
        } else if (!strncmp(argv[i], "mem", 3) && equal) {
            *equal = '\0';
            if (write_hex(strtoul(argv[i] + 3, NULL, 16), equal + 1) < 0) {
                return 2;
            }
        } else if (equal) {
            *equal = '\0';
            if (set_initial(argv[i], strtoul(equal + 1, NULL, 16)) < 0) {
                return 2;
            }
        } else {
            return 2;
        }
    }
    for (unsigned long i = 0; i < steps; i++) {
        if (STEP() == STEP_UNIMPL) {
            return 3;
        }
    }
    print_state(touches, touch_count);
    return 0;
}
