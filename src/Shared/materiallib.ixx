export module shared:materiallib;
import std;
import :win32;
import :mathhelper;
import :d3dutil;
import :texturelib;

export class MaterialLib
{
public:
    MaterialLib(const MaterialLib& rhs) = delete;
    MaterialLib& operator=(const MaterialLib& rhs) = delete;

    static auto GetLib() -> MaterialLib&
    {
        static auto singleton = MaterialLib{};
        return singleton;
    }

    auto IsInitialized()const -> bool
    {
        return mIsInitialized;
    }

    auto GetMaterialCount()const -> std::uint32_t
    {
        return static_cast<uint32_t>(mMaterials.size());
    }

    void Init(D3D12::ID3D12Device* device)
    {
        auto& texLib = TextureLib::GetLib();

        AddMaterial("whiteMat",
            texLib["defaultDiffuseMap"],
            texLib["defaultNormalMap"],
            texLib["defaultGlossHeightAoMap"],
            DirectX::XMFLOAT4(1.0f, 1.0f, 1.0f, 1.0f),
            DirectX::XMFLOAT3(0.1f, 0.1f, 0.1f), 0.3f);

        AddMaterial("crate",
            texLib["crateDiffuseMap"],
            texLib["defaultNormalMap"],
            texLib["defaultGlossHeightAoMap"],
            DirectX::XMFLOAT4(1.0f, 1.0f, 1.0f, 1.0f),
            DirectX::XMFLOAT3(0.1f, 0.1f, 0.1f), 0.3f);

        AddMaterial("water",
            texLib["waterDiffuseMap"],
            texLib["defaultNormalMap"],
            texLib["defaultGlossHeightAoMap"],
            DirectX::XMFLOAT4(1.0f, 1.0f, 1.0f, 0.5f),
            DirectX::XMFLOAT3(0.1f, 0.1f, 0.1f), 0.1f);

        AddMaterial("fence",
            texLib["fenceDiffuseMap"],
            texLib["defaultNormalMap"],
            texLib["defaultGlossHeightAoMap"],
            DirectX::XMFLOAT4(1.0f, 1.0f, 1.0f, 1.0f),
            DirectX::XMFLOAT3(0.1f, 0.1f, 0.1f), 0.25f);

        AddMaterial("grass",
            texLib["grassDiffuseMap"],
            texLib["defaultNormalMap"],
            texLib["defaultGlossHeightAoMap"],
            DirectX::XMFLOAT4(1.0f, 1.0f, 1.0f, 1.0f),
            DirectX::XMFLOAT3(0.1f, 0.1f, 0.1f), 0.8f);

        AddMaterial("bricks0",
            texLib["bricksDiffuseMap"],
            texLib["bricksNormalMap"],
            texLib["bricksGlossHeightAoMap"],
            DirectX::XMFLOAT4(1.0f, 1.0f, 1.0f, 1.0f),
            DirectX::XMFLOAT3(0.1f, 0.1f, 0.1f), 0.3f);

        AddMaterial("tile0",
            texLib["tileDiffuseMap"],
            texLib["defaultNormalMap"],
            texLib["defaultGlossHeightAoMap"],
            DirectX::XMFLOAT4(0.9f, 0.9f, 0.9f, 1.0f),
            DirectX::XMFLOAT3(0.2f, 0.2f, 0.2f),
            0.1f,
            0.25f);

        AddMaterial("stoneFloor",
            texLib["stoneFloorDiffuseMap"],
            texLib["stoneFloorNormalMap"],
            texLib["stoneFloorGlossHeightAoMap"],
            DirectX::XMFLOAT4(0.9f, 0.9f, 0.9f, 1.0f),
            DirectX::XMFLOAT3(0.2f, 0.2f, 0.2f),
            0.1f,
            0.25f);

        AddMaterial("rock0",
            texLib["rock_color"],
            texLib["rock_normal"],
            texLib["rock_gloss_height_ao"],
            DirectX::XMFLOAT4(0.9f, 0.9f, 0.9f, 1.0f),
            DirectX::XMFLOAT3(0.2f, 0.2f, 0.2f), 0.1f);

        AddMaterial("mirror0",
            texLib["defaultDiffuseMap"],
            texLib["defaultNormalMap"],
            texLib["defaultGlossHeightAoMap"],
            DirectX::XMFLOAT4(0.0f, 0.0f, 0.0f, 1.0f),
            DirectX::XMFLOAT3(0.98f, 0.97f, 0.95f), 0.1f);

        AddMaterial("mirror1",
            texLib["defaultDiffuseMap"],
            texLib["defaultNormalMap"],
            texLib["defaultGlossHeightAoMap"],
            DirectX::XMFLOAT4(0.1f, 0.1f, 0.3f, 1.0f),
            DirectX::XMFLOAT3(0.4f, 0.4f, 0.4f), 0.05f);

        AddMaterial("skullMat",
            texLib["defaultDiffuseMap"],
            texLib["defaultNormalMap"],
            texLib["defaultGlossHeightAoMap"],
            DirectX::XMFLOAT4(0.8f, 0.8f, 0.8f, 1.0f),
            DirectX::XMFLOAT3(0.6f, 0.6f, 0.6f), 0.2f);

        AddMaterial("skullMatMatte",
            texLib["defaultDiffuseMap"],
            texLib["defaultNormalMap"],
            texLib["defaultGlossHeightAoMap"],
            DirectX::XMFLOAT4(0.8f, 0.8f, 0.8f, 1.0f),
            DirectX::XMFLOAT3(0.0f, 0.0f, 0.0f), 0.2f);

        AddMaterial("sky",
            texLib["defaultDiffuseMap"],
            texLib["defaultNormalMap"],
            texLib["defaultGlossHeightAoMap"],
            DirectX::XMFLOAT4(1.0f, 1.0f, 1.0f, 1.0f),
            DirectX::XMFLOAT3(0.1f, 0.1f, 0.1f), 1.0f);

        AddMaterial("highlight0",
            texLib["defaultDiffuseMap"],
            texLib["defaultNormalMap"],
            texLib["defaultGlossHeightAoMap"],
            DirectX::XMFLOAT4(1.0f, 1.0f, 0.0f, 0.5f),
            DirectX::XMFLOAT3(0.1f, 0.1f, 0.1f), 0.0f);

        AddMaterial("treeSprites",
            texLib["treeSpritesArray"],
            texLib["defaultNormalMap"],
            texLib["defaultGlossHeightAoMap"],
            DirectX::XMFLOAT4(1.0f, 1.0f, 1.0f, 1.0f),
            DirectX::XMFLOAT3(0.01f, 0.01f, 0.01f), 0.125f);

        AddMaterial("columnRound",
            texLib["columnRoundDiffuseMap"],
            texLib["columnRoundNormalMap"],
            texLib["columnRoundGlossHeightAoMap"],
            DirectX::XMFLOAT4(1.0f, 1.0f, 1.0f, 1.0f),
            DirectX::XMFLOAT3(0.01f, 0.01f, 0.01f), 0.125f);

        AddMaterial("columnSquare",
            texLib["columnSquareDiffuseMap"],
            texLib["columnSquareNormalMap"],
            texLib["columnSquareGlossHeightAoMap"],
            DirectX::XMFLOAT4(1.0f, 1.0f, 1.0f, 1.0f),
            DirectX::XMFLOAT3(0.01f, 0.01f, 0.01f), 0.125f);

        AddMaterial("orbBase",
            texLib["orbBaseDiffuseMap"],
            texLib["orbBaseNormalMap"],
            texLib["orbBaseGlossHeightAoMap"],
            DirectX::XMFLOAT4(1.0f, 1.0f, 1.0f, 1.0f),
            DirectX::XMFLOAT3(0.01f, 0.01f, 0.01f), 0.125f);


        //
        // Terrain layer materials.
        //

        AddMaterial("terrainlayer0",
            texLib["grass_color"],
            texLib["grass_normal"],
            texLib["grass_gloss_height_ao"],
            DirectX::XMFLOAT4(1.0f, 1.0f, 1.0f, 1.0f), DirectX::XMFLOAT3(0.05f, 0.05f, 0.05f),
            0.0f, 0.3f,
            DirectX::SimpleMath::Matrix::CreateScale(128.0f));

        AddMaterial("terrainlayer1",
            texLib["sand_color"],
            texLib["sand_normal"],
            texLib["sand_gloss_height_ao"],
            DirectX::XMFLOAT4(1.0f, 1.0f, 1.0f, 1.0f),
            DirectX::XMFLOAT3(0.1f, 0.1f, 0.1f), 0.0f, 1.0f,
            DirectX::SimpleMath::Matrix::CreateScale(128.0f));

        AddMaterial("terrainlayer2",
            texLib["rock_color"],
            texLib["rock_normal"],
            texLib["rock_gloss_height_ao"],
            DirectX::XMFLOAT4(1.0f, 1.0f, 1.0f, 1.0f),
            DirectX::XMFLOAT3(0.2f, 0.2f, 0.2f),
            0.0f, 1.0f,
            DirectX::SimpleMath::Matrix::CreateScale(32.0f));

        AddMaterial("terrainlayer3",
            texLib["dirt0_color"],
            texLib["dirt0_normal"],
            texLib["dirt0_gloss_height_ao"],
            DirectX::XMFLOAT4(1.0f, 1.0f, 1.0f, 1.0f),
            DirectX::XMFLOAT3(0.1f, 0.1f, 0.1f),
            0.0f, 0.3f,
            DirectX::SimpleMath::Matrix::CreateScale(64.0f));

        AddMaterial("terrainlayer4",
            texLib["dirt1_color"],
            texLib["dirt1_normal"],
            texLib["dirt1_gloss_height_ao"],
            DirectX::XMFLOAT4(1.0f, 1.0f, 1.0f, 1.0f),
            DirectX::XMFLOAT3(0.1f, 0.1f, 0.1f),
            0.0f, 0.5f,
            DirectX::SimpleMath::Matrix::CreateScale(128.0f));

        AddMaterial("terrainlayer5",
            texLib["trail_color"],
            texLib["trail_normal"],
            texLib["trail_gloss_height_ao"],
            DirectX::XMFLOAT4(1.0f, 1.0f, 1.0f, 1.0f),
            DirectX::XMFLOAT3(0.1f, 0.1f, 0.1f),
            0.0f, 0.2f,
            DirectX::SimpleMath::Matrix::CreateScale(256.0f));

        AddMaterial("terrainlayer6",
            texLib["rock1_color"],
            texLib["rock1_normal"],
            texLib["rock1_gloss_height_ao"],
            DirectX::XMFLOAT4(1.0f, 1.0f, 1.0f, 1.0f),
            DirectX::XMFLOAT3(0.1f, 0.1f, 0.1f),
            0.0f, 1.3f,
            DirectX::SimpleMath::Matrix::CreateScale(32.0f));

        const auto transparencyScale = 0.7f;
        const auto indexOfRefraction = 1.06f;
        AddMaterial("glass0",
            texLib["defaultDiffuseMap"],
            texLib["defaultNormalMap"],
            texLib["defaultGlossHeightAoMap"],
            DirectX::XMFLOAT4(0.0f, 0.0f, 0.0f, 1.0f),
            DirectX::XMFLOAT3(0.4f, 0.4f, 0.4f),
            0.1f,
            1.0f,
            MathHelper::Identity4x4,
            transparencyScale,
            indexOfRefraction);

        mIsInitialized = true;
    }

    auto AddMaterial(
        const std::string& name, 
        Texture* albedoMap, 
        Texture* normalMap, 
        Texture* glossHeightAoMap,
        const DirectX::XMFLOAT4& diffuse, 
        const DirectX::XMFLOAT3& fresnel, 
        float roughness,
        float displacementScale = 1.0f,
        const DirectX::XMFLOAT4X4& matTransform = MathHelper::Identity4x4,
        float transparency = 0.0f,
        float indexOfRefraction = 0.0f
    ) -> bool
    {
        if (mMaterials.find(name) != mMaterials.end())
			return false;
        
        static auto matIndex = 0;

        auto mat = std::make_unique<Material>();
        mat->Name = name;
        mat->MatIndex = matIndex;
        mat->AlbedoBindlessIndex = albedoMap != nullptr ? albedoMap->BindlessIndex : -1;
        mat->NormalBindlessIndex = normalMap != nullptr ? normalMap->BindlessIndex : -1;
        mat->GlossHeightAoBindlessIndex = glossHeightAoMap != nullptr ? glossHeightAoMap->BindlessIndex : -1;

        mat->DiffuseAlbedo = diffuse;
        mat->FresnelR0 = fresnel;
        mat->Roughness = roughness;
        mat->DisplacementScale = displacementScale;
        mat->MatTransform = matTransform;

        // Used in ray tracing demos only.
        mat->TransparencyWeight = transparency;
        mat->IndexOfRefraction = indexOfRefraction;

        matIndex++;

        mMaterials[name] = std::move(mat);
        return true;
    }

    auto operator[](const std::string& name) -> Material*
    {
        if (mMaterials.find(name) != mMaterials.end())
            return mMaterials[name].get();
        return nullptr;
    }

    auto GetCollection()const -> const std::unordered_map<std::string, std::unique_ptr<Material>>&
    {
        return mMaterials;
    }
private:
    MaterialLib() = default;

protected:
    bool mIsInitialized = false;
    std::unordered_map<std::string, std::unique_ptr<Material>> mMaterials;
};