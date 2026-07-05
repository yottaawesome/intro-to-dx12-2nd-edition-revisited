import std;
import shared;
import waves;

auto wWinMain(Win32::HINSTANCE hInstance, Win32::HINSTANCE, Win32::LPWSTR, int nCmdShow) -> int
try
{
	if constexpr (IsDebugBuild)
		Win32::_CrtSetDbgFlag(Win32::CrtAllocMemDf | Win32::CrtLeakCheckDf);
	return 0;
}
catch (const DxException& e)
{
	Win32::MessageBoxW(nullptr, e.ToString().c_str(), L"HR Failed", Win32::MbOk);
	return 0;
}
