load("@perfetto//bazel:rules.bzl", "perfetto_agent_skill", "perfetto_validate_skill_test")

package(default_visibility = ["//visibility:public"])

perfetto_validate_skill_test(
    name = "validate_perfetto_infra_querying_traces_skill_test",
)

perfetto_agent_skill(
    name = "perfetto-infra-querying-traces",
    srcs = ["SKILL.md"] + glob(["references/**", "assets/**"]),
    scripts = glob(["scripts/**"]),
)
