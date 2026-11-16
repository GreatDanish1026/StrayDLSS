// DLSS in place of Stray's TAA on the DirectX 11 renderer (the game's default; -dx12 runs
// upscaler.cpp instead). main.cpp registers this when the game wasn't started with -dx12.
//
// The DX11 build runs the same TAA shader bytecode as DX12 (docs/FINDINGS.md, probe run 1):
// cs37876d91 (100%) and csed0bf55b (TAAU, below 100%), with t0 exposure, t1 scene colour
// (r11g11b10f), t2 depth (r32g8x24), t3 velocity (rgba16unorm), t5 history, u0 output (rgba16f).
// D3D11 binds slots, not tables, so the compute stage's t0-t5, u0 and b0-b4 are tracked per device
// context, and the View uniform buffer's first bytes are kept from the CPU's writes to it (D3D11
// constant buffers can't be read back).
//
// At the TAA dispatch, on the immediate context:
//   motion11.hlsl: our motion vectors (camera motion from depth and View.ClipToPrevClip, engine
//     velocity for moving objects) and an r32f copy of the depth;
//   DLSS (dlss11.cpp, NGX D3D11): colour, that depth and motion in, the TAA's output written.
// Both run in a clean ID3DDeviceContextState swapped in and out, so the game's bindings stay as
// they were. The game's dispatch is then skipped; its output is next frame's history too.
//
// Render resolution: the DLSS mode sets r.ScreenPercentage / r.TemporalAA.Upsampling on the game
// thread through upscaler.cpp's code (ResolutionTick), as on DX12. No FSR, frame generation or Ray
// Reconstruction on DX11 (yet).

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <d3d11_1.h>

#include "common.hpp"
#include "dlss11.hpp"
#include "motion11_cs.h"  // kMotion11Cs: motion11.hlsl compiled by build.ps1
#include "upscaler11.hpp"
#include "version.h"

#include <atomic>
#include <cmath>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

using namespace reshade::api;

namespace {

using rd::Log;

constexpr int kToggleHotkey = VK_F9;

// ---------------------------------------------------------------- settings (shared with DX12's)

enum DlssMode { kModeGame, kModeDlaa, kModeQuality, kModeBalanced, kModePerformance, kModeUltraPerformance, kModeCount };
const char* const kModeNames[kModeCount] = {"Game's (100%)", "Native / DLAA (100%)", "Quality (67%)",
                                            "Balanced (58%)", "Performance (50%)", "Ultra Performance (33%)"};
const char* const kModelNames[] = {"Automatic (recommended)", "K (transformer)", "J (transformer)",
                                   "L (transformer, Ultra Performance)", "M (transformer, Performance)", "NVIDIA's default"};
const int kModelIds[] = {dlss11::kPresetAuto, 11, 10, 12, 13, 0};
constexpr int kModelCount = 6;

struct Settings {
    bool enabled = false;
    int mode = 0;
    int preset = dlss11::kPresetAuto;
} g_set;

void LoadSettings() {
    reshade::get_config_value(nullptr, RD_PRODUCT, "Enabled", g_set.enabled);
    reshade::get_config_value(nullptr, RD_PRODUCT, "DlssMode", g_set.mode);
    if (g_set.mode < 0 || g_set.mode >= kModeCount) g_set.mode = 0;
    reshade::get_config_value(nullptr, RD_PRODUCT, "DlssModel", g_set.preset);
    bool known = false;
    for (int id : kModelIds) known |= id == g_set.preset;
    if (!known) g_set.preset = dlss11::kPresetAuto;
}

void SaveSettings() {
    reshade::set_config_value(nullptr, RD_PRODUCT, "Enabled", g_set.enabled);
    reshade::set_config_value(nullptr, RD_PRODUCT, "DlssMode", g_set.mode);
    reshade::set_config_value(nullptr, RD_PRODUCT, "DlssModel", g_set.preset);
}

// ---------------------------------------------------------------- shaders

constexpr uint32_t kTaaIds[] = {rd::kTaaMain, 0xed0bf55b};  // 100% and TAAU (both APIs)

struct ShaderInfo {
    uint32_t id = 0;
    bool taaRegs = false;  // declares exactly t0-t5 and u0
};

std::mutex g_mutex;  // everything below
std::unordered_map<uint64_t, ShaderInfo> g_shaders;

uint32_t Fnv(const void* data, size_t n) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; ++i) h = (h ^ p[i]) * 16777619u;
    return h;
}

void OnInitPipeline(device*, pipeline_layout, uint32_t count, const pipeline_subobject* subobjects, pipeline pipe) {
    for (uint32_t i = 0; i < count; ++i) {
        if (subobjects[i].type != pipeline_subobject_type::compute_shader || !subobjects[i].data) continue;
        const shader_desc& s = *static_cast<const shader_desc*>(subobjects[i].data);
        if (!s.code || !s.code_size) continue;
        ShaderInfo info{Fnv(s.code, s.code_size), rd::CodeHasTaaRegisters(s.code, s.code_size)};
        std::lock_guard<std::mutex> lock(g_mutex);
        g_shaders[pipe.handle] = info;
        return;
    }
}

void OnDestroyPipeline(device*, pipeline pipe) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_shaders.erase(pipe.handle);
}

// ---------------------------------------------------------------- View buffer contents

// The View uniform buffer's bytes this code reads: up to ViewSizeAndInvSize (2080 + 16).
constexpr int kViewToClipJitterRow = 480, kViewClipToPrevClip = 1952, kViewJitter = 2016, kViewRectMin = 2064,
              kViewSize = 2080;
constexpr size_t kViewBytes = 2112;

// Constant buffers large enough to be the View buffer: their first kViewBytes as last written by
// the CPU (initial data, Map/Unmap, UpdateSubresource).
std::unordered_map<uint64_t, std::vector<uint8_t>> g_cbData;
std::unordered_map<uint64_t, void*> g_mapped;

void OnInitResource(device*, const resource_desc& desc, const subresource_data* initial, resource_usage, resource r) {
    if (desc.type != resource_type::buffer || (desc.usage & resource_usage::constant_buffer) == 0 ||
        desc.buffer.size < kViewBytes || desc.buffer.size > 65536)
        return;
    std::lock_guard<std::mutex> lock(g_mutex);
    std::vector<uint8_t>& v = g_cbData[r.handle];
    v.assign(kViewBytes, 0);
    if (initial && initial->data) memcpy(v.data(), initial->data, kViewBytes);
}

void OnMapBuffer(device*, resource r, uint64_t offset, uint64_t, map_access access, void** data) {
    if (access == map_access::read_only || !data || !*data || offset) return;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_cbData.count(r.handle)) g_mapped[r.handle] = *data;
}

void OnUnmapBuffer(device*, resource r) {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto m = g_mapped.find(r.handle);
    if (m == g_mapped.end()) return;
    memcpy(g_cbData[r.handle].data(), m->second, kViewBytes);
    g_mapped.erase(m);
}

void CopyCb(resource r, const void* data, uint64_t offset, uint64_t size) {
    if (!data) return;
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_cbData.find(r.handle);
    if (it == g_cbData.end() || offset >= kViewBytes) return;
    if (size == UINT64_MAX || offset + size > kViewBytes) size = kViewBytes - offset;
    memcpy(it->second.data() + offset, data, static_cast<size_t>(size));
}

bool OnUpdateBuffer(device*, const void* data, resource r, uint64_t offset, uint64_t size) {
    CopyCb(r, data, offset, size);
    return false;
}

bool OnUpdateBufferCommand(command_list*, const void* data, resource r, uint64_t offset, uint64_t size) {
    CopyCb(r, data, offset, size);
    return false;
}

// ---------------------------------------------------------------- compute bindings per context

struct Bindings {
    pipeline cs = {0};
    resource_view srv[6] = {};
    resource_view uav0 = {0};
    buffer_range cb[5] = {};
};
std::unordered_map<uint64_t, Bindings> g_bindings;  // device context -> compute stage

void OnBindPipeline(command_list* cl, pipeline_stage stages, pipeline pipe) {
    if ((stages & pipeline_stage::compute_shader) == 0) return;
    std::lock_guard<std::mutex> lock(g_mutex);
    g_bindings[cl->get_native()].cs = pipe;
}

void OnPushDescriptors(command_list* cl, shader_stage stages, pipeline_layout, uint32_t, const descriptor_table_update& u) {
    if ((stages & shader_stage::compute) == 0) return;
    std::lock_guard<std::mutex> lock(g_mutex);
    Bindings& b = g_bindings[cl->get_native()];
    for (uint32_t i = 0; i < u.count; ++i) {
        const uint32_t slot = u.binding + i;
        switch (u.type) {
            case descriptor_type::constant_buffer:
                if (slot < 5) b.cb[slot] = static_cast<const buffer_range*>(u.descriptors)[i];
                break;
            case descriptor_type::shader_resource_view:
            case descriptor_type::buffer_shader_resource_view:
                if (slot < 6) b.srv[slot] = static_cast<const resource_view*>(u.descriptors)[i];
                break;
            case descriptor_type::unordered_access_view:
            case descriptor_type::buffer_unordered_access_view:
                if (slot == 0) b.uav0 = static_cast<const resource_view*>(u.descriptors)[i];
                break;
            default: break;
        }
    }
}

void OnDestroyCommandList(command_list* cl) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_bindings.erase(cl->get_native());
}

// ---------------------------------------------------------------- our D3D11 objects

ID3D11Device* g_d3d = nullptr;
ID3DDeviceContextState* g_cleanState = nullptr;
ID3D11ComputeShader* g_motionCs = nullptr;
ID3D11Buffer* g_motionCb = nullptr;
bool g_objectsFailed = false;
std::string g_error;

struct Targets {  // render-size textures of ours, one set per size
    UINT w = 0, h = 0;
    ID3D11Texture2D* motion = nullptr;
    ID3D11Texture2D* mask = nullptr;
    ID3D11Texture2D* depth = nullptr;
    ID3D11UnorderedAccessView* uav[3] = {};
};
std::vector<Targets> g_targets;
std::unordered_map<uint64_t, ID3D11ShaderResourceView*> g_srvs;  // game texture -> our SRV of it

template <class T> void SafeRelease(T*& p) {
    if (p) p->Release();
    p = nullptr;
}

void ReleaseObjects() {
    for (Targets& t : g_targets) {
        for (auto*& u : t.uav) SafeRelease(u);
        SafeRelease(t.motion), SafeRelease(t.mask), SafeRelease(t.depth);
    }
    g_targets.clear();
    for (auto& [k, v] : g_srvs) v->Release();
    g_srvs.clear();
    SafeRelease(g_motionCb), SafeRelease(g_motionCs), SafeRelease(g_cleanState);
    g_d3d = nullptr;
    g_objectsFailed = false;
}

bool CreateObjects(ID3D11Device* d3d) {
    if (g_d3d == d3d && g_motionCs) return true;
    if (g_objectsFailed) return false;
    ReleaseObjects();
    g_d3d = d3d;
    ID3D11Device1* dev1 = nullptr;
    if (SUCCEEDED(d3d->QueryInterface(__uuidof(ID3D11Device1), reinterpret_cast<void**>(&dev1)))) {
        const D3D_FEATURE_LEVEL level = d3d->GetFeatureLevel();
        dev1->CreateDeviceContextState(0, &level, 1, D3D11_SDK_VERSION, __uuidof(ID3D11Device), nullptr, &g_cleanState);
        dev1->Release();
    }
    D3D11_BUFFER_DESC bd = {};
    bd.ByteWidth = 96;
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (!g_cleanState || FAILED(d3d->CreateComputeShader(kMotion11Cs, sizeof(kMotion11Cs), nullptr, &g_motionCs)) ||
        FAILED(d3d->CreateBuffer(&bd, nullptr, &g_motionCb))) {
        g_error = g_cleanState ? "couldn't create the motion vector shader" : "Direct3D 11.1 context state not available";
        Log("DX11: %s", g_error.c_str());
        g_objectsFailed = true;
        return false;
    }
    Log("DX11: motion vector shader and clean context state created");
    return true;
}

Targets* GetTargets(UINT w, UINT h) {
    for (Targets& t : g_targets)
        if (t.w == w && t.h == h) return &t;
    if (g_targets.size() >= 4) {  // render sizes no longer used
        Targets& old = g_targets.front();
        for (auto*& u : old.uav) SafeRelease(u);
        SafeRelease(old.motion), SafeRelease(old.mask), SafeRelease(old.depth);
        g_targets.erase(g_targets.begin());
    }
    Targets t;
    t.w = w, t.h = h;
    const DXGI_FORMAT fmts[3] = {DXGI_FORMAT_R16G16_FLOAT, DXGI_FORMAT_R8_UNORM, DXGI_FORMAT_R32_FLOAT};
    ID3D11Texture2D** texs[3] = {&t.motion, &t.mask, &t.depth};
    for (int i = 0; i < 3; ++i) {
        D3D11_TEXTURE2D_DESC td = {};
        td.Width = w, td.Height = h, td.MipLevels = 1, td.ArraySize = 1, td.Format = fmts[i];
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
        if (FAILED(g_d3d->CreateTexture2D(&td, nullptr, texs[i])) ||
            FAILED(g_d3d->CreateUnorderedAccessView(*texs[i], nullptr, &t.uav[i]))) {
            Log("DX11: couldn't create %ux%u textures", w, h);
            for (auto*& u : t.uav) SafeRelease(u);
            SafeRelease(t.motion), SafeRelease(t.mask), SafeRelease(t.depth);
            return nullptr;
        }
    }
    Log("DX11: created %ux%u motion, mask and depth textures", w, h);
    g_targets.push_back(t);
    return &g_targets.back();
}

ID3D11ShaderResourceView* GameSrv(resource r, DXGI_FORMAT fmt) {
    auto it = g_srvs.find(r.handle);
    if (it != g_srvs.end()) return it->second;
    D3D11_SHADER_RESOURCE_VIEW_DESC sd = {};
    sd.Format = fmt;
    sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    sd.Texture2D.MipLevels = 1;
    ID3D11ShaderResourceView* srv = nullptr;
    if (FAILED(g_d3d->CreateShaderResourceView(reinterpret_cast<ID3D11Resource*>(r.handle), &sd, &srv))) return nullptr;
    g_srvs[r.handle] = srv;
    return srv;
}

void OnDestroyResource(device*, resource r) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_cbData.erase(r.handle);
    g_mapped.erase(r.handle);
    auto it = g_srvs.find(r.handle);
    if (it != g_srvs.end()) {
        it->second->Release();
        g_srvs.erase(it);
    }
}

// ---------------------------------------------------------------- TAA

std::atomic<uint64_t> g_evaluated{0}, g_skipped{0}, g_presents{0};
std::map<std::string, uint32_t> g_why;
bool g_resetNext = true;
uint64_t g_lastSizeKey = 0;
unsigned g_lastRender[2] = {}, g_lastOutput[2] = {};
int g_viewSlot = -1;  // the cbuffer slot that holds the View buffer, once seen

void Why(const char* reason) {
    ++g_skipped;
    uint32_t& n = g_why[reason];
    if (n++ < 3) Log("DX11: TAA kept for this frame: %s", reason);
}

bool Near(float a, float b) { return std::fabs(a - b) <= std::fabs(b) * 2e-3f + 1e-9f; }

bool LooksLikeClipToPrevClip(const float* m) {
    for (int i = 0; i < 16; ++i)
        if (!std::isfinite(m[i])) return false;
    return std::fabs(m[0] - 1) < 0.2f && std::fabs(m[5] - 1) < 0.2f;
}

// The View buffer's layout as on DX12: ViewToClip's third row holds TemporalAAJitter's first two
// values, ClipToPrevClip near identity.
bool ViewLayoutOk(const float* v) {
    return v[kViewToClipJitterRow / 4] == v[kViewJitter / 4] && v[kViewToClipJitterRow / 4 + 1] == v[kViewJitter / 4 + 1] &&
           LooksLikeClipToPrevClip(v + kViewClipToPrevClip / 4) && v[kViewSize / 4] >= 16 && v[kViewSize / 4 + 1] >= 16;
}

resource Res(device* dev, resource_view v) { return v.handle ? dev->get_resource_from_view(v) : resource{0}; }

bool IsHdrColor(format f) { return f == format::r16g16b16a16_float || f == format::r11g11b10_float; }
bool IsVelocityFormat(format f) { return f == format::r16g16_unorm || f == format::r16g16b16a16_unorm; }

// g_mutex held. True if the game's dispatch is to be skipped.
bool OnTaa(command_list* cl, const Bindings& b) {
    device* dev = cl->get_device();
    const resource color = Res(dev, b.srv[1]), depth = Res(dev, b.srv[2]), velocity = Res(dev, b.srv[3]),
                   output = Res(dev, b.uav0);
    if (!color.handle || !depth.handle || !output.handle) return Why("an input is missing"), false;
    const resource_desc cd = dev->get_resource_desc(color), dd = dev->get_resource_desc(depth),
                        od = dev->get_resource_desc(output);
    if (!IsHdrColor(cd.texture.format) || !IsHdrColor(od.texture.format) || dd.texture.format != format::r32_g8_typeless)
        return Why("colour, depth or output has an unexpected format"), false;

    // The View buffer: the slot found before, else the first bound one with the expected layout.
    const float* v = nullptr;
    for (int k = -1; k < 5 && !v; ++k) {
        const int slot = k < 0 ? g_viewSlot : k;
        if (slot < 0 || !b.cb[slot].buffer.handle || b.cb[slot].offset) continue;
        auto it = g_cbData.find(b.cb[slot].buffer.handle);
        if (it == g_cbData.end()) continue;
        const float* f = reinterpret_cast<const float*>(it->second.data());
        if (!ViewLayoutOk(f)) continue;
        v = f;
        if (slot != g_viewSlot) Log("DX11: View uniform buffer at b%d", slot);
        g_viewSlot = slot;
    }
    if (!v) return Why("the View buffer wasn't found among b0-b4 (layout check failed)"), false;
    const float size[2] = {v[kViewSize / 4], v[kViewSize / 4 + 1]};
    uint32_t rectMin[2] = {1, 1};
    memcpy(rectMin, v + kViewRectMin / 4, 8);
    if (rectMin[0] || rectMin[1] || size[0] > cd.texture.width || size[1] > cd.texture.height ||
        size[0] > od.texture.width || size[1] > od.texture.height)
        return Why("view rectangle doesn't fit the textures"), false;
    const float jitterClip[2] = {v[kViewJitter / 4], v[kViewJitter / 4 + 1]};
    const float jitter[2] = {jitterClip[0] * size[0] * 0.5f, -jitterClip[1] * size[1] * 0.5f};
    if (std::fabs(jitter[0]) > 0.5f || std::fabs(jitter[1]) > 0.5f) return Why("jitter out of range"), false;
    if (!velocity.handle) return g_resetNext = true, Why("no velocity"), false;
    const resource_desc vd = dev->get_resource_desc(velocity);
    if (!IsVelocityFormat(vd.texture.format) || vd.texture.width < size[0] || vd.texture.height < size[1]) {
        g_resetNext = true;  // camera cut: UE binds a 1x1 placeholder; the game's TAA takes the frame
        ++g_skipped;
        return false;
    }

    auto* d3d = reinterpret_cast<ID3D11Device*>(dev->get_native());
    auto* ctx = reinterpret_cast<ID3D11DeviceContext*>(cl->get_native());
    std::string err;
    if (!dlss11::Init(d3d, err)) {
        g_error = err;
        return Why("DLSS didn't start"), false;
    }
    if (!CreateObjects(d3d)) return Why("our shader objects failed"), false;
    Targets* t = GetTargets(dd.texture.width, dd.texture.height);
    ID3D11ShaderResourceView* srvs[2] = {GameSrv(depth, DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS),
                                         GameSrv(velocity, static_cast<DXGI_FORMAT>(vd.texture.format))};
    if (!t || !srvs[0] || !srvs[1]) return Why("couldn't make our textures or views"), false;

    // motion11.hlsl in the clean state.
    ID3D11DeviceContext1* ctx1 = nullptr;
    if (FAILED(ctx->QueryInterface(__uuidof(ID3D11DeviceContext1), reinterpret_cast<void**>(&ctx1))))
        return Why("no Direct3D 11.1 context"), false;
    D3D11_MAPPED_SUBRESOURCE ms = {};
    if (SUCCEEDED(ctx->Map(g_motionCb, 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) {
        float p[24] = {};
        memcpy(p, v + kViewClipToPrevClip / 4, 64);
        p[16] = size[0], p[17] = size[1], p[18] = 1 / size[0], p[19] = 1 / size[1];
        // MaskMode 0 (no mask), MaskStrength 1, ObjectMode 0 (UE 4's linear velocity)
        p[21] = 1.0f;
        memcpy(ms.pData, p, sizeof(p));
        ctx->Unmap(g_motionCb, 0);
    }
    ID3DDeviceContextState* game = nullptr;
    ctx1->SwapDeviceContextState(g_cleanState, &game);
    ctx1->ClearState();
    ctx->CSSetShader(g_motionCs, nullptr, 0);
    ctx->CSSetConstantBuffers(0, 1, &g_motionCb);
    ctx->CSSetShaderResources(0, 2, srvs);
    ctx->CSSetUnorderedAccessViews(0, 3, t->uav, nullptr);
    ctx->Dispatch((static_cast<UINT>(size[0]) + 7) / 8, (static_cast<UINT>(size[1]) + 7) / 8, 1);
    ctx1->ClearState();
    ctx1->SwapDeviceContextState(game, nullptr);
    if (game) game->Release();
    ctx1->Release();

    const unsigned rw = static_cast<unsigned>(size[0]), rh = static_cast<unsigned>(size[1]);
    const uint64_t sizeKey = (static_cast<uint64_t>(rw) << 48) | (static_cast<uint64_t>(rh) << 32) |
                             (static_cast<uint64_t>(od.texture.width) << 16) | od.texture.height;
    const bool reset = g_resetNext || sizeKey != g_lastSizeKey;
    g_lastSizeKey = sizeKey;

    dlss11::Inputs in;
    in.color = reinterpret_cast<ID3D11Resource*>(color.handle);
    in.depth = t->depth;
    in.motion = t->motion;
    in.output = reinterpret_cast<ID3D11Resource*>(output.handle);
    in.renderWidth = rw, in.renderHeight = rh;
    in.outputWidth = od.texture.width, in.outputHeight = od.texture.height;
    in.jitterX = jitter[0], in.jitterY = jitter[1];
    in.motionScaleX = 1, in.motionScaleY = 1;  // motion11.hlsl writes previous minus current in pixels
    in.reset = reset;
    in.preset = g_set.preset;
    if (!dlss11::Evaluate(ctx, in, err)) {
        if (g_error != err) Log("DX11 DLSS: %s", err.c_str());
        g_error = err;
        return Why("DLSS evaluation failed"), false;
    }
    g_error.clear();
    g_resetNext = false;
    g_lastRender[0] = rw, g_lastRender[1] = rh, g_lastOutput[0] = od.texture.width, g_lastOutput[1] = od.texture.height;
    if (++g_evaluated == 1)
        Log("DX11 DLSS: first frame evaluated (%ux%u -> %ux%u, jitter %.3f %.3f px)", rw, rh, od.texture.width,
            od.texture.height, jitter[0], jitter[1]);
    return true;
}

std::map<uint32_t, bool> g_verdict;  // TAA-shaped shader id -> taken

bool IsTaa(command_list* cl, const Bindings& b) {
    auto s = g_shaders.find(b.cs.handle);
    if (s == g_shaders.end()) return false;
    for (uint32_t id : kTaaIds)
        if (s->second.id == id) return true;
    if (!s->second.taaRegs) return false;
    auto it = g_verdict.find(s->second.id);
    if (it != g_verdict.end()) return it->second;
    // Another TAA permutation: decided by its bindings, as on DX12.
    device* dev = cl->get_device();
    resource r[6];
    for (int i = 0; i < 6; ++i)
        if (!(r[i] = Res(dev, b.srv[i])).handle) return false;
    const resource out = Res(dev, b.uav0);
    if (!out.handle) return false;
    resource_desc d[6];
    for (int i = 0; i < 6; ++i) d[i] = dev->get_resource_desc(r[i]);
    const resource_desc od = dev->get_resource_desc(out);
    const bool match = d[0].texture.width == 1 && d[0].texture.height == 1 && IsHdrColor(d[1].texture.format) &&
                       d[2].texture.format == format::r32_g8_typeless && IsVelocityFormat(d[3].texture.format) &&
                       IsHdrColor(d[5].texture.format) && IsHdrColor(od.texture.format) &&
                       od.texture.width >= d[1].texture.width && od.texture.height >= d[1].texture.height;
    if (g_verdict.size() < 256) g_verdict[s->second.id] = match;
    if (match)
        Log("DX11: TAA permutation cs%08x taken (colour %ux%u -> output %ux%u)", s->second.id, d[1].texture.width,
            d[1].texture.height, od.texture.width, od.texture.height);
    return match;
}

bool OnDispatch(command_list* cl, uint32_t, uint32_t, uint32_t) {
    if (!g_set.enabled) return false;
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_bindings.find(cl->get_native());
    if (it == g_bindings.end()) return false;
    const Bindings b = it->second;
    return IsTaa(cl, b) && OnTaa(cl, b);
}

void OnDestroyDevice(device*) {
    std::lock_guard<std::mutex> lock(g_mutex);
    dlss11::Shutdown();
    ReleaseObjects();
}

// ---------------------------------------------------------------- present

bool g_keyWasDown = false;

void LogStats() {
    static int lines = 0;
    if (g_presents % 240 != 0 || lines >= 20) return;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_set.enabled && !g_why.empty()) {
        std::string s;
        for (const auto& [why, n] : g_why) s += " {" + why + ": " + std::to_string(n) + "}";
        Log("DX11 diag: upscaled %llu, kept %llu:%s", static_cast<unsigned long long>(g_evaluated.load()),
            static_cast<unsigned long long>(g_skipped.load()), s.c_str());
        ++lines;
    }
    g_why.clear();
}

void OnPresent(command_queue*, swapchain*, const rect*, const rect*, uint32_t, const rect*) {
    ++g_presents;
    LogStats();
    rd::ResolutionTick(g_set.enabled, g_set.mode);
    HWND fg = GetForegroundWindow();
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    const bool down = pid == GetCurrentProcessId() && (GetAsyncKeyState(kToggleHotkey) & 0x8000);
    if (down && !g_keyWasDown) {
        g_set.enabled = !g_set.enabled;
        g_resetNext = true;
        SaveSettings();
        Log("DX11 DLSS %s (F9)", g_set.enabled ? "on" : "off");
    }
    g_keyWasDown = down;
}

}  // namespace

namespace upscaler11 {

void Register() {
    LoadSettings();
    Log("DX11 DLSS: %s, mode %s, model %d", g_set.enabled ? "on" : "off", kModeNames[g_set.mode], g_set.preset);
    reshade::register_event<reshade::addon_event::init_pipeline>(&OnInitPipeline);
    reshade::register_event<reshade::addon_event::destroy_pipeline>(&OnDestroyPipeline);
    reshade::register_event<reshade::addon_event::init_resource>(&OnInitResource);
    reshade::register_event<reshade::addon_event::destroy_resource>(&OnDestroyResource);
    reshade::register_event<reshade::addon_event::map_buffer_region>(&OnMapBuffer);
    reshade::register_event<reshade::addon_event::unmap_buffer_region>(&OnUnmapBuffer);
    reshade::register_event<reshade::addon_event::update_buffer_region>(&OnUpdateBuffer);
    reshade::register_event<reshade::addon_event::update_buffer_region_command>(&OnUpdateBufferCommand);
    reshade::register_event<reshade::addon_event::bind_pipeline>(&OnBindPipeline);
    reshade::register_event<reshade::addon_event::push_descriptors>(&OnPushDescriptors);
    reshade::register_event<reshade::addon_event::destroy_command_list>(&OnDestroyCommandList);
    reshade::register_event<reshade::addon_event::dispatch>(&OnDispatch);
    reshade::register_event<reshade::addon_event::destroy_device>(&OnDestroyDevice);
    reshade::register_event<reshade::addon_event::present>(&OnPresent);
}

void DrawOverlay() {
    const float w = rd::ControlWidth();
    if (ImGui::Checkbox("DLSS (F9)", &g_set.enabled)) {
        g_resetNext = true;
        SaveSettings();
        Log("DX11 DLSS %s (overlay)", g_set.enabled ? "on" : "off");
    }
    rd::Help("DLSS in place of the game's TAA. DirectX 11 mode: no FSR, frame generation or Ray Reconstruction "
             "(start the game with -dx12 for those).");
    ImGui::SetNextItemWidth(w);
    if (ImGui::Combo("DLSS mode", &g_set.mode, kModeNames, kModeCount)) {
        SaveSettings();
        Log("DX11 DLSS mode: %s", kModeNames[g_set.mode]);
    }
    rd::Help("Render resolution. \"Game's\" leaves the game's screen percentage.");
    int model = 0;
    for (int i = 0; i < kModelCount; ++i)
        if (kModelIds[i] == g_set.preset) model = i;
    ImGui::SetNextItemWidth(w);
    if (ImGui::Combo("DLSS model", &model, kModelNames, kModelCount)) {
        g_set.preset = kModelIds[model];
        SaveSettings();
    }
    rd::Help("Automatic: K down to Balanced, M at Performance, L at Ultra Performance. The NVIDIA app's DLSS "
             "override, if set, takes precedence.");
    ImGui::Spacing();
    ImGui::TextDisabled("DLSS: %s", dlss11::Status().c_str());
    ImGui::TextDisabled("upscaled %llu frames, TAA kept %llu", static_cast<unsigned long long>(g_evaluated.load()),
                        static_cast<unsigned long long>(g_skipped.load()));
    std::string err, res = rd::ResolutionError();
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        err = g_error;
    }
    if (!err.empty()) ImGui::TextColored(rd::kWarn, "%s", err.c_str());
    if (!res.empty()) ImGui::TextColored(rd::kWarn, "DLSS mode: %s", res.c_str());
}

std::string Headline(bool* warn) {
    *warn = false;
    if (!g_set.enabled) return "DLSS off (DirectX 11)";
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_error.empty()) return *warn = true, "DLSS: " + g_error;
    if (!g_evaluated) return "DLSS on (DirectX 11): waiting for the game's TAA";
    char buf[96];
    snprintf(buf, sizeof(buf), "DLSS on (DirectX 11): %ux%u -> %ux%u", g_lastRender[0], g_lastRender[1], g_lastOutput[0],
             g_lastOutput[1]);
    return buf;
}

}  // namespace upscaler11
