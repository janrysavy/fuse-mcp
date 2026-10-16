# Repeated block-transfer and compare flags

LDIR, LDDR, CPIR and CPDR expose undocumented X/Y flags from the instruction
address during their repeating21T iteration: F5=PC.13 and F3=PC.11. The normal
terminal16T iteration keeps its ordinary A/data-derived flag semantics. This
intermediate state is visible to an IRQ or a debugger/SMC observation even when
the completed transfer later leaves the usual final flags.

The correction changes only F3/F5 and the corresponding Q result on the repeat
path. It retains PC/BC/HL/DE/MEMPTR progression, memory effects, documented flags,
refresh increments, contention and guest instruction clocks. INIR/INDR/OTIR/OTDR
have additional flag rules and are outside this focused change.

Primary hardware research:
https://github.com/hoglet67/Z80Decoder/wiki/Undocumented-Flags#ldxr--cpxr-interrupted

Run `tests/block_repeat_flags.py --executable PATH --output FRESH_DIRECTORY`.
The test constructs an initial48K snapshot; all register/data setup runs as
guest code. RPC sets breakpoints, steps and reads only. It checks repeating and
terminal iterations for all four instructions, all four independent PC X/Y
patterns, source/accumulator patterns, initial preserved flags, compare-match
termination and an instruction crossingFFFF. Exact21T/16T clocks, data, Q,
refresh, pointers and MEMPTR are checked. This is a component proof, not a claim
of exhaustive chip-variant, interrupt, SMC or emulator fidelity qualification.
