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
import {TrackEventDetailsPanel} from '../../public/details_panel';
import {TrackEventSelection} from '../../public/selection';
import {Trace} from '../../public/trace';
import {asSliceSqlId, SliceSqlId} from '../../components/sql_utils/core_types';
import {getSlice, SliceDetails} from '../../components/sql_utils/slice';
import {renderDetails} from '../../components/details/slice_details';
import {Engine} from '../../trace_processor/engine';
import {STR_NULL} from '../../trace_processor/query_result';
import {Tree, TreeNode} from '../../widgets/tree';
import {DetailsShell} from '../../widgets/details_shell';
import {Section} from '../../widgets/section';
import {TrackEventRef} from '../../components/widgets/track_event_ref';

interface BinderTxnDetails {
  // Whether this is the client or server side of the binder transaction.
  txnRole: string;
  interfaceName?: string;
  methodName?: string;
}

async function getBinderTxnDetails(
  engine: Engine,
  id: SliceSqlId,
): Promise<BinderTxnDetails | undefined> {
  const queryResult = await engine.query(`
    SELECT
      IF(binder_txn_id = ${id}, 'Client', IF(binder_reply_id = ${id}, 'Server', '')) as txnRole,
      interface as interfaceName,
      method_name as methodName
    FROM android_binder_txns
    WHERE binder_txn_id = ${id} OR binder_reply_id = ${id} -- slice id is either client or server slice
  `);

  const it = queryResult.iter({
    txnRole: STR_NULL,
    interfaceName: STR_NULL,
    methodName: STR_NULL,
  });

  if (it.valid()) {
    return {
      txnRole: it.txnRole || 'Unknown',
      interfaceName: it.interfaceName ?? undefined,
      methodName: it.methodName ?? undefined,
    };
  }
  return undefined;
}

export class BinderSliceDetailsPanel implements TrackEventDetailsPanel {
  private sliceDetails: SliceDetails | undefined;
  private binderTxnDetails: BinderTxnDetails | undefined;
  private isLoading = true;

  constructor(private readonly trace: Trace) {}

  async load(selection: TrackEventSelection): Promise<void> {
    const sliceId = asSliceSqlId(selection.eventId);

    this.isLoading = true;
    this.sliceDetails = await getSlice(this.trace.engine, sliceId);
    this.binderTxnDetails = await getBinderTxnDetails(
      this.trace.engine,
      sliceId,
    );
    this.isLoading = false;
  }

  render() {
    if (this.isLoading) {
      return m(DetailsShell, {
        title: 'Binder Transaction',
        description: 'Loading...',
      });
    }

    if (!this.sliceDetails) {
      return m(DetailsShell, {
        title: 'Binder Transaction',
        description: 'Slice not found',
      });
    }

    const txnRole = this.binderTxnDetails?.txnRole;
    const sliceId = this.sliceDetails?.id;
    const name = this.sliceDetails?.name;

    return m(
      DetailsShell,
      {title: `${txnRole} ${name}`},
      this.binderTxnDetails &&
        m(
          Section,
          {title: 'Binder'},
          m(
            Tree,
            m(TreeNode, {
              left: 'Interface',
              right: this.binderTxnDetails.interfaceName,
            }),
            m(TreeNode, {
              left: 'Method',
              right: this.binderTxnDetails.methodName,
            }),
            m(TreeNode, {
              left: txnRole + ' slice',
              right: m(TrackEventRef, {
                trace: this.trace,
                table: 'slice',
                id: sliceId,
                name: `slice[${sliceId}]`,
              }),
            }),
          ),
        ),
      renderDetails(this.trace, this.sliceDetails),
    );
  }
}
