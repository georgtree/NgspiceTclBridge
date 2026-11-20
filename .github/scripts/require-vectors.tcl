package require ngspicetclbridge
package require rbc::vector
set lib [expr {$::tcl_platform(platform) eq "windows" ? "ngspice.dll" : "libngspice.so"}]
set sim [::ngspicetclbridge::new -noinit [file join $::env(NGSPICE_DLL) $lib]]
try {
    $sim circuit [list {CI voltage divider} {V1 in 0 2} {R1 in out 1k} {R2 out 0 1k} {.op} {.end}]
    $sim command run
    set v [$sim asyncvector out -output vector]
    try {
        if {[$v length] != 1 || abs([$v index 0] - 1.0) > 1e-10} {
            error "Expected a 1 V operating point, got [$v index :]"
        }
    } finally {::rbc::vector destroy $v}
} finally {$sim destroy}
