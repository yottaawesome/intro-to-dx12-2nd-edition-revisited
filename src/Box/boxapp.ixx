export module boxapp;
import std;
import shared;

constexpr Win32::UINT CBV_SRV_UAV_HEAP_CAPACITY = 16384;

export namespace BoxApp
{
    struct ColorVertex
    {
        DirectX::XMFLOAT3 Pos;
        DirectX::XMFLOAT4 Color;
    };

	struct ObjectConstants
	{
		DirectX::XMFLOAT4X4 World = MathHelper::Identity4x4();
	};

	struct PassConstants
	{
		DirectX::XMFLOAT4X4 ViewProj = MathHelper::Identity4x4();
	};

	enum ROOT_ARG
	{
		ROOT_ARG_OBJECT_CBV = 0,
		ROOT_ARG_PASS_CBV,
		ROOT_ARG_COUNT
	};

    class BoxApp : public D3DApp
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

        virtual bool Initialize()override
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

            // Block until the upload work is complete.
            result.wait();

            return true;
        }

    private:
        //virtual void CreateRtvAndDsvDescriptorHeaps()override;
        //virtual void OnResize()override;
        //virtual void Update(const GameTimer& gt)override;
        //virtual void Draw(const GameTimer& gt)override;

        //virtual void UpdateImgui(const GameTimer& gt)override;
        //virtual void OnMouseDown(WPARAM btnState, int x, int y)override;
        //virtual void OnMouseUp(WPARAM btnState, int x, int y)override;
        //virtual void OnMouseMove(WPARAM btnState, int x, int y)override;

        void BuildCbvSrvUavDescriptorHeap()
        {
            CbvSrvUavHeap& cbvSrvUavHeap = CbvSrvUavHeap::Get();
            cbvSrvUavHeap.Init(md3dDevice.Get(), CBV_SRV_UAV_HEAP_CAPACITY);

            InitImgui(cbvSrvUavHeap);
        }
        void BuildConstantBuffers()
        {
            CbvSrvUavHeap& cbvSrvUavHeap = CbvSrvUavHeap::Get();

            mBoxCBHeapIndex = cbvSrvUavHeap.NextFreeIndex();
            mPassCBHeapIndex = cbvSrvUavHeap.NextFreeIndex();

            const std::uint32_t elementCount = 1;
            const bool isConstantBuffer = true;
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
            std::uint32_t objCBByteSize = d3dUtil::CalcConstantBufferByteSize(sizeof(ObjectConstants));
            std::uint32_t passCBByteSize = d3dUtil::CalcConstantBufferByteSize(sizeof(PassConstants));

            // In this demo we only have one constant buffer element per upload buffer, 
            // but a buffer could store an array of constant buffers. So, in general, we need to 
            // offset to the ith constant buffer when creating a view to it.
            int cbObjElementOffset = 0;
            D3D12::D3D12_GPU_VIRTUAL_ADDRESS objCBAddress =
                mObjectCB->Resource()->GetGPUVirtualAddress() +
                cbObjElementOffset * objCBByteSize;

            int cbPassElementOffset = 0;
            D3D12::D3D12_GPU_VIRTUAL_ADDRESS passCBAddress =
                mPassCB->Resource()->GetGPUVirtualAddress() +
                cbPassElementOffset * passCBByteSize;

            D3D12::D3D12_CONSTANT_BUFFER_VIEW_DESC cbvObj;
            cbvObj.BufferLocation = objCBAddress;
            cbvObj.SizeInBytes = objCBByteSize;

            md3dDevice->CreateConstantBufferView(
                &cbvObj,
                cbvSrvUavHeap.CpuHandle(mBoxCBHeapIndex));

            D3D12::D3D12_CONSTANT_BUFFER_VIEW_DESC cbvPassDesc;
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
            std::uint32_t numDescriptors = 1;
            std::uint32_t baseRegister = 0;
            objectCbvTable.Init(D3D12::D3D12_DESCRIPTOR_RANGE_TYPE::D3D12_DESCRIPTOR_RANGE_TYPE_CBV,
                numDescriptors, baseRegister);

            // Create a table for per-pass constants. Arguments would need to be
            // set once per pass.
            D3D12::CD3DX12_DESCRIPTOR_RANGE passCbvTable;
            baseRegister = 1;
            passCbvTable.Init(D3D12::D3D12_DESCRIPTOR_RANGE_TYPE::D3D12_DESCRIPTOR_RANGE_TYPE_CBV,
                numDescriptors, baseRegister);

            slotRootParameter[ROOT_ARG_OBJECT_CBV].InitAsDescriptorTable(1, &objectCbvTable);
            slotRootParameter[ROOT_ARG_PASS_CBV].InitAsDescriptorTable(1, &passCbvTable);

            // A root signature is an array of root parameters.
            D3D12::CD3DX12_ROOT_SIGNATURE_DESC rootSigDesc(
                ROOT_ARG_COUNT,
                slotRootParameter,
                0, nullptr,
                D3D12::D3D12_ROOT_SIGNATURE_FLAGS::D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

            // create a root signature
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

            auto vsArgs = std::vector<Win32::LPCWSTR>{ L"-E", L"VS", L"-T", L"vs_6_6" COMMA_DEBUG_ARGS };
            auto psArgs = std::vector<Win32::LPCWSTR>{ L"-E", L"PS", L"-T", L"ps_6_6" COMMA_DEBUG_ARGS };

            mvsByteCode = d3dUtil::CompileShader(L"Shaders\\BasicColor.hlsl", vsArgs);
            mpsByteCode = d3dUtil::CompileShader(L"Shaders\\BasicColor.hlsl", psArgs);

            mInputLayout =
            {
                { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
                { "COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }
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

            std::array<std::uint16_t, 36> indices =
            {
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

            const Win32::UINT vbByteSize = (Win32::UINT)vertices.size() * sizeof(ColorVertex);
            const Win32::UINT ibByteSize = (Win32::UINT)indices.size() * sizeof(uint16_t);

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

            // Create a new PSO based off the default PSO:
            D3D12::D3D12_GRAPHICS_PIPELINE_STATE_DESC wireframePsoDesc = basePsoDesc;
            wireframePsoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_WIREFRAME;

            ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(
                &wireframePsoDesc,
                __uuidof(D3D12::ID3D12PipelineState), &mWireframePSO));
        }

    private:

        Microsoft::WRL::ComPtr<D3D12::ID3D12RootSignature> mRootSignature = nullptr;

        std::uint32_t mBoxCBHeapIndex = -1;
        std::unique_ptr<UploadBuffer<ObjectConstants>> mObjectCB = nullptr;

        std::uint32_t mPassCBHeapIndex = -1;
        std::unique_ptr<UploadBuffer<PassConstants>> mPassCB = nullptr;

        std::unique_ptr<MeshGeometry> mBoxGeo = nullptr;

        Microsoft::WRL::ComPtr<DXC::IDxcBlob> mvsByteCode = nullptr;
        Microsoft::WRL::ComPtr<DXC::IDxcBlob> mpsByteCode = nullptr;

        std::vector<D3D12::D3D12_INPUT_ELEMENT_DESC> mInputLayout;

        Microsoft::WRL::ComPtr<D3D12::ID3D12PipelineState> mSolidPSO = nullptr;
        Microsoft::WRL::ComPtr<D3D12::ID3D12PipelineState> mWireframePSO = nullptr;

        DirectX::XMFLOAT4X4 mWorld = MathHelper::Identity4x4();
        DirectX::XMFLOAT4X4 mView = MathHelper::Identity4x4();
        DirectX::XMFLOAT4X4 mProj = MathHelper::Identity4x4();

        float mTheta = 1.25f * DirectX::XM_Pi;
        float mPhi = 0.25f * DirectX::XM_Pi;
        float mRadius = 5.0f;

        Win32::POINT mLastMousePos;

        bool mDrawWireframe = false;
    };
}