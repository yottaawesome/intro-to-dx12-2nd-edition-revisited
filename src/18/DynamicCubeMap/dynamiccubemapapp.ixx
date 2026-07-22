export module dynamiccubemap:dynamiccubemapapp;
import std;
import shared;
import :frameresource;
import :cuberendertarget;

constexpr auto CubeMapSize = 512u;
constexpr auto OffsetToCubeFace0 = 1;

//
// Define named offsets into descriptor heaps for readability.
enum RtvOffsets
{
    // Start after swapchain buffers.
    RTV_CUBE_FACE0 = D3DApp::SwapChainBufferCount,
    RTV_CUBE_FACE1,
    RTV_CUBE_FACE2,
    RTV_CUBE_FACE3,
    RTV_CUBE_FACE4,
    RTV_CUBE_FACE5,
    RTV_COUNT
};

enum DsvOffsets
{
    DSV_MAINVIEW = 0,
    DSV_DYNAMIC_CUBEMAP,
    DSV_COUNT
};

constexpr auto CBV_SRV_UAV_HEAP_CAPACITY = 16384u;

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
    D3D12::D3D12_PRIMITIVE_TOPOLOGY PrimitiveType = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;

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

export class DynamicCubeMap : public D3DApp
{
public:
    DynamicCubeMap(HINSTANCE hInstance);
    DynamicCubeMap(const DynamicCubeMap& rhs) = delete;
    DynamicCubeMap& operator=(const DynamicCubeMap& rhs) = delete;
    ~DynamicCubeMap();

    virtual void Initialize()override;

private:
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
    void UpdatePerObjectCB(const GameTimer& gt, bool cubeMapPass);
    void UpdateMaterialBuffer(const GameTimer& gt);
    void UpdateMainPassCB(const GameTimer& gt);
    void UpdateCubeMapFacePassCBs();

    void LoadTextures();
    void LoadGeometry();
    void BuildRootSignature();
    void BuildCbvSrvUavDescriptorHeap();
    void BuildCubeDepthStencil();
    void BuildShadersAndInputLayout();
    void BuildPSOs();
    void BuildFrameResources();

    void BuildMaterials();

    RenderItem* AddRenderItem(RenderLayer layer, const DirectX::XMFLOAT4X4& world, const DirectX::XMFLOAT4X4& texTransform, Material* mat, MeshGeometry* geo, SubmeshGeometry& drawArgs);
    void BuildRenderItems();

    void DrawRenderItems(D3D12::ID3D12GraphicsCommandList* cmdList, const std::vector<RenderItem*>& ritems);

    void DrawSceneToCubeMap();
    void BuildCubeFaceCamera(float x, float y, float z);
private:
    std::vector<std::unique_ptr<FrameResource>> mFrameResources;
    FrameResource* mCurrFrameResource = nullptr;
    int mCurrFrameResourceIndex = 0;

    Microsoft::WRL::ComPtr<D3D12::ID3D12RootSignature> mRootSignature = nullptr;
    Microsoft::WRL::ComPtr<D3D12::ID3D12RootSignature> mComputeRootSignature = nullptr;

    std::unordered_map<std::string, std::unique_ptr<MeshGeometry>> mGeometries;

    std::vector<D3D12::D3D12_INPUT_ELEMENT_DESC> mInputLayout;

    // List of all the render items.
    std::vector<std::unique_ptr<RenderItem>> mAllRitems;

    // Render items divided by PSO.
    std::vector<RenderItem*> mRitemLayer[(int)RenderLayer::Count];

    uint32_t mRandomTexBindlessIndex = -1;
    uint32_t mSkyBindlessIndex = -1;

    RenderItem* mSkullRitem = nullptr;

    std::unique_ptr<CubeRenderTarget> mDynamicCubeMap = nullptr;

    Microsoft::WRL::ComPtr<D3D12::ID3D12Resource> mCubeDepthStencilBuffer;
    D3D12::CD3DX12_CPU_DESCRIPTOR_HANDLE mCubeDSV;

    PerPassCB mMainPassCB;

    Camera mCamera;
    Camera mCubeMapCamera[6];

    float mLightRotationAngle = 0.0f;
    DirectX::XMFLOAT3 mBaseLightDirections[3] = {
        DirectX::XMFLOAT3(0.57735f, -0.57735f, 0.57735f),
        DirectX::XMFLOAT3(-0.57735f, -0.57735f, 0.57735f),
        DirectX::XMFLOAT3(0.0f, -0.707f, -0.707f)
    };
    DirectX::XMFLOAT3 mRotatedLightDirections[3];

    bool mDrawWireframe = false;
    bool mNormalMapsEnabled = true;
    bool mReflectionsEnabled = true;

    Win32::POINT mLastMousePos;
};
