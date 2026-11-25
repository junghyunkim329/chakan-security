#include "canids.h"
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); return 1; } } while (0)

static int test_boundaries(void) {
    canids_profile p = {.bus=0, .can_id=0x100, .allowed_dlcs=1u<<2, .min_interval_us=100, .max_count_1s=2};
    int error=99;
    canids_detector *d=canids_create(&p,1,1000000,&error);
    CHECK(d != NULL && error == CANIDS_OK);
    canids_frame f={.bus=0,.can_id=0x100,.dlc=2,.timestamp_us=0};
    canids_finding out[4]; size_t n=99;
    CHECK(canids_observe(d,&f,out,4,&n)==CANIDS_OK && n==0);
    f.timestamp_us=100;
    CHECK(canids_observe(d,&f,out,4,&n)==CANIDS_OK && n==0);
    f.timestamp_us=150;
    CHECK(canids_observe(d,&f,out,4,&n)==CANIDS_OK && n==2);
    CHECK(out[0].rule==CANIDS_INTERVAL_SHORT && out[0].observed==50 && out[0].threshold==100);
    CHECK(out[1].rule==CANIDS_RATE_HIGH && out[1].observed==3 && out[1].threshold==2);
    canids_destroy(d);
    d=canids_create(&p,1,1000000,&error);
    CHECK(d != NULL);
    f.timestamp_us=0; CHECK(canids_observe(d,&f,out,4,&n)==0 && n==0);
    f.timestamp_us=999999; CHECK(canids_observe(d,&f,out,4,&n)==0 && n==0);
    f.timestamp_us=1000000; CHECK(canids_observe(d,&f,out,4,&n)==0 && n==1);
    CHECK(out[0].rule==CANIDS_INTERVAL_SHORT);
    canids_destroy(d);
    return 0;
}

static int test_validation_and_state(void) {
    canids_profile p={.bus=0,.can_id=0x100,.allowed_dlcs=1u<<2,.min_interval_us=100,.max_count_1s=2};
    int error;
    canids_detector *d=canids_create(&p,1,1000000,&error);
    CHECK(d != NULL);
    canids_frame f={.bus=0,.can_id=0x777,.dlc=2,.timestamp_us=0};
    canids_finding out[4];size_t n;
    for (int i=0;i<3;++i) {
        f.timestamp_us=i;
        CHECK(canids_observe(d,&f,out,4,&n)==0 && n==1 && out[0].rule==CANIDS_ID_UNSEEN);
    }
    f.can_id=0x100;f.bus=1;f.timestamp_us=3;
    CHECK(canids_observe(d,&f,out,4,&n)==0 && n==1 && out[0].rule==CANIDS_ID_UNSEEN);
    f.bus=0;f.timestamp_us=10;
    CHECK(canids_observe(d,&f,out,4,&n)==0 && n==0);
    f.timestamp_us=9;
    CHECK(canids_observe(d,&f,out,4,&n)==CANIDS_TIME_REGRESSION && n==0);
    f.timestamp_us=20;
    CHECK(canids_observe(d,&f,out,1,&n)==CANIDS_INVALID_ARGUMENT && n==0);
    f.timestamp_us=10;
    CHECK(canids_observe(d,&f,out,4,&n)==0 && n==1 && out[0].observed==0);
    f.dlc=9;CHECK(canids_observe(d,&f,out,4,&n)==CANIDS_INVALID_ARGUMENT);
    f.dlc=2;f.bus=3;CHECK(canids_observe(d,&f,out,4,&n)==CANIDS_INVALID_ARGUMENT);
    f.bus=0;f.can_id=0x800;CHECK(canids_observe(d,&f,out,4,&n)==CANIDS_INVALID_ARGUMENT);
    f.can_id=0x100;f.timestamp_us=-1;CHECK(canids_observe(d,&f,out,4,&n)==CANIDS_INVALID_ARGUMENT);
    canids_destroy(d);
    canids_profile duplicate[2]={p,p};
    CHECK(canids_create(duplicate,2,1000000,&error)==NULL && error==CANIDS_INVALID_ARGUMENT);
    CHECK(canids_create(&p,1,0,&error)==NULL);
    p.allowed_dlcs=0;CHECK(canids_create(&p,1,1000000,&error)==NULL);
    p.allowed_dlcs=1u<<9;CHECK(canids_create(&p,1,1000000,&error)==NULL);
    p.allowed_dlcs=1u<<2;p.min_interval_us=-2;
    CHECK(canids_create(&p,1,1000000,&error)==NULL);
    canids_destroy(NULL);
    return 0;
}

static int test_ring_growth_wrap_and_expiry(void) {
    canids_profile p={.bus=0,.can_id=0x100,.allowed_dlcs=1u<<2,.min_interval_us=-1,.max_count_1s=2};
    int error;canids_detector *d=canids_create(&p,1,1000000,&error);
    CHECK(d != NULL);
    canids_frame f={.bus=0,.can_id=0x100,.dlc=2,.timestamp_us=100};
    canids_finding out[4];size_t n;
    for (size_t i=1;i<=4096;++i) {
        CHECK(canids_observe(d,&f,out,4,&n)==0);
        CHECK(n==(i>2 ? 1u : 0u));
        if (n) CHECK(out[0].rule==CANIDS_RATE_HIGH && out[0].observed==i);
    }
    f.timestamp_us=1000100;
    CHECK(canids_observe(d,&f,out,4,&n)==0 && n==0);
    for (size_t i=2;i<=5000;++i) {
        f.timestamp_us=1000101;
        CHECK(canids_observe(d,&f,out,4,&n)==0);
        if (i>2) CHECK(n==1 && out[0].observed==i);
    }
    f.timestamp_us=2000100;
    CHECK(canids_observe(d,&f,out,4,&n)==0 && n==1 && out[0].observed==5000);
    f.timestamp_us=2000101;
    CHECK(canids_observe(d,&f,out,4,&n)==0 && n==0);
    canids_destroy(d);
    return 0;
}

int main(void) {
    CHECK(canids_abi_version()==1);
    CHECK(canids_frame_size()==sizeof(canids_frame));
    CHECK(canids_profile_size()==sizeof(canids_profile));
    CHECK(canids_finding_size()==sizeof(canids_finding));
    CHECK(test_boundaries()==0);
    CHECK(test_validation_and_state()==0);
    CHECK(test_ring_growth_wrap_and_expiry()==0);
    puts("canids native tests passed");
    return EXIT_SUCCESS;
}
