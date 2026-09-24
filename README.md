# NgspiceTclBridge - drive ngspice (shared library) from Tcl

`ngspicetclbridge` is a Tcl C extension that embeds Ngspice (built as a shared library) and exposes a small, thread-safe
command API for loading circuits, running analyses in the ngspice background thread, and receiving data/messages/events
back into Tcl.

It targets Tcl 9.0 and works on Linux/macOS/Windows. Dynamic loading is abstracted via a tiny portability layer.

## What it gives you (at a glance)

- Create one or more ngspice instances inside Tcl.
- Issue ngspice commands (e.g. bg_run, circbyline ...).
- Wait for events (send_stat, send_data, etc.) with optional timeouts.
- Collect messages from ngspice stdout/stderr.
- Fetch vector metadata and values (both asynchronously collected and on-demand).

All Ngspice → Tcl crossings are deferred onto Tcl’s event loop, so you stay in the safe thread.

## Building & requirements

Requirements:

- ngspice built as a shared library (e.g. libngspice.so, libngspice.dylib, or libngspice.dll) with the sharedspice
  interface (sharedspice.h).
- Tcl headers/libs (9.0).

To install, run following commands:
- `git clone https://github.com/georgtree/NgspiceTclBridge.git`
- `./configure`
- `sudo make install`

During installation manpages are also installed.

For test package in place run `make test`.

For package uninstall run `sudo make uninstall`.

## Documentation

Documentation could be found [here](https://georgtree.github.io/NgspiceTclBridge/).

## Notes

The commands that are subcommand to the simulator command, like `$s1 command bg_run` are considered low level
and communicate/interact directly with the simulator. On the other side, commands that accept simulator as a
parameter are considered as "helper" commands, built on top of low-level commands.

## Warnings

This library should be considere semi-stable now. It works well for **single** loading of shared library per process,
without unloading and loading again. It also works with multiple unloadings (destroying) and loadings of the same
library, but I've encountered a few possible memory corruption cases. I am trying my best to eliminate them, but I am
not in full control of Ngspice shared library behaviour.

One of the instances I've encountered is the running two circuit in sequence, with pattern load circuit->run->load
circuit->run, and then try destroying the instance. This could lead to segmentation violation, unless the command
`remcirc` for unloading previous circuit is issued before loading new circuit. It is done automatically now if you
previously loaded the circuit with `circuit` command. But in case you want to load circuit with command `circbyline`,
you need to issue `remcirc` command yourself.

## Quick start (synchronous operation)

Package loading and initialization:

``` tcl
package require ngspicetclbridge

# Path to your ngspice shared library
set ngspiceLibPath /path/to/libngspice.so

# Create a new instance bound to a Tcl command
set sim [ngspicetclbridge::new $ngspiceLibPath]
```

To feed a circuit one line at a time via ngspice’s circbyline:

``` tcl
set resDivCircuit {
    Resistor divider
    v1 in 0 1
    r1 in out 1e3
    r2 out 0 2e3
    .dc v1 0 5 0.1
    .save all
    .end
}
```

Pass resistor divider circuit to Ngspice, and start the run (in background thread), and wait for completion:

``` tcl
$sim circuit [split $resDivCircuit \n]
run $sim
```

Get vector data dictionary saved in internal buffer:

``` tcl
$sim vectors
```

Destroy instance of simulator (removes instance command, frees internal storages and simulator context)

``` tcl
$sim destroy
```

If you want to continue work with current simulator instance, you need to clear internal messages storage
with next commands:

``` tcl
$s1 messages -clear
```

Vector storage is resetted after run new simulation, so no need to explicitly frees vectors data.

## Troubleshooting

- `waitevent` **always times out**: make sure you actually started the background run (`$sim command bg_run`) and you
  allow the Tcl event loop to process events (update) between waits when appropriate.

- **Circuit is not loaded**: ensure your circuit issues `.end` directive.

- **Complex data surprises**: if you expected real data but get `{re im}` pairs, your vector is complex per
  ngspice. Handle both cases in your Tcl code if needed.

- **Multiple runs**: clear old event data between runs if you want pristine event counters and messages buffer:
  `$sim messages -clear` and  `$sim eventcounts -clear`. Data buffer and saved vectors resetted after run of
  the new simulation to prevent data mixing.

## Checking callback memory ownership

`make test-memory` runs a standalone C regression against the bridge's actual `SEND_DATA` event handler.
It requires Tcl headers and its link library, but does not load ngspice or require argparse/extexpr.
The same check runs before the existing suite with `make test`. For an existing build directory, regenerate
its Makefile with `./config.status` after applying this change.

The test performs 64 repeated real/complex data cycles, retaining snapshots while additional samples arrive.
It verifies sample values, snapshot independence, and that temporary string/list objects are either released
or owned after each batch. This checks object ownership rather than RSS, which can remain elevated because
allocators retain freed memory for reuse.

The callback now releases temporary dictionary keys and complex-sample containers after insertion. Existing
keys do not adopt newly allocated equal key objects; complex list append retains the elements rather than
their temporary container. Both otherwise remain leaked even after clearing the bridge's vector dictionary.
The fix preserves the existing command interface and sample representation.

## Optional RBC vector output

Build with `./configure --with-rbc=/path/to/rbc` (a source tree, installed header directory, or installation prefix).
The default `--without-rbc` build has no RBC dependency. An enabled build uses RBC stubs and loads `rbc::vector`
only when vector output is requested; list-only use still works without RBC installed. Use the current rbc-tk9
version with complex vectors and `-literal` name support.

```tcl
namespace eval ::wave {}
set sim [ngspicetclbridge::new /path/to/libngspice.so -output vector -namespace ::wave -ifexists error]
# Load a circuit, then start it with $sim command bg_run.
# After metadata has been processed by the event loop:
set signals [$sim vectors]
# Example result: time ::wave::time v(out) ::wave::v(out)
set samples [::wave::v(out) range 0 end]
$sim vectors -clear
# Commands survive clear and are refilled by subsequent samples/runs.
set copy [$sim asyncvector v(out) -name ::savedOutput]
set listCopy [$sim asyncvector v(out) -output list]
```

Live vector updates run in the Tcl thread, in batches. Run the event loop to receive them. Complex signals use complex
RBC vectors. A graph can bind directly to the returned names through `-xdata` and `-ydata`; bindings survive clear and
new runs. Use an existing dedicated namespace to avoid collisions such as the global Tcl `time` command.

`-ifexists error` is the default. `replace` adopts an existing same-type RBC vector in place, retaining graph bindings;
it never overwrites an unrelated Tcl command. Previously attached vectors are reused regardless of the collision policy.
Destroying the handle destroys only live vectors it created. Adopted vectors and on-demand `asyncvector` snapshots remain
caller-owned. Do not modify, delete or rename live vectors while they are attached. Output errors are reported once through
Tcl's background error handler and remain available from `SIM vectors`; a new plot retries initialization.

`asyncvector` accepts `-output list|vector`, `-ifexists error|replace`, and `-name destination` overrides. Snapshots never
become live bindings and cannot overwrite an attached live vector. Snapshot data is copied under ngspice's realloc lock;
RBC operations happen after unlocking. `asyncvector -info name` is unchanged.

Signal names with balanced parentheses, including `v(1)`, are preserved literally. Unsafe names, namespace separators,
and the reserved `_raw_` prefix are encoded using the same `_raw_` plus uppercase UTF-8 hex convention as tclsimrawreader.
New vectors have no mapped array variable. `SIM vectors` returns the exact raw-name-to-command mapping.

Optional checks use a deterministic shared-ngspice fixture, including a worker thread; they do not need a circuit solver:

```sh
make test-rbc TCLLIBPATH="/path/to/rbc/package"
make test-rbc-graph TCLLIBPATH="/path/to/rbc/package /path/to/Tk/package"
```

`test/rbc-graph.test` is a separate display-dependent check of graph bindings during streaming, clear and rerun. Neither
optional suite runs through `test/all.tcl`. The graph check skips when Tk/RBC or a display is unavailable. These fixture
checks complement the existing tests against a real ngspice shared library.

### Graph integration with real ngspice

The separate `test-rbc-graph-real` target uses your actual ngspice shared library and the four-bit adder in
`examples/fourbit-adder.cir`. It does not build or load the mock library. The circuit retains its 10 microsecond
transient analysis and saves `v(9)`, `v(10)`, `v(11)` and `v(12)`.

```sh
# Linux
make test-rbc-graph-real NGSPICE_LIBRARY=/usr/local/lib/libngspice.so TCLLIBPATH="/path/to/rbc"

# Windows / MSYS2: pass the complete DLL filename, using forward slashes.
make test-rbc-graph-real NGSPICE_LIBRARY="C:/Spice64/bin/ngspice.dll" TCLLIBPATH="C:/path/to/rbc"
```

Requires an RBC-enabled bridge, full RBC/Tk with a working display, and an ngspice library compatible with the bridge.
Missing requirements cause an explicit error. This target is excluded from ordinary `make test`.

A window plots all four output voltages as the simulation runs. The test observes at least two increases in sample count
while ngspice is active, checks completion at 10 microseconds, and compares every displayed vector and its time scale with
independent list snapshots from ngspice. It then clears the live vectors and repeats the simulation without recreating the
graph elements or vector commands. Closing the test window cancels the test; otherwise it closes automatically on completion.

The timeout is 180 seconds per run. For a slower build, use `NGSPICE_TEST_TIMEOUT_MS=600000`. This is deliberately a long,
two-run integration test. If a machine completes the entire simulation before two live updates can be observed, the test
reports that condition rather than claiming that streaming was verified. Initialization files are disabled with `-noinit`
so local ngspice settings do not alter the example.

## Installation layout and removal

Installation follows the rbc-tk9 layout and honors the directories selected by `configure`:

- The package library, Tcl scripts and `pkgIndex.tcl` go together in `$(libdir)/$(PACKAGE_NAME)$(PACKAGE_VERSION)`.
- Any public headers and stub client sources go in `$(includedir)`; executable binaries go in `$(bindir)`.
- Manpages go in `$(mandir)/mann`.
- HTML documentation, its image/static resources, and `LICENSE` go in `$(datadir)/$(PACKAGE_NAME)$(PACKAGE_VERSION)/doc`.

Use `--prefix`, `--libdir`, `--includedir`, `--datadir` and `--mandir` at configure time to change these locations.
All install and uninstall targets honor `DESTDIR` for staging:

```sh
./configure --prefix=/your/prefix
make
make install DESTDIR=/your/staging/root
make uninstall DESTDIR=/your/staging/root
```

`make uninstall` runs `uninstall-binaries`, `uninstall-libraries` and `uninstall-doc`. It removes the package-owned
runtime directory, but removes only this package's named files from shared binary, include and documentation paths.
Unrelated documentation files are retained; empty documentation directories are removed. Keep the configured build
and source tree to uninstall the corresponding installation, and use the same path overrides for install and uninstall.

`DOC_INSTALL_DIR` can override the complete HTML destination. As in rbc-tk9, its default already includes `DESTDIR`;
when overriding it explicitly, include the staging root yourself if needed.

## Installation archives

`make dist` follows rbc-tk9: it builds the package, stages `make install` under the build directory, and creates
`dist/$(PACKAGE_NAME)$(PACKAGE_VERSION).tar.gz`. `make dist-zip` creates the same payload as a ZIP file as well.
`make dist-clean` removes this package's staging directory and both archives.

```sh
make -j4 dist
make dist-zip
```

These are platform-specific installation archives, not source distributions. Their contents are relative to the
configured installation prefix: normally `lib/`, `include/`, and `share/`, without an enclosing package directory.
Extract or merge the archive contents into the desired prefix. The payload uses the same install targets as normal
installation, including the built library, Tcl runtime files, public headers, manpages, HTML resources and license.
Tests, build files and examples that are not installed by `make install` are not included.

Configured installation directories must be below `prefix`; `dist` reports an error if one is outside it.
When using `--with-tcl` and a custom prefix, set `--exec-prefix` to the same prefix if TEA would otherwise inherit
Tcl's execution prefix, for example `sh ./configure --prefix=/opt/mypackages --exec-prefix=/opt/mypackages`. Custom
subdirectories inside that prefix are preserved. `DESTDIR` is not included in archive paths. The staging install does
not write to the configured system prefix. `DIST_ROOT` and `DIST_NAME` may be overridden to choose the archive output
directory and name; `DIST_NAME` must be a single directory name. Run `dist-clean` separately, not alongside `dist`
or `dist-zip` in the same parallel make invocation.
