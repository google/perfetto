# Quickstart: Record traces on Chrome desktop

Perfetto can capture traces right from the Chrome browser on desktop. It captures traces across all open tabs.

> To record traces from Chrome on Android, follow the [instructions for recording Android system traces](/docs/quickstart/android-tracing.md) and enable the Chrome probe.

>> If you are using [user build of Android](https://source.android.com/docs/setup/build/building#lunch), you'll have to enable integration with system Perfetto by switching chrome://flags#enable-perfetto-system-tracing to "Enabled" and restarting Chrome.

## Recording a trace

1. Navigate to [ui.perfetto.dev](https://ui.perfetto.dev/) and select **"Record new trace"** from the left menu.
    > If you are using the Perfetto UI for the first time, you have to install the [Perfetto UI Chrome extension](https://chrome.google.com/webstore/detail/perfetto-ui/lfmkphfpdbjijhpomgecfikhfohaoine).
2. Select **"Chrome"** as **"Target platform"** in the drop-down.

   On Chrome OS, you can also record system traces by selecting the "Chrome OS (system trace)" target platform.
3. Сonfigure settings in **"Recording settings"**.

   ![Record page of the Perfetto UI](/docs/images/record-trace-chrome.png)

    >Note: "Long trace" mode is not yet available for Chrome desktop.

    > Tips:
    >
    > - To save the current config settings and apply them later go to the "Saved configs" menu.
    > - To share your config settings go to the "Recording command" menu.
>
4. Select which categories (or top level tags) in the **Chrome** probe section that you want.

   > Note: The tags at the top enable groups of related categories, but there is currently no direct way to see them when targeting Chrome. However, you can switch the target to "Android" and then see the categories in the generated config in the "Recording Command" section if you are curious.

   The list at the bottom can be used to select additional categories.

   ![Tracing categories of Chrome](/docs/images/tracing-categories-chrome.png)
5. Now you can start the trace recording. Press the **"Start recording"** button when ready.
6. Proceed to use the browser to capture the action you want to trace, and wait for the trace to finish. You can also stop the trace manually by pressing the "Stop" button.

   **Do not close the perfetto UI tab!** Otherwise, tracing will stop and the trace data will be lost.

7. Once the trace is ready, you can find and analyze it in the left menu **"Current Trace"**.

    > **IMPORTANT**: If you want to share a trace, keep in mind that it will contain the URL and title of all open tabs, URLs of subresources used by each tab, extension IDs, hardware identifying details, and other similar information that you may not want to make public.
