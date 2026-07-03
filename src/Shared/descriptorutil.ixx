//***************************************************************************************
// d3dApp.cpp by Frank Luna (C) 2015 All Rights Reserved.
//***************************************************************************************

export module shared:descriptorutil;
import std;
import :win32;
import :d3dutil;

export 
{
    class DescriptorHeap
    {
    public:
        DescriptorHeap() = default;
        DescriptorHeap(const DescriptorHeap& rhs) = delete;
        DescriptorHeap& operator=(const DescriptorHeap& rhs) = delete;

        void Init(
            D3D12::ID3D12Device* device,
            D3D12::D3D12_DESCRIPTOR_HEAP_TYPE type,
            Win32::UINT capacity
        )
        {
            auto heapDesc = D3D12_DESCRIPTOR_HEAP_DESC{
				.Type = type,
				.NumDescriptors = capacity,
				.Flags = type == D3D12::D3D12_DESCRIPTOR_HEAP_TYPE::D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV or type == D3D12::D3D12_DESCRIPTOR_HEAP_TYPE::D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER
				    ? D3D12::D3D12_DESCRIPTOR_HEAP_FLAGS::D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE
				    : D3D12::D3D12_DESCRIPTOR_HEAP_FLAGS::D3D12_DESCRIPTOR_HEAP_FLAG_NONE,
				.NodeMask = 0
            };
            ThrowIfFailed(device->CreateDescriptorHeap(
                &heapDesc,
                __uuidof(D3D12::ID3D12DescriptorHeap),
                &mHeap));

            mDescriptorSize = device->GetDescriptorHandleIncrementSize(type);
        }

        auto GetD3dHeap() const -> D3D12::ID3D12DescriptorHeap*
        {
            return mHeap.Get();
        }

        auto CpuHandle(Win32::UINT index) -> D3D12::CD3DX12_CPU_DESCRIPTOR_HANDLE
        {
            auto hcpu = D3D12::CD3DX12_CPU_DESCRIPTOR_HANDLE{ mHeap->GetCPUDescriptorHandleForHeapStart() };
            hcpu.Offset(index, mDescriptorSize);
            return hcpu;
        }
        auto GpuHandle(Win32::UINT index) -> D3D12::CD3DX12_GPU_DESCRIPTOR_HANDLE
        {
            auto hgpu = D3D12::CD3DX12_GPU_DESCRIPTOR_HANDLE{ mHeap->GetGPUDescriptorHandleForHeapStart() };
            hgpu.Offset(index, mDescriptorSize);
            return hgpu;
        }

    protected:
        Microsoft::WRL::ComPtr<D3D12::ID3D12DescriptorHeap> mHeap = nullptr;
        Win32::UINT mDescriptorSize = 0;
    };

    // For CbvSrvUav. When a resource is created, request a free bindless index. When a resource is destroyed, 
    // release the index so that it can be reused by another resource. The main idea is to somewhat automate 
    // getting CbvSrvUav descriptors. We do not care where the descriptor is in the heap so long as we have its
    // index, we can reference it in the shader.
    class CbvSrvUavHeap : public DescriptorHeap
    {
    public:
        CbvSrvUavHeap(const DescriptorHeap& rhs) = delete;
        CbvSrvUavHeap& operator=(const CbvSrvUavHeap& rhs) = delete;

        static auto Get() -> CbvSrvUavHeap&
        {
            static auto singleton = CbvSrvUavHeap{};
            return singleton;
        }

        auto IsInitialized() const -> bool
        {
            return mIsInitialized;
        }

        void Init(ID3D12Device* device, Win32::UINT capacity)
        {
            DescriptorHeap::Init(device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, capacity);

            for (auto i = 0u; i < capacity; ++i)
                mFreeIndices.push(i);

            mUsedIndices.clear();
            mIsInitialized = true;
        }

        auto NextFreeIndex() -> std::uint32_t
        {
            //assert(!mFreeIndices.empty());

            const auto index = mFreeIndices.front();

            mUsedIndices.insert(index);

            mFreeIndices.pop();

            return index;
        }
        void ReleaseIndex(std::uint32_t index)
        {
            // If a resource is destroyed, we can reuse its index.
            auto it = mUsedIndices.find(index);

            // Make sure we are releasing a used index.
            //assert(it != std::end(mUsedIndices));

            mUsedIndices.erase(it);
            mFreeIndices.push(index);
        }

    private:
        CbvSrvUavHeap() = default;

    private:
        bool mIsInitialized = false;
        std::queue<std::uint32_t> mFreeIndices;
        // Used for validation. Could put in debug builds only.
        std::unordered_set<std::uint32_t> mUsedIndices;
    };

    // Applications usually only need a handful of samplers.  So just define them all
    // up front in the sampler heap, and index them in shaders.
    class SamplerHeap : public DescriptorHeap
    {
    public:
        SamplerHeap(const SamplerHeap& rhs) = delete;
        SamplerHeap& operator=(const SamplerHeap& rhs) = delete;

        static auto Get() -> SamplerHeap&
        {
            static SamplerHeap singleton;
            return singleton;
        }

        auto IsInitialized() const -> bool
        {
            return mIsInitialized;
        }

        void Init(ID3D12Device* device)
        {
            if (mIsInitialized)
                return;

            // bump as needed
            constexpr auto capacity = 16u;

            DescriptorHeap::Init(device, D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, capacity);

            const D3D12_SAMPLER_DESC pointWrap = InitSamplerDesc(
                D3D12_FILTER_MIN_MAG_MIP_POINT,    // filter
                D3D12_TEXTURE_ADDRESS_MODE_WRAP,   // addressU
                D3D12_TEXTURE_ADDRESS_MODE_WRAP,   // addressV
                D3D12_TEXTURE_ADDRESS_MODE_WRAP);  // addressW

            const D3D12_SAMPLER_DESC pointClamp = InitSamplerDesc(
                D3D12_FILTER_MIN_MAG_MIP_POINT,    // filter
                D3D12_TEXTURE_ADDRESS_MODE_CLAMP,  // addressU
                D3D12_TEXTURE_ADDRESS_MODE_CLAMP,  // addressV
                D3D12_TEXTURE_ADDRESS_MODE_CLAMP); // addressW

            const D3D12_SAMPLER_DESC linearWrap = InitSamplerDesc(
                D3D12_FILTER_MIN_MAG_MIP_LINEAR,  // filter
                D3D12_TEXTURE_ADDRESS_MODE_WRAP,  // addressU
                D3D12_TEXTURE_ADDRESS_MODE_WRAP,  // addressV
                D3D12_TEXTURE_ADDRESS_MODE_WRAP); // addressW

            const D3D12_SAMPLER_DESC linearClamp = InitSamplerDesc(
                D3D12_FILTER_MIN_MAG_MIP_LINEAR,   // filter
                D3D12_TEXTURE_ADDRESS_MODE_CLAMP,  // addressU
                D3D12_TEXTURE_ADDRESS_MODE_CLAMP,  // addressV
                D3D12_TEXTURE_ADDRESS_MODE_CLAMP); // addressW

            const D3D12_SAMPLER_DESC anisotropicWrap = InitSamplerDesc(
                D3D12_FILTER_ANISOTROPIC,         // filter
                D3D12_TEXTURE_ADDRESS_MODE_WRAP,  // addressU
                D3D12_TEXTURE_ADDRESS_MODE_WRAP,  // addressV
                D3D12_TEXTURE_ADDRESS_MODE_WRAP,  // addressW
                0.0f,                             // mipLODBias
                8);                               // maxAnisotropy

            const D3D12_SAMPLER_DESC anisotropicClamp = InitSamplerDesc(
                D3D12_FILTER_ANISOTROPIC,          // filter
                D3D12_TEXTURE_ADDRESS_MODE_CLAMP,  // addressU
                D3D12_TEXTURE_ADDRESS_MODE_CLAMP,  // addressV
                D3D12_TEXTURE_ADDRESS_MODE_CLAMP,  // addressW
                0.0f,                              // mipLODBias
                8);                                // maxAnisotropy

            const D3D12_SAMPLER_DESC shadow = InitSamplerDesc(
                D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT, // filter
                D3D12_TEXTURE_ADDRESS_MODE_BORDER,                // addressU
                D3D12_TEXTURE_ADDRESS_MODE_BORDER,                // addressV
                D3D12_TEXTURE_ADDRESS_MODE_BORDER,                // addressW
                0.0f,                                             // mipLODBias
                16,                                               // maxAnisotropy
                D3D12_COMPARISON_FUNC_LESS_EQUAL,
                DirectX::XMFLOAT4(0.0f, 0.0f, 0.0f, 0.0f));

            auto samplers = std::array<D3D12_SAMPLER_DESC, 7>{
                pointWrap,
                pointClamp,
                linearWrap,
                linearClamp,
                anisotropicWrap,
                anisotropicClamp,
                shadow,
            };

            for (int i = 0; i < static_cast<int>(samplers.size()); ++i)
            {
                CD3DX12_CPU_DESCRIPTOR_HANDLE h = CpuHandle(i);
                device->CreateSampler(&samplers[i], h);
            }
        }

    private:
        SamplerHeap() = default;

        auto InitSamplerDesc(
            D3D12::D3D12_FILTER filter = D3D12::D3D12_FILTER::D3D12_FILTER_ANISOTROPIC,
            D3D12::D3D12_TEXTURE_ADDRESS_MODE addressU = D3D12::D3D12_TEXTURE_ADDRESS_MODE::D3D12_TEXTURE_ADDRESS_MODE_WRAP,
            D3D12::D3D12_TEXTURE_ADDRESS_MODE addressV = D3D12::D3D12_TEXTURE_ADDRESS_MODE::D3D12_TEXTURE_ADDRESS_MODE_WRAP,
            D3D12::D3D12_TEXTURE_ADDRESS_MODE addressW = D3D12::D3D12_TEXTURE_ADDRESS_MODE::D3D12_TEXTURE_ADDRESS_MODE_WRAP,
            Win32::FLOAT mipLODBias = 0,
            Win32::UINT maxAnisotropy = 16,
            D3D12::D3D12_COMPARISON_FUNC comparisonFunc = D3D12::D3D12_COMPARISON_FUNC::D3D12_COMPARISON_FUNC_NONE,
            const DirectX::XMFLOAT4& borderColor = DirectX::XMFLOAT4(0.0f, 0.0f, 0.0f, 0.0f),
            Win32::FLOAT minLOD = 0.f,
            Win32::FLOAT maxLOD = D3D12::D3d12Float32Max
        ) -> D3D12::D3D12_SAMPLER_DESC
        {
            return {
                .Filter = filter,
                .AddressU = addressU,
                .AddressV = addressV,
                .AddressW = addressW,
                .MipLODBias = mipLODBias,
                .MaxAnisotropy = maxAnisotropy,
                .ComparisonFunc = comparisonFunc,
                .BorderColor = { borderColor.x, borderColor.y, borderColor.z, borderColor.w },
                .MinLOD = minLOD,
                .MaxLOD = maxLOD
            };
        }
    private:

        bool mIsInitialized = false;
    };

    inline void CreateDsv(
        D3D12::ID3D12Device* device,
        D3D12::ID3D12Resource* resource,
        D3D12::D3D12_DSV_FLAGS flags,
        DXGI::DXGI_FORMAT format,
        Win32::UINT mipSlice,
        D3D12::CD3DX12_CPU_DESCRIPTOR_HANDLE hDescriptor
    )
    {
        D3D12::D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc;
        dsvDesc.Flags = flags;
        dsvDesc.ViewDimension = D3D12::D3D12_DSV_DIMENSION::D3D12_DSV_DIMENSION_TEXTURE2D;
        dsvDesc.Format = format;
        dsvDesc.Texture2D.MipSlice = mipSlice;
        device->CreateDepthStencilView(resource, &dsvDesc, hDescriptor);
    }

    inline void CreateSrv2d(
        ID3D12Device* device, 
        ID3D12Resource* resource, 
        DXGI_FORMAT format, 
        UINT mipLevels, 
        CD3DX12_CPU_DESCRIPTOR_HANDLE hDescriptor
    )
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Shader4ComponentMapping = D3D12::DefaultShader4ComponentMapping();
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MostDetailedMip = 0;
        srvDesc.Texture2D.ResourceMinLODClamp = 0.0f;
        srvDesc.Format = format;
        srvDesc.Texture2D.MipLevels = mipLevels;
        device->CreateShaderResourceView(resource, &srvDesc, hDescriptor);
    }

    inline void CreateSrv2dArray(
        ID3D12Device* device, 
        ID3D12Resource* resource, 
        DXGI_FORMAT format, 
        UINT mipLevels, 
        UINT arraySize, 
        CD3DX12_CPU_DESCRIPTOR_HANDLE hDescriptor
    )
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Shader4ComponentMapping = D3D12::DefaultShader4ComponentMapping();
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
        srvDesc.Texture2DArray.MostDetailedMip = 0;
        srvDesc.Texture2DArray.FirstArraySlice = 0;
        srvDesc.Texture2DArray.ArraySize = arraySize;
        srvDesc.Texture2DArray.ResourceMinLODClamp = 0.0f;
        srvDesc.Format = format;
        srvDesc.Texture2DArray.MipLevels = mipLevels;
        device->CreateShaderResourceView(resource, &srvDesc, hDescriptor);
    }

    inline void CreateSrvCube(
        ID3D12Device* device, 
        ID3D12Resource* resource, 
        DXGI_FORMAT format, 
        UINT mipLevels, 
        CD3DX12_CPU_DESCRIPTOR_HANDLE hDescriptor
    )
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Shader4ComponentMapping = D3D12::DefaultShader4ComponentMapping();
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
        srvDesc.TextureCube.MostDetailedMip = 0;
        srvDesc.TextureCube.ResourceMinLODClamp = 0.0f;
        srvDesc.Format = format;
        srvDesc.TextureCube.MipLevels = mipLevels;
        device->CreateShaderResourceView(resource, &srvDesc, hDescriptor);
    }

    inline void CreateRtv2d(ID3D12Device* device, ID3D12Resource* resource, DXGI_FORMAT format, UINT mipSlice, CD3DX12_CPU_DESCRIPTOR_HANDLE hDescriptor)
    {
        D3D12_RENDER_TARGET_VIEW_DESC rtvDesc = {};
        rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        rtvDesc.Format = format;
        rtvDesc.Texture2D.MipSlice = mipSlice;
        rtvDesc.Texture2D.PlaneSlice = 0;
        device->CreateRenderTargetView(resource, &rtvDesc, hDescriptor);
    }
    
    inline void CreateUav2d(
        ID3D12Device* device, 
        ID3D12Resource* resource, 
        DXGI_FORMAT format, 
        UINT mipSlice, 
        CD3DX12_CPU_DESCRIPTOR_HANDLE hDescriptor
    )
    {
        D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
        uavDesc.Format = format;
        uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        uavDesc.Texture2D.MipSlice = mipSlice;
        device->CreateUnorderedAccessView(resource, nullptr, &uavDesc, hDescriptor);
    }

    inline void CreateBufferUav(
        ID3D12Device* device, 
        UINT64 firstElement, 
        UINT elementCount,
        UINT elementByteSize, 
        UINT64 counterOffset,
        ID3D12Resource* resource, 
        ID3D12Resource* counterResource,
        CD3DX12_CPU_DESCRIPTOR_HANDLE hDescriptor
    )
    {
        D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc;
        uavDesc.Format = DXGI_FORMAT_UNKNOWN; // structured buffer
        uavDesc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        uavDesc.Buffer.FirstElement = firstElement;
        uavDesc.Buffer.NumElements = elementCount;
        uavDesc.Buffer.StructureByteStride = elementByteSize;
        uavDesc.Buffer.CounterOffsetInBytes = counterOffset;
        uavDesc.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_NONE;
        device->CreateUnorderedAccessView(resource, counterResource, &uavDesc, hDescriptor);
    }

    inline void CreateBufferSrv(
        ID3D12Device* device, 
        UINT64 firstElement, 
        UINT elementCount, 
        UINT elementByteSize,
        ID3D12Resource* resource, 
        CD3DX12_CPU_DESCRIPTOR_HANDLE hDescriptor
    )
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Format = DXGI_FORMAT_UNKNOWN; // structured buffer
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        srvDesc.Shader4ComponentMapping = D3D12::DefaultShader4ComponentMapping();
        srvDesc.Buffer.FirstElement = firstElement;
        srvDesc.Buffer.NumElements = elementCount;
        srvDesc.Buffer.StructureByteStride = elementByteSize;
        srvDesc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;
        device->CreateShaderResourceView(resource, &srvDesc, hDescriptor);
    }
}
