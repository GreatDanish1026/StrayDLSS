// NVIDIA DLSS Super Resolution through NGX's Direct3D 11 interface (DLSS SDK 310.9.16).
#pragma once

#include <string>

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11Resource;

namespace dlss11 {

// Inputs::preset value for "Automatic": the model NVIDIA recommends for the render scale (K
// for DLAA, Quality and Balanced, M for Performance, L for Ultra Performance).
constexpr int kPresetAuto = -1;

struct Inputs {
    ID3D11Resource* color = nullptr;   // render resolution, linear HDR, jittered
    ID3D11Resource* depth = nullptr;   // render resolution, reversed Z
    ID3D11Resource* motion = nullptr;  // render resolution
    ID3D11Resource* output = nullptr;  // output resolution, created with unordered access
    unsigned renderWidth = 0, renderHeight = 0;
    unsigned outputWidth = 0, outputHeight = 0;
    float jitterX = 0, jitterY = 0;  // in render pixels
    float motionScaleX = 1, motionScaleY = 1;
    bool reset = false;
    float preExposure = 1;
    ID3D11Resource* exposure = nullptr;  // 1x1, null: DLSS's auto-exposure
    ID3D11Resource* biasMask = nullptr;  // r8, 1 = trust this frame (particles, effects)
    int preset = 0;                      // NVSDK_NGX_DLSS_Hint_Render_Preset_*, or kPresetAuto
    bool mvJittered = false;             // the motion vectors include the jitter
};

// Loads NGX for the device (once). False with a reason if DLSS can't run.
bool Init(ID3D11Device* device, std::string& error);

// Runs DLSS on the immediate context. One feature (history), recreated when the sizes, preset or
// exposure mode change. NGX changes the context's compute bindings: the caller saves and restores
// them (upscaler11.cpp's CsState).
bool Evaluate(ID3D11DeviceContext* context, const Inputs& in, std::string& error);

void ReleaseFeatures();
void Shutdown();
std::string Status();

}  // namespace dlss11
