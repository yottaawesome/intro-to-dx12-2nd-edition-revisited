export module shared:hr;
import std;
import :win32;

export struct Hr
{
	Win32::HRESULT Code = 0;
	constexpr auto Failed(this auto&& self) noexcept -> bool { return Win32::Failed(self.Code); }
	constexpr auto Succeeded(this auto&& self) noexcept -> bool { return Win32::Succeeded(self.Code); }
	constexpr operator bool(this auto&& self) noexcept { return self.Succeeded(); }
	constexpr operator Win32::HRESULT(this auto&& self) noexcept { return self.Code; }
	constexpr auto operator==(const Win32::HRESULT hr) noexcept -> bool { return Code == hr; }
	constexpr auto operator!=(const Win32::HRESULT hr) noexcept -> bool { return Code != hr; }
	constexpr auto operator=(const Win32::HRESULT hr) noexcept -> Hr& { Code = hr; return *this; }
};

namespace
{
	static_assert(
		[] constexpr -> bool
		{
			auto hr = Hr{ .Code = Win32::HRCodes::Fail };
			if (hr)
				throw "Hr should not be successful";
			if (hr != Win32::HRCodes::Fail)
				throw "Hr did not equal the expected fail code";
			return true; 
		}(), "Hr did not behave as expected");
}
