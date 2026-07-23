export module shared:shadowmap;
import std;
import :win32;
import :descriptorutil;

export class ShadowMap
{
public:
    ShadowMap(D3D12::ID3D12Device* device, std::uint32_t width, std::uint32_t height)
    {
        md3dDevice = device;

        mWidth = width;
        mHeight = height;

        mViewport = { 0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height), 0.0f, 1.0f };
        mScissorRect = { 0, 0, static_cast<int>(width), static_cast<int>(height) };

        BuildResource();
    }

    ShadowMap(const ShadowMap& rhs) = delete;
    auto operator=(const ShadowMap& rhs) -> ShadowMap& = delete;

    auto Width() const -> std::uint32_t
    {
        return mWidth;
    }

    auto Height() const -> std::uint32_t
    {
        return mHeight;
    }

    auto Resource() -> D3D12::ID3D12Resource*
    {
        return mShadowMap.Get();
    }

    auto BindlessIndex() const -> std::uint32_t
    {
        return mBindlessIndex;
    }

    auto Srv() const -> D3D12::CD3DX12_GPU_DESCRIPTOR_HANDLE
    {
        return mhGpuSrv;
    }

    auto Dsv() const -> D3D12::CD3DX12_CPU_DESCRIPTOR_HANDLE
    {
        return mhCpuDsv;
    }

    auto Viewport() const -> D3D12::D3D12_VIEWPORT
    {
        return mViewport;
    }

    auto ScissorRect() const -> D3D12::D3D12_RECT
    {
        return mScissorRect;
    }

    auto BuildDescriptors(CD3DX12_CPU_DESCRIPTOR_HANDLE hCpuDsv) -> std::uint32_t
    {
        auto& bindlessHeap = CbvSrvUavHeap::Get();
        mBindlessIndex = bindlessHeap.NextFreeIndex();
        // Save references to the descriptors.
        mhCpuSrv = bindlessHeap.CpuHandle(mBindlessIndex);
        mhGpuSrv = bindlessHeap.GpuHandle(mBindlessIndex);
        mhCpuDsv = hCpuDsv;
        //  Create the descriptors
        BuildDescriptors();
        return mBindlessIndex;
    }

    void OnResize(std::uint32_t newWidth, std::uint32_t newHeight)
    {
        if ((mWidth != newWidth) or (mHeight != newHeight))
        {
            mWidth = newWidth;
            mHeight = newHeight;
            BuildResource();
            // New resource, so we need new descriptors to that resource.
            BuildDescriptors();
        }
    }

private:
    void BuildDescriptors()
    {
        // Create SRV to resource so we can sample the shadow map in a shader program.
        CreateSrv2d(md3dDevice, mShadowMap.Get(), DXGI::DXGI_FORMAT::DXGI_FORMAT_R24_UNORM_X8_TYPELESS, 1, mhCpuSrv);

        // Create DSV to resource so we can render to the shadow map.
        CreateDsv(md3dDevice, mShadowMap.Get(), D3D12::D3D12_DSV_FLAGS::D3D12_DSV_FLAG_NONE, DXGI::DXGI_FORMAT::DXGI_FORMAT_D24_UNORM_S8_UINT, 0, mhCpuDsv);
    }

    void BuildResource()
    {
        auto texDesc = D3D12::D3D12_RESOURCE_DESC{
            .Dimension = D3D12::D3D12_RESOURCE_DIMENSION::D3D12_RESOURCE_DIMENSION_TEXTURE2D,
            .Alignment = 0,
            .Width = mWidth,
            .Height = mHeight,
            .DepthOrArraySize = 1,
            .MipLevels = 1,
            .Format = mFormat,
            .SampleDesc = { .Count = 1, .Quality = 0 },
            .Layout = D3D12::D3D12_TEXTURE_LAYOUT::D3D12_TEXTURE_LAYOUT_UNKNOWN,
            .Flags = D3D12::D3D12_RESOURCE_FLAGS::D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL
        };
        
        auto optClear = D3D12::D3D12_CLEAR_VALUE{
            .Format = DXGI::DXGI_FORMAT::DXGI_FORMAT_D24_UNORM_S8_UINT,
            .DepthStencil = {
                .Depth = 1.0f,
                .Stencil = 0
            }
        };

        auto heapProperties = D3D12::CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);

        ThrowIfFailed(md3dDevice->CreateCommittedResource(
            &heapProperties,
            D3D12_HEAP_FLAG_NONE,
            &texDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ,
            &optClear,
            __uuidof(D3D12::ID3D12Resource),
            &mShadowMap));
    }

private:
    D3D12::ID3D12Device* md3dDevice = nullptr;

    D3D12::D3D12_VIEWPORT mViewport;
    D3D12::D3D12_RECT mScissorRect;

    std::uint32_t mWidth = 0;
    std::uint32_t mHeight = 0;
    DXGI::DXGI_FORMAT mFormat = DXGI_FORMAT_R24G8_TYPELESS;

    std::uint32_t mBindlessIndex = -1;
    D3D12::CD3DX12_CPU_DESCRIPTOR_HANDLE mhCpuSrv;
    D3D12::CD3DX12_GPU_DESCRIPTOR_HANDLE mhGpuSrv;
    D3D12::CD3DX12_CPU_DESCRIPTOR_HANDLE mhCpuDsv;

    Microsoft::WRL::ComPtr<D3D12::ID3D12Resource> mShadowMap;
};
