// dlss5-feed-host64 - the 64-bit half of DLSS5-Feeder for 32-bit games.
//
// A 32-bit game cannot load NGX or the DLSS 5 add-on (both x64-only). This little
// process can: it puts ReShade x64 (dxgi.dll) and renodx-dlss5.addon64 next to
// itself, opens a hidden 1x1 window with a minimal D3D12 swapchain -- so from the
// DLSS 5 add-on's point of view it IS a D3D12 game -- and runs the NGX DLAA
// evaluate on frames the game delivers through cross-process shared textures
// (created game-side on D3D11; see the phase-0 spike) and shared fences.
//
//   dlss5-feed-host64.exe --test   stand-alone: synthetic pattern, no game needed
//                                  (phase-1 proof: "feature 18 created" in ReShade.log)
//   dlss5-feed-host64.exe <pid>    serve the game with that PID over the pipe
//
// Logs to dlss5-feed-host.log next to the exe; the DLSS 5 add-on's own state
// appears in the host's ReShade.log.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <cstdio>
#include <cstdarg>
#include <cstdint>
#include <cstring>
#include <vector>

#include <nvsdk_ngx.h>
#include <nvsdk_ngx_helpers.h>

#include "../src/feed_ipc.h"

// ---------------------------------------------------------------------------
// Logging
// ---------------------------------------------------------------------------

static char g_log_path[MAX_PATH];
static bool g_show_window = false;   // visible host window = the user's door to the DLSS 5 panel

static void Log(const char *fmt, ...)
{
    char line[2048];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(line, sizeof(line), _TRUNCATE, fmt, ap);
    va_end(ap);
    SYSTEMTIME st;
    GetLocalTime(&st);
    printf("%02u:%02u:%02u.%03u  %s\n", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, line);
    FILE *f = nullptr;
    if (fopen_s(&f, g_log_path, "a") == 0 && f != nullptr)
    {
        fprintf(f, "%02u:%02u:%02u.%03u  %s\n", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, line);
        fclose(f);
    }
}

static const char *NgxResultName(NVSDK_NGX_Result r)
{
    switch (static_cast<unsigned>(r))
    {
    case 0x1:        return "Success";
    case 0xBAD00005: return "InvalidParameter";
    case 0xBAD00007: return "NotInitialized";
    case 0xBAD00008: return "UnsupportedInputFormat";
    case 0xBAD0000A: return "MissingInput";
    case 0xBAD0000B: return "UnableToInitializeFeature";
    case 0xBAD0000D: return "OutOfGPUMemory";
    case 0xBAD0000E: return "UnsupportedFormat";
    default:         return "?";
    }
}

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

struct Host
{
    HWND                       hwnd;
    IDXGISwapChain1           *swap;
    ID3D12Device              *dev;
    ID3D12CommandQueue        *queue;      // NGX work
    ID3D12CommandQueue        *pump_queue; // owns the dummy swapchain
    ID3D12GraphicsCommandList *list;
    static const int           kFrames = 3;
    ID3D12CommandAllocator    *alloc[kFrames];
    UINT64                     alloc_fence[kFrames];
    int                        frame_slot;
    ID3D12Fence               *fence;      // internal (allocator ring)
    HANDLE                     fence_event;
    UINT64                     fence_value;

    // cross-process
    ID3D12Fence *fence_in;   // game signals, host waits
    ID3D12Fence *fence_out;  // host signals, game waits

    bool                 ngx_inited;
    NVSDK_NGX_Parameter *params;
    NVSDK_NGX_Handle    *feature;

    ID3D12Resource *tex[FEED_SLOTS];
    UINT            width, height;
    DXGI_FORMAT     color_fmt, output_fmt;
};

static Host h;

// ---------------------------------------------------------------------------
// Command submission (allocator ring), same shape as the add-on
// ---------------------------------------------------------------------------

static bool BeginCommands()
{
    const int slot = h.frame_slot;
    const UINT64 retire = h.alloc_fence[slot];
    if (retire != 0 && h.fence->GetCompletedValue() < retire)
    {
        h.fence->SetEventOnCompletion(retire, h.fence_event);
        if (WaitForSingleObject(h.fence_event, 2000) != WAIT_OBJECT_0)
        { Log("[host] GPU did not retire allocator slot %d", slot); return false; }
    }
    if (FAILED(h.alloc[slot]->Reset())) return false;
    return SUCCEEDED(h.list->Reset(h.alloc[slot], nullptr));
}

static UINT64 EndCommands()
{
    h.list->Close();
    ID3D12CommandList *lists[] = { h.list };
    h.queue->ExecuteCommandLists(1, lists);
    const UINT64 v = ++h.fence_value;
    h.queue->Signal(h.fence, v);
    h.alloc_fence[h.frame_slot] = v;
    h.frame_slot = (h.frame_slot + 1) % Host::kFrames;
    return v;
}

static bool WaitFenceValue(ID3D12Fence *f, UINT64 v, DWORD ms)
{
    if (f->GetCompletedValue() >= v) return true;
    f->SetEventOnCompletion(v, h.fence_event);
    return WaitForSingleObject(h.fence_event, ms) == WAIT_OBJECT_0;
}

static void CloseListGuarded()
{
    __try { h.list->Close(); } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

static void AbortCommands()   // never execute a list NGX crashed in
{
    if (h.list == nullptr) return;
    CloseListGuarded();
    h.list->Release();
    h.list = nullptr;
    if (SUCCEEDED(h.dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, h.alloc[h.frame_slot], nullptr,
                                           __uuidof(ID3D12GraphicsCommandList), reinterpret_cast<void **>(&h.list))))
        h.list->Close();
}

static NVSDK_NGX_Result SafeCreateDLSS(NVSDK_NGX_DLSS_Create_Params *cp, DWORD *code)
{
    *code = 0;
    __try { return NGX_D3D12_CREATE_DLSS_EXT(h.list, 1, 1, &h.feature, h.params, cp); }
    __except (EXCEPTION_EXECUTE_HANDLER) { *code = GetExceptionCode(); return static_cast<NVSDK_NGX_Result>(0x7FFFFFFF); }
}

static NVSDK_NGX_Result SafeEvaluateDLSS(NVSDK_NGX_D3D12_DLSS_Eval_Params *ep, DWORD *code)
{
    *code = 0;
    __try { return NGX_D3D12_EVALUATE_DLSS_EXT(h.list, h.feature, h.params, ep); }
    __except (EXCEPTION_EXECUTE_HANDLER) { *code = GetExceptionCode(); return static_cast<NVSDK_NGX_Result>(0x7FFFFFFF); }
}

static void SafeReleaseFeature(NVSDK_NGX_Handle *f)
{
    if (f == nullptr) return;
    __try { NVSDK_NGX_D3D12_ReleaseFeature(f); }
    __except (EXCEPTION_EXECUTE_HANDLER) { Log("[host] ReleaseFeature raised 0x%08X (ignored)", GetExceptionCode()); }
}

// ---------------------------------------------------------------------------
// The disguise: hidden window + minimal D3D12 swapchain so ReShade x64 loads
// and the DLSS 5 add-on arms itself, exactly as in a real D3D12 game.
// ---------------------------------------------------------------------------

static LRESULT CALLBACK WndProc(HWND w, UINT m, WPARAM wp, LPARAM lp) { return DefWindowProcW(w, m, wp, lp); }

typedef HRESULT (WINAPI *PFN_D3D12CreateDevice_)(IUnknown *, D3D_FEATURE_LEVEL, REFIID, void **);
typedef HRESULT (WINAPI *PFN_CreateDXGIFactory1_)(REFIID, void **);

static void PumpPresent()
{
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    if (h.swap != nullptr) h.swap->Present(0, 0);
}

static bool InitDisguise()
{
    // ReShade first: the app-directory dxgi.dll IS ReShade x64. Loading it before
    // d3d12.dll means every later D3D12/DXGI entry point goes through its hooks --
    // the same order a real game gets, and what lets the DLSS 5 add-on see us.
    HMODULE dxgi = LoadLibraryW(L"dxgi.dll");
    HMODULE d3d12 = LoadLibraryW(L"d3d12.dll");
    auto create_device  = d3d12 ? reinterpret_cast<PFN_D3D12CreateDevice_>(GetProcAddress(d3d12, "D3D12CreateDevice")) : nullptr;
    auto create_factory = dxgi ? reinterpret_cast<PFN_CreateDXGIFactory1_>(GetProcAddress(dxgi, "CreateDXGIFactory1")) : nullptr;
    if (create_device == nullptr || create_factory == nullptr) { Log("[host] dxgi/d3d12 exports missing"); return false; }

    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(dxgi, exe, MAX_PATH);
    Log("[host] dxgi.dll: %ls", exe);

    WNDCLASSW wc = {};
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"dlss5feedhost";
    RegisterClassW(&wc);
    h.hwnd = CreateWindowExW(0, wc.lpszClassName,
                             L"DLSS 5 Feed host - press Home HERE to tune DLSS 5 neural rendering",
                             WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 960, 540,
                             nullptr, nullptr, wc.hInstance, nullptr);
    if (h.hwnd == nullptr) { Log("[host] window creation failed"); return false; }
    if (g_show_window) ShowWindow(h.hwnd, SW_SHOWNOACTIVATE);   // never steal the game's focus

    HRESULT hr = create_device(nullptr, D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device),
                               reinterpret_cast<void **>(&h.dev));
    if (FAILED(hr)) { Log("[host] D3D12CreateDevice failed 0x%08X", hr); return false; }

    D3D12_COMMAND_QUEUE_DESC qd = {};
    h.dev->CreateCommandQueue(&qd, __uuidof(ID3D12CommandQueue), reinterpret_cast<void **>(&h.pump_queue));
    h.dev->CreateCommandQueue(&qd, __uuidof(ID3D12CommandQueue), reinterpret_cast<void **>(&h.queue));
    if (h.pump_queue == nullptr || h.queue == nullptr) { Log("[host] queue creation failed"); return false; }

    IDXGIFactory2 *factory = nullptr;
    hr = create_factory(__uuidof(IDXGIFactory2), reinterpret_cast<void **>(&factory));
    if (FAILED(hr)) { Log("[host] CreateDXGIFactory1 failed 0x%08X", hr); return false; }

    DXGI_SWAP_CHAIN_DESC1 sd = {};
    sd.Width            = 960;
    sd.Height           = 540;
    sd.Format           = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage      = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount      = 2;
    sd.SwapEffect       = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    hr = factory->CreateSwapChainForHwnd(h.pump_queue, h.hwnd, &sd, nullptr, nullptr, &h.swap);
    factory->Release();
    if (FAILED(hr)) { Log("[host] CreateSwapChainForHwnd failed 0x%08X", hr); return false; }
    Log("[host] disguise up: hidden window + D3D12 swapchain (ReShade should be attached now)");

    for (int i = 0; i < 60; ++i) PumpPresent();   // let ReShade + the DLSS 5 add-on settle

    // Ring + internal fence for our own submissions.
    for (int i = 0; i < Host::kFrames; ++i)
        h.dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, __uuidof(ID3D12CommandAllocator),
                                      reinterpret_cast<void **>(&h.alloc[i]));
    if (h.alloc[0] != nullptr)
        h.dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, h.alloc[0], nullptr,
                                 __uuidof(ID3D12GraphicsCommandList), reinterpret_cast<void **>(&h.list));
    if (h.list != nullptr) h.list->Close();
    h.dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), reinterpret_cast<void **>(&h.fence));
    h.fence_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (h.list == nullptr || h.fence == nullptr) { Log("[host] list/fence creation failed"); return false; }
    return true;
}

static bool InitNgx()
{
    wchar_t data_path[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, data_path, MAX_PATH);
    if (wchar_t *s = wcsrchr(data_path, L'\\')) *(s + 1) = L'\0';

    NVSDK_NGX_Result r = NVSDK_NGX_D3D12_Init(0x1000000ULL, data_path, h.dev, nullptr, NVSDK_NGX_Version_API);
    Log("[host] NVSDK_NGX_D3D12_Init -> 0x%08X (%s)", r, NgxResultName(r));
    if (NVSDK_NGX_FAILED(r))
    {
        r = NVSDK_NGX_D3D12_Init_with_ProjectID("a0f57b54-1daf-4934-90ae-c4035c19df04", NVSDK_NGX_ENGINE_TYPE_CUSTOM,
                                                "1.0", data_path, h.dev, nullptr, NVSDK_NGX_Version_API);
        Log("[host] Init_with_ProjectID -> 0x%08X (%s)", r, NgxResultName(r));
    }
    if (NVSDK_NGX_FAILED(r)) return false;
    h.ngx_inited = true;

    NVSDK_NGX_Parameter *caps = nullptr;
    r = NVSDK_NGX_D3D12_GetCapabilityParameters(&caps);
    if (NVSDK_NGX_SUCCEED(r) && caps != nullptr)
    {
        int avail = 0;
        caps->Get(NVSDK_NGX_Parameter_SuperSampling_Available, &avail);
        Log("[host] SuperSampling.Available=%d", avail);
        if (!avail) return false;
    }
    r = NVSDK_NGX_D3D12_AllocateParameters(&h.params);
    if (NVSDK_NGX_FAILED(r) || h.params == nullptr) { Log("[host] AllocateParameters failed 0x%08X", r); return false; }
    return true;
}

static bool CreateFeature(UINT w, UINT h_, int flags, NVSDK_NGX_Result *out_r)
{
    NVSDK_NGX_DLSS_Create_Params cp = {};
    cp.Feature.InWidth            = w;
    cp.Feature.InHeight           = h_;
    cp.Feature.InTargetWidth      = w;
    cp.Feature.InTargetHeight     = h_;
    cp.Feature.InPerfQualityValue = NVSDK_NGX_PerfQuality_Value_DLAA;
    cp.InFeatureCreateFlags       = flags;
    cp.InEnableOutputSubrects     = false;

    if (!BeginCommands()) return false;
    DWORD ccode = 0;
    NVSDK_NGX_Result rf = SafeCreateDLSS(&cp, &ccode);
    if (out_r != nullptr) *out_r = rf;
    if (ccode != 0)
    {
        AbortCommands();
        Log("[host] CreateFeature raised 0x%08X (caught; nothing submitted)", ccode);
        return false;
    }
    const UINT64 v = EndCommands();
    if (!WaitFenceValue(h.fence, v, 4000)) { Log("[host] feature create did not complete"); return false; }
    if (NVSDK_NGX_FAILED(rf) || h.feature == nullptr)
    { Log("[host] CreateFeature failed 0x%08X (%s)", rf, NgxResultName(rf)); h.feature = nullptr; return false; }
    Log("[host] feature ready: %ux%u DLAA flags=%d", w, h_, flags);
    return true;
}

static bool Evaluate(ID3D12Resource *color, ID3D12Resource *output, ID3D12Resource *depth, ID3D12Resource *mv,
                     UINT w, UINT h_, int reset, float mvsx, float mvsy)
{
    if (!BeginCommands()) return false;

    NVSDK_NGX_D3D12_DLSS_Eval_Params ep = {};
    ep.Feature.pInColor  = color;
    ep.Feature.pInOutput = output;
    ep.pInDepth          = depth;
    ep.pInMotionVectors  = mv;
    ep.InRenderSubrectDimensions.Width  = w;
    ep.InRenderSubrectDimensions.Height = h_;
    ep.InReset           = reset;
    ep.InMVScaleX        = mvsx;
    ep.InMVScaleY        = mvsy;
    ep.InPreExposure     = 1.0f;
    ep.InExposureScale   = 1.0f;

    DWORD ecode = 0;
    NVSDK_NGX_Result re = SafeEvaluateDLSS(&ep, &ecode);
    if (ecode != 0) { AbortCommands(); Log("[host] evaluate raised 0x%08X (caught; nothing submitted)", ecode); return false; }
    EndCommands();
    if (NVSDK_NGX_FAILED(re)) { Log("[host] evaluate failed 0x%08X (%s)", re, NgxResultName(re)); return false; }
    return true;
}

// ---------------------------------------------------------------------------
// --test: prove the whole stack with no game attached
// ---------------------------------------------------------------------------

static ID3D12Resource *MakeTex(UINT w, UINT h_, DXGI_FORMAT fmt, bool uav)
{
    D3D12_HEAP_PROPERTIES hp = {};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd = {};
    rd.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width            = w;
    rd.Height           = h_;
    rd.DepthOrArraySize = 1;
    rd.MipLevels        = 1;
    rd.Format           = fmt;
    rd.SampleDesc.Count = 1;
    rd.Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    rd.Flags            = uav ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE;
    ID3D12Resource *t = nullptr;
    h.dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COMMON, nullptr,
                                   __uuidof(ID3D12Resource), reinterpret_cast<void **>(&t));
    return t;
}

static int RunTest()
{
    const UINT W = 640, H = 360;
    Log("[host] --test: %ux%u synthetic DLAA", W, H);

    ID3D12Resource *color  = MakeTex(W, H, DXGI_FORMAT_R8G8B8A8_UNORM, false);
    ID3D12Resource *output = MakeTex(W, H, DXGI_FORMAT_R8G8B8A8_UNORM, true);
    ID3D12Resource *depth  = MakeTex(W, H, DXGI_FORMAT_R32_FLOAT, false);
    ID3D12Resource *mv     = MakeTex(W, H, DXGI_FORMAT_R16G16_FLOAT, false);
    if (!color || !output || !depth || !mv) { Log("[host] test texture creation failed"); return 1; }

    // Give the DLSS 5 add-on its hook-arming time, with the swapchain pumping.
    for (int i = 0; i < 120; ++i) { PumpPresent(); Sleep(8); }

    int flags = NVSDK_NGX_DLSS_Feature_Flags_MVLowRes | NVSDK_NGX_DLSS_Feature_Flags_AutoExposure |
                NVSDK_NGX_DLSS_Feature_Flags_DepthInverted;
    NVSDK_NGX_Result rf = NVSDK_NGX_Result_Fail;
    if (!CreateFeature(W, H, flags, &rf)) return 1;

    int good = 0;
    for (int i = 0; i < 300; ++i)
    {
        PumpPresent();
        if (Evaluate(color, output, depth, mv, W, H, i == 0 ? 1 : 0, 1.0f, 1.0f)) ++good;
        else break;
        if (i == 180)   // the warm-up re-create, same medicine as in-game
        {
            Log("[host] warm-up: re-creating the feature once");
            NVSDK_NGX_Handle *old = h.feature;
            h.feature = nullptr;
            if (!CreateFeature(W, H, flags, &rf)) { h.feature = old; Log("[host] keeping the previous feature"); }
            else SafeReleaseFeature(old);
        }
    }
    Log("[host] --test finished: %d/300 evaluates succeeded", good);
    Log("[host] check the host's ReShade.log for 'feature 18 created' / 'evaluation succeeded'");
    return good >= 250 ? 0 : 1;
}

// ---------------------------------------------------------------------------
// Serve mode: the real pipe server for a 32-bit game
// ---------------------------------------------------------------------------

static bool ReadFull(HANDLE pipe, void *buf, DWORD len)
{
    DWORD got = 0;
    return ReadFile(pipe, buf, len, &got, nullptr) && got == len;
}

static int Serve(DWORD game_pid)
{
    char name[128];
    sprintf_s(name, FEED_PIPE_FMT, static_cast<unsigned long>(game_pid));
    HANDLE pipe = CreateNamedPipeA(name, PIPE_ACCESS_DUPLEX, PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
                                   1, 1024, 1024, 0, nullptr);
    if (pipe == INVALID_HANDLE_VALUE) { Log("[host] CreateNamedPipe failed %lu", GetLastError()); return 1; }
    Log("[host] serving on %s", name);
    if (!ConnectNamedPipe(pipe, nullptr) && GetLastError() != ERROR_PIPE_CONNECTED)
    { Log("[host] ConnectNamedPipe failed %lu", GetLastError()); return 1; }

    FeedHello hello = {};
    if (!ReadFull(pipe, &hello, sizeof(hello)) || hello.magic != FEED_IPC_MAGIC)
    { Log("[host] bad hello"); return 1; }
    FeedHelloAck ack = { FEED_IPC_MAGIC, FEED_IPC_VERSION };
    DWORD put = 0;
    WriteFile(pipe, &ack, sizeof(ack), &put, nullptr);
    Log("[host] game pid %u connected (protocol v%u)", hello.pid, hello.version);

    HANDLE hgame = OpenProcess(PROCESS_DUP_HANDLE, FALSE, hello.pid);
    if (hgame == nullptr) { Log("[host] OpenProcess failed %lu", GetLastError()); return 1; }

    // Shared fences live for the whole session.
    HANDLE hin = nullptr, hout = nullptr;
    h.dev->CreateFence(0, D3D12_FENCE_FLAG_SHARED, __uuidof(ID3D12Fence), reinterpret_cast<void **>(&h.fence_in));
    h.dev->CreateFence(0, D3D12_FENCE_FLAG_SHARED, __uuidof(ID3D12Fence), reinterpret_cast<void **>(&h.fence_out));
    if (h.fence_in == nullptr || h.fence_out == nullptr ||
        FAILED(h.dev->CreateSharedHandle(h.fence_in, nullptr, GENERIC_ALL, nullptr, &hin)) ||
        FAILED(h.dev->CreateSharedHandle(h.fence_out, nullptr, GENERIC_ALL, nullptr, &hout)))
    { Log("[host] shared fence creation failed"); return 1; }

    HANDLE game_in = nullptr, game_out = nullptr;
    DuplicateHandle(GetCurrentProcess(), hin, hgame, &game_in, 0, FALSE, DUPLICATE_SAME_ACCESS);
    DuplicateHandle(GetCurrentProcess(), hout, hgame, &game_out, 0, FALSE, DUPLICATE_SAME_ACCESS);

    int flags_active = 0;
    bool transport_only = false;
    float mvsx = 1.0f, mvsy = 1.0f;
    // The DLSS 5 add-on arms its NGX hooks ~150 ms after NGX init; the first create must
    // not race that (a 15 ms miss latched STANDBY in Blacklist), so hold it briefly.
    UINT64 hold_until = GetTickCount64() + 800;
    UINT64 evaluated  = 0;
    bool   warm_done  = false;

    for (;;)
    {
        // Peek the next message type by size: Build (big) vs FrameMsg (small).
        // The pipe is byte-mode from a single writer, so read the smaller header
        // first and decide -- FeedFrameMsg and FeedBuild share no prefix, so the
        // client precedes every message with a 1-byte tag instead.
        BYTE tag = 0;
        if (!ReadFull(pipe, &tag, 1)) { Log("[host] pipe closed by the game"); break; }

        if (tag == 'B')
        {
            FeedBuild b = {};
            if (!ReadFull(pipe, &b, sizeof(b))) break;
            Log("[host] build: %ux%u color=%u output=%u hdr=%d inverted=%d", b.width, b.height,
                b.color_fmt, b.output_fmt, b.hdr, b.depth_inverted);

            // Tear down the old set.
            SafeReleaseFeature(h.feature);
            h.feature = nullptr;
            for (int i = 0; i < FEED_SLOTS; ++i)
                if (h.tex[i] != nullptr) { h.tex[i]->Release(); h.tex[i] = nullptr; }

            // Open the game's textures (duplicate the handles out of the game).
            bool ok = true;
            for (int i = 0; i < FEED_SLOTS && ok; ++i)
            {
                HANDLE local = nullptr;
                if (!DuplicateHandle(hgame, reinterpret_cast<HANDLE>(static_cast<uintptr_t>(b.tex[i])),
                                     GetCurrentProcess(), &local, 0, FALSE, DUPLICATE_SAME_ACCESS))
                { Log("[host] DuplicateHandle(tex %d) failed %lu", i, GetLastError()); ok = false; break; }
                HRESULT hr = h.dev->OpenSharedHandle(local, __uuidof(ID3D12Resource),
                                                     reinterpret_cast<void **>(&h.tex[i]));
                CloseHandle(local);
                if (FAILED(hr)) { Log("[host] OpenSharedHandle(tex %d) failed 0x%08X", i, hr); ok = false; }
            }

            NVSDK_NGX_Result rf = NVSDK_NGX_Result_Fail;
            if (ok)
            {
                h.width = b.width; h.height = b.height;
                h.color_fmt  = static_cast<DXGI_FORMAT>(b.color_fmt);
                h.output_fmt = static_cast<DXGI_FORMAT>(b.output_fmt);
                mvsx = b.mv_scale_x; mvsy = b.mv_scale_y;
                transport_only = b.transport != 0;
                flags_active = NVSDK_NGX_DLSS_Feature_Flags_MVLowRes | NVSDK_NGX_DLSS_Feature_Flags_AutoExposure;
                if (b.depth_inverted) flags_active |= NVSDK_NGX_DLSS_Feature_Flags_DepthInverted;
                if (b.hdr)            flags_active |= NVSDK_NGX_DLSS_Feature_Flags_IsHDR;
                if (b.flags_override >= 0) flags_active = b.flags_override;

                if (transport_only)
                {
                    rf = static_cast<NVSDK_NGX_Result>(1);   // no NGX in the loop at all
                    Log("[host] transport-only mode: Color will be copied to Output, no evaluate");
                }
                else
                {
                    const UINT64 now = GetTickCount64();
                    if (now < hold_until) Sleep(static_cast<DWORD>(hold_until - now));  // hook-arming grace
                    ok = CreateFeature(b.width, b.height, flags_active, &rf);
                    hold_until = GetTickCount64() + 1000;   // next create not before +1 s
                }
            }

            evaluated = 0;
            warm_done = transport_only;   // no warm-up needed without NGX

            FeedBuildAck back = {};
            back.ok         = ok ? 1 : 0;
            back.ngx_result = static_cast<uint32_t>(rf);
            back.fence_in   = reinterpret_cast<uint64_t>(game_in);
            back.fence_out  = reinterpret_cast<uint64_t>(game_out);
            WriteFile(pipe, &back, sizeof(back), &put, nullptr);
        }
        else if (tag == 'F')
        {
            FeedFrameMsg fm = {};
            if (!ReadFull(pipe, &fm, sizeof(fm))) break;
            if (h.feature == nullptr && !transport_only) { h.fence_out->Signal(fm.n); continue; }

            if (!WaitFenceValue(h.fence_in, fm.n, 2000))
            { Log("[host] frame %llu: in-fence never arrived", (unsigned long long)fm.n); h.fence_out->Signal(fm.n); continue; }
            h.queue->Wait(h.fence_in, fm.n);   // belt and braces on the GPU timeline

            bool done = false;
            if (transport_only)
            {
                if (BeginCommands())
                {
                    // Deliberately copy only the LEFT half: a split screen in the game is
                    // unambiguous visual proof that the host's output reaches the screen.
                    D3D12_TEXTURE_COPY_LOCATION src = {}, dst = {};
                    src.pResource = h.tex[FEED_COLOR];
                    src.Type      = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                    dst.pResource = h.tex[FEED_OUTPUT];
                    dst.Type      = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                    D3D12_BOX box = { 0, 0, 0, h.width / 2, h.height, 1 };
                    h.list->CopyTextureRegion(&dst, 0, 0, 0, &src, &box);
                    EndCommands();
                    done = true;
                }
            }
            else
                done = Evaluate(h.tex[FEED_COLOR], h.tex[FEED_OUTPUT], h.tex[FEED_DEPTH], h.tex[FEED_MV],
                                h.width, h.height, fm.reset ? 1 : 0, mvsx, mvsy);

            if (done)
            {
                h.queue->Signal(h.fence_out, fm.n);
                // One warm-up re-create per build: the DLSS 5 add-on misses the very first
                // create (STANDBY latch) when its hooks armed a moment too late.
                if (!warm_done && ++evaluated >= 180)
                {
                    warm_done = true;
                    Log("[host] warm-up: re-creating the feature once");
                    WaitFenceValue(h.fence, h.fence_value, 2000);
                    NVSDK_NGX_Handle *old = h.feature;
                    h.feature = nullptr;
                    NVSDK_NGX_Result rr = NVSDK_NGX_Result_Fail;
                    if (CreateFeature(h.width, h.height, flags_active, &rr)) SafeReleaseFeature(old);
                    else { h.feature = old; Log("[host] keeping the previous feature"); }
                }
            }
            else
                h.fence_out->Signal(fm.n);     // CPU-signal so the game never hangs on us

            if (fm.n <= 3 || (fm.n % 1800) == 0)
                Log("[host] frame %llu evaluated", (unsigned long long)fm.n);
            PumpPresent();
        }
        else
        {
            Log("[host] unknown tag 0x%02X", tag);
            break;
        }
    }
    return 0;
}

// ---------------------------------------------------------------------------

int main(int argc, char **argv)
{
    GetModuleFileNameA(nullptr, g_log_path, MAX_PATH);
    if (char *s = strrchr(g_log_path, '\\'))
        strcpy_s(s + 1, MAX_PATH - (s + 1 - g_log_path), "dlss5-feed-host.log");
    { FILE *f = nullptr; if (fopen_s(&f, g_log_path, "w") == 0 && f) fclose(f); }

    Log("dlss5-feed-host64 (built %s %s)", __DATE__, __TIME__);

    bool  test = false, hide = false;
    DWORD pid = 0;
    for (int i = 1; i < argc; ++i)
    {
        if      (strcmp(argv[i], "--test") == 0) test = true;
        else if (strcmp(argv[i], "--hide") == 0) hide = true;
        else pid = static_cast<DWORD>(strtoul(argv[i], nullptr, 10));
    }
    if (!test && pid == 0)
    {
        Log("usage: dlss5-feed-host64 --test | dlss5-feed-host64 <game pid> [--hide]");
        return 1;
    }
    g_show_window = !test && !hide;   // the visible window carries the DLSS 5 add-on's tuning panel

    if (!InitDisguise()) return 1;
    if (!InitNgx()) { Log("[host] NGX unavailable"); return 1; }

    return test ? RunTest() : Serve(pid);
}
