export module ssaodemo:ssao;
import std;
import shared;
import :frameresource;

class Ssao
{
public:
    Ssao(D3D12::ID3D12Device* device, std::uint32_t width, std::uint32_t height)
    {
        md3dDevice = device;

        BuildOffsetVectors();

        mBlurWeights = d3dUtil::CalcGaussWeights(2.5f);

        UpdateSize(width, height);

        BuildResources();
    }

    Ssao(const Ssao& rhs) = delete;
    auto operator=(const Ssao& rhs) -> Ssao& = delete;

    static constexpr int MaxBlurRadius = 5;

    auto GetOcclusionRadius() const -> float
    {
        return mOcclusionRadius;
    }

    auto GetOcclusionFadeStart() const -> float
    {
        return mOcclusionFadeStart;
    }

    auto GetOcclusionFadeEnd() const -> float
    {
        return mOcclusionFadeEnd;
    }

    auto GetSurfaceEpsilon() const -> float
    {
        return mSurfaceEpsilon;
    }

    void SetOcclusionRadius(float value)
    {
        if (mOcclusionRadius != value)
        {
            mOcclusionRadius = value;
            mSsaoConstantsDirty = true;
        }
    }

    void SetOcclusionFadeStart(float value)
    {
        if (mOcclusionFadeStart != value)
        {
            mOcclusionFadeStart = value;
            mSsaoConstantsDirty = true;
        }
    }

    void SetOcclusionFadeEnd(float value)
    {
        if (mOcclusionFadeEnd != value)
        {
            mOcclusionFadeEnd = value;
            mSsaoConstantsDirty = true;
        }
    }
    void SetSurfaceEpsilon(float value)
    {
        if (mSurfaceEpsilon != value)
        {
            mSurfaceEpsilon = value;
            mSsaoConstantsDirty = true;
        }
    }

    auto SsaoMapWidth() const -> std::uint32_t
    {
        return mRenderTargetWidth / 2;
    }

    auto SsaoMapHeight() const -> std::uint32_t
    {
        return mRenderTargetHeight / 2;
    }

    auto NormalMap() -> D3D12::ID3D12Resource*
    {
        return mNormalMap.Get();
    }

    auto NormalMapRtv() const -> D3D12::CD3DX12_CPU_DESCRIPTOR_HANDLE
    {
        return mhNormalMapCpuRtv;
    }

    auto GetNormalMapBindlessIndex() const -> std::uint32_t
    {
        return mNormalMapBindlessIndex;
    }
    
    auto GetAmbientMap0BindlessIndex() const -> std::uint32_t
    {
        return mAmbientMap0BindlessIndex;
    }

    auto GetAmbientMap1BindlessIndex() const -> std::uint32_t
    {
        return mAmbientMap1BindlessIndex;
    }

    void BuildDescriptors(
        D3D12::CD3DX12_CPU_DESCRIPTOR_HANDLE hNormalMapCpuRtv,
        D3D12::CD3DX12_CPU_DESCRIPTOR_HANDLE hAmbientMap0CpuRtv,
        D3D12::CD3DX12_CPU_DESCRIPTOR_HANDLE hAmbientMap1CpuRtv
    )
    {
        // RTVs are in a separate RTV heap.
        mhNormalMapCpuRtv = hNormalMapCpuRtv;
        mhAmbientMap0CpuRtv = hAmbientMap0CpuRtv;
        mhAmbientMap1CpuRtv = hAmbientMap1CpuRtv;

        auto& bindlessHeap = CbvSrvUavHeap::Get();
        mNormalMapBindlessIndex = bindlessHeap.NextFreeIndex();
        mhNormalMapCpuSrv = bindlessHeap.CpuHandle(mNormalMapBindlessIndex);
        mhNormalMapGpuSrv = bindlessHeap.GpuHandle(mNormalMapBindlessIndex);

        mAmbientMap0BindlessIndex = bindlessHeap.NextFreeIndex();
        mhAmbientMap0CpuSrv = bindlessHeap.CpuHandle(mAmbientMap0BindlessIndex);
        mhAmbientMap0GpuSrv = bindlessHeap.GpuHandle(mAmbientMap0BindlessIndex);

        mAmbientMap1BindlessIndex = bindlessHeap.NextFreeIndex();
        mhAmbientMap1CpuSrv = bindlessHeap.CpuHandle(mAmbientMap1BindlessIndex);
        mhAmbientMap1GpuSrv = bindlessHeap.GpuHandle(mAmbientMap1BindlessIndex);

        // Create the descriptors
        BuildDescriptors();
    }

    void OnResize(std::uint32_t newWidth, std::uint32_t newHeight)
    {
        if (mRenderTargetWidth != newWidth or mRenderTargetHeight != newHeight)
        {
            UpdateSize(newWidth, newHeight);
            BuildResources();
            BuildDescriptors();
        }
    }

    ///<summary>
    /// Changes the render target to the Ambient render target and draws a fullscreen
    /// quad to kick off the pixel shader to compute the AmbientMap.  We still keep the
    /// main depth buffer bound to the pipeline, but depth buffer read/writes
    /// are disabled, as we do not need the depth buffer when computing the Ambient map.
    ///</summary>
    void ComputeSsao(D3D12::ID3D12GraphicsCommandList* cmdList, D3D12::ID3D12PipelineState* ssaoPso)
    {
        cmdList->RSSetViewports(1, &mViewport);
        cmdList->RSSetScissorRects(1, &mScissorRect);

        if (mSsaoConstantsDirty)
        {
            UpdateConstants();

            // Allocate new memory.
            auto& linearAllocator = DirectX::GraphicsMemory::Get(md3dDevice);
            mMemHandleSsaoHorzCB = linearAllocator.AllocateConstant(mSsaoHorzConstants);
            mMemHandleSsaoVertCB = linearAllocator.AllocateConstant(mSsaoVertConstants);
        }

        // For SSAO pass, we can use either cbuffer since this shader does not use gHorzBlur. 
        cmdList->SetGraphicsRootConstantBufferView(GFX_ROOT_ARG_OBJECT_CBV, mMemHandleSsaoHorzCB.GpuAddress());

        // We compute the initial SSAO to AmbientMap0.

        // Change to RENDER_TARGET.
        auto transition = D3D12::CD3DX12_RESOURCE_BARRIER::Transition(mAmbientMap0.Get(),
            D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_STATE_RENDER_TARGET);
        cmdList->ResourceBarrier(1, &transition);

        auto clearValue = std::array{ 1.0f, 1.0f, 1.0f, 1.0f };
        cmdList->ClearRenderTargetView(mhAmbientMap0CpuRtv, clearValue.data(), 0, nullptr);

        // Specify the buffers we are going to render to.
        cmdList->OMSetRenderTargets(1, &mhAmbientMap0CpuRtv, true, nullptr);

        cmdList->SetPipelineState(ssaoPso);

        // Draw fullscreen quad.
        cmdList->IASetVertexBuffers(0, 0, nullptr);
        cmdList->IASetIndexBuffer(nullptr);
        cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        cmdList->DrawInstanced(6, 1, 0, 0);

        // Change back to GENERIC_READ so we can read the texture in a shader.
        transition = CD3DX12_RESOURCE_BARRIER::Transition(mAmbientMap0.Get(),
            D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_GENERIC_READ);
        cmdList->ResourceBarrier(1, &transition);
    }

    ///<summary>
    /// Blurs the ambient map to smooth out the noise caused by only taking a
    /// few random samples per pixel.  We use an edge preserving blur so that 
    /// we do not blur across discontinuities--we want edges to remain edges.
    ///</summary>
    void BlurAmbientMap(D3D12::ID3D12GraphicsCommandList* cmdList, D3D12::ID3D12PipelineState* ssaoBlurPso, int blurCount)
    {
        cmdList->SetPipelineState(ssaoBlurPso);

        for (int i = 0; i < blurCount; ++i)
        {
            BlurAmbientMap(cmdList, true);
            BlurAmbientMap(cmdList, false);
        }
    }
private:

    void BlurAmbientMap(D3D12::ID3D12GraphicsCommandList* cmdList, bool horzBlur)
    {
        auto output = static_cast<D3D12::ID3D12Resource*>(nullptr);
        auto outputRtv = D3D12::CD3DX12_CPU_DESCRIPTOR_HANDLE{};

        // Ping-pong the two ambient map textures as we apply
        // horizontal and vertical blur passes.
        if (horzBlur == true)
        {
            output = mAmbientMap1.Get();
            outputRtv = mhAmbientMap1CpuRtv;
            cmdList->SetGraphicsRootConstantBufferView(GFX_ROOT_ARG_OBJECT_CBV, mMemHandleSsaoHorzCB.GpuAddress());
        }
        else
        {
            output = mAmbientMap0.Get();
            outputRtv = mhAmbientMap0CpuRtv;
            cmdList->SetGraphicsRootConstantBufferView(GFX_ROOT_ARG_OBJECT_CBV, mMemHandleSsaoVertCB.GpuAddress());
        }

        auto transition = D3D12::CD3DX12_RESOURCE_BARRIER::Transition(output,
            D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_STATE_RENDER_TARGET);
        cmdList->ResourceBarrier(1, &transition);

        auto clearValue = std::array{ 1.0f, 1.0f, 1.0f, 1.0f };
        cmdList->ClearRenderTargetView(outputRtv, clearValue.data(), 0, nullptr);

        cmdList->OMSetRenderTargets(1, &outputRtv, true, nullptr);

        // Draw fullscreen quad.
        cmdList->IASetVertexBuffers(0, 0, nullptr);
        cmdList->IASetIndexBuffer(nullptr);
        cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        cmdList->DrawInstanced(6, 1, 0, 0);

        transition = D3D12::CD3DX12_RESOURCE_BARRIER::Transition(output,
            D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_GENERIC_READ);
        cmdList->ResourceBarrier(1, &transition);
    }

    void UpdateSize(std::uint32_t width, std::uint32_t height)
    {
        mRenderTargetWidth = width;
        mRenderTargetHeight = height;

        // We render to ambient map at half the resolution.
        mViewport.TopLeftX = 0.0f;
        mViewport.TopLeftY = 0.0f;
        mViewport.Width = mRenderTargetWidth / 2.0f;
        mViewport.Height = mRenderTargetHeight / 2.0f;
        mViewport.MinDepth = 0.0f;
        mViewport.MaxDepth = 1.0f;

        mScissorRect = { 0, 0, (int)mRenderTargetWidth / 2, (int)mRenderTargetHeight / 2 };
    }

    void UpdateConstants()
    {
        std::copy(&mOffsets[0], &mOffsets[14], &mSsaoHorzConstants.gOffsetVectors[0]);

        mSsaoHorzConstants.gBlurWeights[0] = DirectX::XMFLOAT4(&mBlurWeights[0]);
        mSsaoHorzConstants.gBlurWeights[1] = DirectX::XMFLOAT4(&mBlurWeights[4]);
        mSsaoHorzConstants.gBlurWeights[2] = DirectX::XMFLOAT4(&mBlurWeights[8]);

        // Coordinates given in view space.
        mSsaoHorzConstants.gOcclusionRadius = mOcclusionRadius;
        mSsaoHorzConstants.gOcclusionFadeStart = mOcclusionFadeStart;
        mSsaoHorzConstants.gOcclusionFadeEnd = mOcclusionFadeEnd;
        mSsaoHorzConstants.gSurfaceEpsilon = mSurfaceEpsilon;

        const float ambientMapWidth = static_cast<float>(mAmbientMap0->GetDesc().Width);
        const float ambientMapHeight = static_cast<float>(mAmbientMap0->GetDesc().Height);
        mSsaoHorzConstants.gInvAmbientMapSize = DirectX::XMFLOAT2(1.0f / ambientMapWidth, 1.0f / ambientMapHeight);

        mSsaoHorzConstants.gHorzBlur = 1;

        mSsaoVertConstants = mSsaoHorzConstants;
        mSsaoVertConstants.gHorzBlur = 0;
    }

    void BuildDescriptors()
    {
        auto srvDesc = D3D12::D3D12_SHADER_RESOURCE_VIEW_DESC{
            .Format = SceneNormalMapFormat,
            .ViewDimension = D3D12::D3D12_SRV_DIMENSION::D3D12_SRV_DIMENSION_TEXTURE2D,
            .Shader4ComponentMapping = D3D12::DefaultShader4ComponentMapping,
            .Texture2D = { .MostDetailedMip = 0,.MipLevels = 1 }
        };
        
        md3dDevice->CreateShaderResourceView(mNormalMap.Get(), &srvDesc, mhNormalMapCpuSrv);

        srvDesc.Format = SsaoAmbientMapFormat;
        md3dDevice->CreateShaderResourceView(mAmbientMap0.Get(), &srvDesc, mhAmbientMap0CpuSrv);
        md3dDevice->CreateShaderResourceView(mAmbientMap1.Get(), &srvDesc, mhAmbientMap1CpuSrv);

        auto rtvDesc = D3D12::D3D12_RENDER_TARGET_VIEW_DESC{
            .Format = SceneNormalMapFormat,
            .ViewDimension = D3D12::D3D12_RTV_DIMENSION::D3D12_RTV_DIMENSION_TEXTURE2D,
            .Texture2D = { .MipSlice = 0, .PlaneSlice = 0 }
        };
        
        md3dDevice->CreateRenderTargetView(mNormalMap.Get(), &rtvDesc, mhNormalMapCpuRtv);

        rtvDesc.Format = SsaoAmbientMapFormat;
        md3dDevice->CreateRenderTargetView(mAmbientMap0.Get(), &rtvDesc, mhAmbientMap0CpuRtv);
        md3dDevice->CreateRenderTargetView(mAmbientMap1.Get(), &rtvDesc, mhAmbientMap1CpuRtv);
    }

    void BuildResources()
    {
        // Free the old resources if they exist.
        mNormalMap = nullptr;
        mAmbientMap0 = nullptr;
        mAmbientMap1 = nullptr;

        auto texDesc = D3D12::D3D12_RESOURCE_DESC{
            .Dimension = D3D12::D3D12_RESOURCE_DIMENSION::D3D12_RESOURCE_DIMENSION_TEXTURE2D,
            .Alignment = 0,
            .Width = mRenderTargetWidth,
            .Height = mRenderTargetHeight,
            .DepthOrArraySize = 1,
            .MipLevels = 1,
            .Format = SceneNormalMapFormat,
            .SampleDesc = {.Count = 1, .Quality = 0},
            .Layout = D3D12::D3D12_TEXTURE_LAYOUT::D3D12_TEXTURE_LAYOUT_UNKNOWN,
            .Flags = D3D12::D3D12_RESOURCE_FLAGS::D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
        };


        auto normalClearColor = std::array{ 0.0f, 0.0f, 1.0f, 0.0f };
        auto optClear = D3D12::CD3DX12_CLEAR_VALUE{ SceneNormalMapFormat, normalClearColor.data()};
		auto heapProps = D3D12::CD3DX12_HEAP_PROPERTIES(D3D12::D3D12_HEAP_TYPE::D3D12_HEAP_TYPE_DEFAULT);
        ThrowIfFailed(md3dDevice->CreateCommittedResource(
            &heapProps,
            D3D12::D3D12_HEAP_FLAGS::D3D12_HEAP_FLAG_NONE,
            &texDesc,
            D3D12::D3D12_RESOURCE_STATES::D3D12_RESOURCE_STATE_GENERIC_READ,
            &optClear,
            __uuidof(D3D12::ID3D12Resource),
            &mNormalMap));

        // Ambient occlusion maps are at half resolution.
        texDesc.Width = mRenderTargetWidth / 2;
        texDesc.Height = mRenderTargetHeight / 2;
        texDesc.Format = SsaoAmbientMapFormat;

        auto ambientClearColor = std::array{ 1.0f, 1.0f, 1.0f, 1.0f };
        optClear = D3D12::CD3DX12_CLEAR_VALUE(SsaoAmbientMapFormat, ambientClearColor.data());

        ThrowIfFailed(md3dDevice->CreateCommittedResource(
            &heapProps,
            D3D12_HEAP_FLAG_NONE,
            &texDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ,
            &optClear,
            __uuidof(D3D12::ID3D12Resource),
            &mAmbientMap0));

        ThrowIfFailed(md3dDevice->CreateCommittedResource(
            &heapProps,
            D3D12_HEAP_FLAG_NONE,
            &texDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ,
            &optClear,
            __uuidof(D3D12::ID3D12Resource),
            &mAmbientMap1));
    }

    void BuildOffsetVectors()
    {
        // Start with 14 uniformly distributed vectors.  We choose the 8 corners of the cube
        // and the 6 center points along each cube face.  We always alternate the points on 
        // opposites sides of the cubes.  This way we still get the vectors spread out even
        // if we choose to use less than 14 samples.

        // 8 cube corners
        mOffsets[0] = DirectX::XMFLOAT4(+1.0f, +1.0f, +1.0f, 0.0f);
        mOffsets[1] = DirectX::XMFLOAT4(-1.0f, -1.0f, -1.0f, 0.0f);

        mOffsets[2] = DirectX::XMFLOAT4(-1.0f, +1.0f, +1.0f, 0.0f);
        mOffsets[3] = DirectX::XMFLOAT4(+1.0f, -1.0f, -1.0f, 0.0f);

        mOffsets[4] = DirectX::XMFLOAT4(+1.0f, +1.0f, -1.0f, 0.0f);
        mOffsets[5] = DirectX::XMFLOAT4(-1.0f, -1.0f, +1.0f, 0.0f);

        mOffsets[6] = DirectX::XMFLOAT4(-1.0f, +1.0f, -1.0f, 0.0f);
        mOffsets[7] = DirectX::XMFLOAT4(+1.0f, -1.0f, +1.0f, 0.0f);

        // 6 centers of cube faces
        mOffsets[8] = DirectX::XMFLOAT4(-1.0f, 0.0f, 0.0f, 0.0f);
        mOffsets[9] = DirectX::XMFLOAT4(+1.0f, 0.0f, 0.0f, 0.0f);

        mOffsets[10] = DirectX::XMFLOAT4(0.0f, -1.0f, 0.0f, 0.0f);
        mOffsets[11] = DirectX::XMFLOAT4(0.0f, +1.0f, 0.0f, 0.0f);

        mOffsets[12] = DirectX::XMFLOAT4(0.0f, 0.0f, -1.0f, 0.0f);
        mOffsets[13] = DirectX::XMFLOAT4(0.0f, 0.0f, +1.0f, 0.0f);

        for (int i = 0; i < 14; ++i)
        {
            // Create random lengths in [0.25, 1.0].
            auto s = MathHelper::RandF(0.25f, 1.0f);
            auto v = DirectX::XMVECTOR{s * DirectX::XMVector4Normalize(DirectX::XMLoadFloat4(&mOffsets[i]))};
            DirectX::XMStoreFloat4(&mOffsets[i], v);
        }
    }

private:
    D3D12::ID3D12Device* md3dDevice = nullptr;

    // Two sets of constants for ping-pong blurring the ambient map. They are the same
    // except for the gHorzBlur constant. The main SSAO pass can use either since the shader
    // does not use gHorzBlur. 
    bool mSsaoConstantsDirty = true;
    SsaoCB mSsaoHorzConstants;
    SsaoCB mSsaoVertConstants;
    DirectX::GraphicsResource mMemHandleSsaoHorzCB;
    DirectX::GraphicsResource mMemHandleSsaoVertCB;

    std::vector<float> mBlurWeights;
    float mOcclusionRadius = 0.5f;
    float mOcclusionFadeStart = 0.2f;
    float mOcclusionFadeEnd = 1.0f;
    float mSurfaceEpsilon = 0.05f;

    D3D12::D3D12_VIEWPORT mViewport;
    D3D12::D3D12_RECT mScissorRect;

    Microsoft::WRL::ComPtr<D3D12::ID3D12Resource> mNormalMap = nullptr;
    Microsoft::WRL::ComPtr<D3D12::ID3D12Resource> mAmbientMap0 = nullptr;
    Microsoft::WRL::ComPtr<D3D12::ID3D12Resource> mAmbientMap1 = nullptr;

    std::uint32_t mNormalMapBindlessIndex = -1;
    D3D12::CD3DX12_CPU_DESCRIPTOR_HANDLE mhNormalMapCpuSrv;
    D3D12::CD3DX12_GPU_DESCRIPTOR_HANDLE mhNormalMapGpuSrv;
    D3D12::CD3DX12_CPU_DESCRIPTOR_HANDLE mhNormalMapCpuRtv;

    // Need two for ping-ponging during blur.
    std::uint32_t mAmbientMap0BindlessIndex = -1;
    D3D12::CD3DX12_CPU_DESCRIPTOR_HANDLE mhAmbientMap0CpuSrv;
    D3D12::CD3DX12_GPU_DESCRIPTOR_HANDLE mhAmbientMap0GpuSrv;
    D3D12::CD3DX12_CPU_DESCRIPTOR_HANDLE mhAmbientMap0CpuRtv;

    std::uint32_t mAmbientMap1BindlessIndex = -1;
    D3D12::CD3DX12_CPU_DESCRIPTOR_HANDLE mhAmbientMap1CpuSrv;
    D3D12::CD3DX12_GPU_DESCRIPTOR_HANDLE mhAmbientMap1GpuSrv;
    D3D12::CD3DX12_CPU_DESCRIPTOR_HANDLE mhAmbientMap1CpuRtv;

    std::uint32_t mRenderTargetWidth = 0;
    std::uint32_t mRenderTargetHeight = 0;

    DirectX::XMFLOAT4 mOffsets[14];
};