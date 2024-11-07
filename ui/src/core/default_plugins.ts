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

// Plugins in this list will run by default when users boot up the UI.
// Users may choose to enable plugins which are not in this list, but they will
// need to do this manually.
// In order to get a plugin into this list it must:
// - Use only the available plugin API (no hacks).
// - Follow naming conventions for tracks and plugins.
// - Not directly rely on any other plugins.
// - Be approved by one of Perfetto UI owners.
export const defaultPlugins = [
  'com.android.GpuWorkPeriod',
  'com.google.PixelMemory',
  'dev.perfetto.AndroidBinderVizPlugin',
  'dev.perfetto.AndroidClientServer',
  'dev.perfetto.AndroidCujs',
  'dev.perfetto.AndroidDmabuf',
  'dev.perfetto.AndroidLog',
  'dev.perfetto.AndroidLongBatteryTracing',
  'dev.perfetto.AndroidNetwork',
  'dev.perfetto.AndroidPerf',
  'dev.perfetto.AndroidPerfTraceCounters',
  'dev.perfetto.AndroidStartup',
  'dev.perfetto.AsyncSlices',
  'dev.perfetto.BookmarkletApi',
  'dev.perfetto.Counter',
  'dev.perfetto.CpuFreq',
  'dev.perfetto.CpuProfile',
  'dev.perfetto.CpuSlices',
  'dev.perfetto.CriticalPath',
  'dev.perfetto.DebugTracks',
  'dev.perfetto.DeeplinkQuerystring',
  'dev.perfetto.FlagsPage',
  'dev.perfetto.Frames',
  'dev.perfetto.Ftrace',
  'dev.perfetto.HeapProfile',
  'dev.perfetto.LargeScreensPerf',
  'dev.perfetto.MetricsPage',
  'dev.perfetto.PerfSamplesProfile',
  'dev.perfetto.PinAndroidPerfMetrics',
  'dev.perfetto.PinSysUITracks',
  'dev.perfetto.Process',
  'dev.perfetto.ProcessSummary',
  'dev.perfetto.ProcessThreadGroups',
  'dev.perfetto.QueryPage',
  'dev.perfetto.RestorePinnedTrack',
  'dev.perfetto.Sched',
  'dev.perfetto.Screenshots',
  'dev.perfetto.Thread',
  'dev.perfetto.ThreadState',
  'dev.perfetto.TimelineSync',
  'dev.perfetto.TraceInfoPage',
  'dev.perfetto.TraceMetadata',
  'dev.perfetto.VizPage',
  'org.chromium.CriticalUserInteraction',
  'org.kernel.LinuxKernelSubsystems',
  'org.kernel.SuspendResumeLatency',
  'perfetto.CoreCommands',
  'perfetto.ExampleTraces',
  'perfetto.GlobalGroups',
  'perfetto.TrackUtils',
];
