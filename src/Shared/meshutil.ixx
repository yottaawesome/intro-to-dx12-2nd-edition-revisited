//***************************************************************************************
// d3dApp.cpp by Frank Luna (C) 2015 All Rights Reserved.
//***************************************************************************************

export module shared:meshutil;
import std;
import :win32;

// Defines a subrange of geometry in a MeshGeometry.  This is for when multiple
// geometries are stored in one vertex and index buffer.  It provides the offsets
// and data needed to draw a subset of geometry stores in the vertex and index 
// buffers so that we can implement the technique described by Figure 6.3.
export struct SubmeshGeometry
{
	std::uint32_t IndexCount = 0;
	std::uint32_t StartIndexLocation = 0;
	std::int32_t BaseVertexLocation = 0;
	std::uint32_t VertexCount = 0;

	// Bounding box of the geometry defined by this submesh. 
	// This is used in later chapters of the book.
	DirectX::BoundingBox Bounds;
};

export struct MeshGeometry
{
	// Give it a name so we can look it up by name.
	std::string Name;

	// System memory copies.  Use byte blobs because the vertex/index format can be generic.
	// It is up to the client to cast appropriately.  
	std::vector<Win32::byte> VertexBufferCPU;
	std::vector<Win32::byte> IndexBufferCPU;

	Microsoft::WRL::ComPtr<D3D12::ID3D12Resource> VertexBufferGPU;
	Microsoft::WRL::ComPtr<D3D12::ID3D12Resource> IndexBufferGPU;

	// Data about the buffers.
	std::uint32_t VertexByteStride = 0;
	std::uint32_t VertexBufferByteSize = 0;
	DXGI::DXGI_FORMAT IndexFormat = DXGI::DXGI_FORMAT::DXGI_FORMAT_R16_UINT;
	std::uint32_t IndexBufferByteSize = 0;

	// A MeshGeometry may store multiple geometries in one vertex/index buffer.
	// Use this container to define the Submesh geometries so we can draw
	// the Submeshes individually.
	std::unordered_map<std::string, SubmeshGeometry> DrawArgs;

	auto VertexBufferView()const -> D3D12::D3D12_VERTEX_BUFFER_VIEW
	{
		return {
			.BufferLocation = VertexBufferGPU->GetGPUVirtualAddress(),
			.SizeInBytes = VertexBufferByteSize,
			.StrideInBytes = VertexByteStride,
		};
	}

	auto IndexBufferView()const -> D3D12::D3D12_INDEX_BUFFER_VIEW
	{
		return {
			.BufferLocation = IndexBufferGPU->GetGPUVirtualAddress(),
			.SizeInBytes = IndexBufferByteSize,
			.Format = IndexFormat,
		};
	}
};
