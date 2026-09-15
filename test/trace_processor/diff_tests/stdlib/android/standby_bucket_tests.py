from python.generators.diff_tests.testing import Csv, DiffTestBlueprint, TestSuite, Path


class StandbyBucket(TestSuite):

  def test_standby_bucket(self):
    return DiffTestBlueprint(
        trace=Path('standby_bucket_data.py'),
        query="""
        INCLUDE PERFETTO MODULE android.statsd;
        SELECT name FROM android_statsd_atoms;
        """,
        out=Csv("""
        "name"
        "app_standby_bucket_changed"
        "app_standby_bucket_changed"
        """))
