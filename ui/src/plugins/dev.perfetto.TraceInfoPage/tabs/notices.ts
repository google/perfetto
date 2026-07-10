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
import type {Engine} from '../../../trace_processor/engine';
import {
  CategoryLogsTab,
  type CategoryLogsData,
  type CategoryLogsViewConfig,
  loadCategoryLogsData,
} from './category_logs';

export type NoticesData = CategoryLogsData;

export async function loadNoticesData(engine: Engine): Promise<NoticesData> {
  return loadCategoryLogsData(
    engine,
    "severity = 'notice' AND value > 0",
    'notice',
  );
}

const CONFIG: CategoryLogsViewConfig = {
  categoriesTitle: 'Notices',
  categoriesSubtitle:
    'Normal but noteworthy conditions detected during recording or import. These are not errors and do not indicate corruption',
  detailedSubtitle: 'Individual notice entries grouped by category',
  cardSeverity: 'notice',
  cardIcon: 'info',
};

export interface NoticesTabAttrs {
  data: NoticesData;
}

export class NoticesTab implements m.ClassComponent<NoticesTabAttrs> {
  view({attrs}: m.CVnode<NoticesTabAttrs>) {
    return m(CategoryLogsTab, {data: attrs.data, config: CONFIG});
  }
}
