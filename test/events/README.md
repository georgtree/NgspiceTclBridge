# Raw event integration test fixtures

Source: official ngspice repository https://sourceforge.net/p/ngspice/ngspice/ci/a80f6e3/tree/, commit a80f6e3.

- `mixed-invert.cir` is `examples/xspice/original-examples/digital_invert.deck`, unchanged.
  It contains an analog pulse, ADC bridge, digital gates and DAC outputs.
- `digital-source.cir` adapts `examples/xspice/d_source/d_source.cir`: retain the eleven-output
  source, remove the two other sources and interactive control section, and add `.tran 100ps 30ns`.
  It has no analog circuit nodes. `d_source-stimulus.txt` is copied unchanged from the same directory.

Load the analog and digital code-model libraries, and set the bridge inputpath to this directory before loading
these netlists. `make test-events-real` does this automatically when NGSPICE_CODEMODEL_DIR is supplied.

Original license notices are in LICENSE.ngspice. The adaptation adds no restriction to the upstream examples.

`op-dc.cir` is a bridge test fixture (not an upstream example). It uses two ADC inputs inside a subcircuit
to check settled OP/DC states, nested sweep coordinates and internal node names.

## Lifecycle coverage

`make test-events` uses the deterministic mock library. In all three modes it checks clearing committed and pending
records while a native worker remains active, flushing queued Tcl events after clear, halt/resume without restarting
the plot, and repeated transient/OP/DC transitions. The worker waits at a controlled point; these tests do not depend
on a sleep being long enough for the simulation to run.

`make test-events-real` runs both transient fixtures through two breakpoint halts and resumes in every mode. Node
histories must match an uninterrupted run. Snapshot times must match the current native time vector: ngspice can
change its accepted-point grid after resume. It also clears at a halted boundary and verifies the resumed suffix,
replaces mixed/digital/analog circuits, and cycles transient/OP/DC/transient with an explicit node selection.

### Upstream requirements

The analysis-cycle and stress tests deliberately reuse numeric plot names after `destroy all`. Ngspice at `a80f6e3`
can reconnect an old event job during creation of a replacement plot, producing stale OP/DC values or missing
transient callbacks. `ngspice-event-plot-reuse-fix.patch` fixes selection of the latest matching job and restores the
new job's data after plot initialization. These regressions fail on that unpatched version; do not weaken their
expected values to accommodate it. This native patch is separate from the bridge patch.

For long OP/DC runs, also apply the previously supplied `ngspice-event-query-cleanup.patch` (or its upstream
replacement). That patch fixes allocations in `ngGet_Evt_NodeInfo`; it does not fix plot-name reuse.

### Repeat-run diagnostics

`make test-events-memory-real NGSPICE_EVENT_RUNS=1000` runs the real lifecycle suite plus nine stress cases:
three modes for each of the mixed transient, pure digital, and OP/DC fixtures. Supply `NGSPICE_LIBRARY` and
`NGSPICE_CODEMODEL_DIR` as for `test-events-real`. No memory-debug Tcl build is required.

Each iteration checks exact history equality with its first run and verifies empty event storage after clear.
It clears numeric data and messages and destroys numeric plots. Every 25 iterations it replaces the circuit on
the same handle: ngspice retains digital event jobs independently of numeric plots until the circuit is destroyed.
The replacement also checks that mode and automatic node selection continue to work.

RSS is printed at startup, after 25 iterations, every 100 iterations and at completion. Linux uses `/proc/self/status`;
Windows uses PowerShell through its absolute SystemRoot path. Other systems report `unavailable`. RSS is diagnostic,
not a portable leak assertion: allocators retain freed arenas, and other ngspice storage can grow independently.
Command-memory cleanup is used only with explicit code-model loading and the mixed/OP/DC fixtures. The conservative
bridge guard rejects `d_source` text and initialization scripts, so the pure-digital case (and runs using spinit)
reports that its RSS includes retained command allocations. Do not interpret that growth as event-storage leakage.
