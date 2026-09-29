#include <stdio.h>

/* These names were exported by the core before Phase 3.  This fixture links
 * beside every core object: any lingering compatibility alias produces a
 * duplicate-symbol failure before the test can run. */
int cpu_reset(void) { return 1; }
int soc_init(void) { return 2; }
int memory_controller_init(void) { return 3; }
int device_by_name(void) { return 4; }
int flash_read8(void) { return 5; }
int lcd_render_rgb(void) { return 6; }

int main(void) {
    int total = cpu_reset() + soc_init() + memory_controller_init() +
                device_by_name() + flash_read8() + lcd_render_rgb();
    if (total != 21) return 1;
    puts("core namespace collision fixture: PASS");
    return 0;
}
