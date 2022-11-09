# Frequently Asked Questions
This page contains some common questions that the Perfetto team is asked
and their answers.

- [Frequently Asked Questions](#frequently-asked-questions)
  - [How do I open trace in UI from command line?](#how-do-i-open-trace-in-ui-from-command-line)

## How do I open trace in UI from command line?
When collecting traces from the command line, a convenient way to open traces
is to use the [open_trace_in_ui script](/tools/open_trace_in_ui).

This can be used as follows:
```sh
curl -OL https://github.com/google/perfetto/raw/master/tools/open_trace_in_ui
chmod +x open_trace_in_ui
./open_trace_in_ui -i /path/to/trace
```

If you already have a Perfetto checkout, the first steps can be skipped.
From the Perfetto root, run:
```sh
tools/open_trace_in_ui -i /path/to/trace
```