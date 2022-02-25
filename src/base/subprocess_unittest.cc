/*
 * Copyright (C) 2019 The Android Open Source Project
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

#include "perfetto/ext/base/subprocess.h"

#include <thread>

#if PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)
#include <Windows.h>
#else
#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include "perfetto/base/time.h"
#include "perfetto/ext/base/file_utils.h"
#include "perfetto/ext/base/pipe.h"
#include "perfetto/ext/base/temp_file.h"
#include "test/gtest_and_gmock.h"

namespace perfetto {
namespace base {
namespace {

std::string GetOutput(const Subprocess& p) {
  std::string output = p.output();
#if PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)
  size_t pos = 0;
  while ((pos = output.find("\r\n", pos)) != std::string::npos)
    output.erase(pos, 1);
#endif
  return output;
}

std::string GenLargeString() {
  std::string contents;
  for (int i = 0; i < 4096; i++) {
    contents += "very long text " + std::to_string(i) + "\n";
  }
  // Make sure that |contents| is > the default pipe buffer on Linux (4 pages).
  PERFETTO_DCHECK(contents.size() > 4096 * 4);
  return contents;
}

TEST(SubprocessTest, InvalidPath) {
  Subprocess p({"/usr/bin/invalid_1337"});
  EXPECT_FALSE(p.Call());
  EXPECT_EQ(p.status(), Subprocess::kTerminated);
#if PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)
  EXPECT_EQ(p.returncode(), ERROR_FILE_NOT_FOUND);
#else
  EXPECT_EQ(p.returncode(), 128);
  EXPECT_EQ(GetOutput(p), "execve() failed\n");
#endif
}

TEST(SubprocessTest, StdoutOnly) {
#if PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)
  Subprocess p({"cmd", "/c", "(echo skip_err 1>&2) && echo out_only"});
#else
  Subprocess p({"sh", "-c", "(echo skip_err >&2); echo out_only"});
#endif
  p.args.stdout_mode = Subprocess::OutputMode::kBuffer;
  p.args.stderr_mode = Subprocess::OutputMode::kDevNull;

  EXPECT_TRUE(p.Call());
  EXPECT_EQ(p.status(), Subprocess::kTerminated);
  EXPECT_EQ(GetOutput(p), "out_only\n");
}

TEST(SubprocessTest, StderrOnly) {
#if PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)
  Subprocess p({"cmd", "/c", "(echo err_only>&2) && echo skip_out"});
#else
  Subprocess p({"sh", "-c", "(echo err_only >&2); echo skip_out"});
#endif
  p.args.stdout_mode = Subprocess::OutputMode::kDevNull;
  p.args.stderr_mode = Subprocess::OutputMode::kBuffer;
  EXPECT_TRUE(p.Call());
  EXPECT_EQ(GetOutput(p), "err_only\n");
}

TEST(SubprocessTest, BothStdoutAndStderr) {
#if PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)
  Subprocess p({"cmd", "/c", "echo out&&(echo err>&2)&&echo out2"});
#else
  Subprocess p({"sh", "-c", "echo out; (echo err >&2); echo out2"});
#endif
  p.args.stdout_mode = Subprocess::OutputMode::kBuffer;
  p.args.stderr_mode = Subprocess::OutputMode::kBuffer;
  EXPECT_TRUE(p.Call());
  EXPECT_EQ(GetOutput(p), "out\nerr\nout2\n");
}

TEST(SubprocessTest, CatInputModeDevNull) {
  std::string ignored_input = "ignored input";
#if PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)
  Subprocess p({"cmd", "/C", "findstr . || exit 0"});
#else
  Subprocess p({"cat", "-"});
#endif
  p.args.stdout_mode = Subprocess::OutputMode::kBuffer;
  p.args.input = ignored_input;
  p.args.stdin_mode = Subprocess::InputMode::kDevNull;
  EXPECT_TRUE(p.Call());
  EXPECT_EQ(p.status(), Subprocess::kTerminated);
  EXPECT_EQ(GetOutput(p), "");
}

TEST(SubprocessTest, BothStdoutAndStderrInputModeDevNull) {
#if PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)
  Subprocess p({"cmd", "/c", "echo out&&(echo err>&2)&&echo out2"});
#else
  Subprocess p({"sh", "-c", "echo out; (echo err >&2); echo out2"});
#endif
  p.args.stdout_mode = Subprocess::OutputMode::kBuffer;
  p.args.stderr_mode = Subprocess::OutputMode::kBuffer;
  p.args.stdin_mode = Subprocess::InputMode::kDevNull;
  EXPECT_TRUE(p.Call());
  EXPECT_EQ(GetOutput(p), "out\nerr\nout2\n");
}

TEST(SubprocessTest, AllDevNull) {
#if PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)
  Subprocess p({"cmd", "/c", "(exit 1)"});
#else
  Subprocess p({"false"});
#endif
  p.args.stdout_mode = Subprocess::OutputMode::kDevNull;
  p.args.stderr_mode = Subprocess::OutputMode::kDevNull;
  p.args.stdin_mode = Subprocess::InputMode::kDevNull;
  EXPECT_FALSE(p.Call());
  EXPECT_EQ(p.status(), Subprocess::kTerminated);
  EXPECT_EQ(p.returncode(), 1);
}

TEST(SubprocessTest, BinTrue) {
#if PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)
  Subprocess p({"cmd", "/c", "(exit 0)"});
#else
  Subprocess p({"true"});
#endif
  EXPECT_TRUE(p.Call());
  EXPECT_EQ(p.status(), Subprocess::kTerminated);
  EXPECT_EQ(p.returncode(), 0);
}

TEST(SubprocessTest, BinFalse) {
#if PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)
  Subprocess p({"cmd", "/c", "(exit 1)"});
#else
  Subprocess p({"false"});
#endif
  EXPECT_FALSE(p.Call());
  EXPECT_EQ(p.status(), Subprocess::kTerminated);
  EXPECT_EQ(p.returncode(), 1);
}

TEST(SubprocessTest, Echo) {
#if PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)
  Subprocess p({"cmd", "/c", "echo|set /p ignored_var=foobar"});
#else
  Subprocess p({"echo", "-n", "foobar"});
#endif
  p.args.stdout_mode = Subprocess::OutputMode::kBuffer;
  EXPECT_TRUE(p.Call());
  EXPECT_EQ(p.status(), Subprocess::kTerminated);
  EXPECT_EQ(p.returncode(), 0);
  EXPECT_EQ(GetOutput(p), "foobar");
}

TEST(SubprocessTest, FeedbackLongInput) {
  std::string contents = GenLargeString();
#if PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)
  Subprocess p({"cmd", "/C", "findstr ."});
#else
  Subprocess p({"cat", "-"});
#endif
  p.args.stdout_mode = Subprocess::OutputMode::kBuffer;
  p.args.input = contents;
  EXPECT_TRUE(p.Call());
  EXPECT_EQ(p.status(), Subprocess::kTerminated);
  EXPECT_EQ(p.returncode(), 0);
  EXPECT_EQ(GetOutput(p), contents);
}

TEST(SubprocessTest, CatLargeFile) {
  std::string contents = GenLargeString();
  TempFile tf = TempFile::Create();
  WriteAll(tf.fd(), contents.data(), contents.size());
  FlushFile(tf.fd());
#if PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)
  Subprocess p({"cmd", "/c", ("type \"" + tf.path() + "\"").c_str()});
#else
  Subprocess p({"cat", tf.path().c_str()});
#endif
  p.args.stdout_mode = Subprocess::OutputMode::kBuffer;
  EXPECT_TRUE(p.Call());
  EXPECT_EQ(GetOutput(p), contents);
}

TEST(SubprocessTest, Timeout) {
#if PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)
  Subprocess p({"ping", "127.0.0.1", "-n", "60"});
  p.args.stdout_mode = Subprocess::OutputMode::kDevNull;
#else
  Subprocess p({"sleep", "60"});
#endif

  EXPECT_FALSE(p.Call(/*timeout_ms=*/1));
  EXPECT_EQ(p.status(), Subprocess::kTerminated);
  EXPECT_TRUE(p.timed_out());
}

TEST(SubprocessTest, TimeoutNotHit) {
#if PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)
  Subprocess p({"ping", "127.0.0.1", "-n", "1"});
  p.args.stdout_mode = Subprocess::OutputMode::kDevNull;
#else
  Subprocess p({"sleep", "0.01"});
#endif
  EXPECT_TRUE(p.Call(/*timeout_ms=*/100000));
  EXPECT_EQ(p.status(), Subprocess::kTerminated);
}

TEST(SubprocessTest, TimeoutStopOutput) {
#if PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)
  Subprocess p({"cmd", "/c", "FOR /L %N IN () DO @echo stuff>NUL"});
#else
  Subprocess p({"sh", "-c", "while true; do echo stuff; done"});
#endif
  p.args.stdout_mode = Subprocess::OutputMode::kDevNull;
  EXPECT_FALSE(p.Call(/*timeout_ms=*/10));
  EXPECT_EQ(p.status(), Subprocess::kTerminated);
  EXPECT_TRUE(p.timed_out());
}

TEST(SubprocessTest, ExitBeforeReadingStdin) {
#if PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)
  Subprocess p({"ping", "127.0.0.1", "-n", "1"});
#else
  // 'sh -c' is to avoid closing stdin (sleep closes it before sleeping).
  Subprocess p({"sh", "-c", "sleep 0.01"});
#endif
  p.args.stdout_mode = Subprocess::OutputMode::kDevNull;
  p.args.stderr_mode = Subprocess::OutputMode::kDevNull;
  p.args.input = GenLargeString();
  EXPECT_TRUE(p.Call());
  EXPECT_EQ(p.status(), Subprocess::kTerminated);
  EXPECT_EQ(p.returncode(), 0);
}

TEST(SubprocessTest, StdinWriteStall) {
#if PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)
  Subprocess p({"ping", "127.0.0.1", "-n", "10"});
#else
  // 'sh -c' is to avoid closing stdin (sleep closes it before sleeping).
  // This causes a situation where the write on the stdin will stall because
  // nobody reads it and the pipe buffer fills up. In this situation we should
  // still handle the timeout properly.
  Subprocess p({"sh", "-c", "sleep 10"});
#endif
  p.args.stdout_mode = Subprocess::OutputMode::kDevNull;
  p.args.stderr_mode = Subprocess::OutputMode::kDevNull;
  p.args.input = GenLargeString();
  EXPECT_FALSE(p.Call(/*timeout_ms=*/10));
  EXPECT_EQ(p.status(), Subprocess::kTerminated);
  EXPECT_TRUE(p.timed_out());
}

TEST(SubprocessTest, StartAndWait) {
#if PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)
  Subprocess p({"ping", "127.0.0.1", "-n", "1000"});
#else
  Subprocess p({"sleep", "1000"});
#endif
  p.args.stdout_mode = Subprocess::OutputMode::kDevNull;
  p.Start();
  EXPECT_EQ(p.Poll(), Subprocess::kRunning);
  p.KillAndWaitForTermination();

  EXPECT_EQ(p.status(), Subprocess::kTerminated);
  EXPECT_EQ(p.Poll(), Subprocess::kTerminated);
#if PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)
  EXPECT_EQ(p.returncode(), static_cast<int>(STATUS_CONTROL_C_EXIT));
#else
  EXPECT_EQ(p.returncode(), static_cast<int>(128 + SIGKILL));
#endif
}

TEST(SubprocessTest, PollBehavesProperly) {
  Pipe pipe = Pipe::Create();
#if PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)
  Subprocess p({"cmd", "/c", "(exit 0)"});
#else
  Subprocess p({"true"});
#endif
  p.args.stdout_mode = Subprocess::OutputMode::kFd;
  p.args.out_fd = std::move(pipe.wr);
  p.Start();

  // Wait for EOF (which really means the child process has terminated).
  std::string ignored;
  ReadPlatformHandle(*pipe.rd, &ignored);

  // The kernel takes some time to detect the termination of the process. The
  // best thing we can do here is check that we detect the termination within
  // some reasonable time.
  auto start_ms = GetWallTimeMs();
  while (p.Poll() != Subprocess::kTerminated) {
    auto elapsed_ms = GetWallTimeMs() - start_ms;
    ASSERT_LT(elapsed_ms, TimeMillis(10000));
    std::this_thread::sleep_for(TimeMillis(5));
  }

  // At this point Poll() must detect the termination.
  EXPECT_EQ(p.Poll(), Subprocess::kTerminated);
  EXPECT_EQ(p.returncode(), 0);
}

TEST(SubprocessTest, Wait) {
#if PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)
  Subprocess p({"cmd", "/c", "echo exec_done && FOR /L %N IN () DO @echo>NUL"});
#else
  Subprocess p({"sh", "-c", "echo exec_done; while true; do true; done"});
#endif
  p.args.stdout_mode = Subprocess::OutputMode::kBuffer;
  p.Start();

  // Wait for the fork()+exec() to complete.
  while (p.output().find("exec_done") == std::string::npos) {
    EXPECT_FALSE(p.Wait(1 /*ms*/));
    EXPECT_EQ(p.status(), Subprocess::kRunning);
  }

#if PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)
  ScopedPlatformHandle proc_handle(::OpenProcess(
      PROCESS_TERMINATE, /*inherit=*/false, static_cast<DWORD>(p.pid())));
  ASSERT_TRUE(proc_handle);
  ASSERT_TRUE(::TerminateProcess(*proc_handle, DBG_CONTROL_BREAK));
#else
  kill(p.pid(), SIGBUS);
#endif
  EXPECT_TRUE(p.Wait(30000 /*ms*/));  // We shouldn't hit this.
  EXPECT_TRUE(p.Wait());              // Should be a no-op.
  EXPECT_EQ(p.status(), Subprocess::kTerminated);
#if PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)
  EXPECT_EQ(p.returncode(), static_cast<int>(DBG_CONTROL_BREAK));
#else
  EXPECT_EQ(p.returncode(), 128 + SIGBUS);
#endif
}

TEST(SubprocessTest, KillOnDtor) {
  auto is_process_alive = [](PlatformProcessId pid) {
#if PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)
    DWORD ignored = 0;
    return ProcessIdToSessionId(static_cast<DWORD>(pid), &ignored);
#else
    // We use kill(SIGWINCH) as a way to tell if the process is still alive by
    // looking at the kill(2) return value. SIGWINCH is one of the few signals
    // that has default ignore disposition.
    return kill(pid, SIGWINCH) == 0;
#endif
  };

  PlatformProcessId pid;
  {
#if PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)
    Subprocess p({"ping", "127.0.0.1", "-n", "1000"});
#else
    Subprocess p({"sleep", "1000"});
#endif
    p.Start();
    pid = p.pid();
    EXPECT_TRUE(is_process_alive(pid));
  }

  // Both on Windows and Linux, kill can take some time to free up the pid.
  bool alive = true;
  for (int attempt = 0; attempt < 1000 && alive; attempt++) {
    alive = is_process_alive(pid);
    std::this_thread::sleep_for(TimeMillis(5));
  }
  EXPECT_FALSE(alive);
}

// Regression test for b/162505491.
TEST(SubprocessTest, MoveOperators) {
  {
#if PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)
    Subprocess initial({"ping", "127.0.0.1", "-n", "100"});
#else
    Subprocess initial = Subprocess({"sleep", "10000"});
#endif
    initial.args.stdout_mode = Subprocess::OutputMode::kDevNull;
    initial.Start();
    Subprocess moved(std::move(initial));
    EXPECT_EQ(moved.Poll(), Subprocess::kRunning);
    EXPECT_EQ(initial.Poll(), Subprocess::kNotStarted);

    // Check that reuse works
#if PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)
    initial = Subprocess({"cmd", "/c", "echo|set /p ignored_var=hello"});
#else
    initial = Subprocess({"echo", "-n", "hello"});
#endif
    initial.args.stdout_mode = Subprocess::OutputMode::kBuffer;
    initial.Start();
    initial.Wait(/*timeout_ms=*/5000);
    EXPECT_EQ(initial.status(), Subprocess::kTerminated);
    EXPECT_EQ(initial.returncode(), 0);
    EXPECT_EQ(initial.output(), "hello");
  }

  std::vector<Subprocess> v;
  for (int i = 0; i < 10; i++) {
#if PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)
    v.emplace_back(Subprocess({"ping", "127.0.0.1", "-n", "10"}));
#else
    v.emplace_back(Subprocess({"sleep", "10"}));
#endif
    v.back().args.stdout_mode = Subprocess::OutputMode::kDevNull;
    v.back().Start();
  }
  for (auto& p : v)
    EXPECT_EQ(p.Poll(), Subprocess::kRunning);
}

// posix_entrypoint_for_testing is not supported on Windows.
#if !PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)

// Test the case of passing a lambda in |entrypoint| but no cmd.c
TEST(SubprocessTest, Entrypoint) {
  Subprocess p;
  p.args.input = "ping\n";
  p.args.stdout_mode = Subprocess::OutputMode::kBuffer;
  p.args.posix_entrypoint_for_testing = [] {
    char buf[32]{};
    PERFETTO_CHECK(fgets(buf, sizeof(buf), stdin));
    PERFETTO_CHECK(strcmp(buf, "ping\n") == 0);
    printf("pong\n");
    fflush(stdout);
    _exit(42);
  };
  EXPECT_FALSE(p.Call());
  EXPECT_EQ(p.returncode(), 42);
  EXPECT_EQ(GetOutput(p), "pong\n");
}

// Test the case of passing both a lambda entrypoint and a process to exec.
TEST(SubprocessTest, EntrypointAndExec) {
  base::Pipe pipe1 = base::Pipe::Create();
  base::Pipe pipe2 = base::Pipe::Create();
  int pipe1_wr = *pipe1.wr;
  int pipe2_wr = *pipe2.wr;

  Subprocess p({"echo", "123"});
  p.args.stdout_mode = Subprocess::OutputMode::kBuffer;
  p.args.preserve_fds.push_back(pipe2_wr);
  p.args.posix_entrypoint_for_testing = [pipe1_wr, pipe2_wr] {
    base::ignore_result(write(pipe1_wr, "fail", 4));
    base::ignore_result(write(pipe2_wr, "pass", 4));
  };

  p.Start();
  pipe1.wr.reset();
  pipe2.wr.reset();

  char buf[8];
  EXPECT_LE(read(*pipe1.rd, buf, sizeof(buf)), 0);
  EXPECT_EQ(read(*pipe2.rd, buf, sizeof(buf)), 4);
  buf[4] = '\0';
  EXPECT_STREQ(buf, "pass");
  EXPECT_TRUE(p.Wait());
  EXPECT_EQ(p.status(), Subprocess::kTerminated);
  EXPECT_EQ(GetOutput(p), "123\n");
}

#endif

}  // namespace
}  // namespace base
}  // namespace perfetto
