package require ngspicetclbridge
package require rbc::graphtoolbar
namespace import ::ngspicetclbridge::*
namespace import rbc::graphtoolbar
namespace import rbc::vector

global env
if {$::tcl_platform(platform) eq {unix}} {
    set ngspiceLibPath [file join $env(NGSPICE_DLL) libngspice.so]
} elseif {$::tcl_platform(platform) eq {windows}} {
    set ngspiceLibPath [file join $env(NGSPICE_DLL) ngspice.dll]
}

set colors {#5470c6 #91cc75 #fac858 #ee6666 #73c0de #3ba272 #fc8452 #9a60b4 #ea7ccc}
set font {TkFixedFont 10}

proc runSim {sim temps {value {}}} {
    variable parameters
    array for {param value} parameters {
        $sim command "altermod diomod $param = $value"
    }
    dict for {vecName temp} $temps {
        $sim command "option temp=$temp"
        run $sim
        $sim asyncvector i(va) -name $vecName -ifexists replace
        $vecName expr {-$vecName}
    }
    $sim messages -clear
    $sim command -clear
}


set circuit {Diode IV
d1 anode 0 diomod area=1 lm=1e-6
va anode 0 0
.model diomod d(is=1e-12 n=1.2 rs=0.01 cj0=1e-9 trs=0.001 xti=5 ikf=100)
.dc va 0 2 0.01
.temp 25
.end}

set parsDict [dict create is [dict create default 1e-12 min 1e-13 max 1e-11 step 1e-13]\
                      n [dict create default 1.0 min 0.5 max 1.5 step 0.01]\
                      rs [dict create default 0.001 min 1e-4 max 1e-2 step 1e-4]\
                      trs [dict create default 0.005 min 1e-3 max 1e-2 step 1e-4]\
                      xti [dict create default 5.0 min 1 max 10 step 0.1]]

set temps [dict create i_m50(va) -50 i_25(va) 25 i_175(va) 175]
foreach vecName [dict keys $temps] {
    vector create $vecName
} 

set sim [ngspicetclbridge::new -noinit -output vector -ifexists replace $ngspiceLibPath]
$sim circuit -string $circuit


set scalesFrame [ttk::frame .scalesFr]
set i 0
dict for {name data} $parsDict {
    set parameters($name) [dict get $data default]
    set label [label "$scalesFrame.label$name" -text $name -font $font]
    set scale [scale "$scalesFrame.scale$name" -from [dict get $data min] -to [dict get $data max]\
                       -resolution [dict get $data step] -width 10\
                       -variable parameters($name) -command [list runSim $sim $temps] -orient horizontal]
    grid $label -row $i -column 0
    grid $scale -row $i -column 1 -sticky we
    grid rowconfigure $scalesFrame $i -weight 0
    incr i
}
grid columnconfigure $scalesFrame 0 -weight 0
grid columnconfigure $scalesFrame 1 -weight 1
set graph [graphtoolbar .graph -width 800 -height 500 -interactive -control context]
$graph graph grid on
$graph graph axis configure x -title {v(anode), V} -titlefont $font
$graph graph axis configure y -title {i(va), A}

runSim $sim $temps
$sim asyncvector anode -name v(anode) 
set i -1
dict for {vecName temp} $temps {
    $graph graph element create $vecName -x v(anode) -y $vecName -pixels 2 -label "T=${temp}C"\
            -color [lindex $colors [incr i]]
}

pack $graph -fill both
pack $scalesFrame -fill both


