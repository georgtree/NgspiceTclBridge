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
    #  -output - list or vector; inherits the package configuration (initially list); controls live storage and snapshots
    #  -ifexists - error or replace for initial vector destinations; inherits the package configuration (initially error)
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

proc ::ngspicetclbridge::SIM::command {handle args} {
    # Sends an arbitrary Ngspice command line (e.g., `bg_run`, `circbyline` ..., `.save`, `.tran`, `.dc`). If
    # we provide `-capture` switch, it returns not only the result code, but also the next message from stdout
    # of Ngspice. Also, if command is send in the moment background thread is starting or stopping, it will be
    # deferred to the time is could be safely sent, and in that case `-capture` message and command results code
    # are not availaible as a return of `command`.
    #  -capture - Return captured output along with the result code.
    #  string - Command string.
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
    set arguments [argparse -inline -mixed -help {Send a command to ngspice.} {
        {-capture -help {Capture command output along with its result code}}
        {string -help {Ngspice command line}}
    }]
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
        {event -enum {send_char send_stat controlled_exit send_data send_init_data bg_running}\
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
    # - Updates require Tcl event processing; eventcounts/waitevent alone do not guarantee that queued vector
    #   updates have been processed.
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
    set commands {command circuit plot inputpath waitevent asyncvector isrunning abort destroy vectors initvectors\
                          messages eventcounts}
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
    namespace export command circuit plot inputpath waitevent asyncvector isrunning abort destroy vectors initvectors\
            messages eventcounts
    namespace ensemble create
}

namespace eval ::ngspicetclbridge {

    namespace export run readVecsAsync getCircuit getScaleInfo getPlotName getCircuitTitle getPlotDate

    proc run {args} {
        # Runs simulation in background thread, waits for the completion, process event in the queue and returns
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
        update
        $sim command bg_halt
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
