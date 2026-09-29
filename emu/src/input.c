#include "emu_input.h"
#include <stdlib.h>
#include <string.h>

typedef struct owner {
    struct owner *next;
    emu_input_handle_t id, scope;
    uint32_t held;
    int closing;
} owner_t;
typedef struct scope {
    struct scope *next;
    emu_input_handle_t id;
    int closing;
} scope_t;
struct emu_input {
    char names[EMU_INPUT_MAX_KEYS][EMU_INPUT_KEY_NAME_MAX + 1];
    size_t count;
    emu_input_callbacks_t cb;
    emu_input_key_state_t keys[EMU_INPUT_MAX_KEYS];
    owner_t *owners;
    scope_t *scopes;
    uint64_t next, icount;
    uint32_t pc;
};
static owner_t *owner(const emu_input_t *s, uint64_t id) {
    for (owner_t *o = s ? s->owners : NULL; o; o = o->next)
        if (o->id == id) return o;
    return NULL;
}
static scope_t *scope(const emu_input_t *s, uint64_t id) {
    for (scope_t *p = s ? s->scopes : NULL; p; p = p->next)
        if (p->id == id) return p;
    return NULL;
}
static void event(emu_input_t *s, uint64_t id, size_t k, const char *action,
                  int changed, int result) {
    s->keys[k].last_error = result;
    if (!s->cb.trace) return;
    emu_input_event_t e = {
        .icount = s->icount, .pc = s->pc, .owner = id, .key = k,
        .name = s->names[k], .action = action, .holders = s->keys[k].holders,
        .down = s->keys[k].down, .pending = !!s->keys[k].pending_owner,
        .changed = changed, .result = result,
    };
    s->cb.trace(s->cb.opaque, &e);
}
emu_input_t *emu_input_create(const char *const *names, size_t n,
                               const emu_input_callbacks_t *cb) {
    if (!names || !n || n > EMU_INPUT_MAX_KEYS || !cb || !cb->set || !cb->sampled)
        return NULL;
    for (size_t k = 0; k < n; k++) {
        if (!names[k] || !names[k][0] || strlen(names[k]) > EMU_INPUT_KEY_NAME_MAX)
            return NULL;
        for (size_t j = 0; j < k; j++) if (!strcmp(names[j], names[k])) return NULL;
    }
    emu_input_t *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    s->count = n; s->cb = *cb; s->next = 1;
    for (size_t k = 0; k < n; k++) strcpy(s->names[k], names[k]);
    return s;
}
void emu_input_context(emu_input_t *s, uint64_t ticks, uint32_t pc) {
    if (s) { s->icount = ticks; s->pc = pc; }
}
uint64_t emu_input_scope_open(emu_input_t *s) {
    if (!s || !s->next) return 0;
    scope_t *p = calloc(1, sizeof *p);
    if (!p) return 0;
    p->id = s->next++; p->next = s->scopes; s->scopes = p;
    return p->id;
}
uint64_t emu_input_owner_open(emu_input_t *s, uint64_t scope_id) {
    scope_t *p = scope(s, scope_id);
    if (!p || p->closing || !s->next) return 0;
    owner_t *o = calloc(1, sizeof *o);
    if (!o) return 0;
    o->id = s->next++; o->scope = scope_id; o->next = s->owners; s->owners = o;
    return o->id;
}
int emu_input_key(emu_input_t *s, uint64_t id, size_t k, emu_input_action_t action) {
    owner_t *o = owner(s, id);
    if (!o || k >= s->count || action < EMU_INPUT_RELEASE ||
        action > EMU_INPUT_RELEASE_SAMPLED ||
        (o->closing && action != EMU_INPUT_RELEASE)) return -1;
    uint32_t bit = UINT32_C(1) << k;
    emu_input_key_state_t *v = &s->keys[k];
    const char *label = action == EMU_INPUT_PRESS ? "press" :
                        action == EMU_INPUT_RELEASE ? "release" : "sampled-release";
    int rc = 0, changed = 0;
    if (action == EMU_INPUT_PRESS) {
        if (!(o->held & bit)) {
            if (!v->down) rc = s->cb.set(s->cb.opaque, s->names[k], 1);
            if (!rc) {
                changed = 1; o->held |= bit; v->holders++; v->down = 1;
                if (v->pending_owner) {
                    uint64_t pending = v->pending_owner;
                    v->pending_owner = 0;
                    event(s, pending, k, "cancelled", 1, 0);
                }
            }
        }
    } else if (o->held & bit) {
        int sampled = 1;
        if (v->holders == 1) {
            if (action == EMU_INPUT_RELEASE_SAMPLED)
                rc = s->cb.sampled(s->cb.opaque, s->names[k], &sampled);
            if (!rc && sampled) rc = s->cb.set(s->cb.opaque, s->names[k], 0);
        }
        if (!rc) {
            changed = 1; o->held &= ~bit; v->holders--;
            if (!v->holders) {
                v->down = !sampled; v->pending_owner = sampled ? 0 : id;
                if (action == EMU_INPUT_RELEASE_SAMPLED)
                    label = sampled ? "sampled" : "queued";
            }
        }
    } else if (v->pending_owner == id && action == EMU_INPUT_RELEASE) {
        rc = s->cb.set(s->cb.opaque, s->names[k], 0);
        if (!rc) { v->pending_owner = 0; v->down = 0; changed = 1; }
        label = "forced";
    }
    event(s, id, k, label, changed, rc);
    return rc;
}
int emu_input_owner_release_all(emu_input_t *s, uint64_t id) {
    owner_t *o = owner(s, id);
    if (!o) return -1;
    int result = 0;
    for (size_t k = 0; k < s->count; k++)
        if ((o->held & (UINT32_C(1) << k)) || s->keys[k].pending_owner == id) {
            int rc = emu_input_key(s, id, k, EMU_INPUT_RELEASE);
            if (rc) result = rc;
        }
    return result;
}
int emu_input_owner_close(emu_input_t *s, uint64_t id) {
    owner_t *o = owner(s, id);
    if (!o) return -1;
    o->closing = 1;
    int rc = emu_input_owner_release_all(s, id);
    if (rc) return rc;
    owner_t **p = &s->owners;
    while (*p != o) p = &(*p)->next;
    *p = o->next; free(o);
    return 0;
}
static int scope_cleanup(emu_input_t *s, uint64_t id, int close) {
    scope_t *p = scope(s, id);
    if (!p) return -1;
    if (close) p->closing = 1;
    int result = 0;
    for (owner_t *o = s->owners, *next; o; o = next) {
        next = o->next;
        if (o->scope != id) continue;
        int rc = close ? emu_input_owner_close(s, o->id) :
                         emu_input_owner_release_all(s, o->id);
        if (rc) result = rc;
    }
    if (close && !result) {
        scope_t **link = &s->scopes;
        while (*link != p) link = &(*link)->next;
        *link = p->next; free(p);
    }
    return result;
}
int emu_input_scope_release_all(emu_input_t *s, uint64_t id) { return scope_cleanup(s, id, 0); }
int emu_input_scope_close(emu_input_t *s, uint64_t id) { return scope_cleanup(s, id, 1); }
int emu_input_reconcile(emu_input_t *s) {
    if (!s) return -1;
    int result = 0;
    for (size_t k = 0; k < s->count; k++) {
        /* Release first to clear any sampling latch from the old timeline. */
        int rc = s->cb.set(s->cb.opaque, s->names[k], 0);
        if (!rc && s->keys[k].down)
            rc = s->cb.set(s->cb.opaque, s->names[k], 1);
        event(s, s->keys[k].pending_owner, k, "restore", 1, rc);
        if (rc) result = rc;
    }
    return result;
}
int emu_input_poll(emu_input_t *s) {
    if (!s) return -1;
    int result = 0;
    for (size_t k = 0; k < s->count; k++) {
        emu_input_key_state_t *v = &s->keys[k];
        if (!v->pending_owner) continue;
        int sampled = 0;
        int rc = s->cb.sampled(s->cb.opaque, s->names[k], &sampled);
        if (!rc && !sampled) continue;
        uint64_t id = v->pending_owner;
        if (!rc) rc = s->cb.set(s->cb.opaque, s->names[k], 0);
        if (!rc) { v->pending_owner = 0; v->down = 0; }
        event(s, id, k, "sampled", !rc, rc);
        if (rc) result = rc;
    }
    return result;
}
int emu_input_query(const emu_input_t *s, size_t k, emu_input_key_state_t *v) {
    if (!s || k >= s->count || !v) return -1;
    *v = s->keys[k]; return 0;
}
int emu_input_owner_holds(const emu_input_t *s, uint64_t id, size_t k) {
    owner_t *o = owner(s, id);
    return !o || k >= s->count ? -1 : !!(o->held & (UINT32_C(1) << k));
}
uint32_t emu_input_down_mask(const emu_input_t *s) {
    uint32_t mask = 0;
    for (size_t k = 0; s && k < s->count; k++)
        if (s->keys[k].down) mask |= UINT32_C(1) << k;
    return mask;
}
uint32_t emu_input_pending_mask(const emu_input_t *s) {
    uint32_t mask = 0;
    for (size_t k = 0; s && k < s->count; k++)
        if (s->keys[k].pending_owner) mask |= UINT32_C(1) << k;
    return mask;
}
int emu_input_pending(const emu_input_t *s) { return !!emu_input_pending_mask(s); }
int emu_input_destroy(emu_input_t *s) {
    if (!s) return 0;
    int result = 0;
    for (scope_t *p = s->scopes, *next; p; p = next) {
        next = p->next;
        int rc = emu_input_scope_close(s, p->id);
        if (rc) result = rc;
    }
    /* Failed cleanup is reported; caller must destroy the guest afterwards. */
    while (s->owners) { owner_t *o = s->owners; s->owners = o->next; free(o); }
    while (s->scopes) { scope_t *p = s->scopes; s->scopes = p->next; free(p); }
    free(s); return result;
}
