#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "test_support.h"
#include "emu_dispatch.h"

static char executed_path[256];
static char executed_arguments[8][128];
static int executed_count;
static int execute_result;
static int qemu_count;

static int capture_qemu(int argc, char **argv) {
    qemu_count = argc;
    for (int i = 0; i < argc && i < 8; i++)
        snprintf(executed_arguments[i], sizeof executed_arguments[i], "%s",
                 argv[i]);
    return 41;
}

static int capture_execute(const char *path, char *const arguments[]) {
    snprintf(executed_path, sizeof executed_path, "%s", path);
    executed_count = 0;
    while (arguments[executed_count] && executed_count < 8) {
        snprintf(executed_arguments[executed_count],
                 sizeof executed_arguments[executed_count], "%s",
                 arguments[executed_count]);
        executed_count++;
    }
    if (execute_result < 0) errno = ENOENT;
    return execute_result;
}

static char *stream_text(FILE *stream) {
    EMU_CHECK(fflush(stream) == 0);
    EMU_CHECK(fseek(stream, 0, SEEK_END) == 0);
    long size = ftell(stream);
    EMU_CHECK(size >= 0 && fseek(stream, 0, SEEK_SET) == 0);
    char *text = malloc((size_t)size + 1u);
    EMU_CHECK(text != NULL);
    EMU_CHECK(fread(text, 1, (size_t)size, stream) == (size_t)size);
    text[size] = 0;
    return text;
}

static int dispatch(emu_dispatch_entry_t entry, int count, char **arguments,
                    FILE *output, FILE *errors) {
    executed_path[0] = 0;
    executed_count = 0;
    execute_result = 73;
    return emu_dispatch(entry, count, arguments, "/r/plain", "/r/inst",
                        NULL, capture_execute, output, errors);
}

static void test_help(void) {
    FILE *output = tmpfile(), *errors = tmpfile();
    EMU_CHECK(output && errors);
    char *top[] = {"emu", "--help"};
    EMU_CHECK(dispatch(EMU_ENTRY_CANONICAL, 2, top, output, errors) == 0);
    char *text = stream_text(output);
    EMU_CHECK(strstr(text, "emu <command>"));
    EMU_CHECK(strstr(text, "run    boot"));
    EMU_CHECK(!executed_path[0]);
    free(text);
    fclose(output); fclose(errors);

    output = tmpfile(); errors = tmpfile();
    char *run[] = {"emu", "run", "--help"};
    EMU_CHECK(dispatch(EMU_ENTRY_CANONICAL, 3, run, output, errors) == 0);
    text = stream_text(output);
    EMU_CHECK(strstr(text, "usage: emu run <fullflash.bin>"));
    EMU_CHECK(strstr(text, "--monitor"));
    EMU_CHECK(!executed_path[0]);
    free(text);
    fclose(output); fclose(errors);
}

static void test_profile_selection(void) {
    FILE *output = tmpfile(), *errors = tmpfile();
    EMU_CHECK(output && errors);
    char *plain[] = {
        "emu", "run", "flash.bin", "--engine", "cemu",
        "--limit", "2m",
    };
    EMU_CHECK(dispatch(EMU_ENTRY_CANONICAL, 7, plain, output, errors) == 73);
    EMU_CHECK(!strcmp(executed_path, "/r/plain"));
    EMU_CHECK(executed_count == 4);
    EMU_CHECK(!strcmp(executed_arguments[1], "flash.bin"));
    EMU_CHECK(!strcmp(executed_arguments[2], "--limit"));
    fclose(output); fclose(errors);

    output = tmpfile(); errors = tmpfile();
    char *instrumented[] = {
        "emu", "run", "--engine", "cemu", "flash.bin", "--drcov",
    };
    EMU_CHECK(dispatch(EMU_ENTRY_CANONICAL, 6, instrumented,
                       output, errors) == 73);
    EMU_CHECK(!strcmp(executed_path, "/r/inst"));
    EMU_CHECK(executed_count == 3);
    fclose(output); fclose(errors);

    output = tmpfile(); errors = tmpfile();
    char *resume[] = {
        "emu", "run", "--from-snapshot", "snapshot-dir",
    };
    EMU_CHECK(dispatch(EMU_ENTRY_CANONICAL, 4, resume,
                       output, errors) == 73);
    EMU_CHECK(!strcmp(executed_path, "/r/inst"));
    fclose(output); fclose(errors);

    output = tmpfile(); errors = tmpfile();
    char *value_named_engine[] = {
        "emu", "run", "flash.bin", "--label", "--engine",
    };
    EMU_CHECK(dispatch(EMU_ENTRY_CANONICAL, 5, value_named_engine,
                       output, errors) == 73);
    EMU_CHECK(!strcmp(executed_path, "/r/inst"));
    EMU_CHECK(executed_count == 4);
    EMU_CHECK(!strcmp(executed_arguments[3], "--engine"));
    fclose(output); fclose(errors);

    output = tmpfile(); errors = tmpfile();
    char *qemu[] = {"emu", "run", "--engine", "qemu", "flash.bin"};
    EMU_CHECK(emu_dispatch(EMU_ENTRY_CANONICAL, 5, qemu,
                           "/r/plain", "/r/inst", capture_qemu,
                           capture_execute, output, errors) == 41);
    EMU_CHECK(qemu_count == 2);
    EMU_CHECK(!strcmp(executed_arguments[1], "flash.bin"));
    fclose(output); fclose(errors);

    output = tmpfile(); errors = tmpfile();
    char *qemu_gdb[] = {
        "emu", "run", "--engine", "qemu", "flash.bin", "--gdb", "4321",
    };
    EMU_CHECK(emu_dispatch(EMU_ENTRY_CANONICAL, 7, qemu_gdb,
                           "/r/plain", "/r/inst", capture_qemu,
                           capture_execute, output, errors) == 41);
    EMU_CHECK(qemu_count == 4);
    EMU_CHECK(!strcmp(executed_arguments[1], "flash.bin"));
    EMU_CHECK(!strcmp(executed_arguments[2], "--gdb"));
    EMU_CHECK(!strcmp(executed_arguments[3], "4321"));
    fclose(output); fclose(errors);
}

static void test_legacy_and_errors(void) {
    FILE *output = tmpfile(), *errors = tmpfile();
    EMU_CHECK(output && errors);
    char *legacy[] = {"cemu", "flash.bin", "--drcov", NULL};
    EMU_CHECK(dispatch(EMU_ENTRY_LEGACY_PLAIN, 3, legacy,
                       output, errors) == 73);
    EMU_CHECK(!strcmp(executed_path, "/r/plain"));
    EMU_CHECK(dispatch(EMU_ENTRY_LEGACY_INSTRUMENTED, 3, legacy,
                       output, errors) == 73);
    EMU_CHECK(!strcmp(executed_path, "/r/inst"));
    fclose(output); fclose(errors);

    output = tmpfile(); errors = tmpfile();
    char *duplicate[] = {
        "emu", "run", "--engine", "cemu", "--engine", "cemu",
    };
    EMU_CHECK(dispatch(EMU_ENTRY_CANONICAL, 6, duplicate,
                       output, errors) == 2);
    char *text = stream_text(errors);
    EMU_CHECK(strstr(text, "duplicate option: --engine"));
    free(text);
    fclose(output); fclose(errors);

    output = tmpfile(); errors = tmpfile();
    char *unknown[] = {"emu", "run", "--engine", "qemu", "flash.bin"};
    EMU_CHECK(dispatch(EMU_ENTRY_CANONICAL, 5, unknown,
                       output, errors) == 2);
    text = stream_text(errors);
    EMU_CHECK(!strcmp(text, "error: unavailable engine: qemu\n"));
    free(text);
    fclose(output); fclose(errors);

    output = tmpfile(); errors = tmpfile();
    execute_result = -1;
    char *missing[] = {"emu", "run", "flash.bin"};
    executed_path[0] = 0;
    EMU_CHECK(emu_dispatch(EMU_ENTRY_CANONICAL, 3, missing,
                           "/r/plain", "/r/inst", NULL, capture_execute,
                           output, errors) == 2);
    text = stream_text(errors);
    EMU_CHECK(strstr(text, "cannot execute /r/plain: "));
    free(text);
    fclose(output); fclose(errors);
}

int main(void) {
    test_help();
    test_profile_selection();
    test_legacy_and_errors();
    puts("canonical runtime dispatch: PASS");
    return 0;
}
