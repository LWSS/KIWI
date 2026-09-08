# Sequential x64 portability epic

Base: `268af275` (renderer audit already committed). Database and Radiant are excluded. Each folder is audited in order, validated, and checkpointed separately. Existing untracked documents and tools are preserved.

Coding conventions: C-style implementation, explicit `sizeof(Type)`, braces for added/changed conditionals. Fixed-width file/network/GPU formats stay fixed-width; native pointers and runtime records use native widths. Independent correctness patches are exported outside the repository.

| Part | Folder | Status |
| --- | --- | --- |
| 1 | groupvoice | Complete |
| 2 | physics | Complete |
| 3 | qcommon | Complete |
| 4 | ragdoll | Pending |
| 5 | script | Pending |
| 6 | server | Pending |
| 7 | server_mp | Pending |
| 8 | sound | Pending |
| 9 | stringed | Pending |
| 10 | ui | Pending |
| 11 | ui_mp | Pending |
| 12 | universal | Pending |
| 13 | win32 | Pending |
| 14 | xanim | Pending |

Validation distinguishes diagnostic compilation and isolated regression tests from a full native game link/run. Shared x86 layout assertions may require a test-only override until their owning part is ported. Production assertions are never globally disabled.

## Part 1: groupvoice

Audited the DirectSound wrappers and bundled Speex sources, including conditional ARM/SSE headers, native allocations, scratch storage, codec interfaces, and fixed-width bitstream representations. DirectSound descriptors use native type sizes; sample assertions cover both pointer widths. Speex scratch alignment uses `uintptr_t`, and MSVC uses the existing per-call alloca implementation instead of the undersized fixed arena.

Additional fixes: bound the 65-entry sample pool in release; avoid releasing a null buffer after creation failure; format HRESULT as a number; decode exactly the negotiated sample count with the codec's saturating integer conversion and return bytes; do not convert uninitialized float samples after codec failure.

Validation: 78 x86/x64 MP syntax checks, native layout assertions, and real Speex roundtrips for all three bandwidth modes. AddressSanitizer executables on both architectures passed 60 encoded/decoded frames each, decode-boundary canaries, pool exhaustion, and scratch alignment. The initial test found a heap overflow in wideband codebook search beyond the fixed 4,000-byte arena; the per-call scratch path resolves it. DirectSound device playback/capture was not exercised. Reproduce with `scripts/x64_audit/compile_part.py groupvoice --modes MP` and `scripts/x64_audit/test_groupvoice.py`; set `KIWI_TEST_ASAN=1` for sanitizer coverage. Logs: `%TEMP%/kiwi-x64-epic/groupvoice`.

## Part 2: physics

Scope: 66 source/header files including the bundled ODE tree. Ported native user-data pool stride, brush geometry union accesses, custom geometry storage size/alignment, contact stride validation, scratch-stack pointer alignment, preset offsets/string stores, and three callback contexts. Replaced negative indexing from the space array with named world-state fields. Removed an unused pointer packed into float scratch storage and corrected an integer debug count passed as a pointer.

SP world-state serialization now writes the original 204-byte record field by field, retaining the registered callback in memory and ignoring the legacy saved-address slot. The total world/gravity record remains 624 bytes across architectures. Preset QBOOLEAN storage is a full integer, as required by the parser.

Validation: all 29 configured translation units passed x86/x64 MP/SP assertion-enabled diagnostic compilation (116 checks), with no physics pointer-truncation warnings. Native regression executables passed on both architectures: record and geometry-storage layouts, 160-slot joint pool exhaustion/reuse, brush union values, all callback dispatch paths, preset string/offset handling, archive boundaries and callback preservation, and scratch frame alignment. Reproduce with `compile_part.py physics --configured` and `test_physics.py` in `scripts/x64_audit`. No full dynamics/game run was performed.

The game CMake lists exclude 16 legacy ODE translation units (recorded in `%TEMP%/kiwi-x64-epic/physics/excluded.json`). An initial all-files probe found their pre-existing missing declarations, disabled OPCODE dependencies, and an unprocessed stack template on both x86 and x64. They were inspected for portability patterns but were not enabled or rebuilt as an alternative ODE library. ODE's configured runtime already uses typed body/joint pointers and native allocation sizes. The shared pool allocator is revisited in the universal part.

Part 1 checkpoint: `9db19a78`.

Part 2 checkpoint: `70b3b817`.

## Part 3: qcommon

Audited all 54 source/header files. Native collision allocations and leaf-node strides now follow their types; the leaf-node sentinel has real, zeroed storage. Primary lights, profile strings, Huffman pointer sorting, SP network-field traversal, memory tracking sizes, thread handles, and setjmp storage no longer depend on x86 pointer layout. SpawnVar has architecture-specific layout assertions. Fixed-width BSP and network records retain their existing widths. Disabled libwww download code remains disabled and requires a separate implementation if restored.

Corrected MSG_ReadInt64 to return and read all eight bytes, including overflow handling. Its independent upstream patch is exported outside the repository.

Validation: 118 configured x86/x64 MP/SP diagnostic compilation checks passed. Production-code regression executables passed on both architectures for collision allocation canaries/native node strides, 64-bit message reads and truncation, Huffman symbol roundtrips, all 143 SP player-state fields, and HUD serialization. Huffman and SP message output was byte-identical across architectures. Reproduce with `scripts/x64_audit/test_qcommon.py` and `compile_part.py qcommon --configured`. Full game/network integration remains pending the later parts. The compiler runner now respects each mode's CMake file list.
