# radiant_files.cmake — editor source files for KIWI-Radiant
# Phase 1: scaffold + stubs.
# Phase 2: engine subset appended at the bottom.
# NOTE: UNIVERSAL/XANIM/COMMON come from common_files.cmake (included in CMakeLists.txt).

set(RADIANT_SRCS

    # ── App scaffold ─────────────────────────────────────────────────────────
    "${SRC_DIR}/radiant/radiant_main.cpp"   # U-BOOT: WinMain + frame WndProc + message pump
    "${SRC_DIR}/radiant/radiant_frame.h"    # U-BOOT: the shell-agnostic frame API (both shells)
    "${SRC_DIR}/radiant/prefs.cpp"          # CPrefsDlg settings object + registry persistence (P6)
    "${SRC_DIR}/radiant/prefs.h"
    "${SRC_DIR}/radiant/stdafx.h"
    "${SRC_DIR}/radiant/mainfrm.h"
    "${SRC_DIR}/radiant/res/resource.h"
    "${SRC_DIR}/radiant/res/radiant.rc"

    # ── Tier 1 — math/geometry foundation ────────────────────────────────────
    "${SRC_DIR}/radiant/mainfrm.cpp"        # CMainFrame (387 methods; stub for P1)
    "${SRC_DIR}/radiant/cmdlib.cpp"         # TODO_RADIANT Phase 2 (libs/cmdlib reference)
    "${SRC_DIR}/radiant/winding.cpp"
    "${SRC_DIR}/radiant/linearmapping.cpp"  # universal/linearmapping.cpp — double 3x3 LU solver (texture-lock reproject)
    "${SRC_DIR}/radiant/linearmapping.h"
    "${SRC_DIR}/radiant/vehiclepath.cpp"    # universal/g_vehicle_path.cpp — vehicle node graph + path-preview overlay
    "${SRC_DIR}/radiant/primarylights_region.cpp"  # light-region CSG (common/primarylights_region.cpp)
    "${SRC_DIR}/radiant/eclass.cpp"
    "${SRC_DIR}/radiant/brush.cpp"
    "${SRC_DIR}/radiant/texturevecs.cpp"
    "${SRC_DIR}/radiant/csg.cpp"
    "${SRC_DIR}/radiant/entity.cpp"

    # ── Tier 2 — map model & operations ──────────────────────────────────────
    "${SRC_DIR}/radiant/map.cpp"
    "${SRC_DIR}/radiant/select.cpp"
    "${SRC_DIR}/radiant/drag.cpp"
    "${SRC_DIR}/radiant/undo.cpp"
    "${SRC_DIR}/radiant/pmesh.cpp"
    "${SRC_DIR}/radiant/points.cpp"
    "${SRC_DIR}/radiant/errorfile.cpp"
    "${SRC_DIR}/radiant/mapinfo.cpp"

    # ── Tier 3 — CoD-specific editor systems ─────────────────────────────────
    "${SRC_DIR}/radiant/materialdef.cpp"
    "${SRC_DIR}/radiant/scriptgroup.cpp"
    "${SRC_DIR}/radiant/layers.cpp"
    "${SRC_DIR}/radiant/filters.cpp"
    "${SRC_DIR}/radiant/filtersettings.cpp"
    "${SRC_DIR}/radiant/shadowvolume.cpp"
    "${SRC_DIR}/radiant/layeredmaterials.cpp"
    "${SRC_DIR}/radiant/layeredmaterialwnd.cpp"

    # ── Tier 4 — render / glue ────────────────────────────────────────────────
    "${SRC_DIR}/radiant/qedefs.h"          # P3: editor primitive types & enums
    "${SRC_DIR}/radiant/qe3.h"             # P3: editor object model + layout static_asserts
    "${SRC_DIR}/radiant/qe3.cpp"
    "${SRC_DIR}/radiant/win_qe3.cpp"
    "${SRC_DIR}/radiant/gfxwrapper.cpp"
    "${SRC_DIR}/radiant/draw.cpp"

    # ── Phase 5 — MFC windows ────────────────────────────────────────────────
    "${SRC_DIR}/radiant/xywnd.cpp"
    "${SRC_DIR}/radiant/camwnd.cpp"
    "${SRC_DIR}/radiant/z.cpp"
    "${SRC_DIR}/radiant/texwnd.cpp"
    "${SRC_DIR}/radiant/texturebar.cpp"
    "${SRC_DIR}/radiant/win_ent.cpp"

    # ── Phase 6 — dialogs ────────────────────────────────────────────────────
    "${SRC_DIR}/radiant/surfacedlg.cpp"
    "${SRC_DIR}/radiant/entitylist.cpp"     # CEntityListDlg — Edit→Entity Info entity browser (tree + K/V)
    "${SRC_DIR}/radiant/findtexture.cpp"    # CFindTextureDlg + FindReplaceTextures (find/replace texture)
    "${SRC_DIR}/radiant/patchdialog.cpp"
    "${SRC_DIR}/radiant/win_dlg.cpp"
    "${SRC_DIR}/radiant/imgui_shell.cpp"    # UI-rework Phase 2a — ImGui overlay shell (-imgui flag)
    # (imgui_dockhost.cpp retired — the main frame is the dockspace surface now)
    "${SRC_DIR}/radiant/radiant_registry.cpp"     # HKCU profile helpers (replaced the shim CWinApp)
    "${SRC_DIR}/radiant/radiant_rtt.cpp"          # Phase 5 — offscreen render-target viewport textures
    "${SRC_DIR}/radiant/imgui_panels.cpp"   # UI-rework Phase 3 — panels over the Phase-1 actions
    "${SRC_DIR}/radiant/imgui_panel_surface.cpp"  # Phase 3 — surface inspector panel
    "${SRC_DIR}/radiant/imgui_panel_entity.cpp"   # Phase 3 — entity inspector panel
    "${SRC_DIR}/radiant/imgui_panel_findtex.cpp"  # Phase 3 — find/replace texture panel
    "${SRC_DIR}/radiant/imgui_panel_layers.cpp"   # Phase 3 — layers panel
    "${SRC_DIR}/radiant/imgui_panel_dynent.cpp"   # Phase 3 — dynamic entity panel
    "${SRC_DIR}/radiant/imgui_panel_vehicle.cpp"  # Phase 3 — vehicle panel
    "${SRC_DIR}/radiant/imgui_panel_model.cpp"    # Phase 3 — model replace panel
    "${SRC_DIR}/radiant/imgui_panel_vertedit.cpp" # Phase 3 — vertex edit panel
    "${SRC_DIR}/radiant/imgui_panel_kvselect.cpp" # Phase 3 — select by key/value panel
    "${SRC_DIR}/radiant/imgui_panel_mapinfo.cpp"  # Phase 3 — map info panel
    "${SRC_DIR}/radiant/imgui_panel_prefs.cpp"    # Phase 3 — preferences panel
    "${SRC_DIR}/radiant/imgui_panel_patch.cpp"    # Phase 3 — patch inspector panel
    "${SRC_DIR}/radiant/imgui_panel_patchdensity.cpp" # Phase 5 — Simple Patch/Terrain density prompt
    "${SRC_DIR}/radiant/imgui_panel_filters.cpp"  # Filters panel (F) — CFilterWnd replacement
    "${SRC_DIR}/radiant/imgui_panel_commands.cpp" # Phase 3 — command list panel
    "${SRC_DIR}/radiant/imgui_panel_advpatch.cpp" # Phase 3 — advanced patch edit panel
    # ── UX overhaul (RADIANT_UX_DESIGN) Phase 1a — selection core + unified pick ──
    "${SRC_DIR}/radiant/kiwi_selection.h"    # §1 typed selection model (sel_item_t/selection_t)
    "${SRC_DIR}/radiant/kiwi_selection.cpp"  # §1 legacy adapter (Sel_SyncToLegacy / RebuildFromLegacy)
    "${SRC_DIR}/radiant/kiwi_pick.h"         # §2 unified pick API (Pick / ray + projection helpers)
    "${SRC_DIR}/radiant/kiwi_pick.cpp"       # §2 Test_Ray surface pick + screen-space vert/edge pick
    # ── UX overhaul Phase 1b — camera, grid/units, chips, hover tints, box select ──
    "${SRC_DIR}/radiant/kiwi_units.h"        # §17 display-units layer (inches) + modern grid spacing
    "${SRC_DIR}/radiant/kiwi_units.cpp"
    "${SRC_DIR}/radiant/kiwi_ux.h"           # Phase-1b toggles (modern-input master switch) + settings UI
    "${SRC_DIR}/radiant/kiwi_ux.cpp"
    "${SRC_DIR}/radiant/kiwi_lines.h"        # budgeted world-overlay line batcher over R_Add3DLine
    "${SRC_DIR}/radiant/kiwi_lines.cpp"
    "${SRC_DIR}/radiant/kiwi_loft.h"         # ROUND AF item 6: L, the face-to-face bridge (loft)
    "${SRC_DIR}/radiant/kiwi_loft.cpp"
    "${SRC_DIR}/radiant/kiwi_grid.h"         # §17 world axes + camera-footprint ground grid + snap
    "${SRC_DIR}/radiant/kiwi_grid.cpp"
    "${SRC_DIR}/radiant/kiwi_camera.h"       # §10 orbit / dolly layer over camera_s
    "${SRC_DIR}/radiant/kiwi_camera.cpp"
    "${SRC_DIR}/radiant/kiwi_hover.h"        # §18 hover pick state + hover/active accent draw
    "${SRC_DIR}/radiant/kiwi_hover.cpp"
    "${SRC_DIR}/radiant/kiwi_boxselect.h"    # §12 directional box selection (containment/crossing)
    "${SRC_DIR}/radiant/kiwi_boxselect.cpp"
    "${SRC_DIR}/radiant/kiwi_viewport.h"     # shell bridge: camera input routing + §11 chips overlay
    "${SRC_DIR}/radiant/kiwi_viewport.cpp"
    # ── UX overhaul Phase 2 — command core: palette, modal framework, snap v1, keymaps ──
    # (kiwi_snap grew to v2 in Phase 3: edge / mid / face targets + markers;
    #  and to v3 in Phase 4: construction endpoints / intersections / cplane / angle)
    "${SRC_DIR}/radiant/kiwi_snap.h"         # §6 SnapManager v3
    "${SRC_DIR}/radiant/kiwi_snap.cpp"
    "${SRC_DIR}/radiant/kiwi_numeric.h"      # §13 numeric entry + viewport HUD
    "${SRC_DIR}/radiant/kiwi_numeric.cpp"
    "${SRC_DIR}/radiant/kiwi_command.h"      # §3 command metadata + §4 modal framework + undo brackets
    "${SRC_DIR}/radiant/kiwi_command.cpp"
    "${SRC_DIR}/radiant/kiwi_cmdoptions.h"   # §62.3 the in-command options panel (round AI, item 3)
    "${SRC_DIR}/radiant/kiwi_cmdoptions.cpp"
    "${SRC_DIR}/radiant/kiwi_patchverts.h"   # §62.6 patch vertex mode on V (round AI, item 6)
    "${SRC_DIR}/radiant/kiwi_patchverts.cpp"
    "${SRC_DIR}/radiant/kiwi_palette.h"      # §15 F command palette over g_radiantCommands
    "${SRC_DIR}/radiant/kiwi_palette.cpp"
    "${SRC_DIR}/radiant/kiwi_keymap.h"       # §11 keymap profiles (classic / modern)
    "${SRC_DIR}/radiant/kiwi_keymap.cpp"
    # ── UX overhaul Phase 3 — direct manipulation of existing geometry ──
    "${SRC_DIR}/radiant/kiwi_validity.h"     # §19 validity contract + planept/ctrl baseline
    "${SRC_DIR}/radiant/kiwi_validity.cpp"
    "${SRC_DIR}/radiant/kiwi_transform.h"    # §13 G/R/S, §20 face push/pull, §21 edge, §22 vertex
    "${SRC_DIR}/radiant/kiwi_transform.cpp"
    # ── UX overhaul Phase 4 — construction geometry, regions, extrude-to-brush ──
    "${SRC_DIR}/radiant/kiwi_construct.h"    # §7 construction store + §16 planes + drawing tools
    "${SRC_DIR}/radiant/kiwi_construct.cpp"  # …plus the <mapname>.kiwi sidecar persistence
    "${SRC_DIR}/radiant/kiwi_region.h"       # §8 closed-loop regions + fill + the 2D polygon toolkit
    "${SRC_DIR}/radiant/kiwi_region.cpp"
    # ROUND K — the PLANAR ARRANGEMENT behind §8.  USER DIRECTIVE: a region must
    # form "whenever lines close off a section even if they extend further" (four
    # lines crossing like a #).  §8's two passes are ENDPOINT passes and
    # structurally cannot see a mid-span crossing, so this splits every coplanar
    # segment at every mutual crossing and face-walks the resulting planar graph for
    # its bounded cells — which is exactly what Plasticity does in
    # PlanarCurveDatabase (fragment) + RegionManager (OuterContoursBuilder /
    # GetCorrectRegions).  Cited, capped and argued in the header.
    "${SRC_DIR}/radiant/kiwi_arrange.h"      # split-at-crossings + minimal-face walk
    "${SRC_DIR}/radiant/kiwi_arrange.cpp"
    "${SRC_DIR}/radiant/kiwi_extrude.h"      # §23 region extrusion -> ordinary brushes
    "${SRC_DIR}/radiant/kiwi_extrude.cpp"

    # ── Shakeout C — §16b the full creation suite (mapped from the Plasticity
    # inventory in RADIANT_UX_DESIGN §16b).  The four extra CURVE tools live in
    # kiwi_construct.cpp next to the original five; these two files are the SOLID
    # primitives and the Shift+A menu that lists everything.
    "${SRC_DIR}/radiant/kiwi_primitive.h"    # §16b Box / Cylinder / Sphere / Cone -> real brushes
    "${SRC_DIR}/radiant/kiwi_primitive.cpp"
    "${SRC_DIR}/radiant/kiwi_addmenu.h"      # §16b.4 the cursor-anchored add menu (Shift+A)
    "${SRC_DIR}/radiant/kiwi_addmenu.cpp"

    # Phase 5 — modeling power (§24 CSG workflow, §25 bevel/inset/mirror/arrays +
    # selection expansion).  No new CSG math: every op drives a ported core.
    "${SRC_DIR}/radiant/kiwi_csg.h"          # §24 CSG workflow over CSG_Merge / CSG_MakeHollow
    "${SRC_DIR}/radiant/kiwi_csg.cpp"
    "${SRC_DIR}/radiant/kiwi_bevel.h"        # §25 bevel/chamfer edge + inset face (clone)
    "${SRC_DIR}/radiant/kiwi_bevel.cpp"
    "${SRC_DIR}/radiant/kiwi_dupe.h"         # §25 mirror (over the ported flip) + linear/radial arrays
    "${SRC_DIR}/radiant/kiwi_dupe.cpp"
    "${SRC_DIR}/radiant/kiwi_selext.h"       # §25 coplanar / touching / same-material / connected
    "${SRC_DIR}/radiant/kiwi_selext.cpp"

    # Shakeout D — Ctrl+1..4 selection conversion, ported from Plasticity's
    # SelectionConversionStrategy (cites in the header).  Instant block 2 (34100..).
    "${SRC_DIR}/radiant/kiwi_selconv.h"      # selection -> its points / edges / faces / objects
    "${SRC_DIR}/radiant/kiwi_selconv.cpp"

    # Shakeout F — SELECTING construction geometry (a KIWI-owned list parallel to
    # selection_t, never entering it) plus the three things a selection is for:
    # Join (Ctrl+J, over kiwi_region's chain walker), Delete (arbitrated in the key
    # funnel) and Move (an arm inside the existing G command).
    "${SRC_DIR}/radiant/kiwi_conselect.h"    # construction selection + join / delete / move
    "${SRC_DIR}/radiant/kiwi_conselect.cpp"

    # ROUND AR, ITEM 1 — the CONSTRUCTION half of Ctrl+C / Ctrl+V.  The brush half
    # is entirely ported (33039 / 33040) and unchanged; this is a process-local
    # store of kconObject_t VALUES hung on the same two command ids, so a mixed
    # selection copies and pastes both without either half stomping the other's
    # clipboard.  A construction-only paste auto-enters Move through the existing
    # KiwiCmd_AfterPaste hook; a MIXED paste refuses it with a line, because one
    # Move gesture structurally carries one kind (kiwi_conclip.h).
    "${SRC_DIR}/radiant/kiwi_conclip.h"      # construction clipboard: copy / paste
    "${SRC_DIR}/radiant/kiwi_conclip.cpp"

    # Shakeout G — the Plasticity MODELLING verbs.  One shared two-halves splitter
    # over the ported Brush_SplitBrushByFace serves both Cut (C, along a selected
    # construction line, swept away from the camera) and Face Split (Ctrl+R, Tab
    # flips U/V); Match Face (Z) copies one face's plane onto another; Join (J) is
    # a context verb over CSG_Merge and the shakeout-F line joiner.  The E extrude
    # arm lands in kiwi_extrude above (it reuses that file's prism writer verbatim),
    # and the gizmo-only transforms, the movable pivot (V) and the push-through
    # delete land in kiwi_transform / kiwi_gizmo / kiwi_boxselect.
    # ROUND T — MATERIAL INHERITANCE.  USER DIRECTIVE: "Make it so the texture is
    # just inherited from the parent brush that are being operated on.  The caulk
    # texture is not usable."  One place decides which face a NEW surface copies
    # from (cut / split / boolean carve / chamfer / fillet patch) and realizes the
    # copy, which is also the fix for the round-Q fillets landing invisible — they
    # were faithfully copying a tool material.  See kiwi_material.h R1-R6.
    "${SRC_DIR}/radiant/kiwi_material.h"     # the inheritance rules + the realize
    "${SRC_DIR}/radiant/kiwi_material.cpp"

    "${SRC_DIR}/radiant/kiwi_split.h"        # the shared splitter + Cut (C) + Split Face (Ctrl+R)
    "${SRC_DIR}/radiant/kiwi_split.cpp"
    # ROUND L — Q, the BOOLEAN verb.  Difference carves one solid out of another by
    # splitting the target sequentially against every face plane of the tool
    # (kiwi_split's def-level splitter, N times), keeping the OUTSIDE half at each
    # plane and discarding the intersection; union drives the ported CSG_Merge
    # through the classic command id, exactly as kiwi_join's face arm does.  There
    # is no subtract core in this port (kiwi_csg.h's Phase-5 inventory), which is
    # why the carve is built here rather than dispatched.
    "${SRC_DIR}/radiant/kiwi_boolean.h"      # Boolean (Q) — difference / union of solids
    "${SRC_DIR}/radiant/kiwi_boolean.cpp"
    "${SRC_DIR}/radiant/kiwi_matchface.h"    # Match Face (Z) — copy a face's plane
    "${SRC_DIR}/radiant/kiwi_matchface.cpp"
    "${SRC_DIR}/radiant/kiwi_join.h"         # Join (J) — coplanar faces -> CSG_Merge, lines -> chain
    "${SRC_DIR}/radiant/kiwi_join.cpp"

    # Shakeout H — TRIM (T).  Construction lines only: remove the span of a line
    # between its bounding crossings, splitting the object when the span is
    # interior.  Ports plasticity/src/commands/curve/TrimCommand.ts + TrimFactory.
    "${SRC_DIR}/radiant/kiwi_trim.h"         # Trim (T) — cut a line back to its crossings
    "${SRC_DIR}/radiant/kiwi_trim.cpp"

    # ROUND K — the Plasticity EXTRUDE HANDLE.  USER DIRECTIVE: clicking a face
    # should auto-enter extrusion and "it should look like a lollipop", stay
    # external to the face, ride it, and hide the move gizmo.  It REPLACES the
    # three-arrow gizmo for face push/pull and both extrudes rather than joining it
    # — kiwi_gizmo.cpp's GizmoUsable() refuses outright while one is wanted, so the
    # arrows are neither drawn nor hit-tested and cannot swallow the ball's press.
    "${SRC_DIR}/radiant/kiwi_lollipop.h"     # the ring + stem + ball, and its grab
    "${SRC_DIR}/radiant/kiwi_lollipop.cpp"

    # ROUND J — the last Plasticity verbs the overhaul was missing.  Two modal
    # CONSTRUCTION-CURVE editors (a 2D polygon offset with miter/bevel joins, and
    # a per-corner fillet ported from ContourFilletFactory) plus two instant
    # VIEW verbs (frame the selection, and the invert-hidden Radiant never had).
    # Duplicate (Shift+D) lands in kiwi_dupe above — it is the plain form of what
    # that file's arrays already do N times.  Repeat Last Command lives in
    # kiwi_command.cpp, because "what was the last command" is a framework fact.
    "${SRC_DIR}/radiant/kiwi_offset.h"       # Offset Curve (O) — parallel copy of a chain
    "${SRC_DIR}/radiant/kiwi_offset.cpp"
    "${SRC_DIR}/radiant/kiwi_fillet.h"       # Fillet Corners (B) — round a chain's corners
    "${SRC_DIR}/radiant/kiwi_fillet.cpp"

    # ROUND Q — the SOLID fillet B now also means: chamfer a brush edge and lay a
    # q3 biquadratic bezier patch into the notch.  Bare B is context-aware
    # (brush edges -> this, otherwise kiwi_fillet above); see kiwi_patchfillet.h.
    "${SRC_DIR}/radiant/kiwi_patchfillet.h"  # Fillet Edge (patch) — pseudo-fillets
    "${SRC_DIR}/radiant/kiwi_patchfillet.cpp"
    "${SRC_DIR}/radiant/kiwi_focus.h"        # Focus On Selection (/) — frame it
    "${SRC_DIR}/radiant/kiwi_focus.cpp"
    "${SRC_DIR}/radiant/kiwi_visibility.h"   # the hide / isolate family + Invert Hidden
    "${SRC_DIR}/radiant/kiwi_visibility.cpp"

    # Shakeout I — ONE undo timeline over BOTH domains (§27).  USER DIRECTIVE:
    # "Redo the whole undo/redo system so that it works with every action."  A
    # journal of {LEGACY, CONSTRUCTION} tickets appended where each domain CLOSES a
    # record (undo.cpp's Undo_End tail / kiwi_construct's KiwiCon_UndoPush), popped
    # newest-first by the ID_EDIT_UNDO / ID_EDIT_REDO pre-hook in
    # Radiant_DispatchCommandDirect.  Adds the construction store's missing REDO.
    "${SRC_DIR}/radiant/kiwi_undo.h"         # the unified undo/redo journal
    "${SRC_DIR}/radiant/kiwi_undo.cpp"

    # Phase 6 — UV workflow v1 (§26).  Modal texture shift/rotate/scale + the
    # texture pick, all wrapped around the ported Brush_*Texture / Texture_SetTexture
    # cores, plus the read-only texdef readout.  No new texdef math.
    "${SRC_DIR}/radiant/kiwi_uv.h"           # §26 texture shift/rotate/scale + pick + readout
    "${SRC_DIR}/radiant/kiwi_uv.cpp"

    # Shakeout A — user-feedback round over the shipped phases: the §14 move
    # gizmo, the orientation view-cube and the contextual hotkey panel.  (The
    # rest of the round — RMB mouselook/truck/fly, thinner split axis lines —
    # lands inside kiwi_camera / kiwi_viewport / kiwi_grid above.)
    "${SRC_DIR}/radiant/kiwi_gizmo.h"        # §14 translate gizmo -> the SAME Move command as G
    "${SRC_DIR}/radiant/kiwi_gizmo.cpp"
    "${SRC_DIR}/radiant/kiwi_viewcube.h"     # orientation widget (six axis balls, top-right)
    "${SRC_DIR}/radiant/kiwi_viewcube.cpp"
    "${SRC_DIR}/radiant/kiwi_hints.h"        # contextual hotkey panel, read live from g_radiantCommands
    "${SRC_DIR}/radiant/kiwi_hints.cpp"

    # Shakeout B — the 3D-first default layout.  Per-window visibility flags, the
    # native "Windows" popup on the frame menu, and the RTT render gating that makes
    # a hidden 2D / Z / texture view cost nothing.  (The rest of the round — the
    # VK_DELETE binding and the XY declutter / red camera icon — lands inside
    # kiwi_keymap, imgui_shell and xywnd.)
    "${SRC_DIR}/radiant/kiwi_windows.h"      # §9 per-window visibility + Windows menu
    "${SRC_DIR}/radiant/kiwi_windows.cpp"

    # ROUND W — the Plasticity-style OUTLINER.  USER DIRECTIVE: "a collapsible
    # giant list of all brushes on the left like it's Plasticity ... Support groups
    # ... Show hidden ones with a closed eyeball ... Split them by type (curve vs
    # solid) ... allow selection by clicking ... shift clicking, shift dragging ...
    # a group 'folder' in the list that you can drag to."  A brush group IS a
    # func_group entity (real map data, ported create/reparent/free/undo); a
    # construction group is one int on kconObject_t plus two sidecar keywords.
    # Ports plasticity/src/components/outliner/{FlattenOutline,Outliner,
    # OutlinerItems} — the flatten-per-frame model, the per-group Solids/Curves
    # sections and the eye/eye-off row control.
    "${SRC_DIR}/radiant/kiwi_outliner.h"     # the scene list + the two group verbs
    "${SRC_DIR}/radiant/kiwi_outliner.cpp"

    # ROUND AB — the MONITOR-SLEEP crash.  A D3D9 device reset destroys the editor
    # vertex-buffer pool (r_ed_vertbuf) but nothing invalidated the per-face and
    # per-patch vertHandles cached in the surf cache, so the first frame after the
    # wake drew a NULL vertex buffer, leaked tess.indexCount out of
    # RB_DrawEditorSkinnedCached_Sub and dereferenced a NULL g_primStats in
    # RB_EndSurfacePrologue.  This file drops the cache in the same breath as the
    # pool.  See kiwi_devicereset.h for the whole chain.
    "${SRC_DIR}/radiant/kiwi_devicereset.h"  # editor surf-cache invalidation on device reset
    "${SRC_DIR}/radiant/kiwi_devicereset.cpp"

    # ROUND AB — Auto Bool: greedy convex-pair consolidation over the ported CSG_Merge.
    "${SRC_DIR}/radiant/kiwi_autobool.h"     # Auto Bool — reduce brush count by merging pairs
    "${SRC_DIR}/radiant/kiwi_autobool.cpp"

    "${SRC_DIR}/radiant/verteditdlg.cpp"
    "${SRC_DIR}/radiant/layersdlg.cpp"
    "${SRC_DIR}/radiant/dynentitydlg.cpp"
    "${SRC_DIR}/radiant/vehicledlg.cpp"
    "${SRC_DIR}/radiant/modeldlg.cpp"
    "${SRC_DIR}/radiant/mayaexport.cpp"     # Misc->Maya Export — the .mel MEL-script geometry exporter

    # ── Phase 2 — editor-only renderer files (OPUS stubs) ────────────────────
    # These have no kisak equivalents; fresh decompile + OPUS queue items.
    "${SRC_DIR}/radiant/r_ed_scene.cpp"
    "${SRC_DIR}/radiant/r_ed_vertbuf.cpp"

    # ── Build 10 — engine stubs (globals + function stubs for excluded files) ─
    "${SRC_DIR}/radiant/engine_stubs.cpp"
)

# ─────────────────────────────────────────────────────────────────────────────
# Phase 2: engine file subsets
# UNIVERSAL, XANIM, COMMON defined in common_files.cmake.
# ─────────────────────────────────────────────────────────────────────────────

# script/ — only the two utility files Radiant uses (not the full VM)
set(RADIANT_SCRIPT
    "${SRC_DIR}/script/scr_memorytree.cpp"
    "${SRC_DIR}/script/scr_memorytree.h"
    "${SRC_DIR}/script/scr_stringlist.cpp"
    "${SRC_DIR}/script/scr_stringlist.h"
)

# physics/ — load-obj + ODE mass/matrix + ODE math/error support
set(RADIANT_PHYSICS
    "${SRC_DIR}/physics/physpreset_load_obj.cpp"
    "${SRC_DIR}/physics/ode/mass.cpp"
    "${SRC_DIR}/physics/ode/matrix.cpp"
    "${SRC_DIR}/physics/ode/error.cpp"
    "${SRC_DIR}/physics/ode/fastdot.c"
    "${SRC_DIR}/physics/ode/fastlsolve.c"
    "${SRC_DIR}/physics/ode/fastltsolve.c"
)

# qcommon/ — BSP API + console command layer + file access + pack funcs + mem track
set(RADIANT_QCOMMON
    "${SRC_DIR}/qcommon/com_bsp.cpp"
    "${SRC_DIR}/qcommon/com_bsp.h"
    "${SRC_DIR}/qcommon/com_bsp_load_obj.cpp"
    "${SRC_DIR}/qcommon/cmd.cpp"
    "${SRC_DIR}/qcommon/cmd.h"
    "${SRC_DIR}/qcommon/com_fileaccess.cpp"
    "${SRC_DIR}/qcommon/com_fileaccess.h"
    "${SRC_DIR}/qcommon/com_pack.cpp"
    "${SRC_DIR}/qcommon/com_pack.h"
    "${SRC_DIR}/qcommon/mem_track.cpp"
    "${SRC_DIR}/qcommon/mem_track.h"
    # iwd (ZIP) reader — stock images live in iw_*.iwd; the editor must read them
    # (materials/shaders are loose in raw/, but .iwi images are only in iwds).
    # Replaces the no-op unz* stubs in engine_stubs.cpp.
    "${SRC_DIR}/qcommon/unzip.cpp"
    "${SRC_DIR}/qcommon/unzip.h"
)

# gfx_d3d/ — filtered: game-only files excluded per file_map.json cross-reference.
# Excluded (unsafe includes): r_cinematic (mss.h+snd_local.h),
#   r_devgui, rb_drawprofile (scr_parser.h), r_draw_pixelshader (no header).
# r_model_skin_sse RE-INCLUDED 2026-07-25: its includes are clean now (q_shared/
#   r_model_skin.h/xanim/profile/intrinsics) and the master merge (66b37ca5) made
#   r_model_skin.cpp call R_SkinXSurfaceSkinnedSse unconditionally at link time;
#   sys_SSE is registered by the engine_stubs GROUP-C shim.
# Added in Build 10: r_cmds, r_workercmds_common, rb_stats, rb_uploadshaders,
#   rb_logfile, rb_shadowcookie, rb_showcollision, r_draw_lit, r_draw_sunshadow,
#   r_dpvs_dynmodel, r_fog, r_screenshot, r_texturemem.
set(RADIANT_GFX_D3D
    "${SRC_DIR}/gfx_d3d/fxprimitives.h"
    "${SRC_DIR}/gfx_d3d/rb_backend.cpp"
    "${SRC_DIR}/gfx_d3d/rb_backend.h"
    "${SRC_DIR}/gfx_d3d/rb_debug.cpp"
    "${SRC_DIR}/gfx_d3d/rb_debug.h"
    "${SRC_DIR}/gfx_d3d/rb_depthprepass.cpp"
    "${SRC_DIR}/gfx_d3d/rb_depthprepass.h"
    "${SRC_DIR}/gfx_d3d/rb_draw3d.cpp"
    "${SRC_DIR}/gfx_d3d/rb_draw3d.h"
    "${SRC_DIR}/gfx_d3d/rb_fog.cpp"
    "${SRC_DIR}/gfx_d3d/rb_fog.h"
    "${SRC_DIR}/gfx_d3d/rb_imagefilter.cpp"
    "${SRC_DIR}/gfx_d3d/rb_imagefilter.h"
    "${SRC_DIR}/gfx_d3d/rb_imagetouch.cpp"
    "${SRC_DIR}/gfx_d3d/rb_light.cpp"
    "${SRC_DIR}/gfx_d3d/rb_light.h"
    "${SRC_DIR}/gfx_d3d/rb_pixelcost.cpp"
    "${SRC_DIR}/gfx_d3d/rb_pixelcost.h"
    "${SRC_DIR}/gfx_d3d/rb_postfx.cpp"
    "${SRC_DIR}/gfx_d3d/rb_postfx.h"
    "${SRC_DIR}/gfx_d3d/rb_shade.cpp"
    "${SRC_DIR}/gfx_d3d/rb_shade.h"
    "${SRC_DIR}/gfx_d3d/rb_sky.cpp"
    "${SRC_DIR}/gfx_d3d/rb_sky.h"
    "${SRC_DIR}/gfx_d3d/rb_spotshadow.cpp"
    "${SRC_DIR}/gfx_d3d/rb_spotshadow.h"
    "${SRC_DIR}/gfx_d3d/rb_state.cpp"
    "${SRC_DIR}/gfx_d3d/rb_state.h"
    "${SRC_DIR}/gfx_d3d/rb_sunshadow.cpp"
    "${SRC_DIR}/gfx_d3d/rb_sunshadow.h"
    "${SRC_DIR}/gfx_d3d/rb_tess.cpp"
    "${SRC_DIR}/gfx_d3d/rb_tess.h"
    "${SRC_DIR}/gfx_d3d/r_add_bsp.cpp"
    "${SRC_DIR}/gfx_d3d/r_add_cmdbuf.cpp"
    "${SRC_DIR}/gfx_d3d/r_add_staticmodel.cpp"
    "${SRC_DIR}/gfx_d3d/r_add_staticmodel.h"
    "${SRC_DIR}/gfx_d3d/r_bsp.cpp"
    "${SRC_DIR}/gfx_d3d/r_bsp.h"
    "${SRC_DIR}/gfx_d3d/r_bsp_load_obj.cpp"
    "${SRC_DIR}/gfx_d3d/r_buffers.cpp"
    "${SRC_DIR}/gfx_d3d/r_buffers.h"
    "${SRC_DIR}/gfx_d3d/r_cmdbuf.cpp"
    "${SRC_DIR}/gfx_d3d/r_cmdbuf.h"
    "${SRC_DIR}/gfx_d3d/r_debug.cpp"
    "${SRC_DIR}/gfx_d3d/r_debug.h"
    "${SRC_DIR}/gfx_d3d/r_debug_alloc.cpp"
    "${SRC_DIR}/gfx_d3d/r_dobj_skin.cpp"
    "${SRC_DIR}/gfx_d3d/r_dobj_skin.h"
    "${SRC_DIR}/gfx_d3d/r_dpvs.cpp"
    "${SRC_DIR}/gfx_d3d/r_dpvs.h"
    "${SRC_DIR}/gfx_d3d/r_dpvs_entity.cpp"
    "${SRC_DIR}/gfx_d3d/r_dpvs_sceneent.cpp"
    "${SRC_DIR}/gfx_d3d/r_dpvs_static.cpp"
    "${SRC_DIR}/gfx_d3d/r_draw_bsp.cpp"
    "${SRC_DIR}/gfx_d3d/r_draw_bsp.h"
    "${SRC_DIR}/gfx_d3d/r_draw_material.cpp"
    "${SRC_DIR}/gfx_d3d/r_draw_material.h"
    "${SRC_DIR}/gfx_d3d/r_draw_method.cpp"
    "${SRC_DIR}/gfx_d3d/r_draw_method.h"
    "${SRC_DIR}/gfx_d3d/r_draw_shadowable_light.cpp"
    "${SRC_DIR}/gfx_d3d/r_draw_shadowable_light.h"
    "${SRC_DIR}/gfx_d3d/r_draw_staticmodel.cpp"
    "${SRC_DIR}/gfx_d3d/r_draw_staticmodel.h"
    "${SRC_DIR}/gfx_d3d/r_draw_xmodel.cpp"
    "${SRC_DIR}/gfx_d3d/r_draw_xmodel.h"
    "${SRC_DIR}/gfx_d3d/r_drawsurf.cpp"
    "${SRC_DIR}/gfx_d3d/r_drawsurf.h"
    "${SRC_DIR}/gfx_d3d/r_dvars.cpp"
    "${SRC_DIR}/gfx_d3d/r_dvars.h"
    "${SRC_DIR}/gfx_d3d/r_font.cpp"
    "${SRC_DIR}/gfx_d3d/r_font.h"
    "${SRC_DIR}/gfx_d3d/r_gfx.h"
    "${SRC_DIR}/gfx_d3d/r_image.cpp"
    "${SRC_DIR}/gfx_d3d/r_image.h"
    "${SRC_DIR}/gfx_d3d/r_imagedecode.cpp"
    "${SRC_DIR}/gfx_d3d/r_image_load_common.cpp"
    "${SRC_DIR}/gfx_d3d/r_image_load_db.h"
    "${SRC_DIR}/gfx_d3d/r_image_load_obj.cpp"
    "${SRC_DIR}/gfx_d3d/r_image_utils.cpp"
    "${SRC_DIR}/gfx_d3d/r_image_wavelet.cpp"
    "${SRC_DIR}/gfx_d3d/r_init.cpp"
    "${SRC_DIR}/gfx_d3d/r_init.h"
    "${SRC_DIR}/gfx_d3d/r_light.cpp"
    "${SRC_DIR}/gfx_d3d/r_light.h"
    "${SRC_DIR}/gfx_d3d/r_light_load_obj.cpp"
    "${SRC_DIR}/gfx_d3d/r_marks.cpp"
    "${SRC_DIR}/gfx_d3d/r_marks.h"
    "${SRC_DIR}/gfx_d3d/r_material.cpp"
    "${SRC_DIR}/gfx_d3d/r_material.h"
    "${SRC_DIR}/gfx_d3d/r_material_load_db.h"
    "${SRC_DIR}/gfx_d3d/r_material_load_obj.cpp"
    "${SRC_DIR}/gfx_d3d/r_material_override.cpp"
    "${SRC_DIR}/gfx_d3d/r_meshdata.cpp"
    "${SRC_DIR}/gfx_d3d/r_meshdata.h"
    "${SRC_DIR}/gfx_d3d/r_model.cpp"
    "${SRC_DIR}/gfx_d3d/r_model.h"
    "${SRC_DIR}/gfx_d3d/r_model_lighting.cpp"
    "${SRC_DIR}/gfx_d3d/r_model_lighting.h"
    "${SRC_DIR}/gfx_d3d/r_model_pose.cpp"
    "${SRC_DIR}/gfx_d3d/r_model_pose.h"
    "${SRC_DIR}/gfx_d3d/r_model_skin.cpp"
    "${SRC_DIR}/gfx_d3d/r_model_skin.h"
    "${SRC_DIR}/gfx_d3d/r_model_skin_sse.cpp"
    "${SRC_DIR}/gfx_d3d/r_outdoor.cpp"
    "${SRC_DIR}/gfx_d3d/r_outdoor.h"
    "${SRC_DIR}/gfx_d3d/r_pixelcost_load_obj.cpp"
    "${SRC_DIR}/gfx_d3d/r_pixelcost_load_obj.h"
    "${SRC_DIR}/gfx_d3d/r_pretess.cpp"
    "${SRC_DIR}/gfx_d3d/r_pretess.h"
    "${SRC_DIR}/gfx_d3d/r_primarylights.cpp"
    "${SRC_DIR}/gfx_d3d/r_primarylights.h"
    "${SRC_DIR}/gfx_d3d/r_reflection_probe.cpp"
    "${SRC_DIR}/gfx_d3d/r_reflection_probe.h"
    "${SRC_DIR}/gfx_d3d/r_reflection_probe_load_obj.cpp"
    "${SRC_DIR}/gfx_d3d/r_rendercmds.cpp"
    "${SRC_DIR}/gfx_d3d/r_rendercmds.h"
    "${SRC_DIR}/gfx_d3d/r_rendertarget.cpp"
    "${SRC_DIR}/gfx_d3d/r_rendertarget.h"
    "${SRC_DIR}/gfx_d3d/r_scene.cpp"
    "${SRC_DIR}/gfx_d3d/r_scene.h"
    "${SRC_DIR}/gfx_d3d/r_setstate_d3d.cpp"
    "${SRC_DIR}/gfx_d3d/r_setstate_d3d.h"
    "${SRC_DIR}/gfx_d3d/r_shade.cpp"
    "${SRC_DIR}/gfx_d3d/r_shade.h"
    "${SRC_DIR}/gfx_d3d/r_shadowcookie.cpp"
    "${SRC_DIR}/gfx_d3d/r_shadowcookie.h"
    "${SRC_DIR}/gfx_d3d/r_sky.cpp"
    "${SRC_DIR}/gfx_d3d/r_sky.h"
    # r_sky_load_obj.cpp folded into r_sky.cpp in KIWI
    "${SRC_DIR}/gfx_d3d/r_spotshadow.cpp"
    "${SRC_DIR}/gfx_d3d/r_spotshadow.h"
    "${SRC_DIR}/gfx_d3d/r_state.cpp"
    "${SRC_DIR}/gfx_d3d/r_state.h"
    "${SRC_DIR}/gfx_d3d/r_state_utils.cpp"
    # r_staticmodel_load_obj.cpp folded into r_staticmodel.cpp in KIWI; use r_staticmodel.cpp
    "${SRC_DIR}/gfx_d3d/r_staticmodel.cpp"
    "${SRC_DIR}/gfx_d3d/r_staticmodelcache.cpp"
    "${SRC_DIR}/gfx_d3d/r_sunshadow.cpp"
    "${SRC_DIR}/gfx_d3d/r_sunshadow.h"
    "${SRC_DIR}/gfx_d3d/r_utils.cpp"
    "${SRC_DIR}/gfx_d3d/r_utils.h"
    "${SRC_DIR}/gfx_d3d/r_warn.cpp"
    "${SRC_DIR}/gfx_d3d/r_water.cpp"
    "${SRC_DIR}/gfx_d3d/r_water.h"
    "${SRC_DIR}/gfx_d3d/r_water_load_obj.cpp"
    "${SRC_DIR}/gfx_d3d/r_workercmds.cpp"
    "${SRC_DIR}/gfx_d3d/r_workercmds.h"
    "${SRC_DIR}/gfx_d3d/r_xsurface.cpp"
    "${SRC_DIR}/gfx_d3d/r_xsurface.h"
    # r_xsurface_load_obj.cpp folded into r_xsurface.cpp in KIWI

    # ── Build 10 additions ─────────────────────────────────────────────────────
    "${SRC_DIR}/gfx_d3d/r_cmds.cpp"
    "${SRC_DIR}/gfx_d3d/r_cmds.h"
    "${SRC_DIR}/gfx_d3d/r_draw_lit.cpp"
    "${SRC_DIR}/gfx_d3d/r_draw_lit.h"
    "${SRC_DIR}/gfx_d3d/r_draw_pixelshader.cpp"
    "${SRC_DIR}/gfx_d3d/r_draw_sunshadow.cpp"
    "${SRC_DIR}/gfx_d3d/r_draw_sunshadow.h"
    "${SRC_DIR}/gfx_d3d/r_dpvs_dynmodel.cpp"
    "${SRC_DIR}/gfx_d3d/r_fog.cpp"
    "${SRC_DIR}/gfx_d3d/r_fog.h"
    "${SRC_DIR}/gfx_d3d/r_screenshot.cpp"
    "${SRC_DIR}/gfx_d3d/r_screenshot.h"
    "${SRC_DIR}/gfx_d3d/r_texturemem.cpp"
    "${SRC_DIR}/gfx_d3d/r_texturemem.h"
    "${SRC_DIR}/gfx_d3d/r_workercmds_common.cpp"
    "${SRC_DIR}/gfx_d3d/r_workercmds_common.h"
    "${SRC_DIR}/gfx_d3d/rb_logfile.cpp"
    "${SRC_DIR}/gfx_d3d/rb_logfile.h"
    "${SRC_DIR}/gfx_d3d/rb_shadowcookie.cpp"
    "${SRC_DIR}/gfx_d3d/rb_shadowcookie.h"
    "${SRC_DIR}/gfx_d3d/rb_showcollision.cpp"
    "${SRC_DIR}/gfx_d3d/rb_showcollision.h"
    "${SRC_DIR}/gfx_d3d/rb_stats.cpp"
    "${SRC_DIR}/gfx_d3d/rb_stats.h"
    "${SRC_DIR}/gfx_d3d/rb_uploadshaders.cpp"
    "${SRC_DIR}/gfx_d3d/rb_uploadshaders.h"
)
