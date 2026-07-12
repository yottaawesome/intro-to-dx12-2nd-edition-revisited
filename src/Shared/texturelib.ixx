export module shared:texturelib;
import std;
import :win32;
import :d3dutil;

export struct Texture
{
    Texture() = default;
    Texture(const Texture& rhs) = delete;
    Texture& operator = (const Texture& rhs) = delete;

    // Unique material name for lookup.
    std::string Name;
    std::wstring Filename;
    bool IsCubeMap = false;
    int BindlessIndex = -1;
	// original returns `const D3D12::D3D12_RESOURCE_DESC&`, which raises a warning of returning reference to temporary.
    auto Info()const -> D3D12::D3D12_RESOURCE_DESC { return Resource->GetDesc(); }
    Microsoft::WRL::ComPtr<D3D12::ID3D12Resource> Resource = nullptr;
};

// Creates all textures used in the book demos in one place so we do not 
// have to duplicate across demos.
export class TextureLib
{
public:
    TextureLib(const TextureLib& rhs) = delete;
    TextureLib& operator=(const TextureLib& rhs) = delete;

    static auto GetLib() -> TextureLib&
    {
        static auto singleton = TextureLib{};
        return singleton;
    }

    auto IsInitialized()const -> bool
    {
        return mIsInitialized;
    }

    void Init(D3D12::ID3D12Device* device, DirectX::ResourceUploadBatch& uploadBatch)
    {
        auto texNames = std::vector<std::string>{
            "crateDiffuseMap",
            "waterDiffuseMap",
            "fenceDiffuseMap",
            "grassDiffuseMap",

            "bricksDiffuseMap",
            "bricksNormalMap",
            "bricksGlossHeightAoMap",

            "tileDiffuseMap",

            "stoneFloorDiffuseMap",
            "stoneFloorNormalMap",
            "stoneFloorGlossHeightAoMap",

            "checkboardMap",
            "iceMap",

            "treeSpritesArray",

            "defaultDiffuseMap",
            "defaultNormalMap",
            "defaultGlossHeightAoMap",

            "rainParticle",
            "explosionParticle",
            "boltParticles",
            "skyCubeMap",

            "blendMap0",
            "blendMap1",

            "grass_color",
            "grass_normal",
            "grass_gloss_height_ao",

            "sand_color",
            "sand_normal",
            "sand_gloss_height_ao",

            "rock_color",
            "rock_normal",
            "rock_gloss_height_ao",

            "dirt0_color",
            "dirt0_normal",
            "dirt0_gloss_height_ao",

            "dirt1_color",
            "dirt1_normal",
            "dirt1_gloss_height_ao",

            "trail_color",
            "trail_normal",
            "trail_gloss_height_ao",

            "rock1_color",
            "rock1_normal",
            "rock1_gloss_height_ao",

            "columnRoundDiffuseMap",
            "columnRoundNormalMap",
            "columnRoundGlossHeightAoMap",

            "columnSquareDiffuseMap",
            "columnSquareNormalMap",
            "columnSquareGlossHeightAoMap",

            "orbBaseDiffuseMap",
            "orbBaseNormalMap",
            "orbBaseGlossHeightAoMap",
        };

        auto texFilenames = std::vector<std::wstring>{
            L"Textures/WoodCrate01.dds",
            L"Textures/water1.dds",
            L"Textures/WireFence.dds",
            L"Textures/grass.dds",

            L"Textures/bricks0_color.dds",
            L"Textures/bricks0_normal.dds",
            L"Textures/bricks0_gloss_height_ao.dds",

            L"Textures/tile0.dds",

            L"Textures/stonefloor1_diffuse.dds",
            L"Textures/stonefloor1_normal.dds",
            L"Textures/stonefloor1_gloss_height_ao.dds",

            L"Textures/checkboard.dds",
            L"Textures/ice.dds",

            L"Textures/treeArray2.dds",

            L"Textures/white1x1.dds",
            L"Textures/default_nmap.dds",
            L"Textures/default_glossHeightAoMap.dds",

            L"Textures/raindrop.dds",
            L"Textures/explosion.dds",
            L"Textures/bolt.dds",

            L"Textures/cubemaps/cubemap_sunset2.dds",

            L"Textures/terrain/blendmap0.dds",
            L"Textures/terrain/blendmap1.dds",

            L"Textures/terrain/grass0_color.dds",
            L"Textures/terrain/grass0_normal.dds",
            L"Textures/terrain/grass0_gloss_height_ao.dds",

            L"Textures/terrain/sand0_color.dds",
            L"Textures/terrain/sand0_normal.dds",
            L"Textures/terrain/sand0_gloss_height_ao.dds",

            L"Textures/terrain/rock0_color.dds",
            L"Textures/terrain/rock0_normal.dds",
            L"Textures/terrain/rock0_gloss_height_ao.dds",

            L"Textures/terrain/dirt0_color.dds",
            L"Textures/terrain/dirt0_normal.dds",
            L"Textures/terrain/dirt0_gloss_height_ao.dds",

            L"Textures/terrain/dirt1_color.dds",
            L"Textures/terrain/dirt1_normal.dds",
            L"Textures/terrain/dirt1_gloss_height_ao.dds",

            L"Textures/terrain/gravel0_color.dds",
            L"Textures/terrain/gravel0_normal.dds",
            L"Textures/terrain/gravel0_gloss_height_ao.dds",

            L"Textures/terrain/rock1_color.dds",
            L"Textures/terrain/rock1_normal.dds",
            L"Textures/terrain/rock1_gloss_height_ao.dds",

            L"Textures/models/columnRound_diffuse.dds",
            L"Textures/models/columnRound_normal.dds",
            L"Textures/models/columnRound_gloss_height_ao.dds",

            L"Textures/models/columnSquare_diffuse.dds",
            L"Textures/models/columnSquare_normal.dds",
            L"Textures/models/columnSquare_gloss_height_ao.dds",

            L"Textures/models/orbBase_diffuse.dds",
            L"Textures/models/orbBase_normal.dds",
            L"Textures/models/orbBase_gloss_height_ao.dds",
        };

        for (int i = 0; i < (int)texNames.size(); ++i)
        {
            auto texMap = std::make_unique<Texture>();
            texMap->Name = texNames[i];
            texMap->Filename = texFilenames[i];

            if (not std::filesystem::exists(texFilenames[i]))
            {
                auto msg = std::wstring{texFilenames[i] + L" not found."};
                Win32::OutputDebugStringW(msg.c_str());
                Win32::MessageBoxW(0, msg.c_str(), 0, 0);
            }

            auto hr = DirectX::CreateDDSTextureFromFileEx(
                device, uploadBatch,
                texMap->Filename.c_str(), 
                0, 
                D3D12::D3D12_RESOURCE_FLAGS::D3D12_RESOURCE_FLAG_NONE,
                DirectX::DDS_LOADER_FLAGS::DDS_LOADER_DEFAULT,
                &texMap->Resource, 
                nullptr, 
                &texMap->IsCubeMap);
			if (Win32::Failed(hr))
				throw std::runtime_error{ std::format("CreateDDSTextureFromFileEx failed for {}", WStringToAnsi(texMap->Filename)) };

            mTextures[texMap->Name] = std::move(texMap);
        }

        auto randomTex = std::make_unique<Texture>();
        randomTex->Name = "randomTex1024";
        randomTex->Filename = L"";
        randomTex->IsCubeMap = false;
        randomTex->Resource = d3dUtil::CreateRandomTexture(device, uploadBatch, 1024, 1024);

        mTextures[randomTex->Name] = std::move(randomTex);

        mIsInitialized = true;
    }

	auto Contains(const std::string& name) -> bool
    {
        return mTextures.find(name) != mTextures.end();
    }

    auto AddTexture(const std::string& name, std::unique_ptr<Texture> tex) -> bool
    {
        if (mTextures.find(name) == mTextures.end())
        {
            mTextures[name] = std::move(tex);
            return true;
        }

        return false;
    }

    auto operator[](const std::string& name) -> Texture*
    {
        if (mTextures.find(name) != mTextures.end())
            return mTextures[name].get();
        return nullptr;
    }

    auto GetCollection()const -> const std::unordered_map<std::string, std::unique_ptr<Texture>>&
    {
        return mTextures;
    }
private:
    TextureLib() = default;

protected:
    bool mIsInitialized = false;
    std::unordered_map<std::string, std::unique_ptr<Texture>> mTextures;
};
