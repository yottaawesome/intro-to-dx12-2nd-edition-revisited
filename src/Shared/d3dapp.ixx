//***************************************************************************************
// d3dApp.cpp by Frank Luna (C) 2015 All Rights Reserved.
//***************************************************************************************

export module shared:d3dapp;
import std;
import :win32;
import :gametimer;
import :descriptorutil;
import :imgui;
import :d3dutil;
import :build;
import :event;

namespace
{
	template<typename TApp>
	auto MainWndProc(
		Win32::HWND hwnd,
		Win32::UINT msg,
		Win32::WPARAM wParam,
		Win32::LPARAM lParam
	) -> Win32::LRESULT
	{
		// Hook Imgui into the message pump.
		if (ImGui::ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam))
			return 0;

		// Forward hwnd on because we can get messages (e.g., WM_CREATE)
		// before CreateWindow returns, and thus before mhMainWnd is valid.
		return TApp::GetApp()->MsgProc(hwnd, msg, wParam, lParam);
	}
}

export struct ImGuiSrvAllocator
{
	CbvSrvUavHeap* Heap = nullptr;
	std::unordered_map<SIZE_T, std::uint32_t> AllocatedIndices;
};

export class D3DApp
{
protected:

	D3DApp(Win32::HINSTANCE hInstance)
	{
		// Only one D3DApp can be constructed.
		//assert(mApp == nullptr);
		mApp = this;
	}
	D3DApp(const D3DApp& rhs) = delete;
	D3DApp& operator=(const D3DApp& rhs) = delete;
	virtual ~D3DApp()
	{
		ShutdownImgui();

		if (md3dDevice != nullptr)
			FlushCommandQueue();
	}

public:

	static auto GetApp() -> D3DApp*
	{
		return mApp;
	}

	auto AppInst() const -> Win32::HINSTANCE
	{
		return mhAppInst;
	}

	auto MainWnd() const -> Win32::HWND
	{
		return mhMainWnd;
	}

	auto AspectRatio() const -> float
	{
		return static_cast<float>(mClientWidth) / mClientHeight;
	}

	auto Run() -> int
	{
		auto msg = Win32::MSG{ 0 };

		mTimer.Reset();

		while (msg.message != Win32::WindowMessages::Quit)
		{
			// If there are Window messages then process them.
			if (Win32::PeekMessageW(&msg, 0, 0, 0, Win32::PmRemove))
			{
				Win32::TranslateMessage(&msg);
				Win32::DispatchMessageW(&msg);
			}
			// Otherwise, do animation/game stuff.
			else
			{
				mTimer.Tick();
				if (not mAppPaused)
				{
					CalculateFrameStats();
					Update(mTimer);
					Draw(mTimer);
				}
				else
				{
					Win32::Sleep(100);
				}
			}
		}

		return (int)msg.wParam;
	}

	void FlushCommandQueue()
	{
		// Advance the fence value to mark commands up to this fence point.
		mCurrentFence++;

		// Add an instruction to the com*mand queue to set a new fence point.  Because we 
		// are on the GPU timeline, the new fence point won't be set until the GPU finishes
		// processing all the commands prior to this Signal().
		ThrowIfFailed(mCommandQueue->Signal(mFence.Get(), mCurrentFence));

		// Wait until the GPU has completed commands up to this fence point.
		if (mFence->GetCompletedValue() < mCurrentFence)
		{
			auto event = Event{};
			// Fire event when GPU hits current fence.  
			ThrowIfFailed(mFence->SetEventOnCompletion(mCurrentFence, event.Get()));
			// Wait until the GPU hits current fence event is fired.
			event.Wait();
		}
	}

	virtual auto Initialize() -> bool
	{
		if (not InitMainWindow())
			return false;

		if (not InitDirect3D())
			return false;

		mLinearAllocator = std::make_unique<DirectX::GraphicsMemory>(md3dDevice.Get());
		mUploadBatch = std::make_unique<DirectX::ResourceUploadBatch>(md3dDevice.Get());

		// Do the initial resize code.
		OnResize();

		return true;
	}

	virtual auto MsgProc(
		Win32::HWND hwnd, 
		Win32::UINT msg, 
		Win32::WPARAM wParam,
		Win32::LPARAM lParam
	) -> Win32::LRESULT
	{
		switch (msg)
		{
			// WM_ACTIVATE is sent when the window is activated or deactivated.  
			// We pause the game when the window is deactivated and unpause it 
			// when it becomes active.  
		case Win32::WindowMessages::Activate:
			if (Win32::LoWord(wParam) == Win32::WA::Inactive)
			{
				mAppPaused = true;
				mTimer.Stop();
			}
			else
			{
				mAppPaused = false;
				mTimer.Start();
			}
			return 0;

			// WM_SIZE is sent when the user resizes the window.  
		case Win32::WindowMessages::Size:
			// Save the new client area dimensions.
			mClientWidth = Win32::LoWord(lParam);
			mClientHeight = Win32::HiWord(lParam);
			if (md3dDevice)
			{
				if (wParam == Win32::Size::Minimized)
				{
					mAppPaused = true;
					mMinimized = true;
					mMaximized = false;
				}
				else if (wParam == Win32::Size::Maximized)
				{
					mAppPaused = false;
					mMinimized = false;
					mMaximized = true;
					OnResize();
				}
				else if (wParam == Win32::Size::Restored)
				{

					// Restoring from minimized state?
					if (mMinimized)
					{
						mAppPaused = false;
						mMinimized = false;
						OnResize();
					}

					// Restoring from maximized state?
					else if (mMaximized)
					{
						mAppPaused = false;
						mMaximized = false;
						OnResize();
					}
					else if (mResizing)
					{
						// If user is dragging the resize bars, we do not resize 
						// the buffers here because as the user continuously 
						// drags the resize bars, a stream of WM_SIZE messages are
						// sent to the window, and it would be pointless (and slow)
						// to resize for each WM_SIZE message received from dragging
						// the resize bars.  So instead, we reset after the user is 
						// done resizing the window and releases the resize bars, which 
						// sends a WM_EXITSIZEMOVE message.
					}
					else // API call such as SetWindowPos or mSwapChain->SetFullscreenState.
					{
						OnResize();
					}
				}
			}
			return 0;

			// WM_EXITSIZEMOVE is sent when the user grabs the resize bars.
		case Win32::WindowMessages::EnterSizeMove:
			mAppPaused = true;
			mResizing = true;
			mTimer.Stop();
			return 0;

			// WM_EXITSIZEMOVE is sent when the user releases the resize bars.
			// Here we reset everything based on the new window dimensions.
		case Win32::WindowMessages::ExitSizeMove:
			mAppPaused = false;
			mResizing = false;
			mTimer.Start();
			OnResize();
			return 0;

			// WM_DESTROY is sent when the window is being destroyed.
		case Win32::WindowMessages::Destroy:
			Win32::PostQuitMessage(0);
			return 0;

			// The WM_MENUCHAR message is sent when a menu is active and the user presses 
			// a key that does not correspond to any mnemonic or accelerator key. 
		case Win32::WindowMessages::MenuChar:
			// Don't beep when we alt-enter.
			return Win32::MakeLResult(0, Win32::MNC::Close);

			// Catch this message so to prevent the window from becoming too small.
		case Win32::WindowMessages::GetMinMaxInfo:
			((Win32::MINMAXINFO*)lParam)->ptMinTrackSize.x = 200;
			((Win32::MINMAXINFO*)lParam)->ptMinTrackSize.y = 200;
			return 0;

		case Win32::WindowMessages::LButtonDown:
		case Win32::WindowMessages::MButtonDown:
		case Win32::WindowMessages::RButtonDown:
			OnMouseDown(wParam, Win32::GetXLParam(lParam), Win32::GetYLParam(lParam));
			return 0;
		case Win32::WindowMessages::LButtonUp:
		case Win32::WindowMessages::MButtonUp:
		case Win32::WindowMessages::RButtonUp:
			OnMouseUp(wParam, Win32::GetXLParam(lParam), Win32::GetYLParam(lParam));
			return 0;
		case Win32::WindowMessages::MouseMove:
			OnMouseMove(wParam, Win32::GetXLParam(lParam), Win32::GetYLParam(lParam));
			return 0;
		case Win32::WindowMessages::KeyUp:
			if (wParam == Win32::VK::Escape)
			{
				Win32::PostQuitMessage(0);
			}

			return 0;
		}

		return Win32::DefWindowProcW(hwnd, msg, wParam, lParam);
	}

	inline static constexpr auto SwapChainBufferCount = 2;

protected:
	virtual void CreateRtvAndDsvDescriptorHeaps()
	{
		mRtvHeap.Init(md3dDevice.Get(), D3D12::D3D12_DESCRIPTOR_HEAP_TYPE::D3D12_DESCRIPTOR_HEAP_TYPE_RTV, SwapChainBufferCount);
		mDsvHeap.Init(md3dDevice.Get(), D3D12::D3D12_DESCRIPTOR_HEAP_TYPE::D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1);
	}

	virtual void OnResize()
	{
		//assert(md3dDevice);
		//assert(mSwapChain);
		//assert(mDirectCmdListAlloc);

		// Flush before changing any resources.
		FlushCommandQueue();

		ThrowIfFailed(mCommandList->Reset(mDirectCmdListAlloc.Get(), nullptr));

		// Release the previous resources we will be recreating.
		for (int i = 0; i < SwapChainBufferCount; ++i)
			mSwapChainBuffer[i].Reset();
		mDepthStencilBuffer.Reset();

		// Resize the swap chain.
		ThrowIfFailed(mSwapChain->ResizeBuffers(
			SwapChainBufferCount,
			mClientWidth, mClientHeight,
			mBackBufferFormat,
			DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH));

		mCurrBackBuffer = 0;

		for (auto i = 0u; i < SwapChainBufferCount; i++)
		{
			ThrowIfFailed(mSwapChain->GetBuffer(
				i, __uuidof(D3D12::ID3D12Resource), &mSwapChainBuffer[i]));
			md3dDevice->CreateRenderTargetView(
				mSwapChainBuffer[i].Get(),
				nullptr,
				mRtvHeap.CpuHandle(i));
		}

		// Create the depth/stencil buffer and view.
		auto depthStencilDesc = D3D12::D3D12_RESOURCE_DESC{
			.Dimension = D3D12::D3D12_RESOURCE_DIMENSION::D3D12_RESOURCE_DIMENSION_TEXTURE2D,
			.Alignment = 0,
			.Width = static_cast<UINT>(mClientWidth),
			.Height = static_cast<UINT>(mClientHeight),
			.DepthOrArraySize = 1,
			.MipLevels = 1,
			.Format = mDepthStencilFormat,
			.SampleDesc = { .Count = 1, .Quality = 0 },
			.Layout = D3D12::D3D12_TEXTURE_LAYOUT::D3D12_TEXTURE_LAYOUT_UNKNOWN,
			.Flags = D3D12::D3D12_RESOURCE_FLAGS::D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL
		};
		auto heapProperties = D3D12::CD3DX12_HEAP_PROPERTIES{ D3D12::D3D12_HEAP_TYPE::D3D12_HEAP_TYPE_DEFAULT };

		auto optClear = D3D12::D3D12_CLEAR_VALUE{
			.Format = mDepthStencilFormat,
			.DepthStencil = { .Depth = 1.0f, .Stencil = 0 }
		};
		ThrowIfFailed(md3dDevice->CreateCommittedResource(
			&heapProperties,
			D3D12::D3D12_HEAP_FLAGS::D3D12_HEAP_FLAG_NONE,
			&depthStencilDesc,
			D3D12::D3D12_RESOURCE_STATES::D3D12_RESOURCE_STATE_COMMON,
			&optClear,
			__uuidof(D3D12::ID3D12Resource), &mDepthStencilBuffer));

		// Create descriptor to mip level 0 of entire resource using the format of the resource.
		md3dDevice->CreateDepthStencilView(mDepthStencilBuffer.Get(), nullptr, DepthStencilView());

		// Transition the resource from its initial state to be used as a depth buffer.
		D3D12::CD3DX12_RESOURCE_BARRIER depthBarrier[1];
		depthBarrier[0] = D3D12::CD3DX12_RESOURCE_BARRIER::Transition(
			mDepthStencilBuffer.Get(),
			D3D12::D3D12_RESOURCE_STATES::D3D12_RESOURCE_STATE_COMMON,
			D3D12::D3D12_RESOURCE_STATES::D3D12_RESOURCE_STATE_DEPTH_WRITE);

		mCommandList->ResourceBarrier(1, depthBarrier);

		// Execute the resize commands.
		ThrowIfFailed(mCommandList->Close());
		auto cmdLists = std::array<D3D12::ID3D12CommandList*, 1>{ mCommandList.Get() };

		mCommandQueue->ExecuteCommandLists(static_cast<UINT>(cmdLists.size()), cmdLists.data());

		// Wait until resize is complete.
		FlushCommandQueue();

		// Update the viewport transform to cover the client area.
		mScreenViewport.TopLeftX = 0;
		mScreenViewport.TopLeftY = 0;
		mScreenViewport.Width = static_cast<float>(mClientWidth);
		mScreenViewport.Height = static_cast<float>(mClientHeight);
		mScreenViewport.MinDepth = 0.0f;
		mScreenViewport.MaxDepth = 1.0f;

		mScissorRect = { 0, 0, mClientWidth, mClientHeight };
	}

	virtual void Update(const GameTimer& gt) = 0;
	virtual void Draw(const GameTimer& gt) = 0;

	// override to define GUI (call once per frame). Make sure to call base implementation first.
	virtual void UpdateImgui(const GameTimer& gt)
	{
		ImGui_ImplDX12_NewFrame();
		ImGui_ImplWin32_NewFrame();
		ImGui::NewFrame();
	}

	// Convenience overrides for handling mouse input.
	virtual void OnMouseDown(WPARAM btnState, int x, int y) {}
	virtual void OnMouseUp(WPARAM btnState, int x, int y) {}
	virtual void OnMouseMove(WPARAM btnState, int x, int y) {}

protected:

	auto InitMainWindow() -> bool
	{
		auto wc = Win32::WNDCLASS{};
		wc.style = Win32::CsHRedraw | Win32::CsVRedraw;
		wc.lpfnWndProc = MainWndProc<D3DApp>;
		wc.cbClsExtra = 0;
		wc.cbWndExtra = 0;
		wc.hInstance = mhAppInst;
		wc.hIcon = Win32::LoadIconW(0, Win32::IdiApplication());
		wc.hCursor = Win32::LoadCursorW(0, Win32::IdcArrow());
		wc.hbrBackground = (Win32::HBRUSH)Win32::GetStockObject(Win32::NullBrush);
		wc.lpszMenuName = 0;
		wc.lpszClassName = L"MainWnd";

		if (not Win32::RegisterClassW(&wc))
		{
			Win32::MessageBoxW(0, L"RegisterClass Failed.", 0, 0);
			return false;
		}

		// Compute window rectangle dimensions based on requested client area dimensions.
		auto R = Win32::RECT{ 0, 0, mClientWidth, mClientHeight };
		Win32::AdjustWindowRect(&R, Win32::WsOverlappedWindow, false);
		int width = R.right - R.left;
		int height = R.bottom - R.top;

		mhMainWnd = Win32::CreateWindowExW(
			0,
			L"MainWnd", 
			mMainWndCaption.c_str(),
			Win32::WsOverlappedWindow, 
			Win32::CwUseDefault, 
			Win32::CwUseDefault, 
			width, 
			height, 
			0, 
			0, 
			mhAppInst, 
			0
		);
		if (!mhMainWnd)
		{
			Win32::MessageBoxW(0, L"CreateWindow Failed.", 0, 0);
			return false;
		}

		Win32::ShowWindow(mhMainWnd, Win32::SwShow);
		Win32::UpdateWindow(mhMainWnd);

		return true;
	}

	auto InitDirect3D() -> bool
	{
		auto factoryFlags = 0u;
		if constexpr (IsDebugBuild)
		{
			factoryFlags = DXGI::CreateFactoryDebug;

			// Enable the D3D12 debug layer.
			auto debugController0 = Microsoft::WRL::ComPtr<D3D12::ID3D12Debug>{};
			auto debugController1 = Microsoft::WRL::ComPtr<D3D12::ID3D12Debug1>{};
			ThrowIfFailed(D3D12::D3D12GetDebugInterface(
				__uuidof(D3D12::ID3D12Debug), &debugController0));
			ThrowIfFailed(debugController0->QueryInterface(
				__uuidof(D3D12::ID3D12Debug1), &debugController1
			));
			debugController1->EnableDebugLayer();
			//debugController1->SetEnableGPUBasedValidation(true);
		}

		ThrowIfFailed(DXGI::CreateDXGIFactory2(
			factoryFlags,
			__uuidof(DXGI::IDXGIFactory4), &mdxgiFactory));

		auto adapters = std::vector<Microsoft::WRL::ComPtr<DXGI::IDXGIAdapter>>{};
		auto foundAdapter = Microsoft::WRL::ComPtr<DXGI::IDXGIAdapter>{};

		// Find an adapter that supports D3D_FEATURE_LEVEL_12_2. This is mainly for laptops so 
		// it picks the discrete GPU over the integrated GPU.
		auto hardwareResult = Win32::HRCodes::Fail;
		for (int i = 0; mdxgiFactory->EnumAdapters(i, &foundAdapter) != DXGI::Error::NotFound; ++i)
		{
			if constexpr (ForceWARPAdapter)
			{
				// Force WARP adapter.
				mdxgiFactory->EnumWarpAdapter(__uuidof(DXGI::IDXGIAdapter), &foundAdapter);
			}

			// Try to create hardware device.
			auto device = Microsoft::WRL::ComPtr<D3D12::ID3D12Device>{};
			hardwareResult = D3D12::D3D12CreateDevice(
				foundAdapter.Get(),             // default adapter
				D3D12::D3D_FEATURE_LEVEL::D3D_FEATURE_LEVEL_12_2,
				__uuidof(D3D12::ID3D12Device), 
				&device);

			if (Win32::Succeeded(hardwareResult))
			{
				ThrowIfFailed(device->QueryInterface(__uuidof(D3D12::ID3D12Device), &md3dDevice));
				break;
			}
		}

		if (Win32::Failed(hardwareResult))
		{
			Win32::MessageBoxW(0, L"Could not find D3D_FEATURE_LEVEL_12_2 GPU", 0, 0);
			return false;
		}

		// Get default adapter, so we can IDXGIAdapter3::QueryVideoMemoryInfo.
		ThrowIfFailed(foundAdapter.As(&mDefaultAdapter));

		ThrowIfFailed(md3dDevice->CreateFence(
			0, 
			D3D12::D3D12_FENCE_FLAGS::D3D12_FENCE_FLAG_NONE,
			__uuidof(D3D12::ID3D12Fence), &mFence));

#ifdef _DEBUG
		LogAdapters();
#endif

		CreateCommandObjects();
		CreateSwapChain();
		CreateRtvAndDsvDescriptorHeaps();

		SamplerHeap::Get().Init(md3dDevice.Get());

		return true;
	}

	void CreateCommandObjects()
	{
		auto queueDesc = D3D12::D3D12_COMMAND_QUEUE_DESC{
			.Type = D3D12::D3D12_COMMAND_LIST_TYPE::D3D12_COMMAND_LIST_TYPE_DIRECT,
			.Flags = D3D12::D3D12_COMMAND_QUEUE_FLAGS::D3D12_COMMAND_QUEUE_FLAG_NONE,
		};
		ThrowIfFailed(md3dDevice->CreateCommandQueue(
			&queueDesc, 
			__uuidof(D3D12::ID3D12CommandQueue),
			&mCommandQueue
		));

		ThrowIfFailed(md3dDevice->CreateCommandAllocator(
			D3D12::D3D12_COMMAND_LIST_TYPE::D3D12_COMMAND_LIST_TYPE_DIRECT,
			__uuidof(D3D12::ID3D12CommandAllocator),
			&mDirectCmdListAlloc));

		auto cmdList = Microsoft::WRL::ComPtr<D3D12::ID3D12GraphicsCommandList>{};
		ThrowIfFailed(md3dDevice->CreateCommandList(
			0,
			D3D12::D3D12_COMMAND_LIST_TYPE::D3D12_COMMAND_LIST_TYPE_DIRECT,
			mDirectCmdListAlloc.Get(), // Associated command allocator
			nullptr,                   // Initial PipelineStateObject
			__uuidof(D3D12::ID3D12GraphicsCommandList), 
			&cmdList
		));
		ThrowIfFailed(cmdList->QueryInterface(__uuidof(D3D12::ID3D12GraphicsCommandList), &mCommandList));

		// Start off in a closed state.  This is because the first time we refer 
		// to the command list we will Reset it, and it needs to be closed before
		// calling Reset.
		mCommandList->Close();
	}

	void CreateSwapChain()
	{
		// Release the previous swapchain we will be recreating.
		mSwapChain.Reset();

		auto sd = DXGI::DXGI_SWAP_CHAIN_DESC1{
			.Width = static_cast<UINT>(mClientWidth),
			.Height = static_cast<UINT>(mClientHeight),
			.Format = mBackBufferFormat,
			.Stereo = false,
			.SampleDesc = { .Count=1, .Quality=0 },
			.BufferUsage = DXGI::DxgiUsageRenderTargetOutput,
			.BufferCount = SwapChainBufferCount,
			.Scaling = DXGI::DXGI_SCALING::DXGI_SCALING_NONE,
			.SwapEffect = DXGI::DXGI_SWAP_EFFECT::DXGI_SWAP_EFFECT_FLIP_DISCARD,
			.AlphaMode = DXGI::DXGI_ALPHA_MODE::DXGI_ALPHA_MODE_UNSPECIFIED,
			.Flags = DXGI::DXGI_SWAP_CHAIN_FLAG::DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH
		};

		// Note: Swap chain uses queue to perform flush.
		auto swapChain1 = Microsoft::WRL::ComPtr<DXGI::IDXGISwapChain1>{};
		ThrowIfFailed(mdxgiFactory->CreateSwapChainForHwnd(
			mCommandQueue.Get(),
			mhMainWnd,
			&sd,
			nullptr,
			nullptr,
			swapChain1.GetAddressOf()));

		ThrowIfFailed(swapChain1.As(&mSwapChain));
	}

	auto CurrentBackBuffer() const -> D3D12::ID3D12Resource*
	{
		return mSwapChainBuffer[mCurrBackBuffer].Get();
	}

	auto CurrentBackBufferView() -> D3D12::CD3DX12_CPU_DESCRIPTOR_HANDLE
	{
		return mRtvHeap.CpuHandle(mCurrBackBuffer);
	}

	auto DepthStencilView() -> D3D12::CD3DX12_CPU_DESCRIPTOR_HANDLE
	{
		return mDsvHeap.CpuHandle(0);
	}

	static void AllocateImGuiSrv(
		ImGui::ImGui_ImplDX12_InitInfo* info,
		D3D12::D3D12_CPU_DESCRIPTOR_HANDLE* outCpu,
		D3D12::D3D12_GPU_DESCRIPTOR_HANDLE* outGpu
	)
	{
		auto* allocator = static_cast<ImGuiSrvAllocator*>(info->UserData);

		const auto index = allocator->Heap->NextFreeIndex();
		*outCpu = allocator->Heap->CpuHandle(index);
		*outGpu = allocator->Heap->GpuHandle(index);

		allocator->AllocatedIndices.emplace(outCpu->ptr, index);
	}

	static void FreeImGuiSrv(
		ImGui::ImGui_ImplDX12_InitInfo* info,
		D3D12::D3D12_CPU_DESCRIPTOR_HANDLE cpu,
		D3D12::D3D12_GPU_DESCRIPTOR_HANDLE
	)
	{
		auto* allocator = static_cast<ImGuiSrvAllocator*>(info->UserData);

		auto it = allocator->AllocatedIndices.find(cpu.ptr);
		if (it == allocator->AllocatedIndices.end())
			return; // Prefer assert/log here if you have a project convention.

		allocator->Heap->ReleaseIndex(it->second);
		allocator->AllocatedIndices.erase(it);
	}

	// Need to call after cbvSrvUavHeap created in derived app.
	void InitImgui(CbvSrvUavHeap& cbvSrvUavHeap)
	{
		const auto imgGuiBindlessIndex = std::uint32_t{cbvSrvUavHeap.NextFreeIndex()};

		// Setup Dear ImGui context
		ImGui::CheckVersion();
		auto ctx = ImGui::CreateContext();

		// Setup Dear ImGui style
		ImGui::StyleColorsDark();
		//ImGui::StyleColorsClassic();

		// Setup Platform/Renderer backends
		ImGui::ImGui_ImplWin32_Init(mhMainWnd);
		// ImGui_ImplDX12_Init() used by the book is obsolete. Use ImGui_ImplDX12_Init() instead.
		mImguiSrvAllocator.Heap = &cbvSrvUavHeap;
		auto initInfo = ImGui::ImGui_ImplDX12_InitInfo{};
		initInfo.Device = md3dDevice.Get();
		initInfo.CommandQueue = mCommandQueue.Get();
		initInfo.NumFramesInFlight = gNumFrameResources;
		initInfo.RTVFormat = mBackBufferFormat;
		initInfo.DSVFormat = DXGI_FORMAT_UNKNOWN;
		initInfo.SrvDescriptorHeap = cbvSrvUavHeap.GetD3dHeap();
		/* allocate from CbvSrvUavHeap */
		initInfo.UserData = &mImguiSrvAllocator;
		initInfo.SrvDescriptorAllocFn = AllocateImGuiSrv;
		/* release back to CbvSrvUavHeap */
		initInfo.SrvDescriptorFreeFn = FreeImGuiSrv;
		ImGui::ImGui_ImplDX12_Init(&initInfo);
	}

	void ShutdownImgui()
	{
		if (ImGui::GetCurrentContext() != nullptr)
		{
			ImGui::ImGui_ImplDX12_Shutdown();
			ImGui::ImGui_ImplWin32_Shutdown();
			ImGui::DestroyContext();
		}
	}

	void CalculateFrameStats()
	{
		// Code computes the average frames per second, and also the 
		// average time it takes to render one frame.  These stats 
		// are appended to the window caption bar.
		static auto frameCnt = 0;
		static auto timeElapsed = 0.0f;

		frameCnt++;
		// Compute averages over each one second period.
		if ((mTimer.TotalTime() - timeElapsed) < 1.0f)
			return;

		auto fps = static_cast<float>(frameCnt); // fps = frameCnt / 1
		auto mspf = 1000.0f / fps;
		auto fpsStr = std::to_wstring(fps);
		auto mspfStr = std::to_wstring(mspf);
		auto windowText = std::format(L"{}    fps: {}   mspf: {}", mMainWndCaption, fpsStr, mspfStr);
		Win32::SetWindowTextW(mhMainWnd, windowText.c_str());

		// Reset for next average.
		frameCnt = 0;
		timeElapsed += 1.0f;
	}

	void LogAdapters()
	{
		auto i = 0u;
		auto adapter = static_cast<IDXGIAdapter*>(nullptr);
		auto adapterList = std::vector<IDXGIAdapter*>{};
		while (mdxgiFactory->EnumAdapters(i, &adapter) != DXGI::Error::NotFound)
		{
			auto desc = DXGI_ADAPTER_DESC{};
			adapter->GetDesc(&desc);

			auto text = std::format(L"***Adapter: {}. Description: {}\n", i, desc.Description);
			Win32::OutputDebugStringW(text.c_str());
			adapterList.push_back(adapter);
			++i;
		}

		for (size_t i = 0; i < adapterList.size(); ++i)
		{
			LogAdapterOutputs(adapterList[i]);
			adapterList[i]->Release();
		}
	}

	void LogAdapterOutputs(DXGI::IDXGIAdapter* adapter)
	{
		auto i = 0u;
		auto output = Microsoft::WRL::ComPtr<DXGI::IDXGIOutput>{};
		while (adapter->EnumOutputs(i, output.ReleaseAndGetAddressOf()) != DXGI::Error::NotFound)
		{
			auto desc = DXGI::DXGI_OUTPUT_DESC{};
			output->GetDesc(&desc);

			auto text = std::format(L"***Output: {}. Description: {}\n", i, desc.DeviceName);
			Win32::OutputDebugStringW(text.c_str());

			LogOutputDisplayModes(output.Get(), mBackBufferFormat);
			++i;
		}
	}

	void LogOutputDisplayModes(DXGI::IDXGIOutput* output, DXGI_FORMAT format)
	{
		auto count = 0u;
		auto flags = 0u;
		// Call with nullptr to get list count.
		output->GetDisplayModeList(format, flags, &count, nullptr);

		auto modeList = std::vector<DXGI_MODE_DESC>(count);
		output->GetDisplayModeList(format, flags, &count, &modeList[0]);

		for (auto& x : modeList)
		{
			auto n = x.RefreshRate.Numerator;
			auto d = x.RefreshRate.Denominator;
			auto text = std::format(L"Width = {} Height = {} Refresh = {}/{}\n", x.Width, x.Height, n, d);

			Win32::OutputDebugStringW(text.c_str());
		}
	}

protected:
	ImGuiSrvAllocator mImguiSrvAllocator;
	static inline D3DApp* mApp = nullptr;

	Win32::HINSTANCE mhAppInst = nullptr; // application instance handle
	Win32::HWND      mhMainWnd = nullptr; // main window handle
	bool      mAppPaused = false;  // is the application paused?
	bool      mMinimized = false;  // is the application minimized?
	bool      mMaximized = false;  // is the application maximized?
	bool      mResizing = false;   // are the resize bars being dragged?

	// Used to keep track of the “delta-time” and game time (§4.4).
	GameTimer mTimer;

	Microsoft::WRL::ComPtr<DXGI::IDXGIFactory6> mdxgiFactory;
	Microsoft::WRL::ComPtr<DXGI::IDXGISwapChain4> mSwapChain;
	Microsoft::WRL::ComPtr<DXGI::IDXGIAdapter4> mDefaultAdapter;
	Microsoft::WRL::ComPtr<D3D12::ID3D12Device5> md3dDevice;

	Microsoft::WRL::ComPtr<D3D12::ID3D12Fence> mFence;
	std::uint64_t mCurrentFence = 0;

	Microsoft::WRL::ComPtr<D3D12::ID3D12CommandQueue> mCommandQueue;
	Microsoft::WRL::ComPtr<D3D12::ID3D12CommandAllocator> mDirectCmdListAlloc;
	Microsoft::WRL::ComPtr<D3D12::ID3D12GraphicsCommandList6> mCommandList;

	int mCurrBackBuffer = 0;
	Microsoft::WRL::ComPtr<D3D12::ID3D12Resource> mSwapChainBuffer[SwapChainBufferCount];
	Microsoft::WRL::ComPtr<D3D12::ID3D12Resource> mDepthStencilBuffer;

	// Utility that grows/shrinks GPU upload heap memory for fire and forget scenarios.
	// This is particularly useful for per-draw constant buffers.
	// This is explained in Chapter 7.
	std::unique_ptr<DirectX::GraphicsMemory> mLinearAllocator = nullptr;

	// Utility for uploading data to GPU memory. This is explained in Chapter 6.
	std::unique_ptr<DirectX::ResourceUploadBatch> mUploadBatch = nullptr;

	DescriptorHeap mRtvHeap;
	DescriptorHeap mDsvHeap;

	D3D12::D3D12_VIEWPORT mScreenViewport;
	D3D12::D3D12_RECT mScissorRect;

	// Derived class should set these in derived constructor to customize starting values.
	std::wstring mMainWndCaption = L"d3d App";
	D3D::D3D_DRIVER_TYPE md3dDriverType = D3D_DRIVER_TYPE_HARDWARE;
	DXGI::DXGI_FORMAT mBackBufferFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
	DXGI::DXGI_FORMAT mDepthStencilFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
	int mClientWidth = 1280;
	int mClientHeight = 720;

};

