from python.generators.diff_tests.testing import Csv, DiffTestBlueprint, TestSuite, TextProto


class AppWakelocks(TestSuite):

  def test_app_wakelocks_sdk_priority(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet {
          trusted_packet_sequence_id: 1
          timestamp: 1000
          track_event {
            type: 1 # TYPE_SLICE_BEGIN
            track_uuid: 1
            name: "my_wakelock"
          }
        }
        packet {
          trusted_packet_sequence_id: 1
          timestamp: 5000
          track_event {
            type: 2 # TYPE_SLICE_END
            track_uuid: 1
          }
        }
        packet {
          trusted_packet_sequence_id: 1
          track_descriptor {
            uuid: 1
            name: "app_wakelock_events"
          }
        }
        """),
        query="""
        INCLUDE PERFETTO MODULE android.app_wakelocks;

        SELECT ts, dur, name FROM android_app_wakelocks;
        """,
        out=Csv("""
        "ts","dur","name"
        1000,4000,"my_wakelock"
        """))

  def test_app_wakelocks_batterystats_fallback(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet {
          ftrace_events {
            cpu: 0
            event {
              timestamp: 1000
              pid: 1
              print {
                buf: "N|1000|battery_stats.longwake|+longwake=10001:\"my_wakelock\"\n"
              }
            }
            event {
              timestamp: 5000
              pid: 1
              print {
                buf: "N|1000|battery_stats.longwake|-longwake=10001:\"my_wakelock\"\n"
              }
            }
          }
        }
        """),
        query="""
        INCLUDE PERFETTO MODULE android.app_wakelocks;

        SELECT ts, dur, name, uid FROM android_app_wakelocks;
        """,
        out=Csv("""
        "ts","dur","name","uid"
        1000,4000,"my_wakelock",10001
        """))

  def test_app_wakelocks_both_sources_present(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet {
          trusted_packet_sequence_id: 1
          timestamp: 1000
          track_event {
            type: 1 # TYPE_SLICE_BEGIN
            track_uuid: 1
            name: "my_wakelock"
          }
        }
        packet {
          trusted_packet_sequence_id: 1
          timestamp: 5000
          track_event {
            type: 2 # TYPE_SLICE_END
            track_uuid: 1
          }
        }
        packet {
          trusted_packet_sequence_id: 1
          track_descriptor {
            uuid: 1
            name: "app_wakelock_events"
          }
        }
        packet {
          ftrace_events {
            cpu: 0
            event {
              timestamp: 2000
              pid: 1
              print {
                buf: "N|1000|battery_stats.longwake|+longwake=10001:\"my_battery_wakelock\"\n"
              }
            }
            event {
              timestamp: 6000
              pid: 1
              print {
                buf: "N|1000|battery_stats.longwake|-longwake=10001:\"my_battery_wakelock\"\n"
              }
            }
          }
        }
        """),
        query="""
        INCLUDE PERFETTO MODULE android.app_wakelocks;

        SELECT ts, dur, name FROM android_app_wakelocks;
        """,
        out=Csv("""
        "ts","dur","name"
        1000,4000,"my_wakelock"
        """))
