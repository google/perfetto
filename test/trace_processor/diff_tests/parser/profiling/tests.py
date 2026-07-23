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

from python.generators.diff_tests.testing import Path, DataPath, Metric
from python.generators.diff_tests.testing import Csv, Json, TextProto
from python.generators.diff_tests.testing import DiffTestBlueprint
from python.generators.diff_tests.testing import TestSuite


class Profiling(TestSuite):
  # Perf profiling  tests.
  def test_profiler_smaps(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet {
          process_tree {
            processes {
              pid: 1
              ppid: 0
              cmdline: "init"
              uid: 0
            }
            processes {
              pid: 2
              ppid: 1
              cmdline: "system_server"
              uid: 1000
            }
          }
        }
        packet {
          trusted_packet_sequence_id: 999
          timestamp: 10
          smaps_packet {
            pid: 2
            entries {
              path: "/system/lib64/libc.so"
              size_kb: 20
              private_dirty_kb: 4
              swap_kb: 4
            }
            entries {
              path: "[anon: libc_malloc]"
              size_kb: 30
              private_dirty_kb: 10
              swap_kb: 10
            }
          }
        }
        """),
        query="""
        SELECT id, upid, ts, path, size_kb, private_dirty_kb, swap_kb
        FROM profiler_smaps;
        """,
        out=Csv("""
        "id","upid","ts","path","size_kb","private_dirty_kb","swap_kb"
        0,2,10,"/system/lib64/libc.so",20,4,4
        1,2,10,"[anon: libc_malloc]",30,10,10
        """))

  def test_profiler_smaps_metric(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet {
          process_tree {
            processes {
              pid: 1
              ppid: 0
              cmdline: "init"
              uid: 0
            }
            processes {
              pid: 2
              ppid: 1
              cmdline: "system_server"
              uid: 1000
            }
          }
        }
        packet {
          trusted_packet_sequence_id: 999
          timestamp: 10
          smaps_packet {
            pid: 2
            entries {
              path: "/system/lib64/libc.so"
              size_kb: 20
              private_dirty_kb: 4
              swap_kb: 4
            }
            entries {
              path: "[anon: libc_malloc]"
              size_kb: 30
              private_dirty_kb: 10
              swap_kb: 10
            }
          }
        }
        """),
        query=Metric('profiler_smaps'),
        out=TextProto(r"""
        profiler_smaps {
          instance {
            process {
              name: "system_server"
              uid: 1000
              pid: 2
              android_user_id: 0
              is_kernel_task: false
            }
            mappings {
              path: "[anon: libc_malloc]"
              size_kb: 30
              private_dirty_kb: 10
              swap_kb: 10
            }
            mappings {
              path: "/system/lib64/libc.so"
              size_kb: 20
              private_dirty_kb: 4
              swap_kb: 4
            }
          }
        }
        """))

  # Regression test for b/222297079: when cumulative size in a flamegraph
  # a signed 32-bit integer.
  def test_heap_graph_flamegraph_matches_objects(self):
    return DiffTestBlueprint(
        trace=Path('heap_graph_huge_size.textproto'),
        query="""
        SELECT
          obj.upid AS upid,
          obj.graph_sample_ts AS ts,
          SUM(obj.self_size + obj.native_size) AS total_objects_size,
          (
            SELECT SUM(cumulative_size)
            FROM experimental_flamegraph(
              'graph',
              obj.graph_sample_ts,
              NULL,
              obj.upid,
              NULL,
              NULL
            )
            WHERE depth = 0 -- only the roots
          ) AS total_flamegraph_size
        FROM
          heap_graph_object AS obj
        WHERE
          obj.reachable != 0
        GROUP BY obj.upid, obj.graph_sample_ts;
        """,
        out=Csv("""
        "upid","ts","total_objects_size","total_flamegraph_size"
        1,10,3000000036,3000000036
        """))

  # TODO(b/153552977): Stop supporting legacy heap graphs. These never made it
  # a public release, so we should eventually stop supporting workarounds for
  def test_heap_graph_flamegraph(self):
    return DiffTestBlueprint(
        trace=Path('heap_graph_legacy.textproto'),
        query="""
        SELECT
          id,
          depth,
          name,
          map_name,
          count,
          cumulative_count,
          size,
          cumulative_size,
          parent_id
        FROM experimental_flamegraph(
          'graph',
          (SELECT max(graph_sample_ts) FROM heap_graph_object),
          NULL,
          (SELECT max(upid) FROM heap_graph_object),
          NULL,
          NULL
        )
        LIMIT 10;
        """,
        out=Path('heap_graph_flamegraph.out'))

  def test_stack_profile_tracker_empty_callstack(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet {
          clock_snapshot {
            clocks: {
              clock_id: 6 # BOOTTIME
              timestamp: 0
            }
            clocks: {
              clock_id: 4 # MONOTONIC_COARSE
              timestamp: 0
            }
          }
        }

        packet {
          previous_packet_dropped: 1
          incremental_state_cleared: true
          trusted_packet_sequence_id: 1
          timestamp: 0
          interned_data {
            callstacks {
              iid: 1
            }
          }
        }

        packet {
          trusted_packet_sequence_id: 1
          timestamp: 0
          profile_packet {
            index: 0
            continued: false
            process_dumps {
              samples {
                callstack_id: 1
                self_allocated: 1
                alloc_count: 1
              }
              samples {
                callstack_id: 1
                self_allocated: 1
                alloc_count: 1
              }
            }
          }
        }
        """),
        query="""
        SELECT count(1) AS count FROM heap_profile_allocation;
        """,
        out=Csv("""
        "count"
        0
        """))

  # perf_sample table (traced_perf) with android R and S trace inputs.
  def test_perf_sample_rvc(self):
    return DiffTestBlueprint(
        trace=DataPath('perf_sample.pb'),
        query="""
        SELECT ps.ts, ps.cpu, ps.cpu_mode, ps.unwind_error, ps.perf_session_id,
               pct.name AS cntr_name, pct.is_timebase,
               thread.tid,
               spf.name
        FROM perf_sample ps
        JOIN experimental_annotated_callstack(ps.callsite_id) eac
        JOIN perf_counter_track pct
          USING(perf_session_id, cpu)
        JOIN thread
          USING(utid)
        JOIN stack_profile_frame spf
          ON (eac.frame_id = spf.id)
        ORDER BY ps.ts ASC, eac.depth ASC;
        """,
        out=Path('perf_sample_rvc.out'))

  def test_perf_sample_timebase_count(self):
    return DiffTestBlueprint(
        trace=TextProto(R"""
        packet {
          trusted_packet_sequence_id: 1
          incremental_state_cleared: true
          timestamp: 1000
          trace_packet_defaults {
            perf_sample_defaults {
              timebase {
                name: "leader"
                counter: SW_CPU_CLOCK
                frequency: 1000
              }
            }
          }
        }
        packet {
          trusted_packet_sequence_id: 1
          timestamp: 3000
          perf_sample {
            cpu: 0
            pid: 1
            tid: 42
            cpu_mode: MODE_USER
            timebase_count: 512
          }
        }
        packet {
          trusted_packet_sequence_id: 1
          timestamp: 4000
          perf_sample {
            cpu: 0
            pid: 1
            tid: 42
            cpu_mode: MODE_USER
            # Note: No timebase_count set!
          }
        }
        """),
        query="""
        SELECT ts, value FROM counter;
        """,
        out=Csv("""
        "ts","value"
        3000,512.000000
        """))

  def test_perf_sample_counter_only(self):
    return DiffTestBlueprint(
        trace=TextProto(R"""
        packet {
          trusted_packet_sequence_id: 1
          incremental_state_cleared: true
          timestamp: 1000
          trace_packet_defaults {
            perf_sample_defaults {
              timebase {
                name: "leader"
                counter: SW_CPU_CLOCK
                frequency: 1000
              }
            }
          }
        }
        packet {
          trusted_packet_sequence_id: 1
          timestamp: 3000
          perf_sample {
            cpu: 0
            pid: 1
            tid: 42
            timebase_count: 512
            sample_skipped_reason: PROFILER_SKIP_NOT_IN_SCOPE
          }
        }
        """),
        query="""
        -- A counter-only perf sample (no callstack) stays visible in
        -- perf_sample with a null callsite and its counter values reachable
        -- via the counter set; it is excluded from stack_sample.
        SELECT
          ps.ts,
          ps.callsite_id,
          ec.cpu_mode AS intrinsic_cpu_mode,
          ps.cpu_mode AS perf_cpu_mode,
          c.value,
          (SELECT count(*) FROM stack_sample) AS stack_sample_count,
          (SELECT count(*) FROM stack_sample_session)
            AS stack_sample_session_count,
          (SELECT count(*) FROM stack_sample_counter_track)
            AS stack_sample_counter_track_count,
          (SELECT count(*) FROM stack_sample_counter)
            AS stack_sample_counter_count
        FROM perf_sample AS ps
        JOIN __intrinsic_profiler_sample AS psi
          ON psi.id = ps.id
        JOIN __intrinsic_profiler_counter_set AS pcs
          ON psi.counter_set_id = pcs.counter_set_id
        JOIN counter AS c
          ON c.id = pcs.counter_id
        LEFT JOIN __intrinsic_profiler_execution_context AS ec
          ON ec.id = psi.execution_context_id;
        """,
        out=Csv("""
        "ts","callsite_id","intrinsic_cpu_mode","perf_cpu_mode","value","stack_sample_count","stack_sample_session_count","stack_sample_counter_track_count","stack_sample_counter_count"
        3000,"[NULL]","[NULL]","unknown",512.000000,0,0,0,0
        """))

  def test_stack_sample_session_source(self):
    return DiffTestBlueprint(
        trace=Path('stack_sample.textproto'),
        query="""
        -- StackSample streams allocate a profiler session tagged with the
        -- stream's source and the unit of its timebase counter; they must not
        -- leak into the perf_session view.
        SELECT
          (
            SELECT count(*)
            FROM stack_sample_session
            WHERE source = 'python.wall' AND timebase_unit = 'ns'
          ) AS stack_sample_sessions,
          (SELECT count(*) FROM perf_session) AS perf_sessions;
        """,
        out=Csv("""
        "stack_sample_sessions","perf_sessions"
        1,0
        """))

  def test_perf_sample_sc(self):
    return DiffTestBlueprint(
        trace=DataPath('perf_sample_sc.pb'),
        query="""
        SELECT ps.ts, ps.cpu, ps.cpu_mode, ps.unwind_error, ps.perf_session_id,
               pct.name AS cntr_name, pct.is_timebase,
               thread.tid,
               spf.name
        FROM perf_sample ps
        JOIN experimental_annotated_callstack(ps.callsite_id) eac
        JOIN perf_counter_track pct
          USING(perf_session_id, cpu)
        JOIN thread
          USING(utid)
        JOIN stack_profile_frame spf
          ON (eac.frame_id = spf.id)
        ORDER BY ps.ts ASC, eac.depth ASC;
        """,
        out=Path('perf_sample_sc.out'))

  def test_annotations(self):
    return DiffTestBlueprint(
        trace=DataPath('perf_sample_annotations.pftrace'),
        query="""
        select
          eac.depth, eac.annotation, spm.name as map_name,
          ifnull(demangle(spf.name), spf.name) as frame_name
        from experimental_annotated_callstack((
          select spc.id as callsite_id
          from stack_profile_callsite spc
          join stack_profile_frame spf on (spc.frame_id = spf.id)
          where spf.name = "_ZN3art28ResolveFieldWithAccessChecksEPNS_6ThreadEPNS_11ClassLinkerEtPNS_9ArtMethodEbbm")) eac
          join stack_profile_frame spf on (eac.frame_id = spf.id)
          join stack_profile_mapping spm on (spf.mapping = spm.id)
        where depth != 10  -- Skipped because cause symbolization issues on clang vs gcc due to llvm-demangle
        order by depth asc;
        """,
        out=Csv("""
        "depth","annotation","map_name","frame_name"
        0,"[NULL]","/apex/com.android.runtime/lib64/bionic/libc.so","__libc_init"
        1,"[NULL]","/system/bin/app_process64","main"
        2,"[NULL]","/system/lib64/libandroid_runtime.so","android::AndroidRuntime::start(char const*, android::Vector<android::String8> const&, bool)"
        3,"[NULL]","/system/lib64/libandroid_runtime.so","_JNIEnv::CallStaticVoidMethod(_jclass*, _jmethodID*, ...)"
        4,"[NULL]","/apex/com.android.art/lib64/libart.so","art::JNI<true>::CallStaticVoidMethodV(_JNIEnv*, _jclass*, _jmethodID*, std::__va_list)"
        5,"[NULL]","/apex/com.android.art/lib64/libart.so","art::JValue art::InvokeWithVarArgs<_jmethodID*>(art::ScopedObjectAccessAlreadyRunnable const&, _jobject*, _jmethodID*, std::__va_list)"
        6,"[NULL]","/apex/com.android.art/lib64/libart.so","art_quick_invoke_static_stub"
        7,"aot","/system/framework/arm64/boot-framework.oat","com.android.internal.os.ZygoteInit.main"
        8,"aot","/system/framework/arm64/boot-framework.oat","com.android.internal.os.RuntimeInit$MethodAndArgsCaller.run"
        9,"common-frame","/system/framework/arm64/boot.oat","art_jni_trampoline"
        11,"common-frame","/apex/com.android.art/lib64/libart.so","_jobject* art::InvokeMethod<(art::PointerSize)8>(art::ScopedObjectAccessAlreadyRunnable const&, _jobject*, _jobject*, _jobject*, unsigned long)"
        12,"common-frame","/apex/com.android.art/lib64/libart.so","art_quick_invoke_static_stub"
        13,"aot","/system/framework/arm64/boot-framework.oat","android.app.ActivityThread.main"
        14,"aot","/system/framework/arm64/boot-framework.oat","android.os.Looper.loop"
        15,"aot","/system/framework/arm64/boot-framework.oat","android.os.Looper.loopOnce"
        16,"aot","/system/framework/arm64/boot-framework.oat","android.os.Handler.dispatchMessage"
        17,"aot","/system/framework/arm64/boot-framework.oat","android.view.Choreographer$FrameDisplayEventReceiver.run"
        18,"aot","/system/framework/arm64/boot-framework.oat","android.view.Choreographer.doFrame"
        19,"aot","/system/framework/arm64/boot-framework.oat","android.view.Choreographer.doCallbacks"
        20,"aot","/system/framework/arm64/boot-framework.oat","android.view.ViewRootImpl$TraversalRunnable.run"
        21,"aot","/system/framework/arm64/boot-framework.oat","android.view.ViewRootImpl.doTraversal"
        22,"aot","/system/framework/arm64/boot-framework.oat","android.view.ViewRootImpl.performTraversals"
        23,"interp","/system/framework/framework.jar","android.view.ViewRootImpl.notifyDrawStarted"
        24,"common-frame-interp","/apex/com.android.art/lib64/libart.so","nterp_op_iget_object_slow_path"
        25,"common-frame-interp","/apex/com.android.art/lib64/libart.so","nterp_get_instance_field_offset"
        26,"common-frame-interp","/apex/com.android.art/lib64/libart.so","NterpGetInstanceFieldOffset"
        27,"common-frame","/apex/com.android.art/lib64/libart.so","art::ResolveFieldWithAccessChecks(art::Thread*, art::ClassLinker*, unsigned short, art::ArtMethod*, bool, bool, unsigned long)"
        """))

  def test_annotations_switch_interpreter(self):
    return DiffTestBlueprint(
        trace=Path('perf_sample_switch_interp.textproto'),
        query="""
        select
          eac.depth, eac.annotation, spm.name as map_name,
          ifnull(demangle(spf.name), spf.name) as frame_name
        from experimental_annotated_callstack((select callsite_id from perf_sample)) eac
          join stack_profile_frame spf on (eac.frame_id = spf.id)
          join stack_profile_mapping spm on (spf.mapping = spm.id)
        order by depth asc;
        """,
        out=Csv("""
        "depth","annotation","map_name","frame_name"
        0,"interp","/example.vdex","com.example.managed.frame"
        1,"common-frame-interp","/apex/com.android.art/lib64/libart.so","ExecuteSwitchImplAsm"
        2,"common-frame-interp","/apex/com.android.art/lib64/libart.so","void art::interpreter::ExecuteSwitchImplCpp<false>(art::interpreter::SwitchImplContext*)"
        3,"common-frame-interp","/apex/com.android.art/lib64/libart.so","bool art::interpreter::DoCall<true>(art::ArtMethod*, art::Thread*, art::ShadowFrame&, art::Instruction const*, unsigned short, bool, art::JValue*)"
        4,"common-frame","/apex/com.android.art/lib64/libart.so","art::ArtMethod::Invoke(art::Thread*, unsigned int*, unsigned int, art::JValue*, char const*)"
        """))

  def test_perf_sample_followers(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet {
          clock_snapshot {
            primary_trace_clock: BUILTIN_CLOCK_BOOTTIME
            clocks {
              clock_id: 6
              timestamp: 273574904041306
            }
            clocks {
              clock_id: 2
              timestamp: 1737644264730557105
            }
            clocks {
              clock_id: 4
              timestamp: 106208706668231
            }
            clocks {
              clock_id: 1
              timestamp: 1737644264734590878
            }
            clocks {
              clock_id: 3
              timestamp: 106208710702167
            }
            clocks {
              clock_id: 5
              timestamp: 106208710702494
            }
          }
          trusted_packet_sequence_id: 1
        }
        packet {
          first_packet_on_sequence: true
          timestamp: 273574983771490
          timestamp_clock_id: 6
          sequence_flags: 1
          trace_packet_defaults {
            timestamp_clock_id: 3
            perf_sample_defaults {
              timebase {
                frequency: 100
                counter: SW_CPU_CLOCK
              }
              followers {
                counter: HW_CPU_CYCLES
              }
              followers {
                counter: HW_INSTRUCTIONS
              }
            }
          }
          trusted_packet_sequence_id: 4
          previous_packet_dropped: 1
        }
        packet {
          interned_data {
            build_ids {
              iid: 0
              str: ""
            }
            mapping_paths {
              iid: 0
              str: ""
            }
            function_names {
              iid: 0
              str: ""
            }
          }
          sequence_flags: 2
          trusted_packet_sequence_id: 4
        }
        packet {
          sequence_flags: 2
          timestamp: 106208800213886
          interned_data {
            callstacks {
              iid: 1
            }
          }
          perf_sample {
            cpu: 0
            pid: 0
            tid: 0
            cpu_mode: MODE_KERNEL
            timebase_count: 10020141
            follower_counts: 4672142
            follower_counts: 1144537
            callstack_iid: 1
          }
          trusted_packet_sequence_id: 4
        }
        """),
        query="""
        select
          c.ts,
          c.value,
          pct.cpu,
          pct.perf_session_id,
          pct.is_timebase
        from
          counter c join perf_counter_track pct on c.track_id = pct.id
        order by ts, c.id
        """,
        out=Csv("""
        "ts","value","cpu","perf_session_id","is_timebase"
        273574993553025,10020141.000000,0,0,1
        273574993553025,4672142.000000,0,0,0
        273574993553025,1144537.000000,0,0,0
        """))

  def test_perf_sample_counter_set(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet {
          clock_snapshot {
            primary_trace_clock: BUILTIN_CLOCK_BOOTTIME
            clocks {
              clock_id: 6
              timestamp: 273574904041306
            }
            clocks {
              clock_id: 2
              timestamp: 1737644264730557105
            }
            clocks {
              clock_id: 4
              timestamp: 106208706668231
            }
            clocks {
              clock_id: 1
              timestamp: 1737644264734590878
            }
            clocks {
              clock_id: 3
              timestamp: 106208710702167
            }
            clocks {
              clock_id: 5
              timestamp: 106208710702494
            }
          }
          trusted_packet_sequence_id: 1
        }
        packet {
          first_packet_on_sequence: true
          timestamp: 273574983771490
          timestamp_clock_id: 6
          sequence_flags: 1
          trace_packet_defaults {
            timestamp_clock_id: 3
            perf_sample_defaults {
              timebase {
                frequency: 100
                counter: SW_CPU_CLOCK
              }
              followers {
                counter: HW_CPU_CYCLES
              }
              followers {
                counter: HW_INSTRUCTIONS
              }
            }
          }
          trusted_packet_sequence_id: 4
          previous_packet_dropped: 1
        }
        packet {
          interned_data {
            build_ids {
              iid: 0
              str: ""
            }
            mapping_paths {
              iid: 0
              str: ""
            }
            function_names {
              iid: 0
              str: ""
            }
          }
          sequence_flags: 2
          trusted_packet_sequence_id: 4
        }
        packet {
          sequence_flags: 2
          timestamp: 106208800213886
          interned_data {
            callstacks {
              iid: 1
            }
          }
          perf_sample {
            cpu: 0
            pid: 0
            tid: 0
            cpu_mode: MODE_KERNEL
            timebase_count: 10020141
            follower_counts: 4672142
            follower_counts: 1144537
            callstack_iid: 1
          }
          trusted_packet_sequence_id: 4
        }
        """),
        query="""
        -- Test the counter_set_id on profiler_sample and the
        -- __intrinsic_profiler_counter_set table.
        select
          ps.id as sample_id,
          psi.counter_set_id,
          pcs.counter_id,
          c.value
        from
          perf_sample ps
          join __intrinsic_profiler_sample psi on psi.id = ps.id
          join __intrinsic_profiler_counter_set pcs
            on psi.counter_set_id = pcs.counter_set_id
          join counter c on c.id = pcs.counter_id
        order by ps.id, pcs.counter_id
        """,
        out=Csv("""
        "sample_id","counter_set_id","counter_id","value"
        0,0,0,10020141.000000
        0,0,1,4672142.000000
        0,0,2,1144537.000000
        """))

  def test_stack_sample(self):
    return DiffTestBlueprint(
        trace=Path('stack_sample.textproto'),
        query="""
        SELECT
          ss.ts,
          ec.ucpu AS cpu,
          ec.cpu_mode AS mode,
          CAST(c.value AS INTEGER) AS weight,
          ss.source,
          ct.name AS timebase_name,
          ct.unit,
          spf.name AS frame_name
        FROM stack_sample ss
        JOIN stack_sample_counter c ON c.stack_sample_id = ss.id
        JOIN stack_sample_counter_track ct
          ON c.track_id = ct.id AND ct.name = 'wall-time'
        JOIN stack_profile_callsite spc ON ss.callsite_id = spc.id
        JOIN stack_profile_frame spf ON spc.frame_id = spf.id
        LEFT JOIN stack_sample_execution_context ec
          ON ec.id = ss.execution_context_id
        ORDER BY ss.ts;
        """,
        out=Csv("""
        "ts","cpu","mode","weight","source","timebase_name","unit","frame_name"
        1000,2,"user",1000000,"python.wall","wall-time","ns","foo"
        2000,2,"kernel",2000000,"python.wall","wall-time","ns","foo"
        6000,"[NULL]","[NULL]",4000000,"python.wall","wall-time","ns","foo"
        7000,"[NULL]","[NULL]",7000000,"python.wall","wall-time","ns","foo"
        """))

  def test_stack_sample_contexts(self):
    return DiffTestBlueprint(
        trace=Path('stack_sample.textproto'),
        query="""
        -- The ts=1000 sample is attributed to a task and execution context;
        -- the ts=7000 sample has neither, so its context columns are unset.
        SELECT
          ss.ts,
          p.name AS process_name,
          ec.ucpu AS cpu,
          ec.cpu_mode AS mode
        FROM stack_sample ss
        LEFT JOIN stack_sample_task_context tc
          ON tc.id = ss.task_context_id
        LEFT JOIN stack_sample_execution_context ec
          ON ec.id = ss.execution_context_id
        LEFT JOIN process p ON tc.upid = p.upid
        ORDER BY ss.ts;
        """,
        out=Csv("""
        "ts","process_name","cpu","mode"
        1000,"myproc",2,"user"
        2000,"myproc",2,"kernel"
        6000,"myproc","[NULL]","[NULL]"
        7000,"[NULL]","[NULL]","[NULL]"
        """))

  def test_stack_sample_interned_callstack(self):
    return DiffTestBlueprint(
        trace=Path('stack_sample.textproto'),
        query="""
        -- The ts=2000 sample references its callstack via callstack_iid.
        SELECT
          ss.ts,
          spf.name AS frame_name
        FROM stack_sample ss
        JOIN stack_profile_callsite spc ON ss.callsite_id = spc.id
        JOIN stack_profile_frame spf ON spc.frame_id = spf.id
        WHERE ss.ts = 2000;
        """,
        out=Csv("""
        "ts","frame_name"
        2000,"foo"
        """))

  def test_stack_sample_inline_callstack(self):
    return DiffTestBlueprint(
        trace=Path('stack_sample.textproto'),
        query="""
        -- The ts=1000 sample carries a fully-inline callstack: its frames are
        -- interned from function name + source location alone.
        WITH RECURSIVE cs AS (
          SELECT spc.id, spc.parent_id, spc.depth, spc.frame_id
          FROM stack_sample ss
          JOIN stack_profile_callsite spc ON ss.callsite_id = spc.id
          WHERE ss.ts = 1000
          UNION ALL
          SELECT p.id, p.parent_id, p.depth, p.frame_id
          FROM stack_profile_callsite p
          JOIN cs ON cs.parent_id = p.id
        )
        SELECT cs.depth, spf.name AS frame_name, sym.source_file,
          sym.line_number
        FROM cs
        JOIN stack_profile_frame spf ON cs.frame_id = spf.id
        LEFT JOIN stack_profile_symbol sym
          ON spf.symbol_set_id = sym.symbol_set_id
        ORDER BY cs.depth;
        """,
        out=Csv("""
        "depth","frame_name","source_file","line_number"
        0,"main","/src/main.py",10
        1,"foo","/src/foo.py",42
        """))

  def test_stack_sample_async(self):
    return DiffTestBlueprint(
        trace=Path('stack_sample.textproto'),
        query="""
        -- The ts=6000 sample is attributed to async context 7, whose parent
        -- is async context 6.
        SELECT
          ss.ts,
          p.name AS process_name,
          ac.name AS async_name,
          ac.kind AS async_kind,
          parent.name AS parent_name
        FROM stack_sample ss
        JOIN stack_sample_task_context tc
          ON tc.id = ss.task_context_id
        JOIN stack_sample_async_context ac
          ON ac.id = tc.async_context_id
        LEFT JOIN stack_sample_async_context parent
          ON parent.id = ac.parent_id
        LEFT JOIN process p ON tc.upid = p.upid
        ORDER BY ss.ts;
        """,
        out=Csv("""
        "ts","process_name","async_name","async_kind","parent_name"
        6000,"myproc","worker-1","fiber","worker-pool"
        """))

  def test_stack_sample_followers(self):
    return DiffTestBlueprint(
        trace=Path('stack_sample.textproto'),
        query="""
        -- The ts=1000 sample has one follower ("instructions") with value 500;
        -- follower values are counter rows on their own counter track, linked
        -- to the sample via its counter set.
        SELECT
          ss.ts,
          ct.name AS counter_name,
          ct.unit,
          CAST(c.value AS INTEGER) AS weight
        FROM stack_sample ss
        JOIN stack_sample_counter c ON c.stack_sample_id = ss.id
        JOIN stack_sample_counter_track ct ON c.track_id = ct.id
        WHERE ct.name = 'instructions'
        ORDER BY ss.ts;
        """,
        out=Csv("""
        "ts","counter_name","unit","weight"
        1000,"instructions","instructions",500
        """))

  def test_stack_sample_cpu_profiling_samples(self):
    return DiffTestBlueprint(
        trace=Path('stack_sample.textproto'),
        query="""
        INCLUDE PERFETTO MODULE stacks.cpu_profiling;

        SELECT source, count(*) AS cnt
        FROM cpu_profiling_samples
        GROUP BY source;
        """,
        out=Csv("""
        "source","cnt"
        "python.wall",4
        """))

  def test_cpu_profiling_samples_timebase_filter(self):
    return DiffTestBlueprint(
        trace=TextProto(R"""
        packet {
          trusted_packet_sequence_id: 1
          incremental_state_cleared: true
          timestamp: 1000
          trace_packet_defaults {
            perf_sample_defaults {
              timebase {
                counter: SW_CPU_CLOCK
                frequency: 100
              }
            }
          }
        }
        packet {
          trusted_packet_sequence_id: 1
          sequence_flags: 2
          timestamp: 2000
          interned_data {
            build_ids { iid: 1 str: "" }
            mapping_paths { iid: 1 str: "libfoo.so" }
            mappings { iid: 1 build_id: 1 path_string_ids: 1 }
            function_names { iid: 1 str: "on_cpu_func" }
            frames { iid: 1 mapping_id: 1 function_name_id: 1 }
            callstacks { iid: 1 frame_ids: 1 }
          }
          perf_sample {
            cpu: 0
            pid: 10
            tid: 10
            cpu_mode: MODE_USER
            callstack_iid: 1
          }
        }
        packet {
          trusted_packet_sequence_id: 2
          incremental_state_cleared: true
          timestamp: 1000
          trace_packet_defaults {
            perf_sample_defaults {
              timebase {
                counter: SW_PAGE_FAULTS
                frequency: 100
              }
            }
          }
        }
        packet {
          trusted_packet_sequence_id: 2
          sequence_flags: 2
          timestamp: 3000
          interned_data {
            build_ids { iid: 1 str: "" }
            mapping_paths { iid: 1 str: "libfoo.so" }
            mappings { iid: 1 build_id: 1 path_string_ids: 1 }
            function_names { iid: 1 str: "fault_func" }
            frames { iid: 1 mapping_id: 1 function_name_id: 1 }
            callstacks { iid: 1 frame_ids: 1 }
          }
          perf_sample {
            cpu: 1
            pid: 10
            tid: 10
            cpu_mode: MODE_USER
            callstack_iid: 1
          }
        }
        """),
        query="""
        INCLUDE PERFETTO MODULE stacks.cpu_profiling;

        -- Two perf sessions: one sampling on cpu-clock, one on page faults.
        -- Both capture callstacks, but only the cpu-clock session's samples
        -- are CPU profiling.
        SELECT
          (SELECT count(*) FROM stack_sample) AS stack_samples,
          (SELECT count(*) FROM cpu_profiling_samples) AS profiling_samples,
          (SELECT ts FROM cpu_profiling_samples) AS profiling_ts,
          (
            SELECT count(*) FROM stack_sample_session WHERE timebase_unit = 'ns'
          ) AS ns_sessions,
          (
            SELECT count(*) FROM stack_sample_session WHERE timebase_unit = 'count'
          ) AS count_sessions;
        """,
        out=Csv("""
        "stack_samples","profiling_samples","profiling_ts","ns_sessions","count_sessions"
        2,1,2000,1,1
        """))

  def test_cpu_profiling_samples_counter_only_excluded(self):
    return DiffTestBlueprint(
        trace=TextProto(R"""
        packet {
          trusted_packet_sequence_id: 1
          incremental_state_cleared: true
          timestamp: 1000
          trace_packet_defaults {
            perf_sample_defaults {
              timebase {
                name: "leader"
                counter: SW_CPU_CLOCK
                frequency: 1000
              }
            }
          }
        }
        packet {
          trusted_packet_sequence_id: 1
          timestamp: 3000
          perf_sample {
            cpu: 0
            pid: 1
            tid: 42
            cpu_mode: MODE_USER
            timebase_count: 512
            sample_skipped_reason: PROFILER_SKIP_NOT_IN_SCOPE
          }
        }
        """),
        query="""
        INCLUDE PERFETTO MODULE stacks.cpu_profiling;

        -- Counter-only perf samples have no callstack: they show up in
        -- perf_sample but not in cpu_profiling_samples.
        SELECT
          (SELECT count(*) FROM perf_sample) AS perf_samples,
          (SELECT count(*) FROM cpu_profiling_samples) AS profiling_samples;
        """,
        out=Csv("""
        "perf_samples","profiling_samples"
        1,0
        """))

  def test_frame_types(self):
    return DiffTestBlueprint(
        trace=Path('frame_types.textproto'),
        query="""
        -- Frame.kind (well-known enum) and Frame.kind_str (custom label) are
        -- parsed into stack_profile_frame.type; frames with no kind are NULL.
        SELECT name, type
        FROM stack_profile_frame
        ORDER BY name;
        """,
        out=Csv("""
        "name","type"
        "custom_fn","custom"
        "gc_fn","gc"
        "interp_fn","interpreted"
        "plain_fn","[NULL]"
        """))

  def test_art_oome_stack_sample(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet {
          timestamp: 1234567890123456
          trusted_packet_sequence_id: 1
          art_process_metadata {
            pid: 12345
            uid: 10155
            process_name: "com.example.oometest"
            oom_allocation_size: 1048576
            oom_total_bytes_free: 512000
            oom_free_bytes_until_oom: 204800
            oom_error_msg: "Failed to allocate 1048576 bytes"
            oom_thread_java_stack {
              frames {
                method_name: "com.example.oometest.MainActivity.oomeInducer"
                source_file: "MainActivity.java"
                line_number: 45
              }
              frames {
                method_name: "com.example.oometest.MainActivity.oomeInducer"
                source_file: "MainActivity.java"
                line_number: 42
              }
              frames {
                method_name: "com.example.oometest.MainActivity.triggerOOM"
                source_file: "MainActivity.java"
                line_number: 31
              }
            }
          }
        }
        """),
        query="""
        INCLUDE PERFETTO MODULE android.memory.heap_graph.oome;

        SELECT
          g.ts,
          p.name AS process_name,
          g.dump_reason,
          o.allocation_size_bytes,
          o.total_bytes_free,
          o.free_bytes_until_oom,
          o.error_msg
        FROM heap_graph g
        JOIN android_heap_graph_java_oome_details o ON o.heap_graph_id = g.id
        JOIN process p ON g.upid = p.upid;
        """,
        out=Csv("""
        "ts","process_name","dump_reason","allocation_size_bytes","total_bytes_free","free_bytes_until_oom","error_msg"
        1234567890123456,"com.example.oometest","OOME",1048576,512000,204800,"Failed to allocate 1048576 bytes"
        """))

  def test_art_oome_stack_sample_callstack(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet {
          timestamp: 1234567890123456
          trusted_packet_sequence_id: 1
          art_process_metadata {
            pid: 12345
            uid: 10155
            process_name: "com.example.oometest"
            oom_allocation_size: 1048576
            oom_total_bytes_free: 512000
            oom_free_bytes_until_oom: 204800
            oom_error_msg: "Failed to allocate 1048576 bytes"
            oom_thread_java_stack {
              frames {
                method_name: "void com.example.oometest.MainActivity.oomeInducer(int, java.lang.String)"
                source_file: "MainActivity.java"
                line_number: 45
              }
              frames {
                method_name: "com.example.oometest.MainActivity.oomeInducer(double)"
                source_file: "MainActivity.java"
                line_number: 42
              }
              frames {
                method_name: "void com.example.oometest.MainActivity.triggerOOM"
                source_file: "MainActivity.java"
                line_number: 31
              }
            }
          }
        }
        """),
        query="""
        WITH RECURSIVE callstack AS (
          SELECT c.id, c.parent_id, c.depth, c.frame_id
          FROM stack_profile_callsite c
          WHERE c.id = (SELECT callsite_id FROM heap_graph_thread_callsite LIMIT 1)
          
          UNION ALL
          
          SELECT c.id, c.parent_id, c.depth, c.frame_id
          FROM stack_profile_callsite c
          JOIN callstack ON c.id = callstack.parent_id
        )
        SELECT
          cs.depth,
          f.name AS frame_name,
          s.source_file,
          s.line_number
        FROM callstack cs
        JOIN stack_profile_frame f ON cs.frame_id = f.id
        LEFT JOIN stack_profile_symbol s ON f.symbol_set_id = s.symbol_set_id
        ORDER BY cs.depth ASC;
        """,
        out=Csv("""
        "depth","frame_name","source_file","line_number"
        0,"com.example.oometest.MainActivity.triggerOOM","MainActivity.java",31
        1,"com.example.oometest.MainActivity.oomeInducer","MainActivity.java",42
        2,"com.example.oometest.MainActivity.oomeInducer","MainActivity.java",45
        """))

  def test_art_oome_heap_graph(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet {
          process_tree {
            processes {
              pid: 2
              ppid: 1
              cmdline: "com.example.oometest"
              uid: 1000
            }
          }
        }
        packet {
          timestamp: 10
          trusted_packet_sequence_id: 1
          art_process_metadata {
            pid: 2
            uid: 1000
            process_name: "com.example.oometest"
            oom_allocation_size: 100
            oom_total_bytes_free: 200
            oom_free_bytes_until_oom: 300
            oom_error_msg: "OOM"
          }
        }
        packet {
          timestamp: 10
          trusted_packet_sequence_id: 2
          [com.android.art.tracing.ArtHeapGraphTracePacket.heap_graph] {
            pid: 2
            heap_bytes_allocated: 100000
            types {
              id: 1
              class_name: "java.lang.Object"
            }
            objects {
              id: 1
              type_id: 1
              self_size: 64
            }
            continued: false
            index: 0
          }
        }
        """),
        query="""
        SELECT ts, dump_reason, heap_size
        FROM heap_graph;
        """,
        out=Csv("""
        "ts","dump_reason","heap_size"
        10,"OOME",100000
        """))

  def test_art_process_metadata_non_oome(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet {
          process_tree {
            processes {
              pid: 2
              ppid: 1
              cmdline: "com.example.normaldump"
              uid: 1000
            }
          }
        }
        packet {
          timestamp: 10
          trusted_packet_sequence_id: 1
          art_process_metadata {
            pid: 2
            uid: 1000
            process_name: "com.example.normaldump"
          }
        }
        packet {
          timestamp: 10
          trusted_packet_sequence_id: 2
          [com.android.art.tracing.ArtHeapGraphTracePacket.heap_graph] {
            pid: 2
            heap_bytes_allocated: 100000
            types {
              id: 1
              class_name: "java.lang.Object"
            }
            objects {
              id: 1
              type_id: 1
              self_size: 64
            }
            continued: false
            index: 0
          }
        }
        """),
        query="""
        SELECT ts, dump_reason, heap_size
        FROM heap_graph;
        """,
        out=Csv("""
        "ts","dump_reason","heap_size"
        10,"[NULL]",100000
        """))
