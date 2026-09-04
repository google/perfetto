from python.generators.diff_tests.testing import Csv, DiffTestBlueprint, TestSuite, TextProto


class Codec(TestSuite):

  def test_codec_events(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet {
          trusted_packet_sequence_id: 1
          track_descriptor { 
            uuid: 1 
            name: "codec.track.state.c2.google.av1.decoder.1" 
            process { pid: 1000 }
          }
        }
        packet {
          trusted_packet_sequence_id: 1
          track_event { track_uuid: 1 name: "event=Configured metadata=inputFormat info=detail pid=2000 uid=10001 render=true intervalMs=10 count=5 ctr=100" type: 3 }
          timestamp: 1000
        }
        """),
        query="""
        INCLUDE PERFETTO MODULE android.codec;
        SELECT ts, track_name, track_event_type, codec_name, unique_no, event, metadata, info, pid, uid, render, interval_ms, count, ctr
        FROM android_codec_events;
        """,
        out=Csv("""
        "ts","track_name","track_event_type","codec_name","unique_no","event","metadata","info","pid","uid","render","interval_ms","count","ctr"
        1000,"codec.track.state.c2.google.av1.decoder.1","state","c2.google.av1.decoder","1","Configured","inputFormat","detail",2000,10001,"true",10,5,100
        """))
