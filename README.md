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

Documentation could be found [here](https://georgtree.github.io//).

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

# Initialize callbacks and internal structures
$sim init
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
