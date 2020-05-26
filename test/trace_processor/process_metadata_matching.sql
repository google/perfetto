-- Create so that RUN_METRIC will run without outputting any rows.
CREATE TABLE TEST_TMP AS
SELECT RUN_METRIC('android/process_metadata.sql');

DROP TABLE TEST_TMP;

SELECT upid, process_name, uid, shared_uid, package_name, version_code
FROM process_metadata_table
WHERE upid != 0;
