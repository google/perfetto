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
  DatasetSchema,
  filterToQuery,
  SourceDataset,
} from '../../trace_processor/dataset';
import {LONG, NUM, STR} from '../../trace_processor/query_result';
import {assertTrue} from '../../base/logging';
import {ColorScheme} from '../../base/color_scheme';
import {TrackEventSelection} from '../../public/selection';
import {TrackEventDetailsPanel} from '../../public/details_panel';

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

export interface DatasetSliceTrackAttrs<T extends DatasetSchema> {
  readonly trace: Trace;
  readonly uri: string;
  readonly dataset: SourceDataset<T>;

  // Optional: Define a function which can override the color scheme for each
  // slice in the dataset. If omitted, the default slice colour scheme will be
  // used instead.
  colorizer?(row: T): ColorScheme;

  // Optional: A callback used to customize the details panel displayed when an
  // event on this track is selected. The callback function is called every time
  // an event on this track is selected.
  detailsPanel?(sel: TrackEventSelection): TrackEventDetailsPanel;
}

// This is the minimum viable schema that all datasets must implement.
const rowSchema = {
  id: NUM,
  ts: LONG,
  dur: LONG,
  name: STR,
};

type ROW_SCHEMA = typeof rowSchema;

export class DatasetSliceTrack<
  // TODO(stevegolton): Once we remove all classes that extend DatasetSliceTrack
  // we can remove this default type value. TypeScript is not able to
  // automatically infer this type when using inheritance, but it is able to if
  // instantiating this class directly, so we use a default value here to avoid
  // every subclass from having to pass a type here.
  T extends ROW_SCHEMA = ROW_SCHEMA,
> extends NamedSliceTrack<Slice, NamedRow & T> {
  protected readonly sqlSource: string;
  private readonly createTableOnInit: boolean;

  constructor(private readonly attrs: DatasetSliceTrackAttrs<T>) {
    super(attrs.trace, attrs.uri, {...NAMED_ROW, ...attrs.dataset.schema});

    // This is the minimum viable implementation that the source dataset must
    // implement for the track to work properly. Typescript should enforce this
    // now, but typescript can be worked around, and checking it is cheap.
    // Better to error out early.
    assertTrue(this.attrs.dataset.implements(rowSchema));

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

  rowToSlice(row: NamedRow & T): Slice {
    const slice = this.rowToSliceBase(row);
    // Use the colorizer if we have been passed one.
    return {
      ...slice,
      colorScheme: this.attrs.colorizer?.(row) ?? slice.colorScheme,
    };
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

  detailsPanel(sel: TrackEventSelection): TrackEventDetailsPanel | undefined {
    return this.attrs.detailsPanel?.(sel) ?? super.detailsPanel(sel);
  }
}
