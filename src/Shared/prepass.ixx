export module shared:prepass;
import std;
import :win32;
import :d3dutil;
import :descriptorutil;
import :uploadbuffer;

// Manages 
export class Prepass
{
public:
	Prepass(D3D12::ID3D12Device* device)
		: md3dDevice(device)
	{}

	Prepass(const Prepass& rhs) = delete;
	Prepass& operator=(const Prepass& rhs) = delete;

	auto GetSceneNormalMap()const -> D3D12::ID3D12Resource*
	{
		return mSceneNormalMap.Get();
	}

	auto GetSceneNormalMapBindlessIndex()const -> std::uint32_t
	{
		return mSceneNormalMapBindlessIndex;
	}

	auto GetSceneDepthMapBindlessIndex()const -> std::uint32_t
	{
		return mSceneDepthMapBindlessIndex;
	}

	auto GetSceneNormalMapRtv()const -> D3D12::CD3DX12_CPU_DESCRIPTOR_HANDLE
	{
		return mSceneNormalMapRtv;
	}

	void AllocateDescriptors(D3D12::CD3DX12_CPU_DESCRIPTOR_HANDLE sceneNormalMapRtv)
	{
		mSceneNormalMapRtv = sceneNormalMapRtv;

		auto& cbvSrvUavHeap = CbvSrvUavHeap::Get();
		//assert(cbvSrvUavHeap.IsInitialized());

		mSceneNormalMapBindlessIndex = cbvSrvUavHeap.NextFreeIndex();

		// Create SRV to depth buffer so we can sample it in a shader. When we start to sample from it,
		// we need to be done writing to it.
		mSceneDepthMapBindlessIndex = cbvSrvUavHeap.NextFreeIndex();
	}

	void OnResize(std::uint32_t newWidth, std::uint32_t newHeight, D3D12::ID3D12Resource* depthStencilBuffer)
	{
		mWidth = newWidth;
		mHeight = newHeight;
		BuildResources();
		BuildDescriptors(depthStencilBuffer);
	}

private:
	void BuildResources()
	{
		// Free the old resources if they exist.
		mSceneNormalMap = nullptr;

		auto texDesc = D3D12::D3D12_RESOURCE_DESC{
			.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D,
			.Alignment = 0,
			.Width = mWidth,
			.Height = mHeight,
			.DepthOrArraySize = 1,
			.MipLevels = 1,
			.Format = SceneNormalMapFormat,
			.SampleDesc = { 1, 0 },
			.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN,
			.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET
		};
		
		auto normalClearColor = std::array{ 0.0f, 0.0f, 1.0f, 0.0f };
		auto optClear = D3D12::CD3DX12_CLEAR_VALUE(SceneNormalMapFormat, normalClearColor.data());
		auto heapProps = D3D12::CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
		ThrowIfFailed(md3dDevice->CreateCommittedResource(
			&heapProps,
			D3D12_HEAP_FLAG_NONE,
			&texDesc,
			D3D12_RESOURCE_STATE_GENERIC_READ,
			&optClear,
			__uuidof(D3D12::ID3D12Resource),
			&mSceneNormalMap));
	}

	void BuildDescriptors(D3D12::ID3D12Resource* depthStencilBuffer)
	{
		CbvSrvUavHeap& cbvSrvUavHeap = CbvSrvUavHeap::Get();
		//assert(cbvSrvUavHeap.IsInitialized());

		CreateSrv2d(md3dDevice, mSceneNormalMap.Get(), SceneNormalMapFormat, 1, cbvSrvUavHeap.CpuHandle(mSceneNormalMapBindlessIndex));
		CreateRtv2d(md3dDevice, mSceneNormalMap.Get(), SceneNormalMapFormat, 0, mSceneNormalMapRtv);
		CreateSrv2d(md3dDevice, depthStencilBuffer, DXGI_FORMAT_R24_UNORM_X8_TYPELESS, 1, cbvSrvUavHeap.CpuHandle(mSceneDepthMapBindlessIndex));
	}

private:
	D3D12::ID3D12Device* md3dDevice = nullptr;

	std::uint32_t mWidth = 0;
	std::uint32_t mHeight = 0;

	D3D12::CD3DX12_CPU_DESCRIPTOR_HANDLE mSceneNormalMapRtv;

	Microsoft::WRL::ComPtr<D3D12::ID3D12Resource> mSceneNormalMap = nullptr;
	std::uint32_t mSceneNormalMapBindlessIndex = -1;
	std::uint32_t mSceneDepthMapBindlessIndex = -1;
};