# Recording traces on Chrome

Perfetto can capture traces right from the Chrome browser on desktop. It
captures traces across all open tabs.

> NOTE: To record traces from Chrome on Android, follow the
> [instructions for recording Android system traces](/docs/getting-started/system-tracing.md)
> and enable the Chrome probe. If you are using a
> [user build of Android](https://source.android.com/docs/setup/build/building#lunch),
> you'll have to enable integration with system Perfetto by switching
> `chrome://flags#enable-perfetto-system-tracing` to "Enabled" and restarting
> Chrome.

## Recording a trace manually

> NOTE: If you need automated trace collection follow the
> [crossbench instructions](#recording-a-trace-with-crossbench-automation).

1.  Navigate to [ui.perfetto.dev](https://ui.perfetto.dev/) and select [**"Record
    new trace"**](https://ui.perfetto.dev/#!/record) from the left menu.
    > If you are using the Perfetto UI for the first time, you have to install
    > the
    > [Perfetto UI Chrome extension](https://chrome.google.com/webstore/detail/perfetto-ui/lfmkphfpdbjijhpomgecfikhfohaoine).
2.  Select **"Chrome"** as **"Target platform"** in the [Overview settings](https://ui.perfetto.dev/#!/record/target).

3.  Сonfigure settings in [**"Recording settings"**](https://ui.perfetto.dev/#!/record/config).

    ![Record page of the Perfetto UI](/docs/images/record-trace-chrome.png)
    > NOTE: "Long trace" mode is not yet available for Chrome desktop.
    > Tips:
    >
    > - To save the current config settings and apply them later go to the "Saved configs" menu.
    > - To share your config settings go to the "Recording command" menu.

4.  Select which categories (or top level tags) in the
    [**Chrome browser**](https://ui.perfetto.dev/#!/record/chrome) probe section
    that you want.

    > NOTE: The tags at the top enable groups of related categories, but there
    > is currently no direct way to see them when targeting Chrome. However, you
    > can switch the target to "Android" and then see the categories in the
    > generated config in the "Recording Command" section if you are curious.

    The list at the bottom can be used to select additional categories.

    ![Tracing categories of Chrome](/docs/images/tracing-categories-chrome.png)

5.  Now you can start the trace recording. Press the **"Start recording"**
    button when ready.
6.  Proceed to use the browser to capture the action you want to trace, and wait
    for the trace to finish. You can also stop the trace manually by pressing
    the "Stop" button.

    **Do not close the perfetto UI tab!** Otherwise, tracing will stop and the
    trace data will be lost.

7.  Once the trace is ready, you can find and analyze it in the left menu
    **"Current Trace"**.

    > **IMPORTANT**: If you want to share a trace, keep in mind that it will
    > contain the URL and title of all open tabs, URLs of subresources used by
    > each tab, extension IDs, hardware identifying details, and other similar
    > information that you may not want to make public.

## Recording a trace with crossbench automation

If you need to automate collecting traces or need more precise control over
chrome flags we recommend using
[crossbench](https://chromium.googlesource.com/crossbench).
It supports collecting traces for chrome on all major platforms.

1. Follow Steps 1-4 from the [manual process](#recording-a-trace-manually) to create a trace configuration.
2. Download the textproto config from the
   [cmdline instructions page ](https://ui.perfetto.dev/#!/record/cmdline)
   and save it locally as `config.txtpb`
3. Run crossbench with your configuration
   ```bash
   ./tools/perf/crossbench load \
     --probe='perfetto:/tmp/config.txtpb' \
     --url="http://test.com" \
     --browser=path/to/chrome \
     -- $CUSTOM_CHROME_FLAGS
   ```