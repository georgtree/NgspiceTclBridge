package require ruff
package require ngspicetclbridge
package require fileutil

set docDir [file dirname [file normalize [info script]]]
set projectRoot [file normalize [file join $docDir ..]]
source [file join $docDir doc_include.tcl]
source [file join $docDir startPage.ruff]
source [file join $docDir rbcBindings.ruff]
source [file join $docDir digitalEvents.ruff]
source [file join $docDir notesAndInternals.ruff]
source [file join $docDir troubleshooting.ruff]
source [file join $docDir examples.ruff]

set packageVersion [package provide ngspicetclbridge]
puts $packageVersion
set title "Tcl NgspiceTclBridge package"

set commonSphinx [list -title $title -sortnamespaces false -preamble $startPage -pagesplit namespace\
                          -recurse false -includesource false -pagesplit namespace -autopunctuate true -compact false\
                          -includeprivate false -excludeprocs {^(Native.*|Dispatch)$} -product ngspicetclbridge\
                          -diagrammer "ditaa --border-width 1" -version $packageVersion -copyright "George Yashin"\
                          {*}$::argv]
set commonNroff [list -title $title -sortnamespaces false -preamble $startPage -pagesplit namespace -recurse false\
                         -pagesplit namespace -autopunctuate true -compact false -includeprivate false -excludeprocs\
                         {^(Native.*|Dispatch)$} -product ngspicetclbridge -diagrammer "ditaa --border-width 1"\
                         -version $packageVersion -copyright "George Yashin" {*}$::argv]

set namespaces [list ::rbcBindings ::digitalEvents ::Examples ::Troubleshooting ::ngspicetclbridge\
                        {::Notes and internals}]

foreach ns $namespaces {
    set preambleVar "${ns}::_ruff_preamble"
    if {[info exists $preambleVar]} {
        set $preambleVar [::docgen::expandIncludes [set $preambleVar] $projectRoot]
    }
}

ruff::document $namespaces -format sphinx -outfile ngspicetclbridge.rst -outdir [file join $docDir sphinx]\
        {*}$commonSphinx
ruff::document $namespaces -format nroff -outdir $docDir -outfile ngspicetclbridge.n {*}$commonNroff


::fileutil::appendToFile [file join $docDir sphinx conf.py] {
extensions = list(dict.fromkeys(globals().get("extensions", []) + [
    "sphinx_rtd_theme",
    "sphinx.ext.githubpages",
]))
suppress_warnings = [
    "image.not_readable",
]
html_theme = "sphinx_rtd_theme"
html_theme_options = {
    "collapse_navigation": False,
    "sticky_navigation": True,
    "navigation_depth": 4,
    "includehidden": True,
    "titles_only": False,
}
# Custom CSS is staged by doc_gen.tcl; the source lives inside the HTML outdir.
html_static_path = []
html_css_files = list(globals().get("html_css_files", [])) + ["manual.css"]
from pygments.lexers.tcl import TclLexer
from pygments.token import Operator

class MyTclLexer(TclLexer):
    def get_tokens_unprocessed(self, text):
        for i, t, v in super().get_tokens_unprocessed(text):
            if v == "=":
                yield i, Operator, v   # or Name.Builtin
            elif v == "$":
                yield i, Operator, v   # or Name.Builtin
            elif v == "\\":
                yield i, Operator, v   # or Name.Builtin
            elif v == "%":
                yield i, Operator, v   # or Name.Builtin
            elif v == "'":
                yield i, Operator, v   # or Name.Builtin
            else:
                yield i, t, v

def setup(app):
    from sphinx.highlighting import lexers
    lexers["tcl"] = MyTclLexer()
}

# Use one Python environment for Sphinx and the RTD theme; propagate failures.
set python python
if {[info exists ::env(DOC_PYTHON)] && $::env(DOC_PYTHON) ne {}} {
    set python $::env(DOC_PYTHON)
}
file mkdir [file join $docDir _static]
file copy -force [file join $docDir theme manual.css] [file join $docDir _static manual.css]
exec -- $python -m sphinx -E -a -b html [file join $docDir sphinx] $docDir >@stdout 2>@stderr

# ticklechart graphs substitutions
# Embed a standalone ticklecharts document without nesting HTML documents or
# sharing chart IDs/globals with the manual. Assets travel with installed docs.
proc chartFrame {filename} {
    global docDir
    set html [::fileutil::cat $filename]
    set height 0
    foreach {match pixels} [regexp -all -inline {height:\s*([0-9]+)px} $html] {
        set height [expr {max($height, $pixels)}]
    }
    if {$height == 0} {
        set height 500
    }
    # Keep ECharts' coordinate system in sync with the available RTD width.
    set responsive {
<style>
html, body { margin: 0; padding: 0; }
.chart-container { max-width: 100%; }
</style>
<script>
window.addEventListener("load", function () {
    document.querySelectorAll(".chart-container").forEach(function (element) {
        function resize() {
            const chart = window.echarts && echarts.getInstanceByDom(element);
            if (chart) chart.resize();
        }
        resize();
        if (window.ResizeObserver) new ResizeObserver(resize).observe(element);
        else window.addEventListener("resize", resize);
    });
});
</script>
}
    set html [string map [list </head> "$responsive</head>"] $html]
    set destination [file join $docDir assets charts]
    file mkdir $destination
    set name [file tail $filename]
    ::fileutil::writeFile [file join $destination $name] $html
    set title [string map [list & &amp\; \" &quot\; < &lt\; > &gt\;] [file rootname $name]]
    return "<iframe class=\"doc-chart\" src=\"assets/charts/$name\" title=\"$title\" height=\"$height\" loading=\"lazy\"></iframe>"
}

proc processContentsTutorial {fileContents} {
    global path chartsMap
    dict for {mark file} $chartsMap {
        set fileData [chartFrame [file join $path $file]]
        set fileContents [string map [list "<p>$mark</p>" $fileData $mark $fileData] $fileContents]
    }
    return $fileContents
}

set chartsMap [dict create !ticklechart_parametric_simulation! parametric_simulation.html]
set path [file join $docDir .. examples html_charts]
fileutil::updateInPlace [file join $docDir ngspicetclbridge-Examples.html] processContentsTutorial

# nroff pages names processing
foreach file [glob -directory $docDir *.n] {
    set old $file
    set tmp [file join $docDir __temp_rename__.n]
    set new [file join $docDir [string tolower [file tail $file]]]
    if {$old eq $new} {continue}
    file rename -force $old $tmp
    file rename -force $tmp $new
}
set specialPages [list ngspicetclbridge-examples ngspicetclbridge-troubleshooting ngspicetclbridge-notes-and-internals]
foreach namespacePath $namespaces {
    set tails [list]
    while {$namespacePath ne {}} {
        set tail [string tolower [namespace tail $namespacePath]]
        regsub -all {\s+} [string trim $tail] {-} tail
        set namespacePath [namespace qualifiers $namespacePath]
        lappend tails $tail
    }
    lappend tails [string tolower ngspicetclbridge]
    set manFileName [join [lreverse $tails] -]
    if {$manFileName ni $specialPages} {
        lappend manFilesLinks "${manFileName}(n)"
    }
}

set linksString ".SH SEE ALSO
ngspicetclbridge(n) - package's main page
.br
ngspicetclbridge-examples(n) - examples of usage with detailed explanations
.br
ngspicetclbridge-troubleshooting(n) - solutions to common issues
.br
ngspicetclbridge-notes-and-internals(n) - information about implementation
.br
.sp 1
Public commands documentation:
.br
[join $manFilesLinks \n.br\n]"

proc addLinks2man {fileContents} {
    global linksString
    append fileContents "\n$linksString"
    return $fileContents
}

foreach file [glob -directory $docDir *.n] {
    fileutil::updateInPlace $file addLinks2man
}
