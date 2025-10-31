package require argparse

namespace eval ::ngspicetclbridge {

    namespace export run readVecsAsync getCircuit

    proc run {sim} {
        # Runs simulation in background thread, waits for the completion, process event in the queue and returns
        #  sim -  simulator handler that is returned by `ngspicetclbridge::new`
        # Returns:
        $sim command bg_run
        $sim waitevent bg_running -n 2
        update
        $sim command bg_halt
        return
    }
    proc readVecsAsync {args} {
        # Reads all availible vectors of the current plot asynchronously and return dictionary with vector name as a key,
        # and data as a value. If `-info` switch is provided, command returns information about each availiable vector.
        #  -info - if provided, metainfo about vectors is returned instead of the data.
        #  sim - simulator handler that is returned by `ngspicetclbridge::new`
        #
        # Returns: dictionary
        # Synopsis: ?-info? sim
        argparse {
            -info
            sim
        }
        set vecNames [$sim plot -vecs [$sim plot]]
        set result [dict create]
        foreach vecName $vecNames {
            if {[info exists info]} {
                dict append result $vecName [$sim asyncvector -info $vecName]
            } else {
                dict append result $vecName [$sim asyncvector $vecName]
            }
        }
        return $result
    }

    proc getCircuit {args} {
        # Returns list with currently loaded circuit (its listing) in the form specified by the switch.
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

}
