#!/usr/bin/env python3
# Copyright (C) 2026 The Android Open Source Project
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

from python.generators.diff_tests.testing import Csv, TextProto
from python.generators.diff_tests.testing import DiffTestBlueprint
from python.generators.diff_tests.testing import TestSuite


class AndroidUiHierarchy(TestSuite):

  def test_stdlib_nodes_at(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet {
          trusted_packet_sequence_id: 1
          timestamp: 1000
          trusted_pid: 100
          sequence_flags: 1
          interned_data {
            ui_strings { iid: 1 str: "MainWindow" }
            ui_strings { iid: 2 str: "RootView" }
            ui_strings { iid: 3 str: "Button" }
            ui_strings { iid: 4 str: "Submit" }
          }
          ui_hierarchy {
            is_keyframe: true
            windows {
              id: 1
              title_iid: 1
              nodes {
                id: 1
                kind: KIND_VIEW
                name_iid: 2
                x: 0
                y: 0
                width: 1080
                height: 1920
                flags: 1
              }
              nodes {
                id: 2
                parent_id: 1
                index: 0
                kind: KIND_COMPOSE_NODE
                name_iid: 3
                text_iid: 4
                role_iid: 3
                x: 100
                y: 200
                width: 300
                height: 100
                flags: 7
              }
            }
          }
        }
        """),
        query="""
        INCLUDE PERFETTO MODULE android.ui_hierarchy;
        SELECT
          node_id, parent_id, kind, name, text, role, bounds_left, bounds_top, bounds_right, bounds_bottom
        FROM android_ui_hierarchy_nodes_at(1000)
        ORDER BY node_id;
        """,
        out=Csv("""
        "node_id","parent_id","kind","name","text","role","bounds_left","bounds_top","bounds_right","bounds_bottom"
        1,"[NULL]","view","RootView","[NULL]","[NULL]",0,0,1080,1920
        2,1,"compose_node","Button","Submit","Button",100,200,400,300
        """))

  def test_stdlib_screen_text(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet {
          trusted_packet_sequence_id: 1
          timestamp: 1000
          trusted_pid: 100
          sequence_flags: 1
          interned_data {
            ui_strings { iid: 1 str: "MainWindow" }
            ui_strings { iid: 2 str: "TitleText" }
            ui_strings { iid: 3 str: "Welcome" }
            ui_strings { iid: 4 str: "title_tag" }
            ui_strings { iid: 5 str: "ActionBtn" }
            ui_strings { iid: 6 str: "Login" }
            ui_strings { iid: 7 str: "Button" }
          }
          ui_hierarchy {
            is_keyframe: true
            windows {
              id: 1
              title_iid: 1
              nodes {
                id: 1
                name_iid: 2
                text_iid: 3
                test_tag_iid: 4
                x: 50
                y: 50
                width: 450
                height: 50
                flags: 1
              }
              nodes {
                id: 2
                name_iid: 5
                text_iid: 6
                role_iid: 7
                x: 50
                y: 150
                width: 250
                height: 100
                flags: 5  # visible | clickable
              }
            }
          }
        }
        """),
        query="""
        INCLUDE PERFETTO MODULE android.ui_hierarchy;
        SELECT
          role, label, test_tag, bounds_left, bounds_top, is_clickable
        FROM android_ui_hierarchy_screen_text(1000);
        """,
        out=Csv("""
        "role","label","test_tag","bounds_left","bounds_top","is_clickable"
        "TitleText","Welcome","title_tag",50,50,0
        "Button","Login","[NULL]",50,150,1
        """))

  def test_stdlib_node_at_point(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet {
          trusted_packet_sequence_id: 1
          timestamp: 1000
          trusted_pid: 100
          sequence_flags: 1
          interned_data {
            ui_strings { iid: 1 str: "MainWindow" }
            ui_strings { iid: 2 str: "Container" }
            ui_strings { iid: 3 str: "NestedButton" }
          }
          ui_hierarchy {
            is_keyframe: true
            windows {
              id: 1
              title_iid: 1
              nodes {
                id: 1
                name_iid: 2
                x: 0
                y: 0
                width: 1000
                height: 1000
                flags: 1
              }
              nodes {
                id: 2
                parent_id: 1
                name_iid: 3
                x: 100
                y: 100
                width: 200
                height: 100
                flags: 5
              }
            }
          }
        }
        """),
        query="""
        INCLUDE PERFETTO MODULE android.ui_hierarchy;
        SELECT
          node_id, name
        FROM android_ui_hierarchy_node_at_point(1000, 150, 150);
        """,
        out=Csv("""
        "node_id","name"
        2,"NestedButton"
        """))

  def test_stdlib_node_basis_and_lookahead(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet {
          trusted_packet_sequence_id: 1
          timestamp: 1000
          trusted_pid: 100
          sequence_flags: 1
          interned_data {
            ui_strings { iid: 1 str: "MainWindow" }
            ui_strings { iid: 2 str: "LookaheadNode" }
          }
          ui_hierarchy {
            is_keyframe: true
            windows {
              id: 1
              title_iid: 1
              nodes {
                id: 10
                name_iid: 2
                x: 12.5
                y: 34.5
                width: 200
                height: 100
                lookahead_x: 10
                lookahead_y: 30
                lookahead_width: 220
                lookahead_height: 110
              }
            }
          }
        }
        """),
        query="""
        INCLUDE PERFETTO MODULE android.ui_hierarchy;
        SELECT
          node_id, parent_id, local_x, local_y, width, height,
          lookahead_x, lookahead_y, lookahead_width, lookahead_height
        FROM android_ui_hierarchy_node;
        """,
        out=Csv("""
        "node_id","parent_id","local_x","local_y","width","height","lookahead_x","lookahead_y","lookahead_width","lookahead_height"
        10,"[NULL]",12.500000,34.500000,200,100,10,30,220,110
        """))

  def test_stdlib_window_and_snapshot(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet {
          trusted_packet_sequence_id: 1
          timestamp: 1000
          trusted_pid: 100
          sequence_flags: 1
          interned_data {
            ui_strings { iid: 1 str: "MainActivity" }
          }
          ui_hierarchy {
            vsync_id: 12345
            is_keyframe: true
            windows {
              id: 1
              title_iid: 1
              display_id: 0
              left: 10
              top: 20
              right: 1080
              bottom: 1920
            }
          }
        }
        """),
        query="""
        INCLUDE PERFETTO MODULE android.ui_hierarchy;
        SELECT
          s.frame_id, w.window_name, w.bounds_left, w.bounds_top, w.bounds_right, w.bounds_bottom
        FROM android_ui_hierarchy_snapshot s
        JOIN android_ui_hierarchy_window w ON s.upid = w.upid;
        """,
        out=Csv("""
        "frame_id","window_name","bounds_left","bounds_top","bounds_right","bounds_bottom"
        12345,"MainActivity",10,20,1080,1920
        """))

  def test_stdlib_diff(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet {
          trusted_packet_sequence_id: 1
          timestamp: 1000
          trusted_pid: 100
          sequence_flags: 1
          interned_data {
            ui_strings { iid: 1 str: "MainWindow" }
            ui_strings { iid: 2 str: "RootView" }
            ui_strings { iid: 3 str: "Button" }
            ui_strings { iid: 4 str: "Submit" }
            ui_strings { iid: 5 str: "Text" }
            ui_strings { iid: 6 str: "Original" }
            ui_strings { iid: 7 str: "OldView" }
            ui_strings { iid: 8 str: "ToDelete" }
          }
          ui_hierarchy {
            is_keyframe: true
            windows {
              id: 1
              title_iid: 1
              left: 0
              top: 0
              right: 1080
              bottom: 1920
              nodes {
                id: 1
                kind: KIND_VIEW
                name_iid: 2
                x: 0
                y: 0
                width: 1080
                height: 1920
                flags: 1
              }
              nodes {
                id: 2
                parent_id: 1
                index: 0
                kind: KIND_COMPOSE_NODE
                name_iid: 3
                text_iid: 4
                x: 100
                y: 200
                width: 300
                height: 100
                flags: 5
              }
              nodes {
                id: 3
                parent_id: 1
                index: 1
                kind: KIND_COMPOSE_NODE
                name_iid: 5
                text_iid: 6
                x: 50
                y: 50
                width: 150
                height: 30
                flags: 1
              }
              nodes {
                id: 4
                parent_id: 1
                index: 2
                kind: KIND_VIEW
                name_iid: 7
                text_iid: 8
                x: 10
                y: 10
                width: 40
                height: 40
                flags: 1
              }
            }
          }
        }
        packet {
          trusted_packet_sequence_id: 1
          timestamp: 2000
          trusted_pid: 100
          sequence_flags: 2
          interned_data {
            ui_strings { iid: 9 str: "Updated" }
            ui_strings { iid: 10 str: "NewView" }
          }
          ui_hierarchy {
            windows {
              id: 1
              title_iid: 1
              left: 0
              top: 0
              right: 1080
              bottom: 1920
              removed_node_ids: 4
              nodes {
                id: 2
                parent_id: 1
                index: 0
                kind: KIND_COMPOSE_NODE
                name_iid: 3
                text_iid: 4
                x: 150
                y: 250
                width: 300
                height: 100
                flags: 5
              }
              nodes {
                id: 3
                parent_id: 1
                index: 1
                kind: KIND_COMPOSE_NODE
                name_iid: 5
                text_iid: 9
                x: 50
                y: 50
                width: 150
                height: 30
                flags: 1
              }
              nodes {
                id: 5
                parent_id: 1
                index: 2
                kind: KIND_VIEW
                name_iid: 10
                x: 500
                y: 500
                width: 100
                height: 100
                flags: 1
              }
            }
          }
        }
        """),
        query="""
        INCLUDE PERFETTO MODULE android.ui_hierarchy_analysis;
        SELECT
          status, node_id, name, changed_properties, old_text, new_text,
          old_bounds_left, old_bounds_top, new_bounds_left, new_bounds_top
        FROM android_ui_hierarchy_diff(1000, 2000)
        ORDER BY node_id;
        """,
        out=Csv("""
        "status","node_id","name","changed_properties","old_text","new_text","old_bounds_left","old_bounds_top","new_bounds_left","new_bounds_top"
        "changed",2,"Button","bounds","Submit","Submit",100,200,150,250
        "changed",3,"Text","text","Original","Updated",50,50,50,50
        "removed",4,"OldView","[NULL]","ToDelete","[NULL]",10,10,"[NULL]","[NULL]"
        "added",5,"NewView","[NULL]","[NULL]","[NULL]","[NULL]","[NULL]",500,500
        """))

  def test_stdlib_node_history(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet {
          trusted_packet_sequence_id: 1
          timestamp: 1000
          trusted_pid: 100
          sequence_flags: 1
          interned_data {
            ui_strings { iid: 1 str: "MainWindow" }
            ui_strings { iid: 2 str: "Button" }
            ui_strings { iid: 3 str: "Click me" }
          }
          ui_hierarchy {
            is_keyframe: true
            windows {
              id: 1
              title_iid: 1
              nodes {
                id: 2
                kind: KIND_COMPOSE_NODE
                name_iid: 2
                text_iid: 3
                x: 100
                y: 200
                width: 200
                height: 50
                flags: 5
              }
            }
          }
        }
        packet {
          trusted_packet_sequence_id: 1
          timestamp: 2000
          trusted_pid: 100
          sequence_flags: 2
          ui_hierarchy {
            windows {
              id: 1
              nodes {
                id: 2
                kind: KIND_COMPOSE_NODE
                name_iid: 2
                text_iid: 3
                x: 120
                y: 200
                width: 200
                height: 50
                flags: 5
              }
            }
          }
        }
        packet {
          trusted_packet_sequence_id: 1
          timestamp: 3000
          trusted_pid: 100
          sequence_flags: 2
          interned_data {
            ui_strings { iid: 4 str: "Clicked!" }
          }
          ui_hierarchy {
            windows {
              id: 1
              nodes {
                id: 2
                kind: KIND_COMPOSE_NODE
                name_iid: 2
                text_iid: 4
                x: 120
                y: 200
                width: 200
                height: 50
                flags: 5
              }
            }
          }
        }
        """),
        query="""
        INCLUDE PERFETTO MODULE android.ui_hierarchy_analysis;
        SELECT
          ts, node_id, name, text, bounds_left, changed_properties
        FROM android_ui_hierarchy_node_history(2)
        ORDER BY ts;
        """,
        out=Csv("""
        "ts","node_id","name","text","bounds_left","changed_properties"
        1000,2,"Button","Click me",100,"initial"
        2000,2,"Button","Click me",120,"bounds"
        3000,2,"Button","Clicked!",120,"text"
        """))

  def test_stdlib_recomposition_stats(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet {
          trusted_packet_sequence_id: 1
          timestamp: 500
          trusted_pid: 100
          sequence_flags: 1
          interned_data {
            ui_strings { iid: 1 str: "MyComponent" }
            ui_strings { iid: 2 str: "OtherComponent" }
            ui_strings { iid: 3 str: "MutableState" }
            ui_strings { iid: 4 str: "42" }
          }
          ui_hierarchy_events {
            events {
              type: TYPE_SCOPE
              ts: 1000
              dur: 5000000
              scope_id: 1
              name_iid: 1
            }
            events {
              type: TYPE_SCOPE_INVALIDATED
              ts: 1500
              scope_id: 1
              state_id: 10
              value_iid: 4
            }
            events {
              type: TYPE_STATE_CHANGED
              ts: 1500
              state_id: 10
              name_iid: 3
            }
            events {
              type: TYPE_SCOPE
              ts: 2000
              dur: 3000000
              scope_id: 1
              name_iid: 1
            }
            events {
              type: TYPE_SCOPE
              ts: 3000
              dur: 1000000
              scope_id: 2
              name_iid: 2
            }
          }
        }
        """),
        query="""
        INCLUDE PERFETTO MODULE android.ui_hierarchy_analysis;
        SELECT
          composable_name, recomposition_count, total_dur_ns, avg_dur_ns, top_cause, causes
        FROM android_ui_hierarchy_recomposition_stats
        ORDER BY total_dur_ns DESC;
        """,
        out=Csv("""
        "composable_name","recomposition_count","total_dur_ns","avg_dur_ns","top_cause","causes"
        "MyComponent",2,8000000,4000000.000000,"MutableState","MutableState"
        "OtherComponent",1,1000000,1000000.000000,"[NULL]","[NULL]"
        """))

  def test_stdlib_animation(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet {
          trusted_packet_sequence_id: 1
          timestamp: 500
          trusted_pid: 100
          sequence_flags: 1
          interned_data {
            ui_strings { iid: 1 str: "fade_in" }
            ui_strings { iid: 2 str: "0.0 -> 1.0" }
            ui_strings { iid: 3 str: "tween(300ms)" }
            ui_strings { iid: 4 str: "0.2" }
            ui_strings { iid: 5 str: "0.6" }
            ui_strings { iid: 6 str: "1.0" }
          }
          ui_hierarchy_events {
            events {
              type: TYPE_ANIMATION_START
              ts: 1000
              object_id: 101
              name_iid: 1
              value_iid: 2
              spec_iid: 3
            }
            events {
              type: TYPE_ANIMATION_FRAME
              ts: 1016
              object_id: 101
              value_iid: 4
              play_time_ns: 16000000
            }
            events {
              type: TYPE_ANIMATION_FRAME
              ts: 1032
              object_id: 101
              value_iid: 5
              play_time_ns: 32000000
            }
            events {
              type: TYPE_ANIMATION_FRAME
              ts: 1048
              object_id: 101
              value_iid: 6
              play_time_ns: 48000000
            }
            events {
              type: TYPE_ANIMATION_END
              ts: 1050
              object_id: 101
              action: 0
            }
          }
        }
        """),
        query="""
        INCLUDE PERFETTO MODULE android.ui_hierarchy_analysis;
        SELECT
          animation_id, label, start_ts, end_ts, dur, end_reason, frame_count, value_range, spec
        FROM android_ui_hierarchy_animation
        ORDER BY start_ts;
        """,
        out=Csv("""
        "animation_id","label","start_ts","end_ts","dur","end_reason","frame_count","value_range","spec"
        101,"fade_in",1000,1050,50,"finished",3,"0.0 -> 1.0","tween(300ms)"
        """))
