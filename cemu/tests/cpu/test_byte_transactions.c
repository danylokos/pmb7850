/* M166 PDF p.83; C166S V1 PDF pp.82-83: Rb address = CP + n. */
#include "test_harness.h"

static bus_transaction_t accesses[32];
static int count;
static void record(void *ctx, bus_transaction_t *txn) {
    if (txn->addr >= 0xfc00 && txn->addr < 0xfc20 && count < 32)
        accesses[count++] = *txn;
    fs_access(ctx, txn);
}
static void test_byte_transactions(void) {
    for (int n = 0; n < 16; n++) {
        SET_MEM16(0xfc00 + (n & ~1), 0x5678);
        g_bus.access = record;
        count = 0;
        uint8_t code[] = {0xe7, 0xf0 + n, 0xa5, 0};
        run_code(code, sizeof code, 0x2000);
        CHECK(count == 1);
        CHECK(accesses[0].kind == BUS_ACCESS_WRITE);
        CHECK(accesses[0].addr == 0xfc00u + n);
        CHECK(accesses[0].size == 1);
        CHECK(MEM8(0xfc00 + n) == 0xa5);
        CHECK(MEM8(0xfc00 + (n ^ 1)) == ((n & 1) ? 0x78 : 0x56));
        count = 0;
        code[0] = 0xf7; code[2] = 0; code[3] = 0x30;
        run_code(code, sizeof code, 0x2000);
        CHECK(count == 1);
        CHECK(accesses[0].kind == BUS_ACCESS_READ);
        CHECK(accesses[0].addr == 0xfc00u + n);
        CHECK(accesses[0].size == 1);
        CHECK(MEM8(0x3000) == 0xa5);
    }
    CHECK(g_cpu.icount == 32);
}
int main(void) {
    const test_entry_t tests[] = {{"byte_transactions", test_byte_transactions}};
    return run_all(tests, 1);
}
