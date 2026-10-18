#include <cassert>
#include <cstdio>
#include "video_core/renderer_vulkan/postprocess_capture_budget.h"
int main() {
    Vulkan::PostprocessCaptureBudget b;
    for (unsigned f=0; f<600000; ++f) {
        assert(b.Begin(0,f)==-1);
        assert(b.NextStage(f)==-1);
    }
    for (unsigned r=1;r<=3;++r) {
        const unsigned f=600000+r;
        assert(b.Begin(r,f)==int(r-1));
        assert(b.Begin(r,f)==-1); // repeated dispatch cannot rearm
        assert(b.NextStage(f+1)==-1); // never mix command-stream frames
        for(unsigned s=0;s<16;++s) assert(b.NextStage(f)==int(s));
        assert(b.NextStage(f)==-1);
        b.End();
        assert(b.NextStage(f)==-1);
        assert(b.Begin(r,f+1)==-1);
    }
    for(unsigned r=4;r<10000;++r) assert(b.Begin(r,r)==-1);
    Vulkan::PostprocessCaptureBudget delayed;
    assert(delayed.Begin(2,99)==0);
    delayed.End();
    assert(delayed.Begin(3,100)==1);
    assert(delayed.Begin(4,101)==-1);
    std::puts("PASS: long idle, frame pairing, stage/process limits, no rearm, coalescing");
}
