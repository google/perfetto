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

import m from 'mithril';
import type {Engine} from '../../../trace_processor/engine';
import {NUM_NULL, STR, STR_NULL} from '../../../trace_processor/query_result';
import {Section} from '../../../widgets/section';
import {Grid, GridCell, GridHeaderCell} from '../../../widgets/grid';

// A single metadata entry with its display value already coalesced and its
// display label derived from the key name.
interface MetadataEntry {
  readonly name: string;
  readonly value: string;
  readonly label: string;
  readonly traceId: number | null;
  readonly machineId: number | null;
}

// A group of metadata entries. A section without a title is rendered without a
// header (used when everything landed in the "Other" bucket).
interface MetadataSection {
  readonly title?: string;
  readonly description?: string;
  readonly entries: ReadonlyArray<MetadataEntry>;
}

export interface MetadataData {
  readonly sections: ReadonlyArray<MetadataSection>;
  readonly isMultiTrace: boolean;
  readonly isMultiMachine: boolean;
}

// Curated sections for the well-known metadata keys. Each entry matches a key
// either by exact name or by prefix; the first matching section wins and any
// key that matches nothing falls into the "Other" section. `strip` removes a
// redundant namespace prefix from the displayed label (the section title
// already conveys it).
interface SectionDef {
  readonly title: string;
  readonly description: string;
  readonly matches: (name: string) => boolean;
  readonly strip?: string;
}

// Keys that are never shown on the metadata tab. trace_config_pbtxt is a large
// serialized blob better viewed on the dedicated Trace Config tab.
const EXCLUDED_KEYS = new Set<string>(['trace_config_pbtxt']);

const TRACE_KEYS = new Set<string>([
  'trace_uuid',
  'trace_type',
  'trace_size_bytes',
  'trace_trigger',
  'trace_time_clock_id',
  'unique_session_name',
  'ui_state',
  'range_of_interest_start_us',
  'statsd_triggering_subscription_id',
]);

const DEVICE_KEYS = new Set<string>([
  'timezone_off_mins',
  'tracing_service_version',
]);

const SECTION_DEFS: ReadonlyArray<SectionDef> = [
  {
    title: 'Trace',
    description: 'Identity and configuration of the trace itself.',
    matches: (n) => TRACE_KEYS.has(n),
  },
  {
    title: 'Device & OS',
    description: 'The operating system and hardware the trace was recorded on.',
    matches: (n) =>
      n.startsWith('system_') || n.startsWith('android_') || DEVICE_KEYS.has(n),
  },
  {
    title: 'Benchmark',
    description: 'Parameters of the benchmark run that produced the trace.',
    matches: (n) => n.startsWith('benchmark_'),
  },
  {
    title: 'Chrome',
    description: 'Metadata reported by Chrome.',
    matches: (n) => n.startsWith('cr-'),
    strip: 'cr-',
  },
];

const OTHER_TITLE = 'Other';
const OTHER_DESCRIPTION =
  'Metadata keys that do not fall into a known category.';

const metadataRowSpec = {
  name: STR,
  value: STR_NULL,
  traceId: NUM_NULL,
  machineId: NUM_NULL,
};

export async function loadMetadataData(engine: Engine): Promise<MetadataData> {
  const result = await engine.query(`
    select
      name,
      ifnull(str_value, cast(int_value as text)) as value,
      trace_id as traceId,
      machine_id as machineId
    from metadata
    order by name
  `);

  const traceIds = new Set<number>();
  const machineIds = new Set<number>();
  const buckets = new Map<string, MetadataEntry[]>();
  const other: MetadataEntry[] = [];

  for (const it = result.iter(metadataRowSpec); it.valid(); it.next()) {
    const name = it.name;
    if (EXCLUDED_KEYS.has(name)) {
      continue;
    }
    if (it.traceId !== null) {
      traceIds.add(it.traceId);
    }
    if (it.machineId !== null) {
      machineIds.add(it.machineId);
    }
    const base = {
      name,
      value: it.value ?? '',
      traceId: it.traceId,
      machineId: it.machineId,
    };
    const def = SECTION_DEFS.find((d) => d.matches(name));
    if (def === undefined) {
      other.push({...base, label: name});
      continue;
    }
    const label =
      def.strip !== undefined && name.startsWith(def.strip)
        ? name.substring(def.strip.length)
        : name;
    const entries = buckets.get(def.title) ?? [];
    entries.push({...base, label});
    buckets.set(def.title, entries);
  }

  const byLabel = (a: MetadataEntry, b: MetadataEntry) =>
    a.label.localeCompare(b.label);

  const sections: MetadataSection[] = [];
  for (const def of SECTION_DEFS) {
    const entries = buckets.get(def.title);
    if (entries !== undefined && entries.length > 0) {
      sections.push({
        title: def.title,
        description: def.description,
        entries: entries.sort(byLabel),
      });
    }
  }
  if (other.length > 0) {
    sections.push({
      title: OTHER_TITLE,
      description: OTHER_DESCRIPTION,
      entries: other.sort(byLabel),
    });
  }

  // If nothing matched a curated section, there is only the "Other" bucket;
  // drop its header and show a plain list.
  if (sections.length === 1 && sections[0].title === OTHER_TITLE) {
    sections[0] = {entries: sections[0].entries};
  }

  return {
    sections,
    isMultiTrace: traceIds.size > 1,
    isMultiMachine: machineIds.size > 1,
  };
}

export function hasMetadataData(data?: MetadataData): boolean {
  return (data?.sections.length ?? 0) > 0;
}

export interface MetadataTabAttrs {
  readonly data: MetadataData;
}

export class MetadataTab implements m.ClassComponent<MetadataTabAttrs> {
  view({attrs}: m.CVnode<MetadataTabAttrs>) {
    const {sections, isMultiTrace, isMultiMachine} = attrs.data;
    return m(
      '.pf-trace-info-page__tab-content',
      sections.map((section) => {
        const grid = renderGrid(section.entries, isMultiTrace, isMultiMachine);
        return section.title === undefined
          ? grid
          : m(
              Section,
              {title: section.title, subtitle: section.description},
              grid,
            );
      }),
    );
  }
}

function renderGrid(
  entries: ReadonlyArray<MetadataEntry>,
  isMultiTrace: boolean,
  isMultiMachine: boolean,
): m.Children {
  return m(Grid, {
    columns: [
      ...(isMultiTrace
        ? [{key: 'trace', header: m(GridHeaderCell, 'Trace')}]
        : []),
      ...(isMultiMachine
        ? [{key: 'machine', header: m(GridHeaderCell, 'Machine')}]
        : []),
      {key: 'key', header: m(GridHeaderCell, 'Key')},
      {key: 'value', header: m(GridHeaderCell, 'Value')},
    ],
    rowData: entries.map((e) => {
      const cells = [];
      if (isMultiTrace) {
        cells.push(m(GridCell, e.traceId === null ? '-' : e.traceId));
      }
      if (isMultiMachine) {
        cells.push(m(GridCell, e.machineId === null ? '-' : e.machineId));
      }
      cells.push(
        m(GridCell, {className: 'pf-trace-info-page__grid-key'}, e.label),
        m(GridCell, e.value),
      );
      return cells;
    }),
    className: 'pf-trace-info-page__dense-grid',
  });
}
