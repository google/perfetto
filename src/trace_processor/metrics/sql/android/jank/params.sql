-- Table to store parameters that will be matched with CUJs using the CUJ name.
DROP TABLE IF EXISTS android_jank_cuj_param_set;
CREATE TABLE android_jank_cuj_param_set (cuj_name_glob STRING, main_thread_override STRING);
INSERT INTO android_jank_cuj_param_set (cuj_name_glob, main_thread_override)
VALUES
('SPLASHSCREEN_EXIT_ANIM', 'll.splashscreen'),
('SPLASHSCREEN_AVD', 'll.splashscreen'),
('ONE_HANDED_ENTER_TRANSITION::*', 'wmshell.main'),
('ONE_HANDED_EXIT_TRANSITION::*', 'wmshell.main'),
('PIP_TRANSITION::*', 'wmshell.main');


-- Matches each CUJ with the right set of parameters.
DROP TABLE IF EXISTS android_jank_cuj_param;
CREATE PERFETTO TABLE android_jank_cuj_param AS
SELECT cuj_id, main_thread_override
FROM android_jank_cuj
LEFT JOIN android_jank_cuj_param_set ON cuj_name GLOB cuj_name_glob;
