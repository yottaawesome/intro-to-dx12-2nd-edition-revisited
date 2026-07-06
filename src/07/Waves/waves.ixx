export module waves;
import std;
import shared;

constexpr auto CBV_SRV_UAV_HEAP_CAPACITY = 16384u;

struct ObjectConstants
{
	DirectX::XMFLOAT4X4 World = MathHelper::Identity4x4;
};

struct PassConstants
{
	DirectX::XMFLOAT4X4 ViewProj = MathHelper::Identity4x4;
};

struct ColorVertex
{
	DirectX::XMFLOAT3 Pos;
	DirectX::XMFLOAT4 Color;
};

enum ROOT_ARG
{
	ROOT_ARG_OBJECT_CBV = 0,
	ROOT_ARG_PASS_CBV,
	ROOT_ARG_COUNT
};

enum class RenderLayer : int
{
	Opaque = 0,
	Debug,
	Sky,
	Count
};

// Stores the resources needed for the CPU to build the command lists
// for a frame.  
struct FrameResource
{
public:

	FrameResource(D3D12::ID3D12Device* device, Win32::UINT passCount, Win32::UINT waveVertCount)
	{
		auto hr = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, __uuidof(D3D12::ID3D12CommandAllocator), &CmdListAlloc);
		if (Win32::Failed(hr))
			throw DxException{ hr };

		PassCB = std::make_unique<UploadBuffer<PassConstants>>(device, passCount, true);
		WavesVB = std::make_unique<UploadBuffer<ColorVertex>>(device, waveVertCount, false);
	}
	FrameResource(const FrameResource& rhs) = delete;
	FrameResource& operator=(const FrameResource& rhs) = delete;

	// We cannot reset the allocator until the GPU is done processing the commands.
	// So each frame needs their own allocator.
	Microsoft::WRL::ComPtr<D3D12::ID3D12CommandAllocator> CmdListAlloc;

	// We cannot update a buffer until the GPU is done processing the commands
	// that reference it.  So each frame needs their own buffers.
	std::unique_ptr<UploadBuffer<PassConstants>> PassCB = nullptr;

	// We cannot update a dynamic vertex buffer until the GPU is done processing
	// the commands that reference it.  So each frame needs their own.
	std::unique_ptr<UploadBuffer<ColorVertex>> WavesVB = nullptr;

	// Fence value to mark commands up to this fence point.  This lets us
	// check if these frame resources are still in use by the GPU.
	Win32::UINT64 Fence = 0;
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

	ObjectConstants ObjectCB;

	// Handle to memory in linear allocator.
	DirectX::GraphicsResource MemHandleToObjectCB;

	Material* Mat = nullptr;
	MeshGeometry* Geo = nullptr;

	// Primitive topology.
	D3D::D3D_PRIMITIVE_TOPOLOGY PrimitiveType = D3D::D3D_PRIMITIVE_TOPOLOGY::D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;

	// DrawIndexedInstanced parameters.
	Win32::UINT IndexCount = 0;
	Win32::UINT StartIndexLocation = 0;
	int BaseVertexLocation = 0;
};

class Waves
{
public:
	constexpr Waves(int m, int n, float dx, float dt, float speed, float damping)
	{
		mNumRows = m;
		mNumCols = n;

		mVertexCount = m * n;
		mTriangleCount = (m - 1) * (n - 1) * 2;

		mTimeStep = dt;
		mSpatialStep = dx;

		SetConstants(speed, damping);

		mPrevSolution.resize(m * n);
		mCurrSolution.resize(m * n);
		mNormals.resize(m * n);
		mTangentX.resize(m * n);

		// Generate grid vertices in system memory.

		auto halfWidth = (n - 1) * dx * 0.5f;
		auto halfDepth = (m - 1) * dx * 0.5f;
		for (auto i = 0; i < m; ++i)
		{
			auto z = halfDepth - i * dx;
			for (auto j = 0; j < n; ++j)
			{
				auto x = -halfWidth + j * dx;
				mPrevSolution[i * n + j] = DirectX::XMFLOAT3(x, 0.0f, z);
				mCurrSolution[i * n + j] = DirectX::XMFLOAT3(x, 0.0f, z);
				mNormals[i * n + j] = DirectX::XMFLOAT3(0.0f, 1.0f, 0.0f);
				mTangentX[i * n + j] = DirectX::XMFLOAT3(1.0f, 0.0f, 0.0f);
			}
		}
	}

	Waves(const Waves& rhs) = delete;
	Waves& operator=(const Waves& rhs) = delete;

	constexpr auto RowCount()const noexcept -> int { return mNumRows; }
	constexpr auto ColumnCount()const noexcept -> int { return mNumCols; }
	constexpr auto VertexCount()const noexcept -> int { return mVertexCount; }
	constexpr auto TriangleCount()const noexcept -> int { return mTriangleCount; }
	constexpr auto Width()const noexcept -> float { return mNumCols * mSpatialStep; }
	constexpr auto Depth()const noexcept -> float { return mNumRows * mSpatialStep; }

	// Returns the solution at the ith grid point.
	constexpr auto Position(int i)const -> const DirectX::XMFLOAT3& { return mCurrSolution[i]; }

	// Returns the solution normal at the ith grid point.
	constexpr auto Normal(int i)const -> const DirectX::XMFLOAT3& { return mNormals[i]; }

	// Returns the unit tangent vector at the ith grid point in the local x-axis direction.
	constexpr auto TangentX(int i)const -> const DirectX::XMFLOAT3& { return mTangentX[i]; }

	constexpr void SetConstants(float speed, float damping)
	{
		auto d = damping * mTimeStep + 2.0f;
		auto e = (speed * speed) * (mTimeStep * mTimeStep) / (mSpatialStep * mSpatialStep);
		mK1 = (damping * mTimeStep - 2.0f) / d;
		mK2 = (4.0f - 8.0f * e) / d;
		mK3 = (2.0f * e) / d;
	}

	void Update(float dt)
	{
		static auto t = 0.f;

		// Accumulate time.
		t += dt;

		// Only update the simulation at the specified time step.
		if (t < mTimeStep)
			return;

		// Only update interior points; we use zero boundary conditions.
		Concurrency::parallel_for(
			1, 
			mNumRows - 1, 
			[this](int i) //for(int i = 1; i < mNumRows-1; ++i)
			{
				for (int j = 1; j < mNumCols - 1; ++j)
				{
					// After this update we will be discarding the old previous
					// buffer, so overwrite that buffer with the new update.
					// Note how we can do this inplace (read/write to same element) 
					// because we won't need prev_ij again and the assignment happens last.

					// Note j indexes x and i indexes z: h(x_j, z_i, t_k)
					// Moreover, our +z axis goes "down"; this is just to 
					// keep consistent with our row indices going down.

					mPrevSolution[i * mNumCols + j].y =
						mK1 * mPrevSolution[i * mNumCols + j].y +
						mK2 * mCurrSolution[i * mNumCols + j].y +
						mK3 * (mCurrSolution[(i + 1) * mNumCols + j].y +
							mCurrSolution[(i - 1) * mNumCols + j].y +
							mCurrSolution[i * mNumCols + j + 1].y +
							mCurrSolution[i * mNumCols + j - 1].y);
				}
			});

		// We just overwrote the previous buffer with the new data, so
		// this data needs to become the current solution and the old
		// current solution becomes the new previous solution.
		std::swap(mPrevSolution, mCurrSolution);

		t = 0.0f; // reset time

		//
		// Compute normals using finite difference scheme.
		//
		Concurrency::parallel_for(
			1, 
			mNumRows - 1, 
			[this](int i) //for(int i = 1; i < mNumRows - 1; ++i)
			{
				for (int j = 1; j < mNumCols - 1; ++j)
				{
					float l = mCurrSolution[i * mNumCols + j - 1].y;
					float r = mCurrSolution[i * mNumCols + j + 1].y;
					float t = mCurrSolution[(i - 1) * mNumCols + j].y;
					float b = mCurrSolution[(i + 1) * mNumCols + j].y;
					mNormals[i * mNumCols + j].x = -r + l;
					mNormals[i * mNumCols + j].y = 2.0f * mSpatialStep;
					mNormals[i * mNumCols + j].z = b - t;

					auto n = DirectX::XMVECTOR{DirectX::XMVector3Normalize(XMLoadFloat3(&mNormals[i * mNumCols + j]))};
					DirectX::XMStoreFloat3(&mNormals[i * mNumCols + j], n);

					mTangentX[i * mNumCols + j] = DirectX::XMFLOAT3(2.0f * mSpatialStep, r - l, 0.0f);
					auto T = DirectX::XMVECTOR{DirectX::XMVector3Normalize(DirectX::XMLoadFloat3(&mTangentX[i * mNumCols + j]))};
					DirectX::XMStoreFloat3(&mTangentX[i * mNumCols + j], T);
				}
			});
	}
	constexpr void Disturb(int i, int j, float magnitude)
	{
		// Don't disturb boundaries.
		//assert(i > 1 && i < mNumRows - 2);
		//assert(j > 1 && j < mNumCols - 2);

		float halfMag = 0.5f * magnitude;

		// Disturb the ijth vertex height and its neighbors.
		mCurrSolution[i * mNumCols + j].y += magnitude;
		mCurrSolution[i * mNumCols + j + 1].y += halfMag;
		mCurrSolution[i * mNumCols + j - 1].y += halfMag;
		mCurrSolution[(i + 1) * mNumCols + j].y += halfMag;
		mCurrSolution[(i - 1) * mNumCols + j].y += halfMag;
	}

private:
	int mNumRows = 0;
	int mNumCols = 0;

	int mVertexCount = 0;
	int mTriangleCount = 0;

	// Simulation constants we can precompute.
	float mK1 = 0.0f;
	float mK2 = 0.0f;
	float mK3 = 0.0f;

	float mTimeStep = 0.0f;
	float mSpatialStep = 0.0f;

	std::vector<DirectX::XMFLOAT3> mPrevSolution;
	std::vector<DirectX::XMFLOAT3> mCurrSolution;
	std::vector<DirectX::XMFLOAT3> mNormals;
	std::vector<DirectX::XMFLOAT3> mTangentX;
};

export class WavesApp : public D3DApp
{
public:
	WavesApp(Win32::HINSTANCE hInstance)
		: D3DApp(hInstance)
	{ }
	WavesApp(const WavesApp& rhs) = delete;
	WavesApp& operator=(const WavesApp& rhs) = delete;
	~WavesApp()
	{
		if (md3dDevice)
			FlushCommandQueue();
	}

	auto Initialize() -> bool override
	{
		if (not D3DApp::Initialize())
			return false;

		mWaves = std::make_unique<Waves>(128, 128, 1.0f, 0.016f, mWaveSpeed, mWaveDamping);

		// We will upload on the direct queue for the book samples, but 
		// copy queue would be better for real game.
		mUploadBatch->Begin(D3D12_COMMAND_LIST_TYPE_DIRECT);

		auto landGeo = std::unique_ptr<MeshGeometry>{ BuildLandGeometry(md3dDevice.Get(), *mUploadBatch.get()) };
		mGeometries[landGeo->Name] = std::move(landGeo);
		auto waveGeo = std::unique_ptr<MeshGeometry>{ BuildWaveGeometry(md3dDevice.Get(), *mUploadBatch.get()) };
		mGeometries[waveGeo->Name] = std::move(waveGeo);

		// Kick off upload work asyncronously.
		auto result = std::future<void>{mUploadBatch->End(mCommandQueue.Get())};

		// Other init work...
		BuildRootSignature();
		BuildCbvSrvUavDescriptorHeap();
		BuildShadersAndInputLayout();
		BuildRenderItems();
		BuildFrameResources();
		BuildPSOs();

		// Block until the upload work is complete.
		result.wait();

		return true;
	}

private:
	void CreateRtvAndDsvDescriptorHeaps()override
	{
		mRtvHeap.Init(md3dDevice.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_RTV, SwapChainBufferCount);
		mDsvHeap.Init(md3dDevice.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_DSV, SwapChainBufferCount);
	}

	void OnResize()override
	{
		D3DApp::OnResize();

		// The window resized, so update the aspect ratio and recompute the projection matrix.
		auto P = DirectX::XMMATRIX{DirectX::XMMatrixPerspectiveFovLH(0.25f * MathHelper::Pi, AspectRatio(), 1.0f, 1000.0f)};
		DirectX::XMStoreFloat4x4(&mProj, P);
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
		if (mCurrFrameResource->Fence != 0 && mFence->GetCompletedValue() < mCurrFrameResource->Fence)
		{
			auto event = Event{};
			if (auto hr = mFence->SetEventOnCompletion(mCurrFrameResource->Fence, event.Get()); Win32::Failed(hr))
				throw DxException{ hr };
			event.Wait();
		}

		UpdatePerObjectCB(gt);
		UpdateMainPassCB(gt);
		UpdateWaves(gt);
	}

	void Draw(const GameTimer& gt)override
	{
		auto& cbvSrvUavHeap = CbvSrvUavHeap::Get();

		UpdateImgui(gt);

		auto cmdListAlloc = mCurrFrameResource->CmdListAlloc;

		// Reuse the memory associated with command recording.
		// We can only reset when the associated command lists have finished execution on the GPU.
		
		if (auto hr = cmdListAlloc->Reset(); Win32::Failed(hr))
			throw DxException{ hr };

		// A command list can be reset after it has been added to the command queue via ExecuteCommandList.
		// Reusing the command list reuses memory.
		if (auto hr = mCommandList->Reset(cmdListAlloc.Get(), mPSOs["opaque"].Get()); Win32::Failed(hr))
			throw DxException{ hr };

		auto descriptorHeaps = std::array{ cbvSrvUavHeap.GetD3dHeap() };
		mCommandList->SetDescriptorHeaps(static_cast<UINT>(descriptorHeaps.size()), descriptorHeaps.data());

		mCommandList->SetGraphicsRootSignature(mRootSignature.Get());

		mCommandList->RSSetViewports(1, &mScreenViewport);
		mCommandList->RSSetScissorRects(1, &mScissorRect);

		// Indicate a state transition on the resource usage.
		auto transition = D3D12::CD3DX12_RESOURCE_BARRIER::Transition(CurrentBackBuffer(),
			D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
		mCommandList->ResourceBarrier(1, &transition);

		// Clear the back buffer and depth buffer.
		mCommandList->ClearRenderTargetView(CurrentBackBufferView(), DirectX::Colors::LightSteelBlue, 0, nullptr);
		mCommandList->ClearDepthStencilView(
			DepthStencilView(), 
			D3D12::D3D12_CLEAR_FLAGS{ D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL },
			1.0f, 0, 0, nullptr);

		// Specify the buffers we are going to render to.
		auto cbbv = CurrentBackBufferView();
		auto dsv = DepthStencilView();
		mCommandList->OMSetRenderTargets(1, &cbbv, true, &dsv);

		auto passCB = mCurrFrameResource->PassCB->Resource();
		mCommandList->SetGraphicsRootConstantBufferView(ROOT_ARG_PASS_CBV, passCB->GetGPUVirtualAddress());

		mCommandList->SetPipelineState(mDrawWireframe ? mPSOs["opaque_wireframe"].Get() : mPSOs["opaque"].Get());
		DrawRenderItems(mCommandList.Get(), mRitemLayer[(int)RenderLayer::Opaque]);

		// Draw imgui UI.
		ImGui::ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), mCommandList.Get());

		// Indicate a state transition on the resource usage.
		transition = D3D12::CD3DX12_RESOURCE_BARRIER::Transition(CurrentBackBuffer(),
			D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
		mCommandList->ResourceBarrier(1, &transition);

		// Done recording commands.
		if (auto hr = mCommandList->Close(); Win32::Failed(hr))
			throw DxException{ hr };

		mLinearAllocator->Commit(mCommandQueue.Get());

		// Add the command list to the queue for execution.
		auto cmdsLists = std::array<D3D12::ID3D12CommandList*, 1>{ mCommandList.Get() };
		mCommandQueue->ExecuteCommandLists(static_cast<UINT>(cmdsLists.size()), cmdsLists.data());

		// Swap the back and front buffers
		auto presentParams = DXGI::DXGI_PRESENT_PARAMETERS{ 0 };
		
		if (auto hr = mSwapChain->Present1(0, 0, &presentParams); Win32::Failed(hr))
			throw DxException{ hr };
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
	void OnMouseDown(WPARAM btnState, int x, int y)override
	{
		if (auto& io = ImGui::GetIO(); not io.WantCaptureMouse)
		{
			mLastMousePos.x = x;
			mLastMousePos.y = y;
			Win32::SetCapture(mhMainWnd);
		}
	}

	void OnMouseUp(WPARAM btnState, int x, int y)override
	{
		if (auto& io = ImGui::GetIO(); not io.WantCaptureMouse)
			Win32::ReleaseCapture();
	}
	void OnMouseMove(Win32::WPARAM btnState, int x, int y)override
	{
		auto& io = ImGui::GetIO();
		if (io.WantCaptureMouse)
			return;

		if ((btnState & Win32::MK::LButton) != 0)
		{
			// Make each pixel correspond to a quarter of a degree.
			auto dx = DirectX::XMConvertToRadians(0.25f * static_cast<float>(x - mLastMousePos.x));
			auto dy = DirectX::XMConvertToRadians(0.25f * static_cast<float>(y - mLastMousePos.y));

			// Update angles based on input to orbit camera around box.
			mTheta += dx;
			mPhi += dy;

			// Restrict the angle mPhi.
			mPhi = std::clamp(mPhi, 0.1f, MathHelper::Pi - 0.1f);
		}
		else if ((btnState & Win32::MK::RButton) != 0)
		{
			// Make each pixel correspond to 0.005 unit in the scene.
			auto dx = 0.05f * static_cast<float>(x - mLastMousePos.x);
			auto dy = 0.05f * static_cast<float>(y - mLastMousePos.y);

			// Update the camera radius based on input.
			mRadius += dx - dy;

			// Restrict the radius.
			mRadius = std::clamp(mRadius, 5.0f, 150.0f);
		}

		mLastMousePos.x = x;
		mLastMousePos.y = y;
	}
	
	void OnKeyboardInput(const GameTimer& gt) {}

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
		// Update per object constants once per frame so the data can be shared across different render passes.
		for (auto& ri : mAllRitems)
		{
			DirectX::XMStoreFloat4x4(
				&ri->ObjectCB.World,
				DirectX::XMMatrixTranspose(DirectX::XMLoadFloat4x4(&ri->World)));

			// Need to hold handle until we submit work to GPU.
			ri->MemHandleToObjectCB = mLinearAllocator->AllocateConstant(ri->ObjectCB);
		}
	}
	void UpdateMainPassCB(const GameTimer& gt)
	{
		auto view = DirectX::XMMATRIX{DirectX::XMLoadFloat4x4(&mView)};
		auto proj = DirectX::XMMATRIX{DirectX::XMLoadFloat4x4(&mProj)};
		auto viewProj = DirectX::XMMatrixMultiply(view, proj);

		DirectX::XMStoreFloat4x4(&mMainPassCB.ViewProj, DirectX::XMMatrixTranspose(viewProj));

		auto currPassCB = mCurrFrameResource->PassCB.get();
		currPassCB->CopyData(0, mMainPassCB);
	}

	void UpdateWaves(const GameTimer& gt)
	{
		// Every quarter second, generate a random wave.
		static auto t_base = 0.0f;
		if ((mTimer.TotalTime() - t_base) >= 0.25f)
		{
			t_base += 0.25f;
			auto i = MathHelper::Rand(4, mWaves->RowCount() - 5);
			auto j = MathHelper::Rand(4, mWaves->ColumnCount() - 5);
			auto r = mWaveScale * MathHelper::RandF(0.3f, 0.6f);
			mWaves->Disturb(i, j, r);
		}

		// Update the wave simulation.
		mWaves->Update(gt.DeltaTime());

		// Update the wave vertex buffer with the new solution.
		auto currWavesVB = mCurrFrameResource->WavesVB.get();
		auto verts = std::vector<ColorVertex>(mWaves->VertexCount());
		for (int i = 0; i < mWaves->VertexCount(); ++i)
		{
			verts[i].Pos = mWaves->Position(i);
			verts[i].Color = DirectX::XMFLOAT4{DirectX::Colors::Blue};
		}
		currWavesVB->CopyData(verts.data(), static_cast<UINT>(verts.size()));

		// Set the dynamic VB of the wave renderitem to the current frame VB.
		mWavesRitem->Geo->VertexBufferGPU = currWavesVB->Resource();
	}

	void BuildCbvSrvUavDescriptorHeap()
	{
		auto& cbvSrvUavHeap = CbvSrvUavHeap::Get();
		cbvSrvUavHeap.Init(md3dDevice.Get(), CBV_SRV_UAV_HEAP_CAPACITY);
		InitImgui(cbvSrvUavHeap);
	}

	void BuildRootSignature()
	{
		// Root parameter can be a table, root descriptor or root constants.
		auto gfxRootParameters = std::array<D3D12::CD3DX12_ROOT_PARAMETER, ROOT_ARG_COUNT>{};

		// Perfomance TIP: Order from most frequent to least frequent.
		gfxRootParameters[ROOT_ARG_OBJECT_CBV].InitAsConstantBufferView(0);
		gfxRootParameters[ROOT_ARG_PASS_CBV].InitAsConstantBufferView(1);

		// A root signature is an array of root parameters.
		auto rootSigDesc = D3D12::CD3DX12_ROOT_SIGNATURE_DESC{
			ROOT_ARG_COUNT,
			gfxRootParameters.data(),
			0, 
			nullptr,
			D3D12::D3D12_ROOT_SIGNATURE_FLAGS::D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT};

		// create a root signature with a single slot which points to a descriptor range consisting of a single constant buffer
		auto serializedRootSig = Microsoft::WRL::ComPtr<D3D::ID3DBlob>{};
		auto errorBlob = Microsoft::WRL::ComPtr<D3D::ID3DBlob>{};
		auto hr = D3D12::D3D12SerializeRootSignature(
			&rootSigDesc,
			D3D::D3D_ROOT_SIGNATURE_VERSION::D3D_ROOT_SIGNATURE_VERSION_1,
			serializedRootSig.GetAddressOf(),
			errorBlob.GetAddressOf());

		if (errorBlob)
			Win32::OutputDebugStringA((char*)errorBlob->GetBufferPointer());
		if (Win32::Failed(hr))
			throw DxException{ hr };

		hr = md3dDevice->CreateRootSignature(
			0,
			serializedRootSig->GetBufferPointer(),
			serializedRootSig->GetBufferSize(),
			__uuidof(D3D12::ID3D12RootSignature), 
			&mRootSignature);
		if (Win32::Failed(hr))
			throw DxException{ hr };
	}

	void BuildShadersAndInputLayout()
	{
		if constexpr (IsDebugBuild)
		{
			mShaders["standardVS"] = d3dUtil::CompileShader(L"Shaders\\BasicColor.hlsl", { L"-E", L"VS", L"-T", L"vs_6_6",DXC::ArgDebug, DXC::ArgSkipOptimizations });
			mShaders["opaquePS"] = d3dUtil::CompileShader(L"Shaders\\BasicColor.hlsl", { L"-E", L"PS", L"-T", L"ps_6_6",DXC::ArgDebug, DXC::ArgSkipOptimizations });
		}
		else
		{
			mShaders["standardVS"] = d3dUtil::CompileShader(L"Shaders\\BasicColor.hlsl", { L"-E", L"VS", L"-T", L"vs_6_6" });
			mShaders["opaquePS"] = d3dUtil::CompileShader(L"Shaders\\BasicColor.hlsl", { L"-E", L"PS", L"-T", L"ps_6_6" });
		}

		mInputLayout = {
			{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
			{ "COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }
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

		auto hr = md3dDevice->CreateGraphicsPipelineState(&basePsoDesc, __uuidof(D3D12::ID3D12PipelineState), &mPSOs["opaque"]);
		if (Win32::Failed(hr))
			throw DxException{ hr };

		auto wireframePsoDesc = D3D12::D3D12_GRAPHICS_PIPELINE_STATE_DESC{basePsoDesc};
		wireframePsoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_WIREFRAME;
		hr = md3dDevice->CreateGraphicsPipelineState(&wireframePsoDesc, __uuidof(D3D12::ID3D12PipelineState), &mPSOs["opaque_wireframe"]);
		if (Win32::Failed(hr))
			throw DxException{ hr };
	}

	void BuildFrameResources()
	{
		constexpr auto passCount = 1u;
		for (int i = 0; i < gNumFrameResources; ++i)
			mFrameResources.push_back(std::make_unique<FrameResource>(md3dDevice.Get(), passCount, mWaves->VertexCount()));
	}

	void AddRenderItem(RenderLayer layer, const DirectX::XMFLOAT4X4& world, MeshGeometry* geo, SubmeshGeometry& drawArgs)
	{
		auto ritem = std::make_unique<RenderItem>();
		ritem->World = world;
		ritem->TexTransform = MathHelper::Identity4x4;
		ritem->Mat = nullptr;
		ritem->Geo = geo;
		ritem->PrimitiveType = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
		ritem->IndexCount = drawArgs.IndexCount;
		ritem->StartIndexLocation = drawArgs.StartIndexLocation;
		ritem->BaseVertexLocation = drawArgs.BaseVertexLocation;

		mRitemLayer[(int)layer].push_back(ritem.get());
		mAllRitems.push_back(std::move(ritem));
	}

	void BuildRenderItems()
	{
		auto worldTransform = MathHelper::Identity4x4;
		AddRenderItem(RenderLayer::Opaque, worldTransform, mGeometries["waterGeo"].get(), mGeometries["waterGeo"]->DrawArgs["grid"]);
		mWavesRitem = mAllRitems.back().get();
		AddRenderItem(RenderLayer::Opaque, worldTransform, mGeometries["landGeo"].get(), mGeometries["landGeo"]->DrawArgs["grid"]);
	}

	void DrawRenderItems(D3D12::ID3D12GraphicsCommandList* cmdList, const std::vector<RenderItem*>& ritems)
	{
		for (auto i = 0ull; i < ritems.size(); ++i)
		{
			auto ri = ritems[i];
			auto vbv = ri->Geo->VertexBufferView();
			auto ibv = ri->Geo->IndexBufferView();
			cmdList->IASetVertexBuffers(0, 1, &vbv);
			cmdList->IASetIndexBuffer(&ibv);
			cmdList->IASetPrimitiveTopology(ri->PrimitiveType);
			cmdList->SetGraphicsRootConstantBufferView(ROOT_ARG_OBJECT_CBV, ri->MemHandleToObjectCB.GpuAddress());
			cmdList->DrawIndexedInstanced(ri->IndexCount, 1, ri->StartIndexLocation, ri->BaseVertexLocation, 0);
		}
	}
	
	auto BuildShapeGeometry(D3D12::ID3D12Device* device, DirectX::ResourceUploadBatch& uploadBatch) -> std::unique_ptr<MeshGeometry>
	{
		auto meshGen = MeshGen{};
		auto box = MeshGenData{ meshGen.CreateBox(1.0f, 1.0f, 1.0f, 3) };
		auto grid = MeshGenData{ meshGen.CreateGrid(20.0f, 30.0f, 60, 40) };
		auto sphere = MeshGenData{ meshGen.CreateSphere(0.5f, 20, 20) };
		auto cylinder = MeshGenData{ meshGen.CreateCylinder(0.5f, 0.3f, 3.0f, 20, 20) };
		auto quad = MeshGenData{ meshGen.CreateQuad(0.0f, 0.0f, 1.0f, 1.0f, 0.0f) };

		//
		// We are concatenating all the geometry into one big vertex/index buffer.  So
		// define the regions in the buffer each submesh covers.
		//
		auto compositeMesh = MeshGenData{};
		auto boxSubmesh = SubmeshGeometry{ compositeMesh.AppendSubmesh(box) };
		auto gridSubmesh = SubmeshGeometry{ compositeMesh.AppendSubmesh(grid) };
		auto sphereSubmesh = SubmeshGeometry{ compositeMesh.AppendSubmesh(sphere) };
		auto cylinderSubmesh = SubmeshGeometry{ compositeMesh.AppendSubmesh(cylinder) };
		auto quadSubmesh = SubmeshGeometry{ compositeMesh.AppendSubmesh(quad) };

		auto color = DirectX::XMFLOAT4(0.2f, 0.2f, 0.2f, 1.0f);

		// Extract the vertex elements we are interested into our vertex buffer. 
		auto vertices = std::vector<ColorVertex>(compositeMesh.Vertices.size());
		for (auto i = 0ull; i < compositeMesh.Vertices.size(); ++i)
		{
			vertices[i].Pos = compositeMesh.Vertices[i].Position;
			vertices[i].Color = color;
		}

		constexpr auto indexElementByteSize = sizeof(uint16_t);
		const auto indexCount = static_cast<UINT>(compositeMesh.Indices32.size());
		const auto vbByteSize = static_cast<UINT>(vertices.size() * sizeof(ColorVertex));
		const auto ibByteSize = static_cast<UINT>(indexCount * indexElementByteSize);

		auto indexData = reinterpret_cast<Win32::byte*>(compositeMesh.GetIndices16().data());

		auto geo = std::make_unique<MeshGeometry>();
		geo->Name = "shapeGeo";

		geo->VertexBufferCPU.resize(vbByteSize);
		std::memcpy(geo->VertexBufferCPU.data(), vertices.data(), vbByteSize);

		geo->IndexBufferCPU.resize(ibByteSize);
		std::memcpy(geo->IndexBufferCPU.data(), indexData, ibByteSize);

		DirectX::CreateStaticBuffer(device, uploadBatch,
			vertices.data(), vertices.size(), sizeof(ColorVertex),
			D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER, &geo->VertexBufferGPU);

		DirectX::CreateStaticBuffer(device, uploadBatch,
			indexData, indexCount, indexElementByteSize,
			D3D12_RESOURCE_STATE_INDEX_BUFFER, &geo->IndexBufferGPU);

		geo->VertexByteStride = sizeof(ColorVertex);
		geo->VertexBufferByteSize = vbByteSize;
		geo->IndexFormat = DXGI_FORMAT_R16_UINT;
		geo->IndexBufferByteSize = ibByteSize;

		geo->DrawArgs["box"] = boxSubmesh;
		geo->DrawArgs["grid"] = gridSubmesh;
		geo->DrawArgs["sphere"] = sphereSubmesh;
		geo->DrawArgs["cylinder"] = cylinderSubmesh;
		geo->DrawArgs["quad"] = quadSubmesh;

		return geo;
	}

	auto BuildLandGeometry(D3D12::ID3D12Device* device, DirectX::ResourceUploadBatch& uploadBatch) -> std::unique_ptr<MeshGeometry>
	{
		auto meshGen = MeshGen{};
		auto grid = MeshGenData{ meshGen.CreateGrid(160.0f, 160.0f, 50, 50) };

		//
		// Extract the vertex elements we are interested and apply the height function to
		// each vertex.  In addition, color the vertices based on their height so we have
		// sandy looking beaches, grassy low hills, and snow mountain peaks.
		//

		auto vertices = std::vector<ColorVertex>(grid.Vertices.size());
		for (auto i = 0ull; i < grid.Vertices.size(); ++i)
		{
			auto& p = grid.Vertices[i].Position;
			vertices[i].Pos = p;
			vertices[i].Pos.y = GetHillsHeight(p.x, p.z);

			// Color the vertex based on its height.
			if (vertices[i].Pos.y < -10.0f)
			{
				// Sandy beach color.
				vertices[i].Color = DirectX::XMFLOAT4(1.0f, 0.96f, 0.62f, 1.0f);
			}
			else if (vertices[i].Pos.y < 5.0f)
			{
				// Light yellow-green.
				vertices[i].Color = DirectX::XMFLOAT4(0.48f, 0.77f, 0.46f, 1.0f);
			}
			else if (vertices[i].Pos.y < 12.0f)
			{
				// Dark yellow-green.
				vertices[i].Color = DirectX::XMFLOAT4(0.1f, 0.48f, 0.19f, 1.0f);
			}
			else if (vertices[i].Pos.y < 20.0f)
			{
				// Dark brown.
				vertices[i].Color = DirectX::XMFLOAT4(0.45f, 0.39f, 0.34f, 1.0f);
			}
			else
			{
				// White snow.
				vertices[i].Color = DirectX::XMFLOAT4(1.0f, 1.0f, 1.0f, 1.0f);
			}
		}

		const auto indexCount = static_cast<UINT>(grid.Indices32.size());
		constexpr auto indexElementByteSize = sizeof(uint16_t);
		const auto vbByteSize = static_cast<UINT>(vertices.size() * sizeof(ColorVertex));
		const auto ibByteSize = static_cast<UINT>(indexCount * indexElementByteSize);

		auto indices = std::vector<std::uint16_t>{grid.GetIndices16()};

		auto geo = std::make_unique<MeshGeometry>();
		geo->Name = "landGeo";

		geo->VertexBufferCPU.resize(vbByteSize);
		std::memcpy(geo->VertexBufferCPU.data(), vertices.data(), vbByteSize);

		geo->IndexBufferCPU.resize(ibByteSize);
		std::memcpy(geo->IndexBufferCPU.data(), indices.data(), ibByteSize);

		DirectX::CreateStaticBuffer(
			device, uploadBatch,
			vertices.data(), vertices.size(), sizeof(ColorVertex),
			D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER,
			&geo->VertexBufferGPU);

		DirectX::CreateStaticBuffer(
			device, uploadBatch,
			indices.data(), indexCount, indexElementByteSize,
			D3D12_RESOURCE_STATE_INDEX_BUFFER,
			&geo->IndexBufferGPU);

		geo->VertexByteStride = sizeof(ColorVertex);
		geo->VertexBufferByteSize = vbByteSize;
		geo->IndexFormat = DXGI_FORMAT_R16_UINT;
		geo->IndexBufferByteSize = ibByteSize;

		auto submesh = SubmeshGeometry{};
		submesh.IndexCount = static_cast<UINT>(indices.size());
		submesh.StartIndexLocation = 0;
		submesh.BaseVertexLocation = 0;
		submesh.VertexCount = static_cast<UINT>(vertices.size());
		geo->DrawArgs["grid"] = submesh;

		return geo;
	}

	auto BuildWaveGeometry(ID3D12Device* device, DirectX::ResourceUploadBatch& uploadBatch) -> std::unique_ptr<MeshGeometry>
	{
		auto indices = std::vector<std::uint32_t>(3 * mWaves->TriangleCount()); // 3 indices per face
		//assert(mWaves->VertexCount() < 0x0000ffff);

		// Iterate over each quad.
		auto m = mWaves->RowCount();
		auto n = mWaves->ColumnCount();
		auto k = 0;
		for (auto i = 0; i < m - 1; ++i)
		{
			for (auto j = 0; j < n - 1; ++j)
			{
				indices[k] = i * n + j;
				indices[k + 1] = i * n + j + 1;
				indices[k + 2] = (i + 1) * n + j;

				indices[k + 3] = (i + 1) * n + j;
				indices[k + 4] = i * n + j + 1;
				indices[k + 5] = (i + 1) * n + j + 1;

				k += 6; // next quad
			}
		}

		auto vbByteSize = static_cast<UINT>(mWaves->VertexCount() * sizeof(ColorVertex));
		auto ibByteSize = static_cast<UINT>(indices.size() * sizeof(std::uint32_t));

		auto geo = std::make_unique<MeshGeometry>();
		geo->Name = "waterGeo";

		// Set dynamically every frame in UpdateWaves().
		geo->VertexBufferCPU.clear();
		geo->VertexBufferGPU = nullptr;

		geo->IndexBufferCPU.resize(ibByteSize);
		std::memcpy(geo->IndexBufferCPU.data(), indices.data(), ibByteSize);

		DirectX::CreateStaticBuffer(
			device, uploadBatch,
			indices.data(), indices.size(), sizeof(std::uint32_t),
			D3D12_RESOURCE_STATE_INDEX_BUFFER,
			&geo->IndexBufferGPU);

		geo->VertexByteStride = sizeof(ColorVertex);
		geo->VertexBufferByteSize = vbByteSize;
		geo->IndexFormat = DXGI_FORMAT_R32_UINT;
		geo->IndexBufferByteSize = ibByteSize;

		auto submesh = SubmeshGeometry{};
		submesh.IndexCount = static_cast<UINT>(indices.size());
		submesh.StartIndexLocation = 0;
		submesh.BaseVertexLocation = 0;

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
			-0.03f * z * cosf(0.1f * x) - 0.3f * cosf(0.1f * z),
			1.0f,
			-0.3f * sinf(0.1f * x) + 0.03f * x * sinf(0.1f * z)};

		auto unitNormal = DirectX::XMVECTOR{ DirectX::XMVector3Normalize(DirectX::XMLoadFloat3(&n)) };
		DirectX::XMStoreFloat3(&n, unitNormal);

		return n;
	}

private:
	std::vector<std::unique_ptr<FrameResource>> mFrameResources;
	FrameResource* mCurrFrameResource = nullptr;
	int mCurrFrameResourceIndex = 0;

	Microsoft::WRL::ComPtr<D3D12::ID3D12RootSignature> mRootSignature;

	std::unordered_map<std::string, std::unique_ptr<MeshGeometry>> mGeometries;
	std::unordered_map<std::string, Microsoft::WRL::ComPtr<DXC::IDxcBlob>> mShaders;
	std::unordered_map<std::string, Microsoft::WRL::ComPtr<D3D12::ID3D12PipelineState>> mPSOs;

	std::vector<D3D12::D3D12_INPUT_ELEMENT_DESC> mInputLayout;

	// List of all the render items.
	std::vector<std::unique_ptr<RenderItem>> mAllRitems;

	// Render items divided by PSO.
	std::vector<RenderItem*> mRitemLayer[(int)RenderLayer::Count];

	RenderItem* mWavesRitem = nullptr;
	std::unique_ptr<Waves> mWaves;

	PassConstants mMainPassCB;

	DirectX::XMFLOAT4X4 mView = MathHelper::Identity4x4;
	DirectX::XMFLOAT4X4 mProj = MathHelper::Identity4x4;

	DirectX::XMFLOAT3 mEyePos = { 0.0f, 0.0f, 0.0f };
	float mTheta = 1.5f * DirectX::Pi;
	float mPhi = DirectX::PiOverTwo - 0.1f;
	float mRadius = 50.0f;

	Win32::POINT mLastMousePos{};

	bool mDrawWireframe = true;
	float mWaveScale = 1.0f;
	float mWaveSpeed = 8.0f;
	float mWaveDamping = 0.1f;
};
