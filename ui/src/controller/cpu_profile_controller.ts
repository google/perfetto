// Copyright (C) 2020 The Android Open Source Project
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
import {slowlyCountRows} from '../common/query_iterator';
import {CallsiteInfo, CpuProfileSampleSelection} from '../common/state';
import {CpuProfileDetails} from '../frontend/globals';

import {Controller} from './controller';
import {globals} from './globals';

export interface CpuProfileControllerArgs {
  engine: Engine;
}

export class CpuProfileController extends Controller<'main'> {
  private lastSelectedSample?: CpuProfileSampleSelection;
  private requestingData = false;
  private queuedRunRequest = false;

  constructor(private args: CpuProfileControllerArgs) {
    super('main');
  }

  run() {
    const selection = globals.state.currentSelection;
    if (!selection || selection.kind !== 'CPU_PROFILE_SAMPLE') {
      return;
    }

    const selectedSample = selection as CpuProfileSampleSelection;
    if (!this.shouldRequestData(selectedSample)) {
      return;
    }

    if (this.requestingData) {
      this.queuedRunRequest = true;
      return;
    }

    this.requestingData = true;
    globals.publish('CpuProfileDetails', {});
    this.lastSelectedSample = this.copyCpuProfileSample(selection);

    this.getSampleData(selectedSample.id)
        .then(sampleData => {
          if (sampleData !== undefined && selectedSample &&
              this.lastSelectedSample &&
              this.lastSelectedSample.id === selectedSample.id) {
            const cpuProfileDetails: CpuProfileDetails = {
              id: selectedSample.id,
              ts: selectedSample.ts,
              utid: selectedSample.utid,
              stack: sampleData,
            };

            globals.publish('CpuProfileDetails', cpuProfileDetails);
          }
        })
        .finally(() => {
          this.requestingData = false;
          if (this.queuedRunRequest) {
            this.queuedRunRequest = false;
            this.run();
          }
        });
  }

  private copyCpuProfileSample(cpuProfileSample: CpuProfileSampleSelection):
      CpuProfileSampleSelection {
    return {
      kind: cpuProfileSample.kind,
      id: cpuProfileSample.id,
      utid: cpuProfileSample.utid,
      ts: cpuProfileSample.ts,
    };
  }

  private shouldRequestData(selection: CpuProfileSampleSelection) {
    return this.lastSelectedSample === undefined ||
        (this.lastSelectedSample !== undefined &&
         (this.lastSelectedSample.id !== selection.id));
  }

  async getSampleData(id: number) {
    // The goal of the query is to get all the frames of
    // the callstack at the callsite given by |id|. To do this, it does
    // the following:
    // 1. Gets the leaf callsite id for the sample given by |id|.
    // 2. For this callsite, get all the frame ids and depths
    //    for the frame and all ancestors in the callstack.
    // 3. For each frame, get the mapping name (i.e. library which
    //    contains the frame).
    // 4. Symbolize each frame using the symbol table if possible.
    // 5. Sort the query by the depth of the callstack frames.
    const sampleQuery = `
      SELECT
        samples.id,
        IFNULL(
          (
            SELECT name
            FROM stack_profile_symbol symbol
            WHERE symbol.symbol_set_id = spf.symbol_set_id
            LIMIT 1
          ),
          spf.name
        ) AS frame_name,
        spm.name AS mapping_name
      FROM cpu_profile_stack_sample AS samples
      LEFT JOIN (
        SELECT
          id,
          frame_id,
          depth
        FROM stack_profile_callsite
        UNION ALL
        SELECT
          leaf.id AS id,
          callsite.frame_id AS frame_id,
          callsite.depth AS depth
        FROM stack_profile_callsite leaf
        JOIN experimental_ancestor_stack_profile_callsite(leaf.id) AS callsite
      ) AS callsites
        ON samples.callsite_id = callsites.id
      LEFT JOIN stack_profile_frame AS spf
        ON callsites.frame_id = spf.id
      LEFT JOIN stack_profile_mapping AS spm
        ON spf.mapping = spm.id
      WHERE samples.id = ${id}
      ORDER BY callsites.depth;
    `;

    const callsites = await this.args.engine.query(sampleQuery);

    if (slowlyCountRows(callsites) < 1) {
      return undefined;
    }

    const sampleData: CallsiteInfo[] = new Array();
    for (let i = 0; i < slowlyCountRows(callsites); i++) {
      const id = +callsites.columns[0].longValues![i];
      const name = callsites.columns[1].stringValues![i];
      const mapping = callsites.columns[2].stringValues![i];

      sampleData.push({
        id,
        totalSize: 0,
        depth: 0,
        parentId: 0,
        name,
        selfSize: 0,
        mapping,
        merged: false,
        highlighted: false
      });
    }

    return sampleData;
  }
}
