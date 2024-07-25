// Copyright (C) 2024 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use size file except in compliance with the License.
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

import {AggregationPanel} from './aggregation_panel';
import {globals} from './globals';
import {isEmptyData} from '../common/aggregation_data';
import {DetailsShell} from '../widgets/details_shell';
import {Button, ButtonBar} from '../widgets/button';
import {raf} from '../core/raf_scheduler';
import {EmptyState} from '../widgets/empty_state';
import {FlowEventsAreaSelectedPanel} from './flow_events_panel';
import {PivotTable} from './pivot_table';
import {
  LegacyFlamegraphDetailsPanel,
  FlamegraphSelectionParams,
} from './legacy_flamegraph_panel';
import {AreaSelection, ProfileType, TrackState} from '../common/state';
import {assertExists} from '../base/logging';
import {Monitor} from '../base/monitor';
import {
  PERF_SAMPLES_PROFILE_TRACK_KIND,
  THREAD_SLICE_TRACK_KIND,
} from '../core/track_kinds';
import {
  QueryFlamegraph,
  QueryFlamegraphAttrs,
  USE_NEW_FLAMEGRAPH_IMPL,
  metricsFromTableOrSubquery,
} from '../core/query_flamegraph';
import {DisposableStack} from '../base/disposable_stack';

interface View {
  key: string;
  name: string;
  content: m.Children;
}

class AreaDetailsPanel implements m.ClassComponent {
  private readonly monitor = new Monitor([() => globals.state.selection]);
  private currentTab: string | undefined = undefined;
  private perfSampleFlamegraphAttrs?: QueryFlamegraphAttrs;
  private sliceFlamegraphAttrs?: QueryFlamegraphAttrs;
  private legacyFlamegraphSelection?: FlamegraphSelectionParams;

  private getCurrentView(): string | undefined {
    const types = this.getViews().map(({key}) => key);

    if (types.length === 0) {
      return undefined;
    }

    if (this.currentTab === undefined) {
      return types[0];
    }

    if (!types.includes(this.currentTab)) {
      return types[0];
    }

    return this.currentTab;
  }

  private getViews(): View[] {
    const views: View[] = [];

    for (const [key, value] of globals.aggregateDataStore.entries()) {
      if (!isEmptyData(value)) {
        views.push({
          key: value.tabName,
          name: value.tabName,
          content: m(AggregationPanel, {kind: key, key, data: value}),
        });
      }
    }

    const pivotTableState = globals.state.nonSerializableState.pivotTable;
    if (pivotTableState.selectionArea !== undefined) {
      views.push({
        key: 'pivot_table',
        name: 'Pivot Table',
        content: m(PivotTable, {
          selectionArea: pivotTableState.selectionArea,
        }),
      });
    }

    const isChanged = this.monitor.ifStateChanged();
    if (USE_NEW_FLAMEGRAPH_IMPL.get()) {
      this.addFlamegraphView(isChanged, views);
    } else {
      this.addLegacyFlamegraphView(isChanged, views);
    }

    // Add this after all aggregation panels, to make it appear after 'Slices'
    if (globals.selectedFlows.length > 0) {
      views.push({
        key: 'selected_flows',
        name: 'Flow Events',
        content: m(FlowEventsAreaSelectedPanel),
      });
    }

    return views;
  }

  view(_: m.Vnode): m.Children {
    const views = this.getViews();
    const currentViewKey = this.getCurrentView();

    const aggregationButtons = views.map(({key, name}) => {
      return m(Button, {
        onclick: () => {
          this.currentTab = key;
          raf.scheduleFullRedraw();
        },
        key,
        label: name,
        active: currentViewKey === key,
      });
    });

    if (currentViewKey === undefined) {
      return this.renderEmptyState();
    }

    const content = views.find(({key}) => key === currentViewKey)?.content;
    if (content === undefined) {
      return this.renderEmptyState();
    }

    return m(
      DetailsShell,
      {
        title: 'Area Selection',
        description: m(ButtonBar, aggregationButtons),
      },
      content,
    );
  }

  private renderEmptyState(): m.Children {
    return m(
      EmptyState,
      {
        className: 'pf-noselection',
        title: 'Unsupported area selection',
      },
      'No details available for this area selection',
    );
  }

  private addFlamegraphView(isChanged: boolean, views: View[]) {
    this.perfSampleFlamegraphAttrs =
      this.computePerfSampleFlamegraphAttrs(isChanged);
    if (this.perfSampleFlamegraphAttrs !== undefined) {
      views.push({
        key: 'perf_sample_flamegraph_selection',
        name: 'Perf Sample Flamegraph',
        content: m(QueryFlamegraph, this.perfSampleFlamegraphAttrs),
      });
    }
    this.sliceFlamegraphAttrs = this.computeSliceFlamegraphAttrs(isChanged);
    if (this.sliceFlamegraphAttrs !== undefined) {
      views.push({
        key: 'slice_flamegraph_selection',
        name: 'Slice Flamegraph',
        content: m(QueryFlamegraph, this.sliceFlamegraphAttrs),
      });
    }
  }

  private computePerfSampleFlamegraphAttrs(isChanged: boolean) {
    const currentSelection = globals.state.selection;
    if (currentSelection.kind !== 'area') {
      return undefined;
    }
    if (!isChanged) {
      // If the selection has not changed, just return a copy of the last seen
      // attrs.
      return this.perfSampleFlamegraphAttrs;
    }
    const upids = getUpidsFromPerfSampleAreaSelection(currentSelection);
    if (upids.length === 0) {
      const utids = getUtidsFromPerfSampleAreaSelection(currentSelection);
      if (utids.length === 0) {
        return undefined;
      }
      return {
        engine: assertExists(this.getCurrentEngine()),
        metrics: [
          ...metricsFromTableOrSubquery(
            `
              (
                select id, parent_id as parentId, name, self_count
                from _linux_perf_callstacks_for_samples!((
                  select p.callsite_id
                  from perf_sample p
                  where p.ts >= ${currentSelection.start}
                    and p.ts <= ${currentSelection.end}
                    and p.utid in (${utids.join(',')})
                ))
              )
            `,
            [
              {
                name: 'Perf Samples',
                unit: '',
                columnName: 'self_count',
              },
            ],
            'include perfetto module linux.perf.samples',
          ),
        ],
      };
    }
    return {
      engine: assertExists(this.getCurrentEngine()),
      metrics: [
        ...metricsFromTableOrSubquery(
          `
            (
              select id, parent_id as parentId, name, self_count
              from _linux_perf_callstacks_for_samples!((
                select p.callsite_id
                from perf_sample p
                join thread t using (utid)
                where p.ts >= ${currentSelection.start}
                  and p.ts <= ${currentSelection.end}
                  and t.upid in (${upids.join(',')})
              ))
            )
          `,
          [
            {
              name: 'Perf Samples',
              unit: '',
              columnName: 'self_count',
            },
          ],
          'include perfetto module linux.perf.samples',
        ),
      ],
    };
  }

  private computeSliceFlamegraphAttrs(isChanged: boolean) {
    const currentSelection = globals.state.selection;
    if (currentSelection.kind !== 'area') {
      return undefined;
    }
    if (!isChanged) {
      // If the selection has not changed, just return a copy of the last seen
      // attrs.
      return this.sliceFlamegraphAttrs;
    }
    const trackIds = [];
    for (const trackId of currentSelection.tracks) {
      const track: TrackState | undefined = globals.state.tracks[trackId];
      const trackInfo = globals.trackManager.resolveTrackInfo(track?.uri);
      if (trackInfo?.tags?.kind !== THREAD_SLICE_TRACK_KIND) {
        continue;
      }
      if (trackInfo.tags?.trackIds === undefined) {
        continue;
      }
      trackIds.push(...trackInfo.tags.trackIds);
    }
    if (trackIds.length === 0) {
      return undefined;
    }
    return {
      engine: assertExists(this.getCurrentEngine()),
      metrics: [
        ...metricsFromTableOrSubquery(
          `(
            select *
            from _viz_slice_ancestor_agg!((
              select s.id, s.dur
              from slice s
              left join slice t on t.parent_id = s.id
              where s.ts >= ${currentSelection.start}
                and s.ts <= ${currentSelection.end}
                and s.track_id in (${trackIds.join(',')})
                and t.id is null
            ))
          )`,
          [
            {
              name: 'Duration',
              unit: 'ns',
              columnName: 'self_dur',
            },
            {
              name: 'Samples',
              unit: '',
              columnName: 'self_count',
            },
          ],
          'include perfetto module viz.slices;',
        ),
      ],
    };
  }

  private addLegacyFlamegraphView(isChanged: boolean, views: View[]) {
    this.legacyFlamegraphSelection =
      this.computeLegacyFlamegraphSelection(isChanged);
    if (this.legacyFlamegraphSelection === undefined) {
      return;
    }
    views.push({
      key: 'flamegraph_selection',
      name: 'Flamegraph Selection',
      content: m(LegacyFlamegraphDetailsPanel, {
        cache: globals.areaFlamegraphCache,
        selection: this.legacyFlamegraphSelection,
      }),
    });
  }

  private computeLegacyFlamegraphSelection(isChanged: boolean) {
    const currentSelection = globals.state.selection;
    if (currentSelection.kind !== 'area') {
      return undefined;
    }
    if (!isChanged) {
      // If the selection has not changed, just return a copy of the last seen
      // selection.
      return this.legacyFlamegraphSelection;
    }
    const upids = getUpidsFromPerfSampleAreaSelection(currentSelection);
    if (upids.length === 0) {
      return undefined;
    }
    return {
      profileType: ProfileType.PERF_SAMPLE,
      start: currentSelection.start,
      end: currentSelection.end,
      upids,
    };
  }

  private getCurrentEngine() {
    const engineId = globals.getCurrentEngine()?.id;
    if (engineId === undefined) return undefined;
    return globals.engines.get(engineId);
  }
}

export class AggregationsTabs implements Disposable {
  private trash = new DisposableStack();

  constructor() {
    const unregister = globals.tabManager.registerDetailsPanel({
      render(selection) {
        if (selection.kind === 'area') {
          return m(AreaDetailsPanel);
        } else {
          return undefined;
        }
      },
    });

    this.trash.use(unregister);
  }

  [Symbol.dispose]() {
    this.trash.dispose();
  }
}

function getUpidsFromPerfSampleAreaSelection(currentSelection: AreaSelection) {
  const upids = [];
  for (const trackId of currentSelection.tracks) {
    const track: TrackState | undefined = globals.state.tracks[trackId];
    const trackInfo = globals.trackManager.resolveTrackInfo(track?.uri);
    if (trackInfo?.tags?.kind !== PERF_SAMPLES_PROFILE_TRACK_KIND) {
      continue;
    }
    if (trackInfo.tags?.upid === undefined) {
      continue;
    }
    upids.push(trackInfo.tags?.upid);
  }
  return upids;
}

function getUtidsFromPerfSampleAreaSelection(currentSelection: AreaSelection) {
  const utids = [];
  for (const trackId of currentSelection.tracks) {
    const track: TrackState | undefined = globals.state.tracks[trackId];
    const trackInfo = globals.trackManager.resolveTrackInfo(track?.uri);
    if (trackInfo?.tags?.kind !== PERF_SAMPLES_PROFILE_TRACK_KIND) {
      continue;
    }
    if (trackInfo.tags?.utid === undefined) {
      continue;
    }
    utids.push(trackInfo.tags?.utid);
  }
  return utids;
}
