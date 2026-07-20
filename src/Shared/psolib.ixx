export module shared:psolib;
import std;
import :win32;
import :d3dutil;
import :shaderlib;

// Creates all PSOs used in the book demos in one place so we do not 
// have to duplicate across demos.
export class PsoLib
{
public:
    PsoLib(const PsoLib& rhs) = delete;
    PsoLib& operator=(const PsoLib& rhs) = delete;

    static PsoLib& GetLib()
    {
        static PsoLib singleton;
        return singleton;
    }

	auto IsInitialized()const -> bool
    {
        return mIsInitialized;
    }

    void Init(D3D12::ID3D12Device5* device,
        DXGI::DXGI_FORMAT backBufferFormat,
        DXGI::DXGI_FORMAT depthStencilFormat,
        DXGI::DXGI_FORMAT ambientMapFormat,
        DXGI::DXGI_FORMAT screenNormalMapFormat,
        D3D12::ID3D12RootSignature* rootSig,
        D3D12::ID3D12RootSignature* computeRootSig = nullptr)
    {
        auto& shaderLib = ShaderLib::GetLib();

        //
        // Input Layouts
        const auto modelInputLayout = std::vector<D3D12_INPUT_ELEMENT_DESC>{
            { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "TANGENT", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 32, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        };

        const auto terrainInputLayout = std::vector<D3D12_INPUT_ELEMENT_DESC>{
            { "POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        };

        const auto skinnedInputLayout = std::vector<D3D12_INPUT_ELEMENT_DESC>{
            { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "TANGENT", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 32, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "WEIGHTS", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 44, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "BONEINDICES", 0, DXGI_FORMAT_R8G8B8A8_UINT, 0, 56, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }
        };


        auto basePsoDesc = d3dUtil::InitDefaultPso(
            backBufferFormat, depthStencilFormat, modelInputLayout, rootSig,
            shaderLib["standardVS"], shaderLib["opaquePS"]);

        //
        // PSO for opaque objects.
        //

        auto opaquePsoDesc = basePsoDesc;
        ThrowIfFailed(device->CreateGraphicsPipelineState(&opaquePsoDesc, __uuidof(D3D12::ID3D12PipelineState), &mPSOs["opaque"]));

        auto opaqueWireframePsoDesc = opaquePsoDesc;
        opaqueWireframePsoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_WIREFRAME;
        ThrowIfFailed(device->CreateGraphicsPipelineState(&opaqueWireframePsoDesc, __uuidof(D3D12::ID3D12PipelineState), &mPSOs["opaque_wireframe"]));

        // Note: Because for SSAO we do a separate depth prepass, when we draw the main opaque pass, 
        // we can change the depth test to EQUAL.
        auto opaqueWithPrepassPsoDesc = basePsoDesc;
        opaqueWithPrepassPsoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_EQUAL;
        opaqueWithPrepassPsoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
        ThrowIfFailed(device->CreateGraphicsPipelineState(&opaqueWithPrepassPsoDesc, __uuidof(D3D12::ID3D12PipelineState), &mPSOs["opaque_wprepass"]));

        //
        // PSO for opaque skinned objects.
        //

        auto opaqueSkinnedPsoDesc = basePsoDesc;
        opaqueSkinnedPsoDesc.InputLayout = { skinnedInputLayout.data(), (UINT)skinnedInputLayout.size() };
        opaqueSkinnedPsoDesc.VS = d3dUtil::ByteCodeFromBlob(shaderLib["skinnedVS"]);
        opaqueSkinnedPsoDesc.PS = d3dUtil::ByteCodeFromBlob(shaderLib["opaquePS"]);
        ThrowIfFailed(device->CreateGraphicsPipelineState(&opaqueSkinnedPsoDesc, __uuidof(D3D12::ID3D12PipelineState), &mPSOs["skinnedOpaque"]));

        auto opaqueSkinnedWireframePsoDesc = opaqueSkinnedPsoDesc;
        opaqueSkinnedWireframePsoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_WIREFRAME;
        ThrowIfFailed(device->CreateGraphicsPipelineState(&opaqueSkinnedWireframePsoDesc, __uuidof(D3D12::ID3D12PipelineState), &mPSOs["skinnedOpaque_wireframe"]));

        auto opaqueSkinnedWithPrePassPsoDesc = opaqueSkinnedPsoDesc;
        opaqueSkinnedWithPrePassPsoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_EQUAL;
        opaqueSkinnedWithPrePassPsoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
        ThrowIfFailed(device->CreateGraphicsPipelineState(&opaqueSkinnedWithPrePassPsoDesc, __uuidof(D3D12::ID3D12PipelineState), &mPSOs["skinnedOpaque_wprepass"]));

        //
        // PSO for opaque instanced objects.
        //

        auto opaqueInstancedPsoDesc = basePsoDesc;
        opaqueInstancedPsoDesc.VS = d3dUtil::ByteCodeFromBlob(shaderLib["instancedStandardVS"]);
        opaqueInstancedPsoDesc.PS = d3dUtil::ByteCodeFromBlob(shaderLib["instancedOpaquePS"]);
        ThrowIfFailed(device->CreateGraphicsPipelineState(&opaqueInstancedPsoDesc, __uuidof(D3D12::ID3D12PipelineState), &mPSOs["opaque_instanced"]));

        auto opaqueInstancedWireframePsoDesc = opaqueInstancedPsoDesc;
        opaqueInstancedWireframePsoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_WIREFRAME;
        ThrowIfFailed(device->CreateGraphicsPipelineState(&opaqueInstancedWireframePsoDesc, __uuidof(D3D12::ID3D12PipelineState), &mPSOs["opaque_instanced_wireframe"]));


        //
        // PSO for opaque tessellated objects.
        //

        auto opaqueTessPsoDesc = basePsoDesc;
        opaqueTessPsoDesc.VS = d3dUtil::ByteCodeFromBlob(shaderLib["tessellatedVS"]);
        opaqueTessPsoDesc.HS = d3dUtil::ByteCodeFromBlob(shaderLib["tessellatedHS"]);
        opaqueTessPsoDesc.DS = d3dUtil::ByteCodeFromBlob(shaderLib["tessellatedDS"]);
        opaqueTessPsoDesc.PS = d3dUtil::ByteCodeFromBlob(shaderLib["opaquePS"]);
        opaqueTessPsoDesc.PrimitiveTopologyType = D3D12::D3D12_PRIMITIVE_TOPOLOGY_TYPE::D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH;
        ThrowIfFailed(device->CreateGraphicsPipelineState(&opaqueTessPsoDesc, __uuidof(D3D12::ID3D12PipelineState), &mPSOs["opaque_tess"]));

        auto opaqueTessWireframePsoDesc = opaqueTessPsoDesc;
        opaqueTessWireframePsoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_WIREFRAME;
        ThrowIfFailed(device->CreateGraphicsPipelineState(&opaqueTessWireframePsoDesc, __uuidof(D3D12::ID3D12PipelineState), &mPSOs["opaque_tess_wireframe"]));

        //
        // PSO for highlight objects (used in picking demo).
        //

        auto highlightPsoDesc = opaquePsoDesc;

        // Change the depth test from < to <= so that if we draw the same triangle twice, it will
        // still pass the depth test.  This is needed because we redraw the picked triangle with a
        // different material to highlight it.  If we do not use <=, the triangle will fail the 
        // depth test the 2nd time we try and draw it.
        highlightPsoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;

        // Standard transparency blending.
        auto highlightBlendDesc = D3D12::D3D12_RENDER_TARGET_BLEND_DESC{};
        highlightBlendDesc.BlendEnable = true;
        highlightBlendDesc.LogicOpEnable = false;
        highlightBlendDesc.SrcBlend = D3D12_BLEND_SRC_ALPHA;
        highlightBlendDesc.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
        highlightBlendDesc.BlendOp = D3D12_BLEND_OP_ADD;
        highlightBlendDesc.SrcBlendAlpha = D3D12_BLEND_ONE;
        highlightBlendDesc.DestBlendAlpha = D3D12_BLEND_ZERO;
        highlightBlendDesc.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        highlightBlendDesc.LogicOp = D3D12_LOGIC_OP_NOOP;
        highlightBlendDesc.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

        highlightPsoDesc.BlendState.RenderTarget[0] = highlightBlendDesc;
        ThrowIfFailed(device->CreateGraphicsPipelineState(&highlightPsoDesc, __uuidof(D3D12::ID3D12PipelineState), &mPSOs["highlight"]));

        //
        // PSO for shadow map pass.
        //
        auto smapPsoDesc = basePsoDesc;
        smapPsoDesc.RasterizerState.DepthBias = 10000;//100000;
        smapPsoDesc.RasterizerState.DepthBiasClamp = 0.0f;
        smapPsoDesc.RasterizerState.SlopeScaledDepthBias = 1.0f;
        smapPsoDesc.pRootSignature = rootSig;
        smapPsoDesc.VS = d3dUtil::ByteCodeFromBlob(shaderLib["shadowVS"]);
        smapPsoDesc.PS = d3dUtil::ByteCodeFromBlob(shaderLib["shadowOpaquePS"]);
        smapPsoDesc.RTVFormats[0] = DXGI_FORMAT_UNKNOWN; // depth pass only
        smapPsoDesc.NumRenderTargets = 0;
        ThrowIfFailed(device->CreateGraphicsPipelineState(&smapPsoDesc, __uuidof(D3D12::ID3D12PipelineState), &mPSOs["shadow_opaque"]));

        auto skinnedSmapPsoDesc = smapPsoDesc;
        skinnedSmapPsoDesc.InputLayout = { skinnedInputLayout.data(), (UINT)skinnedInputLayout.size() };
        skinnedSmapPsoDesc.VS = d3dUtil::ByteCodeFromBlob(shaderLib["skinnedShadowVS"]);
        skinnedSmapPsoDesc.PS = d3dUtil::ByteCodeFromBlob(shaderLib["shadowOpaquePS"]);
        ThrowIfFailed(device->CreateGraphicsPipelineState(&skinnedSmapPsoDesc, __uuidof(D3D12::ID3D12PipelineState), &mPSOs["skinnedShadow_opaque"]));

        //
        // PSO for debug layer.
        //
        auto debugPsoDesc = basePsoDesc;
        debugPsoDesc.VS = d3dUtil::ByteCodeFromBlob(shaderLib["debugVS"]);
        debugPsoDesc.PS = d3dUtil::ByteCodeFromBlob(shaderLib["debugPS"]);
        ThrowIfFailed(device->CreateGraphicsPipelineState(&debugPsoDesc, __uuidof(D3D12::ID3D12PipelineState), &mPSOs["debug"]));

        //
        // PSO for drawing normals.
        //
        auto drawViewNormalsPsoDesc = basePsoDesc;
        drawViewNormalsPsoDesc.VS = d3dUtil::ByteCodeFromBlob(shaderLib["drawNormalsVS"]);
        drawViewNormalsPsoDesc.PS = d3dUtil::ByteCodeFromBlob(shaderLib["drawViewNormalsPS"]);
        drawViewNormalsPsoDesc.RTVFormats[0] = screenNormalMapFormat;
        drawViewNormalsPsoDesc.SampleDesc.Count = 1;
        drawViewNormalsPsoDesc.SampleDesc.Quality = 0;
        drawViewNormalsPsoDesc.DSVFormat = depthStencilFormat;
        ThrowIfFailed(device->CreateGraphicsPipelineState(&drawViewNormalsPsoDesc, __uuidof(D3D12::ID3D12PipelineState), &mPSOs["drawViewNormals"]));

        auto drawSkinnedViewNormalsPsoDesc = drawViewNormalsPsoDesc;
        drawSkinnedViewNormalsPsoDesc.InputLayout = { skinnedInputLayout.data(), (UINT)skinnedInputLayout.size() };
        drawSkinnedViewNormalsPsoDesc.VS = d3dUtil::ByteCodeFromBlob(shaderLib["drawSkinnedNormalsVS"]);
        drawSkinnedViewNormalsPsoDesc.PS = d3dUtil::ByteCodeFromBlob(shaderLib["drawViewNormalsPS"]);
        ThrowIfFailed(device->CreateGraphicsPipelineState(&drawSkinnedViewNormalsPsoDesc, __uuidof(D3D12::ID3D12PipelineState), &mPSOs["drawSkinnedViewNormals"]));

        //
        // PSO for drawing normals.
        //
        auto drawBumpedWorldNormalsPsoDesc = basePsoDesc;
        drawBumpedWorldNormalsPsoDesc.VS = d3dUtil::ByteCodeFromBlob(shaderLib["drawNormalsVS"]);
        drawBumpedWorldNormalsPsoDesc.PS = d3dUtil::ByteCodeFromBlob(shaderLib["drawBumpedWorldNormalsPS"]);
        drawBumpedWorldNormalsPsoDesc.RTVFormats[0] = SceneNormalMapFormat;
        drawBumpedWorldNormalsPsoDesc.SampleDesc.Count = 1;
        drawBumpedWorldNormalsPsoDesc.SampleDesc.Quality = 0;
        drawBumpedWorldNormalsPsoDesc.DSVFormat = depthStencilFormat;
        ThrowIfFailed(device->CreateGraphicsPipelineState(&drawBumpedWorldNormalsPsoDesc, __uuidof(D3D12::ID3D12PipelineState), &mPSOs["drawBumpedWorldNormals"]));

        auto drawSkinnedBumpedWorldNormalsPsoDesc = drawBumpedWorldNormalsPsoDesc;
        drawSkinnedBumpedWorldNormalsPsoDesc.InputLayout = { skinnedInputLayout.data(), (UINT)skinnedInputLayout.size() };
        drawSkinnedBumpedWorldNormalsPsoDesc.VS = d3dUtil::ByteCodeFromBlob(shaderLib["drawSkinnedNormalsVS"]);
        drawSkinnedBumpedWorldNormalsPsoDesc.PS = d3dUtil::ByteCodeFromBlob(shaderLib["drawBumpedWorldNormalsPS"]);
        ThrowIfFailed(device->CreateGraphicsPipelineState(&drawSkinnedBumpedWorldNormalsPsoDesc, __uuidof(D3D12::ID3D12PipelineState), &mPSOs["drawSkinnedBumpedWorldNormals"]));

        //
        // PSO for SSAO.
        //
        auto ssaoPsoDesc = basePsoDesc;
        ssaoPsoDesc.InputLayout = { nullptr, 0 };
        ssaoPsoDesc.pRootSignature = rootSig;
        ssaoPsoDesc.VS = d3dUtil::ByteCodeFromBlob(shaderLib["ssaoVS"]);
        ssaoPsoDesc.PS = d3dUtil::ByteCodeFromBlob(shaderLib["ssaoPS"]);

        // SSAO effect does not need the depth buffer.
        ssaoPsoDesc.DepthStencilState.DepthEnable = false;
        ssaoPsoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
        ssaoPsoDesc.RTVFormats[0] = ambientMapFormat;
        ssaoPsoDesc.SampleDesc.Count = 1;
        ssaoPsoDesc.SampleDesc.Quality = 0;
        ssaoPsoDesc.DSVFormat = DXGI_FORMAT_UNKNOWN;
        ThrowIfFailed(device->CreateGraphicsPipelineState(&ssaoPsoDesc, __uuidof(D3D12::ID3D12PipelineState), &mPSOs["ssao"]));

        //
        // PSO for SSAO blur.
        //
        auto ssaoBlurPsoDesc = ssaoPsoDesc;
        ssaoBlurPsoDesc.VS = d3dUtil::ByteCodeFromBlob(shaderLib["ssaoBlurVS"]);
        ssaoBlurPsoDesc.PS = d3dUtil::ByteCodeFromBlob(shaderLib["ssaoBlurPS"]);
        ThrowIfFailed(device->CreateGraphicsPipelineState(&ssaoBlurPsoDesc, __uuidof(D3D12::ID3D12PipelineState), &mPSOs["ssaoBlur"]));

        //
        // PSO for sky.
        //
        auto skyPsoDesc = basePsoDesc;

        // The camera is inside the sky sphere, so just turn off culling.
        skyPsoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;

        // Make sure the depth function is LESS_EQUAL and not just LESS.  
        // Otherwise, the normalized depth values at z = 1 (NDC) will 
        // fail the depth test if the depth buffer was cleared to 1.
        skyPsoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;

        skyPsoDesc.VS = d3dUtil::ByteCodeFromBlob(shaderLib["skyVS"]);
        skyPsoDesc.PS = d3dUtil::ByteCodeFromBlob(shaderLib["skyPS"]);
        ThrowIfFailed(device->CreateGraphicsPipelineState(&skyPsoDesc, __uuidof(D3D12::ID3D12PipelineState), &mPSOs["sky"]));

        //
        // PSO for terrain.
        // 
        auto terrainPsoDesc = opaquePsoDesc;

        terrainPsoDesc.InputLayout = { terrainInputLayout.data(), (UINT)terrainInputLayout.size() };
        terrainPsoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH;
        terrainPsoDesc.VS = d3dUtil::ByteCodeFromBlob(shaderLib["terrainVS"]);
        terrainPsoDesc.HS = d3dUtil::ByteCodeFromBlob(shaderLib["terrainHS"]);
        terrainPsoDesc.DS = d3dUtil::ByteCodeFromBlob(shaderLib["terrainDS"]);
        terrainPsoDesc.PS = d3dUtil::ByteCodeFromBlob(shaderLib["terrainPS"]);
        ThrowIfFailed(device->CreateGraphicsPipelineState(&terrainPsoDesc, __uuidof(D3D12::ID3D12PipelineState), &mPSOs["terrain"]));

        terrainPsoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_WIREFRAME;
        ThrowIfFailed(device->CreateGraphicsPipelineState(&terrainPsoDesc, __uuidof(D3D12::ID3D12PipelineState), &mPSOs["terrain_wireframe"]));

        auto terrainRasterizerDesc = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
        terrainRasterizerDesc.FillMode = D3D12_FILL_MODE_SOLID;
        terrainRasterizerDesc.DepthBias = 10000;
        terrainRasterizerDesc.DepthBiasClamp = 0.0f;
        terrainRasterizerDesc.SlopeScaledDepthBias = 1.0f;

        terrainPsoDesc = smapPsoDesc;
        terrainPsoDesc.RasterizerState = terrainRasterizerDesc;
        terrainPsoDesc.InputLayout = { terrainInputLayout.data(), (UINT)terrainInputLayout.size() };
        terrainPsoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH;
        terrainPsoDesc.VS = d3dUtil::ByteCodeFromBlob(shaderLib["terrainShadowVS"]);
        terrainPsoDesc.HS = d3dUtil::ByteCodeFromBlob(shaderLib["terrainShadowHS"]);
        terrainPsoDesc.DS = d3dUtil::ByteCodeFromBlob(shaderLib["terrainShadowDS"]);
        terrainPsoDesc.PS = D3D12_SHADER_BYTECODE{ nullptr, 0 };
        ThrowIfFailed(device->CreateGraphicsPipelineState(&terrainPsoDesc, __uuidof(D3D12::ID3D12PipelineState), &mPSOs["terrain_shadow"]));

        //
        // PSOs for particles.
        //

        if (computeRootSig != nullptr)
        {
            auto updateParticlesPsoDesc = D3D12::D3D12_COMPUTE_PIPELINE_STATE_DESC{
                .pRootSignature = computeRootSig,
                .CS = d3dUtil::ByteCodeFromBlob(shaderLib["updateParticlesCS"]),
                .NodeMask = 0,
                .Flags = D3D12_PIPELINE_STATE_FLAG_NONE,
            };
            ThrowIfFailed(device->CreateComputePipelineState(&updateParticlesPsoDesc, __uuidof(D3D12::ID3D12PipelineState), &mPSOs["updateParticles"]));

            auto emitParticlesPsoDesc = D3D12::D3D12_COMPUTE_PIPELINE_STATE_DESC{
                .pRootSignature = computeRootSig,
                .CS = d3dUtil::ByteCodeFromBlob(shaderLib["emitParticlesCS"]),
                .Flags = D3D12_PIPELINE_STATE_FLAG_NONE
            };
            ThrowIfFailed(device->CreateComputePipelineState(&emitParticlesPsoDesc, __uuidof(D3D12::ID3D12PipelineState), &mPSOs["emitParticles"]));

            auto postUpdateParticlesPsoDesc = D3D12::D3D12_COMPUTE_PIPELINE_STATE_DESC{
                .pRootSignature = computeRootSig,
                .CS = d3dUtil::ByteCodeFromBlob(shaderLib["postUpdateParticlesCS"]),
                .Flags = D3D12_PIPELINE_STATE_FLAG_NONE
            };
            
            ThrowIfFailed(device->CreateComputePipelineState(&postUpdateParticlesPsoDesc, __uuidof(D3D12::ID3D12PipelineState), &mPSOs["postUpdateParticles"]));

            auto drawParticlesPsoDesc = opaquePsoDesc;
            drawParticlesPsoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;

            auto particlesAddBlendDesc = D3D12::D3D12_RENDER_TARGET_BLEND_DESC{
                .BlendEnable = true,
                .LogicOpEnable = false,
                .SrcBlend = D3D12_BLEND_ONE,
                .DestBlend = D3D12_BLEND_ONE,
                .BlendOp = D3D12_BLEND_OP_ADD,
                .SrcBlendAlpha = D3D12_BLEND_ONE,
                .DestBlendAlpha = D3D12_BLEND_ZERO,
                .BlendOpAlpha = D3D12_BLEND_OP_ADD,
                .LogicOp = D3D12_LOGIC_OP_NOOP,
                .RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL
            };
            
            drawParticlesPsoDesc.BlendState.RenderTarget[0] = particlesAddBlendDesc;
            drawParticlesPsoDesc.VS = d3dUtil::ByteCodeFromBlob(shaderLib["drawParticlesVS"]);
            drawParticlesPsoDesc.PS = d3dUtil::ByteCodeFromBlob(shaderLib["drawParticlesAddBlendPS"]);
            ThrowIfFailed(device->CreateGraphicsPipelineState(&drawParticlesPsoDesc, __uuidof(D3D12::ID3D12PipelineState), &mPSOs["drawParticlesAddBlend"]));

            auto particlesTransparencyBlendDesc = particlesAddBlendDesc;
            particlesTransparencyBlendDesc.SrcBlend = D3D12_BLEND_SRC_ALPHA;
            particlesTransparencyBlendDesc.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
            drawParticlesPsoDesc.BlendState.RenderTarget[0] = particlesTransparencyBlendDesc;
            drawParticlesPsoDesc.PS = d3dUtil::ByteCodeFromBlob(shaderLib["drawParticlesTransparencyBlendPS"]);
            ThrowIfFailed(device->CreateGraphicsPipelineState(&drawParticlesPsoDesc, __uuidof(D3D12::ID3D12PipelineState), &mPSOs["drawParticlesTransparencyBlend"]));
        }

        InitHelixParticleMeshShaderPSOs(
            device,
            backBufferFormat,
            depthStencilFormat,
            rootSig);

        InitTerrainMeshShaderPSOs(
            device,
            backBufferFormat,
            depthStencilFormat,
            rootSig);

        //
        // RT hybrid composite ray traced reflections
        // 

        auto opaqueHybridRTPsoDesc = basePsoDesc;
        opaqueHybridRTPsoDesc.VS = d3dUtil::ByteCodeFromBlob(shaderLib["opaqueHybridRT_vs"]);
        opaqueHybridRTPsoDesc.PS = d3dUtil::ByteCodeFromBlob(shaderLib["opaqueHybridRT_ps"]);
        opaqueHybridRTPsoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_EQUAL;
        opaqueHybridRTPsoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
        ThrowIfFailed(device->CreateGraphicsPipelineState(&opaqueHybridRTPsoDesc, __uuidof(D3D12::ID3D12PipelineState), &mPSOs["opaque_hybrid_rt"]));

        mIsInitialized = true;
    }

    auto AddPso(const std::string& name, Microsoft::WRL::ComPtr<D3D12::ID3D12PipelineState> pso) -> bool
    {
        if (mPSOs.find(name) == mPSOs.end())
        {
            mPSOs[name] = pso;
            return true;
        }
        return false;
    }

    auto operator[](const std::string& name) -> D3D12::ID3D12PipelineState*
    {
        if (mPSOs.find(name) != mPSOs.end())
            return mPSOs[name].Get();
        return nullptr;
    }

private:
    PsoLib() = default;

    void InitHelixParticleMeshShaderPSOs(
        D3D12::ID3D12Device5* device,
        DXGI::DXGI_FORMAT backBufferFormat,
        DXGI::DXGI_FORMAT depthStencilFormat,
        D3D12::ID3D12RootSignature* rootSig
    )
    {
        auto& shaderLib = ShaderLib::GetLib();

        // Define the members we are interested in setting. Non-specified PSO properties will use defaults.
        struct ParticlesPsoStream
        {
            D3D12::CD3DX12_PIPELINE_STATE_STREAM_ROOT_SIGNATURE pRootSignature;
            D3D12::CD3DX12_PIPELINE_STATE_STREAM_MS MS;
            D3D12::CD3DX12_PIPELINE_STATE_STREAM_PS PS;
            D3D12::CD3DX12_PIPELINE_STATE_STREAM_RENDER_TARGET_FORMATS RTVFormats;
            D3D12::CD3DX12_PIPELINE_STATE_STREAM_DEPTH_STENCIL_FORMAT DSVFormat;
            D3D12::CD3DX12_PIPELINE_STATE_STREAM_BLEND_DESC BlendDesc;
            D3D12::CD3DX12_PIPELINE_STATE_STREAM_DEPTH_STENCIL DepthStencilDesc;
        };

		auto particlesPsoFormats = std::array<DXGI_FORMAT, 8>
        {
            backBufferFormat,
            DXGI_FORMAT_UNKNOWN,
            DXGI_FORMAT_UNKNOWN,
            DXGI_FORMAT_UNKNOWN,
            DXGI_FORMAT_UNKNOWN,
            DXGI_FORMAT_UNKNOWN,
            DXGI_FORMAT_UNKNOWN,
            DXGI_FORMAT_UNKNOWN,
        };

        auto particlesAddBlendDesc = D3D12::D3D12_RENDER_TARGET_BLEND_DESC{
            .BlendEnable = true,
            .LogicOpEnable = false,
            .SrcBlend = D3D12_BLEND_ONE,
            .DestBlend = D3D12_BLEND_ONE,
            .BlendOp = D3D12_BLEND_OP_ADD,
            .SrcBlendAlpha = D3D12_BLEND_ONE,
            .DestBlendAlpha = D3D12_BLEND_ZERO,
            .BlendOpAlpha = D3D12_BLEND_OP_ADD,
            .LogicOp = D3D12_LOGIC_OP_NOOP,
            .RenderTargetWriteMask =D3D12::D3D12_COLOR_WRITE_ENABLE::D3D12_COLOR_WRITE_ENABLE_ALL
        };
        
        auto blendDesc = D3D12::CD3DX12_BLEND_DESC(D3D12_DEFAULT);
        blendDesc.RenderTarget[0] = particlesAddBlendDesc;

        auto depthStencilDesc = D3D12::CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
        depthStencilDesc.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;

        auto psoStream = ParticlesPsoStream{
            .pRootSignature = rootSig,
            .MS = d3dUtil::ByteCodeFromBlob(shaderLib["helixParticlesMS"]),
            .PS = d3dUtil::ByteCodeFromBlob(shaderLib["helixParticlesPS"]),
            .RTVFormats = D3D12::CD3DX12_RT_FORMAT_ARRAY(particlesPsoFormats.data(), 1),
            .DSVFormat = depthStencilFormat,
            .BlendDesc = blendDesc,
            .DepthStencilDesc = depthStencilDesc
        };
        
        auto streamDesc = D3D12::D3D12_PIPELINE_STATE_STREAM_DESC{
            .SizeInBytes = sizeof(ParticlesPsoStream),
            .pPipelineStateSubobjectStream = &psoStream
        };

        ThrowIfFailed(device->CreatePipelineState(&streamDesc, __uuidof(D3D12::ID3D12PipelineState), &mPSOs["helixParticles_ms"]));
    }

    void InitTerrainMeshShaderPSOs(
        D3D12::ID3D12Device5* device,
        DXGI::DXGI_FORMAT backBufferFormat,
        DXGI::DXGI_FORMAT depthStencilFormat,
        D3D12::ID3D12RootSignature* rootSig
    )
    {
        auto& shaderLib = ShaderLib::GetLib();

        // Define the members we are interested in setting. Non-specified PSO properties will use defaults.
        struct TerrainPsoStream
        {
            D3D12::CD3DX12_PIPELINE_STATE_STREAM_ROOT_SIGNATURE pRootSignature;
            D3D12::CD3DX12_PIPELINE_STATE_STREAM_AS AS;
            D3D12::CD3DX12_PIPELINE_STATE_STREAM_MS MS;
            D3D12::CD3DX12_PIPELINE_STATE_STREAM_PS PS;
            D3D12::CD3DX12_PIPELINE_STATE_STREAM_RENDER_TARGET_FORMATS RTVFormats;
            D3D12::CD3DX12_PIPELINE_STATE_STREAM_DEPTH_STENCIL_FORMAT DSVFormat;
            D3D12::CD3DX12_PIPELINE_STATE_STREAM_RASTERIZER RasterizerState;
        };

		auto terrainPsoFormats = std::array<DXGI_FORMAT, 8>{
            backBufferFormat,
            DXGI_FORMAT_UNKNOWN,
            DXGI_FORMAT_UNKNOWN,
            DXGI_FORMAT_UNKNOWN,
            DXGI_FORMAT_UNKNOWN,
            DXGI_FORMAT_UNKNOWN,
            DXGI_FORMAT_UNKNOWN,
            DXGI_FORMAT_UNKNOWN,
        };

        auto terrainRasterizerDesc = D3D12::CD3DX12_RASTERIZER_DESC(D3D12::D3D12_DEFAULT);

        auto terrainPsoStream = TerrainPsoStream{
            .pRootSignature = rootSig,
            .AS = d3dUtil::ByteCodeFromBlob(shaderLib["terrainAS"]),
            .MS = d3dUtil::ByteCodeFromBlob(shaderLib["terrainMS"]),
            .PS = d3dUtil::ByteCodeFromBlob(shaderLib["terrainPS"]),
            .RTVFormats = D3D12::CD3DX12_RT_FORMAT_ARRAY(terrainPsoFormats.data(), 1),
            .DSVFormat = depthStencilFormat,
            .RasterizerState = terrainRasterizerDesc
        };
        

        auto terrainStreamDesc = D3D12::D3D12_PIPELINE_STATE_STREAM_DESC{
            .SizeInBytes = sizeof(TerrainPsoStream),
            .pPipelineStateSubobjectStream = &terrainPsoStream,
        };

        ThrowIfFailed(device->CreatePipelineState(&terrainStreamDesc, __uuidof(D3D12::ID3D12PipelineState), &mPSOs["terrain_ms"]));

        terrainRasterizerDesc.FillMode = D3D12_FILL_MODE_WIREFRAME;
        terrainPsoStream.RasterizerState = terrainRasterizerDesc;
        ThrowIfFailed(device->CreatePipelineState(&terrainStreamDesc, __uuidof(D3D12::ID3D12PipelineState), &mPSOs["terrain_ms_wireframe"]));

        terrainPsoFormats[0] = DXGI_FORMAT_UNKNOWN;

        terrainRasterizerDesc.FillMode = D3D12_FILL_MODE_SOLID;
        terrainRasterizerDesc.DepthBias = 10000;
        terrainRasterizerDesc.DepthBiasClamp = 0.0f;
        terrainRasterizerDesc.SlopeScaledDepthBias = 1.0f;

        auto shadowTerrainPsoStream = TerrainPsoStream{
            .pRootSignature = rootSig,
            .AS = d3dUtil::ByteCodeFromBlob(shaderLib["terrainAS"]),
            .MS = d3dUtil::ByteCodeFromBlob(shaderLib["terrainShadowMS"]),
            .PS = D3D12::D3D12_SHADER_BYTECODE{ nullptr, 0 },
            .RTVFormats = D3D12::CD3DX12_RT_FORMAT_ARRAY(terrainPsoFormats.data(), 0),
            .DSVFormat = depthStencilFormat,
            .RasterizerState = terrainRasterizerDesc
        };
        
        auto terrainShadowStreamDesc = D3D12::D3D12_PIPELINE_STATE_STREAM_DESC{
            .SizeInBytes = sizeof(TerrainPsoStream),
            .pPipelineStateSubobjectStream = &shadowTerrainPsoStream,
        };
        
        ThrowIfFailed(device->CreatePipelineState(&terrainShadowStreamDesc, __uuidof(D3D12::ID3D12PipelineState), &mPSOs["terrain_ms_shadow"]));

        terrainPsoFormats[0] = backBufferFormat;

        auto terrainSkirtPsoStream = TerrainPsoStream{
            .pRootSignature = rootSig,
            .AS = d3dUtil::ByteCodeFromBlob(shaderLib["terrainSkirtAS"]),
            .MS = d3dUtil::ByteCodeFromBlob(shaderLib["terrainSkirtMS"]),
            .PS = d3dUtil::ByteCodeFromBlob(shaderLib["terrainPS"]),
            .RTVFormats = D3D12::CD3DX12_RT_FORMAT_ARRAY(terrainPsoFormats.data(), 1),
            .DSVFormat = depthStencilFormat,
            .RasterizerState = terrainRasterizerDesc
        };

        auto terrainSkirtStreamDesc = D3D12::D3D12_PIPELINE_STATE_STREAM_DESC{
            .SizeInBytes = sizeof(TerrainPsoStream),
            .pPipelineStateSubobjectStream = &terrainSkirtPsoStream,
        };
        
        ThrowIfFailed(device->CreatePipelineState(&terrainSkirtStreamDesc, __uuidof(D3D12::ID3D12PipelineState), &mPSOs["terrain_ms_skirt"]));

        terrainRasterizerDesc.FillMode = D3D12_FILL_MODE_WIREFRAME;
        terrainSkirtPsoStream.RasterizerState = terrainRasterizerDesc;
        ThrowIfFailed(device->CreatePipelineState(&terrainSkirtStreamDesc, __uuidof(D3D12::ID3D12PipelineState), &mPSOs["terrain_ms_skirt_wireframe"]));

        terrainPsoFormats[0] = DXGI_FORMAT_UNKNOWN;

        terrainRasterizerDesc.FillMode = D3D12_FILL_MODE_SOLID;
        terrainRasterizerDesc.DepthBias = 10000;
        terrainRasterizerDesc.DepthBiasClamp = 0.0f;
        terrainRasterizerDesc.SlopeScaledDepthBias = 1.0f;

        auto shadowTerrainSkirtPsoStream = TerrainPsoStream{
            .pRootSignature = rootSig,
            .AS = d3dUtil::ByteCodeFromBlob(shaderLib["terrainSkirtAS"]),
            .MS = d3dUtil::ByteCodeFromBlob(shaderLib["terrainSkirtShadowMS"]),
            .PS = D3D12_SHADER_BYTECODE{ nullptr, 0 },
            .RTVFormats = D3D12::CD3DX12_RT_FORMAT_ARRAY(terrainPsoFormats.data(), 0),
            .DSVFormat = depthStencilFormat,
            .RasterizerState = terrainRasterizerDesc
        };

        auto terrainShadowSkirtStreamDesc = D3D12::D3D12_PIPELINE_STATE_STREAM_DESC{
            .SizeInBytes = sizeof(TerrainPsoStream),
            .pPipelineStateSubobjectStream = &shadowTerrainSkirtPsoStream,
        };
        
        ThrowIfFailed(device->CreatePipelineState(&terrainShadowSkirtStreamDesc, __uuidof(D3D12::ID3D12PipelineState), &mPSOs["terrain_ms_skirt_shadow"]));
    }

protected:
    bool mIsInitialized = false;

    std::unordered_map<std::string, Microsoft::WRL::ComPtr<D3D12::ID3D12PipelineState>> mPSOs;
};