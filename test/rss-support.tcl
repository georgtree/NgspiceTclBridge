# RSS is diagnostic only: allocator caching makes a portable pass/fail threshold unreliable.
proc residentKiB {} {
    if {$::tcl_platform(platform) eq "windows"} {
        set powershell [file join $::env(SystemRoot) System32 WindowsPowerShell v1.0 powershell.exe]
        set script [format {[long]((Get-Process -Id %d).WorkingSet64 / 1024)} [pid]]
        if {[catch {exec $powershell -NoProfile -NonInteractive -Command $script} rss]} {
            return unavailable
        }
        return [string trim $rss]
    }
    if {![file readable /proc/self/status]} {
        return unavailable
    }
    set fd [open /proc/self/status]
    try {
        set status [read $fd]
    } finally {
        close $fd
    }
    if {[regexp {VmRSS:\s+(\d+)} $status -> rss]} {
        return $rss
    }
    return unavailable
}
