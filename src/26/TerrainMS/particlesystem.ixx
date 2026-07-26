//***************************************************************************************
// ParticleSystem.cpp by Frank Luna (C) 2011 All Rights Reserved.
//***************************************************************************************

export module terrainmsdemo:particlesystem;
import shared;
import std;
import :frameresource;

class ParticleSystem
{
public:
	ParticleSystem(
		D3D12::ID3D12Device* device,
		DirectX::ResourceUploadBatch& uploadBatch,
		std::uint32_t maxParticleCount,
		bool requiresSorting
	) : md3dDevice(device),
		mMaxParticleCount(maxParticleCount),
		mRequiresSorting(requiresSorting)
	{
		auto indices = std::vector<uint32_t>(mMaxParticleCount * 6);
		for (auto i = 0u; i < mMaxParticleCount; ++i)
		{
			indices[i * 6 + 0] = i * 4 + 0;
			indices[i * 6 + 1] = i * 4 + 1;
			indices[i * 6 + 2] = i * 4 + 2;

			indices[i * 6 + 3] = i * 4 + 2;
			indices[i * 6 + 4] = i * 4 + 1;
			indices[i * 6 + 5] = i * 4 + 3;
		}

		CreateStaticBuffer(md3dDevice, uploadBatch,
			indices.data(), indices.size(), sizeof(uint32_t),
			D3D12::D3D12_RESOURCE_STATES::D3D12_RESOURCE_STATE_INDEX_BUFFER, &mGeoIndexBuffer);

		BuildParticleBuffers(uploadBatch);

		mEmitInstances.reserve(16);
		mMemHandlesToEmitCB.reserve(16);
	}

	ParticleSystem(const ParticleSystem&) = delete;
	auto operator=(const ParticleSystem&) -> ParticleSystem& = delete;

	auto GetParticleBufferUavIndex()const -> std::uint32_t
	{
		return mParticleBufferUavIndex;
	}

	auto GetCurrAliveIndexBufferUavIndex()const -> std::uint32_t
	{
		return mCurrAliveIndexBufferUavIndex;
	}

	void BuildDescriptors()
	{
		auto& heap = CbvSrvUavHeap::Get();

		mParticleBufferUavIndex = heap.NextFreeIndex();
		mFreeIndexBufferUavIndex = heap.NextFreeIndex();
		mPrevAliveIndexBufferUavIndex = heap.NextFreeIndex();
		mCurrAliveIndexBufferUavIndex = heap.NextFreeIndex();
		mFreeCountUavIndex = heap.NextFreeIndex();
		mPrevAliveCountUavIndex = heap.NextFreeIndex();
		mCurrAliveCountUavIndex = heap.NextFreeIndex();
		mIndirectArgsUavIndex = heap.NextFreeIndex();

		CreateBufferUav(md3dDevice, 0, mMaxParticleCount, sizeof(Particle), 0, mParticleBuffer.Get(), nullptr, heap.CpuHandle(mParticleBufferUavIndex));
		CreateBufferUav(md3dDevice, 0, mMaxParticleCount, sizeof(uint32_t), 0, mFreeIndexBuffer.Get(), mFreeCountBuffer.Get(), heap.CpuHandle(mFreeIndexBufferUavIndex));
		CreateBufferUav(md3dDevice, 0, mMaxParticleCount, sizeof(uint32_t), 0, mPrevAliveIndexBuffer.Get(), mPrevAliveCountBuffer.Get(), heap.CpuHandle(mPrevAliveIndexBufferUavIndex));
		CreateBufferUav(md3dDevice, 0, mMaxParticleCount, sizeof(uint32_t), 0, mCurrAliveIndexBuffer.Get(), mCurrAliveCountBuffer.Get(), heap.CpuHandle(mCurrAliveIndexBufferUavIndex));

		CreateBufferUav(md3dDevice, 0, 1, sizeof(uint32_t), 0, mFreeCountBuffer.Get(), nullptr, heap.CpuHandle(mFreeCountUavIndex));
		CreateBufferUav(md3dDevice, 0, 1, sizeof(uint32_t), 0, mPrevAliveCountBuffer.Get(), nullptr, heap.CpuHandle(mPrevAliveCountUavIndex));
		CreateBufferUav(md3dDevice, 0, 1, sizeof(uint32_t), 0, mCurrAliveCountBuffer.Get(), nullptr, heap.CpuHandle(mCurrAliveCountUavIndex));

		CreateBufferUav(md3dDevice, 0, 8, sizeof(uint32_t), 0, mIndirectArgsBuffer.Get(), nullptr, heap.CpuHandle(mIndirectArgsUavIndex));
	}

	// Can call multiple times per frame to emit particles at different positions and with different properties.
	void Emit(const ParticleEmitCB& emitConstants)
	{
		mEmitInstances.push_back(emitConstants);
	}

	void FrameSetup(const GameTimer& gt)
	{
		mEmitInstances.clear();
		mMemHandlesToEmitCB.clear();
	}

	void Update(
		const GameTimer& gt,
		const DirectX::XMFLOAT3& acceleration,
		D3D12::ID3D12GraphicsCommandList* cmdList,
		D3D12::ID3D12CommandSignature* updateParticlesCommandSig,
		D3D12::ID3D12PipelineState* updateParticlesPso,
		D3D12::ID3D12PipelineState* emitParticlesPso,
		D3D12::ID3D12PipelineState* postUpdateParticlesPso,
		D3D12::ID3D12Resource* particleCountReadback
	)
	{
		auto& linearAllocator = DirectX::GraphicsMemory::Get(md3dDevice);

		auto updateConstants = ParticleUpdateCB{
			.gAcceleration = acceleration,
			.gParticleBufferUavIndex = mParticleBufferUavIndex,
			.gFreeIndexBufferUavIndex = mFreeIndexBufferUavIndex,
			.gPrevAliveIndexBufferUavIndex = mPrevAliveIndexBufferUavIndex,
			.gCurrAliveIndexBufferUavIndex = mCurrAliveIndexBufferUavIndex,
			.gFreeCountUavIndex = mFreeCountUavIndex,
			.gPrevAliveCountUavIndex = mPrevAliveCountUavIndex,
			.gCurrAliveCountUavIndex = mCurrAliveCountUavIndex,
			.gIndirectArgsUavIndex = mIndirectArgsUavIndex,
		};
		

		// Put bindless indices in our "extra" CB slot.
		mMemHandleUpdateCB = linearAllocator.AllocateConstant(updateConstants);
		cmdList->SetComputeRootConstantBufferView(
			COMPUTE_ROOT_ARG_PASS_EXTRA_CBV,
			mMemHandleUpdateCB.GpuAddress());

		//
		// Update 
		//   Input: previous alive particle list.
		//   Output: particles still alive to currently alive list.
		//

		cmdList->SetPipelineState(updateParticlesPso);

		const auto numCommands = 1u;
		const auto argOffset = static_cast<std::uint32_t>(5 * sizeof(UINT));
		cmdList->ExecuteIndirect(
			updateParticlesCommandSig,
			numCommands,
			mIndirectArgsBuffer.Get(),
			argOffset,
			nullptr, 0);

		//
		// Append new particles to the currently alive list.
		//

		cmdList->SetPipelineState(emitParticlesPso);

		for (auto i = 0u; i < mEmitInstances.size(); ++i)
		{
			const ParticleEmitCB& emitConstants = mEmitInstances[i];

			// Need to hold handle until we submit work to GPU.
			auto memHandle = linearAllocator.AllocateConstant(emitConstants);

			cmdList->SetComputeRootConstantBufferView(
				COMPUTE_ROOT_ARG_DISPATCH_CBV,
				memHandle.GpuAddress());

			const auto numGroupsX = static_cast<std::uint32_t>(std::ceilf(emitConstants.gEmitCount / 128.0f));
			cmdList->Dispatch(numGroupsX, 1, 1);

			mMemHandlesToEmitCB.emplace_back(std::move(memHandle));
		}

		if (particleCountReadback != nullptr)
		{
			auto readbackBarrier = DirectX::ScopedBarrier{
				cmdList,
				{
					D3D12::CD3DX12_RESOURCE_BARRIER::Transition(
					mCurrAliveCountBuffer.Get(),
					D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
					D3D12_RESOURCE_STATE_COPY_SOURCE)
				} 
			};

			cmdList->CopyResource(
				particleCountReadback,
				mCurrAliveCountBuffer.Get());
		}

		//
		// Post update CS
		//

		auto indirectArgsBarrier = DirectX::ScopedBarrier{
			cmdList,
			{
				D3D12::CD3DX12_RESOURCE_BARRIER::Transition(
				mIndirectArgsBuffer.Get(),
				D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT,
				D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
			} 
		};

		cmdList->SetComputeRootConstantBufferView(COMPUTE_ROOT_ARG_DISPATCH_CBV, mMemHandleUpdateCB.GpuAddress());
		cmdList->SetPipelineState(postUpdateParticlesPso);
		cmdList->Dispatch(1, 1, 1);
	}

	void Draw(
		D3D12::ID3D12GraphicsCommandList* cmdList,
		D3D12::ID3D12CommandSignature* drawParticlesCommandSig,
		D3D12::ID3D12PipelineState* drawParticlesPso
	)
	{
		cmdList->SetPipelineState(drawParticlesPso);

		auto ibv = D3D12::D3D12_INDEX_BUFFER_VIEW{
			.BufferLocation = mGeoIndexBuffer->GetGPUVirtualAddress(),
			.SizeInBytes = sizeof(std::uint32_t) * mMaxParticleCount * 6,
			.Format = DXGI_FORMAT_R32_UINT,
		};
		

		cmdList->IASetVertexBuffers(0, 0, nullptr);
		cmdList->IASetIndexBuffer(&ibv);
		cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

		auto drawCB = ParticleDrawCB{
			.gParticleBufferIndex = GetParticleBufferUavIndex(),
			.gParticleCurrAliveBufferIndex = GetCurrAliveIndexBufferUavIndex()
		};
		

		auto& linearAllocator = DirectX::GraphicsMemory::Get(md3dDevice);
		mMemHandleDrawCB = linearAllocator.AllocateConstant(drawCB);
		cmdList->SetGraphicsRootConstantBufferView(
			GFX_ROOT_ARG_OBJECT_CBV,
			mMemHandleDrawCB.GpuAddress());

		// Draw the current particles.
		const auto numCommands = 1u;
		const auto argOffset = 0u;
		cmdList->ExecuteIndirect(
			drawParticlesCommandSig,
			numCommands,
			mIndirectArgsBuffer.Get(),
			argOffset,
			nullptr, 0);

		//
		// Swap: current becomes prev for next update.
		//

		std::swap(mPrevAliveIndexBufferUavIndex, mCurrAliveIndexBufferUavIndex);
		std::swap(mPrevAliveCountUavIndex, mCurrAliveCountUavIndex);
	}

private:
	void BuildParticleBuffers(DirectX::ResourceUploadBatch& uploadBatch)
	{
		auto zeroParticles = std::vector<Particle>(mMaxParticleCount, Particle());

		CreateStaticBuffer(md3dDevice, uploadBatch,
			zeroParticles.data(), zeroParticles.size(), sizeof(Particle),
			D3D12::D3D12_RESOURCE_STATES::D3D12_RESOURCE_STATE_UNORDERED_ACCESS, mParticleBuffer.GetAddressOf(),
			D3D12::D3D12_RESOURCE_FLAGS::D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);

		auto zeroIndices = std::vector<std::uint32_t>(mMaxParticleCount, 0);

		CreateStaticBuffer(md3dDevice, uploadBatch,
			zeroIndices.data(), zeroIndices.size(), sizeof(std::uint32_t),
			D3D12::D3D12_RESOURCE_STATES::D3D12_RESOURCE_STATE_UNORDERED_ACCESS, mPrevAliveIndexBuffer.GetAddressOf(),
			D3D12::D3D12_RESOURCE_FLAGS::D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
		CreateStaticBuffer(md3dDevice, uploadBatch,
			zeroIndices.data(), zeroIndices.size(), sizeof(std::uint32_t),
			D3D12::D3D12_RESOURCE_STATES::D3D12_RESOURCE_STATE_UNORDERED_ACCESS, mCurrAliveIndexBuffer.GetAddressOf(),
			D3D12::D3D12_RESOURCE_FLAGS::D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);


		// Put in reverse order because we grab the free indices at the end of the buffer (like a stack), and
		// we want the indices to start at the front of the particle buffer (i.e., start at index 0).
		auto initFreeIndices = std::vector<std::uint32_t>(mMaxParticleCount);
		for (std::uint32_t i = 0; i < mMaxParticleCount; ++i)
			initFreeIndices[i] = mMaxParticleCount - 1 - i;

		CreateStaticBuffer(md3dDevice, uploadBatch,
			initFreeIndices.data(), initFreeIndices.size(), sizeof(std::uint32_t),
			D3D12::D3D12_RESOURCE_STATES::D3D12_RESOURCE_STATE_UNORDERED_ACCESS, mFreeIndexBuffer.GetAddressOf(),
			D3D12::D3D12_RESOURCE_FLAGS::D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);

		auto initCounters = std::array<std::uint32_t, 1>{};

		initCounters[0] = mMaxParticleCount;
		CreateStaticBuffer(md3dDevice, uploadBatch,
			initCounters.data(), initCounters.size(), sizeof(std::uint32_t),
			D3D12::D3D12_RESOURCE_STATES::D3D12_RESOURCE_STATE_UNORDERED_ACCESS, mFreeCountBuffer.GetAddressOf(),
			D3D12::D3D12_RESOURCE_FLAGS::D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);

		initCounters[0] = 0;
		CreateStaticBuffer(md3dDevice, uploadBatch,
			initCounters.data(), initCounters.size(), sizeof(std::uint32_t),
			D3D12::D3D12_RESOURCE_STATES::D3D12_RESOURCE_STATE_UNORDERED_ACCESS, mPrevAliveCountBuffer.GetAddressOf(),
			D3D12::D3D12_RESOURCE_FLAGS::D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);

		initCounters[0] = 0;
		CreateStaticBuffer(md3dDevice, uploadBatch,
			initCounters.data(), initCounters.size(), sizeof(std::uint32_t),
			D3D12::D3D12_RESOURCE_STATES::D3D12_RESOURCE_STATE_UNORDERED_ACCESS, mCurrAliveCountBuffer.GetAddressOf(),
			D3D12::D3D12_RESOURCE_FLAGS::D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);

		/*
		typedef struct D3D12_DRAW_INDEXED_ARGUMENTS
		{
		UINT IndexCountPerInstance;
		UINT InstanceCount;
		UINT StartIndexLocation;
		INT BaseVertexLocation;
		UINT StartInstanceLocation;
		} 	D3D12_DRAW_INDEXED_ARGUMENTS;
		*/
		/*
		typedef struct D3D12_DISPATCH_ARGUMENTS
		{
		UINT ThreadGroupCountX;
		UINT ThreadGroupCountY;
		UINT ThreadGroupCountZ;
		} 	D3D12_DISPATCH_ARGUMENTS;
		*/

		// Buffer stores args for 1 draw-indexed indirect and 1 dispatch indirect.
		// First 5 UINTs store D3D12_DRAW_ARGUMENTS, next 3 store D3D12_DISPATCH_ARGUMENTS.

		auto initIndirect = std::array<std::uint32_t, 8>{ 0, 0, 0, 0, 0, 0, 0, 0 };
		CreateStaticBuffer(md3dDevice, uploadBatch,
			initIndirect.data(), initIndirect.size(), sizeof(std::uint32_t),
			D3D12::D3D12_RESOURCE_STATES::D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT, mIndirectArgsBuffer.GetAddressOf(),
			D3D12::D3D12_RESOURCE_FLAGS::D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
	}

private:
	std::vector<ParticleEmitCB> mEmitInstances;

	std::vector<DirectX::GraphicsResource> mMemHandlesToEmitCB;
	DirectX::GraphicsResource mMemHandleUpdateCB;
	DirectX::GraphicsResource mMemHandleDrawCB;

	std::uint32_t mMaxParticleCount = 0;

	bool mRequiresSorting = false;

	std::uint32_t mParticleBufferUavIndex = -1;
	std::uint32_t mFreeIndexBufferUavIndex = -1;
	std::uint32_t mPrevAliveIndexBufferUavIndex = -1;
	std::uint32_t mCurrAliveIndexBufferUavIndex = -1;
	std::uint32_t mFreeCountUavIndex = -1;
	std::uint32_t mPrevAliveCountUavIndex = -1;
	std::uint32_t mCurrAliveCountUavIndex = -1;
	std::uint32_t mIndirectArgsUavIndex = -1;

	D3D12::ID3D12Device* md3dDevice = nullptr;

	// For drawing.
	Microsoft::WRL::ComPtr<D3D12::ID3D12Resource> mGeoIndexBuffer = nullptr;

	// Stores the actual particle data.
	Microsoft::WRL::ComPtr<D3D12::ID3D12Resource> mParticleBuffer = nullptr;

	// Stores indices to free particles.
	Microsoft::WRL::ComPtr<D3D12::ID3D12Resource> mFreeIndexBuffer = nullptr;

	// Stores indices to previous alive particles.
	Microsoft::WRL::ComPtr<D3D12::ID3D12Resource> mPrevAliveIndexBuffer = nullptr;

	// Stores indices of alive particles to draw. During update, we kill off
	// particles, so need a new updated list of alive particles to draw.
	Microsoft::WRL::ComPtr<D3D12::ID3D12Resource> mCurrAliveIndexBuffer = nullptr;

	Microsoft::WRL::ComPtr<D3D12::ID3D12Resource> mFreeCountBuffer = nullptr;
	Microsoft::WRL::ComPtr<D3D12::ID3D12Resource> mPrevAliveCountBuffer = nullptr;
	Microsoft::WRL::ComPtr<D3D12::ID3D12Resource> mCurrAliveCountBuffer = nullptr;

	Microsoft::WRL::ComPtr<D3D12::ID3D12Resource> mIndirectArgsBuffer = nullptr;
};
