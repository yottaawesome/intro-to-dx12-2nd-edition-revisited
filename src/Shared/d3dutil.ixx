export module shared:d3dutil;
import std;
import :win32;
import :mathhelper;
import :meshutil;
import :meshgen;

export
{
    constexpr auto gNumFrameResources = 3;

    inline constexpr auto SsaoAmbientMapFormat = DXGI::DXGI_FORMAT::DXGI_FORMAT_R16_UNORM;
    inline constexpr auto SceneNormalMapFormat = DXGI::DXGI_FORMAT::DXGI_FORMAT_R16G16B16A16_FLOAT;

    inline auto AnsiToWString(const std::string& str) -> std::wstring
    {
        auto buffer = std::array<Win32::WCHAR, 512>{};
        Win32::MultiByteToWideChar(Win32::CpAcp, 0, str.c_str(), -1, buffer.data(), static_cast<int>(buffer.size()));
        return buffer.data();
    }

    class DxException
    {
    public:
        DxException() = default;
        DxException(
            Win32::HRESULT hr,
            const std::source_location& location = std::source_location::current()
        ) : ErrorCode(hr),
            Location(location)
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

    //TODO: migrate
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

        static auto Align(Win32::UINT size, Win32::UINT alignment) -> Win32::UINT
        {
            return (size + (alignment - 1)) & ~(alignment - 1);
        }

        static auto CalcConstantBufferByteSize(Win32::UINT byteSize) -> Win32::UINT
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
            std::vector<Win32::LPCWSTR>& compileArgs
        ) -> Microsoft::WRL::ComPtr<DXC::IDxcBlob>
        {
            if (!std::filesystem::exists(filename))
            {
                auto msg = std::format(L"{} not found.", filename);
                Win32::OutputDebugStringW(msg.c_str());
                Win32::MessageBoxW(0, msg.c_str(), 0, 0);
            }

			using Microsoft::WRL::ComPtr;
            static auto [utils, compiler, defaultIncludeHandler] = 
				[] static -> std::tuple<ComPtr<DXC::IDxcUtils>, ComPtr<DXC::IDxcCompiler3>, ComPtr<DXC::IDxcIncludeHandler>>
                {
                    auto result = HRESULT{};
                    auto utils = ComPtr<DXC::IDxcUtils>{};
                    result = DXC::DxcCreateInstance(DXC::CLSID_DxcUtils, __uuidof(DXC::IDxcUtils), &utils);
                    ThrowIfFailed(result);
                    auto compiler = ComPtr<DXC::IDxcCompiler3>{};
                    result = DXC::DxcCreateInstance(DXC::CLSID_DxcCompiler, __uuidof(DXC::IDxcCompiler3), &compiler);
                    ThrowIfFailed(result);
                    auto defaultIncludeHandler = ComPtr<DXC::IDxcIncludeHandler>{};
                    result = utils->CreateDefaultIncludeHandler(&defaultIncludeHandler);
                    ThrowIfFailed(result);
                    return std::make_tuple(utils, compiler, defaultIncludeHandler);
                }();

            // Use IDxcUtils to load the text file.
            auto codePage = std::uint32_t{ Win32::CpUtf8 };
            auto sourceBlob = Microsoft::WRL::ComPtr<DXC::IDxcBlobEncoding>{};
            ThrowIfFailed(utils->LoadFile(filename.c_str(), &codePage, &sourceBlob));

            auto sourceBuffer = DXC::DxcBuffer{
                .Ptr = sourceBlob->GetBufferPointer(),
                .Size = sourceBlob->GetBufferSize(),
                .Encoding = 0,
            };

            auto result = Microsoft::WRL::ComPtr<DXC::IDxcResult>{};
            auto hr = Win32::HRESULT{
                compiler->Compile(
                    &sourceBuffer,               // source code
                    compileArgs.data(),          // arguments
                    static_cast<Win32::UINT>(compileArgs.size()),    // argument count
                    defaultIncludeHandler.Get(), // include handler
                    __uuidof(DXC::IDxcResult),
                    &result // output
                )};

            if (Win32::Succeeded(hr))
                result->GetStatus(&hr);

            // Get errors and output them if any.
            auto errorMsgs = Microsoft::WRL::ComPtr<DXC::IDxcBlobUtf8>{};
            result->GetOutput(
                DXC::DXC_OUT_KIND::DXC_OUT_ERRORS,
				__uuidof(DXC::IDxcBlobUtf8), 
                &errorMsgs, 
                nullptr
            );

            if (errorMsgs && errorMsgs->GetStringLength())
            {
                auto errorText = AnsiToWString(errorMsgs->GetStringPointer());

                // replace the hlsl.hlsl placeholder in the error string with the shader filename.
                auto dummyFilename = std::wstring{L"hlsl.hlsl"};
                errorText.replace(errorText.find(dummyFilename), dummyFilename.length(), filename);

                OutputDebugStringW(errorText.c_str());
                ThrowIfFailed(Win32::Hresults::Fail);
            }

            // Get the DX intermediate language, which the GPU driver will translate
            // into native GPU code.
            auto dxil = Microsoft::WRL::ComPtr<DXC::IDxcBlob>{};
            ThrowIfFailed(
                result->GetOutput(
                    DXC::DXC_OUT_KIND::DXC_OUT_OBJECT,
                    __uuidof(DXC::IDxcBlob),
                    &dxil, 
                    nullptr
                ));

#if defined(DEBUG) || defined(_DEBUG)  
            // Write PDB data for PIX debugging.
            const auto pdbDirectory = std::string{ "HLSL PDB/" };
            if (!std::filesystem::exists(pdbDirectory))
            {
                std::filesystem::create_directory(pdbDirectory);
            }

            auto pdbData = Microsoft::WRL::ComPtr<DXC::IDxcBlob>{};
            auto pdbPathFromCompiler = Microsoft::WRL::ComPtr<DXC::IDxcBlobUtf16>{};
            ThrowIfFailed(
                result->GetOutput(
                    DXC::DXC_OUT_KIND::DXC_OUT_PDB, 
                    __uuidof(DXC::IDxcBlob),
                    &pdbData, 
                    &pdbPathFromCompiler
                ));
            WriteBinaryToFile(
                pdbData.Get(),
                AnsiToWString(pdbDirectory) + std::wstring(pdbPathFromCompiler->GetStringPointer())
            );
#endif
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
            auto psoDesc = D3D12::D3D12_GRAPHICS_PIPELINE_STATE_DESC{};

            psoDesc.InputLayout = { inputLayout.data(), static_cast<Win32::UINT>(inputLayout.size()) };
            psoDesc.pRootSignature = rootSig;
            psoDesc.VS = ByteCodeFromBlob(vertexShader);
            psoDesc.PS = ByteCodeFromBlob(pixelShader);

            psoDesc.RasterizerState = D3D12::CD3DX12_RASTERIZER_DESC(D3D12::D3D12_DEFAULT);
            psoDesc.BlendState = D3D12::CD3DX12_BLEND_DESC(D3D12::D3D12_DEFAULT);
            psoDesc.DepthStencilState = D3D12::CD3DX12_DEPTH_STENCIL_DESC(D3D12::D3D12_DEFAULT);
            psoDesc.SampleMask = Win32::UIntMax;
            psoDesc.PrimitiveTopologyType = D3D12::D3D12_PRIMITIVE_TOPOLOGY_TYPE::D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
            psoDesc.NumRenderTargets = 1;
            psoDesc.RTVFormats[0] = rtvFormat;
            psoDesc.SampleDesc.Count = 1;
            psoDesc.SampleDesc.Quality = 0;
            psoDesc.DSVFormat = dsvFormat;

            return psoDesc;
        }

        auto CreateRandomTexture(
            D3D12::ID3D12Device* device, 
            DirectX::ResourceUploadBatch& resourceUpload,
            size_t width, 
            size_t height
        ) -> Microsoft::WRL::ComPtr<D3D12::ID3D12Resource>
        {
            std::vector<DirectX::PackedVector::XMCOLOR> initData(width * height);
            for (int i = 0; i < height; ++i)
            {
                for (int j = 0; j < width; ++j)
                {
                    // Random vector in [0,1).
                    DirectX::XMFLOAT4 v(
                        MathHelper::RandF(),
                        MathHelper::RandF(),
                        MathHelper::RandF(),
                        MathHelper::RandF());

                    initData[i * width + j] = DirectX::PackedVector::XMCOLOR(v.x, v.y, v.z, v.w);
                }
            }

            D3D12::D3D12_SUBRESOURCE_DATA subResourceData = {};
            subResourceData.pData = initData.data();
            subResourceData.RowPitch = width * sizeof(DirectX::PackedVector::XMCOLOR);
            subResourceData.SlicePitch = subResourceData.RowPitch * width;

            Microsoft::WRL::ComPtr<D3D12::ID3D12Resource> randomTex;
            ThrowIfFailed(DirectX::CreateTextureFromMemory(device,
                resourceUpload,
                width, height,
                DXGI_FORMAT_R8G8B8A8_UNORM,
                subResourceData,
                &randomTex));

            return randomTex;
        }

        auto BuildShapeGeometry(
            ID3D12Device* device, 
            DirectX::ResourceUploadBatch& uploadBatch, 
            bool useIndex32
        ) -> std::unique_ptr<MeshGeometry>
        {
            MeshGen meshGen;
            MeshGenData box = meshGen.CreateBox(1.0f, 1.0f, 1.0f, 3);
            MeshGenData grid = meshGen.CreateGrid(20.0f, 30.0f, 30, 20);
            MeshGenData sphere = meshGen.CreateSphere(0.5f, 20, 20);
            MeshGenData cylinder = meshGen.CreateCylinder(0.5f, 0.3f, 3.0f, 20, 20);
            MeshGenData quad = meshGen.CreateQuad(0.0f, 0.0f, 1.0f, 1.0f, 0.0f);

            //
            // We are concatenating all the geometry into one big vertex/index buffer.  So
            // define the regions in the buffer each submesh covers.
            //
            MeshGenData compositeMesh;
            SubmeshGeometry boxSubmesh = compositeMesh.AppendSubmesh(box);
            SubmeshGeometry gridSubmesh = compositeMesh.AppendSubmesh(grid);
            SubmeshGeometry sphereSubmesh = compositeMesh.AppendSubmesh(sphere);
            SubmeshGeometry cylinderSubmesh = compositeMesh.AppendSubmesh(cylinder);
            SubmeshGeometry quadSubmesh = compositeMesh.AppendSubmesh(quad);

            // Extract the vertex elements we are interested into our vertex buffer. 
            std::vector<ModelVertex> vertices(compositeMesh.Vertices.size());
            for (size_t i = 0; i < compositeMesh.Vertices.size(); ++i)
            {
                vertices[i].Pos = compositeMesh.Vertices[i].Position;
                vertices[i].Normal = compositeMesh.Vertices[i].Normal;
                vertices[i].TexC = compositeMesh.Vertices[i].TexC;
                vertices[i].TangentU = compositeMesh.Vertices[i].TangentU;
            }

            const uint32_t indexCount = (UINT)compositeMesh.Indices32.size();

            const UINT indexElementByteSize = useIndex32 ? sizeof(uint32_t) : sizeof(uint16_t);
            const UINT vbByteSize = (UINT)vertices.size() * sizeof(ModelVertex);
            const UINT ibByteSize = indexCount * indexElementByteSize;

            const byte* indexData = useIndex32 ?
                reinterpret_cast<byte*>(compositeMesh.Indices32.data()) :
                reinterpret_cast<byte*>(compositeMesh.GetIndices16().data());

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
        DirectX::XMFLOAT4X4 MatTransform = MathHelper::Identity4x4();

        // Used in ray tracing demos only.
        float TransparencyWeight = 0.0f;
        float IndexOfRefraction = 0.0f;
    };

    
}
