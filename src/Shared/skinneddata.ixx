//***************************************************************************************
// d3dApp.cpp by Frank Luna (C) 2015 All Rights Reserved.
//***************************************************************************************

export module shared:skinneddata;
import std;
import :win32;
import :mathhelper;

///<summary>
/// A Keyframe defines the bone transformation at an instant in time.
///</summary>
export struct Keyframe
{
	float TimePos = 0;
	DirectX::XMFLOAT3 Translation = {0.0f, 0.0f, 0.0f};
	DirectX::XMFLOAT3 Scale = {1.0f, 1.0f, 1.0f};
	DirectX::XMFLOAT4 RotationQuat = {0.0f, 0.0f, 0.0f, 1.0f};
};

///<summary>
/// A BoneAnimation is defined by a list of keyframes.  For time
/// values inbetween two keyframes, we interpolate between the
/// two nearest keyframes that bound the time.  
///
/// We assume an animation always has two keyframes.
///</summary>
export struct BoneAnimation
{
	auto GetStartTime() const -> float
	{
		// Keyframes are sorted by time, so first keyframe gives start time.
		return Keyframes.front().TimePos;
	}

	auto GetEndTime() const -> float
	{
		// Keyframes are sorted by time, so last keyframe gives end time.
		return Keyframes.back().TimePos;
	}

	void Interpolate(float t, DirectX::XMFLOAT4X4& M) const
	{
		if (t <= Keyframes.front().TimePos)
		{
			auto S = DirectX::XMVECTOR{DirectX::XMLoadFloat3(&Keyframes.front().Scale)};
			auto P = DirectX::XMVECTOR{DirectX::XMLoadFloat3(&Keyframes.front().Translation)};
			auto Q = DirectX::XMVECTOR{DirectX::XMLoadFloat4(&Keyframes.front().RotationQuat)};

			auto zero = DirectX::XMVECTOR{DirectX::XMVectorSet(0.0f, 0.0f, 0.0f, 1.0f)};
			DirectX::XMStoreFloat4x4(&M, DirectX::XMMatrixAffineTransformation(S, zero, Q, P));
		}
		else if (t >= Keyframes.back().TimePos)
		{
			auto S = DirectX::XMVECTOR{DirectX::XMLoadFloat3(&Keyframes.back().Scale)};
			auto P = DirectX::XMVECTOR{DirectX::XMLoadFloat3(&Keyframes.back().Translation)};
			auto Q = DirectX::XMVECTOR{DirectX::XMLoadFloat4(&Keyframes.back().RotationQuat)};

			auto zero = DirectX::XMVECTOR{DirectX::XMVectorSet(0.0f, 0.0f, 0.0f, 1.0f)};
			DirectX::XMStoreFloat4x4(&M, DirectX::XMMatrixAffineTransformation(S, zero, Q, P));
		}
		else
		{
			for (auto i = 0u; i < Keyframes.size() - 1; ++i)
			{
				if (t >= Keyframes[i].TimePos && t <= Keyframes[i + 1].TimePos)
				{
					float lerpPercent = (t - Keyframes[i].TimePos) / (Keyframes[i + 1].TimePos - Keyframes[i].TimePos);

					auto s0 = DirectX::XMVECTOR{DirectX::XMLoadFloat3(&Keyframes[i].Scale)};
					auto s1 = DirectX::XMVECTOR{DirectX::XMLoadFloat3(&Keyframes[i + 1].Scale)};

					auto p0 = DirectX::XMVECTOR{DirectX::XMLoadFloat3(&Keyframes[i].Translation)};
					auto p1 = DirectX::XMVECTOR{DirectX::XMLoadFloat3(&Keyframes[i + 1].Translation)};

					auto q0 = DirectX::XMVECTOR{DirectX::XMLoadFloat4(&Keyframes[i].RotationQuat)};
					auto q1 = DirectX::XMVECTOR{DirectX::XMLoadFloat4(&Keyframes[i + 1].RotationQuat)};

					auto S = DirectX::XMVECTOR{DirectX::XMVectorLerp(s0, s1, lerpPercent)};
					auto P = DirectX::XMVECTOR{DirectX::XMVectorLerp(p0, p1, lerpPercent)};
					auto Q = DirectX::XMVECTOR{DirectX::XMQuaternionSlerp(q0, q1, lerpPercent)};

					auto zero = DirectX::XMVECTOR{DirectX::XMVectorSet(0.0f, 0.0f, 0.0f, 1.0f)};
					DirectX::XMStoreFloat4x4(&M, DirectX::XMMatrixAffineTransformation(S, zero, Q, P));

					break;
				}
			}
		}
	}

	std::vector<Keyframe> Keyframes;
};

///<summary>
/// Examples of AnimationClips are "Walk", "Run", "Attack", "Defend".
/// An AnimationClip requires a BoneAnimation for every bone to form
/// the animation clip.    
///</summary>
export struct AnimationClip
{
	auto GetClipStartTime() const -> float
	{
		// Find smallest start time over all bones in this clip.
		auto t = MathHelper::Infinity;
		for (auto i = 0u; i < BoneAnimations.size(); ++i)
		{
			t = MathHelper::Min(t, BoneAnimations[i].GetStartTime());
		}

		return t;
	}
	auto GetClipEndTime() const -> float
	{
		// Find largest end time over all bones in this clip.
		auto t = 0.0f;
		for (auto i = 0u; i < BoneAnimations.size(); ++i)
		{
			t = MathHelper::Max(t, BoneAnimations[i].GetEndTime());
		}
		return t;
	}

	void Interpolate(float t, std::vector<DirectX::XMFLOAT4X4>& boneTransforms) const
	{
		for (auto i = 0u; i < BoneAnimations.size(); ++i)
		{
			BoneAnimations[i].Interpolate(t, boneTransforms[i]);
		}
	}

	std::vector<BoneAnimation> BoneAnimations;
};

export class SkinnedData
{
public:

	auto BoneCount()const -> Win32::UINT
	{
		return (Win32::UINT)mBoneHierarchy.size();
	}

	auto GetClipStartTime(const std::string& clipName) const -> float
	{
		auto clip = mAnimations.find(clipName);
		return clip->second.GetClipStartTime();
	}

	auto GetClipEndTime(const std::string& clipName) const -> float
	{
		auto clip = mAnimations.find(clipName);
		return clip->second.GetClipEndTime();
	}

	void Set(
		std::vector<int>& boneHierarchy,
		std::vector<DirectX::XMFLOAT4X4>& boneOffsets,
		std::unordered_map<std::string, AnimationClip>& animations
	)
	{
		mBoneHierarchy = boneHierarchy;
		mBoneOffsets = boneOffsets;
		mAnimations = animations;
	}

	// In a real project, you'd want to cache the result if there was a chance
	// that you were calling this several times with the same clipName at 
	// the same timePos.
	void GetFinalTransforms(
		const std::string& clipName, 
		float timePos,
		std::vector<DirectX::XMFLOAT4X4>& finalTransforms
	) const
	{
		auto numBones = static_cast<Win32::UINT>(mBoneOffsets.size());
		auto toParentTransforms = std::vector<DirectX::XMFLOAT4X4>(numBones);

		// Interpolate all the bones of this clip at the given time instance.
		auto clip = mAnimations.find(clipName);
		clip->second.Interpolate(timePos, toParentTransforms);

		//
		// Traverse the hierarchy and transform all the bones to the root space.
		//
		auto toRootTransforms = std::vector<DirectX::XMFLOAT4X4>(numBones);

		// The root bone has index 0.  The root bone has no parent, so its toRootTransform
		// is just its local bone transform.
		toRootTransforms[0] = toParentTransforms[0];

		// Now find the toRootTransform of the children.
		for (auto i = 1u; i < numBones; ++i)
		{
			auto toParent = DirectX::XMMATRIX{DirectX::XMLoadFloat4x4(&toParentTransforms[i])};

			auto parentIndex = int{ mBoneHierarchy[i] };
			auto parentToRoot = DirectX::XMMATRIX{DirectX::XMLoadFloat4x4(&toRootTransforms[parentIndex])};

			auto toRoot = DirectX::XMMATRIX{DirectX::XMMatrixMultiply(toParent, parentToRoot)};

			DirectX::XMStoreFloat4x4(&toRootTransforms[i], toRoot);
		}

		// Premultiply by the bone offset transform to get the final transform.
		for (auto i = 0u; i < numBones; ++i)
		{
			auto offset = DirectX::XMMATRIX{DirectX::XMLoadFloat4x4(&mBoneOffsets[i])};
			auto toRoot = DirectX::XMMATRIX{DirectX::XMLoadFloat4x4(&toRootTransforms[i])};
			auto finalTransform = DirectX::XMMATRIX{DirectX::XMMatrixMultiply(offset, toRoot)};
			DirectX::XMStoreFloat4x4(&finalTransforms[i], DirectX::XMMatrixTranspose(finalTransform));
		}
	}

private:
	// Gives parentIndex of ith bone.
	std::vector<int> mBoneHierarchy;

	std::vector<DirectX::XMFLOAT4X4> mBoneOffsets;

	std::unordered_map<std::string, AnimationClip> mAnimations;
};
