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

import * as m from 'mithril';

import {Actions} from '../common/actions';
import {isEmptyData} from '../common/aggregation_data';
import {LogExists, LogExistsKey} from '../common/logs';
import {QueryResponse} from '../common/queries';
import {addSelectionChangeObserver} from '../common/selection_observer';
import {Selection} from '../common/state';

import {AggregationPanel} from './aggregation_panel';
import {ChromeSliceDetailsPanel} from './chrome_slice_panel';
import {CounterDetailsPanel} from './counter_panel';
import {CpuProfileDetailsPanel} from './cpu_profile_panel';
import {DEFAULT_DETAILS_CONTENT_HEIGHT} from './css_constants';
import {DragGestureHandler} from './drag_gesture_handler';
import {FlamegraphDetailsPanel} from './flamegraph_panel';
import {
  FlowEventsAreaSelectedPanel,
  FlowEventsPanel,
} from './flow_events_panel';
import {globals} from './globals';
import {LogPanel} from './logs_panel';
import {NotesEditorTab} from './notes_panel';
import {AnyAttrsVnode, PanelContainer} from './panel_container';
import {PivotTableRedux} from './pivot_table_redux';
import {QueryTable} from './query_table';
import {SliceDetailsPanel} from './slice_details_panel';
import {ThreadStateTab} from './thread_state_tab';

const UP_ICON = 'keyboard_arrow_up';
const DOWN_ICON = 'keyboard_arrow_down';
const DRAG_HANDLE_HEIGHT_PX = 28;

function getDetailsHeight() {
  // This needs to be a function instead of a const to ensure the CSS constants
  // have been initialized by the time we perform this calculation;
  return DEFAULT_DETAILS_CONTENT_HEIGHT + DRAG_HANDLE_HEIGHT_PX;
}

function getFullScreenHeight() {
  const panelContainer =
      document.querySelector('.pan-and-zoom-content') as HTMLElement;
  if (panelContainer !== null) {
    return panelContainer.clientHeight;
  } else {
    return getDetailsHeight();
  }
}

function hasLogs(): boolean {
  const data = globals.trackDataStore.get(LogExistsKey) as LogExists;
  return data && data.exists;
}

interface Tab {
  key: string;
  name: string;
}

interface DragHandleAttrs {
  height: number;
  resize: (height: number) => void;
  tabs: Tab[];
  currentTabKey?: string;
}

class DragHandle implements m.ClassComponent<DragHandleAttrs> {
  private dragStartHeight = 0;
  private height = 0;
  private previousHeight = this.height;
  private resize: (height: number) => void = () => {};
  private isClosed = this.height <= DRAG_HANDLE_HEIGHT_PX;
  private isFullscreen = false;
  // We can't get real fullscreen height until the pan_and_zoom_handler exists.
  private fullscreenHeight = getDetailsHeight();

  oncreate({dom, attrs}: m.CVnodeDOM<DragHandleAttrs>) {
    this.resize = attrs.resize;
    this.height = attrs.height;
    this.isClosed = this.height <= DRAG_HANDLE_HEIGHT_PX;
    this.fullscreenHeight = getFullScreenHeight();
    const elem = dom as HTMLElement;
    new DragGestureHandler(
        elem,
        this.onDrag.bind(this),
        this.onDragStart.bind(this),
        this.onDragEnd.bind(this));
  }

  onupdate({attrs}: m.CVnodeDOM<DragHandleAttrs>) {
    this.resize = attrs.resize;
    this.height = attrs.height;
    this.isClosed = this.height <= DRAG_HANDLE_HEIGHT_PX;
  }

  onDrag(_x: number, y: number) {
    const newHeight =
        Math.floor(this.dragStartHeight + (DRAG_HANDLE_HEIGHT_PX / 2) - y);
    this.isClosed = newHeight <= DRAG_HANDLE_HEIGHT_PX;
    this.isFullscreen = newHeight >= this.fullscreenHeight;
    this.resize(newHeight);
    globals.rafScheduler.scheduleFullRedraw();
  }

  onDragStart(_x: number, _y: number) {
    this.dragStartHeight = this.height;
  }

  onDragEnd() {}

  view({attrs}: m.CVnode<DragHandleAttrs>) {
    const icon = this.isClosed ? UP_ICON : DOWN_ICON;
    const title = this.isClosed ? 'Show panel' : 'Hide panel';
    const renderTab = (tab: Tab) => {
      if (attrs.currentTabKey === tab.key) {
        return m('.tab[active]', tab.name);
      }
      return m(
          '.tab',
          {
            onclick: () => {
              globals.dispatch(Actions.setCurrentTab({tab: tab.key}));
            },
          },
          tab.name);
    };
    return m(
        '.handle',
        m('.tabs', attrs.tabs.map(renderTab)),
        m('.buttons',
          m('i.material-icons',
            {
              onclick: () => {
                this.isClosed = false;
                this.isFullscreen = true;
                this.resize(this.fullscreenHeight);
                globals.rafScheduler.scheduleFullRedraw();
              },
              title: 'Open fullscreen',
              disabled: this.isFullscreen,
            },
            'vertical_align_top'),
          m('i.material-icons',
            {
              onclick: () => {
                if (this.height === DRAG_HANDLE_HEIGHT_PX) {
                  this.isClosed = false;
                  if (this.previousHeight === 0) {
                    this.previousHeight = getDetailsHeight();
                  }
                  this.resize(this.previousHeight);
                } else {
                  this.isFullscreen = false;
                  this.isClosed = true;
                  this.previousHeight = this.height;
                  this.resize(DRAG_HANDLE_HEIGHT_PX);
                }
                globals.rafScheduler.scheduleFullRedraw();
              },
              title,
            },
            icon)));
  }
}

// For queries that are supposed to be displayed in the bottom bar, return a
// name for a tab. Otherwise, return null.
function userVisibleQueryName(id: string): string|null {
  if (id === 'command') {
    return 'Omnibox Query';
  }
  if (id === 'analyze-page-query') {
    return 'Standalone Query';
  }
  if (id.startsWith('command_')) {
    return 'Pinned Query';
  }
  if (id.startsWith('pivot_table_details_')) {
    return 'Pivot Table Details';
  }
  if (id.startsWith('slices_with_arg_value_')) {
    return `Arg: ${id.substr('slices_with_arg_value_'.length)}`;
  }
  if (id === 'chrome_scroll_jank_long_tasks') {
    return 'Scroll Jank: long tasks';
  }
  return null;
}

function handleSelectionChange(newSelection?: Selection, _?: Selection): void {
  const currentSelectionTag = 'current_selection';
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
      });
      break;
    default:
      bottomTabList.closeTabByTag(currentSelectionTag);
  }
}
addSelectionChangeObserver(handleSelectionChange);

export class DetailsPanel implements m.ClassComponent {
  private detailsHeight = getDetailsHeight();

  view() {
    interface DetailsPanel {
      key: string;
      name: string;
      vnode: AnyAttrsVnode;
    }

    const detailsPanels: DetailsPanel[] = [];

    if (globals.bottomTabList) {
      for (const tab of globals.bottomTabList.tabs) {
        detailsPanels.push({
          key: tab.uuid,
          name: tab.getTitle(),
          vnode: tab.createPanelVnode(),
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
        case 'CHROME_SLICE':
          detailsPanels.push({
            key: 'current_selection',
            name: 'Current Selection',
            vnode: m(ChromeSliceDetailsPanel, {key: 'chrome_slice'}),
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

    const queryResults = [];
    for (const queryId of globals.queryResults.keys()) {
      const readableName = userVisibleQueryName(queryId);
      if (readableName !== null) {
        queryResults.push({queryId, name: readableName});
      }
    }

    for (const {queryId, name} of queryResults) {
      const count =
          (globals.queryResults.get(queryId) as QueryResponse).rows.length;
      detailsPanels.push({
        key: `query_result_${queryId}`,
        name: `${name} (${count})`,
        vnode: m(QueryTable, {key: `query_${queryId}`, queryId}),
      });
    }


    if (globals.state.nonSerializableState.pivotTableRedux.selectionArea !==
        undefined) {
      detailsPanels.push({
        key: 'pivot_table_redux',
        name: 'Pivot Table',
        vnode: m(PivotTableRedux, {
          key: 'pivot_table_redux',
          selectionArea:
              globals.state.nonSerializableState.pivotTableRedux.selectionArea,
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
    const panels = panel ? [panel] : [];

    return m(
        '.details-content',
        {
          style: {
            height: `${this.detailsHeight}px`,
            display: detailsPanels.length > 0 ? null : 'none',
          },
        },
        m(DragHandle, {
          resize: (height: number) => {
            this.detailsHeight = Math.max(height, DRAG_HANDLE_HEIGHT_PX);
          },
          height: this.detailsHeight,
          tabs: detailsPanels.map((tab) => {
            return {key: tab.key, name: tab.name};
          }),
          currentTabKey: currentTabDetails?.key,
        }),
        m('.details-panel-container.x-scrollable',
          m(PanelContainer, {doesScroll: true, panels, kind: 'DETAILS'})));
  }
}
