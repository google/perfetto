// Copyright (C) 2023 The Android Open Source Project
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

import {Icon} from './icon';

interface CalloutAttrs {
  icon?: string;
  // Remaining attributes forwarded to the underlying HTML <button>.
  [htmlAttrs: string]: any;
}

export class Callout implements m.ClassComponent<CalloutAttrs> {
  view({attrs, children}: m.CVnode<CalloutAttrs>) {
    const {icon, ...htmlAttrs} = attrs;

    return m(
        '.pf-callout',
        {...htmlAttrs},
        icon && m(Icon, {className: 'pf-left-icon', icon}),
        children,
    );
  }
}
