#ifndef EMU_TEST_SUPPORT_H
#define EMU_TEST_SUPPORT_H
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "emu_engine.h"
#define EMU_CHECK(x) do{if(!(x)){fprintf(stderr,"FAIL %s:%d: %s\n",__FILE__,__LINE__,#x);exit(1);}}while(0)
static inline void emu_test_prepared(emu_prepared_session_t*p){
    static const char locator[]="memory:\xF0\x9F\x93\xB1";emu_prepared_init(p);p->source.locator=malloc(sizeof locator);memcpy(p->source.locator,locator,sizeof locator);p->source.size=16;p->source.bytes=malloc(16);for(size_t i=0;i<16;i++)p->source.bytes[i]=(uint8_t)i;emu_sha256(p->source.bytes,p->source.size,p->source.sha256);emu_sha256_hex(p->source.sha256,p->source.sha256_hex);snprintf(p->metadata.model,sizeof p->metadata.model,"FAKE");snprintf(p->metadata.langpack,sizeof p->metadata.langpack,"lg1");snprintf(p->metadata.flash_vendor,sizeof p->metadata.flash_vendor,"test");snprintf(p->metadata.flash_engine,sizeof p->metadata.flash_engine,"fake-nor");snprintf(p->metadata.flash_classification,sizeof p->metadata.flash_classification,"exact");snprintf(p->metadata.flash_file_order,sizeof p->metadata.flash_file_order,"single");snprintf(p->selected_device,sizeof p->selected_device,"fake");p->chip_count=1;snprintf(p->chips[0].role,sizeof p->chips[0].role,"flash");snprintf(p->chips[0].model,sizeof p->chips[0].model,"fake-nor");p->chips[0].size=16;emu_sha256(p->source.bytes,16,p->chips[0].sha256);emu_sha256_hex(p->chips[0].sha256,p->chips[0].sha256_hex);p->options.requested_slice_ticks=7;
}
#endif
