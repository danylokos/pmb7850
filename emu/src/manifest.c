#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "emu_manifest.h"

typedef struct { char *data; size_t size, capacity; int failed; } emu_json_buffer_t;

static void emu_json_append(emu_json_buffer_t *b, const char *format, ...) {
    if (b->failed) return;
    for (;;) {
        if (b->capacity - b->size < 128u) {
            size_t capacity = b->capacity ? b->capacity * 2u : 2048u;
            char *data = realloc(b->data, capacity);
            if (!data) { b->failed = 1; return; }
            b->data = data; b->capacity = capacity;
        }
        va_list args;
        va_start(args, format);
        int count = vsnprintf(b->data + b->size, b->capacity - b->size,
                              format, args);
        va_end(args);
        if (count < 0) { b->failed = 1; return; }
        if ((size_t)count < b->capacity - b->size) { b->size += (size_t)count; return; }
        size_t capacity = b->size + (size_t)count + 1u;
        char *data = realloc(b->data, capacity);
        if (!data) { b->failed = 1; return; }
        b->data = data; b->capacity = capacity;
    }
}

static uint32_t emu_utf8_next(const unsigned char **cursor) {
    const unsigned char *p = *cursor;
    uint32_t cp;
    if (p[0] < 0x80) { *cursor = p + 1; return p[0]; }
    if ((p[0]&0xe0)==0xc0 && (p[1]&0xc0)==0x80) {
        cp=((uint32_t)(p[0]&0x1f)<<6)|(p[1]&0x3f); if(cp>=0x80){*cursor=p+2;return cp;}
    } else if ((p[0]&0xf0)==0xe0 && (p[1]&0xc0)==0x80 && (p[2]&0xc0)==0x80) {
        cp=((uint32_t)(p[0]&15)<<12)|((uint32_t)(p[1]&63)<<6)|(p[2]&63);
        if(cp>=0x800 && !(cp>=0xd800&&cp<=0xdfff)){*cursor=p+3;return cp;}
    } else if ((p[0]&0xf8)==0xf0 && (p[1]&0xc0)==0x80 &&
               (p[2]&0xc0)==0x80 && (p[3]&0xc0)==0x80) {
        cp=((uint32_t)(p[0]&7)<<18)|((uint32_t)(p[1]&63)<<12)|
           ((uint32_t)(p[2]&63)<<6)|(p[3]&63);
        if(cp>=0x10000&&cp<=0x10ffff){*cursor=p+4;return cp;}
    }
    *cursor = p + 1; return 0xfffd;
}

static void emu_json_string(emu_json_buffer_t *b, const char *text) {
    emu_json_append(b, "\"");
    const unsigned char *p = (const unsigned char *)(text ? text : "");
    while (*p) {
        uint32_t cp = emu_utf8_next(&p);
        if (cp=='\"') emu_json_append(b,"\\\"");
        else if (cp=='\\') emu_json_append(b,"\\\\");
        else if (cp=='\b') emu_json_append(b,"\\b");
        else if (cp=='\f') emu_json_append(b,"\\f");
        else if (cp=='\n') emu_json_append(b,"\\n");
        else if (cp=='\r') emu_json_append(b,"\\r");
        else if (cp=='\t') emu_json_append(b,"\\t");
        else if (cp>=0x20 && cp<=0x7e) emu_json_append(b,"%c",(char)cp);
        else if (cp<=0xffff) emu_json_append(b,"\\u%04x",(unsigned)cp);
        else {
            cp-=0x10000; unsigned hi=0xd800+(cp>>10), lo=0xdc00+(cp&0x3ff);
            emu_json_append(b,"\\u%04x\\u%04x",hi,lo);
        }
    }
    emu_json_append(b, "\"");
}

static void emu_json_hex(emu_json_buffer_t *b,const uint8_t *bytes,size_t size){
    static const char digits[]="0123456789abcdef"; emu_json_append(b,"\"");
    for(size_t i=0;i<size;i++)emu_json_append(b,"%c%c",digits[bytes[i]>>4],digits[bytes[i]&15]);
    emu_json_append(b,"\"");
}

static emu_error_code_t emu_manifest_fail(emu_error_t *error,
                                           emu_error_code_t code,
                                           const char *message) {
    if(error){error->code=code;snprintf(error->message,sizeof error->message,"%s",message);}
    return code;
}

void emu_manifest_init(emu_manifest_t *manifest,
                       const emu_prepared_session_t *prepared,
                       const emu_engine_descriptor_t *engine,
                       const emu_run_result_t *result) {
    if (!manifest) return;
    memset(manifest, 0, sizeof *manifest);
    manifest->schema_version=1;
    manifest->prepared=prepared; manifest->engine=engine;
    if(result)manifest->result=*result;
}

void emu_manifest_init_v2(emu_manifest_t *manifest,
                          const emu_prepared_session_t *prepared,
                          const emu_engine_descriptor_t *engine,
                          const emu_run_result_t *result,
                          const emu_preparation_request_t *request,
                          const emu_preparation_result_t *preparation) {
    emu_manifest_init(manifest, prepared, engine, result);
    if (!manifest) return;
    manifest->schema_version=2;
    if (request) {
        manifest->startup_mode=request->mode;
        manifest->identity_source=request->identity_source;
        manifest->selected_patches=request->selected_patches;
    }
    if (preparation) {
        snprintf(manifest->snapshot_path,sizeof manifest->snapshot_path,"%s",
                 preparation->snapshot_path);
        manifest->inherited_patches=preparation->inherited_patches;
        manifest->effective_patches=preparation->effective_patches;
        manifest->identity=preparation->identity;
        manifest->identity_planned=preparation->identity_planned;
    }
    snprintf(manifest->serial_transport,sizeof manifest->serial_transport,
             "%s",engine&&(engine->capabilities&EMU_CAP_SERIAL)?"engine":"unavailable");
    snprintf(manifest->frame_transport,sizeof manifest->frame_transport,
             "%s",engine&&(engine->capabilities&EMU_CAP_FRAMES)?"engine":"unavailable");
    snprintf(manifest->event_transport,sizeof manifest->event_transport,
             "%s",engine&&(engine->capabilities&EMU_CAP_EVENTS)?"engine":"unavailable");
    snprintf(manifest->artifact_state,sizeof manifest->artifact_state,"final");
}

emu_error_code_t emu_manifest_add_artifact(emu_manifest_t *manifest,
                                            const char *kind,const char *path,
                                            uint64_t size,const char *sha256,
                                            emu_error_t *error) {
    if(!manifest||!kind||!path||!sha256||strlen(sha256)!=64||
       manifest->artifact_count>=EMU_MAX_ARTIFACTS)
        return emu_manifest_fail(error,EMU_ERR_ARGUMENT,"invalid artifact");
    emu_artifact_t *a=&manifest->artifacts[manifest->artifact_count++];
    snprintf(a->kind,sizeof a->kind,"%s",kind); snprintf(a->path,sizeof a->path,"%s",path);
    a->size=size; snprintf(a->sha256,sizeof a->sha256,"%s",sha256);
    if(error){error->code=EMU_OK;error->message[0]=0;} return EMU_OK;
}

static emu_error_code_t emu_manifest_serialize_v1(const emu_manifest_t *m,
                                                   char **json,size_t *size,
                                                   emu_error_t *error) {
    if(!m||!m->prepared||!m->engine||!json||*json||!size)
        return emu_manifest_fail(error,EMU_ERR_ARGUMENT,"invalid manifest serialization request");
    const emu_prepared_session_t *p=m->prepared; emu_json_buffer_t b={0};
    emu_json_append(&b,"{\n  \"schema\":\"emu-session-manifest\",\n  \"schema_version\":1,\n  \"source\":{\"locator\":");
    emu_json_string(&b,p->source.locator); emu_json_append(&b,",\"size\":%zu,\"sha256\":\"%s\"},\n",p->source.size,p->source.sha256_hex);
    emu_json_append(&b,"  \"image_metadata\":{\"model\":"); emu_json_string(&b,p->metadata.model);
    emu_json_append(&b,",\"software_version\":%d,\"langpack\":",p->metadata.software_version); emu_json_string(&b,p->metadata.langpack);
    emu_json_append(&b,",\"bcore_software_version\":%d,\"flash_manufacturer_id\":%u,\"flash_device_id\":%u,\"flash_vendor\":",p->metadata.bcore_software_version,p->metadata.flash_manufacturer_id,p->metadata.flash_device_id); emu_json_string(&b,p->metadata.flash_vendor);
    emu_json_append(&b,",\"flash_engine\":");emu_json_string(&b,p->metadata.flash_engine);emu_json_append(&b,",\"flash_classification\":");emu_json_string(&b,p->metadata.flash_classification);
    emu_json_append(&b,",\"metadata_view_offset\":%zu,\"metadata_instance_count\":%zu,\"flash_file_order\":",p->metadata.metadata_view_offset,p->metadata.metadata_instance_count);emu_json_string(&b,p->metadata.flash_file_order);emu_json_append(&b,"},\n");
    emu_json_append(&b,"  \"selected_device\":");emu_json_string(&b,p->selected_device);emu_json_append(&b,",\n  \"physical_chips\":[");
    for(size_t i=0;i<p->chip_count;i++){const emu_chip_view_t*c=&p->chips[i];if(i)emu_json_append(&b,",");emu_json_append(&b,"{\"index\":%zu,\"role\":",i);emu_json_string(&b,c->role);emu_json_append(&b,",\"model\":");emu_json_string(&b,c->model);emu_json_append(&b,",\"source_offset\":%zu,\"size\":%zu,\"sha256\":\"%s\"}",c->source_offset,c->size,c->sha256_hex);}
    emu_json_append(&b,"],\n  \"storage_operations\":[");
    for(size_t i=0;i<p->operation_count;i++){const emu_storage_operation_t*o=&p->operations[i];if(i)emu_json_append(&b,",");emu_json_append(&b,"{\"order\":%zu,\"chip_index\":%zu,\"space\":\"%s\",\"offset\":%zu,\"expected\":",o->order,o->chip_index,emu_storage_space_name(o->space),o->offset);if(o->expected)emu_json_hex(&b,o->expected,o->expected_size);else emu_json_append(&b,"null");emu_json_append(&b,",\"replacement\":");emu_json_hex(&b,o->replacement,o->replacement_size);emu_json_append(&b,",\"provenance\":");emu_json_string(&b,o->provenance);emu_json_append(&b,"}");}
    emu_json_append(&b,"],\n  \"engine\":{\"name\":");emu_json_string(&b,m->engine->name);emu_json_append(&b,",\"version\":");emu_json_string(&b,m->engine->version);emu_json_append(&b,",\"capabilities\":%llu},\n",(unsigned long long)m->engine->capabilities);
    emu_json_append(&b,"  \"effective_options\":{\"requested_slice_ticks\":%llu,\"synthetic_mask\":%u,\"serial_autobaud_bypass\":%s},\n",(unsigned long long)p->options.requested_slice_ticks,p->options.synthetic_mask,p->options.serial_autobaud_bypass?"true":"false");
    emu_json_append(&b,"  \"result\":{\"status\":\"%s\",\"ticks\":%llu,\"guest_instructions\":%llu,\"end_icount\":%llu,\"pc\":%u,\"digest\":\"%016llx\"},\n",emu_run_status_name(m->result.status),(unsigned long long)m->result.ticks,(unsigned long long)m->result.guest_instructions,(unsigned long long)m->result.end_icount,m->result.pc,(unsigned long long)m->result.digest);
    emu_json_append(&b,"  \"artifacts\":[");for(size_t i=0;i<m->artifact_count;i++){const emu_artifact_t*a=&m->artifacts[i];if(i)emu_json_append(&b,",");emu_json_append(&b,"{\"kind\":");emu_json_string(&b,a->kind);emu_json_append(&b,",\"path\":");emu_json_string(&b,a->path);emu_json_append(&b,",\"size\":%llu,\"sha256\":\"%s\"}",(unsigned long long)a->size,a->sha256);}emu_json_append(&b,"]\n}\n");
    if(b.failed){free(b.data);return emu_manifest_fail(error,EMU_ERR_NOMEM,"cannot allocate manifest");}
    *json=b.data;*size=b.size;if(error){error->code=EMU_OK;error->message[0]=0;}return EMU_OK;
}

static const char *emu_startup_mode_name(emu_startup_mode_t mode) {
    return mode==EMU_STARTUP_RESUME?"resume":"fresh";
}

static const char *emu_identity_source_name(emu_identity_source_t source) {
    switch(source){
    case EMU_IDENTITY_SOURCE_DEFAULT:return "default";
    case EMU_IDENTITY_SOURCE_FSN:return "fsn";
    case EMU_IDENTITY_SOURCE_FILE:return "file";
    case EMU_IDENTITY_SOURCE_NONE:return "none";
    }
    return "unknown";
}

static const char *emu_storage_stage_name(emu_storage_stage_t stage) {
    return stage==EMU_STORAGE_STAGE_POST_RESTORE?"post-restore":"pre-reset";
}

static void emu_json_patch_set(emu_json_buffer_t *b,emu_patch_set_t set) {
    size_t count=0;const emu_patch_definition_t *catalog=emu_patch_catalog(&count);
    emu_json_append(b,"[");int separator=0;
    for(size_t i=0;i<count;i++)if(set&(UINT64_C(1)<<i)){
        if(separator)emu_json_append(b,",");
        emu_json_string(b,catalog[i].name);separator=1;
    }
    emu_json_append(b,"]");
}

static void emu_json_artifacts(emu_json_buffer_t *b,const emu_manifest_t *m) {
    emu_json_append(b,"[");
    for(size_t i=0;i<m->artifact_count;i++){const emu_artifact_t*a=&m->artifacts[i];if(i)emu_json_append(b,",");emu_json_append(b,"{\"kind\":");emu_json_string(b,a->kind);emu_json_append(b,",\"path\":");emu_json_string(b,a->path);emu_json_append(b,",\"size\":%llu,\"sha256\":\"%s\"}",(unsigned long long)a->size,a->sha256);}
    emu_json_append(b,"]");
}

static emu_error_code_t emu_manifest_serialize_v2(const emu_manifest_t *m,
                                                   char **json,size_t *size,
                                                   emu_error_t *error) {
    if(!m||!m->prepared||!m->engine||!json||*json||!size)
        return emu_manifest_fail(error,EMU_ERR_ARGUMENT,"invalid manifest serialization request");
    const emu_prepared_session_t *p=m->prepared;emu_json_buffer_t b={0};
    emu_json_append(&b,"{\n  \"schema\":\"emu-session-manifest\",\n  \"schema_version\":2,\n  \"startup\":{\"mode\":");
    emu_json_string(&b,emu_startup_mode_name(m->startup_mode));emu_json_append(&b,",\"identity_source\":");emu_json_string(&b,emu_identity_source_name(m->identity_source));emu_json_append(&b,",\"snapshot_path\":");if(m->snapshot_path[0])emu_json_string(&b,m->snapshot_path);else emu_json_append(&b,"null");emu_json_append(&b,"},\n");
    emu_json_append(&b,"  \"provenance\":{\"source\":{\"locator\":");emu_json_string(&b,p->source.locator);emu_json_append(&b,",\"size\":%zu,\"sha256\":\"%s\"},\"identity\":{\"planned\":%s,\"source\":",p->source.size,p->source.sha256_hex,m->identity_planned?"true":"false");emu_json_string(&b,m->identity.source);emu_json_append(&b,",\"fsn\":%u,\"imei\":",m->identity.fsn);emu_json_string(&b,m->identity.imei);emu_json_append(&b,"},\"patches\":{\"inherited\":");emu_json_patch_set(&b,m->inherited_patches);emu_json_append(&b,",\"selected\":");emu_json_patch_set(&b,m->selected_patches);emu_json_append(&b,",\"effective\":");emu_json_patch_set(&b,m->effective_patches);emu_json_append(&b,"}},\n");
    emu_json_append(&b,"  \"image_metadata\":{\"model\":");emu_json_string(&b,p->metadata.model);emu_json_append(&b,",\"software_version\":%d,\"langpack\":",p->metadata.software_version);emu_json_string(&b,p->metadata.langpack);emu_json_append(&b,",\"bcore_software_version\":%d,\"flash_manufacturer_id\":%u,\"flash_device_id\":%u,\"flash_vendor\":",p->metadata.bcore_software_version,p->metadata.flash_manufacturer_id,p->metadata.flash_device_id);emu_json_string(&b,p->metadata.flash_vendor);emu_json_append(&b,",\"flash_engine\":");emu_json_string(&b,p->metadata.flash_engine);emu_json_append(&b,",\"flash_classification\":");emu_json_string(&b,p->metadata.flash_classification);emu_json_append(&b,",\"metadata_view_offset\":%zu,\"metadata_instance_count\":%zu,\"flash_file_order\":",p->metadata.metadata_view_offset,p->metadata.metadata_instance_count);emu_json_string(&b,p->metadata.flash_file_order);emu_json_append(&b,"},\n");
    emu_json_append(&b,"  \"selected_device\":");emu_json_string(&b,p->selected_device);emu_json_append(&b,",\n  \"physical_chips\":[");for(size_t i=0;i<p->chip_count;i++){const emu_chip_view_t*c=&p->chips[i];if(i)emu_json_append(&b,",");emu_json_append(&b,"{\"index\":%zu,\"role\":",i);emu_json_string(&b,c->role);emu_json_append(&b,",\"model\":");emu_json_string(&b,c->model);emu_json_append(&b,",\"source_offset\":%zu,\"size\":%zu,\"sha256\":\"%s\"}",c->source_offset,c->size,c->sha256_hex);}emu_json_append(&b,"],\n");
    emu_json_append(&b,"  \"storage_operations\":[");for(size_t i=0;i<p->operation_count;i++){const emu_storage_operation_t*o=&p->operations[i];if(i)emu_json_append(&b,",");emu_json_append(&b,"{\"order\":%zu,\"stage\":\"%s\",\"group\":%zu,\"chip_index\":%zu,\"space\":\"%s\",\"offset\":%zu,\"expected\":",o->order,emu_storage_stage_name(o->stage),o->group,o->chip_index,emu_storage_space_name(o->space),o->offset);if(o->expected)emu_json_hex(&b,o->expected,o->expected_size);else emu_json_append(&b,"null");emu_json_append(&b,",\"replacement\":");emu_json_hex(&b,o->replacement,o->replacement_size);emu_json_append(&b,",\"provenance\":");emu_json_string(&b,o->provenance);emu_json_append(&b,"}");}emu_json_append(&b,"],\n");
    emu_json_append(&b,"  \"engine\":{\"name\":");emu_json_string(&b,m->engine->name);emu_json_append(&b,",\"version\":");emu_json_string(&b,m->engine->version);emu_json_append(&b,",\"capabilities\":%llu},\n",(unsigned long long)m->engine->capabilities);
    emu_json_append(&b,"  \"effective_options\":{\"requested_slice_ticks\":%llu,\"synthetic_mask\":%u,\"serial_autobaud_bypass\":%s},\n",(unsigned long long)p->options.requested_slice_ticks,p->options.synthetic_mask,p->options.serial_autobaud_bypass?"true":"false");
    emu_json_append(&b, "  \"transport\":{\"serial\":");
    emu_json_string(&b, m->serial_transport);
    emu_json_append(&b, ",\"frames\":");
    emu_json_string(&b, m->frame_transport);
    emu_json_append(&b, ",\"events\":");
    emu_json_string(&b, m->event_transport);
    emu_json_append(&b, ",\"gdb\":");
    if (p->options.gdb_enabled) {
        char endpoint[128];
        if (p->options.gdb_socket[0])
            snprintf(endpoint, sizeof endpoint, "unix:%s", p->options.gdb_socket);
        else
            snprintf(endpoint, sizeof endpoint, "127.0.0.1:%u", p->options.gdb_port);
        emu_json_string(&b, endpoint);
    } else emu_json_append(&b, "null");
    emu_json_append(&b, "},\n");
    emu_json_append(&b, "  \"debugger\":{\"mode\":");
    emu_json_string(&b, p->options.gdb_mode == 2 ? "interactive" :
                       p->options.gdb_mode == 1 ? "batch" :
                       p->options.gdb_enabled ? "external" : "none");
    emu_json_append(&b, ",\"binary\":");
    emu_json_string(&b, p->options.gdb_binary);
    emu_json_append(&b, ",\"actions\":[");
    for (size_t i = 0; i < p->options.gdb_action_count; i++) {
        if (i) emu_json_append(&b, ",");
        emu_json_append(&b, p->options.gdb_actions[i].script
                            ? "{\"script\":" : "{\"command\":");
        emu_json_string(&b, p->options.gdb_actions[i].value);
        emu_json_append(&b, "}");
    }
    emu_json_append(&b, "]},\n");
    emu_json_append(&b,"  \"result\":{\"status\":\"%s\",\"ticks\":%llu,\"guest_instructions\":%llu,\"end_icount\":%llu,\"pc\":%u,\"digest\":\"%016llx\"},\n",emu_run_status_name(m->result.status),(unsigned long long)m->result.ticks,(unsigned long long)m->result.guest_instructions,(unsigned long long)m->result.end_icount,m->result.pc,(unsigned long long)m->result.digest);
    emu_json_append(&b,"  \"artifact_lifecycle\":{\"state\":");emu_json_string(&b,m->artifact_state);emu_json_append(&b,",\"explicit_writeback\":%s},\n  \"artifacts\":",m->explicit_writeback?"true":"false");emu_json_artifacts(&b,m);emu_json_append(&b,"\n}\n");
    if(b.failed){free(b.data);return emu_manifest_fail(error,EMU_ERR_NOMEM,"cannot allocate manifest");}
    *json=b.data;*size=b.size;if(error){error->code=EMU_OK;error->message[0]=0;}return EMU_OK;
}

emu_error_code_t emu_manifest_serialize(const emu_manifest_t *m,
                                         char **json,size_t *size,
                                         emu_error_t *error) {
    if(m&&m->schema_version==2)
        return emu_manifest_serialize_v2(m,json,size,error);
    return emu_manifest_serialize_v1(m,json,size,error);
}
