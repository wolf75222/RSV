#!/bin/bash
# Script de développement pour RSV
# Usage: ./dev.sh [clean|configure|build|run|test|demo|all|help]

set -e

ROOT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="$ROOT_DIR/build"
FAKE_BIN="$ROOT_DIR/tests/fake-slurm/bin"

# Couleurs
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

# cmake n'est pas installé sur ROMEO hors Spack: même recherche que install.sh
find_cmake() {
    command -v cmake >/dev/null 2>&1 && return
    type romeo_load_x64cpu_env >/dev/null 2>&1 || source /etc/profile.d/spack_bash_functions.sh 2>/dev/null || true
    if type romeo_load_x64cpu_env >/dev/null 2>&1; then
        romeo_load_x64cpu_env >/dev/null 2>&1 || true
        local prefix
        prefix=$(spack find --format '{prefix}' cmake 2>/dev/null | tail -n 1 || true)
        [ -n "$prefix" ] && export PATH="$prefix/bin:$PATH"
    fi
    command -v cmake >/dev/null 2>&1 || { print_error "cmake introuvable"; exit 1; }
}

do_clean() {
    print_step "Nettoyage du build"
    rm -rf "$BUILD_DIR"
    print_success "Build nettoyé"
}

do_configure() {
    print_step "Configuration CMake"
    find_cmake
    cmake -S "$ROOT_DIR" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release -DRSV_TESTS=ON
    print_success "Configuration terminée"
}

do_build() {
    print_step "Compilation"
    find_cmake
    if [ ! -f "$BUILD_DIR/CMakeCache.txt" ]; then
        do_configure
    fi
    cmake --build "$BUILD_DIR" -j "$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)"
    print_success "Compilation terminée"
}

do_test() {
    do_build
    print_step "Tests du parsing Slurm"
    "$BUILD_DIR/parse_test"
}

do_run() {
    if [ ! -f "$BUILD_DIR/rsv" ]; then
        print_error "Exécutable non trouvé. Lance d'abord: ./dev.sh build"
        exit 1
    fi
    "$BUILD_DIR/rsv" "$@"
}

# Avec un faux Slurm (jobs inventés): partout, même sans cluster
do_demo() {
    do_build
    local state="${TMPDIR:-/tmp}/rsv-demo-$USER"
    rm -rf "$state"
    PATH="$FAKE_BIN:$PATH" FAKE_SLURM_STATE="$state" FAKE_SLURM_HOME="$state/home" HOME="$state/home" \
        USER=demo "$BUILD_DIR/rsv" "$@"
}

show_help() {
    echo -e "${YELLOW}RSV - Script de développement${NC}"
    echo ""
    echo "Usage: ./dev.sh [commande]"
    echo ""
    echo "Commandes:"
    echo "  clean     Supprime le dossier build"
    echo "  configure Configure CMake (télécharge FTXUI si nécessaire)"
    echo "  build     Compile le projet (configure si nécessaire)"
    echo "  run       Lance l'application"
    echo "  test      Compile et lance les tests du parsing"
    echo "  demo      Lance l'application avec un faux Slurm (jobs inventés)"
    echo "  all       Clean + Build + Tests"
    echo "  help      Affiche cette aide"
    echo ""
    echo "Sans argument: build. Pour installer dans ~/.local/bin: ./install.sh"
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
        print_success "Build complet terminé!"
        ;;
    help|--help|-h)
        show_help
        ;;
    "")
        do_build
        echo ""
        echo -e "${YELLOW}Pour lancer l'application:${NC} ./dev.sh run   ${BLUE}ou${NC}   ./dev.sh demo"
        ;;
    *)
        print_error "Commande inconnue: $1"
        show_help
        exit 1
        ;;
esac
