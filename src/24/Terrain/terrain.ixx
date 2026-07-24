export module terraindemo:terrain;
import std;
import shared;

class Terrain
{
public:

	struct InitInfo
	{
		// Filename of RAW heightmap data.
		std::wstring HeightMapFilename;

		// Scale and offset to apply to heights after they have been
		// loaded from the heightmap.
		float HeightScale = 1.0f;
		float HeightOffset = 0.0f;

		// Dimensions of the heightmap.
		std::uint32_t HeightmapWidth = 0;
		std::uint32_t HeightmapHeight = 0;

		// The world spacing between heightmap samples. 
		float CellSpacing = 1.0f;

		// The number of material layers.
		std::uint32_t NumLayers = 0;
	};

public:

	Terrain(D3D12::ID3D12Device* device, DirectX::ResourceUploadBatch& uploadBatch, const InitInfo& initInfo);
	Terrain(const Terrain& rhs) = delete;
	Terrain& operator=(const Terrain& rhs) = delete;

	void BuildDescriptors();

	void SetMaterialLayers(std::initializer_list<Material*> layers,
		std::uint32_t blendMap0SrvIndex, std::uint32_t blendMap1SrvIndex);

	void SetMaxTess(float maxTess);
	void SetMinTessDist(float value);
	void SetMaxTessDist(float value);

	void SetUseTerrainHeightMap(bool value);
	void SetUseMaterialHeightMaps(bool value);

	float GetWidth()const;
	float GetDepth()const;
	float GetHeight(float x, float z)const;

	DirectX::XMFLOAT4X4 GetWorld()const;
	void SetWorld(const DirectX::XMFLOAT4X4& W);

	void Draw(D3D12::ID3D12GraphicsCommandList* cmdList, D3D12::ID3D12PipelineState* drawTerrainPso);

private:
	void LoadHeightmapRaw16();
	void CalcAllPatchBoundsY();
	void CalcPatchBoundsY(std::uint32_t i, std::uint32_t j);
	void BuildQuadPatchVB(DirectX::ResourceUploadBatch& uploadBatch);
	void BuildQuadPatchIB(DirectX::ResourceUploadBatch& uploadBatch);
	void BuildHeightMapTexture(DirectX::ResourceUploadBatch& uploadBatch);

private:

	D3D12::ID3D12Device* md3dDevice = nullptr;

	// Divide heightmap into patches such that each patch has CellsPerPatch cells
	// and CellsPerPatch+1 vertices.  
	// Note: Can't make this too small without going to 32-bit indices.
	static const int CellsPerPatch = 32;

	Microsoft::WRL::ComPtr<D3D12::ID3D12Resource> mQuadPatchVB = nullptr;
	Microsoft::WRL::ComPtr<D3D12::ID3D12Resource> mQuadPatchIB = nullptr;
	Microsoft::WRL::ComPtr<D3D12::ID3D12Resource> mHeightMapTexture = nullptr;

	DirectX::GraphicsResource mDrawConstants;

	std::uint32_t mHeightMapSrvIndex = -1;
	std::uint32_t mBlendMap0SrvIndex = -1;
	std::uint32_t mBlendMap1SrvIndex = -1;

	InitInfo mInfo;

	std::uint32_t mNumPatchVertices = 0;
	std::uint32_t mNumPatchQuadFaces = 0;

	std::uint32_t mNumPatchVertRows = 0;
	std::uint32_t mNumPatchVertCols = 0;

	DirectX::XMFLOAT4X4 mWorld = MathHelper::Identity4x4;

	std::vector<DirectX::XMFLOAT2> mPatchBoundsY;
	std::vector<float> mHeightmap;

	std::vector<Material*> mLayerMaterials;

	float mMaxTess = 6.0f;
	float mMinTessDist = 20;
	float mMaxTessDist = 250;

	bool mUseTerrainHeightMap = true;
	bool mUseMaterialHeightMaps = true;
};
