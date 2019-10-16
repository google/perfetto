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

import {globals} from '../../controller/globals';
import {
  TrackController,
  trackControllerRegistry
} from '../../controller/track_controller';
import {CallsiteInfo} from '../../frontend/globals';

import {
  Config,
  Data,
  HEAP_PROFILE_FLAMEGRAPH_TRACK_KIND,
  HeapProfileFlamegraphKey,
} from './common';

class HeapProfileFlameraphTrackController extends
    TrackController<Config, Data> {
  static readonly kind = HEAP_PROFILE_FLAMEGRAPH_TRACK_KIND;
  private start = 0;
  private end = 0;
  private resolution = 0;
  private length = 0;
  private lastSelectedTs?: number;
  private lastSelectedId?: number;

  async onBoundsChange(start: number, end: number, resolution: number):
      Promise<Data> {
    this.start = start;
    this.end = end;
    this.resolution = resolution;

    return this.generateEmptyData();
  }

  private generateEmptyData() {
    const data: Data = {
      start: -1,
      end: -1,
      resolution: this.resolution,
      length: 0,
      flamegraph: new Array()
    };
    return data;
  }

  run() {
    super.run();
    const selection = globals.state.currentHeapProfileFlamegraph;
    if (selection && selection.kind === 'HEAP_PROFILE_FLAMEGRAPH') {
      if (this.lastSelectedId === selection.id &&
          this.lastSelectedTs === selection.ts) {
        return;
      }
      const selectedId = selection.id;
      const selectedKind = selection.kind;
      const selectedTs = selection.ts;
      this.lastSelectedId = selection.id;
      this.lastSelectedTs = selection.ts;

      // Sending empty data to show tha Loading state before we get an actual
      // data.
      globals.publish(
          'TrackData',
          {id: HeapProfileFlamegraphKey, data: this.generateEmptyData()});

      this.getFlamegraphData(selection.ts, selection.upid).then(flamegraph => {
        if (flamegraph !== undefined && selection &&
            selection.kind === selectedKind && selection.id === selectedId &&
            selection.ts === selectedTs) {
          globals.publish('TrackData', {
            id: HeapProfileFlamegraphKey,
            data: {
              start: this.start,
              end: this.end,
              resolution: this.resolution,
              length: this.length,
              flamegraph
            }
          });
        }
      });
    }
  }

  async getFlamegraphData(ts: number, upid: number) {
    // Collecting data for drawing flagraph for selected heap profile.
    // Data needs to be in following format:
    // id, name, parent_id, depth, total_size

    // Creating unique names for views so we can reuse and not delete them
    // for each marker.
    const tableNameCallsiteNameSize =
        this.tableName(`callsite_with_name_and_size_${ts}`);
    const tableNameCallsiteHashNameSize =
        this.tableName(`callsite_hash_name_size_${ts}`);
    // Joining the callsite table with frame table then with alloc table to get
    // the size and name for each callsite.
    await this.query(
        // TODO(tneda|lalitm): get names from symbols to exactly replicate
        // pprof.
        `create view if not exists ${tableNameCallsiteNameSize} as
      select cs.id, parent_id, depth, name, SUM(IFNULL(size, 0)) as size
      from stack_profile_callsite cs
      join stack_profile_frame on cs.frame_id = stack_profile_frame.id
      left join heap_profile_allocation alloc on alloc.callsite_id = cs.id and
      alloc.ts <= ${ts} and alloc.upid = ${upid} group by cs.id`);

    // Recursive query to compute the hash for each callsite based on names
    // rather than ids.
    // We get all the children of the row in question and emit a row with hash
    // equal hash(name, parent.hash). Roots without the parent will have -1 as
    // hash.  Slices will be merged into a big slice.
    await this.query(
        `create view if not exists ${tableNameCallsiteHashNameSize} as
      with recursive callsite_table_names(
        id, hash, name, size, parent_hash, depth) AS (
      select id, hash(name) as hash, name, size, -1, depth
      from ${tableNameCallsiteNameSize}
      where depth = 0
      UNION ALL
      SELECT cs.id, hash(cs.name, ctn.hash) as hash, cs.name, cs.size, ctn.hash,
      cs.depth
      FROM callsite_table_names ctn
      INNER JOIN ${tableNameCallsiteNameSize} cs ON ctn.id = cs.parent_id
      )
      SELECT hash, name, parent_hash, depth, SUM(size) as size
      FROM callsite_table_names
      group by hash`);

    // Recursive query to compute the cumulative size of each callsite.
    // Base case: We get all the callsites where the size is non-zero.
    // Recursive case: We get the callsite which is the parent of the current
    //  callsite(in terms of hashes) and emit a row with the size of the current
    //  callsite plus all the info of the parent.
    // Grouping: For each callsite, our recursive table has n rows where n is
    //  the number of descendents with a non-zero self size. We need to group on
    //  the hash and sum all the sizes to get the cumulative size for each
    //  callsite hash.
    const callsites = await this.query(
        `with recursive callsite_children(hash, name, parent_hash, depth, size)
        AS (
        select *
        from ${tableNameCallsiteHashNameSize}
        where size > 0
        union all
        select chns.hash, chns.name, chns.parent_hash, chns.depth, cc.size
        from ${tableNameCallsiteHashNameSize} chns
        inner join callsite_children cc on chns.hash = cc.parent_hash
        )
        SELECT hash, name, parent_hash, depth, SUM(size) as size
        from callsite_children
        group by hash
        order by depth, parent_hash, size desc, name`);

    const flamegraphData: CallsiteInfo[] = new Array();
    for (let i = 0; i < callsites.numRecords; i++) {
      const hash = callsites.columns[0].longValues![i];
      const name = callsites.columns[1].stringValues![i];
      const parentHash = callsites.columns[2].longValues![i];
      const depth = callsites.columns[3].longValues![i];
      const totalSize = callsites.columns[4].longValues![i];
      flamegraphData.push({
        hash: +hash,
        totalSize: +totalSize,
        depth: +depth,
        parentHash: +parentHash,
        name
      });
    }
    return flamegraphData;
  }
}

trackControllerRegistry.register(HeapProfileFlameraphTrackController);
