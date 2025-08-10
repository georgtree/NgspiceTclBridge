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

## Availible commands (instance subcommands)

### `::ngspicetclbridge::new path`

Load dynamic library, path should be provided in native form for target OS.  Every `::ngspicetclbridge::new` returns a
command (e.g. `::ngspicetclbridge::s1`). The following subcommands operate on that instance.

``` tcl
set sim [::ngspicetclbridge::new /usr/local/lib/libngspice.so]
```

### `init`

Initialize the ngspice shared instance and the bridge’s data structures.

``` tcl
$sim init  ;# returns ngspice init code (int)
```

### `command string`

Send an arbitrary Ngspice command line (e.g., `bg_run`, `circbyline` ..., `.save`, `.tran`, `.dc`).

```tcl
$sim command "circbyline v1 in 0 1"
$sim command bg_run
```

Returns the ngspice result code (int).

### `waitevent name ?timeout_ms?`

Block until a named event is observed, the instance is aborted/destroyed, or the timeout expires.

The table of possible events:

| Event name        | Ngspice callback function name | When it is called                                                                                                                 |
|:------------------|:-------------------------------|:----------------------------------------------------------------------------------------------------------------------------------|
| `send_char`       | `SendChar`                     | Whenever Ngspice produces a line of text on stdout or stderr.                                                                     |
| `send_stat`       | `SendStat`                     | When Ngspice’s simulation status changes (e.g., --ready--, tran 50.1%, convergence messages).                                     |
| `controlled_exit` | `ControlledExit`               | When Ngspice exits, either due to an error or after a quit command from Tcl/Ngspice.                                              |
| `send_data`       | `SendData`                     | During an analysis, whenever Ngspice sends a row of vector values (time step or sweep point) to the callback.                     |
| `send_init_data`  | `SendInitData`                 | At the start of a run, when Ngspice sends metadata for all vectors in the current plot (names, types, indexes, real/complex).     |
| `bg_running`      | `BGThreadRunning`              | When the Ngspice background thread changes state: running=false means it just started running, running=true means it has stopped. |

The table with typical run timeline:

| Time | Tcl Script Action         | Ngspice Core Activity       | Bridge Callback Fired    | Tcl Event Name Seen |
|------|---------------------------|-----------------------------|--------------------------|---------------------|
| t0   | `set s [..::new ..]`      | Library loaded              | *(none)*                 | *(none)*            |
| t1   | `$s init`                 | Initialization completed    | *(none)*                 | *(none)*            |
| t2   | `cirPass …`               | Parses circuit line         | `send_char("...")`       | `send_char`         |
| t3   | *(more circbyline calls)* | Parses circuit line         | `send_char("...")`       | `send_char`         |
| t4   | `$s command bg_run`       | Start background simulation | `bg_running(false)`      | `bg_running`        |
| t5   | *(analysis setup)*        | Build vector table          | `send_init_data()`       | `send_init_data`    |
| t6   | *(analysis running)*      | First data point            | `send_data(...)`         | `send_data`         |
| t7   | *(analysis running)*      | More points                 | `send_data(...)`         | `send_data`         |
| t8   | *(analysis running)*      | Status change               | `send_stat("...")`       | `send_stat`         |
| t9   | *(analysis completed)*    | Simulation ready            | `send_stat("--ready--")` | `send_stat`         |
| t10  | *(BG thread exits)*       | Background thread exits     | `bg_running(true)`       | `bg_running`        |
| t11  | `$s command quit`         | ngspice quits               | `controlled_exit(...)`   | `controlled_exit`   |
| t12  | `$s destroy`              | Teardown                    | *(no further calls)*     | *(command removed)* |

Result dictionary of the command:

- `fired` — `0|1`, whether the event occurred during this wait
- `count` — cumulative total count for that event so far
- `status` — `ok | timeout | aborted`

Examples:

```tcl
$sim waitevent send_stat 1000
# -> {fired 1 count 1 status ok}

$sim waitevent send_stat 1000
# -> {fired 0 count 1 status timeout}  ;# nothing new in the next 1s
```

If you want “fresh” waits, you can clear counts with `eventcounts -clear`.


### `vectors ?-clear?`

Holds **asynchronously accumulated** vector values (built from `send_data` events) in a dict:

- Without flags: returns the dict.
- With `-clear`: empties the dict and returns **nothing**.

Examples:

```tcl
$sim vectors
# -> v(out) {0.0 0.1 0.2 ...} v(in) {...} v-sweep {...}

$sim vectors -clear
# -> (no result; succeeds)
```

**Warning**: accumulation of data continues even if you run new circuit or analysis until you explicitly clear the data
storage.


### `initvectors ?-clear?`

Holds **initial vector metadata** (built from send_init_data) in a dict:

- Without flags: returns the dict.
- With `-clear`: empties the dict and returns nothing.

```tcl
$sim initvectors
# -> out {number 1 real 1} in {number 2 real 1} v-sweep {number 3 real 1}
```

**Warning**: accumulation of data **do not** continues after call `bg_run`, new metadata replace the old one in the
storage.


### `asyncvector name`

Fetch the current values of a named vector on demand via ngspice `ngGet_Vec_Info`. Works after the simulation has
produced any data (not necessarily the complete vector).

- Real vectors return a flat list of doubles.
- Complex vectors return a list of `{re im}` pairs.

Examples:

```tcl
$sim asyncvector out
# -> {0.0 0.066666... 0.133333... ...}

$sim asyncvector V(9)
# -> {{0.01 0.00} {0.02 0.00} ...}   ;# if complex
```

Return error if the vector does not exist.


### `messages ?-clear?`

Queue of textual messages captured from Ngspice (stdout/stderr) and bridge status lines.

- Without flags: returns the list of messages.
- With `-clear`: empties the queue and returns nothing.

```tcl
join [$sim messages] \n
# stdout ******
# stdout ** ngspice-44.x shared library
# ...
# # status[0]: --ready--
```

**Warning**: accumulation of messages continues even if you run new circuit or analysis until you explicitly clear the
data storage.


### `eventcounts ?-clear?`

Get or reset the cumulative event counters for this instance.

- Without flags: returns a dict:

```text
send_char N  send_stat N  controlled_exit N send_data N  send_init_data N  bg_running N
```

- With `-clear`: zeros all counts and returns nothing.


### `abort`

Set an internal abort flag and wake any waiters (useful to force waitevent to return). This does **not** free the
instance.

### `isrunning`

Asynchronous call to check if background thread is running, return `1` if true.

### `destroy`

Delete the instance command. This:

- Marks the context as destroying
- Wakes any waiters
- Quiesces the ngspice background thread
- Defers and performs full cleanup


## Notes & internals

- **Threading**: ngspice callbacks arrive on ngspice’s side; the bridge queues Tcl events and processes them on the
  thread that created the instance. Synchronization uses `Tcl_Mutex` and `Tcl_Condition`. Waiters use either
  `Tcl_ConditionWait` (no timeout) or a short sleep/poll loop (with timeout).

- **Event counts**: The count returned by waitevent is cumulative since instance creation (not “just this wait”). Use
  eventcounts -clear if you prefer to measure deltas from a known zero.

- **Complex vs real vectors**: asyncvector checks `vinfo->v_flags & VF_COMPLEX`. If set, you get `{re im}` pairs,
  otherwise doubles.

- **Portability**: `PDl_OpenFromObj/PDl_Sym/PDl_Close` abstract `dlopen/GetProcAddress/FreeLibrary`. The library path is
  taken from a Tcl path object to handle platform Unicode semantics.


## Troubleshooting

- `waitevent` **always times out**: make sure you actually started the background run (`$sim command bg_run`) and you
  allow the Tcl event loop to process events (update) between waits when appropriate.

- **Circuit is not loaded**: ensure your circuit issues `.end` directive.

- **Complex data surprises**: if you expected real data but get `{re im}` pairs, your vector is complex per
  ngspice. Handle both cases in your Tcl code if needed.

- **Multiple runs**: clear old data between runs if you want pristine buffers: `$sim vectors -clear`, `$sim initvectors
  -clear`, and optionally `$sim eventcounts -clear`.
