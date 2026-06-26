module;

#include <Windows.h>
#include <comdef.h>
#include <dxgi1_6.h>
#include <d3d12.h>
#include <DirectXMath.h>
#include <d3dx12.h>
#include <dxcapi.h>
#include <wrl/client.h>

export module shared:win32;

export namespace Win32
{
	using
		::UINT,
		::LARGE_INTEGER,
		::WCHAR,
		::HRESULT,
		::IUnknown,
		::_com_error,
		::LPCWSTR,
		::BYTE,
		::OutputDebugStringW,
		::MessageBoxA,
		::MessageBoxW,
		::GetAsyncKeyState,
		::MultiByteToWideChar,
		::WideCharToMultiByte,
		::lstrlenA,
		::QueryPerformanceCounter,
		::QueryPerformanceFrequency
		;

	constexpr auto CpAcp = CP_ACP;
	constexpr auto CpUtf8 = CP_UTF8;
	constexpr auto UIntMax = UINT_MAX;

	inline constexpr auto Failed(HRESULT hr) noexcept -> bool
	{
		return FAILED(hr);
	}
	inline constexpr auto Succeeded(HRESULT hr) noexcept -> bool
	{
		return SUCCEEDED(hr);
	}

	enum Hresults
	{
		Fail = E_FAIL,
	};
}

export namespace Microsoft::WRL
{
	using
		::Microsoft::WRL::ComPtr
		;
}

export namespace D3D
{
	using
		::WKPDID_D3DDebugObjectName
		;
}

export namespace DXC
{
	using 
		::CLSID_DxcUtils,
		::CLSID_DxcCompiler,
		::DxcCreateInstance,
		::DXC_OUT_KIND,
		::IDxcBlobUtf16,
		::IDxcBlobUtf8,
		::IDxcResult,
		::DxcBuffer,
		::IDxcBlobEncoding,
		::IDxcBlob,
		::IDxcCompiler3,
		::IDxcUtils,
		::IDxcIncludeHandler
		;
}

export namespace DXGI
{
	using
		::IDXGIObject,
		::IDXGIAdapter,
		::DXGI_FORMAT,
		::DXGI_SAMPLE_DESC,
		::DXGI_SWAP_CHAIN_DESC,
		::DXGI_SWAP_CHAIN_DESC1,
		::DXGI_SWAP_CHAIN_FULLSCREEN_DESC
		;
}	

export namespace D3D12
{
	using
		::CD3DX12_RASTERIZER_DESC,
		::CD3DX12_BLEND_DESC,
		::CD3DX12_DEPTH_STENCIL_DESC,
		::D3D12_SHADER_BYTECODE,
		::D3D12_INPUT_ELEMENT_DESC,
		::D3D12_GRAPHICS_PIPELINE_STATE_DESC,
		::D3D12_DEFAULT,
		::ID3D12Device,
		::ID3D12DeviceChild,
		::ID3D12RootSignature,
		::D3D12_PRIMITIVE_TOPOLOGY_TYPE
		;

	constexpr auto D3d12ConstantBufferDataPlacementAlignment = D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT;
}

export namespace DirectX
{
	using
		::DirectX::XMFLOAT4,
		::DirectX::XMFLOAT3,
		::DirectX::XMFLOAT4X4,
		::DirectX::FXMVECTOR,
		::DirectX::FXMMATRIX,
		::DirectX::XMFLOAT2,
		::DirectX::XMStoreFloat4,
		::DirectX::XMMatrixIdentity,
		::DirectX::XMMatrixTranspose,
		::DirectX::XMMatrixDeterminant,
		::DirectX::XMMatrixInverse
		;
}
