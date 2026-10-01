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


class UiHierarchy(TestSuite):

  def test_keyframe_and_delta(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet {
          trusted_packet_sequence_id: 1
          timestamp: 1000
          trusted_pid: 100
          sequence_flags: 1
          interned_data {
            ui_strings { iid: 1 str: "com.example/.MainActivity" }
            ui_strings { iid: 2 str: "RootView" }
            ui_strings { iid: 3 str: "Button" }
            ui_strings { iid: 4 str: "Click Me" }
          }
          ui_hierarchy {
            is_keyframe: true
            windows {
              id: 1
              title_iid: 1
              display_id: 0
              left: 0
              top: 0
              right: 1080
              bottom: 1920
              has_focus: true
              nodes {
                id: 10
                kind: KIND_VIEW
                name_iid: 2
                x: 0
                y: 0
                width: 1080
                height: 1920
                flags: 1
              }
              nodes {
                id: 20
                parent_id: 10
                index: 0
                kind: KIND_VIEW
                name_iid: 3
                text_iid: 4
                x: 100
                y: 100
                width: 200
                height: 100
                flags: 7
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
            ui_strings { iid: 5 str: "Clicked!" }
          }
          ui_hierarchy {
            is_keyframe: false
            windows {
              id: 1
              title_iid: 1
              display_id: 0
              left: 0
              top: 0
              right: 1080
              bottom: 1920
              has_focus: true
              nodes {
                id: 20
                parent_id: 10
                index: 0
                kind: KIND_VIEW
                name_iid: 3
                text_iid: 5
                x: 100
                y: 100
                width: 200
                height: 100
                flags: 7
              }
            }
          }
        }
        """),
        query="""
        SELECT
          id, ts, dur, node_id, parent_node_id, name, text
        FROM __intrinsic_ui_hierarchy_node
        ORDER BY node_id, ts;
        """,
        out=Csv("""
        "id","ts","dur","node_id","parent_node_id","name","text"
        1,1000,-1,10,"[NULL]","RootView","[NULL]"
        0,1000,1000,20,10,"Button","Click Me"
        2,2000,-1,20,10,"Button","Clicked!"
        """))

  def test_removed_node_ids(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet {
          trusted_packet_sequence_id: 1
          timestamp: 1000
          trusted_pid: 100
          sequence_flags: 1
          interned_data {
            ui_strings { iid: 1 str: "MainWindow" }
            ui_strings { iid: 2 str: "NodeA" }
            ui_strings { iid: 3 str: "NodeB" }
          }
          ui_hierarchy {
            is_keyframe: true
            windows {
              id: 1
              title_iid: 1
              display_id: 0
              left: 0
              top: 0
              right: 100
              bottom: 100
              nodes {
                id: 10
                name_iid: 2
                x: 0
                y: 0
                width: 100
                height: 100
              }
              nodes {
                id: 20
                parent_id: 10
                name_iid: 3
                x: 10
                y: 10
                width: 40
                height: 40
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
            is_keyframe: false
            windows {
              id: 1
              title_iid: 1
              display_id: 0
              left: 0
              top: 0
              right: 100
              bottom: 100
              removed_node_ids: 20
            }
          }
        }
        """),
        query="""
        SELECT
          node_id, ts, dur
        FROM __intrinsic_ui_hierarchy_node
        ORDER BY node_id;
        """,
        out=Csv("""
        "node_id","ts","dur"
        10,1000,-1
        20,1000,1000
        """))

  def test_window_removed(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet {
          trusted_packet_sequence_id: 1
          timestamp: 1000
          trusted_pid: 100
          sequence_flags: 1
          interned_data {
            ui_strings { iid: 1 str: "Dialog" }
            ui_strings { iid: 2 str: "DialogView" }
          }
          ui_hierarchy {
            is_keyframe: true
            windows {
              id: 2
              title_iid: 1
              display_id: 0
              left: 50
              top: 50
              right: 200
              bottom: 200
              nodes {
                id: 99
                name_iid: 2
                x: 0
                y: 0
                width: 150
                height: 150
              }
            }
          }
        }
        packet {
          trusted_packet_sequence_id: 1
          timestamp: 2500
          trusted_pid: 100
          sequence_flags: 2
          ui_hierarchy {
            is_keyframe: false
            windows {
              id: 2
              removed: true
            }
          }
        }
        """),
        query="""
        SELECT
          w.window_id, w.ts AS win_ts, w.dur AS win_dur,
          n.node_id, n.ts AS node_ts, n.dur AS node_dur
        FROM __intrinsic_ui_hierarchy_window w
        LEFT JOIN __intrinsic_ui_hierarchy_node n ON w.window_id = n.window_id;
        """,
        out=Csv("""
        "window_id","win_ts","win_dur","node_id","node_ts","node_dur"
        2,1000,1500,99,1000,1500
        """))

  def test_node_missing_from_keyframe_closed(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet {
          trusted_packet_sequence_id: 1
          timestamp: 1000
          trusted_pid: 100
          sequence_flags: 1
          interned_data {
            ui_strings { iid: 1 str: "Win" }
            ui_strings { iid: 2 str: "KeepMe" }
            ui_strings { iid: 3 str: "DropMe" }
            ui_strings { iid: 4 str: "DropChild" }
          }
          ui_hierarchy {
            is_keyframe: true
            windows {
              id: 1
              title_iid: 1
              nodes {
                id: 10
                name_iid: 2
                x: 0
                y: 0
                width: 100
                height: 100
              }
              nodes {
                id: 20
                parent_id: 10
                name_iid: 3
                x: 10
                y: 10
                width: 50
                height: 50
              }
              nodes {
                id: 30
                parent_id: 20
                name_iid: 4
                x: 5
                y: 5
                width: 20
                height: 20
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
            ui_strings { iid: 1 str: "Win" }
            ui_strings { iid: 2 str: "KeepMe" }
          }
          ui_hierarchy {
            is_keyframe: true
            windows {
              id: 1
              title_iid: 1
              nodes {
                id: 10
                name_iid: 2
                x: 0
                y: 0
                width: 100
                height: 100
              }
            }
          }
        }
        """),
        query="""
        SELECT
          node_id, ts, dur
        FROM __intrinsic_ui_hierarchy_node
        ORDER BY node_id;
        """,
        out=Csv("""
        "node_id","ts","dur"
        10,1000,-1
        20,1000,2000
        30,1000,2000
        """))

  def test_incremental_state_reset_same_content(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet {
          trusted_packet_sequence_id: 1
          timestamp: 1000
          trusted_pid: 100
          sequence_flags: 1
          interned_data {
            ui_strings { iid: 1 str: "AppWin" }
            ui_strings { iid: 2 str: "StaticNode" }
          }
          ui_hierarchy {
            is_keyframe: true
            windows {
              id: 1
              title_iid: 1
              display_id: 0
              left: 0
              top: 0
              right: 500
              bottom: 500
              has_focus: true
              nodes {
                id: 100
                name_iid: 2
                x: 0
                y: 0
                width: 500
                height: 500
              }
            }
          }
        }
        packet {
          trusted_packet_sequence_id: 1
          timestamp: 2000
          trusted_pid: 100
          sequence_flags: 1
          interned_data {
            ui_strings { iid: 10 str: "AppWin" }
            ui_strings { iid: 20 str: "StaticNode" }
          }
          ui_hierarchy {
            is_keyframe: true
            windows {
              id: 1
              title_iid: 10
              display_id: 0
              left: 0
              top: 0
              right: 500
              bottom: 500
              has_focus: true
              nodes {
                id: 100
                name_iid: 20
                x: 0
                y: 0
                width: 500
                height: 500
              }
            }
          }
        }
        """),
        query="""
        SELECT count(*) AS node_count, min(ts) AS min_ts, max(dur) AS max_dur
        FROM __intrinsic_ui_hierarchy_node;
        """,
        out=Csv("""
        "node_count","min_ts","max_dur"
        1,1000,-1
        """))

  def test_two_processes(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet {
          trusted_packet_sequence_id: 1
          timestamp: 1000
          trusted_pid: 101
          sequence_flags: 1
          interned_data {
            ui_strings { iid: 1 str: "Proc1Win" }
            ui_strings { iid: 2 str: "Proc1Node" }
          }
          ui_hierarchy {
            is_keyframe: true
            windows {
              id: 1
              title_iid: 1
              nodes {
                id: 10
                name_iid: 2
                x: 0
                y: 0
                width: 100
                height: 100
              }
            }
          }
        }
        packet {
          trusted_packet_sequence_id: 2
          timestamp: 1500
          trusted_pid: 102
          sequence_flags: 1
          interned_data {
            ui_strings { iid: 1 str: "Proc2Win" }
            ui_strings { iid: 2 str: "Proc2Node" }
          }
          ui_hierarchy {
            is_keyframe: true
            windows {
              id: 1
              title_iid: 1
              nodes {
                id: 10
                name_iid: 2
                x: 0
                y: 0
                width: 100
                height: 100
              }
            }
          }
        }
        """),
        query="""
        SELECT
          p.pid, w.title, n.name
        FROM __intrinsic_ui_hierarchy_node n
        JOIN __intrinsic_ui_hierarchy_window w ON n.window_id = w.window_id AND n.upid = w.upid
        JOIN process p ON n.upid = p.upid
        ORDER BY p.pid;
        """,
        out=Csv("""
        "pid","title","name"
        101,"Proc1Win","Proc1Node"
        102,"Proc2Win","Proc2Node"
        """))

  def test_missing_interned_string_stat(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet {
          trusted_packet_sequence_id: 1
          timestamp: 1000
          trusted_pid: 100
          sequence_flags: 1
          ui_hierarchy {
            is_keyframe: true
            windows {
              id: 1
              title_iid: 9999
              nodes {
                id: 10
                name_iid: 8888
                x: 0
                y: 0
                width: 100
                height: 100
              }
            }
          }
        }
        """),
        query="""
        SELECT name, value
        FROM stats
        WHERE name = 'ui_hierarchy_missing_interned_string';
        """,
        out=Csv("""
        "name","value"
        "ui_hierarchy_missing_interned_string",2
        """))

  def test_parent_moves_descendants_shift(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet {
          trusted_packet_sequence_id: 1
          timestamp: 1000
          trusted_pid: 100
          sequence_flags: 1
          interned_data {
            ui_strings { iid: 1 str: "Win" }
            ui_strings { iid: 2 str: "Parent" }
            ui_strings { iid: 3 str: "Child" }
          }
          ui_hierarchy {
            is_keyframe: true
            windows {
              id: 1
              title_iid: 1
              left: 0
              top: 0
              right: 1000
              bottom: 1000
              nodes {
                id: 10
                name_iid: 2
                x: 50
                y: 50
                width: 200
                height: 200
              }
              nodes {
                id: 20
                parent_id: 10
                name_iid: 3
                x: 10
                y: 20
                width: 50
                height: 30
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
            ui_strings { iid: 1 str: "Win" }
            ui_strings { iid: 2 str: "Parent" }
          }
          ui_hierarchy {
            is_keyframe: false
            windows {
              id: 1
              title_iid: 1
              left: 0
              top: 0
              right: 1000
              bottom: 1000
              nodes {
                id: 10
                name_iid: 2
                x: 150
                y: 250
                width: 200
                height: 200
              }
            }
          }
        }
        """),
        query="""
        SELECT
          node_id, ts, dur, bounds_left, bounds_top, bounds_right, bounds_bottom
        FROM __intrinsic_ui_hierarchy_node
        ORDER BY node_id, ts;
        """,
        out=Csv("""
        "node_id","ts","dur","bounds_left","bounds_top","bounds_right","bounds_bottom"
        10,1000,1000,50,50,250,250
        10,2000,-1,150,250,350,450
        20,1000,1000,60,70,110,100
        20,2000,-1,160,270,210,300
        """))

  def test_window_moves_nodes_shift(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet {
          trusted_packet_sequence_id: 1
          timestamp: 1000
          trusted_pid: 100
          sequence_flags: 1
          interned_data {
            ui_strings { iid: 1 str: "Win" }
            ui_strings { iid: 2 str: "RootNode" }
            ui_strings { iid: 3 str: "ChildNode" }
          }
          ui_hierarchy {
            is_keyframe: true
            windows {
              id: 1
              title_iid: 1
              left: 0
              top: 0
              right: 1000
              bottom: 1000
              nodes {
                id: 10
                name_iid: 2
                x: 10
                y: 20
                width: 100
                height: 100
              }
              nodes {
                id: 20
                parent_id: 10
                name_iid: 3
                x: 5
                y: 5
                width: 20
                height: 20
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
            ui_strings { iid: 1 str: "Win" }
          }
          ui_hierarchy {
            is_keyframe: false
            windows {
              id: 1
              title_iid: 1
              left: 100
              top: 200
              right: 1100
              bottom: 1200
            }
          }
        }
        """),
        query="""
        SELECT
          node_id, ts, dur, bounds_left, bounds_top, bounds_right, bounds_bottom
        FROM __intrinsic_ui_hierarchy_node
        ORDER BY node_id, ts;
        """,
        out=Csv("""
        "node_id","ts","dur","bounds_left","bounds_top","bounds_right","bounds_bottom"
        10,1000,1000,10,20,110,120
        10,2000,-1,110,220,210,320
        20,1000,1000,15,25,35,45
        20,2000,-1,115,225,135,245
        """))

  def test_transform_matrix_aabb(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet {
          trusted_packet_sequence_id: 1
          timestamp: 1000
          trusted_pid: 100
          sequence_flags: 1
          interned_data {
            ui_strings { iid: 1 str: "Win" }
            ui_strings { iid: 2 str: "Rotated" }
            ui_strings { iid: 3 str: "Scaled" }
          }
          ui_hierarchy {
            is_keyframe: true
            windows {
              id: 1
              title_iid: 1
              left: 0
              top: 0
              right: 1000
              bottom: 1000
              nodes {
                id: 10
                name_iid: 2
                width: 20
                height: 10
                transform: 0.0
                transform: -1.0
                transform: 100.0
                transform: 1.0
                transform: 0.0
                transform: 50.0
                transform: 0.0
                transform: 0.0
                transform: 1.0
              }
              nodes {
                id: 20
                name_iid: 3
                width: 30
                height: 40
                transform: 2.0
                transform: 0.0
                transform: 10.0
                transform: 0.0
                transform: 2.0
                transform: 20.0
                transform: 0.0
                transform: 0.0
                transform: 1.0
              }
            }
          }
        }
        """),
        query="""
        SELECT
          node_id, width, height, bounds_left, bounds_top, bounds_right, bounds_bottom
        FROM __intrinsic_ui_hierarchy_node
        ORDER BY node_id;
        """,
        out=Csv("""
        "node_id","width","height","bounds_left","bounds_top","bounds_right","bounds_bottom"
        10,20,10,90,50,100,70
        20,30,40,10,20,70,100
        """))

  def test_reparent_node(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet {
          trusted_packet_sequence_id: 1
          timestamp: 1000
          trusted_pid: 100
          sequence_flags: 1
          interned_data {
            ui_strings { iid: 1 str: "Win" }
            ui_strings { iid: 2 str: "Root" }
            ui_strings { iid: 3 str: "ParentA" }
            ui_strings { iid: 4 str: "ParentB" }
            ui_strings { iid: 5 str: "Child" }
          }
          ui_hierarchy {
            is_keyframe: true
            windows {
              id: 1
              title_iid: 1
              left: 0
              top: 0
              right: 1000
              bottom: 1000
              nodes {
                id: 1
                name_iid: 2
                x: 0
                y: 0
                width: 1000
                height: 1000
              }
              nodes {
                id: 10
                parent_id: 1
                name_iid: 3
                x: 10
                y: 10
                width: 100
                height: 100
              }
              nodes {
                id: 20
                parent_id: 1
                name_iid: 4
                x: 200
                y: 200
                width: 100
                height: 100
              }
              nodes {
                id: 30
                parent_id: 10
                name_iid: 5
                x: 5
                y: 5
                width: 20
                height: 20
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
            ui_strings { iid: 1 str: "Win" }
            ui_strings { iid: 5 str: "Child" }
          }
          ui_hierarchy {
            is_keyframe: false
            windows {
              id: 1
              title_iid: 1
              left: 0
              top: 0
              right: 1000
              bottom: 1000
              nodes {
                id: 30
                parent_id: 20
                name_iid: 5
                x: 5
                y: 5
                width: 20
                height: 20
              }
            }
          }
        }
        """),
        query="""
        SELECT
          node_id, ts, dur, parent_node_id, bounds_left, bounds_top, bounds_right, bounds_bottom
        FROM __intrinsic_ui_hierarchy_node
        WHERE node_id = 30
        ORDER BY ts;
        """,
        out=Csv("""
        "node_id","ts","dur","parent_node_id","bounds_left","bounds_top","bounds_right","bounds_bottom"
        30,1000,1000,10,15,15,35,35
        30,2000,-1,20,205,205,225,225
        """))

  def test_malformed_transform_stat(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet {
          trusted_packet_sequence_id: 1
          timestamp: 1000
          trusted_pid: 100
          sequence_flags: 1
          interned_data {
            ui_strings { iid: 1 str: "Win" }
            ui_strings { iid: 2 str: "BadTransformNode" }
          }
          ui_hierarchy {
            is_keyframe: true
            windows {
              id: 1
              title_iid: 1
              left: 0
              top: 0
              right: 1000
              bottom: 1000
              nodes {
                id: 10
                name_iid: 2
                width: 100
                height: 50
                transform: 1.0
                transform: 0.0
                transform: 0.0
                transform: 0.0
                transform: 1.0
                transform: 0.0
                transform: 0.0
                transform: 0.0
              }
            }
          }
        }
        """),
        query="""
        SELECT
          n.node_id,
          n.transform,
          n.bounds_left,
          n.bounds_top,
          n.bounds_right,
          n.bounds_bottom,
          s.value AS invalid_transform_stat
        FROM __intrinsic_ui_hierarchy_node n
        JOIN stats s ON s.name = 'ui_hierarchy_invalid_transform';
        """,
        out=Csv("""
        "node_id","transform","bounds_left","bounds_top","bounds_right","bounds_bottom","invalid_transform_stat"
        10,"[NULL]",0,0,100,50,1
        """))
