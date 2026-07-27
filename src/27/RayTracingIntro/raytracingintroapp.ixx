export module raytracingintro:raytracingintroapp;
import std;
import shared;
import :frameresource;
import :proceduralraytracer;

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
    RTV_OFFSET = D3DApp::SwapChainBufferCount,
};

enum DsvOffsets
{
    DSV_MAINVIEW = 0,
    DSV_SHADOWMAP,
};

constexpr auto CBV_SRV_UAV_HEAP_CAPACITY = 16384u;

export class RayTracingIntroApp : public D3DApp
{
public:
    RayTracingIntroApp(Win32::HINSTANCE hInstance)
        : D3DApp(hInstance)
    {
        // Estimate the scene bounding sphere manually since we know how the scene was constructed.
        // In general, you need to loop over every world space vertex position and compute the bounding sphere.
        mSceneBounds.Center = DirectX::XMFLOAT3(0.0f, 0.0f, 0.0f);
        mSceneBounds.Radius = 512;
        Initialize();
    }

    RayTracingIntroApp(const RayTracingIntroApp& rhs) = delete;
    RayTracingIntroApp& operator=(const RayTracingIntroApp& rhs) = delete;
    ~RayTracingIntroApp()
    {
        if (md3dDevice != nullptr)
            FlushCommandQueue();
    }

private:
    void Initialize()override
    {
        D3DApp::Initialize();

        mCamera.SetPosition(0.0f, 3.0f, -20.0f);

        // Create the singleton.
        // GraphicsMemory::Get(md3dDevice.Get());

        // We will upload on the direct queue for the book samples, but 
        // copy queue would be better for real game.
        mUploadBatch->Begin(D3D12_COMMAND_LIST_TYPE_DIRECT);

        // Do init work that requires mUploadBatch...
        LoadTextures();

        // Kick off upload work asyncronously.
        auto result = std::future<void>{mUploadBatch->End(mCommandQueue.Get())};

        // Other init work while uploading.
        BuildRootSignatures();
        BuildCbvSrvUavDescriptorHeap();
        BuildMaterials();
        BuildShaders();
        BuildRayTraceScene(); //BuildRenderItems();
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
        mRtvHeap.Init(md3dDevice.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_RTV, SwapChainBufferCount);
        mDsvHeap.Init(md3dDevice.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 2);
    }

    void OnResize()override
    {
        D3DApp::OnResize();
        mCamera.SetLens(0.25f * MathHelper::Pi, AspectRatio(), 1.0f, 1500.0f);
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
        if (mCurrFrameResource->Fence != 0 and mFence->GetCompletedValue() < mCurrFrameResource->Fence)
        {
            auto event = Event{};
            ThrowIfFailed(mFence->SetEventOnCompletion(mCurrFrameResource->Fence, event.Get()));
            event.Wait();
        }

        //
        // Animate the lights (and hence shadows).
        //

        //mLightRotationAngle += 0.05f*gt.DeltaTime();
        mLightRotationAngle = 0.0f;

        auto R = DirectX::XMMATRIX{DirectX::XMMatrixRotationY(mLightRotationAngle)};
        for (int i = 0; i < 3; ++i)
        {
            auto lightDir = DirectX::XMVECTOR{DirectX::XMVector3Normalize(DirectX::XMLoadFloat3(&mBaseLightDirections[i]))};
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
        ThrowIfFailed(mCommandList->Reset(cmdListAlloc.Get(), nullptr));

        // SetDescriptorHeaps must be called before SetGraphicsRootSignature when using HEAP_DIRECTLY_INDEXED.
        auto descriptorHeaps = std::array{ cbvSrvUavHeap.GetD3dHeap(), samHeap.GetD3dHeap() };
        mCommandList->SetDescriptorHeaps(static_cast<std::uint32_t>(descriptorHeaps.size()), descriptorHeaps.data());

        mCommandList->SetGraphicsRootSignature(mGfxRootSignature.Get());
        mCommandList->SetComputeRootSignature(mComputeRootSignature.Get());

        auto passCB = mCurrFrameResource->PassCB->Resource();
        mCommandList->SetComputeRootConstantBufferView(COMPUTE_ROOT_ARG_PASS_CBV, passCB->GetGPUVirtualAddress());

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

        mCommandList->SetGraphicsRootConstantBufferView(GFX_ROOT_ARG_PASS_CBV, passCB->GetGPUVirtualAddress());

        mCommandList->SetPipelineState(psoLib["opaque"]);
        DrawRenderItems(mCommandList.Get(), mRitemLayer[(int)RenderLayer::Opaque]);

        mCommandList->SetPipelineState(psoLib["debug"]);
        DrawRenderItems(mCommandList.Get(), mRitemLayer[(int)RenderLayer::Debug]);

        mCommandList->SetPipelineState(psoLib["sky"]);
        DrawRenderItems(mCommandList.Get(), mRitemLayer[(int)RenderLayer::Sky]);

        if (mRayTracer != nullptr)
        {
            mRayTracer->Draw(passCB, matBuffer);

            auto renderTarget = static_cast<D3D12::ID3D12Resource*>(CurrentBackBuffer());
            auto rayTraceOutput = static_cast<D3D12::ID3D12Resource*>(mRayTracer->GetOutputImage());

            auto preCopyBarriers = std::array{
                D3D12::CD3DX12_RESOURCE_BARRIER::Transition(renderTarget, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_DEST),
                D3D12::CD3DX12_RESOURCE_BARRIER::Transition(rayTraceOutput, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE)
            };
            mCommandList->ResourceBarrier(static_cast<std::uint32_t>(preCopyBarriers.size()), preCopyBarriers.data());

            mCommandList->CopyResource(renderTarget, rayTraceOutput);

            auto postCopyBarriers = std::array{
                D3D12::CD3DX12_RESOURCE_BARRIER::Transition(renderTarget, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_RENDER_TARGET),
                D3D12::CD3DX12_RESOURCE_BARRIER::Transition(rayTraceOutput, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
            };
            mCommandList->ResourceBarrier(static_cast<std::uint32_t>(postCopyBarriers.size()), postCopyBarriers.data());

            // Restore 
            mCommandList->SetComputeRootSignature(mComputeRootSignature.Get());
            mCommandList->SetComputeRootConstantBufferView(COMPUTE_ROOT_ARG_PASS_CBV, passCB->GetGPUVirtualAddress());
        }

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

        // Define a panel to render GUI elements.

        ImGui::Begin("Options");

        ImGui::Text("Application average %.3f ms/frame (%.1f FPS)", 1000.0f / ImGui::GetIO().Framerate, ImGui::GetIO().Framerate);

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
        auto cameraSpeed = 10.0f;
        if (GetAsyncKeyState(Win32::VK::Shift) & 0x8000)
            cameraSpeed = 100.0f;
        if (GetAsyncKeyState('W') & 0x8000)
            mCamera.Walk(cameraSpeed * dt);
        if (GetAsyncKeyState('S') & 0x8000)
            mCamera.Walk(-cameraSpeed * dt);
        if (GetAsyncKeyState('A') & 0x8000)
            mCamera.Strafe(-cameraSpeed * dt);
        if (GetAsyncKeyState('D') & 0x8000)
            mCamera.Strafe(cameraSpeed * dt);
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
            // From documentation: 
            //   Make sure to keep the GraphicsResource handle alive as long as you need to access
            //   the memory on the CPU. For example, do not simply cache GpuAddress() and discard
            //   the GraphicsResource object, or your memory may be overwritten later.
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
                .DisplacementScale = mat->DisplacementScale,
                .DiffuseMapIndex = static_cast<std::uint32_t>(mat->AlbedoBindlessIndex),
                .NormalMapIndex = static_cast<std::uint32_t>(mat->NormalBindlessIndex),
                .GlossHeightAoMapIndex = static_cast<std::uint32_t>(mat->GlossHeightAoBindlessIndex),
                .TransparencyWeight = mat->TransparencyWeight,
                .IndexOfRefraction = mat->IndexOfRefraction,
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
        auto lightDir = DirectX::XMVECTOR{DirectX::XMLoadFloat3(&mRotatedLightDirections[0])};
        auto lightPos = DirectX::XMVECTOR{ -2.0f * mSceneBounds.Radius * lightDir };
        auto targetPos = DirectX::XMVECTOR{DirectX::XMLoadFloat3(&mSceneBounds.Center)};
        auto lightUp = DirectX::XMVECTOR{DirectX::XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f)};
        auto lightView = DirectX::XMMATRIX{DirectX::XMMatrixLookAtLH(lightPos, targetPos, lightUp)};

        DirectX::XMStoreFloat3(&mLightPosW, lightPos);

        // Transform bounding sphere to light space.
        auto sphereCenterLS = DirectX::XMFLOAT3{};
        DirectX::XMStoreFloat3(&sphereCenterLS, DirectX::XMVector3TransformCoord(targetPos, lightView));

        // Ortho frustum in light space encloses scene.
        float l = sphereCenterLS.x - mSceneBounds.Radius;
        float b = sphereCenterLS.y - mSceneBounds.Radius;
        float n = sphereCenterLS.z - mSceneBounds.Radius;
        float r = sphereCenterLS.x + mSceneBounds.Radius;
        float t = sphereCenterLS.y + mSceneBounds.Radius;
        float f = sphereCenterLS.z + mSceneBounds.Radius;

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

        auto shadowTransform = DirectX::XMMATRIX{DirectX::XMLoadFloat4x4(&mShadowTransform)};

        DirectX::XMStoreFloat4x4(&mMainPassCB.gView, DirectX::XMMatrixTranspose(view));
        DirectX::XMStoreFloat4x4(&mMainPassCB.gInvView, DirectX::XMMatrixTranspose(invView));
        DirectX::XMStoreFloat4x4(&mMainPassCB.gProj, DirectX::XMMatrixTranspose(proj));
        DirectX::XMStoreFloat4x4(&mMainPassCB.gInvProj, DirectX::XMMatrixTranspose(invProj));
        DirectX::XMStoreFloat4x4(&mMainPassCB.gViewProj, DirectX::XMMatrixTranspose(viewProj));
        DirectX::XMStoreFloat4x4(&mMainPassCB.gInvViewProj, DirectX::XMMatrixTranspose(invViewProj));
        DirectX::XMStoreFloat4x4(&mMainPassCB.gShadowTransform, DirectX::XMMatrixTranspose(shadowTransform));

        MathHelper::ExtractFrustumPlanes(viewProj, mMainPassCB.gWorldFrustumPlanes);

        mMainPassCB.gEyePosW = mCamera.GetPosition3f();
        mMainPassCB.gRenderTargetSize = DirectX::XMFLOAT2((float)mClientWidth, (float)mClientHeight);
        mMainPassCB.gInvRenderTargetSize = DirectX::XMFLOAT2(1.0f / mClientWidth, 1.0f / mClientHeight);
        mMainPassCB.gNearZ = mCamera.GetNearZ();
        mMainPassCB.gFarZ = mCamera.GetFarZ();
        mMainPassCB.gTotalTime = gt.TotalTime();
        mMainPassCB.gDeltaTime = gt.DeltaTime();
        mMainPassCB.gAmbientLight = { 0.35f, 0.35f, 0.45f, 1.0f };
        mMainPassCB.gSkyBoxIndex = mSkyBindlessIndex;
        mMainPassCB.gRandomTexIndex = mRandomTexBindlessIndex;
        mMainPassCB.gSunShadowMapIndex = mShadowMapBindlessIndex;
        mMainPassCB.gRayTraceImageIndex = mRayTracer->GetOutputTextureUavIndex();
        mMainPassCB.gDebugTexIndex = mShadowMapBindlessIndex;
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

    void BuildRootSignatures()
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
            reinterpret_cast<void**>(mGfxRootSignature.GetAddressOf()))
        );

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
        hr = D3D12::D3D12SerializeRootSignature(&computeRootSigDesc, D3D_ROOT_SIGNATURE_VERSION_1,
            serializedRootSig.GetAddressOf(), errorBlob.GetAddressOf());
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
            mGfxRootSignature.Get(),
            mComputeRootSignature.Get());
    }

    void BuildFrameResources()
    {
        for (auto i = 0; i < gNumFrameResources; ++i)
        {
            mFrameResources.push_back(std::make_unique<FrameResource>(md3dDevice.Get(), 2, (UINT)mAllRitems.size(), MaterialLib::GetLib().GetMaterialCount()));
        }
    }

    void BuildMaterials()
    {
        MaterialLib::GetLib().Init(md3dDevice.Get());
    }

    void BuildRayTraceScene()
    {
        auto& shaderLib = ShaderLib::GetLib();
        auto& matLib = MaterialLib::GetLib();

        mRayTracer = std::make_unique<ProceduralRayTracer>(
            md3dDevice.Get(),
            mCommandList.Get(),
            shaderLib["rayTracingLib"],
            DXGI_FORMAT_R8G8B8A8_UNORM,
            mClientWidth, 
            mClientHeight);

        // Basically ray tracing variation of BuildRenderItems.
        //   The box is [-1, 1]^3 in local space.
        //   void AddBox(const DirectX::XMFLOAT4X4& worldTransform, UINT materialIndex);
        // 
        //   The cylinder is centered at the origin, aligned with +y axis, has radius 1 and length 2 in local space.
        //   void AddCylinder(const DirectX::XMFLOAT4X4& worldTransform, UINT materialIndex);
        // 
        //   The sphere is centered at origin with radius 1 in local space.
        //   void AddSphere(const DirectX::XMFLOAT4X4& worldTransform, UINT materialIndex);

        auto worldTransform = DirectX::XMFLOAT4X4{};

        DirectX::XMStoreFloat4x4(&worldTransform, DirectX::XMMatrixScaling(2.0f, 1.0f, 2.0f) * DirectX::XMMatrixTranslation(0.0f, 1.0f, 0.0f));
        mRayTracer->AddBox(worldTransform, DirectX::XMFLOAT2(1.0f, 0.5f), matLib["bricks0"]->MatIndex);

        DirectX::XMStoreFloat4x4(&worldTransform, DirectX::XMMatrixScaling(20.0f, 0.01f, 30.0f));
        mRayTracer->AddBox(worldTransform, DirectX::XMFLOAT2(8.0f, 8.0f), matLib["tile0"]->MatIndex);

        DirectX::XMStoreFloat4x4(&worldTransform, DirectX::XMMatrixScaling(1.5f, 1.5f, 1.5f) * DirectX::XMMatrixTranslation(0.0f, 3.5f, 0.0f));
        mRayTracer->AddSphere(worldTransform, DirectX::XMFLOAT2(1.0f, 1.0f), matLib["glass0"]->MatIndex);

        for (auto i = 0; i < 5; ++i)
        {
            DirectX::XMStoreFloat4x4(&worldTransform, DirectX::XMMatrixScaling(0.5f, 1.5f, 0.5f) * DirectX::XMMatrixTranslation(-5.0f, 1.5f, -10.0f + i * 5.0f));
            mRayTracer->AddCylinder(worldTransform, DirectX::XMFLOAT2(1.5f, 2.0f), matLib["bricks0"]->MatIndex);

            DirectX::XMStoreFloat4x4(&worldTransform, DirectX::XMMatrixScaling(0.5f, 1.5f, 0.5f) * DirectX::XMMatrixTranslation(+5.0f, 1.5f, -10.0f + i * 5.0f));
            mRayTracer->AddCylinder(worldTransform, DirectX::XMFLOAT2(1.5f, 2.0f), matLib["bricks0"]->MatIndex);

            DirectX::XMStoreFloat4x4(&worldTransform, DirectX::XMMatrixScaling(0.5f, 1.0f, 0.5f) * DirectX::XMMatrixTranslation(-5.0f, 3.0f, -10.0f + i * 5.0f));
            mRayTracer->AddDisk(worldTransform, DirectX::XMFLOAT2(0.25f, 0.25f), matLib["bricks0"]->MatIndex);

            DirectX::XMStoreFloat4x4(&worldTransform, DirectX::XMMatrixScaling(0.5f, 1.0f, 0.5f) * DirectX::XMMatrixTranslation(+5.0f, 3.0f, -10.0f + i * 5.0f));
            mRayTracer->AddDisk(worldTransform, DirectX::XMFLOAT2(0.25f, 0.25f), matLib["bricks0"]->MatIndex);

            DirectX::XMStoreFloat4x4(&worldTransform, DirectX::XMMatrixScaling(0.5f, 0.5f, 0.5f) * DirectX::XMMatrixTranslation(-5.0f, 3.5f, -10.0f + i * 5.0f));
            mRayTracer->AddSphere(worldTransform, DirectX::XMFLOAT2(1.0f, 1.0f), matLib["mirror0"]->MatIndex);

            DirectX::XMStoreFloat4x4(&worldTransform, DirectX::XMMatrixScaling(0.5f, 0.5f, 0.5f) * DirectX::XMMatrixTranslation(+5.0f, 3.5f, -10.0f + i * 5.0f));
            mRayTracer->AddSphere(worldTransform, DirectX::XMFLOAT2(1.0f, 1.0f), matLib["mirror0"]->MatIndex);
        }
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

    void BuildRenderItems() {}

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

            cmdList->DrawIndexedInstanced(ri->IndexCount, 1, ri->StartIndexLocation, ri->BaseVertexLocation, 0);
        }
    }

private:
    std::vector<std::unique_ptr<FrameResource>> mFrameResources;
    FrameResource* mCurrFrameResource = nullptr;
    int mCurrFrameResourceIndex = 0;

    Microsoft::WRL::ComPtr<D3D12::ID3D12RootSignature> mGfxRootSignature;
    Microsoft::WRL::ComPtr<D3D12::ID3D12RootSignature> mComputeRootSignature;

    Microsoft::WRL::ComPtr<D3D12::ID3D12CommandSignature> mIndirectDispatch;
    Microsoft::WRL::ComPtr<D3D12::ID3D12CommandSignature> mIndirectDrawIndexed;

    std::unordered_map<std::string, std::unique_ptr<MeshGeometry>> mGeometries;

    // List of all the render items.
    std::vector<std::unique_ptr<RenderItem>> mAllRitems;

    // Render items divided by PSO.
    std::vector<RenderItem*> mRitemLayer[(int)RenderLayer::Count];

    std::unique_ptr<ProceduralRayTracer> mRayTracer = nullptr;

    std::uint32_t mRandomTexBindlessIndex = -1;
    std::uint32_t mSkyBindlessIndex = -1;
    std::uint32_t mShadowMapBindlessIndex = -1;

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
        DirectX::XMFLOAT3(0.4f, -0.2f, 0.4f),
        DirectX::XMFLOAT3(-0.57735f, -0.57735f, 0.57735f),
        DirectX::XMFLOAT3(0.0f, -0.707f, -0.707f)
    };
    DirectX::XMFLOAT3 mRotatedLightDirections[3];

    DirectX::XMFLOAT3 mAcceleration{ 0.0f, -9.8f, 0.0f };

    Win32::POINT mLastMousePos;
};