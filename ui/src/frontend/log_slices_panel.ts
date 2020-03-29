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

import {SliceDetails} from './globals';
import {Panel} from './panel';
import {globals} from './globals';
import { ChromeSliceSelection } from 'src/common/state';
import {Actions} from '../common/actions';

export class LogSlicesPanel extends Panel<{slices: SliceDetails[]}> {
  view({attrs}: m.CVnode<{slices: SliceDetails[]}>) {
    return m(
        '.details-panel',
        m('.log-slice-panel-container', this.getRows(attrs.slices)));
  }

  getRows(slices: SliceDetails[]) {
    const selection = globals.state.currentSelection;
    let selectedSliceId: number = -1;
    if (selection != null && selection.kind == 'CHROME_SLICE') {
      let chromeSliceSelection = selection as ChromeSliceSelection;
      selectedSliceId = chromeSliceSelection.id;
    }


    return slices.map(slice => {
      const formattedTime =
          (slice.ts ? slice.ts : 0).toFixed(6).padStart(12, '0');

      const children = [m('.log-slice-panel-time', formattedTime)];
      const indent = slice.depth ? slice.depth : 0;
      for (let i = 0; i < indent; i++) {
        children.push(m('.log-slice-panel-indent-box'));
      }
      children.push(m('.log-slice-panel-name', slice.name));

      const isSelected = selectedSliceId === slice.id;

      // TODO add hardwired warnings and error colors here
      const hue = ((slice.trackId ? +slice.trackId : 0) * 100) % 360;
      const lum = isSelected ? 50 : 90;
      const textColor = isSelected ? 'color: white' : '';

      return m(
          '.log-slice-panel-row',
          {
            class: isSelected ? 'selected' : '',
            onclick: () => {
              if (slice.id !== undefined && slice.trackId !== undefined) {
                globals.dispatch(Actions.selectChromeSlice({id: slice.id, trackId: slice.trackId}));
              }
            },
            style: `background:hsl(${hue}, 50%, ${lum}%);${textColor}`
          },
          children);
    });
  }

  onupdate({dom}: m.CVnodeDOM<{slices: SliceDetails[]}>) {
    const rootElem = dom as HTMLElement;
    const selectedElements = rootElem.getElementsByClassName('selected');
    if (selectedElements.length > 0) {
      selectedElements[0].scrollIntoView({
        block: "nearest"
      });
    }
  }

  renderCanvas() {}
}