/*
 * Copyright (C) 2026 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "src/trace_processor/shell/ai_subcommand.h"

#include <fcntl.h>
#include <sys/stat.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ai/skills/skills.h"
#include "perfetto/base/build_config.h"
#include "perfetto/base/logging.h"
#include "perfetto/base/status.h"
#include "perfetto/ext/base/file_utils.h"
#include "perfetto/ext/base/scoped_file.h"
#include "perfetto/ext/base/status_or.h"
#include "perfetto/ext/base/string_utils.h"
#include "perfetto/ext/trace_processor/trace_processor_shell.h"
#include "src/trace_processor/shell/subcommand.h"
#include "src/trace_processor/util/gzip_utils.h"
#include "src/trace_processor/util/sql_bundle.h"

namespace perfetto::trace_processor::shell {

namespace {

std::string HomeDir() {
#if PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)
  if (const char* p = std::getenv("USERPROFILE"); p && *p)
    return p;
  if (const char* p = std::getenv("APPDATA"); p && *p)
    return p;
#else
  if (const char* p = std::getenv("HOME"); p && *p)
    return p;
#endif
  return {};
}

std::function<base::StatusOr<std::string>()> HomeRelativeResolver(
    std::string subdir) {
  return [subdir = std::move(subdir)]() -> base::StatusOr<std::string> {
    std::string home = HomeDir();
    if (home.empty()) {
      return base::ErrStatus(
          "cannot resolve home directory; set HOME (or USERPROFILE on "
          "Windows), or pass --dest explicitly.");
    }
    return home + "/" + subdir;
  };
}

std::vector<AiAgentSpec> BuiltinAgents() {
  std::vector<AiAgentSpec> out;
  out.push_back({"claudecode", "Anthropic Claude Code",
                 HomeRelativeResolver(".claude/skills")});
  out.push_back({"geminicli", "Google Gemini CLI",
                 HomeRelativeResolver(".gemini/skills")});
  out.push_back(
      {"codex", "OpenAI Codex CLI", HomeRelativeResolver(".agents/skills")});
  return out;
}

std::vector<AiAgentSpec> RegisteredAgents(
    TraceProcessorShell_PlatformInterface* platform) {
  std::vector<AiAgentSpec> agents = BuiltinAgents();
  if (platform) {
    for (auto& extra : platform->GetExtraAiAgents()) {
      auto it = std::find_if(
          agents.begin(), agents.end(),
          [&](const AiAgentSpec& a) { return a.cli_name == extra.cli_name; });
      if (it != agents.end()) {
        *it = std::move(extra);
      } else {
        agents.push_back(std::move(extra));
      }
    }
  }
  return agents;
}

const AiAgentSpec* LookupAgent(const std::vector<AiAgentSpec>& agents,
                               const std::string& name) {
  for (const auto& spec : agents) {
    if (spec.cli_name == name)
      return &spec;
  }
  return nullptr;
}

std::string AgentNameList(const std::vector<AiAgentSpec>& agents) {
  std::string out;
  for (size_t i = 0; i < agents.size(); ++i) {
    if (i)
      out += "|";
    out += agents[i].cli_name;
  }
  return out;
}

base::Status MkdirParents(const std::string& path) {
  if (path.empty() || path == "/" || path == ".")
    return base::OkStatus();
  if (base::FileExists(path))
    return base::OkStatus();
  std::string parent = base::Dirname(path);
  if (parent != path) {
    if (auto s = MkdirParents(parent); !s.ok())
      return s;
  }
  if (!base::Mkdir(path)) {
    if (base::FileExists(path))
      return base::OkStatus();
    return base::ErrStatus("failed to create directory '%s' (errno=%d)",
                           path.c_str(), errno);
  }
  return base::OkStatus();
}

bool GlobMatch(std::string_view pattern, std::string_view text) {
  size_t p = 0, t = 0;
  size_t star_p = std::string_view::npos, star_t = 0;
  while (t < text.size()) {
    if (p < pattern.size() && pattern[p] == '*') {
      star_p = p++;
      star_t = t;
    } else if (p < pattern.size() &&
               (pattern[p] == '?' || pattern[p] == text[t])) {
      ++p;
      ++t;
    } else if (star_p != std::string_view::npos) {
      p = star_p + 1;
      t = ++star_t;
    } else {
      return false;
    }
  }
  while (p < pattern.size() && pattern[p] == '*')
    ++p;
  return p == pattern.size();
}

struct Skill {
  std::string source_path;   // "perfetto-infra-querying-traces/SKILL.md"
  std::string slug;          // "perfetto-infra-querying-traces"
  std::string_view content;  // full file bytes; points into `bytes` storage
};

std::vector<Skill> LoadBundledSkills(std::vector<uint8_t>* bytes_out) {
  *bytes_out = util::GzipDecompressor::DecompressFully(
      ai_skills::kSkills.data(), ai_skills::kSkills.size());
  std::vector<Skill> out;
  for (const auto& entry : SqlBundle(bytes_out->data(), bytes_out->size())) {
    Skill s;
    s.source_path = entry.path;
    s.slug = base::Dirname(s.source_path);
    s.content = entry.sql;
    out.push_back(std::move(s));
  }
  std::sort(out.begin(), out.end(), [](const Skill& a, const Skill& b) {
    return a.source_path < b.source_path;
  });
  return out;
}

bool MatchesAnyGlob(std::string_view text,
                    const std::vector<std::string>& globs) {
  for (const auto& g : globs) {
    if (GlobMatch(g, text))
      return true;
  }
  return false;
}

bool ContainsCaseInsensitive(std::string_view haystack,
                             std::string_view needle) {
  if (needle.empty())
    return true;
  if (haystack.size() < needle.size())
    return false;
  for (size_t i = 0; i + needle.size() <= haystack.size(); ++i) {
    bool match = true;
    for (size_t j = 0; j < needle.size(); ++j) {
      char a = haystack[i + j];
      char b = needle[j];
      if (a >= 'A' && a <= 'Z')
        a = static_cast<char>(a - 'A' + 'a');
      if (b >= 'A' && b <= 'Z')
        b = static_cast<char>(b - 'A' + 'a');
      if (a != b) {
        match = false;
        break;
      }
    }
    if (match)
      return true;
  }
  return false;
}

base::Status WriteFileAtomic(const std::string& path, std::string_view body) {
  if (auto s = MkdirParents(base::Dirname(path)); !s.ok())
    return s;
  base::ScopedFile fd =
      base::OpenFile(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (!fd) {
    return base::ErrStatus("failed to open '%s' for write (errno=%d)",
                           path.c_str(), errno);
  }
  ssize_t n = base::WriteAll(*fd, body.data(), body.size());
  if (n < 0 || static_cast<size_t>(n) != body.size()) {
    return base::ErrStatus("short write to '%s' (wrote %zd of %zu bytes)",
                           path.c_str(), n, body.size());
  }
  return base::OkStatus();
}

std::vector<Skill> FilterSkills(std::vector<Skill> skills,
                                const std::vector<std::string>& include_globs,
                                const std::vector<std::string>& exclude_globs) {
  std::vector<Skill> out;
  for (auto& s : skills) {
    if (!include_globs.empty() && !MatchesAnyGlob(s.slug, include_globs))
      continue;
    if (MatchesAnyGlob(s.slug, exclude_globs))
      continue;
    out.push_back(std::move(s));
  }
  return out;
}

base::Status InstallSkills(const AiAgentSpec& agent,
                           const std::string& dest_root,
                           bool dry_run,
                           const std::vector<Skill>& selected) {
  printf("Installing %zu Perfetto skill%s for %s into %s\n", selected.size(),
         selected.size() == 1 ? "" : "s", agent.description.c_str(),
         dest_root.c_str());
  for (const auto& s : selected) {
    std::string out_path = dest_root + "/" + s.slug + "/SKILL.md";
    printf("  %s  %s\n", dry_run ? "DRY-RUN" : "WRITE  ", out_path.c_str());
    if (!dry_run) {
      if (auto st = WriteFileAtomic(out_path, s.content); !st.ok())
        return st;
    }
  }
  return base::OkStatus();
}

void PrintSkill(const Skill& s) {
  printf("- %s\n", s.slug.c_str());
}

base::Status RunListSkills(const SubcommandContext& ctx,
                           const std::vector<Skill>& skills) {
  if (ctx.positional_args.size() > 1) {
    return base::ErrStatus(
        "ai list-skills: unexpected positional argument '%s'.",
        ctx.positional_args[1].c_str());
  }
  printf("Bundled Perfetto skills (%zu):\n", skills.size());
  for (const auto& s : skills)
    PrintSkill(s);
  return base::OkStatus();
}

base::Status RunSearchSkills(const SubcommandContext& ctx,
                             const std::vector<Skill>& skills) {
  if (ctx.positional_args.size() < 2) {
    return base::ErrStatus(
        "ai search-skills: missing query. "
        "Usage: trace_processor ai search-skills <query>.");
  }
  if (ctx.positional_args.size() > 2) {
    return base::ErrStatus(
        "ai search-skills: unexpected extra positional argument '%s'. "
        "Quote multi-word queries.",
        ctx.positional_args[2].c_str());
  }
  const std::string& query = ctx.positional_args[1];
  size_t hits = 0;
  for (const auto& s : skills) {
    if (ContainsCaseInsensitive(s.slug, query) ||
        ContainsCaseInsensitive(s.content, query)) {
      PrintSkill(s);
      ++hits;
    }
  }
  printf("\n%zu of %zu skill%s match '%s'.\n", hits, skills.size(),
         skills.size() == 1 ? "" : "s", query.c_str());
  return base::OkStatus();
}

base::Status RunInstallSkills(const SubcommandContext& ctx,
                              const std::vector<Skill>& skills,
                              const std::vector<AiAgentSpec>& agents,
                              const std::string& agent_list,
                              const std::string& dest_override,
                              bool dry_run,
                              const std::vector<std::string>& include_globs,
                              const std::vector<std::string>& exclude_globs) {
  if (ctx.positional_args.size() < 2) {
    return base::ErrStatus("ai install-skills: missing agent. Pass one of %s.",
                           agent_list.c_str());
  }
  if (ctx.positional_args.size() > 2) {
    return base::ErrStatus(
        "ai install-skills: unexpected extra positional argument '%s'.",
        ctx.positional_args[2].c_str());
  }
  const std::string& agent_name = ctx.positional_args[1];
  const AiAgentSpec* agent = LookupAgent(agents, agent_name);
  if (!agent) {
    return base::ErrStatus(
        "ai install-skills: unknown agent '%s'. Try one of %s.",
        agent_name.c_str(), agent_list.c_str());
  }
  std::vector<Skill> selected =
      FilterSkills(skills, include_globs, exclude_globs);
  if (selected.empty()) {
    return base::ErrStatus(
        "ai install-skills: --include/--exclude filters left no skills "
        "to install. Run `trace_processor ai list-skills` to see what's "
        "available.");
  }
  std::string dest = dest_override;
  if (dest.empty()) {
    if (!agent->resolve_install_dir) {
      return base::ErrStatus(
          "ai install-skills: agent '%s' has no install-dir resolver "
          "and no --dest was given.",
          agent_name.c_str());
    }
    base::StatusOr<std::string> resolved = agent->resolve_install_dir();
    if (!resolved.ok()) {
      return base::ErrStatus("ai install-skills: %s",
                             resolved.status().c_message());
    }
    dest = *resolved;
  }
  return InstallSkills(*agent, dest, dry_run, selected);
}

}  // namespace

const char* AiSubcommand::name() const {
  return "ai";
}

const char* AiSubcommand::description() const {
  return "AI-related actions (today: list/search/install skills).";
}

const char* AiSubcommand::usage_args() const {
  return "<action> [args...]";
}

const char* AiSubcommand::detailed_help() const {
  return R"(Umbrella subcommand for AI-related actions. Today the actions
under it list, search, and install Perfetto's bundled set of
[Agent Skills](https://agentskills.io) into a coding
agent's discovery directory.

Actions:
  list-skills             Print every bundled SKILL.md.
  search-skills <query>   Print bundled skills whose slug or content
                          contains the case-insensitive substring.
  install-skills <agent>  Copy bundled SKILL.md files into the on-disk
                          discovery directory of an AI coding agent so
                          the agent picks them up automatically.

Built-in agents for `install-skills`:
  claudecode              Anthropic Claude Code (~/.claude/skills/)
  geminicli               Google Gemini CLI    (~/.gemini/skills/)
  codex                   OpenAI Codex CLI     (~/.agents/skills/)

Embedders that wrap trace_processor (e.g. Google internal builds) can
register additional agents via TraceProcessorShell_PlatformInterface;
those appear alongside the built-ins for that build.

Glob filters (`--include` / `--exclude`, repeatable) match the skill's
slug: `perfetto-infra-*`, `perfetto-workflow-android-*`, `*heap*`. With
no filters, every bundled skill is installed.

Examples:
  trace_processor ai list-skills
  trace_processor ai search-skills 'heap dump'
  trace_processor ai install-skills claudecode
  trace_processor ai install-skills geminicli --include 'perfetto-infra-*' --dry-run
  trace_processor ai install-skills codex --exclude 'perfetto-workflow-*' --dest /tmp/x)";
}

std::vector<FlagSpec> AiSubcommand::GetFlags() {
  return {
      StringFlag("dest", '\0', "DIR",
                 "(install-skills) Override the install root. Default "
                 "is the agent's standard per-user skills directory.",
                 &dest_),
      BoolFlag("dry-run", '\0',
               "(install-skills) Print what would be written without "
               "touching the filesystem.",
               &dry_run_),
      FlagSpec{"include", '\0', true, "GLOB",
               "(install-skills) Only install skills whose slug "
               "matches GLOB. Repeatable. Default: install everything.",
               [this](const char* v) { include_globs_.emplace_back(v); }},
      FlagSpec{"exclude", '\0', true, "GLOB",
               "(install-skills) Skip skills whose slug matches GLOB. "
               "Repeatable. Applied after --include.",
               [this](const char* v) { exclude_globs_.emplace_back(v); }},
  };
}

base::Status AiSubcommand::Run(const SubcommandContext& ctx) {
  if (ctx.positional_args.empty()) {
    return base::ErrStatus(
        "ai: missing action. Try `trace_processor help ai`.");
  }
  std::vector<uint8_t> bytes;
  std::vector<Skill> skills = LoadBundledSkills(&bytes);
  const std::string& action = ctx.positional_args[0];
  if (action == "list-skills") {
    return RunListSkills(ctx, skills);
  }
  if (action == "search-skills") {
    return RunSearchSkills(ctx, skills);
  }
  if (action == "install-skills") {
    std::vector<AiAgentSpec> agents = RegisteredAgents(ctx.platform);
    std::string agent_list = AgentNameList(agents);
    return RunInstallSkills(ctx, skills, agents, agent_list, dest_, dry_run_,
                            include_globs_, exclude_globs_);
  }
  return base::ErrStatus(
      "ai: unknown action '%s'. Supported actions: list-skills, "
      "search-skills, install-skills.",
      action.c_str());
}

}  // namespace perfetto::trace_processor::shell
