#include <string.h>
#include "emu_input.h"
#include "test_support.h"

typedef struct {
    int down[2], sampled[2], fail_set[2], fail_query, calls[2], edges;
    emu_input_event_t last;
} backend_t;
static int set(void *p, const char *name, int down) {
    backend_t *b = p; int k = name[0] == 'b';
    b->calls[k]++;
    if (b->fail_set[k]) return -7;
    b->edges += b->down[k] != down; b->down[k] = down;
    return 0;
}
static int sampled(void *p, const char *name, int *v) {
    backend_t *b = p;
    if (b->fail_query) return -8;
    *v = b->sampled[name[0] == 'b']; return 0;
}
static void trace(void *p, const emu_input_event_t *e) { ((backend_t *)p)->last = *e; }
static emu_input_t *create(backend_t *b) {
    const char *keys[] = {"a", "b"};
    emu_input_callbacks_t cb = {.opaque = b, .set = set, .sampled = sampled, .trace = trace};
    return emu_input_create(keys, 2, &cb);
}
static void test_ownership(void) {
    backend_t b = {0}; emu_input_t *s = create(&b);
    uint64_t scope = emu_input_scope_open(s), other = emu_input_scope_open(s);
    uint64_t a = emu_input_owner_open(s, scope), c = emu_input_owner_open(s, scope);
    uint64_t d = emu_input_owner_open(s, other);
    emu_input_context(s, 123, 0x800123);
    EMU_CHECK(emu_input_key(s, a, 0, EMU_INPUT_PRESS) == 0);
    EMU_CHECK(b.last.icount == 123 && b.last.pc == 0x800123 && b.last.owner == a);
    EMU_CHECK(emu_input_key(s, a, 0, EMU_INPUT_PRESS) == 0 && b.calls[0] == 1);
    EMU_CHECK(emu_input_key(s, c, 0, EMU_INPUT_PRESS) == 0 && b.edges == 1);
    b.fail_query = 1; /* Nonlast release does not query sampling. */
    EMU_CHECK(emu_input_key(s, a, 0, EMU_INPUT_RELEASE_SAMPLED) == 0 && b.down[0]);
    b.fail_query = 0;
    EMU_CHECK(emu_input_key(s, c, 0, EMU_INPUT_RELEASE_SAMPLED) == 0);
    EMU_CHECK(emu_input_pending(s) && !emu_input_owner_holds(s, c, 0));
    for (int i = 0; i < 10; i++) {
        emu_input_context(s, UINT64_C(1000000000) * i, 0);
        EMU_CHECK(emu_input_poll(s) == 0 && b.down[0]);
    }
    EMU_CHECK(emu_input_key(s, d, 0, EMU_INPUT_PRESS) == 0 && !emu_input_pending(s));
    EMU_CHECK(b.edges == 1 && b.calls[0] == 1);
    EMU_CHECK(emu_input_scope_close(s, scope) == 0 && b.down[0]);
    EMU_CHECK(emu_input_key(s, d, 0, EMU_INPUT_RELEASE_SAMPLED) == 0);
    EMU_CHECK(emu_input_owner_close(s, d) == 0 && !b.down[0] && !emu_input_pending(s));
    EMU_CHECK(emu_input_key(s, d, 0, EMU_INPUT_PRESS) == -1);
    d = emu_input_owner_open(s, other);
    EMU_CHECK(emu_input_key(s, d, 0, EMU_INPUT_PRESS) == 0);
    b.sampled[0] = 1;
    EMU_CHECK(emu_input_key(s, d, 0, EMU_INPUT_RELEASE_SAMPLED) == 0 && !b.down[0]);
    b.sampled[0] = 0;
    EMU_CHECK(emu_input_key(s, d, 0, EMU_INPUT_PRESS) == 0);
    EMU_CHECK(emu_input_key(s, d, 0, EMU_INPUT_RELEASE_SAMPLED) == 0);
    b.sampled[0] = 1;
    EMU_CHECK(emu_input_poll(s) == 0 && !b.down[0]);
    EMU_CHECK(emu_input_key(s, d, 0, EMU_INPUT_RELEASE) == 0);
    EMU_CHECK(emu_input_key(s, d, 2, EMU_INPUT_PRESS) == -1);
    EMU_CHECK(emu_input_key(s, d, 0, (emu_input_action_t)3) == -1);
    EMU_CHECK(emu_input_scope_close(s, other) == 0);
    EMU_CHECK(emu_input_owner_open(s, other) == 0);
    EMU_CHECK(emu_input_destroy(s) == 0);
}
static void test_failure_and_restore(void) {
    backend_t b = {.down = {1, 0}}; emu_input_t *s = create(&b);
    uint64_t scope = emu_input_scope_open(s), a = emu_input_owner_open(s, scope);
    EMU_CHECK(b.calls[0] == 0 && emu_input_down_mask(s) == 0);
    EMU_CHECK(emu_input_owner_release_all(s, a) == 0 && b.down[0]);
    b.fail_set[0] = 1;
    EMU_CHECK(emu_input_key(s, a, 0, EMU_INPUT_PRESS) == -7);
    EMU_CHECK(emu_input_owner_holds(s, a, 0) == 0 && b.last.result == -7);
    b.fail_set[0] = 0;
    EMU_CHECK(emu_input_key(s, a, 0, EMU_INPUT_PRESS) == 0);
    b.fail_query = 1;
    EMU_CHECK(emu_input_key(s, a, 0, EMU_INPUT_RELEASE_SAMPLED) == -8);
    EMU_CHECK(emu_input_owner_holds(s, a, 0) == 1);
    b.fail_query = 0;
    EMU_CHECK(emu_input_key(s, a, 0, EMU_INPUT_RELEASE_SAMPLED) == 0);
    b.sampled[0] = 1; b.fail_set[0] = 1;
    EMU_CHECK(emu_input_poll(s) == -7 && emu_input_pending(s));
    EMU_CHECK(emu_input_key(s, a, 1, EMU_INPUT_PRESS) == 0);
    EMU_CHECK(emu_input_scope_close(s, scope) == -7 && !b.down[1]);
    EMU_CHECK(emu_input_key(s, a, 1, EMU_INPUT_PRESS) == -1);
    emu_input_key_state_t v;
    EMU_CHECK(emu_input_query(s, 0, &v) == 0 && v.pending_owner == a && v.last_error == -7);
    b.fail_set[0] = 0;
    EMU_CHECK(emu_input_scope_close(s, scope) == 0 && !b.down[0]);
    EMU_CHECK(emu_input_destroy(s) == 0);
    /* Replacement never imports ownership from the guest. */
    b.down[0] = 1; s = create(&b);
    EMU_CHECK(emu_input_destroy(s) == 0 && b.down[0]);
    s = create(&b); scope = emu_input_scope_open(s); a = emu_input_owner_open(s, scope);
    EMU_CHECK(emu_input_key(s, a, 0, EMU_INPUT_PRESS) == 0);
    b.fail_set[0] = 1;
    EMU_CHECK(emu_input_destroy(s) == -7);
}
static void test_checkpoint_reconcile(void) {
    backend_t b = {0};
    emu_input_t *s = create(&b);
    uint64_t scope = emu_input_scope_open(s), a = emu_input_owner_open(s, scope);
    EMU_CHECK(emu_input_key(s, a, 0, EMU_INPUT_PRESS) == 0);
    EMU_CHECK(emu_input_key(s, a, 0, EMU_INPUT_RELEASE_SAMPLED) == 0);
    b.down[0] = 0; b.down[1] = 1; /* guest rewind, host owners unchanged */
    EMU_CHECK(emu_input_reconcile(s) == 0);
    EMU_CHECK(b.down[0] && !b.down[1] && emu_input_pending(s));
    EMU_CHECK(!emu_input_owner_holds(s, a, 0));
    EMU_CHECK(emu_input_poll(s) == 0 && b.down[0]);
    b.sampled[0] = 1;
    EMU_CHECK(emu_input_poll(s) == 0 && !b.down[0]);
    EMU_CHECK(emu_input_key(s, a, 1, EMU_INPUT_PRESS) == 0);
    b.down[1] = 0;
    EMU_CHECK(emu_input_reconcile(s) == 0 && b.down[1]);
    EMU_CHECK(emu_input_owner_holds(s, a, 1));
    b.fail_set[1] = 1;
    EMU_CHECK(emu_input_reconcile(s) == -7);
    b.fail_set[1] = 0;
    EMU_CHECK(emu_input_destroy(s) == 0 && !b.down[1]);
}
int main(void) {
    test_ownership(); test_failure_and_restore(); test_checkpoint_reconcile();
    puts("input service without sockets: PASS");
    return 0;
}
