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
		::PerTerrainCB
		;

	constexpr auto MaximumTerrainLayers = MaxTerrainLayers;
}
