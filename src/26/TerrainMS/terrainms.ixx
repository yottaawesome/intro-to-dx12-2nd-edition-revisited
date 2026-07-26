export module terrainmsdemo:terrainms;
import std;
import shared; 

class TerrainMS
{
public:
	using Vector2 = DirectX::SimpleMath::Vector2;
	using Matrix = DirectX::SimpleMath::Matrix;

	struct InitInfo
	{
		std::wstring HeightMapFilename;
		float HeightScale = 0;
		float HeightOffset = 0;
		std::uint32_t HeightmapWidth = 0;
		std::uint32_t HeightmapHeight = 0;
		float CellSpacing = 0;
		std::uint32_t NumLayers = 0;
	};

public:
	TerrainMS(D3D12::ID3D12Device* device, DirectX::ResourceUploadBatch& uploadBatch, const InitInfo& initInfo);
	TerrainMS(const TerrainMS& rhs) = delete;
	~TerrainMS();
	TerrainMS& operator=(const TerrainMS& rhs) = delete;

	void BuildDescriptors();

	void SetMaterialLayers(std::initializer_list<Material*> layers,
		std::uint32_t blendMap0SrvIndex, std::uint32_t blendMap1SrvIndex);

	void SetSkirtOffsetY(float value);
	void SetMinTessDist(float value);
	void SetMaxTessDist(float value);
	void SetMaxTess(float maxTess);

	float GetWidth()const;
	float GetDepth()const;
	float GetHeight(float x, float z)const;

	Matrix GetWorld()const;
	void SetWorld(const Matrix& W);

	void Draw(D3D12::ID3D12GraphicsCommandList6* cmdList,
		D3D12::ID3D12PipelineState* drawTerrainPso,
		D3D12::ID3D12PipelineState* drawTerrainSkirtPso,
		bool drawSkirts);

private:
	void LoadHeightmapRaw16();
	void CalcAllQuadGroupBounds();
	DirectX::BoundingBox CalcQuadGroupBounds(std::uint32_t groupX, std::uint32_t groupY);
	void CalcAllQuadPatchBoundsY();
	void CalcQuadPatchBoundsY(std::uint32_t i, std::uint32_t j);
	void BuildQuadPatchVB(DirectX::ResourceUploadBatch& uploadBatch);
	void BuildQuadGroupBoundsBuffer(DirectX::ResourceUploadBatch& uploadBatch);
	void BuildHeightMapTexture(DirectX::ResourceUploadBatch& uploadBatch);

private:
	D3D12::ID3D12Device* md3dDevice = nullptr;

	// Divide heightmap into quad patches such that each quad patch has 
	// CellsPerQuadPatch cells and CellsPerQuadPatch+1 vertices.  
	static const int CellsPerQuadPatch = 32;

	Microsoft::WRL::ComPtr<D3D12::ID3D12Resource> mQuadGroupBoundsBuffer = nullptr;
	Microsoft::WRL::ComPtr<D3D12::ID3D12Resource> mQuadPatchVB = nullptr;
	Microsoft::WRL::ComPtr<D3D12::ID3D12Resource> mHeightMapTexture = nullptr;

	DirectX::GraphicsResource mDrawConstants;

	std::uint32_t mHeightMapSrvIndex = -1;
	std::uint32_t mBlendMap0SrvIndex = -1;
	std::uint32_t mBlendMap1SrvIndex = -1;

	std::uint32_t mTerrainVerticesSrvIndex = -1;
	std::uint32_t mTerrainGroupBoundsSrvIndex = -1;

	InitInfo mInfo;

	std::uint32_t mNumPatchVertices = 0;
	std::uint32_t mNumPatchQuadFaces = 0;

	std::uint32_t mNumPatchVertRows = 0;
	std::uint32_t mNumPatchVertCols = 0;

	// Each thread in the group processes a quad patch.
	const std::uint32_t mNumQuadsPerGroupX = 8;
	const std::uint32_t mNumQuadsPerGroupY = 8;

	std::uint32_t mNumAmplificationGroupsX = 0;
	std::uint32_t mNumAmplificationGroupsY = 0;

	Matrix mWorld = Matrix::Identity;

	std::vector<DirectX::BoundingBox> mGroupBounds;
	std::vector<Vector2> mQuadPatchBoundsY;
	std::vector<float> mHeightmap;

	std::vector<Material*> mLayerMaterials;

	float mSkirtOffsetY = 2.0f;
	float mMaxTess = 6.0f;
	float mMinTessDist = 50;
	float mMaxTessDist = 250;
};