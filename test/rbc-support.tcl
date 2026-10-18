if {[llength $argv] != 3 || [lindex $argv 0] ne "--rbc-fixture"} {
    error {usage: tclsh rbc.test|rbc-graph.test --rbc-fixture bridgeLibrary mockLibrary}
}

load [file normalize [lindex $argv 1]] Ngspicetclbridge
set mockLibrary [file normalize [lindex $argv 2]]
set argv {}
# Probe the extension build as well as runtime RBC availability. Restore initial defaults.
set haveRbc [expr {![catch {::ngspicetclbridge::configure -output vector}]}]
::ngspicetclbridge::configure -output list

testConstraint rbc $haveRbc

namespace eval ::live {}
namespace eval ::snap {}

proc newSim {args} {
    ::ngspicetclbridge::new $::mockLibrary -namespace ::live {*}$args
}
proc begin {sim {count 3}} {
    $sim command mock_begin
    for {set i 0} {$i < $count} {incr i} {
        $sim command mock_point
    }
    update
}
proc clean {sim} {
    catch {$sim destroy}
    update
    namespace delete ::live ::snap
    namespace eval ::live {}
    namespace eval ::snap {}
}
proc waitUntil {script} {
    set deadline [expr {[clock milliseconds] + 5000}]
    while {![uplevel 1 [list expr $script]]} {
        if {[clock milliseconds] > $deadline} {
            error {simulation timed out}
        }
        after 2
        update
    }
    update
}
proc finishTests {} {
    set failed $::tcltest::numTests(Failed)
    cleanupTests
    exit [expr {$failed != 0}]
}
