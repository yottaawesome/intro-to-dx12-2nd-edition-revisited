//***************************************************************************************
// d3dApp.cpp by Frank Luna (C) 2015 All Rights Reserved.
//***************************************************************************************

#include <windows.h>
#include <DirectXMath.h>
#include <DirectXPackedVector.h>

import std;

using namespace std;
using namespace DirectX;
using namespace DirectX::PackedVector;

// Overload the  "<<" operators so that we can use cout to 
// output XMVECTOR and XMMATRIX objects.
auto XM_CALLCONV operator << (ostream& os, FXMVECTOR v) -> ostream&
{
    auto dest = XMFLOAT4{};
    XMStoreFloat4(&dest, v);

    os << "(" << dest.x << ", " << dest.y << ", " << dest.z << ", " << dest.w << ")";
    return os;
}

auto XM_CALLCONV operator<<(ostream& os, FXMMATRIX m) -> ostream&
{
    for (int i = 0; i < 4; ++i)
    {
        os << XMVectorGetX(m.r[i]) << "\t";
        os << XMVectorGetY(m.r[i]) << "\t";
        os << XMVectorGetZ(m.r[i]) << "\t";
        os << XMVectorGetW(m.r[i]);
        os << endl;
    }
    return os;
}

auto main() -> int
{
    // Check support for SSE2 (Pentium4, AMD K8, and above).
    if (not XMVerifyCPUSupport())
    {
        cout << "directx math not supported" << endl;
        return 0;
    }

    auto A = XMMATRIX{ 
        1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 2.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 4.0f, 0.0f,
        1.0f, 2.0f, 3.0f, 1.0f 
    };

    auto B = XMMATRIX{ XMMatrixIdentity() };

    auto C = A * B;

    auto D = XMMatrixTranspose(A);

    auto det = XMMatrixDeterminant(A);
    auto E = XMMatrixInverse(&det, A);

    auto F = A * E;

    cout << "A = " << endl << A << endl;
    cout << "B = " << endl << B << endl;
    cout << "C = A*B = " << endl << C << endl;
    cout << "D = transpose(A) = " << endl << D << endl;
    cout << "det = determinant(A) = " << det << endl << endl;
    cout << "E = inverse(A) = " << endl << E << endl;
    cout << "F = A*E = " << endl << F << endl;

    return 0;
}
