//***************************************************************************************
// d3dApp.cpp by Frank Luna (C) 2015 All Rights Reserved.
//***************************************************************************************

#include <Windows.h>
#include <DirectXMath.h>
#include <DirectXPackedVector.h>

import std;

using namespace std;
using namespace DirectX;
using namespace DirectX::PackedVector;

// Overload the  "<<" operators so that we can use cout to 
// output XMVECTOR objects.
auto XM_CALLCONV operator<<(ostream& os, FXMVECTOR v) -> ostream&
{
    auto dest = XMFLOAT3{};
    XMStoreFloat3(&dest, v);

    os << std::format("({:.3f}, {:.3f}, {:.3f})", dest.x, dest.y, dest.z);
    return os;
}

auto main() -> int
{
    cout.setf(ios_base::boolalpha);

    // Check support for SSE2 (Pentium4, AMD K8, and above).
    if (not XMVerifyCPUSupport())
    {
        cout << "directx math not supported" << endl;
        return 0;
    }

    auto n = XMVECTOR{ XMVectorSet(1.0f, 0.0f, 0.0f, 0.0f) };
    auto u = XMVECTOR{ XMVectorSet(1.0f, 2.0f, 3.0f, 0.0f) };
    auto v = XMVECTOR{ XMVectorSet(-2.0f, 1.0f, -3.0f, 0.0f) };
    auto w = XMVECTOR{ XMVectorSet(0.707f, 0.707f, 0.0f, 0.0f) };

    // Vector addition: XMVECTOR operator + 
    auto a = XMVECTOR{ u + v };

    // Vector subtraction: XMVECTOR operator - 
    auto b = XMVECTOR{ u - v };

    // Scalar multiplication: XMVECTOR operator * 
    auto c = XMVECTOR{ 10.0f * u };

    // ||u||
    auto L = XMVECTOR{ XMVector3Length(u) };

    // d = u / ||u||
    auto d = XMVECTOR{ XMVector3Normalize(u) };

    // s = u dot v
    auto s = XMVECTOR{ XMVector3Dot(u, v) };

    // e = u x v
    auto e = XMVECTOR{ XMVector3Cross(u, v) };

    // Find proj_n(w) and perp_n(w)
    auto projW = XMVECTOR{};
    auto perpW = XMVECTOR{};
    XMVector3ComponentsFromNormal(&projW, &perpW, w, n);

    // Does projW + perpW == w?
    auto equal = XMVector3Equal(projW + perpW, w) != 0;
    auto notEqual = XMVector3NotEqual(projW + perpW, w) != 0;

    // The angle between projW and perpW should be 90 degrees.
    auto angleVec = XMVECTOR{ XMVector3AngleBetweenVectors(projW, perpW) };
    auto angleRadians = XMVectorGetX(angleVec);
    auto angleDegrees = XMConvertToDegrees(angleRadians);

    cout << "u                   = " << u << endl;
    cout << "v                   = " << v << endl;
    cout << "w                   = " << w << endl;
    cout << "n                   = " << n << endl;
    cout << "a = u + v           = " << a << endl;
    cout << "b = u - v           = " << b << endl;
    cout << "c = 10 * u          = " << c << endl;
    cout << "d = u / ||u||       = " << d << endl;
    cout << "e = u x v           = " << e << endl;
    cout << "L  = ||u||          = " << L << endl;
    cout << "s = u.v             = " << s << endl;
    cout << "projW               = " << projW << endl;
    cout << "perpW               = " << perpW << endl;
    cout << "projW + perpW == w  = " << equal << endl;
    cout << "projW + perpW != w  = " << notEqual << endl;
    cout << "angle               = " << angleDegrees << endl;

    return 0;
}
