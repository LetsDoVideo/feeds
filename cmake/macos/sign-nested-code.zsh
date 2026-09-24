#!/bin/zsh
# Re-sign every piece of nested code under a directory with our Developer ID,
# deepest first, so the enclosing bundle can then be signed over it.
#
#   sign-nested-code.zsh <identity> <directory>
#
# Used on FeedsEngine.app/Contents/Frameworks, which holds the Zoom macOS SDK
# exactly as Zoom ships it: frameworks, bundles and loose dylibs signed with
# ZOOM's certificate, some without a secure timestamp or the hardened runtime.
# Notarization rejects any Mach-O in the submission that is not signed with a
# Developer ID, timestamped, and (for executables) hardened, so every one of
# them has to be re-signed with ours.
#
# Order matters. A bundle's signature seals the code inside it, so anything
# nested must be signed BEFORE the bundle that contains it, or signing the inner
# item afterwards breaks the outer seal. Two passes do that:
#   1. every Mach-O file on its own (a bundle's main executable included; the
#      bundle signature in step 2 simply replaces that one), then
#   2. every bundle that contains Mach-O code, deepest path first.
# Symlinks are never followed (find without -L), so a framework's
# Versions/Current and top-level aliases are not signed twice.
#
# --preserve-metadata keeps each item's identifier and entitlements; only the
# signer, timestamp and runtime flag change.

emulate -L zsh
setopt ERR_EXIT PIPE_FAIL NO_UNSET EXTENDED_GLOB

if (( # != 2 )) {
  print -u2 "usage: ${0:t} <identity> <directory>"
  exit 2
}

local -r identity=${1}
local -r root=${2:A}

if [[ ! -d ${root} ]] {
  print -u2 "${0:t}: not a directory: ${root}"
  exit 2
}

local -a sign_args=(
  --force
  --sign "${identity}"
  --timestamp
  --options runtime
  --preserve-metadata=identifier,entitlements
)

# Mach-O detection by content, not by name or mode: dylibs are often not
# executable, and helper binaries have no extension.
local -a macho_files=()
local f
while IFS= read -r -d '' f; do
  if [[ "$(/usr/bin/file -b "${f}")" == *Mach-O* ]] macho_files+=("${f}")
done < <(find "${root}" -type f -print0)

# The bundle(s) enclosing each Mach-O file, up to (not including) root. For a
# versioned framework the thing codesign seals is Versions/<v>, not the
# .framework directory itself.
local -A bundles=()
local dir
for f (${macho_files}) {
  dir=${f:h}
  while [[ ${dir} != ${root} && ${dir} == ${root}/* ]] {
    if [[ ${dir:h:t} == Versions && ${dir:h:h} == *.framework ]] {
      bundles[${dir}]=1
    } elif [[ ${dir} == *.(app|bundle|xpc|appex|plugin) ]] {
      bundles[${dir}]=1
    } elif [[ ${dir} == *.framework && ! -d ${dir}/Versions ]] {
      bundles[${dir}]=1
    }
    dir=${dir:h}
  }
}

# Deepest first: sort on the number of path components, descending.
by_depth() {
  local p
  for p (${@}) print -r -- "${#${(s:/:)p}} ${p}"
}

local -i count=0
local line
for line (${(f)"$(by_depth ${macho_files} | sort -rn -k1,1)"}) {
  /usr/bin/codesign ${sign_args} "${line#* }"
  (( ++count ))
}
for line (${(f)"$(by_depth ${(k)bundles} | sort -rn -k1,1)"}) {
  /usr/bin/codesign ${sign_args} "${line#* }"
  (( ++count ))
}

print "${0:t}: re-signed ${count} nested items (${#macho_files} Mach-O files, ${#bundles} bundles) under ${root}"
