// Copyright (C) 2019 The Android Open Source Project
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

import {Gate} from '../base/mithril_utils';
import {exists} from '../base/utils';
import {Actions} from '../common/actions';
import {isEmptyData} from '../common/aggregation_data';
import {LogExists, LogExistsKey} from '../common/logs';
import {addSelectionChangeObserver} from '../common/selection_observer';
import {Selection} from '../common/state';

import {AggregationPanel} from './aggregation_panel';
import {ChromeSliceDetailsTab} from './chrome_slice_details_tab';
import {CounterDetailsPanel} from './counter_panel';
import {CpuProfileDetailsPanel} from './cpu_profile_panel';
import {DragHandle, getDefaultDetailsHeight} from './drag_handle';
import {FlamegraphDetailsPanel} from './flamegraph_panel';
import {
  FlowEventsAreaSelectedPanel,
  FlowEventsPanel,
} from './flow_events_panel';
import {FtracePanel} from './ftrace_panel';
import {globals} from './globals';
import {LogPanel} from './logs_panel';
import {NotesEditorTab} from './notes_panel';
import {PivotTable} from './pivot_table';
import {SliceDetailsPanel} from './slice_details_panel';
import {ThreadStateTab} from './thread_state_tab';

export const CURRENT_SELECTION_TAG = 'current_selection';

function hasLogs(): boolean {
  const data =
      globals.trackDataStore.get(LogExistsKey) as LogExists | undefined;
  return Boolean(data?.exists);
}


function handleSelectionChange(
  newSelection: Selection|undefined, openCurrentSelectionTab: boolean): void {
  const currentSelectionTag = CURRENT_SELECTION_TAG;
  const bottomTabList = globals.bottomTabList;
  if (!bottomTabList) return;
  if (newSelection === undefined) {
    bottomTabList.closeTabByTag(currentSelectionTag);
    return;
  }
  switch (newSelection.kind) {
  case 'NOTE':
    bottomTabList.addTab({
      kind: NotesEditorTab.kind,
      tag: currentSelectionTag,
      config: {
        id: newSelection.id,
      },
      select: openCurrentSelectionTab,
    });
    break;
  case 'AREA':
    if (newSelection.noteId !== undefined) {
      bottomTabList.addTab({
        kind: NotesEditorTab.kind,
        tag: currentSelectionTag,
        config: {
          id: newSelection.noteId,
        },
        select: openCurrentSelectionTab,
      });
    }
    break;
  case 'THREAD_STATE':
    bottomTabList.addTab({
      kind: ThreadStateTab.kind,
      tag: currentSelectionTag,
      config: {
        id: newSelection.id,
      },
      select: openCurrentSelectionTab,
    });
    break;
  case 'GENERIC_SLICE':
    bottomTabList.addTab({
      kind: newSelection.detailsPanelConfig.kind,
      tag: currentSelectionTag,
      config: newSelection.detailsPanelConfig.config,
      select: openCurrentSelectionTab,
    });
    break;
  case 'CHROME_SLICE':
    bottomTabList.addTab({
      kind: ChromeSliceDetailsTab.kind,
      tag: currentSelectionTag,
      config: {
        id: newSelection.id,
        table: newSelection.table,
      },
      select: openCurrentSelectionTab,
    });
    break;
  default:
    bottomTabList.closeTabByTag(currentSelectionTag);
  }
}
addSelectionChangeObserver(handleSelectionChange);

export class DetailsPanel implements m.ClassComponent {
  private detailsHeight = getDefaultDetailsHeight();

  view() {
    interface DetailsPanel {
      key: string;
      name: string;
      vnode: m.Children;
    }

    const detailsPanels: DetailsPanel[] = [];

    if (globals.bottomTabList) {
      for (const tab of globals.bottomTabList.getTabs()) {
        detailsPanels.push({
          key: tab.tag ?? tab.uuid,
          name: tab.getTitle(),
          vnode: tab.renderPanel(),
        });
      }
    }

    const curSelection = globals.state.currentSelection;
    if (curSelection) {
      switch (curSelection.kind) {
      case 'NOTE':
        // Handled in handleSelectionChange.
        break;
      case 'AREA':
        if (globals.flamegraphDetails.isInAreaSelection) {
          detailsPanels.push({
            key: 'flamegraph_selection',
            name: 'Flamegraph Selection',
            vnode: m(FlamegraphDetailsPanel, {key: 'flamegraph'}),
          });
        }
        break;
      case 'SLICE':
        detailsPanels.push({
          key: 'current_selection',
          name: 'Current Selection',
          vnode: m(SliceDetailsPanel, {
            key: 'slice',
          }),
        });
        break;
      case 'COUNTER':
        detailsPanels.push({
          key: 'current_selection',
          name: 'Current Selection',
          vnode: m(CounterDetailsPanel, {
            key: 'counter',
          }),
        });
        break;
      case 'PERF_SAMPLES':
      case 'HEAP_PROFILE':
        detailsPanels.push({
          key: 'current_selection',
          name: 'Current Selection',
          vnode: m(FlamegraphDetailsPanel, {key: 'flamegraph'}),
        });
        break;
      case 'CPU_PROFILE_SAMPLE':
        detailsPanels.push({
          key: 'current_selection',
          name: 'Current Selection',
          vnode: m(CpuProfileDetailsPanel, {
            key: 'cpu_profile_sample',
          }),
        });
        break;
      default:
        break;
      }
    }
    if (hasLogs()) {
      detailsPanels.push({
        key: 'android_logs',
        name: 'Android Logs',
        vnode: m(LogPanel, {key: 'logs_panel'}),
      });
    }

    const trackGroup = globals.state.trackGroups['ftrace-track-group'];
    // eslint-disable-next-line @typescript-eslint/strict-boolean-expressions
    if (trackGroup) {
      const {collapsed} = trackGroup;
      if (!collapsed) {
        detailsPanels.push({
          key: 'ftrace_events',
          name: 'Ftrace Events',
          vnode: m(FtracePanel, {key: 'ftrace_panel'}),
        });
      }
    }

    if (globals.state.nonSerializableState.pivotTable.selectionArea !==
        undefined) {
      detailsPanels.push({
        key: 'pivot_table',
        name: 'Pivot Table',
        vnode: m(PivotTable, {
          key: 'pivot_table',
          selectionArea:
              globals.state.nonSerializableState.pivotTable.selectionArea,
        }),
      });
    }

    if (globals.connectedFlows.length > 0) {
      detailsPanels.push({
        key: 'bound_flows',
        name: 'Flow Events',
        vnode: m(FlowEventsPanel, {key: 'flow_events'}),
      });
    }

    for (const [key, value] of globals.aggregateDataStore.entries()) {
      if (!isEmptyData(value)) {
        detailsPanels.push({
          key: value.tabName,
          name: value.tabName,
          vnode: m(AggregationPanel, {kind: key, key, data: value}),
        });
      }
    }

    // Add this after all aggregation panels, to make it appear after 'Slices'
    if (globals.selectedFlows.length > 0) {
      detailsPanels.push({
        key: 'selected_flows',
        name: 'Flow Events',
        vnode: m(FlowEventsAreaSelectedPanel, {key: 'flow_events_area'}),
      });
    }

    let currentTabDetails =
        detailsPanels.find((tab) => tab.key === globals.state.currentTab);
    if (currentTabDetails === undefined && detailsPanels.length > 0) {
      currentTabDetails = detailsPanels[0];
    }

    const panel = currentTabDetails?.vnode;

    if (!exists(panel)) {
      return null;
    }

    return [
      m(DragHandle, {
        resize: (height: number) => {
          this.detailsHeight = Math.max(height, 0);
        },
        height: this.detailsHeight,
        tabs: detailsPanels.map((tab) => {
          return {key: tab.key, title: tab.name};
        }),
        currentTabKey: currentTabDetails?.key,
        onTabClick: (key) => {
          globals.dispatch(Actions.setCurrentTab({tab: key}));
        },
      }),
      m(
        '.details-panel-container',
        {
          style: {height: `${this.detailsHeight}px`},
        },
        detailsPanels.map((tab) => {
          const active = tab === currentTabDetails;
          return m(Gate, {open: active}, tab.vnode);
        }),
      ),
    ];
  }
}
