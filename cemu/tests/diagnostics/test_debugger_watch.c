/* Native command policy, independent of GDB expression parsing. */
#include <assert.h>
#include <string.h>
#include "soc.h"
#include "synth.h"
#include "debugger.h"

static soc_t soc;
static cpu_t cpu;
static debugger_t dbg;
static FILE *output;
static void command(const char *text) { debugger_run_script(&dbg, text, output); }
static void program(void) {
    /* MOVB rh0,#0x12 (unchanged); MOVB rh0,#0x34; MOVB 0xf601,rh0;
     * MOVB rh1,0xf601; NOP. M166 PDF p.83. */
    const uint8_t bytes[] = {0xe7,0xf1,0x12,0,0xe7,0xf1,0x34,0,
                            0xf7,0xf1,1,0xf6,0xf3,0xf3,1,0xf6,0xcc,0};
    debugger_write_mem(&dbg, 0xf600, bytes, sizeof bytes);
    debugger_set_reg(&dbg, "pc", 0xf600);
    debugger_set_gpr(&dbg, 0, 0x1278);
}
int main(void) {
    uint8_t flash[4] = {0xff,0xff,0xff,0xff};
    cemu_soc_init(&soc, flash, sizeof flash, cemu_device_by_name("c55"), synth_defaults(), 0);
    cemu_cpu_init(&cpu, &soc.bus);
    cemu_soc_attach_cpu(&soc, &cpu);
    cemu_cpu_reset(&cpu);
    debugger_init(&dbg, &cpu, &soc, 0);
    output = tmpfile(); assert(output);
    program();
    command("watch 0xfc01");
    stop_reason_t stop = debugger_step(&dbg, 1);
    assert(!strcmp(stop.kind, "step") && cpu.icount == 1);
    stop = debugger_step(&dbg, 1);
    assert(!strcmp(stop.kind, "watch") && cpu.icount == 2);
    assert(stop.pc == 0xf608 && stop.trigger_pc == 0xf604);
    assert(stop.value_change && stop.old_value == 0x12 && stop.new_value == 0x34);
    char short_text[9];
    memset(short_text, 'X', sizeof short_text);
    stop_reason_str(&stop, short_text, 8);
    assert(short_text[7] == 0 && short_text[8] == 'X');
    debugger_checkpoint(&dbg);
    debugger_set_reg(&dbg, "pc", 0xf604);
    debugger_set_gpr(&dbg, 0, 0x3478);
    stop = debugger_step(&dbg, 1); assert(!strcmp(stop.kind, "step"));
    assert(debugger_restore(&dbg));
    debugger_set_reg(&dbg, "pc", 0xf604);
    stop = debugger_step(&dbg, 1); assert(!strcmp(stop.kind, "step"));
    debugger_clear_watches(&dbg);
    program();
    command("awatch 0xfc00"); /* rh0 must not read or write its neighbor */
    stop = debugger_step(&dbg, 2); assert(!strcmp(stop.kind, "step"));
    debugger_clear_watches(&dbg);
    program();
    command("rwatch 0xfc01");
    stop = debugger_step(&dbg, 2); assert(!strcmp(stop.kind, "step"));
    stop = debugger_step(&dbg, 1); assert(!strcmp(stop.kind, "watch"));
    assert(stop.trigger_pc == 0xf608 && stop.pc == 0xf60c);
    debugger_clear_watches(&dbg);
    program();
    command("watch 0xfc01 w");
    stop = debugger_step(&dbg, 1); assert(!strcmp(stop.kind, "watch"));
    debugger_clear_watches(&dbg);
    program();
    debugger_add_watch(&dbg, 0xfc01, 0xfc01, WK_MEM_WRITE, 0);
    stop = debugger_step(&dbg, 1); assert(!strcmp(stop.kind, "watch"));
    debugger_clear_watches(&dbg);
    program();
    command("wlog 0xfc01 w");
    stop = debugger_step(&dbg, 2); assert(!strcmp(stop.kind, "step"));
    assert(dbg.watch_log_count == 2);
    debugger_clear_watches(&dbg);
    program();
    command("awatch 0xf600..0xf603"); /* exclude fetch */
    stop = debugger_step(&dbg, 1); assert(!strcmp(stop.kind, "step"));
    debugger_clear_watches(&dbg);
    /* SCXT SP,0xf700 temporarily decrements SP, then restores its value.
     * The complete instruction leaves the watched word unchanged. */
    const uint8_t scxt[] = {0xd6,0x09,0x00,0xf7};
    debugger_write_mem(&dbg, 0xf600, scxt, sizeof scxt);
    debugger_write_word(&dbg, 0xf700, 0xfc00);
    debugger_set_reg(&dbg, "sp", 0xfc00);
    debugger_set_reg(&dbg, "pc", 0xf600);
    command("watch 0xfe12..0xfe13");
    stop = debugger_step(&dbg, 1); assert(!strcmp(stop.kind, "step"));
    assert(debugger_read_word(&dbg, 0xfe12) == 0xfc00);
    debugger_detach(&dbg); debugger_free(&dbg); cemu_soc_free(&soc); fclose(output);
    puts("PASS native value/access watches and byte transactions");
    return 0;
}
