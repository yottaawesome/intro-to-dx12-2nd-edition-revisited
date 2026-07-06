import std;
import shared;
import litshapes;

// Required exports for DX12-Agility SDK
// https://devblogs.microsoft.com/directx/gettingstarted-dx12agility/
// Make sure this is in sync with the nuget package version.
extern "C" { __declspec(dllexport) extern const auto D3D12SDKVersion = 619u; }
extern "C" { __declspec(dllexport) extern const char* D3D12SDKPath = ".\\D3D12\\"; }

#pragma comment(lib,"d3dcompiler.lib")
#pragma comment(lib, "D3D12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dxcompiler.lib") // dxc

auto wWinMain(Win32::HINSTANCE hInstance, Win32::HINSTANCE, Win32::LPWSTR, int) -> int
try
{
	if constexpr (IsDebugBuild)
		Win32::_CrtSetDbgFlag(Win32::CrtAllocMemDf | Win32::CrtLeakCheckDf);
	auto theApp = LitShapesApp{ hInstance };
	if (not theApp.Initialize())
		return 0;
    return theApp.Run();
}
catch (const DxException& e)
{
	Win32::MessageBoxW(nullptr, e.ToString().c_str(), L"HR Failed", Win32::MbOk);
	return 0;
}
