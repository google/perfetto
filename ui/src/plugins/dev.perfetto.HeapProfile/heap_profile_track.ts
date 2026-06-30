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

import m from 'mithril';
import {materialColorScheme} from '../../components/colorizer';
import {Time, type time} from '../../base/time';
import {formatFileSize} from '../../base/file_utils';
import {SliceTrack, renderTooltip} from '../../components/tracks/slice_track';
import type {Trace} from '../../public/trace';
import {SourceDataset} from '../../trace_processor/dataset';
import {LONG, LONG_NULL, NUM, STR} from '../../trace_processor/query_result';
import type {FlamegraphState} from '../../widgets/flamegraph';
import {type ProfileDescriptor, profileDescriptor, ProfileType} from './common';
import {HeapProfileFlamegraphDetailsPanel} from './heap_profile_details_panel';

export function createHeapProfileTrack(
  trace: Trace,
  uri: string,
  tableName: string,
  upid: number,
  heapProfileIsIncomplete: boolean,
  detailsPanelState: FlamegraphState | undefined,
  onDetailsPanelStateChange: (state: FlamegraphState) => void,
  onNodeSelected?: (args: {
    pathHashes: string;
    isDominator: boolean;
    upid: number;
    ts: time;
  }) => void,
) {
  return SliceTrack.create({
    trace,
    uri,
    dataset: new SourceDataset({
      src: tableName,
      schema: {
        ts: LONG,
        dur: LONG,
        type: STR,
        id: NUM,
        // Per-slice deltas: bytes allocated in the interval, net bytes still
        // held afterwards (unreleased), and the allocation count. Null for
        // heap dumps and OOME callstacks, which carry no allocation total.
        summary_alloc_size: LONG_NULL,
        summary_net_size: LONG_NULL,
        summary_alloc_count: LONG_NULL,
      },
      filter: {col: 'upid', eq: upid},
    }),
    detailsPanel: (row) => {
      const ts = Time.fromRaw(row.ts);
      const tsEnd = Time.fromRaw(row.ts + row.dur);
      const descriptor = profileDescriptor(row.type);
      return new HeapProfileFlamegraphDetailsPanel(
        trace,
        heapProfileIsIncomplete,
        upid,
        descriptor,
        ts,
        tsEnd,
        detailsPanelState,
        onDetailsPanelStateChange,
        onNodeSelected,
      );
    },
    sliceName: (row) =>
      heapProfileTitle(
        profileDescriptor(row.type),
        row.summary_alloc_size,
        row.summary_net_size,
      ) ?? '',
    tooltip: (slice) => {
      const descriptor = profileDescriptor(slice.row.type);
      return renderTooltip(trace, slice, {
        title: descriptor.label,
        extras: heapProfileTooltip(
          descriptor,
          slice.row.summary_alloc_size,
          slice.row.summary_net_size,
          slice.row.summary_alloc_count,
        ),
      });
    },
    colorizer: (slice) => {
      return materialColorScheme(slice.ts.toString());
    },
  });
}

// Title drawn on each heap-profile slice, summarising that slice's dump only.
// Native/generic profiles show both what was allocated in the interval and
// what is still held; ART allocation samples have no frees, so allocated and
// unreleased coincide. Returns undefined for profile types with no allocation
// total (ART heap dumps, OOME callstacks).
function heapProfileTitle(
  descriptor: ProfileDescriptor,
  allocSize: bigint | null,
  netSize: bigint | null,
): string | undefined {
  if (allocSize === null || netSize === null) return undefined;
  switch (descriptor.type) {
    case ProfileType.NATIVE_HEAP_PROFILE:
    case ProfileType.GENERIC_HEAP_PROFILE:
      return `${formatFileSize(netSize)} unreleased, ${formatFileSize(allocSize)} allocated`;
    case ProfileType.JAVA_HEAP_SAMPLES:
      return `${formatFileSize(allocSize)} allocated`;
    case ProfileType.JAVA_HEAP_GRAPH:
    case ProfileType.OOME_CALLSTACK:
      return undefined;
  }
}

// Fuller breakdown for the hover tooltip, including bytes freed in the interval
// (derived as allocated - net) and the allocation count.
function heapProfileTooltip(
  descriptor: ProfileDescriptor,
  allocSize: bigint | null,
  netSize: bigint | null,
  allocCount: bigint | null,
): m.Children {
  if (allocSize === null || netSize === null) return undefined;
  const count =
    allocCount !== null
      ? ` (${Number(allocCount).toLocaleString()} allocations)`
      : '';
  switch (descriptor.type) {
    case ProfileType.NATIVE_HEAP_PROFILE:
    case ProfileType.GENERIC_HEAP_PROFILE:
      return m(
        '',
        `${formatFileSize(netSize)} unreleased, ` +
          `${formatFileSize(allocSize)} allocated, ` +
          `${formatFileSize(allocSize - netSize)} freed${count}`,
      );
    case ProfileType.JAVA_HEAP_SAMPLES:
      return m('', `${formatFileSize(allocSize)} allocated${count}`);
    case ProfileType.JAVA_HEAP_GRAPH:
    case ProfileType.OOME_CALLSTACK:
      return undefined;
  }
}
