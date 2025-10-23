package require argparse

namespace eval ::ngspicetclbridge {

    proc runAndWaitForCompletion {sim} {
        # Runs simulation in background thread, waits for the completion, process event in the queue and returns
        #  sim -  simulator handler that is returned by `ngspicetclbridge::new`
        # Returns:
        $sim command bg_run
        set i 0
        while {$i<2} {
            $sim waitevent bg_running
            incr i
        }
        update
        return
    }
    proc readPlotVectorsAsync {args} {
        # Reads all availible vectorsof current plot asynchronously and return dictionary with vector name as a key,
        # and data as a value. If `-info` switch is provided, command returns information about each availiable vector.
        #  -info - if provided, metainfo about vectors is returned instead of the data.
        #  sim - simulator handler that is returned by `ngspicetclbridge::new`
        # Returns: dictionary
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


}
