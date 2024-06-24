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
import {Disposable, DisposableStack} from '../base/disposable';
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
import {ProfileType, TrackState} from '../common/state';
import {assertExists} from '../base/logging';
import {Monitor} from '../base/monitor';
import {PERF_SAMPLES_PROFILE_TRACK_KIND} from '../core/track_kinds';

interface View {
  key: string;
  name: string;
  content: m.Children;
}

class AreaDetailsPanel implements m.ClassComponent {
  private readonly monitor = new Monitor([() => globals.state.selection]);
  private currentTab: string | undefined = undefined;
  private flamegraphSelection?: FlamegraphSelectionParams;

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
    const views = [];

    this.flamegraphSelection = this.computeFlamegraphSelection();
    if (this.flamegraphSelection !== undefined) {
      views.push({
        key: 'flamegraph_selection',
        name: 'Flamegraph Selection',
        content: m(LegacyFlamegraphDetailsPanel, {
          cache: globals.areaFlamegraphCache,
          selection: this.flamegraphSelection,
        }),
      });
    }

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

  private computeFlamegraphSelection() {
    const currentSelection = globals.state.selection;
    if (currentSelection.kind !== 'area') {
      return undefined;
    }
    if (!this.monitor.ifStateChanged()) {
      // If the selection has not changed, just return a copy of the last seen
      // selection.
      return this.flamegraphSelection;
    }
    const upids = [];
    for (const trackId of currentSelection.tracks) {
      const track: TrackState | undefined = globals.state.tracks[trackId];
      const trackInfo = globals.trackManager.resolveTrackInfo(track?.uri);
      if (trackInfo?.kind !== PERF_SAMPLES_PROFILE_TRACK_KIND) {
        continue;
      }
      upids.push(assertExists(trackInfo.upid));
    }
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

  dispose(): void {
    this.trash.dispose();
  }
}
