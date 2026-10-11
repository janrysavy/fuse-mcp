# Actual fetched-byte and branch-decision coverage

The optional `coverage.detail.start`, `stop`, `reset`, `status` and `read`
methods aggregate exact logical PC, fetched bytes, M1 provenance, completion PC
and handler decision tuples. The old PC bitmap is independent. All detailed
operations except status require an existing owner-thread pause. No method
implicitly runs, pauses or repairs the guest.

Start defaults to `reset: true` and capacity 131072. Capacity must be a power
of two from 256 to 1048576; resizing requires reset. Read accepts insertion
`offset` and `limit` (1–1024, default 256). Counts and dispatch ordinals are
64-bit. `clock_cycles` is Fuse's 64-bit absolute physical guest cycle count.

Hooks wrap the values returned by explicit existing M1 and operand reads.
Data reads through HL, stack, indexed addresses or devices are not classified
as operands just because their address happens to equal PC. All 29 base
conditional and eight ED-repeat predicates return the same actual decision
to their original handler. Completion happens before post-instruction device
events. HALT refresh units are excluded, matching the old PC map convention.

Tuple flags are: 1, ignored DD/FD prefix fragment; 4, redispatch whose first
byte was already fetched by the previous fragment. Flags may combine to 5
for successive ignored prefixes. A fragment has two actual M1-read bytes,
then backtracks one PC; the next unit reuses the second byte without another
guest read. Its M1 bit identifies the original fetch, not a second physical
read. This follows Fuse's unchanged redispatch and PC-map unit conventions.
Nominal guest instruction timing and memory callbacks are unchanged.

The underlying untaken JR/DJNZ operand-read correction is a separately
qualified prerequisite, not a collector side effect. The shared bounded
tuple table is byte-identical to the qualified MZ implementation. Disabled
collection allocates no table. Reset retains enabled state and capacity.

Capacity exhaustion, nonsequential or overlong fetches, missing decisions and
overridden dispatches cannot produce a complete capture. The API exposes
explicit dropped-unit and failure counters. An emulator peripheral overriding
the fetched opcode marks that unit malformed and increments fetch failures;
it is never reported as a faithful fetched instruction. Known tuples still
count after capacity exhaustion.

`physical_mapping_certified: false` is intentional: this version identifies
logical addresses and byte versions, not physical bank identities. Coverage
begins when requested, not automatically at the first native bootstrap
instruction. Complete capture does not prove a reachable-code denominator,
all branches reached, campaign completion, audio parity or performance.

This is an isolated experimental feature. Runtime noninterference and authentic
campaign qualification must pass before selecting it for accepted regressions.
