#!/bin/bash
# Script de développement pour RSV
# Usage: ./dev.sh [clean|build|run|all]

set -e

ROOT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="$ROOT_DIR/build"

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

do_clean() {
    print_step "Nettoyage du build"
    rm -rf "$BUILD_DIR"
    print_success "Build nettoyé"
}

do_configure() {
    print_step "Configuration CMake"
    mkdir -p "$BUILD_DIR"
    cd "$BUILD_DIR"
    cmake ..
    print_success "Configuration terminée"
}

do_build() {
    print_step "Compilation"
    if [ ! -d "$BUILD_DIR" ]; then
        do_configure
    fi
    cd "$BUILD_DIR"
    cmake --build . -j$(nproc 2>/dev/null || echo 4)
    print_success "Compilation terminée"
}

do_run() {
    if [ ! -f "$BUILD_DIR/rsv" ]; then
        print_error "Exécutable non trouvé. Lance d'abord: ./dev.sh build"
        exit 1
    fi
    cd "$BUILD_DIR"
    ./rsv
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
    echo "  all       Clean + Build + affiche la commande pour lancer"
    echo "  help      Affiche cette aide"
    echo ""
    echo "Sans argument: build + affiche la commande pour lancer"
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
        do_run
        ;;
    all)
        do_clean
        do_build
        echo ""
        print_success "Build complet terminé!"
        echo -e "${YELLOW}Pour lancer l'application:${NC}"
        echo -e "  cd $BUILD_DIR && ./rsv"
        echo -e "  ${BLUE}ou${NC}"
        echo -e "  ./dev.sh run"
        ;;
    help|--help|-h)
        show_help
        ;;
    "")
        do_build
        echo ""
        print_success "Compilation terminée!"
        echo -e "${YELLOW}Pour lancer l'application:${NC}"
        echo -e "  cd $BUILD_DIR && ./rsv"
        echo -e "  ${BLUE}ou${NC}"
        echo -e "  ./dev.sh run"
        ;;
    *)
        print_error "Commande inconnue: $1"
        show_help
        exit 1
        ;;
esac
