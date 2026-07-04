import std;
import shared;
import boxgridapp;

auto wWinMain(Win32::HINSTANCE hInstance, Win32::HINSTANCE, Win32::LPWSTR, int) -> int
try
{
	return 0;
}
catch (const DxException& e)
{
	Win32::MessageBoxW(nullptr, e.ToString().c_str(), L"HR Failed", Win32::MbOk);
	return 0;
}
