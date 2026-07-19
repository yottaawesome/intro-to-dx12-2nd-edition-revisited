export module wavescs:gpuwaves;
import std;
import shared;
import :computerootarg;

class GpuWaves
{
public:
	// Note that m,n should be divisible by 16 so there is no 
	// remainder when we divide into thread groups.
	GpuWaves(D3D12::ID3D12Device* device, DirectX::ResourceUploadBatch& uploadBatch, int m, int n, float dx, float dt, float speed, float damping)
	{
		md3dDevice = device;

		mNumRows = m;
		mNumCols = n;

		//assert((m * n) % 256 == 0);

		mVertexCount = m * n;
		mTriangleCount = (m - 1) * (n - 1) * 2;

		mTimeStep = dt;
		mSpatialStep = dx;

		float d = damping * dt + 2.0f;
		float e = (speed * speed) * (dt * dt) / (dx * dx);
		mK[0] = (damping * dt - 2.0f) / d;
		mK[1] = (4.0f - 8.0f * e) / d;
		mK[2] = (2.0f * e) / d;

		BuildResources(uploadBatch);
	}
	GpuWaves(const GpuWaves& rhs) = delete;
	GpuWaves& operator=(const GpuWaves& rhs) = delete;

	auto RowCount()const-> std::uint32_t
	{
		return mNumRows;
	}
	auto ColumnCount()const-> std::uint32_t
	{
		return mNumCols;
	}
	auto VertexCount()const-> std::uint32_t
	{
		return mVertexCount;
	}
	auto TriangleCount()const-> std::uint32_t
	{
		return mTriangleCount;
	}
	auto Width()const-> float
	{
		return mNumCols * mSpatialStep;
	}
	auto Depth()const-> float
	{
		return mNumRows * mSpatialStep;
	}
	auto SpatialStep()const-> float
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

		std::vector<float> zeroFloats(mVertexCount, 0.0f);

		auto initData = D3D12::D3D12_SUBRESOURCE_DATA{
			.pData = zeroFloats.data(),
			.RowPitch = mNumCols * sizeof(float),
			.SlicePitch = mNumCols * sizeof(float) * mNumRows
		};
		

		DirectX::CreateTextureFromMemory(
			md3dDevice, uploadBatch,
			mNumCols, mNumRows,
			DXGI_FORMAT_R32_FLOAT,
			initData,
			mPrevSol.GetAddressOf(),
			false, // generateMips,
			D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
			D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);

		DirectX::CreateTextureFromMemory(
			md3dDevice, uploadBatch,
			mNumCols, mNumRows,
			DXGI_FORMAT_R32_FLOAT,
			initData,
			mCurrSol.GetAddressOf(),
			false, // generateMips,
			D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
			D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);

		DirectX::CreateTextureFromMemory(
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
		D3D12::ID3D12PipelineState* pso)
	{
		static auto t = 0.0f;

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