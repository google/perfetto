#!/usr/bin/env python3
# Copyright (C) 2025 The Android Open Source Project
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


class SlicesStack(TestSuite):

  def test_identical_stacks_same_stack_id(self):
    # Tests that slices with identical call stacks get the same stack_id
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet {
          ftrace_events {
            cpu: 0
            event {
              timestamp: 1000
              pid: 10
              print {
                buf: "B|10|A"
              }
            }
            event {
              timestamp: 1100
              pid: 10
              print {
                buf: "B|10|B"
              }
            }
            event {
              timestamp: 1200
              pid: 10
              print {
                buf: "B|10|C"
              }
            }
            event {
              timestamp: 1300
              pid: 10
              print {
                buf: "E|10"
              }
            }
            event {
              timestamp: 1400
              pid: 10
              print {
                buf: "E|10"
              }
            }
            event {
              timestamp: 1500
              pid: 10
              print {
                buf: "E|10"
              }
            }
            # Second identical stack A->B->C
            event {
              timestamp: 2000
              pid: 10
              print {
                buf: "B|10|A"
              }
            }
            event {
              timestamp: 2100
              pid: 10
              print {
                buf: "B|10|B"
              }
            }
            event {
              timestamp: 2200
              pid: 10
              print {
                buf: "B|10|C"
              }
            }
            event {
              timestamp: 2300
              pid: 10
              print {
                buf: "E|10"
              }
            }
            event {
              timestamp: 2400
              pid: 10
              print {
                buf: "E|10"
              }
            }
            event {
              timestamp: 2500
              pid: 10
              print {
                buf: "E|10"
              }
            }
          }
        }
        """),
        query="""
        INCLUDE PERFETTO MODULE slices.stack;

        SELECT
          s1.name AS name1,
          s1.ts AS ts1,
          s2.name AS name2,
          s2.ts AS ts2,
          s1.stack_id = s2.stack_id AS same_stack_id
        FROM slice_with_stack_id s1
        JOIN slice_with_stack_id s2
        WHERE s1.name = 'C' AND s2.name = 'C' AND s1.ts < s2.ts;
        """,
        out=Csv("""
        "name1","ts1","name2","ts2","same_stack_id"
        "C",1200,"C",2200,1
        """))

  def test_different_stacks_different_stack_ids(self):
    # Tests that slices with different call stacks get different stack_ids
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet {
          ftrace_events {
            cpu: 0
            event {
              timestamp: 1000
              pid: 10
              print {
                buf: "B|10|A"
              }
            }
            event {
              timestamp: 1100
              pid: 10
              print {
                buf: "B|10|B"
              }
            }
            event {
              timestamp: 1200
              pid: 10
              print {
                buf: "E|10"
              }
            }
            event {
              timestamp: 1300
              pid: 10
              print {
                buf: "E|10"
              }
            }
            # Different stack A->C
            event {
              timestamp: 2000
              pid: 10
              print {
                buf: "B|10|A"
              }
            }
            event {
              timestamp: 2100
              pid: 10
              print {
                buf: "B|10|C"
              }
            }
            event {
              timestamp: 2200
              pid: 10
              print {
                buf: "E|10"
              }
            }
            event {
              timestamp: 2300
              pid: 10
              print {
                buf: "E|10"
              }
            }
          }
        }
        """),
        query="""
        INCLUDE PERFETTO MODULE slices.stack;

        SELECT
          s1.name AS leaf1,
          s2.name AS leaf2,
          s1.stack_id != s2.stack_id AS different_stack_ids
        FROM slice_with_stack_id s1
        JOIN slice_with_stack_id s2
        WHERE s1.name = 'B' AND s2.name = 'C' AND s1.depth = 1 AND s2.depth = 1;
        """,
        out=Csv("""
        "leaf1","leaf2","different_stack_ids"
        "B","C",1
        """))

  def test_parent_child_stack_id_relationship(self):
    # Tests that parent_stack_id correctly points to parent's stack_id
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet {
          ftrace_events {
            cpu: 0
            event {
              timestamp: 1000
              pid: 10
              print {
                buf: "B|10|Parent"
              }
            }
            event {
              timestamp: 1100
              pid: 10
              print {
                buf: "B|10|Child"
              }
            }
            event {
              timestamp: 1200
              pid: 10
              print {
                buf: "E|10"
              }
            }
            event {
              timestamp: 1300
              pid: 10
              print {
                buf: "E|10"
              }
            }
          }
        }
        """),
        query="""
        INCLUDE PERFETTO MODULE slices.stack;

        SELECT
          child.name AS child_name,
          parent.name AS parent_name,
          child.parent_stack_id = parent.stack_id AS correct_parent_stack_id
        FROM slice_with_stack_id child
        JOIN slice_with_stack_id parent ON child.parent_id = parent.id
        WHERE child.name = 'Child';
        """,
        out=Csv("""
        "child_name","parent_name","correct_parent_stack_id"
        "Child","Parent",1
        """))

  def test_depth_zero_slices_parent_stack_id(self):
    # Tests that depth 0 slices have parent_stack_id = 0
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet {
          ftrace_events {
            cpu: 0
            event {
              timestamp: 1000
              pid: 10
              print {
                buf: "B|10|RootSlice"
              }
            }
            event {
              timestamp: 1500
              pid: 10
              print {
                buf: "E|10"
              }
            }
          }
        }
        """),
        query="""
        INCLUDE PERFETTO MODULE slices.stack;

        SELECT
          name,
          depth,
          parent_stack_id
        FROM slice_with_stack_id
        WHERE depth = 0;
        """,
        out=Csv("""
        "name","depth","parent_stack_id"
        "RootSlice",0,0
        """))

  def test_slice_stack_id_computed(self):
    # Tests that stack_id is computed correctly in the view
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet {
          ftrace_events {
            cpu: 0
            event {
              timestamp: 1000
              pid: 10
              print {
                buf: "B|10|A"
              }
            }
            event {
              timestamp: 1100
              pid: 10
              print {
                buf: "B|10|B"
              }
            }
            event {
              timestamp: 1200
              pid: 10
              print {
                buf: "E|10"
              }
            }
            event {
              timestamp: 1300
              pid: 10
              print {
                buf: "E|10"
              }
            }
          }
        }
        """),
        query="""
        INCLUDE PERFETTO MODULE slices.stack;

        SELECT
          name,
          depth,
          stack_id,
          stack_id != 0 AS stack_id_nonzero
        FROM slice_with_stack_id
        ORDER BY depth;
        """,
        out=Csv("""
        "name","depth","stack_id","stack_id_nonzero"
        "A",0,1905114530773834795,1
        "B",1,6901956697539716495,1
        """))

  def test_ancestor_slice_by_stack(self):
    # Tests ancestor_slice_by_stack function
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet {
          ftrace_events {
            cpu: 0
            event {
              timestamp: 1000
              pid: 10
              print {
                buf: "B|10|Root"
              }
            }
            event {
              timestamp: 1100
              pid: 10
              print {
                buf: "B|10|Middle"
              }
            }
            event {
              timestamp: 1200
              pid: 10
              print {
                buf: "B|10|Leaf"
              }
            }
            event {
              timestamp: 1300
              pid: 10
              print {
                buf: "E|10"
              }
            }
            event {
              timestamp: 1400
              pid: 10
              print {
                buf: "E|10"
              }
            }
            event {
              timestamp: 1500
              pid: 10
              print {
                buf: "E|10"
              }
            }
            # Second identical stack
            event {
              timestamp: 2000
              pid: 10
              print {
                buf: "B|10|Root"
              }
            }
            event {
              timestamp: 2100
              pid: 10
              print {
                buf: "B|10|Middle"
              }
            }
            event {
              timestamp: 2200
              pid: 10
              print {
                buf: "B|10|Leaf"
              }
            }
            event {
              timestamp: 2300
              pid: 10
              print {
                buf: "E|10"
              }
            }
            event {
              timestamp: 2400
              pid: 10
              print {
                buf: "E|10"
              }
            }
            event {
              timestamp: 2500
              pid: 10
              print {
                buf: "E|10"
              }
            }
          }
        }
        """),
        query="""
        INCLUDE PERFETTO MODULE slices.stack;

        SELECT name, depth
        FROM ancestor_slice_by_stack((
          SELECT stack_id FROM slice_with_stack_id
          WHERE name = 'Leaf' AND ts = 1200
          LIMIT 1
        ))
        ORDER BY depth;
        """,
        out=Csv("""
        "name","depth"
        "Root",0
        "Root",0
        "Middle",1
        "Middle",1
        "Leaf",2
        "Leaf",2
        """))

  def test_descendant_slice_by_stack(self):
    # Tests descendant_slice_by_stack function
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet {
          ftrace_events {
            cpu: 0
            event {
              timestamp: 1000
              pid: 10
              print {
                buf: "B|10|Root"
              }
            }
            event {
              timestamp: 1100
              pid: 10
              print {
                buf: "B|10|Child1"
              }
            }
            event {
              timestamp: 1200
              pid: 10
              print {
                buf: "E|10"
              }
            }
            event {
              timestamp: 1300
              pid: 10
              print {
                buf: "B|10|Child2"
              }
            }
            event {
              timestamp: 1400
              pid: 10
              print {
                buf: "E|10"
              }
            }
            event {
              timestamp: 1500
              pid: 10
              print {
                buf: "E|10"
              }
            }
            # Second identical root
            event {
              timestamp: 2000
              pid: 10
              print {
                buf: "B|10|Root"
              }
            }
            event {
              timestamp: 2100
              pid: 10
              print {
                buf: "B|10|Child1"
              }
            }
            event {
              timestamp: 2200
              pid: 10
              print {
                buf: "E|10"
              }
            }
            event {
              timestamp: 2300
              pid: 10
              print {
                buf: "E|10"
              }
            }
          }
        }
        """),
        query="""
        INCLUDE PERFETTO MODULE slices.stack;

        SELECT name, ts
        FROM descendant_slice_by_stack((
          SELECT stack_id FROM slice_with_stack_id
          WHERE name = 'Root' AND ts = 1000
          LIMIT 1
        ))
        ORDER BY ts;
        """,
        out=Csv("""
        "name","ts"
        "Root",1000
        "Child1",1100
        "Child2",1300
        "Root",2000
        "Child1",2100
        """))

  def test_stack_with_categories(self):
    # Tests that categories are included in stack hash computation
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet {
          ftrace_events {
            cpu: 0
            event {
              timestamp: 1000
              pid: 10
              print {
                buf: "B|10|A"
              }
            }
            event {
              timestamp: 1500
              pid: 10
              print {
                buf: "E|10"
              }
            }
          }
        }
        packet {
          trusted_packet_sequence_id: 1
          timestamp: 2000
          incremental_state_cleared: true
          track_descriptor {
            uuid: 1
            parent_uuid: 10
            thread {
              pid: 5
              tid: 1
              thread_name: "t1"
            }
          }
          trace_packet_defaults {
            track_event_defaults {
              track_uuid: 1
            }
          }
        }
        packet {
          trusted_packet_sequence_id: 1
          timestamp: 2000
          track_descriptor {
            uuid: 10
            process {
              pid: 5
              process_name: "p1"
            }
          }
        }
        packet {
          trusted_packet_sequence_id: 1
          timestamp: 3000
          track_event {
            categories: "cat1"
            name: "A"
            type: 1
          }
        }
        packet {
          trusted_packet_sequence_id: 1
          timestamp: 3500
          track_event {
            categories: "cat1"
            name: "A"
            type: 2
          }
        }
        """),
        query="""
        INCLUDE PERFETTO MODULE slices.stack;

        SELECT
          s1.category AS cat1,
          s2.category AS cat2,
          s1.name AS name1,
          s2.name AS name2,
          s1.stack_id != s2.stack_id AS different_stacks
        FROM slice_with_stack_id s1
        JOIN slice_with_stack_id s2
        WHERE s1.name = 'A' AND s2.name = 'A'
          AND s1.category IS NULL AND s2.category = 'cat1';
        """,
        out=Csv("""
        "cat1","cat2","name1","name2","different_stacks"
        "[NULL]","cat1","A","A",1
        """))

  def test_multiple_depth_levels_stack_ids(self):
    # Tests that each depth level has correct stack_id and parent_stack_id
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet {
          ftrace_events {
            cpu: 0
            event {
              timestamp: 1000
              pid: 10
              print {
                buf: "B|10|D0"
              }
            }
            event {
              timestamp: 1100
              pid: 10
              print {
                buf: "B|10|D1"
              }
            }
            event {
              timestamp: 1200
              pid: 10
              print {
                buf: "B|10|D2"
              }
            }
            event {
              timestamp: 1300
              pid: 10
              print {
                buf: "B|10|D3"
              }
            }
            event {
              timestamp: 1400
              pid: 10
              print {
                buf: "E|10"
              }
            }
            event {
              timestamp: 1500
              pid: 10
              print {
                buf: "E|10"
              }
            }
            event {
              timestamp: 1600
              pid: 10
              print {
                buf: "E|10"
              }
            }
            event {
              timestamp: 1700
              pid: 10
              print {
                buf: "E|10"
              }
            }
          }
        }
        """),
        query="""
        INCLUDE PERFETTO MODULE slices.stack;

        SELECT
          child.name AS child,
          child.depth AS child_depth,
          parent.name AS parent,
          parent.depth AS parent_depth,
          child.parent_stack_id = parent.stack_id AS valid_relationship
        FROM slice_with_stack_id child
        LEFT JOIN slice_with_stack_id parent ON child.parent_id = parent.id
        ORDER BY child.depth;
        """,
        out=Csv("""
        "child","child_depth","parent","parent_depth","valid_relationship"
        "D0",0,"[NULL]","[NULL]","[NULL]"
        "D1",1,"D0",0,1
        "D2",2,"D1",1,1
        "D3",3,"D2",2,1
        """))

  def test_empty_trace_stack_functions(self):
    # Tests that stack functions work correctly with empty results
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet {
          ftrace_events {
            cpu: 0
            event {
              timestamp: 1000
              pid: 10
              print {
                buf: "B|10|A"
              }
            }
            event {
              timestamp: 1500
              pid: 10
              print {
                buf: "E|10"
              }
            }
          }
        }
        """),
        query="""
        INCLUDE PERFETTO MODULE slices.stack;

        SELECT COUNT(*) AS count
        FROM ancestor_slice_by_stack(999999999);
        """,
        out=Csv("""
        "count"
        0
        """))
