import re

boundaries_path = "src/trace_processor/perfetto_sql/stdlib/android/cujs/boundaries.sql"
with open(boundaries_path, "r") as f:
    text = f.read()

# Remove sf_main_thread_frame_boundary definition
text = re.sub(r"-- Similar to `_android_jank_cuj_main_thread_frame_boundary`.*?FROM expected_frame_timeline_slice expected_timeline.*?ON main_thread_slice.vsync = CAST\(expected_timeline.name AS INTEGER\);\n", "", text, flags=re.DOTALL)

with open(boundaries_path, "w") as f:
    f.write(text)
