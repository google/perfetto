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


class ShellTransitions(TestSuite):

  def test_has_expected_transition_rows(self):
    return DiffTestBlueprint(
        trace=Path('shell_transitions.textproto'),
        query="""
        SELECT
          id, transition_id
        FROM
          window_manager_shell_transitions;
        """,
        out=Csv("""
        "id","transition_id"
        0,7
        1,10
        2,11
        3,8
        4,9
        5,12
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
        SELECT COUNT(*) FROM window_manager_shell_transitions
        WHERE base64_proto IS NOT NULL AND base64_proto_id IS NOT NULL
        """,
        out=Csv("""
        "COUNT(*)"
        6
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
        WHERE base64_proto IS NOT NULL AND base64_proto_id IS NOT NULL
        """,
        out=Csv("""
        "COUNT(*)"
        3
        """))
