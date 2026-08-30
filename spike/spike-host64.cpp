// Phase-0 spike, 64-bit half: create a shared D3D12 fence and hand it to the 32-bit
// client; receive the client's D3D11-created shared texture (the direction
// MakeSharedPair actually uses), open it on D3D12, wait for the client's D3D11
// fence signal, read the texture back and verify the pattern.
//
// PASS proves the exact cross-process transport dlss5-feed.addon32 + host64 need.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <cstdio>
#include <cstdint>

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")

static const char *kPipeName = "\\\\.\\pipe\\dlss5-feed-spike";
static const UINT  kSize     = 64;
static const BYTE  kPattern  = 0xAB;

#define CHECK(hr, what)                                                     \
    if (FAILED(hr)) { printf("HOST FAIL: %s -> 0x%08lX\n", what, (unsigned long)(hr)); return 1; }

int main()
{
    printf("spike-host64 (pid %lu)\n", GetCurrentProcessId());

    ID3D12Device *dev = nullptr;
    HRESULT hr = D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device),
                                   reinterpret_cast<void **>(&dev));
    CHECK(hr, "D3D12CreateDevice");

    ID3D12Fence *fence = nullptr;
    hr = dev->CreateFence(0, D3D12_FENCE_FLAG_SHARED, __uuidof(ID3D12Fence), reinterpret_cast<void **>(&fence));
    CHECK(hr, "CreateFence(shared)");
    HANDLE hfence = nullptr;
    hr = dev->CreateSharedHandle(fence, nullptr, GENERIC_ALL, nullptr, &hfence);
    CHECK(hr, "CreateSharedHandle(fence)");

    HANDLE pipe = CreateNamedPipeA(kPipeName, PIPE_ACCESS_DUPLEX, PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
                                   1, 256, 256, 0, nullptr);
    if (pipe == INVALID_HANDLE_VALUE) { printf("HOST FAIL: CreateNamedPipe -> %lu\n", GetLastError()); return 1; }
    printf("host: waiting for the 32-bit client on %s ...\n", kPipeName);
    if (!ConnectNamedPipe(pipe, nullptr) && GetLastError() != ERROR_PIPE_CONNECTED)
    { printf("HOST FAIL: ConnectNamedPipe -> %lu\n", GetLastError()); return 1; }

    DWORD client_pid = 0, got = 0;
    if (!ReadFile(pipe, &client_pid, sizeof(client_pid), &got, nullptr) || got != sizeof(client_pid))
    { printf("HOST FAIL: reading client pid\n"); return 1; }
    printf("host: client pid %lu\n", client_pid);

    HANDLE hclient = OpenProcess(PROCESS_DUP_HANDLE, FALSE, client_pid);
    if (hclient == nullptr) { printf("HOST FAIL: OpenProcess -> %lu\n", GetLastError()); return 1; }

    // Fence goes host -> client (duplicated into the client).
    HANDLE remote_fence = nullptr;
    if (!DuplicateHandle(GetCurrentProcess(), hfence, hclient, &remote_fence, 0, FALSE, DUPLICATE_SAME_ACCESS))
    { printf("HOST FAIL: DuplicateHandle(fence) -> %lu\n", GetLastError()); return 1; }
    uint64_t fence_val = reinterpret_cast<uint64_t>(remote_fence);
    DWORD put = 0;
    if (!WriteFile(pipe, &fence_val, sizeof(fence_val), &put, nullptr))
    { printf("HOST FAIL: sending the fence handle\n"); return 1; }
    printf("host: fence handle sent (%p in client)\n", remote_fence);

    // Texture comes client -> host: the client created it on D3D11, we duplicate the
    // handle OUT of the client and open it on D3D12 (MakeSharedPair's working branch).
    uint64_t tex_val = 0;
    if (!ReadFile(pipe, &tex_val, sizeof(tex_val), &got, nullptr) || got != sizeof(tex_val))
    { printf("HOST FAIL: receiving the texture handle\n"); return 1; }
    HANDLE local_tex = nullptr;
    if (!DuplicateHandle(hclient, reinterpret_cast<HANDLE>(static_cast<uintptr_t>(tex_val)),
                         GetCurrentProcess(), &local_tex, 0, FALSE, DUPLICATE_SAME_ACCESS))
    { printf("HOST FAIL: DuplicateHandle(texture out of client) -> %lu\n", GetLastError()); return 1; }

    ID3D12Resource *tex = nullptr;
    hr = dev->OpenSharedHandle(local_tex, __uuidof(ID3D12Resource), reinterpret_cast<void **>(&tex));
    CHECK(hr, "OpenSharedHandle (32-bit D3D11 texture into 64-bit D3D12)");
    D3D12_RESOURCE_DESC rd = tex->GetDesc();
    printf("host: shared texture opened on D3D12: %ux%u format=%u flags=0x%X\n",
           (UINT)rd.Width, rd.Height, rd.Format, rd.Flags);

    // Wait for the client's D3D11 Signal(1).
    HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    fence->SetEventOnCompletion(1, ev);
    if (WaitForSingleObject(ev, 15000) != WAIT_OBJECT_0)
    { printf("HOST FAIL: shared fence was never signalled by the 32-bit client\n"); return 1; }
    printf("host: shared fence reached 1 (signalled from 32-bit D3D11)\n");

    // Read the texture back on D3D12 and verify the client's pattern.
    D3D12_COMMAND_QUEUE_DESC qd = {};
    ID3D12CommandQueue *queue = nullptr;
    ID3D12CommandAllocator *alloc = nullptr;
    ID3D12GraphicsCommandList *list = nullptr;
    dev->CreateCommandQueue(&qd, __uuidof(ID3D12CommandQueue), reinterpret_cast<void **>(&queue));
    dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, __uuidof(ID3D12CommandAllocator),
                                reinterpret_cast<void **>(&alloc));
    dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, nullptr,
                           __uuidof(ID3D12GraphicsCommandList), reinterpret_cast<void **>(&list));
    if (queue == nullptr || alloc == nullptr || list == nullptr)
    { printf("HOST FAIL: queue/list creation\n"); return 1; }

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp = {};
    UINT64 total = 0;
    dev->GetCopyableFootprints(&rd, 0, 1, 0, &fp, nullptr, nullptr, &total);

    D3D12_HEAP_PROPERTIES rbhp = {};
    rbhp.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC bd = {};
    bd.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width            = total;
    bd.Height           = 1;
    bd.DepthOrArraySize = 1;
    bd.MipLevels        = 1;
    bd.SampleDesc.Count = 1;
    bd.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource *rb = nullptr;
    hr = dev->CreateCommittedResource(&rbhp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST,
                                      nullptr, __uuidof(ID3D12Resource), reinterpret_cast<void **>(&rb));
    CHECK(hr, "readback buffer");

    D3D12_TEXTURE_COPY_LOCATION src = {}, dst = {};
    src.pResource = tex;
    src.Type      = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.pResource = rb;
    dst.Type      = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint = fp;
    list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    list->Close();
    ID3D12CommandList *lists[] = { list };
    queue->ExecuteCommandLists(1, lists);
    queue->Signal(fence, 2);
    fence->SetEventOnCompletion(2, ev);
    if (WaitForSingleObject(ev, 5000) != WAIT_OBJECT_0)
    { printf("HOST FAIL: readback copy never completed\n"); return 1; }

    BYTE *bytes = nullptr;
    hr = rb->Map(0, nullptr, reinterpret_cast<void **>(&bytes));
    CHECK(hr, "Map(readback)");
    int bad = 0;
    for (UINT y = 0; y < kSize && bad == 0; ++y)
        for (UINT x = 0; x < kSize * 4; ++x)
            if (bytes[y * fp.Footprint.RowPitch + x] != kPattern) { bad = 1; break; }

    BYTE ack = 1;
    WriteFile(pipe, &ack, 1, &put, nullptr);   // release the client

    if (bad)
    {
        printf("HOST FAIL: pattern mismatch (first row: %02X %02X %02X %02X)\n",
               bytes[0], bytes[1], bytes[2], bytes[3]);
        return 1;
    }

    printf("SPIKE PASS: 32-bit D3D11 created+wrote the shared texture, 64-bit D3D12 opened+read it, and the shared fence crossed both processes.\n");
    return 0;
}
