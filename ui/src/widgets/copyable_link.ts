// Copyright (C) 2024 The Android Open Source Project
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
import {copyToClipboard} from '../base/clipboard';
import {Anchor} from './anchor';

interface CopyableLinkAttrs {
  url: string;
  text?: string; // Will use url if omitted.
  noicon?: boolean;
}

export class CopyableLink implements m.ClassComponent<CopyableLinkAttrs> {
  view({attrs}: m.CVnode<CopyableLinkAttrs>) {
    const url = attrs.url;
    return m(
      'div',
      m(
        Anchor,
        {
          href: url,
          title: 'Click to copy the URL into the clipboard',
          target: '_blank',
          icon: attrs.noicon ? undefined : 'content_copy',
          onclick: (e: Event) => {
            e.preventDefault();
            copyToClipboard(url);
          },
        },
        attrs.text ?? url,
      ),
    );
  }
}
