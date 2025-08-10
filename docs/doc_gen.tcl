package require fileutil

set script_path [file dirname [file normalize [info script]]]
set script_path_loc $script_path
set script_path [file join $script_path ..]
# nroff generating
set files {README.md}

foreach file [lrange $files 0 end] {
    exec md2man-roff [file join $script_path $file] > [file join $script_path_loc ngspicetclbridge.n]
}

# html generating
foreach file $files {
    catch {exec {*}[list pandoc [file join $script_path $file] -s -o [file join $script_path_loc\
                                                                              [file rootname $file].html]]} errorStr
    puts $errorStr
    lappend html_files [file join $script_path_loc [file rootname $file].html]
}

proc processContents {fileContents} {
    return [string map {{max-width: 36em} max-width:72em .md .html} $fileContents]
}
foreach html_file $html_files {
    fileutil::updateInPlace $html_file processContents
}
