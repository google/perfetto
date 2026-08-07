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

import './styles.scss';
import m from 'mithril';

import type {PerfettoPlugin} from '../../public/plugin';
import type {Trace} from '../../public/trace';
import {NUM, STR} from '../../trace_processor/query_result';
import type {Row} from '../../trace_processor/query_result';
import HeapProfilePlugin, {
  traceHasTimelineData,
} from '../dev.perfetto.HeapProfile';
import {AggregateProfilesPage} from './aggregate_profiles_page';
import {AggregateProfilesMergePage} from './merge_page';
import {aggregateProfileMetric} from './metrics';
import {
  type AggregateProfilesPageState,
  AGGREGATE_PROFILES_PAGE_STATE_SCHEMA,
  type MergeColumn,
  type MergePageState,
  type MergeProfile,
  type MergeProfileMetric,
  MERGE_PAGE_STATE_SCHEMA,
  type SampleType,
} from './types';
import type {Store} from '../../base/store';
import {ensureExists} from '../../base/assert';

interface LoadedProfiles {
  readonly profiles: MergeProfile[];
  readonly sampleTypes: SampleType[];
  readonly columns: MergeColumn[];
  readonly rows: Row[];
}

// Views the aggregate profiles (pprof, collapsed stack, ...) in the trace.
// A single profile gets a flamegraph with a metric selector; an archive of
// many gets a page that filters and merges them (see merge_page).
export default class implements PerfettoPlugin {
  static readonly id = 'dev.perfetto.AggregateProfiles';
  static readonly dependencies = [HeapProfilePlugin];
  private store?: Store<AggregateProfilesPageState>;
  private mergeStore?: Store<MergePageState>;

  async onTraceLoad(trace: Trace): Promise<void> {
    const scopes = await trace.engine.query(
      'SELECT count(DISTINCT scope) AS n FROM __intrinsic_aggregate_profile',
    );
    const profileCount = scopes.firstRow({n: NUM}).n;
    if (profileCount === 0) {
      return;
    }
    if (profileCount === 1) {
      await this.registerSingleProfilePage(trace);
    } else {
      await this.registerMergePage(trace);
    }
    trace.sidebar.addMenuItem({
      section: 'current_trace',
      sortOrder: 11,
      text: 'Aggregate Profiles',
      href: '#!/aggregateprofiles',
      icon: 'analytics',
    });
    if (!(await traceHasTimelineData(trace))) {
      // Profile-only traces land here rather than on the empty timeline.
      trace.initialPage.suggest('/aggregateprofiles', 10);
    }
  }

  private async registerSingleProfilePage(trace: Trace): Promise<void> {
    this.store = trace.mountStore('dev.perfetto.AggregateProfiles', (init) => {
      const parsed = AGGREGATE_PROFILES_PAGE_STATE_SCHEMA.safeParse(init);
      return parsed.data ?? {};
    });
    const profiles = await this.getProfiles(trace);
    const store = ensureExists(this.store);
    trace.pages.registerPage({
      route: '/aggregateprofiles',
      render: () =>
        m(AggregateProfilesPage, {
          trace,
          state: store.state,
          onStateChange: (state: AggregateProfilesPageState) => {
            store.edit((draft) => {
              draft.selectedProfileId = state.selectedProfileId;
              draft.flamegraphState = state.flamegraphState;
            });
          },
          profiles,
        }),
    });
  }

  private async registerMergePage(trace: Trace): Promise<void> {
    const loaded = await loadMergeProfiles(trace);
    // The callstack forest build in this module is the expensive one-off that
    // every flamegraph query needs; do it at load so the page stays responsive.
    await trace.engine.query(
      'include perfetto module callstacks.stack_profile',
    );
    this.mergeStore = trace.mountStore(
      'dev.perfetto.AggregateProfiles.merge',
      (init) => {
        const parsed = MERGE_PAGE_STATE_SCHEMA.safeParse(init);
        return parsed.success ? parsed.data : MERGE_PAGE_STATE_SCHEMA.parse({});
      },
    );
    const store = ensureExists(this.mergeStore);
    trace.pages.registerPage({
      route: '/aggregateprofiles',
      render: () =>
        m(AggregateProfilesMergePage, {
          trace,
          profiles: loaded.profiles,
          sampleTypes: loaded.sampleTypes,
          columns: loaded.columns,
          rows: loaded.rows,
          state: store.state,
          onStateChange: (s: MergePageState) => {
            store.edit((draft) => {
              draft.flamegraphState = s.flamegraphState;
              draft.merge = s.merge;
              draft.columns = s.columns;
              draft.filters = s.filters;
            });
          },
        }),
    });
  }

  private async getProfiles(trace: Trace) {
    const result = await trace.engine.query(
      'SELECT DISTINCT scope FROM __intrinsic_aggregate_profile ORDER BY scope',
    );
    const profiles = [];
    for (const it = result.iter({scope: STR}); it.valid(); it.next()) {
      const metrics = await getProfileMetrics(trace, it.scope);
      if (metrics.length > 0) {
        profiles.push({
          id: `profile_${it.scope}`,
          displayName: it.scope,
          metrics,
        });
      }
    }
    return profiles;
  }
}

async function getProfileMetrics(trace: Trace, scope: string) {
  const result = await trace.engine.query(`
    SELECT
      id,
      sample_type_unit,
      sample_type_type || ' (' || sample_type_unit || ')' as display_name
    FROM __intrinsic_aggregate_profile
    WHERE scope = '${scope}'
    ORDER BY sample_type_type
  `);
  const metrics = [];
  for (
    const it = result.iter({
      id: NUM,
      sample_type_unit: STR,
      display_name: STR,
    });
    it.valid();
    it.next()
  ) {
    metrics.push(
      aggregateProfileMetric(it.display_name, it.sample_type_unit, [it.id]),
    );
  }
  return metrics;
}

// Loads each profile's sample-type totals as DataGrid rows + columns. Just a
// grouped scan; the callstack tables are only touched for the selected subset,
// so this scales to thousands of profiles.
async function loadMergeProfiles(trace: Trace): Promise<LoadedProfiles> {
  const byScope = new Map<string, Map<string, MergeProfileMetric>>();
  const sampleKinds = new Map<string, SampleType>();
  const result = await trace.engine.query(`
    SELECT
      ap.scope AS scope,
      ap.id AS agg_id,
      ap.sample_type_type AS type,
      ap.sample_type_unit AS unit,
      coalesce(sum(s.value), 0) AS total,
      count(s.id) AS n
    FROM __intrinsic_aggregate_profile ap
    LEFT JOIN __intrinsic_aggregate_sample s
      ON s.aggregate_profile_id = ap.id
    GROUP BY ap.id
    ORDER BY ap.scope
  `);
  for (
    const it = result.iter({
      scope: STR,
      agg_id: NUM,
      type: STR,
      unit: STR,
      total: NUM,
      n: NUM,
    });
    it.valid();
    it.next()
  ) {
    const key = `${it.type} (${it.unit})`;
    sampleKinds.set(key, {key, type: it.type, unit: it.unit});
    let sampleTypes = byScope.get(it.scope);
    if (sampleTypes === undefined) {
      sampleTypes = new Map();
      byScope.set(it.scope, sampleTypes);
    }
    sampleTypes.set(key, {aggId: it.agg_id, total: it.total, count: it.n});
  }

  const profiles: MergeProfile[] = Array.from(
    byScope.entries(),
    ([scope, sampleTypes]) => ({scope, sampleTypes}),
  );

  const columns: MergeColumn[] = [{field: 'c0', title: 'profile', kind: 'id'}];
  for (const st of sampleKinds.values()) {
    columns.push({
      field: `c${columns.length}`,
      title: st.key,
      kind: 'numeric',
      unit: st.unit,
      sampleKey: st.key,
    });
  }

  const rows: Row[] = profiles.map((p) => {
    const row: Row = {c0: p.scope};
    for (const c of columns) {
      if (c.sampleKey !== undefined) {
        row[c.field] = p.sampleTypes.get(c.sampleKey)?.total ?? null;
      }
    }
    return row;
  });

  return {
    profiles,
    sampleTypes: Array.from(sampleKinds.values()),
    columns,
    rows,
  };
}
