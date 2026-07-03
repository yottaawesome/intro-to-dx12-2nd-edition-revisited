//***************************************************************************************
// d3dApp.cpp by Frank Luna (C) 2015 All Rights Reserved.
//***************************************************************************************

export module shared:meshutil;
import std;
import :win32;

export
{
	// Defines a subrange of geometry in a MeshGeometry.  This is for when multiple
	// geometries are stored in one vertex and index buffer.  It provides the offsets
	// and data needed to draw a subset of geometry stores in the vertex and index 
	// buffers so that we can implement the technique described by Figure 6.3.
	struct SubmeshGeometry
	{
		Win32::UINT IndexCount = 0;
		Win32::UINT StartIndexLocation = 0;
		Win32::INT BaseVertexLocation = 0;
		Win32::UINT VertexCount = 0;

		// Bounding box of the geometry defined by this submesh. 
		// This is used in later chapters of the book.
		DirectX::BoundingBox Bounds;
	};

	struct MeshGeometry
	{
		// Give it a name so we can look it up by name.
		std::string Name;

		// System memory copies.  Use byte blobs because the vertex/index format can be generic.
		// It is up to the client to cast appropriately.  
		std::vector<Win32::byte> VertexBufferCPU;
		std::vector<Win32::byte> IndexBufferCPU;

		Microsoft::WRL::ComPtr<D3D12::ID3D12Resource> VertexBufferGPU = nullptr;
		Microsoft::WRL::ComPtr<D3D12::ID3D12Resource> IndexBufferGPU = nullptr;

		// Data about the buffers.
		Win32::UINT VertexByteStride = 0;
		Win32::UINT VertexBufferByteSize = 0;
		DXGI::DXGI_FORMAT IndexFormat = DXGI::DXGI_FORMAT::DXGI_FORMAT_R16_UINT;
		Win32::UINT IndexBufferByteSize = 0;

		// A MeshGeometry may store multiple geometries in one vertex/index buffer.
		// Use this container to define the Submesh geometries so we can draw
		// the Submeshes individually.
		std::unordered_map<std::string, SubmeshGeometry> DrawArgs;

		D3D12::D3D12_VERTEX_BUFFER_VIEW VertexBufferView()const
		{
			D3D12::D3D12_VERTEX_BUFFER_VIEW vbv;
			vbv.BufferLocation = VertexBufferGPU->GetGPUVirtualAddress();
			vbv.StrideInBytes = VertexByteStride;
			vbv.SizeInBytes = VertexBufferByteSize;

			return vbv;
		}

		D3D12::D3D12_INDEX_BUFFER_VIEW IndexBufferView()const
		{
			D3D12::D3D12_INDEX_BUFFER_VIEW ibv;
			ibv.BufferLocation = IndexBufferGPU->GetGPUVirtualAddress();
			ibv.Format = IndexFormat;
			ibv.SizeInBytes = IndexBufferByteSize;

			return ibv;
		}
	};
}
