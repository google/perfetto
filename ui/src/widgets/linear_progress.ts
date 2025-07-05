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

import m from 'mithril';
import {HTMLAttrs} from './common';
import {classNames} from '../base/classnames';

export interface LinearProgressAttrs extends HTMLAttrs {
  readonly state?: 'none' | 'indeterminate';
}

/**
 * A linear progress bar component that can be used to indicate loading state.
 *
 * It supports two states:
 * - none: not loading, no progress bar shown.
 * - indeterminate: loading but progress in indeterminate.
 */
export class LinearProgress implements m.ClassComponent<LinearProgressAttrs> {
  view({attrs}: m.CVnode<LinearProgressAttrs>): m.Children {
    return m('.pf-linear-progress', {
      ...attrs,
      className: classNames(
        attrs.state === 'indeterminate' && 'pf-linear-progress--anim',
        attrs.className,
      ),
    });
  }
}
