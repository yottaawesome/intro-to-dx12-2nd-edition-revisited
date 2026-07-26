module;

#include "../Shaders/SharedTypes.h"

export module shared:sharedtypes;

export
{
	using 
		::PerPassCB,
		::MaterialData,
		::PerObjectCB,
		::BlurDispatchCB,
		::ShadowRayPayload,
		::ColorRayPayload,
		::GpuWavesCB,
		::InstanceData,
		::RTVertex,
		::GeoAttributes,
		::SsaoCB,
		::SkinnedCB,
		::ParticleEmitCB,
		::Particle,
		::ParticleDrawCB,
		::ParticleUpdateCB,
		::PerTerrainCB,
		::HelixParticlesCB
		;

	constexpr auto MaximumTerrainLayers = MaxTerrainLayers;
	constexpr auto MaxRecursionDepth = MAX_RECURSION_DEPTH;
}
