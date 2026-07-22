export module dynamiccubemap:cuberendertarget;
import std;
import shared;

enum class CubeMapFace : int
{
	PositiveX = 0,
	NegativeX = 1,
	PositiveY = 2,
	NegativeY = 3,
	PositiveZ = 4,
	NegativeZ = 5
};

class CubeRenderTarget
{
public:
	CubeRenderTarget(
		D3D12::ID3D12Device* device,
		std::uint32_t width, 
		std::uint32_t height,
		DXGI::DXGI_FORMAT format
	)
	{
		md3dDevice = device;
		mWidth = width;
		mHeight = height;
		mFormat = format;
		mViewport = { 0.0f, 0.0f, (float)width, (float)height, 0.0f, 1.0f };
		mScissorRect = { 0, 0, (int)width, (int)height };
		BuildResource();
	}

	CubeRenderTarget(const CubeRenderTarget& rhs) = delete;
	CubeRenderTarget& operator=(const CubeRenderTarget& rhs) = delete;

	auto Width()const -> std::uint32_t
	{
		return mWidth;
	}

	auto Height()const -> std::uint32_t
	{
		return mHeight;
	}

	auto Resource() -> D3D12::ID3D12Resource*
	{
		return mCubeMap.Get();
	}

	auto BindlessIndex()const -> std::uint32_t
	{
		return mBindlessIndex;
	}

	auto Srv() -> D3D12::CD3DX12_GPU_DESCRIPTOR_HANDLE
	{
		return mhGpuSrv;
	}

	auto Rtv(int faceIndex) -> D3D12::CD3DX12_CPU_DESCRIPTOR_HANDLE
	{
		return mhCpuRtv[faceIndex];
	}

	auto Viewport()const -> D3D12::D3D12_VIEWPORT
	{
		return mViewport;
	}
	
	auto ScissorRect()const -> D3D12::D3D12_RECT
	{
		return mScissorRect;
	}

	void BuildDescriptors(D3D12::CD3DX12_CPU_DESCRIPTOR_HANDLE hCpuRtv[6])
	{
		auto& bindlessHeap = CbvSrvUavHeap::Get();
		mBindlessIndex = bindlessHeap.NextFreeIndex();
		// Save references to the descriptors.
		mhCpuSrv = bindlessHeap.CpuHandle(mBindlessIndex);
		mhGpuSrv = bindlessHeap.GpuHandle(mBindlessIndex);
		for (int i = 0; i < 6; ++i)
			mhCpuRtv[i] = hCpuRtv[i];
		//  Create the descriptors
		BuildDescriptors();
	}

	void OnResize(std::uint32_t newWidth, std::uint32_t newHeight)
	{
		if ((mWidth != newWidth) or (mHeight != newHeight))
		{
			mWidth = newWidth;
			mHeight = newHeight;
			BuildResource();
			// New resource, so we need new descriptors to that resource.
			BuildDescriptors();
		}
	}

private:
	void BuildDescriptors()
	{
		auto srvDesc = D3D12::D3D12_SHADER_RESOURCE_VIEW_DESC{
			.Format = mFormat,
			.ViewDimension = D3D12::D3D12_SRV_DIMENSION::D3D12_SRV_DIMENSION_TEXTURECUBE,
			.Shader4ComponentMapping = D3D12::DefaultShader4ComponentMapping,
			.TextureCube = {
				.MostDetailedMip = 0,
				.MipLevels = 1,
				.ResourceMinLODClamp = 0.0f
			}
		};

		// Create SRV to the entire cubemap resource.
		md3dDevice->CreateShaderResourceView(mCubeMap.Get(), &srvDesc, mhCpuSrv);

		// Create RTV to each cube face.
		for (int i = 0; i < 6; ++i)
		{
			auto rtvDesc = D3D12::D3D12_RENDER_TARGET_VIEW_DESC{
				.Format = mFormat,
				.ViewDimension = D3D12::D3D12_RTV_DIMENSION::D3D12_RTV_DIMENSION_TEXTURE2DARRAY,
				.Texture2DArray = {
					.MipSlice = 0,
					.PlaneSlice = 0
				}
			};
			// Render target to ith element.
			rtvDesc.Texture2DArray.FirstArraySlice = i;
			// Only view one element of the array.
			rtvDesc.Texture2DArray.ArraySize = 1;
			// Create RTV to ith cubemap face.
			md3dDevice->CreateRenderTargetView(mCubeMap.Get(), &rtvDesc, mhCpuRtv[i]);
		}
	}

	void BuildResource()
	{
		// Note, compressed formats cannot be used for UAV.  We get error like:
		// ERROR: ID3D11Device::CreateTexture2D: The format (0x4d, BC3_UNORM) 
		// cannot be bound as an UnorderedAccessView, or cast to a format that
		// could be bound as an UnorderedAccessView.  Therefore this format 
		// does not support D3D11_BIND_UNORDERED_ACCESS.
		auto texDesc = D3D12::D3D12_RESOURCE_DESC{
			.Dimension = D3D12::D3D12_RESOURCE_DIMENSION::D3D12_RESOURCE_DIMENSION_TEXTURE2D,
			.Alignment = 0,
			.Width = mWidth,
			.Height = mHeight,
			.DepthOrArraySize = 6,
			.MipLevels = 1,
			.Format = mFormat,
			.SampleDesc = {1, 0},
			.Layout = D3D12::D3D12_TEXTURE_LAYOUT::D3D12_TEXTURE_LAYOUT_UNKNOWN,
			.Flags = D3D12::D3D12_RESOURCE_FLAGS::D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET
		};

		auto optClear = D3D12::D3D12_CLEAR_VALUE{
			.Format = mFormat,
			.Color = {0.0f, 0.0f, 0.0f, 1.0f }
		};

		auto heapProps = D3D12::CD3DX12_HEAP_PROPERTIES{D3D12::D3D12_HEAP_TYPE::D3D12_HEAP_TYPE_DEFAULT};
		ThrowIfFailed(md3dDevice->CreateCommittedResource(
			&heapProps,
			D3D12_HEAP_FLAG_NONE,
			&texDesc,
			D3D12_RESOURCE_STATE_GENERIC_READ,
			&optClear,
			__uuidof(D3D12::ID3D12Resource), 
			&mCubeMap));
	}

private:
	D3D12::ID3D12Device* md3dDevice = nullptr;

	D3D12::D3D12_VIEWPORT mViewport;
	D3D12::D3D12_RECT mScissorRect;

	std::uint32_t mWidth = 0;
	std::uint32_t mHeight = 0;
	DXGI::DXGI_FORMAT mFormat = DXGI_FORMAT_R8G8B8A8_UNORM;

	std::uint32_t mBindlessIndex = -1;
	D3D12::CD3DX12_CPU_DESCRIPTOR_HANDLE mhCpuSrv;
	D3D12::CD3DX12_GPU_DESCRIPTOR_HANDLE mhGpuSrv;
	D3D12::CD3DX12_CPU_DESCRIPTOR_HANDLE mhCpuRtv[6];

	Microsoft::WRL::ComPtr<D3D12::ID3D12Resource> mCubeMap;
};
