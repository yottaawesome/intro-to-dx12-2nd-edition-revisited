export module shared:meshgen;
import std;
import :win32;
import :meshutil;

export
{
    struct MeshGenVertex
    {
        MeshGenVertex() :
            Position(0.0f, 0.0f, 0.0f),
            Normal(0.0f, 0.0f, 0.0f),
            TangentU(0.0f, 0.0f, 0.0f),
            TexC(0.0f, 0.0f) {}
        MeshGenVertex(
            const DirectX::XMFLOAT3& p,
            const DirectX::XMFLOAT3& n,
            const DirectX::XMFLOAT3& t,
            const DirectX::XMFLOAT2& uv) :
            Position(p),
            Normal(n),
            TangentU(t),
            TexC(uv) {}
        MeshGenVertex(
            float px, float py, float pz,
            float nx, float ny, float nz,
            float tx, float ty, float tz,
            float u, float v) :
            Position(px, py, pz),
            Normal(nx, ny, nz),
            TangentU(tx, ty, tz),
            TexC(u, v) {}

        DirectX::XMFLOAT3 Position;
        DirectX::XMFLOAT3 Normal;
        DirectX::XMFLOAT3 TangentU;
        DirectX::XMFLOAT2 TexC;
    };

    struct MeshGenData
    {
        std::vector<MeshGenVertex> Vertices;
        std::vector<std::uint32_t> Indices32;

        SubmeshGeometry AppendSubmesh(const MeshGenData& meshData);

        std::vector<std::uint16_t>& GetIndices16()
        {
            if (mIndices16.empty())
            {
                mIndices16.resize(Indices32.size());
                for (size_t i = 0; i < Indices32.size(); ++i)
                    mIndices16[i] = static_cast<std::uint16_t>(Indices32[i]);
            }

            return mIndices16;
        }

    private:
        std::vector<std::uint16_t> mIndices16;
    };

    class MeshGen
    {
    public:

        ///<summary>
        /// Creates a box centered at the origin with the given dimensions, where each
        /// face has m rows and n columns of vertices.
        ///</summary>
        MeshGenData CreateBox(float width, float height, float depth, uint32_t numSubdivisions)
        {
            MeshGenData meshData;

            //
            // Create the vertices.
            //

            MeshGenVertex v[24];

            float w2 = 0.5f * width;
            float h2 = 0.5f * height;
            float d2 = 0.5f * depth;

            // Fill in the front face vertex data.
            v[0] = MeshGenVertex(-w2, -h2, -d2, 0.0f, 0.0f, -1.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f);
            v[1] = MeshGenVertex(-w2, +h2, -d2, 0.0f, 0.0f, -1.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f);
            v[2] = MeshGenVertex(+w2, +h2, -d2, 0.0f, 0.0f, -1.0f, 1.0f, 0.0f, 0.0f, 1.0f, 0.0f);
            v[3] = MeshGenVertex(+w2, -h2, -d2, 0.0f, 0.0f, -1.0f, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f);

            // Fill in the back face vertex data.
            v[4] = MeshGenVertex(-w2, -h2, +d2, 0.0f, 0.0f, 1.0f, -1.0f, 0.0f, 0.0f, 1.0f, 1.0f);
            v[5] = MeshGenVertex(+w2, -h2, +d2, 0.0f, 0.0f, 1.0f, -1.0f, 0.0f, 0.0f, 0.0f, 1.0f);
            v[6] = MeshGenVertex(+w2, +h2, +d2, 0.0f, 0.0f, 1.0f, -1.0f, 0.0f, 0.0f, 0.0f, 0.0f);
            v[7] = MeshGenVertex(-w2, +h2, +d2, 0.0f, 0.0f, 1.0f, -1.0f, 0.0f, 0.0f, 1.0f, 0.0f);

            // Fill in the top face vertex data.
            v[8] = MeshGenVertex(-w2, +h2, -d2, 0.0f, 1.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f);
            v[9] = MeshGenVertex(-w2, +h2, +d2, 0.0f, 1.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f);
            v[10] = MeshGenVertex(+w2, +h2, +d2, 0.0f, 1.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f, 0.0f);
            v[11] = MeshGenVertex(+w2, +h2, -d2, 0.0f, 1.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f);

            // Fill in the bottom face vertex data.
            v[12] = MeshGenVertex(-w2, -h2, -d2, 0.0f, -1.0f, 0.0f, -1.0f, 0.0f, 0.0f, 1.0f, 1.0f);
            v[13] = MeshGenVertex(+w2, -h2, -d2, 0.0f, -1.0f, 0.0f, -1.0f, 0.0f, 0.0f, 0.0f, 1.0f);
            v[14] = MeshGenVertex(+w2, -h2, +d2, 0.0f, -1.0f, 0.0f, -1.0f, 0.0f, 0.0f, 0.0f, 0.0f);
            v[15] = MeshGenVertex(-w2, -h2, +d2, 0.0f, -1.0f, 0.0f, -1.0f, 0.0f, 0.0f, 1.0f, 0.0f);

            // Fill in the left face vertex data.
            v[16] = MeshGenVertex(-w2, -h2, +d2, -1.0f, 0.0f, 0.0f, 0.0f, 0.0f, -1.0f, 0.0f, 1.0f);
            v[17] = MeshGenVertex(-w2, +h2, +d2, -1.0f, 0.0f, 0.0f, 0.0f, 0.0f, -1.0f, 0.0f, 0.0f);
            v[18] = MeshGenVertex(-w2, +h2, -d2, -1.0f, 0.0f, 0.0f, 0.0f, 0.0f, -1.0f, 1.0f, 0.0f);
            v[19] = MeshGenVertex(-w2, -h2, -d2, -1.0f, 0.0f, 0.0f, 0.0f, 0.0f, -1.0f, 1.0f, 1.0f);

            // Fill in the right face vertex data.
            v[20] = MeshGenVertex(+w2, -h2, -d2, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 1.0f);
            v[21] = MeshGenVertex(+w2, +h2, -d2, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f);
            v[22] = MeshGenVertex(+w2, +h2, +d2, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f);
            v[23] = MeshGenVertex(+w2, -h2, +d2, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f);

            meshData.Vertices.assign(&v[0], &v[24]);

            //
            // Create the indices.
            //

            uint32_t i[36];

            // Fill in the front face index data
            i[0] = 0; i[1] = 1; i[2] = 2;
            i[3] = 0; i[4] = 2; i[5] = 3;

            // Fill in the back face index data
            i[6] = 4; i[7] = 5; i[8] = 6;
            i[9] = 4; i[10] = 6; i[11] = 7;

            // Fill in the top face index data
            i[12] = 8; i[13] = 9; i[14] = 10;
            i[15] = 8; i[16] = 10; i[17] = 11;

            // Fill in the bottom face index data
            i[18] = 12; i[19] = 13; i[20] = 14;
            i[21] = 12; i[22] = 14; i[23] = 15;

            // Fill in the left face index data
            i[24] = 16; i[25] = 17; i[26] = 18;
            i[27] = 16; i[28] = 18; i[29] = 19;

            // Fill in the right face index data
            i[30] = 20; i[31] = 21; i[32] = 22;
            i[33] = 20; i[34] = 22; i[35] = 23;

            meshData.Indices32.assign(&i[0], &i[36]);

            // Put a cap on the number of subdivisions.
            numSubdivisions = std::min<uint32_t>(numSubdivisions, 6u);

            for (uint32_t i = 0; i < numSubdivisions; ++i)
                Subdivide(meshData);

            return meshData;
        }

        ///<summary>
        /// Creates a sphere centered at the origin with the given radius.  The
        /// slices and stacks parameters control the degree of tessellation.
        ///</summary>
        MeshGenData CreateSphere(float radius, uint32_t sliceCount, uint32_t stackCount)
        {
            MeshGenData meshData;

            //
            // Compute the vertices stating at the top pole and moving down the stacks.
            //

            // Poles: note that there will be texture coordinate distortion as there is
            // not a unique point on the texture map to assign to the pole when mapping
            // a rectangular texture onto a sphere.
            MeshGenVertex topVertex(0.0f, +radius, 0.0f, 0.0f, +1.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f);
            MeshGenVertex bottomVertex(0.0f, -radius, 0.0f, 0.0f, -1.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f);

            meshData.Vertices.push_back(topVertex);

            float phiStep = DirectX::XM_Pi / stackCount;
            float thetaStep = 2.0f * DirectX::XM_2Pi / sliceCount;

            // Compute vertices for each stack ring (do not count the poles as rings).
            for (uint32_t i = 1; i <= stackCount - 1; ++i)
            {
                float phi = i * phiStep;

                // Vertices of ring.
                for (uint32_t j = 0; j <= sliceCount; ++j)
                {
                    float theta = j * thetaStep;

                    MeshGenVertex v;

                    // spherical to cartesian
                    v.Position.x = radius * std::sinf(phi) * std::cosf(theta);
                    v.Position.y = radius * std::cosf(phi);
                    v.Position.z = radius * std::sinf(phi) * std::sinf(theta);

                    // Partial derivative of P with respect to theta
                    v.TangentU.x = -radius * std::sinf(phi) * std::sinf(theta);
                    v.TangentU.y = 0.0f;
                    v.TangentU.z = +radius * std::sinf(phi) * std::cosf(theta);

                    DirectX::XMVECTOR T = DirectX::XMLoadFloat3(&v.TangentU);
                    DirectX::XMStoreFloat3(&v.TangentU, DirectX::XMVector3Normalize(T));

                    DirectX::XMVECTOR p = DirectX::XMLoadFloat3(&v.Position);
                    DirectX::XMStoreFloat3(&v.Normal, DirectX::XMVector3Normalize(p));

                    v.TexC.x = theta / DirectX::XM_2Pi;
                    v.TexC.y = phi / DirectX::XM_Pi;

                    meshData.Vertices.push_back(v);
                }
            }

            meshData.Vertices.push_back(bottomVertex);

            //
            // Compute indices for top stack.  The top stack was written first to the vertex buffer
            // and connects the top pole to the first ring.
            //

            for (uint32_t i = 1; i <= sliceCount; ++i)
            {
                meshData.Indices32.push_back(0);
                meshData.Indices32.push_back(i + 1);
                meshData.Indices32.push_back(i);
            }

            //
            // Compute indices for inner stacks (not connected to poles).
            //

            // Offset the indices to the index of the first vertex in the first ring.
            // This is just skipping the top pole vertex.
            uint32_t baseIndex = 1;
            uint32_t ringVertexCount = sliceCount + 1;
            for (uint32_t i = 0; i < stackCount - 2; ++i)
            {
                for (uint32_t j = 0; j < sliceCount; ++j)
                {
                    meshData.Indices32.push_back(baseIndex + i * ringVertexCount + j);
                    meshData.Indices32.push_back(baseIndex + i * ringVertexCount + j + 1);
                    meshData.Indices32.push_back(baseIndex + (i + 1) * ringVertexCount + j);

                    meshData.Indices32.push_back(baseIndex + (i + 1) * ringVertexCount + j);
                    meshData.Indices32.push_back(baseIndex + i * ringVertexCount + j + 1);
                    meshData.Indices32.push_back(baseIndex + (i + 1) * ringVertexCount + j + 1);
                }
            }

            //
            // Compute indices for bottom stack.  The bottom stack was written last to the vertex buffer
            // and connects the bottom pole to the bottom ring.
            //

            // South pole vertex was added last.
            uint32_t southPoleIndex = (uint32_t)meshData.Vertices.size() - 1;

            // Offset the indices to the index of the first vertex in the last ring.
            baseIndex = southPoleIndex - ringVertexCount;

            for (uint32_t i = 0; i < sliceCount; ++i)
            {
                meshData.Indices32.push_back(southPoleIndex);
                meshData.Indices32.push_back(baseIndex + i);
                meshData.Indices32.push_back(baseIndex + i + 1);
            }

            return meshData;
        }

        ///<summary>
        /// Creates a geosphere centered at the origin with the given radius.  The
        /// depth controls the level of tessellation.
        ///</summary>
        MeshGenData CreateGeosphere(float radius, uint32_t numSubdivisions);

        ///<summary>
        /// Creates a cylinder parallel to the y-axis, and centered about the origin.  
        /// The bottom and top radius can vary to form various cone shapes rather than true
        // cylinders.  The slices and stacks parameters control the degree of tessellation.
        ///</summary>
        MeshGenData CreateCylinder(float bottomRadius, float topRadius, float height, uint32_t sliceCount, uint32_t stackCount);

        ///<summary>
        /// Creates an mxn grid in the xz-plane with m rows and n columns, centered
        /// at the origin with the specified width and depth.
        ///</summary>
        MeshGenData CreateGrid(float width, float depth, uint32_t m, uint32_t n);

        ///<summary>
        /// Creates a quad aligned with the screen.  This is useful for postprocessing and screen effects.
        ///</summary>
        MeshGenData CreateQuad(float x, float y, float w, float h, float depth);

    private:

        void Subdivide(MeshGenData& meshData);
        MeshGenVertex MidPoint(const MeshGenVertex& v0, const MeshGenVertex& v1);
        void BuildCylinderTopCap(float bottomRadius, float topRadius, float height, uint32_t sliceCount, uint32_t stackCount, MeshGenData& meshData);
        void BuildCylinderBottomCap(float bottomRadius, float topRadius, float height, uint32_t sliceCount, uint32_t stackCount, MeshGenData& meshData);
    };
}
