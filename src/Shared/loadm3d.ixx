//***************************************************************************************
// d3dApp.cpp by Frank Luna (C) 2015 All Rights Reserved.
//***************************************************************************************

export module shared:loadm3d;
import std;
import :win32;
import :skinneddata;

export class M3DLoader
{
public:
    struct Vertex
    {
        DirectX::XMFLOAT3 Pos;
        DirectX::XMFLOAT3 Normal;
        DirectX::XMFLOAT2 TexC;
        DirectX::XMFLOAT4 TangentU;
    };

    struct SkinnedVertex
    {
        DirectX::XMFLOAT3 Pos;
        DirectX::XMFLOAT3 Normal;
        DirectX::XMFLOAT2 TexC;
        DirectX::XMFLOAT3 TangentU;
        DirectX::XMFLOAT3 BoneWeights;
        Win32::BYTE BoneIndices[4];
    };

    struct Subset
    {
        Win32::UINT Id = -1;
        Win32::UINT VertexStart = 0;
        Win32::UINT VertexCount = 0;
        Win32::UINT FaceStart = 0;
        Win32::UINT FaceCount = 0;
    };

    struct M3dMaterial
    {
        std::string Name;

        DirectX::XMFLOAT4 DiffuseAlbedo = { 1.0f, 1.0f, 1.0f, 1.0f };
        DirectX::XMFLOAT3 FresnelR0 = { 0.01f, 0.01f, 0.01f };
        float Roughness = 0.8f;
        bool AlphaClip = false;

        std::string MaterialTypeName;
        std::string DiffuseMapName;
        std::string NormalMapName;
    };

    auto LoadM3d(
        const std::string& filename,
        std::vector<Vertex>& vertices,
        std::vector<Win32::UINT>& indices,
        std::vector<Subset>& subsets,
        std::vector<M3dMaterial>& mats
	) -> bool
    {
        auto fin = std::ifstream{filename};
        if (not fin)
            return false;

        auto numMaterials = 0u;
        auto numVertices = 0u;
        auto numTriangles = 0u;
        auto numBones = 0u;
        auto numAnimationClips = 0u;

        auto ignore = std::string{};

        fin >> ignore; // file header text
        fin >> ignore >> numMaterials;
        fin >> ignore >> numVertices;
        fin >> ignore >> numTriangles;
        fin >> ignore >> numBones;
        fin >> ignore >> numAnimationClips;

        ReadMaterials(fin, numMaterials, mats);
        ReadSubsetTable(fin, numMaterials, subsets);
        ReadVertices(fin, numVertices, vertices);
        ReadTriangles(fin, numTriangles, indices);

        return true;
    }
    auto LoadM3d(
        const std::string& filename,
        std::vector<SkinnedVertex>& vertices,
        std::vector<Win32::UINT>& indices,
        std::vector<Subset>& subsets,
        std::vector<M3dMaterial>& mats,
        SkinnedData& skinInfo
    ) -> bool
    {
        auto fin = std::ifstream{filename};
        if (not fin)
        {
            Win32::MessageBoxW(0, L"LoadM3d failed--file not found.", 0, 0);
            return false;
        }

        auto numMaterials = 0u;
        auto numVertices = 0u;
        auto numTriangles = 0u;
        auto numBones = 0u;
        auto numAnimationClips = 0u;

        auto ignore = std::string{};

        fin >> ignore; // file header text
        fin >> ignore >> numMaterials;
        fin >> ignore >> numVertices;
        fin >> ignore >> numTriangles;
        fin >> ignore >> numBones;
        fin >> ignore >> numAnimationClips;

        auto boneOffsets = std::vector<DirectX::XMFLOAT4X4>{};
        auto boneIndexToParentIndex = std::vector<int>{};
        auto animations = std::unordered_map<std::string, AnimationClip>{};

        ReadMaterials(fin, numMaterials, mats);
        ReadSubsetTable(fin, numMaterials, subsets);
        ReadSkinnedVertices(fin, numVertices, vertices);
        ReadTriangles(fin, numTriangles, indices);
        ReadBoneOffsets(fin, numBones, boneOffsets);
        ReadBoneHierarchy(fin, numBones, boneIndexToParentIndex);
        ReadAnimationClips(fin, numBones, numAnimationClips, animations);

        skinInfo.Set(boneIndexToParentIndex, boneOffsets, animations);

        return true;
    }

private:
    void ReadMaterials(std::ifstream& fin, Win32::UINT numMaterials, std::vector<M3dMaterial>& mats)
    {
        auto ignore = std::string{};
        mats.resize(numMaterials);

        auto diffuseMapName = std::string{};
        auto normalMapName = std::string{};

        fin >> ignore; // materials header text
        for (auto i = 0u; i < numMaterials; ++i)
        {
            fin >> ignore >> mats[i].Name;
            fin >> ignore >> mats[i].DiffuseAlbedo.x >> mats[i].DiffuseAlbedo.y >> mats[i].DiffuseAlbedo.z;
            fin >> ignore >> mats[i].FresnelR0.x >> mats[i].FresnelR0.y >> mats[i].FresnelR0.z;
            fin >> ignore >> mats[i].Roughness;
            fin >> ignore >> mats[i].AlphaClip;
            fin >> ignore >> mats[i].MaterialTypeName;
            fin >> ignore >> mats[i].DiffuseMapName;
            fin >> ignore >> mats[i].NormalMapName;
        }
    }
    void ReadSubsetTable(std::ifstream& fin, Win32::UINT numSubsets, std::vector<Subset>& subsets)
    {
        auto ignore = std::string{};
        subsets.resize(numSubsets);

        fin >> ignore; // subset header text
        for (auto i = 0u; i < numSubsets; ++i)
        {
            fin >> ignore >> subsets[i].Id;
            fin >> ignore >> subsets[i].VertexStart;
            fin >> ignore >> subsets[i].VertexCount;
            fin >> ignore >> subsets[i].FaceStart;
            fin >> ignore >> subsets[i].FaceCount;
        }
    }
    void ReadVertices(std::ifstream& fin, Win32::UINT numVertices, std::vector<Vertex>& vertices)
    {
        auto ignore = std::string{};
        vertices.resize(numVertices);

        fin >> ignore; // vertices header text
        for (auto i = 0u; i < numVertices; ++i)
        {
            fin >> ignore >> vertices[i].Pos.x >> vertices[i].Pos.y >> vertices[i].Pos.z;
            fin >> ignore >> vertices[i].TangentU.x >> vertices[i].TangentU.y >> vertices[i].TangentU.z >> vertices[i].TangentU.w;
            fin >> ignore >> vertices[i].Normal.x >> vertices[i].Normal.y >> vertices[i].Normal.z;
            fin >> ignore >> vertices[i].TexC.x >> vertices[i].TexC.y;
        }
    }
    void ReadSkinnedVertices(std::ifstream& fin, Win32::UINT numVertices, std::vector<SkinnedVertex>& vertices)
    {
        auto ignore = std::string{};
        vertices.resize(numVertices);

        fin >> ignore; // vertices header text
        auto boneIndices = std::array<int, 4>{};
        auto weights = std::array<float, 4>{};
        for (auto i = 0u; i < numVertices; ++i)
        {
            auto blah = float{};
            fin >> ignore >> vertices[i].Pos.x >> vertices[i].Pos.y >> vertices[i].Pos.z;
            fin >> ignore >> vertices[i].TangentU.x >> vertices[i].TangentU.y >> vertices[i].TangentU.z >> blah /*vertices[i].TangentU.w*/;
            fin >> ignore >> vertices[i].Normal.x >> vertices[i].Normal.y >> vertices[i].Normal.z;
            fin >> ignore >> vertices[i].TexC.x >> vertices[i].TexC.y;
            fin >> ignore >> weights[0] >> weights[1] >> weights[2] >> weights[3];
            fin >> ignore >> boneIndices[0] >> boneIndices[1] >> boneIndices[2] >> boneIndices[3];

            vertices[i].BoneWeights.x = weights[0];
            vertices[i].BoneWeights.y = weights[1];
            vertices[i].BoneWeights.z = weights[2];

            vertices[i].BoneIndices[0] = (BYTE)boneIndices[0];
            vertices[i].BoneIndices[1] = (BYTE)boneIndices[1];
            vertices[i].BoneIndices[2] = (BYTE)boneIndices[2];
            vertices[i].BoneIndices[3] = (BYTE)boneIndices[3];
        }
    }
    void ReadTriangles(std::ifstream& fin, Win32::UINT numTriangles, std::vector<Win32::UINT>& indices)
    {
        auto ignore = std::string{};
        indices.resize(numTriangles * 3);

        fin >> ignore; // triangles header text
        for (auto i = 0u; i < numTriangles; ++i)
        {
            fin >> indices[i * 3 + 0] >> indices[i * 3 + 1] >> indices[i * 3 + 2];
        }
    }
    void ReadBoneOffsets(std::ifstream& fin, Win32::UINT numBones, std::vector<DirectX::XMFLOAT4X4>& boneOffsets)
    {
        auto ignore = std::string{};
        boneOffsets.resize(numBones);

        fin >> ignore; // BoneOffsets header text
        for (auto i = 0u; i < numBones; ++i)
        {
            fin >> ignore >>
                boneOffsets[i](0, 0) >> boneOffsets[i](0, 1) >> boneOffsets[i](0, 2) >> boneOffsets[i](0, 3) >>
                boneOffsets[i](1, 0) >> boneOffsets[i](1, 1) >> boneOffsets[i](1, 2) >> boneOffsets[i](1, 3) >>
                boneOffsets[i](2, 0) >> boneOffsets[i](2, 1) >> boneOffsets[i](2, 2) >> boneOffsets[i](2, 3) >>
                boneOffsets[i](3, 0) >> boneOffsets[i](3, 1) >> boneOffsets[i](3, 2) >> boneOffsets[i](3, 3);
        }
    }
    void ReadBoneHierarchy(std::ifstream& fin, Win32::UINT numBones, std::vector<int>& boneIndexToParentIndex)
    {
        auto ignore = std::string{};
        boneIndexToParentIndex.resize(numBones);

        fin >> ignore; // BoneHierarchy header text
        for (auto i = 0u; i < numBones; ++i)
        {
            fin >> ignore >> boneIndexToParentIndex[i];
        }
    }
    void ReadAnimationClips(std::ifstream& fin, Win32::UINT numBones, Win32::UINT numAnimationClips, std::unordered_map<std::string, AnimationClip>& animations)
    {
        auto ignore = std::string{};
        fin >> ignore; // AnimationClips header text
        for (auto clipIndex = 0u; clipIndex < numAnimationClips; ++clipIndex)
        {
            auto clipName = std::string{};
            fin >> ignore >> clipName;
            fin >> ignore; // {

            auto clip = AnimationClip{};
            clip.BoneAnimations.resize(numBones);

            for (auto boneIndex = 0u; boneIndex < numBones; ++boneIndex)
            {
                ReadBoneKeyframes(fin, numBones, clip.BoneAnimations[boneIndex]);
            }
            fin >> ignore; // }

            animations[clipName] = clip;
        }
    }
    void ReadBoneKeyframes(std::ifstream& fin, Win32::UINT numBones, BoneAnimation& boneAnimation)
    {
        auto ignore = std::string{};
        auto numKeyframes = 0u;
        fin >> ignore >> ignore >> numKeyframes;
        fin >> ignore; // {

        boneAnimation.Keyframes.resize(numKeyframes);
        for (auto i = 0u; i < numKeyframes; ++i)
        {
            auto t = 0.0f;
            auto p = DirectX::XMFLOAT3(0.0f, 0.0f, 0.0f);
            auto s = DirectX::XMFLOAT3(1.0f, 1.0f, 1.0f);
            auto q = DirectX::XMFLOAT4(0.0f, 0.0f, 0.0f, 1.0f);
            fin >> ignore >> t;
            fin >> ignore >> p.x >> p.y >> p.z;
            fin >> ignore >> s.x >> s.y >> s.z;
            fin >> ignore >> q.x >> q.y >> q.z >> q.w;

            boneAnimation.Keyframes[i].TimePos = t;
            boneAnimation.Keyframes[i].Translation = p;
            boneAnimation.Keyframes[i].Scale = s;
            boneAnimation.Keyframes[i].RotationQuat = q;
        }

        fin >> ignore; // }
    }
};
