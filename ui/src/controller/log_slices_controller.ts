// Copyright (C) 2019 The Android Open Source Project
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


import {Engine} from '../common/engine';
import {TimestampedAreaSelection} from '../common/state';
import {fromNs, toNs} from '../common/time';
import {SliceDetails} from '../frontend/globals';
import {Config, SLICE_TRACK_KIND} from '../tracks/chrome_slices/common';

import {Controller} from './controller';
import {globals} from './globals';

export interface LogSlicesControllerArgs {
  engine: Engine;
}

export class LogSlicesController extends Controller<'main'> {
  private previousArea: TimestampedAreaSelection = {lastUpdate: 0};
  private previousSearch: string = '';
  private requestingData = false;
  private queuedRequest = false;

  constructor(private args: LogSlicesControllerArgs) {
    super('main');
  }

  // TODO this should be centralized
  getSearch() {
    const omniboxState = globals.state.frontendLocalState.omniboxState;
    if (omniboxState.mode == "SEARCH" && omniboxState.omnibox.length >= 4) {
        return omniboxState.omnibox.trim();
    } else {
      return '';
    }
  }

  run() {
    const selectedArea = globals.state.frontendLocalState.selectedArea;
    const search = this.getSearch();
    if (this.previousArea.lastUpdate >= selectedArea.lastUpdate &&
      search === this.previousSearch) {
      return;
    };

    if (this.requestingData) {
      this.queuedRequest = true;
    } else {
      this.requestingData = true;
      this.previousArea = selectedArea;
      this.previousSearch = search;
      this.getLogSlices(search)
          .then(slices => globals.publish('LogSlices', slices))
          .catch(reason => {
            console.error(reason);
          })
          .finally(() => {
            this.requestingData = false;
            if (this.queuedRequest) {
              this.queuedRequest = false;
              this.run();
            }
          });
    }
  }

  private static ColumnNames =
      ['ts', 'dur', 'name', 'cat', 'id', 'depth', 'arg_set_id', 'track_id'];

  private static PropertyNames =
      LogSlicesController.ColumnNames.map((colName: string) => {
        return colName.replace(/_(\w)/g, ((m: string) => m[1].toUpperCase()));
      });

  async getLogSlices(search: string): Promise<SliceDetails[]> {
    const selectedArea = globals.state.frontendLocalState.selectedArea;
    const area = selectedArea.area;
    let queryAndedClauses = [];
    if (area !== undefined) {
      // TODO tracks should be selectable without selecting a time region.
      queryAndedClauses.push(`slice.ts + slice.dur > ${toNs(area.startSec)}`);
      queryAndedClauses.push(`slice.ts < ${toNs(area.endSec)}`);

      const trackIds = [];
      for (const trackId of area.tracks) {
        const track = globals.state.tracks[trackId];
        // Track will be undefined for track groups.
        if (track !== undefined && track.kind === SLICE_TRACK_KIND) {
          trackIds.push((track.config as Config).trackId);
        }
      }
      if (trackIds.length !== 0) {
        queryAndedClauses.push(`slice.track_id IN (${trackIds.join(',')})`);
      }
    }

    if (search.length > 0) {
      queryAndedClauses.push(`slice.name LIKE "%${search}%"`);
    }

    let whereClause = '';
    if (queryAndedClauses.length > 0) {
      whereClause = `WHERE ${queryAndedClauses.join(' AND\n')}`;
    }

    const query = `SELECT ${LogSlicesController.ColumnNames.join(',')}
      FROM slice
      ${whereClause}
      ORDER BY ts ASC`;

    const result = await this.args.engine.query(query);
    const numRows = +result.numRecords;

    const cols = result.columns;
    const slices = [];
    for (let row = 0; row < numRows; row++) {
      const slicePojo: {[key: string]: string|number} = {};
      for (let col = 0; col < LogSlicesController.ColumnNames.length; col++) {
        if (cols[col].stringValues && cols[col].stringValues!.length > 0) {
          slicePojo[LogSlicesController.PropertyNames[col]] =
              cols[col].stringValues![row];
        } else if (cols[col].longValues && cols[col].longValues!.length > 0) {
          slicePojo[LogSlicesController.PropertyNames[col]] =
              cols[col].longValues![row] as number;
        } else if (
            cols[col].doubleValues && cols[col].doubleValues!.length > 0) {
          slicePojo[LogSlicesController.PropertyNames[col]] =
              cols[col].doubleValues![row];
        }
      }
      const slice = slicePojo as SliceDetails;
      slice.ts =
          fromNs(slice.ts ? slice.ts : globals.state.traceTime.startSec) -
          globals.state.traceTime.startSec;
      slices.push(slice);
    }

    return slices;
  }
}
