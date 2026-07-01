export module shared:d3dapp;
import std;
import :win32;
import :gametimer;
import :descriptorutil;
import :imgui;
import :d3dutil;

//extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

template<typename TApp>
auto MainWndProc(
    Win32::HWND hwnd, 
    Win32::UINT msg, 
    Win32::WPARAM wParam, 
    Win32::LPARAM lParam
) -> Win32::LRESULT
{
    // Hook Imgui into the message pump.
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam))
        return 0;

    // Forward hwnd on because we can get messages (e.g., WM_CREATE)
    // before CreateWindow returns, and thus before mhMainWnd is valid.
    return TApp::GetApp()->MsgProc(hwnd, msg, wParam, lParam);
}

// TODO
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
//
//    int Run();

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
            Win32::HANDLE eventHandle = Win32::CreateEventExW(nullptr, nullptr, 0, Win32::EventAllAccess);

            // Fire event when GPU hits current fence.  
            ThrowIfFailed(mFence->SetEventOnCompletion(mCurrentFence, eventHandle));

            // Wait until the GPU hits current fence event is fired.
            Win32::WaitForSingleObject(eventHandle, Win32::Infinite);
            Win32::CloseHandle(eventHandle);
        }
    }
//
//    virtual bool Initialize();
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
//
protected:
    virtual void CreateRtvAndDsvDescriptorHeaps()
    {
        mRtvHeap.Init(md3dDevice.Get(), D3D12::D3D12_DESCRIPTOR_HEAP_TYPE::D3D12_DESCRIPTOR_HEAP_TYPE_RTV, SwapChainBufferCount);
        mDsvHeap.Init(md3dDevice.Get(), D3D12::D3D12_DESCRIPTOR_HEAP_TYPE::D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1);
    }

    //TODO
    virtual void OnResize()
    {

    }

//    virtual void Update(const GameTimer& gt) = 0;
//    virtual void Draw(const GameTimer& gt) = 0;
//
//    // override to define GUI (call once per frame). Make sure to call base implementation first.
//    virtual void UpdateImgui(const GameTimer& gt);
//
    // Convenience overrides for handling mouse input.
    virtual void OnMouseDown(WPARAM btnState, int x, int y) {}
    virtual void OnMouseUp(WPARAM btnState, int x, int y) {}
    virtual void OnMouseMove(WPARAM btnState, int x, int y) {}

protected:

    bool InitMainWindow()
    {
        Win32::WNDCLASS wc;
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

        if (!Win32::RegisterClassW(&wc))
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
//    bool InitDirect3D();
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

        DXGI::DXGI_SWAP_CHAIN_DESC1 sd;
        sd.Width = mClientWidth;
        sd.Height = mClientHeight;
        sd.Format = mBackBufferFormat;
        sd.Stereo = false;
        sd.SampleDesc.Count = 1;
        sd.SampleDesc.Quality = 0;
        sd.BufferUsage = DXGI::DxgiUsageRenderTargetOutput;
        sd.BufferCount = SwapChainBufferCount;
        sd.Scaling = DXGI::DXGI_SCALING::DXGI_SCALING_NONE;
        sd.SwapEffect = DXGI::DXGI_SWAP_EFFECT::DXGI_SWAP_EFFECT_FLIP_DISCARD;
        sd.AlphaMode = DXGI::DXGI_ALPHA_MODE::DXGI_ALPHA_MODE_UNSPECIFIED;
        sd.Flags = DXGI::DXGI_SWAP_CHAIN_FLAG::DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;

        // Note: Swap chain uses queue to perform flush.
        Microsoft::WRL::ComPtr<DXGI::IDXGISwapChain1> swapChain1;
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

    // Need to call after cbvSrvUavHeap created in derived app.
    void InitImgui(CbvSrvUavHeap& cbvSrvUavHeap)
    {
        const uint32_t imgGuiBindlessIndex = cbvSrvUavHeap.NextFreeIndex();

        // Setup Dear ImGui context
        ImGui::CheckVersion();
        auto ctx = ImGui::CreateContext();

        // Setup Dear ImGui style
        ImGui::StyleColorsDark();
        //ImGui::StyleColorsClassic();

        // Setup Platform/Renderer backends
        ImGui::ImGui_ImplWin32_Init(mhMainWnd);
        ImGui::ImGui_ImplDX12_Init(
            md3dDevice.Get(), 
            gNumFrameResources,
            mBackBufferFormat,
            cbvSrvUavHeap.GetD3dHeap(),
            cbvSrvUavHeap.CpuHandle(imgGuiBindlessIndex),
            cbvSrvUavHeap.GpuHandle(imgGuiBindlessIndex));
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
        UINT count = 0;
        UINT flags = 0;

        // Call with nullptr to get list count.
        output->GetDisplayModeList(format, flags, &count, nullptr);

        std::vector<DXGI_MODE_DESC> modeList(count);
        output->GetDisplayModeList(format, flags, &count, &modeList[0]);

        for (auto& x : modeList)
        {
            UINT n = x.RefreshRate.Numerator;
            UINT d = x.RefreshRate.Denominator;
            std::wstring text =
                L"Width = " + std::to_wstring(x.Width) + L" " +
                L"Height = " + std::to_wstring(x.Height) + L" " +
                L"Refresh = " + std::to_wstring(n) + L"/" + std::to_wstring(d) +
                L"\n";

            Win32::OutputDebugStringW(text.c_str());
        }
    }

protected:

    static D3DApp* mApp;

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
    Microsoft::WRL::ComPtr<DXGI::ID3D12Device5> md3dDevice;

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

