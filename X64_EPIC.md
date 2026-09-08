# Sequential x64 portability epic

Base: `268af275` (renderer audit already committed). Database and Radiant are excluded. Each folder is audited in order, validated, and checkpointed separately. Existing untracked documents and tools are preserved.

Coding conventions: C-style implementation, explicit `sizeof(Type)`, braces for added/changed conditionals. Fixed-width file/network/GPU formats stay fixed-width; native pointers and runtime records use native widths. Independent correctness patches are exported outside the repository.

| Part | Folder | Status |
| --- | --- | --- |
| 1 | groupvoice | Complete |
| 2 | physics | Pending |
| 3 | qcommon | Pending |
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
