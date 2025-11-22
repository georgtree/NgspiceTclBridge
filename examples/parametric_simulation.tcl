package require ngspicetclbridge
namespace import ::ngspicetclbridge::*
package require ticklecharts

set ngspiceLibPath /usr/local/lib/libngspice.so

set circuit {Diode IV
d1 anode 0 diomod area=1 lm=1e-6
va anode 0 0
.model diomod d(is=1e-12 n=1.2 rs=0.01 cj0=1e-9 trs=0.001 xti=5 ikf=100)
.dc va 0 2 0.01
.temp 25
.end}

# load ngspice library and initialize command for library control
set sim [ngspicetclbridge::new $ngspiceLibPath]
# load circuit into simulation
$sim circuit -string $circuit
# add n parameter sweep
set ns {0.8 0.9 1.0 1.1 1.2}

foreach n $ns {
    # change model parameter
    $sim command "altermod diomod n = $n"
    # run simulation and wait for the end
    run $sim
    # read result vectors and prepare for plotting
    foreach x [$sim asyncvector anode] y [$sim asyncvector i(va)] {
        set xf [format "%.3f" $x]
        set yf [format "%.3f" [= {-$y}]]
        lappend xydata [list $xf $yf]
    }
    lappend dataList $xydata
    unset xydata
}

# plot results with ticklecharts
set chart [ticklecharts::chart new]
$chart Xaxis -name "v(anode), V" -minorTick {show "True"}  -type "value" -splitLine {show "True"}
$chart Yaxis -name "Idiode, A" -minorTick {show "True"}  -type "value" -splitLine {show "True"}
$chart SetOptions -title {} -tooltip {trigger "axis"} -animation "False" -legend {}\
        -toolbox {feature {dataZoom {yAxisIndex "none"}}}  -grid {left "5%" right "15%"}
foreach data $dataList n $ns {
    $chart Add "lineSeries" -data $data -showAllSymbol "nothing" -name "n=${n}" -symbolSize "2"
}
set fbasename [file rootname [file tail [info script]]]

$chart Render -outfile [file normalize [file join html_charts $fbasename.html]]
