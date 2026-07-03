// Copyright (c) 2026, Vasilios Magriplis
// Licensed under the MIT License.
export module shared:build;

export
{
	constexpr auto IsDebugBuild =
#if defined(_DEBUG) || defined(DEBUG)
		true;
#else
		false;
#endif // _DEBUG
		;
	constexpr auto IsReleaseBuild = not IsDebugBuild;
	constexpr auto ForceWARPAdapter = false;
}