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
import {QueryResponse} from 'src/common/queries';

import {LogExists, LogExistsKey} from '../common/logs';

import {AggregationPanel} from './aggregation_panel';
import {ChromeSliceDetailsPanel} from './chrome_slice_panel';
import {CounterDetailsPanel} from './counter_panel';
import {CpuProfileDetailsPanel} from './cpu_profile_panel';
import {DragGestureHandler} from './drag_gesture_handler';
import {
  FlowEventsAreaSelectedPanel,
  FlowEventsPanel
} from './flow_events_panel';
import {globals} from './globals';
import {HeapProfileDetailsPanel} from './heap_profile_panel';
import {LogPanel} from './logs_panel';
import {NotesEditorPanel} from './notes_panel';
import {AnyAttrsVnode, PanelContainer} from './panel_container';
import {QueryTable} from './query_table';
import {SliceDetailsPanel} from './slice_panel';
import {ThreadStatePanel} from './thread_state_panel';

const UP_ICON = 'keyboard_arrow_up';
const DOWN_ICON = 'keyboard_arrow_down';
const DRAG_HANDLE_HEIGHT_PX = 28;
const DEFAULT_DETAILS_HEIGHT_PX = 230 + DRAG_HANDLE_HEIGHT_PX;

function getFullScreenHeight() {
  const panelContainer =
      document.querySelector('.pan-and-zoom-content') as HTMLElement;
  if (panelContainer !== null) {
    return panelContainer.clientHeight;
  } else {
    return DEFAULT_DETAILS_HEIGHT_PX;
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
}

class DragHandle implements m.ClassComponent<DragHandleAttrs> {
  private dragStartHeight = 0;
  private height = 0;
  private previousHeight = this.height;
  private resize: (height: number) => void = () => {};
  private isClosed = this.height <= DRAG_HANDLE_HEIGHT_PX;
  private isFullscreen = false;
  // We can't get real fullscreen height until the pan_and_zoom_handler exists.
  private fullscreenHeight = DEFAULT_DETAILS_HEIGHT_PX;

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
    const activeTabExists = globals.frontendLocalState.currentTab &&
        attrs.tabs.map(tab => tab.key)
            .includes(globals.frontendLocalState.currentTab);
    if (!activeTabExists) {
      globals.frontendLocalState.currentTab = undefined;
    }
    const renderTab = (tab: Tab) => {
      if (globals.frontendLocalState.currentTab === tab.key ||
          globals.frontendLocalState.currentTab === undefined &&
              attrs.tabs.keys().next().value === tab.key) {
        // Update currentTab in case we didn't have one before.
        globals.frontendLocalState.currentTab = tab.key;
        return m('.tab[active]', tab.name);
      }
      return m(
          '.tab',
          {
            onclick: () => {
              globals.frontendLocalState.currentTab = tab.key;
              globals.rafScheduler.scheduleFullRedraw();
            }
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
              disabled: this.isFullscreen
            },
            'vertical_align_top'),
          m('i.material-icons',
            {
              onclick: () => {
                if (this.height === DRAG_HANDLE_HEIGHT_PX) {
                  this.isClosed = false;
                  if (this.previousHeight === 0) {
                    this.previousHeight = DEFAULT_DETAILS_HEIGHT_PX;
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
              title
            },
            icon)));
  }
}

export class DetailsPanel implements m.ClassComponent {
  private detailsHeight = DEFAULT_DETAILS_HEIGHT_PX;
  // Used to set details panel to default height on selection.
  private showDetailsPanel = true;

  view() {
    interface DetailsPanel {
      key: string;
      name: string;
      vnode: AnyAttrsVnode;
    }

    const detailsPanels: DetailsPanel[] = [];
    const curSelection = globals.state.currentSelection;
    if (curSelection) {
      switch (curSelection.kind) {
        case 'NOTE':
          detailsPanels.push({
            key: 'current_selection',
            name: 'Current Selection',
            vnode: m(NotesEditorPanel, {
              key: 'notes',
              id: curSelection.id,
            })
          });
          break;
        case 'AREA':
          if (curSelection.noteId !== undefined) {
            detailsPanels.push({
              key: 'current_selection',
              name: 'Current Selection',
              vnode: m(NotesEditorPanel, {
                key: 'area_notes',
                id: curSelection.noteId,
              })
            });
          }
          break;
        case 'SLICE':
          detailsPanels.push({
            key: 'current_selection',
            name: 'Current Selection',
            vnode: m(SliceDetailsPanel, {
              key: 'slice',
            })
          });
          break;
        case 'COUNTER':
          detailsPanels.push({
            key: 'current_selection',
            name: 'Current Selection',
            vnode: m(CounterDetailsPanel, {
              key: 'counter',
            })
          });
          break;
        case 'HEAP_PROFILE':
          detailsPanels.push({
            key: 'current_selection',
            name: 'Current Selection',
            vnode: m(HeapProfileDetailsPanel, {key: 'heap_profile'})
          });
          break;
        case 'CPU_PROFILE_SAMPLE':
          detailsPanels.push({
            key: 'current_selection',
            name: 'Current Selection',
            vnode: m(CpuProfileDetailsPanel, {
              key: 'cpu_profile_sample',
            })
          });
          break;
        case 'CHROME_SLICE':
          detailsPanels.push({
            key: 'current_selection',
            name: 'Current Selection',
            vnode: m(ChromeSliceDetailsPanel, {key: 'chrome_slice'})
          });
          break;
        case 'THREAD_STATE':
          detailsPanels.push({
            key: 'current_selection',
            name: 'Current Selection',
            vnode: m(ThreadStatePanel, {key: 'thread_state'})
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
        vnode: m(LogPanel, {key: 'logs_panel'})
      });
    }

    if (globals.queryResults.has('command')) {
      const count =
          (globals.queryResults.get('command') as QueryResponse).rows.length;
      detailsPanels.push({
        key: 'query_result',
        name: `Query Result (${count})`,
        vnode: m(QueryTable, {key: 'query', queryId: 'command'})
      });
    }

    if (globals.connectedFlows.length > 0) {
      detailsPanels.push({
        key: 'bound_flows',
        name: 'Flow Events',
        vnode: m(FlowEventsPanel, {key: 'flow_events'})
      });
    }

    for (const [key, value] of globals.aggregateDataStore.entries()) {
      if (value.columns.length > 0 && value.columns[0].data.length > 0) {
        detailsPanels.push({
          key: value.tabName,
          name: value.tabName,
          vnode: m(AggregationPanel, {kind: key, key, data: value})
        });
      }
    }

    // Add this after all aggregation panels, to make it appear after 'Slices'
    if (globals.selectedFlows.length > 0) {
      detailsPanels.push({
        key: 'selected_flows',
        name: 'Flow Events',
        vnode: m(FlowEventsAreaSelectedPanel)
      });
    }

    this.showDetailsPanel = detailsPanels.length > 0;

    const currentTabDetails = detailsPanels.filter(
        tab => tab.key === globals.frontendLocalState.currentTab)[0];

    const panel = currentTabDetails ?
        currentTabDetails.vnode :
        detailsPanels.values().next().value?.vnode;
    const panels = panel ? [panel] : [];

    return m(
        '.details-content',
        {
          style: {
            height: `${this.detailsHeight}px`,
            display: this.showDetailsPanel ? null : 'none'
          }
        },
        m(DragHandle, {
          resize: (height: number) => {
            this.detailsHeight = Math.max(height, DRAG_HANDLE_HEIGHT_PX);
          },
          height: this.detailsHeight,
          tabs: detailsPanels.map(tab => {
            return {key: tab.key, name: tab.name};
          }),
        }),
        m('.details-panel-container',
          m(PanelContainer, {doesScroll: true, panels, kind: 'DETAILS'})));
  }
}
