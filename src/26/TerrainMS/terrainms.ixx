export module terrainmsdemo:terrainms;
import std;
import shared; 
import :frameresource;

class TerrainMS
{
public:
	//using Vector2 = DirectX::SimpleMath::Vector2;
	//using Matrix = DirectX::SimpleMath::Matrix;

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
	TerrainMS(D3D12::ID3D12Device* device, DirectX::ResourceUploadBatch& uploadBatch, const InitInfo& initInfo)
		: md3dDevice(device), mInfo(initInfo)
	{
		// Divide heightmap into patches such that each patch has CellsPerQuadPatch.
		mNumPatchVertRows = ((mInfo.HeightmapHeight - 1) / CellsPerQuadPatch) + 1;
		mNumPatchVertCols = ((mInfo.HeightmapWidth - 1) / CellsPerQuadPatch) + 1;

		//assert(mNumPatchVertRows == mNumPatchVertCols);

		const std::uint32_t numPatchQuadsX = mNumPatchVertCols - 1;
		const std::uint32_t numPatchQuadsY = mNumPatchVertRows - 1;
		mNumAmplificationGroupsX = (numPatchQuadsX + mNumQuadsPerGroupX - 1) / mNumQuadsPerGroupX;
		mNumAmplificationGroupsY = (numPatchQuadsY + mNumQuadsPerGroupY - 1) / mNumQuadsPerGroupY;

		// Shader does not have proper bounds checking if the number of patches 
		// is not evenly divisible by the thread group count.
		//assert(numPatchQuadsX % mNumQuadsPerGroupX == 0);
		//assert(numPatchQuadsY % mNumQuadsPerGroupY == 0);

		mNumPatchVertices = mNumPatchVertRows * mNumPatchVertCols;
		mNumPatchQuadFaces = (mNumPatchVertRows - 1) * (mNumPatchVertCols - 1);

		LoadHeightmapRaw16();
		CalcAllQuadGroupBounds();
		CalcAllQuadPatchBoundsY();
		BuildQuadPatchVB(uploadBatch);
		BuildQuadGroupBoundsBuffer(uploadBatch);
		BuildHeightMapTexture(uploadBatch);
	}

	TerrainMS(const TerrainMS& rhs) = delete;
	TerrainMS& operator=(const TerrainMS& rhs) = delete;

	void BuildDescriptors()
	{
		auto& heap = CbvSrvUavHeap::Get();

		mHeightMapSrvIndex = heap.NextFreeIndex();
		mTerrainVerticesSrvIndex = heap.NextFreeIndex();
		mTerrainGroupBoundsSrvIndex = heap.NextFreeIndex();

		CreateSrv2d(md3dDevice, mHeightMapTexture.Get(), DXGI_FORMAT_R32_FLOAT, 1, heap.CpuHandle(mHeightMapSrvIndex));

		CreateBufferSrv(md3dDevice, 0, mNumPatchVertRows * mNumPatchVertCols, sizeof(DirectX::XMFLOAT4), mQuadPatchVB.Get(), heap.CpuHandle(mTerrainVerticesSrvIndex));
		CreateBufferSrv(md3dDevice, 0, static_cast<std::uint32_t>(mGroupBounds.size()), sizeof(DirectX::BoundingBox), mQuadGroupBoundsBuffer.Get(), heap.CpuHandle(mTerrainGroupBoundsSrvIndex));
	}

	void SetMaterialLayers(std::initializer_list<Material*> layers,
		std::uint32_t blendMap0SrvIndex, std::uint32_t blendMap1SrvIndex)
	{
		//assert(layers.size() <= MaxTerrainLayers);
		mLayerMaterials = layers;

		mBlendMap0SrvIndex = blendMap0SrvIndex;
		mBlendMap1SrvIndex = blendMap1SrvIndex;
	}

	void SetSkirtOffsetY(float value)
	{
		mSkirtOffsetY = value;
	}
	void SetMinTessDist(float value)
	{
		mMinTessDist = value;
	}
	void SetMaxTessDist(float value)
	{
		mMaxTessDist = value;
	}
	void SetMaxTess(float maxTess)
	{
		mMaxTess = maxTess;
	}

	auto GetWidth()const->float
	{
		// Total terrain width.
		return (mInfo.HeightmapWidth - 1) * mInfo.CellSpacing;
	}

	auto GetDepth()const->float
	{
		// Total terrain depth.
		return (mInfo.HeightmapHeight - 1) * mInfo.CellSpacing;
	}

	auto GetHeight(float x, float z)const->float
	{
		// Transform from terrain local space to "cell" space.
		float c = (x + 0.5f * GetWidth()) / mInfo.CellSpacing;
		float d = (z - 0.5f * GetDepth()) / -mInfo.CellSpacing;

		// Get the row and column we are in.
		int row = (int)std::floorf(d);
		int col = (int)std::floorf(c);

		// Grab the heights of the cell we are in.
		// A*--*B
		//  | /|
		//  |/ |
		// C*--*D
		float A = mHeightmap[row * mInfo.HeightmapWidth + col];
		float B = mHeightmap[row * mInfo.HeightmapWidth + col + 1];
		float C = mHeightmap[(row + 1) * mInfo.HeightmapWidth + col];
		float D = mHeightmap[(row + 1) * mInfo.HeightmapWidth + col + 1];

		// Where we are relative to the cell.
		float s = c - (float)col;
		float t = d - (float)row;

		// If upper triangle ABC.
		if (s + t <= 1.0f)
		{
			float uy = B - A;
			float vy = C - A;
			return A + s * uy + t * vy;
		}
		else // lower triangle DCB.
		{
			float uy = C - D;
			float vy = B - D;
			return D + (1.0f - s) * uy + (1.0f - t) * vy;
		}
	}

	auto GetWorld()const -> DirectX::SimpleMath::Matrix
	{
		return mWorld;
	}

	void SetWorld(const DirectX::SimpleMath::Matrix& W)
	{
		mWorld = W;
	}

	void Draw(D3D12::ID3D12GraphicsCommandList6* cmdList,
		D3D12::ID3D12PipelineState* drawTerrainPso,
		D3D12::ID3D12PipelineState* drawTerrainSkirtPso,
		bool drawSkirts)
	{
		auto drawCB = PerTerrainCB{
			.gTerrainWorld = mWorld
		};

		auto matIndices = std::array<std::uint32_t, MaximumTerrainLayers>{};

		for (auto i = 0u; i < mInfo.NumLayers; ++i)
		{
			matIndices[i] = mLayerMaterials[i]->MatIndex;
		}

		drawCB.gTerrainLayerMaterialIndices[0] = DirectX::XMUINT4(matIndices[0], matIndices[1], matIndices[2], matIndices[3]);
		drawCB.gTerrainLayerMaterialIndices[1] = DirectX::XMUINT4(matIndices[4], matIndices[5], matIndices[6], matIndices[7]);

		drawCB.gTerrainWorldCellSpacing = MathHelper::Vector2(mInfo.CellSpacing);
		drawCB.gTerrainWorldSize = MathHelper::Vector2(GetWidth(), GetDepth());

		drawCB.gTerrainHeightMapSize.x = static_cast<float>(mInfo.HeightmapWidth);
		drawCB.gTerrainHeightMapSize.y = static_cast<float>(mInfo.HeightmapHeight);

		drawCB.gTerrainTexelSizeUV.x = 1.0f / static_cast<float>(mInfo.HeightmapWidth);
		drawCB.gTerrainTexelSizeUV.y = 1.0f / static_cast<float>(mInfo.HeightmapHeight);

		drawCB.gTerrainMinTessDist = mMinTessDist;
		drawCB.gTerrainMaxTessDist = mMaxTessDist;
		drawCB.gTerrainMinTess = 0.0f;
		drawCB.gTerrainMaxTess = mMaxTess;

		drawCB.gBlendMap0SrvIndex = mBlendMap0SrvIndex;
		drawCB.gBlendMap1SrvIndex = mBlendMap1SrvIndex;
		drawCB.gHeightMapSrvIndex = mHeightMapSrvIndex;
		drawCB.gNumTerrainLayers = mInfo.NumLayers;

		// Assume square.
		//assert(mNumPatchVertRows == mNumPatchVertCols);
		drawCB.gNumQuadVertsPerTerrainSide = mNumPatchVertCols;

		drawCB.gTerrainVerticesSrvIndex = mTerrainVerticesSrvIndex;
		drawCB.gTerrainGroupBoundsSrvIndex = mTerrainGroupBoundsSrvIndex;

		drawCB.gNumAmplificationGroupsX = mNumAmplificationGroupsX;
		drawCB.gNumAmplificationGroupsY = mNumAmplificationGroupsY;

		drawCB.gSkirtOffsetY = mSkirtOffsetY;

		auto& linearAllocator = DirectX::GraphicsMemory::Get(md3dDevice);
		mDrawConstants = linearAllocator.AllocateConstant(drawCB);
		cmdList->SetGraphicsRootConstantBufferView(GFX_ROOT_ARG_OBJECT_CBV, mDrawConstants.GpuAddress());

		cmdList->SetPipelineState(drawTerrainPso);
		cmdList->DispatchMesh(mNumAmplificationGroupsX, mNumAmplificationGroupsY, 1);

		if (drawSkirts)
		{
			cmdList->SetPipelineState(drawTerrainSkirtPso);
			cmdList->DispatchMesh(mNumAmplificationGroupsX, mNumAmplificationGroupsY, 1);
		}
	}

private:
	void LoadHeightmapRaw16()
	{
		// A 16-bit height for each vertex
		auto in = std::vector<std::uint16_t>(mInfo.HeightmapWidth * mInfo.HeightmapHeight);

		// Open the file.
		auto absolutePath = std::filesystem::absolute(mInfo.HeightMapFilename);
		auto inFile = std::ifstream{ absolutePath, std::ios_base::binary };
		if (inFile)
		{
			// Read the RAW bytes.
			inFile.read((char*)&in[0], (std::streamsize)(in.size() * sizeof(std::uint16_t)));
			// Done with file.
			inFile.close();
		}

		float MaxUShort = static_cast<float>(std::numeric_limits<std::uint16_t>::max());

		// Copy the array data into a float array and scale it.
		mHeightmap.resize(in.size(), 0);
		for (auto i = 0u; i < in.size(); ++i)
		{
			auto heightUnorm = float{ in[i] / MaxUShort };
			mHeightmap[i] = mInfo.HeightScale * heightUnorm + mInfo.HeightOffset;
		}
	}

	void CalcAllQuadGroupBounds()
	{
		mGroupBounds.resize(mNumAmplificationGroupsX * mNumAmplificationGroupsY);

		// Computing a bounding box around each group of quad patches for amplification shader culling.
		for (auto groupY = 0u; groupY < mNumAmplificationGroupsY; ++groupY)
		{
			for (auto groupX = 0u; groupX < mNumAmplificationGroupsX; ++groupX)
			{
				auto groupBox = DirectX::BoundingBox{CalcQuadGroupBounds(groupX, groupY)};
				mGroupBounds[groupY * mNumAmplificationGroupsY + groupX] = groupBox;
			}
		}
	}


	auto CalcQuadGroupBounds(std::uint32_t groupX, std::uint32_t groupY) -> DirectX::BoundingBox
	{
		auto halfWidth = 0.5f * GetWidth();
		auto halfDepth = 0.5f * GetDepth();

		auto groupWidth = GetWidth() / mNumAmplificationGroupsX;
		auto groupDepth = GetDepth() / mNumAmplificationGroupsY;

		// Scan the heightmap values this patch covers and compute the min/max height.

		auto x0 = std::uint32_t{groupX * mNumQuadsPerGroupX * CellsPerQuadPatch};
		auto x1 = std::uint32_t{(groupX + 1) * mNumQuadsPerGroupX * CellsPerQuadPatch};

		auto y0 = std::uint32_t{groupY * mNumQuadsPerGroupY * CellsPerQuadPatch};
		auto y1 = std::uint32_t{(groupY + 1) * mNumQuadsPerGroupY * CellsPerQuadPatch};

		auto minY = +MathHelper::Infinity;
		auto maxY = -MathHelper::Infinity;
		for (auto y = y0; y <= y1; ++y)
		{
			for (auto x = x0; x <= x1; ++x)
			{
				auto k = y * mInfo.HeightmapWidth + x;
				minY = std::min(minY, mHeightmap[k]);
				maxY = std::max(maxY, mHeightmap[k]);
			}
		}

		auto groupCenterX = -halfWidth + groupX * groupWidth + 0.5f * groupWidth;
		auto groupCenterZ = halfDepth - groupY * groupDepth - 0.5f * groupDepth;
		auto groupCenterY = 0.5f * (minY + maxY);

		auto extentX = 0.5f * groupWidth;
		auto extentY = 0.5f * (maxY - minY);
		auto extentZ = 0.5f * groupDepth;

		auto box = DirectX::BoundingBox{};
		box.Center = DirectX::XMFLOAT3(groupCenterX, groupCenterY, groupCenterZ);
		box.Extents = DirectX::XMFLOAT3(extentX, extentY, extentZ);

		return box;
	}

	void CalcAllQuadPatchBoundsY()
	{
		mQuadPatchBoundsY.resize(mNumPatchQuadFaces);
		// For each patch
		for (auto i = 0u; i < mNumPatchVertRows - 1; ++i)
		{
			for (auto j = 0u; j < mNumPatchVertCols - 1; ++j)
			{
				CalcQuadPatchBoundsY(i, j);
			}
		}
	}

	void CalcQuadPatchBoundsY(std::uint32_t i, std::uint32_t j)
	{
		// Scan the heightmap values this patch covers and compute the min/max height.
		auto x0 = std::uint32_t{j * CellsPerQuadPatch};
		auto x1 = std::uint32_t{(j + 1) * CellsPerQuadPatch};
		auto y0 = std::uint32_t{i * CellsPerQuadPatch};
		auto y1 = std::uint32_t{(i + 1) * CellsPerQuadPatch};

		auto minY = +MathHelper::Infinity;
		auto maxY = -MathHelper::Infinity;
		for (auto y = y0; y <= y1; ++y)
		{
			for (auto x = x0; x <= x1; ++x)
			{
				auto k = std::uint32_t{y * mInfo.HeightmapWidth + x};
				minY = std::min(minY, mHeightmap[k]);
				maxY = std::max(maxY, mHeightmap[k]);
			}
		}

		auto patchID = std::uint32_t{i * (mNumPatchVertCols - 1) + j};
		mQuadPatchBoundsY[patchID] = DirectX::XMFLOAT2(minY, maxY);
	}

	void BuildQuadPatchVB(DirectX::ResourceUploadBatch& uploadBatch)
	{
		auto patchVertices = std::vector<DirectX::XMFLOAT4>(mNumPatchVertRows * mNumPatchVertCols);

		auto halfWidth = float{0.5f * GetWidth()};
		auto halfDepth = float{0.5f * GetDepth()};

		auto patchWidth = float{GetWidth() / (mNumPatchVertCols - 1)};
		auto patchDepth = float{GetDepth() / (mNumPatchVertRows - 1)};

		for (auto i = 0u; i < mNumPatchVertRows; ++i)
		{
			auto z = float{halfDepth - i * patchDepth};
			for (auto j = 0u; j < mNumPatchVertCols; ++j)
			{
				auto x = float{-halfWidth + j * patchWidth};

				// xy: Patch 2d point position in xz-plane.
				patchVertices[i * mNumPatchVertCols + j] = DirectX::XMFLOAT4(x, z, 0.0f, 0.0f);
			}
		}

		// Store axis-aligned bounding box y-bounds in upper-left patch corner.
		for (auto i = 0u; i < mNumPatchVertRows - 1; ++i)
		{
			for (auto j = 0u; j < mNumPatchVertCols - 1; ++j)
			{
				auto patchID = std::uint32_t{i * (mNumPatchVertCols - 1) + j};

				// zw: Patch axis y-bounds.
				patchVertices[i * mNumPatchVertCols + j].z = mQuadPatchBoundsY[patchID].x;
				patchVertices[i * mNumPatchVertCols + j].w = mQuadPatchBoundsY[patchID].y;
			}
		}


		CreateStaticBuffer(md3dDevice, uploadBatch,
			patchVertices.data(), patchVertices.size(), sizeof(DirectX::XMFLOAT4),
			D3D12::D3D12_RESOURCE_STATES::D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER, mQuadPatchVB.GetAddressOf(),
			D3D12::D3D12_RESOURCE_FLAGS::D3D12_RESOURCE_FLAG_NONE);
	}

	void BuildQuadGroupBoundsBuffer(DirectX::ResourceUploadBatch& uploadBatch)
	{
		CreateStaticBuffer(md3dDevice, uploadBatch,
			mGroupBounds.data(), mGroupBounds.size(), sizeof(DirectX::BoundingBox),
			D3D12::D3D12_RESOURCE_STATES::D3D12_RESOURCE_STATE_GENERIC_READ, mQuadGroupBoundsBuffer.GetAddressOf(),
			D3D12::D3D12_RESOURCE_FLAGS::D3D12_RESOURCE_FLAG_NONE);
	}

	void BuildHeightMapTexture(DirectX::ResourceUploadBatch& uploadBatch)
	{
		 auto subResourceData = D3D12::D3D12_SUBRESOURCE_DATA{
			.pData = mHeightmap.data(),
			.RowPitch = mInfo.HeightmapWidth * sizeof(float),
			.SlicePitch = 0,
		};

		ThrowIfFailed(CreateTextureFromMemory(md3dDevice,
			uploadBatch,
			mInfo.HeightmapWidth, mInfo.HeightmapHeight,
			DXGI::DXGI_FORMAT::DXGI_FORMAT_R32_FLOAT,
			subResourceData,
			&mHeightMapTexture,
			false,
			D3D12::D3D12_RESOURCE_STATES::D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE));
	}

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

	// TODO: weird undefined symbol error, I think it's a compiler bug, but will need to confirm
	//DirectX::SimpleMath::Matrix mWorld2 = DirectX::SimpleMath::Matrix::Identity; 
	DirectX::SimpleMath::Matrix mWorld = 
		DirectX::SimpleMath::Matrix{ 
			1.0f, 0.0f, 0.0f, 0.0f,
			0.0f, 1.0f, 0.0f, 0.0f,
			0.0f, 0.0f, 1.0f, 0.0f,
			0.0f, 0.0f, 0.0f, 1.0f
		};
		

	std::vector<DirectX::BoundingBox> mGroupBounds;
	std::vector<DirectX::SimpleMath::Vector2> mQuadPatchBoundsY;
	std::vector<float> mHeightmap;

	std::vector<Material*> mLayerMaterials;

	float mSkirtOffsetY = 2.0f;
	float mMaxTess = 6.0f;
	float mMinTessDist = 50;
	float mMaxTessDist = 250;
};