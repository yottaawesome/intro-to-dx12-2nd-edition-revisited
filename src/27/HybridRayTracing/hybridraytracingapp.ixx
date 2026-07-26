export module hybridraytracing:hybridraytracingapp;
import std;
import shared;
import :hybridraytracer;

// Lightweight structure stores parameters to draw a shape.  This will
// vary from app-to-app.
struct RenderItem
{
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
    D3D12::D3D12_PRIMITIVE_TOPOLOGY PrimitiveType = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;

    // DrawIndexedInstanced parameters.
    std::uint32_t IndexCount = 0;
    std::uint32_t StartIndexLocation = 0;
    int BaseVertexLocation = 0;
};

enum class RenderLayer : int
{
    Opaque = 0,
    Debug,
    Sky,
    Count
};

//
// Define named offsets into descriptor heaps for readability.
enum RtvOffsets
{
    // Start after swapchain buffers.
    RTV_NORMALMAP = D3DApp::SwapChainBufferCount,
    RTV_COUNT
};

enum DsvOffsets
{
    DSV_MAINVIEW = 0,
    DSV_SHADOWMAP,
};

constexpr auto CBV_SRV_UAV_HEAP_CAPACITY = 16384u;

export class HybridRayTracingApp : public D3DApp
{
public:
    HybridRayTracingApp(Win32::HINSTANCE hInstance)
        : D3DApp(hInstance)
    {
        // Estimate the scene bounding sphere manually since we know how the scene was constructed.
        // The grid is the "widest object" with a width of 20 and depth of 30.0f, and centered at
        // the world space origin.  In general, you need to loop over every world space vertex
        // position and compute the bounding sphere.
        mSceneBounds.Center = DirectX::XMFLOAT3(0.0f, 0.0f, 0.0f);
        mSceneBounds.Radius = std::sqrtf(10.0f * 10.0f + 15.0f * 15.0f);
		Initialize();
    }

    ~HybridRayTracingApp()
    {
        if (md3dDevice != nullptr)
            FlushCommandQueue();
    }

    HybridRayTracingApp(const HybridRayTracingApp&) = delete;
    HybridRayTracingApp& operator=(const HybridRayTracingApp&) = delete;

private:
    void Initialize()override
    {
        D3DApp::Initialize();

        mCamera.SetPosition(0.0f, 2.0f, -15.0f);

        mPrepass = std::make_unique<Prepass>(md3dDevice.Get());

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
        BuildShaders();
        BuildMaterials();
        InitRayTracing();
        BuildRenderItems();
        BuildFrameResources();
        BuildPSOs();

        // Block until the upload work is complete.
        result.wait();

        // Build ray tracing structs on GPU and wait for it to be done. 
        // In a large app where it might take a while to build, we could 
        // refactor to do other async work here while waiting for build.
        mCommandList->Reset(mDirectCmdListAlloc.Get(), nullptr);
        mRayTracer->ExecuteBuildAccelerationStructureCommands(mCommandQueue.Get());
    }

    void CreateRtvAndDsvDescriptorHeaps()override
    {
        mRtvHeap.Init(md3dDevice.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_RTV, RTV_COUNT);
        mDsvHeap.Init(md3dDevice.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 2);
    }

    void OnResize()override
    {
        D3DApp::OnResize();
        mCamera.SetLens(0.25f * MathHelper::Pi, AspectRatio(), 1.0f, 1000.0f);
        if (mPrepass != nullptr)
            mPrepass->OnResize(mClientWidth, mClientHeight, mDepthStencilBuffer.Get());
        if (mRayTracer != nullptr)
            mRayTracer->OnResize(mClientWidth, mClientHeight);
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

        auto R = DirectX::XMMATRIX{DirectX::XMMatrixRotationY(mLightRotationAngle)};
        for (int i = 0; i < 3; ++i)
        {
            auto lightDir = DirectX::XMVECTOR{DirectX::XMLoadFloat3(&mBaseLightDirections[i])};
            lightDir = DirectX::XMVector3TransformNormal(lightDir, R);
            DirectX::XMStoreFloat3(&mRotatedLightDirections[i], lightDir);
        }

        AnimateMaterials(gt);
        UpdatePerObjectCB(gt);
        UpdateMaterialBuffer(gt);
        UpdateShadowTransform(gt);
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

        //
        // Normal/depth pass.
        //

        DrawNormalsAndDepth();

        //
        // Compute hybrid ray tracing.
        // 

        auto passCB = mCurrFrameResource->PassCB->Resource();
        if (mRayTracer != nullptr)
            mRayTracer->Draw(passCB, matBuffer);

        //
        // Main rendering pass.
        //

        mCommandList->RSSetViewports(1, &mScreenViewport);
        mCommandList->RSSetScissorRects(1, &mScissorRect);

        // Indicate a state transition on the resource usage.
		auto transition = D3D12::CD3DX12_RESOURCE_BARRIER::Transition(CurrentBackBuffer(),
			D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
        mCommandList->ResourceBarrier(1, &transition);

        // Clear the back buffer and depth buffer.
        mCommandList->ClearRenderTargetView(CurrentBackBufferView(), DirectX::Colors::LightSteelBlue, 0, nullptr);

        // WE ALREADY WROTE THE DEPTH INFO TO THE DEPTH BUFFER IN DrawNormalsAndDepth,
        // SO DO NOT CLEAR DEPTH.

        // Specify the buffers we are going to render to.
		auto cbbv = CurrentBackBufferView();
		auto dsv = DepthStencilView();
        mCommandList->OMSetRenderTargets(1, &cbbv, true, &dsv);

        mCommandList->SetGraphicsRootConstantBufferView(GFX_ROOT_ARG_PASS_CBV, passCB->GetGPUVirtualAddress());

        mCommandList->SetPipelineState(psoLib["opaque_hybrid_rt"]);
        DrawRenderItems(mCommandList.Get(), mRitemLayer[(int)RenderLayer::Opaque]);

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

        ImGui::Checkbox("NormalMaps", &mNormalMapsEnabled);
        ImGui::Checkbox("Reflections", &mReflectionsEnabled);
        ImGui::Checkbox("Shadows", &mShadowsEnabled);
        ImGui::SliderFloat("LightAngle", &mLightRotationAngle, 0.0f, 2.0f * DirectX::Pi);

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
        if (auto& io = ImGui::GetIO(); not io.WantCaptureMouse)
        {
            mLastMousePos.x = x;
            mLastMousePos.y = y;
            Win32::SetCapture(mhMainWnd);
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

    void AnimateMaterials(const GameTimer& gt) {}

    void UpdatePerObjectCB(const GameTimer& gt)
    {
        // Update per object constants once per frame so the data can be shared across different render passes.
        for (auto& ri : mAllRitems)
        {
            DirectX::XMStoreFloat4x4(&ri->ObjectConstants.gWorld, DirectX::XMMatrixTranspose(DirectX::XMLoadFloat4x4(&ri->World)));
            DirectX::XMStoreFloat4x4(&ri->ObjectConstants.gTexTransform, DirectX::XMMatrixTranspose(DirectX::XMLoadFloat4x4(&ri->TexTransform)));
            ri->ObjectConstants.gMaterialIndex = ri->Mat->MatIndex;
            ri->ObjectConstants.gCubeMapIndex = mSkyBindlessIndex;

            // Need to hold handle until we submit work to GPU.
            ri->MemHandleToObjectCB = mLinearAllocator->AllocateConstant(ri->ObjectConstants);
        }
    }

    void UpdateMaterialBuffer(const GameTimer& gt)
    {
        auto currMaterialBuffer = mCurrFrameResource->MaterialBuffer.get();

        auto& matLib = MaterialLib::GetLib();
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
                .TransparencyWeight = mat->TransparencyWeight,
                .IndexOfRefraction = mat->IndexOfRefraction,
            };

            XMStoreFloat4x4(&matData.MatTransform, XMMatrixTranspose(matTransform));
            currMaterialBuffer->CopyData(mat->MatIndex, matData);

            // Next FrameResource need to be updated too.
            mat->NumFramesDirty--;
        }
    }

    void UpdateShadowTransform(const GameTimer& gt)
    {
        // Only the first "main" light casts a shadow.
        auto lightDir = DirectX::XMVECTOR{DirectX::XMLoadFloat3(&mRotatedLightDirections[0])};
        auto lightPos = DirectX::XMVECTOR{ -2.0f * mSceneBounds.Radius * lightDir };
        auto targetPos = DirectX::XMVECTOR{ DirectX::XMLoadFloat3(&mSceneBounds.Center) };
        auto lightUp = DirectX::XMVECTOR{DirectX::XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f)};
        auto lightView = DirectX::XMMATRIX{DirectX::XMMatrixLookAtLH(lightPos, targetPos, lightUp)};

        DirectX::XMStoreFloat3(&mLightPosW, lightPos);

        // Transform bounding sphere to light space.
        auto sphereCenterLS = DirectX::XMFLOAT3{};
        DirectX::XMStoreFloat3(&sphereCenterLS, DirectX::XMVector3TransformCoord(targetPos, lightView));

        // Ortho frustum in light space encloses scene.
        auto l = float{sphereCenterLS.x - mSceneBounds.Radius};
        auto b = float{sphereCenterLS.y - mSceneBounds.Radius};
        auto n = float{sphereCenterLS.z - mSceneBounds.Radius};
        auto r = float{sphereCenterLS.x + mSceneBounds.Radius};
        auto t = float{sphereCenterLS.y + mSceneBounds.Radius};
        auto f = float{sphereCenterLS.z + mSceneBounds.Radius};

        mLightNearZ = n;
        mLightFarZ = f;
        auto lightProj = DirectX::XMMATRIX{DirectX::XMMatrixOrthographicOffCenterLH(l, r, b, t, n, f)};

        // Transform NDC space [-1,+1]^2 to texture space [0,1]^2
        auto T = DirectX::XMMATRIX{
            0.5f, 0.0f, 0.0f, 0.0f,
            0.0f, -0.5f, 0.0f, 0.0f,
            0.0f, 0.0f, 1.0f, 0.0f,
            0.5f, 0.5f, 0.0f, 1.0f
        };

        auto S = DirectX::XMMATRIX{lightView * lightProj * T};
        DirectX::XMStoreFloat4x4(&mLightView, lightView);
        DirectX::XMStoreFloat4x4(&mLightProj, lightProj);
        DirectX::XMStoreFloat4x4(&mShadowTransform, S);
    }

    void UpdateMainPassCB(const GameTimer& gt)
    {
        auto view = DirectX::XMMATRIX{mCamera.GetView()};
		auto detView = DirectX::XMVECTOR{DirectX::XMMatrixDeterminant(view)};
        auto invView = DirectX::XMMATRIX{DirectX::XMMatrixInverse(&detView, view)};

        auto proj = DirectX::XMMATRIX{mCamera.GetProj()};
        auto detProj = DirectX::XMVECTOR{DirectX::XMMatrixDeterminant(proj)};
        auto invProj = DirectX::XMMATRIX{DirectX::XMMatrixInverse(&detProj, proj)};

        auto viewProj = DirectX::XMMATRIX{DirectX::XMMatrixMultiply(view, proj)};
        auto detViewProj = DirectX::XMVECTOR{DirectX::XMMatrixDeterminant(viewProj)};
        auto invViewProj = DirectX::XMMATRIX{DirectX::XMMatrixInverse(&detViewProj, viewProj)};

        // Transform NDC space [-1,+1]^2 to texture space [0,1]^2
        auto T = DirectX::XMMATRIX{
            0.5f, 0.0f, 0.0f, 0.0f,
            0.0f, -0.5f, 0.0f, 0.0f,
            0.0f, 0.0f, 1.0f, 0.0f,
            0.5f, 0.5f, 0.0f, 1.0f
        };

        auto viewProjTex = DirectX::XMMATRIX{DirectX::XMMatrixMultiply(viewProj, T)};
        auto shadowTransform = DirectX::XMMATRIX{DirectX::XMLoadFloat4x4(&mShadowTransform)};

        DirectX::XMStoreFloat4x4(&mMainPassCB.gView, DirectX::XMMatrixTranspose(view));
        DirectX::XMStoreFloat4x4(&mMainPassCB.gInvView, DirectX::XMMatrixTranspose(invView));
        DirectX::XMStoreFloat4x4(&mMainPassCB.gProj, DirectX::XMMatrixTranspose(proj));
        DirectX::XMStoreFloat4x4(&mMainPassCB.gInvProj, DirectX::XMMatrixTranspose(invProj));
        DirectX::XMStoreFloat4x4(&mMainPassCB.gViewProj, DirectX::XMMatrixTranspose(viewProj));
        DirectX::XMStoreFloat4x4(&mMainPassCB.gInvViewProj, DirectX::XMMatrixTranspose(invViewProj));
        DirectX::XMStoreFloat4x4(&mMainPassCB.gShadowTransform, DirectX::XMMatrixTranspose(shadowTransform));
        DirectX::XMStoreFloat4x4(&mMainPassCB.gViewProjTex, DirectX::XMMatrixTranspose(viewProjTex));

        mMainPassCB.gEyePosW = mCamera.GetPosition3f();
        mMainPassCB.gRenderTargetSize = DirectX::XMFLOAT2((float)mClientWidth, (float)mClientHeight);
        mMainPassCB.gInvRenderTargetSize = DirectX::XMFLOAT2(1.0f / mClientWidth, 1.0f / mClientHeight);
        mMainPassCB.gNearZ = 1.0f;
        mMainPassCB.gFarZ = 1000.0f;
        mMainPassCB.gTotalTime = gt.TotalTime();
        mMainPassCB.gDeltaTime = gt.DeltaTime();
        mMainPassCB.gAmbientLight = { 0.25f, 0.25f, 0.35f, 1.0f };
        mMainPassCB.gRandomTexIndex = mRandomTexBindlessIndex;
        mMainPassCB.gSkyBoxIndex = mSkyBindlessIndex;

        mMainPassCB.gSceneDepthMapIndex = mPrepass->GetSceneDepthMapBindlessIndex();
        mMainPassCB.gSceneNormalMapIndex = mPrepass->GetSceneNormalMapBindlessIndex();
        mMainPassCB.gReflectionMapUavIndex = mRayTracer->GetReflectionMapUavIndex();
        mMainPassCB.gReflectionMapSrvIndex = mRayTracer->GetReflectionMapSrvIndex();
        mMainPassCB.gDebugTexIndex = mRayTracer->GetReflectionMapSrvIndex();

        mMainPassCB.gNormalMapsEnabled = mNormalMapsEnabled;
        mMainPassCB.gReflectionsEnabled = mReflectionsEnabled;
        mMainPassCB.gShadowsEnabled = mShadowsEnabled;

        mMainPassCB.gLights[0].Direction = mRotatedLightDirections[0];
        mMainPassCB.gLights[0].Strength = { 0.8f, 0.8f, 0.8f };
        mMainPassCB.gLights[1].Direction = mRotatedLightDirections[1];
        mMainPassCB.gLights[1].Strength = { 0.1f, 0.1f, 0.1f };
        mMainPassCB.gLights[2].Direction = mRotatedLightDirections[2];
        mMainPassCB.gLights[2].Strength = { 0.1f, 0.1f, 0.1f };

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
        constexpr bool useIndex32 = true;
        auto shapeGeo = std::unique_ptr<MeshGeometry>{d3dUtil::BuildShapeGeometry(md3dDevice.Get(), *mUploadBatch.get(), useIndex32)};
        mGeometries[shapeGeo->Name] = std::move(shapeGeo);

        auto skullGeo = std::unique_ptr<MeshGeometry>{d3dUtil::BuildSkullGeometry(md3dDevice.Get(), *mUploadBatch.get())};
        mGeometries[skullGeo->Name] = std::move(skullGeo);

        auto columnSquare = std::unique_ptr<MeshGeometry>{
            d3dUtil::LoadSimpleModelGeometry(md3dDevice.Get(), *mUploadBatch.get(), "Models/columnSquare.m3d", "columnSquare", useIndex32)
        };

        mGeometries[columnSquare->Name] = std::move(columnSquare);

        auto columnSquareBroken = std::unique_ptr<MeshGeometry>{
            d3dUtil::LoadSimpleModelGeometry(md3dDevice.Get(), *mUploadBatch.get(), "Models/columnSquareBroken.m3d", "columnSquareBroken", useIndex32)
        };
        mGeometries[columnSquareBroken->Name] = std::move(columnSquareBroken);

        auto columnRound = std::unique_ptr<MeshGeometry>{
            d3dUtil::LoadSimpleModelGeometry(
                md3dDevice.Get(), 
                *mUploadBatch.get(),
                "Models/columnRound.m3d", 
                "columnRound",
                useIndex32
            )
        };
        mGeometries[columnRound->Name] = std::move(columnRound);

        auto columnRoundBroken = std::unique_ptr<MeshGeometry>{
            d3dUtil::LoadSimpleModelGeometry(
                md3dDevice.Get(), *mUploadBatch.get(),
                "Models/columnRoundBroken.m3d", "columnRoundBroken",
                useIndex32
            )
        };
        mGeometries[columnRoundBroken->Name] = std::move(columnRoundBroken);

        auto orbBase = std::unique_ptr<MeshGeometry>{
            d3dUtil::LoadSimpleModelGeometry(
                md3dDevice.Get(), 
                *mUploadBatch.get(),
                "Models/orbBase.m3d", 
                "orbBase",
                useIndex32
            )
        };
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
            0, 
            nullptr, // static samplers
            D3D12::D3D12_ROOT_SIGNATURE_FLAGS{
                D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT |
                D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED |
                D3D12_ROOT_SIGNATURE_FLAG_SAMPLER_HEAP_DIRECTLY_INDEXED
            }
        };

        // create a root signature with a single slot which points to a descriptor range consisting of a single constant buffer
        auto serializedRootSig = Microsoft::WRL::ComPtr<D3D::ID3DBlob>{};
        auto errorBlob = Microsoft::WRL::ComPtr<D3D::ID3DBlob>{};
        auto hr = D3D12::D3D12SerializeRootSignature(&gfxRootSigDesc, D3D_ROOT_SIGNATURE_VERSION_1,
            serializedRootSig.GetAddressOf(), errorBlob.GetAddressOf());

        if (errorBlob != nullptr)
            Win32::OutputDebugStringA((char*)errorBlob->GetBufferPointer());
        ThrowIfFailed(hr);

        ThrowIfFailed(md3dDevice->CreateRootSignature(
            0,
            serializedRootSig->GetBufferPointer(),
            serializedRootSig->GetBufferSize(),
            __uuidof(D3D12::ID3D12RootSignature),
            reinterpret_cast<void**>(mRootSignature.GetAddressOf())));
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

        mPrepass->AllocateDescriptors(mRtvHeap.CpuHandle(RTV_NORMALMAP));
        mPrepass->OnResize(mClientWidth, mClientHeight, mDepthStencilBuffer.Get());

        //
        // Ray tracing needs descriptors to the geometry buffers.
        //
        mShapeVertexBufferBindlessIndex = cbvSrvUavHeap.NextFreeIndex();
        mShapeIndexBufferBindlessIndex = cbvSrvUavHeap.NextFreeIndex();
        mOrbBaseVertexBufferBindlessIndex = cbvSrvUavHeap.NextFreeIndex();
        mOrbBaseIndexBufferBindlessIndex = cbvSrvUavHeap.NextFreeIndex();
        mColumnRoundBrokenVertexBufferBindlessIndex = cbvSrvUavHeap.NextFreeIndex();
        mColumnRoundBrokenIndexBufferBindlessIndex = cbvSrvUavHeap.NextFreeIndex();
        mColumnSquareBrokenVertexBufferBindlessIndex = cbvSrvUavHeap.NextFreeIndex();
        mColumnSquareBrokenIndexBufferBindlessIndex = cbvSrvUavHeap.NextFreeIndex();
        mColumnSquareVertexBufferBindlessIndex = cbvSrvUavHeap.NextFreeIndex();
        mColumnSquareIndexBufferBindlessIndex = cbvSrvUavHeap.NextFreeIndex();

        auto indexByteSize = std::uint32_t{mGeometries["shapeGeo"]->IndexFormat == DXGI_FORMAT_R16_UINT ? 2u : 4u};
        auto vertexCount = std::uint32_t{mGeometries["shapeGeo"]->VertexBufferByteSize / mGeometries["shapeGeo"]->VertexByteStride};
        auto indexCount = std::uint32_t{mGeometries["shapeGeo"]->IndexBufferByteSize / indexByteSize};

        CreateBufferSrv(md3dDevice.Get(), 0, vertexCount, sizeof(RTVertex), mGeometries["shapeGeo"]->VertexBufferGPU.Get(), cbvSrvUavHeap.CpuHandle(mShapeVertexBufferBindlessIndex));
        CreateBufferSrv(md3dDevice.Get(), 0, indexCount, indexByteSize, mGeometries["shapeGeo"]->IndexBufferGPU.Get(), cbvSrvUavHeap.CpuHandle(mShapeIndexBufferBindlessIndex));

        indexByteSize = std::uint32_t{mGeometries["orbBase"]->IndexFormat == DXGI_FORMAT_R16_UINT ? 2u : 4u};
        vertexCount = std::uint32_t{mGeometries["orbBase"]->VertexBufferByteSize / mGeometries["orbBase"]->VertexByteStride};
        indexCount = std::uint32_t{mGeometries["orbBase"]->IndexBufferByteSize / indexByteSize};

        CreateBufferSrv(md3dDevice.Get(), 0, vertexCount, sizeof(RTVertex), mGeometries["orbBase"]->VertexBufferGPU.Get(), cbvSrvUavHeap.CpuHandle(mOrbBaseVertexBufferBindlessIndex));
        CreateBufferSrv(md3dDevice.Get(), 0, indexCount, indexByteSize, mGeometries["orbBase"]->IndexBufferGPU.Get(), cbvSrvUavHeap.CpuHandle(mOrbBaseIndexBufferBindlessIndex));

        indexByteSize = std::uint32_t{mGeometries["columnRoundBroken"]->IndexFormat == DXGI_FORMAT_R16_UINT ? 2u : 4u};
        vertexCount = std::uint32_t{mGeometries["columnRoundBroken"]->VertexBufferByteSize / mGeometries["columnRoundBroken"]->VertexByteStride};
        indexCount = std::uint32_t{mGeometries["columnRoundBroken"]->IndexBufferByteSize / indexByteSize};

        CreateBufferSrv(md3dDevice.Get(), 0, vertexCount, sizeof(RTVertex), mGeometries["columnRoundBroken"]->VertexBufferGPU.Get(), cbvSrvUavHeap.CpuHandle(mColumnRoundBrokenVertexBufferBindlessIndex));
        CreateBufferSrv(md3dDevice.Get(), 0, indexCount, indexByteSize, mGeometries["columnRoundBroken"]->IndexBufferGPU.Get(), cbvSrvUavHeap.CpuHandle(mColumnRoundBrokenIndexBufferBindlessIndex));

        indexByteSize = std::uint32_t{mGeometries["columnSquareBroken"]->IndexFormat == DXGI_FORMAT_R16_UINT ? 2u : 4u};
        vertexCount = std::uint32_t{mGeometries["columnSquareBroken"]->VertexBufferByteSize / mGeometries["columnSquareBroken"]->VertexByteStride};
        indexCount = std::uint32_t{mGeometries["columnSquareBroken"]->IndexBufferByteSize / indexByteSize};

        CreateBufferSrv(md3dDevice.Get(), 0, vertexCount, sizeof(RTVertex), mGeometries["columnSquareBroken"]->VertexBufferGPU.Get(), cbvSrvUavHeap.CpuHandle(mColumnSquareBrokenVertexBufferBindlessIndex));
        CreateBufferSrv(md3dDevice.Get(), 0, indexCount, indexByteSize, mGeometries["columnSquareBroken"]->IndexBufferGPU.Get(), cbvSrvUavHeap.CpuHandle(mColumnSquareBrokenIndexBufferBindlessIndex));

        indexByteSize = std::uint32_t{mGeometries["columnSquare"]->IndexFormat == DXGI_FORMAT_R16_UINT ? 2u : 4u};
        vertexCount = std::uint32_t{mGeometries["columnSquare"]->VertexBufferByteSize / mGeometries["columnSquare"]->VertexByteStride};
        indexCount = std::uint32_t{mGeometries["columnSquare"]->IndexBufferByteSize / indexByteSize};

        CreateBufferSrv(md3dDevice.Get(), 0, vertexCount, sizeof(RTVertex), mGeometries["columnSquare"]->VertexBufferGPU.Get(), cbvSrvUavHeap.CpuHandle(mColumnSquareVertexBufferBindlessIndex));
        CreateBufferSrv(md3dDevice.Get(), 0, indexCount, indexByteSize, mGeometries["columnSquare"]->IndexBufferGPU.Get(), cbvSrvUavHeap.CpuHandle(mColumnSquareIndexBufferBindlessIndex));
    }

    void BuildShaders()
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
            mRootSignature.Get(),
            nullptr);
    }

    void BuildFrameResources()
    {
        for (int i = 0; i < gNumFrameResources; ++i)
        {
            mFrameResources.push_back(std::make_unique<FrameResource>(md3dDevice.Get(),
                2, static_cast<std::uint32_t>(mAllRitems.size()), MaterialLib::GetLib().GetMaterialCount()));
        }
    }

    void BuildMaterials()
    {
        MaterialLib::GetLib().Init(md3dDevice.Get());
    }

    void InitRayTracing()
    {
        auto& shaderLib = ShaderLib::GetLib();

        mRayTracer = std::make_unique<HybridRayTracer>(
            md3dDevice.Get(),
            mCommandList.Get(),
            shaderLib["hybridReflectionsRTLib"],
            mClientWidth, mClientHeight);

        // For simplicity, we use 32-bit indices in this demo. But you can use 
        // a ByteAddressBuffer and pack two 16-bit indices per dword.
        constexpr auto MakeRtModel = 
            [](const MeshGeometry* geo, const SubmeshGeometry& drawArgs, std::uint32_t vbIndex, std::uint32_t ibIndex) -> HybridRayTracer::RTModelDef
            {
                return {
                    .VertexBuffer = geo->VertexBufferGPU.Get(),
                    .IndexBuffer = geo->IndexBufferGPU.Get(),
                    .VertexBufferBindlessIndex = vbIndex,
                    .IndexBufferBindlessIndex = ibIndex,
                    .IndexFormat = DXGI_FORMAT_R32_UINT,
                    .VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT,
                    .IndexCount = drawArgs.IndexCount,
                    .VertexCount = drawArgs.VertexCount,
                    .StartIndexLocation = drawArgs.StartIndexLocation,
                    .BaseVertexLocation = static_cast<std::uint32_t>(drawArgs.BaseVertexLocation),
                    .VertexSizeInBytes = sizeof(RTVertex),
                    .IndexSizeInBytes = sizeof(std::uint32_t),
                };
            };

        /*assert(mShapeVertexBufferBindlessIndex != -1);
        assert(mShapeIndexBufferBindlessIndex != -1);
        assert(mOrbBaseVertexBufferBindlessIndex != -1);
        assert(mOrbBaseIndexBufferBindlessIndex != -1);
        assert(mColumnRoundBrokenVertexBufferBindlessIndex != -1);
        assert(mColumnRoundBrokenIndexBufferBindlessIndex != -1);
        assert(mColumnSquareBrokenVertexBufferBindlessIndex != -1);
        assert(mColumnSquareBrokenIndexBufferBindlessIndex != -1);
        assert(mColumnSquareVertexBufferBindlessIndex != -1);
        assert(mColumnSquareIndexBufferBindlessIndex != -1);*/

        // Define RTModelDefs to the same geometry we use for rasterization.
        HybridRayTracer::RTModelDef grid = MakeRtModel(
            mGeometries["shapeGeo"].get(), mGeometries["shapeGeo"]->DrawArgs["grid"],
            mShapeVertexBufferBindlessIndex, mShapeIndexBufferBindlessIndex);
        HybridRayTracer::RTModelDef sphere = MakeRtModel(
            mGeometries["shapeGeo"].get(), mGeometries["shapeGeo"]->DrawArgs["sphere"],
            mShapeVertexBufferBindlessIndex, mShapeIndexBufferBindlessIndex);
        HybridRayTracer::RTModelDef orbBaseModel = MakeRtModel(
            mGeometries["orbBase"].get(), mGeometries["orbBase"]->DrawArgs["subset0"],
            mOrbBaseVertexBufferBindlessIndex, mOrbBaseIndexBufferBindlessIndex);
        HybridRayTracer::RTModelDef columnRoundBrokenModel = MakeRtModel(
            mGeometries["columnRoundBroken"].get(), mGeometries["columnRoundBroken"]->DrawArgs["subset0"],
            mColumnRoundBrokenVertexBufferBindlessIndex, mColumnRoundBrokenIndexBufferBindlessIndex);
        HybridRayTracer::RTModelDef columnSquareBrokenModel = MakeRtModel(
            mGeometries["columnSquareBroken"].get(), mGeometries["columnSquareBroken"]->DrawArgs["subset0"],
            mColumnSquareBrokenVertexBufferBindlessIndex, mColumnSquareBrokenIndexBufferBindlessIndex);
        HybridRayTracer::RTModelDef columnSquareModel = MakeRtModel(
            mGeometries["columnSquare"].get(), mGeometries["columnSquare"]->DrawArgs["subset0"],
            mColumnSquareVertexBufferBindlessIndex, mColumnSquareIndexBufferBindlessIndex);

        mRayTracer->AddModel("gridModel", grid);
        mRayTracer->AddModel("sphereModel", sphere);
        mRayTracer->AddModel("orbBaseModel", orbBaseModel);
        mRayTracer->AddModel("columnRoundBrokenModel", columnRoundBrokenModel);
        mRayTracer->AddModel("columnSquareBrokenModel", columnSquareBrokenModel);
        mRayTracer->AddModel("columnSquareModel", columnSquareModel);
    }

    void AddRenderItem(RenderLayer layer, const DirectX::XMFLOAT4X4& world, const DirectX::XMFLOAT4X4& texTransform, Material* mat, MeshGeometry* geo, SubmeshGeometry& drawArgs)
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

    void BuildRenderItems()
    {
        auto& matLib = MaterialLib::GetLib();

        auto worldTransform = DirectX::XMFLOAT4X4{};
        auto texTransform = DirectX::XMFLOAT4X4{};

        DirectX::XMStoreFloat4x4(&worldTransform, DirectX::XMMatrixScaling(5000.0f, 5000.0f, 5000.0f));
        texTransform = MathHelper::Identity4x4;
        AddRenderItem(RenderLayer::Sky, worldTransform, texTransform, matLib["sky"], mGeometries["shapeGeo"].get(), mGeometries["shapeGeo"]->DrawArgs["sphere"]);

        DirectX::XMStoreFloat4x4(&worldTransform, DirectX::XMMatrixScaling(6.0f, 6.0f, 6.0f) * DirectX::XMMatrixTranslation(0.0f, 0.0f, 0.0f));
        DirectX::XMStoreFloat4x4(&texTransform, DirectX::XMMatrixScaling(1.0f, 1.0f, 1.0f));
        AddRenderItem(RenderLayer::Opaque, worldTransform, texTransform, matLib["orbBase"], mGeometries["orbBase"].get(), mGeometries["orbBase"]->DrawArgs["subset0"]);
        mRayTracer->AddInstance("orbBaseModel", worldTransform, DirectX::XMFLOAT2(1.0f, 1.0f), matLib["orbBase"]->MatIndex);

        DirectX::XMStoreFloat4x4(&worldTransform, DirectX::XMMatrixScaling(2.0f, 2.0f, 2.0f) * DirectX::XMMatrixTranslation(0.0f, 1.75f, 0.0f));
        texTransform = MathHelper::Identity4x4;
        AddRenderItem(RenderLayer::Opaque, worldTransform, texTransform, matLib["mirror1"], mGeometries["shapeGeo"].get(), mGeometries["shapeGeo"]->DrawArgs["sphere"]);
        mRayTracer->AddInstance("sphereModel", worldTransform, DirectX::XMFLOAT2(1.0f, 1.0f), matLib["mirror1"]->MatIndex);

        worldTransform = MathHelper::Identity4x4;
        DirectX::XMStoreFloat4x4(&texTransform, DirectX::XMMatrixScaling(6.0f, 6.0f, 1.0f));
        AddRenderItem(RenderLayer::Opaque, worldTransform, texTransform, matLib["stoneFloor"], mGeometries["shapeGeo"].get(), mGeometries["shapeGeo"]->DrawArgs["grid"]);
        mRayTracer->AddInstance("gridModel", worldTransform, DirectX::XMFLOAT2(6.0f, 6.0f), matLib["stoneFloor"]->MatIndex);

        auto falledColumnTransform0 = DirectX::XMMATRIX{
            DirectX::XMMatrixRotationZ(-0.54f * DirectX::Pi)*
            DirectX::XMMatrixRotationY(0.15f * DirectX::Pi)*
            DirectX::XMMatrixScaling(1.0f, 1.0f, 1.0f)*
            DirectX::XMMatrixTranslation(-3.0f, 0.35f, 3.0f)
        };
        DirectX::XMStoreFloat4x4(&worldTransform, falledColumnTransform0);
        texTransform = MathHelper::Identity4x4;
        AddRenderItem(RenderLayer::Opaque, worldTransform, texTransform, matLib["columnRound"], mGeometries["columnRoundBroken"].get(), mGeometries["columnRoundBroken"]->DrawArgs["subset0"]);
        mRayTracer->AddInstance("columnRoundBrokenModel", worldTransform, DirectX::XMFLOAT2(1.0f, 1.0f), matLib["columnRound"]->MatIndex);

        auto falledColumnTransform1 = DirectX::XMMATRIX{
            DirectX::XMMatrixRotationZ(-0.54f * DirectX::Pi) *
            DirectX::XMMatrixRotationY(0.75f * DirectX::Pi) *
            DirectX::XMMatrixScaling(1.0f, 1.0f, 1.0f) *
            DirectX::XMMatrixTranslation(1.5f, 0.35f, -4.0f)
        };
        DirectX::XMStoreFloat4x4(&worldTransform, falledColumnTransform1);
        texTransform = MathHelper::Identity4x4;
        AddRenderItem(RenderLayer::Opaque, worldTransform, texTransform, matLib["columnRound"], mGeometries["columnRoundBroken"].get(), mGeometries["columnRoundBroken"]->DrawArgs["subset0"]);
        mRayTracer->AddInstance("columnRoundBrokenModel", worldTransform, DirectX::XMFLOAT2(1.0f, 1.0f), matLib["columnRound"]->MatIndex);

        for (int i = 0; i < 5; ++i)
        {
            bool isLeftColumnBroken = (i == 2);
            bool isRightColumnBroken = (i == 0 || i == 4);

            auto columnNameLeft = isLeftColumnBroken ? std::string{ "columnSquareBroken" } : std::string{ "columnSquare" };
            auto columnNameRight = isRightColumnBroken ? std::string{ "columnSquareBroken" } : std::string{ "columnSquare" };

            DirectX::XMStoreFloat4x4(&texTransform, DirectX::XMMatrixScaling(1.0f, 1.0f, 1.0f));
            DirectX::XMStoreFloat4x4(&worldTransform, DirectX::XMMatrixTranslation(-5.0f, 0.0f, -10.0f + i * 5.0f));
            AddRenderItem(RenderLayer::Opaque, worldTransform, texTransform, matLib["columnSquare"], mGeometries[columnNameLeft].get(), mGeometries[columnNameLeft]->DrawArgs["subset0"]);
            mRayTracer->AddInstance(columnNameLeft + "Model", worldTransform, DirectX::XMFLOAT2(1.0f, 1.0f), matLib["columnSquare"]->MatIndex);

            DirectX::XMStoreFloat4x4(&texTransform, DirectX::XMMatrixScaling(1.0f, 1.0f, 1.0f));
            DirectX::XMStoreFloat4x4(&worldTransform, DirectX::XMMatrixTranslation(+5.0f, 0.0f, -10.0f + i * 5.0f));
            AddRenderItem(RenderLayer::Opaque, worldTransform, texTransform, matLib["columnSquare"], mGeometries[columnNameRight].get(), mGeometries[columnNameRight]->DrawArgs["subset0"]);
            mRayTracer->AddInstance(columnNameRight + "Model", worldTransform, DirectX::XMFLOAT2(1.0f, 1.0f), matLib["columnSquare"]->MatIndex);

            if (not isLeftColumnBroken)
            {
                texTransform = MathHelper::Identity4x4;
                DirectX::XMStoreFloat4x4(&worldTransform, DirectX::XMMatrixTranslation(-5.0f, 4.0f, -10.0f + i * 5.0f));
                AddRenderItem(RenderLayer::Opaque, worldTransform, texTransform, matLib["mirror0"], mGeometries["shapeGeo"].get(), mGeometries["shapeGeo"]->DrawArgs["sphere"]);
                mRayTracer->AddInstance("sphereModel", worldTransform, DirectX::XMFLOAT2(1.0f, 1.0f), matLib["mirror0"]->MatIndex);
            }

            if (not isRightColumnBroken)
            {
                texTransform = MathHelper::Identity4x4;
                DirectX::XMStoreFloat4x4(&worldTransform, DirectX::XMMatrixTranslation(+5.0f, 4.0f, -10.0f + i * 5.0f));
                AddRenderItem(RenderLayer::Opaque, worldTransform, texTransform, matLib["mirror0"], mGeometries["shapeGeo"].get(), mGeometries["shapeGeo"]->DrawArgs["sphere"]);
                mRayTracer->AddInstance("sphereModel", worldTransform, DirectX::XMFLOAT2(1.0f, 1.0f), matLib["mirror0"]->MatIndex);
            }
        }

        worldTransform = MathHelper::Identity4x4;
        texTransform = MathHelper::Identity4x4;
        AddRenderItem(RenderLayer::Debug, worldTransform, texTransform, matLib["bricks0"], mGeometries["shapeGeo"].get(), mGeometries["shapeGeo"]->DrawArgs["quad"]);
    }

    void DrawRenderItems(D3D12::ID3D12GraphicsCommandList* cmdList, const std::vector<RenderItem*>& ritems)
    {
        for (size_t i = 0; i < ritems.size(); ++i)
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

    void DrawNormalsAndDepth()
    {
        auto& psoLib = PsoLib::GetLib();

        auto normalMap = mPrepass->GetSceneNormalMap();
        auto normalMapRtv = mPrepass->GetSceneNormalMapRtv();

        mCommandList->RSSetViewports(1, &mScreenViewport);
        mCommandList->RSSetScissorRects(1, &mScissorRect);

        // Change to RENDER_TARGET.
        auto transition = D3D12::CD3DX12_RESOURCE_BARRIER::Transition(normalMap,
            D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_STATE_RENDER_TARGET);
        mCommandList->ResourceBarrier(1, &transition);

        // Clear the screen normal map and depth buffer.
        auto clearValue = std::array{ 0.0f, 0.0f, 1.0f, 0.0f };
        mCommandList->ClearRenderTargetView(normalMapRtv, clearValue.data(), 0, nullptr);
        mCommandList->ClearDepthStencilView(DepthStencilView(), D3D12::D3D12_CLEAR_FLAGS{ D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL }, 1.0f, 0, 0, nullptr);

        // Specify the buffers we are going to render to.
		auto dsv = DepthStencilView();
        mCommandList->OMSetRenderTargets(1, &normalMapRtv, true, &dsv);

        // Bind the constant buffer for this pass.
        auto passCB = mCurrFrameResource->PassCB->Resource();
        mCommandList->SetGraphicsRootConstantBufferView(GFX_ROOT_ARG_PASS_CBV, passCB->GetGPUVirtualAddress());

        mCommandList->SetPipelineState(psoLib["drawBumpedWorldNormals"]);

        DrawRenderItems(mCommandList.Get(), mRitemLayer[(int)RenderLayer::Opaque]);

        // Change back to GENERIC_READ so we can read the texture in a shader.
        transition = D3D12::CD3DX12_RESOURCE_BARRIER::Transition(normalMap,
            D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_GENERIC_READ);
        mCommandList->ResourceBarrier(1, &transition);
    }

private:
    std::vector<std::unique_ptr<FrameResource>> mFrameResources;
    FrameResource* mCurrFrameResource = nullptr;
    int mCurrFrameResourceIndex = 0;

    Microsoft::WRL::ComPtr<D3D12::ID3D12RootSignature> mRootSignature = nullptr;

    std::unordered_map<std::string, std::unique_ptr<MeshGeometry>> mGeometries;

    std::unique_ptr<Prepass> mPrepass;

    std::uint32_t mShapeVertexBufferBindlessIndex = -1;
    std::uint32_t mShapeIndexBufferBindlessIndex = -1;
    std::uint32_t mOrbBaseVertexBufferBindlessIndex = -1;
    std::uint32_t mOrbBaseIndexBufferBindlessIndex = -1;
    std::uint32_t mColumnRoundBrokenVertexBufferBindlessIndex = -1;
    std::uint32_t mColumnRoundBrokenIndexBufferBindlessIndex = -1;
    std::uint32_t mColumnSquareBrokenVertexBufferBindlessIndex = -1;
    std::uint32_t mColumnSquareBrokenIndexBufferBindlessIndex = -1;
    std::uint32_t mColumnSquareVertexBufferBindlessIndex = -1;
    std::uint32_t mColumnSquareIndexBufferBindlessIndex = -1;

    // List of all the render items.
    std::vector<std::unique_ptr<RenderItem>> mAllRitems;

    // Render items divided by PSO.
    std::vector<RenderItem*> mRitemLayer[(int)RenderLayer::Count];

    std::unique_ptr<HybridRayTracer> mRayTracer = nullptr;

    std::uint32_t mRandomTexBindlessIndex = -1;
    std::uint32_t mSkyBindlessIndex = -1;

    D3D12::CD3DX12_GPU_DESCRIPTOR_HANDLE mNullSrv;

    PerPassCB mMainPassCB;  // index 0 of pass cbuffer.
    PerPassCB mShadowPassCB;// index 1 of pass cbuffer.

    Camera mCamera;

    DirectX::BoundingSphere mSceneBounds;

    float mLightNearZ = 0.0f;
    float mLightFarZ = 0.0f;
    DirectX::XMFLOAT3 mLightPosW;
    DirectX::XMFLOAT4X4 mLightView = MathHelper::Identity4x4;
    DirectX::XMFLOAT4X4 mLightProj = MathHelper::Identity4x4;
    DirectX::XMFLOAT4X4 mShadowTransform = MathHelper::Identity4x4;

    float mLightRotationAngle = 0.0f;
    DirectX::XMFLOAT3 mBaseLightDirections[3] = {
        DirectX::XMFLOAT3(0.57735f, -0.57735f, 0.57735f),
        DirectX::XMFLOAT3(-0.57735f, -0.57735f, 0.57735f),
        DirectX::XMFLOAT3(0.0f, -0.707f, -0.707f)
    };
    DirectX::XMFLOAT3 mRotatedLightDirections[3];

    bool mNormalMapsEnabled = true;
    bool mReflectionsEnabled = true;
    bool mShadowsEnabled = true;

    Win32::POINT mLastMousePos;
};