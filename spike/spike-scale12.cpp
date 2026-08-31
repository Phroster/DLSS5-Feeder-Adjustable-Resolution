// Standalone smoke test for src/feed_scale12.h.
//
// This deliberately does not create an NGX feature. It builds the private 50%
// work set from bridge resources shaped like the Vulkan/OpenGL transports,
// records Prepare, clears the would-be NGX output, records Finish, submits the
// list, and proves that the native output is black and the device stayed alive.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <cstdio>
#include <cstring>
#include <vector>

#include "../src/feed_scale12.h"

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")

using Microsoft::WRL::ComPtr;

static const UINT kNativeWidth = 128;
static const UINT kNativeHeight = 72;
static const UINT kWorkWidth = kNativeWidth / 2;
static const UINT kWorkHeight = kNativeHeight / 2;

static bool Failed(HRESULT hr, const char *what)
{
    if (SUCCEEDED(hr)) return false;
    std::printf("FAIL: %s -> 0x%08lX\n", what, static_cast<unsigned long>(hr));
    return true;
}

static void Barrier(ID3D12GraphicsCommandList *list, ID3D12Resource *resource,
                    D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
{
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    list->ResourceBarrier(1, &barrier);
}

static HRESULT CreateBridgeTexture(ID3D12Device *device, DXGI_FORMAT format,
                                   D3D12_RESOURCE_FLAGS extra_flags,
                                   ID3D12Resource **resource)
{
    D3D12_HEAP_PROPERTIES heap = {};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = kNativeWidth;
    desc.Height = kNativeHeight;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS | extra_flags;

    HRESULT hr = device->CreateCommittedResource(
        &heap, D3D12_HEAP_FLAG_SHARED, &desc, D3D12_RESOURCE_STATE_COMMON,
        nullptr, __uuidof(ID3D12Resource), reinterpret_cast<void **>(resource));
    if (FAILED(hr)) return hr;

    // Also prove this really is a usable bridge allocation, rather than merely a
    // private texture that happens to have compatible dimensions and formats.
    HANDLE shared = nullptr;
    hr = device->CreateSharedHandle(*resource, nullptr, GENERIC_ALL, nullptr, &shared);
    if (SUCCEEDED(hr)) CloseHandle(shared);
    return hr;
}

static HRESULT RecordUpload(ID3D12Device *device, ID3D12GraphicsCommandList *list,
                            ID3D12Resource *texture, BYTE fill,
                            std::vector<ComPtr<ID3D12Resource>> *uploads)
{
    const D3D12_RESOURCE_DESC texture_desc = texture->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
    UINT64 bytes = 0;
    device->GetCopyableFootprints(&texture_desc, 0, 1, 0, &footprint, nullptr, nullptr, &bytes);

    D3D12_HEAP_PROPERTIES heap = {};
    heap.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC buffer_desc = {};
    buffer_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer_desc.Width = bytes;
    buffer_desc.Height = 1;
    buffer_desc.DepthOrArraySize = 1;
    buffer_desc.MipLevels = 1;
    buffer_desc.SampleDesc.Count = 1;
    buffer_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    ComPtr<ID3D12Resource> upload;
    HRESULT hr = device->CreateCommittedResource(
        &heap, D3D12_HEAP_FLAG_NONE, &buffer_desc, D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr, __uuidof(ID3D12Resource), reinterpret_cast<void **>(upload.GetAddressOf()));
    if (FAILED(hr)) return hr;

    BYTE *mapped = nullptr;
    D3D12_RANGE no_read = { 0, 0 };
    hr = upload->Map(0, &no_read, reinterpret_cast<void **>(&mapped));
    if (FAILED(hr)) return hr;
    std::memset(mapped, fill, static_cast<size_t>(bytes));
    upload->Unmap(0, nullptr);

    D3D12_TEXTURE_COPY_LOCATION source = {};
    source.pResource = upload.Get();
    source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    source.PlacedFootprint = footprint;
    D3D12_TEXTURE_COPY_LOCATION destination = {};
    destination.pResource = texture;
    destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;

    Barrier(list, texture, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
    list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
    Barrier(list, texture, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
    uploads->push_back(upload);
    return S_OK;
}

static bool WaitForGpu(ID3D12CommandQueue *queue, ID3D12Fence *fence,
                       HANDLE event_handle, UINT64 value)
{
    HRESULT hr = queue->Signal(fence, value);
    if (Failed(hr, "ID3D12CommandQueue::Signal")) return false;
    hr = fence->SetEventOnCompletion(value, event_handle);
    if (Failed(hr, "ID3D12Fence::SetEventOnCompletion")) return false;
    if (WaitForSingleObject(event_handle, 10000) != WAIT_OBJECT_0)
    {
        std::printf("FAIL: GPU completion timed out\n");
        return false;
    }
    return true;
}

int main()
{
    ComPtr<ID3D12Debug> debug;
    if (SUCCEEDED(D3D12GetDebugInterface(__uuidof(ID3D12Debug),
                                         reinterpret_cast<void **>(debug.GetAddressOf()))))
    {
        debug->EnableDebugLayer();
        std::printf("scale12: D3D12 debug layer enabled\n");
    }

    ComPtr<ID3D12Device> device;
    HRESULT hr = D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0,
                                   __uuidof(ID3D12Device),
                                   reinterpret_cast<void **>(device.GetAddressOf()));
    if (Failed(hr, "D3D12CreateDevice")) return 1;

    D3D12_COMMAND_QUEUE_DESC queue_desc = {};
    queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueue> queue;
    hr = device->CreateCommandQueue(&queue_desc, __uuidof(ID3D12CommandQueue),
                                    reinterpret_cast<void **>(queue.GetAddressOf()));
    if (Failed(hr, "CreateCommandQueue")) return 1;

    ComPtr<ID3D12CommandAllocator> allocator;
    hr = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                        __uuidof(ID3D12CommandAllocator),
                                        reinterpret_cast<void **>(allocator.GetAddressOf()));
    if (Failed(hr, "CreateCommandAllocator")) return 1;

    ComPtr<ID3D12GraphicsCommandList> list;
    hr = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                                   __uuidof(ID3D12GraphicsCommandList),
                                   reinterpret_cast<void **>(list.GetAddressOf()));
    if (Failed(hr, "CreateCommandList")) return 1;

    ComPtr<ID3D12Fence> fence;
    hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence),
                             reinterpret_cast<void **>(fence.GetAddressOf()));
    if (Failed(hr, "CreateFence")) return 1;
    HANDLE event_handle = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (event_handle == nullptr)
    {
        std::printf("FAIL: CreateEventW -> %lu\n", GetLastError());
        return 1;
    }

    ComPtr<ID3D12Resource> color, output, depth, motion, mask;
    if (Failed(CreateBridgeTexture(device.Get(), DXGI_FORMAT_R8G8B8A8_UNORM,
                                   D3D12_RESOURCE_FLAG_NONE, color.GetAddressOf()), "Color bridge") ||
        Failed(CreateBridgeTexture(device.Get(), DXGI_FORMAT_R8G8B8A8_UNORM,
                                   D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS |
                                   D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
                                   output.GetAddressOf()), "Output bridge") ||
        Failed(CreateBridgeTexture(device.Get(), DXGI_FORMAT_R32_FLOAT,
                                   D3D12_RESOURCE_FLAG_NONE, depth.GetAddressOf()), "Depth bridge") ||
        Failed(CreateBridgeTexture(device.Get(), DXGI_FORMAT_R16G16_FLOAT,
                                   D3D12_RESOURCE_FLAG_NONE, motion.GetAddressOf()), "MV bridge") ||
        Failed(CreateBridgeTexture(device.Get(), DXGI_FORMAT_R8_UNORM,
                                   D3D12_RESOURCE_FLAG_NONE, mask.GetAddressOf()), "Mask bridge"))
    {
        CloseHandle(event_handle);
        return 1;
    }

    // Initialize every sampled bridge input. Seed the native output with nonzero
    // bytes so the final readback proves Finish actually overwrote it.
    std::vector<ComPtr<ID3D12Resource>> uploads;
    if (Failed(RecordUpload(device.Get(), list.Get(), color.Get(), 0, &uploads), "Upload Color") ||
        Failed(RecordUpload(device.Get(), list.Get(), output.Get(), 0x7F, &uploads), "Upload Output") ||
        Failed(RecordUpload(device.Get(), list.Get(), depth.Get(), 0, &uploads), "Upload Depth") ||
        Failed(RecordUpload(device.Get(), list.Get(), motion.Get(), 0, &uploads), "Upload MV") ||
        Failed(RecordUpload(device.Get(), list.Get(), mask.Get(), 0, &uploads), "Upload Mask") ||
        Failed(list->Close(), "Close initialization list"))
    {
        CloseHandle(event_handle);
        return 1;
    }
    ID3D12CommandList *initialization_lists[] = { list.Get() };
    queue->ExecuteCommandLists(1, initialization_lists);
    if (!WaitForGpu(queue.Get(), fence.Get(), event_handle, 1))
    {
        CloseHandle(event_handle);
        return 1;
    }
    uploads.clear();

    feed_scale12::Desc desc = {};
    desc.device = device.Get();
    desc.native_color = color.Get();
    desc.native_output = output.Get();
    desc.native_depth = depth.Get();
    desc.native_mv = motion.Get();
    desc.native_mask = mask.Get();
    desc.native_width = kNativeWidth;
    desc.native_height = kNativeHeight;
    desc.work_width = kWorkWidth;
    desc.work_height = kWorkHeight;
    desc.color_format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.output_format = DXGI_FORMAT_R8G8B8A8_UNORM;

    // The helper is intentionally bypass-only at 100%; it must reject a scaler
    // build whose native and work dimensions are identical.
    feed_scale12::Context reject_context;
    feed_scale12::Desc native_desc = desc;
    native_desc.work_width = kNativeWidth;
    native_desc.work_height = kNativeHeight;
    if (feed_scale12::FeedScale12Build(&reject_context, native_desc) ||
        reject_context.last_error != E_INVALIDARG || reject_context.ready)
    {
        std::printf("FAIL: 100%% Build was not rejected cleanly (%s, 0x%08lX)\n",
                    reject_context.error, static_cast<unsigned long>(reject_context.last_error));
        feed_scale12::FeedScale12Release(&reject_context);
        CloseHandle(event_handle);
        return 1;
    }
    std::printf("scale12: 100%% Build rejected as expected\n");

    feed_scale12::Context scaler;
    if (!feed_scale12::FeedScale12Build(&scaler, desc))
    {
        std::printf("FAIL: FeedScale12Build -> %s (0x%08lX)\n", scaler.error,
                    static_cast<unsigned long>(scaler.last_error));
        CloseHandle(event_handle);
        return 1;
    }
    for (unsigned slot = 0; slot < feed_scale12::SLOT_COUNT; ++slot)
    {
        ID3D12Resource *work = feed_scale12::FeedScale12Work(&scaler,
            static_cast<feed_scale12::Slot>(slot));
        if (work == nullptr || work->GetDesc().Width != kWorkWidth ||
            work->GetDesc().Height != kWorkHeight)
        {
            std::printf("FAIL: work slot %u has the wrong shape\n", slot);
            feed_scale12::FeedScale12Release(&scaler);
            CloseHandle(event_handle);
            return 1;
        }
    }
    std::printf("scale12: built %ux%u private work resources for %ux%u bridges\n",
                kWorkWidth, kWorkHeight, kNativeWidth, kNativeHeight);

    // One descriptor is enough to clear the work Output in place of NGX.
    D3D12_DESCRIPTOR_HEAP_DESC uav_heap_desc = {};
    uav_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    uav_heap_desc.NumDescriptors = 1;
    uav_heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ComPtr<ID3D12DescriptorHeap> uav_heap;
    hr = device->CreateDescriptorHeap(&uav_heap_desc, __uuidof(ID3D12DescriptorHeap),
                                      reinterpret_cast<void **>(uav_heap.GetAddressOf()));
    if (Failed(hr, "CreateDescriptorHeap(test UAV)"))
    {
        feed_scale12::FeedScale12Release(&scaler);
        CloseHandle(event_handle);
        return 1;
    }
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav_desc = {};
    uav_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    uav_desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    device->CreateUnorderedAccessView(
        feed_scale12::FeedScale12Work(&scaler, feed_scale12::SLOT_OUTPUT),
        nullptr, &uav_desc, uav_heap->GetCPUDescriptorHandleForHeapStart());

    hr = allocator->Reset();
    if (!Failed(hr, "Reset allocator") &&
        !Failed(list->Reset(allocator.Get(), nullptr), "Reset command list") &&
        feed_scale12::FeedScale12RecordPrepare(&scaler, list.Get(), false))
    {
        ID3D12DescriptorHeap *heaps[] = { uav_heap.Get() };
        list->SetDescriptorHeaps(1, heaps);
        const float black[4] = {};
        list->ClearUnorderedAccessViewFloat(
            uav_heap->GetGPUDescriptorHandleForHeapStart(),
            uav_heap->GetCPUDescriptorHandleForHeapStart(),
            feed_scale12::FeedScale12Work(&scaler, feed_scale12::SLOT_OUTPUT),
            black, 0, nullptr);

        if (!feed_scale12::FeedScale12RecordFinish(&scaler, list.Get()))
        {
            std::printf("FAIL: FeedScale12RecordFinish -> %s\n", scaler.error);
            hr = E_FAIL;
        }
        else
            hr = list->Close();
    }
    else
    {
        hr = E_FAIL;
        std::printf("FAIL: FeedScale12RecordPrepare -> %s\n", scaler.error);
    }
    if (FAILED(hr))
    {
        if (hr != E_FAIL) Failed(hr, "Close scaler list");
        feed_scale12::FeedScale12Release(&scaler);
        CloseHandle(event_handle);
        return 1;
    }

    ID3D12CommandList *scale_lists[] = { list.Get() };
    queue->ExecuteCommandLists(1, scale_lists);
    if (!WaitForGpu(queue.Get(), fence.Get(), event_handle, 2))
    {
        feed_scale12::FeedScale12Release(&scaler);
        CloseHandle(event_handle);
        return 1;
    }
    hr = device->GetDeviceRemovedReason();
    if (Failed(hr, "GetDeviceRemovedReason after Prepare/Finish"))
    {
        feed_scale12::FeedScale12Release(&scaler);
        CloseHandle(event_handle);
        return 1;
    }

    // Read back native Output. It started at 0x7F and must now contain the black
    // work-output clear expanded by FeedScale12RecordFinish.
    const D3D12_RESOURCE_DESC output_desc = output->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
    UINT64 readback_bytes = 0;
    device->GetCopyableFootprints(&output_desc, 0, 1, 0, &footprint,
                                  nullptr, nullptr, &readback_bytes);
    D3D12_HEAP_PROPERTIES readback_heap = {};
    readback_heap.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC readback_desc = {};
    readback_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    readback_desc.Width = readback_bytes;
    readback_desc.Height = 1;
    readback_desc.DepthOrArraySize = 1;
    readback_desc.MipLevels = 1;
    readback_desc.SampleDesc.Count = 1;
    readback_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> readback;
    hr = device->CreateCommittedResource(
        &readback_heap, D3D12_HEAP_FLAG_NONE, &readback_desc,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, __uuidof(ID3D12Resource),
        reinterpret_cast<void **>(readback.GetAddressOf()));
    if (Failed(hr, "Create readback buffer"))
    {
        feed_scale12::FeedScale12Release(&scaler);
        CloseHandle(event_handle);
        return 1;
    }

    hr = allocator->Reset();
    if (Failed(hr, "Reset allocator for readback") ||
        Failed(list->Reset(allocator.Get(), nullptr), "Reset command list for readback"))
    {
        feed_scale12::FeedScale12Release(&scaler);
        CloseHandle(event_handle);
        return 1;
    }
    Barrier(list.Get(), output.Get(), D3D12_RESOURCE_STATE_COMMON,
            D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION source = {};
    source.pResource = output.Get();
    source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION destination = {};
    destination.pResource = readback.Get();
    destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    destination.PlacedFootprint = footprint;
    list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
    Barrier(list.Get(), output.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
            D3D12_RESOURCE_STATE_COMMON);
    if (Failed(list->Close(), "Close readback list"))
    {
        feed_scale12::FeedScale12Release(&scaler);
        CloseHandle(event_handle);
        return 1;
    }
    ID3D12CommandList *readback_lists[] = { list.Get() };
    queue->ExecuteCommandLists(1, readback_lists);
    if (!WaitForGpu(queue.Get(), fence.Get(), event_handle, 3))
    {
        feed_scale12::FeedScale12Release(&scaler);
        CloseHandle(event_handle);
        return 1;
    }

    BYTE *bytes = nullptr;
    D3D12_RANGE read_range = { 0, static_cast<SIZE_T>(readback_bytes) };
    hr = readback->Map(0, &read_range, reinterpret_cast<void **>(&bytes));
    if (Failed(hr, "Map readback"))
    {
        feed_scale12::FeedScale12Release(&scaler);
        CloseHandle(event_handle);
        return 1;
    }
    bool black = true;
    for (UINT y = 0; y < kNativeHeight && black; ++y)
        for (UINT x = 0; x < kNativeWidth * 4; ++x)
            if (bytes[static_cast<size_t>(y) * footprint.Footprint.RowPitch + x] != 0)
            {
                black = false;
                break;
            }
    D3D12_RANGE no_write = { 0, 0 };
    readback->Unmap(0, &no_write);

    feed_scale12::FeedScale12Release(&scaler);
    hr = device->GetDeviceRemovedReason();
    CloseHandle(event_handle);
    if (!black)
    {
        std::printf("FAIL: native Output was not replaced by the cleared work Output\n");
        return 1;
    }
    if (Failed(hr, "GetDeviceRemovedReason after cleanup")) return 1;

    std::printf("SPIKE PASS: 50%% D3D12 scaler built, Prepare/clear/Finish executed, "
                "native output verified, GPU completed, device alive, resources released.\n");
    return 0;
}
