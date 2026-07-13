export module blenddemo;
import std;
import shared;

class Waves
{
public:
    Waves(int m, int n, float dx, float dt, float speed, float damping)
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

    auto RowCount()const -> int
    {
        return mNumRows;
    }
    auto ColumnCount()const -> int
    {
        return mNumCols;
    }
    auto VertexCount()const -> int
    {
        return mVertexCount;
    }
    auto TriangleCount()const -> int
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

    // Returns the solution at the ith grid point.
    auto Position(int i)const -> const DirectX::XMFLOAT3& { return mCurrSolution[i]; }

    // Returns the solution normal at the ith grid point.
    auto Normal(int i)const -> const DirectX::XMFLOAT3& { return mNormals[i]; }

    // Returns the unit tangent vector at the ith grid point in the local x-axis direction.
    auto TangentX(int i)const -> const DirectX::XMFLOAT3& { return mTangentX[i]; }

    void SetConstants(float speed, float damping)
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
                for (auto j = 1; j < mNumCols - 1; ++j)
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

                    auto n = DirectX::XMVECTOR{ DirectX::XMVector3Normalize(XMLoadFloat3(&mNormals[i * mNumCols + j])) };
                    DirectX::XMStoreFloat3(&mNormals[i * mNumCols + j], n);

                    mTangentX[i * mNumCols + j] = DirectX::XMFLOAT3(2.0f * mSpatialStep, r - l, 0.0f);
                    auto T = DirectX::XMVECTOR{ DirectX::XMVector3Normalize(XMLoadFloat3(&mTangentX[i * mNumCols + j])) };
                    DirectX::XMStoreFloat3(&mTangentX[i * mNumCols + j], T);
                }
            });
    }
    void Disturb(int i, int j, float magnitude)
    {
        // Don't disturb boundaries.
        //assert(i > 1 && i < mNumRows - 2);
        //assert(j > 1 && j < mNumCols - 2);

        auto halfMag = 0.5f * magnitude;

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

constexpr auto CBV_SRV_UAV_HEAP_CAPACITY = 16384u;

//
// Define named offsets to root parameters in root signature for readability.
enum GFX_ROOT_ARG
{
    GFX_ROOT_ARG_OBJECT_CBV = 0,
    GFX_ROOT_ARG_PASS_CBV,
    GFX_ROOT_ARG_MATERIAL_SRV,
    GFX_ROOT_ARG_COUNT
};

// Stores the resources needed for the CPU to build the command lists
// for a frame. 
struct FrameResource
{
    FrameResource(D3D12::ID3D12Device* device, Win32::UINT passCount, Win32::UINT materialCount, Win32::UINT waveVertCount)
    {
        ThrowIfFailed(device->CreateCommandAllocator(
            D3D12_COMMAND_LIST_TYPE_DIRECT,
			__uuidof(D3D12::ID3D12CommandAllocator),
            (void**)CmdListAlloc.GetAddressOf()));

        PassCB = std::make_unique<UploadBuffer<PerPassCB>>(device, passCount, true);
        MaterialBuffer = std::make_unique<UploadBuffer<MaterialData>>(device, materialCount, false);
        WavesVB = std::make_unique<UploadBuffer<ModelVertex>>(device, waveVertCount, false);
    }

    // We cannot reset the allocator until the GPU is done processing the commands.
    // So each frame needs their own allocator.
    Microsoft::WRL::ComPtr<D3D12::ID3D12CommandAllocator> CmdListAlloc;

    // We cannot update a buffer until the GPU is done processing the commands
    // that reference it.  So each frame needs their own buffers.
    std::unique_ptr<UploadBuffer<PerPassCB>> PassCB = nullptr;
    std::unique_ptr<UploadBuffer<MaterialData>> MaterialBuffer = nullptr;

    // We cannot update a dynamic vertex buffer until the GPU is done processing
    // the commands that reference it.  So each frame needs their own.
    std::unique_ptr<UploadBuffer<ModelVertex>> WavesVB = nullptr;

    // Fence value to mark commands up to this fence point.  This lets us
    // check if these frame resources are still in use by the GPU.
    std::uint64_t Fence = 0;
};

enum class RenderLayer : int
{
    Opaque = 0,
    Transparent,
    AlphaTested,
    Debug,
    Sky,
    Count
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

export class BlendDemoApp : public D3DApp
{
public:
    BlendDemoApp(HINSTANCE hInstance)
        : D3DApp(hInstance)
    { }
    BlendDemoApp(const BlendDemoApp& rhs) = delete;
    BlendDemoApp& operator=(const BlendDemoApp& rhs) = delete;
    ~BlendDemoApp()
    {
        if (md3dDevice != nullptr)
            FlushCommandQueue();
    }

    virtual void Initialize()override
    {
        D3DApp::Initialize();

        mWaves = std::make_unique<Waves>(128, 128, 1.0f, 0.016f, mWaveSpeed, mWaveDamping);

        // We will upload on the direct queue for the book samples, but 
        // copy queue would be better for real game.
        mUploadBatch->Begin(D3D12_COMMAND_LIST_TYPE_DIRECT);

        LoadTextures();

        auto shapeGeo = std::unique_ptr<MeshGeometry>{ d3dUtil::BuildShapeGeometry(md3dDevice.Get(), *mUploadBatch.get()) };
        mGeometries[shapeGeo->Name] = std::move(shapeGeo);

        auto landGeo = std::unique_ptr<MeshGeometry>{ BuildLandGeometry(md3dDevice.Get(), *mUploadBatch.get()) };
        mGeometries[landGeo->Name] = std::move(landGeo);

        auto waveGeo = std::unique_ptr<MeshGeometry>{ BuildWaveGeometry(md3dDevice.Get(), *mUploadBatch.get()) };
        mGeometries[waveGeo->Name] = std::move(waveGeo);

        // Kick off upload work asyncronously.
        std::future<void> result = mUploadBatch->End(mCommandQueue.Get());

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
    virtual void CreateRtvAndDsvDescriptorHeaps()override
    {
        mRtvHeap.Init(md3dDevice.Get(), D3D12::D3D12_DESCRIPTOR_HEAP_TYPE::D3D12_DESCRIPTOR_HEAP_TYPE_RTV, SwapChainBufferCount);
        mDsvHeap.Init(md3dDevice.Get(), D3D12::D3D12_DESCRIPTOR_HEAP_TYPE::D3D12_DESCRIPTOR_HEAP_TYPE_DSV, SwapChainBufferCount);
    }
    virtual void OnResize()override
    {
        D3DApp::OnResize();
        // The window resized, so update the aspect ratio and recompute the projection matrix.
        auto P = DirectX::XMMATRIX{DirectX::XMMatrixPerspectiveFovLH(0.25f * MathHelper::Pi, AspectRatio(), 1.0f, 1000.0f)};
        DirectX::XMStoreFloat4x4(&mProj, P);
    }
    virtual void Update(const GameTimer& gt)override
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
			auto event = Event{ };
            ThrowIfFailed(mFence->SetEventOnCompletion(mCurrFrameResource->Fence, event.Get()));
            event.Wait();
        }

        //
        // Animate the lights.
        //

        mLightRotationAngle += 0.1f * gt.DeltaTime();

        auto R = DirectX::XMMATRIX{DirectX::XMMatrixRotationY(mLightRotationAngle)};
        for (int i = 0; i < 3; ++i)
        {
            DirectX::XMVECTOR lightDir = DirectX::XMLoadFloat3(&mBaseLightDirections[i]);
            lightDir = DirectX::XMVector3TransformNormal(lightDir, R);
            DirectX::XMStoreFloat3(&mRotatedLightDirections[i], lightDir);
        }

        AnimateMaterials(gt);
        UpdatePerObjectCB(gt);
        UpdateMaterialBuffer(gt);
        UpdateMainPassCB(gt);
        UpdateWaves(gt);
    }
    virtual void Draw(const GameTimer& gt)override
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
        mCommandList->SetDescriptorHeaps(static_cast<UINT>(descriptorHeaps.size()), descriptorHeaps.data());

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
        mCommandList->ClearDepthStencilView(DepthStencilView(), D3D12::D3D12_CLEAR_FLAGS{ D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL }, 1.0f, 0, 0, nullptr);

        // Specify the buffers we are going to render to.
        auto cbbv = CurrentBackBufferView();
        auto dsv = DepthStencilView();
        mCommandList->OMSetRenderTargets(1, &cbbv, true, &dsv);

        auto passCB = mCurrFrameResource->PassCB->Resource();
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

        // Draw imgui UI.
        ImGui::ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), mCommandList.Get());

        // Indicate a state transition on the resource usage.
        transition = CD3DX12_RESOURCE_BARRIER::Transition(CurrentBackBuffer(),
            D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
        mCommandList->ResourceBarrier(1, &transition);

        // Done recording commands.
        ThrowIfFailed(mCommandList->Close());

        mLinearAllocator->Commit(mCommandQueue.Get());

        // Add the command list to the queue for execution.
        auto cmdsLists = std::array{ static_cast<ID3D12CommandList*>(mCommandList.Get()) };
        mCommandQueue->ExecuteCommandLists(static_cast<UINT>(cmdsLists.size()), cmdsLists.data());

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

    virtual void UpdateImgui(const GameTimer& gt)override
    {
        D3DApp::UpdateImgui(gt);

        //
        // Define a panel to render GUI elements.
        ImGui::Begin("Options");

        ImGui::Text("Application average %.3f ms/frame (%.1f FPS)", 1000.0f / ImGui::GetIO().Framerate, ImGui::GetIO().Framerate);

        ImGui::Checkbox("Wireframe", &mDrawWireframe);
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
    virtual void OnMouseDown(WPARAM btnState, int x, int y)override
    {
        if (auto& io = ImGui::GetIO(); not io.WantCaptureMouse)
        {
            mLastMousePos.x = x;
            mLastMousePos.y = y;
            Win32::SetCapture(mhMainWnd);
        }
    }
    virtual void OnMouseUp(WPARAM btnState, int x, int y)override
    {
        if (auto& io = ImGui::GetIO(); not io.WantCaptureMouse)
            Win32::ReleaseCapture();
    }
    virtual void OnMouseMove(WPARAM btnState, int x, int y)override
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

    void OnKeyboardInput(const GameTimer& gt)
    {}
    void AnimateMaterials(const GameTimer& gt)
    {
        auto& matLib = MaterialLib::GetLib();

        // Scroll the water material texture coordinates.
        auto waterMat = matLib["water"];

        auto& tu = waterMat->MatTransform(3, 0);
        auto& tv = waterMat->MatTransform(3, 1);

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
        auto pos = DirectX::XMVectorSet(mEyePos.x, mEyePos.y, mEyePos.z, 1.0f);
        auto target = DirectX::XMVectorZero();
        auto up = DirectX::XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
        auto view = DirectX::XMMatrixLookAtLH(pos, target, up);
        DirectX::XMStoreFloat4x4(&mView, view);
    }
    void UpdatePerObjectCB(const GameTimer& gt)
    {
        // Update per object constants once per frame so the data can be shared across different render passes.
        for (auto& ri : mAllRitems)
        {
            DirectX::XMStoreFloat4x4(&ri->ObjectConstants.gWorld, DirectX::XMMatrixTranspose(DirectX::XMLoadFloat4x4(&ri->World)));
            DirectX::XMStoreFloat4x4(&ri->ObjectConstants.gTexTransform, DirectX::XMMatrixTranspose(DirectX::XMLoadFloat4x4(&ri->TexTransform)));
            ri->ObjectConstants.gMaterialIndex = ri->Mat->MatIndex;
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
            if (mat->NumFramesDirty > 0)
            {
                auto matTransform = DirectX::XMMATRIX{ DirectX::XMLoadFloat4x4(&mat->MatTransform) };
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
        auto verts = std::vector<ModelVertex>(mWaves->VertexCount());
        for (int i = 0; i < mWaves->VertexCount(); ++i)
        {
            auto pos = DirectX::XMFLOAT3{ mWaves->Position(i) };
            verts[i].Pos = pos;
            verts[i].Normal = mWaves->Normal(i);
            // Derive tex-coords from position by 
            // mapping [-w/2,w/2] --> [0,1]
            verts[i].TexC.x = 0.5f + pos.x / mWaves->Width();
            verts[i].TexC.y = 0.5f - pos.z / mWaves->Depth();

            // Not used in this demo.
            verts[i].TangentU = DirectX::XMFLOAT3{ 0.0f, 0.0f, 0.0f };
        }
        currWavesVB->CopyData(verts.data(), static_cast<UINT>(verts.size()));

        // Set the dynamic VB of the wave renderitem to the current frame VB.
        mWavesRitem->Geo->VertexBufferGPU = currWavesVB->Resource();
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

            auto hDescriptor = D3D12::CD3DX12_CPU_DESCRIPTOR_HANDLE{ cbvSrvUavHeap.CpuHandle(tex->BindlessIndex) };
            auto texResource = static_cast<ID3D12Resource*>(tex->Resource.Get());
            if (tex->IsCubeMap)
                CreateSrvCube(md3dDevice.Get(), texResource, texResource->GetDesc().Format, texResource->GetDesc().MipLevels, hDescriptor);
            else
                CreateSrv2d(md3dDevice.Get(), texResource, texResource->GetDesc().Format, texResource->GetDesc().MipLevels, hDescriptor);
        }
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
        auto rootSigDesc = D3D12::CD3DX12_ROOT_SIGNATURE_DESC{
            GFX_ROOT_ARG_COUNT,
            gfxRootParameters.data(),
            0, 
            nullptr,
            D3D12::D3D12_ROOT_SIGNATURE_FLAGS{
                D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT |
                D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED |
                D3D12_ROOT_SIGNATURE_FLAG_SAMPLER_HEAP_DIRECTLY_INDEXED
            }
        };

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
			__uuidof(D3D12::ID3D12RootSignature), 
            &mRootSignature
        ));
    }
    void BuildShadersAndInputLayout()
    {
        if constexpr (IsDebugBuild)
        {
            mShaders["standardVS"] = d3dUtil::CompileShader(L"Shaders\\BasicBlend.hlsl", { L"-E", L"VS", L"-T", L"vs_6_6", DXC::ArgDebug, DXC::ArgSkipOptimizations });
            mShaders["opaquePS"] = d3dUtil::CompileShader(L"Shaders\\BasicBlend.hlsl", { L"-E", L"PS", L"-T", L"ps_6_6", DXC::ArgDebug, DXC::ArgSkipOptimizations });
            mShaders["alphaTestedPS"] = d3dUtil::CompileShader(L"Shaders\\BasicBlend.hlsl", { L"-E", L"PS", L"-T", L"ps_6_6", L"-D ALPHA_TEST=1", DXC::ArgDebug, DXC::ArgSkipOptimizations });
        }
        else
        {
            mShaders["standardVS"] = d3dUtil::CompileShader(L"Shaders\\BasicBlend.hlsl", { L"-E", L"VS", L"-T", L"vs_6_6" });
            mShaders["opaquePS"] = d3dUtil::CompileShader(L"Shaders\\BasicBlend.hlsl", { L"-E", L"PS", L"-T", L"ps_6_6" });
            mShaders["alphaTestedPS"] = d3dUtil::CompileShader(L"Shaders\\BasicBlend.hlsl", { L"-E", L"PS", L"-T", L"ps_6_6", L"-D ALPHA_TEST=1" });
        }

        mInputLayout = {
            { "POSITION", 0, DXGI::DXGI_FORMAT::DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12::D3D12_INPUT_CLASSIFICATION::D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "NORMAL", 0, DXGI::DXGI_FORMAT::DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12::D3D12_INPUT_CLASSIFICATION::D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "TEXCOORD", 0, DXGI::DXGI_FORMAT::DXGI_FORMAT_R32G32_FLOAT, 0, 24, D3D12::D3D12_INPUT_CLASSIFICATION::D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "TANGENT", 0, DXGI::DXGI_FORMAT::DXGI_FORMAT_R32G32B32_FLOAT, 0, 32, D3D12::D3D12_INPUT_CLASSIFICATION::D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
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
            __uuidof(ID3D12PipelineState), &mPSOs["opaque"]));

        auto wireframePsoDesc = basePsoDesc;
        wireframePsoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_WIREFRAME;

        ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(
            &wireframePsoDesc,
            __uuidof(ID3D12PipelineState), &mPSOs["opaque_wireframe"]));

        //
        // PSO for transparent objects
        //

        auto transparentPsoDesc = basePsoDesc;

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
            .RenderTargetWriteMask = D3D12::D3D12_COLOR_WRITE_ENABLE::D3D12_COLOR_WRITE_ENABLE_ALL,
        };

        transparentPsoDesc.BlendState.RenderTarget[0] = transparencyBlendDesc;
        ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(&transparentPsoDesc,
            __uuidof(ID3D12PipelineState), &mPSOs["transparent"]));

        //
        // PSO for alpha tested objects
        auto alphaTestedPsoDesc = basePsoDesc;
        alphaTestedPsoDesc.PS = {
            reinterpret_cast<Win32::BYTE*>(mShaders["alphaTestedPS"]->GetBufferPointer()),
            mShaders["alphaTestedPS"]->GetBufferSize()
        };
        alphaTestedPsoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(&alphaTestedPsoDesc,
			__uuidof(ID3D12PipelineState), &mPSOs["alphaTested"]));
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
        RenderLayer layer, 
        const DirectX::XMFLOAT4X4& world,
        const DirectX::XMFLOAT4X4& texTransform, 
        Material* mat,
        MeshGeometry* geo, 
        SubmeshGeometry& drawArgs
    )
    {
        auto ritem = std::unique_ptr<RenderItem>(
            new RenderItem{
                .World = world,
                .TexTransform = texTransform,
                .Mat = mat,
                .Geo = geo,
                .PrimitiveType = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST,
                .IndexCount = drawArgs.IndexCount,
                .StartIndexLocation = drawArgs.StartIndexLocation,
                .BaseVertexLocation = drawArgs.BaseVertexLocation,
            });
        mRitemLayer[(int)layer].push_back(ritem.get());
        mAllRitems.push_back(std::move(ritem));
    }

    void BuildRenderItems()
    {
        auto& matLib = MaterialLib::GetLib();

        DirectX::XMFLOAT4X4 worldTransform = MathHelper::Identity4x4;
        DirectX::XMFLOAT4X4 texTransform = MathHelper::Identity4x4;

        worldTransform = MathHelper::Identity4x4;
        DirectX::XMStoreFloat4x4(&texTransform, DirectX::XMMatrixScaling(5.0f, 5.0f, 1.0f));
        AddRenderItem(RenderLayer::Transparent, worldTransform, texTransform, matLib["water"], mGeometries["waterGeo"].get(), mGeometries["waterGeo"]->DrawArgs["grid"]);
        mWavesRitem = mAllRitems.back().get();

        worldTransform = MathHelper::Identity4x4;
        DirectX::XMStoreFloat4x4(&texTransform, DirectX::XMMatrixScaling(8.0f, 8.0f, 1.0f));
        AddRenderItem(RenderLayer::Opaque,
            worldTransform,
            texTransform,
            matLib["grass"],
            mGeometries["landGeo"].get(),
            mGeometries["landGeo"]->DrawArgs["grid"]);

        DirectX::XMMATRIX S = DirectX::XMMatrixScaling(8.0f, 8.0f, 8.0f);
        DirectX::XMMATRIX T = DirectX::XMMatrixTranslation(3.0f, 2.0f, -9.0f);
        DirectX::XMStoreFloat4x4(&worldTransform, S * T);
        texTransform = MathHelper::Identity4x4;
        AddRenderItem(RenderLayer::AlphaTested, worldTransform, texTransform, matLib["fence"], mGeometries["shapeGeo"].get(), mGeometries["shapeGeo"]->DrawArgs["box"]);
    }

    void DrawRenderItems(D3D12::ID3D12GraphicsCommandList* cmdList, const std::vector<RenderItem*>& ritems)
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

    auto BuildLandGeometry(D3D12::ID3D12Device* device, DirectX::ResourceUploadBatch& uploadBatch) -> std::unique_ptr<MeshGeometry>
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
            vertices[i].TangentU = DirectX::XMFLOAT3(0.0f, 0.0f, 0.0f);
        }

        const auto indexCount = static_cast<UINT>(grid.Indices32.size());
        const auto indexElementByteSize = UINT{ sizeof(uint16_t) };
        const auto vbByteSize = static_cast<UINT>(vertices.size()) * sizeof(ModelVertex);
        const auto ibByteSize = indexCount * indexElementByteSize;
        const auto indices = std::vector<std::uint16_t>{ grid.GetIndices16() };

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
        geo->VertexBufferByteSize = static_cast<Win32::UINT>(vbByteSize);
        geo->IndexFormat = DXGI_FORMAT_R16_UINT;
        geo->IndexBufferByteSize = static_cast<Win32::UINT>(ibByteSize);

        auto submesh = SubmeshGeometry{
            .IndexCount = static_cast<UINT>(indices.size()),
            .StartIndexLocation = 0,
            .BaseVertexLocation = 0,
            .VertexCount = static_cast<UINT>(vertices.size())
        };
        geo->DrawArgs["grid"] = submesh;

        return geo;
    }
    auto BuildWaveGeometry(D3D12::ID3D12Device* device, DirectX::ResourceUploadBatch& uploadBatch) -> std::unique_ptr<MeshGeometry>
    {
        auto indices = std::vector<std::uint32_t>(3 * mWaves->TriangleCount()); // 3 indices per face

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

        auto vbByteSize = Win32::UINT{ mWaves->VertexCount() * sizeof(ModelVertex) };
        auto ibByteSize = static_cast<Win32::UINT>(indices.size() * sizeof(std::uint32_t));

        auto geo = std::make_unique<MeshGeometry>();
        geo->Name = "waterGeo";

        // Set dynamically every frame in UpdateWaves().
        geo->VertexBufferCPU.clear();
        geo->VertexBufferGPU = nullptr;

        geo->IndexBufferCPU.resize(ibByteSize);
        std::memcpy(geo->IndexBufferCPU.data(), indices.data(), ibByteSize);

        CreateStaticBuffer(
            device, uploadBatch,
            indices.data(),
            indices.size(),
            sizeof(std::uint32_t),
            D3D12_RESOURCE_STATE_INDEX_BUFFER,
            &geo->IndexBufferGPU);

        geo->VertexByteStride = sizeof(ModelVertex);
        geo->VertexBufferByteSize = vbByteSize;
        geo->IndexFormat = DXGI_FORMAT_R32_UINT;
        geo->IndexBufferByteSize = ibByteSize;

        auto submesh = SubmeshGeometry{
            .IndexCount = (UINT)indices.size(),
            .StartIndexLocation = 0,
            .BaseVertexLocation = 0
        };

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
        DirectX::XMFLOAT3 n(
            -0.03f * z * std::cosf(0.1f * x) - 0.3f * std::cosf(0.1f * z),
            1.0f,
            -0.3f * std::sinf(0.1f * x) + 0.03f * x * std::sinf(0.1f * z));

        DirectX::XMVECTOR unitNormal = DirectX::XMVector3Normalize(DirectX::XMLoadFloat3(&n));
        DirectX::XMStoreFloat3(&n, unitNormal);

        return n;
    }

private:

    std::vector<std::unique_ptr<FrameResource>> mFrameResources;
    FrameResource* mCurrFrameResource = nullptr;
    int mCurrFrameResourceIndex = 0;

    Microsoft::WRL::ComPtr<ID3D12RootSignature> mRootSignature = nullptr;

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
    float mWaveSpeed = 8.0f;
    float mWaveDamping = 0.1f;

    bool mFogEnabled = true;
    float mFogStart = 20.0f;
    float mFogEnd = 160.0f;
};