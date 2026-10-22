package require ngspicetclbridge
namespace import ::ngspicetclbridge::*
package require ticklecharts

# docs-begin find-shared-library
global env
if {$::tcl_platform(platform) eq {unix}} {
    set ngspiceLibPath [file join $env(NGSPICE_DLL) libngspice.so]
} elseif {$::tcl_platform(platform) eq {windows}} {
    set ngspiceLibPath [file join $env(NGSPICE_DLL) ngspice.dll]
}
# docs-end find-shared-library

# docs-begin netlist
set circuit {Diode IV
d1 anode 0 diomod area=1 lm=1e-6
va anode 0 0
.model diomod d(is=1e-12 n=1.2 rs=0.01 cj0=1e-9 trs=0.001 xti=5 ikf=100)
.dc va 0 2 0.01
.temp 25
.end}
# docs-end netlist

# load ngspice library and initialize command for library control
# docs-begin create-handle
set sim [ngspicetclbridge::new $ngspiceLibPath]
# docs-end create-handle
# load circuit into simulation
# docs-begin load-circuit
$sim circuit -string $circuit
# docs-end load-circuit
# add n parameter sweep
# docs-begin set-n-values
set ns {0.8 0.9 1.0 1.1 1.2}
# docs-end set-n-values

# docs-begin simulation-loop
foreach n $ns {
    # change model parameter
    $sim command "altermod diomod n = $n"
    # run simulation and wait for the end
    run $sim
    # read result vectors and prepare for plotting
    foreach x [$sim asyncvector anode] y [$sim asyncvector i(va)] {
        set xf [format %.3f $x]
        set yf [format %.3f [expr {-$y}]]
        lappend xydata [list $xf $yf]
    }
    lappend dataList $xydata
    unset xydata
}
# docs-end simulation-loop

# plot results with ticklecharts
# docs-begin plot-ticklecharts
set chart [ticklecharts::chart new]
$chart Xaxis -name {v(anode), V} -minorTick {show True}  -type value -splitLine {show True}
$chart Yaxis -name {Idiode, A} -minorTick {show True}  -type value -splitLine {show True}
$chart SetOptions -title {} -tooltip {trigger axis} -animation False -legend {}\
        -toolbox {feature {dataZoom {yAxisIndex none}}}  -grid {left 5% right 15%}
foreach data $dataList n $ns {
    $chart Add lineSeries -data $data -showAllSymbol nothing -name n=$n -symbolSize 2
}
set fbasename [file rootname [file tail [info script]]]

$chart Render -outfile [file normalize [file join html_charts $fbasename.html]] -width 800px -height 500px\
        -divid $fbasename -jschartvar chart_$fbasename -jsvar option_$fbasename
# docs-end plot-ticklecharts
