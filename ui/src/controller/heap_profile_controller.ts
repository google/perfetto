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
import {
  ALLOC_SPACE_MEMORY_ALLOCATED_KEY,
  DEFAULT_VIEWING_OPTION,
  expandCallsites,
  findRootSize,
  mergeCallsites,
  OBJECTS_ALLOCATED_KEY,
  OBJECTS_ALLOCATED_NOT_FREED_KEY,
  SPACE_MEMORY_ALLOCATED_NOT_FREED_KEY
} from '../common/flamegraph_util';
import {CallsiteInfo, HeapProfileFlamegraph} from '../common/state';
import {fromNs} from '../common/time';
import {HeapProfileDetails} from '../frontend/globals';

import {Controller} from './controller';
import {globals} from './globals';

export interface HeapProfileControllerArgs {
  engine: Engine;
}
const MIN_PIXEL_DISPLAYED = 1;

export class HeapProfileController extends Controller<'main'> {
  private flamegraphDatasets: Map<string, CallsiteInfo[]> = new Map();
  private lastSelectedHeapProfile?: HeapProfileFlamegraph;
  private requestingData = false;
  private queuedRequest = false;
  private heapProfileDetails: HeapProfileDetails = {};

  constructor(private args: HeapProfileControllerArgs) {
    super('main');
  }

  run() {
    const selection = globals.state.currentHeapProfileFlamegraph;

    if (!selection) return;

    if (this.shouldRequestData(selection)) {
      if (this.requestingData) {
        this.queuedRequest = true;
      } else {
        this.requestingData = true;
        const selectedHeapProfile: HeapProfileFlamegraph =
            this.copyHeapProfile(selection);

        this.getHeapProfileMetadata(
                selectedHeapProfile.ts, selectedHeapProfile.upid)
            .then(result => {
              if (result !== undefined) {
                Object.assign(this.heapProfileDetails, result);
              }

              this.lastSelectedHeapProfile = this.copyHeapProfile(selection);

              const expandedId = selectedHeapProfile.expandedCallsite ?
                  selectedHeapProfile.expandedCallsite.id :
                  -1;
              const rootSize =
                  selectedHeapProfile.expandedCallsite === undefined ?
                  undefined :
                  selectedHeapProfile.expandedCallsite.totalSize;

              const key =
                  `${selectedHeapProfile.upid};${selectedHeapProfile.ts}`;

              this.getFlamegraphData(
                      key,
                      selectedHeapProfile.viewingOption ?
                          selectedHeapProfile.viewingOption :
                          DEFAULT_VIEWING_OPTION,
                      selection.ts,
                      selectedHeapProfile.upid)
                  .then(flamegraphData => {
                    if (flamegraphData !== undefined && selection &&
                        selection.kind === selectedHeapProfile.kind &&
                        selection.id === selectedHeapProfile.id &&
                        selection.ts === selectedHeapProfile.ts) {
                      const expandedFlamegraphData =
                          expandCallsites(flamegraphData, expandedId);
                      this.prepareAndMergeCallsites(
                          expandedFlamegraphData,
                          this.lastSelectedHeapProfile!.viewingOption,
                          rootSize,
                          this.lastSelectedHeapProfile!.expandedCallsite);
                    }
                  })
                  .finally(() => {
                    this.requestingData = false;
                    if (this.queuedRequest) {
                      this.queuedRequest = false;
                      this.run();
                    }
                  });
            });
      }
    }
  }

  private copyHeapProfile(heapProfile: HeapProfileFlamegraph):
      HeapProfileFlamegraph {
    return {
      kind: heapProfile.kind,
      id: heapProfile.id,
      upid: heapProfile.upid,
      ts: heapProfile.ts,
      expandedCallsite: heapProfile.expandedCallsite,
      viewingOption: heapProfile.viewingOption
    };
  }

  private shouldRequestData(selection: HeapProfileFlamegraph) {
    return selection.kind === 'HEAP_PROFILE_FLAMEGRAPH' &&
        (this.lastSelectedHeapProfile === undefined ||
         (this.lastSelectedHeapProfile !== undefined &&
          (this.lastSelectedHeapProfile.id !== selection.id ||
           this.lastSelectedHeapProfile.ts !== selection.ts ||
           this.lastSelectedHeapProfile.upid !== selection.upid ||
           this.lastSelectedHeapProfile.viewingOption !==
               selection.viewingOption ||
           this.lastSelectedHeapProfile.expandedCallsite !==
               selection.expandedCallsite)));
  }

  private prepareAndMergeCallsites(
      flamegraphData: CallsiteInfo[],
      viewingOption: string|undefined = DEFAULT_VIEWING_OPTION,
      rootSize?: number, expandedCallsite?: CallsiteInfo) {
    const mergedFlamegraphData = mergeCallsites(
        flamegraphData, this.getMinSizeDisplayed(flamegraphData, rootSize));
    this.heapProfileDetails.flamegraph = mergedFlamegraphData;
    this.heapProfileDetails.expandedCallsite = expandedCallsite;
    this.heapProfileDetails.viewingOption = viewingOption;
    globals.publish('HeapProfileDetails', this.heapProfileDetails);
  }


  async getFlamegraphData(
      baseKey: string, viewingOption: string, ts: number,
      upid: number): Promise<CallsiteInfo[]> {
    let currentData: CallsiteInfo[];
    const key = `${baseKey}-${viewingOption}`;
    if (this.flamegraphDatasets.has(key)) {
      currentData = this.flamegraphDatasets.get(key)!;
    } else {
      // TODO(taylori): Show loading state.

      // Collecting data for drawing flamegraph for selected heap profile.
      // Data needs to be in following format:
      // id, name, parent_id, depth, total_size
      const tableName = await this.prepareViewsAndTables(ts, upid);
      currentData =
          await this.getFlamegraphDataFromTables(tableName, viewingOption);
      this.flamegraphDatasets.set(key, currentData);
    }
    return currentData;
  }

  async getFlamegraphDataFromTables(
      tableName: string, viewingOption = DEFAULT_VIEWING_OPTION) {
    let orderBy = '';
    let sizeIndex = 4;
    switch (viewingOption) {
      case SPACE_MEMORY_ALLOCATED_NOT_FREED_KEY:
        orderBy = `where size > 0 order by depth, parent_hash, size desc, name`;
        sizeIndex = 4;
        break;
      case ALLOC_SPACE_MEMORY_ALLOCATED_KEY:
        orderBy =
            `where alloc_size > 0 order by depth, parent_hash, alloc_size desc,
            name`;
        sizeIndex = 5;
        break;
      case OBJECTS_ALLOCATED_NOT_FREED_KEY:
        orderBy =
            `where count > 0 order by depth, parent_hash, count desc, name`;
        sizeIndex = 6;
        break;
      case OBJECTS_ALLOCATED_KEY:
        orderBy = `where alloc_count > 0 order by depth, parent_hash,
            alloc_count desc, name`;
        sizeIndex = 7;
        break;
      default:
        break;
    }

    const callsites = await this.args.engine.query(
        `SELECT hash, name, parent_hash, depth, size, alloc_size, count,
        alloc_count, map_name, self_size from ${tableName} ${orderBy}`);

    const flamegraphData: CallsiteInfo[] = new Array();
    const hashToindex: Map<number, number> = new Map();
    for (let i = 0; i < callsites.numRecords; i++) {
      const hash = callsites.columns[0].longValues![i];
      const name = callsites.columns[1].stringValues![i];
      const parentHash = callsites.columns[2].longValues![i];
      const depth = +callsites.columns[3].longValues![i];
      const totalSize = +callsites.columns[sizeIndex].longValues![i];
      const mapping = callsites.columns[8].stringValues![i];
      const selfSize = +callsites.columns[9].longValues![i];
      const parentId =
          hashToindex.has(+parentHash) ? hashToindex.get(+parentHash)! : -1;
      hashToindex.set(+hash, i);
      // Instead of hash, we will store index of callsite in this original array
      // as an id of callsite. That way, we have quicker access to parent and it
      // will stay unique.
      flamegraphData.push(
          {id: i, totalSize, depth, parentId, name, selfSize, mapping});
    }
    return flamegraphData;
  }

  private async prepareViewsAndTables(ts: number, upid: number):
      Promise<string> {
    // Creating unique names for views so we can reuse and not delete them
    // for each marker.
    const tableNameCallsiteNameSize =
        this.tableName(`callsite_with_name_and_size`);
    const tableNameCallsiteHashNameSize =
        this.tableName(`callsite_hash_name_size`);
    const tableNameGroupedCallsitesForFlamegraph =
        this.tableName(`grouped_callsites_for_flamegraph`);
    // Joining the callsite table with frame table then with alloc table to get
    // the size and name for each callsite.
    // TODO(taylori): Make frame name nullable in the trace processor for
    // consistency with the other columns.
    await this.args.engine.query(
        `create view if not exists ${tableNameCallsiteNameSize} as
         select id, parent_id, depth, IFNULL(DEMANGLE(name), name) as name,
            map_name, size, alloc_size, count, alloc_count from (
         select cs.id as id, parent_id, depth,
            coalesce(symbols.name,
                case when fr.name != '' then fr.name else map.name end) as name,
            map.name as map_name,
            SUM(IFNULL(size, 0)) as size,
            SUM(IFNULL(size, 0)) as size,
            SUM(case when size > 0 then size else 0 end) as alloc_size,
            SUM(IFNULL(count, 0)) as count,
            SUM(case when count > 0 then count else 0 end) as alloc_count
         from stack_profile_callsite cs
         join stack_profile_frame fr on cs.frame_id = fr.id
         join stack_profile_mapping map on fr.mapping = map.id
         inner join (
              select symbol_set_id, FIRST_VALUE(name) OVER(PARTITION BY
                symbol_set_id) as name
              from stack_profile_symbol GROUP BY symbol_set_id
            ) as symbols using(symbol_set_id)
         left join heap_profile_allocation alloc on alloc.callsite_id = cs.id
         and alloc.ts <= ${ts} and alloc.upid = ${upid} group by cs.id)`);

    // Recursive query to compute the hash for each callsite based on names
    // rather than ids.
    // We get all the children of the row in question and emit a row with hash
    // equal hash(name, parent.hash). Roots without the parent will have -1 as
    // hash.  Slices will be merged into a big slice.
    await this.args.engine.query(
        `create view if not exists ${tableNameCallsiteHashNameSize} as
        with recursive callsite_table_names(
          id, hash, name, map_name, size, alloc_size, count, alloc_count,
          parent_hash, depth) AS (
        select id, hash(name) as hash, name, map_name, size, alloc_size, count,
          alloc_count, -1, depth
        from ${tableNameCallsiteNameSize}
        where depth = 0
        union all
        select cs.id, hash(cs.name, ctn.hash) as hash, cs.name, cs.map_name,
          cs.size, cs.alloc_size, cs.count, cs.alloc_count, ctn.hash, cs.depth
        from callsite_table_names ctn
        inner join ${tableNameCallsiteNameSize} cs ON ctn.id = cs.parent_id
        )
        select hash, name, map_name, parent_hash, depth, SUM(size) as size,
          SUM(case when alloc_size > 0 then alloc_size else 0 end)
            as alloc_size, SUM(count) as count,
          SUM(case when alloc_count > 0 then alloc_count else 0 end)
            as alloc_count
        from callsite_table_names
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
    await this.args.engine.query(`create temp table if not exists ${
        tableNameGroupedCallsitesForFlamegraph}
        as with recursive callsite_children(
          hash, name, map_name, parent_hash, depth, size, alloc_size, count,
          alloc_count, self_size, self_alloc_size, self_count, self_alloc_count)
        as (
        select hash, name, map_name, parent_hash, depth, size, alloc_size,
          count, alloc_count, size as self_size, alloc_size as self_alloc_size,
          count as self_count, alloc_count as self_alloc_count
        from ${tableNameCallsiteHashNameSize}
        union all
        select chns.hash, chns.name, chns.map_name, chns.parent_hash,
          chns.depth, cc.size, cc.alloc_size, cc.count, cc.alloc_count,
          chns.size, chns.alloc_size, chns.count, chns.alloc_count
        from ${tableNameCallsiteHashNameSize} chns
        inner join callsite_children cc on chns.hash = cc.parent_hash
        )
        select hash, name, map_name, parent_hash, depth, SUM(size) as size,
          SUM(case when alloc_size > 0 then alloc_size else 0 end)
            as alloc_size, SUM(count) as count,
          SUM(case when alloc_count > 0 then alloc_count else 0 end) as
            alloc_count,
          self_size, self_alloc_size, self_count, self_alloc_count
        from callsite_children
        group by hash`);
    return tableNameGroupedCallsitesForFlamegraph;
  }

  tableName(name: string): string {
    const selection = globals.state.currentHeapProfileFlamegraph;
    if (!selection) return name;
    return `${name}_${selection.upid}_${selection.ts}`;
  }

  getMinSizeDisplayed(flamegraphData: CallsiteInfo[], rootSize?: number):
      number {
    const timeState = globals.state.frontendLocalState.visibleState;
    const width =
        (timeState.endSec - timeState.startSec) / timeState.resolution;
    if (rootSize === undefined) {
      rootSize = findRootSize(flamegraphData);
    }
    return MIN_PIXEL_DISPLAYED * rootSize / width;
  }

  async getHeapProfileMetadata(ts: number, upid: number) {
    // Don't do anything if selection of the marker stayed the same.
    if ((this.lastSelectedHeapProfile !== undefined &&
         ((this.lastSelectedHeapProfile.ts === ts &&
           this.lastSelectedHeapProfile.upid === upid)))) {
      return undefined;
    }

    // Collecting data for more information about heap profile, such as:
    // total memory allocated, memory that is allocated and not freed.
    const pidValue = await this.args.engine.query(
        `select pid from process where upid = ${upid}`);
    const pid = pidValue.columns[0].longValues![0];
    const allocatedMemory = await this.args.engine.query(
        `select sum(size) from heap_profile_allocation where ts <= ${
            ts} and size > 0 and upid = ${upid}`);
    const allocated = allocatedMemory.columns[0].longValues![0];
    const allocatedNotFreedMemory = await this.args.engine.query(
        `select sum(size) from heap_profile_allocation where ts <= ${
            ts} and upid = ${upid}`);
    const allocatedNotFreed = allocatedNotFreedMemory.columns[0].longValues![0];
    const startTime = fromNs(ts) - globals.state.traceTime.startSec;
    return {ts: startTime, allocated, allocatedNotFreed, tsNs: ts, pid, upid};
  }
}
