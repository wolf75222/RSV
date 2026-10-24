#!/bin/bash
# Development script for RSV
# Usage: ./dev.sh [clean|configure|build|run|test|demo|all|help]

set -e

ROOT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="$ROOT_DIR/build"
FAKE_BIN="$ROOT_DIR/tests/fake-slurm/bin"

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

print_step() {
    echo -e "${BLUE}━━━ $1 ━━━${NC}"
}

print_success() {
    echo -e "${GREEN}✓ $1${NC}"
}

print_error() {
    echo -e "${RED}✗ $1${NC}"
}

# cmake isn't installed on ROMEO outside Spack: found the same way as in install.sh
find_cmake() {
    command -v cmake >/dev/null 2>&1 && return
    type romeo_load_x64cpu_env >/dev/null 2>&1 || source /etc/profile.d/spack_bash_functions.sh 2>/dev/null || true
    if type romeo_load_x64cpu_env >/dev/null 2>&1; then
        romeo_load_x64cpu_env >/dev/null 2>&1 || true
        local prefix
        prefix=$(spack find --format '{prefix}' cmake 2>/dev/null | tail -n 1 || true)
        [ -n "$prefix" ] && export PATH="$prefix/bin:$PATH"
    fi
    command -v cmake >/dev/null 2>&1 || { print_error "cmake not found"; exit 1; }
}

do_clean() {
    print_step "Removing the build"
    rm -rf "$BUILD_DIR"
    print_success "Build removed"
}

do_configure() {
    print_step "Configuring CMake"
    find_cmake
    cmake -S "$ROOT_DIR" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release -DRSV_TESTS=ON
    print_success "Configured"
}

do_build() {
    print_step "Building"
    find_cmake
    if [ ! -f "$BUILD_DIR/CMakeCache.txt" ]; then
        do_configure
    fi
    cmake --build "$BUILD_DIR" -j "$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)"
    print_success "Built"
}

do_test() {
    do_build
    print_step "Slurm parsing tests"
    "$BUILD_DIR/parse_test"
}

do_run() {
    if [ ! -f "$BUILD_DIR/rsv" ]; then
        print_error "No binary yet: run ./dev.sh build first"
        exit 1
    fi
    "$BUILD_DIR/rsv" "$@"
}

# With a fake Slurm (made-up jobs): anywhere, even without a cluster
do_demo() {
    do_build
    local state="${TMPDIR:-/tmp}/rsv-demo-$USER"
    rm -rf "$state"
    PATH="$FAKE_BIN:$PATH" FAKE_SLURM_STATE="$state" FAKE_SLURM_HOME="$state/home" HOME="$state/home" \
        USER=demo "$BUILD_DIR/rsv" "$@"
}

show_help() {
    echo -e "${YELLOW}RSV - development script${NC}"
    echo ""
    echo "Usage: ./dev.sh [command]"
    echo ""
    echo "Commands:"
    echo "  clean     Remove the build folder"
    echo "  configure Configure CMake (downloads FTXUI if needed)"
    echo "  build     Build (configures if needed)"
    echo "  run       Run RSV"
    echo "  test      Build and run the parsing tests"
    echo "  demo      Run RSV on a fake Slurm (made-up jobs)"
    echo "  all       Clean, build and test"
    echo "  help      Show this help"
    echo ""
    echo "No argument: build. To install in ~/.local/bin: ./install.sh"
}

case "${1:-}" in
    clean)
        do_clean
        ;;
    configure)
        do_configure
        ;;
    build)
        do_build
        ;;
    run)
        shift
        do_run "$@"
        ;;
    test)
        do_test
        ;;
    demo)
        shift
        do_demo "$@"
        ;;
    all)
        do_clean
        do_test
        echo ""
        print_success "Clean build tested"
        ;;
    help|--help|-h)
        show_help
        ;;
    "")
        do_build
        echo ""
        echo -e "${YELLOW}To run it:${NC} ./dev.sh run   ${BLUE}or${NC}   ./dev.sh demo"
        ;;
    *)
        print_error "Unknown command: $1"
        show_help
        exit 1
        ;;
esac
