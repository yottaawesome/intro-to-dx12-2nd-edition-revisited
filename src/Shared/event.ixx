// Copyright (c) 2026, Vasilios Magriplis
// Licensed under the MIT License.
export module shared:event;
import std;
import :win32;

export class Event final
{
public:
	~Event() { Win32::CloseHandle(mEventHandle); }
	Event() = default;
	Event(const Event& rhs) = delete;
	auto operator=(const Event& rhs) -> Event& = delete;
	auto Get() const noexcept -> Win32::HANDLE { return mEventHandle; }
	auto Wait() const -> bool
	{
		auto result = Win32::WaitForSingleObject(mEventHandle, Win32::Infinite);
		if (result == Win32::WaitResult::WaitFailed)
			throw std::runtime_error{ "Failed to wait for event." };
		return result == Win32::WaitResult::Object0;
	}

private:
	// nonsignaled, auto-reset
	Win32::HANDLE mEventHandle =
		[] -> Win32::HANDLE
		{
			auto handle = Win32::CreateEventExW(nullptr, nullptr, 0, Win32::EventAllAccess);
			return handle ? handle : throw std::runtime_error{ "Failed to create event." };
		}();
};