export module terraindemo:terrainapp;
import std;
import shared;
import :frameresource;
import :ssao;
import :particlesystem;
import :terrain;

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

export class TerrainApp : public D3DApp
{
public:
    TerrainApp(HINSTANCE hInstance);
    TerrainApp(const TerrainApp& rhs) = delete;
    TerrainApp& operator=(const TerrainApp& rhs) = delete;
    ~TerrainApp();

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

    void UpdateShadowPassCB(const GameTimer& gt)
    {
        auto view = DirectX::XMMATRIX{DirectX::XMLoadFloat4x4(&mLightView)};
		auto detView = DirectX::XMVECTOR{DirectX::XMMatrixDeterminant(view)};
        auto invView = DirectX::XMMATRIX{DirectX::XMMatrixInverse(&detView, view)};

        auto proj = DirectX::XMMATRIX{DirectX::XMLoadFloat4x4(&mLightProj)};
		auto detProj = DirectX::XMVECTOR{DirectX::XMMatrixDeterminant(proj)};
        auto invProj = DirectX::XMMATRIX{DirectX::XMMatrixInverse(&detProj, proj)};

        auto viewProj = DirectX::XMMATRIX{DirectX::XMMatrixMultiply(view, proj)};
		auto detViewProj = DirectX::XMVECTOR{DirectX::XMMatrixDeterminant(viewProj)};
        auto invViewProj = DirectX::XMMATRIX{DirectX::XMMatrixInverse(&detViewProj, viewProj)};

        auto w = mShadowMap->Width();
        auto h = mShadowMap->Height();

        DirectX::XMStoreFloat4x4(&mShadowPassCB.gView, DirectX::XMMatrixTranspose(view));
        DirectX::XMStoreFloat4x4(&mShadowPassCB.gInvView, DirectX::XMMatrixTranspose(invView));
        DirectX::XMStoreFloat4x4(&mShadowPassCB.gProj, DirectX::XMMatrixTranspose(proj));
        DirectX::XMStoreFloat4x4(&mShadowPassCB.gInvProj, DirectX::XMMatrixTranspose(invProj));
        DirectX::XMStoreFloat4x4(&mShadowPassCB.gViewProj, DirectX::XMMatrixTranspose(viewProj));
        DirectX::XMStoreFloat4x4(&mShadowPassCB.gInvViewProj, DirectX::XMMatrixTranspose(invViewProj));

        MathHelper::ExtractFrustumPlanes(viewProj, mShadowPassCB.gWorldFrustumPlanes);

        mShadowPassCB.gEyePosW = mCamera.GetPosition3f();
        mShadowPassCB.gRenderTargetSize = DirectX::XMFLOAT2((float)w, (float)h);
        mShadowPassCB.gInvRenderTargetSize = DirectX::XMFLOAT2(1.0f / w, 1.0f / h);
        mShadowPassCB.gNearZ = mLightNearZ;
        mShadowPassCB.gFarZ = mLightFarZ;

        auto currPassCB = mCurrFrameResource->PassCB.get();
        currPassCB->CopyData(1, mShadowPassCB);
    }

    void EmitExplosionParticles(const GameTimer& gt)
    {
        auto& texLib = TextureLib::GetLib();

        auto spawnPos =  MathHelper::Vector3{mWorldRayPos} + MathHelper::Vector3{mWorldRayDir} * MathHelper::RandF(5.0f, 20.0f);

        auto numParticlesEmitted = MathHelper::Rand(2000u, 3000u);

        if (not mSpawnExplosion)
            return;

        auto explosionParticles = ParticleEmitCB{
            .gEmitBoxMin = spawnPos + MathHelper::Vector3(-0.1f, -0.1f, -0.1f),
            .gMinLifetime = 0.3f,
            .gEmitBoxMax = spawnPos + MathHelper::Vector3(+0.1f, 0.1f, +0.1f),
            .gMaxLifetime = 0.9f,
            .gEmitDirectionMin = MathHelper::Vector3(-1.0f, -1.0f, -1.0f),
            .gMinInitialSpeed = 20.0f,
            .gEmitDirectionMax = MathHelper::Vector3(+1.0f, +1.0f, +1.0f),
            .gMaxInitialSpeed = 80.0f,
            .gEmitColorMin = MathHelper::Vector4(0.0f, 0.0f, 0.0f, 1.0f),
            .gEmitColorMax = MathHelper::Vector4(1.0f, 1.0f, 1.0f, 1.0f),
            .gMaxRotation = 2.0f * MathHelper::Pi,
            .gMinRotationSpeed = 1.0f,
            .gMaxRotationSpeed = 5.0f,
            .gMinScale = MathHelper::Vector2(0.5f),
            .gMaxScale = MathHelper::Vector2(1.5f),

            .gDragScale = 0.75f,
            .gEmitCount = numParticlesEmitted,
            .gBindlessTextureIndex = static_cast<std::uint32_t>(texLib["explosionParticle"]->BindlessIndex),
            .gEmitRandomValues = MathHelper::Vector4(MathHelper::RandF(), MathHelper::RandF(), MathHelper::RandF(), MathHelper::RandF())
        };

        mExplosionParticleSystem->Emit(explosionParticles);

        mSpawnExplosion = false;
    }

    void EmitRainParticles(const GameTimer& gt)
    {
        auto& texLib = TextureLib::GetLib();

        auto camPos = MathHelper::Vector3{mCamera.GetPosition()};

        static auto rainParticlesToEmit = 0.0f;
        rainParticlesToEmit += gt.DeltaTime() * mRainEmitRate;

        // Wait until we have enough particles to fill one thread group.
        if (rainParticlesToEmit > 128.0f)
        {
            auto numParticlesEmitted = static_cast<std::uint32_t>(rainParticlesToEmit);

            auto rainParticles = ParticleEmitCB{
                .gEmitBoxMin = camPos + MathHelper::Vector3(-40.0f, 8.0f, -40.0f),
                .gMinLifetime = 2.5f,
                .gEmitBoxMax = camPos + MathHelper::Vector3(+40.0f, 10.0f, +40.0f),
                .gMaxLifetime = 3.5f,
                .gEmitDirectionMin = MathHelper::Vector3(-1.0f, -4.0f, -1.0f),
                .gMinInitialSpeed = 0.0f,
                .gEmitDirectionMax = MathHelper::Vector3(+1.0f, -3.0f, +1.0f),
                .gMaxInitialSpeed = 0.0f,
                .gEmitColorMin = MathHelper::Vector4(1.0f, 1.0f, 1.0f, 1.0f),
                .gEmitColorMax = MathHelper::Vector4(1.0f, 1.0f, 1.0f, 1.0f),
                .gMinRotation = 0.0f,
                .gMaxRotation = 0.0f,
                .gMinRotationSpeed = 0.0f,
                .gMaxRotationSpeed = 0.0f,
                .gMinScale = mRainScale * MathHelper::Vector2(0.1f, 0.2f),
                .gMaxScale = mRainScale * MathHelper::Vector2(0.2f, 0.3f),
                .gDragScale = 0.0f,
                .gEmitCount = numParticlesEmitted,
                .gBindlessTextureIndex = static_cast<std::uint32_t>(texLib["rainParticle"]->BindlessIndex),
                .gEmitRandomValues = MathHelper::Vector4(MathHelper::RandF(), MathHelper::RandF(), MathHelper::RandF(), MathHelper::RandF())
            };

            mRainParticleSystem->Emit(rainParticles);

            rainParticlesToEmit -= numParticlesEmitted;
        }
    }

    void ReadParticleCounts(const GameTimer& gt)
    {
        auto rainParticleCount = static_cast<std::uint32_t*>(nullptr);
        ThrowIfFailed(mCurrFrameResource->RainParticleCountReadbackBuffer->Map(0, nullptr, reinterpret_cast<void**>(&rainParticleCount)));

        // This is actually gNumFrameResources frames behind so we do not have to block GPU to read.
        mRainParticleCount = rainParticleCount[0];

        mCurrFrameResource->RainParticleCountReadbackBuffer->Unmap(0, nullptr);

        static float particleCountPollTime = 0.0f;
        particleCountPollTime += gt.DeltaTime();

        // For display, do not update every frame as it is hard to read text changing that fast :)
        if (particleCountPollTime >= 0.5f)
        {
            mDisplayedRainParticleCount = mRainParticleCount;
            particleCountPollTime -= 0.5f;

            // TODO: Why is this here?
            //if (mDisplayedRainParticleCount == 0)
            //    MessageBox(0, 0, 0, 0);
        }
    }

    void LoadTextures()
    {
        auto& texLib = TextureLib::GetLib();
        texLib.Init(md3dDevice.Get(), *mUploadBatch.get());
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

        ThrowIfFailed(md3dDevice->CreateRootSignature(0,
            serializedRootSig->GetBufferPointer(),
            serializedRootSig->GetBufferSize(),
            __uuidof(D3D12::ID3D12RootSignature),
            reinterpret_cast<void**>(mGfxRootSignature.GetAddressOf())
        ));


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
			reinterpret_cast<void**>(mComputeRootSignature.GetAddressOf())
        ));
    }

    void BuildCommandSignatures()
    {
        // Since the particle system is updated on the GPU, we do not know how many particles are 
        // alive on the CPU. Thus we do not know how many particles to draw. Reading from GPU memory to
        // CPU memory is slow. The DrawIndirect API allows us to specify the draw arguments via a GPU 
        // buffer, so we can keep everything on the GPU.

        // Describe the data of each indirect argument. The order here must match the actual data.
        // This can be more complicated to set root constants and change vertex buffer views, for example.
        auto indirectDispatchArgs = std::array{
            D3D12::D3D12_INDIRECT_ARGUMENT_DESC{
				.Type = D3D12::D3D12_INDIRECT_ARGUMENT_TYPE::D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH
            }
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
			reinterpret_cast<void**>(mIndirectDispatch.GetAddressOf())));

        auto indirectDrawIndexedArgs = std::array{
            D3D12::D3D12_INDIRECT_ARGUMENT_DESC{
				.Type = D3D12::D3D12_INDIRECT_ARGUMENT_TYPE::D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED
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

            auto hDescriptor = D3D12::CD3DX12_CPU_DESCRIPTOR_HANDLE{cbvSrvUavHeap.CpuHandle(tex->BindlessIndex)};
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
                2, static_cast<std::uint32_t>(mAllRitems.size()), MaterialLib::GetLib().GetMaterialCount()));
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

        auto columnSceneOffset = DirectX::XMMATRIX{DirectX::XMMatrixTranslation(5.0f, -33.0f, -160.0f)};

        DirectX::XMStoreFloat4x4(&worldTransform, DirectX::XMMatrixScaling(5000.0f, 5000.0f, 5000.0f));
        texTransform = MathHelper::Identity4x4;
        AddRenderItem(RenderLayer::Sky, worldTransform, texTransform, matLib["sky"], mGeometries["shapeGeo"].get(), mGeometries["shapeGeo"]->DrawArgs["sphere"]);

        DirectX::XMStoreFloat4x4(&worldTransform, DirectX::XMMatrixScaling(6.0f, 6.0f, 6.0f) * columnSceneOffset);
        DirectX::XMStoreFloat4x4(&texTransform, DirectX::XMMatrixScaling(1.0f, 1.0f, 1.0f));
        AddRenderItem(RenderLayer::Opaque, worldTransform, texTransform, matLib["orbBase"], mGeometries["orbBase"].get(), mGeometries["orbBase"]->DrawArgs["subset0"]);

        DirectX::XMStoreFloat4x4(&worldTransform, DirectX::XMMatrixScaling(2.0f, 2.0f, 2.0f) * DirectX::XMMatrixTranslation(0.0f, 1.75f, 0.0f) * columnSceneOffset);
        texTransform = MathHelper::Identity4x4;
        AddRenderItem(RenderLayer::Opaque, worldTransform, texTransform, matLib["mirror1"], mGeometries["shapeGeo"].get(), mGeometries["shapeGeo"]->DrawArgs["sphere"]);

        worldTransform = MathHelper::Identity4x4 * columnSceneOffset;
        DirectX::XMStoreFloat4x4(&texTransform, DirectX::XMMatrixScaling(6.0f, 6.0f, 1.0f));
        AddRenderItem(RenderLayer::Opaque, worldTransform, texTransform, matLib["stoneFloor"], mGeometries["shapeGeo"].get(), mGeometries["shapeGeo"]->DrawArgs["grid"]);

        auto falledColumnTransform0 = DirectX::XMMATRIX{
            DirectX::XMMatrixRotationZ(-0.54f * DirectX::Pi) *
            DirectX::XMMatrixRotationY(0.15f * DirectX::Pi) *
            DirectX::XMMatrixScaling(1.0f, 1.0f, 1.0f) *
            DirectX::XMMatrixTranslation(-3.0f, 0.35f, 3.0f) * columnSceneOffset
        };
        DirectX::XMStoreFloat4x4(&worldTransform, falledColumnTransform0);
        texTransform = MathHelper::Identity4x4;
        AddRenderItem(RenderLayer::Opaque, worldTransform, texTransform, matLib["columnRound"], mGeometries["columnRoundBroken"].get(), mGeometries["columnRoundBroken"]->DrawArgs["subset0"]);

        auto falledColumnTransform1 = DirectX::XMMATRIX{
            DirectX::XMMatrixRotationZ(-0.54f * DirectX::Pi) *
            DirectX::XMMatrixRotationY(0.75f * DirectX::Pi) *
            DirectX::XMMatrixScaling(1.0f, 1.0f, 1.0f) *
            DirectX::XMMatrixTranslation(1.5f, 0.35f, -4.0f) * columnSceneOffset 
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
            DirectX::XMStoreFloat4x4(&worldTransform, DirectX::XMMatrixTranslation(-5.0f, 0.0f, -10.0f + i * 5.0f) * columnSceneOffset);
            AddRenderItem(RenderLayer::Opaque, worldTransform, texTransform, matLib["columnSquare"], mGeometries[columnNameLeft].get(), mGeometries[columnNameLeft]->DrawArgs["subset0"]);

            DirectX::XMStoreFloat4x4(&texTransform, DirectX::XMMatrixScaling(1.0f, 1.0f, 1.0f));
            DirectX::XMStoreFloat4x4(&worldTransform, DirectX::XMMatrixTranslation(+5.0f, 0.0f, -10.0f + i * 5.0f) * columnSceneOffset);
            AddRenderItem(RenderLayer::Opaque, worldTransform, texTransform, matLib["columnSquare"], mGeometries[columnNameRight].get(), mGeometries[columnNameRight]->DrawArgs["subset0"]);

            if (not isLeftColumnBroken)
            {
                texTransform = MathHelper::Identity4x4;
                DirectX::XMStoreFloat4x4(&worldTransform, DirectX::XMMatrixTranslation(-5.0f, 4.0f, -10.0f + i * 5.0f) * columnSceneOffset);
                AddRenderItem(RenderLayer::Opaque, worldTransform, texTransform, matLib["mirror0"], mGeometries["shapeGeo"].get(), mGeometries["shapeGeo"]->DrawArgs["sphere"]);
            }

            if (not isRightColumnBroken)
            {
                texTransform = MathHelper::Identity4x4;
                DirectX::XMStoreFloat4x4(&worldTransform, DirectX::XMMatrixTranslation(+5.0f, 4.0f, -10.0f + i * 5.0f) * columnSceneOffset);
                AddRenderItem(RenderLayer::Opaque, worldTransform, texTransform, matLib["mirror0"], mGeometries["shapeGeo"].get(), mGeometries["shapeGeo"]->DrawArgs["sphere"]);
            }
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

        auto passCBByteSize = std::uint32_t{ d3dUtil::CalcConstantBufferByteSize(sizeof(PerPassCB)) };

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

        mTerrain->Draw(mCommandList.Get(), psoLib["terrain_shadow"]);

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

    std::unique_ptr<Terrain> mTerrain;

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
    float mMaxTess = 6.0f;
    float mMinTessDistance = 20;
    float mMaxTessDistance = 200;

    bool mNormalMapsEnabled = true;
    bool mReflectionsEnabled = true;
    bool mShadowsEnabled = true;
    bool mUseTerrainHeightMap = true;
    bool mUseMaterialHeightMaps = true;

    Win32::POINT mLastMousePos{};
};
