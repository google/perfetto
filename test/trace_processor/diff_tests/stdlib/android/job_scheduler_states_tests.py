from python.generators.diff_tests.testing import Csv, DataPath, DiffTestBlueprint, TestSuite


class JobSchedulerStates(TestSuite):

  def test_job_scheduler_states(self):
    return DiffTestBlueprint(
        trace=DataPath('android_job_scheduler.perfetto-trace'),
        query="""
        INCLUDE PERFETTO MODULE android.job_scheduler_states;
        SELECT ts, dur, job_name, package_name
        FROM android_job_scheduler;
        """,
        out=Csv("""
        "ts","dur","job_name","package_name"
        377089754138,83200835,"@androidx.work.systemjobscheduler@com.android.providers.media.module/androidx.work.impl.background.systemjob.SystemJobService","com.android.providers.media.module"
        385507499374,111746552,"@androidx.work.systemjobscheduler@com.android.providers.media.module/androidx.work.impl.background.systemjob.SystemJobService","com.android.providers.media.module"
        416753734715,129444346,"@androidx.work.systemjobscheduler@com.android.providers.media.module/androidx.work.impl.background.systemjob.SystemJobService","com.android.providers.media.module"
        422530232411,86735906,"@androidx.work.systemjobscheduler@com.android.providers.media.module/androidx.work.impl.background.systemjob.SystemJobService","com.android.providers.media.module"
        """))
