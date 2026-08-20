from python.generators.diff_tests.testing import Csv, DiffTestBlueprint, TestSuite, Path


class ProcessUidState(TestSuite):

  def test_process_uid_state(self):
    return DiffTestBlueprint(
        trace=Path('process_uid_state_data.py'),
        query="""
        INCLUDE PERFETTO MODULE android.process_uid_state;
        SELECT ts, dur, uid, process_state_name 
        FROM android_process_uid_state;
        """,
        out=Csv("""
        "ts","dur","uid","process_state_name"
        1000,4000,10001,"2"
        5000,0,10001,"4"
        """))
