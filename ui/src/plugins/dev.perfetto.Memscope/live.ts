// Copyright (C) 2026 The Android Open Source Project
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

// The live-session surface Memscope shares with other plugins (e.g. Sysmem):
// device connection, the live monitoring session, per-process heap profiling
// and the Memscope widget kit.

export {ConnectionPage} from './views/connection';
export {
  extractSnapshotData,
  LiveSession,
  type ProcessInfo,
  type SnapshotData,
} from './sessions/live_session';
export {
  LiveProfileView,
  type LiveSessionAttrs,
  SessionPill,
} from './views/live_chrome';
export * from './utils';
export {memscopeProcessHref, memscopeProductProcessUrl} from './process_focus';
export * from './process_data';
export {Billboard, BillboardStrip} from './components/billboard';
export {ColorChip, chipColor} from './components/color_chip';
export {HelpTooltip} from './components/help_tooltip';
export {Page} from './components/page';
export {Panel} from './components/panel';
export {PreviewBanner} from './components/preview_banner';
export {noData, xAxisFor} from './views/chart_axis';
export {renderSystemSummary} from './views/system_summary';
export {ProcessesTab} from './views/processes_table';
