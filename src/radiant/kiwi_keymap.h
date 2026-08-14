#pragma once
#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// ─────────────────────────────────────────────────────────────────────────────
// kiwi_keymap.h — RADIANT_UX_DESIGN §11: keymap profiles.
//
// §11's ruling, restated: most of the draft's bindings are TAKEN in classic
// Radiant (1-9 = grid size, R = mouse-rotate, S/N/O/F = inspectors, X = clipper,
// V = vertex mode, Esc = deselect).  Rebinding blind would wreck muscle memory
// AND silently shadow ported features, so the migration is a PROFILE, and every
// new feature stays reachable in BOTH profiles through the §15 palette.
//
// ── HOW A PROFILE IS APPLIED ─────────────────────────────────────────────────
// By rewriting vk/mods in the MUTABLE `g_radiantCommands` — the exact field pair
// `LoadCommandMap` patches (mainfrm.cpp: it parses radiant.ini [Commands] and
// assigns `c.vk = vk; c.mods = mods;`).  There is no second binding store.
// Applying is idempotent in both directions:
//
//     Radiant_ResetCommandBindings()   // back to the compiled-in defaults
//     Radiant_LoadCommandMap()         // re-layer the user's radiant.ini
//     <profile patch>                  // modern only
//
// so KEYMAP_CLASSIC is literally "the default table + the user's own remaps",
// bit for bit, and switching back and forth never accumulates drift.  User
// remaps therefore sit UNDER the profile: an explicit modern binding wins.
//
// ── THE MODERN PROFILE, IN FULL ──────────────────────────────────────────────
// (mods: Shift=1, Alt=2, Ctrl=4, LWin=8 — mainfrm.cpp's own mask)
//
//   key        | was (classic)                     | becomes (modern)
//   -----------+-----------------------------------+---------------------------
//   1          | SetGrid1      35022               | Select Mode: Point   34001
//   2          | SetGrid2      35023               | Select Mode: Edge    34002
//   3          | SetGrid4      35024               | Select Mode: Face    34003
//   4          | SetGrid8      35025               | Select Mode: Object  34004
//   5          | SetGrid16     35026               | Select Mode: All     34005
//   F          | ViewFilters   33104               | Command Palette      34010
//   [          | GridDown      33084               | Grid Spacing Halve   34020
//   ]          | GridUp        33083               | Grid Spacing Double  34021
//   G          | VertEdit         33199            | Move    (§13)        34031
//   R          | MouseRotate      32810            | Rotate  (§13)        34032
//   S          | SurfaceInspector 33041            | Scale   (§13)        34033
//   Left       | CameraLeft       33057            | UNBOUND -> modern fly
//   Right      | CameraRight      33058            | UNBOUND -> modern fly
//   Up         | CameraForward    33059            | UNBOUND -> modern fly
//   Down       | CameraBack       33060            | UNBOUND -> modern fly
//   Delete     | ZoomIn (XY)      32995            | Delete Selection     33003
//   Shift+A    | SelectAllOfType  33093            | Construct: Line      34034
//   Shift+S    | PatchInspector   33092            | Construct: Spline    34050
//   Shift+Q    | (free)                            | Construct: Rect      34036
//   Shift+W    | TogglePatchWire  32857            | Box                  34051
//   Shift+Z    | (free)                            | Sphere               34053
//   Shift+X    | ToggleCrosshairs 33100            | Cylinder             34052
//   Shift+C    | CapCurrentCurve  32885            | Construct: Circle    34037
//   Shift+V    | VehicleGroup     33221            | Box (centre)         34064
//   Alt+V      | (free)                            | Construct: Rect(ctr) 34047
//   Ctrl+J     | (free)                            | Join Lines           34104
//   Ctrl+X     | File->Exit 32951 (.rc ACCELERATOR)| Cut (clipboard)      34112
//   C          | CameraDown       33056            | Cut                  34055
//   Z          | CameraAngleDown  33062            | Match Face           34056
//   J          | ToggleOutlineDraw 33103           | Join (context)       34106
//   E          | DragEdges        33006            | Extrude (context)    34058
//   Ctrl+R     | RemoveColorNode  10               | Split Face           34057
//   T          | ViewTextures     33018            | Trim (lines)         34059
//   O          | ViewConsole      33016            | Offset Curve         34060
//   B          | SameTargetname   36121            | Fillet Corners       34061
//   Q          | (free)                            | Boolean (diff/union) 34062
//   Shift+D    | RotateZ          32961            | Duplicate (+ Move)   34107
//   /          | (free)                            | Focus On Selection   34108
//   Ctrl+H     | (free)                            | Invert Hidden        34109
//   Shift+R    | MouseRotate (displaced here)      | Repeat Last Command  34110
//   Shift+H    | ShowHidden       32924            | HideUnSelected       32934
//   Alt+H      | HideUnSelected   32934            | ShowHidden           32924
//
// ── ROUND J: THE LAST PLASTICITY VERBS (O · B · Shift+D · / · Ctrl+H · Shift+R
//    · and the H-family swap) ────────────────────────────────────────────────
// Four of the seven are the keys PLASTICITY itself uses, and the sources are
// cited in each feature's own header:
//   o        command:offset-curve        default-keymap.ts:268
//   shift-d  command:duplicate           default-keymap.ts:280
//   /        viewport:focus              default-keymap.ts:327 · Menu.ts:53
//   shift-r  edit:repeat-last-command    default-keymap.ts:304 · Menu.ts:42
//   h / shift-h / alt-h / ctrl-h        default-keymap.ts:296-299 · Menu.ts:48-51
// B is KIWI's own: Plasticity's `b` is `command:fillet-solid` (a SOLID edge
// fillet KIWI does not have), and its CURVE fillet lives inside modify-contour,
// which has no top-level chord at all — kiwi_fillet.h sets that out in full.
//
//   vk 0x4F O   occupancy: 0 ViewConsole 33016 (mainfrm.cpp:1054) ·
//               1 LinkSelectionToggle 1085 (:1066) · 2 ExtrudeTerrainRow 33192
//               (:1006) · 4 FileOpen 57601 (:1096).  mods 3/5/6/7 free.
//               33016 -> Shift+Alt+O (mods 3), the house destination.
//               Bare O: KIWI_CMD_OFFSET_CURVE 34060.
//               Ctrl+O (FileOpen) is ALSO an accelerator (res/radiant.rc:493) and
//               is deliberately untouched — TranslateAccelerator runs BEFORE the
//               hotkey table, so moving it would have been silently overridden.
//               The console is still one click away: it is the ImGui shell's own
//               Console window (KIWI_CMD_WINDOW_CONSOLE 34018) in the modern
//               layout, plus the Inspector menu, plus the palette.
//
//   vk 0x42 B   occupancy: 0 SameTargetname 36121 (mainfrm.cpp:1080) ·
//               1 FitBrush 33098 (:996) · 4 SameTarget 36123 (:1081).
//               mods 2/3/5/6/7 free.  36121 -> Shift+Alt+B (mods 3).
//               Bare B: KIWI_CMD_FILLET_CURVE 34061.
//               SameTargetname is a SELECT-BY-KEY helper reachable from the
//               Selection menu and the palette; nothing about it is a per-second
//               key the way a fillet is.
//
//   vk 0x44 D   occupancy: 0 CameraUp 33055 (mainfrm.cpp:1034) · 1 RotateZ 32961
//               (:1078) · 5 MakeDetail 33042 (:1088) · 7 DropVertices 33213
//               (:1077).  mods 2/3/4/6 free.  32961 -> Shift+Alt+D (mods 3).
//               Shift+D: KIWI_CMD_DUPLICATE 34107.
//               (kiwi_dupe.cpp's shakeout-5 note claimed 0x44 carried only mods 4;
//               that was WRONG and is corrected in place there.  Rotate Z keeps
//               its menu item, its palette row and its new chord.)
//
//   vk 0xBF /   occupancy: 4 ToggleTurnTerrainEdges 33141 (mainfrm.cpp:1001) and
//               NOTHING ELSE.  mods 0 is FREE — nothing is displaced.
//               "/": KIWI_CMD_FOCUS_SELECTION 34108.
//               "/" is not in res/radiant.rc's accelerator table (:490-501).
//               ONE KNOWN LIMITATION, logged in RADIANT_KNOWN_ISSUES: the ported
//               key-NAME table (g_radiantKeys, mainfrm.cpp:1323-1336, lifted
//               verbatim from the binary) has no entry for 0xBF, and both
//               CommandList_KeyName (a port of sub_40BBC0) and LoadCommandMap's
//               parser read that table.  So the command-list panel prints a
//               garbage glyph for this row and radiant.ini cannot NAME "/" to
//               rebind it.  kiwi_hints.cpp carries a KIWI-side OEM name table so
//               the on-screen chip is right; the ported table is not edited.
//
//   vk 0x48 H   occupancy: 0 HideSelected 32923 (mainfrm.cpp:992) ·
//               1 ShowHidden 32924 (:994) · 2 HideUnSelected 32934 (:993) ·
//               5 ShowLastHidden 33246 (:995) · 7 HideByClassname 32925 (:1171).
//               mods 3/4/6 free.
//               THE MODERN PROFILE SWAPS mods 1 and 2, so the family reads in
//               Plasticity's order (h hide · shift-h isolate · alt-h unhide).
//               NOTHING IS DISPLACED and no command becomes unreachable: the two
//               ids trade chords with each other.
//               Ctrl+H (mods 4) is FREE and takes KIWI_CMD_HIDE_INVERT 34109,
//               which is Plasticity's `command:invert-hidden` and had no Radiant
//               equivalent in any spelling (kiwi_visibility.h).
//
//   vk 0x52 R   AFTER shakeout G the modern profile's R carried 0 Rotate 34032 ·
//               1 MouseRotate 32810 · 3 ToggleTexRotateLock 32835 · 4 SplitFace
//               34057 · 5 RemoveColorNode 10.  Free: 2, 6, 7.
//               Shift+R is claimed for KIWI_CMD_REPEAT_LAST 34110 and MouseRotate
//               moves ONE more step, to Alt+S's neighbour Alt+R (mods 2).
//               THIS IS THE ROUND'S ONE UNCOMFORTABLE TRADE and it is made with
//               eyes open: MouseRotate (32810) is the CLASSIC 2D-view rotate MODE,
//               which the modern profile has already superseded with the real
//               Rotate command on bare R, and Alt+R inherits the Alt-chord
//               limitation already in RADIANT_KNOWN_ISSUES (WM_SYSKEYDOWN is not
//               routed to the hotkey table in the ImGui shell).  It stays on the
//               menu and in the palette, exactly like Surface Inspector on Alt+S.
//               Shift+R is not in res/radiant.rc's accelerator table (:490-501).
//
// FINAL OCCUPANCY RE-CHECK after the round-J patches:
//   O carries 0=OffsetCurve, 1=LinkSelectionToggle, 2=ExtrudeTerrainRow,
//             3=ViewConsole, 4=FileOpen;
//   B carries 0=FilletCorners, 1=FitBrush, 3=SameTargetname, 4=SameTarget;
//   D carries 0=CameraUp, 1=Duplicate, 3=RotateZ, 5=MakeDetail, 7=DropVertices;
//   / carries 0=FocusOnSelection, 4=ToggleTurnTerrainEdges;
//   H carries 0=HideSelected, 1=HideUnSelected, 2=ShowHidden, 4=InvertHidden,
//             5=ShowLastHidden, 7=HideByClassname;
//   R carries 0=Rotate, 1=RepeatLastCommand, 2=MouseRotate,
//             3=ToggleTexRotateLock, 4=SplitFace, 5=RemoveColorNode.
// No (vk, mods) pair carries two rows on any of them.
//
// ── SHAKEOUT H: T (kiwi_trim.h) ─────────────────────────────────────────────
//   vk 0x54 T   occupancy: 0 ViewTextures 33018 (mainfrm.cpp:998) ·
//               1 ToggleTexMoveLock 32785 (:1064) · 5 ThickenPatch 32904 (:999).
//               mods 2/3/4/6/7 free.  33018 -> Shift+Alt+T (mods 3), the house
//               destination.  Bare T: KIWI_CMD_TRIM 34059 — the key Plasticity
//               itself uses bare (default-keymap.ts:267 `"t": "command:trim"`).
//               T is NOT in res/radiant.rc's accelerator table (:490-501) and is
//               NOT one of the fly's swallowed keys (W/A/S/D/Q/E), so there is
//               nothing further to arbitrate.
//               FINAL 0x54 occupancy: 0 Trim · 1 ToggleTexMoveLock ·
//               3 ViewTextures · 5 ThickenPatch.  No pair carries two rows.
//
// ── SHAKEOUT H: Z INSIDE A DRAWING TOOL IS NOT A REBINDING ──────────────────
// The drawing tools take a bare Z as their VERTICAL CONSTRAINT toggle
// (kiwi_construct.cpp, the Z rung in KiwiDrawTool::KeyDown).  NO TABLE ROW MOVES
// for it and Match Face keeps bare Z outright, for exactly the reason the movable
// pivot's V needs no row either: KiwiUX_KeyFunnel's FIRST arm hands every key to
// the ACTIVE MODAL COMMAND before Radiant_TryHotkey is ever consulted
// (kiwi_command.cpp:983-984), so Z means "vertical" only for the frames a drawing
// tool owns the gesture and means Match Face every other time.  Match Face is a
// SELECTION-context verb; a drawing tool is a COMMAND context; the funnel has
// ranked them that way since shakeout E.  Plasticity draws the same line — its
// axis choices live in the `body[gizmo=point-picker]` keymap scope
// (default-keymap.ts:353-366, `"z": "snaps:set-z"`), i.e. active only while a
// point is being picked.
//
// ── SHAKEOUT G: THE MODELLING VERBS (C · Z · J · E · Ctrl+R) ────────────────
// Three of the five are the keys PLASTICITY itself uses, bare: `c` cut,
// `j` join-curves, `e` extrude (default-keymap.ts:257/258/264).  Z and Ctrl+R are
// KIWI's own — Plasticity has NO match-face verb (kiwi_matchface.h) and its
// SplitFactory is unreachable from its UI (kiwi_split.h).
//
//   vk 0x43 C   occupancy: 0 CameraDown 33056 · 1 CapCurrentCurve 32885 ·
//               2 AutoCaulk 33220 · 4 Copy 33039 · 5 ToggleCamera 33069
//               (mods 1 is PRIM_BOX and mods 3 is CapCurrentCurve after
//               shakeout F, so 6 and 7 are the free C chords)
//               33056 -> Ctrl+Alt+C (mods 6).  Bare C: KIWI_CMD_CUT 34055
//               CameraDown is a TANK CAMERA key — the same family whose bare
//               arrows this profile already unbinds for the smooth fly, and whose
//               modern equivalent is the fly's Q/E.  Menu + palette keep it.
//
//   vk 0x5A Z   occupancy: 0 CameraAngleDown 33062 · 4 Undo 57643 ·
//               5 ToggleZ 33070 (mods 1 is PRIM_SPHERE after shakeout F)
//               33062 -> Shift+Alt+Z (mods 3).  Bare Z: MATCH_FACE 34056
//
//   vk 0x4A J   occupancy: 0 ToggleOutlineDraw 33103 · 1 ToggleTintDraw 33172 ·
//               5 TolerantWeld 33155 (mods 4 is CONSTRUCT_JOIN after shakeout F)
//               33103 -> Shift+Alt+J (mods 3).  Bare J: KIWI_CMD_JOIN 34106
//               THIS REVERSES SHAKEOUT F'S OWN RULING ("displacing
//               ToggleOutlineDraw to buy a chord Ctrl+J gives away free is a bad
//               trade"), and deliberately: the directive names the bare key, and J
//               is no longer a lines-only verb — it is the MERGE key too.  Ctrl+J
//               is untouched and stays bound to Join Lines as the lines-only
//               alias, so nothing shakeout F taught stops working.
//
//   vk 0x45 E   occupancy: 0 DragEdges 33006 · 1 RedisperseRows 32888 ·
//               4 SelectTargettedEntities 36110 · 5 RedisperseCols 32889 ·
//               6 SelectConnectedEntities 33134
//               33006 -> Shift+Alt+E (mods 3).  Bare E: EXTRUDE_FACE 34058,
//               which is a CONTEXT id (face -> new body, else region — see
//               kiwi_extrude.h).  E is also a fly key under RMB mouselook; the
//               fly's swallow rung sits BELOW the active-command arm in
//               KiwiUX_KeyFunnel, so the two never both fire — the same
//               arrangement S (scale / strafe) has had since shakeout A.
//
//   vk 0x52 R   Ctrl+R occupancy: mods 4 RemoveColorNode 10.  (0x52's other rows
//               are MouseRotate 32810 at mods 0 and ToggleTexRotateLock 32835 at
//               mods 1, both already displaced by this profile; free after it: 2,
//               5, 6, 7.)  10 -> Shift+Ctrl+R (mods 5).  Ctrl+R: SPLIT_FACE 34057
//               Ctrl+R is NOT in res/radiant.rc's accelerator table (:490-501),
//               which matters because TranslateAccelerator runs BEFORE the hotkey
//               table and a collision there would have been silent.
//
//   V           NOT BOUND, and must not be.  The movable pivot's V is
//               COMMAND-LOCAL, exactly as Plasticity binds it (per gizmo context,
//               default-keymap.ts:120/147/159/179 — never globally).  KiwiUX_Key-
//               Funnel routes every key to the active modal command, so V reaches
//               Move / Rotate without a table row and vk 0x56's global occupancy
//               (0 DragVertices 33005 · 1 DRAW_RECT_CENTER · 3 VehicleGroup ·
//               4 Paste 33040 · 5 ToggleView 33071) is completely undisturbed.
//
// FINAL OCCUPANCY RE-CHECK after the shakeout-G patches: C carries 0=Cut,
// 1=Box, 2=AutoCaulk, 3=CapCurrentCurve, 4=Copy, 5=ToggleCamera, 6=CameraDown;
// Z carries 0=MatchFace, 1=Sphere, 3=CameraAngleDown, 4=Undo, 5=ToggleZ;
// J carries 0=Join, 1=ToggleTintDraw, 3=ToggleOutlineDraw, 4=JoinLines,
// 5=TolerantWeld; E carries 0=Extrude, 1=RedisperseRows, 3=DragEdges,
// 4=SelectTargettedEntities, 5=RedisperseCols, 6=SelectConnectedEntities;
// R carries 0=Rotate, 1=MouseRotate, 3=ToggleTexRotateLock, 4=SplitFace,
// 5=RemoveColorNode.  No (vk, mods) pair carries two rows on any of them.
//
// ── SHAKEOUT F: THE PLASTICITY CREATION CHORDS ──────────────────────────────
// USER DIRECTIVE, verbatim: "the shift-A menu is unacceptable.  Shift-A is for
// LINES.  Start the line tool immediately and the other ones are on other keys
// (lookup how plasticity keybinds work)."
//
// Looked up: plasticity/src/startup/default-keymap.ts, `body:not([gizmo])`,
// lines 283-292 — shift-a line · shift-s curve · shift-q corner-rectangle ·
// shift-w center-circle · shift-z sphere · shift-x cylinder · shift-c corner-box
// · shift-v center-box.  All eight ship.  Shakeout C's Shift+A ADD MENU is
// UNBOUND, not deleted: KIWI_CMD_ADD_MENU 34027 still dispatches, still has its
// palette row ("Add Menu (create)") and still has its button in the KIWI panel's
// Construct block, and radiant.ini [Commands] can rebind it by name.
//
// ONE DEVIATION: Plasticity's shift-v is `command:center-box` and KIWI has no
// centre-box primitive (RADIANT_UX_DESIGN §16b.3 lists it as LATER).  Shift+V
// takes Rectangle (CENTRE) instead, which keeps the corner/centre pairing the
// neighbouring keys teach and means the chord will not have to move when the
// centre box does ship.
//
// ── THE FULL COLLISION / DISPLACEMENT CHAIN (shakeout F round) ──────────────
// Every chord audited against the WHOLE 187-row default table.  "Occupancy" is
// every mods value that key carries in the DEFAULT table; the arrow rows are what
// the modern profile does about it.  Displacement is always the house two-step:
// the occupant of the claimed Shift chord goes to Shift+Alt+key, which was free on
// all five of them.
//
//   vk 0x41 A   occupancy: 0 CameraAngleUp 33061 · 1 SelectAllOfType 33093 ·
//               2 SelectAllOfTypeRecurse 33212 · 4 ShowAllTextures 32973 ·
//               5 AddTerrainRow 33153
//               33093 -> Shift+Alt+A (mods 3) [shakeout C, unchanged]
//               Shift+A: ADD_MENU 34027 -> DRAW_LINE 34034   [shakeout F]
//
//   vk 0x53 S   occupancy: 0 SurfaceInspector 33041 · 1 PatchInspector 33092 ·
//               4 FileSave 57603 · 5 MakeStructural 33043
//               33092 -> Shift+Alt+S (mods 3)                [shakeout C]
//               33041 -> Shift+S (shakeout C) -> **Alt+S (mods 2)** [shakeout F]
//               Shift+S: DRAW_SPLINE 34050
//               NOTE: mods 2 is the ONLY chord left on S — 0 is Scale, 1 is the
//               spline, 3 is the Patch Inspector, 4 and 5 are compiled-in.  The
//               Alt-chord limitation (WM_SYSKEYDOWN is not routed to the hotkey
//               table in the ImGui shell) is already in RADIANT_KNOWN_ISSUES; the
//               Surface Inspector stays on the Textures menu and in the palette.
//
//   vk 0x51 Q   occupancy: 2 LinkSelected 33211 (mainfrm.cpp:1143) ·
//               5 RemoveTerrainRow 33154 (mainfrm.cpp:1007)
//               mods 1 FREE — nothing displaced.  Shift+Q: DRAW_RECT 34036
//               mods 0 FREE TOO — ROUND L takes it for BOOLEAN 34062, again with
//               nothing displaced.  Q is a fly key (W/A/S/D/Q/E) and that is the
//               same non-collision E already lives with: the fly's swallow rung
//               sits below the active-command arm in KiwiUX_KeyFunnel.
//               FINAL 0x51: 0 Boolean · 1 Rectangle · 2 LinkSelected ·
//               5 RemoveTerrainRow.
//
//   vk 0x57 W   occupancy: 0 ConnectSelection 33021 · 1 TogglePatchWireframes
//               32857 · 2 SplaySelection 33157 · 5 MakeWeaponClip 196
//               32857 -> Shift+Alt+W (mods 3).  Shift+W: was DRAW_CIRCLE 34037,
//               ROUND AG item 4: PRIM_BOX 34051 (straight swap with Shift+C)
//
//   vk 0x5A Z   occupancy: 0 CameraAngleDown 33062 · 4 Undo 57643 ·
//               5 ToggleZ 33070
//               mods 1 FREE — nothing displaced.  Shift+Z: PRIM_SPHERE 34053
//               (Ctrl+Z never reaches the table: Radiant_PreTranslateMessage
//               handles it upstream, radiant_main.cpp:661-665.)
//
//   vk 0x58 X   occupancy: 0 ToggleClipper 32783 · 1 ToggleCrosshairs 33100 ·
//               2 Center2DOnCamera 33108 · 4 SelectedAssociated 33152 ·
//               5 SplitPatch 33158
//               33100 -> Shift+Alt+X (mods 3).  Shift+X: PRIM_CYLINDER 34052
//               ROUND S: 33152 -> Ctrl+Alt+X (mods 6).  Ctrl+X: CLIP_CUT 34112
//               (and res/radiant.rc's Ctrl+X -> File->Exit accelerator is gone)
//
//   vk 0x43 C   occupancy: 0 CameraDown 33056 · 1 CapCurrentCurve 32885 ·
//               2 AutoCaulk 33220 · 4 Copy 33039 · 5 ToggleCamera 33069
//               32885 -> Shift+Alt+C (mods 3).  Shift+C: was PRIM_BOX 34051,
//               ROUND AG item 4: DRAW_CIRCLE 34037 (user directive; this is the
//               one place the creation table deliberately leaves Plasticity's)
//               ROUND S: 33056 -> Ctrl+Alt+C (mods 6), freeing C for CUT 34055
//
//   vk 0x56 V   occupancy: 0 DragVertices 33005 · 1 VehicleGroup 33221 ·
//               4 Paste 33040 · 5 ToggleView 33071
//               33221 -> Shift+Alt+V (mods 3).  Shift+V: ROUND AF gave it to
//               PRIM_BOX_CENTER 34064; DRAW_RECT_CENTER 34047 moved to Alt+V (2)
//
//   vk 0x4A J   occupancy: 0 ToggleOutlineDraw 33103 · 1 ToggleTintDraw 33172 ·
//               5 TolerantWeld 33155
//               mods 4 FREE — nothing displaced.  Ctrl+J: CONSTRUCT_JOIN 34104
//               DELIBERATE PARTING FROM PLASTICITY: it binds join-curves to a BARE
//               j (default-keymap.ts:261).  Bare J here is ToggleOutlineDraw, a
//               display toggle a mapper uses constantly, and displacing it to buy
//               a chord Ctrl+J gives away free is a bad trade.  Ctrl+J is also not
//               in res/radiant.rc's accelerator table (:490-501), which matters
//               because TranslateAccelerator runs BEFORE the hotkey table and an
//               accelerator collision would have been silent.
//
// FINAL OCCUPANCY CHECK — after the profile is applied, no (vk, mods) pair carries
// two rows on any of the nine keys above, and every KIWI extension row sits AFTER
// all 187 compiled-in rows, so Radiant_TryHotkey's first-match-wins can only ever
// hand a KIWI command a chord the profile actually vacated.
//
// The Delete row is shakeout B (user directive: "Change backspace to delete and
// make it work with the new 3d cam view").  Two things about it:
//
//   * vk 0x2E was NOT free.  Stock Radiant binds it to ZoomIn (mainfrm.cpp:1056,
//     View->Zoom->XY Zoom In) and Radiant_TryHotkey is first-match-wins over ROW
//     ORDER, so simply appending a Delete row would have been dead code.  ZoomIn
//     moves to Shift+Delete; the audit is the usual one — 0x2E carries only mods
//     0 (ZoomIn) and mods 4 (ZZoomIn 32999, also the Ctrl+Delete accelerator in
//     res/radiant.rc:499), so mods 1 was free.  Insert/ZoomOut is untouched.
//   * BOTH keys delete in modern.  Delete Selection 33003 gets a SECOND table row
//     ("KiwiDeleteSelection", registered by kiwi_command.cpp through
//     Radiant_RegisterCommandAlias), and the profile binds THAT row rather than
//     the base one — so the binary's Backspace row is bit-identical in both
//     profiles and the classic profile is still "defaults + radiant.ini".
//
// The four arrow rows are shakeout A.  Classic's bare arrows ARE the "tank
// controls" (22.5-degree yaw hops and 32-unit lurches, mainfrm.cpp:4529-4569);
// unbinding them in the modern profile hands the bare arrows to the smooth,
// dt-scaled fly in kiwi_camera.cpp, which polls them with GetAsyncKeyState and so
// never enters the hotkey table at all.  Only the BARE arrows move: every
// modified arrow chord (TexShift*, TexRotate*, TexScale*, SelectNudge*, Vertex
// Select Up/Down) keeps its classic binding in both profiles, and the fly ignores
// modified arrows for exactly that reason.
//
// The DISPLACED commands are not dropped — they move, so nothing becomes
// keyboard-unreachable.  Each destination was checked against the whole default
// table for a (vk, mods) collision first:
//
//   CameraDown          33056  ->  Ctrl+Alt+C     (0x43, 6)   [shakeout G]
//   CameraAngleDown     33062  ->  Shift+Alt+Z    (0x5A, 3)   [shakeout G]
//   ToggleOutlineDraw   33103  ->  Shift+Alt+J    (0x4A, 3)   [shakeout G]
//   DragEdges           33006  ->  Shift+Alt+E    (0x45, 3)   [shakeout G]
//   RemoveColorNode     10     ->  Shift+Ctrl+R   (0x52, 5)   [shakeout G]
//   ViewTextures        33018  ->  Shift+Alt+T    (0x54, 3)   [shakeout H]
//   ViewConsole         33016  ->  Shift+Alt+O    (0x4F, 3)   [round J]
//   SameTargetname      36121  ->  Shift+Alt+B    (0x42, 3)   [round J]
//   RotateZ             32961  ->  Shift+Alt+D    (0x44, 3)   [round J]
//   MouseRotate         32810  ->  Alt+R          (0x52, 2)   [round J; was Shift+R]
//   ShowHidden          32924  ->  Alt+H          (0x48, 2)   [round J; traded with 32934]
//   HideUnSelected      32934  ->  Shift+H        (0x48, 1)   [round J; traded with 32924]
//   SurfaceInspector    33041  ->  Alt+S          (0x53, 2)   [shakeout F; was Shift+S]
//   PatchInspector      33092  ->  Shift+Alt+S    (0x53, 3)   [vacated Shift+S]
//   TogglePatchWireframes 32857 -> Shift+Alt+W    (0x57, 3)   [shakeout F]
//   ToggleCrosshairs    33100  ->  Shift+Alt+X    (0x58, 3)   [shakeout F]
//   CapCurrentCurve     32885  ->  Shift+Alt+C    (0x43, 3)   [shakeout F]
//   VehicleGroup        33221  ->  Shift+Alt+V    (0x56, 3)   [shakeout F]
//   MouseRotate         32810  ->  Shift+R        (0x52, 1)
//   ToggleTexRotateLock 32835  ->  Shift+Alt+R    (0x52, 3)   [vacated Shift+R]
//   VertEdit            33199  ->  Shift+G        (0x47, 1)
//   AssociateEntities   33150  ->  Shift+Alt+G    (0x47, 3)   [vacated Shift+G]
//   ViewFilters         33104  ->  Alt+F          (0x46, 2)
//   ZoomIn (XY)         32995  ->  Shift+Delete   (0x2E, 1)
//   SelectAllOfType     33093  ->  Shift+Alt+A    (0x41, 3)   [vacated Shift+A]
//   GridDown            33084  ->  Shift+Ctrl+[   (0xDB, 5)
//   GridUp              33083  ->  Shift+Ctrl+]   (0xDD, 5)
//   SetGrid1/2/4/8/16   35022..35026  ->  UNBOUND (vk 0), reachable from the Grid
//                       menu, from the classic profile, and by stepping [ ].
//
// The G chain's audit, written out because G was the crowded one: in the default
// table vk 0x47 carries mods 0 (VertEdit), 1 (AssociateEntities), 4
// (SelectSnapPointsToGrid), 5 (DisassociateEntities) and 6 (ToggleSnapToGrid), so
// mods 3 was the one free G chord and the two-step displacement lands exactly in
// it.  Shift+Alt+G shares the Alt-chord limitation already logged in
// RADIANT_KNOWN_ISSUES (WM_SYSKEYDOWN is not routed to the hotkey table in the
// ImGui shell) — Associate Entities stays reachable from the menu and the palette,
// same as the other Alt destinations above.
//
// Everything else in the 187-row table is untouched.  In particular the grid-size
// digits 6..9 (SetGrid32/64/256/512) keep their classic meaning in BOTH profiles
// for now — §17's arbitrary-spacing migration owns them later.
//
// The KIWI rows themselves live in `g_radiantCommands` too (kiwi_command.cpp
// registers them), so the palette lists them, the command-list panel shows their
// binding, and radiant.ini [Commands] can remap them by name.
// ─────────────────────────────────────────────────────────────────────────────

enum kiwiKeymap_t
{
    KEYMAP_CLASSIC = 0,
    KEYMAP_MODERN  = 1,      // the overhaul's DEFAULT
};

kiwiKeymap_t KiwiKeymap_Get();

// Switch profile, persist it, rebuild the live table, and re-annotate the menu
// bar's accelerator text.
void KiwiKeymap_Set( kiwiKeymap_t profile );

// Boot hook: called right after Radiant_LoadCommandMap (radiant_main.cpp), so the
// profile lands on top of the user's radiant.ini overrides.  Patches only — it
// does NOT re-run the reset/reload, because boot already did both in order.
void KiwiKeymap_ApplyBoot();

// The profile switcher, drawn inside the KiwiUX settings block.
void KiwiKeymap_DrawSettings();
