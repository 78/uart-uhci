# RX ownership regression

Run from the component root:

```sh
python3 tests/run_host_tests.py
```

Requires Python 3 and `clang++` (or `CXX` pointing to a C++20 compiler) with
AddressSanitizer and UndefinedBehaviorSanitizer. No third-party Python packages.
The runner reads the actual public header and RX method bodies, substitutes only
ESP-IDF/GDMA hardware primitives, and compiles a temporary host binary. Production
algorithms are not copied into a second test implementation.

Eight scenarios cover ring wraparound without returns (one callback per buffer),
duplicate return before another delivery, restart refusal while a buffer is held,
deferred reclamation/overflow recovery, immediate callback return, late descriptor
error, invalid descriptor lengths, concurrent scan/return, deferred return after
stop, and no registered callback (some scenarios cover multiple checks).

This is not an interrupt timing or DMA/cache-coherency test. On hardware, stress
slow consumers and repeated StopReceive/StartReceive, and check that the pending
RX leases drain before Deinit. The current driver allocates a non-cache-safe GDMA
interrupt; flash-resident gdma_link helpers must not run with the cache disabled.
