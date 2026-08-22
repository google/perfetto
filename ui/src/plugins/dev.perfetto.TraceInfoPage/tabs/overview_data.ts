// Copyright (C) 2025 The Android Open Source Project
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

// Data-loading and derivation helpers for the Overview tab. These live in their
// own leaf module (no widgets / no Mithril components) so that both the
// Overview tab and the Trace Doctor drawer can depend on them without creating
// an import cycle between overview.ts and diagnostics.ts.

import {
  LONG_NULL,
  NUM_NULL,
  STR_NULL,
} from '../../../trace_processor/query_result';
import type {Trace} from '../../../public/trace';
import type {duration} from '../../../base/time';
import type {TabKey} from '../utils';

export interface OverviewData {
  // Status counts
  importErrors: number;
  traceErrors: number;
  dataLosses: number;
  notices: number;
  uiLoadingErrorCount: number;
  // Metrics
  traceSizeBytes?: bigint;
  traceTypes: string[];
  uuid?: string;
  durationNs?: duration;
  schedDurationNs?: duration;
  // System information
  androidBuildFingerprint?: string;
  systemName?: string;
  systemMachine?: string;
  systemRelease?: string;
  // Multi-trace/machine counts
  traceCount: number;
  machineCount: number;
}

export interface StatusCardConfig {
  title: string;
  count: number;
  severity: 'success' | 'danger' | 'warning' | 'notice';
  icon: string;
  helpText: string;
  targetTab: TabKey;
}

export async function loadOverviewData(trace: Trace): Promise<OverviewData> {
  // Load everything in a single query
  const result = await trace.engine.query(`
    SELECT
      -- Status card counts
      (SELECT IFNULL(sum(value), 0) FROM stats WHERE severity = 'error' AND source = 'analysis') as import_errors,
      (SELECT IFNULL(sum(value), 0) FROM stats WHERE severity = 'error' AND source = 'trace') as trace_errors,
      (SELECT IFNULL(sum(value), 0) FROM stats WHERE severity = 'data_loss') as data_losses,
      (SELECT IFNULL(sum(value), 0) FROM stats WHERE severity = 'notice') as notices,
      -- Metrics
      extract_metadata('trace_size_bytes') as trace_size_bytes,
      extract_metadata('tracing_disabled_ns') -
        extract_metadata('tracing_started_ns') as duration_ns,
      (SELECT max(ts) - min(ts) FROM sched) as sched_duration_ns,
      -- System info for the host machine. label_index orders the host first
      -- (raw_id 0), falling back to the lowest-id machine when a merged trace
      -- has no host row. See the machine view in the prelude.
      (SELECT sysname FROM machine ORDER BY label_index LIMIT 1) as system_name,
      (SELECT release FROM machine ORDER BY label_index LIMIT 1) as system_release,
      (SELECT arch FROM machine ORDER BY label_index LIMIT 1) as system_machine,
      (SELECT android_build_fingerprint FROM machine ORDER BY label_index LIMIT 1)
        as android_build_fingerprint,
      (SELECT COUNT(DISTINCT trace_id) FROM metadata WHERE trace_id IS NOT NULL) as trace_count,
      (SELECT COUNT(DISTINCT machine_id) FROM metadata WHERE machine_id IS NOT NULL) as machine_count;
  `);

  const row = result.firstRow({
    import_errors: NUM_NULL,
    trace_errors: NUM_NULL,
    data_losses: NUM_NULL,
    notices: NUM_NULL,
    trace_size_bytes: LONG_NULL,
    duration_ns: LONG_NULL,
    sched_duration_ns: LONG_NULL,
    system_name: STR_NULL,
    system_release: STR_NULL,
    system_machine: STR_NULL,
    android_build_fingerprint: STR_NULL,
    trace_count: NUM_NULL,
    machine_count: NUM_NULL,
  });
  return {
    importErrors: row.import_errors ?? 0,
    traceErrors: row.trace_errors ?? 0,
    dataLosses: row.data_losses ?? 0,
    notices: row.notices ?? 0,
    uiLoadingErrorCount: trace.loadingErrors.length,
    traceSizeBytes: row.trace_size_bytes ?? undefined,
    traceTypes: trace.traceInfo.traceTypes,
    uuid: trace.traceInfo.uuid,
    durationNs: row.duration_ns ?? undefined,
    schedDurationNs: row.sched_duration_ns ?? undefined,
    androidBuildFingerprint: row.android_build_fingerprint ?? undefined,
    systemName: row.system_name ?? undefined,
    systemMachine: row.system_machine ?? undefined,
    systemRelease: row.system_release ?? undefined,
    traceCount: row.trace_count ?? 0,
    machineCount: row.machine_count ?? 0,
  };
}

export function createStatusCards(data: OverviewData): StatusCardConfig[] {
  const statusCards: StatusCardConfig[] = [
    {
      title: 'Import Errors',
      count: data.importErrors,
      severity: data.importErrors === 0 ? 'success' : 'danger',
      icon: data.importErrors === 0 ? 'check_circle' : 'error',
      helpText:
        'Errors encountered during trace import by the trace processor. These may indicate missing or corrupted data.',
      targetTab: data.importErrors > 0 ? 'import_errors' : 'stats',
    },
    {
      title: 'Trace Errors',
      count: data.traceErrors,
      severity: data.traceErrors === 0 ? 'success' : 'danger',
      icon: data.traceErrors === 0 ? 'check_circle' : 'error',
      helpText:
        'Errors that occurred during trace recording. These indicate problems during data collection.',
      targetTab: data.traceErrors > 0 ? 'trace_errors' : 'stats',
    },
    {
      title: 'Data Losses',
      count: data.dataLosses,
      severity: data.dataLosses === 0 ? 'success' : 'warning',
      icon: data.dataLosses === 0 ? 'check_circle' : 'warning',
      helpText:
        'Events that were dropped during trace recording due to buffer overflow or other issues.',
      targetTab: data.dataLosses > 0 ? 'data_losses' : 'stats',
    },
    {
      title: 'Notices',
      count: data.notices,
      severity: data.notices === 0 ? 'success' : 'notice',
      icon: data.notices === 0 ? 'check_circle' : 'info',
      helpText:
        'Normal but noteworthy conditions detected during recording or import, such as ftrace categories that failed to enable or packets skipped while incremental state was invalid. These are not errors.',
      targetTab: data.notices > 0 ? 'notices' : 'stats',
    },
  ];
  // Optional UI loading errors card - only show if there are errors
  if (data.uiLoadingErrorCount > 0) {
    statusCards.push({
      title: 'UI Loading Errors',
      count: data.uiLoadingErrorCount,
      severity: 'danger',
      icon: 'error',
      helpText:
        'Errors that occurred in the UI while loading or processing the trace.',
      targetTab: 'ui_loading_errors',
    });
  }
  return statusCards;
}
