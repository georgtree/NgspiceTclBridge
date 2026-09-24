#!/bin/bash -norc
dnl	This file is an input file used by the GNU "autoconf" program to
dnl	generate the file "configure", which is run during Tcl installation
dnl	to configure the system for the local environment.


AC_INIT([ngspicetclbridge],[0.1])
TEA_INIT()
AC_CONFIG_AUX_DIR(tclconfig)
TEA_PATH_TCLCONFIG
TEA_LOAD_TCLCONFIG
TEA_PREFIX
TEA_SETUP_COMPILER
TEA_ADD_SOURCES([ngspicetclbridge.c ngspicetclbridgeRbc.c])
TEA_ADD_HEADERS([generic/ngspicetclbridge.h generic/sharedspice.h generic/portable_dl.h])
TEA_ADD_INCLUDES([])
TEA_ADD_LIBS([])
TEA_ADD_CFLAGS([])
TEA_ADD_STUB_SOURCES([])
TEA_ADD_TCL_SOURCES([ngspicetclbridge.tcl])

# RBC support is opt-in. Compile its portable stub client source, never link the RBC shared library.
AC_ARG_WITH([rbc],
    [AS_HELP_STRING([--with-rbc=DIR], [RBC public header directory, source tree, or installation prefix])],
    [rbc_dir="$withval"], [rbc_dir=no])
if test "$rbc_dir" != no; then
    if test "$rbc_dir" = yes; then
        AC_MSG_ERROR([--with-rbc requires a directory])
    fi
    if test "$TCL_MAJOR_VERSION" -lt 9; then
        AC_MSG_ERROR([rbc-tk9 vector support requires Tcl 9; use --without-rbc for Tcl 8.6])
    fi
    RBC_INCLUDE_DIR=
    for rbc_candidate in "$rbc_dir" "$rbc_dir/include" "$rbc_dir/generic"; do
        if test -f "$rbc_candidate/rbcVector.h" && test -f "$rbc_candidate/rbcDecls.h" &&
           test -f "$rbc_candidate/rbcStubLib.c"; then
            RBC_INCLUDE_DIR=`cd "$rbc_candidate" && pwd`
            break
        fi
    done
    if test -z "$RBC_INCLUDE_DIR"; then
        AC_MSG_ERROR([cannot find rbcVector.h, rbcDecls.h and rbcStubLib.c under $rbc_dir])
    fi
    TEA_ADD_INCLUDES([-I"${RBC_INCLUDE_DIR}"])
    AC_DEFINE([HAVE_RBC], [1], [Build RBC vector output support])
    AC_DEFINE([USE_RBC_STUBS], [1], [Use the RBC stubs interface])
    AC_MSG_NOTICE([RBC vector output enabled: $RBC_INCLUDE_DIR])
fi

DOC_HTML_FILES="index.html objects.inv search.html searchindex.js genindex.html"
DOC_SRCDIR="${srcdir}/docs"
if test -d "$DOC_SRCDIR"; then
    # Add all top-level *.html files from docs/, not subdirectories.
    for f in "$DOC_SRCDIR"/*.html; do
        test -f "$f" || continue

        b=`basename "$f"`

        case " $DOC_HTML_FILES " in
            *" $b "*) ;;
            *) DOC_HTML_FILES="$DOC_HTML_FILES $b" ;;
        esac
    done
    # Add everything from docs/_static and docs/assets recursively.
    for d in _static assets; do
        if test -d "$DOC_SRCDIR/$d"; then
            for f in `find "$DOC_SRCDIR/$d" -type f -print`; do
                rel=`echo "$f" | sed "s|^$DOC_SRCDIR/||"`

                case " $DOC_HTML_FILES " in
                    *" $rel "*) ;;
                    *) DOC_HTML_FILES="$DOC_HTML_FILES $rel" ;;
                esac
            done
        fi
    done

else
    AC_MSG_WARN([documentation directory not found: $DOC_SRCDIR])
fi
#AC_MSG_NOTICE([DOC_HTML_FILES = $DOC_HTML_FILES])
AC_SUBST([DOC_HTML_FILES])

# Test files to include in the distribution.
# Paths are relative to $srcdir.
DIST_TEST_FILES="test/all.tcl test/ngspicebridge.test"
AC_SUBST([DIST_TEST_FILES])

TEA_PUBLIC_TCL_HEADERS
TEA_ENABLE_THREADS
TEA_ENABLE_SHARED
TEA_CONFIG_CFLAGS
TEA_ENABLE_SYMBOLS
TEA_MAKE_LIB
TEA_PROG_TCLSH
AC_SUBST(TCLLIBPATH)
AC_CONFIG_FILES([Makefile pkgIndex.tcl])
AC_OUTPUT
