import std;
import shared;
import basictesselation;

#pragma comment(lib,"d3dcompiler.lib")
#pragma comment(lib, "D3D12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dxcompiler.lib") // dxc

auto wWinMain(Win32::HINSTANCE hInstance, Win32::HINSTANCE, Win32::LPWSTR, int) -> int
try
{
	return BasicTessellationApp{hInstance}.Run();
}
catch (const std::exception& ex)
{
	Win32::MessageBoxW(nullptr, AnsiToWString(ex.what()).c_str(), L"Error", Win32::MbOk);
	return 0;
}
