export module shared:uploadbuffer;
import std;
import :win32;
import :d3dutil;

export
{
    template<typename T>
    class UploadBuffer
    {
    public:
        UploadBuffer(D3D12::ID3D12Device* device, Win32::UINT elementCount, bool isConstantBuffer) :
            mIsConstantBuffer(isConstantBuffer)
        {
            mElementByteSize = sizeof(T);

            // Constant buffer elements need to be multiples of 256 bytes.
            // This is because the hardware can only view constant data 
            // at m*256 byte offsets and of n*256 byte lengths. 
            // typedef struct D3D12_CONSTANT_BUFFER_VIEW_DESC {
            // UINT64 OffsetInBytes; // multiple of 256
            // UINT   SizeInBytes;   // multiple of 256
            // } D3D12_CONSTANT_BUFFER_VIEW_DESC;
            if (isConstantBuffer)
                mElementByteSize = d3dUtil::CalcConstantBufferByteSize(sizeof(T));

            auto heapProps = D3D12::CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
            auto size = D3D12::CD3DX12_RESOURCE_DESC::Buffer(mElementByteSize * elementCount);
            ThrowIfFailed(device->CreateCommittedResource(
                &heapProps,
                D3D12_HEAP_FLAG_NONE,
                &size,
                D3D12_RESOURCE_STATE_GENERIC_READ,
                nullptr,
				__uuidof(D3D12::ID3D12Resource),
                &mUploadBuffer));

            ThrowIfFailed(mUploadBuffer->Map(0, nullptr, reinterpret_cast<void**>(&mMappedData)));

            // We do not need to unmap until we are done with the resource.  However, we must not write to
            // the resource while it is in use by the GPU (so we must use synchronization techniques).
        }

        UploadBuffer(const UploadBuffer& rhs) = delete;
        UploadBuffer& operator=(const UploadBuffer& rhs) = delete;
        ~UploadBuffer()
        {
            if (mUploadBuffer != nullptr)
                mUploadBuffer->Unmap(0, nullptr);

            mMappedData = nullptr;
        }

        auto Resource()const -> D3D12::ID3D12Resource*
        {
            return mUploadBuffer.Get();
        }

        void CopyData(int elementIndex, const T& data)
        {
            memcpy(&mMappedData[elementIndex * mElementByteSize], &data, sizeof(T));
        }

        void CopyData(const T* data, std::uint32_t count)
        {
            assert(mElementByteSize == sizeof(T));

            memcpy(mMappedData, data, count * sizeof(T));
        }

    private:
        Microsoft::WRL::ComPtr<D3D12::ID3D12Resource> mUploadBuffer;
        Win32::BYTE* mMappedData = nullptr;

        Win32::UINT mElementByteSize = 0;
        bool mIsConstantBuffer = false;
    };
}
