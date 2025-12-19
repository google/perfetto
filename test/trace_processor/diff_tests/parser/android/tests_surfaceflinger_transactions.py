#!/usr/bin/env python3
# Copyright (C) 2023 The Android Open Source Project
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

from python.generators.diff_tests.testing import Path
from python.generators.diff_tests.testing import Csv
from python.generators.diff_tests.testing import DiffTestBlueprint
from python.generators.diff_tests.testing import TestSuite


class SurfaceFlingerTransactions(TestSuite):

  def test_has_expected_transactions_rows(self):
    return DiffTestBlueprint(
        trace=Path('surfaceflinger_transactions.textproto'),
        query="""
        SELECT
          id, ts
        FROM
          surfaceflinger_transactions;
        """,
        out=Csv("""
        "id","ts"
        0,2749532892211
        1,2749555538126
        2,2749578184041
        """))

  def test_has_expected_transactions_args(self):
    return DiffTestBlueprint(
        trace=Path('surfaceflinger_transactions.textproto'),
        query="""
        SELECT
          args.key, args.display_value
        FROM
          surfaceflinger_transactions AS sft JOIN args ON sft.arg_set_id = args.arg_set_id
        WHERE sft.id = 0
        ORDER BY args.key;
        """,
        out=Csv("""
        "key","display_value"
        "displays_changed","false"
        "elapsed_realtime_nanos","2749532892211"
        "transactions[0].input_event_id","1223067573"
        "transactions[0].layer_changes[0].auto_refresh","false"
        "transactions[0].layer_changes[0].buffer_crop.bottom","0"
        "transactions[0].layer_changes[0].buffer_crop.left","0"
        "transactions[0].layer_changes[0].buffer_crop.right","0"
        "transactions[0].layer_changes[0].buffer_crop.top","0"
        "transactions[0].layer_changes[0].buffer_data.buffer_id","10518374907909"
        "transactions[0].layer_changes[0].buffer_data.cached_buffer_id","10518374907909"
        "transactions[0].layer_changes[0].buffer_data.flags","7"
        "transactions[0].layer_changes[0].buffer_data.frame_number","293"
        "transactions[0].layer_changes[0].buffer_data.height","2400"
        "transactions[0].layer_changes[0].buffer_data.pixel_format","PIXEL_FORMAT_RGBA_8888"
        "transactions[0].layer_changes[0].buffer_data.usage","2816"
        "transactions[0].layer_changes[0].buffer_data.width","1080"
        "transactions[0].layer_changes[0].destination_frame.bottom","2400"
        "transactions[0].layer_changes[0].destination_frame.left","0"
        "transactions[0].layer_changes[0].destination_frame.right","1080"
        "transactions[0].layer_changes[0].destination_frame.top","0"
        "transactions[0].layer_changes[0].layer_id","100"
        "transactions[0].layer_changes[0].transform","0"
        "transactions[0].layer_changes[0].transform_to_display_inverse","false"
        "transactions[0].layer_changes[0].what","17631439233024"
        "transactions[0].pid","0"
        "transactions[0].post_time","2749525432616"
        "transactions[0].transaction_id","10518374908656"
        "transactions[0].uid","10238"
        "transactions[0].vsync_id","24769"
        "vsync_id","24776"
        """))

  def test_table_has_raw_protos(self):
    return DiffTestBlueprint(
        trace=Path('surfaceflinger_transactions.textproto'),
        query="""
        SELECT COUNT(*) FROM surfaceflinger_transactions
        WHERE base64_proto_id IS NOT NULL
        """,
        out=Csv("""
        "COUNT(*)"
        3
        """))

  def test_surfaceflinger_transaction_rows(self):
    return DiffTestBlueprint(
        trace=Path('surfaceflinger_transactions.textproto'),
        query="""
        INCLUDE PERFETTO MODULE android.winscope.surfaceflinger;
        SELECT COUNT(*) FROM android_surfaceflinger_transaction
        """,
        out=Csv("""
        "COUNT(*)"
        11
        """))

  def test_surfaceflinger_transaction_layer_changes(self):
    return DiffTestBlueprint(
        trace=Path('surfaceflinger_transactions.textproto'),
        query="""
        INCLUDE PERFETTO MODULE android.winscope.surfaceflinger;
        SELECT
          snapshot_id,
          transaction_id,
          pid,
          uid,
          layer_id,
          display_id,
          flags_id
        FROM
          android_surfaceflinger_transaction
        WHERE transaction_type = 'LAYER_CHANGED';
        """,
        out=Csv("""
        "snapshot_id","transaction_id","pid","uid","layer_id","display_id","flags_id"
        0,10518374908656,0,10238,100,"[NULL]",0
        1,10518374908658,0,10238,100,"[NULL]",0
        """))

  def test_surfaceflinger_transaction_layer_change_args(self):
    return DiffTestBlueprint(
        trace=Path('surfaceflinger_transactions.textproto'),
        query="""
        INCLUDE PERFETTO MODULE android.winscope.surfaceflinger;
        SELECT
          args.key, args.display_value
        FROM
          android_surfaceflinger_transaction AS sft JOIN args ON sft.arg_set_id = args.arg_set_id
        WHERE sft.transaction_type = 'LAYER_CHANGED' AND sft.snapshot_id = 1
        ORDER BY args.key;
        """,
        out=Csv("""
        "key","display_value"
        "apply_token","987654321"
        "auto_refresh","false"
        "buffer_crop.bottom","0"
        "buffer_crop.left","0"
        "buffer_crop.right","0"
        "buffer_crop.top","0"
        "buffer_data.buffer_id","10518374907908"
        "buffer_data.cached_buffer_id","10518374907908"
        "buffer_data.flags","7"
        "buffer_data.frame_number","294"
        "buffer_data.height","2400"
        "buffer_data.pixel_format","PIXEL_FORMAT_RGBA_8888"
        "buffer_data.usage","2816"
        "buffer_data.width","1080"
        "destination_frame.bottom","2400"
        "destination_frame.left","0"
        "destination_frame.right","1080"
        "destination_frame.top","0"
        "layer_id","100"
        "transaction_barriers[0].barrier_token","12345"
        "transaction_barriers[0].kind","432"
        "transaction_barriers[1].barrier_token","67890"
        "transaction_barriers[1].kind","987"
        "transform","0"
        "transform_to_display_inverse","false"
        "what","17631439233024"
        """))

  def test_surfaceflinger_transaction_display_changes(self):
    return DiffTestBlueprint(
        trace=Path('surfaceflinger_transactions.textproto'),
        query="""
        INCLUDE PERFETTO MODULE android.winscope.surfaceflinger;
        SELECT
          snapshot_id,
          arg_set_id IS NOT NULL AS has_arg_set_id,
          transaction_id,
          pid,
          uid,
          layer_id,
          display_id,
          flags_id
        FROM
          android_surfaceflinger_transaction
        WHERE transaction_type = 'DISPLAY_CHANGED';
        """,
        out=Csv("""
        "snapshot_id","has_arg_set_id","transaction_id","pid","uid","layer_id","display_id","flags_id"
        2,1,10518374908660,3,415,"[NULL]",1234,1
        """))

  def test_surfaceflinger_transaction_display_change_args(self):
    return DiffTestBlueprint(
        trace=Path('surfaceflinger_transactions.textproto'),
        query="""
        INCLUDE PERFETTO MODULE android.winscope.surfaceflinger;
        SELECT
          args.key, args.display_value
        FROM
          android_surfaceflinger_transaction AS sft JOIN args ON sft.arg_set_id = args.arg_set_id
        WHERE sft.transaction_type = 'DISPLAY_CHANGED' AND sft.snapshot_id = 2
        ORDER BY args.key;
        """,
        out=Csv("""
        "key","display_value"
        "apply_token","123456789"
        "flags","8"
        "id","1234"
        "transaction_barriers[0].barrier_token","54321"
        "transaction_barriers[0].kind","234"
        "transaction_barriers[1].barrier_token","9876"
        "transaction_barriers[1].kind","789"
        "what","22"
        """))

  def test_surfaceflinger_transaction_noop(self):
    return DiffTestBlueprint(
        trace=Path('surfaceflinger_transactions.textproto'),
        query="""
        INCLUDE PERFETTO MODULE android.winscope.surfaceflinger;
        SELECT
          snapshot_id,
          arg_set_id,
          transaction_id,
          pid,
          uid,
          layer_id,
          display_id,
          flags_id
        FROM
          android_surfaceflinger_transaction
        WHERE transaction_type = 'NOOP';
        """,
        out=Csv("""
        "snapshot_id","arg_set_id","transaction_id","pid","uid","layer_id","display_id","flags_id"
        2,"[NULL]",10518374908661,3,415,"[NULL]","[NULL]","[NULL]"
        """))

  def test_surfaceflinger_transaction_added_layers(self):
    return DiffTestBlueprint(
        trace=Path('surfaceflinger_transactions.textproto'),
        query="""
        INCLUDE PERFETTO MODULE android.winscope.surfaceflinger;
        SELECT
          snapshot_id,
          arg_set_id IS NOT NULL AS has_arg_set_id,
          transaction_id,
          pid,
          uid,
          layer_id,
          display_id,
          flags_id
        FROM
          android_surfaceflinger_transaction
        WHERE transaction_type = 'LAYER_ADDED';
        """,
        out=Csv("""
        "snapshot_id","has_arg_set_id","transaction_id","pid","uid","layer_id","display_id","flags_id"
        2,1,"[NULL]","[NULL]","[NULL]",4,"[NULL]","[NULL]"
        """))

  def test_surfaceflinger_transaction_added_layer_args(self):
    return DiffTestBlueprint(
        trace=Path('surfaceflinger_transactions.textproto'),
        query="""
        INCLUDE PERFETTO MODULE android.winscope.surfaceflinger;
        SELECT
          args.key, args.display_value
        FROM
          android_surfaceflinger_transaction AS sft JOIN args ON sft.arg_set_id = args.arg_set_id
        WHERE sft.transaction_type = 'LAYER_ADDED' AND sft.snapshot_id = 2
        ORDER BY args.key;
        """,
        out=Csv("""
        "key","display_value"
        "layer_id","4"
        """))

  def test_surfaceflinger_transaction_destroyed_layers(self):
    return DiffTestBlueprint(
        trace=Path('surfaceflinger_transactions.textproto'),
        query="""
        INCLUDE PERFETTO MODULE android.winscope.surfaceflinger;
        SELECT
          snapshot_id,
          arg_set_id,
          transaction_id,
          pid,
          uid,
          layer_id,
          display_id,
          flags_id
        FROM
          android_surfaceflinger_transaction
        WHERE transaction_type = 'LAYER_DESTROYED';
        """,
        out=Csv("""
        "snapshot_id","arg_set_id","transaction_id","pid","uid","layer_id","display_id","flags_id"
        2,"[NULL]","[NULL]","[NULL]","[NULL]",5,"[NULL]","[NULL]"
        2,"[NULL]","[NULL]","[NULL]","[NULL]",6,"[NULL]","[NULL]"
        """))

  def test_surfaceflinger_transaction_added_displays(self):
    return DiffTestBlueprint(
        trace=Path('surfaceflinger_transactions.textproto'),
        query="""
        INCLUDE PERFETTO MODULE android.winscope.surfaceflinger;
        SELECT
          snapshot_id,
          arg_set_id IS NOT NULL AS has_arg_set_id,
          transaction_id,
          pid,
          uid,
          layer_id,
          display_id,
          flags_id
        FROM
          android_surfaceflinger_transaction
        WHERE transaction_type = 'DISPLAY_ADDED';
        """,
        out=Csv("""
        "snapshot_id","has_arg_set_id","transaction_id","pid","uid","layer_id","display_id","flags_id"
        2,1,"[NULL]","[NULL]","[NULL]","[NULL]",5678,2
        """))

  def test_surfaceflinger_transaction_added_layer_args(self):
    return DiffTestBlueprint(
        trace=Path('surfaceflinger_transactions.textproto'),
        query="""
        INCLUDE PERFETTO MODULE android.winscope.surfaceflinger;
        SELECT
          args.key, args.display_value
        FROM
          android_surfaceflinger_transaction AS sft JOIN args ON sft.arg_set_id = args.arg_set_id
        WHERE sft.transaction_type = 'DISPLAY_ADDED' AND sft.snapshot_id = 2
        ORDER BY args.key;
        """,
        out=Csv("""
        "key","display_value"
        "flags","22"
        "id","5678"
        "what","8"
        """))

  def test_surfaceflinger_transaction_removed_displays(self):
    return DiffTestBlueprint(
        trace=Path('surfaceflinger_transactions.textproto'),
        query="""
        INCLUDE PERFETTO MODULE android.winscope.surfaceflinger;
        SELECT
          snapshot_id,
          arg_set_id,
          transaction_id,
          pid,
          uid,
          layer_id,
          display_id,
          flags_id
        FROM
          android_surfaceflinger_transaction
        WHERE transaction_type = 'DISPLAY_REMOVED';
        """,
        out=Csv("""
        "snapshot_id","arg_set_id","transaction_id","pid","uid","layer_id","display_id","flags_id"
        2,"[NULL]","[NULL]","[NULL]","[NULL]","[NULL]",5,"[NULL]"
        2,"[NULL]","[NULL]","[NULL]","[NULL]","[NULL]",7,"[NULL]"
        """))

  def test_surfaceflinger_transaction_destroyed_layer_handles(self):
    return DiffTestBlueprint(
        trace=Path('surfaceflinger_transactions.textproto'),
        query="""
        INCLUDE PERFETTO MODULE android.winscope.surfaceflinger;
        SELECT
          snapshot_id,
          arg_set_id,
          transaction_id,
          pid,
          uid,
          layer_id,
          display_id,
          flags_id
        FROM
          android_surfaceflinger_transaction
        WHERE transaction_type = 'LAYER_HANDLE_DESTROYED';
        """,
        out=Csv("""
        "snapshot_id","arg_set_id","transaction_id","pid","uid","layer_id","display_id","flags_id"
        2,"[NULL]","[NULL]","[NULL]","[NULL]",9,"[NULL]","[NULL]"
        """))

  def test_surfaceflinger_transaction_flags(self):
    return DiffTestBlueprint(
        trace=Path('surfaceflinger_transactions.textproto'),
        query="""
        INCLUDE PERFETTO MODULE android.winscope.surfaceflinger;
        SELECT flags_id, flag FROM android_surfaceflinger_transaction_flag
        """,
        out=Csv("""
        "flags_id","flag"
        0,"eBufferCropChanged"
        0,"eBufferTransformChanged"
        0,"eTransformToDisplayInverseChanged"
        0,"eBufferChanged"
        0,"eDataspaceChanged"
        0,"eHdrMetadataChanged"
        0,"eSurfaceDamageRegionChanged"
        0,"eHasListenerCallbacksChanged"
        0,"eDestinationFrameChanged"
        0,"eMetadataChanged"
        0,"eAutoRefreshChanged"
        1,"eLayerStackChanged"
        1,"eDisplayProjectionChanged"
        1,"eFlagsChanged"
        2,"eDisplaySizeChanged"
        """))
