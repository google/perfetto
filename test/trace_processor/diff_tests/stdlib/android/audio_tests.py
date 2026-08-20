from python.generators.diff_tests.testing import Csv, DiffTestBlueprint, TestSuite, TextProto


class Audio(TestSuite):

  def test_audio_track_state(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet {
          trusted_packet_sequence_id: 1
          track_descriptor { 
            uuid: 1 
            name: "audio.track.interval.1" 
            process { pid: 1000 }
          }
        }
        packet {
          trusted_packet_sequence_id: 1
          track_event { track_uuid: 1 name: "event=beginInterval uid=10001 pid=2000 contentType=MUSIC usage=MEDIA devices=SPEAKER flags=0 format=AUDIO_FORMAT_PCM_16_BIT sampleRate=48000" type: 3 }
          timestamp: 1000
        }
        packet {
          trusted_packet_sequence_id: 1
          track_event { track_uuid: 1 name: "event=endInterval uid=10001 pid=2000" type: 3 }
          timestamp: 5000
        }
        """),
        query="""
        INCLUDE PERFETTO MODULE android.audio;
        SELECT ts, dur, uid, pid, track_name, content_type, usage, devices, flags, format, sample_rate 
        FROM android_audio_track_state;
        """,
        out=Csv("""
        "ts","dur","uid","pid","track_name","content_type","usage","devices","flags","format","sample_rate"
        1000,4000,10001,2000,"audio.track.interval.1","MUSIC","MEDIA","SPEAKER","0","AUDIO_FORMAT_PCM_16_BIT",48000
        """))
