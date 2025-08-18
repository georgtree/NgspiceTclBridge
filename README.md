# NgspiceTclBridge — drive ngspice (shared library) from Tcl

`ngspicetclbridge` is a Tcl C extension that embeds Ngspice (built as a shared library) and exposes a small, thread-safe
command API for loading circuits, running analyses in the ngspice background thread, and receiving data/messages/events
back into Tcl.

It targets Tcl 8.6–9.0 and works on Linux/macOS/Windows. Dynamic loading is abstracted via a tiny portability layer.

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
- Tcl headers/libs (8.6–9.0).

To install, run following commands:
- `git clone https://github.com/georgtree/NgspiceTclBridge.git`
- `./configure`
- `sudo make install`

During installation manpages are also installed.

For test package in place run `make test`.

For package uninstall run `sudo make uninstall`.

## Documentation

Documentation could be found [here](https://georgtree.github.io/NgspiceTclBridge/). 

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
proc cirPass {sim circuitText} {
    foreach line [split $circuitText "\n"] {
        if {[string trim $line] ne ""} {
            $sim command [concat circbyline $line]
        }
    }
}
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

Pass resistor divider circuit to Ngspice, and start the run background thread:

``` tcl
cirPass $sim $resDivCircuit
$sim command bg_run
```

Wait for an event (e.g., status becoming ready):

``` tcl
set res [$sim waitevent send_stat 1000]
```

Explicitly process pending events handlers (without processing the events the messages, vector data and status will
not be written into internal data storages)

``` tcl
update
```

Get vector data dictionary saved in internal buffer:

``` tcl
$sim vectors
```

Destroy instance of simulator (removes instance command, frees internal storages and simulator context)

``` tcl
$sim destroy
```

If you want to continue work with current simulator instance, you need to clear internal vector/messages storage
with next commands (otherwise data will be appended to data from previous simulation):

``` tcl
$s1 vectors -clear
$s1 messages -clear
```

## Troubleshooting

- `waitevent` **always times out**: make sure you actually started the background run (`$sim command bg_run`) and you
  allow the Tcl event loop to process events (update) between waits when appropriate.

- **Circuit is not loaded**: ensure your circuit issues `.end` directive.

- **Complex data surprises**: if you expected real data but get `{re im}` pairs, your vector is complex per
  ngspice. Handle both cases in your Tcl code if needed.

- **Multiple runs**: clear old data between runs if you want pristine buffers: `$sim vectors -clear`, `$sim initvectors
  -clear`, and optionally `$sim eventcounts -clear`.
