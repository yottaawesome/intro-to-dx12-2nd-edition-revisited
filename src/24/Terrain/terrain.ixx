export module terraindemo:terrain;
import std;
import shared;
import :frameresource;

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
	Terrain(D3D12::ID3D12Device* device, DirectX::ResourceUploadBatch& uploadBatch, const InitInfo& initInfo)
	{
		// Divide heightmap into patches such that each patch has CellsPerPatch.
		mNumPatchVertRows = ((mInfo.HeightmapHeight - 1) / CellsPerPatch) + 1;
		mNumPatchVertCols = ((mInfo.HeightmapWidth - 1) / CellsPerPatch) + 1;

		mNumPatchVertices = mNumPatchVertRows * mNumPatchVertCols;
		mNumPatchQuadFaces = (mNumPatchVertRows - 1) * (mNumPatchVertCols - 1);

		LoadHeightmapRaw16();
		CalcAllPatchBoundsY();

		BuildQuadPatchVB(uploadBatch);
		BuildQuadPatchIB(uploadBatch);
		BuildHeightMapTexture(uploadBatch);
	}

	Terrain(const Terrain&) = delete;
	auto operator=(const Terrain&) -> Terrain& = delete;

	void BuildDescriptors()
	{
		CbvSrvUavHeap& heap = CbvSrvUavHeap::Get();
		mHeightMapSrvIndex = heap.NextFreeIndex();

		CreateSrv2d(md3dDevice, mHeightMapTexture.Get(), DXGI_FORMAT_R32_FLOAT, 1, heap.CpuHandle(mHeightMapSrvIndex));
	}

	void SetMaterialLayers(std::initializer_list<Material*> layers,
		std::uint32_t blendMap0SrvIndex, std::uint32_t blendMap1SrvIndex)
	{
		//assert(layers.size() <= MaximumTerrainLayers);
		mLayerMaterials = layers;

		mBlendMap0SrvIndex = blendMap0SrvIndex;
		mBlendMap1SrvIndex = blendMap1SrvIndex;
	}

	void SetMaxTess(float maxTess)
	{
		mMaxTess = std::clamp(maxTess, 0.0f, 6.0f);
	}

	void SetMinTessDist(float value)
	{
		mMinTessDist = value;
	}

	void SetMaxTessDist(float value)
	{
		mMaxTessDist = value;
	}

	void SetUseTerrainHeightMap(bool value)
	{
		mUseTerrainHeightMap = value;
	}

	void SetUseMaterialHeightMaps(bool value)
	{
		mUseMaterialHeightMaps = value;
	}

	auto GetWidth()const -> float
	{
		// Total terrain width.
		return (mInfo.HeightmapWidth - 1) * mInfo.CellSpacing;
	}

	auto GetDepth()const -> float
	{
		// Total terrain depth.
		return (mInfo.HeightmapHeight - 1) * mInfo.CellSpacing;
	}

	auto GetHeight(float x, float z)const -> float
	{
		// Transform from terrain local space to "cell" space.
		auto c = float{(x + 0.5f * GetWidth()) / mInfo.CellSpacing};
		auto d = float{(z - 0.5f * GetDepth()) / -mInfo.CellSpacing};

		// Get the row and column we are in.
		auto row = static_cast<int>(std::floorf(d));
		auto col = static_cast<int>(std::floorf(c));

		// Grab the heights of the cell we are in.
		// A*--*B
		//  | /|
		//  |/ |
		// C*--*D
		auto A = float{mHeightmap[row * mInfo.HeightmapWidth + col]};
		auto B = float{mHeightmap[row * mInfo.HeightmapWidth + col + 1]};
		auto C = float{mHeightmap[(row + 1) * mInfo.HeightmapWidth + col]};
		auto D = float{mHeightmap[(row + 1) * mInfo.HeightmapWidth + col + 1]};

		// Where we are relative to the cell.
		auto s = float{c - static_cast<float>(col)};
		auto t = float{d - static_cast<float>(row)};

		// If upper triangle ABC.
		if (s + t <= 1.0f)
		{
			auto uy = float{B - A};
			auto vy = float{C - A};
			return A + s * uy + t * vy;
		}
		else // lower triangle DCB.
		{
			auto uy = float{C - D};
			auto vy = float{B - D};
			return D + (1.0f - s) * uy + (1.0f - t) * vy;
		}
	}

	auto GetWorld()const -> DirectX::XMFLOAT4X4
	{
		return mWorld;
	}

	void SetWorld(const DirectX::XMFLOAT4X4& W)
	{
		mWorld = W;
	}

	void Draw(D3D12::ID3D12GraphicsCommandList* cmdList, D3D12::ID3D12PipelineState* drawTerrainPso)
	{
		cmdList->SetPipelineState(drawTerrainPso);

		D3D12::D3D12_INDEX_BUFFER_VIEW ibv;
		ibv.BufferLocation = mQuadPatchIB->GetGPUVirtualAddress();
		ibv.Format = DXGI_FORMAT_R32_UINT;
		ibv.SizeInBytes = mNumPatchQuadFaces * 4 * sizeof(std::uint32_t);

		D3D12::D3D12_VERTEX_BUFFER_VIEW vbv;
		vbv.BufferLocation = mQuadPatchVB->GetGPUVirtualAddress();
		vbv.StrideInBytes = sizeof(DirectX::XMFLOAT4);
		vbv.SizeInBytes = mNumPatchVertRows * mNumPatchVertCols * sizeof(DirectX::XMFLOAT4);

		cmdList->IASetVertexBuffers(0, 1, &vbv);
		cmdList->IASetIndexBuffer(&ibv);
		cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_4_CONTROL_POINT_PATCHLIST);

		PerTerrainCB drawCB;
		drawCB.gTerrainWorld = mWorld;

		std::uint32_t matIndices[MaximumTerrainLayers] = { 0 };

		for (std::uint32_t i = 0; i < mInfo.NumLayers; ++i)
		{
			matIndices[i] = mLayerMaterials[i]->MatIndex;
		}

		drawCB.gTerrainLayerMaterialIndices[0] = DirectX::XMUINT4(matIndices[0], matIndices[1], matIndices[2], matIndices[3]);
		drawCB.gTerrainLayerMaterialIndices[1] = DirectX::XMUINT4(matIndices[4], matIndices[5], matIndices[6], matIndices[7]);


		drawCB.gTerrainWorldCellSpacing = DirectX::XMFLOAT2(mInfo.CellSpacing, mInfo.CellSpacing);
		drawCB.gTerrainWorldSize = DirectX::XMFLOAT2(GetWidth(), GetDepth());

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

		drawCB.gUseTerrainHeightMap = mUseTerrainHeightMap ? 1 : 0;
		drawCB.gUseMaterialHeightMaps = mUseMaterialHeightMaps ? 1 : 0;

		auto& linearAllocator = DirectX::GraphicsMemory::Get(md3dDevice);
		mDrawConstants = linearAllocator.AllocateConstant(drawCB);
		cmdList->SetGraphicsRootConstantBufferView(GFX_ROOT_ARG_OBJECT_CBV, mDrawConstants.GpuAddress());

		cmdList->DrawIndexedInstanced(mNumPatchQuadFaces * 4, 1, 0, 0, 0);
	}

private:
	void LoadHeightmapRaw16()
	{
		// A 16-bit height for each vertex
		auto in = std::vector<std::uint16_t>(mInfo.HeightmapWidth * mInfo.HeightmapHeight);

		// Open the file.
		auto absolutePath = std::filesystem::absolute(mInfo.HeightMapFilename);
		auto inFile = std::ifstream{};
		inFile.open(absolutePath, std::ios_base::binary);

		if (inFile)
		{
			// Read the RAW bytes.
			inFile.read((char*)&in[0], (std::streamsize)(in.size() * sizeof(std::uint16_t)));

			// Done with file.
			inFile.close();
		}

		constexpr float MaxUShort = static_cast<float>(std::numeric_limits<std::uint16_t>::max());

		// Copy the array data into a float array and scale it.
		mHeightmap.resize(in.size(), 0);
		for (auto i = 0u; i < in.size(); ++i)
		{
			auto heightUnorm = in[i] / MaxUShort;
			mHeightmap[i] = mInfo.HeightScale * heightUnorm + mInfo.HeightOffset;
		}
	}

	void CalcAllPatchBoundsY()
	{
		mPatchBoundsY.resize(mNumPatchQuadFaces);

		// For each patch
		for (auto i = 0u; i < mNumPatchVertRows - 1; ++i)
		{
			for (auto j = 0u; j < mNumPatchVertCols - 1; ++j)
			{
				CalcPatchBoundsY(i, j);
			}
		}
	}

	void CalcPatchBoundsY(std::uint32_t i, std::uint32_t j)
	{
		// Scan the heightmap values this patch covers and compute the min/max height.

		auto x0 = j * CellsPerPatch;
		auto x1 = (j + 1) * CellsPerPatch;

		auto y0 = i * CellsPerPatch;
		auto y1 = (i + 1) * CellsPerPatch;

		auto minY = +std::numeric_limits<float>::infinity();
		auto maxY = -std::numeric_limits<float>::infinity();
		for (auto y = y0; y <= y1; ++y)
		{
			for (auto x = x0; x <= x1; ++x)
			{
				auto k = y * mInfo.HeightmapWidth + x;
				minY = std::min(minY, mHeightmap[k]);
				maxY = std::max(maxY, mHeightmap[k]);
			}
		}

		auto patchID = i * (mNumPatchVertCols - 1) + j;
		mPatchBoundsY[patchID] = DirectX::XMFLOAT2(minY, maxY);
	}

	void BuildQuadPatchVB(DirectX::ResourceUploadBatch& uploadBatch)
	{
		auto patchVertices = std::vector<DirectX::XMFLOAT4>(mNumPatchVertRows * mNumPatchVertCols);

		auto halfWidth = 0.5f * GetWidth();
		auto halfDepth = 0.5f * GetDepth();

		auto patchWidth = GetWidth() / (mNumPatchVertCols - 1);
		auto patchDepth = GetDepth() / (mNumPatchVertRows - 1);

		for (auto i = 0u; i < mNumPatchVertRows; ++i)
		{
			auto z = halfDepth - i * patchDepth;
			for (auto j = 0u; j < mNumPatchVertCols; ++j)
			{
				auto x = -halfWidth + j * patchWidth;

				// xy: Patch 2d point position in xz-plane.
				patchVertices[i * mNumPatchVertCols + j] = DirectX::XMFLOAT4(x, z, 0.0f, 0.0f);
			}
		}

		// Store axis-aligned bounding box y-bounds in upper-left patch corner.
		for (auto i = 0u; i < mNumPatchVertRows - 1; ++i)
		{
			for (auto j = 0u; j < mNumPatchVertCols - 1; ++j)
			{
				auto patchID = i * (mNumPatchVertCols - 1) + j;

				// zw: Patch axis y-bounds.
				patchVertices[i * mNumPatchVertCols + j].z = mPatchBoundsY[patchID].x;
				patchVertices[i * mNumPatchVertCols + j].w = mPatchBoundsY[patchID].y;
			}
		}

		CreateStaticBuffer(md3dDevice, uploadBatch,
			patchVertices.data(), patchVertices.size(), sizeof(DirectX::XMFLOAT4),
			D3D12::D3D12_RESOURCE_STATES::D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER, mQuadPatchVB.GetAddressOf(),
			D3D12::D3D12_RESOURCE_FLAGS::D3D12_RESOURCE_FLAG_NONE);
	}

	void BuildQuadPatchIB(DirectX::ResourceUploadBatch& uploadBatch)
	{
		auto indices = std::vector<std::uint32_t>(mNumPatchQuadFaces * 4); // 4 indices per quad face

		// Iterate over each quad and compute indices.
		int k = 0;
		for (std::uint32_t i = 0; i < mNumPatchVertRows - 1; ++i)
		{
			for (std::uint32_t j = 0; j < mNumPatchVertCols - 1; ++j)
			{
				// Top row of 2x2 quad patch
				indices[k] = i * mNumPatchVertCols + j;
				indices[k + 1] = i * mNumPatchVertCols + j + 1;

				// Bottom row of 2x2 quad patch
				indices[k + 2] = (i + 1) * mNumPatchVertCols + j;
				indices[k + 3] = (i + 1) * mNumPatchVertCols + j + 1;

				k += 4; // next quad
			}
		}

		CreateStaticBuffer(md3dDevice, uploadBatch,
			indices.data(), indices.size(), sizeof(std::uint32_t),
			D3D12::D3D12_RESOURCE_STATES::D3D12_RESOURCE_STATE_INDEX_BUFFER, mQuadPatchIB.GetAddressOf(),
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
