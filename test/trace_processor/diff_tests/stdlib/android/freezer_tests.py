from python.generators.diff_tests.testing import Csv, DiffTestBlueprint, TestSuite, TextProto, Json, Path


class Freezer(TestSuite):

  def test_freezer_state_statsd_intervals(self):
    return DiffTestBlueprint(
        trace=Path('freezer_data.py'),
        query="""
            INCLUDE PERFETTO MODULE android.freezer;
            SELECT ts, dur, pid, process_name, freezer_state, time_unfrozen_millis, unfreeze_reason 
            FROM android_freezer_state_statsd;
            """,
        out=Csv("""
            "ts","dur","pid","process_name","freezer_state","time_unfrozen_millis","unfreeze_reason"
            1000000000,4000000000,1234,"com.example.app","FREEZE_APP",0,"UFR_NONE"
            5000000000,0,1234,"com.example.app","UNFREEZE_APP",4,"UFR_ACTIVITY"
            """))
