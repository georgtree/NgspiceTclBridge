package require argparse 0.65

namespace eval ::ngspicetclbridge {}

proc ::ngspicetclbridge::configure {args} {
    # Queries or changes package-wide representation and collision defaults in this Tcl interpreter.
    #  -output mode - Default output representation: list or vector; initially list.
    #  -ifexists policy - Default destination collision policy: error or replace; initially error.
    #
    # With no arguments, returns all settings as an option/value dictionary. With one option, returns its value.
    # With option/value pairs, validates all settings before applying them and returns the complete configuration.
    # Invalid options or values, or unavailable RBC support leave the configuration unchanged. Selecting
    # vector mode requires an RBC-enabled build and a loadable rbc::vector package; list queries/settings do not
    # load RBC. Defaults are independent in separate interpreters.
    #
    # New simulator handles capture these defaults unless -output or -ifexists is supplied explicitly at creation.
    # Existing handles retain their settings. Per-read overrides take precedence over the handle default.
    # This public command is not namespace-exported; call it as ::ngspicetclbridge::configure.
    # The collision policy affects vector output only; it can be configured while using list mode or without RBC.
    # Replacement still requires a compatible, writable destination. Namespace selection and naming are unchanged.
    #
    # Example:
    # ```tcl
    # set saved [::ngspicetclbridge::configure]
    # ::ngspicetclbridge::configure -output vector -ifexists replace
    # ::ngspicetclbridge::configure -output
    # ::ngspicetclbridge::configure {*}$saved
    # ```
    #
    # Synopsis: ?option?
    # Synopsis: ?-output list|vector? ?-ifexists error|replace?
    # Returns: selected option value for a single-option query; otherwise all settings as a dictionary.
    if {([llength $args]==1) && ([lindex $args 0] ne {-help})} {
        set arguments [argparse -inline -help {Query or change package defaults. With no arguments returns all\
                                                       settings.} {
            {-output -key query -value output -help {Query the default output representation}}
            {-ifexists -key query -value ifexists -help {Query the default collision policy}}
        }]
        return [NativeConfigure [dict get $arguments query] {}]
    }
    set arguments [argparse -inline -help {Query or change package defaults atomically. Defaults are local to this\
                                                   interpreter.} {
        {-output= -enum {list vector} -help {Default representation: list or vector}}
        {-ifexists= -enum {error replace} -help {Default collision policy: error or replace}}
    }]
    return [NativeConfigure {} $arguments]
}

proc ::ngspicetclbridge::new {args} {
    # Load dynamic library, path should be provided in native form for target OS.  Every `::ngspicetclbridge::new`
    # returns a command (e.g. `::ngspicetclbridge::s1`). The following subcommands operate on that instance.
    # Optionally the spinit and/or spiceinit initialization files could be forbidden for loading.
    #  -nospinit - inhibits spinit
    #  -nospiceinit - inhibits spiceinit
    #  -noinit - inhibit both spinit and spiceinit
    #  -output - list or vector; inherits the package configuration (initially list); controls live storage and
    #   snapshots
    #  -ifexists - error or replace for initial vector destinations; inherits the package configuration (initially
    #   error)
    #  -namespace - destination for live vectors and snapshots; created at handle construction if missing
    #
    # Namespace:
    # - Relative names resolve against the handle creation caller; the fully qualified name is retained.
    # - Missing namespaces and parents are created. Empty names are rejected.
    # - Destroying the handle does not delete the namespace.
    # - Without this option, live vectors use the creation namespace; snapshots use their caller's namespace.
    # - Use a dedicated namespace to avoid collisions, such as the signal time and Tcl's global time command.
    #
    # Vector output:
    # - Requires an RBC-enabled build and `rbc::vector` with literal-name support.
    # - With replace, existing same-type vectors are adopted, preserving their commands and graph clients.
    # - Unrelated commands and vectors maintained by another simulator cannot be replaced.
    #
    # Example:
    #```tcl
    # set sim [::ngspicetclbridge::new /usr/local/lib/libngspice.so]
    #```
    # Synopsis: ?-nospinit|-nospiceinit|-noinit? libpath ?-output list|vector? ?-ifexists error|replace?
    # ?-namespace name?
    # Returns: command name to work with Ngspice instance, references as `SIM` later
    set arguments [argparse -inline -mixed -help {Create a simulator handle.} {
        {-nospinit -help {Do not read spinit}}
        {-nospiceinit -help {Do not read spiceinit}}
        {-noinit -help {Do not read either initialization file}}
        {-output= -enum {list vector} -help {Output representation; overrides the configured default}}
        {-ifexists= -enum {error replace} -help {Destination collision policy; overrides the configured default}}
        {-namespace= -help {Destination namespace; relative to the caller, created if missing}}
        {libpath -help {Path to the ngspice shared library}}
    }]
    return [uplevel 1 [list ::ngspicetclbridge::NativeNew $arguments]]
}

namespace eval ::ngspicetclbridge::SIM {}

proc ::ngspicetclbridge::SIM::eventnodes {handle args} {
    # Discovers event nodes and selects raw event collection independently of numeric output mode.
    # -all - Select all names returned by ngSpice_AllEvtNodes, including internal subcircuit nodes.
    # -nodes names - Select an explicit list; an empty list disables recording.
    # -mode mode - changes, snapshot or pernode; initially changes.
    #
    # Call after loading the circuit and while idle, before run/bg_run. Without options, returns supported, available,
    # selected and mode. Raw symbols are optional; unsupported libraries retain analog functionality.  Each node is
    # registered once per loaded circuit. Changing selection or mode clears event history and known states.  Resume
    # retains registrations and histories. Disabled registrations remain alive until circuit replacement or handle
    # destruction.  Event collection supports externally driven transient, operating-point and DC sweep analyses. After registering nodes, use the
    # circuit subcommand to replace the circuit while idle. Mode and -all are retained; explicit -nodes selections,
    # known states and histories are cleared. Direct source/reset/remcirc/setcirc/circbyline commands and
    # circuit-changing scripts or aliases remain unsupported.
    #
    # Transient modes collect through SendRawEvtData; OP/DC modes read settled node histories at numeric output
    # points. All modes preserve ngspice's printable strings, including drive strength.
    # changes publishes complete accepted-step batches; snapshot publishes every selected node at accepted analog
    # output times, carrying unchanged values forward; pernode exposes individual events immediately.
    # Complete batches become visible at the next analog output callback, or after completion/halt for the final
    # batch. Pure digital transient circuits use ngspice's time-only output. There is no wall-clock sampling timer.
    #
    # Synopsis: ?-all|-nodes names? ?-mode changes|snapshot|pernode?
    # Returns: dictionary describing support, available nodes, selected nodes and mode.
    set arguments [argparse -inline -help {Discover or select raw event nodes; requires an idle simulator.} {
        {-all -help {Select all event nodes}}
        {-nodes= -help {List of event node names; empty disables recording}}
        {-mode= -enum {changes snapshot pernode} -help {Publication mode}}
    }]
    return [uplevel 1 [list ::ngspicetclbridge::NativeCall $handle eventnodes $arguments]]
}

proc ::ngspicetclbridge::SIM::eventdata {handle args} {
    # Reads an independent snapshot of published string-valued event data.
    # -clear - Discard stored and pending event history; retain selection and latest known states.
    #
    # Result keys are mode, analysis, nodes and batches. nodes maps names to dictionaries containing type.
    # For transient analysis, in pernode mode each node also has events: a list of {eventTime stringValue last} records.
    # In changes mode batches contains {time sampleTime events {{node eventTime stringValue last} ...}} dictionaries.
    # In snapshot mode batches contains {time sampleTime values {node stringValue ...} known {node ...}} dictionaries.
    # An uninitialized node has an empty value and is absent from known; this differs from ngspice's explicit U state.
    # last is only the end of ONE node's native batch, never an all-nodes completion marker. Event times can be
    # out of order across nodes; callback order and repeated same-time transitions are preserved.
    #
    # analysis is none before plot initialization, then tran, op or dc. Unsupported analyses report an error.
    # OP/DC batches use point (zero-based output index) and coordinates (sweep-name/value dictionary; empty for OP)
    # instead of time. Snapshot batches retain values and known; changes batches contain {node value} records.
    # OP/DC pernode events are dictionaries with point, coordinates and value. They have no time or last fields.
    # OP/DC changes and pernode record initial states and subsequent value changes; snapshot records every point.
    # See the Digital events data guide for examples and the native query cleanup prerequisite for repeated OP/DC use.
    #
    # Data is copied from mutex-protected C storage on this call; no Tcl event-loop turn is required. A read or flush
    # cannot complete a partially delivered timestep. Clear does not erase ngspice's own saved event data or reset
    # counters; later callbacks continue recording. A fresh plot clears histories; halt/resume preserves them.  Decoder
    # errors are retained and returned here until a new plot or configuration resets them.  Numeric
    # -output/-namespace/-ifexists settings do not apply; event values are always Tcl strings.
    #
    # Synopsis: ?-clear?
    # Returns: dictionary of published event data.
    set arguments [argparse -inline -help {Read raw event strings, or clear their history.} {
        {-clear -help {Discard event history and pending records while retaining current states}}
    }]
    return [uplevel 1 [list ::ngspicetclbridge::NativeCall $handle eventdata $arguments]]
}

proc ::ngspicetclbridge::SIM::command {handle args} {
    # Sends an arbitrary Ngspice command line (e.g., `bg_run`, `circbyline` ..., `.save`, `.tran`, `.dc`). If we provide
    # `-capture` switch, it returns not only the result code, but also the next message from stdout of Ngspice. Also, if
    # command is send in the moment background thread is starting or stopping, it will be deferred to the time is could
    # be safely sent, and in that case `-capture` message and command results code are not availaible as a return of
    # `command`.
    #  -capture - Return captured output along with the result code.
    #  -clear - Release retained ngspice command memory at an idle boundary.
    #  string - Command string; omitted with -clear.
    #
    # Command memory:
    # - `command -clear` calls `ngSpice_Command(NULL)` and returns its result code. An empty string does not do this.
    # - Use a handle created with -noinit (or both -nospinit and -nospiceinit), driven directly from Tcl.
    # - Cleanup is rejected during background startup, execution or shutdown, native calls, and pending commands.
    #   It never halts a simulation or defers cleanup. After bg_halt returns, cleanup permits a later bg_resume.
    # - Scripts, aliases, includes and control workers disable cleanup permanently for that handle. Detection is
    #   conservative, including matching text in comments. Initialization files also disable cleanup because
    #   ngspice exposes no reliable query/join for its separate control worker.
    # - Cleanup discards command history, labels and unfinished control blocks. Only call it between complete
    #   command sequences. It preserves circuit state, plots, snapshots, live vectors/lists and bridge messages.
    # - Unlike `destroy all`, `vectors -clear` and `messages -clear`, this releases the command interpreter's storage.
    #   The run helper does not do this automatically, so existing control-script behavior remains unchanged.
    #
    # Example:
    #```tcl
    # $sim command {circbyline v1 in 0 1}
    # $sim command bg_run
    #```
    # Returns: Ngspice result code, or dictionary with result code and the message
    #
    # The handle argument is supplied automatically when called as $handle command.
    # Synopsis: ?-capture? string
    # Synopsis: -clear
    set arguments [argparse -inline -mixed -help {Send a command to ngspice.} {
        {-clear -help {Release command memory at an idle boundary; requires a script-free -noinit handle}}
        {-capture -help {Capture command output along with its result code}}
        {string? -help {Ngspice command line}}
    }]
    if {[dict exists $arguments clear]} {
        if {[dict exists $arguments string] || [dict exists $arguments capture]} {
            return -code error {-clear cannot be combined with a command string or -capture}
        }
    } elseif {![dict exists $arguments string]} {
        return -code error {specify a command string or -clear}
    }
    return [uplevel 1 [list ::ngspicetclbridge::NativeCall $handle command $arguments]]
}

proc ::ngspicetclbridge::SIM::circuit {handle args} {
    # Sends an circuit to Ngspice in form of the Tcl list, each element is the line of the netlist. If switch
    # `-string` is provided, the input netlists could be passed as ordinary string with newline character as a
    # separator between netlists lines.
    #  -string - allows to pass circuit as ordinary string
    #  circuit -  list of string
    #
    # Example:
    #```tcl
    # set resDivCircuit {
    #     Resistor divider
    #     v1 in 0 1
    #     r1 in out 1e3
    #     r2 out 0 2e3
    #     .dc v1 0 5 0.1
    #     .save all
    #     .end
    # }
    # $sim circuit [split $resDivCircuit \n]
    # $sim command bg_run
    #```
    # Synopsis: ?-string? circuit
    # Returns: Ngspice result code
    #
    # The handle argument is supplied automatically when called as $handle circuit.
    #
    # Loading a replacement requires an idle simulator. The old circuit is removed before loading the new one; a load
    # failure does not restore it. Raw event histories and explicit node selections are cleared. The event mode and -all
    # selection are retained, with -all resolved against the new circuit before this call returns.  If automatic event
    # registration fails, the new circuit remains loaded and this command reports an error.
    set arguments [argparse -inline -mixed -help {Load a circuit netlist.} {
        {-string -help {Interpret the circuit as newline-separated text instead of a Tcl list}}
        {value -help {Circuit netlist}}
    }]
    return [uplevel 1 [list ::ngspicetclbridge::NativeCall $handle circuit $arguments]]
}

proc ::ngspicetclbridge::SIM::plot {handle args} {
    # Gets current plot name if no arguments provided, names of all plots or all vectors names that belongs to
    # name of the provided plot.
    #  -all - switch to return all plots names
    #  -vecs plotname - switch with argument to get all vectors names belonging to `name` plot
    #
    # Synopsis: ?-all?
    # Synopsis: -vecs name
    # Returns: current plot, or list of all plots, or list of all vectors
    #
    # The handle argument is supplied automatically when called as $handle plot.
    set arguments [argparse -inline -help {Return the current plot, all plots, or vectors belonging to a plot.} {
        {-all -help {Return all plot names}}
        {-vecs= -help {Return vector names for the specified plot}}
    }]
    if {[dict exists $arguments all] && [dict exists $arguments vecs]} {
        return -code error {-all and -vecs are mutually exclusive}
    }
    return [uplevel 1 [list ::ngspicetclbridge::NativeCall $handle plot $arguments]]
}

proc ::ngspicetclbridge::SIM::inputpath {handle args} {
    # Gets current path for XSPICE code models if `-current` switch is provided, or set current path to value
    # provided.
    #  -current - switch to return current path
    #  path - full path to desired location
    #
    # Synopsis: -current
    # Synopsis: path
    # Returns: current path
    #
    # The handle argument is supplied automatically when called as $handle inputpath.
    set arguments [argparse -inline -mixed -help {Query or set the ngspice input path.} {
        {-current -help {Return the current input path}}
        {path? -help {Set the input path; required unless -current is supplied}}
    }]
    if {[dict exists $arguments current] == [dict exists $arguments path]} {
        return -code error {specify either -current or a path}
    }
    return [uplevel 1 [list ::ngspicetclbridge::NativeCall $handle inputpath $arguments]]
}

proc ::ngspicetclbridge::SIM::waitevent {handle args} {
    # Blocks until a named event is observed n number of times (or once), the instance is aborted/destroyed,
    # or the timeout expires (if provided).
    #  name -  name of the event
    #  -n N - required number of callbacks relative to the baseline; defaults to one
    #  -since count - optional baseline captured with eventcounts before starting the operation
    #  timeout_ms -  timeout in miliseconds, optional
    #
    # | Event name        | Ngspice callback function name | When it is called                                                                                                                 |
    # |:------------------|:-------------------------------|:----------------------------------------------------------------------------------------------------------------------------------|
    # | `send_char`       | `SendChar`                     | Whenever Ngspice produces a line of text on stdout or stderr.                                                                     |
    # | `send_stat`       | `SendStat`                     | When Ngspice’s simulation status changes (e.g., --ready--, tran 50.1%, convergence messages).                                     |
    # | `controlled_exit` | `ControlledExit`               | When Ngspice exits, either due to an error or after a quit command from Tcl/Ngspice.                                              |
    # | `send_data`       | `SendData`                     | During an analysis, whenever Ngspice sends a row of vector values (time step or sweep point) to the callback.                     |
    # | `send_init_data`  | `SendInitData`                 | At the start of a run, when Ngspice sends metadata for all vectors in the current plot (names, types, indexes, real/complex).     |
    # | `bg_running`      | `BGThreadRunning`              | When the Ngspice background thread changes state: running=false means it just started running, running=true means it has stopped. |
    #
    #
    # | Time | Tcl Script Action         | Ngspice Core Activity       | Bridge Callback Fired    | Tcl Event Name Seen |
    # |------|---------------------------|-----------------------------|--------------------------|---------------------|
    # | t0   | `set s [..::new ..]`      | Library loaded              | *(none)*                 | *(none)*            |
    # | t1   | `$s init`                 | Initialization completed    | *(none)*                 | *(none)*            |
    # | t2   | `cirPass …`               | Parses circuit line         | `send_char("...")`       | `send_char`         |
    # | t3   | *(more circbyline calls)* | Parses circuit line         | `send_char("...")`       | `send_char`         |
    # | t4   | `$s command bg_run`       | Start background simulation | `bg_running(false)`      | `bg_running`        |
    # | t5   | *(analysis setup)*        | Build vector table          | `send_init_data()`       | `send_init_data`    |
    # | t6   | *(analysis running)*      | First data point            | `send_data(...)`         | `send_data`         |
    # | t7   | *(analysis running)*      | More points                 | `send_data(...)`         | `send_data`         |
    # | t8   | *(analysis running)*      | Status change               | `send_stat("...")`       | `send_stat`         |
    # | t9   | *(analysis completed)*    | Simulation ready            | `send_stat("--ready--")` | `send_stat`         |
    # | t10  | *(BG thread exits)*       | Background thread exits     | `bg_running(true)`       | `bg_running`        |
    # | t11  | `$s command quit`         | ngspice quits               | `controlled_exit(...)`   | `controlled_exit`   |
    # | t12  | `$s destroy`              | Teardown                    | *(no further calls)*     | *(command removed)* |
    #
    # Result dictionary of the command:
    #   - `fired` \- `0|1`, whether the target count was reached
    #   - `count` \- cumulative total count for that event so far
    #   - `status` \- `ok | timeout | aborted`
    #
    # Examples:
    #```tcl
    # $sim waitevent send_stat 1000
    # # -> {fired 1 count 1 status ok}
    #
    # $sim waitevent send_stat 1000
    # # -> {fired 0 count 1 status timeout}  ;# nothing new in the next 1s
    #```
    #
    # By default, only callbacks received after entering the wait count. To include callbacks received
    # between starting an operation and waiting, capture a baseline first:
    #```tcl
    # set baseline [dict get [$sim eventcounts] bg_running]
    # $sim command bg_run
    # $sim waitevent bg_running -since $baseline -n 2 10000
    #```
    # With -since, the target is baseline + N; an already reached target returns immediately.
    # Use a count from the same handle and event. Do not clear counters between capturing it and waiting.
    # A timeout returns status timeout, not a Tcl error. This command does not process queued data updates.
    #
    # Synopsis: name ?-n N? ?-since count? ?timeout_ms?
    # Returns: dictionary with information about event
    #
    # The handle argument is supplied automatically when called as $handle waitevent.
    set arguments [argparse -inline -mixed -help {Wait for an ngspice event count. This does not drain the Tcl event\
                                                          queue.} {
        {-since= -type wideinteger -validate {$arg >= 0} -help {Count captured before starting the operation; includes\
                                                                        callbacks already received}}
        {-n= -type wideinteger -validate {$arg > 0} -help {Required event count; defaults to one}}
        {event -enum {send_char send_stat controlled_exit send_data send_init_data bg_running send_raw_evt event_init event_batch}\
                 -help {Event to wait for}}
        {timeout? -type wideinteger -help {Timeout in milliseconds; zero or negative waits indefinitely}}
    }]
    return [uplevel 1 [list ::ngspicetclbridge::NativeCall $handle waitevent $arguments]]
}

proc ::ngspicetclbridge::SIM::asyncvector {handle args} {
    # Fetches the current values of a named vector through ngspice's ngGet_Vec_Info. Available once the simulation
    # has produced data; the vector may still be incomplete.
    #  -output - list or vector; overrides the handle default for this snapshot
    #  -ifexists - error or replace; overrides the handle collision policy
    #  -name - destination name or #auto for a fresh RBC name (vector output only)
    #  -namespace - destination namespace; created with missing parents (vector output only)
    #  -info - returns a metadata dictionary: signal type, real/complex type and length
    #  name - ngspice vector name
    #
    # Ownership and replacement:
    # - Vector output creates an independent, caller-owned snapshot that survives handle destruction.
    # - With -ifexists replace, existing same-type snapshot vectors are updated in place. Unrelated commands and
    #   live vectors maintained by any simulator cannot be overwritten.
    #
    # Destination namespace:
    # - Uses the explicit handle -namespace, or otherwise the caller's namespace.
    # - A per-call -namespace overrides this selection and resolves relative to the caller. It does not change
    #   handle defaults or live bindings. Empty names are rejected.
    # - A fully qualified -name overrides the selected namespace; its parent must already exist.
    # - #auto creates a fresh RBC vector in the selected namespace.
    #
    # Signal-derived names and access:
    # - Balanced parentheses, including v(1), remain literal. Names unsafe for RBC or containing namespace
    #   separators are encoded as _raw_ followed by uppercase hexadecimal UTF-8 bytes.
    # - No mapped array variable is created. Read values through the vector command, for example: $vector range 0
    #    end.
    #
    # Example:
    #```tcl
    # $sim initvectors
    # $sim asyncvector out
    # # -> {0.0 0.066666... 0.133333... ...}
    #
    # $sim asyncvector V(9)
    # # -> {{0.01 0.00} {0.02 0.00} ...}   ;# if complex
    #```
    #
    # Synopsis: name ?-output list|vector? ?-ifexists error|replace? ?-name vectorName? ?-namespace name?
    # Synopsis: -info name
    # Returns: real vectors as a flat list of doubles, complex vectors as a list of `{re im}` pairs. Error if vector
    # does not exists.
    #
    # The handle argument is supplied automatically when called as $handle asyncvector.
    set arguments [argparse -inline -mixed -help {Fetch an independent snapshot or metadata for a named vector.} {
        {-output= -enum {list vector} -help {Output representation; overrides the configured default}}
        {-ifexists= -enum {error replace} -help {Destination collision policy; overrides the configured default}}
        {-namespace= -help {Destination namespace; relative to the caller, created if missing}}
        {-name= -help {Snapshot destination name or #auto; requires vector output}}
        {-info -help {Return metadata instead of values; incompatible with output options}}
        {source -help {Ngspice vector name}}
    }]
    if {[dict exists $arguments info]} {
        foreach key {output ifexists namespace name} {
            if {[dict exists $arguments $key]} {
                return -code error {-info cannot be combined with output options}
            }
        }
    }
    return [uplevel 1 [list ::ngspicetclbridge::NativeCall $handle asyncvector $arguments]]
}

proc ::ngspicetclbridge::SIM::isrunning {handle args} {
    # Calls asynchronously to check if background thread is running.
    # Returns: `1` if thread is running, `0` otherwise.
    #
    # The handle argument is supplied automatically when called as $handle isrunning.
    # Synopsis: ?-help?
    set arguments [argparse -inline -help {Return whether the simulation is running.} {}]
    return [uplevel 1 [list ::ngspicetclbridge::NativeCall $handle isrunning $arguments]]
}

proc ::ngspicetclbridge::SIM::abort {handle args} {
    # Sets an internal abort flag and wake any waiters (useful to force waitevent to return). This does **not** free
    # the instance.
    # Returns: nothing.
    #
    # The handle argument is supplied automatically when called as $handle abort.
    # Synopsis: ?-help?
    set arguments [argparse -inline -help {Abort an active event wait.} {}]
    return [uplevel 1 [list ::ngspicetclbridge::NativeCall $handle abort $arguments]]
}

proc ::ngspicetclbridge::SIM::destroy {handle args} {
    # Deletes the instance command. In details:
    # - Marks the context as destroying
    # - Wakes any waiters
    # - Quiesces the ngspice background thread
    # - Defers and performs full cleanup
    # Returns: nothing.
    #
    # The handle argument is supplied automatically when called as $handle destroy.
    # Synopsis: ?-help?
    set arguments [argparse -inline -help {Destroy the simulator handle and its owned live vectors.} {}]
    return [uplevel 1 [list ::ngspicetclbridge::NativeCall $handle destroy $arguments]]
}

proc ::ngspicetclbridge::SIM::flush {handle args} {
    # Publishes pending initialization metadata and samples for this simulator into live storage.
    # Works in both list and vector modes. Returns an empty result on success.
    #
    # The command processes one buffered batch without waiting for simulation completion or servicing the Tcl
    # event loop. Samples arriving during publication remain pending for a later flush or event-loop iteration.
    # Initialization is applied before samples. Repeated calls without new data have no effect, and queued
    # bridge events cannot append the same samples twice. Existing notifications for this handle are removed
    # before publication so repeated flushes do not retain an unbounded event queue. Callback counters are unchanged.
    #
    # Publication errors are returned directly and retained for subsequent flush/vectors calls until a new plot.
    # RBC notifications may invoke application callbacks. Reentrant operations on this handle are rejected
    # during publication, except destruction. Graph redraws still require normal Tk event processing.
    #
    # Example:
    # ```tcl
    # $sim command run
    # $sim flush
    # set data [$sim vectors]
    # ```
    #
    # The handle argument is supplied automatically when called as $handle flush.
    # Synopsis: ?-help?
    # Returns: empty string
    set arguments [argparse -inline -help {Publish buffered live data without processing the Tcl event loop.} {}]
    return [uplevel 1 [list ::ngspicetclbridge::NativeCall $handle flush $arguments]]
}

proc ::ngspicetclbridge::SIM::vectors {handle args} {
    # Gets data accumulated from send_data events through the Tcl event loop.
    #  -clear - empties stored samples and returns **nothing**.
    #
    # Output:
    # - List mode: dictionary mapping signal names to lists. Complex samples use interleaved real/imaginary doubles.
    # - Vector mode: dictionary mapping raw signal names to fully qualified live RBC vector commands. Real and
    # complex signals use the corresponding vector types.
    #
    # Updates and clearing:
    # - Each new plot replaces stored list data, for both run and bg_run. Lists contain the current plot's
    #   accumulated samples, not concatenated history. Previously returned dictionaries remain valid.
    # - Halting preserves samples. Resuming the same plot appends to them, including samples still queued
    #   for Tcl processing. A different plot resets storage even when reached through resume.
    # - Explicit -clear empties stored samples and discards already queued samples in both output modes.
    #   Samples received afterwards continue appending; ngspice snapshot data is unaffected.
    # - Vector identities remain stable across runs. Each new plot clears retained vectors, including signals absent
    #   from that plot.
    # - Updates require Tcl event processing or an explicit flush; eventcounts/waitevent alone do not publish
    #   queued samples. The run helper flushes automatically before returning.
    # - In vector mode, -clear preserves vector commands and their graph bindings.
    #
    # Ownership and renaming:
    # - The handle destroys vectors it created; adopted vectors remain alive.
    # - Treat live contents as read-only. Deleting, resizing or replacing attached vectors invalidates continued
    #   streaming.
    # - Use rbc::vector rename oldName newName to rename a live vector. Its identity, ownership and streaming
    #   connection are preserved across clear and subsequent runs.
    # - After renaming completes, query the bridge for the updated dictionary: signal keys remain unchanged, while
    #   destination names reflect the rename.
    # - Ordinary Tcl rename is unsupported because it leaves RBC's registry unchanged.
    #
    # Errors:
    # - Asynchronous output failures are reported once through Tcl's background error handler and on subsequent
    #   vectors calls. Incoming samples are discarded until a new plot initializes output.
    #
    #
    # Example:
    #```tcl
    # $sim vectors
    # # -> v(out) {0.0 0.1 0.2 ...} v(in) {...} v-sweep {...}
    #
    # $sim vectors -clear
    # # -> (no result; succeeds)
    #```
    #
    # Synopsis: ?-clear?
    # Returns: dict with vectors values accumulated up to this point in internal structure
    #
    # The handle argument is supplied automatically when called as $handle vectors.
    set arguments [argparse -inline -help {Return accumulated live samples or live RBC vector bindings.} {
        {-clear -help {Clear the accumulated data}}
    }]
    return [uplevel 1 [list ::ngspicetclbridge::NativeCall $handle vectors $arguments]]
}

proc ::ngspicetclbridge::SIM::initvectors {handle args} {
    # Gets held **initial vector metadata** (built from send_init_data) in a dict.
    #  -clear - empties the internal memory structure and returns **nothing**.
    #
    # Example:
    #```tcl
    # $sim initvectors
    # # -> out {number 1 real 1} in {number 2 real 1} v-sweep {number 3 real 1}
    #```
    #
    # **Warning**: accumulation of data **do not** continues after call `bg_run`, new metadata replace the old one
    # in the storage.
    #
    # Synopsis: ?-clear?
    # Returns: dict with initial vector metadata saved in internal structure
    #
    # The handle argument is supplied automatically when called as $handle initvectors.
    set arguments [argparse -inline -help {Return vector initialization metadata.} {
        {-clear -help {Clear the accumulated data}}
    }]
    return [uplevel 1 [list ::ngspicetclbridge::NativeCall $handle initvectors $arguments]]
}

proc ::ngspicetclbridge::SIM::messages {handle args} {
    # Queues of textual messages captured from Ngspice (stdout/stderr) and bridge status lines.
    #  -clear - empties the internal queue structure and returns **nothing**.
    # Returns: list of messages
    #
    # Example:
    #```tcl
    # join [$sim messages] \n
    # # stdout ******
    # # stdout ** ngspice-44.x shared library
    # # ...
    # # # status[0]: --ready--
    #```
    #
    # **Warning**: accumulation of messages continues even if you run new circuit or analysis until you explicitly
    # clear the data storage.
    #
    # Synopsis: ?-clear?
    #
    # The handle argument is supplied automatically when called as $handle messages.
    set arguments [argparse -inline -help {Return accumulated ngspice messages.} {
        {-clear -help {Clear the accumulated data}}
    }]
    return [uplevel 1 [list ::ngspicetclbridge::NativeCall $handle messages $arguments]]
}

proc ::ngspicetclbridge::SIM::eventcounts {handle args} {
    # Raw event counters: send_raw_evt counts native callbacks (including retained disabled registrations);
    # event_init counts event-store initialization on configuration or a new plot; event_batch counts completed
    # raw batches, including batches without changed selected nodes. The latter two are bridge-generated events.
    # They support waitevent -since and eventcounts -clear just like the existing callback counters.
    # Gets or reset the cumulative event counters for this simulator instance.
    #  -clear - zeros all counts and returns nothing.
    #
    # Example:
    #```tcl
    # $sim eventcounts
    # # -> send_char N  send_stat N  controlled_exit N send_data N  send_init_data N  bg_running N
    #```
    #
    # Synopsis: ?-clear?
    # Returns: returns a dict with keys as events names, and counts for each event as a value
    #
    # The handle argument is supplied automatically when called as $handle eventcounts.
    set arguments [argparse -inline -help {Return accumulated event counters.} {
        {-clear -help {Clear the accumulated data}}
    }]
    return [uplevel 1 [list ::ngspicetclbridge::NativeCall $handle eventcounts $arguments]]
}

# Private dispatch adds no lifetime owner or namespace context of its own.
proc ::ngspicetclbridge::Dispatch {handle args} {
    set commands {eventnodes eventdata command circuit plot inputpath waitevent asyncvector isrunning abort destroy\
                          flush vectors initvectors messages eventcounts}
    if {![llength $args]} {
        return -code error "missing subcommand: must be [join $commands {, }]"
    }
    set operation [lindex $args 0]
    if {($operation in {-help -h}) && ([llength $args]==1)} {
        puts "Usage: $handle subcommand ?arguments?\nSubcommands: [join $commands {, }]\nUse $handle subcommand -help\
                    for details."
        return
    }
    if {$operation ni $commands} {
        return -code error "unknown subcommand \"$operation\": must be [join $commands {, }]"
    }
    return [uplevel 1 [list ::ngspicetclbridge::SIM::$operation $handle {*}[lrange $args 1 end]]]
}

namespace eval ::ngspicetclbridge::SIM {
    namespace export eventnodes eventdata command circuit plot inputpath waitevent asyncvector isrunning abort destroy\
            flush vectors initvectors messages eventcounts
    namespace ensemble create
}

namespace eval ::ngspicetclbridge {

    namespace export run readVecsAsync getCircuit getScaleInfo getPlotName getCircuitTitle getPlotDate

    proc run {args} {
        # Runs simulation in a background thread, waits for completion and flushes live data before returning.
        # Does not service the Tcl event loop; GUI events and redraws remain pending.
        #  sim -  simulator handle that is returned by `ngspicetclbridge::new`
        #  -nocleanup - if provided, previous ngspice plots are retained; live storage still resets for a new plot
        # Returns:
        # Synopsis: ?-nocleanup? sim
        set arguments [argparse -inline -mixed -help {Run a simulation and wait for completion.} {
            {-nocleanup -boolean -help {Retain previous ngspice plots}}
            {sim -help {Simulator handle that is returned by ngspicetclbridge::new}}
        }]
        dict with arguments {}
        set sim [uplevel 1 [list namespace which -command $sim]]
        if {$sim eq {}} {return -code error {unknown simulator handle}}
        if {!$nocleanup} {
            $sim command {destroy all}
        }
        if {$::tcl_platform(platform) eq {windows}} {
            $sim command {set num_threads=1}
        }
        set baseline [dict get [$sim eventcounts] bg_running]
        set rc [$sim command bg_run]
        if {![string is integer -strict $rc] || ($rc != 0)} {
            return -code error "could not start background simulation: $rc"
        }
        set waited [$sim waitevent bg_running -since $baseline -n 2]
        if {[dict get $waited status] ne {ok}} {
            return -code error "background simulation wait [dict get $waited status]"
        }
        $sim command bg_halt
        $sim flush
        return
    }
    proc readVecsAsync {args} {
        # Reads all available vectors of the current plot as independent snapshots. Returns a dictionary with
        # raw vector names as keys and lists or fully qualified RBC vector names as values.
        #  -info - return vector metadata instead of data; cannot be combined with output options
        #  -output - list or vector; overrides the simulator default
        #  -namespace - destination namespace for vector snapshots; created if missing, relative to the caller
        #  -ifexists - error or replace; overrides the simulator collision policy
        #  -names - `#auto` for fresh vectors, or a dictionary of signal names to explicit destinations or `#auto`
        #  sim - simulator handle that is returned by `ngspicetclbridge::new`
        #
        # Output and ownership:
        # - Handle defaults apply unless overridden. Without an explicit namespace, snapshots use the caller's
        #   namespace.
        # - Complex data produces complex RBC vectors. Snapshots survive simulator destruction.
        #
        # Naming:
        # - Naming options require vector output. RBC generates returned names in the selected namespace.
        # - Signals omitted from -names keep signal-derived names.
        # - #auto requests automatic naming; auto without # is an ordinary name.
        #
        # Consistency and errors:
        # - Vectors are copied separately; reading a running simulation is not an atomic snapshot.
        # - If a later copy fails, previously created or replaced snapshots remain in the destination namespace.
        #
        # Synopsis: ?-info? ?-output list|vector? ?-namespace name? ?-ifexists error|replace? ?-names dictionary|#auto?
        # sim
        # Returns: dictionary
        set arguments [argparse -inline -mixed -help {Read all vectors of the current plot as independent snapshots.} {
            {-info -help {Return vector metadata instead of data}}
            {-output= -enum {list vector} -help {Output mode}}
            {-namespace= -help {Destination namespace for vector snapshots}}
            {-names= -help {#auto for fresh vectors, or a dictionary of signal names to explicit destinations or\
                                    #auto}}
            {-ifexists= -enum {error replace} -help {Collision policy, error or replace}}
            {sim -help {simulator handle that is returned by ngspicetclbridge::new}}
        }]
        dict with arguments {}
        set options {}
        foreach option {output namespace ifexists} {
            if {[info exists $option]} {
                lappend options -$option [set $option]
            }
        }
        if {[info exists info] && ([llength $options] || [info exists names])} {
            return -code error {-info cannot be combined with -output, -namespace, -ifexists or -names}
        }
        # Execute in the caller's frame so relative simulator commands and namespace defaults resolve there.
        set plot [uplevel 1 [list $sim plot]]
        set vecNames [uplevel 1 [list $sim plot -vecs $plot]]
        if {[info exists names]} {
            if {[info exists output] && ($output ne {vector})} {
                return -code error {-names requires -output vector}
            }
            if {$names ne {#auto}} {
                dict for {key destination} $names {
                    if {$key ni $vecNames} {
                        return -code error "unknown vector in -names: $key"
                    }
                    if {$destination eq {}} {
                        return -code error {empty destination vector name}
                    }
                }
            }
        }
        set result [dict create]
        foreach vecName $vecNames {
            if {[info exists info]} {
                dict set result $vecName [uplevel 1 [list $sim asyncvector -info $vecName]]
            } else {
                set vectorOptions $options
                if {[info exists names]} {
                    if {$names eq {#auto}} {
                        lappend vectorOptions -name #auto
                    } elseif {[dict exists $names $vecName]} {
                        lappend vectorOptions -name [dict get $names $vecName]
                    }
                }
                dict set result $vecName [uplevel 1 [list $sim asyncvector $vecName {*}$vectorOptions]]
            }
        }
        return $result
    }
    proc getCircuit {args} {
        # Gets list with currently loaded circuit (its listing) in the form specified by the switch.
        #  -logical - the circuit is with all continuation lines collapsed into one line.
        #  -physical - the circuits lines are printed out as they were found in the file.
        #  -deck - just like the physical listing, except without the line numbers it recreates the input file verbatim
        #   (except that it does not preserve case)
        #  -expand - the circuit is printed with all subcircuits expanded.
        #  -runnable - circuit netlist expanded, but without additional line numbers, ready to be sourced again
        #   and run in ngspice. Default version if no witch is provided.
        #  -param - printing all parameters and their actual values.
        #  sim - simulator handler that is returned by `ngspicetclbridge::new`
        #
        # Synopsis: ?-logical|-physical|-deck|-expand|-runnable|-param? sim
        # Returns: list with listing lines
        set arguments [argparse -inline -mixed -help {Return the circuit listing in the selected format.} {
            {-logical -key type -value logical -help {The circuit is with all continuation lines collapsed into one\
                                                              line}}
            {-physical -key type -value physical -help {The circuits lines are printed out as they were found in the\
                                                                file}}
            {-deck -key type -value deck -help {Just like the physical listing, except without the line numbers it\
                                                        recreates the input file verbatim}}
            {-expand -key type -value expand -help {The circuit is printed with all subcircuits expanded}}
            {-runnable -key type -value runnable -default runnable -help {The circuit netlist expanded, but without\
                                                                                  additional line numbers, ready to be\
                                                                                  sourced again and run in\
                                                                                  ngspice. Default version if no witch\
                                                                                  is provided.}}
            {-param -key type -value param -help {Printing all parameters and their actual values}}
            {sim -help {Simulator handle that is returned by ngspicetclbridge::new}}
        }]
        dict with arguments {}
        set circuit [dict get [uplevel 1 [list $sim command -capture "listing $type"]] output]
        foreach line $circuit {
            lappend result [string map {{stdout } {}} $line]
        }
        return $result
    }
    proc getScaleInfo {args} {
        # Gets dictionary with information about current scale vector.
        #
        # Returns: dictionary with the information
        # Synopsis: sim
        set arguments [argparse -inline -help {Query simulator metadata.} {
            {sim -help {Simulator handle that is returned by ngspicetclbridge::new}}
        }]
        set sim [dict get $arguments sim]
        set info [dict get [uplevel 1 [list $sim command -capture setscale]] output]
        regexp {^\s*\S+\s+(\S+)\s*:\s*(\w+),\s*(\w+),\s*(\d+)\s+(\w+)} $info -> name type ntype length unit
        return [dict create name $name type $type ntype $ntype length $length]
    }
    proc getPlotName {args} {
        # Gets name of the current plot, i.e. Operating Point, AC Analysis, etc.
        #
        # Returns: name of the plot
        # Synopsis: sim
        set arguments [argparse -inline -help {Query simulator metadata.} {
            {sim -help {Simulator handle that is returned by ngspicetclbridge::new}}
        }]
        set sim [dict get $arguments sim]
        return {*}[string map {{stdout } {}} [dict get [uplevel 1 [list $sim command -capture {echo $curplotname}]]\
                                                      output]]
    }
    proc getCircuitTitle {args} {
        # Gets title of the current circuit (first line of circuits netlist).
        #
        # Returns: title
        # Synopsis: sim
        set arguments [argparse -inline -help {Query simulator metadata.} {
            {sim -help {Simulator handle that is returned by ngspicetclbridge::new}}
        }]
        set sim [dict get $arguments sim]
        return {*}[string map {{stdout } {}} [dict get [uplevel 1 [list $sim command -capture {echo $curplottitle}]]\
                                                      output]]
    }
    proc getPlotDate {args} {
        # Gets time stamp of current plot generation.
        #
        # Returns: time stamp
        # Synopsis: sim
        set arguments [argparse -inline -help {Query simulator metadata.} {
            {sim -help {Simulator handle that is returned by ngspicetclbridge::new}}
        }]
        set sim [dict get $arguments sim]
        return {*}[string map {{stdout } {}} [dict get [uplevel 1 [list $sim command -capture {echo $curplotdate}]]\
                                                      output]]
    }
}
