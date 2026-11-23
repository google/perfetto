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

import './linear_progress.scss';
import m from 'mithril';
import type {HTMLAttrs} from './common';
import {classNames} from '../base/classnames';

export interface LinearProgressAttrs extends HTMLAttrs {
  readonly state?: 'none' | 'indeterminate';
  // Progress from 0 to 1. When set, the bar fills to this fraction instead of
  // following `state`. Values outside [0, 1] are clamped.
  readonly value?: number;
}

/**
 * A linear progress bar component that can be used to indicate loading state.
 *
 * It supports three modes:
 * - none: not loading, no progress bar shown.
 * - indeterminate: loading but progress is unknown (animated).
 * - determinate: `value` is set, and the bar fills to that fraction.
 */
export class LinearProgress implements m.ClassComponent<LinearProgressAttrs> {
  view({attrs}: m.CVnode<LinearProgressAttrs>): m.Children {
    const {state, value, className, ...htmlAttrs} = attrs;
    const determinate = value !== undefined;
    const fraction = determinate ? Math.min(Math.max(value, 0), 1) : 0;
    const busy = determinate || state === 'indeterminate';
    return m(
      '.pf-linear-progress',
      {
        ...htmlAttrs,
        className: classNames(
          !determinate &&
            state === 'indeterminate' &&
            'pf-linear-progress--anim',
          className,
        ),
        role: busy ? 'progressbar' : undefined,
        ...(determinate && {
          'aria-valuemin': 0,
          'aria-valuemax': 100,
          'aria-valuenow': Math.round(fraction * 100),
        }),
      },
      determinate &&
        m('.pf-linear-progress__fill', {
          style: {transform: `scaleX(${fraction})`},
        }),
    );
  }
}
