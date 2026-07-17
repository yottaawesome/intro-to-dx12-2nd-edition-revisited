export module blur;
import std;
import shared;

constexpr auto CBV_SRV_UAV_HEAP_CAPACITY = 16384u;

//
// Define named offsets to root parameters in root signature for readability.
//

enum GFX_ROOT_ARG
{
	GFX_ROOT_ARG_OBJECT_CBV = 0,
	GFX_ROOT_ARG_PASS_CBV,
	GFX_ROOT_ARG_MATERIAL_SRV,
	GFX_ROOT_ARG_COUNT
};

enum COMPUTE_ROOT_ARG
{
	COMPUTE_ROOT_ARG_DISPATCH_CBV = 0,
	COMPUTE_ROOT_ARG_PASS_CBV,
	COMPUTE_ROOT_ARG_PASS_EXTRA_CBV,
	COMPUTE_ROOT_ARG_COUNT
};

struct WavesDispatchCB
{
	std::uint32_t gBufferIndexA;
	std::uint32_t gBufferIndexB;
	std::uint32_t gBufferIndexOutput;
	std::uint32_t DispatchCB_Pad0;
};

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
	auto operator=(const BlurFilter& rhs) -> BlurFilter& = delete;
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

//***************************************************************************************
// GpuWaves.h by Frank Luna (C) 2011 All Rights Reserved.
//
// Performs the calculations for the wave simulation using the ComputeShader on the GPU.  
// The solution is saved to a floating-point texture.  The client must then set this 
// texture as a SRV and do the displacement mapping in the vertex shader over a grid.
//***************************************************************************************
class GpuWaves
{
public:
	// Note that m,n should be divisible by 16 so there is no 
	// remainder when we divide into thread groups.
	GpuWaves(
		D3D12::ID3D12Device* device, 
		DirectX::ResourceUploadBatch& uploadBatch, 
		int m, 
		int n, 
		float dx, 
		float dt, 
		float speed, 
		float damping
	)
	{
		md3dDevice = device;

		mNumRows = m;
		mNumCols = n;

		//assert((m * n) % 256 == 0);

		mVertexCount = m * n;
		mTriangleCount = (m - 1) * (n - 1) * 2;

		mTimeStep = dt;
		mSpatialStep = dx;

		auto d = damping * dt + 2.0f;
		auto e = (speed * speed) * (dt * dt) / (dx * dx);
		mK[0] = (damping * dt - 2.0f) / d;
		mK[1] = (4.0f - 8.0f * e) / d;
		mK[2] = (2.0f * e) / d;

		BuildResources(uploadBatch);
	}
	GpuWaves(const GpuWaves& rhs) = delete;
	GpuWaves& operator=(const GpuWaves& rhs) = delete;

	auto RowCount()const -> std::uint32_t
	{
		return mNumRows;
	}
	auto ColumnCount()const -> std::uint32_t
	{
		return mNumCols;
	}
	auto VertexCount()const -> std::uint32_t
	{
		return mVertexCount;
	}
	auto TriangleCount()const -> std::uint32_t
	{
		return mTriangleCount;
	}
	auto Width()const -> float
	{
		return mNumCols * mSpatialStep;
	}
	auto Depth()const -> float 
	{
		return mNumRows * mSpatialStep;
	}
	auto SpatialStep()const -> float
	{
		return mSpatialStep;
	}

	auto DisplacementMapSrvIndex()const -> std::uint32_t
	{
		// It is mCurrSolSrvIndex here instead of mNextSolSrvIndex
		// because of the ping-pong done at the end of GpuWaves::Update().
		return mCurrSolSrvIndex;
	}

	void SetConstants(float speed, float damping)
	{
		auto d = damping * mTimeStep + 2.0f;
		auto e = (speed * speed) * (mTimeStep * mTimeStep) / (mSpatialStep * mSpatialStep);
		mK[0] = (damping * mTimeStep - 2.0f) / d;
		mK[1] = (4.0f - 8.0f * e) / d;
		mK[2] = (2.0f * e) / d;
	}

	void BuildResources(DirectX::ResourceUploadBatch& uploadBatch)
	{
		// All the textures for the wave simulation will be bound as a shader resource and
		// unordered access view at some point since we ping-pong the buffers.

		auto zeroFloats = std::vector<float>(mVertexCount, 0.0f);
		auto initData = D3D12::D3D12_SUBRESOURCE_DATA{
			.pData = zeroFloats.data(),
			.RowPitch = mNumCols * sizeof(float),
			.SlicePitch = mNumCols * sizeof(float) * mNumRows
		};
		

		CreateTextureFromMemory(
			md3dDevice, uploadBatch,
			mNumCols, mNumRows,
			DXGI_FORMAT_R32_FLOAT,
			initData,
			mPrevSol.GetAddressOf(),
			false, // generateMips,
			D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
			D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);

		CreateTextureFromMemory(
			md3dDevice, uploadBatch,
			mNumCols, mNumRows,
			DXGI_FORMAT_R32_FLOAT,
			initData,
			mCurrSol.GetAddressOf(),
			false, // generateMips,
			D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
			D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);

		CreateTextureFromMemory(
			md3dDevice, uploadBatch,
			mNumCols, mNumRows,
			DXGI_FORMAT_R32_FLOAT,
			initData,
			mNextSol.GetAddressOf(),
			false, // generateMips,
			D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
			D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);

		d3dSetDebugName(mPrevSol.Get(), "GpuWaves::mPrevSol");
		d3dSetDebugName(mCurrSol.Get(), "GpuWaves::mCurrSol");
		d3dSetDebugName(mNextSol.Get(), "GpuWaves::mNextSol");
	}
	void BuildDescriptors()
	{
		auto& heap = CbvSrvUavHeap::Get();

		mPrevSolSrvIndex = heap.NextFreeIndex();
		mCurrSolSrvIndex = heap.NextFreeIndex();
		mNextSolSrvIndex = heap.NextFreeIndex();

		mPrevSolUavIndex = heap.NextFreeIndex();
		mCurrSolUavIndex = heap.NextFreeIndex();
		mNextSolUavIndex = heap.NextFreeIndex();

		const auto mipLevels = 1u;
		CreateSrv2d(md3dDevice, mPrevSol.Get(), DXGI_FORMAT_R32_FLOAT, mipLevels, heap.CpuHandle(mPrevSolSrvIndex));
		CreateSrv2d(md3dDevice, mCurrSol.Get(), DXGI_FORMAT_R32_FLOAT, mipLevels, heap.CpuHandle(mCurrSolSrvIndex));
		CreateSrv2d(md3dDevice, mNextSol.Get(), DXGI_FORMAT_R32_FLOAT, mipLevels, heap.CpuHandle(mNextSolSrvIndex));

		const auto mipSlice = 0u;
		CreateUav2d(md3dDevice, mPrevSol.Get(), DXGI_FORMAT_R32_FLOAT, mipSlice, heap.CpuHandle(mPrevSolUavIndex));
		CreateUav2d(md3dDevice, mCurrSol.Get(), DXGI_FORMAT_R32_FLOAT, mipSlice, heap.CpuHandle(mCurrSolUavIndex));
		CreateUav2d(md3dDevice, mNextSol.Get(), DXGI_FORMAT_R32_FLOAT, mipSlice, heap.CpuHandle(mNextSolUavIndex));
	}

	void Update(
		const GameTimer& gt,
		D3D12::ID3D12GraphicsCommandList* cmdList,
		D3D12::ID3D12RootSignature* rootSig,
		D3D12::ID3D12Resource* passCB,
		D3D12::ID3D12PipelineState* pso
	)
	{
		static float t = 0.0f;

		// Accumulate time.
		t += gt.DeltaTime();

		// Only update the simulation at the specified time step.
		if (t < mTimeStep)
			return;
		cmdList->SetPipelineState(pso);
		cmdList->SetComputeRootSignature(rootSig);

		cmdList->SetComputeRootConstantBufferView(
			COMPUTE_ROOT_ARG_PASS_CBV,
			passCB->GetGPUVirtualAddress());

		auto wavesCB = GpuWavesCB{
			.gWaveConstant0 = mK[0],
			.gWaveConstant1 = mK[1],
			.gWaveConstant2 = mK[2],
			.gDisturbMag = 0.0f,
			.gDisturbIndex = DirectX::XMUINT2(0, 0),
			.gGridSize = DirectX::XMUINT2(mNumCols, mNumRows),
			.gPrevSolIndex = mPrevSolUavIndex,
			.gCurrSolIndex = mCurrSolUavIndex,
			.gOutputIndex = mNextSolUavIndex
		};

		auto& linearAllocator = DirectX::GraphicsMemory::Get(md3dDevice);
		mUpdateConstantsMemHandle = linearAllocator.AllocateConstant(wavesCB);
		cmdList->SetComputeRootConstantBufferView(
			COMPUTE_ROOT_ARG_DISPATCH_CBV,
			mUpdateConstantsMemHandle.GpuAddress());

		auto transition = D3D12::CD3DX12_RESOURCE_BARRIER::Transition(
			mPrevSol.Get(),
			D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
			D3D12_RESOURCE_STATE_GENERIC_READ);
		cmdList->ResourceBarrier(1, &transition);

		transition = D3D12::CD3DX12_RESOURCE_BARRIER::Transition(
			mCurrSol.Get(),
			D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
			D3D12_RESOURCE_STATE_GENERIC_READ);
		cmdList->ResourceBarrier(1, &transition);

		transition = D3D12::CD3DX12_RESOURCE_BARRIER::Transition(
			mPrevSol.Get(),
			D3D12_RESOURCE_STATE_GENERIC_READ,
			D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		cmdList->ResourceBarrier(1, &transition);

		transition = D3D12::CD3DX12_RESOURCE_BARRIER::Transition(
			mCurrSol.Get(),
			D3D12_RESOURCE_STATE_GENERIC_READ,
			D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		cmdList->ResourceBarrier(1, &transition);

		// How many groups do we need to dispatch to cover the wave grid.  
		// Note that mNumRows and mNumCols should be divisible by 16
		// so there is no remainder.
		auto numGroupsX = std::uint32_t{ mNumCols / 16 };
		auto numGroupsY = std::uint32_t{ mNumRows / 16 };
		cmdList->Dispatch(numGroupsX, numGroupsY, 1);

		//
		// Ping-pong buffers in preparation for the next update.
		// The previous solution is no longer needed and becomes the target of the next solution in the next update.
		// The current solution becomes the previous solution.
		// The next solution becomes the current solution.
		//

		auto resTemp = mPrevSol;
		mPrevSol = mCurrSol;
		mCurrSol = mNextSol;
		mNextSol = resTemp;

		auto srvTempIndex = mPrevSolSrvIndex;
		mPrevSolSrvIndex = mCurrSolSrvIndex;
		mCurrSolSrvIndex = mNextSolSrvIndex;
		mNextSolSrvIndex = srvTempIndex;

		auto uavTempIndex = mPrevSolUavIndex;
		mPrevSolUavIndex = mCurrSolUavIndex;
		mCurrSolUavIndex = mNextSolUavIndex;
		mNextSolUavIndex = uavTempIndex;

		t = 0.0f; // reset time
	}

	void Disturb(
		D3D12::ID3D12GraphicsCommandList* cmdList,
		D3D12::ID3D12RootSignature* rootSig,
		D3D12::ID3D12Resource* passCB,
		D3D12::ID3D12PipelineState* pso,
		std::uint32_t i, 
		std::uint32_t j,
		float magnitude
	)
	{
		cmdList->SetPipelineState(pso);
		cmdList->SetComputeRootSignature(rootSig);

		cmdList->SetComputeRootConstantBufferView(
			COMPUTE_ROOT_ARG_PASS_CBV,
			passCB->GetGPUVirtualAddress());

		auto wavesCB = GpuWavesCB{
			.gWaveConstant0 = mK[0],
			.gWaveConstant1 = mK[1],
			.gWaveConstant2 = mK[2],
			.gDisturbMag = magnitude,
			.gDisturbIndex = DirectX::XMUINT2(j, i),
			.gGridSize = DirectX::XMUINT2(mNumCols, mNumRows),
			.gPrevSolIndex = mPrevSolUavIndex,
			.gCurrSolIndex = mCurrSolUavIndex,
			.gOutputIndex = mNextSolUavIndex
		};
		

		auto& linearAllocator = DirectX::GraphicsMemory::Get(md3dDevice);
		mDisturbConstantsMemHandle = linearAllocator.AllocateConstant(wavesCB);
		cmdList->SetComputeRootConstantBufferView(
			COMPUTE_ROOT_ARG_DISPATCH_CBV,
			mDisturbConstantsMemHandle.GpuAddress());

		// One thread group kicks off one thread, which displaces the height of one
		// vertex and its neighbors.
		cmdList->Dispatch(1, 1, 1);
	}

private:
	std::uint32_t mNumRows;
	std::uint32_t mNumCols;

	std::uint32_t mVertexCount;
	std::uint32_t mTriangleCount;

	// Simulation constants we can precompute.
	float mK[3];

	float mTimeStep;
	float mSpatialStep;

	D3D12::ID3D12Device* md3dDevice = nullptr;

	DirectX::GraphicsResource mUpdateConstantsMemHandle;
	DirectX::GraphicsResource mDisturbConstantsMemHandle;

	// For rendering we just need to read from the resource, so a 
	// SRV is likely more performant for readonly. It also means we
	// can sample the texture if we needed to.
	std::uint32_t mPrevSolSrvIndex = -1;
	std::uint32_t mCurrSolSrvIndex = -1;
	std::uint32_t mNextSolSrvIndex = -1;

	std::uint32_t mPrevSolUavIndex = -1;
	std::uint32_t mCurrSolUavIndex = -1;
	std::uint32_t mNextSolUavIndex = -1;

	// Two for ping-ponging the textures.
	Microsoft::WRL::ComPtr<D3D12::ID3D12Resource> mPrevSol = nullptr;
	Microsoft::WRL::ComPtr<D3D12::ID3D12Resource> mCurrSol = nullptr;
	Microsoft::WRL::ComPtr<D3D12::ID3D12Resource> mNextSol = nullptr;
};

enum class RenderLayer : int
{
	Opaque = 0,
	Transparent,
	AlphaTested,
	GpuWaves,
	Debug,
	Sky,
	Count
};

// Lightweight structure stores parameters to draw a shape.  This will
// vary from app-to-app.
struct RenderItem
{
	RenderItem() = default;
	RenderItem(const RenderItem& rhs) = delete;

	// World matrix of the shape that describes the object's local space
	// relative to the world space, which defines the position, orientation,
	// and scale of the object in the world.
	DirectX::XMFLOAT4X4 World = MathHelper::Identity4x4;

	DirectX::XMFLOAT4X4 TexTransform = MathHelper::Identity4x4;

	DirectX::XMUINT4 MiscUint4 = { 0, 0, 0, 0 };
	DirectX::XMFLOAT4 MiscFloat4 = { 0.0f, 0.0f, 0.0f, 0.0f };

	PerObjectCB ObjectConstants;

	// Handle to memory in linear allocator.
	DirectX::GraphicsResource MemHandleToObjectCB;

	Material* Mat = nullptr;
	MeshGeometry* Geo = nullptr;

	// Primitive topology.
	D3D::D3D_PRIMITIVE_TOPOLOGY PrimitiveType = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;

	// DrawIndexedInstanced parameters.
	std::uint32_t IndexCount = 0;
	std::uint32_t StartIndexLocation = 0;
	int BaseVertexLocation = 0;
};

export class BlurApp : public D3DApp
{
public:
	BlurApp(HINSTANCE hInstance)
		: D3DApp(hInstance)
	{
		Initialize();
	}
	BlurApp(const BlurApp& rhs) = delete;
	BlurApp& operator=(const BlurApp& rhs) = delete;
	~BlurApp()
	{
		if (md3dDevice != nullptr)
			FlushCommandQueue();
	}

	void Initialize()override
	{
		D3DApp::Initialize();

		// We will upload on the direct queue for the book samples, but 
		// copy queue would be better for real game.
		mUploadBatch->Begin(D3D12_COMMAND_LIST_TYPE_DIRECT);

		mWaves = std::make_unique<GpuWaves>(
			md3dDevice.Get(),
			*mUploadBatch,
			256, 256, 0.25f, 0.016f, mWaveSpeed, mWaveDamping);

		mBlurFilter = std::make_unique<BlurFilter>(
			md3dDevice.Get(),
			mClientWidth, mClientHeight,
			DXGI_FORMAT_R8G8B8A8_UNORM);

		LoadTextures();

		auto shapeGeo = std::unique_ptr<MeshGeometry>{d3dUtil::BuildShapeGeometry(md3dDevice.Get(), *mUploadBatch.get())};
		mGeometries[shapeGeo->Name] = std::move(shapeGeo);

		auto landGeo = std::unique_ptr<MeshGeometry>{BuildLandGeometry(md3dDevice.Get(), *mUploadBatch.get())};
		mGeometries[landGeo->Name] = std::move(landGeo);

		auto waveGeo = std::unique_ptr<MeshGeometry>{BuildWaveGeometry(md3dDevice.Get(), *mUploadBatch.get())};
		mGeometries[waveGeo->Name] = std::move(waveGeo);

		// Kick off upload work asyncronously.
		auto result = std::future<void>{mUploadBatch->End(mCommandQueue.Get())};

		// Other init work...
		BuildRootSignature();
		BuildCbvSrvUavDescriptorHeap();
		BuildShadersAndInputLayout();
		BuildMaterials();
		BuildRenderItems();
		BuildFrameResources();
		BuildPSOs();

		// Block until the upload work is complete.
		result.wait();
	}

private:
	void CreateRtvAndDsvDescriptorHeaps()override
	{
		mRtvHeap.Init(md3dDevice.Get(),D3D12::D3D12_DESCRIPTOR_HEAP_TYPE::D3D12_DESCRIPTOR_HEAP_TYPE_RTV, SwapChainBufferCount);
		mDsvHeap.Init(md3dDevice.Get(), D3D12::D3D12_DESCRIPTOR_HEAP_TYPE::D3D12_DESCRIPTOR_HEAP_TYPE_DSV, SwapChainBufferCount);
	}

	void OnResize()override
	{
		D3DApp::OnResize();

		// The window resized, so update the aspect ratio and recompute the projection matrix.
		auto P = DirectX::XMMatrixPerspectiveFovLH(0.25f * MathHelper::Pi, AspectRatio(), 1.0f, 1000.0f);
		DirectX::XMStoreFloat4x4(&mProj, P);

		if (CbvSrvUavHeap::Get().IsInitialized())
			mBlurFilter->OnResize(mClientWidth, mClientHeight);
	}

	void Update(const GameTimer& gt)override
	{
		OnKeyboardInput(gt);
		UpdateCamera(gt);

		// Cycle through the circular frame resource array.
		mCurrFrameResourceIndex = (mCurrFrameResourceIndex + 1) % gNumFrameResources;
		mCurrFrameResource = mFrameResources[mCurrFrameResourceIndex].get();

		// Has the GPU finished processing the commands of the current frame resource?
		// If not, wait until the GPU has completed commands up to this fence point.
		if (mCurrFrameResource->Fence != 0 and mFence->GetCompletedValue() < mCurrFrameResource->Fence)
		{
			auto event = Event{};
			ThrowIfFailed(mFence->SetEventOnCompletion(mCurrFrameResource->Fence, event.Get()));
			event.Wait();
		}

		//
		// Animate the lights.
		//

		mLightRotationAngle += 0.1f * gt.DeltaTime();

		auto R = DirectX::XMMatrixRotationY(mLightRotationAngle);
		for (int i = 0; i < 3; ++i)
		{
			auto lightDir = DirectX::XMVECTOR{DirectX::XMLoadFloat3(&mBaseLightDirections[i])};
			lightDir = DirectX::XMVector3TransformNormal(lightDir, R);
			DirectX::XMStoreFloat3(&mRotatedLightDirections[i], lightDir);
		}

		AnimateMaterials(gt);
		UpdatePerObjectCB(gt);
		UpdateMaterialBuffer(gt);
		UpdateMainPassCB(gt);
	}

	void Draw(const GameTimer& gt)override
	{
		auto& cbvSrvUavHeap = CbvSrvUavHeap::Get();
		auto& samHeap = SamplerHeap::Get();

		UpdateImgui(gt);

		auto cmdListAlloc = mCurrFrameResource->CmdListAlloc;

		// Reuse the memory associated with command recording.
		// We can only reset when the associated command lists have finished execution on the GPU.
		ThrowIfFailed(cmdListAlloc->Reset());

		// A command list can be reset after it has been added to the command queue via ExecuteCommandList.
		// Reusing the command list reuses memory.
		ThrowIfFailed(mCommandList->Reset(cmdListAlloc.Get(), mPSOs["opaque"].Get()));

		auto descriptorHeaps = std::array{ cbvSrvUavHeap.GetD3dHeap(), samHeap.GetD3dHeap() };
		mCommandList->SetDescriptorHeaps(static_cast<std::uint32_t>(descriptorHeaps.size()), descriptorHeaps.data());

		auto passCB = mCurrFrameResource->PassCB->Resource();

		UpdateWavesGPU(gt, passCB);

		mCommandList->SetGraphicsRootSignature(mRootSignature.Get());

		// Bind all the materials used in this scene.  For structured buffers, we can bypass the heap and 
		// set as a root descriptor.
		auto matBuffer = mCurrFrameResource->MaterialBuffer->Resource();
		mCommandList->SetGraphicsRootShaderResourceView(GFX_ROOT_ARG_MATERIAL_SRV, matBuffer->GetGPUVirtualAddress());

		mCommandList->RSSetViewports(1, &mScreenViewport);
		mCommandList->RSSetScissorRects(1, &mScissorRect);

		// Indicate a state transition on the resource usage.
		auto transition = D3D12::CD3DX12_RESOURCE_BARRIER::Transition(CurrentBackBuffer(),
			D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
		mCommandList->ResourceBarrier(1, &transition);

		// Clear the back buffer and depth buffer.
		float clearColor[4] = { 0.0f, 0.0f, 0.2f, 0.0f };
		if (mFogEnabled)
		{
			// Use fog color for background
			clearColor[0] = mFogColor.x;
			clearColor[1] = mFogColor.y;
			clearColor[2] = mFogColor.z;
		}
		mCommandList->ClearRenderTargetView(CurrentBackBufferView(), clearColor, 0, nullptr);
		mCommandList->ClearDepthStencilView(DepthStencilView(), 
			D3D12::D3D12_CLEAR_FLAGS{ D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL }, 
			1.0f, 
			0, 
			0, 
			nullptr);

		// Specify the buffers we are going to render to.
		auto cbbv = CurrentBackBufferView();
		auto dsv = DepthStencilView();
		mCommandList->OMSetRenderTargets(1, &cbbv, true, &dsv);


		mCommandList->SetGraphicsRootConstantBufferView(GFX_ROOT_ARG_PASS_CBV, passCB->GetGPUVirtualAddress());

		mCommandList->SetPipelineState(
			mDrawWireframe ?
			mPSOs["opaque_wireframe"].Get() :
			mPSOs["opaque"].Get());
		DrawRenderItems(mCommandList.Get(), mRitemLayer[(int)RenderLayer::Opaque]);

		mCommandList->SetPipelineState(
			mDrawWireframe ?
			mPSOs["opaque_wireframe"].Get() :
			mPSOs["alphaTested"].Get());
		DrawRenderItems(mCommandList.Get(), mRitemLayer[(int)RenderLayer::AlphaTested]);

		mCommandList->SetPipelineState(
			mDrawWireframe ?
			mPSOs["opaque_wireframe"].Get() :
			mPSOs["transparent"].Get());
		DrawRenderItems(mCommandList.Get(), mRitemLayer[(int)RenderLayer::Transparent]);

		mCommandList->SetPipelineState(
			mDrawWireframe ?
			mPSOs["waves_wireframe"].Get() :
			mPSOs["waves_transparent"].Get());
		DrawRenderItems(mCommandList.Get(), mRitemLayer[(int)RenderLayer::GpuWaves]);

		// Note: This changes the back buffer state to D3D12_RESOURCE_STATE_COPY_SOURCE.
		mBlurFilter->Execute(
			mCommandList.Get(),
			mComputeRootSignature.Get(),
			passCB,
			mPSOs["horzBlur"].Get(),
			mPSOs["vertBlur"].Get(),
			CurrentBackBuffer(),
			mBlurCount,
			mBlurSigma);

		// Prepare to copy blurred output to the back buffer.
		transition = D3D12::CD3DX12_RESOURCE_BARRIER::Transition(CurrentBackBuffer(),
			D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
		mCommandList->ResourceBarrier(1, &transition);

		mCommandList->CopyResource(CurrentBackBuffer(), mBlurFilter->Output());

		// Transition to RENDER_TARGET state.
		transition = D3D12::CD3DX12_RESOURCE_BARRIER::Transition(CurrentBackBuffer(),
			D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_RENDER_TARGET);
		mCommandList->ResourceBarrier(1, &transition);

		// Draw imgui UI.
		ImGui::ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), mCommandList.Get());

		// Indicate a state transition on the resource usage.
		transition = D3D12::CD3DX12_RESOURCE_BARRIER::Transition(CurrentBackBuffer(),
			D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
		mCommandList->ResourceBarrier(1, &transition);

		// Done recording commands.
		ThrowIfFailed(mCommandList->Close());

		mLinearAllocator->Commit(mCommandQueue.Get());

		// Add the command list to the queue for execution.
		auto cmdsLists = std::array{ static_cast<D3D12::ID3D12CommandList*>(mCommandList.Get()) };
		mCommandQueue->ExecuteCommandLists(static_cast<std::uint32_t>(cmdsLists.size()), cmdsLists.data());

		// Swap the back and front buffers
		auto presentParams = DXGI::DXGI_PRESENT_PARAMETERS{ 0 };
		ThrowIfFailed(mSwapChain->Present1(0, 0, &presentParams));
		mCurrBackBuffer = (mCurrBackBuffer + 1) % SwapChainBufferCount;

		// Advance the fence value to mark commands up to this fence point.
		mCurrFrameResource->Fence = ++mCurrentFence;

		// Add an instruction to the command queue to set a new fence point. 
		// Because we are on the GPU timeline, the new fence point won't be 
		// set until the GPU finishes processing all the commands prior to this Signal().
		mCommandQueue->Signal(mFence.Get(), mCurrentFence);
	}

	void UpdateImgui(const GameTimer& gt)override
	{
		D3DApp::UpdateImgui(gt);

		//
		// Define a panel to render GUI elements.
		// 
		ImGui::Begin("Options");

		ImGui::Text("Application average %.3f ms/frame (%.1f FPS)", 1000.0f / ImGui::GetIO().Framerate, ImGui::GetIO().Framerate);

		ImGui::Checkbox("Wireframe", &mDrawWireframe);

		ImGui::SliderFloat("BlurSigma", &mBlurSigma, 0.5f, 7.0f);
		ImGui::SliderInt("BlurCount", &mBlurCount, 1, 4);

		ImGui::Checkbox("FogEnabled", &mFogEnabled);

		ImGui::SliderFloat("FogStart", &mFogStart, 10.0f, 100);
		ImGui::SliderFloat("FogEnd", &mFogEnd, 20.0f, 200.0f);

		if (mFogStart >= mFogEnd)
			mFogStart = 10.0f;

		ImGui::SliderFloat("WaveScale", &mWaveScale, 0.25f, 4.0f);
		ImGui::SliderFloat("WaveSpeed", &mWaveSpeed, 2.0f, 16.0f);
		ImGui::SliderFloat("WaveDamping", &mWaveDamping, 0.0f, 3.0f);

		mWaves->SetConstants(mWaveSpeed, mWaveDamping);

		auto gfxMemStats = DirectX::GraphicsMemory::Get(md3dDevice.Get()).GetStatistics();

		if (ImGui::CollapsingHeader("VideoMemoryInfo"))
		{
			static auto vidMemPollTime = 0.0f;
			vidMemPollTime += gt.DeltaTime();

			static auto videoMemInfo = DXGI::DXGI_QUERY_VIDEO_MEMORY_INFO{};
			if (vidMemPollTime >= 1.0f) // poll every second
			{
				mDefaultAdapter->QueryVideoMemoryInfo(
					0, // assume single GPU
					DXGI::DXGI_MEMORY_SEGMENT_GROUP::DXGI_MEMORY_SEGMENT_GROUP_LOCAL, // interested in local GPU memory, not shared
					&videoMemInfo);

				vidMemPollTime -= 1.0f;
			}

			ImGui::Text("Budget (bytes): %u", videoMemInfo.Budget);
			ImGui::Text("CurrentUsage (bytes): %u", videoMemInfo.CurrentUsage);
			ImGui::Text("AvailableForReservation (bytes): %u", videoMemInfo.AvailableForReservation);
			ImGui::Text("CurrentReservation (bytes): %u", videoMemInfo.CurrentReservation);

		}
		if (ImGui::CollapsingHeader("GraphicsMemoryStatistics"))
		{
			ImGui::Text("Bytes of memory in-flight: %u", gfxMemStats.committedMemory);
			ImGui::Text("Total bytes used: %u", gfxMemStats.totalMemory);
			ImGui::Text("Total page count: %u", gfxMemStats.totalPages);
		}

		ImGui::End();

		ImGui::Render();
	}
	void OnMouseDown(Win32::WPARAM btnState, int x, int y)override
	{
		if (auto& io = ImGui::GetIO(); !io.WantCaptureMouse)
		{
			mLastMousePos.x = x;
			mLastMousePos.y = y;
			Win32::SetCapture(mhMainWnd);
		}
	}
	void OnMouseUp(Win32::WPARAM btnState, int x, int y)override
	{
		if (auto& io = ImGui::GetIO(); !io.WantCaptureMouse)
			Win32::ReleaseCapture();
	}
	void OnMouseMove(WPARAM btnState, int x, int y)override
	{
		auto& io = ImGui::GetIO();

		if (io.WantCaptureMouse)
			return;
		if ((btnState & Win32::MK::LButton) != 0)
		{
			// Make each pixel correspond to a quarter of a degree.
			float dx = DirectX::XMConvertToRadians(0.25f * static_cast<float>(x - mLastMousePos.x));
			float dy = DirectX::XMConvertToRadians(0.25f * static_cast<float>(y - mLastMousePos.y));

			// Update angles based on input to orbit camera around box.
			mTheta += dx;
			mPhi += dy;

			// Restrict the angle mPhi.
			mPhi = std::clamp(mPhi, 0.1f, MathHelper::Pi - 0.1f);
		}
		else if ((btnState & Win32::MK::RButton) != 0)
		{
			// Make each pixel correspond to 0.005 unit in the scene.
			float dx = 0.05f * static_cast<float>(x - mLastMousePos.x);
			float dy = 0.05f * static_cast<float>(y - mLastMousePos.y);

			// Update the camera radius based on input.
			mRadius += dx - dy;

			// Restrict the radius.
			mRadius = std::clamp(mRadius, 5.0f, 150.0f);
		}

		mLastMousePos.x = x;
		mLastMousePos.y = y;
	}

	void OnKeyboardInput(const GameTimer& gt)
	{}

	void AnimateMaterials(const GameTimer& gt)
	{
		auto& matLib = MaterialLib::GetLib();

		// Scroll the water material texture coordinates.
		auto waterMat = matLib["water"];

		float& tu = waterMat->MatTransform(3, 0);
		float& tv = waterMat->MatTransform(3, 1);

		tu += 0.1f * gt.DeltaTime();
		tv += 0.02f * gt.DeltaTime();

		if (tu >= 1.0f)
			tu -= 1.0f;

		if (tv >= 1.0f)
			tv -= 1.0f;

		waterMat->MatTransform(3, 0) = tu;
		waterMat->MatTransform(3, 1) = tv;

		// Material has changed, so need to update cbuffer.
		waterMat->NumFramesDirty = gNumFrameResources;
	}
	void UpdateCamera(const GameTimer& gt)
	{
		// Convert Spherical to Cartesian coordinates.
		mEyePos.x = mRadius * std::sinf(mPhi) * std::cosf(mTheta);
		mEyePos.z = mRadius * std::sinf(mPhi) * std::sinf(mTheta);
		mEyePos.y = mRadius * std::cosf(mPhi);

		// Build the view matrix.
		auto pos = DirectX::XMVECTOR{DirectX::XMVectorSet(mEyePos.x, mEyePos.y, mEyePos.z, 1.0f)};
		auto target = DirectX::XMVECTOR{DirectX::XMVectorZero()};
		auto up = DirectX::XMVECTOR{DirectX::XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f)};
		auto view = DirectX::XMMATRIX{DirectX::XMMatrixLookAtLH(pos, target, up)};
		DirectX::XMStoreFloat4x4(&mView, view);
	}
	void UpdatePerObjectCB(const GameTimer& gt)
	{
		for (auto& ri : mRitemLayer[(int)RenderLayer::GpuWaves])
		{
			// The current solution displacement map gets ping-ponged every frame, 
			// so we need to set it every frame.
			ri->MiscUint4.x = mWaves->DisplacementMapSrvIndex();
		}

		// Update per object constants once per frame so the data can be shared across different render passes.
		for (auto& ri : mAllRitems)
		{
			XMStoreFloat4x4(&ri->ObjectConstants.gWorld, XMMatrixTranspose(XMLoadFloat4x4(&ri->World)));
			XMStoreFloat4x4(&ri->ObjectConstants.gTexTransform, XMMatrixTranspose(XMLoadFloat4x4(&ri->TexTransform)));
			ri->ObjectConstants.gMaterialIndex = ri->Mat->MatIndex;

			ri->ObjectConstants.gMiscUint4 = ri->MiscUint4;
			ri->ObjectConstants.gMiscFloat4 = ri->MiscFloat4;

			// Need to hold handle until we submit work to GPU.
			ri->MemHandleToObjectCB = mLinearAllocator->AllocateConstant(ri->ObjectConstants);
		}
	}
	void UpdateMaterialBuffer(const GameTimer& gt)
	{
		auto& matLib = MaterialLib::GetLib();

		auto currMaterialBuffer = mCurrFrameResource->MaterialBuffer.get();
		for (auto& e : matLib.GetCollection())
		{
			// Only update the buffer data if the data has changed.  If the buffer
			// data changes, it needs to be updated for each FrameResource.
			auto mat = static_cast<Material*>(e.second.get());
			if (mat->NumFramesDirty < 1)
				continue;
			auto matTransform = DirectX::XMMATRIX{DirectX::XMLoadFloat4x4(&mat->MatTransform)};

			auto matData = MaterialData{
				.DiffuseAlbedo = mat->DiffuseAlbedo,
				.FresnelR0 = mat->FresnelR0,
				.Roughness = mat->Roughness,
				.DiffuseMapIndex = static_cast<std::uint32_t>(mat->AlbedoBindlessIndex),
				.NormalMapIndex = static_cast<std::uint32_t>(mat->NormalBindlessIndex),
				.GlossHeightAoMapIndex = static_cast<std::uint32_t>(mat->GlossHeightAoBindlessIndex)
			};
			matData.DiffuseAlbedo = mat->DiffuseAlbedo;
			matData.FresnelR0 = mat->FresnelR0;
			matData.Roughness = mat->Roughness;
			DirectX::XMStoreFloat4x4(&matData.MatTransform, DirectX::XMMatrixTranspose(matTransform));

			currMaterialBuffer->CopyData(mat->MatIndex, matData);

			// Next FrameResource need to be updated too.
			mat->NumFramesDirty--;
		}
	}
	void UpdateMainPassCB(const GameTimer& gt)
	{
		mMainPassCB = {};

		auto view = DirectX::XMMATRIX{DirectX::XMLoadFloat4x4(&mView)};
		auto proj = DirectX::XMMATRIX{DirectX::XMLoadFloat4x4(&mProj)};

		auto viewProj = DirectX::XMMATRIX{ DirectX::XMMatrixMultiply(view, proj) };
		auto detView = DirectX::XMVECTOR{ DirectX::XMMatrixDeterminant(view) };
		auto detProj = DirectX::XMVECTOR{ DirectX::XMMatrixDeterminant(proj) };
		auto detViewProj = DirectX::XMVECTOR{ DirectX::XMMatrixDeterminant(viewProj) };
		auto invView = DirectX::XMMATRIX{DirectX::XMMatrixInverse(&detView, view)};
		auto invProj = DirectX::XMMATRIX{DirectX::XMMatrixInverse(&detProj, proj)};
		auto invViewProj = DirectX::XMMATRIX{DirectX::XMMatrixInverse(&detViewProj, viewProj)};

		DirectX::XMStoreFloat4x4(&mMainPassCB.gView, DirectX::XMMatrixTranspose(view));
		DirectX::XMStoreFloat4x4(&mMainPassCB.gInvView, DirectX::XMMatrixTranspose(invView));
		DirectX::XMStoreFloat4x4(&mMainPassCB.gProj, DirectX::XMMatrixTranspose(proj));
		DirectX::XMStoreFloat4x4(&mMainPassCB.gInvProj, DirectX::XMMatrixTranspose(invProj));
		DirectX::XMStoreFloat4x4(&mMainPassCB.gViewProj, DirectX::XMMatrixTranspose(viewProj));
		DirectX::XMStoreFloat4x4(&mMainPassCB.gInvViewProj, DirectX::XMMatrixTranspose(invViewProj));
		mMainPassCB.gEyePosW = mEyePos;
		mMainPassCB.gRenderTargetSize = DirectX::XMFLOAT2((float)mClientWidth, (float)mClientHeight);
		mMainPassCB.gInvRenderTargetSize = DirectX::XMFLOAT2(1.0f / mClientWidth, 1.0f / mClientHeight);
		mMainPassCB.gNearZ = 1.0f;
		mMainPassCB.gFarZ = 1000.0f;
		mMainPassCB.gTotalTime = gt.TotalTime();
		mMainPassCB.gDeltaTime = gt.DeltaTime();
		mMainPassCB.gAmbientLight = { 0.25f, 0.25f, 0.35f, 1.0f };

		mMainPassCB.gFogColor = mFogColor;
		mMainPassCB.gFogStart = mFogStart;
		mMainPassCB.gFogRange = mFogEnd - mFogStart;
		mMainPassCB.gFogEnabled = mFogEnabled;

		mMainPassCB.gNumDirLights = 3;
		mMainPassCB.gNumPointLights = 0;
		mMainPassCB.gNumSpotLights = 0;
		mMainPassCB.gLights[0].Direction = mRotatedLightDirections[0];
		mMainPassCB.gLights[0].Strength = { 0.8f, 0.75f, 0.7f };
		mMainPassCB.gLights[1].Direction = mRotatedLightDirections[1];
		mMainPassCB.gLights[1].Strength = { 0.3f, 0.3f, 0.3f };
		mMainPassCB.gLights[2].Direction = mRotatedLightDirections[2];
		mMainPassCB.gLights[2].Strength = { 0.2f, 0.2f, 0.2f };

		auto currPassCB = mCurrFrameResource->PassCB.get();
		currPassCB->CopyData(0, mMainPassCB);
	}
	void UpdateWavesGPU(const GameTimer& gt, D3D12::ID3D12Resource* passCB)
	{
		// Every quarter second, generate a random wave.
		static auto t_base = 0.0f;
		if ((mTimer.TotalTime() - t_base) >= 0.25f)
		{
			t_base += 0.25f;
			auto i = MathHelper::Rand(4, mWaves->RowCount() - 5);
			auto j = MathHelper::Rand(4, mWaves->ColumnCount() - 5);
			auto r = MathHelper::RandF(1.0f, 2.0f);
			mWaves->Disturb(mCommandList.Get(), mComputeRootSignature.Get(), passCB, mPSOs["wavesDisturb"].Get(), i, j, r);
		}

		// Update the wave simulation.
		mWaves->Update(gt, mCommandList.Get(), mComputeRootSignature.Get(), passCB, mPSOs["wavesUpdate"].Get());
	}

	void LoadTextures()
	{
		auto& texLib = TextureLib::GetLib();
		texLib.Init(md3dDevice.Get(), *mUploadBatch.get());
	}

	void BuildCbvSrvUavDescriptorHeap()
	{
		auto& cbvSrvUavHeap = CbvSrvUavHeap::Get();
		cbvSrvUavHeap.Init(md3dDevice.Get(), CBV_SRV_UAV_HEAP_CAPACITY);

		InitImgui(cbvSrvUavHeap);

		auto& texLib = TextureLib::GetLib();
		for (auto& it : texLib.GetCollection())
		{
			auto tex = static_cast<Texture*>(it.second.get());
			tex->BindlessIndex = cbvSrvUavHeap.NextFreeIndex();

			auto hDescriptor = D3D12::CD3DX12_CPU_DESCRIPTOR_HANDLE{cbvSrvUavHeap.CpuHandle(tex->BindlessIndex)};
			auto texResource = static_cast<D3D12::ID3D12Resource*>(tex->Resource.Get());
			if (tex->IsCubeMap)
				CreateSrvCube(md3dDevice.Get(), texResource, texResource->GetDesc().Format, texResource->GetDesc().MipLevels, hDescriptor);
			else
				CreateSrv2d(md3dDevice.Get(), texResource, texResource->GetDesc().Format, texResource->GetDesc().MipLevels, hDescriptor);
		}

		mWaves->BuildDescriptors();
		mBlurFilter->BuildDescriptors();
	}
	void BuildRootSignature()
	{
		// Root parameter can be a table, root descriptor or root constants.
		auto gfxRootParameters = std::array<D3D12::CD3DX12_ROOT_PARAMETER, GFX_ROOT_ARG_COUNT>{};

		// Perfomance TIP: Order from most frequent to least frequent.
		gfxRootParameters[GFX_ROOT_ARG_OBJECT_CBV].InitAsConstantBufferView(0);
		gfxRootParameters[GFX_ROOT_ARG_PASS_CBV].InitAsConstantBufferView(1);
		gfxRootParameters[GFX_ROOT_ARG_MATERIAL_SRV].InitAsShaderResourceView(0);

		// A root signature is an array of root parameters.
		auto rootSigDesc = D3D12::CD3DX12_ROOT_SIGNATURE_DESC(
			GFX_ROOT_ARG::GFX_ROOT_ARG_COUNT,
			gfxRootParameters.data(),
			0, 
			nullptr,
			D3D12::D3D12_ROOT_SIGNATURE_FLAGS{ 
				D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT |
				D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED |
				D3D12_ROOT_SIGNATURE_FLAG_SAMPLER_HEAP_DIRECTLY_INDEXED 
			});

		// create a root signature with a single slot which points to a descriptor range consisting of a single constant buffer
		auto serializedRootSig = Microsoft::WRL::ComPtr<D3D::ID3DBlob>{};
		auto errorBlob = Microsoft::WRL::ComPtr<D3D::ID3DBlob>{};
		auto hr = D3D12::D3D12SerializeRootSignature(
			&rootSigDesc,
			D3D::D3D_ROOT_SIGNATURE_VERSION::D3D_ROOT_SIGNATURE_VERSION_1,
			serializedRootSig.GetAddressOf(),
			errorBlob.GetAddressOf());

		if (errorBlob != nullptr)
			Win32::OutputDebugStringA((char*)errorBlob->GetBufferPointer());
		ThrowIfFailed(hr);

		ThrowIfFailed(md3dDevice->CreateRootSignature(
			0,
			serializedRootSig->GetBufferPointer(),
			serializedRootSig->GetBufferSize(),
			__uuidof(D3D12::ID3D12RootSignature), &mRootSignature));

		// Root parameter can be a table, root descriptor or root constants.
		CD3DX12_ROOT_PARAMETER computeRootParameters[COMPUTE_ROOT_ARG::COMPUTE_ROOT_ARG_COUNT];

		// Perfomance TIP: Order from most frequent to least frequent.
		computeRootParameters[COMPUTE_ROOT_ARG_DISPATCH_CBV].InitAsConstantBufferView(0);
		computeRootParameters[COMPUTE_ROOT_ARG_PASS_CBV].InitAsConstantBufferView(1);
		computeRootParameters[COMPUTE_ROOT_ARG_PASS_EXTRA_CBV].InitAsConstantBufferView(2);

		// A root signature is an array of root parameters.
		auto computeRootSigDesc = D3D12::CD3DX12_ROOT_SIGNATURE_DESC(
			COMPUTE_ROOT_ARG::COMPUTE_ROOT_ARG_COUNT,
			computeRootParameters,
			0, 
			nullptr, // static samplers
			D3D12::D3D12_ROOT_SIGNATURE_FLAGS{ 
				D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED | D3D12_ROOT_SIGNATURE_FLAG_SAMPLER_HEAP_DIRECTLY_INDEXED 
			});

		hr = D3D12::D3D12SerializeRootSignature(&computeRootSigDesc, D3D::D3D_ROOT_SIGNATURE_VERSION::D3D_ROOT_SIGNATURE_VERSION_1,
			serializedRootSig.GetAddressOf(), errorBlob.GetAddressOf());

		if (errorBlob != nullptr)
			Win32::OutputDebugStringA((char*)errorBlob->GetBufferPointer());
		ThrowIfFailed(hr);

		ThrowIfFailed(md3dDevice->CreateRootSignature(0,
			serializedRootSig->GetBufferPointer(),
			serializedRootSig->GetBufferSize(),
			__uuidof(D3D12::ID3D12RootSignature), 
			(void**)mComputeRootSignature.GetAddressOf()));
	}
	void BuildShadersAndInputLayout()
	{
#if defined(DEBUG) || defined(_DEBUG)  
#define COMMA_DEBUG_ARGS ,DXC::ArgDebug, DXC::ArgSkipOptimizations
#else
#define COMMA_DEBUG_ARGS
#endif

		auto vsArgs = std::vector{ L"-E", L"VS", L"-T", L"vs_6_6" COMMA_DEBUG_ARGS };
		auto vsWavesArgs = std::vector{ L"-E", L"VS", L"-T", L"vs_6_6", L"-D WAVES_VS=1" COMMA_DEBUG_ARGS };

		auto psArgs = std::vector{ L"-E", L"PS", L"-T", L"ps_6_6" COMMA_DEBUG_ARGS };

		auto psAlphaTestedArgs = std::vector{ L"-E", L"PS", L"-T", L"ps_6_6", L"-D ALPHA_TEST=1" COMMA_DEBUG_ARGS };

		auto csUpdateWavesArgs = std::vector{ L"-E", L"UpdateWavesCS", L"-T", L"cs_6_6" COMMA_DEBUG_ARGS };
		auto csDisturbWavesArgs = std::vector{ L"-E", L"DisturbWavesCS", L"-T", L"cs_6_6" COMMA_DEBUG_ARGS };

		auto csHorzBlurArgs = std::vector{ L"-E", L"HorzBlurCS", L"-T", L"cs_6_6" COMMA_DEBUG_ARGS };
		auto csVertBlurArgs = std::vector{ L"-E", L"VertBlurCS", L"-T", L"cs_6_6" COMMA_DEBUG_ARGS };

		mShaders["standardVS"] = d3dUtil::CompileShader(L"Shaders\\BasicBlend.hlsl", vsArgs);
		mShaders["opaquePS"] = d3dUtil::CompileShader(L"Shaders\\BasicBlend.hlsl", psArgs);
		mShaders["alphaTestedPS"] = d3dUtil::CompileShader(L"Shaders\\BasicBlend.hlsl", psAlphaTestedArgs);

		mShaders["wavesVS"] = d3dUtil::CompileShader(L"Shaders\\BasicBlend.hlsl", vsWavesArgs);
		mShaders["wavesUpdateCS"] = d3dUtil::CompileShader(L"Shaders\\WaveSim.hlsl", csUpdateWavesArgs);
		mShaders["wavesDisturbCS"] = d3dUtil::CompileShader(L"Shaders\\WaveSim.hlsl", csDisturbWavesArgs);

		mShaders["horzBlurCS"] = d3dUtil::CompileShader(L"Shaders\\Blur.hlsl", csHorzBlurArgs);
		mShaders["vertBlurCS"] = d3dUtil::CompileShader(L"Shaders\\Blur.hlsl", csVertBlurArgs);

		mInputLayout = {
			{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12::D3D12_INPUT_CLASSIFICATION::D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
			{ "NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12::D3D12_INPUT_CLASSIFICATION::D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
			{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 24, D3D12::D3D12_INPUT_CLASSIFICATION::D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
			{ "TANGENT", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 32, D3D12::D3D12_INPUT_CLASSIFICATION::D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		};
	}

	void BuildPSOs()
	{
		auto basePsoDesc = d3dUtil::InitDefaultPso(
			mBackBufferFormat,
			mDepthStencilFormat,
			mInputLayout,
			mRootSignature.Get(),
			mShaders["standardVS"].Get(),
			mShaders["opaquePS"].Get());

		ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(
			&basePsoDesc,
			__uuidof(D3D12::ID3D12PipelineState), 
			&mPSOs["opaque"]));

		auto wireframePsoDesc = D3D12::D3D12_GRAPHICS_PIPELINE_STATE_DESC{ basePsoDesc };
		wireframePsoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_WIREFRAME;

		ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(
			&wireframePsoDesc,
			__uuidof(D3D12::ID3D12PipelineState),
			&mPSOs["opaque_wireframe"]));

		//
		// PSO for transparent objects
		//

		auto transparentPsoDesc = D3D12::D3D12_GRAPHICS_PIPELINE_STATE_DESC{ basePsoDesc };

		auto transparencyBlendDesc = D3D12::D3D12_RENDER_TARGET_BLEND_DESC{
			.BlendEnable = true,
			.LogicOpEnable = false,
			.SrcBlend = D3D12_BLEND_SRC_ALPHA,
			.DestBlend = D3D12_BLEND_INV_SRC_ALPHA,
			.BlendOp = D3D12_BLEND_OP_ADD,
			.SrcBlendAlpha = D3D12_BLEND_ONE,
			.DestBlendAlpha = D3D12_BLEND_ZERO,
			.BlendOpAlpha = D3D12_BLEND_OP_ADD,
			.LogicOp = D3D12_LOGIC_OP_NOOP,
			.RenderTargetWriteMask = D3D12::D3D12_COLOR_WRITE_ENABLE::D3D12_COLOR_WRITE_ENABLE_ALL
		};
		

		transparentPsoDesc.BlendState.RenderTarget[0] = transparencyBlendDesc;
		ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(&transparentPsoDesc,
			__uuidof(D3D12::ID3D12PipelineState),
			&mPSOs["transparent"]));

		//
		// PSO for alpha tested objects
		//

		auto alphaTestedPsoDesc = D3D12::D3D12_GRAPHICS_PIPELINE_STATE_DESC{ basePsoDesc };
		alphaTestedPsoDesc.PS = d3dUtil::ByteCodeFromBlob(mShaders["alphaTestedPS"].Get());
		alphaTestedPsoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
		ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(&alphaTestedPsoDesc,
			__uuidof(D3D12::ID3D12PipelineState),
			&mPSOs["alphaTested"]));

		//
		// PSO for drawing waves
		//
		auto wavesRenderPSO = D3D12::D3D12_GRAPHICS_PIPELINE_STATE_DESC{ transparentPsoDesc };
		wavesRenderPSO.VS = d3dUtil::ByteCodeFromBlob(mShaders["wavesVS"].Get());
		ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(
			&wavesRenderPSO, 
			__uuidof(D3D12::ID3D12PipelineState),
			&mPSOs["waves_transparent"]));

		auto wavesRenderWireframePSO = D3D12::D3D12_GRAPHICS_PIPELINE_STATE_DESC{ basePsoDesc };
		wavesRenderWireframePSO.VS = d3dUtil::ByteCodeFromBlob(mShaders["wavesVS"].Get());
		wavesRenderWireframePSO.RasterizerState.FillMode = D3D12_FILL_MODE_WIREFRAME;
		ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(&wavesRenderWireframePSO, 
			__uuidof(D3D12::ID3D12PipelineState),
			&mPSOs["waves_wireframe"]));

		//
		// PSO for disturbing waves
		//
		auto wavesDisturbPSO = D3D12::D3D12_COMPUTE_PIPELINE_STATE_DESC{};
		wavesDisturbPSO.pRootSignature = mComputeRootSignature.Get();
		wavesDisturbPSO.CS = d3dUtil::ByteCodeFromBlob(mShaders["wavesDisturbCS"].Get());
		wavesDisturbPSO.Flags = D3D12_PIPELINE_STATE_FLAG_NONE;
		ThrowIfFailed(md3dDevice->CreateComputePipelineState(&wavesDisturbPSO, 
			__uuidof(D3D12::ID3D12PipelineState),
			&mPSOs["wavesDisturb"]));

		//
		// PSO for updating waves
		//
		auto wavesUpdatePSO = D3D12::D3D12_COMPUTE_PIPELINE_STATE_DESC{};
		wavesUpdatePSO.pRootSignature = mComputeRootSignature.Get();
		wavesUpdatePSO.CS = d3dUtil::ByteCodeFromBlob(mShaders["wavesUpdateCS"].Get());
		wavesUpdatePSO.Flags = D3D12_PIPELINE_STATE_FLAG_NONE;
		ThrowIfFailed(md3dDevice->CreateComputePipelineState(&wavesUpdatePSO, 
			__uuidof(D3D12::ID3D12PipelineState),
			&mPSOs["wavesUpdate"]));

		//
		// PSO for horizontal blur
		//
		auto horzBlurPSO = D3D12::D3D12_COMPUTE_PIPELINE_STATE_DESC{};
		horzBlurPSO.pRootSignature = mComputeRootSignature.Get();
		horzBlurPSO.CS = d3dUtil::ByteCodeFromBlob(mShaders["horzBlurCS"].Get());
		horzBlurPSO.Flags = D3D12_PIPELINE_STATE_FLAG_NONE;
		ThrowIfFailed(md3dDevice->CreateComputePipelineState(&horzBlurPSO, 
			__uuidof(D3D12::ID3D12PipelineState),
			&mPSOs["horzBlur"]));

		//
		// PSO for vertical blur
		//
		auto vertBlurPSO = D3D12::D3D12_COMPUTE_PIPELINE_STATE_DESC{};
		vertBlurPSO.pRootSignature = mComputeRootSignature.Get();
		vertBlurPSO.CS = d3dUtil::ByteCodeFromBlob(mShaders["vertBlurCS"].Get());
		vertBlurPSO.Flags = D3D12_PIPELINE_STATE_FLAG_NONE;
		ThrowIfFailed(md3dDevice->CreateComputePipelineState(&vertBlurPSO, 
			__uuidof(D3D12::ID3D12PipelineState),
			&mPSOs["vertBlur"]));
	}
	void BuildFrameResources()
	{
		auto& matLib = MaterialLib::GetLib();

		constexpr auto passCount = 1u;
		for (int i = 0; i < gNumFrameResources; ++i)
			mFrameResources.push_back(std::make_unique<FrameResource>(md3dDevice.Get(), passCount, matLib.GetMaterialCount(), mWaves->VertexCount()));
	}
	void BuildMaterials()
	{
		MaterialLib::GetLib().Init(md3dDevice.Get());
	}

	void AddRenderItem(
		RenderLayer layer, const DirectX::XMFLOAT4X4& world,
		const DirectX::XMFLOAT4X4& texTransform, Material* mat,
		MeshGeometry* geo, SubmeshGeometry& drawArgs)
	{
		auto ritem = std::make_unique<RenderItem>();
		ritem->World = world;
		ritem->TexTransform = texTransform;
		ritem->Mat = mat;
		ritem->Geo = geo;
		ritem->PrimitiveType = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
		ritem->IndexCount = drawArgs.IndexCount;
		ritem->StartIndexLocation = drawArgs.StartIndexLocation;
		ritem->BaseVertexLocation = drawArgs.BaseVertexLocation;

		mRitemLayer[(int)layer].push_back(ritem.get());
		mAllRitems.push_back(std::move(ritem));
	}

	void AddWaveRenderItem(
		RenderLayer layer,
		const DirectX::XMFLOAT4X4& world,
		const DirectX::XMFLOAT4X4& texTransform,
		uint32_t wavesGridWidth,
		uint32_t wavesGridDepth,
		float wavesGridSpatialStep,
		Material* mat,
		MeshGeometry* geo,
		SubmeshGeometry& drawArgs)
	{
		AddRenderItem(layer,
			world,
			texTransform,
			mat,
			geo,
			drawArgs);

		mAllRitems.back()->MiscUint4.y = wavesGridWidth;
		mAllRitems.back()->MiscUint4.z = wavesGridDepth;
		mAllRitems.back()->MiscFloat4.x = wavesGridSpatialStep;
	}

	void BuildRenderItems()
	{
		auto& matLib = MaterialLib::GetLib();

		auto texTransform = DirectX::XMFLOAT4X4{MathHelper::Identity4x4};
		auto worldTransform = DirectX::XMFLOAT4X4{ MathHelper::Identity4x4 };
		XMStoreFloat4x4(&texTransform, DirectX::XMMatrixScaling(5.0f, 5.0f, 1.0f));
		AddWaveRenderItem(
			RenderLayer::GpuWaves,
			worldTransform,
			texTransform,
			mWaves->ColumnCount(),
			mWaves->RowCount(),
			mWaves->SpatialStep(),
			matLib["water"],
			mGeometries["waterGeo"].get(),
			mGeometries["waterGeo"]->DrawArgs["grid"]);

		worldTransform = MathHelper::Identity4x4;
		XMStoreFloat4x4(&texTransform, DirectX::XMMatrixScaling(8.0f, 8.0f, 1.0f));
		AddRenderItem(RenderLayer::Opaque,
			worldTransform,
			texTransform,
			matLib["grass"],
			mGeometries["landGeo"].get(),
			mGeometries["landGeo"]->DrawArgs["grid"]);

		auto S = DirectX::XMMatrixScaling(8.0f, 8.0f, 8.0f);
		auto T = DirectX::XMMatrixTranslation(3.0f, 2.0f, -9.0f);
		XMStoreFloat4x4(&worldTransform, S * T);
		texTransform = MathHelper::Identity4x4;
		AddRenderItem(RenderLayer::AlphaTested, worldTransform, texTransform, matLib["fence"], mGeometries["shapeGeo"].get(), mGeometries["shapeGeo"]->DrawArgs["box"]);
	}

	void DrawRenderItems(
		D3D12::ID3D12GraphicsCommandList* cmdList, 
		const std::vector<RenderItem*>& ritems
	)
	{
		auto& cbvSrvUavHeap = CbvSrvUavHeap::Get();

		for (auto i = 0ull; i < ritems.size(); ++i)
		{
			auto ri = ritems[i];

			auto vbv = ri->Geo->VertexBufferView();
			auto ibv = ri->Geo->IndexBufferView();
			cmdList->IASetVertexBuffers(0, 1, &vbv);
			cmdList->IASetIndexBuffer(&ibv);
			cmdList->IASetPrimitiveTopology(ri->PrimitiveType);

			cmdList->SetGraphicsRootConstantBufferView(GFX_ROOT_ARG_OBJECT_CBV, ri->MemHandleToObjectCB.GpuAddress());

			cmdList->DrawIndexedInstanced(ri->IndexCount, 1, ri->StartIndexLocation, ri->BaseVertexLocation, 0);
		}
	}

	auto BuildLandGeometry(
		D3D12::ID3D12Device* device, 
		DirectX::ResourceUploadBatch& uploadBatch
	) -> std::unique_ptr<MeshGeometry>
	{
		auto meshGen = MeshGen{};
		auto grid = MeshGenData{ meshGen.CreateGrid(160.0f, 160.0f, 50, 50) };

		//
		// Extract the vertex elements we are interested and apply the height function to
		// each vertex.  In addition, color the vertices based on their height so we have
		// sandy looking beaches, grassy low hills, and snow mountain peaks.
		//

		auto vertices = std::vector<ModelVertex>(grid.Vertices.size());
		for (auto i = 0ull; i < grid.Vertices.size(); ++i)
		{
			auto& p = grid.Vertices[i].Position;
			vertices[i].Pos = p;
			vertices[i].Pos.y = GetHillsHeight(p.x, p.z);

			vertices[i].Normal = GetHillsNormal(p.x, p.z);

			vertices[i].TexC = grid.Vertices[i].TexC;

			// Not used in this demo.
			vertices[i].TangentU = DirectX::XMFLOAT3{0.0f, 0.0f, 0.0f};
		}

		const auto indexCount = static_cast<std::uint32_t>(grid.Indices32.size());
		const auto indexElementByteSize = static_cast<std::uint32_t>(sizeof(std::uint16_t));
		const auto vbByteSize = static_cast<std::uint32_t>(vertices.size() * sizeof(ModelVertex));
		const auto ibByteSize = indexCount * indexElementByteSize;

		auto indices = std::vector<std::uint16_t>{grid.GetIndices16()};

		auto geo = std::make_unique<MeshGeometry>();
		geo->Name = "landGeo";

		geo->VertexBufferCPU.resize(vbByteSize);
		std::memcpy(geo->VertexBufferCPU.data(), vertices.data(), vbByteSize);

		geo->IndexBufferCPU.resize(ibByteSize);
		std::memcpy(geo->IndexBufferCPU.data(), indices.data(), ibByteSize);

		CreateStaticBuffer(
			device, uploadBatch,
			vertices.data(), vertices.size(), sizeof(ModelVertex),
			D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER,
			&geo->VertexBufferGPU);

		CreateStaticBuffer(
			device, uploadBatch,
			indices.data(), indexCount, indexElementByteSize,
			D3D12_RESOURCE_STATE_INDEX_BUFFER,
			&geo->IndexBufferGPU);

		geo->VertexByteStride = sizeof(ModelVertex);
		geo->VertexBufferByteSize = vbByteSize;
		geo->IndexFormat = DXGI_FORMAT_R16_UINT;
		geo->IndexBufferByteSize = ibByteSize;

		auto submesh = SubmeshGeometry{
			.IndexCount = static_cast<std::uint32_t>(indices.size()),
			.StartIndexLocation = 0,
			.BaseVertexLocation = 0,
			.VertexCount = static_cast<UINT>(vertices.size())
		};
		
		geo->DrawArgs["grid"] = submesh;

		return geo;
	}

	auto BuildWaveGeometry(
		D3D12::ID3D12Device* device, 
		DirectX::ResourceUploadBatch& uploadBatch
	) -> std::unique_ptr<MeshGeometry>
	{
		auto m = mWaves->RowCount();
		auto n = mWaves->ColumnCount();

		const auto waterWorldSize = 128.0f;

		auto meshGen = MeshGen{};
		auto grid = MeshGenData{meshGen.CreateGrid(waterWorldSize, waterWorldSize, m, n)};

		// Extract the vertex elements we are interested into our vertex buffer. 
		auto vertices = std::vector<ModelVertex>(grid.Vertices.size());
		for (auto i = 0ull; i < grid.Vertices.size(); ++i)
		{
			vertices[i].Pos = grid.Vertices[i].Position;
			vertices[i].Normal = grid.Vertices[i].Normal;
			vertices[i].TexC = grid.Vertices[i].TexC;
			vertices[i].TangentU = grid.Vertices[i].TangentU;
		}

		const auto indexCount = static_cast<std::uint32_t>(grid.Indices32.size());
		const auto indexElementByteSize = static_cast<std::uint32_t>(sizeof(std::uint32_t));
		const auto vbByteSize = static_cast<std::uint32_t>(vertices.size() * sizeof(ModelVertex));
		const auto ibByteSize = indexCount * indexElementByteSize;
		const auto indexData = reinterpret_cast<Win32::byte*>(grid.Indices32.data());

		auto geo = std::make_unique<MeshGeometry>();
		geo->Name = "waterGeo";

		geo->VertexBufferCPU.resize(vbByteSize);
		std::memcpy(geo->VertexBufferCPU.data(), vertices.data(), vbByteSize);

		geo->IndexBufferCPU.resize(ibByteSize);
		std::memcpy(geo->IndexBufferCPU.data(), indexData, ibByteSize);

		CreateStaticBuffer(device, uploadBatch,
			vertices.data(), vertices.size(), sizeof(ModelVertex),
			D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER, &geo->VertexBufferGPU);

		CreateStaticBuffer(device, uploadBatch,
			indexData, indexCount, indexElementByteSize,
			D3D12_RESOURCE_STATE_INDEX_BUFFER, &geo->IndexBufferGPU);

		geo->VertexByteStride = sizeof(ModelVertex);
		geo->VertexBufferByteSize = vbByteSize;
		geo->IndexFormat = DXGI_FORMAT_R32_UINT;
		geo->IndexBufferByteSize = ibByteSize;

		auto submesh = SubmeshGeometry{
			.IndexCount = indexCount,
			.StartIndexLocation = 0,
			.BaseVertexLocation = 0,
			.VertexCount = static_cast<UINT>(vertices.size()),
			.Bounds{
				DirectX::XMFLOAT3(0.0f, 0.0f, 0.0f),
				DirectX::XMFLOAT3(waterWorldSize, waterWorldSize, 2.0f)
			}};
		

		geo->DrawArgs["grid"] = submesh;

		return geo;
	}

	auto GetHillsHeight(float x, float z)const -> float
	{
		return 0.3f * (z * std::sinf(0.1f * x) + x * std::cosf(0.1f * z));
	}

	auto GetHillsNormal(float x, float z)const -> DirectX::XMFLOAT3
	{
		// n = (-df/dx, 1, -df/dz)
		auto n = DirectX::XMFLOAT3{
			-0.03f * z * std::cosf(0.1f * x) - 0.3f * std::cosf(0.1f * z),
			1.0f,
			-0.3f * std::sinf(0.1f * x) + 0.03f * x * std::sinf(0.1f * z) };
		auto unitNormal = DirectX::XMVECTOR{DirectX::XMVector3Normalize(DirectX::XMLoadFloat3(&n))};
		DirectX::XMStoreFloat3(&n, unitNormal);
		return n;
	}

private:
	std::vector<std::unique_ptr<FrameResource>> mFrameResources;
	FrameResource* mCurrFrameResource = nullptr;
	int mCurrFrameResourceIndex = 0;

	Microsoft::WRL::ComPtr<D3D12::ID3D12RootSignature> mRootSignature;
	Microsoft::WRL::ComPtr<D3D12::ID3D12RootSignature> mComputeRootSignature;

	std::unordered_map<std::string, std::unique_ptr<MeshGeometry>> mGeometries;
	std::unordered_map<std::string, Microsoft::WRL::ComPtr<DXC::IDxcBlob>> mShaders;
	std::unordered_map<std::string, Microsoft::WRL::ComPtr<D3D12::ID3D12PipelineState>> mPSOs;

	std::vector<D3D12::D3D12_INPUT_ELEMENT_DESC> mInputLayout;

	// List of all the render items.
	std::vector<std::unique_ptr<RenderItem>> mAllRitems;

	// Render items divided by PSO.
	std::vector<RenderItem*> mRitemLayer[(int)RenderLayer::Count];

	std::unique_ptr<GpuWaves> mWaves;

	std::unique_ptr<BlurFilter> mBlurFilter;

	PerPassCB mMainPassCB;

	DirectX::XMFLOAT4X4 mView = MathHelper::Identity4x4;
	DirectX::XMFLOAT4X4 mProj = MathHelper::Identity4x4;

	DirectX::XMFLOAT3 mEyePos = { 0.0f, 0.0f, 0.0f };
	float mTheta = 1.5f * DirectX::Pi;
	float mPhi = DirectX::PiOverTwo - 0.1f;
	float mRadius = 50.0f;

	float mLightRotationAngle = 0.0f;
	DirectX::XMFLOAT3 mBaseLightDirections[3] = {
		DirectX::XMFLOAT3(0.57735f, -0.57735f, 0.57735f),
		DirectX::XMFLOAT3(-0.57735f, -0.57735f, 0.57735f),
		DirectX::XMFLOAT3(0.0f, -0.707f, -0.707f)
	};
	DirectX::XMFLOAT3 mRotatedLightDirections[3];

	Win32::POINT mLastMousePos;

	DirectX::XMFLOAT4 mFogColor = { 0.6f, 0.6f, 0.6f, 1.0f };
	bool mDrawWireframe = false;
	float mWaveScale = 1.0f;
	float mWaveSpeed = 3.5f;
	float mWaveDamping = 0.3f;

	bool mFogEnabled = true;
	float mFogStart = 20.0f;
	float mFogEnd = 160.0f;

	int mBlurCount = 1;
	float mBlurSigma = 4.0f;
};