# all.tcl --
#
# This file contains a top-level script to run all of the Tcl
# tests.  Execute it by invoking "source all.test" when running tcltest
# in this directory.
#
# Copyright (c) 1998-2000 by Scriptics Corporation.
# All rights reserved.

package require tcltest
namespace import ::tcltest::*
set dir [file normalize [file dirname [info script]]]

package require extexpr
configure {*}$argv -testdir $dir
runAllTests

