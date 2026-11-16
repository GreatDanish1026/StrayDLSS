// StrayDLSS on Stray's DirectX 11 renderer (the game's default; DX12 needs -dx12): for now only
// the frame capture probe, ported from RE2DLSS-DX11 0.0.1. main.cpp registers this instead of
// the DX12 upscaler and frame generation when the game wasn't started with -dx12.
//
// Frame capture (overlay button or F10) records two frames to StrayDLSS-DX11-frame-N.txt next to
// the game exe:
//   - every compute dispatch and every full-screen draw (up to 4 vertices / 6 indices, one
//     instance), with a stable id of its shaders (FNV-1a of the bytecode), the viewport, render
//     targets, and the textures, UAVs and constant buffers bound in the registers its shader
//     declares;
//   - other draws only as a count between those passes, plus copies, resolves, clears and which
//     device context recorded them (deferred contexts' work appears where the immediate context
//     runs it).
// D3D11 keeps bindings until they're changed, so they're tracked all the time, not only while
// recording. Shaders listed in ReShade.ini [StrayDLSS] DumpShaders=cs1234abcd,ps... also get their
// bytecode saved and their constant buffers dumped (from the CPU's last write to them).
//
// Without a capture, the log names every compute shader declaring exactly t0-t5 and u0 (the shape
// of UE 4's TemporalAA.usf on DX12, cs37876d91 / csed0bf55b there) the first time it's dispatched,
// with its colour input and output, so the DX11 TAA ids show up in StrayDLSS.log.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "common.hpp"
#include "probe11.hpp"
#include "version.h"

#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {

using namespace reshade::api;
using rd::Log;

constexpr int kCaptureHotkey = VK_F10;
constexpr int kCaptureFrames = 2;
#define PROBE_NAME RD_PRODUCT "-DX11"

std::string Format(const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    return buf;
}

// ---------------------------------------------------------------- crash catcher
//
// Logs the first access violations with this add-on on the stack (module + offset and a stack).

HMODULE g_self = nullptr;
void* g_veh = nullptr;
std::atomic<int> g_faults{0};

std::string ModuleOffset(const void* addr) {
    HMODULE m = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            static_cast<LPCWSTR>(addr), &m) || !m)
        return Format("%p", addr);
    char name[MAX_PATH] = {};
    GetModuleFileNameA(m, name, MAX_PATH);
    const char* base = strrchr(name, '\\');
    return Format("%s+0x%llx", base ? base + 1 : name,
                  static_cast<unsigned long long>(reinterpret_cast<const uint8_t*>(addr) - reinterpret_cast<const uint8_t*>(m)));
}

LONG CALLBACK OnException(EXCEPTION_POINTERS* e) {
    const DWORD code = e->ExceptionRecord->ExceptionCode;
    if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_ILLEGAL_INSTRUCTION && code != EXCEPTION_STACK_OVERFLOW &&
        code != 0xC0000409)
        return EXCEPTION_CONTINUE_SEARCH;
    void* frames[32];
    const USHORT count = RtlCaptureStackBackTrace(0, 32, frames, nullptr);
    bool ours = false;
    for (USHORT i = 1; i < count && !ours; ++i) {
        HMODULE m = nullptr;
        ours = GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                  static_cast<LPCWSTR>(frames[i]), &m) && m == g_self;
    }
    if (!ours || ++g_faults > 3) return EXCEPTION_CONTINUE_SEARCH;
    Log("EXCEPTION 0x%08lx at %s (thread %lu)", code, ModuleOffset(e->ExceptionRecord->ExceptionAddress).c_str(),
        GetCurrentThreadId());
    for (USHORT i = 0; i < count; ++i) Log("  stack %2u: %s", i, ModuleOffset(frames[i]).c_str());
    return EXCEPTION_CONTINUE_SEARCH;
}

// ---------------------------------------------------------------- shader ids
//
// D3D11 shaders are separate objects (ReShade reports each as a pipeline with one shader); a hash
// of the bytecode names them the same way every run.

struct Decl {
    char cls;  // 't', 'u', 'b' or 's'
    uint32_t lo, hi;
};

struct ShaderInfo {
    char kind = 0;  // 'v', 'p', 'c' or 0
    uint32_t id = 0;
    bool taaShape = false;  // compute: declares exactly t0-t5 and u0 (plus samplers and cbuffers)
};

std::mutex g_pipeMutex;
std::unordered_map<uint64_t, ShaderInfo> g_shaders;       // shader object -> id
std::unordered_map<uint32_t, std::vector<Decl>> g_decls;  // id -> registers it declares
size_t g_counts[3] = {};                                   // vertex, pixel, compute shaders seen
std::unordered_set<uint32_t> g_dumpPs, g_dumpCs, g_dumpVs; // ReShade.ini DumpShaders

uint32_t Fnv(const void* data, size_t n) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; ++i) h = (h ^ p[i]) * 16777619u;
    return h;
}

// The registers an SM 5.0 shader declares (DXBC RDEF part).
std::vector<Decl> ParseDecls(const uint8_t* p, size_t n) {
    std::vector<Decl> out;
    auto u32 = [&](size_t off) -> uint32_t {
        uint32_t v = 0;
        if (off + 4 <= n) memcpy(&v, p + off, 4);
        return v;
    };
    if (n < 32 || memcmp(p, "DXBC", 4) != 0) return out;
    const uint32_t parts = u32(28);
    for (uint32_t i = 0; i < parts && 32 + i * 4 + 4 <= n; ++i) {
        const size_t part = u32(32 + i * 4);
        if (part + 8 > n || memcmp(p + part, "RDEF", 4) != 0) continue;
        const size_t data = part + 8, size = u32(part + 4);
        if (data + size > n) continue;
        const uint32_t count = u32(data + 8), off = u32(data + 12);
        const uint32_t version = u32(data + 16);
        const bool sm51 = ((version >> 8) & 0xff) == 5 && (version & 0xff) == 1;
        const uint32_t stride = sm51 ? 40 : 32;
        for (uint32_t k = 0; k < count; ++k) {
            const size_t at = data + off + k * stride;
            if (at + stride > data + size) break;
            const uint32_t type = u32(at + 4);  // D3D_SHADER_INPUT_TYPE
            const char cls = type == 0 ? 'b' : type == 3 ? 's' : (type == 4 || type >= 6) && type != 7 ? 'u' : 't';
            const uint32_t lo = u32(at + 20), cnt = u32(at + 24);
            out.push_back({cls, lo, cnt ? lo + cnt - 1 : lo});
        }
    }
    return out;
}

// t0-t5 and u0, nothing else in t/u: UE 4's TemporalAA.usf compute permutations on DX12.
bool TaaShape(const std::vector<Decl>& decls) {
    uint64_t t = 0, u = 0;
    for (const Decl& d : decls) {
        if (d.cls != 't' && d.cls != 'u') continue;
        for (uint32_t r = d.lo; r <= d.hi; ++r) {
            if (r >= 64) return false;
            (d.cls == 't' ? t : u) |= 1ull << r;
        }
    }
    return t == 0x3f && u == 1;
}

void SaveBytecode(char kind, uint32_t id, const void* code, size_t size) {
    static std::mutex m;
    static std::unordered_set<uint64_t> saved;
    std::lock_guard<std::mutex> lock(m);
    if (!saved.insert((static_cast<uint64_t>(kind) << 32) | id).second) return;
    wchar_t name[96];
    swprintf(name, 96, L"" PROBE_NAME "-%cs%08x.cso", kind, id);
    if (FILE* f = _wfopen((rd::ExeDir() + name).c_str(), L"wb")) {
        fwrite(code, 1, size, f);
        fclose(f);
        Log("saved %cs%08x bytecode (%zu bytes)", kind, id, size);
    }
}

void OnInitPipeline(device*, pipeline_layout, uint32_t count, const pipeline_subobject* subobjects, pipeline pipe) {
    for (uint32_t i = 0; i < count; ++i) {
        const pipeline_subobject& s = subobjects[i];
        char kind = 0;
        int slot = 0;
        if (s.type == pipeline_subobject_type::vertex_shader) kind = 'v', slot = 0;
        else if (s.type == pipeline_subobject_type::pixel_shader) kind = 'p', slot = 1;
        else if (s.type == pipeline_subobject_type::compute_shader) kind = 'c', slot = 2;
        if (!kind || !s.data) continue;
        const shader_desc& d = *static_cast<const shader_desc*>(s.data);
        if (!d.code || !d.code_size) continue;
        const uint32_t id = Fnv(d.code, d.code_size);
        if ((kind == 'p' && g_dumpPs.count(id)) || (kind == 'c' && g_dumpCs.count(id)) || (kind == 'v' && g_dumpVs.count(id)))
            SaveBytecode(kind, id, d.code, d.code_size);
        std::vector<Decl> decls = ParseDecls(static_cast<const uint8_t*>(d.code), d.code_size);
        const bool taa = kind == 'c' && TaaShape(decls);
        std::lock_guard<std::mutex> lock(g_pipeMutex);
        g_decls[id] = std::move(decls);
        g_shaders[pipe.handle] = {kind, id, taa};
        ++g_counts[slot];
        return;  // D3D11: one shader per object
    }
}

void OnDestroyPipeline(device*, pipeline pipe) {
    std::lock_guard<std::mutex> lock(g_pipeMutex);
    g_shaders.erase(pipe.handle);
}

ShaderInfo Shader(pipeline pipe) {
    std::lock_guard<std::mutex> lock(g_pipeMutex);
    auto it = g_shaders.find(pipe.handle);
    return it == g_shaders.end() ? ShaderInfo{} : it->second;
}

std::vector<Decl> Decls(uint32_t id, bool* known) {
    std::lock_guard<std::mutex> lock(g_pipeMutex);
    auto it = g_decls.find(id);
    *known = it != g_decls.end() && !it->second.empty();
    return *known ? it->second : std::vector<Decl>{};
}

void LoadDumpList() {
    char buf[2048] = {};
    size_t size = sizeof(buf);
    if (!reshade::get_config_value(nullptr, RD_PRODUCT, "DumpShaders", buf, &size)) return;
    std::string s(buf, size && size <= sizeof(buf) ? size : strlen(buf));
    for (char& c : s)
        if (c == ' ' || c == ';' || c == '\0') c = ',';
    size_t pos = 0;
    while (pos < s.size()) {
        size_t end = s.find(',', pos);
        if (end == std::string::npos) end = s.size();
        std::string t = s.substr(pos, end - pos);
        pos = end + 1;
        if (t.size() != 10) continue;
        const uint32_t id = static_cast<uint32_t>(strtoul(t.c_str() + 2, nullptr, 16));
        if (t.compare(0, 2, "ps") == 0) g_dumpPs.insert(id);
        else if (t.compare(0, 2, "cs") == 0) g_dumpCs.insert(id);
        else if (t.compare(0, 2, "vs") == 0) g_dumpVs.insert(id);
    }
    Log("dumping %zu pixel, %zu compute and %zu vertex shaders", g_dumpPs.size(), g_dumpCs.size(), g_dumpVs.size());
}

// ---------------------------------------------------------------- resources

const char* FormatName(format f) {
    switch (static_cast<uint32_t>(f)) {
        case 2: return "rgba32f";
        case 10: return "rgba16f";
        case 11: return "rgba16unorm";
        case 13: return "rgba16snorm";
        case 16: return "rg32f";
        case 19: return "r32g8x24_typeless";
        case 20: return "d32s8";
        case 21: return "r32f_x8x24";
        case 23: return "rgb10a2_typeless";
        case 24: return "rgb10a2unorm";
        case 26: return "r11g11b10f";
        case 27: return "rgba8_typeless";
        case 28: return "rgba8unorm";
        case 29: return "rgba8srgb";
        case 33: return "rg16_typeless";
        case 34: return "rg16f";
        case 35: return "rg16unorm";
        case 36: return "rg16uint";
        case 37: return "rg16snorm";
        case 39: return "r32_typeless";
        case 40: return "d32f";
        case 41: return "r32f";
        case 42: return "r32uint";
        case 44: return "r24g8_typeless";
        case 45: return "d24s8";
        case 46: return "r24x8";
        case 47: return "x24g8uint";
        case 49: return "rg8unorm";
        case 50: return "rg8uint";
        case 51: return "rg8snorm";
        case 53: return "r16_typeless";
        case 54: return "r16f";
        case 55: return "d16";
        case 56: return "r16unorm";
        case 57: return "r16uint";
        case 61: return "r8unorm";
        case 62: return "r8uint";
        case 87: return "bgra8unorm";
        case 91: return "bgra8srgb";
        default: return nullptr;
    }
}

struct ViewInfo {
    resource res;
    resource_view_desc desc;
};

std::mutex g_resMutex;
std::unordered_map<uint64_t, resource_desc> g_resources;
std::unordered_map<uint64_t, ViewInfo> g_views;

// Constant buffers' contents as the CPU last wrote them (Map/Unmap, UpdateSubresource), for
// dumping the constants of shaders in the dump list. Buffers of up to 8 KB (UE's View buffer).
constexpr uint64_t kCbTrackMax = 8192;
std::unordered_map<uint64_t, std::vector<uint8_t>> g_cbData;
std::unordered_map<uint64_t, void*> g_mapped;

void OnInitResource(device*, const resource_desc& desc, const subresource_data* initial, resource_usage, resource r) {
    std::lock_guard<std::mutex> lock(g_resMutex);
    g_resources[r.handle] = desc;
    if (desc.type == resource_type::buffer && (desc.usage & resource_usage::constant_buffer) != 0 &&
        desc.buffer.size <= kCbTrackMax) {
        std::vector<uint8_t>& v = g_cbData[r.handle];
        v.assign(static_cast<size_t>(desc.buffer.size), 0);
        if (initial && initial->data) memcpy(v.data(), initial->data, v.size());
    }
}

void OnDestroyResource(device*, resource r) {
    std::lock_guard<std::mutex> lock(g_resMutex);
    g_resources.erase(r.handle);
    g_cbData.erase(r.handle);
    g_mapped.erase(r.handle);
}

void OnInitView(device*, resource r, resource_usage, const resource_view_desc& desc, resource_view v) {
    std::lock_guard<std::mutex> lock(g_resMutex);
    g_views[v.handle] = {r, desc};
}

void OnDestroyView(device*, resource_view v) {
    std::lock_guard<std::mutex> lock(g_resMutex);
    g_views.erase(v.handle);
}

void OnMapBuffer(device*, resource r, uint64_t offset, uint64_t, map_access access, void** data) {
    if (access == map_access::read_only || !data || !*data || offset) return;
    std::lock_guard<std::mutex> lock(g_resMutex);
    if (g_cbData.count(r.handle)) g_mapped[r.handle] = *data;
}

void OnUnmapBuffer(device*, resource r) {
    std::lock_guard<std::mutex> lock(g_resMutex);
    auto m = g_mapped.find(r.handle);
    if (m == g_mapped.end()) return;
    std::vector<uint8_t>& v = g_cbData[r.handle];
    memcpy(v.data(), m->second, v.size());
    g_mapped.erase(m);
}

void CopyCb(resource r, const void* data, uint64_t offset, uint64_t size) {
    if (!data) return;
    std::lock_guard<std::mutex> lock(g_resMutex);
    auto it = g_cbData.find(r.handle);
    if (it == g_cbData.end() || offset >= it->second.size()) return;
    if (size == UINT64_MAX || offset + size > it->second.size()) size = it->second.size() - offset;
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

bool FindView(resource_view v, ViewInfo* out) {
    std::lock_guard<std::mutex> lock(g_resMutex);
    auto it = g_views.find(v.handle);
    if (it == g_views.end()) return false;
    *out = it->second;
    return true;
}

std::string Describe(device* dev, resource r) {
    if (!r.handle) return "null";
    std::string id = Format("#%05llx", (r.handle >> 4) & 0xfffff);
    resource_desc d;
    {
        std::lock_guard<std::mutex> lock(g_resMutex);
        auto it = g_resources.find(r.handle);
        d = it != g_resources.end() ? it->second : dev->get_resource_desc(r);
    }
    if (d.type == resource_type::buffer) return id + Format(" buf %llu", d.buffer.size);
    const char* fn = FormatName(d.texture.format);
    std::string fmt = fn ? fn : Format("fmt%u", static_cast<uint32_t>(d.texture.format));
    std::string s = id + Format(" tex %ux%u %s", d.texture.width, d.texture.height, fmt.c_str());
    if (d.texture.depth_or_layers > 1) s += Format(" x%u", d.texture.depth_or_layers);
    if (d.texture.levels > 1) s += Format(" mips%u", d.texture.levels);
    return s;
}

std::string DescribeView(device* dev, resource_view v) {
    if (!v.handle) return "null";
    ViewInfo info;
    if (!FindView(v, &info)) return Format("view %llx (unknown)", v.handle);
    std::string s = Describe(dev, info.res);
    if (const char* fn = FormatName(info.desc.format)) s += Format(" as %s", fn);
    if (info.desc.type == resource_view_type::texture_2d && info.desc.texture.first_level)
        s += Format(" mip%u", info.desc.texture.first_level);
    return s;
}

// ---------------------------------------------------------------- binding state (always tracked)

struct Stage {
    ShaderInfo shader;
    std::map<uint32_t, resource_view> srv, uav;
    std::map<uint32_t, buffer_range> cb;
};

struct Context {
    Stage vs, ps, cs;
    viewport vp = {};
    std::vector<resource_view> rtvs;
    resource_view dsv = {0};
    // capture
    std::vector<std::string> lines;
    uint32_t draws = 0;
    uint64_t id = 0;
};

std::mutex g_ctxMutex;
std::unordered_map<uint64_t, Context> g_contexts;                    // device context -> state
std::unordered_map<uint64_t, std::vector<std::string>> g_finished;  // ID3D11CommandList -> its lines
uint64_t g_nextCtx = 1;
uint64_t g_immediate = 0;  // the immediate context (the one that presents)
std::atomic<uint64_t> g_executes{0}, g_deferredSeen{0};

Context& Ctx(command_list* cl) {  // g_ctxMutex held
    Context& c = g_contexts[cl->get_native()];
    if (!c.id) c.id = g_nextCtx++;
    return c;
}

Stage* StageOf(Context& c, shader_stage s) {
    if ((s & shader_stage::compute) != 0) return &c.cs;
    if ((s & shader_stage::pixel) != 0) return &c.ps;
    if ((s & shader_stage::vertex) != 0) return &c.vs;
    return nullptr;
}

void OnInitCommandList(command_list* cl) {
    std::lock_guard<std::mutex> lock(g_ctxMutex);
    Ctx(cl);
}

void OnDestroyCommandList(command_list* cl) {
    std::lock_guard<std::mutex> lock(g_ctxMutex);
    g_contexts.erase(cl->get_native());
    g_finished.erase(cl->get_native());
}

void OnBindPipeline(command_list* cl, pipeline_stage stages, pipeline pipe) {
    const ShaderInfo s = pipe.handle ? Shader(pipe) : ShaderInfo{};
    std::lock_guard<std::mutex> lock(g_ctxMutex);
    Context& c = Ctx(cl);
    if ((stages & pipeline_stage::compute_shader) != 0) c.cs.shader = s;
    if ((stages & pipeline_stage::pixel_shader) != 0) c.ps.shader = s;
    if ((stages & pipeline_stage::vertex_shader) != 0) c.vs.shader = s;
}

void OnPushDescriptors(command_list* cl, shader_stage stages, pipeline_layout, uint32_t, const descriptor_table_update& u) {
    std::lock_guard<std::mutex> lock(g_ctxMutex);
    Context& c = Ctx(cl);
    Stage* st = StageOf(c, stages);
    if (!st) return;
    for (uint32_t i = 0; i < u.count; ++i) {
        const uint32_t slot = u.binding + i;
        switch (u.type) {
            case descriptor_type::constant_buffer: st->cb[slot] = static_cast<const buffer_range*>(u.descriptors)[i]; break;
            case descriptor_type::shader_resource_view:  // = texture_shader_resource_view
            case descriptor_type::buffer_shader_resource_view:
                st->srv[slot] = static_cast<const resource_view*>(u.descriptors)[i];
                break;
            case descriptor_type::unordered_access_view:  // = texture_unordered_access_view
            case descriptor_type::buffer_unordered_access_view:
                st->uav[slot] = static_cast<const resource_view*>(u.descriptors)[i];
                break;
            default: break;
        }
    }
}

void OnBindViewports(command_list* cl, uint32_t first, uint32_t count, const viewport* vps) {
    if (first != 0 || !count) return;
    std::lock_guard<std::mutex> lock(g_ctxMutex);
    Ctx(cl).vp = vps[0];
}

// ---------------------------------------------------------------- capture

enum class Capture { Idle, Recording };
std::atomic<Capture> g_capture{Capture::Idle};
int g_framesLeft = 0;
int g_captureCount = 0;
std::string g_status;
std::vector<std::string> g_frame;  // g_ctxMutex
size_t g_passes = 0;

bool Recording() { return g_capture.load(std::memory_order_relaxed) == Capture::Recording; }

void FlushDraws(Context& c) {
    if (!c.draws) return;
    c.lines.push_back(Format("  draw x%u", c.draws));
    c.draws = 0;
}

void AddLocked(Context& c, std::string line) {
    FlushDraws(c);
    c.lines.push_back("  " + std::move(line));
}

void OnBindRenderTargets(command_list* cl, uint32_t count, const resource_view* rtvs, resource_view dsv) {
    std::lock_guard<std::mutex> lock(g_ctxMutex);
    Context& c = Ctx(cl);
    c.rtvs.assign(rtvs, rtvs + count);
    c.dsv = dsv;
    if (!Recording()) return;
    device* dev = cl->get_device();
    std::string s;
    for (uint32_t i = 0; i < count; ++i) s += " " + DescribeView(dev, rtvs[i]);
    if (dsv.handle) s += " depth " + DescribeView(dev, dsv);
    AddLocked(c, "render targets:" + s);
}

std::string Floats(const uint32_t* v, size_t n) {
    std::string s;
    for (size_t k = 0; k < n; ++k) {
        float f;
        memcpy(&f, &v[k], 4);
        const uint32_t e = (v[k] >> 23) & 0xff;
        if (v[k] == 0) s += "          0";
        else if (e > 90 && e < 165) s += Format(" %10.6g", f);
        else s += Format("  0x%08x", v[k]);
    }
    return s;
}

void DumpCb(Context& c, uint32_t slot, const buffer_range& b) {
    std::vector<uint8_t> data;
    {
        std::lock_guard<std::mutex> lock(g_resMutex);
        auto it = g_cbData.find(b.buffer.handle);
        if (it == g_cbData.end()) {
            AddLocked(c, Format("    b%u: contents not tracked (larger than 8 KB?)", slot));
            return;
        }
        data = it->second;
    }
    const size_t off = static_cast<size_t>(b.offset);
    if (off >= data.size()) return;
    std::vector<uint32_t> v((data.size() - off) / 4);
    memcpy(v.data(), data.data() + off, v.size() * 4);
    AddLocked(c, Format("    b%u: %zu bytes (CPU's last write)", slot, v.size() * 4));
    for (size_t i = 0; i < v.size(); i += 4)
        AddLocked(c, Format("      c %4zu:", i * 4) + Floats(&v[i], v.size() - i < 4 ? v.size() - i : 4));
}

void WriteStage(device* dev, Context& c, const Stage& st, bool dump) {
    bool known = false;
    const std::vector<Decl> decls = Decls(st.shader.id, &known);
    auto declared = [&](char cls, uint32_t reg) {
        if (!known) return reg < 16;
        for (const Decl& d : decls)
            if (d.cls == cls && reg >= d.lo && reg <= d.hi) return true;
        return false;
    };
    if (!known) AddLocked(c, "    (shader declarations unknown: first 16 slots shown)");
    for (const auto& [slot, v] : st.srv)
        if (v.handle && declared('t', slot)) AddLocked(c, Format("    t%u = %s", slot, DescribeView(dev, v).c_str()));
    for (const auto& [slot, v] : st.uav)
        if (v.handle && declared('u', slot)) AddLocked(c, Format("    u%u = %s", slot, DescribeView(dev, v).c_str()));
    for (const auto& [slot, b] : st.cb) {
        if (!b.buffer.handle || !declared('b', slot)) continue;
        AddLocked(c, Format("    b%u = cb %s +%llu", slot, Describe(dev, b.buffer).c_str(), b.offset));
        if (dump) DumpCb(c, slot, b);
    }
}

void Pass(command_list* cl, bool compute, const std::string& head) {
    device* dev = cl->get_device();
    std::lock_guard<std::mutex> lock(g_ctxMutex);
    Context& c = Ctx(cl);
    ++g_passes;
    if (compute) {
        AddLocked(c, Format("DISPATCH cs%08x%s ", c.cs.shader.id, c.cs.shader.taaShape ? " (TAA shape)" : "") + head);
        WriteStage(dev, c, c.cs, g_dumpCs.count(c.cs.shader.id) != 0);
        return;
    }
    std::string rts;
    for (resource_view v : c.rtvs) rts += " " + DescribeView(dev, v);
    if (c.dsv.handle) rts += " depth " + DescribeView(dev, c.dsv);
    AddLocked(c, Format("FULLSCREEN ps%08x vs%08x ", c.ps.shader.id, c.vs.shader.id) + head +
                     Format(" viewport %gx%g at %g,%g ->%s", c.vp.width, c.vp.height, c.vp.x, c.vp.y, rts.c_str()));
    WriteStage(dev, c, c.ps, g_dumpPs.count(c.ps.shader.id) != 0);
    if (!c.ps.uav.empty()) {
        for (const auto& [slot, v] : c.ps.uav)
            if (v.handle) AddLocked(c, Format("    ps u%u = %s", slot, DescribeView(dev, v).c_str()));
    }
}

// Every TAA-shaped compute shader, logged the first time it runs (outside captures too).
std::unordered_set<uint32_t> g_taaLogged;  // g_ctxMutex

void NoteTaaShape(command_list* cl, uint32_t x, uint32_t y) {
    device* dev = cl->get_device();
    std::string line;
    {
        std::lock_guard<std::mutex> lock(g_ctxMutex);
        Context& c = Ctx(cl);
        if (!c.cs.shader.taaShape || !g_taaLogged.insert(c.cs.shader.id).second) return;
        auto srv = [&](uint32_t s) {
            auto it = c.cs.srv.find(s);
            return it == c.cs.srv.end() ? std::string("unbound") : DescribeView(dev, it->second);
        };
        auto uav = c.cs.uav.find(0);
        line = Format("TAA-shaped compute shader cs%08x dispatched %u x %u: t1 %s, t3 %s, u0 %s", c.cs.shader.id, x, y,
                      srv(1).c_str(), srv(3).c_str(),
                      uav == c.cs.uav.end() ? "unbound" : DescribeView(dev, uav->second).c_str());
    }
    Log("%s", line.c_str());
}

bool OnDispatch(command_list* cl, uint32_t x, uint32_t y, uint32_t z) {
    NoteTaaShape(cl, x, y);
    if (Recording()) Pass(cl, true, Format("%u x %u x %u", x, y, z));
    return false;
}

void Other(command_list* cl) {
    std::lock_guard<std::mutex> lock(g_ctxMutex);
    ++Ctx(cl).draws;
}

bool OnDraw(command_list* cl, uint32_t vertices, uint32_t instances, uint32_t, uint32_t) {
    if (!Recording()) return false;
    if (vertices <= 4 && instances == 1) Pass(cl, false, Format("draw %u verts", vertices));
    else Other(cl);
    return false;
}

bool OnDrawIndexed(command_list* cl, uint32_t indices, uint32_t instances, uint32_t, int32_t, uint32_t) {
    if (!Recording()) return false;
    if (indices <= 6 && instances == 1) Pass(cl, false, Format("draw %u indices", indices));
    else Other(cl);
    return false;
}

bool OnIndirect(command_list* cl, indirect_command type, resource, uint64_t, uint32_t count, uint32_t) {
    if (!Recording()) return false;
    if (type == indirect_command::dispatch) Pass(cl, true, Format("indirect x%u", count));
    else Other(cl);
    return false;
}

void Note(command_list* cl, std::string s) {
    std::lock_guard<std::mutex> lock(g_ctxMutex);
    AddLocked(Ctx(cl), std::move(s));
}

bool OnCopyResource(command_list* cl, resource src, resource dst) {
    if (Recording()) Note(cl, Format("copy %s -> %s", Describe(cl->get_device(), src).c_str(), Describe(cl->get_device(), dst).c_str()));
    return false;
}

bool OnCopyTexture(command_list* cl, resource src, uint32_t, const subresource_box*, resource dst, uint32_t,
                   const subresource_box*, filter_mode) {
    if (Recording())
        Note(cl, Format("copy region %s -> %s", Describe(cl->get_device(), src).c_str(), Describe(cl->get_device(), dst).c_str()));
    return false;
}

bool OnResolve(command_list* cl, resource src, uint32_t, const subresource_box*, resource dst, uint32_t, uint32_t,
               uint32_t, uint32_t, format) {
    if (Recording())
        Note(cl, Format("resolve %s -> %s", Describe(cl->get_device(), src).c_str(), Describe(cl->get_device(), dst).c_str()));
    return false;
}

bool OnClearRtv(command_list* cl, resource_view rtv, const float color[4], uint32_t, const rect*) {
    if (Recording())
        Note(cl, Format("clear %s to %g %g %g %g", DescribeView(cl->get_device(), rtv).c_str(), color[0], color[1], color[2], color[3]));
    return false;
}

// D3D11: FinishCommandList (secondary = the new command list, cl = the deferred context) and
// ExecuteCommandList (cl = the context running it). The deferred context's lines move with the
// command list into the stream of the context that runs it.
void OnSecondary(command_list* cl, command_list* secondary) {
    std::lock_guard<std::mutex> lock(g_ctxMutex);
    const uint64_t key = secondary->get_native();
    auto f = g_finished.find(key);
    if (f == g_finished.end()) {  // FinishCommandList
        Context& c = Ctx(cl);
        ++g_deferredSeen;
        if (!Recording()) return;
        FlushDraws(c);
        std::vector<std::string>& out = g_finished[key];
        out.push_back(Format("  -- command list from deferred context C%llu, %zu events --", c.id, c.lines.size()));
        for (std::string& s : c.lines) out.push_back("  " + std::move(s));
        c.lines.clear();
        return;
    }
    ++g_executes;  // ExecuteCommandList
    Context& c = Ctx(cl);
    if (Recording()) {
        FlushDraws(c);
        for (std::string& s : f->second) c.lines.push_back(std::move(s));
    }
    g_finished.erase(f);
}

void WriteCapture() {
    std::vector<std::string> frame;
    size_t passes;
    {
        std::lock_guard<std::mutex> lock(g_ctxMutex);
        frame.swap(g_frame);
        for (auto& [k, c] : g_contexts) c.lines.clear(), c.draws = 0;
        g_finished.clear();
        passes = g_passes;
    }
    ++g_captureCount;
    std::wstring name = rd::ExeDir() + L"" PROBE_NAME "-frame-" + std::to_wstring(g_captureCount) + L".txt";
    FILE* f = _wfopen(name.c_str(), L"w");
    if (!f) {
        g_status = "couldn't write the capture file";
        return;
    }
    for (const std::string& s : frame) {
        fputs(s.c_str(), f);
        fputc('\n', f);
    }
    fclose(f);
    g_status = Format("Capture %d: %zu lines, %zu passes -> " PROBE_NAME "-frame-%d.txt", g_captureCount, frame.size(),
                      passes, g_captureCount);
    Log("%s", g_status.c_str());
}

void StartCapture() {
    if (g_capture.load() != Capture::Idle) return;
    {
        std::lock_guard<std::mutex> lock(g_ctxMutex);
        for (auto& [k, c] : g_contexts) c.lines.clear(), c.draws = 0;
        g_finished.clear();
        g_frame.clear();
        g_passes = 0;
        g_frame.push_back("==== capture start (deferred work recorded before this point is missing)");
    }
    g_framesLeft = kCaptureFrames;
    g_capture = Capture::Recording;
    Log("capture started");
}

bool g_keyWasDown = false;
std::atomic<uint64_t> g_presents{0};
bool g_wrongApi = false;

void OnPresent(command_queue* q, swapchain* sc, const rect*, const rect*, uint32_t, const rect*) {
    ++g_presents;
    if (!g_immediate) {
        g_immediate = q->get_native();
        device* dev = sc->get_device();
        const resource_desc d = dev->get_resource_desc(sc->get_current_back_buffer());
        const char* fn = FormatName(d.texture.format);
        g_wrongApi = dev->get_api() != device_api::d3d11;
        Log("first present: API %s, back buffer %ux%u %s", g_wrongApi ? "NOT D3D11" : "D3D11", d.texture.width,
            d.texture.height, fn ? fn : Format("fmt%u", static_cast<uint32_t>(d.texture.format)).c_str());
        if (g_wrongApi)
            Log("the game runs DirectX 12 without -dx12 on its command line: StrayDLSS's DX12 code is off this run; "
                "add -dx12 to the launch options");
    }
    if (g_capture.load() == Capture::Recording) {
        {
            std::lock_guard<std::mutex> lock(g_ctxMutex);
            Context& c = g_contexts[q->get_native()];  // D3D11: the queue is the immediate context
            if (!c.id) c.id = g_nextCtx++;
            FlushDraws(c);
            g_frame.push_back(Format("---- immediate context C%llu, %zu events", c.id, c.lines.size()));
            for (std::string& s : c.lines) g_frame.push_back(std::move(s));
            c.lines.clear();
            for (auto& [k, other] : g_contexts) {  // deferred work never executed (shouldn't happen)
                if (k == q->get_native() || other.lines.empty()) continue;
                g_frame.push_back(Format("---- deferred context C%llu, %zu events not yet in a command list", other.id,
                                         other.lines.size()));
            }
            g_frame.push_back(Format("==== PRESENT (back buffer %s), %d more",
                                     Describe(sc->get_device(), sc->get_current_back_buffer()).c_str(), g_framesLeft - 1));
        }
        if (--g_framesLeft <= 0) {
            WriteCapture();
            g_capture = Capture::Idle;
        }
    }
    HWND fg = GetForegroundWindow();
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    const bool down = pid == GetCurrentProcessId() && (GetAsyncKeyState(kCaptureHotkey) & 0x8000);
    if (down && !g_keyWasDown) StartCapture();
    g_keyWasDown = down;
}

}  // namespace

namespace probe11 {

void Register(HMODULE module) {
    g_self = module;
    g_veh = AddVectoredExceptionHandler(1, &OnException);
    Log("DirectX 11 mode (no -dx12 on the command line): frame capture probe only, no upscaling yet");
    LoadDumpList();
    reshade::register_event<reshade::addon_event::init_resource>(&OnInitResource);
    reshade::register_event<reshade::addon_event::destroy_resource>(&OnDestroyResource);
    reshade::register_event<reshade::addon_event::init_resource_view>(&OnInitView);
    reshade::register_event<reshade::addon_event::destroy_resource_view>(&OnDestroyView);
    reshade::register_event<reshade::addon_event::map_buffer_region>(&OnMapBuffer);
    reshade::register_event<reshade::addon_event::unmap_buffer_region>(&OnUnmapBuffer);
    reshade::register_event<reshade::addon_event::update_buffer_region>(&OnUpdateBuffer);
    reshade::register_event<reshade::addon_event::update_buffer_region_command>(&OnUpdateBufferCommand);
    reshade::register_event<reshade::addon_event::init_pipeline>(&OnInitPipeline);
    reshade::register_event<reshade::addon_event::destroy_pipeline>(&OnDestroyPipeline);
    reshade::register_event<reshade::addon_event::init_command_list>(&OnInitCommandList);
    reshade::register_event<reshade::addon_event::destroy_command_list>(&OnDestroyCommandList);
    reshade::register_event<reshade::addon_event::execute_secondary_command_list>(&OnSecondary);
    reshade::register_event<reshade::addon_event::bind_pipeline>(&OnBindPipeline);
    reshade::register_event<reshade::addon_event::push_descriptors>(&OnPushDescriptors);
    reshade::register_event<reshade::addon_event::bind_viewports>(&OnBindViewports);
    reshade::register_event<reshade::addon_event::bind_render_targets_and_depth_stencil>(&OnBindRenderTargets);
    reshade::register_event<reshade::addon_event::dispatch>(&OnDispatch);
    reshade::register_event<reshade::addon_event::draw>(&OnDraw);
    reshade::register_event<reshade::addon_event::draw_indexed>(&OnDrawIndexed);
    reshade::register_event<reshade::addon_event::draw_or_dispatch_indirect>(&OnIndirect);
    reshade::register_event<reshade::addon_event::copy_resource>(&OnCopyResource);
    reshade::register_event<reshade::addon_event::copy_texture_region>(&OnCopyTexture);
    reshade::register_event<reshade::addon_event::resolve_texture_region>(&OnResolve);
    reshade::register_event<reshade::addon_event::clear_render_target_view>(&OnClearRtv);
    reshade::register_event<reshade::addon_event::present>(&OnPresent);
}

void Unregister() {
    if (g_veh) RemoveVectoredExceptionHandler(g_veh);
    g_veh = nullptr;
}

void DrawOverlay() {
    ImGui::TextUnformatted("DirectX 11: probe build (no upscaling yet)");
    if (g_wrongApi) ImGui::TextColored(rd::kWarn, "The game is on DirectX 12 but wasn't started with -dx12: add -dx12.");
    ImGui::BeginDisabled(g_capture.load() != Capture::Idle);
    if (ImGui::Button("Capture frame (F10)")) StartCapture();
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextDisabled("%s", g_capture.load() == Capture::Recording ? "recording..." : g_status.c_str());
    {
        std::lock_guard<std::mutex> lock(g_pipeMutex);
        ImGui::TextDisabled("shaders: %zu vertex, %zu pixel, %zu compute", g_counts[0], g_counts[1], g_counts[2]);
    }
    size_t contexts, taa;
    {
        std::lock_guard<std::mutex> lock(g_ctxMutex);
        contexts = g_contexts.size();
        taa = g_taaLogged.size();
    }
    ImGui::TextDisabled("device contexts: %zu; deferred command lists finished %llu, executed %llu", contexts,
                        static_cast<unsigned long long>(g_deferredSeen.load()), static_cast<unsigned long long>(g_executes.load()));
    ImGui::TextDisabled("TAA-shaped compute shaders seen: %zu (StrayDLSS.log)", taa);
}

}  // namespace probe11
