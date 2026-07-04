export module boxgridapp;
import std;
import shared;

struct ObjectConstants
{
    DirectX::XMFLOAT4X4 World = MathHelper::Identity4x4;
};

struct PassConstants
{
    DirectX::XMFLOAT4X4 ViewProj = MathHelper::Identity4x4;
};

enum ROOT_ARG
{
    ROOT_ARG_OBJECT_CBV = 0,
    ROOT_ARG_PASS_CBV,
    ROOT_ARG_COUNT
};

constexpr auto BOX_GRID_SIZE = 3u;
constexpr auto BOX_COUNT = BOX_GRID_SIZE * BOX_GRID_SIZE;

constexpr Win32::UINT CBV_SRV_UAV_HEAP_CAPACITY = 16384;

struct ColorVertex
{
    DirectX::XMFLOAT3 Pos;
    DirectX::XMFLOAT4 Color;
};

export class BoxGridApp : public D3DApp
{
public:
    BoxGridApp(HINSTANCE hInstance)
        : D3DApp(hInstance)
    { }

    ~BoxGridApp()
    {
        if (md3dDevice != nullptr)
            FlushCommandQueue();
    }

	auto Initialize() -> bool override
    {
        if (!D3DApp::Initialize())
            return false;

        // We will upload on the direct queue for the book samples, but 
        // copy queue would be better for real game.
        mUploadBatch->Begin(D3D12_COMMAND_LIST_TYPE_DIRECT);

        BuildBoxGeometry(md3dDevice.Get(), *mUploadBatch.get());

        // Kick off upload work asyncronously.
        std::future<void> result = mUploadBatch->End(mCommandQueue.Get());

        // Other init work...
        BuildCbvSrvUavDescriptorHeap();
        BuildConstantBuffers();
        BuildRootSignature();
        BuildShadersAndInputLayout();
        BuildPSO();

        // Position boxes in a grid in xz-plane.
        const float boxSpacing = 5.0f;
        for (UINT i = 0; i < BOX_GRID_SIZE; ++i)
        {
            for (UINT j = 0; j < BOX_GRID_SIZE; ++j)
            {
                float x = -boxSpacing + j * boxSpacing;
                float z = +boxSpacing - i * boxSpacing;

                DirectX::XMMATRIX W = DirectX::XMMatrixTranslation(x, 0.0f, z);
                DirectX::XMStoreFloat4x4(&mWorld[i * BOX_GRID_SIZE + j], W);
            }
        }

        // Block until the upload work is complete.
        result.wait();

        return true;
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
        DirectX::XMMATRIX P = DirectX::XMMatrixPerspectiveFovLH(0.25f * MathHelper::Pi, AspectRatio(), 1.0f, 1000.0f);
        DirectX::XMStoreFloat4x4(&mProj, P);
    }

    void Update(const GameTimer& gt)override
    {
        // Convert Spherical to Cartesian coordinates.
        float x = mRadius * std::sinf(mPhi) * std::cosf(mTheta);
        float z = mRadius * std::sinf(mPhi) * std::sinf(mTheta);
        float y = mRadius * std::cosf(mPhi);

        // Build the view matrix.
        DirectX::XMVECTOR pos = DirectX::XMVectorSet(x, y, z, 1.0f);
        DirectX::XMVECTOR target = DirectX::XMVectorZero();
        DirectX::XMVECTOR up = DirectX::XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);

        DirectX::XMMATRIX view = DirectX::XMMatrixLookAtLH(pos, target, up);
        DirectX::XMStoreFloat4x4(&mView, view);

        // Update the per-object buffer with the latest world matrix.
        for (UINT i = 0; i < BOX_COUNT; ++i)
        {
            DirectX::XMMATRIX world = DirectX::XMLoadFloat4x4(&mWorld[i]);

            ObjectConstants objConstants;
            DirectX::XMStoreFloat4x4(&objConstants.World, DirectX::XMMatrixTranspose(world));
            mObjectCB->CopyData(i, objConstants);
        }

        // Update the per-pass buffer with the latest viewProj matrix.

        DirectX::XMMATRIX proj = DirectX::XMLoadFloat4x4(&mProj);
        DirectX::XMMATRIX viewProj = view * proj;

        PassConstants passConstants;
        DirectX::XMStoreFloat4x4(&passConstants.ViewProj, DirectX::XMMatrixTranspose(viewProj));
        mPassCB->CopyData(0, passConstants);
    }

    void Draw(const GameTimer& gt)override
    {
        CbvSrvUavHeap& cbvSrvUavHeap = CbvSrvUavHeap::Get();

        UpdateImgui(gt);

        // Reuse the memory associated with command recording.
        // We can only reset when the associated command lists have finished execution on the GPU.
        ThrowIfFailed(mDirectCmdListAlloc->Reset());

        // A command list can be reset after it has been added to the command queue via ExecuteCommandList.
        // Reusing the command list reuses memory.
        ThrowIfFailed(mCommandList->Reset(mDirectCmdListAlloc.Get(), mSolidPSO.Get()));

        auto descriptorHeaps = std::array{ cbvSrvUavHeap.GetD3dHeap() };
        mCommandList->SetDescriptorHeaps(static_cast<UINT>(descriptorHeaps.size()), descriptorHeaps.data());

        mCommandList->RSSetViewports(1, &mScreenViewport);
        mCommandList->RSSetScissorRects(1, &mScissorRect);

        // Indicate a state transition on the resource usage.
        auto transition = D3D12::CD3DX12_RESOURCE_BARRIER::Transition(CurrentBackBuffer(),
            D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
        mCommandList->ResourceBarrier(1, &transition);

        // Clear the back buffer and depth buffer.
        mCommandList->ClearRenderTargetView(CurrentBackBufferView(), DirectX::Colors::LightSteelBlue, 0, nullptr);
        mCommandList->ClearDepthStencilView(DepthStencilView(), D3D12::D3D12_CLEAR_FLAGS{D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL
        }, 1.0f, 0, 0, nullptr);

        // Specify the buffers we are going to render to.
        auto cbbv = CurrentBackBufferView();
        auto dsv = DepthStencilView();
        mCommandList->OMSetRenderTargets(1, &cbbv, true, &dsv);

        mCommandList->SetPipelineState(mDrawWireframe ? mWireframePSO.Get() : mSolidPSO.Get());
        mCommandList->SetGraphicsRootSignature(mRootSignature.Get());

        mCommandList->SetGraphicsRootDescriptorTable(
            ROOT_ARG_PASS_CBV,
            cbvSrvUavHeap.GpuHandle(mPassCBHeapIndex));

        auto vbv = mBoxGeo->VertexBufferView();
        auto ibv = mBoxGeo->IndexBufferView();
        mCommandList->IASetVertexBuffers(0, 1, &vbv);
        mCommandList->IASetIndexBuffer(&ibv);
        mCommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

        for (UINT i = 0; i < BOX_COUNT; ++i)
        {
            mCommandList->SetGraphicsRootDescriptorTable(
                ROOT_ARG_OBJECT_CBV,
                cbvSrvUavHeap.GpuHandle(mBoxCBHeapIndex[i]));

            mCommandList->DrawIndexedInstanced(
                mBoxGeo->DrawArgs["box"].IndexCount,
                1, // instanceCount
                mBoxGeo->DrawArgs["box"].StartIndexLocation,
                mBoxGeo->DrawArgs["box"].BaseVertexLocation,
                0); // startInstanceLocation
        }

        // Draw imgui UI.
        ImGui::ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), mCommandList.Get());

        // Indicate a state transition on the resource usage.
        transition = D3D12::CD3DX12_RESOURCE_BARRIER::Transition(CurrentBackBuffer(),
            D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
        mCommandList->ResourceBarrier(1, &transition);

        // Done recording commands.
        ThrowIfFailed(mCommandList->Close());

        // Add the command list to the queue for execution.
        std::array<D3D12::ID3D12CommandList*, 1> cmdsLists = { mCommandList.Get() };
        mCommandQueue->ExecuteCommandLists(static_cast<UINT>(cmdsLists.size()), cmdsLists.data());

        // Swap the back and front buffers
        DXGI::DXGI_PRESENT_PARAMETERS presentParams = { 0 };
        ThrowIfFailed(mSwapChain->Present1(0, 0, &presentParams));
        mCurrBackBuffer = (mCurrBackBuffer + 1) % SwapChainBufferCount;

        // Wait until frame commands are complete.  This waiting is inefficient and is
        // done for simplicity.  Later we will show how to organize our rendering code
        // so we do not have to wait per frame.
        FlushCommandQueue();
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

        DirectX::GraphicsMemoryStatistics gfxMemStats = DirectX::GraphicsMemory::Get(md3dDevice.Get()).GetStatistics();

        if (ImGui::CollapsingHeader("VideoMemoryInfo"))
        {
            static float vidMemPollTime = 0.0f;
            vidMemPollTime += gt.DeltaTime();

            static DXGI::DXGI_QUERY_VIDEO_MEMORY_INFO videoMemInfo;
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
        ImGui::ImGuiIO& io = ImGui::GetIO();

        if (!io.WantCaptureMouse)
        {
            mLastMousePos.x = x;
            mLastMousePos.y = y;

            Win32::SetCapture(mhMainWnd);
        }
    }

    void OnMouseUp(Win32::WPARAM btnState, int x, int y)override
    {
        ImGui::ImGuiIO& io = ImGui::GetIO();

        if (!io.WantCaptureMouse)
        {
            Win32::ReleaseCapture();
        }
    }

    void OnMouseMove(Win32::WPARAM btnState, int x, int y)override
    {
        ImGui::ImGuiIO& io = ImGui::GetIO();

        if (!io.WantCaptureMouse)
        {
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
                float dx = 0.005f * static_cast<float>(x - mLastMousePos.x);
                float dy = 0.005f * static_cast<float>(y - mLastMousePos.y);

                // Update the camera radius based on input.
                mRadius += dx - dy;

                // Restrict the radius.
                mRadius = std::clamp(mRadius, 3.0f, 15.0f);
            }

            mLastMousePos.x = x;
            mLastMousePos.y = y;
        }
    }

    void BuildCbvSrvUavDescriptorHeap()
    {
        CbvSrvUavHeap& cbvSrvUavHeap = CbvSrvUavHeap::Get();
        cbvSrvUavHeap.Init(md3dDevice.Get(), CBV_SRV_UAV_HEAP_CAPACITY);

        InitImgui(cbvSrvUavHeap);
    }

    void BuildConstantBuffers()
    {
        CbvSrvUavHeap& cbvSrvUavHeap = CbvSrvUavHeap::Get();


        const bool isConstantBuffer = true;

        //
        // CB per object
        //

        mObjectCB = std::make_unique<UploadBuffer<ObjectConstants>>(
            md3dDevice.Get(),
            BOX_COUNT,
            isConstantBuffer);

        // Constant buffers must be a multiple of the
        // minimum hardware allocation size (usually 256 bytes).
        UINT objCBByteSize = d3dUtil::CalcConstantBufferByteSize(sizeof(ObjectConstants));

        for (UINT cbObjIndex = 0; cbObjIndex < BOX_COUNT; ++cbObjIndex)
        {
            D3D12::D3D12_GPU_VIRTUAL_ADDRESS objCBAddress = mObjectCB->Resource()->GetGPUVirtualAddress() +
                cbObjIndex * objCBByteSize;

            D3D12::D3D12_CONSTANT_BUFFER_VIEW_DESC cbvObj;
            cbvObj.BufferLocation = objCBAddress;
            cbvObj.SizeInBytes = objCBByteSize;

            mBoxCBHeapIndex[cbObjIndex] = cbvSrvUavHeap.NextFreeIndex();

            md3dDevice->CreateConstantBufferView(
                &cbvObj,
                cbvSrvUavHeap.CpuHandle(mBoxCBHeapIndex[cbObjIndex]));
        }

        //
        // Pass CB
        //

        mPassCBHeapIndex = cbvSrvUavHeap.NextFreeIndex();

        UINT numPassCB = 1;
        mPassCB = std::make_unique<UploadBuffer<PassConstants>>(
            md3dDevice.Get(),
            numPassCB,
            isConstantBuffer);

        UINT passCBByteSize = d3dUtil::CalcConstantBufferByteSize(sizeof(PassConstants));

        int cbPassElementOffset = 0;
        D3D12_GPU_VIRTUAL_ADDRESS passCBAddress = mPassCB->Resource()->GetGPUVirtualAddress() +
            cbPassElementOffset * passCBByteSize;

        D3D12_CONSTANT_BUFFER_VIEW_DESC cbvPassDesc;
        cbvPassDesc.BufferLocation = passCBAddress;
        cbvPassDesc.SizeInBytes = passCBByteSize;

        md3dDevice->CreateConstantBufferView(
            &cbvPassDesc,
            cbvSrvUavHeap.CpuHandle(mPassCBHeapIndex));
    }

    void BuildRootSignature()
    {
        // Shader programs typically require resources as input (constant buffers,
        // textures, samplers).  The root signature defines the resources the shader
        // programs expect.  If we think of the shader programs as a function, and
        // the input resources as function parameters, then the root signature can be
        // thought of as defining the function signature.  

        // Root parameter can be a table, root descriptor or root constants.
        D3D12::CD3DX12_ROOT_PARAMETER slotRootParameter[ROOT_ARG_COUNT] = {};

        // Create a table for per-object constants. Arguments would need to be
        // set once per object.
        D3D12::CD3DX12_DESCRIPTOR_RANGE objectCbvTable;
        Win32::UINT numDescriptors = 1;
        Win32::UINT baseRegister = 0;
        objectCbvTable.Init(D3D12_DESCRIPTOR_RANGE_TYPE_CBV,
            numDescriptors, baseRegister);

        // Create a table for per-pass constants. Arguments would need to be
        // set once per pass.
        D3D12::CD3DX12_DESCRIPTOR_RANGE passCbvTable;
        baseRegister = 1;
        passCbvTable.Init(D3D12_DESCRIPTOR_RANGE_TYPE_CBV,
            numDescriptors, baseRegister);

        slotRootParameter[ROOT_ARG_OBJECT_CBV].InitAsDescriptorTable(1, &objectCbvTable);
        slotRootParameter[ROOT_ARG_PASS_CBV].InitAsDescriptorTable(1, &passCbvTable);

        // A root signature is an array of root parameters.
        D3D12::CD3DX12_ROOT_SIGNATURE_DESC rootSigDesc(
            ROOT_ARG_COUNT,
            slotRootParameter,
            0, nullptr,
            D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

        // create a root signature with a single slot which points to a descriptor range consisting of a single constant buffer
        Microsoft::WRL::ComPtr<D3D::ID3DBlob> serializedRootSig = nullptr;
        Microsoft::WRL::ComPtr<D3D::ID3DBlob> errorBlob = nullptr;
        Win32::HRESULT hr = D3D12::D3D12SerializeRootSignature(
            &rootSigDesc,
            D3D::D3D_ROOT_SIGNATURE_VERSION::D3D_ROOT_SIGNATURE_VERSION_1,
            serializedRootSig.GetAddressOf(),
            errorBlob.GetAddressOf());

        if (errorBlob != nullptr)
        {
            Win32::OutputDebugStringA((char*)errorBlob->GetBufferPointer());
        }
        ThrowIfFailed(hr);

        ThrowIfFailed(md3dDevice->CreateRootSignature(
            0,
            serializedRootSig->GetBufferPointer(),
            serializedRootSig->GetBufferSize(),
			__uuidof(D3D12::ID3D12RootSignature), &mRootSignature));
    }

    void BuildShadersAndInputLayout()
    {
#if defined(DEBUG) || defined(_DEBUG)  
#define COMMA_DEBUG_ARGS ,DXC::ArgDebug, DXC::ArgSkipOptimizations
#else
#define COMMA_DEBUG_ARGS
#endif

        std::vector<LPCWSTR> vsArgs = std::vector<LPCWSTR>{ L"-E", L"VS", L"-T", L"vs_6_6" COMMA_DEBUG_ARGS };
        std::vector<LPCWSTR> psArgs = std::vector<LPCWSTR>{ L"-E", L"PS", L"-T", L"ps_6_6" COMMA_DEBUG_ARGS };

        mvsByteCode = d3dUtil::CompileShader(L"Shaders\\BasicColor.hlsl", vsArgs);
        mpsByteCode = d3dUtil::CompileShader(L"Shaders\\BasicColor.hlsl", psArgs);

        mInputLayout =
        {
            { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }
        };
    }

    void BuildBoxGeometry(ID3D12Device* device, DirectX::ResourceUploadBatch& uploadBatch)
    {
        auto vertices = std::array<ColorVertex, 8>{
            ColorVertex({ DirectX::XMFLOAT3(-1.0f, -1.0f, -1.0f), DirectX::XMFLOAT4(DirectX::Colors::White) }),
            ColorVertex({ DirectX::XMFLOAT3(-1.0f, +1.0f, -1.0f), DirectX::XMFLOAT4(DirectX::Colors::Black) }),
            ColorVertex({ DirectX::XMFLOAT3(+1.0f, +1.0f, -1.0f), DirectX::XMFLOAT4(DirectX::Colors::Red) }),
            ColorVertex({ DirectX::XMFLOAT3(+1.0f, -1.0f, -1.0f), DirectX::XMFLOAT4(DirectX::Colors::Green) }),
            ColorVertex({ DirectX::XMFLOAT3(-1.0f, -1.0f, +1.0f), DirectX::XMFLOAT4(DirectX::Colors::Blue) }),
            ColorVertex({ DirectX::XMFLOAT3(-1.0f, +1.0f, +1.0f), DirectX::XMFLOAT4(DirectX::Colors::Yellow) }),
            ColorVertex({ DirectX::XMFLOAT3(+1.0f, +1.0f, +1.0f), DirectX::XMFLOAT4(DirectX::Colors::Cyan) }),
            ColorVertex({ DirectX::XMFLOAT3(+1.0f, -1.0f, +1.0f), DirectX::XMFLOAT4(DirectX::Colors::Magenta) })
        };

        auto indices = std::array<std::uint16_t, 36>{
            // front face
            0, 1, 2,
            0, 2, 3,

            // back face
            4, 6, 5,
            4, 7, 6,

            // left face
            4, 5, 1,
            4, 1, 0,

            // right face
            3, 2, 6,
            3, 6, 7,

            // top face
            1, 5, 6,
            1, 6, 2,

            // bottom face
            4, 0, 3,
            4, 3, 7
        };

        const auto vbByteSize = (UINT)vertices.size() * sizeof(ColorVertex);
        const auto ibByteSize = (UINT)indices.size() * sizeof(uint16_t);

        mBoxGeo = std::make_unique<MeshGeometry>();
        mBoxGeo->Name = "boxGeo";

        mBoxGeo->VertexBufferCPU.resize(vbByteSize);
        std::memcpy(mBoxGeo->VertexBufferCPU.data(), vertices.data(), vbByteSize);

        mBoxGeo->IndexBufferCPU.resize(ibByteSize);
        std::memcpy(mBoxGeo->IndexBufferCPU.data(), indices.data(), ibByteSize);

        CreateStaticBuffer(
            device, uploadBatch,
            vertices.data(), vertices.size(), sizeof(ColorVertex),
            D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER,
            &mBoxGeo->VertexBufferGPU);

        CreateStaticBuffer(
            device, uploadBatch,
            indices.data(), indices.size(), sizeof(uint16_t),
            D3D12_RESOURCE_STATE_INDEX_BUFFER,
            &mBoxGeo->IndexBufferGPU);

        mBoxGeo->VertexByteStride = sizeof(ColorVertex);
        mBoxGeo->VertexBufferByteSize = vbByteSize;
        mBoxGeo->IndexFormat = DXGI_FORMAT_R16_UINT;
        mBoxGeo->IndexBufferByteSize = ibByteSize;

        SubmeshGeometry submesh;
        submesh.IndexCount = (UINT)indices.size();
        submesh.StartIndexLocation = 0;
        submesh.BaseVertexLocation = 0;
        submesh.VertexCount = 8;

        // Box that tightly contains all the geometry. This 
        // is used in later chapters of the book.
        submesh.Bounds = DirectX::BoundingBox(
            DirectX::XMFLOAT3(0.0f, 0.0f, 0.0f), // center
            DirectX::XMFLOAT3(1.0f, 1.0f, 1.0f));// extents

        mBoxGeo->DrawArgs["box"] = submesh;
    }

    void BuildPSO()
    {
        D3D12::D3D12_GRAPHICS_PIPELINE_STATE_DESC basePsoDesc = d3dUtil::InitDefaultPso(
            mBackBufferFormat,
            mDepthStencilFormat,
            mInputLayout,
            mRootSignature.Get(),
            mvsByteCode.Get(), mpsByteCode.Get());

        ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(
            &basePsoDesc,
			__uuidof(D3D12::ID3D12PipelineState), &mSolidPSO));

        D3D12::D3D12_GRAPHICS_PIPELINE_STATE_DESC wireframePsoDesc = basePsoDesc;
        wireframePsoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_WIREFRAME;

        ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(
            &wireframePsoDesc,
            __uuidof(D3D12::ID3D12PipelineState), &mWireframePSO));
    }

private:

    Microsoft::WRL::ComPtr<D3D12::ID3D12RootSignature> mRootSignature = nullptr;

    uint32_t mBoxCBHeapIndex[BOX_COUNT];
    std::unique_ptr<UploadBuffer<ObjectConstants>> mObjectCB = nullptr;

    uint32_t mPassCBHeapIndex = -1;
    std::unique_ptr<UploadBuffer<PassConstants>> mPassCB = nullptr;

    std::unique_ptr<MeshGeometry> mBoxGeo = nullptr;

    Microsoft::WRL::ComPtr<DXC::IDxcBlob> mvsByteCode = nullptr;
    Microsoft::WRL::ComPtr<DXC::IDxcBlob> mpsByteCode = nullptr;

    std::vector<D3D12::D3D12_INPUT_ELEMENT_DESC> mInputLayout;

    Microsoft::WRL::ComPtr<D3D12::ID3D12PipelineState> mSolidPSO = nullptr;
    Microsoft::WRL::ComPtr<D3D12::ID3D12PipelineState> mWireframePSO = nullptr;

    DirectX::XMFLOAT4X4 mWorld[BOX_COUNT];
    DirectX::XMFLOAT4X4 mView = MathHelper::Identity4x4;
    DirectX::XMFLOAT4X4 mProj = MathHelper::Identity4x4;

    float mTheta = 1.25f * DirectX::Pi;
    float mPhi = 0.25f * DirectX::Pi;
    float mRadius = 15.0f;

    Win32::POINT mLastMousePos;

    bool mDrawWireframe = false;
};