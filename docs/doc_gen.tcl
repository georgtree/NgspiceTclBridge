package require ruff
#source /home/georgtree/tcl/ruff/src/ruff.tcl
package require fileutil

set docDir [file dirname [file normalize [info script]]]
set sourceDir [file join $docDir ..]
source [file join $docDir startPage.ruff]
source [file join $docDir notesAndInternals.ruff]
source [file join $docDir troubleshooting.ruff]
source [file join $docDir ngspicetclbridge.ruff]
source [file join $docDir examples.ruff]
source [file join $sourceDir ngspicetclbridge.tcl]

set packageVersion [package versions ngspicetclbridge]
puts $packageVersion
set title "Tcl NgspiceTclBridge package"

set commonSphinx [list -title $title -sortnamespaces false -preamble $startPage -pagesplit namespace -recurse false\
                    -includesource false -pagesplit namespace -autopunctuate true -compact false -includeprivate false\
                    -product ngspicetclbridge -diagrammer "ditaa --border-width 1" -version $packageVersion\
                    -copyright "George Yashin" {*}$::argv]
set commonNroff [list -title $title -sortnamespaces false -preamble $startPage -pagesplit namespace -recurse false\
                         -pagesplit namespace -autopunctuate true -compact false -includeprivate false\
                         -product ngspicetclbridge -diagrammer "ditaa --border-width 1" -version $packageVersion\
                         -copyright "George Yashin" {*}$::argv]

set namespaces [list ::Examples ::Troubleshooting ::ngspicetclbridge {::Notes and internals}]

ruff::document $namespaces -format sphinx -outfile ngspicetclbridge.rst -outdir [file join $docDir sphinx]\
        {*}$commonSphinx
ruff::document $namespaces -format nroff -outdir $docDir -outfile ngspicetclbridge.n {*}$commonNroff

::fileutil::appendToFile [file join $docDir sphinx conf.py] {html_theme = "classic"
extensions = [
    "sphinx.ext.githubpages",
]
from pygments.lexers.tcl import TclLexer
from pygments.token import Operator

class MyTclLexer(TclLexer):
    def get_tokens_unprocessed(self, text):
        for i, t, v in super().get_tokens_unprocessed(text):
            if v == "=":
                yield i, Operator, v   # or Name.Builtin
            else:
                yield i, t, v

def setup(app):
    from sphinx.highlighting import lexers
    lexers["tcl"] = MyTclLexer()
}

catch {exec sphinx-build -b html [file join $docDir sphinx] [file join $docDir]} errorStr
puts $errorStr

# ticklechart graphs substitutions
proc processContentsTutorial {fileContents} {
    global path chartsMap
    dict for {mark file} $chartsMap {
        set fileData [fileutil::cat [file join $path $file]]
        set fileContents [string map [list $mark $fileData] $fileContents]
    }
    return $fileContents
}

set chartsMap [dict create !ticklechart_parametric_simulation! parametric_simulation.html]
set path [file join $docDir .. examples html_charts]
fileutil::updateInPlace [file join $docDir ngspicetclbridge-Examples.html] processContentsTutorial
