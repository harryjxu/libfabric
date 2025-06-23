#!/usr/bin/env bash
# Build script for libfabric C reproduction test

set -e

# Get the directory of this script
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# Function to print colored output
print_info() {
    echo -e "${GREEN}[INFO]${NC} $1"
}

print_warn() {
    echo -e "${YELLOW}[WARN]${NC} $1"
}

print_error() {
    echo -e "${RED}[ERROR]${NC} $1"
}

# Show help
show_help() {
    cat << EOF
Build script for libfabric endpoint close bug reproduction

Usage: $0 [OPTIONS] [MAKE_TARGETS]

OPTIONS:
    -h, --help              Show this help message
    -l, --libfabric-lib     Path to libfabric.so
    -i, --libfabric-inc     Path to libfabric include directory
    -p, --libfabric-prefix  Libfabric installation prefix (e.g., /opt/amazon/efa)
    --portable              Use portable Makefile (no nix dependencies)

ENVIRONMENT VARIABLES:
    LIBFABRIC_LIB          Path to libfabric.so
    LIBFABRIC_INC          Path to libfabric include directory
    LIBFABRIC_PREFIX       Libfabric installation prefix

EXAMPLES:
    # Auto-detect (tries nix-shell, pkg-config, standard locations)
    $0 all
    ./run.sh repro_bug

    # Use specific libfabric installation
    $0 --libfabric-prefix /opt/amazon/efa all
    ./run.sh repro_bug

    # Use custom libfabric build
    $0 -l ~/libfabric/build/lib/libfabric.so -i ~/libfabric/include all
    ./run.sh repro_bug

    # Use environment variables
    LIBFABRIC_PREFIX=/usr/local $0 all
    ./run.sh repro_bug

BUILD MODES:
    1. Nix build (default): Uses neuron-env from nix store
       - Automatically finds libfabric in nix store
       - No additional configuration needed
       - Run 'use-neuron' first if not in nix shell

    2. Portable build: Uses system or specified libfabric
       - Triggered by --portable or when LIBFABRIC_* is set
       - Works with any libfabric installation
       - Suitable for sharing with external teams

The script will attempt to detect your environment and choose the
appropriate build mode automatically.

AFTER BUILDING:
    To run the bug reproduction test:
        ./run.sh repro_bug

    This will reproduce the ofi_buf_is_valid assertion at iteration ~104.

EOF
}

# Parse command line arguments
PORTABLE_BUILD=0
MAKE_ARGS=()

while [[ $# -gt 0 ]]; do
    case $1 in
        -h|--help)
            show_help
            exit 0
            ;;
        -l|--libfabric-lib)
            export LIBFABRIC_LIB="$2"
            PORTABLE_BUILD=1
            shift 2
            ;;
        -i|--libfabric-inc)
            export LIBFABRIC_INC="$2"
            PORTABLE_BUILD=1
            shift 2
            ;;
        -p|--libfabric-prefix)
            export LIBFABRIC_PREFIX="$2"
            PORTABLE_BUILD=1
            shift 2
            ;;
        --portable)
            PORTABLE_BUILD=1
            shift
            ;;
        *)
            MAKE_ARGS+=("$1")
            shift
            ;;
    esac
done

# If no make targets specified, default to 'all'
if [ ${#MAKE_ARGS[@]} -eq 0 ]; then
    MAKE_ARGS=("all")
fi

# Check if we should use portable build
if [ -n "$LIBFABRIC_LIB" ] || [ -n "$LIBFABRIC_INC" ] || [ -n "$LIBFABRIC_PREFIX" ]; then
    PORTABLE_BUILD=1
fi

print_info "Building libfabric C reproduction test..."
echo "========================================"

cd "$SCRIPT_DIR"

if [ $PORTABLE_BUILD -eq 1 ]; then
    print_info "Using portable build mode"

    # Build environment string for make
    MAKE_ENV=""
    if [ -n "$LIBFABRIC_LIB" ]; then
        MAKE_ENV="$MAKE_ENV LIBFABRIC_LIB=$LIBFABRIC_LIB"
        print_info "  LIBFABRIC_LIB=$LIBFABRIC_LIB"
    fi
    if [ -n "$LIBFABRIC_INC" ]; then
        MAKE_ENV="$MAKE_ENV LIBFABRIC_INC=$LIBFABRIC_INC"
        print_info "  LIBFABRIC_INC=$LIBFABRIC_INC"
    fi
    if [ -n "$LIBFABRIC_PREFIX" ]; then
        MAKE_ENV="$MAKE_ENV LIBFABRIC_PREFIX=$LIBFABRIC_PREFIX"
        print_info "  LIBFABRIC_PREFIX=$LIBFABRIC_PREFIX"
    fi

    # Use portable Makefile
    if [ -f "Makefile.portable" ]; then
        print_info "Using Makefile.portable"
        make -f Makefile.portable $MAKE_ENV "${MAKE_ARGS[@]}"
    else
        print_error "Makefile.portable not found!"
        print_info "Using standard Makefile with environment variables"
        make $MAKE_ENV "${MAKE_ARGS[@]}"
    fi
else
    # Try nix build
    print_info "Checking for nix/neuron environment..."

    if [[ -n "$IN_NIX_SHELL" ]]; then
        print_info "Already in nix shell, building directly..."
        make "${MAKE_ARGS[@]}"
    elif [ -x "/root/code/concourse/neuron/use-neuron" ]; then
        print_info "Found use-neuron script, entering neuron environment..."
        exec /root/code/concourse/neuron/use-neuron bash -c "cd '$SCRIPT_DIR' && make ${MAKE_ARGS[*]}"
    elif command -v nix-shell &> /dev/null && ls /nix/store/*-neuron-env 2>/dev/null | head -1 > /dev/null; then
        print_info "Found neuron-env in nix store, using nix-shell..."
        NEURON_ENV=$(ls /nix/store/*-neuron-env 2>/dev/null | head -1)
        print_info "Using: $NEURON_ENV"
        nix-shell "$NEURON_ENV" --run "make ${MAKE_ARGS[*]}"
    else
        print_warn "No nix environment found, trying system libfabric..."

        # Check for pkg-config
        if pkg-config --exists libfabric 2>/dev/null; then
            print_info "Found libfabric via pkg-config"
            make "${MAKE_ARGS[@]}"
        elif [ -d "/opt/amazon/efa" ]; then
            print_info "Found AWS EFA installation at /opt/amazon/efa"
            LIBFABRIC_PREFIX=/opt/amazon/efa make "${MAKE_ARGS[@]}"
        else
            print_error "Cannot find libfabric!"
            echo ""
            echo "Please either:"
            echo "  1. Run 'use-neuron' to enter nix environment"
            echo "  2. Install libfabric system-wide"
            echo "  3. Specify libfabric location:"
            echo "     $0 --libfabric-prefix /path/to/libfabric all"
            echo ""
            echo "Run '$0 --help' for more options"
            exit 1
        fi
    fi
fi

print_info "Build complete!"
