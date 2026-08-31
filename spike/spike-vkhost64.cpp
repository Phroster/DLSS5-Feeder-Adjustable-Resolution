// Phase-0 spike for the 32-bit Vulkan transport (PLAN-VULKAN32 §Phase 0): the
// 64-bit D3D12 half, standing in for dlss5-feed-host64.exe.
//
//   D3D12  creates a shared texture and two shared fences, duplicates all three
//          INTO the 32-bit client, writes pattern A, Signal(fence_in, 1)
//   Vulkan (spike-vkclient32.exe) waits fence_in >= 1 on its queue, reads the
//          texture back, verifies A, writes pattern B, Signal(fence_out, 1)
//   D3D12  waits fence_out >= 1, reads back, verifies B
//
// The direction is forced and is the whole point of the spike: D3D12 cannot open
// memory that Vulkan exported, so the host must be the creator -- the same design A
// the OpenGL path already ships (see spike-gl64.cpp), now with a Vulkan importer.
//
// Run this first; it waits for the client:
//   spike-vkhost64.exe        (then, in another console) spike-vkclient32.exe
//
// A PASS here answers the questions the plan lists as gating everything else:
// whether the 32-bit ICD exposes the interop extensions at all, whether a D3D12
// committed texture imports at 32-bit (with and without storage usage), and whether
// the lowest-set-bit memory-type heuristic in feed_vk.h picks a usable type.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <cstdio>
#include <cstdint>
#include <cstring>

#include "spike-vk-share.h"

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")

static const BYTE kPatternA = 0xAB;   // written by D3D12
static const BYTE kPatternB = 0x5C;   // written by Vulkan

#define CHECK(hr, what) \
    if (FAILED(hr)) { printf("FAIL: %s -> 0x%08lX\n", what, (unsigned long)(hr)); return 1; }

struct D12
{
    ID3D12Device              *dev;
    ID3D12CommandQueue        *queue;
    ID3D12CommandAllocator    *alloc;
    ID3D12GraphicsCommandList *list;
    ID3D12Fence               *fence_in, *fence_out, *fence_local;
    HANDLE                     h_in, h_out, h_tex, ev;
    ID3D12Resource            *tex;
    D3D12_RESOURCE_DESC        rd;
    UINT64                     alloc_size;
    UINT64                     local_val;
};

static int MakeD12(D12 *d)
{
    *d = {};
    HRESULT hr = D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device),
                                   reinterpret_cast<void **>(&d->dev));
    CHECK(hr, "D3D12CreateDevice");

    D3D12_COMMAND_QUEUE_DESC qd = {};
    d->dev->CreateCommandQueue(&qd, __uuidof(ID3D12CommandQueue), reinterpret_cast<void **>(&d->queue));
    d->dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, __uuidof(ID3D12CommandAllocator),
                                   reinterpret_cast<void **>(&d->alloc));
    d->dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, d->alloc, nullptr,
                              __uuidof(ID3D12GraphicsCommandList), reinterpret_cast<void **>(&d->list));
    if (d->queue == nullptr || d->alloc == nullptr || d->list == nullptr) { printf("FAIL: D3D12 queue/list\n"); return 1; }
    d->list->Close();

    hr = d->dev->CreateFence(0, D3D12_FENCE_FLAG_SHARED, __uuidof(ID3D12Fence), reinterpret_cast<void **>(&d->fence_in));
    CHECK(hr, "CreateFence(in, shared)");
    hr = d->dev->CreateSharedHandle(d->fence_in, nullptr, GENERIC_ALL, nullptr, &d->h_in);
    CHECK(hr, "CreateSharedHandle(fence in)");
    hr = d->dev->CreateFence(0, D3D12_FENCE_FLAG_SHARED, __uuidof(ID3D12Fence), reinterpret_cast<void **>(&d->fence_out));
    CHECK(hr, "CreateFence(out, shared)");
    hr = d->dev->CreateSharedHandle(d->fence_out, nullptr, GENERIC_ALL, nullptr, &d->h_out);
    CHECK(hr, "CreateSharedHandle(fence out)");
    hr = d->dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), reinterpret_cast<void **>(&d->fence_local));
    CHECK(hr, "CreateFence(local)");
    d->ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    // Byte for byte what MakeSharedTexHost creates for a host-creating client.
    D3D12_HEAP_PROPERTIES hp = {};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    d->rd.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d->rd.Width            = kSpikeVkSize;
    d->rd.Height           = kSpikeVkSize;
    d->rd.DepthOrArraySize = 1;
    d->rd.MipLevels        = 1;
    d->rd.Format           = DXGI_FORMAT_R8G8B8A8_UNORM;
    d->rd.SampleDesc.Count = 1;
    d->rd.Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    d->rd.Flags            = D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    hr = d->dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_SHARED, &d->rd, D3D12_RESOURCE_STATE_COMMON,
                                         nullptr, __uuidof(ID3D12Resource), reinterpret_cast<void **>(&d->tex));
    CHECK(hr, "CreateCommittedResource(shared texture)");
    hr = d->dev->CreateSharedHandle(d->tex, nullptr, GENERIC_ALL, nullptr, &d->h_tex);
    CHECK(hr, "CreateSharedHandle(texture)");
    d->alloc_size = d->dev->GetResourceAllocationInfo(0, 1, &d->rd).SizeInBytes;
    printf("d3d12: %ux%u R8G8B8A8_UNORM shared, allocation %llu bytes\n",
           kSpikeVkSize, kSpikeVkSize, (unsigned long long)d->alloc_size);
    return 0;
}

static int D12WritePattern(D12 *d, uint64_t n)
{
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp = {};
    UINT64 total = 0;
    d->dev->GetCopyableFootprints(&d->rd, 0, 1, 0, &fp, nullptr, nullptr, &total);

    D3D12_HEAP_PROPERTIES uhp = {};
    uhp.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC bd = {};
    bd.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width            = total;
    bd.Height           = 1;
    bd.DepthOrArraySize = 1;
    bd.MipLevels        = 1;
    bd.SampleDesc.Count = 1;
    bd.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource *up = nullptr;
    HRESULT hr = d->dev->CreateCommittedResource(&uhp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ,
                                                 nullptr, __uuidof(ID3D12Resource), reinterpret_cast<void **>(&up));
    CHECK(hr, "upload buffer");
    BYTE *p = nullptr;
    hr = up->Map(0, nullptr, reinterpret_cast<void **>(&p));
    CHECK(hr, "Map(upload)");
    memset(p, kPatternA, static_cast<size_t>(total));
    up->Unmap(0, nullptr);

    d->alloc->Reset();
    d->list->Reset(d->alloc, nullptr);
    D3D12_TEXTURE_COPY_LOCATION src = {}, dst = {};
    src.pResource = up;     src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; src.PlacedFootprint = fp;
    dst.pResource = d->tex; dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    d->list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    d->list->Close();
    ID3D12CommandList *ls[] = { d->list };
    d->queue->ExecuteCommandLists(1, ls);
    d->queue->Signal(d->fence_in, n);

    d->queue->Signal(d->fence_local, ++d->local_val);
    d->fence_local->SetEventOnCompletion(d->local_val, d->ev);
    WaitForSingleObject(d->ev, 5000);
    up->Release();
    printf("d3d12: wrote 0x%02X everywhere and signalled the first fence to %llu\n",
           kPatternA, (unsigned long long)n);
    return 0;
}

static int D12VerifyAnswer(D12 *d, uint64_t n)
{
    if (d->fence_out->GetCompletedValue() < n)
    {
        d->fence_out->SetEventOnCompletion(n, d->ev);
        if (WaitForSingleObject(d->ev, 30000) != WAIT_OBJECT_0)
        { printf("FAIL: the Vulkan side never signalled the second fence\n"); return 1; }
    }
    printf("d3d12: the second fence reached %llu (signalled from Vulkan, in the 32-bit process)\n",
           (unsigned long long)n);

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp = {};
    UINT64 total = 0;
    d->dev->GetCopyableFootprints(&d->rd, 0, 1, 0, &fp, nullptr, nullptr, &total);
    D3D12_HEAP_PROPERTIES rhp = {};
    rhp.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC bd = {};
    bd.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width            = total;
    bd.Height           = 1;
    bd.DepthOrArraySize = 1;
    bd.MipLevels        = 1;
    bd.SampleDesc.Count = 1;
    bd.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource *rb = nullptr;
    HRESULT hr = d->dev->CreateCommittedResource(&rhp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST,
                                                 nullptr, __uuidof(ID3D12Resource), reinterpret_cast<void **>(&rb));
    CHECK(hr, "readback buffer");

    d->alloc->Reset();
    d->list->Reset(d->alloc, nullptr);
    D3D12_TEXTURE_COPY_LOCATION src = {}, dst = {};
    src.pResource = d->tex; src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.pResource = rb;     dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; dst.PlacedFootprint = fp;
    d->list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    d->list->Close();
    ID3D12CommandList *ls[] = { d->list };
    d->queue->Wait(d->fence_out, n);
    d->queue->ExecuteCommandLists(1, ls);
    d->queue->Signal(d->fence_local, ++d->local_val);
    d->fence_local->SetEventOnCompletion(d->local_val, d->ev);
    if (WaitForSingleObject(d->ev, 5000) != WAIT_OBJECT_0) { printf("FAIL: the readback copy never completed\n"); return 1; }

    BYTE *bytes = nullptr;
    hr = rb->Map(0, nullptr, reinterpret_cast<void **>(&bytes));
    CHECK(hr, "Map(readback)");
    for (UINT y = 0; y < kSpikeVkSize; ++y)
        for (UINT x = 0; x < kSpikeVkSize * 4; ++x)
            if (bytes[y * fp.Footprint.RowPitch + x] != kPatternB)
            {
                printf("FAIL: D3D12 read 0x%02X at (%u,%u), expected 0x%02X everywhere\n",
                       bytes[y * fp.Footprint.RowPitch + x], x, y, kPatternB);
                return 1;
            }
    rb->Release();
    printf("d3d12: PASS -- Vulkan's write came back through the same memory\n");
    return 0;
}

int main()
{
    printf("spike-vkhost64: D3D12 host for the 32-bit Vulkan client (pid %lu)\n", GetCurrentProcessId());

    D12 d;
    if (const int rc = MakeD12(&d)) return rc;

    HANDLE pipe = CreateNamedPipeA(kSpikeVkPipe, PIPE_ACCESS_DUPLEX, PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
                                   1, 256, 256, 0, nullptr);
    if (pipe == INVALID_HANDLE_VALUE) { printf("FAIL: CreateNamedPipe -> %lu\n", GetLastError()); return 1; }
    printf("host: waiting for spike-vkclient32.exe on %s ...\n", kSpikeVkPipe);
    if (!ConnectNamedPipe(pipe, nullptr) && GetLastError() != ERROR_PIPE_CONNECTED)
    { printf("FAIL: ConnectNamedPipe -> %lu\n", GetLastError()); return 1; }

    DWORD client_pid = 0, got = 0, put = 0;
    if (!ReadFile(pipe, &client_pid, sizeof(client_pid), &got, nullptr) || got != sizeof(client_pid))
    { printf("FAIL: reading the client pid\n"); return 1; }
    HANDLE hclient = OpenProcess(PROCESS_DUP_HANDLE, FALSE, client_pid);
    if (hclient == nullptr) { printf("FAIL: OpenProcess(%lu) -> %lu\n", client_pid, GetLastError()); return 1; }
    printf("host: client pid %lu\n", client_pid);

    SpikeVkShare s = {};
    s.width    = kSpikeVkSize;
    s.height   = kSpikeVkSize;
    s.tex_size = d.alloc_size;
    s.pattern_a = kPatternA;
    s.pattern_b = kPatternB;
    HANDLE r_tex = nullptr, r_in = nullptr, r_out = nullptr;
    if (!DuplicateHandle(GetCurrentProcess(), d.h_tex, hclient, &r_tex, 0, FALSE, DUPLICATE_SAME_ACCESS) ||
        !DuplicateHandle(GetCurrentProcess(), d.h_in,  hclient, &r_in,  0, FALSE, DUPLICATE_SAME_ACCESS) ||
        !DuplicateHandle(GetCurrentProcess(), d.h_out, hclient, &r_out, 0, FALSE, DUPLICATE_SAME_ACCESS))
    { printf("FAIL: DuplicateHandle into the client -> %lu\n", GetLastError()); return 1; }
    s.tex       = reinterpret_cast<uint64_t>(r_tex);
    s.fence_in  = reinterpret_cast<uint64_t>(r_in);
    s.fence_out = reinterpret_cast<uint64_t>(r_out);
    if (!WriteFile(pipe, &s, sizeof(s), &put, nullptr) || put != sizeof(s))
    { printf("FAIL: sending the handles\n"); return 1; }
    printf("host: texture + both fences duplicated into the client\n");

    // Wait for the client to import and put the image into VK_IMAGE_LAYOUT_GENERAL
    // BEFORE anything is written into it. That ordering is the real one: the add-on's
    // UNDEFINED -> GENERAL transition happens on the first frame after a build, ahead
    // of the host's first write, and UNDEFINED is allowed to discard what is there.
    BYTE ready = 0;
    if (!ReadFile(pipe, &ready, 1, &got, nullptr) || got != 1 || ready != 1)
    { printf("FAIL: the client never reported a successful import\n"); return 1; }
    printf("host: the client has imported everything and is in GENERAL layout\n");

    if (const int rc = D12WritePattern(&d, 1)) return rc;
    BYTE go = 1;
    WriteFile(pipe, &go, 1, &put, nullptr);

    if (const int rc = D12VerifyAnswer(&d, 1)) return rc;
    printf("SPIKE PASS: 64-bit D3D12 created the shared texture and fences, a 32-bit Vulkan process "
           "imported them cross-process, and the frame counter crossed in both directions.\n");
    return 0;
}
