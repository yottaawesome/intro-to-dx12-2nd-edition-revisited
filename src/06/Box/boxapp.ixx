export module boxapp;
import std;
import shared;

constexpr auto CBV_SRV_UAV_HEAP_CAPACITY = 16384u;

struct ColorVertex
{
    DirectX::XMFLOAT3 Pos;
    DirectX::XMFLOAT4 Color;
};

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

export class BoxApp : public D3DApp
{
public:
    BoxApp(Win32::HINSTANCE hInstance)
        : D3DApp(hInstance)
    { }
    BoxApp(const BoxApp& rhs) = delete;
    BoxApp& operator=(const BoxApp& rhs) = delete;
    ~BoxApp()
    {
        if (md3dDevice != nullptr)
            FlushCommandQueue();
    }

	auto Initialize() -> bool override
    {
        if (not D3DApp::Initialize())
            return false;

        // We will upload on the direct queue for the book samples, but 
        // copy queue would be better for real game.
        mUploadBatch->Begin(D3D12::D3D12_COMMAND_LIST_TYPE::D3D12_COMMAND_LIST_TYPE_DIRECT);

        BuildBoxGeometry(md3dDevice.Get(), *mUploadBatch.get());

        // Kick off upload work asyncronously.
        auto result = std::future<void>{mUploadBatch->End(mCommandQueue.Get())};

        // Other init work...
        BuildCbvSrvUavDescriptorHeap();
        BuildConstantBuffers();
        BuildRootSignature();
        BuildShadersAndInputLayout();
        BuildPSO();

        // Block until the upload work is complete.
        result.wait();

        return true;
    }

private:
    void CreateRtvAndDsvDescriptorHeaps()override
    {
        mRtvHeap.Init(md3dDevice.Get(), D3D12::D3D12_DESCRIPTOR_HEAP_TYPE::D3D12_DESCRIPTOR_HEAP_TYPE_RTV, SwapChainBufferCount);
        mDsvHeap.Init(md3dDevice.Get(), D3D12::D3D12_DESCRIPTOR_HEAP_TYPE::D3D12_DESCRIPTOR_HEAP_TYPE_DSV, SwapChainBufferCount);
    }
    void OnResize()override
    {
        D3DApp::OnResize();

        // The window resized, so update the aspect ratio and recompute the projection matrix.
        auto P = DirectX::XMMATRIX{ 
            DirectX::XMMatrixPerspectiveFovLH(
                0.25f * MathHelper::Pi, 
                AspectRatio(), 
                1.0f, 
                1000.0f
            )};
        DirectX::XMStoreFloat4x4(&mProj, P);
    }
    void Update(const GameTimer& gt)override
    {
        // Convert Spherical to Cartesian coordinates.
        auto x = mRadius * std::sinf(mPhi) * std::cosf(mTheta);
        auto z = mRadius * std::sinf(mPhi) * std::sinf(mTheta);
        auto y = mRadius * std::cosf(mPhi);

        // Build the view matrix.
        auto pos = DirectX::XMVECTOR{DirectX::XMVectorSet(x, y, z, 1.0f)};
        auto target = DirectX::XMVECTOR{DirectX::XMVectorZero()};
        auto up = DirectX::XMVECTOR{DirectX::XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f)};

        auto view = DirectX::XMMATRIX{DirectX::XMMatrixLookAtLH(pos, target, up)};
        DirectX::XMStoreFloat4x4(&mView, view);

        auto world = DirectX::XMMATRIX{DirectX::XMLoadFloat4x4(&mWorld)};
        auto proj = DirectX::XMMATRIX{DirectX::XMLoadFloat4x4(&mProj)};
        auto viewProj = view * proj;

        // Update the per-object buffer with the latest world matrix.
        auto objConstants = ObjectConstants{};
        DirectX::XMStoreFloat4x4(&objConstants.World, DirectX::XMMatrixTranspose(world));
        mObjectCB->CopyData(0, objConstants);

        // Update the per-pass buffer with the latest viewProj matrix.
        auto passConstants = PassConstants{};
        DirectX::XMStoreFloat4x4(&passConstants.ViewProj, DirectX::XMMatrixTranspose(viewProj));
        mPassCB->CopyData(0, passConstants);
    }
        
    void Draw(const GameTimer& gt)override
    {
        auto& cbvSrvUavHeap = CbvSrvUavHeap::Get();

        UpdateImgui(gt);

        // Reuse the memory associated with command recording.
        // We can only reset when the associated command lists have finished execution on the GPU.
        ThrowIfFailed(mDirectCmdListAlloc->Reset());

        // A command list can be reset after it has been added to the command queue via ExecuteCommandList.
        // Reusing the command list reuses memory.
        ThrowIfFailed(mCommandList->Reset(mDirectCmdListAlloc.Get(), mSolidPSO.Get()));

		auto descriptorHeaps = std::array{ cbvSrvUavHeap.GetD3dHeap() };
        mCommandList->SetDescriptorHeaps(static_cast<std::uint32_t>(descriptorHeaps.size()), descriptorHeaps.data());

        mCommandList->RSSetViewports(1, &mScreenViewport);
        mCommandList->RSSetScissorRects(1, &mScissorRect);

        // Indicate a state transition on the resource usage.
        auto transition = D3D12::CD3DX12_RESOURCE_BARRIER::Transition(
            CurrentBackBuffer(),
            D3D12::D3D12_RESOURCE_STATES::D3D12_RESOURCE_STATE_PRESENT,
            D3D12::D3D12_RESOURCE_STATES::D3D12_RESOURCE_STATE_RENDER_TARGET
        );
        mCommandList->ResourceBarrier(1, &transition);

        // Clear the back buffer and depth buffer.
        mCommandList->ClearRenderTargetView(CurrentBackBufferView(), DirectX::Colors::LightSteelBlue, 0, nullptr);
        mCommandList->ClearDepthStencilView(
            DepthStencilView(), 
            D3D12::D3D12_CLEAR_FLAGS{ D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL }, 
            1.0f, 
            0, 
            0, 
            nullptr
        );

        // Specify the buffers we are going to render to.
        auto cbbv = CurrentBackBufferView();
		auto dsv = DepthStencilView();
        mCommandList->OMSetRenderTargets(1, &cbbv, true, &dsv);

        mCommandList->SetPipelineState(mDrawWireframe ? mWireframePSO.Get() : mSolidPSO.Get());
        mCommandList->SetGraphicsRootSignature(mRootSignature.Get());

        mCommandList->SetGraphicsRootDescriptorTable(
            ROOT_ARG_OBJECT_CBV,
            cbvSrvUavHeap.GpuHandle(mBoxCBHeapIndex));
        mCommandList->SetGraphicsRootDescriptorTable(
            ROOT_ARG_PASS_CBV,
            cbvSrvUavHeap.GpuHandle(mPassCBHeapIndex));


		auto vbv = mBoxGeo->VertexBufferView();
		auto ibv = mBoxGeo->IndexBufferView();
        mCommandList->IASetVertexBuffers(0, 1, &vbv);
        mCommandList->IASetIndexBuffer(&ibv);
        mCommandList->IASetPrimitiveTopology(D3D::D3D_PRIMITIVE_TOPOLOGY::D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

        mCommandList->DrawIndexedInstanced(
            mBoxGeo->DrawArgs["box"].IndexCount,
            1, // instanceCount
            mBoxGeo->DrawArgs["box"].StartIndexLocation,
            mBoxGeo->DrawArgs["box"].BaseVertexLocation,
            0 // startInstanceLocation
        );

        // Draw imgui UI.
        ImGui::ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), mCommandList.Get());

        // Indicate a state transition on the resource usage.
        transition = D3D12::CD3DX12_RESOURCE_BARRIER::Transition(
            CurrentBackBuffer(),
            D3D12_RESOURCE_STATE_RENDER_TARGET, 
            D3D12_RESOURCE_STATE_PRESENT);
        mCommandList->ResourceBarrier(1, &transition);

        // Done recording commands.
        ThrowIfFailed(mCommandList->Close());

        // Add the command list to the queue for execution.
		auto cmdsLists = std::array<D3D12::ID3D12CommandList*, 1>{ mCommandList.Get() };
        mCommandQueue->ExecuteCommandLists(static_cast<std::uint32_t>(cmdsLists.size()), cmdsLists.data());

        // Swap the back and front buffers
        auto presentParams = DXGI::DXGI_PRESENT_PARAMETERS{ 0 };
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

        auto gfxMemStats = DirectX::GraphicsMemoryStatistics{ DirectX::GraphicsMemory::Get(md3dDevice.Get()).GetStatistics() };

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
        mLastMousePos.x = x;
        mLastMousePos.y = y;
        Win32::SetCapture(mhMainWnd);
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
            auto dx = 0.005f * static_cast<float>(x - mLastMousePos.x);
            auto dy = 0.005f * static_cast<float>(y - mLastMousePos.y);

            // Update the camera radius based on input.
            mRadius += dx - dy;

            // Restrict the radius.
            mRadius = std::clamp(mRadius, 3.0f, 15.0f);
        }

        mLastMousePos.x = x;
        mLastMousePos.y = y;
    }

    void BuildCbvSrvUavDescriptorHeap()
    {
        auto& cbvSrvUavHeap = CbvSrvUavHeap::Get();
        cbvSrvUavHeap.Init(md3dDevice.Get(), CBV_SRV_UAV_HEAP_CAPACITY);

        InitImgui(cbvSrvUavHeap);
    }
    void BuildConstantBuffers()
    {
        auto& cbvSrvUavHeap = CbvSrvUavHeap::Get();

        mBoxCBHeapIndex = cbvSrvUavHeap.NextFreeIndex();
        mPassCBHeapIndex = cbvSrvUavHeap.NextFreeIndex();

        constexpr auto elementCount = 1u;
        constexpr auto isConstantBuffer = true;
        mObjectCB = std::make_unique<UploadBuffer<ObjectConstants>>(
            md3dDevice.Get(),
            elementCount,
            isConstantBuffer);

        mPassCB = std::make_unique<UploadBuffer<PassConstants>>(
            md3dDevice.Get(),
            elementCount,
            isConstantBuffer);

        // Constant buffers must be a multiple of the
        // minimum hardware allocation size (usually 256 bytes).
        constexpr auto objCBByteSize = d3dUtil::CalcConstantBufferByteSize(sizeof(ObjectConstants));
        constexpr auto passCBByteSize = d3dUtil::CalcConstantBufferByteSize(sizeof(PassConstants));

        // In this demo we only have one constant buffer element per upload buffer, 
        // but a buffer could store an array of constant buffers. So, in general, 
        // we need to offset to the ith constant buffer when creating a view to it.
        constexpr auto cbObjElementOffset = 0;
        auto objCBAddress = D3D12::D3D12_GPU_VIRTUAL_ADDRESS{
            mObjectCB->Resource()->GetGPUVirtualAddress() + cbObjElementOffset * objCBByteSize};
        auto cbvObj = D3D12::D3D12_CONSTANT_BUFFER_VIEW_DESC{
            .BufferLocation = objCBAddress,
            .SizeInBytes = objCBByteSize};
        md3dDevice->CreateConstantBufferView(&cbvObj, cbvSrvUavHeap.CpuHandle(mBoxCBHeapIndex));

        constexpr auto cbPassElementOffset = 0;
        auto passCBAddress = D3D12::D3D12_GPU_VIRTUAL_ADDRESS{
            mPassCB->Resource()->GetGPUVirtualAddress() + cbPassElementOffset * passCBByteSize};
        auto cbvPassDesc = D3D12::D3D12_CONSTANT_BUFFER_VIEW_DESC{
            .BufferLocation = passCBAddress,
            .SizeInBytes = passCBByteSize};
        md3dDevice->CreateConstantBufferView(&cbvPassDesc, cbvSrvUavHeap.CpuHandle(mPassCBHeapIndex));
    }
    void BuildRootSignature()
    {
        // Shader programs typically require resources as input (constant buffers,
        // textures, samplers).  The root signature defines the resources the shader
        // programs expect.  If we think of the shader programs as a function, and
        // the input resources as function parameters, then the root signature can be
        // thought of as defining the function signature.  

        // Root parameter can be a table, root descriptor or root constants.
		auto slotRootParameter = std::array<D3D12::CD3DX12_ROOT_PARAMETER, ROOT_ARG_COUNT>{};

        // Create a table for per-object constants. Arguments would need to be
        // set once per object.
        auto objectCbvTable = D3D12::CD3DX12_DESCRIPTOR_RANGE{};
        auto numDescriptors = 1u;
        auto baseRegister = 0u;
        objectCbvTable.Init(D3D12::D3D12_DESCRIPTOR_RANGE_TYPE::D3D12_DESCRIPTOR_RANGE_TYPE_CBV,
            numDescriptors, baseRegister);

        // Create a table for per-pass constants. Arguments would need to be
        // set once per pass.
        auto passCbvTable = D3D12::CD3DX12_DESCRIPTOR_RANGE{};
        baseRegister = 1;
        passCbvTable.Init(D3D12::D3D12_DESCRIPTOR_RANGE_TYPE::D3D12_DESCRIPTOR_RANGE_TYPE_CBV,
            numDescriptors, baseRegister);

        slotRootParameter[ROOT_ARG_OBJECT_CBV].InitAsDescriptorTable(1, &objectCbvTable);
        slotRootParameter[ROOT_ARG_PASS_CBV].InitAsDescriptorTable(1, &passCbvTable);

        // A root signature is an array of root parameters.
        auto rootSigDesc = D3D12::CD3DX12_ROOT_SIGNATURE_DESC{
            ROOT_ARG_COUNT,
            slotRootParameter.data(),
            0, nullptr,
            D3D12::D3D12_ROOT_SIGNATURE_FLAGS::D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT};

        // create a root signature
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
            mvsByteCode = d3dUtil::CompileShader(L"Shaders\\BasicColor.hlsl", { L"-E", L"VS", L"-T", L"vs_6_6", DXC::ArgDebug, DXC::ArgSkipOptimizations });
            mpsByteCode = d3dUtil::CompileShader(L"Shaders\\BasicColor.hlsl", { L"-E", L"PS", L"-T", L"ps_6_6", DXC::ArgDebug, DXC::ArgSkipOptimizations });
		}
        else
        {
            mvsByteCode = d3dUtil::CompileShader(L"Shaders\\BasicColor.hlsl", { L"-E", L"VS", L"-T", L"vs_6_6" });
            mpsByteCode = d3dUtil::CompileShader(L"Shaders\\BasicColor.hlsl", { L"-E", L"PS", L"-T", L"ps_6_6" });
        }

        mInputLayout = {
            { "POSITION", 0, DXGI::DXGI_FORMAT::DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12::D3D12_INPUT_CLASSIFICATION::D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "COLOR", 0, DXGI::DXGI_FORMAT::DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 12, D3D12::D3D12_INPUT_CLASSIFICATION::D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }
        };
    }
    void BuildBoxGeometry(D3D12::ID3D12Device* device, DirectX::ResourceUploadBatch& uploadBatch)
    {
        auto vertices = std::array{
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

        const auto vbByteSize = static_cast<Win32::UINT>(vertices.size() * sizeof(ColorVertex));
        const auto ibByteSize = static_cast<Win32::UINT>(indices.size() * sizeof(uint16_t));

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

        auto submesh = SubmeshGeometry{
            .IndexCount = static_cast<UINT>(indices.size()),
            .StartIndexLocation = 0,
            .BaseVertexLocation = 0,
            .VertexCount = 8
        };

        // Box that tightly contains all the geometry. This 
        // is used in later chapters of the book.
        submesh.Bounds = DirectX::BoundingBox{
            DirectX::XMFLOAT3(0.0f, 0.0f, 0.0f), // center
            DirectX::XMFLOAT3(1.0f, 1.0f, 1.0f)};// extents

        mBoxGeo->DrawArgs["box"] = submesh;
    }
    void BuildPSO()
    {
        auto basePsoDesc = D3D12::D3D12_GRAPHICS_PIPELINE_STATE_DESC{
            d3dUtil::InitDefaultPso(
                mBackBufferFormat,
                mDepthStencilFormat,
                mInputLayout,
                mRootSignature.Get(),
                mvsByteCode.Get(),
                mpsByteCode.Get()
            )};

        ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(
            &basePsoDesc,
            __uuidof(D3D12::ID3D12PipelineState), 
            &mSolidPSO));

        // Create a new PSO based off the default PSO:
        auto wireframePsoDesc = D3D12::D3D12_GRAPHICS_PIPELINE_STATE_DESC{ basePsoDesc };
        wireframePsoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_WIREFRAME;

        ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(
            &wireframePsoDesc,
            __uuidof(D3D12::ID3D12PipelineState), &mWireframePSO));
    }

private:

    Microsoft::WRL::ComPtr<D3D12::ID3D12RootSignature> mRootSignature;

    std::uint32_t mBoxCBHeapIndex = -1;
    std::unique_ptr<UploadBuffer<ObjectConstants>> mObjectCB;

    std::uint32_t mPassCBHeapIndex = -1;
    std::unique_ptr<UploadBuffer<PassConstants>> mPassCB;

    std::unique_ptr<MeshGeometry> mBoxGeo;

    Microsoft::WRL::ComPtr<DXC::IDxcBlob> mvsByteCode;
    Microsoft::WRL::ComPtr<DXC::IDxcBlob> mpsByteCode;

    std::vector<D3D12::D3D12_INPUT_ELEMENT_DESC> mInputLayout;

    Microsoft::WRL::ComPtr<D3D12::ID3D12PipelineState> mSolidPSO;
    Microsoft::WRL::ComPtr<D3D12::ID3D12PipelineState> mWireframePSO;

    DirectX::XMFLOAT4X4 mWorld = MathHelper::Identity4x4;
    DirectX::XMFLOAT4X4 mView = MathHelper::Identity4x4;
    DirectX::XMFLOAT4X4 mProj = MathHelper::Identity4x4;

    float mTheta = 1.25f * DirectX::Pi;
    float mPhi = 0.25f * DirectX::Pi;
    float mRadius = 5.0f;

    Win32::POINT mLastMousePos;

    bool mDrawWireframe = false;
};
