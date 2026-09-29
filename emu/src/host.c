#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include "emu_engine.h"
#include "emu_product.h"

struct emu_session {
    const emu_engine_descriptor_t *descriptor;
    void *engine;
    int terminal;
    int started;
};

static emu_error_code_t emu_fail(emu_error_t *error, emu_error_code_t code,
                                 const char *format, ...) {
    if (error) {
        va_list args;
        error->code = code;
        va_start(args, format);
        vsnprintf(error->message, sizeof error->message, format, args);
        va_end(args);
    }
    return code;
}

static void emu_clear_error(emu_error_t *error) {
    if (error) { error->code = EMU_OK; error->message[0] = 0; }
}

const char *emu_error_code_name(emu_error_code_t code) {
    static const char *const names[] = {
        "ok","argument","no-memory","io","hash-mismatch",
        "invalid-prepared-session","unknown-engine","duplicate-engine",
        "unsupported","lifecycle","engine"
    };
    return (unsigned)code < sizeof names / sizeof names[0] ? names[code] : "invalid-error";
}

const char *emu_run_status_name(emu_run_status_t status) {
    static const char *const names[] = {"limit","stopped","halted","unimplemented"};
    return (unsigned)status < sizeof names / sizeof names[0] ? names[status] : "invalid";
}

const char *emu_storage_space_name(emu_storage_space_t space) {
    static const char *const names[] = {
        "main-array","factory-uid","am29-factory-secsi","am29-customer-secsi"
    };
    return (unsigned)space < sizeof names / sizeof names[0] ? names[space] : "invalid";
}

const char *emu_event_kind_name(emu_event_kind_t kind) {
    static const char *const names[] = {
        "slice","reset","poll","key","serial","link","artifact"
    };
    return (unsigned)kind < sizeof names / sizeof names[0]
         ? names[kind] : "invalid";
}

const char *emu_frame_kind_name(emu_frame_kind_t kind) {
    static const char *const names[] = {"display","ddram"};
    return (unsigned)kind < sizeof names / sizeof names[0]
         ? names[kind] : "invalid";
}

const char *emu_artifact_request_kind_name(emu_artifact_request_kind_t kind) {
    static const char *const names[] = {
        "lcd-frame","lcd-ddram","snapshot","coverage","trace"
    };
    return (unsigned)kind < sizeof names / sizeof names[0]
         ? names[kind] : "invalid";
}

void emu_prepared_init(emu_prepared_session_t *prepared) {
    if (prepared) memset(prepared, 0, sizeof *prepared);
}

emu_error_code_t emu_prepared_load_source(emu_prepared_session_t *prepared,
                                           const char *locator,
                                           emu_error_t *error) {
    if (!prepared || !locator || prepared->source.bytes)
        return emu_fail(error, EMU_ERR_ARGUMENT, "invalid or already loaded source");
    FILE *file = fopen(locator, "rb");
    if (!file) return emu_fail(error, EMU_ERR_IO, "cannot open source: %s", strerror(errno));
    if (fseek(file, 0, SEEK_END) || ftell(file) < 0) {
        fclose(file); return emu_fail(error, EMU_ERR_IO, "cannot size source");
    }
    long length = ftell(file);
    if (fseek(file, 0, SEEK_SET)) {
        fclose(file); return emu_fail(error, EMU_ERR_IO, "cannot seek source");
    }
    uint8_t *bytes = malloc((size_t)length ? (size_t)length : 1u);
    char *path = malloc(strlen(locator) + 1u);
    if (!bytes || !path) {
        free(bytes); free(path); fclose(file);
        return emu_fail(error, EMU_ERR_NOMEM, "cannot allocate source");
    }
    if (length && fread(bytes, 1, (size_t)length, file) != (size_t)length) {
        free(bytes); free(path); fclose(file);
        return emu_fail(error, EMU_ERR_IO, "cannot read source");
    }
    fclose(file);
    memcpy(path, locator, strlen(locator) + 1u);
    prepared->source.locator = path;
    prepared->source.bytes = bytes;
    prepared->source.size = (size_t)length;
    emu_sha256(bytes, (size_t)length, prepared->source.sha256);
    emu_sha256_hex(prepared->source.sha256, prepared->source.sha256_hex);
    emu_clear_error(error);
    return EMU_OK;
}

emu_error_code_t emu_prepared_rehash_source(const emu_prepared_session_t *prepared,
                                             emu_error_t *error) {
    uint8_t digest[32];
    if (!prepared || !prepared->source.bytes)
        return emu_fail(error, EMU_ERR_ARGUMENT, "source is not loaded");
    emu_sha256(prepared->source.bytes, prepared->source.size, digest);
    if (memcmp(digest, prepared->source.sha256, sizeof digest))
        return emu_fail(error, EMU_ERR_HASH_MISMATCH, "immutable source bytes changed");
    emu_clear_error(error);
    return EMU_OK;
}

emu_error_code_t emu_prepared_add_grouped_storage_operation(
    emu_prepared_session_t *prepared, emu_storage_stage_t stage,
    size_t group, size_t chip_index, emu_storage_space_t space, size_t offset,
    const void *expected, size_t expected_size,
    const void *replacement, size_t replacement_size,
    const char *provenance, emu_error_t *error) {
    if (!prepared || !replacement || !replacement_size || !provenance ||
        !group || (unsigned)stage > EMU_STORAGE_STAGE_POST_RESTORE ||
        prepared->operation_count >= EMU_MAX_STORAGE_OPERATIONS ||
        (expected_size && !expected) || (unsigned)space > EMU_STORAGE_AM29_CUSTOMER_SECSI)
        return emu_fail(error, EMU_ERR_ARGUMENT, "invalid storage operation");
    uint8_t *expect_copy = NULL, *replace_copy = malloc(replacement_size);
    if (expected_size) expect_copy = malloc(expected_size);
    if (!replace_copy || (expected_size && !expect_copy)) {
        free(expect_copy); free(replace_copy);
        return emu_fail(error, EMU_ERR_NOMEM, "cannot allocate storage operation");
    }
    if (expected_size) memcpy(expect_copy, expected, expected_size);
    memcpy(replace_copy, replacement, replacement_size);
    emu_storage_operation_t *op = &prepared->operations[prepared->operation_count];
    memset(op, 0, sizeof *op);
    op->order = prepared->operation_count;
    op->stage = stage; op->group = group;
    op->chip_index = chip_index; op->space = space; op->offset = offset;
    op->expected = expect_copy; op->expected_size = expected_size;
    op->replacement = replace_copy; op->replacement_size = replacement_size;
    snprintf(op->provenance, sizeof op->provenance, "%s", provenance);
    prepared->operation_count++;
    emu_clear_error(error);
    return EMU_OK;
}

emu_error_code_t emu_prepared_add_storage_operation(
    emu_prepared_session_t *prepared, size_t chip_index,
    emu_storage_space_t space, size_t offset,
    const void *expected, size_t expected_size,
    const void *replacement, size_t replacement_size,
    const char *provenance, emu_error_t *error) {
    size_t group = prepared && prepared->operation_count
                 ? prepared->operations[prepared->operation_count - 1u].group + 1u
                 : 1u;
    return emu_prepared_add_grouped_storage_operation(
        prepared, EMU_STORAGE_STAGE_PRE_RESET, group, chip_index, space,
        offset, expected, expected_size, replacement, replacement_size,
        provenance, error);
}

emu_error_code_t emu_prepared_validate(const emu_prepared_session_t *prepared,
                                       uint64_t capabilities,
                                       emu_error_t *error) {
    if (!prepared || !prepared->source.locator || !prepared->source.bytes ||
        !prepared->source.size || !prepared->selected_device[0] ||
        !prepared->metadata.model[0] || !prepared->chip_count ||
        prepared->chip_count > EMU_MAX_CHIPS)
        return emu_fail(error, EMU_ERR_INVALID_PREPARED_SESSION,
                        "missing source, metadata, device, or chip views");
    size_t covered = 0;
    for (size_t i = 0; i < prepared->chip_count; i++) {
        const emu_chip_view_t *chip = &prepared->chips[i];
        if (!chip->role[0] || !chip->model[0] || !chip->size ||
            chip->source_offset > prepared->source.size ||
            chip->size > prepared->source.size - chip->source_offset)
            return emu_fail(error, EMU_ERR_INVALID_PREPARED_SESSION,
                            "malformed physical chip view %zu", i);
        for (size_t j = 0; j < i; j++) {
            const emu_chip_view_t *prior = &prepared->chips[j];
            if (chip->source_offset < prior->source_offset + prior->size &&
                prior->source_offset < chip->source_offset + chip->size)
                return emu_fail(error, EMU_ERR_INVALID_PREPARED_SESSION,
                                "overlapping physical chip views");
        }
        if (!chip->source_offset && chip->size == prepared->source.size) {
            if (memcmp(prepared->source.sha256, chip->sha256,
                       sizeof chip->sha256))
                return emu_fail(error, EMU_ERR_HASH_MISMATCH,
                                "chip view hash mismatch");
        } else {
            uint8_t digest[32];
            emu_sha256(prepared->source.bytes + chip->source_offset,
                       chip->size, digest);
            if (memcmp(digest, chip->sha256, sizeof digest))
                return emu_fail(error, EMU_ERR_HASH_MISMATCH,
                                "chip view hash mismatch");
        }
        covered += chip->size;
    }
    if (covered != prepared->source.size)
        return emu_fail(error, EMU_ERR_INVALID_PREPARED_SESSION,
                        "physical chip views do not cover the source");
    if (prepared->operation_count && !(capabilities & EMU_CAP_STORAGE_INIT))
        return emu_fail(error, EMU_ERR_UNSUPPORTED,
                        "engine does not support storage initialization");
    for (size_t i = 0; i < prepared->operation_count; i++) {
        const emu_storage_operation_t *op = &prepared->operations[i];
        if (op->order != i || !op->group ||
            (unsigned)op->stage > EMU_STORAGE_STAGE_POST_RESTORE ||
            op->chip_index >= prepared->chip_count ||
            (unsigned)op->space > EMU_STORAGE_AM29_CUSTOMER_SECSI ||
            (op->expected_size && !op->expected) ||
            !op->replacement || !op->replacement_size || !op->provenance[0])
            return emu_fail(error, EMU_ERR_INVALID_PREPARED_SESSION,
                            "malformed storage operation %zu", i);
        if (i && (op->stage < prepared->operations[i - 1u].stage ||
                  op->group < prepared->operations[i - 1u].group ||
                  (op->group == prepared->operations[i - 1u].group &&
                   op->stage != prepared->operations[i - 1u].stage)))
            return emu_fail(error, EMU_ERR_INVALID_PREPARED_SESSION,
                            "storage operations are not stage/group ordered");
        size_t operation_size = op->expected_size > op->replacement_size
                              ? op->expected_size : op->replacement_size;
        if (op->space == EMU_STORAGE_MAIN_ARRAY &&
            (op->offset > prepared->chips[op->chip_index].size ||
             operation_size > prepared->chips[op->chip_index].size - op->offset))
            return emu_fail(error, EMU_ERR_INVALID_PREPARED_SESSION,
                            "main-array operation is out of bounds");
    }
    return emu_prepared_rehash_source(prepared, error);
}

void emu_prepared_free(emu_prepared_session_t *prepared) {
    if (!prepared) return;
    free(prepared->source.locator); free(prepared->source.bytes);
    for (size_t i = 0; i < prepared->operation_count; i++) {
        free(prepared->operations[i].expected);
        free(prepared->operations[i].replacement);
    }
    memset(prepared, 0, sizeof *prepared);
}

emu_error_code_t emu_registry_register(emu_engine_registry_t *registry,
                                        const emu_engine_descriptor_t *descriptor,
                                        emu_error_t *error) {
    if (!registry || !descriptor || !descriptor->name || !descriptor->version ||
        !descriptor->ops.create || !descriptor->ops.reset ||
        !descriptor->ops.request_stop || !descriptor->ops.query || !descriptor->ops.destroy)
        return emu_fail(error, EMU_ERR_ARGUMENT, "invalid engine descriptor");
    if ((descriptor->execution_model == EMU_EXECUTION_SLICED &&
         !descriptor->ops.run) ||
        (descriptor->execution_model == EMU_EXECUTION_CONTINUOUS &&
         (!descriptor->ops.start || !descriptor->ops.pump)) ||
        (unsigned)descriptor->execution_model > EMU_EXECUTION_CONTINUOUS)
        return emu_fail(error, EMU_ERR_ARGUMENT,
                        "invalid engine execution model");
    if (emu_registry_find(registry, descriptor->name))
        return emu_fail(error, EMU_ERR_DUPLICATE_ENGINE, "duplicate engine: %s", descriptor->name);
    if (registry->count == sizeof registry->items / sizeof registry->items[0])
        return emu_fail(error, EMU_ERR_NOMEM, "engine registry is full");
    registry->items[registry->count++] = descriptor;
    emu_clear_error(error);
    return EMU_OK;
}

const emu_engine_descriptor_t *emu_registry_find(
    const emu_engine_registry_t *registry, const char *name) {
    if (!registry || !name) return NULL;
    for (size_t i = 0; i < registry->count; i++)
        if (!strcmp(registry->items[i]->name, name)) return registry->items[i];
    return NULL;
}

emu_error_code_t emu_session_create(const emu_engine_registry_t *registry,
                                     const char *engine,
                                     const emu_prepared_session_t *prepared,
                                     const emu_callbacks_t *callbacks,
                                     emu_session_t **out, emu_error_t *error) {
    if (!out || *out) return emu_fail(error, EMU_ERR_ARGUMENT, "invalid session output");
    if (!prepared)
        return emu_fail(error, EMU_ERR_ARGUMENT, "missing prepared session");
    const emu_engine_descriptor_t *descriptor = emu_registry_find(registry, engine);
    if (!descriptor) return emu_fail(error, EMU_ERR_UNKNOWN_ENGINE, "unknown engine: %s", engine ? engine : "(null)");
    int model_supported = 0;
    for (size_t i = 0; i < descriptor->supported_model_count; i++)
        if (!strcmp(prepared->selected_device, descriptor->supported_models[i])) {
            model_supported = 1;
            break;
        }
    if (!model_supported)
        return emu_fail(error, EMU_ERR_UNSUPPORTED,
                        "engine does not support selected model: %s",
                        prepared->selected_device);
    emu_error_code_t rc = emu_prepared_validate(prepared, descriptor->capabilities, error);
    if (rc != EMU_OK) return rc;
    emu_session_t *session = calloc(1, sizeof *session);
    if (!session) return emu_fail(error, EMU_ERR_NOMEM, "cannot allocate session");
    session->descriptor = descriptor;
    rc = descriptor->ops.create(prepared, callbacks, &session->engine, error);
    if (rc != EMU_OK || !session->engine) {
        if (session->engine) descriptor->ops.destroy(session->engine);
        free(session);
        return rc != EMU_OK ? rc : emu_fail(error, EMU_ERR_ENGINE, "engine returned no session");
    }
    *out = session; emu_clear_error(error); return EMU_OK;
}

static emu_error_code_t emu_live(emu_session_t *s, emu_error_t *e) {
    return (!s || !s->engine) ? emu_fail(e, EMU_ERR_LIFECYCLE, "session is not live") : EMU_OK;
}

emu_error_code_t emu_session_run_request(
        emu_session_t *s, const emu_run_request_t *request,
        emu_run_result_t *result, emu_error_t *error) {
    emu_error_code_t live = emu_live(s, error);
    if (live != EMU_OK) return live;
    if (!result || !request || !request->tick_budget)
        return emu_fail(error, EMU_ERR_ARGUMENT, "invalid run request");
    if (s->terminal) return emu_fail(error, EMU_ERR_LIFECYCLE, "terminal session must be reset");
    if (s->descriptor->execution_model != EMU_EXECUTION_SLICED)
        return emu_fail(error, EMU_ERR_UNSUPPORTED,
                        "continuous engine has no bounded run operation");
    emu_error_code_t rc=s->descriptor->ops.run(s->engine,request,result,error);
    if (rc == EMU_OK) {
        if (result->ticks > request->tick_budget) return emu_fail(error,EMU_ERR_ENGINE,"engine exceeded slice budget");
        s->terminal = result->status != EMU_RUN_LIMIT;
    }
    return rc;
}

emu_error_code_t emu_session_start(emu_session_t *s, emu_error_t *error) {
    emu_error_code_t live = emu_live(s, error);
    if (live != EMU_OK) return live;
    if (s->descriptor->execution_model != EMU_EXECUTION_CONTINUOUS)
        return emu_fail(error, EMU_ERR_UNSUPPORTED,
                        "sliced engine has no continuous start operation");
    if (s->started || s->terminal)
        return emu_fail(error, EMU_ERR_LIFECYCLE,
                        "continuous session is already started or terminal");
    emu_error_code_t code = s->descriptor->ops.start(s->engine, error);
    if (code == EMU_OK) s->started = 1;
    return code;
}

emu_error_code_t emu_session_pump(emu_session_t *s, unsigned timeout_ms,
                                  int *complete, emu_run_result_t *result,
                                  emu_error_t *error) {
    emu_error_code_t live = emu_live(s, error);
    if (live != EMU_OK) return live;
    if (!complete || !result || !s->started || s->terminal ||
        s->descriptor->execution_model != EMU_EXECUTION_CONTINUOUS)
        return emu_fail(error, EMU_ERR_LIFECYCLE,
                        "invalid continuous session pump");
    emu_error_code_t code = s->descriptor->ops.pump(
        s->engine, timeout_ms, complete, result, error);
    if (code == EMU_OK && *complete) s->terminal = 1;
    return code;
}

emu_error_code_t emu_session_run(emu_session_t *s, uint64_t budget,
                                  emu_run_result_t *result,
                                  emu_error_t *error) {
    emu_run_request_t request = {
        .tick_budget = budget,
        .compute_digest = 1,
    };
    return emu_session_run_request(s, &request, result, error);
}

emu_error_code_t emu_session_reset(emu_session_t *s, emu_error_t *e) {
    if (emu_live(s,e)!=EMU_OK) return EMU_ERR_LIFECYCLE;
    emu_error_code_t rc=s->descriptor->ops.reset(s->engine,e); if(rc==EMU_OK)s->terminal=0; return rc;
}
emu_error_code_t emu_session_request_stop(emu_session_t *s,emu_error_t *e){if(emu_live(s,e)!=EMU_OK)return EMU_ERR_LIFECYCLE;return s->descriptor->ops.request_stop(s->engine,e);}
emu_error_code_t emu_session_query_request(emu_session_t*s,int d,emu_engine_state_t*v,emu_error_t*e){emu_error_code_t rc=emu_live(s,e);if(rc!=EMU_OK)return rc;if(!v)return emu_fail(e,EMU_ERR_ARGUMENT,"invalid query");return s->descriptor->ops.query(s->engine,d,v,e);}
emu_error_code_t emu_session_query(emu_session_t*s,emu_engine_state_t*v,emu_error_t*e){return emu_session_query_request(s,1,v,e);}

static emu_error_code_t emu_cap(emu_session_t *s,uint64_t cap,int has_fn,emu_error_t *e){if(emu_live(s,e)!=EMU_OK)return EMU_ERR_LIFECYCLE;if(!(s->descriptor->capabilities&cap)||!has_fn)return emu_fail(e,EMU_ERR_UNSUPPORTED,"engine capability is unsupported");return EMU_OK;}
emu_error_code_t emu_session_key(emu_session_t*s,const char*k,int p,emu_error_t*e){emu_error_code_t rc=emu_cap(s,EMU_CAP_KEYS,s&&s->descriptor->ops.key,e);return rc==EMU_OK?s->descriptor->ops.key(s->engine,k,p,e):rc;}
emu_error_code_t emu_session_serial_rx(emu_session_t*s,const uint8_t*d,size_t n,emu_error_t*e){emu_error_code_t rc=emu_cap(s,EMU_CAP_SERIAL,s&&s->descriptor->ops.serial_rx,e);return rc==EMU_OK?s->descriptor->ops.serial_rx(s->engine,d,n,e):rc;}
emu_error_code_t emu_session_sim(emu_session_t*s,int a,emu_error_t*e){emu_error_code_t rc=emu_cap(s,EMU_CAP_SIM,s&&s->descriptor->ops.sim,e);return rc==EMU_OK?s->descriptor->ops.sim(s->engine,a,e):rc;}
emu_error_code_t emu_session_battery(emu_session_t*s,unsigned l,int c,emu_error_t*e){emu_error_code_t rc=emu_cap(s,EMU_CAP_BATTERY,s&&s->descriptor->ops.battery,e);return rc==EMU_OK?s->descriptor->ops.battery(s->engine,l,c,e):rc;}
emu_error_code_t emu_session_snapshot(emu_session_t*s,const char*p,int load,emu_error_t*e){emu_error_code_t rc=emu_cap(s,EMU_CAP_SNAPSHOTS,s&&s->descriptor->ops.snapshot,e);return rc==EMU_OK?s->descriptor->ops.snapshot(s->engine,p,load,e):rc;}
emu_error_code_t emu_session_snapshot_restore(emu_session_t*s,const char*p,emu_error_t*e){if(!s||!p||!*p)return emu_fail(e,EMU_ERR_ARGUMENT,"invalid snapshot restore request");const emu_engine_diagnostics_t*d=s->descriptor->diagnostics;const emu_engine_diagnostic_ops_t*o=s->descriptor->diagnostic_ops;if(!d||!(d->capabilities&EMU_DIAG_SNAPSHOT_RESTORE)||!o||!o->snapshot_restore)return emu_fail(e,EMU_ERR_UNSUPPORTED,"engine diagnostic snapshot restore is unsupported");return o->snapshot_restore(s->engine,p,e);}
emu_error_code_t emu_session_snapshot_write(emu_session_t*s,const char*p,const char*j,int f,emu_error_t*e){if(!s||!p||!*p||!j||!*j)return emu_fail(e,EMU_ERR_ARGUMENT,"invalid diagnostic snapshot request");const emu_engine_diagnostics_t*d=s->descriptor->diagnostics;const emu_engine_diagnostic_ops_t*o=s->descriptor->diagnostic_ops;if(!d||!(d->capabilities&EMU_DIAG_SNAPSHOT)||!o||!o->snapshot_write)return emu_fail(e,EMU_ERR_UNSUPPORTED,"engine diagnostic snapshot writer is unsupported");return o->snapshot_write(s->engine,p,j,f,e);}
emu_error_code_t emu_session_trace_attach(emu_session_t*s,emu_trace_sink_t*t,int deferred,int g,emu_error_t*e){if(!s||!t)return emu_fail(e,EMU_ERR_ARGUMENT,"invalid diagnostic trace request");const emu_engine_diagnostics_t*d=s->descriptor->diagnostics;const emu_engine_diagnostic_ops_t*o=s->descriptor->diagnostic_ops;if(!d||!d->trace_available||!o||!o->trace_attach)return emu_fail(e,EMU_ERR_UNSUPPORTED,"engine diagnostic trace is unsupported");return o->trace_attach(s->engine,t,deferred,g,e);}
emu_error_code_t emu_session_trace_arm(emu_session_t*s,emu_error_t*e){if(!s)return emu_fail(e,EMU_ERR_ARGUMENT,"invalid diagnostic trace request");const emu_engine_diagnostic_ops_t*o=s->descriptor->diagnostic_ops;if(!o||!o->trace_arm)return emu_fail(e,EMU_ERR_UNSUPPORTED,"engine diagnostic trace is unsupported");return o->trace_arm(s->engine,e);}
emu_error_code_t emu_session_trace_detach(emu_session_t*s,emu_error_t*e){if(!s)return emu_fail(e,EMU_ERR_ARGUMENT,"invalid diagnostic trace request");const emu_engine_diagnostic_ops_t*o=s->descriptor->diagnostic_ops;if(!o||!o->trace_detach)return emu_fail(e,EMU_ERR_UNSUPPORTED,"engine diagnostic trace is unsupported");return o->trace_detach(s->engine,e);}
emu_error_code_t emu_session_trace_host_event(
        emu_session_t *session, const emu_trace_event_t *event, emu_error_t *error) {
    if (!session || !event)
        return emu_fail(error, EMU_ERR_ARGUMENT, "invalid host service diagnostic");
    const emu_engine_diagnostic_ops_t *ops = session->descriptor->diagnostic_ops;
    if (!ops || !ops->trace_host_event)
        return emu_fail(error, EMU_ERR_UNSUPPORTED, "no ordered host service consumer");
    return ops->trace_host_event(session->engine, event, error);
}

emu_error_code_t emu_session_drcov_attach(emu_session_t*s,emu_drcov_t**c,emu_error_t*e){if(!s||!c||*c)return emu_fail(e,EMU_ERR_ARGUMENT,"invalid diagnostic drcov request");const emu_engine_diagnostics_t*d=s->descriptor->diagnostics;const emu_engine_diagnostic_ops_t*o=s->descriptor->diagnostic_ops;if(!d||!(d->capabilities&EMU_DIAG_COVERAGE)||!o||!o->drcov_attach)return emu_fail(e,EMU_ERR_UNSUPPORTED,"engine diagnostic drcov is unsupported");return o->drcov_attach(s->engine,c,e);}
emu_error_code_t emu_session_drcov_detach(emu_session_t*s,emu_error_t*e){if(!s)return emu_fail(e,EMU_ERR_ARGUMENT,"invalid diagnostic drcov request");const emu_engine_diagnostic_ops_t*o=s->descriptor->diagnostic_ops;if(!o||!o->drcov_detach)return emu_fail(e,EMU_ERR_UNSUPPORTED,"engine diagnostic drcov is unsupported");return o->drcov_detach(s->engine,e);}
emu_error_code_t emu_session_flash_size(emu_session_t*s,size_t*n,emu_error_t*e){if(!s||!n)return emu_fail(e,EMU_ERR_ARGUMENT,"invalid diagnostic flash request");const emu_engine_diagnostic_ops_t*o=s->descriptor->diagnostic_ops;if(!o||!o->flash_size)return emu_fail(e,EMU_ERR_UNSUPPORTED,"engine diagnostic flash export is unsupported");return o->flash_size(s->engine,n,e);}
emu_error_code_t emu_session_flash_read(emu_session_t*s,size_t o,uint8_t*b,size_t n,emu_error_t*e){if(!s||(!b&&n))return emu_fail(e,EMU_ERR_ARGUMENT,"invalid diagnostic flash request");const emu_engine_diagnostic_ops_t*d=s->descriptor->diagnostic_ops;if(!d||!d->flash_read)return emu_fail(e,EMU_ERR_UNSUPPORTED,"engine diagnostic flash export is unsupported");return d->flash_read(s->engine,o,b,n,e);}
emu_error_code_t emu_session_debugger_run(emu_session_t*s,const emu_debugger_request_t*r,emu_error_t*e){if(!s||!r||!r->output.write||(r->interactive&&!r->input.read))return emu_fail(e,EMU_ERR_ARGUMENT,"invalid diagnostic debugger request");const emu_engine_diagnostics_t*d=s->descriptor->diagnostics;const emu_engine_diagnostic_ops_t*o=s->descriptor->diagnostic_ops;if(!d||!(d->capabilities&EMU_DIAG_DEBUGGER)||!o||!o->debugger_run)return emu_fail(e,EMU_ERR_UNSUPPORTED,"engine diagnostic debugger is unsupported");return o->debugger_run(s->engine,r,e);}
emu_error_code_t emu_session_monitor_start(emu_session_t*s,uint64_t l,uint64_t w,emu_error_t*e){if(!s)return emu_fail(e,EMU_ERR_ARGUMENT,"invalid diagnostic monitor request");const emu_engine_diagnostics_t*d=s->descriptor->diagnostics;const emu_engine_diagnostic_ops_t*o=s->descriptor->diagnostic_ops;if(!d||!(d->capabilities&EMU_DIAG_MONITOR)||!o||!o->monitor_start)return emu_fail(e,EMU_ERR_UNSUPPORTED,"engine diagnostic monitor is unsupported");return o->monitor_start(s->engine,l,w,e);}
emu_error_code_t emu_session_monitor_poll(emu_session_t*s,emu_monitor_verdict_t*v,emu_error_t*e){if(!s||!v)return emu_fail(e,EMU_ERR_ARGUMENT,"invalid diagnostic monitor request");const emu_engine_diagnostic_ops_t*o=s->descriptor->diagnostic_ops;if(!o||!o->monitor_poll)return emu_fail(e,EMU_ERR_UNSUPPORTED,"engine diagnostic monitor is unsupported");return o->monitor_poll(s->engine,v,e);}
emu_error_code_t emu_session_summary_format(emu_session_t*s,const emu_summary_request_t*r,char*b,size_t n,emu_error_t*e){if(!s||!r||!b||!n)return emu_fail(e,EMU_ERR_ARGUMENT,"invalid diagnostic summary request");const emu_engine_diagnostic_ops_t*o=s->descriptor->diagnostic_ops;if(!o||!o->summary_format)return emu_fail(e,EMU_ERR_UNSUPPORTED,"engine diagnostic summary is unsupported");return o->summary_format(s->engine,r,b,n,e);}
emu_error_code_t emu_session_monitor_stop(emu_session_t*s,emu_error_t*e){if(!s)return emu_fail(e,EMU_ERR_ARGUMENT,"invalid diagnostic monitor request");const emu_engine_diagnostic_ops_t*o=s->descriptor->diagnostic_ops;if(!o||!o->monitor_stop)return emu_fail(e,EMU_ERR_UNSUPPORTED,"engine diagnostic monitor is unsupported");return o->monitor_stop(s->engine,e);}
emu_error_code_t emu_session_debugger(emu_session_t*s,const char*c,emu_error_t*e){emu_error_code_t rc=emu_cap(s,EMU_CAP_DEBUGGER,s&&s->descriptor->ops.debugger,e);return rc==EMU_OK?s->descriptor->ops.debugger(s->engine,c,e):rc;}
emu_error_code_t emu_session_coverage(emu_session_t*s,int en,emu_error_t*e){emu_error_code_t rc=emu_cap(s,EMU_CAP_COVERAGE,s&&s->descriptor->ops.coverage,e);return rc==EMU_OK?s->descriptor->ops.coverage(s->engine,en,e):rc;}
emu_error_code_t emu_session_poll(emu_session_t*s,emu_error_t*e){emu_error_code_t rc=emu_cap(s,EMU_CAP_EVENTS,s&&s->descriptor->ops.poll,e);return rc==EMU_OK?s->descriptor->ops.poll(s->engine,e):rc;}
emu_error_code_t emu_session_select_events(emu_session_t*s,const emu_event_selection_t*v,emu_error_t*e){emu_error_code_t rc=emu_cap(s,EMU_CAP_EVENTS,s&&s->descriptor->ops.select_events,e);if(rc!=EMU_OK)return rc;if(!v||(v->kind_mask&~EMU_EVENT_MASK_ALL))return emu_fail(e,EMU_ERR_ARGUMENT,"invalid event selection");return s->descriptor->ops.select_events(s->engine,v,e);}
emu_error_code_t emu_session_key_query(emu_session_t*s,const char*k,emu_key_sample_t*v,emu_error_t*e){emu_error_code_t rc=emu_cap(s,EMU_CAP_KEY_SAMPLING,s&&s->descriptor->ops.key_query,e);if(rc!=EMU_OK)return rc;if(!k||!v)return emu_fail(e,EMU_ERR_ARGUMENT,"invalid sampled-key query");return s->descriptor->ops.key_query(s->engine,k,v,e);}
emu_error_code_t emu_session_serial_link(emu_session_t*s,emu_serial_link_attachment_t a,emu_serial_link_state_t*v,emu_error_t*e){emu_error_code_t rc=emu_cap(s,EMU_CAP_SERIAL_LINK,s&&s->descriptor->ops.serial_link,e);if(rc!=EMU_OK)return rc;if((unsigned)a>EMU_SERIAL_LINK_UI||!v)return emu_fail(e,EMU_ERR_ARGUMENT,"invalid serial-link request");return s->descriptor->ops.serial_link(s->engine,a,v,e);}
emu_error_code_t emu_session_artifact(emu_session_t*s,const emu_artifact_request_t*r,emu_artifact_result_t*v,emu_error_t*e){emu_error_code_t rc=emu_cap(s,EMU_CAP_ARTIFACT_REQ,s&&s->descriptor->ops.artifact,e);if(rc!=EMU_OK)return rc;if(!r||!v||(unsigned)r->kind>EMU_ARTIFACT_TRACE||(unsigned)r->action>EMU_ARTIFACT_STOP)return emu_fail(e,EMU_ERR_ARGUMENT,"invalid artifact request");return s->descriptor->ops.artifact(s->engine,r,v,e);}

const emu_engine_descriptor_t *emu_session_engine(const emu_session_t *s){return s?s->descriptor:NULL;}
void emu_session_destroy(emu_session_t **slot){if(!slot||!*slot)return;emu_session_t*s=*slot;if(s->engine)s->descriptor->ops.destroy(s->engine);memset(s,0,sizeof*s);free(s);*slot=NULL;}
