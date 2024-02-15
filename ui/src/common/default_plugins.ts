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
  'dev.perfetto.AndroidBinderVizPlugin',
  'dev.perfetto.AndroidClientServer',
  'dev.perfetto.AndroidCujs',
  'dev.perfetto.AndroidLongBatteryTracing',
  'dev.perfetto.AndroidNetwork',
  'dev.perfetto.AndroidPerf',
  'dev.perfetto.AndroidPerfTraceCounters',
  'dev.perfetto.BookmarkletApi',
  'dev.perfetto.CoreCommands',
  'dev.perfetto.LargeScreensPerf',
  'perfetto.AndroidLog',
  'perfetto.Annotation',
  'perfetto.AsyncSlices',
  'perfetto.ChromeScrollJank',
  'perfetto.ChromeSlices',
  'perfetto.Counter',
  'perfetto.CpuFreq',
  'perfetto.CpuProfile',
  'perfetto.CpuSlices',
  'perfetto.CriticalUserInteraction',
  'perfetto.CustomSqlTrack',
  'perfetto.DebugSlices',
  'perfetto.Flows',
  'perfetto.Frames',
  'perfetto.FtraceRaw',
  'perfetto.HeapProfile',
  'perfetto.NullTrack',
  'perfetto.PerfSamplesProfile',
  'perfetto.PivotTable',
  'perfetto.ProcessSummary',
  'perfetto.Sched',
  'perfetto.Screenshots',
  'perfetto.ThreadState',
  'perfetto.VisualisedArgs',
];
