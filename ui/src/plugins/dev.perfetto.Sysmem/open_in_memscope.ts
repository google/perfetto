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

import m from 'mithril';
import type {App} from '../../public/app';
import {Button, ButtonVariant} from '../../widgets/button';
import {
  memscopeProcessHref,
  memscopeProductProcessUrl,
} from '../dev.perfetto.Memscope/live';

// In the Perfetto UI Memscope runs alongside Sysmem, so navigate in place.
// The standalone Sysmem product has no Memscope: open the Memscope product.
export function openInMemscope(app: App, processName: string): void {
  if (app.productId === undefined) {
    app.navigate(memscopeProcessHref(processName));
  } else {
    window.open(memscopeProductProcessUrl(processName), '_blank', 'noopener');
  }
}

export function openInMemscopeButton(
  app: App,
  processName: string,
): m.Children {
  return m(Button, {
    label: 'Memscope',
    icon: 'memory',
    rounded: true,
    variant: ButtonVariant.Outlined,
    tooltip: `Open ${processName} in Memscope`,
    onclick: () => openInMemscope(app, processName),
  });
}
