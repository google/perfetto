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

  # Semantic keys/roles on anchor nodes and app state / interaction events,
  # and the pending work mask of windows.
  def test_stdlib_semantic_key_and_pending_work(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet {
          trusted_packet_sequence_id: 1
          timestamp: 500
          trusted_pid: 100
          sequence_flags: 1
          interned_data {
            ui_strings { iid: 1 str: "NotificationShade" }
            ui_strings { iid: 2 str: "0|com.example|7|null|10001" }
            ui_strings { iid: 3 str: "title" }
            ui_strings { iid: 4 str: "ExpandableNotificationRow" }
            ui_strings { iid: 5 str: "TextView" }
            ui_strings { iid: 6 str: "expanded" }
            ui_strings { iid: 7 str: "true" }
            ui_strings { iid: 8 str: "tap" }
          }
          ui_hierarchy {
            is_keyframe: true
            windows {
              id: 1
              title_iid: 1
              right: 1080
              bottom: 2400
              pending_work_mask: 18
              nodes {
                id: 2
                kind: KIND_VIEW
                name_iid: 4
                semantic_key_iid: 2
                width: 1080
                height: 300
                flags: 1
              }
              nodes {
                id: 3
                parent_id: 2
                kind: KIND_VIEW
                name_iid: 5
                semantic_role_iid: 3
                width: 500
                height: 50
                flags: 1
              }
            }
          }
        }
        packet {
          trusted_packet_sequence_id: 1
          timestamp: 1000
          trusted_pid: 100
          sequence_flags: 2
          ui_hierarchy {
            windows {
              id: 1
              title_iid: 1
              right: 1080
              bottom: 2400
            }
          }
        }
        packet {
          trusted_packet_sequence_id: 1
          timestamp: 1200
          trusted_pid: 100
          sequence_flags: 2
          ui_hierarchy_events {
            events {
              type: TYPE_APP_STATE
              ts: 1100
              semantic_key_iid: 2
              args { name_iid: 6 value_iid: 7 }
            }
            events {
              type: TYPE_INTERACTION
              ts: 1150
              name_iid: 8
              semantic_key_iid: 2
            }
          }
        }
        """),
        query="""
        INCLUDE PERFETTO MODULE android.ui_hierarchy;
        SELECT ts, pending_work_mask FROM android_ui_hierarchy_window ORDER BY ts;
        SELECT node_id, semantic_key, semantic_role
        FROM android_ui_hierarchy_node
        ORDER BY node_id;
        SELECT ts, type, name, semantic_key, args
        FROM android_ui_hierarchy_compose_event
        ORDER BY ts;
        """,
        out=Csv("""
        "ts","pending_work_mask"
        500,18
        1000,"[NULL]"

        "node_id","semantic_key","semantic_role"
        2,"0|com.example|7|null|10001","[NULL]"
        3,"[NULL]","title"

        "ts","type","name","semantic_key","args"
        1100,"app_state","[NULL]","0|com.example|7|null|10001","expanded=true"
        1150,"interaction","tap","0|com.example|7|null|10001","[NULL]"
        """))

  # A node is effectively visible if it and all its ancestors are visible, and
  # its effective alpha is the product of the alphas up to the root. Children
  # are listed before their parents on purpose: the result must not depend on
  # the order of the nodes in the packet. Hiding only the root in a delta
  # updates every descendant.
  def test_stdlib_effective_visibility(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet {
          trusted_packet_sequence_id: 1
          timestamp: 1000
          trusted_pid: 100
          sequence_flags: 1
          interned_data {
            ui_strings { iid: 1 str: "Window" }
            ui_strings { iid: 2 str: "View" }
          }
          ui_hierarchy {
            is_keyframe: true
            windows {
              id: 1
              title_iid: 1
              right: 100
              bottom: 100
              nodes { id: 4 parent_id: 2 kind: KIND_VIEW name_iid: 2 width: 10 height: 10 flags: 0 }
              nodes { id: 3 parent_id: 2 kind: KIND_VIEW name_iid: 2 width: 10 height: 10 flags: 1 alpha: 0.5 }
              nodes { id: 2 parent_id: 1 kind: KIND_VIEW name_iid: 2 width: 50 height: 50 flags: 1 alpha: 0.5 }
              nodes { id: 1 kind: KIND_VIEW name_iid: 2 width: 100 height: 100 flags: 1 }
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
              title_iid: 1
              right: 100
              bottom: 100
              nodes { id: 1 kind: KIND_VIEW name_iid: 2 width: 100 height: 100 flags: 0 }
            }
          }
        }
        packet {
          trusted_packet_sequence_id: 1
          timestamp: 3000
          trusted_pid: 100
          sequence_flags: 2
          ui_hierarchy {
            windows {
              id: 1
              title_iid: 1
              right: 100
              bottom: 100
              nodes { id: 1 kind: KIND_VIEW name_iid: 2 width: 100 height: 100 flags: 1 alpha: 0 }
            }
          }
        }
        """),
        query="""
        INCLUDE PERFETTO MODULE android.ui_hierarchy;
        SELECT node_id, ts, is_visible, alpha, is_effectively_visible,
          effective_alpha
        FROM android_ui_hierarchy_node
        ORDER BY node_id, ts;
        """,
        out=Csv("""
        "node_id","ts","is_visible","alpha","is_effectively_visible","effective_alpha"
        1,1000,1,1.000000,1,1.000000
        1,2000,0,1.000000,0,1.000000
        1,3000,1,0.000000,1,0.000000
        2,1000,1,0.500000,1,0.500000
        2,2000,1,0.500000,0,0.500000
        2,3000,1,0.500000,1,0.000000
        3,1000,1,0.500000,1,0.250000
        3,2000,1,0.500000,0,0.250000
        3,3000,1,0.500000,1,0.000000
        4,1000,0,1.000000,0,0.500000
        4,3000,0,1.000000,0,0.000000
        """))

  # Screen recording frames <-> UI snapshots, by closest timestamp.
  def test_stdlib_video_frame_helpers(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet {
          trusted_packet_sequence_id: 1
          timestamp: 1000
          trusted_pid: 100
          sequence_flags: 1
          interned_data {
            ui_strings { iid: 1 str: "Window" }
          }
          ui_hierarchy {
            is_keyframe: true
            windows { id: 1 title_iid: 1 right: 100 bottom: 100 }
          }
        }
        packet {
          trusted_packet_sequence_id: 1
          timestamp: 2000
          trusted_pid: 100
          sequence_flags: 2
          ui_hierarchy {
            windows { id: 1 title_iid: 1 right: 100 bottom: 100 }
          }
        }
        packet {
          trusted_packet_sequence_id: 2
          timestamp: 1100
          [com.android.internal.FrameworksBaseTracePacket.video_frame] {
            display_id: 0
            frame_number: 1
            au_data: "\x00\x00\x00\x01"
          }
        }
        packet {
          trusted_packet_sequence_id: 2
          timestamp: 1600
          [com.android.internal.FrameworksBaseTracePacket.video_frame] {
            display_id: 0
            frame_number: 2
            au_data: "\x00\x00\x00\x01"
          }
        }
        packet {
          trusted_packet_sequence_id: 2
          timestamp: 2900
          [com.android.internal.FrameworksBaseTracePacket.video_frame] {
            display_id: 0
            frame_number: 3
            au_data: "\x00\x00\x00\x01"
          }
        }
        """),
        query="""
        INCLUDE PERFETTO MODULE android.ui_hierarchy;
        SELECT
          v.ts AS frame_ts,
          (SELECT s.ts FROM android_ui_hierarchy_snapshot s
           WHERE s.id = (
             SELECT id FROM android_ui_hierarchy_snapshot_for_video_frame(v.id)
           )) AS snapshot_ts
        FROM __intrinsic_video_frames v
        ORDER BY v.ts;
        SELECT
          s.ts AS snapshot_ts,
          (SELECT v.ts FROM __intrinsic_video_frames v
           WHERE v.id = (
             SELECT id FROM android_video_frame_for_ui_hierarchy_snapshot(s.id)
           )) AS frame_ts
        FROM android_ui_hierarchy_snapshot s
        ORDER BY s.ts;
        """,
        out=Csv("""
        "frame_ts","snapshot_ts"
        1100,1000
        1600,2000
        2900,2000

        "snapshot_ts","frame_ts"
        1000,1100
        2000,1600
        """))

  # UiStateEvent -> android_sysui_state: one row per value, lasting until the
  # next change of the same field in the same process (or the trace end).
  def test_stdlib_sysui_state(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet {
          trusted_packet_sequence_id: 1
          timestamp: 1000
          trusted_pid: 100
          sequence_flags: 1
          interned_data {
            ui_strings { iid: 1 str: "shade_expansion" }
            ui_strings { iid: 2 str: "bouncer" }
            ui_strings { iid: 3 str: "scene" }
            ui_strings { iid: 4 str: "Lockscreen" }
            ui_strings { iid: 5 str: "Shade" }
            ui_strings { iid: 6 str: "pinned_huns" }
            ui_strings { iid: 7 str: "key_a" }
            ui_strings { iid: 8 str: "key_b" }
          }
          ui_hierarchy_events {
            state_events { ts: 1000 field_iid: 1 value_float: 0 }
            state_events { ts: 1000 field_iid: 2 value_bool: false }
            state_events { ts: 1000 field_iid: 3 value_string_iid: 4 }
            state_events { ts: 1500 field_iid: 1 value_float: 0.5 }
            state_events { ts: 1600 field_iid: 6 value_key_iids: 7 value_key_iids: 8 }
            state_events { ts: 2000 field_iid: 1 value_float: 1 }
            state_events { ts: 2000 field_iid: 3 value_string_iid: 5 }
            state_events { ts: 2500 field_iid: 2 value_bool: true }
          }
        }
        """),
        query="""
        INCLUDE PERFETTO MODULE android.ui_hierarchy;
        SELECT
          s.ts, s.dur, p.pid, s.field, s.value_float, s.value_bool,
          s.value_string, s.value_key
        FROM android_sysui_state s
        JOIN process p USING (upid)
        ORDER BY s.field, s.ts;
        """,
        out=Csv("""
        "ts","dur","pid","field","value_float","value_bool","value_string","value_key"
        1000,1500,100,"bouncer","[NULL]",0,"[NULL]","[NULL]"
        2500,0,100,"bouncer","[NULL]",1,"[NULL]","[NULL]"
        1600,900,100,"pinned_huns","[NULL]","[NULL]","[NULL]","key_a,key_b"
        1000,1000,100,"scene","[NULL]","[NULL]","Lockscreen","[NULL]"
        2000,500,100,"scene","[NULL]","[NULL]","Shade","[NULL]"
        1000,500,100,"shade_expansion",0.000000,"[NULL]","[NULL]","[NULL]"
        1500,500,100,"shade_expansion",0.500000,"[NULL]","[NULL]","[NULL]"
        2000,500,100,"shade_expansion",1.000000,"[NULL]","[NULL]","[NULL]"
        """))

  # UiHierarchySnapshot.sysui_state -> android_ui_hierarchy_snapshot_state.
  def test_stdlib_snapshot_sysui_state(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet {
          trusted_packet_sequence_id: 1
          timestamp: 1000
          trusted_pid: 100
          sequence_flags: 1
          interned_data {
            ui_strings { iid: 1 str: "NotificationShade" }
            ui_strings { iid: 2 str: "KEYGUARD" }
            ui_strings { iid: 3 str: "Lockscreen" }
            ui_strings { iid: 4 str: "LOCKSCREEN" }
            ui_strings { iid: 5 str: "AOD" }
            ui_strings { iid: 6 str: "RUNNING" }
            ui_strings { iid: 7 str: "key_a" }
            ui_strings { iid: 8 str: "key_b" }
          }
          ui_hierarchy {
            is_keyframe: true
            windows { id: 1 title_iid: 1 right: 1080 bottom: 2400 }
            sysui_state {
              shade_expansion: 0.25
              qs_expansion: 0
              status_bar_state_iid: 2
              scene_iid: 3
              keyguard_transition_from_iid: 4
              keyguard_transition_to_iid: 5
              keyguard_transition_state_iid: 6
              keyguard_transition_value: 0.5
              dozing: false
              bouncer: true
              pinned_hun_key_iids: 7
              pinned_hun_key_iids: 8
              guts_key_iid: 7
              user_expanded_key_iids: 8
              lockscreen_show_notifications: true
            }
          }
        }
        packet {
          trusted_packet_sequence_id: 1
          timestamp: 2000
          trusted_pid: 100
          sequence_flags: 2
          ui_hierarchy {
            windows { id: 1 title_iid: 1 right: 1080 bottom: 2400 }
          }
        }
        """),
        query="""
        INCLUDE PERFETTO MODULE android.ui_hierarchy;
        SELECT
          s.ts, st.shade_expansion, st.qs_expansion, st.status_bar_state,
          st.scene, st.keyguard_transition_from, st.keyguard_transition_to,
          st.keyguard_transition_state, st.keyguard_transition_value, st.dozing,
          st.bouncer, st.pinned_hun_keys, st.guts_key, st.user_expanded_keys,
          st.remote_input_keys, st.lockscreen_show_notifications,
          st.lockscreen_show_private
        FROM android_ui_hierarchy_snapshot s
        LEFT JOIN android_ui_hierarchy_snapshot_state st
          ON st.snapshot_id = s.id
        ORDER BY s.ts;
        """,
        out=Csv("""
        "ts","shade_expansion","qs_expansion","status_bar_state","scene","keyguard_transition_from","keyguard_transition_to","keyguard_transition_state","keyguard_transition_value","dozing","bouncer","pinned_hun_keys","guts_key","user_expanded_keys","remote_input_keys","lockscreen_show_notifications","lockscreen_show_private"
        1000,0.250000,0.000000,"KEYGUARD","Lockscreen","LOCKSCREEN","AOD","RUNNING",0.500000,0,1,"key_a,key_b","key_a","key_b","[NULL]",1,"[NULL]"
        2000,"[NULL]","[NULL]","[NULL]","[NULL]","[NULL]","[NULL]","[NULL]","[NULL]","[NULL]","[NULL]","[NULL]","[NULL]","[NULL]","[NULL]","[NULL]","[NULL]"
        """))

  # android_sysui_state starts each changing field at its value in the first
  # snapshot with SystemUI state: unless the first change repeats it (bouncer)
  # or the field never changes (qs_expansion). A change with no value clears a
  # set (pinned_hun_key).
  def test_stdlib_sysui_state_initial_values(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet {
          trusted_packet_sequence_id: 1
          timestamp: 1000
          trusted_pid: 100
          sequence_flags: 1
          interned_data {
            ui_strings { iid: 1 str: "NotificationShade" }
            ui_strings { iid: 2 str: "Shade" }
            ui_strings { iid: 3 str: "Gone" }
            ui_strings { iid: 4 str: "key_a" }
            ui_strings { iid: 5 str: "shade_expansion" }
            ui_strings { iid: 6 str: "scene" }
            ui_strings { iid: 7 str: "pinned_hun_key" }
            ui_strings { iid: 8 str: "bouncer" }
          }
          ui_hierarchy {
            is_keyframe: true
            windows { id: 1 title_iid: 1 right: 1080 bottom: 2400 }
            sysui_state {
              shade_expansion: 1
              qs_expansion: 0
              scene_iid: 2
              bouncer: false
              pinned_hun_key_iids: 4
            }
          }
        }
        packet {
          trusted_packet_sequence_id: 1
          timestamp: 1500
          trusted_pid: 100
          sequence_flags: 2
          ui_hierarchy_events {
            state_events { ts: 1500 field_iid: 5 value_float: 0 }
            state_events { ts: 1500 field_iid: 6 value_string_iid: 3 }
            state_events { ts: 1600 field_iid: 7 }
            state_events { ts: 1700 field_iid: 8 value_bool: false }
          }
        }
        """),
        query="""
        INCLUDE PERFETTO MODULE android.ui_hierarchy;
        SELECT
          s.ts, s.dur, p.pid, s.field, s.value_float, s.value_bool,
          s.value_string, s.value_key, s.value
        FROM android_sysui_state s
        JOIN process p USING (upid)
        ORDER BY s.field, s.ts;
        """,
        out=Csv("""
        "ts","dur","pid","field","value_float","value_bool","value_string","value_key","value"
        1700,0,100,"bouncer","[NULL]",0,"[NULL]","[NULL]","false"
        1000,600,100,"pinned_hun_key","[NULL]","[NULL]","[NULL]","key_a","key_a"
        1600,100,100,"pinned_hun_key","[NULL]","[NULL]","[NULL]","[NULL]","[NULL]"
        1000,500,100,"scene","[NULL]","[NULL]","Shade","[NULL]","Shade"
        1500,200,100,"scene","[NULL]","[NULL]","Gone","[NULL]","Gone"
        1000,500,100,"shade_expansion",1.000000,"[NULL]","[NULL]","[NULL]","1"
        1500,200,100,"shade_expansion",0.000000,"[NULL]","[NULL]","[NULL]","0"
        """))

  # The queries the UI plugin runs for the Transition section and the slider
  # marks (ui_hierarchy_transitions.ts): Shell transitions with their handler
  # name and participants (SF layer ids), in a trace with UI hierarchy data.
  def test_shell_transitions_for_ui(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet {
          trusted_packet_sequence_id: 1
          timestamp: 1000
          trusted_pid: 100
          sequence_flags: 1
          interned_data {
            ui_strings { iid: 1 str: "NotificationShade" }
          }
          ui_hierarchy {
            is_keyframe: true
            windows { id: 1 title_iid: 1 right: 1080 bottom: 2400 }
          }
        }
        packet {
          trusted_packet_sequence_id: 2
          trusted_pid: 1305
          timestamp: 1000
          [com.android.internal.FrameworksBaseWinscopeTracePacket.shell_handler_mappings] {
            mapping {
              id: 1
              name: "com.android.wm.shell.transition.DefaultTransitionHandler"
            }
          }
        }
        packet {
          trusted_packet_sequence_id: 2
          trusted_pid: 1305
          timestamp: 1500
          [com.android.internal.FrameworksBaseWinscopeTracePacket.shell_transition] {
            id: 41
            create_time_ns: 1100
            send_time_ns: 1200
            dispatch_time_ns: 1250
            finish_time_ns: 1500
            start_transaction_id: 7
            finish_transaction_id: 8
            handler: 1
            type: 3
            changes { mode: 3 layer_id: 74 window_id: 1074 flags: 0 }
            changes { mode: 4 layer_id: 53 window_id: 1053 flags: 2 }
          }
        }
        packet {
          trusted_packet_sequence_id: 1
          timestamp: 2000
          trusted_pid: 100
          sequence_flags: 2
          ui_hierarchy {
            windows { id: 1 title_iid: 1 right: 1080 bottom: 2400 }
          }
        }
        """),
        query="""
        SELECT
          t.transition_id,
          t.transition_type,
          t.status,
          t.send_time_ns,
          t.dispatch_time_ns,
          t.finish_time_ns,
          t.start_transaction_id,
          t.finish_transaction_id,
          h.handler_name
        FROM __intrinsic_window_manager_shell_transitions t
        LEFT JOIN (
          SELECT handler_id, min(handler_name) AS handler_name
          FROM __intrinsic_window_manager_shell_transition_handlers
          GROUP BY handler_id
        ) h ON h.handler_id = t.handler
        ORDER BY coalesce(t.dispatch_time_ns, t.send_time_ns, t.ts);
        SELECT transition_id, layer_id, mode, flags
        FROM __intrinsic_window_manager_shell_transition_participants
        ORDER BY layer_id;
        """,
        out=Csv("""
        "transition_id","transition_type","status","send_time_ns","dispatch_time_ns","finish_time_ns","start_transaction_id","finish_transaction_id","handler_name"
        41,3,"played",1200,1250,1500,7,8,"com.android.wm.shell.transition.DefaultTransitionHandler"

        "transition_id","layer_id","mode","flags"
        41,53,4,2
        41,74,3,0
        """))
