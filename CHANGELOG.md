# Changelog

## Unreleased

- Add compatibility with the ESP-IDF 6.2 GDMA channel alignment API while retaining support for the legacy API used by ESP-IDF 5.5 through 6.1.
- Propagate errors returned while querying RX DMA alignment constraints.

## 0.4.0 — 2026-09-18

- Track consumer ownership separately from DMA completion to prevent duplicate RX delivery; preserve leases across StopReceive and reject restart until all buffers are returned.
- Add DeferReturnBuffer/ReclaimDeferredBuffers for queue-full ISR recovery without another queue entry.
- Remove dynamic allocation and formatted logging from overflow recovery under the RX lock.
- Give FIFO Transmit a total timeout (default 1000 ms) and optional atomic cancellation flag, with balanced PM locks on every exit.
- Add host regression tests for RX ownership, overflow and FIFO timeout/cancellation.
