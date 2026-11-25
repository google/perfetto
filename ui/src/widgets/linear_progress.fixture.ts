// Copyright (C) 2026 The Android Open Source Project
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

// Component harness fixtures for LinearProgress. Only bundled into the
// component harness (ui/build --enable-component-harness), never shipped.

import m from 'mithril';
import type {FixtureArgs, FixtureModule} from '../component_harness/registry';
import {LinearProgress, type LinearProgressAttrs} from './linear_progress';

// The bar is as wide as its container and only 1px tall by default, so give
// it a fixed width (and a visible track) to make it easy to see.
//
// Args, for tweaking any of the states below:
//   thick=true   render 6px tall instead of the default hairline
function barAttrs(
  args: FixtureArgs,
  attrs: LinearProgressAttrs,
): LinearProgressAttrs {
  return {
    ...attrs,
    style: {
      width: '320px',
      background: 'var(--pf-color-background-secondary)',
      ...(args.thick === 'true' && {height: '6px', marginBottom: '0'}),
    },
  };
}

function bar(args: FixtureArgs, attrs: LinearProgressAttrs): m.Component {
  return {view: () => m(LinearProgress, barAttrs(args, attrs))};
}

export default {
  none: (args) => bar(args, {state: 'none'}),
  indeterminate: (args) => bar(args, {state: 'indeterminate'}),
  empty: (args) => bar(args, {value: 0}),
  half: (args) => bar(args, {value: 0.5}),
  full: (args) => bar(args, {value: 1}),

  // Steps through 0% -> 100% in 10% increments every 500ms, to show the
  // transition between values.
  animated: (args) => {
    let value = 0;
    let timer: ReturnType<typeof setInterval> | undefined;
    return {
      oncreate: () => {
        timer = setInterval(() => {
          value = value >= 1 ? 0 : Math.round((value + 0.1) * 10) / 10;
          m.redraw();
        }, 500);
      },
      onremove: () => clearInterval(timer),
      view: () => m(LinearProgress, barAttrs(args, {value})),
    };
  },
} satisfies FixtureModule;
