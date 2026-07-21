export module picking;
import std;
import shared;

enum GFX_ROOT_ARG
{
	GFX_ROOT_ARG_OBJECT_CBV = 0,
	GFX_ROOT_ARG_PASS_CBV,
	GFX_ROOT_ARG_SKINNED_CBV,
	GFX_ROOT_ARG_MATERIAL_SRV,
	GFX_ROOT_ARG_INSTANCEDATA_SRV,
	GFX_ROOT_ARG_COUNT
};

enum COMPUTE_ROOT_ARG
{
	COMPUTE_ROOT_ARG_DISPATCH_CBV = 0,
	COMPUTE_ROOT_ARG_PASS_CBV,
	COMPUTE_ROOT_ARG_PASS_EXTRA_CBV,
	COMPUTE_ROOT_ARG_COUNT
};


// Stores the resources needed for the CPU to build the command lists
// for a frame.  
struct FrameResource
{
	FrameResource(D3D12::ID3D12Device* device, std::uint32_t passCount, std::uint32_t objectCount, std::uint32_t materialCount)
	{
		ThrowIfFailed(device->CreateCommandAllocator(
			D3D12_COMMAND_LIST_TYPE_DIRECT,
			__uuidof(D3D12::ID3D12CommandAllocator), reinterpret_cast<void**>(CmdListAlloc.GetAddressOf())));

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

enum RtvOffsets
{
	// Start after swapchain buffers.
	RTV_OFFSET = D3DApp::SwapChainBufferCount,
};

enum DsvOffsets
{
	DSV_MAINVIEW = 0,
};

constexpr auto CBV_SRV_UAV_HEAP_CAPACITY = 16384u;

// Lightweight structure stores parameters to draw a shape.  This will
// vary from app-to-app.
struct RenderItem
{
	bool Visible = true;
	DirectX::BoundingBox Bounds;

	// World matrix of the shape that describes the object's local space
	// relative to the world space, which defines the position, orientation,
	// and scale of the object in the world.
	DirectX::XMFLOAT4X4 World = MathHelper::Identity4x4;

	DirectX::XMFLOAT4X4 TexTransform = MathHelper::Identity4x4;

	PerObjectCB ObjectConstants;

	// Handle to memory in linear allocator.
	DirectX::GraphicsResource MemHandleToObjectCB;

	Material* Mat = nullptr;
	MeshGeometry* Geo = nullptr;

	// Primitive topology.
	D3D12::D3D12_PRIMITIVE_TOPOLOGY PrimitiveType = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;

	// DrawIndexedInstanced parameters.
	std::uint32_t IndexCount = 0;
	std::uint32_t StartIndexLocation = 0;
	int BaseVertexLocation = 0;
};

enum class RenderLayer : int
{
	Opaque = 0,
	Highlight,
	Debug,
	Sky,
	Count
};

export class PickingApp : public D3DApp
{
public:
	PickingApp(Win32::HINSTANCE hInstance)
		: D3DApp(hInstance)
	{
		Initialize();
	}

	PickingApp(const PickingApp& rhs) = delete;
	PickingApp& operator=(const PickingApp& rhs) = delete;
	~PickingApp()
	{
		if (md3dDevice != nullptr)
			FlushCommandQueue();
	}

private:
	void Initialize()override
	{
		D3DApp::Initialize();

		mCamera.SetPosition(0.0f, 2.0f, -15.0f);

		// Create the singleton.
		DirectX::GraphicsMemory::Get(md3dDevice.Get());

		// We will upload on the direct queue for the book samples, but 
		// copy queue would be better for real game.
		mUploadBatch->Begin(D3D12_COMMAND_LIST_TYPE_DIRECT);

		// Do init work that requires mUploadBatch...
		LoadTextures();
		LoadGeometry();

		// Kick off upload work asyncronously.
		auto result = std::future<void>{mUploadBatch->End(mCommandQueue.Get())};

		// Other init work.
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

	void CreateRtvAndDsvDescriptorHeaps()override
	{
		mRtvHeap.Init(md3dDevice.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_RTV, SwapChainBufferCount);
		mDsvHeap.Init(md3dDevice.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 2);
	}

	void OnResize()override
	{
		D3DApp::OnResize();
		mCamera.SetLens(0.25f * MathHelper::Pi, AspectRatio(), 1.0f, 1000.0f);
	}
	
	void Update(const GameTimer& gt)override
	{
		OnKeyboardInput(gt);

		// Cycle through the circular frame resource array.
		mCurrFrameResourceIndex = (mCurrFrameResourceIndex + 1) % gNumFrameResources;
		mCurrFrameResource = mFrameResources[mCurrFrameResourceIndex].get();

		// Has the GPU finished processing the commands of the current frame resource?
		// If not, wait until the GPU has completed commands up to this fence point.
		if (mCurrFrameResource->Fence != 0 && mFence->GetCompletedValue() < mCurrFrameResource->Fence)
		{
			auto event = Event{};
			ThrowIfFailed(mFence->SetEventOnCompletion(mCurrFrameResource->Fence, event.Get()));
			event.Wait();
		}

		//
		// Animate the lights (and hence shadows).
		//

		mLightRotationAngle += 0.1f * gt.DeltaTime();

		auto R = DirectX::XMMATRIX{ DirectX::XMMatrixRotationY(mLightRotationAngle) };
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
		auto& psoLib = PsoLib::GetLib();
		auto& cbvSrvUavHeap = CbvSrvUavHeap::Get();
		auto& samHeap = SamplerHeap::Get();

		UpdateImgui(gt);

		auto cmdListAlloc = mCurrFrameResource->CmdListAlloc;

		// Reuse the memory associated with command recording.
		// We can only reset when the associated command lists have finished execution on the GPU.
		ThrowIfFailed(cmdListAlloc->Reset());

		// A command list can be reset after it has been added to the command queue via ExecuteCommandList.
		// Reusing the command list reuses memory.
		ThrowIfFailed(mCommandList->Reset(cmdListAlloc.Get(), psoLib["opaque"]));

		auto descriptorHeaps = std::array{ cbvSrvUavHeap.GetD3dHeap(), samHeap.GetD3dHeap() };
		mCommandList->SetDescriptorHeaps(static_cast<std::uint32_t>(descriptorHeaps.size()), descriptorHeaps.data());

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
		mCommandList->ClearRenderTargetView(CurrentBackBufferView(), DirectX::Colors::LightSteelBlue, 0, nullptr);
		mCommandList->ClearDepthStencilView(DepthStencilView(), D3D12::D3D12_CLEAR_FLAGS{ D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL }, 1.0f, 0, 0, nullptr);

		// Specify the buffers we are going to render to.
		auto cbbv = CurrentBackBufferView();
		auto dsv = DepthStencilView();
		mCommandList->OMSetRenderTargets(1, &cbbv, true, &dsv);

		auto passCB = mCurrFrameResource->PassCB->Resource();
		mCommandList->SetGraphicsRootConstantBufferView(GFX_ROOT_ARG_PASS_CBV, passCB->GetGPUVirtualAddress());

		mCommandList->SetPipelineState(mDrawWireframe ? psoLib["opaque_wireframe"] : psoLib["opaque"]);
		DrawRenderItems(mCommandList.Get(), mRitemLayer[(int)RenderLayer::Opaque]);

		mCommandList->SetPipelineState(psoLib["highlight"]);
		DrawRenderItems(mCommandList.Get(), mRitemLayer[(int)RenderLayer::Highlight]);

		mCommandList->SetPipelineState(psoLib["debug"]);
		DrawRenderItems(mCommandList.Get(), mRitemLayer[(int)RenderLayer::Debug]);

		mCommandList->SetPipelineState(psoLib["sky"]);
		DrawRenderItems(mCommandList.Get(), mRitemLayer[(int)RenderLayer::Sky]);

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
		auto cmdsLists = std::array{ static_cast<ID3D12CommandList*>(mCommandList.Get()) };
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
		auto& io = ImGui::GetIO();
		if (io.WantCaptureMouse)
			return;

		if ((btnState & Win32::MK::LButton) != 0)
		{
			mLastMousePos.x = x;
			mLastMousePos.y = y;

			Win32::SetCapture(mhMainWnd);
		}
		else if ((btnState & Win32::MK::RButton) != 0)
		{
			Pick(x, y);
		}
	}

	void OnMouseUp(Win32::WPARAM btnState, int x, int y)override
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

			mCamera.Pitch(dy);
			mCamera.RotateY(dx);
		}
		mLastMousePos.x = x;
		mLastMousePos.y = y;
	}

	void OnKeyboardInput(const GameTimer& gt)
	{
		const auto dt = gt.DeltaTime();
		if (Win32::GetAsyncKeyState('W') & 0x8000)
			mCamera.Walk(10.0f * dt);
		if (Win32::GetAsyncKeyState('S') & 0x8000)
			mCamera.Walk(-10.0f * dt);
		if (Win32::GetAsyncKeyState('A') & 0x8000)
			mCamera.Strafe(-10.0f * dt);
		if (Win32::GetAsyncKeyState('D') & 0x8000)
			mCamera.Strafe(10.0f * dt);
		mCamera.UpdateViewMatrix();
	}

	void AnimateMaterials(const GameTimer& gt)
	{
	}

	void UpdatePerObjectCB(const GameTimer& gt)
	{
		// Update per object constants once per frame so the data can be shared across different render passes.
		for (auto& ri : mAllRitems)
		{
			DirectX::XMStoreFloat4x4(&ri->ObjectConstants.gWorld, DirectX::XMMatrixTranspose(DirectX::XMLoadFloat4x4(&ri->World)));
			DirectX::XMStoreFloat4x4(&ri->ObjectConstants.gTexTransform, DirectX::XMMatrixTranspose(DirectX::XMLoadFloat4x4(&ri->TexTransform)));
			ri->ObjectConstants.gMaterialIndex = ri->Mat->MatIndex;
			ri->ObjectConstants.gCubeMapIndex = mSkyBindlessIndex;
			ri->ObjectConstants.gMiscUint4 = DirectX::XMUINT4(0, 0, 0, 0);
			ri->ObjectConstants.gMiscFloat4 = DirectX::XMFLOAT4(0.0f, 0.0f, 0.0f, 0.0f);
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
			// Only update the cbuffer data if the constants have changed.  If the cbuffer
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
				.GlossHeightAoMapIndex = static_cast<std::uint32_t>(mat->GlossHeightAoBindlessIndex),
			};
			
			DirectX::XMStoreFloat4x4(&matData.MatTransform, DirectX::XMMatrixTranspose(matTransform));

			currMaterialBuffer->CopyData(mat->MatIndex, matData);

			// Next FrameResource need to be updated too.
			mat->NumFramesDirty--;
		}
	}

	void UpdateMainPassCB(const GameTimer& gt)
	{
		auto view = DirectX::XMMATRIX{ mCamera.GetView() };
		auto proj = DirectX::XMMATRIX{ mCamera.GetProj() };

		auto viewProj = DirectX::XMMATRIX{ DirectX::XMMatrixMultiply(view, proj) };
		auto detView = DirectX::XMMatrixDeterminant(view);
		auto invView = DirectX::XMMATRIX{ DirectX::XMMatrixInverse(&detView, view) };
		auto detProj = DirectX::XMMatrixDeterminant(proj);
		auto invProj = DirectX::XMMATRIX{ DirectX::XMMatrixInverse(&detProj, proj) };
		auto detViewProj = DirectX::XMMatrixDeterminant(viewProj);
		auto invViewProj = DirectX::XMMATRIX{ DirectX::XMMatrixInverse(&detViewProj, viewProj) };

		DirectX::XMStoreFloat4x4(&mMainPassCB.gView, DirectX::XMMatrixTranspose(view));
		DirectX::XMStoreFloat4x4(&mMainPassCB.gInvView, DirectX::XMMatrixTranspose(invView));
		DirectX::XMStoreFloat4x4(&mMainPassCB.gProj, DirectX::XMMatrixTranspose(proj));
		DirectX::XMStoreFloat4x4(&mMainPassCB.gInvProj, DirectX::XMMatrixTranspose(invProj));
		DirectX::XMStoreFloat4x4(&mMainPassCB.gViewProj, DirectX::XMMatrixTranspose(viewProj));
		DirectX::XMStoreFloat4x4(&mMainPassCB.gInvViewProj, DirectX::XMMatrixTranspose(invViewProj));
		mMainPassCB.gEyePosW = mCamera.GetPosition3f();
		mMainPassCB.gRenderTargetSize = DirectX::XMFLOAT2((float)mClientWidth, (float)mClientHeight);
		mMainPassCB.gInvRenderTargetSize = DirectX::XMFLOAT2(1.0f / mClientWidth, 1.0f / mClientHeight);
		mMainPassCB.gNearZ = 1.0f;
		mMainPassCB.gFarZ = 1000.0f;
		mMainPassCB.gTotalTime = gt.TotalTime();
		mMainPassCB.gDeltaTime = gt.DeltaTime();
		mMainPassCB.gAmbientLight = { 0.25f, 0.25f, 0.35f, 1.0f };
		mMainPassCB.gSkyBoxIndex = mSkyBindlessIndex;

		mMainPassCB.gNormalMapsEnabled = mNormalMapsEnabled;
		mMainPassCB.gReflectionsEnabled = mReflectionsEnabled;
		mMainPassCB.gShadowsEnabled = false;
		mMainPassCB.gSsaoEnabled = false;

		mMainPassCB.gNumDirLights = 3;
		mMainPassCB.gNumPointLights = 0;
		mMainPassCB.gNumSpotLights = 0;
		mMainPassCB.gLights[0].Direction = mRotatedLightDirections[0];
		mMainPassCB.gLights[0].Strength = { 0.9f, 0.8f, 0.7f };
		mMainPassCB.gLights[1].Direction = mRotatedLightDirections[1];
		mMainPassCB.gLights[1].Strength = { 0.4f, 0.4f, 0.4f };
		mMainPassCB.gLights[2].Direction = mRotatedLightDirections[2];
		mMainPassCB.gLights[2].Strength = { 0.2f, 0.2f, 0.2f };

		auto currPassCB = mCurrFrameResource->PassCB.get();
		currPassCB->CopyData(0, mMainPassCB);
	}

	void LoadTextures()
	{
		auto& texLib = TextureLib::GetLib();
		texLib.Init(md3dDevice.Get(), *mUploadBatch.get());
	}

	void LoadGeometry()
	{
		auto shapeGeo = std::unique_ptr<MeshGeometry>{d3dUtil::BuildShapeGeometry(md3dDevice.Get(), *mUploadBatch.get())};
		mGeometries[shapeGeo->Name] = std::move(shapeGeo);

		auto skullGeo = std::unique_ptr<MeshGeometry>{d3dUtil::BuildSkullGeometry(md3dDevice.Get(), *mUploadBatch.get())};
		mGeometries[skullGeo->Name] = std::move(skullGeo);

		auto columnSquare = std::unique_ptr<MeshGeometry>{d3dUtil::LoadSimpleModelGeometry(md3dDevice.Get(), *mUploadBatch.get(), "Models/columnSquare.m3d", "columnSquare")};
		mGeometries[columnSquare->Name] = std::move(columnSquare);

		auto columnSquareBroken = std::unique_ptr<MeshGeometry>{d3dUtil::LoadSimpleModelGeometry(md3dDevice.Get(), *mUploadBatch.get(), "Models/columnSquareBroken.m3d", "columnSquareBroken")};
		mGeometries[columnSquareBroken->Name] = std::move(columnSquareBroken);

		auto columnRound = std::unique_ptr<MeshGeometry>{d3dUtil::LoadSimpleModelGeometry(md3dDevice.Get(), *mUploadBatch.get(), "Models/columnRound.m3d", "columnRound")};
		mGeometries[columnRound->Name] = std::move(columnRound);

		auto columnRoundBroken = std::unique_ptr<MeshGeometry>{d3dUtil::LoadSimpleModelGeometry(md3dDevice.Get(), *mUploadBatch.get(), "Models/columnRoundBroken.m3d", "columnRoundBroken")};
		mGeometries[columnRoundBroken->Name] = std::move(columnRoundBroken);
		
		auto orbBase = std::unique_ptr<MeshGeometry>{d3dUtil::LoadSimpleModelGeometry(md3dDevice.Get(), *mUploadBatch.get(), "Models/orbBase.m3d", "orbBase")};
		mGeometries[orbBase->Name] = std::move(orbBase);
	}

	void BuildRootSignature()
	{
		// Root parameter can be a table, root descriptor or root constants.
		auto gfxRootParameters = std::array<D3D12::CD3DX12_ROOT_PARAMETER, GFX_ROOT_ARG_COUNT>{};

		// Perfomance TIP: Order from most frequent to least frequent.
		gfxRootParameters[GFX_ROOT_ARG_OBJECT_CBV].InitAsConstantBufferView(0);
		gfxRootParameters[GFX_ROOT_ARG_PASS_CBV].InitAsConstantBufferView(1);
		gfxRootParameters[GFX_ROOT_ARG_SKINNED_CBV].InitAsConstantBufferView(2);
		gfxRootParameters[GFX_ROOT_ARG_MATERIAL_SRV].InitAsShaderResourceView(0);
		gfxRootParameters[GFX_ROOT_ARG_INSTANCEDATA_SRV].InitAsShaderResourceView(1);

		auto gfxRootSigDesc = D3D12::CD3DX12_ROOT_SIGNATURE_DESC{
			GFX_ROOT_ARG_COUNT,
			gfxRootParameters.data(),
			0, nullptr, // static samplers
			D3D12::D3D12_ROOT_SIGNATURE_FLAGS{
				D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT |
				D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED |
				D3D12_ROOT_SIGNATURE_FLAG_SAMPLER_HEAP_DIRECTLY_INDEXED
			}
		};

		// create a root signature with a single slot which points to a descriptor range consisting of a single constant buffer
		auto serializedRootSig = Microsoft::WRL::ComPtr<D3D::ID3DBlob>{};
		auto errorBlob = Microsoft::WRL::ComPtr<D3D::ID3DBlob>{};
		auto hr = D3D12::D3D12SerializeRootSignature(&gfxRootSigDesc, 
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
			__uuidof(D3D12::ID3D12RootSignature), 
			reinterpret_cast<void**>(mRootSignature.GetAddressOf())));

		// Root parameter can be a table, root descriptor or root constants.
		auto computeRootParameters = std::array<D3D12::CD3DX12_ROOT_PARAMETER, COMPUTE_ROOT_ARG_COUNT>{};

		// Perfomance TIP: Order from most frequent to least frequent.
		computeRootParameters[COMPUTE_ROOT_ARG_DISPATCH_CBV].InitAsConstantBufferView(0);
		computeRootParameters[COMPUTE_ROOT_ARG_PASS_CBV].InitAsConstantBufferView(1);
		computeRootParameters[COMPUTE_ROOT_ARG_PASS_EXTRA_CBV].InitAsConstantBufferView(2);

		// A root signature is an array of root parameters.
		auto computeRootSigDesc = D3D12::CD3DX12_ROOT_SIGNATURE_DESC{
			COMPUTE_ROOT_ARG_COUNT,
			computeRootParameters.data(),
			0, 
			nullptr, // static samplers
			D3D12::D3D12_ROOT_SIGNATURE_FLAGS{
				D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED |
				D3D12_ROOT_SIGNATURE_FLAG_SAMPLER_HEAP_DIRECTLY_INDEXED
			}
		};

		hr = D3D12SerializeRootSignature(
			&computeRootSigDesc, 
			D3D::D3D_ROOT_SIGNATURE_VERSION::D3D_ROOT_SIGNATURE_VERSION_1,
			serializedRootSig.GetAddressOf(), 
			errorBlob.GetAddressOf());

		if (errorBlob != nullptr)
			Win32::OutputDebugStringA((char*)errorBlob->GetBufferPointer());
		ThrowIfFailed(hr);

		ThrowIfFailed(md3dDevice->CreateRootSignature(0,
			serializedRootSig->GetBufferPointer(),
			serializedRootSig->GetBufferSize(),
			__uuidof(D3D12::ID3D12RootSignature), 
			reinterpret_cast<void**>(mComputeRootSignature.GetAddressOf())));
	}

	void BuildCbvSrvUavDescriptorHeap()
	{
		auto& cbvSrvUavHeap = CbvSrvUavHeap::Get();
		cbvSrvUavHeap.Init(md3dDevice.Get(), CBV_SRV_UAV_HEAP_CAPACITY);

		//
		// Fill out the heap with actual descriptors.
		//

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

		mRandomTexBindlessIndex = texLib["randomTex1024"]->BindlessIndex;
		mSkyBindlessIndex = texLib["skyCubeMap"]->BindlessIndex;
	}
	
	void BuildShadersAndInputLayout()
	{
		ShaderLib::GetLib().Init(md3dDevice.Get());
	}

	void BuildPSOs()
	{
		PsoLib::GetLib().Init(
			md3dDevice.Get(),
			mBackBufferFormat,
			mDepthStencilFormat,
			SsaoAmbientMapFormat,
			SceneNormalMapFormat,
			mRootSignature.Get());
	}

	void BuildFrameResources()
	{
		constexpr auto passCount = 1u;
		for (int i = 0; i < gNumFrameResources; ++i)
			mFrameResources.push_back(std::make_unique<FrameResource>(md3dDevice.Get(), passCount, (UINT)mAllRitems.size(), MaterialLib::GetLib().GetMaterialCount()));
	}

	void BuildMaterials()
	{
		MaterialLib::GetLib().Init(md3dDevice.Get());
	}

	void AddRenderItem(
		RenderLayer layer, 
		const DirectX::XMFLOAT4X4& world, 
		const DirectX::XMFLOAT4X4& texTransform, 
		Material* mat, 
		MeshGeometry* geo, 
		SubmeshGeometry& drawArgs
	)
	{
		auto ritem = std::make_unique<RenderItem>();
		ritem->Visible = true;
		ritem->World = world;
		ritem->TexTransform = texTransform;
		ritem->Mat = mat;
		ritem->Geo = geo;
		ritem->PrimitiveType = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
		ritem->IndexCount = drawArgs.IndexCount;
		ritem->StartIndexLocation = drawArgs.StartIndexLocation;
		ritem->BaseVertexLocation = drawArgs.BaseVertexLocation;
		ritem->Bounds = drawArgs.Bounds;

		mRitemLayer[(int)layer].push_back(ritem.get());
		mAllRitems.push_back(std::move(ritem));
	}

	void BuildRenderItems()
	{
		auto& matLib = MaterialLib::GetLib();

		auto worldTransform = DirectX::XMFLOAT4X4{};
		auto texTransform = DirectX::XMFLOAT4X4{};

		DirectX::XMStoreFloat4x4(&worldTransform, DirectX::XMMatrixScaling(6.0f, 6.0f, 6.0f) * DirectX::XMMatrixTranslation(0.0f, 0.0f, 0.0f));
		DirectX::XMStoreFloat4x4(&texTransform, DirectX::XMMatrixScaling(1.0f, 1.0f, 1.0f));
		AddRenderItem(RenderLayer::Opaque, worldTransform, texTransform, matLib["orbBase"], mGeometries["orbBase"].get(), mGeometries["orbBase"]->DrawArgs["subset0"]);

		DirectX::XMStoreFloat4x4(&worldTransform, DirectX::XMMatrixScaling(2.0f, 2.0f, 2.0f) * DirectX::XMMatrixTranslation(0.0f, 1.75f, 0.0f));
		texTransform = MathHelper::Identity4x4;
		AddRenderItem(RenderLayer::Opaque, worldTransform, texTransform, matLib["mirror1"], mGeometries["shapeGeo"].get(), mGeometries["shapeGeo"]->DrawArgs["sphere"]);

		worldTransform = MathHelper::Identity4x4;
		DirectX::XMStoreFloat4x4(&texTransform, DirectX::XMMatrixScaling(6.0f, 6.0f, 1.0f));
		AddRenderItem(RenderLayer::Opaque, worldTransform, texTransform, matLib["stoneFloor"], mGeometries["shapeGeo"].get(), mGeometries["shapeGeo"]->DrawArgs["grid"]);

		auto falledColumnTransform0 = DirectX::XMMATRIX{
			DirectX::XMMatrixRotationZ(-0.54f * DirectX::Pi)*
			DirectX::XMMatrixRotationY(0.15f * DirectX::Pi)*
			DirectX::XMMatrixScaling(1.0f, 1.0f, 1.0f)*
			DirectX::XMMatrixTranslation(-3.0f, 0.35f, 3.0f)
		};
			
		DirectX::XMStoreFloat4x4(&worldTransform, falledColumnTransform0);
		texTransform = MathHelper::Identity4x4;
		AddRenderItem(RenderLayer::Opaque, worldTransform, texTransform, matLib["columnRound"], mGeometries["columnRoundBroken"].get(), mGeometries["columnRoundBroken"]->DrawArgs["subset0"]);

		auto falledColumnTransform1 = DirectX::XMMATRIX{
			DirectX::XMMatrixRotationZ(-0.54f * DirectX::Pi) *
			DirectX::XMMatrixRotationY(0.75f * DirectX::Pi) *
			DirectX::XMMatrixScaling(1.0f, 1.0f, 1.0f) *
			DirectX::XMMatrixTranslation(1.5f, 0.35f, -4.0f)
		};
		DirectX::XMStoreFloat4x4(&worldTransform, falledColumnTransform1);
		texTransform = MathHelper::Identity4x4;
		AddRenderItem(RenderLayer::Opaque, worldTransform, texTransform, matLib["columnRound"], mGeometries["columnRoundBroken"].get(), mGeometries["columnRoundBroken"]->DrawArgs["subset0"]);


		for (int i = 0; i < 5; ++i)
		{
			auto isLeftColumnBroken = (i == 2);
			auto isRightColumnBroken = (i == 0 or i == 4);

			auto columnNameLeft = std::string{ isLeftColumnBroken ? "columnSquareBroken" : "columnSquare" };
			auto columnNameRight = std::string{ isRightColumnBroken ? "columnSquareBroken" : "columnSquare" };

			DirectX::XMStoreFloat4x4(&texTransform, DirectX::XMMatrixScaling(1.0f, 1.0f, 1.0f));
			DirectX::XMStoreFloat4x4(&worldTransform, DirectX::XMMatrixTranslation(-5.0f, 0.0f, -10.0f + i * 5.0f));
			AddRenderItem(RenderLayer::Opaque, worldTransform, texTransform, matLib["columnSquare"], mGeometries[columnNameLeft].get(), mGeometries[columnNameLeft]->DrawArgs["subset0"]);

			DirectX::XMStoreFloat4x4(&texTransform, DirectX::XMMatrixScaling(1.0f, 1.0f, 1.0f));
			DirectX::XMStoreFloat4x4(&worldTransform, DirectX::XMMatrixTranslation(+5.0f, 0.0f, -10.0f + i * 5.0f));
			AddRenderItem(RenderLayer::Opaque, worldTransform, texTransform, matLib["columnSquare"], mGeometries[columnNameRight].get(), mGeometries[columnNameRight]->DrawArgs["subset0"]);

			if (not isLeftColumnBroken)
			{
				texTransform = MathHelper::Identity4x4;
				DirectX::XMStoreFloat4x4(&worldTransform, DirectX::XMMatrixTranslation(-5.0f, 4.0f, -10.0f + i * 5.0f));
				AddRenderItem(RenderLayer::Opaque, worldTransform, texTransform, matLib["mirror0"], mGeometries["shapeGeo"].get(), mGeometries["shapeGeo"]->DrawArgs["sphere"]);
			}
			if (not isRightColumnBroken)
			{
				texTransform = MathHelper::Identity4x4;
				DirectX::XMStoreFloat4x4(&worldTransform, DirectX::XMMatrixTranslation(+5.0f, 4.0f, -10.0f + i * 5.0f));
				AddRenderItem(RenderLayer::Opaque, worldTransform, texTransform, matLib["mirror0"], mGeometries["shapeGeo"].get(), mGeometries["shapeGeo"]->DrawArgs["sphere"]);
			}
		}


		// Add a render item for drawing the selected triangle. 
		// Its properties will be filled out when something is selected.
		auto pickedRitem = std::make_unique<RenderItem>();
		pickedRitem->Mat = matLib["highlight0"];
		pickedRitem->Visible = false;

		mRitemLayer[(int)RenderLayer::Highlight].push_back(pickedRitem.get());
		mAllRitems.push_back(std::move(pickedRitem));

		mPickedRitem = mAllRitems.back().get();
	}

	void DrawRenderItems(D3D12::ID3D12GraphicsCommandList* cmdList, const std::vector<RenderItem*>& ritems)
	{
		for (auto i = 0ull; i < ritems.size(); ++i)
		{
			auto ri = ritems[i];
			if (not ri->Visible)
				continue;

			auto vbv = ri->Geo->VertexBufferView();
			auto ibv = ri->Geo->IndexBufferView();
			cmdList->IASetVertexBuffers(0, 1, &vbv);
			cmdList->IASetIndexBuffer(&ibv);
			cmdList->IASetPrimitiveTopology(ri->PrimitiveType);
			cmdList->SetGraphicsRootConstantBufferView(GFX_ROOT_ARG_OBJECT_CBV, ri->MemHandleToObjectCB.GpuAddress());
			cmdList->DrawIndexedInstanced(ri->IndexCount, 1, ri->StartIndexLocation, ri->BaseVertexLocation, 0);
		}
	}

	void Pick(int sx, int sy)
	{
		auto P = DirectX::XMFLOAT4X4{mCamera.GetProj4x4f()};

		// Compute picking ray in view space.
		auto vx = (+2.0f * sx / mClientWidth - 1.0f) / P(0, 0);
		auto vy = (-2.0f * sy / mClientHeight + 1.0f) / P(1, 1);

		// Ray definition in view space.
		auto rayOriginV = DirectX::XMVECTOR{DirectX::XMVectorSet(0.0f, 0.0f, 0.0f, 1.0f)};
		auto rayDirV = DirectX::XMVECTOR{DirectX::XMVectorSet(vx, vy, 1.0f, 0.0f)};

		auto V = DirectX::XMMATRIX{mCamera.GetView()};
		auto detV = DirectX::XMVECTOR{DirectX::XMMatrixDeterminant(V)};
		auto invView = DirectX::XMMATRIX{DirectX::XMMatrixInverse(&detV, V)};

		// Assume nothing is picked to start, so the picked render-item is invisible.
		mPickedRitem->Visible = false;

		auto minWorldDist = MathHelper::Infinity;

		// Check if we picked an opaque render item.  A real app might keep a separate "picking list"
		// of objects that can be selected.   
		for (auto ri : mRitemLayer[(int)RenderLayer::Opaque])
		{
			auto geo = ri->Geo;

			// Skip invisible render-items.
			if (ri->Visible == false)
				continue;

			auto W = DirectX::XMMATRIX{DirectX::XMLoadFloat4x4(&ri->World)};
			auto detW = DirectX::XMVECTOR{ DirectX::XMMatrixDeterminant(W) };
			auto invWorld = DirectX::XMMATRIX{ DirectX::XMMatrixInverse(&detW, W) };

			// Tranform ray from view space to the local space of the mesh.
			auto toLocal = DirectX::XMMATRIX{DirectX::XMMatrixMultiply(invView, invWorld)};

			auto rayOriginL = DirectX::XMVECTOR{DirectX::XMVector3TransformCoord(rayOriginV, toLocal)};
			auto rayDirL = DirectX::XMVECTOR{DirectX::XMVector3TransformNormal(rayDirV, toLocal)};

			// Make the ray direction unit length for the intersection tests.
			rayDirL = DirectX::XMVector3Normalize(rayDirL);

			// If we did not hit the bounding box, then it is impossible that we hit 
			// the Mesh, so do not waste effort doing ray/triangle tests.
			auto boxDist = 0.0f;
			if (not ri->Bounds.Intersects(rayOriginL, rayDirL, boxDist))
				continue;

			// NOTE: For the demo, we know what to cast the vertex/index data to (we force 16-bit indices). 
			// If we were mixing formats, some metadata would be needed to figure out what to cast it to.
			auto vertices = reinterpret_cast<ModelVertex*>(geo->VertexBufferCPU.data());
			auto indices = reinterpret_cast<std::uint16_t*>(geo->IndexBufferCPU.data());

			auto triCount = ri->IndexCount / 3;

			// Find the nearest ray/triangle intersection.
			for (auto i = 0u; i < triCount; ++i)
			{
				// Indices for this triangle.
				auto i0 = std::uint32_t(ri->BaseVertexLocation + indices[ri->StartIndexLocation + i * 3 + 0]);
				auto i1 = std::uint32_t(ri->BaseVertexLocation + indices[ri->StartIndexLocation + i * 3 + 1]);
				auto i2 = std::uint32_t(ri->BaseVertexLocation + indices[ri->StartIndexLocation + i * 3 + 2]);

				// Vertices for this triangle.
				auto v0 = DirectX::XMVECTOR{DirectX::XMLoadFloat3(&vertices[i0].Pos)};
				auto v1 = DirectX::XMVECTOR{DirectX::XMLoadFloat3(&vertices[i1].Pos)};
				auto v2 = DirectX::XMVECTOR{DirectX::XMLoadFloat3(&vertices[i2].Pos)};

				// We have to iterate over all the triangles of all the objects whose bounds
				// we hit in order to find the nearest intersection.
				float t = std::numeric_limits<float>::max();
				if (not DirectX::TriangleTests::Intersects(rayOriginL, rayDirL, v0, v1, v2, t))
					continue;

				auto hitPosL = DirectX::XMVECTOR{rayOriginL + t * rayDirL};
				auto hitPosW = DirectX::XMVECTOR{DirectX::XMVector3TransformCoord(hitPosL, W)};

				// Take hit position closest to the camera. Do this in world space, as 
				// we want the nearest hit across all objects in the scene, not just
				// the current mesh.
				auto t_world = DirectX::XMVectorGetX(DirectX::XMVector3Length(hitPosW - mCamera.GetPosition()));

				if (t_world >= minWorldDist)
					continue;

				// This is the new nearest picked triangle.
				minWorldDist = t_world;
				auto pickedTriangle = i;

				mPickedRitem->Visible = true;

				// Propagate properties from selected geometry.
				mPickedRitem->BaseVertexLocation = ri->BaseVertexLocation;
				mPickedRitem->World = ri->World;
				mPickedRitem->TexTransform = ri->TexTransform;
				mPickedRitem->Geo = ri->Geo;
				mPickedRitem->PrimitiveType = ri->PrimitiveType;

				// Offset to the picked triangle in the mesh index buffer.
				mPickedRitem->IndexCount = 3;
				mPickedRitem->StartIndexLocation = ri->StartIndexLocation + 3 * pickedTriangle;
			}
		}
	}

private:

	std::vector<std::unique_ptr<FrameResource>> mFrameResources;
	FrameResource* mCurrFrameResource = nullptr;
	int mCurrFrameResourceIndex = 0;

	Microsoft::WRL::ComPtr<ID3D12RootSignature> mRootSignature;
	Microsoft::WRL::ComPtr<ID3D12RootSignature> mComputeRootSignature;

	std::unordered_map<std::string, std::unique_ptr<MeshGeometry>> mGeometries;

	std::vector<D3D12::D3D12_INPUT_ELEMENT_DESC> mInputLayout;

	// List of all the render items.
	std::vector<std::unique_ptr<RenderItem>> mAllRitems;

	// Render items divided by PSO.
	std::vector<RenderItem*> mRitemLayer[(int)RenderLayer::Count];

	// Cache a pointer to the render-item of the picked
	// triangle in the PickingApp class.
	RenderItem* mPickedRitem = nullptr;

	std::uint32_t mRandomTexBindlessIndex = -1;
	std::uint32_t mSkyBindlessIndex = -1;

	std::uint32_t mNullCubeSrvIndex = 0;
	std::uint32_t mNullTexSrvIndex = 0;

	CD3DX12_GPU_DESCRIPTOR_HANDLE mNullSrv;

	PerPassCB mMainPassCB;  // index 0 of pass cbuffer.

	Camera mCamera;

	float mLightNearZ = 0.0f;
	float mLightFarZ = 0.0f;
	DirectX::XMFLOAT3 mLightPosW;
	DirectX::XMFLOAT4X4 mLightView = MathHelper::Identity4x4;
	DirectX::XMFLOAT4X4 mLightProj = MathHelper::Identity4x4;

	float mLightRotationAngle = 0.0f;
	DirectX::XMFLOAT3 mBaseLightDirections[3] = {
		DirectX::XMFLOAT3(0.57735f, -0.57735f, 0.57735f),
		DirectX::XMFLOAT3(-0.57735f, -0.57735f, 0.57735f),
		DirectX::XMFLOAT3(0.0f, -0.707f, -0.707f)
	};
	DirectX::XMFLOAT3 mRotatedLightDirections[3];

	bool mDrawWireframe = false;
	bool mNormalMapsEnabled = false;
	bool mReflectionsEnabled = false;

	Win32::POINT mLastMousePos;
};
