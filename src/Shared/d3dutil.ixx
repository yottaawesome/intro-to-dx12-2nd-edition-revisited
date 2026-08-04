//***************************************************************************************
// d3dApp.cpp by Frank Luna (C) 2015 All Rights Reserved.
//***************************************************************************************

export module shared:d3dutil;
import std;
import :win32;
import :mathhelper;
import :meshutil;
import :meshgen;
import :loadm3d;
import :build;

export
{
    constexpr auto gNumFrameResources = 3;

    inline constexpr auto SsaoAmbientMapFormat = DXGI::DXGI_FORMAT::DXGI_FORMAT_R16_UNORM;
    inline constexpr auto SceneNormalMapFormat = DXGI::DXGI_FORMAT::DXGI_FORMAT_R16G16B16A16_FLOAT;

    void ShowErrorMessage(const std::exception& ex)
    {
		Win32::MessageBoxA(nullptr, ex.what(), "Error", Win32::MbOk);
    }

    [[nodiscard]]
    inline auto WStringToAnsi(std::wstring_view wstr) -> std::string
    {
        if (wstr.empty())
            return {};

        // https://docs.microsoft.com/en-us/windows/win32/api/stringapiset/nf-stringapiset-widechartomultibyte
        // Returns the size in bytes, this differs from MultiByteToWideChar, which returns the size in characters
        auto sizeInBytes =
            Win32::WideCharToMultiByte(
                Win32::CpUtf8,									// CodePage
                Win32::WcNoBestFitChars,						// dwFlags 
                wstr.data(),									// lpWideCharStr
                static_cast<int>(wstr.size()),					// cchWideChar 
                nullptr,										// lpMultiByteStr
                0,												// cbMultiByte
                nullptr,										// lpDefaultChar
                nullptr											// lpUsedDefaultChar
            );
        if (sizeInBytes == 0)
            throw std::runtime_error{ "WideCharToMultiByte() [1] failed" };

        auto strTo = std::string(sizeInBytes / sizeof(char), '\0');
        auto status =
            WideCharToMultiByte(
                Win32::CpUtf8,									// CodePage
                Win32::WcNoBestFitChars,						// dwFlags 
                wstr.data(),									// lpWideCharStr
                static_cast<int>(wstr.size()),					// cchWideChar 
                strTo.data(),									// lpMultiByteStr
                static_cast<int>(strTo.size() * sizeof(char)),	// cbMultiByte
                nullptr,										// lpDefaultChar
                nullptr											// lpUsedDefaultChar
            );
        if (status == 0)
            throw std::runtime_error{ "WideCharToMultiByte() [2] failed" };

        return strTo;
    }

    [[nodiscard]]
    inline auto AnsiToWString(std::string_view str) -> std::wstring
    {
        if (str.empty())
            return {};

        // https://docs.microsoft.com/en-us/windows/win32/api/stringapiset/nf-stringapiset-multibytetowidechar
        // Returns the size in characters, this differs from WideCharToMultiByte, which returns the size in bytes
        auto sizeInCharacters =
            Win32::MultiByteToWideChar(
                Win32::CpUtf8,									// CodePage
                0,											// dwFlags
                str.data(),									// lpMultiByteStr
                static_cast<int>(str.size() * sizeof(char)),// cbMultiByte
                nullptr,									// lpWideCharStr
                0											// cchWideChar
            );
        if (sizeInCharacters == 0)
            throw std::runtime_error{ "MultiByteToWideChar() [1] failed" };

        auto wstrTo = std::wstring(sizeInCharacters, '\0');
        auto status =
            Win32::MultiByteToWideChar(
                Win32::CpUtf8,									// CodePage
                0,											// dwFlags
                str.data(),									// lpMultiByteStr
                static_cast<int>(str.size() * sizeof(char)),	// cbMultiByte
                wstrTo.data(),									// lpWideCharStr
                static_cast<int>(wstrTo.size())				// cchWideChar
            );
        if (status == 0)
            throw std::runtime_error{ "MultiByteToWideChar() [2] failed" };

        return wstrTo;
    }

    class DxException : public std::runtime_error
    {
    public:
        DxException() = default;
        DxException(
            Win32::HRESULT hr,
            const std::source_location& location = std::source_location::current()
        ) : ErrorCode(hr),
            Location(location),
			std::runtime_error{ WStringToAnsi(ToString()) }
        {}

        auto ToString() const -> std::wstring
        {
            // Get the string description of the error code.
            auto msg = std::wstring{ Win32::_com_error{ ErrorCode }.ErrorMessage() };

            auto err1 = std::format(
                "{} failed in {} at line {}",
                Location.function_name(),
                Location.file_name(),
                Location.line()
            );
            return std::format(L"{}; error: {}", AnsiToWString(err1), msg);
        }

        Win32::HRESULT ErrorCode = 0x0;
        std::source_location Location = std::source_location::current();
    };

    inline void ReleaseCom(Win32::IUnknown** obj)
    {
        if (obj && *obj)
        {
            (*obj)->Release();
            *obj = nullptr;
        }
    }

    inline void ThrowIfFailed(Win32::HRESULT hr)
    {
        if (Win32::Failed(hr))
            throw DxException(hr);
    }

    inline void d3dSetDebugName(DXGI::IDXGIObject* obj, const char* name)
    {
        if (obj)
            obj->SetPrivateData(D3D::WKPDID_D3DDebugObjectName, Win32::lstrlenA(name), name);
    }
    inline void d3dSetDebugName(D3D12::ID3D12Device* obj, const char* name)
    {
        if (obj)
            obj->SetPrivateData(D3D::WKPDID_D3DDebugObjectName, Win32::lstrlenA(name), name);
    }
    inline void d3dSetDebugName(D3D12::ID3D12DeviceChild* obj, const char* name)
    {
        if (obj)
            obj->SetPrivateData(D3D::WKPDID_D3DDebugObjectName, Win32::lstrlenA(name), name);
    }

    struct ModelVertex
    {
        ModelVertex() = default;
        ModelVertex(
            float px, float py, float pz,
            float nx, float ny, float nz,
            float u, float v) :
            Pos(px, py, pz),
            Normal(nx, ny, nz),
            TexC(u, v)
        {}

        DirectX::XMFLOAT3 Pos{};
        DirectX::XMFLOAT3 Normal{};
        DirectX::XMFLOAT2 TexC{};
        DirectX::XMFLOAT3 TangentU{};
    };

    class d3dUtil
    {
    public:
        static void WriteBinaryToFile(DXC::IDxcBlob* blob, const std::wstring& filename)
        {
            auto fout = std::ofstream{ filename, std::ios::binary };
            fout.write((char*)blob->GetBufferPointer(), blob->GetBufferSize());
            fout.close();
        }

        static auto IsKeyDown(int vkeyCode) -> bool
        {
            return (Win32::GetAsyncKeyState(vkeyCode) & 0x8000) != 0;
        }

        static constexpr auto Align(Win32::UINT size, Win32::UINT alignment) -> Win32::UINT
        {
            return (size + (alignment - 1)) & ~(alignment - 1);
        }

        static constexpr auto CalcConstantBufferByteSize(Win32::UINT byteSize) -> Win32::UINT
        {
            // Constant buffers must be a multiple of the minimum hardware
            // allocation size (usually 256 bytes).  So round up to nearest
            // multiple of 256.  We do this by adding 255 and then masking off
            // the lower 2 bytes which store all bits < 256.
            // Example: Suppose byteSize = 300.
            // (300 + 255) & ~255
            // 555 & ~255
            // 0x022B & ~0x00ff
            // 0x022B & 0xff00
            // 0x0200
            // 512
            return Align(byteSize, D3D12::D3d12ConstantBufferDataPlacementAlignment);
        }

        // Uses dxc for shader model 6.0+. compileArgs is same as you would pass to dxc command line.
        // For example: "-E main -T ps_6_0 -Zi -Fd pdbPath -D mydefine=1"
        // There are also helper strings defined in dxcapi.h (partial list):
        // #define DXC_ARG_DEBUG L"-Zi"
        // #define DXC_ARG_SKIP_VALIDATION L"-Vd"
        // #define DXC_ARG_SKIP_OPTIMIZATIONS L"-Od"
        // #define DXC_ARG_PACK_MATRIX_ROW_MAJOR L"-Zpr"
        // #define DXC_ARG_PACK_MATRIX_COLUMN_MAJOR L"-Zpc"
        static auto CompileShader(
            const std::wstring& filename, 
            const std::vector<Win32::LPCWSTR>& compileArgs
        ) -> Microsoft::WRL::ComPtr<DXC::IDxcBlob>
        {
            if (not std::filesystem::exists(filename))
            {
                auto msg = std::format(L"{} not found.", filename);
                Win32::OutputDebugStringW(msg.c_str());
                Win32::MessageBoxW(0, msg.c_str(), 0, 0);
            }

			using Microsoft::WRL::ComPtr;
            static auto [utils, compiler, defaultIncludeHandler] = 
				[] static -> std::tuple<ComPtr<DXC::IDxcUtils>, ComPtr<DXC::IDxcCompiler3>, ComPtr<DXC::IDxcIncludeHandler>>
                {
                    auto utils = ComPtr<DXC::IDxcUtils>{};
					if (auto result = DXC::DxcCreateInstance(DXC::CLSID_DxcUtils, __uuidof(DXC::IDxcUtils), &utils); Win32::Failed(result))
						throw DxException{ result };
                    auto compiler = ComPtr<DXC::IDxcCompiler3>{};
                    if (auto result = DXC::DxcCreateInstance(DXC::CLSID_DxcCompiler, __uuidof(DXC::IDxcCompiler3), &compiler); Win32::Failed(result))
                        throw DxException{ result };
                    auto defaultIncludeHandler = ComPtr<DXC::IDxcIncludeHandler>{};
                    if (auto result = utils->CreateDefaultIncludeHandler(&defaultIncludeHandler); Win32::Failed(result))
                        throw DxException{ result };
                    return std::make_tuple(utils, compiler, defaultIncludeHandler);
                }();

            // Use IDxcUtils to load the text file.
            auto codePage = std::uint32_t{ Win32::CpUtf8 };
            auto sourceBlob = Microsoft::WRL::ComPtr<DXC::IDxcBlobEncoding>{};
            if (auto hr = utils->LoadFile(filename.c_str(), &codePage, &sourceBlob); Win32::Failed(hr))
                throw DxException{ hr };

            auto sourceBuffer = DXC::DxcBuffer{
                .Ptr = sourceBlob->GetBufferPointer(),
                .Size = sourceBlob->GetBufferSize(),
                .Encoding = 0,
            };

            auto result = Microsoft::WRL::ComPtr<DXC::IDxcResult>{};
            auto hr = Win32::HRESULT{
                compiler->Compile(
                    &sourceBuffer,               // source code
                    const_cast<Win32::LPCWSTR*>(compileArgs.data()),          // arguments
                    static_cast<Win32::UINT>(compileArgs.size()),    // argument count
                    defaultIncludeHandler.Get(), // include handler
                    __uuidof(DXC::IDxcResult),
                    &result // output
                )};
            if (Win32::Succeeded(hr))
                result->GetStatus(&hr);

            // Get errors and output them if any.
            auto errorMsgs = Microsoft::WRL::ComPtr<DXC::IDxcBlobUtf8>{};
            result->GetOutput(DXC::DXC_OUT_KIND::DXC_OUT_ERRORS, __uuidof(DXC::IDxcBlobUtf8), &errorMsgs, nullptr);

            if (errorMsgs and errorMsgs->GetStringLength())
            {
                auto errorText = AnsiToWString(errorMsgs->GetStringPointer());
                // replace the hlsl.hlsl placeholder in the error string with the shader filename.
                auto dummyFilename = std::wstring{L"hlsl.hlsl"};
                errorText.replace(errorText.find(dummyFilename), dummyFilename.length(), filename);
                Win32::OutputDebugStringW(errorText.c_str());
                ThrowIfFailed(Win32::HRCodes::Fail);
            }

            // Get the DX intermediate language, which the GPU driver will translate
            // into native GPU code.
            auto dxil = Microsoft::WRL::ComPtr<DXC::IDxcBlob>{};
            if (auto hr = result->GetOutput(DXC::DXC_OUT_KIND::DXC_OUT_OBJECT, __uuidof(DXC::IDxcBlob), &dxil, nullptr); Win32::Failed(hr))
                throw DxException{ hr }; 

            // Write PDB data for PIX debugging.
            if constexpr (IsDebugBuild)
            {
                constexpr auto pdbDirectory = "HLSL PDB/";
                if (not std::filesystem::exists(pdbDirectory))
                    std::filesystem::create_directory(pdbDirectory);
                auto pdbData = Microsoft::WRL::ComPtr<DXC::IDxcBlob>{};
                auto pdbPathFromCompiler = Microsoft::WRL::ComPtr<DXC::IDxcBlobUtf16>{};
                if (auto hr = result->GetOutput(DXC::DXC_OUT_KIND::DXC_OUT_PDB, __uuidof(DXC::IDxcBlob), &pdbData, &pdbPathFromCompiler); Win32::Failed(hr))
                    throw DxException{ hr };
                WriteBinaryToFile(pdbData.Get(), AnsiToWString(pdbDirectory) + std::wstring(pdbPathFromCompiler->GetStringPointer()));
            }
            // Return the data blob containing the DXIL code.
            return dxil;
        }

        static auto ByteCodeFromBlob(DXC::IDxcBlob* shader) -> D3D12::D3D12_SHADER_BYTECODE
        {
            return { reinterpret_cast<Win32::BYTE*>(shader->GetBufferPointer()), shader->GetBufferSize() };
        }

        static auto InitDefaultPso(
            DXGI::DXGI_FORMAT rtvFormat,
            DXGI::DXGI_FORMAT dsvFormat,
            const std::vector<D3D12::D3D12_INPUT_ELEMENT_DESC>& inputLayout, 
            D3D12::ID3D12RootSignature* rootSig,
            DXC::IDxcBlob* vertexShader, 
            DXC::IDxcBlob* pixelShader
        ) -> D3D12::D3D12_GRAPHICS_PIPELINE_STATE_DESC
        {
            return {
                .pRootSignature = rootSig,
                .VS = ByteCodeFromBlob(vertexShader),
                .PS = ByteCodeFromBlob(pixelShader),
                .BlendState = D3D12::CD3DX12_BLEND_DESC(D3D12::D3D12_DEFAULT),
                .SampleMask = std::numeric_limits<std::uint32_t>::max(),
                .RasterizerState = D3D12::CD3DX12_RASTERIZER_DESC(D3D12::D3D12_DEFAULT),
                .DepthStencilState = D3D12::CD3DX12_DEPTH_STENCIL_DESC(D3D12::D3D12_DEFAULT),
                .InputLayout = {
                    inputLayout.data(), 
                    static_cast<std::uint32_t>(inputLayout.size()) 
                },
                .PrimitiveTopologyType = D3D12::D3D12_PRIMITIVE_TOPOLOGY_TYPE::D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE,
                .NumRenderTargets = 1,
                .RTVFormats = { rtvFormat },
                .DSVFormat = dsvFormat,
                .SampleDesc = { .Count = 1, .Quality = 0 },
            };
        }

        static auto CreateRandomTexture(
            D3D12::ID3D12Device* device, 
            DirectX::ResourceUploadBatch& resourceUpload,
            size_t width, 
            size_t height
        ) -> Microsoft::WRL::ComPtr<D3D12::ID3D12Resource>
        {
            auto initData = std::vector<DirectX::PackedVector::XMCOLOR>(width * height);
            for (auto i = 0; i < height; ++i)
            {
                for (auto j = 0; j < width; ++j)
                {
                    // Random vector in [0,1).
                    auto v = DirectX::XMFLOAT4{
                        MathHelper::RandF(),
                        MathHelper::RandF(),
                        MathHelper::RandF(),
                        MathHelper::RandF()
                    };
                    initData[i * width + j] = DirectX::PackedVector::XMCOLOR(v.x, v.y, v.z, v.w);
                }
            }
            auto subResourceData = D3D12::D3D12_SUBRESOURCE_DATA{
                .pData = initData.data(),
                .RowPitch = static_cast<std::uint32_t>(width * sizeof(DirectX::PackedVector::XMCOLOR)),
                .SlicePitch = static_cast<std::uint32_t>(width * width * sizeof(DirectX::PackedVector::XMCOLOR))
            };
            
            Microsoft::WRL::ComPtr<D3D12::ID3D12Resource> randomTex;
            ThrowIfFailed(DirectX::CreateTextureFromMemory(device,
                resourceUpload,
                width, height,
                DXGI_FORMAT_R8G8B8A8_UNORM,
                subResourceData,
                &randomTex));

            return randomTex;
        }

        static auto BuildShapeGeometry(
            ID3D12Device* device, 
            DirectX::ResourceUploadBatch& uploadBatch, 
            bool useIndex32 = false
        ) -> std::unique_ptr<MeshGeometry>
        {
            auto meshGen = MeshGen{};
            auto box = meshGen.CreateBox(1.0f, 1.0f, 1.0f, 3);
            auto grid = meshGen.CreateGrid(20.0f, 30.0f, 30, 20);
            auto sphere = meshGen.CreateSphere(0.5f, 20, 20);
            auto cylinder = meshGen.CreateCylinder(0.5f, 0.3f, 3.0f, 20, 20);
            auto quad = meshGen.CreateQuad(0.0f, 0.0f, 1.0f, 1.0f, 0.0f);

            //
            // We are concatenating all the geometry into one big vertex/index buffer.  So
            // define the regions in the buffer each submesh covers.
            //
            auto compositeMesh = MeshGenData{};
            auto boxSubmesh = compositeMesh.AppendSubmesh(box);
            auto gridSubmesh = compositeMesh.AppendSubmesh(grid);
            auto sphereSubmesh = compositeMesh.AppendSubmesh(sphere);
            auto cylinderSubmesh = compositeMesh.AppendSubmesh(cylinder);
            auto quadSubmesh = compositeMesh.AppendSubmesh(quad);

            // Extract the vertex elements we are interested into our vertex buffer. 
            auto vertices = std::vector<ModelVertex>(compositeMesh.Vertices.size());
            for (auto i = 0ull; i < compositeMesh.Vertices.size(); ++i)
            {
                vertices[i].Pos = compositeMesh.Vertices[i].Position;
                vertices[i].Normal = compositeMesh.Vertices[i].Normal;
                vertices[i].TexC = compositeMesh.Vertices[i].TexC;
                vertices[i].TangentU = compositeMesh.Vertices[i].TangentU;
            }

            const auto indexCount = static_cast<std::uint32_t>(compositeMesh.Indices32.size());

            const auto indexElementByteSize = static_cast<std::uint32_t>(useIndex32 ? sizeof(std::uint32_t) : sizeof(std::uint16_t));
            const auto vbByteSize = static_cast<std::uint32_t>(vertices.size() * sizeof(ModelVertex));
            const auto ibByteSize = static_cast<std::uint32_t>(indexCount * indexElementByteSize);

            const auto indexData = useIndex32 ?
                reinterpret_cast<Win32::byte*>(compositeMesh.Indices32.data()) :
                reinterpret_cast<Win32::byte*>(compositeMesh.GetIndices16().data());

            auto geo = std::make_unique<MeshGeometry>();
            geo->Name = "shapeGeo";

            geo->VertexBufferCPU.resize(vbByteSize);
            std::memcpy(geo->VertexBufferCPU.data(), vertices.data(), vbByteSize);

            geo->IndexBufferCPU.resize(ibByteSize);
            std::memcpy(geo->IndexBufferCPU.data(), indexData, ibByteSize);

            DirectX::CreateStaticBuffer(device, uploadBatch,
                vertices.data(), vertices.size(), sizeof(ModelVertex),
                D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER, &geo->VertexBufferGPU);

            DirectX::CreateStaticBuffer(device, uploadBatch,
                indexData, indexCount, indexElementByteSize,
                D3D12_RESOURCE_STATE_INDEX_BUFFER, &geo->IndexBufferGPU);

            geo->VertexByteStride = sizeof(ModelVertex);
            geo->VertexBufferByteSize = vbByteSize;
            geo->IndexFormat = useIndex32 ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R16_UINT;
            geo->IndexBufferByteSize = ibByteSize;

            geo->DrawArgs["box"] = boxSubmesh;
            geo->DrawArgs["grid"] = gridSubmesh;
            geo->DrawArgs["sphere"] = sphereSubmesh;
            geo->DrawArgs["cylinder"] = cylinderSubmesh;
            geo->DrawArgs["quad"] = quadSubmesh;

            return geo;
        }

        static auto BuildSkullGeometry(D3D12::ID3D12Device* device, DirectX::ResourceUploadBatch& uploadBatch) -> std::unique_ptr<MeshGeometry>
        {
            auto fin = std::ifstream("Models/skull.txt");

            if (not fin)
                throw std::runtime_error{"Models/skull.txt not found."};

            auto vcount = 0u;
            auto tcount = 0u;
            auto ignore = std::string{};

            fin >> ignore >> vcount;
            fin >> ignore >> tcount;
            fin >> ignore >> ignore >> ignore >> ignore;

            auto vMinf3 = DirectX::XMFLOAT3(+MathHelper::Infinity, +MathHelper::Infinity, +MathHelper::Infinity);
            auto vMaxf3 = DirectX::XMFLOAT3(-MathHelper::Infinity, -MathHelper::Infinity, -MathHelper::Infinity);

            auto vMin = DirectX::XMLoadFloat3(&vMinf3);
            auto vMax = DirectX::XMLoadFloat3(&vMaxf3);

            auto vertices = std::vector<ModelVertex>(vcount);
            for (auto i = 0u; i < vcount; ++i)
            {
                fin >> vertices[i].Pos.x >> vertices[i].Pos.y >> vertices[i].Pos.z;
                fin >> vertices[i].Normal.x >> vertices[i].Normal.y >> vertices[i].Normal.z;

                auto P = DirectX::XMLoadFloat3(&vertices[i].Pos);

                // Project point onto unit sphere and generate spherical texture coordinates.
                auto spherePos = DirectX::XMFLOAT3{};
                DirectX::XMStoreFloat3(&spherePos, DirectX::XMVector3Normalize(P));

                auto N = DirectX::XMLoadFloat3(&vertices[i].Normal);

                // Generate a tangent vector so normal mapping works.  We aren't applying
                // a texture map to the skull, so we just need any tangent vector so that
                // the math works out to give us the original interpolated vertex normal.
                auto up = DirectX::XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
                if (std::fabsf(DirectX::XMVectorGetX(DirectX::XMVector3Dot(N, up))) < 1.0f - 0.001f)
                {
                    auto T = DirectX::XMVector3Normalize(DirectX::XMVector3Cross(up, N));
                    DirectX::XMStoreFloat3(&vertices[i].TangentU, T);
                }
                else
                {
                    up = DirectX::XMVectorSet(0.0f, 0.0f, 1.0f, 0.0f);
                    auto T = DirectX::XMVector3Normalize(DirectX::XMVector3Cross(N, up));
                    DirectX::XMStoreFloat3(&vertices[i].TangentU, T);
                }

                // The skull mesh does not have defined texture coordinates, but 
                // we can auto generate some. We generate sphereical projection
                // texture coordinates by projecting the vertices onto the unit
                // sphere. Because the skull is not a sphere, there will be some
                // distortion from this transformation, but it gives reasonable 
                // texture coordinates when we have none.

                auto theta = std::atan2(spherePos.z, spherePos.x);

                // Put in [0, 2pi].
                if (theta < 0.0f)
                    theta += DirectX::TwoPi;

                auto phi = std::acos(spherePos.y);

                auto u = theta / (2.0f * DirectX::Pi);
                auto v = phi / DirectX::Pi;

                vertices[i].TexC = { u, v };

                vMin = DirectX::XMVectorMin(vMin, P);
                vMax = DirectX::XMVectorMax(vMax, P);
            }

            auto bounds = DirectX::BoundingBox{};
            DirectX::XMStoreFloat3(&bounds.Center, 0.5f * (vMin + vMax));
            DirectX::XMStoreFloat3(&bounds.Extents, 0.5f * (vMax - vMin));

            fin >> ignore;
            fin >> ignore;
            fin >> ignore;

            auto indices = std::vector<std::int32_t>(3 * tcount);
            for (auto i = 0u; i < tcount; ++i)
            {
                fin >> indices[i * 3 + 0] >> indices[i * 3 + 1] >> indices[i * 3 + 2];
            }
            fin.close();

            const auto vbByteSize = static_cast<std::uint32_t>(vertices.size() * sizeof(ModelVertex));
            const auto ibByteSize = static_cast<std::uint32_t>(indices.size() * sizeof(std::int32_t));

            auto geo = std::make_unique<MeshGeometry>();
            geo->Name = "skullGeo";

            geo->VertexBufferCPU.resize(vbByteSize);
            std::memcpy(geo->VertexBufferCPU.data(), vertices.data(), vbByteSize);

            geo->IndexBufferCPU.resize(ibByteSize);
            std::memcpy(geo->IndexBufferCPU.data(), indices.data(), ibByteSize);

            DirectX::CreateStaticBuffer(
                device, 
                uploadBatch,
                vertices.data(), 
                vertices.size(), 
                sizeof(ModelVertex),
                D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER, 
                &geo->VertexBufferGPU
            );

            DirectX::CreateStaticBuffer(
                device, 
                uploadBatch,
                indices.data(), 
                indices.size(), 
                sizeof(std::uint32_t),
                D3D12_RESOURCE_STATE_INDEX_BUFFER, 
                &geo->IndexBufferGPU
            );

            geo->VertexByteStride = sizeof(ModelVertex);
            geo->VertexBufferByteSize = vbByteSize;
            geo->IndexFormat = DXGI_FORMAT_R32_UINT;
            geo->IndexBufferByteSize = ibByteSize;

            auto submesh = SubmeshGeometry{
                .IndexCount = static_cast<std::uint32_t>(indices.size()),
                .StartIndexLocation = 0,
                .BaseVertexLocation = 0,
                .VertexCount = static_cast<std::uint32_t>(vertices.size()),
                .Bounds = bounds
            };
            geo->DrawArgs["skull"] = submesh;

            return geo;
        }

        //loadm3d.h <- SkinnedData.h
        static auto LoadSimpleModelGeometry(
            D3D12::ID3D12Device* device,
            DirectX::ResourceUploadBatch& uploadBatch,
            const std::string& filename,
            const std::string& geoName,
            bool useIndex32 = false
        ) -> std::unique_ptr<MeshGeometry>
        {
            std::vector<M3DLoader::Vertex> m3dVertices;
            std::vector<UINT> indices32;
            std::vector<M3DLoader::Subset> subsets;
            std::vector<M3DLoader::M3dMaterial> mats;

            M3DLoader loader;
            loader.LoadM3d(filename, m3dVertices, indices32, subsets, mats);

            // Assume simple model has one subset and one material.
            //assert(subsets.size() == 1);
            //assert(mats.size() == 1);

            DirectX::XMFLOAT3 vMinf3(+MathHelper::Infinity, +MathHelper::Infinity, +MathHelper::Infinity);
            DirectX::XMFLOAT3 vMaxf3(-MathHelper::Infinity, -MathHelper::Infinity, -MathHelper::Infinity);

            DirectX::XMVECTOR vMin = DirectX::XMLoadFloat3(&vMinf3);
            DirectX::XMVECTOR vMax = DirectX::XMLoadFloat3(&vMaxf3);

            std::vector<ModelVertex> vertices(m3dVertices.size());
            for (UINT i = 0; i < m3dVertices.size(); ++i)
            {
                DirectX::XMVECTOR P = DirectX::XMLoadFloat3(&m3dVertices[i].Pos);

                vertices[i].Pos = m3dVertices[i].Pos;
                vertices[i].Normal = m3dVertices[i].Normal;
                vertices[i].TangentU = DirectX::XMFLOAT3(m3dVertices[i].TangentU.x, m3dVertices[i].TangentU.y, m3dVertices[i].TangentU.z);
                vertices[i].TexC = m3dVertices[i].TexC;

                vMin = DirectX::XMVectorMin(vMin, P);
                vMax = DirectX::XMVectorMax(vMax, P);
            }

            DirectX::BoundingBox bounds;
            DirectX::XMStoreFloat3(&bounds.Center, 0.5f * (vMin + vMax));
            DirectX::XMStoreFloat3(&bounds.Extents, 0.5f * (vMax - vMin));

            const UINT indexElementByteSize = useIndex32 ? sizeof(uint32_t) : sizeof(uint16_t);
            const UINT vbByteSize = (UINT)vertices.size() * sizeof(ModelVertex);
            const UINT ibByteSize = (UINT)indices32.size() * indexElementByteSize;

            auto geo = std::make_unique<MeshGeometry>();
            geo->Name = geoName;

            geo->VertexBufferCPU.resize(vbByteSize);
            std::memcpy(geo->VertexBufferCPU.data(), vertices.data(), vbByteSize);

            DirectX::CreateStaticBuffer(device, uploadBatch,
                vertices.data(), vertices.size(), sizeof(ModelVertex),
                D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER, &geo->VertexBufferGPU);

            if (useIndex32)
            {
                geo->IndexBufferCPU.resize(ibByteSize);
                std::memcpy(geo->IndexBufferCPU.data(), indices32.data(), ibByteSize);

                DirectX::CreateStaticBuffer(
                    device, uploadBatch,
                    indices32.data(), indices32.size(), sizeof(uint32_t),
                    D3D12_RESOURCE_STATE_INDEX_BUFFER, &geo->IndexBufferGPU);
            }
            else
            {
                std::vector<Win32::USHORT> indices16(indices32.size());
                std::transform(std::begin(indices32), std::end(indices32), std::begin(indices16), [](UINT x)
                    {
                        return static_cast<Win32::USHORT>(x);
                    });

                geo->IndexBufferCPU.resize(ibByteSize);
                std::memcpy(geo->IndexBufferCPU.data(), indices16.data(), ibByteSize);

                DirectX::CreateStaticBuffer(
                    device, uploadBatch,
                    indices16.data(), indices16.size(), sizeof(uint16_t),
                    D3D12_RESOURCE_STATE_INDEX_BUFFER, &geo->IndexBufferGPU);
            }

            geo->VertexByteStride = sizeof(ModelVertex);
            geo->VertexBufferByteSize = vbByteSize;
            geo->IndexFormat = useIndex32 ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R16_UINT;
            geo->IndexBufferByteSize = ibByteSize;

            SubmeshGeometry submesh;
            submesh.IndexCount = (UINT)indices32.size();
            submesh.StartIndexLocation = 0;
            submesh.BaseVertexLocation = 0;
            submesh.VertexCount = (UINT)vertices.size();
            submesh.Bounds = bounds;

            geo->DrawArgs["subset0"] = submesh;

            return geo;
        }

        static auto CalcGaussWeights(float sigma) -> std::vector<float>
        {
            float twoSigma2 = 2.0f * sigma * sigma;

            // Estimate the blur radius based on sigma since sigma controls the "width" of the bell curve.
            // For example, for sigma = 3, the width of the bell curve is 
            int blurRadius = (int)ceil(2.0f * sigma);

            std::vector<float> weights;
            weights.resize(2 * blurRadius + 1);

            float weightSum = 0.0f;

            for (int i = -blurRadius; i <= blurRadius; ++i)
            {
                float x = (float)i;

                weights[i + blurRadius] = std::expf(-x * x / twoSigma2);

                weightSum += weights[i + blurRadius];
            }

            // Divide by the sum so all the weights add up to 1.0.
            for (int i = 0; i < weights.size(); ++i)
            {
                weights[i] /= weightSum;
            }

            return weights;
        }
    };
    
    // Simple struct to represent a material for our demos. 
    struct Material
    {
        // Unique material name for lookup.
        std::string Name;

        // Index into material buffer.
        int MatIndex = -1;

        // For bindless texturing.
        int AlbedoBindlessIndex = -1;
        int NormalBindlessIndex = -1;
        int GlossHeightAoBindlessIndex = -1;

        // Dirty flag indicating the material has changed and we need to update the buffer.
        // Because we have a material buffer for each FrameResource, we have to apply the
        // update to each FrameResource.  Thus, when we modify a material we should set 
        // NumFramesDirty = gNumFrameResources so that each frame resource gets the update.
        int NumFramesDirty = gNumFrameResources;

        // Material constant buffer data used for shading.
        DirectX::XMFLOAT4 DiffuseAlbedo = { 1.0f, 1.0f, 1.0f, 1.0f };
        DirectX::XMFLOAT3 FresnelR0 = { 0.01f, 0.01f, 0.01f };
        float Roughness = .25f;
        float DisplacementScale = 1.0f;
        DirectX::XMFLOAT4X4 MatTransform = MathHelper::Identity4x4;

        // Used in ray tracing demos only.
        float TransparencyWeight = 0.0f;
        float IndexOfRefraction = 0.0f;
    };
}
