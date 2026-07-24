export module skinnedmesh:skinnedmeshapp;
import std;
import shared;
import :frameresource;
import :ssao;

struct SkinnedModelInstance
{
    SkinnedData* SkinnedInfo = nullptr;
    std::vector<DirectX::XMFLOAT4X4> FinalTransforms;
    std::string ClipName;
    float TimePos = 0.0f;

    DirectX::GraphicsResource MemHandleToSkinnedCB;

    // Called every frame and increments the time position, interpolates the 
    // animations for each bone based on the current animation clip, and 
    // generates the final transforms which are ultimately set to the effect
    // for processing in the vertex shader.
    void UpdateSkinnedAnimation(float dt)
    {
        TimePos += dt;

        // Loop animation
        if (TimePos > SkinnedInfo->GetClipEndTime(ClipName))
            TimePos = 0.0f;

        // Compute the final transforms for this time position.
        SkinnedInfo->GetFinalTransforms(ClipName, TimePos, FinalTransforms);
    }
};

// Lightweight structure stores parameters to draw a shape.  This will
// vary from app-to-app.
struct RenderItem
{
    // World matrix of the shape that describes the object's local space
    // relative to the world space, which defines the position, orientation,
    // and scale of the object in the world.
    DirectX::XMFLOAT4X4 World = MathHelper::Identity4x4;

    DirectX::XMFLOAT4X4 TexTransform = MathHelper::Identity4x4;

    PerObjectCB ObjectConstants{};

    // Handle to memory in linear allocator.
    DirectX::GraphicsResource MemHandleToObjectCB;

    // nullptr if this render-item is not animated by skinned mesh.
    SkinnedModelInstance* SkinnedModelInst = nullptr;

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
    SkinnedOpaque,
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
    RTV_SSAO_MAP0,
    RTV_SSAO_MAP1,
    RTV_COUNT
};

enum DsvOffsets
{
    DSV_MAINVIEW = 0,
    DSV_SHADOWMAP,
};

constexpr auto CBV_SRV_UAV_HEAP_CAPACITY = 16384u;

export class SkinnedMeshApp : public D3DApp
{
public:
    SkinnedMeshApp(Win32::HINSTANCE hInstance)
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

    ~SkinnedMeshApp()
    {
        if (md3dDevice != nullptr)
            FlushCommandQueue();
    }

    SkinnedMeshApp(const SkinnedMeshApp& rhs) = delete;
    SkinnedMeshApp& operator=(const SkinnedMeshApp& rhs) = delete;

private:
    void Initialize()override
    {
        D3DApp::Initialize();

        mCamera.SetPosition(0.0f, 2.0f, -15.0f);

        mShadowMap = std::make_unique<ShadowMap>(md3dDevice.Get(), 2048, 2048);

        mSsao = std::make_unique<Ssao>(md3dDevice.Get(), mClientWidth, mClientHeight);

        // Create the singleton.
        DirectX::GraphicsMemory::Get(md3dDevice.Get());

        // We will upload on the direct queue for the book samples, but 
        // copy queue would be better for real game.
        mUploadBatch->Begin(D3D12_COMMAND_LIST_TYPE_DIRECT);

        // Do init work that requires mUploadBatch...
        LoadSkinnedModel();
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
        mRtvHeap.Init(md3dDevice.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_RTV, RTV_COUNT);
        mDsvHeap.Init(md3dDevice.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 2);
    }

    void OnResize()override
    {
        D3DApp::OnResize();

        mCamera.SetLens(0.25f * MathHelper::Pi, AspectRatio(), 1.0f, 1000.0f);

        if (mSsao)
            mSsao->OnResize(mClientWidth, mClientHeight);

        if (CbvSrvUavHeap::Get().IsInitialized())
        {
            // Depth/stencil buffer gets recreated on resize, so need to recreate the view.
            CbvSrvUavHeap& cbvSrvUavHeap = CbvSrvUavHeap::Get();
            CreateSrv2d(md3dDevice.Get(), mDepthStencilBuffer.Get(), DXGI_FORMAT_R24_UNORM_X8_TYPELESS, 1, cbvSrvUavHeap.CpuHandle(mMainDepthBufferBindlessIndex));
        }
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
        mLightRotationAngle += 0.1f * gt.DeltaTime();

        auto R = DirectX::XMMATRIX{DirectX::XMMatrixRotationY(mLightRotationAngle)};
        for (int i = 0; i < 3; ++i)
        {
            auto lightDir = DirectX::XMVECTOR{DirectX::XMLoadFloat3(&mBaseLightDirections[i])};
            lightDir = DirectX::XMVector3TransformNormal(lightDir, R);
            DirectX::XMStoreFloat3(&mRotatedLightDirections[i], lightDir);
        }

        AnimateMaterials(gt);
        UpdatePerObjectCB(gt);
        UpdateSkinnedCBs(gt);
        UpdateMaterialBuffer(gt);
        UpdateShadowTransform(gt);
        UpdateMainPassCB(gt);
        UpdateShadowPassCB(gt);

        mSsao->SetOcclusionRadius(mOcclusionRadius);
        mSsao->SetOcclusionFadeStart(mOcclusionFadeStart);
        mSsao->SetOcclusionFadeEnd(mOcclusionFadeEnd);
        mSsao->SetSurfaceEpsilon(mSurfaceEpsilon);
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
        mCommandList->SetDescriptorHeaps(static_cast<UINT>(descriptorHeaps.size()), descriptorHeaps.data());

        mCommandList->SetGraphicsRootSignature(mRootSignature.Get());

        // Bind all the materials used in this scene.  For structured buffers, we can bypass the heap and 
        // set as a root descriptor.
        auto matBuffer = mCurrFrameResource->MaterialBuffer->Resource();
        mCommandList->SetGraphicsRootShaderResourceView(GFX_ROOT_ARG_MATERIAL_SRV, matBuffer->GetGPUVirtualAddress());

        DrawSceneToShadowMap();

        //
        // Normal/depth pass.
        DrawNormalsAndDepth();

        //
        // Compute SSAO.
        mSsao->ComputeSsao(mCommandList.Get(), psoLib["ssao"]);
        mSsao->BlurAmbientMap(mCommandList.Get(), psoLib["ssaoBlur"], 3);

        //
        // Main rendering pass.
        mCommandList->RSSetViewports(1, &mScreenViewport);
        mCommandList->RSSetScissorRects(1, &mScissorRect);

        // Indicate a state transition on the resource usage.
        auto transition = D3D12::CD3DX12_RESOURCE_BARRIER::Transition(CurrentBackBuffer(),
            D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
        mCommandList->ResourceBarrier(1, &transition);

        // Clear the back buffer and depth buffer.
        mCommandList->ClearRenderTargetView(CurrentBackBufferView(), DirectX::Colors::LightSteelBlue, 0, nullptr);

        if (mDrawWireframe)
            mCommandList->ClearDepthStencilView(DepthStencilView(), D3D12::D3D12_CLEAR_FLAGS{ D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL }, 1.0f, 0, 0, nullptr);

        // WE ALREADY WROTE THE DEPTH INFO TO THE DEPTH BUFFER IN DrawNormalsAndDepth,
        // SO DO NOT CLEAR DEPTH.

        // Specify the buffers we are going to render to.
		auto cbbv = CurrentBackBufferView();
		auto dsv = DepthStencilView();
        mCommandList->OMSetRenderTargets(1, &cbbv, true, &dsv);

        auto passCB = mCurrFrameResource->PassCB->Resource();
        mCommandList->SetGraphicsRootConstantBufferView(GFX_ROOT_ARG_PASS_CBV, passCB->GetGPUVirtualAddress());

        mCommandList->SetPipelineState(mDrawWireframe ? psoLib["opaque_wireframe"] : psoLib["opaque_wprepass"]);
        DrawRenderItems(mCommandList.Get(), mRitemLayer[(int)RenderLayer::Opaque]);

        mCommandList->SetPipelineState(mDrawWireframe ? psoLib["skinnedOpaque_wireframe"] : psoLib["skinnedOpaque_wprepass"]);
        DrawRenderItems(mCommandList.Get(), mRitemLayer[(int)RenderLayer::SkinnedOpaque]);

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
        auto presentParams = DXGI::DXGI_PRESENT_PARAMETERS{ };
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
        ImGui::Checkbox("NormalMaps", &mNormalMapsEnabled);
        ImGui::Checkbox("Reflections", &mReflectionsEnabled);
        ImGui::Checkbox("Shadows", &mShadowsEnabled);

        if (ImGui::CollapsingHeader("SSAO"))
        {
            ImGui::Checkbox("SsaoEnabled", &mSsaoEnabled);
            ImGui::SliderFloat("OcclusionRadius", &mOcclusionRadius, 0.1f, 2.0f);
            ImGui::SliderFloat("OcclusionFadeStart", &mOcclusionFadeStart, 0.0f, 4.0f);
            ImGui::SliderFloat("OcclusionFadeEnd", &mOcclusionFadeEnd, 0.0f, 4.0f);
            ImGui::SliderFloat("SurfaceEpsilon", &mSurfaceEpsilon, 0.0f, 10.0f);
        }

        //assert(mOcclusionFadeStart < mOcclusionFadeEnd);

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
        if (auto& io = ImGui::GetIO();!io.WantCaptureMouse)
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

    void UpdateSkinnedCBs(const GameTimer& gt)
    {
        // We only have one skinned model being animated.
        mSkinnedModelInst->UpdateSkinnedAnimation(gt.DeltaTime());

        auto skinnedConstants = SkinnedCB{};
        std::copy(
            std::begin(mSkinnedModelInst->FinalTransforms),
            std::end(mSkinnedModelInst->FinalTransforms),
            &skinnedConstants.gBoneTransforms[0]);

        mSkinnedModelInst->MemHandleToSkinnedCB = mLinearAllocator->AllocateConstant(skinnedConstants);
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

            auto matTransform = DirectX::XMLoadFloat4x4(&mat->MatTransform);
            auto matData = MaterialData{
                .DiffuseAlbedo = mat->DiffuseAlbedo,
                .FresnelR0 = mat->FresnelR0,
                .Roughness = mat->Roughness,
                .DiffuseMapIndex = static_cast<std::uint32_t>(mat->AlbedoBindlessIndex),
                .NormalMapIndex = static_cast<std::uint32_t>(mat->NormalBindlessIndex),
                .GlossHeightAoMapIndex = static_cast<std::uint32_t>(mat->GlossHeightAoBindlessIndex)
            };
            DirectX::XMStoreFloat4x4(&matData.MatTransform, DirectX::XMMatrixTranspose(matTransform));
            currMaterialBuffer->CopyData(mat->MatIndex, matData);
            // Next FrameResource need to be updated too.
            mat->NumFramesDirty--;
        }
    }

    void UpdateShadowTransform(const GameTimer& gt)
    {
        // Only the first "main" light casts a shadow.
        auto lightDir = DirectX::XMVECTOR{ DirectX::XMLoadFloat3(&mRotatedLightDirections[0]) };
        auto lightPos = DirectX::XMVECTOR{ -2.0f * mSceneBounds.Radius * lightDir };
        auto targetPos = DirectX::XMVECTOR{ DirectX::XMLoadFloat3(&mSceneBounds.Center) };
        auto lightUp = DirectX::XMVECTOR{ DirectX::XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f) };
        auto lightView = DirectX::XMMATRIX{ DirectX::XMMatrixLookAtLH(lightPos, targetPos, lightUp) };

        DirectX::XMStoreFloat3(&mLightPosW, lightPos);

        // Transform bounding sphere to light space.
        auto sphereCenterLS = DirectX::XMFLOAT3{};
        DirectX::XMStoreFloat3(&sphereCenterLS, DirectX::XMVector3TransformCoord(targetPos, lightView));

        // Ortho frustum in light space encloses scene.
        auto l = float{ sphereCenterLS.x - mSceneBounds.Radius };
        auto b = float{ sphereCenterLS.y - mSceneBounds.Radius };
        auto n = float{ sphereCenterLS.z - mSceneBounds.Radius };
        auto r = float{ sphereCenterLS.x + mSceneBounds.Radius };
        auto t = float{ sphereCenterLS.y + mSceneBounds.Radius };
        auto f = float{ sphereCenterLS.z + mSceneBounds.Radius };

        mLightNearZ = n;
        mLightFarZ = f;
        auto lightProj = DirectX::XMMATRIX{ DirectX::XMMatrixOrthographicOffCenterLH(l, r, b, t, n, f) };

        // Transform NDC space [-1,+1]^2 to texture space [0,1]^2
        auto T = DirectX::XMMATRIX{
            0.5f, 0.0f, 0.0f, 0.0f,
            0.0f, -0.5f, 0.0f, 0.0f,
            0.0f, 0.0f, 1.0f, 0.0f,
            0.5f, 0.5f, 0.0f, 1.0f
        };

        auto S = DirectX::XMMATRIX{ lightView * lightProj * T };
        DirectX::XMStoreFloat4x4(&mLightView, lightView);
        DirectX::XMStoreFloat4x4(&mLightProj, lightProj);
        DirectX::XMStoreFloat4x4(&mShadowTransform, S);
    }

    void UpdateMainPassCB(const GameTimer& gt)
    {
        auto view = DirectX::XMMATRIX{ mCamera.GetView() };
		auto detView = DirectX::XMVECTOR{ DirectX::XMMatrixDeterminant(view) };
		auto invView = DirectX::XMMATRIX{ DirectX::XMMatrixInverse(&detView, view) };

        auto proj = DirectX::XMMATRIX{ mCamera.GetProj() };
        auto detProj = DirectX::XMVECTOR{ DirectX::XMMatrixDeterminant(proj) };
        auto invProj = DirectX::XMMATRIX{ DirectX::XMMatrixInverse(&detProj, proj) };

        auto viewProj = DirectX::XMMATRIX{ DirectX::XMMatrixMultiply(view, proj) };
		auto detViewProj = DirectX::XMVECTOR{ DirectX::XMMatrixDeterminant(viewProj) };
		auto invViewProj = DirectX::XMMATRIX{ DirectX::XMMatrixInverse(&detViewProj, viewProj) };

        // Transform NDC space [-1,+1]^2 to texture space [0,1]^2
        auto T = DirectX::XMMATRIX{
            0.5f, 0.0f, 0.0f, 0.0f,
            0.0f, -0.5f, 0.0f, 0.0f,
            0.0f, 0.0f, 1.0f, 0.0f,
            0.5f, 0.5f, 0.0f, 1.0f 
        };

        auto viewProjTex = DirectX::XMMATRIX{ DirectX::XMMatrixMultiply(viewProj, T) };
        auto shadowTransform = DirectX::XMMATRIX{ DirectX::XMLoadFloat4x4(&mShadowTransform) };

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
        mMainPassCB.gSunShadowMapIndex = mShadowMapBindlessIndex;
        mMainPassCB.gSceneDepthMapIndex = mMainDepthBufferBindlessIndex;
        mMainPassCB.gSceneNormalMapIndex = mSsao->GetNormalMapBindlessIndex();
        mMainPassCB.gSsaoAmbientMap0Index = mSsao->GetAmbientMap0BindlessIndex();
        mMainPassCB.gSsaoAmbientMap1Index = mSsao->GetAmbientMap1BindlessIndex();

        mMainPassCB.gDebugTexIndex = mSsao->GetAmbientMap0BindlessIndex();

        mMainPassCB.gNormalMapsEnabled = mNormalMapsEnabled;
        mMainPassCB.gReflectionsEnabled = mReflectionsEnabled;
        mMainPassCB.gShadowsEnabled = mShadowsEnabled;
        mMainPassCB.gSsaoEnabled = mSsaoEnabled;

        mMainPassCB.gNumDirLights = 3;
        mMainPassCB.gNumPointLights = 0;
        mMainPassCB.gNumSpotLights = 0;

        // Tone down the light strength a bit just to accentuate the SSAO effect.
        mMainPassCB.gLights[0].Direction = mRotatedLightDirections[0];
        mMainPassCB.gLights[0].Strength = { 0.39f, 0.38f, 0.37f };
        mMainPassCB.gLights[1].Direction = mRotatedLightDirections[1];
        mMainPassCB.gLights[1].Strength = { 0.24f, 0.24f, 0.24f };
        mMainPassCB.gLights[2].Direction = mRotatedLightDirections[2];
        mMainPassCB.gLights[2].Strength = { 0.12f, 0.12f, 0.12f };

        auto currPassCB = mCurrFrameResource->PassCB.get();
        currPassCB->CopyData(0, mMainPassCB);
    }

    void UpdateShadowPassCB(const GameTimer& gt)
    {
        auto view = DirectX::XMMATRIX{ DirectX::XMLoadFloat4x4(&mLightView) };
		auto detView = DirectX::XMVECTOR{ DirectX::XMMatrixDeterminant(view) };
        auto invView = DirectX::XMMATRIX{ DirectX::XMMatrixInverse(&detView, view) };

        auto proj = DirectX::XMMATRIX{ DirectX::XMLoadFloat4x4(&mLightProj) };
		auto detProj = DirectX::XMVECTOR{ DirectX::XMMatrixDeterminant(proj) };
        auto invProj = DirectX::XMMATRIX{ DirectX::XMMatrixInverse(&detProj, proj) };

        auto viewProj = DirectX::XMMATRIX{ DirectX::XMMatrixMultiply(view, proj) };
		auto detViewProj = DirectX::XMVECTOR{ DirectX::XMMatrixDeterminant(viewProj) };
        auto invViewProj = DirectX::XMMATRIX{ DirectX::XMMatrixInverse(&detViewProj, viewProj) };

        auto w = mShadowMap->Width();
        auto h = mShadowMap->Height();

        DirectX::XMStoreFloat4x4(&mShadowPassCB.gView, DirectX::XMMatrixTranspose(view));
        DirectX::XMStoreFloat4x4(&mShadowPassCB.gInvView, DirectX::XMMatrixTranspose(invView));
        DirectX::XMStoreFloat4x4(&mShadowPassCB.gProj, DirectX::XMMatrixTranspose(proj));
        DirectX::XMStoreFloat4x4(&mShadowPassCB.gInvProj, DirectX::XMMatrixTranspose(invProj));
        DirectX::XMStoreFloat4x4(&mShadowPassCB.gViewProj, DirectX::XMMatrixTranspose(viewProj));
        DirectX::XMStoreFloat4x4(&mShadowPassCB.gInvViewProj, DirectX::XMMatrixTranspose(invViewProj));
        mShadowPassCB.gEyePosW = mLightPosW;
        mShadowPassCB.gRenderTargetSize = DirectX::XMFLOAT2((float)w, (float)h);
        mShadowPassCB.gInvRenderTargetSize = DirectX::XMFLOAT2(1.0f / w, 1.0f / h);
        mShadowPassCB.gNearZ = mLightNearZ;
        mShadowPassCB.gFarZ = mLightFarZ;

        auto currPassCB = mCurrFrameResource->PassCB.get();
        currPassCB->CopyData(1, mShadowPassCB);
    }

    void LoadTextures()
    {
        auto& texLib = TextureLib::GetLib();
        texLib.Init(md3dDevice.Get(), *mUploadBatch.get());

        auto texNames = std::vector<std::string>{};
        auto texFilenames = std::vector<std::wstring>{};
        for (auto i = 0u; i < mSkinnedMats.size(); ++i)
        {
            auto diffuseName = mSkinnedMats[i].DiffuseMapName;
            auto normalName = mSkinnedMats[i].NormalMapName;

            auto diffuseFilename =  std::format(L"Textures/{}", AnsiToWString(diffuseName));
            auto normalFilename = std::format(L"Textures/{}", AnsiToWString(normalName));

            // strip off extension
            diffuseName = diffuseName.substr(0, diffuseName.find_last_of("."));
            normalName = normalName.substr(0, normalName.find_last_of("."));

            mSkinnedTextureNames.push_back(diffuseName);
            texNames.push_back(diffuseName);
            texFilenames.push_back(diffuseFilename);

            mSkinnedTextureNames.push_back(normalName);
            texNames.push_back(normalName);
            texFilenames.push_back(normalFilename);
        }

        for (auto i = 0u; i < texNames.size(); ++i)
        {
            if (texLib.Contains(texNames[i]))
                continue;
            auto texMap = std::make_unique<Texture>();
            texMap->Name = texNames[i];
            texMap->Filename = texFilenames[i];
            ThrowIfFailed(DirectX::CreateDDSTextureFromFileEx(md3dDevice.Get(), *mUploadBatch.get(),
                texMap->Filename.c_str(), 0, D3D12_RESOURCE_FLAG_NONE, DirectX::DDS_LOADER_FLAGS::DDS_LOADER_MIP_AUTOGEN,
                &texMap->Resource, nullptr, &texMap->IsCubeMap));
            texLib.AddTexture(texNames[i], std::move(texMap));
        }
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

    void LoadSkinnedModel()
    {
        auto vertices = std::vector<M3DLoader::SkinnedVertex>{};
        auto indices32 = std::vector<std::uint32_t>{};

        auto m3dLoader = M3DLoader{};
        if (not m3dLoader.LoadM3d(mSkinnedModelFilename, vertices, indices32, mSkinnedSubsets, mSkinnedMats, mSkinnedInfo))
            throw std::runtime_error{"Failed to load skinned model"};

        auto indices = std::vector<std::uint16_t>(indices32.size());
        for (auto i = 0ull; i < indices32.size(); ++i)
            indices[i] = static_cast<std::uint16_t>(indices32[i]);

        mSkinnedModelInst = std::make_unique<SkinnedModelInstance>();
        mSkinnedModelInst->SkinnedInfo = &mSkinnedInfo;
        mSkinnedModelInst->FinalTransforms.resize(mSkinnedInfo.BoneCount());
        mSkinnedModelInst->ClipName = "Take1";
        mSkinnedModelInst->TimePos = 0.0f;

        const auto vbByteSize = static_cast<std::uint32_t>(vertices.size() * sizeof(M3DLoader::SkinnedVertex));
        const auto ibByteSize = static_cast<std::uint32_t>(indices.size() * sizeof(std::uint16_t));

        auto geo = std::make_unique<MeshGeometry>();
        geo->Name = mSkinnedModelFilename;

        geo->VertexBufferCPU.resize(vbByteSize);
        std::memcpy(geo->VertexBufferCPU.data(), vertices.data(), vbByteSize);

        geo->IndexBufferCPU.resize(ibByteSize);
        std::memcpy(geo->IndexBufferCPU.data(), indices.data(), ibByteSize);

        CreateStaticBuffer(md3dDevice.Get(), *mUploadBatch.get(),
            vertices.data(), vertices.size(), sizeof(M3DLoader::SkinnedVertex),
            D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER, &geo->VertexBufferGPU);

        CreateStaticBuffer(md3dDevice.Get(), *mUploadBatch.get(),
            indices.data(), indices.size(), sizeof(std::uint16_t),
            D3D12_RESOURCE_STATE_INDEX_BUFFER, &geo->IndexBufferGPU);

        geo->VertexByteStride = sizeof(M3DLoader::SkinnedVertex);
        geo->VertexBufferByteSize = vbByteSize;
        geo->IndexFormat = DXGI_FORMAT_R16_UINT;
        geo->IndexBufferByteSize = ibByteSize;

        for (auto i = 0u; i < static_cast<std::uint32_t>(mSkinnedSubsets.size()); ++i)
        {
            auto submesh = SubmeshGeometry{
                .IndexCount = static_cast<std::uint32_t>(mSkinnedSubsets[i].FaceCount * 3),
                .StartIndexLocation = static_cast<std::uint32_t>(mSkinnedSubsets[i].FaceStart * 3),
                .BaseVertexLocation = 0,
                .VertexCount = static_cast<std::uint32_t>(vertices.size())
            };
            //submesh.Bounds = bounds;

            auto name = std::format("sm_{}", i);
            geo->DrawArgs[name] = submesh;
        }

        mGeometries[geo->Name] = std::move(geo);
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

        mShadowMapBindlessIndex = mShadowMap->BuildDescriptors(mDsvHeap.CpuHandle(DSV_SHADOWMAP));

        mSsao->BuildDescriptors(
            mRtvHeap.CpuHandle(RTV_NORMALMAP),
            mRtvHeap.CpuHandle(RTV_SSAO_MAP0),
            mRtvHeap.CpuHandle(RTV_SSAO_MAP1));

        // Create SRV to depth buffer so we can sample it in a shader. When we start to sample from it,
        // we need to be done writing to it.
        mMainDepthBufferBindlessIndex = cbvSrvUavHeap.NextFreeIndex();
        CreateSrv2d(md3dDevice.Get(), mDepthStencilBuffer.Get(), DXGI_FORMAT_R24_UNORM_X8_TYPELESS, 1, cbvSrvUavHeap.CpuHandle(mMainDepthBufferBindlessIndex));
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
        for (auto i = 0u; i < gNumFrameResources; ++i)
        {
            mFrameResources.push_back(std::make_unique<FrameResource>(md3dDevice.Get(), 2, static_cast<std::uint32_t>(mAllRitems.size()), MaterialLib::GetLib().GetMaterialCount()));
        }
    }

    void BuildMaterials()
    {
        auto& matLib = MaterialLib::GetLib();
        matLib.Init(md3dDevice.Get());

        auto& texLib = TextureLib::GetLib();

        for (auto i = 0u; i < mSkinnedMats.size(); ++i)
        {
            auto diffuseName = mSkinnedMats[i].DiffuseMapName;
            auto normalName = mSkinnedMats[i].NormalMapName;

            // strip off extension
            diffuseName = diffuseName.substr(0, diffuseName.find_last_of("."));
            normalName = normalName.substr(0, normalName.find_last_of("."));

            matLib.AddMaterial(
                mSkinnedMats[i].Name,
                texLib[diffuseName],
                texLib[normalName],
                texLib["defaultGlossHeightAoMap"],
                mSkinnedMats[i].DiffuseAlbedo,
                mSkinnedMats[i].FresnelR0,
                mSkinnedMats[i].Roughness);
        }
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
        ritem->World = world;
        ritem->TexTransform = texTransform;
        ritem->Mat = mat;
        ritem->Geo = geo;
        ritem->PrimitiveType = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
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

        DirectX::XMStoreFloat4x4(&worldTransform, DirectX::XMMatrixScaling(2.0f, 2.0f, 2.0f) * DirectX::XMMatrixTranslation(0.0f, 1.75f, 0.0f));
        texTransform = MathHelper::Identity4x4;
        AddRenderItem(RenderLayer::Opaque, worldTransform, texTransform, matLib["mirror1"], mGeometries["shapeGeo"].get(), mGeometries["shapeGeo"]->DrawArgs["sphere"]);

        worldTransform = MathHelper::Identity4x4;
        DirectX::XMStoreFloat4x4(&texTransform, DirectX::XMMatrixScaling(6.0f, 6.0f, 1.0f));
        AddRenderItem(RenderLayer::Opaque, worldTransform, texTransform, matLib["stoneFloor"], mGeometries["shapeGeo"].get(), mGeometries["shapeGeo"]->DrawArgs["grid"]);

        auto falledColumnTransform0 = DirectX::XMMATRIX{
            DirectX::XMMatrixRotationZ(-0.54f * DirectX::Pi) *
            DirectX::XMMatrixRotationY(0.15f * DirectX::Pi) *
            DirectX::XMMatrixScaling(1.0f, 1.0f, 1.0f) *
            DirectX::XMMatrixTranslation(-3.0f, 0.35f, 3.0f)
        };
        DirectX::XMStoreFloat4x4(&worldTransform, falledColumnTransform0);
        texTransform = MathHelper::Identity4x4;
        AddRenderItem(RenderLayer::Opaque, worldTransform, texTransform, matLib["columnRound"], mGeometries["columnRoundBroken"].get(), mGeometries["columnRoundBroken"]->DrawArgs["subset0"]);

        auto falledColumnTransform1 = DirectX::XMMATRIX{
            DirectX::XMMatrixRotationZ(-0.54f * DirectX::Pi)*
            DirectX::XMMatrixRotationY(0.75f * DirectX::Pi)*
            DirectX::XMMatrixScaling(1.0f, 1.0f, 1.0f)*
            DirectX::XMMatrixTranslation(1.5f, 0.35f, -4.0f)
        };
        DirectX::XMStoreFloat4x4(&worldTransform, falledColumnTransform1);
        texTransform = MathHelper::Identity4x4;
        AddRenderItem(RenderLayer::Opaque, worldTransform, texTransform, matLib["columnRound"], mGeometries["columnRoundBroken"].get(), mGeometries["columnRoundBroken"]->DrawArgs["subset0"]);


        for (int i = 0; i < 5; ++i)
        {
            bool isLeftColumnBroken = (i == 2);
            bool isRightColumnBroken = (i == 0 || i == 4);

            auto columnNameLeft = isLeftColumnBroken ? std::string{"columnSquareBroken"} : std::string{"columnSquare"};
            auto columnNameRight = isRightColumnBroken ? std::string{"columnSquareBroken"} : std::string{"columnSquare"};

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

        for (auto i = 0u; i < mSkinnedMats.size(); ++i)
        {
            auto submeshName = std::format("sm_{}", i);
            auto ritem = std::make_unique<RenderItem>();
            // Reflect to change coordinate system from the RHS the data was exported out as.
            auto modelScale = DirectX::XMMATRIX{ DirectX::XMMatrixScaling(0.05f, 0.05f, -0.05f) };
            auto modelRot = DirectX::XMMATRIX{ DirectX::XMMatrixRotationY(DirectX::Pi) };
            auto modelOffset = DirectX::XMMATRIX{ DirectX::XMMatrixTranslation(0.0f, 0.0f, -5.0f) };
            DirectX::XMStoreFloat4x4(&ritem->World, modelScale * modelRot * modelOffset);

            ritem->TexTransform = MathHelper::Identity4x4;
            ritem->Mat = matLib[mSkinnedMats[i].Name];
            ritem->Geo = mGeometries[mSkinnedModelFilename].get();
            ritem->PrimitiveType = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
            ritem->IndexCount = ritem->Geo->DrawArgs[submeshName].IndexCount;
            ritem->StartIndexLocation = ritem->Geo->DrawArgs[submeshName].StartIndexLocation;
            ritem->BaseVertexLocation = ritem->Geo->DrawArgs[submeshName].BaseVertexLocation;

            // All render items for this solider.m3d instance share
            // the same skinned model instance.
            ritem->SkinnedModelInst = mSkinnedModelInst.get();

            mRitemLayer[(int)RenderLayer::SkinnedOpaque].push_back(ritem.get());
            mAllRitems.push_back(std::move(ritem));
        }

        DirectX::XMStoreFloat4x4(
            &worldTransform,
            DirectX::XMMatrixScaling(0.5f, 0.5f, 1.0f) *
            DirectX::XMMatrixTranslation(0.5f, -0.5f, 0.0f));
        texTransform = MathHelper::Identity4x4;
        AddRenderItem(RenderLayer::Debug, worldTransform, texTransform, matLib["bricks0"], mGeometries["shapeGeo"].get(), mGeometries["shapeGeo"]->DrawArgs["quad"]);
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
            cmdList->SetGraphicsRootConstantBufferView(GFX_ROOT_ARG_OBJECT_CBV, ri->MemHandleToObjectCB.GpuAddress());
            if (ri->SkinnedModelInst != nullptr)
                cmdList->SetGraphicsRootConstantBufferView(GFX_ROOT_ARG_SKINNED_CBV, ri->SkinnedModelInst->MemHandleToSkinnedCB.GpuAddress());
            else
                cmdList->SetGraphicsRootConstantBufferView(GFX_ROOT_ARG_SKINNED_CBV, 0);
            cmdList->DrawIndexedInstanced(ri->IndexCount, 1, ri->StartIndexLocation, ri->BaseVertexLocation, 0);
        }
    }

    void DrawSceneToShadowMap()
    {
        auto& psoLib = PsoLib::GetLib();

		auto viewport = mShadowMap->Viewport();
		auto scissorRect = mShadowMap->ScissorRect();
        mCommandList->RSSetViewports(1, &viewport);
        mCommandList->RSSetScissorRects(1, &scissorRect);

        // Change to DEPTH_WRITE.
        auto transition = D3D12::CD3DX12_RESOURCE_BARRIER::Transition(mShadowMap->Resource(),
            D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_STATE_DEPTH_WRITE);
        mCommandList->ResourceBarrier(1, &transition);

        auto passCBByteSize = d3dUtil::CalcConstantBufferByteSize(sizeof(PerPassCB));

        // Clear the back buffer and depth buffer.
        mCommandList->ClearDepthStencilView(mShadowMap->Dsv(),
            D3D12::D3D12_CLEAR_FLAGS{ D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL }, 1.0f, 0, 0, nullptr);

        // Set null render target because we are only going to draw to
        // depth buffer.  Setting a null render target will disable color writes.
        // Note the active PSO also must specify a render target count of 0.
		auto dsv = mShadowMap->Dsv();
        mCommandList->OMSetRenderTargets(0, nullptr, false, &dsv);

        // Bind the pass constant buffer for the shadow map pass.
        auto passCB = mCurrFrameResource->PassCB->Resource();
        auto passCBAddress = D3D12::D3D12_GPU_VIRTUAL_ADDRESS{passCB->GetGPUVirtualAddress() + 1 * passCBByteSize};
        mCommandList->SetGraphicsRootConstantBufferView(GFX_ROOT_ARG_PASS_CBV, passCBAddress);

        mCommandList->SetPipelineState(psoLib["shadow_opaque"]);
        DrawRenderItems(mCommandList.Get(), mRitemLayer[(int)RenderLayer::Opaque]);

        mCommandList->SetPipelineState(psoLib["skinnedShadow_opaque"]);
        DrawRenderItems(mCommandList.Get(), mRitemLayer[(int)RenderLayer::SkinnedOpaque]);

        // Change back to GENERIC_READ so we can read the texture in a shader.
        transition = D3D12::CD3DX12_RESOURCE_BARRIER::Transition(mShadowMap->Resource(),
            D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_GENERIC_READ);
        mCommandList->ResourceBarrier(1, &transition);
    }

    void DrawNormalsAndDepth()
    {
        auto& psoLib = PsoLib::GetLib();

        mCommandList->RSSetViewports(1, &mScreenViewport);
        mCommandList->RSSetScissorRects(1, &mScissorRect);

        auto normalMap = mSsao->NormalMap();
        auto normalMapRtv = mSsao->NormalMapRtv();

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

        mCommandList->SetPipelineState(psoLib["drawViewNormals"]);
        DrawRenderItems(mCommandList.Get(), mRitemLayer[(int)RenderLayer::Opaque]);

        mCommandList->SetPipelineState(psoLib["drawSkinnedViewNormals"]);
        DrawRenderItems(mCommandList.Get(), mRitemLayer[(int)RenderLayer::SkinnedOpaque]);

        // Change back to GENERIC_READ so we can read the texture in a shader.
        transition = D3D12::CD3DX12_RESOURCE_BARRIER::Transition(normalMap,
            D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_GENERIC_READ);
        mCommandList->ResourceBarrier(1, &transition);
    }

private:
    std::vector<std::unique_ptr<FrameResource>> mFrameResources;
    FrameResource* mCurrFrameResource = nullptr;
    int mCurrFrameResourceIndex = 0;

    Microsoft::WRL::ComPtr<D3D12::ID3D12RootSignature> mRootSignature;

    std::unordered_map<std::string, std::unique_ptr<MeshGeometry>> mGeometries;

    // List of all the render items.
    std::vector<std::unique_ptr<RenderItem>> mAllRitems;

    // Render items divided by PSO.
    std::vector<RenderItem*> mRitemLayer[(int)RenderLayer::Count];

    std::uint32_t mRandomTexBindlessIndex = -1;
    std::uint32_t mSkyBindlessIndex = -1;
    std::uint32_t mShadowMapBindlessIndex = -1;
    std::uint32_t mMainDepthBufferBindlessIndex = -1;

    std::uint32_t mNullCubeSrvIndex = 0;
    std::uint32_t mNullTexSrvIndex = 0;

    D3D12::CD3DX12_GPU_DESCRIPTOR_HANDLE mNullSrv;

    PerPassCB mMainPassCB;  // index 0 of pass cbuffer.
    PerPassCB mShadowPassCB;// index 1 of pass cbuffer.

    Camera mCamera;

    std::unique_ptr<ShadowMap> mShadowMap;

    std::unique_ptr<Ssao> mSsao;

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

    std::string mSkinnedModelFilename = "Models/soldier.m3d";
    std::unique_ptr<SkinnedModelInstance> mSkinnedModelInst;
    SkinnedData mSkinnedInfo;
    std::vector<M3DLoader::Subset> mSkinnedSubsets;
    std::vector<M3DLoader::M3dMaterial> mSkinnedMats;
    std::vector<std::string> mSkinnedTextureNames;


    float mOcclusionRadius = 0.5f;
    float mOcclusionFadeStart = 0.2f;
    float mOcclusionFadeEnd = 1.0f;
    float mSurfaceEpsilon = 0.05f;

    bool mDrawWireframe = false;
    bool mNormalMapsEnabled = true;
    bool mReflectionsEnabled = true;
    bool mShadowsEnabled = true;
    bool mSsaoEnabled = true;

    Win32::POINT mLastMousePos{};
};