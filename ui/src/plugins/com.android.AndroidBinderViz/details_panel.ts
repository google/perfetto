
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
import {TrackEventDetailsPanel} from '../../public/details_panel';
import {Trace} from '../../public/trace';
import {asSliceSqlId, SliceSqlId} from '../../components/sql_utils/core_types';
import {SliceRef} from '../../components/widgets/slice';
import {getSlice, SliceDetails} from '../../components/sql_utils/slice';
import {renderDetails} from '../../components/details/slice_details';
import {Engine} from '../../trace_processor/engine';
import {NUM, STR_NULL} from '../../trace_processor/query_result';
import {Tree, TreeNode} from '../../widgets/tree';
import {Section} from '../../widgets/section';

// Define an interface for the binder transaction details
interface BinderTxnDetails {
  sliceType: string;
  clientSliceId: SliceSqlId;
  serverSliceId?: SliceSqlId;
  clientProcess?: string;
  serverProcess?: string;
  interfaceName?: string;
  methodName?: string;
}

async function getBinderTxnDetails(
  engine: Engine,
  id: SliceSqlId,
): Promise<BinderTxnDetails|undefined> {
  const queryResult = await engine.query(`
    SELECT
      IF(binder_txn_id = ${id}, 'Client', IF(binder_reply_id = ${id}, 'Server', '')) as sliceType,
      binder_txn_id as clientSliceId,
      binder_reply_id as serverSliceId,
      client_process as clientProcess,
      server_process as serverProcess,
      interface as interfaceName,
      method_name as methodName
    FROM android_binder_txns
    WHERE binder_txn_id = ${id} OR binder_reply_id = ${id} -- slice id is either client or server slice
  `);

  const it = queryResult.iter({
    sliceType: STR_NULL,
    clientSliceId: NUM,
    serverSliceId: NUM,
    clientProcess: STR_NULL,
    serverProcess: STR_NULL,
    interfaceName: STR_NULL,
    methodName: STR_NULL,
  });

  if (it.valid()) {
    return {
      sliceType: it.sliceType || 'Unknown',
      clientSliceId: asSliceSqlId(it.clientSliceId),
      serverSliceId: asSliceSqlId(it.serverSliceId),
      clientProcess: it.clientProcess ?? undefined,
      serverProcess: it.serverProcess ?? undefined,
      interfaceName: it.interfaceName ?? undefined,
      methodName: it.methodName ?? undefined,
    };
  }
  return undefined;
}

export class BinderTransactionDetailsPanel implements TrackEventDetailsPanel {
  private sliceDetails: SliceDetails|undefined;
  private binderTxnDetails: BinderTxnDetails|undefined;
  private isLoading = true;

  constructor(
    private readonly trace: Trace,
    private readonly id: bigint,
  ) {
    const sliceId = asSliceSqlId(Number(this.id));
    Promise
      .all([
        getSlice(this.trace.engine, sliceId),
        getBinderTxnDetails(this.trace.engine, sliceId),
      ])
      .then(([slice, binderTxn]) => {
        this.sliceDetails = slice;
        this.binderTxnDetails = binderTxn;
      })
      .catch((error) => {
        console.error('Error getting binder transaction details:', error);
      })
      .finally(() => {
        this.isLoading = false;
        m.redraw();
      });
  }

  render() {
    if (this.isLoading) {
      return m('.details-panel', m('h2', 'Loading...'));
    }

    if (!this.sliceDetails) {
      return m('.details-panel', m('h2', 'Slice not found'));
    }

    const sliceType = this.binderTxnDetails?.sliceType;
    const clientSliceId = this.binderTxnDetails?.clientSliceId;
    const serverSliceId = this.binderTxnDetails?.serverSliceId;
    const title = this.sliceDetails?.name;

    return m(
      '.details-panel',
      m(Section, {title: sliceType + " " + title},
        this.binderTxnDetails &&
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
            sliceType === 'Client' && m(TreeNode, {
              left: 'Client Process',
              right: this.binderTxnDetails.clientProcess,
            }),
            sliceType === 'Client' && clientSliceId && m(TreeNode, {
              left: 'Client slice',
              right: m(SliceRef, {
                trace: this.trace,
                id: clientSliceId,
                name: `slice[${clientSliceId}]`,
              }),
            }),
            sliceType === 'Server' && m(TreeNode, {
              left: 'Server Process',
              right: this.binderTxnDetails.serverProcess,
            }),
            sliceType === 'Server' && serverSliceId && m(TreeNode, {
              left: 'Server slice',
              right: m(SliceRef, {
                trace: this.trace,
                id: serverSliceId,
                name: `slice[${serverSliceId}]`,
              }),
            }),
          ),
        ),
      renderDetails(this.trace, this.sliceDetails),
    );
  }
}
