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
		::GpuWavesCB,
		::InstanceData,
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
}
