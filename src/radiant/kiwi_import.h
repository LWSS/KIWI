#pragma once
#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// ═════════════════════════════════════════════════════════════════════════════════════
//  kiwi_import.h — KIWI-UX (ROUND BE): DRAG-AND-DROP TEXTURE IMPORT + THE MATERIAL WIZARD.
// ═════════════════════════════════════════════════════════════════════════════════════
// USER DIRECTIVE, verbatim: *"A texture import system that allows drag and drop into the
// textures window. It should handle all intermediate CoD4 material properties with popup
// dialogs and save the material next to all the other ones in the library for future use."*
//
// This file owns the ORCHESTRATION and the UI.  The two things it orchestrates are pure
// I/O and live in their own files, each with its own derivation from the engine's readers:
//   kiwi_iwi.h        the .iwi writer      (D-BE-A..D)
//   kiwi_matwriter.h  the material writer  (D-BE-E..I)
// Read those two headers for the file formats; read this one for the flow.
//
// ── D-BE-J: THE DROP IS A WIN32 DROP, NOT AN ImGui ONE ─────────────────────────────
// ImGui's drag-drop is an INTERNAL payload system (kiwi_entbrowser.cpp:363 drags an eclass
// name from one ImGui item to another); it never sees a file dragged in from Explorer.
// The only mechanism that does is `WM_DROPFILES`, which requires `DragAcceptFiles` on a
// real HWND.  There was none anywhere in src/ before this round.  So:
//   * `DragAcceptFiles(frame, TRUE)` once, at the end of Radiant_BootFrame.
//   * a `WM_DROPFILES` case in Radiant_FrameWndProc (radiant_main.cpp) that does the
//     DragQueryFileA enumeration + DragFinish IMMEDIATELY (an HDROP is only valid until
//     DragFinish returns, so the queue cannot be filled lazily) and then POSTS
//     KIWI_CMD_IMPORT_DROPPED.
// The POST is the same discipline round AU established for the entity drop
// (kiwi_entbrowser.cpp:946-949): real work never runs inside a message handler, because a
// handler can fire from inside the compositing scene bracket or from a nested modal pump,
// and opening a wizard from there would re-enter ImGui mid-frame.
//
// ── D-BE-K: DROP TARGET SCOPE IN v1 ────────────────────────────────────────────────
// WM_DROPFILES is delivered to the window under the cursor that registered for it — here,
// the frame.  The Textures dock window is an ImGui window drawn INTO the frame's client
// area, not an HWND of its own, so "was the drop over the texture browser" is a question
// only the next ImGui frame could answer, and the answer would arrive after the HDROP is
// already gone.  v1 therefore accepts a drop ANYWHERE on the editor window and always
// means "import these as textures".  Nothing else in the editor consumes a file drop, so
// there is no ambiguity to resolve — only a highlight to add later.
//
// ── D-BE-L: THE QUEUE, AND "APPLY TO THE REST" ─────────────────────────────────────
// A drop of N files becomes N wizard passes over ONE queue.  The name is always re-derived
// per file (lowercase, spaces to underscores, everything outside [a-z0-9_-] dropped), because
// two files can never share a material name.  Everything else — type, usage, locale, surface
// type, compression, tiling — is remembered when "apply these settings to the remaining
// files" is ticked, and that checkbox turns the rest of the queue into a silent batch.
//
// ── D-BE-M: NOTHING IS "IMPORTED" UNTIL THE ENGINE HAS READ IT BACK ────────────────
// The OK path is: write the .iwi -> KiwiIwi_VerifyOnDisk (the engine's own
// Image_ValidateHeader + Image_CountMipmapsForFile + the two size invariants) -> write the
// material -> KiwiMat_Verify (Material_RegisterHandle, which drags the WHOLE loader chain
// down to Image_LoadFromFileWithReader over our .iwi, then the Load_Materials browser gate)
// -> register into the browser.  ANY failure deletes both files and leaves the browser
// untouched: a half-written pair in the library is worse than no import, because the next
// Load_Materials would register a broken material at startup.
// ═════════════════════════════════════════════════════════════════════════════════════

// The WM_DROPFILES handler.  `hDropOpaque` is the HDROP wParam.  Enumerates and DragFinishes
// it immediately, queues every accepted file, prints a refusal line naming any it rejected,
// and returns true when at least one file was queued (i.e. the caller should post
// KIWI_CMD_IMPORT_DROPPED).  Safe to call from inside a message handler — it touches no
// ImGui state and does no I/O beyond the path enumeration.
bool KiwiImport_HandleDropFiles( void *hDropOpaque );

// The instant-command arm: KIWI_CMD_IMPORT_DROPPED (open the wizard on the queue) and
// KIWI_CMD_IMPORT_BROWSE (GetOpenFileName multi-select -> the same queue -> the same wizard).
bool KiwiImport_DispatchInstant( unsigned int cmdId );

// Registers KIWI_CMD_IMPORT_BROWSE.  KIWI_CMD_IMPORT_DROPPED is deliberately NOT registered:
// it is an internal continuation of a finished gesture, exactly like KIWI_CMD_ENT_DROP.
void KiwiImport_RegisterCommands();

// The wizard.  Called once per ImGui frame at top-level window scope (imgui_shell.cpp,
// beside the other self-drawing panels).  Draws nothing when the queue is empty.
void KiwiImport_Draw();
