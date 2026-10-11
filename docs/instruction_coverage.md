# Actual fetched-byte and branch-decision coverage

The optional `coverage.detail.start`, `stop`, `reset`, `status` and `read`
methods aggregate exact logical PC, fetched bytes, M1 provenance, completion PC
and handler decision tuples. The old PC bitmap is independent. All detailed
operations except status require an existing owner-thread pause. No method
implicitly runs, pauses or repairs the guest.

Start defaults to `reset: true` and capacity 131072. Capacity must be a power
of two from 256 to 1048576; resizing requires reset. Read accepts insertion
`offset` and `limit` (1–1024, default 256). Counts and dispatch ordinals are
signed-64-bit representable (maximum `counter_limit`, 2^63-1). Checked
accounting saturates and sets sticky `counter_overflow` on exhaustion;
`complete` stays false and additions stop until reset. `clock_cycles` is Fuse's 64-bit absolute physical guest cycle count.

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

Schema 2 / `z80-dispatch-fetched-v2` adds host semantic-shortcut provenance.
Flag 8 and `semantic_kinds` are part of the exact tuple key. Bits 1, 2, 4,
and 8 respectively identify tape save, tape load, SLT bulk load, and loader
acceleration; bits 16 and 32 are reserved for the shared MZ CMT file/body
paths. Repeated hooks in one unit count once per kind. A unit can have several
kinds. Hooks outside a pending dispatch do nothing.

`complete` means no dispatch evidence was lost. Known shortcut rows remain
complete even when a host shortcut skipped the normal conditional predicate
(`outcome:-1`); if that predicate really ran after a partial-error shortcut,
its actual 0/1 is retained. `semantic_override_units` and named
`semantic_kind_units` count marked units, including those dropped for another
reason. `contains_semantic_overrides` reports any marked unit;
`native_semantics_only` requires complete capture and zero marked units.
Exclude every semantic row from native instruction/branch targets. These
fields do not certify hardware accuracy or guest boot provenance.

Capacity exhaustion, nonsequential/overlong fetches and missing ordinary
decisions make capture incomplete. Unknown opcode substitution is dropped
with `opcode_override_failures`, distinct from a memory-fetch failure. Known
tuples still count after capacity exhaustion. Schema-1 archives remain
unchanged; they cannot certify the absence of host semantic shortcuts.

`physical_mapping_certified: false` is intentional: this version identifies
logical addresses and byte versions, not physical bank identities. Coverage
begins when requested, not automatically at the first native bootstrap
instruction. Complete capture does not prove a reachable-code denominator,
all branches reached, campaign completion, audio parity or performance.

This is an isolated experimental feature. Runtime noninterference and authentic
campaign qualification must pass before selecting it for accepted regressions.

Portable table controls are in `tests/instruction-coverage/table.c`:

```sh
gcc -std=c11 -Wall -Wextra -Werror tests/instruction-coverage/table.c -o table-test
./table-test
g++ -std=c++11 -Wall -Wextra -Werror -x c++ tests/instruction-coverage/table.c -o table-cpp-test
./table-cpp-test
```

These synthetic controls cover exact kind-key separation, combined/repeated
hooks, skipped and resumed predicates, outside-dispatch no-op hooks, loss
reasons, capacity, and counter overflow/reset. They do not replace runtime
noninterference tests or authentic campaign evidence.

Additional asset-free controls, run against a configured headless build:

```sh
python3 tests/instruction-coverage/host_paths.py --compiled /path/to/fuse-build --prefix /path/to/libspectrum-prefix --compiler /usr/bin/gcc --output /tmp/fuse-host-controls
python3 tests/instruction-coverage/runtime.py --executable /path/to/fuse-build/.libs/fuse --output /tmp/fuse-tape-controls
```

The runtime test uses six ordinary/disabled/success/error tape cases and
compares enabled/disabled CPU state, RAM, independent PC counts and physical
clocks. It generates its own tiny TAP payload. No original game assets are
needed. Use the packaged runtime with its DLLs/ROM directory on Windows.

The host-control recipe extracts verbatim SLT, loader acceleration and tape
load functions/constants from this checkout. Observable synthetic dependencies
exercise all five non-NONE acceleration modes, SLT enabled/disabled/empty,
and tape-load prechecks/playback/success/both failures after PC mutation for
48K and Timex. It records source hashes. The injected tape errors certify
hook placement and retained decisions; they do not certify real tape-device
behavior or hardware timing. Outside-dispatch calls cannot fabricate units.
Output directories must be new; preserve failed results.
