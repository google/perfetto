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

import {Trash} from '../base/disposable';
import {Actions} from '../common/actions';
import {isEmptyData} from '../common/aggregation_data';
import {LogExists, LogExistsKey} from '../common/logs';
import {addSelectionChangeObserver} from '../common/selection_observer';
import {Selection} from '../common/state';
import {raf} from '../core/raf_scheduler';

import {AggregationPanel} from './aggregation_panel';
import {ChromeSliceDetailsTab} from './chrome_slice_details_tab';
import {CounterDetailsPanel} from './counter_panel';
import {CpuProfileDetailsPanel} from './cpu_profile_panel';
import {DEFAULT_DETAILS_CONTENT_HEIGHT} from './css_constants';
import {DragGestureHandler} from './drag_gesture_handler';
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

const UP_ICON = 'keyboard_arrow_up';
const DOWN_ICON = 'keyboard_arrow_down';
const DRAG_HANDLE_HEIGHT_PX = 28;

export const CURRENT_SELECTION_TAG = 'current_selection';

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
  private isClosed = this.height <= 0;
  private isFullscreen = false;
  // We can't get real fullscreen height until the pan_and_zoom_handler exists.
  private fullscreenHeight = getDetailsHeight();
  private trash: Trash = new Trash();

  oncreate({dom, attrs}: m.CVnodeDOM<DragHandleAttrs>) {
    this.resize = attrs.resize;
    this.height = attrs.height;
    this.isClosed = this.height <= 0;
    this.fullscreenHeight = getFullScreenHeight();
    const elem = dom as HTMLElement;
    this.trash.add(new DragGestureHandler(
        elem,
        this.onDrag.bind(this),
        this.onDragStart.bind(this),
        this.onDragEnd.bind(this)));
  }

  onupdate({attrs}: m.CVnodeDOM<DragHandleAttrs>) {
    this.resize = attrs.resize;
    this.height = attrs.height;
    this.isClosed = this.height <= 0;
  }

  onremove(_: m.CVnodeDOM<DragHandleAttrs>) {
    this.trash.dispose();
  }

  onDrag(_x: number, y: number) {
    const newHeight =
        Math.floor(this.dragStartHeight + (DRAG_HANDLE_HEIGHT_PX / 2) - y);
    this.isClosed = newHeight <= 0;
    this.isFullscreen = newHeight >= this.fullscreenHeight;
    this.resize(newHeight);
    raf.scheduleFullRedraw();
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
                raf.scheduleFullRedraw();
              },
              title: 'Open fullscreen',
              disabled: this.isFullscreen,
            },
            'vertical_align_top'),
          m('i.material-icons',
            {
              onclick: () => {
                if (this.height === 0) {
                  this.isClosed = false;
                  if (this.previousHeight === 0) {
                    this.previousHeight = getDetailsHeight();
                  }
                  this.resize(this.previousHeight);
                } else {
                  this.isFullscreen = false;
                  this.isClosed = true;
                  this.previousHeight = this.height;
                  this.resize(0);
                }
                raf.scheduleFullRedraw();
              },
              title,
            },
            icon)));
  }
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
  private detailsHeight = getDetailsHeight();

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

    if (!panel) {
      return null;
    }

    return [
      m(DragHandle, {
        resize: (height: number) => {
          this.detailsHeight = Math.max(height, 0);
        },
        height: this.detailsHeight,
        tabs: detailsPanels.map((tab) => {
          return {key: tab.key, name: tab.name};
        }),
        currentTabKey: currentTabDetails?.key,
      }),
      m('.details-panel-container',
        {
          style: {height: `${this.detailsHeight}px`},
        },
        panel),
    ];
  }
}
