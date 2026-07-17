export module blur:blurfilter;
import std;
import shared;
import :computerootarg;

class BlurFilter
{
public:
	///<summary>
	/// The width and height should match the dimensions of the input texture to blur.
	/// Recreate when the screen is resized. 
	///</summary>
	BlurFilter(
		D3D12::ID3D12Device* device,
		Win32::UINT width,
		Win32::UINT height,
		DXGI::DXGI_FORMAT format
	) : md3dDevice{ device },
		mWidth{ width },
		mHeight{ height },
		mFormat{ format }
	{
		BuildResources();
	}

	BlurFilter(const BlurFilter& rhs) = delete;
	auto operator=(const BlurFilter& rhs) -> BlurFilter & = delete;
	~BlurFilter() = default;

	auto Output() -> D3D12::ID3D12Resource*
	{
		return mBlurMap0.Get();
	}

	void BuildDescriptors()
	{
		auto& heap = CbvSrvUavHeap::Get();

		if (mBlur0SrvIndex == -1)
		{
			mBlur0SrvIndex = heap.NextFreeIndex();
			mBlur1SrvIndex = heap.NextFreeIndex();

			mBlur0UavIndex = heap.NextFreeIndex();
			mBlur1UavIndex = heap.NextFreeIndex();
		}

		const auto mipLevels = 1u;
		CreateSrv2d(md3dDevice, mBlurMap0.Get(), mFormat, mipLevels, heap.CpuHandle(mBlur0SrvIndex));
		CreateSrv2d(md3dDevice, mBlurMap1.Get(), mFormat, mipLevels, heap.CpuHandle(mBlur1SrvIndex));

		const auto mipSlice = 0u;
		CreateUav2d(md3dDevice, mBlurMap0.Get(), mFormat, mipSlice, heap.CpuHandle(mBlur0UavIndex));
		CreateUav2d(md3dDevice, mBlurMap1.Get(), mFormat, mipSlice, heap.CpuHandle(mBlur1UavIndex));
	}

	void OnResize(Win32::UINT newWidth, Win32::UINT newHeight)
	{
		if (mWidth == newWidth and mHeight == newHeight)
			return;

		mWidth = newWidth;
		mHeight = newHeight;
		BuildResources();
		// New resource, so we need new descriptors to that resource.
		BuildDescriptors();
	}

	///<summary>
	/// Blurs the input texture blurCount times.
	///</summary>
	void Execute(D3D12::ID3D12GraphicsCommandList* cmdList,
		D3D12::ID3D12RootSignature* rootSig,
		D3D12::ID3D12Resource* passCB,
		D3D12::ID3D12PipelineState* horzBlurPSO,
		D3D12::ID3D12PipelineState* vertBlurPSO,
		D3D12::ID3D12Resource* input,
		int blurCount,
		float blurSigma
	)
	{
		auto weights = CalcGaussWeights(blurSigma);
		auto blurRadius = static_cast<int>(weights.size() / 2);

		cmdList->SetComputeRootSignature(rootSig);

		cmdList->SetComputeRootConstantBufferView(
			COMPUTE_ROOT_ARG_PASS_CBV,
			passCB->GetGPUVirtualAddress());

		auto blurCB = BlurDispatchCB{};
		std::memcpy(blurCB.gWeightVec, weights.data(), weights.size() * sizeof(float));
		blurCB.gBlurRadius = blurRadius;
		blurCB.gBlurInputIndex = mBlur0SrvIndex;
		blurCB.gBlurOutputIndex = mBlur1UavIndex;
		auto& linearAllocator = DirectX::GraphicsMemory::Get(md3dDevice);
		mHorzPassConstants = linearAllocator.AllocateConstant(blurCB);

		// Swap input/output for vertical blur pass.
		blurCB.gBlurInputIndex = mBlur1SrvIndex;
		blurCB.gBlurOutputIndex = mBlur0UavIndex;
		mVertPassConstants = linearAllocator.AllocateConstant(blurCB);

		auto transition = D3D12::CD3DX12_RESOURCE_BARRIER::Transition(input,
			D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
		cmdList->ResourceBarrier(1, &transition);

		transition = D3D12::CD3DX12_RESOURCE_BARRIER::Transition(mBlurMap0.Get(),
			D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_STATE_COPY_DEST);
		cmdList->ResourceBarrier(1, &transition);

		// Copy the input (back-buffer in this example) to BlurMap0.
		cmdList->CopyResource(mBlurMap0.Get(), input);

		transition = D3D12::CD3DX12_RESOURCE_BARRIER::Transition(mBlurMap0.Get(),
			D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_GENERIC_READ);
		cmdList->ResourceBarrier(1, &transition);

		for (auto i = 0; i < blurCount; ++i)
		{
			//
			// Horizontal Blur pass.
			transition = D3D12::CD3DX12_RESOURCE_BARRIER::Transition(mBlurMap1.Get(),
				D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
			cmdList->ResourceBarrier(1, &transition);

			cmdList->SetComputeRootConstantBufferView(
				COMPUTE_ROOT_ARG_DISPATCH_CBV,
				mHorzPassConstants.GpuAddress());

			cmdList->SetPipelineState(horzBlurPSO);

			// How many groups do we need to dispatch to cover a row of pixels, where each
			// group covers 256 pixels (the 256 is defined in the ComputeShader).
			std::uint32_t numGroupsX = (std::uint32_t)std::ceilf(mWidth / 256.0f);
			cmdList->Dispatch(numGroupsX, mHeight, 1);

			transition = D3D12::CD3DX12_RESOURCE_BARRIER::Transition(mBlurMap0.Get(),
				D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
			cmdList->ResourceBarrier(1, &transition);

			transition = D3D12::CD3DX12_RESOURCE_BARRIER::Transition(mBlurMap1.Get(),
				D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_GENERIC_READ);
			cmdList->ResourceBarrier(1, &transition);

			//
			// Vertical Blur pass.
			cmdList->SetComputeRootConstantBufferView(
				COMPUTE_ROOT_ARG_DISPATCH_CBV,
				mVertPassConstants.GpuAddress());

			cmdList->SetPipelineState(vertBlurPSO);

			// How many groups do we need to dispatch to cover a column of pixels, where each
			// group covers 256 pixels  (the 256 is defined in the ComputeShader).
			std::uint32_t numGroupsY = (std::uint32_t)ceilf(mHeight / 256.0f);
			cmdList->Dispatch(mWidth, numGroupsY, 1);

			transition = D3D12::CD3DX12_RESOURCE_BARRIER::Transition(mBlurMap0.Get(),
				D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_GENERIC_READ);
			cmdList->ResourceBarrier(1, &transition);
		}
	}

private:
	auto CalcGaussWeights(float sigma) -> std::vector<float>
	{
		auto twoSigma2 = 2.0f * sigma * sigma;

		// Estimate the blur radius based on sigma since sigma controls the "width" of the bell curve.
		auto blurRadius = static_cast<int>(ceil(2.0f * sigma));

		//assert(blurRadius <= MaxBlurRadius);

		auto weights = std::vector<float>{};
		weights.resize(2 * blurRadius + 1);

		auto weightSum = 0.0f;

		for (auto i = -blurRadius; i <= blurRadius; ++i)
		{
			auto x = static_cast<float>(i);
			weights[i + blurRadius] = std::expf(-x * x / twoSigma2);
			weightSum += weights[i + blurRadius];
		}

		// Divide by the sum so all the weights add up to 1.0.
		for (auto i = 0; i < weights.size(); ++i)
		{
			weights[i] /= weightSum;
		}

		return weights;
	}

	void BuildResources()
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
			.DepthOrArraySize = 1,
			.MipLevels = 1,
			.Format = mFormat,
			.SampleDesc = {.Count = 1, .Quality = 0},
			.Layout = D3D12::D3D12_TEXTURE_LAYOUT::D3D12_TEXTURE_LAYOUT_UNKNOWN,
			.Flags = D3D12::D3D12_RESOURCE_FLAGS::D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS
		};


		auto heapProps = D3D12::CD3DX12_HEAP_PROPERTIES(D3D12::D3D12_HEAP_TYPE::D3D12_HEAP_TYPE_DEFAULT);
		ThrowIfFailed(md3dDevice->CreateCommittedResource(
			&heapProps,
			D3D12::D3D12_HEAP_FLAGS::D3D12_HEAP_FLAG_NONE,
			&texDesc,
			D3D12::D3D12_RESOURCE_STATES::D3D12_RESOURCE_STATE_GENERIC_READ,
			nullptr,
			__uuidof(D3D12::ID3D12Resource),
			&mBlurMap0));

		ThrowIfFailed(md3dDevice->CreateCommittedResource(
			&heapProps,
			D3D12::D3D12_HEAP_FLAGS::D3D12_HEAP_FLAG_NONE,
			&texDesc,
			D3D12::D3D12_RESOURCE_STATES::D3D12_RESOURCE_STATE_GENERIC_READ,
			nullptr,
			__uuidof(D3D12::ID3D12Resource),
			&mBlurMap1));

		d3dSetDebugName(mBlurMap0.Get(), "BlurFilter::mBlurMap0");
		d3dSetDebugName(mBlurMap1.Get(), "BlurFilter::mBlurMap1");
	}

private:
	const int MaxBlurRadius = 15;

	D3D12::ID3D12Device* md3dDevice = nullptr;

	std::uint32_t mWidth = 0;
	std::uint32_t mHeight = 0;
	DXGI::DXGI_FORMAT mFormat = DXGI_FORMAT_R8G8B8A8_UNORM;

	DirectX::GraphicsResource mHorzPassConstants;
	DirectX::GraphicsResource mVertPassConstants;

	std::uint32_t mBlur0SrvIndex = -1;
	std::uint32_t mBlur1SrvIndex = -1;

	std::uint32_t mBlur0UavIndex = -1;
	std::uint32_t mBlur1UavIndex = -1;

	// Two for ping-ponging the textures.
	Microsoft::WRL::ComPtr<D3D12::ID3D12Resource> mBlurMap0 = nullptr;
	Microsoft::WRL::ComPtr<D3D12::ID3D12Resource> mBlurMap1 = nullptr;
};