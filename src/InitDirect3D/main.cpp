import shared;

// Required exports for DX12-Agility SDK
// https://devblogs.microsoft.com/directx/gettingstarted-dx12agility/
extern "C" { __declspec(dllexport) extern const Win32::UINT D3D12SDKVersion = 614; }
extern "C" { __declspec(dllexport) extern const char* D3D12SDKPath = ".\\D3D12\\"; }

#pragma comment(lib,"d3dcompiler.lib")
#pragma comment(lib, "D3D12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dxcompiler.lib") // dxc

constexpr auto CBV_SRV_UAV_HEAP_CAPACITY = 16384u;

// CBV = Constant Buffer View
// SRV = Shader Resource View
// UAV = Unordered Access View

class InitDirect3DApp : public D3DApp
{
public:
    InitDirect3DApp(Win32::HINSTANCE hInstance)
        : D3DApp(hInstance)
    { }

    InitDirect3DApp(const InitDirect3DApp& rhs) = delete;
    InitDirect3DApp& operator=(const InitDirect3DApp& rhs) = delete;
    ~InitDirect3DApp()
    {
        if (md3dDevice != nullptr)
            FlushCommandQueue();
    }

    virtual auto Initialize() -> bool override
    {
        if (!D3DApp::Initialize())
            return false;

        BuildCbvSrvUavDescriptorHeap();

        return true;
    }

private:
    virtual void CreateRtvAndDsvDescriptorHeaps()override
    {
        mRtvHeap.Init(md3dDevice.Get(), D3D12::D3D12_DESCRIPTOR_HEAP_TYPE::D3D12_DESCRIPTOR_HEAP_TYPE_RTV, SwapChainBufferCount);
        mDsvHeap.Init(md3dDevice.Get(), D3D12::D3D12_DESCRIPTOR_HEAP_TYPE::D3D12_DESCRIPTOR_HEAP_TYPE_DSV, SwapChainBufferCount);
    }
    virtual void OnResize()override
    {
        D3DApp::OnResize();
    }
    virtual void Update(const GameTimer& gt)override
    {

    }
    virtual void Draw(const GameTimer& gt)override
    {
        CbvSrvUavHeap& cbvSrvUavHeap = CbvSrvUavHeap::Get();

        UpdateImgui(gt);

        // Reuse the memory associated with command recording.
        // We can only reset when the associated command lists have finished execution on the GPU.
        ThrowIfFailed(mDirectCmdListAlloc->Reset());

        // A command list can be reset after it has been added to the command queue via ExecuteCommandList.
        // Reusing the command list reuses memory.
        ThrowIfFailed(mCommandList->Reset(mDirectCmdListAlloc.Get(), nullptr));

        D3D12::ID3D12DescriptorHeap* descriptorHeaps[] = { cbvSrvUavHeap.GetD3dHeap() };
        mCommandList->SetDescriptorHeaps(1, descriptorHeaps);

        mCommandList->RSSetViewports(1, &mScreenViewport);
        mCommandList->RSSetScissorRects(1, &mScissorRect);

        // Indicate a state transition on the resource usage.
        auto transition = D3D12::CD3DX12_RESOURCE_BARRIER::Transition(
            CurrentBackBuffer(),
            D3D12::D3D12_RESOURCE_STATES::D3D12_RESOURCE_STATE_PRESENT,
            D3D12::D3D12_RESOURCE_STATES::D3D12_RESOURCE_STATE_RENDER_TARGET
        );
        mCommandList->ResourceBarrier(1, &transition);

        // Clear the back buffer and depth buffer.
        mCommandList->ClearRenderTargetView(
            CurrentBackBufferView(),
            DirectX::Colors::LightSteelBlue,
            0, nullptr);
        mCommandList->ClearDepthStencilView(
            DepthStencilView(),
            static_cast<D3D12::D3D12_CLEAR_FLAGS>(D3D12::D3D12_CLEAR_FLAGS::D3D12_CLEAR_FLAG_DEPTH | D3D12::D3D12_CLEAR_FLAGS::D3D12_CLEAR_FLAG_STENCIL),
            1.0f, 0, 0, nullptr);

        // Specify the buffers we are going to render to.
        auto cbbv = CurrentBackBufferView();
        auto dsv = DepthStencilView();
        mCommandList->OMSetRenderTargets(1, &cbbv, true, &dsv);

        // Draw imgui UI.
        ImGui::ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), mCommandList.Get());

        // Indicate a state transition on the resource usage.

        transition = D3D12::CD3DX12_RESOURCE_BARRIER::Transition(
            CurrentBackBuffer(),
            D3D12::D3D12_RESOURCE_STATES::D3D12_RESOURCE_STATE_RENDER_TARGET, 
            D3D12::D3D12_RESOURCE_STATES::D3D12_RESOURCE_STATE_PRESENT
        );
        mCommandList->ResourceBarrier(1, &transition);

        // Done recording commands.
        ThrowIfFailed(mCommandList->Close());

        // Add the command list to the queue for execution.
        D3D12::ID3D12CommandList* cmdsLists[] = { mCommandList.Get() };
        mCommandQueue->ExecuteCommandLists(1, cmdsLists);

        // Swap the back and front buffers
        DXGI::DXGI_PRESENT_PARAMETERS presentParams = { 0 };
        ThrowIfFailed(mSwapChain->Present1(0, 0, &presentParams));
        mCurrBackBuffer = (mCurrBackBuffer + 1) % SwapChainBufferCount;

        // Wait until frame commands are complete.  This waiting is inefficient and is
        // done for simplicity.  Later we will show how to organize our rendering code
        // so we do not have to wait per frame.
        FlushCommandQueue();
    }

    virtual void UpdateImgui(const GameTimer& gt)override
    {
        D3DApp::UpdateImgui(gt);

        //
        // Define a panel to render GUI elements.
        // 
        ImGui::Begin("Options");

        ImGui::Text("Application average %.3f ms/frame (%.1f FPS)",
            1000.0f / ImGui::GetIO().Framerate, ImGui::GetIO().Framerate);

        DirectX::GraphicsMemoryStatistics gfxMemStats = DirectX::GraphicsMemory::Get(
            md3dDevice.Get()).GetStatistics();

        if (ImGui::CollapsingHeader("VideoMemoryInfo"))
        {
            static float vidMemPollTime = 0.0f;
            vidMemPollTime += gt.DeltaTime();

            static DXGI::DXGI_QUERY_VIDEO_MEMORY_INFO videoMemInfo;
            if (vidMemPollTime >= 1.0f) // poll every second
            {
                mDefaultAdapter->QueryVideoMemoryInfo(
                    0, // assume single GPU
                    DXGI::DXGI_MEMORY_SEGMENT_GROUP::DXGI_MEMORY_SEGMENT_GROUP_LOCAL, // interested in local GPU memory, not shared
                    &videoMemInfo);

                vidMemPollTime -= 1.0f;
            }

            ImGui::Text("Budget (bytes): %u", videoMemInfo.Budget);
            ImGui::Text("CurrentUsage (bytes): %u", videoMemInfo.CurrentUsage);
            ImGui::Text("AvailableForReservation (bytes): %u", videoMemInfo.AvailableForReservation);
            ImGui::Text("CurrentReservation (bytes): %u", videoMemInfo.CurrentReservation);

        }
        if (ImGui::CollapsingHeader("GraphicsMemoryStatistics"))
        {
            ImGui::Text("Bytes of memory in-flight: %u", gfxMemStats.committedMemory);
            ImGui::Text("Total bytes used: %u", gfxMemStats.totalMemory);
            ImGui::Text("Total page count: %u", gfxMemStats.totalPages);
        }

        ImGui::End();

        ImGui::Render();
    }

    virtual void OnMouseDown(Win32::WPARAM btnState, int x, int y)override
    {
        ImGui::ImGuiIO& io = ImGui::GetIO();

        if (!io.WantCaptureMouse)
        {
            mLastMousePos.x = x;
            mLastMousePos.y = y;

            Win32::SetCapture(mhMainWnd);
        }
    }
    virtual void OnMouseUp(Win32::WPARAM btnState, int x, int y)override
    {
        ImGui::ImGuiIO& io = ImGui::GetIO();

        if (!io.WantCaptureMouse)
        {
            Win32::ReleaseCapture();
        }
    }
    virtual void OnMouseMove(Win32::WPARAM btnState, int x, int y)override
    {
        ImGui::ImGuiIO& io = ImGui::GetIO();

        if (!io.WantCaptureMouse)
        {
            mLastMousePos.x = x;
            mLastMousePos.y = y;
        }
    }

    void BuildCbvSrvUavDescriptorHeap()
    {
        CbvSrvUavHeap& cbvSrvUavHeap = CbvSrvUavHeap::Get();
        cbvSrvUavHeap.Init(md3dDevice.Get(), CBV_SRV_UAV_HEAP_CAPACITY);

        InitImgui(cbvSrvUavHeap);
    }

private:

    Win32::POINT mLastMousePos;
};

auto wWinMain(
    Win32::HINSTANCE hInstance,
    Win32::HINSTANCE hPrevInstance,
    Win32::LPWSTR    lpCmdLine,
    int       nCmdShow
) -> int
{
    // Enable run-time memory check for debug builds.
#if defined(DEBUG) | defined(_DEBUG)
    Win32::_CrtSetDbgFlag(Win32::CrtAllocMemDf | Win32::CrtLeakCheckDf);
#endif

    try
    {
        auto theApp = InitDirect3DApp{ hInstance };
        if (!theApp.Initialize())
            return 0;

        return theApp.Run();
    }
    catch (DxException& e)
    {
        Win32::MessageBoxW(nullptr, e.ToString().c_str(), L"HR Failed", Win32::MbOk);
        return 0;
    }
}
