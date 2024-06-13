// Copyright (C) 2024 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// This file contains a list of well known (to the core) track kinds.
// This file exists purely to keep legacy systems in place without introducing a
// ton of circular imports.
export const CPU_SLICE_TRACK_KIND = 'CpuSliceTrack';
export const THREAD_STATE_TRACK_KIND = 'ThreadStateTrack';
export const THREAD_SLICE_TRACK_KIND = 'ThreadSliceTrack';
export const EXPECTED_FRAMES_SLICE_TRACK_KIND = 'ExpectedFramesSliceTrack';
export const ACTUAL_FRAMES_SLICE_TRACK_KIND = 'ActualFramesSliceTrack';
export const ASYNC_SLICE_TRACK_KIND = 'AsyncSliceTrack';
export const PERF_SAMPLES_PROFILE_TRACK_KIND = 'PerfSamplesProfileTrack';
