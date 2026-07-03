//***************************************************************************************
// d3dApp.cpp by Frank Luna (C) 2015 All Rights Reserved.
//***************************************************************************************

export module initd3d;
import shared;

export
{
    constexpr auto CBV_SRV_UAV_HEAP_CAPACITY = 16384u;

    // CBV = Constant Buffer View
    // SRV = Shader Resource View
    // UAV = Unordered Access View

    class InitDirect3DApp : public D3DApp
    {
    public:
        InitDirect3DApp(Win32::HINSTANCE hInstance)
            : D3DApp(hInstance)
        {}

        InitDirect3DApp(const InitDirect3DApp& rhs) = delete;
        InitDirect3DApp& operator=(const InitDirect3DApp& rhs) = delete;
        ~InitDirect3DApp()
        {
            if (md3dDevice != nullptr)
                FlushCommandQueue();
        }

        auto Initialize() -> bool override
        {
            if (not D3DApp::Initialize())
                return false;
            BuildCbvSrvUavDescriptorHeap();
            return true;
        }

    private:
        void CreateRtvAndDsvDescriptorHeaps()override
        {
            mRtvHeap.Init(md3dDevice.Get(), D3D12::D3D12_DESCRIPTOR_HEAP_TYPE::D3D12_DESCRIPTOR_HEAP_TYPE_RTV, SwapChainBufferCount);
            mDsvHeap.Init(md3dDevice.Get(), D3D12::D3D12_DESCRIPTOR_HEAP_TYPE::D3D12_DESCRIPTOR_HEAP_TYPE_DSV, SwapChainBufferCount);
        }
        void OnResize()override
        {
            D3DApp::OnResize();
        }
        void Update(const GameTimer& gt)override
        {

        }
        void Draw(const GameTimer& gt)override
        {
            auto& cbvSrvUavHeap = CbvSrvUavHeap::Get();

            UpdateImgui(gt);

            // Reuse the memory associated with command recording.
            // We can only reset when the associated command lists have finished execution on the GPU.
            ThrowIfFailed(mDirectCmdListAlloc->Reset());

            // A command list can be reset after it has been added to the command queue via ExecuteCommandList.
            // Reusing the command list reuses memory.
            ThrowIfFailed(mCommandList->Reset(mDirectCmdListAlloc.Get(), nullptr));

            auto descriptorHeaps = std::array{ cbvSrvUavHeap.GetD3dHeap() };
            mCommandList->SetDescriptorHeaps(1, descriptorHeaps.data());

            mCommandList->RSSetViewports(1, &mScreenViewport);
            mCommandList->RSSetScissorRects(1, &mScissorRect);

            // Indicate a state transition on the resource usage.
            auto transition = D3D12::CD3DX12_RESOURCE_BARRIER::Transition(
                CurrentBackBuffer(),
                D3D12::D3D12_RESOURCE_STATES::D3D12_RESOURCE_STATE_PRESENT,
                D3D12::D3D12_RESOURCE_STATES::D3D12_RESOURCE_STATE_RENDER_TARGET);
            mCommandList->ResourceBarrier(1, &transition);

            // Clear the back buffer and depth buffer.
            mCommandList->ClearRenderTargetView(
                CurrentBackBufferView(),
                DirectX::Colors::LightSteelBlue,
                0,
                nullptr);
            constexpr auto clearFlags = D3D12::D3D12_CLEAR_FLAGS{ D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL };
            mCommandList->ClearDepthStencilView(
                DepthStencilView(),
                clearFlags,
                1.0f,
                0,
                0,
                nullptr);

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
                D3D12::D3D12_RESOURCE_STATES::D3D12_RESOURCE_STATE_PRESENT);
            mCommandList->ResourceBarrier(1, &transition);

            // Done recording commands.
            ThrowIfFailed(mCommandList->Close());

            // Add the command list to the queue for execution.

            auto cmdsLists = std::array<D3D12::ID3D12CommandList*, 1>{ mCommandList.Get() };
            mCommandQueue->ExecuteCommandLists(static_cast<UINT>(cmdsLists.size()), cmdsLists.data());

            // Swap the back and front buffers
            auto presentParams = DXGI::DXGI_PRESENT_PARAMETERS{ 0 };
            ThrowIfFailed(mSwapChain->Present1(0, 0, &presentParams));
            mCurrBackBuffer = (mCurrBackBuffer + 1) % SwapChainBufferCount;

            // Wait until frame commands are complete.  This waiting is inefficient and is
            // done for simplicity.  Later we will show how to organize our rendering code
            // so we do not have to wait per frame.
            FlushCommandQueue();
        }

        void UpdateImgui(const GameTimer& gt)override
        {
            D3DApp::UpdateImgui(gt);

            //
            // Define a panel to render GUI elements.
            // 
            ImGui::Begin("Options");

            ImGui::Text(
                "Application average %.3f ms/frame (%.1f FPS)",
                1000.0f / ImGui::GetIO().Framerate,
                ImGui::GetIO().Framerate);

            auto gfxMemStats = DirectX::GraphicsMemoryStatistics{
                DirectX::GraphicsMemory::Get(md3dDevice.Get()).GetStatistics() };

            if (ImGui::CollapsingHeader("VideoMemoryInfo"))
            {
                static auto vidMemPollTime = 0.0f;
                vidMemPollTime += gt.DeltaTime();

                static auto videoMemInfo = DXGI::DXGI_QUERY_VIDEO_MEMORY_INFO{};
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

        void OnMouseDown(Win32::WPARAM btnState, int x, int y)override
        {
            if (ImGui::ImGuiIO& io = ImGui::GetIO(); not io.WantCaptureMouse)
            {
                mLastMousePos.x = x;
                mLastMousePos.y = y;
                Win32::SetCapture(mhMainWnd);
            }
        }
        void OnMouseUp(Win32::WPARAM btnState, int x, int y)override
        {
            if (ImGui::ImGuiIO& io = ImGui::GetIO(); not io.WantCaptureMouse)
                Win32::ReleaseCapture();
        }
        void OnMouseMove(Win32::WPARAM btnState, int x, int y)override
        {
            if (ImGui::ImGuiIO& io = ImGui::GetIO(); not io.WantCaptureMouse)
            {
                mLastMousePos.x = x;
                mLastMousePos.y = y;
            }
        }

        void BuildCbvSrvUavDescriptorHeap()
        {
            auto& cbvSrvUavHeap = CbvSrvUavHeap::Get();
            cbvSrvUavHeap.Init(md3dDevice.Get(), CBV_SRV_UAV_HEAP_CAPACITY);
            InitImgui(cbvSrvUavHeap);
        }

    private:
        Win32::POINT mLastMousePos;
    };
}