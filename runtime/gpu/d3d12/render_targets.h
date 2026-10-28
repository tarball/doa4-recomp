#pragma once

// EDRAM render targets as host textures, and resolves (EDRAM -> memory copies).
//
// Each distinct EDRAM surface (base tile, pitch, MSAA, format) gets its own host render target sized
// to the EDRAM space after its base. Overlapping surfaces don't share contents, which only matters
// for games that reinterpret EDRAM data between formats.
//
// Draws are recorded in explicit render passes, which the cache begins when targets are bound and
// ends before any other work (copies, dispatches, barriers and clears aren't allowed inside a pass),
// so tile-based GPUs know exactly where targets must be loaded and stored.
#include "gpu/d3d12/resources.h"
#include "gpu/d3d12/scaled_resolves.h"
#include "gpu/d3d12/shared_memory.h"
#include "gpu/xenos.h"

#include <memory>
#include <unordered_map>
#include <vector>

namespace gpu::d3d12
{
    class RenderTargetCache
    {
    public:
        bool Initialize(ID3D12Device* device, DescriptorHeap& views, SharedMemory& sharedMemory, ScaledResolves& scaledResolves);

        struct Target
        {
            uint32_t base = 0, pitch = 0, samples = 1, format = 0;
            uint32_t hostSamples = 1; // samples, or more with an MSAA override
            bool depth = false;
            uint32_t width = 0, height = 0;
            ComPtr<ID3D12Resource> resource;
            D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_RENDER_TARGET;
            D3D12_CPU_DESCRIPTOR_HANDLE view{}; // RTV or DSV
            uint32_t srv = 0;                   // persistent SRV (color, or depth)
            DXGI_FORMAT viewFormat = DXGI_FORMAT_UNKNOWN; // RTV/DSV format
        };

        // Render targets for a draw from the current registers. `colorMask` selects the color targets
        // (0-3); `depth` whether to use the depth buffer. Returns false if there are none.
        struct Bindings
        {
            Target* color[4] = {};
            Target* depth = nullptr;
            uint32_t samples = 1; // host samples
            uint32_t width = 0, height = 0; // of the targets (the smallest)
        };
        bool Lookup(uint32_t colorMask, bool depth, Bindings& out);
        // Makes the targets current: inside a render pass with them (ending a pass with others).
        void Bind(ID3D12GraphicsCommandList* list, const Bindings& targets);
        // Ends the current render pass, if any. Call before recording anything but draws.
        void EndPass(ID3D12GraphicsCommandList* list);

        // Executes a resolve draw (RB_MODECONTROL copy mode) from the current registers.
        void Resolve(ID3D12GraphicsCommandList* list, UploadAllocator& upload);

        // Debugging: copies the color target bound at slot 0 (8-bit RGBA formats) into a readback
        // buffer, resolving multisampling. Returns false if there's nothing to capture.
        struct Capture
        {
            ComPtr<ID3D12Resource> readback;
            D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
            uint32_t width = 0, height = 0;
            uint32_t base = 0, pitch = 0, samples = 1;
        };
        bool CaptureBoundColor(ID3D12GraphicsCommandList* list, uint32_t maxHeight, Capture& out);

        static DXGI_FORMAT HostColorFormat(xenos::ColorFormat format);

        uint64_t resolves = 0; // statistics

        // Draw resolution scale (settings::Graphics): host render targets are this many times the guest's in each
        // direction, and resolves also write scaled copies (ScaledResolves). Targets keep the guest's size in
        // width/height. Set before the first target is created, or Reset after changing it.
        uint32_t scale = 1;
        // MSAA override (settings::Graphics): surfaces the guest multisamples get this many samples on the host (0: the
        // guest's).
        uint32_t msaaSamples = 0;
        // Drops every target, e.g. for another scale or MSAA override (with the GPU idle, outside a render pass). Their
        // contents are lost, as across a frame in DOA4.
        void Reset();

    private:
        Target* GetTarget(uint32_t base, uint32_t pitch, uint32_t samples, uint32_t format, bool depth);
        void Transition(ID3D12GraphicsCommandList* list, Target& target, D3D12_RESOURCE_STATES state);

        ID3D12Device* m_device = nullptr;
        DescriptorHeap* m_views = nullptr;
        SharedMemory* m_sharedMemory = nullptr;
        ScaledResolves* m_scaledResolves = nullptr;
        CpuDescriptorPool m_rtvs, m_dsvs;
        std::unordered_map<uint64_t, std::unique_ptr<Target>> m_targets;

        Bindings m_bound;      // the targets of the current render pass
        bool m_passActive = false;
        ID3D12GraphicsCommandList* m_passList = nullptr;
        ComPtr<ID3D12GraphicsCommandList4> m_passList4;
        D3D12_CPU_DESCRIPTOR_HANDLE m_nullView{}; // for holes between color targets
        std::vector<ComPtr<ID3D12Resource>> m_captureCopies;

        ComPtr<ID3D12RootSignature> m_resolveRootSignature;
        ComPtr<ID3D12PipelineState> m_resolvePipelines[2]; // single-sampled, multisampled source
    };
}
