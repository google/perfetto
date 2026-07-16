/*
 * Copyright (C) 2022 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef INCLUDE_PERFETTO_PUBLIC_ABI_EXPORT_H_
#define INCLUDE_PERFETTO_PUBLIC_ABI_EXPORT_H_

#ifdef _WIN32
#define PERFETTO_INTERNAL_DLL_EXPORT __declspec(dllexport)
#define PERFETTO_INTERNAL_DLL_IMPORT __declspec(dllimport)
#else
#define PERFETTO_INTERNAL_DLL_EXPORT __attribute__((visibility("default")))
#define PERFETTO_INTERNAL_DLL_IMPORT
#endif

// PERFETTO_SDK_EXPORT: Exports a symbol from the perfetto SDK shared library.
//
// The SDK is linked statically by default and PERFETTO_SDK_EXPORT expands to
// nothing. This is controlled by two defines (that likely come from the
// compiler command line):
// * PERFETTO_SDK_SHLIB: This must be defined when the SDK is built as a
//   shared library, both when compiling the library itself and when compiling
//   objects that use it.
// * PERFETTO_SDK_SHLIB_IMPLEMENTATION: This must additionally be defined when
//   compiling the shared library itself (in order to export the symbols), but
//   must be undefined when compiling objects that use the shared library (in
//   order to import the symbols).
#if defined(PERFETTO_SDK_SHLIB)
#if defined(PERFETTO_SDK_SHLIB_IMPLEMENTATION)
#define PERFETTO_SDK_EXPORT PERFETTO_INTERNAL_DLL_EXPORT
#else
#define PERFETTO_SDK_EXPORT PERFETTO_INTERNAL_DLL_IMPORT
#endif
#else  // !defined(PERFETTO_SDK_SHLIB)
#define PERFETTO_SDK_EXPORT
#endif  // !defined(PERFETTO_SDK_SHLIB)

#endif  // INCLUDE_PERFETTO_PUBLIC_ABI_EXPORT_H_
