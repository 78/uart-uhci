# Changelog

## 0.4.1 — 2026-10-15

- Support the replacement GDMA channel alignment API in ESP-IDF maintenance branches, including release/v6.1, while preserving compatibility with the legacy API.
- Detect the available GDMA declaration during configuration instead of relying on the SDK version number.
- Preserve internal and encrypted/ECC external-memory allocation constraints and propagate alignment-query errors during initialization.

## 0.4.0 — 2026-09-18

- Track consumer ownership separately from DMA completion to prevent duplicate RX delivery; preserve leases across StopReceive and reject restart until all buffers are returned.
- Add DeferReturnBuffer/ReclaimDeferredBuffers for queue-full ISR recovery without another queue entry.
- Remove dynamic allocation and formatted logging from overflow recovery under the RX lock.
- Give FIFO Transmit a total timeout (default 1000 ms) and optional atomic cancellation flag, with balanced PM locks on every exit.
- Add host regression tests for RX ownership, overflow and FIFO timeout/cancellation.
