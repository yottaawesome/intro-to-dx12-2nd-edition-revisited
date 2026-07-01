export module shared:d3dapp;
import std;
import :win32;
import :gametimer;

#if false
class D3DApp
{
protected:

    D3DApp(Win32::HINSTANCE hInstance);
    D3DApp(const D3DApp& rhs) = delete;
    D3DApp& operator=(const D3DApp& rhs) = delete;
    virtual ~D3DApp();

public:

    static D3DApp* GetApp();

    Win32::HINSTANCE AppInst()const;
    Win32::HWND      MainWnd()const;
    float     AspectRatio()const;

    int Run();

    void FlushCommandQueue();

    virtual bool Initialize();
    virtual Win32::LRESULT MsgProc(
        Win32::HWND hwnd, 
        Win32::UINT msg, 
        Win32::WPARAM wParam,
        Win32::LPARAM lParam
    );

    static const int SwapChainBufferCount = 2;

protected:
    virtual void CreateRtvAndDsvDescriptorHeaps();
    virtual void OnResize();
    virtual void Update(const GameTimer& gt) = 0;
    virtual void Draw(const GameTimer& gt) = 0;

    // override to define GUI (call once per frame). Make sure to call base implementation first.
    virtual void UpdateImgui(const GameTimer& gt);

    // Convenience overrides for handling mouse input.
    virtual void OnMouseDown(WPARAM btnState, int x, int y) {}
    virtual void OnMouseUp(WPARAM btnState, int x, int y) {}
    virtual void OnMouseMove(WPARAM btnState, int x, int y) {}

protected:

    bool InitMainWindow();
    bool InitDirect3D();
    void CreateCommandObjects();
    void CreateSwapChain();

    D3D12::ID3D12Resource* CurrentBackBuffer()const;
    D3D12::CD3DX12_CPU_DESCRIPTOR_HANDLE CurrentBackBufferView();
    D3D12::CD3DX12_CPU_DESCRIPTOR_HANDLE DepthStencilView();

    // Need to call after cbvSrvUavHeap created in derived app.
    void InitImgui(CbvSrvUavHeap& cbvSrvUavHeap);
    void ShutdownImgui();

    void CalculateFrameStats();

    void LogAdapters();
    void LogAdapterOutputs(DXGI::IDXGIAdapter* adapter);
    void LogOutputDisplayModes(DXGI::IDXGIOutput* output, DXGI::DXGI_FORMAT format);

protected:

    static D3DApp* mApp;

    HINSTANCE mhAppInst = nullptr; // application instance handle
    HWND      mhMainWnd = nullptr; // main window handle
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
    UINT64 mCurrentFence = 0;

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
    DXGI_FORMAT mBackBufferFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
    DXGI_FORMAT mDepthStencilFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
    int mClientWidth = 1280;
    int mClientHeight = 720;
};
#endif
