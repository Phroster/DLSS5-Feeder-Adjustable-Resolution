// dlss5-feed - ReShade add-on
//
// Makes DLSS 5 neural rendering work in a D3D11 game that has no DLSS of its own.
//
// The DLSS 5 add-on (renodx-dlss5) only detours NVSDK_NGX_D3D12_CreateFeature /
// EvaluateFeature and reads the DLSS "contract" it finds there (Color, Depth,
// MotionVectors, Output, sizes, jitter, reset...). Nothing in a DLSS-less game ever
// issues those calls, so this add-on issues them itself: it takes the frame ReShade is
// processing (the backbuffer), the raw depth and the motion vectors prepared by the
// companion effect "DLSS5_Feed.fx" (which converts iMMERSE LaunchPad's optical flow),
// copies the three into textures shared with a private D3D12 device, runs a genuine
// DLSS DLAA evaluate on that device -- where the DLSS 5 add-on inserts its pass --
// and copies the result back over the backbuffer, still inside ReShade's effect chain.
//
// The D3D11 <-> D3D12 transport (shared textures, shared fence, allocator ring) is
// adapted from NIGos' dlss5-dx11-bridge (MIT), see external/bridge-1.0.19/LICENSE.
// The NGX side uses NVIDIA's NGX SDK static library, which locates and loads the
// driver's _nvngx.dll by itself.
//
// Behaviour is driven by dlss5-feed.cfg (re-read while the game runs). dlss5-feed.log
// records what was found, what was built and the result of every NGX call.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11_4.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <d3dcompiler.h>
#include <cstdio>
#include <cstdarg>
#include <cstdint>
#include <cstring>
#include <cstdlib>

#include <reshade.hpp>

#include <nvsdk_ngx.h>
#include <nvsdk_ngx_helpers.h>

#define FEED_VERSION "0.1.0"

extern "C" __declspec(dllexport) const char *NAME = "DLSS 5 Feed " FEED_VERSION;
extern "C" __declspec(dllexport) const char *DESCRIPTION =
    "Feeds DLSS 5 neural rendering with ReShade's depth and LaunchPad motion vectors in games "
    "without DLSS: runs a real DLSS DLAA pass on a private D3D12 device (where the DLSS 5 add-on "
    "hooks in) and writes the result back into the frame. Needs DLSS5_Feed.fx + MartysMods LaunchPad. "
    "Settings in dlss5-feed.cfg, re-read while the game runs.";

// ---------------------------------------------------------------------------
// Logging
// ---------------------------------------------------------------------------

static HMODULE          g_self;
static char             g_log_path[MAX_PATH];
static CRITICAL_SECTION g_log_cs;

static void Log(const char *fmt, ...)
{
    char line[2048];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(line, sizeof(line), _TRUNCATE, fmt, ap);
    va_end(ap);

    SYSTEMTIME st;
    GetLocalTime(&st);

    static long written = 0;
    static bool capped  = false;

    EnterCriticalSection(&g_log_cs);
    if (!capped)
    {
        FILE *f = nullptr;
        if (fopen_s(&f, g_log_path, "a") == 0 && f != nullptr)
        {
            written += fprintf(f, "%02u:%02u:%02u.%03u  %s\n", st.wHour, st.wMinute, st.wSecond,
                               st.wMilliseconds, line);
            if (written > 8 * 1024 * 1024)
            {
                fprintf(f, "\n--- log capped at 8 MB ---\n");
                capped = true;
            }
            fclose(f);
        }
    }
    LeaveCriticalSection(&g_log_cs);
}

// Also raised in ReShade's log/overlay: reserved for things that stop the add-on working.
static void Warn(const char *fmt, ...)
{
    char line[1024];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(line, sizeof(line), _TRUNCATE, fmt, ap);
    va_end(ap);
    Log("%s", line);
    char tagged[1100];
    _snprintf_s(tagged, sizeof(tagged), _TRUNCATE, "[DLSS 5 Feed] %s", line);
    reshade::log::message(reshade::log::level::warning, tagged);
}

static const char *volatile g_where = "starting up";
static void Breadcrumb(const char *what) { g_where = what; }

static LPTOP_LEVEL_EXCEPTION_FILTER g_prev_filter;
static LONG WINAPI CrashFilter(EXCEPTION_POINTERS *ep)
{
    const void *addr = ep && ep->ExceptionRecord ? ep->ExceptionRecord->ExceptionAddress : nullptr;
    const DWORD code = ep && ep->ExceptionRecord ? ep->ExceptionRecord->ExceptionCode : 0;
    wchar_t owner[MAX_PATH] = L"unknown";
    HMODULE mod = nullptr;
    if (addr != nullptr &&
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           static_cast<LPCWSTR>(addr), &mod) && mod != nullptr)
        GetModuleFileNameW(mod, owner, MAX_PATH);
    Log("### CRASH RECORDED ###  exception 0x%08X at %p in %ls; this add-on was last doing: %s%s", code, addr,
        owner, g_where, mod == g_self ? " (inside this add-on)" : "");
    return g_prev_filter != nullptr ? g_prev_filter(ep) : EXCEPTION_CONTINUE_SEARCH;
}

// ---------------------------------------------------------------------------
// Configuration (dlss5-feed.cfg next to the add-on, re-read every 60 frames)
// ---------------------------------------------------------------------------

struct Cfg
{
    int   enabled;         // 0 = do nothing at all
    int   mode;            // 0 inert, 1 transport only (copies the input back, no NGX), 2 full DLSS path
    int   hdr;             // -1 auto (FP16/R11G11B10 backbuffer = HDR), 0 force SDR, 1 force HDR
    int   depth_inverted;  // -1 auto (RESHADE_DEPTH_INPUT_IS_REVERSED), 0 no, 1 yes
    int   flags;           // -1 auto, else raw DLSS.Feature.Create.Flags
    int   reset_every;     // 1 = NGX Reset flag every frame (diagnostic: no temporal history)
    int   warmup_rebuild;  // frames after the first successful evaluate at which the feature is re-created once (0 = never)
    int   rebuild;         // any change of this number re-creates the feature once (manual trigger)
    int   log_frames;      // how many first frames get a full parameter dump in the log
    float mv_scale_x;      // multiplier applied to the motion vectors (the FX already outputs pixels)
    float mv_scale_y;
};

static Cfg g_cfg = { 1, 2, -1, -1, -1, 0, 180, 0, 3, 1.0f, 1.0f };

static void CfgPath(char *out)
{
    GetModuleFileNameA(g_self, out, MAX_PATH);
    if (char *s = strrchr(out, '\\'))
        strcpy_s(s + 1, MAX_PATH - (s + 1 - out), "dlss5-feed.cfg");
}

static void CfgWriteDefault()
{
    char path[MAX_PATH];
    CfgPath(path);
    if (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES) return;
    FILE *f = nullptr;
    if (fopen_s(&f, path, "w") != 0 || f == nullptr) return;
    fprintf(f,
            "enabled=%d\n"
            "mode=%d\n"
            "hdr=%d\n"
            "depth_inverted=%d\n"
            "flags=%d\n"
            "reset_every=%d\n"
            "warmup_rebuild=%d\n"
            "rebuild=%d\n"
            "log_frames=%d\n"
            "mv_scale_x=%.3f\n"
            "mv_scale_y=%.3f\n",
            g_cfg.enabled, g_cfg.mode, g_cfg.hdr, g_cfg.depth_inverted, g_cfg.flags, g_cfg.reset_every,
            g_cfg.warmup_rebuild, g_cfg.rebuild, g_cfg.log_frames, g_cfg.mv_scale_x, g_cfg.mv_scale_y);
    fclose(f);
    Log("[feed] wrote default config to %s", path);
}

// Returns true when a creation-time value changed (the feature has to be rebuilt).
static bool CfgReload()
{
    char path[MAX_PATH];
    CfgPath(path);
    FILE *f = nullptr;
    if (fopen_s(&f, path, "r") != 0 || f == nullptr) return false;

    Cfg next = g_cfg;
    char line[160];
    while (fgets(line, sizeof(line), f) != nullptr)
    {
        char  key[64];
        float val = 0.0f;
        if (sscanf_s(line, "%63[^=]=%f", key, static_cast<unsigned>(sizeof(key)), &val) != 2) continue;
        const int iv = static_cast<int>(val);
        if      (_stricmp(key, "enabled")        == 0) next.enabled        = iv;
        else if (_stricmp(key, "mode")           == 0) next.mode           = iv;
        else if (_stricmp(key, "hdr")            == 0) next.hdr            = iv;
        else if (_stricmp(key, "depth_inverted") == 0) next.depth_inverted = iv;
        else if (_stricmp(key, "flags")          == 0) next.flags          = iv;
        else if (_stricmp(key, "reset_every")    == 0) next.reset_every    = iv;
        else if (_stricmp(key, "warmup_rebuild") == 0) next.warmup_rebuild = iv;
        else if (_stricmp(key, "rebuild")        == 0) next.rebuild        = iv;
        else if (_stricmp(key, "log_frames")     == 0) next.log_frames     = iv;
        else if (_stricmp(key, "mv_scale_x")     == 0) next.mv_scale_x     = val;
        else if (_stricmp(key, "mv_scale_y")     == 0) next.mv_scale_y     = val;
    }
    fclose(f);
    if (next.mode < 0 || next.mode > 2) next.mode = g_cfg.mode;

    const bool rebuild = next.hdr != g_cfg.hdr || next.depth_inverted != g_cfg.depth_inverted ||
                         next.flags != g_cfg.flags || next.rebuild != g_cfg.rebuild;
    const bool changed = rebuild || memcmp(&next, &g_cfg, sizeof(Cfg)) != 0;
    if (!changed) return false;
    g_cfg = next;
    Log("[feed] config: enabled=%d mode=%d hdr=%d depth_inverted=%d flags=%d reset_every=%d warmup_rebuild=%d "
        "rebuild=%d log_frames=%d mv_scale=%.3f,%.3f",
        g_cfg.enabled, g_cfg.mode, g_cfg.hdr, g_cfg.depth_inverted, g_cfg.flags, g_cfg.reset_every,
        g_cfg.warmup_rebuild, g_cfg.rebuild, g_cfg.log_frames, g_cfg.mv_scale_x, g_cfg.mv_scale_y);
    return rebuild;
}

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

enum { SLOT_COLOR = 0, SLOT_OUTPUT, SLOT_DEPTH, SLOT_MV, SLOT_COUNT };
static const char *kSlotName[SLOT_COUNT] = { "Color", "Output", "Depth", "MV" };

static const char *kEffectFile     = "DLSS5_Feed.fx";
static const char *kTechnique      = "DLSS5_Feed";
static const char *kLaunchpadFile  = "MartysMods_LAUNCHPAD.fx";
static const char *kLaunchpadTech  = "MartysMods_Launchpad";

struct Feed
{
    // ReShade side
    reshade::api::effect_runtime          *runtime;
    reshade::api::effect_technique         technique;
    reshade::api::effect_technique         launchpad;
    reshade::api::effect_texture_variable  mv_var;
    reshade::api::effect_texture_variable  depth_var;
    bool                                   depth_reversed;
    bool                                   handles_ok;
    bool                                   missing_reported;

    bool disabled;
    bool session_ready;
    bool frame_ready;
    bool need_reset;
    bool warmup_done;
    int  consecutive_fails;
    int  cfg_rebuild_seen;

    // D3D12 side
    ID3D12Device              *dev12;
    ID3D12CommandQueue        *queue;
    ID3D12GraphicsCommandList *list;
    static const int           kFrames = 3;
    ID3D12CommandAllocator    *alloc[kFrames];
    UINT64                     alloc_fence[kFrames];
    int                        frame_slot;
    HANDLE                     fence_event;
    ID3D12Fence               *fence12;
    ID3D11Fence               *fence11;
    ID3D11DeviceContext4      *ctx4;
    UINT64                     fence_value;
    ID3D11Device              *dev11;      // not owned

    // NGX
    bool                 ngx_inited;
    NVSDK_NGX_Parameter *params;
    NVSDK_NGX_Handle    *feature;

    // shared textures
    ID3D12Resource  *tex12[SLOT_COUNT];
    ID3D11Texture2D *tex11[SLOT_COUNT];
    HANDLE           shared[SLOT_COUNT];
    ID3D11ShaderResourceView *output_srv;   // on tex11[SLOT_OUTPUT], for the copy-back blit
    UINT        width, height;
    DXGI_FORMAT color_fmt, output_fmt;      // shared texture formats
    DXGI_FORMAT bb_fmt;                     // the backbuffer's format, to notice swaps
    bool        hdr;
    int         create_flags;

    // copy-back blit
    ID3D11VertexShader *blit_vs;
    ID3D11PixelShader  *blit_ps;
    ID3D11SamplerState *blit_sampler;

    UINT64 frames_done;

    LONGLONG qpf, cpu_ticks, span_start;
    UINT64   timed_frames;
};

static Feed g;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

template <typename T> static void SafeRelease(T *&p) { if (p) { p->Release(); p = nullptr; } }

static const char *FormatName(DXGI_FORMAT f)
{
    switch (f)
    {
    case DXGI_FORMAT_R16G16B16A16_FLOAT:    return "R16G16B16A16_FLOAT";
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: return "R16G16B16A16_TYPELESS";
    case DXGI_FORMAT_R11G11B10_FLOAT:       return "R11G11B10_FLOAT";
    case DXGI_FORMAT_R10G10B10A2_UNORM:     return "R10G10B10A2_UNORM";
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:  return "R10G10B10A2_TYPELESS";
    case DXGI_FORMAT_R8G8B8A8_UNORM:        return "R8G8B8A8_UNORM";
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:   return "R8G8B8A8_UNORM_SRGB";
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:     return "R8G8B8A8_TYPELESS";
    case DXGI_FORMAT_B8G8R8A8_UNORM:        return "B8G8R8A8_UNORM";
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:   return "B8G8R8A8_UNORM_SRGB";
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:     return "B8G8R8A8_TYPELESS";
    case DXGI_FORMAT_R16G16_FLOAT:          return "R16G16_FLOAT";
    case DXGI_FORMAT_R32_FLOAT:             return "R32_FLOAT";
    case DXGI_FORMAT_R32_TYPELESS:          return "R32_TYPELESS";
    case DXGI_FORMAT_R24G8_TYPELESS:        return "R24G8_TYPELESS";
    default:                                return "?";
    }
}

// The shared Color copy must be typed (the DLSS 5 add-on samples it) and in the same
// typeless family as the backbuffer so CopyResource can move the frame across.
static DXGI_FORMAT TypedColorFormat(DXGI_FORMAT f)
{
    switch (f)
    {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_B8G8R8X8_TYPELESS: case DXGI_FORMAT_B8G8R8X8_UNORM: case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
        return DXGI_FORMAT_B8G8R8X8_UNORM;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS: case DXGI_FORMAT_R10G10B10A2_UNORM:
        return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: case DXGI_FORMAT_R16G16B16A16_FLOAT:
        return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case DXGI_FORMAT_R11G11B10_FLOAT:
        return DXGI_FORMAT_R11G11B10_FLOAT;
    default:
        return DXGI_FORMAT_UNKNOWN;
    }
}

// DLSS writes its Output through a UAV; BGRA/X8 variants are not reliably UAV-typed, so
// they get an RGBA8 output and the copy-back blit takes care of the channel order.
static DXGI_FORMAT OutputFormatFor(DXGI_FORMAT color_typed)
{
    switch (color_typed)
    {
    case DXGI_FORMAT_R16G16B16A16_FLOAT: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case DXGI_FORMAT_R11G11B10_FLOAT:    return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case DXGI_FORMAT_R10G10B10A2_UNORM:  return DXGI_FORMAT_R10G10B10A2_UNORM;
    default:                             return DXGI_FORMAT_R8G8B8A8_UNORM;
    }
}

static bool IsHdrFormat(DXGI_FORMAT typed)
{
    return typed == DXGI_FORMAT_R16G16B16A16_FLOAT || typed == DXGI_FORMAT_R11G11B10_FLOAT;
}

static const char *NgxResultName(NVSDK_NGX_Result r)
{
    switch (static_cast<unsigned>(r))
    {
    case 0x1:        return "Success";
    case 0xBAD00001: return "FeatureNotSupported";
    case 0xBAD00002: return "PlatformError";
    case 0xBAD00003: return "FeatureAlreadyExists";
    case 0xBAD00004: return "FeatureNotFound";
    case 0xBAD00005: return "InvalidParameter";
    case 0xBAD00006: return "ScratchBufferTooSmall";
    case 0xBAD00007: return "NotInitialized";
    case 0xBAD00008: return "UnsupportedInputFormat";
    case 0xBAD00009: return "RWFlagMissing";
    case 0xBAD0000A: return "MissingInput";
    case 0xBAD0000B: return "UnableToInitializeFeature";
    case 0xBAD0000C: return "OutOfDate";
    case 0xBAD0000D: return "OutOfGPUMemory";
    case 0xBAD0000E: return "UnsupportedFormat";
    case 0xBAD0000F: return "UnableToWriteToAppDataPath";
    case 0xBAD00010: return "UnsupportedParameter";
    case 0xBAD00011: return "Denied";
    case 0xBAD00012: return "NotImplemented";
    default:         return "?";
    }
}

static void FeedDisable(const char *why)
{
    if (g.disabled) return;
    g.disabled = true;
    Warn("stopped: %s. The game renders normally. See dlss5-feed.log for the detail.", why);
}

static void FeedFail(const char *what)
{
    Log("[feed] failure: %s", what);
    if (++g.consecutive_fails >= 3)
        FeedDisable("repeated failures");
}

// ---------------------------------------------------------------------------
// D3D12 command submission (allocator ring + shared fence), from the bridge
// ---------------------------------------------------------------------------

static bool BeginCommands()
{
    const int slot = g.frame_slot;
    const UINT64 retire = g.alloc_fence[slot];
    if (retire != 0 && g.fence12->GetCompletedValue() < retire)
    {
        g.fence12->SetEventOnCompletion(retire, g.fence_event);
        if (WaitForSingleObject(g.fence_event, 2000) != WAIT_OBJECT_0)
        {
            Log("[feed] the GPU did not retire allocator slot %d within 2 s", slot);
            FeedDisable("the GPU stopped completing work");
            return false;
        }
    }
    if (g.alloc[slot] == nullptr) return false;
    if (FAILED(g.alloc[slot]->Reset())) return false;
    return SUCCEEDED(g.list->Reset(g.alloc[slot], nullptr));
}

static UINT64 EndCommands()
{
    g.list->Close();
    ID3D12CommandList *lists[] = { g.list };
    g.queue->ExecuteCommandLists(1, lists);
    const UINT64 v = ++g.fence_value;
    g.queue->Signal(g.fence12, v);
    g.alloc_fence[g.frame_slot] = v;
    g.frame_slot = (g.frame_slot + 1) % Feed::kFrames;
    return v;
}

static void DrainGpu()
{
    if (g.queue == nullptr || g.fence12 == nullptr) return;
    const UINT64 v = ++g.fence_value;
    g.queue->Signal(g.fence12, v);
    if (g.fence12->GetCompletedValue() < v && g.fence_event != nullptr)
    {
        g.fence12->SetEventOnCompletion(v, g.fence_event);
        if (WaitForSingleObject(g.fence_event, 5000) != WAIT_OBJECT_0)
            Log("[feed] timed out draining the queue before teardown");
    }
    for (int i = 0; i < Feed::kFrames; ++i) g.alloc_fence[i] = 0;
}

static void Barrier(ID3D12Resource *res, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to)
{
    D3D12_RESOURCE_BARRIER b = {};
    b.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource   = res;
    b.Transition.StateBefore = from;
    b.Transition.StateAfter  = to;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    g.list->ResourceBarrier(1, &b);
}

// ---------------------------------------------------------------------------
// Resources
// ---------------------------------------------------------------------------

static void ReleaseFrameResources()
{
    DrainGpu();
    SafeRelease(g.output_srv);
    for (int i = 0; i < SLOT_COUNT; ++i)
    {
        SafeRelease(g.tex11[i]);
        SafeRelease(g.tex12[i]);
        if (g.shared[i] != nullptr) { CloseHandle(g.shared[i]); g.shared[i] = nullptr; }
    }
    if (g.feature != nullptr)
    {
        Breadcrumb("releasing the DLSS feature");
        NVSDK_NGX_D3D12_ReleaseFeature(g.feature);
        g.feature = nullptr;
    }
    g.frame_ready = false;
}

// One texture visible to both APIs: created on D3D12 and opened on D3D11, or the other
// way round if the driver refuses (WD2's driver only accepted the second route).
static bool MakeSharedPair(ID3D11Device1 *dev1, int i, UINT w, UINT h, DXGI_FORMAT fmt, bool uav)
{
    D3D12_HEAP_PROPERTIES hp = {};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd = {};
    rd.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width            = w;
    rd.Height           = h;
    rd.DepthOrArraySize = 1;
    rd.MipLevels        = 1;
    rd.Format           = fmt;
    rd.SampleDesc.Count = 1;
    rd.Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    rd.Flags            = D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS |
                          (uav ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE);

    HRESULT hr = g.dev12->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_SHARED, &rd, D3D12_RESOURCE_STATE_COMMON,
                                                  nullptr, __uuidof(ID3D12Resource),
                                                  reinterpret_cast<void **>(&g.tex12[i]));
    if (SUCCEEDED(hr))
        hr = g.dev12->CreateSharedHandle(g.tex12[i], nullptr, GENERIC_ALL, nullptr, &g.shared[i]);
    if (SUCCEEDED(hr))
        hr = dev1->OpenSharedResource1(g.shared[i], __uuidof(ID3D11Texture2D), reinterpret_cast<void **>(&g.tex11[i]));
    if (SUCCEEDED(hr))
    {
        Log("[feed] %-6s %ux%u %s via D3D12->D3D11", kSlotName[i], w, h, FormatName(fmt));
        return true;
    }
    Log("[feed] %s: D3D12->D3D11 path failed 0x%08X, trying the other direction", kSlotName[i], hr);
    SafeRelease(g.tex11[i]);
    SafeRelease(g.tex12[i]);
    if (g.shared[i] != nullptr) { CloseHandle(g.shared[i]); g.shared[i] = nullptr; }

    D3D11_TEXTURE2D_DESC td = {};
    td.Width            = w;
    td.Height           = h;
    td.MipLevels        = 1;
    td.ArraySize        = 1;
    td.Format           = fmt;
    td.SampleDesc.Count = 1;
    td.Usage            = D3D11_USAGE_DEFAULT;
    td.BindFlags        = D3D11_BIND_SHADER_RESOURCE | (uav ? D3D11_BIND_UNORDERED_ACCESS : 0);
    td.MiscFlags        = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED;
    hr = dev1->CreateTexture2D(&td, nullptr, &g.tex11[i]);
    if (FAILED(hr)) { Log("[feed] %s: CreateTexture2D failed 0x%08X", kSlotName[i], hr); return false; }

    IDXGIResource1 *dxgi_res = nullptr;
    hr = g.tex11[i]->QueryInterface(__uuidof(IDXGIResource1), reinterpret_cast<void **>(&dxgi_res));
    if (SUCCEEDED(hr))
    {
        hr = dxgi_res->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr,
                                          &g.shared[i]);
        dxgi_res->Release();
    }
    if (SUCCEEDED(hr))
        hr = g.dev12->OpenSharedHandle(g.shared[i], __uuidof(ID3D12Resource), reinterpret_cast<void **>(&g.tex12[i]));
    if (FAILED(hr)) { Log("[feed] %s: D3D11->D3D12 path failed 0x%08X", kSlotName[i], hr); return false; }

    D3D12_RESOURCE_DESC got = g.tex12[i]->GetDesc();
    Log("[feed] %-6s %ux%u %s via D3D11->D3D12 (d3d12 flags=0x%X%s)", kSlotName[i], w, h, FormatName(fmt), got.Flags,
        (got.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) ? " UAV" : "");
    if (i == SLOT_OUTPUT && !(got.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS))
        Log("[feed]   *** Output has no UAV flag on the D3D12 side: DLSS cannot write it ***");
    return true;
}

static bool MakeBlitShaders()
{
    if (g.blit_vs != nullptr && g.blit_ps != nullptr) return true;

    static const char kSrc[] =
        "Texture2D<float4> src : register(t0);\n"
        "SamplerState smp : register(s0);\n"
        "struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };\n"
        "VSOut vs(uint id : SV_VertexID) { VSOut o; float2 uv = float2((id << 1) & 2, id & 2);\n"
        "  o.uv = uv; o.pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1); return o; }\n"
        "float4 ps(VSOut i) : SV_Target { return float4(src.Sample(smp, i.uv).rgb, 1.0); }\n";

    HMODULE m = LoadLibraryW(L"d3dcompiler_47.dll");
    auto compile = m != nullptr ? reinterpret_cast<pD3DCompile>(GetProcAddress(m, "D3DCompile")) : nullptr;
    if (compile == nullptr) { Log("[feed] d3dcompiler_47.dll unavailable"); return false; }

    ID3DBlob *vs = nullptr, *ps = nullptr, *err = nullptr;
    HRESULT hr = compile(kSrc, sizeof(kSrc) - 1, "feedblit", nullptr, nullptr, "vs", "vs_5_0", 0, 0, &vs, &err);
    if (FAILED(hr)) { Log("[feed] blit VS compile failed 0x%08X: %s", hr, err ? (const char *)err->GetBufferPointer() : ""); SafeRelease(err); return false; }
    SafeRelease(err);
    hr = compile(kSrc, sizeof(kSrc) - 1, "feedblit", nullptr, nullptr, "ps", "ps_5_0", 0, 0, &ps, &err);
    if (FAILED(hr)) { Log("[feed] blit PS compile failed 0x%08X: %s", hr, err ? (const char *)err->GetBufferPointer() : ""); SafeRelease(err); SafeRelease(vs); return false; }
    SafeRelease(err);

    hr = g.dev11->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &g.blit_vs);
    if (SUCCEEDED(hr)) hr = g.dev11->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &g.blit_ps);
    vs->Release();
    ps->Release();
    if (FAILED(hr)) { Log("[feed] blit shader creation failed 0x%08X", hr); return false; }

    D3D11_SAMPLER_DESC sd = {};
    sd.Filter   = D3D11_FILTER_MIN_MAG_MIP_POINT;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD   = D3D11_FLOAT32_MAX;
    if (FAILED(g.dev11->CreateSamplerState(&sd, &g.blit_sampler))) { Log("[feed] blit sampler failed"); return false; }
    Log("[feed] copy-back blit shaders ready");
    return true;
}

static bool BuildResources(UINT w, UINT h, DXGI_FORMAT bb_fmt)
{
    Breadcrumb("building shared textures");
    ReleaseFrameResources();

    ID3D11Device1 *dev1 = nullptr;
    if (FAILED(g.dev11->QueryInterface(__uuidof(ID3D11Device1), reinterpret_cast<void **>(&dev1))) || dev1 == nullptr)
    { Log("[feed] ID3D11Device1 unavailable"); return false; }

    g.width      = w;
    g.height     = h;
    g.bb_fmt     = bb_fmt;
    g.color_fmt  = TypedColorFormat(bb_fmt);
    g.output_fmt = OutputFormatFor(g.color_fmt);
    g.hdr        = g_cfg.hdr >= 0 ? g_cfg.hdr != 0 : IsHdrFormat(g.color_fmt);
    const bool inverted = g_cfg.depth_inverted >= 0 ? g_cfg.depth_inverted != 0 : g.depth_reversed;

    if (g.color_fmt == DXGI_FORMAT_UNKNOWN)
    {
        Log("[feed] backbuffer format %u (%s) is not supported", bb_fmt, FormatName(bb_fmt));
        dev1->Release();
        FeedDisable("unsupported backbuffer format");
        return false;
    }

    bool ok = MakeSharedPair(dev1, SLOT_COLOR,  w, h, g.color_fmt,           false) &&
              MakeSharedPair(dev1, SLOT_OUTPUT, w, h, g.output_fmt,          true)  &&
              MakeSharedPair(dev1, SLOT_DEPTH,  w, h, DXGI_FORMAT_R32_FLOAT, false) &&
              MakeSharedPair(dev1, SLOT_MV,     w, h, DXGI_FORMAT_R16G16_FLOAT, false);
    dev1->Release();
    if (!ok) { ReleaseFrameResources(); return false; }

    D3D11_SHADER_RESOURCE_VIEW_DESC sv = {};
    sv.Format              = g.output_fmt;
    sv.ViewDimension       = D3D11_SRV_DIMENSION_TEXTURE2D;
    sv.Texture2D.MipLevels = 1;
    if (FAILED(g.dev11->CreateShaderResourceView(g.tex11[SLOT_OUTPUT], &sv, &g.output_srv)))
    { Log("[feed] output SRV creation failed"); ReleaseFrameResources(); return false; }

    if (!MakeBlitShaders()) { ReleaseFrameResources(); return false; }

    if (g_cfg.mode < 2) { g.frame_ready = true; g.need_reset = true; Log("[feed] transport ready (mode %d, no NGX feature)", g_cfg.mode); return true; }

    // The DLSS contract. DLAA: render size == output size, no jitter, MVs at render size.
    int flags = NVSDK_NGX_DLSS_Feature_Flags_MVLowRes | NVSDK_NGX_DLSS_Feature_Flags_AutoExposure;
    if (inverted) flags |= NVSDK_NGX_DLSS_Feature_Flags_DepthInverted;
    if (g.hdr)    flags |= NVSDK_NGX_DLSS_Feature_Flags_IsHDR;
    if (g_cfg.flags >= 0) flags = g_cfg.flags;
    g.create_flags = flags;

    NVSDK_NGX_DLSS_Create_Params cp = {};
    cp.Feature.InWidth            = w;
    cp.Feature.InHeight           = h;
    cp.Feature.InTargetWidth      = w;
    cp.Feature.InTargetHeight     = h;
    cp.Feature.InPerfQualityValue = NVSDK_NGX_PerfQuality_Value_DLAA;
    cp.InFeatureCreateFlags       = flags;
    cp.InEnableOutputSubrects     = false;

    if (!BeginCommands()) { Log("[feed] could not start a command list"); return false; }
    Breadcrumb("creating the DLSS feature");
    NVSDK_NGX_Result rf = NGX_D3D12_CREATE_DLSS_EXT(g.list, 1, 1, &g.feature, g.params, &cp);
    const UINT64 v = EndCommands();
    if (g.fence12->GetCompletedValue() < v)
    {
        g.fence12->SetEventOnCompletion(v, g.fence_event);
        if (WaitForSingleObject(g.fence_event, 4000) != WAIT_OBJECT_0)
        {
            Log("[feed] feature creation did not complete within 4 s");
            FeedDisable("creating the DLSS feature hung");
            return false;
        }
    }
    if (NVSDK_NGX_FAILED(rf) || g.feature == nullptr)
    {
        Log("[feed] CreateFeature failed 0x%08X (%s)", rf, NgxResultName(rf));
        g.feature = nullptr;
        return false;
    }

    Log("[feed] feature ready: %ux%u DLAA, flags=%d (%s%s%s%s), color %s -> output %s, depth R32_FLOAT%s, mv R16G16_FLOAT",
        w, h, flags,
        (flags & NVSDK_NGX_DLSS_Feature_Flags_IsHDR) ? "HDR " : "SDR ",
        (flags & NVSDK_NGX_DLSS_Feature_Flags_MVLowRes) ? "MVLowRes " : "",
        (flags & NVSDK_NGX_DLSS_Feature_Flags_DepthInverted) ? "DepthInverted " : "",
        (flags & NVSDK_NGX_DLSS_Feature_Flags_AutoExposure) ? "AutoExposure" : "",
        FormatName(g.color_fmt), FormatName(g.output_fmt), inverted ? " (reversed)" : "");
    g.need_reset  = true;
    g.frame_ready = true;
    return true;
}

// ---------------------------------------------------------------------------
// Session: private D3D12 device + NGX
// ---------------------------------------------------------------------------

typedef HRESULT (WINAPI *PFN_D3D12CreateDevice_)(IUnknown *, D3D_FEATURE_LEVEL, REFIID, void **);

static bool InitSession(ID3D11Device *dev11, ID3D11DeviceContext *ctx)
{
    Breadcrumb("opening the D3D12 session");
    Log("################ feed: opening D3D12 session ################");
    g.dev11 = dev11;

    IDXGIDevice  *dxgi_dev = nullptr;
    IDXGIAdapter *adapter  = nullptr;
    if (SUCCEEDED(dev11->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void **>(&dxgi_dev))) && dxgi_dev)
    {
        dxgi_dev->GetAdapter(&adapter);
        dxgi_dev->Release();
    }
    if (adapter != nullptr)
    {
        DXGI_ADAPTER_DESC ad = {};
        adapter->GetDesc(&ad);
        Log("[feed] adapter: %ls  vram=%llu MB", ad.Description, (unsigned long long)(ad.DedicatedVideoMemory >> 20));
    }

    // Loaded here, not imported: ReShade installs its D3D12 hooks when the library arrives,
    // and those hooks are what let the DLSS 5 add-on see this device.
    HMODULE d3d12 = LoadLibraryW(L"d3d12.dll");
    auto create_device = d3d12 ? reinterpret_cast<PFN_D3D12CreateDevice_>(GetProcAddress(d3d12, "D3D12CreateDevice")) : nullptr;
    if (create_device == nullptr) { Log("[feed] no D3D12CreateDevice"); goto fail; }

    {
        HRESULT hr = create_device(adapter, D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), reinterpret_cast<void **>(&g.dev12));
        if (FAILED(hr) || g.dev12 == nullptr) { Log("[feed] D3D12CreateDevice failed 0x%08X", hr); goto fail; }

        wchar_t data_path[MAX_PATH] = {};
        GetModuleFileNameW(g_self, data_path, MAX_PATH);
        if (wchar_t *s = wcsrchr(data_path, L'\\')) *(s + 1) = L'\0';

        Breadcrumb("initialising NGX on D3D12");
        NVSDK_NGX_Result r = NVSDK_NGX_D3D12_Init(0x1000000ULL, data_path, g.dev12, nullptr, NVSDK_NGX_Version_API);
        Log("[feed] NVSDK_NGX_D3D12_Init -> 0x%08X (%s)", r, NgxResultName(r));
        if (NVSDK_NGX_FAILED(r))
        {
            r = NVSDK_NGX_D3D12_Init_with_ProjectID("a0f57b54-1daf-4934-90ae-c4035c19df04", NVSDK_NGX_ENGINE_TYPE_CUSTOM,
                                                    "1.0", data_path, g.dev12, nullptr, NVSDK_NGX_Version_API);
            Log("[feed] NVSDK_NGX_D3D12_Init_with_ProjectID -> 0x%08X (%s)", r, NgxResultName(r));
        }
        if (NVSDK_NGX_FAILED(r)) { Log("[feed] NGX would not initialise on this device/driver"); goto fail; }
        g.ngx_inited = true;

        NVSDK_NGX_Parameter *caps = nullptr;
        r = NVSDK_NGX_D3D12_GetCapabilityParameters(&caps);
        if (NVSDK_NGX_SUCCEED(r) && caps != nullptr)
        {
            int avail = 0, needs_driver = 0, maj = 0, min = 0;
            caps->Get(NVSDK_NGX_Parameter_SuperSampling_Available, &avail);
            caps->Get(NVSDK_NGX_Parameter_SuperSampling_NeedsUpdatedDriver, &needs_driver);
            caps->Get(NVSDK_NGX_Parameter_SuperSampling_MinDriverVersionMajor, &maj);
            caps->Get(NVSDK_NGX_Parameter_SuperSampling_MinDriverVersionMinor, &min);
            Log("[feed] NGX capabilities: SuperSampling.Available=%d NeedsUpdatedDriver=%d MinDriver=%d.%d", avail,
                needs_driver, maj, min);
            if (!avail) { Log("[feed] DLSS super sampling is not available on this GPU/driver"); goto fail; }
        }
        else
            Log("[feed] capability query failed 0x%08X (%s); continuing", r, NgxResultName(r));

        r = NVSDK_NGX_D3D12_AllocateParameters(&g.params);
        if (NVSDK_NGX_FAILED(r) || g.params == nullptr) { Log("[feed] AllocateParameters failed 0x%08X", r); goto fail; }

        D3D12_COMMAND_QUEUE_DESC qd = {};
        qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        g.dev12->CreateCommandQueue(&qd, __uuidof(ID3D12CommandQueue), reinterpret_cast<void **>(&g.queue));
        for (int i = 0; i < Feed::kFrames; ++i)
            g.dev12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, __uuidof(ID3D12CommandAllocator),
                                            reinterpret_cast<void **>(&g.alloc[i]));
        if (g.alloc[0] != nullptr)
            g.dev12->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g.alloc[0], nullptr,
                                       __uuidof(ID3D12GraphicsCommandList), reinterpret_cast<void **>(&g.list));
        if (g.list != nullptr) g.list->Close();
        g.fence_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);

        HANDLE fh = nullptr;
        hr = g.dev12->CreateFence(0, D3D12_FENCE_FLAG_SHARED, __uuidof(ID3D12Fence), reinterpret_cast<void **>(&g.fence12));
        if (SUCCEEDED(hr)) hr = g.dev12->CreateSharedHandle(g.fence12, nullptr, GENERIC_ALL, nullptr, &fh);
        ID3D11Device5 *dev5 = nullptr;
        if (SUCCEEDED(hr) && SUCCEEDED(dev11->QueryInterface(__uuidof(ID3D11Device5), reinterpret_cast<void **>(&dev5))) && dev5)
        {
            hr = dev5->OpenSharedFence(fh, __uuidof(ID3D11Fence), reinterpret_cast<void **>(&g.fence11));
            dev5->Release();
        }
        if (fh != nullptr) CloseHandle(fh);
        if (FAILED(hr) || g.fence11 == nullptr) { Log("[feed] shared fence setup failed 0x%08X", hr); goto fail; }

        if (FAILED(ctx->QueryInterface(__uuidof(ID3D11DeviceContext4), reinterpret_cast<void **>(&g.ctx4))) || g.ctx4 == nullptr)
        { Log("[feed] ID3D11DeviceContext4 unavailable"); goto fail; }

        if (g.queue == nullptr || g.list == nullptr) { Log("[feed] D3D12 queue/list creation failed"); goto fail; }

        Log("[feed] session ready: queue=%p list=%p fence12=%p fence11=%p", (void *)g.queue, (void *)g.list,
            (void *)g.fence12, (void *)g.fence11);
        Log("############# feed: session open #############");
        if (adapter != nullptr) adapter->Release();
        g.session_ready = true;
        return true;
    }

fail:
    if (adapter != nullptr) adapter->Release();
    FeedDisable("the D3D12/NGX session failed to start");
    return false;
}

static void ShutdownSession()
{
    ReleaseFrameResources();
    if (g.params != nullptr) { NVSDK_NGX_D3D12_DestroyParameters(g.params); g.params = nullptr; }
    if (g.ngx_inited && g.dev12 != nullptr) { NVSDK_NGX_D3D12_Shutdown1(g.dev12); g.ngx_inited = false; }
    SafeRelease(g.blit_vs);
    SafeRelease(g.blit_ps);
    SafeRelease(g.blit_sampler);
    SafeRelease(g.ctx4);
    SafeRelease(g.fence11);
    SafeRelease(g.fence12);
    if (g.fence_event != nullptr) { CloseHandle(g.fence_event); g.fence_event = nullptr; }
    SafeRelease(g.list);
    for (int i = 0; i < Feed::kFrames; ++i) SafeRelease(g.alloc[i]);
    SafeRelease(g.queue);
    SafeRelease(g.dev12);
    g.session_ready = false;
    g.dev11 = nullptr;
}

// ---------------------------------------------------------------------------
// Copy-back: shared Output -> the backbuffer ReShade handed us (any format, raw values)
// ---------------------------------------------------------------------------

static void BlitOutputToBackbuffer(ID3D11DeviceContext *ctx, ID3D11RenderTargetView *rtv)
{
    // Save what we touch; ReShade rebinds its own state for every following pass anyway.
    ID3D11RenderTargetView   *old_rtv = nullptr;
    ID3D11DepthStencilView   *old_dsv = nullptr;
    ID3D11VertexShader       *old_vs  = nullptr;
    ID3D11PixelShader        *old_ps  = nullptr;
    ID3D11ShaderResourceView *old_srv = nullptr;
    ID3D11SamplerState       *old_smp = nullptr;
    ID3D11InputLayout        *old_il  = nullptr;
    ID3D11BlendState         *old_bs  = nullptr; FLOAT old_bf[4]; UINT old_mask = 0;
    ID3D11DepthStencilState  *old_ds  = nullptr; UINT old_sref = 0;
    ID3D11RasterizerState    *old_rs  = nullptr;
    D3D11_PRIMITIVE_TOPOLOGY  old_topo = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
    UINT nvp = 1; D3D11_VIEWPORT old_vp = {};
    ctx->OMGetRenderTargets(1, &old_rtv, &old_dsv);
    ctx->VSGetShader(&old_vs, nullptr, nullptr);
    ctx->PSGetShader(&old_ps, nullptr, nullptr);
    ctx->PSGetShaderResources(0, 1, &old_srv);
    ctx->PSGetSamplers(0, 1, &old_smp);
    ctx->IAGetInputLayout(&old_il);
    ctx->IAGetPrimitiveTopology(&old_topo);
    ctx->OMGetBlendState(&old_bs, old_bf, &old_mask);
    ctx->OMGetDepthStencilState(&old_ds, &old_sref);
    ctx->RSGetState(&old_rs);
    ctx->RSGetViewports(&nvp, &old_vp);

    D3D11_VIEWPORT vp = {};
    vp.Width    = static_cast<float>(g.width);
    vp.Height   = static_cast<float>(g.height);
    vp.MaxDepth = 1.0f;
    ID3D11RenderTargetView *rtvs[] = { rtv };
    ID3D11ShaderResourceView *srvs[] = { g.output_srv };
    ID3D11SamplerState *smps[] = { g.blit_sampler };
    ctx->OMSetRenderTargets(1, rtvs, nullptr);
    ctx->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
    ctx->OMSetDepthStencilState(nullptr, 0);
    ctx->RSSetState(nullptr);
    ctx->RSSetViewports(1, &vp);
    ctx->IASetInputLayout(nullptr);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(g.blit_vs, nullptr, 0);
    ctx->PSSetShader(g.blit_ps, nullptr, 0);
    ctx->PSSetShaderResources(0, 1, srvs);
    ctx->PSSetSamplers(0, 1, smps);
    ctx->Draw(3, 0);

    ID3D11ShaderResourceView *no_srv = nullptr;
    ctx->PSSetShaderResources(0, 1, &no_srv);
    ctx->OMSetRenderTargets(1, &old_rtv, old_dsv);
    ctx->VSSetShader(old_vs, nullptr, 0);
    ctx->PSSetShader(old_ps, nullptr, 0);
    ctx->PSSetShaderResources(0, 1, &old_srv);
    ctx->PSSetSamplers(0, 1, &old_smp);
    ctx->IASetInputLayout(old_il);
    ctx->IASetPrimitiveTopology(old_topo);
    ctx->OMSetBlendState(old_bs, old_bf, old_mask);
    ctx->OMSetDepthStencilState(old_ds, old_sref);
    ctx->RSSetState(old_rs);
    if (nvp) ctx->RSSetViewports(1, &old_vp);
    SafeRelease(old_rtv); SafeRelease(old_dsv); SafeRelease(old_vs); SafeRelease(old_ps); SafeRelease(old_srv);
    SafeRelease(old_smp); SafeRelease(old_il); SafeRelease(old_bs); SafeRelease(old_ds); SafeRelease(old_rs);
}

// ---------------------------------------------------------------------------
// Per frame
// ---------------------------------------------------------------------------

static void TimingTick(LONGLONG entry, LONGLONG exit)
{
    if (g.qpf == 0)
    {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        g.qpf = f.QuadPart;
        g.span_start = entry;
    }
    g.cpu_ticks += (exit - entry);
    if (++g.timed_frames < 600) return;
    const double span_ms = 1000.0 * double(exit - g.span_start) / double(g.qpf);
    const double cpu_ms  = 1000.0 * double(g.cpu_ticks) / double(g.qpf);
    const double n       = double(g.timed_frames);
    Log("[feed] 600 frames: feed CPU %.2f ms/frame | frame interval %.2f ms (%.1f fps) | feed is %.0f%% of the frame",
        cpu_ms / n, span_ms / n, 1000.0 / (span_ms / n), 100.0 * cpu_ms / span_ms);
    g.cpu_ticks = 0;
    g.timed_frames = 0;
    g.span_start = exit;
}

static ID3D11Texture2D *AsTexture2D(ID3D11Resource *res, D3D11_TEXTURE2D_DESC *desc)
{
    if (res == nullptr) return nullptr;
    ID3D11Texture2D *tex = nullptr;
    if (FAILED(res->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void **>(&tex))) || tex == nullptr)
        return nullptr;
    tex->GetDesc(desc);
    return tex;  // caller releases
}

static void FeedFrame(reshade::api::effect_runtime *rt, reshade::api::command_list *cl, reshade::api::resource_view rtv)
{
    if (!g_cfg.enabled || g.disabled || g_cfg.mode == 0) return;

    LARGE_INTEGER t0, t1;
    QueryPerformanceCounter(&t0);

    reshade::api::device *dev_api = rt->get_device();
    if (dev_api->get_api() != reshade::api::device_api::d3d11)
    {
        FeedDisable("only Direct3D 11 games are supported");
        return;
    }

    auto *ctx = reinterpret_cast<ID3D11DeviceContext *>(cl->get_native());
    if (ctx == nullptr || ctx->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE) return;

    if ((g.frames_done % 60) == 0 && CfgReload()) g.frame_ready = false;
    if (!g_cfg.enabled || g_cfg.mode == 0) return;

    // Inputs from ReShade: the frame being processed, and the companion effect's two textures.
    reshade::api::resource_view mv_srv = {}, mv_srgb = {}, d_srv = {}, d_srgb = {};
    if (g.mv_var.handle != 0)    rt->get_texture_binding(g.mv_var, &mv_srv, &mv_srgb);
    if (g.depth_var.handle != 0) rt->get_texture_binding(g.depth_var, &d_srv, &d_srgb);
    if (mv_srv.handle == 0 || d_srv.handle == 0)
    {
        if (!g.missing_reported)
        {
            g.missing_reported = true;
            Warn("DLSS5_Feed.fx textures not found (technique %s). Install DLSS5_Feed.fx + MartysMods LaunchPad and enable both.",
                 g.technique.handle ? "found" : "MISSING");
        }
        return;
    }

    auto *color_res = reinterpret_cast<ID3D11Resource *>(dev_api->get_resource_from_view(rtv).handle);
    auto *mv_res    = reinterpret_cast<ID3D11Resource *>(dev_api->get_resource_from_view(mv_srv).handle);
    auto *depth_res = reinterpret_cast<ID3D11Resource *>(dev_api->get_resource_from_view(d_srv).handle);
    auto *rtv11     = reinterpret_cast<ID3D11RenderTargetView *>(rtv.handle);

    D3D11_TEXTURE2D_DESC cd = {}, md = {}, dd = {};
    ID3D11Texture2D *color = AsTexture2D(color_res, &cd);
    ID3D11Texture2D *mv    = AsTexture2D(mv_res, &md);
    ID3D11Texture2D *depth = AsTexture2D(depth_res, &dd);
    if (color == nullptr || mv == nullptr || depth == nullptr)
    {
        SafeRelease(color); SafeRelease(mv); SafeRelease(depth);
        return;
    }

    bool ok = true;
    if (cd.Width != md.Width || cd.Height != md.Height || cd.Width != dd.Width || cd.Height != dd.Height ||
        cd.SampleDesc.Count != 1 || md.Format != DXGI_FORMAT_R16G16_FLOAT || dd.Format != DXGI_FORMAT_R32_FLOAT)
    {
        static bool said = false;
        if (!said)
        {
            said = true;
            Log("[feed] input mismatch: color %ux%u %s samp=%u | mv %ux%u %s | depth %ux%u %s -- skipping",
                cd.Width, cd.Height, FormatName(cd.Format), cd.SampleDesc.Count, md.Width, md.Height,
                FormatName(md.Format), dd.Width, dd.Height, FormatName(dd.Format));
        }
        ok = false;
    }

    if (ok && !g.session_ready)
    {
        ID3D11Device *dev = nullptr;
        ctx->GetDevice(&dev);
        if (dev != nullptr) { ok = InitSession(dev, ctx); dev->Release(); }
        else ok = false;
    }

    if (ok && (!g.frame_ready || cd.Width != g.width || cd.Height != g.height || cd.Format != g.bb_fmt))
    {
        Log("[feed] building: %ux%u backbuffer %s (mv %s, depth %s, depth reversed=%d)", cd.Width, cd.Height,
            FormatName(cd.Format), FormatName(md.Format), FormatName(dd.Format), g.depth_reversed ? 1 : 0);
        ok = BuildResources(cd.Width, cd.Height, cd.Format);
        if (!ok) FeedFail("resource build");
        else g.consecutive_fails = 0;
    }

    if (ok)
    {
        Breadcrumb("copying inputs");
        ctx->CopyResource(g.tex11[SLOT_COLOR], color);
        ctx->CopyResource(g.tex11[SLOT_DEPTH], depth);
        ctx->CopyResource(g.tex11[SLOT_MV], mv);

        if (g_cfg.mode == 1)
        {
            // Transport test: what went out comes straight back, through the same copy-back path.
            ctx->CopyResource(g.tex11[SLOT_OUTPUT], g.tex11[SLOT_COLOR]);
            BlitOutputToBackbuffer(ctx, rtv11);
            ++g.frames_done;
        }
        else
        {
            const UINT64 v_in = ++g.fence_value;
            g.ctx4->Signal(g.fence11, v_in);
            ctx->Flush();
            g.queue->Wait(g.fence12, v_in);

            if (!BeginCommands()) { FeedFail("command list"); ok = false; }
            else
            {
                Barrier(g.tex12[SLOT_COLOR],  D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                Barrier(g.tex12[SLOT_DEPTH],  D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                Barrier(g.tex12[SLOT_MV],     D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                Barrier(g.tex12[SLOT_OUTPUT], D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

                const int reset = (g.need_reset || g_cfg.reset_every) ? 1 : 0;
                g.need_reset = false;

                NVSDK_NGX_D3D12_DLSS_Eval_Params ep = {};
                ep.Feature.pInColor  = g.tex12[SLOT_COLOR];
                ep.Feature.pInOutput = g.tex12[SLOT_OUTPUT];
                ep.Feature.InSharpness = 0.0f;
                ep.pInDepth          = g.tex12[SLOT_DEPTH];
                ep.pInMotionVectors  = g.tex12[SLOT_MV];
                ep.InJitterOffsetX   = 0.0f;
                ep.InJitterOffsetY   = 0.0f;
                ep.InRenderSubrectDimensions.Width  = g.width;
                ep.InRenderSubrectDimensions.Height = g.height;
                ep.InReset           = reset;
                ep.InMVScaleX        = g_cfg.mv_scale_x;
                ep.InMVScaleY        = g_cfg.mv_scale_y;
                ep.InPreExposure     = 1.0f;
                ep.InExposureScale   = 1.0f;

                Breadcrumb("running the D3D12 evaluate");
                NVSDK_NGX_Result re = NGX_D3D12_EVALUATE_DLSS_EXT(g.list, g.feature, g.params, &ep);

                Barrier(g.tex12[SLOT_COLOR],  D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
                Barrier(g.tex12[SLOT_DEPTH],  D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
                Barrier(g.tex12[SLOT_MV],     D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
                Barrier(g.tex12[SLOT_OUTPUT], D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
                const UINT64 v_out = EndCommands();

                if (NVSDK_NGX_FAILED(re))
                {
                    Log("[feed] evaluate failed 0x%08X (%s)", re, NgxResultName(re));
                    FeedFail("evaluate");
                    g.frame_ready = false;  // rebuild rather than repeat the same failure
                    ok = false;
                }
                else
                {
                    Breadcrumb("waiting for the D3D12 result");
                    g.ctx4->Wait(g.fence11, v_out);
                    BlitOutputToBackbuffer(ctx, rtv11);
                    const UINT64 n = ++g.frames_done;
                    g.consecutive_fails = 0;
                    if (n <= static_cast<UINT64>(g_cfg.log_frames) || (n % 1800) == 0)
                        Log("[feed] frame %llu delivered (%ux%u, reset=%d)", n, g.width, g.height, reset);

                    // The DLSS 5 add-on sometimes latches STANDBY/FAILED on the very first create and only
                    // recovers on a fresh one; re-create once after the pipeline has settled.
                    if (g_cfg.warmup_rebuild > 0 && !g.warmup_done && n >= static_cast<UINT64>(g_cfg.warmup_rebuild))
                    {
                        g.warmup_done = true;
                        g.frame_ready = false;
                        Log("[feed] warm-up: re-creating the DLSS feature once (frame %llu)", n);
                    }
                }
            }
        }
    }

    SafeRelease(color);
    SafeRelease(mv);
    SafeRelease(depth);

    QueryPerformanceCounter(&t1);
    TimingTick(t0.QuadPart, t1.QuadPart);
}

// ---------------------------------------------------------------------------
// ReShade events
// ---------------------------------------------------------------------------

static void ResolveHandles(reshade::api::effect_runtime *rt)
{
    g.technique = rt->find_technique(kEffectFile, kTechnique);
    g.mv_var    = rt->find_texture_variable(kEffectFile, "DLSS5_MV");
    g.depth_var = rt->find_texture_variable(kEffectFile, "DLSS5_Depth");
    g.launchpad = rt->find_technique(kLaunchpadFile, kLaunchpadTech);

    char v[16] = {};
    g.depth_reversed = true;  // ReShade.fxh's own default when the definition is absent
    if (rt->get_preprocessor_definition("RESHADE_DEPTH_INPUT_IS_REVERSED", v))
        g.depth_reversed = atoi(v) != 0;

    g.handles_ok = g.technique.handle != 0 && g.mv_var.handle != 0 && g.depth_var.handle != 0;
    g.missing_reported = false;

    // Games can recreate the swapchain (and ReShade its runtime) dozens of times per second;
    // only say something when the situation actually changed.
    const int signature = (g.technique.handle ? 1 : 0) | (g.mv_var.handle ? 2 : 0) | (g.depth_var.handle ? 4 : 0) |
                          (g.launchpad.handle ? 8 : 0) | (g.depth_reversed ? 16 : 0) |
                          ((g.launchpad.handle && rt->get_technique_state(g.launchpad)) ? 32 : 0);
    static int last_signature = -1;
    if (signature == last_signature) return;
    last_signature = signature;

    Log("[feed] effects: %s technique %s, DLSS5_MV %s, DLSS5_Depth %s, LaunchPad technique %s (%s), depth reversed=%d",
        kEffectFile, g.technique.handle ? "found" : "MISSING", g.mv_var.handle ? "found" : "MISSING",
        g.depth_var.handle ? "found" : "MISSING", g.launchpad.handle ? "found" : "MISSING",
        g.launchpad.handle ? ((signature & 32) ? "enabled" : "DISABLED") : "-", g.depth_reversed ? 1 : 0);
    if (!g.handles_ok)
        Warn("DLSS5_Feed.fx is not loaded (technique/textures missing) -- install it into reshade-shaders\\Shaders and enable it below MartysMods_Launchpad.");
    else if (g.launchpad.handle == 0)
        Warn("MartysMods_LAUNCHPAD.fx not found: motion vectors will be zero (still images only).");
}

static void OnInitEffectRuntime(reshade::api::effect_runtime *rt)
{
    g.runtime = rt;
    ResolveHandles(rt);
    // A recreated runtime means the DLSS 5 add-on has re-armed its hooks: give it a fresh feature.
    if (g.session_ready) g.frame_ready = false;
    static int inits = 0;
    if (++inits <= 8) Log("[feed] effect runtime %p initialised", (void *)rt);
    else if (inits == 9) Log("[feed] (further runtime init/destroy messages suppressed)");
}

static void OnDestroyEffectRuntime(reshade::api::effect_runtime *rt)
{
    if (rt != g.runtime) return;
    static int destroys = 0;
    if (++destroys <= 8) Log("[feed] effect runtime %p destroyed", (void *)rt);
    ReleaseFrameResources();
    g.runtime = nullptr;
    g.technique = {}; g.launchpad = {}; g.mv_var = {}; g.depth_var = {};
    g.handles_ok = false;
}

static void OnReloadedEffects(reshade::api::effect_runtime *rt)
{
    if (rt == g.runtime || g.runtime == nullptr) { g.runtime = rt; ResolveHandles(rt); }
}

static void OnRenderTechnique(reshade::api::effect_runtime *rt, reshade::api::effect_technique technique,
                              reshade::api::command_list *cl, reshade::api::resource_view rtv,
                              reshade::api::resource_view /*rtv_srgb*/)
{
    if (rt != g.runtime || g.technique.handle == 0 || technique.handle != g.technique.handle) return;
    FeedFrame(rt, cl, rtv);
}

static void OnDestroyDevice(reshade::api::device *dev)
{
    if (g.dev11 != nullptr && reinterpret_cast<ID3D11Device *>(dev->get_native()) == g.dev11)
    {
        Log("[feed] D3D11 device destroyed; shutting the session down");
        ShutdownSession();
    }
}

// ---------------------------------------------------------------------------

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        g_self = module;
        DisableThreadLibraryCalls(module);
        InitializeCriticalSection(&g_log_cs);
        GetModuleFileNameA(module, g_log_path, MAX_PATH);
        if (char *s = strrchr(g_log_path, '\\'))
            strcpy_s(s + 1, MAX_PATH - (s + 1 - g_log_path), "dlss5-feed.log");
        { FILE *f = nullptr; if (fopen_s(&f, g_log_path, "w") == 0 && f) fclose(f); }

        if (!reshade::register_addon(module)) return FALSE;

        g_prev_filter = SetUnhandledExceptionFilter(&CrashFilter);
        Log("dlss5-feed %s (built %s %s) attached.", FEED_VERSION, __DATE__, __TIME__);
        {
            wchar_t exe[MAX_PATH] = {};
            GetModuleFileNameW(nullptr, exe, MAX_PATH);
            Log("  host: %ls", exe);
        }
        CfgWriteDefault();
        CfgReload();

        reshade::register_event<reshade::addon_event::init_effect_runtime>(OnInitEffectRuntime);
        reshade::register_event<reshade::addon_event::destroy_effect_runtime>(OnDestroyEffectRuntime);
        reshade::register_event<reshade::addon_event::reshade_reloaded_effects>(OnReloadedEffects);
        reshade::register_event<reshade::addon_event::reshade_render_technique>(OnRenderTechnique);
        reshade::register_event<reshade::addon_event::destroy_device>(OnDestroyDevice);
    }
    else if (reason == DLL_PROCESS_DETACH)
    {
        reshade::unregister_event<reshade::addon_event::init_effect_runtime>(OnInitEffectRuntime);
        reshade::unregister_event<reshade::addon_event::destroy_effect_runtime>(OnDestroyEffectRuntime);
        reshade::unregister_event<reshade::addon_event::reshade_reloaded_effects>(OnReloadedEffects);
        reshade::unregister_event<reshade::addon_event::reshade_render_technique>(OnRenderTechnique);
        reshade::unregister_event<reshade::addon_event::destroy_device>(OnDestroyDevice);
        ShutdownSession();
        reshade::unregister_addon(module);
        Log("shut down cleanly.");
    }
    return TRUE;
}
