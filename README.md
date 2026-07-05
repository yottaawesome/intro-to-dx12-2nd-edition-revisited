# Revisited source code for _Introduction to 3D Game Programming with DirectX 12_

## Introduction

This is an ongoing effort at revisiting the source code for [Frank D. Luna's](https://www.d3dcoder.net/default.htm) second edition of [*Introduction to 3D Game Programming with DirectX 12*](https://www.d3dcoder.net/d3d12_v2.htm). Luna's books on DX12 (and its predecessors) are considered the de facto standard reference for learning DX12, and the primary intent here is to simplify and make the codebase easier to study (at least for me) by adapting the codebase to modern C++ standards and best practices. DirectX 12 is already a very difficult API to work with, and any way we can make it easier to understand is a good thing.

I began this effort with the original book, but have since moved on to the second edition after I discovered its existence.

## Status

The effort is currently in progress. The following projects have been converted and are functional.

* `Common` code moved to static library `Shared`;
* XMVECTOPR;
* XMMATRIX;
* InitDirect3D;
* Box;
* BoxGrid.

## Building and running

Microsoft Visual Studio 2026 with the _Desktop development with C++_ and _Game development with C++_ workloads is required. Prior to running, make sure to set the `Working Directory` setting `$(ProjectDir)\..` to allow the relevant resource files (like shaders) to be found. It goes without saying that you must have a GPU with DirectX 12 support to actually run the samples.

## Running summary of changes

* The common code has been moved into a shared library `Shared`, which exports the `shared` module, and which is consumed by the samples. Previously, the common files were included in each project via VCPROJ links, leading to unnecessarily increased build times when building all samples.
* Ported compiler settings from `c++17` to `c++latest`. This required fixing some compiler errors present in `c++20` and onward, mainly to do with temporaries being passed as arguments to functions.
* The code has been ported to C++20 modules (with the exception of the first DirectX math related ones), with platform and dependecies exported via modules. This has eliminated the need for separate header and source files, which has improved the code locality and significantly reduced the total LoC.
* `vcpkg` (in manifest mode) has been used to manage dependencies such as the `DirectX12 Toolkit` ([dx12tk](https://github.com/microsoft/directxtk12)) and `imgui` ([imgui](https://github.com/ocornut/imgui)). This has resulted in the removal of these files being bundled with the sample projects, which makes it more obvious what is sample code and what's dependency code.
* Use of `constexpr` where applicable, such as the replacement of the `Identity4x4()` function with a simpler `constexpr` variable.
* The DirectX Agility SDK has been updated to version 619.
* `MathHelper`'s `Min()`, `Max()` and `Clamp()` functions have been replaced with `std::min()`, `std::max()` and `std::clamp()` respectively.
* Removal of macros like `CALLBACK` and `WINAPI`, these are ignored for x64 builds and add visual noise.
* The original code used an obsolete version of `imgui`. Newer versions of `imgui` require the application to provide SRV allocation/deallocation functions; this is now done in the `D3DApp` superclass.
* Multiprocessor compilation has been enabled in the project settings.
* A lightweight `Event` class has been added to prevent a potential memory leak in `FlushCommandQueue()`.
* Function signatures are being updated to use trailing return type syntax, which are visually easier to navigate in classes with many methods.
* Various class-level `static` member variables have been made `inline`, simplifying their initialisation.
* Variable declarations are being ported to Almost Always Auto (AAA) idiom with braced or designated initialisation. AAA is easier to visually navigate, while braced initialisation guarantees left-to-right evaluation order and catches otherwise invisible narrowing conversions. Designated initialisation is much cleaner for aggregate types.
* Replacement of certain raw arrays with `std::array`, which also removes the need for the non-standard `_countof()` extension.
* The use of `!` is being with replaced with the more obvious `not`.
* Use of certain macros and preprocessor checks such as `#if defined(DEBUG)` have been replaced with `if constexpr(...)` checks, which are far less uglier.
* Use of `FLT_MAX` has been removed in favour of the standard `std::numeric_limits<float>::max()`.
* `main()` code has been ported to function-try-block syntax.
* General code cleanup. This includes removal of redundant conditionals, improvements to static initialisation, improvements to the `Random` class, and removal of declared but undefined functions that were found to not be used anywhere.

## License and copyright

I've preserved the relevant copyright notices for Luna's code. It's unclear what license applies to Luna's code, as no notices are posted anywhere including on the official site, but given there are various longstanding copies/remixes of Luna's code on Github, I assume the author is OK so long as they retain the copyright notices.

Code files I've exclusively authored (identifiable by the banner) are licensed under the MIT license.

## Additional resources

* [Microsoft's DirectX graphics samples](https://github.com/microsoft/DirectX-Graphics-Samples)
