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

import {
  QueryFlamegraphMetric,
  metricsFromTableOrSubquery,
} from '../../components/query_flamegraph';
import {PerfettoPlugin} from '../../public/plugin';
import {Trace} from '../../public/trace';
import {
  LONG,
  NUM,
  NUM_NULL,
  STR,
  STR_NULL,
} from '../../trace_processor/query_result';
import {AggregatesPage} from './aggregates_page';
import {AggregateScope, AggregatesPageState} from './types';
import {Dataset} from '../../trace_processor/dataset';

export default class implements PerfettoPlugin {
  static readonly id = 'dev.perfetto.AggregatesPage';

  private state: AggregatesPageState = {
    selectedScope: undefined,
    availableScopes: [],
  };
  private readonly onStateUpdate = (update: AggregatesPageState): void => {
    this.state = update;
    m.redraw();
  };

  async onTraceLoad(trace: Trace): Promise<void> {
    const registeredScopes: AggregateScope[] = [];
    trace.pages.registerPage({
      route: '/aggregates',
      render: () =>
        m(AggregatesPage, {
          trace,
          state: this.state,
          onStateUpdate: this.onStateUpdate,
          registeredScopes: registeredScopes,
        }),
    });
    trace.sidebar.addMenuItem({
      section: 'current_trace',
      sortOrder: 11,
      text: 'Aggregates',
      href: '#!/aggregates',
      icon: 'analytics',
    });
    trace.onTraceReady.addListener(async () => {
      const pprofScopes = await this.getPprofAggregateScopes(trace);
      const sliceScopes = this.getSliceTrackAggregateScopes(trace);
      registeredScopes.push(...pprofScopes, ...sliceScopes);

      // If there are no tracks in the trace, automatically navigate to
      // aggregates page.
      const hasAnyTracks = trace.workspaces.all[0].flatTracks.length > 0;
      if (!hasAnyTracks && registeredScopes.length > 0) {
        trace.navigate('#!/aggregates');
      }
    });
  }

  private async getPprofProfileCount(trace: Trace): Promise<number> {
    const result = await trace.engine.query(
      'SELECT COUNT(*) as count FROM __intrinsic_aggregate_profile LIMIT 1',
    );
    return result.maybeFirstRow({count: NUM})?.count ?? 0;
  }

  private async getPprofAggregateScopes(
    trace: Trace,
  ): Promise<AggregateScope[]> {
    const profileCount = await this.getPprofProfileCount(trace);
    if (profileCount === 0) {
      return [];
    }
    const result = await trace.engine.query(
      'SELECT DISTINCT scope FROM __intrinsic_aggregate_profile ORDER BY scope',
    );
    const scopes: AggregateScope[] = [];
    for (const it = result.iter({scope: STR}); it.valid(); it.next()) {
      const metrics = await this.getPprofMetrics(trace, it.scope);
      if (metrics.length > 0) {
        scopes.push({
          id: `pprof_${it.scope}`,
          displayName: `pprof: ${it.scope}`,
          metrics,
        });
      }
    }
    return scopes;
  }

  private async getPprofMetrics(
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

  private getSliceTrackAggregateScopes(trace: Trace): AggregateScope[] {
    const allTracks = trace.workspaces.all[0].flatTracks;
    const scopes: AggregateScope[] = [];

    for (const trackNode of allTracks) {
      if (!trackNode.uri) {
        continue;
      }

      const track = trace.tracks.getTrack(trackNode.uri);
      if (!track) {
        continue;
      }

      const dataset = track.renderer.getDataset?.();
      if (!dataset) {
        continue;
      }

      if (!this.isValidSliceDataset(dataset)) {
        continue;
      }
      const hasParentId = dataset.implements({
        parent_id: NUM_NULL,
      });
      const queryFields = {
        id: NUM,
        name: STR_NULL,
        dur: LONG,
        ...(hasParentId && {parent_id: NUM_NULL}),
      };
      const query = dataset.query(queryFields);
      const selectParentId = hasParentId
        ? 'parent_id as parentId,'
        : 'NULL as parentId,';
      const metrics = metricsFromTableOrSubquery(
        `(
          SELECT
            id,
            ${selectParentId}
            COALESCE(name, '[unnamed]') as name,
            dur
          FROM (${query})
          WHERE dur > 0
        )`,
        [{name: 'Duration', unit: 'ns', columnName: 'dur'}],
      );

      if (metrics.length > 0) {
        scopes.push({
          id: `slice_track_${trackNode.id}`,
          displayName: `Slices: ${trackNode.fullPath.join(' > ')}`,
          metrics,
        });
      }
    }

    return scopes;
  }

  private isValidSliceDataset(dataset: Dataset): boolean {
    // Check for minimum required fields (parent_id is optional)
    return dataset.implements({
      id: NUM,
      name: STR_NULL,
      dur: LONG,
    });
  }
}
