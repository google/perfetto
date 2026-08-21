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

# NOTE: these tests only assert the diagnostic `name`, deliberately not the
# confidence, description or remediation, as those are expected to be tuned over
# time.

from python.generators.diff_tests.testing import Csv, TextProto
from python.generators.diff_tests.testing import DiffTestBlueprint
from python.generators.diff_tests.testing import TestSuite


class TraceDiagnostics(TestSuite):
  # A config that trips several rules at once: a tiny buffer, an extreme drain
  # period (hence very low drain bandwidth), and unfiltered syscall tracing.
  def test_bad_ftrace_config(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet {
          trace_config {
            data_sources {
              config {
                name: "linux.ftrace"
                ftrace_config {
                  ftrace_events: "raw_syscalls/sys_enter"
                  ftrace_events: "raw_syscalls/sys_exit"
                  buffer_size_kb: 32
                  drain_period_ms: 100000
                }
              }
            }
          }
        }
        """),
        query="""
        SELECT key FROM __intrinsic_trace_diagnostics ORDER BY key;
        """,
        out=Csv("""
        "key"
        "extreme_ftrace_drain_period"
        "low_ftrace_drain_bandwidth"
        "syscalls_without_filter"
        "tiny_ftrace_buffer"
        """))

  # A well-formed config trips nothing.
  def test_good_ftrace_config(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet {
          trace_config {
            data_sources {
              config {
                name: "linux.ftrace"
                ftrace_config {
                  ftrace_events: "raw_syscalls/sys_enter"
                  buffer_size_kb: 8192
                  drain_period_ms: 250
                  syscall_events: "sys_read"
                }
              }
            }
          }
        }
        """),
        query="""
        SELECT count(*) AS n FROM __intrinsic_trace_diagnostics;
        """,
        out=Csv("""
        "n"
        0
        """))

  # A small buffer trips tiny_ftrace_buffer.
  def test_tiny_buffer(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet {
          trace_config {
            data_sources {
              config {
                name: "linux.ftrace"
                ftrace_config {
                  buffer_size_kb: 544
                }
              }
            }
          }
        }
        """),
        query="""
        SELECT key FROM __intrinsic_trace_diagnostics;
        """,
        out=Csv("""
        "key"
        "tiny_ftrace_buffer"
        """))

  # Exercises the recorded-data-loss path (a lost_events bundle) for a buffer
  # diagnostic.
  def test_tiny_buffer_with_data_loss(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet {
          trace_config {
            data_sources {
              config {
                name: "linux.ftrace"
                ftrace_config {
                  buffer_size_kb: 544
                }
              }
            }
          }
        }
        packet {
          ftrace_events {
            cpu: 0
            lost_events: true
          }
        }
        """),
        query="""
        SELECT key FROM __intrinsic_trace_diagnostics;
        """,
        out=Csv("""
        "key"
        "tiny_ftrace_buffer"
        """))

  # A low buffer/drain-period ratio trips low_ftrace_drain_bandwidth.
  def test_low_drain_bandwidth(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet {
          trace_config {
            data_sources {
              config {
                name: "linux.ftrace"
                ftrace_config {
                  buffer_size_kb: 3072
                  drain_period_ms: 1000
                }
              }
            }
          }
        }
        """),
        query="""
        SELECT key FROM __intrinsic_trace_diagnostics;
        """,
        out=Csv("""
        "key"
        "low_ftrace_drain_bandwidth"
        """))

  # enable_function_graph without symbolize_ksyms is a hard misconfiguration.
  def test_function_graph_requires_symbolize_ksyms(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet {
          trace_config {
            data_sources {
              config {
                name: "linux.ftrace"
                ftrace_config {
                  enable_function_graph: true
                  function_graph_roots: "__schedule"
                  buffer_size_kb: 8192
                }
              }
            }
          }
        }
        """),
        query="""
        SELECT key FROM __intrinsic_trace_diagnostics;
        """,
        out=Csv("""
        "key"
        "function_graph_requires_symbolize_ksyms"
        """))

  # An event whose payload is a kernel symbol address without symbolize_ksyms
  # warns.
  def test_events_require_symbolize_ksyms(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet {
          trace_config {
            data_sources {
              config {
                name: "linux.ftrace"
                ftrace_config {
                  ftrace_events: "workqueue/workqueue_execute_start"
                  buffer_size_kb: 8192
                }
              }
            }
          }
        }
        """),
        query="""
        SELECT key FROM __intrinsic_trace_diagnostics;
        """,
        out=Csv("""
        "key"
        "events_require_symbolize_ksyms"
        """))

  # ...but setting symbolize_ksyms suppresses it.
  def test_events_symbolize_ksyms_present(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet {
          trace_config {
            data_sources {
              config {
                name: "linux.ftrace"
                ftrace_config {
                  ftrace_events: "workqueue/workqueue_execute_start"
                  buffer_size_kb: 8192
                  symbolize_ksyms: true
                }
              }
            }
          }
        }
        """),
        query="""
        SELECT count(*) AS n FROM __intrinsic_trace_diagnostics;
        """,
        out=Csv("""
        "n"
        0
        """))

  # Rules iterate over every ftrace data source: only the second is bad here.
  def test_bad_second_ftrace_data_source(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet {
          trace_config {
            data_sources {
              config {
                name: "linux.ftrace"
                ftrace_config {
                  buffer_size_kb: 8192
                }
              }
            }
            data_sources {
              config {
                name: "linux.ftrace"
                ftrace_config {
                  buffer_size_kb: 16
                }
              }
            }
          }
        }
        """),
        query="""
        SELECT key FROM __intrinsic_trace_diagnostics ORDER BY key;
        """,
        out=Csv("""
        "key"
        "tiny_ftrace_buffer"
        """))

  # A DISCARD buffer backing ftrace while streaming into a file warns.
  def test_discard_buffer_for_streaming_ftrace(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet {
          trace_config {
            write_into_file: true
            file_write_period_ms: 1000
            buffers {
              size_kb: 2048
              fill_policy: DISCARD
            }
            data_sources {
              config {
                name: "linux.ftrace"
                target_buffer: 0
                ftrace_config {
                  buffer_size_kb: 8192
                }
              }
            }
          }
        }
        """),
        query="""
        SELECT key FROM __intrinsic_trace_diagnostics;
        """,
        out=Csv("""
        "key"
        "discard_buffer_for_streaming"
        """))

  # A DISCARD buffer used only by a one-shot data source (not ftrace/track_event)
  # is fine and must not warn.
  def test_discard_buffer_one_shot_source_ok(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet {
          trace_config {
            write_into_file: true
            file_write_period_ms: 1000
            buffers {
              size_kb: 2048
              fill_policy: DISCARD
            }
            data_sources {
              config {
                name: "android.packages_list"
                target_buffer: 0
              }
            }
          }
        }
        """),
        query="""
        SELECT count(*) AS n FROM __intrinsic_trace_diagnostics;
        """,
        out=Csv("""
        "n"
        0
        """))

  # A heapprofd sampling_interval_bytes below 100 KB warns.
  def test_heapprofd_sampling_interval_too_low(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet {
          trace_config {
            data_sources {
              config {
                name: "android.heapprofd"
                heapprofd_config {
                  sampling_interval_bytes: 4096
                }
              }
            }
          }
        }
        """),
        query="""
        SELECT key FROM __intrinsic_trace_diagnostics;
        """,
        out=Csv("""
        "key"
        "heapprofd_sampling_interval_too_low"
        """))

  # A reasonable heapprofd sampling interval does not warn.
  def test_heapprofd_sampling_interval_ok(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet {
          trace_config {
            data_sources {
              config {
                name: "android.heapprofd"
                heapprofd_config {
                  sampling_interval_bytes: 1048576
                }
              }
            }
          }
        }
        """),
        query="""
        SELECT count(*) AS n FROM __intrinsic_trace_diagnostics;
        """,
        out=Csv("""
        "n"
        0
        """))

  # android.display.video configured on a user build, but no frames captured
  # and no producer error: trips display_video_not_enabled (the sysprop hint).
  def test_display_video_not_enabled_on_user_build(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet {
          system_info {
            android_build_fingerprint: "google/x/x:14/AB/1:user/release-keys"
          }
        }
        packet {
          trace_config {
            data_sources {
              config {
                name: "android.display.video"
              }
            }
          }
        }
        """),
        query="""
        SELECT key FROM __intrinsic_trace_diagnostics;
        """,
        out=Csv("""
        "key"
        "display_video_not_enabled"
        """))

  # Same config on a userdebug build, where display video works out of the box:
  # the rule must not fire.
  def test_display_video_userdebug_ok(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet {
          system_info {
            android_build_fingerprint: "google/x/x:14/AB/1:userdebug/dev-keys"
          }
        }
        packet {
          trace_config {
            data_sources {
              config {
                name: "android.display.video"
              }
            }
          }
        }
        """),
        query="""
        SELECT count(*) AS n FROM __intrinsic_trace_diagnostics;
        """,
        out=Csv("""
        "n"
        0
        """))

  # preserve_ftrace_buffer is set but there is no tracing_started_ns metadata to
  # tell how long after boot tracing started. The rule must degrade gracefully.
  def test_preserve_ftrace_buffer_without_clock(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet {
          trace_config {
            data_sources {
              config {
                name: "linux.ftrace"
                ftrace_config {
                  buffer_size_kb: 8192
                  drain_period_ms: 250
                  preserve_ftrace_buffer: true
                }
              }
            }
          }
        }
        """),
        query="""
        SELECT count(*) AS n FROM __intrinsic_trace_diagnostics;
        """,
        out=Csv("""
        "n"
        0
        """))
