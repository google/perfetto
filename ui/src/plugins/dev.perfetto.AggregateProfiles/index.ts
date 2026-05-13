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

import {QueryFlamegraphMetric} from '../../components/query_flamegraph';
import {PerfettoPlugin} from '../../public/plugin';
import {Trace} from '../../public/trace';
import {NUM, STR} from '../../trace_processor/query_result';
import {AggregateProfilesPage} from './aggregate_profiles_page';
import {
  AggregateProfilesPageState,
  AGGREGATE_PROFILES_PAGE_STATE_SCHEMA,
} from './types';
import {Store} from '../../base/store';
import {assertExists} from '../../base/assert';

export default class implements PerfettoPlugin {
  static readonly id = 'dev.perfetto.AggregateProfiles';
  private store?: Store<AggregateProfilesPageState>;

  private migratePageState(init: unknown): AggregateProfilesPageState {
    const result = AGGREGATE_PROFILES_PAGE_STATE_SCHEMA.safeParse(init);
    return result.data ?? {};
  }

  async onTraceLoad(trace: Trace): Promise<void> {
    this.store = trace.mountStore('dev.perfetto.AggregateProfiles', (init) =>
      this.migratePageState(init),
    );
    const profiles = await this.getProfiles(trace);
    if (profiles.length === 0) {
      return;
    }
    const store = assertExists(this.store);
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
      if (!hasAnyTracks && profiles.length > 0) {
        trace.navigate('#!/aggregateprofiles');
      }
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

  private async getProfileMetrics(
    trace: Trace,
    scope: string,
  ): Promise<QueryFlamegraphMetric[]> {
    const result = await trace.engine.query(`
      SELECT
        id,
        sample_type_type,
        sample_type_unit,
        sample_type_type || ' (' || sample_type_unit || ')' as display_name
      FROM __intrinsic_aggregate_profile
      WHERE scope = '${scope}'
      ORDER BY sample_type_type
    `);
    const metrics: QueryFlamegraphMetric[] = [];
    for (
      const it = result.iter({
        id: NUM,
        sample_type_unit: STR,
        display_name: STR,
      });
      it.valid();
      it.next()
    ) {
      metrics.push({
        name: it.display_name,
        unit: it.sample_type_unit,
        nameColumnLabel: 'Symbol',
        dependencySql: 'include perfetto module callstacks.stack_profile',
        statement: `
          WITH profile_samples AS MATERIALIZED (
            SELECT callsite_id, sum(sample.value) AS sample_value
            FROM __intrinsic_aggregate_sample sample
            WHERE sample.aggregate_profile_id = ${it.id}
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
          {
            name: 'inlined',
            displayName: 'Inlined',
            isVisible: () => false,
          },
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
      });
    }
    return metrics;
  }
}
