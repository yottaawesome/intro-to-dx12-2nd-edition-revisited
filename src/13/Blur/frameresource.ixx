export module blur:frameresource;
import std;
import shared;

// Stores the resources needed for the CPU to build the command lists
// for a frame.  
struct FrameResource
{
	FrameResource(D3D12::ID3D12Device* device, std::uint32_t passCount, std::uint32_t materialCount, std::uint32_t waveVertCount)
	{
		ThrowIfFailed(device->CreateCommandAllocator(
			D3D12_COMMAND_LIST_TYPE_DIRECT,
			__uuidof(D3D12::ID3D12CommandAllocator), (void**)CmdListAlloc.GetAddressOf()));

		PassCB = std::make_unique<UploadBuffer<PerPassCB>>(device, passCount, true);
		MaterialBuffer = std::make_unique<UploadBuffer<MaterialData>>(device, materialCount, false);
		WavesVB = std::make_unique<UploadBuffer<ModelVertex>>(device, waveVertCount, false);
	}

	// We cannot reset the allocator until the GPU is done processing the commands.
	// So each frame needs their own allocator.
	Microsoft::WRL::ComPtr<D3D12::ID3D12CommandAllocator> CmdListAlloc;

	// We cannot update a buffer until the GPU is done processing the commands
	// that reference it.  So each frame needs their own buffers.
	std::unique_ptr<UploadBuffer<PerPassCB>> PassCB;
	std::unique_ptr<UploadBuffer<MaterialData>> MaterialBuffer;

	// We cannot update a dynamic vertex buffer until the GPU is done processing
	// the commands that reference it.  So each frame needs their own.
	std::unique_ptr<UploadBuffer<ModelVertex>> WavesVB;

	// Fence value to mark commands up to this fence point.  This lets us
	// check if these frame resources are still in use by the GPU.
	std::uint64_t Fence = 0;
};