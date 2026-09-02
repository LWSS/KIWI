# Radiant known issues / review log

## U-RIP DELETIONS WORTH KNOWING (2026-08-08)

- eclass.cpp g_bBuildList `CFile` block DELETED (old ~1589-1607): it hand-read
  MFC's CStringData COW header off a NULL `lpBuffer` (guaranteed AV if the
  flag was ever set) and wrote newdefs.def. The `g_bBuildList` flag itself
  survives (referenced at :126/:1268/:1400); if the build-list dump is ever
  wanted again, reimplement on std::string + fopen.
- The MFC shell (all C*Wnd/C*Dlg classes, CMainFrame, radiantapp.cpp, dialog
  .rc templates) is deleted, not archived — recovery is git history and the
  scratchpad pre-rip snapshot only.

## PHASE-4 TEST NOTES (for the user's ON/OFF-build A/B — HISTORICAL; the OFF
## build no longer exists after U-RIP. The camera-restoration items remain
## worth exercising in the single remaining build.)

- U-GLOBALS restored camera features that transited dead storage mid-campaign:
  deliberately exercise camera fly keys, View→Center, Up/Down Floor, map-load
  camera placement, point-file next/prev, error-file jump, Z/XY ctrl-click
  camera ops, light previews, prefab reframe — in BOTH builds.
- Headless-gate input changes from dropped null-guards: pmesh
  Patch_SelectCtrlPoint fallback nViewType was 0, now reads live state
  (default 2); drag marquee corners/brush MakeSided read real state instead of
  zeros. Selftest baselines may shift accordingly.
- mainfrm.cpp ~1005 Radiant_CenterXYOnMap: dead code, last writer of CXYWnd
  m_vOrigin/m_fScale class members — delete with U-GUARD.

Found during the UI rework's as-you-go sanity pass. Triage: REVIEW = check vs
IDA when convenient; NOTE = faithful-but-ugly, no action planned.

## REVIEW

<!-- REMEDIATION PASS 2026-08-26 (after the kiwi_* comment cleanup + checkpoint 62ba4b8).
     Method: these are PORTED files, so each item is verified against the CoD4Radiant IDB
     (127.0.0.1:13337) BEFORE any edit — a wrong "fix" that deviates from the binary is worse
     than the bug (match-IDA, never invent). Resolved this pass:
       * modeldlg.cpp ModelDlg_DoReplace HANG  -> REAL port bug (dropped address-of), FIXED.
       * surfacedlg.cpp &mat_texDef+layer       -> VERIFIED FAITHFUL vs 0x457b08, left as-is.
     The remaining REVIEW items are each a per-item IDA verification (many are ImGui-shell
     CONSOLIDATION todos, not code bugs; several "verify vs IDA" tend to resolve faithful like
     surfacedlg did). Work them incrementally, IDA-first. The kiwi_* cleanup also surfaced ~160
     flags in the NEW code, compiled separately at F:\CLAUDE WORKING DIR\kiwi_cleanup. -->

- [FIXED 2026-08-26] `src/radiant/modeldlg.cpp` ~46 — HANG in ModelDlg_DoReplace.
  IDA-verified (CoD4Radiant 0x434f10): the binary saves `p_prev = &v5->next->prev`
  (ADDRESS-OF the prev field) and later `v5 = (selbrush_t*)p_prev`; with prev@+0x00
  that address IS `b->next`. The port dropped the `&`, writing `b->next->prev` (the
  VALUE), which == b on a well-formed list → infinite loop on the first brush.
  Fixed to `next = b->next` (faithful; matches xywnd:1930/drag:1501/csg:263).

- [VERIFIED FAITHFUL 2026-08-26] `src/radiant/surfacedlg.cpp` 230/249/... the
  per-layer `&md->mat_texDef + layer` idiom. IDA-checked: 0x457b08 does exactly
  `texdef = &mtlDef->mat_texDef + LayerMat::GetCurrentLayer(mtlDef)` (with its own
  `// 7*4bytes` note), AND upstream (0x457a3e) already selects
  `mtldef[current_edit_layer]` — so the "double step" is IN THE BINARY, not a
  transcription error. The port reproduces it correctly; per match-IDA it STAYS
  (any layer>0 quirk is a faithful-original one). NOT a port bug — no change.
- `src/radiant/surfacedlg.cpp` 143-144/160-161 — log-and-continue asserts on
  null pointers that are dereferenced immediately after (release null-deref);
  151 — guard tests `width` but the assert string says "size[0]" and division
  by zero width proceeds.
- `src/radiant/surfacedlg.cpp` 115-119 — `if (...) return X;` followed by an
  unconditional `return X;` with the identical expression: dead branch (so
  Surf_TargetMaterialDef never returns NULL, contradicting its :99 contract
  comment); check the IDB for a dropped condition body.
- `src/radiant/mainfrm.cpp:2137` CMainFrame::TryHotkey walks g_radiantCommands
  with NO seed call (the only reader that doesn't) — benign today (unseeded
  table is all-zero), but a live ordering hazard the moment the ImGui shell
  dispatches keys before LoadCommandMap runs. Add the seed (or route through
  Radiant_GetCommandTable) in the consolidation pass.
- Modifier-name spelling differs across the three formatters (CommandList_Mods
  "Left Win" / ShowMenuItemKeyBindings "LWin-" / LoadCommandMap parses "+lwin")
  — each may be faithful to its own binary site; verify one wasn't transcribed
  from the wrong function.
- `src/radiant/patchdialog.cpp:362-407` PHASE-3/4 CONSOLIDATION: the adv-patch
  paint MODE radios + channel/soft-select checkboxes are read live off the
  dialog HWND (AdvDlg_IsChecked → sub_401DB0; no dialog → mode 6 "Disabled") —
  terrain paint is uncontrollable from an ImGui-only shell until a
  UI-independent paint-settings store replaces the HWND reads.
- `src/pmesh.cpp:4997-5002` — CurveEdit_InputToValue amplitude path: typed 0 →
  -inf, negative → NaN, and SnapStore's clamps pass NaN through to storage
  (bogus paint strength). Reachable from BOTH shells via the typed field.
  Also :4982 SetFromStep doesn't clamp pos (unguarded for future callers).
- PHASE-3/4 CONSOLIDATION: QE_SingleBrush Sys_Printfs on every rejection
  (qe3.cpp:342/352) — any polling caller floods the console; panels currently
  carry a silent pre-gate copy (imgui_panel_patch.cpp). Core fix: a silent
  selection predicate the printing wrapper calls.
- SHELL MUST-FIX before Phase 4: panel-reachable actions pop modal MessageBoxA
  (EclassCreate_Apply win_ent.cpp:582/611, EntDeleteKey_Apply :369) — from the
  ImGui overlay these run a modal message pump INSIDE the D3D scene bracket
  (overlay is submitted pre-EndScene in rb_backend). Fix candidates: submit the
  overlay in its own BeginScene/EndScene pair before Present (RB_SwapBuffers
  head), or defer/queue the message boxes. MFC shell unaffected.
- `src/radiant/findtexture.cpp:322/510-526` PHASE-3/4 CONSOLIDATION: the
  texture-window pick path fills the Find/Replace fields only via the MFC
  singleton's member setters (g_dlgFind->SetFindText/SetReplaceText, driven by
  byte_73C380) — the ImGui panel never receives picks. Fix in the
  consolidation pass: UI-independent pick sink both shells subscribe to.
- `src/radiant/win_ent.cpp:672` PHASE-3/4 CONSOLIDATION: UpdateSelection
  early-returns without hwndEnt_entlist, so EclassSelect_Apply from an
  ImGui-only shell only caches the class name — the entity rebind/refresh
  chain never runs (panel re-gathers per frame to compensate). Same
  HWND-gating family as the surface inspector's SetTexMods/Wnd02.
- `src/radiant/surfacedlg.cpp` PHASE-3/4 CONSOLIDATION: SurfaceInspector_
  SetTexMods (:288) and _Wnd02 (:320) iassert surfDlgGlob.hwnd, and
  Surf_RefreshFields early-returns without g_pSurfDlg — the multi-layer texmod
  scratch save/commit transaction is unreachable from an ImGui-only shell and
  the trailing refresh in SurfaceDlg_Apply/_Spin is a silent no-op there (the
  ImGui panel re-Gathers to compensate). Decouple before MFC removal. Also
  :955-960 SurfaceSet_Apply caches surfInsp_tex_repeatx/y BEFORE validating
  `checked` — the error path still overwrites the cached repeats.

- `src/radiant/layersdlg.cpp` ~72: Layers_SetHidden → Layers_RebuildVisibility-
  Filters frees + re-creates every filter entry, and Layers_BuildFilter
  hardcodes `isShown = true` (filters.cpp:1180) — hiding/showing ONE layer
  resets every other layer's runtime visibility. Filter-registry trap area;
  verify vs IDA whether the binary rebuilds or patches in place.
- `src/radiant/layersdlg.cpp` ~502/511: the Hide/Show BUTTONS invalidate views
  only when Layers_SetLayerVisible returns true (false for layers with no
  script_layer brushes) while the context-menu path always forces -1 —
  same command, different repaint behavior. Check which matches the binary.
- `src/radiant/layers.cpp` ~556: Layers_DeleteLayer has no built-in guard —
  only the dialog's Layers_CanDeleteLayer protects 000_Global/The Map; any
  non-dialog caller can delete them (SetMapLayers re-creates, re-tagging
  brushes onto 000_Global as a side effect).
- `src/radiant/win_dlg.cpp` `GetSelectionIndex` (~line 161): hitting the
  def-list sentinel fires `Assert(...)` but does not return/break — a release
  build walks `j->onext` past the sentinel and keeps counting. Only
  hand-expanded `Assert` in the file (hardcoded line 175); siblings use
  iassert. Check the IDA original for whether the guard should terminate.

- [VERIFIED FAITHFUL 2026-09-01] `src/radiant/verteditdlg.cpp` ~34: VED_BracketPatch
  sets `patch->xx22b = true` and never clears it, so a second [Apply] on the same
  patch skips the undo bracket until a paint op resets the flag. IDB 0x461210 does
  exactly the same (sets the flag at both bracket sites, no Patch_Paint/PMESH_18 on
  the path) — a RETAIL quirk, not a dropped call. Any fix is a deliberate divergence.
  The :116 cite of 0x4611C8 (below the function entry) is still a transposed digit.
- `src/radiant/scriptgroup.cpp` ~1481 vs mainfrm.cpp ~3388: byte-identical
  duplicate Disassociate bodies in two files — the known duplicate-function
  drift trap; dedupe when either is next touched. Also turret handlers
  (~1541/1591) deref `b->owner->def` and write GetDlgItem(d_hwndMedia,…) with
  no guards, unlike every sibling walk; and ScriptGroupFlags_Gather's
  seen[256][16] dedup scratch has no length/count caps (faithful).
- `src/radiant/prefs.cpp` + res/radiant.rc:612: the camera MoveSpeed trackbar
  (1219) is in the template but never DDX-bound and absent from
  prefsDlgState_t — the "MoveSpeed" pref (prefs.h:36, live divisor in camwnd
  movement) is unreachable from ANY preferences UI. Check the IDB for the lost
  WM_HSCROLL/DDX; then add the field to the state struct + both panels. Also
  rc spin controls 1079/1098/1091 have no UDN_DELTAPOS handler (inert arrows),
  and radio 1012 is bound by nothing (template leftover?).
- `src/radiant/prefs.cpp`: four keys read by Prefs_LoadPrefs are never written
  by Prefs_SavePrefs (ApplyDismissesSurface, CleanTinyBrushes,
  CleanTinyBrusheSize, SelectCurves) — only SelectCurves is documented as a
  binary quirk; confirm the other three vs IDA and comment them. Also
  OnInitDialog never runs the view-mode enable pass, so Detached Windows /
  Transparent Background checkboxes start enabled regardless of m_nView until
  a view radio is clicked; and the FOV/scale clamps live inside Prefs_SavePrefs
  as a save side effect — unsaved writes keep out-of-range values.
- `src/radiant/win_ent.cpp` ~500: SpawnFlags_Apply single path has NO
  !edit_entity guard (faithful to 0x497040) — spawnflag click with nothing
  edited is a null deref; the ImGui panel must add a deliberate guard. Same
  hazard on the Up/Down pitch angle cases (~1618/1625). Also: sprintf into
  sz[4100] from 4095-char key + 4095-char value can overflow (~265); EditProp's
  `while (sz[j] != '\t')` scan is unbounded (~428); OnSize's proportional
  layout pass is dead code fully overwritten by the fixed-pixel block (~1286);
  UpdateSelection's comma-expression around LB_FINDSTRINGEXACT==-1 (~697) reads
  like a hex-rays artifact — re-check vs IDA.
- `src/radiant/layeredmaterialwnd.cpp`: tooltip map (~501) says 35014=up/
  35015=down but the command dispatch treats 35014=down/35015=up — inverted
  somewhere; check the binary's toolbar order. Also hit-test (~779) uses 66px
  row pitch vs the 68px draw pitch — clicks drift one row per ~33 rows; and
  ~829 sets hbrBackground to COLOR_BTNTEXT (18) while the comment claims IDB
  value 16 (COLOR_BTNSHADOW) — name/constant disagree.
- `src/radiant/layeredmaterialwnd.cpp` cmd 5 (layer up): passes
  `selectedLayerIndex - 1` into an unsigned param — wraps at sel==0; only the
  toolbar-disable guards it today. The ImGui panel MUST reproduce that guard.
- `src/radiant/texturebar.cpp` ~120: `g_tbRotateAmt` (binary member @0x25C,
  init 45) has no control on the bar — the binary's rotate-amount field looks
  unported, so the rotate spin step is stuck at 45. Also ~311: the bar writes
  size[0]/[1] from edit fields with NO zero guard while surfacedlg.cpp:250
  clamps 0→0.25f — blank scale field zeroes the texdef; check which matches
  the binary.
- `src/radiant/select.cpp` ~2725: CKeyValueSelectDlg::PostNcDestroy only chains
  to CWnd — never nulls g_dlgKeyValue / the s_kvs* HWND statics and never
  `delete this` (every sibling popup does both) — after a real destroy the
  statics dangle and the object leaks. Compare CFindBrushDlg win_dlg.cpp:311.
- `src/radiant/entitylist.cpp` ~223: the Close button is created with id IDOK
  but the class is a CWnd with no ON_BN_CLICKED(IDOK) — clicking Close does
  nothing. Also ~296: PopulateTree re-inserts the Key/Value columns on every
  EntList_Open without deleting old ones — columns accumulate per re-open.
- `src/radiant/findtexture.cpp` ~209-217: the rebuild tail (Brush_BuildWindings,
  MarkMapModified, version bump, update bits) runs for EVERY brush walked even
  when nothing was replaced — a no-match find/replace still dirties the map.
  Re-check against IDB 0x492E20's epilogue.
- `src/radiant/findtexture.cpp` ~183: memcpy bit-casts a packed float sample
  size into a `MaterialDef*` passed to Init_MaterialLayer on the flags&8 "Live"
  path — plausible AV; verify the IDB really does this.
- `src/radiant/pmesh.cpp` ~7019 (Patch_Cap): cap size chosen from `pm->width > 9`
  but the ring loop length is `bByColumn ? width : height` — width<=9 &&
  height>9 && !bByColumn reads past the 8-entry s_capPerimeter3x3 table. May be
  a faithful original bug; verify vs IDA.
- `src/radiant/pmesh.cpp` ~7188-7221 (Patch_CapSpecial): nType 1/3 arms read
  SRC(3)/SRC(4) with no `pm->width >= 5` guard — reachable from the cap dialog
  with a 3-wide patch. Verify vs IDA.
- `src/radiant/pmesh.cpp` ~7291/7344: `(entity_brush_s *)sel` cast of a
  `selbrush_t*` then `->owner` deref — smells like the instance-vs-def hex-rays
  field trap; verify member offsets.

- `src/radiant/patchdialog.cpp` ~155: comment says the Color/Alpha pick "redraws
  the owner-draw swatch" but neither handler calls InvalidateRect — swatch only
  repaints on incidental WM_DRAWITEM. Stale comment or dropped redraw (classic
  cleanup-path omission); check the IDB handler tails.
- `src/radiant/patchdialog.cpp` ~875: `g_pParentWnd->m_pActiveXY->m_nViewType`
  deref with no null guard on the patch-density OK path — no active XY view
  would fault. Verify the binary really assumes one.

## NOTE

- `win_dlg.cpp` ~96: `selbrush_t *&inst = selectedBrushInst;` reference alias
  to the local above it; hex-rays-shaped, faithful.
- `win_dlg.cpp` `ArbRot_ApplyAxis`: `(float (*)[4][3])rot_around` cast of a
  `float[4][3]` — faithful binary idiom; callee re-derives layout. Also
  `g_nUpdateBits = -1` written both here and in `ArbRotate_Apply` (redundant,
  faithful).
- Hand-built popup dialogs keep widget HWNDs in file-scope statics and
  `PostNcDestroy` nulls them unconditionally — fine while dialogs are
  singletons; dies with the MFC shell in Phase 4 anyway.
- `win_dlg.cpp` `DoColor`: blue-channel operand order flipped vs red/green
  (`255.0 * x` vs `x * 255.0`) — hex-rays transcription artifact, harmless.
- `dynentitydlg.cpp` ~159: DynEntHelp_Gather consults only the FIRST selected
  dyn_ entity (return nullptr inside the loop) — faithful to the extraction;
  re-check 0x40E5C0 whether the binary tries later entities.
- Panel-side pattern note: imgui_panel_dynent.cpp renders the two MessageBoxA
  texts INLINE (dismissible) — the preferred pattern for new panels vs modal
  boxes inside the scene bracket.
- `mapinfo.cpp` ~176: MapInfo_01 smuggles `prefabStats` pointer through an
  `int` parameter — faithful, but a hard x64-port blocker (see x64 plan).
- `mapinfo.cpp` ~63-67: dead retained inlined-helper head (`{ int *stats =
  worldStats; iassert(stats); }`) — faithful.
- `mapinfo.cpp` ~111: bare `& 8` class bit where CLASS_PREFAB beside it is
  named; the model-entity bit lacks a symbolic constant.
- `findtexture.cpp` ~110: raw-offset deref `*(const char **)((char*)def->
  modelClass + 4)` for the prefab .map name — raw-offset-sweep leftover.
- `findtexture.cpp`: `byte_73C380` (which edit box a texture pick fills) is
  file-scope UI routing state read by texwnd.cpp — needs a home in the ImGui
  shell (Phase 3).
- `dynentitydlg.cpp` ~337: CB_GETLBTEXT into char[256] with no size arg — safe
  only while the combo holds the two seeded presets ("clutter"/"destruct").
- `dynentitydlg.cpp` ~392: with no `basepath` epair in the project entity, the
  Browse dialogs' prefix check rejects every pick with "File must be under
  [...]" — silent always-fail path.
- `vehicledlg.cpp`/`dynentitydlg.cpp`: every dialog-side `g_nUpdateBits |= 1`
  is dead — SetPair/RemovePair already set -1 internally. Faithful; leave.
- `vehicledlg.cpp` ~128: comment documents the binary resetting the accuracy
  slider to 44 on empty value; the port's edit box performs no reset —
  documented divergence, revisit in the ImGui panel.
- `vehicledlg.cpp` ~151: port's script-group ON_CONTROL_RANGE ids are
  contiguous, binary's are not — id→key mapping differs from the binary;
  relevant when building the ImGui panel from IDA references.
- `pmesh.cpp` ~6979: stale comment claims the cap dialog "is never popped";
  Patch_CapCurrent pops it. Fix when that region is next touched.
- `patchdialog.cpp` ~559/586: `(char*)&cp->texCoord + 8*current_edit_layer` —
  raw-offset arithmetic over what should be `texCoord[layer]`; sweep candidate,
  carried verbatim into PatchInspector_GatherPoint/ApplyPoint.
- `patchdialog.cpp` ~871: `(unsigned)wSel <= maxSel` doubles as the CB_ERR(-1)
  wrap check — the binary's own idiom, do NOT "fix".
- `patchdialog.cpp` CTextureLayout: defaults check button 1449 but OnOK only
  tests 1475/1476 (mode falls through to 0) — correct only if they're one radio
  group in PE rsrc 0x9F; also reuses ids 1089/1142 (inspector's X/Y edits) —
  trap if the two ever merge into one ImGui panel.

## UX overhaul (kiwi_* layer)

- `kiwi_selection.cpp` Sel_SyncToLegacy: a selection_t holding BOTH brush items
  and face items syncs into a legacy state with `selected_brushes` populated AND
  `g_SelectedFaces` non-empty. The legacy model treats those as mutually
  exclusive (Select_Brush iasserts faces==0||patch; sub_48E170 converts one
  into the other). Sync ordering (brushes first) dodges the assert, but ported
  ops were never exercised against the hybrid state — watch when mode 5
  "Everything" multi-kind selections start driving legacy commands (Phase 2+).
- `kiwi_pick.cpp` vertex/edge candidates have no occlusion test in v1 — a
  vertex behind a wall can win the hover pick. Revisit with Phase 1b hover
  rendering if it feels wrong in practice.
- `kiwi_boxselect.cpp` SEL_EDGE marquee: a physical brush edge shared by two
  faces yields TWO (face,edge) items. Harmless for selection (legacy sync
  promotes the owner brush either way). PAID (Phase 3): `kiwi_transform.cpp`
  BeginEdges dedups coincident segments unordered at 0.1 units before solving,
  and BeginVerts dedups coincident world positions (the ported Brush_MoveVertex
  does the per-face fan-out itself). The selection model still holds the
  duplicates — the dedup is at the CONSUMER, which is the right place while
  sel_item_t stays a (face,edge) pair.
- `kiwi_viewport.cpp` chips: the chip row sits inside the camera image and
  takes the image hover even with modern input OFF — a legacy click landing
  exactly on the top-left chip row no longer reaches CamWnd_*. Give chips
  their own visibility toggle if strict parity is ever needed.
- `kiwi_hover.cpp` BrushLive(): hover/active liveness is checked by list
  membership only; a freed node whose address is reused by a new selbrush_t
  draws the wrong outline for one frame. Cosmetic, self-healing.
- Alt-chord hotkeys are DEAD in the ImGui shell (pre-existing, surfaced by the
  Phase-2 keymap): Alt+key arrives as WM_SYSKEYDOWN, which nothing routes to
  Radiant_TryHotkey - affects ~21 stock Alt bindings (AutoCaulk, HideUnSelected,
  ...) AND the modern keymap's displaced destinations (Alt+F ViewFilters,
  Shift+Alt+S/R). All remain reachable via menus + palette. Proper fix: route
  WM_SYSKEYDOWN through Radiant_PreTranslateMessage's funnel/TryHotkey, minding
  Alt+menu-mnemonic conflicts (Alt+F = File menu).
- `kiwi_snap.cpp` v2 BEHAVIOUR CHANGE vs v1: pointing at a surface now yields
  SNAP_FACE (the raw Test_Ray point), where v1 grid-snapped that point. §6 ranks
  area above grid so this is the specified ranking, but it means free placement
  onto a face is off-grid unless the user types a value or locks an axis. §6's
  "numeric hygiene" quantise-near-integers-on-commit rule (decision D-4, still
  open) is NOT implemented — revisit together.
- `kiwi_transform.cpp` vertex move inherits the LEGACY grid: the ported
  Brush_MoveVertex (brush.cpp 0x471C30) always grid-snaps its `end` to
  `grid_sizes[d_gridsize]` and refuses moves under 0.3 units. So a typed value or
  a geometry snap is NOT exact for brush vertices — the solver rounds it. Spec
  §22 says re-skin that core rather than write a new solver, so this is accepted;
  the alternative is a new vertex solver, which §22 forbids for v1.
- `kiwi_transform.cpp` scale pivot: Select_Scale (select.cpp 0x48FDC0) recomputes
  Select_GetMid on EVERY call, and Select_GetMid floor-snaps to the legacy grid.
  A long S gesture therefore applies its residual ratios about a pivot that can
  shift by up to one legacy grid cell. Rotate does not have this (its pivot is
  latched by the caller, as in the ported nudge). Fixing it means either a
  pivot argument on Select_Scale (ported-logic change) or a re-implementation —
  neither is in scope; watch for drift on long scale drags.
- `kiwi_transform.cpp` edge move v1 limit: two SELECTED edges that share a face
  are ill-posed (the shared face has no retained third point that stays put).
  BeginEdges drops the second one with a console message rather than letting the
  last write win. A simultaneous multi-edge solve belongs with Phase 5 modeling.
- `kiwi_transform.cpp` edge move does NOT texture-lock (face push/pull does).
  An edge move reorients the adjacent planes and the ported edge/vertex drag
  path does not lock either, so there is no oracle for what the reprojection
  should do. Textures will swim on edge drags.
- `kiwi_pick.cpp` PICKF_EXCLUDE_SELECTED, area arm: Test_Ray keeps only ONE
  nearest hit, so when the frontmost surface is excluded the pick reports "no
  surface" rather than the surface behind it. ~~Only reachable for FACE selections
  (brushes on selected_brushes already carry BRUSHFLAG_SELECTED and the ported
  walker skips them anyway).~~ **AMENDED, ROUND X:** that parenthesis was true and
  was itself the "two clicks" bug — `sub_48D460` (select.cpp:677) discards every
  brush carrying `BRUSHFLAG_SELECTED` in BOTH lists, so nothing selected was ever
  pickable. `kiwi_pick.cpp`'s `selUnmask_t` now lifts the bit across the one
  `Test_Ray` call **when PICKF_EXCLUDE_SELECTED is NOT set**, so this note's own
  limitation is now the ONLY thing that hides a selected brush from a pick — and it
  is deliberate. Consequence, unchanged: while pushing a face, the snap falls
  through to the ground-plane grid instead of finding geometry directly behind
  the pushed face.
- ~~**UNDO IS SPLIT IN TWO while a construction tool runs**~~ **FIXED in
  shakeout I** (`kiwi_undo.h`). The two STORES are still separate — the ported
  undo is brush-snapshot based and still has no room for a thing that is neither
  a brush nor an entity — but the ORDER is now unified: a journal of
  `{LEGACY, CONSTRUCTION}` tickets, appended where each domain closes a record,
  popped newest-first by one Ctrl+Z. Construction REDO exists for the first time.
  See the shakeout-I section for what is left.
- `kiwi_construct.cpp` construction geometry is OUTSIDE `selection_t` in v1
  (scope ruling 1): the 1-5 modes never see it, box select never touches it,
  G/R/S never move it and Delete never deletes it. Editing an existing object
  means deleting it (Clear All) and redrawing. Unifying it with the typed
  selection needs a second addressing mode on `sel_item_t` — a later phase's
  decision. Snapping is the one crossing that DOES exist (snap v3).
- `kiwi_region.cpp` v1 has NO holes and NO nested loops (spec D-3): two
  concentric circles are two regions, not a ring. A chain component with a
  T-junction (any endpoint shared by 3+ segments) is rejected whole rather than
  guessed at, and a group that splits into two rings is dropped entirely.
- `kiwi_region.cpp` region loops are capped at 64 vertices (KREG_MAX_LOOP ==
  the §23 profile cap == the densest circle the store tessellates). A larger
  loop simply does not become a region — silently, with no console message.
- `kiwi_extrude.cpp` calls `KiwiValid_Rebuild` on each piece BEFORE linking it,
  and that helper calls `MarkMapModified()`. So a REJECTED extrusion still
  marks the map dirty even though it changed nothing. Cosmetic (the dirty flag
  only drives the save prompt); fixing it means a rebuild variant that does not
  mark, which is a ported-helper change.
- `kiwi_extrude.cpp` new brushes are seeded from `random_texture_stuff[0]` and
  get NO texdef fitting — every face carries the current material at its default
  projection, exactly as the ported `Ed_NewBrushDrag` leaves a dragged box.
  Extruded side faces will therefore need a Surface Inspector pass. §26 says
  preserve texdef through new ops; it is preserved, just not fitted.
- `kiwi_construct.cpp` polyline double-click detection is local (400 ms + 4 px
  since the previous click) rather than a shell WM_LBUTTONDBLCLK: the ImGui
  shell's camera dispatch does not deliver double-clicks. Enter and
  click-near-the-first-point both end a polyline, so this is a convenience path.
- `kiwi_snap.cpp` v3 SNAP_INTERSECTION is CONSTRUCTION-ONLY and resolves only on
  the ACTIVE construction plane. Brush-edge x brush-edge intersections need an
  occlusion story the pick layer does not have (see the vertex/edge occlusion
  entry above). The pair scan is capped at KSNAP_MAX_CSEGS (128) segments.
- `kiwi_snap.cpp` v3 changed the Esc/Enter ordering in `KiwiCmd_KeyDown`
  (kiwi_command.cpp): both keys now reach the active command's KeyDown BEFORE
  the framework acts on them, which is what kiwi_command.h always documented and
  what the implementation did not do. No Phase-2/3 command consumes either key,
  so their behaviour is unchanged — but a future command that DOES consume Esc
  can now make itself uncancellable. Watch for it in review.
- **CSG SUBTRACT DOES NOT EXIST IN THIS PORT** (Phase 5, §24). `csg.cpp` has
  `CSG_MakeHollow` (0x47D3C0, id 32982), `Brush_MergeList` (0x47D600),
  `CSG_Merge` (0x47DA40, id 32927) and the AutoCaulk pair (0x47E080/0x47E0F0,
  id 33220) — and nothing else. There is no subtract core, no dispatch case and
  no resource id; the CoD branch dropped GtkRadiant's `CSG_Subtract`. What
  survives of that machinery is `Brush_SplitBrushByFace` (brush.cpp 0x471960),
  the clipper's/hollow's per-plane split, on top of which a subtract would have
  to be WRITTEN. Phase 5's brief forbids new CSG math, so "CSG: Subtract" is
  simply not offered. Whoever adds it owns the decomposition and the fragment
  explosion that comes with it.
- `kiwi_csg.cpp` has NO result preview (§24 asks for one "where practical").
  Both cores mutate the live lists in place — CSG_Merge unlinks every selected
  instance before it tries and re-adds them on failure; CSG_MakeHollow frees the
  source brush as it goes — so a faithful preview needs a clone-and-run harness.
  Half of that now exists (kiwi_bevel.cpp draws an unlinked def for Inset).
- CSG Hollow's wall thickness is `grid_sizes[d_gridsize]`, the CLASSIC grid, not
  the modern §17 spacing. The core reads it directly; changing that is a
  ported-logic change. Set the classic grid before hollowing.
- `kiwi_bevel.cpp` bevel rebuilds by REMOVING and RE-ADDING its faces every
  frame (Brush_RemoveFace / Face_Alloc), i.e. two face-array reallocations per
  bevelled edge per frame. Deliberate: it makes rollback exact by construction
  (the untouched faces are never written at all). It is also why no face pointer
  may be held across a Face_Alloc call in that file.
- `kiwi_bevel.cpp` bevel requires an edge with EXACTLY TWO adjacent faces. A
  non-manifold edge, an edge whose adjacent faces are exactly opposed, and any
  patch edge are skipped silently per edge (the command refuses outright only if
  nothing survives). Vertex/corner chamfers are not offered at all.
- `kiwi_bevel.cpp` bevel does NOT texture-lock, and the new face inherits one
  adjacent face's material and texdef unfitted — the same limit the extruded
  side faces have. §26 says preserve texdef; it is preserved, not fitted.
- After a bevel commits, the SEL_EDGE items that drove it index windings that
  the rebuild has changed, so the selection is stale (harmless — every consumer
  bounds-checks — but the highlighted edges will not be the ones you bevelled).
- **`kiwi_bevel.cpp` INSET IS NOT A MESH INSET** and never can be on a
  plane-defined brush (no per-face vertex list to insert a ring into, and a
  border ring is non-convex). What ships is "Inset (clone)": clone the brush and
  push every face ADJACENT to the target face inward along its own normal. The
  original is untouched; the result is the smaller brush you then push/pull.
  Adjacency is "shares two winding points with the target at 0.1 units", so a
  face meeting the target only at a corner is NOT pushed.
- `kiwi_bevel.cpp` inset drag maps the cursor's in-plane distance from the face
  centre (start radius minus current radius). On a very elongated face that
  mapping is coarse near the long axis; type an exact depth instead.
- `kiwi_dupe.cpp` arrays: the numeric layer offers ONE scalar, so linear array
  splits its two parameters — DRAG sets the step offset, TYPED digits set the
  count. Radial takes only a count; its drag does nothing in v1 (arbitrary sweep
  angle and a pickable pivot are the obvious v2). The HUD states the split.
- `kiwi_dupe.cpp` arrays inherit `Clone_Selection`'s skips (select.cpp:2482):
  PATCHES and FIXED-SIZE ENTITY brushes are not cloned, because the binary
  duplicated those through the OLE clipboard path this port does not reproduce.
  Arraying a selection of only those produces nothing and says so.
- `kiwi_dupe.cpp` radial array's ghost preview rotates the source BBOX CENTRE
  and keeps the extents, so the ghosts stay axis-aligned while the real copies
  do not. It shows where each copy goes, not its exact silhouette.
- `kiwi_dupe.cpp` mirror is pure presentation over the CLASSIC ids 32956/57/58
  (DoFlip → Select_FlipAxis). Its pivot is `Select_GetMid`, which floor-snaps to
  the LEGACY grid — so a mirror of an off-grid selection can shift by up to one
  classic grid cell. Same class as the Select_Scale pivot note above.
- `kiwi_selext.cpp` "Select Touching" / "Select Connected (touching)" are
  BOUNDS-OVERLAP tests (±1 unit, the ported Select_Touching_R epsilon), not real
  geometric contact: two brushes whose boxes overlap but whose solids do not
  still count. Connected is capped at 1000 brushes and 64 sweeps, with a console
  message when a cap bites. The ported Select_*Tall / Touching / Inside cores
  are NOT reused — they take the single selected brush as a marquee box and
  DELETE it (select.cpp:1585 Region_BeginFromSingleBrush), which is a different
  operation.
- `kiwi_selext.cpp` "Select Same Material" compares only the CURRENT EDIT
  LAYER's registered material, and matches whole brushes (any face) when the
  active item is an object, faces when it is a face. Layered materials that
  differ on another layer still count as the same.
- `kiwi_selext.cpp` double-click = select connected consumes the second click
  with an inert gesture (kiwi_viewport.cpp KG_CONSUMED) instead of letting the
  marquee run, because `KiwiBox_End` treats a stationary release as a CLICK
  SELECT and would immediately replace the expansion with one brush. The FIRST
  click of the pair has already changed the selection, so a double-click on
  empty space leaves whatever that first click did.
- **`kiwi_uv.cpp` Texture Rotate and Texture Scale DO NOT PREVIEW.** Their cores
  open their own undo brackets (`Brush_RotateTexture` select.cpp:3320,
  `Brush_ScaleTexture` select.cpp:3238), and `Undo_GeneralStart` always allocates
  a fresh record (undo.cpp:347) against a 64-record cap (undo.cpp:82) — driving
  either once per frame would evict the user's whole undo history inside a couple
  of seconds AND leave `g_lastundo` pointing at the core's record while our own
  bracket was open. So the gesture mutates nothing until Commit, which makes ONE
  core call. The HUD carries the live value; the geometry does not move until you
  commit. Only Texture Shift is live (its core brackets nothing).
- `kiwi_uv.cpp` Texture Rotate is limited to WHOLE DEGREES because the texdef
  stores `(int)(rotate + deg + 2^-30) % 360` (select.cpp:3334). The fractional
  part of the 0.5 deg/px drag accumulates and is rounded once, at commit; a typed
  value is likewise rounded. The HUD says "whole degrees".
- `kiwi_uv.cpp` Texture Shift emits WHOLE TEXTURE UNITS only, including for typed
  values. The core's brush pass truncates its argument (`(float)(int)a1`,
  select.cpp:3078) while its face pass takes the raw float, so a fractional delta
  would apply to face selections and vanish on whole-brush selections. Emitting
  integers makes the two passes agree. A fractional shift still has to go through
  the Surface Inspector.
- `kiwi_uv.cpp` Texture Shift on a PATCH brush is scaled by 0.001 by the core
  (`a1 * 0.001f`, select.cpp:3073) — that is the ported behaviour, so a 1-unit
  drag step moves a patch's texture imperceptibly. Patch texture shifting is
  effectively a Surface-Inspector job.
- `kiwi_uv.cpp` "Texture Scale" is an ADDITIVE SIZE STEP, not a factor: the core
  does `size[0] += (float)a1` (select.cpp:3249), exactly like the classic texture
  bar's scale spins. It is named and HUD-labelled accordingly; a multiplicative
  UV scale is not offered because no ported core provides one.
- `kiwi_uv.cpp` Texture Shift's Cancel applies the inverse of the applied total
  AND relies on the undo bracket's `Undo_Undo` restore. The bracket restore is
  exact (Undo_AddBrush clones the whole def); the inverse leg on its own is
  pure-delta float arithmetic and is NOT bit-exact, so it is the fallback, not
  the authority. Rotate/Scale cancel is exact by construction (nothing applied).
- `kiwi_uv.cpp` "Pick Texture" keeps the classic pick's side effect: it ends in
  `Texture_SetTexture` → `Brush_SetTexture(a2, 1)` (texwnd.cpp:1971), which
  APPLIES the picked material to the CURRENT SELECTION under the core's own undo
  bracket. Picking with something selected retextures it. The tooltip says so.
- `kiwi_uv.cpp` "Pick Texture" deliberately does NOT reproduce the classic
  branch's other side effect — overwriting the new-brush vertical template
  `g_qeglobals.d_new_brush_bottom_*` / `_top_*` from the hit brush's bounds
  (drag.cpp:702-708). Picking a texture should not silently resize the next brush.
- `kiwi_uv.cpp` "Pick Texture" on a PATCH runs `Radiant_PatchGetTexdef`
  (brush.cpp:2220), which WRITES the recovered texdef into the picked face's slot
  BEFORE any undo bracket exists — same as the classic path (drag.cpp:729), so
  that write is not undoable. Faithful, and unchanged.
- `kiwi_uv.cpp` "Pick Texture" runs off the LAST cursor position seen over the
  camera image (`ImGuiShell_CameraPaintCursor` returns false the moment the
  cursor leaves, imgui_shell.cpp:202), because the command is invoked from a
  panel button or the palette. If the cursor has never been over the 3D view this
  session the command says so and does nothing.
- `kiwi_uv.cpp` is the FIRST half of §26 only. There is NO TrenchBroom-style UV
  editing: no drag handles on the texture plane, no UV-space viewport, no
  per-vertex UV editing, no fit/centre/best-axis operators. §26's own text calls
  that direction Phase 6's goal; what shipped is viewport-modal manipulation over
  the ported cores plus a read-only readout. The Surface Inspector is still THE
  numeric texdef editor.
- `kiwi_uv.cpp`'s readout shows the ACTIVE selection item only, and only when it
  is a SEL_FACE. A multi-face selection shows one face's numbers; an object or
  edge/vertex active item shows nothing. It reads the CURRENT EDIT LAYER's
  sub-layer slot (`(&mat_texDef)[GetCurrentLayer]`), the same slot the cores
  write, so other layers are invisible.
### Shakeout A (camera nav, gizmo, view-cube, hints, axes)

- **RMB mouselook does NOT capture or re-centre the cursor**, deliberately. The
  ported free-look paths pair `ShowCursor(FALSE)` with `Cam_MouseUp`'s
  `ShowCursor(TRUE)`-until-visible loop, and the modern layer never enters that
  path (so there is no counter to keep balanced — `kiwi_camera.h`). Consequence:
  a long mouselook drag stops when the cursor reaches the screen edge; let go and
  drag again. Fixing it properly means raw input or a SetCursorPos/ShowCursor
  pair owned by this layer, which is exactly the state the RTT shell fought.
- The classic-RMB-click **replay** calls `Cam_MouseUp`, whose
  `do { r = ShowCursor(TRUE); } while (r < 0)` leaves the counter at +1 when
  nothing had hidden the cursor. That is the PORTED behaviour on any RMB click
  that did not fly (with the stock `CameraMode` pref of 1, every one of them), so
  the replay inherits it rather than introduces it. Harmless — the cursor is
  visible either way — but a later `ShowCursor(FALSE)` needs one extra call.
- The keyboard fly has **no collision and no clipping**: it walks straight
  through geometry, exactly like every DCC flycam. Deliberate.
- The fly polls with `GetAsyncKeyState` and is gated on `GetActiveWindow()`
  being non-NULL (this thread owns the foreground). If another app is focused but
  the editor still has the cursor over the camera image, the arrows do nothing.
- Bare arrow keys fly ONLY in the **modern** keymap profile — that profile is
  what unbinds classic's `CameraForward/Back/Left/Right` (mainfrm.cpp:1023-1026).
  In the classic profile the arrows keep the 22.5-degree/32-unit tank hops. The
  fly deliberately ignores modified arrows in both profiles so `TexShift*`,
  `TexRotate*`, `TexScale*` and `SelectNudge*` are never double-driven.
- **The gizmo is TRANSLATE-ONLY (v1).** No rotate ring, no scale box — R and S
  are the only way to rotate/scale. §14's ImGuizmo proposal is explicitly not
  used; the rationale is in the amended §14 and in `kiwi_gizmo.h`.
- The gizmo hit-test claims LMB inside ~8 px of a handle **before** box select,
  so a marquee started on top of the selection's own handles grabs the handle
  instead. That is the intended affordance, but it means the free-move square
  sits on the selection's reference point and cannot be marquee-dragged from.
  The "Move gizmo" toggle in KiwiUX settings turns the whole thing off.
- The gizmo's anchor is recomputed **every frame** from the live selection
  (`KiwiXform_ReferencePoint`), including mid-drag, so it follows the geometry.
  For an OBJECT selection with no active item that goes through
  `Select_GetTrueMid`, which walks `selected_brushes` — O(selection) per frame,
  plus one per hover tick. Fine at editor selection sizes; watch it if someone
  selects ten thousand brushes.
- `KiwiXform_ReferencePoint` DUPLICATES the per-kind reference rules from
  `KiwiMoveCommand::Begin*` rather than sharing them (the command's arms also
  build the units they will mutate, which the gizmo must not do). Three lines
  each — but they are two copies, and a change to one must be made in both.
- **The view-cube has no orthographic mode** and no corner/edge views. This
  camera is yaw/pitch with no roll and no ortho projection, so the six axis views
  are all it can express; the "persp" label under the widget is a readout, not a
  toggle. Top/bottom keep the current yaw and use pitch ∓89 (the pole clamp every
  other consumer of this camera already lives with), so they are 1 degree off
  vertical.
- The view-cube and the hint panel are ImDrawList-only and resolve their own
  input **during** the ImGui frame (like the chips), not in the post-present
  dispatch. That is safe because neither pops a modal or touches the message
  pump; do not add anything to them that does.
- The hint panel's FACE rows advertise **Bevel**, whose `KiwiBevel_CanBevel`
  predicate needs an EDGE selection — so pressing it on a face selection does
  nothing. Kept because the round's brief specified that row set; drop it or swap
  it for a face-meaningful op on the next pass.
- The hint panel's key column uses its own compact modifier spelling
  ("Shift+G") rather than `CommandList_Mods`'s full one ("Shift + G"); only the
  KEY NAME comes from mainfrm's table. The two can therefore differ in
  presentation — never in which vk they name.
- imgui_panel_commands.cpp builds its rows once on first open - shows stale
  bindings after a runtime keymap-profile switch (pre-existing staleness class;
  rebuild on table generation change when next touched).
- mainfrm.cpp comment text is double-encoded UTF-8 throughout (pre-existing,
  whole ported file; displays as mojibake in UTF-8 editors). Comment-only.
  Do NOT bulk-fix casually: shell pattern args can alias the byte sequences
  (see Phase-2 review near-miss). If ever normalized, do it file-wide with
  explicit byte-level tooling and no other pending changes.

### Shakeout B (3D-first layout, Windows menu, Delete key, XY declutter)

- **The dock ini name was bumped `kiwi_dock3.ini` -> `kiwi_dock4.ini`.** That is
  what makes the new 3D-first default layout actually appear; the cost is that
  any dock arrangement a user had built under `kiwi_dock3.ini` is not migrated
  (the old file is left on disk, unread). Same trade the 2->3 bump made.
- Per-window visibility (`kiwi_windows.cpp`) is stored in `kiwi_radiant.ini`
  `[KiwiWindows]`, which is INDEPENDENT of the dock ini. Deleting one and not
  the other gives a window that is "open" with no remembered dock node — it
  lands in the dockspace root, which is the intended fallback but is not where
  the user left it.
- **A closed viewport's RTT render is skipped, so its render target keeps the
  LAST frame it drew.** Re-opening shows that stale image for exactly one tick
  (the cell size is re-recorded by the Begin, and the render that consumes it
  runs at the top of the NEXT tick). Cosmetic, self-healing, and it is the same
  one-frame lag every RTT resize already has.
- A viewport docked as a BACKGROUND TAB still renders: `Begin` returns false but
  the cell size it recorded last time it was visible is not cleared, so the flag
  says "open" and the RT is still produced. Only fully CLOSED windows are free.
  Deliberate — zeroing on `!open` also fires during dockspace layout transitions
  and would flicker.
- The five Windows-menu items are registered in `g_radiantCommands` (so the §15
  palette lists them and radiant.ini can rebind them) and are UNBOUND by
  default. `Radiant_ShowMenuItemKeyBindings` appends a bare "\t" to any menu item
  whose command is unbound — faithful to the binary, but it means that after a
  runtime keymap-profile switch (the one path that re-runs the annotator with
  the Windows popup already built) those five captions gain a trailing tab, i.e.
  extra right padding. Boot is clean: the popup is appended AFTER the annotator.
- The Windows popup is appended to the END of the menu bar. `FillTextureMenu`
  and the MRU insertion both address top-level popups by INDEX
  (`GetSubMenu(menu, 5)` texwnd.cpp:1600, `GetSubMenu(menu, 0)` qe3.cpp:766) —
  appending cannot disturb either, but INSERTING a popup anywhere else would.
- **`ImGuiShell_HandleMessage`'s key arm was narrowed from
  `io.WantCaptureKeyboard` to `io.WantTextInput`.** Consequence: a key that ImGui
  itself is consuming for something OTHER than text (a keyboard-driven slider,
  nav) now also reaches `Radiant_TryHotkey` from the frame WndProc. Acceptable
  because the pump-side gate (`Radiant_PreTranslateMessage`) has always used the
  narrow rule, so the two paths now agree instead of disagreeing; and because
  nav is off (`ImGuiConfigFlags_NavEnableKeyboard` is not set).
- **Delete works in the MODERN keymap only.** The classic profile keeps stock
  Radiant's meaning for `VK_DELETE` (`ZoomIn` 32995, View->Zoom->XY Zoom In) and
  its Backspace binding for Delete Selection, untouched. In modern, ZoomIn moves
  to Shift+Delete and BOTH Backspace and Delete delete.
- Delete Selection now has TWO rows in `g_radiantCommands` (the base 33003 row
  and the "KiwiDeleteSelection" alias). The §15 palette dedups by id and shows
  one; `imgui_panel_commands.cpp` mirrors a listbox and shows BOTH, the same way
  it already shows the binary's duplicated "Patch TAB" (33089).
- **XY DECLUTTER IS A SANCTIONED DEVIATION FROM THE BINARY** (user directive:
  "try to cleanup the clutter when zoomed out (deviate from original in this
  regard)"). Three suppressions, all fenced `// KIWI-UX DEVIATION` in xywnd.cpp,
  all one-directional (they only ever REMOVE things at far zoom):
  block labels need >= 48 px per 1024-block, block lines >= 12 px, and the
  coordinate-ruler label step doubles until labels are >= 40 px apart. Anything
  at or above view scale 0.625 is bit-identical to before. If a 1:1 comparison
  against CoD4Radiant is ever run at far zoom, these three WILL differ.
- **The XY camera icon is red, ~1.5x bigger, fixed SCREEN size and 2 px wide** —
  also a sanctioned deviation. It no longer scales with the view, so at extreme
  zoom-in it is smaller relative to the map than the binary's (which is the
  point). `DrawZIcon` (the blue Shift+click marker) is deliberately NOT changed:
  it marks a world position and it is now the only blue marker, which is what
  distinguishes the two.
- The **Z view's** camera marker (`Z_DrawCameraMarker`, z.cpp:371) was checked
  and left alone: its horizontal extent is already view-relative (`z_width/4`)
  and the Z view is OFF by default now. Its vertical extents (camZ +/-8, -48)
  are still world units and will shrink with `z_scale` — give it the same
  treatment if the Z view is ever brought back into normal use.

### Shakeout C (creation suite, infinite grid, View menu)

User directives, verbatim: *"How am I supposed to create a new brush in the
3dcam window? There's no line tools like in plasticity. I want ALL the line
tools (shift-a to create a line, go look all of them up and dont half ass
it)"* · *"The grid does not go to infinity (I want it to go to infinity, and
add a disable grid option in the View Dropdown at the topbar)"* · *"the grid
lines get yucky at the far edges of the viewport."*

The Plasticity command inventory this round was derived from, and the
ship-now / later / never map, are in **RADIANT_UX_DESIGN §16b** (with file and
line cites into `plasticity/`). What follows is only what a user or a future
audit could trip over.

- **Shift+A displaces `SelectAllOfType` (33093) to Shift+Alt+A** in the MODERN
  profile only. vk 0x41's full occupancy was audited (mainfrm.cpp:1000/1031/
  1148/1149/1150) and mods 3 was the one free A chord. Shift+Alt+A shares the
  Alt-chord limitation already logged above (WM_SYSKEYDOWN is not routed to the
  hotkey table in the ImGui shell) — Select All Of Type stays reachable from the
  menu, the palette and the whole classic profile.
- **The KIWI instant-id block 34001..34029 is now FULL.** The next instant
  command must open a new block out of the 34100..34999 headroom; it may NOT
  take 34055+, because `KiwiCmd_Dispatch` tests `KiwiCmd_IsModalId` first and
  anything in 34030..34069 is routed to `KiwiCmd_Start` regardless of what it
  is. `kiwi_command.h` says so at the allocation table.
- **Polygon / spline are stored as closed `KCON_POLYLINE`s**, not as new store
  types — so the `.kiwi` sidecar format and its version are unchanged, and
  neither shape is re-editable after it is placed (you redraw it). If a later
  phase wants parametric editing of either, that IS a sidecar format change.
- **Solid primitives land on the LEGACY power-of-two grid, not the modern one.**
  All three ported primitives finish with `Brush_BuildWindings(def, 1)`
  (brush.cpp:3475 / 3691 / 3786), and bFull 1 is `Brush_SnapPlanepts`. The
  PLACEMENT is snapped to the §17 modern grid, then the ported rebuild
  re-quantises it. Fixing that means editing ported logic, which this layer does
  not do. Box is unaffected (it goes through §23's writer, which rebuilds with
  bFull 0).
- **Cylinder needs an axis-aligned construction plane (XY/XZ/YZ); Cone needs
  XY.** `Brush_MakeSided` takes a WORLD axis (brush.cpp:3381) and
  `Brush_MakeSidedCone` fixes the apex at +Z (brush.cpp:3681-3683). Both commands
  refuse with a console message rather than silently building something pointing
  elsewhere. Sphere accepts any plane (it fills a cube).
- **Cone and Sphere land BEFORE they are validated.** Both ported cores gate on
  `QE_SingleBrush()` (qe3.cpp:362) and read `selected_brushes.next->def`, so they
  are only reachable through the live selection. A failed §19 gate is rolled back
  through `KiwiCmd_UndoCancel()`. Box and Cylinder keep the §23 build-gate-land
  order, where a rejection costs nothing.
- **`Brush_MakeSidedSphere` builds sides×sides faces** (brush.cpp:3736), which is
  why the sphere's side count defaults to 8 (64 faces) and is capped at 12 (144).
  Cylinder/cone default to 16 — the number `Brush_MakePhysCylinder` itself
  hardcodes (brush.cpp:3589).
- **`[` and `]` are shadowed while a polygon / cylinder / sphere / cone gesture
  is running** (they adjust the side count). Their modern grid-spacing meaning
  returns the moment the command ends. This is the ordinary §4 rule — a modal
  command owns its keys — but it is the first time a KIWI command has taken a key
  that the modern profile also binds.
- **The grid's footprint is now the ground-plane frustum hit**, not a camera-
  height square, and its hard cap is ±131072 (the engine's own world bound,
  brush.cpp:1459). The old heuristic survives only as the fallback for a
  degenerate camera. Worst-case segment count was re-derived: 2 × 201 grid + 6
  axis = **408**, still under `KGRID_MAX_SEGMENTS` (440). The world-axis lines
  were lengthened from 65536 to 131072 to match, or they would have become the
  visible edge instead.
- **Far bands drop MINOR grid lines entirely** (bands 4 and 5 of 6), and minors
  whose projected spacing falls under 3 px at their nearest point are skipped.
  Both are one-directional — they only ever REMOVE lines — but they mean the far
  field shows a 10-cell lattice rather than every cell. That is the fix for
  "yucky at the far edges", not a bug.
- **"Show Grid" / "Show Axes" are appended INTO the existing View popup**
  (index 2), not into a new top-level popup. Appending ITEMS cannot shift the
  menu bar's POPUP indices, which is all the index-based consumers read
  (`texwnd.cpp:1600/1651/1665/1679` index 5 = Textures; `radiant_main.cpp:529`
  and `qe3.cpp:766` index 0 = File/MRU). Adding or removing a POPUP would; the §9
  "Windows" popup is appended after Help, so it cannot either. The check marks
  are kept in step from BOTH UIs — the native items and the shell checkboxes
  write the same `KiwiUX_*` flag and both call `KiwiWindows_SyncViewMenu()`.
- **The modal command's overlay budget went 64 → 192 segments per gesture**
  (`KiwiCmd_DrawWorld`). The §16b previews are the first that legitimately want
  more (a sphere preview is three 24-segment great circles); 64 truncated them
  mid-shape, which reads as a rendering bug rather than as a budget.
- **Staged tools now clear the typed value between stages** (arc, and all four
  primitives). Before this round, typing a radius at stage 1 of the arc and then
  clicking silently reinterpreted that number as the stage-2 SWEEP IN DEGREES.
  The framework resets the numeric buffer per COMMAND, which is right for a
  one-scalar gesture and wrong for a staged one.

### Shakeout D (gizmos on G/R, Ctrl+digit conversion, edge-only selection, real cube)

- **The gizmos are MODAL CHROME now — nothing is drawn on an idle selection.**
  USER DIRECTIVE: "The Move gizmo should show up when pressing G, nothing should
  show up by default." The visibility rule inverted: the move arrows exist only
  while `KiwiXform_IsMoveActive()`, the rotate rings only while
  `KiwiXform_IsRotateActive()`. The persisted "Transform gizmos" checkbox is now
  an ADDITIONAL gate rather than the main one. Consequence worth knowing: the
  gizmo can no longer START a move — you press G (or pick Move from the palette)
  and *then* grab a handle. That is the Plasticity shape, and it is what makes
  "nothing by default" possible at all.
- **The LMB-down order changed in `KiwiVP_CameraButtonDown`.** The gizmo hit test
  now runs FIRST, above the active-modal-command arm. It had to: that arm commits
  the live command on ANY left press, so with the old ordering a press on a handle
  ended the gesture before the handle could be tested. The test is guarded by
  `KiwiCmd_Active()`, so the no-command path does not run one projection.
- **`R` spawns three rings; a held ring measures a real angular sweep.** The ring
  drag projects the cursor ray onto the ring's plane and accumulates the angle
  about the pivot (unwrapped across ±180, so multi-turn drags keep counting),
  then feeds it to `KiwiRotateCommand` as DEGREES. Free R drag — no ring grabbed
  — keeps the old 0.5°/px horizontal rule. The 5° snap, the numeric override and
  the HUD are inherited unchanged because the ring only replaces the *source* of
  the command's one scalar.
- **The rotate feed is NEGATED, and that is not a bug.** `Select_RotateAxis`
  builds its matrix from `Ed_SinCos(-deg, ...)` and it is applied row-vector-wise
  by `OrientationPosToWorldPos`, so a positive `deg` turns the geometry the
  NEGATIVE way about the axis. The derivation is written out at the top of
  `kiwi_gizmo.cpp`; the ring feeds `-sweep` so the solid follows the cursor.
- **Grabbing a ring resets the angle to zero at the grab point.** Any degrees the
  free drag had already applied are rolled back by the command's own
  apply-from-baseline residual. Deliberate: the alternative is the geometry
  jumping the instant a ring is touched.
- **The gizmo segment budget went 48 → 192** (`KGIZMO_MAX_SEGMENTS`): three
  48-segment rings are 144. `KiwiCmd_DrawWorld`'s own 192-segment gesture batch is
  UNCHANGED — the rings live in the gizmo's batch, not the command's.
- **Selecting an edge or a vertex no longer selects the whole brush.** USER
  DIRECTIVE. `Sel_SyncToLegacy` pass 1 used to PROMOTE an edge/vertex item's owner
  brush onto `selected_brushes` (so the ported `SetupVertexSelection` handles had
  something to build from), which also lit the whole brush up. Only `SEL_OBJECT`
  promotes now. Consumers checked before cutting: `kiwi_transform`'s
  BeginEdges/BeginVerts and `kiwi_bevel`'s edge path read `KiwiSel().items`
  directly and cover their brushes with `UndoCoverBrush`, exactly as the FACE path
  already did; `SetupVertexSelection` is no longer called at all (it walks
  `selected_brushes` and would now always produce an empty handle list, and its
  only consumers are legacy draw/drag paths the modern layer does not use).
- **R and S now REFUSE a pure edge/vertex selection.** They are whole-object ops
  and their predicates test `selected_brushes`, which is empty for a fine
  selection. That is the correct refusal — before this round they would silently
  rotate the entire brush the edge belonged to.
- **A fine selection draws its own accent, and it is NOT under the hover toggle.**
  With no promotion there is no red brush outline, so every selected
  edge/vertex/face item is drawn amber (dimmer than the active item) by a new pass
  in `kiwi_hover.cpp`. Cap: **300 segments total**, thick pass (edges/verts) before
  thin (face outlines), items walked NEWEST-FIRST — so what gets dropped at the cap
  is always the OLDEST items in the selection. Turning "show hover highlight" off
  must not make a fine selection invisible, so the accent pass runs above that gate
  and takes over drawing the active item when hover is off.
- **`Sel_RebuildFromLegacy`'s carry rule changed with the promotion.** It used to
  carry edge/vertex items only while their owner was still on `selected_brushes` —
  which only ever worked *because* of the promotion. It now carries any item that
  still resolves (liveness-guarded), and the one case that must drop them — an
  explicit deselect — is announced by a new `Sel_NoteLegacyDeselect()` hook at
  `Select_Deselect`. It is announced rather than INFERRED from an empty legacy
  list, because an edge-only selection legitimately leaves that list empty.
- **Ctrl+1..4 convert the selection** (points / edges / faces / objects), ported
  from Plasticity's `SelectionConversionStrategy.ts`. Two divergences worth
  knowing: KIWI's Ctrl+1 is REAL (Plasticity binds `selection:convert:control-point`
  to a command it never registers, so it is dead there), and KIWI also sets the
  matching MODE MASK, because a converted selection with the picker still in the
  old mode would be thrown away by the very next click. Ctrl+2 / Ctrl+3 on a
  PATCH-only selection is the documented empty case: patches have no plane faces
  and no winding edges here, so the selection is KEPT and the console says why.
- **Instant command ids opened a SECOND block at 34100**, and the mainfrm dispatch
  gate was widened `34000..34099` → `34000..34199` to reach it. The first instant
  block (34001..34029) filled up in shakeout C, and the free tail of the MODAL
  block could not be used: `KiwiCmd_Dispatch` tests `KiwiCmd_IsModalId` first, so
  any id in 34030..34069 is routed to `KiwiCmd_Start` whatever it is.
- **Ctrl+1..4 displaced NOTHING.** Audited: vk 0x31..0x34 carry only SetGrid1/2/4/8
  at mods 0 in the default table (mainfrm.cpp:1037-1041) — which this profile
  already unbinds to free the bare digits — and `res/radiant.rc:490-501` has no
  Ctrl+digit accelerator. First modern-profile claim in the whole overhaul that
  needed no two-step displacement.
- **The view-cube is an actual cube.** USER DIRECTIVE ("the 'blender cube' you
  gave me is ugly"). Eight corners projected orthographically through the camera
  basis, six quads painter-sorted by `dot(normal, vpn)`, visible faces labelled
  X / -X / Y / -Y / TOP / BOT. **14 snap targets**: six faces plus eight CORNERS
  (isometric views), corners tested first because they are small targets sitting on
  top of three faces. The six-row pitch/yaw table is gone — the general derivation
  `pitch = asin(vpn.z), yaw = atan2(vpn.y, vpn.x)` reproduces every row of it and
  is what the corner views need anyway. The widget grew 90 → 104 px so a cube
  corner (`sqrt(3) * 29 px`) can never spill out of its own backdrop.
- **The twelve EDGE views of the cube are NOT wired.** They would need a third hit
  region between the corner discs and the face quads on a 104-px widget, and an
  edge view is the one orientation nobody reaches for. Future work, logged rather
  than crammed in.
- **Every whole-object legacy op is a no-op on a pure edge/vertex selection now**,
  not just R and S — Delete, Clone, Hide, the CSG rows, the flips. They all read
  `selected_brushes`, which a fine selection no longer populates. Ctrl+4 (convert
  to objects) is the one-keystroke way back, which is exactly why it is bound and
  why it is advertised in the hint panel for the Edge and Point kinds.

## Shakeout E (2026-08-09) — confirm flow, Tab fields, RMB pan, snap glyph

- **AN LMB RELEASE NO LONGER COMMITS A MODAL COMMAND — IT PAUSES IT.** The
  single biggest behavioural change in the overhaul so far. User directive:
  *"Releasing an action shouldn't commit it, it should just pause the wip move.
  A right click OR an enter press confirms it."* Framework-level, in
  `kiwi_command.cpp`, so **every** modal command inherits it: a live command is
  HOT (geometry follows the cursor) or PAUSED (preview frozen, value latched,
  `KiwiCmd_MouseMove` returns before the pick and snap queries). An LMB press
  pauses, any LMB press resumes, RMB-click or Enter confirms, Esc still cancels.
  The full state table is in `kiwi_command.h` and in RADIANT_UX_DESIGN §4.
- **RMB-CLICK IS NOW A CONFIRM while a command is live** (decision D-2, reopened
  and resolved). RMB *drag* is still camera; only a no-drag release
  (< `KVP_RMB_CLICK_PIXELS` = 4 px travel — the same threshold and the same code
  path shakeout A used) confirms, and the legacy entity context menu is not
  replayed for it. With no command live, RMB click is the context menu exactly as
  before.
- **RMB drag is PAN now; the mouselook moved to Alt+RMB** (user directive).
  Shift+RMB and Shift+MMB stay pan, plain MMB stays orbit. Alt is read from
  `ImGui::GetIO().KeyAlt` inside `kiwi_viewport.cpp` rather than plumbed through
  `KiwiVP_CameraButtonDown`'s signature — it is the same `io` the shell reads
  `KeyShift`/`KeyCtrl` from two lines away (imgui_shell.cpp:342), not a second
  source. **Audited:** nothing else in the modern camera layer binds Alt+RMB.
- **THE PAN SENSITIVITY BUG WAS THE PIVOT.** `KiwiCam_PanDrag` scaled by
  `KiwiCam_WorldPerPixel( s_lookAt )`, and `s_lookAt` is the ORBIT pivot — only
  re-seated by an orbit, a dolly or a fly. Pan along a wall and it stays parked at
  its old depth; dolly close and `PivotOnAxis` puts it at the clamped `s_dist`,
  which can be a fraction of (or several times) the real depth under the cursor.
  World-per-pixel is LINEAR in that depth, so pan speed swung by the same factor —
  on a quantity the user cannot see. `KiwiCam_PanBegin` now does **one** area pick
  at the press and caches world-per-pixel at the hit point for the whole gesture
  (pivot on a miss). The grabbed point tracks the cursor ~1:1.
- **The dolly's step reference is smoothed across consecutive notches.** Crossing
  a silhouette edge mid-scroll used to step the step size (15% of 40 units, then
  15% of 4000). Notches inside 0.35 s blend 50% toward the new reference; a
  restarted dolly takes it outright. **The punch-through clamp still uses the TRUE
  hit distance**, not the blended one — a blended reference can sit beyond the
  wall.
- **`KiwiNum_Reset` now also reinstalls the DEFAULT field table**, which made
  `kiwi_primitive.cpp`'s `ClearNumeric` (it called Reset on every stage advance) a
  latent field-table wipe. It now calls the new `KiwiNum_ClearEntry`. Any future
  staged command must do the same — Reset is a per-COMMAND operation.
- **`NumericChanged` was NOT given a defaulted field parameter.** A default
  argument does not make an override match, so all seven existing
  `void NumericChanged(bool,float) override` sites would have stopped overriding
  and failed to compile. The framework calls a new
  `NumericFieldChanged(int,bool,float)` whose default body forwards field 0 to the
  old hook — every pre-shakeout-E command is untouched and byte-identical.
- **Field KIND is a formatting fact only.** `KiwiNum_ValueWorldField` still applies
  `Units_FromDisplay` for every kind, including ANGLE and COUNT, so R / S / the
  array / the texture commands keep undoing it with `Units_ToDisplay` exactly as
  they always did. Changing that would have been a silent arithmetic change in
  seven files for a cosmetic gain.
- **The editable Tab "angle" field exists only on Line and Polyline.** They are
  the two drawing tools whose current segment has an unambiguous in-plane bearing.
  For rect / circle / polygon / arc / spline, `m_cur` is a corner, a radius
  endpoint or a curve control point, and "the angle of that" would mean a
  different thing in each — so they expose `length` only.
- **The value bubble shows nothing for Bevel and Inset until you type.** Their
  live scalar lives in the subclasses with no shared member to report, so
  `NumericFieldValue` is not overridden there; the verbose bottom HUD still
  carries the live number. Wiring it is a two-line change per subclass if it
  proves annoying.
- **The snap marker lost its per-type colours.** Every marker is now near-black
  (`KSNAP_COL_MARK`), and the POINT ranks share Plasticity's dot-and-ring glyph —
  including SNAP_INTERSECTION, whose "X" is gone. `kiwi_lines` has no alpha
  channel, so "near-black with slight alpha" is approximated with a very dark
  grey; pure black reads as a hole punched in dark geometry. Type differentiation
  is now **only** in the label text (which kept its per-rank tint).
- **The rotate ring's release used to JUMP and nobody could see it.** Handing the
  angle mapping back to the pixel rule (`RingFeed(false, …)`) made `Recompute`
  re-derive `deg` from `(x - m_startX)`, a pixel origin latched at `Begin` with no
  relation to the swept angle. It was invisible while a ring release committed
  instantly; with the release now PAUSING it would have been left on screen.
  `RingFeed` re-latches through the new `Rebase()` first.
- **`Rebase()` is implemented for G / R / S and Extrude only.** Bevel, inset, the
  array and the texture commands have no delta-from-start mapping worth
  re-latching, or map from an absolute cursor position that resuming re-reads
  correctly anyway. If one of them turns out to jump on resume, the fix is one
  `Rebase()` override, not a framework change.

## Shakeout F — Plasticity creation chords, construction selection, region flash, snap points

- **`Shift+A` no longer opens the add menu.** User directive: *"the shift-A menu
  is unacceptable. Shift-A is for LINES."* All eight Plasticity creation chords
  are bound in the modern profile (`shift-a/s/q/w/z/x/c/v`), and **five
  compiled-in commands were displaced** to make room —
  `TogglePatchWireframes` → Shift+Alt+W, `ToggleCrosshairs` → Shift+Alt+X,
  `CapCurrentCurve` → Shift+Alt+C, `VehicleGroup` → Shift+Alt+V, and
  `SurfaceInspector` **moved a second time**, from Shift+S (shakeout C) to
  **Alt+S**. The full occupancy audit per key is in `kiwi_keymap.h`.
- **`SurfaceInspector` is on an Alt chord and Alt chords do not reach the hotkey
  table** (`WM_SYSKEYDOWN` is not routed in the ImGui shell — the pre-existing
  limitation logged above). It is the only chord left on `S`: bare S is Scale,
  Shift+S is the spline tool, Shift+Alt+S is the Patch Inspector, Ctrl+S is Save
  and Shift+Ctrl+S is MakeStructural. The Surface Inspector stays reachable from
  the Textures menu and the palette.
- **`Shift+V` is a documented DEVIATION from Plasticity.** Plasticity binds it to
  `command:center-box`; KIWI has no centre-box primitive (§16b.3 lists it as
  LATER), so Shift+V takes **Rectangle (centre)** instead. When the centre box
  ships it will want that chord, and the rectangle will have to move.
- **`Join` is `Ctrl+J`, not the bare `J` Plasticity uses.** Bare J is
  `ToggleOutlineDraw` in stock Radiant, a display toggle mappers use constantly;
  Ctrl+J was free and costs no displacement.
- **Construction geometry has a SECOND selection, parallel to `selection_t`.**
  Scope ruling 1 (`kiwi_construct.h`) is intact — `sel_item_t` still has one
  addressing mode and construction items never enter `selection_t` or
  `Sel_SyncToLegacy`. The consequence is that **both selections can be non-empty
  at once**, and each command reads whichever list its own logic names. All three
  construction verbs (Join / Delete / Move) are gated on a **pure** construction
  selection so that ambiguity never has to be adjudicated at runtime.
- **Delete is arbitrated in `KiwiUX_KeyFunnel`, not inside command 33003.** With
  ANY brush selection — the legacy sentinel non-empty *or* `KiwiSel()` non-empty
  (an edge-only selection appears only in the latter) — the funnel arm declines
  and the key falls through to the ported Delete Selection untouched.
- **A construction click BEATS an area (face/object) brush pick.** `Test_Ray` hits
  report `screenDist == 0`, so a line drawn on a wall would otherwise be
  unclickable. The cost is a 6-pixel ribbon along each construction segment where
  the wall behind stops being selectable — exactly as wide as the ribbon a brush
  edge already steals.
- **The construction selection is DROPPED, never remapped, when the store is
  rearranged.** Store objects are addressed by position and have no identity, so
  `KiwiCon_RemoveAt` / `ClearAll` / `UndoPop` clear it. Appending does not
  renumber anything, so drawing a new line while two are selected keeps them
  selected — which is the case that matters, because it is how you get to Join.
- **`G` on construction geometry is WHOLE-OBJECT only.** A selected point or
  segment still drags its whole object. Per-point editing needs the store to grow
  a point-level edit AND the drawing tools to agree what a moved point means for a
  PARAMETRIC circle/arc — bigger than a shakeout round.
- **A construction move translates the object's PLANE ORIGIN, not its points.**
  That is the only spelling that cannot lose coplanarity (ruling 3): resolving the
  delta into u/v would silently drop whatever ran along the normal. An object
  therefore stays on its own plane, moved to a parallel one.
- **`R` and `S` still refuse a construction selection.** Rotating and scaling
  scaffolding needs a pivot story nobody has asked for yet.
- **Region-on-any-plane was NOT a plane-keying bug.** `SamePlane` derives the
  plane constant from the origin every call (no stored `d` to drift), the chain
  loop is re-projected through world into the group plane, and
  `KiwiRegion_PickAt` / the extrude drag are plain plane math with no axis
  assumption. The real bug was in `KiwiCon_AutoPlaneForTool`: **every drawing tool
  re-derived its plane from the face under the cursor at Begin**, so starting line
  2 from line 1's endpoint over a *different* face silently changed planes and the
  two lines were never coplanar. A drawing tool now inherits the plane of the
  construction object under the cursor when there is one, and falls back to the
  face otherwise.
- **PASS 2's three-edge floor was rejecting real loops.** Two arcs, or two
  polylines, can close a region; the floor is now two, and the `KREG_MIN_AREA`
  gate is what rejects the degenerate two-straight-segments case.
- **The region flash is FRAME-counted and gated on the region COUNT growing.** A
  wall-clock flash would keep animating while the editor is idle and not
  redrawing. The count gate exists because a construction MOVE re-derives regions
  every frame with every centroid in a new place — without it the same untouched
  region would "form" sixty times a second.
- **`SNAP_FACE_CENTER` is BRUSH FACES ONLY.** A patch has no winding and its
  "centre" is a tessellation question, not a geometry one. It also costs a fourth
  `Pick()` per snap query (the `SEL_MASK_FACE` mask is what makes `Test_Ray`'s hit
  resolve to a `faceIndex` at all) and is suppressed while a drawing tool runs.
- **The relative angle snap prefers the IN-PROGRESS chain.** A drawing tool
  commits its object at Finish, not per click, so the segment the user just placed
  is not in the store; without that first rung a polyline could never turn 45° off
  its own previous segment. Below it, construction segments and brush edges
  compete on equal terms for the nearest line within the weld tolerance.
- **The relative angle snap keeps the 15° increment.** The directive names
  0/45/90/135; those are all on the 15° ladder, and a second increment constant
  would be a second rule for the same thing.
- **`KiwiRegion_DrawFills` is now the only legal per-frame caller.** It ages the
  flash counters, so calling it twice in a frame would halve the flash duration.

## Shakeout G — modelling verbs, gizmo-only transforms, movable pivot, face fill

- **Move and Rotate now change NOTHING from a bare mouse move.** R's
  0.5-deg-per-pixel mapping is deleted outright; G's four cursor mappings survive
  but are gated on a HELD gizmo handle (`KiwiXform_NoteMoveGrab`, fed from
  `kiwi_gizmo.cpp`'s press/release edges). The **snap arm is gated with them** —
  a geometry snap firing with nothing held would drag the reference point onto a
  target, which is the same bug in a different costume.
- **`S` still free-drags, deliberately.** It has no handle set (the gizmo draws
  arrows for Move and rings for Rotate and nothing for S), so gating it would
  leave scale reachable only by typing a factor. **Scale-handle debt** — when S
  gets handles it gets the gate and the `V` pivot with it (Plasticity already
  binds `keyboard:scale:pivot`, `default-keymap.ts:159`).
- **Face push/pull needed a NORMAL ARROW to survive the gate.** The unlocked push
  runs along the drive face's own normal and previously had no handle — the user
  had to grab the CENTRE square, which reads as "free move" everywhere else.
  `KGZ_NORMAL` (amber, 1.25x the world arrows so it wins on an axis-aligned face)
  is what makes the gated push reachable at all.
- **The pivot's reset rule is a SELECTION SIGNATURE, not `Sel_Generation()`.**
  The generation bumps on every `Sel_SyncToLegacy`, including syncs that end with
  the same things selected, so keying on it would drop the pivot between the two
  commands the user explicitly asked to share it. The signature is item count +
  a mix of each item's (instance pointer, kind, indices) + the
  construction-selection count. **Known limit:** it is a hash, so two different
  selections could in principle collide and keep a stale pivot; the cost is a
  gizmo drawn in the wrong place until `V` is pressed again.
- **The pivot is NEVER persisted.** No ini key, no profile entry, gone on exit —
  the directive says "in memory (not disk)" and restoring one from a previous
  session would silently aim a rotate at a point nobody remembers choosing.
- **The gizmo must stand down while the pivot is being placed.** Not cosmetic:
  `kiwi_viewport.cpp:292` offers an LMB press to `KiwiGizmo_MouseDown` BEFORE
  `KiwiCmd_MouseButton` at `:311`, so a live gizmo swallows the placing click and
  grabs a handle instead. `GizmoUsable()` returns false during placement, which
  is also what Plasticity does (`gizmo.disable()`, `TranslateCommand.ts:316`).
- **`PressIntercept` is a one-press veto, NOT `WantsClicks`.** `WantsClicks`
  changes a command's whole grammar (it opts out of pausing entirely and every
  click becomes a `Click()` event); the pivot needs exactly one press to mean
  something else for the handful of frames a placement is live. Default body is
  false, so every pre-shakeout-G command is byte-identical.
- **Tab reaches a command only when the numeric layer has <= 1 field.**
  `KiwiNum_Key` consumes VK_TAB unconditionally (`kiwi_numeric.cpp:238-242`,
  above its own modifier gate), so a new rung 0 in `KiwiCmd_KeyDown` offers it
  first when there is nothing to cycle. The two-field commands (construction
  tools with an angle field, `kiwi_construct.cpp:780-789`; ring primitives,
  `kiwi_primitive.cpp:173-182`) are unaffected, and no one-field command
  overrides `KeyDown` to take Tab — so only `Ctrl+R`'s U/V flip changes.
- **The face FILL emits nothing into the line batches.** That is the point of the
  directive: a face accent must not look like an edge one. Selected/hover/active
  faces are translucent fans (`R_AddRenderCmdDrawTris`, neutral MATERIAL_COLOR
  then white again) nudged `-vpn * 0.50` — twice the outline nudge, because a
  fill loses AREA to z-fighting where an outline lost a one-pixel sliver.
  **Caps:** 64 points per winding and 64 fills per frame (each fill is its own
  draw command); a Ctrl+3 conversion of a large selection degrades oldest-first.
- **Mode 5 does NOT auto-enter push/pull.** The mask must be FACE and nothing
  else. In mode 5 a face click is one of four things it could have meant, and
  starting a modal command off an ambiguous pick loses a selection in progress.
  Shift/ctrl clicks do not auto-enter either.
- **Once auto-entered, another face click RESUMES the move instead of selecting
  that face.** That is shakeout E's own resume rule (any LMB press in the
  viewport resumes a paused gesture) meeting the new auto-entry. Esc or RMB gets
  out first. Deliberate, not overlooked — narrowing the resume rule needs a pick
  against a preview that may not be geometry yet, which shakeout E already
  rejected for good reasons.
- **The first-mutation guard was NOT actually honoured on the face/edge/vertex
  paths.** `ApplyObjects` had its `Len3(d) <= KX_EPS` early-out; the other three
  opened the bracket and then wrote the baseline back over itself, so a gesture
  started and escaped without moving left an EMPTY undo record. Harmless when G
  had to be pressed deliberately; unacceptable now that clicking a face starts
  one. All three now return before `OpenUndoForBrushes` while the delta is zero.
- **A push-through delete lands in the gesture's OWN record**, labelled "push
  face". One gesture, one record — and the alternative (cancel the push record,
  open a delete one) would run `Undo_Undo` mid-gesture, which deselects
  everything and re-links brush CLONES, freeing the very nodes the command is
  holding.
- **The delete threshold is measured ONCE, at Begin.** `faceUnit_t::depth` is the
  brush's thickness along that face's baseline normal over the WHOLE brush's
  winding points (the far side is on some other face). Measuring per frame would
  read geometry the gesture has already deformed.
- **`Select_Delete` takes no arguments** — it deletes whatever is on
  `selected_brushes` (`select.cpp:1504-1526`) and additionally frees an owner
  entity left with no brushes, which is why `Undo_AddEntity_W` is fed first,
  exactly as `Cmd_OnSelectionDelete` (mainfrm.cpp) does.
- **Cut's plane FREEZES on pause.** While HOT the orientation re-derives from the
  live camera every MouseMove; a pause latches it so the user can orbit to
  inspect the preview. No second flag exists: `KiwiCmd_MouseMove` returns early
  when paused, so "re-derive in MouseMove" already means "only while hot".
- **Cut refuses a line pointing at the camera.** `away = vpn - u*(vpn.u)`
  collapses and there is no such plane. Orbit and retry.
- **Match Face applies TWICE on a good pick, once on a bad one.** §19 says a
  rejection must not touch undo, and the bracket says `Undo_AddBrush` must run
  before the first mutation. For a one-shot op those pull opposite ways, so the
  plane is applied as a TRIAL first (no bracket, no texture lock) and rolled
  straight back either way; only a passing trial opens the bracket and runs the
  real, texture-locked apply.
- **Join's face arm is a CSG merge, and it reaches it through the CLASSIC id
  32927.** `CSG_Merge` takes no arguments — its contract is "merge everything on
  `selected_brushes`" (`csg.cpp:572`) — so the only decision is what is selected
  when it runs. Every refusal (fixed-size entities, patches, different owner
  entities, non-convex hull) is CSG_Merge's own and it re-adds the originals on
  its own failure path.
- **Join's coplanarity test is the SAME-INFINITE-PLANE one, on magnitudes.** Two
  brushes stacked against each other share a plane whose outward normals point AT
  each other; requiring `dot > 0` would reject the one arrangement the verb
  exists for. `|dot| > KVALID_PLANE_DOT` and `|d1 - sign*d2| < KVALID_PLANE_DIST`,
  §19's own constants.
- **Plasticity CANNOT join faces.** `j` is curves only (`default-keymap.ts:258`
  -> `JoinCurvesFactory.ts:22`), `MergerFaceCommand` is an empty body
  (`ModifyFaceCommand.ts:141`) and `UnitedFaceFactory` has no reference outside
  its own definition. The face arm is KIWI's own reading of the directive; the
  line arm ports a real verb.
- **Plasticity has NO match-face verb at all**, and its `SplitFactory`
  (`CutFactory.ts:176`) is unreachable from its UI. Z and Ctrl+R are KIWI's, and
  Ctrl+R deliberately splits the BRUSH rather than dividing a face: a classic
  brush is a convex half-space intersection and cannot carry a divided face.
- **`E` extrudes to a SEPARATE brush; Plasticity unions.** Its extrude is a
  boolean by default (`PossiblyBooleanExtrudeFactory`, `ExtrudeCommand.ts:93`,
  whose targets include the face's own parent solid). A classic brush is convex,
  so a prism unioned onto a solid is two brushes anyway — and `J` is the explicit
  verb for asking for one where one is possible.
- **`E`'s winding sense is TESTED, not assumed.** The prism writer's whole
  orientation argument is written against a CCW ring in (u, v); a brush winding's
  order about its own outward normal is whatever `Brush_BuildWindings`' clipping
  left, so the signed area decides and the ring is reversed when it is negative.
- **`E` is a CONTEXT id resolved in `KiwiExtrude_CommandForId`.** Returning a
  different command object for the same id IS the dispatch — `KiwiCmd_Start`
  calls it before `CanExecute` and before `Begin`, so no framework change was
  needed and the HUD/hint rows automatically describe whichever arm ran. The
  no-face-no-region fallback is the FACE command, whose `CanExecute` is
  unconditional so its `Begin` prints a message naming both arms; returning the
  region command there would refuse silently.
- **`V` has NO keymap row and must not have one.** It is command-local, matching
  Plasticity (bound per gizmo context, never globally). `KiwiUX_KeyFunnel` routes
  every key to the active modal command, so V reaches Move/Rotate without a table
  row and vk 0x56's global occupancy (DragVertices, the centre-rect tool,
  VehicleGroup, Paste, ToggleView) is completely undisturbed.
- **Bare `J` reverses shakeout F's own ruling.** F declined to displace
  `ToggleOutlineDraw` for a chord Ctrl+J gave away free. G takes it because the
  directive names the bare key and because J is no longer lines-only — it is the
  merge key too. `Ctrl+J` is untouched and keeps Join Lines as the alias.
- **`E` collides with the fly, and that is fine.** E is one of the six keys the
  RMB-mouselook fly swallows (`KiwiCam_FlySwallowKey`). The fly's swallow rung
  sits BELOW the active-command arm in `KiwiUX_KeyFunnel` and the fly polls with
  `GetAsyncKeyState`, so E extrudes when the user is not flying and flies when
  they are — the same arrangement S (scale / strafe) has had since shakeout A.
- **The paste/clone hook is a TAIL on the command ids, not a change to the
  handlers.** `map.cpp:569` and `entity.cpp:1873` call `XYWnd_PasteClip` directly
  to carry a selection across File->New; starting a modal command there would be
  nonsense. Both tails self-guard on `KiwiUX_ModernInput`.
- **`KiwiCmd_StartDeferred` exists because `KiwiCmd_Commit` is not finished when
  `Commit()` returns.** `KiwiCmd_UndoCommit` (which would close the NEW command's
  bracket) and `KiwiNum_Reset` (which would throw away its field table) both run
  afterwards. A cancelled gesture drops the request — there is no result to place.

## Shakeout H — world-space construction store, Trim (T), grid polish

RADIANT_UX_DESIGN Part VI (§34-§38) carries the full reasoning and every
Plasticity cite. These are the things a later round would otherwise re-discover.

- **§7 SCOPE RULING 3 IS REVERSED.** The construction store is WORLD-SPACE
  (`kconObject_t::pts` = 3 floats per point) for LINE / POLYLINE / RECT. Circles
  and arcs stay parametric with an authoritative stored plane. Anything that
  reads `o.pts` with a stride of 2, or reads `o.plane` for a point object without
  going through `KiwiCon_ObjectPlane`, is now wrong.
- **`o.plane` on a point object is a CACHE, not data.** It is refit by
  `KiwiCon_Add` and by `KiwiCon_NoteMutated` (which refits the WHOLE store — the
  caller does not always know which objects it touched). `planeValid` says whether
  the fit succeeded. Never read it directly; `KiwiCon_ObjectPlane` does the check
  and answers for the parametric case too.
- **A construction object may legitimately have NO plane.** A non-planar polyline
  is a valid object; it simply never becomes a region and never donates a plane to
  `KiwiCon_AutoPlaneForTool`. Code that assumes every object has a plane will
  silently do the wrong thing rather than crash, which is the worse failure.
- **THE CORNER-SNAP BUG WAS ONE LINE.** `KiwiDrawTool::MouseMove` used to do
  `KiwiCon_WorldToPlane( m_plane, snap.position, m_cur )` — projecting the snapped
  point onto the working plane, i.e. storing a brush corner's *shadow* instead of
  the corner. If a future round adds a placement path, the rule is: **never
  project a geometry snap.** The plane is a fallback for empty space only.
- **`KiwiRegion_ChainWalk` lost its plane parameter** and welds in 3D world
  distance. It had to: the plane it used to weld in is now derived *from* the
  chain the walker is being asked to find, which was circular.
- **`KiwiRegion_SamePlane`, `KREG_PLANE_DOT` and `KREG_PLANE_DIST` are DELETED.**
  Both callers became point-vs-plane questions. §8's coplanarity tolerance now has
  exactly one spelling: `KCON_PLANE_FIT_DIST` (0.5) in `kiwi_construct.h`.
- **Join no longer requires coplanarity.** A 3D chain is a legal polyline; whether
  it is also a region is the region layer's question, answered from the merged
  points on the next frame. Do not re-add the precondition.
- **`KiwiConSel_MoveApply` moves POINTS now.** It used to move only
  `plane.origin`, which under the old ruling translated the whole object and under
  the new one would translate nothing at all. Both the points and the origin are
  written, absolute from the baseline.
- **Sidecar is KIWI2, and KIWI1 still loads.** The reader accepts both headers and
  both `pt u v` (converted through the object's `plane` at `end`) and
  `wpt x y z`. The writer only ever emits KIWI2, so a map saved by this build
  cannot be read by an older one. That is the deal every sidecar bump makes.
- **`Z` inside a drawing tool is a TOGGLE, and it is not a rebinding.** No keymap
  row moved; Match Face keeps bare Z outright. `KiwiUX_KeyFunnel`'s first arm
  routes every key to the active modal command before the hotkey table is
  consulted, exactly as the movable pivot's `V` already relied on. A toggle rather
  than a hold because the funnel is fed `WM_KEYDOWN` only — there is no key-up
  path into a command anywhere in this layer.
- **The two-click Line no longer commits on its second click.** It parks the
  endpoint; another click resumes tracking; RMB/Enter commits. Polyline and spline
  are byte-identical to shakeout G. Implemented inside `KiwiLineTool` rather than
  through `KiwiCmd_Pause`, because a `WantsClicks` command opts out of the
  framework's pause outright (`kiwi_command.cpp` `KiwiCmd_Pause` returns early).
- **Trim pushes ONE store-undo snapshot PER CLICK, and `Esc` does not roll them
  back.** Esc means "stop trimming". Ctrl+Z inside the tool pops one trim at a
  time, which is what the per-click snapshot is for.
- **Trim refuses circles, arcs and rects on purpose.** Trimming a parametric shape
  means converting it to a polyline, which destroys what the user drew. A polygon
  or a spline IS trimmable, because the store keeps both as closed polylines.
- **`SNAP_FACE_CENTER` is no longer suppressed during a drawing tool.** It was
  suppressed *because* of the projection this round removed. The area `SNAP_FACE`
  arm is still suppressed and should stay that way — it fires on every pixel of
  every surface.
- **`SNAP_INTERSECTION` is 3D now** (closest approach under `KCON_ISECT_DIST`,
  0.25) and no longer restricted to segments lying on the active plane. It shares
  `KiwiCon_SegSegClosest` with Trim, deliberately: what trims is what snaps.
- **The grid's minor-line cull has HYSTERESIS and per-line state.** One byte per
  line in a 512-slot table, keyed by `index & 511`, cleared whenever the spacing
  coarsens. The no-collision argument depends on the coarsening loop capping a
  frame at 200 *consecutive* indices per axis — if `KGRID_LINES_PER_AXIS` ever
  exceeds 512, the table must grow with it.
- **The construction-plane grid patch is suppressed on the ground plane.** It is
  not a bug that no patch appears while drawing on Z=0; it was painting a second
  grid on top of the first, which is what "the nearby grid is pink" meant.

### Still open after shakeout H

- **Regions still have no holes and no nested loops** (spec decision D-3). Two
  concentric circles are two regions, not a ring. Unchanged by this round.
- **Trim ignores self-intersections.** A polyline that crosses itself is a knot,
  and the span the user would mean at its own crossing is ambiguous; the crossing
  scan excludes the hovered object outright. A later round could offer it.
- **A non-planar closed chain draws as geometry but never fills.** There is no UI
  telling the user *why* — the region simply does not appear. A "this loop is not
  flat" hint would be the honest fix.
- **The Z constraint is vertical only.** Plasticity offers x / y / z / normal /
  binormal / tangent choices from the point picker (`default-keymap.ts:353-366`).
  KIWI ships the one the user asked for; the other five are a later round's
  letter-budget argument.

## Shakeout I — unified undo/redo, two hint strips, grey cube, right column, scale

- **There is ONE undo timeline now** (`kiwi_undo.h`/`.cpp`, user directive: "Redo
  the whole undo/redo system so that it works with every action"). A journal of
  `{LEGACY, CONSTRUCTION}` tickets records the ORDER in which the two domains
  close records; `Ctrl+Z` pops the newest ticket and forwards it to `Undo_Undo()`
  or `KiwiCon_UndoPop()`. The two stores, their formats and their depth limits are
  unchanged.
- **The tickets are minted at exactly two places.** `undo.cpp`'s `Undo_End` tail
  (the single close point — it is the only writer of `done`, and both `Undo_Undo`
  and `Undo_Redo` consume records only in that state) and
  `kiwi_construct.cpp`'s `KiwiCon_UndoPush`. Both are one-line `// KIWI-UX` tails.
- **The routing hook is in `Radiant_DispatchCommandDirect`**, on `ID_EDIT_UNDO` /
  `ID_EDIT_REDO` (57643 / 57644), NOT in the key funnel — the menu, the palette,
  the `radiant.ini` remap and the `Ctrl+Z`/`Ctrl+Y` arm in
  `Radiant_PreTranslateMessage` all converge there. An empty journal falls through
  to the ported handler unchanged.
- **Eviction is hooked, not inferred.** `Undo_FreeFirstUndo` (the one eviction
  point: the 64-record cap in `Undo_GeneralStart` and the 2 MB cap in `Undo_End`)
  drops the journal's oldest LEGACY ticket; `Undo_Clear` drops all of them.
  `KiwiUndo_Undo` additionally verifies `g_lastundo` before forwarding, so an
  unhooked future path degrades to "one dropped step" rather than a desync.
- **A cancelled gesture mints no ticket.** `KiwiCmd_UndoCancel` brackets itself in
  `KiwiUndo_SuppressBegin/End` — its `Undo_End` is a real close followed
  immediately by the `Undo_Undo` that destroys the record. The `Undo_ClearRedo`
  hook still fires inside that window on purpose.
- **The redo rule is conservative.** ANY new ticket in EITHER domain, and any
  legacy `Undo_ClearRedo`, clears the WHOLE journal redo stack (and the
  construction store's redo with it). That is `Undo_ClearRedo`'s own rule
  generalised; the cost is occasionally dropping a redo that could have survived.
- **`Ctrl+Z` inside a drawing tool is now the SAME Ctrl+Z.** The tool still
  consumes the key (so the legacy arm cannot also fire), but it forwards to the
  journal. `Ctrl+Shift+Z` / `Ctrl+Y` inside a tool are the redo. The palette's
  "Construction: Undo Edit" became "Undo (unified timeline)" and is an alias.
- **The old right-edge hint panel is GONE**, replaced by two bottom strips
  (`kiwi_hints.cpp`): keyboard PROMPTS bottom-left, selection VERBS bottom-right.
  In the Plasticity 0.6 tree in this repo it is the other way round
  (`components/viewport/Keybindings.tsx:52` is `right-3 bottom-3`;
  `components/dialog/Dialog.tsx:32` is `bottom-2 left-2`) — one constant,
  `KHINT_PROMPTS_LEFT`, swaps them.
- **A command can now advertise its OWN keys** via
  `KiwiEditorCommand::HudPrompts` (static list, copied structs / borrowed
  strings). Only the drawing tools override it so far (`Z` vertical constraint,
  `Esc` clears the chain) — both were unadvertised before.
- **The bottom-right strip is EMPTY while a command runs.** A verb list about the
  selection is noise in the middle of an edit to it.
- **The view cube's BODY has no hue at all** (user directive: "make it more like
  blender (grey)"). Faces 0.62 grey front / 0.30 back / 0.78 hovered, dark grey
  labels, darker grey edges. The only colour left is three short axis stubs
  (X red / Y green / Z blue) projected through the same orthographic relation, and
  the hovered-corner dot. Corner targets stay invisible until hovered.
- **`2D View` and `Textures` are OPEN by default again**, docked as a right-hand
  column (2D View top, Textures under it) with the console strip under the CAMERA
  only. `Z` and the KIWI shell panel stay closed. The dock ini bumped
  `kiwi_dock4.ini` -> `kiwi_dock5.ini`, and `[KiwiWindows]` gained a
  `DefaultsVersion` key that re-seeds the five flags ONCE — an existing profile
  would otherwise pin the shakeout-B "all closed" defaults forever. **A user who
  had deliberately closed one of those windows loses that choice exactly once.**
- **Scale tone-down, ~10x, four numbers** (user directive: "Scale of the map (3d
  view) is still way too big. Tone it down by about a factor of 10"):
  `KCAM_DEF_DIST` 256 -> 96, `KCAM_DOLLY_STEP` 0.85 -> 0.90, the fly multiplier
  default 10x -> 4x (profile entry renamed `FlySpeedScale2` -> `FlySpeedScale3`
  so a persisted 1000 cannot pin the old default; the SLIDER is unchanged), and a
  new modern-only spawn at `(0,-160,96)` looking at the origin. **The default grid
  spacing is deliberately UNCHANGED at 10 in** — the directive was about the
  camera, and a grid that no longer matches the map format is a bigger lie.
- **Arrows now fly while a modal command is live.** `KiwiVP_CameraTick`'s hover
  arm dropped its `!KiwiCmd_Active()` test: MMB orbit, RMB-drag pan and the wheel
  dolly were all already live mid-command, so the exclusion was an inconsistency.
  Nothing consumes an arrow key inside a command (checked: the only
  `VK_LEFT/RIGHT/UP/DOWN` references outside the camera layer are the fly's own).

### Still open after shakeout I

- **A cross-domain compound edit is still TWO tickets.** Extruding a region
  consumes construction geometry AND creates brushes; that is one user action and
  two Ctrl+Z presses. Merging them needs a compound ticket kind.
- **Adjacent tickets are never coalesced.** Ten nudges of one brush are ten
  Ctrl+Z presses, exactly as the ported undo always behaved.
- **`mainfrm.cpp`'s layout-dependent `Undo_Redo` tail-jump (the faithful
  `Cmd_OnView*` quirk) bypasses the journal.** It consumes a legacy redo record
  without popping a ticket; the journal self-heals on the next `Ctrl+Y` by
  discarding the stale ticket and printing one console line.
- **The journal is not persisted.** Undo history dies with the map, same as the
  ported stack.
- **The hint strips share the bottom band with the §13 numeric HUD**, which owns
  bottom-CENTRE. Each strip is capped at 46% of the image width and two rows;
  beyond that, chips are dropped from the "More…" end rather than wrapped further.

### Shakeout H follow-up (review pass, uncommitted on top of e41db63/ff5c3a8)

Seven defects found by a read-only review of the shakeout-H diff, all fixed. They
are recorded because every one of them is a SHAPE of bug this layer can grow
again, not just an instance.

- **Per-stage tool state must be reset by the Esc rung, not only by Begin.** The
  line tool's parked-endpoint latch (`m_endLocked`) survived
  `KiwiDrawTool::KeyDown`'s chain-clear, so Esc-then-click left the rubber band
  frozen and RMB committed "zero length". There is now a `virtual void
  OnChainCleared()` hook on the base, called from the Esc rung AND from `Leave()`;
  a tool with per-stage state overrides it. `KiwiLineTool::Finish` also clears the
  latch at the TOP, before either early return.
- **A drawing tool must put the GLOBAL active plane back.** `PushPoint` re-seats it
  at every placed point (that is what makes the next segment fall back to the
  height being worked at) and nothing undid it, so one line drawn at z = 128 moved
  the working plane there permanently — for every later tool, and permanently
  defeating `WorkingPlaneIsGround`, so the grey cplane patch came back on the
  ground plane and stayed. `KiwiDrawTool` now latches `m_planeOnEntry` in `Begin`
  (AFTER `KiwiCon_AutoPlaneForTool`, so the deliberate §16 face derivation still
  persists as it has since shakeout F) and restores it in `Leave()`.
- **Closest-approach is not a drop-in for a 2D line-line solve.** The 3D
  `SNAP_INTERSECTION` rewrite lost three rejections the 2D version got for free,
  and the first was severe: **adjacent segments of the same object share a vertex,
  so their closest approach is exactly zero** — every tessellation vertex of every
  circle, arc, polyline and rect became a fake intersection, 64 per circle, out-
  ranking the midpoint and edge arms below it. The arm now carries each segment's
  owner and ordinal and rejects (1) same-object adjacent pairs including the closed
  seam, (2) near-parallel/collinear pairs on `|d1 x d2| / (|d1||d2|) < 1e-3`, and
  (3) pairs whose crossing sits on an endpoint the two segments SHARE (that point
  is already `SNAP_ENDPOINT`, which out-ranks this arm anyway). Non-adjacent
  same-object pairs are still allowed — a self-crossing polyline has a real
  intersection there.
- **A hash slot needs to know whose it is.** The grid's minor-line hysteresis keyed
  `index & 511` with no owner. Collision-free *within* a frame (200 consecutive
  indices), but panning slides the window and hands a slot to a different line,
  which inherits the previous owner's latch — and a line arriving inside the
  2.1..3.9 px dead band satisfies neither edge, so it keeps the inherited bit
  FOREVER. Each slot now stores the index it owns; a mismatch re-latches from the
  bare threshold. **If `KGRID_LINES_PER_AXIS` ever exceeds `KGRID_HYST_SLOTS` the
  within-frame argument breaks too** — the two must be kept apart.
- **A budget that truncates a search is a correctness bug, not a performance
  knob.** Trim's crossing scan spent one 512-segment budget across the whole store
  IN STORE ORDER: a couple of circles early in the list exhausted it, a real
  crossing went missing, `hi` fell back to `total` and the click deleted a span the
  user never pointed at. It now rejects candidates by AABB at two levels (segment
  vs the hovered chain's box; then per pair), which makes the realistic cost tiny,
  and the remaining cap (`KTRIM_MAX_PAIRS`, 40000 pair tests) is a **safety valve**:
  tripping it makes `GatherCuts` return false, the hover is discarded and the tool
  REFUSES with a console line. The failure mode is "Trim declines", never "Trim
  removes the wrong piece".
- **The store-undo snapshot must be pushed after the last thing that can fail.**
  Trim pushed first and could then discover the object had gone — a Ctrl+Z that
  does nothing. Its first repair, `KiwiCon_UndoPop()` on failure, was worse: since
  shakeout I every `KiwiCon_UndoPush` also mints a ticket in the unified journal
  (kiwi_undo.h) and `UndoPop` is driven BY the journal rather than minting
  anything, so the pop would have stranded an orphan CONSTRUCTION ticket that a
  later Ctrl+Z forwards into somebody else's snapshot. **There is no discard
  spelling in the store API and none was added**: the work is split into
  `PrepareTrim` (reads only, may decline) and `CommitTrim` (cannot decline), with
  the push between them.
- **`IsItemHovered()` returns false on a disabled item.** Every "why is this greyed
  out" tooltip in the Construct panel was dead code — it could only fire while the
  button was enabled, when its predicate is false anyway. All three now pass
  `ImGuiHoveredFlags_AllowWhenDisabled` (deps/imgui/imgui.h:1524).

Also fixed in passing: `KiwiCon_FitPlane`'s longest-edge scan now wraps like the
Newell sum it is paired with; a stale hover is cleared when `KiwiCmd_LastCursor`
fails; and the closed-object trim no longer drops the seam vertex when the cut
lands within epsilon of the ring's end (the unconditional "skip head[0]" is now
conditional on there being a tail it duplicated).

### Still open after the follow-up

- **PASS 2's group plane is still store-order dependent in one narrow case.** When
  the seed is a straight line it determines no plane alone, so a partner has to
  co-determine one. The greedy "first coplanar candidate wins" is now a two-pass
  preference — **pass 1 only considers candidates that TOUCH one of the seed's ends
  (within `KREG_JOIN_DIST`), i.e. that could actually be the next link of the chain
  being looked for; pass 2 falls back to any coplanar candidate** — which removes
  the common failure (a stray line lying in the same floor fixing the plane and
  blocking a real wall loop). It does NOT close it: two candidates that both touch
  the seed end and lie on different planes still resolve by store order. The real
  answer is to derive the plane from the CHAIN the walker finds rather than
  choosing one before walking, which re-orders the whole pass and is a round of its
  own.
- **Trim's crossing scan is still O(chain segments x nearby segments).** The AABB
  rejection makes that small in practice, but a 256-point polyline lying along a
  wall covered in scaffolding can still trip the valve and be refused. Raising
  `KTRIM_MAX_PAIRS` is safe; a spatial hash over the store would be the real fix.

## Round J — Plasticity verbs: hide/isolate, focus, duplicate, offset, fillet, repeat

Six features, all read out of `plasticity/src/startup/default-keymap.ts` +
`src/Menu.ts` rather than guessed. The full design write-up is RADIANT_UX_DESIGN
§41; this section is only the traps and the things a user will trip over.

### Traps this round hit (and what the fix teaches)

- **A "documented" audit can be wrong, and a wrong audit is worse than none.**
  `kiwi_dupe.cpp`'s shakeout-5 note said vk `0x44` carried "only mods 4 = Select
  Inside" and floated Shift+D as free. It is not: `0x44` carries mods 0 `CameraUp`
  33055 (`mainfrm.cpp:1034`), mods 1 `RotateZ` 32961 (`:1078`), mods 5
  `MakeDetail` 33042 (`:1088`) and mods 7 `DropVertices` 33213 (`:1077`). Binding
  Shift+D on the strength of that note would have produced a **silently shadowed
  row** — `Radiant_TryHotkey` is first-match-wins over row order and every KIWI row
  sits after the 187 compiled-in ones, so the new binding would simply never fire.
  The note is corrected in place rather than deleted. **Every chord in this round
  was re-derived from `mainfrm.cpp` directly; none was taken from a comment.**
- **The classic Clone has no undo record at all.** `Cmd_OnSelectionClone`
  (`mainfrm.cpp:2973-2980`) calls `Clone_Selection` and two flag-refresh loops and
  opens no bracket. `Undo_Undo` only removes brushes carrying an undo id stamped by
  `Undo_EndBrushList`, so the copies are invisible to undo: Ctrl+Z after a Clone
  undoes whatever came *before* it and leaves the clones sitting in the map. The
  new `KIWI_CMD_DUPLICATE` brackets itself with the **creation** shape
  (`Undo_GeneralStart` → clone → `Undo_EndBrushList` → `Undo_End`, deliberately
  **without** `Undo_AddBrushList`). The classic id 33001 is untouched — fixing it
  would be a ported-logic change — so **Space still has the old behaviour and
  Shift+D has the correct one.**
- **`Undo_EndBrushList` does not need a preceding `Undo_AddBrushList`.** Checked in
  `undo.cpp:576-590` rather than assumed, because the whole creation bracket rests
  on it: it needs an open, not-yet-`done` record and nothing else. `kiwi_extrude`
  reaches the same state by deselecting first so that `AddBrushList` is a no-op;
  Duplicate cannot deselect (the originals must be selected for `Clone_Selection`)
  and so omits the call instead. Two spellings of one bracket, both documented.
- **A miter join without a limit fires a spike across the map.** The offset's
  per-joint miter length is `|d| / cos(half-turn)`, which is unbounded as a corner
  closes. `KOFF_MITER_LIMIT` (4) degrades the joint to a two-point bevel past ~29
  degrees. That is the standard stroke-join treatment and it is not optional.
- **An offset that self-intersects must be refused, not repaired.** Asked with the
  §8 toolkit's own `KiwiRegion_SelfIntersects` for closed loops, so an offset that
  would be rejected as a *region* is rejected at the offset instead of becoming an
  un-extrudable loop the user has to diagnose. A closed loop whose **signed area
  flipped** is refused too — that is an inward offset that has eaten past the
  shape's own medial axis, and it is the cheap total test for it. Open chains get
  the same non-adjacent-crossing test locally, because the region helper reads its
  input as a ring.
- **A fillet radius must be clamped per corner, not globally.** Two corners sharing
  an edge that each consume more than half of it produce crossing arcs. Each corner
  is clamped to `0.5 * min(|BA|, |BC|)` worth of tangent, so **typing a huge radius
  saturates instead of failing** — every corner rounds as much as its own edges
  allow. Corners within `KFIL_FLAT_DOT` of straight or of a full reversal are
  skipped rather than approximated.
- **The construction store's snapshot still goes last.** Both new modal commands
  compute their whole result into a buffer during the gesture and touch the store
  only at Commit, immediately before the single mutation and only after every
  refusal test has passed — §37's "nothing can fail after the push" rule, which
  exists because a discarded `KiwiCon_UndoPush` strands a ticket in the unified
  journal (there is still no discard spelling in the store API, and still none was
  added).

### Live limitations

- **`/` (Focus) cannot be renamed or rebound from `radiant.ini`, and the
  command-list panel prints a garbage glyph for it.** The key-NAME table
  (`g_radiantKeys`, `mainfrm.cpp:1323-1336`) was lifted verbatim from the binary
  and has no entry for `0xBF`; both `CommandList_KeyName` (a port of `sub_40BBC0`)
  and `LoadCommandMap`'s parser read it, and `LoadCommandMap` only accepts a single
  **alnum** char or a table name. The ported table is **not** edited for a display
  detail: `kiwi_hints.cpp` carries a KIWI-side OEM name table so the on-screen chip
  says "/", and the command-list panel is left showing the raw glyph. Rebinding
  Focus needs either an entry in that ported table or a KIWI-side alias row.
- **`Alt+R` (MouseRotate, displaced by Repeat Last Command) inherits the Alt-chord
  limitation** already logged in this file — `WM_SYSKEYDOWN` is not routed to the
  hotkey table in the ImGui shell, so Alt chords do not fire there. MouseRotate
  stays on the Selection menu and in the palette, exactly like Surface Inspector on
  Alt+S. **This is the round's one uncomfortable trade** and it is made knowingly:
  MouseRotate is the classic 2D rotate *mode* that the modern profile's bare `R`
  has already superseded.
- **Duplicate-then-place is TWO Ctrl+Z presses**, because the Move it hands off to
  opens its own bracket. That is the same split the classic Paste + Move already
  has and it is deliberate (two intentional acts, two records), but it will surprise
  anyone expecting one.
- **Invert Hidden flattens the hide DEPTH to a single level**, so a "Show Last
  Hidden" (33246) after an invert peels everything rather than one layer.
  `Select_Hide`'s own third pass writes `xx5 = 1` unconditionally to its targets
  (`select.cpp:4179-4180`), so this is the house behaviour; inventing a depth
  arithmetic for a fused hide+show would be a rule with no source anywhere.
- **Offset does not accept a FACE.** Plasticity's `offset-curve` takes a face (and
  a set of edges) as well as a curve, offsetting its boundary loop into a new curve
  (`OffsetCurveCommand.ts:8-21`). KIWI has no face-winding to construction-object
  converter, so only the curve arm ships. The 2D core is reusable as-is when one
  exists.
- **Fillet produces a tessellated polyline, not an arc-and-line contour.** The
  store has no CONTOUR type, and adding one is a sidecar format change (§16b made
  the same call for splines and polygons). Density follows `KCON_SEGS_PER_UNIT`, so
  a filleted corner is exactly as smooth as a drawn arc of the same radius — but it
  is no longer re-editable as "a corner with a radius".
- **X-RAY IS NOT SHIPPED, on purpose.** `alt-z` → `viewport:toggle-x-ray` does two
  things in Plasticity: translucent solids **and** selection reaching occluded
  geometry (the layer mask it flips is read by the raycaster). The renderer half has
  a real ported hook — `camera.draw_mode` (`mainfrm.h:33`) → `Cam_TechForDrawMode`
  `case 0` = `TECHNIQUE_WIREFRAME_SHADED` (`camwnd.cpp:414-430`) — but the selection
  half needs an occlusion/pick-mask flag in `kiwi_pick.cpp`, which does not exist.
  A wireframe-only toggle would let you SEE through a wall and still not CLICK what
  is behind it, which is worse than no key. The sequence is: occlusion flag in
  `Pick()` first, then one toggle driving both halves. RADIANT_UX_DESIGN §41.7.

### Not attempted this round

- **`space` / `shift-space`** (`viewport:navigate:selection` /
  `viewport:grid:selection`). The first REORIENTS onto the selection's construction
  plane rather than framing it, and `space` is classic Clone (33001). The second —
  set the construction plane FROM the selection — is a genuinely good next feature
  and belongs with the existing `KIWI_CMD_CPLANE_*` family, not bolted onto this
  round.
- **Per-corner fillet RADII.** Per-corner *selection* ships (Point mode names the
  corners, matching `ContourFilletFactory`'s `controlPoints` narrowing); per-corner
  *radii* need per-corner gizmos, which is a separate feature.

### Round J — defects the review pass caught (all fixed)

- **A delta-mapped modal command MUST bias its Rebase, and both new ones did not.**
  Offset and Fillet map the drag as `value = raw - m_start`; their first `Rebase()`
  was a bare `LatchStart()`, which sets `m_start = raw` and therefore makes the very
  next `Recompute` produce **zero**. Concretely: press `O`, drag out a 32-unit
  offset, release LMB (the gesture PAUSES, preview still shows 32), press LMB to
  resume → the offset collapses and the HUD turns red with "distance too small".
  The house fix already existed twice — `kiwi_extrude.cpp:291-299` and the scale arm
  of `kiwi_transform.cpp` both do `const float keep = value; LatchStart(); if
  (m_haveStart) m_start -= keep;` — and both new files now do it. **Any future
  command whose scalar is a delta from a latched origin needs those three lines;
  `LatchStart()` alone in `Rebase()` is always a bug.**
- **A palette predicate runs every frame the palette is open, for every row.**
  `KiwiFocus_CanFocus` was implemented as "compute the bounds and see if there are
  any", which on an empty selection walks both brush lists *and* tessellates every
  construction circle/arc with `cosf`/`sinf` per vertex — ~200 rows × once a frame
  (`kiwi_palette.cpp:114`). It is now three pointer compares plus `KiwiCon_Count()`,
  which gives the identical answer. **A `canExecute` must be O(cheap); if the honest
  test is expensive, find the cheap test with the same answer.**
- **`KiwiRegion_SignedArea` returns exactly `0.0f` for a degenerate loop**, and a
  sign-comparison refusal written as `(area1 > 0) != (area0 > 0)` silently passes
  when `area0` is 0 (`false != false`). A collinear "closed" polyline would have
  offset into a zero-thickness ribbon and committed. Zero area is now its own
  refusal, before the flip test.
- **Invert Hidden was repeatable.** `Shift+R` after `Ctrl+H` re-inverted, i.e. undid
  what the user had just done. It is a view verb by its own file's argument (that is
  why it takes no undo bracket), so it joins Focus and the other viewport actions in
  `Repeatable()`'s exclusion list. **The test for "should Shift+R repeat this" is the
  same one as "does this open an undo bracket".**
- Also: the offset's parametric "object went away" bail-out skipped its
  `g_nUpdateBits` and left a dead preview on screen; `SignedDistance2D`'s comment
  claimed a point-in-polygon guarantee the nearest-edge sign does not give (it is a
  drag-sign nicety past the medial axis, not a correctness property, and now says
  so); `kiwi_offset.h` claimed the offset "goes where the cursor is" when the
  mapping is a delta (true only because the gesture usually starts on the chain —
  now stated that way); and Fillet's "nothing to round" refusal now distinguishes
  "you selected anchors and none is a corner" from "this chain has no corners".

## Round K — intersection-bounded regions, the lollipop, the box-creation fix

### FIXED — the primitive/box creation bug (root cause, not a symptom)

USER REPORT: *"Creating a 3d brush vertically is impossible right now, any camera
angle creates it horizontally. You need to fix this like how it is in plasticity.
I can't even create a box, the line never expands."*

ROOT CAUSE. `kiwi_snap.cpp`'s arm 8 (`SNAP_CPLANE` — ray ∩ the active construction
plane, grid-snapped in plane) and arm 6's suppression were both gated on
`KiwiCon_ToolActive()`, and that predicate means exactly *"one of
kiwi_construct.cpp's nine `KiwiDrawTool` objects is `s_activeTool`"*. The four §16b
primitives are `KiwiEditorCommand`s in a different file and were never visible to
it, so with a Box gesture live the snap fell through to arm 6 (the `Test_Ray`
surface hit) or arm 9 (ray ∩ the world Z=0 ground grid), and
`KiwiPrimitiveCommand::MouseMove` then projected *that* answer onto its own working
plane. On a horizontal plane at z==0 the projection happens to track the cursor —
which is why every box came out horizontal and looked like it "worked"; on a
**vertical** plane the ground answer has a constant height, so the in-plane `v`
coordinate never moved and the base rectangle could not grow in that direction.
Not the round-G `m_grabbed` gating and not the round-E pause flow: a `WantsClicks`
command never pauses (`KiwiCmd_Pause` returns early for one) and the primitives have
no grab gate at all.

FIX, in three parts:
1. `KiwiCon_SetPlanePlacement(bool)` / `KiwiCon_PlanePlacement()` — a command that
   places on the working plane without being a drawing tool announces itself, and
   the snap query asks the new predicate. With no primitive live the two predicates
   are identical, so every other snap answer is bit-for-bit unchanged.
2. `KiwiPrimitiveCommand::MouseMove` now resolves stage 0/1 itself: a *geometry*
   snap is projected onto the plane as before, anything else is `KiwiCon_RayPlane`
   against the working plane and `KiwiCon_SnapUV`. This removes the dependency
   rather than only fixing today's instance of it, and it is Plasticity's own point
   picker (geometry snaps win, the construction plane is the fallback that always
   has an answer).
3. `Z` at stage 0 cycles the working plane XY → XZ → YZ, because while a modal
   command runs the framework swallows every unconsumed key and the §16
   construction-plane commands are unreachable mid-Box. The stage-0 HUD names the
   plane in force.

Stage-2 (height) already mapped the cursor onto the plane normal via the
closest-point-on-axis solve and needed nothing; it is now reachable at any camera
angle because stage 1 finally completes on a vertical plane.

### FIXED — "left clicking is disabled" after a face click

A mode-3 face click auto-enters a PAUSED push/pull, and shakeout E's resume rule is
deliberately the broadest one ("any LMB press inside the camera image resumes"), so
the next click was eaten as a resume and never reached the selection layer.
`KiwiEditorCommand::IdlePressReselect` is offered above the resume arm and only
while the gesture is PAUSED *and* has applied nothing; it delegates to the exported
`KiwiBox_ClickSelectAt`, so there is still exactly ONE click grammar. The rule table
is on the override in `kiwi_transform.cpp`.

### FIXED — the pink face outline

`camwnd.cpp:786 Cam_DrawSelectedFaces` was it: the port's own selected-face
highlight, which outlines every `g_SelectedFaces` winding in magenta `0xFFFF00FF` at
width 3 (the binary tints picked faces through DrawGeo's editor surf-cache; the port
could not). Gated on `KiwiUX_ModernInput()` rather than deleted, so the classic
profile keeps its only selected-face feedback. Second path: the face-extrude
command's own preview traced the source winding exactly at distance 0 — it now draws
nothing below `KEXT_MIN_DIST`.

### OPEN / accepted limits

- **Arrangement caps are per-group and hard.** 256 segments, 1024 fragments, 1024
  nodes, 64 cells. An overflow logs and yields no cells *for that group* rather than
  a partial arrangement. A store dense enough to hit this (four 64-segment circles
  on one plane) will silently lose the arrangement's regions while the exact-loop
  passes keep working — the console line is the only signal.
- **The arrangement re-runs whole on every store-generation bump.** It is not
  incremental. Plasticity's `PlanarCurveDatabase` *is* incremental (it re-processes
  only the curves an added curve touched); KIWI re-derives everything, which is the
  same choice §8 already made for the exact-loop passes. At the caps above the cost
  is bounded, and the pass is per store CHANGE, never per frame.
- **A bounded cell containing a spur is rejected, not repaired.** The face walk
  emits the spur's out-and-back, the loop is non-simple, and `AcceptLoop`'s
  self-intersection gate drops it. That is §8's stated "reject > repair" and it is
  visible as a region that does not appear where a stray line pokes *into* an
  otherwise closed area.
- **No holes, still.** A rectangle with a smaller rectangle inside it produces two
  separate regions, not a ring. The arrangement finds both cells correctly; §8's
  region model has no hole representation, which is spec decision D-3 and unchanged.
- **Shift+D edge chaining is greedy at an ambiguous node.** Three brush edges
  meeting at one corner (which is every corner when three faces' edges are selected
  at once) resolve in selection order rather than being refused the way
  `KiwiRegion_ChainWalk` refuses a degree-3 node. A wrong guess costs a polyline
  split in two, not a wrong region — §8 re-chains the pieces and the arrangement
  does not care about object boundaries at all.
- **Region selection survives by centroid.** Two genuinely different regions whose
  centroids are within `KREG_FLASH_MATCH` (2 units) of each other are two loops
  drawn on top of each other; the selection may resolve to either. Same key, same
  tolerance and same reasoning as the shakeout-F flash carry.
- **A face push/pull loses the gizmo’s three world arrows.** The lollipop replaces
  the whole gizmo for that context (the directive: “Also hide the move gizmo when
  extruding”), so the arrows are no longer available to axis-LOCK a push by
  grabbing one. The X / Y / Z keys still do it, unchanged, and the prompt strip
  still advertises them — but the mouse-only route is gone for faces. Objects,
  edges and vertices keep the gizmo exactly as before.
- **Face-winding outlines that were CHECKED AND KEPT.** `kiwi_matchface.cpp:314-319`
  and `kiwi_split.cpp:792` both trace a face winding — those are *live command
  previews* ("this is the face you are matching / splitting"), up only while their
  gesture runs and colour-coded as previews. The directive was about a face
  **selection** looking like an edge selection; if the user wants those gone too they
  are two `if`s.


## Round L (2026-08-10) — grab rebase, riding pivot, Cut rework, Boolean (Q)

- **The gizmo now RIDES the geometry it is moving.** This reverses a deliberate
  shakeout-D choice ("a translate gizmo that crawls with the geometry runs away
  from the cursor") because the user reported the opposite as a bug. It is safe
  because the DRAW anchor and the MAPPING anchor were separated —
  `KiwiXform_ActivePivot` serves `m_ref + applied` while `MapCursor` still
  measures from the latched `m_ref` — but it does mean the arrows are somewhere
  new at the end of a drag. If a future round wants the old behaviour back it is
  one call site (`KiwiXform_ActivePivot`), not a redesign.
- **The pivot rides only an OBJECT move.** A face push, an edge move and a vertex
  drag RESHAPE the selection, so there is no single vector the anchor travelled;
  the session pivot stays where it was placed. That is defensible but it is a
  choice, and a user who places a pivot on a corner and then pushes the face that
  corner belongs to will find the pivot left behind. Fixing it properly means
  tracking what happened to the pivot POINT rather than to the selection, which
  needs a per-kind answer this round did not attempt.
- **The grab freeze is one PIXEL, not one frame.** `GrabLive` releases the moment
  the cursor lands on a different pixel, so a user who presses a handle and drags
  in the same frame the press arrives loses nothing — but a user whose mouse
  reports sub-pixel movement (a very high-DPI device with pointer precision on)
  can hold a handle without releasing the freeze until the accumulated motion
  crosses a pixel boundary. That is at most one pixel of dead zone.
- **Cut refuses to start when the map has no construction line at all.** The
  directive removed the *selected*-line precondition, not the *existence* one:
  "click a line to cut along" cannot be answered from inside a modal gesture, and
  drawing one requires leaving it. The console message says which key draws one.
- **Cut's plane cannot be re-aimed without re-clicking.** Locking it at the click
  is exactly what was asked for, but it means the only way to change the sweep
  direction is Esc (back to stage 1), orbit, click the line again. There is no
  "re-aim in place" key. Adding one would re-introduce the thing that was removed.
- **Boolean has no INTERSECTION.** `kiwi_boolean.h` argues it in full: the same
  loop with the halves swapped produces the tool-shaped lump, which for a convex
  tool is just the tool and for a concave one is not representable as one classic
  brush. It would be a verb that either does nothing visible or refuses.
- **A difference target entirely INSIDE the tool is left untouched.** Classic
  Radiant would delete it (its difference is the empty set). KIWI declines, prints
  why, and leaves it — "a verb that silently removes a brush you can no longer see
  to undo" is the worse failure. The user deletes it with Del if that is what they
  wanted. This is a real behavioural difference from every other brush editor.
- **A sliver intersection can refuse a whole carve.** The final remainder IS
  `target AND tool`, and it is §19-gated like every other half, so a tool face that
  is coincident-but-not-quite with a target face can produce a remainder that fails
  the zero-area / opposed-plane checks and takes the entire carve down with it
  (all-or-nothing, by policy). Nudging the tool off the coincident plane fixes it;
  the console message names the failing check.
- **The difference TOOL survives; Plasticity's does not.** `keepTools` defaults
  false there (`BooleanFactory.ts:264`) and is a dialog checkbox. KIWI has no
  dialog to hang it on and picked "keep" as the single default. There is no way to
  ask for the other behaviour short of deleting the tool afterwards.
- **Union inherits every `CSG_Merge` limitation, unchanged.** Two solids whose
  union is not convex are refused by the ported core with its own message; so are
  patches, fixed-size entities and brushes from different entities. Nothing in
  `kiwi_boolean.cpp` second-guesses it and nothing fakes a non-convex union.
- **A multi-target union merges EVERYTHING at once.** `CSG_Merge`'s contract is
  "merge everything on `selected_brushes`", so selecting three solids and unioning
  against a tool produces one merge of four, not three merges of two. That matches
  Plasticity's own union special case (`BooleanFactory.ts:340-343` folds every
  target after the first into the tool list of a single factory).
- **The boolean preview fill is capped at 8 solids.** One
  `R_AddRenderCmdDrawTris` per FACE means an unbounded multi-target union could
  put hundreds of draw calls in one frame. Past the cap the operands are outlined
  only; the tool is always filled.
- **`KiwiXform_NoteMoveGrab` was REMOVED.** The move grab gate is raised through
  `KiwiEditorCommand::HandleGrab` by the shared arm now. Anything outside this
  tree that called it (nothing does) would need to move to `KiwiCmd_HandleGrab`.

---

## Round M — grid v3, ortho camera, cut z-order

- **The ground grid's reach is finite and now visibly so.** v3 spends its budget
  on a `spacing × 120` world lattice window instead of the whole frustum
  footprint, so the far edge is a real edge, softened by the outermost brightness
  band rather than hidden. At a very shallow pitch you WILL see where it stops.
  That is deliberate: v2 reached further only by coarsening the cells until the
  near field was empty, which is the defect this round removed. If the reach needs
  to be longer, the number is `KGRID_HALF_CELLS`, and the cost is linear segments.
- **The grid ignores the horizon.** The window centre is pushed 0.45·R along the
  yaw-plane forward and that is the ONLY concession to where the camera is
  looking. Look straight down from high up and roughly a fifth of the emitted
  lines are behind you (the behind-the-eye cull removes the ones fully behind, not
  the ones straddling). Bounded waste, not a correctness problem.
- **The grid LOD is altitude-driven, so a camera at Z≈0 looking along a wall gets
  the finest cells.** Correct by the directive ("density must not change with
  angle") but it means a ground-level camera in a large room draws base-spacing
  cells out to `120 × base` and no further. Raising the camera is the control.
- **The "anti-aliasing" is a halo, not AA.** Majors get a dim width-2 under-pass;
  minors get nothing. Real AA needs either per-vertex alpha on the `$line` path
  (the technique lerps toward MATERIAL_COLOR — kiwi_lines.h TRAP 2) or MSAA on the
  shared D3D9 device (which would mean re-creating the device and every RTT
  target). Both are renderer work and neither was attempted here.
- **The construction-plane grid patch is GONE, on every plane.** §16 asked for a
  "subtle finite grid patch around the cursor" and three rounds failed to make it
  wanted. Working-plane feedback is now the snap marker, the numeric bubble, the
  active chain itself and §17's ground grid. A user drawing on a steeply tilted
  off-world plane has no *grid* cue for that plane's orientation — only the chain
  and the marker. If that turns out to matter, it comes back as an opt-in.
- **Ortho: the cubic cull planes are still the perspective cone.** `CamWnd_Fov`
  builds `CullCubic`'s side planes from the FOV, and the fixedsize MODEL/PREFAB
  pass culls against them. In ortho the parallel slab is wider than the cone near
  the eye, so a misc_model very close and far off-axis can be culled while it
  should be visible. World brushes are unaffected (their `CullCubic` is elided in
  this port) and so are patches.
- **Ortho: two CLASSIC-profile paths still build perspective rays.** The ported 3D
  marquee (`Camera_GetRectSelection3D` + the translucent drag quad) and the
  terrain-paint cursor ring call the static `CameraCalcRayDir` directly, which is
  deliberately left byte-identical. The modern box-select goes through
  `Pick_WorldToImage` and IS correct in ortho; the classic one is not.
- **Ortho: "zoom to cursor" degrades to "zoom to centre".** `KiwiCam_Dolly` still
  picks under the cursor and still moves the origin along the ray, but in ortho
  the image only responds to the `s_dist` change, which scales about the view
  centre. The pivot stays put, so the result is usable — it just is not the
  cursor-converging zoom the perspective camera gives.
- **The cut z-order fix is narrow on purpose.** The world fill no longer
  substitutes `d_white` (`white_tools`) for a tool material that WOULD have
  written depth — in the shipped asset set that is exactly `caulk` and `nodraw`.
  Every other tool material (`clip`, `trigger`, `hint`, `skip`, `portal`,
  `origin`, `lightgrid_volume`) is itself alpha-blended and depth-write-free, so
  those volumes still draw through each other exactly as before. If a project
  ships an OPAQUE variant of one of those, it will now occlude where it used to
  not — which is the correct reading of its own state bits, but it is a change.
- **The same substitution exists nowhere else, but the same TRAP does.** Any
  future editor pass that swaps in a "flat" material silently inherits that
  material's blend/depth state. `Cam_MaterialWritesDepth` is the predicate to
  reach for; there is no general guard.

## Round N — the Ctrl+R swallow, ortho default, grid controls, cube alignment,
## boolean operand consumption, shift-click faces, snap accents

- **Ctrl+R (and Z / J / E / C / Q) were never broken — they were SWALLOWED, and
  the swallow was structural.** Clicking a face in mode 3 auto-enters a PAUSED
  push/pull (`kiwi_boxselect.cpp`), so in Face mode "a face is selected" and "a
  modal command is live" are the SAME state. `KiwiUX_KeyFunnel` routes every key
  to `KiwiCmd_KeyDown` while a command is live, and that ladder's last rung
  returns `true` for everything it does not recognise. So each of these verbs died
  in exactly the state its own `canExecute` requires. The fix is one new rung
  above the funnel's active-command arm plus one new virtual
  (`KiwiEditorCommand::PreemptIdle`). **It is an ALLOW-LIST, not a general
  un-swallow**: only the six ids in `PreemptVerb` (kiwi_command.cpp) may cancel a
  parked gesture, and only while that gesture has applied nothing. A seventh verb
  that later wants the same treatment has to be added there by hand — deliberately,
  because preempting means cancelling a gesture the user is standing in.
- **The preempt is cancel-and-start, so anything the parked gesture had "in
  progress" that is NOT geometry is lost.** Today that is nothing (an unmoved
  push/pull holds a latched centroid and a zero scalar), but a future auto-entered
  command that accumulated state before its first mutation would lose it silently.
  `PreemptIdle` is the place to say no.
- **Ortho is the default, via an entry RENAME.** `CameraOrtho` → `CameraOrtho2`
  (the `FlySpeedScale3` precedent). Anyone who had toggled the pill under round M
  has their old preference ORPHANED, not migrated — one click on the pill restores
  it and writes the new key. Every ortho caveat in the round-M block above is now
  the DEFAULT experience rather than an opt-in one; the classic 3D marquee and the
  terrain ring in particular still build perspective rays.
- **PageUp / PageDown displace UpFloor / DownFloor.** The two classic "jump the
  camera a floor" commands move to Shift+Alt+PageUp / PageDown in the modern
  profile. They stay on the menus and in the palette. `[` and `]` are unchanged —
  the new keys are ALIAS ROWS (`KiwiGridDoublePage` / `KiwiGridHalvePage`), the
  same mechanism the Delete key uses, so one id carries two bindings.
- **The grid readout is a display, not a field.** The top-right pill steps by
  doubling/halving only. There is no way to type `7 in` there; the settings block
  is where an arbitrary spacing is set. A text field would have to take the
  keyboard away from the viewport, which is a worse trade than one missing route.
- **The view cube's TOP / BOT views snap yaw to the nearest 90 and the corner
  views to the nearest 45.** That is the "reset the spin" the directive asked for,
  but it means a TOP click can now rotate the plan up to 45 degrees from where the
  user was orbiting (it takes the nearest quarter turn, so never more). The PITCH
  is still clamped to ±89 at the poles — a pole-exact pitch degenerates the
  yaw-plane basis `CamWnd_BuildMatrix` derives, which is why every camera path in
  this layer clamps there. A "plan" view is therefore 1 degree off true plan, as
  it has been since shakeout A.
- **Boolean difference now CONSUMES THE TOOL, reversing round L's deviation 3.**
  The re-usable-cutter workflow round L argued for is gone; the replacement is
  Ctrl+Z (which restores the tool, because the tool is explicitly covered with
  `Undo_AddBrush` inside the bracket — it is NOT on `selected_brushes` and the
  bracket head therefore never cloned it) or Shift+D before pressing Q. If a
  future round wants the choice back it needs a dialog, not a default.
- **Difference deletes the tool only when SOMETHING was carved.** A Q that misses
  every target opens no bracket and leaves both operands alone, which is right,
  but it does mean "the tool is still there" is not by itself evidence the command
  did nothing — read the console line.
- **Shift+click multi-face had TWO causes and both are fixed; neither was the
  round-K rule table.** (1) The additive rung was gated on `!s_hot`, so any
  Shift+click after the auto-entered gesture had been resumed (the broad
  resume arm, a lollipop-ball grab) fell through to the HOT tail, which only
  pauses — the press was consumed and the selection never grew. (2)
  `KBOX_CLICK_PIXELS` was 4, so a Shift+click that wobbled five pixels
  LEFT-TO-RIGHT became a five-pixel CONTAINMENT marquee, which needs a whole face
  inside it, found nothing, and (because Shift suppresses the clear) changed
  nothing at all. Raised to 8. **Consequence: a deliberate marquee under 8 px in
  both axes is now a click.** Nobody box-selects an 8-pixel region, but it is a
  real behaviour change.
- **The snap accents are FACE-ONLY and PLACEMENT-ONLY.** They appear for the one
  brush face under the cursor while a `WantsClicks` command is live — the drawing
  tools and the four primitives. They do NOT appear for patches (no winding),
  for construction geometry (which has its own point markers), or during a drag
  gesture. The budget is 40 dots; a face with more than ~19 winding points loses
  its centroid dot first and then its later midpoints. No brush face comes close.
- **The accents cost one extra `Pick()` per frame while a tool is live.** It is
  the same `Test_Ray` chain the snap query already runs, so the frame does that
  work twice during a placement stage. Measurable only on very large maps; the
  obvious fix (cache the query's own face hit) needs the snap result to carry it,
  which is a wider change than this round wanted.
- **Missing-material faces now draw with `$default3d` instead of `$default`, and
  lose the selected-brush red tint.** A brush face whose material file is absent
  gets a clone of `rgp.defaultMaterial`, whose techset is `2d` -> statemap
  `default2d`, which forces `depthTest Disable` AND `depthWrite Disable`
  unconditionally. Such a face neither occluded nor was occluded: checkerboard
  boxes drew through each other in submission order (so a COPY, appended later,
  always won) and the ground grid — which correctly depth-tests LESSEQUAL — passed
  over everything because nothing had written depth. Both reports were this one
  bug. `Cam_DrawMaterial` (camwnd.cpp) redirects those faces to `wc/$default3d`,
  the shipped 3D twin: same `default` colormap, same opaque blend, techset
  `default` -> statemap `default` -> depth write ON. **Cost:** the substitute's
  `unlit` technique is `textured_simple`, which has neither a vertex-colour nor a
  MATERIAL_COLOR term, so a missing-material brush no longer takes the red
  selected-brush tint. It still shows the white wireframe outline and the KIWI
  selection accents. **Not covered:** the `DrawGeo` path (brush.cpp) that draws
  MODEL and PREFAB contents — a prefab whose material is missing keeps the old
  no-depth behaviour. That path takes its material from the per-face `faceVis`
  array rather than from `Face_BuildLayerGeom`, so covering it is a separate
  edit, not a wider version of this one.

## Round P — the MMB jump, Alt+MMB views, the chained curve, axis guides,
##           absolute grid snapping

- **Alt+MMB is a KIWI invention, not a Plasticity port, and the directive that
  asked for it was based on a false premise.** The brief was "add the alt-middle
  click shortcut that plasticity has to cycle through the stages of the camera
  cube. (look it up)". Looked it up: Plasticity's default keymap binds exactly
  two mouse chords in the orbit scope — `"mouse1": "orbit:rotate"`,
  `"mouse2": "orbit:pan"` (`default-keymap.ts:330-333`) — and
  `OrbitControls.onMouseDown` falls to `default: state = {tag:'none'}` for
  anything not in that table (`OrbitControls.ts:362-392`), so bare `alt-mouse1`
  does NOTHING there. It also cannot reach a command:
  `KeyboardEventManager.ts:56` converts only RMB into a command keystroke and
  `ViewportControl.ts:97` refuses every button but LMB. `alt-mouse1` gains a
  meaning only in the OPTIONAL orbit presets (`ConfigFiles.ts:101-122`: pan in
  Maya, rotate in 3ds Max). What Plasticity does have is six named axis views
  (`viewport:navigate:front|right|top|back|left|bottom`, `Viewport.tsx:150-156`,
  numpad-bound at `default-keymap.ts:218-231`, and driven by its own nav cube at
  `ViewportNavigator.ts:136`) — so THOSE are what the chord steps through here.
  Consequence to watch: anyone who later wants the Maya/3ds-Max orbit presets
  will find Alt+MMB taken.
- **Alt+MMB costs an Alt-held orbit 4 pixels of lead-in.** The chord has to
  discriminate click from drag, so with Alt held the orbit is deferred until the
  travel passes `KVP_RMB_CLICK_PIXELS`; the accumulated travel is then applied in
  one go (no lost degrees). A BARE MMB orbit is unaffected and still rotates from
  pixel one.
- **The orbit no longer re-latches the reference distance, so a LEGACY camera
  path that moves `camera.origin` behind this layer's back can leave the ortho
  zoom stale.** Round P removed `KiwiCam_OrbitBegin`'s `s_dist` re-latch because
  in ortho `s_dist` IS the zoom and re-latching it from a surface pick was a
  visible jump on the press edge (kiwi_camera.h "THE MMB JUMP", jump 2). Every
  MODERN path that moves the eye moves the pivot with it, so `s_dist` stays
  consistent; only the classic free-look / fly paths (which the modern input
  profile does not use) could desynchronise it. The wheel, `/` Focus and the
  view cube all re-seat it.
- **The shakeout-H PARKED ENDPOINT is gone from the line tool.** Click 2 used to
  park the rubber band so the user could sweep the cursor elsewhere and re-aim;
  in the chained curve, click 2 places the second point. The two grammars cannot
  share one click. The need behind the original directive ("lines should not
  confirm until a right-click or Enter") is still met and more directly — no
  click commits anything now — but re-aiming a point ALREADY PLACED is done by
  pressing Esc to drop it and placing it again, not by unparking it.
- **Line and Polyline are the same command.** Both ids stay registered and both
  resolve to `KiwiCurveTool`; the menu, palette and panel rows label the second
  one as the alias it is. A user looking for a distinct "polyline" behaviour will
  not find one — that is the merge, not a bug.
- **Esc inside the curve tool means "drop the last point", not "clear the
  chain".** Plasticity binds point-removal to `ctrl-z`
  (`default-keymap.ts:182-194`, `gizmo:line:undo`); that slot is not free here
  because shakeout I made Ctrl+Z inside a drawing tool the ONE unified undo
  journal. Consequence: there is no single key that clears a long chain — it is
  Esc per point, or Esc-to-exit after a finish.
- **The axis guides capture at 10 px (16 px for the vertical near the horizon),
  not Plasticity's 30.** Plasticity's point picker uses
  `Line2: { threshold: 30 }` screen pixels (`PointPicker.ts:138-142`); this
  editor's viewport is full of brush edges at 6 px and construction segments at
  10 px, and a 30-px axis would shadow both. Consequence: the guides need a
  closer aim than Plasticity's do.
- **The axis guides rank by pixel distance against brush edges, where Plasticity
  gives axes a fixed priority above faces and planes** (`SnapPicker.ts:136-161`,
  AxisSnap = 2). A brush edge nearer the cursor than the axis wins here. The
  guide still draws (it is "you are near the vertical" either way).
- **A vertical axis seen from near-overhead is refused.** It projects to a dot;
  below 8 px of projected length there is nothing to aim along. Use the Z lock
  (which is unchanged) or orbit to a flatter angle.
- **The axis guides only exist for a drawing tool's LAST PLACED POINT.** They are
  not offered for transforms, for the primitives' second stage, or before the
  first point of a chain. Extending them to `KiwiMoveCommand` would need an
  anchor concept the snap layer does not have yet.
- **Grid snapping is ABSOLUTE now — a move that used to preserve an object's
  off-grid offset no longer does.** That is the fix (the directive is quoted in
  `kiwi_transform.h`), but it is a behaviour change: dragging an off-grid brush
  with grid snapping live now PULLS IT ONTO the grid on the first snapped frame.
  Users who relied on "nudge by exact multiples" should place a pivot (V) on the
  feature they want on the grid, or hold Ctrl to suppress snapping entirely.
- **Face push/pull snaps absolutely only on an axis-aligned normal.** On a
  slanted face there is no single world axis for the plane to be on the grid of,
  so that case still quantises the DISTANCE and therefore still preserves the
  face's original off-grid offset. Deliberate and narrow; a real fix would need a
  per-plane lattice concept.

### Round Q — un-extrude, bevel fineness, patch fillets

- **The bevel/inset drag no longer snaps AT ALL, at any grid spacing.** The
  scalar was always the raw closest-point solve; a `floorf(d/g + 0.5f)*g` on the
  frames where the snap query returned `SNAP_GRID` — i.e. almost all of them —
  was what made the chamfer step in whole cells. That line is gone
  (`kiwi_bevel.h` ROUND Q). Consequence: **there is now no way to get a
  grid-multiple chamfer by dragging.** Type the number — numeric entry is exact
  and unaffected. A GEOMETRY snap still works and still means "chamfer up to
  that thing", because it targets rather than quantises.
- **`E` on a face with a NEGATIVE distance no longer creates anything.** It
  carves the source face inward, and past the brush's own thickness along that
  normal it DESTROYS the brush. Anyone whose muscle memory was "drag past zero,
  nothing happens" now has a live delete one confirm away. The HUD names the
  outcome on every frame before the confirm, and the doomed brush is outlined in
  the delete red, but it is a real behaviour change on an existing key.
- **The un-extrude preview is a preview, not live geometry.** Unlike G's face
  push/pull, `E` mutates nothing until commit (that is this command's standing
  contract), so a negative drag draws where the face WILL land rather than
  moving it. The carve itself is `KiwiXform_PushFaceOnce`, i.e. the same code
  the interactive push runs.
- **The patch fillet's PATCH is created at commit, not per frame.** The chamfer
  is fully live and baseline-disciplined; the arc is drawn as a polyline (the
  bezier evaluated, so it is exact) and the `patchMesh_t` is built once. The
  reasoning is in `kiwi_patchfillet.h` — a patch creation links a brush into the
  entity def list and mallocs a tessellated mesh, and doing that tens of times a
  second is not something the undo/selection funnels are built for. Consequence:
  nothing about the patch (its material, its column count) can be inspected
  until it lands.
- **A q3 patch cannot be a circular arc.** It is non-rational, so the fillet is
  an approximation: `ε = r·(1 - cos(α/2))² / (2·cos(α/2))` per span, and the
  command subdivides to 30° per span, giving ~0.06% of the radius on a square
  corner. It is NOT exact, and a single-span (3-column) fillet — which is what
  classic Radiant's Curve→Bevel gives you — is 6% off.
- **The fillet's collision hull is the CHAMFER, not the arc.** The solid keeps
  the flat cut; the patch is a surface over it. That is what "pseudo-fillet"
  means and it is the same trade every q3-lineage map makes.
- **Edges whose faces meet at more than 170° or less than 5° are refused** with a
  console message rather than approximated. Between those the general angle is
  handled exactly (the construction is written in `k = sin(θ/2)`, not assumed
  square).
- **The patch's facing is derived, not chosen.** `Curve_ComputeNormals`
  (`pmesh.cpp:502/609`) makes a control point's normal proportional to
  `cross(dCol, dRow)`, so the row order is flipped when needed to make that
  agree with the outward bisector. If a fillet ever renders backfacing, the
  escape hatch is Curve → Matrix → Invert, exactly as for any other patch.
- **`B` is a context verb now.** With brush edges selected it is the SOLID
  fillet; otherwise it is round J's construction-corner fillet. When BOTH
  selections are non-empty the brush edges win (the same precedence `J` uses).
  The construction panel's own "Fillet (B)" button bypasses the redirect and
  always means the curve fillet; the PALETTE row named "Fillet Corners" does
  NOT, so picking it by name with brush edges selected runs the solid fillet.
- **`MakeNewPatch` leaves `patchMesh_t::flags` uninitialised** (it sets
  `contents` but not `flags` — `pmesh.cpp:136`), and every ported creator then
  copies it onto the symbiont brush's faces. The fillet seeds both from the
  source face instead. The ported creators are untouched and still carry the
  quirk.

### Round R — the duplicate-plane reject, off-grid cplane snapping, line clicks

- **The "duplicate plane" reject was a PROFILE defect, not a geometry one, and
  the profile defect was a REDUNDANT COLLINEAR VERTEX.** A square drawn out of
  five lines has one side split in two; §23's prism builder writes one side face
  per profile edge, so the two halves of that side produce two faces on the SAME
  plane and §19's V5 gate refuses the brush — correctly. `AcceptLoop`
  (`kiwi_region.cpp`) now drops a vertex whose two edges agree to within
  `KVALID_PLANE_DOT`, which is §19's own constant, so a profile the region layer
  accepts is a profile the validity gate accepts by construction. Consequence:
  **a region's vertex count can be lower than the number of lines that made it**
  — the console's "N points" and a region's corner count are no longer the same
  number, and that is intended.
- **The store now normalises point lists on ADD and on SIDECAR LOAD.**
  Consecutive coincident points collapse, and a CLOSED object's repeated seam
  vertex is dropped (`KiwiCon_Add` / `KiwiCon_LoadSidecar`). Deliberately NOT run
  from `KiwiCon_NoteMutated`: a move gesture legitimately drags one point through
  another, and eating a vertex mid-drag would destroy data the user is holding.
  Consequence: an object can come back from `KiwiCon_Add` with fewer points than
  it went in with, so a caller that cached the count it built is wrong — the one
  that did (`KiwiConSel_Join`'s console line) now reads it back from the store.
- **`KiwiCon_SnapUV` is anchored at the WORLD ORIGIN, not at the plane's own
  origin.** That is the whole of the "grid snapping completely breaks with custom
  grid spacing … even deleting the whole scene doesn't fix it" report: the active
  plane's origin is written by `PlaneFromFacePick` (the raw ray hit on a face) and
  by `KiwiDrawTool::PushPoint` (every placed point, including one on a geometry
  snap), so it is off-grid by construction, and every snapped point was congruent
  to it modulo the spacing. Behaviour change: **an axis-aligned working plane now
  snaps in absolute world coordinates**, so a cplane placement no longer preserves
  the plane origin's fractional offset. A genuinely TILTED plane still snaps in
  its own u/v — there is no world lattice for it to be on — but from a fixed
  anchor, so the same cursor position always gives the same point.
- **`KiwiCon_ClearAll` now also resets the active construction plane to ground
  XY.** It is reached from `Map_NewMap`, which both File→New and every map load
  pass through. Consequence: an explicit "Construction: Clear All" ALSO resets the
  working plane — that is the same command, and a scaffolding-free editor with a
  plane inherited from deleted scaffolding is the state this fixes.
- **The construction CLICK radius (14 px) is now wider than the hover/snap radius
  (10 px).** `KCON_CLICK_PIXELS` vs `KCON_LINE_PIXELS`. The arbitration is
  unchanged (`kiwi_boxselect.cpp`): a construction line beats a brush FACE at any
  depth and loses to a nearer brush VERTEX or EDGE. Consequence: two construction
  lines within 14 px of each other are decided by pixel distance, so a dense
  cluster is harder to pick apart than it was — box select is the escape.
- **The region fill is nudged 0.5 units toward the eye.** A region drawn on a
  brush's top face is exactly coplanar with it, and a coplanar fan under a
  LESSEQUAL depth test is decided per pixel — which is the "the blue line plane
  was invisible" report. The nudge is `kiwi_hover.cpp`'s own fill number. Cost:
  the fill floats a half unit in front of its true plane, so at a grazing angle
  it can peek past the silhouette of a thin brush it lies on.
- **The grid steppers walk a fixed ladder and can no longer produce 1.25 in.**
  0.125/0.25/0.5 then 1, 2, 4, 5, 8, 10, 16, 20, 25, 32, 50, 64, 100, 128, 200,
  256, 500, 512, 1000, 1024. The three sub-inch rungs are NOT in the directive's
  list and are kept because `KGRID_MIN_INCHES` has been 0.125 since §17 shipped
  and the steppers are the only controls that reach it. Consequence: a spacing
  typed off the ladder steps to the nearest rung on the far side rather than
  doubling from where it is.
- **The grid pill's value is now a real ImGui item — the only one in the
  top-right cluster.** Clicking it opens a small popup with an InputText (Enter
  applies, Esc cancels); while it has focus `io.WantTextInput` is true and the
  shell's `ImGuiShell_WantsKeyboard` gate keeps every hotkey out, the same
  mechanism the command palette's filter field relies on. Typed values are NOT
  ladder-snapped — §17 says any positive spacing is legal — and are clamped to
  [0.125, 1024]. Consequence: the cube's "no ImGui item" contract now has one
  documented exception.

## Round S

- **Ctrl+X no longer quits the editor; it is CUT.** `res/radiant.rc`'s
  `IDR_MAIN_ACCEL` bound `Ctrl+X` to command **32951 = `ID_FILE_EXIT_RAD`**
  (`res/resource.h:34`), dispatched at `mainfrm.cpp:5646` as
  `case ID_FILE_EXIT_RAD: Radiant_FileExit();`. That accelerator ENTRY is deleted
  (a `// KIWI-UX` fence in the .rc records what it was and why). The **File→Exit
  menu item keeps id 32951**, so quitting from the menu is unchanged. With the
  accelerator gone, `Ctrl+X` reaches the editor command map, where the MODERN
  profile binds it to `KIWI_CMD_CLIP_CUT` (34112) = ported **Copy (33039)** then
  ported **Delete Selection (33003)**. In the CLASSIC profile `Ctrl+X` is
  **unbound** — the safe reading of "should not quit".
  - Consequences: the undo record for a cut is labelled **"delete"**, because
    `Cmd_OnSelectionDelete` is already a complete record and nesting a second
    bracket for one act would be worse than a cosmetic label. `SelectedAssociated`
    (33152), which held Ctrl+X in the default table, moves to **Ctrl+Alt+X**
    (mods 6) and inherits the already-logged Alt-chord limitation (WM_SYSKEYDOWN
    is not routed to the hotkey table in the ImGui shell) — it stays reachable
    from the menus and the §15 palette.
- **The §17 grid + axes are drawn BEFORE the world, not after.** `KiwiGrid_Draw`
  moved from the Cam_Draw tail (after every world/entity/patch pass) to just
  before the world's first `R_SortMaterials`. Painter's order then guarantees the
  world covers the grid **regardless of the world's depth state**, which is the
  property round O's `$default3d` substitution could not provide on its own (it
  only reaches faces that resolve to a `$default` clone, and only when the asset
  set ships `$default3d`).
  - The grid still uses `$line`, i.e. **depthTest LESSEQUAL + depthWrite ON**
    (`main/materials/$line` refStateBits `0x08128812` / `0x0000000d`, techSet
    "tools" → `vertcol_shaded_tools` → stateMap `default`, whose depthWrite rule
    `mtlBlendOp == Disable: Enable` fires). It cannot occlude world geometry: a
    nearer world pixel passes, a *coplanar* one passes too (LESSEQUAL admits ties
    and the world is submitted later — so a floor brush on Z=0 now HIDES the grid
    where it used to lose the tie), and a world pixel strictly behind a grid line
    is correctly occluded, exactly as before.
  - `R_AddCmd_Line3DNoDepth` is deliberately NOT used. `$line_nodepth` clears
    DEPTHWRITE in its ref bits, but carries the same blendOp-Disable and the same
    "tools" techset, so the same `default.sm` rule forces the write back on: the
    bound state is depth test OFF, depth write ON — paint over everything AND
    stamp depth, i.e. backwards for a pre-world pass. There is no shipped line
    material with depthWrite disabled.
- **The grid fades out when the view goes edge-on to Z=0.** Ramp on `|vpn·Z|`:
  below **0.03** (~1.7°) the ground lattice is skipped entirely (the axes still
  draw), 0.03→**0.08** (~4.6°) is a linear brightness ramp, and MINOR lines are
  dropped below the half-way point of that ramp. Consequence: at a near-edge-on
  axis view there is briefly a majors-only grid and then no grid at all — that is
  the fix for the moiré slab, and the three world axes are what still says which
  way is which.
- **Ctrl+R is "Split Brush at Face", and the line is live.** The command has
  always split the BRUSH — a convex plane-brush is the intersection of its faces'
  half-spaces (`Brush_BuildWindings`, `brush.cpp:1459`), so there is no
  representation in which the solid stays one brush and one face becomes two. The
  HUD, the palette row, the hint chip and a console line now say so. The cut line
  follows the cursor (projected onto the face plane along the slide axis), snaps
  to geometry or the §17 lattice, and takes a typed **offset measured from the
  face edge at the LOW end of the slide axis** — the HUD prints `offset X / span`
  so the reference always has its scale attached. `SNAP_FACE` is excluded from the
  geometry rank (it is the unsnapped Test_Ray surface hit and would defeat grid
  snapping); Tab still flips U/V and **resets the offset to the centroid**,
  because the reference edge changes with the axis.
- **Alt+MMB is a swipe and never orbits.** Round P deferred the orbit by 4 px and
  then handed the gesture over; an Alt+MMB press that travels now stays a view
  gesture for its whole life. Release under **24 px** of net travel = round P's
  nearest-then-ring step; past it = a 90° step in the swipe's dominant screen
  direction (right/left = yaw ∓90 through front→left→back→right; down = TOP, up =
  BOTTOM; from a pole the opposite swipe returns to the side view at the current
  yaw and a horizontal swipe spins the pole in place). Consequence: **Alt+MMB can
  no longer orbit** — bare MMB is the orbit, unchanged.
- **The viewport image is drawn at the render target's integer size.** It used to
  be blitted across the fractional `GetContentRegionAvail()` remainder while the
  RT was created at `((int)avail.x, (int)avail.y)`, i.e. scaled by two different
  factors (≤1 px on each axis). Sub-pixel, but a real anisotropic stretch;
  removed. It is NOT the cause of the "cylinder looks skewed" report — see below.
- **The "cylinder is skewed" report is NOT a projection bug (investigated, no
  change made).** Both projections are isotropic and agree with the pick ray:
  perspective is `m[0][0] = C/tanX`, `m[1][1] = C/tanY` with `tanX = tanY*w/h`;
  ortho is `m[0][0] = C/halfW`, `m[1][1] = C/halfH` with
  `halfW = halfH*(tanX/tanY) == halfH*w/h`. Both give **`C·h/(2·halfH)` pixels per
  world unit on BOTH axes**, and the ray builder's `s = 2·tanY/h` with
  `dist = halfH/tanY` gives `h/(2·halfH)` on both — so the 0.75 convention lives
  inside `tanY` only, cancels in `tanX/tanY`, and is applied exactly once. The
  D3D viewport is the render target's own size (`R_GetViewport` takes the
  `GFX_USE_VIEWPORT_FULL` arm because the editor's
  `R_ViewportBehaviorForRenderTarget` returns FULL for FRAME_BUFFER,
  `r_state.cpp:2194`), so the stale `dx.windows[0]` size
  `R_Ed_SetSceneParms` pushes is inert.
  - What IS true, and is cosmetic-classic, left alone: `Brush_MakeSided`
    (`brush.cpp:3381`) builds a **circumscribed (tangent) polygon** whose planes
    are tangent to a circle of radius `width` at angles `0, 2π/n, …`. The solid is
    regular and axis-symmetric (inradius = the drawn radius, circumradius 1.96%
    larger at n=16), but its VERTICES sit at `π/n + k·2π/n` — half a step off the
    axes — which is the classic "the cylinder looks rotated" impression.
  - Also true and left alone: `CameraCalcRayDir` uses INTEGER `width/2` and
    `height/2` while the matrix centres exactly, so on an odd-sized viewport cell
    picks are up to half a pixel off what is drawn. That is the binary's own
    convention.

### Round T — inheritance over caulk, the merged bevel, view-oriented planes

- **The KIWI split verbs no longer stamp caulk on the faces they create.**
  `Cut`, `Split Face` and the `Q` boolean's carve seeded their template face with
  `Ed_BuildClipFaceMaterial_Kiwi` (the clipper's caulk / nodraw_decal synthesis).
  They now inherit from the source brush under `kiwi_material.h` R2 — the largest
  INHERITABLE face whose plane is most perpendicular to the cut — and fall back to
  the caulk synthesis only when the brush has no inheritable face (R3), so a caulk
  block still cuts into caulk. **The CLASSIC clipper is untouched and still
  caulks**: that is the binary's behaviour and it stays. Consequence: a brush cut
  in half is now visibly textured on the cut, which also removes the depth-writeless
  tool material that round M/O identified behind the z-order complaints.
- **The Q boolean's hole belongs to the TARGET, never to the tool** (R4). It falls
  out of the splitter being handed the target's own def; the tool is scaffolding
  and is consumed by the commit.
- **THE FILLETS WERE INVISIBLE BECAUSE THEY FAITHFULLY COPIED A TOOL MATERIAL.**
  The chain, end to end: `kiwiBevelEdge_t::srcFace` was hardcoded to `adj[0]`, an
  arbitrary one of the two faces at the corner → after one Cut, half the edges in a
  map have a CAULK neighbour → the chamfer inherited caulk → and round Q's
  `LandPatches` copied the patch's material from **the chamfer face it had just
  created**, so the patch landed carrying caulk. Present, selectable, drawing
  nothing. Fixed in three places: `srcFace` now runs through R5 (prefer an
  inheritable adjacent face, else the brush's dominant one, else `adj[0]`); the
  patch copies from `srcFace` rather than from the chamfer; and every copied
  `MaterialDef` is pushed through `Materialdef_Realize` (R6). R6 is the *other*
  half and is worth knowing on its own: a copied def carries `{lyrMtl, radMtl}` as
  POINTERS and `lyrMtl` can be a DEGENERATE zero-layer handle, which makes
  `MaterialDef_11 == 0` → `visCount = 0 / visArray = NULL`
  (`pmesh.cpp:9903`) → `DrawPatches` returns before emitting anything.
- **`Cut` (C) accepts a brush FACE or a brush EDGE as well as a construction
  line.** A FACE's plane IS the cutting plane — no camera term, so orbiting between
  the click and the confirm cannot change it. An EDGE is treated exactly like a
  line (plane through it, swept away from the camera at click time). Ranking: a
  construction line and a brush edge compete on pixel distance with the line
  winning an exact tie; a FACE is an area hit and can never out-rank either.
  Cut's `PICKF_EXCLUDE_SELECTED` means the plane can never come from a brush being
  cut. **Cut no longer refuses to start when the map has no construction lines.**
- **`Bevel Edge` and `Fillet Edge (patch)` are ONE command now.** They were the same
  live drag with different commits; `KiwiBevelCommand` is deleted and
  `KIWI_CMD_BEVEL_EDGE` forwards to the fillet command, which **starts in chamfer
  mode**. `D` toggles chamfer ↔ fillet mid-gesture (the scalar is converted through
  `d = r(1-k²)/k` so the solid does not jump; a typed value is cleared, because a
  depth is not a radius). Consequence: the palette/menu no longer offer two
  buttons, and a gesture recorded by Repeat Last replays whichever mode it ended in.
- **The chamfer has an ANGLE BIAS; the fillet does not.** Bias is a second,
  Tab-reachable field in DEGREES: the chamfer plane's normal is the bisector rotated
  about the edge, positive = toward the second adjacent face, 0 = the old symmetric
  plane exactly. It is clamped PER EDGE to the corner's own half-angle minus 2°, and
  the HUD says "(clamped)" when the applied angle differs from the request. The
  FILLET refuses it: a tangent-to-both-faces arc has its axis pinned on the bisector,
  and a tilted plane's tangent solution is an ellipse the biquadratic construction is
  not derived for. Pressing D with a bias in force drops it, out loud.
- **DELETE with exactly one brush FACE selected now REMOVES THAT FACE** instead of
  deleting the brush. That is the restore: a chamfer is one extra half-space, so
  removing it lets the neighbours re-extend and the edge comes back. Deliberately
  the GENERAL verb — nothing records that a face was made by a bevel and every
  heuristic for guessing it is wrong on somebody's map. §19-validated on the rebuilt
  brush; a refusal restores the brush from its planepts snapshot, says why, **and
  still consumes the key** (falling through to the classic delete after "I could not
  remove that face" would delete the whole brush). Any PATCH brushes also selected go
  in the same record — that is how a fillet's two halves come off together. Anything
  else selected and the rung does not fire at all.
- **Trim counts SELF-crossings now, and refuses rather than eating a whole curve.**
  The failing case: since round P the line/polyline tools are one CHAINED curve tool,
  so an "oddly drawn shape" is a SINGLE polyline — and `GatherCuts` skipped `self`
  whole, found zero crossings, and the open-chain branch fell back to
  `lo = 0 / hi = total`, i.e. the click deleted the entire object. `self` is now
  scanned like anything else minus the pairs that are not crossings (a segment against
  itself or either neighbour, plus the wrap pair on a closed chain). AND the
  zero-crossing fallback is gone: **no crossings, no trim**, which is what Plasticity
  does (`TrimFactory.ts:52`) and what shakeout H deliberately diverged from. To remove
  a whole line, select it and press Delete — the console says so.
- **The working plane is derived from the VIEW when nothing is under the cursor.**
  `KiwiCon_AutoPlaneForTool`'s rungs are now: construction object under the cursor →
  brush FACE under the cursor → **the world axis most aligned with the camera's view
  direction** → the active plane stands. Computed at tool START only (orbiting
  mid-gesture must not re-seat a plane you have placed points on); the Z cycle remains
  the explicit override. It quantises to a world axis rather than taking the view
  plane itself, because the cylinder and cone primitives REFUSE a non-axis-aligned
  plane (the ported `Brush_MakeSided` takes a WORLD axis) — which is exactly the
  "I can't make a horizontal cylinder" report. Consequence: the plane keeps its
  current position along the new normal, so on a fresh map a YZ plane derived this way
  sits at x = 0 until something is placed.
- **A Move drag with a placed pivot runs its SNAP QUERY at the pivot, not the
  cursor.** Round L made the pivot the mapping reference and gave it a riding draw
  anchor, but the snap query is screen-space (every arm in `kiwi_snap.h` ranks by
  pixel distance) and was still being asked at the mouse — so grabbing an axis arrow
  with the pivot on a far corner offered whatever was near the ARROW. The command now
  names the pivot's projected position through the new `SnapQueryAnchor` hook. Limits:
  OBJECT moves only, only with a pivot override, not while the pivot is being placed,
  and the position used is the PRE-snap mapped one, so the anchor is **one frame
  behind** — deliberately, so the query cannot feed on its own output.

## Round U

- **Shift+click multi-face selection: ROOT CAUSE FOUND, and it was never in the
  selection layer.** `Radiant_PreTranslateMessage` (radiant_main.cpp:646) fed every
  `WM_KEYDOWN` to `KiwiUX_KeyFunnel` and, when the funnel returned true, returned
  true itself — so the pump called neither `TranslateMessage` nor `DispatchMessage`
  and the message never reached the window procedure. ImGui's Win32 backend updates
  `io.KeyShift` / `io.KeyCtrl` / `io.KeyAlt` in exactly one place, its
  `WM_KEYDOWN`/`WM_KEYUP` arm (`ImGui_ImplWin32_UpdateKeyModifiers`,
  imgui_impl_win32.cpp:848-857) — nothing polls them; `NewFrame`'s
  `ProcessKeyEventsWorkarounds` can only take a stuck modifier DOWN-to-UP. With a face
  auto-enter parked (i.e. every time), the funnel's active-command arm swallowed
  VK_SHIFT, `io.KeyShift` stayed FALSE, and `KiwiVP_CameraButtonDown` was handed
  `shift = false` — so `IdlePressReselect` ran its PLAIN arm and REPLACED the
  selection. Round K's rung and round N's widening were both correct and both
  unreachable. **Fix: the funnel passes bare modifier keys straight through, above
  every other arm.** Audited safe: no row of `g_radiantCommandsDefault` carries vk
  0x10/0x11/0x12 and no accelerator fires on a bare modifier. This fixed CTRL
  (the deselect modifier) at the same time.
- **A Shift+click near the lollipop ball no longer grabs it.** Shift is additive
  everywhere else in the editor and never means "grab a handle"; the ball sits 42 px
  off the face along its normal, close enough to a neighbouring face to matter.
- **The preempt allow-list gained the Ctrl+1..4 conversions and the whole Shift-chord
  creation set** (line/curve/rect/circle/polygon/spline + the four primitives + the
  add menu). Same swallow round N fixed for six verbs: in Face mode a face selection
  IS a live PAUSED push/pull, so anything not on the list was eaten. The conversions
  cannot re-trigger the auto-enter (converting to edges deselects the face, and the
  auto-enter needs a CLICK).
- **`KiwiCon_AutoPlaneForTool` gained a rung: the SELECTED face's plane.** Order is
  now construction-object-under-cursor, then **exactly one face SELECTED**, then
  face-under-cursor, then view-dominant axis, then the active plane stands. Above the
  cursor rung because selecting is deliberate and pointing is incidental. Refuses on
  two or more selected faces — there is no single honest answer. This is what makes
  "click a face, Shift+A, draw on it" land the sketch ON the face.
- **DELETE on a MIXED selection deletes the construction half and skips the brush
  half.** `KiwiConSel_OwnsDelete` used to require nothing brush-side at all, reading
  both the legacy sentinel AND `KiwiSel()`; a mode-2 marquee over scaffolding that
  overlaps a wall produces SEL_EDGE items, so the arm declined and the key fell
  through to 33003 — which walks `selected_brushes`, finds it empty (fine kinds do not
  promote since shakeout D) and does nothing. Net effect: Delete silently did nothing.
  The test is now the sentinel alone; face/edge/vertex items are skipped and COUNTED
  in the console. A WHOLE-OBJECT selection still falls through to the classic delete
  untouched.
- **Construction geometry can be HIDDEN (H).** `kconObject_t::hidden`; **hidden means
  INERT** — skipped by draw, click-pick, marquee, snap AND section-8 region derivation.
  The region ruling is deliberate: scaffolding is hidden because it has done its job,
  and a region held together by an invisible line is a region nobody can fix. Hiding
  clears the selection of what it hid. Persisted in the sidecar as `hidden 1`, format
  version still **KIWI2** — absent means visible, and older builds skip unknown
  keywords, so it is compatible in both directions. **H dispatch:** construction only
  hides and consumes the key; brush only is untouched (the ported 32923); BOTH hides
  the construction half AND lets 32923 hide the brushes. Bare H only — Shift/Alt/Ctrl+H
  are round J's family, unchanged. `Unhide All (construction)` is a palette verb.
- **Construction anchors are filled dots, not x's.** Camera-facing 8-gons with their
  long diagonals filled in (the snap-marker/lollipop trick): 2.5 px rose unselected,
  3.5 px white selected. The old 3 px cross and the 5 px selected square are gone;
  the per-anchor batch guard went 6 to 16 segments to match.
- **The lollipop is restyled**: ring 18 to 14 px (28 to 20 chords), stem 48 to 42 px
  starting at 0.55 of the ring radius (was 0.35, which crossed the ring), ball 7 to 5 px
  (10 to 12 sides), ring colour near-white to the dim yellow so the ring and ball read
  as one object. The 12 px PICK radius is deliberately unchanged.
- **The marquee is one clean translucent rect.** The crossing marquee used to draw a
  hand-rolled DASHED border (6 px dash / 4 px gap) over a 10%-alpha fill — a row of
  disconnected segments with an invisible interior, i.e. "lines extending from each
  point". Both modes now draw a filled rect (alpha 26 to 48) with a solid 1 px border
  and differ only in hue. Everything else that could draw during a marquee was checked
  and cleared: the snap axis guides refuse without a click-wanting command, the
  label/HUD/bubble are inside `if ( KiwiCmd_Active() )`, the hover accents are cleared
  at `KiwiBox_Begin`, and the LEGACY 3D marquee quad (`Ed_DrawSelectionBoxQuad`) is
  unreachable — it is armed only by `Drag_Begin`, reached only from
  `CamWnd_OnLButtonDown`, which `KiwiVP_CameraButtonDown` consumes first.
- **Ctrl+X is unbound again.** "Cut should only be on (C)" — with C already the cut
  TOOL, a second "Cut" one chord away made the word mean two things. The .rc
  accelerator stays deleted (Ctrl+X must never quit); `KIWI_CMD_CLIP_CUT` survives as
  the palette row **"Cut to Clipboard"**. `SelectedAssociated` stays on Ctrl+Alt+X —
  un-displacing it would make the profile depend on which round wrote your ini.
- **The default grid is 1 inch** (was 10). The registry entry is renamed
  `GridSpacingInches` to **`GridSpacingInches2`**, the FlySpeedScale precedent: the
  spacing is persisted on every change, so every existing session had a stored 10 that
  a bare default change would never have overridden. The old entry is deliberately not
  read as a fallback.
- **The vertical Alt+MMB swipe WRAPS.** Round S's mapping was a ladder with two ends
  ("already bottom - swipe the other way"). It is a great circle now:
  `ref side -> TOP -> opposite side (yaw+180) -> BOTTOM -> ref side -> ...`, i.e. from
  front: front, top, back, bottom, front — and UP is that backwards. The horizontal
  ring is unchanged. It carries two ints of phase and has to: the quantity
  distinguishing "front, having come over the top" from "back, clicked on the cube" is
  ROLL, and this camera has none. The phase is VALIDATED against the live camera on
  every swipe and resynced from it on any disagreement.
- **The white-window flicker: ROOT CAUSE FOUND.** The frame's `WM_PAINT`
  unconditionally ran `R_AddCmdClearScreen( 7, g_qeglobals.d_savedinfo.colors[1], ... )`
  and presented — and `colors[1]` is set to `{1,1,1,1}`, **pure white**, in
  win_qe3.cpp:415 (it is the classic 2D grid background). The ImGui scene that covers
  it is submitted by a different function entirely (`ImGuiShell_DrawOverlay`,
  pre-EndScene) which refuses to draw on any paint the pump did not authorize
  (`s_beginFrame`) and on its own re-entrancy guard. So ANY unrequested `WM_PAINT` — a
  nested `TrackPopupMenu` / `MessageBox` message loop, a window move, an OS repaint —
  cleared the client area to white and presented it empty. **Fix: the paint does
  nothing but validate the region unless a scene is actually going to be drawn**
  (`ImGuiShell_FrameAuthorized()`); under DWM the window keeps its last presented
  content, so the correct frame stays up until the pump's next tick (16 ms or less).
  Re-authorizing instead was not an option — `s_beginFrame` keeps `NewFrame` paired
  1:1 with `UpdatePlatformWindows`. One exception, the boot instant: before the pump
  has ever driven a frame there is no last-good frame to keep, so the first paint
  still clears.

## Round V — the AFK crash, and the emergency rescue save

- **The AFK crash: ROOT CAUSE CONFIRMED, and it was a lost D3D9 device meeting a
  Com_Error.** USER REPORT: *"After going AFK for a while and coming back I get this
  crash... it ruins all progress of your map."* The reported message is
  `.\r_init.cpp (2163) dx.device->CreateAdditionalSwapChain(...) failed` raised from
  `R_Hwnd_Resize`, under a `Radiant_FrameWndProc` resize arm nested inside uxtheme's
  `OnDwpNcLButtonDown` -> `OnDwpSysCommand`. Every hop:
  1. AFK (screensaver / monitor sleep / lock / another app takes the GPU) loses the
     D3D9 device. Nothing notices, because nothing paints.
  2. The operator clicks the title bar. `DefWindowProc` nests `SendMessage` down to
     `WM_SIZE`.
  3. `Radiant_FrameWndProc`'s `WM_SIZE` arm (radiant_main.cpp:236-250) chains
     `DefWindowProcA` then calls `R_Hwnd_Resize` (radiant_main.cpp:244).
  4. `R_Hwnd_Resize` (r_init.cpp:4961) releases the window's swap chain and calls
     `CreateAdditionalSwapChain` on a **lost** device, which cannot succeed.
  5. `hr < 0` -> `Com_Error(ERR_FATAL)` -> engine_stubs.cpp `Com_Error` ->
     `ExitProcess(1)`. The map dies with the process.
  Step 5 is the only bug; 1-4 are ordinary Windows. **Fix: on failure the arm now asks
  whether the device is lost** (`hr`, `dx.deviceLost`, and a fresh
  `TestCooperativeLevel` — the same authority `R_TestDevice` and
  `R_CanRecoverLostDevice` use). If it is: release the half-created state, leave the
  slot as `swapChain == NULL` with the **new** width/height, and return. If the device
  is HEALTHY the `Com_Error` is kept — a create that fails on a fine device is a real
  OOM / invalid-parameter fatal with no recovery, and swallowing it would trade a loud
  crash for a permanently black viewport.
- **The deferred recreation already existed.** `R_ResetDevice`'s KISAK_RADIANT tail
  (r_init.cpp:4498-4499) re-creates EVERY window's additional swap chain by calling
  `R_Hwnd_Resize` again at `dx.windows[w].width/height` — so storing the new size in
  the lost arm is what makes the window come back at the size the operator dragged it
  to rather than the pre-AFK one. A second landing site was added for the case where
  the loss resolves **without** a Reset ever running (`TestCooperativeLevel` goes
  straight back to `D3D_OK`, so `R_RecoverLostDevice` never fires and nothing would
  ever rebuild that chain): `R_SetupRendertarget_CheckDevice` (r_init.cpp:4803) now
  retries the create in place instead of only skipping the frame — it is reached only
  after `R_TestDevice()` returned 1, i.e. the device is confirmed healthy.
  Degenerate sizes were already refused in both places and still are
  (`R_Hwnd_Resize` head, r_init.cpp:4964; the `width <= 0 || height <= 0` gate at
  r_init.cpp:4800; and `WM_SIZE` skips `SIZE_MINIMIZED` at radiant_main.cpp:243).
  Neither `g_disableRendering` (never decremented) nor `dx.deviceLost` is touched by
  the lost arm.
- **A fatal error can no longer take your map with it.** Every deliberate death path
  now writes `<mapname>_rescue.map` next to the real file — or `<exe dir>\rescue.map`
  for an untitled map — plus the `.kiwi` construction sidecar, then pops ONE message
  box naming the file. Hooked at `Com_Error`'s death arm (engine_stubs.cpp:351),
  `Sys_Error`, `Sys_OutOfMemErrorInternal`, and `Sys_DirectXFatalError`
  (r_init.cpp — the `exit(-1)` that `R_FatalInitError` / `R_FatalLockError` funnel
  into and that never passed through `Com_Error`).
  - **It is NOT `Map_SaveFile`.** That function is operator-attended and unusable from
    a fatal handler: `Map_SaveFileToPerforce` pops two `MessageBoxA` prompts (a nested
    message pump, into the renderer we are dying inside), `LayeredMaterials_Save` can
    abort the whole save, it sends window messages to the frame and status bar, and it
    clears `modified` — which would claim the work reached its real path. It touches
    no renderer state; the disqualifiers are all modal/UI. The rescue reproduces its
    WRITE CORE only (`iwmap 4`, `Layers_WriteToFile`, the entity gate +
    `MapFile_WriteEntity` with the region flag hard 0), which is pure `fprintf`.
  - **Limits.** One attempt per process, ever. The whole writer is inside
    `__try/__except` so a fault in the rescue cannot mask the original error. The real
    `.map` is never touched and `modified` is never cleared. It runs whenever the
    entity list is non-empty — deliberately NOT gated on `modified`, because
    `MarkMapModified` is called from 73 ported edit sites but only two `kiwi_*` files,
    so a `modified == 0` reading is not evidence the session is clean. Undo history,
    selection, camera and layer UI state are not rescued; the geometry is.

## Round X

The design rationale for every item is in **RADIANT_UX_DESIGN §54**. What follows
is only what a user or a future audit could trip over.

### Fixed

- **The z-order bug is `Cam_MaterialIsMissing`'s pointer gate** (camwnd.cpp).
  `rgp.defaultMaterial` is registered UNPREFIXED (techset `2d`, r_material.cpp:219)
  and every brush face's material is registered through `wc/` (techset `wc_2d`,
  g_materialTypeInfo[4] at r_material_load_obj.cpp:5266-5273, applied at :5514) — so
  the techniqueSet POINTER can never match for a face, the test returned false on the
  line above the depth-write test, and `$default` faces kept depthTest+depthWrite
  DISABLED. The gate now falls back to a NAME compare with the vertex-declaration
  prefix stripped. **Consequence to watch:** the predicate is now broader by exactly
  the prefix set (`""`, `m_`, `mc_`, `w_`, `wc_`), so any material whose techset base
  name is `2d` *and* which does not write depth is substituted with `$default3d`. That
  is the intent, but it is a bigger net than round O cast and it is the place to look
  if some surface starts drawing with the wrong material.
- **`$default3d` must exist in the asset set or nothing improves.**
  `Cam_MissingMaterialSubstitute` prints `WARNING: no "$default3d" material...` and
  returns null. It registers as `wc/$default3d` and validates with the SAME (now
  name-based) predicate, so a broken asset set is still detected — but check the
  console on a repro before concluding the fix did not work.
- **A SELECTED brush is now pickable.** `kiwi_pick.cpp`'s `selUnmask_t` clears
  `BRUSHFLAG_SELECTED` across the one `Test_Ray` call and restores it. Before this,
  `sub_48D460` (select.cpp:677) threw away every member of the selected list, so
  clicking a selected brush — or a face of one — found nothing and DESELECTED instead.
  That was both "mode-3 face select takes 2 clicks" and "sometimes an object isn't
  selected and it takes 2 tries".
- **Hidden construction objects now trigger a REPAINT.** The draw pass has skipped
  them since round U; `KiwiCon_SetHidden` never asked for a redraw, so hiding a curve
  from the outliner's eye left it on screen until something else invalidated. Also
  fixed: the cut tool's construction-line pick, the trim tool's hover and its
  `KiwiTrim_CanTrim` predicate had never honoured the flag at all.
- **Extrusions no longer snap to their own start plane** (`KEXT_SELF_SNAP_BAND`,
  2 units, kiwi_extrude.h) and **parking a gesture no longer re-runs the snap**
  (`KiwiCmd_MouseButton`'s pause arm dropped its `KiwiCmd_MouseMove`).
- **The extrude preview is a translucent solid** (side quads + moving cap) with the
  bottom ring the wireframe never had.
- **The dock layout rebuilds once**: `kiwi_dock6.ini` -> `kiwi_dock7.ini`,
  `KW_VERSION` 3 -> 4.

### Limits, and the things that are deliberately not fixed

- **ITEM 3's "the inch snapping only goes to 32" COULD NOT BE REPRODUCED FROM
  SOURCE.** As shipped in round R, `KCMD_GRID_LADDER` already ran 0.125 -> 1024 and
  no cap at 32 exists anywhere in the chain: not in `StepGrid`, not in
  `KiwiUnits_SetGridSpacingInches`, not in `KiwiGrid_Snap`, not in the top-right pill
  (kiwi_viewcube.cpp), not in the settings field (kiwi_ux.cpp) and not in
  `KiwiCon_SnapUV`. The `32` that does exist in the grid chain is
  `grid_sizes[6] == 32.0f` (engine_stubs.cpp:771), the CLASSIC power-of-two table
  that the modern spacing is deliberately independent of. Round X therefore removed
  every upper bound it could find rather than guessing at a mechanism — the ladder
  now runs to 65536 and `KGRID_MAX_INCHES` with it. **If the stop is still there
  after this round, it is not in the ladder and the report needs a screenshot of the
  control that stops.**
- **Worldspawn brush rows CANNOT be renamed, and this will not change without a map
  format change.** A brush is not an entity, so it has no epair; it is not in the
  `.kiwi` sidecar, so it has no editor-side record; and the `.map` format carries no
  per-brush id (`brush_t::numberId` is a physics kind, brush.cpp:3524 — not an id).
  Keying names by the brush's ordinal in its owner's chain was considered and
  REFUSED: that ordinal is head-insertion order and is reordered by CSG, clone,
  delete, undo and by the load order of the `.map` itself, so a stored name would
  silently attach to a DIFFERENT brush. Double-clicking a brush row prints a console
  line pointing at "New Group from Selection", whose folder *can* be named.
  Entities, entity folders, construction groups and construction objects all rename.
- **A construction-object rename IS undoable** (one store snapshot). A construction
  GROUP rename still is not — the name TABLE is outside the snapshot, which is the
  round-W limitation, unchanged.
- **The `.kiwi` sidecar gained an optional `name "..."` line** and the format version
  stays `KIWI2`. Back-compat runs both ways by construction (an older reader skips
  unknown keywords; this reader defaults to unnamed when the line is absent). A name
  may contain spaces but **not a double quote** — `KiwiCon_SetName` strips them at
  the source, because the syntax is quote-delimited and an escape grammar is a format
  change.
- **`KCON_PLANE_REACH` (16384) is a hard bound on where a drawing tool can place a
  point.** Past that the point pins at the window edge instead of continuing. It is
  centred on the working plane's origin (which follows the last placed point), so
  the reachable area travels with the work — but a single un-anchored gesture cannot
  span more than 32768 units, and the ground-plane grid snap is likewise clamped to
  ±16384 of the camera's own ground position. Both are far outside any CoD4 playable
  area; both would bite in a synthetic test that drags across the whole world bound.
- **`KiwiCon_RayPlane` and `KiwiCon_RayPlaneBounded` are now TWO functions with
  different contracts.** The strict one is the hit-TEST form and is used only by
  `kiwi_region.cpp`'s point-in-cell arms; every drawing tool and the snap query use
  the bounded one. A new caller must pick deliberately — using the bounded form for a
  hit test would make a grazing ray report a hit at the window corner.
- **The axis-guide capture radius is now 30 px at a flat camera for EVERY axis**, not
  just the vertical (it was 16 px vertical / 10 px everything else). At a grazing
  pitch an axis guide will therefore win against a brush edge more often than it did.
  That is the intent, and the guide still loses to any nearer point-rank snap, but it
  is a real change in feel near the horizon.
- **`selUnmask_t` writes to `brushFlags` from the pick path.** It is exactly
  reversible and the restore runs from a destructor, but it does mean `Pick()` is no
  longer read-only with respect to brush state. Nothing may be added between
  `unmask.Begin()` and `Test_Ray` that can throw, pump messages, draw, or re-enter
  `Pick`. The block is three lines for that reason.
- **The self-snap band is 2 world units, i.e. wider than `KEXT_MIN_DIST` (0.5).** A
  deliberate hair's-breadth extrusion onto geometry that happens to sit within 2
  units of the source plane cannot be reached by snapping; type the value, or hold
  Ctrl to suppress snapping. Nothing legal is lost below 0.5 (the commands reject it
  as a no-op) but the 0.5..2.0 band is a real, small loss traded for the stickiness.
- **Parking no longer re-evaluates, so a command whose value depends on a pick the
  cursor stream has not yet delivered will park one frame stale.** No shipped command
  is in that position (the move stream is continuous and every one-axis gesture is
  either grab-gated or cursor-mapped on the same stream), but a future command that
  computes its value only in `MouseMove` from something other than the cursor should
  latch it rather than rely on the parking event.
- **The extrude preview's translucent fill is drawn from inside the framework's open
  `kiwi_lines` batch**, as the cut tool's quad already was. The two paths interleave
  cleanly today; if a future change makes `kiwi_lines` order-sensitive against the
  command stream, both call sites move together.
- **The prism preview has no backface culling** (the state comes from `d_white`'s
  material), so the far side of the box shows through the near side. That is what
  translucent means here and it matches the region fills; it is not the ITEM 6
  z-order bug.

## Round Y

The design rationale for every item is in **RADIANT_UX_DESIGN §55**. What follows
is only what a user or a future audit could trip over.

### Fixed

- **THE Z-ORDER BUG WAS `aa_default`, AND THE BOOT PUT IT THERE.**
  `Radiant_ApplyStartupTextureScale` (mainfrm.cpp:763) -> `Radiant_CheckTextureScale`
  (mainfrm.cpp:2914) -> `Texture_ResetPosition` (texwnd.cpp:2017) ends in
  `TexWnd_ApplyMaterialAtIndex( TexWnd_HitTest( 9, 9 ) )` (texwnd.cpp:2028-2033),
  i.e. **the first visible thumbnail becomes the current brush texture** — its own
  header comment says so (texwnd.cpp:2006-2008). The browser lists alphabetically,
  so the editor booted with the template set to `main/materials/aa_default`:
  techset **"tools"** (not `2d`), sortKey 43, refStateBits[0] `0x08128965`
  (blendOp **Add**), refStateBits[1] `0x0000000C`. Through
  `tools.techset -> vertcol_shaded_tools -> statemap "default"`, whose rule is
  `depthWrite { mtlBlendOp == Disable: Enable; default: Disable; }`, the bound
  state is **depth test ON, depth write OFF**. Round X's premise ("the template is
  `$default` and nothing changes it until the user clicks a thumbnail") was false
  about the boot, and its base-name gate compared against `$default`'s `"2d"`, so
  `"tools"` sailed through.
- **The predicate now has two arms and both were measured.** Substitute when the
  camera's technique does not write depth AND either (A) the techset base name is
  `2d` or `tools`, or (B) `sortKey <= 4`. Census of all 4355 files in
  `main/materials`: techsets `2d` (862) and `tools` (98) are HUD/editor materials
  only; sortKey 4 is the opaque world sort (2131 non-editor materials, and the
  engine special-cases exactly `sortKey == 4 && R_IsWorldMaterialType`,
  r_material_load_obj.cpp:5458).
- **`g_qeglobals.d_white` is excluded by pointer** — `white_tools` is itself a
  depth-write-free `tools` material and is round M's deliberate flat-colour handle
  for the see-through tool volumes.
- **The substitute is a ladder**: `$default3d` then `caulk`, each validated with
  the same predicate, with a console warning naming the rung taken and a louder one
  when neither is usable.
- **The boot template is RESTORED to `$default`** after the startup texture-scale
  pass, using step 6a-pre's own `SetMaterial` + `Init_MaterialLayer` and its own
  sample size — so new geometry stops acquiring a tool material and `.map` output
  is exactly the un-clobbered boot's.
- **`KiwiMatInfo`** (`KIWI_CMD_MATINFO`, palette-only, unbound) prints the ground
  truth for the face under the cursor and for the template.
- **Clicks**: the sliver marquee (kiwi_boxselect.cpp), the focused-text-field
  hover kill (imgui_shell.cpp), the grid-popup viewport claim (kiwi_viewcube.cpp),
  the cross-object double-click (kiwi_selext.cpp) and the skipped-frame double
  dispatch (imgui_shell.cpp) are all fixed. See §55.6.
- **The split tool draws and snaps to the face centre**; **the bevel has a
  lollipop that goes red out of range**; **the cut disc faces the camera**; **the
  outliner column is 0.125 wide** (`kiwi_dock8.ini`, `KW_VERSION` 5).

### Limits, accepted costs, and things deliberately not done

- **Arm B (`sortKey <= 4`) catches 13 non-editor materials in this asset set** —
  `cloud_dust_mote` (particle_cloud), the eleven `gfx_distortion_*`
  (distortion_scale_zfeather), `glow_apply_sky_bleed` and `javelin_overlay_grain`.
  All are FX materials that declare themselves opaque and blend anyway; none is a
  brush-face material anyone applies from the texture browser. If one of them is
  ever painted on a brush it will draw as the substitute in the CAMERA VIEW only —
  the `.map` is untouched and the compiler sees the real material. **This is the
  place to look if a surface starts drawing as a checkerboard.** Run `KiwiMatInfo`
  on it: the readout says whether it was substituted and why.
- **`SORTKEY_DECAL` (24) IS NOT THE OPAQUE/TRANSPARENT BOUNDARY** and must not be
  used as one. CoD4's shipped world DECALS sit at sortKeys 9..12 — 434 of them,
  alpha-blended and correctly depth-write-free. A `sortKey < 24` gate would
  substitute a checkerboard for every decal in the game. This was checked before
  the arm was written and is recorded here so it is not "generalised" later.
- **A material on the `2d` or `tools` techset that a user deliberately paints on a
  brush and wants to see BLENDED cannot be seen blended in the camera.** That is
  the intended trade: those techsets are HUD and tool art, and a brush face wearing
  one is broken for world geometry by construction. Tool VOLUMES are unaffected —
  they go down round M's flat-colour arm with `d_white`, which is excluded.
- **`Texture_ResetPosition` is UNCHANGED and still applies the first visible
  thumbnail.** Changing the texture scale from the menu mid-session will therefore
  still change the current material, which is its faithful behaviour and is
  reasonable when the user is looking at the browser. Only the BOOT call is
  corrected, and it is corrected by restoring the seed rather than by choosing a
  different material — `.map` output does not move.
- **Mode 2 + Ctrl no longer subtracts.** While Ctrl is held in EDGE mode the
  grammar is: Ctrl = lines only, REPLACE; Ctrl+Shift = lines only, ADD. Subtracting
  in edge mode requires releasing Ctrl (and is unchanged in every other mode). This
  is the directive's own suggestion taken literally; plain mode 2 is byte-for-byte
  unchanged.
- **The lines-only filter does not clear the brush selection.** A filter that also
  dropped the other selection would be a second act nobody asked for. So a Ctrl box
  select in mode 2 leaves whatever brushes were selected exactly as they were.
- **A SLIVER marquee that finds no brushes falls back to a CLICK, and therefore
  skips `KiwiConSel_ApplyRect`.** A deliberately thin (< 8 px on one axis) marquee
  drawn to catch several construction lines will now select only the one under the
  press pixel. Draw a wider box. The alternative — asking the construction rect
  whether it found anything first — needs a return value `KiwiConSel_ApplyRect`
  does not have; it is a one-line change if this ever bites.
- **`AllowWhenBlockedByActiveItem` on the viewport image means a click into the 3D
  view while a text field is focused now BOTH defocuses the field and selects.**
  That is the intent (it is what every other application does), but it is a
  behaviour change for anyone who had learned to click twice.
- **`s_frameLive` gates the whole post-present dispatch.** If a future change makes
  `ImGuiShell_DrawOverlay` early-out routinely rather than exceptionally, viewport
  input stops entirely rather than degrading. The stuck-drag guard still runs on
  the skipped path (it is physical-key based), so a gesture cannot latch on.
- **The double-click gate needs the first click to have SELECTED something on that
  brush.** Double-clicking a brush that is not selected at all — e.g. the very
  first interaction with it, if both clicks land inside 300 ms and 6 px — now
  selects it plainly instead of select-connected. Click once, then double-click, or
  use the palette's "Select Connected". This is the narrowing the fix is made of.
- **The construction plane now CHANGES when the camera is on an axis view**, and
  that is visible even when no tool is running (the view-cube face click sets it
  and prints). A user who had deliberately set a plane and then snapped the camera
  to an axis will find the plane re-seated to the view axis at the next tool start.
  The plane's POSITION along the new normal is preserved, so the work does not
  move; only the orientation does.
- **Rung 0 uses the LIVE camera, so a camera within 3 degrees of an axis counts.**
  `KVC_VIEW_ALIGNED` is cos(3 degrees) and is shared with the Alt+MMB stepper, so a
  hand-orbited near-axis view will also take the view plane. That is intended (it
  is the same threshold that already decides "am I on this view") but it means
  there is no way to be *nearly* on an axis view and still get the face rungs —
  orbit further off, or set the plane explicitly from the palette.
- **The plane NORMAL is now an axis-guide candidate**, so on a vertical working
  plane there are up to four guides instead of three. With the round-X 30 px flat
  capture radius that is more competition for a click near the anchor. The
  world-vertical dedup means it never doubles up on Z.
- **The bevel lollipop makes `KiwiGizmo` stand down during a bevel**
  (`GizmoUsable()` refuses whenever a lollipop is wanted). No gizmo was drawn for
  an edge selection anyway, but the coupling is now real: a future gizmo for edges
  would have to arbitrate.
- **`HudInvalid()` now drives the lollipop colour for EVERY host**, not just the
  bevel. The face push/pull and both extrudes will go red when their own §19 gate
  refuses the value, which is new but is exactly what their previews already did.
- **The cut disc's winding fix does not change the region fills or `DrawQuad`.**
  Those emit their own geometry with their own normals and have not been reported;
  if one of them turns out to be one-sided too, the same one-sign fix applies.
- **NOT FIXED, deliberately: a construction line within `KCON_CLICK_PIXELS` (14)
  beats a brush FACE or OBJECT hit at any depth** (kiwi_boxselect.cpp's
  `conWins = !hit.valid || !brushPointish || (conDist < hit.screenDist)`; a face or
  object hit has `screenDist` 0 by definition, so `!brushPointish` is what decides
  it). This can read as "the click was ignored" when scaffolding is over
  brushwork — but it is shakeout F's documented rule, it selects something visible
  rather than nothing, and the click IS answered. Changing it is a selection-grammar
  decision, not a bug fix. Hide the construction geometry (H) or switch mode.
- **NOT FIXED: an LMB press while another mouse button is held is dropped.**
  `ImGuiShell_ViewportInput`'s `owns` branch tests only `IsMouseReleased`, so a left
  click during an RMB look or MMB orbit never reaches a viewport. That is arguably
  correct (a drag owns the mouse) and changing it risks the stuck-drag class of bug
  the guard exists for; recorded here because it is a real way a click goes nowhere.

## Round Z

The design rationale for every item is in **RADIANT_UX_DESIGN §56**, decision log
D-Z1..D-Z8. What follows is only what a user or a future audit could trip over.

### Fixed

- **TOOL SWAPS OUT OF A LIVE GESTURE.** `G` / `R` / `S` now take over whatever
  transform is running, in any order, instead of being swallowed by
  `KiwiCmd_KeyDown`'s catch-all rung. The paste / clone / `Shift+D` auto-entered
  Move (`KiwiCmd_AfterPaste`, kiwi_command.cpp) is the reported case and needs no
  deselection first. A gesture that has APPLIED something is COMMITTED (its undo
  record closes on its own edit); one that has not is CANCELLED record-free. This
  is Plasticity's `interrupt()` fork (CancellableRegistor.ts:48-70) with a better
  question asked of it. New: `KiwiEditorCommand::CanSwapTo` / `::GestureMoved`
  (kiwi_command.h), `SwapVerb` + one funnel rung (kiwi_command.cpp).
- **SNAPPING IS OPT-IN FOR THE THREE ONE-AXIS GESTURES.** Face push/pull, region
  extrude and face extrude / un-extrude now drag RAW by default — no geometry
  snapping AND no grid quantisation — and snap only while **Ctrl** is held. Arm 0
  of kiwi_snap.cpp is an XOR against `KiwiCmd_SnapOptIn()` rather than an
  unconditional suppression. Every other command is byte-for-byte unchanged.
- **A SURFACE SNAP IS RESOLVED AGAINST THE TARGET FACE'S PLANE.** `KiwiSnap_AxisDepth`
  (kiwi_snap.h/.cpp) intersects the plane with the gesture's axis, so the whole face
  gives one answer instead of one per cursor pixel. Point candidates (vertex, edge,
  both midpoint kinds, construction anchor / intersection, face CENTRE) are still
  projected exactly as before.
- **THE PIVOT WAS COUNTED TWICE, AND THE GIZMO DREW THE DOUBLE.** `ApplyPivot` wrote
  a CURRENT world position into `m_ref`, which is the BASE of the invariant
  `live == m_ref + m_total`, so the gizmo, the marker, the snap reference and the
  commit ride were all off by exactly the distance the move had travelled — the
  reported 101.5 in. Fixed at the source by subtracting the applied delta.
  `PivotAnchor()` and `BeginPivot` now read the LIVE anchor too.
- **ROUND T-7's SNAP-QUERY REDIRECT IS REVERSED.** The snap query during a
  pivot-anchored move is asked AT THE CURSOR again, so hovering a corner of another
  brush lands the pivot on that corner.
- **THE BOTTOM ROWS STACK.** `KiwiHud_BandBegin` / `KiwiHud_BandTake`
  (kiwi_hints.h) allocate one vertical slot per bottom-anchored overlay, and the
  numeric HUD drops its grammar tail while the chip strip is up.

### Limits, accepted costs, and things deliberately not done

- **A FACE PUSH THAT HAS ALREADY MOVED WILL NOT SWAP.** `KiwiMoveCommand::CanSwapTo`
  refuses it, because round K's after-confirm deselect clears the selection on that
  path and the incoming verb would get nothing. Press Enter / RMB to confirm, then
  `R`. An UNMOVED face push swaps fine (it cancels). The construction move and a
  live `V` pivot placement are refused on the same "there is nothing sane to hand
  over" grounds.
- **A SWAP OUT OF A MOVED GESTURE LEAVES AN UNDO RECORD BEHIND, ALWAYS.** That is
  the design (D-Z1), but it means `G`, drag, `R` produces TWO undo entries where a
  user might have expected one act. `Ctrl+Z` twice gets back to before the paste.
- **THE SWAP LIST IS THE THREE TRANSFORMS AND NOTHING ELSE.** `E`, `Q`, `C`, `Ctrl+R`
  and the creation chords still go through round N's narrower `PreemptVerb` rung,
  which only fires on an UNMOVED auto-entered gesture and only ever cancels. So
  pressing `E` half way through a deliberate move is still swallowed. Widening the
  list means committing an edit to run a verb that may not act on the surviving
  selection, which is a bigger decision than a shakeout round.
- **CTRL NOW MEANS TWO DIFFERENT THINGS DEPENDING ON WHAT IS RUNNING.** Suppress
  during a Move/Rotate/Scale of objects, a drawing tool, a bevel…; ENABLE during
  the three one-axis gestures. The chip strip says which (`Ctrl — Snap` appears
  only for the opt-in commands), and D-Z2 argues the asymmetry, but it is a real
  thing to learn and it is the deliberate cost of the directive.
- **THE ONE-AXIS GESTURES LOST THEIR DEFAULT GRID QUANTISATION TOO**, not just
  geometry snapping. That is what "should not snap by default" was read to mean
  (and it is what makes a cluttered scene usable), but it does mean a plain
  push/pull now lands off-grid unless the user holds Ctrl or types a value. Round
  P's absolute-grid work (§50) is intact and runs the moment Ctrl is held.
- **A FACE EDGE-ON TO THE PUSH AXIS NO LONGER SNAPS AT ALL** (`KSNAP_AXIS_PARALLEL`,
  0.05 ≈ 87°). Pushing a floor upward and grazing a wall used to produce *some*
  number; it now produces none and the drag continues raw. That is intended — the
  old number was the cursor's height on the wall — but it reads as "the snap stopped
  working" if that is the case being aimed at. Aim at the wall's TOP EDGE or a
  corner instead; those are point candidates and still answer.
- **`SNAP_FACE_CENTER` IS TREATED AS A POINT, NOT AS ITS FACE.** Snapping a push to
  a face centroid gives the centroid's projection, which for a face NOT parallel to
  the push axis is not the same as the plane intersection. It is a deliberate single
  point (shakeout F produced it as one) and the accents draw it as one, so
  projecting it is the honest answer to what the user aimed at. Recorded because
  the two arms can disagree by design.
- **THE PIVOT FIX CHANGES WHERE A MID-GESTURE `V` LANDS THE SESSION PIVOT.** Before,
  a `V` placed after travelling stored a pivot offset by that travel (and the next
  gesture inherited it). Sessions that had learned to compensate will find the
  pivot where they actually clicked now.
- **THE RMB CONFIRM ACCEPTS UP TO 8 PIXELS OF CAMERA PAN, AND DOES NOT UNDO IT.**
  Between 5 and 8 px the camera pans AND the gesture confirms. Deliberate — see
  §56.6; `KiwiCam_PanDrag` has no exact cheap inverse and the amount is invisible.
  The CONTEXT MENU still needs a release inside 4 px, so a 6 px RMB tap with nothing
  running does nothing at all rather than popping the menu. That gap is the price of
  keeping the menu tight; it was not reported and is one constant away if it bites.
- **NO TIME COMPONENT ON THE RMB CLICK TEST.** Plasticity pairs its 4 px with 200 ms
  (KeyboardEventManager.ts:12-13); this layer has no press timestamp and none was
  added. A very slow, very small RMB drag is therefore a confirm here and would be a
  pan there.
- **THE BOTTOM BAND STACKS VERTICALLY AND IGNORES THE HORIZONTAL.** A narrow
  bottom-centre HUD and a narrow bottom-left readout that would not have collided
  are still put on separate rows, so the band can climb higher than strictly needed
  on a busy frame. D-Z6 says why packing was refused. If the band runs out of room
  (a very short viewport) every remaining taker is clamped to the top of the image
  and they overlap there — visibly, on purpose.
- **THE NUMERIC HUD'S GRAMMAR TAIL IS GONE WHILE THE CHIPS ARE UP.** Anyone reading
  the confirm keys off that line must now read them off the chips. Toggling hints
  off (`KiwiHints_SetShow`) restores the tail in full.
- **NOT FIXED: an LMB press while another mouse button is held is still dropped**
  (round Y's note, `ImGuiShell_ViewportInput`'s `owns` branch). Unrelated to this
  round's RMB work and unchanged by it.

## Round AA

The design rationale for every item is in **RADIANT_UX_DESIGN §57**, decision log
D-AA1..D-AA10. What follows is only what a user or a future audit could trip over.

### Fixed

- **THE TEXTURE BROWSER SHOWS EVERYTHING AGAIN, AND THE TEMPLATE IS STILL
  `$default`.** Round Y's restore at the tail of `Radiant_ApplyStartupTextureScale`
  was a verbatim excerpt of `Radiant_SeedCurrentTexdefs` — `SetMaterial` +
  `Init_MaterialLayer`. `SetMaterial` (materialdef.cpp:101) reaches
  `Texture_GetHandle` (texwnd.cpp:313), which REGISTERS into the browser's own
  material list and sets `is_in_use`; the seed runs those calls BEFORE
  `Load_Materials`, the restore ran them after the browser was built and after
  `Texture_ShowAll`. The restore is now a byte COPY of a snapshot the seed takes
  (`Radiant_Kiwi_SeedSnapshotLayer0`) and makes no call at all, and
  `Texture_ShowAll()` is additionally asked AFTER the scale pass so the boot's
  browser state stops being order-dependent.
- **EVERY TRANSLUCENT FILL IN THE KIWI LAYER IS TWO-SIDED NOW.** Round Y fixed the
  cut disc; five other emitters had the same fixed-winding defect against
  `white_tools`' `GFXS0_CULL_BACK`. New shared `KiwiTris_OrientToEye`
  (kiwi_lines.h TRAP 3) flips a triangle's last two indices when its geometric
  normal faces away from the eye — PER TRIANGLE, so closed volumes get both
  halves. Applied to the region fill (the reported one), the hover / selected face
  fills, the boolean operand fills, the extrude prism (cap + sides), and the split
  sweep quad.
- **A CHAINED POLYLINE IS COPLANAR BY CONSTRUCTION.** The working plane latches at
  the first placed point and every later point is projected onto it, so a
  rectangle drawn part-on and part-off a brush closes as a real region instead of
  being silently rejected as a skew ring. `PushPoint`'s plane re-seat now happens
  only for the first point. This is Plasticity's `restrictToPlaneThroughPoint(p1)`
  shape (RectangleCommand.ts:65 and seven siblings).
- **THE LOOP-CLOSING POINT IS A SNAP.** New snap arm 0b: the open chain's first
  point, at `PICK_VERT_PIXELS`, reported as `SNAP_ENDPOINT`, offered only once the
  chain holds >= 3 points (Plasticity's own `canBeClosed`, CurveFactory.ts:185-187).
- **THE WORKING PLANE IS VISIBLE WHILE A DRAWING TOOL IS LIVE** — a bounded square
  plus a cross at the anchor, pixel-sized, fading with the grazing angle the way
  Plasticity's construction grid does (FloorHelper.ts:124-135).
- **`KiwiSnap_AxisDepth` REACHED THE LAST TWO SITES THAT NEEDED IT**: the
  bevel/chamfer drag (kiwi_patchfillet.cpp) and the X/Y/Z-locked object move
  (kiwi_transform.cpp `CON_AXIS`). Full sweep list in §57.5.
- **PICK MODES 3 AND 4 EXCLUDE THINGS AGAIN.** Mode 4 picks brushes and entities
  only; mode 3 picks brush faces and construction REGION faces but no construction
  LINES. Both gates test EXACT mask equality so mode 5 keeps picking everything.
  The 14 px line-beats-face rule is unreachable in those modes as a consequence,
  with no constant changed.

### Limits, accepted costs, and things deliberately not done

- **A FREE 3D POLYLINE NOW NEEDS THE Z LOCK PER RISING SEGMENT.** The chain latch
  projects every point after the first onto the working plane, so a chain can no
  longer wander in three dimensions just because the snaps happened to land on
  surfaces at different heights. `Z` is the escape and it is the only one (D-AA5).
  Before this round a "3D" polyline was possible but was never REQUESTED — it was
  whatever the snap rungs produced, which is the same accident that broke the
  reported rectangle.
- **THE FIRST POINT IS STILL FREE, SO THE PLANE STILL DEPENDS ON WHERE YOU START.**
  The latch makes the chain consistent, not the plane predictable-in-advance. If
  the first point lands on a face 64 units up, every later point in that chain is
  on that face's plane. That is the intended reading of "draw on the thing I
  started on", but it means an accidental first click on the wrong surface now
  affects the whole shape rather than one point. Esc clears the chain.
- **THE SPLIT TOOL WAS NOT CONVERTED TO `KiwiSnap_AxisDepth`, WHICH IS WHAT THE
  DIRECTIVE ASKED FOR.** Its reduction axis lies IN the face being split, so every
  SNAP_FACE candidate there is near-parallel by construction and `AxisDepth` would
  refuse all of them; the tool already excludes SNAP_FACE and already tests plane
  membership correctly. Doing what was asked would have replaced a correct answer
  with no answer. See §57.5 and D-AA8. If the split tool is still misbehaving the
  cause is somewhere else and a fresh report with what it does would help.
- **THREE ONE-AXIS GESTURES STILL HAVE NO SNAP ARM AT ALL** — the primitive HEIGHT
  stage, `KiwiBevelCommand`'s Inset, and Scale. They store a `snap_result_t` and
  never read it (`SnapActive()` in kiwi_bevel.cpp has no callers anywhere). That is
  an adjacent gap, not this round's bug, and it was recorded rather than filled.
- **THE LOOP-CLOSING SNAP OUTRANKS EVERY OTHER CANDIDATE**, including a brush
  vertex sitting under the cursor. That is a deliberate deviation from Plasticity,
  which ranks it flat with every other point (D-AA7). Consequence: within 8 px of
  an open chain's first point you cannot snap to anything else. Move the cursor
  away and everything answers normally, and the arm is inert unless a chain of 3+
  points is open.
- **NO HYSTERESIS WAS ADDED TO THE PLANE RUNGS, AND THAT WAS THE FINDING.**
  `KiwiCon_AutoPlaneForTool` runs once per gesture, so the plane cannot flip
  mid-hover and there is nothing to damp. If a plane still changes under a live
  tool, that is a NEW bug and not this one.
- **THE WORKING-PLANE INDICATOR SHARES THE FRAMEWORK'S LINE BUDGET** and is drawn
  FIRST, so on a frame where the batch is exhausted the indicator is what gets
  dropped rather than the geometry being placed. It also does not draw at all
  below a grazing threshold, which is intended (edge-on it would be one bright
  smear) but reads as "it disappeared" if you orbit into the plane.
- **MODE 3 AND MODE 4 MARQUEES NO LONGER CLEAR THE CONSTRUCTION SELECTION.** The
  refusal in `KiwiConSel_ApplyRect` returns before its own clear, on purpose: a
  brush-mode marquee has no opinion about construction geometry. So a construction
  selection made in mode 2 survives a mode-4 marquee. Deliberate, but it is a
  behaviour change for anyone who used a mode-4 drag to clear everything.
- **NOTHING WAS DONE ABOUT `Texture_ShowInuse` NARROWING THE BROWSER ON A MAP
  LOAD** (map.cpp:546). That is the ported binary's behaviour and it is correct —
  but it IS the other way a user ends up looking at four materials, and it is worth
  knowing that `Textures -> Show All` (Ctrl+A) is the one-key answer.

### Round AA — items 8 and 9

**Fixed**

- **THE LINEAR ARRAY TAKES A CONSTRUCTION LINE AS ITS VECTOR.** Click a line while
  the linear array is live and the copies span that line: `spacing = length /
  (count - 1)`, which is Plasticity's default `'extent'` mode
  (ArrayFactory.ts:119-146, RectangularArrayCommand.ts:35-47). The derived spacing
  is shown as a read-only numeric field. The hand-drag mode is untouched.
- **THE ARRAY MEASURES FROM THE CUSTOM PIVOT (V) WHEN ONE IS SET.** For the radial
  array the pivot IS the rotation centre; for the linear array it is the origin the
  step is measured from and it decides which end of a picked line the array runs
  away from. Read through the existing `KiwiXform_PivotOverride`.
- **THE DIFFERENCE TOOL (Q) TAKES MANY TOOL BRUSHES.** Shift+click adds (and
  removes), Shift+drag box-adds a batch, and one confirm subtracts all of them from
  all intersected targets in ONE undo bracket. The carve is a cascade — carve by
  tool 0, feed the survivors through tool 1, and so on — never a union of the tools.
  Esc pops the last tool.

**Limits, accepted costs, and things deliberately not done**

- **THE BOOLEAN CAN NO LONGER PARK.** `WantsClicks()` is now true in BOTH stages so
  further tools can be picked while the preview is live, and `KiwiCmd_Pause`
  refuses click tools. The `KiwiCmd_Resume()` on the Esc path is therefore a no-op
  now; it was kept (with its comment corrected) rather than deleted.
- **THE BOOLEAN'S SNAP MARKERS AND FRAMEWORK CHIPS NOW DRAW IN BOTH STAGES.**
  kiwi_snap.cpp's axis guides / face accents and kiwi_hints.cpp's click-tool chip
  set key off `WantsClicks()`, which used to be false in the live stage. Cosmetic,
  and arguably more correct now that clicks really are live throughout — but it IS
  a visible change to a screen the user knows.
- **A PIECE SWALLOWED BY A LATER TOOL IS NOT A REFUSAL.** Only a target that ends
  with zero pieces refuses. For a single tool this is byte-for-byte the old
  behaviour; with several, a thin fragment consumed by the second crack of a
  sidewalk is accepted rather than aborting that whole target.
- **THE MARQUEE RUNG IS SHIFT-GATED AT THE DISPATCH SITE.** A bare LMB press while
  any command is live still means exactly what it meant before, for opted-in and
  opted-out commands alike. Only Shift+LMB reaches the new gesture, and only for a
  command whose `WantsMarquee()` is true (today: the boolean, and nothing else).
- **BOX-ADDED TOOLS ARE OBJECT-GRANULARITY ONLY**, regardless of the current pick
  mode. `KiwiBox_CollectBrushes` is pinned to `SEL_OBJECT` — a boolean tool is a
  whole solid and there is no other reading — so a Shift+drag in face mode still
  collects brushes.
- **THE ARRAY TAKES CONSTRUCTION LINES ONLY, NOT BRUSH EDGES.** The brief said
  "line"; the edge arm is a separate pick and was left out of scope. It is a clean
  later addition (the adopt path takes plain endpoints and does not care where they
  came from).
- **THE ARRAY'S LINE MODE RELEASES ON A 48 px DEADZONE FROM THE PICK PIXEL, NOT ON
  THE NEXT MOUSE MOVE.** The linear array has no grab gate, so "drag" and "move the
  mouse" are the same event; a raw release would end line mode at the next twitch.
  The release always prints and the HUD names the mode in force, but 48 px is a
  number that will want tuning if it turns out to fight anyone's hand.
- **THE RADIAL ARRAY GAINED NO LINE MODE.** A rotation has no line vector. Its only
  change this round is that it revolves about the pivot when one is set.
- **THE ARRAY DOES NOT MOVE THE ORIGINALS ONTO THE PIVOT.** The copies are
  distributed FROM the selection; teleporting the originals would make this a
  gesture that modifies existing brushes and would need the `Undo_AddBrushList` half
  of the bracket that `Perform` deliberately does not open.

### Round AA — item 4

**Fixed**

- **MATCH FACE ACCEPTS THE CHAMFER CASE.** The refusal was never about skew and was
  never in kiwi_matchface.cpp: taking the target's plane verbatim makes the two
  faces COPLANAR, and the trial's result gate rejected the intermediate brush at
  kiwi_validity V5 *"duplicate plane"*. V5 is unchanged (a duplicated half-space on
  disk is genuinely ambiguous to the compiler); the trial now gates the brush MINUS
  the source face and, on success, removes it. Planarizing a chamfer onto its
  neighbour and deleting the chamfer are the same act arriving from two directions.
- **REMOVE FACE WORKS IN FACE MODE, WHICH IS THE ONLY MODE IT MATTERS IN.** Round
  T-4's DELETE rung sat BELOW `if ( g_activeCommand ) return KiwiCmd_KeyDown(...)`,
  and in Face mode a face selection IS a live PAUSED push/pull (kiwi_boxselect.cpp
  auto-enters and pauses `KIWI_CMD_MOVE` on the click that makes the selection) — so
  the key was swallowed in precisely the state the verb needs. Same trap as round
  N's Ctrl+R and round U's Ctrl+1..4. New rung immediately above the swallow, gated
  on `PreemptIdle()` and on the construction selection not owning DELETE.
- **A REMOVAL IS ACCEPTED ONLY IF THE SOLID STILL CLOSES.** New
  `KiwiValid_BrushCloses` (V8): grow the def's bounds into a probe box, rebuild each
  face with the ported `Brush_MakeFaceWinding`, and refuse if any winding TOUCHES
  the box — that touch is the leak. Needed because `Brush_BuildWindings` has no
  world box, so an open cell still has finite bounds and V7 passes it. Material and
  texture lock on the survivors needed nothing: `Brush_RemoveFace` memmoves whole
  `face_t` records and no surviving PLANE moves.

**Limits, accepted costs, and things deliberately not done**

- **MATCH FACE ONTO A COPLANAR NEIGHBOUR NOW DELETES A FACE.** That is the fix, but
  it means the verb can reduce the brush's face count where before it only ever
  reoriented a plane. It is one undo record either way, and it refuses (with a
  status line, changing nothing) when the result would not close.
- **MATCH FACE'S ORDINARY PATH NOW ALSO RUNS THE CLOSURE TEST.** A match that
  currently produces a closed brush still passes untouched; only a match that would
  OPEN the solid is newly refused. If a match that used to "work" now refuses, the
  brush it used to produce was open.
- **REMOVE FACE STILL HAS NO KEY OF ITS OWN.** It is reached from the DELETE funnel
  and from the palette by name. Binding it to 0x2E would be a second competing
  answer to that key — round T's reasoning, still valid.
- **A 4-FACE BRUSH REFUSES BEFORE ANYTHING IS TOUCHED.** 5 → 4 is allowed (a
  tetrahedron is the floor); 4 → 3 is V1 stated up front rather than discovered by
  the closure test.
- **V8 IS NOT IN `KiwiValid_CheckBrush`** and per-frame drags do not pay for it
  (faceCount² winding clips). If a future verb removes a half-space during a drag,
  it must call `KiwiValid_BrushCloses` itself.
- **NOT RUNTIME-VERIFIED.** Two things to exercise: select a chamfer face, press Z,
  click the neighbouring face — the chamfer should vanish and the edge come back,
  one undo; and select a chamfer face and press DEL — same result, one undo.

## Round AB

The design rationale for every item is in **RADIANT_UX_DESIGN §58**, decision log
D-AB1..D-AB12. What follows is only what a user or a future audit could trip over.

Three reports: a crash after a monitor sleep/wake, a tool gesture being cancelled
when the mouse is dragged off, and a new "auto bool" brush-count consolidation.

### Fixed

- **THE MONITOR-SLEEP CRASH IS FIXED AT ITS SOURCE: A DEVICE RESET NOW INVALIDATES
  THE EDITOR SURFACE CACHE.** `RB_EndSurfacePrologue` (rb_shade.cpp:202) was
  dereferencing a NULL `g_primStats`. The state that produced it — a `tess` batch
  leaking out of `RB_DrawEditorSkinnedCached_Sub` — came from a per-face/per-patch
  `vertHandle` cached BEFORE a device reset and used AFTER it, resolving to a NULL
  vertex buffer. Round V correctly released the editor VB pool for the reset
  (`Editor_VB_ReleaseForReset`); nothing invalidated the handles into it.
  `KiwiDevice_InvalidateEditorSurfCache` (new, kiwi_devicereset.cpp) now drops the
  cache in the same breath, using the binary's own `Brush_InvalidateVis`.
- **THE VIEWPORTS NO LONGER RENDER WHILE THE DEVICE IS LOST.** New
  `RTT_DeviceHealthy()` gates `ImGuiShell_RenderViewportsToRT` as one decision for
  all four viewports. Recovery is still driven by the compositing WM_PAINT exactly
  as round V arranged, and `RTT_Begin`'s own per-viewport check is unchanged.
- **THREE DEFENCE-IN-DEPTH GUARDS ON THE SAME CHAIN.** `Editor_VB_ForBuffer` is
  bounds-checked (it used to read `vb[-1]` for a zero handle); a MESH surf with no
  vertex buffer is SKIPPED instead of drawn through (which is what leaked the tess
  batch); `RB_EndSurfacePrologue` returns early on a NULL `g_primStats` instead of
  writing through it.
- **A DRAG THAT ENDS OUTSIDE THE WINDOW NOW COMMITS INSTEAD OF UNDOING.** The
  stuck-drag guard `ImGuiShell_ForceReleaseIfStuck` (round Z) fired correctly — "we
  own a drag and no physical button is down" — but ABORTED, which for a modal edit
  runs `KiwiCmd_Cancel` → `Undo_Undo` and prints "`<verb>` undone." It now
  synthesizes the RELEASE through the same up-handlers a release inside the image
  would have run, at the real cursor position.
- **A LIVE DRAG NEVER LOSES THE CURSOR.** While a viewport owns a drag and ImGui's
  `io.MousePos` is invalid (the `-FLT_MAX` the Win32 backend posts on
  `WM_MOUSELEAVE`, which its own fallback only repairs while the app is
  foreground), the position comes from `GetCursorPos` + `ScreenToClient`, unclamped.
- **NEW: AUTO BOOL.** Greedy pairwise CSG merge over the selection to a fixed
  point, one undo record for the whole run. Palette name "Auto Bool (consolidate
  brushes)" and a button in the CSG panel. Reports
  `Auto-bool: N brushes -> M (K merges).`

### Known limits — Auto Bool

- **IT IS NOT A SIMPLIFIER.** It only ever replaces two brushes with one brush that
  is EXACTLY their union. No face is deleted, no plane is moved, no shape is
  approximated. The volume cannot change. If you were hoping it would clean up
  overlapping or nearly-coplanar geometry, it will not, and that is deliberate.
- **IT IS GREEDY, SO THE RESULT DEPENDS ON ORDER.** Merging A+B first can make
  A+B+C impossible where merging B+C first would have allowed it. Running it twice
  on the same selection gives the same answer; running it on a differently-ordered
  selection may give a different (never a wrong, only a less thorough) one. There
  is no search and no backtracking.
- **IT REFUSES, POLITELY AND WITHOUT CHANGING ANYTHING, FOR:** fewer than 2 brushes;
  a selection containing a patch; a selection containing a fixed-size entity's
  brush; a selection spanning more than one entity. Those are `CSG_Merge`'s own
  refusals, reproduced rather than relaxed.
- **THE MERGED BRUSH TAKES THE ACTIVE LAYER, not the sources' layer**, and takes its
  owning entity from the later of each merged pair. Both are inside the ported
  `Brush_MergeList` (csg.cpp:548-555) and are what a manual Selection → CSG → Merge
  already does; over a cascade it simply happens more than once.
- **PER-FACE MATERIALS ARE THE CORE'S CHOICE.** Each surviving plane keeps the
  material of whichever contributing face supplied it. Merging two
  differently-textured brushes therefore keeps a mixture, chosen by list-walk order.
  Nothing here overrides that.
- **NO HOTKEY, EVER, AND NO AUTO-TRIGGER.** It is registered unbound. Nothing else
  in the editor chains into it.
- **IT CAN BE SLOW ON VERY LARGE SELECTIONS.** The cascade is one O(n²) bounds sweep
  per pass and repeats while anything merged. A few hundred brushes is fine; a
  several-thousand-brush selection will pause the editor for a noticeable moment.
  There is no progress indicator and no cancel.

### Known limits — the device-reset fixes

- **THE MAP IS BRIEFLY UNCACHED AFTER A WAKE.** Every brush's faceVis is dropped at
  reset and rebuilt on the first frame after the device returns. On a large map that
  first frame is slower than usual. This is the correct trade — the alternative is
  the crash.
- **A CONSOLE LINE APPEARS ON EVERY DEVICE RESET**
  (`Device reset: dropped the editor surface cache for N brush(es).`). It is
  deliberate: it is the one visible marker that a reset happened, which matters when
  diagnosing anything else that looks odd after a sleep/wake or alt-tab.
- **`Brush_MakeFaceVisuals` NO LONGER ALWAYS SYNCS THE VERSION.** While the device
  is lost it leaves the rebuild armed on purpose. The post-rebuild invariant assert
  in `Brush_CheckBuildFaceVis` is skipped in exactly that window. If a future change
  makes something rebuild faceVis during a loss and then rely on `b->version ==
  b->def->version`, it will be surprised — that is the one place the two differ.
- **AN ACCESS VIOLATION STILL BYPASSES THE RESCUE SAVE.** `Kiwi_FatalRescue` is only
  reached from `Com_Error` (engine_stubs.cpp:351). This round removed one AV; it did
  not add a structured-exception handler, so any other AV still loses the map. That
  remains open and is worth a future round.
- **THE `Editor_VB_FreePoolList` ALLOCATOR MISMATCH IS UNFIXED.** It `free()`s nodes
  allocated with `operator new` (r_ed_vertbuf.cpp:106-119 vs :85, :98), while
  `Editor_VB_ReleaseForReset` uses `operator delete` for the `edMatVertBuf`. Benign
  on MSVC (both reach the CRT heap) and unrelated to the crash, so it was left
  alone rather than churned on the reset path.

### Known limits — the drag fix

- **ABORT STILL EXISTS AND IS STILL REACHABLE.** `ImGuiShell_ReleaseViewportInput`
  falls back to `ImGuiShell_AbortViewportInput` when there is no button left in
  `io.MouseDown[]` to release. That path can still cancel a modal gesture; it is now
  the exception rather than the rule.
- **THE CAMERA'S RMB RELEASE IS STILL ROUTED AWAY FROM `VP_Up`.** A synthesized
  release on the camera goes through `KiwiVP_CameraButtonUp` only, because
  `CamWnd_OnRButtonUp` would pop the classic context menu. So a legacy-profile
  camera RMB drag that loses its release edge is torn down rather than replayed —
  unchanged from round Z, and correct, because a context menu popping under a
  cursor that has left the window is worse than either.
- **NOT RUNTIME-VERIFIED.** Three things to exercise: grab the move gizmo, drag the
  cursor right off the app window, release outside — the move must STAY, with no
  "undone" line; do the same but alt-tab away mid-drag and release over another app;
  and drag an XY-view pan off the window and back to confirm the pan's own
  cursor-recentring is unaffected.

### Not runtime-verified (this whole round)

Nothing in round AB was built or run — the user builds. The device-reset path in
particular can only be exercised by an actual sleep/wake or an alt-tab from
exclusive fullscreen, so the fix is reasoned rather than observed.

## Round AC (single item)

- **"Textures panel only shows in-use textures after map load" is the BINARY's
  behaviour, not a port bug**: every map load ends in `Texture_ShowInuse`
  (map.cpp:546, faithful to 0x45B850); the way back is Textures→Show All
  (32973). The discoverability gap is real under the 3D-first shell, so the
  Textures panel now carries `Show: [All] [In Use]` buttons (imgui_shell.cpp,
  ROUND AC block) calling the same texwnd primitives as the menu handlers —
  minus 32973's script-group trigger overload, which is a selection act and
  would be a baffling side effect on a browser button. The buttons show no
  latched state: the underlying flags are per-texture (`is_in_use`), map load
  and other paths mutate them independently, and a fake radio state would lie.

## Round AD

One item: the map load that finished loading and then never drew anything again.
The design rationale is in **RADIANT_UX_DESIGN §59**, decision log D-AD1..D-AD9.

### Fixed

- **THE EDITOR NEVER RELEASED ITS UNMANAGED (D3DPOOL_DEFAULT) IMAGES, SO A DEVICE
  RESET COULD NOT SUCCEED ONCE A MAP WAS LOADED — this is the root cause.** The
  engine releases them in `R_ReleaseLostImages`, which is
  `DB_EnumXAssets(ASSET_TYPE_IMAGE, R_FreeLostImage, ...)` — and `DB_EnumXAssets` is a
  no-op stub in Radiant (engine_stubs.cpp:537). A water image is
  `IMG_CATEGORY_WATER` and `Image_BuildWaterMap` creates it `D3DUSAGE_DYNAMIC`, which
  `Image_Create2DTexture_PC` turns into **D3DPOOL_DEFAULT**. One such image alive at
  `Reset()` time makes it return `D3DERR_INVALIDCALL` — forever, because the editor
  retries the same call every tick. New `KiwiDevice_ReleaseUnmanagedImages` /
  `KiwiDevice_RebuildUnmanagedImages` (kiwi_devicereset.cpp) walk
  `imageGlobals.imageHashTable`, which is where the editor's images actually live, and
  apply the engine's own per-image rules.
- **AN `INVALIDCALL` RESET RETRY IS NO LONGER A BARE RETRY.** Round V's
  `s_releasedForReset` latch made every retry skip both the unbind block and the
  release cascade. That is correct for `D3DERR_DEVICELOST` and fatal for
  `D3DERR_INVALIDCALL`. `R_ResetDevice` now remembers which failure it was; an
  `INVALIDCALL` retry re-runs the full unbind and an idempotent second-chance release
  (ImGui buffers, RTT textures, unmanaged images, editor VB pool, render targets, swap
  chains) before trying again.
- **THE RECOVERY CHAIN CANNOT FAIL SILENTLY ANY MORE.** `R_TestDevice` keeps the
  `TestCooperativeLevel` HRESULT it used to discard, `R_ResetDevice` reports every
  attempt's HRESULT and which release pass ran, and the frame `WM_PAINT` reports a
  tick that rendered nothing.
- **A PERMANENT LOSS NOW SAVES YOUR MAP AND TELLS YOU.** After ~15 s of unbroken
  black, `KiwiDevice_FrameHealthWatch` writes the rescue save and pops one message box
  with both HRESULTs, the rescue path, and "restart the editor".

### What you will actually see

While the device is down, ONE line every ~2 seconds — to the editor console AND to
`OutputDebugStringA` (the Visual Studio Output window; use DebugView without a
debugger attached), prefixed `[KIWI device]`:

    [KIWI device] NOTHING RENDERED for 4032 ms. Reason: device LOST - waiting on
    TestCooperativeLevel/Reset. TestCooperativeLevel=0x88760869 D3DERR_DEVICENOTRESET;
    Reset=0x8876086c D3DERR_INVALIDCALL after 240 attempt(s), release pass:
    second-chance default-pool re-release

and, when it comes back:

    [KIWI device] recovered - frames are rendering again after 4210 ms (241 Reset
    attempt(s), last hr 0x00000000 D3D_OK / not attempted)

Plus, on any reset that actually released something, the existing one-shot console
lines: `Device reset: dropped the editor surface cache for N brush(es).`,
`Device reset: released N unmanaged (default-pool) image(s).`, and
`Device reset: rebuilt N unmanaged (default-pool) image(s).`

**The `Reason:` field is the fast diagnosis.** It names which of the eight ways
`R_SetupRendertarget_CheckDevice` can refuse actually fired (RADIANT_UX_DESIGN §59.1).
Two of them are worth reacting to on sight:
`a render target is still ACTIVE (targetWindowIndex >= 0)` means some draw path
opened a render bracket and never closed it — that is a bug at the bracket, not here.
`g_disableRendering is set` should be impossible in a living process; if it is ever
printed, the analysis in D-AD6 is wrong and wants revisiting.

### Known limits

- **THE MAP IS BRIEFLY UNTEXTURED FOR WATER AFTER A RECOVERY.** Unmanaged images are
  rebuilt in `R_RecoverLostDevice`'s tail, so they are back before the first frame —
  but the rebuild is `Image_BuildWaterMap`, i.e. regenerated, not reloaded. Any state
  a water image had accumulated is gone. That is the engine's own recovery behaviour.
- **A MID-LOAD DEVICE DEATH CAN STILL KILL THE PROCESS, AND IT IS NOT FIXED.** If the
  device dies while a map load is registering textures, `CreateTexture` fails and
  `Image_Create2DTexture_PC` raises `Com_Error(ERR_DROP, "Create2DTexture(...)
  failed")`. Inside `Brush_RealizeFaceMaterialsGuarded` that is caught and the brush's
  materials are skipped; anywhere else (`Load_Materials`, the texture browser, a direct
  `Material_RegisterHandle`) it falls through to the death arm and `ExitProcess` —
  with the rescue save, but still an exit. Making it non-fatal is NOT a one-liner:
  `Image_LoadFromFileWithReader` would then `LockRect` a NULL texture, trading a clean
  exit for an access violation. The honest fix is a load-time device gate that defers
  registration while `dx.deviceLost`, and that is its own round. See D-AD9.
- **A CATEGORY < 5 IMAGE IS NOT REBUILT BY THE NEW PASS, DELIBERATELY.** The engine's
  `R_RebuildLostImage` would `Com_Error(ERR_DROP)` for one it cannot reload, and in
  this editor an image with no texture is an ordinary missing asset. Managed images
  keep their texture across a `Reset` anyway, so there is nothing to rebuild.
- **THE 15-SECOND THRESHOLD IS A JUDGEMENT, NOT A MEASUREMENT.** A driver TDR plus a
  reinstall-class recovery can exceed it; the box says "restart" while the editor may
  still recover on its own. It is one-shot and does not stop the retries, so the cost
  of being wrong is one dismissable dialog and one extra rescue file.
- **THE HEALTH WATCH ONLY JUDGES THE FRAME WINDOW'S PAINT.** A viewport that is black
  for its own reason (its RT texture failed to create, its dock cell is 0-sized) is
  not covered — the frame is still painting, so the watch sees a healthy tick.
- **`s_escaped` IS PER-PROCESS, NOT PER-EPISODE.** The box appears at most once per
  run. A second permanent loss in the same session is logged but not popped.
- **NOT RUNTIME-VERIFIED.** Nothing in round AD was built or run — the user builds.
  The reset path can only be exercised by a real device loss (sleep/wake, alt-tab from
  exclusive fullscreen, or a driver TDR), and the specific failure this round fixes
  additionally needs a map containing a water material loaded at the time.

## Round AE (single item)

- **Textures panel search bar** (next to the round-AC Show buttons). The filter is
  the BINARY's own machinery — `TexWnd_FilterAccept`'s searchbar arm (IDB
  0x45bcdd) was ported faithfully in the original texwnd port and sat waiting for
  a UI ("searchbar_filter is false until a search UI is wired"). Wired via
  `TexWnd_SetSearchFilter` (texwnd.cpp) + an ImGui input box (imgui_shell.cpp).
  One deliberate deviation, fenced at the predicate: the binary matches PREFIX
  only; a SUBSTRING arm is added after the verbatim prefix arm, because shipped
  material names carry family prefixes ("ch_", "ac_") and a user typing "brick"
  means ch_brick_*. Case-insensitivity is structural (names lowercased at
  registration, query lowercased in the setter). Note the binary semantics kept:
  an active search REPLACES the is_in_use base predicate, so search results come
  from ALL loaded materials regardless of the Show All / In Use state — which is
  what "search for something to add" wants. Scroll re-homes to the top per edit.

## Round AF

Nine items. The design rationale for all of them is in **RADIANT_UX_DESIGN §60**,
decision log D-AF1..D-AF18.

### Fixed (bugs)

- **THE BOOLEAN PREVIEW STOPPED DRAWING AT TWELVE TOOLS, AND THE REPORT NAMED THE
  NUMBER TO THE BRUSH** (item 3). `KiwiCmd_DrawWorld` opened one 288-segment line
  batch for the whole gesture, a box brush costs 24 segments, and 288/24 = 12. Past
  that the outline arm bailed — taking the HOVER outline with it, so the editor also
  stopped saying which solid the next Shift+click would take. A second, earlier cliff
  (`KBOOL_MAX_FILL` = 8) killed the red tint from the ninth tool. Both were spent in
  PICK ORDER, which is why the operand that vanished was always the newest one.
  Now: the batch **scales** with what the command actually needs
  (`KiwiEditorCommand::LineBudget`, clamped to 1536), both budgets are spent
  **newest-first**, the hover is reserved out of the budget, and a degrade prints ONE
  console line naming the counts. `KBOOL_MAX_FILL` is 32.
- **q3 PATCHES DID NOT HIDE, AND THAT WAS THE SAME DEFECT AS "THEY ARE NOT
  SELECTABLE"** (item 9). The camera's patch pass (camwnd.cpp) never called
  `FilterBrush`, which is where the HIDDEN bit is read (filters.cpp:724,
  `(brushFlags & 5) != 0`). The convex world pass gates on it (camwnd.cpp:2477), both
  of xywnd.cpp's loops gate on it (:1085, :1163) and all three pick entries gate on it
  — so `Select_Hide` set the bit correctly, the 2D views dropped the patch, every pick
  path dropped it, **and the 3D camera kept drawing it**. Both patch sub-passes are
  now gated, which is what the binary's own single `DrawGeneralWorld_` loop does.
- **BOX-SELECTING A PATCH NOW USES ITS BOUNDING BRUSH AS WELL AS ITS CONTROL POINTS**
  (item 9), on the CROSSING path only. For a fillet patch — three or four control
  points strung along one edge — "the marquee clipped it strictly between two control
  points" was most of the patch.

### Added

- **LOFT (`L`)** — item 6, the round's centrepiece. Select one brush face and press L
  to pick the second, or select two and press L to start immediately; the second face
  can be on any solid. `D` toggles **RULED (G0)** and **TANGENT (G1)**, `Tab` cycles
  **density** and **tension**, RMB/Enter applies. It outputs N convex brushes, leaves
  both source brushes untouched, and takes ONE undo record.
- **CUT WITH A CHAIN OR A LOOP** — item 1. A click on a construction line now takes
  the **whole object**, so a plain click on a circle is the whole ring; **Shift+click
  adds another** (and shift-clicking one already in takes it out). The fence is swept
  along the objects' own plane normal and the targets are split by every plane in
  sequence, **keeping every piece** — a closed loop separates inside from outside.
- **PUSH A REGION INTO A SOLID AND YOU GET A CAVITY** — item 2. Draw a profile on a
  wall, press E and drag inward: the prism is boolean-differenced out of every brush
  it penetrates instead of landing as a second brush inside the wall. One undo record,
  amber preview, the HUD reads `CARVE (cavity)` before the confirm.
- **TRIM NOW ACCEPTS CIRCLES, ARCS AND RECTANGLES** — item 4. Cross a circle with two
  lines and click an arc to eat it; a diameter gives you two halves.
- **THE ROUND SHAPES HAVE A TYPEABLE SIDE COUNT** — item 7. `sides` is a Tab field on
  the construction circle, 2-point circle, arc and n-gon; the n-gon's `[` `]` keys and
  the field are two views of one number; the last value used is remembered across
  sessions. Cylinder and cone caps went 32 → 64, the n-gon 32 → 64.
- **CENTRE BOX (`Shift+V`)** — item 8, Plasticity's own pairing (`shift-c` corner,
  `shift-v` centre). First click is the CENTRE of the base rect, the drag is its
  half-extents, then height as usual.
- **GRID-AWARE LOOP DETECTION, AND A REASON WHEN IT FAILS** — item 5.

### What you will actually see

**The boolean, past 32 operands:**

    Boolean: 3 operand(s) are outlined but not tinted and 0 are not drawn at all —
    the preview budget is full.  The NEWEST operands are the ones you can see; every
    operand in the set is still applied on commit.

**The patch gates, on `H` (once per session, only with a patch in the map) and on
Ctrl+H (always):**

    Patches: q3 curves are currently HARD TO SELECT —  'Don't select curves' is ON
    (toolbar / command 32852).  selection mode 3 (Face) has no face on a patch —
    press 4 for Object or 5 for Everything.

…or, when nothing is in the way (Ctrl+H only):

    Patches: nothing is blocking patch selection — 'Don't select curves' is OFF, no
    curve/terrain filter is hidden, and the selection mode resolves whole objects.  A
    patch that still will not pick has no curveDef.

**Loop detection, on an Extrude that finds no region:**

    Extrude: no closed construction region.
    Construction: loop gap 0.80 at (128 64 0) — nearly closed — 2 open end(s) are not
    joined.  The weld is 2.00 (a quarter of the grid); raise the grid or move that
    point closer.

**Trim, hovering a closed loop with fewer than two crossings:**

    Trim: that is a CLOSED loop and only one line crosses it — a loop needs TWO
    crossings before an arc between them exists.  Draw a second line across it (a
    diameter gives you two halves).

**The fence cut, on the first click:**

    Cut: 32 segment(s) from 1 object(s), swept along its own plane normal — every
    piece is KEPT (a closed loop separates inside from outside).  Shift+click adds
    more, RMB / Enter cuts.

### Keymap changes (modern profile only)

| chord | was | is now | displaced to |
|---|---|---|---|
| `L` | ToggleLayers 33954 | **Loft** | ToggleLayers → `Shift+Alt+L` |
| `Shift+V` | Rectangle (centre) 34047 | **Box (centre)** | centre rect → `Alt+V` |

Both displacements were audited across the whole default command table and against
res/radiant.rc's accelerator table (which `TranslateAccelerator` consults BEFORE the
hotkey table). `Shift+Alt+L` and `Alt+V` were both free. The Layers panel is still on
the Windows menu; the centre rect is still in the palette and the `Shift+A` add menu
by name.

### Known limits

**LOFT (item 6)**

- **PLANAR PROFILES ONLY.** Brush face windings always are, so this is not a
  restriction you can hit from a face — but it is the reason the command takes faces
  and not arbitrary curves. (It is also Plasticity's own rule: `LoftFactory.ts:40-41`
  throws `"Curve cannot be converted to planar"`.)
- **G1 IS EXACT AT THE ENDS AND PIECEWISE-LINEAR IN BETWEEN.** The first and last
  station ARE the source windings and the end tangents ARE the source normals; the
  smoothness of everything between them is bought with the `density` field. That is
  what a faceted medium can offer.
- **G2 IS NOT ATTAINABLE IN BRUSHES AND IS NOT OFFERED.** Curvature continuity needs a
  curved medium. The editor has one — q3 patches — and kiwi_patchfillet.cpp already
  lays them into a chamfer, so a PATCH loft is the honest next step. It is not in this
  round.
- **INTERMEDIATE STATIONS ARE PLANARISED, and that is the loft's error term.** It is
  exactly zero at both ends and shrinks with density. Two faces at a large angle with
  a low density will show it as a slight bulge or pinch; raise the density.
- **A SEGMENT CAN BE REFUSED.** Twisted or near-degenerate correspondences fail §19,
  and a TANGENT loft that fails is rebuilt RULED for the WHOLE bridge (never per span
  — see D-AF7) with a console line saying so. If RULED also fails, nothing is created
  and the reason is named.
- **THE SOURCE FACES ARE LEFT IN PLACE.** A loft BRIDGES; it does not weld. For a
  continuous tunnel you must delete the two interior end caps yourself — select each
  and press `Delete` (Remove Face, round T). **This is not done automatically**, and
  deliberately: Remove Face is only safe when the remaining half-spaces still enclose
  a finite cell (§19 V8), the loft cannot know whether the user wants the caps gone,
  and doing it silently would turn one undo record into a three-step edit.
- **MISMATCHED CORNER COUNTS ARE EQUALISED BY SUBDIVIDING THE SHORTER RING**, capped
  at 64 corners. A 4-gon lofted to a 32-gon works and gives a fair result; a 4-gon
  lofted to a 64-gon is at the cap and cannot be equalised further.
- **NO SPINE.** Plasticity's loft has an optional spine curve (`LoftCommand.ts:23-32`);
  KIWI's path is the Hermite between the two faces and nothing steers it but
  `tension`. A construction-curve spine is future work.

**CUT WITH A FENCE (item 1)**

- **A CONCAVE FENCE MAKES MANY PIECES, AND THAT IS CORRECT.** Every plane cuts every
  piece it crosses, so an L-shaped fence through a slab gives four solids, not two.
  A classic brush cannot be concave, so there is no "the inside piece" to hand back.
  The HUD advertises the plane count before you commit.
- **THE FENCE IS CAPPED AT 64 PLANES** (`KCUT_MAX_FENCE`), the same 64 a construction
  circle is capped at, so a maximally tessellated ring fits exactly.
- **A SEGMENT PARALLEL TO THE SWEEP IS SKIPPED**, not fatal: its extrusion is a line,
  not a plane. One bad segment in a fifty-segment ring is not a reason to refuse the
  ring.
- **BRUSH EDGES AND FACES ARE STILL SINGLE-PLANE SOURCES.** An edge has no plane of
  its own to sweep along; picking one drops any fence.
- **STAGE 2 NO LONGER PARKS ON A CLICK.** It had to become a click tool for a fence to
  be reachable at all (the first click enters stage 2). The cut has no drag to park,
  so nothing is lost — but if you were relying on a stray click parking the gesture,
  it now re-picks instead. RMB/Enter and Esc are unchanged.

**THE CAVE (item 2)**

- **IT TRIGGERS ON "THE DRAG GOES INTO A SOLID", NOT ON A NEGATIVE NUMBER.** A
  negative extrude in open space still grows a brush the other way, exactly as it did.
  The probe is one point, `KEXT_MIN_DIST` along the drag from the profile's centroid.
- **A PRISM THAT SWALLOWS A BRUSH WHOLE IS REFUSED, per brush, with a message.**
  Deleting a brush the user was trying to dent is the worst possible reading of the
  gesture.
- **ONLY THE REGION EXTRUDE CARVES.** The FACE extrude (`E` with a face selected) is
  unchanged: round Q's grow / carve / destroy, where "carve" still means pushing that
  one face plane. A partial-face dent still wants a region drawn on the face.
- **CSG-USABLE BRUSHES ONLY** — patches, fixed-size entities and sub-4-face brushes
  are refused here exactly as they are by `Q`.

**TRIM (item 4)**

- **TRIMMING A CIRCLE, ARC OR RECT CONVERTS IT TO A POLYLINE.** There is no parametric
  spelling for half a circle in this store. The HUD says `· becomes a polyline` before
  the click, and the tessellation it freezes at is the object's own `segs` — which
  item 7 made a number you choose. Set the sides BEFORE trimming, not after.
- **A CLOSED LOOP STILL NEEDS TWO CROSSINGS.** One crossing does not bound an arc.
  Plasticity's own fragmenter reaches the same answer (`PlanarCurveDatabase.ts:121-123`
  plus the `> 10e-6` emission gate at :135 produce zero fragments); the difference is
  that KIWI now says so.

**LOOP DETECTION (item 5)**

- **THE WELD AND THE COPLANARITY BAND ARE NOW A QUARTER OF THE GRID SPACING**, floored
  at the old 0.5 and ceilinged at 16. That means **raising the grid loosens loop
  detection**, which is intended and is the lever the console line points you at. It
  also means that at a very coarse grid two genuinely separate endpoints CAN weld if
  they were placed with snapping off — the ceiling exists to bound that, but it does
  not eliminate it.
- **THE GAP REPORT ONLY LOOKS AT OPEN OBJECTS' ENDPOINTS.** A loop that fails because
  it is not planar enough, or because a T-junction gave a node degree 3, is not
  diagnosed by name — you get the "every open end is joined" line instead, which at
  least rules the endpoint case out.
- **ROUND AA'S CHAIN LATCH IS UNCHANGED.** A single chain still cannot leave its own
  plane, and that is still the right fix for the same-chain case.

**SEGMENT DENSITY (item 7)**

- **A SPHERE STOPS AT 16 SIDES**, not 64, because a sphere is `sides × sides` faces —
  64 would be a 4096-face brush. The asymmetry with the cylinder is the geometry's.
- **THE REMEMBERED COUNT IS ONE NUMBER FOR ALL FOUR ROUND TOOLS.** Setting 48 on a
  circle means the next arc starts at 48 too.
- **CHANGING `sides` DOES NOT RETESSELLATE AN EXISTING OBJECT.** The count is stamped
  at creation; to change a circle's density, delete and redraw it.
- **A SIDECAR WRITTEN BY THIS BUILD IS READ BY AN OLDER ONE**, which ignores the
  `segs` line and falls back to the automatic count. That is a lossy but safe
  downgrade.

### General

- **NOT RUNTIME-VERIFIED.** Nothing in round AF was built or run — the user builds.
  The items most worth exercising first, because they are the ones with the most new
  geometry code: **Loft** (try a 4-gon to a 4-gon face-to-face first, then TANGENT at
  density 8, then two faces at 90°), **the fence cut** (a circle on a floor slab),
  and **the cave** (a rectangle drawn on a wall, pushed in).
- **TWO COMMAND IDS WERE TAKEN AND FOUR MODAL IDS REMAIN** (34066..34069). The modal
  block cannot grow: `KiwiCmd_Dispatch` routes ANY id inside 34030..34069 to
  `KiwiCmd_Start` regardless of what it is, so widening it would swallow instant ids.
  A sixth modal verb past that needs a SECOND modal range and a second
  `KiwiCmd_IsModalId` clause. This is recorded in kiwi_command.h at the allocation
  table.
- **`Prefs_SavePrefs` STILL DOES NOT WRITE `SelectCurves`, DELIBERATELY.** It is a
  faithful port of the binary's own omission (prefs.cpp:279) and it is left alone.
  The practical consequence is benign — nothing ever writes the key, so
  `Radiant_ProfileGetInt(..., 1)` returns 1 at every load and the toggle cannot
  persist an "off" across a restart. It IS a one-way trap **within** a session, which
  is why the round-AF diagnostic names it.


## Round AG

Eleven items. The design rationale for all of them is in **RADIANT_UX_DESIGN §61**,
decision log D-AG1..D-AG23.

### Fixed (bugs)

- **THE LIGHT-BLUE FILL ONLY APPEARED SOMETIMES, AND "SOMETIMES" WAS THE GRID**
  (item 2). Round AF scaled the region weld to `grid * 0.25`, which is correct for
  USER-PLACED endpoints (they snap, so two the user meant to keep apart are a full
  step apart) and **false for TESSELLATED vertices** — a circle/arc edge is
  `0.098 * radius` and has nothing to do with the grid. At grid 16 the weld is 4.0
  and a radius-32 arc has 3.1-unit edges, so `kiwi_arrange.cpp`'s `NodeFor` welded
  each arc vertex onto its predecessor, every one of that arc's fragments came back
  with `n0 == n1` and was dropped, **the arc left the arrangement entirely**, and the
  cells it bounded stopped existing. A rectangle at the same grid sailed through.
  Now: a weld may never exceed **40% of the finest edge it is applied to**
  (`KiwiRegion_WeldFor`), computed per call from the actual input.
- **…AND THE LOOP CAP WAS THE TESSELLATION CAP** (item 2). `KREG_MAX_LOOP` was
  `KCON_SEGS_MAX` (64), but a DERIVED cell is bounded by parts of SEVERAL objects —
  a 64-segment circle with one chord across it makes a 66-vertex cell, which was
  over the cap and dropped **silently in two places**. An arch profile on a
  tessellated arc is exactly that shape. `KREG_MAX_LOOP` is 128, `KEXT_MAX_PROFILE`
  is now *defined as* `KREG_MAX_LOOP` so the "extrudable ⇔ filled" invariant is
  structural, `KEXT_MAX_PIECES` is 128, and both drops print.
- **…AND THE ARRANGEMENT NEVER RAN ON A PLANE WITH NO OPEN OBJECT ON IT** (item 2).
  PASS 3 lived inside the PASS-2 open-object group loop, so two overlapping
  rectangles (or a circle inside a rect) enclosed real cells and produced NO fill —
  while the same picture with one stray line across it produced all of them. PASS 3b
  is a second sweep over the planes PASS 3 could not reach.
- **THE GRID ENDED IN MID-AIR AT SHALLOW ANGLES** (item 10). Nothing was shrinking:
  grid v3's lattice window is `R = spacing * 120`, which at 192 units up on a 10-unit
  grid stops **1740 units in front of the eye**. Looking down you never see the edge;
  looking at the horizon the visible ground runs to tens of thousands of units and
  the window's own edge is what the user was seeing. v3 made DENSITY pitch-free (the
  directive) and left REACH pitch-free too (never the directive). A **FAR RING** —
  8x the spacing, 8x the reach, the SAME line count — is emitted when the frustum
  says there is ground past R. The near field is completely unchanged.
- **HIDE IS UNDOABLE** (item 7). It could not be a legacy record and that is a fact
  about the data: the hidden state is `brushFlags & 4` + `xx5`, both on the
  **INSTANCE**, while the legacy undo snapshots **DEFS** — and `Undo_Undo`'s
  re-create phase goes through `Brush_AddToList`, whose `memset( b, 0, 0x38u )`
  **zeroes** them. A legacy record would destroy the state it was meant to save. So
  hide has its own journal domain (`KUNDO_VISIBILITY`) with its own snapshot store,
  keyed on the stable `brush_t *` def. ONE record per `H` / `Shift+H` / `Alt+H` /
  `Shift+Ctrl+H` / `Ctrl+H` / eye click.
- **THE CIRCLE'S `sides` FIELD WAS UNREACHABLE** (item 5a). It was attached, Tab did
  reach it and the typed value did flow — but the base `ToolSides()` returns 0 for
  AUTO, `NumericFieldValue` refuses a 0, and `kiwi_numeric.cpp` skips the row for a
  field with no value. **A field with no value does not exist in this HUD.** Tab
  moved a caret onto an invisible box. The circle, the 2-point circle and the arc
  now report the count the shape WILL HAVE, never 0 — which is exactly why the
  cylinder always worked (`m_sides` is never 0 there).
- **BOX-SELECTING FACES TOOK THE BACK FACES TOO** (item 9). The mode-3 marquee arm
  has existed since round U and had no facing test, so one drag over a wall took that
  wall's face, the face on the far side of the same brush, and both faces of
  everything behind it. On "lots of small brushes" that is indistinguishable from
  "it does not work". A facing gate is added; the crossing/containment semantics are
  otherwise untouched.

### Added

- **SHIFT+CLICK ACCUMULATES SOURCES, AND ONE `E` EXTRUDES ALL OF THEM** (item 1).
  Construction REGIONS became a selection SET (shift+click toggles, the fill tints
  every member); brush FACES were already a set and `E` now reads all of it instead
  of only the active one. Each source grows along ITS OWN normal by the same
  distance, in ONE undo bracket.
- **THE OUTLINER HIGHLIGHTS IN 3D ON HOVER** (item 3). A brush row tints that brush,
  a group/entity row tints every member, a curve row re-traces its polyline — all in
  the §18 hover cyan. This is Plasticity's own outliner behaviour
  (`Outliner.tsx` `onPointerEnter` → `selection.hovered.add`).
- **THE MARQUEE SHOWS WHAT IT WILL TAKE, LIVE** (item 6), by re-running the same
  `Collect()` the release runs, throttled to rect changes.
- **`Shift+C` IS THE CIRCLE** (item 4). A straight swap with the corner box, which
  takes the vacated `Shift+W`.
- **THE CIRCLE'S SEGMENTS LAND ON THE CARDINALS** (item 5b). The count rounds UP to
  a multiple of four, so 0/90/180/270 are always vertices and a scaled circle is a
  proper oval with clean quadrant symmetry.
- **LIGHT GRID SNAPPING BY DEFAULT, CTRL FOR THE FULL QUERY** (item 11). A 5-pixel
  capture band onto the global lattice, grid-only, on the three gestures round Z
  made raw. Outside the band the drag is untouched — it is a magnet, not a ratchet.
- **EXPERIMENTAL PATCH MODE ON THE CYLINDER (`P`)** (item 8). Emits a q3 bezier
  patch instead of a faceted brush — which is what stock Radiant's Curve > Cylinder
  does and what CoD4 curves actually are. Remembered across sessions.

### What you will actually see

**A region cell too big to fill (once per store change):**

    Construction: an enclosed area has 137 corners and the region cap is 128, so it
    is neither filled nor extrudable.  Redraw its round parts with a lower 'sides'
    count (Tab while drawing).

**A multi-source extrude, on commit:**

    Extruded 3 region(s): 5 brush(es) from 22 profile vertices.
    Extrude Face: 4 new brush(es) from 4 face(s) (4-gon drive).

**Carving with several faces selected:**

    Extrude Face: carving IN applies to the ACTIVE face only — the other 3 selected
    face(s) are left alone.  Drag OUT to grow all of them at once.

**A patch cylinder, on creation:**

    Cylinder (PATCH, experimental): a q3 curve, 4 spans.  It has NO COLLISION — add
    a caulk brush inside it — and no end caps (stock CoD4 practice).  Press P to go
    back to a brush.

**Hovering a very large group in the outliner:**

    Outliner: that row owns more than 96 brushes — the 3D highlight shows the first 96.

**Undo / redo, with the third domain in the journal:**

    Undo: hide selected (4 left).
    Undo: dropped a stale hide step.

### Keymap changes (modern profile only)

| chord | was | is now |
|---|---|---|
| `Shift+C` | Box (corner) 34051 | **Construct: Circle 34037** |
| `Shift+W` | Construct: Circle 34037 | **Box (corner) 34051** |

A straight swap — nothing else moves and no chord is dropped. Both keys were
re-audited across the whole default command table and against `res/radiant.rc`'s
accelerator table (which `TranslateAccelerator` consults BEFORE the hotkey table);
neither appears there. The complete final creation-chord table is in
RADIANT_UX_DESIGN §61.9.

New in-command key: **`P`** toggles the experimental patch mode while the Cylinder
tool is running. It is command-local and takes nothing from the global table.

### Known limits

**THE REGION FILL (item 2)**

- **THE 128-CORNER CAP IS REAL, and a cell past it is neither filled nor
  extrudable.** It now says so, once per store change, and names the lever
  (`sides`). A 64-segment circle crossed by a second 64-segment circle can produce a
  cell over it.
- **THE WELD BOUND IS PER-GROUP, NOT PER-OBJECT.** `ArrangeWeld` takes the finest
  segment in the WHOLE coplanar group, so one very finely tessellated object tightens
  the weld for the straight lines around it too. That is the safe direction (it can
  only refuse to fuse, never fuse wrongly), but on a plane holding both a 64-segment
  circle of radius 8 and a set of lines drawn at grid 64, the lines weld at 0.5
  instead of 16.
- **PASS 3b RUNS THE ARRANGEMENT PER UNARRANGED PLANE**, and the plane-equality test
  is approximate (normal dot 0.999, offset within the plane band). A false negative
  costs one redundant arrangement whose cells `DuplicateRegion` then drops; it cannot
  produce a wrong region.
- **THE SEED-PLANE AMBIGUITY FROM SHAKEOUT H IS UNCHANGED.** Two lines that both
  touch the seed end and lie on different planes still resolve by store order.

**THE GRID (item 10)**

- **THE FAR RING REACHES 8x THE NEAR REACH AND NO FURTHER.** At 192 units up on a
  10-unit grid that is ~9600 units, which is past the far side of any CoD4 map — but
  a camera parked at 20 units with a 1-unit grid reaches only ~960 units, and past
  that the ground is still bare. The cap is a budget decision (a second far ring
  would be another 482 segments).
- **THE FAR RING POPS ON AND OFF at the latch thresholds** (1.30 R on, 1.05 R off)
  when the camera pitches across the boundary. It is a dim tier appearing at a
  distance, and the hysteresis stops it flickering, but it is not a fade.
- **AT AN EDGE-ON VIEW THE WHOLE LATTICE STILL GOES** (round S's `EdgeFade`), far
  ring included. That is deliberate and unchanged.
- **THE PROBE READS THE FRUSTUM.** It is confined to one boolean and the near
  lattice cannot be perturbed by it (D-AG5), but it does mean the far ring's presence
  is a function of camera angle — which is the point.

**HIDE UNDO (item 7)**

- **PRESSING `H` WITH BOTH CONSTRUCTION LINES AND BRUSHES SELECTED MINTS TWO
  TICKETS** and therefore needs two Ctrl+Z presses. The bare-`H` funnel deliberately
  runs both domains for one press (`KiwiConSel_OwnsHide` then falls through to 32923),
  and the journal has no compound/cross-domain ticket — `kiwi_undo.h` says that is
  deliberately absent. Not worth inventing one for this case.
- **A BRUSH CREATED SINCE THE SNAPSHOT IS NOT IN IT** and is left exactly as it is;
  a brush destroyed since is skipped. Both are the journal's existing "dropped a
  stale step" case.
- **AN UNRELATED LEGACY UNDO STILL RESETS THE HIDDEN BIT** on the brushes it touches,
  because `Brush_AddToList` memsets the fresh instance. That is a property of the
  ported undo, not of this domain, and fixing it would mean teaching `Undo_Undo` to
  carry instance state.
- **THE SELECTION IS NOT PART OF THE RECORD.** `Cmd_OnHideSelected` also calls
  `Select_Deselect(1)`; undoing the hide brings the brushes back visible but not
  selected.
- **`Select_Hide` DEEPENS `xx5` ON EVERY ALREADY-HIDDEN BRUSH IN THE MAP** (its first
  two passes), and the snapshot covers every live instance, so that IS captured — but
  a brush that was hidden before the map was loaded and has no live instance is not.

**MULTI-SOURCE EXTRUSION (item 1)**

- **MIXED REGIONS + FACES IS NOT SUPPORTED.** They are two different commands and
  `E` dispatches to one of them; select regions or faces, not both. Nothing crashes —
  the face arm simply never sees the regions and vice versa.
- **THE FACE CARVE (negative `E`) IS SINGLE-SOURCE**, and says so. Growing OUT is
  multi-source; carving IN applies to the active face only.
- **ALL-OR-NOTHING ON REJECTION.** One region whose decomposition fails refuses the
  whole gesture. The one exception is an extra FACE that cannot be profiled at all,
  which is dropped and counted.
- **THE REGION SET IS LATCHED AT `Begin`.** Nothing edits the construction store
  during a drag, so this cannot go stale in practice — but a region index is what is
  held, not a centroid, for the duration of the gesture.
- **THE HUD SHOWS `x3`, NOT WHICH THREE.** The preview is the only per-source
  feedback, and a source off screen is a source you cannot see.
- **NOT RUNTIME-VERIFIED**, like everything else this round.

**THE LIVE MARQUEE PREVIEW (item 6)**

- **A CROSSING RECT ENTIRELY INSIDE ONE BIG FACE PREVIEWS NOTHING** and then selects
  that face on release. The centre-ray rescue that covers this case fires one full
  `Pick()` and is deliberately skipped in the preview.
- **NO EDGE OR VERTEX PREVIEW.** At those granularities the marquee names dozens of
  handles the ported vertex pass already draws.
- **THE CONSTRUCTION STORE IS NOT PREVIEWED** — `KiwiConSel_ApplyRect` runs at the
  release and has no dry-run form.
- **900 SEGMENTS.** Past that the preview is partial; the selection is not.
- **THE COLLECT IS STILL A FULL LIST WALK** on every rect change past the 2-pixel
  threshold, bbox-rejected per brush. On a very large map a fast drag will do real
  work per frame.

**BOX-SELECTING FACES (item 9)**

- **IT IS A FACING TEST, NOT AN OCCLUSION TEST.** A front-facing face behind a wall
  is still collected — matching what the object marquee does with the brush behind
  the wall.
- **PATCHES HAVE NO FACES** and are unaffected (mode 3 has never resolved a patch).

**LIGHT GRID SNAPPING (item 11)**

- **IT IS ON THE THREE ONE-AXIS GESTURES ONLY** — the region extrude, the face
  extrude / un-extrude and the face push/pull. The `G` move and everything else were
  never made raw by round Z and still snap fully.
- **THE BAND IS 5 PIXELS, CAPPED AT 0.22 OF A CELL.** At a very coarse grid the cap
  binds and the magnet is proportionally weaker; at a very fine grid the pixel band
  binds and it is proportionally stronger. Both are the intended direction.
- **ON A SLANTED AXIS IT SNAPS THE DISTANCE, NOT A COORDINATE** (round P's rule),
  so an off-grid start keeps its offset there.

**THE PATCH MODE (item 8) — EXPERIMENTAL**

- **NO COLLISION AND NO CAPS.** A patch is a render surface. Put a caulk brush
  inside it. This is stock CoD4 practice and it is stated on every creation.
- **CYLINDER ONLY.** Not the cone (no faithful stock spelling — `Patch_BrushToMesh`'s
  cone arm collapses a ring to a point), not the sphere (`PATCH_HEMISPHERE` has no
  creation path at all), not the box.
- **THE CIRCLE-WHEN-EXTRUDED PATCH ARM IS NOT IN THIS ROUND.** The region extruder
  is a convex-decomposition brush builder end to end (`BuildPieceDef` →
  `KiwiValid_CheckBrush` → `LandDef`); a patch arm there is a second geometry
  pipeline, not a flag. The cylinder covers the round case the report was about.
- **FOUR SPANS, FIXED.** A patch's smoothness is its tessellation, not its
  control-point count, so `sides` and `[` `]` do nothing in patch mode. The format
  would allow up to 7 spans; a user-facing span count is future work.
- **THE WORKING PLANE MUST STILL BE AXIS-ALIGNED**, because the tool refuses a
  non-axis-aligned plane at `Begin` for the brush path's sake. The patch itself would
  not need it; the gate is shared and was not loosened.
- **NO ARCH TOOL.** The workflow is: patch cylinder through the wall, delete the half
  you do not want, caulk brush inside. Documented rather than automated, as the brief
  allowed.
- **THE MATERIAL IS TAKEN FROM A THROWAWAY BOX DEF** built by the ordinary path and
  freed immediately — the same source `Patch_BrushToMesh` reads (`faces[0].mtldef`).
  If that ever stops being how the current material reaches a new brush, the patch
  will silently get `$default`.

### General

- **NOT RUNTIME-VERIFIED.** Nothing in round AG was built or run — the user builds.
  The items most worth exercising first, because they carry the most new geometry
  code or the most invasive refactor: **the multi-source extrude** (two regions on
  two walls; then two faces at 90°; then a rejection, to check nothing half-lands),
  **the patch cylinder** (draw one, confirm it renders — a patch that is created and
  never drawn is the classic `KiwiMtl_RealizePatch` failure), and **the region fill**
  at grid 16 and 32 with a large arc in the loop, which is the exact case item 2 was
  about.
- **THE MODAL ID BLOCK IS UNCHANGED AND STILL HAS FOUR FREE IDS** (34066..34069).
  Round AG added NO command ids: the patch mode is an in-command toggle (D-AG21) and
  every other item extends an existing verb.
- **ONE PORTED FILE GAINED FOUR LINES**: `mainfrm.cpp`'s four hide handlers each get
  one `KiwiVis_UndoPush` at the head, fenced `// KIWI-UX`. The ported cores in
  `select.cpp` are untouched.

## Round AI

Six items. The design rationale for all of them is in **RADIANT_UX_DESIGN §62**,
decision log D-AI1..D-AI21.

### Fixed (bugs)

- **THUMBNAILS WENT SOLID RED AFTER THE SELECTED ONE** (item 1). The
  selected-texture frame is drawn with `R_AddCmd_Line2D`, which in this port is
  **state-destructive by design** and says so at its own definition
  (r_rendercmds.cpp:2028-2035): it routes through `Ed_EmitLineBatch`, which pushes
  the line colour as `MATERIAL_COLOR` (:2004) and never restores it. The frame colour
  is `colors[10]` = **{1,0,0,1}** (win_qe3.cpp:423), and that `w == 1` is exactly the
  flat-override weight the thumbnail shader lerps by — so every `Draw2DImage` emitted
  afterwards in the same frame renders **flat red instead of its texture**.
  **"Sometimes" is POSITIONAL, not random:** the per-paint seed
  (texwnd.cpp:1066/:1110) re-neutralises at the top of every frame, so the damage
  reaches exactly the thumbnails after the selected one — scroll the selection off
  the top (its border is skipped by the cull `continue`) or onto the last visible
  cell and the grid looks perfect. `R_DrawOutlineRect` now restores `{1,1,1,0}`
  itself, which also covers the layered sub-view's per-row frame, whose non-active
  colour is `colors[8]` = BLACK and would have tinted the next row's thumbnails
  solid black by the identical mechanism. `CLayermatWnd_OnPaint` gains the
  `MATERIAL_COLOR` seed every other paint already had.
- **THE BOX HEIGHT WAS ALREADY -140 yd AT A TOP-LOCKED CAMERA** (item 2). Not a
  stale value — a **real solve of a degenerate system**. Every one-axis gesture maps
  the cursor through `RayAxis`, whose denominator is `sin^2(theta)` between the axis
  and the ray, so the answer is amplified by `1/sin^2` — about **70 world units per
  pixel at 5 degrees** — and the local guards refused only **theta < 0.6 degrees**.
  Two gates now: a **VIEW gate** on the camera's `vpn`
  (`KiwiCam_AxisPortrayable`, 0.97 = 14 degrees) that HOLDS the scalar and REBASES
  when the view can portray the axis again, and a **SAMPLE gate**
  (`KCAM_RAYAXIS_MIN_DEN`, the same angle as `sin^2`) in all six `RayAxis` copies for
  the pixels that are end-on even at a legal `vpn`. Applied to the primitive height
  stage, region extrude, face extrude and the Move command's push/pull + axis drags.
  Typing a number works at any angle, as it always did.
- **BOX-SELECTING FACES STILL WENT THROUGH WALLS AND TOOK GRAZING FACES** (item 5).
  Round AG's facing gate explicitly did not occlude. Two gates added:
  **EDGE-ON** — reject `|n . d| < 0.12` (the face's foreshortening; a perfect south
  view drops the east/west walls), free, runs in the preview too; and **OCCLUSION** —
  one ray from the eye to the face's centroid (plus up to three fallback witnesses
  only if that one is blocked), asking `SEL_MASK_OBJECT` and requiring the first hit
  to be this brush. It **fails open** on a no-hit so a contents-filtered but visible
  brush stays selectable, and it is spent LAST so it is never paid for a face the
  rect was not going to take.

### Changed

- **HOVER NOW READS AS STRONG AS A SELECTION** (item 4). A selected brush is **two
  channels** — a 0.25 `MATERIAL_COLOR` tint over the textured surface AND a white
  wireframe (camwnd.cpp:2755-2790). The outliner hover had only the tint, which is
  why no alpha would have fixed it. Face hover 0.18 to 0.26, outliner hover 0.26 to
  0.34 **plus a cyan wireframe at width 2**, marquee preview outline to width 2. The
  whole fill path was audited for a hidden multiplier first: there is none.
- **AN IN-COMMAND OPTIONS PANEL, AND IT IS GENERIC** (item 3). Three new virtuals on
  `KiwiEditorCommand` (`CommandOptions` / `OptionValue` / `OptionChanged`) with four
  row kinds — segmented enum, toggle, `- N +` stepper, and a slider bound to an
  existing NUMERIC FIELD. Loft declares mode / density / tension; the panel calls the
  same handlers the keyboard calls (the mode button is literally `KeyDown('D')`) so
  there is no second state. Enter / RMB confirm and Esc cancel are untouched, and the
  footer's own buttons call `KiwiCmd_Confirm` / `KiwiCmd_Cancel`, i.e. those exact
  paths.
- **PATCH VERTEX MODE ON V WORKS** (item 6). See the binding rule below. The legacy
  drag was dead for three independent reasons (the camera never reaches `Drag_Begin`;
  `Sel_SyncToLegacy` resets `d_select_mode` on every click; the legacy handle lists
  are empty since shakeout D), so it is **routed rather than resurrected** — the
  modern layer already picks, marqueees, moves, snaps, gizmos and journals patch
  control points. Entering the mode sets the `SEL_MASK_VERTEX` pick mask and draws
  the control lattice from its own latched patch list.

### Keymap — the V rule

**No chord moves. Both profiles behave identically.**

| when | V means |
|---|---|
| a modal command is running (Move / Rotate) | the movable pivot (§29) — command-local, unchanged |
| nothing running, the selection HAS a patch | **patch vertex mode** (toggle) |
| nothing running, no patch | the classic 33005 Drag Vertices, byte for byte |

The third row is the important one: `KiwiPatchVerts_ToggleForSelection()` returns
false when there is no patch and the ported brush-vertex toggle runs exactly as it
did. V was never globally bound to the pivot (kiwi_keymap.cpp:705-715) and still is
not.

### What you will actually see

**Starting a box height at a top-locked camera:**

    box  height 0 in  ·  ORBIT to set height — this view looks straight along Z
    ·  or type = height

**Clicking to commit that height anyway:**

    Box: this view looks straight along Z, so the cursor cannot express a height —
    orbit away from the top/bottom lock, or type one.

**Grabbing a move-gizmo axis that points at the camera:**

    Move: this view looks straight along the drag axis, so the cursor cannot express
    movement along it — orbit a little and grab again, or type a distance.

**Entering patch vertex mode:**

    Patch vertex mode: 1 patch(es).  Click a control point and drag the move gizmo
    (or press G); box-select takes several.  V leaves.

### Known limits

**THE ONE-AXIS VIEW GATE (item 2)**

- **A GIZMO AXIS GRAB THAT STARTS DEGENERATE STAYS DEAD FOR THAT GRAB.**
  `LatchMapStart` in kiwi_transform.cpp sets `m_haveMapStart = false` and does not
  retry, because a mid-grab re-latch would have to rebase `m_lockBase` too and that
  is a larger change than this round wanted. It says so on the console once. Release,
  orbit slightly, grab again.
- **THE SAMPLE GATE LEAVES A SMALL DEAD DISC ON SCREEN** wherever the drag axis is
  end-on, even at a legal camera angle (a 65-degree FOV puts edge-of-image rays 32
  degrees off `vpn`). The scalar holds while the cursor is inside it. That is
  deliberate and is strictly better than the jump it replaces.
- **THE VIEW GATE READS `vpn`, NOT THE CURSOR RAY**, so it is one boolean per frame.
  A gesture recovers on the first cursor MOVE after the orbit, not on the orbit
  itself — the command's `MouseMove` is what re-runs the test.
- **14 DEGREES IS A JUDGEMENT.** It is derived from the amplification (17x, ~9 units
  per pixel at d = 500), but a user who wants to push a face from a nearly head-on
  view will be refused inside that cone.

**THE FACE MARQUEE (item 5)**

- **THE OCCLUSION WITNESS IS THE CENTROID PLUS THREE FALLBACKS.** A face that is
  visible only in a region none of those four points lands in is still refused. Four
  rays is the ceiling per rejected face.
- **THE LIVE PREVIEW MAY OVER-REPORT.** Its occlusion budget is 96 rays per rect
  change against the release's 8192; past it the gate is skipped and the face is
  admitted. So a very large marquee can highlight faces the release then drops. The
  direction is deliberate — the preview is a superset, never a subset.
- **THE GATE IS ON THE MARQUEE, NOT ON CLICK SELECTION.** A click already goes
  through `Pick`, which is a real ray and therefore already occlusion-correct; no
  edge-on gate was added there, because clicking a 1-pixel sliver is a deliberate act
  and the pick has a screen-distance tolerance rather than an area test.
- **BOTH GATES ARE FACE-ONLY.** The OBJECT marquee still takes brushes behind walls,
  which is its documented semantics and was not part of this directive.

**THE OPTIONS PANEL (item 3)**

- **ONLY LOFT DECLARES OPTIONS.** The cylinder's `P` patch toggle and the drawing
  tools' `Z` plane cycle are the obvious next tenants and were NOT converted; they
  still exist only as keys and HUD text.
- **THE PANEL HAS NO NUMERIC TEXT ENTRY.** Deliberate: a focusable text field would
  make `ImGuiShell_WantsKeyboard` true and steal Enter / Esc / Tab from the live
  gesture. Type numbers the way you always did — Tab plus digits, into the HUD.
- **IT ANCHORS TO THE CAMERA IMAGE**, so it does not appear until the camera
  viewport has been drawn at least once, and it does not follow a popped-out
  viewport into a separate platform window.
- **A `KOPT_NUMFIELD` SLIDER CLEARS THAT FIELD'S TYPED ENTRY** when dragged. That is
  intended (dragging says the typed entry is finished with) but it means a
  half-typed number is discarded by touching the slider.

**PATCH VERTEX MODE (item 6)**

- **IT IS A 3D-VIEWPORT MODE.** The XY pane still routes to the legacy
  `Drag_Begin` chain, which under this mode's mask has no handle list, so control
  points are not draggable in 2D. The legacy 2D path was not un-killed.
- **NO CONTROL-POINT INSERT / DELETE / ROW OPS.** `Patch_InsertColumn` and friends
  are unreached; this is point MOVEMENT only, which is what the directive asked for.
- **THE MODE IS SCOPED AT ENTRY.** It latches the patches named by the selection at
  the moment V is pressed (cap 16). Selecting a different patch while inside does not
  extend the mode — leave and re-enter.
- **`d_select_mode` IS NOT SET**, so the ported `Patch_DrawControlPoints` and the
  legacy toolbar state do not know the mode is on. This is deliberate (that flag is
  reset by every modern click anyway) but it means any future ported code gated on
  `sel_curvepoint` will not fire.
- **THE MODE DOES NOT SURVIVE A MAP LOAD** — the draw hook drops it quietly when
  none of its patches are live any more, restoring the previous pick mask.
- **NO UNDO FOR ENTERING / LEAVING**, by design: it changes no geometry. The drags
  inside it are journalled one record each by the Move command's existing bracket.

## Round AJ

Six items. The design rationale for all of them is in **RADIANT_UX_DESIGN §63**,
decision log D-AJ1..D-AJ24.

### Fixed (bugs)

- **THE PATCH VERTEX LATTICE OUTLIVED THE PATCH** (item 1a). Round AI latched the
  patch list at entry and the only thing that could ever drop it was the draw hook
  noticing no latched patch was live. The latch was load-bearing — entering the mode
  clears the selection and the first click replaces the object item with a vertex
  item, so *"is the patch still selected"* is false within one click and cannot be
  the test — so the fix is **named exit triggers**, checked once per frame from
  `KiwiVP_CameraTick` (NOT from the draw: D-AI21, the draw is inside a sentinel-list
  walk and the exit relinks brushes). They are: the selection names something that
  is not a latched patch; the mode mask moved off `SEL_MASK_VERTEX`; **Esc**; no
  latched patch is live. An EMPTY selection is deliberately not one of them — that
  is "click empty space drops the point selection and stays in the mode". The
  selection walk is gated on `Sel_Generation()`, so an idle frame costs one compare.
- **BOX-SELECTING PATCH CONTROL POINTS SHOWED NO PREVIEW** (item 1c). This is the
  whole of *"impossible to edit more than 1"*. Shift-add (`Sel_Add` in
  `ClickSelect`), the rect's `SEL_VERTEX` arm and `kiwi_transform.cpp`'s multi-point
  move (one `m_total` applied from each point's own baseline, **one**
  `Patch_Rebuild` per patch, **one** undo record) were all already whole and were
  read end to end before anything was touched. `KiwiBox_DrawPreview` simply skipped
  `SEL_VERTEX`, on the grounds that "the ported vertex pass is already drawing"
  those handles — **untrue in this mode**, because those builders walk
  `selected_brushes` and shakeout D removed the promotion that fills it. So the rect
  swept over the points and highlighted nothing. The preview now emits a 5 px marker
  per `SEL_VERTEX` item; edges stay absent for the reason that was always true of
  them.
- **THE BEVEL CUT BEFORE IT WAS ASKED TO** (item 6). `Begin()` returned with the
  command HOT, so the first `MouseMove` mapped the cursor and appended a chamfer face
  at whatever depth that pixel expressed — the lollipop was drawn on geometry that had
  already deformed. Two halves now: the command **parks** (`KiwiCmd_Pause()` from
  inside `Begin()`, which is legal because `g_activeCommand` is set before `Begin` is
  called) **and** carries its own `m_grabbed` gate, because shakeout E's resume rule
  is "any LMB press inside the image" and one stray click would otherwise hand the
  gesture back to the cursor. The geometry-snap arm is behind the same gate; numeric
  entry is not.
- **THE FILLET LEFT A LENS-SHAPED HOLE AT EACH END** (item 2a). Derived, not
  eyeballed: `n.(T - mid) = -d` for both tangent points, so the arc meets the chamfer
  plane exactly along both long rails and bulges outward between them. The bottom of
  the gap is the chamfer FACE (already solid, already the same material), so of stock
  `Patch_Thicken`'s five pieces only seams C and D are missing. `LandCaps` builds
  that pair the way pmesh.cpp:7529-7581 does (row 0 = the arc, row 2 = its projection
  onto the chamfer plane, row 1 = the midpoint).
- **THE FILLET PATCH'S TEXTURE DENSITY DID NOT MATCH THE PARENT FACE** (item 2b).
  `Patch_Naturalize2` divides the cumulative world width-distance by
  `texWidth * a3`, and a brush face's texdef `size[0]` IS that same product
  (brush.cpp:3327 / pmesh.cpp:855 both seed it as `texWidth * 0.25`).
  `Patch_KiwiFinishNew` hardcoded the editor default sample size, so a patch grown
  out of a rescaled wall came out at a different repeat length.
  `Patch_KiwiFinishNewLike` takes `a3 = size[0] / texWidth`; the old entry point is
  now a forwarder with byte-identical behaviour and no ported caller changed.

### Changed

- **THE WHEEL DOLLY EASES NEAR GEOMETRY** (item 3). "Squirrely" is only half a speed
  problem: this editor dollies along the CURSOR RAY, so each notch also slides the
  view sideways by `move * sin(ray angle)` — invisible far out, a visible slice of
  the screen up close. (Plasticity has no zoom-to-cursor at all: `onMouseWheel` never
  reads `clientX/Y`.) One parameter, `t = clamp(reference/48, 0, 1)`, eases the STEP
  0.90 -> 0.96 and eases the AIM from the cursor ray toward the view axis (a third of
  the cursor aim always survives). **Beyond 48 in nothing changes at all.** Orbit and
  pan were checked against Plasticity's formulas and deliberately left alone.
- **A GRID-SNAP MASTER SWITCH, ON THE GRID PILL** (item 5a). A 2x2-lattice checkbox
  sharing the pill's row, plus a "Snap to grid" checkbox in the settings block; both
  drive `KiwiGrid_SetSnapEnabled`. Enforced at the two FUNCTIONS the lattice can
  reach a number through — `KiwiGrid_Snap` and `KiwiSnap_LightGridAxis` — not at the
  six call sites. Persisted under `KiwiUX/GridSnap`, default ON. It is **not** Ctrl:
  Ctrl is per-gesture and suppresses geometry snapping too.
- **NO GROUND LATTICE IN PERSPECTIVE** (item 5b). `KiwiGrid_Draw` early-outs on
  `!KiwiCam_Ortho()`. **This is a workaround the user chose while the perspective
  grid bugs remain open, not the end state** — see the open item below. Axes stay,
  snapping is untouched.
- **THE GIZMOS ARE RESTYLED TO PLASTICITY'S LOOK** (item 4). Muted 600-ramp axis
  colours (#CF1124 / #199473 / #2563EB) with its 400 ramp as the hover and yellow
  kept for HELD; small closed arrowheads at its 0.2 / 0.1 proportions; the plane
  handles are small squares centred at 0.5 of the shaft instead of L corners; the
  free-move handle is a **white screen-space ring** at the origin (the same
  `KGZ_CENTER` handle, restyled — nothing was wired); the rotate rings have their
  **back halves culled** and gain a **white outer view ring**; and the hovered
  element is re-emitted at line width 3. All grab semantics and the resolution order
  are unchanged.

### What you will actually see

**Entering patch vertex mode:**

    Patch vertex mode: 1 patch(es).  Click a control point and drag the move gizmo
    (or press G).  SHIFT-CLICK adds points and a DRAGGED BOX takes every point
    inside it — the gizmo then moves them all together, as one undo record.
    V or Esc leaves.

**…and on the idle chip strip, for as long as it is live:**

    [Shift+LMB] Add point   [Drag box] Take several   [V / Esc] Leave patch vertex mode

**Pressing B on a brush edge:**

    Bevel Edge: 2 edge(s), parked at 0.  GRAB THE BALL on the stem and drag to cut
    the chamfer, or just type a depth.  D curves it into a patch fillet, Tab reaches
    the angle bias, RMB / Enter confirms, Esc cancels.

    bevel  2 edge(s)  parked at 0  ·  grab the ball and drag (or type a depth)  (D: curve it)

**Confirming a fillet:**

    Filleted 2 edge(s) — 6 patch(es) at radius 12 (arc + sealed ends).

**Turning grid snapping off:**

    Grid snapping: OFF (geometry snaps still work).

### Known limits

**PATCH VERTEX MODE (item 1)**

- **IT IS STILL A 3D-VIEWPORT MODE.** The XY pane routes to the legacy `Drag_Begin`
  chain, which under this mask has no handle list, so control points are still not
  draggable in 2D.
- **STILL NO CONTROL-POINT INSERT / DELETE / ROW OPS.** `Patch_InsertColumn` and
  friends remain unreached; this is point MOVEMENT only.
- **THE MODE NOW ADOPTS**, so round AI's "scoped at entry" limit is gone — clicking a
  control point on another patch widens the mode instead of doing nothing visible.
  The cap is still 16 patches; past it the adoption silently does not happen and that
  patch's lattice will not draw.
- **`d_select_mode` IS STILL NOT SET**, so any future ported code gated on
  `sel_curvepoint` will not fire. Unchanged from round AI and deliberate.
- **THE EXIT TRIGGERS ARE CHECKED PER FRAME, NOT PER EVENT.** A selection change and
  the lattice disappearing are one frame apart. Not perceptible, but it means a
  single-frame screenshot taken mid-transition can show both.

**THE BEVEL / FILLET GRAB (item 6)**

- **THE OPTIONS PANEL AND THE D TOGGLE STILL WORK WHILE PARKED**, by design, but D
  before the first grab converts a zero, so it changes nothing visible.
- **A TYPED NUMBER STILL BYPASSES THE BALL ENTIRELY.** That is deliberate (numeric
  entry never went through the cursor), but it means the tool can be driven to a
  depth without the lollipop ever having been touched.
- **THE OTHER LOLLIPOP TOOLS WERE NOT CONVERTED.** Face push/pull, region extrude and
  face extrude auto-enter PAUSED already, but once resumed they have no per-command
  grab gate of their own beyond Move's. Only the bevel/fillet gained one this round.

**THE FILLET SEAL (item 2)**

- **THREE PATCHES PER EDGE NOW, NOT ONE.** A 4-edge fillet lands 12 patches. That is
  the cost of a closed surface and there is no option to skip it — the directive says
  "automatically" — but it is worth knowing before filleting a long selection.
- **THE CAPS ARE SEPARATE PATCH ENTITIES**, so deleting the arc leaves them behind.
  DEL on a selected chamfer face still deletes every paired patch on the legacy list
  (kiwi_bevel.cpp's Remove Face), which covers the normal case.
- **THE SEAL IS GEOMETRIC, NOT A BOOLEAN.** Nothing is welded; the caps meet the arc
  and the chamfer face at coincident control points. At extreme radii relative to the
  edge length a hairline can still show at the join.
- **THE TEXDEF SCALE IS READ FROM SUB-LAYER 0** (`mtldef[0].mat_texDef.size`). A face
  whose CURRENT layer is a different sub-layer block will donate the base layer's
  scale instead of that one's. Falls back to the old editor-default behaviour when
  the size is zero or negative.

**THE DOLLY EASE (item 3)**

- **IT IS TUNED, NOT DERIVED.** 0.96, 48 in and the 0.35 aim floor are judgements
  calibrated against Plasticity's flat 0.95 and its no-zoom-to-cursor behaviour; they
  are not read off a formula the way round AI's 14 degrees was.
- **THE REFERENCE IS THE SURFACE UNDER THE CURSOR**, so the ease engages on what you
  are pointing at, not on the orbit radius. Point at a distant wall from a close-in
  camera and the ease does not engage.
- **`KCAM_MIN_DIST` IS UNCHANGED AT 1 IN**, and so is the pan floor that reads it.

**THE GRID CHANGES (item 5)**

- **THE PERSPECTIVE GRID BUGS ARE STILL OPEN.** Suppressing the draw removed the
  surface they show up on; the horizon-plane aliasing, the band seams and the
  near-plane crawl are all still there and will come back with the lattice. THIS IS
  THE ITEM TO REOPEN when the grid is next worked on.
- **THERE IS NO "GRID IN PERSPECTIVE" OVERRIDE.** The View-menu / settings "Ground
  grid" checkbox still exists and still means "draw the lattice", but in perspective
  it now has no effect. A user who wants it back has to switch to ortho.
- **THE SNAP CHECKBOX HIDES ON A NARROW VIEWPORT.** It is clamped to the image's left
  margin like the type-in popup; below that width the pill loses it. The settings
  block's copy is always reachable.
- **TURNING SNAPPING OFF DOES NOT RETROACTIVELY UNSNAP.** Values already quantised
  stay where they are; only future gestures are free.

**THE GIZMO RESTYLE (item 4)**

- **THE WHITE OUTER ROTATE RING IS NOT GRABBABLE.** In Plasticity it is the VIEW-AXIS
  rotation; the ported core is `Select_RotateAxis( axis, deg, ... )` (select.cpp:2337)
  which takes an axis INDEX, and R's whole state is one `m_axis` int — so an
  arbitrary-axis rotate is a new core rather than a repaint and was out of scope for
  a visual pass. The ring is drawn as the sphere silhouette it also is.
- **THE PLANE HANDLE'S HIT ZONE MOVED** with its square (22 px corner -> 35 px centre).
  Larger in area and identical in tolerance and priority, but old muscle memory aimed
  at the L corner will now land on the axis shaft.
- **THE ROTATE RINGS' BACK HALVES ARE HIDDEN BUT STILL HIT-TESTABLE**, deliberately,
  so a ring can be grabbed at a pixel where nothing is drawn.
- **THE GIZMO PALETTE NOW DIFFERS FROM THE WORLD AXES AND THE CONSTRAINT ACCENT.**
  Same hues, different saturation (D-AJ20). If that reads as an inconsistency in use,
  the fix is to move the other two, not to move this one back.
- **LINE WIDTH IS PER BATCH.** Only the ONE hovered element gets width 3; there is no
  general per-segment width in `kiwi_lines`.


## Round AK

Four items. The design rationale for all of them is in **RADIANT_UX_DESIGN §64**,
decision log D-AK1..D-AK18.

### Fixed (bugs)

- **THE REGION FILL WAS FEEDING A PLANE-LOCKED NORMAL, AND ROUND AA'S FLIP COULD
  NEVER REACH IT** (item 1). *"…still lack their light blue background on the face.
  It worked a few days ago sometimes, but now never shows - not even from the
  backside."* The regions still EXTRUDE, so derivation (round AG) is fine and this
  is a draw defect. Ruled out first, each with an argument rather than a shrug:
  `KiwiRegion_Triangulate` cannot fail on a region that extrudes (a convex one
  bypasses it in `kiwi_extrude.cpp:934-939` but cannot fail it — `AcceptLoop:336`
  already ran the identical self-intersect test and a convex CCW loop always has an
  ear; a concave one reaches it through `KiwiRegion_ConvexPieces:1607` and has
  already proven it succeeds); `SegCross` is strict-`> 0` on all four determinants
  so it can only under-report; `RewindCCW` is the last mutation in `AcceptLoop`;
  `EnsureBuilt` commits its cache keys before returning so the nested call from
  `ResolveCentroid` short-circuits; `KiwiCon_Generation` only moves inside `Touch()`;
  `KiwiHover_DrawWorld`'s MATERIAL_COLOR brackets are balanced; `KREG_FILL_NUDGE`
  (0.5 u) is 32 depth steps against the ortho buffer's 1/64 u. **What is left is one
  line.** `s_normal[i]` read `reg.plane.normal[]` while `kiwi_hover.cpp:206-209` and
  `kiwi_extrude.cpp:213-215` both read `-vpn`. `RB_DrawTriangles_Internal` packs that
  normal per vertex (`rb_backend.cpp:186-217`) and the tools techniques are the
  **vertcol_SHADED** family (`r_rendercmds.cpp:1944-1947` names the resolved
  technique) — so a plane-locked normal presents the same fixed value from BOTH
  sides, and `KiwiTris_OrientToEye` reorders INDICES and cannot touch it. That is
  both halves of the report: "sometimes" = a function of which way the sketch's plane
  faced; "not even from the backside" = a normal orbiting cannot flip. Now `-c->vpn`,
  matching the other two fills. The NUDGE still reads `c->vpn` (round R's argument is
  about depth and is untouched).
- **THE GRID LOD WAS DRIVEN BY CAMERA ALTITUDE, WHICH THE ORTHO IMAGE DOES NOT
  CONTAIN** (item 2). *"There is still (after like 10 attempts) bugs in the grid
  where it snaps and morphs and transforms into an ugly mess."* The screenshots read
  **"P"** on the viewcube pill, i.e. *switch to* perspective — every one of them is
  ORTHO, which since round AJ is the only mode that draws a lattice at all. Ortho
  zoom is `KiwiCam_OrthoHalfHeight()`; altitude is `lookAt[2] - forward[2]*s_dist`.
  So (a) zooming does not move the tier outside a TOP view — altitude is dominated by
  the PIVOT's height — which is why cells swell to fill the screen up close and
  compress into a near-black field far out; (b) ORBITING *does* move it, by up to
  `s_dist`, with nothing on screen changing scale — that is the "morphs and
  transforms"; and (c) the far ring's probe is ray math, and under a parallel
  projection all its rays are the same direction, so it latched ON over tightly
  zoomed images and painted an 8x lattice over them. The ortho arm now derives
  everything from `KiwiCam_WorldPerPixel` (exact and position-independent there):
  spacing = the smallest tier whose cell is >= `KGRID_PX_MIN` (9 px, Schmitt-latched
  at 22), reach = the closed-form XY footprint of the view rect on Z=0 **per axis**,
  centre = the screen-centre ground hit. Density is a function of ZOOM ALONE.
- **A FILLET'S CONTROL POINTS WERE A WALL YOU COULD NOT PICK THROUGH** (item 3a).
  *"bevel is good, but I can't select the curve parts to retexture them."* **None of
  round AF's four gates was the cause — all four were re-verified and all four
  already default permissive** (`m_bSelectCurves` = 1 at prefs.cpp:82/:191; the
  `Curve`/`Terrain` filters `isShown` = true at filters.cpp:982 with registry default
  1 at :1091; the mode mask starts `SEL_MASK_EVERYTHING` at kiwi_selection.cpp:55).
  The fifth cause is one chain: mode 5 carries BOTH the VERTEX and OBJECT bits ->
  `Pick()` runs the screen-space pass first and **returns on a hit**, so the area pass
  never runs -> `ScanList`'s patch branch takes any control point within 8 px -> a
  fillet arc is `spans*2+1` x 3 control points inside one chamfer
  (`kiwi_patchfillet.cpp:1272-1273`), so at working zoom those discs **tile the whole
  visible patch** -> every click became a `SEL_VERTEX` -> `Sel_SyncToLegacy` pushes
  only `SEL_OBJECT` onto `selected_brushes` (kiwi_selection.cpp:352, shakeout D) ->
  `Brush_SetTexture` early-returned (select.cpp:1792) and the texture click was a
  **silent no-op**. It bites fillets and not large patches because the only variable
  is control-point density per pixel. Fixed with the rule `Pick()` already applies to
  faces: control points need the VERTEX bit **and no OBJECT bit**. Patch vertex mode
  (V) sets `SEL_MASK_VERTEX` alone (kiwi_patchverts.cpp:370, asserted at :286) so it
  is untouched; brush vertices are untouched (the change is inside `if (b->patch)`).
- **RETEXTURING A PATCH STRETCHED IT** (item 3b, and it was unobservable until 3a
  landed). `Texture_SetTexture` reaches a patch through `sub_476ED0`
  (brush.cpp:2852-2871) with `a5 == 1`, which swaps the two material POINTERS and
  never re-lays `ctrl[][].texCoord` — **faithful to 0x476ED0 and not changed there**.
  So world-units-per-repeat rescales by `newWidth/oldWidth`. New KIWI fence at the end
  of `TexWnd_ApplyMaterialAtIndex` calls `Patch_KiwiReNaturalizeSelected` (pmesh.cpp,
  beside `Patch_KiwiFinishNewLike` because it needs that file's static
  `Patch_WidthDistanceTo`/`HeightDistanceTo`), which recovers the OLD density **from
  the coordinates themselves** — `WUPR_s = widthDist(width-1) / ctrl[width-1][0].st[0]`,
  `WUPR_t = heightDist(height-1) / -ctrl[0][height-1].st[1]` — and feeds it back as a
  synthesized `texdef_sub_t.size[]`. The old material width cancels, so nothing needs
  capturing before the apply. Round AJ's parent-face inheritance and a manual
  retexture therefore COMPOSE. It also rebuilds `curveDef` on the way out.

### Changed (visual / behaviour)

- **THE GIZMO IS FILLED** (item 4). Arrowheads are closed cones (`KGZ_CONE_SEGS` = 10
  side + 10 cap triangles, opaque), plane handles gain two filled triangles over the
  same `PlaneSquare` corners (0.42 alpha, 0.72 hovered/held), and the origin gains a
  24-triangle billboarded disc over the same `CentreRingPoint` rim (0.85, 1.0
  emphasised). Same `R_AddRenderCmdDrawTris` + neutral MATERIAL_COLOR +
  `KiwiTris_OrientToEye` path as every other kiwi fill. **All the outlines stay**, so
  the hover overdraw, the hit zones and the resolution order are byte-for-byte what
  round AJ shipped. Rotate rings are unchanged (a filled circle is not a ring).
- **THE GIZMO PALETTE IS ONE RAMP STEP BRIGHTER** (item 4). Rest 600 -> 500:
  X `#E12D39`, Y `#27AB83`, Z `#3B82F6`. Hover stays on the 400 ramp, HELD stays
  yellow — three states, still one step apart each.
- **ORTHO DRAWS ONE FLAT BRIGHTNESS TIER** (item 2). The three distance bands model
  perspective attenuation, which a parallel projection does not have, so banding by
  world distance painted a bullseye centred on the viewer — the "banded density mess"
  in the report. Also takes the colour runs from six to two.
- **THE GRID LOD LATCH CARRIES ITS REGIME** (item 2). `s_lodMode` resets `k` when the
  ortho pixel ladder and the perspective altitude ladder swap, so toggling P cannot
  hand one ladder a tier the other chose.

### Instrumented

- **THE REGION FILL PASS NOW REPORTS WHEN IT DREW NOTHING.** Round AG made the
  DERIVATION drops print; the DRAW had no account at all, so "the fill never shows"
  could not be told apart from "the region never derived" without a rebuild.
  `KiwiRegion_DrawFills` counts its four outcomes and prints **only the anomaly**
  (regions exist, none reached the renderer), throttled on the store generation like
  the loop-cap report. A healthy editor never prints.

### Known limitations / open

- **THE ITEM-1 DIAGNOSIS IS STRUCTURAL, NOT INSTRUMENTED.** The normal channel is the
  only difference between this fill and the two that demonstrably work, and the
  symptom ("neither side") is one a per-triangle winding flip provably cannot produce.
  But the `vertcol_shaded` VERTEX shader is not in this tree, so the exact term the
  normal feeds was inferred from the technique name and the packing path, not read.
  If the fill is still absent after this build, the new console line settles the
  remaining question in one keystroke: **regions listed and none drawn = the skips;
  nothing printed at all = the geometry is submitted and the emitter or its blend
  state is at fault**, and the next probe is whether `kiwi_hover.cpp`'s face tint and
  the extrude preview's translucent cap are also missing (same material, same
  technique, same bracket, `-vpn` normal).
- **THE ORTHO REACH CLAMP STILL ENDS THE LATTICE IN MID-AIR AT GRAZING ANGLES**, by
  design (D-AK5). Below `KGRID_ORTHO_MIN_SIN` (= `KGRID_EDGE_FULL`, ~4.6 deg) the
  visible ground span grows as 1/sin while the spacing is pinned to a pixel size, so
  the window is capped at `KGRID_HALF_CELLS` cells per axis and the far ring covers
  what it can. Round S's edge ramp is already dimming and dropping minors through that
  whole regime. The alternative — coarsening spacing by the same 1/sin factor — is v2's
  dead auto-coarsen loop and is refused.
- **THE PERSPECTIVE GRID IS STILL SUPPRESSED** (round AJ item 5b, the user's own
  workaround, unchanged). Everything round AK fixed is ortho-only. The perspective
  lattice's open problems — horizon aliasing, band seams, near-plane crawl — are all
  still there behind that gate, and the altitude ladder is still what would drive it.
- **`KiwiVis_ReportPatchGates` STILL DOES NOT REPORT TWO REAL GATES.** Found while
  proving item 3 and left alone because neither is implicated in the report:
  **Reverse Filter** (`filters.cpp:704` inverts the entire filter verdict for every
  brush, one persisted checkbox) and **edit layer 1** (`filters.cpp:691` filters
  non-fixedsize brushes outright). Both would make a patch invisible AND unpickable,
  because `FilterBrush(b,0)` returns `(brushFlags & 5) != 0` — filtered OR hidden —
  and that one predicate gates the pick as well as the draw (select.cpp:672, :774,
  kiwi_pick.cpp `BrushPickable`, both `kiwi_boxselect.cpp` marquee walks).
- **FILTER TOGGLES ARE NOT PERSISTED, AND A STALE REGISTRY VALUE CANNOT BE REPAIRED
  FROM THE PANEL.** `CFilterWnd_SaveCheck` (`filters.cpp:1096`, declared `qe3.h:1105`)
  **has no callers anywhere in `src/`**. Entries load from
  `HKCU\...\Filters\<name>` with default 1, so a genuine CoD4Radiant install that left
  `Filters\Curve = 0` behind hides every non-terrain patch **every session**, and
  un-ticking/re-ticking in the ImGui panel fixes it only until restart. Workaround:
  delete the value, or re-toggle each session. (This is the same shape as the
  "tool-volumes-missing = filter registry" note.)
- **`m_bSelectCurves` HAS NO UI.** Command id 32852 (`mainfrm.cpp:5860`) appears
  nowhere else in the tree — no menu item, no `Radiant_RegisterCommand`, no keybind,
  no prefs checkbox — and `Prefs_SavePrefs` deliberately omits the write
  (prefs.cpp:279-282, faithful to 0x44f280's omission). It is pinned to whatever
  `Radiant_ProfileGetInt( "Prefs", "SelectCurves", 1 )` returns at startup. Harmless
  today because the default is permissive; worth knowing if a stale registry value
  ever turns up.
- **PATCH -> NATURALIZE IS STILL THE ONLY EXPLICIT NATURALIZE.** Item 3b makes the
  Textures-panel click compose, but a texture applied through any other door
  (`kiwi_uv.cpp:696`, `drag.cpp:734`) still goes through `sub_476ED0` un-relayed.
  Workaround: Patch -> Naturalize by hand, which resets to the editor default density
  rather than the parent face's.

## Round AL

Three items, all follow-ups on round AK. The design rationale for all of them is
in **RADIANT_UX_DESIGN section 65**, decision log D-AL1..D-AL12.

### Fixed (bugs)

- **A FILL'S NORMAL IS A LIGHTING INPUT, AND ROUND AK REPLACED ONE WRONG CONSTANT
  WITH A CAMERA VECTOR** (items 1 and 2, one root cause). *"the blue face only
  shows up at steep angles. Fix this!"* and *"the gizmos are only solid at a high
  zoom level."* The brief's fix — make the region fill byte-identical to the hover
  fill — was **already true at c5457db**: material, technique, MATERIAL_COLOR
  bracket and scope, per-vertex colour packing, `xyzw[3]`, `st`, normal,
  `KiwiTris_OrientToEye`, draw call and pass position all matched, and only the
  nudge constant and the palette differed (the field-by-field table is in section
  65). The hover fill is not a control — it carries the same defect. `d_white` is
  `white_tools`, techset `tools`, whose `unlit` entry is **`vertcol_shaded_tools`**,
  and r_shade.cpp:366-372 records from a live DEVPROBE that a zeroed def constant
  *"NaNs the fakelight vertex colour"* — i.e. the vertex colour the measured pixel
  shader (`lerp(sample*vcol, matColor.rgb, matColor.w)`, r_rendercmds.cpp:1927-1946)
  receives has already been modulated by a term the VERTEX stage computes **from
  the normal**. The shader is not in this tree, so its direction was derived from
  two rounds of user evidence: AK's plane-locked normal made a WALL sketch dark
  **from both sides** (which excludes a view-relative `dot(N,V)` term and a
  two-sided `abs()` one), AL's `-vpn` made it a function of camera PITCH. Only a
  fixed, roughly world-UP light fits both. The fix is neither normal tried: it is a
  **constant**, world +Z, which is also what the BINARY's own fill batcher writes
  (`Face_AddWindingToTriBatch`, brush.cpp:5915-5917, 0x47b86a). The rule now lives
  once as `KiwiTris_FillNormal` (kiwi_lines.h **TRAP 4**) and six emitters use it:
  kiwi_region, kiwi_hover, kiwi_extrude, kiwi_split (x2), kiwi_loft, kiwi_gizmo.
  **This also explains "the fills vanish and the outlines stay"**: line batches are
  immune because `Ed_EmitLineBatch` pushes a per-colour-run MATERIAL_COLOR with
  `.w == 1`, which lerps the vertex term entirely away (r_rendercmds.cpp:1940-1975).
- **THE GIZMO NOW RENDERS ON TOP OF EVERYTHING, AT EVERY DISTANCE** (item 2). A
  gizmo is a HUD object with world coordinates: screen-constant in size, so its
  WORLD extent grows without bound as the view zooms out (about 0.12 * `s_dist` for
  the shaft in ortho), anchored at the selection pivot which is normally inside the
  geometry being moved. `KiwiGizmo_DrawWorld` now opens with
  `R_AddCmdClearScreen(6 = depth|stencil, ...)` — the editor's own device, used by
  the binary for its selected-outline pass (camwnd.cpp:2883-2889, 0x4084d2) and
  already re-issued once by the port for the terrain-paint ring
  (camwnd.cpp:2996-3001). It buys two things the prelude clear did not: every KIWI
  line pass between it and the gizmo **writes depth** (`$line` is depthTest
  LESSEQUAL / depthWrite ON, `refStateBits[1] = 0x0d`) — hover outlines,
  construction lines, marquee, patch lattice, snap accents, the live command
  overlay — and the prelude clear is gated on `!dontDrawSelectedOutlines`
  (mainfrm.cpp:5121), so the gizmo's "on top" hung off an unrelated preference.
  Both call sites are past their `Build*` gate, so a frame with no gizmo costs
  nothing, and the lollipop is mutually exclusive with the gizmo by
  `GizmoUsable()`.
- **`KiwiTris_OrientToEye` TESTED A POINT IN A VIEW THAT HAS NO EYE POINT** (found
  while proving item 2). Under a parallel projection the rasteriser's winding sign
  is `n . vpn`; the camera POSITION does not enter it. KIWI's ortho pseudo-eye sits
  `s_dist` back with a half-height of 0.75 * `s_dist`
  (`KiwiCam_OrthoHalfHeight`), so a vertex at the screen edge subtends about 37
  degrees off the view axis — every triangle within that cone of edge-on was
  oriented the wrong way and culled (the silhouette walls of the gizmo's cones, the
  rim of a large fan). The helper now tests the DIRECTION in ortho and the point in
  perspective, which is each projection's own winding rule. All nine callers
  benefit; the perspective arm is byte-for-byte what it was.
- **THE ORTHO GRID LADDER MEASURED THE CELL AXIS THAT NEVER COLLAPSES** (item 3).
  *"Grid ugliness still there. not fixed."* Round AK held `spacing / wpp` above
  `KGRID_PX_MIN` (9 px). Under a parallel projection a tilted ground plane is
  compressed by **exactly `|vpn.Z|`** along the screen's vertical axis and not at
  all along `vright` (the derivation is in kiwi_grid.h THE FORESHORTENED FLOOR and
  in section 65: with no roll, `h . vup = sin phi` and `vright` is horizontal). So
  a 9 px floor on the free axis is a `9*|vpn.Z|` px floor on the compressed one —
  **4.5 px at 30 degrees, 2.3 px at 15** — which is the reported anisotropic
  moire. The ladder now takes `wppEff = wpp / |vpn.Z|`, i.e. it tests the
  foreshortened extent. Safe against the historic pitch-coupling disease because
  that was a per-line/per-row factor under a perspective divide; this is one scalar
  per frame, identical at every pixel, feeding the same Schmitt latch. Clamped at
  `KGRID_ORTHO_MIN_SIN` (= `KGRID_EDGE_FULL`), so the ladder stops coarsening at
  exactly the angle round S's `EdgeFade` takes over. Straight top-down
  (`|vpn.Z| = 1`) is bit-for-bit round AK.

### Known limitations / open

- **THE `vertcol_shaded` VERTEX SHADER IS STILL NOT IN THIS TREE.** Its direction
  is now derived from two rounds of evidence rather than one, and the model is
  written down so it can be falsified: *the fill's brightness is a fixed-direction,
  roughly world-UP function of the vertex normal.* If a constant +Z normal still
  leaves the fills dim, the next probe is the other immunity — push
  `MATERIAL_COLOR` with `.w = 1` (a flat colour override, what the LINE batches
  already do) instead of the neutral `{0,0,0,0}`. **It carries a real risk and that
  is why it was not taken now**: `.w` is the lerp weight, and if this shader's
  ALPHA output is `matColor.a` rather than `sample.a * vcol.a`, every translucent
  fill in the layer becomes an OPAQUE SLAB — the round-AH failure mode. Try it on
  ONE fill first.
- **THE ITEM-2 ZOOM COUPLING IS NOT PINNED TO A SINGLE LINE.** Two mechanisms were
  fixed (the shading normal, which is fill-only by construction and therefore
  matches "fills vanish, outlines stay"; and the depth ordering, which the clear
  now makes unconditional at any distance), and two suspects were ruled out with
  argument: `KiwiTris_OrientToEye`'s degeneracy test (`n.n <= 1e-12`) fails on
  SMALL triangles, so it would bite at HIGH zoom, the opposite end of the report;
  and render-command-buffer overflow (48 MB, r_rendercmds.cpp:149) drops
  *everything* after it fills, which would take the outlines with the fills. If the
  gizmo is still hollow at a wide zoom after this build, the remaining variable is
  the shading term's magnitude, not its depth state.
- **`KiwiRegion_DrawFills`' ANOMALY COUNTER CANNOT SEE A POST-SUBMISSION DROP.**
  `nDrawn` increments after `R_AddRenderCmdDrawTris`, which returns `void` and
  drops silently when the material lacks the technique or the command buffer is
  full. So it reports derivation-side failures only. It would have printed
  **nothing** for this round's bug — which, per round AK's own decision table, is
  the correct reading ("the geometry is submitted and the emitter or its blend
  state is at fault"). Left as is; the blind spot is documented at the counter.
- **`kiwi_boolean.cpp`'s BRUSH PREVIEW FILL STILL USES FACE PLANE NORMALS**
  (`FillBrush`, kiwi_boolean.cpp:417). Deliberately not converted: that fill is a
  preview of a SOLID, where per-face shading is what makes the solid readable, and
  nothing has been reported against it. If it ever reads as too dark on a
  vertical-walled preview, this is the same TRAP 4 question.
- **THE ORTHO REACH CLAMP STILL ENDS THE LATTICE IN MID-AIR AT GRAZING ANGLES**
  (round AK, D-AK5, unchanged). The foreshortened floor coarsens the SPACING with
  tilt, which reduces how often the clamp bites (a coarser tier has a larger `cap`)
  but does not remove the regime.
- **THE PERSPECTIVE GRID IS STILL SUPPRESSED** (round AJ item 5b, the user's own
  workaround, unchanged). Item 3 is ortho-only, and the perspective ladder is
  untouched.

## Round AM

Seven items plus one mid-round directive. The design rationale for all of them is
in **RADIANT_UX_DESIGN section 66**, decision log D-AM1..D-AM15.

**The headline finding: items 1, 2 and 4 were ONE bug.** "No light blue face on
extrudable line clusters", "need hover highlighting when hovering a face in mode
3" and "gizmo arrows are still not filled in" are three reports of the same
defect on three different shapes — every one of them is an
`R_AddRenderCmdDrawTris` fill on `d_white` inside a neutral `MATERIAL_COLOR`
bracket. That is why four rounds of fixes aimed at the region fill alone all
missed.

### Fixed (bugs)

- **A KIWI FILL UNDER A NEUTRAL MATERIAL_COLOR COMES BACK AT ABOUT A THIRD
  STRENGTH, AND THE MEASUREMENT WAS ALREADY IN THIS TREE** (items 1, 2, 4).
  r_rendercmds.cpp's own RADIANT_LINEVCOL experiment (:1913-1930) records that
  with `MATERIAL_COLOR` neutral — the state that makes the draw
  `sample(colorMap) * vertexColour` — *"the XY brush wireframes came back at
  ~0.32x, so the neutral-matColor route needs the colorMap binding verified
  first"*. The editor draws outside a full scene render, so the tools shaders
  return roughly a third of the colour asked for. A LINE at 0.32x is dim but
  visible; a 0.22-alpha FILL whose colour is also multiplied by 0.32 is nothing.
  That is the asymmetry every round since AA has been describing ("the fills
  vanish and the outlines stay") without naming its cause. The fix is the route
  the lines already took: `Ed_EmitLineBatch` pushes a per-colour-run
  `MATERIAL_COLOR` with `.w == 1`, a FLAT COLOUR OVERRIDE that lerps the sampled
  term entirely away. `KiwiTris_FillFlatColor` (kiwi_lines.h **TRAP 5**) is that
  push, spelled once. **Taken this round on the gizmo HEADS and the ORIGIN DISC
  (alpha 1.00 and 0.85, opaque by design — nothing to lose) and on the REGION
  FILL as the one translucent probe**, which is exactly what round AL's own open
  item asked for ("Try it on ONE fill first"). Round AL refused the probe citing
  "the round-AH failure mode" — but round AH's regression was the grid's far ring
  drawing minors, a LINE-DENSITY bug with no alpha in it, so that precedent does
  not support the risk it was cited for. The risk itself is still real and still
  unproven, which is why the scope is what it is.
- **THE REGION-FILL PASS NOW NAMES THE GATE THAT CLOSED** (item 1). The complete
  call chain from `Cam_Draw` to the D3D submission was walked gate by gate and
  the table with per-gate verdicts is in section 66. Two things came out of it.
  First, **`KiwiRegion_DrawFills` IS called with no construction tool live** —
  every caller was grepped, there is exactly one (kiwi_construct.cpp:4099), it is
  the first statement after the viewport check, and nothing re-gated it after
  round AA; the user's own screenshot proves the enclosing function runs, because
  the construction lines it draws two statements later are visible. Second,
  **`regions.empty()` returned silently and had done for four rounds** — the one
  state in this subsystem with no instrumentation at all. The counter is
  rewritten: it counts TRIANGLES SUBMITTED computed from the submitted index list
  BEFORE the call (round AK's `nDrawn` incremented after a void call and could
  not see a drop — D-AL6), it threads a GATE NAME through every early-out, and it
  probes both of `R_AddRenderCmdDrawTris`' silent drops before they can happen:
  the missing technique via `stateBitsEntry[tech] == 0xFF` (the read
  camwnd.cpp:589 already uses) and the command-buffer wall via the new
  `R_Ed_CmdBufferHeadroom` (r_rendercmds.cpp). Its SILENCE is now informative
  too: it means the geometry reached the backend with a technique and room to
  hold it.
- **A HOVERED FACE NOW HAS A BORDER AS WELL AS A TINT** (item 2). The mode-3
  wiring the report asks for **already existed**: `KiwiHover_Update` picks with
  the LIVE mode mask (kiwi_hover.cpp:707), so in mode 3 the hover result is a
  `SEL_FACE`, `hoverIsFace` accepts it and `EmitFaceFill` emits the cyan tint —
  in mode 3 exactly as in mode 5. What was missing is that the tint is a face's
  ONLY channel (`EmitItem` carried an explicit empty `case SEL_FACE:`), and that
  channel is the one this round found does not reach the screen. So a face gets
  the second channel round AI already prescribed for the outliner hover ("a
  selected brush is a tint AND a white wireframe; this pass had only the tint,
  which is the whole reason it read as weaker"). Shakeout G's directive is not
  reopened: it banned a face reading like an EDGE, and an edge accent is one
  segment at width 2 while this is the face's whole closed border at width 1, in
  a different batch.
- **A PATCH IS NOW CLICKABLE IN FACE MODE** (item 7a). *"I still cant face select
  a curve, makes it hard to correct the texture on the sides and front of the
  fillet."* Mode 3 is `SEL_MASK_FACE` with neither the VERTEX nor the OBJECT bit,
  so a patch hit was rejected TWICE — `ScanList`'s control-point arm skips it
  (`if ( !wantVert ) continue;`) and `Pick`'s tail fell to `r.valid = false`. The
  click then hit `ClickSelect`'s miss arm, which CLEARS the selection, so
  clicking a fillet in face mode did not merely fail, it deselected. A patch has
  no faces (its symbiont brush is a bounding box), so the patch itself is the
  finest surface the mode can name — object granularity is the correct semantic
  and not a fallback, and it is round AK's mode-5 ruling applied to the one mode
  it did not reach. Scoped to patches: prefab/model hits still refuse (spec 2 has
  no editing inside prefab instances). The fillet's arc patch and its two end
  caps are separate patch entities, so each is individually clickable and can
  take its own texture. The mode-3 tooltip enumerates the new target.
- **ALT CHORDS REACH THE HOTKEY TABLE, AND ALT NO LONGER LATCHES** (item 7b).
  *"the alt-s bind to open the surface inspector window doesn't work because the
  win32 topbar steals the input and then locks alt pressed down."* Windows
  delivers a key pressed with Alt as `WM_SYSKEYDOWN`, and every hotkey path in
  this shell filtered on `WM_KEYDOWN`, so Alt+S (Surface Inspector,
  kiwi_keymap.cpp:97), Shift+Alt+S (Patch Inspector), Alt+V (centre rect), Alt+F,
  Alt+R, Alt+H and every Ctrl+Alt row reached neither `Radiant_TryHotkey` nor
  `KiwiUX_KeyFunnel` — they fell to `DefWindowProc`, i.e. the menu-bar mnemonic
  handler. `Radiant_PreTranslateMessage` now routes `WM_SYSKEYDOWN` through the
  same `Radiant_TryHotkey`, whose modifier mask already comes from `GetKeyState`;
  an UNBOUND Alt chord returns false and the native menu is untouched. A matched
  chord is consumed (the pump then calls neither `TranslateMessage` nor
  `DispatchMessage`), **and the trailing bare-Alt RELEASE is consumed with it**,
  because that release is what `DefWindowProc` turns into "activate the menu bar"
  — which is the latch. Guards, all explicit: bare modifiers fall through (round
  U's law, reproduced at the new site), F10 falls through by construction (it
  arrives with no Alt down), and Alt+F4 / Alt+Space / Alt+Tab / Alt+Enter /
  Alt+Esc are refused by name. ImGui text input outranks it, as it does every
  other key path. Alt+MMB is a mouse gesture and never enters this path.
- **THE FILLET'S HAIRLINE SEAM WAS THE CROSS-SECTION TRUSTING AN IDEAL CHAMFER
  PLANE** (item 5). *"The Curve is leaking a small gap on each side."* Bezier sag
  was ruled OUT with a sign: a patch column triple is a quadratic bezier with the
  handle at the tangent intersection, and its mid-span radius is
  `r(1+cos^2(a/2))/(2cos(a/2)) > r` — it bulges OUTWARD, and at mid-span, not at
  the sides. The rails are exact in the ideal (column 0's offset dots to 0 against
  the adjacent face normal and to `-ChamferDepth` against the bisector), which is
  why three rounds of arc math found nothing. What is not ideal is the BRUSH:
  `KiwiBevel_AppendFace` lays three planepts down and the rebuild re-derives the
  plane from them (and snaps them when the preference says to), so the chamfer
  face that actually exists is a few thousandths from the plane the radius was
  solved against. `RefitRails` measures each rail off the geometry that exists —
  the point on the intersection line of the ACTUAL chamfer plane and the ACTUAL
  adjacent plane nearest the ideal one — and re-solves the radius through
  `|rail - mid| = r*sqrt(1-k^2)/k`. The two rail COLUMNS are then written exactly,
  so the seam closes to float precision on both sides. `RowEnds` had already
  conceded this principle for the along-edge axis; this is the other half. A refit
  more than 5% from the dragged radius is refused rather than silently reshaping
  the fillet. `CrossSection` is now the single profile rule the arc patch and both
  end caps share, and `LandCaps` projects onto the refitted plane — refitting one
  and not the other would have closed the rails and opened the ends.
- **THE BEVEL LOLLIPOP'S BALL WAS 42 PX INSIDE THE BRUSH** (item 6). *"the
  lollipop for the bevel command needs to be on the other side (180 flip it)."*
  The anchor was right and has not moved (the chamfer face midpoint at the current
  depth, so the handle rides the live geometry). The DIRECTION was
  kiwi_lollipop.h's "it points the way the drag goes" taken too literally: the
  stem ran along `-n`, into the solid, and the ball sits at
  `anchor + dir * KLOL_STEM_PIX` (42 px), i.e. buried in the geometry the gesture
  is cutting. It now points outward, into the open air in front of the chamfer
  face. Nothing else reads that direction — kiwi_lollipop.cpp uses it for the tip,
  the perpendicular basis and the ball's hit test only — so the gesture, its sign
  and its numbers are exactly what they were.
- **THE GRID'S STRAY DIAGONALS ARE THE FAR RING, AND THE FADE'S CEILING IS THE
  HOLE** (item 3). Identified on four properties: dark (the ring paints the FAR
  band's major colour, the dimmest tier, times `KGRID_FAR_MUL` 0.62 — nothing else
  in the ortho arm draws that dim), long and stopping in mid-air (`ClampAxis`),
  diagonal (world-axis-aligned under yaw) and few (majors only since round AH, at
  8x the near spacing). `EdgeFade` returns 1.0 for every `|vpn.Z| >=
  KGRID_EDGE_FULL` (0.08), so the ring is at FULL strength above about 4.6
  degrees, while between there and roughly 9 degrees the near lattice is
  reach-clamped into a small patch and the ring is the only thing drawing across
  the rest of the view. Two gates, on the RING only and ORTHO only:
  `KGRID_FAR_MIN_SIN` (= 2x `KGRID_EDGE_FULL`, stacked so no angle half-applies
  both) and `KGRID_FAR_MIN_LINES` = 6, counted with the same index arithmetic
  `EmitRun` uses. The near lattice keeps its own ramp and its own ladder; the AXES
  remain exempt from every fade by design.

### Changed (behaviour)

- **A FILLET'S PATCHES ARE NOW "lmap"-ALIGNED AT CREATION** (mid-round directive:
  *"Please set the texture alignment to 'lmap' when doing a fillet curve (for all
  parts). It's the setting that makes it lineup perfect."*). All three parts — the
  arc patch and both end caps — run `Patch_KiwiLmapAlign` immediately after
  `Patch_KiwiFinishNewLike`, which is the same order a user gets pressing Natural
  and then Lmap in the Surface Inspector, with Lmap winning as the directive says.
  Nothing is reimplemented: `Patch_KiwiLmapAlign` is a round-Q-shaped forwarder
  onto the ported per-patch primitive `Patch_Lightmap_Texturing_Sub` (0x4397B0),
  which is file-static. The public spelling (`Patch_Lightmap_Texturing`, the
  button's own path) is NOT used because it walks `selected_brushes` and opens its
  own `Undo_GeneralStart`/`Undo_End`, which inside a commit would be a second undo
  record in one gesture. The fillet keeps its single bracket.
- Mode 3's chip tooltip now says a curve/patch picks as the whole patch.

### Known limitations / open

- **THE FLAT-COLOUR OVERRIDE'S ALPHA BEHAVIOUR IS STILL UNPROVEN, AND THE REGION
  FILL IS THE EXPERIMENT.** `.w = 1` is a full RGB override; whether the shader's
  ALPHA output is `sample.a * vcol.a` (the fill stays translucent) or
  `matColor.a` (the fill becomes an OPAQUE SLAB) cannot be read from this tree.
  **Three outcomes and what each one means:** the region fill comes back as a
  readable translucent light blue — the model is right, and the rest of the layer
  (hover face fills, plane squares, extrude/split/loft) follows next round; it
  comes back as a SOLID light-blue slab hiding the wall behind it — the model is
  also right and the alpha comes from `matColor.a`, and one constant reverts it
  while the gizmo heads keep the fix (they want opacity); it is still invisible —
  the model is WRONG, and the new diagnostic will have printed the live gate.
- **THE NEW DIAGNOSTIC'S SILENCE IS PART OF ITS OUTPUT.** If nothing prints and
  the fill is still invisible, that is a positive result: geometry reached the
  backend with a technique and buffer room, so the loss is blend state or the
  `vertcol_shaded` vertex term. If it prints `first closed gate: DERIVATION`, the
  sketch never formed a region and the gap report that follows says where.
- **`kiwi_boolean.cpp`'s BRUSH PREVIEW FILL, the HOVER face fills, the gizmo PLANE
  SQUARES, and the extrude/split/loft fills all still use the neutral bracket**
  and are therefore still subject to the ~0.32x attenuation. Deliberate: the
  translucent probe is deliberately ONE fill this round.
- **THE FILLET REFIT IS NOT APPLIED TO THE LIVE PREVIEW.** `DrawWorld` still walks
  the ideal arc at `m_radius`, because the chamfer face is being re-cut every
  frame under the drag and refitting per frame would make the preview chase its
  own rebuild. The committed patch and the preview can therefore differ by the
  same few thousandths the fix is about — invisible at preview line widths.
- **THE REFIT ASSUMES THE CHAMFER NORMAL IS STILL `e.n`.** Fillet mode forces bias
  0, so that holds today; only the plane's OFFSET is corrected. A chamfer whose
  plane was re-planed to a different orientation after creation falls outside the
  5% band and lands the ideal arc instead.
- **THE FRAME WNDPROC HAS NO `WM_SYSKEYDOWN` ARM.** Deliberate: the pump covers
  every message the frame sees, and the only messages that bypass it are those in
  nested modal loops (context menus, `DialogBoxParamA`, `MessageBoxA`) where a
  native menu owns the keyboard. Alt chords are not intercepted there, by choice.
- **THE PERSPECTIVE GRID IS STILL SUPPRESSED** (round AJ item 5b), so item 3's
  gates are ortho-only. **THE ORTHO REACH CLAMP STILL ENDS THE NEAR LATTICE IN
  MID-AIR AT GRAZING ANGLES** (D-AK5, unchanged) — the far ring standing down
  removes the longest of those lines, not the regime.
- **NOT RUNTIME-VERIFIED (this whole round).** No build was run; every change is
  reasoned from source and the cited measurements.

## Round AO

Five new reports plus the four deferred out of round AN. The design rationale for
all of them is in **RADIANT_UX_DESIGN section 67**, decision log D-AO1..D-AO16.

**The headline finding: the boolean did not regress.** Not one line of
`kiwi_boolean.cpp`'s carve has changed since round AA — round AF touched only its
two PREVIEW budgets, rounds AL/AM/AN did not touch the file at all, and AL's single
edit to `kiwi_split.cpp` was TRAP 4's fill NORMAL. What "I can't diff an arch into
a square pyramid anymore" found is an older defect that a PYRAMID is the worst
possible shape for.

### Fixed (bugs)

- **A SLIVER FRAGMENT NO LONGER DESTROYS THE WHOLE CARVE** (item 3).
  `KiwiSplit_DefByPlane` is all-or-nothing — a §19 failure on EITHER half frees
  BOTH and returns false — and `CarveByOneTool` turned that single indivisible
  false into a refusal of the ENTIRE TARGET. But the two halves of a subtract do
  not have the same standing. The FRONT half is a piece that would be landed, so a
  sliver front (V3 "face collapsed", V4 "zero-area face" under
  `KVALID_MIN_FACE_AREA` = 0.1 sq units, V6 "planes crossed" under
  `KVALID_MIN_THICKNESS` = 0.01) should be DROPPED — the volume forfeited is
  smaller than the gate that rejected it, by definition. The BACK half is the
  REMAINDER, which the subtract discards by construction, so a sliver remainder
  proves the intersection is itself below the gate, i.e. the two solids MISS and
  the target must be left alone. **A SQUARE PYRAMID IS THE WORST CASE**: an apex
  where four sloped planes meet at a point and four sloped edges where they meet in
  pairs, so the outside slices an arch's many planes cut off it are wedges that
  taper to nothing — and ONE of them under 0.1 sq units anywhere in the cascade
  printed "one brush left untouched — zero-area face" and abandoned everything.
  Against a BOX the same tool never tapers and the same boolean "works", which is
  the shape-dependence the report describes. New `KiwiSplit_DefByPlaneParts`
  reports the halves separately (`KSPLIT_HALF_NONE`/`_OK`/`_SLIVER`);
  `KiwiSplit_DefByPlane` is now a wrapper reproducing its old contract EXACTLY, so
  cut and split are byte-unchanged. **§19 is NOT loosened** — nothing that fails
  the gate is ever landed; only what a failure MEANS changed. Drops are counted and
  reported once per gesture, on both the boolean and the cave carve.
- **THE BOOLEAN NOW SAYS WHEN IT SKIPPED A CURVE** (item 3, the round-AM half).
  Round AM made a patch clickable in FACE mode, so a patch now ENTERS
  `selected_brushes` where the click used to bounce — and a mapper rubber-banding
  "the arch" can be holding patches (round AG's patch-cylinder mode exists
  *because* "a boolean'd cylinder arch comes out as a fan of sliver faces").
  `Usable` dropped every one of them in silence (csg.cpp:572's ported validation
  refuses patches: no volume, so nothing to carve and nothing to carve with).
  `Begin` now counts and names them, and the tool-pick click says "that is a
  curve/patch — use the caulk brush inside it as the tool" instead of the generic
  "not a usable solid", which was equally true of empty space.
- **THE LOFT'S CORRESPONDENCE WAS MEASURED IN THE WRONG PLANE, AND THAT IS WHY
  PERPENDICULAR FACES NEVER WORKED** (item 1). Step 4 compared the two rings after
  projecting out the STRAIGHT AXIS. That is an isometry only when the faces are
  roughly parallel. When they are not, ring A lies at angle α to the axis and ring
  B at α on the other side, so the projection squashes each by cos α **along two
  different in-plane directions, 2α apart** — at 90°, A's square projects to a
  rectangle squashed one way and B's to a rectangle squashed the ORTHOGONAL way,
  and the cyclic offset is chosen between two shapes whose aspect ratios are
  inverted. It is routinely ONE VERTEX OUT, i.e. a 90° twist, and a twisted square
  bridge has side quads that cross, which §19 rejects. **THE TWIST IS DECIDED
  BEFORE ANY STATION MATH RUNS** — which is exactly why the report says it "will
  not work no matter what". Fixed with a ROTATION-MINIMISING FRAME: A's frame is
  carried to B by the minimal rotation nA→nB (Rodrigues about `normalize(nA × nB)`
  through `acos(nA·nB)`), which by definition has no component about the normal and
  so introduces zero twist; the correspondence is then measured as 2D coordinates
  in those aligned frames.
- **AND THE SAME CHANGE IS WHY THE LOFT NO LONGER PINCHES** (item 1, "the curve
  needs to be the same thickness"). Blending corresponding vertices in WORLD space
  between two non-parallel planes shrinks the section: for the pictured wall the
  midpoint ring comes out at w·√2/2 ≈ 0.71 of the wall's width. Stations are now
  built as the 2D blend of the two profiles mapped THROUGH the frame at that
  parameter, so **congruent profiles stay congruent at every station** and every
  station is **planar by construction** (step 6's projection becomes exact and can
  no longer introduce a twist of its own). With nA = nB the rotation is the
  identity and the whole thing reduces algebraically to the old world-space lerp,
  term for term — every loft that already worked builds the same brushes.
- **THE LOFT PREVIEW NOW RUNS THE COMMIT** (item 1, *"dont make the preview blue
  until it will actually work (misleading! and a time waster!)"*). Steps 7 and 8 —
  build every segment, gate it against §19 — ran only inside `Apply`, so the
  preview was blue whenever STATIONS existed. `Rebuild` now performs the whole
  build as a DRY RUN, frees every def and goes BAD-red unless the commit would
  succeed ("rejection is free", the property kiwi_extrude.h states and
  `KiwiBool_WouldCarve` already relies on). **The TANGENT→RULED retry is previewed
  too**, so when the retry is what will happen the preview shows the RULED bridge
  and the HUD reads `RULED (tangent refused)` BEFORE the click. `Apply` no longer
  decides anything — it refuses with the same words the HUD was showing.
- **ALT+LMB REACHES THE TERRAIN PAINT** (item 4, deferred from AN: *"The circle
  shows up but you can't paint with alt-click. The inputs get eaten."*). The
  shell offers every camera press to `KiwiVP_CameraButtonDown` first
  (imgui_shell.cpp:407) and runs the legacy handler only on false (:412) — and the
  `KiwiBox_Begin` arm **claimed every LMB press as `KG_MARQUEE`, unconditionally,
  with no test of Alt at all**. So `CamWnd_OnLButtonDown` was never called, and
  with it the whole ported chain `CamWnd_DropModelsToPlane` (camwnd.cpp:3334) →
  `Drag_Begin` (drag.cpp:571), whose `LABEL_34` arm at drag.cpp:650 IS the paint
  start → `Patch_Paint_Start` (pmesh.cpp:5994). **The ring is drawn from the SAME
  `sub_401D50` gate** (camwnd.cpp:2988), which is precisely why it appeared while
  the stroke did not. Cleared by name, not by assumption: round AM's
  `WM_SYSKEYDOWN` routing cannot see a mouse gesture (there is no `WM_SYS*BUTTON*`
  message in Win32 — an Alt+click is a plain `WM_LBUTTONDOWN`);
  `io.WantCaptureMouse` gates the legacy hidden child windows, not this dispatch;
  Alt+RMB mouselook is latched in the RMB arm and this arm is LMB-only. The fix is
  a DECLINE, not a new gesture — returning false lets the ported chain run, and the
  drag and release follow by themselves because both later arms already return
  false while no modern gesture is live. The undo bracket is the PORTED one
  (`Patch_Paint_Start` opens it, `Drag_MouseUp`'s `sel_addpoint` arm closes it).
- **MATCH FACE ACCEPTS A CURVE AS THE SOURCE** (item 6, deferred from AN: *"the cap
  needs to be adjustable, I would use the (Z) match face command on the curve
  itself."*). The source stage demanded exactly one brush FACE; it now also takes a
  PATCH, fits the patch's own plane over its control grid and projects the control
  points onto the target face's plane ALONG THAT NORMAL — exact for a planar cap,
  not an approximation. Non-planar patches, a degenerate patch normal, and a normal
  near-perpendicular to the target plane are all REFUSED with a message naming the
  reason. One undo record; the projected control net is drawn before the click.
- **HIDDEN SOLIDS SURVIVE SAVE AND LOAD** (item 7, deferred from AN). The hidden
  bit lives on the `selbrush_t` INSTANCE, so it can never be a .map field — the
  .map has no instances. It goes in the KIWI2 sidecar as optional `hiddenbrush N`
  lines, N being the ordinal in **Map_SaveFile's own brush walk**, matched to that
  walk exactly. No version bump: a sidecar without those lines behaves exactly as
  before. The apply runs at the TAIL OF THE MAP LOAD and not in the sidecar reader,
  because the instances the bit lives on only exist once the load has built them.

### Added / changed

- **THE LOFT HAS A THIRD BRIDGE MODE: CURVE** (item 1, *"I want the option to make
  the loft a curve"*). RULED and TANGENT still build BRUSHES and are untouched.
  CURVE builds q3 BEZIER PATCHES over the same stations: every corresponding
  profile EDGE sweeps to one patch strip, so a rectangular wall section becomes
  four strips — two large sides plus top and bottom, i.e. the tube, with both ends
  left open because the source faces are already there. Control grid is
  `2·spans + 1` columns × 3 rows; EVEN columns sit on the stations and ODD columns
  are the ARC HANDLE (the meeting point of the two stations' path tangents,
  kiwi_patchfillet.cpp's `ArcPoint` rule generalised, solved as the closest
  approach of two lines with a midpoint fallback for parallel tangents, a meeting
  point behind the span, or a handle further than 2× the chord). **THICKNESS IS
  FREE** — both sheets of the wall are the same profile swept from the same
  stations, so there is no offset surface to keep in step. **TEXTURES** come from
  the face of brush A adjacent to the profile edge each strip continues, resolved
  geometrically, and the alignment is `Patch_KiwiCapAlign` (round AN's directive
  for the fillet's SWEPT surface — a loft strip is the arc's shape, not a flat end
  cap); the landing sequence is kiwi_patchfillet.cpp:1441-1559 in order, not a
  second spelling. **NO COLLISION** — patches are render surfaces and the caulk
  hint prints on every creation.
- **A TWIST STEPPER** (item 1). `[` and `]`, plus a panel row, step the ring
  correspondence by whole vertices on top of the computed offset. The automatic
  answer is now measured in the right frame, but a profile with rotational symmetry
  has several offsets of near-equal cost, and when the refusal IS a correspondence
  this rescues it in one keypress. `D` now CYCLES the three modes rather than
  toggling two.
- **FILLETS FOLLOW A FACE PUSH** (item 2, *"When extending a surface that has
  fillets, the fillets also need to stretch with the surface."*). There is no
  stored link between a brush and its fillet patches and there is not going to be
  one: a persisted id is only as good as the round trip through a text file other
  tools write, and a STALE id names the wrong brush with full confidence. The
  association is GEOMETRIC and re-derived every time, which is the only design that
  is still correct after a map RELOAD — and a fillet's own construction makes it
  decidable, because round AM's `RefitRails` put the rail columns exactly on the
  brush's planes. ONE RULE: a control point on the moved face's plane AND inside
  that face's winding moves with the plane, and interior rows are re-interpolated
  so `KPF_PATCH_ROWS`' "both ends and the exact midpoint" grid stays true (an end
  that moved while the midpoint did not would leave the patch straight but
  non-uniformly parameterised — a texture stretch on the very surface the round
  AM/AN alignment work exists to make line up). The arc STRETCHES; the end cap,
  lying wholly on the moved plane, translates whole and stays sealed. Same undo
  record as the push, each patch covered with `Undo_AddBrush` inside the open
  bracket; post-mutation bookkeeping is `Patch_Rebuild( p, 1 )`, the ported tail —
  **not** `bDirty`, which means "carries an explicit sample size" and gates `size`
  in `Patch_Write`.
- **AUTO BOOL TRIES HARDER** (item 5, deferred from AN: *"Make it try combining all
  the solids at once along with more combinations."*). Three phases, each reported
  separately: the existing pairwise fixed point, then a CLUSTER phase that builds
  the touching graph (union-find) of what is left and hands each connected
  component of 3+ to `Brush_MergeList` WHOLE, then a REVERSE pairwise sweep.
  **The cluster phase is guarded by TOTAL VOLUME CONSERVATION** computed from the
  windings by the divergence theorem, because `Brush_MergeList` misclassifies inner
  faces across non-touching flipped-equal planes and will otherwise return a merged
  brush that swallows empty space; a component that fails the check is restored.
  The reverse sweep exists because `MergeList`'s pairwise result is order-dependent.
- The Y panel no longer promises "LMB raise, RMB lower". Alt+RMB is the mouselook
  in this shell, so only the LMB half is routed; mode 0's amount is
  `grid_sizes[gridsize] * sign * 0.5` and `sign` comes ONLY from the button, so no
  panel control can invert it. Smooth is the way down, and the panel says so. A
  literal `Alt+LMB — Paint terrain` chip appears only while the tool is armed.

### Known limitations / open

- **THE FILLET CARRY IS SCOPED TO THE FACE PUSH, AND THE AXIS-LOCKED BRUSH MOVE IS
  NOT DONE.** The geometric association is decidable for a single moving PLANE (on
  the plane, inside the winding). A whole-brush move has no equivalent
  discriminator that does not also claim a NEIGHBOURING brush's fillet, and
  inventing a looser one would silently deform someone else's geometry — the exact
  failure mode D-AO13 rejects a stored id for. Selecting the fillet patches along
  with the brush already carries them, which is why this is a gap rather than a
  wrong answer. ROTATE, VERTEX edits, free-form drags and SCALE are likewise not
  carried, by design: none of them is a single plane translation.
- **A CHAMFER-PLANE PUSH REFUSES TO CARRY THE FILLET, LOUDLY.** When the on-plane
  control points do not form complete ROWS the move is cutting the patch's
  cross-section, and carrying it correctly means re-solving the RADIUS against the
  new chamfer plane (round AM's `RefitRails`), which cannot be done from a landed
  patch without reconstructing the gesture's `filletUnit_t`. Moving the rails
  without re-solving would shear the arc and re-open the seam round AM closed. The
  console says to re-run the bevel.
- **THE LOFT'S DRY RUN COSTS A FULL BUILD PER STATE CHANGE.** Up to 128 brushes are
  allocated, gated and freed on every density/tension/mode/twist change — including
  every frame of a density SLIDER drag (round AN made that row a drag slider). That
  is one commit's worth of work per frame, bounded and fast, but it is real and it
  is the price of the preview being honest.
- **CURVE MODE'S DENSITY CEILING IS 7**, because `2·spans + 1` must stay inside the
  patch format's control grid (`Patch_GenericMesh` refuses a width outside 3..15,
  pmesh.cpp:1550). The brush modes keep round AN's 128. The HUD shows the active
  ceiling.
- **CURVE MODE EMITS NO END CAPS AND NO COLLISION.** The ends are deliberately open
  (the two source faces are already there). A caulk brush inside the curve is the
  mapper's job, as it is for every other patch in this editor.
- **THE LOFT'S ANTIPODAL CASE IS REFUSED, NOT GUESSED.** Two sections exactly back
  to back after the sense rewind have no defined bridge direction.
- **ITEM 7's ORDINAL ASSOCIATION IS FRAGILE AND SAYS SO.** It breaks if the map is
  edited outside KIWI or if the save and load orders ever diverge. Out-of-range
  ordinals are ignored rather than erroring, a wrong hide is harmless, and Unhide
  All fixes it — which is the whole reason an ordinal is an acceptable key here and
  is refused for item 2's fillet association, where being wrong deforms geometry.
- **ITEM 3'S SLIVER DROP IS SILENT ABOUT WHERE.** The count is reported, not the
  location. A mapper who loses a fragment is told how many, not which — locating
  them would mean landing and highlighting geometry that failed §19, which is the
  thing the gate exists to prevent.
- **THE ROUND-AM FLAT-COLOUR PROBE IS UNCHANGED AND STILL UNREPORTED.**
  `kiwi_boolean.cpp`'s brush preview fill, the hover face fills, the gizmo plane
  squares and the extrude/split/loft fills all still use the neutral bracket and
  are still subject to the ~0.32x attenuation. Nothing this round touched that
  question.
- **NOT RUNTIME-VERIFIED (this whole round).** No build was run; every change is
  reasoned from source and from the cited definitions.

## Round AP

Two user reports on top of e765821 (round AO). One is a WITHDRAWAL of a shipped
feature; one is a live-drag oscillation. Full write-up in RADIANT_UX_DESIGN.md §68.

### Fixed / changed

- **THE EXTRUDE NEVER CARVES BY ITSELF ANY MORE** (item 1, verbatim: *"when
  extruding, dont automatically bool diff the solid. It can be done by the user with
  a boolean after."*). Round AF item 2's push-in cave is **removed** from
  `kiwi_extrude.cpp`: the `DragsIntoSolid()` point probe
  (`KiwiBool_PointInSolid` one `KEXT_MIN_DIST` along the drag from the profile
  centroid), the `Carve()` commit (build → `KiwiBool_WouldCarve` dry run →
  `KiwiBool_DifferenceByDef` cascade inside a `"carve region"` bracket → free), the
  `m_carve` member, the amber `KEXT_COL_CARVE` preview branch, the `CARVE (cavity)`
  HUD rung, the cavity sentence in the entry message and the `kiwi_boolean.h`
  include are all gone. **A region extrude now always lands its prism**, in both
  drag directions, overlapping whatever it overlaps. Carving is Q afterwards, which
  reaches the same `CarveTarget` subtract and therefore produces the same geometry.
- **ROUND Q'S FACE UN-EXTRUDE IS UNTOUCHED — checked, not assumed.**
  `KEXTF_CARVE` / `KEXTF_DESTROY` on `KiwiExtrudeFaceCommand` share the word "carve"
  with the removed feature and nothing else: they call `KiwiXform_PushFaceOnce`,
  which moves ONE face plane of ONE brush (the same edit the lollipop performs by
  hand), never touch a bystander, and are triggered by the SIGN OF THE DRAG the user
  is steering and can read in the HUD before confirming. The verdict and its three
  tests are recorded in `kiwi_extrude.h`.
- **THE CONSTRUCTION MOVE NO LONGER SNAPS TO ITSELF** (item 2, verbatim: *"when
  dragging a line set using the move gizmo, it teleports back and forth from the
  pivot point to the arrow…"*). **Root cause:** `PICKF_EXCLUDE_SELECTED` — the flag
  that stops a transform snapping to what it is dragging — reaches only the snap arms
  that go through `Pick()`, i.e. the BRUSH arms. Construction candidates are walked
  straight out of the store in five places in `kiwi_snap.cpp` (scope ruling 1 keeps
  them out of `Pick`), so nothing filtered them. The geometry-snap arm is ABSOLUTE
  (`total = snapPos - m_ref`), so with a moved anchor as the target it solves
  `total_new = total_old + (base - m_ref)` — it adds the anchor's offset from the
  reference point every frame. The set jumps out of the 8 px radius, the arm misses,
  `total` falls back to the cursor mapping and puts it back under the arrow, the
  anchor re-enters the radius: a two-state limit cycle at frame rate, between "the
  reference point is at the cursor" and "some anchor is at the cursor" — the pivot
  and the arrow, verbatim. **Fix:** `KiwiConSel_SnapMuted( objectIndex )` (off
  `s_moveBase`, the same list `MoveApply` writes through) plus ONE shared gate
  `ConCandidateUsable( o, i )` folding it with the round-U hidden test across all
  five walks. Per-gesture only; construction geometry is a snap target again the
  moment the drag ends.
- **…EXCEPT WHILE PLACING THE PIVOT**, which is the same exception `PickFlags`
  already makes (`m_pivotPlacing ? PICKF_NONE : PICKF_EXCLUDE_SELECTED`) so a pivot
  can land on the very thing being moved. Safe as well as symmetric: a live placement
  is consumed by `TrackPivot` and never reaches `Recompute`.
- **THE PIVOT NOW TAKES CONTROL ON THE CONSTRUCTION ARM TOO.**
  `KiwiMoveCommand::Begin`'s construction branch returned before the shakeout-G line
  `m_pivotOverridden = PivotActive( m_ref )`, so it was the one Move arm that ignored
  a placed pivot outright. It now asks the same question at the same point in the
  sequence (after the natural anchor, before `LatchMapStart`), and the construction
  `Commit` branch — which likewise returned before round L's `PivotRide` — now runs
  the same ride, in the same order, before `Reset`.

### Verified-correct during the item 2 audit (recorded so it is not re-searched)

- `m_ref` on the construction arm is **latched once** in `Begin` from
  `KiwiConSel_MoveBegin`'s anchor centroid and never rewritten. It is not recomputed
  per frame from the live (already-moved) selection.
- `KiwiConSel_MoveApply` is **baseline-absolute**: every world point and the plane
  origin are written as `base + delta` over the `s_moveBase` snapshot taken at Begin.
  Nothing compounds, so the full-total application composes with round AN's
  owned-axes carried-base rule without double counting.
- **Round AN did not cause this.** AN changed *which axes* a snap arm may write; the
  self-snap loop predates it and fires identically under AN's predecessor.

### Known limitations / open

- **THE THREE BY-DEF BOOLEAN ENTRY POINTS NOW HAVE NO CALLER.**
  `KiwiBool_DifferenceByDef`, `KiwiBool_WouldCarve` and `KiwiBool_PointInSolid` were
  the extrude cave's, and only the extrude cave's — Q's interactive command carves
  with a brush the user clicked and goes through `CarveTarget` directly. They are
  kept, and `kiwi_boolean.h` now says so plainly: they are the documented route for a
  future tool that wants to cut with scaffolding it built, and two sibling subsystems
  cite them by name as the definition of the dry-run and the on-plane tolerance. If a
  later round wants them gone, that is a deliberate deletion, not a cleanup.
- **THE SELF-SNAP MUTE COVERS THE CONSTRUCTION *MOVE* ONLY.** Any future gesture that
  mutates construction geometry live (a per-point drag, a parametric handle) will
  need to declare its own mute; the hook is `KiwiConSel_SnapMuted`, but it keys on
  `s_moveBase`, which only the move gesture fills. v1 whole-object-only construction
  moves are the only live-mutating construction gesture today.
- **THE REGION EXTRUDE CAN NOW LEAVE INTERPENETRATING BRUSHES, BY DESIGN.** That is
  what the directive asks for. Nothing warns about it, because a warning on a legal
  and deliberate act would be the same kind of noise the auto-carve was.
- **A CONSTRUCTION MOVE STILL DRAGS WHOLE OBJECTS ONLY** (v1, unchanged since
  shakeout F): a `KCONSEL_POINT` or `KCONSEL_SEGMENT` item moves its whole object.
- **NOT RUNTIME-VERIFIED (this whole round).** No build was run; every change is
  reasoned from source and from the cited definitions. In particular the oscillation
  mechanism is derived from the code path, not observed under a debugger — the
  recurrence is stated in §68 so it can be checked against behaviour directly.

## Round AQ

Eight user reports on top of 52027e0 (round AP), including a hard crash with a stack.
Full write-up in RADIANT_UX_DESIGN.md §69.

### Fixed (bugs)

- **CRASH: the terrain-paint ring read past the end of a patch** (item 1, user stack:
  `PMESH_19_Radius` line 5046 <- `DrawAdvancedTerrainEditCircle` line 2133, read AV,
  `turnRow` = `0x60CF436`). Reproduced by making a **patch cylinder** (`P` mode) with the
  advanced terrain-edit `Y` ring live. `PMESH_19_Radius` iterates the TESSELLATED mesh
  (`curvePatchDef_t::width/height`) and indexed the 16x16 CONTROL grid with those same
  indices. The `1280` in that walk is `sizeof(drawVert_t[16])` — one ROW of
  `patchMesh_t::ctrl[16][16]` at `+0x38` — and it is faithful; the bounds were not. Any
  patch that tessellates past 16 columns walked `1280*col` bytes off the end of the whole
  20 556-byte `patchMesh_t`. Now indexed as the typed array (`patch->ctrl[col][row]`) and
  consulted only for a TERRAIN patch with in-range indices, which is the rule
  `Patch_Fill_BuildFrontIndices` (0x43FB70) already applies to the same flag.
- **The terrain-paint ring is refused on non-terrain patches**, with one console line per
  editor run. A cylinder/bevel/endcap/cone is not a terrain sheet and the paint modes
  cannot edit it, so drawing on it was never meaningful.
- **The terrain-paint ring was offset from the cursor** (item 3). It built its ray with
  the ported, deliberately perspective-only `CameraCalcRayDir` plus `camera.origin`, while
  KIWI's camera is ORTHOGRAPHIC by default — so the hit point was displaced by the full
  parallax, zero at the image centre and growing toward the edges. Also flipped with
  `cph - cpy` instead of the tree's `height - y - 1`. Now goes through
  `Pick_RayFromImagePos`, the canonical picker hover/gizmo/box-select/snap all use, which
  already carries both ortho arms.
- **Circle side count disagreed with the tessellation until Tab** (item 6). The circle,
  2-point circle and arc previews build a throwaway `kconObject_t` and never assigned its
  `segs`, so the rubber band drew at the AUTO radius-driven count while the field, the HUD
  and the committed object all used `m_sidesOverride`. Two more of the same class fixed
  with it: `KiwiArcTool::ToolSides` was missing the `CardinalSegs` rounding its own
  geometry applies (field said 30, mesh used 32), and the **n-gon** never re-derived
  `m_sides` from a typed count until the tool restarted — its Tab field was inert for the
  whole gesture (the comment claiming `Recompute` folded it back in was stale; it does
  now).
- **Enter early-committed staged creation tools** (item 8, verbatim: *"I tab, type in 120,
  then press enter. This should confirm step1 of the creation operation, not submit it
  early with 0 z height"*). Enter went straight to `KiwiCmd_Commit`, which a staged tool
  refuses below its final stage — so the most natural way to accept a typed number was the
  one input that guaranteed nothing was built. New `KiwiEditorCommand::AdvanceStage()`
  hook, consulted in exactly one place (`KiwiCmd_KeyDown` rung 5). Overridden by the five
  primitives (box, centre box, cylinder, sphere, cone) and by Construct Arc. RMB confirm
  and the options-panel Confirm button inherit it because `KiwiCmd_Confirm` routes through
  the same rung.

### Changed (behaviour)

- **THE REGION FILL IS NOW BYTE-IDENTICAL TO THE BOOLEAN'S RED PREVIEW, INCLUDING THE
  DRAW PASS** (item 2, verbatim: *"You seriously need to fix the light blue construction
  plane visibility!"*, fifth round). Two changes: round AM's `KiwiTris_FillFlatColor`
  probe is **withdrawn** (the pass-level neutral MATERIAL_COLOR bracket now covers every
  region, which is the boolean's exact submission state), and `KiwiRegion_DrawFills` is
  **relocated** out of `KiwiCon_DrawWorld` into the command-overlay slot in `camwnd.cpp`'s
  Cam_Draw tail — immediately before `KiwiCmd_DrawWorld`, inside its own
  `KiwiLines_Begin`/`Flush` pair, which is where the boolean's `FillBrush` runs. The AM
  diagnostic is kept. **This is a falsifiable submission**: if the fill appears, pass
  location was the bug; if it does not, there are ZERO differences left between a visible
  fill and an invisible one and the next round starts at the renderer's blend state.
- **Numeric entry takes expressions and unit suffixes** (item 4). Standard precedence
  `+ - * /`, optional parentheses, unit suffixes `y/yd/i/in/f/ft` (plus the long forms)
  binding to the number before them, compounds by juxtaposition (`10ft6in`, `10f6i`), bare
  numbers in inches, fractions as ordinary division (`1/8` = 0.125 in, `10ft/2` = 5 ft).
  All conversion still happens at the one `Units_FromDisplay` boundary. Operators come
  from the numpad and from OEM keys unshifted; `*`, `(`, `)` from a narrow Shift+8/9/0
  arm. Unit letters are accepted **only when the field is a LENGTH and already has text**,
  so tool hotkeys are untouched with an empty field. The value bubble shows
  `10ft6in = 10 ft 6 in` for a live expression and `(incomplete)` for one that does not
  evaluate yet.
- **The viewcube grid-spacing popup uses the same parser** — it was the tree's only other
  `atof` on typed text. `ImGuiInputTextFlags_CharsDecimal` removed so operators and unit
  letters can be typed there.
- **Trim deletes strays** (item 5, verbatim: *"Allow the trim tool to delete whole lines
  (some become stray lines and it's easy to just mash click with T on)"*). A hovered
  chain with NO crossings — open or closed — highlights whole and a click deletes the
  object, one `KiwiCon_UndoPush` per click. Round T's rule is preserved, not reversed: an
  INCOMPLETE crossing scan (the `KTRIM_MAX_PAIRS` cap) still refuses before this case is
  reached, so a MISSED crossing still cannot become a delete. A closed loop with exactly
  one crossing is still refused with round AF's explanation. Hints updated.
- **The boolean carves what it can and says what it skipped** (item 7, verbatim: *"I'm
  having trouble dragging arches into boxes. This needs to work. The boolean tool needs
  to be more robust."*). A hard refusal on one tool x one piece no longer abandons the
  whole difference — it is treated as that tool having missed that piece, which is exactly
  what it is (a refusal frees everything it made and mutates nothing), and it is reported.
  An extruded arch is a convex decomposition of many near-tangent wedges, so under the old
  rule one bad plane anywhere in the fan left the box whole. Section 19 is NOT relaxed;
  nothing invalid is landed.

### Instrumented

- **Per-refusal boolean reporting.** `CarveByOneTool` now distinguishes the two MISS
  verdicts by name and by plane index — *"no shared volume — tool plane N leaves the
  target entirely outside it"* vs *"the shared volume collapsed below the validity gate at
  tool plane N (a near-tangent cut: <the V-rule>)"* — and the bounds early-out names
  itself. Each skipped cut prints tool index, piece index and gate (capped at 8 lines per
  operation, with a tally after). Target-level `KBOOL_MISS` prints its reason on both the
  interactive and by-def paths instead of the old bare *"did not meet the tool(s)"*. The
  dry run (`KiwiBool_WouldCarve`, asked every hovered frame) is silenced.

### Known limitations / open

- **NOT RUNTIME-VERIFIED (this whole round).** No build was run; every change is reasoned
  from source and from cited definitions.
- **THE REGION FILL MAY STILL NOT APPEAR.** That is the point of the submission. If it
  does not, the submission-side model is dead and the remaining candidates are inside
  `RB_DrawTriangles_Internal` — blend state, or the vertcol_shaded vertex term.
- **THE OTHER TWO CLASSIC ORTHO RAY PATHS ARE STILL PERSPECTIVE.** The 3D marquee quad and
  `Camera_GetRectSelection3D` (both `camwnd.cpp`) still call the ported
  `CameraCalcRayDir` directly. Only the terrain ring moved this round.
- **THE SHIFTED OPERATOR KEYS ASSUME A US LAYOUT.** Shift+8/9/0 -> `*`/`(`/`)` is a VK
  mapping, not a layout-aware one. The numpad and OEM routes give unshifted access to
  `+ - * /` on any layout, but `(` and `)` are US-only. A layout-aware path would need the
  WM_CHAR stream, which this funnel deliberately does not use.
- **UNIT LETTERS CAN STILL BE PRE-EMPTED.** The funnel's preempt/swap rungs run before the
  numeric layer, so a letter bound to a swappable verb could be taken as that verb while a
  gesture is idle. No unit letter is bound to a swap verb today, and the gate ("field is a
  LENGTH and already has text") means the ambiguity cannot arise mid-number.
- **A PARTIAL CARVE IS NOW POSSIBLE.** If the boolean skips a cut, the hole is incomplete
  where that plane fell. It is announced, per cut and in a summary line, but nothing
  prevents it — that is the deliberate trade against the old "discard everything".
- **THE ARCH-INTO-BOX ROOT CAUSE IS NOT PROVEN, ONLY MADE SURVIVABLE AND VISIBLE.** No
  console output from the failing case has ever been seen. The instrumentation above is
  built to name the exact gate on the next attempt.
- **`AdvanceStage` IS OVERRIDDEN ON SIX TOOLS.** Any future staged tool that wants the
  Enter rule must override it; the default is false, which lands on the old commit
  behaviour. Two-stage construction tools deliberately do not override it — their typed
  value is already applied by `Recompute` before `Finish`, so Enter commits correctly.

## Round AR

Two user items on top of 96e593b (round AQ): Copy/Paste for construction geometry, and
the boolean that fails after a JOIN and works without one. Full write-up in
RADIANT_UX_DESIGN.md section 70.

### Added

- **CONSTRUCTION GEOMETRY HAS COPY AND PASTE** (item 1, verbatim: *"allow copy pasting
  of construction lines! after a paste, it should automatically go into G(move) mode."*)
  Wired to the SAME classic ids the brush clipboard uses — `Ctrl+C` = 33039, `Ctrl+V` =
  33040 (`mainfrm.cpp`) — so there is one Copy and one Paste in the editor and no third
  key. The ported handlers run unchanged; `KiwiConClip_Copy` / `KiwiConClip_Paste`
  (new `kiwi_conclip.cpp`) run beside them and are no-ops when their side of the
  selection is empty.
  * The payload is a **process-local vector of `kconObject_t` VALUES**, not the OS
    clipboard and not store indices. A mixed selection needs BOTH payloads at once and
    the OS clipboard has one slot for this format; indices shift on every
    `KiwiCon_RemoveAt` and would paste the wrong lines after a delete.
  * A **whole object** is copied whatever the item granularity — a selected point or
    segment names its object, which is the rule G's construction arm already applies.
    Duplicates (three segments of one polyline) collapse.
  * A pasted object is **visible and ungrouped** (`hidden` false, `group` -1) and lands
    **in place**; everything else — type, points, `closed`, the parametric block,
    `segs`, `name` — carries verbatim, and `KiwiCon_Add` normalises and refits it like
    any drawn object. It persists in the sidecar with no special casing.
  * **ONE `KiwiCon_UndoPush` for the whole paste**, before the first Add, so one
    construction Ctrl+Z removes all of it.
  * The paste **replaces the construction selection with exactly the pasted set**
    (`KiwiConSel_SelectObjects`, new) and then **auto-enters a paused Move** through the
    existing `KiwiCmd_AfterPaste` hook — no new machinery: `KiwiXform_CanMove` already
    answers true for a pure construction selection and Move already has its construction
    arm.
- **A COPY THAT TOOK SOLIDS AND NO LINES CLEARS THE LINE CLIPBOARD.** Without it the two
  stores drift and a later Ctrl+V pastes a mixture the user never assembled. A Copy with
  *nothing* selected still leaves both alone (the existing Cut rule).

### Fixed (bugs)

- **JOIN WELDED AT A DIFFERENT TOLERANCE THAN THE WALK THAT AUTHORISED IT** (item 2b).
  `KiwiRegion_ChainWalk` welds two ends into one node at `KiwiRegion_WeldDist()` —
  grid-scaled since round AF, `clamp( grid * 0.25, 0.5, 16 )` — while
  `KiwiConSel_Join`'s concatenation and its closing-vertex drop both used the bare
  `KREG_JOIN_DIST` floor of **0.5**. At any grid of 4 or coarser the walker therefore
  declared two ends identical and the join stored BOTH: a **stub edge** at every
  junction (up to 16 units) and a **duplicate closing vertex**. Those are exactly what
  section 23's prism builder turns into two nearly-identical side planes (V5 "duplicate
  plane") and the boolean into a near-tangent cut plane — and the UNJOINED outline never
  acquires them, because nothing bakes the walker's weld into the store. Join now welds
  at `KiwiRegion_WeldFor( finest edge )`, bit for bit what the region ring's own dedupe
  uses on the same points.
  **The store's `NormalizePoints` is deliberately NOT changed with it**: it runs on every
  `KiwiCon_Add` including a hand-drawn polyline, and its hard 0.5 must never eat a point
  the user placed. Join is different because the walker already ruled those two points
  are one.
- **A BOOLEAN DIFFERENCE COULD FAIL COMPLETELY AND SILENTLY BECAUSE SECTION 19 WAS
  APPLIED TO AN INTERMEDIATE** (item 2c — the mechanism behind "the pyramid is
  uncarved"). `KiwiSplit_DefByPlaneParts` gated BOTH halves of a split, and
  `CarveByOneTool` read a refused BACK half as `KBOOL_MISS` and abandoned the whole
  difference. But the back half is the **running intersection**, which `kiwi_boolean.cpp`
  discards at the end of the loop and never lands. Half of section 19's checks are about
  presentation, not volume (V3 clipped-away winding, V4 face under 0.1 sq units, V5
  coincident planes), and a running intersection against a many-planed tool — an
  extruded arch is 15+ near-tangent side planes — collects them routinely **with a
  healthy volume**. With ONE tool and ONE target that single verdict left `owned` false
  and the operation was a silent no-op. New `KiwiSplit_DefByPlaneCarve` hands a refused
  back half back instead of freeing it, gated on two conditions: every bounds extent
  >= `KSPLIT_CARRY_EXTENT` (1.0 world unit — anything thinner is round AO's genuine
  graze and still stops the cascade), AND its bounds inside the input's with 1 unit of
  slack (a half-space cut is a SUBSET, so this refuses V7/V8 — "not a bounded solid at
  all" — and admits only presentation verdicts on a real subset).
  **Nothing landed is relaxed**: the FRONT halves are gated exactly as before.

### Changed (behaviour)

- **THE RING SANITIZER IS ONE FUNCTION NOW** (item 2a). All three region passes already
  funnelled through `AcceptLoop`, so joined and unjoined outlines were already cleaned
  identically — the "the joined path skips DropCollinear" hypothesis is FALSE and is
  recorded here so it is not re-searched. What was missing was enforcement, and one
  step: `KiwiRegion_SanitizeRing` (exported) is dedupe -> DropCollinear -> **dedupe
  again**, the third pass being new and being exactly what a near-tangent arc/line
  junction leaves behind. `RewindCCW` stays in `AcceptLoop` — it is an acceptance
  decision, not a cleaning one.
- **A MIXED PASTE (solids + construction) DOES NOT AUTO-ENTER MOVE.** Both halves are
  pasted and both are selected; only the auto-enter is withheld, with a console line
  naming the two counts. One Move gesture structurally carries one kind:
  `KiwiConSel_CanMove()` requires an empty brush selection and Move reaches its
  construction arm only when `DominantKind` fails. Starting it would move the solids and
  leave the lines at the paste position.

### Instrumented

- **Every carried intermediate is announced.** `CarveByOneTool` prints the tool plane
  index and the section-19 gate that refused the running intersection ("carried on,
  because that piece is an intermediate and is never landed"), capped at 8 lines per
  operation like round AQ's refusals, with a tally line on both the interactive and the
  by-def commit paths. **Pasting those console lines from a failing carve remains the
  fastest way to close this out** — between round AQ's per-refusal lines and these, the
  exact tool, piece, plane and gate are now all named.

### Known limitations / open

- **NOT RUNTIME-VERIFIED (this whole round).** No build was run; every change is reasoned
  from source and from cited definitions.
- **THE JOIN DIFFERENTIAL IS NOT PROVEN, ONLY NARROWED TO TWO PROVABLE DEFECTS.** No
  console output from the failing case has ever been seen. What is proven is that (b) is
  a real ring-level asymmetry between joined and unjoined outlines at any real grid, and
  that (c) is a real mechanism by which a single many-planed tool no-ops entirely while a
  decomposed one does not. Which of the two the pictures show is decided by the next
  console paste.
- **A CARRIED INTERMEDIATE CAN STILL PRODUCE AN IMPERFECT HOLE.** The carry means the
  cascade continues from a piece section 19 did not like; the pieces that get LANDED are
  still gated, so nothing invalid enters the map, but the hole's shape around that plane
  is whatever the intermediate was. It is announced per plane and in the tally.
- **THE CONSTRUCTION CLIPBOARD IS PROCESS-LOCAL.** Construction geometry does not travel
  between two running copies of the editor. Brushes still do, because their half is the
  untouched ported OS-clipboard path.
- **CUT (Ctrl+X) IS STILL BRUSH-ONLY.** `KiwiCmd_ClipCut` returns early when
  `selected_brushes` is empty, so Ctrl+X over a pure construction selection says
  "nothing is selected". Copy+Delete by hand is the workaround; wiring the construction
  half needs the delete arbitration (`KiwiConSel_OwnsDelete`) folded in and is a separate
  change.
- **`KiwiRegion_SanitizeRing` HAS NO CALLER OUTSIDE `AcceptLoop` TODAY.** It is exported
  because it is the published spelling of the standing region invariant — any future
  fourth source of loops must call it — not because something else already does.
- **PASTED OBJECTS LAND IN PLACE, WITH NO OFFSET.** Same as the brush paste, and the
  Move that follows is how they are placed. A paste that is never moved sits exactly on
  its source, which is only visible as a doubled selection count.

## Round AT

Three user items on top of ff04342 (round AR): the terrain-paint ring that stopped
rendering, the drawing plane (tilted in open space, floating on TOP), and the
loft's CURVE mode. Full write-up in RADIANT_UX_DESIGN.md section 71.

### Fixed (bugs)

- **THE CYAN TERRAIN-PAINT RING REFUSED EVERY PATCH A MAPPER MAKES** (item 1,
  verbatim: *"the cyan circle for the advanced patch editor no longer renders."*).
  Round AQ's eligibility gate was `(int)def->type == PATCH_TERRAIN`, and
  `PATCH_TERRAIN` (0x40) is **a creation-path marker, not a shape**: only
  `Patch_ParseMesh`'s `"mesh"`/`patchTerrainDef3` branch (`pmesh.cpp:1002`),
  `Create_Terrain` from the Terrain dialog (`pmesh.cpp:1810`) and the curve->terrain
  conversion (`pmesh.cpp:9311`) set it. `Patch_GenericMesh`'s "simple patch mesh"
  sets `type = 0` (`pmesh.cpp:1702`) and so does every patch KIWI's own verbs
  build, so an ordinary flat sheet failed the test and the whole overlay went
  dark. The gate now asks what the overlay actually needs: control dims in [2,16]
  (`PMESH_20_Radius_2` walks `ctrl[col][row]` directly), a tessellated `curveDef`
  of at least 2x2 to clip the rings against, and NOT one of the CLOSED families
  (cylinder / cone / hemisphere), which keep AQ's refusal and its one console line
  per run. BEVEL and ENDCAP are no longer refused — they are open sheets (a
  quarter and a half turn), and KIWI's own swept surfaces stamp `PATCH_BEVEL` on
  themselves (loft CURVE strips, the patch fillet arc), so refusing that bit would
  refuse geometry the user had just built. **The crash fix is untouched** — the typed, in-range
  `ctrl[col][row]` access in `PMESH_19_Radius` (`pmesh.cpp:5068-5075`) is what
  stops the AV, and it is what allows the eligibility test to be about meaning.
  Recorded because it was checked: the paint stroke itself (`sub_43E4B0` ->
  `PMESH_16`) tests only `if ( i->patch )` and edits ANY patch, so the ring was
  strictly narrower than the verb it draws.
- **A STALE FACE SELECTION, OR A DISTANT GRAZING FACE, CAPTURED A SKETCH DRAWN IN
  OPEN SPACE** (item 2a, verbatim: *"The lines should just hit the nearest major
  plane? Why dont they? … The construction plane here should be flat."*). Rung 2
  of the working-plane ladder fires on SELECTION STATE ALONE — a face picked
  minutes ago still owned the plane with the cursor thousands of units away — and
  rung 3 accepts any face the ray reaches, at any distance and any grazing angle.
  Rung 4, the axis-aligned major plane, is below both and was therefore never
  reached. Both rungs now require the face to FACE the ray (`|n . raydir| >=`
  `KCON_PLANE_FACING_MIN`, cos 85 degrees — past that a pixel of cursor travel is
  tens of world units), and rung 2 additionally requires the cursor ray to meet the
  face's plane within the face's own bounds grown by one face-extent
  (`KCON_FACE_PLANE_SLACK`, floor `KCON_FACE_PLANE_MINGROW` 16 units). A refusal is
  a fall-through, so the ladder reaches rung 4 and the plane comes out flat. With
  no cursor ray at all (a tool started from a menu) round U's answer is unchanged.
- **A MAJOR PLANE INHERITED AN ARBITRARY OFF-GRID OFFSET FROM WHATEVER TOUCHED THE
  PLANE LAST** (item 2b, verbatim: *"when I lock the camera to TOP … IT draws them
  slightly above it??"*). The Z cycle, rung 4 and rung 0 all built the axis plane
  with `origin = the previous plane's origin, verbatim`. That origin comes from
  rung 3's RAW `pick.point`, rung 2's winding centroid, rung 1's object origin or
  a placed point — none of them a height the user chose, and none of them
  quantised (`KiwiCon_SnapUV` deliberately never snaps the normal component). Two
  rules now: an offset is **inherited only from a PARALLEL plane** (otherwise
  `origin[axis]` is the coordinate of a point on some other plane and the major
  plane is the world's own, 0), and it is **grid-quantised**. TOP over empty space
  is therefore z = 0 exactly, unless the user was already working at a height on
  that same plane. Round R fixed this same poisoning for the snap LATTICE; this is
  the plane's own offset, the other half of it.
- **RUNG 0's CONDITIONAL RUNG-1 PROBE LEFT ITS PLANE INSTALLED AFTER LOSING**
  (item 2b, same root). `KiwiCon_PlaneFromCursorConstruction` writes the active
  plane before the "is it parallel to the view plane" test can reject it, so a
  non-parallel construction object under the cursor still moved the working height
  and the fall-through then read ITS origin. The plane is snapshotted and restored
  on the rejecting path.
- **LOFT / CURVE PRODUCED A POLYLINE OF FLAT QUADS, NOT A CURVE** (item 3a,
  verbatim: *"it does not curve. It only acts like a brush."*). The control grid
  and the handle solve were both correct; the TANGENTS were not. `SpanTangent`
  clamped to a ONE-SIDED difference at the two end stations, which is *the span's
  own chord* — so `ArcHandle` intersected a line lying on the chord, the handle
  landed on the chord, and the quadratic span degenerated to a straight segment.
  At the default density (`KLOFT_DEF_SEGS_CURVE` = 2) **every** span is an end
  span, so the whole strip was two flat facets meeting at an angle. The end
  tangents are now the source faces' own plane normals — `outA` / `-outB`, which
  `BuildStations` already computed for the Hermite path and discarded as locals.

### Added

- **CONTINUITY PER END, CURVE MODE ONLY** (item 3b). Two panel rows, START and END,
  each G0 / G1 / G2, persisted in the profile ini `[KiwiLoft]`.
  * **G0** — the end span leaves along its CHORD (the pre-AT shape, kept and named:
    it is a deliberate crease, and it is exact for a straight run).
  * **G1** — the end span leaves along the SOURCE FACE'S NORMAL. Exact, and the
    DEFAULT.
  * **G2** — **an arc fit, and it is named that on purpose**: the handle is placed
    where a CIRCLE would put its tangent intersection,
    `chord / (2*cos(angle(end tangent, chord)))` along the end tangent — the
    generalisation of `kiwi_patchfillet.cpp`'s `r / cos(alpha/2)` (`:217-234`), to
    which it reduces exactly for a quarter turn. See the limits section for what it
    is not.
- **TWO-SIDED CURVE STRIPS** (item 3c, verbatim: *"have an option for 2-sided faces
  (Texture on BOTH sides). Currently in some scenarios it only shows 1 side"*). A
  panel toggle, persisted, DEFAULT OFF. Each strip is written twice through one
  helper, the second copy with its ROW ORDER MIRRORED — which reverses
  `cross(dCol, dRow)` and therefore the facing, the same thing the ported
  `patchInvert2` (`pmesh.cpp:2232`, Curve->Negative) does to a selection and the
  same build-time swap `kiwi_patchfillet.cpp:1437-1443` performs for its `rowFlip`.
  Both copies land in the one undo bracket with identical material and CAP
  alignment.
- **THE CURVE PREVIEW IS THE CURVE** (item 3a). In CURVE mode each rail span is
  drawn as the quadratic Bezier it will become (`KLOFT_CURVE_PREVIEW` = 6 segments
  per span), through the same `SpanTangent` / `ArcHandle` the commit uses, so a
  continuity change is visible before Enter. The straight rails it replaces are
  exactly what hid this round's bug.
- **A WORKING-PLANE CHIP** (item 2b). While a plane-placing tool is live the
  command chip strip shows `Plane  XY 128` (in §17 display units) or
  `Plane  tilted` — and nothing at all on the world's own major plane, the same
  "only in the non-default state" rule the `Grid / Snap OFF` chip beside it
  follows.

### Changed (behaviour)

- **`KiwiCon_SetPlaneAxis` (the Z cycle and the view-cube face click) now follows
  the same parallel-inheritance rule.** Clicking the cube's TOP face while a wall
  plane is active gives XY at 0, not XY at the wall's centroid height. Its
  documented intent — "XY after a placement at z = 128 means the plane THROUGH that
  placement" — is preserved exactly, because a placement leaves a plane parallel to
  the one it was placed on.
- **The loft HUD and the CURVE creation line name the continuity and the two-sided
  state**, and each option change prints one line saying what that setting does.

### Known limitations / open

- **NOT RUNTIME-VERIFIED (this whole round).** No build was run; every change is
  reasoned from source and from cited definitions.
- **G2 IS AN ARC FIT, NOT CURVATURE CONTINUITY, AND CANNOT BE MORE IN THIS
  FORMAT.** A patch span here is a QUADRATIC Bezier: three columns, one interior
  control point, therefore exactly one second-derivative vector. Once the tangent
  is matched there is no freedom left to match a curvature — true G2 needs a higher
  degree than the format offers. What G2 delivers is a span of CONSTANT curvature
  (a circular arc) leaving along the face normal, which makes equal spans agree on
  curvature at the station between them. It is explicitly **not** a curvature match
  against the source face: that face is a plane, curvature zero, and matching it
  would mean leaving dead straight.
- **A G2 END WHOSE GEOMETRY CANNOT SUPPORT AN ARC SILENTLY BEHAVES AS G1.** If the
  end tangent does not lean toward the far end by `KLOFT_ARC_MIN_COS` the arc fit
  is refused and the ordinary tangent intersection is used (and its own chord-mid
  fallback below that). Nothing is printed per span; the alternative was a shape
  nobody asked for.
- **WITH ONE SPAN AND BOTH ENDS G2 THE START WINS.** One quadratic cannot be two
  different circles. Raise the density to give each end its own span.
- **TWO-SIDED DOUBLES THE PATCH COUNT**, and patches still carry no collision at
  all — the caulk hint prints on every CURVE creation. A two-sided wall is two
  render surfaces, not a solid.
- **THE CURVE PREVIEW'S TRANSLUCENT SLEEVE IS STILL STRAIGHT.** Only the RAILS are
  evaluated as beziers; `FillSleeve` still shades between consecutive station rings
  as flat sleeves, so the fill under a strongly curved span reads slightly inside
  the true surface. The rails are what the shape is read from.
- **THE FACING TEST CAN REFUSE A FACE THE USER MEANT.** Selecting a face and then
  orbiting until it is within 5 degrees of edge-on, then starting a tool, now falls
  through to the major plane instead of using that face. That is the intended
  trade — the plane it would have handed back is one where a click cannot be placed
  meaningfully — but it is a behaviour change for anyone who was drawing on
  near-edge-on faces deliberately.
- **THE REACH TEST USES THE FACE'S AXIS-ALIGNED BOUNDS, NOT ITS WINDING.** A long
  diagonal face has a bounding box much larger than itself, so the slack is
  generous for that shape. A winding-accurate test would be an in-polygon check in
  plane space; the box is what the round-U tolerance was already stated as
  ("a drift of a face-width").
- **THE LOFT'S CURVE SETTINGS PERSIST; THE BRIDGE MODE AND DENSITY DO NOT.**
  `Reset()` still re-seeds the mode to RULED on every `Begin()`, so CURVE is still
  two presses of `D` away each time. Making the MODE sticky is a bigger call (it
  changes what `L` does by default) and was not made here.
- **THE TERRAIN RING NOW DRAWS ON NON-TERRAIN SHEETS, WHICH IS NEW SURFACE AREA.**
  `PMESH_19_Radius` is exercised on patches the retail editor never handed it
  (a subdivided type-0 sheet whose tessellated dims exceed the control grid). The
  guarded `turned_edge` read is what makes that safe, and every such quad takes the
  untorn diagonal — the same rule `Patch_Fill_BuildFrontIndices` (0x43FB70) applies
  when drawing it — so the ring clips against the triangulation the patch is drawn
  with. If a ring ever looks torn on a heavily subdivided sheet, that is the place
  to look.

## Round AU

One feature and one bind fix on top of a934e34 (round AT): a TrenchBroom-style
ENTITY BROWSER docked as a tab beside the texture browser, drag-and-drop placement
into the 3D view, and the N / Entity-inspector bind. Full write-up in
RADIANT_UX_DESIGN.md section 72.

### Added (features)

- **THE ENTITY BROWSER** (`kiwi_entbrowser.h/.cpp`, verbatim: *"a new panel that's
  in the same viewport (tabbed) with the textures tab. Make a whole new entity
  browser similar to the one in trenchbroom"*). A §9 dock window
  (`KIWI_WIN_ENTITIES`, **default OPEN**, `KiwiWindowEntities` in the palette,
  `Windows -> Entity Browser` in the native menu) docked into the SAME dock node as
  `Textures` — which is all it takes to make them tabs of each other. The list is
  `EclassList_Gather` (`win_ent.cpp:169`), i.e. `FillClassList` 0x496800's own
  `g_eclass` walk, so the browser and the Entity inspector's class list cannot
  disagree. Search box (the Textures panel's own `InputTextWithHint` idiom, round
  AE) plus an All / Point / Brush combo. **Grouped by the classname's text up to
  the first underscore**, `(other)` when there is none — KIWI's own rule, stated
  because the RMB eclass tree it was meant to mirror is a STUB in this shell
  (`xywnd.cpp:3175`, `(void)hwnd; // raw shell: no context menu until the HMENU
  rewrite lands`). Buckets are built in one pass in first-seen order, NOT by a
  contiguous-run walk: `g_eclass` is alphabetical so prefix-less classes are
  scattered through it and `(other)` would otherwise be emitted several times with
  the same ImGui id.
- **THE TILE IS A REAL ISOMETRIC PROJECTION OF THE ECLASS BBOX**, in the eclass's
  own colour: the eight corners of `[mins,maxs]` through `sx = (x-y)*cos30`,
  `sy = (x+y)*sin30 - z`, fitted to the tile, three filled faces at three
  brightnesses, twelve stroked edges — all `ImDrawList`, no texture and no render
  target. A brush class (no bbox — only the QUAKED size block sets `mins`/`maxs`)
  shows the wireframe unit cube plus a `B` badge; a class carrying `defaultmdl=`
  gets an `M` badge and its tooltip prints the model path. See "Known limitations"
  for why model thumbnails are not shipped.
- **DRAG A TILE INTO THE 3D VIEW TO PLACE IT** (verbatim: *"you can add them into
  the scene by dragging from the entity viewer to the 3d scene"*). The tile is an
  ImGui drag source carrying the classname; the CAMERA IMAGE is the drop target
  (`BeginDragDropTarget` immediately after `ImGui::Image`, which derives the target
  id from the item RECT when the item's id is 0 — `imgui.cpp:16032-16037`). The
  drop RECORDS the classname and the drop PIXEL and `PostMessage`s
  `KIWI_CMD_ENT_DROP` to the frame; the placement runs post-present, because
  `CreateEntityFromName` can raise a `MessageBoxA` and a nested Win32 pump inside
  the compositing scene bracket is the crash class the shell already moved context
  menus out of (the `kiwi_palette.cpp:151` deferral).
- **PLACEMENT LADDER**: the surface under the drop pixel (`Test_Ray`, called as
  `Cam_ContextMenu` calls it, `camwnd.cpp:3892`, with `Pick_CameraContents()`), then
  the ACTIVE WORKING PLANE (`KiwiCon_RayPlane` — a READ; a drop never moves the
  plane the user is drawing on), then 256 units down the ray. The bbox is centred on
  the hit across the surface and pushed OFF it along the dominant normal axis (rests
  on a floor, hangs under a ceiling), and grid-quantised through `KiwiGrid_Snap`.
- **THE CREATION IS THE PORTED PATH.** `CreateEntityFromClassname`'s own bracket
  (`xywnd.cpp:3374-3386`) verbatim — `Undo_ClearRedo` / `Undo_GeneralStart("create
  entity")` / placeholder brush / `CreateEntityFromName` / `Undo_End` — ONE undo
  record per drop, with `Select_Deselect(1)` BEFORE the bracket so the drop is
  create-only (round W's trap: `Entity_Create` MERGES into a selected non-world
  entity, `entity.cpp:1633-1666`) and so the record clones nothing that is about to
  be created (§23's ordering). The placeholder is built with the five calls
  `CreateEntityBrush` uses and landed with `KiwiExtrude_LandDef`. After the drop the
  new entity is selected (`CreateEntityFromName`'s own `Select_Deselect` +
  `Select_Brush`) and `KiwiCmd_AfterPaste()` puts it under a PAUSED move gizmo —
  the paste / clone precedent.
- **A LIVE MODAL COMMAND IS ARBITRATED.** An unmoved, auto-entered gesture
  (`PreemptIdle`) is cancelled and the drop proceeds; a gesture that has applied
  something refuses the drop with a console line naming it. The
  `KIWI_CMD_REMOVE_FACE` rule, applied to a new caller.

### Fixed (bugs)

- **THE ENTITY INSPECTOR NEVER SAW THE SELECTION — `edit_entity` WAS NEVER SET**
  (verbatim: *"make sure the legacy entity inspector (N) bind works so we can change
  their properties"*). The BINDING was fine and always was:
  `{ "ViewEntityInfo", 0x4E, 0, 33017 }` (`mainfrm.cpp:1140`) is in the default
  table, `kiwi_keymap.cpp`'s modern profile never touches vk 0x4E, and 0x4E is not
  in `res/radiant.rc`'s accelerator table. The BLOCKER was `UpdateSelection`
  (`win_ent.cpp:666`, IDB 0x497180), whose port guard `if ( !hwndEnt_entlist )
  return 1;` sits at line ONE — and `hwndEnt_entlist` is the MFC `CEntityWnd`'s
  eclass LISTBOX, initialised to `nullptr` at `win_ent.cpp:86` and **never assigned
  anywhere in this build**. So every selection change returned before the three
  lines that publish `edit_entity` / `multiple_edit_entities`, and every editor in
  the file opens with `if ( !edit_entity ) return;` (`EntSetKey_Apply :278`,
  `EntDeleteKey_Apply :358`, `SpawnFlags_Apply :476`, `EntAngle_Apply :908`). The
  panel printed "(no entity selected)" forever. **FIX**: those two globals are
  hoisted ABOVE the guard (plus a `world_entity` / sentinel NULL test, since the
  hoisted code now also runs during boot and `Map_Free` where the binary's version
  never did); everything below the guard — the `LB_*` calls that genuinely need a
  window — is untouched. The selection funnels were always correct
  (`UpdateSelection(-1,0)` from `Brush_Select_Helper`'s splice, `brush.cpp:947`, and
  from `Brush_RemoveFromList`, `:980`); they were calling a function that returned
  immediately.
- **THE INSPECTOR CLOSED THE MOMENT YOU CLICKED THE THING YOU WERE INSPECTING.**
  `ImGuiShell_CloseOnFocusLoss` (`imgui_shell.cpp:91`) closes a FLOATING panel ~130
  ms after focus leaves it, and clicking a brush in the 3D view takes the focus.
  Removed from the entity inspector. (It is also now docked by default, and the
  helper already exempts docked windows — but a user who tears the tab off must not
  lose the panel either.)
- **N WAS A BLIND BOOLEAN FLIP, WHICH BREAKS ONCE THE PANEL IS A TAB.** Pressing N
  while the inspector sat BEHIND the Textures tab closed a window the user could not
  see. N now opens + focuses (`ImGuiShell_FocusTab`, the same `SetWindowFocus` the
  O / texture-view keys use) and closes only when the panel IS the focused surface,
  which it answers from its own last draw.
- **A DRAG ACROSS THE 3D VIEW WAS BEING DISPATCHED AS VIEWPORT INPUT.** Round Y
  gave the camera image `ImGuiHoveredFlags_AllowWhenBlockedByActiveItem` so a
  focused text field could not swallow a click; a drag SOURCE holds `ActiveId` the
  same way, so carrying a payload over the viewport fed `CamWnd_OnMouseMove` (and a
  press, for a drag that began over the image). The camera's hover is now forced
  false while `ImGui::IsDragDropActive()`.

### Changed (behaviour)

- **The dock ini goes `kiwi_dock8.ini` -> `kiwi_dock9.ini` and `KW_VERSION` 5 -> 6.**
  Existing installs get the new layout (and the re-seeded window set) ONCE, and lose
  hand-made dock arrangements and per-window open/closed choices once — the same
  deal every layout default change in this project has made.
- **`Entity inspector` is pre-docked into the Textures node** even though it has no
  §9 flag: `DockBuilderDockWindow` records where a window belongs, so the frame it
  first appears it appears as a tab there instead of floating in the top-left corner.
- **With nothing selected the inspector now edits WORLDSPAWN and says so.** That is
  `UpdateSelection`'s own else-arm (the binary's rule), visible in this shell for the
  first time because `edit_entity` finally tracks the selection. The panel prints the
  classname, and prints "nothing selected — editing worldspawn" outright.
- **`KIWI_CMD_ENT_DROP` is excluded from Repeat Last (Shift+R)**, alongside the
  window toggles: it is the tail of a drag that has already happened and carries its
  argument in a one-shot slot, so repeating it would run a gesture with no payload
  AND would displace the modelling verb the user actually last ran.

### Known limitations / open

- **NOT RUNTIME-VERIFIED (this whole round).** No build was run; every change is
  reasoned from source and from cited definitions.
- **MODEL THUMBNAILS ARE NOT SHIPPED, AND THIS IS THE SCOPING NOTE.** A point
  eclass's box and colour are exact data (`eclass.cpp:816`/`:836`), but a MODEL is
  only a NAME — `eclass_t.cycleModelName[]` (`qe3.h:604`, from `defaultmdl=` at
  `eclass.cpp:915`) — resolved inside the scene walk (`brush.cpp:5295`) against the
  live editor render target. The RTT layer has exactly four fixed slots
  (`rttViewport_t` / `RTT_COUNT`), each one a dock window, so a per-tile thumbnail
  needs a fifth surface, its own camera and model registration, and its own place in
  `ImGuiShell_RenderViewportsToRT`'s per-tick budget — a new render path in the frame
  loop rounds V and AB were spent stabilising. Classes that WOULD gain from it are
  badged `M` and print their model path in the tooltip, so the gap is visible rather
  than silent.
- **A DROPPED `misc_model` / `misc_prefab` / `script_model` LANDS MODEL-LESS.**
  `CreateEntityFromName`'s arm for those classes calls `Ed_PostAddModelCommand`
  (`win_ent.cpp:873`), which posts to `g_qeglobals.d_hwndEntity` — the MFC entity
  window, which is NULL in this shell, so the post is a documented no-op and the
  model picker never appears. The entity and its bbox are created correctly; the
  `model` key has to be set by hand (the inspector's key/value grid) or through the
  Model panel. Not introduced by this round — it is the same for the eclass list's
  double-click create — but a drag-drop workflow makes it much more visible.
- **THE PLACEMENT USES THE DOMINANT NORMAL AXIS, NOT THE TRUE PLANE.** Dropping onto
  a sloped or angled face rests the box on the nearest AXIS of that face's normal, so
  on a steep slope the box can end up partly buried or slightly floating. A true
  plane-projected rest would need the box's support point along the normal, which is
  a different (and for a rotated entity, ill-defined) question.
- **A BRUSH-CLASS DROP IS A 64-UNIT CUBE.** A `func_*` / `trigger_*` class has no
  bbox at all, so there is nothing to size the box from; the drop lands a 64-unit
  cube for the user to reshape, which is the same thing the XY create menu does with
  an empty selection (its own placeholder is 32 in-plane with a template depth). The
  cube is not a user preference yet.
- **THE TILE GRID HAS NO CLIPPER.** Every visible group's tiles are submitted every
  frame (ImGui's own item clipping keeps the offscreen ones cheap, but the loop still
  runs). With a very large `.def` set and every group expanded this is more per-frame
  work than the outliner's clipper-driven list. It is bounded by the eclass count and
  was not measured; if a `.def` directory ever makes the panel sluggish, an
  `ImGuiListClipper` over rows-of-tiles is the fix, and it must not clip the tile the
  user is dragging FROM (ImGui expires a payload whose source stops being submitted).
- **THE GROUP KEY IS PURELY LEXICAL.** `light` and `lightJunior` land in different
  groups (`(other)` and `(other)` — both have no underscore, so this pair is fine),
  but e.g. `worldspawn` sits in `(other)` next to `origin`, and a `.def` set that
  uses a different naming convention will group oddly. There is no per-class group
  metadata in the QUAKED format to read instead.
- **THE INSPECTOR'S ECLASS LIST IS NOW REDUNDANT WITH THE BROWSER.**
  `imgui_panel_entity.cpp` still carries its own filtered eclass list with
  single-click-select / double-click-create, because it is the ported inspector's own
  surface and removing it would change what the panel IS. Two lists of the same data
  in two tabs is a wart; merging them is a UI call, not a bug fix, and was not made
  here.
- **`edit_entity` NOW FOLLOWS THE SELECTION EVERYWHERE, INCLUDING PATHS THAT NEVER
  HAD IT.** The hoist means every `Brush_Select_Helper` / `Brush_RemoveFromList` in
  the build — including bulk loops inside CSG, undo restore and map load — writes
  those two globals. They are plain assignments with no side effects, and the binary
  wrote them from the same call sites, but this is the one behavioural surface the
  hoist widens and is the place to look if the inspector ever shows a stale entity.
- **A MAP LOAD FREES ENTITY DEFS WITHOUT GOING THROUGH THE SELECTION FUNNELS**, so a
  cached `edit_entity` can outlive its def (`Map_Free` -> `Map_New`,
  `entity.cpp:1824`; neither clears it, and neither did before this round). The
  ImGui inspector is safe — it calls `Entity_UpdateSelection()` once per drawn frame
  and re-derives the pointer from `selected_brushes` + `world_entity`, both of which
  the load rebuilds. The OTHER reader is not covered: `mainfrm.cpp:3344` (the entity
  COLOUR picker) tests `edit_entity` for NULL and then walks `edit_entity->epairs`.
  That exposure is **pre-existing** — `EclassCreate_Apply` has always been able to
  leave a pointer there — but this round makes a non-NULL `edit_entity` the normal
  case rather than the rare one, so it is now much more reachable. The proper fix is
  one line in `Map_Free` / `Map_New`; it was not made because it is a ported file and
  the round's brief was one feature plus one bind.

## Round AV

Three entity-rendering items on top of 0f7192a (round AU). Items 1 and 2 turned out
to be ONE defect; item 3 is the model thumbnail round AU scoped out. Full write-up
in RADIANT_UX_DESIGN.md section 73.

### Fixed (bugs)

- **`View -> Entities as...` HAD NO READER FOR THE TWO BITS IT IS NAMED AFTER**
  (verbatim: *"there is a view -> entities as... -> option class that has no affect.
  The ents are always rendered as wireframe currently"*). The menu was fine end to
  end: items in `res/radiant.rc:121-128`, dispatch at `mainfrm.cpp:5542-5547`,
  handlers storing `g_PrefsDlg->m_nEntityShowState` at `mainfrm.cpp:3292-3303`,
  persistence as `"EntityShow"` (`prefs.cpp:284`/`:177-179`), default 65552 =
  `0x10010` = **Skinned** (`prefs.cpp:70`). So the field was never uninitialised —
  the editor booted into "Skinned" and drew wireframe. The five existing readers
  (`brush.cpp:5379`, `:6739`, `:7304`, `select.cpp:397`/`:636`) covered hide-all,
  selected-only, the BOXED bbox, the prefab-content technique and the pick chain;
  **none read `WIREFRAME` (0x1) or `SKIN_MODEL` (0x10) for the xmodel MESH.** What
  decided the mesh technique instead was a port invention in `camwnd.cpp`,
  `entTech = (ec->classtype & 0x18) ? worldTech : 29`. `classtype` bit 0x8 is set by
  NAME for four classes and 0x10 for one (`eclass.cpp:929-935`), so every other
  model-bearing class was hardwired to `TECHNIQUE_WIREFRAME_SHADED`.
- **THE CLASSES THIS HIT ARE ALL THE ONES WITH REAL MODELS AND NO SPECIAL NAME.**
  AI spawners are not in `cod4.def`; they are synthesized at load from
  `main/aitype/*.gsc` by `Load_Defs` -> `Init_ScanFiles(1)` (`eclass.cpp:1676`),
  each carrying its own QUAKED block plus `defaultmdl="body_complete_sp_sas_ct_..."`.
  Their `classtype` is `0x2` and nothing else. Same for the `weapon_*` classes
  synthesized by `Init_ScanWeapons` (`eclass.cpp:1645`). A `misc_model` was skinning
  correctly all along, which is why this survived the model-render epic — the test
  content had the right classname.
- **THE BLUE WAS THE TEAM COLOUR, AND IT WAS THE SAME DEFECT** (verbatim: *"they
  turn blue when not selected"*, then *"the blue tint is probably a team thing,
  might be normal"* — correct). `SkinModelInst` takes a per-vertex colour override
  **only at technique 29**: `meshTech != 29 ? nullptr : (const int *)col`
  (`camwnd.cpp:1549`, the binary's `DrawModels` call at 0x479735), and stamps it over
  every skinned vertex (`r_ed_scene.cpp:490-497`). The colour is
  `Cam_BrushColor2d`'s (`camwnd.cpp:1890`) = the eclass colour = the QUAKED triple.
  Across `main/aitype/*.gsc` there are exactly three: `(0.0 0.25 1.0)` allies,
  `(1.0 0.25 0.0)` axis, `(0.5 0.5 0.5)` neutral — and `(.3 .3 1)` for weapons. So
  the blue was the editor reporting team, through the binary's own mechanism.
  **No colour constant was edited.** Giving the mesh the technique the show state
  asks for makes the `colorPtr` arm unreachable (`colorOverride = -1`) and the model
  draws textured; picking *Entities as -> Wireframe* brings the team colours back
  deliberately.
- **NOT A `MATERIAL_COLOR` LEAK — CHECKED, NOT ASSUMED.** The round-AM
  state-destructive line batcher was the other suspect and is not involved: the
  entity flush pushes the binary's neutral `{0,0,0,0}` first (`camwnd.cpp:2820-2823`)
  and the editor's lit family (`vertcol_shaded` 24/25/29) LERPs toward
  `MATERIAL_COLOR` by `.w`, so `w == 0` is a no-op whatever `.rgb` holds. The tint
  survived it because it was baked into VERTEX colours.
- **THE FIX RESTORES THE BINARY'S SECOND TECHNIQUE PARAMETER.** `DrawBrush`
  (0x47afc0) takes `draw_meth1` (geometry / prefab contents / placeholder bbox) and
  `draw_meth2` (the xmodel MESH); the port collapsed them into one `technique` and
  then needed the `classtype` heuristic to stand in for the missing one.
  `g_drawBrushMeshTech` (`brush.cpp:6535`) publishes `draw_meth2` the way
  `g_drawAnglesBrush` publishes `DrawAngles`' ECX argument, `-1` meaning "same as
  `draw_meth1`". `DrawBrush` reads it at `brush.cpp:7287-7291`;
  `Cam_EntityMeshTech` (`camwnd.cpp:578`) is the six-mode table; it is armed around
  the ACTIVE (`camwnd.cpp:2766`) and SELECTED (`:2923`) camera entity loops and
  **reset to -1 immediately after each** (`:2804`, `:2950`), because three other
  callers reach the same `DrawBrush` and genuinely want 29 — both 2D-view model
  passes (`xywnd.cpp:4055`, `:4156`) and the white selected-outline pass
  (`camwnd.cpp:2985`).
- **"SKINNED AND BOXED" HAD NO BOX.** `0x1000` (BOXED) is read in exactly one place,
  `DrawOriginBox`'s third arm (`brush.cpp:6739`), reachable only through
  `DrawModels_Decorations` — which is behind `Radiant_DecorEnabled()`, default OFF.
  A model that draws RETURNS before the placeholder bbox, so the "and Boxed" half was
  inert. The decoration tail now runs when
  `Radiant_DecorEnabled() || (m_nEntityShowState & 0x1000)`, which is what the binary
  does unconditionally from `DrawModels` (0x4796f0).
- **THE SUBMENU OPENED WITH NOTHING TICKED.** `Radiant_SetEntityCheck`
  (`mainfrm.cpp:3275`, the binary's `SetEntityCheck` 0x42B1F0) was only ever called
  BY the six setters, so the persisted mode was invisible until you changed it. Now
  seeded at boot beside the grid and light-preview seeds (`radiant_main.cpp:657`).

### Added (features)

- **REAL 3D MODEL THUMBNAILS IN THE ENTITY BROWSER** (`kiwi_entthumb.h/.cpp`,
  verbatim: *"I would really like 3d previews in the entities viewer, try to get
  that to work."*). Round AU's four objections each got an answer rather than a
  workaround:
  - **A FIFTH RENDER TARGET THAT IS NOT A FIFTH VIEWPORT.** `RTT_BeginThumb` /
    `RTT_EndThumb` / `RTT_ThumbSurface` (`radiant_rtt.h:36-60`,
    `radiant_rtt.cpp:186-210`) drive a standalone `rtSlot_t` through the same
    `EnsureSlot`/`FreeSlot` the viewports use. Deliberately NOT a new
    `rttViewport_t`: `RTT_COUNT` also sizes `imgui_shell.cpp`'s per-viewport INPUT
    arrays (`s_cellW`/`s_cellH`/`s_hovered`/`s_wheel`/`s_imgMin`), so a new
    enumerator would have enrolled a thumbnail surface in
    `ImGuiShell_DispatchViewportInput`. 128x128 A8R8G8B8 `D3DPOOL_DEFAULT`, one mip,
    no depth-stencil of its own (it borrows the engine's shared, strictly larger one
    exactly as the four viewports do).
  - **THE MODEL IS REGISTERED WITHOUT AN ENTITY.** `edMapGlobals.modelInst[]`
    (`r_ed_scene.cpp:237-252`) is `{quat, origin, scale, colorOverride, XModel*,
    inuse}` — no `entity_s`, no `brush_t`. `Entity_UpdateModelInst`
    (`entity.cpp:696-699`) is one `AddModelToModelInstBuff` call with an entity's
    fields dug out first. So the path is `R_RegisterModel` ->
    `AddModelToModelInstBuff` -> `SkinModelInst` -> `R_AddEditorSurfsCmd` ->
    `RemoveModelInstFromBuf`, and `SetupModelInst` / `Model_SetModel` / `Eclass_01`
    — the three entity-coupled helpers — are **not on it at all**. No ported
    internals are re-entered.
  - **ORTHOGRAPHIC CAMERA, ROUND M'S MATRIX VERBATIM.** `R_Ed_SetSceneParms`
    (`r_scene.h:320`) already consumes an ortho `GfxMatrix` from the 2D views every
    frame; ortho also frames a bbox exactly with no fit iteration and no
    perspective disagreement between a rifle and a helicopter. Same
    `0.99951171875` guard band and same symmetric-about-the-eye depth as
    `camwnd.cpp:291-302`, guarded by `R_Ed_ProjectionWouldBeValid`. Pitch 20 / yaw
    200 (past 180 so a CoD character, facing +X, shows its face).
  - **ONE THUMBNAIL PER TICK, IN THE VIEWPORT PASS.** `KiwiEntThumb_Tick()` runs at
    the END of `ImGuiShell_RenderViewportsToRT` (`imgui_shell.cpp:1003`) — inside
    that function, not beside its call site, so it inherits the `!s_primary`
    early-out and round AB's `RTT_DeviceHealthy()` gate. With a warm cache, or with
    the Entity Browser closed (a panel that does not draw never requests), it is a
    null-check and a return.
- **THE CACHE IS `RT -> SYSTEMMEM -> MANAGED`.** One shared RT is rendered into,
  then `GetRenderTargetData` into one reused `D3DPOOL_SYSTEMMEM` offscreen surface,
  then `memcpy` into a per-entry `D3DPOOL_MANAGED` texture. This adds **exactly one
  default-pool object to the build** instead of one per entry, and the CPU hop is
  where alpha is forced to `0xFF` — so the tiles need **no `ImDrawList` callback**
  (the viewport image needs an `ALPHABLENDENABLE`-off callback pair,
  `imgui_shell.cpp:786 + :654`, and a callback splits the draw list, which in a grid of
  tiles is the one thing to avoid).
- **DEVICE-RESET COVERAGE IS INHERITED, NOT RE-DECLARED.** The one default-pool
  object is a slot inside `radiant_rtt.cpp`, freed by the `FreeSlot` loop in
  `RTT_ReleaseForReset()` — which is **already registered in both release sites**:
  `r_init.cpp:4276` (the normal `R_ReleaseForShutdownOrReset` cascade) and
  `r_init.cpp:4524` (round AD's INVALIDCALL second-chance list). It qualifies for
  the second list under round AD's own rule (`r_init.cpp:4449-4457`, D-AD3: entries
  must be things that CAN come back between retries and must tolerate a double call)
  — a lazily recreated RT comes back on any tick and `FreeSlot` null-checks.
  Recreation is the RTT layer's existing lazy `EnsureSlot`-on-next-`Begin`, so there
  is no rebuild hook to add either. `RTT_ReleaseForReset` additionally calls
  `KiwiEntThumb_ReleaseForReset()`, dropping the MANAGED cache and the SYSTEMMEM
  surface — neither pool requires it, and it is done anyway so the whole feature has
  ONE teardown entry point and so no `ImTextureID` can outlive the
  `ImGuiShell_InvalidateDeviceObjects()` that runs immediately before
  (`r_init.cpp:4272`).
- **THE BRACKET CANNOT BE LEFT OPEN.** `RTT_End` does not restore the previous
  render target, so an unpaired `Begin` parks an app-owned default-pool surface as
  the active RT and the next `Reset` fails `D3DERR_INVALIDCALL` — the exact thing
  `kiwi_devicereset.cpp:143` reports. `RenderThumb` is written so every
  early-out is either BEFORE `RTT_BeginThumb` or AFTER `RTT_EndThumb`; there is no
  `return` between them at all. The model LOAD — the only call
  that can `Com_Error(ERR_DROP)` and `longjmp` — runs before the target is bound,
  inside `setjmp(g_radiantAssetLoadJmp)` + SEH (the pair
  `Editor_InstanceAndSkinModel` installs, `camwnd.cpp:1565`). The skin between
  `Begin` and `End` is `__try/__except` only, whose unwind lands in the same
  function.

### Changed (behaviour)

- **A TILE WITH A PREVIEW DRAWS THE MODEL INSTEAD OF THE ISOMETRIC BBOX, AND DROPS
  ITS `M` BADGE.** The bbox tile is not replaced, it is the FALLBACK, and it stays
  the answer for every class with no class-level model — on exactly the code path it
  has today (`kiwi_entbrowser.cpp:371-397`). The image is drawn SQUARE and CENTRED
  (64x64 inside the 84x64 box) rather than stretched, so models are not squashed.
- **MODELS NOW SKIN AT THE CAMERA TECHNIQUE BY DEFAULT.** With the show state at its
  default "Skinned", every model-bearing point entity that previously drew as a
  team-coloured wireframe now draws textured. This is the intended fix, but it is a
  visible change to what a populated map looks like in the 3D view.

### Known limitations / open

- **NOT RUNTIME-VERIFIED (this whole round).** No build was run; every change is
  reasoned from source and from cited definitions.
- **IF A MODEL MATERIAL LACKS THE CAMERA TECHNIQUE, ITS SURF IS DROPPED RATHER THAN
  DEMOTED.** `Editor_AddSurfCmd` queues a surf only when
  `techType >= 34 || material->techniqueSet->techniques[techType]`
  (`r_ed_scene.cpp:444`); there is no per-surf demotion to UNLIT the way
  `Cam_TechAvailable` demotes world faces (`camwnd.cpp:1004-1006`). Camera draw_mode
  is fixed at 1 -> `TECHNIQUE_UNLIT` (`camwnd.cpp:142`, nothing else writes it) and
  `misc_model`/`misc_prefab` have skinned at that technique since the model-render
  epic, so this should not bite — but if a particular character model comes out
  INVISIBLE rather than textured, this is the mechanism, and
  *View -> Entities as -> Wireframe* restores the previous look exactly (as does
  `RADIANT_MODELS=0` for the mesh entirely).
- **IN THE TWO "SELECTED ..." MODES, AN UNSELECTED `misc_model` DRAWS ITS
  PLACEHOLDER BBOX FILLED.** `Entity_HasRenderableModel` returns 0 for it, so
  `DrawBrush` falls through to `DrawGeo` with `draw_meth1` — which for a
  `classtype & 0x18` class is the camera technique, not 29. Pre-existing (it is the
  same path a failed model already took) and untouched by this round, but those two
  modes make it reachable on purpose for the first time.
- **THE THUMBNAIL IS PER-CLASS, SO `misc_model` / `script_model` / `misc_prefab` DO
  NOT GET ONE.** Those carry no class-level model — their model is a per-ENTITY
  `model` key — so there is nothing for a per-CLASS tile to preview. `CLASS_PREFAB`
  is excluded explicitly: `Eclass_LoadModel` takes the `Prefab_Load` branch for it
  (`eclass.cpp:1009`) and yields entities, not an `XModel`. Those tiles keep the
  isometric bbox, correctly.
- **WARM-UP IS ONE TILE PER FRAME, AND SO IS RE-WARM AFTER AN ALT-TAB.** Scrolling
  a large group fills in over a second or so, and because
  `KiwiEntThumb_ReleaseForReset` drops the cache on every device reset, the visible
  tiles re-render after each alt-tab. That cost was accepted for the
  single-teardown-function property; if it ever annoys, the MANAGED textures are
  spec-guaranteed to survive a `Reset` and the call can simply be removed.
- **A FAILED MODEL IS NEVER RETRIED.** Deliberate — a per-frame retry on a missing
  asset is a per-frame asset load. But it also means a model that fails once because
  of a transient condition OTHER than device loss (device loss is explicitly
  excluded and left uncached) stays badge-only for the session. There is no
  "refresh thumbnails" command yet.
- **THE THUMBNAIL IGNORES `View -> Entities as...` ON PURPOSE.** It always renders
  skinned. That menu is about what the VIEWPORT shows, and a browser tile that went
  wireframe because of a viewport preference would just be a worse bbox.
- **THE THUMBNAIL SCENE HAS NO LIGHTING RIG.** It is the same `TECHNIQUE_UNLIT`
  fullbright the camera uses at its default draw_mode, against a flat neutral
  background — no fake-light direction is set up for it. Models read clearly but
  flat; a `FAKELIGHT_NORMAL` pass with a fixed key direction would look better and
  was not attempted this round.
- **NO CLIPPER STILL** (carried over from round AU). Every visible group's tiles are
  submitted every frame. Thumbnails do not change that — `KiwiEntThumb_Get` is a
  `std::map` lookup per tile, which is more than the zero it was but far less than
  the `ImDrawList` box it replaces.

## Round AW

Two residuals on top of 3897e6f (round AV). Item 1 is a one-command render-state bug
with a decode-backed proof; item 2 turned out to be the shipped assets, and the work
there is the readout that says so. Full write-up in RADIANT_UX_DESIGN.md section 74.

### Fixed (bugs)

- **THE ENTITY-BROWSER THUMBNAILS DREW FLAT WHITE BECAUSE THEIR `MATERIAL_COLOR` WAS
  WHITE** (verbatim: *"the thumbnails render without textures"*). Round AV opened the
  thumbnail frame with `R_AddCmdSetMaterialColor({1,1,1,1})`, copied from
  `CamWnd_RenderToRT`'s frame opener (`camwnd.cpp:4569-4570`), and never reset it. The
  camera copies the same opener and then pushes the binary's NEUTRAL `{0,0,0,0}`
  immediately before its model flush (`camwnd.cpp:2827-2832`, the binary's
  `R_SetMaterialColor(NULL)` at 0x4080f7/0x408115). That reset is the whole difference.
- **AND IT IS FATAL BECAUSE THE MODEL "unlit" SHADER LERPS, DECODED END TO END.**
  `raw/materials/mtl_sas_ct_bodies_sp` -> techset `l_sm_r0c0n0s0`;
  `raw/techsets/l_sm_r0c0n0s0.techset` -> `"unlit": vcsh;`;
  `raw/techniques/vcsh.tech` -> `pixelShader 2.0 "vertcol_shaded_fog.hlsl"`;
  `raw/shader_bin/ps_2_0_68679706` CTAB declares `colorMapSampler`, `fogColor` and
  **`materialColor`**. That is the vertcol_shaded family documented at
  `camwnd.cpp:2821-2826`:
  `result.rgb = lerp( sample(colorMap)*vColor, materialColor.rgb, materialColor.w )`.
  `.w` is a FLAT-COLOUR OVERRIDE FACTOR, so `w == 1` replaces the texture outright.
  The camera's `{0,0,0,0}` is a no-op and models come out textured; the thumbnail's
  `{1,1,1,1}` painted every tile white. Cross-check that closes it: if the shader
  MULTIPLIED by `materialColor`, the camera's zeros would render models BLACK — they
  do not, and the CTAB rules out the value being ignored, so lerp is the only combine
  consistent with both. FIX: the thumbnail frame now OPENS neutral
  (`kiwi_entthumb.cpp` `RenderThumb`), which is smaller than a reset and leaves no
  window for a future addition to inherit a white override.
- **IT WAS NOT AN ASSET-REALIZATION PROBLEM, AND THAT WAS RULED OUT FROM SOURCE.** The
  attractive hypothesis was that the thumbnail renders and CACHES a single frame taken
  before the model's images were resident, while the camera re-renders every frame. It
  is wrong: image creation is EAGER and synchronous inside material load
  (`Material_RegisterHandle` -> `Material_Load` -> `Material_LoadRaw` ->
  `Image_Register` -> ... -> `Image_Create2DTexture_PC` -> `dx.device->CreateTexture`;
  `r_material.cpp:560-573`, `r_material_load_obj.cpp:5929`, `r_image_load_obj.cpp:593`,
  `r_image.cpp:1394-1421`/`:1345`) and `R_RegisterModel` -> `XModelPrecache` registers
  every surface material on the spot (`r_model.cpp:151-157`,
  `xmodel_load_obj.cpp:1323-1336`). The deferred `delayLoadPixels` / `R_DelayLoadImage`
  path is fast-file only (`db_file_load.cpp:188`/`:192`), and there is NO streaming, NO
  per-frame image sync and nothing that happens on first reference anywhere in the
  build. Thumbnail and camera share the same `GfxImage` with the same live
  `texture.basemap`.
- **THE THUMBNAIL PASS ALSO HAD NO OPENING `R_SortMaterials`.** `Cam_Draw` calls it
  before EACH accumulation (0x407ab9 world -> `camwnd.cpp:2603`; 0x407fbf tint ->
  `:2919`; 0x4084f0 white); round AV's thumbnail had only the trailing one. It
  advances `sceneSurfCount_saved` so `R_AddEditorSurfsCmd` carries exactly this pass's
  surfs (`r_ed_scene.cpp:1079`), and it runs `Material_Sort`, the ONLY writer of
  `info.drawSurf.fields.primarySortKey` (`r_material_load_obj.cpp:6792-6795`) — which
  `Editor_AddSurfCmd` reads as the surf sort key (`r_ed_scene.cpp:453`). The thumbnail
  is the one caller that may have loaded these materials microseconds earlier in its
  own `RegisterGuarded`; `Material_Add` sets `rgp.needSortMaterials`
  (`r_material.cpp:537`) and `R_BeginFrame`'s own sort is gated on `rgp.world`
  (`r_rendercmds.cpp:1284`), which Radiant never has. Added.
- **A MODEL SURF WHOSE MATERIAL LACKS THE REQUESTED TECHNIQUE IS NOW DEMOTED, NOT
  DROPPED.** Round AV's own known limit (`Editor_AddSurfCmd`'s gate at
  `r_ed_scene.cpp:444`, the binary's 0x4fdff3) made such a surf INVISIBLE, with no
  equivalent of `Cam_TechAvailable`'s world-face demotion (`camwnd.cpp:1004-1006`).
  `Editor_ModelSurfTech` now walks `requested -> TECHNIQUE_UNLIT (4) ->
  TECHNIQUE_WIREFRAME_SHADED (29) -> leave it and let the gate drop it`, testing each
  rung with the gate's OWN predicate so it can never pick one the gate then drops.
  Both rungs are techniques the editor already draws models at (`camwnd.cpp:518`;
  `xywnd.cpp:4055`/`:4156`, `camwnd.cpp:2985`). **On this asset set it never fires** —
  every character techset carries `"unlit"` — so it is insurance, not a repair.

### Not bugs (decoded, with the evidence)

- **THE SOLID-WHITE CHARACTER IS AN AC130 THERMAL CHARACTER AND IT IS FAITHFUL.**
  Checking all 44 distinct `defaultmdl=` models in `main/aitype/*.gsc` against
  `raw/xmodel` / `raw/materials` / `raw/techsets` / `main/images`: exactly TWO —
  `body_complete_sp_sas_ct_ac130` and `body_complete_sp_spetsnaz_boris_sp_ac130` —
  carry materials outside the `l_sm_*` family. `mtl_sas_urban_ac130`,
  `mtl_sas_urban_head_ac130`, `mtl_spetsnaz_body_sp_ac130` and
  `mtl_spetsnaz_head_sp_ac130` use the techset literally named **`unlit`**, whose
  `"unlit"` technique is `vertcol_simple_fog_dtex` — pixel shader
  `vertcol_simple_fog`, the FLAT member of the family — they declare **only a
  colorMap** (every other character material declares colorMap + normalMap +
  specularMap), and that colorMap is the AC130 thermal texture. First DXT1 block read
  from the `.iwi`: `sas_ct_bodies_sp_col_ac130` 512x256 -> `(255,242,255)`;
  `sas_ct_head_sp_col_ac130` 128x128 -> `(255,238,255)`; `spetsnaz_body_col_sp_ac130`
  2048x1024 -> `(197,182,197)`. A near-white flat ghost is what that asset IS.
- **THE CHECKERBOARD CANNOT BE A MISSING MATERIAL IN THIS INSTALL.** On the model path
  the checkerboard has exactly one source: `Material_Register_LoadObj` fails to find
  the asset and `Material_MakeDefault` (`r_material.cpp:566-569` -> `:520-531`) returns
  a clone of `$default`, whose colorMap is the 16x16 `main/images/default.iwi`. It is
  NOT round Y's `$default3d` substitution — `Cam_DrawMaterial` (`camwnd.cpp:1030-1037`)
  is reachable only from the world-FACE emitters, never from a model surf. Scanning all
  **4372** `raw/xmodel` assets: **zero** referenced materials are absent from
  `raw/materials`. So that shirt pattern is a real shipped texture; the likeliest
  candidates are alpha layers drawn without their blend/alpha-test state —
  `mtl_usmc_ghillie_mesh_sp` (alpha-tested ghillie NET, techset `l_sm_t0c0n0s0`),
  `mtl_german_shepherd_fur` / `mtl_cobra_pilot_velinda_eyelash_sp` (`l_sm_b0c0*` blend
  cards). **UNCONFIRMED** — it needs the user to point at the model, which is what
  `KiwiModelInfo` is for.
- **TWO SPAWNER CLASSES NAME AN XMODEL THAT DOES NOT EXIST HERE.**
  `body_complete_sp_sonofzakhaev` and `character_us_cod3_axis` have no file in
  `raw/xmodel`, so those classes get no mesh at all (`XModelBad`) — a bbox, not a
  white body. Asset-set gap, not an editor defect.

### Added (features)

- **`KiwiModelInfo` — THE MODEL TWIN OF `KiwiMatInfo`** (`KIWI_CMD_MODELINFO` 34123,
  `kiwi_command.h`, registered unbound in `kiwi_command.cpp` and dispatched beside
  `KIWI_CMD_MATINFO`). `KiwiMatInfo` decodes the FACE under the cursor; a model's
  materials come from the xmodel asset and are never picked, so that question had no
  answer for models at all. Toggling this on makes every model surface print its
  material, techset, the technique it is ACTUALLY queued at, and its colorMap's name
  and dimensions. **A 1x1 or 16x16 colorMap in that readout IS a fallback image**
  (`$white` via `Image_AssignDefaultTexture`, `r_image.cpp:1049-1073`; or `$default`'s
  16x16 checker), so "missing asset" and "the asset looks like that" are one glance
  apart.
- **AN AUTOMATIC ONE-LINE REPORT, SILENT ON A HEALTHY MAP.** `Editor_ModelSurfTech`
  prints once per (model, material) pair, and ONLY on a demotion, a drop, or a
  `$default` clone: `KIWI model "X" surf N: material "Y" [techset "Z"] has no technique
  4 - drawing at 29 instead.` / `... IS THE $default FALLBACK (the asset was not found)
  - it draws as the 16x16 checkerboard.`

### Known limitations / open

- **NOT RUNTIME-VERIFIED (this whole round).** No build was run; every change is
  reasoned from source and from decoded shipped assets, with cites.
- **THE CHECKERED SHIRT IS NAMED BY HYPOTHESIS, NOT BY OBSERVATION.** The asset scan
  proves it is not a missing material and not the `$default3d` substitution; it does
  not prove WHICH material it is. Run `KiwiModelInfo` with that character in view and
  the surface table names it.
- **`KiwiModelInfo` IS A TOGGLE, NOT A ONE-SHOT, AND CANNOT BE ONE.** The only
  per-frame hook in `r_ed_scene.cpp` is `R_SortMaterials`' reset, and `Cam_Draw` calls
  that at the TOP of the frame (`camwnd.cpp:2603`) — BEFORE any model is skinned, so a
  frame-scoped one-shot would clear itself before printing. It self-limits by deduping
  every line per (model, material) pair instead; run it again to turn it off, and again
  after that to re-list from scratch.
- **THE DEDUP TABLE IS 256 PAIRS AND GOES QUIET WHEN FULL.** Both the automatic report
  and the dump share it, keyed on asset-name POINTERS (stable for the asset's lifetime,
  so identity compare is enough and nothing is copied). A scene with more than 256
  distinct (model, material) pairs stops reporting rather than growing.
- **THE DEMOTION LADDER HAS TWO RUNGS AND NO THIRD.** If a material carries neither
  technique 4 nor technique 29 the surf is still dropped, exactly as before — but now
  it says so on the console instead of vanishing silently.
- **THE LADDER SITS IN `SkinModelInst`, SO IT APPLIES TO EVERY MODEL CALLER, NOT JUST
  THE CAMERA.** The 2D views request 29 (`xywnd.cpp:4055`/`:4156`); a model material
  that lacked 29 was invisible there and would now demote to 4, i.e. draw TEXTURED in
  an XY window. No material in this asset set is in that position (every character
  techset carries both `"shaded wireframe"` and `"unlit"`), but on a different asset
  set it is a visible change, and `Entities as -> Wireframe` is not an escape from it
  the way it was in round AV.
- **THE THUMBNAIL STILL HAS NO LIGHTING RIG** (carried from round AV): flat
  `TECHNIQUE_UNLIT` against a neutral background. With `MATERIAL_COLOR` fixed the tiles
  are now TEXTURED and flat rather than white and flat; a `FAKELIGHT_NORMAL` pass with
  a fixed key direction would still look better.
- **THE AC130 SPAWNERS WILL STILL LOOK WRONG, BECAUSE THEY ARE RIGHT.** Their tiles and
  their 3D-view bodies are near-white by asset. Nothing in the editor should "fix" that;
  if it ever becomes annoying, the honest change is a browser-side note, not a render
  override.

## Round AX

Six reports on top of da4b775 (round AW). One is a reproducible hang that made the editor
unusable; the rest are entity-browser and visibility work. Full write-up in
RADIANT_UX_DESIGN.md section 75.

### Fixed (bugs)

- **THE EDITOR WENT BLACK FOREVER AFTER A DEVICE LOSS, BECAUSE ROUND AV'S THUMBNAIL
  RENDER TARGET WAS NEVER RELEASED** (verbatim: *"hit this while loading a new map. I can
  consistently reproduce this hang while loading kisak_trash.map. ... Unusable!"*).
  `RTT_ReleaseForReset` released the RTT slots with `for ( rtSlot_t &s : s_slots )`, and
  `s_slots` is the `RTT_COUNT`-element ARRAY. Round AV's thumbnail RT is `s_thumbSlot`, a
  STANDALONE slot declared beside that array (`radiant_rtt.cpp:26`) — deliberately not a
  fifth `rttViewport_t`, for reasons that are still correct. A standalone object is not in
  an array, so the loop could never reach it, and both `radiant_rtt.h` and the function's
  own comment nonetheless claimed it "joins the loop above". That texture is
  `D3DUSAGE_RENDERTARGET` + `D3DPOOL_DEFAULT` (`radiant_rtt.cpp:47-48`) plus its level-0
  surface, and `Reset()` returns `D3DERR_INVALIDCALL` while ANY app-created default-pool
  resource is alive — so **the first device loss after the first thumbnail ever rendered
  latched the failure for the rest of the session**. FIX: an explicit
  `FreeSlot( s_thumbSlot )`. No new hook was needed — both device-release sites already
  call `RTT_ReleaseForReset` (`r_init.cpp:4276` full pass, `:4524` INVALIDCALL
  second-chance pass) and the slot is recreated lazily by `EnsureSlot` on the next
  `RTT_BeginThumb`, exactly like the four viewport slots.
- **THE ROUND-AD ESCAPE HATCH WORKED EXACTLY AS DESIGNED, AND IS WORTH SAYING SO.** It
  named both HRESULTs, named the release pass, printed through `OutputDebugStringA` while
  the window was black, wrote the rescue save, and popped one message box after 15 s. The
  876 attempts in that box are the leak, not a defect in the reporting. Nothing about that
  machinery changed except that it now has more to say.
- **THE OTHER THUMBNAIL D3D OBJECTS WERE AUDITED AND ARE INNOCENT.** The readback surface
  is `D3DPOOL_SYSTEMMEM` (`kiwi_entthumb.cpp:186-188`), the per-entry cache textures are
  `D3DPOOL_MANAGED` (`:198-199`); by the D3D9 contract neither blocks a `Reset`. They are
  still dropped in the same pass, which is round AV's own decision and still right.
- **ENTITIES WERE NOT HIDABLE IN THE 3D VIEW** (verbatim: *"entities are not hidable via H,
  it does nothing even though the eyeball changes in the UI"*). The eyeball was telling the
  truth: H sets `selbrush_t.brushFlags` bit 2 (`Select_Hide`, `select.cpp:4179-4180`), the
  outliner reads the same bit (`kiwi_outliner.cpp:403-406`), and `KUNDO_VISIBILITY` covers
  entity-owned brushes because it walks both display lists (`kiwi_visibility.cpp:105-149`).
  What never asked was the CAMERA's entity passes. `FilterBrush` folds the hidden bit into
  `(brushFlags & 5)` at `filters.cpp:724` and gates the convex world pass, the round-AF
  patch pass, both 2D loops and every pick entry — but not the three entity loops in
  `camwnd.cpp`, nor `Cam_DrawOneLightPreview`, whose comment CLAIMED a `FilterBrush` gate
  while the code tested only bit 1 (the LAYER bit). FIX: `if ( FilterBrush( b, 0 ) )
  continue;` at all four sites. Hiding worked in XY the whole time, which is the fingerprint.
- **THE WHITE-OUTLINE PASS NEEDED THE GATE MOST.** It passes `drawFlags 1`, which is
  precisely what defeats `DrawBrush`'s own visibility test (`brush.cpp:7185`), and it draws
  after a depth clear — so a hidden selected entity was drawing a white wireframe THROUGH
  everything else.
- **THE THUMBNAIL LOAD QUEUE RAN IN LIST ORDER, NOT READING ORDER.** Round AV's comment
  said "first VISIBLE requester wins", but the browser has no `ImGuiListClipper` — it
  submits every tile of every OPEN group — so a class scrolled hundreds of rows out of view
  registered before the row under the cursor. `KiwiEntThumb_Get` now takes the tile's own
  `ImGui::IsRectVisible` answer and only a visible tile may enqueue a load.

### Changed (deliberate)

- **THUMBNAIL LOADS ARE NOW BUDGETED, AND THIS MAKES THE TOTAL FILL SLOWER ON PURPOSE**
  (request: *"It lags while loading the model previews (can you put it on a thread?)"*). At
  most one model registration per 66 ms, and additionally at least twice however long the
  previous one measured. Round AV rendered one per tick, so expensive characters produced
  back-to-back multi-frame stalls. The trade is explicit: a large group takes longer to
  fill, and the editor stays interactive while it does.
- **WEAPON-SHAPED MODELS ARE YAWED IN THE THUMBNAIL.** If one horizontal bbox axis is
  longer than 2x the other, the instance is yawed so that axis lands at world yaw 75
  degrees. The thumbnail camera is at yaw 200, so a model +X axis projects onto the screen
  horizontal with factor `|sin(0 - 20)| = 0.34` — near edge-on, which is the complaint; 75
  degrees gives `cos 35 = 0.82`, a 3/4 presentation. It is a geometric rule with no weapon
  list, and roughly-square models match neither arm and are unchanged. The ortho framing is
  fitted to the ROTATED corners, so the fit stays exact.
- **THE DRAG NOW PREVIEWS IN TWO PLACES.** The ImGui drag tooltip draws the tile's own
  visual (cached thumbnail, else the isometric bbox), and while the payload hovers the
  camera image a wireframe ghost box is drawn in the 3D view at the would-be placement
  point. The ghost runs the REAL placement ladder (`ResolveDropBox`: `Test_Ray`, then the
  working plane, then the fallback distance, then grid snap) read-only — it creates nothing
  and moves no working plane — so the preview and the drop cannot disagree.
- **THE DROP TARGET NOW ACCEPTS BEFORE DELIVERY.**
  `ImGuiDragDropFlags_AcceptBeforeDelivery | ImGuiDragDropFlags_AcceptNoDrawDefaultRect`.
  The delivery arm is unchanged; the preview arm is new and only reads. ImGui's default
  yellow accept rectangle is suppressed because it would frame the whole 3D viewport.
- **THE DEVICE-LOSS CHAIN NOW NAMES THE LOSS, NOT ONLY THE FAILED RECOVERY.**
  `KiwiDevice_NoteLoss` prints one line per loss episode naming which site armed
  `dx.deviceLost` and the deciding `TestCooperativeLevel` HRESULT; the loss origin is
  carried into the throttled health line and into the escape-hatch message box. On the
  FIRST `INVALIDCALL` of an episode the reporter also prints `RTT_DescribeLiveSlots` — the
  RTT slots still holding a default-pool texture, by name — plus the count of unmanaged
  default-pool images. Had that line existed, this round would have read `[thumb] (1)`.
- **AC130 THERMAL CLASSES GET A TOOLTIP NOTE** rather than a rendering change. A class
  whose resolved `defaultmdl` ends in `_ac130` shows "AC130 thermal model - near-white is
  the shipped asset". The test is on the MODEL name and not the class name deliberately:
  the naming convention holds across stock content but nothing enforces it.
- **A WHITE-TILE DETECTOR RIDES THE THUMBNAIL COPY-OUT.** A tile whose non-background
  pixels are 85% or more near-white is named once, with class, model and measured
  percentage, and the line states whether it is the known thermal case. Silent on a healthy
  grid.

### Known limitations / follow-ups

- **THE CAUSE OF THE DEVICE LOSS ITSELF IS NOT ESTABLISHED, ONLY THE UNRECOVERABILITY.**
  What is proven from source: the editor has exactly two writers of `dx.deviceLost`
  (`r_init.cpp:4699`, `:4810`) and both only arm it after `TestCooperativeLevel` has
  already answered negatively, so the editor observes a loss rather than manufacturing one;
  nothing in `Map_LoadFromFile` touches the device; and `D3DERR_DEVICENOTRESET` is
  self-sustaining once a `Reset()` has failed, so everything after attempt 1 is the leak.
  The map is small (127 brushes), which weakens the VRAM-pressure story for this map
  specifically. The working hypothesis — a transient driver-side loss during the texture
  churn of a map load, which is round AD's own report shape — is a hypothesis. The new
  one-line loss report settles it on the next occurrence: `D3DERR_DEVICELOST` or
  `D3DERR_DRIVERINTERNALERROR` means the driver, anything else points back at the editor.
  **If it recurs, screenshot the message box — it now names the loss site and the first
  HRESULT.**
- **SELECTING A HIDDEN BRUSH SILENTLY UN-HIDES IT.** `Brush_AddToList2` does
  `b->brushFlags &= ~0x1Fu;` (`brush.cpp:940`), which clears bit 2 along with the filter
  cache bits, while leaving `xx5` at 1 — so the two halves of the hide state desynchronise.
  The ordinary pick paths cannot reach a hidden brush (they all gate on `FilterBrush`), but
  the OUTLINER can select a hidden row directly, and that route will read as "the eye
  flipped back on its own". This is the ported binary's own behaviour and was NOT changed
  this round; it needs its own decision about whether to preserve the bit across
  `Brush_AddToList2` or to make the outliner's row-select refuse hidden rows.
- **THE GHOST TRAILS THE CURSOR BY ONE TICK (~16 ms).** The camera viewports are rendered
  to their RTs BEFORE the compositing ImGui frame that owns the drag
  (`radiant_main.cpp:942` vs `:945`), so the box drawn this frame was resolved last frame.
  Resolving it inside the draw instead would mean running `CamWnd_BuildMatrix` from inside
  `CamWnd_Draw`'s overlay tail, which is not worth it for a decoration.
- **THE GHOST IS ONE COLOUR AND NOT TRANSLUCENT.** `kiwi_lines` has no per-vertex alpha —
  alpha is a lerp weight there, not transparency (kiwi_lines.h TRAP 2) — so "ghost" is
  expressed as a brightened tint of the eclass colour. A translucent fill would need the
  `KiwiTris` layer and its backface rules.
- **THE THUMBNAIL BUDGET IS FIXED, NOT ADAPTIVE TO THE MACHINE.** 66 ms and a 2x cost
  factor are constants chosen against the observed stall shape, not measured against a
  frame-time target. If a future round wants a real frame budget it should measure the
  pump's actual tick cost rather than tune these two numbers.
- **THE THUMBNAIL STILL HAS NO LIGHTING RIG** (carried from rounds AV/AW): flat
  `TECHNIQUE_UNLIT` against a neutral background. The yaw makes long models read better;
  it does not add shading.
- **`actor_enemy_villain_sonofzakhaev` CAN NEVER RENDER A THUMBNAIL.** Its
  `defaultmdl=body_complete_sp_sonofzakhaev` has no xmodel anywhere in this asset tree
  (only its materials and a docs JPEG exist). It caches FAILED after one tick and keeps the
  bbox tile, which is the designed behaviour, but it is an asset gap and not an editor bug.
- **THE AC130 TILES ARE STILL WHITE, BECAUSE THEY ARE RIGHT.** Six of the 72
  `actor_enemy_*` classes are thermal variants and `Eclass_InsertAlphabetized` sorts them
  first, so they are the opening tiles of that group. If a white tile appears that is NOT
  one of those six, the new detector will have named it on the console with its model.

## Round AY

One user item on top of 20f2748 (round AX), verbatim: *"looks like the loft is still one
sided, see pics."* Screenshots of a curved wall built with loft CURVE mode: textured from
outside, white selection wireframe and nothing else from inside. Full write-up in
RADIANT_UX_DESIGN.md section 76.

### Verdict

**IT WAS THE DEFAULT (a), NOT THE MIRROR (b) AND NOT THE MATERIAL (c).** Round AT's
two-sided emit was shipped **default OFF** and this user had not turned it on. The
mechanism was re-verified before the default was touched and is correct at every link:
the panel row binds (`KOPT_TOGGLE`, option index 6, `kiwi_cmdoptions.cpp:201-210` renders
it and `:249-253` dispatches `OptionChanged`), the flag persists and re-seeds
(`LoftPrefs_Write` / `Reset`), it reaches the commit (`ApplyCurve`'s
`copies = m_twoSided ? 2 : 1`), and the mirrored copy is the binary's own inversion —
`WriteCurveStrip`'s `flip` writes `ctrl[col][height-1-row]`, which is exactly
`patchInvert2`'s swap (`pmesh.cpp:2232-2241`, the Curve → Negative menu item), so
`cross(dCol, dRow)` and therefore `Curve_ComputeNormals`' normal (`pmesh.cpp:502`) really
do reverse. Both copies also run `KiwiMtl_RealizePatch`, so the round-T unrealized-
`MaterialDef` failure (visCount 0 → `DrawPatches` emits nothing) cannot be it either.

### Fixed

- **A CURVED WALL CAME OUT WITH ONE SIDE BECAUSE THE OPTION THAT GIVES IT TWO WAS OFF
  UNTIL ASKED.** `s_twoSided`, the `[KiwiLoft] TwoSided` ini default and the member seed
  all flip to **ON** for CURVE mode. It stays a toggle — a backdrop or a one-room ceiling
  does not need two sheets and the patch count is real — but the answer to "make me a
  curved wall" is a wall, and this is the second time the same user has asked. A profile
  that never toggled the option has no key at all (`LoftPrefs_Write` is the only writer
  and only runs on a change), so existing users pick the new default up while anyone who
  deliberately turned it off keeps their `0`.
  **WHY ONE PATCH CANNOT BE ENOUGH, with the line:** `DrawPatches` (`pmesh.cpp:10233-
  10241`) emits the `PM_BACK_FACE` index list at `TECHNIQUE_WIREFRAME_SHADED` — never
  textured — and while a patch is SELECTED it skips that back pass entirely (the
  `drawFlags & 1` arm). The loft's fresh strips land on `selected_brushes` (that is what
  lets `Undo_EndBrushList` stamp them), so the instant after Enter the far side of the
  wall is white selection wireframe over nothing. That is the screenshot exactly.
- **CAP TEXTURE ALIGNMENT HAS NEVER ACTUALLY RUN ON A LOFT CURVE STRIP.** `ApplyCurve`
  called `Patch_KiwiCapAlign` **before** `AddBrushForPatch`, because this file copied
  `kiwi_patchfillet.cpp`'s landing sequence before round AS moved that call in it. CAP's
  reference face is a face of the patch's SYMBIONT brush and `AddBrushForPatch` is what
  creates it (`pmesh.cpp:869`), so round AS's defensive guard (`pmesh.cpp:2907-2912`) was
  taking the early-out on **every strip ever built**, printing `Patch_KiwiCapAlign: patch
  has no symbiont brush yet` once per strip and silently keeping
  `Patch_KiwiFinishNewLike`'s naturalize instead. Round AN's CAP directive was honoured by
  the fillet and ignored by the loft for four rounds. The call moves after
  `AddBrushForPatch`; the header comment that still described the pre-AS order is
  corrected with it.

### Changed (behaviour)

- **THE TWO-SIDED "off" CONSOLE LINE NAMES THE SYMPTOM.** Turning the toggle off now says
  each strip is emitted "once: the far side of the wall will show wireframe, not texture"
  instead of "once, visible from one side". Same fact, but the new wording is the thing
  the user would otherwise file a report about.
- **Loft CURVE strips are now CAP-aligned in fact as well as in the header**, which will
  change the texture lay on strips built by this command compared with 20f2748. It is the
  alignment round AN asked for; strips built before this round keep whatever they were
  saved with.

### Known limitations / open

- **NOT RUNTIME-VERIFIED (this whole round).** No build was run; every change is reasoned
  from source and from cited definitions.
- **THE TWO SHEETS ARE COINCIDENT AND THAT IS DELIBERATE.** Each is culled from the side
  the other draws from, so exactly one rasterises per view direction and there is nothing
  to z-fight — the same thing stock double-sided patches do. **Do not "fix" this with an
  offset**: it would open a seam at every silhouette edge. If a future report describes
  shimmering on a two-sided loft, the suspect is the *unselected* back-face
  `TECHNIQUE_WIREFRAME_SHADED` pass of one sheet landing on the other sheet's textured
  front at equal depth — a wireframe overlay, not a depth fight, and it is stock
  `DrawPatches` behaviour that would need a change there rather than in the loft.
- **TWO-SIDED STILL DOUBLES THE PATCH COUNT**, and patches still carry no collision at
  all — the caulk hint prints on every CURVE creation. A two-sided wall is two render
  surfaces, not a solid.
- **THE `[KiwiLoft] TwoSided` KEY IS NOW AMBIGUOUS FOR ONE CASE.** A user who toggled the
  option ON under round AT has `TwoSided=1` written and sees no change; a user who toggled
  it ON and then OFF has `TwoSided=0` and will still get one-sided strips despite the new
  default. That is the correct outcome (an explicit choice outranks a default) but it
  means "it is still one sided" from such a user is a stale ini, not a regression — check
  the panel checkbox first.
- **NOTHING WAS VERIFIED ABOUT THE MIRROR AT RUNTIME, only against `patchInvert2`.** The
  claim that a row-mirrored control grid reverses the drawn facing rests on
  `Curve_ComputeNormals` taking `cross(dCol, dRow)` (`pmesh.cpp:502`) and on
  `patchInvert2` being the editor's own working Curve → Negative primitive. If the strips
  come out two-sided but both facing the same way, that chain is where to look.

## Round AZ

Three user items on top of 730c051 (round AY), verbatim:

1. *"allow shift clicking of patch sides like they're faces, so can they be textured at
   the same time as the faces."*
2. *"when zooming out extremely far, it breaks the 3d camera completely."* (screenshot: an
   empty viewport, a sliver of grid along the bottom edge, no geometry at all)
3. *"We need a skybox feature. Invent a nice way to show a skybox and allow the camera to
   ignore it's there so we can fly inside of the skybox and see the sky. Make this a
   separate tab like the entity tab."*

Item 2 went first (a breakage report). Full write-up in RADIANT_UX_DESIGN.md section 77.

### Verdicts

**ITEM 2 IS THE ORTHOGRAPHIC FAR PLANE, AND THE ZOOM CEILING WAS EXACTLY TWICE IT.** KIWI's
3D camera is ORTHOGRAPHIC by default (round N), and the ortho arm builds a **finite,
eye-centred** depth slab: `zNear = -131072`, `zFar = +131072` (`camwnd.cpp:294-303`,
`KCAM_ORTHO_DEPTH` at `camwnd.cpp:168`). The perspective arm has an INFINITE far plane
(`m[2][2] = C`, `m[2][3] = 1`), so this only bites the default profile. The dolly holds the
pivot still and walks the eye backwards, so the framed geometry sits at view depth exactly
`s_dist` — and `KCAM_MAX_DIST` was **262144**, twice the far plane. At the ceiling the
depth budget behind the pivot is −131072: the whole map is beyond zFar and the projection
throws all of it out, leaving only what is within 131072 of the eye — for a camera looking
down from far above, the near corner of the ground grid. That is the screenshot exactly.
Nothing was culled and nothing went degenerate.

**ITEM 1'S CLICK PATH ALREADY WORKED END TO END; THE DRAG DID NOT.** Audited link by link
before anything was written, and three of four links were already correct: round AM's pick
resolves a patch in mode 3 as `SEL_OBJECT` (`kiwi_pick.cpp:641-669`); `ClickSelect`'s shift
arm has no kind test in it at all (`kiwi_boxselect.cpp:826-829`); and `Sel_SyncToLegacy`
pushes objects → `selected_brushes` **and** faces → `g_SelectedFaces` in one pass
(`kiwi_selection.cpp:349-394`), which the legacy model explicitly sanctions —
`Select_Brush`'s assert is `g_SelectedFaces.GetSize() == 0 || brush->patch`
(`select.cpp:903`). **THE LEGACY-SYNC VERDICT: `Brush_SetTexture` runs BOTH loops in ONE
undo bracket with no exclusivity early-out.** The face array only chooses the record's
NAME (`select.cpp:1802-1812`); the brush/patch pass at `:1866-1877` is unconditional and
outside the `if`, and both sit inside one `Undo_GeneralStart`/`Undo_End`
(`:1798`…`:1862`). A mixed selection already took one texture on both kinds, in one undo
record. What was broken is the MARQUEE and one un-fenced apply funnel.

**ITEM 3 HAD NO STARTING SURFACE AT ALL.** `grep -i skybox src/radiant/` returned zero
matches before this round.

### Fixed

- **ZOOMING OUT PAST THE ORTHO FAR PLANE EMPTIED THE VIEWPORT.** `KCAM_MAX_DIST` goes
  262144 → **65536**, DERIVED as `KCAM_ORTHO_DEPTH / 2` — "half the depth slab is reserved
  for whatever is behind what you framed", leaving 65536 units of budget behind the pivot.
  Cost computed, not eyeballed: at the default fov of 65 the half-height is
  `s_dist * tan(32.5°) * 0.75 = s_dist * 0.47780`, so the new ceiling still frames **62,636
  units vertically and 111,353 horizontally** (16:9) in one screen — just under four times
  a full ±16384 CoD4 map, top to bottom. It also clears the perspective arm's separate
  failure at the same ceiling: `camwnd.cpp:203-208` records the float32 `MatrixInverse44`
  losing the sign of the inverse-VP's `m[3][3]` on far-from-origin cameras (*"blackout at
  world X ~ -175000"*), at which point `R_Ed_ProjectionWouldBeValid` fails and
  `CamWnd_SetupScene` **drops whole frames**.
- **THE EYE WAS NEVER ACTUALLY BOUNDED BY THAT CEILING — IT RAN AWAY FOR EVER.** The
  dolly's clamps were on the recorded DISTANCE; the origin write above them was unguarded
  and `move` came from the separately-clamped `reference`. So once `s_dist` pinned at the
  ceiling, every further notch still walked the eye backwards by `reference * (k-1)` =
  `262144 * 0.1111` ≈ **29,127 units per notch, indefinitely**, while `s_dist` sat still
  and claimed nothing had happened. Twenty notches past the ceiling put the eye 580,000
  units outside a world that ends at 131,072. Three fixes: the dolly now saturates the
  **move** (`if ( move < 0.0f && s_dist - move > KCAM_MAX_DIST ) move = s_dist -
  KCAM_MAX_DIST;` — exactly 0 at the ceiling, so the wheel stops dead); `ClampToWorld`
  boxes `c->origin` into ±131072 (the engine world bound) from inside `Commit`, the single
  funnel every camera mutator already goes through, carrying the orbit pivot and look-at by
  the SAME correction so the pair stays rigid; and `KiwiCam_OrthoHalfHeight` gains its own
  `KCAM_ORTHO_HALF_MAX = 32768` ceiling, because `camera_fov` is a user pref that
  MULTIPLIES the distance and the distance clamp alone does not bound it.
  **ONLY THE ZOOM-OUT SIDE IS SATURATED**, deliberately: with zoom-to-cursor the eye
  travels along the cursor ray, not the view axis, so `s_dist -= move` is an approximation
  and clamping the move by it on the way IN would re-erect the shakeout-C wall (*"after a
  certain point, you can't zoom in further"*).
- **A PATCH JOINED A FACE SELECTION ON SHIFT-CLICK BUT NOT ON SHIFT-DRAG.**
  `CollectFromBrush`'s face/edge arm opened `if ( b->patch || !def->faces ) return;`, so
  the mode-3 marquee walked past every patch. Round AM's ruling was made for the click and
  never mirrored into the drag. The arm now routes a patch in a FACE marquee through the
  **existing OBJECT arm** (`const bool patchAsObject = ( kind == SEL_FACE ) && b->patch;`)
  rather than reimplementing containment — that arm carries round AF's crossing-only bbox
  widen (`kiwi_boxselect.cpp:339-368`, the fillet fix) and already knows `Contained()` and
  `Crossed()` mean different things. `SEL_EDGE` is deliberately not included: a patch has
  no windings, so there is no edge to name. The crossing centre-ray rescue needed nothing —
  it picks with `SEL_MASK_FACE`, which round AM already makes return the patch.
- **PICK TEXTURE (MMB) STRETCHED EVERY PATCH IT TOUCHED.** Round AK's
  `Patch_KiwiReNaturalizeSelected` fence was hung on the texture-browser funnel only.
  `PickTexture` (`kiwi_uv.cpp`) applies the picked material to the CURRENT SELECTION
  through the same `Texture_SetTexture` → `Brush_SetTexture` call — its own comment says so
  (`kiwi_uv.cpp:641-645`) — and had no fence, so `sub_476ED0`'s patch branch
  (`brush.cpp:2852-2866`) swapped the material pointers without re-laying
  `ctrl[][].texCoord` and the surface rescaled by `newWidth/oldWidth`. The fence is added
  there. This predates round AZ; item 1 is what made it visible, because a mixed selection
  picked onto would have textured the faces correctly and stretched the patches **in the
  same gesture**.

### Added

- **A "SKY" TAB, third tab of the Textures/Entities dock node** (`kiwi_skybox.h/.cpp`,
  `KIWI_WIN_SKY`, `KW_VERSION` 6→7, dock ini `kiwi_dock9` → `kiwi_dock10`, ids
  34124–34126). Lists every sky material with its **real colormap** as the tile image,
  search box, click to select, double-click to apply.
- **ONE SPELLING FOR "IS THIS SKY", AND IT IS THE DATA'S.** `KiwiSky_IsSkyMaterial` tests
  `SURF_SKY` (bit 2) in `qtexture_s::color_or_surfacetype_filter` (@0x1C, `qe3.h:65`) —
  the material's full surfaceFlags word, written from the header at both registration paths
  (`texwnd.cpp:298` / `:414`) and the same flag `CullCubic` folds
  (`camwnd.cpp:428-437`). The leaf-name `"sky"` substring survives **only as a fallback
  gated on the flags word being entirely zero**, i.e. only where the data said nothing. The
  tree had three disagreeing tests before this; the other two are widened in place, never
  replaced.
- **TWO VERBS.** *Apply to selection* routes through `TexWnd_ApplyMaterialAtIndex` — the
  identical funnel a thumbnail click takes — so it inherits the ported one-record undo
  bracket and round AK's fence rather than growing a third apply path. *Create skybox
  shell* builds six brushes enclosing the map bounds + margin in one
  `KiwiCmd_UndoBegin`/`Commit` bracket; the six boxes **tile without overlapping** (floor
  and roof take the full outer footprint, the X walls the full outer Y but inner Z, the Y
  walls inner X and inner Z), defaults margin 512 / thickness 64, both persisted, clamped
  inside ±131072.
- **VIEW-DEPENDENT SEE-THROUGH.** `KiwiSky_SeeThroughNow()` is true only while the toggle
  is on AND the camera is outside the union AABB of the map's sky brushes; when it fires,
  sky joins the see-through tool volumes down the existing `d_white` route (blended,
  depth-write-free, tinted by the sky-blue `MATERIAL_COLOR` already at `camwnd.cpp:601`).
  Fly inside and the sky goes fully textured on its own. The whole render change is one
  term in one expression at `camwnd.cpp:2726`.
- **"SKY PICKABLE" CHECKBOX** — the binary's own `g_PrefsDlg->sky_brush_off`
  (`prefs.h:89`, menu cmd 33169), shown inverted. The walker's material test
  (`select.cpp:708-727`) is widened from the `"sky_"` substring to `"sky_"` **OR** the
  predicate, so the pref finally means what its menu item says.

### Changed (behaviour)

- **THE MAXIMUM ZOOM-OUT IS NOW A QUARTER OF WHAT IT WAS** (262144 → 65536 units of orbit
  distance). Anyone who had genuinely parked the camera further out than 65536 will find
  the wheel stops sooner — but everything past ~131072 was rendering an empty viewport, so
  what is lost is a range that did not work.
- **SKY BRUSHES NO LONGER OCCLUDE THE MAP FROM OUTSIDE, BY DEFAULT.** `[KiwiSky]
  SeeThrough` defaults to 1. A map that has no sky brushes is completely unaffected (the
  union box is empty and the arm returns false); a map that does will look different from
  outside the shell than it did on 730c051. Uncheck "See-through from outside" in the Sky
  tab for the old behaviour.
- **A MODE-3 MARQUEE NOW COLLECTS PATCHES.** A shift-drag over a region containing curves
  will select more than it did on 730c051 — specifically, the curves, which is the point.
- **PATCHES RETEXTURED BY MMB PICK KEEP THEIR DENSITY** instead of rescaling. Patches
  textured that way before this round keep whatever they were saved with.
- **EVERY PROFILE RE-SEEDS ITS WINDOW SET ONCE** (`KW_VERSION` 6 → 7) and re-builds its
  dock layout once (`kiwi_dock10.ini`), which is the standing cost of adding a tab. A user
  who had deliberately closed a window loses that choice exactly once.

### Known limitations / open

- **NOT RUNTIME-VERIFIED (this whole round).** No build was run; every change is reasoned
  from source and from cited definitions. Item 2's arithmetic is checkable on paper; items
  1 and 3 are not.
- **`texwnd.cpp:1148-1161` CAN STILL TURN A MIXED SELECTION BRUSH-ONLY.** The apply's
  validation fence is ALL-OR-NOTHING: if any one entry in `g_SelectedFaces` is stale
  (version mismatch, out-of-range index, dangling `face` pointer) it does
  `g_SelectedFaces.m_nSize = 0` and drops **every** face. In a patch+faces selection that
  means the patch takes the texture and the faces silently do not — which is exactly the
  shape of the reported complaint. It was left alone deliberately (it is defensive
  hardening that was not diagnosed as firing, and per-entry removal is a behaviour change
  to a path this round could not test), but **if item 1 comes back as "the faces still
  didn't take it", this is the first place to look**, and the fix is to remove the bad
  entries rather than the array.
- **THE SURFACE INSPECTOR IS THE LAST UN-FENCED APPLY FUNNEL.** `surfacedlg.cpp:255`,
  `:626` and `:666` call `Brush_SetTexture` with no `Patch_KiwiReNaturalizeSelected`, so a
  patch retextured from the Surface Inspector still stretches. Not touched this round
  (three ported call sites vs. one KIWI one), and it is the third instance of round AK's
  fence being needed somewhere it was not written.
- **THE SEE-THROUGH TEST IS THE UNION BOX OF ALL SKY BRUSHES, NOT "THE SHELL".** A map with
  a sky brush somewhere far from the main shell (a light-well, a separate skylight) widens
  that box, and the camera can then be "inside" it while standing outside the real shell —
  in which case the sky stays textured and occludes. This is the accepted cost of a test
  that is six float compares; a per-shell answer would need connectivity analysis.
- **THE UNION BOX IS REFRESHED ON A 250 ms TIMER** (`KSKY_BOUNDS_MS`), so crossing the
  shell boundary can take up to a quarter second to change the treatment, and a sky brush
  created or deleted by something other than this file's own verbs is picked up on the same
  delay. `KiwiSky_InvalidateBounds()` is public for anything that wants it sooner.
- **"SEE THE SKY" MEANS THE COLORMAP ON THE FACES.** There is no sky dome and no
  `TECHNIQUE_SKY` anywhere in this tree — the engine's only sky symbol is
  `TEXTURE_SRC_CODE_SKY = 0xE` (`r_material.h:44`), a runtime code-image slot the editor
  never binds. From inside the shell you see the sky texture laid flat on brush faces,
  which is what stock Radiant shows.
- **A SKY COLORMAP WITH A REAL ALPHA CHANNEL WILL SHOW THE TILE BACKGROUND THROUGH IT.**
  The entity browser bakes alpha to 0xFF in its CPU copy; the Sky tab has no copy (it reads
  the engine's `IDirect3DTexture9` fresh every frame and owns nothing, which is what keeps
  it out of the device-reset path entirely). A per-tile draw callback would fix it and
  SPLIT THE DRAW LIST, which `kiwi_entbrowser.cpp:454-459` says not to do in a grid.
- **THE SHELL USES MAP BOUNDS, INCLUDING HIDDEN BRUSHES AND INCLUDING ANY EXISTING SKY.**
  Running "Create skybox shell" twice builds a second shell around the first. There is no
  detect-and-replace; delete the old one first.
- **THE SHELL DOES NOT SNAP TO THE GRID.** It lands on `map bounds ± margin ± thickness`,
  and the map bounds are wherever the geometry actually ends. The defaults (512 / 64) are
  multiples of every grid size the editor offers, so a map built on-grid produces an
  on-grid shell; a map with an off-grid brush does not.
- **`KiwiSky_SeeThroughNow` IS CALLED PER FACE**, but only for faces that already tested as
  sky (`isSky &&` short-circuits ahead of it), so a map with no sky pays literally nothing
  and a map with sky pays one `GetTickCount` plus six float compares per sky face.
- **THE SKY TAB'S LIST IS REBUILT EVERY FRAME** (a walk of `sorted_materials` with an int
  test and an optional case-insensitive substring). Deliberate — the registry grows when a
  map loads and when a material is first referenced — but it is O(material count) per
  frame while the tab is open.

## Round BA

One user item on top of 51b2452 + 298f38c (round AZ), verbatim, with a screenshot:

> *"sky's are all blue, make it so they work properly. It works in the real radiant and
> we're using the same rendering code basically, so it should work here."*

Two symptoms in that one screenshot: every Sky-tab tile showed the **"no map"** placeholder,
and the sky brushes in the 3D view were a **giant flat opaque blue plane**. Full write-up in
RADIANT_UX_DESIGN.md section 78.

### Verdicts

**A SKY COLORMAP IS ALWAYS A CUBEMAP, AND ROUND AZ'S TILE GUARD REJECTED EXACTLY THAT.**
Decoded from the shipped assets rather than assumed. `main/materials/sky_aftermath` (116
bytes): techSetName `sky`, `textureCount` **1**, one textureTable entry — name `colorMap`,
semantic 2 (`TS_COLOR_MAP`), samplerState 0xEB, image `aftermath_ft`; surfaceFlags 0x34
(`SURF_SKY` is in it), refStateBits `{0x08128812, 0x0D}`. `main/images/aftermath_ft.iwi`:
version 6, format 11 (DXT1), **flags 0xC6 — 0x04 is `IMG_FLAG_CUBEMAP`** (`r_image.h:24-26`),
1024x1024, 3,145,756 bytes = 6 x (1024x1024/2) + 28, i.e. six faces. `Image_Setup` routes
that flag to `Image_CreateCubeTexture_PC` (`r_image.cpp:1406-1408`), which sets
`mapType = MAPTYPE_CUBE` (`r_image.cpp:1194`). And the engine *requires* it: `R_SetSkyImage`
Com_Errors with *"colorMap '%s' for sky material '%s' is not a cubemap"*
(`r_bsp_load_obj.cpp:1169-1181`). All 45 `sky_*` materials have that same one-entry shape.
Round AZ's walk ended `if ( img->mapType != MAPTYPE_2D ) continue;` and called the cubemap
case an edge — it is 100% of sky materials, and with `textureCount == 1` there is nothing
else in the table to fall back to.

**THE FLAT BLUE WAS THE EDITOR'S OWN PAINT, NOT THE SKY MATERIAL, AND IT PREDATES ROUND AZ.**
`Cam_EditorMaterialColor` (`camwnd.cpp:590-615`) initialises `out[0..3] = 1.0f` and
overwrites **rgb only** from its name table, so every match is emitted at
`materialColor.w == 1` — a flat-colour override, since the editor shader family computes
`rgb = lerp( sample(colorMap) * vColor, materialColor.rgb, materialColor.w )`. Row 2 of that
table is `{ "sky", 0.45f, 0.62f, 0.92f }` (`camwnd.cpp:601`) = **(115, 158, 235)**, the blue
in the screenshot to the byte. Sky was painted solid before its technique could sample
anything, whichever material handle the `d_white` substitution picked — which is why round
AZ's careful exemption of sky from that substitution changed nothing visible.

**THE BINARY'S SKY RENDER PATH IS ALREADY REACHABLE FROM THE PORTED DRAW; NOTHING NEEDED
ADDING.** `main/techsets/wc_sky.techset` maps `"unlit"` — and every lit/fakelight slot — to
the technique `sky`. `main/techniques/sky.tech` is `stateMap "color_only"`,
`pixelShader 2.0 "sky.hlsl" { skyMapSampler = material.colorMap; }`,
`vertex.position = code.position`: it samples the CUBEMAP by a direction it computes itself.
`main/statemaps/color_only.sm` is depthTest LessEqual, depthWrite Disable, blendFunc
Disable/One/Zero. The world fill already asks for `TECHNIQUE_UNLIT` on a sky face, so the
user's *"we're using the same rendering code basically"* was a correct diagnosis: KIWI had
to stop painting over it, not add a sky renderer.

**ROUND AZ'S SEE-THROUGH ARM WAS NEVER TRANSLUCENT — "ALREADY BLENDED" IS NOT
"TRANSLUCENT".** `white_tools` blends `SrcAlpha / InvSrcAlpha`, and `Cam_DrawFace` pins the
per-vertex colour to `0xFFFFFFFF` (`camwnd.cpp:1153`), so source alpha is 1 and the blend is
arithmetically **opaque**. The `d_white` route's real effect is losing the DEPTH WRITE — the
clip/trigger/hint volumes read as see-through because they fail to occlude, not because you
see through them. `camwnd.cpp:642-644` had already written this down for caulk (*"It still
looks opaque (the vertex colour is 0xFFFFFFFF), but nothing behind it is ever occluded"*).
So the see-through sky was flat blue paint, fully opaque, that occluded nothing: exactly the
reported plane.

### Fixed

- **EVERY SKY TILE SAID "no map".** The tile walk keeps its `MAPTYPE_2D` fast path (which
  stores nothing and stays reset-proof for free) and gains a CUBE arm: `CubeFaceThumb`
  (`kiwi_skybox.cpp:259-314`) takes `GetCubeMapSurface(face, 0)`, creates a 128-square
  `D3DFMT_A8R8G8B8` texture in **`D3DPOOL_MANAGED`**, and copies with
  `D3DXLoadSurfaceFromSurface` (DXT1 decompress + downsample in one call; d3dx9 is already
  in this build via `stdafx.h:33` and `${D3DX_LIB}`). One cached texture per material name,
  `failed` resolved once. **The round-AX rule is kept, not bent: no `D3DPOOL_DEFAULT` object
  is created here.** MANAGED survives `Reset` by contract (the same choice
  `kiwi_entthumb.cpp:240-241` made), and the cache is dropped by the new
  `KiwiSky_ReleaseForReset`, called from `RTT_ReleaseForReset` beside
  `KiwiEntThumb_ReleaseForReset` (`radiant_rtt.cpp`). The copy loop also bakes alpha to
  `0xFF`, which retires D-AZ-C's accepted "alpha channel shows the tile background" edge for
  the cube arm.
- **SKY BRUSHES RENDERED AS FLAT OPAQUE BLUE.** The sky tests are hoisted above the
  world/tool split (`camwnd.cpp:2705-2708`) so the MATERIAL_COLOR can be chosen per arm, and
  a TEXTURED sky face is now emitted at `{1,1,1,0}` — `w = 0`, the world arm's own neutral —
  which hands the pixel to the material's `sky` technique. The `d_white` substitution below
  is unchanged for every other material.
- **THE SEE-THROUGH FILM IS A REAL FILM NOW.** It draws through `Cam_DrawFaceTinted` with
  MATERIAL_COLOR `{0,0,0,0}` and a packed per-vertex RGBA of `{0.45, 0.62, 0.92, 0.30}` —
  the binary's own translucent-fill recipe (`Cam_DrawSelectedFaceFill` at
  `colors[16].a = 0.25`, `camwnd.cpp:1523-1533`; `kiwi_hover.cpp:156-158` at 0.25-0.35
  through the identical `d_white` + `TECHNIQUE_UNLIT` route). `materialColor.w` is a lerp
  weight and writes `oC0.rgb` only, so it was never the opacity lever.
- **A SKY THAT IS ONE SLAB WAS "OUTSIDE" FROM EVERYWHERE.** D-AZ-B's union-AABB test assumed
  the sky brushes form a shell. A sky CEILING (8192 x 8192 x 64) has an axis nothing can be
  inside, so `KiwiSky_SeeThroughNow` was true from every camera position and that map's sky
  was film for ever — *"step inside and it goes textured"* never fired. `FlattenRescue`
  (`kiwi_skybox.cpp:398-441`) widens any axis whose sky extent is under half the MAP's extent
  on the same axis to cover the map's range. A real six-brush shell is unaffected bit for
  bit (its extent equals the map's by construction).
- **THE RENDER SITE NOW USES THE SAME SKY PREDICATE AS EVERYTHING ELSE.** Round AZ made
  `SURF_SKY` canonical (D-AZ-A) and left `camwnd.cpp:2676` on `strstr(leaf, "sky")`, so a
  `desert_backdrop` carrying `SURF_SKY` never entered the sky arm. `KiwiSky_IsSkyMaterial` is
  OR'd in ahead of the name test — widened, not replaced.

### Added

- **A "face" COMBO in the Sky tab header** (+X / -X / +Y / -Y / +Z / -Z, default +X,
  persisted as `KiwiSky/TileFace`). A tile is one face of a six-face cubemap and the sky
  shader computes its own sample direction inside a compiled shader this tree cannot read,
  so which D3D face carries the horizon is not derivable here. Changing it drops the tile
  cache; that is safe within a frame because the combo is submitted above the grid.

### Known limits, round BA

- **THE TILE IS ONE CUBE FACE, AND WHICH ONE IS RIGHT IS AN EYEBALL QUESTION.** Face 0 is a
  side face under every axis convention that maps world-up onto a cube axis, so it is the
  reasoned default — but the CoD4 sky shader's swizzle is not readable from this tree, and
  a set whose art puts the interesting part elsewhere needs the combo moved. No tile shows
  more than one face; a four-face horizon strip was considered and rejected as a much larger
  copy per material for a browser thumbnail.
- **THE 3D SKY IS THE `sky` TECHNIQUE'S OUTPUT, NOT A CoD4 SKY DOME.** The technique samples
  the cubemap by a direction derived from `code.position`, which is what the shipped editor
  does — but the editor draws brush faces, not a sky sphere, so what you see is that shader
  evaluated over the brush surface. Whether it matches the in-game sky pixel for pixel has
  not been verified against a running game.
- **`FlattenRescue` USES THE MAP BOUNDS, WHICH INCLUDE THE SKY BRUSHES THEMSELVES.** For a
  ceiling that is what makes it work (the ceiling's own Z is part of the span you count as
  "inside"). For a map that is a single sky slab and nothing else, the sky and map extents
  are equal on every axis and nothing widens — see-through then behaves exactly as it did in
  round AZ.
- **THE FILM ALPHA (0.30) IS A CHOSEN CONSTANT IN A MEASURED BAND**, not a derived one. It
  sits between the ported selected-face fill (0.25) and the round-AX active hover (0.35).
- **THE `MAPTYPE_2D` TILE ARM STILL OWNS NOTHING AND STILL CARRIES THE ALPHA EDGE.** It is
  reached only by a "sky"-NAMED material whose colormap is genuinely 2D — i.e. not an engine
  sky material at all.
- **THE TILE CACHE IS NEVER TRIMMED WHILE THE EDITOR RUNS.** One 128x128 A8R8G8B8 texture
  (64 KB) per sky material the user has actually scrolled past; 45 shipped sky materials is
  under 3 MB of MANAGED memory, and it is dropped whole on a device reset. There is no LRU.

## Round BB

Four user items on top of 249f83f (round BA), verbatim, plus two mid-round addenda:

> *"skybox still doesn't show, even when manuvering the camera into the hull."*
> *"i think with plasticity style controls, it's impossible to fly into this skybox cube.
> You need to setup the camera to ignore it as an object I'm guessing?"*
> *"Create skybox shell"* should group its brushes — *"yeah so just add it to a group"*
> *"the previews are sideways(+Y default I think)."*
> *"Add the hollow operation from legacy radiant to the search (F) menu as a command."*
> *"ah it works if you select it"*

Full write-up in RADIANT_UX_DESIGN.md section 79.

### Verdicts

**THE SKY TECHNIQUE RENDERS CORRECTLY IN THE EDITOR AND ALWAYS DID — THE FILM WAS SIMPLY
ALWAYS CHOSEN.** The user's own differential settles it: the world fill draws
`active_brushes` only, and the selected-brush pass (`camwnd.cpp:2863-2880`) draws each
face's OWN material at `TECHNIQUE_UNLIT` and never calls `KiwiSky_SeeThroughNow`. That
pass IS the textured arm with the film skipped, so *"it works if you select it"* means the
cubemap-by-direction `sky` technique already works under the editor's own view constants.
No render-path change was needed and **the planned 2D-cube-face fallback was not built.**

**THE INSIDE/OUTSIDE TEST READ A POINT THE ORBIT RIG CANNOT PUT INSIDE THE SHELL.** Round
AZ tested `Ed_Camera()->origin`. In KIWI the EYE is DERIVED, not steered: every mover
writes `origin = lookAt - forward * s_dist` (`kiwi_camera.cpp:935/:1019-1022`) and
`KiwiCam_FlyTick` ends at `KiwiCam_Translate`, which moves eye AND pivot by the same delta
(`:842`) — so the eye is rigidly leashed `s_dist` behind the pivot and flying never closes
that gap. Only the wheel changes `s_dist` (ceiling 65536), and under ORTHO — KIWI's default
since round M — `s_dist` IS the zoom (`KiwiCam_OrthoHalfHeight = s_dist*tan(fov/2)*0.75`),
so the pseudo-eye is thousands of units back by definition. The eye was therefore outside
the shell at every working zoom, `SeeThroughNow` was true for ever, and the sky was film
for ever. There is NO collision anywhere: flying through the shell was never blocked.

**FRAME-ALL WAS FEEDING THE SAME TRAP.** A sky shell is the map bounds plus a margin plus a
wall thickness, i.e. the biggest object in the map by construction, so `/` set the opening
distance from the very box the user then wanted to be inside of.

**TWO OF ROUND BA'S CLAIMS ARE CORRECTED.** (1) `main/techniques/sky.tech` declares NO
`materialColor`, so the `sky` technique never reads that constant and BA's `w = 0` for a
textured sky face is a NO-OP for every genuine sky material (kept anyway — it is still
right for a "sky"-NAMED material that resolves to a lerping shader). (2) The flat blue was
therefore NOT paint over the sky technique: it came from the `d_white` SUBSTITUTION, whose
techset is `tools` -> `vertcol_shaded_tools`, which does lerp by `materialColor.w`. Same
colour, different mechanism — and it means the see-through arm was the only thing between
the user and the sky.

**WHICH CUBE FACE IS THE HORIZON WAS ANSWERABLE FROM THE ART, NOT THE SHADER.** D-BA6 called
it underivable and made it a UI choice with default `+X`. Decoding four shipped cubemaps
(`aftermath_ft`, `sp_bog_ft`, `fallujah_ft`, `chechnya_ft` — a 28-byte header plus six DXT1
faces in D3D face order; `Image_CubemapFace`, `r_image_load_common.cpp:7-11`, is the
identity) and LOOKING at the faces gives the same answer four times: face 0/1 have the
horizon running VERTICALLY (world up = image left / right), face 2 is UPSIDE DOWN, **face 3
(`-Y`) is upright**, face 4 is the zenith cap and face 5 the ground disc. That is exactly
what falls out of sampling the cube by the RAW WORLD direction (z up) through D3D's Y-up
per-face `(sc,tc)` basis, so it is a property of the ENGINE and not of an art set.

**CSG HOLLOW WAS NOT A STUB AND WAS NOT MISSING — ONLY ITS REGISTRY ROW WAS.**
`Cmd_OnSelectionMakehollow` (`mainfrm.cpp:2412`) + `CSG_MakeHollow` (`csg.cpp:318`) are
ported, dispatched (`mainfrm.cpp:5478`), labelled (`kiwi_command.cpp:588`) and already have
a CSG-panel button (`kiwi_csg.cpp:150`). The palette lists the RADIANT COMMAND TABLE, and
the shipped table binds `CSGMerge` and `AutoCaulk` but not hollow — faithfully, because the
binary gave hollow a menu item and no key.

### Fixed

- **THE SKY NEVER WENT TEXTURED.** `KiwiSky_SeeThroughNow` now tests the ORBIT PIVOT
  (`KiwiCam_LookAt`) OR the eye. The pivot is the point the user is working at, and under
  ortho it is the only honest position there is; the eye stays OR'd in so a perspective
  fly-through that leaves the pivot behind still counts as inside, and so that every camera
  that previously read INSIDE still does.
- **"/" FRAMED THE SHELL.** `kiwi_focus.cpp`'s no-selection fallback skips sky brushes
  (`AddWorldBrushes(box, skipSky)`), through the new exported `KiwiSky_IsSkyBrush`. Only the
  FALLBACK skips — an explicit selection is framed whatever it contains — and the walk is
  re-run including sky when skipping it would have left nothing to frame.
- **THE SHELL IS SIX LOOSE BRUSHES.** "Create skybox shell" now lands them in a `func_group`
  whose `targetname` is **SkyBox**, through round W's create-only path
  (`Eclass_ForName` -> `Entity_Create` -> `Undo_SetIdForEntity` -> `SetKeyValue`) inside the
  bracket the verb already opened, so it is still exactly one Ctrl+Z. The merge trap cannot
  fire: the selection at that point is exactly the world-owned brushes `LandDef` just
  created.
- **THE TILES WERE SIDEWAYS.** The default face is now 3 (`-Y`), the one that is stored
  upright, and every other face is turned upright inside the copy (quarter-turns
  `{1,3,2,0,0,0}` clockwise) in the same lock pass that bakes alpha. `rot == 0` takes no
  scratch copy, so the default face costs exactly what round BA's did.
- **HOLLOW WAS NOT IN THE F MENU.** One `Radiant_RegisterCommand( "CSGHollow", 0, 0, 32982 )`
  in `KiwiCmd_RegisterCommands`, UNBOUND — the binary gave it no key and this adds none. The
  row routes to the ported handler through the same `PostMessage(WM_COMMAND)` path every
  other palette row takes.

### Known limits, round BB

- **THE SHELL CAN NOW HIDE THE MAP FROM OUTSIDE, AND THAT IS THE ACCEPTED COST OF THE PIVOT
  TEST.** With the pivot at the map centre — where it is whenever the user is working — the
  see-through film only fires once the PIVOT leaves the shell. Frame the whole map from
  outside and the near sky wall is drawn textured over it. The escape shipped in the same
  round: the shell is a named `func_group`, so one Outliner click (or `H`) hides it. If this
  turns out to be the more annoying of the two failure modes, the next lever is a
  three-state toggle (always film / auto / never film) rather than a different point.
- **A SKY WALL DOES NOT WRITE DEPTH, SO WHETHER IT PAINTS OVER THE MAP IS SUBMISSION-ORDER
  DEPENDENT.** `color_only.sm` is depthTest LessEqual + depthWrite Disable, and the world
  fill is an immediate per-face draw in brush-list order. A sky face submitted BEFORE the
  geometry behind it cannot occlude it; one submitted after can. Not changed this round.
- **THE FACE ROTATION IS MEASURED, NOT DERIVED FROM THE SHADER.** Four art sets agree and the
  D3D per-face basis explains all four, but `sky.hlsl` itself is still a compiled blob this
  tree cannot read. A custom sky cubemap authored by hand against a different convention
  would show up rotated, and the combo remains the answer.
- **"SkyBox" IS NOT UNIQUIFIED.** Build the shell twice in one map and there are two
  `func_group`s named SkyBox. Deliberate — the duplicate is a mistake worth seeing.
- **THE GROUP IS NOT APPLIED RETROACTIVELY.** Shells built before this round stay loose in
  worldspawn; select them and use the Outliner's Group Selection.
- **THE COMMAND-TABLE EXTENSION BLOCK IS NOW ~96 OF 112 ROWS.** Its overflow mode is still
  SILENCE (`mainfrm.cpp` RADIANT_COMMANDS_KIWI_EXTRA): the next few rounds can add rows, but
  the block wants raising before it is full.
- **NOTHING HERE WAS BUILT OR RUN.** Per the standing rule the agent does not build; every
  claim above is from reading the tree and decoding the shipped assets, except the ones the
  user observed directly.

## Round BC

Zoom range, the zoom round trip, and the grid regression. RADIANT_UX_DESIGN §80 carries the
derivations; this file carries what is still true afterwards.

### Verdicts

- **"The zoom out limit is too restrictive"** — CONFIRMED, and the constant it was derived
  from was itself wrong. `KCAM_ORTHO_DEPTH` (camwnd.cpp:183) was sized at the engine's
  *brush* bound while being measured from the *eye*. Slab 131072 -> 524288, ortho ceiling
  65536 -> 262144 on the unchanged `depth/2` derivation. FIXED.
- **"Zoom is broken after zooming out all the way, can't zoom back in all the way"** —
  CONFIRMED, two independent causes, both a clamp applied downstream of the state feeding
  it. (a) `ClampToWorld` pinned the eye while `s_dist -= move` had already recorded the
  move, so `PivotOnAxis` walked the ORBIT PIVOT forward one notch's worth every notch and
  nothing on the way back in could undo it. (b) the `KCAM_ORTHO_HALF_MAX` clamp on the
  derived half-height created a wheel dead zone at any fov above ~68. Both FIXED.
- **"the grid broke totally in the last update" / "doesn't render at all"** — CONFIRMED as a
  REGRESSION, and it is not a grid bug. Round BB (`e1b9ae4`) made `KiwiSky_SeeThroughNow`
  return false whenever the ORBIT PIVOT is inside the sky shell, which under the ortho
  default is always; the six sky walls went opaque, and the near one — depthTest LESSEQUAL
  with depthWrite DISABLED (`color_only.sm`) — painted over the ground lattice that
  camwnd.cpp:2622 submits BEFORE the world. The map survived because sky writes no depth.
  Round BB logged this exact mechanism as an accepted cost ("THE SHELL CAN NOW HIDE THE MAP
  FROM OUTSIDE"). FIXED by making the predicate per-face and depth-based.

### Fixed

- `src/radiant/camwnd.cpp:183` — `KCAM_ORTHO_DEPTH` 131072 -> **524288**, with the
  measurement error that produced the old number written down.
- `src/radiant/kiwi_camera.cpp` — the ceiling is two numbers now, read through
  `KCam_MaxDist()`: `KCAM_MAX_DIST_ORTHO` = depth/2 = **262144**, `KCAM_MAX_DIST_PERSP` =
  **65536** (round AZ's, deliberately unchanged — perspective is bounded by the float32
  inverse-VP degeneracy, not by the slab). The eye leash likewise:
  `KCAM_ORIGIN_BOUND_ORTHO` = **524288**, `KCAM_ORIGIN_BOUND_PERSP` = **131072**, read
  through `KCam_OriginBound()`. The three ortho constants are written as relations of
  `KCAM_ORTHO_DEPTH_HALF` so the ladder cannot be raised half way.
- `src/radiant/kiwi_camera.cpp KiwiCam_Dolly` — `move` is now saturated against the eye
  leash as well as against the ceiling, so `ClampToWorld` can never rewrite an origin the
  dolly has already accounted for in `s_dist`. This is the pivot-walk fix.
- `src/radiant/kiwi_camera.cpp KCam_MaxDist` — the `KCAM_ORTHO_HALF_MAX` limit is folded
  into the DISTANCE ceiling instead of clamping the derived half-height, so there is no fov
  at which the wheel moves `s_dist` without moving the image.
- `src/radiant/kiwi_camera.cpp KiwiCam_SetOrtho` — leaving ortho above the perspective
  ceiling now pulls the eye/pivot pair in rigidly (pivot fixed, eye re-derived) instead of
  leaving `s_dist` and the eye 200,000 units apart.
- `src/radiant/kiwi_skybox.{h,cpp}` — `KiwiSky_SeeThroughNow()` ->
  `KiwiSky_SeeThroughFace( const float *facePoint )`. A sky face is film iff its view-space
  depth is less than the orbit pivot's; `nullptr` falls back to round BB's containment
  answer verbatim. Design note D-BC-A.
- `src/radiant/camwnd.cpp` (world fill, sky arm) — passes the face winding's CENTROID to the
  new predicate. One fenced block; the rest of the arm is byte-identical.
- `src/radiant/kiwi_grid.h` — `KGRID_MAX_EXTENT` 131072 -> **524288**, matching the new
  slab, so the lattice can cover the frame at the new ceiling. Costs no extra segments
  (`KGRID_HALF_CELLS` bounds the count; this constant only truncates the window).

### Changed (behaviour)

- **A SKY SHELL NOW READS AS FILM IN FRONT AND TEXTURE BEHIND, IN THE SAME FRAME.** Round AZ
  filmed all six walls; round BB textured all six; round BC films the ones between you and
  the pivot and textures the rest. Framing a map from outside a shell therefore shows the
  map through a 30%-alpha near wall with real cubemap sky visible beyond the map. The
  see-through toggle still switches the whole behaviour off.
- **THE PERSPECTIVE ZOOM CEILING IS UNCHANGED AT 65536** and pressing `P` while zoomed
  further out than that in ortho now moves the camera (the eye comes in to 65536 from the
  pivot). That is a visible jump, and it is deliberate: the alternative is a projection the
  frame-validity guard starts rejecting.

### Known limits, round BC

- **3D PICKING STOPS REACHING THE GROUND ABOVE ~245,760 OF ORTHO STANDOFF.** The ported
  `Test_Ray` casts a fixed 262144-unit segment (`select.cpp:317-319`, and it seeds every
  slot's distance at the same number, `:773`), and KIWI's ortho pick ray starts
  `KCAM_ORTHO_PICK_LEAD` = 16384 BEHIND the eye plane (camwnd.cpp:180). So in the top ~6% of
  the new zoom range a click cannot reach what it is pointing at: selection, hover and the
  dolly's zoom-to-cursor surface reference all come back empty. The dolly degrades cleanly
  (it falls back to `s_dist` and the wheel keeps working); selection simply does nothing.
  NOT FIXED — the fix is either an adaptive pick lead or a raised seed in ported code, and
  neither was worth touching for a range nobody selects brushes at.
- **DEPTH RESOLUTION IN ORTHO IS NOW ~1/16 UNIT PER STEP** (2 x 524288 linear over 24 bits),
  where it was ~1/64 before. Brush coordinates are integers and coplanar faces tie either
  way, so nothing the editor draws should stop resolving — but if Z-fighting appears between
  surfaces less than 1/16 unit apart at extreme range, this is the cause and the lever is
  `KCAM_ORTHO_DEPTH`.
- **AT A GRAZING PITCH THE FAR HALF OF THE LATTICE CAN STILL LEAVE THE DEPTH SLAB.** The
  ortho footprint's 1/sin term lets `need` reach ~1.5M units near `KGRID_ORTHO_MIN_SIN`
  (~4.6 deg), the window is then clamped to `spacing * KGRID_HALF_CELLS`, and at the new
  ceiling that clamped span plus the standoff can exceed 524288 of view depth — so the
  furthest lines are clipped by zFar. Round S's edge fade is already ramping the whole
  lattice out at that angle, so it is invisible in practice; it is written down because it
  is the one place where raising the ceiling did NOT come with a matching lattice bound.
- **THE ORTHO/PERSPECTIVE CEILING HANDOVER IS ONE-WAY-VISIBLE.** Going ortho -> perspective
  above 65536 moves the camera; going back does not restore the old standoff (the wheel
  does). Camera state is not undoable by design (§10), so there is nothing to restore it
  from.
- **THE SKY DEPTH TEST USES THE ORBIT PIVOT, SO IT MOVES WHEN THE PIVOT MOVES.** Orbiting
  onto a point outside the shell will flip the near walls from film to texture mid-gesture.
  That is the rule working as specified — the pivot IS the subject — but it means the sky's
  appearance is a function of camera state and not of the map, and a user who wants it
  stable has the toggle.
- **NOTHING HERE WAS BUILT OR RUN.** Per the standing rule the agent does not build; every
  claim above is from reading the tree, except the ones the user observed directly.

## Cleanup pass (2026-08)

A three-auditor read-only audit of the KIWI layer produced 188 findings (AUDIT_A 74,
AUDIT_B 40, AUDIT_C 74; bodies and routing table in
`F:\CLAUDE WORKING DIR\kiwi_cleanup\`). They were remediated in three waves:
**wave 1 — correctness** (`2fcb60f`, all S1s plus the behaviour-coupled S2s),
**wave 2 — consolidation** (`4453d0b`, one canonical spelling per question; added
`kiwi_vec.h`, `kiwi_str.h`, `kiwi_texcache.h`, `kiwi_cmdui.h`), and **wave 3 —
dead code, stale comments, silent-failure reporting, safe perf and polish**
(this working tree; 96 files, +2039 / −2189 lines). Wave 3 deleted ~2,190 lines of
unreachable code and superseded narration, gave every named silent bail-out a console
line (grid bail-outs behind a once-per-transition latch, not per frame), and moved
per-frame work below the visibility early-out in the palette, the add menu and the
hint strip. Nothing in this pass was built or run — every claim is from reading the
tree, which is the standing rule.

**Deliberately deferred — real rewrites, not cleanup:**
- **Perf rewrites** A-31 (extrude re-triangulates every source profile per frame),
  A-32 (trim copies a `chain_t` per candidate per frame), A-62 (spline preview
  allocates three vectors per frame), B-22 (`KiwiPatchVerts_DrawWorld` is
  O(points × selection)), B-23 (`KiwiArrange_Cells` is O(n²) three times over),
  B-37 (3 KB stack array rebuilt per frame), C-13 (`ImGuiConsole_Append` memmoves
  ~96 KB per line), C-42 (`KiwiVis_Restore` is O(brushes × snapshot)), C-51
  (per-frame O(rows × store) scans in the Outliner). Each needs a cache with a
  correct invalidation key — feature-round material, not a mechanical sweep.
- **B-8** — the six-helper `KiwiConDragBase` for kiwi_fillet/kiwi_offset. Inserting a
  base class under two live modal commands whose `Recompute`s already differ is not
  reviewable-correct by reading without a build. B-9's shared constants shipped.
- **A-60 (half)** — the `ToolSides()` collapse needs a `KiwiRoundTool` intermediate
  inserted into a 10-subclass hierarchy with `m_radius` moved up out of three classes.
  The preview-emit half (`DrawObjectPreview`) shipped in wave 2.
- **C-47** — the third hand-maintained copy of the map save-enumeration. Two of the
  three sites are ported (`map.cpp`), it is on the save path, and the visibility
  sidecar's correctness argument depends on the ordinal space. Needs a fence, a
  3-map round-trip gate and operator sign-off.
- **C-20 (half)** — the entity-browser drop path now prints honestly, but the classic
  Add-menu create path still lands a model-less `misc_model`. Fixing it needs a new
  fence in ported `xywnd.cpp`/`win_ent.cpp`.
- **C-21** — Surface Inspector is still the last patch-retexture funnel with no KIWI
  fence, so retexturing a patch there loses its density. Feature work on a ported dialog.
- **C-25** — `mainfrm.cpp` is double-encoded UTF-8 on 251 lines. Left for the operator:
  it wants its own commit, not a mix-in. Until then every `mainfrm.cpp` edit must
  anchor on exact existing bytes (wave 3's C-2 edit did).

**Cite convention (C-15).** An offline checker now lives at
`F:\CLAUDE WORKING DIR\kiwi_cleanup\tools\check_cites.py` (stdlib only). The
load-bearing population — `extern` declaration cites — went from **128 wrong of 242**
to **2 wrong of 254**, and both residuals are false positives on one prose line.
The whole-corpus figure moved 1197 → 1035 wrong of 1626; that remainder is almost
entirely PROSE cites into ported IDA files (the `imgui_panel_*` narrations of
`patchdialog.cpp` behaviour). Per C-15 those were deliberately not hand-swept. Run
`check_cites.py --root <repo> --externs-only` after any ported-file edit; the drift
is structural, and the long-term fix is dropping line numbers from prose cites.

## Round BD

The TrenchBroom-style UV editor window (`src/radiant/kiwi_uveditor.cpp/.h`,
RADIANT_UX_DESIGN §82). Nothing in this round was built or run — every claim is from
reading the tree, which is the standing rule.

**Deliberate limitations, all visible in the UI rather than only here:**

- **The numeric fields are FACE-ONLY.** A patch carries per-control-point ST directly
  (`qe3.h:71-81`, `drawVert_t.texCoord`) and has no texdef at all, so "shift x = 12" has
  nothing to write to. The row is greyed with a tooltip that says so. The five gesture
  verbs (offset, rotate, scale, skew, origin) and the Flip / Rot 90 buttons DO reach
  patches, through an ST transform about the origin handle — that is what the directive
  asked for. A future round could map the six fields onto a synthesised relative texdef
  and push it through `Patch_SetTextureInfo` (pmesh.cpp:3369), which is exactly what the
  Surface Inspector's patch spin arrows already do; it was not attempted here because that
  core dispatches one field at a time by "is it non-zero", which is a different editing
  model from "type an absolute value".

- **Reset on a selection holding patches is TWO undo steps.**
  `Patch_NaturalizeSelected` opens its own bracket (pmesh.cpp:2755-2757) and cannot join
  ours, so a mixed face+patch selection gets "uv reset" plus the naturalize's own record.
  The console line says so at the moment it happens. The alternative — a bracket-free
  forwarder in the `Patch_KiwiLmapAlign` / `Patch_KiwiCapAlign` shape (pmesh.cpp:2875 /
  :2908) — is a ported-file change and wants its own round.

- **Rot 90 turns faces and patches about DIFFERENT pivots.** A face turns through its
  texdef `rotate` field (`rotate ± 90`, exact, no decompose — TrenchBroom's own button);
  a patch turns its control STs about the yellow ORIGIN handle, because there is no texdef
  to add 90 to. On a mixed selection the two halves therefore do not agree about the centre
  of rotation. Both are in one undo record and the tooltip states the split.

- **A material whose colorMap is not MAPTYPE_2D gets no background.** The window takes only
  the round-AZ 2D arm (kiwi_skybox.cpp:468) and never allocates a texture (§82 D-BD-E), so
  a cubemap colormap — i.e. any sky material — draws the flat plate plus a readout note
  instead of tiles. The grid, the wireframes and every gesture still work. Fixing it would
  mean owning a MANAGED per-face copy and joining `RTT_ReleaseForReset`, which is the whole
  cost that arm was chosen to avoid for the common case.

- **Zoomed far enough out, the tiled background stops drawing.** The ImGui D3D9 backend
  forces `D3DTADDRESS_CLAMP` (`imgui_impl_dx9.cpp:149-150`), so tiling is a bounded grid of
  unit-quad `AddImage` calls; past `KUVE_MAX_TILES` (1600 quads) the canvas keeps the flat
  plate. Nothing else changes.

- **Esc may not reach the window.** Cancel is wired to `ImGui::IsKeyPressed(Escape)` and to
  `io.AppFocusLost`, but `KiwiUX_KeyFunnel` (kiwi_command.h) sits in
  `Radiant_PreTranslateMessage` ahead of ImGui for keys the shell does not claim, so an
  Escape typed with the canvas hovered but not keyboard-focused can be eaten by the editor
  instead. The mouse release is the primary end of a gesture and is unaffected; a cancelled
  gesture can also always be undone with Ctrl+Z, since it is one record.

- **A very large ST range disables the grid and the tiles.** Both convert the visible UV
  rect to integer line indices, so `VisibleUvRect` refuses anything past 1e6 repeats rather
  than run an undefined `(int)floorf(1e30f)`. That is reachable from a texdef `size` of a
  few thousandths, which the Surface Inspector will type.

**One line-number sweep this round forced.** Adding one `#include` to `imgui_shell.cpp`
shifted every definition in it by one line and invalidated seven extern cites across
`imgui_panel_entity.cpp`, `kiwi_construct.cpp`, `kiwi_entbrowser.cpp`, `kiwi_skybox.cpp`,
`kiwi_uv.cpp`, `kiwi_uveditor.cpp` and `kiwi_viewport.cpp`. All seven were corrected;
`check_cites.py --externs-only` reports 268 OK of 270 with the two pre-existing
`kiwi_primitive.cpp:67` prose false positives as the only residual. This is the structural
drift C-15 already names — the long-term fix is still dropping line numbers from prose
cites, and PROSE cites into `imgui_shell.cpp` (about 26 of them) are now one line stale
and were deliberately not swept, per C-15.

## Round BE

Texture import: `WM_DROPFILES` on the frame, a modal "Import texture" wizard, an .iwi
writer and a binary material writer. Design and derivations are RADIANT_UX_DESIGN.md
section 83, which carries the same D-BE-A..M labels the three headers use. No layout version bump — the wizard is a popup, not a dock window.

**LIMITS AND DEFERRED WORK**

- **Source formats: .tga .png .jpg .jpeg .bmp .dds only** (case-insensitive), and only
  2D images. `KiwiIwi_Probe` refuses anything `D3DXGetImageInfoFromFileA` reports as a cube
  or volume texture, and anything over 4096 on a side. A dropped file with any other
  extension is refused with a console line naming it. Everything is decoded through D3DX;
  no format the DXSDK cannot open will ever work here.

- **Power of two is mandatory.** Non-POT sources are resampled by D3DX (checkbox, default
  on); with the checkbox off the writer refuses. The reason is not taste — see section 83
  D-BE-D: `Image_CountMipmaps` and D3D disagree on the level count for non-POT dimensions,
  so a non-POT .iwi would make the reader upload to a mip level that does not exist.

- **Output is 2D, mipped, non-cube, non-wavelet.** Format 1 (A8R8G8B8) or, with Compress
  ticked, 11 (DXT1, no alpha) / 13 (DXT5, alpha). `flags` is always 0. Cubemaps
  (flags & 4), the wavelet formats (6..10), A8L8/L8/A8 (3/4/5) and DXT3 (12) are all
  readable by the engine and are all outside this writer.

- **DXT is skipped below 4 px.** A POT image narrower or shorter than one 4x4 block falls
  back to uncompressed, because D3D rounds such a level up to a full block and would stop
  matching the reader's `((w+3)>>2)` advance.

- **Four material templates.** World lit opaque (`l_sm_r0c0`), lit alpha-test
  (`l_sm_t0c0`), lit alpha-blend (`l_sm_b0c0`), unlit opaque (`unlit`). NOT covered: sky,
  water, effects, tools, `2d`, layered materials, anything needing a real normal or
  specular map, and anything with more than one colormap. The dropdown greys out any family
  this data tree has no clonable member for.

- **The clone inherits the template's normal/spec binding.** All four templates bind
  `$identitynormalmap`, so an imported material is flat-normal and spec-less by
  construction. There is no way to supply a normal or specular map through the wizard; that
  needs a multi-image import flow and a second file slot per material.

- **Overwrite refreshes the image but not the material metadata this session.**
  `Material_Register_LoadObj` (r_material.cpp:554) returns a cached `Material*` when the
  name is already in the hash table, so re-importing over a name this session has already
  browsed re-binds nothing: the wizard calls `Image_Reload` so the PIXELS update, but the
  cached material's techset, sortKey and stashed editorUsage/editorLocale stay as they
  were. The on-disk header IS correct and a restart picks it up. The browser qtexture is
  re-stamped in place, so the thumbnail size and the filters do update.
  Corollary: on that same path, verification stage 2 re-validates the cached material
  rather than re-parsing the new blob. The header check and the browser gate always read
  the new bytes off disk, so an invisible material can still never be produced.

- **An overwrite that fails verification is NOT rolled back.** Deliberate: the previous
  files are already gone by then, and deleting the replacement would turn a bad import into
  a missing asset that every map referencing that name would load as `$default`. The
  console says so explicitly. A FRESH import that fails at any stage does delete both files.

- **The drop is accepted anywhere on the editor window, not just over the Textures pane,
  and there is no hover highlight.** Section 83 (D-BE-J / D-BE-K) explains why the position cannot be
  resolved in time (the Textures dock is an ImGui window, not an HWND, and the HDROP is
  dead before the next ImGui frame). A highlight would need a WM_ level drag-over hook
  (IDropTarget) rather than WM_DROPFILES.

- **The wizard is a modal popup, so it blocks the editor while it is up.** With a large
  drop the "apply these settings to the remaining files" checkbox turns the rest into a
  silent one-file-per-frame batch; there is no progress bar and no way to abort a running
  batch except closing the editor.

- **Escape closes the wizard as Cancel All.** Nothing has been written at that point on the
  current file, so there is nothing to roll back, but the rest of the queue is dropped.

- **Usage/locale come from the loaded filter tables, so a tree without
  `deffiles/materials/usage.txt` cannot import.** The wizard detects that
  (`FillTextureMenu` left both counts at 0) and disables Import with an explanation, rather
  than writing a `usage == 0` material that `Load_Materials` would hide forever.

- **Locale values are only meaningful within the locale.txt this tree ships.** Shipped CoD4
  materials carry bits (e.g. 0x80 = bit 7) that the four-entry stock `locale.txt` does not
  name; the filter chain ANDs against whatever bit the selected row carries, so an imported
  material tagged "Generic" (bit 6) will not appear under a locale filter for a bit the
  shipped art uses. Only the "all" default (the boot state) shows everything.

- **Name sanitisation is lossy and silent.** Lowercase, spaces/dots/hyphens to underscores,
  everything outside `[a-z0-9_]` dropped, leading underscores stripped, truncated to 50
  characters (the engine's 64-byte `"materials/%s"` and `"images/%s.iwi"` buffers). Two
  differently-named source files can therefore collide on one material name; the second one
  hits the duplicate warning rather than silently overwriting.

- **The material and the image always share one name.** There is no way to point a new
  material at an existing image, or to import an image without a material.

- **Tiling is written as `autoTexScaleWidth/Height`, which is a `uint16`.** A tiling value
  above 65535 is rejected by the input; the browser thumbnail size and a fresh face's
  texdef both derive from it (`TexWnd_BuildClickedMaterialDef`), so it is not purely
  cosmetic.

- **No undo.** An import writes two files and adds a browser entry; Ctrl+Z does not remove
  either. Deleting `<gameroot>/main/materials/<name>` and
  `<gameroot>/main/images/<name>.iwi` and restarting is the manual reversal.

**CITE SWEEP THIS ROUND.** One `#include` added to `imgui_shell.cpp` and ~105 lines added
to `texwnd.cpp` shifted 16 extern cites across `imgui_panel_entity.cpp`, `imgui_shell.cpp`,
`kiwi_construct.cpp`, `kiwi_entbrowser.cpp`, `kiwi_skybox.cpp/.h`, `kiwi_uv.cpp`,
`kiwi_uveditor.cpp` and `kiwi_viewport.cpp`. All were corrected;
`check_cites.py --externs-only` reports **279 OK of 281**, with the two pre-existing
`kiwi_primitive.cpp:67` prose false positives as the only residual. This is the same C-15
structural drift round BD recorded, and the long-term fix is still dropping line numbers
from prose cites; PROSE cites into `imgui_shell.cpp` and `texwnd.cpp` were again not swept.

## Round BF

The Build & Run launch dialog: a floating "Build & Run" window (instant command
**KIWI_CMD_BUILD_RUN 34130**, unbound, palette-searchable) that invokes `KIWI-cod4map.exe`,
`KIWI-cod4rad.exe` and `KIWI-mp.exe` as separate processes, streams their stdout+stderr into
a log pane, and feeds compile failures into the editor's existing `.errlog` / `.lin` readers.
Design and derivations are RADIANT_UX_DESIGN.md section 84, which carries the same D-BF-A..G
labels the header uses. No layout version bump — the dialog is a floating window, not a dock
window. `src/cod4map/` and `src/cod4rad/` were read but not modified.

**LIMITS AND DEFERRED WORK**

- **Every build targets `raw\maps\mp\<name>.map`, and only that.** Both compilers require the
  map argument to sit two folders below a component named `maps`, and the MP game reads
  `maps/mp/<name>.d3dbsp`. There is no SP target (`maps/<name>.d3dbsp`), no per-map override
  of the directory, and no way to compile into `usermaps\` or `devraw\`. A map whose basename
  collides with a shipped stock map will overwrite that map's `.d3dbsp` in `raw\maps\mp\`.

- **No zone / .ff build.** There is no fast-file builder anywhere in the tree, so the dialog
  cannot produce one. This works today only because `useFastFile` defaults 0
  (common.cpp:1525-1529) and the game loads the loose `.d3dbsp`. If that dvar is ever flipped
  on by default, Run Map stops finding maps and this dialog cannot fix it.

- **`+map` / `+devmap` require a player profile.** `SV_Map_f` drops with
  `PLATFORM_NOTSIGNEDINTOPROFILE` when `Com_HasPlayerProfile()` is false and the process is
  not dedicated (sv_ccmds_mp.cpp:388-390). `bin\<CONFIG>\players\` exists in this tree but its
  contents were NOT verified this round; if Run Map drops at startup, that is the first thing
  to check. The dialog cannot detect it in advance — the check lives inside the game.

- **The `.errlog` / `.lin` handoff only fires when `currentmap` matches the map we compiled.**
  Both ported readers key off `currentmap` (map.cpp:44) while the dialog compiles
  `s_currentMapPath` (mainfrm.cpp:488). They agree after File>Open and after a plain Save;
  they DIVERGE after Save-As (only `s_currentMapPath` moves, mainfrm.cpp:4447) and while a
  prefab is open (`currentmap` is stomped, map.cpp:1797-1813). In that state the dialog prints
  both paths and the real `.errlog` / `.lin` location and refuses to call the readers, rather
  than popping "Error log file was not found" at a path the user never named. Re-opening the
  saved map from disk resyncs the two. Making `Save-As` write `currentmap` too would fix it at
  the source and is a deliberate NON-change this round: `currentmap` is ported state with four
  other writers and its own prefab semantics.

- **A leak and an error log are mutually exclusive in the editor, so only one is loaded.** The
  ported File menu treats them that way (each handler clears the other) and Misc>Next leak spot
  prefers the pointfile; the dialog follows, preferring the leak. The other file is still on
  disk and the log names it.

- **The poll only runs while the shell is drawing frames.** The child's pipe is drained from
  `KiwiLaunch_Draw`, which the pump reaches once per 1/60 s while the frame is visible and the
  primary surface is active. If the editor stops rendering entirely (device lost and not yet
  recovered, or the pump blocked in a nested modal loop such as a MessageBox), the 64 KB pipe
  can fill and the compiler will block on its next write until frames resume. It does not lose
  output and it does not deadlock — it stalls until the editor draws again.

- **One capture stage at a time.** Build BSP and Build Light are mutually exclusive (they both
  own the same `.d3dbsp`) and the buttons grey while either runs. Run Map is exempt and every
  press spawns another game; the dialog does not track them and does not close them.

- **Kill leaves partial output.** `TerminateProcess` on cod4rad leaves a half-lit `.d3dbsp`
  (it rewrites the file in place), and on cod4map whatever it had written. The confirm text
  says so. Recovery is a fresh BSP build.

- **`radtrans.bin` lands in the game root.** cod4rad opens its relight cache by bare relative
  name, so it lands in the process CWD — which this dialog sets to the game root — and is
  shared across maps. `-NoRelight` is ON by default for that reason; unticking it is only safe
  when re-lighting the same map repeatedly.

- **Extra-args fields are passed through verbatim.** They are inserted before the `-loadFrom`
  pair (cod4map) or before the map name (cod4rad) with no validation and no quoting. A
  malformed switch reaches the tool, which prints its usage block and exits — cod4map with
  `exit(-1)`, i.e. exit code 4294967295.

- **The dialog does not watch the map for changes.** "Save the map before building" (default
  on) runs the same funnel `Radiant_FileSave` uses, unconditionally — it does not check
  `modified`. Untick it to compile the last saved state while the editor holds newer edits.

## Round BG

The UV editor user-feedback batch plus the legacy camera context-menu removal
(`src/radiant/kiwi_uveditor.cpp/.h`, `kiwi_viewport.cpp`, `camwnd.cpp`, `imgui_shell.cpp`;
RADIANT_UX_DESIGN 82.1). Nothing in this round was built or run - every claim is from
reading the tree, which is the standing rule.

**Deliberate limitations:**

- **The snap candidates come from the ACTIVE face only, so a folded-out neighbour offers
  none of its own.** Vertex snap (scale), edge-angle snap (rotate) and edge-slope snap
  (skew) all read the active face's winding, whose display transform is identity by
  construction, so the candidates they produce are exact in the frame the gesture is
  expressed in. A neighbour folded out beside it contributes nothing: its vertices would
  have to be mapped through its own D and back, and the four snap helpers would have to
  agree on which face a candidate came from. Ctrl still suppresses snapping entirely, and
  the grid snap (which is a canvas object, not a face) is unaffected.

- **The world-edge weld is a quantised bucket match, not an epsilon search.** Winding
  points are rounded to a 0.25-unit bucket (KUVE_WELD_EPS) and matched exactly, which keeps
  the whole solve O(P log P). A shared vertex whose true coordinate sits within a
  thousandth of a bucket boundary can land in two different buckets, in which case that
  adjacency is missed and the face is SHELVED beside the chain instead of folded onto it.
  Cosmetic, and the fallback is a sane layout rather than a wrong one.

- **A fold joins the edge DIRECTION and one endpoint, not the edge LENGTH.** D_f is rigid
  by design (rotation + translation, optionally a reflection), and two faces carrying
  different texture scales have different ST lengths for the same world edge. The shared
  endpoint and the direction match; the far end runs long or short. Making it a similarity
  would close the gap and would also mean a displayed outline is no longer the true shape
  scaled uniformly by the canvas zoom, which every pick tolerance in the file assumes.

- **Past 256 selected faces the chain is not built at all** (KUVE_CHAIN_MAX_FACES): every
  D stays identity and the canvas is BD's stacked view. The readout says so. A fold-out of
  a thousand faces is not a diagram, and the weld is not free.

- **The chain re-solves after every edit.** The layout is frozen for the duration of a drag
  (it is defined in terms of the STs the drag is rewriting), then re-solved when the
  signature moves. An edit that changes only some shapes can therefore shift the whole
  fold-out when it commits. That is the layout being correct again, not drift, but it is a
  visible jump.

- **Rot 90 and Flip now go through texturevecs_02 for faces, so the numeric readout can
  re-parameterise.** They used to write `rotate` and `size` directly, which was exact but
  pivoted on the texture origin (the reported bug). Through the affine path they inherit
  D-BD-B consequence 1: a face carrying a negative size[0] comes back with a positive
  size[0], rotate turned 180 and size[1] re-signed - the same mapping, a different
  parameterisation. The pixels do not move; the numbers jump once.

- **Reset on patches still ignores the target subset.** `Patch_NaturalizeSelected`
  (pmesh.cpp:2742) works from the 3D selection and has no per-patch argument, so Reset's
  patch half re-naturalises every selected patch even when the canvas is targeting one.
  The face half honours the target set. Reset on a mixed selection is still two undo steps
  (Round BD, above).

- **The target sub-selection is keyed, not held.** It survives a repaint by re-matching
  (brush_t*, faceIndex) / patchMesh_t* against a fresh gather. A gather in which none of
  the stored keys appear falls back to "all" rather than to "nothing" - a canvas where no
  gesture does anything is worse than a canvas that edits too much.

- **The camera face-picker menu is unreachable, not deleted.** `CamWnd_ContextMenu` and its
  three WM_COMMAND handlers (camwnd.cpp) are ported code and are left byte-intact as
  reference; both call sites are fenced off (`// KIWI-UX (ROUND BG)`). If the menu is ever
  wanted back it is two comment lines. The XY and texture-window popups are different
  menus and were not touched.

## Round BH

The caulk workflow, immediate texture/UV apply, and Build & Run on the top bar
(`src/radiant/kiwi_caulk.h/.cpp` **new**, `camwnd.cpp`, `kiwi_uv.h/.cpp`, `kiwi_uveditor.cpp`,
`texwnd.cpp`, `kiwi_command.h/.cpp`, `kiwi_keymap.cpp`, `kiwi_launch.h/.cpp`,
`radiant_main.cpp`; RADIANT_UX_DESIGN 85). Nothing in this round was built or run - every
claim is from reading the tree and decoding the shipped assets, which is the standing rule.

**Deliberate limitations:**

- **A caulk apply is TWO undo steps, and it says so.** `Brush_SetTexture` and
  `Brush_FitTexture` each open and close their OWN undo record (select.cpp:1814-1815/:1879
  and :3675-3676/:3730), and `Undo_GeneralStart` ALWAYS allocates a fresh record
  (undo.cpp:367) - so an outer `KiwiCmd_UndoBegin` would be ORPHANED by the first core's own
  start, which is the exact trap kiwi_uv.h's UNDO RULING documents. Merging them would mean
  either re-spelling `Brush_FitTexture`'s body around the raw `Texture_Fit` / `sub_47C950`
  primitives (a duplicate of a ported function, i.e. the drift trap this codebase has been
  bitten by) or teaching kiwi_undo.cpp to coalesce tickets, which its own header rules out by
  name. So Ctrl+Z once undoes the fit and twice undoes the material, and the console prints
  one line per caulk apply saying exactly that. Applies to the End key and to a browser click
  on caulk alike.

- **Only caulk is auto-fit, and the match is the LEAF name.** `wc/caulk` and `caulk` both
  match; `caulk_something` does not. That is deliberately narrower than
  `Cam_EditorMaterialColor`'s substring table (camwnd.cpp), which may class a whole family as
  tool-coloured but must never make a texdef rewrite fire on one. A user who renames the
  shipped material loses the auto-fit; the End key would also stop finding it, and says so.

- **NODRAW changes appearance with caulk, and that is intended.** The item-4 gate is round
  M's `losesDepth` property - "writes depth at UNLIT where `d_white` does not" - and the
  shipped set answers TRUE for exactly caulk and nodraw (`refStateBits[1] = 0x0d`). Both now
  draw their own colormap at the face's real texdef in the unselected camera pass. Every
  see-through tool volume (clip / trigger / origin / hint / skip / portal / lightgrid_volume,
  all `0x0c`) is unchanged and still goes through `d_white`, so the round AZ/BA tool-volume
  route and the round M/T/Y depth-write settlement are untouched. No statemap was changed and
  no technique was overridden.

- **Item 4 does not touch the surf-cache or sun-approx world passes.** `Cam_DrawFaceCached`
  (RADIANT_SURFCACHE) never emitted the editor tint in the first place, and `Cam_DrawFaceSun`
  is the sun-approximation path; both are non-default. Only the DEFAULT immediate world fill
  was changed.

- **A texture click now ENDS the parked face gesture and immediately restarts it.** The
  gizmo blinks for one frame. The restart is what makes the apply stick (it takes a fresh
  `BeginFaces` baseline over the new material), and it is gated exactly as
  kiwi_boxselect.cpp's own auto-enter is - Face mode only, a live SEL_FACE active item,
  nothing else running - so it can never start a gesture the user did not have. In mode 5
  (EVERYTHING) there is no auto-enter and therefore no restart, matching the click grammar.

- **A live gesture that has APPLIED something and does not opt into `CanSwapTo` REFUSES a
  texture apply.** `CanSwapTo` defaults to false and `GestureMoved` defaults to TRUE
  (kiwi_command.h), so without that gate a live extrude / bevel / loft would be COMMITTED by
  a texture click. The refusal prints a line naming the command instead. Reaching this state
  needs a paused-but-applied non-transform gesture, which no ordinary mouse sequence
  produces; it is a guard, not a workflow.

- **The UV editor calls the gesture-end but ignores its refusal.** `UndoOpen` runs
  `KiwiUv_EndGestureBeforeApply` and then opens its bracket regardless, so a refusal leaves
  that window behaving exactly as it did before this round plus one console line. Refusing
  the edit outright would be a new way for the UV window to do nothing.

- **Build & Run is a MENU-BAR item, not a toolbar button, because this shell has no
  toolbar.** The `CToolBar` and `IDR_TOOLBAR152`'s BITMAP/TOOLBAR resources went out with the
  MFC rip (radiant_main.cpp step 1b, res/radiant.rc:12-13) and `res/toolbar.bmp` has had no
  consumer since. A real `TOOLBARCLASSNAME` child would have to be carved out of the frame
  client that IS the ImGui dockspace surface, and the whole layout re-inset for it - a
  structural change, not a button. The bar item sits at the right end, after the two existing
  KIWI popups, where it cannot move any POPUP INDEX the index-based consumers read.

- **The Build & Run bar caption carries no key text of its own.**
  `Radiant_ShowMenuItemKeyBindings` searches BY COMMAND ID through the whole menu tree, a
  menu-bar item included, and re-runs on every keymap-profile switch - so it will annotate
  this row to "Build & Run\tF9" itself. Writing "(F9)" into the caption as well would name the
  key twice after the first switch. Consequence: at boot the bar reads "Build & Run" with no
  key, and after a profile switch it reads with one. Cosmetic, and self-correcting in the
  direction of MORE information.

- **End displaces View->Center in the MODERN profile only.** vk `0x23` had exactly one row
  in the whole default table (`{ "CenterView", 0x23, 0, 32953 }`, mainfrm.cpp:1155); the
  directive says End wins, so Center View moves to Shift+End there. A user on the classic
  profile keeps End = Center View and reaches Caulk Selection from the palette only. A user
  who had remapped End in `radiant.ini` still loses it to the profile, exactly as every other
  modern-profile claim works.

## Round BI

The UV editor's interaction rework v2 - the controls come off the grid lines and onto the
shapes (`src/radiant/kiwi_uveditor.h/.cpp`; RADIANT_UX_DESIGN 82.2). Nothing in this round
was built or run - every claim is from reading the tree, which is the standing rule. The
write paths from rounds BD/BG/BH are untouched; only the gesture and selection layer above
them was replaced.

**Deliberate limitations:**

- **A transform box under 24 px on either axis carries NO handles**, so scale / rotate /
  skew are unreachable for a target set that small until the wheel makes it bigger. Eight
  14 px pick squares on a 20 px box would cover the box completely and delete the BODY DRAG,
  which is the gesture the round exists for. The box still draws, the body still drags, and
  `BoxHasHandles` is the single function both the pick and the draw ask, so the handles can
  never be drawn where they cannot be grabbed.

- **The box is an AABB with no rotation frame, so it BREATHES during a rotate.** It is
  recomputed every frame from the targeted outlines' displayed bbox; a rotating shape's
  axis-aligned bbox grows and shrinks. The gesture is unaffected (it is measured from the
  press, not from the box), and the box is correct again the moment the drag ends. Carrying
  an oriented box would mean storing a rotation per target set and re-deriving it after every
  numeric edit, undo and re-gather.

- **The rotate cursor is a HAND.** ImGui 1.92's cursor set has no rotate shape (Arrow,
  TextInput, ResizeAll, ResizeNS, ResizeEW, ResizeNESW, ResizeNWSE, Hand, NotAllowed), so the
  corner annulus and the pivot share it. They never overlap - the annulus is strictly outside
  the box and the pivot disc is inside it.

- **A press in the box's empty interior means two different things**, and it is a
  reconciliation rather than a mode: with a sub-selection live it drags the set (the brief's
  "inside the box but on no handle"), with no sub-selection it rubber-bands (the brief's
  "empty canvas"). The rule is stated in D-BI-C and both readings end at the same place -
  a zero-distance press there restores "all".

- **Repeated presses cycle DOWN the stack and wrap, with no visible list.** The only feedback
  is the white outline the hover draws around the shape the next press would take, plus the
  "targeting 1 of N shapes here" status line. There is no popup naming the stack, and no
  modifier for "the one under that": at 4 px the cycle anchor is tight enough that ordinary
  aiming does not accidentally advance it, and any press further away starts at the top again.

- **The snap candidate sets are frozen at the press and capped at 1024 points / 1024 edges
  each.** A selection larger than that snaps against a prefix of its own outlines - the cap
  is per set, so both the dragged points and the other shapes' vertices keep their first 1024.
  Ctrl still disables snapping entirely, and the grid remains a candidate regardless of the
  cap because it is computed, not enumerated.

- **The pivot follows the target set until you drag it, and then stops for the rest of that
  3D selection.** There is no UI to un-latch it other than changing the selection (or Reset,
  which does not touch it). That is the price of "recomputed when the target set changes,
  unless the user has dragged it".

- **Everything §82.1 listed as a limit still holds**: snap candidates offered by the fold-out
  are still not read from folded neighbours' own frames, the world-edge weld is still a
  quantised bucket match, the fold still joins edge DIRECTION and one endpoint rather than
  length, past 256 faces the chain is not built, Rot 90 / Flip still re-parameterise a
  negative `size[0]` through texturevecs_02, and Reset's patch half still ignores the target
  subset (`Patch_NaturalizeSelected` takes the 3D selection).

## Round BJ

End-key caulk semantics plus six UV-editor refinements, all from user reports
(`src/radiant/kiwi_caulk.h/.cpp`, `kiwi_uveditor.h/.cpp`, `kiwi_undo.h/.cpp`, one rung in
`kiwi_command.cpp`; RADIANT_UX_DESIGN 82.3). Nothing in this round was built or run - every
claim is from reading the tree, which is the standing rule. `csg.cpp` was NOT touched: the
AUTO half reaches the ported `Brush_AutoCaulk` through command id 33220, which already
carries its own undo bracket.

**Deliberate limitations:**

- **The caulk verb is still several undo records, and now it depends on the arm.** FACE
  selection: TWO (the apply, then the fit - D-BH-C's ruling is unchanged, and both cores
  open their own bracket). SOLID selection: ONE ("Auto caulk"). MIXED: THREE. The brief asked
  for one bracket; it is not reachable without re-spelling `Brush_SetTexture`,
  `Brush_FitTexture` or `Cmd_OnSelectionAutoCaulk` bodies, which is the duplicate-of-a-ported-
  function trap `kiwi_undo.h` and `kiwi_caulk.h` both rule out by name. The console line says
  which arm ran and what it did.

- **The AUTO arm does not fit the faces it caulked.** `Brush_AutoCaulkFace` is the only thing
  that knows which faces it converted and it returns nothing, while `Brush_FitTexture` takes
  the SELECTION - so fitting afterwards would re-fit every face of every selected solid and
  destroy hand-aligned texdefs on the faces that are still visible. An auto-caulked face is by
  definition one you cannot see, so the fit buys nothing there.

- **The AUTO arm's face count is MEASURED, not reported.** `Brush_AutoCaulk` prints
  "%i faces caulked" and returns void; the number the verb prints is the difference between a
  non-caulk face count taken either side of the call. It agrees with the ported line in every
  ordinary case and can disagree if something else changes a material during the call.

- **The MIXED arm does not give the face gizmo back.** It runs the ROUND BH handshake once at
  its head (a parked face push holds an index into a selection that arm replaces twice), so
  the browser funnel's own re-arm at the tail finds nothing to restore. The FACE arm - the
  common case and the reported one - keeps its gizmo.

- **A whole-brush selection made only of patches or fixed-size entities takes the PLAIN arm.**
  `Brush_AutoCaulk` skips both (`Brush_IsFixedOrPatch`), so there is no hidden-face test to
  run; those keep round BH's plain caulk + fit rather than silently doing nothing.

- **The UV target set derived from the 3D face selection does not include patches.** A patch
  is not in `g_SelectedFaces`, so a mixed face+patch selection arrives with the faces targeted
  and the patch dim; one click on the patch (or Esc) brings it back.

- **A targeted patch draws GOLD, so it loses the green family tell while it is targeted.**
  Item 3 asked for everything selected to be yellow; the control net still reads as "patch"
  at a glance.

- **The ACTIVE face no longer has an outline of its own.** It survives as the material that
  paints the background and as the chain-fold anchor, both named in the readout, but there is
  no longer a way to see WHICH gathered face is the anchor at a glance.

- **The fold layout is LATCHED and can drift into overlaps.** It is solved once per (gathered
  rows and their order, active face, Chain toggle) and never re-solved because a texdef
  changed - that is the whole of the item-7 fix. After heavy editing the arrangement can
  therefore overlap in ways a fresh solve would have avoided; the "Re-fold" toolbar button is
  the way to re-solve, and it is a deliberate decision rather than a side effect.

- **The scale anchor is the opposite handle, and there is NO modifier for pivot-anchored
  scaling.** Alt is already SKEW on the edge mids (D-BI-B), so an Alt rule would have covered
  corners only, and a half-rule in a direct-manipulation surface is worse than no rule.
  Dragging the pivot onto the opposite corner makes the two coincide.

- **The snap accent marks the ABSORB only.** The whole-texel rounding that `SnapDeltaAxis`
  applies outside the 8 px band is not marked: at 128 px/repeat one texel of a 512-wide
  material is a quarter of a pixel, and marking it would light the accent permanently.

- **The rotate snap gets a status clause and no dot.** It locks an ANGLE, not a point, so
  there is no place on the canvas a mark would honestly name.

- **The Ctrl+Z face restore is scoped to KIWI-created records that held a FACE selection.**
  An object-only KIWI gesture and every ported record keep the classic behaviour exactly. The
  restore is also skipped while a modal command is live, and it is DISCARDED (leaving the
  ported undo's own selection) whenever no face key resolves - which is what happens after a
  KIWI op that adds or removes brushes, because the bounds+faceCount fingerprint no longer
  matches anything.

- **The restore's brush fingerprint is bounds + face count, matched first-unused-wins in list
  order.** Two exactly coincident brushes with the same face count are told apart only by the
  ordinal, so a record covering such a pair can in principle restore the faces onto the wrong
  member. The face-plane check catches every case where their geometry differs at all.

- **Everything rounds BI / BG / BD listed as a limit still holds** except the two this round
  fixed: the fold no longer re-solves on a texdef change, and the pivot's snap candidates are
  no longer read from the active face's un-displayed ST frame.

## Round BK

Eight user reports: the patch wireframe overlay, the line tool's RMB, trim granularity, the
region fill (sixth attempt), axis-locked creation, the gizmo snap chain, Space, and the ortho
arrow fly. Touched `pmesh.cpp`, `camwnd.cpp`, `kiwi_region.cpp`, `kiwi_construct.cpp`,
`kiwi_trim.cpp`, `kiwi_primitive.cpp/.h`, `kiwi_snap.cpp/.h`, `kiwi_transform.cpp/.h`,
`kiwi_focus.cpp/.h`, `kiwi_camera.cpp/.h`, `kiwi_command.cpp/.h`, `kiwi_keymap.cpp`
(RADIANT_UX_DESIGN 86). No new files, so `radiant_files.cmake` is unchanged. Nothing in this
round was built or run - every claim is from reading the tree, which is the standing rule.

**Deliberate limitations:**

- **THE REGION FILL IS A ROUTE SWAP, NOT A PROVEN FIX.** The evidence is strong (the donor is
  a translucent world-space polygon the user asked for and has not complained about since
  round BC, and the four channels that differed are named in 86.4), but nothing in this tree
  can prove a shader's output. What IS new is that the pass now prints on SUCCESS once per
  store edit - route, piece count, vertex count, first world vertex, colour, bracket. If the
  fill is still invisible, that line is the next round's starting evidence; if the line does
  not appear at all, the pass is not running and that is a different bug.

- **The region fill's per-region MATERIAL_COLOR is a push per region, not per pass.** Regions
  are few and the push is four floats, but a sketch with dozens of closed loops now emits
  dozens of `R_AddCmdSetMaterialColor` commands per frame where it used to emit two.

- **A convex piece over 64 vertices is refused with a named gate.** The donor's stack arrays
  are `CAM_MAXFACEVERTS` (64) and it returns SILENTLY past that, which is exactly the drop
  class this pass exists to name - so the cap is checked on this side. Hertel-Mehlhorn only
  merges while the result stays convex, so reaching it needs a 64-corner convex region, which
  the KCON tools cannot produce; the check is there because "not reachable" is how the last
  five rounds of this bug were argued.

- **`KiwiTris_FillNormal`'s constant +Z is STILL what every other kiwi fill writes.** The
  hover face tint, the extrude/split/loft previews and the gizmo heads were NOT moved to the
  donor route this round. If the region fill comes back visible, they are the follow-up and
  the change is mechanical; if it does not, moving them would have been five more guesses.

- **The patch fix is an ORDER change and cannot help a genuinely one-sided patch.** A single
  sheet seen from behind still draws as a wireframe grid and nothing else - that is stock
  `DrawPatches`, it is what round AY's Two-sided default exists to answer, and this round did
  not touch it.

- **The `sortKeyBias` argument makes a biased pass FLAT in the layer index.** A multi-layer
  patch's back-face wireframe is emitted at one key rather than one per layer. The wireframe
  is the same hairline grid whatever layer emits it, so nothing is lost, but a future reader
  expecting per-layer keys on that pass will not find them.

- **RMB-ends-without-placing is the CURVE tool only.** The spline tool (`KiwiSplineTool`)
  chains points the same way and still takes the cursor point on an RMB finish. The brief
  scoped this to the line/curve tool; extending it is one line in the same shape if the same
  report comes back for splines.

- **Trim's vertex cut set applies ONLY when the crossing set is empty.** A polyline that HAS a
  crossing still trims to its crossings, so "delete one segment" and "trim the overhang" are
  different answers on the same object depending on what else is in the sketch. That is
  deliberate (round T's fix must not be undone) but it is not one uniform rule.

- **A closed loop with EXACTLY ONE crossing is still refused**, with round AF's console line.
  Zero crossings now takes the segment; one crossing is the ill-defined case and keeps its
  explanation.

- **The auto-height fires only at height-stage ENTRY.** Enter the height stage from a workable
  angle and then orbit INTO the top/bottom lock and you get round AI's held-height behaviour
  and its "orbit away, or type one" refusal, exactly as before. Only the stage-entry case
  completes itself.

- **`ArmTopFacePush` finds the cap by NORMAL, at better than 45 degrees.** A primitive whose
  top is not a single planar cap - the sphere (which never reaches the height stage anyway)
  and the EXPERIMENTAL patch cylinder (explicitly skipped, `inst->patch`) - arms nothing and
  leaves the solid selected. A cone's apex is a point, not a face, so a cone auto-created this
  way arms the face nearest the height direction, which is a side face when the cone tapers to
  a point; the push then moves that side.

- **The landed brush is found as `selected_brushes.next`.** That is sound because `Create()`
  runs `Select_Deselect(1)` immediately before `Brush_AddToList2`, but it is an assumption
  about the landing order rather than a value returned by the creator.

- **`SNAP_FACE` is now a 14 px magnet, and that IS a behaviour change for round AA ITEM 5.**
  "X-lock this brush until it meets that wall" now engages when the drag is within 14 screen
  pixels of the wall's plane rather than from any distance. That is the intended trade (the
  gizmo has to track the cursor), but a user who relied on the long-range snap will notice.

- **The grid magnet is a BAND, not a quantiser** - round AG's D-Z2 ruling, unchanged. "Snap to
  the grid" with the gizmo means the value locks when it comes near a lattice line, not that
  every drag steps. Ctrl still runs the full ranked query, and the AJ grid-snap checkbox still
  disables both the quantiser and the band.

- **The major-line preference reads `KSNAP_MAJOR_STRIDE` (10) as a constant**, matching the
  grid draw's own `i % 10 == 0`. If the grid's major stride is ever made configurable, the two
  will drift and the magnet will stick to lines the user cannot see.

- **The snap dots are drawn for ONE face - the one under the cursor.** During a gizmo drag the
  cursor is often over the object being moved, which `PICKF_EXCLUDE_SELECTED` removes from the
  pick, so the dots will show the face BEHIND the selection. That is the correct target set
  (you cannot snap to what you are dragging) but it can read as "the dots are on the wrong
  object".

- **No bbox-centre dots.** The directive listed "centers"; face centres are drawn (arm 4b) and
  object bounding-box centres are not, because no snap arm answers one and this function's own
  rule forbids advertising a target the query cannot honour.

- **Space displaces Clone to Shift+Space in the MODERN profile.** A user with muscle memory
  for Space-to-clone will duplicate nothing and re-aim the camera instead. The console prints
  what View Face did, and Shift+D is the modern duplicate.

- **`View Face` clamps a floor/ceiling face to 89 degrees of pitch**, because
  `KiwiCam_LookAlong` does (a true +/-90 pitch has no yaw). One degree off vertical is not
  visible and it is the same limit the view cube's top/bottom already carry.

- **`View Face` does NOT swap the construction plane or the projection**, unlike Plasticity's
  `viewport:navigate:selection`. Both are explicit user-facing controls in KIWI and reaching
  into them from a view key would be a surprise.

- **THE ORTHO SECTION IS A CLIP, NOT A CUTAWAY.** A brush the plane passes through shows its
  interior as open - there is no cap polygon and no section hatching. That is what a CAD
  section view without capping looks like, and adding capping is a renderer feature, not a
  camera one.

- **THE SECTION PLANE IS ANCHORED TO THE EYE, NOT TO THE PIVOT, so it takes a moment to
  bite.** In ortho the eye sits `s_dist` behind the pivot (`s_dist` is about 2.09 half-heights
  at the default fov), so after the first forward press nothing disappears until the eye has
  flown up to the wall - about half a second at the default 4x fly scale, longer if the slider
  was turned down. That is deliberate: anchoring the plane at the PIVOT instead would need
  `KiwiCam_OrthoBackDepth` to return a NEGATIVE depth (a near plane in FRONT of the eye), which
  changes the sign of `m[3][2]`, inverts the pick-lead clamp and interacts with the
  zero-thickness guard - four things at once, in a round that cannot be built. If "it takes too
  long to cut in" comes back, that is the change, and it is a contained one.

- **`kiwi_pick.cpp`'s screen-space vertex/edge pass has no behind-the-eye rejection in ortho.**
  Already true and already recorded at `KCAM_ORTHO_PICK_LEAD`; with a section armed it means a
  VERTEX or EDGE of clipped geometry can still be picked even though its FACE cannot (the ray
  lead now stops at the cut). Two picks can therefore disagree about geometry the section has
  removed.

- **The section has no readout but the console line.** No pill, no HUD clause, no status bar
  entry. If the user flies forward, walks away and comes back, the only evidence that geometry
  is being clipped is the scrollback.

- **The ortho fly rate is now zoom-relative, which changes the FEEL of the existing left/right
  strafe.** Zoomed far in it is slower in world units than the old fixed 1400 u/s; zoomed out
  it is very much faster. That is the point (a fixed world rate is invisible zoomed out) but it
  is a change to a control the user did not report as wrong.

- **Nothing in this round changes the PERSPECTIVE camera.** The section, the rate change and
  the pick-lead clamp are all inside `KiwiCam_Ortho()` arms.

## Round BL

Four items, three of them RE-REPORTS of round BK fixes that did not work in the user's build
(outliner collapse on load, trim granularity, the construction-face fill, the pivot/grid snap
during a move). Touched `kiwi_outliner.cpp/.h`, `map.cpp`, `kiwi_trim.cpp/.h`,
`kiwi_region.cpp/.h`, `kiwi_viewport.cpp`, `kiwi_snap.cpp/.h`, `kiwi_transform.cpp`. No new
files, so `radiant_files.cmake` is unchanged. Nothing in this round was built or run - every
claim is from reading the tree, which is the standing rule.

**What the three autopsies found (the short form; the long form is in the code):**

- **TRIM.** Round BK computed the hovered segment's bounds but only USED them inside
  `if ( cuts.empty() )`. A triangle finished with Enter/RMB rather than by clicking its first
  point is stored as an OPEN four-point chain `v0,v1,v2,v0` (`kiwi_construct.cpp:3187-3192` is
  the only thing that sets `closed`; `NormalizePoints` deliberately does not weld an open
  chain's seam, `:977-988`), whose first and last segments share `v0` - so `GatherCuts` records
  crossings at parameters 0 and `total`, `cuts` is NOT empty, and the round-T span logic's
  fallback bounds (0 and `total`) could not be improved by them. The span became the whole
  object. The clamp is now an invariant for LINE / POLYLINE / RECT.

- **THE FILL.** No readable gate closes. The autopsy could NOT name the defect from the source,
  and says so: the route swap is now moot because the fill is additionally drawn in the ImGui
  camera overlay.

- **THE MOVE.** The cursor->selection mapping is RELATIVE (`d = p - m_mapStart`) and always
  was, so the gap between the cursor and the pivot at the instant of the grab is carried for
  the whole drag; round BK's answer to "the gizmo centre should track my mouse" was applied
  inside the SNAP arm, which is downstream of that mapping. And every snap band in the command
  is measured around the PIVOT, so the user was aiming with a cursor that was not the thing
  being snapped - which is why "snap to the major line" felt impossible even where the magnet
  was firing.

**Deliberate limitations:**

- **THE OVERLAY FILL HAS NO DEPTH TEST.** This is the big one and it is the accepted price of
  a fill that is guaranteed to exist. `KiwiRegion_DrawFillsOverlay` draws screen-space polygons
  on the HUD's own `ImDrawList`, so a region BEHIND a wall shows through it and a region is
  drawn OVER geometry in front of it. If the engine route (`KiwiRegion_DrawFills`, kept and
  unchanged, console line and all) is ever proven to work, the overlay can be demoted at its
  single call site in `KiwiVP_DrawCameraOverlay`.

- **A REGION WITH ANY VERTEX AT OR BEHIND THE EYE PLANE IS DROPPED WHOLE, not clipped.**
  `Pick_WorldToImage` refuses such a point (it cannot express it), and drawing the survivors
  would wrap the polygon round the viewport. In ortho this never fires; in perspective it means
  a fill you are standing inside blinks out. Honest and cheap; real near-plane clipping in
  screen space is a bigger change than this round can justify.

- **The overlay fill is capped at 96 loops per frame** (`KREG_OVERLAY_MAX_LOOPS`) and at
  `KREG_MAX_LOOP` (128) vertices per loop - the derivation's own cap. A sketch past that draws
  its first 96 regions.

- **The overlay draws the OUTLINE even when the ear clip refuses the loop**, so a
  self-intersecting region shows its boundary and no fill. That is more information than the
  engine route gives (it drops the region silently and counts a gate).

- **THE PIVOT NOW JUMPS ONCE, AT THE GRAB.** "Keep the pivot in line with the cursor" is not
  free: re-basing the offset means that grabbing a gizmo arrow 50 px along its shaft, or
  grabbing a large brush anywhere but its centre, moves the selection by that offset on the
  first live frame of the drag. It is one jump per grab (and per axis-key constraint change),
  it only ever moves the components the constraint OWNS, and after it the drag is
  frame-for-frame what it always was. This is the behaviour the user asked for in the words
  they used; if the jump reads worse than the offset did, the rebase is one `if` to remove.

- **Changing arrows mid-gesture re-pins that axis to the cursor.** Round AN's "a new constraint
  continues from where the old one left off" is preserved for the axes the new constraint does
  NOT own (that is where the previous arrows' travel lives), but the newly-owned axis is
  re-based onto the cursor, so it jumps.

- **The object move's grid arm is now `KiwiSnap_LatticeAxis( ..., hard = true )` rather than
  `KiwiGrid_Snap`.** Per axis the arithmetic is identical (absolute round to the nearest cell at
  `KiwiUnits_GridSpacingWorld`, master switch respected) EXCEPT that a MAJOR line within 0.75 of
  a cell now wins. So a hard-quantised drag can land three quarters of a cell from where it used
  to, on a line the user can see, and that is the point of the item. The band is HALF a cell on
  purpose: at a full cell the major would own +-1.5 cells and the two cells either side of every
  major would become unreachable.

- **`m_majorLock` is HUD-only and one frame deep.** It says a major took an owned axis THIS
  frame; it is not latched, so releasing on a major and then nudging clears the lamp without
  moving anything.

- **The outliner collapse is a ONE-SHOT armed by `Map_LoadFromFile`, not a preference.** File >
  New does not arm it (there is nothing to collapse), and a map loaded while the panel is
  closed collapses on the panel's next draw, whenever that is. Manual expand/collapse still
  persists for the session exactly as before - the default for an untouched key is still OPEN.

- **`AutoExpandForSelection` still wins afterwards.** Selecting anything re-opens its ancestors,
  including the Solids section, on the very next draw. That is round W's rule and this round
  only orders itself after it for the load frame.

## Round BM

Camera properness pass, section analysis, the zoom meter. `RADIANT_UX_DESIGN.md` Part XIII carries
the CAMERA CONTRACT, the disposition table for every round-BK camera change, and the derivations.

### The clip plane (section analysis)

- **A CLIP IS NOT A CUTAWAY.** The section is a D3D9 user clip plane, so the cut faces are OPEN:
  you see the inside of the far wall and the back of the far side's texture, not a capped solid.
  Synthesising the cross-section polygon needs a stencil-capped pass (draw the world with the
  plane, then fill the stencil-marked area with a flat colour), which is a renderer change of a
  different size. Every real-time section short of that looks like this one.

- **BACKFACE CULLING SHOWS THROUGH THE CUT.** Faces whose material culls back-faces vanish rather
  than showing their reverse when the section opens them up. That is the material's own cull state
  (`kiwi_lines.h` TRAP 3 records the same fact for the fill layer), not something the section
  chooses, and it means a sectioned room can look "half missing" where a wall's inside face is
  culled. The world brush faces the editor draws are two-sided in practice; misc_models and prefab
  content may not be.

- **ONE PLANE, NO FLIP.** The section keeps one plane and cuts the `dot(n,p) > d` half. There is no
  "flip" control in v1: to cut the other way, toggle the section off and click the opposing face.
  A flip is one line of state whenever it is asked for.

- **NO NUMERIC ENTRY FOR THE SLIDE.** The section is deliberately an INSTANT command with view
  state rather than a modal command (a modal one would own the active-command slot and block every
  modelling verb for as long as the section was useful — see UX_DESIGN §33), and `KiwiNum_*` is
  reached through the active command. Dragging the lollipop is the only way to set the level. The
  cheap fix if it is wanted is a type-in on the section button, the round-R grid-pill pattern.

- **THE SLIDE NEEDS AN ANGLE.** Dragging the lollipop maps the cursor through `KiwiCam_RayAxis`,
  which refuses samples inside the round-AI 14-degree cone (`KCAM_RAYAXIS_MIN_DEN`). Looking
  straight down the section normal therefore cannot slide it, and the console says so on the press
  rather than starting a drag that will not track. Orbit a little and it works.

- **A HIDDEN VERTEX CAN STILL SUPPRESS A VISIBLE ONE.** The screen-space vertex/edge pick tests
  the WINNER against the plane, not every candidate (one plane test per pick instead of one per
  vertex). If a clipped-away vertex and a visible one are both inside the pixel tolerance and the
  hidden one is nearer the cursor, the pick falls through to the area pass instead of taking the
  visible vertex. Falling through to the surface hit is the honest answer in that case, and the
  cost of the alternative is a plane test per candidate on every hover frame.

- **THE SECTION SURVIVES A MAP LOAD.** It is session view state, like the UV editor's origin and
  the projection pill, and nothing clears it on File > Open. If a freshly loaded map looks like it
  is missing geometry, check whether the SECTION button above the cube is lit.

- **PICKS ARE CLAMPED, THE 2D VIEWS ARE NOT.** The clamp lives in `Pick()`, which is the 3D
  camera's picker. The XY/Z views use the ported `Test_Ray` paths directly and are unaffected by
  the section in both render and pick — deliberate (the section is a 3D-camera analysis aid), but
  it does mean a brush the 3D view has cut away is still selectable in the 2D views.

- **THE CLIP COMMAND IS DROPPABLE, AND THE WORST CASE IS ONE OVER-CLIPPED FRAME.**
  `RC_SET_CLIP_PLANE` sits above `RC_FIRST_NONCRITICAL`, so a full render-command buffer (which a
  big map reaches routinely) may refuse it. The bracket is ACKNOWLEDGED rather than merely matched
  — `R_AddCmdSetClipPlane` reports the drop, `kiwi_section.cpp` tracks the believed device state,
  and the head of every camera frame re-issues a pending disable before any geometry — so a dropped
  disable costs one frame in which the ImGui overlay is clipped by the plane, and then heals. It
  cannot become permanent.

- **DEVICE CAPABILITY IS NOT PROBED.** If a device supported no user clip planes at all,
  `SetClipPlane` / `D3DRS_CLIPPLANEENABLE` would be ignored and the section would draw its plane
  and its handle while clipping nothing. Every D3D9-class part supports at least six, so this is
  logged rather than guarded.

### The camera

- **THE ORBIT RATE IS NOW RESOLUTION-RELATIVE** (360/viewport-height degrees per pixel, Plasticity's
  `2*PI*delta/clientHeight`). At the shipped ~1030 px camera image that is 0.35 deg/px, i.e. the
  old constant, so the common case is unchanged — but a user with a very small or a very large
  camera pane WILL feel a difference from the pre-BM build, and it is the intended one. Mouselook
  (Alt+RMB) deliberately keeps the flat 0.35: it is the classic free-look and mirrors the ported
  `CamWnd_Rotate2`.

- **THE PAN REFERENCE MOVED BACK TO THE ORBIT PIVOT** and no longer picks the surface under the
  cursor at the press. In ORTHO this cannot change anything (world-per-pixel is depth-independent
  there). In PERSPECTIVE, panning while looking at something much nearer or much further than the
  pivot now tracks at the PIVOT's rate rather than that surface's — which is exactly what
  Plasticity does (`OrbitControls.ts:265-275`), and what shakeout E moved away from for a reason
  (a pivot that wandered on every wheel notch) that the new ortho dolly removes.

- **THE ORTHO ARROW KEYS PAN; THEY DO NOT WALK THE EYE FORWARD.** Up/Down move the view along
  screen-up, Left/Right along screen-right, at a fixed 600 px/s scaled by the fly-speed slider. In
  a parallel projection walking the eye along the view axis changes nothing visible, which is what
  round BK reached for a clip plane to paper over; interiors are the section's job now. The
  PERSPECTIVE arrows are unchanged (forward/back along `vpn`, strafe along `vright`).

- **THE FLY-SPEED SLIDER IS NORMALISED IN ORTHO.** The ortho rate is
  `600 px/s * (flyScale / 4.0)`, so at the shipped default the factor is exactly 1 and doubling the
  slider doubles both the ortho and the perspective fly. A user who had previously set the slider
  to compensate for round BK's runaway rate will want to put it back to 4x.

- **`s_dist` AND THE REAL EYE-TO-PIVOT DISTANCE CAN DIVERGE DURING A LIVE ORBIT.** The ortho dolly
  re-seats the eye at `lookAt - vpn*s_dist` only when no orbit is latched, because re-seating with
  an off-axis latched pivot is exactly round P's JUMP 1. Wheeling while holding MMB therefore
  leaves the two slightly out of step until the next notch after the release restores the
  invariant. Nothing visible follows from it in ortho (the standoff does not set the scale there);
  toggling to PERSPECTIVE at that exact moment would use the recorded `s_dist`.

- **ROUNDS AJ/AN'S NEAR-FIELD DOLLY EASES ARE GONE.** The per-notch step is a flat 0.95
  (Plasticity's) at every distance and the cursor aim is the full cursor ray at every distance. If
  zooming in ever reads as "squirrely" again, that is a REGRESSION against UX_DESIGN Part XIII's
  diagnosis and not a request for another ease — the ortho arm's reference is the thing to check.

## Round BN — known limits and accepted trade-offs

- **THE SNAP OCCLUSION GATE CAN REFUSE A TARGET YOU CAN SEE THROUGH A HOLE.** A vertex, edge or
  construction point that lies further down the cursor ray than the surface under the cursor is no
  longer a snap candidate (kiwi_snap.h THE SNAP LADDER, RE-STATED). That is the fix for the
  reported teleport, and it has one honest cost: geometry visible through a doorway, a window or a
  gap in the mesh IS behind a surface for rays that pass through the frame, so aiming at it from
  outside will fall through to the surface or the grid instead. Three ways round it, all
  pre-existing: use **Ctrl** to invert the context's snapping (round BO: held = snap in a transform,
  held = free in a construction tool — the SNAP CONTRACT in `RADIANT_UX_DESIGN.md` §35),
  **hide/isolate** the occluder (H / Shift+H), or arm **Section Analysis** and cut
  the wall away. A drawing tool placing on its own construction plane is already exempt when that
  plane sits BEHIND the surface.

- **THE PERSPECTIVE GRID IS A PATCH OF LATTICE AROUND THE PIVOT, NOT A HORIZON.** Its reach stops
  where its own cells stop resolving, so at a grazing pitch you get a handful of major lines and
  then nothing, and at a steep pitch it covers roughly the same ground the ortho window would.
  There is no far ring in perspective. This is deliberate (kiwi_grid.h) and it is why the toggle is
  opt-in and off by default; the ortho lattice is unchanged and is still the accurate one.

- **THE PERSPECTIVE GRID'S SPACING FOLLOWS THE ORBIT PIVOT, NOT THE CURSOR.** Dollying without
  moving the pivot can leave the tier one step coarser or finer than the near field wants. The
  Schmitt band (KGRID_PX_REFINE / KGRID_PX_MIN = 2.44) still makes it impossible to oscillate.

- **A VERY DENSE CURVE LOFT TRUNCATES ITS OWN PREVIEW.** `LineBudget` asks for
  `stations * ring * KLOFT_CURVE_PREVIEW` segments and the framework clamps at
  `KCMD_LINE_BUDGET_MAX`, so past a few hundred segments the red/blue preview stops drawing
  mid-shape while the COMMIT still produces the whole loft. The commit gate
  (`KLOFT_MAX_CURVE_PATCHES`) is what refuses a loft that is genuinely too big, and it refuses in
  words before you press anything.

- **"INTERIOR VISIBLE" (was "Two-sided") DEFAULTS OFF AND ITS PROFILE KEY MOVED.** The old
  `KiwiLoft/TwoSided` value is ignored; the new key is `KiwiLoft/TwoSided2`, default 0. A user who
  had turned the old one on to work around the backwards winding gets the fixed single-sided loft
  instead of the workaround. Lofts BUILT under the old default keep both sheets — nothing rewrites
  existing geometry — so an old map still has coincident patches on its curve lofts, and deleting
  the inward-facing copy is still a manual cleanup.

- **THE UV SCOPE LATCH IS ONE FRAME BEHIND.** `KiwiUvEd_InScope()` is computed inside the UV
  editor's ImGui pass and read by `Cam_Draw`, so entering or leaving scope takes effect on the next
  frame. At 60 Hz this is not observable; it is written down because the alternative (asking ImGui
  for focus/hover from the render pass) is not answerable at all.

- **THE UV SCOPE HIDES THE SELECTION OVERLAYS, NOT THE SELECTION.** Nothing about the selection
  changes, and a brush or face that is not in the UV gather keeps every overlay it had. The
  selected-brush RED TINT is dropped as a COLOUR only — the brush still draws, because that pass is
  the only thing that draws a selected brush's textured surface.

- **SPACE AND `/` NOW REACH A PARKED FACE GESTURE; THEY STILL DO NOT REACH A LIVE DRAG.** The new
  funnel rung is gated on `PreemptIdle` (auto-entered, nothing applied) and on an EMPTY numeric
  entry. Mid-drag, or with a value half-typed, both keys are still swallowed — deliberately: a
  camera re-aim during a cursor-mapped drag would teleport it, and Space inside a typed value is
  part of the value.

- **THE LINE TOOL'S "RMB PLACES A POINT AND ENDS" ARM IS REACHABLE ONLY ON AN LMB-FREE CHAIN.**
  Today the curve tool's only route into `PushPoint` is `OnClick` (LMB), so in practice that arm is
  the EMPTY chain — where the placed point makes a one-point chain and `Finish` still refuses it
  with "needs at least two points". The latch is written as the directive states it
  (`m_anyLmbPoint`), so it stays correct if a non-LMB placement route is ever added; if the intent
  was that RMB should place the FINAL point of a chain started with LMB, that is the opposite rule
  and needs saying.

## Round BO — known limits and accepted trade-offs

- **THE SECTION MAY STILL CUT NOTHING, AND NOW IT SAYS SO.** The round-BO autopsy proved every
  software link in the chain correct — the emit, the command-table entry, the enum value, the view
  install, the clip-space transform (worked example in `RADIANT_UX_DESIGN.md` §36) — and found no
  state clobber anywhere in either tree. What could not be settled by reading is whether the DEVICE
  applies a D3D9 user clip plane to the shaders this build binds. That is now probed: the first
  enable of a session logs `SECTIONPROBE:` to `%TEMP%\radiant_firstlight.log` with both HRESULTs,
  the `D3DRS_CLIPPLANEENABLE` read-back and `gfxMetrics.maxClipPlanes`, prints the same to the
  console, and warns if the device did not accept it. If the section still cuts nothing while the
  probe reports success and a non-zero read-back, the runtime/driver is declining to apply user clip
  planes to these draws and the fix is a different MECHANISM (an oblique near-plane projection, or a
  shader-side clip distance) rather than a repair to this one. That is a renderer change of a
  different size and it is deliberately not being guessed at untested.

- **A DROPPED CLIP COMMAND IS NOW VISIBLE — AND IT WAS ALWAYS POSSIBLE.** `R_AddCmdSetClipPlane`
  used to return `true` even when the command was silently discarded, because the editor's
  `R_GetCommandBuffer` overflow arm hands back a scratch slot instead of NULL. Round BM's
  "acknowledged bracket" therefore never acknowledged anything. With the return fixed, the repair it
  described actually runs — but the first drop on a huge map now prints a one-shot console line
  saying the section is not cutting this frame. It is a real limit of the 48 MB command buffer, not
  a new bug.

- **`R_AddCmdSetClipPlane` IS THE ONLY EDITOR COMMAND THAT REPORTS ITS OWN DROP.** Every other
  non-critical adder in `r_rendercmds.cpp` still returns `void` and still cannot tell its caller
  that the editor overflow arm swallowed it. Nothing else currently needs to know; if another
  feature ever does, the `s_cmdList->lastCmd` discriminator is the pattern to copy.

- **CTRL NO LONGER MEANS "DON'T SNAP" ANYWHERE IN A TRANSFORM, AND THAT IS A RELEARN.** Every drag,
  push/pull, extrude, gizmo axis drag and UV gesture is RAW until Ctrl is held. A user with muscle
  memory from any round before BO will find their first few drags land off-grid; that is the
  requested behaviour (*"it's impossible to get fine details"*) and not a regression. The HUD chip
  says which way Ctrl goes for the live gesture, in every gesture, which is the mitigation.

- **THE LIGHT-GRID MAGNET IS GONE AND NOTHING REPLACES IT.** Rounds AG/BK gave the one-axis gestures
  a capture band that pulled them onto the lattice even with the ranked query suppressed. It is
  deleted. An extrude or a push with no Ctrl held will now stop at whatever the cursor mapped to,
  to full float precision. Landing on a round number is Ctrl, or the numeric field.

- **THE UV CANVAS AND THE 3D VIEWPORT READ CTRL FROM DIFFERENT PLACES.** The 3D side polls
  `GetAsyncKeyState(VK_CONTROL)` inside `KiwiCmd_SnapEngaged` (it runs post-present, with no ImGui
  frame around it); the UV canvas reads `io.KeyCtrl` (it runs inside the ImGui frame, with no active
  KIWI command to ask). They agree in every observable case, but a synthetic key state injected into
  one and not the other would diverge.

- **PIVOT PLACEMENT IS THE ONE PLACE CTRL DOES NOTHING TO SNAPPING.** Deliberate, argued on the
  SNAP CONTRACT page, and announced in the HUD as `Snap On` — but it IS an exception, and it is the
  only one. Anything that wants a raw pivot has no way to ask for it.

- **THE UV DEFERRED COLLAPSE COMMITS A NO-OP UNDO STEP.** A click (no drag) on an already-targeted
  shape runs the whole `UVG_MOVE` gesture — snapshot, apply a zero delta, `UndoCommit` — before the
  collapse fires on the release. The undo bracket therefore records a move of zero, exactly as a
  click-with-no-drag on an untargeted shape always has. Harmless, and unchanged from the pre-BO
  behaviour; noted because "click to select" now reaches that path more often.

- **THE UV COLLAPSE THRESHOLD IS THE MARQUEE'S.** `KUVE_MARQUEE_MIN_PX` decides drag-vs-click for
  the marquee, for the empty-box press and now for the collapse, so the canvas has one answer. It is
  NOT the 3D viewport's `KBOX_CLICK_PIXELS`; the two canvases keep their own numbers.

---

## Round BP — section on the projection route, Ctrl-absolute extrude, explicit construction planes

- **THE SECTION IS A Z LEVEL AND NOTHING ELSE NOW.** Round BM let you click a brush FACE and cut
  along that face's plane. That is gone: clicking anything sets the cut LEVEL to that point's height.
  Anyone who found the face arm useful has lost it, deliberately — the directive said "Z down" twice
  and an arbitrary-normal plane is what made the feature's own state big enough to hide bugs in.

- **THE SECTION WILL REFUSE TO CUT IN TWO CONFIGURATIONS, AND SAYS SO ONCE EACH.** The oblique
  near-plane fold only sorts depth correctly when the camera is above the cut (perspective) or
  looking at least ~1 degree downward (ortho). An exactly level FRONT/SIDE ortho view and a camera
  sitting at or below the cut both draw UNCUT. The console names the remedy, the view-cube button
  reads `SECTION (no cut)`, and — by design — the pick clamp is off in that state too, so nothing is
  half-sectioned. Full derivation in RADIANT_UX_DESIGN §37c.

- **A SECTIONED FRAME LOSES ITS NEAR PLANE.** Geometry closer to the eye than `r_znear` is drawn
  while the section is cutting (the oblique plane replaced the near plane). In practice such geometry
  is above the cut and is removed by the section test itself; if you fly the camera INTO a solid that
  is below the cut level, you will see its interior faces where you previously saw them clipped.

- **A SECTIONED FRAME DOES NOT UPDATE `CONST_SRC_CODE_NEARPLANE_ORG/DX/DY`.**
  `R_DeriveNearPlaneConstantsForView` skips its derivation when the inverse-VP is oblique. Radiant
  binds no material that reads those constants, so nothing observable changes — but a future editor
  material that DID read them would see stale values for as long as a section is live.

- **`RC_SET_CLIP_PLANE` IS NOW DEAD CODE THAT STILL COMPILES.** The command, its enum slot, its
  backend handler and the round-BO probe all remain in the tree and nothing emits them. They are kept
  because the probe is the evidence that closed the section autopsy (RADIANT_UX_DESIGN §37a) and
  because the handler is correct — the device simply ignores user clip planes under a programmable
  vertex shader.

- **`Com_Printf` IN A RADIANT TU GOES TO A STDOUT NOBODY READS.** `engine_stubs.cpp:209` implements
  it as a bare `vprintf`. Any diagnostic written with it is invisible in the editor console. This is
  unchanged behaviour, now documented — it is what made round BO's section probe look like it had
  never fired.

- **CTRL DURING AN EXTRUDE OR FACE PUSH NOW JUMPS THE FACE TO THE CURSOR.** That is the requested
  behaviour ("match the mouse absolutely"), and the jump is exactly the cursor's axis offset at the
  moment the gesture was grabbed. Grab the lollipop and it is imperceptible; grab a gizmo arrow with
  the cursor far up the axis and pressing Ctrl will move the face by that much in one frame.
  RELEASING Ctrl never moves anything.

- **IN CTRL-ABSOLUTE MODE A GRID LINE CAN BEAT A REAL FACE.** The lattice and the geometry snap are
  both computed and the nearer one wins. If a face and a grid plane are almost equidistant from the
  cursor, which one takes the extrusion can flip with a pixel of movement. The HUD says `[MAJOR]` for
  a major-grid lock but does not distinguish "landed on a face" from "landed on a minor grid line".

- **THE CONSTRUCTION-PLANE LADDER IS GONE; FOUR OLD FLOWS NOW NEED [SPACE].** Drawing tools no longer
  derive a plane from the camera's dominant axis, from a construction object under the cursor, or
  from a brush face merely under the cursor. If you relied on "orbit to face a wall, then draw a
  circle on it", the new gesture is to press Space ON that wall first — which also flies the camera
  head-on. The demoted/kept table with a reason per rung is RADIANT_UX_DESIGN §16-BP.c.

- **THE DEFAULT PLANE IS ALWAYS AT THE WORLD ORIGIN.** Draw on a roof at 61 ft, start a new tool, and
  the new tool starts at z = 0 — the working height is NOT carried forward any more. Deliberate: that
  inheritance is what produced the reported teleport. Set the height explicitly (Space on a face at
  that height, or the XY palette row while an explicit plane is live) if you want it.

- **[SPACE] ON EMPTY SPACE NO LONGER PRINTS ITS "no face is selected" HINT WHEN A PLANE IS LATCHED.**
  It clears the plane instead. With no plane latched the hint is unchanged. So the same key does two
  different things depending on hidden state — mitigated by the PLANE chip, which is visible exactly
  when the clear is what Space will do.

- **OFF-PLANE SNAP CANDIDATES ARE REFUSED BEYOND 24 px OF WORLD FROM THE PLANE HIT.** 3D chaining
  still works (near candidates are ranked, not killed), but a deliberate snap to something far off
  the working plane — a rooftop corner while drawing on the ground, at a zoom where 24 px is a few
  tens of units — now needs the plane moved to that height first. That is the intended trade; the
  constants are `KSNAP_ONPLANE_PX` / `KSNAP_OFFPLANE_PX` in `kiwi_snap.h`.

## Round BR — the snapped point is the placed point

- **A CHAIN IS NO LONGER GUARANTEED COPLANAR.** Round AA made every point after the first project
  onto the plane the first point latched, so a ring was flat by construction. Under the ruling, a
  point that came off a GEOMETRY snap (or the Z axis guide) keeps its exact world position, so a ring
  drawn part-on and part-off a brush can be skew — and `kiwi_region.cpp` will refuse a skew ring
  (`KVALID_PLANE_DOT`), which reads as "my rectangle did not become a face". Deliberate: the user
  ruled that a snap must never be moved. To force a chain flat, set the plane first ([Space] on the
  face) and place the off-plane points over empty space, or use the Z lock for the rising segments.

- **A FACE SELECTION NO LONGER SETS THE WORKING PLANE.** Selecting one face and then starting a
  drawing tool from the **Construct palette**, the legacy menu, or a re-entry gives you the DEFAULT
  plane, not that face's. Only the **Shift+A gesture itself** (a creation chord pressed while the
  face is parked) still puts the sketch on the selected face. Snapping to that face's corners, edges
  and centre is unaffected — and, after ruling 1, now lands exactly on them.

- **THE SHIFT+A EXCEPTION NEEDS THE FACE TO BE *PARKED*.** The arm is set from the preempt rung,
  which requires an unmoved, auto-entered face gesture (`PreemptIdle`). A face selected and then
  nudged, or selected in a state that did not auto-enter, does not arm it — the chord is swallowed
  exactly as before and the tool starts on the default plane.

- **ARM 4c (THE Z AXIS GUIDE) CHANGES BEHAVIOUR IN A CHAIN.** It used to be flattened back onto the
  working plane by the round-AA latch, i.e. it did nothing after the first point. It now places
  verbatim, which is what round P designed it for — expect chains to climb when the dashed vertical
  guide is the live snap.


## Round BS — the construction plane is out of the drawing path

- **THE LINE, POLYLINE AND SPLINE TOOLS HAVE NO WORKING PLANE AT ALL.** A point is the engaged snap,
  else the ray's surface hit on geometry, else the world ground at z = 0 grid-snapped. Choosing a
  plane ([Space] on a face, the §16 palette rows XY / XZ / YZ / From face / From view) no longer
  changes where a line lands — those rows now serve the PLANAR shape tools (rect, rect-from-centre,
  circle, 2-point circle, arc, n-gon) and the §16b primitives, and nothing else. They are NOT stubbed:
  the code is unchanged and still installs and announces a plane.

- **THE 15° ANGLE LOCK IS GONE FOR FREE 3D CHAINS.** `SNAP_ANGLE` (arm 7) only exists in a plane, so
  it is now reachable by the planar tools only. For a line/polyline/spline use the **axis guides** —
  arm 4c now offers world **X, Y and Z** through the last placed point, drawn dashed and snapped by
  aiming — or type an exact bearing into the Tab **angle** field, which still composes with a typed
  length.

- **THE PLANE BANNER ONLY APPEARS WHILE A PLANAR TOOL IS LIVE.** Pressing [Space] on a face still sets
  an explicit plane and says so on the console, but the top-left `PLANE:` chip is drawn only while a
  rect/circle/arc/n-gon or a §16b primitive is running — i.e. only while the plane is doing something.
  The same is true of the amber plane-indicator square in the viewport and of the working-plane chip
  in the hint strip.

- **A PLANAR TOOL TAKES ITS PLANE FROM ITS FIRST POINT'S SURFACE.** Click a wall first and the rect
  lies on that wall; click empty space and it lies on the world ground through that point. An
  EXPLICIT plane overrides both. The choice is printed on the console at the moment it is made. The
  consequence to know: the plane is decided by the FIRST click, so a rect started in the void and
  finished on a roof is a ground rect, not a roof rect.

- **THE Z LOCK NOW TAKES A SURFACE HIT'S HEIGHT.** With the vertical constraint on and a face under
  the cursor, the point takes that surface's z (the area snap is no longer suppressed for the free
  tools and `SNAP_FACE` counts as a geometry snap). Point at the height you want on the wall. Over
  empty space it is the old ray solve, grid-snapped.

- **THE SNAP CHIP SAYS `snap off - hold Ctrl` DURING A TRANSFORM.** This is not a new restriction: a
  move / rotate / scale / extrude / bevel has been raw-unless-Ctrl since round BO, at the user's own
  direction. It used to print the single word `off`, which read as a refusal. The word changed, the
  behaviour did not. In a drawing tool with Ctrl held the same chip reads `snap free (Ctrl)`.

## Round BV — fully detailed textures (color + normal + specular import): known limits

- **"SHINY" ONLY FULLY SHOWS AFTER THE MAP IS COMPILED, AND IN GAME.** The specular families'
  reflection term is a CUBE sample — `texldl r1, r1, s1` against the reflection-probe sampler,
  with the mip chosen by the specular map's alpha. That cube is a **reflection probe built by
  the map compiler**, not something the editor synthesises. In the editor's own camera view a
  freshly imported metal or glass material will therefore look lit and bumped but not
  genuinely reflective. Run the compile (and place a reflection probe) to see the real thing.
  Nothing is wrong with the material; the probe simply does not exist yet.

- **`envMapParms` IS INHERITED FROM THE TEMPLATE AND IS NOT EXPOSED.** It is a real per-material
  control — `(scale at normal incidence, scale at grazing, Fresnel exponent, 0.625)`, read as
  `c5` by the lit pixel shader — and the shipped tree uses **56 distinct values** across the
  456 members of `l_sm_r0c0n0s0`. Every ROUND BV template candidate was chosen to carry the
  **modal** value `(0.8, 2.0, 1.0, 0.625)`, so an imported material gets the tree's own
  default and nothing else. A wizard control for it is a genuine candidate for a later round
  and is deliberately NOT invented here (the data supports the control; it does not tell us
  what a good value is for an arbitrary imported texture). Same for `colorTint`, which every
  chosen candidate carries as white.

- **THERE IS NO DETAIL-MAP SLOT.** The census found `l_sm_r0c0d0n0s0` (4 gated members) and
  `l_sm_b0c0d0n0s0` (2) and `l_sm_b0c0d0n0` (1). Six materials is not a family and a `d0`
  slot would need its own tiling semantics; it is out of scope for this round.
  `l_sm_t0c0s0` (1 member) is skipped for the same reason.

- **THE GREEN-CHANNEL HANDEDNESS OF A SOURCE NORMAL MAP CANNOT BE DERIVED.** The .iwi encoding
  is fully determined (alpha = the tangent-direction slope, green = the binormal-direction
  slope, both `value*scale + bias` with the shader's own `4.08 / 4.06452 / -2.08 / -2.06452`),
  but whether a given texture pack authored its green as +Y (OpenGL) or -Y (DirectX) is a
  property of **that pack's art**, not of this format, and no amount of reading the binary can
  settle it. The importer takes the source's green **verbatim**. If an imported surface looks
  lit from the wrong side — bumps reading as dents — flip the green channel of the source
  image and re-import. A "flip green" checkbox is a reasonable later addition; it is not added
  now because there is nothing in the data tree to validate a default against.

- **A SOURCE NORMAL MAP MUST BE A CONVENTIONAL RGB TANGENT-SPACE IMAGE.** That is what every
  texture pack ships and what the encoder assumes: `n = normalize(2*RGB - 1)`, from which it
  computes the slopes `nx/nz`, `ny/nz`. Feeding it an image that is already in the CoD4
  two-channel form (for instance a shipped `_nml` decoded back out of an .iwi) will be
  re-interpreted as an RGB normal and produce nonsense. Nothing detects this.

- **A NORMAL MAP PIXEL FACING AWAY FROM THE SURFACE HAS NO REPRESENTATION.** The stored value
  is a slope, so `nz -> 0` is a slope that runs off to infinity. The encoder floors `nz` at
  1/256 and lets the byte clamp finish the job; the representable slope window is roughly
  `x` in `[-2.08, +2.0]` and `y` in `[-2.06, +2.0]`. Very steep authored normals therefore
  flatten slightly at the extreme. Shipped maps live inside the same window, so this is the
  format's limit, not the importer's.

- **"COMPRESS" APPLIES TO THE COLOR MAP ONLY.** Every shipped normal map (1292 of 1293) and
  every shipped specular map (1437 of 1437) is DXT5, so both extra slots are always written
  DXT5 regardless of the checkbox. The tooltip says so.

- **A SPECULAR SOURCE WITHOUT AN ALPHA CHANNEL READS AS GLOSS 1.** The specular map's alpha is
  the reflection cube's mip selector (`lod = 6 - 8*alpha`), so an RGB-only source becomes
  alpha 255, i.e. the sharpest possible reflection. The wizard says this under the specular
  preview. If that is too mirror-like, author an alpha channel.

- **`$black` ON AN UNFILLED SPECULAR SLOT IS DERIVED FROM THE ENGINE, NOT FROM A SHIPPED FILE.**
  No shipped material binds a built-in to a `specularMap` entry — the tree instead switches to
  a techset without the slot. Our binding follows `Image_AssignDefaultTexture`
  (r_image.cpp:1070), which falls semantic 8 back to `rgp.blackImage`, i.e. zero specular.
  The honest alternative, and what the wizard's smart default actually does, is to pick a
  family that has no specular slot in the first place; the `$black` path only happens when the
  user deliberately selects a specular family and leaves the slot empty.

- **A DEAD NORMAL OR SPECULAR MAP IS REPORTED, NOT FATAL.** If a written `.iwi` for one of the
  optional slots fails to produce a texture, `Image_AssignDefaultTexture` substitutes the
  built-in and the material still renders; the verify line prints `MISSING` for that map and
  the import succeeds. Only a dead colorMap fails the import. This is deliberate — the
  alternative would be deleting a perfectly good color map and material over a bad third file.

- **PAIRING IS BY NAME AND HAS NO CONTENT HEURISTIC.** A file called `wall_s.png` is treated as
  a specular map because of its suffix, whatever is inside it. Two files claiming the same
  slot of the same key do not merge — the second becomes its own material. And a **single**
  dropped file never pairs at all: it is always a color map with its whole basename as the
  material name, so `brick_col.png` alone still imports as `brick_col`, exactly as in round BE.

- **THE OVERWRITE PROBE IS PER PLAN, NOT PER FILE.** Ticking "Overwrite the existing files"
  consents to replacing **all** of the material and up to three images this import would
  write, not just the one that collided. The "Will write:" list above the checkbox is the
  complete inventory.
## Round BX — decal/blend/glass draw order in the 3D camera: known limits

- **THE FIX IS A SORT, AND IT IS PER PASS.** `CamWnd_Draw`'s world fill and its selected-brush
  tint now gather their faces, order them by the binary's own key (`Editor_MaterialSortKey`,
  `100 * material->info.drawSurf.fields.primarySortKey`, r_ed_scene.cpp:172-175) and then draw
  — which is what `R_AddEditorSurfsCmd` (0x4FDA10) has always done for every surf-cache pass
  via `Editor_SurfCompare` (0x4FD9C0). Ordering is therefore correct **within** a pass. It is
  not global: a brush decal drawn in the world pass can still be repainted by an opaque
  **patch**, because the port splits patches into their own later pass (camwnd.cpp, the ROUND
  AF note) while the binary ran one loop and let `DrawBrush` dispatch patches out of it. Same
  for prefab/model contents, which are flushed after the world. Merging those passes is an
  architectural change, not a sort, so it is deliberately not attempted here.

- **THE TIE-BREAK IS SUBMISSION ORDER, NOT THE BINARY'S.** `Editor_SurfCompare` breaks a
  sortKey tie on techType and then on the vertex-buffer first index. The immediate path has
  one technique per draw arm and no VB handle, so the port breaks ties on gather order. That
  keeps every same-bucket family (notably the 2227 sortKey-4 opaque world materials) emitting
  in exactly the order it did before this round, which is the property the regression check
  wanted; it is not bit-for-bit the binary's tie-break.

- **THE SEE-THROUGH SKY FILM IS NOT SORTED WITH SKY.** Sky materials are all sortKey 5 and now
  correctly draw just after the opaque world. Round BC's translucent film, which *replaces*
  the sky face's draw with a `d_white` quad at alpha 0.30, is a KIWI overlay rather than a
  surface the binary has, and a film has to composite last — so it is keyed one bucket past
  every material bucket. If a future round makes that arm draw the sky *and* the film, the
  bucket has to be revisited.

- **A FACE WHOSE MATERIAL HANDLE DOES NOT RESOLVE KEYS AT 0.** It draws first, ahead of the
  opaques. Only the selected pass can reach that case (the world loop drops such a face before
  gathering), and a face with no resolvable material cannot declare itself a decal, so drawing
  it first is the safe end of the range rather than a considered ordering.

- **`RADIANT_SURFCACHE`'S KEY WAS A HARDCODED 0 AND IS NOW THE MATERIAL'S.** That opt-in path
  queued every surf into one bucket, so the flush's qsort had nothing to order by. It now
  passes `Editor_MaterialSortKey(layer0) + layerIndex` like `DrawGeo` does. The path is still
  opt-in and still not the default world draw.

## Round BY9 — the XY view's model tint and the dirty-flagged 2D renders: known limits

- **THE CAPTURE THIS ROUND WAS BUILT ON.** `F:\msvcproj\tracy-captures\backlot_by8_runtime_and_reload.tracy`
  — the BY8 build, mp_backlot, Debug, camera + 2D view open, 358 frames, median frame ~212 ms:
  `Camera RenderToRT` 107.6 ms/frame, `XY RenderToRT` 102.8 ms/frame, `Texture RenderToRT`
  0.4 ms/frame; `ed: model surfs dynamic` 1,395 of 2,788 skinned surfs, `ed: model upload KB`
  8,551 KB/frame (peak 34 MB), `ed: temp skin bytes` 8.76 MB/frame; all `culled*` plots flat
  zero (BY8's ruling), walk replays 2/frame with 0 re-records (BY6), `ed: inst cache build` 0
  at 39,250 KB resident (BY4 warm). Future rounds should cite it rather than re-argue it.

- **THE XY MODEL TINT IS NOW A CONSTANT, AND IT IS FLAT.** Technique 29 computes
  `lerp( colorMap x vertexColour, materialColor.rgb, materialColor.w )`. In this port every
  editor line batch pushes its own MATERIAL_COLOR with the vertex alpha in `.w`
  (`Ed_EmitLineBatch`, r_rendercmds.cpp) — the binary does not, it leaves
  `R_SetMaterialColor(NULL)`'s `{0,0,0,0}` parked — and XY brush wireframes pack alpha 1. So
  the 2D view's models were ALREADY drawing flat, in whatever colour the last brush wireframe
  had pushed. They are still flat; the colour is now deliberate (the entities' own
  `XY_BrushColor`) instead of incidental. `.w = 0` in `XY_FlushModelSurfs` (xywnd.cpp) is the
  one-literal A/B that would make them textured instead, and closing the gap properly means
  removing the per-line-batch MATERIAL_COLOR push, which is a separate round.

- **ONE TINT PER FLUSH WINDOW, CAPPED AT 64 CUTS.** A colour change with surfs already queued
  cuts the window (`KIWI_XY_TINT_MAX_CUTS`, xywnd.cpp). Past the cap the remaining surfs of
  that pass share the last adopted tint. On any real map the top-level entity colours form a
  handful of runs; the cap exists so a pathological alternating-colour map cannot turn one
  flush into thousands of render commands.

- **THE XY SELECTED PASS STILL STAMPS PER VERTEX.** It is tens of instances and it is round
  BY3's deliberate exception, so `ed: model surfs dynamic` / `ed: temp skin bytes` will never
  be exactly zero with something selected — that residue is the selection tint working.

- **THE DIRTY GATE TRUSTS `g_nUpdateBits`, AND TWO XY HANDLERS DID NOT SET IT.** The RMB view
  scroll only called `InvalidateRect` on the hidden native child, and the wheel zoom set
  `W_CAMERA` alone; both were correct while the RT was re-rendered every tick and are fixed in
  this round. Anything ADDED later that changes 2D content must either set `W_XY` /
  `W_XY_OVERLAY` or be covered by `XYWnd_OverlayIsLive` — the failure mode is a stale 2D view
  for up to `KIWI_VIEWDIRTY_HEARTBEAT_TICKS` (2 s), not forever.

- **THE HEARTBEAT IS INSURANCE, AND IT COSTS A HITCH.** Every 120 ticks a clean XY view renders
  anyway. Set `KIWI_VIEWDIRTY_HEARTBEAT_TICKS` to 0 once the dirty-source list is believed
  complete; leave it on while it is not.

- **THE Z VIEW IS ON THE SAME GATE; THE TEXTURE VIEW IS NOT.** Z shares the shape exactly and
  is invalidated by `W_Z` / `W_Z_OVERLAY` (it is also closed in the default layout, so it costs
  nothing either way). The texture browser is measured at 0.4 ms and its content — an
  async-loading material list with its own scroll state — is not expressible in the `W_*`
  currency, so it stays every-tick. The camera stays every-tick by design.

## Terrain Sculpt / Decals (round BQ, 2026-09-01) — UNBUILT, verify on next build

- **Not yet compiled.** `kiwi_terrain.cpp`, `kiwi_decal.cpp`, the forwarding
  `imgui_panel_advpatch.cpp` and the hooks in kiwi_viewport / kiwi_hover /
  kiwi_command / kiwi_windows / imgui_shell / pmesh.cpp tail were written without a
  build (user builds). First-build candidates: `ImGuiChildFlags_Borders` /
  `ImTextureID` idioms, `Pick()` mask arguments, block-scope externs.
- **Smooth seams across patch borders.** Smooth averages within one patch's control
  grid, so a shared edge point of two patches receives two different means. Sculpt
  both patches selected and expect a small step at the seam under heavy smoothing.
- **Blend paint shows only when the patch wears a blend material.** Painting alpha on
  a single-layer material changes nothing visible (the technique ignores it).
- **Decal preview/outline is flat**; a decal spanning two faces is not projected —
  it stays on the picked face's plane.
- **Decals placed on patch surfaces** use the pick's tessellated-triangle normal;
  curved patches get a flat decal tangent to that triangle.
- **Legacy soft-select option** arms the ported Alt+LMB chain (mode 1): Alt+LMB then
  opens/closes an empty "patch painting" undo record and draws the legacy ring.
- **Texture paint (layers) — UNBUILT.** First-build candidates: `Material_Load`
  takes `char*` (a mutable buffer is passed), `Patch_Duplicate` + `Brush_AddToList`
  + active-list splice for the new layer, `KiwiMat_Write` template lookup by techset.
  Behavioural: the blend twin only previews if the shipped `l_sm_b0c0*` template is
  healthy; a failed twin prints once and the layer draws opaque (z-fight) in the
  camera only. Layer order for a LOADED map is .map order; the registry does not
  persist across sessions. Twin materials are written into the KIWI materials dir.
- **Textures tab: click-apply moved to RELEASE (imgui_shell.cpp VP_Down/VP_Up).** A
  press now only records the spot; the ported TexWnd_OnLButtonDown apply fires on
  release within 4 px. This lets thumbnails be DRAGGED (KMTL_PAYLOAD) onto the
  Terrain Sculpt layer list and the Decals material field without re-texturing the
  selection on the way out. Unbuilt; check the press/release hit positions match.
- **Auto-transition bands are per-session** (keyed by layer patch); the default
  bands only stagger by layer index (L2 from Z 256 up, L3+ on slopes >= 35 deg).
- **Terrain chunks (round BQ, 2026-09-01) — UNBUILT.** Split / Expander / Trim create
  and delete patches through CreateChunk (MakeNewPatch + Patch_KiwiTextureAndBuild +
  AddBrushForPatch + Brush_AddToList) and the Edit->Delete undo bracket. Split
  resamples a REGULAR axis-aligned control grid (bilinear); a skewed or rotated
  source patch resamples wrongly. A non-square source splits into square chunks of
  the smaller side and leaves a strip uncovered (squares cannot tile a rectangle).
  The expander lattice is anchored on the corner of the patch under the cursor.
- **Single-patch texture layers (round BQ addendum 2) — UNBUILT.** New `kiwilayer`
  map lines: an OLD cod4map/radiant build ignores/rejects them (radiant's reader stops
  at an unknown token; cod4map's ParsePatch would choke). Both readers here handle
  them. Vertex colour painting on a layered patch edits layer weights (documented in
  the panel). Old maps with duplicate stacks: use "Fold overlapping duplicate patches
  into layers" once.
- **Terrain seams are now STITCHED after every height stamp** (kiwi_terrain.cpp
  StitchSeams): coincident border points across all eligible patches take the mean of
  the stroke's patches and unselected neighbours are pulled along (undo-marked). This
  supersedes the earlier "smooth seams across patch borders" caveat. T-junctions
  (a coarse chunk next to a denser one) still crack between points; Tessellate a
  region to one density, or let the expander build chunks (it matches per-axis cells).
