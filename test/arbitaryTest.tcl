package require tcltest
namespace import ::tcltest::*
package require ngspicetclbridge
namespace import ::ngspicetclbridge::*
set ngspiceLibPath /usr/local/lib/libngspice.so
set currentDir [file normalize [file dirname [info script]]]
proc cirPass {sim circuit} {
    foreach line [split $circuit "\n"] {
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
set resDivCircuitAlt {
    Resistor divider
    v1 in1 0 1
    r1 in1 out1 1e3
    r2 out1 0 2e3
    .dc v1 0 5 0.1
    .save all
    .end
}
set fourBitAdderCircuit {
    four-bit adder
    .tran 1e-9 10e-6
    .options noacct=-sw
    .subckt nand 1 2 3 4
    q1 9 5 1 qmod
    d1clamp 0 1 dmod
    q2 9 5 2 qmod
    d2clamp 0 2 dmod
    rb 4 5 4e3
    r1 4 6 1.6e3
    q3 6 9 8 qmod
    r2 8 0 1e3
    rc 4 7 130
    q4 7 6 10 qmod
    dvbedrop 10 3 dmod
    q5 3 8 0 qmod
    .ends nand
    .subckt onebit 1 2 3 4 5 6
    xx1 1 2 7 6 nand
    xx2 1 7 8 6 nand
    xx3 2 7 9 6 nand
    xx4 8 9 10 6 nand
    xx5 3 10 11 6 nand
    xx6 3 11 12 6 nand
    xx7 10 11 13 6 nand
    xx8 12 13 4 6 nand
    xx9 11 7 5 6 nand
    .ends onebit
    .subckt twobit 1 2 3 4 5 6 7 8 9
    xx1 1 2 7 5 10 9 onebit
    xx2 3 4 10 6 8 9 onebit
    .ends twobit
    .subckt fourbit 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15
    xx1 1 2 3 4 9 10 13 16 15 twobit
    xx2 5 6 7 8 11 12 16 14 15 twobit
    .ends fourbit
    xx18 1 2 3 4 5 6 7 8 9 10 11 12 0 13 99 fourbit
    vcc 99 0 5
    vin1a 1 0 pulse 0 3 0 10e-9 10e-9 1e-8 5e-8
    vin1b 2 0 pulse 0 3 0 10e-9 10e-9 2e-8 1e-7
    vin2a 3 0 pulse 0 3 0 10e-9 10e-9 4e-8 2e-7
    vin2b 4 0 pulse 0 3 0 10e-9 10e-9 8e-8 4e-7
    vin3a 5 0 pulse 0 3 0 10e-9 10e-9 1.6e-7 8e-7
    vin3b 6 0 pulse 0 3 0 10e-9 10e-9 3.2e-7 1.6e-6
    vin4a 7 0 pulse 0 3 0 10e-9 10e-9 6.4e-7 3.2e-6
    vin4b 8 0 pulse 0 3 0 10e-9 10e-9 1.28e-6 6.4e-6
    rbit0 9 0 1e3
    rbit1 10 0 1e3
    rbit2 11 0 1e3
    rbit3 12 0 1e3
    rcout 13 0 1e3
    .model dmod d
    .model qmod npn(level=1 bf=75 rb=100 cje=1e-12 cjc=3e-12)
    .save v(9) v(10) v(11) v(12)
    .end
}









test test-9 {waitevent status - timeout condition} -setup {
    set s1 [ngspicetclbridge::new $ngspiceLibPath]
    $s1 init
    cirPass $s1 $resDivCircuit
} -body {
    return [$s1 waitevent send_stat 1000]
} -match glob -result {fired 0 count * status timeout} -cleanup {
    #$s1 command quit
    $s1 destroy
    unset s1
}

# test test-9 {waitevent status - aborted} -setup {
#     set s1 [ngspicetclbridge::new $ngspiceLibPath]
#     $s1 init
#     cirPass $s1 $resDivCircuit
# } -body {
#     after 1000 { $s1 abort; $s1 destroy }   ;# destroy after 1 sec
#     puts [$s1 waitevent send_stat 5000]
# } -result {fired 0 count 0 status timeout} -cleanup {
#     unset s1
# }


test test-10 {run long simulation and wait for two event, the second one marks the end of bg thread simulation} -setup {
    set s1 [ngspicetclbridge::new $ngspiceLibPath]
    $s1 init
    cirPass $s1 $fourBitAdderCircuit
} -body {
    $s1 command bg_run
    set i 0
    while {$i<2} {
        $s1 waitevent bg_running
        incr i
    }
    update
    return [$s1 initvectors]
} -result {V(12) {number 0 real 1} V(11) {number 1 real 1} V(10) {number 2 real 1} V(9) {number 3 real 1}\
 time {number 4 real 1}} -cleanup {
    $s1 destroy
    unset s1
}

test test-10.1 {run long simulation and wait for two event, the second one marks the end of bg thread simulation} -setup {
    set s1 [ngspicetclbridge::new $ngspiceLibPath]
    $s1 init
    cirPass $s1 $fourBitAdderCircuit
} -body {
    $s1 command bg_run
    $s1 waitevent bg_running -n 2
    update
    return [$s1 initvectors]
} -result {V(12) {number 0 real 1} V(11) {number 1 real 1} V(10) {number 2 real 1} V(9) {number 3 real 1}\
 time {number 4 real 1}} -cleanup {
    $s1 destroy
    unset s1
}

test test-10.2 {run long simulation and wait for three events (never happens, exit with timeout)} -setup {
    set s1 [ngspicetclbridge::new $ngspiceLibPath]
    $s1 init
    cirPass $s1 $fourBitAdderCircuit
} -body {
    $s1 command bg_run
    $s1 waitevent bg_running -n 3 20000
    update
    return [$s1 initvectors]
} -result {V(12) {number 0 real 1} V(11) {number 1 real 1} V(10) {number 2 real 1} V(9) {number 3 real 1}\
 time {number 4 real 1}} -cleanup {
    $s1 destroy
    unset s1
}

test test-11 {run long simulation, wait for ready message, and print status message} -setup {
    set s1 [ngspicetclbridge::new $ngspiceLibPath]
    $s1 init
    cirPass $s1 $fourBitAdderCircuit
} -body {
    $s1 command bg_run
    set message {}
    while {$message ne {# status[0]: --ready--}} {
        $s1 waitevent send_stat
        update
        set message [lindex [$s1 messages] end]
        puts $message
    }
    return [$s1 initvectors]
} -result {V(12) {number 0 real 1} V(11) {number 1 real 1} V(10) {number 2 real 1} V(9) {number 3 real 1}\
 time {number 4 real 1}} -cleanup {
    $s1 destroy
    unset s1
}

test test-12 {test asyncvector command after finising simulation} -setup {
    set s1 [ngspicetclbridge::new $ngspiceLibPath]
    $s1 init
    cirPass $s1 $resDivCircuit
} -body {
    runAndWaitForCompletion $s1
    return [$s1 asyncvector out]
} -result {0.0 0.06666666666666667 0.13333333333333333 0.2 0.26666666666666666 0.3333333333333333 0.39999999999999997\
 0.4666666666666666 0.5333333333333332 0.6 0.6666666666666665 0.7333333333333332 0.7999999999999999 0.8666666666666667\
 0.9333333333333335 1.0 1.0666666666666669 1.1333333333333335 1.2000000000000004 1.266666666666667 1.3333333333333335\
 1.4000000000000004 1.466666666666667 1.5333333333333339 1.6000000000000003 1.6666666666666672 1.733333333333334\
 1.8000000000000005 1.8666666666666674 1.933333333333334 2.000000000000001 2.0666666666666678 2.133333333333334\
 2.200000000000001 2.2666666666666675 2.3333333333333344 2.4000000000000012 2.4666666666666677 2.5333333333333345\
 2.6000000000000014 2.666666666666668 2.733333333333334 2.8000000000000007 2.866666666666667 2.933333333333333 3.0\
 3.0666666666666664 3.133333333333333 3.199999999999999 3.2666666666666657 3.333333333333332} -cleanup {
    $s1 destroy
    unset s1
}

test test-13 {test asyncvector command after finising simulation with wrong name of the vector} -setup {
    set s1 [ngspicetclbridge::new $ngspiceLibPath]
    $s1 init
    cirPass $s1 $resDivCircuit
} -body {
    runAndWaitForCompletion $s1
    catch {$s1 asyncvector out1} errorStr
    return $errorStr
} -result {vector with name "out1" does not exist} -cleanup {
    $s1 destroy
    unset s1 errorStr
}


test test-14 {test of isrunning command} -setup {
    set s1 [ngspicetclbridge::new $ngspiceLibPath]
    $s1 init
    cirPass $s1 $fourBitAdderCircuit
} -body {
    $s1 command bg_run
    set i 0
    while {$i<2} {
        $s1 waitevent bg_running
        if {$i==0} {
            set result [$s1 isrunning]
        }
        incr i
    }
    update
    return $result
} -result 1 -cleanup {
    $s1 destroy
    unset s1 result
}

test test-15 {run two times without processing events with update after first time - check that internal storage and Tcl\
                     vectors values are resetted} -setup {
    set s1 [ngspicetclbridge::new $ngspiceLibPath]
    $s1 init
    cirPass $s1 $resDivCircuit
} -body {
    $s1 command bg_run
    $s1 waitevent send_stat 1000
    $s1 command bg_run
    $s1 waitevent send_stat 1000
    update
    return [$s1 vectors]
} -result {v1#branch {0.0 -3.3333333333333335e-5 -6.666666666666667e-5 -0.00010000000000000002 -0.00013333333333333334\
 -0.0001666666666666667 -0.00019999999999999998 -0.00023333333333333339 -0.0002666666666666667 -0.00030000000000000003\
 -0.00033333333333333327 -0.0003666666666666666 -0.00039999999999999996 -0.0004333333333333334 -0.00046666666666666677\
 -0.0005000000000000002 -0.0005333333333333334 -0.0005666666666666667 -0.0006000000000000003 -0.0006333333333333336\
 -0.000666666666666667 -0.0007000000000000003 -0.0007333333333333334 -0.000766666666666667 -0.0008000000000000004\
 -0.0008333333333333337 -0.0008666666666666671 -0.0009000000000000004 -0.000933333333333334 -0.0009666666666666671\
 -0.0010000000000000005 -0.001033333333333334 -0.0010666666666666672 -0.0011000000000000007 -0.0011333333333333338\
 -0.0011666666666666674 -0.001200000000000001 -0.0012333333333333341 -0.0012666666666666677 -0.0013000000000000008\
 -0.001333333333333334 -0.0013666666666666675 -0.0014000000000000006 -0.0014333333333333338 -0.001466666666666667\
 -0.0015000000000000005 -0.0015333333333333336 -0.0015666666666666663 -0.0015999999999999999 -0.001633333333333333\
 -0.0016666666666666661} out {0.0 0.06666666666666667 0.13333333333333333 0.2 0.26666666666666666 0.3333333333333333\
 0.39999999999999997 0.4666666666666666 0.5333333333333332 0.6 0.6666666666666665 0.7333333333333332 0.7999999999999999\
 0.8666666666666667 0.9333333333333335 1.0 1.0666666666666669 1.1333333333333335 1.2000000000000004 1.266666666666667\
 1.3333333333333335 1.4000000000000004 1.466666666666667 1.5333333333333339 1.6000000000000003 1.6666666666666672\
 1.733333333333334 1.8000000000000005 1.8666666666666674 1.933333333333334 2.000000000000001 2.0666666666666678\
 2.133333333333334 2.200000000000001 2.2666666666666675 2.3333333333333344 2.4000000000000012 2.4666666666666677\
 2.5333333333333345 2.6000000000000014 2.666666666666668 2.733333333333334 2.8000000000000007 2.866666666666667\
 2.933333333333333 3.0 3.0666666666666664 3.133333333333333 3.199999999999999 3.2666666666666657 3.333333333333332}\
 in {0.0 0.1 0.2 0.30000000000000004 0.4 0.5 0.6 0.7 0.7999999999999999 0.8999999999999999 0.9999999999999999\
 1.0999999999999999 1.2 1.3 1.4000000000000001 1.5000000000000002 1.6000000000000003 1.7000000000000004\
 1.8000000000000005 1.9000000000000006 2.0000000000000004 2.1000000000000005 2.2000000000000006 2.3000000000000007\
 2.400000000000001 2.500000000000001 2.600000000000001 2.700000000000001 2.800000000000001 2.9000000000000012\
 3.0000000000000013 3.1000000000000014 3.2000000000000015 3.3000000000000016 3.4000000000000017 3.5000000000000018\
 3.600000000000002 3.700000000000002 3.800000000000002 3.900000000000002 4.000000000000002 4.100000000000001\
 4.200000000000001 4.300000000000001 4.4 4.5 4.6 4.699999999999999 4.799999999999999 4.899999999999999\
 4.999999999999998} v-sweep {0.0 0.1 0.2 0.30000000000000004 0.4 0.5 0.6 0.7 0.7999999999999999 0.8999999999999999\
 0.9999999999999999 1.0999999999999999 1.2 1.3 1.4000000000000001 1.5000000000000002 1.6000000000000003\
 1.7000000000000004 1.8000000000000005 1.9000000000000006 2.0000000000000004 2.1000000000000005 2.2000000000000006\
 2.3000000000000007 2.400000000000001 2.500000000000001 2.600000000000001 2.700000000000001 2.800000000000001\
 2.9000000000000012 3.0000000000000013 3.1000000000000014 3.2000000000000015 3.3000000000000016 3.4000000000000017\
 3.5000000000000018 3.600000000000002 3.700000000000002 3.800000000000002 3.900000000000002 4.000000000000002\
 4.100000000000001 4.200000000000001 4.300000000000001 4.4 4.5 4.6 4.699999999999999 4.799999999999999\
 4.899999999999999 4.999999999999998}} -cleanup {
    $s1 destroy
    unset s1
}

test test-16 {test circuit subcommand} -setup {
    set s1 [ngspicetclbridge::new $ngspiceLibPath]
    $s1 init
    $s1 circuit [split $resDivCircuit \n]
} -body {
    runAndWaitForCompletion $s1
    return [$s1 vectors]
} -result {v1#branch {0.0 -3.3333333333333335e-5 -6.666666666666667e-5 -0.00010000000000000002 -0.00013333333333333334\
 -0.0001666666666666667 -0.00019999999999999998 -0.00023333333333333339 -0.0002666666666666667 -0.00030000000000000003\
 -0.00033333333333333327 -0.0003666666666666666 -0.00039999999999999996 -0.0004333333333333334 -0.00046666666666666677\
 -0.0005000000000000002 -0.0005333333333333334 -0.0005666666666666667 -0.0006000000000000003 -0.0006333333333333336\
 -0.000666666666666667 -0.0007000000000000003 -0.0007333333333333334 -0.000766666666666667 -0.0008000000000000004\
 -0.0008333333333333337 -0.0008666666666666671 -0.0009000000000000004 -0.000933333333333334 -0.0009666666666666671\
 -0.0010000000000000005 -0.001033333333333334 -0.0010666666666666672 -0.0011000000000000007 -0.0011333333333333338\
 -0.0011666666666666674 -0.001200000000000001 -0.0012333333333333341 -0.0012666666666666677 -0.0013000000000000008\
 -0.001333333333333334 -0.0013666666666666675 -0.0014000000000000006 -0.0014333333333333338 -0.001466666666666667\
 -0.0015000000000000005 -0.0015333333333333336 -0.0015666666666666663 -0.0015999999999999999 -0.001633333333333333\
 -0.0016666666666666661} out {0.0 0.06666666666666667 0.13333333333333333 0.2 0.26666666666666666 0.3333333333333333\
 0.39999999999999997 0.4666666666666666 0.5333333333333332 0.6 0.6666666666666665 0.7333333333333332 0.7999999999999999\
 0.8666666666666667 0.9333333333333335 1.0 1.0666666666666669 1.1333333333333335 1.2000000000000004 1.266666666666667\
 1.3333333333333335 1.4000000000000004 1.466666666666667 1.5333333333333339 1.6000000000000003 1.6666666666666672\
 1.733333333333334 1.8000000000000005 1.8666666666666674 1.933333333333334 2.000000000000001 2.0666666666666678\
 2.133333333333334 2.200000000000001 2.2666666666666675 2.3333333333333344 2.4000000000000012 2.4666666666666677\
 2.5333333333333345 2.6000000000000014 2.666666666666668 2.733333333333334 2.8000000000000007 2.866666666666667\
 2.933333333333333 3.0 3.0666666666666664 3.133333333333333 3.199999999999999 3.2666666666666657 3.333333333333332}\
 in {0.0 0.1 0.2 0.30000000000000004 0.4 0.5 0.6 0.7 0.7999999999999999 0.8999999999999999 0.9999999999999999\
 1.0999999999999999 1.2 1.3 1.4000000000000001 1.5000000000000002 1.6000000000000003 1.7000000000000004\
 1.8000000000000005 1.9000000000000006 2.0000000000000004 2.1000000000000005 2.2000000000000006 2.3000000000000007\
 2.400000000000001 2.500000000000001 2.600000000000001 2.700000000000001 2.800000000000001 2.9000000000000012\
 3.0000000000000013 3.1000000000000014 3.2000000000000015 3.3000000000000016 3.4000000000000017 3.5000000000000018\
 3.600000000000002 3.700000000000002 3.800000000000002 3.900000000000002 4.000000000000002 4.100000000000001\
 4.200000000000001 4.300000000000001 4.4 4.5 4.6 4.699999999999999 4.799999999999999 4.899999999999999\
 4.999999999999998} v-sweep {0.0 0.1 0.2 0.30000000000000004 0.4 0.5 0.6 0.7 0.7999999999999999 0.8999999999999999\
 0.9999999999999999 1.0999999999999999 1.2 1.3 1.4000000000000001 1.5000000000000002 1.6000000000000003\
 1.7000000000000004 1.8000000000000005 1.9000000000000006 2.0000000000000004 2.1000000000000005 2.2000000000000006\
 2.3000000000000007 2.400000000000001 2.500000000000001 2.600000000000001 2.700000000000001 2.800000000000001\
 2.9000000000000012 3.0000000000000013 3.1000000000000014 3.2000000000000015 3.3000000000000016 3.4000000000000017\
 3.5000000000000018 3.600000000000002 3.700000000000002 3.800000000000002 3.900000000000002 4.000000000000002\
 4.100000000000001 4.200000000000001 4.300000000000001 4.4 4.5 4.6 4.699999999999999 4.799999999999999\
 4.899999999999999 4.999999999999998}} -cleanup {
    $s1 destroy
    unset s1
}

test test-17 {halt simulation, then resume it} -setup {
    set s1 [ngspicetclbridge::new $ngspiceLibPath]
    $s1 init
    cirPass $s1 $fourBitAdderCircuit
} -body {
    $s1 command bg_run
    after 100
    $s1 command bg_halt
    $s1 command bg_resume
    set i 0
    while {$i<2} {
        $s1 waitevent bg_running
        incr i
    }
    update
    return [$s1 initvectors]
} -result {V(12) {number 0 real 1} V(11) {number 1 real 1} V(10) {number 2 real 1} V(9) {number 3 real 1}\
 time {number 4 real 1}} -cleanup {
    $s1 destroy
    unset s1
}

test test-18 {get current plot} -setup {
    set s1 [ngspicetclbridge::new $ngspiceLibPath]
    $s1 init
    cirPass $s1 $resDivCircuit
} -body {
    runAndWaitForCompletion $s1
    return [$s1 plot]
} -result dc9 -cleanup {
    $s1 destroy
    unset s1
}

test test-19 {get all vectors belong to current plot} -setup {
    set s1 [ngspicetclbridge::new $ngspiceLibPath]
    $s1 init
    cirPass $s1 $resDivCircuit
} -body {
    runAndWaitForCompletion $s1
    return [$s1 plot -vecs [$s1 plot]]
} -result {v1#branch out in v-sweep} -cleanup {
    $s1 destroy
    unset s1
}

test test-20 {get all vector with plot name prefix} -setup {
    set s1 [ngspicetclbridge::new $ngspiceLibPath]
    $s1 init
    cirPass $s1 $resDivCircuit
} -body {
    runAndWaitForCompletion $s1
    return [$s1 asyncvector [$s1 plot].out]
} -result {0.0 0.06666666666666667 0.13333333333333333 0.2 0.26666666666666666 0.3333333333333333 0.39999999999999997\
0.4666666666666666 0.5333333333333332 0.6 0.6666666666666665 0.7333333333333332 0.7999999999999999 0.8666666666666667\
0.9333333333333335 1.0 1.0666666666666669 1.1333333333333335 1.2000000000000004 1.266666666666667 1.3333333333333335\
1.4000000000000004 1.466666666666667 1.5333333333333339 1.6000000000000003 1.6666666666666672 1.733333333333334\
1.8000000000000005 1.8666666666666674 1.933333333333334 2.000000000000001 2.0666666666666678 2.133333333333334\
2.200000000000001 2.2666666666666675 2.3333333333333344 2.4000000000000012 2.4666666666666677 2.5333333333333345\
2.6000000000000014 2.666666666666668 2.733333333333334 2.8000000000000007 2.866666666666667 2.933333333333333 3.0\
3.0666666666666664 3.133333333333333 3.199999999999999 3.2666666666666657 3.333333333333332} -cleanup {
    $s1 destroy
    unset s1
}

test test-21 {wrong arguments to plot command} -setup {
    set s1 [ngspicetclbridge::new $ngspiceLibPath]
    $s1 init
    cirPass $s1 $resDivCircuit
} -body {
    runAndWaitForCompletion $s1
    catch {$s1 plot 1} errorStr
    return $errorStr
} -result {unknown option: 1 (expected -all)} -cleanup {
    $s1 destroy
    unset s1
}

test test-22 {wrong arguments to plot command} -setup {
    set s1 [ngspicetclbridge::new $ngspiceLibPath]
    $s1 init
    cirPass $s1 $resDivCircuit
} -body {
    runAndWaitForCompletion $s1
    catch {$s1 plot -vec out} errorStr
    return $errorStr
} -result {unknown option: -vec (expected -vecs)} -cleanup {
    $s1 destroy
    unset s1
}

test test-23 {wrong arguments to plot command} -setup {
    set s1 [ngspicetclbridge::new $ngspiceLibPath]
    $s1 init
    cirPass $s1 $resDivCircuit
} -body {
    runAndWaitForCompletion $s1
    catch {$s1 plot -vecs out out} errorStr
    return $errorStr
} -result {wrong # args: should be "::ngspicetclbridge::s25 plot ?-all?|?-vecs plotname?"} -cleanup {
    $s1 destroy
    unset s1
}

test test-24 {} -setup {
    set s1 [ngspicetclbridge::new $ngspiceLibPath]
    $s1 init
    cirPass $s1 $resDivCircuit
} -body {
    ngspicetclbridge::runAndWaitForCompletion $s1
    $s1 messages -clear
    $s1 command {listing runnable}
    update
    return [$s1 messages]
} -result {{stdout Background thread stopped with timeout = 0} {stdout * expanded deck of resistor divider} {stdout *\
resistor divider} {stdout v1 in 0 1} {stdout r1 in out 1e3} {stdout r2 out 0 2e3} {stdout .dc v1 0 5 0.1} {stdout\
.end}} -cleanup {
    $s1 destroy
    unset s1
}

test test-25 {} -setup {
    set s1 [ngspicetclbridge::new $ngspiceLibPath]
    $s1 init
    cirPass $s1 $resDivCircuit
} -body {
    ngspicetclbridge::runAndWaitForCompletion $s1
    return [$s1 initvectors]
} -result {v1#branch {number 0 real 1} out {number 1 real 1} in {number 2 real 1} v-sweep {number 3 real 1}} -cleanup {
    $s1 destroy
    unset s1
}

test test-26 {} -setup {
    set s1 [ngspicetclbridge::new $ngspiceLibPath]
    $s1 init
    cirPass $s1 $resDivCircuit
} -body {
    ngspicetclbridge::runAndWaitForCompletion $s1
    return [dict get [ngspicetclbridge::readPlotVectorsAsync $s1] out]
} -result {0.0 0.06666666666666667 0.13333333333333333 0.2 0.26666666666666666 0.3333333333333333 0.39999999999999997\
0.4666666666666666 0.5333333333333332 0.6 0.6666666666666665 0.7333333333333332 0.7999999999999999 0.8666666666666667\
0.9333333333333335 1.0 1.0666666666666669 1.1333333333333335 1.2000000000000004 1.266666666666667 1.3333333333333335\
1.4000000000000004 1.466666666666667 1.5333333333333339 1.6000000000000003 1.6666666666666672 1.733333333333334\
1.8000000000000005 1.8666666666666674 1.933333333333334 2.000000000000001 2.0666666666666678 2.133333333333334\
2.200000000000001 2.2666666666666675 2.3333333333333344 2.4000000000000012 2.4666666666666677 2.5333333333333345\
2.6000000000000014 2.666666666666668 2.733333333333334 2.8000000000000007 2.866666666666667 2.933333333333333 3.0\
3.0666666666666664 3.133333333333333 3.199999999999999 3.2666666666666657 3.333333333333332} -cleanup {
    $s1 destroy
    unset s1
}

test test-27 {} -setup {
    set s1 [ngspicetclbridge::new $ngspiceLibPath]
    $s1 init
    cirPass $s1 $resDivCircuit
} -body {
    ngspicetclbridge::runAndWaitForCompletion $s1
    return [$s1 asyncvector -info out]
} -result {type voltage length 51 ntype real} -cleanup {
    $s1 destroy
    unset s1
}

test test-28 {} -setup {
    set s1 [ngspicetclbridge::new $ngspiceLibPath]
    $s1 init
    cirPass $s1 $resDivCircuit
} -body {
    ngspicetclbridge::runAndWaitForCompletion $s1
    return [ngspicetclbridge::readPlotVectorsAsync -info $s1]
} -result {v1#branch {type current length 51 ntype real} out {type voltage length 51 ntype real} in {type voltage\
length 51 ntype real} v-sweep {type voltage length 51 ntype real}} -cleanup {
    $s1 destroy
    unset s1
}

test test-29 {run long simulation and exit with error due to wrong subcommand before running thread stops} -setup {
    set s1 [ngspicetclbridge::new $ngspiceLibPath]
    $s1 init
    cirPass $s1 $fourBitAdderCircuit
} -body {
    $s1 command bg_run
    catch {$s1 waitevent bg_running 50000 -n 3 } errorStr
    update
    return $errorStr
} -result {wrong # args: should be "::ngspicetclbridge::s31 waitevent name ?-n N? ?timeout_ms?"} -cleanup {
    $s1 destroy
    unset s1
}

test test-30 {} -setup {
    set s1 [ngspicetclbridge::new $ngspiceLibPath]
    $s1 init
    cirPass $s1 $fourBitAdderCircuit
} -body {
    $s1 command -capture bg_run
    return [$s1 command -capture kolp]
} -result {background thread is starting, command kolp is deffered} -cleanup {
    $s1 destroy
    unset s1
}

test test-31 {} -setup {
    set s1 [ngspicetclbridge::new $ngspiceLibPath]
    $s1 init
    cirPass $s1 $fourBitAdderCircuit
} -body {
    $s1 command bg_run
    $s1 command bg_halt
    update
    return [$s1 command -capture kolp]
} -result {background thread is starting, command kolp is deffered} -cleanup {
    $s1 destroy
    unset s1
}


test test-32 {} -setup {
    set s1 [ngspicetclbridge::new $ngspiceLibPath]
    $s1 init
    cirPass $s1 $fourBitAdderCircuit
} -body {
    $s1 command -capture bg_run
    after 1000
    return [$s1 command -capture kolp]
} -result {background thread is starting, command kolp is deffered} -cleanup {
    #$s1 destroy
    unset s1
}

