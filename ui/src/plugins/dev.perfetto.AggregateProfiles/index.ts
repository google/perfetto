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

import {
  DEFAULT_FLAMEGRAPH_COLLECTION_STATE,
  FlamegraphCollection,
} from '../../components/flamegraph_collection';
import type {
  FlamegraphCollectionState,
  FlamegraphCollectionColumn,
} from '../../components/flamegraph_collection';
import type {TreeExplorerQueryMetric} from '../../components/tree_explorer_fetcher';
import type {PerfettoPlugin} from '../../public/plugin';
import type {Trace} from '../../public/trace';
import {NUM, STR} from '../../trace_processor/query_result';
import type {Row} from '../../trace_processor/query_result';
import {Anchor} from '../../widgets/anchor';
import {AggregateProfilesPage} from './aggregate_profiles_page';
import {
  type AggregateProfilesPageState,
  AGGREGATE_PROFILES_PAGE_STATE_SCHEMA,
  type MergeColumn,
  type MergeProfile,
  type MergeProfileMetric,
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

// A single profile gets a flamegraph with a metric selector; an archive of
// many gets a FlamegraphCollection page that filters and merges them.
export default class implements PerfettoPlugin {
  static readonly id = 'dev.perfetto.AggregateProfiles';
  private store?: Store<AggregateProfilesPageState>;

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
    trace.onTraceReady.addListener(async () => {
      const hasAnyTracks = trace.workspaces.all[0].flatTracks.length > 0;
      // TODO(lalitm): it's really bad that we're unconditionally navigating
      // to the profiles page: really we should check if the user has not already
      // set a page and then only navigate if no page is set. However:
      //  a) no API exists for checking the current page
      //  b) there is already some code in UI load time which navigates
      //     to the viewer page so we would always fail this check.
      // So for now just leave this as-is.
      if (!hasAnyTracks && profileCount > 0) {
        trace.navigate('#!/aggregateprofiles');
      }
    });
  }

  private migratePageState(init: unknown): AggregateProfilesPageState {
    const result = AGGREGATE_PROFILES_PAGE_STATE_SCHEMA.safeParse(init);
    return result.data ?? {};
  }

  private async registerSingleProfilePage(trace: Trace): Promise<void> {
    this.store = trace.mountStore('dev.perfetto.AggregateProfiles', (init) =>
      this.migratePageState(init),
    );
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
    // The expensive one-off every flamegraph query needs; do it at load.
    await trace.engine.query(
      'include perfetto module callstacks.stack_profile',
    );
    // Lives as long as the page is mounted, which is the rest of the session
    // (a visited page stays mounted behind a Gate).
    let state = DEFAULT_FLAMEGRAPH_COLLECTION_STATE;
    const byScope = new Map(loaded.profiles.map((p) => [p.scope, p]));
    // One metric per sample type, summing the given profiles.
    const metricsForKeys = (keys: ReadonlyArray<string>) => {
      const metrics: TreeExplorerQueryMetric[] = [];
      for (const st of loaded.sampleTypes) {
        const ids = keys
          .map((k) => byScope.get(k)?.sampleTypes.get(st.key)?.aggId)
          .filter((id): id is number => id !== undefined);
        if (ids.length > 0) {
          metrics.push(this.aggregateProfileMetric(st.key, st.unit, ids));
        }
      }
      return metrics;
    };
    const columns: ReadonlyArray<FlamegraphCollectionColumn> = loaded.columns;
    trace.pages.registerPage({
      route: '/aggregateprofiles',
      render: () =>
        m(FlamegraphCollection, {
          trace,
          rows: loaded.rows,
          columns,
          entryKey: (row: Row) => String(row['c0']),
          metricsForKeys,
          entityName: 'profiles',
          renderEntryTitle: (key: string) =>
            key.startsWith('http://') || key.startsWith('https://')
              ? m(Anchor, {href: key, target: '_blank'}, key)
              : key,
          state,
          onStateChange: (s: FlamegraphCollectionState) => {
            state = s;
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
      const metrics = await this.getProfileMetrics(trace, it.scope);
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

  private async getProfileMetrics(trace: Trace, scope: string) {
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
        this.aggregateProfileMetric(it.display_name, it.sample_type_unit, [
          it.id,
        ]),
      );
    }
    return metrics;
  }

  // Metric summing the given profiles. Callsites are interned globally, so one
  // query merges same-stack samples across them.
  private aggregateProfileMetric(
    name: string,
    unit: string,
    aggIds: ReadonlyArray<number>,
  ): TreeExplorerQueryMetric {
    return {
      name,
      unit: displayUnit(unit),
      nameColumnLabel: 'Symbol',
      dependencySql: 'include perfetto module callstacks.stack_profile',
      statement: `
        WITH profile_samples AS MATERIALIZED (
          SELECT callsite_id, sum(sample.value) AS sample_value
          FROM __intrinsic_aggregate_sample sample
          WHERE sample.aggregate_profile_id IN (${aggIds.join(',')})
          GROUP BY callsite_id
        )
        SELECT
          c.id,
          c.parent_id as parentId,
          c.name,
          c.mapping_name,
          c.source_file || ':' || c.line_number as source_location,
          cast_string!(c.inlined) AS inlined,
          CASE WHEN c.is_leaf_function_in_callsite_frame
            THEN coalesce(m.sample_value, 0)
            ELSE 0
          END AS value
        FROM _callstacks_for_stack_profile_samples!(profile_samples) AS c
        LEFT JOIN profile_samples AS m USING (callsite_id)
      `,
      unaggregatableProperties: [
        {name: 'mapping_name', displayName: 'Mapping'},
        {name: 'inlined', displayName: 'Inlined', isVisible: () => false},
      ],
      aggregatableProperties: [
        {
          name: 'source_location',
          displayName: 'Source Location',
          mergeAggregation: 'ONE_OR_SUMMARY',
        },
      ],
      optionalMarker: {
        name: 'Inlined Function',
        isVisible: (properties: ReadonlyMap<string, string>) =>
          properties.get('inlined') === '1',
      },
    };
  }
}

// Maps pprof units onto the flamegraph's vocabulary; displaySize scales
// 'ns' and 'B'.
function displayUnit(unit: string): string {
  switch (unit.toLowerCase()) {
    case 'nanoseconds':
      return 'ns';
    case 'bytes':
      return 'B';
    default:
      return unit;
  }
}

// Each profile's sample-type totals as grid rows. A grouped scan only: the
// callstack tables are touched for the selected subset alone.
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
      unit: displayUnit(st.unit),
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
