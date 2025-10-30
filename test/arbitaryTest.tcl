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

set resDivCircuit [split {
    Resistor divider
    v1 in 0 1
    r1 in out 1e3
    r2 out 0 2e3
    .dc v1 0 5 0.1
    .save all
    .end
} \n]
set resDivCircuitAlt [split {
    Resistor divider
    v1 in1 0 1
    r1 in1 out1 1e3
    r2 out1 0 2e3
    .dc v1 0 5 0.1
    .save all
    .end
} \n]
set fourBitAdderCircuit [split {
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
} \n]



# test test-24.1 {test listing command with -capture} -setup {
#     set s1 [ngspicetclbridge::new $ngspiceLibPath]
#     $s1 init
#     $s1 circuit $resDivCircuit
# } -body {
#     ngspicetclbridge::run $s1
#     #$s1 command bg_halt
#     return [$s1 command -capture {listing runnable}]
# } -result {{stdout Background thread stopped with timeout = 0} {stdout * expanded deck of resistor divider} {stdout *\
# resistor divider} {stdout v1 in 0 1} {stdout r1 in out 1e3} {stdout r2 out 0 2e3} {stdout .dc v1 0 5 0.1} {stdout\
# .end}} -cleanup {
#     $s1 destroy
#     unset s1
# }

test test-44 {check inputpath command, returning current path} -setup {
    set s1 [ngspicetclbridge::new $ngspiceLibPath]
    $s1 init
} -body {
    catch {$s1 inputpath} errorStr
    return $errorStr
 } -match glob -result {/home/georgtree/Downloads/} -cleanup {
    $s1 destroy
    unset s1 errorStr
}
