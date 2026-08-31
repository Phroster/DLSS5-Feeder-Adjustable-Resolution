// SPDX-License-Identifier: MIT
// Adjustable-resolution additions Copyright (c) 2026 Phroster.
// feed_scale12.h - private-D3D12 work-resolution scaler.
//
// This helper keeps an API transport's shared textures at native resolution and
// creates a separate set of private D3D12 textures at the requested work size.
// That separation is important for Vulkan in particular: its transport can keep
// doing exact native-size copies across UNORM/SRGB-compatible images instead of
// using a scaling vkCmdBlitImage that may decode and re-encode the frame.
//
// Lifetime and state contract
// ---------------------------
//  * Zero/default-initialize Context, then call FeedScale12Build after the
//    native bridge textures have been created.
//  * Every native bridge texture passed to Build is retained until Release.
//    As with the caller's other frame resources, the GPU must be drained before
//    Release destroys a scaler that has recorded submitted work.
//  * All native bridge and private work textures rest in COMMON.
//  * RecordPrepare transitions the native inputs, downsamples them, and leaves
//    work Color/Depth/MV/Mask in NON_PIXEL_SHADER_RESOURCE and work Output in
//    UNORDERED_ACCESS, ready for an NGX evaluate.
//  * RecordFinish is called after NGX. It expands work Output into the native
//    bridge Output and returns every resource to COMMON.
//  * RecordAbort returns the work resources to COMMON without presenting an
//    output. Do not call it when the entire command list is discarded: no
//    recorded transition executed in that case, so the resources are already in
//    COMMON.
//  * The native Output resource must have ALLOW_RENDER_TARGET. Work Output has
//    ALLOW_UNORDERED_ACCESS for NGX.
//
// The header intentionally resolves D3DCompile and D3D12SerializeRootSignature
// at runtime. Including it adds no d3dcompiler.lib or d3d12.lib dependency.

#pragma once

#include <windows.h>
#include <d3d12.h>
#include <dxgiformat.h>
#include <d3dcompiler.h>
#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace feed_scale12
{
enum Slot : unsigned
{
    SLOT_COLOR = 0,
    SLOT_OUTPUT,
    SLOT_DEPTH,
    SLOT_MV,
    SLOT_MASK,
    SLOT_COUNT
};

struct Desc
{
    ID3D12Device   *device        = nullptr;
    ID3D12Resource *native_color  = nullptr;
    ID3D12Resource *native_output = nullptr;
    ID3D12Resource *native_depth  = nullptr;
    ID3D12Resource *native_mv     = nullptr;
    ID3D12Resource *native_mask   = nullptr;

    UINT native_width  = 0;
    UINT native_height = 0;
    UINT work_width    = 0;
    UINT work_height   = 0;

    // Typed views used by the resample shaders. The underlying native resources
    // may be typeless members of the same format family.
    DXGI_FORMAT color_format  = DXGI_FORMAT_UNKNOWN;
    DXGI_FORMAT output_format = DXGI_FORMAT_UNKNOWN;
};

struct Context
{
    ID3D12Device   *device = nullptr;
    ID3D12Resource *native[SLOT_COUNT] = {};
    ID3D12Resource *work[SLOT_COUNT]   = {};

    ID3D12RootSignature *root = nullptr;
    ID3D12PipelineState *downsample_pso = nullptr;
    ID3D12PipelineState *expand_pso     = nullptr;
    ID3D12DescriptorHeap *srv_heap = nullptr;
    ID3D12DescriptorHeap *rtv_heap = nullptr;
    UINT srv_stride = 0;
    UINT rtv_stride = 0;

    UINT native_width  = 0;
    UINT native_height = 0;
    UINT work_width    = 0;
    UINT work_height   = 0;
    DXGI_FORMAT color_format  = DXGI_FORMAT_UNKNOWN;
    DXGI_FORMAT output_format = DXGI_FORMAT_UNKNOWN;

    HRESULT last_error = S_OK;
    char error[512] = {};
    bool ready = false;

    Context() = default;
    Context(const Context &) = delete;
    Context &operator=(const Context &) = delete;
};

template <typename T> static inline void SafeRelease(T *&p)
{
    if (p != nullptr)
    {
        p->Release();
        p = nullptr;
    }
}

static inline void SetError(Context *ctx, HRESULT hr, const char *fmt, ...)
{
    if (ctx == nullptr) return;
    ctx->last_error = hr;
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(ctx->error, sizeof(ctx->error), _TRUNCATE, fmt, ap);
    va_end(ap);
}

static inline void FeedScale12Release(Context *ctx)
{
    if (ctx == nullptr) return;

    SafeRelease(ctx->expand_pso);
    SafeRelease(ctx->downsample_pso);
    SafeRelease(ctx->root);
    SafeRelease(ctx->srv_heap);
    SafeRelease(ctx->rtv_heap);
    for (unsigned i = 0; i < SLOT_COUNT; ++i)
    {
        SafeRelease(ctx->work[i]);
        SafeRelease(ctx->native[i]);
    }
    SafeRelease(ctx->device);

    ctx->srv_stride = 0;
    ctx->rtv_stride = 0;
    ctx->native_width = ctx->native_height = 0;
    ctx->work_width = ctx->work_height = 0;
    ctx->color_format = ctx->output_format = DXGI_FORMAT_UNKNOWN;
    ctx->ready = false;
}

static inline bool ValidateNativeResource(Context *ctx, ID3D12Resource *resource,
                                          UINT width, UINT height, DXGI_FORMAT typed_format,
                                          const char *name, bool require_render_target)
{
    if (resource == nullptr)
    {
        SetError(ctx, E_INVALIDARG, "%s native bridge resource is null", name);
        return false;
    }

    const D3D12_RESOURCE_DESC rd = resource->GetDesc();
    if (rd.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || rd.DepthOrArraySize != 1 ||
        rd.MipLevels != 1 || rd.SampleDesc.Count != 1 || rd.Width != width || rd.Height != height)
    {
        SetError(ctx, E_INVALIDARG,
                 "%s native bridge resource has the wrong shape (got %llux%u, expected %ux%u, one non-MSAA mip)",
                 name, static_cast<unsigned long long>(rd.Width), rd.Height, width, height);
        return false;
    }
    if (require_render_target && (rd.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) == 0)
    {
        SetError(ctx, E_INVALIDARG, "%s native bridge resource is missing ALLOW_RENDER_TARGET", name);
        return false;
    }
    if ((rd.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE) != 0 && !require_render_target)
    {
        SetError(ctx, E_INVALIDARG, "%s native bridge resource denies shader-resource views", name);
        return false;
    }

    // A typeless resource is expected for some backbuffers. Create*View validates
    // the precise family later; reject only an obviously unknown typed request here.
    if (typed_format == DXGI_FORMAT_UNKNOWN)
    {
        SetError(ctx, E_INVALIDARG, "%s typed view format is unknown", name);
        return false;
    }
    return true;
}

static inline bool CheckFormat(Context *ctx, DXGI_FORMAT format,
                               D3D12_FORMAT_SUPPORT1 need1, D3D12_FORMAT_SUPPORT2 need2,
                               const char *name)
{
    D3D12_FEATURE_DATA_FORMAT_SUPPORT fs = {};
    fs.Format = format;
    const HRESULT hr = ctx->device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &fs, sizeof(fs));
    if (FAILED(hr) || (fs.Support1 & need1) != need1 || (fs.Support2 & need2) != need2)
    {
        SetError(ctx, FAILED(hr) ? hr : E_FAIL,
                 "%s format %u lacks required D3D12 support (Support1=0x%X, Support2=0x%X)",
                 name, static_cast<unsigned>(format), static_cast<unsigned>(fs.Support1),
                 static_cast<unsigned>(fs.Support2));
        return false;
    }
    return true;
}

static inline bool MakeWorkTexture(Context *ctx, unsigned slot, DXGI_FORMAT format,
                                   D3D12_RESOURCE_FLAGS flags)
{
    D3D12_HEAP_PROPERTIES hp = {};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC rd = {};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = ctx->work_width;
    rd.Height = ctx->work_height;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.Format = format;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    rd.Flags = flags;

    const HRESULT hr = ctx->device->CreateCommittedResource(
        &hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COMMON, nullptr,
        __uuidof(ID3D12Resource), reinterpret_cast<void **>(&ctx->work[slot]));
    if (FAILED(hr))
    {
        SetError(ctx, hr, "CreateCommittedResource failed for work slot %u (%ux%u format %u)",
                 slot, ctx->work_width, ctx->work_height, static_cast<unsigned>(format));
        return false;
    }
    return true;
}

static inline bool CompileShaders(Context *ctx, ID3DBlob **vs, ID3DBlob **down_ps, ID3DBlob **up_ps)
{
    *vs = *down_ps = *up_ps = nullptr;
    static const char source[] =
        "Texture2D<float4> src_color : register(t0);\n"
        "Texture2D<float2> src_mv : register(t1);\n"
        "Texture2D<float> src_depth : register(t2);\n"
        "Texture2D<float> src_mask : register(t3);\n"
        "SamplerState linear_smp : register(s0);\n"
        "SamplerState point_smp : register(s1);\n"
        "cbuffer ScaleConstants : register(b0) { float2 mv_scale; float mask_valid; float _pad; };\n"
        "struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };\n"
        "VSOut vs(uint id : SV_VertexID) { VSOut o; float2 uv=float2((id<<1)&2,id&2);"
        " o.uv=uv; o.pos=float4(uv*float2(2,-2)+float2(-1,1),0,1); return o; }\n"
        "struct DownOut { float4 color:SV_Target0; float2 mv:SV_Target1;"
        " float depth:SV_Target2; float mask:SV_Target3; };\n"
        "DownOut down(VSOut i) { DownOut o;"
        " o.color=src_color.SampleLevel(linear_smp,i.uv,0);"
        " o.mv=src_mv.SampleLevel(point_smp,i.uv,0)*mv_scale;"
        " o.depth=src_depth.SampleLevel(point_smp,i.uv,0);"
        " o.mask=mask_valid>0.5?src_mask.SampleLevel(point_smp,i.uv,0):0.0; return o; }\n"
        "float4 up(VSOut i):SV_Target { return src_color.SampleLevel(linear_smp,i.uv,0); }\n";

    // Cache the module once. The returned ID3DBlob implementation lives inside
    // d3dcompiler_47.dll, so the module must outlive every compiled blob.
    static HMODULE compiler = LoadLibraryW(L"d3dcompiler_47.dll");
    static pD3DCompile compile = compiler != nullptr
        ? reinterpret_cast<pD3DCompile>(GetProcAddress(compiler, "D3DCompile")) : nullptr;
    if (compile == nullptr)
    {
        SetError(ctx, HRESULT_FROM_WIN32(ERROR_MOD_NOT_FOUND), "d3dcompiler_47.dll/D3DCompile is unavailable");
        return false;
    }

    ID3DBlob *error = nullptr;
    auto one = [&](const char *entry, const char *target, ID3DBlob **out) -> bool
    {
        SafeRelease(error);
        const HRESULT hr = compile(source, sizeof(source) - 1, "feed_scale12", nullptr, nullptr,
                                   entry, target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, out, &error);
        if (FAILED(hr))
        {
            SetError(ctx, hr, "D3DCompile(%s/%s) failed: %.*s", entry, target,
                     error != nullptr ? static_cast<int>(error->GetBufferSize()) : 0,
                     error != nullptr ? static_cast<const char *>(error->GetBufferPointer()) : "");
            return false;
        }
        return true;
    };

    const bool ok = one("vs", "vs_5_0", vs) && one("down", "ps_5_0", down_ps) &&
                    one("up", "ps_5_0", up_ps);
    SafeRelease(error);
    if (!ok)
    {
        SafeRelease(*vs);
        SafeRelease(*down_ps);
        SafeRelease(*up_ps);
    }
    return ok;
}

static inline bool MakeRootSignature(Context *ctx)
{
    D3D12_DESCRIPTOR_RANGE range = {};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range.NumDescriptors = 4;
    range.BaseShaderRegister = 0;
    range.RegisterSpace = 0;
    range.OffsetInDescriptorsFromTableStart = 0;

    D3D12_ROOT_PARAMETER params[2] = {};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[0].DescriptorTable.NumDescriptorRanges = 1;
    params[0].DescriptorTable.pDescriptorRanges = &range;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[1].Constants.ShaderRegister = 0;
    params[1].Constants.RegisterSpace = 0;
    params[1].Constants.Num32BitValues = 4;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_STATIC_SAMPLER_DESC samplers[2] = {};
    samplers[0].Filter = D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT;
    samplers[1].Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
    for (unsigned i = 0; i < 2; ++i)
    {
        samplers[i].AddressU = samplers[i].AddressV = samplers[i].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        samplers[i].MipLODBias = 0.0f;
        samplers[i].MaxAnisotropy = 1;
        samplers[i].ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
        samplers[i].BorderColor = D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
        samplers[i].MinLOD = 0.0f;
        samplers[i].MaxLOD = D3D12_FLOAT32_MAX;
        samplers[i].ShaderRegister = i;
        samplers[i].RegisterSpace = 0;
        samplers[i].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    }

    D3D12_ROOT_SIGNATURE_DESC rd = {};
    rd.NumParameters = 2;
    rd.pParameters = params;
    rd.NumStaticSamplers = 2;
    rd.pStaticSamplers = samplers;
    rd.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT |
               D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS |
               D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS |
               D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS;

    using SerializeFn = HRESULT (WINAPI *)(const D3D12_ROOT_SIGNATURE_DESC *, D3D_ROOT_SIGNATURE_VERSION,
                                           ID3DBlob **, ID3DBlob **);
    HMODULE d3d12 = LoadLibraryW(L"d3d12.dll");
    auto serialize = d3d12 != nullptr
        ? reinterpret_cast<SerializeFn>(GetProcAddress(d3d12, "D3D12SerializeRootSignature")) : nullptr;
    if (serialize == nullptr)
    {
        if (d3d12 != nullptr) FreeLibrary(d3d12);
        SetError(ctx, HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND), "D3D12SerializeRootSignature is unavailable");
        return false;
    }

    ID3DBlob *blob = nullptr, *error = nullptr;
    HRESULT hr = serialize(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error);
    if (FAILED(hr))
    {
        SetError(ctx, hr, "D3D12SerializeRootSignature failed: %.*s",
                 error != nullptr ? static_cast<int>(error->GetBufferSize()) : 0,
                 error != nullptr ? static_cast<const char *>(error->GetBufferPointer()) : "");
    }
    else
    {
        hr = ctx->device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                              __uuidof(ID3D12RootSignature),
                                              reinterpret_cast<void **>(&ctx->root));
        if (FAILED(hr)) SetError(ctx, hr, "CreateRootSignature failed");
    }
    SafeRelease(blob);
    SafeRelease(error);
    FreeLibrary(d3d12);
    return SUCCEEDED(hr) && ctx->root != nullptr;
}

static inline D3D12_BLEND_DESC DisabledBlend()
{
    D3D12_BLEND_DESC d = {};
    d.AlphaToCoverageEnable = FALSE;
    d.IndependentBlendEnable = FALSE;
    const D3D12_RENDER_TARGET_BLEND_DESC rt = {
        FALSE, FALSE,
        D3D12_BLEND_ONE, D3D12_BLEND_ZERO, D3D12_BLEND_OP_ADD,
        D3D12_BLEND_ONE, D3D12_BLEND_ZERO, D3D12_BLEND_OP_ADD,
        D3D12_LOGIC_OP_NOOP, D3D12_COLOR_WRITE_ENABLE_ALL
    };
    for (auto &target : d.RenderTarget) target = rt;
    return d;
}

static inline D3D12_RASTERIZER_DESC Rasterizer()
{
    D3D12_RASTERIZER_DESC d = {};
    d.FillMode = D3D12_FILL_MODE_SOLID;
    d.CullMode = D3D12_CULL_MODE_NONE;
    d.FrontCounterClockwise = FALSE;
    d.DepthBias = D3D12_DEFAULT_DEPTH_BIAS;
    d.DepthBiasClamp = D3D12_DEFAULT_DEPTH_BIAS_CLAMP;
    d.SlopeScaledDepthBias = D3D12_DEFAULT_SLOPE_SCALED_DEPTH_BIAS;
    d.DepthClipEnable = TRUE;
    d.MultisampleEnable = FALSE;
    d.AntialiasedLineEnable = FALSE;
    d.ForcedSampleCount = 0;
    d.ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;
    return d;
}

static inline D3D12_DEPTH_STENCIL_DESC NoDepth()
{
    D3D12_DEPTH_STENCIL_DESC d = {};
    d.DepthEnable = FALSE;
    d.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    d.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    d.StencilEnable = FALSE;
    return d;
}

static inline bool MakePipelines(Context *ctx)
{
    ID3DBlob *vs = nullptr, *down = nullptr, *up = nullptr;
    if (!CompileShaders(ctx, &vs, &down, &up)) return false;

    D3D12_GRAPHICS_PIPELINE_STATE_DESC p = {};
    p.pRootSignature = ctx->root;
    p.VS = { vs->GetBufferPointer(), vs->GetBufferSize() };
    p.BlendState = DisabledBlend();
    p.SampleMask = UINT_MAX;
    p.RasterizerState = Rasterizer();
    p.DepthStencilState = NoDepth();
    p.InputLayout = { nullptr, 0 };
    p.IBStripCutValue = D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_DISABLED;
    p.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    p.DSVFormat = DXGI_FORMAT_UNKNOWN;
    p.SampleDesc.Count = 1;
    p.NodeMask = 0;
    p.Flags = D3D12_PIPELINE_STATE_FLAG_NONE;

    p.PS = { down->GetBufferPointer(), down->GetBufferSize() };
    p.NumRenderTargets = 4;
    p.RTVFormats[0] = ctx->color_format;
    p.RTVFormats[1] = DXGI_FORMAT_R16G16_FLOAT;
    p.RTVFormats[2] = DXGI_FORMAT_R32_FLOAT;
    p.RTVFormats[3] = DXGI_FORMAT_R8_UNORM;
    HRESULT hr = ctx->device->CreateGraphicsPipelineState(&p, __uuidof(ID3D12PipelineState),
                                                          reinterpret_cast<void **>(&ctx->downsample_pso));
    if (FAILED(hr)) SetError(ctx, hr, "CreateGraphicsPipelineState(downsample) failed");

    if (SUCCEEDED(hr))
    {
        p.PS = { up->GetBufferPointer(), up->GetBufferSize() };
        p.NumRenderTargets = 1;
        for (DXGI_FORMAT &f : p.RTVFormats) f = DXGI_FORMAT_UNKNOWN;
        p.RTVFormats[0] = ctx->output_format;
        hr = ctx->device->CreateGraphicsPipelineState(&p, __uuidof(ID3D12PipelineState),
                                                      reinterpret_cast<void **>(&ctx->expand_pso));
        if (FAILED(hr)) SetError(ctx, hr, "CreateGraphicsPipelineState(expand) failed");
    }

    SafeRelease(vs);
    SafeRelease(down);
    SafeRelease(up);
    return SUCCEEDED(hr) && ctx->downsample_pso != nullptr && ctx->expand_pso != nullptr;
}

static inline D3D12_CPU_DESCRIPTOR_HANDLE CpuOffset(D3D12_CPU_DESCRIPTOR_HANDLE h, UINT index, UINT stride)
{
    h.ptr += static_cast<SIZE_T>(index) * stride;
    return h;
}

static inline D3D12_GPU_DESCRIPTOR_HANDLE GpuOffset(D3D12_GPU_DESCRIPTOR_HANDLE h, UINT index, UINT stride)
{
    h.ptr += static_cast<UINT64>(index) * stride;
    return h;
}

static inline bool MakeDescriptors(Context *ctx)
{
    D3D12_DESCRIPTOR_HEAP_DESC hd = {};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = 12; // down [0..3], expand [4..7], down-with-null-mask [8..11]
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    HRESULT hr = ctx->device->CreateDescriptorHeap(&hd, __uuidof(ID3D12DescriptorHeap),
                                                   reinterpret_cast<void **>(&ctx->srv_heap));
    if (FAILED(hr))
    {
        SetError(ctx, hr, "CreateDescriptorHeap(SRV) failed");
        return false;
    }

    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    hd.NumDescriptors = 5; // four work inputs, then native Output
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    hr = ctx->device->CreateDescriptorHeap(&hd, __uuidof(ID3D12DescriptorHeap),
                                           reinterpret_cast<void **>(&ctx->rtv_heap));
    if (FAILED(hr))
    {
        SetError(ctx, hr, "CreateDescriptorHeap(RTV) failed");
        return false;
    }

    ctx->srv_stride = ctx->device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    ctx->rtv_stride = ctx->device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    const D3D12_CPU_DESCRIPTOR_HANDLE srv0 = ctx->srv_heap->GetCPUDescriptorHandleForHeapStart();
    const D3D12_CPU_DESCRIPTOR_HANDLE rtv0 = ctx->rtv_heap->GetCPUDescriptorHandleForHeapStart();

    auto make_srv = [&](UINT index, ID3D12Resource *resource, DXGI_FORMAT format)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC s = {};
        s.Format = format;
        s.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        s.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        s.Texture2D.MostDetailedMip = 0;
        s.Texture2D.MipLevels = 1;
        s.Texture2D.PlaneSlice = 0;
        s.Texture2D.ResourceMinLODClamp = 0.0f;
        ctx->device->CreateShaderResourceView(resource, &s, CpuOffset(srv0, index, ctx->srv_stride));
    };
    make_srv(0, ctx->native[SLOT_COLOR], ctx->color_format);
    make_srv(1, ctx->native[SLOT_MV], DXGI_FORMAT_R16G16_FLOAT);
    make_srv(2, ctx->native[SLOT_DEPTH], DXGI_FORMAT_R32_FLOAT);
    make_srv(3, ctx->native[SLOT_MASK], DXGI_FORMAT_R8_UNORM);
    make_srv(4, ctx->work[SLOT_OUTPUT], ctx->output_format);
    // The expand shader only reads t0, but its table has the root signature's four
    // descriptors. Fill the unused entries with legal null SRVs for debug-layer hygiene.
    make_srv(5, nullptr, DXGI_FORMAT_R8G8B8A8_UNORM);
    make_srv(6, nullptr, DXGI_FORMAT_R8G8B8A8_UNORM);
    make_srv(7, nullptr, DXGI_FORMAT_R8G8B8A8_UNORM);
    make_srv(8, ctx->native[SLOT_COLOR], ctx->color_format);
    make_srv(9, ctx->native[SLOT_MV], DXGI_FORMAT_R16G16_FLOAT);
    make_srv(10, ctx->native[SLOT_DEPTH], DXGI_FORMAT_R32_FLOAT);
    make_srv(11, nullptr, DXGI_FORMAT_R8_UNORM);

    auto make_rtv = [&](UINT index, ID3D12Resource *resource, DXGI_FORMAT format)
    {
        D3D12_RENDER_TARGET_VIEW_DESC r = {};
        r.Format = format;
        r.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        r.Texture2D.MipSlice = 0;
        r.Texture2D.PlaneSlice = 0;
        ctx->device->CreateRenderTargetView(resource, &r, CpuOffset(rtv0, index, ctx->rtv_stride));
    };
    make_rtv(0, ctx->work[SLOT_COLOR], ctx->color_format);
    make_rtv(1, ctx->work[SLOT_MV], DXGI_FORMAT_R16G16_FLOAT);
    make_rtv(2, ctx->work[SLOT_DEPTH], DXGI_FORMAT_R32_FLOAT);
    make_rtv(3, ctx->work[SLOT_MASK], DXGI_FORMAT_R8_UNORM);
    make_rtv(4, ctx->native[SLOT_OUTPUT], ctx->output_format);
    return true;
}

static inline bool FeedScale12Build(Context *ctx, const Desc &d)
{
    if (ctx == nullptr) return false;
    FeedScale12Release(ctx);
    ctx->last_error = S_OK;
    ctx->error[0] = '\0';

    if (d.device == nullptr || d.native_width == 0 || d.native_height == 0 ||
        d.work_width == 0 || d.work_height == 0 || d.work_width > d.native_width ||
        d.work_height > d.native_height ||
        (d.work_width == d.native_width && d.work_height == d.native_height))
    {
        SetError(ctx, E_INVALIDARG, "invalid native/work dimensions (native %ux%u, work %ux%u); scaler is only for reduced work size",
                 d.native_width, d.native_height, d.work_width, d.work_height);
        return false;
    }

    ctx->device = d.device;
    ctx->device->AddRef();
    ctx->native[SLOT_COLOR] = d.native_color;
    ctx->native[SLOT_OUTPUT] = d.native_output;
    ctx->native[SLOT_DEPTH] = d.native_depth;
    ctx->native[SLOT_MV] = d.native_mv;
    ctx->native[SLOT_MASK] = d.native_mask;
    for (auto *resource : ctx->native) if (resource != nullptr) resource->AddRef();
    ctx->native_width = d.native_width;
    ctx->native_height = d.native_height;
    ctx->work_width = d.work_width;
    ctx->work_height = d.work_height;
    ctx->color_format = d.color_format;
    ctx->output_format = d.output_format;

    bool ok = ValidateNativeResource(ctx, ctx->native[SLOT_COLOR], d.native_width, d.native_height,
                                     d.color_format, "Color", false) &&
              ValidateNativeResource(ctx, ctx->native[SLOT_OUTPUT], d.native_width, d.native_height,
                                     d.output_format, "Output", true) &&
              ValidateNativeResource(ctx, ctx->native[SLOT_DEPTH], d.native_width, d.native_height,
                                     DXGI_FORMAT_R32_FLOAT, "Depth", false) &&
              ValidateNativeResource(ctx, ctx->native[SLOT_MV], d.native_width, d.native_height,
                                     DXGI_FORMAT_R16G16_FLOAT, "MV", false) &&
              ValidateNativeResource(ctx, ctx->native[SLOT_MASK], d.native_width, d.native_height,
                                     DXGI_FORMAT_R8_UNORM, "Mask", false);

    const D3D12_FORMAT_SUPPORT1 sampled_rt = D3D12_FORMAT_SUPPORT1_SHADER_SAMPLE |
                                             D3D12_FORMAT_SUPPORT1_RENDER_TARGET;
    if (ok) ok = CheckFormat(ctx, d.color_format, sampled_rt, D3D12_FORMAT_SUPPORT2_NONE, "Color");
    if (ok) ok = CheckFormat(ctx, DXGI_FORMAT_R16G16_FLOAT, sampled_rt, D3D12_FORMAT_SUPPORT2_NONE, "MV");
    if (ok) ok = CheckFormat(ctx, DXGI_FORMAT_R32_FLOAT, sampled_rt, D3D12_FORMAT_SUPPORT2_NONE, "Depth");
    if (ok) ok = CheckFormat(ctx, DXGI_FORMAT_R8_UNORM, sampled_rt, D3D12_FORMAT_SUPPORT2_NONE, "Mask");
    if (ok) ok = CheckFormat(ctx, d.output_format,
                             D3D12_FORMAT_SUPPORT1_SHADER_SAMPLE | D3D12_FORMAT_SUPPORT1_RENDER_TARGET,
                             D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE, "Output");

    if (ok) ok = MakeWorkTexture(ctx, SLOT_COLOR, d.color_format, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
    if (ok) ok = MakeWorkTexture(ctx, SLOT_OUTPUT, d.output_format, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    if (ok) ok = MakeWorkTexture(ctx, SLOT_DEPTH, DXGI_FORMAT_R32_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
    if (ok) ok = MakeWorkTexture(ctx, SLOT_MV, DXGI_FORMAT_R16G16_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
    if (ok) ok = MakeWorkTexture(ctx, SLOT_MASK, DXGI_FORMAT_R8_UNORM, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
    if (ok) ok = MakeRootSignature(ctx);
    if (ok) ok = MakePipelines(ctx);
    if (ok) ok = MakeDescriptors(ctx);

    if (!ok)
    {
        const HRESULT hr = ctx->last_error;
        char saved[sizeof(ctx->error)] = {};
        strcpy_s(saved, ctx->error);
        FeedScale12Release(ctx);
        ctx->last_error = hr;
        strcpy_s(ctx->error, saved);
        return false;
    }

    ctx->ready = true;
    return true;
}

static inline void Transition(ID3D12GraphicsCommandList *list, ID3D12Resource *resource,
                              D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
{
    if (before == after) return;
    D3D12_RESOURCE_BARRIER b = {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = resource;
    b.Transition.StateBefore = before;
    b.Transition.StateAfter = after;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    list->ResourceBarrier(1, &b);
}

static inline void BindFullscreen(Context *ctx, ID3D12GraphicsCommandList *list,
                                  ID3D12PipelineState *pso, UINT width, UINT height,
                                  D3D12_GPU_DESCRIPTOR_HANDLE table)
{
    D3D12_VIEWPORT vp = {};
    vp.Width = static_cast<float>(width);
    vp.Height = static_cast<float>(height);
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    D3D12_RECT scissor = { 0, 0, static_cast<LONG>(width), static_cast<LONG>(height) };
    ID3D12DescriptorHeap *heaps[] = { ctx->srv_heap };

    list->SetDescriptorHeaps(1, heaps);
    list->SetGraphicsRootSignature(ctx->root);
    list->SetPipelineState(pso);
    list->RSSetViewports(1, &vp);
    list->RSSetScissorRects(1, &scissor);
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    list->IASetVertexBuffers(0, 0, nullptr);
    list->IASetIndexBuffer(nullptr);
    list->SetGraphicsRootDescriptorTable(0, table);
}

static inline bool FeedScale12RecordPrepare(Context *ctx, ID3D12GraphicsCommandList *list,
                                            bool mask_valid)
{
    if (ctx == nullptr || !ctx->ready || list == nullptr)
    {
        if (ctx != nullptr) SetError(ctx, E_INVALIDARG, "RecordPrepare called on an unready scaler/list");
        return false;
    }

    const D3D12_RESOURCE_STATES ps = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    Transition(list, ctx->native[SLOT_COLOR], D3D12_RESOURCE_STATE_COMMON, ps);
    Transition(list, ctx->native[SLOT_MV], D3D12_RESOURCE_STATE_COMMON, ps);
    Transition(list, ctx->native[SLOT_DEPTH], D3D12_RESOURCE_STATE_COMMON, ps);
    if (mask_valid) Transition(list, ctx->native[SLOT_MASK], D3D12_RESOURCE_STATE_COMMON, ps);
    Transition(list, ctx->work[SLOT_COLOR], D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RENDER_TARGET);
    Transition(list, ctx->work[SLOT_MV], D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RENDER_TARGET);
    Transition(list, ctx->work[SLOT_DEPTH], D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RENDER_TARGET);
    Transition(list, ctx->work[SLOT_MASK], D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RENDER_TARGET);

    const D3D12_CPU_DESCRIPTOR_HANDLE rtv0 = ctx->rtv_heap->GetCPUDescriptorHandleForHeapStart();
    D3D12_GPU_DESCRIPTOR_HANDLE srv0 = ctx->srv_heap->GetGPUDescriptorHandleForHeapStart();
    if (!mask_valid) srv0 = GpuOffset(srv0, 8, ctx->srv_stride);
    BindFullscreen(ctx, list, ctx->downsample_pso, ctx->work_width, ctx->work_height, srv0);
    list->OMSetRenderTargets(4, &rtv0, TRUE, nullptr);
    const float constants[4] = {
        static_cast<float>(ctx->work_width) / static_cast<float>(ctx->native_width),
        static_cast<float>(ctx->work_height) / static_cast<float>(ctx->native_height),
        mask_valid ? 1.0f : 0.0f, 0.0f
    };
    list->SetGraphicsRoot32BitConstants(1, 4, constants, 0);
    list->DrawInstanced(3, 1, 0, 0);

    Transition(list, ctx->native[SLOT_COLOR], ps, D3D12_RESOURCE_STATE_COMMON);
    Transition(list, ctx->native[SLOT_MV], ps, D3D12_RESOURCE_STATE_COMMON);
    Transition(list, ctx->native[SLOT_DEPTH], ps, D3D12_RESOURCE_STATE_COMMON);
    if (mask_valid) Transition(list, ctx->native[SLOT_MASK], ps, D3D12_RESOURCE_STATE_COMMON);
    const D3D12_RESOURCE_STATES ngx_input = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    Transition(list, ctx->work[SLOT_COLOR], D3D12_RESOURCE_STATE_RENDER_TARGET, ngx_input);
    Transition(list, ctx->work[SLOT_MV], D3D12_RESOURCE_STATE_RENDER_TARGET, ngx_input);
    Transition(list, ctx->work[SLOT_DEPTH], D3D12_RESOURCE_STATE_RENDER_TARGET, ngx_input);
    Transition(list, ctx->work[SLOT_MASK], D3D12_RESOURCE_STATE_RENDER_TARGET, ngx_input);
    Transition(list, ctx->work[SLOT_OUTPUT], D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    return true;
}

static inline bool FeedScale12RecordFinish(Context *ctx, ID3D12GraphicsCommandList *list)
{
    if (ctx == nullptr || !ctx->ready || list == nullptr)
    {
        if (ctx != nullptr) SetError(ctx, E_INVALIDARG, "RecordFinish called on an unready scaler/list");
        return false;
    }

    Transition(list, ctx->work[SLOT_OUTPUT], D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
               D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    Transition(list, ctx->native[SLOT_OUTPUT], D3D12_RESOURCE_STATE_COMMON,
               D3D12_RESOURCE_STATE_RENDER_TARGET);

    const D3D12_CPU_DESCRIPTOR_HANDLE out_rtv = CpuOffset(
        ctx->rtv_heap->GetCPUDescriptorHandleForHeapStart(), 4, ctx->rtv_stride);
    const D3D12_GPU_DESCRIPTOR_HANDLE out_srv = GpuOffset(
        ctx->srv_heap->GetGPUDescriptorHandleForHeapStart(), 4, ctx->srv_stride);
    BindFullscreen(ctx, list, ctx->expand_pso, ctx->native_width, ctx->native_height, out_srv);
    list->OMSetRenderTargets(1, &out_rtv, TRUE, nullptr);
    const float constants[4] = {};
    list->SetGraphicsRoot32BitConstants(1, 4, constants, 0);
    list->DrawInstanced(3, 1, 0, 0);

    Transition(list, ctx->native[SLOT_OUTPUT], D3D12_RESOURCE_STATE_RENDER_TARGET,
               D3D12_RESOURCE_STATE_COMMON);
    Transition(list, ctx->work[SLOT_OUTPUT], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
               D3D12_RESOURCE_STATE_COMMON);
    const D3D12_RESOURCE_STATES ngx_input = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    Transition(list, ctx->work[SLOT_COLOR], ngx_input, D3D12_RESOURCE_STATE_COMMON);
    Transition(list, ctx->work[SLOT_MV], ngx_input, D3D12_RESOURCE_STATE_COMMON);
    Transition(list, ctx->work[SLOT_DEPTH], ngx_input, D3D12_RESOURCE_STATE_COMMON);
    Transition(list, ctx->work[SLOT_MASK], ngx_input, D3D12_RESOURCE_STATE_COMMON);
    return true;
}

static inline bool FeedScale12RecordAbort(Context *ctx, ID3D12GraphicsCommandList *list)
{
    if (ctx == nullptr || !ctx->ready || list == nullptr)
    {
        if (ctx != nullptr) SetError(ctx, E_INVALIDARG, "RecordAbort called on an unready scaler/list");
        return false;
    }
    const D3D12_RESOURCE_STATES ngx_input = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    Transition(list, ctx->work[SLOT_COLOR], ngx_input, D3D12_RESOURCE_STATE_COMMON);
    Transition(list, ctx->work[SLOT_MV], ngx_input, D3D12_RESOURCE_STATE_COMMON);
    Transition(list, ctx->work[SLOT_DEPTH], ngx_input, D3D12_RESOURCE_STATE_COMMON);
    Transition(list, ctx->work[SLOT_MASK], ngx_input, D3D12_RESOURCE_STATE_COMMON);
    Transition(list, ctx->work[SLOT_OUTPUT], D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
               D3D12_RESOURCE_STATE_COMMON);
    return true;
}

static inline ID3D12Resource *FeedScale12Work(const Context *ctx, Slot slot)
{
    return ctx != nullptr && ctx->ready && slot < SLOT_COUNT ? ctx->work[slot] : nullptr;
}
} // namespace feed_scale12
