#!/usr/bin/env bash
# Run script that enters the neuron environment

set -e

# Get the directory of this script
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

# Default program to run
PROGRAM="${1:-efa_discover}"

echo "Running $PROGRAM in neuron environment..."
echo "========================================="

# Check if we're already in the neuron environment
if [[ -n "$IN_NIX_SHELL" ]]; then
    echo "Already in neuron environment, running directly..."
    cd "$SCRIPT_DIR"
    if [[ ! -f "build/$PROGRAM" ]]; then
        echo "Error: build/$PROGRAM not found. Please build first with ./build.sh"
        exit 1
    fi
    exec "./build/$PROGRAM" "${@:2}"
else
    echo "Entering neuron environment..."
    echo ""

    # Use the use-neuron script to enter the environment and run
    exec /root/code/concourse/neuron/use-neuron bash -c "cd '$SCRIPT_DIR' && ./build/$PROGRAM ${*:2}"
fi
