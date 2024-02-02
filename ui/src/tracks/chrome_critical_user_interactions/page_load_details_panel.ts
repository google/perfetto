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

import {
  BottomTab,
  bottomTabRegistry,
  NewBottomTabArgs,
} from '../../frontend/bottom_tab';
import {
  GenericSliceDetailsTabConfig,
} from '../../frontend/generic_slice_details_tab';
import {Details, DetailsSchema} from '../../frontend/sql/details/details';
import {wellKnownTypes} from '../../frontend/sql/details/well_known_types';
import {DetailsShell} from '../../widgets/details_shell';
import {GridLayout, GridLayoutColumn} from '../../widgets/grid_layout';

import d = DetailsSchema;

export class PageLoadDetailsPanel extends
  BottomTab<GenericSliceDetailsTabConfig> {
  static readonly kind = 'org.perfetto.PageLoadDetailsPanel';
  private data: Details;

  static create(args: NewBottomTabArgs<GenericSliceDetailsTabConfig>):
      PageLoadDetailsPanel {
    return new PageLoadDetailsPanel(args);
  }

  constructor(args: NewBottomTabArgs<GenericSliceDetailsTabConfig>) {
    super(args);
    this.data = new Details(
      this.engine,
      'chrome_page_loads',
      this.config.id,
      {
        'Navigation start': d.Timestamp('navigation_start_ts'),
        'FCP event': d.Timestamp('fcp_ts'),
        'FCP': d.Interval('navigation_start_ts', 'fcp'),
        'LCP event': d.Timestamp('lcp_ts', {skipIfNull: true}),
        'LCP': d.Interval('navigation_start_ts', 'lcp', {skipIfNull: true}),
        'DOMContentLoaded':
              d.Timestamp('dom_content_loaded_event_ts', {skipIfNull: true}),
        'onload timestamp': d.Timestamp('load_event_ts', {skipIfNull: true}),
        'performance.mark timings': d.Dict({
          data: {
            'Fully loaded':
                  d.Timestamp('mark_fully_loaded_ts', {skipIfNull: true}),
            'Fully visible':
                  d.Timestamp('mark_fully_visible_ts', {skipIfNull: true}),
            'Interactive':
                  d.Timestamp('mark_interactive_ts', {skipIfNull: true}),
          },
          skipIfEmpty: true,
        }),
        'Navigation ID': 'navigation_id',
        'Browser process': d.SqlIdRef('process', 'browser_upid'),
        'URL': d.URLValue('url'),
      },
      wellKnownTypes);
  }

  viewTab() {
    return m(
      DetailsShell,
      {
        title: this.getTitle(),
      },
      m(GridLayout, m(GridLayoutColumn, this.data.render())));
  }

  getTitle(): string {
    return this.config.title;
  }

  isLoading() {
    return this.data.isLoading();
  }
}

bottomTabRegistry.register(PageLoadDetailsPanel);
