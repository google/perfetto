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

// Component harness fixtures for OpenForm, the content of the "Open from
// Google Drive" dialog. Only bundled into the component harness
// (ui/build --enable-component-harness), never shipped.

import m from 'mithril';
import {
  type FixtureArgs,
  type FixtureModule,
  record,
} from '../../../component_harness/registry';
import {OpenForm} from './open_form';

export default {
  // Initial empty state.
  empty: (args) => openForm(args, ''),

  // A valid Google Drive file link pasted into the input.
  validLink: (args) =>
    openForm(
      args,
      'https://drive.google.com/file/d/file_123/view?resourcekey=0-key_9',
    ),

  // An invalid link showing the validation error.
  invalidLink: (args) => openForm(args, 'https://example.com/not-a-drive-link'),
} satisfies FixtureModule;

function openForm(args: FixtureArgs, initialLink: string): m.Component {
  let link = args.link ?? initialLink;
  return {
    view: () =>
      m(
        'div',
        {style: {maxWidth: '480px'}},
        m(OpenForm, {
          link,
          onLinkChange: (value) => {
            link = value;
            record({link: value});
          },
          onBrowse: () => record({action: 'browse'}),
          onOpenLink: (file) => record({action: 'openLink', file}),
        }),
      ),
  };
}
