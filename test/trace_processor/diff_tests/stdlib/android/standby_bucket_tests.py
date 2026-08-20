from python.generators.diff_tests.testing import Csv, DiffTestBlueprint, TestSuite, Path


class StandbyBucket(TestSuite):

  def test_standby_bucket(self):
    return DiffTestBlueprint(
        trace=Path('standby_bucket_data.py'),
        query="""
        INCLUDE PERFETTO MODULE android.standby_bucket;
        SELECT ts, dur, package_name, bucket, main_reason
        FROM android_standby_bucket;
        """,
        out=Csv("""
        "ts","dur","package_name","bucket","main_reason"
        1000000000,4000000000,"com.example.app","BUCKET_ACTIVE","MAIN_UNKNOWN"
        5000000000,0,"com.example.app","BUCKET_WORKING_SET","1"
        """))
