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

// Copyright 2025 The Lynx Authors. All rights reserved.
// Licensed under the Apache License Version 2.0 that can be found in the
// LICENSE file in the root directory of this source tree.

import {PerfettoPlugin} from '../../public/plugin';
import {App} from '../../public/app';

/**
 * Plugin to handle router arguments and control UI components accordingly.
 */
export default class LynxRouterArgsPlugin implements PerfettoPlugin {
  static readonly id = 'lynx.RouterArgs';
  static async onActivate(ctx: App) {
    const args = ctx.initialRouteArgs;
    // If the route argument 'hide' is true and the sidebar is visible, hide the sidebar.
    if (args.hide && ctx.sidebar.visible) {
      ctx.sidebar.toggleVisibility();
    }
  }
}
