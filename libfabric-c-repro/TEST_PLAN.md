# Ping-Pong Test Plan

## Overview
This document outlines the testing procedure for the libfabric C ping-pong implementation that matches fabulous's threading model.

## Pre-Test Setup

1. Enter neuron environment:
   ```bash
   use-neuron
   ```

2. Build the project:
   ```bash
   cd /root/code/sandbox/sandbox/harry/libfabric-c-repro
   ./build.sh all
   ```

3. Verify build:
   ```bash
   ls -la build/
   # Should see: efa_discover, endpoint_test, pingpong, completion_thread.o
   ```

## Test 1: Basic Functionality

### 1.1 Thread Verification
```bash
# In terminal 1:
./run.sh pingpong

# In terminal 2:
ps -eLf | grep pingpong
# Expected: 3 threads (main + 2 workers)
```

### 1.2 Debug Mode
```bash
DEBUG=1 ./run.sh pingpong 2>&1 | tee debug.log
# Check for:
# - "Completion thread started for domain" (2 times)
# - "Created endpoint" messages
# - Address insertion logs
```

## Test 2: Performance Comparison

### 2.1 Run Fabulous Python Version
```bash
cd /root/code/sandbox/sandbox/harry
python fabulous_ping_pong_many.py 2>&1 | tee fabulous_perf.log
# Note the transfer times and throughput
```

### 2.2 Run C Version
```bash
cd /root/code/sandbox/sandbox/harry/libfabric-c-repro
./run.sh pingpong 2>&1 | tee c_perf.log
# Compare transfer times
```

### 2.3 Performance Red Flags
- ✅ GOOD: ~1.5 Gbps throughput
- ❌ BAD: ~50 Mbps throughput (indicates slow mode bug)
- ❌ BAD: Transfer takes > 5 seconds for 400MB

## Test 3: Stress Testing

### 3.1 Endpoint Bind Delay Test
```bash
# Test timing sensitivity
FABULOUS_EP_BIND_DELAY_MS=50 ./run.sh pingpong
FABULOUS_EP_BIND_DELAY_MS=100 ./run.sh pingpong
FABULOUS_EP_BIND_DELAY_MS=200 ./run.sh pingpong
```

### 3.2 Multiple Runs
```bash
# Run 10 times to check consistency
for i in {1..10}; do
    echo "=== Run $i ==="
    ./run.sh pingpong || break
done
```

## Test 4: Debugging

### 4.1 LibFabric Debug Output
```bash
FI_LOG_LEVEL=debug ./run.sh pingpong 2>&1 | tee fi_debug.log
# Look for EFA provider messages
```

### 4.2 System Call Tracing
```bash
strace -f -e trace=network,read,write ./build/pingpong 2>&1 | tee strace.log
# Check for:
# - Socket operations for thread communication
# - No unexpected network calls
```

### 4.3 GDB Analysis (if hung)
```bash
gdb ./build/pingpong
(gdb) run
# If it hangs, press Ctrl+C
(gdb) info threads
(gdb) thread apply all bt
# Look for which thread/function is stuck
```

## Test 5: Data Integrity

### 5.1 Verify Output
The program should print:
- "✓ Data verified after warmup"
- "✓ Data verified after actual transfer"
- "✅ SUCCESS: Ping-pong test completed successfully!"

### 5.2 Check for Errors
Look for any of these error messages:
- "Data verification failed"
- "Failed to create endpoint"
- "fi_av_insert failed"
- "Transfer timed out"

## Expected Results Summary

| Metric | Expected | Red Flag |
|--------|----------|----------|
| Thread Count | 3 | != 3 |
| Throughput | ~1.5 Gbps | < 100 Mbps |
| Transfer Time (400MB) | ~2-3 seconds | > 10 seconds |
| Data Verification | Pass | Any failure |
| Program Exit | Clean (0) | Crash/hang |

## Known Limitations

1. **Device Selection**: Currently doesn't actually select specific devices 11 & 3
2. **Memory Type**: Uses CPU memory instead of HBM
3. **NUMA Affinity**: Not implemented yet

## Next Steps After Testing

1. If all tests pass:
   - Proceed to implement `repro_bug.c` with endpoint killing
   - Add random timing to reproduce the hang

2. If performance is slow:
   - Investigate timing differences with fabulous
   - Check if receiver posting before sender affects performance
   - Add delays between operations to test timing sensitivity

3. If crashes/hangs occur:
   - Use GDB to identify exact location
   - Check thread synchronization
   - Verify libfabric call sequences

## Comparison Checklist

- [ ] Thread count matches fabulous (3 threads)
- [ ] Endpoint creation in worker threads
- [ ] Address caching works correctly
- [ ] Performance within 20% of fabulous
- [ ] No memory leaks (use valgrind if needed)
- [ ] Clean shutdown without hangs