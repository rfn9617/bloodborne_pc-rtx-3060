// SPDX-License-Identifier: GPL-2.0-or-later
#include "bbport_settings.h"
#include "video_core/renderer_vulkan/scene_resolution.h"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cmath>
int main(int argc,char**argv) {
    assert(argc==2);
#ifdef _WIN32
    _putenv_s("BB_CONFIG",argv[1]);
#else
    setenv("BB_CONFIG",argv[1],1);
#endif
    using namespace BbSettings;using Vulkan::SceneResolution::ForPreset;
    static_assert(Quality==1 && UltraPerformance==4 && UltraQuality==5);
    const auto a=ForPreset(UltraQuality);assert(a.width==1440 && a.height==810);
    const auto b=ForPreset(UltraQuality,{2560,1440});assert(b.width==1920 && b.height==1080);
    const auto q=ForPreset(Quality);assert(q.width==1280 && q.height==720);
    assert(std::abs(PresetScale(UltraQuality)-4.f/3.f)<1e-6f);
    auto& s=Get();s.upscaler=UpscalerDlss;s.preset=UltraQuality;s.dlss_model=11;
    Save();s.preset=NativeAA;Load();assert(s.preset==UltraQuality && s.upscaler==UpscalerDlss && s.dlss_model==11);
    std::remove(argv[1]);puts("Ultra Quality: stable preset IDs, 1440x810 / 1920x1080, config roundtrip PASS");
}
