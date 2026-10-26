# NgspiceTclBridge - drive ngspice (shared library) from Tcl

`ngspicetclbridge` is a Tcl C extension that embeds ngspice as a shared library. It provides commands for loading
circuits, running analyses in ngspice's background thread, and receiving data, messages and events through Tcl's event
loop.

The package targets Tcl 9 and includes a dynamic-loading portability layer for Linux, macOS and Windows.

## What it gives you (at a glance)

- Create simulator handles and issue ngspice commands.
- Load circuits, run analyses and wait for events with optional timeouts.
- Collect ngspice stdout/stderr messages and query vector metadata.
- Read simulation results as Tcl lists or, optionally, real and complex RBC vectors.
- Stream results into live RBC vectors for graph bindings, or create independent snapshots.

Ngspice callbacks buffer live samples for publication on the Tcl thread. Keep the Tcl event loop running for automatic
updates, or call `$sim flush` to publish pending data without processing GUI, timer or idle events. The
`ngspicetclbridge::run` helper waits for completion and flushes automatically before returning.

## Building & requirements

For building, you need Tcl development files, a C compiler and make. The ngspice shared-library interface header is
included in the source tree. At runtime, you need:

- An ngspice shared library, such as `libngspice.so`, `libngspice.dylib` or `ngspice.dll`.
- [argparse](https://github.com/georgtree/argparse) 0.65 or later for all public commands; Tcllib (`textutil::adjust`) is also required.
- Optionally, RBC for vector output (see below).

```sh
git clone https://github.com/georgtree/NgspiceTclBridge.git
cd NgspiceTclBridge
./configure
make
sudo make install
```

Use `--with-tcl=/path/to/tcl/lib` to select the directory containing the desired `tclConfig.sh`.  Ensure Tcl packages
are discoverable through `auto_path` or `TCLLIBPATH`. Pass the ngspice shared library's filename when creating a
simulator handle; it is loaded at runtime.

For Windows, build from an [MSYS2 UCRT64](https://www.msys2.org/) shell with a compatible Tcl and ngspice DLL.

## Documentation

See the [online documentation](https://georgtree.github.io/NgspiceTclBridge/) for the command reference, examples,
troubleshooting and implementation notes.

The detailed RBC guide is maintained in [docs/rbcBindings.ruff](docs/rbcBindings.ruff) and included in the generated
documentation as **RBC vectors support**. It covers live updates, snapshots, ownership, naming, namespaces, replacement,
read-only destinations and graph tests.

Handle subcommands, such as `$sim command bg_run`, communicate directly with the simulator.  Namespace procedures, such
as `ngspicetclbridge::run $sim`, are higher-level helpers.

## Quick start (synchronous operation)

```tcl
package require ngspicetclbridge

# Use the full filename of your ngspice shared library.
set sim [::ngspicetclbridge::new /path/to/libngspice.so]
set circuit [split {Resistor divider
v1 in 0 1
r1 in out 1e3
r2 out 0 2e3
.dc v1 0 5 0.1
.save all
.end} \n]

$sim circuit $circuit
::ngspicetclbridge::run $sim

# Dictionary mapping ngspice signal names to sample lists.
set signals [$sim vectors]
puts [dict get $signals v(out)]

$sim destroy
```

The `circuit` subcommand loads a list of circuit lines and handles removal of the previous circuit. When reusing a
handle, bridge vector storage resets for a new simulation. Clear retained messages and event counters explicitly when
needed:

```tcl
$sim messages -clear
$sim eventcounts -clear
```

These commands apply while the handle is still alive. The `run` helper also destroys previous ngspice plots by default;
use `run -nocleanup $sim` to retain them.

## Optional RBC vector output

Enable RBC support when configuring:

```sh
./configure --with-tcl=/path/to/tcl/lib --with-rbc=/path/to/rbc
make
make install
```

Use Tcl 9 and RBC 0.8.1 or later headers and runtime. `--with-rbc` accepts an RBC source tree, installed header
directory or installation prefix containing `rbcVector.h`, `rbcDecls.h` and `rbcStubLib.c`. The bridge uses RBC stubs
and loads `rbc::vector` only when vector output is requested. The default build has no RBC dependency; list-only use of
an enabled build also works without RBC installed.

Set `-output vector` when creating a handle to receive live vectors from `$sim vectors`.  Alternatively, request
independent snapshots after a simulation, including from a list-output handle:

```tcl
# After a run, before destroying $sim:
set waves [::ngspicetclbridge::readVecsAsync \
    -output vector -namespace ::results -names #auto $sim]
set output [dict get $waves v(out)]
puts [$output length]

# Snapshots belong to the caller and survive simulator destruction.
::rbc::vector destroy {*}[dict values $waves]
```

The dictionary retains the original signal names as keys and returns fully qualified RBC vector names as values. `#auto`
creates fresh vectors in the selected namespace. Live vectors and snapshots have different lifetimes; see [the RBC
guide](docs/rbcBindings.ruff) for the full ownership and naming rules.

## Digital event tests

Headless deterministic tests and real-ngspice tests:

```sh
make test-events TCLLIBPATH="/path/to/argparse /path/to/tcllib"
make test-events-real NGSPICE_LIBRARY=/path/to/libngspice.so NGSPICE_CODEMODEL_DIR=/path/to/ngspice/models
```

On Windows supply the full DLL path; `NGSPICE_DLL` is also supported as a library-directory fallback. The model
directory may contain `analog.cm` and `digital.cm` directly, or `analog/analog.cm` and `digital/digital.cm` from a
source build. Omit it to use code models loaded by your ngspice installation's initialization files.

The real suite uses the upstream mixed-signal digital-inverter example and a purely digital source example. Their
origins and adaptation details are in [test/events/README.md](test/events/README.md). It checks all three
modes, independent results, complete final batches, counters, repeated halt/resume, clear/resume, circuit replacement,
and transient/OP/DC transitions. Deterministic worker tests cover clearing committed and pending data while running.
RBC and Tk are not required.

For longer repeat-run and clear checks with RSS diagnostics:

```sh
make test-events-memory-real NGSPICE_LIBRARY=/path/to/libngspice.so NGSPICE_CODEMODEL_DIR=/path/to/ngspice/models NGSPICE_EVENT_RUNS=1000
```

The default is 200 iterations per circuit/mode combination. See [test/events/README.md](test/events/README.md)
for cleanup policy, platform support, and the upstream plot-name reuse fix required by the new real regressions.

## Testing

The ordinary Tcl suite requires `argparse`, `extexpr` and a real ngspice shared library.  Set `NGSPICE_DLL` to its
**directory**, not its filename:

```sh
# Linux: directory containing libngspice.so
NGSPICE_DLL=/usr/local/lib make test

# Windows / MSYS2: directory containing ngspice.dll
NGSPICE_DLL=C:/Spice64/bin make test
```

`make test` also runs the standalone C callback-ownership regression. Run it separately with `make test-memory`; it
requires Tcl headers and the Tcl link library, but no ngspice, argparse or extexpr. It checks real/complex sample
ownership and snapshot independence rather than RSS, which can remain elevated when allocators retain freed memory.

The headless `make test-rbc` suite also checks native-worker cleanup in list and vector modes,
including repeated runs, halt/resume and controlled exit. Its mock uses POSIX threads or Windows
`_beginthreadex`, without `Tcl_ExitThread`, so missing bridge cleanup cannot be masked by the fixture.
The tests count Tcl thread-exit handlers after joining workers; no memory-debug Tcl build is needed.
List-mode cases run even when the bridge is built without RBC support.

For Tcl-driven simulation loops, explicitly release ngspice's retained command interpreter storage between
complete command sequences:

```tcl
set sim [::ngspicetclbridge::new -noinit -output vector $ngspiceLibrary]
$sim circuit -string $netlist
# Repeat these operations as needed:
::ngspicetclbridge::run $sim
$sim command -clear
```

`command -clear` preserves simulation data and plots. It differs from `vectors -clear`, `messages -clear`, and
`command {destroy all}`. It requires an idle handle created with initialization files disabled. Loading scripts,
aliases, includes or control workers permanently disables cleanup for that handle; ngspice does not expose a safe
join/query for its separate control worker. Detection is conservative, including quoted/comment text and command
substitutions. Cleanup discards labels and unfinished control blocks, so call it only between complete sequences.
The `run` helper does not clear command memory automatically.

The fixture suite checks NULL dispatch, invalid combinations, active-run rejection, halt/clear/resume, and data
preservation in list/vector modes. For the actual library, a separate headless test changes diode parameters,
reuses snapshots, and verifies that cleanup preserves values, plots and messages:

```sh
make test-command-memory-real NGSPICE_LIBRARY=/path/to/libngspice.so NGSPICE_MEMORY_RUNS=10000 TCLLIBPATH="/path/to/argparse /path/to/rbc"
# No RBC dependency for this variant:
make test-command-memory-real NGSPICE_LIBRARY=/path/to/libngspice.so NGSPICE_MEMORY_OUTPUT=list
```

Use the DLL filename on Windows. Linux RSS samples are printed for comparison; they are diagnostic rather than a
portable pass/fail assertion. These checks require no memory-debug Tcl build.

Optional RBC suites have separate targets: `test-rbc`, `test-rbc-graph` and `test-rbc-graph-real`. Their dependencies,
fixture behavior and real-library configuration are documented in [the RBC guide](docs/rbcBindings.ruff); they are not
part of ordinary `make test`.

## Installation layout and removal

Install and uninstall targets honor `--prefix`, `--exec-prefix`, standard directory options and `DESTDIR`. The default
layout is:

| Files                                             | Destination                                         |
|---------------------------------------------------|-----------------------------------------------------|
| Runtime library, Tcl interface and `pkgIndex.tcl` | `$(libdir)/ngspicetclbridge$(PACKAGE_VERSION)`      |
| Public C headers                                  | `$(includedir)`                                     |
| Manpages                                          | `$(mandir)/mann`                                    |
| HTML documentation, assets and license            | `$(datadir)/ngspicetclbridge$(PACKAGE_VERSION)/doc` |

For version 0.5, the package directory is `ngspicetclbridge0.5`. Set both `--prefix` and `--exec-prefix` when relocating
the complete installation:

```sh
./configure --with-tcl=/path/to/tcl/lib \
    --prefix=/desired/prefix --exec-prefix=/desired/prefix
make
make install DESTDIR=/temporary/staging
make uninstall DESTDIR=/temporary/staging
```

`make uninstall` removes the package runtime directory and its installed headers, manpages and documentation. Unrelated
documentation files are retained; empty documentation directories are removed. Keep the corresponding source and
configured build directory, and use the same path overrides and `DESTDIR` for installation and removal.

`DOC_INSTALL_DIR` overrides the complete HTML destination. Its default includes `DESTDIR`; an explicit override must
include the staging directory when staging an installation.

## Installation archives

`make dist` builds the package, stages `make install` and creates `dist/ngspicetclbridge0.5.tar.gz` in the build
directory. `make dist-zip` also creates `dist/ngspicetclbridge0.5.zip` with the same payload.

These are platform-specific installation archives, not source archives. Extract their contents into the desired
installation prefix. The payload follows the configured `lib`, `include` and `share` layout without an enclosing package
directory. Tests, build files and uninstalled examples are excluded.

All configured installation directories must be beneath `prefix`. Custom subdirectories are preserved; `DESTDIR` is not
included in archive paths. Staging does not write to the configured system prefix. `DIST_ROOT` overrides the
staging/output directory and `DIST_NAME` overrides the archive basename, which must be a single directory name.

`make dist-clean` removes the staging tree and archives. Run it separately from `dist` and `dist-zip`, not in the same
parallel make invocation.

## Regenerating documentation

`make doc` regenerates documentation explicitly. Install and archive targets use the existing files in `docs` and do not
require documentation tools.

Generation requires Ruff!, Tcllib (`fileutil`), argparse and Sphinx (`sphinx-build` on `PATH`).  Diagram generation uses
ditaa. Use `TCLLIBPATH` to locate Tcl dependencies and `DOCFLAGS` to pass Ruff options:

```sh
make doc TCLLIBPATH="/path/to/ruff /path/to/tcllib /path/to/argparse"
./config.status --recheck && ./config.status
```

Generated HTML and manpages are written into the source `docs` directory, including for out-of-tree builds. Re-run
configuration after generation, as shown above, so newly generated HTML and static assets are included in the configured
install list. Check the generation output for errors before installing or creating archives.

## Notes and troubleshooting

- **Library lifetime:** repeated unloading and reloading of ngspice has historically exposed possible memory
  corruption. Prefer reusing a simulator handle where practical.
- **Circuit replacement:** the `circuit` subcommand removes the previous circuit automatically.  If you load lines
  yourself through `command circbyline`, issue `command remcirc` before loading a replacement circuit. Always include
  `.end`.
- **Event timeouts:** make sure the background run has started and the Tcl event loop can process its callbacks. Use the
  `run` helper for synchronous operation.
- **Complex results:** list output represents complex samples as `{re im}` pairs; vector output uses complex RBC
  vectors.
- **Repeated runs:** clear retained messages and event counters when needed. Bridge data buffers reset for a new
  simulation; caller-owned RBC snapshots still require explicit cleanup.

# Interactive help

All public methods have interactive help. To get information about command and its arguments call it with `-help`
switch:

```tcl
package require ngspicetclbridge
ngspicetclbridge::run -help
```

```text
Run a simulation and wait for completion.. Can accepts unambiguous prefixes
instead of switches names. Allows switches to appear after parameters.
    Switches:
        -nocleanup - Boolean. If provided, results from previous run are
            retained.
        -help - Help switch, when provided, forces ignoring all other switches
            and parameters, prints the help message to stdout, and returns up to 1
            levels above the current level.
    Parameters:
        sim - Simulator handle that is returned by ngspicetclbridge::new.
```

Best to do it in interactive console, see [tkcon](https://github.com/bohagan1/TkCon)
