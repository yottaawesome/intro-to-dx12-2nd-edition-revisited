export module shared:shaderlib;
import std;
import :win32;
import :d3dutil;

// Creates all shaders used in the book demos in one place so we do not 
// have to duplicate across demos.
export class ShaderLib
{
public:
    ShaderLib(const ShaderLib& rhs) = delete;
    ShaderLib& operator=(const ShaderLib& rhs) = delete;

    static auto GetLib() -> ShaderLib&
    {
        static auto singleton = ShaderLib{};
        return singleton;
    }

    auto IsInitialized()const -> bool
    {
        return mIsInitialized;
    }

    void Init(D3D12::ID3D12Device* device)
    {
#if defined(DEBUG) || defined(_DEBUG)  
#define COMMA_DEBUG_ARGS ,DXC::ArgDebug, DXC::ArgSkipOptimizations
#else
#define COMMA_DEBUG_ARGS
#endif

        auto msArgs = std::vector{ L"-E", L"MS", L"-T", L"ms_6_6" COMMA_DEBUG_ARGS };
        auto terrainASArgs = std::vector{ L"-E", L"TerrainAS", L"-T", L"as_6_6" COMMA_DEBUG_ARGS };
        auto terrainMSArgs = std::vector{ L"-E", L"TerrainMS", L"-T", L"ms_6_6" COMMA_DEBUG_ARGS };
        auto terrainSkirtASArgs = std::vector{ L"-E", L"TerrainSkirtAS", L"-T", L"as_6_6" COMMA_DEBUG_ARGS };
        auto terrainSkirtMSArgs = std::vector{ L"-E", L"TerrainSkirtMS", L"-T", L"ms_6_6" COMMA_DEBUG_ARGS };

        auto vsArgs = std::vector{ L"-E", L"VS", L"-T", L"vs_6_6" COMMA_DEBUG_ARGS };
        auto hsArgs = std::vector{ L"-E", L"HS", L"-T", L"hs_6_6" COMMA_DEBUG_ARGS };
        auto dsArgs = std::vector{ L"-E", L"DS", L"-T", L"ds_6_6" COMMA_DEBUG_ARGS };
        auto psArgs = std::vector{ L"-E", L"PS", L"-T", L"ps_6_6" COMMA_DEBUG_ARGS };
        auto psAlphaTestedArgs = std::vector{ L"-E", L"PS", L"-T", L"ps_6_6", L"-D ALPHA_TEST=1" COMMA_DEBUG_ARGS };


        auto vsSkinnedArgs = std::vector{ L"-E", L"VS", L"-T", L"vs_6_6", L"-D SKINNED=1" COMMA_DEBUG_ARGS };

        auto vsDrawInstancedArgs = std::vector{ L"-E", L"VS", L"-T", L"vs_6_6", L"-D DRAW_INSTANCED=1" COMMA_DEBUG_ARGS };
        auto psDrawInstancedArgs = std::vector{ L"-E", L"PS", L"-T", L"ps_6_6", L"-D DRAW_INSTANCED=1" COMMA_DEBUG_ARGS };

        auto drawViewspaceNormalsPsArgs = std::vector{ L"-E", L"DrawViewNormalsPS", L"-T", L"ps_6_6" COMMA_DEBUG_ARGS };
        auto drawBumpedWorldNormalsPsArgs = std::vector{ L"-E", L"DrawBumpedWorldNormalsPS", L"-T", L"ps_6_6" COMMA_DEBUG_ARGS };

        mShaders["standardVS"] = d3dUtil::CompileShader(L"Shaders\\DefaultGeo.hlsl", vsArgs);
        mShaders["opaquePS"] = d3dUtil::CompileShader(L"Shaders\\DefaultPS.hlsl", psArgs);
        mShaders["opaqueAlphaTestedPS"] = d3dUtil::CompileShader(L"Shaders\\DefaultPS.hlsl", psAlphaTestedArgs);

        mShaders["instancedStandardVS"] = d3dUtil::CompileShader(L"Shaders\\DefaultGeo.hlsl", vsDrawInstancedArgs);
        mShaders["instancedOpaquePS"] = d3dUtil::CompileShader(L"Shaders\\DefaultPS.hlsl", psDrawInstancedArgs);

        mShaders["tessellatedVS"] = d3dUtil::CompileShader(L"Shaders\\TessGeo.hlsl", vsArgs);
        mShaders["tessellatedHS"] = d3dUtil::CompileShader(L"Shaders\\TessGeo.hlsl", hsArgs);
        mShaders["tessellatedDS"] = d3dUtil::CompileShader(L"Shaders\\TessGeo.hlsl", dsArgs);

        mShaders["shadowVS"] = d3dUtil::CompileShader(L"Shaders\\Shadows.hlsl", vsArgs);
        mShaders["shadowOpaquePS"] = d3dUtil::CompileShader(L"Shaders\\Shadows.hlsl", psArgs);
        mShaders["shadowAlphaTestedPS"] = d3dUtil::CompileShader(L"Shaders\\Shadows.hlsl", psAlphaTestedArgs);

        mShaders["skinnedVS"] = d3dUtil::CompileShader(L"Shaders\\DefaultGeo.hlsl", vsSkinnedArgs);
        mShaders["skinnedShadowVS"] = d3dUtil::CompileShader(L"Shaders\\Shadows.hlsl", vsSkinnedArgs);

        mShaders["debugVS"] = d3dUtil::CompileShader(L"Shaders\\DebugTex.hlsl", vsArgs);
        mShaders["debugPS"] = d3dUtil::CompileShader(L"Shaders\\DebugTex.hlsl", psArgs);

        mShaders["drawNormalsVS"] = d3dUtil::CompileShader(L"Shaders\\DrawNormals.hlsl", vsArgs);
        mShaders["drawSkinnedNormalsVS"] = d3dUtil::CompileShader(L"Shaders\\DrawNormals.hlsl", vsSkinnedArgs);

        mShaders["drawViewNormalsPS"] = d3dUtil::CompileShader(L"Shaders\\DrawNormals.hlsl", drawViewspaceNormalsPsArgs);
        mShaders["drawBumpedWorldNormalsPS"] = d3dUtil::CompileShader(L"Shaders\\DrawNormals.hlsl", drawBumpedWorldNormalsPsArgs);

        mShaders["ssaoVS"] = d3dUtil::CompileShader(L"Shaders\\Ssao.hlsl", vsArgs);
        mShaders["ssaoPS"] = d3dUtil::CompileShader(L"Shaders\\Ssao.hlsl", psArgs);

        mShaders["ssaoBlurVS"] = d3dUtil::CompileShader(L"Shaders\\SsaoBlur.hlsl", vsArgs);
        mShaders["ssaoBlurPS"] = d3dUtil::CompileShader(L"Shaders\\SsaoBlur.hlsl", psArgs);

        mShaders["skyVS"] = d3dUtil::CompileShader(L"Shaders\\Sky.hlsl", vsArgs);
        mShaders["skyPS"] = d3dUtil::CompileShader(L"Shaders\\Sky.hlsl", psArgs);

        //
        // Particles
        //
        auto csUpdateParticlesArgs = std::vector{ L"-E", L"ParticlesUpdateCS", L"-T", L"cs_6_6" COMMA_DEBUG_ARGS };
        auto csEmitParticlesArgs = std::vector{ L"-E", L"ParticlesEmitCS", L"-T", L"cs_6_6" COMMA_DEBUG_ARGS };
        auto csPostUpdateParticlesArgs = std::vector{ L"-E", L"PostUpdateCS", L"-T", L"cs_6_6" COMMA_DEBUG_ARGS };
        mShaders["updateParticlesCS"] = d3dUtil::CompileShader(L"Shaders\\ParticlesCS.hlsl", csUpdateParticlesArgs);
        mShaders["emitParticlesCS"] = d3dUtil::CompileShader(L"Shaders\\ParticlesCS.hlsl", csEmitParticlesArgs);
        mShaders["postUpdateParticlesCS"] = d3dUtil::CompileShader(L"Shaders\\ParticlesCS.hlsl", csPostUpdateParticlesArgs);
        auto psParticlesAddBlend = std::vector{ L"-E", L"PSAddBlend", L"-T", L"ps_6_6" COMMA_DEBUG_ARGS };
        auto psParticlesTransparencyBlend = std::vector{ L"-E", L"PSTransparencyBlend", L"-T", L"ps_6_6" COMMA_DEBUG_ARGS };
        mShaders["drawParticlesVS"] = d3dUtil::CompileShader(L"Shaders\\DrawParticles.hlsl", vsArgs);
        mShaders["drawParticlesAddBlendPS"] = d3dUtil::CompileShader(L"Shaders\\DrawParticles.hlsl", psParticlesAddBlend);
        mShaders["drawParticlesTransparencyBlendPS"] = d3dUtil::CompileShader(L"Shaders\\DrawParticles.hlsl", psParticlesTransparencyBlend);

        // 
        // Terrain
        //
        auto vsShadowTerrainArgs = std::vector{ L"-E", L"VS", L"-T", L"vs_6_6", L"-D IS_SHADOW_PASS=1" COMMA_DEBUG_ARGS };
        auto hsShadowTerrainArgs = std::vector{ L"-E", L"HS", L"-T", L"hs_6_6", L"-D IS_SHADOW_PASS=1" COMMA_DEBUG_ARGS };
        auto dsShadowTerrainArgs = std::vector{ L"-E", L"DS", L"-T", L"ds_6_6", L"-D IS_SHADOW_PASS=1" COMMA_DEBUG_ARGS };
        mShaders["terrainVS"] = d3dUtil::CompileShader(L"Shaders\\Terrain.hlsl", vsArgs);
        mShaders["terrainHS"] = d3dUtil::CompileShader(L"Shaders\\Terrain.hlsl", hsArgs);
        mShaders["terrainDS"] = d3dUtil::CompileShader(L"Shaders\\Terrain.hlsl", dsArgs);
        mShaders["terrainShadowVS"] = d3dUtil::CompileShader(L"Shaders\\Terrain.hlsl", vsShadowTerrainArgs);
        mShaders["terrainShadowHS"] = d3dUtil::CompileShader(L"Shaders\\Terrain.hlsl", hsShadowTerrainArgs);
        mShaders["terrainShadowDS"] = d3dUtil::CompileShader(L"Shaders\\Terrain.hlsl", dsShadowTerrainArgs);
        mShaders["terrainPS"] = d3dUtil::CompileShader(L"Shaders\\Terrain.hlsl", psArgs);

        //
        // Particle mesh shader
        // 

        mShaders["helixParticlesMS"] = d3dUtil::CompileShader(L"Shaders\\HelixParticlesMS.hlsl", msArgs);
        mShaders["helixParticlesPS"] = d3dUtil::CompileShader(L"Shaders\\HelixParticlesMS.hlsl", psArgs);

        // 
        // TerrainMS
        //
        auto msShadowTerrainArgs = std::vector{ L"-E", L"TerrainMS", L"-T", L"ms_6_6", L"-D IS_SHADOW_PASS=1" COMMA_DEBUG_ARGS };
        auto msShadowTerrainSkirtArgs = std::vector{ L"-E", L"TerrainSkirtMS", L"-T", L"ms_6_6", L"-D IS_SHADOW_PASS=1" COMMA_DEBUG_ARGS };
        mShaders["terrainAS"] = d3dUtil::CompileShader(L"Shaders\\TerrainMS.hlsl", terrainASArgs);
        mShaders["terrainMS"] = d3dUtil::CompileShader(L"Shaders\\TerrainMS.hlsl", terrainMSArgs);
        mShaders["terrainShadowMS"] = d3dUtil::CompileShader(L"Shaders\\TerrainMS.hlsl", msShadowTerrainArgs);
        mShaders["terrainPS"] = d3dUtil::CompileShader(L"Shaders\\TerrainMS.hlsl", psArgs);
        mShaders["terrainSkirtAS"] = d3dUtil::CompileShader(L"Shaders\\TerrainMS.hlsl", terrainSkirtASArgs);
        mShaders["terrainSkirtMS"] = d3dUtil::CompileShader(L"Shaders\\TerrainMS.hlsl", terrainSkirtMSArgs);
        mShaders["terrainSkirtShadowMS"] = d3dUtil::CompileShader(L"Shaders\\TerrainMS.hlsl", msShadowTerrainSkirtArgs);

        //
        // Ray Tracing
        //

        auto rtArgs = std::vector{ L"-T", L"lib_6_6" COMMA_DEBUG_ARGS };
        mShaders["rayTracingLib"] = d3dUtil::CompileShader(L"Shaders\\RayTracing.hlsl", rtArgs);
        mShaders["hybridReflectionsRTLib"] = d3dUtil::CompileShader(L"Shaders\\HybridReflections.hlsl", rtArgs);

        mShaders["opaqueHybridRT_vs"] = d3dUtil::CompileShader(L"Shaders\\DefaultHybridRT.hlsl", vsArgs);
        mShaders["opaqueHybridRT_ps"] = d3dUtil::CompileShader(L"Shaders\\DefaultHybridRT.hlsl", psArgs);

        mIsInitialized = true;
    }

    auto AddShader(const std::string& name, Microsoft::WRL::ComPtr<DXC::IDxcBlob> shader) -> bool
    {
        if (mShaders.find(name) == mShaders.end())
        {
            mShaders[name] = shader;
            return true;
        }
        return false;
    }

    auto operator[](const std::string& name) -> DXC::IDxcBlob*
    {
        if (mShaders.find(name) != mShaders.end())
            return mShaders[name].Get();
        return nullptr;
    }

private:
    ShaderLib() = default;

protected:
    bool mIsInitialized = false;
    std::unordered_map<std::string, Microsoft::WRL::ComPtr<DXC::IDxcBlob>> mShaders;
};