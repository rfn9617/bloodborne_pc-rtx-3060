#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include "bbport_toggles.h"
#include "video_core/renderer_vulkan/vk_frame_capture.h"
#include "video_core/texture_cache/image_info.h"

int main() {
    const char* directory=std::getenv("BB_CAPTURE_DIR");
    assert(directory);
    std::filesystem::create_directories(directory);
    const char* value=std::getenv("BB_CAPTURE_DRAW_DETAILS");
    const bool detailed=value && value[0]=='1';
    VideoCore::ImageInfo display{};
    display.guest_address=0x1000;
    display.pixel_format=vk::Format::eR8G8B8A8Unorm;
    display.size.width=1920;display.size.height=1080;
    VideoCore::ImageInfo texture=display;
    texture.guest_address=0x2000;
    texture.size.width=256;texture.size.height=256;
    const VideoCore::ImageInfo* targets[]={&display};
    Vulkan::FrameCapture::AddDisplayBuffer(display.guest_address);
    assert(!Vulkan::FrameCapture::Active());
    BbStats::snapshot_seq.store(1);
    Vulkan::FrameCapture::OnFlip(display.guest_address);
    assert(Vulkan::FrameCapture::Active());
    Vulkan::FrameCapture::BeginPass(targets,1,nullptr);
    for(u32 i=1;i<=6;++i) {
        float data[8]={float(i),2,3,4,5,6,7,8};
        texture.guest_address=0x2000+i*0x100;
        Vulkan::FrameCapture::Sampled(texture,false);
        Vulkan::FrameCapture::Buffer(0x2222,i,0x3000+i*0x100,data,sizeof(data));
        Vulkan::FrameCapture::Draw(0x1111,0x2222,6,1);
    }
    Vulkan::FrameCapture::Note("\n  fixture state");
    // A different display pass ends this capture. All writes are synchronous metadata.
    display.guest_address=0x4000;
    Vulkan::FrameCapture::AddDisplayBuffer(display.guest_address);
    Vulkan::FrameCapture::BeginPass(targets,1,nullptr);
    assert(!Vulkan::FrameCapture::Active());
    std::string contents;
    size_t files=0;
    for(const auto& entry:std::filesystem::directory_iterator(directory)) {
        if(entry.path().extension()!=".txt") continue;
        ++files;
        std::ifstream input(entry.path());
        contents.assign(std::istreambuf_iterator<char>(input),{});
    }
    assert(files==1);
    assert(contents.find("PASS draws 6")!=std::string::npos);
    assert(contents.find("fixture state")!=std::string::npos);
    assert((contents.find("DRAW 6 vs 0000000000001111 ps 0000000000002222")!=std::string::npos)==detailed);
    assert((contents.find("buffer stage 0000000000002222 slot 6")!=std::string::npos)==detailed);
    assert(contents.find("samples 0x2600")!=std::string::npos);
    assert(contents.find("truncated 1")==std::string::npos);
    // Once idle, draw calls cannot create another recording or output file.
    Vulkan::FrameCapture::Draw(0x1111,0x2222,6,1);
    assert(!Vulkan::FrameCapture::Active());
    std::puts(detailed?"PASS: detailed snapshot captures later draws and per-draw constants":"PASS: default snapshot retains legacy metadata");
    // A postprocess capture starts on the GPU thread at the HDR tap, with a
    // serial shared by image readbacks; it does not wait for a later flip.
    Vulkan::FrameCapture::BeginPostprocessSnapshot(2, 123456);
    assert(Vulkan::FrameCapture::Active());
    const VideoCore::ImageInfo* post_targets[]={&texture};
    Vulkan::FrameCapture::BeginPass(post_targets,1,nullptr);
    float constants[4]={10,20,30,40};
    Vulkan::FrameCapture::Buffer(0x9876,3,0x5000,constants,sizeof(constants));
    Vulkan::FrameCapture::Draw(0x1234,0x9876,4,1);
    Vulkan::FrameCapture::BeginPass(targets,1,nullptr);
    assert(!Vulkan::FrameCapture::Active());
    files=0;
    bool found=false;
    for(const auto& entry:std::filesystem::directory_iterator(directory)) {
        if(entry.path().extension()!=".txt") continue;
        ++files;
        std::ifstream input(entry.path());
        std::string text(std::istreambuf_iterator<char>(input),{});
        if(text.find("postprocess request 2 command-stream frame 123456")==std::string::npos) continue;
        found=true;
        assert(text.find("PASS draws 1")!=std::string::npos);
        assert(text.find("0000000000009876")!=std::string::npos);
        assert(text.find("fixture state")==std::string::npos);
        assert(text.find("buffer stage 0000000000009876 slot 3")!=std::string::npos);
    }
    assert(found && files==2);
    std::puts("PASS: exact postprocess frame stamp, constants, no stale previous-frame metadata");
}
