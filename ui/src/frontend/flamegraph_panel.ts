// Copyright (C) 2019 The Android Open Source Project
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

import * as m from 'mithril';

import {assertExists, assertTrue} from '../base/logging';
import {Actions} from '../common/actions';
import {
  ALLOC_SPACE_MEMORY_ALLOCATED_KEY,
  OBJECTS_ALLOCATED_KEY,
  OBJECTS_ALLOCATED_NOT_FREED_KEY,
  PERF_SAMPLES_KEY,
  SPACE_MEMORY_ALLOCATED_NOT_FREED_KEY,
} from '../common/flamegraph_util';
import {
  CallsiteInfo,
  FlamegraphStateViewingOption,
  ProfileType,
} from '../common/state';
import {timeToCode} from '../common/time';
import {profileType} from '../controller/flamegraph_controller';

import {PerfettoMouseEvent} from './events';
import {Flamegraph, NodeRendering} from './flamegraph';
import {globals} from './globals';
import {Modal, ModalDefinition} from './modal';
import {Panel, PanelSize} from './panel';
import {debounce} from './rate_limiters';
import {Router} from './router';
import {getCurrentTrace} from './sidebar';
import {convertTraceToPprofAndDownload} from './trace_converter';

interface FlamegraphDetailsPanelAttrs {}

const HEADER_HEIGHT = 30;

function toSelectedCallsite(c: CallsiteInfo|undefined): string {
  if (c !== undefined && c.name !== undefined) {
    return c.name;
  }
  return '(none)';
}

const RENDER_SELF_AND_TOTAL: NodeRendering = {
  selfSize: 'Self',
  totalSize: 'Total',
};
const RENDER_OBJ_COUNT: NodeRendering = {
  selfSize: 'Self objects',
  totalSize: 'Subtree objects',
};

export class FlamegraphDetailsPanel extends Panel<FlamegraphDetailsPanelAttrs> {
  private profileType?: ProfileType = undefined;
  private ts = 0;
  private pids: number[] = [];
  private flamegraph: Flamegraph = new Flamegraph([]);
  private focusRegex = '';
  private updateFocusRegexDebounced = debounce(() => {
    this.updateFocusRegex();
  }, 20);

  view() {
    const flamegraphDetails = globals.flamegraphDetails;
    if (flamegraphDetails && flamegraphDetails.type !== undefined &&
        flamegraphDetails.startNs !== undefined &&
        flamegraphDetails.durNs !== undefined &&
        flamegraphDetails.pids !== undefined &&
        flamegraphDetails.upids !== undefined) {
      this.profileType = profileType(flamegraphDetails.type);
      this.ts = flamegraphDetails.startNs + flamegraphDetails.durNs;
      this.pids = flamegraphDetails.pids;
      if (flamegraphDetails.flamegraph) {
        this.flamegraph.updateDataIfChanged(
            this.nodeRendering(), flamegraphDetails.flamegraph);
      }
      const height = flamegraphDetails.flamegraph ?
          this.flamegraph.getHeight() + HEADER_HEIGHT :
          0;
      return m(
          '.details-panel',
          {
            onclick: (e: PerfettoMouseEvent) => {
              if (this.flamegraph !== undefined) {
                this.onMouseClick({y: e.layerY, x: e.layerX});
              }
              return false;
            },
            onmousemove: (e: PerfettoMouseEvent) => {
              if (this.flamegraph !== undefined) {
                this.onMouseMove({y: e.layerY, x: e.layerX});
                globals.rafScheduler.scheduleRedraw();
              }
            },
            onmouseout: () => {
              if (this.flamegraph !== undefined) {
                this.onMouseOut();
              }
            },
          },
          this.maybeShowModal(flamegraphDetails.graphIncomplete),
          m('.details-panel-heading.flamegraph-profile',
            {onclick: (e: MouseEvent) => e.stopPropagation()},
            [
              m('div.options',
                [
                  m('div.title', this.getTitle()),
                  this.getViewingOptionButtons(),
                ]),
              m('div.details',
                [
                  m('div.selected',
                    `Selected function: ${
                        toSelectedCallsite(
                            flamegraphDetails.expandedCallsite)}`),
                  m('div.time',
                    `Snapshot time: ${timeToCode(flamegraphDetails.durNs)}`),
                  m('input[type=text][placeholder=Focus]', {
                    oninput: (e: Event) => {
                      const target = (e.target as HTMLInputElement);
                      this.focusRegex = target.value;
                      this.updateFocusRegexDebounced();
                    },
                    // Required to stop hot-key handling:
                    onkeydown: (e: Event) => e.stopPropagation(),
                  }),
                  this.profileType === ProfileType.NATIVE_HEAP_PROFILE ||
                          this.profileType === ProfileType.JAVA_HEAP_SAMPLES ?
                      m('button.download',
                        {
                          onclick: () => {
                            this.downloadPprof();
                          },
                        },
                        m('i.material-icons', 'file_download'),
                        'Download profile') :
                      null,
                ]),
            ]),
          m(`div[style=height:${height}px]`),
      );
    } else {
      return m(
          '.details-panel',
          m('.details-panel-heading', m('h2', `Flamegraph Profile`)));
    }
  }


  private maybeShowModal(graphIncomplete?: boolean) {
    if (!graphIncomplete || globals.state.flamegraphModalDismissed) {
      return undefined;
    }
    return m(Modal, {
      title: 'The flamegraph is incomplete',
      vAlign: 'TOP',
      content: m('div',
          'The current trace does not have a fully formed flamegraph'),
      buttons: [
        {
          text: 'Show the errors',
          primary: true,
          action: () => Router.navigate('#!/info'),
        },
        {
          text: 'Skip',
          action: () => {
            globals.dispatch(Actions.dismissFlamegraphModal({}));
            globals.rafScheduler.scheduleFullRedraw();
          },
        },
      ],
    } as ModalDefinition);
  }

  private getTitle(): string {
    switch (this.profileType!) {
      case ProfileType.HEAP_PROFILE:
        return 'Heap profile:';
      case ProfileType.NATIVE_HEAP_PROFILE:
        return 'Native heap profile:';
      case ProfileType.JAVA_HEAP_SAMPLES:
        return 'Java heap samples:';
      case ProfileType.JAVA_HEAP_GRAPH:
        return 'Java heap graph:';
      case ProfileType.PERF_SAMPLE:
        return 'Profile:';
      default:
        throw new Error('unknown type');
    }
  }

  private nodeRendering(): NodeRendering {
    if (this.profileType === undefined) {
      return {};
    }
    const viewingOption = globals.state.currentFlamegraphState!.viewingOption;
    switch (this.profileType) {
      case ProfileType.JAVA_HEAP_GRAPH:
        if (viewingOption === OBJECTS_ALLOCATED_NOT_FREED_KEY) {
          return RENDER_OBJ_COUNT;
        } else {
          return RENDER_SELF_AND_TOTAL;
        }
      case ProfileType.HEAP_PROFILE:
      case ProfileType.NATIVE_HEAP_PROFILE:
      case ProfileType.JAVA_HEAP_SAMPLES:
      case ProfileType.PERF_SAMPLE:
        return RENDER_SELF_AND_TOTAL;
      default:
        throw new Error('unknown type');
    }
  }

  private updateFocusRegex() {
    globals.dispatch(Actions.changeFocusFlamegraphState({
      focusRegex: this.focusRegex,
    }));
  }

  getViewingOptionButtons(): m.Children {
    return m(
        'div',
        ...FlamegraphDetailsPanel.selectViewingOptions(
            assertExists(this.profileType)));
  }

  downloadPprof() {
    const engine = globals.getCurrentEngine();
    if (!engine) return;
    getCurrentTrace()
        .then((file) => {
          assertTrue(
              this.pids.length === 1,
              'Native profiles can only contain one pid.');
          convertTraceToPprofAndDownload(file, this.pids[0], this.ts);
        })
        .catch((error) => {
          throw new Error(`Failed to get current trace ${error}`);
        });
  }

  private changeFlamegraphData() {
    const data = globals.flamegraphDetails;
    const flamegraphData = data.flamegraph === undefined ? [] : data.flamegraph;
    this.flamegraph.updateDataIfChanged(
        this.nodeRendering(), flamegraphData, data.expandedCallsite);
  }

  renderCanvas(ctx: CanvasRenderingContext2D, size: PanelSize) {
    this.changeFlamegraphData();
    const current = globals.state.currentFlamegraphState;
    if (current === null) return;
    const unit =
        current.viewingOption === SPACE_MEMORY_ALLOCATED_NOT_FREED_KEY ||
            current.viewingOption === ALLOC_SPACE_MEMORY_ALLOCATED_KEY ?
        'B' :
        '';
    this.flamegraph.draw(ctx, size.width, size.height, 0, HEADER_HEIGHT, unit);
  }

  onMouseClick({x, y}: {x: number, y: number}): boolean {
    const expandedCallsite = this.flamegraph.onMouseClick({x, y});
    globals.dispatch(Actions.expandFlamegraphState({expandedCallsite}));
    return true;
  }

  onMouseMove({x, y}: {x: number, y: number}): boolean {
    this.flamegraph.onMouseMove({x, y});
    return true;
  }

  onMouseOut() {
    this.flamegraph.onMouseOut();
  }

  private static selectViewingOptions(profileType: ProfileType) {
    switch (profileType) {
      case ProfileType.PERF_SAMPLE:
        return [this.buildButtonComponent(PERF_SAMPLES_KEY, 'Samples')];
      case ProfileType.JAVA_HEAP_GRAPH:
        return [
          this.buildButtonComponent(
              SPACE_MEMORY_ALLOCATED_NOT_FREED_KEY, 'Size'),
          this.buildButtonComponent(OBJECTS_ALLOCATED_NOT_FREED_KEY, 'Objects'),
        ];
      case ProfileType.HEAP_PROFILE:
        return [
          this.buildButtonComponent(
              SPACE_MEMORY_ALLOCATED_NOT_FREED_KEY, 'Unreleased size'),
          this.buildButtonComponent(
              OBJECTS_ALLOCATED_NOT_FREED_KEY, 'Unreleased count'),
          this.buildButtonComponent(
              ALLOC_SPACE_MEMORY_ALLOCATED_KEY, 'Total size'),
          this.buildButtonComponent(OBJECTS_ALLOCATED_KEY, 'Total count'),
        ];
      case ProfileType.NATIVE_HEAP_PROFILE:
        return [
          this.buildButtonComponent(
              SPACE_MEMORY_ALLOCATED_NOT_FREED_KEY, 'Unreleased malloc size'),
          this.buildButtonComponent(
              OBJECTS_ALLOCATED_NOT_FREED_KEY, 'Unreleased malloc count'),
          this.buildButtonComponent(
              ALLOC_SPACE_MEMORY_ALLOCATED_KEY, 'Total malloc size'),
          this.buildButtonComponent(
              OBJECTS_ALLOCATED_KEY, 'Total malloc count'),
        ];
      case ProfileType.JAVA_HEAP_SAMPLES:
        return [
          this.buildButtonComponent(
              ALLOC_SPACE_MEMORY_ALLOCATED_KEY, 'Total allocation size'),
          this.buildButtonComponent(
              OBJECTS_ALLOCATED_KEY, 'Total allocation count'),
        ];
      default:
        throw new Error(`Unexpected profile type ${profileType}`);
    }
  }

  private static buildButtonComponent(
      viewingOption: FlamegraphStateViewingOption, text: string) {
    const buttonsClass =
        (globals.state.currentFlamegraphState &&
         globals.state.currentFlamegraphState.viewingOption === viewingOption) ?
        '.chosen' :
        '';
    return m(
        `button${buttonsClass}`,
        {
          onclick: () => {
            globals.dispatch(
                Actions.changeViewFlamegraphState({viewingOption}));
          },
        },
        text);
  }
}
