export module shared:camera;
import std;
import :win32;
import :mathhelper;

export class Camera
{
public:
	Camera()
	{
		SetLens(0.25f * MathHelper::Pi, 1.0f, 1.0f, 1000.0f);
	}

	// Get/Set world camera position.
	auto GetPosition()const->DirectX::XMVECTOR
	{
		return XMLoadFloat3(&mPosition);
	}

	auto GetPosition3f()const->DirectX::XMFLOAT3
	{
		return mPosition;
	}

	void SetPosition(float x, float y, float z)
	{
		mPosition = DirectX::XMFLOAT3(x, y, z);
		mViewDirty = true;
	}

	void SetPosition(const DirectX::XMFLOAT3& v)
	{
		mPosition = v;
		mViewDirty = true;
	}

	// Get camera basis vectors.
	auto GetRight()const->DirectX::XMVECTOR
	{
		return XMLoadFloat3(&mRight);
	}

	auto GetRight3f()const->DirectX::XMFLOAT3
	{
		return mRight;
	}

	auto GetUp()const->DirectX::XMVECTOR
	{
		return XMLoadFloat3(&mUp);
	}

	auto GetUp3f()const->DirectX::XMFLOAT3
	{
		return mUp;
	}

	auto GetLook()const->DirectX::XMVECTOR
	{
		return XMLoadFloat3(&mLook);
	}

	auto GetLook3f()const->DirectX::XMFLOAT3
	{
		return mLook;
	}

	// Get frustum properties.
	auto GetNearZ()const->float
	{
		return mNearZ;
	}

	auto GetFarZ()const->float
	{
		return mFarZ;
	}

	auto GetAspect()const->float
	{
		return mAspect;
	}

	auto GetFovY()const->float
	{
		return mFovY;
	}

	auto GetFovX()const->float
	{
		float halfWidth = 0.5f * GetNearWindowWidth();
		return 2.0f * atan(halfWidth / mNearZ);
	}

	// Get near and far plane dimensions in view space coordinates.
	auto GetNearWindowWidth()const->float
	{
		return mAspect * mNearWindowHeight;
	}

	auto GetNearWindowHeight()const->float
	{
		return mNearWindowHeight;
	}

	auto GetFarWindowWidth()const->float
	{
		return mAspect * mFarWindowHeight;
	}

	auto GetFarWindowHeight()const->float
	{
		return mFarWindowHeight;
	}

	// Set frustum.
	void SetLens(float fovY, float aspect, float zn, float zf)
	{
		// cache properties
		mFovY = fovY;
		mAspect = aspect;
		mNearZ = zn;
		mFarZ = zf;

		mNearWindowHeight = 2.0f * mNearZ * std::tanf(0.5f * mFovY);
		mFarWindowHeight = 2.0f * mFarZ * std::tanf(0.5f * mFovY);

		auto P = DirectX::XMMATRIX{DirectX::XMMatrixPerspectiveFovLH(mFovY, mAspect, mNearZ, mFarZ)};
		DirectX::XMStoreFloat4x4(&mProj, P);
	}

	// Define camera space via LookAt parameters.
	void LookAt(DirectX::FXMVECTOR pos, DirectX::FXMVECTOR target, DirectX::FXMVECTOR worldUp)
	{
		auto L = DirectX::XMVECTOR{DirectX::XMVector3Normalize(DirectX::XMVectorSubtract(target, pos))};
		auto R = DirectX::XMVECTOR{DirectX::XMVector3Normalize(DirectX::XMVector3Cross(worldUp, L))};
		auto U = DirectX::XMVECTOR{DirectX::XMVector3Cross(L, R)};

		DirectX::XMStoreFloat3(&mPosition, pos);
		DirectX::XMStoreFloat3(&mLook, L);
		DirectX::XMStoreFloat3(&mRight, R);
		DirectX::XMStoreFloat3(&mUp, U);

		mViewDirty = true;
	}

	void LookAt(const DirectX::XMFLOAT3& pos, const DirectX::XMFLOAT3& target, const DirectX::XMFLOAT3& up)
	{
		auto P = DirectX::XMVECTOR{DirectX::XMLoadFloat3(&pos)};
		auto T = DirectX::XMVECTOR{DirectX::XMLoadFloat3(&target)};
		auto U = DirectX::XMVECTOR{DirectX::XMLoadFloat3(&up)};

		LookAt(P, T, U);

		mViewDirty = true;
	}

	// Get View/Proj matrices.
	auto GetView()const->DirectX::XMMATRIX
	{
		//assert(!mViewDirty);
		return DirectX::XMLoadFloat4x4(&mView);
	}

	auto GetProj()const->DirectX::XMMATRIX
	{
		return DirectX::XMLoadFloat4x4(&mProj);
	}

	auto GetView4x4f()const -> DirectX::XMFLOAT4X4
	{
		//assert(!mViewDirty);
		return mView;
	}

	auto GetProj4x4f()const -> DirectX::XMFLOAT4X4
	{
		return mProj;
	}

	// Strafe/Walk the camera a distance d.
	void Strafe(float d)
	{
		// mPosition += d*mRight
		auto s = DirectX::XMVectorReplicate(d);
		auto r = DirectX::XMLoadFloat3(&mRight);
		auto p = DirectX::XMLoadFloat3(&mPosition);
		DirectX::XMStoreFloat3(&mPosition, DirectX::XMVectorMultiplyAdd(s, r, p));

		mViewDirty = true;
	}

	void Walk(float d)
	{
		// mPosition += d*mLook
		auto s = DirectX::XMVectorReplicate(d);
		auto l = DirectX::XMLoadFloat3(&mLook);
		auto p = DirectX::XMLoadFloat3(&mPosition);
		DirectX::XMStoreFloat3(&mPosition, DirectX::XMVectorMultiplyAdd(s, l, p));

		mViewDirty = true;
	}

	// Rotate the camera.
	void Pitch(float angle)
	{
		// Rotate up and look vector about the right vector.

		auto R = DirectX::XMMATRIX{DirectX::XMMatrixRotationAxis(DirectX::XMLoadFloat3(&mRight), angle)};

		DirectX::XMStoreFloat3(&mUp, DirectX::XMVector3TransformNormal(DirectX::XMLoadFloat3(&mUp), R));
		DirectX::XMStoreFloat3(&mLook, DirectX::XMVector3TransformNormal(DirectX::XMLoadFloat3(&mLook), R));

		mViewDirty = true;
	}

	void RotateY(float angle)
	{
		// Rotate the basis vectors about the world y-axis.

		auto R = DirectX::XMMATRIX{DirectX::XMMatrixRotationY(angle)};

		DirectX::XMStoreFloat3(&mRight, DirectX::XMVector3TransformNormal(DirectX::XMLoadFloat3(&mRight), R));
		DirectX::XMStoreFloat3(&mUp, DirectX::XMVector3TransformNormal(DirectX::XMLoadFloat3(&mUp), R));
		DirectX::XMStoreFloat3(&mLook, DirectX::XMVector3TransformNormal(DirectX::XMLoadFloat3(&mLook), R));

		mViewDirty = true;
	}

	// After modifying camera position/orientation, call to rebuild the view matrix.
	void UpdateViewMatrix()
	{
		if (not mViewDirty)
			return;

		auto R = DirectX::XMVECTOR{DirectX::XMLoadFloat3(&mRight)};
		auto U = DirectX::XMVECTOR{DirectX::XMLoadFloat3(&mUp)};
		auto L = DirectX::XMVECTOR{DirectX::XMLoadFloat3(&mLook)};
		auto P = DirectX::XMVECTOR{DirectX::XMLoadFloat3(&mPosition)};

		// Keep camera's axes orthogonal to each other and of unit length.
		L = DirectX::XMVector3Normalize(L);
		U = DirectX::XMVector3Normalize(DirectX::XMVector3Cross(L, R));

		// U, L already ortho-normal, so no need to normalize cross product.
		R = DirectX::XMVector3Cross(U, L);

		// Fill in the view matrix entries.
		auto x = -DirectX::XMVectorGetX(DirectX::XMVector3Dot(P, R));
		auto y = -DirectX::XMVectorGetX(DirectX::XMVector3Dot(P, U));
		auto z = -DirectX::XMVectorGetX(DirectX::XMVector3Dot(P, L));

		DirectX::XMStoreFloat3(&mRight, R);
		DirectX::XMStoreFloat3(&mUp, U);
		DirectX::XMStoreFloat3(&mLook, L);

		mView(0, 0) = mRight.x;
		mView(1, 0) = mRight.y;
		mView(2, 0) = mRight.z;
		mView(3, 0) = x;

		mView(0, 1) = mUp.x;
		mView(1, 1) = mUp.y;
		mView(2, 1) = mUp.z;
		mView(3, 1) = y;

		mView(0, 2) = mLook.x;
		mView(1, 2) = mLook.y;
		mView(2, 2) = mLook.z;
		mView(3, 2) = z;

		mView(0, 3) = 0.0f;
		mView(1, 3) = 0.0f;
		mView(2, 3) = 0.0f;
		mView(3, 3) = 1.0f;

		mViewDirty = false;
	}

private:
	// Camera coordinate system with coordinates relative to world space.
	DirectX::XMFLOAT3 mPosition = { 0.0f, 0.0f, 0.0f };
	DirectX::XMFLOAT3 mRight = { 1.0f, 0.0f, 0.0f };
	DirectX::XMFLOAT3 mUp = { 0.0f, 1.0f, 0.0f };
	DirectX::XMFLOAT3 mLook = { 0.0f, 0.0f, 1.0f };

	// Cache frustum properties.
	float mNearZ = 0.0f;
	float mFarZ = 0.0f;
	float mAspect = 0.0f;
	float mFovY = 0.0f;
	float mNearWindowHeight = 0.0f;
	float mFarWindowHeight = 0.0f;

	bool mViewDirty = true;

	// Cache View/Proj matrices.
	DirectX::XMFLOAT4X4 mView = MathHelper::Identity4x4;
	DirectX::XMFLOAT4X4 mProj = MathHelper::Identity4x4;
};