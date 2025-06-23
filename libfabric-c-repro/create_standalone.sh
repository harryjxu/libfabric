#!/bin/bash
# Create a standalone tarball for AWS to reproduce the bug

set -e

echo "Creating standalone libfabric bug reproduction package..."

# Create temporary directory
TMPDIR=$(mktemp -d)
PKGNAME="libfabric-endpoint-close-bug"
PKGDIR="$TMPDIR/$PKGNAME"

# Create package structure
mkdir -p "$PKGDIR"/{src,include}

# Copy source files
cp src/repro_bug.c "$PKGDIR/src/"
cp src/completion_thread.c "$PKGDIR/src/"
cp include/*.h "$PKGDIR/include/"

# Copy portable Makefile
cp Makefile.portable "$PKGDIR/Makefile"

# Create README for AWS
cat > "$PKGDIR/README.md" << 'EOF'
# Libfabric Endpoint Close Bug Reproduction

This is a minimal C reproduction of the `ofi_buf_is_valid` assertion failure
that occurs when closing EFA endpoints during active transfers.

## Bug Details

- **Assertion**: `ofi_buf_is_valid(buf)` at ofi_mem.h:460
- **Trigger**: Closing endpoints while transfers are in progress
- **Provider**: EFA (Elastic Fabric Adapter)
- **Reproducible**: Yes, with seed 42 at iteration ~104

## Prerequisites

- libfabric with EFA provider support
- At least 2 EFA devices
- GCC compiler

## Building

### Option 1: Auto-detect libfabric
```bash
make
```

### Option 2: Specify AWS EFA location
```bash
make LIBFABRIC_PREFIX=/opt/amazon/efa
```

### Option 3: Custom libfabric build
```bash
make LIBFABRIC_LIB=/path/to/libfabric.so LIBFABRIC_INC=/path/to/include
```

## Running

```bash
./build_portable/repro_bug
```

The test will:
1. Create 2 domains (simulating devices)
2. Run iterations creating/destroying endpoints
3. Randomly kill endpoints during transfers
4. Trigger the assertion around iteration 104 (~12 seconds)

## Expected Output

```
[INFO] Using hardcoded seed: 42
...
[INFO] [Iter 104 @ 12.2s] Scenario: kill_sender, delay: 16ms
[INFO] 🔪 KILLING endpoint 0
repro_bug: ./include/ofi_mem.h:460: ofi_buf_free: Assertion `ofi_buf_is_valid(buf)' failed.
Aborted (core dumped)
```

## Architecture

- Uses 3 threads: main + 2 completion worker threads
- All libfabric operations happen in completion threads
- Matches the threading model that triggers the bug
- Includes Python-like timing delays to reproduce the race condition

## Integration with fabtests

This test could be integrated into fabtests as:
- `fabtests/functional/fi_rdm_endpoint_close_stress`
- Add to `fabtests/functional/Makefile.am`
- Follow fabtests conventions for options and logging

## Debugging

To get more libfabric debug output:
```bash
FI_LOG_LEVEL=debug ./build_portable/repro_bug
```

To use with gdb:
```bash
gdb ./build_portable/repro_bug
run
# When it crashes:
bt
frame 5  # or wherever ofi_buf_free is
print *buf
```
EOF

# Create integration example for fabtests
cat > "$PKGDIR/fabtests_integration.patch" << 'EOF'
--- a/fabtests/functional/Makefile.am
+++ b/fabtests/functional/Makefile.am
@@ -20,7 +20,8 @@ bin_PROGRAMS = \
 	functional/fi_rdm_shared_av \
 	functional/fi_rdm_stress \
 	functional/fi_rdm_tagged_peek \
-	functional/fi_multi_mr
+	functional/fi_multi_mr \
+	functional/fi_rdm_endpoint_close_stress

 functional_fi_msg_SOURCES = \
 	functional/msg.c \
@@ -50,6 +51,9 @@ functional_fi_rdm_atomic_SOURCES = \
 functional_fi_rdm_stress_SOURCES = \
 	functional/rdm_stress.c

+functional_fi_rdm_endpoint_close_stress_SOURCES = \
+	functional/rdm_endpoint_close_stress.c
+
 functional_fi_poll_SOURCES = \
 	functional/poll.c \
 	common/shared.c
EOF

# Create a simplified version without our custom timing
cat > "$PKGDIR/src/repro_bug_minimal.c" << 'EOF'
/*
 * Minimal version without Anthropic-specific timing delays
 * This version may not reproduce as reliably but has no dependencies
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <rdma/fabric.h>
#include <rdma/fi_domain.h>
#include <rdma/fi_endpoint.h>
#include <rdma/fi_tagged.h>
#include <rdma/fi_cm.h>
#include <pthread.h>
#include <unistd.h>

#define TRANSFER_SIZE 512
#define MAX_ITERATIONS 10000
#define TAG_BASE 1000

/* Minimal version - see repro_bug.c for full implementation */
int main(void) {
    printf("This is a minimal template. Use repro_bug.c for the actual test.\n");
    printf("The full version includes:\n");
    printf("- 3-thread architecture matching the bug scenario\n");
    printf("- Proper endpoint lifecycle management\n");
    printf("- Random endpoint killing during transfers\n");
    printf("- Deterministic seed for reproducibility\n");
    return 0;
}
EOF

# Create build script
cat > "$PKGDIR/build.sh" << 'EOF'
#!/bin/bash
# Quick build script

# Try common locations
if [ -d "/opt/amazon/efa" ]; then
    echo "Found AWS EFA installation"
    make LIBFABRIC_PREFIX=/opt/amazon/efa
elif pkg-config --exists libfabric; then
    echo "Found libfabric via pkg-config"
    make
else
    echo "Please specify libfabric location:"
    echo "  LIBFABRIC_PREFIX=/path/to/libfabric ./build.sh"
    exit 1
fi
EOF
chmod +x "$PKGDIR/build.sh"

# Create tarball
cd "$TMPDIR"
tar czf "$PKGNAME.tar.gz" "$PKGNAME"
cd - > /dev/null

# Move to current directory
mv "$TMPDIR/$PKGNAME.tar.gz" .

# Cleanup
rm -rf "$TMPDIR"

echo "Created: $PKGNAME.tar.gz"
echo ""
echo "To share with AWS:"
echo "  1. Send them $PKGNAME.tar.gz"
echo "  2. They can extract: tar xzf $PKGNAME.tar.gz"
echo "  3. Build: cd $PKGNAME && ./build.sh"
echo "  4. Run: ./build_portable/repro_bug"
