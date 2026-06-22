export module shared:d3dutil;
import std;
import :win32;
import :mathhelper;

export
{
    constexpr auto gNumFrameResources = 3;

    inline constexpr auto SsaoAmbientMapFormat = DXGI::DXGI_FORMAT::DXGI_FORMAT_R16_UNORM;
    inline constexpr auto SceneNormalMapFormat = DXGI::DXGI_FORMAT::DXGI_FORMAT_R16G16B16A16_FLOAT;

    inline auto AnsiToWString(const std::string& str) -> std::wstring
    {
        auto buffer = std::array<Win32::WCHAR, 512>{};
        Win32::MultiByteToWideChar(Win32::CpAcp, 0, str.c_str(), -1, buffer.data(), buffer.size());
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
            static auto utils = Microsoft::WRL::ComPtr<DXC::IDxcUtils>{};
            static auto compiler = Microsoft::WRL::ComPtr<DXC::IDxcCompiler3>{};
            static auto defaultIncludeHandler = Microsoft::WRL::ComPtr<DXC::IDxcIncludeHandler>{};

            if (!std::filesystem::exists(filename))
            {
                auto msg = std::format(L"{} not found.", filename);
                Win32::OutputDebugStringW(msg.c_str());
                Win32::MessageBoxW(0, msg.c_str(), 0, 0);
            }

            if (compiler == nullptr)
            {
                auto result = DXC::DxcCreateInstance(
                    DXC::CLSID_DxcUtils, __uuidof(DXC::IDxcUtils), &utils);
                ThrowIfFailed(result);
                result = DXC::DxcCreateInstance(
                    DXC::CLSID_DxcCompiler, __uuidof(DXC::IDxcCompiler3), &compiler );
                ThrowIfFailed(result);
				result = utils->CreateDefaultIncludeHandler(&defaultIncludeHandler); 
                ThrowIfFailed(result);
            }

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
    };

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
