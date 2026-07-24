# Revisited source code for _Introduction to 3D Game Programming with DirectX 12_

## Introduction

This is an ongoing effort at revisiting the source code for [Frank D. Luna's](https://www.d3dcoder.net/default.htm) second edition of [*Introduction to 3D Game Programming with DirectX 12*](https://www.d3dcoder.net/d3d12_v2.htm). Luna's books on DX12 (and its predecessors) are considered the de facto standard reference for learning DX12, and the primary intent here is to simplify and make the codebase easier to study (at least for me) by adapting the codebase to modern C++ standards and best practices. DirectX 12 is already a very difficult API to work with, and any way we can make it easier to understand is a good thing.

I began this effort with the original book, but have since moved on to the second edition after I discovered its existence.

## Status

The effort is currently in progress. The following projects have been converted and are functional.

* [Common](./src/Shared)
* [01/XMVECTOR](./src/01/XMVECTOR)
* [02/XMMATRIX](./src/02/XMMATRIX)
* [04/InitDirect3D](./src/04/InitDirect3D)
* [06/Box](./src/06/Box)
* [06/BoxGrid](./src/06/BoxGrid)
* [07/Shapes](./src/07/Shapes)
* [07/Waves](./src/07/Waves)
* [08/LitShapes](./src/08/LitShapes)
* [08/LitWaves](./src/08/LitWaves)
* [09/Crate](./src/09/Crate)
* [09/TexturedShapes](./src/09/TexturedShapes)
* [09/TexWaves](./src/09/TexWaves)
* [10/BlendDemo](./src/10/BlendDemo)
* [11/Stenciling](./src/11/Stenciling)
* [12/BillboardGS](./src/12/BillboardGS)
* [13/Blur](./src/13/Blur)
* [13/VecAddCS](./src/13/VecAddCS)
* [13/WavesCS](./src/13/WavesCS)
* [14/BasicTesselation](./src/14/BasicTesselation)
* [14/BezierPatch](./src/14/BezierPatch)
* [16/InstancingAndCulling](./src/16/InstancingAndCulling)
* [17/Picking](./src/17/Picking)
* [18/CubeAndNormalMaps](./src/18/CubeAndNormalMaps)
* [18/DynamicCubeMap](./src/18/DynamicCubeMap)
* [19/DisplacementMapping](./src/19/DisplacementMapping)
* [20/Shadows](./src/20/Shadows)
* [21/Ssao](./src/21/Ssao)
* [22/QuatApp](./src/22/QuatApp)

## Building and running

Microsoft Visual Studio 2026 with the _Desktop development with C++_ and _Game development with C++_ workloads is required. The projects depend on the DirectX 12 Agility nuget package. You can either restore the package manually in your environment or run `.\Restore-NuGetPackages.ps1` from the `src` directory. To test a clean restore, run `.\Restore-NuGetPackages.ps1 -Clean`; the script finds MSBuild through the current environment or Visual Studio's `vswhere.exe`. Prior to running a project, make sure to set the `Working Directory` setting to `$(ProjectDir)..\..` to allow the relevant resource files (like shaders) to be found. It goes without saying that you must have a GPU with DirectX 12 support to actually run the samples.

## Running summary of changes

* The common code has been moved into a shared library `Shared`, which exports the `shared` module, and which is consumed by the samples. Previously, the common files were included in each project via VCPROJ links, leading to unnecessarily increased build times when building all samples.
* Ported compiler settings from `c++17` to `c++latest`. This required fixing some compiler errors present in `c++20` and onward, mainly to do with temporaries being passed as arguments to functions.
* The code has been ported to C++20 modules (with the exception of the first DirectX math related ones), with platform and dependecies exported via modules. This has eliminated the need for separate header and source files, which has improved the code locality and significantly reduced the total LoC.
* `vcpkg` (in manifest mode) has been used to manage dependencies such as the `DirectX12 Toolkit` ([dx12tk](https://github.com/microsoft/directxtk12)) and `imgui` ([imgui](https://github.com/ocornut/imgui)). This has resulted in the removal of these files being bundled with the sample projects, which makes it more obvious what is sample code and what's dependency code.
* Use of `constexpr` where applicable, such as the replacement of the `Identity4x4()` function with a simpler `constexpr` variable.
* `Initialize()` has been moved to a private method in the demo subclasses and is now invoked automatically by the relevant constructor, removing the need to invoke it from `main()` as a two-stage initialization process.
* The DirectX Agility SDK has been updated to version 619.
* `MathHelper`'s `Min()`, `Max()` and `Clamp()` functions have been replaced with `std::min()`, `std::max()` and `std::clamp()` respectively.
* The `Waves` class was repeated across multiple demos. Rather than have copies of it in the different projects, I've consolidated it into Shared to reduce the noise.
* Removal of macros like `CALLBACK` and `WINAPI`, these are ignored for x64 builds and add visual noise.
* The original code used an obsolete version of `imgui`. Newer versions of `imgui` require the application to provide SRV allocation/deallocation functions; this is now done in the `D3DApp` superclass.
* Multiprocessor compilation has been enabled in the project settings.
* A back buffer state transition bug in the Blur demo is now fixed. The bug didn't crash the demo, but caused the DX12 runtime to log debug error messages.
* A lightweight `Event` class has been added to prevent a potential memory leak in `FlushCommandQueue()`.
* The unused struct `WaveDispatchCB` was removed from the Blur and WavesCS demos.
* The `ShadowMap` class was duplicated across all the demos starting from `ShadowMap` one. Rather than have this duplicated, I moved it to the `Shared` library.
* Function signatures are being updated to use trailing return type syntax, which are visually easier to navigate in classes with many methods.
* Various class-level `static` member variables have been made `inline`, simplifying their initialisation.
* Variable declarations are being ported to Almost Always Auto (AAA) idiom with braced or designated initialisation. AAA is easier to visually navigate, while braced initialisation guarantees left-to-right evaluation order and catches otherwise invisible narrowing conversions. Designated initialisation is much cleaner for aggregate types.
* `MeshGen::CreateBox()` has been updated to use `std::begin()` and `std::end()` in the `assign()` calls. The previous code did a direct index past the end of the range, which interestingly triggered a debug check, even though it's technically safe.
* Replacement of certain raw arrays with `std::array`, which also removes the need for the non-standard `_countof()` extension.
* `Texture::Info()` returned a reference to a temporary, which was undefined behaviour. This has been fixed.
* `SkinnedMesh::LoadSkinnedModel()` now throws an exception if it fails to load the model, instead of returning `false`.
* The use of `!` is being with replaced with the more obvious `not`.
* Use of certain macros and preprocessor checks such as `#if defined(DEBUG)` have been replaced with `if constexpr(...)` checks, which are far less uglier.
* `DxException` has been cleaned up, with the reliance on macros for lines and file numbers replaced by the standard `std::source_location` object.
* `D3DApp`'s `Initialize()`, `InitMainWindow()`, and `InitDirect3D()` were a mix of exception throwing and returning `bool`s on error. The `bool` return has been removed and now exceptions are thrown consistently. This also eliminates the need to check the return value of `Initialize()`.
* Use of `FLT_MAX` has been removed in favour of the standard `std::numeric_limits<float>::max()`.
* `main()` code has been ported to function-try-block syntax.
* General code cleanup. This includes removal of redundant conditionals, improvements to static initialisation, improvements to the `Random` class, and removal of declared but undefined functions that were found to not be used anywhere.

## License and copyright

I've preserved the relevant copyright notices for Luna's code. It's unclear what license applies to Luna's code, as no notices are posted anywhere including on the official site, but given there are various longstanding copies/remixes of Luna's code on Github, I assume the author is OK so long as they retain the copyright notices.

Code files I've exclusively authored (identifiable by the banner) are licensed under the MIT license.

## Additional resources

* [Microsoft's DirectX graphics samples](https://github.com/microsoft/DirectX-Graphics-Samples)
