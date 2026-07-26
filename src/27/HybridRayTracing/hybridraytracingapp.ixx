export module hybridraytracing:hybridraytracingapp;
import std;
import shared;
import :hybridraytracer;

// Lightweight structure stores parameters to draw a shape.  This will
// vary from app-to-app.
struct RenderItem
{
    // World matrix of the shape that describes the object's local space
    // relative to the world space, which defines the position, orientation,
    // and scale of the object in the world.
    DirectX::XMFLOAT4X4 World = MathHelper::Identity4x4;

    DirectX::XMFLOAT4X4 TexTransform = MathHelper::Identity4x4;

    PerObjectCB ObjectConstants;

    // Handle to memory in linear allocator.
    DirectX::GraphicsResource MemHandleToObjectCB;

    Material* Mat = nullptr;
    MeshGeometry* Geo = nullptr;

    // Primitive topology.
    D3D12::D3D12_PRIMITIVE_TOPOLOGY PrimitiveType = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;

    // DrawIndexedInstanced parameters.
    std::uint32_t IndexCount = 0;
    std::uint32_t StartIndexLocation = 0;
    int BaseVertexLocation = 0;
};

enum class RenderLayer : int
{
    Opaque = 0,
    Debug,
    Sky,
    Count
};

//
// Define named offsets into descriptor heaps for readability.
enum RtvOffsets
{
    // Start after swapchain buffers.
    RTV_NORMALMAP = D3DApp::SwapChainBufferCount,
    RTV_COUNT
};

enum DsvOffsets
{
    DSV_MAINVIEW = 0,
    DSV_SHADOWMAP,
};

constexpr auto CBV_SRV_UAV_HEAP_CAPACITY = 16384u;

class HybridRayTracingApp : public D3DApp
{
public:
    HybridRayTracingApp(HINSTANCE hInstance);
    HybridRayTracingApp(const HybridRayTracingApp& rhs) = delete;
    HybridRayTracingApp& operator=(const HybridRayTracingApp& rhs) = delete;
    ~HybridRayTracingApp();

private:
    virtual void Initialize()override;

    virtual void CreateRtvAndDsvDescriptorHeaps()override;
    virtual void OnResize()override;
    virtual void Update(const GameTimer& gt)override;
    virtual void Draw(const GameTimer& gt)override;

    virtual void UpdateImgui(const GameTimer& gt)override;
    virtual void OnMouseDown(WPARAM btnState, int x, int y)override;
    virtual void OnMouseUp(WPARAM btnState, int x, int y)override;
    virtual void OnMouseMove(WPARAM btnState, int x, int y)override;

    void OnKeyboardInput(const GameTimer& gt);
    void AnimateMaterials(const GameTimer& gt);
    void UpdatePerObjectCB(const GameTimer& gt);
    void UpdateMaterialBuffer(const GameTimer& gt);
    void UpdateShadowTransform(const GameTimer& gt);
    void UpdateMainPassCB(const GameTimer& gt);

    void LoadTextures();
    void LoadGeometry();
    void BuildRootSignature();
    void BuildCbvSrvUavDescriptorHeap();
    void BuildShaders();
    void BuildPSOs();
    void BuildFrameResources();

    void BuildMaterials();

    void InitRayTracing();
    void AddRenderItem(RenderLayer layer, const DirectX::XMFLOAT4X4& world, const DirectX::XMFLOAT4X4& texTransform, Material* mat, MeshGeometry* geo, SubmeshGeometry& drawArgs);
    void BuildRenderItems();

    void DrawRenderItems(D3D12::ID3D12GraphicsCommandList* cmdList, const std::vector<RenderItem*>& ritems);
    void DrawNormalsAndDepth();
private:

    std::vector<std::unique_ptr<FrameResource>> mFrameResources;
    FrameResource* mCurrFrameResource = nullptr;
    int mCurrFrameResourceIndex = 0;

    Microsoft::WRL::ComPtr<D3D12::ID3D12RootSignature> mRootSignature = nullptr;

    std::unordered_map<std::string, std::unique_ptr<MeshGeometry>> mGeometries;

    std::unique_ptr<Prepass> mPrepass;

    std::uint32_t mShapeVertexBufferBindlessIndex = -1;
    std::uint32_t mShapeIndexBufferBindlessIndex = -1;
    std::uint32_t mOrbBaseVertexBufferBindlessIndex = -1;
    std::uint32_t mOrbBaseIndexBufferBindlessIndex = -1;
    std::uint32_t mColumnRoundBrokenVertexBufferBindlessIndex = -1;
    std::uint32_t mColumnRoundBrokenIndexBufferBindlessIndex = -1;
    std::uint32_t mColumnSquareBrokenVertexBufferBindlessIndex = -1;
    std::uint32_t mColumnSquareBrokenIndexBufferBindlessIndex = -1;
    std::uint32_t mColumnSquareVertexBufferBindlessIndex = -1;
    std::uint32_t mColumnSquareIndexBufferBindlessIndex = -1;

    // List of all the render items.
    std::vector<std::unique_ptr<RenderItem>> mAllRitems;

    // Render items divided by PSO.
    std::vector<RenderItem*> mRitemLayer[(int)RenderLayer::Count];

    std::unique_ptr<HybridRayTracer> mRayTracer = nullptr;

    std::uint32_t mRandomTexBindlessIndex = -1;
    std::uint32_t mSkyBindlessIndex = -1;

    D3D12::CD3DX12_GPU_DESCRIPTOR_HANDLE mNullSrv;

    PerPassCB mMainPassCB;  // index 0 of pass cbuffer.
    PerPassCB mShadowPassCB;// index 1 of pass cbuffer.

    Camera mCamera;

    DirectX::BoundingSphere mSceneBounds;

    float mLightNearZ = 0.0f;
    float mLightFarZ = 0.0f;
    DirectX::XMFLOAT3 mLightPosW;
    DirectX::XMFLOAT4X4 mLightView = MathHelper::Identity4x4;
    DirectX::XMFLOAT4X4 mLightProj = MathHelper::Identity4x4;
    DirectX::XMFLOAT4X4 mShadowTransform = MathHelper::Identity4x4;

    float mLightRotationAngle = 0.0f;
    DirectX::XMFLOAT3 mBaseLightDirections[3] = {
        DirectX::XMFLOAT3(0.57735f, -0.57735f, 0.57735f),
        DirectX::XMFLOAT3(-0.57735f, -0.57735f, 0.57735f),
        DirectX::XMFLOAT3(0.0f, -0.707f, -0.707f)
    };
    DirectX::XMFLOAT3 mRotatedLightDirections[3];

    bool mNormalMapsEnabled = true;
    bool mReflectionsEnabled = true;
    bool mShadowsEnabled = true;

    Win32::POINT mLastMousePos;
};