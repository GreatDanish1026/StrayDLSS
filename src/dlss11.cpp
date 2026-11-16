// NVIDIA DLSS Super Resolution through NGX (Direct3D 11; from RE2DLSS-DX11 1.1.0). This software contains source code
// provided by NVIDIA Corporation (the NGX helper headers in third_party/dlss).

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <d3d11_1.h>

#include "common.hpp"
#include "dlss11.hpp"

#include <nvsdk_ngx.h>
#include <nvsdk_ngx_helpers.h>
#include <nvsdk_ngx_helpers_d3d.h>

#include <cstdio>
#include <mutex>

namespace dlss11 {
namespace {

constexpr const char* kProjectId = "6f1d6e2a-5b8c-4f2e-9a43-7c2b8e1d4a90";

std::mutex g_mutex;
ID3D11Device* g_device = nullptr;
bool g_initTried = false, g_ready = false;
std::string g_initError;
NVSDK_NGX_Parameter* g_params = nullptr;
std::string g_status = "not started";
ID3DDeviceContextState* g_cleanState = nullptr;  // an empty pipeline state NGX runs in

struct Feature {
    NVSDK_NGX_Handle* handle = nullptr;
    unsigned renderW = 0, renderH = 0, outW = 0, outH = 0;
    int preset = -1;
    bool autoExposure = true;
    bool mvJittered = false;
} g_feature;

void NVSDK_CONV NgxLog(const char* message, NVSDK_NGX_Logging_Level, NVSDK_NGX_Feature) {
    std::string m = message ? message : "";
    while (!m.empty() && (m.back() == '\n' || m.back() == '\r')) m.pop_back();
    rd::Log("NGX: %s", m.c_str());
}

std::string ResultText(NVSDK_NGX_Result r) {
    char buf[64];
    snprintf(buf, sizeof(buf), "0x%08x", static_cast<unsigned>(r));
    return buf;
}

const char* QualityName(NVSDK_NGX_PerfQuality_Value q) {
    switch (q) {
        case NVSDK_NGX_PerfQuality_Value_DLAA: return "DLAA";
        case NVSDK_NGX_PerfQuality_Value_UltraQuality: return "Ultra Quality";
        case NVSDK_NGX_PerfQuality_Value_MaxQuality: return "Quality";
        case NVSDK_NGX_PerfQuality_Value_Balanced: return "Balanced";
        case NVSDK_NGX_PerfQuality_Value_MaxPerf: return "Performance";
        case NVSDK_NGX_PerfQuality_Value_UltraPerformance: return "Ultra Performance";
        default: return "?";
    }
}

// The DLSS mode whose optimal render size is closest to the game's, among those whose supported
// range holds it. (The first mode whose range holds it, as before, named every scale down to 50%
// "Quality": the ranges overlap.)
NVSDK_NGX_PerfQuality_Value PickQuality(unsigned renderW, unsigned renderH, unsigned outW, unsigned outH) {
    if (renderW >= outW && renderH >= outH) return NVSDK_NGX_PerfQuality_Value_DLAA;
    const NVSDK_NGX_PerfQuality_Value order[] = {
        NVSDK_NGX_PerfQuality_Value_UltraQuality, NVSDK_NGX_PerfQuality_Value_MaxQuality, NVSDK_NGX_PerfQuality_Value_Balanced,
        NVSDK_NGX_PerfQuality_Value_MaxPerf,      NVSDK_NGX_PerfQuality_Value_UltraPerformance};
    NVSDK_NGX_PerfQuality_Value best = NVSDK_NGX_PerfQuality_Value_MaxQuality;
    unsigned bestDiff = UINT32_MAX;
    for (NVSDK_NGX_PerfQuality_Value q : order) {
        unsigned optW = 0, optH = 0, maxW = 0, maxH = 0, minW = 0, minH = 0;
        float sharp = 0;
        if (NVSDK_NGX_FAILED(NGX_DLSS_GET_OPTIMAL_SETTINGS(g_params, outW, outH, q, &optW, &optH, &maxW, &maxH, &minW, &minH,
                                                           &sharp)) ||
            !optW)
            continue;
        if (renderW < minW || renderW > maxW || renderH < minH || renderH > maxH) continue;
        const unsigned diff = optW > renderW ? optW - renderW : renderW - optW;
        if (diff < bestDiff) bestDiff = diff, best = q;
    }
    return best;
}

// The model for Automatic, by render scale: K down to Balanced (58%), M around Performance (50%),
// L at Ultra Performance (33%).
int AutoPreset(unsigned renderW, unsigned outputW) {
    const float r = outputW ? static_cast<float>(renderW) / outputW : 1.0f;
    return r >= 0.54f ? 11 : r >= 0.42f ? 13 : 12;
}

bool CreateFeature(ID3D11DeviceContext* context, const Inputs& in, std::string& error) {
    if (g_feature.handle) NVSDK_NGX_D3D11_ReleaseFeature(g_feature.handle);  // D3D11: no frames in flight to wait for
    g_feature.handle = nullptr;
    const NVSDK_NGX_PerfQuality_Value q = PickQuality(in.renderWidth, in.renderHeight, in.outputWidth, in.outputHeight);
    const char* presetKeys[] = {
        NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA,        NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality,
        NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Balanced,    NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Performance,
        NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraPerformance, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraQuality};
    for (const char* k : presetKeys) NVSDK_NGX_Parameter_SetUI(g_params, k, static_cast<unsigned>(in.preset));
    NVSDK_NGX_DLSS_Create_Params cp = {};
    cp.Feature.InWidth = in.renderWidth;
    cp.Feature.InHeight = in.renderHeight;
    cp.Feature.InTargetWidth = in.outputWidth;
    cp.Feature.InTargetHeight = in.outputHeight;
    cp.Feature.InPerfQualityValue = q;
    cp.InFeatureCreateFlags = NVSDK_NGX_DLSS_Feature_Flags_IsHDR | NVSDK_NGX_DLSS_Feature_Flags_MVLowRes |
                              NVSDK_NGX_DLSS_Feature_Flags_DepthInverted |
                              (in.exposure ? 0 : NVSDK_NGX_DLSS_Feature_Flags_AutoExposure) |
                              (in.mvJittered ? NVSDK_NGX_DLSS_Feature_Flags_MVJittered : 0);
    const NVSDK_NGX_Result r = NGX_D3D11_CREATE_DLSS_EXT(context, &g_feature.handle, g_params, &cp);
    if (NVSDK_NGX_FAILED(r)) {
        g_feature.handle = nullptr;
        error = "DLSS feature creation failed (" + ResultText(r) + ")";
        return false;
    }
    g_feature.mvJittered = in.mvJittered;
    g_feature.renderW = in.renderWidth, g_feature.renderH = in.renderHeight;
    g_feature.outW = in.outputWidth, g_feature.outH = in.outputHeight;
    g_feature.preset = in.preset, g_feature.autoExposure = !in.exposure;
    rd::Log("DLSS feature created: %ux%u -> %ux%u, %s mode, preset %d, %s exposure", in.renderWidth, in.renderHeight,
            in.outputWidth, in.outputHeight, QualityName(q), in.preset, in.exposure ? "game" : "auto");
    return true;
}

}  // namespace

bool Init(ID3D11Device* device, std::string& error) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_initTried) {
        error = g_initError;
        return g_ready;
    }
    g_initTried = true;
    g_device = device;

    // Stray: no ID3DDeviceContextState swap (RE2DLSS-DX11 swapped one in around NGX). UE 4's D3D11
    // RHI keeps queries open across the frame, and swapping the context state mid-frame made D3D11
    // throw (abort) in the game's next calls (1.2.0-dx11a, 2026-11-16). The caller saves and
    // restores the compute stage around Evaluate instead (upscaler11.cpp).

    static const std::wstring dir = rd::ExeDir();
    static const wchar_t* paths[] = {dir.c_str()};
    NVSDK_NGX_FeatureCommonInfo info = {};
    info.PathListInfo.Path = paths;
    info.PathListInfo.Length = 1;
    info.LoggingInfo.LoggingCallback = &NgxLog;
    info.LoggingInfo.MinimumLoggingLevel = NVSDK_NGX_LOGGING_LEVEL_ON;
    info.LoggingInfo.DisableOtherLoggingSinks = true;
    NVSDK_NGX_Result r = NVSDK_NGX_D3D11_Init_with_ProjectID(kProjectId, NVSDK_NGX_ENGINE_TYPE_CUSTOM, "1.0", dir.c_str(),
                                                             device, &info);
    if (NVSDK_NGX_FAILED(r)) {
        g_initError = "NGX didn't start (" + ResultText(r) + "): NVIDIA GPU and driver needed";
        g_status = g_initError;
        rd::Log("%s", g_initError.c_str());
        error = g_initError;
        return false;
    }
    r = NVSDK_NGX_D3D11_GetCapabilityParameters(&g_params);
    int available = 0;
    if (NVSDK_NGX_SUCCEED(r)) NVSDK_NGX_Parameter_GetI(g_params, NVSDK_NGX_Parameter_SuperSampling_Available, &available);
    if (!available) {
        int needsUpdate = 0;
        unsigned minMajor = 0, minMinor = 0;
        if (g_params) {
            NVSDK_NGX_Parameter_GetI(g_params, NVSDK_NGX_Parameter_SuperSampling_NeedsUpdatedDriver, &needsUpdate);
            NVSDK_NGX_Parameter_GetUI(g_params, NVSDK_NGX_Parameter_SuperSampling_MinDriverVersionMajor, &minMajor);
            NVSDK_NGX_Parameter_GetUI(g_params, NVSDK_NGX_Parameter_SuperSampling_MinDriverVersionMinor, &minMinor);
        }
        g_initError = needsUpdate ? "DLSS needs a newer NVIDIA driver (" + std::to_string(minMajor) + "." +
                                        std::to_string(minMinor) + " or later)"
                                  : "DLSS not available: is nvngx_dlss.dll next to the game exe?";
        g_status = g_initError;
        rd::Log("%s", g_initError.c_str());
        error = g_initError;
        return false;
    }
    g_ready = true;
    g_status = "ready";
    rd::Log("NGX ready (Direct3D 11), DLSS Super Resolution available");
    return true;
}

bool Evaluate(ID3D11DeviceContext* context, const Inputs& requested, std::string& error) {
    Inputs in = requested;
    if (in.preset == kPresetAuto) in.preset = AutoPreset(in.renderWidth, in.outputWidth);
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_ready) {
        error = g_initError.empty() ? "DLSS not started" : g_initError;
        return false;
    }
    bool ok = true;
    const bool fresh = !g_feature.handle || in.renderWidth != g_feature.renderW || in.renderHeight != g_feature.renderH ||
                       in.outputWidth != g_feature.outW || in.outputHeight != g_feature.outH ||
                       in.preset != g_feature.preset || g_feature.autoExposure != !in.exposure ||
                       g_feature.mvJittered != in.mvJittered;
    if (fresh && !CreateFeature(context, in, error)) ok = false;
    if (ok) {
        NVSDK_NGX_D3D11_DLSS_Eval_Params ep = {};
        ep.Feature.pInColor = in.color;
        ep.Feature.pInOutput = in.output;
        ep.pInDepth = in.depth;
        ep.pInMotionVectors = in.motion;
        ep.InJitterOffsetX = in.jitterX;
        ep.InJitterOffsetY = in.jitterY;
        ep.InRenderSubrectDimensions = {in.renderWidth, in.renderHeight};
        ep.InReset = in.reset || fresh ? 1 : 0;
        ep.InMVScaleX = in.motionScaleX;
        ep.InMVScaleY = in.motionScaleY;
        ep.InPreExposure = in.preExposure;
        ep.InExposureScale = 1.0f;
        ep.pInExposureTexture = in.exposure;
        ep.pInBiasCurrentColorMask = in.biasMask;
        const NVSDK_NGX_Result r = NGX_D3D11_EVALUATE_DLSS_EXT(context, g_feature.handle, g_params, &ep);
        if (NVSDK_NGX_FAILED(r)) {
            error = "DLSS evaluation failed (" + ResultText(r) + ")";
            ok = false;
        }
    }

    if (!ok) {
        g_status = error;
        return false;
    }
    char buf[160];
    const unsigned scale = g_feature.outW ? (g_feature.renderW * 100 + g_feature.outW / 2) / g_feature.outW : 0;
    snprintf(buf, sizeof(buf), "running: %ux%u -> %ux%u (%u%%%s), preset %c", g_feature.renderW, g_feature.renderH,
             g_feature.outW, g_feature.outH, scale, scale >= 100 ? ", DLAA" : "",
             g_feature.preset ? static_cast<char>('A' + g_feature.preset - 1) : 'D');
    g_status = buf;
    return true;
}

void ReleaseFeatures() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_feature.handle) NVSDK_NGX_D3D11_ReleaseFeature(g_feature.handle);
    g_feature = Feature();
}

void Shutdown() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_feature.handle) NVSDK_NGX_D3D11_ReleaseFeature(g_feature.handle);
    g_feature = Feature();
    if (g_params) NVSDK_NGX_D3D11_DestroyParameters(g_params);
    g_params = nullptr;
    if (g_ready && g_device) NVSDK_NGX_D3D11_Shutdown1(g_device);
    if (g_cleanState) g_cleanState->Release();
    g_cleanState = nullptr;
    g_ready = false;
    g_initTried = false;
    g_device = nullptr;
}

std::string Status() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_status;
}

}  // namespace dlss11
