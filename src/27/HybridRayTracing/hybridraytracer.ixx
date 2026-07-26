export module hybridraytracing:hybridraytracer;
import std;
import shared;
import :frameresource;
import :directxraytracinghelper;

struct LocalRootArguments
{
	std::uint32_t MaterialIndex;
	std::uint32_t VertexBufferBindlessIndex;
	std::uint32_t VertexBufferOffset;
	std::uint32_t IndexBufferBindlessIndex;
	std::uint32_t IndexBufferOffset;
	DirectX::XMFLOAT2 TexScale;
};

constexpr auto NumLocalRootConstants = std::uint32_t{sizeof(LocalRootArguments) / sizeof(std::uint32_t)};

class HybridRayTracer
{
	struct RTInstance
	{
		std::string ModelName;
		DirectX::XMFLOAT4X4 Transform;
		std::uint32_t MaterialIndex;
		DirectX::XMFLOAT2 TexScale;
	};

public:
	static constexpr auto ReflectionMapFormat = DXGI::DXGI_FORMAT::DXGI_FORMAT_R8G8B8A8_UNORM;

	// In our demo, we have one geometry per model. However, a model could be built from multiple geometries.
	// For example, a car model could be made from different geometries for the wheels, body, seats, 
	// windows, etc. 
	struct RTModelDef
	{
		D3D12::ID3D12Resource* VertexBuffer = nullptr;
		D3D12::ID3D12Resource* IndexBuffer = nullptr;
		std::uint32_t VertexBufferBindlessIndex = -1;
		std::uint32_t IndexBufferBindlessIndex = -1;
		DXGI::DXGI_FORMAT IndexFormat = DXGI_FORMAT_R16_UINT;
		DXGI::DXGI_FORMAT VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
		std::uint32_t IndexCount = 0;
		std::uint32_t VertexCount = 0;
		std::uint32_t StartIndexLocation = 0;
		std::uint32_t BaseVertexLocation = 0;
		std::uint32_t VertexSizeInBytes = 0;
		std::uint32_t IndexSizeInBytes = 2;
	};

	static constexpr auto HitGroupName = L"HitGroup0";
	static constexpr auto RaygenShaderName = L"RaygenShader";
	static constexpr auto ClosestHitShaderName = L"ClosestHit";
	static constexpr auto ColorMissShaderName = L"Color_MissShader";
	static constexpr auto ShadowMissShaderName = L"Shadow_MissShader";

	HybridRayTracer(
		D3D12::ID3D12Device5* device,
		D3D12::ID3D12GraphicsCommandList6* cmdList,
		DXC::IDxcBlob* rayTraceLibByteCode,
		std::uint32_t width, 
		std::uint32_t height
	) : mdxrDevice(device), mdxrCmdList(cmdList)
	{
		mShaderLib = d3dUtil::ByteCodeFromBlob(rayTraceLibByteCode);

		BuildGlobalRootSignature();
		BuildLocalRootSignature();
		BuildRayTraceStateObject();

		OnResize(width, height);
	}

	HybridRayTracer(const HybridRayTracer& rhs) = delete;
	HybridRayTracer& operator=(const HybridRayTracer& rhs) = delete;
	~HybridRayTracer() = default;

	auto GetReflectionMap()const -> D3D12::ID3D12Resource*
	{
		return mReflectionMap.Get();
	}

	auto GetReflectionMapUavIndex()const -> std::uint32_t
	{
		return mReflectionMapUavIndex;
	}

	auto GetReflectionMapSrvIndex()const -> std::uint32_t
	{
		return mReflectionMapSrvIndex;
	}

	void OnResize(std::uint32_t newWidth, std::uint32_t newHeight)
	{
		if ((mWidth != newWidth) or (mHeight != newHeight))
		{
			mWidth = newWidth;
			mHeight = newHeight;

			BuildOutputTextures();

			// New resource, so we need new descriptors to that resource.
			BuildDescriptors();
		}
	}

	void AddModel(const std::string& modelName, const RTModelDef& modelDef)
	{
		// Each model corresponds to a BLAS
		auto it = mModels.find(modelName);
		if (it == std::end(mModels))
		{
			mModels[modelName] = modelDef;
		}
	}

	void AddInstance(
		const std::string& modelName,
		const DirectX::XMFLOAT4X4& worldTransform,
		DirectX::XMFLOAT2 texScale,
		std::uint32_t materialIndex
	)
	{
		auto inst = RTInstance{
			.ModelName = modelName,
			.Transform = worldTransform,
			.MaterialIndex = materialIndex,
			.TexScale = texScale,
		};
		mInstances.push_back(inst);
	}

	// Cannot add anymore geometries once we start building.
	void ExecuteBuildAccelerationStructureCommands(D3D12::ID3D12CommandQueue* commandQueue)
	{
		BuildShaderBindingTables();

		std::unordered_map<std::string, AccelerationStructureBuffers> modelBlases = BuildBlases();
		for (const auto& [name, blas] : modelBlases)
		{
			auto uav = D3D12::CD3DX12_RESOURCE_BARRIER::UAV(blas.accelerationStructure.Get());
			mdxrCmdList->ResourceBarrier(1, &uav);
		}

		AccelerationStructureBuffers tlas = BuildTlas(modelBlases);

		// Build acceleration structures on GPU and wait until it is done.
		ThrowIfFailed(mdxrCmdList->Close());
		D3D12::ID3D12CommandList* commandLists[] = { mdxrCmdList };
		commandQueue->ExecuteCommandLists(std::size(commandLists), commandLists);

		// Need to finish building on GPU before AccelerationStructureBuffers goes out of scope.
		D3DApp::GetApp()->FlushCommandQueue();

		// Building uses intermediate resources, but we only need to save the final results for rendering.
		for (const auto& [name, blas] : modelBlases)
		{
			mModelBlases[name] = blas.accelerationStructure;
		}
		mSceneTlas = tlas.accelerationStructure;
	}

	void Draw(D3D12::ID3D12Resource* passCB, D3D12::ID3D12Resource* matBuffer)
	{
		mdxrCmdList->SetComputeRootSignature(mGlobalRootSig.Get());
		mdxrCmdList->SetComputeRootConstantBufferView(RT_ROOT_ARG_PASS_CBV, passCB->GetGPUVirtualAddress());
		mdxrCmdList->SetComputeRootShaderResourceView(RT_ROOT_ARG_MATERIAL_SRV, matBuffer->GetGPUVirtualAddress());
		mdxrCmdList->SetComputeRootShaderResourceView(RT_ROOT_ARG_ACCELERATION_STRUCT_SRV, mSceneTlas->GetGPUVirtualAddress());

		// Specify dimensions and SBT spans.
		D3D12::D3D12_DISPATCH_RAYS_DESC dispatchDesc = {};
		dispatchDesc.HitGroupTable.StartAddress = mHitGroupShaderTable->GetGPUVirtualAddress();
		dispatchDesc.HitGroupTable.SizeInBytes = mHitGroupShaderTable->GetDesc().Width;
		dispatchDesc.HitGroupTable.StrideInBytes = mHitGroupShaderTableStrideInBytes;
		dispatchDesc.MissShaderTable.StartAddress = mMissShaderTable->GetGPUVirtualAddress();
		dispatchDesc.MissShaderTable.SizeInBytes = mMissShaderTable->GetDesc().Width;
		dispatchDesc.MissShaderTable.StrideInBytes = mMissShaderTableStrideInBytes;
		dispatchDesc.RayGenerationShaderRecord.StartAddress = mRayGenShaderTable->GetGPUVirtualAddress();
		dispatchDesc.RayGenerationShaderRecord.SizeInBytes = mRayGenShaderTable->GetDesc().Width;
		dispatchDesc.Width = mWidth;
		dispatchDesc.Height = mHeight;
		dispatchDesc.Depth = 1;

		mdxrCmdList->SetPipelineState1(mdxrStateObject.Get());
		mdxrCmdList->DispatchRays(&dispatchDesc);
	}

private:
	void BuildOutputTextures()
	{
		D3D12::D3D12_RESOURCE_DESC texDesc{};
		texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
		texDesc.Alignment = 0;
		texDesc.Width = mWidth;
		texDesc.Height = mHeight;
		texDesc.DepthOrArraySize = 1;
		texDesc.MipLevels = 1;
		texDesc.Format = ReflectionMapFormat;
		texDesc.SampleDesc.Count = 1;
		texDesc.SampleDesc.Quality = 0;
		texDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
		texDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

		auto heapProps = D3D12::CD3DX12_HEAP_PROPERTIES(D3D12::D3D12_HEAP_TYPE::D3D12_HEAP_TYPE_DEFAULT);
		ThrowIfFailed(mdxrDevice->CreateCommittedResource(
			&heapProps,
			D3D12_HEAP_FLAG_NONE,
			&texDesc,
			D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
			nullptr,
			__uuidof(D3D12::ID3D12Resource),
			&mReflectionMap));
	}

	void BuildGlobalRootSignature()
	{
		//
		// Define shader parameters global to all ray-trace shaders.
		//

		D3D12::CD3DX12_ROOT_PARAMETER rayTraceRootParameters[RT_ROOT_ARG_COUNT];

		rayTraceRootParameters[RT_ROOT_ARG_PASS_CBV].InitAsConstantBufferView(1);
		rayTraceRootParameters[RT_ROOT_ARG_MATERIAL_SRV].InitAsShaderResourceView(0);
		rayTraceRootParameters[RT_ROOT_ARG_ACCELERATION_STRUCT_SRV].InitAsShaderResourceView(1);

		D3D12::CD3DX12_ROOT_SIGNATURE_DESC rtGlobalRootSigDesc{
			RT_ROOT_ARG_COUNT,
			rayTraceRootParameters,
			0,
			nullptr, // static samplers
			D3D12_ROOT_SIGNATURE_FLAGS{
				D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED |
				D3D12_ROOT_SIGNATURE_FLAG_SAMPLER_HEAP_DIRECTLY_INDEXED
			}
		};

		Microsoft::WRL::ComPtr<D3D::ID3DBlob> serializedRootSig = nullptr;
		Microsoft::WRL::ComPtr<D3D::ID3DBlob> errorBlob = nullptr;
		auto hr = D3D12::D3D12SerializeRootSignature(
			&rtGlobalRootSigDesc, D3D_ROOT_SIGNATURE_VERSION_1,
			serializedRootSig.GetAddressOf(), errorBlob.GetAddressOf());

		if (errorBlob != nullptr)
		{
			Win32::OutputDebugStringA((char*)errorBlob->GetBufferPointer());
		}
		ThrowIfFailed(hr);

		ThrowIfFailed(mdxrDevice->CreateRootSignature(0,
			serializedRootSig->GetBufferPointer(),
			serializedRootSig->GetBufferSize(),
			__uuidof(D3D12::ID3D12RootSignature),
			reinterpret_cast<void**>(mGlobalRootSig.GetAddressOf())));
	}

	void BuildLocalRootSignature()
	{
		//
		// Define additional "local" shader parameters whose arguments vary per shader table entry.
		// In particular, this is how we pass per-object arguments. The data would be similar to a
		// PerObjectCB, except that the world transform is not needed because it is baked into the 
		// acceleration structure already.
		// 

		const UINT numRootParams = 1;
		const UINT num32BitValues = NumLocalRootConstants; // see LocalRootArguments
		const UINT shaderRegister = 0;
		D3D12::CD3DX12_ROOT_PARAMETER rayTraceRootParameters[1];
		rayTraceRootParameters[0].InitAsConstants(num32BitValues, shaderRegister);

		D3D12::CD3DX12_ROOT_SIGNATURE_DESC rtLocalRootSigDesc(numRootParams, rayTraceRootParameters);
		rtLocalRootSigDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_LOCAL_ROOT_SIGNATURE;

		Microsoft::WRL::ComPtr<D3D::ID3DBlob> serializedRootSig = nullptr;
		Microsoft::WRL::ComPtr<D3D::ID3DBlob> errorBlob = nullptr;
		auto hr = D3D12::D3D12SerializeRootSignature(
			&rtLocalRootSigDesc, D3D_ROOT_SIGNATURE_VERSION_1,
			serializedRootSig.GetAddressOf(), errorBlob.GetAddressOf());

		if (errorBlob != nullptr)
		{
			Win32::OutputDebugStringA((char*)errorBlob->GetBufferPointer());
		}
		ThrowIfFailed(hr);

		ThrowIfFailed(mdxrDevice->CreateRootSignature(0,
			serializedRootSig->GetBufferPointer(),
			serializedRootSig->GetBufferSize(),
			__uuidof(D3D12::ID3D12RootSignature),
			reinterpret_cast<void**>(mLocalRootSig.GetAddressOf())));
	}

	void BuildRayTraceStateObject()
	{
		//
		// A bit of boilerplate needed to configure the ray tracing pipeline.
		//

		D3D12::CD3DX12_STATE_OBJECT_DESC raytracingPipeline{ D3D12::D3D12_STATE_OBJECT_TYPE::D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE };

		//
		// Set the compiled DXIL library code that contains our ray tracing shaders and define which shaders
		// to export from the library. If we omit explicit exports, all will be exported.
		//
		auto lib = raytracingPipeline.CreateSubobject<D3D12::CD3DX12_DXIL_LIBRARY_SUBOBJECT>();
		lib->SetDXILLibrary(&mShaderLib);
		lib->DefineExport(RaygenShaderName);
		lib->DefineExport(ClosestHitShaderName);
		lib->DefineExport(ColorMissShaderName);
		lib->DefineExport(ShadowMissShaderName);

		//
		// Define a hit group, which basically specifies the shaders involved with ray hits.
		// 
		// SetHitGroupExport: Give the hit group a name so we can refer to it by name in other parts of the DXR API.
		// SetHitGroupType: Either D3D12_HIT_GROUP_TYPE_PROCEDURAL_PRIMITIVE or D3D12_HIT_GROUP_TYPE_TRIANGLES. 
		//                  The API has some automatic functionality for triangles. For example, there is a 
		//                  built-in ray-triangle intersection.
		// SetClosestHitShaderImport: Sets the closest hit shader for this hit group.
		// SetAnyHitShaderImport: Sets the any hit shader for this hit group.
		// SetIntersectionShaderImport: Sets the intersection shader for this hit group.
		// 
		// Note that if your ray tracing program does not use one of the hit shader types, then it does not need to set it.
		// 
		auto hitGroup = raytracingPipeline.CreateSubobject<D3D12::CD3DX12_HIT_GROUP_SUBOBJECT>();
		hitGroup->SetHitGroupExport(HitGroupName);
		hitGroup->SetHitGroupType(D3D12::D3D12_HIT_GROUP_TYPE::D3D12_HIT_GROUP_TYPE_TRIANGLES);
		hitGroup->SetClosestHitShaderImport(ClosestHitShaderName);
		// hitGroup->SetAnyHitShaderImport(); Not used

		// 
		// Define the size of the payload and attribute structures. 
		// This is application defined and the smaller the better for performance.
		// 
		auto shaderConfig = raytracingPipeline.CreateSubobject<D3D12::CD3DX12_RAYTRACING_SHADER_CONFIG_SUBOBJECT>();
		auto payloadSize = static_cast<std::uint32_t>(std::max(sizeof(ColorRayPayload), sizeof(ShadowRayPayload)));
		auto attributeSize = static_cast<std::uint32_t>(sizeof(GeoAttributes));
		shaderConfig->Config(payloadSize, attributeSize);

		//
		// Set local root signature, and associate it with hit group.
		//
		auto localRootSignature = raytracingPipeline.CreateSubobject<D3D12::CD3DX12_LOCAL_ROOT_SIGNATURE_SUBOBJECT>();
		localRootSignature->SetRootSignature(mLocalRootSig.Get());

		auto rootSignatureAssociation = raytracingPipeline.CreateSubobject<D3D12::CD3DX12_SUBOBJECT_TO_EXPORTS_ASSOCIATION_SUBOBJECT>();
		rootSignatureAssociation->SetSubobjectToAssociate(*localRootSignature);
		rootSignatureAssociation->AddExport(HitGroupName);

		//
		// Set the global root signature.
		//
		auto globalRootSignature = raytracingPipeline.CreateSubobject<D3D12::CD3DX12_GLOBAL_ROOT_SIGNATURE_SUBOBJECT>();
		globalRootSignature->SetRootSignature(mGlobalRootSig.Get());

		//
		// Set max recursion depth. For internal driver optimizations, specify the lowest number you need.
		//
		auto pipelineConfig = raytracingPipeline.CreateSubobject<D3D12::CD3DX12_RAYTRACING_PIPELINE_CONFIG_SUBOBJECT>();
		const UINT maxRecursionDepth = MaxRecursionDepth;
		pipelineConfig->Config(maxRecursionDepth);

		ThrowIfFailed(mdxrDevice->CreateStateObject(raytracingPipeline, __uuidof(D3D12::ID3D12StateObject), &mdxrStateObject));
	}

	using ModelBlasList = std::unordered_map<std::string, AccelerationStructureBuffers>;

	auto BuildBlases() -> ModelBlasList
	{
		constexpr std::uint32_t numGeometries = 1;

		D3D12::D3D12_RAYTRACING_GEOMETRY_DESC geoDesc[numGeometries];

		std::unordered_map<std::string, AccelerationStructureBuffers> blasBuffers(mModels.size());

		for (const auto& [name, modelDef] : mModels)
		{
			const int indexByteOffset = modelDef.StartIndexLocation * modelDef.IndexSizeInBytes;
			const int vertexByteOffset = modelDef.BaseVertexLocation * modelDef.VertexSizeInBytes;

			geoDesc[0].Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
			geoDesc[0].Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
			geoDesc[0].Triangles.IndexBuffer = modelDef.IndexBuffer->GetGPUVirtualAddress() + indexByteOffset;
			geoDesc[0].Triangles.IndexCount = modelDef.IndexCount;
			geoDesc[0].Triangles.IndexFormat = modelDef.IndexFormat;
			geoDesc[0].Triangles.VertexFormat = modelDef.VertexFormat;
			geoDesc[0].Triangles.VertexCount = modelDef.VertexCount;
			geoDesc[0].Triangles.VertexBuffer.StartAddress = modelDef.VertexBuffer->GetGPUVirtualAddress() + vertexByteOffset;
			geoDesc[0].Triangles.VertexBuffer.StrideInBytes = modelDef.VertexSizeInBytes;

			// This transform (if used) is used to transform the geometry relative to its model.
			// A separate instance/world transform will be applied on top of that to transform the entire model instance.
			geoDesc[0].Triangles.Transform3x4 = 0;

			//
			// BLAS is built from N geometries.
			//
			D3D12::D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC blasDesc = {};
			blasDesc.Inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
			blasDesc.Inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
			blasDesc.Inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
			blasDesc.Inputs.NumDescs = numGeometries;
			blasDesc.Inputs.pGeometryDescs = geoDesc;

			// Query some info that is device dependent for building the BLAS.
			D3D12::D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO prebuildInfo = {};
			mdxrDevice->GetRaytracingAccelerationStructurePrebuildInfo(&blasDesc.Inputs, &prebuildInfo);
			//assert(prebuildInfo.ResultDataMaxSizeInBytes > 0);

			Microsoft::WRL::ComPtr<D3D12::ID3D12Resource> scratch;
			AllocateUAVBuffer(mdxrDevice,
				prebuildInfo.ScratchDataSizeInBytes,
				&scratch,
				D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
				L"ScratchResource");

			Microsoft::WRL::ComPtr<D3D12::ID3D12Resource> blas;
			AllocateUAVBuffer(mdxrDevice, prebuildInfo.ResultDataMaxSizeInBytes,
				&blas,
				D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE,
				L"BottomLevelAccelerationStructure");

			blasDesc.ScratchAccelerationStructureData = scratch->GetGPUVirtualAddress();
			blasDesc.DestAccelerationStructureData = blas->GetGPUVirtualAddress();

			mdxrCmdList->BuildRaytracingAccelerationStructure(&blasDesc, 0, nullptr);

			AccelerationStructureBuffers bottomLevelASBuffers;
			bottomLevelASBuffers.accelerationStructure = blas;
			bottomLevelASBuffers.scratch = scratch;
			bottomLevelASBuffers.ResultDataMaxSizeInBytes = prebuildInfo.ResultDataMaxSizeInBytes;

			blasBuffers[name] = bottomLevelASBuffers;
		}

		return blasBuffers;
	}

	auto BuildInstanceBuffer(ModelBlasList& modelBlases) -> Microsoft::WRL::ComPtr<D3D12::ID3D12Resource>
	{
		std::vector<D3D12::D3D12_RAYTRACING_INSTANCE_DESC> instanceDescs;
		instanceDescs.resize(mInstances.size());

		for (uint32_t i = 0; i < mInstances.size(); ++i)
		{
			AccelerationStructureBuffers& blas = modelBlases[mInstances[i].ModelName];
			D3D12::D3D12_GPU_VIRTUAL_ADDRESS blasGpuAddress = blas.accelerationStructure->GetGPUVirtualAddress();

			instanceDescs[i].InstanceMask = 1;
			instanceDescs[i].InstanceContributionToHitGroupIndex = i * RayCount * NumGeometriesPerInstance; // instance offset for SBT
			instanceDescs[i].AccelerationStructure = blasGpuAddress;
			instanceDescs[i].InstanceID = i; // for shader SV_InstanceID
			instanceDescs[i].Flags = D3D12::D3D12_RAYTRACING_INSTANCE_FLAGS::D3D12_RAYTRACING_INSTANCE_FLAG_NONE;

			DirectX::XMMATRIX worldTransform = DirectX::XMLoadFloat4x4(&mInstances[i].Transform);
			DirectX::XMStoreFloat3x4(reinterpret_cast<DirectX::XMFLOAT3X4*>(instanceDescs[i].Transform), worldTransform);
		}

		std::uint64_t bufferSize = static_cast<std::uint64_t>(instanceDescs.size() * sizeof(D3D12::D3D12_RAYTRACING_INSTANCE_DESC));
		Microsoft::WRL::ComPtr<D3D12::ID3D12Resource> instanceBuffer;
		AllocateUploadBuffer(mdxrDevice, instanceDescs.data(), bufferSize, &instanceBuffer, L"InstanceDescs");

		return instanceBuffer;
	}

	auto BuildTlas(ModelBlasList& modelBlases) -> AccelerationStructureBuffers
	{
		// TLAS defines instances of BLAS structures. 

		D3D12::D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC tlasDesc = {};
		tlasDesc.Inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
		tlasDesc.Inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
		tlasDesc.Inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
		tlasDesc.Inputs.NumDescs = (UINT)mInstances.size();

		D3D12::D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO prebuildInfo = {};
		mdxrDevice->GetRaytracingAccelerationStructurePrebuildInfo(&tlasDesc.Inputs, &prebuildInfo);
		//assert(prebuildInfo.ResultDataMaxSizeInBytes > 0);

		Microsoft::WRL::ComPtr<D3D12::ID3D12Resource> scratch;
		AllocateUAVBuffer(mdxrDevice,
			prebuildInfo.ScratchDataSizeInBytes,
			&scratch,
			D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
			L"ScratchResource");

		Microsoft::WRL::ComPtr<D3D12::ID3D12Resource> tlas;
		AllocateUAVBuffer(mdxrDevice,
			prebuildInfo.ResultDataMaxSizeInBytes,
			&tlas,
			D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE,
			L"TopLevelAccelerationStructure");

		Microsoft::WRL::ComPtr<D3D12::ID3D12Resource> instanceBuffer = BuildInstanceBuffer(modelBlases);
		tlasDesc.Inputs.InstanceDescs = instanceBuffer->GetGPUVirtualAddress();

		tlasDesc.DestAccelerationStructureData = tlas->GetGPUVirtualAddress();
		tlasDesc.ScratchAccelerationStructureData = scratch->GetGPUVirtualAddress();

		mdxrCmdList->BuildRaytracingAccelerationStructure(&tlasDesc, 0, nullptr);

		AccelerationStructureBuffers tlasBuffers;
		tlasBuffers.accelerationStructure = tlas;
		tlasBuffers.instanceDesc = instanceBuffer;
		tlasBuffers.scratch = scratch;
		tlasBuffers.ResultDataMaxSizeInBytes = prebuildInfo.ResultDataMaxSizeInBytes;

		return tlasBuffers;
	}

	// With rasterization, we draw meshes one-by-one. We can draw the meshes with different shaders by 
// binding a different PSO like this:
//
// SetTerrainPSO();
// DrawTerrainMesh();
//
// SetStaticOpaqueMeshPSO();
// DrawStaticOpaqueMeshes();
// 
// SetParticlesPSO();
// DrawParticles();
//
// Ray tracing is more complicated because when view rays are generated, we do not know 
// which geometries they will intersect. So the ray tracing system needs to know about all 
// possible shaders that might need to be run per ray dispatch. This is specified by the shader binding table (SBT). 
//
// The SBT is just a buffer of shader records we fill out. There is an implicit agreement that it is filled out 
// correctly such that it matches how the scene and ray tracing pipeline is configured. 
// More specifically, we need to map each geometry in each instance in the scene to an entry in the shader table.
// Furthermore, we might have multiple types of rays being casted (e.g., primary and shadow rays). Thus each
// geometry will have an entry for each type of ray.
//  
// From [GPU Gems 2] the general formula is:
// 
// HG_index = I_offset + R_offset + R_stride * G_id
// HG_byteOffset = HG_stride * HG_index
//   
// where
//   
//   I_offset: Index to the starting record in the shader table for the instance.
//   R_offset: ray index from [0, RayTypeCount).
//   G_id: instance geometry index from [0, GeometryCount(instanceId)).
//   R_stride: The ray type count.
//   HG_stride: byte size between shader records
// 
// To understand the formula, it is a bit easier to start with a common example and then modify it as needed. 
// Suppose we have 3 instances, where instance 1 has 1 geometry, instances 2 and 3 and have 2 geometries, and 
// suppose we are casting two types of rays: primary and shadow. Then R_stride = 2 and R_offset in {0, 1} and 
// our shader table looks like this:
// 
// ShaderRecord shaderTable[NUM_ENTRIES];
// 
// instanceOffset0 = 0;
// ShaderRecord* instance0 = shaderTable[instanceOffset0];
//    instance0[0]: ShaderRecord for { Instance0, Geo0, Ray0 (primary) }
//    instance0[1]: ShaderRecord for { Instance0, Geo0, Ray1 (shadow) }
// 
// instanceOffset1 = 2;
// ShaderRecord* instance1 = shaderTable[instanceOffset1];
//    instance1[0]: ShaderRecord for { Instance1, Geo0, Ray0 (primary) }
//    instance1[1]: ShaderRecord for { Instance1, Geo0, Ray1 (shadow) }
//    instance1[2]: ShaderRecord for { Instance1, Geo1, Ray0 (primary) }
//    instance1[3]: ShaderRecord for { Instance1, Geo1, Ray1 (shadow) }
// 
// instanceOffset2 = 6;
// ShaderRecord* instance2 = shaderTable[instanceOffset2];
//    instance2[0]: ShaderRecord for { Instance2, Geo0, Ray0 (primary) }
//    instance2[1]: ShaderRecord for { Instance2, Geo0, Ray1 (shadow) }
//    instance2[2]: ShaderRecord for { Instance2, Geo1, Ray0 (primary) }
//    instance2[3]: ShaderRecord for { Instance2, Geo1, Ray1 (shadow) }
// 
// Note that, in general, the number of geometries per instance can vary, so we must manually specify I_offset for each instance. 
//
// There is also a "miss" shader table. However, it is much simpler because you do not need an entry per geometry.
// You only need an entry per ray type.
	void BuildShaderBindingTables()
	{
		Microsoft::WRL::ComPtr<D3D12::ID3D12StateObjectProperties> stateObjectProperties;
		ThrowIfFailed(mdxrStateObject.As(&stateObjectProperties));

		const uint32_t shaderIdentifierSize = D3D12::ShaderIdentifierSizeInBytes;

		//
		// Ray gen shader table
		//

		void* rayGenShaderIdentifier = stateObjectProperties->GetShaderIdentifier(RaygenShaderName);
		uint32_t numShaderRecords = 1;
		uint32_t shaderRecordSize = shaderIdentifierSize;
		ShaderTable rayGenShaderTable(mdxrDevice, numShaderRecords, shaderRecordSize, L"RayGenShaderTable");
		rayGenShaderTable.push_back(ShaderRecord(rayGenShaderIdentifier, shaderIdentifierSize));
		mRayGenShaderTable = rayGenShaderTable.GetResource();

		//
		// Miss shader table: two entries, one for color rays and one for shadow rays.
		//

		void* colorMissShaderIdentifier = stateObjectProperties->GetShaderIdentifier(ColorMissShaderName);
		void* shadowMissShaderIdentifier = stateObjectProperties->GetShaderIdentifier(ShadowMissShaderName);
		numShaderRecords = 2;
		shaderRecordSize = shaderIdentifierSize;
		ShaderTable missShaderTable(mdxrDevice, numShaderRecords, shaderRecordSize, L"MissShaderTable");
		missShaderTable.push_back(ShaderRecord(colorMissShaderIdentifier, shaderIdentifierSize));
		missShaderTable.push_back(ShaderRecord(shadowMissShaderIdentifier, shaderIdentifierSize));
		mMissShaderTableStrideInBytes = missShaderTable.GetShaderRecordSize();
		mMissShaderTable = missShaderTable.GetResource();

		//
		// Hit group shader table
		//

		// To keep things simple, all our objects use the same hit group shaders. In general, 
		// different objects might use different hit group shaders.
		void* hitGroupShaderIdentifier = stateObjectProperties->GetShaderIdentifier(HitGroupName);

		// Again, for simplicity, we assume in this demo that each instance only has one geometry.
		numShaderRecords = RayCount * (UINT)mInstances.size();
		shaderRecordSize = shaderIdentifierSize + sizeof(LocalRootArguments);
		ShaderTable hitGroupShaderTable(mdxrDevice, numShaderRecords, shaderRecordSize, L"HitGroupShaderTable");

		for (uint32_t instanceIndex = 0; instanceIndex < mInstances.size(); ++instanceIndex)
		{
			for (uint32_t rayTypeIndex = 0; rayTypeIndex < RayCount; ++rayTypeIndex)
			{
				LocalRootArguments rootArguments;

				const RTModelDef& model = mModels[mInstances[instanceIndex].ModelName];

				rootArguments.MaterialIndex = mInstances[instanceIndex].MaterialIndex;
				rootArguments.VertexBufferBindlessIndex = model.VertexBufferBindlessIndex;
				rootArguments.VertexBufferOffset = model.BaseVertexLocation;
				rootArguments.IndexBufferBindlessIndex = model.IndexBufferBindlessIndex;
				rootArguments.IndexBufferOffset = model.StartIndexLocation;
				rootArguments.TexScale = mInstances[instanceIndex].TexScale;
				hitGroupShaderTable.push_back(ShaderRecord(
					hitGroupShaderIdentifier,
					shaderIdentifierSize,
					&rootArguments,
					sizeof(LocalRootArguments)));
			}
		}

		mHitGroupShaderTableStrideInBytes = hitGroupShaderTable.GetShaderRecordSize();
		mHitGroupShaderTable = hitGroupShaderTable.GetResource();
	}
	void BuildDescriptors()
	{
		auto& heap = CbvSrvUavHeap::Get();

		mReflectionMapUavIndex = heap.NextFreeIndex();
		mReflectionMapSrvIndex = heap.NextFreeIndex();

		CreateUav2d(mdxrDevice, mReflectionMap.Get(), mFormat, 0, heap.CpuHandle(mReflectionMapUavIndex));
		CreateSrv2d(mdxrDevice, mReflectionMap.Get(), mFormat, 1, heap.CpuHandle(mReflectionMapSrvIndex));
	}

private:

	static constexpr auto RayCount = 2u; // primary / shadow
	static constexpr auto NumGeometriesPerInstance = 1u;

	D3D12::ID3D12Device5* mdxrDevice = nullptr;
	D3D12::ID3D12GraphicsCommandList6* mdxrCmdList = nullptr;
	Microsoft::WRL::ComPtr<D3D12::ID3D12StateObject> mdxrStateObject;

	Microsoft::WRL::ComPtr<D3D12::ID3D12RootSignature> mGlobalRootSig;
	Microsoft::WRL::ComPtr<D3D12::ID3D12RootSignature> mLocalRootSig;

	D3D12::D3D12_SHADER_BYTECODE mShaderLib;

	std::uint32_t mWidth = 0;
	std::uint32_t mHeight = 0;
	DXGI::DXGI_FORMAT mFormat = DXGI_FORMAT_UNKNOWN;

	std::uint32_t mReflectionMapUavIndex = -1;
	std::uint32_t mReflectionMapSrvIndex = -1;

	Microsoft::WRL::ComPtr<D3D12::ID3D12Resource> mReflectionMap = nullptr;

	std::unordered_map<std::string, Microsoft::WRL::ComPtr<D3D12::ID3D12Resource>> mModelBlases;
	Microsoft::WRL::ComPtr<D3D12::ID3D12Resource> mSceneTlas = nullptr;

	std::unordered_map<std::string, RTModelDef> mModels;
	std::vector<RTInstance> mInstances;

	std::vector<D3D12::D3D12_RAYTRACING_AABB> mInstanceBounds;

	// shader table
	Microsoft::WRL::ComPtr<D3D12::ID3D12Resource> mMissShaderTable;
	Microsoft::WRL::ComPtr<D3D12::ID3D12Resource> mHitGroupShaderTable;
	Microsoft::WRL::ComPtr<D3D12::ID3D12Resource> mRayGenShaderTable;

	std::uint64_t mHitGroupShaderTableStrideInBytes = 0;
	std::uint64_t mMissShaderTableStrideInBytes = 0;
};
