load("@perfetto//bazel:rules.bzl", "perfetto_agent_skill")

package(default_visibility = ["//visibility:public"])

perfetto_agent_skill(
    name = "perfetto-infra-getting-trace-processor",
    srcs = ["SKILL.md"] + glob(["references/**", "assets/**"]),
    scripts = glob(["scripts/**"]),
)
