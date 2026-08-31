// Phase-0 spike, 32-bit half: connect to the 64-bit host, receive the shared-fence
// handle, CREATE the shared texture on D3D11 (the direction MakeSharedPair actually
// uses on this driver: D3D11 -> D3D12), send its NT handle back, fill the texture
// with a pattern and signal the fence from D3D11.
//
// Mirrors exactly what dlss5-feed.addon32 will do inside a 32-bit game.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11_4.h>
#include <dxgi1_2.h>
#include <cstdio>
#include <cstdint>
#include <vector>

#pragma comment(lib, "d3d11.lib")

static const char *kPipeName = "\\\\.\\pipe\\dlss5-feed-spike";
static const UINT  kSize     = 64;
static const BYTE  kPattern  = 0xAB;

#define CHECK(hr, what)                                                     \
    if (FAILED(hr)) { printf("CLIENT FAIL: %s -> 0x%08lX\n", what, (unsigned long)(hr)); return 1; }

int main()
{
    printf("spike-client32 (pid %lu, %u-bit)\n", GetCurrentProcessId(), (unsigned)(sizeof(void *) * 8));

    HANDLE pipe = INVALID_HANDLE_VALUE;
    for (int i = 0; i < 100 && pipe == INVALID_HANDLE_VALUE; ++i)   // retry up to 10 s
    {
        pipe = CreateFileA(kPipeName, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (pipe == INVALID_HANDLE_VALUE) Sleep(100);
    }
    if (pipe == INVALID_HANDLE_VALUE) { printf("CLIENT FAIL: pipe open -> %lu\n", GetLastError()); return 1; }

    DWORD pid = GetCurrentProcessId(), put = 0;
    if (!WriteFile(pipe, &pid, sizeof(pid), &put, nullptr)) { printf("CLIENT FAIL: sending pid\n"); return 1; }

    // The host sends the shared FENCE handle (D3D12-created, duplicated into us).
    uint64_t fence_val = 0;
    DWORD got = 0;
    if (!ReadFile(pipe, &fence_val, sizeof(fence_val), &got, nullptr) || got != sizeof(fence_val))
    { printf("CLIENT FAIL: receiving the fence handle\n"); return 1; }
    HANDLE hfence = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(fence_val));
    printf("client: received fence=%p\n", hfence);

    ID3D11Device *dev = nullptr;
    ID3D11DeviceContext *ctx = nullptr;
    D3D_FEATURE_LEVEL fl = {};
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
                                   D3D11_SDK_VERSION, &dev, &fl, &ctx);
    CHECK(hr, "D3D11CreateDevice");
    printf("client: D3D11 device up (feature level 0x%X)\n", fl);

    // Open the 64-bit D3D12 fence on 32-bit D3D11.
    ID3D11Device5 *dev5 = nullptr;
    hr = dev->QueryInterface(__uuidof(ID3D11Device5), reinterpret_cast<void **>(&dev5));
    CHECK(hr, "QI ID3D11Device5");
    ID3D11Fence *fence = nullptr;
    hr = dev5->OpenSharedFence(hfence, __uuidof(ID3D11Fence), reinterpret_cast<void **>(&fence));
    CHECK(hr, "OpenSharedFence (64-bit D3D12 fence into 32-bit D3D11)");
    printf("client: shared fence opened\n");

    // Create the shared texture on D3D11 -- the production direction of MakeSharedPair.
    D3D11_TEXTURE2D_DESC td = {};
    td.Width            = kSize;
    td.Height           = kSize;
    td.MipLevels        = 1;
    td.ArraySize        = 1;
    td.Format           = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage            = D3D11_USAGE_DEFAULT;
    td.BindFlags        = D3D11_BIND_SHADER_RESOURCE;
    td.MiscFlags        = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED;
    ID3D11Texture2D *tex = nullptr;
    hr = dev->CreateTexture2D(&td, nullptr, &tex);
    CHECK(hr, "CreateTexture2D(shared NT handle)");

    IDXGIResource1 *dxgi_res = nullptr;
    hr = tex->QueryInterface(__uuidof(IDXGIResource1), reinterpret_cast<void **>(&dxgi_res));
    CHECK(hr, "QI IDXGIResource1");
    HANDLE htex = nullptr;
    hr = dxgi_res->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &htex);
    CHECK(hr, "IDXGIResource1::CreateSharedHandle");
    printf("client: created shared texture, local handle %p\n", htex);

    // Send the handle VALUE; the host duplicates it out of our process.
    uint64_t tex_val = reinterpret_cast<uintptr_t>(htex);
    if (!WriteFile(pipe, &tex_val, sizeof(tex_val), &put, nullptr))
    { printf("CLIENT FAIL: sending the texture handle\n"); return 1; }

    // Fill the shared texture from the 32-bit side and signal the fence from D3D11.
    std::vector<BYTE> px(kSize * kSize * 4, kPattern);
    ctx->UpdateSubresource(tex, 0, nullptr, px.data(), kSize * 4, 0);

    ID3D11DeviceContext4 *ctx4 = nullptr;
    hr = ctx->QueryInterface(__uuidof(ID3D11DeviceContext4), reinterpret_cast<void **>(&ctx4));
    CHECK(hr, "QI ID3D11DeviceContext4");
    hr = ctx4->Signal(fence, 1);
    CHECK(hr, "ID3D11DeviceContext4::Signal");
    ctx->Flush();

    // Hold the process (and its texture) alive until the host confirms the readback.
    BYTE ack = 0;
    ReadFile(pipe, &ack, 1, &got, nullptr);

    printf("client: pattern 0x%02X written and fence signalled to 1 -- done\n", kPattern);
    return 0;
}
