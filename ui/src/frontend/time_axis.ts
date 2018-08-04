// Copyright (C) 2018 The Android Open Source Project
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

import {DESIRED_PX_PER_STEP, getGridStepSize} from './gridline_helper';
import {Milliseconds, TimeScale} from './time_scale';

/**
 * Axis for showing time ticks.
 */
export const TimeAxis = {
  oncreate(vnode) {
    const rect = vnode.dom.getBoundingClientRect();
    this.width = rect.width;
  },
  onupdate(vnode) {
    const rect = vnode.dom.getBoundingClientRect();
    this.width = rect.width;
  },
  view({attrs}) {
    const range = attrs.visibleWindowMs.end - attrs.visibleWindowMs.start;
    const desiredSteps = this.width / DESIRED_PX_PER_STEP;
    const step = getGridStepSize(range, desiredSteps);

    let unit = 'ns';
    let representationFactor = 1 / 1000;

    if (step / representationFactor > 1000) {
      unit = 'ms';
      representationFactor *= 1000;
    }
    if (step / representationFactor > 1000) {
      unit = 's';
      representationFactor *= 1000;
    }

    const start = Math.round(attrs.visibleWindowMs.start / step) * step;
    const gridMarks = [];

    for (let t: Milliseconds = start; t < attrs.visibleWindowMs.end;
         t += step) {
      const xPos = Math.floor(attrs.timeScale.msToPx(t));

      if (xPos >= 0 && xPos <= this.width - attrs.contentOffset) {
        const template =
            m('.mark',
              {
                style: {
                  position: 'absolute',
                  overflow: 'visible',
                  left: `${xPos}px`,
                }
              },
              m('.mark-label',
                Math.round(t / representationFactor).toString() + unit),
              m('.tick'));

        gridMarks.push(template);
      }
    }

    return m(
        '.axis',
        {
          style: {
            width: `${this.width}px`,
          },
        },
        m('.axis-content',
          {
            style: {
              position: 'absolute',
              left: `${attrs.contentOffset}px`,
              width: '100%',
              height: '100%',
              overflow: 'visible',
            }
          },
          ...gridMarks));
  },
} as m.Component<{
  timeScale: TimeScale,
  contentOffset: number,
  visibleWindowMs: {start: number, end: number},
},
                        {
                          width: number,
                        }>;
