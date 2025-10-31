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
import {Section} from '../../../widgets/section';
import {Icon} from '../../../widgets/icon';
import {Card} from '../../../widgets/card';
import {GridLayout} from '../../../widgets/grid_layout';

export interface UiLoadingErrorsData {
  errors: ReadonlyArray<string>;
}

export interface UiLoadingErrorsTabAttrs {
  data: UiLoadingErrorsData;
}

export class UiLoadingErrorsTab
  implements m.ClassComponent<UiLoadingErrorsTabAttrs>
{
  view({attrs}: m.CVnode<UiLoadingErrorsTabAttrs>) {
    const errors = attrs.data.errors;

    return m(
      '.pf-trace-info-page__tab-content',
      m(
        Section,
        {
          title: 'UI Loading Error Summary',
          subtitle:
            'Errors that occurred in the UI while loading or processing the trace',
        },
        m(
          GridLayout,
          {},
          m(
            Card,
            {
              className:
                'pf-trace-info-page__status-card pf-trace-info-page__status-card--danger',
            },
            m(
              '.pf-trace-info-page__status-card-main',
              m(Icon, {
                icon: 'error',
                className: 'pf-trace-info-page__status-icon',
                filled: true,
              }),
              m(
                '.pf-trace-info-page__status-content',
                m('.pf-trace-info-page__status-title', 'UI Loading Errors'),
                m('.pf-trace-info-page__status-value', errors.length),
              ),
            ),
          ),
        ),
      ),
      m(
        Section,
        {
          title: 'Error Details',
          subtitle: 'Detailed list of errors encountered',
        },
        m(
          '.pf-trace-info-page__loading-errors',
          errors.map((error, idx) =>
            m(
              '.pf-trace-info-page__loading-error-item',
              m('.pf-trace-info-page__loading-error-number', `${idx + 1}.`),
              m('.pf-trace-info-page__loading-error-text', error),
            ),
          ),
        ),
      ),
    );
  }
}
