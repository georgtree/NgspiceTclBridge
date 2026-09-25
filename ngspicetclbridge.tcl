package require argparse

namespace eval ::ngspicetclbridge {

    namespace export run readVecsAsync getCircuit getScaleInfo getPlotName getCircuitTitle getPlotDate

    proc run {args} {
        # Runs simulation in background thread, waits for the completion, process event in the queue and returns
        #  sim -  simulator handler that is returned by `ngspicetclbridge::new`
        #  -nocleanup -if provided, results from previous run are retained
        # Returns:
        argparse -exact {
            {-nocleanup -boolean}
            sim
        }
        if {!$nocleanup} {
            $sim command {destroy all}
        }
        if {$::tcl_platform(platform) eq {windows}} {
            $sim command {set num_threads=1}
        }
        $sim command bg_run
        $sim waitevent bg_running -n 2
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
        #  sim - simulator handler that is returned by `ngspicetclbridge::new`
        # Without overrides, the handle defaults apply. Without an explicit namespace, snapshots use the caller's
        # namespace. Complex data creates complex RBC vectors. Snapshots survive simulator destruction.
        # Each vector is copied separately; this is not an atomic snapshot of a running simulation. If a later
        # vector fails, previously created or replaced snapshots remain available in the destination namespace.
        #
        # Returns: dictionary
        # Synopsis: ?-info? ?-output list|vector? ?-namespace name? ?-ifexists error|replace? sim
        argparse -exact {
            -info
            {-output= -enum {list vector}}
            -namespace=
            {-ifexists= -enum {error replace}}
            sim
        }
        set options {}
        foreach option {output namespace ifexists} {
            if {[info exists $option]} {
                lappend options -$option [set $option]
            }
        }
        if {[info exists info] && [llength $options]} {
            error {-info cannot be combined with -output, -namespace or -ifexists}
        }
        # Execute in the caller's frame so relative simulator commands and namespace defaults resolve there.
        set plot [uplevel 1 [list $sim plot]]
        set vecNames [uplevel 1 [list $sim plot -vecs $plot]]
        set result [dict create]
        foreach vecName $vecNames {
            if {[info exists info]} {
                dict set result $vecName [uplevel 1 [list $sim asyncvector -info $vecName]]
            } else {
                dict set result $vecName [uplevel 1 [list $sim asyncvector $vecName {*}$options]]
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
        # Returns: list with listing lines
        # Synopsis: ?-logical|-physical|-deck|-expand|-runnable|-param? sim
        argparse {
            {-logical -key type -value logical}
            {-physical -key type -value physical}
            {-deck -key type -value deck}
            {-expand -key type -value expand}
            {-runnable -key type -value runnable -default runnable}
            {-param -key type -value param}
            sim
        }
        set circuit [dict get [$sim command -capture "listing $type"] output]
        foreach line $circuit {
            lappend result [string map {{stdout } {}} $line]
        }
        return $result
    }
    proc getScaleInfo {sim} {
        # Gets dictionary with information about current scale vector.
        #
        # Returns: dictionary with the information
        set info [dict get [$sim command -capture setscale] output]
        regexp {^\s*\S+\s+(\S+)\s*:\s*(\w+),\s*(\w+),\s*(\d+)\s+(\w+)} $info -> name type ntype length unit
        return [dict create name $name type $type ntype $ntype length $length]
    }
    proc getPlotName {sim} {
        # Gets name of the current plot, i.e. Operating Point, AC Analysis, etc.
        #
        # Returns: name of the plot
        return {*}[string map {{stdout } {}} [dict get [$sim command -capture {echo $curplotname}] output]]
    }
    proc getCircuitTitle {sim} {
        # Gets title of the current circuit (first line of circuits netlist).
        #
        # Returns: title
        return {*}[string map {{stdout } {}} [dict get [$sim command -capture {echo $curplottitle}] output]]
    }
    proc getPlotDate {sim} {
        # Gets time stamp of current plot generation.
        #
        # Returns: time stamp
        return {*}[string map {{stdout } {}} [dict get [$sim command -capture {echo $curplotdate}] output]]
    }

}
