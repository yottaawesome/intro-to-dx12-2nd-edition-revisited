export module instancingandculling;
import std;
import shared;

enum GFX_ROOT_ARG
{
    GFX_ROOT_ARG_OBJECT_CBV = 0,
    GFX_ROOT_ARG_PASS_CBV,
    GFX_ROOT_ARG_SKINNED_CBV,
    GFX_ROOT_ARG_MATERIAL_SRV,
    GFX_ROOT_ARG_INSTANCEDATA_SRV,
    GFX_ROOT_ARG_COUNT
};

// Stores the resources needed for the CPU to build the command lists
// for a frame.  
struct FrameResource
{
    FrameResource(D3D12::ID3D12Device* device, std::uint32_t passCount, std::uint32_t maxInstanceCount, std::uint32_t materialCount)
    {
        auto hr = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, __uuidof(D3D12::ID3D12CommandAllocator), reinterpret_cast<void**>(CmdListAlloc.GetAddressOf()));
		if (Win32::Failed(hr))
			throw DxException{ hr };

        PassCB = std::make_unique<UploadBuffer<PerPassCB>>(device, passCount, true);
        MaterialBuffer = std::make_unique<UploadBuffer<MaterialData>>(device, materialCount, false);
        InstanceBuffer = std::make_unique<UploadBuffer<InstanceData>>(device, maxInstanceCount, false);
    }

    // We cannot reset the allocator until the GPU is done processing the commands.
    // So each frame needs their own allocator.
    Microsoft::WRL::ComPtr<D3D12::ID3D12CommandAllocator> CmdListAlloc;

    // We cannot update a buffer until the GPU is done processing the commands
    // that reference it.  So each frame needs their own buffers.
    std::unique_ptr<UploadBuffer<PerPassCB>> PassCB;
    std::unique_ptr<UploadBuffer<MaterialData>> MaterialBuffer;

    // NOTE: In this demo, we instance only one render-item, so we only have one structured buffer to 
    // store instancing data.  To make this more general (i.e., to support instancing multiple render-items), 
    // you would need to have a structured buffer for each render-item, and allocate each buffer with enough
    // room for the maximum number of instances you would ever draw.  
    // This sounds like a lot, but it is actually no more than the amount of per-object constant data we 
    // would need if we were not using instancing.  For example, if we were drawing 1000 objects without instancing,
    // we would create a constant buffer with enough room for a 1000 objects.  With instancing, we would just
    // create a structured buffer large enough to store the instance data for 1000 instances.  
    std::unique_ptr<UploadBuffer<InstanceData>> InstanceBuffer;

    // Fence value to mark commands up to this fence point.  This lets us
    // check if these frame resources are still in use by the GPU.
    std::uint64_t Fence = 0;
};

// Lightweight structure stores parameters to draw a shape.  This will
// vary from app-to-app.
struct RenderItem
{
    RenderItem() = default;
    RenderItem(const RenderItem& rhs) = delete;

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
    D3D::D3D_PRIMITIVE_TOPOLOGY PrimitiveType = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;

    DirectX::BoundingBox Bounds;
    std::vector<InstanceData> Instances;

    // DrawIndexedInstanced parameters.
    std::uint32_t IndexCount = 0;
    std::uint32_t InstanceCount = 0;
    std::uint32_t StartIndexLocation = 0;
    int BaseVertexLocation = 0;
};

enum class RenderLayer : int
{
    Opaque = 0,
    OpaqueInstanced,
    Debug,
    Sky,
    Count
};

export class InstancingAndCullingApp : public D3DApp
{
public:
    InstancingAndCullingApp(HINSTANCE hInstance);
    InstancingAndCullingApp(const InstancingAndCullingApp& rhs) = delete;
    InstancingAndCullingApp& operator=(const InstancingAndCullingApp& rhs) = delete;
    ~InstancingAndCullingApp();

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
    void UpdateInstanceData(const GameTimer& gt);
    void UpdateMaterialBuffer(const GameTimer& gt);
    void UpdateMainPassCB(const GameTimer& gt);

    void LoadTextures();
    void BuildRootSignature();
    void BuildCbvSrvUavDescriptorHeap();
    void BuildShadersAndInputLayout();
    void BuildPSOs();
    void BuildFrameResources();

    void BuildMaterials();

    auto AddRenderItem(
        RenderLayer layer, 
        const DirectX::XMFLOAT4X4& world, 
        const DirectX::XMFLOAT4X4& texTransform, 
        Material* mat, 
        MeshGeometry* geo, 
        SubmeshGeometry& drawArgs
    ) ->RenderItem*;
    void BuildRenderItems();

    void DrawRenderItems(D3D12::ID3D12GraphicsCommandList* cmdList, const std::vector<RenderItem*>& ritems);
    void DrawInstancedRenderItems(D3D12::ID3D12GraphicsCommandList* cmdList, const std::vector<RenderItem*>& ritems);

private:

    std::vector<std::unique_ptr<FrameResource>> mFrameResources;
    FrameResource* mCurrFrameResource = nullptr;
    int mCurrFrameResourceIndex = 0;

    Microsoft::WRL::ComPtr<ID3D12RootSignature> mRootSignature = nullptr;

    std::unordered_map<std::string, std::unique_ptr<MeshGeometry>> mGeometries;

    std::vector<D3D12::D3D12_INPUT_ELEMENT_DESC> mInputLayout;

    // List of all the render items.
    std::vector<std::unique_ptr<RenderItem>> mAllRitems;

    // Render items divided by PSO.
    std::vector<RenderItem*> mRitemLayer[(int)RenderLayer::Count];

    std::uint32_t mRandomTexBindlessIndex = -1;
    std::uint32_t mSkyBindlessIndex = -1;

    std::uint32_t mNullCubeSrvIndex = 0;
    std::uint32_t mNullTexSrvIndex = 0;

    D3D12::CD3DX12_GPU_DESCRIPTOR_HANDLE mNullSrv;

    PerPassCB mMainPassCB;  // index 0 of pass cbuffer.

    Camera mCamera;

    static constexpr int InstanceGridSize = 15;
    static constexpr int MaxInstanceCount = InstanceGridSize * InstanceGridSize * InstanceGridSize;

    float mLightNearZ = 0.0f;
    float mLightFarZ = 0.0f;
    DirectX::XMFLOAT3 mLightPosW;
    DirectX::XMFLOAT4X4 mLightView = MathHelper::Identity4x4;
    DirectX::XMFLOAT4X4 mLightProj = MathHelper::Identity4x4;

    float mLightRotationAngle = 0.0f;
    DirectX::XMFLOAT3 mBaseLightDirections[3] = {
        DirectX::XMFLOAT3(0.57735f, -0.57735f, 0.57735f),
        DirectX::XMFLOAT3(-0.57735f, -0.57735f, 0.57735f),
        DirectX::XMFLOAT3(0.0f, -0.707f, -0.707f)
    };
    DirectX::XMFLOAT3 mRotatedLightDirections[3];

    DirectX::BoundingFrustum mCamFrustum;

    bool mDrawWireframe = false;
    bool mFrustumCullingEnabled = true;
    bool mNormalMapsEnabled = false;
    bool mReflectionsEnabled = false;

    Win32::POINT mLastMousePos;
};
