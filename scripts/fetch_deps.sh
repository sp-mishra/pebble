#!/usr/bin/env bash
# =============================================================================
# fetch_deps.sh — Dependency Fetcher for Pebble
# =============================================================================
# Downloads, unzips, and cleans up dependency archives from GitHub releases.
# Compatible with bash 3.2+ (macOS system bash).
#
# Usage:
#   ./scripts/fetch_deps.sh [OPTIONS] [LIBRARY...]
#
# Options:
#   -g, --group <name>    Download a predefined group of libraries
#   -d, --deps-dir <dir>  Target directory (default: <repo>/dependencies)
#   -f, --force           Re-download even if the folder already exists
#   -q, --quiet           Suppress verbose output (verbose is ON by default)
#   -l, --list            List all known libraries and groups, then exit
#   -h, --help            Show this help message and exit
#
# Examples:
#   ./scripts/fetch_deps.sh                     # download all missing deps
#   ./scripts/fetch_deps.sh --group pebble      # download the pebble group
#   ./scripts/fetch_deps.sh glaze spdlog        # download specific libs
#   ./scripts/fetch_deps.sh --force Catch2      # force re-download
#   ./scripts/fetch_deps.sh --quiet             # suppress verbose output
#   ./scripts/fetch_deps.sh -d /tmp/deps        # custom output directory
# =============================================================================

set -euo pipefail

# ---------------------------------------------------------------------------
# Colour helpers (disabled when not a terminal)
# ---------------------------------------------------------------------------
if [[ -t 1 ]]; then
    RED="\033[0;31m"; GREEN="\033[0;32m"; YELLOW="\033[1;33m"
    CYAN="\033[0;36m"; MAGENTA="\033[0;35m"; DIM="\033[2m"
    BOLD="\033[1m"; RESET="\033[0m"
else
    RED=""; GREEN=""; YELLOW=""; CYAN=""; MAGENTA=""; DIM=""
    BOLD=""; RESET=""
fi

# ---------------------------------------------------------------------------
# Logging
# ---------------------------------------------------------------------------
VERBOSE=1   # on by default; --quiet sets to 0

log_ok()      { printf "${GREEN}[OK]${RESET}      %s\n" "$*"; }
log_error()   { printf "${RED}[ERROR]${RESET}   %s\n" "$*" >&2; }
log_warn()    { printf "${YELLOW}[WARN]${RESET}    %s\n" "$*"; }
log_section() { printf "\n${BOLD}════════════════════════════════════════\n  %s\n════════════════════════════════════════${RESET}\n" "$*"; }
log_info()    { [[ $VERBOSE -eq 1 ]] && printf "${CYAN}[INFO]${RESET}    %s\n"    "$*" || true; }
log_detail()  { [[ $VERBOSE -eq 1 ]] && printf "${DIM}          %s${RESET}\n"     "$*" || true; }
log_step()    { [[ $VERBOSE -eq 1 ]] && printf "${MAGENTA}[STEP]${RESET}    %s\n" "$*" || true; }

# ---------------------------------------------------------------------------
# Library registry — parallel arrays (bash 3.2 compatible)
# ---------------------------------------------------------------------------
LIB_KEYS=(    "Catch2"       "crc32c"    "glaze"    "liblmdb"    "highway"    "spdlog"    )
LIB_URLS=(
    "https://github.com/sp-mishra/dependencies/releases/download/v0.1/Catch2-3.9.0.zip"
    "https://github.com/sp-mishra/dependencies/releases/download/v0.1/crc32c.zip"
    "https://github.com/sp-mishra/dependencies/releases/download/v0.1/glaze.zip"
    "https://github.com/sp-mishra/dependencies/releases/download/v0.1/liblmdb.zip"
    "https://github.com/sp-mishra/dependencies/releases/download/v0.1/highway.zip"
    "https://github.com/sp-mishra/dependencies/releases/download/v0.1/spdlog.zip"
)
LIB_FOLDERS=( "Catch2-3.9.0" "crc32c"    "glaze"    "liblmdb"    "highway"    "spdlog"    )

# ---------------------------------------------------------------------------
# Groups
# ---------------------------------------------------------------------------
GROUP_PEBBLE="Catch2 crc32c glaze liblmdb highway spdlog"
GROUP_ALL="Catch2 crc32c glaze liblmdb highway spdlog"

# ---------------------------------------------------------------------------
# Lookup helpers
# ---------------------------------------------------------------------------
find_lib_index() {
    local key="$1"; local i
    for i in "${!LIB_KEYS[@]}"; do
        [[ "${LIB_KEYS[$i]}" == "$key" ]] && { echo "$i"; return 0; }
    done
    echo "-1"; return 1
}

resolve_group() {
    case "$1" in
        pebble) echo "$GROUP_PEBBLE" ;;
        all)    echo "$GROUP_ALL"    ;;
        *)      return 1 ;;
    esac
}

human_bytes() {
    local b="${1:-0}"
    if   (( b >= 1073741824 )); then printf "%.1f GiB" "$(echo "scale=1; $b/1073741824" | bc)"
    elif (( b >= 1048576    )); then printf "%.1f MiB" "$(echo "scale=1; $b/1048576"    | bc)"
    elif (( b >= 1024       )); then printf "%.1f KiB" "$(echo "scale=1; $b/1024"       | bc)"
    else printf "%d B" "$b"
    fi
}

# ---------------------------------------------------------------------------
# Defaults
# ---------------------------------------------------------------------------
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
DEPS_DIR="${REPO_ROOT}/dependencies"
FORCE=0
MODE="missing"
GROUP_NAME=""
SPECIFIC_LIBS=()

# ---------------------------------------------------------------------------
# Usage / list
# ---------------------------------------------------------------------------
usage() {
    sed -n '/^# Usage:/,/^# ===/p' "$0" | sed 's/^# \?//'
    exit 0
}

list_libs() {
    log_section "Known Libraries"
    local i
    for i in "${!LIB_KEYS[@]}"; do
        printf "  ${BOLD}%-20s${RESET} ${DIM}%s${RESET}\n" "${LIB_KEYS[$i]}" "${LIB_URLS[$i]}"
    done
    log_section "Known Groups"
    printf "  ${BOLD}%-20s${RESET} %s\n" "pebble" "$GROUP_PEBBLE"
    printf "  ${BOLD}%-20s${RESET} %s\n" "all"    "$GROUP_ALL"
    exit 0
}

# ---------------------------------------------------------------------------
# Parse arguments
# ---------------------------------------------------------------------------
while [[ $# -gt 0 ]]; do
    case "$1" in
        -g|--group)
            [[ -z "${2:-}" ]] && { log_error "--group requires a group name"; exit 1; }
            GROUP_NAME="$2"; MODE="group"; shift 2 ;;
        -d|--deps-dir)
            [[ -z "${2:-}" ]] && { log_error "--deps-dir requires a path"; exit 1; }
            DEPS_DIR="$2"; shift 2 ;;
        -f|--force)   FORCE=1;    shift ;;
        -q|--quiet)   VERBOSE=0;  shift ;;
        -l|--list)    list_libs ;;
        -h|--help)    usage ;;
        -*)
            log_error "Unknown option: $1  (use --help for usage)"; exit 1 ;;
        *)
            SPECIFIC_LIBS+=("$1"); MODE="specific"; shift ;;
    esac
done

# ---------------------------------------------------------------------------
# Resolve target library set
# ---------------------------------------------------------------------------
TARGET_LIBS=()
case "$MODE" in
    group)
        local_group="$(resolve_group "$GROUP_NAME" 2>/dev/null || true)"
        if [[ -z "$local_group" ]]; then
            log_error "Unknown group '${GROUP_NAME}'. Use --list to see available groups."
            exit 1
        fi
        read -ra TARGET_LIBS <<< "$local_group"
        ;;
    specific)
        TARGET_LIBS=("${SPECIFIC_LIBS[@]}")
        ;;
    missing)
        TARGET_LIBS=("${LIB_KEYS[@]}")
        ;;
esac

# ---------------------------------------------------------------------------
# Pre-flight: verify tools, detect HTTP/2
# ---------------------------------------------------------------------------
CURL_EXTRA_FLAGS=()

check_tools() {
    log_step "Checking required tools …"
    local missing_tools=()
    for tool in curl unzip; do
        if command -v "$tool" &>/dev/null; then
            log_detail "$(printf "%-10s → %s" "$tool" "$(command -v "$tool")")"
        else
            missing_tools+=("$tool")
        fi
    done
    if [[ ${#missing_tools[@]} -gt 0 ]]; then
        log_error "Required tools not found: ${missing_tools[*]}"
        exit 1
    fi

    if curl --http2 --silent --head "https://github.com" -o /dev/null 2>/dev/null; then
        CURL_EXTRA_FLAGS=(--http2)
        log_detail "HTTP/2   : enabled"
    else
        log_detail "HTTP/2   : not available — using HTTP/1.1"
    fi
}

# ---------------------------------------------------------------------------
# Purge stale artefacts left by previous crashed/partial runs
# ---------------------------------------------------------------------------
cleanup_stale() {
    log_step "Cleaning up any stale temp dirs …"
    local stale found=0
    for stale in "${DEPS_DIR}"/.tmp_*; do
        if [[ -d "$stale" ]]; then
            log_warn "Removing stale temp dir: ${stale}"
            rm -rf "$stale"
            found=1
        fi
    done
    # Also remove any stray *.zip files left over from crashed downloads
    for stale in "${DEPS_DIR}"/*.zip; do
        if [[ -f "$stale" ]]; then
            log_warn "Removing stale zip: ${stale}"
            rm -f "$stale"
            found=1
        fi
    done
    [[ $found -eq 0 ]] && log_detail "Nothing stale found."
}

# ---------------------------------------------------------------------------
# Download and extract a single library.
# Returns 0 on success (FETCH_RESULT set to "skip" or "fetch").
# Returns 1 on error.
# ---------------------------------------------------------------------------
FETCH_RESULT=""

fetch_library() {
    local name="$1"
    FETCH_RESULT="error"

    local idx
    idx="$(find_lib_index "$name")" || true
    if [[ "$idx" == "-1" ]]; then
        log_error "Unknown library '${name}'. Use --list to see available libraries."
        return 1
    fi

    local url="${LIB_URLS[$idx]}"
    local folder="${LIB_FOLDERS[$idx]}"
    local target_dir="${DEPS_DIR}/${folder}"

    log_info "Library  : ${BOLD}${name}${RESET}"
    log_detail "URL      : ${url}"
    log_detail "Target   : ${target_dir}"

    # ---- Skip if already present ----
    if [[ -d "$target_dir" && $FORCE -eq 0 ]]; then
        local n_files
        n_files="$(find "$target_dir" -type f | wc -l | tr -d ' ')"
        log_detail "Contents : ${n_files} files already on disk"
        log_ok "${name} already present — skipping."
        FETCH_RESULT="skip"
        return 0
    fi

    if [[ -d "$target_dir" && $FORCE -eq 1 ]]; then
        log_warn "${name} present but --force set; removing for re-download."
        rm -rf "$target_dir"
    fi

    local zip_name zip_path tmp_dir
    zip_name="$(basename "$url")"
    # Use a unique tmp name for the zip to avoid conflicts with any directory of the same base name
    zip_path="${DEPS_DIR}/.dl_${name}_$$.zip"
    tmp_dir="${DEPS_DIR}/.tmp_${name}_$$"

    # ---- Download ----
    log_step "Downloading ${name} …"

    local curl_flags=(
        --location --fail --compressed
        --retry 3 --retry-delay 2
        --connect-timeout 15
        --speed-limit 1024 --speed-time 30
    )
    if [[ "${#CURL_EXTRA_FLAGS[@]}" -gt 0 ]]; then
        curl_flags+=("${CURL_EXTRA_FLAGS[@]}")
    fi

    local curl_rc=0
    if [[ $VERBOSE -eq 1 && -t 1 ]]; then
        curl --progress-bar "${curl_flags[@]}" --output "$zip_path" "$url" 2>&1 || curl_rc=$?
    else
        curl --silent --show-error "${curl_flags[@]}" --output "$zip_path" "$url" || curl_rc=$?
    fi

    if [[ $curl_rc -ne 0 ]]; then
        log_error "Download failed for '${name}' (curl exit ${curl_rc})"
        rm -f "$zip_path"
        return 1
    fi

    # Verify the downloaded file is a real file with content
    if [[ ! -f "$zip_path" ]]; then
        log_error "Download completed but file not found at: ${zip_path}"
        return 1
    fi

    local zip_size
    zip_size="$(wc -c < "$zip_path" | tr -d ' ')"
    if [[ "$zip_size" -eq 0 ]]; then
        log_error "Downloaded file is empty: ${zip_path}"
        rm -f "$zip_path"
        return 1
    fi
    log_detail "Archive  : $(human_bytes "$zip_size")"

    # ---- Extract ----
    log_step "Extracting ${name} …"
    mkdir -p "$tmp_dir"

    local unzip_rc=0
    if [[ $VERBOSE -eq 1 ]]; then
        unzip "$zip_path" -d "$tmp_dir" || unzip_rc=$?
    else
        unzip -q "$zip_path" -d "$tmp_dir" || unzip_rc=$?
    fi

    # Zip no longer needed — remove immediately
    rm -f "$zip_path"

    if [[ $unzip_rc -ne 0 ]]; then
        log_error "Extraction failed for '${name}'"
        rm -rf "$tmp_dir"
        return 1
    fi

    # Strip macOS resource-fork ghost directory injected by zip tool
    rm -rf "${tmp_dir}/__MACOSX"

    # Find the single real content directory (ignore dot-prefixed entries)
    local real_dirs=()
    while IFS= read -r -d $'\0' entry; do
        local bname
        bname="$(basename "$entry")"
        [[ "$bname" == __MACOSX || "$bname" == .* ]] && continue
        real_dirs+=("$entry")
    done < <(find "$tmp_dir" -maxdepth 1 -mindepth 1 -type d -print0)

    log_detail "Zip layout : ${#real_dirs[@]} real top-level dir(s)"

    # ---- Install ----
    log_step "Installing ${name} …"

    # Always remove target first — unconditionally, no [[ -d ]] guard.
    # This avoids mv nesting inside an existing non-empty directory.
    rm -rf "$target_dir"

    if [[ ${#real_dirs[@]} -eq 1 ]]; then
        # Single wrapper dir: rename it to target_dir
        mv "${real_dirs[0]}" "$target_dir"
        rm -rf "$tmp_dir"
    else
        # Files at zip root: the tmp_dir is the content itself
        mv "$tmp_dir" "$target_dir"
    fi

    local installed_files
    installed_files="$(find "$target_dir" -type f | wc -l | tr -d ' ')"
    log_detail "Installed : ${installed_files} files → ${target_dir}"
    log_ok "${name} installed successfully."
    FETCH_RESULT="fetch"
    return 0
}

# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------
main() {
    log_section "Pebble Dependency Fetcher"

    log_info "Mode     : ${MODE}"
    log_info "Deps dir : ${DEPS_DIR}"
    log_info "Force    : $( [[ $FORCE -eq 1 ]] && echo "yes" || echo "no" )"
    log_info "Targets  : ${TARGET_LIBS[*]}"

    check_tools
    mkdir -p "$DEPS_DIR"
    cleanup_stale

    local failed=()
    local fetched=0 skipped=0
    local total="${#TARGET_LIBS[@]}"
    local current=0
    local lib_word; (( total == 1 )) && lib_word="library" || lib_word="libraries"

    log_section "Fetching ${total} ${lib_word}"

    local lib
    for lib in "${TARGET_LIBS[@]}"; do
        ((current++)) || true

        if [[ $VERBOSE -eq 1 ]]; then
            printf "\n${DIM}──── [%d/%d] %s ────────────────────────────────${RESET}\n" \
                "$current" "$total" "$lib"
        fi

        local rc=0
        fetch_library "$lib" || rc=$?

        if [[ $rc -ne 0 ]]; then
            failed+=("$lib")
        else
            case "$FETCH_RESULT" in
                fetch) ((fetched++)) || true ;;
                skip)  ((skipped++)) || true ;;
            esac
        fi
    done

    log_section "Summary"
    printf "  ${BOLD}%-14s${RESET} %d\n"  "Processed"  "$total"
    printf "  ${GREEN}%-14s${RESET} %d\n" "Fetched"    "$fetched"
    printf "  ${CYAN}%-14s${RESET} %d\n"  "Skipped"    "$skipped"

    if [[ ${#failed[@]} -gt 0 ]]; then
        printf "  ${RED}%-14s${RESET} %d — %s\n" "Failed" "${#failed[@]}" "${failed[*]}"
        log_error "One or more dependencies could not be fetched."
        exit 1
    fi

    printf "\n"
    log_ok "All ${total} dependencies are ready in: ${DEPS_DIR}"
}

main
