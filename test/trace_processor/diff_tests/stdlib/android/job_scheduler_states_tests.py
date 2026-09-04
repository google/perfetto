from python.generators.diff_tests.testing import Csv, DiffTestBlueprint, TestSuite, Path


class JobSchedulerStates(TestSuite):

  def test_job_scheduler_states(self):
    return DiffTestBlueprint(
        trace=Path('job_scheduler_states_data.py'),
        query="""
        INCLUDE PERFETTO MODULE android.job_scheduler_states;
        SELECT ts, dur, job_name, uid
        FROM android_job_scheduler_states;
        """,
        out=Csv("""
        "ts","dur","job_name","uid"
        1000000000,4000000000,"job1",10001
        """))
