# Runs at BUILD time, not configure time.
#
# The title bar is the only proof of what is running, so the hash it shows must
# be the hash the binary was built from. Capturing it in execute_process at
# configure time meant a rebuild after a commit kept the previous hash: the
# title claimed a commit the binary predated. Worse, it said nothing about
# uncommitted edits, which is the normal state while working.
#
# Invoked with -D SRC_DIR=... -D OUT_FILE=... -D VERSION=...

execute_process(COMMAND git rev-parse --short HEAD
                WORKING_DIRECTORY ${SRC_DIR}
                OUTPUT_VARIABLE GIT_HASH OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
if(NOT GIT_HASH)
    set(GIT_HASH "nogit")
endif()

# Any tracked modification, staged or not. Untracked files do not count: they
# cannot change what the compiler saw.
execute_process(COMMAND git status --porcelain --untracked-files=no
                WORKING_DIRECTORY ${SRC_DIR}
                OUTPUT_VARIABLE GIT_DIRTY OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
if(GIT_DIRTY)
    set(GIT_HASH "${GIT_HASH}-dirty")
endif()

set(CONTENT "// Generated at build time by StampBuildInfo.cmake. Do not edit.\n")
string(APPEND CONTENT "#pragma once\n")
string(APPEND CONTENT "#define EJMAP_VERSION \"${VERSION}\"\n")
string(APPEND CONTENT "#define EJMAP_GIT_HASH \"${GIT_HASH}\"\n")

# Only rewrite when it actually changes, so an unchanged hash does not force a
# relink on every build.
set(EXISTING "")
if(EXISTS ${OUT_FILE})
    file(READ ${OUT_FILE} EXISTING)
endif()
if(NOT EXISTING STREQUAL CONTENT)
    file(WRITE ${OUT_FILE} "${CONTENT}")
    message(STATUS "ejmap build stamp: ${VERSION} (${GIT_HASH})")
endif()
