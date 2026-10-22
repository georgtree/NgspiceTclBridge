package require ruff
package require ngspicetclbridge
package require fileutil

set docDir [file dirname [file normalize [info script]]]
set projectRoot [file normalize [file join $docDir ..]]
source [file join $docDir doc_include.tcl]
source [file join $docDir startPage.ruff]
source [file join $docDir rbcBindings.ruff]
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

set namespaces [list ::rbcBindings ::Examples ::Troubleshooting ::ngspicetclbridge {::Notes and internals}]

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
extensions = [
    "sphinx.ext.githubpages",
]
suppress_warnings = [
    "image.not_readable",
]
html_theme = "classic"
html_theme_options = {
    "stickysidebar": True,
}
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

catch {exec sphinx-build -E -a -b html [file join $docDir sphinx] [file join $docDir]} errorStr
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

# nroff pages names processing
foreach file [glob -directory $docDir *.n] {
    set old $file
    set tmp [file join $docDir __temp_rename__.n]
    set new [file join $docDir [string tolower [file tail $file]]]
    file rename $old $tmp
    file rename $tmp $new
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
