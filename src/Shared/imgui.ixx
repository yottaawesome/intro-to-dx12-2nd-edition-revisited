// Copyright (c) 2026, Vasilios Magriplis
// Licensed under the MIT License.
module;

// IMGUI is an opensource library used for drawing GUI elements
// using Direct3D 12 (and other graphics APIs).
#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx12.h"

export module shared:imgui;

// Forward declare message handler from imgui_impl_win32.cpp
extern "C++" IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

export namespace ImGui
{
	auto CheckVersion()
	{
		IMGUI_CHECKVERSION();
	}
	using 
		::ImGui_ImplDX12_NewFrame,
		::ImGui_ImplWin32_NewFrame,
		::ImGui_ImplDX12_Init,
		::ImGui_ImplDX12_Shutdown,
		::ImGui_ImplWin32_Init,
		::ImGui_ImplWin32_Shutdown,
		::ImGui_ImplWin32_WndProcHandler,
		::ImGui_ImplDX12_RenderDrawData,
		::ImGui_ImplDX12_InitInfo,
		::ImGuiIO,
		::ImGuiStyle,
		::ImGui::Checkbox,
		::ImGui::BeginChild,
		::ImGui::GetDrawData,
		::ImGui::CollapsingHeader,
		::ImGui::Text,
		::ImGui::GetIO,
		::ImGui::DestroyContext,
		::ImGui::CreateContext,
        ::ImGui::StyleColorsDark,
		::ImGui::GetCurrentContext,
		::ImGui::SliderFloat,
		::ImGui::NewFrame,
		::ImGui::Begin,
		::ImGui::End,
		::ImGui::Render
		;
}
