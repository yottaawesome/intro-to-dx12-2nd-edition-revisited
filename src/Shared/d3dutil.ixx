export module shared:d3dutil;
import std;
import :win32;
import :mathhelper;

export
{
    constexpr auto gNumFrameResources = 3;

    inline constexpr auto SsaoAmbientMapFormat = DXGI::DXGI_FORMAT::DXGI_FORMAT_R16_UNORM;
    inline constexpr auto SceneNormalMapFormat = DXGI::DXGI_FORMAT::DXGI_FORMAT_R16G16B16A16_FLOAT;

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

    inline auto AnsiToWString(const std::string& str) -> std::wstring
    {
        auto buffer = std::array<Win32::WCHAR, 512>{};
        Win32::MultiByteToWideChar(Win32::CpAcp, 0, str.c_str(), -1, buffer.data(), buffer.size());
        return buffer.data();
    }

    //TODO: migrate
    class d3dUtil
    {
    public:
        void WriteBinaryToFile(DXC::IDxcBlob* blob, const std::wstring& filename)
        {
            auto fout = std::ofstream{ filename, std::ios::binary };
            fout.write((char*)blob->GetBufferPointer(), blob->GetBufferSize());
            fout.close();
        }

        auto IsKeyDown(int vkeyCode) -> bool
        {
            return (Win32::GetAsyncKeyState(vkeyCode) & 0x8000) != 0;
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

	inline void ThrowIfFailed(Win32::HRESULT hr)
	{
		if (Win32::Failed(hr))
            throw DxException(hr);
	}

	inline void ReleaseCom(Win32::IUnknown** obj)
	{
		if (obj && *obj)
		{
			(*obj)->Release();
			*obj = nullptr;
		}   
	}
}
