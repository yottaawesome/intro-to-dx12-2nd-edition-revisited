export module shared:skinneddata;
import std;
import :win32;
import :mathhelper;

export
{
	///<summary>
	/// A Keyframe defines the bone transformation at an instant in time.
	///</summary>
	struct Keyframe
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
	struct BoneAnimation
	{
		auto GetStartTime() const -> float
		{
			// Keyframes are sorted by time, so first keyframe gives start time.
			return Keyframes.front().TimePos;
		}

		auto GetEndTime() const -> float
		{
			// Keyframes are sorted by time, so last keyframe gives end time.
			float f = Keyframes.back().TimePos;

			return f;
		}

		void Interpolate(float t, DirectX::XMFLOAT4X4& M) const
		{
			if (t <= Keyframes.front().TimePos)
			{
				DirectX::XMVECTOR S = DirectX::XMLoadFloat3(&Keyframes.front().Scale);
				DirectX::XMVECTOR P = DirectX::XMLoadFloat3(&Keyframes.front().Translation);
				DirectX::XMVECTOR Q = DirectX::XMLoadFloat4(&Keyframes.front().RotationQuat);

				DirectX::XMVECTOR zero = DirectX::XMVectorSet(0.0f, 0.0f, 0.0f, 1.0f);
				DirectX::XMStoreFloat4x4(&M, DirectX::XMMatrixAffineTransformation(S, zero, Q, P));
			}
			else if (t >= Keyframes.back().TimePos)
			{
				DirectX::XMVECTOR S = DirectX::XMLoadFloat3(&Keyframes.back().Scale);
				DirectX::XMVECTOR P = DirectX::XMLoadFloat3(&Keyframes.back().Translation);
				DirectX::XMVECTOR Q = DirectX::XMLoadFloat4(&Keyframes.back().RotationQuat);

				DirectX::XMVECTOR zero = DirectX::XMVectorSet(0.0f, 0.0f, 0.0f, 1.0f);
				DirectX::XMStoreFloat4x4(&M, DirectX::XMMatrixAffineTransformation(S, zero, Q, P));
			}
			else
			{
				for (Win32::UINT i = 0; i < Keyframes.size() - 1; ++i)
				{
					if (t >= Keyframes[i].TimePos && t <= Keyframes[i + 1].TimePos)
					{
						float lerpPercent = (t - Keyframes[i].TimePos) / (Keyframes[i + 1].TimePos - Keyframes[i].TimePos);

						DirectX::XMVECTOR s0 = DirectX::XMLoadFloat3(&Keyframes[i].Scale);
						DirectX::XMVECTOR s1 = DirectX::XMLoadFloat3(&Keyframes[i + 1].Scale);

						DirectX::XMVECTOR p0 = DirectX::XMLoadFloat3(&Keyframes[i].Translation);
						DirectX::XMVECTOR p1 = DirectX::XMLoadFloat3(&Keyframes[i + 1].Translation);

						DirectX::XMVECTOR q0 = DirectX::XMLoadFloat4(&Keyframes[i].RotationQuat);
						DirectX::XMVECTOR q1 = DirectX::XMLoadFloat4(&Keyframes[i + 1].RotationQuat);

						DirectX::XMVECTOR S = DirectX::XMVectorLerp(s0, s1, lerpPercent);
						DirectX::XMVECTOR P = DirectX::XMVectorLerp(p0, p1, lerpPercent);
						DirectX::XMVECTOR Q = DirectX::XMQuaternionSlerp(q0, q1, lerpPercent);

						DirectX::XMVECTOR zero = DirectX::XMVectorSet(0.0f, 0.0f, 0.0f, 1.0f);
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
	struct AnimationClip
	{
		auto GetClipStartTime() const -> float
		{
			// Find smallest start time over all bones in this clip.
			float t = MathHelper::Infinity;
			for (Win32::UINT i = 0; i < BoneAnimations.size(); ++i)
			{
				t = MathHelper::Min(t, BoneAnimations[i].GetStartTime());
			}

			return t;
		}
		auto GetClipEndTime() const -> float
		{
			// Find largest end time over all bones in this clip.
			float t = 0.0f;
			for (Win32::UINT i = 0; i < BoneAnimations.size(); ++i)
			{
				t = MathHelper::Max(t, BoneAnimations[i].GetEndTime());
			}
			return t;
		}

		void Interpolate(float t, std::vector<DirectX::XMFLOAT4X4>& boneTransforms) const
		{
			for (Win32::UINT i = 0; i < BoneAnimations.size(); ++i)
			{
				BoneAnimations[i].Interpolate(t, boneTransforms[i]);
			}
		}

		std::vector<BoneAnimation> BoneAnimations;
	};

	class SkinnedData
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
			Win32::UINT numBones = (Win32::UINT)mBoneOffsets.size();

			std::vector<DirectX::XMFLOAT4X4> toParentTransforms(numBones);

			// Interpolate all the bones of this clip at the given time instance.
			auto clip = mAnimations.find(clipName);
			clip->second.Interpolate(timePos, toParentTransforms);

			//
			// Traverse the hierarchy and transform all the bones to the root space.
			//

			std::vector<DirectX::XMFLOAT4X4> toRootTransforms(numBones);

			// The root bone has index 0.  The root bone has no parent, so its toRootTransform
			// is just its local bone transform.
			toRootTransforms[0] = toParentTransforms[0];

			// Now find the toRootTransform of the children.
			for (Win32::UINT i = 1; i < numBones; ++i)
			{
				DirectX::XMMATRIX toParent = DirectX::XMLoadFloat4x4(&toParentTransforms[i]);

				int parentIndex = mBoneHierarchy[i];
				DirectX::XMMATRIX parentToRoot = DirectX::XMLoadFloat4x4(&toRootTransforms[parentIndex]);

				DirectX::XMMATRIX toRoot = DirectX::XMMatrixMultiply(toParent, parentToRoot);

				DirectX::XMStoreFloat4x4(&toRootTransforms[i], toRoot);
			}

			// Premultiply by the bone offset transform to get the final transform.
			for (Win32::UINT i = 0; i < numBones; ++i)
			{
				DirectX::XMMATRIX offset = DirectX::XMLoadFloat4x4(&mBoneOffsets[i]);
				DirectX::XMMATRIX toRoot = DirectX::XMLoadFloat4x4(&toRootTransforms[i]);
				DirectX::XMMATRIX finalTransform = DirectX::XMMatrixMultiply(offset, toRoot);
				DirectX::XMStoreFloat4x4(&finalTransforms[i], DirectX::XMMatrixTranspose(finalTransform));
			}
		}

	private:
		// Gives parentIndex of ith bone.
		std::vector<int> mBoneHierarchy;

		std::vector<DirectX::XMFLOAT4X4> mBoneOffsets;

		std::unordered_map<std::string, AnimationClip> mAnimations;
	};
}
