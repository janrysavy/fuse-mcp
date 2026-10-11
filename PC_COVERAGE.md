# Passive logical-PC coverage over JSON-RPC

The existing localhost JSON-RPC endpoint exposes `coverage.start`,
`coverage.stop`, `coverage.reset`, `coverage.status` and `coverage.read`.
The map is an 8,192-byte host array; no guest RAM is consumed.

`coverage.start` accepts optional `mode: "pc"` and boolean `reset` (default
true). Stop retains the map; reset clears it while preserving enabled state.
Read returns `bitmap_base64`. All commands return `mode: "pc"`,
`format: "pc-bitset-lsb0"`, `map_bytes: 8192`, `enabled`, `unique_pcs`,
`dispatch_units`, `clock_cycles` and `clock_bits: 64`. Bit `(address & 7)` of
byte `(address >> 3)` marks an executed logical dispatch address. Clock is the
existing real frame/cycle observation, never a substituted test clock.

All operations except status require an already paused emulator. Socket threads
only enqueue requests; Fuse's main thread reads or changes the map. The hook
records an actual opcode dispatch, rather than a breakpoint's unexecuted PC.
Prefix operands consumed inside the instruction and HALT refresh cycles are
excluded. Ignored prefixes separately dispatched by the existing CPU retain
that behavior. Repeated block instructions reuse their logical address.

Coverage is disabled by default and independent of bounded tracing/profiling.
It performs no extra guest reads, register writes, clock changes or cycle costs.
Reset/load retains the map; use explicit reset for a new interval. Banks and
self-modified instruction versions share a bit. Address coverage proves neither
both branch outcomes nor every gameplay feature. The same command names and
bitmap encoding are implemented in the MZ-800 fork.
