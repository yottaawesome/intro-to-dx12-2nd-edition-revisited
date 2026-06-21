module;

#include <Windows.h>
#include <comdef.h>
#include <dxgi1_6.h>
#include <d3d12.h>
#include <DirectXMath.h>
#include <dxcapi.h>

export module shared:win32;

export namespace Win32
{
	using
		::LARGE_INTEGER,
		::WCHAR,
		::HRESULT,
		::IUnknown,
		::_com_error,
		::GetAsyncKeyState,
		::MultiByteToWideChar,
		::WideCharToMultiByte,
		::lstrlenA,
		::QueryPerformanceCounter,
		::QueryPerformanceFrequency
		;

	constexpr auto CpAcp = CP_ACP;

	inline constexpr auto Failed(HRESULT hr) -> bool
	{
		return FAILED(hr);
	}
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
		::IDxcBlob
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
		::ID3D12Device,
		::ID3D12DeviceChild
		;
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
