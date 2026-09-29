#include "common.h"

/* C55 v24's initialized voltage/percentage upper thresholds at physical LM
 * 0x272C0. Each 0--95% point is one millivolt inside its matching firmware
 * bucket; 100% extends to 4200 mV. This CEMU policy is shared by the enabled
 * x55 configurations. Its ADC endpoints match EEPROM block 67
 * synthesized-cemu-battery-v1; neither is a claim about each handset's
 * physical transfer function. */
const battery_curve_point_t cemu_device_battery_curve[
    DEVICE_CEMU_BATTERY_CURVE_COUNT] = {
    {0, 3649}, {1, 3659}, {2, 3669}, {3, 3679}, {4, 3689}, {5, 3699},
    {10, 3754}, {15, 3774}, {20, 3789}, {25, 3804}, {30, 3814},
    {35, 3824}, {40, 3834}, {45, 3849}, {50, 3859}, {55, 3879},
    {60, 3894}, {65, 3919}, {70, 3939}, {75, 3969}, {80, 3999},
    {85, 4029}, {90, 4059}, {95, 4104}, {100, 4200},
};

/* C55 table 0x936C98 and A55 table 0xA6FE04 are byte-identical. */
const keypad_button_config_t cemu_device_c55_keypad[DEVICE_C55_KEYPAD_COUNT] = {
    {"0", 0x020B, 0x30}, {"1", 0x0107, 0x31},
    {"2", 0x010B, 0x32}, {"3", 0x010D, 0x33},
    {"4", 0x0087, 0x34}, {"5", 0x008B, 0x35},
    {"6", 0x008D, 0x36}, {"7", 0x0047, 0x37},
    {"8", 0x004B, 0x38}, {"9", 0x004D, 0x39},
    {"star", 0x0207, 0x2A}, {"hash", 0x020D, 0x23},
    {"up", 0x008E, 0x3B}, {"down", 0x004E, 0x3C},
    {"soft-left", 0x010E, 0x01}, {"soft-right", 0x020E, 0x04},
    {"send", 0x0017, 0x0B}, {"power", 0x000D, 0x0C},
};

/* A60 SW27 at file 0x3A6D90 and A62 SW07 at file 0x318B94 contain
 * the same 20 raw/logical pairs. */
const keypad_button_config_t cemu_device_a60_keypad[DEVICE_A60_KEYPAD_COUNT] = {
    {"0", 0x020B, 0x30}, {"1", 0x0107, 0x31},
    {"2", 0x010B, 0x32}, {"3", 0x010D, 0x33},
    {"4", 0x0087, 0x34}, {"5", 0x008B, 0x35},
    {"6", 0x008D, 0x36}, {"7", 0x0047, 0x37},
    {"8", 0x004B, 0x38}, {"9", 0x004D, 0x39},
    {"star", 0x0207, 0x2A}, {"hash", 0x020D, 0x23},
    {"up", 0x008E, 0x3B}, {"down", 0x004E, 0x3C},
    {"left", 0x010E, 0x3D}, {"right", 0x020E, 0x3E},
    {"soft-left", 0x001E, 0x01}, {"soft-right", 0x001B, 0x04},
    {"send", 0x0017, 0x0B}, {"power", 0x000D, 0x0C},
};

/* M55 0x627F32 and S55 0x5B7E56 hold the same 23 raw/logical pairs.
 * Three entries are not exposed until their fitted controls are identified. */
const keypad_button_config_t
cemu_device_m55_s55_keypad[DEVICE_M55_S55_KEYPAD_COUNT] = {
    {"0", 0x001B, 0x30}, {"1", 0x0107, 0x31},
    {"2", 0x010B, 0x32}, {"3", 0x010D, 0x33},
    {"4", 0x0087, 0x34}, {"5", 0x008B, 0x35},
    {"6", 0x008D, 0x36}, {"7", 0x0047, 0x37},
    {"8", 0x004B, 0x38}, {"9", 0x004D, 0x39},
    {"star", 0x0017, 0x2A}, {"hash", 0x001D, 0x23},
    {"up", 0x010E, 0x3B}, {"down", 0x001E, 0x3C},
    {"left", 0x020B, 0x3D}, {"right", 0x004E, 0x3E},
    {"soft-left", 0x0207, 0x01}, {"soft-right", 0x008E, 0x04},
    {"send", 0x020E, 0x0B}, {"power", 0x000D, 0x0C},
};
