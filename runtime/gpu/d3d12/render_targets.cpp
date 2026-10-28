#include "gpu/d3d12/render_targets.h"

#include "gpu/memory_watch.h"
#include "gpu/registers.h"
#include "gpu/renderer.h"
#include "guest_memory.h"
#include "log.h"
#include "util/profiler.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <string>

namespace gpu::d3d12
{
    namespace
    {
        constexpr uint32_t kEdramTiles = 2048;
        constexpr uint32_t kEdramTileBytes = 5120; // 80x16 samples of 32 bits

        // Copies a rectangle of a render target into guest memory as a tiled texture. Depth sources are
        // written as the EDRAM stores them: 24-bit depth (unorm or 20e4 float) above 8-bit stencil.
        // Compiled twice: with XE_MSAA for multisampled sources.
        const char kResolveShader[] = R"(
#ifdef XE_MSAA
Texture2DMS<float4> xe_source : register(t0);
#else
Texture2D<float4> xe_source : register(t0);
#endif
RWByteAddressBuffer xe_dest : register(u0);
// Resolution scaling (see scaled_resolves.h): guest page -> pool page + 1, and the pool of scaled copies.
ByteAddressBuffer xe_page_table : register(t1);
RWByteAddressBuffer xe_scaled : register(u1);
cbuffer XeResolveConstants : register(b0)
{
    uint2 xe_source_origin;
    uint2 xe_size;
    uint2 xe_dest_origin;
    uint xe_dest_base;
    uint xe_dest_pitch;    // pixels, multiple of 32
    uint xe_dest_format;   // Xenos texture format
    uint xe_dest_endian;
    uint xe_swap_red_blue;
    float xe_scale;        // 2^exp_bias (and render target range scaling)
    uint xe_sample_select; // 0-3 one sample, 4 average of 0 and 1, 5 of 2 and 3, 6 of all
    uint xe_samples;
    uint xe_source_depth;  // 0 color, 1 unorm depth, 2 20e4 float depth
    uint xe_resolution_scale; // host pixels per guest pixel in each direction
    uint xe_scaled_pass;      // 1: a thread per host pixel, writing the scaled copy
};

float4 XeLoadHost(int2 p)
{
#ifdef XE_MSAA
    uint last = xe_samples - 1u;
    if (xe_sample_select < 4u)
        return xe_source.Load(p, min(xe_sample_select, last));
    if (xe_sample_select == 4u)
        return (xe_source.Load(p, 0) + xe_source.Load(p, min(1u, last))) * 0.5;
    if (xe_sample_select == 5u)
        return (xe_source.Load(p, min(2u, last)) + xe_source.Load(p, min(3u, last))) * 0.5;
    float4 sum = float4(0.0, 0.0, 0.0, 0.0);
    for (uint i = 0u; i < xe_samples; i++)
        sum += xe_source.Load(p, i);
    return sum / float(xe_samples);
#else
    return xe_source.Load(int3(p, 0));
#endif
}

// A guest pixel of the source: with resolution scaling, the average of its block of host pixels (depth: the first).
float4 XeLoadSource(int2 p)
{
    int scale = int(xe_resolution_scale);
    if (scale == 1 || xe_source_depth != 0u)
        return XeLoadHost(p * scale);
    float4 sum = float4(0.0, 0.0, 0.0, 0.0);
    for (int y = 0; y < scale; y++)
    {
        for (int x = 0; x < scale; x++)
            sum += XeLoadHost(p * scale + int2(x, y));
    }
    return sum / float(scale * scale);
}

uint XeTiledOffset2D(uint x, uint y, uint pitch, uint log_bpp)
{
    uint macro = ((x >> 5u) + (y >> 5u) * (pitch >> 5u)) << (log_bpp + 7u);
    uint micro = ((x & 7u) + ((y & 6u) << 2u)) << log_bpp;
    uint offset = macro + ((micro & ~15u) << 1u) + (micro & 15u) + ((y & 8u) << (3u + log_bpp)) + ((y & 1u) << 4u);
    return ((offset & ~511u) << 3u) + ((offset & 448u) << 2u) + (offset & 63u) + ((y & 16u) << 7u) + (((((y & 8u) >> 2u) + (x >> 3u)) & 3u) << 6u);
}

uint XeSwap(uint v, uint endian)
{
    if (endian == 1u)
        return ((v & 0x00FF00FFu) << 8u) | ((v >> 8u) & 0x00FF00FFu);
    if (endian == 2u)
        return (v << 24u) | ((v & 0xFF00u) << 8u) | ((v >> 8u) & 0xFF00u) | (v >> 24u);
    if (endian == 3u)
        return (v << 16u) | (v >> 16u);
    return v;
}

uint XeUnorm(float v, float scale)
{
    return uint(saturate(v) * scale + 0.5);
}

// To Xenos 20e4 float depth (4-bit exponent, 20-bit mantissa), rounding to nearest even.
uint XeFloat32To20e4(float f)
{
    uint u = asuint(f);
    if (int(u) <= 0)
        return 0u;
    u = min(u, 0x3FFFFFF8u);
    if (u < 0x38800000u)
        u = (0x800000u | (u & 0x7FFFFFu)) >> min(113u - (u >> 23u), 24u);
    else
        u += 0xC8000000u;
    u += 3u + ((u >> 3u) & 1u);
    return (u >> 3u) & 0xFFFFFFu;
}

// Where the host texel `sub` of the guest texel at `address` is in the scaled copy (0xFFFFFFFF if the page has none).
uint XeScaledAddress(uint address, uint2 sub, uint log_bpp)
{
    uint entry = xe_page_table.Load((address >> 12u) << 2u);
    if (entry == 0u)
        return 0xFFFFFFFFu;
    uint scale = xe_resolution_scale;
    return (((entry - 1u) << 12u) + (address & 4095u)) * scale * scale + ((sub.y * scale + sub.x) << log_bpp);
}

// k_5_6_5: a thread for each pair of pixels sharing a 32-bit word (the even one in its low half, after the endian
// swap), so the word is written whole; a pixel of the pair outside the rectangle keeps what's there. No scaled copy.
void XeResolve16(uint2 id)
{
    uint first = xe_dest_origin.x & ~1u;
    uint pairs = ((xe_dest_origin.x + xe_size.x + 1u) >> 1u) - (first >> 1u);
    if (id.x >= pairs || id.y >= xe_size.y)
        return;
    uint2 d = uint2(first + id.x * 2u, xe_dest_origin.y + id.y);
    uint address = xe_dest_base + XeTiledOffset2D(d.x, d.y, xe_dest_pitch, 1u);
    uint word = XeSwap(xe_dest.Load(address), xe_dest_endian);
    for (uint i = 0u; i < 2u; i++)
    {
        uint x = d.x + i;
        if (x < xe_dest_origin.x || x >= xe_dest_origin.x + xe_size.x)
            continue;
        float4 c = XeLoadSource(int2(xe_source_origin + uint2(x - xe_dest_origin.x, id.y))) * xe_scale;
        if (xe_swap_red_blue != 0u)
            c = c.zyxw;
        uint pixel = XeUnorm(c.x, 31.0) | (XeUnorm(c.y, 63.0) << 5u) | (XeUnorm(c.z, 31.0) << 11u);
        word = (word & ~(0xFFFFu << (i * 16u))) | (pixel << (i * 16u));
    }
    xe_dest.Store(address, XeSwap(word, xe_dest_endian));
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (xe_dest_format == 4u)
    {
        XeResolve16(id.xy);
        return;
    }
    uint scale = xe_resolution_scale;
    bool scaled = xe_scaled_pass != 0u;
    if (any(id.xy >= (scaled ? xe_size * scale : xe_size)))
        return;
    float4 c = scaled ? XeLoadHost(int2(xe_source_origin * scale + id.xy)) : XeLoadSource(int2(xe_source_origin + id.xy));
    uint2 d = xe_dest_origin + (scaled ? id.xy / scale : id.xy);
    uint2 packed = uint2(0u, 0u);
    uint log_bpp = 2u;
    if (xe_source_depth != 0u)
    {
        // (The stencil isn't read: it's 0.)
        packed.x = (xe_source_depth == 1u ? uint(saturate(c.x) * 16777215.0 + 0.5) : XeFloat32To20e4(c.x)) << 8u;
    }
    else
    {
    c *= xe_scale;
    if (xe_swap_red_blue != 0u)
        c = c.zyxw;
    switch (xe_dest_format)
    {
    case 6u: // k_8_8_8_8
    case 62u:
        packed.x = XeUnorm(c.x, 255.0) | (XeUnorm(c.y, 255.0) << 8u) | (XeUnorm(c.z, 255.0) << 16u) | (XeUnorm(c.w, 255.0) << 24u);
        break;
    case 7u: // k_2_10_10_10
        packed.x = XeUnorm(c.x, 1023.0) | (XeUnorm(c.y, 1023.0) << 10u) | (XeUnorm(c.z, 1023.0) << 20u) | (XeUnorm(c.w, 3.0) << 30u);
        break;
    case 25u: // k_16_16
        packed.x = XeUnorm(c.x, 65535.0) | (XeUnorm(c.y, 65535.0) << 16u);
        break;
    case 26u: // k_16_16_16_16
        packed = uint2(XeUnorm(c.x, 65535.0) | (XeUnorm(c.y, 65535.0) << 16u), XeUnorm(c.z, 65535.0) | (XeUnorm(c.w, 65535.0) << 16u));
        log_bpp = 3u;
        break;
    case 31u: // k_16_16_FLOAT
        packed.x = f32tof16(c.x) | (f32tof16(c.y) << 16u);
        break;
    case 32u: // k_16_16_16_16_FLOAT
        packed = uint2(f32tof16(c.x) | (f32tof16(c.y) << 16u), f32tof16(c.z) | (f32tof16(c.w) << 16u));
        log_bpp = 3u;
        break;
    case 36u: // k_32_FLOAT
        packed.x = asuint(c.x);
        break;
    case 37u: // k_32_32_FLOAT
        packed = asuint(c.xy);
        log_bpp = 3u;
        break;
    default:
        return;
    }
    }
    uint address = xe_dest_base + XeTiledOffset2D(d.x, d.y, xe_dest_pitch, log_bpp);
    uint2 words = uint2(XeSwap(packed.x, xe_dest_endian), 0u);
    if (log_bpp == 3u)
    {
        // 8-in-64 swaps the dwords too.
        uint endian = xe_dest_endian == 4u ? 2u : xe_dest_endian;
        words = uint2(XeSwap(packed.x, endian), XeSwap(packed.y, endian));
        if (xe_dest_endian == 4u)
            words = words.yx;
    }
    if (scaled)
    {
        address = XeScaledAddress(address, id.xy % scale, log_bpp);
        if (address == 0xFFFFFFFFu)
            return;
        if (log_bpp == 3u)
            xe_scaled.Store2(address, words);
        else
            xe_scaled.Store(address, words.x);
    }
    else if (log_bpp == 3u)
    {
        xe_dest.Store2(address, words);
    }
    else
    {
        xe_dest.Store(address, words.x);
    }
}
)";

        struct ResolveConstants
        {
            uint32_t sourceX, sourceY, width, height, destX, destY, destBase, destPitch, destFormat, destEndian, swapRedBlue;
            float scale;
            uint32_t sampleSelect, samples;
            uint32_t sourceDepth;
            uint32_t resolutionScale;
            uint32_t scaledPass;
        };

        uint32_t SwapForEndian(uint32_t value, uint32_t endian)
        {
            switch (endian)
            {
            case 1: return ((value & 0x00FF00FF) << 8) | ((value >> 8) & 0x00FF00FF);
            case 2: return __builtin_bswap32(value);
            case 3: return (value << 16) | (value >> 16);
            default: return value;
            }
        }

        // Xenos 20e4 float depth (4-bit exponent, 20-bit mantissa) to float.
        float Float20e4ToFloat(uint32_t value)
        {
            const uint32_t exponent = (value >> 20) & 0xF, mantissa = value & 0xFFFFF;
            if (exponent == 0)
                return std::ldexp(float(mantissa) / float(1 << 20), -14);
            return std::ldexp(1.0f + float(mantissa) / float(1 << 20), int(exponent) - 15);
        }

        uint32_t ColorInfoRegister(uint32_t index)
        {
            static const uint32_t kRegisters[4] = { reg::RB_COLOR_INFO, reg::RB_COLOR1_INFO, reg::RB_COLOR2_INFO, reg::RB_COLOR3_INFO };
            return kRegisters[index & 3];
        }
    }

    DXGI_FORMAT RenderTargetCache::HostColorFormat(xenos::ColorFormat format)
    {
        switch (format)
        {
        case xenos::ColorFormat::k_8_8_8_8:
        case xenos::ColorFormat::k_8_8_8_8_GAMMA:
            return DXGI_FORMAT_R8G8B8A8_TYPELESS;
        case xenos::ColorFormat::k_2_10_10_10:
        case xenos::ColorFormat::k_2_10_10_10_AS_10_10_10_10:
            return DXGI_FORMAT_R10G10B10A2_UNORM;
        case xenos::ColorFormat::k_2_10_10_10_FLOAT:
        case xenos::ColorFormat::k_2_10_10_10_FLOAT_AS_16_16_16_16:
        case xenos::ColorFormat::k_16_16_16_16_FLOAT:
            return DXGI_FORMAT_R16G16B16A16_FLOAT;
        case xenos::ColorFormat::k_16_16:
            return DXGI_FORMAT_R16G16_SNORM;
        case xenos::ColorFormat::k_16_16_16_16:
            return DXGI_FORMAT_R16G16B16A16_SNORM;
        case xenos::ColorFormat::k_16_16_FLOAT:
            return DXGI_FORMAT_R16G16_FLOAT;
        case xenos::ColorFormat::k_32_FLOAT:
            return DXGI_FORMAT_R32_FLOAT;
        case xenos::ColorFormat::k_32_32_FLOAT:
            return DXGI_FORMAT_R32G32_FLOAT;
        default:
            return DXGI_FORMAT_R8G8B8A8_TYPELESS;
        }
    }

    bool RenderTargetCache::Initialize(ID3D12Device* device, DescriptorHeap& views, SharedMemory& sharedMemory, ScaledResolves& scaledResolves)
    {
        m_device = device;
        m_views = &views;
        m_sharedMemory = &sharedMemory;
        m_scaledResolves = &scaledResolves;
        if (!m_rtvs.Initialize(device, D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1024) || !m_dsvs.Initialize(device, D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 256))
            return false;
        m_nullView = m_rtvs.Allocate();
        D3D12_RENDER_TARGET_VIEW_DESC nullRtv{};
        nullRtv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        nullRtv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        device->CreateRenderTargetView(nullptr, &nullRtv, m_nullView);

        // Resolve pipeline: constants, the source texture (table), the shared memory (raw UAV), and for resolution
        // scaling the page table (raw SRV) and the scaled pool (raw UAV).
        D3D12_DESCRIPTOR_RANGE range{ D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0 };
        D3D12_ROOT_PARAMETER parameters[5]{};
        parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        parameters[0].Constants = { 0, 0, sizeof(ResolveConstants) / 4 };
        parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        parameters[1].DescriptorTable = { 1, &range };
        parameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        parameters[2].Descriptor = { 0, 0 };
        parameters[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        parameters[3].Descriptor = { 1, 0 };
        parameters[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        parameters[4].Descriptor = { 1, 0 };
        D3D12_ROOT_SIGNATURE_DESC rootDescription{ 5, parameters, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE };
        ComPtr<ID3DBlob> blob, error;
        if (FAILED(D3D12SerializeRootSignature(&rootDescription, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error)) ||
            FAILED(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&m_resolveRootSignature))))
        {
            LOG_ERROR("D3D12: can't create the resolve root signature");
            return false;
        }
        for (int msaa = 0; msaa < 2; msaa++)
        {
            const std::string source = std::string(msaa ? "#define XE_MSAA 1\n" : "") + kResolveShader;
            auto shader = CompileShader(source.c_str(), "main", "cs_5_1", msaa ? "resolve (MSAA)" : "resolve");
            if (!shader)
                return false;
            D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline{};
            pipeline.pRootSignature = m_resolveRootSignature.Get();
            pipeline.CS = { shader->GetBufferPointer(), shader->GetBufferSize() };
            if (FAILED(device->CreateComputePipelineState(&pipeline, IID_PPV_ARGS(&m_resolvePipelines[msaa]))))
            {
                LOG_ERROR("D3D12: can't create the resolve pipeline");
                return false;
            }
        }
        return true;
    }

    void RenderTargetCache::Reset()
    {
        for (const auto& [key, target] : m_targets)
        {
            if (!target) // creation failed
                continue;
            (target->depth ? m_dsvs : m_rtvs).Free(target->view);
            m_views->FreePersistent(target->srv);
        }
        m_targets.clear();
        m_bound = {};
    }

    RenderTargetCache::Target* RenderTargetCache::GetTarget(uint32_t base, uint32_t pitch, uint32_t samples, uint32_t format, bool depth)
    {
        if (pitch == 0 || base >= kEdramTiles)
            return nullptr;
        const uint64_t key = uint64_t(base) | (uint64_t(pitch) << 12) | (uint64_t(samples) << 28) | (uint64_t(format) << 32) | (uint64_t(depth) << 40);
        auto& slot = m_targets[key];
        if (slot)
            return slot.get();

        auto target = std::make_unique<Target>();
        target->base = base;
        target->pitch = pitch;
        target->samples = samples;
        target->hostSamples = samples;
        target->format = format;
        target->depth = depth;
        const uint32_t bytesPerSample = !depth && xenos::Is64bpp(xenos::ColorFormat(format)) ? 8 : 4;
        const uint64_t available = uint64_t(kEdramTiles - base) * kEdramTileBytes;
        target->width = pitch;
        target->height = std::clamp<uint32_t>(uint32_t(available / (uint64_t(pitch) * samples * bytesPerSample)) & ~15u, 16, 2048);

        D3D12_RESOURCE_DESC description{};
        description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        description.Width = target->width * scale;
        description.Height = target->height * scale;
        description.DepthOrArraySize = 1;
        description.MipLevels = 1;
        description.SampleDesc.Count = samples;
        D3D12_CLEAR_VALUE clear{};
        DXGI_FORMAT srvFormat;
        const uint32_t requestedSamples = samples > 1 ? std::max(samples, msaaSamples) : samples;
        if (depth)
        {
            const bool floatDepth = xenos::DepthFormat(format) == xenos::DepthFormat::kD24FS8;
            description.Format = floatDepth ? DXGI_FORMAT_R32G8X24_TYPELESS : DXGI_FORMAT_R24G8_TYPELESS;
            description.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
            target->viewFormat = floatDepth ? DXGI_FORMAT_D32_FLOAT_S8X24_UINT : DXGI_FORMAT_D24_UNORM_S8_UINT;
            srvFormat = floatDepth ? DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS : DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
            clear.Format = target->viewFormat;
            clear.DepthStencil = { 1.0f, 0 };
            target->state = D3D12_RESOURCE_STATE_DEPTH_WRITE;
        }
        else
        {
            const xenos::ColorFormat colorFormat = xenos::ColorFormat(format);
            description.Format = HostColorFormat(colorFormat);
            description.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
            if (description.Format == DXGI_FORMAT_R8G8B8A8_TYPELESS)
            {
                target->viewFormat = colorFormat == xenos::ColorFormat::k_8_8_8_8_GAMMA ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_UNORM;
                srvFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
            }
            else
            {
                target->viewFormat = srvFormat = description.Format;
            }
            clear.Format = target->viewFormat;
            target->state = D3D12_RESOURCE_STATE_RENDER_TARGET;
        }
        // An MSAA override, if the device supports that sample count for the format.
        if (requestedSamples > samples)
        {
            D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS levels{ target->viewFormat, requestedSamples, D3D12_MULTISAMPLE_QUALITY_LEVELS_FLAG_NONE, 0 };
            if (SUCCEEDED(m_device->CheckFeatureSupport(D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS, &levels, sizeof(levels))) && levels.NumQualityLevels)
                target->hostSamples = requestedSamples;
        }
        description.SampleDesc.Count = target->hostSamples;
        D3D12_HEAP_PROPERTIES heap{ D3D12_HEAP_TYPE_DEFAULT };
        if (FAILED(m_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &description, target->state, &clear, IID_PPV_ARGS(&target->resource))))
        {
            LOG_ERROR("D3D12: can't create a %ux%u render target (format %u%s)", target->width * scale, target->height * scale, format, depth ? ", depth" : "");
            return nullptr;
        }

        if (depth)
        {
            D3D12_DEPTH_STENCIL_VIEW_DESC dsv{};
            dsv.Format = target->viewFormat;
            dsv.ViewDimension = target->hostSamples > 1 ? D3D12_DSV_DIMENSION_TEXTURE2DMS : D3D12_DSV_DIMENSION_TEXTURE2D;
            target->view = m_dsvs.Allocate();
            m_device->CreateDepthStencilView(target->resource.Get(), &dsv, target->view);
        }
        else
        {
            D3D12_RENDER_TARGET_VIEW_DESC rtv{};
            rtv.Format = target->viewFormat;
            rtv.ViewDimension = target->hostSamples > 1 ? D3D12_RTV_DIMENSION_TEXTURE2DMS : D3D12_RTV_DIMENSION_TEXTURE2D;
            target->view = m_rtvs.Allocate();
            m_device->CreateRenderTargetView(target->resource.Get(), &rtv, target->view);
        }
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Format = srvFormat;
        srv.ViewDimension = target->hostSamples > 1 ? D3D12_SRV_DIMENSION_TEXTURE2DMS : D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Texture2D.MipLevels = 1;
        target->srv = m_views->AllocatePersistent(1);
        m_device->CreateShaderResourceView(target->resource.Get(), &srv, m_views->Cpu(target->srv));

        LOG_INFO("GPU: render target at tile %u, pitch %u, %ux MSAA, format %u%s -> %ux%u%s", base, pitch, samples, format, depth ? " (depth)" : "",
            target->width * scale, target->height * scale, target->hostSamples != samples ? (" with " + std::to_string(target->hostSamples) + " samples").c_str() : "");
        slot = std::move(target);
        return slot.get();
    }

    void RenderTargetCache::Transition(ID3D12GraphicsCommandList* list, Target& target, D3D12_RESOURCE_STATES state)
    {
        if (target.state == state)
            return;
        const D3D12_RESOURCE_BARRIER barrier = TransitionBarrier(target.resource.Get(), target.state, state);
        list->ResourceBarrier(1, &barrier);
        target.state = state;
    }

    bool RenderTargetCache::Lookup(uint32_t colorMask, bool depth, Bindings& out)
    {
        const uint32_t surfaceInfo = Register(reg::RB_SURFACE_INFO);
        const uint32_t pitch = surfaceInfo & 0x3FFF;
        const uint32_t samples = 1u << std::min((surfaceInfo >> 16) & 3, 2u);

        out = Bindings{};
        out.samples = samples;
        out.width = UINT32_MAX;
        out.height = UINT32_MAX;
        for (uint32_t i = 0; i < 4; i++)
        {
            if (!(colorMask & (1u << i)))
                continue;
            const uint32_t info = Register(ColorInfoRegister(i));
            Target* target = GetTarget(info & 0xFFF, pitch, samples, (info >> 16) & 0xF, false);
            if (!target)
                continue;
            out.color[i] = target;
            out.samples = target->hostSamples;
            out.width = std::min(out.width, target->width);
            out.height = std::min(out.height, target->height);
        }
        if (depth)
        {
            const uint32_t info = Register(reg::RB_DEPTH_INFO);
            Target* target = GetTarget(info & 0xFFF, pitch, samples, (info >> 16) & 1, true);
            if (target)
            {
                out.depth = target;
                out.samples = target->hostSamples;
                out.width = std::min(out.width, target->width);
                out.height = std::min(out.height, target->height);
            }
        }
        return out.width != UINT32_MAX;
    }

    void RenderTargetCache::Bind(ID3D12GraphicsCommandList* list, const Bindings& targets)
    {
        if (m_passActive && m_passList == list)
        {
            bool same = targets.depth == m_bound.depth;
            for (uint32_t i = 0; same && i < 4; i++)
                same = targets.color[i] == m_bound.color[i];
            if (same)
                return;
        }
        EndPass(list);
        if (m_passList != list)
        {
            m_passList = list;
            m_passList4.Reset();
            list->QueryInterface(IID_PPV_ARGS(&m_passList4));
        }

        D3D12_RENDER_PASS_RENDER_TARGET_DESC colors[4]{};
        uint32_t count = 0;
        for (uint32_t i = 0; i < 4; i++)
        {
            if (targets.color[i])
                count = i + 1;
        }
        for (uint32_t i = 0; i < count; i++)
        {
            if (Target* target = targets.color[i])
            {
                Transition(list, *target, D3D12_RESOURCE_STATE_RENDER_TARGET);
                colors[i].cpuDescriptor = target->view;
                colors[i].BeginningAccess.Type = D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_PRESERVE;
                colors[i].EndingAccess.Type = D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_PRESERVE;
            }
            else
            {
                colors[i].cpuDescriptor = m_nullView;
                colors[i].BeginningAccess.Type = D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_NO_ACCESS;
                colors[i].EndingAccess.Type = D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_NO_ACCESS;
            }
        }
        D3D12_RENDER_PASS_DEPTH_STENCIL_DESC depth{};
        if (targets.depth)
        {
            Transition(list, *targets.depth, D3D12_RESOURCE_STATE_DEPTH_WRITE);
            depth.cpuDescriptor = targets.depth->view;
            depth.DepthBeginningAccess.Type = depth.StencilBeginningAccess.Type = D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_PRESERVE;
            depth.DepthEndingAccess.Type = depth.StencilEndingAccess.Type = D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_PRESERVE;
        }
        m_passList4->BeginRenderPass(count, count ? colors : nullptr, targets.depth ? &depth : nullptr, D3D12_RENDER_PASS_FLAG_NONE);
        m_passActive = true;
        m_bound = targets;
    }

    void RenderTargetCache::EndPass(ID3D12GraphicsCommandList* list)
    {
        if (!m_passActive)
            return;
        if (m_passList == list)
            m_passList4->EndRenderPass();
        m_passActive = false;
    }

    bool RenderTargetCache::CaptureBoundColor(ID3D12GraphicsCommandList* list, uint32_t maxHeight, Capture& out)
    {
        Target* target = m_passActive ? m_bound.color[0] : nullptr;
        if (!target || (target->viewFormat != DXGI_FORMAT_R8G8B8A8_UNORM && target->viewFormat != DXGI_FORMAT_R8G8B8A8_UNORM_SRGB))
            return false;
        EndPass(list);
        out.width = target->width * scale;
        out.height = std::min(target->height, maxHeight) * scale;
        out.base = target->base;
        out.pitch = target->pitch;
        out.samples = target->hostSamples;

        // A single-sampled copy of the whole target.
        D3D12_RESOURCE_DESC description = target->resource->GetDesc();
        description.SampleDesc.Count = 1;
        description.SampleDesc.Quality = 0;
        description.Alignment = 0; // multisampled targets carry the 4 MB MSAA alignment
        description.Flags = D3D12_RESOURCE_FLAG_NONE;
        D3D12_HEAP_PROPERTIES heap{ D3D12_HEAP_TYPE_DEFAULT };
        ComPtr<ID3D12Resource> copy;
        if (FAILED(m_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &description, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&copy))))
            return false;
        description.Height = out.height;
        UINT64 size = 0;
        m_device->GetCopyableFootprints(&description, 0, 1, 0, &out.footprint, nullptr, nullptr, &size);
        out.readback = CreateBuffer(m_device, size, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
        if (!out.readback)
            return false;

        const D3D12_RESOURCE_STATES previous = target->state;
        if (target->hostSamples > 1)
        {
            Transition(list, *target, D3D12_RESOURCE_STATE_RESOLVE_SOURCE);
            D3D12_RESOURCE_BARRIER barrier = TransitionBarrier(copy.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_RESOLVE_DEST);
            list->ResourceBarrier(1, &barrier);
            list->ResolveSubresource(copy.Get(), 0, target->resource.Get(), 0, DXGI_FORMAT_R8G8B8A8_UNORM);
            barrier = TransitionBarrier(copy.Get(), D3D12_RESOURCE_STATE_RESOLVE_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
            list->ResourceBarrier(1, &barrier);
        }
        else
        {
            Transition(list, *target, D3D12_RESOURCE_STATE_COPY_SOURCE);
            list->CopyResource(copy.Get(), target->resource.Get());
            const D3D12_RESOURCE_BARRIER barrier = TransitionBarrier(copy.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
            list->ResourceBarrier(1, &barrier);
        }
        D3D12_TEXTURE_COPY_LOCATION destination{ out.readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
        destination.PlacedFootprint = out.footprint;
        D3D12_TEXTURE_COPY_LOCATION source{ copy.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
        const D3D12_BOX box{ 0, 0, 0, out.width, out.height, 1 };
        list->CopyTextureRegion(&destination, 0, 0, 0, &source, &box);
        Transition(list, *target, previous);
        m_captureCopies.push_back(copy); // kept alive until the frame has executed (released with the cache)
        return true;
    }

    void RenderTargetCache::Resolve(ID3D12GraphicsCommandList* list, UploadAllocator& upload)
    {
        EndPass(list);
        const uint32_t copyControl = Register(reg::RB_COPY_CONTROL);
        const uint32_t sourceSelect = copyControl & 7;
        const bool colorClear = (copyControl >> 8) & 1;
        const bool depthClear = (copyControl >> 9) & 1;
        const uint32_t command = (copyControl >> 20) & 3;

        const uint32_t surfaceInfo = Register(reg::RB_SURFACE_INFO);
        const uint32_t pitch = surfaceInfo & 0x3FFF;
        const uint32_t samples = 1u << std::min((surfaceInfo >> 16) & 3, 2u);

        // The rectangle is the bounding box of the resolve draw's three vertices (fetch constant 0).
        const uint32_t fetch0 = Register(reg::SHADER_CONSTANT_FETCH_00_0), fetch1 = Register(reg::SHADER_CONSTANT_FETCH_00_0 + 1);
        const uint32_t vertices = guest::PhysicalToGuest(fetch0 & 0xFFFFFFFC);
        float x[3], y[3];
        for (int i = 0; i < 3; i++)
        {
            const uint32_t rawX = SwapForEndian(*reinterpret_cast<const uint32_t*>(guest::ToHost(vertices + i * 8)), fetch1 & 3);
            const uint32_t rawY = SwapForEndian(*reinterpret_cast<const uint32_t*>(guest::ToHost(vertices + i * 8 + 4)), fetch1 & 3);
            memcpy(&x[i], &rawX, 4);
            memcpy(&y[i], &rawY, 4);
        }
        int32_t x0 = int32_t(std::lround(std::min({ x[0], x[1], x[2] }))), x1 = int32_t(std::lround(std::max({ x[0], x[1], x[2] })));
        int32_t y0 = int32_t(std::lround(std::min({ y[0], y[1], y[2] }))), y1 = int32_t(std::lround(std::max({ y[0], y[1], y[2] })));
        if (Register(reg::PA_SU_SC_MODE_CNTL) & (1u << 16))
        {
            const uint32_t offset = Register(reg::PA_SC_WINDOW_OFFSET);
            const int32_t offsetX = int32_t(offset << 17) >> 17, offsetY = int32_t(offset << 1) >> 17;
            x0 += offsetX;
            x1 += offsetX;
            y0 += offsetY;
            y1 += offsetY;
        }

        Target* source;
        if (sourceSelect < 4)
        {
            const uint32_t info = Register(ColorInfoRegister(sourceSelect));
            source = GetTarget(info & 0xFFF, pitch, samples, (info >> 16) & 0xF, false);
        }
        else
        {
            const uint32_t info = Register(reg::RB_DEPTH_INFO);
            source = GetTarget(info & 0xFFF, pitch, samples, (info >> 16) & 1, true);
        }
        if (!source)
            return;
        x0 = std::clamp<int32_t>(x0, 0, int32_t(source->width));
        x1 = std::clamp<int32_t>(x1, 0, int32_t(source->width));
        y0 = std::clamp<int32_t>(y0, 0, int32_t(source->height));
        y1 = std::clamp<int32_t>(y1, 0, int32_t(source->height));
        if (x1 <= x0 || y1 <= y0)
            return;
        resolves++;
        profile::Count(profile::Counter::kResolves);

        // Copy.
        const uint32_t destBase = Register(reg::RB_COPY_DEST_BASE);
        const uint32_t destPitchInfo = Register(reg::RB_COPY_DEST_PITCH);
        const uint32_t destInfo = Register(reg::RB_COPY_DEST_INFO);
        const uint32_t destFormat = (destInfo >> 7) & 0x3F;
        // Depth goes to any 32-bit format (raw EDRAM words, sampled as k_24_8 / k_24_8_FLOAT or k_8_8_8_8). Of the 16-bit
        // formats, k_5_6_5 (DOA4's attract cinematic builds a glow in it).
        const bool depthSource = sourceSelect >= 4;
        const bool wideDest = destFormat == 26 || destFormat == 32 || destFormat == 37;
        const bool narrowDest = destFormat == 4;
        const bool supported = depthSource ? !wideDest && !narrowDest
                                           : (destFormat == 4 || destFormat == 6 || destFormat == 7 || destFormat == 25 || destFormat == 26 ||
                                                 destFormat == 31 || destFormat == 32 || destFormat == 36 || destFormat == 37 || destFormat == 62);
        if (command != 3 && destBase && supported)
        {
            const uint32_t destPitch = std::max((destPitchInfo & 0x3FFF), 1u);
            const uint32_t alignedPitch = (destPitch + 31) & ~31u;
            const uint32_t originX = uint32_t(x0) & 31, originY = uint32_t(y0) & 31;
            const uint32_t width = uint32_t(x1 - x0), height = uint32_t(y1 - y0);
            const uint32_t bytesPerPixel = wideDest ? 8 : narrowDest ? 2 : 4;
            const uint32_t rows = (originY + height + 31) & ~31u;
            const uint32_t destSize = alignedPitch * rows * bytesPerPixel;

            // Pages partly covered must hold current data before the GPU overwrites the rest.
            m_sharedMemory->RequestRange(upload, destBase, destSize);
            m_sharedMemory->Transition(list, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            Transition(list, *source, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

            ResolveConstants constants{};
            constants.sourceX = uint32_t(x0);
            constants.sourceY = uint32_t(y0);
            constants.width = width;
            constants.height = height;
            constants.destX = originX;
            constants.destY = originY;
            constants.destBase = destBase;
            constants.destPitch = alignedPitch;
            constants.destFormat = destFormat;
            constants.destEndian = destInfo & 7;
            constants.swapRedBlue = (destInfo >> 24) & 1;
            const int32_t expBias = int32_t(destInfo << 10) >> 26;
            const xenos::ColorFormat sourceFormat = xenos::ColorFormat(source->format);
            // 16-bit fixed render targets hold -32...32 as snorm.
            const float rangeScale = (sourceFormat == xenos::ColorFormat::k_16_16 || sourceFormat == xenos::ColorFormat::k_16_16_16_16) ? 32.0f : 1.0f;
            constants.scale = std::ldexp(rangeScale, expBias);
            constants.sampleSelect = (copyControl >> 4) & 7;
            constants.samples = source->hostSamples;
            if (source->hostSamples > samples && constants.sampleSelect >= 4)
                constants.sampleSelect = 6; // (the guest's 2 samples are all of a pixel)
            if (depthSource)
            {
                constants.sourceDepth = xenos::DepthFormat(source->format) == xenos::DepthFormat::kD24FS8 ? 2 : 1;
                constants.sampleSelect = std::min(constants.sampleSelect, 3u); // samples aren't averaged
            }
            constants.resolutionScale = scale;

            // The 32x32 macro tiles the resolve writes, as one byte range per row of them.
            const uint32_t log2Bpp = wideDest ? 3 : narrowDest ? 1 : 2;
            const uint32_t tileBytes = 1u << (log2Bpp + 10);
            const uint32_t firstColumn = originX >> 5, columns = ((originX + width - 1) >> 5) - firstColumn + 1;
            std::vector<std::pair<uint32_t, uint32_t>> written;
            for (uint32_t row = originY >> 5; row <= (originY + height - 1) >> 5; row++)
                written.emplace_back(destBase + (row * (alignedPitch >> 5) + firstColumn) * tileBytes, columns * tileBytes);

            list->SetComputeRootSignature(m_resolveRootSignature.Get());
            list->SetPipelineState(m_resolvePipelines[source->hostSamples > 1 ? 1 : 0].Get());
            list->SetComputeRoot32BitConstants(0, sizeof(constants) / 4, &constants, 0);
            list->SetComputeRootDescriptorTable(1, m_views->Gpu(source->srv));
            list->SetComputeRootUnorderedAccessView(2, m_sharedMemory->GpuAddress());
            // (Unused without the scaled pass; root descriptors just need an address.)
            list->SetComputeRootShaderResourceView(3, m_sharedMemory->GpuAddress());
            list->SetComputeRootUnorderedAccessView(4, m_sharedMemory->GpuAddress());
            // (16-bit formats: a thread per pair of pixels.)
            const uint32_t threadsX = narrowDest ? ((originX + width + 1) >> 1) - (originX >> 1) : width;
            list->Dispatch((threadsX + 7) / 8, (height + 7) / 8, 1);
            m_sharedMemory->UavBarrier(list);
            for (const auto& [start, length] : written)
                watch::NoteGpuWrite(start, length);

            // Resolution scaling: the scaled copy, a thread per host pixel (see ScaledResolves). Textures load from them
            // only with 4 or 8 bytes per texel.
            bool scaledStorage = scale > 1 && !narrowDest;
            for (const auto& [start, length] : written)
                scaledStorage = scaledStorage && m_scaledResolves->Allocate(list, upload, start, length);
            if (scaledStorage)
            {
                m_scaledResolves->TransitionPageTable(list, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                m_scaledResolves->TransitionPool(list, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                constants.scaledPass = 1;
                list->SetComputeRoot32BitConstants(0, sizeof(constants) / 4, &constants, 0);
                list->SetComputeRootShaderResourceView(3, m_scaledResolves->PageTableAddress());
                list->SetComputeRootUnorderedAccessView(4, m_scaledResolves->PoolAddress());
                list->Dispatch((width * scale + 7) / 8, (height * scale + 7) / 8, 1);
                m_scaledResolves->PoolUavBarrier(list);
                for (const auto& [start, length] : written)
                    m_scaledResolves->NoteWritten(start, length, log2Bpp);
            }
        }
        else if (command != 3 && destBase)
        {
            static bool reported = false;
            if (!reported)
            {
                reported = true;
                LOG_WARN("GPU: unsupported resolve (source %u, %ux MSAA, dest format %u)", sourceSelect, samples, destFormat);
            }
        }

        // Clears of the resolved area (in host pixels).
        const int32_t hostScale = int32_t(scale);
        const D3D12_RECT rect{ x0 * hostScale, y0 * hostScale, x1 * hostScale, y1 * hostScale };
        if (colorClear)
        {
            const uint32_t index = sourceSelect < 4 ? sourceSelect : 0;
            const uint32_t info = Register(ColorInfoRegister(index));
            if (Target* color = GetTarget(info & 0xFFF, pitch, samples, (info >> 16) & 0xF, false))
            {
                const uint32_t value = Register(reg::RB_COLOR_CLEAR), low = Register(reg::RB_COLOR_CLEAR_LO);
                float rgba[4];
                switch (xenos::ColorFormat(color->format))
                {
                case xenos::ColorFormat::k_8_8_8_8:
                case xenos::ColorFormat::k_8_8_8_8_GAMMA:
                    for (int i = 0; i < 4; i++)
                        rgba[i] = float((value >> (8 * i)) & 0xFF) / 255.0f;
                    break;
                case xenos::ColorFormat::k_32_FLOAT:
                    memcpy(&rgba[0], &value, 4);
                    rgba[1] = rgba[2] = rgba[3] = 0.0f;
                    break;
                default:
                    // 64bpp formats split the value over two registers; approximate with the high one.
                    for (int i = 0; i < 4; i++)
                        rgba[i] = float(((i < 2 ? low : value) >> (16 * (i & 1))) & 0xFFFF) / 65535.0f;
                    break;
                }
                Transition(list, *color, D3D12_RESOURCE_STATE_RENDER_TARGET);
                list->ClearRenderTargetView(color->view, rgba, 1, &rect);
            }
        }
        if (depthClear)
        {
            const uint32_t info = Register(reg::RB_DEPTH_INFO);
            if (Target* depth = GetTarget(info & 0xFFF, pitch, samples, (info >> 16) & 1, true))
            {
                const uint32_t value = Register(reg::RB_DEPTH_CLEAR);
                const float depthValue = xenos::DepthFormat(depth->format) == xenos::DepthFormat::kD24FS8 ? Float20e4ToFloat(value >> 8)
                                                                                                        : float(value >> 8) / float(0xFFFFFF);
                Transition(list, *depth, D3D12_RESOURCE_STATE_DEPTH_WRITE);
                list->ClearDepthStencilView(depth->view, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, depthValue, uint8_t(value & 0xFF), 1, &rect);
            }
        }
    }
}
