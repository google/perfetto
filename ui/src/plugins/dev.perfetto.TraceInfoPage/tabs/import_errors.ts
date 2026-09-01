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
import type {Engine} from '../../../trace_processor/engine';
import {
  CategoryLogsTab,
  type CategoryLogsData,
  type CategoryLogsViewConfig,
  loadCategoryLogsData,
} from './category_logs';

export type ImportErrorsData = CategoryLogsData;

export async function loadImportErrorsData(
  engine: Engine,
): Promise<ImportErrorsData> {
  return loadCategoryLogsData(
    engine,
    "severity = 'error' AND source = 'analysis' AND value > 0",
    'error',
  );
}

const CONFIG: CategoryLogsViewConfig = {
  categoriesTitle: 'Import Error Categories',
  categoriesSubtitle:
    'Summary of import errors grouped by category. These errors occurred during trace processing',
  detailedSubtitle: 'Individual import error entries grouped by category',
  cardSeverity: 'danger',
  cardIcon: 'error',
};

export interface ImportErrorsTabAttrs {
  data: ImportErrorsData;
}

export class ImportErrorsTab implements m.ClassComponent<ImportErrorsTabAttrs> {
  view({attrs}: m.CVnode<ImportErrorsTabAttrs>) {
    return m(CategoryLogsTab, {data: attrs.data, config: CONFIG});
  }
}
