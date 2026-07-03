//***************************************************************************************
// d3dApp.cpp by Frank Luna (C) 2015 All Rights Reserved.
//***************************************************************************************

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
            const DirectX::XMFLOAT2& uv
        ) : Position(p), 
            Normal(n), 
            TangentU(t), 
            TexC(uv) {}
        MeshGenVertex(
            float px, float py, float pz,
            float nx, float ny, float nz,
            float tx, float ty, float tz,
            float u, float v
        ) : Position(px, py, pz),
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

        auto AppendSubmesh(const MeshGenData& meshData) -> SubmeshGeometry
        {
            auto vertexOffset = static_cast<UINT>(Vertices.size());
            auto indexOffset = static_cast<UINT>(Indices32.size());

			constexpr auto fltMax = std::numeric_limits<float>::max();
            constexpr auto vMinf3 = DirectX::XMFLOAT3(+fltMax, +fltMax, +fltMax);
            constexpr auto vMaxf3 = DirectX::XMFLOAT3(-fltMax, -fltMax, -fltMax);

            auto vMin = DirectX::XMVECTOR{DirectX::XMLoadFloat3(&vMinf3)};
            auto vMax = DirectX::XMVECTOR{DirectX::XMLoadFloat3(&vMaxf3)};

            for (auto i = 0u; i < meshData.Vertices.size(); ++i)
            {
                auto P = DirectX::XMVECTOR{DirectX::XMLoadFloat3(&meshData.Vertices[i].Position)};

                vMin = DirectX::XMVectorMin(vMin, P);
                vMax = DirectX::XMVectorMax(vMax, P);
            }

            auto bounds = DirectX::BoundingBox{};
            DirectX::XMStoreFloat3(&bounds.Center, 0.5f * (vMin + vMax));
            DirectX::XMStoreFloat3(&bounds.Extents, 0.5f * (vMax - vMin));

            auto submesh = SubmeshGeometry{
				.IndexCount = static_cast<UINT>(meshData.Indices32.size()),
                .StartIndexLocation = indexOffset,
                .BaseVertexLocation = static_cast<int>(vertexOffset),
                .VertexCount = static_cast<UINT>(meshData.Vertices.size()),
                .Bounds = bounds,
            };

            Vertices.insert(std::end(Vertices), std::begin(meshData.Vertices), std::end(meshData.Vertices));
            Indices32.insert(std::end(Indices32), std::begin(meshData.Indices32), std::end(meshData.Indices32));

            return submesh;
        }

        auto GetIndices16() -> std::vector<std::uint16_t>&
        {
            if (mIndices16.empty())
            {
                mIndices16.resize(Indices32.size());
                for (auto i = size_t{}; i < Indices32.size(); ++i)
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
        auto CreateBox(float width, float height, float depth, uint32_t numSubdivisions) -> MeshGenData
        {
            auto meshData = MeshGenData{};

            //
            // Create the vertices.
            //
			auto v = std::array<MeshGenVertex, 24>{};

            auto w2 = 0.5f * width;
            auto h2 = 0.5f * height;
            auto d2 = 0.5f * depth;

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
            auto i = std::array<uint32_t, 36>{};

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

            for (auto i = 0u; i < numSubdivisions; ++i)
                Subdivide(meshData);

            return meshData;
        }

        ///<summary>
        /// Creates a sphere centered at the origin with the given radius.  The
        /// slices and stacks parameters control the degree of tessellation.
        ///</summary>
        auto CreateSphere(float radius, uint32_t sliceCount, uint32_t stackCount) -> MeshGenData
        {
            auto meshData = MeshGenData{};

            //
            // Compute the vertices stating at the top pole and moving down the stacks.
            //

            // Poles: note that there will be texture coordinate distortion as there is
            // not a unique point on the texture map to assign to the pole when mapping
            // a rectangular texture onto a sphere.
            auto topVertex = MeshGenVertex{0.0f, +radius, 0.0f, 0.0f, +1.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f};
            auto bottomVertex = MeshGenVertex{0.0f, -radius, 0.0f, 0.0f, -1.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f};

            meshData.Vertices.push_back(topVertex);

            auto phiStep = DirectX::XM_Pi / stackCount;
            auto thetaStep = 2.0f * DirectX::XM_2Pi / sliceCount;

            // Compute vertices for each stack ring (do not count the poles as rings).
            for (auto i = 1u; i <= stackCount - 1; ++i)
            {
                auto phi = i * phiStep;

                // Vertices of ring.
                for (auto j = 0u; j <= sliceCount; ++j)
                {
                    auto theta = j * thetaStep;

                    auto v = MeshGenVertex{};

                    // spherical to cartesian
                    v.Position.x = radius * std::sinf(phi) * std::cosf(theta);
                    v.Position.y = radius * std::cosf(phi);
                    v.Position.z = radius * std::sinf(phi) * std::sinf(theta);

                    // Partial derivative of P with respect to theta
                    v.TangentU.x = -radius * std::sinf(phi) * std::sinf(theta);
                    v.TangentU.y = 0.0f;
                    v.TangentU.z = +radius * std::sinf(phi) * std::cosf(theta);

                    auto T = DirectX::XMVECTOR{DirectX::XMLoadFloat3(&v.TangentU)};
                    DirectX::XMStoreFloat3(&v.TangentU, DirectX::XMVector3Normalize(T));

                    auto p = DirectX::XMVECTOR{DirectX::XMLoadFloat3(&v.Position)};
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

            for (auto i = 1u; i <= sliceCount; ++i)
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
            auto baseIndex = 1u;
            auto ringVertexCount = sliceCount + 1;
            for (auto i = 0u; i < stackCount - 2; ++i)
            {
                for (auto j = 0u; j < sliceCount; ++j)
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
            auto southPoleIndex = static_cast<uint32_t>(meshData.Vertices.size() - 1);

            // Offset the indices to the index of the first vertex in the last ring.
            baseIndex = southPoleIndex - ringVertexCount;

            for (auto i = 0u; i < sliceCount; ++i)
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
        auto CreateGeosphere(float radius, uint32_t numSubdivisions) -> MeshGenData
        {
            auto meshData = MeshGenData{};

            // Put a cap on the number of subdivisions.
            numSubdivisions = std::min<uint32_t>(numSubdivisions, 6u);

            // Approximate a sphere by tessellating an icosahedron.

            constexpr auto X = 0.525731f;
            constexpr auto Z = 0.850651f;

            auto pos = std::array<DirectX::XMFLOAT3, 12>
            {
                DirectX::XMFLOAT3{-X, 0.0f, Z},  DirectX::XMFLOAT3{X, 0.0f, Z},
                DirectX::XMFLOAT3{-X, 0.0f, -Z}, DirectX::XMFLOAT3{X, 0.0f, -Z},
                DirectX::XMFLOAT3{0.0f, Z, X},   DirectX::XMFLOAT3{0.0f, Z, -X},
                DirectX::XMFLOAT3{0.0f, -Z, X},  DirectX::XMFLOAT3{0.0f, -Z, -X},
                DirectX::XMFLOAT3{Z, X, 0.0f},   DirectX::XMFLOAT3{-Z, X, 0.0f},
                DirectX::XMFLOAT3{Z, -X, 0.0f},  DirectX::XMFLOAT3{-Z, -X, 0.0f}
            };

            auto k = std::array<uint32_t, 60>
            {
                1,4,0,  4,9,0,  4,5,9,  8,5,4,  1,8,4,
                1,10,8, 10,3,8, 8,3,5,  3,2,5,  3,7,2,
                3,10,7, 10,6,7, 6,11,7, 6,0,11, 6,1,0,
                10,1,6, 11,0,9, 2,11,9, 5,2,9,  11,2,7
            };

            meshData.Vertices.resize(12);
            meshData.Indices32.assign(&k[0], &k[60]);

            for (auto i = 0u; i < 12; ++i)
                meshData.Vertices[i].Position = pos[i];

            for (auto i = 0u; i < numSubdivisions; ++i)
                Subdivide(meshData);

            // Project vertices onto sphere and scale.
            for (auto i = 0u; i < meshData.Vertices.size(); ++i)
            {
                // Project onto unit sphere.
                auto n = DirectX::XMVECTOR{DirectX::XMVector3Normalize(DirectX::XMLoadFloat3(&meshData.Vertices[i].Position))};

                // Project onto sphere.
                auto p = DirectX::XMVECTOR{radius * n};

                DirectX::XMStoreFloat3(&meshData.Vertices[i].Position, p);
                DirectX::XMStoreFloat3(&meshData.Vertices[i].Normal, n);

                // Derive texture coordinates from spherical coordinates.
                auto theta = std::atan2f(meshData.Vertices[i].Position.z, meshData.Vertices[i].Position.x);

                // Put in [0, 2pi].
                if (theta < 0.0f)
                    theta += DirectX::XM_2Pi;

                auto phi = std::acosf(meshData.Vertices[i].Position.y / radius);

                meshData.Vertices[i].TexC.x = theta / DirectX::XM_2Pi;
                meshData.Vertices[i].TexC.y = phi / DirectX::XM_Pi;

                // Partial derivative of P with respect to theta
                meshData.Vertices[i].TangentU.x = -radius * sinf(phi) * sinf(theta);
                meshData.Vertices[i].TangentU.y = 0.0f;
                meshData.Vertices[i].TangentU.z = +radius * sinf(phi) * cosf(theta);

                auto T = DirectX::XMVECTOR{DirectX::XMLoadFloat3(&meshData.Vertices[i].TangentU)};
                DirectX::XMStoreFloat3(&meshData.Vertices[i].TangentU, DirectX::XMVector3Normalize(T));
            }

            return meshData;
        }

        ///<summary>
        /// Creates a cylinder parallel to the y-axis, and centered about the origin.  
        /// The bottom and top radius can vary to form various cone shapes rather than true
        // cylinders.  The slices and stacks parameters control the degree of tessellation.
        ///</summary>
        auto CreateCylinder(float bottomRadius, float topRadius, float height, uint32_t sliceCount, uint32_t stackCount) -> MeshGenData
        {
            auto meshData = MeshGenData{};

            //
            // Build Stacks.
            // 
            auto stackHeight = height / stackCount;

            // Amount to increment radius as we move up each stack level from bottom to top.
            auto radiusStep = (topRadius - bottomRadius) / stackCount;

            auto ringCount = uint32_t{ stackCount + 1 };

            // Compute vertices for each stack ring starting at the bottom and moving up.
            for (auto i = 0u; i < ringCount; ++i)
            {
                auto y = -0.5f * height + i * stackHeight;
                auto r = bottomRadius + i * radiusStep;

                // vertices of ring
                auto dTheta = 2.0f * DirectX::XM_Pi / sliceCount;
                for (auto j = 0u; j <= sliceCount; ++j)
                {
                    auto vertex = MeshGenVertex{};

                    auto c = cosf(j * dTheta);
                    auto s = sinf(j * dTheta);

                    vertex.Position = DirectX::XMFLOAT3{r * c, y, r * s};

                    vertex.TexC.x = static_cast<float>(j) / sliceCount;
                    vertex.TexC.y = 1.0f - static_cast<float>(i) / stackCount;

                    // Cylinder can be parameterized as follows, where we introduce v
                    // parameter that goes in the same direction as the v tex-coord
                    // so that the bitangent goes in the same direction as the v tex-coord.
                    //   Let r0 be the bottom radius and let r1 be the top radius.
                    //   y(v) = h - hv for v in [0,1].
                    //   r(v) = r1 + (r0-r1)v
                    //
                    //   x(t, v) = r(v)*cos(t)
                    //   y(t, v) = h - hv
                    //   z(t, v) = r(v)*sin(t)
                    // 
                    //  dx/dt = -r(v)*sin(t)
                    //  dy/dt = 0
                    //  dz/dt = +r(v)*cos(t)
                    //
                    //  dx/dv = (r0-r1)*cos(t)
                    //  dy/dv = -h
                    //  dz/dv = (r0-r1)*sin(t)

                    // This is unit length.
                    vertex.TangentU = DirectX::XMFLOAT3{ -s, 0.0f, c };

                    auto dr = bottomRadius - topRadius;
                    auto bitangent = DirectX::XMFLOAT3{ dr * c, -height, dr * s };

                    auto T = DirectX::XMVECTOR{ DirectX::XMLoadFloat3(&vertex.TangentU) };
                    auto B = DirectX::XMVECTOR{ DirectX::XMLoadFloat3(&bitangent) };
                    auto N = DirectX::XMVECTOR{ DirectX::XMVector3Normalize(DirectX::XMVector3Cross(T, B)) };
                    DirectX::XMStoreFloat3(&vertex.Normal, N);

                    meshData.Vertices.push_back(vertex);
                }
            }
            // Add one because we duplicate the first and last vertex per ring
            // since the texture coordinates are different.
            auto ringVertexCount = uint32_t{ sliceCount + 1 };

            // Compute indices for each stack.
            for (auto i = 0u; i < stackCount; ++i)
            {
                for (auto j = 0u; j < sliceCount; ++j)
                {
                    meshData.Indices32.push_back(i * ringVertexCount + j);
                    meshData.Indices32.push_back((i + 1) * ringVertexCount + j);
                    meshData.Indices32.push_back((i + 1) * ringVertexCount + j + 1);

                    meshData.Indices32.push_back(i * ringVertexCount + j);
                    meshData.Indices32.push_back((i + 1) * ringVertexCount + j + 1);
                    meshData.Indices32.push_back(i * ringVertexCount + j + 1);
                }
            }

            BuildCylinderTopCap(bottomRadius, topRadius, height, sliceCount, stackCount, meshData);
            BuildCylinderBottomCap(bottomRadius, topRadius, height, sliceCount, stackCount, meshData);

            return meshData;
        }

        ///<summary>
        /// Creates an mxn grid in the xz-plane with m rows and n columns, centered
        /// at the origin with the specified width and depth.
        ///</summary>
        auto CreateGrid(float width, float depth, uint32_t m, uint32_t n) -> MeshGenData
        {
            auto meshData = MeshGenData{};

            auto vertexCount = uint32_t{ m * n };
            auto faceCount = uint32_t{ (m - 1) * (n - 1) * 2 };

            //
            // Create the vertices.
            //

            auto halfWidth = 0.5f * width;
            auto halfDepth = 0.5f * depth;

            auto dx = width / (n - 1);
            auto dz = depth / (m - 1);

            auto du = 1.0f / (n - 1);
            auto dv = 1.0f / (m - 1);

            meshData.Vertices.resize(vertexCount);
            for (auto i = 0u; i < m; ++i)
            {
                auto z = halfDepth - i * dz;
                for (auto j = 0u; j < n; ++j)
                {
                    auto x = -halfWidth + j * dx;

                    meshData.Vertices[i * n + j].Position = DirectX::XMFLOAT3{x, 0.0f, z};
                    meshData.Vertices[i * n + j].Normal = DirectX::XMFLOAT3{0.0f, 1.0f, 0.0f};
                    meshData.Vertices[i * n + j].TangentU = DirectX::XMFLOAT3{1.0f, 0.0f, 0.0f};

                    // Stretch texture over grid.
                    meshData.Vertices[i * n + j].TexC.x = j * du;
                    meshData.Vertices[i * n + j].TexC.y = i * dv;
                }
            }

            //
            // Create the indices.
            //

            meshData.Indices32.resize(faceCount * 3); // 3 indices per face

            // Iterate over each quad and compute indices.
            auto k = 0u;
            for (auto i = 0u; i < m - 1; ++i)
            {
                for (auto j = 0u; j < n - 1; ++j)
                {
                    meshData.Indices32[k] = i * n + j;
                    meshData.Indices32[k + 1] = i * n + j + 1;
                    meshData.Indices32[k + 2] = (i + 1) * n + j;

                    meshData.Indices32[k + 3] = (i + 1) * n + j;
                    meshData.Indices32[k + 4] = i * n + j + 1;
                    meshData.Indices32[k + 5] = (i + 1) * n + j + 1;

                    k += 6; // next quad
                }
            }

            return meshData;
        }

        ///<summary>
        /// Creates a quad aligned with the screen.  This is useful for postprocessing and screen effects.
        ///</summary>
        auto CreateQuad(float x, float y, float w, float h, float depth) -> MeshGenData
        {
            auto meshData = MeshGenData{};
            meshData.Vertices.resize(4);
            meshData.Indices32.resize(6);

            // Position coordinates specified in NDC space.
            meshData.Vertices[0] = MeshGenVertex{
                x, y - h, depth,
                0.0f, 0.0f, -1.0f,
                1.0f, 0.0f, 0.0f,
                0.0f, 1.0f };

            meshData.Vertices[1] = MeshGenVertex{
                x, y, depth,
                0.0f, 0.0f, -1.0f,
                1.0f, 0.0f, 0.0f,
                0.0f, 0.0f };

            meshData.Vertices[2] = MeshGenVertex{
                x + w, y, depth,
                0.0f, 0.0f, -1.0f,
                1.0f, 0.0f, 0.0f,
                1.0f, 0.0f };

            meshData.Vertices[3] = MeshGenVertex{
                x + w, y - h, depth,
                0.0f, 0.0f, -1.0f,
                1.0f, 0.0f, 0.0f,
                1.0f, 1.0f };

            meshData.Indices32[0] = 0;
            meshData.Indices32[1] = 1;
            meshData.Indices32[2] = 2;

            meshData.Indices32[3] = 0;
            meshData.Indices32[4] = 2;
            meshData.Indices32[5] = 3;

            return meshData;
        }

    private:

        void Subdivide(MeshGenData& meshData)
        {
            // Save a copy of the input geometry.
            auto inputCopy = MeshGenData{meshData};

            meshData.Vertices.resize(0);
            meshData.Indices32.resize(0);

            //       v1
            //       *
            //      / \
        	//     /   \
        	//  m0*-----*m1
            //   / \   / \
        	//  /   \ /   \
        	// *-----*-----*
            // v0    m2     v2

            auto numTris = (uint32_t)inputCopy.Indices32.size() / 3;
            for (auto i = 0u; i < numTris; ++i)
            {
                auto v0 = MeshGenVertex{inputCopy.Vertices[inputCopy.Indices32[i * 3 + 0]]};
                auto v1 = MeshGenVertex{inputCopy.Vertices[inputCopy.Indices32[i * 3 + 1]]};
                auto v2 = MeshGenVertex{inputCopy.Vertices[inputCopy.Indices32[i * 3 + 2]]};

                //
                // Generate the midpoints.
                //

                auto m0 = MeshGenVertex{MidPoint(v0, v1)};
                auto m1 = MeshGenVertex{MidPoint(v1, v2)};
                auto m2 = MeshGenVertex{MidPoint(v0, v2)};

                //
                // Add new geometry.
                //

                meshData.Vertices.push_back(v0); // 0
                meshData.Vertices.push_back(v1); // 1
                meshData.Vertices.push_back(v2); // 2
                meshData.Vertices.push_back(m0); // 3
                meshData.Vertices.push_back(m1); // 4
                meshData.Vertices.push_back(m2); // 5

                meshData.Indices32.push_back(i * 6 + 0);
                meshData.Indices32.push_back(i * 6 + 3);
                meshData.Indices32.push_back(i * 6 + 5);

                meshData.Indices32.push_back(i * 6 + 3);
                meshData.Indices32.push_back(i * 6 + 4);
                meshData.Indices32.push_back(i * 6 + 5);

                meshData.Indices32.push_back(i * 6 + 5);
                meshData.Indices32.push_back(i * 6 + 4);
                meshData.Indices32.push_back(i * 6 + 2);

                meshData.Indices32.push_back(i * 6 + 3);
                meshData.Indices32.push_back(i * 6 + 1);
                meshData.Indices32.push_back(i * 6 + 4);
            }
        }
        auto MidPoint(const MeshGenVertex& v0, const MeshGenVertex& v1) -> MeshGenVertex
        {
            auto p0 = DirectX::XMVECTOR{ DirectX::XMLoadFloat3(&v0.Position) };
            auto p1 = DirectX::XMVECTOR{ DirectX::XMLoadFloat3(&v1.Position) };

            auto n0 = DirectX::XMVECTOR{ DirectX::XMLoadFloat3(&v0.Normal) };
            auto n1 = DirectX::XMVECTOR{ DirectX::XMLoadFloat3(&v1.Normal) };

            auto tan0 = DirectX::XMVECTOR{ DirectX::XMLoadFloat3(&v0.TangentU) };
            auto tan1 = DirectX::XMVECTOR{ DirectX::XMLoadFloat3(&v1.TangentU) };

            auto tex0 = DirectX::XMVECTOR{ DirectX::XMLoadFloat2(&v0.TexC) };
            auto tex1 = DirectX::XMVECTOR{ DirectX::XMLoadFloat2(&v1.TexC) };

            // Compute the midpoints of all the attributes.  Vectors need to be normalized
            // since linear interpolating can make them not unit length.  
            auto pos = DirectX::XMVECTOR{ 0.5f * (p0 + p1) };
            auto normal = DirectX::XMVECTOR{ DirectX::XMVector3Normalize(0.5f * (n0 + n1)) };
            auto tangent = DirectX::XMVECTOR{ DirectX::XMVector3Normalize(0.5f * (tan0 + tan1)) };
            auto tex = DirectX::XMVECTOR{ 0.5f * (tex0 + tex1) };

            auto v = MeshGenVertex{};
            DirectX::XMStoreFloat3(&v.Position, pos);
            DirectX::XMStoreFloat3(&v.Normal, normal);
            DirectX::XMStoreFloat3(&v.TangentU, tangent);
            DirectX::XMStoreFloat2(&v.TexC, tex);

            return v;
        }
        void BuildCylinderTopCap(float bottomRadius, float topRadius, float height, uint32_t sliceCount, uint32_t stackCount, MeshGenData& meshData)
        {
            auto baseIndex = uint32_t{ static_cast<uint32_t>(meshData.Vertices.size()) };

            float y = 0.5f * height;
            float dTheta = 2.0f * DirectX::XM_Pi / sliceCount;

            // Duplicate cap ring vertices because the texture coordinates and normals differ.
            for (auto i = 0u; i <= sliceCount; ++i)
            {
                auto x = topRadius * cosf(i * dTheta);
                auto z = topRadius * sinf(i * dTheta);

                // Scale down by the height to try and make top cap texture coord area
                // proportional to base.
                auto u = x / height + 0.5f;
                auto v = z / height + 0.5f;

                meshData.Vertices.push_back(MeshGenVertex(
                    x, y, z,
                    0.0f, 1.0f, 0.0f,
                    1.0f, 0.0f, 0.0f,
                    u, v));
            }

            // Cap center vertex.
            meshData.Vertices.push_back(MeshGenVertex(
                0.0f, y, 0.0f,
                0.0f, 1.0f, 0.0f,
                1.0f, 0.0f, 0.0f,
                0.5f, 0.5f));

            // Index of center vertex.
            auto centerIndex = uint32_t{ static_cast<uint32_t>(meshData.Vertices.size() - 1) };

            for (auto i = 0u; i < sliceCount; ++i)
            {
                meshData.Indices32.push_back(centerIndex);
                meshData.Indices32.push_back(baseIndex + i + 1);
                meshData.Indices32.push_back(baseIndex + i);
            }
        }
        void BuildCylinderBottomCap(float bottomRadius, float topRadius, float height, uint32_t sliceCount, uint32_t stackCount, MeshGenData& meshData)
        {
            // 
            // Build bottom cap.
            //

            auto baseIndex = uint32_t{ static_cast<uint32_t>(meshData.Vertices.size()) };
            auto y = float{ -0.5f * height };

            // vertices of ring
            auto dTheta = float{ 2.0f * DirectX::XM_Pi / sliceCount };
            for (auto i = 0u; i <= sliceCount; ++i)
            {
                auto x = float{ bottomRadius * cosf(i * dTheta) };
                auto z = float{ bottomRadius * sinf(i * dTheta) };

                // Scale down by the height to try and make top cap texture coord area
                // proportional to base.
                auto u = float{ x / height + 0.5f };
                auto v = float{ z / height + 0.5f };

                meshData.Vertices.push_back(
                    MeshGenVertex{
                        x, y, z,
                        0.0f, -1.0f, 0.0f,
                        1.0f, 0.0f, 0.0f,
                        u, v 
                    });
            }

            // Cap center vertex.
            meshData.Vertices.push_back(
                MeshGenVertex{
                    0.0f, y, 0.0f,
                    0.0f, -1.0f, 0.0f,
                    1.0f, 0.0f, 0.0f,
                    0.5f, 0.5f 
                });

            // Cache the index of center vertex.
            auto centerIndex = uint32_t{ static_cast<uint32_t>(meshData.Vertices.size() - 1) };

            for (auto i = 0u; i < sliceCount; ++i)
            {
                meshData.Indices32.push_back(centerIndex);
                meshData.Indices32.push_back(baseIndex + i);
                meshData.Indices32.push_back(baseIndex + i + 1);
            }
        }
    };
}
