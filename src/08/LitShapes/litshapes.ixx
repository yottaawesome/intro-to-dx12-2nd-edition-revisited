module;

#include "../../Shaders/SharedTypes.h"

export module litshapes;
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

// Stores the resources needed for the CPU to build the command lists
// for a frame.  
struct FrameResource
{
public:
    FrameResource(D3D12::ID3D12Device* device, Win32::UINT passCount, Win32::UINT materialCount)
    {
        ThrowIfFailed(device->CreateCommandAllocator(
            D3D12_COMMAND_LIST_TYPE_DIRECT,
            __uuidof(D3D12::ID3D12CommandAllocator),
            &CmdListAlloc));

        PassCB = std::make_unique<UploadBuffer<PerPassCB>>(device, passCount, true);
        MaterialBuffer = std::make_unique<UploadBuffer<MaterialData>>(device, materialCount, false);
    }
    FrameResource(const FrameResource& rhs) = delete;
    FrameResource& operator=(const FrameResource& rhs) = delete;

    // We cannot reset the allocator until the GPU is done processing the commands.
    // So each frame needs their own allocator.
    Microsoft::WRL::ComPtr<D3D12::ID3D12CommandAllocator> CmdListAlloc;

    // We cannot update a buffer until the GPU is done processing the commands
    // that reference it.  So each frame needs their own buffers.
    std::unique_ptr<UploadBuffer<PerPassCB>> PassCB = nullptr;
    std::unique_ptr<UploadBuffer<MaterialData>> MaterialBuffer = nullptr;

    // Fence value to mark commands up to this fence point.  This lets us
    // check if these frame resources are still in use by the GPU.
    Win32::UINT64 Fence = 0;
};

enum class RenderLayer : int
{
    Opaque = 0,
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

    PerObjectCB ObjectConstants;

    // Handle to memory in linear allocator.
    DirectX::GraphicsResource MemHandleToObjectCB;

    Material* Mat = nullptr;
    MeshGeometry* Geo = nullptr;

    // Primitive topology.
    D3D::D3D_PRIMITIVE_TOPOLOGY PrimitiveType = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;

    // DrawIndexedInstanced parameters.
    UINT IndexCount = 0;
    UINT StartIndexLocation = 0;
    int BaseVertexLocation = 0;
};

export class LitShapesApp : public D3DApp
{
public:
    LitShapesApp(HINSTANCE hInstance)
        : D3DApp(hInstance) 
    {
        Initialize();
    }
    LitShapesApp(const LitShapesApp& rhs) = delete;
    LitShapesApp& operator=(const LitShapesApp& rhs) = delete;
    ~LitShapesApp()
    {
        if (md3dDevice != nullptr)
            FlushCommandQueue();
    }

private:
    void Initialize() override
    {
        D3DApp::Initialize();

        // We will upload on the direct queue for the book samples, but 
        // copy queue would be better for real game.
        mUploadBatch->Begin(D3D12_COMMAND_LIST_TYPE_DIRECT);

        auto shapeGeo = d3dUtil::BuildShapeGeometry(md3dDevice.Get(), *mUploadBatch.get());
        mGeometries[shapeGeo->Name] = std::move(shapeGeo);

        auto skullGeo = d3dUtil::BuildSkullGeometry(md3dDevice.Get(), *mUploadBatch.get());
        mGeometries[skullGeo->Name] = std::move(skullGeo);

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
    }

    void Draw(const GameTimer& gt)override
    {
        auto& cbvSrvUavHeap = CbvSrvUavHeap::Get();

        UpdateImgui(gt);

        auto cmdListAlloc = mCurrFrameResource->CmdListAlloc;

        // Reuse the memory associated with command recording.
        // We can only reset when the associated command lists have finished execution on the GPU.
        ThrowIfFailed(cmdListAlloc->Reset());

        // A command list can be reset after it has been added to the command queue via ExecuteCommandList.
        // Reusing the command list reuses memory.
        ThrowIfFailed(mCommandList->Reset(cmdListAlloc.Get(), mPSOs["opaque"].Get()));

        auto descriptorHeaps = std::array<ID3D12DescriptorHeap*, 1>{ cbvSrvUavHeap.GetD3dHeap() };
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
        mCommandList->ClearRenderTargetView(CurrentBackBufferView(), DirectX::Colors::LightSteelBlue, 0, nullptr);
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
        auto cmdsLists = std::array<ID3D12CommandList*, 1>{ mCommandList.Get() };
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

    void UpdateImgui(const GameTimer& gt)override
    {
        D3DApp::UpdateImgui(gt);

        //
        // Define a panel to render GUI elements.
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

            // Update angles based on input to orbit camera around box.
            mTheta += dx;
            mPhi += dy;

            // Restrict the angle mPhi.
            mPhi = std::clamp(mPhi, 0.1f, MathHelper::Pi - 0.1f);
        }
        else if ((btnState & Win32::MK::RButton) != 0)
        {
            // Make each pixel correspond to 0.005 unit in the scene.
            auto dx = 0.005f * static_cast<float>(x - mLastMousePos.x);
            auto dy = 0.005f * static_cast<float>(y - mLastMousePos.y);

            // Update the camera radius based on input.
            mRadius += dx - dy;

            // Restrict the radius.
            mRadius = std::clamp(mRadius, 5.0f, 25.0f);
        }

        mLastMousePos.x = x;
        mLastMousePos.y = y;
    }

    void OnKeyboardInput(const GameTimer& gt)
    {}

    void AnimateMaterials(const GameTimer& gt)
    {}

    void UpdateCamera(const GameTimer& gt)
    {
        // Convert Spherical to Cartesian coordinates.
        mEyePos.x = mRadius * sinf(mPhi) * cosf(mTheta);
        mEyePos.z = mRadius * sinf(mPhi) * sinf(mTheta);
        mEyePos.y = mRadius * cosf(mPhi);

        // Build the view matrix.
        auto pos = DirectX::XMVECTOR{ DirectX::XMVectorSet(mEyePos.x, mEyePos.y, mEyePos.z, 1.0f) };
        auto target = DirectX::XMVECTOR{ DirectX::XMVectorZero() };
        auto up = DirectX::XMVECTOR{ DirectX::XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f) };

        auto view = DirectX::XMMATRIX{DirectX::XMMatrixLookAtLH(pos, target, up)};
        DirectX::XMStoreFloat4x4(&mView, view);
    }

    void UpdatePerObjectCB(const GameTimer& gt)
    {
        // Update per object constants once per frame so the data can be shared across different render passes.
        for (auto& ri : mAllRitems)
        {
            XMStoreFloat4x4(&ri->ObjectConstants.gWorld, XMMatrixTranspose(XMLoadFloat4x4(&ri->World)));
            XMStoreFloat4x4(&ri->ObjectConstants.gTexTransform, XMMatrixTranspose(XMLoadFloat4x4(&ri->TexTransform)));
            ri->ObjectConstants.gMaterialIndex = ri->Mat->MatIndex;

            // Need to hold handle until we submit work to GPU.
            ri->MemHandleToObjectCB = mLinearAllocator->AllocateConstant(ri->ObjectConstants);
        }
    }

    void UpdateMaterialBuffer(const GameTimer& gt)
    {
        auto currMaterialBuffer = mCurrFrameResource->MaterialBuffer.get();
        for (auto& e : mMaterials)
        {
            // Only update the buffer data if the data has changed.  If the buffer
            // data changes, it needs to be updated for each FrameResource.
            Material* mat = e.second.get();
            if (mat->NumFramesDirty > 0)
            {
                auto matTransform = DirectX::XMMATRIX{ DirectX::XMLoadFloat4x4(&mat->MatTransform) };

                auto matData = MaterialData{
					.DiffuseAlbedo = mat->DiffuseAlbedo,
					.FresnelR0 = mat->FresnelR0,
					.Roughness = mat->Roughness,
                };

                currMaterialBuffer->CopyData(mat->MatIndex, matData);

                // Next FrameResource need to be updated too.
                mat->NumFramesDirty--;
            }
        }
    }

    void UpdateMainPassCB(const GameTimer& gt)
    {
        mMainPassCB = {};
        //ZeroMemory(&mMainPassCB, sizeof(mMainPassCB));

        auto view = DirectX::XMMATRIX{ XMLoadFloat4x4(&mView) };
        auto proj = DirectX::XMMATRIX{ XMLoadFloat4x4(&mProj) };

        auto viewProj = DirectX::XMMATRIX{ DirectX::XMMatrixMultiply(view, proj) };
        auto detView = DirectX::XMVECTOR{ DirectX::XMMatrixDeterminant(view) };
        auto detProj = DirectX::XMVECTOR{ DirectX::XMMatrixDeterminant(proj) };
        auto invView = DirectX::XMMATRIX{ DirectX::XMMatrixInverse(&detView, view) };
        auto invProj = DirectX::XMMATRIX{ DirectX::XMMatrixInverse(&detProj, proj) };
        auto detViewProj = DirectX::XMVECTOR{ DirectX::XMMatrixDeterminant(viewProj) };
        auto invViewProj = DirectX::XMMATRIX{ DirectX::XMMatrixInverse(&detViewProj, viewProj)};

        DirectX::XMStoreFloat4x4(&mMainPassCB.gView, DirectX::XMMatrixTranspose(view));
        DirectX::XMStoreFloat4x4(&mMainPassCB.gInvView, DirectX::XMMatrixTranspose(invView));
        DirectX::XMStoreFloat4x4(&mMainPassCB.gProj, DirectX::XMMatrixTranspose(proj));
        DirectX::XMStoreFloat4x4(&mMainPassCB.gInvProj, DirectX::XMMatrixTranspose(invProj));
        DirectX::XMStoreFloat4x4(&mMainPassCB.gViewProj, DirectX::XMMatrixTranspose(viewProj));
        DirectX::XMStoreFloat4x4(&mMainPassCB.gInvViewProj, DirectX::XMMatrixTranspose(invViewProj));
        mMainPassCB.gEyePosW = mEyePos;
        mMainPassCB.gRenderTargetSize = DirectX::XMFLOAT2{(float)mClientWidth, (float)mClientHeight};
        mMainPassCB.gInvRenderTargetSize = DirectX::XMFLOAT2{1.0f / mClientWidth, 1.0f / mClientHeight};
        mMainPassCB.gNearZ = 1.0f;
        mMainPassCB.gFarZ = 1000.0f;
        mMainPassCB.gTotalTime = gt.TotalTime();
        mMainPassCB.gDeltaTime = gt.DeltaTime();
        mMainPassCB.gAmbientLight = { 0.25f, 0.25f, 0.35f, 1.0f };

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

    void BuildCbvSrvUavDescriptorHeap()
    {
        auto& cbvSrvUavHeap = CbvSrvUavHeap::Get();
        cbvSrvUavHeap.Init(md3dDevice.Get(), CBV_SRV_UAV_HEAP_CAPACITY);

        InitImgui(cbvSrvUavHeap);
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
            0, nullptr,
            D3D12::D3D12_ROOT_SIGNATURE_FLAGS::D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT};

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
    }
    void BuildShadersAndInputLayout()
    {
        if constexpr (IsDebugBuild)
        {
            mShaders["standardVS"] = d3dUtil::CompileShader(L"Shaders\\BasicLit.hlsl", { L"-E", L"VS", L"-T", L"vs_6_6", DXC::ArgDebug, DXC::ArgSkipOptimizations });
            mShaders["opaquePS"] = d3dUtil::CompileShader(L"Shaders\\BasicLit.hlsl", { L"-E", L"PS", L"-T", L"ps_6_6", DXC::ArgDebug, DXC::ArgSkipOptimizations });
        }
        else
        {
            mShaders["standardVS"] = d3dUtil::CompileShader(L"Shaders\\BasicLit.hlsl", { L"-E", L"VS", L"-T", L"vs_6_6" });
            mShaders["opaquePS"] = d3dUtil::CompileShader(L"Shaders\\BasicLit.hlsl", { L"-E", L"PS", L"-T", L"ps_6_6" });
        }

        mInputLayout = {
            { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "TANGENT", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 32, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
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

        if (auto hr = md3dDevice->CreateGraphicsPipelineState(&basePsoDesc, __uuidof(D3D12::ID3D12PipelineState), &mPSOs["opaque"]); Win32::Failed(hr))
            throw DxException{ hr };

        auto wireframePsoDesc = D3D12::D3D12_GRAPHICS_PIPELINE_STATE_DESC{basePsoDesc};
        wireframePsoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_WIREFRAME;

        if (auto hr = md3dDevice->CreateGraphicsPipelineState(&wireframePsoDesc, __uuidof(D3D12::ID3D12PipelineState), &mPSOs["opaque_wireframe"]); Win32::Failed(hr))
            throw DxException{ hr };
    }

    void BuildFrameResources()
    {
        constexpr auto passCount = 1u;
        for (auto i = 0; i < gNumFrameResources; ++i)
            mFrameResources.push_back(std::make_unique<FrameResource>(md3dDevice.Get(), passCount, static_cast<UINT>(mMaterials.size())));
    }

    void BuildMaterials()
    {
        int matIndex = 0;

        auto AddMaterial = 
            [&matIndex, this](
                const std::string& name,
                const DirectX::XMFLOAT4& diffuse,
                const DirectX::XMFLOAT3& fresnel,
                float roughness
            )
            {
                auto mat = std::make_unique<Material>();
                mat->Name = name;
                mat->MatIndex = matIndex;
                mat->DiffuseAlbedo = diffuse;
                mat->FresnelR0 = fresnel;
                mat->Roughness = roughness;
                mMaterials[name] = std::move(mat);
                ++matIndex;
            };

        AddMaterial("forestGreen",
            DirectX::XMFLOAT4{DirectX::Colors::ForestGreen},
            DirectX::XMFLOAT3{0.02f, 0.02f, 0.02f},
            0.1f);

        AddMaterial("steelBlue",
            DirectX::XMFLOAT4{DirectX::Colors::SteelBlue},
            DirectX::XMFLOAT3{0.05f, 0.05f, 0.05f},
            0.3f);

        AddMaterial("lightGray",
            DirectX::XMFLOAT4{DirectX::Colors::LightGray},
            DirectX::XMFLOAT3{0.02f, 0.02f, 0.02f},
            0.2f);

        AddMaterial("skullMat",
            DirectX::XMFLOAT4{1.0f, 1.0f, 1.0f, 1.0f},
            DirectX::XMFLOAT3{0.05f, 0.05f, 0.05f},
            0.3f);
    }

    void AddRenderItem(RenderLayer layer, const DirectX::XMFLOAT4X4& world, Material* mat, MeshGeometry* geo, SubmeshGeometry& drawArgs)
    {
        auto ritem = std::make_unique<RenderItem>();
        ritem->World = world;
        ritem->TexTransform = MathHelper::Identity4x4;
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
        auto worldTransform = DirectX::XMFLOAT4X4{};

        DirectX::XMStoreFloat4x4(&worldTransform, DirectX::XMMatrixScaling(2.0f, 1.0f, 2.0f) * DirectX::XMMatrixTranslation(0.0f, 0.5f, 0.0f));
        AddRenderItem(RenderLayer::Opaque, worldTransform, mMaterials["forestGreen"].get(), mGeometries["shapeGeo"].get(), mGeometries["shapeGeo"]->DrawArgs["box"]);

        worldTransform = MathHelper::Identity4x4;
        AddRenderItem(RenderLayer::Opaque, worldTransform, mMaterials["lightGray"].get(), mGeometries["shapeGeo"].get(), mGeometries["shapeGeo"]->DrawArgs["grid"]);

        DirectX::XMStoreFloat4x4(&worldTransform, DirectX::XMMatrixScaling(0.4f, 0.4f, 0.4f) * DirectX::XMMatrixTranslation(0.0f, 1.0f, 0.0f));
        AddRenderItem(RenderLayer::Opaque, worldTransform, mMaterials["skullMat"].get(), mGeometries["skullGeo"].get(), mGeometries["skullGeo"]->DrawArgs["skull"]);

        for (int i = 0; i < 5; ++i)
        {
            DirectX::XMStoreFloat4x4(&worldTransform, DirectX::XMMatrixTranslation(-5.0f, 1.5f, -10.0f + i * 5.0f));
            AddRenderItem(RenderLayer::Opaque, worldTransform, mMaterials["forestGreen"].get(), mGeometries["shapeGeo"].get(), mGeometries["shapeGeo"]->DrawArgs["cylinder"]);

            DirectX::XMStoreFloat4x4(&worldTransform, DirectX::XMMatrixTranslation(+5.0f, 1.5f, -10.0f + i * 5.0f));
            AddRenderItem(RenderLayer::Opaque, worldTransform, mMaterials["forestGreen"].get(), mGeometries["shapeGeo"].get(), mGeometries["shapeGeo"]->DrawArgs["cylinder"]);

            DirectX::XMStoreFloat4x4(&worldTransform, DirectX::XMMatrixTranslation(-5.0f, 3.5f, -10.0f + i * 5.0f));
            AddRenderItem(RenderLayer::Opaque, worldTransform, mMaterials["steelBlue"].get(), mGeometries["shapeGeo"].get(), mGeometries["shapeGeo"]->DrawArgs["sphere"]);

            DirectX::XMStoreFloat4x4(&worldTransform, DirectX::XMMatrixTranslation(+5.0f, 3.5f, -10.0f + i * 5.0f));
            AddRenderItem(RenderLayer::Opaque, worldTransform, mMaterials["steelBlue"].get(), mGeometries["shapeGeo"].get(), mGeometries["shapeGeo"]->DrawArgs["sphere"]);
        }
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
            cmdList->DrawIndexedInstanced(ri->IndexCount, 1, ri->StartIndexLocation, ri->BaseVertexLocation, 0);
        }
    }

private:

    std::vector<std::unique_ptr<FrameResource>> mFrameResources;
    FrameResource* mCurrFrameResource = nullptr;
    int mCurrFrameResourceIndex = 0;

    Microsoft::WRL::ComPtr<ID3D12RootSignature> mRootSignature = nullptr;

    std::unordered_map<std::string, std::unique_ptr<MeshGeometry>> mGeometries;
    std::unordered_map<std::string, Microsoft::WRL::ComPtr<DXC::IDxcBlob>> mShaders;
    std::unordered_map<std::string, std::unique_ptr<Material>> mMaterials;
    std::unordered_map<std::string, Microsoft::WRL::ComPtr<ID3D12PipelineState>> mPSOs;

    std::vector<D3D12::D3D12_INPUT_ELEMENT_DESC> mInputLayout;

    // List of all the render items.
    std::vector<std::unique_ptr<RenderItem>> mAllRitems;

    // Render items divided by PSO.
    std::vector<RenderItem*> mRitemLayer[(int)RenderLayer::Count];

    PerPassCB mMainPassCB;

    DirectX::XMFLOAT4X4 mView = MathHelper::Identity4x4;
    DirectX::XMFLOAT4X4 mProj = MathHelper::Identity4x4;

    DirectX::XMFLOAT3 mEyePos = { 0.0f, 0.0f, 0.0f };
    float mTheta = 1.5f * DirectX::XM_PI;
    float mPhi = 0.35f * DirectX::XM_PI;
    float mRadius = 20.0f;

    float mLightRotationAngle = 0.0f;
    DirectX::XMFLOAT3 mBaseLightDirections[3] = {
        DirectX::XMFLOAT3(0.57735f, -0.57735f, 0.57735f),
        DirectX::XMFLOAT3(-0.57735f, -0.57735f, 0.57735f),
        DirectX::XMFLOAT3(0.0f, -0.707f, -0.707f)
    };
    DirectX::XMFLOAT3 mRotatedLightDirections[3];

    Win32::POINT mLastMousePos;

    bool mDrawWireframe = false;
};