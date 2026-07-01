module;

#include <Windows.h>
#include <WindowsX.h>
#include <comdef.h>
#include <dxgi1_6.h>
#include <d3d12.h>
#include <DirectXMath.h>
#include <d3dx12.h>
#include <dxcapi.h>
#include <wrl/client.h>
#include <DirectXPackedVector.h>
#include <DirectXTK12/ResourceUploadBatch.h>
#include <directxtk12/SimpleMath.h>
#include <directxtk12/BufferHelpers.h>
#include <DirectXCollision.h>

export module shared:win32;

template<auto X>
struct ConstValue
{
	constexpr operator decltype(X)() noexcept
	{ 
		return X; 
	}
	static constexpr auto operator()() noexcept -> decltype(X)
	{
		return X;
	}
};

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
		::INT,
		::byte,
		::BOOL,
		::USHORT,
		::DWORD,
		::HINSTANCE,
		::LRESULT,
		::HWND,
		::HANDLE,
		::UINT_PTR,
		::WPARAM,
		::LPARAM,
		::FLOAT,
		::WNDCLASS,
		::HBRUSH,
		::RECT,
		::MINMAXINFO,
		::POINT,
		::PostQuitMessage,
		::DefWindowProc,
		::CreateWindowExW,
		::AdjustWindowRect,
		::ShowWindow,
		::UpdateWindow,
		::RegisterClassW,
		::GetStockObject,
		::LoadIconW,
		::LoadCursorW,
		::SetWindowTextW,
		::CloseHandle,
		::WaitForSingleObject,
		::CreateEventEx,
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

	namespace HRCodes
	{
		constexpr auto OK = S_OK;
		constexpr auto Fail = E_FAIL;
		constexpr auto InvalidArg = E_INVALIDARG;
		constexpr auto OutOfMemory = E_OUTOFMEMORY;
		constexpr auto NotImpl = E_NOTIMPL;
	}

	constexpr auto Failed(HRESULT hr) noexcept -> bool
	{
		return FAILED(hr);
	}
	constexpr auto Succeeded(HRESULT hr) noexcept -> bool
	{
		return SUCCEEDED(hr);
	}

	constexpr auto GetXLParam(auto lParam) noexcept -> auto
	{
		return GET_X_LPARAM(lParam);
	}
	constexpr auto GetYLParam(auto lParam) noexcept -> auto
	{
		return GET_Y_LPARAM(lParam);
	}
	constexpr auto MakeLResult(auto a, auto b) noexcept -> auto
	{
		return MAKELRESULT(a, b);
	}
	constexpr auto LoWord(auto dw) noexcept -> auto
	{
		return LOWORD(dw);
	}
	constexpr auto HiWord(auto dw) noexcept -> auto
	{
		return HIWORD(dw);
	}
	enum MNC//MenuChar
	{
		Close = MNC_CLOSE,
	};
	enum WindowMessages
	{
		Activate = WM_ACTIVATE,
		Size = WM_SIZE,
		EnterSizeMove = WM_ENTERSIZEMOVE,
		ExitSizeMove = WM_EXITSIZEMOVE,
		Destroy = WM_DESTROY,
		MenuChar = WM_MENUCHAR,
		GetMinMaxInfo = WM_GETMINMAXINFO,
		LButtonDown = WM_LBUTTONDOWN,
		MButtonDown = WM_MBUTTONDOWN,
		RButtonDown = WM_RBUTTONDOWN,
		LButtonUp = WM_LBUTTONUP,
		MButtonUp = WM_MBUTTONUP,
		RButtonUp = WM_RBUTTONUP,
		MouseMove = WM_MOUSEMOVE,
		KeyUp = WM_KEYUP
	};
	enum VK
	{
		Escape = VK_ESCAPE
	};

	enum Size
	{
		Minimized = SIZE_MINIMIZED,
		Maximized = SIZE_MAXIMIZED,
		Restored = SIZE_RESTORED
	};

	enum WA
	{
		Inactive = WA_INACTIVE,
		Active = WA_ACTIVE,
		ClickActive = WA_CLICKACTIVE
	};

	constexpr auto CsHRedraw = CS_HREDRAW;
	constexpr auto CsVRedraw = CS_VREDRAW;
	constexpr auto WsOverlappedWindow = WS_OVERLAPPEDWINDOW;
	constexpr auto CwUseDefault = CW_USEDEFAULT;
	constexpr auto SwShow = SW_SHOW;

	constexpr auto NullBrush = NULL_BRUSH;
	constexpr auto IdiApplication = ConstValue<IDI_APPLICATION>{};
	constexpr auto IdcArrow = ConstValue<IDC_ARROW>{};

	constexpr auto Infinite = INFINITE;
	constexpr auto EventAllAccess = EVENT_ALL_ACCESS;
	constexpr auto CpAcp = CP_ACP;
	constexpr auto CpUtf8 = CP_UTF8;
	constexpr auto UIntMax = UINT_MAX;
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
		::WKPDID_D3DDebugObjectName,
		::D3D_DRIVER_TYPE
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
	constexpr auto CreateFactoryDebug = DXGI_CREATE_FACTORY_DEBUG;
	enum Error
	{
		NotFound = DXGI_ERROR_NOT_FOUND,
		DeviceRemoved = DXGI_ERROR_DEVICE_REMOVED,
		DeviceHung = DXGI_ERROR_DEVICE_HUNG,
		DeviceReset = DXGI_ERROR_DEVICE_RESET,
		DriverInternalError = DXGI_ERROR_DRIVER_INTERNAL_ERROR,
		AccessDenied = DXGI_ERROR_ACCESS_DENIED
	};

	constexpr auto DxgiUsageRenderTargetOutput = DXGI_USAGE_RENDER_TARGET_OUTPUT;

	using
		::DXGI_SCALING,
		::IDXGISwapChain1,
		::DXGI_OUTPUT_DESC,
		::DXGI_SWAP_EFFECT,
		::DXGI_MODE_SCANLINE_ORDER,
		::DXGI_ALPHA_MODE,
		::DXGI_SWAP_CHAIN_FLAG,
		::IDXGIObject,
		::IDXGIOutput,
		::IDXGIFactory,
		::IDXGIFactory6,
		::IDXGISwapChain4,
		::IDXGIAdapter4,
		::IDXGIAdapter,
		::IDXGIFactory4,
		::ID3D12Device5,
		::ID3D12Fence,
		::ID3D12CommandQueue,
		::ID3D12CommandAllocator,
		::ID3D12GraphicsCommandList6,
		::DXGI_FORMAT,
		::DXGI_SAMPLE_DESC,
		::DXGI_SWAP_CHAIN_DESC,
		::DXGI_SWAP_CHAIN_DESC1,
		::DXGI_SWAP_CHAIN_FULLSCREEN_DESC,
		::CreateDXGIFactory2
		;
}	

export namespace D3D12
{
	constexpr auto D3d12Float32Max = D3D12_FLOAT32_MAX;

	inline constexpr auto DefaultShader4ComponentMapping() noexcept -> std::uint32_t
	{
		return D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	}

	using
		::ID3D12CommandList,
		::CD3DX12_HEAP_PROPERTIES,
		::CD3DX12_RESOURCE_BARRIER,
		::D3D12_RESOURCE_STATES,
		::D3D12_HEAP_TYPE,
		::D3D_FEATURE_LEVEL,
		::D3D12_FEATURE_DATA_D3D12_OPTIONS,
		::D3D12_HEAP_FLAGS,
		::D3D12_CLEAR_VALUE,
		::D3D12_RESOURCE_DESC,
		::D3D12_RESOURCE_FLAGS,
		::D3D12_RESOURCE_DIMENSION,
		::D3D12_TEXTURE_LAYOUT,
		::D3D12_COMMAND_QUEUE_DESC,
		::D3D12_COMMAND_QUEUE_FLAGS,
		::D3D12_COMMAND_LIST_TYPE,
		::D3D12_VERTEX_BUFFER_VIEW,
		::D3D12_COMPARISON_FUNC,
		::D3D12_RECT,
		::D3D12_VIEWPORT,
		::D3D12_SAMPLER_DESC,
		::D3D12_FILTER,
		::D3D12_DSV_DIMENSION,
		::D3D12_DSV_FLAGS,
		::D3D12_TEXTURE_COPY_LOCATION,
		::D3D12_TEXTURE_ADDRESS_MODE,
		::D3D12_INDEX_BUFFER_VIEW,
		::D3D12_CONSTANT_BUFFER_VIEW_DESC,
		::CD3DX12_RASTERIZER_DESC,
		::D3D12_DEPTH_STENCIL_VIEW_DESC,
		::CD3DX12_BLEND_DESC,
		::CD3DX12_DEPTH_STENCIL_DESC,
		::D3D12_SHADER_BYTECODE,
		::D3D12_DESCRIPTOR_HEAP_TYPE,
		::D3D12_SUBRESOURCE_DATA,
		::CD3DX12_GPU_DESCRIPTOR_HANDLE,
		::D3D12_INPUT_ELEMENT_DESC,
		::D3D12_GRAPHICS_PIPELINE_STATE_DESC,
		::CD3DX12_CPU_DESCRIPTOR_HANDLE,
		::D3D12_DEFAULT,
		::ID3D12Device,
		::ID3D12Fence,
		::ID3D12DeviceChild,
		::ID3D12RootSignature,
		::ID3D12Debug,
		::ID3D12Debug1,
		::ID3D12Resource,
		::ID3D12CommandQueue,
		::ID3D12DescriptorHeap,
		::ID3D12CommandAllocator,
		::ID3D12GraphicsCommandList,
		::ID3D12GraphicsCommandList6,
		::D3D12_PRIMITIVE_TOPOLOGY_TYPE,
		::D3D12GetDebugInterface,
		::D3D12CreateDevice
		;

	constexpr auto D3d12ConstantBufferDataPlacementAlignment = D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT;
}
// vector math operators are not exported by default, so we need to export them explicitly
export 
{
	using 
		::DirectX::operator+,
		::DirectX::operator-,
		::DirectX::operator*
		;
}

export namespace DirectX
{
	// using produces a warning
	constexpr auto XM_Pi = DirectX::XM_PI;
	constexpr auto XM_2Pi = DirectX::XM_2PI;

	using
		::DirectX::ResourceUploadBatch,
		::DirectX::XMFLOAT4,
		::DirectX::XMFLOAT3,
		::DirectX::XMFLOAT4X4,
		::DirectX::FXMVECTOR,
		::DirectX::FXMMATRIX,
		::DirectX::XMFLOAT2,
		::DirectX::CXMMATRIX,
		::DirectX::XMVECTOR,
		::DirectX::BoundingSphere,
		::DirectX::BoundingFrustum,
		::DirectX::GraphicsMemory,
		::DirectX::BoundingBox,
		::DirectX::XMLoadFloat4x4,
		::DirectX::XMMatrixPerspectiveFovRH,
		::DirectX::CreateStaticBuffer,
		::DirectX::XMStoreFloat3,
		::DirectX::XMVectorMin,
		::DirectX::XMVectorMax,
		::DirectX::XMStoreFloat3,
		::DirectX::XMLoadFloat3,
		::DirectX::XMLoadFloat4,
		::DirectX::XMMatrixAffineTransformation,
		::DirectX::XMVectorLerp,
		::DirectX::XMQuaternionSlerp,
		::DirectX::XMMatrixRotationQuaternion,
		::DirectX::XMMatrixMultiply,
		::DirectX::XMStoreFloat4x4,
		::DirectX::XMVector3Less,
		::DirectX::XMVector3Cross,
		::DirectX::XMVectorGetX,
		::DirectX::XMVector3Dot,
		::DirectX::XMVectorZero,
		::DirectX::XMVector3Greater,
		::DirectX::XMVector3Normalize,
		::DirectX::XMVector3LengthSq,
		::DirectX::CreateTextureFromMemory,
		::DirectX::XMVectorSet,
		::DirectX::XMStoreFloat4,
		::DirectX::XMMatrixIdentity,
		::DirectX::XMMatrixTranspose,
		::DirectX::XMMatrixDeterminant,
		::DirectX::XMMatrixInverse
		;

	namespace SimpleMath
	{
		using 
			::DirectX::SimpleMath::Vector2,
			::DirectX::SimpleMath::Vector3,
			::DirectX::SimpleMath::Vector4,
			::DirectX::SimpleMath::Plane,
			::DirectX::SimpleMath::Matrix
		;
	}

	namespace PackedVector
	{
		using
			::DirectX::PackedVector::XMStoreColor,
			::DirectX::PackedVector::XMCOLOR
			;
	}
}
