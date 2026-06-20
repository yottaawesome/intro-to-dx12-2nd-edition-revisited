export module shared:mathhelper;
import :win32;

export namespace MathHelper
{
    inline auto Identity4x4() -> DirectX::XMFLOAT4X4
    {
        return DirectX::XMFLOAT4X4{
            1.0f, 0.0f, 0.0f, 0.0f,
            0.0f, 1.0f, 0.0f, 0.0f,
            0.0f, 0.0f, 1.0f, 0.0f,
            0.0f, 0.0f, 0.0f, 1.0f 
        };
    }
}
