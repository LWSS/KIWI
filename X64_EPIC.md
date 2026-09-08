# Sequential x64 portability epic

Base: `268af275` (renderer audit already committed). Database and Radiant are excluded. Each folder is audited in order, validated, and checkpointed separately. Existing untracked documents and tools are preserved.

Coding conventions: C-style implementation, explicit `sizeof(Type)`, braces for added/changed conditionals. Fixed-width file/network/GPU formats stay fixed-width; native pointers and runtime records use native widths. Independent correctness patches are exported outside the repository.

| Part | Folder | Status |
| --- | --- | --- |
| 1 | groupvoice | Complete |
| 2 | physics | Complete |
| 3 | qcommon | Complete |
| 4 | ragdoll | Complete |
| 5 | script | Complete |
| 6 | server | Complete |
| 7 | server_mp | Complete |
| 8 | sound | Complete |
| 9 | stringed | Complete |
| 10 | ui | Complete |
| 11 | ui_mp | Pending |
| 12 | universal | Pending |
| 13 | win32 | Pending |
| 14 | xanim | Pending |

Validation distinguishes diagnostic compilation and isolated regression tests from a full native game link/run. Shared x86 layout assertions may require a test-only override until their owning part is ported. Production assertions are never globally disabled.

Part 5 checkpoint: `510631ac`.
Part 6 checkpoint: `91eb549f`.
Part 7 checkpoint: `ec0f97bc`.
Part 8 checkpoint: `1883deb0`.
Part 9 checkpoint: `18c58233`.

## Part 10: ui

Audited all 13 files. Runtime operands and local-variable unions preserve native string pointers; local-variable lookup returns the actual table entry. Parser tokens, macros, scripts, pointer tables, expressions, menus, widgets, debugger windows and watch elements use native allocation/copy sizes and alignment. Parser allocation headers maintain eight-byte alignment on both architectures. Replaced debugger frame/AST offset tricks, enum-dvar pointer arithmetic, scroll-state aliasing and conversion-argument pointer strides with typed fields.

The parser evaluator now accesses its actual value stack rather than indexing past the operator array. Full value copies preserve strings; ternary evaluation frees the unselected string at the correct index. Invalid expressions, division by zero and exhausted evaluator storage stop in release. Conversion substitution respects destination capacity and valid argument indexes; its integer-list helper has a typed array interface. Parser varargs are closed and output terminated. Independent fixes are exported outside the repository in `10-ui-correctness.patch`.

Validation: all 36 configured x86/x64 MP/SP diagnostic checks pass. `test_ui.py` runs production functions on both architectures and passes aligned token/macro chain copies, native expression strings, a full 60-entry operand stack, all 256 local-variable slots plus overflow rejection, bounded substitutions, mixed integer/double precedence, string concatenation and both ternary branches, and invalid-expression handling. No complete menu load, debugger interaction or rendered UI session was run.

## Part 9: stringed

Audited both source files and both headers, including the existing native STL storage. Line parsing now uses pointers/size_t lengths and an explicit destination capacity; language tracking uses its native array size. Removed pointer-to-int casts from newline handling.

Independent parser fixes: preserve the last character of LF-only lines; reject oversized source lines and localized tokens/results before copying; use overlap-safe token copying; reject &&0 before it indexes before the format-argument array; handle whitespace-only quoted text without indexing an empty string; reject missing insertion placeholders; bound invalid language selections. The caller passes the actual line/token buffer capacity.

Validation: eight configured x86/x64 MP/SP diagnostic checks pass. `test_stringed.py` runs actual inline package methods and localization functions on both architectures, covering LF/CRLF/final lines, overflow canaries, empty quotes, valid/invalid/duplicate placeholders, insertion, token/result bounds, and invalid language fallback. No full language-asset/game load was performed.

## Part 8: sound

Audited all seven files. Native loaded-sound allocation and sound/alias/curve layout assertions now cover both architectures. Miles file-open callbacks initialize the entire native handle; allocator dispatch preserves native return pointers. Removed a pointer cast into unused integer scratch. Playback lookup uses typed channel traversal and a real entity handle, with the cgame caller updated. Script notification tokens remain four bytes on disk and are explicitly widened on restore; subtitle alias pointers remain native. Native sound tracking and debug dvar string access were corrected.

Independent fixes include bounds for channel-name/count parsing and length notifications, restore-segment size validation, and a malformed EQ assertion. Replaced the fade setup's integer/counter packing into one int64 with separate variables; the old loop changed the denominator while iterating. Deferred sound restore copies into its actual buffer. All new/changed save fields retain their existing fixed-width layout.

Validation: 20 sound and six shared cgame call-site diagnostic checks pass. `test_sound.py` links the supplied x86/x64 Miles libraries and runs their actual WAV parser on PCM data. Both native runs pass handle-width, allocator-return, loaded-sound canary/layout, 53-channel lookup, fade-rate, four-entry/24-byte notification save/restore, and restore-buffer boundary tests. Audio-device playback was not performed.

CMake selects the matching supplied Miles import library/runtime directory by pointer width. Full x64 game configuration/linking still needs the global `/machine:x86`, DirectX/Steam selection and other pending owners addressed; this is not a claim that the full game builds yet.

## Part 7: server_mp

Audited all 12 files. Replaced fixed client allocation/copy/clear lengths and server clearing/tracking sizes with native sizes. Dvar strings use the string union member. Snapshot information is fully initialized; baseline traversal uses native entity-array stride, archive frame sizes use named fields, and player-state accesses use named fields. Removed bogus offset-derived pointer assertions. Netfield offsets use `offsetof`.

Independent fixes: stats now address their actual storage (2000 bytes followed by 1498 integers), matching `LiveStorage_GetStat`; range checks reject invalid client/stat indexes. The prior accesses depended on packed voice-packet layout and could corrupt voice storage even on x86. Master-server port detection uses `strchr` with the correct argument order. Formatted commands close their va_list and terminate their output. Voice reads reject truncated messages, and queue writes reject invalid payload sizes and exhausted capacity.

Validation: 22 configured x86/x64 MP diagnostic compilation checks pass. Native regression tests (`test_server_mp.py`) pass on both architectures: all 3498 stat values and unchanged-value suppression, voice-storage canaries, client resizing with pointer/tail preservation and full clearing, allocation canaries, queue capacity/invalid lengths, and static verification of the replaced player-state offsets. No multiplayer match or network integration run was performed. External correctness patch: `07-server-mp-correctness.patch`.

## Part 6: server

Audited all 13 files. Fixed native `jmp_buf` use, skeleton pointer alignment, all three command-argument pointer strides, the gametype dvar string, full server-static clearing, configstring array traversal, native tracking sizes, and snapshot indexing through the actual entity array. Added a release snapshot-capacity return and terminated formatted server commands after `va_end`.

Demo history uses an explicit 172-byte record with zeroed legacy pointer slots, preserving its x86 cache format on x64. Reads never restore saved pointers. Replaced hard-coded server offsets with message fields; an x86 layout probe confirmed offsets 60052/60060 correspond to `cursize`/`readcount`. History allocation checks avoid signed addition overflow, and segment totals are bounded before narrowing. Native buffer membership uses integer address ranges rather than forming out-of-array pointers. Removed an unused integer-to-FILE-pointer return from cache clearing.

Validation: all 20 configured x86/x64 MP/SP diagnostic checks pass. `test_server.py` passes on both architectures with assertions enabled: high-address buffer membership, negative/oversized allocations, exact capacity and exhaustion, 199-byte history header/payload roundtrip, native destination pointers, and all 2048 snapshot entries plus overflow rejection. No live server/demo playback was run.

Integration follow-up for universal: `MemFile_CopySegments` still returns a count disguised as a pointer and truncates an address internally; reconcile its interface and implementation in that part. `SaveImmediate.f` was traced to an integer FS handle encoded by `savedevice_pc.cpp`, so the existing intptr_t bridge does not truncate a native FILE pointer. The XModel allocator callback signature is shared with server_mp/xanim and will be reconciled with its owner.

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

Part 3 checkpoint: `652ab0aa`.

## Part 4: ragdoll

Audited all five files, including definition parsing, native body/joint arrays, controller and state callback traversal, physics handoff, and quaternion routines. Initialization and allocation tracking now cover the complete native arrays. No raw archive implementation exists in this folder. Corrected the second bone endpoint's invalid-index check and the timeout diagnostic's swapped pointer/handle arguments; these are exported separately for upstream.

Validation: all 16 x86/x64 MP/SP diagnostic compilation checks passed. Production-function regression executables passed on both architectures: complete array initialization from poisoned memory, 32-slot exhaustion, all 28 joint pairs and 14 body pointers through destruction callbacks, both orientation buffers, and rejection of an invalid second bone endpoint. Native Joint/Bone/StateEnt layouts were asserted. Reproduce with `scripts/x64_audit/test_ragdoll.py` and `compile_part.py ragdoll --configured`. Physics calls are boundary stubs in these tests; full animated simulation remains an integration check.

Part 4 checkpoint: `00bec97f`.

## Part 5: script

Audited 31 source/header files. Ported native union copies, parse-node storage, builtin addresses, suspended-stack strides/readers/writers, native layout assertions, watch sorting/allocation, vector accesses, save union interfaces, class traversal, compiler case/child storage, source-buffer copies, and reference/debug reporting. The MT allocator keeps its 12-byte node IDs but reserves at least two buckets on x64 to align allocations to eight bytes. The program arena doubles on x64 to accommodate wider operands. Active compiler is scr_compiler2.cpp; scr_compiler.cpp and scr_yacc.cpp are excluded by CMake and remain legacy alternatives requiring reconciliation if restored. Runtime bytecode uses native-width address/count operands where emitted by EmitCodepos/EmitNativeValue; ordinary integer/float operands remain four bytes. Animation and jump readers and the debugger walker now match the active compiler. Saved values retain type-specific scalar/code-offset encoding.

Validation: all 54 configured x86/x64 MP/SP diagnostic checks pass, without script pointer-truncation warnings. Assertion-enabled `test_script.py` executables pass on both architectures for parse-node pointer copies, operand emit/read pairs and debugger traversal, large integer width, builtin registration/calls, native switch-record sorting, MT bucket alignment, vector arithmetic, nested suspended-stack roundtrips/canaries, and production save/load helpers. Saved stack bytes match across architectures. `script_layout.py` reports every declared layout on both architectures; pointer-bearing assertions were reconciled against these results. This does not constitute a full linked game, arbitrary GSC execution, or remote-debugger session. SaveImmediate's legacy integer-file-handle bridge is revisited with server; shared Hunk/Z allocators are revisited with universal.

Independent fixes include preserving the stack timestamp when growing a notified stack, passing the complete entity reference to evaluated builtin methods, and treating script log text as data. The upstream patch is outside the repository.
