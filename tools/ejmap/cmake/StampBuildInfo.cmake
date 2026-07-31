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

# apply_header_sha: the hash of the shared apply header AS THE COMPILER IS
# ABOUT TO SEE IT. This stamp target runs before compilation on every build,
# so the hash in the binary is the hash of what that binary compiled -- if the
# header changes later without a rebuild, the RUNNING binary keeps the hash of
# what it was built from, which is the correct claim. Hashing at map-write
# time from disk would break exactly there: this project has already been
# bitten by a stamp that lagged its binary.
file(SHA256 ${APPLY_HEADER} APPLY_SHA)

set(CONTENT "// Generated at build time by StampBuildInfo.cmake. Do not edit.\n")
string(APPEND CONTENT "#pragma once\n")
string(APPEND CONTENT "#define EJMAP_VERSION \"${VERSION}\"\n")
string(APPEND CONTENT "#define EJMAP_GIT_HASH \"${GIT_HASH}\"\n")
string(APPEND CONTENT "#define EJMAP_APPLY_HEADER_SHA \"${APPLY_SHA}\"\n")

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
