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

import {generateSqlWithInternalLayout} from '../sql_utils/layout';
import {NAMED_ROW, NamedRow, NamedSliceTrack} from './named_slice_track';
import {createView} from '../../trace_processor/sql_utils';
import {Slice} from '../../public/track';
import {sqlNameSafe} from '../../base/string_utils';
import {Trace} from '../../public/trace';
import {
  Dataset,
  filterToQuery,
  SourceDataset,
} from '../../trace_processor/dataset';
import {LONG, NUM, STR} from '../../trace_processor/query_result';
import {assertTrue} from '../../base/logging';

/**
 * This track implementation is defined using a dataset, with optional automatic
 * layout functionality.
 *
 * At the bare minimum, the following columns must be available in the dataset:
 *
 * - id: NUM
 * - ts: LONG
 * - dur: LONG
 * - name: STR
 *
 * If a `depth: NUM` column is provided, this shall be used to show the depth of
 * the slice, otherwise the slices shall be arranged automatically to avoid
 * overlapping slices.
 */

export interface DatasetSliceTrackAttrs {
  readonly trace: Trace;
  readonly uri: string;
  readonly dataset: SourceDataset;
}

export class DatasetSliceTrack extends NamedSliceTrack<Slice, NamedRow> {
  protected readonly sqlSource: string;
  private readonly createTableOnInit: boolean;

  constructor(private readonly attrs: DatasetSliceTrackAttrs) {
    super(attrs.trace, attrs.uri);

    // This is the minimum viable implementation that the source dataset must
    // implement for the track to work properly.
    assertTrue(
      this.attrs.dataset.implements({
        id: NUM,
        ts: LONG,
        dur: LONG,
        name: STR,
      }),
    );

    // TODO(stevegolton): If we ever support typed datasets, we should enforce
    // this at the type-system level.

    // If the dataset already has a depth property, don't bother doing the
    // automatic layout.
    if (attrs.dataset.implements({depth: NUM})) {
      // The dataset already has a depth property, we don't need to handle the
      // layout ourselves.
      this.sqlSource = attrs.dataset.src;
      this.createTableOnInit = false;
    } else {
      // The dataset doesn't have a depth column, create a new table (on init)
      // that lays out slices automatically.
      this.createTableOnInit = true;
      this.sqlSource = `__dataset_slice_track_${sqlNameSafe(attrs.uri)}`;
    }
  }

  getRowSpec(): NamedRow {
    return NAMED_ROW;
  }

  rowToSlice(row: NamedRow): Slice {
    return this.rowToSliceBase(row);
  }

  async onInit() {
    if (!this.createTableOnInit) return undefined;

    const dataset = this.attrs.dataset;
    return await createView(
      this.engine,
      this.sqlSource,
      generateSqlWithInternalLayout({
        columns: Object.keys(dataset.schema),
        source: dataset.src,
        ts: 'ts',
        dur: 'dur',
        whereClause: dataset.filter ? filterToQuery(dataset.filter) : undefined,
      }),
    );
  }

  getSqlSource(): string {
    return this.sqlSource;
  }

  getDataset(): Dataset {
    return this.attrs.dataset;
  }
}
