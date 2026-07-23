export module ssaodemo:ssaoapp;
import std;
import shared;
import :frameresource;
import :shadowmap;
import :ssao;

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

export class SsaoApp : public D3DApp
{
public:
    SsaoApp(Win32::HINSTANCE hInstance);
    SsaoApp(const SsaoApp& rhs) = delete;
    SsaoApp& operator=(const SsaoApp& rhs) = delete;
    ~SsaoApp();

    void Initialize()override;

private:
    void CreateRtvAndDsvDescriptorHeaps()override;
    void OnResize()override;
    void Update(const GameTimer& gt)override;
    void Draw(const GameTimer& gt)override;

    void UpdateImgui(const GameTimer& gt)override;
    void OnMouseDown(WPARAM btnState, int x, int y)override;
    void OnMouseUp(WPARAM btnState, int x, int y)override;
    void OnMouseMove(WPARAM btnState, int x, int y)override;

    void OnKeyboardInput(const GameTimer& gt);
    void AnimateMaterials(const GameTimer& gt);
    void UpdatePerObjectCB(const GameTimer& gt);
    void UpdateMaterialBuffer(const GameTimer& gt);
    void UpdateShadowTransform(const GameTimer& gt);
    void UpdateMainPassCB(const GameTimer& gt);
    void UpdateShadowPassCB(const GameTimer& gt);

    void LoadTextures();
    void LoadGeometry();
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
            D3D12::D3D12_ROOT_SIGNATURE_FLAGS {
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
        CbvSrvUavHeap& cbvSrvUavHeap = CbvSrvUavHeap::Get();
        cbvSrvUavHeap.Init(md3dDevice.Get(), CBV_SRV_UAV_HEAP_CAPACITY);

        //
        // Fill out the heap with actual descriptors.
        //

        InitImgui(cbvSrvUavHeap);

        TextureLib& texLib = TextureLib::GetLib();
        for (auto& it : texLib.GetCollection())
        {
            Texture* tex = it.second.get();
            tex->BindlessIndex = cbvSrvUavHeap.NextFreeIndex();

            D3D12::CD3DX12_CPU_DESCRIPTOR_HANDLE hDescriptor = cbvSrvUavHeap.CpuHandle(tex->BindlessIndex);
            D3D12::ID3D12Resource* texResource = tex->Resource.Get();
            if (tex->IsCubeMap)
            {
                CreateSrvCube(md3dDevice.Get(), texResource, texResource->GetDesc().Format, texResource->GetDesc().MipLevels, hDescriptor);
            }
            else
            {
                CreateSrv2d(md3dDevice.Get(), texResource, texResource->GetDesc().Format, texResource->GetDesc().MipLevels, hDescriptor);
            }
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

        mInputLayout = {
            { "POSITION", 0, DXGI::DXGI_FORMAT::DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12::D3D12_INPUT_CLASSIFICATION::D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            { "NORMAL", 0, DXGI::DXGI_FORMAT::DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12::D3D12_INPUT_CLASSIFICATION::D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "TEXCOORD", 0, DXGI::DXGI_FORMAT::DXGI_FORMAT_R32G32_FLOAT, 0, 24, D3D12::D3D12_INPUT_CLASSIFICATION::D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "TANGENT", 0, DXGI::DXGI_FORMAT::DXGI_FORMAT_R32G32B32_FLOAT, 0, 32, D3D12::D3D12_INPUT_CLASSIFICATION::D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        };
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
        for (int i = 0; i < gNumFrameResources; ++i)
        {
            mFrameResources.push_back(std::make_unique<FrameResource>(md3dDevice.Get(),
                2, (std::uint32_t)mAllRitems.size(), MaterialLib::GetLib().GetMaterialCount()));
        }
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
            DirectX::XMMatrixRotationZ(-0.54f * DirectX::Pi) *
            DirectX::XMMatrixRotationY(0.75f * DirectX::Pi) *
            DirectX::XMMatrixScaling(1.0f, 1.0f, 1.0f) *
            DirectX::XMMatrixTranslation(1.5f, 0.35f, -4.0f)
        };
        DirectX::XMStoreFloat4x4(&worldTransform, falledColumnTransform1);
        texTransform = MathHelper::Identity4x4;
        AddRenderItem(RenderLayer::Opaque, worldTransform, texTransform, matLib["columnRound"], mGeometries["columnRoundBroken"].get(), mGeometries["columnRoundBroken"]->DrawArgs["subset0"]);


        for (auto i = 0; i < 5; ++i)
        {
            auto isLeftColumnBroken = (i == 2);
            auto isRightColumnBroken = (i == 0 || i == 4);

            auto columnNameLeft = isLeftColumnBroken ? std::string{ "columnSquareBroken" } : std::string{ "columnSquare" };
            auto columnNameRight = isRightColumnBroken ? std::string{ "columnSquareBroken" } : std::string{ "columnSquare" };

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

        worldTransform = MathHelper::Identity4x4;
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

        // Change back to GENERIC_READ so we can read the texture in a shader.
        transition = CD3DX12_RESOURCE_BARRIER::Transition(normalMap,
            D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_GENERIC_READ);
        mCommandList->ResourceBarrier(1, &transition);
    }
private:

    std::vector<std::unique_ptr<FrameResource>> mFrameResources;
    FrameResource* mCurrFrameResource = nullptr;
    int mCurrFrameResourceIndex = 0;

    Microsoft::WRL::ComPtr<D3D12::ID3D12RootSignature> mRootSignature = nullptr;

    std::unordered_map<std::string, std::unique_ptr<MeshGeometry>> mGeometries;

    std::vector<D3D12::D3D12_INPUT_ELEMENT_DESC> mInputLayout;

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