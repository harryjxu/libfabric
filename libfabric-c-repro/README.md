# libfabric-c-repro

C reproduction of libfabric `ofi_buf_is_valid` assertion bug.

## Build & Run

```bash
# Build
./build.sh all

# Run bug reproduction
./run.sh repro_bug
```

## Expected Output

Triggers assertion at iteration ~104 (12 seconds):

```
[INFO] [Iter 104 @ 12.2s] Scenario: kill_sender, delay: 16ms
[INFO] 🔪 KILLING endpoint 0
repro_bug: ./include/ofi_mem.h:460: ofi_buf_free: Assertion `ofi_buf_is_valid(buf)' failed.
Aborted (core dumped)
```

## Build Options

If auto-detection fails, specify libfabric location:

```bash
# AWS EFA
./build.sh --libfabric-prefix /opt/amazon/efa all

# Custom build
./build.sh -l /path/to/libfabric.so -i /path/to/include all
```