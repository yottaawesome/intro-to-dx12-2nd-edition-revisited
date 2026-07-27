//***************************************************************************************
// ProceduralRayTracer.cpp by Frank Luna (C) 2011 All Rights Reserved.
//***************************************************************************************

export module raytracingintro:proceduralraytracer;
import std;
import shared;
import :directxraytracinghelper;
import :frameresource;

class ProceduralRayTracer
{
	struct RTInstance
	{
		DirectX::XMFLOAT4X4 Transform;
		std::uint32_t MaterialIndex = 0;
		std::uint32_t PrimitiveType = 0;
		DirectX::XMFLOAT2 TexScale = { 1.0f, 1.0f };
	};

public:
	static constexpr auto HitGroupName = L"HitGroup0";
	static constexpr auto RaygenShaderName = L"RaygenShader";
	static constexpr auto ClosestHitShaderName = L"ClosestHit";
	static constexpr auto ColorMissShaderName = L"Color_MissShader";
	static constexpr auto ShadowMissShaderName = L"Shadow_MissShader";
	static constexpr auto IntersectionShaderName = L"PrimitiveIntersectionShader";

	ProceduralRayTracer(
		D3D12::ID3D12Device5* device,
		D3D12::ID3D12GraphicsCommandList6* cmdList,
		DXC::IDxcBlob* rayTraceLibByteCode,
		DXGI::DXGI_FORMAT format, 
		std::uint32_t width, 
		std::uint32_t height
	)	: mdxrDevice(device)
		, mdxrCmdList(cmdList)
		, mFormat(format)
	{
		mShaderLib = d3dUtil::ByteCodeFromBlob(rayTraceLibByteCode);

		BuildGlobalRootSignature();
		BuildLocalRootSignature();
		BuildRayTraceStateObject();

		OnResize(width, height);
	}

	ProceduralRayTracer(const ProceduralRayTracer& rhs) = delete;
	ProceduralRayTracer& operator=(const ProceduralRayTracer& rhs) = delete;

	auto GetOutputImage()const -> D3D12::ID3D12Resource*
	{
		return mOutputTexture.Get();
	}

	auto GetOutputTextureUavIndex()const -> std::uint32_t
	{
		return mOutputTextureUavIndex;
	}
	
	auto GetOutputTextureSrvIndex()const -> std::uint32_t
	{
		return mOutputTextureSrvIndex;
	}

	void OnResize(std::uint32_t newWidth, std::uint32_t newHeight)
	{
		if ((mWidth != newWidth) or (mHeight != newHeight))
		{
			mWidth = newWidth;
			mHeight = newHeight;
			BuildOutputTexture();
			// New resource, so we need new descriptors to that resource.
			BuildDescriptors();
		}
	}

	// The box is [-1, 1]^3 in local space.
	void AddBox(const DirectX::XMFLOAT4X4& worldTransform, DirectX::XMFLOAT2 texScale, std::uint32_t materialIndex)
	{
		// The box is [-1, 1]^3 in local space.
		auto inst = RTInstance{
			.Transform = worldTransform,
			.MaterialIndex = materialIndex,
			.PrimitiveType = GeoType::Box,
			.TexScale = texScale,
		};

		mInstances.push_back(inst);
	}

	// The cylinder is centered at the origin, aligned with +y axis, has radius 1 and length 2 in local space.
	void AddCylinder(const DirectX::XMFLOAT4X4& worldTransform, DirectX::XMFLOAT2 texScale, std::uint32_t materialIndex)
	{
		// The cylinder is centered at the origin, aligned with +y axis, has radius 1 and length 2 in local space.
		auto inst = RTInstance{
			.Transform = worldTransform,
			.MaterialIndex = materialIndex,
			.PrimitiveType = GeoType::Cylinder,
			.TexScale = texScale,
		};

		mInstances.push_back(inst);
	}

	// The disk is centered at the origin, with normal aimed down the +y-axis, and has radius 1 in local space.
	void AddDisk(const DirectX::XMFLOAT4X4& worldTransform, DirectX::XMFLOAT2 texScale, std::uint32_t materialIndex)
	{
		// The disk is centered at the origin, with normal aimed down the +y-axis, and has radius 1 in local space.
		auto inst = RTInstance{
			.Transform = worldTransform,
			.MaterialIndex = materialIndex,
			.PrimitiveType = GeoType::Disk,
			.TexScale = texScale,
		};

		mInstances.push_back(inst);
	}

	// The sphere is centered at origin with radius 1 in local space.
	void AddSphere(const DirectX::XMFLOAT4X4& worldTransform, DirectX::XMFLOAT2 texScale, std::uint32_t materialIndex)
	{
		// The sphere is centered at origin with radius 1 in local space.
		auto inst = RTInstance{
			.Transform = worldTransform,
			.MaterialIndex = materialIndex,
			.PrimitiveType = GeoType::Sphere,
			.TexScale = texScale,
		};
		mInstances.push_back(inst);
	}

	// Cannot add anymore geometries once we start building.
	void ExecuteBuildAccelerationStructureCommands(D3D12::ID3D12CommandQueue* commandQueue)
	{
		BuildShaderBindingTables();

		auto primitiveBlas = AccelerationStructureBuffers{BuildPrimitiveBlas()};
		auto transition = D3D12::CD3DX12_RESOURCE_BARRIER::UAV(primitiveBlas.accelerationStructure.Get());
		mdxrCmdList->ResourceBarrier(1, &transition);

		auto tlas = AccelerationStructureBuffers{BuildTlas(primitiveBlas.accelerationStructure->GetGPUVirtualAddress())};

		// Build acceleration structures on GPU and wait until it is done.
		ThrowIfFailed(mdxrCmdList->Close());
		auto commandLists = std::array{ static_cast<D3D12::ID3D12CommandList*>(mdxrCmdList) };
		commandQueue->ExecuteCommandLists(static_cast<std::uint32_t>(commandLists.size()), commandLists.data());

		// Need to finish building on GPU before AccelerationStructureBuffers goes out of scope.
		D3DApp::GetApp()->FlushCommandQueue();

		// Building uses intermediate resources, but we only need to save the final results for rendering.
		mPrimitiveBlas = primitiveBlas.accelerationStructure;
		mSceneTlas = tlas.accelerationStructure;
	}

	void Draw(D3D12::ID3D12Resource* passCB, D3D12::ID3D12Resource* matBuffer)
	{
		mdxrCmdList->SetComputeRootSignature(mGlobalRootSig.Get());
		mdxrCmdList->SetComputeRootConstantBufferView(RT_ROOT_ARG_PASS_CBV, passCB->GetGPUVirtualAddress());
		mdxrCmdList->SetComputeRootShaderResourceView(RT_ROOT_ARG_MATERIAL_SRV, matBuffer->GetGPUVirtualAddress());
		mdxrCmdList->SetComputeRootShaderResourceView(RT_ROOT_ARG_ACCELERATION_STRUCT_SRV, mSceneTlas->GetGPUVirtualAddress());

		// Specify dimensions and SBT spans.
		auto dispatchDesc = D3D12::D3D12_DISPATCH_RAYS_DESC{
			.RayGenerationShaderRecord = D3D12::D3D12_GPU_VIRTUAL_ADDRESS_RANGE{
				.StartAddress = mRayGenShaderTable->GetGPUVirtualAddress(),
				.SizeInBytes = mRayGenShaderTable->GetDesc().Width,
			},
			.MissShaderTable = D3D12::D3D12_GPU_VIRTUAL_ADDRESS_RANGE_AND_STRIDE{
				.StartAddress = mMissShaderTable->GetGPUVirtualAddress(),
				.SizeInBytes = mMissShaderTable->GetDesc().Width,
				.StrideInBytes = mMissShaderTableStrideInBytes,
			},
			.HitGroupTable = D3D12::D3D12_GPU_VIRTUAL_ADDRESS_RANGE_AND_STRIDE{
				.StartAddress = mHitGroupShaderTable->GetGPUVirtualAddress(),
				.SizeInBytes = mHitGroupShaderTable->GetDesc().Width,
				.StrideInBytes = mHitGroupShaderTableStrideInBytes,
			},
			.Width = mWidth,
			.Height = mHeight,
			.Depth = 1
		};
		mdxrCmdList->SetPipelineState1(mdxrStateObject.Get());
		mdxrCmdList->DispatchRays(&dispatchDesc);
	}

private:
	void BuildOutputTexture()
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
			.Flags = D3D12::D3D12_RESOURCE_FLAGS::D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS
		};
		
		auto heapProps = D3D12::CD3DX12_HEAP_PROPERTIES{D3D12::D3D12_HEAP_TYPE::D3D12_HEAP_TYPE_DEFAULT};
		ThrowIfFailed(mdxrDevice->CreateCommittedResource(
			&heapProps,
			D3D12::D3D12_HEAP_FLAGS::D3D12_HEAP_FLAG_NONE,
			&texDesc,
			D3D12::D3D12_RESOURCE_STATES::D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
			nullptr,
			__uuidof(D3D12::ID3D12Resource), 
			&mOutputTexture));
	}

	void BuildGlobalRootSignature()
	{
		//
		// Define shader parameters global to all ray-trace shaders.
		auto rayTraceRootParameters = std::array<D3D12::CD3DX12_ROOT_PARAMETER, RT_ROOT_ARG_COUNT>{};
		rayTraceRootParameters[RT_ROOT_ARG_PASS_CBV].InitAsConstantBufferView(1);
		rayTraceRootParameters[RT_ROOT_ARG_MATERIAL_SRV].InitAsShaderResourceView(0);
		rayTraceRootParameters[RT_ROOT_ARG_ACCELERATION_STRUCT_SRV].InitAsShaderResourceView(1);

		auto rtGlobalRootSigDesc = D3D12::CD3DX12_ROOT_SIGNATURE_DESC{
			RT_ROOT_ARG_COUNT, 
			rayTraceRootParameters.data(),
			0, 
			nullptr, // static samplers
			D3D12::D3D12_ROOT_SIGNATURE_FLAGS{
				D3D12::D3D12_ROOT_SIGNATURE_FLAGS::D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED |
				D3D12::D3D12_ROOT_SIGNATURE_FLAGS::D3D12_ROOT_SIGNATURE_FLAG_SAMPLER_HEAP_DIRECTLY_INDEXED
			}
		};

		auto serializedRootSig = Microsoft::WRL::ComPtr<D3D::ID3DBlob>{};
		auto errorBlob = Microsoft::WRL::ComPtr<D3D::ID3DBlob>{};
		auto hr = D3D12::D3D12SerializeRootSignature(
			&rtGlobalRootSigDesc, 
			D3D::D3D_ROOT_SIGNATURE_VERSION::D3D_ROOT_SIGNATURE_VERSION_1,
			serializedRootSig.GetAddressOf(), 
			errorBlob.GetAddressOf()
		);

		if (errorBlob != nullptr)
			Win32::OutputDebugStringA((char*)errorBlob->GetBufferPointer());
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

		constexpr auto numRootParams = 1u;
		constexpr auto num32BitValues = 4u;// see LocalRootArguments
		constexpr auto shaderRegister = 0u;
		auto rayTraceRootParameters = std::array{ D3D12::CD3DX12_ROOT_PARAMETER{} };
		rayTraceRootParameters[0].InitAsConstants(num32BitValues, shaderRegister);

		auto rtLocalRootSigDesc = D3D12::CD3DX12_ROOT_SIGNATURE_DESC(numRootParams, rayTraceRootParameters.data());
		rtLocalRootSigDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_LOCAL_ROOT_SIGNATURE;

		auto serializedRootSig = Microsoft::WRL::ComPtr<D3D::ID3DBlob>{};
		auto errorBlob = Microsoft::WRL::ComPtr<D3D::ID3DBlob>{};
		auto hr = D3D12::D3D12SerializeRootSignature(
			&rtLocalRootSigDesc, 
			D3D_ROOT_SIGNATURE_VERSION_1,
			serializedRootSig.GetAddressOf(), 
			errorBlob.GetAddressOf());
		if (errorBlob != nullptr)
			Win32::OutputDebugStringA((char*)errorBlob->GetBufferPointer());
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

		auto raytracingPipeline = D3D12::CD3DX12_STATE_OBJECT_DESC{ D3D12::D3D12_STATE_OBJECT_TYPE::D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE };

		//
		// Set the compiled DXIL library code that contains our ray tracing shaders and define which shaders
		// to export from the library. If we omit explicit exports, all will be exported.
		//
		auto lib = raytracingPipeline.CreateSubobject<CD3DX12_DXIL_LIBRARY_SUBOBJECT>();
		lib->SetDXILLibrary(&mShaderLib);
		lib->DefineExport(RaygenShaderName);
		lib->DefineExport(ClosestHitShaderName);
		lib->DefineExport(ColorMissShaderName);
		lib->DefineExport(ShadowMissShaderName);
		lib->DefineExport(IntersectionShaderName);

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
		hitGroup->SetHitGroupType(D3D12_HIT_GROUP_TYPE_PROCEDURAL_PRIMITIVE);
		hitGroup->SetClosestHitShaderImport(ClosestHitShaderName);
		hitGroup->SetIntersectionShaderImport(IntersectionShaderName);
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
		constexpr auto maxRecursionDepth = std::uint32_t{ MaxRecursionDepth };
		pipelineConfig->Config(maxRecursionDepth);

		ThrowIfFailed(mdxrDevice->CreateStateObject(raytracingPipeline, __uuidof(D3D12::ID3D12StateObject),&mdxrStateObject));
	}

	auto BuildPrimitiveBlas() -> AccelerationStructureBuffers
	{
		// For procedural primitive geometry, DXR just cares about the bounding box.
		// The actual intersection will be done in the intersection shader. 
		// All of our primitive objects (sphere, box, cylinder) are defined in [-1,1]^3 in
		// local space. Therefore, we only need one geometry that we will instance
		// multiple times and branch based on primitive type.

		const auto numGeometries = 1u;

		// Bounds of each geometry is needed for BLAS.
		auto bounds = D3D12::D3D12_RAYTRACING_AABB{
			.MinX = -1.0f,
			.MinY = -1.0f,
			.MinZ = -1.0f,
			.MaxX = +1.0f,
			.MaxY = +1.0f,
			.MaxZ = +1.0f,
		};
		
		mGeoBoundsBuffer = std::make_unique<UploadBuffer<D3D12::D3D12_RAYTRACING_AABB>>(mdxrDevice, 1, false);
		mGeoBoundsBuffer->CopyData(0, bounds);

		auto boundsBasePtr = D3D12::D3D12_GPU_VIRTUAL_ADDRESS{mGeoBoundsBuffer->Resource()->GetGPUVirtualAddress()};

		auto geoDesc = std::array{
			D3D12::D3D12_RAYTRACING_GEOMETRY_DESC{
				.Type = D3D12::D3D12_RAYTRACING_GEOMETRY_TYPE::D3D12_RAYTRACING_GEOMETRY_TYPE_PROCEDURAL_PRIMITIVE_AABBS,
				.Flags = D3D12::D3D12_RAYTRACING_GEOMETRY_FLAGS::D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE,
				.AABBs = {
					.AABBCount = 1,
					.AABBs = {
						.StartAddress = boundsBasePtr,
						.StrideInBytes = sizeof(D3D12::D3D12_RAYTRACING_AABB)
					}
				}
			}
		};

		//
		// BLAS is built from N geometries.
		//
		auto blasDesc = D3D12::D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC{
			.Inputs = {
				.Type = D3D12::D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE::D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL,
				.Flags = D3D12::D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS::D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE,
				.NumDescs = numGeometries,
				.DescsLayout = D3D12::D3D12_ELEMENTS_LAYOUT::D3D12_ELEMENTS_LAYOUT_ARRAY,
				.pGeometryDescs = geoDesc.data()
			}
		};

		// Query some info that is device dependent for building the BLAS.
		D3D12::D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO prebuildInfo = {};
		mdxrDevice->GetRaytracingAccelerationStructurePrebuildInfo(&blasDesc.Inputs, &prebuildInfo);
		//assert(prebuildInfo.ResultDataMaxSizeInBytes > 0);

		auto scratch = Microsoft::WRL::ComPtr<D3D12::ID3D12Resource>{};
		AllocateUAVBuffer(mdxrDevice,
			prebuildInfo.ScratchDataSizeInBytes,
			&scratch,
			D3D12::D3D12_RESOURCE_STATES::D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
			L"ScratchResource");

		auto blas = Microsoft::WRL::ComPtr<D3D12::ID3D12Resource>{};
		AllocateUAVBuffer(mdxrDevice, prebuildInfo.ResultDataMaxSizeInBytes,
			&blas,
			D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE,
			L"BottomLevelAccelerationStructure");
		blasDesc.ScratchAccelerationStructureData = scratch->GetGPUVirtualAddress();
		blasDesc.DestAccelerationStructureData = blas->GetGPUVirtualAddress();

		mdxrCmdList->BuildRaytracingAccelerationStructure(&blasDesc, 0, nullptr);

		auto bottomLevelASBuffers = AccelerationStructureBuffers{
			.scratch = scratch,
			.accelerationStructure = blas,
			.ResultDataMaxSizeInBytes = prebuildInfo.ResultDataMaxSizeInBytes
		};

		return bottomLevelASBuffers;
	}

	auto BuildInstanceBuffer(D3D12::D3D12_GPU_VIRTUAL_ADDRESS blasGpuAddress) -> Microsoft::WRL::ComPtr<D3D12::ID3D12Resource>
	{
		auto instanceDescs = std::vector<D3D12::D3D12_RAYTRACING_INSTANCE_DESC>{};
		instanceDescs.resize(mInstances.size());

		// Instances of BLAS structures. Here we only have one BLAS, but a TLAS can contain instances of different BLASes.
		for (auto i = 0u; i < mInstances.size(); ++i)
		{
			instanceDescs[i].InstanceMask = 1;
			instanceDescs[i].InstanceContributionToHitGroupIndex = i * RayCount * NumGeometriesPerInstance; // instance offset for SBT
			instanceDescs[i].AccelerationStructure = blasGpuAddress;
			instanceDescs[i].InstanceID = i; // for shader SV_InstanceID
			instanceDescs[i].Flags = D3D12::D3D12_RAYTRACING_INSTANCE_FLAGS::D3D12_RAYTRACING_INSTANCE_FLAG_NONE;
			auto worldTransform = DirectX::XMMATRIX{DirectX::XMLoadFloat4x4(&mInstances[i].Transform)};
			DirectX::XMStoreFloat3x4(reinterpret_cast<DirectX::XMFLOAT3X4*>(instanceDescs[i].Transform), worldTransform);
		}

		auto bufferSize = static_cast<std::uint64_t>(instanceDescs.size() * sizeof(D3D12::D3D12_RAYTRACING_INSTANCE_DESC));
		auto instanceBuffer = Microsoft::WRL::ComPtr<D3D12::ID3D12Resource>{};
		AllocateUploadBuffer(mdxrDevice, instanceDescs.data(), bufferSize, &instanceBuffer, L"InstanceDescs");

		return instanceBuffer;
	}

	auto BuildTlas(D3D12::D3D12_GPU_VIRTUAL_ADDRESS blasGpuAddress) -> AccelerationStructureBuffers
	{
		// TLAS defines instances to one or more BLAS structures. Here we only have one BLAS.

		auto tlasDesc = D3D12::D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC{
			.Inputs = {
				.Type = D3D12::D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE::D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL,
				.Flags = D3D12::D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS::D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE,
				.NumDescs = static_cast<std::uint32_t>(mInstances.size()),
				.DescsLayout = D3D12::D3D12_ELEMENTS_LAYOUT::D3D12_ELEMENTS_LAYOUT_ARRAY,
			}
		};

		auto prebuildInfo = D3D12::D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO{};
		mdxrDevice->GetRaytracingAccelerationStructurePrebuildInfo(&tlasDesc.Inputs, &prebuildInfo);
		//assert(prebuildInfo.ResultDataMaxSizeInBytes > 0);

		auto scratch = Microsoft::WRL::ComPtr<D3D12::ID3D12Resource>{};
		AllocateUAVBuffer(mdxrDevice,
			prebuildInfo.ScratchDataSizeInBytes,
			&scratch,
			D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
			L"ScratchResource");

		auto tlas = Microsoft::WRL::ComPtr<D3D12::ID3D12Resource>{};
		AllocateUAVBuffer(mdxrDevice,
			prebuildInfo.ResultDataMaxSizeInBytes,
			&tlas,
			D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE,
			L"TopLevelAccelerationStructure");

		auto instanceBuffer = Microsoft::WRL::ComPtr<D3D12::ID3D12Resource>{BuildInstanceBuffer(blasGpuAddress)};
		tlasDesc.Inputs.InstanceDescs = instanceBuffer->GetGPUVirtualAddress();
		tlasDesc.DestAccelerationStructureData = tlas->GetGPUVirtualAddress();
		tlasDesc.ScratchAccelerationStructureData = scratch->GetGPUVirtualAddress();

		mdxrCmdList->BuildRaytracingAccelerationStructure(&tlasDesc, 0, nullptr);

		return AccelerationStructureBuffers{
			.scratch = scratch,
			.accelerationStructure = tlas,
			.instanceDesc = instanceBuffer,
			.ResultDataMaxSizeInBytes = prebuildInfo.ResultDataMaxSizeInBytes
		};
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
		auto stateObjectProperties = Microsoft::WRL::ComPtr<D3D12::ID3D12StateObjectProperties>{};
		ThrowIfFailed(mdxrStateObject.As(&stateObjectProperties));

		constexpr auto shaderIdentifierSize = std::uint32_t{ D3D12::ShaderIdentifierSizeInBytes };

		//
		// Ray gen shader table
		//

		auto rayGenShaderIdentifier = static_cast<void*>(stateObjectProperties->GetShaderIdentifier(RaygenShaderName));
		auto numShaderRecords = 1u;
		auto shaderRecordSize = shaderIdentifierSize;
		ShaderTable rayGenShaderTable(mdxrDevice, numShaderRecords, shaderRecordSize, L"RayGenShaderTable");
		rayGenShaderTable.push_back(ShaderRecord(rayGenShaderIdentifier, shaderIdentifierSize));
		mRayGenShaderTable = rayGenShaderTable.GetResource();

		//
		// Miss shader table: two entries, one for color rays and one for shadow rays.
		//

		auto colorMissShaderIdentifier = static_cast<void*>(stateObjectProperties->GetShaderIdentifier(ColorMissShaderName));
		auto shadowMissShaderIdentifier = static_cast<void*>(stateObjectProperties->GetShaderIdentifier(ShadowMissShaderName));
		numShaderRecords = 2u;
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
		auto hitGroupShaderIdentifier = static_cast<void*>(stateObjectProperties->GetShaderIdentifier(HitGroupName));

		struct LocalRootArguments
		{
			std::uint32_t MaterialIndex;
			std::uint32_t PrimitiveType;
			DirectX::XMFLOAT2 TexScale;
		};

		// Again, for simplicity, we assume in this demo that each instance only has one geometry.
		numShaderRecords = static_cast<std::uint32_t>(RayCount * mInstances.size());
		shaderRecordSize = shaderIdentifierSize + sizeof(LocalRootArguments);
		auto hitGroupShaderTable = ShaderTable(mdxrDevice, numShaderRecords, shaderRecordSize, L"HitGroupShaderTable");

		for (auto instanceIndex = 0u; instanceIndex < mInstances.size(); ++instanceIndex)
		{
			for (auto rayTypeIndex = 0u; rayTypeIndex < RayCount; ++rayTypeIndex)
			{
				// Use same root args for primary and shadow rays, but could be different in general.
				auto rootArguments = LocalRootArguments{
					.MaterialIndex = mInstances[instanceIndex].MaterialIndex,
					.PrimitiveType = mInstances[instanceIndex].PrimitiveType,
					.TexScale = mInstances[instanceIndex].TexScale
				};
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

		mOutputTextureUavIndex = heap.NextFreeIndex();
		mOutputTextureSrvIndex = heap.NextFreeIndex();

		constexpr auto mipSlice = 0u;
		constexpr auto mipCount = 1u;
		CreateUav2d(mdxrDevice, mOutputTexture.Get(), mFormat, mipSlice, heap.CpuHandle(mOutputTextureUavIndex));
		CreateSrv2d(mdxrDevice, mOutputTexture.Get(), mFormat, mipCount, heap.CpuHandle(mOutputTextureSrvIndex));
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

	std::uint32_t mOutputTextureUavIndex = -1;
	std::uint32_t mOutputTextureSrvIndex = -1;

	Microsoft::WRL::ComPtr<D3D12::ID3D12Resource> mOutputTexture;
	Microsoft::WRL::ComPtr<D3D12::ID3D12Resource> mPrimitiveBlas;
	Microsoft::WRL::ComPtr<D3D12::ID3D12Resource> mSceneTlas;

	std::vector<RTInstance> mInstances;

	std::vector<D3D12::D3D12_RAYTRACING_AABB> mInstanceBounds;
	std::unique_ptr<UploadBuffer<D3D12::D3D12_RAYTRACING_AABB>> mGeoBoundsBuffer;

	// shader table
	Microsoft::WRL::ComPtr<D3D12::ID3D12Resource> mMissShaderTable;
	Microsoft::WRL::ComPtr<D3D12::ID3D12Resource> mHitGroupShaderTable;
	Microsoft::WRL::ComPtr<D3D12::ID3D12Resource> mRayGenShaderTable;

	std::uint64_t mHitGroupShaderTableStrideInBytes = 0;
	std::uint64_t mMissShaderTableStrideInBytes = 0;
};
