export module quats:frameresource;
import std;
import shared;

enum GFX_ROOT_ARG
{
    GFX_ROOT_ARG_OBJECT_CBV = 0,
    GFX_ROOT_ARG_PASS_CBV,
    GFX_ROOT_ARG_SKINNED_CBV, // used for skinning demo
    GFX_ROOT_ARG_MATERIAL_SRV,
    GFX_ROOT_ARG_INSTANCEDATA_SRV, // used for instancing demo
    GFX_ROOT_ARG_COUNT
};

// Stores the resources needed for the CPU to build the command lists
// for a frame.  
struct FrameResource
{
    FrameResource(D3D12::ID3D12Device* device, std::uint32_t passCount, std::uint32_t objectCount, std::uint32_t materialCount)
    {
        ThrowIfFailed(device->CreateCommandAllocator(
            D3D12_COMMAND_LIST_TYPE_DIRECT,
			__uuidof(D3D12::ID3D12CommandAllocator),
			reinterpret_cast<void**>(CmdListAlloc.GetAddressOf())));

        PassCB = std::make_unique<UploadBuffer<PerPassCB>>(device, passCount, true);
        MaterialBuffer = std::make_unique<UploadBuffer<MaterialData>>(device, materialCount, false);
    }

    // We cannot reset the allocator until the GPU is done processing the commands.
    // So each frame needs their own allocator.
    Microsoft::WRL::ComPtr<D3D12::ID3D12CommandAllocator> CmdListAlloc;

    // We cannot update a buffer until the GPU is done processing the commands
    // that reference it.  So each frame needs their own buffers.
    std::unique_ptr<UploadBuffer<PerPassCB>> PassCB;
    std::unique_ptr<UploadBuffer<MaterialData>> MaterialBuffer;

    // Fence value to mark commands up to this fence point.  This lets us
    // check if these frame resources are still in use by the GPU.
    std::uint64_t Fence = 0;
};