load("@perfetto//bazel:rules.bzl", "perfetto_agent_skill")

package(default_visibility = ["//visibility:public"])

perfetto_agent_skill(
    name = "perfetto-workflow-android-heap-dump",
    srcs = ["SKILL.md"] + glob(["references/**", "assets/**"]),
    scripts = glob(["scripts/**"]),
)
