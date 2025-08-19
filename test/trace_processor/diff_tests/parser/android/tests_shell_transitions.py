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


class ShellTransitions(TestSuite):

  def test_has_expected_transition_rows(self):
    return DiffTestBlueprint(
        trace=Path('shell_transitions.textproto'),
        query="""
        SELECT
          id,
          ts,
          transition_id,
          transition_type,
          send_time_ns,
          dispatch_time_ns,
          duration_ns,
          finish_time_ns,
          shell_abort_time_ns,
          handler,
          status,
          flags,
          start_transaction_id,
          finish_transaction_id
        FROM
          window_manager_shell_transitions
        ORDER BY id;
        """,
        out=Csv("""
        "id","ts","transition_id","transition_type","send_time_ns","dispatch_time_ns","duration_ns","finish_time_ns","shell_abort_time_ns","handler","status","flags","start_transaction_id","finish_transaction_id"
        0,76879063147,7,"[NULL]",76875395422,76879063147,"[NULL]","[NULL]","[NULL]",2,"[NULL]","[NULL]",5604932321952,5604932321954
        1,77899001013,10,"[NULL]",77894307328,77899001013,727303101,78621610429,"[NULL]",4,"played","[NULL]",5604932322158,5604932322159
        2,82536817137,11,"[NULL]",82535513345,82536817137,"[NULL]","[NULL]",82536817537,2,"aborted","[NULL]",5604932322346,5604932322347
        3,77320527177,8,"[NULL]",77277756832,77320527177,"[NULL]","[NULL]","[NULL]",3,"merged","[NULL]",5604932322028,5604932322029
        4,77876414832,9,"[NULL]",77843436723,77876414832,30498739,77873935462,"[NULL]",3,"played","[NULL]",5604932322137,5604932322138
        5,0,12,1,"[NULL]","[NULL]","[NULL]","[NULL]","[NULL]","[NULL]","merged","[NULL]","[NULL]","[NULL]"
        """))

  def test_has_expected_transition_args(self):
    return DiffTestBlueprint(
        trace=Path('shell_transitions_simple_merge.textproto'),
        query="""
        SELECT
          args.key, args.display_value
        FROM
          window_manager_shell_transitions JOIN args ON window_manager_shell_transitions.arg_set_id = args.arg_set_id
        WHERE window_manager_shell_transitions.transition_id = 15
        ORDER BY args.key;
        """,
        out=Csv("""
        "key","display_value"
        "create_time_ns","2187614568227"
        "dispatch_time_ns","2187673373973"
        "finish_time_ns","2188195968659"
        "finish_transaction_id","5738076308938"
        "flags","0"
        "handler","2"
        "id","15"
        "send_time_ns","2187671767120"
        "start_transaction_id","5738076308937"
        "starting_window_remove_time_ns","2188652838898"
        "targets[0].flags","0"
        "targets[0].layer_id","244"
        "targets[0].mode","1"
        "targets[0].window_id","219481253"
        "targets[1].flags","1"
        "targets[1].layer_id","47"
        "targets[1].mode","4"
        "targets[1].window_id","54474511"
        "type","1"
        """))

  def test_shell_transitions_table_has_raw_protos(self):
    return DiffTestBlueprint(
        trace=Path('shell_transitions.textproto'),
        query="""
        SELECT COUNT(*) FROM __intrinsic_window_manager_shell_transition_protos
        WHERE transition_id IS NOT NULL AND base64_proto_id IS NOT NULL
        """,
        out=Csv("""
        "COUNT(*)"
        15
        """))

  def test_has_shell_handlers(self):
    return DiffTestBlueprint(
        trace=Path('shell_handlers.textproto'),
        query="""
      SELECT
        handler_id, handler_name
      FROM
        window_manager_shell_transition_handlers;
      """,
        out=Csv("""
      "handler_id","handler_name"
      1,"DefaultTransitionHandler"
      2,"RecentsTransitionHandler"
      3,"FreeformTaskTransitionHandler"
      """))

  def test_shell_handlers_table_has_raw_protos(self):
    return DiffTestBlueprint(
        trace=Path('shell_handlers.textproto'),
        query="""
        SELECT COUNT(*) FROM window_manager_shell_transition_handlers
        WHERE base64_proto_id IS NOT NULL
        """,
        out=Csv("""
        "COUNT(*)"
        3
        """))

  def test_handles_weird_args_table_issue(self):
    return DiffTestBlueprint(
        trace=Path('args_table_issue.textproto'),
        query="""
        SELECT
          args.key, args.display_value
        FROM
          window_manager_shell_transitions JOIN args ON window_manager_shell_transitions.arg_set_id = args.arg_set_id
        ORDER BY args.key;
        """,
        out=Csv("""
        "key","display_value"
        "handler","2"
        "id","729"
        "targets[0].flags","1048577"
        """))

  def test_participants(self):
    return DiffTestBlueprint(
        trace=Path('shell_transitions.textproto'),
        query="""
        INCLUDE PERFETTO MODULE android.winscope.transitions;
        SELECT
          transition_id, layer_id, window_id
        FROM
          android_window_manager_shell_transition_participants
        ORDER BY transition_id;
        """,
        out=Csv("""
        "transition_id","layer_id","window_id"
        11,"[NULL]",11
        11,4,"[NULL]"
        12,4,12
        """))