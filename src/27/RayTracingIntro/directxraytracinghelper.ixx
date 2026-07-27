//*********************************************************
//
// Copyright (c) Microsoft. All rights reserved.
// This code is licensed under the MIT License (MIT).
// THIS CODE IS PROVIDED *AS IS* WITHOUT WARRANTY OF
// ANY KIND, EITHER EXPRESS OR IMPLIED, INCLUDING ANY
// IMPLIED WARRANTIES OF FITNESS FOR A PARTICULAR
// PURPOSE, MERCHANTABILITY, OR NON-INFRINGEMENT.
//
//*********************************************************

export module raytracingintro:directxraytracinghelper;
import std;
import shared;

struct AccelerationStructureBuffers
{
    Microsoft::WRL::ComPtr<D3D12::ID3D12Resource> scratch;
    Microsoft::WRL::ComPtr<D3D12::ID3D12Resource> accelerationStructure;
    Microsoft::WRL::ComPtr<D3D12::ID3D12Resource> instanceDesc;    // Used only for top-level AS
    std::uint64_t                 ResultDataMaxSizeInBytes;
};

class GpuUploadBuffer
{
public:
    auto GetResource() -> Microsoft::WRL::ComPtr<D3D12::ID3D12Resource> 
    { 
        return m_resource; 
    }

protected:
    Microsoft::WRL::ComPtr<D3D12::ID3D12Resource> m_resource;

    ~GpuUploadBuffer()
    {
        if (m_resource.Get())
            m_resource->Unmap(0, nullptr);
    }

    void Allocate(D3D12::ID3D12Device* device, std::uint32_t bufferSize, Win32::LPCWSTR resourceName = nullptr)
    {
        auto uploadHeapProperties = D3D12::CD3DX12_HEAP_PROPERTIES(D3D12::D3D12_HEAP_TYPE::D3D12_HEAP_TYPE_UPLOAD);

        auto bufferDesc = D3D12::CD3DX12_RESOURCE_DESC::Buffer(bufferSize);
        ThrowIfFailed(device->CreateCommittedResource(
            &uploadHeapProperties,
            D3D12::D3D12_HEAP_FLAGS::D3D12_HEAP_FLAG_NONE,
            &bufferDesc,
            D3D12::D3D12_RESOURCE_STATES::D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr,
			__uuidof(D3D12::ID3D12Resource),
			&m_resource));
        m_resource->SetName(resourceName);
    }

    auto MapCpuWriteOnly() -> std::uint8_t*
    {
        auto mappedData = static_cast<std::uint8_t*>(nullptr);
        // We don't unmap this until the app closes. Keeping buffer mapped for the lifetime of the resource is okay.
        auto readRange = D3D12::CD3DX12_RANGE(0, 0);        // We do not intend to read from this resource on the CPU.
        ThrowIfFailed(m_resource->Map(0, &readRange, reinterpret_cast<void**>(&mappedData)));
        return mappedData;
    }
};

// Shader record = {{Shader ID}, {RootArguments}}
class ShaderRecord
{
public:
    ShaderRecord(void* pShaderIdentifier, std::uint32_t shaderIdentifierSize) :
        shaderIdentifier(pShaderIdentifier, shaderIdentifierSize)
    {}

    ShaderRecord(void* pShaderIdentifier, std::uint32_t shaderIdentifierSize, void* pLocalRootArguments, std::uint32_t localRootArgumentsSize) :
        shaderIdentifier(pShaderIdentifier, shaderIdentifierSize),
        localRootArguments(pLocalRootArguments, localRootArgumentsSize)
    {}

    void CopyTo(void* dest) const
    {
        auto byteDest = static_cast<std::uint8_t*>(dest);
        std::memcpy(byteDest, shaderIdentifier.ptr, shaderIdentifier.size);
        if (localRootArguments.ptr)
            std::memcpy(byteDest + shaderIdentifier.size, localRootArguments.ptr, localRootArguments.size);
    }

    struct PointerWithSize 
    {
        void* ptr = nullptr;
        std::uint32_t size = 0;
    };
    PointerWithSize shaderIdentifier;
    PointerWithSize localRootArguments;
};

// Shader table = {{ ShaderRecord 1}, {ShaderRecord 2}, ...}
class ShaderTable : public GpuUploadBuffer
{
private:
    std::uint8_t* m_mappedShaderRecords;
    std::uint32_t m_shaderRecordSize;

    // Debug support
    std::wstring m_name;
    std::vector<ShaderRecord> m_shaderRecords;

    ShaderTable() = default;

public:
    ShaderTable(D3D12::ID3D12Device* device, std::uint32_t numShaderRecords, std::uint32_t shaderRecordSize, LPCWSTR resourceName = nullptr)
        : m_name(resourceName)
    {
        constexpr auto Align = 
            [](std::uint32_t size, std::uint32_t alignment)
            {
                return (size + (alignment - 1)) & ~(alignment - 1);
            };

        m_shaderRecordSize = Align(shaderRecordSize, D3D12::RaytracingShaderRecordByteAlignment);
        m_shaderRecords.reserve(numShaderRecords);
        auto bufferSize = numShaderRecords * m_shaderRecordSize;
        Allocate(device, bufferSize, resourceName);
        m_mappedShaderRecords = MapCpuWriteOnly();
    }

    void push_back(const ShaderRecord& shaderRecord)
    {
        m_shaderRecords.push_back(shaderRecord);
        shaderRecord.CopyTo(m_mappedShaderRecords);
        m_mappedShaderRecords += m_shaderRecordSize;
    }

    auto GetShaderRecordSize() -> std::uint32_t
    { 
        return m_shaderRecordSize; 
    }

    // Pretty-print the shader records.
    void DebugPrint(std::unordered_map<void*, std::wstring> shaderIdToStringMap)
    {
        auto wstr = std::wostringstream{};
        wstr << L"|--------------------------------------------------------------------\n";
        wstr << L"|Shader table - " << m_name.c_str() << L": "
            << m_shaderRecordSize << L" | "
            << m_shaderRecords.size() * m_shaderRecordSize << L" bytes\n";

        for (auto i = 0u; i < m_shaderRecords.size(); i++)
        {
            wstr << L"| [" << i << L"]: ";
            wstr << shaderIdToStringMap[m_shaderRecords[i].shaderIdentifier.ptr] << L", ";
            wstr << m_shaderRecords[i].shaderIdentifier.size << L" + " << m_shaderRecords[i].localRootArguments.size << L" bytes \n";
        }
        wstr << L"|--------------------------------------------------------------------\n";
        wstr << L"\n";
        Win32::OutputDebugStringW(wstr.str().c_str());
    }
};

inline void AllocateUAVBuffer(D3D12::ID3D12Device* pDevice, std::uint64_t bufferSize, D3D12::ID3D12Resource** ppResource, D3D12_RESOURCE_STATES initialResourceState = D3D12_RESOURCE_STATE_COMMON, const wchar_t* resourceName = nullptr)
{
    auto uploadHeapProperties = D3D12::CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
    auto bufferDesc = D3D12::CD3DX12_RESOURCE_DESC::Buffer(bufferSize, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    ThrowIfFailed(pDevice->CreateCommittedResource(
        &uploadHeapProperties,
        D3D12::D3D12_HEAP_FLAGS::D3D12_HEAP_FLAG_NONE,
        &bufferDesc,
        initialResourceState,
        nullptr,
        __uuidof(D3D12::ID3D12Resource),
        reinterpret_cast<void**>(ppResource)));
    if (resourceName)
        (*ppResource)->SetName(resourceName);
}

template<class T, size_t N>
void DefineExports(T* obj, LPCWSTR(&Exports)[N])
{
    for (auto i = 0u; i < N; i++)
        obj->DefineExport(Exports[i]);
}

template<class T, size_t N, size_t M>
void DefineExports(T* obj, LPCWSTR(&Exports)[N][M])
{
    for (auto i = 0u; i < N; i++)
        for (auto j = 0u; j < M; j++)
            obj->DefineExport(Exports[i][j]);
}


inline void AllocateUploadBuffer(D3D12::ID3D12Device* pDevice, void* pData, std::uint64_t datasize, D3D12::ID3D12Resource** ppResource, const wchar_t* resourceName = nullptr)
{
    auto uploadHeapProperties = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
    auto bufferDesc = CD3DX12_RESOURCE_DESC::Buffer(datasize);
    ThrowIfFailed(pDevice->CreateCommittedResource(
        &uploadHeapProperties,
        D3D12_HEAP_FLAG_NONE,
        &bufferDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr,
        __uuidof(D3D12::ID3D12Resource),
        reinterpret_cast<void**>(ppResource)));
    if (resourceName)
    {
        (*ppResource)->SetName(resourceName);
    }
    void* pMappedData;
    (*ppResource)->Map(0, nullptr, &pMappedData);
    std::memcpy(pMappedData, pData, datasize);
    (*ppResource)->Unmap(0, nullptr);
}

// Pretty-print a state object tree.
inline void PrintStateObjectDesc(const D3D12::D3D12_STATE_OBJECT_DESC* desc)
{
    auto wstr = std::wostringstream{};
    wstr << L"\n";
    wstr << L"--------------------------------------------------------------------\n";
    wstr << L"| D3D12 State Object 0x" << static_cast<const void*>(desc) << L": ";
    if (desc->Type == D3D12::D3D12_STATE_OBJECT_TYPE::D3D12_STATE_OBJECT_TYPE_COLLECTION) 
        wstr << L"Collection\n";
    if (desc->Type == D3D12::D3D12_STATE_OBJECT_TYPE::D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE) 
        wstr << L"Raytracing Pipeline\n";

    constexpr auto ExportTree = 
        [](std::uint32_t depth, std::uint32_t numExports, const D3D12::D3D12_EXPORT_DESC* exports)
        {
            auto woss = std::wostringstream{};
            for (auto i = 0u; i < numExports; i++)
            {
                woss << L"|";
                if (depth > 0)
                    for (auto j = 0u; j < 2 * depth - 1; j++) 
                        woss << L" ";
                woss << L" [" << i << L"]: ";
                if (exports[i].ExportToRename) 
                    woss << exports[i].ExportToRename << L" --> ";
                woss << exports[i].Name << L"\n";
            }
            return woss.str();
        };

    for (auto i = 0u; i < desc->NumSubobjects; i++)
    {
        wstr << L"| [" << i << L"]: ";
        switch (desc->pSubobjects[i].Type)
        {
        case D3D12::D3D12_STATE_SUBOBJECT_TYPE::D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE:
            wstr << L"Global Root Signature 0x" << desc->pSubobjects[i].pDesc << L"\n";
            break;
        case D3D12::D3D12_STATE_SUBOBJECT_TYPE::D3D12_STATE_SUBOBJECT_TYPE_LOCAL_ROOT_SIGNATURE:
            wstr << L"Local Root Signature 0x" << desc->pSubobjects[i].pDesc << L"\n";
            break;
        case D3D12::D3D12_STATE_SUBOBJECT_TYPE::D3D12_STATE_SUBOBJECT_TYPE_NODE_MASK:
            wstr << L"Node Mask: 0x" << std::hex << std::setfill(L'0') << std::setw(8) << *static_cast<const UINT*>(desc->pSubobjects[i].pDesc) << std::setw(0) << std::dec << L"\n";
            break;
        case D3D12::D3D12_STATE_SUBOBJECT_TYPE::D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY:
        {
            wstr << L"DXIL Library 0x";
            auto lib = static_cast<const D3D12::D3D12_DXIL_LIBRARY_DESC*>(desc->pSubobjects[i].pDesc);
            wstr << lib->DXILLibrary.pShaderBytecode << L", " << lib->DXILLibrary.BytecodeLength << L" bytes\n";
            wstr << ExportTree(1, lib->NumExports, lib->pExports);
            break;
        }
        case D3D12::D3D12_STATE_SUBOBJECT_TYPE::D3D12_STATE_SUBOBJECT_TYPE_EXISTING_COLLECTION:
        {
            wstr << L"Existing Library 0x";
            auto collection = static_cast<const D3D12::D3D12_EXISTING_COLLECTION_DESC*>(desc->pSubobjects[i].pDesc);
            wstr << collection->pExistingCollection << L"\n";
            wstr << ExportTree(1, collection->NumExports, collection->pExports);
            break;
        }
        case D3D12::D3D12_STATE_SUBOBJECT_TYPE::D3D12_STATE_SUBOBJECT_TYPE_SUBOBJECT_TO_EXPORTS_ASSOCIATION:
        {
            wstr << L"Subobject to Exports Association (Subobject [";
            auto association = static_cast<const D3D12::D3D12_SUBOBJECT_TO_EXPORTS_ASSOCIATION*>(desc->pSubobjects[i].pDesc);
            std::uint32_t index = static_cast<std::uint32_t>(association->pSubobjectToAssociate - desc->pSubobjects);
            wstr << index << L"])\n";
            for (std::uint32_t j = 0; j < association->NumExports; j++)
            {
                wstr << L"|  [" << j << L"]: " << association->pExports[j] << L"\n";
            }
            break;
        }
        case D3D12::D3D12_STATE_SUBOBJECT_TYPE::D3D12_STATE_SUBOBJECT_TYPE_DXIL_SUBOBJECT_TO_EXPORTS_ASSOCIATION:
        {
            wstr << L"DXIL Subobjects to Exports Association (";
            auto association = static_cast<const D3D12::D3D12_DXIL_SUBOBJECT_TO_EXPORTS_ASSOCIATION*>(desc->pSubobjects[i].pDesc);
            wstr << association->SubobjectToAssociate << L")\n";
            for (std::uint32_t j = 0; j < association->NumExports; j++)
            {
                wstr << L"|  [" << j << L"]: " << association->pExports[j] << L"\n";
            }
            break;
        }
        case D3D12::D3D12_STATE_SUBOBJECT_TYPE::D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG:
        {
            wstr << L"Raytracing Shader Config\n";
            auto config = static_cast<const D3D12::D3D12_RAYTRACING_SHADER_CONFIG*>(desc->pSubobjects[i].pDesc);
            wstr << L"|  [0]: Max Payload Size: " << config->MaxPayloadSizeInBytes << L" bytes\n";
            wstr << L"|  [1]: Max Attribute Size: " << config->MaxAttributeSizeInBytes << L" bytes\n";
            break;
        }
        case D3D12::D3D12_STATE_SUBOBJECT_TYPE::D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG:
        {
            wstr << L"Raytracing Pipeline Config\n";
            auto config = static_cast<const D3D12::D3D12_RAYTRACING_PIPELINE_CONFIG*>(desc->pSubobjects[i].pDesc);
            wstr << L"|  [0]: Max Recursion Depth: " << config->MaxTraceRecursionDepth << L"\n";
            break;
        }
        case D3D12::D3D12_STATE_SUBOBJECT_TYPE::D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP:
        {
            wstr << L"Hit Group (";
            auto hitGroup = static_cast<const D3D12::D3D12_HIT_GROUP_DESC*>(desc->pSubobjects[i].pDesc);
            wstr << (hitGroup->HitGroupExport ? hitGroup->HitGroupExport : L"[none]") << L")\n";
            wstr << L"|  [0]: Any Hit Import: " << (hitGroup->AnyHitShaderImport ? hitGroup->AnyHitShaderImport : L"[none]") << L"\n";
            wstr << L"|  [1]: Closest Hit Import: " << (hitGroup->ClosestHitShaderImport ? hitGroup->ClosestHitShaderImport : L"[none]") << L"\n";
            wstr << L"|  [2]: Intersection Import: " << (hitGroup->IntersectionShaderImport ? hitGroup->IntersectionShaderImport : L"[none]") << L"\n";
            break;
        }
        }
        wstr << L"|--------------------------------------------------------------------\n";
    }
    wstr << L"\n";
    Win32::OutputDebugStringW(wstr.str().c_str());
}

// Returns bool whether the device supports DirectX Raytracing tier.
inline bool IsDirectXRaytracingSupported(DXGI::IDXGIAdapter1* adapter)
{
    Microsoft::WRL::ComPtr<D3D12::ID3D12Device> testDevice;
    D3D12::D3D12_FEATURE_DATA_D3D12_OPTIONS5 featureSupportData = {};

    return Win32::Succeeded(D3D12::D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_11_0, __uuidof(D3D12::ID3D12Device), &testDevice))
        && Win32::Succeeded(testDevice->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &featureSupportData, sizeof(featureSupportData)))
        && featureSupportData.RaytracingTier != D3D12_RAYTRACING_TIER_NOT_SUPPORTED;
}