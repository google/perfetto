# Trace Summary

**Purpose:** Bundle multiple metrics into a single trace summary specification. This node collects Metrics nodes as inputs and combines them into a `TraceSummarySpec` proto that can be exported for use in trace analysis pipelines.

**How to use:**
- Connect one or more **Metrics** nodes as inputs (only Metrics nodes are accepted)
- Each connected Metrics node defines one metric template with its own ID prefix, value columns, dimensions, and units
- Ensure each Metrics node has a unique **Metric ID Prefix** to avoid conflicts
- Click **Export** to generate a textproto representation of the complete trace summary specification

**Data transformation:**
- This node does not produce tabular output — it aggregates metric definitions
- Each connected Metrics node contributes a `TraceMetricV2TemplateSpec` to the summary
- The summary includes all metric templates with their embedded query trees for self-contained export

**How to build a trace summary:**
1. Create one or more data pipelines (e.g., Table Source → Filter → Aggregation)
2. Add a **Metrics** node to each pipeline to define metric values, dimensions, units, and polarity
3. Connect all Metrics nodes to this Trace Summary node
4. Review the metrics in the accordion panel — each shows its source, dimensions, and value configurations
5. Click **Export** to download the specification as a `.pbtxt` file

**Example workflow:**
1. Pipeline A: `slice` table → Aggregation (`SUM(dur)` by `process_name`) → Metrics (prefix: `slice_dur`)
2. Pipeline B: `counter` table → Counter to Intervals → Aggregation (`AVG(value)` by `track_name`) → Metrics (prefix: `counter_avg`)
3. Connect both Metrics nodes to a single Trace Summary node
4. Export the combined specification containing both metric templates

**Validation rules:**
- All inputs must be Metrics nodes
- At least one Metrics node must be connected
- Each Metrics node must have a unique ID prefix
- All connected Metrics nodes must individually pass validation

**Export:** The exported textproto contains the full `TraceSummarySpec` with embedded query trees, making it self-contained and portable across different Perfetto trace analysis tools.
