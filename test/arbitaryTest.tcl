package require ngspicetclbridge

set s1 [ngspicetclbridge::new /home/georgtree/NgspiceTclBridge/bin/libngspice1.so]
puts [info commands $s1]
$s1 init

# Define the circuit setup
set circuit {
    circbyline Resistor divider
    circbyline v1 in 0 1
    circbyline r1 in out 1e3
    circbyline r2 out 0 2e3
    circbyline .dc v1 0 5 0.1
    circbyline .save all
    circbyline .end
}

# Initialize the circuit
foreach line [split $circuit "\n"] {
    if {[string trim $line] ne ""} {
        puts $line
        $s1 command $line
    }
}

# Start simulation (running in the background)
$s1 command "bg_run"

#puts [ngspicetclbridge::wait-event send_data 5000]
after 1000
puts [$s1 get-messages 0]
update
# Now retrieve results after the simulation finishes
puts "== Init Vectors =="
puts [$s1 get-init-vectors]

puts "== Simulation Results =="
puts [$s1 get-vectors]

#puts [ngspicetclbridge::get-messages 1]

# peek
puts [$s1 event-counts]
# get-and-clear
#puts [ngspicetclbridge::event-counts 1]
