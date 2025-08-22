set path_to_hl_tcl "/home/georgtree/tcl/hl_tcl"

#package require ruff
source /home/georgtree/tcl/ruff/src/ruff.tcl
package require fileutil
package require hl_tcl

set docDir [file dirname [file normalize [info script]]]
set sourceDir "${docDir}"
source [file join $docDir startPage.ruff]
source [file join $docDir notesAndInternals.ruff]
source [file join $docDir troubleshooting.ruff]
source [file join $sourceDir ngspicetclbridge.ruff]

set packageVersion [package versions ngspicetclbridge]
puts $packageVersion
set title "Tcl NgspiceTclBridge package"

set common [list -title $title -sortnamespaces false -preamble $startPage -pagesplit namespace -recurse false\
                    -includesource false -pagesplit namespace -autopunctuate true -compact false -includeprivate false\
                    -product NgspiceTclBridge -diagrammer "ditaa --border-width 1" -version $packageVersion\
                    -copyright "George Yashin" {*}$::argv]
set commonNroff [list -title $title -sortnamespaces false -preamble $startPage -pagesplit namespace -recurse false\
                         -pagesplit namespace -autopunctuate true -compact false -includeprivate false\
                         -product NgspiceTclBridge -diagrammer "ditaa --border-width 1" -version $packageVersion\
                         -copyright "George Yashin" {*}$::argv]

set namespaces [list ::ngspicetclbridge "::Notes and internals" ::Troubleshooting ]
set namespacesNroff [list ::ngspicetclbridge "::Notes and internals" ::Troubleshooting ]

if {[llength $argv] == 0 || "html" in $argv} {
    ruff::document $namespaces -outdir $docDir -format html -outfile index.html {*}$common
    ruff::document $namespacesNroff -outdir $docDir -format nroff -outfile NgspiceTclBridge.n {*}$commonNroff
}

foreach file [glob ${docDir}/*.html] {
    exec tclsh "${path_to_hl_tcl}/tcl_html.tcl" [file join ${docDir} $file]
}

# change default width
proc processContentsCss {fileContents} {
    return [string map [list max-width:60rem max-width:100rem "overflow-wrap:break-word" "overflow-wrap:normal"]\
                    $fileContents]
}
# change default theme 
proc processContentsJs {fileContents} {
    return [string map {init()\{currentTheme=localStorage.ruff_theme init()\{currentTheme=currentTheme="v1"}\
                    $fileContents]
}

fileutil::updateInPlace [file join $docDir assets ruff-min.css] processContentsCss
fileutil::updateInPlace [file join $docDir assets ruff-min.js] processContentsJs

set tableWrapping {
    .ruff-bd table.ruff_deflist th:first-child,
    .ruff-bd table.ruff_deflist td:first-child {
        white-space: nowrap;      /* never wrap */
        overflow-wrap: normal;
        word-break: normal;
    }
}
::fileutil::appendToFile [file join $docDir assets ruff-min.css] $tableWrapping
