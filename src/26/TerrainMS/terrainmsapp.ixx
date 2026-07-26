export module terrainmsdemo:terrainmsapp;
import std;
import shared;
import :frameresource;
import :particlesystem;
import :terrainms;

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
//

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

export class TerrainMSApp : public D3DApp
{
public:
    TerrainMSApp(HINSTANCE hInstance);
    TerrainMSApp(const TerrainMSApp& rhs) = delete;
    TerrainMSApp& operator=(const TerrainMSApp& rhs) = delete;
    ~TerrainMSApp();


private:
    void Initialize()override;

    void CreateRtvAndDsvDescriptorHeaps()override;
    void OnResize()override;
    void Update(const GameTimer& gt)override;
    void Draw(const GameTimer& gt)override;

    void UpdateImgui(const GameTimer& gt)override;
    void OnMouseDown(Win32::WPARAM btnState, int x, int y)override;
    void OnMouseUp(Win32::WPARAM btnState, int x, int y)override;
    void OnMouseMove(Win32::WPARAM btnState, int x, int y)override;

    void OnKeyboardInput(const GameTimer& gt);
    void AnimateMaterials(const GameTimer& gt);
    void UpdatePerObjectCB(const GameTimer& gt);
    void UpdateMaterialBuffer(const GameTimer& gt);
    void UpdateShadowTransform(const GameTimer& gt);
    void UpdateMainPassCB(const GameTimer& gt);
    void UpdateShadowPassCB(const GameTimer& gt);
    void EmitExplosionParticles(const GameTimer& gt);
    void EmitRainParticles(const GameTimer& gt);
    void ReadParticleCounts(const GameTimer& gt);

    void LoadTextures();
    void LoadGeometry();
    void BuildRootSignatures();
    void BuildCommandSignatures()
    {
        // Since the particle system is updated on the GPU, we do not know how many particles are 
        // alive on the CPU. Thus we do not know how many particles to draw. Reading from GPU memory to
        // CPU memory is slow. The DrawIndirect API allows us to specify the draw arguments via a GPU 
        // buffer, so we can keep everything on the GPU.

        // Describe the data of each indirect argument. The order here must match the actual data.
        // This can be more complicated to set root constants and change vertex buffer views, for example.
        auto indirectDispatchArgs = std::array{
            D3D12::D3D12_INDIRECT_ARGUMENT_DESC{ .Type = D3D12::D3D12_INDIRECT_ARGUMENT_TYPE::D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH }
        };
        auto indirectDispatchDesc = D3D12::D3D12_COMMAND_SIGNATURE_DESC{
            .ByteStride = sizeof(D3D12::D3D12_DISPATCH_ARGUMENTS),
            .NumArgumentDescs = 1,
            .pArgumentDescs = indirectDispatchArgs.data(),
            .NodeMask = 0 // used for multiple GPUs
        };

        ThrowIfFailed(md3dDevice->CreateCommandSignature(
            &indirectDispatchDesc,
            nullptr, // root args not changing
            __uuidof(D3D12::ID3D12CommandSignature),
            reinterpret_cast<void**>(mIndirectDispatch.GetAddressOf())
        ));

        auto indirectDrawIndexedArgs = std::array{
            D3D12::D3D12_INDIRECT_ARGUMENT_DESC{
                .Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED
            }
        };

        auto indirectDrawIndexedDesc = D3D12::D3D12_COMMAND_SIGNATURE_DESC{
            .ByteStride = sizeof(D3D12::D3D12_DRAW_INDEXED_ARGUMENTS),
            .NumArgumentDescs = 1,
            .pArgumentDescs = indirectDrawIndexedArgs.data(),
            .NodeMask = 0 // used for multiple GPUs
        };

        ThrowIfFailed(md3dDevice->CreateCommandSignature(
            &indirectDrawIndexedDesc,
            nullptr, // root args not changing
			__uuidof(D3D12::ID3D12CommandSignature), 
            reinterpret_cast<void**>(mIndirectDrawIndexed.GetAddressOf())));
    }

    void BuildCbvSrvUavDescriptorHeap()
    {
        auto& cbvSrvUavHeap = CbvSrvUavHeap::Get();
        cbvSrvUavHeap.Init(md3dDevice.Get(), CBV_SRV_UAV_HEAP_CAPACITY);

        //
        // Fill out the heap with actual descriptors.
        //

        InitImgui(cbvSrvUavHeap);

        mShadowMapBindlessIndex = mShadowMap->BuildDescriptors(mDsvHeap.CpuHandle(DSV_SHADOWMAP));

        auto& texLib = TextureLib::GetLib();
        for (auto& it : texLib.GetCollection())
        {
            auto tex = static_cast<Texture*>(it.second.get());
            tex->BindlessIndex = cbvSrvUavHeap.NextFreeIndex();

            auto hDescriptor = D3D12::CD3DX12_CPU_DESCRIPTOR_HANDLE{ cbvSrvUavHeap.CpuHandle(tex->BindlessIndex) };
            auto texResource = static_cast<D3D12::ID3D12Resource*>(tex->Resource.Get());
            if (tex->IsCubeMap)
                CreateSrvCube(md3dDevice.Get(), texResource, texResource->GetDesc().Format, texResource->GetDesc().MipLevels, hDescriptor);
            else
                CreateSrv2d(md3dDevice.Get(), texResource, texResource->GetDesc().Format, texResource->GetDesc().MipLevels, hDescriptor);
        }

        mRandomTexBindlessIndex = texLib["randomTex1024"]->BindlessIndex;
        mSkyBindlessIndex = texLib["skyCubeMap"]->BindlessIndex;

        mExplosionParticleSystem->BuildDescriptors();
        mRainParticleSystem->BuildDescriptors();

        mTerrain->BuildDescriptors();
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
        for (int i = 0; i < gNumFrameResources; ++i)
        {
            mFrameResources.push_back(std::make_unique<FrameResource>(md3dDevice.Get(),
                2, (UINT)mAllRitems.size(), MaterialLib::GetLib().GetMaterialCount()));
        }
    }

    void BuildMaterials()
    {
        MaterialLib::GetLib().Init(md3dDevice.Get());
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

        mTerrain->Draw(mCommandList.Get(), psoLib["terrain_ms_shadow"], psoLib["terrain_ms_skirt_shadow"], mDrawSkirts);

        // Change back to GENERIC_READ so we can read the texture in a shader.
        transition = D3D12::CD3DX12_RESOURCE_BARRIER::Transition(mShadowMap->Resource(),
            D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_GENERIC_READ);
        mCommandList->ResourceBarrier(1, &transition);
    }

private:

    std::vector<std::unique_ptr<FrameResource>> mFrameResources;
    FrameResource* mCurrFrameResource = nullptr;
    int mCurrFrameResourceIndex = 0;

    Microsoft::WRL::ComPtr<D3D12::ID3D12RootSignature> mGfxRootSignature = nullptr;
    Microsoft::WRL::ComPtr<D3D12::ID3D12RootSignature> mComputeRootSignature = nullptr;

    Microsoft::WRL::ComPtr<D3D12::ID3D12CommandSignature> mIndirectDispatch = nullptr;
    Microsoft::WRL::ComPtr<D3D12::ID3D12CommandSignature> mIndirectDrawIndexed = nullptr;

    std::unordered_map<std::string, std::unique_ptr<MeshGeometry>> mGeometries;

    // List of all the render items.
    std::vector<std::unique_ptr<RenderItem>> mAllRitems;

    // Render items divided by PSO.
    std::vector<RenderItem*> mRitemLayer[(int)RenderLayer::Count];

    std::uint32_t mRandomTexBindlessIndex = -1;
    std::uint32_t mSkyBindlessIndex = -1;
    std::uint32_t mShadowMapBindlessIndex = -1;

    PerPassCB mMainPassCB;  // index 0 of pass cbuffer.
    PerPassCB mShadowPassCB;// index 1 of pass cbuffer.

    Camera mCamera;

    std::unique_ptr<ShadowMap> mShadowMap;

    std::unique_ptr<ParticleSystem> mExplosionParticleSystem;
    std::unique_ptr<ParticleSystem> mRainParticleSystem;

    std::unique_ptr<TerrainMS> mTerrain;

    static constexpr std::uint32_t MaxExplosionParticleCount = 1024 * 20;
    static constexpr std::uint32_t MaxRainParticleCount = 1024 * 50;

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

    bool mSpawnExplosion = false;
    DirectX::SimpleMath::Vector3 mWorldRayPos;
    DirectX::SimpleMath::Vector3 mWorldRayDir;

    std::uint32_t mRainParticleCount = 0;
    std::uint32_t mDisplayedRainParticleCount = 0;

    float mRainEmitRate = 5000.0f;
    float mRainScale = 1.0f;
    DirectX::XMFLOAT3 mAcceleration{ -1.0f, -9.8f, 0.0f };

    bool mIsWireframe = false;
    bool mDrawSkirts = true;
    float mSkirtOffsetY = 2.0f;
    int mNumSubdivisionsPerPatch = 0;
    float mMinTessDistance = 10.0f;
    float mMaxTessDistance = 150.0f;
    float mMaxTess = 6.0f;

    bool mNormalMapsEnabled = true;
    bool mReflectionsEnabled = true;
    bool mShadowsEnabled = true;

    Win32::POINT mLastMousePos{};
};