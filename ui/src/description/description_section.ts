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

import m from 'mithril';
import {Anchor} from '../widgets/anchor';
import {HTMLAnchorAttrs} from '../widgets/common';

export interface DescriptionAttrs {
  readonly description: string;
}

export class DescriptionSection implements m.ClassComponent<DescriptionAttrs> {
  private parseTextToLinks(
    description: string,
  ): (m.Vnode<HTMLAnchorAttrs> | string)[] {
    const regex = /(@link\{https?:\/\/[^\s{}]+\})/g;
    const parts = description.split(regex);
    let match: RegExpExecArray | null;
    return parts.map((part) => {
      const urlReg = /@link\{(https?:\/\/[^\s{}]+)\}/g;
      if ((match = urlReg.exec(part))) {
        return m(
          Anchor,
          {
            href: match[1],
            target: '_blank',
            icon: 'anchor',
          },
          'link',
        );
      }
      return part;
    });
  }
  view({attrs}: m.Vnode<DescriptionAttrs>): m.Children {
    return m(
      'div',
      {
        style: {
          whiteSpace: 'pre-wrap',
          paddingTop: '5px',
          paddingBottom: '5px',
          wordWrap: 'break-word',
        },
      },
      this.parseTextToLinks(attrs.description),
    );
  }
}
