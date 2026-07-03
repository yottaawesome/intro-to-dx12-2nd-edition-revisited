//***************************************************************************************
// d3dApp.cpp by Frank Luna (C) 2015 All Rights Reserved.
//***************************************************************************************

export module shared:mathhelper;
import std;
import :win32;
import :random;

export class MathHelper
{
public:

    using Vector2 = DirectX::SimpleMath::Vector2;
    using Vector3 = DirectX::SimpleMath::Vector3;
    using Vector4 = DirectX::SimpleMath::Vector4;
    using Plane = DirectX::SimpleMath::Plane;
    using Matrix = DirectX::SimpleMath::Matrix;

    // Returns random float in [0, 1).
    static auto RandF() -> float
    {
        return Random{}.Uniform(0.0f, 1.0f);
    }

    // Returns random float in [a, b).
    static auto RandF(float a, float b) -> float
    {
        return Random{}.Uniform(a, b);
    }

    static auto Rand(int a, int b) -> int
    {
        return Random{}.Uniform(a, b);
    }

    template<typename T>
    static auto Lerp(const T& a, const T& b, float t) -> T
    {
        return a + (b - a) * t;
    }

    // Returns the polar angle of the point (x,y) in [0, 2*PI).
    static auto AngleFromXY(float x, float y) -> float
    {
        float theta = 0.0f;

        // Quadrant I or IV
        if (x >= 0.0f)
        {
            // If x = 0, then atanf(y/x) = +pi/2 if y > 0
            //                atanf(y/x) = -pi/2 if y < 0
            theta = std::atanf(y / x); // in [-pi/2, +pi/2]

            if (theta < 0.0f)
                theta += 2.0f * Pi; // in [0, 2*pi).
        }

        // Quadrant II or III
        else
            theta = std::atanf(y / x) + Pi; // in [0, 2*pi).

        return theta;
    }

    static auto SphericalToCartesian(
        float radius, 
        float theta, 
        float phi
    ) -> DirectX::XMVECTOR
    {
        return DirectX::XMVectorSet(
            radius * std::sinf(phi) * std::cosf(theta),
            radius * std::cosf(phi),
            radius * std::sinf(phi) * std::sinf(theta),
            1.0f);
    }

    static auto InverseTranspose(DirectX::CXMMATRIX M) -> DirectX::XMMATRIX
    {
        // Inverse-transpose is just applied to normals.  So zero out 
        // translation row so that it doesn't get into our inverse-transpose
        // calculation--we don't want the inverse-transpose of the translation.
        auto A = DirectX::XMMATRIX{ M };
        A.r[3] = DirectX::XMVectorSet(0.0f, 0.0f, 0.0f, 1.0f);

        auto det = DirectX::XMVECTOR{DirectX::XMMatrixDeterminant(A)};
        return DirectX::XMMatrixTranspose(DirectX::XMMatrixInverse(&det, A));
    }

	constexpr static auto Identity4x4 = DirectX::XMFLOAT4X4{
        1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f,
        0.0f, 0.0f, 0.0f, 1.0f
    };

    static auto RandUnitVec3() -> DirectX::XMVECTOR
    {
        auto One = DirectX::XMVECTOR{DirectX::XMVectorSet(1.0f, 1.0f, 1.0f, 1.0f)};
        auto Zero = DirectX::XMVECTOR{DirectX::XMVectorZero()};

        // Keep trying until we get a point on/in the sphere.
        while (true)
        {
            // Generate random point in the cube [-1,1]^3.
            auto v = DirectX::XMVECTOR{
                DirectX::XMVectorSet(MathHelper::RandF(-1.0f, 1.0f), MathHelper::RandF(-1.0f, 1.0f), MathHelper::RandF(-1.0f, 1.0f), 0.0f)
            };

            // Ignore points outside the unit sphere in order to get an even distribution 
            // over the unit sphere.  Otherwise points will clump more on the sphere near 
            // the corners of the cube.
            if (DirectX::XMVector3Greater(DirectX::XMVector3LengthSq(v), One))
                continue;

            return DirectX::XMVector3Normalize(v);
        }
    }

    static auto RandHemisphereUnitVec3(DirectX::XMVECTOR n) -> DirectX::XMVECTOR
    {
        auto One = DirectX::XMVECTOR{DirectX::XMVectorSet(1.0f, 1.0f, 1.0f, 1.0f)};
        auto Zero = DirectX::XMVECTOR{DirectX::XMVectorZero()};

        // Keep trying until we get a point on/in the hemisphere.
        while (true)
        {
            // Generate random point in the cube [-1,1]^3.
            auto v = DirectX::XMVECTOR{
                DirectX::XMVectorSet(MathHelper::RandF(-1.0f, 1.0f), MathHelper::RandF(-1.0f, 1.0f), MathHelper::RandF(-1.0f, 1.0f), 0.0f)};

            // Ignore points outside the unit sphere in order to get an even distribution 
            // over the unit sphere.  Otherwise points will clump more on the sphere near 
            // the corners of the cube.

            if (DirectX::XMVector3Greater(DirectX::XMVector3LengthSq(v), One))
                continue;

            // Ignore points in the bottom hemisphere.
            if (DirectX::XMVector3Less(DirectX::XMVector3Dot(n, v), Zero))
                continue;

            return DirectX::XMVector3Normalize(v);
        }
    }

    static void CalcPickingRay(
        const Vector2& screenPos,
        const Vector2& screenSize,
        const Matrix& viewMatrix,
        const Matrix& projMatrix,
        Vector3& worldRayOrigin,
        Vector3& worldRayDirection
    )
    {
        auto invView = Matrix{viewMatrix.Invert()};

        // Compute picking ray in view space.
        auto vx = (+2.0f * screenPos.x / screenSize.x - 1.0f) / projMatrix(0, 0);
        auto vy = (-2.0f * screenPos.y / screenSize.y + 1.0f) / projMatrix(1, 1);

        // Ray definition in view space.
        auto rayOrigin = Vector3{0.0f, 0.0f, 0.0f};
        auto rayDir = Vector3{vx, vy, 1.0f};

        worldRayOrigin = Vector3::Transform(rayOrigin, invView);
        worldRayDirection = Vector3::TransformNormal(rayDir, invView);

        // Make the ray direction unit length for the intersection tests.
        worldRayDirection.Normalize();
    }


    // Order: left, right, bottom, top, near, far.
    static void ExtractFrustumPlanes(const Matrix& M, DirectX::XMFLOAT4 outPlanes[6])
    {
        auto planes = std::array<Plane, 6>{};

        //
        // Left
        //
        planes[0].x = M(0, 3) + M(0, 0);
        planes[0].y = M(1, 3) + M(1, 0);
        planes[0].z = M(2, 3) + M(2, 0);
        planes[0].w = M(3, 3) + M(3, 0);

        //
        // Right
        //
        planes[1].x = M(0, 3) - M(0, 0);
        planes[1].y = M(1, 3) - M(1, 0);
        planes[1].z = M(2, 3) - M(2, 0);
        planes[1].w = M(3, 3) - M(3, 0);

        //
        // Bottom
        //
        planes[2].x = M(0, 3) + M(0, 1);
        planes[2].y = M(1, 3) + M(1, 1);
        planes[2].z = M(2, 3) + M(2, 1);
        planes[2].w = M(3, 3) + M(3, 1);

        //
        // Top
        //
        planes[3].x = M(0, 3) - M(0, 1);
        planes[3].y = M(1, 3) - M(1, 1);
        planes[3].z = M(2, 3) - M(2, 1);
        planes[3].w = M(3, 3) - M(3, 1);

        //
        // Near
        //
        planes[4].x = M(0, 2);
        planes[4].y = M(1, 2);
        planes[4].z = M(2, 2);
        planes[4].w = M(3, 2);

        //
        // Far
        //
        planes[5].x = M(0, 3) - M(0, 2);
        planes[5].y = M(1, 3) - M(1, 2);
        planes[5].z = M(2, 3) - M(2, 2);
        planes[5].w = M(3, 3) - M(3, 2);

        // Normalize the plane equations.
        for (int i = 0; i < 6; ++i)
        {
            planes[i].Normalize();
            outPlanes[i] = planes[i];
        }
    }

    static auto ComputeFrustumBoundingSphereInViewSpace(
        const DirectX::BoundingFrustum& subfrustum
    ) -> DirectX::BoundingSphere
    {
        //
        // In view space, the bounding sphere has center C = (0, 0, s) for some s.
        //
        // Let P be a corner point on the near window and let Q be a corner point on the far window.
        //
        // For the circumscribed sphere we have distance(C, P) == distance(C, Q).
        //
        // ||P - C||^2 == ||Q - C||^2
        //
        // dot(P-C,P-C) = dot(Q-C,Q-C)
        //
        // dot(P,P) - 2*dot(P,C) + dot(C,C) == dot(Q,Q) - 2*dot(Q,C) + dot(C,C)
        //
        // -2*dot(P,C) + 2*dot(Q,C) == dot(Q,Q) - dot(P,P)
        //
        // 2 * dot(C, Q-P) == dot(Q,Q) - dot(P,P)
        //
        // Since C = (0, 0, s), dot(C, Q-P) = s(qz - pz)
        //
        // s = (dot(Q,Q) - dot(P,P)) / (2(qz - pz))
        // 
        // The circumscribed sphere is not necessarily the smallest bounding sphere. 
        //
        // Let n be the near plane and let f be the far plane.
        //
        // If n <= s <= f, then the circumscribed sphere is the smallest bounding sphere.
        //
        // If s > f, then C = (0, 0, f) with r = distance(C,Q) gives the smallest bounding sphere.
        // 

        auto corners = std::array<DirectX::XMFLOAT3, 8>{};
        subfrustum.GetCorners(corners.data());

        // Point on near plane (left-bottom).
        const auto P = Vector3{ corners[3] };

        // Point on far plane (top-right).
        const auto Q = Vector3{ corners[5] };

        float n = subfrustum.Near;
        float f = subfrustum.Far;

        float s = (Q.Dot(Q) - P.Dot(P)) / (2.0f * (f - n));

        auto result = DirectX::BoundingSphere{};

        // This only happens if the frustum slope is steep.
        if (s > f)
            result.Center = DirectX::XMFLOAT3{0.0f, 0.0f, f};
        else
            result.Center = DirectX::XMFLOAT3{0.0f, 0.0f, s};

        // This works if s > f, too.
        // r = ||Q - C||
        result.Radius = DirectX::SimpleMath::Vector3::Distance(Q, result.Center);

        return result;
    }

    static inline constexpr auto Infinity = std::numeric_limits<float>::max();
    static inline constexpr auto Pi = 3.1415926535f;
};
