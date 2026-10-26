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

import './preview_banner.scss';
import m from 'mithril';
import type {App} from '../../../public/app';
import {getBugReportUrl} from '../../../public/utils';
import {Anchor} from '../../../widgets/anchor';
import {Callout} from './callout';

export interface PreviewBannerAttrs {
  readonly app: App;
}

export class PreviewBanner implements m.ClassComponent<PreviewBannerAttrs> {
  view({attrs}: m.CVnode<PreviewBannerAttrs>) {
    return m(
      Callout,
      {
        className: 'pf-memscope-preview-callout',
        icon: 'info',
      },
      m('.pf-memscope-preview-banner__body', [
        m(
          'span.pf-memscope-preview-banner__message',
          'This feature is in preview.',
        ),
        m(
          '.pf-memscope-preview-banner__actions',
          m(
            Anchor,
            {
              href: getBugReportUrl(attrs.app),
              target: '_blank',
              startIcon: 'bug_report',
            },
            'Provide feedback',
          ),
        ),
      ]),
    );
  }
}
