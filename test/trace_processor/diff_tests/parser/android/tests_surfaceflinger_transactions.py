#!/usr/bin/env python3
# Copyright (C) 2023 The Android Open Source Project
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License a
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
