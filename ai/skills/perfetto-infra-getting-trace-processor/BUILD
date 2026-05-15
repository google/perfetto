load("@perfetto//bazel:rules.bzl", "perfetto_agent_skill", "perfetto_validate_skill_test")

package(default_visibility = ["//visibility:public"])

perfetto_validate_skill_test(
    name = "validate_perfetto_infra_getting_trace_processor_skill_test",
)

perfetto_agent_skill(
    name = "perfetto-infra-getting-trace-processor",
    srcs = ["SKILL.md"] + glob(["references/**", "assets/**"]),
    scripts = glob(["scripts/**"]),
)
