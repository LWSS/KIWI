# RADIANT_UX_DESIGN — Plasticity-Style 3D Brush Editing Overhaul

This is the Phase-5 design doc reserved by `RADIANT_UI_REWORK_PLAN.md`. It
supersedes the earlier draft plan and grounds it in what actually exists in
this repo. Divergence from 1:1 IDA fidelity is SANCTIONED here, feature by
feature — but only in the interaction/UI layer. Load-bearing cores (map
serialization, winding math, CSG, texture lock, undo) stay the ported,
IDA-faithful code.

## Project goal

Keep every Radiant concept — brushes, faces, vertices, edges, entities,
models, patches, prefabs, the .map format, renderer/game compatibility — and
replace the *editing workflow* with a fast, 3D-first, direct-modeling
interface in the spirit of Plasticity and TrenchBroom.

The target loop for every operation:

```text
select geometry → press one shortcut → manipulate live in 3D
→ snap or type an exact value → commit
```

never:

```text
change mode → open a 2D viewport → pick a tool → open a dialog → apply
```

No CAD feature-history tree. Editing stays destructive/direct.

## Licensing note (changed from the draft)

The local Plasticity tree at `F:\msvcproj\KIWI\plasticity` is **LGPLv3**
(© Nicholas Kallen). LGPLv3 is GPL-compatible: its code **may be ported into
this GPLv3 project**, not merely studied — keep the copyright notice and note
provenance per file. It is TypeScript/three.js, so reuse is transliteration
rather than copy-paste, but the important architecture files are directly
minable:

- `src/command/Command.ts`, `CommandExecutor.ts` — modal command lifecycle
- `src/command/point-picker/` — the snap/point-input system (the crown jewel)
- `src/command/SnapPresenter.ts`, `SnapIndicator.ts` — snap feedback UX
- `src/selection/SelectionModeSet.ts`, `ChangeSelectionExecutor.ts`,
  `Boxcaster.ts` — selection modes, click resolution, directional box select
- `src/command/AbstractGizmo.ts`, `MiniGizmos.ts` — gizmo interaction

iw3xo-radiant remains design-reference ONLY (no license — never copy).

## Ground rules carried over from the port

- User builds all gates; agents never build.
- 32-bit / D3D9 / ImGui-docking shell with RTT viewports. Don't balloon
  memory (address space), don't fight the device-reset rules already learned.
- Every touched legacy core: read the logic, check suspicious code vs IDA or
  log it in `RADIANT_KNOWN_ISSUES.md`.
- Each deliberate behavior divergence gets a decision entry in this file's
  appendix (date + rationale), same protocol as the rework plan.

---

# Part I — Architecture

The overhaul is a **new layer over the ported cores**, not a rewrite. The
single most important bridging decision is #1 below; everything else hangs
off it.

## 1. Selection core + legacy adapter

All ~50 ported operation cores consume the legacy selection globals:
`selected_brushes` (sentinel list), `selFace[]`, patch/entity selection
state, `QE_SingleBrush()` checks. They must keep working unmodified.

New model:

```cpp
enum sel_kind_t { SEL_VERTEX, SEL_EDGE, SEL_FACE, SEL_OBJECT };

struct sel_item_t
{
    sel_kind_t kind;
    brush_t   *brush;      // owning object (brush/patch instance/entity)
    int        faceIndex;  // SEL_FACE / SEL_EDGE
    int        edgeIndex;  // SEL_EDGE (index into winding pair)
    int        vertIndex;  // SEL_VERTEX (winding vert or patch ctrl point)
};

struct selection_t
{
    std::vector<sel_item_t> items;
    sel_item_t              active;   // last-clicked, for active-item ops
};
```

**Adapter rule:** every mutation of `selection_t` immediately syncs the
legacy globals (object selections → `selected_brushes`, face selections →
`selFace`, etc.), and legacy code that changes selection (Select_Deselect,
CSG results, clipper) is hooked to rebuild `selection_t`. One function each
way, called at defined sync points — never lazily. This lets old commands
(hide, CSG, texture apply, clipper) and new commands share one selection
without porting them twice.

Selection modes (the 1–5 filters) live here as a `sel_kind_t` mask applied
**during picking**, not by post-converting an object pick.

## 2. Unified pick API

Build one entry point over the already-ported `Test_Ray` chain
(brush/model/prefab/patch):

```cpp
struct pick_result_t
{
    bool        valid;
    sel_item_t  item;        // what was hit, at current filter granularity
    vec3_t      point;       // world hit point
    float       screenDist;  // px from cursor (0 for area hits)
};

pick_result_t Pick(const ray_t &ray, unsigned kindMask);
```

- Faces/objects: ray intersection (Test_Ray as-is).
- Edges/vertices: **screen-space tolerance in pixels** (project candidates,
  rank by px distance; ~8 px verts, ~6 px edges). Never world-unit
  tolerance — it breaks at distance.
- Candidates come from the frame's visible set (the editor already walks
  active+selected brush lists for drawing) — no new spatial structure until
  profiling demands one. Budget: hover pick runs once per frame max.
- Patch control points are SEL_VERTEX items (the terrain-point drag core
  already manipulates them; picking reuses its point set).
- Prefab/model hits resolve to SEL_OBJECT (their entity), transforms edit
  origin/angles keys — no attempt to edit inside prefab instances in v1.

Hover state, selected state, active item, and snap markers all render from
this one API's results.

## 3. Command system — extend, don't reinvent

The repo already has the pieces the draft plan proposed to build:

- `g_radiantCommands` (mainfrm.cpp, ~187 entries, mutable copy remapped by
  `LoadCommandMap`) — name, command id, vk+mods.
- `Radiant_ExecCommand` / `Radiant_DispatchCommandDirect` (313-case) — one
  dispatch path shared by menus + hotkeys.
- `Radiant_TryHotkey` — vk+mods lookup.

The command palette, menus, toolbar, and hotkeys all route through this
table. Extend the entry with palette metadata instead of adding a second
registry:

```cpp
struct RadiantCommandEx   // additive; g_radiantCommands stays the source of truth
{
    const char *displayName;   // "Extrude Region"
    const char *category;      // "Modeling"
    unsigned    selKindMask;   // enables context-aware availability
    bool      (*canExecute)(const selection_t &);   // grey-out / palette filter
};
```

New modal commands get ids in a reserved range and dispatch through the same
switch. **One command id = one behavior everywhere** (menu, palette, hotkey,
toolbar, gizmo).

## 4. Modal command framework

```cpp
class EditorCommand
{
public:
    virtual bool CanExecute(const selection_t &);
    virtual void Begin(const selection_t &);          // takes undo snapshot
    virtual void MouseMove(const pick_result_t &, const snap_result_t &);
    virtual bool KeyDown(int vk, unsigned mods);      // axis locks, numerics
    virtual void Commit();
    virtual void Cancel();                            // exact restore
};
```

Exactly one modal command owns editing input at a time (`g_activeCommand`).
Lifecycle: selection → invoke → live manipulation → commit/cancel.

**Preview strategy (changed from the draft):** brush edits mutate the live
geometry with an undo snapshot taken at `Begin()` — the same
snapshot-then-mutate pattern the existing `Drag_*` code and `Undo_Begin/
Undo_AddBrush*/Undo_End` brackets already implement. No cloned "preview
geometry" for brush ops; Radiant's undo is snapshot-based and this falls out
for free (`Cancel()` = restore snapshot; `Commit()` = close the bracket —
ONE undo record per gesture, never per mouse-move). Cloned previews are
reserved for ops that *create* geometry (extrude, CSG results) where the
preview is drawn as overlay until commit.

**Camera navigation stays live during modal commands.** Orbit/pan/zoom must
work mid-extrude, mid-move. This is core to the Plasticity feel and forces
the input arbitration below.

**Gesture lifecycle — REWRITTEN in shakeout E (2026-08-09).** The original
rule was "Escape cancels; Enter / LMB-click commits", and live testing killed
it: an LMB release ending the whole edit means every accidental click is a
committed edit, and there is no way to nudge a value after the first pass.
The user's own words: *"Releasing an action shouldn't commit it, it should
just pause the wip move. A right click OR an enter press confirms it."*

That is also what Plasticity does. `AbstractGizmo.execute` defaults to
`Mode.Persistent` (src/command/AbstractGizmo.ts:92), and the state machine's
`pointerUp` only resolves the gizmo's promise when Persistent is *off*
(AbstractGizmo.ts:135-144) — pointer-up ends the *drag*, not the *command*.
The command is finished by a separate explicit signal, the `gizmo:finish`
registry command wired at AbstractGizmo.ts:117-120. Releasing the mouse
parks the edit; something else ends it.

So a live modal command now has two states:

| state | meaning |
| --- | --- |
| **HOT** | geometry follows the cursor. Every command starts here. |
| **PAUSED** | preview frozen, value latched, `MouseMove` stops reaching the command. |

| edge | driven by |
| --- | --- |
| HOT → PAUSED, LMB press in the viewport | `KiwiCmd_MouseButton` |
| HOT → PAUSED, gizmo-handle release | `KiwiGizmo_Release` (KG_GIZMO arm) |
| PAUSED → HOT (+`Rebase()`), *any* LMB press | `KiwiCmd_MouseButton` |
| PAUSED → HOT (+`Rebase()`), gizmo-handle press | `KiwiGizmo_MouseDown` |
| either → **COMMIT**, RMB *click* (< 4 px travel) or Enter | `kiwi_viewport.cpp` KG_LOOK release arm → `KiwiCmd_Confirm`; `KiwiCmd_KeyDown` rung 5 |
| either → **CANCEL**, Esc (numeric-clear first) | `KiwiCmd_KeyDown` rungs 2 + 4 |

`Rebase()` is a new command virtual: on a PAUSED → HOT edge it re-latches
whatever delta-from-start state the command maps against, so resuming does
not teleport the geometry by however far the cursor wandered while parked.

**Multi-click tools (`WantsClicks`) do not pause.** A click there *places a
point* — that grammar is unchanged, byte for byte. They gain only the new
confirm: RMB-click now does exactly what Enter does, routed through the same
`KiwiCmd_KeyDown(VK_RETURN)` ladder so a command that vetoes Enter (the
polyline's "finish the chain") vetoes an RMB click identically.

**The resume rule is deliberately the broadest one:** *any* LMB press inside
the camera image resumes, not just a press on the dragged geometry. A press
on a gizmo handle resumes and grabs that handle; a press anywhere else
resumes the free drag. Requiring a hit on the geometry would need a pick
against a preview that may not be geometry yet (a construction line, an
unplaced primitive) and would leave the user with a command they cannot get
back into.

## 5. Input arbitration (state machine, written down)

Priority order every frame, in the RTT camera viewport:

```text
1. active modal command   — gets MouseMove/KeyDown; LMB=pause/resume,
                            RMB-click or Enter=confirm, Esc=cancel  (shakeout E)
2. camera navigation      — MMB orbit, RMB pan/free-look, wheel dolly;
                            ALWAYS available, including during (1)
3. gizmo hot-spot         — LMB-down on a gizmo handle starts its drag
4. selection              — LMB click = pick; LMB drag = box select
5. hover                  — pick for highlight + snap preflight
```

Rules learned the hard way in the RTT work and kept: input dispatch is
post-present, wheel captured during-frame, force-abort runs the owner's own
teardown and never pre-empts the normal release edge, panels driving
viewport gestures never auto-close.

**Deliberate break with classic Radiant:** LMB click/drag NEVER moves
geometry directly (classic drag-to-move is gone from the modern keymap;
movement is G / gizmo only). Clicking must never surprise-move brushes.

## 6. SnapManager

One global subsystem every command queries — but grown **with its
consumers**, not big-bang (each phase adds the target types its tools need).

```cpp
enum snap_type_t {
    SNAP_NONE, SNAP_GRID, SNAP_VERTEX, SNAP_EDGE_MID, SNAP_EDGE,
    SNAP_FACE, SNAP_FACE_CENTER, SNAP_ENDPOINT, SNAP_INTERSECTION,
    SNAP_AXIS, SNAP_CPLANE, SNAP_ANGLE
};

struct snap_result_t
{
    bool        valid;
    snap_type_t type;
    vec3_t      position;
    sel_item_t  source;      // what we snapped to (for the marker + label)
};
```

Pipeline: collect candidates near the mouse ray (same visible-set source as
Pick) → rank by (type priority, screen px distance) → best candidate →
draw marker + text tag ("mid", "vert", "45°") → feed active tool.

- **ON by default; CTRL held = temporarily off** (intentional, even though
  inverted vs other apps).
- Grid snap is one provider among many; grid size keys keep working.
- Point-type snaps (vert/endpoint/mid/intersection) outrank line/area snaps
  at equal distance; px radius ~10.
- Port the ranking/priority logic from `plasticity/src/command/point-picker/`
  (LGPL-ok) rather than deriving it from scratch — it encodes years of feel
  tuning.

**Numeric hygiene (CoD-specific):** snapping to existing geometry inherits
its precision, which is fine; *free* off-grid placement is what breeds
microleaks and ugly plane equations. Commit paths quantize near-integer
coordinates (epsilon ~1e-3) to exact integers, and face push/pull along an
axis-aligned normal stays exactly on-grid. `Brush_SnapPlanepts` exists for
explicit re-snap.

## 7. Construction geometry store + persistence (missing from the draft)

Editor-only object kinds: line, polyline, rect, circle, arc, spline —
points + segments, participating in pick/selection/snap/transform.

- Lives in its own list, drawn by its own pass. **Never enters map data**
  until an explicit convert/extrude produces brushes.
- **Persistence: sidecar file** `<mapname>.kiwi` (text, versioned) saved
  alongside the .map. Never serialize into the .map — stock compilers and
  stock Radiant must keep loading our maps untouched. Construction planes
  and (later) groups also live there.
- Undo participates through the same bracket system (construction edits are
  cheap snapshots).
- **Points are WORLD-SPACE (shakeout H — see §34).** Ruling 3 originally stored
  them in each object's own plane basis; that made 3D lines impossible and
  projected every snapped point off the thing it was snapped to. Planarity is now
  *derived and tested* per object (`KiwiCon_FitPlane`, Newell, 0.5 units), and
  circles/arcs alone keep an authoritative stored plane. Sidecar format `KIWI2`;
  `KIWI1` files still load.

## 8. Region detection

A closed, coplanar loop of construction segments becomes a **Region** —
a translucent light-blue fill, selectable in Face mode, extrudable.

- Recompute incrementally on point edit; fill disappears when the loop opens.
- Coplanarity within editor tolerance; loop = each endpoint shared by
  exactly two segments.
- **Shakeout H:** coplanarity is a *point-vs-plane* test against a plane fitted
  over the chain (`KCON_PLANE_FIT_DIST`, 0.5 units), not a comparison of two
  stored planes — see §34. Endpoint welding is 3D.
- v1: single outer loop, no holes. Holes/nested loops are a later decision.

### ROUND K — regions form from ENCLOSED AREAS, not from matched endpoints

USER DIRECTIVE, verbatim: *"When creating a closed off line shape, it currently
only closes off when the points are perfectly at corners, it should do it whenever
lines close off a section even if they extend further (see pic)."* The picture is
four lines crossing like a hash — `#` — with the quad in the middle asked to be a
region even though all four lines run past the corners they help make.

Everything above is an **endpoint** rule, and no tolerance fixes that: in the `#`
case no endpoint touches any other endpoint, every meeting is a mid-span crossing,
and `KiwiRegion_ChainWalk` sees four disjoint degree-1 pairs. So §8 gains a third
pass, `kiwi_arrange.h/.cpp`:

1. **Fragment.** Project the coplanar group's world segments into the group's
   plane and split every segment at every mutual crossing (proper crossings *and*
   T-junctions). The crossing test is `KiwiCon_SegSegClosest` at `KCON_ISECT_DIST`
   — the trim tool's own solve, in 3D on the unprojected segments, so the
   projection can never invent a crossing that is really half a unit apart along
   the normal.
2. **Face-walk.** Weld the fragment endpoints into nodes at `KREG_JOIN_DIST`, sort
   each node's outgoing half-edges by angle, and trace minimal cycles by always
   taking the *clockwise-next* half-edge after the twin. That rule traces every
   face with its interior on the left, so a **bounded** cell comes out CCW
   (positive signed area) and the single unbounded outer face of each connected
   component comes out CW and is dropped by sign. A dangling spur — which is
   exactly what the `#` picture is full of — is walked out and straight back along
   the same edge pair, contributing zero area.

This is Plasticity's own shape, read out of the LGPL tree rather than guessed:
`src/editor/curves/PlanarCurveDatabase.ts` states the fragment algorithm in its
`add()` docstring (:24-30) and implements it with `IntersectWithAll` (:77) +
per-crossing `Trimmed` (:122-134); `src/editor/curves/RegionManager.ts` then hands
the flat segment list to `ContourGraph.OuterContoursBuilder` and
`ActionRegion.GetCorrectRegions` (:33-35). Plasticity has no endpoint-chaining
region path at all.

**Both paths ship.** The exact-loop passes stay (they are cheap, they own the
parametric circle/arc case, and they survive the arrangement's caps), the
arrangement runs unconditionally over the same group, and duplicates are dropped
by world centroid (within `KREG_JOIN_DIST`) + |area| (within 1%). Making them
exclusive would mean a store where four lines chain *and* a fifth crosses them
shows only half its faces.

**CLOSED objects join the arrangement group** and nothing else: they cannot chain,
but a rectangle with a line drawn across it encloses two cells and this is the only
pass that can say so.

**Caps** (per coplanar group, evaluated once per store-generation bump, never per
frame). An overflow logs and yields nothing *for that group* — a partial
arrangement is a wrong arrangement. Segments 256 (the O(n²) split step: 65k pair
tests), fragments 1024, nodes 1024, cells 64. Complexity is O(S²) for the split,
O(N²) worst case for the weld and O(H log H) for the angular sorts.

### ROUND K — a region is SELECTABLE and EXTRUDABLE

USER DIRECTIVE: *"Also I can't grab the light blue part as if its a face. It should
be extrudable into a new solid(brush)."* Clicking a region fill selects it and
auto-enters `Extrude Region` PAUSED with the §14 lollipop on the region plane; `E`
with a region selected does the same. The click arbitration: the ray must be inside
the cell, a **closer brush face wins**, and a point/line brush hit (vertex, edge)
wins outright at any depth. A region win leaves the brush selection alone, exactly
as a construction-line win does.

Region selection is a third KIWI-owned selection that never enters `selection_t`,
and its state is a **world centroid rather than an index** — regions are derived
and re-derived on every store change, and all three passes append, so an index goes
stale the moment a line moves. Confirming the extrude lands the new brush selected
and drops the region, which is Plasticity's own tail (`ExtrudeCommand.ts:65-68`
adds the results and removes the source faces and regions). The source lines stay:
they are scaffolding, and deleting them would be a second, unasked-for edit.

---

# Part II — UX specification

## 9. Viewport layout

3D camera = the central, dominant viewport. XY/XZ/YZ + Z views become
optional dock tabs (they already render via RTT; this is a default-layout
change, not new code). Texture/console/inspector panels keep their docks.
Classic users can restore the 4-view layout from the View menu.

**Shipped (shakeout B).** The draft's "optional dock tabs" turned out to be too
weak — the user's ruling was "3d cam view IS the new primary way of working with
the editor. We want to obsolete the 2d view and z views completely (but leave
them openable in the topbar somehow via a windows dropdown)" plus "Hide the
texture and 'KIWI Imgui shell' tabs by default". So §9 is implemented as:

- **Default layout: two nodes.** Camera fills the entire centre; the console is
  an 18% strip along the bottom. Nothing else is docked at all
  (`ImGuiShell_BuildDefaultDockLayout`). The dock ini name went
  `kiwi_dock3.ini` → `kiwi_dock4.ini` so existing installs actually rebuild.
- **Per-window visibility flags** (`kiwi_windows.cpp`), persisted in
  `kiwi_radiant.ini` `[KiwiWindows]`. Defaults: 2D View **off**, Z **off**,
  Textures **off**, KIWI ImGui shell **off**, Console **on**. The camera has no
  flag — it is the editor now, and a control that can hide the last viewport is
  a trap.
- A closed window skips `ImGui::Begin` **entirely**, and — the point of doing it
  in the shell rather than with `ImGuiWindowFlags` — its RTT render is skipped
  too, so a hidden 2D / Z / texture view costs nothing per tick instead of a
  full off-screen scene. An open window passes its flag as `Begin`'s `p_open`,
  so the title-bar ✕ and the menu write the same state.
- **A native "Windows" popup** appended to the frame's `IDR_MENU_QUAKE3` menu at
  boot, with a check mark per window. Items carry KIWI instant command ids
  (34007/34008/34009/34018/34019) and route through the ordinary
  `Radiant_ExecCommand` → `Radiant_DispatchCommandDirect` KIWI arm; the check
  marks are re-synced through `Radiant_CheckMenu` on every toggle.
- Re-opening a window docks it into the live dockspace root
  (`SetNextWindowDockID`) rather than restoring whatever floating position the
  ini last remembered.

The "restore the 4-view layout from the View menu" line above is superseded:
the Windows menu is that control, one window at a time.

## 10. Camera

```cpp
struct camera_orbit_state
{
    vec3_t        look_at;
    float         distance;
    orientation_t orientation;   // from existing camera angles
};
```

- **MMB drag: orbit** around `look_at`. On orbit-begin, ray-trace under the
  cursor; hit → re-center `look_at` there (miss → keep current).
- **RMB drag: pan** (truck — moves camera AND look_at). **Alt+RMB drag:
  mouselook** (angles only). **Shift+RMB / Shift+MMB drag: pan** as well.
  **RMB click: confirm a live modal command, else the classic context menu.**
  *(D-1 resolved 2026-08-09 to "RMB = mouselook"; REVISED the same day in
  shakeout E after live testing — the user asked for pan on plain RMB. D-2
  resolved with it. The appendix carries both tables.)*
- **Wheel: dolly** toward cursor ray, with the step's reference distance
  smoothed across consecutive notches (shakeout E) so crossing a silhouette
  edge mid-scroll ramps instead of stepping.
- **Pan scale is anchored at press time** on whatever surface is under the
  cursor, not on the orbit pivot (shakeout E — the pivot's depth is invisible
  to the user and swung the pan speed by whatever factor it happened to be off
  by). The grabbed point then tracks the cursor ~1:1.
- **LMB: select only** (or a gizmo handle — §14).
- **Fly keys:** W/A/S/D/Q/E while RMB is held; the bare arrow keys any time the
  camera image has the cursor. Speed is the classic `MoveSpeed` pref read as
  units/second, dt-scaled, ×3 with Shift.
- Classic free-look (`camera_mode` drive) remains available with the modern-input
  master toggle OFF; the orbit + mouselook camera is the Modern default.

**Orientation view-cube** (shakeout A, user request): a Blender-style axis widget
in the camera image's top-right — six balls (±X ±Y ±Z) laid out by projecting the
world axes through the current camera basis, positive ends filled and labelled.
Clicking one looks along that axis from the current pivot at the current
distance. ±Z keep the current yaw and use pitch ∓89° (the pole clamp every other
consumer of this camera already lives with). *(Shakeout D rebuilt it as a real
projected cube with 14 snap targets; shakeout I made the body grey.)*

Implementation: a thin layer producing the same camera origin/angles the
existing `Cam_Draw` consumes — the render path doesn't change.

### 10.1 Orthographic / perspective toggle (ROUND M)

User directive: *"while at the perfect axis align plane angle, it doesn't show a
box as just a square. I think this is due to the renderer. We need an
orthographic and perspective camera toggle. Add that in the top right somewhere as
a button in the 3d camera viewport."*

This one **does** change the render path, minimally and in one place —
`CamWnd_SetupScene` builds a different `GfxMatrix`. It was verified swappable
before it was written: the 2D views already push an orthographic projection
through the same `R_Ed_SetSceneParms` entry point, so every consumer downstream
(view-projection setup, the inverse-VP, `R_DeriveNearPlaneConstantsForView`)
already handles a w-free projection.

- **Half-height** `H = distance-to-pivot × tan(fov/2) × 0.75` — exactly the
  perspective frustum's half-height *at the pivot*, so the toggle is
  scale-continuous there and the wheel keeps zooming (the dolly moves `s_dist`,
  `H` follows).
- **Depth** is symmetric about the eye (±131072). In an orthographic view the eye
  *point* is arbitrary along the view axis, so a one-sided near plane would clip
  away geometry the user is looking straight at.
- **Picking is a matched pair and both halves moved.** The ray builder gains a
  lateral *origin* offset with `dir = vpn`; the projection inverse divides by the
  constant pivot distance instead of by the point's own depth. Written out in
  full in `camwnd.cpp` at `Ed_CameraCalcRayDir` — a picker whose forward and
  inverse disagree breaks every click silently, so the pair is stated once and
  referenced from `kiwi_pick.cpp`. `KiwiCam_WorldPerPixel` gains the matching
  depth-independent arm, and the three private copies of that scale that had
  accumulated (`kiwi_snap`, `kiwi_construct`, `kiwi_hover`) are now all aliases
  over it.
- **Control**: an `ORTHO`/`PERSP` pill under the view cube (draw-list only, own
  hover/click, drawn outside the cube's own show toggle), plus
  `View → Orthographic Camera` and the palette command `KiwiViewOrtho`.
- **Known limits** (logged): the cubic cull planes used by the model/prefab pass
  are still the perspective cone, and the *classic*-profile 3D marquee and terrain
  cursor still build perspective rays. Brush geometry and the modern box-select
  are unaffected.

## 11. Selection modes + hotkey reality (changed from the draft)

Modes: **1 Point, 2 Edge, 3 Face, 4 Object, 5 Everything** — a pick-time
`kindMask`, shown as a Plasticity-style chip row in the viewport's top-left.

The draft ignored that most of its bindings are TAKEN in classic Radiant:
**1–9 = grid size**, R = mouse-rotate, S/N/O/F = inspectors, X = clipper,
V = vertex mode, Esc = deselect. Rebinding blind would wreck muscle memory
and silently shadow ported features.

**Solution: keymap profiles**, built on the existing `LoadCommandMap`
remap infrastructure (user remapping already round-trips through it):

- `keymap_modern` (default for the overhaul): 1–5 selection modes, G/R/S
  transforms, F command palette, TAB grid-size popup (or [ ] step),
  inspectors move to Shift-chords.
- `keymap_classic`: today's bindings, untouched; new features reachable via
  palette + menus only.

Every new feature must be reachable in BOTH keymaps (palette guarantees
this). Ship a printed conflict table before flipping the default.

## 12. Box selection

LMB drag = box select, honoring the current mode's kindMask:

- **Left→right: containment** (fully enclosed only).
- **Right→left: crossing** (anything touched).
- Shift adds, Ctrl toggles/removes (match click modifiers).
- Mine `Boxcaster.ts` for the containment/crossing tests per kind.

## 13. Transforms — G / R / S

Modal, context-aware on the current selection:

| keys | object sel | face sel | edge sel | vertex sel |
|------|-----------|----------|----------|------------|
| G | move objects | push/pull along normal (default) or free-move | move edge (2 planes) | move vert |
| R | rotate objects | rotate face plane (later) | — v1 | — |
| S | scale objects | — v1 | — | — |

- **Axis locks during transform:** X/Y/Z constrain to axis; Shift+X/Y/Z
  constrain to the perpendicular plane. Second press of same axis cycles
  world→local (later).
- **Numeric entry:** typing digits/`-`/`.` during a modal transform builds a
  value shown in the viewport HUD; Enter commits exactly. Grammar v1: single
  scalar (distance/degrees/factor). `G X 64 ⏎`, `R Z 45 ⏎`, `S 2 ⏎`.
  Distances are read and displayed in **inches** (§17's units layer).
- **§13a NAMED FIELDS + Tab cycling (shakeout E).** User directive: *"allow
  pressing of [tab] to go into the length box and type in an exact length.
  Pressing [tab] again should cycle to the angle too when applicable to the
  action."* A command declares its fields once
  (`KiwiEditorCommand::NumericFields`, a static `kiwiNumField_t[]` of
  label + kind); Tab / Shift+Tab cycle the focus, digits edit the focused
  field, Esc clears the focused field before it reaches the cancel rung.
  Kinds are `LENGTH` / `ANGLE` / `FACTOR` / `COUNT` and drive **formatting
  only** — every field still goes through `Units_FromDisplay`, so no existing
  command's arithmetic changed. A command that declares nothing keeps the
  single unnamed scalar, byte for byte.

  | command | fields |
  | --- | --- |
  | Move (G), incl. face push/pull | `length` |
  | Rotate (R) | `angle` |
  | Scale (S) | `factor` |
  | Extrude Region | `length` |
  | Box | `size` |
  | Cylinder / Sphere / Cone | `radius` → `height` (relabelled per stage) + `sides` |
  | Line, Polyline | `length`, `angle` (editable bearing) |
  | Rect / Circle / Polygon / Arc / Spline | `length` |
  | Bevel, Inset | `distance` |
  | Array (linear/radial) | `count` |
  | Texture Shift / Rotate / Scale | `shift` / `angle` / `steps` |

  Move stays **length-only** in v1: G's unconstrained direction rule is
  already "the ground-plane projection of the mouse direction" (below), and a
  second, differently-defined bearing layered on that is how a grammar stops
  being predictable. Press an axis key, then type a length.
- **§13b VALUE BUBBLE (shakeout E).** User directive: *"The distance unit
  should also be somewhere near the line/extrusion/whatever (see pic)"* —
  Plasticity pins a small pill carrying the live value next to the dragged
  geometry, secondary field beside it. A command reports where its action
  geometry is via `BubbleAnchor` (default: the last snap point, which for a
  drawing tool IS the moving end of the segment) and the live value of each
  field via `NumericFieldValue`; the framework projects the anchor with
  `Pick_WorldToImage` and draws one row per field, focused row caret-marked.
  It **replaces nothing** — the bottom-centre HUD stays as the verbose line.
- Both the gizmo and G/R/S feed the SAME command object — one code path,
  one snap query, one undo bracket.

## 14. Gizmo

*Amended in shakeout A (2026-08-09): ImGuizmo is NOT used, and v1 is
translate-only. Rationale below; the original proposal is kept for the record.*

~~Use **ImGuizmo (MIT)** for object-mode move/rotate/scale on day one (it's
ImGui-native and D3D9-friendly), wrapped so its output drives the same modal
command as G/R/S.~~ Three structural problems killed the wrapper:

1. ImGuizmo draws into an **ImDrawList in screen space** over a matrix it is
   handed. Every other overlay in this layer draws in the `Cam_Draw` **world**
   tail through `kiwi_lines` — which is the ported `R_Add3DLine` path, budgeted,
   and correctly ordered against the selected-outline depth clear. A screen
   overlay could not share that ordering.
2. It wants a view/projection **matrix pair**. This camera is origin+angles, and
   the projection is assembled inside `CamWnd_SetupScene` from the binary's own
   constants; reconstructing a matrix for ImGuizmo is a second source of truth
   for the projection.
3. v1 is **translate only** — rotate and scale stay on R and S, which §14 already
   allowed for ("the gizmo is optional chrome"). That is ~120 lines of geometry
   in `kiwi_gizmo.cpp` versus a new third-party dependency plus a matrix bridge.

The shipped gizmo: 3 axis arrows (70 px, screen-constant), 3 plane corner-Ls
(22 px), 1 free-move square (8 px), hit-tested in **screen pixels** through
`Pick_WorldToImage` at ~8 px tolerance, resolved centre → plane → axis. It is
visible when modern input is on, no modal command runs, and the selection has a
movable dominant kind; it is anchored at the point the G drag would pivot about,
per kind. Grabbing a handle is exactly `KiwiCmd_Start(KIWI_CMD_MOVE)` followed by
`KiwiXform_PresetMoveConstraint(...)` — the **same command object** as G, so the
snap, numeric entry, validity gate and single undo record are shared, as this
section always required. The one interaction difference: a handle grab commits on
**release** (drag-style, Plasticity behaviour) rather than on the next click.

Face/edge/vertex selections get the same translate gizmo rather than the proposed
"single arrow" special case, because the Move command is already context-aware —
a face grab on the free-move square pushes along the face normal by itself.

**ROUND K — the LOLLIPOP replaces the gizmo for one-axis pushes.** USER DIRECTIVE,
verbatim: *"when clicking a face on a solid(brush), it should auto enter the
extrusion mode and it should look like a lollipop (see pic). The lollipop should
also stay external to the face, it should move with the face so it doesn't get
buried after a grab. Also hide the move gizmo when extruding."* The picture is
Plasticity's extrude gizmo: a thin white circle on the face, a stem out along the
normal, a yellow ball on the end.

`kiwi_lollipop.h/.cpp` ships exactly that — ring 18 px **in the face plane** (so it
reads as lying on the surface), stem 48 px, ball 7 px radius (~14 px across), hit
radius 12 px — for the three one-degree-of-freedom gestures: face push/pull, region
extrude and face extrude. Two rules make it behave:

* **it rides.** The anchor is recomputed from the command every frame and is the
  face/region centroid *including the push applied so far*, so it cannot be buried
  by the surface it is dragging. (The three-arrow gizmo deliberately anchors on the
  latched reference instead, because a translate gizmo that crawls with its own
  geometry runs away from the cursor. A one-axis handle has no such problem.)
* **it flips.** The direction carries the push's sign, so a face pulled inward gets
  its stem on the inward side rather than poking out of the far face.

The move gizmo does not merely hide: `GizmoUsable()` refuses outright while a
lollipop is wanted, so the arrows are neither drawn nor hit-tested. That second half
is load-bearing — the viewport offers an LMB press to the gizmo *before* the
command, so a gizmo that stayed hit-testable would swallow the ball's press.
Grabbing the ball re-latches the command (resume→`Rebase`, or `Rebase` directly when
already hot) before opening the grab gate, so the geometry cannot jump by however
far the cursor wandered while the gesture was parked.
Hotkeys remain first-class; the gizmo has its own on/off switch.

**Contextual hotkey hints** (shakeout A, user request) are the gizmo's keyboard
twin: a six-row panel on the camera image's right edge listing what the current
selection kind (or the running modal command) can do. Every key shown is read
live from `g_radiantCommands`, so a remap or a keymap-profile switch is reflected
on the next frame, and an unbound command shows the palette's own key instead.

### ROUND K — Shift+D over brush EDGES makes construction lines

USER DIRECTIVE: *"New action (Shift-D) Duplicate. When pressing Shift-D, while
having edges of a solid(brush) selected, it should create new lines in place of
those edges. Those edges should copy exactly the edges of the solid and possibly
create a new lineface that can be used for extrusion (light blue). Plasticity has
this."*

**Dispatch rule, one line:** *any* `SEL_EDGE` item in the typed selection sends the
press to the edge arm; everything else keeps round J's clone. Not "the dominant kind
is EDGE" and not "only edges", because a mixed selection cannot be cloned in the
edge sense anyway (the ported `Clone_Selection` works on whole brushes and would
clone the owner solids), and since shakeout D's promotion removal an edge selection
is a deliberate mode-2 act. Tested *before* the cloneable count, because a pure edge
selection has `CloneableSelectedCount() == 0`.

The edge arm resolves each selected winding edge to its two world endpoints, drops
**coincident** duplicates (a brush edge belongs to two faces, so it arrives twice
whenever both contributed), joins contiguous runs into **polylines** and pushes them
into the ordinary construction store under ONE undo ticket. Plasticity does the same
thing structurally — its `DuplicateFactory` returns a *curve* for a duplicated edge,
because an edge does not exist outside its solid — and the directive's "possibly
create a new lineface" then comes for free: the lines are ordinary store objects, §8
runs over them on the next generation bump, and a closed ring of duplicated edges is
immediately a light-blue extrudable region. With round K's arrangement pass even a
set that does *not* quite ring up bounds a region as long as it encloses something.

---

## 15. Command palette — F (modern keymap)

- F opens; typing filters immediately; fuzzy match; arrows navigate; Enter
  runs; shortcut shown right-aligned; greyed rows for `canExecute == false`
  with the reason on hover.
- Backed by `g_radiantCommands` + the Ex metadata — every command id in the
  dispatch switch is automatically listed. No second registry.
- This is also the escape hatch guaranteeing keymap_classic users reach new
  features.

## 16. Construction planes

Active construction plane for drawing tools: XY / XZ / YZ / from-selected-
face / from-view. **Default behavior kills most explicit management:**
starting a drawing tool while hovering a face auto-derives the plane from
that face; hovering nothing uses the global plane (XY at last hit height).
The plane participates in snapping (SNAP_CPLANE) and renders as a subtle
finite grid patch around the cursor.

**Shakeout H demotes it from authority to fallback (§35).** A drawing tool places
at the *snap's* world position whenever a snap wins; the plane is only what
ray-casting falls back to over empty space, and it now follows the last placed
point. The grid patch is neutral grey and is suppressed entirely when the working
plane *is* the ground plane (§38).

## 16b. Creation suite, from the Plasticity inventory (shakeout C)

USER DIRECTIVE: *"How am I supposed to create a new brush in the 3dcam window?
There's no line tools like in plasticity. I want ALL the line tools (shift-a to
create a line, go look all of them up and dont half ass it)."*

So the suite was derived from the ACTUAL Plasticity source at `plasticity/`
(LGPLv3, GPL-compatible with this tree), not from memory.

### 16b.1 What Plasticity actually ships

`plasticity/src/commands/` — 32 directories: `arc array boolean box
character-curve circle curve cylinder delete duplicate ellipse evolution export
extend extrude fillet hole line loft material mirror modify_contour modifyface
multiline place polygon rebuild rect region sphere spiral thin-solid translate`,
plus `CommandLike.ts` and `GeometryCommands.ts` (the registry — every command
class is re-exported there, `GeometryCommands.ts:40-114`).

The CREATION-FROM-NOTHING commands, read out of their sources:

| # | Plasticity command | File | Flow |
|---|---|---|---|
| 1 | `LineCommand` | `curve/CurveCommand.ts:68` | `CurveCommand` with `Polyline3D` — click points, Enter/close ends |
| 2 | `CurveCommand` | `curve/CurveCommand.ts:10` | control-point clicks, `Hermit3D`; keys 1/2/3/4 switch hermite/bezier/nurbs/cubic-spline, ctrl-Z undoes one point |
| 3 | `CornerRectangleCommand` | `rect/RectangleCommand.ts:47` | corner → corner; a keyboard gizmo swaps to centre mode mid-command |
| 4 | `CenterRectangleCommand` | `rect/RectangleCommand.ts:97` | centre → corner |
| 5 | `ThreePointRectangleCommand` | `rect/RectangleCommand.ts:12` | p1 → p2 (an edge) → p3 (width) |
| 6 | `CenterCircleCommand` | `circle/CircleCommand.ts:13` | centre → rim |
| 7 | `TwoPointCircleCommand` | `circle/CircleCommand.ts:79` | two diameter endpoints |
| 8 | `ThreePointCircleCommand` | `circle/CircleCommand.ts:114` | three rim points |
| 9 | `CenterPointArcCommand` | `circle/CircleCommand.ts:139` | centre → start → end |
| 10 | `ThreePointArcCommand` | `arc/ThreePointArcCommand.ts` | end → end → bulge |
| 11 | `CenterEllipseCommand` | `ellipse/EllipseCommand.ts:9` | centre → axis 1 → axis 2 |
| 12 | `ThreePointEllipseCommand` | `ellipse/EllipseCommand.ts:45` | three points |
| 13 | `PolygonCommand` | `polygon/PolygonCommand.ts` | centre → radius; `shift-wheel` ±vertexCount, `v` toggles inscribed/circumscribed |
| 14 | `SpiralCommand` | `spiral/SpiralCommand.ts` | p1 → p2 (axis) → radius, then a dialog for turns/angle |
| 15 | `CharacterCurveCommand` | `character-curve/` | curve from a parametric expression |
| 16 | `MultilineCommand` | `curve/MultilineCommand.ts` | offsets an EXISTING curve into a multiline |
| 17 | `CornerBoxCommand` | `box/BoxCommand.ts:60` | corner → corner (rect preview) → height pull |
| 18 | `CenterBoxCommand` | `box/BoxCommand.ts:155` | centre → corner → height pull |
| 19 | `ThreePointBoxCommand` | `box/BoxCommand.ts:18` | p1 → p2 → p3 (base) → p4 (height) |
| 20 | `CylinderCommand` | `cylinder/CylinderCommand.ts` | centre → radius (circle preview) → height pull |
| 21 | `SphereCommand` | `sphere/SphereCommand.ts` | centre → radius |

Everything else exported by `GeometryCommands.ts` is a command over EXISTING
geometry — extrude, loft, revolution, evolution, pipe, slot, boolean, cut,
fillet, shell/thin-solid, extension, mirror, radial/rectangular array,
duplicate, place, trim, offset-curve, bridge-curves, join-curves,
modify-contour, the five modify-face variants, draft-solid, rebuild,
set/remove material, the delete family and the move/rotate/scale families
(each with an Item and a Freestyle variant).

### 16b.2 What Plasticity BINDS (checked, not assumed)

`plasticity/src/startup/default-keymap.ts`, the `body:not([gizmo])` block
(lines 283-292) — the creation keys:

```
shift-a → command:line            shift-q → command:corner-rectangle
shift-s → command:curve           shift-w → command:center-circle
shift-z → command:sphere          shift-c → command:corner-box
shift-x → command:cylinder        shift-v → command:center-box
```

…plus the in-command keyboard gizmos: `gizmo:polygon:add-vertex` =
`shift-wheel+up`, `subtract-vertex` = `shift-wheel+down`, `gizmo:polygon:mode`
= `v`; `gizmo:circle:mode` = `v`; `keyboard:rectangle:mode` swaps corner↔centre
mid-command; `gizmo:box:width/length/height` = `d/f/h`;
`gizmo:cylinder:height/radius` = `d/f`; `gizmo:curve:*` = `1/2/3/4` + ctrl-Z.

`plasticity/dot-plasticity/keymap.json` is the USER override sample — every
line in it is commented out, and its (stale) comment block still shows
`shift-A → command:deselect-all`; the LIVE default is `shift-a → command:line`.
The user's "shift-a to create a line" matches the live default exactly.

### 16b.2b What KIWI ACTUALLY BINDS (shakeout F — the shipped table)

Shakeout C spent one chord on an add menu.  The user rejected that outright —
*"the shift-A menu is unacceptable.  Shift-A is for LINES.  Start the line tool
immediately and the other ones are on other keys (lookup how plasticity keybinds
work)"* — so the modern profile now binds all eight of Plasticity's creation
chords directly:

| chord | Plasticity | KIWI ships | id |
|---|---|---|---|
| Shift+A | `command:line` | Construct: Line | 34034 |
| Shift+S | `command:curve` | Construct: Spline | 34050 |
| Shift+Q | `command:corner-rectangle` | Construct: Rectangle | 34036 |
| Shift+W | `command:center-circle` | Construct: Circle | 34037 |
| Shift+Z | `command:sphere` | Sphere | 34053 |
| Shift+X | `command:cylinder` | Cylinder | 34052 |
| Shift+C | `command:corner-box` | Box | 34051 |
| Shift+V | `command:center-box` | **Rectangle (centre)** — DEVIATION | 34047 |
| Ctrl+J | `j` → `command:join-curves` | Join Lines — DEVIATION (chord) | 34104 |
| T | `t` → `command:trim` | Trim (lines) — shakeout H, §37 | 34059 |

Two deviations, both deliberate:

* **Shift+V.**  KIWI has no centre-box primitive (§16b.3 lists it LATER).  Binding
  Rectangle (centre) keeps the corner/centre pairing the neighbouring keys teach
  — Shift+Q corner rect / Shift+V centre rect, mirroring Plasticity's
  Shift+C corner box / Shift+V centre box — and the chord will not have to move
  when the centre box does ship.
* **Ctrl+J instead of bare J.**  Bare J is `ToggleOutlineDraw` in stock Radiant, a
  display toggle mappers use constantly.  Ctrl+J is free and costs no
  displacement.

FIVE compiled-in commands were displaced to free those chords, all with the house
two-step (occupant → Shift+Alt+key):
`TogglePatchWireframes` → Shift+Alt+W, `ToggleCrosshairs` → Shift+Alt+X,
`CapCurrentCurve` → Shift+Alt+C, `VehicleGroup` → Shift+Alt+V, and
`SurfaceInspector` a **second** time (Shift+S → **Alt+S**).  The per-key occupancy
audit behind every one of those is written out in `kiwi_keymap.h`.

The **add menu survives, unbound**: it is still a palette row, still a button in
the KIWI panel's Construct block, still remappable by name from `radiant.ini`,
and it is still the only single place that lists the four creators that have no
chord (Polyline, Arc, Circle (2-point), Polygon).

**Plasticity has NO add-menu popup.** Creation is reached by those direct keys,
by the top toolbar (`components/toolbar/Toolbar.tsx` — and that toolbar is
SELECTION-CONTEXTUAL, it lists ops valid for what is selected, not creators),
and the Electron menu (`src/Menu.ts`) has File/Edit/Selection/View/Keybindings/
Help and no create menu at all. **The Shift+A palette KIWI ships is therefore a
BLENDER convention carrying a PLASTICITY inventory** — a deliberate deviation,
taken because the directive asks for one key that reaches every line tool and
because KIWI's letter budget (§11) cannot afford eight Shift+letter creators.
Shift+A still runs "the line tools", it just opens the list instead of
committing to one of them.

### 16b.3 The map — ships now / later / never

| Plasticity | KIWI | Where |
|---|---|---|
| LineCommand | **NOW** — Construct: Line (34034, shipped Phase 4) | `kiwi_construct.cpp` |
| LineCommand (multi-point) | **NOW** — Construct: Polyline (34035, Phase 4) | `kiwi_construct.cpp` |
| CurveCommand (spline) | **NOW** — Construct: Spline (34050), Catmull-Rom through the clicked points | `kiwi_construct.cpp` |
| CornerRectangleCommand | **NOW** — Construct: Rectangle (34036, Phase 4) | `kiwi_construct.cpp` |
| CenterRectangleCommand | **NOW** — Construct: Rectangle (center) (34047) | `kiwi_construct.cpp` |
| CenterCircleCommand | **NOW** — Construct: Circle (34037, Phase 4) | `kiwi_construct.cpp` |
| TwoPointCircleCommand | **NOW** — Construct: Circle (2-point) (34048) | `kiwi_construct.cpp` |
| CenterPointArcCommand | **NOW** — Construct: Arc (34038, Phase 4) | `kiwi_construct.cpp` |
| PolygonCommand | **NOW** — Construct: Polygon (34049), 3..32 sides on `[` / `]` | `kiwi_construct.cpp` |
| CornerBoxCommand | **NOW** — Box (34051): base rect + height pull → ONE brush | `kiwi_primitive.cpp` |
| CylinderCommand | **NOW** — Cylinder (34052) → ported `Brush_MakeSided` | `kiwi_primitive.cpp` |
| SphereCommand | **NOW** — Sphere (34053) → ported `Brush_MakeSidedSphere` | `kiwi_primitive.cpp` |
| *(no Plasticity equivalent)* | **NOW** — Cone (34054) → ported `Brush_MakeSidedCone`; Radiant has the primitive, so it ships | `kiwi_primitive.cpp` |
| ExtrudeCommand | **NOW** — Extrude Region (34039, Phase 4) | `kiwi_extrude.cpp` |
| ThreePointRectangleCommand | **LATER** — needs an oriented (non-axis-aligned) rect in the store; today `KCON_RECT` is axis-aligned in plane space. Reachable via Polyline meanwhile. |
| ThreePointCircleCommand | **LATER** — circumcircle solve; the store already holds centre+radius, so it is a solver, not a format change. |
| ThreePointArcCommand | **LATER** — same solver, arc form. |
| Center/ThreePointEllipse | **LATER** — the store has no ellipse; an ellipse is a sidecar format change (`KCON_ELLIPSE` + tessellation rule). Worth it only once someone extrudes one. |
| SpiralCommand | **LATER** — a spiral is a 3-D curve; the store's ruling 3 (points live in ONE plane) forbids it outright. It needs a per-object out-of-plane payload. |
| CenterBoxCommand / ThreePointBoxCommand | **LATER** — Box(center) is a two-line variant of Box; ThreePointBox needs the oriented rect above. |
| CharacterCurveCommand | **NEVER** — parametric-expression curves are a CAD nicety with no brush meaning; a level editor's users type coordinates, not formulas. |
| MultilineCommand | **NEVER** (as a creator) — it is an OFFSET of an existing curve; the equivalent KIWI feature is a construction-object offset, which belongs with the modeling ops (§25), not the creation suite. |
| LoftCommand / RevolutionCommand / EvolutionCommand / PipeCommand | **NEVER** — these produce NURBS/swept surfaces. A brush is a convex polyhedron: there is no faithful landing for a lofted or revolved surface short of a tessellator that would emit hundreds of brushes. Region-extrude is the brush-shaped subset and it already ships. |
| FilletSolidCommand / ShellCommand / ExtensionShellCommand / DraftSolid | **NEVER here** — not creation; §25 already ships the brush-meaningful subset (bevel, inset). |
| Boolean / Cut | already shipped as §24 CSG over the ported cores. |
| Mirror / arrays / duplicate / place / material / rebuild / delete / move-rotate-scale | already shipped (§13, §25) or ported classic. |

### 16b.4 The KIWI add menu (Shift+A)

One ImGui popup at the cursor, categorised, type-to-filter, arrows + Enter,
rows showing the live binding from `g_radiantCommands`. It runs commands the
same DEFERRED way the palette does (`PostMessage(WM_COMMAND)` — see
`kiwi_palette.cpp` `Run()` for why in-frame dispatch is unsafe). It is a VIEW
over the command table, never a second registry (§3).

Categories: **Curves** (Line, Polyline, Spline, Rectangle, Rectangle (center),
Circle, Circle (2-point), Arc, Polygon) · **Solids** (Box, Cylinder, Sphere,
Cone) · **From construction** (Extrude Region) · **Plane** (the five §16
construction planes, because "which plane am I drawing on" is the question the
menu provokes).

## 17. World grid, axes, and units (user directive 2026-08-09)

- **Global axis lines**: X/Y/Z axes drawn through the world origin at all
  times in the 3D viewport (X red, Y green, Z blue), fading with distance —
  Blender/Plasticity style, subtle, never dominating the scene. *(Shakeout A:
  width 1 — already the thinnest the ported line path offers — desaturated a
  notch so they stop competing with the pure-white selection outline and with the
  saturated transform-constraint accents, and each axis SPLIT AT THE ORIGIN with
  the negative half dimmer, the standard DCC convention that makes which side of
  zero you are on readable at a glance.)*
- **Infinite grey ground grid** on the Z=0 XY plane. Implementation for the
  D3D9 fixed pipeline: draw a camera-footprint-sized patch of grid lines
  centered under the camera each frame, alpha-fading toward the horizon
  (cheap, no shader tricks needed; regenerated per frame from view
  distance). Major/minor line weighting every 10 cells.

### 17.1 Ground grid v3 — the world lattice (ROUND M rewrite)

User report: *"The grid is still buggy. at shallow camera angles it doesn't
render. The density also changes with angle, it shouldn't do that. Look how ugly
the grid is. You need to fix it. Could use anti aliasing too."*

**Why v2 (shakeout C) failed, exactly.** v2 derived the footprint from the view
frustum: four corner rays through the ported `CameraCalcRayDir`, each intersected
with Z=0, and a ray that missed the plane walked 131072 units along itself
instead. Both arms diverge as the pitch flattens — a miss puts the corner ~131072
units out in XY, and a *hit* one degree below the horizon from 192 units up lands
at `192/sin(1°) ≈ 11000`. The footprint span is therefore proportional to
`1/sin(pitch)`, and the auto-coarsen loop
(`while span/spacing > 200: spacing *= 2`) turned that span straight into cell
size: a ~131072 span forces spacing ≥ 655, i.e. seven doublings of the default 10
unit grid. At 640–1280 units per cell the near field is empty — which is both
"doesn't render at shallow angles" and "density changes with angle". **One bug.**

**v3 removes the coupling rather than compensating for it.** Nothing in the
emitter reads `camera.angles[0]`:

| quantity | v3 source |
|---|---|
| LOD (cell size) | camera **height above Z=0**, alone, `cell ≈ h/16`, chosen as `base·2^k` with a Schmitt band (coarsen at 2.0×, refine at 1.2× — a 1.67× dead zone, so a parked camera cannot oscillate) |
| range `R` | `spacing × 120` — tied to the LOD, so the line count is bounded *by construction* and the reach grows as you climb (clamped to the ±131072 engine world bound) |
| window centre | camera ground point + `0.45·R` along **`camera.forward`**, the yaw-plane forward `CamWnd_BuildMatrix` already computes — pitch-free by construction, so it buys ~45% more reach in front without introducing an angle term |

Lines are emitted at that one LOD across the whole window. There are **no
visibility culls**: v2's majors-only outer bands, its projected-pixel-gap cull and
the 512-slot per-line Schmitt table that existed only to stop that cull flickering
are all gone. What remains is **brightness only** — three distance bands × major
(every 10th) / minor, six colour runs.

**Budget (worst case, by construction).** 241 indices per axis × 2 = 482, plus 6
axis segments = 488 in the width-1 batch; 25 majors per axis × 2 = 50 in the
width-2 halo batch. **538 segments, two `RC_DRAW_LINES` commands.**

**"Anti-aliasing", honestly.** Real AA is unavailable on this path and that was
verified: the editor `$line` technique consumes the run colour as MATERIAL_COLOR
and *lerps* toward it, so sub-1 alpha means "blend less of my colour", not
transparency (kiwi_lines.h TRAP 2); and MSAA would mean re-creating the shared
D3D9 device and every RTT render target — a renderer change, not a grid change.
So it is faked the way DCC line overlays always fake it: each **major** line draws
twice, a width-2 pass at 35% brightness first and the width-1 bright core over it
(`$line` is depthTest LESSEQUAL / depthWrite ON, so the coplanar core wins). Minors
stay single-pass — haloing them would fill the gaps between them.
- The ground grid is a **snap target**: SNAP_GRID resolves against it (and
  against the active construction plane's grid when one is set). Drawing
  tools with no face under the cursor default onto it.
- **Units: inches everywhere the user sees a number.** Internal geometry
  stays in game units; one conversion constant (`UNITS_PER_INCH` pref,
  default 1.0 — CoD's own convention is ~1 unit = 1 inch) is applied at the
  display/entry boundary ONLY, through a single `Units_ToDisplay` /
  `Units_FromDisplay` helper pair used by the HUD, numeric entry, grid
  labels, and inspector readouts. Never store converted values; never let
  the factor touch .map serialization.
- **Default grid spacing: 10 inches, user-adjustable to any value** — not
  restricted to powers of two. The classic 1/2/4…256 presets remain as
  quick picks in the grid popup; the modern keymap's grid control ([ ] step
  + typed entry) accepts arbitrary spacing. Audit legacy snap sites when
  generalizing: some assume power-of-two `d_gridsize` (bit tricks, digit-key
  mapping).

## 18. Viewport visual language

- Hover: light outline tint. Selected: strong tint (existing selection
  color). Active item: brighter still. Snap marker: small glyph per type +
  short text tag. Regions: translucent light blue. Construction lines:
  bright editor-only color (never a map-geometry color).
- **Snap marker, REVISED in shakeout E.** User directive: *"The tool
  previewer does not need to be a pink square. Have it just be a black dot
  with a black circle around the dot (with space between like in
  plasticity)."* Every POINT-rank snap (vertex / midpoint / construction
  endpoint / intersection) is now a **2 px filled dot inside a separated 6 px
  ring**, screen-constant, in one **near-black** colour; the edge tick and the
  face dot keep their shapes and take the same colour. The five per-rank
  marker colours are gone — green on green world geometry, amber on the grid
  and rose on the construction lines themselves were each cases where the
  marker was the *least* readable thing on screen at the moment it mattered
  most. Type differentiation lives entirely in the **label text**, which
  still carries the per-rank tint. The construction rose is untouched for
  construction *geometry*.
- Reuse the existing handle/line draw passes (`Ed_DrawVertexHandles`,
  connection-lines pass, patch wireframe pass) — add tint parameters, don't
  build a parallel overlay renderer.
- Respect the render-command-buffer overflow protections already landed
  (dropped-command scratch path); overlays must degrade, not crash.

---

# Part III — Geometry operations

## 19. Validity contract (governs everything below)

Classic brushes are plane-defined; windings are DERIVED
(`Brush_BuildWindings`). Therefore **every brush edit is ultimately a plane
edit**, followed by rebuild + validation:

```text
edit planes → Brush_BuildWindings → validate:
  convexity, no duplicate/degenerate planes, no zero-area faces,
  no tiny edges, winding sanity, tolerance checks
→ ok: keep   |   bad: reject gesture (live geometry shows last-valid state,
  HUD shows "invalid" tint; commit disabled while invalid)
```

Never silently commit invalid geometry; never crash on it. Reject > repair
in v1 (repair/decompose comes with CSG work).

## 20. Face push/pull — FIRST direct-edit op (order changed from the draft)

The draft ordered vertex → edge → face. Inverted here: **face push/pull
ships first** because it is the highest-value operation AND the only
trivially-safe one (translate one plane along its normal — convexity can
only be lost by planes crossing, which validation catches; face *deletion*
by pushing past the opposite plane is detected the same way).

- `G` on a face = distance along normal (mouse projected onto the normal
  axis), snap + numeric entry apply.
- **Texture lock:** wrap with the already-ported `Face_TexLock_Save` /
  `Face_TexLock_Reproject` so textures don't swim. This core exists and is
  gate-verified — use it, don't re-derive.
- Multi-face selection pushes each along its own normal (Plasticity
  behavior).

## 21. Edge move

Move both adjacent faces' planes such that the shared edge translates;
re-solve the two plane equations from their moved edge + retained third
points. Validation as above. Ships after face push/pull.

## 22. Vertex move — LAST (hardest)

Vertex edit on plane-brushes is the classic can of worms (a winding vert is
the intersection of ≥3 planes; moving it must re-solve all of them).
Radiant's existing vertex-edit core (`SelectVertexByRay` + the drag path)
and the vert-snap subsystem (`VertSnapTo_ModelBrushPrefab` family) already
implement the binary's answer to this. **Re-skin the existing core with the
new input/snap layer; do not write a new solver.** Patch control points are
the easy case (free-floating) and reuse the terrain-drag core.

## 23. Region extrusion → brushes

```text
closed Region + drag/typed distance
→ convex profile: one brush (planes from profile edges + 2 caps)
→ concave profile: Hertel-Mehlhorn convex decomposition → N brushes,
  auto-grouped as one selection on commit
```

- v1 may ship convex-only with a clear "concave: not yet" HUD message, but
  the decomposition algorithm choice (triangulate → Hertel-Mehlhorn merge)
  is fixed now so the data model doesn't paint us into a corner.
- Output is ordinary brush_t data in the ordinary .map — no new formats.
- New brushes get the current texture (same seeding path new brushes use —
  `random_texture_stuff[layer]`, see the whole-brush-texture fix).

## 24. CSG workflow

`CSG_Merge`, `CSG_MakeHollow`, and subtract exist in `csg.cpp` (ported).
The work is WORKFLOW, not math: viewport-first invocation (select A, invoke
Subtract, click/pick B or use selection order), overlay preview of the
result before commit where practical, result brushes selected afterward.
Union = merge where legal (`CSG_Merge` requires the union to stay convex) —
otherwise keep both and say so; don't fake a boolean union that classic
brushes can't represent.

## 25. Later modeling ops (unchanged from draft, briefly)

Bevel/chamfer edge (add a plane), inset face, mirror, array/radial array,
offset profile, bridge, smarter selection (coplanar/parallel/connected/
similar-orientation, double-click = connected). None are v1 blockers.

## 26. Textures/UV — out of scope, but don't burn bridges

- Preserve per-face material + texdef through every new op (texture lock on
  push/pull, texdef copy on extrude/decompose).
- Never assume texture data is discardable in new geometry APIs.
- TrenchBroom-style UV manipulation is Phase 6, after modeling ships.

---

# Part IV — Phasing

Reordered from the draft: stabilization gate added (Phase 0), snap manager
grows alongside its consumers instead of landing consumer-less, face
push/pull promoted, vertex demoted, palette moved earlier (it's cheap and
de-risks the keymap migration).

## Phase 0 — Stabilize the substrate (gate: user build + soak)

The repo currently has a stack of UNBUILT RTT/crash fixes (device-reset
rebind, command-buffer overflow, keybind wiring). A drastic overhaul cannot
start on an unbuilt foundation.

1. User builds; shake out the pending fix stack until the ImGui shell is
   daily-drivable (load/edit/save the 3-map round-trip without crashes).
2. Tag `radiant-imgui-baseline` — the overhaul's A/B oracle, like
   `radiant-mfc-baseline` was for the UI port.
3. Write the keymap conflict table (audit `g_radiantCommandsDefault`).

## Phase 1 — Camera + selection foundation

4. Orbit camera layer (look_at/orbit/pan/dolly; trace-to-recenter).
5. `Pick()` unified API over Test_Ray + screen-space vert/edge picking.
6. `selection_t` + legacy adapter (THE bridge — build and gate-test it
   against existing ops: hide, texture apply, clipper, CSG still work
   driven from the new selection).
7. Selection modes 1–5 + chip UI + keymap profiles (modern/classic).
8. Hover/selected/active rendering tints.
9. Directional box selection.
9b. World axes + infinite ground grid + the inches units-display layer and
    adjustable grid spacing (10" default) — §17.

Gate: full classic editing session performed with the new camera + selection
layer; zero regressions in old commands.

## Phase 2 — Command core

10. Command metadata extension over `g_radiantCommands` + palette (F).
11. Modal command framework + input arbitration state machine.
12. Undo-bracket integration (one gesture = one record; cancel = exact
    restore) — gate-test with a scripted 300-move drag.
13. Numeric entry HUD.
14. SnapManager v1: grid + vertex targets only (consumed by Phase 3's move).

## Phase 3 — Direct manipulation of existing geometry

15. G/R/S object transforms (brushes, patches, entities, prefabs-as-objects)
    + axis locks + numeric entry + ImGuizmo.
16. **Face push/pull with texture lock** (the flagship op).
17. SnapManager v2: edge/mid/face/endpoint targets + markers.
18. Edge move.
19. Vertex move (re-skinned existing core) + patch control points.
20. Validity pipeline (reject invalid, HUD feedback).

Gate: "the editor feels modern" checkpoint — a real map edited start-to-
finish without touching a 2D view.

## Phase 4 — Construction geometry

21. Construction store + `<mapname>.kiwi` sidecar persistence + undo.
22. Line/polyline/rect (planar tools + construction planes + auto-plane-
    from-face). Circle/arc/spline after.
23. Closed-loop Region detection + fill rendering.
24. Region extrude → convex brush; then Hertel-Mehlhorn for concave.
25. SnapManager v3: intersections, angle increments, cplane.

## Phase 5 — Modeling power

26. CSG viewport workflow (subtract/merge/hollow with preview).
27. Bevel/chamfer, inset, mirror, arrays.
28. Selection expansion helpers.

## Phase 6 — UV workflow

29. TrenchBroom-style interactive UV editing. Not before.

### Verification strategy (all phases)

Per project practice: map-free selftest gates with `*_BREAK` non-vacuity
proofs where math is involved — snap ranking determinism, region loop
detection, extrusion → `Brush_BuildWindings` validity, convex decomposition
area conservation, undo restore byte-compare. Plus gui_smoke monkey pass and
the 3-map round-trip at every phase gate. User builds; user eyeballs feel.

---

# Non-goals (unchanged, binding)

Not a SolidWorks clone, not feature-history CAD, not a new game-geometry
format, not a NURBS/B-rep kernel, not a Radiant rewrite, not a texture-
editor rewrite in the geometry pass, not a four-viewport-dependent UI.
Plasticity is interaction inspiration (and now a legal code quarry); the
result is still Radiant writing stock-compatible .map files.

# Appendix — Decision log

- **D-1 (RESOLVED, 2026-08-09, shakeout A):** RMB semantics. Hands-on says the
  draft's "RMB drag = pan" is wrong for a 3D-first editor: pan is the *rare*
  navigation and look is the constant one. The resolved scheme, all of it gated
  by the modern-input master toggle (with the toggle off, RMB is the ported
  free-look/`camera_mode` drive, byte for byte):

  | input | meaning |
  |-------|---------|
  | RMB drag (> 4 px) | **mouselook** — `camera.angles` only, position unchanged, 0.35°/px, pitch clamped ±89°. The legacy RMB path receives **nothing** for the whole gesture. |
  | RMB click (≤ 4 px) | **the classic context menu**, unchanged. The modern layer owns the gesture and, on a no-drag release, *replays* the legacy `CamWnd_OnRButtonDown` + `CamWnd_OnRButtonUp` pair so `cam_was_not_dragged` is set by the same code that reads it. Runs post-present, so `TrackPopupMenu` still nests outside the frame. |
  | Shift+RMB drag | **truck-pan** — origin *and* the orbit `look_at` by `−right·dx·k + up·dy·k`, `k` = world-per-pixel at the pivot. |
  | Shift+MMB drag | same truck-pan. Plain MMB stays orbit. |
  | W/A/S/D/Q/E while RMB held | **fly**, camera basis, `MoveSpeed` units/s × the fly multiplier, ×3 with Shift. |
  | bare arrow keys | the same fly with no RMB, whenever the cursor is over the camera image, no modal command runs and no ImGui field has focus. The modern keymap unbinds classic's `CameraLeft/Right/Forward/Back` (the 22.5°-hop "tank controls") to free them. |

  Free-look is therefore *not* classic-only as the draft proposed — it is the
  modern default, because "hold RMB and look" is the Plasticity/game-engine idiom
  the user asked for. Pan moves to Shift, where it is still one chord away.

- **D-2 (REOPENED and RESOLVED — RMB click = confirm, shipped shakeout E,
  2026-08-09):** the "won't do" reasoning was that D-1 already gave RMB two
  meanings and a third would make it unpredictable. Live testing inverted the
  argument. RMB now has **two** meanings, not three, and they never overlap:

  | RMB gesture | meaning |
  | --- | --- |
  | drag (> 4 px) | **camera** — truck, or mouselook with Alt (see D-1's revision below) |
  | click (≤ 4 px), modal command live | **CONFIRM** the command |
  | click (≤ 4 px), nothing live | the classic **context menu**, replayed as before |

  The click/drag split is the same threshold and the same code path shakeout A
  already used to separate mouselook from the context menu — no new
  discrimination was invented. And an entity context menu *during an edit to
  that entity* was never a useful third meaning; it was a meaningless one.

  This lands with the confirm-flow rework in §4: LMB release now PAUSES, so
  something else had to confirm, and the user asked for exactly this.

- **D-1 (REVISED, shakeout E):** the bare-RMB **mouselook** moves to
  **Alt+RMB**, and a bare RMB drag becomes the **truck-pan** — user directive,
  *"The camera should be pan on right click (not shift-right click)"*.
  Shift+RMB stays pan (nothing is taken away) and Shift+MMB is unchanged.
  Nothing else in the modern camera layer binds Alt+RMB; the XY window's
  `Shift+RMB(+Alt)` `Drag_Begin` arming (xywnd.cpp:3082) is a different window
  and a different handler.
- **D-3 (open):** Region holes/nested loops — after flat regions ship.
- **D-4 (open):** quantize-on-commit epsilon + whether it's a pref.
- **D-5 (decided):** construction geometry persists in a sidecar file,
  never in the .map (stock-tool compatibility is inviolable).
- **D-6 (decided):** classic LMB drag-to-move is removed from the modern
  keymap; movement is G/gizmo only. Classic keymap keeps it.
- **D-7 (decided, 2026-08-09):** display units = inches (conversion constant
  pref, default 1 unit = 1 inch); default grid spacing = 10 inches,
  arbitrary spacings allowed; global XYZ axis lines + infinite snappable
  grey XY ground grid always rendered in the 3D viewport. Internal storage
  and .map serialization remain raw game units.

---

# Part V — Shakeout G (2026-08-09): the Plasticity modelling verbs

Round G answers a single block of user feedback about *modelling* — the verbs
Plasticity has that KIWI did not, plus two direct-manipulation bugs that made the
existing verbs feel wrong. Every claim below about Plasticity was read out of
`plasticity/src`, and the cite is in the owning header.

## 27. The verb table

| key | command | id | what it does | Plasticity |
|-----|---------|----|--------------|------------|
| `C` | Cut | 34055 | >=1 selected brush; then **click** a construction line -> two solids, split by a plane through the line swept **away from the camera** and **locked at the click** (reworked in round L — see §42) | `c` -> `command:cut` (`default-keymap.ts:257`); cutter curve extruded along a placement Z (`CutFactory.ts:112-116`), red 10%-opaque double-sided phantom (`:333-343`), result is 2 solids (`__tests__/commands/Cut.test.ts:49`) |
| `Z` | Match Face | 34056 | one selected face, then click a target face -> the source's plane becomes the target's | **none** — no match/align-face verb exists in `plasticity/src`; the whole direct-face family is `ModifyFaceFactory.ts:71-130` and `ModifyFaceCommand.ts:22-26` dispatches only Refillet/Offset |
| `J` | Join | 34106 | two coplanar faces on two brushes -> `CSG_Merge` the owners; else construction lines -> chain them | `j` -> `command:join-curves` (`:258`), **curves only**; `MergerFaceCommand` is an empty body (`ModifyFaceCommand.ts:141`) |
| `E` | Extrude | 34058 | face -> a **new prism brush** along its normal; else region -> §23 | `e` -> `command:extrude` (`:264`); `ExtrudeCommand.ts:69-95` fans the selection out per kind, `:65` adds the results to the selection |
| `Ctrl+R` | Split Face | 34057 | one selected face -> split the owning brush along a line across it; **Tab** flips U/V | `SplitFactory` exists (`CutFactory.ts:176`) but is **dead code from the UI** — only `CutAndSplitFactory` (`:207-240`) reaches it and `CutCommand` never instantiates it |
| `V` | Move pivot | (command-local) | while Move or Rotate runs: place the gizmo origin with full snapping | `v` per gizmo context (`:120/:147/:159/:179`); handler `TranslateCommand.ts:296-312`, placement `:314-330` |

The full keymap audit — every displaced occupant and every free-chord proof — is
in `kiwi_keymap.h`. Nothing was dropped: `CameraDown`->Ctrl+Alt+C,
`CameraAngleDown`->Shift+Alt+Z, `ToggleOutlineDraw`->Shift+Alt+J,
`DragEdges`->Shift+Alt+E, `RemoveColorNode`->Shift+Ctrl+R, and `Ctrl+J` keeps
Join Lines as the lines-only alias.

## 28. Gizmo-only transforms

> *"The rotate tool is bugged. It needs to not do any rotation at all unless the
> gizmo is being dragged."*
> *"When selecting an object and pressing G (move), it moves with the mouse. It
> should only move with the gizmo."*

**Move and Rotate now apply exactly zero change from bare mouse movement.** The
only two sources are a **held gizmo handle** and a **typed value**.

* `R` — the 0.5-degrees-per-horizontal-pixel mapping is **deleted**. The angle is
  latched; a held ring (`KiwiXform_FeedRotateDegrees`) or a typed number is what
  moves it. The 5-degree snap now applies only to a ring drag.
* `G` — the four cursor mappings (free / axis / plane / face-normal) are
  unchanged but **gated on `m_grabbed`**, fed by `KiwiXform_NoteMoveGrab` from
  the gizmo's press and release edges. The snap arm is gated with them: a snap
  that fired while nothing was held would drag the reference point onto a target,
  i.e. the same bug wearing a hat.
* Face push/pull gains a **fourth gizmo arrow along the face normal**
  (`KGZ_NORMAL`, amber). The three world arrows still axis-lock the push.
* Idle HUD: `objects  free  drag a handle / type a value`, and for R
  `objects  axis Z  grab a ring / type degrees`.
* **`S` is deliberately unchanged** and still free-drags. It has no handle set at
  all, so gating it would leave scale reachable only by typing a factor — that is
  removing a working tool, not fixing a broken one. Logged as scale-handle debt.

## 29. The movable pivot (V)

A **session** pivot, in memory only, shared by Move and Rotate.

* `V` while either runs enters placement: the point follows the cursor through
  the full `KiwiSnap_Query` stack (with `PICKF_EXCLUDE_SELECTED` lifted, so it
  can land on the very solid being edited), and off-geometry it rides the
  view-facing plane so "anywhere in 3D" is reachable. A click or `V` again
  places; `Esc` leaves placement without changing anything; RMB/Enter still
  confirm the underlying op.
* It overrides **Move's `m_ref`** (the constraint anchor, the gizmo origin and
  the axis/plane-lock origin are all one point) and **Rotate's `m_pivot`**, which
  is `rot_around[0]` — `Select_RotateAxis` fills only rows 1..3
  (`select.cpp:2360-2372`), so row 0 *is* the rotation centre and no downstream
  plumbing changes. Moving it mid-rotate first rolls back the applied angle, the
  same rule re-aiming the axis already used.
* **Reset rule:** the pivot dies when the **selection signature** changes — item
  count plus a mix of each item's (instance pointer, kind, face/edge/vert index),
  plus the construction-selection count. Deliberately *not* `Sel_Generation()`,
  which bumps on every sync even when the same things end up selected and would
  therefore throw the pivot away between the two commands the user wants to share
  it. Committing a move leaves the signature alone, so "reuse soon after" works.
* **Never written to disk.** No ini key, no profile entry.
* Drawn as a distinct marker (three axis ticks + a screen-facing diamond), violet
  when in force and yellow while being placed. The gizmo **stands down** during
  placement — Plasticity's own `gizmo.disable()` (`TranslateCommand.ts:316`), and
  here it is load-bearing: the viewport offers a press to the gizmo before the
  command (`kiwi_viewport.cpp:292` vs `:311`), so a live gizmo would swallow the
  placing click.

**Two deviations from Plasticity, both stated:** V does *not* finish and
re-enqueue the command (Plasticity has to — its snap points only update on
commit, `TranslateCommand.ts:297`; KIWI's snap reads the live lists), and the
pivot **persists** across commands (Plasticity's `choosePivot` is false on every
fresh command, `:326-329`).

## 30. Faces render as FILL, not outline

> *"When selecting a face, it still does a thick pink outline on the face. Stop
> doing this."*

A face accent used to be `EmitWinding` — the face's own boundary, in the same
line batch an EDGE accent uses — so "one face selected" and "the four edges
around it" drew identically. A face now draws as a **translucent triangle-fan
fill** through the same `R_AddRenderCmdDrawTris` + `MATERIAL_COLOR` bracket
`kiwi_region.cpp` uses, and emits **nothing** into the line batches.

* hover cyan alpha 0.18 - selected amber alpha 0.25 - active amber alpha 0.35
* verts nudged `-vpn * 0.50` (twice the outline's 0.25 — a fill loses *area* to
  z-fighting where an outline lost a one-pixel sliver)
* fan from vertex 0 is exact: a brush face is convex by construction
* caps: 64 points per winding, 64 fills per frame, degrading oldest-first
* edge and vertex accents are untouched

## 31. Face-select auto-extrude and push-through delete

* **Mode 3 (`mask == FACE` exactly), plain click on a face -> the ordinary Move
  command starts PAUSED** with the gizmo up. Mode 5 does not auto-enter (a face
  click there is one of four things it could have meant); shift/ctrl clicks do
  not either. An unmoved `Esc` leaves **no undo record**: `Begin` never mutates,
  `MouseMove` never reaches a PAUSED command, and `ApplyFaces` now returns before
  `OpenUndoForBrushes` while the scalar is zero (the shakeout-G first-mutation
  guard, added to the edge and vertex paths too).
* **Pushing a face past the opposite extent switches to a DELETE state.** The
  threshold is the brush's thickness along that face's baseline normal, measured
  once at `Begin`; the preview becomes a dim-red wireframe over the *untouched*
  geometry and the HUD says `PUSHED THROUGH - confirm DELETES n brush(es)`.
  Confirming runs the classic `Select_Delete` inside the gesture's own bracket
  (`Undo_AddEntity_W` per owner first, mirroring `Cmd_OnSelectionDelete`).
  Pulling back restores the ordinary push — the flag is recomputed every frame
  and the geometry is re-applied from the baseline.

## 32. The shared two-halves splitter

`C` and `Ctrl+R` both reduce to "cut brush B by the plane through p0/p1/p2", and
that is ONE function — `KiwiSplit_BrushByPlane` — a gated wrapper over the ported
`Brush_SplitBrushByFace` (`brush.cpp:4596`, 0x471960) the classic clipper uses.
The template face's caulk / nodraw_decal material comes from
`Ed_BuildClipFaceMaterial_Kiwi`, hoisted **verbatim** out of
`Ed_ProduceSplitLists` (`xywnd.cpp`) so the synthesis exists once.

What the wrapper adds: a §19 gate on **both** halves while they are still
unlinked defs (a refusal frees them with `Entity_UnlinkBrush` + `Brush_Free_R`,
the pair `CSG_MakeHollow` uses on the piece it discards, and touches neither the
map nor the selection nor undo), and the ordering rule that the **source instance
is freed by the caller, after both halves are linked**, so the owner entity never
transiently drops to zero brushes.

Undo is `CSG_MakeHollow`'s wrapper shape, one record for "remove one, add two":
`Undo_ClearRedo -> Undo_GeneralStart -> Undo_AddBrushList` (which clones the
originals) ... land both halves ... free the source ...
`Undo_EndBrushList -> Undo_End` (which stamps the halves so
`Undo_Undo` Phase 1 removes them while Phase 4 re-links the clones).

**Cut's plane:** `u = normalise(p1 - p0)`, `away = normalise(vpn - u*(vpn.u))`,
`n = normalise(cross(u, away))` — so the plane contains the line and sweeps
straight into the screen. `camera_s.vpn` points INTO the scene, so no sign flip.
A line pointing at the camera collapses `away` and the click is refused.

**Round L replaced the freeze rule outright.** Shakeout G re-derived the
orientation from the live camera on every HOT frame and froze it only when the
gesture paused; the plane is now derived **once**, at the instant the line is
clicked, and never again. §42 has the full before/after.

## 33. Paste, Clone and E enter Move

`Paste` (33040) and `Clone` (33001) both gain a `// KIWI-UX` **post-dispatch
tail** in `mainfrm.cpp`'s switch — `KiwiCmd_AfterPaste()` — which starts a PAUSED
Move over whatever they just selected. The hook is on the two **command ids**,
not inside the ported handlers, because `map.cpp:569` and `entity.cpp:1873` call
`XYWnd_PasteClip` directly to carry a selection across File->New, where starting
a modal command would be nonsense. It self-guards on `KiwiUX_ModernInput`, so the
classic profile is byte-identical.

Verified rather than assumed: `Cmd_OnEditPastebrush -> XYWnd_PasteClip
(mainfrm.cpp:4340) -> RadiantClipboard_Paste (entity.cpp:1978) ->
Map_ImportBuffer (map.cpp 0x487C90)`, which opens with `Select_Deselect(1)` and
then selects every parsed brush; and `Clone_Selection` (`select.cpp:2492`) ends
with the copies on `selected_brushes`. Neither needs help selecting.

`E`'s new body does the same through `KiwiCmd_StartDeferred` — a start parked
until after `KiwiCmd_Commit` has closed the bracket and reset the numeric layer,
because calling `KiwiCmd_Start` from inside `Commit()` would let those two land
on the wrong command.

---

# Part VI — Shakeout H (2026-08-10): true 3D construction lines, Trim, grid polish

Round H answers one block of feedback about *drawing*. Every Plasticity claim
below was read out of `plasticity/src`; the cites are repeated in the owning
headers.

## 34. §7 ruling 3 REVERSED: the construction store is WORLD-SPACE

**USER REPORT, verbatim:** *"the lines are only in 2D. It's not possible, even
with an aggressive camera angle, to get them to go up or down on Z. We need 3d
lines, plasticity has this."* and *"When using the line tool, snapping to corners
on a brush is buggy. Probably related to the 3D work."*

Those are one bug. §7's ruling 3 said points live in **plane space** — each
object carrying a `kconPlane_t` and storing 2D `(u,v)` pairs — on the argument
that *coplanarity is intrinsic in plane space and cannot be lost*, and that
coplanarity is what makes a loop a region (§8) and an extrusion a prism (§23).

**Why it fell.** The ruling was true about coplanarity and wrong about
everything else:

1. every point was placed by ray-plane intersection, so **no gesture could leave
   the plane** — 2D lines by construction, exactly as reported;
2. worse, a **snap was projected onto the plane before being stored**.
   `KiwiCon_WorldToPlane` drops the normal component by definition, so snapping to
   a brush corner 64 units above the working plane stored that corner's *shadow*.
   The snap marker drew in one place and the geometry went to another. **That is
   the corner-snap bug, in one line of `MouseMove`.**

The second point is the one that mattered: *a snapped point must not be projected
away from the thing the user snapped to.* Every other consequence follows.

**Plasticity does not do this**, and its source says so plainly: a picked point's
position is the snap's own `project()` — `PointSnap.project` returns the stored
vertex verbatim (`src/editor/snaps/PointSnap.ts:15-19`), `CurveSnap`/`FaceSnap`
return the kernel's near-point (`src/editor/snaps/Snaps.ts:206-214`, `:326-336`) —
and the construction plane is the **lowest-priority candidate**, priority 5 below
points (1), curves/axes (2) and faces (3) (`src/editor/snaps/SnapPicker.ts:136-156`),
taken only when nothing else answered (`PointPickerSnapPicker.ts:68-88`, whose own
comment reads *"the construction plane can either act just as a fallback ... OR it
can act like a real object"*).

**The new ruling.** `kconObject_t::pts` is **3 floats per point, world space**,
for LINE / POLYLINE / RECT. CIRCLE and ARC keep an authoritative stored plane and
stay parametric — they are intrinsically planar and were never the complaint.

**What pays for coplanarity now.** It is derived and *tested*:
`KiwiCon_FitPlane` fits by **Newell's method** (area-weighted, so it survives a
leading run of collinear points that a three-point cross product would collapse
on) and refuses the fit when any point lies more than `KCON_PLANE_FIT_DIST`
(**0.5 world units**) off it. A region is then "a closed chain whose points fit
one plane", *at any orientation*; `kregion_t` is unchanged (plane + 2D loop), so
**§23's extruder is untouched** — it still receives a plane and a 2D loop, with
the projection error already bounded by the fit.

An object that fits no plane is legal. It just never becomes a region. That is the
honest trade, and Plasticity makes the same one — its `PlanarCurveDatabase`
silently declines to fragment a non-planar curve (`:46-47`).

**Consumers touched:** region PASS 1 (fitted plane, projected loop) and PASS 2
(group plane **fitted over the members** instead of copied from a seed, membership
by point-vs-plane); `KiwiRegion_ChainWalk` (welds in **3D** distance, no longer
takes a plane — it used to need one before it could tell whether two lines met,
which was circular now that the plane is derived *from* the chain);
`KiwiConSel_Join` (concatenates in world, and **no longer requires coplanarity** —
a 3D chain is a legal polyline, and whether it is also a region is the region
layer's question); `KiwiConSel_MoveApply` (translates every point, not just the
plane origin, which under the new ruling would have moved nothing); the snap
intersection arm (3D closest-approach); the sidecar.

**Sidecar: KIWI2.** `wpt x y z` per point; `plane` written only for circles and
arcs. **KIWI1 still loads** — the `plane` + `pt u v` parser path is kept verbatim
and the points are converted to world at the object's `end`. Only KIWI2 is
written.

## 35. Placement in 3D, and the Z constraint

A drawing tool's point is now **the snap result's world position, verbatim**, when
any snap wins; over empty space the ray-plane fallback is unchanged, so nothing
about drawing on the floor moved. `SNAP_FACE_CENTER` is **no longer suppressed**
while a tool runs (it was, precisely because of the projection this round
removed); the *area* `SNAP_FACE` arm stays suppressed, because it fires on every
pixel of every surface and would turn a drag across a wall into a surface crawl.

The **working plane follows the last placed point** — `PushPoint` re-seats the
plane origin there and pushes it to the active plane, so the fallback height for
the next segment is the height the user is working at.

The intrinsically planar tools (rect, rect-centre, circle, circle-2pt, arc,
polygon) still project onto the working plane: a rect with one corner 40 units off
its own plane is not a rect. Line, polyline and spline are free 3D.

**`Z` toggles a vertical constraint** on the next segment: the placement becomes
the closest point on the vertical line through the last placed point to the cursor
ray, grid-snapped in Z — and, when a *geometry* snap is live, that snap's own
height, so "go straight up to that corner's level" is one gesture.

*A toggle rather than a hold*, and the reason is mechanical: the key funnel is fed
`WM_KEYDOWN` only and there is no key-up path into a command anywhere in this
layer, so a hold-to-constrain would latch on and never release. The HUD carries
`[Z LOCK]` and `Z: vertical ON` whenever it is engaged.

**Arbitration with Match Face (bare `Z`, shakeout G): none needed.**
`KiwiUX_KeyFunnel`'s first arm is `if ( g_activeCommand ) return
KiwiCmd_KeyDown(...)`, so with a drawing tool live the key never reaches
`Radiant_TryHotkey`. Match Face is a *selection*-context verb, a drawing tool is a
*command* context, and the funnel has ranked them that way since shakeout E — the
same reason the movable pivot's `V` needs no table row. Plasticity draws the line
in exactly the same place: its axis choices are scoped to
`body[gizmo=point-picker]` (`default-keymap.ts:353-366`, `"z": "snaps:set-z"`).

**The value bubble and the angle field keep working.** Length is a 3D distance.
The angle is the **in-plane bearing**, measured in the working plane's own
`(u,v)`; it is **omitted** (the field reports nothing and the swing is skipped)
when the segment's out-of-plane component exceeds its in-plane length — which is
always true under the Z lock, where "the bearing" would be a number with no
meaning. A typed bearing now swings only the in-plane part and **keeps** the
out-of-plane offset, so it turns a rising segment instead of flattening it.

## 36. The line confirms on RMB / Enter, never on a click

> **SUPERSEDED BY §46 (Round P).** The *confirm* half of this section still
> stands and is now stronger: no click in the curve tool commits anything. The
> *parked endpoint* half is gone — click 2 places the second point of a chain.
> See §46 for the replacement and `RADIANT_KNOWN_ISSUES.md` for what that costs.

**USER DIRECTIVE, verbatim:** *"Lines should not confirm until a right-click or
Enter. This allows the operator to Go back 180 degrees to get a snapping point
setup, and then go the other way 180 degrees to get the desired length."*

| stage | before | after |
|-------|--------|-------|
| click 1 | places the start point | unchanged |
| endpoint | follows cursor + snap | unchanged |
| click 2 | **committed the line** | **parks** the endpoint where it is; the rubber band stops tracking |
| click 3, 4... | — | unpark / re-park: the endpoint resumes tracking, then parks again |
| RMB / Enter | committed | **commits** |
| Esc | drops the chain, then leaves | unchanged |

This is the shakeout-E pause flow applied to the line's second stage, implemented
*in the tool* rather than through `KiwiCmd_Pause` because a `WantsClicks` command
opts out of the framework's pause outright — a click in a drawing tool means
"place", and that grammar is unchanged for the other eight tools.
**Polyline and spline are untouched**: multi-click placement, RMB/Enter to end,
click-near-the-first-point to close. Only the two-click line needed it, because it
was the only tool whose second click silently committed.

Plasticity behaves the same way at the top level: `pointPicker.execute` resolves
one point per click and `CurveCommand` loops; only `point-picker:finish` — bound
to `enter` and `mouse2` (`default-keymap.ts:363-364`) — breaks out and commits
(`CurveCommand.ts:40-64`).

## 37. Trim (`T`)

**USER DIRECTIVE:** *"Add a Trim tool (T) that only works on lines. Its job is to
trim off excess bits when lines are hastily drawn."*

Ported from `plasticity/src/commands/curve/TrimCommand.ts` + `TrimFactory.ts` +
`Interval.ts`, bound to a bare `t` there too (`default-keymap.ts:267`). Four facts
shaped the port:

1. **curves only** — the picker is `SelectionMode.Curve` and
   `LayerManager.showFragments()` actively disables solid/face/region picking
   (`LayerManager.ts:37-50`). "Only works on lines" *is* Plasticity;
2. **the pieces pre-exist** — `PlanarCurveDatabase` splits every coplanar curve at
   every mutual intersection into invisible fragments the moment a curve is added
   (`:24-33`, `:79 c3d.CurveEnvelope.IntersectWithAll`). KIWI computes the span on
   **hover** instead: same answer, no second database to keep in sync;
3. **an interior span splits the curve in two** — `TrimFactory` takes the
   *complement* of the removed range (`:84-91`) and `Interval.trim` returns two
   intervals for an interior cut, one for an end cut, none when it swallows the
   whole curve (`Interval.ts:6-42`);
4. **it keeps going** — no loop; the command re-enqueues itself after each trim
   (`TrimCommand.ts:30`) until Escape.

**KIWI's version.** Modal, `WantsClicks` (a click is one trim, never a commit),
RMB/Enter finishes. Trimmable: `KCON_LINE` and `KCON_POLYLINE`, open or closed —
so polygons and splines are trimmable too, since the store keeps both as closed
polylines. **Not** circles, arcs or rects: trimming a parametric shape means
converting it to a polyline, which destroys the thing the user drew.

*Intersections are 3D*, because the store is: two segments cross when their
closest approach is under `KCON_ISECT_DIST` (**0.25 units**) via
`KiwiCon_SegSegClosest` — the same helper the `SNAP_INTERSECTION` arm now uses, so
**what trims is what snaps**. An exact 3D line-line test would be an exact-zero
test on floats and would never fire.

*Span selection*: the hovered object is flattened into a cumulative arc-length
parameterisation; the span is bounded by the nearest crossing below the cursor and
the nearest above, falling back to the object's own ends. That is why the crossed-
lines picture works — each tail is bounded by the crossing on one side and an
endpoint on the other. A closed object needs **two** crossings before a span is
well defined; the result of trimming one is a single open polyline.

*Undo*: **one store snapshot per trim click**, not per command — a trim session is
a sequence of independent removals (Plasticity makes each one its own command for
the same reason), and rolling five back together would surprise anyone who wanted
only the last one gone. `Esc` therefore means "stop trimming", not "put them all
back".

**Keymap.** vk `0x54`'s full compiled-in occupancy: mods 0 `ViewTextures` 33018
(`mainfrm.cpp:998`), mods 1 `ToggleTexMoveLock` 32785 (`:1064`), mods 5
`ThickenPatch` 32904 (`:999`). *(Not `ToggleTexLock` at mods 0 — that is 32785 and
it is on Shift+T.)* mods 3 is free, so `ViewTextures` takes the house two-step to
Shift+Alt+T and stays on the menus and in the palette. `T` is not in
`res/radiant.rc`'s accelerator table (`:490-501`) and is not one of the fly's
swallowed keys (W/A/S/D/Q/E), so nothing further needed arbitrating.

## 38. Grid polish

**"stop making the nearby grid pink when using any tool."** The §16
construction-plane patch was drawn in the rose §18 reserves for construction
geometry. Two changes, and the second matters more:

1. it is now **neutral grey** (`0.38` minor / `0.54` axes — a notch brighter than
   the ground grid's near band at `0.30`/`0.46`), so the working plane still reads
   as a surface without inventing a hue. The rose stays where §18 actually wanted
   it: on the construction **lines**;
2. it is **only drawn when the working plane is not the ground plane**
   (`|n.Z| > 0.999` and `|origin.z| < 0.5`). A tool on Z=0 was painting a second
   grid exactly on top of the one already there — the same lines in a different
   colour, which is all "pink noise" could ever have meant.

**"still some shimmering on faraway lines."** Shakeout C's grazing cull was a
*bare threshold*, and a bare threshold is itself a shimmer generator: a line whose
projected gap sits at 3.0 px flips visible/hidden on alternate frames as the
camera drifts by a pixel, and a hundred of them flipping independently is exactly
the twinkle that was left. Two numbers changed it:

- **hysteresis, per grid line.** A drawn line stays drawn until its projected gap
  falls below `3.0 x 0.7 = 2.1 px`; a hidden line stays hidden until the gap rises
  above `3.0 x 1.3 = 3.9 px`. Between 2.1 and 3.9 px nothing changes state, so
  sub-pixel drift cannot flip anything. The state is one byte per line, keyed by
  the line's own grid index masked into 512 slots — **exact, not approximate**:
  the coarsening loop caps a frame at 200 *consecutive* indices per axis, and 200
  consecutive integers masked to 9 bits cannot collide. The table is cleared
  whenever the spacing changes, because a coarsen renumbers every line;
- **the majors-only transition moves one band nearer**: bands 3, 4 and 5 now drop
  minor lines (shakeout C dropped them in 4 and 5). Band 3 is the outer half of
  the middle distance, where a minor line is a few pixels wide at any realistic
  pitch and there is nothing in it a user can count.

---

## 39. ONE undo timeline (`kiwi_undo.h`)

**USER DIRECTIVE, verbatim:** *"Redo the whole undo/redo system so that it works
with every action."*

### What was actually wrong

Not the ported undo — that works, and this round does not touch a line of its
restore code. What was wrong is that there were **two stacks and no relationship
between them**:

| | storage | depth | redo? | driven by |
|---|---|---|---|---|
| ported | cloned `brush_t` defs + `entity_s` defs | 64 records / 2 MB | yes | `Undo_Start` … `Undo_End`, `Undo_Undo` / `Undo_Redo` |
| construction | whole-store `std::vector<kconObject_t>` snapshots | 32 | **no** | `KiwiCon_UndoPush` / `KiwiCon_UndoPop` |

`kiwi_construct.h` **scope ruling 2** wrote the split down honestly and predicted
the consequence in as many words: *"a split undo is exactly the kind of thing that
surprises a user once and then never again."* It surprised them. The three
concrete symptoms:

1. `Ctrl+Z` **inside a drawing tool** popped the construction stack; `Ctrl+Z`
   anywhere else popped the legacy one. The user had to know which world they were
   in before pressing a key that exists to undo the *last thing that happened*;
2. draw a line, move a brush, press `Ctrl+Z` twice → the brush move is undone, and
   then the brush move *before that*. The line never goes away;
3. there was **no construction redo in any spelling**. `Ctrl+Y` after a
   construction undo redid an unrelated legacy record.

### The fix is an ORDER, not a merge

Merging the two snapshot formats is a much larger change than the problem needs,
and it would put editor-only scaffolding into a structure the map format has to
survive. What was missing is not shared storage, it is **shared order**. So:

```
        THE JOURNAL (kiwi_undo.cpp)          the two stores, unchanged
   s_undo:  [ LEGACY  "drag selection"   ] ──► undo.cpp record #12
            [ CONSTR  "construction edit"] ──► s_undo.back()  (kiwi_construct)
            [ LEGACY  "extrude region"   ] ──► undo.cpp record #13   <- newest
   s_redo:  [ CONSTR  "construction edit"] ──► kiwi_construct s_redo.back()
```

A **ticket** is `{domain, label}` — no geometry, no copy. `Ctrl+Z` pops
`s_undo.back()`, forwards it to `Undo_Undo()` or `KiwiCon_UndoPop()`, and pushes
it onto `s_redo`. `Ctrl+Y` mirrors it through `Undo_Redo()` / the new
`KiwiCon_RedoPop()`.

### The four hooks, and why each is where it is

- **append, legacy** — `undo.cpp` `Undo_End` **tail**. `Undo_End` is *the* single
  close point: it is the only writer of `done`, its two early-outs both mean
  "there is no record to close", and both `Undo_Undo` and `Undo_Redo` consume
  records only in the `done` state. A ticket minted anywhere else could name a
  record that can never be undone.
- **append, construction** — `KiwiCon_UndoPush`. The store snapshots *before*
  mutating, so the push **is** the record; there is no separate close.
- **eviction** — `Undo_FreeFirstUndo` tail. It is the ONE eviction point (the
  64-record cap in `Undo_GeneralStart`, the 2 MB cap in `Undo_End`'s trim loop),
  and the oldest legacy record maps to the oldest LEGACY ticket by construction.
  `Undo_Clear` frees the whole list in its own loop instead, so it gets its own
  hook.
- **redo destruction** — `Undo_ClearRedo` tail, which is where `Undo_Start`,
  `KiwiCmd_UndoBegin` and the cancel path all destroy the legacy redo.

Plus a **belt-and-braces** check that costs nothing: `KiwiUndo_Undo` verifies
`g_lastundo` (and `KiwiUndo_Redo` verifies `Undo_RedoAvailable()`) before
forwarding. A ticket that cannot be honoured is discarded with one console line
and the next one is tried, so an unhooked future eviction path degrades to *one
lost step* instead of a stuck timeline.

### Routing: the dispatcher, not the key funnel

The obvious place is `KiwiUX_KeyFunnel`, and it is **wrong**: it sees only the
keyboard, so the Edit menu would keep the old split behaviour. The obvious second
place is the KIWI command arm, and there is no KIWI id involved at all —
`Ctrl+Z`/`Ctrl+Y` are the CLASSIC ids 57643 / 57644 (`ID_EDIT_UNDO` /
`ID_EDIT_REDO`, `res/resource.h:45-46`).

Every route converges on **`Radiant_DispatchCommandDirect`**: the
`Radiant_PreTranslateMessage` Ctrl+Z/Ctrl+Y arm posts through
`Radiant_ExecCommand`, the Edit menu posts the same ids from `WM_COMMAND`, the
palette runs classic ids through `Radiant_ExecCommand`, and `radiant.ini` can
remap the keys to anything. So the hook is a `// KIWI-UX` **pre-hook** on those
two cases, and an empty journal falls through to the ported handler unchanged —
which means a build with only LEGACY tickets in flight behaves identically to the
one before this round, by construction.

### Cancel suppression

`KiwiCmd_UndoCancel` rolls a gesture back by closing its own bracket and undoing
it immediately: `Undo_EndBrushList` -> `Undo_End` -> `Undo_Undo` ->
`Undo_ClearRedo`. That `Undo_End` is a real close, and unsuppressed it would mint
a ticket for a record destroyed on the next line — **one phantom `Ctrl+Z` per
cancelled gesture**, which is precisely the bug class the journal exists to
remove. So the helper brackets itself in `KiwiUndo_SuppressBegin/End`, a
re-entrant depth counter. It suppresses **appends only**: the `Undo_ClearRedo`
hook still fires inside the window, because the legacy redo list genuinely is
cleared there.

### Redo rules

`undo.cpp`'s own rule is *"any new record destroys the redo list"* (`Undo_Start`
is literally `Undo_ClearRedo` + `Undo_GeneralStart`). Generalised to two domains:

- a new ticket in **either** domain clears the whole journal redo stack **and**
  the construction store's redo stack;
- `Undo_ClearRedo` clears it too;
- a **replay** (`KiwiUndo_Redo` forwarding a ticket) does neither. `Undo_Redo`
  runs a full `Undo_GeneralStart` / `Undo_End` bracket internally, so without the
  replay flag one `Ctrl+Y` would wipe the rest of the stack it is walking.

Conservative on purpose. The worst case is losing a redo that could have survived;
the failure it rules out is forwarding a ticket into an empty stack.

### The action -> `Ctrl+Z` table

| action | ticket appended | one `Ctrl+Z` does |
|---|---|---|
| brush move (`G`) | LEGACY `"move objects"` | `Undo_Undo` restores the pre-move brush defs |
| face push/pull | LEGACY `"push face"` | `Undo_Undo` restores the owning brush |
| extrude face (`E`) | LEGACY `"extrude face"` | removes the new body |
| extrude region | LEGACY `"extrude region"` | removes the brushes; the construction lines stay (two tickets — see Still-open) |
| cut (`C`) | LEGACY `"cut brushes"` | restores the single pre-cut brush |
| draw line / rect / circle | CONSTRUCTION | `KiwiCon_UndoPop` restores the pre-draw store |
| trim (`T`), per click | CONSTRUCTION | pops one trim |
| construction move (`G` on lines) | CONSTRUCTION | restores the pre-move store |
| delete brush (`Del`) | LEGACY `"delete"` | `Undo_Undo` re-links the brush defs |
| delete line (`Del`, construction) | CONSTRUCTION | restores the deleted objects |
| **cancelled gesture (`Esc`)** | **none** (suppressed) | undoes whatever came *before* the cancelled gesture |

### Selection safety

`KiwiCon_UndoPop` and `KiwiCon_RedoPop` replace the store **wholesale**, so every
index the construction selection holds is meaningless; both call
`KiwiConSel_NoteStoreReplaced`, which drops the selection rather than guessing.
Nothing else caches construction indices across a frame — `kiwi_region` re-derives
on the generation counter, the drawing tools hold their own in-progress point
*list* (not indices), and `kiwi_snap` walks the store live. Brush-side,
`Undo_Undo`/`Undo_Redo` already end with `Sel_InvalidateFromLegacy()`.

---

## 40. The hint strips, the grey cube, the right column, the scale

**USER REPORTS:** *"this popup box is not very helpful, also it should be in the
bottom left and bottom right like modern plasticity."* · *"The cube in the top
right is ugly, make it more like blender (grey)."* · *"put the texture view by
default under the 2d view on the right middle dock."* · *"Scale of the map (3d
view) is still way too big. Tone it down by about a factor of 10 in terms of zoom
and such."*

### The strips

Plasticity puts exactly two things at the bottom of its viewport, one per corner
(`src/index.html:31-32` mounts `<plasticity-keybindings>` and
`<plasticity-dialog>` inside the viewport):

- `components/viewport/Keybindings.tsx:52` — `"flex absolute right-3 bottom-3 …"`;
  its content is a `Set` of command ids pushed and popped by editor signals
  (`:11-12` `keybindingsRegistered.add` / `keybindingsCleared.add`), each resolved
  to a keystroke through `keymaps.findKeyBindings({ command })` and to a label
  through the static `keybindings` map in `toolbar/icons.ts:56-182`;
- `components/dialog/Dialog.tsx:32` — `"absolute bottom-2 left-2 w-96 …"`; the
  per-command options dialog, appended on the `dialogAdded` signal
  (`command/AbstractDialog.ts:66`).

i.e. in the 0.6 tree vendored here it is **chips bottom-right, options
bottom-left**. KIWI ships the mirror (prompts left, verbs right) because that is
what this round specified; `KHINT_PROMPTS_LEFT` in `kiwi_hints.cpp` is the one
constant that swaps them.

The **content model** is the part that answers "not very helpful". The old panel
showed ONE list — the command's keys *or* the selection's verbs, never both — and
never showed a command's own invented keys at all (a drawing tool's `Z` vertical
constraint shipped in shakeout H was advertised **nowhere**). Now:

- **bottom-left = what the keys do right now.** With a command live: the
  framework's grammar, *derived* here because it is identical for every modal
  command (`RMB/Enter` confirm · `Esc` cancel · `Drag`/`LMB` · `X/Y/Z` axis ·
  `Shift+XYZ` plane on Move · `Tab` field · `0-9` exact · `V` pivot on Move and
  Rotate) **plus** whatever the command adds through the new
  `KiwiEditorCommand::HudPrompts` virtual (static list; the strip copies the
  structs, borrows the strings, rebuilds every frame). With nothing running: the
  selection's universal keys, or — with nothing selected — the CREATE chords.
- **bottom-right = what can I do with what is selected.** The verb menu for the
  dominant kind, or for a construction selection. **Empty while a command runs**:
  a verb list about the selection is noise in the middle of an edit to it.

Every verb chip still reads its key **live** from `g_radiantCommands`, exactly as
the old panel did. The prompt chips are literals, because the keys they name are
handled inside `KiwiCmd_KeyDown` and the command's own `KeyDown` and are not table
rows at all.

### The cube

The body lost every hue. Six saturated faces made a 58-pixel widget the most
colour-dense object on screen **and** put it in direct competition with the §18
axis language it was borrowing — a red face and the red X world axis meant two
different things at once. Blender's gizmo is a neutral body with colour on the
AXES only, and that division of labour is the whole point: grey says "chrome",
colour says "axis". So: faces at 0.62 grey (front) / 0.30 (back) / 0.78 (hovered),
**dark** grey labels on the light fill, **darker**-grey edges (on a light body a
dark edge draws the silhouette; a light one fuzzes it), and three short coloured
axis stubs at 1.32x the half-size, projected through the same orthographic
relation as the corners so they cannot drift out of register. Corner targets stay
invisible until hovered. Projection, painter sort, hit test and view snap are
untouched — this is paint.

### The layout

`ImGuiShell_BuildDefaultDockLayout` grows a right-hand column, split in half:
**2D View** top, **Textures** under it, with the console strip now under the
**camera only**. Split order matters — the column is taken from the ROOT first, so
the console split that follows can only eat the camera's node.

Two things had to change together, and this is the trap: the windows also have to
be **open** (`kiwi_windows.cpp` `defOpen`), because shakeout B closed all four.
And a default change is invisible to an existing install, so **both** persistence
layers are versioned: the dock ini bumps `kiwi_dock4.ini` -> `kiwi_dock5.ini`, and
`[KiwiWindows]` gained a `DefaultsVersion` key that re-seeds the five flags once.
`Z` and the KIWI shell panel stay closed — the directive named two windows, and
the 3D-first ruling still holds for the rest.

### The scale

The **world** did not change: a unit is still an inch, the default grid is still
10 in. What was wrong is every number deciding how much of it you see at once and
how fast you move through it:

| lever | was | now | why |
|---|---|---|---|
| `KCAM_DEF_DIST` | 256 | **96** | 21 ft of standoff before you have touched anything is why a fresh map reads as an empty plain. 8 ft makes a 128-unit brush fill the frame. |
| `KCAM_DOLLY_STEP` | 0.85 | **0.90** | 15%/notch more than halves the distance in two notches once the reference is small. 10% halves it in ~7. |
| fly multiplier default | 10x | **4x** | the 10x was compensating for the two rows above. 4x = 1400 u/s crosses a 4096-unit map in ~3 s. **The slider is unchanged.** |
| map-new / map-load camera | `(0,0,48)` / `(0,0,0)` at the origin | **`(0,-160,96)` looking at the origin** | the ported placements start you *inside* the world with nothing in frame, so nothing gives the world a size. Modern-input gated; the classic profile keeps the ported placement byte for byte. |

The fly profile entry is renamed `FlySpeedScale2` -> `FlySpeedScale3` so a
persisted `1000` from the shakeout-A build cannot pin the old default — the same
trick that rename made when it moved 1x -> 10x.

### Camera during a command

§4 says camera navigation never stops for a command, and three of the four
gestures already honoured it: `KiwiVP_CameraButtonDown` dispatches MMB and RMB
**above** the command arm, and `KiwiCmd_MouseButton` refuses everything but LMB
(`kiwi_command.cpp:881`), so MMB orbit and RMB-drag pan reach the camera with a
command HOT *or* PAUSED; `KiwiVP_CameraWheel` never consults the command at all.
The RMB click-vs-drag split is what makes pan reachable — travel past
`KVP_RMB_CLICK_PIXELS` (4) is navigation, a no-drag release is the confirm.

The **arrow fly** was the exception: `KiwiVP_CameraTick`'s hover arm tested
`!KiwiCmd_Active()`. That test is gone. It is safe rather than hopeful: nothing in
the tree consumes an arrow key inside a command (the only `VK_LEFT/RIGHT/UP/DOWN`
references outside the camera layer are the fly's own,
`kiwi_camera.cpp:498/533-536`), and the fly reads keys with `GetAsyncKeyState`, so
it never enters the message queue and cannot double-fire a binding.

## 41. Round J — the last Plasticity verbs

**USER DIRECTIVE, verbatim:** *"See if you can come up with more plasticity
authentic features to surprise me with while I'm gone. Good luck!"*

So the whole round is a keymap read: `plasticity/src/startup/default-keymap.ts`
end to end, cross-checked against `src/Menu.ts` (which carries the same
accelerators *with their menu labels*, i.e. the author's own description of what
each one means). Six things shipped; two were considered and one of those is
explicitly **not** shipped, with the reason below.

### 41.1 The hide / isolate family (`H` · `Shift+H` · `Alt+H` · `Ctrl+H`)

`default-keymap.ts:296-299` and `Menu.ts:48-51`:

| key | command | Plasticity's own label |
|---|---|---|
| `h` | `command:hide-selected` | "Hide selected" |
| `shift-h` | `command:hide-unselected` | "Hide everything other than selected" |
| `alt-h` | `command:unhide-all` | "Unhide all" |
| `ctrl-h` | `command:invert-hidden` | "Invert hidden" |

**There is no isolate MODE in Plasticity.** Isolate *is* `hide-unselected`, and
you leave it with `alt-h`. A mode would need its own state, its own restore set
and its own answer to "what happens when the selection changes underneath it" —
none of which exist upstream and none of which anyone asked for.

Three quarters of this were **already ported and already wired**, on the same
letter: `Select_Hide` (`select.cpp:4160`), `Select_HideUnselected` (`:4185`) and
`ShowHidden` (`:4245`), on ids 32923 / 32934 / 32924, bound H / Alt+H / Shift+H
(`mainfrm.cpp:992-994`). Classic Radiant simply has **shift and alt the other way
round**. The modern profile swaps those two rows — the ids trade chords with each
other, so nothing is displaced and nothing becomes unreachable — and `Shift+H`
starts meaning *isolate*, which is what it is used for.

The fourth, `invert-hidden`, **had no Radiant equivalent in any spelling**: no
command id, no menu item, no core. `kiwi_visibility.cpp` adds one, written against
exactly the two fields the three ported cores touch — the hidden bit
(`brushFlags & 4`) and the hide depth (`xx5`) — and its per-brush body is
literally `ShowHidden`'s (`:4249-4250`) or `Select_Hide`'s third pass
(`:4179-4180`), chosen per brush. It therefore **flattens the hide depth to one
level**, exactly as `Select_Hide`'s own third pass does to its targets; that is
the house behaviour rather than a new rule.

**Undo: none, and that is faithful.** None of the three ported handlers opens a
bracket (`mainfrm.cpp:4963-4975`) because hiding changes no geometry — it is view
state, as it is in Plasticity, whose hide commands go through the Scene's
visibility database and not through its history. Per §4's rule ("a command that
mutates nothing must not open a bracket") the KIWI one opens none either. `Ctrl+H`
is its own inverse.

### 41.2 Focus on selection (`/`)

Two Plasticity verbs are easy to confuse and only one of them is "frame this":

* **`/` → `viewport:focus`** (`default-keymap.ts:327`, `Menu.ts:53` — *"Focus
  camera on selected"*). `Viewport.tsx:568-571` collects every selected thing
  regardless of kind and hands it, plus `scene.visibleObjects`, to
  `navigationControls.focus`. The view **direction is untouched**; only the target
  and the standoff move.
* **`space` → `viewport:navigate:selection`** (`:231`). `Viewport.tsx:156` →
  `_navigate(cplanes.constructionPlaneForSelection(...))`, which **re-orients** the
  camera onto the selection's construction plane, swaps the active construction
  plane and may transition to ortho. That is "look at this face square on".

**KIWI ships the first, on Plasticity's own key.** The second is not shipped and is
not half-shipped: it owns the construction plane (KIWI already has five commands
for that), and `space` in classic Radiant is `CloneSelection` 33001
(`mainfrm.cpp:1055`) — one of the most-pressed keys in the editor.

`kiwi_focus.cpp` frames, in order: the legacy brush selection via the ported
`Select_GetBounds` (`select.cpp:2056`); the **KIWI construction selection**, which
lives outside `selection_t` and would otherwise make `/` a no-op in a pure
construction workflow; and, with nothing selected, the whole **visible** world —
Plasticity's `visibleObjects` fallback, which makes `/` the "where *is* everything"
key too. Hidden brushes are excluded from the fallback only; a brush that is both
hidden and selected was named explicitly and is framed.

`KiwiCam_FrameBounds` keeps the current angles, seats the orbit pivot on the box
centre and solves the standoff from the box's bounding sphere against the smaller
of the two half-FOVs — using the **same** `tan(fov/2) * 0.75` the ray builder and
`KiwiCam_WorldPerPixel` use, so a frame is exactly tight on the axis that clips.
Margin 1.15; the radius is floored at 16 units so framing one tiny brush does not
park the eye inside it.

### 41.3 Duplicate (`Shift+D`)

`shift-d` → `command:duplicate` (`:280`). `DuplicateCommand.ts` is short enough to
quote its shape: duplicate every selected item, **replace the selection with the
copies**, and — the last line of the file — `this.editor.enqueue(new
MoveCommand(this.editor), false)`. Duplicating without placing is not a thing
Plasticity does.

Both halves already existed here: `Clone_Selection` (`select.cpp:2492`) ends with
`Select_Deselect(1)` and the copies on `selected_brushes`, and
`KiwiCmd_AfterPaste` (shakeout G, §33) starts Move **paused** over whatever is
selected. The classic tail is replicated exactly, `sub_47B940` loops included.

**It also fixes an undo hole.** `Cmd_OnSelectionClone` (`mainfrm.cpp:2973-2980`)
opens **no bracket at all**, and `Undo_Undo` only removes brushes stamped by
`Undo_EndBrushList` — so Ctrl+Z after a classic Clone undoes whatever came *before*
it and leaves the copies. `KIWI_CMD_DUPLICATE` uses the **creation** bracket
§23/`kiwi_extrude.h` documents: `Undo_ClearRedo` → `Undo_GeneralStart("duplicate")`
→ clone → `Undo_EndBrushList(&selected_brushes)` → `Undo_End()`, with **no**
`Undo_AddBrushList`. AddBrushList is what saves a restorable clone of an *existing*
brush; a duplicate modifies nothing existing, so there is nothing to save, and
"remove every stamped brush with no saved clone" is precisely "delete the copies".
Verified in `undo.cpp`: `Undo_EndBrushList` needs only an open record (`:578-579`),
not a preceding AddBrushList.

The Move that follows opens its own bracket, so duplicate-then-place is **two**
Ctrl+Z presses. That is the honest decomposition: two intentional acts, two
records — the same split the classic Paste + Move already has.

### 41.4 Offset curve (`O`)

`o` → `command:offset-curve` (`:268`), one scalar gizmo
(`gizmo:offset-curve:distance`, `:133-135`). `OffsetCurveCommand.ts` takes
`selected.curves.first`, feeds the factory `activeViewport?.constructionPlane`
(`:15`) and — the detail that matters — **leaves the original alone**: its tail
only removes the source from the *selection* (`:34`). Offset **adds**.

KIWI ports the curve arm. The **face** arm (offset a solid face's boundary loop
into a new curve) is not shipped: it needs a face-winding to construction-object
converter that does not exist, and half of it would be worse than none. The whole
2D core is reusable when it does.

* **Source**: the first selected construction object, at any granularity.
* **Plane**: `KiwiCon_ObjectPlane` (the Newell fit) where there is one; for a
  **straight** chain — a 2-point line, or a collinear polyline, which determines no
  plane and which Newell correctly refuses — the *active construction plane's*
  normal decides which way "sideways" is, orthogonalised against the chain so the
  built plane genuinely contains it. That is exactly the fallback Plasticity feeds
  its factory. A chain running *along* that normal is refused, with a message
  naming the plane to change.
* **Circles and arcs** are parametric, so the offset is `radius += d` — exact, not
  tessellated, which is the whole reason §7 keeps them parametric.
* **Joins**: miter, degrading to a two-point **bevel** past `KOFF_MITER_LIMIT`
  (4 x |d|, i.e. corners sharper than ~29 degrees) or when the two edges are
  parallel.
* **Sign**: for a closed loop positive is *outward* (the winding decides, via
  `KiwiRegion_SignedArea`); for an open chain it is the right of travel. The drag
  maps the cursor's own signed side, so the offset goes where the cursor is and
  nobody has to know the convention.
* **It refuses rather than mangles**: a self-intersecting result (asked with the §8
  toolkit's own `KiwiRegion_SelfIntersects` for a loop, so an offset that would be
  rejected as a *region* is rejected here first), a loop whose winding flipped (an
  inward offset past its own medial axis), a circle collapsing to a point, or a
  result over `KCON_MAX_POINTS`. The HUD goes red and Commit becomes Cancel.

*Undo*: construction domain. One `KiwiCon_UndoPush` at Commit, immediately before
the single `KiwiCon_Add`, and only after every refusal test has passed — the
"nothing can fail after the push" rule §37 learned the hard way.

### 41.5 Fillet corners (`B`)

**The verb is ported; the key is KIWI's own, and the header says so.** Plasticity's
`b` is `command:fillet-solid` (`:264`) — `MultiFilletFactory` over **solid edges**,
which KIWI does not have. Curve corners are filleted by **`modify-contour`**, whose
gizmo carries a fillet-all magnitude (`ModifyContourGizmo.ts:14`,
`"modify-contour:fillet-all"`, bound to `d` inside the command, `:169-171`) and
which has **no top-level chord at all**. So B is claimed because it is the fillet
letter and nothing in KIWI wants it.

The math is `ContourFilletFactory.ts`, matched rather than invented:

* **which corners** — `:44-47` sizes the radius array as `segments - (closed ? 0 :
  1)`, i.e. *every* vertex of a closed chain and every *interior* vertex of an open
  one. KIWI uses exactly that set.
* **per-corner control** — `:53-72` (`cornerAngles`): with control points selected,
  only *those* corners; with none, all of them. KIWI gets this for free, because the
  construction selection already addresses points (`KCONSEL_POINT`): select the
  chain in Object/Edge mode and every corner rounds, select individual anchors in
  Point mode (`1`) and only those do. **The round's brief said per-corner could be
  deferred; it is not deferred.**
* **one radius** — `fillet-all` is a single magnitude for every chosen corner, so
  that is what ships. Per-corner *radii* would need per-corner gizmos and are a
  separate feature, not a half one.

**The clamp is what stops it mangling.** A fillet of radius `r` at an interior
angle `theta` eats `r / tan(theta/2)` of each adjacent edge; two corners sharing an
edge that both eat more than half of it cross. So each corner is clamped to
`t_avail = 0.5 * min(|BA|, |BC|)`, half even at an open chain's free ends (one
always-safe rule beats two that differ by which end you are at). Corners within
`KFIL_FLAT_DOT` of straight or of a full reversal are **skipped**, not
approximated. Consequence, stated plainly: **typing a huge radius saturates rather
than failing**, and the HUD says when clamping is happening.

**Fillet replaces; offset adds.** `ContourFilletFactory` carries an `originalItem`
(`:74-76`), which is a Plasticity GeometryFactory saying "my result replaces this".
So the source object is removed and the rounded one added — one
`KiwiCon_UndoPush`, one Ctrl+Z. The result is a **tessellated polyline**, at the
store's own `KCON_SEGS_PER_UNIT` density, for the reason §16b already gave for
splines and polygons: a new `kconType_t` is a sidecar format change bought for
nothing.

### 41.6 Repeat last command (`Shift+R`)

`shift-r` → `edit:repeat-last-command` (`:304`, `Menu.ts:42`).
`CommandExecutor.ts:58-61` is two lines: re-construct `lastCommand` and enqueue it.

The subtlety worth copying is **what sets `lastCommand`**: only the executor, i.e.
only things that are `Command` subclasses. Plasticity's `viewport:*`,
`selection:mode:*`, `selection:convert:*` and `edit:*` actions are plain callbacks
on the editor and never touch it — `edit:repeat-last-command` is itself one of
them. KIWI mirrors that split exactly: `KiwiCmd_Dispatch` records the id it just
ran unless it is in a short, explicit exclusion list (the selection modes and
conversions, the palette and add menu, the grid/snap helpers, the construction
planes, the window/view toggles, the unified undo, Focus, and Repeat itself).
Everything else — every modelling and creation verb — is repeatable, and a **new**
id defaults to repeatable, which is right for almost anything that will be added.

Classic ids are **not** recorded, deliberately: hooking all ~313 cases of
`Radiant_DispatchCommandDirect` would be a ported-logic change for a feature none
of them ask for.

*Keymap cost, and it is the round's one uncomfortable trade.* `Shift+R` in the
modern profile was `MouseRotate` 32810 — parked there by this same profile when it
took bare `R` for the real Rotate command. It moves one more step, to `Alt+R`
(mods 2, the only free non-Alt chord left on R). `MouseRotate` is the classic
2D-view rotate *mode* that bare R has superseded; it stays on the menu and in the
palette, and `Alt+R` inherits the Alt-chord limitation already in
RADIANT_KNOWN_ISSUES.

### 41.7 NOT SHIPPED: X-ray / wireframe (`Alt+Z`)

`alt-z` → `viewport:toggle-x-ray` (`:233`), `Viewport.tsx:486-490` →
`editor.layers.toggleXRay()`. In Plasticity, X-ray does two things at once: it
draws solids translucent **and it lets selection reach occluded geometry** — the
layer mask it flips is read by the *raycaster*, not only by the renderer.

**Half of it is reachable here and half is not, so none of it ships.**

* The renderer half has a genuine ported hook: `camera.draw_mode` (`mainfrm.h:33`)
  is fed to `Cam_TechForDrawMode` (`camwnd.cpp:414-430`), whose `case 0` is
  `TECHNIQUE_WIREFRAME_SHADED`. Nothing in the tree binds a command to it (no id,
  no menu item), but writing the field is a one-liner and the technique path is
  already exercised by the 2D views.
* The selection half is **not** reachable without changing `kiwi_pick.cpp`. KIWI's
  picking is a ray test against brush geometry with nearest-hit-wins; it has no
  notion of a per-object pick mask and no "ignore occlusion" flag. Adding one is a
  real change to the layer every other feature in this overhaul depends on, and the
  round's brief says explicitly: *do not invent renderer changes*, and a feature
  whose faithful version is infeasible gets a paragraph rather than a half version.

A wireframe *view toggle* on `Alt+Z` would look like X-ray and not behave like it —
you would see through the wall and still be unable to click what is behind it,
which is worse than not having the key. The honest sequence is: add an occlusion
flag to `Pick()` first, then wire both halves to one toggle. `draw_mode 0` is
recorded here as the hook that is waiting.

### 41.8 Also considered

* **`space` / `shift-space`** — `viewport:navigate:selection` and
  `viewport:grid:selection`. See §41.2: the first re-orients rather than frames and
  collides with Clone; the second sets the construction plane from the selection,
  which is a real feature and a good next one (`KIWI_CMD_CPLANE_FACE` is its
  cursor-driven cousin), but it is a construction-plane change and belongs with
  that family rather than bolted on here.
* **Invert selection** — already ported and already bound (`Select_Invert`,
  `select.cpp:1176`, id 33101 on `Ctrl+I`, `mainfrm.cpp:1058`) and already in the
  palette. Nothing to do; Plasticity has no invert-selection key at all.
* **`shift-v` = `command:center-box`** — still the one deliberate deviation from
  Plasticity's creation chords (§16b): KIWI has no centre-box primitive, and
  `Shift+V` holds Rectangle (centre) so the chord will not have to move when it

---

# Part VII — Round L (2026-08-10): the grab jump, the riding pivot, Cut, Boolean

Round L answers four pieces of user feedback. Two are direct-manipulation bugs
whose fixes are small and load-bearing; two are workflow verbs. As always, every
claim about Plasticity below was read out of `plasticity/src` and the cite is in
the owning header.

## 42. The grab must not pre-apply anything

> *"When clicking on the gizmo after selecting a shape, as soon as the gizmo is
> clicked it pre-calculates the mouse delta and applies it. It should ONLY move
> with gizmo drag, no pre existing mouse offset."*

Round K fixed this for the **lollipop** by writing a careful four-step arm in
`kiwi_lollipop.cpp` and leaving `kiwi_gizmo.cpp`'s older copy of the same arm
alone. Two copies of a sequence whose entire value is its *order*, and the
gizmo's copy was a rung short.

**There is now ONE arm**, `KiwiCmd_HandleGrab` (`kiwi_command.h`), and every
handle in the editor calls it — the three move arrows, the three plane corners,
the centre square, the face-normal arrow, the three rotate rings and the
lollipop's ball:

1. feed the **press pixel**, so `KiwiCmd_LastCursor` is the grab point;
2. **resume** (which `Rebase()`s) if PAUSED, else `Rebase()` directly;
3. open the command's own gate — `KiwiEditorCommand::HandleGrab(true)`, which
   replaces round K's lollipop-only `LollipopGrab`;
4. **rebase again**, in case the gate moved the mapping origin;
5. feed the move once more — the first fed frame, and its delta is exactly zero.

| handle | before | after |
|--------|--------|-------|
| Move arrow / plane / centre / normal | `if (wasPaused) Resume()` only, so the **first** grab after `G` (command still HOT) never re-latched; and the SNAP arm — `total = snapPos - ref`, an *absolute* mapping a re-latch cannot zero — fired on the grab frame | the shared arm always re-latches, and the total is frozen until the cursor leaves the grab pixel |
| Rotate ring | the ring fed an **absolute** angle and zeroed its accumulator on every grab, so re-grabbing a ring fed `0 deg` and the apply-from-baseline residual spun the selection back to the start | the ring feeds a **sweep since the grab**; `KiwiRotateCommand::HandleGrab` latches the base it is added to, so frame one feeds the angle already applied |
| Lollipop ball | correct since round K | unchanged behaviour, now via the shared arm |

**The grab-freshness freeze** (`KiwiMoveCommand::NoteGrab` / `AgeGrab` /
`GrabLive`) is the second half and the one that makes the guarantee absolute. A
re-latch zeroes a *delta* mapping; it does nothing to the snap arm or the grid
quantiser, which are absolute. So the grab pixel is latched too and **all** of
the cursor-driven half of `Recompute` is held until the cursor leaves it. One
pixel of movement releases it, so nothing perceivable is lost — and until then a
press on a handle moves nothing whatever is underneath it. Rotate has the same
freeze on its 5-degree snap, so a re-grab cannot quantise a typed angle either.

## 43. The pivot rides the object

> *"The pivot point is broken when moving a box because the gizmo doesn't move
> with the object. Fix this. I should be able to snap corners together easily by
> placing a pivot and moving it."*

Shakeout D anchored the gizmo on `KiwiMoveCommand::m_ref`, the **latched**
reference point, and argued that a translate gizmo which crawls with its geometry
runs away from the cursor. That is true only when the draw anchor and the
*mapping* anchor are the same value. Round L separates them:

* **mapping anchor** — still `m_ref`, latched for the whole gesture. `MapCursor`
  measures from it, so the drag mapping can never move under the drag.
* **draw / hit-test anchor** — `m_ref + whatever has been applied`
  (`KiwiMoveCommand::LiveAnchor`, served through `KiwiXform_ActivePivot`). The
  arrows travel *with* the cursor and stay on the object; the pivot marker rides
  with them.
* **commit** — an OBJECT move carries the session pivot with it
  (`KiwiMoveCommand::PivotRide` -> `PivotStore(m_ref + m_total)`), so the next
  grab starts from where the corner now is instead of from where it was.
  Face / edge / vertex moves *reshape* rather than translate the selection, so
  there is no one vector the anchor rode and the pivot stays put.

**The snap reference was already the pivot** and is now documented as such:
`Begin` overrides `m_ref` with the session pivot (`PivotActive(m_ref)`), and
`Recompute`'s geometry-snap arm is `total = snapPos - m_ref` — it drags *that*
point onto the target. Placing the pivot on a corner is therefore exactly what
makes that corner the thing that snaps; what was missing was the ride, without
which it only worked once.

## 44. Cut is two stages, and the plane locks at the click

> *"The cut workflow is clunky. It should be: Select a solid, press C, then the
> selection expects a line to be selected. Also the way it cuts needs to be
> decided at line click-time and not update as the camera moves."*

| | shakeout G | round L |
|-|-----------|---------|
| precondition | >= 1 brush **and** a construction line already selected | >= 1 brush. The only remaining refusal is "the map has no construction line at all", because *click a line* is unanswerable from inside a modal gesture |
| choosing the line | read out of the construction selection before `C` was pressed | **stage 1**: the segment under the cursor highlights (10 px, `KCON_LINE_PIXELS` — the shared construction clickbox) and clicking it locks the plane |
| the plane | re-derived from the live camera every HOT frame, frozen on pause | derived **once**, in `Click()`, from the clicked line and `vpn` at that instant. `AimFromCamera` has exactly one caller |
| confirming | RMB / Enter | **stage 2**: red sweep quad + brush outlines, RMB / Enter cuts |
| Esc | cancels | walks back one stage (preview -> pick a line), then cancels |

The line is picked by a local segment scan rather than `KiwiConSel_PickAt`, for
the reason `kiwi_matchface.cpp` re-casts its own ray: that entry point resolves at
the granularity of the **current selection mode** and names no segment at all in
Object / Face / All mode. The tolerance is the shared constant, so the clickbox
is identical to every other construction pick.

The splitter itself (section 32) is untouched, and so is the undo shape.

## 45. Boolean (`Q`) — difference and union

> *"Add a new feature for solids(brush). Boolean and difference. (Q). First
> select a solid, then press Q, another solid is expected to be picked. Click on
> that solid and it automatically enters difference mode, however press Q again
> and it enters boolean(combining) mode."*

**Plasticity's own key and its own default.** `default-keymap.ts:294`
`"q": "command:boolean"`; `BooleanFactory.ts:272`
`private _operationType = c3d.OperationType.Difference;`. Its target/tool split is
a `shift` off a copy of the live selection (`BooleanCommand.ts:43/46`) with the
leftovers becoming the tools (`:91`), which is exactly "the selection is the
target, the click names the tool". Its preview colours the tool **by operation**
(`BooleanFactory.ts:203-205`, red for difference). Four deliberate deviations —
`Q` toggles instead of Plasticity's absolute `q`/`w`/`e`; no intersection; the
tool **survives** (Plasticity consumes it, `BooleanFactory.ts:264/325-328`); one
tool rather than a set — are argued in full in `kiwi_boolean.h`.

**There is no subtract core in this port** (`kiwi_csg.h`'s Phase-5 inventory of
`csg.cpp` is `CSG_MakeHollow`, `Brush_MergeList`, `CSG_Merge`), so difference is
built out of the one thing the port does have — the two-halves splitter over
`Brush_SplitBrushByFace`, now reachable at the **def level** without landing
anything (`KiwiSplit_DefByPlane`). The classic brush-CSG subtract:

```
remainder = target
for each face plane (n_i, d_i) of the tool, outward normals:
    split remainder by that plane
    front = { n.p >= d }  the part OUTSIDE the tool here   -> a result piece
    back  = { n.p <= d }  the part that may still be inside -> the new remainder
    front == NULL -> wholly inside this half-space; carry on
    back  == NULL -> wholly outside the tool: the two solids miss. Stop.
discard the final remainder            it IS target AND tool
```

Every piece is a convex half-space intersection by construction, so every piece
is a legal classic brush, and their union is exactly `target \ tool`. At most one
piece per tool face — six for a box, which is also the classic result.

**Validation and undo.** Every half comes back section-19-gated and **unlanded**,
so a rejection is free (the `kiwi_extrude.h` rule). The policy is all-or-nothing:
a target that yields zero pieces, or whose carve hits any invalid piece, is left
completely untouched with a console message. A target *entirely inside* the tool
yields zero pieces — classic Radiant would delete it, KIWI declines and says so,
because a verb that silently removes a brush you can no longer see is the worse
failure. One undo bracket covers the whole verb: land every piece, then free each
carved original, in that order (section 32's ordering, for the same reason).

**Union** owns no bracket and no geometry: it selects target(s) + tool through the
ported funnels and dispatches the **classic** id 32927, exactly as `kiwi_join.cpp`'s
face arm does. Every refusal — patches, fixed-size entities, different owner
entities, and a union that is **not convex** — is `CSG_Merge`'s own and is printed
by `CSG_Merge`. Nothing here fakes a non-convex union; a classic brush cannot be
one.

**Preview.** Difference draws the tool red and translucent (`0.22` alpha — the
value the region fills chose because Plasticity's `0.1` reads as nothing over a
lit wall) with both operands outlined; union tints both green. Fills are capped
at 8 solids (one `R_AddRenderCmdDrawTris` per face), outlines share the
framework's 192-segment batch and stop when it is full.

# Part VIII — Round P (2026-08-10): the MMB jump, the chained curve, axis guides

## 46. The MMB orbit jump, and the latch rule

**USER REPORT, verbatim:** *"having a bug where the camera jumps while using mmb.
Mmb shouldn't jump the camera, just smoothly rotate it with the mouse. Has to do
with raytrace against the object and the mouse moving off the object while the
camera rotates. Keep the pos fixed so it doesn't jump the camera."*

Two independent jumps, both re-derivations, and the user's instinct named the
source of both.

**Jump 1 — the first drag frame re-seated the eye on the pivot's axis.**
`KiwiCam_OrbitBegin` picked the surface under the **cursor**, which is by
definition off the view axis, and stored it as `look_at`. `KiwiCam_OrbitDrag`
then did `origin = look_at - forward * dist`, i.e. it *assumed* the eye was
already exactly `dist` back along the view axis from `look_at`. The first frame
with a non-zero delta therefore slid the eye across the sphere of radius `dist`
until the pivot was dead-centre — a rotation of the whole view by the pivot's
off-axis angle, in one frame, from no input. Its size is exactly how far off
centre the cursor was: with the default FOV the half-image is `0.75*tan(fov/2)`
of the depth, so an MMB press near the top of the viewport swung the camera
~35-40 degrees instantly. Grab the middle of the screen and it is invisible; grab
a corner and it is violent — which is why it read as intermittent.

**Jump 2 — the press re-latched `s_dist`, and in ortho `s_dist` is the zoom.**
`OrbitBegin` also set `s_dist` to the picked point's distance. Since round M the
camera is orthographic **by default** and `KiwiCam_OrthoHalfHeight` is
`s_dist * tan(fov/2) * 0.75`, so pressing MMB on a near wall instead of the far
floor instantly rescaled the whole image. No drag needed — this one fired on the
press edge alone, which is the "consecutive presses re-tracing to wildly
different depths" case.

**The rule now** (round L's grab-rebase discipline, applied to the camera):

| what | rule |
|---|---|
| `OrbitBegin` | latches pivot, the eye's **offset** from it, and the angles. Nothing re-derives any of it until `KiwiCam_OrbitEnd` (called from the release **and** abort arms). |
| `OrbitDrag` | applies the **total accumulated** pixel delta to those latched values as one rigid rotation of the offset, so a zero delta reproduces the camera exactly and the grabbed point holds its screen position for the whole gesture. |
| `s_dist` | **not touched by the orbit at all.** It is the view's reference distance (ortho zoom, dolly reference, pan fallback); an orbit is not a zoom. |
| the wheel | the one path that can still reach in mid-gesture (section 4: navigation never stops). `KiwiCam_Dolly` leaves the pivot alone while an orbit is latched and re-latches the offset afterwards, so the dolly **composes** with the orbit. |

The rotation is derived, not tabulated: with this layer's angle convention
`f(p,y) = (cos p cos y, cos p sin y, sin p)` and `CamWnd_BuildMatrix`'s roll-free
right `r(y) = (sin y, -cos y, 0)`, differentiating `f` by `p` gives exactly
`r(y) x f` — so Rodrigues about `r(y0)` by `+dp` **is** the pitch change, and
about `+Z` by `+dy` **is** the yaw change. Applying both to the eye offset moves
the eye by the same rigid rotation the angles undergo.

**No distance smoothing was needed**, and that is a finding rather than an
omission: under the new formulation the camera's translation per pixel is
*proportional to* the orbit radius, so a pivot picked very close makes the
gesture gentler, not more violent. The old formulation's violence came entirely
from the one-frame re-seat.

## 47. Alt+MMB steps the six face views

**USER DIRECTIVE, verbatim:** *"add the alt-middle click shortcut that plasticity
has to cycle through the stages of the camera cube. (look it up)."*

**Looked it up — Plasticity has no such binding.** MMB is `mouse1` there
(`KeyboardEventManager.ts:131-143`), the default orbit scope binds only
`"mouse1": "orbit:rotate"` and `"mouse2": "orbit:pan"`
(`default-keymap.ts:330-333`), and `OrbitControls.onMouseDown` drops anything
else into `default: state = {tag:'none'}` (`OrbitControls.ts:362-392`). It cannot
reach a command either: only RMB is converted into a command keystroke
(`KeyboardEventManager.ts:56`) and `ViewportControl` refuses every button but LMB
(`ViewportControl.ts:97`). `alt-mouse1` gains meaning only in the optional orbit
presets — pan in Maya, rotate in 3ds Max (`ConfigFiles.ts:101-122`).

What Plasticity **does** have is six named axis views —
`viewport:navigate:front|right|top|back|left|bottom` (`Viewport.tsx:150-156`,
numpad-bound at `default-keymap.ts:218-231`, driven by its own nav cube at
`ViewportNavigator.ts:136`). Those *are* "the stages of the camera cube", so the
directive is honoured with them, on the chord the user asked for — which the
trace above shows is free.

**Alt + MMB click** (a press that never travels past `KVP_RMB_CLICK_PIXELS`; a
drag is still an orbit, deferred until that threshold so the click cannot nudge
the view): snap to the **nearest** face view if not already on one, otherwise
**advance** through front, right, back, left, top, bottom and round again. It
routes through the view cube's own `LookAlongDirection`, so the yaw lattice, the
pole handling and round N's spin reset are shared code.

## 48. The chained curve — Line and Polyline are one tool

**USER DIRECTIVE, verbatim:** *"You need to allow chain-lines being created. I
should be able to make a square or more advanced shape without opening the line
tool again. blam, blam, blam, blam, click it out and it should be 1 line when
finished. (Maybe the entire line concept is called a 'curve' in plasticity, look
it up). We need this throughout the entire system. Too slow to do A->B over and
over again."*

**The guess is exactly right.** In Plasticity "line" *is* "curve" —
`LineCommand` is a three-line subclass of `CurveCommand` that changes only the
c3d curve type and the keyboard gizmo (`CurveCommand.ts:68-71`), and `shift-a` —
the key this editor already uses — binds `command:line`
(`default-keymap.ts:283-284`). The chaining is a `while (true)` around **one**
point picker (`CurveCommand.ts:40-60`); `point-picker:finish`, bound to **both**
`mouse2` and `enter` (`default-keymap.ts:353-363`), rejects with a `Finish`
exception which breaks the loop and commits one curve (`CurveCommand.ts:56-64`).
Closing is a named `PointSnap` on the start point once there are 3 or more points
(`CurveCommand.ts:73-81`) plus `wouldBeClosed` (`CurveFactory.ts:114-116`).
Point removal is `gizmo:line:undo` calling `pointPicker.undo()` and
`makeCurve.undo()` (`CurveCommand.ts:31-36`).

**The KIWI grammar** (`KiwiCurveTool`, `kiwi_construct.cpp`):

| input | meaning |
|---|---|
| click | place a point; the rubber band runs to the next |
| click near the **first** point (3+ points) | close the loop and finish |
| double-click (2+ points) | finish |
| RMB / Enter | finish the whole chain as **one** object |
| Esc | remove the last point; an empty chain leaves the tool |
| Z | the shakeout-H vertical lock (still here — see section 49) |

RMB and Enter are literally the same path: `KiwiCmd_Confirm` is
`KiwiCmd_KeyDown(VK_RETURN, 0)`, and the tool leaves `WantsEnterFinish` false so
both fall to the framework's Enter rung = `Commit()` = `Finish + Leave` — which is
what Plasticity's break-then-commit does.

**Both command ids run this one object.** `KIWI_CMD_DRAW_LINE` and
`KIWI_CMD_DRAW_POLYLINE` both resolve to `s_curveTool`; neither is unregistered,
so no keymap row, menu row or palette entry naming either goes dead. The menu,
palette and panel rows label the second as the alias it now is.

**Esc, not Ctrl+Z, drops a point.** Plasticity uses `ctrl-z`
(`default-keymap.ts:182-194`); that slot is not free here because shakeout I made
Ctrl+Z inside a drawing tool the one unified undo journal (section 39). Esc was
free and was doing something worse (clearing the entire chain).

**"Throughout the entire system" — the consumers, checked one by one.** The chain
stores as ONE `kconObject_t`, typed `KCON_LINE` at exactly two points and
`KCON_POLYLINE` otherwise — which is not a new rule but the convention every
other producer already follows (`kiwi_trim.cpp:558`, `kiwi_offset.cpp:577`,
`kiwi_conselect.cpp:826`).

| consumer | how it reads a chain | verdict |
|---|---|---|
| regions / arrangement (`kiwi_arrange.cpp:255`, `kiwi_region.cpp:419/456/627`) | `KiwiCon_SegmentCount` plus the `closed` flag; never switches on LINE vs POLYLINE | unchanged — a closed chain is now a one-object loop instead of four joined lines, which the half-edge walk prefers |
| extrude (`kiwi_extrude.cpp`) | consumes a **region**, not a curve | unchanged |
| Trim (`kiwi_trim.cpp:81`) | accepts LINE or POLYLINE explicitly | unchanged |
| Join (`kiwi_conselect.cpp:593`) | endpoint chaining, emits POLYLINE at any point count | unchanged |
| Offset (`kiwi_offset.cpp:577`), Fillet (`kiwi_fillet.cpp:477`) | walk vertices, re-type by count | unchanged |
| selection / snapping (`KiwiCon_AnchorWorld`, `KiwiCon_SegmentWorld`) | type-agnostic | unchanged |

## 49. Axis guides — drawing vertical by aiming

**USER REPORT, verbatim:** *"It's still hard to draw a vertical Z line from a 2D
xy line, even when using the camera. Make it more easy like plasticity. The
camera angle should really help."*

**Plasticity's point picker** keeps `straightSnaps` = X/Y/Z
(`PointPickerModel.ts:18,25`; `AxisSnap.ts:29-31`) and **rebuilds them through
the last picked point** every time a point is placed
(`PointPickerModel.ts:102-130`, via `PointSnap.axes` and `AxisSnap.move`, which
returns a named `PointAxisSnap`, `AxisSnap.ts:69-71`). They are ordinary
candidates: raycast at `Line2: { threshold: 30 }` **screen pixels**
(`PointPicker.ts:138-142`) and ranked type 2 — below every point snap, above
faces and planes (`SnapPicker.ts:136-161`). The guide draws only while that axis
is among the hit snaps (`SnapPresenter.ts:71-84`): a plus/minus 100,000-unit
line, grey `0xaaaaaa`, dashed, plus a dot on the source point
(`AxisSnap.ts:8-27,113-127`). `x`/`y`/`z` also lock an axis outright
(`default-keymap.ts:353-360`).

**KIWI's arm 4c** (`kiwi_snap.h`, `kiwi_snap.cpp`), with every deviation stated:

- **The axes:** world Z always, plus the working plane's U and V (our plane basis
  is the local frame Plasticity gets from the hovered face). A plane axis
  parallel to world Z is dropped — Plasticity's `isAxisAligned` dedup.
- **Radius 10 px, not 30.** This viewport is full of brush edges at 6 px and
  construction segments at 10 px; a 30-px axis would shadow both.
- **The camera angle helps** — the directive's real point. The **vertical** axis
  widens to **16 px** as the camera flattens toward the horizon (`|vpn.z|` at or
  below 0.35), tapering back to 10 px by `|vpn.z|` at or above 0.70. Looking at
  the horizon is both the posture in which "up" is what you are reaching for and
  the posture in which the vertical is easiest to aim at. Looking
  near-straight-down the axis projects to a **dot** and is refused outright (an
  8-px minimum projected length).
- **The point is grid-snapped along the axis**, so "straight up, exactly three
  cells" is one gesture — and **absolutely** when the axis is a world axis, which
  is section 50's rule for the transform and has always been the Z lock's (it
  quantises the world `z`). An off-grid anchor does not drag its offset up the
  axis. A slanted plane axis has no world coordinate to be on the grid of and
  quantises the distance instead.
- **It competes on pixel distance** with the LINE rank rather than taking a fixed
  priority over brush edges — "closest wins" is already this file's rule for the
  two line sources it had, and a real edge under the cursor is a thing the user
  is aiming at.
- **No x/y/z lock key.** The shakeout-H `Z` toggle is the lock we already have and
  it still works: it is now the **second** way to go vertical, with aiming at the
  guide being the first. The tool's HUD names both.

**The guide** is dashed (a run of short segments — `kiwi_lines` has no dash mode
and no alpha, TRAP 2, so "faint" is a dim grey 0.42/0.42/0.46), symmetric about
the anchor at 32 dashes per side, clipped to about 420 px each way at the
anchor's depth rather than Plasticity's plus/minus 100,000 — a world-length guide
either vanishes up close or crosses the whole map from far away. A dot marks the
anchor. Own budgeted batch (96 segments), drawn from the `Cam_Draw` tail beside
the round-N face accents. The snap label reads **"Z axis"**, which is what tells
the user they are *on* the vertical rather than near it.

## 50. Grid snapping is absolute

**USER DIRECTIVE, verbatim:** *"when moving something offgrid, the snaps no
longer are aligned to the grid. Fix this, the grid snaps should only be for the
grid, no offset, custom offsets are done each time."*

The Move command's grid arm quantised the **delta** (`KiwiGrid_Snap(total)`), so
an object 3 units off a 10-unit grid moved in clean multiples of 10 and stayed 3
units off it forever. The grid guaranteed nothing except that the error was
preserved.

**Move** now snaps the **moved reference point** to an absolute grid position:

    total = KiwiGrid_Snap( m_ref + total ) - m_ref

then re-applies the axis/plane constraint. `m_ref` is the session pivot whenever
one has been placed (`m_pivotOverridden = PivotActive(m_ref)`) and the natural
reference otherwise — so *which* point lands on the grid is a thing the user can
choose with `V`, which is the "custom offsets are done each time" half.

**Face push/pull** does the same wherever it can: when the push direction is a
world axis (`|pushDir[axis]| > 0.999`) the moved **plane's position along that
axis** is snapped —

    pos  = m_ref[axis] + sign * dist      // m_ref is the drive face's winding centre
    dist = ( snap(pos) - m_ref[axis] ) * sign

— and on a **slanted** normal it keeps quantising the distance, because there is
no single axis for the plane to be on the grid of. Deliberate and narrow.

**Geometry snaps are untouched.** Vertex / edge / midpoint / endpoint /
intersection / face-centre name an exact target and were never a grid question.
Ctrl still suppresses snapping entirely (section 6 arm 0).

> **SUPERSEDED IN PART BY ROUND Z (§56.2, D-Z2).**  Arm 0 is now an **XOR** against
> the active command's own default rather than an unconditional suppression.  For
> every command in the editor except three the behaviour above is unchanged (default
> = snap, Ctrl = no snap).  For the three **one-axis gestures** — the face push/pull
> (§20), the region extrude (§23) and the face extrude / un-extrude — the default is
> **no snapping at all** (no geometry arms, no grid quantisation) and **Ctrl turns
> the full ranked query on**.  The asymmetry is deliberate and is argued in D-Z2.

## 51. Round S — live split, grid ordering, Ctrl+X, and the Alt+MMB swipe

### 51.1 Ctrl+R splits the BRUSH, and the line is live

**USER DIRECTIVE, verbatim:** *"You misunderstood the split command (ctrl-R). It
only splits the face, not the whole brush (although that might be a tech
limitation?). Also it needs to have a live update when hovering the mouse around
to where it's going to split, it's not always just 50/50, let me customize it."*

**The first half is a real tech limitation, and it is now said out loud.** A
Radiant brush is a convex solid defined as the intersection of its faces'
half-spaces — `Brush_BuildWindings` (`brush.cpp:1459`) derives every winding by
clipping that face's plane against every other face's plane. A face has no
independent existence to divide: "two faces where there was one" is only
expressible as two coplanar faces belonging to two different convex solids, which
*is* the brush split. So the command is renamed **"Split Brush at Face"**, the
hint chip reads **"Split brush"**, the HUD line starts `split brush`, and `Begin`
prints the limitation in one console line before anything can be committed.

**The second half is the work.** The cut line slides with the cursor:

- **slide axis** `offDir = normalise( cross( faceNormal, lineDir ) )` — in the
  face plane, perpendicular to the line. The face winding is projected onto it to
  give the span `[lo, hi]`; the cut is one scalar `t` in that span.
- **cursor mapping** ray ∩ **the face plane** (not the snap layer's own fallback
  plane — the cut must stay on the face even when the cursor leaves it), then
  `t = dot(hit, offDir)`, clamped inside the span with a 0.5-unit guard at each
  end so neither half can come back a sliver.
- **snap** a GEOMETRY snap that lies on the face plane wins outright;
  `SNAP_FACE` is excluded from that rank because section 6 arm 6 is the ported
  `Test_Ray` surface hit **unsnapped** and would defeat grid snapping entirely.
  Otherwise the plane hit goes through `KiwiGrid_Snap` (the section 17
  world-anchored lattice). Ctrl still suppresses everything: the layer answers
  `SNAP_NONE` and the raw hit is used.
- **numeric** one field, `offset`, measured **from the face edge at the LOW end of
  the slide axis**. The HUD prints `offset X / span` so the number always carries
  its scale. Tab flips U/V and **resets to the centroid**, because the reference
  edge changes with the axis.
- **commit** RMB / Enter at the current position; Esc cancels. The field table
  stays at exactly ONE entry, because the Tab rung is
  `KiwiNum_FieldCount() <= 1` (`kiwi_command.cpp:1089`).

### 51.2 The grid draws before the world, and fades edge-on

**USER REPORT:** *"(see pic) Look at this grid bug!!"* + *"grid should never
render on top of shapes"*.

`KiwiGrid_Draw` moved to the head of `CamWnd_Draw`'s world section. Painter's
order makes "does the world hide the grid?" independent of the world's depth
state, which round O's `$default3d` substitution could not do on its own. The
grid keeps `$line` (depthTest LESSEQUAL, depthWrite ON) and still cannot occlude
world geometry — nearer wins, coplanar wins (LESSEQUAL admits ties and the world
is later), behind is correctly occluded. `$line_nodepth` is not used: the same
`default.sm` `depthWrite { mtlBlendOp == Disable: Enable }` rule forces its write
back on, giving depth-test-OFF + depth-write-ON, which is backwards here.

The edge-on collapse is a projection fact, not a LOD failure: lines on Z=0 project
with a separation proportional to `|vpn·Z|`, and the LOD is driven by camera
HEIGHT, which is exactly what has gone to zero. So a ramp on `|vpn·Z|`: skip
below **0.03**, linear brightness ramp to **0.08**, minors dropped below the
half-way point. Brightness, never alpha (`kiwi_lines.h` TRAP 2). The **axes are
outside the ramp** — three lines, no aliasing, and the only thing left saying
which way is which.

### 51.3 Ctrl+X is Cut

**USER DIRECTIVE, verbatim:** *"Ctrl-X should not quit the app!!!"*

It did: `IDR_MAIN_ACCEL` bound it to **32951 = `ID_FILE_EXIT_RAD`**. The
accelerator entry is removed (fenced in the .rc); File→Exit keeps the id. The
modern profile binds Ctrl+X to `KIWI_CMD_CLIP_CUT` = ported Copy (33039) then
ported Delete Selection (33003), whose own undo record covers the act. The
classic profile leaves Ctrl+X unbound.

### 51.4 Alt+MMB is a swipe

**USER DIRECTIVE, verbatim:** *"The alt-MMB shortcut should be more of a swipe,
it's hard to actually do a standstill press of mmb."*

Section 47's chord is unchanged in intent and rebuilt in mechanism: the Alt+MMB
gesture belongs to the view for its whole life and **never becomes an orbit**
(bare MMB is the orbit, untouched). The release reads the NET travel — under
**24 px** it is section 47's nearest-then-ring step, past it a 90° step in the
dominant screen direction. The mapping is `KiwiCam_OrbitDrag`'s own directions
quantised (`dyaw = -dx·k`, `pitch = pitch0 - dy·k`; the model follows the mouse on
both axes, as Blender's turntable does), starting from the face view nearest the
current camera:

| swipe | effect | ring |
|---|---|---|
| right | yaw −90 | front → left → back → right → front |
| left  | yaw +90 | front → right → back → left → front |
| down  | pitch −90 | side → **top**; bottom → the side view at the current yaw; top → nothing |
| up    | pitch +90 | side → **bottom**; top → the side view at the current yaw; bottom → nothing |

A horizontal swipe while already on a pole **spins that pole view** 90° in place.
Four arrowheads are drawn around the viewport centre while the swipe is live; the
one the travel points at lights up **only once the 24 px threshold is passed**, so
the widget teaches the threshold instead of hiding it.

## 52. Round V — crash safety

### 52.1 A lost device is a stall, not a fatal

**USER REPORT, verbatim:** *"After going AFK for a while and coming back I get this
crash... it ruins all progress of your map."*

Windows takes the D3D9 device away for entirely ordinary reasons — screensaver,
monitor sleep, workstation lock, a full-screen app, a driver TDR. The editor already
knows this: `R_TestDevice` polls `TestCooperativeLevel` on every paint,
`R_RecoverLostDevice` -> `R_ResetDevice` puts the device back, and `R_ResetDevice`'s
editor tail re-creates every child window's additional swap chain. The one place that
did **not** know it was the resize path: restoring the window after an AFK sends
`WM_SIZE`, `R_Hwnd_Resize` tries to build a new swap chain on a device that has none
to give, and the ported failure arm is `Com_Error(ERR_FATAL)`.

**The rule for round V and after: renderer code may only be fatal about failures that
have no recovery.** A failure the editor has a documented, already-implemented
recovery for must take that recovery. So the resize path distinguishes the two:

- **device lost** (`hr`, `dx.deviceLost`, or a fresh `TestCooperativeLevel`) -> release
  the half-created state, leave the slot as `swapChain == NULL` carrying the **new**
  size, return. The existing per-paint recovery resets the device and the existing
  reset tail rebuilds the chain at that new size. `R_SetupRendertarget_CheckDevice`
  already refuses to paint a window without a swap chain, so nothing else has to
  change; under DWM the view holds its last presented frame until it comes back.
- **device healthy** -> the `Com_Error` stays. That is a genuine out-of-memory /
  invalid-parameter failure, and turning it into a silently blank viewport would be
  worse than the crash.

One landing site was added, not invented: the loss can resolve without a Reset ever
running, in which case nothing would rebuild that one chain. `R_SetupRendertarget_
CheckDevice` retries the create in place — it is reached only once `R_TestDevice()`
has confirmed the device healthy, so it is the same call the reset tail makes.

### 52.2 The rescue save

The bug above was one instance of a class: **a fatal error must never be able to
destroy unsaved work.** Every deliberate death path in the editor now writes the map
out first.

- **Where.** `<mapname>_rescue.map` beside the real file; `<exe dir>\rescue.map` when
  the map is untitled or its own directory refuses the write. The construction sidecar
  is written next to it. **The real `.map` is never touched and `modified` is never
  cleared** — a rescue file is not a save, and the editor must not claim it was.
- **What is rescued.** Geometry: worldspawn brushes, patches, entities, layers, and
  the `.kiwi` construction store. Not rescued: undo history, selection, camera, and
  panel state.
- **Hooks.** `Com_Error`'s death arm, `Sys_Error`, `Sys_OutOfMemErrorInternal`, and
  `Sys_DirectXFatalError` (the `exit(-1)` behind `R_FatalInitError` /
  `R_FatalLockError`, which never passed through `Com_Error`). An `ERR_DROP` that the
  asset-load guard catches longjmps out well before the hook — recovered errors do not
  write rescue files.
- **Why not `Map_SaveFile`.** It touches no renderer state, but it is operator-
  attended: two Perforce/read-only `MessageBoxA` prompts (each a nested message pump
  driven straight back into the renderer we are dying inside), a layered-materials
  save that can abort the whole write, window messages to the frame and status bar,
  and `modified = 0`. The rescue reproduces its write core only — `iwmap 4`,
  `Layers_WriteToFile`, the entity gate and `MapFile_WriteEntity` with the region flag
  hard 0 — which is pure `fprintf` over the brush lists.
- **Guards.** One attempt per process; the flag is set before anything can fault. The
  whole writer sits in `__try/__except`, because a crash inside the rescue that
  replaced the user's original error would make the editor harder to fix, not easier.
  No console writes, no window messages, no `modified` write.
- **The operator is told.** A single message box names the rescue file and says the
  original was not touched. A rescue nobody knows about is the same as no rescue.

---

# Part IX — Round W (2026-08-10): the OUTLINER

## 53. The scene list, and what a "group" is

**USER DIRECTIVE, verbatim:** *"Please create a collapsible giant list of all
brushes on the left like it's Plasticity. Support groups in this list. Show hidden
ones with a closed eyeball like in plasticity. Split them by type (curve vs solid)
just like in plasticity). allow selection by clicking on them in the list and allow
multiple by shift clicking, shift dragging. When a group is created, it has a group
'folder' in the list that you can drag to."*

### 53.1 What Plasticity's outliner actually is

Three files, and the whole design is in them —
`plasticity/src/components/outliner/`:

| source | what it establishes |
|---|---|
| `FlattenOutline.ts:13` | the tree is flattened to a **flat array once per render**; a collapsed group contributes exactly one row (`:54-57`) |
| `FlattenOutline.ts:18,42-49` | children are bucketed into `solids` / `curves` while walking, and each non-empty bucket is emitted behind its own **section row**. The type split is **per group**, not global, and an empty section is not drawn |
| `Outliner.tsx:113,142` | `flatten(root, ...)` then `.map(...)` — one element per entry, rebuilt every render |
| `Outliner.tsx:150-152` | selection is re-asked **per row per render** (`selected.has(object)`). The outliner caches no selection |
| `Outliner.tsx:56` | the collapse state is a `Set` of ids held by the panel, never a flag on the model |
| `Outliner.tsx:89-100` | a selection change **auto-expands** the ancestors of whatever became selected |
| `Outliner.tsx:158` | an unnamed row reads "klass id" — "Solid 12" |
| `Outliner.tsx:196-205` | the header is a title plus **one** button: `command:group-selected` |
| `OutlinerItems.tsx:87` | `name={ !hidden ? 'eye' : 'eye-off' }` — the closed eyeball |
| `OutlinerItems.tsx:77,146,166` | **double-click renames**; blur/Enter commits through a command |

KIWI ports all of that. It deliberately does **not** port the other two per-row
toggles Plasticity has (`OutlinerItems.tsx:89-102`, "disable in viewport" and "lock
selection"): Radiant has exactly one visibility concept — the hide bit the `H`
family writes — and adding two editor-only per-brush states that nothing else in
the editor respects would be three rules where the directive asked for one.

### 53.2 A brush group IS a `func_group` entity

This is the round's one real decision, and it is a decision **not** to invent
anything. A "group of brushes" in a Radiant map is a **brush entity**, and the
classname classic Radiant uses for a purely organisational one is `func_group`.
Choosing it means the group:

- survives the `.map` save with no new format — `map.cpp:693-706` writes every
  entity whose def-list is non-empty;
- is understood downstream (`cm_load_obj.cpp:574`, `cod4map/map.cpp:1437`) and by
  stock Radiant;
- already has creation, reparenting, freeing **and undo** code in this tree.

The alternative — an editor-only group id on the brush, saved in a sidecar — would
have been invisible to the compiler, invisible to stock Radiant, and a second
grouping concept next to entities that already group brushes.

**The exact sequences used**, each one an existing caller's, not a new invention:

```
CREATE     xywnd.cpp:3399-3403 (CreateEntityFromName) + pmesh.cpp:7396's tail
             Undo_ClearRedo(); Undo_GeneralStart("outliner group");
             Undo_AddBrushList( &selected_brushes );
             Entity_Create( Eclass_ForName( 0, "func_group" ) );   // reparents
             Undo_SetIdForEntity( newDef );  SetKeyValue(newDef,"targetname",..)
             Undo_End();

REPARENT   the only spelling of it in the tree — entity.cpp:1697-1699 / :1743-1745
           and select.cpp:5119-5126 are the same three calls in the same order
             Entity_UnlinkBrush( def );                    // out of the def-list
             Entity_LinkBrush( def, targetDef );           // into the def-list
             Entity_LinkBrush_0_extern( targetInst, inst );// the owner chain
           inside the ported envelope: Brush_Deselect_Helper (if selected) ->
           the triple -> Brush_BuildWindings(def,1) -> the sel_vertex/sel_edge
           SetupVertexSelection gate -> MarkMapModified -> ++def->version ->
           Brush_Select_Helper.

UNGROUP    Select_Ungroup's body (select.cpp:5079) reduced to ONE entity: the same
           triple back to worldspawn, then Entity_Free.  NOT a call to
           Select_Ungroup, which walks the SELECTION — the outliner's ungroup acts
           on the folder row that was right-clicked.

RENAME     SetKeyValue( def, "targetname", text ) — entity.cpp:209, the setter
           win_ent.cpp:278 EntSetKey_Apply already drives.
```

### 53.3 Undo — checked in the restore code, not assumed

`Undo_AddBrush` (undo.cpp:494) clones the brush def **and stores
`clone->unk1 = brush->owner->numberId`** (undo.cpp:527) — the owning entity's unique
number. `Undo_Undo`'s phase 4 (undo.cpp:942-968) re-links each restored def into the
entity instance whose def carries that number, falling back to worldspawn.
**Ownership is therefore part of the record**, and a reparent bracketed with one
`Undo_AddBrush` per moved brush — taken *before* the move, so the OLD owner is what
gets stored — undoes completely.

Three consequences the code depends on:

1. the reparent path deliberately does **not** call `Undo_AddEntity` on either
   entity: that stamps the LIVE entity (undo.cpp:620) and phase 2 would then delete
   and re-create an entity the user never touched;
2. the ungroup path **does** call `Undo_AddEntity_W(groupDef)` — phase 3 restores
   entity defs *before* phase 4 relinks brushes, which is the order that makes the
   group come back **with its members** rather than as an empty name;
3. `Entity_Create` has three arms and only one of them allocates. With a non-world
   brush in the selection it **merges** into that brush's existing entity
   (entity.cpp:1669-1710) and returns it, indistinguishably from a create — and it
   can also refuse outright (`:1648`, `:1661`). Stamping a merge return with
   `Undo_SetIdForEntity` would make one `Ctrl+Z` **delete a group the user never
   created**, and a refusal would leave an opened bracket around a no-op. So the
   arm is determined *before* the bracket opens (are all selected brushes
   worldspawn-owned?) and **"Group Selection" is create-only** — adding brushes to
   an existing group is the drag, which names its target unambiguously.

Every verb opens exactly **one** `Undo_GeneralStart`/`Undo_End` bracket, so section
39's journal mints one ticket and one `Ctrl+Z` undoes a whole regroup however many
brushes it moved. The construction half pushes exactly one `KiwiCon_UndoPush`,
which already mints its own ticket (kiwi_construct.cpp:971) — the outliner must
**not** also call `KiwiUndo_NoteConstructionRecord` or the journal double-counts.

### 53.4 Construction groups are one int plus two sidecar keywords

Construction geometry is editor-only and never reaches the `.map`, so its groups
cannot be entities. `kconObject_t` gains one `int group` (-1 = ungrouped) and the
store gains a `{id, name}` table. The sidecar gains:

```
congroup 3 "handrail"      <- top level, one per declared group
object polyline
group 3                    <- inside an object block
...
end
```

**No version bump**, on exactly the argument ROUND U's `hidden` made: the reader
skips every keyword it does not know (kiwi_construct.cpp's load-loop tail) and
treats an unknown top-level line as noise, and this build defaults `group` to -1
when the line is absent — which is what every KIWI2 file written before ROUND W is.
Ids are **handles** from a monotonic counter, never indices, so removing a group
cannot silently rename another one in a sidecar already on disk; the loader keeps
`s_nextGroupId` above anything a file declared.

### 53.5 The panel

- `kiwi_outliner.h/.cpp`, ImGui window **"Outliner"**, registered in the section-9
  table (`KIWI_WIN_OUTLINER`, default **ON**, `KW_VERSION` 2 -> 3 so existing
  profiles get it once) and docked into a **new left column** (~0.18 of the root) by
  the first-run layout. Dock ini bumped `kiwi_dock5.ini` -> `kiwi_dock6.ini`, same
  trap as every previous layout change.
- **Rows** are `[eye] [indent] [arrow] [name]`, all the same height, built into one
  flat array per frame and drawn through an `ImGuiListClipper` — a 20k-brush map
  submits ~40 rows. Section headers are ordinary rows with an arrow glyph rather
  than `CollapsingHeader`, precisely so the height stays uniform.
  The clipper is told `GetTextLineHeightWithSpacing()` while the items are
  `GetTextLineHeight()` tall: ImGui adds `ItemSpacing.y` after each line, and a
  clipper told the item height drifts by one spacing per row.
- **The eye** writes the SAME two fields the ported hide family writes —
  `brushFlags & 4` and the `xx5` depth (select.cpp:4179-4180 / :4249-4250) — so a
  brush hidden from the list is hidden to `H`, to `Ctrl+H` and to "Show Hidden".
  There is one hide state, not two. Construction rows write `kconObject_t::hidden`.
  A folder's eye reads "is *every* child hidden" and writes the opposite to all.
- **Selection is re-asked per row per frame** and never cached, so a viewport
  selection lights rows up with no notification path at all. Clicks leave through
  the funnels the viewport uses (`Sel_*` + `Sel_SyncToLegacy`,
  `KiwiConSel_ApplyClick`). Plain click replaces, `Ctrl` toggles, `Shift` ranges
  from the last clicked row, `Shift`+drag paints. The range **anchor is stored as
  an identity, not a row index** — the flatten changes shape whenever anything is
  expanded or created.
- **Drag to group.** The payload is a descriptor, never a bare pointer: a brush is
  re-validated with `Sel_BrushLive` before any deref, a construction object with
  the store **generation** (indices are only meaningful within one). Dropping on a
  folder moves; dropping on the `Solids` / `Curves` section header **un**groups.
  Dragging a row that is part of the current selection moves the whole selection.
  `Shift`+drag is paint-select and is *exclusive* with the drag source — one
  gesture, one meaning.
- **`s_structural`**: any row action that frees an entity or reorders the store
  stops the draw loop for that frame. The row array was flattened at the top of the
  frame, and continuing would read rows naming things that no longer exist. One
  frame's stall against a dangling deref inside an ImGui loop.
- **Commands.** `KIWI_CMD_WINDOW_OUTLINER` (34116) in the Windows menu and the
  palette; `KIWI_CMD_GROUP_CREATE` (34117) / `KIWI_CMD_GROUP_UNGROUP` (34118) as
  "Group Selection" / "Ungroup Selection", both **unbound** by default on the same
  argument the window toggles make — the letter budget is spent on modelling verbs.

### 53.6 Deviations, and the risks that are known

- **An empty `func_group` is session-only.** `map.cpp:700` writes an entity only
  when its def-list is non-empty, so a folder you emptied by dragging everything
  out survives until the next save and no further. Deliberate: the alternative is
  writing brushless entities into the `.map`, which changes what the compiler sees
  for a purely cosmetic editor concern. Empty folders stay alive **in session**
  because an empty folder is still a legal drag target.
- **A construction group RENAME is not undoable.** The name table is not part of
  the whole-store snapshot `KiwiCon_UndoPush` takes; group *membership* is. Same
  class of decision as section 39's "no cross-domain compound records".
- **Groups are one level.** Plasticity nests; this does not. `func_group` entities
  cannot contain each other in a `.map` anyway, so a nested solid group would have
  been a lie the moment it was saved.
- **A brush's row number is its position in its owner's chain**, which is
  head-insertion order, so it renumbers when a sibling is added. Brushes have no
  identity in the map format to number them by (`brush_t::numberId` is a physics
  kind, brush.cpp:3524 — not an id), and Plasticity itself numbers by `lookupId`.
- **Patches live under Solids and say so.** The directive's "curve vs solid" maps
  to *construction geometry vs map brushes*, not to bezier patches: a patch is map
  geometry the compiler emits. Patch rows read "Patch N" rather than "Brush N" so
  the list does not lie about what they are.
- **RISK — a very large map's first paint.** The flatten walks every entity and
  every brush once per frame. It touches no windings, faces or epairs (only a
  group's `targetname`), so it is a pointer walk, but it is not clipped. If a
  200k-brush map ever shows up in a profile, the fix is to cache the flatten on
  `Sel_Generation` + `KiwiCon_Generation` + a map-modified counter — not to make
  the rows lazier.

## 54. Round X — the shakeout batch: layout, grazing angles, and the z-order

Twelve user reports, one round. Everything below is implemented; the limits and
the one item that could not be reproduced from source are in
`RADIANT_KNOWN_ISSUES.md` under **Round X**.

### 54.1 The default layout: the console spans the whole bottom

USER DIRECTIVE: *"the default layout should look like this: (see pic, console takes
up whole bottom)"*. Outliner left, camera centre, 2D View over Textures on the
right, and the console as one uninterrupted strip under all three.

**The whole fix is SPLIT ORDER**, and it is the reverse of shakeout I's. A node
split off the dockspace ROOT is full-extent in the perpendicular axis; anything
split later can only eat the node it was given. So `ImGuiShell_BuildDefaultDockLayout`
now takes the console off the root FIRST (0.20 down, full width), then the left
column (0.18), then the right column (0.25), and the camera is whatever is left.
Taking the columns first is exactly what made the console narrow before.

The dock ini goes `kiwi_dock6.ini` -> `kiwi_dock7.ini` and `KW_VERSION` 3 -> 4, the
same paired bump every default-layout change has made: the ini pins the old layout
forever and the window flags have to be re-seeded with it or the new layout places
windows that are not open.

### 54.2 The construction plane is FINITE (the shallow-angle fix)

USER DIRECTIVE: *"when at a relatively shallow angle, its impossible to snap/guide
the line onto the xy plane as seen in the pic. Fix this. I would just like
identical plasticity behavior."*

**Plasticity's construction plane is not a plane, it is a 10000x10000 quad.**
`PlaneSnap.geometry = new THREE.PlaneGeometry(10000, 10000, 2, 2)` with
`snapper = new THREE.Mesh(...)` (`plasticity/src/editor/snaps/PlaneSnap.ts:12-14`);
the picker RAYCASTS that mesh
(`plasticity/src/editor/snaps/PointPickerSnapPickerStrategy.ts:25-28`) and only then
orthogonally projects the hit onto the analytic plane (`PlaneSnap.ts:87-93`). There
is **no near-parallel guard anywhere in their picking path** — I looked for one; the
finite quad *is* the guard. A grazing ray runs off the quad, `intersections.length
=== 0`, and the construction plane stops being a candidate.

KIWI's `KiwiCon_RayPlane` was the infinite form with a `t > 1e6` sanity reject, so:

* a ray half a degree off the plane resolved a hundred thousand units away — the
  placed point left the drawn grid entirely, which is the picture attached to the
  directive; and
* a ray pointing infinitesimally the *wrong* side returned false outright, which
  dropped snap arms 7+8 and let the drawing tool place OFF its own plane.

Both are "impossible to snap the line onto the xy plane". `KiwiCon_RayPlaneBounded`
(new; `kiwi_construct.h`) is the finite form: a forward hit is clamped into the
+/-`KCON_PLANE_REACH` square in the plane's own (u,v) — that square IS their quad, in
plane space, centred on the plane origin as theirs is — and a ray with **no** forward
hit slides from its own origin's plane projection along its in-plane bearing out to
the same square. Plasticity drops the candidate at that point; KIWI pins it at the
quad edge instead, because a drawing tool with no plane point has nowhere to put the
click at all. `KCON_PLANE_REACH` is 16384 — one eighth of the engine's own +/-131072
bound (`brush.cpp:1459`), the same gesture in CoD inches that 5000 is in their tree.

`KiwiCon_RayPlane` itself is UNCHANGED and stays the hit-TEST form: `kiwi_region.cpp`'s
point-in-cell arm needs a hit that is genuinely where the ray met the plane. The snap
query and all five drawing tools use the bounded one. The ground-plane fallback
(`kiwi_snap.cpp RayHitsGroundPlane`) gets the same window, centred on the camera's
ground point because that is where `kiwi_grid.cpp` centres the lattice it draws.

**The axis guide was already right and is now easier to hit.** `ScanAxes` computes
the closest point on the guide in SCREEN space and maps the parameter back to the
world segment — line-vs-line, never ray-vs-plane — so it is stable at any camera
angle, and arm 5 (LINE) already outranks arms 7+8 (the plane), which is Plasticity's
own ranking (`AxisSnap.prototype.priority = 2` vs
`ConstructionPlaneSnap.prototype.priority = 5`, `SnapPicker.ts:136-161`; and when the
cplane is the default floor it is a *fallback* appended after sorting and can never
outrank anything at all, `PointPickerSnapPicker.ts:70-88`). What changed is the
capture radius: round P widened only the VERTICAL guide as the camera flattened, and
the widening now applies to EVERY candidate with its flat end at **30 px** —
Plasticity's own line threshold (`Line2: { threshold: 30 }`, `SnapPicker.ts:28-32`
and `PointPicker.ts:138-142`; `AxisSnap`'s snapper is a `Line2`, `AxisSnap.ts:25`).
The steep end keeps KIWI's tighter 10 px, where a 30 px axis would steal clicks from
geometry.

### 54.3 The grid ladder has no ceiling

USER DIRECTIVE: *"why does the inch snapping only go to 32? Weird! You should have
this all the way imo, fine control can be done with ctrl."*

`KCMD_GRID_LADDER` continues past 1024 in the same mixed style (2000, 2048, 4096,
5000, 8192, 10000, 16384, 32768, 65536) and `KGRID_MAX_INCHES` moves 1024 -> 65536,
half the engine world bound. The lower end is untouched — the directive names only
the top, and "fine control can be done with ctrl" is a statement that the bottom is
already covered (Ctrl suppresses snapping outright, §6). **The observed stop at 32
could not be reproduced from source** — see KNOWN_ISSUES.

### 54.4 An extrusion does not snap to itself, and its preview is a solid

USER DIRECTIVE: *"When extruding, dont allow snapping to self, it's just an
annoyance fix. The Preview is still not 3d either. It's just lines from each vertex
going up."*

**(a) The self-snap.** An extrusion is a one-axis gesture measured from a start
plane, and everything it is made OF lies on that plane: the source face and its
corners, the source region's boundary lines and their endpoints, every midpoint of
both. Those are the candidates nearest the cursor for the whole first part of the
drag and every one of them resolves to a scalar of **zero**. The depth was glued to
"no extrusion at all" exactly where the user was trying to leave it.

The rule is about the SOURCE PLANE, not about a list of objects: a geometry snap
whose projection onto the extrusion axis lands within `KEXT_SELF_SNAP_BAND` (2 units)
of the start is refused and the gesture keeps its cursor-mapped distance. It needs no
exclusion set, it cannot go stale mid-gesture, and it catches CONSTRUCTION geometry —
which the pick layer's `PICKF_EXCLUDE_SELECTED` cannot see at all — on the same terms
as brush geometry. Nothing legal is lost: the commands already reject
`|d| < KEXT_MIN_DIST` as a no-op. One band, applied at all three one-axis gestures
(region extrude, face extrude, face push) so they cannot drift apart.

**(b) The preview.** It really was "lines from each vertex going up": the top ring
plus one vertical per profile vertex, and no bottom ring and no faces at all. It now
draws the SIDE QUADS and the MOVING CAP filled and translucent (`EmitPrismSolid`),
with the full wireframe — bottom ring included — over the top. The fill uses the
`R_AddRenderCmdDrawTris` + neutral-`MATERIAL_COLOR` idiom `kiwi_region.cpp`'s region
fills and `kiwi_split.cpp`'s cut quad already use, because `kiwi_lines` pins alpha to
1 and cannot express a translucent surface (kiwi_lines.h TRAP 2). The cap is drawn a
shade stronger than the sides so the face the drag is actually moving is the one the
eye lands on. The modal overlay budget goes 192 -> 288: three segments per profile
vertex at the 64-vertex cap is exactly 192, i.e. the old budget truncated the largest
legal preview at its own last vertex.

### 54.5 A SELECTED brush is invisible to `Test_Ray` — the "two clicks" bug

USER DIRECTIVES: *"If a brush is selected and while its selected, I switch to mode-3
and select a face, it takes 2 clicks. Fix this so it just works."* and *"Sometimes
when clicking, an object isn't selected and it takes 2 tries. No clue why. This also
happens with faces."*

**One cause, and it is not click slop.** `sub_48D460` — the ported brush-list walker
`Test_Ray` runs over BOTH lists — skips outright:

```cpp
int brushflags = sbn->brushFlags;                 // select.cpp:675
if ( ( brushflags & BRUSHFLAG_SELECTED ) != 0 )   // select.cpp:677
    continue;
```

and `Brush_Select_Helper` (`brush.cpp:844-846`, called by `Brush_AddToList2` *before*
it links the node into `selected_brushes`) sets exactly that bit. The selected list is
walked and then every member of it is thrown away, so the area pass cannot hit
anything that is already selected. Click 1 finds nothing and the miss arm clears the
selection; click 2 hits it because it is no longer selected. In face mode that is the
first report exactly; in object mode it is the second.

The walker's skip is ported behaviour and stays. What round X changes is the STATE it
is asked to walk: `kiwi_pick.cpp`'s `selUnmask_t` lifts the bit for the duration of the
one `Test_Ray` call and puts it straight back. Synchronous, nothing that can throw
between the halves, no drawing or message pump inside, and the selection COUNTERS are
untouched (the bit is flipped directly, never through `Brush_Select_Helper` /
`Brush_Deselect_Helper`, which are what own `d_select_count`). It is **not** applied
under `PICKF_EXCLUDE_SELECTED`, where unmasking would only produce a nearest hit the
exclude test then rejects — hiding the surface behind it.

Keeping an already-selected object selectable is also Plasticity's behaviour and every
other editor's: a plain click makes it the selection, Ctrl is what removes it.

**The slop half.** `KiwiBox_End` has arbitrated click-vs-drag at release since Phase 2
(`KBOX_CLICK_PIXELS`, 8 px), so the click was never actually lost to hand-wobble —
but the marquee RECTANGLE appeared on the first pixel of travel, so the gesture *looked*
like a box-select while resolving as a click. Two different stories about one gesture
is how a user comes to believe the click went missing. The marquee is now suppressed
below the same threshold that decides the outcome. Plasticity draws the line in the
same place and calls it consummating the drag —
`consummationDistanceThreshold = 4` px in clientX/clientY
(`plasticity/src/components/viewport/ViewportControl.ts:281-284`, tested in
`onPointerMove`'s `'down'` state at `:149-165`, with box-select reached only afterwards,
`ViewportSelector.ts:43-48`). KIWI keeps its own 8 rather than adopting their 4: one
number already decides the outcome, a second would be a second answer to one question,
and 8 is the more forgiving of the two — which is the direction the report asks for.

### 54.6 The z-order: the pointer gate was the bug, not the fix

USER DIRECTIVES: *"The Z ordering of surfaces after doing an extrusion is still wrong.
Need to fix this. This is a really big bug."* + *"The Z order is only messed up when
looking from 1 direction."*

Direction-dependent occlusion is the signature of a face drawn with **no depth write**
— painter's order decides, so one camera side happens to agree with depth and the
other does not. Round O found that mechanism and fixed half of it.

Round O's own safety argument is the hole. Its comment reads: *"A world material that
really loaded can never collide with it: it registers through the `wc/` prefix and so
resolves techset `wc_2d`, a DIFFERENT `MaterialTechniqueSet` object."* That is true,
and it is a description of the defect, because **the editor's current-material template
is `$default`**: `Radiant_SeedCurrentTexdefs` seeds `SetMaterial("$default", &rts[0].mtl)`
at boot (`mainfrm.cpp:701`) and nothing changes it until the user clicks a texture
thumbnail. That name registers through `Register_WorldMaterial` (`texwnd.cpp:239-242`)
as `wc/$default`, and `materials/$default` EXISTS — so this is a **successful load**,
not a `Material_MakeDefault` clone. `Material_Load` then builds the techset name with
the type prefix (`Com_sprintf(techniqueSetName, "%s%s", techniqueSetVertDeclPrefix, ...)`,
`r_material_load_obj.cpp:5514`; prefix `"wc_"` from `g_materialTypeInfo[4]`, `:5266-5273`),
so the techset is `"wc_2d"` while `rgp.defaultMaterial`'s — registered unprefixed by
name `"$default"`, `r_material.cpp:219` — is `"2d"`. Two different interned objects.
`Cam_MaterialIsMissing`'s pointer test returned false on the line ABOVE the depth test,
and the depth test never ran.

The face therefore kept `$default`'s real state — techset `2d` -> `vertcol_simple2d` ->
statemap `default2d`, depthTest Disable **and** depthWrite Disable — and the newest
submission wins unconditionally. Extrusion makes it obvious because `BuildPieceDef`
stamps that template onto a fresh brush (`kiwi_extrude.cpp:275` -> `Ed_BrushSetFaceCount`
-> `brush.cpp:7641` `memcpy` from `random_texture_stuff[0].mtl`) and `LandDef`
tail-inserts it, but **it is not an extrusion bug** — the user's own round-O report of
checkerboard boxes drawing through each other "even without operations" was the same
defect seen from the other end.

**The fix is in the detection predicate**: compare the techset by NAME with the
material-type prefix stripped, so `wc_2d` and `2d` are recognised as the same techset —
which they are; the prefix only selects a vertex-declaration variant. The pointer test
is kept as the fast path (still exactly right for the clone case round O was built for)
and the depth-write gate is UNCHANGED and still ANDed in, so the widened first test
cannot drag in a material that legitimately writes depth. **No global depth-state
forcing**, and no change to any statemap: the substitution mechanism is the shipped one
and only its gate moved.

**Deliberately NOT done:** reseeding `Radiant_SeedCurrentTexdefs` with `"$default3d"`
(which is what the engine's own BSP loader does, `r_material_load_obj.cpp:4704`). That
would change what is WRITTEN INTO `.map` files, and map serialization is on the
IDA-faithful side of the line. Fixing the draw gate fixes every existing map too, which
reseeding would not.

### 54.7 The cut disc

USER DIRECTIVE: *"The cut previewer is pretty good, but I would like you to make it a
semi-transparent circle about 250% bigger than the area we're cutting."*

The previewer the directive is looking at is the snap marker's dot-and-ring
(`kiwi_snap.cpp EmitDotAndRing` — `KSNAP_DOT_PIX` 2 px filled, `KSNAP_RING_PIX` 6 px
outline), which marks the point a click would cut at. 6 px is "the area we're cutting"
and 250% of it is 15 px. **The dot and the ring both stay** — they are the precise mark
and the directive calls the existing previewer good; the disc is added UNDER them as
the area readout, in the cut tool's hover accent at the sweep quad's own alpha, so the
two previews read as one language. It is oriented on the surface being cut when there
is one (a FACE hover has a plane) and faces the camera otherwise, because a line and an
edge have no surface to lie on and a disc edge-on to the view is invisible. Centred on
the live SNAP point rather than the raw pick, because that IS where the click will cut
and it is where the dot-and-ring already is — two markers for one act must not be able
to disagree about where the act is.

### 54.8 Hidden means hidden, and rename means every row that has somewhere to put one

USER DIRECTIVES: *"When hiding objects, curves are not hidden even though they are
marked as hidden. Fix this."* and *"Move curves into their own section in the
'outliner'. Allow renaming of items in the outliner by double clicking the item and
typing."*

**The hidden bug was a missing REPAINT, not a missing skip.** `KiwiCon_DrawWorld` has
skipped hidden objects since round U and still does. What was missing is the request to
redraw: the shell paints no scene unless something asks (the white-flicker guard), so a
store change that bumped only the generation left the last-rendered image — with the
curve still in it — on screen. The outliner's eye is exactly that path:
`SetBrushHidden` ends in `g_nUpdateBits = -1`, so hiding a BRUSH from the panel
repainted and hiding a CURVE did not. `KiwiCon_SetHidden` / `KiwiCon_UnhideAll` now do
too, at the store rather than at the call sites so there is one owner. The audit that
went with it found three PICK paths that had never honoured the flag — the cut tool's
construction-line pick, the trim tool's hover and its `CanTrim` predicate — plus
`KiwiArrange_Cells`, which is defensive (its caller pre-filters). All four now skip
hidden: "hidden = it is not there" is one rule.

**Curves already always land under Curves**, including grouped ones: `Flatten` walks
`KiwiCon_GroupCount()` inside the Curves section and nothing else can push a
construction row anywhere else. Verified, no mixing, no change.

**Rename.** Round W shipped it for FOLDERS only, keyed on the folder's collapse key
(which every leaf row leaves at 0). It now covers every row that has somewhere to put a
name: entity folders and entity rows (`targetname`), construction group folders (the
store's name table) and **construction objects** — which gain a `name` on
`kconObject_t`, persisted as an optional quoted `name "..."` line in the sidecar under
the same no-version-bump deal `hidden` and `group` already make (an older build's reader
skips keywords it does not know; this build defaults to empty when the line is absent).
A named object shows its name and an unnamed one falls back to the generated
`<type> <ordinal>`, which is Plasticity's own fallback shape (`${klass} ${id}`,
`Outliner.tsx:158`). The rename is one store snapshot, so it is undoable exactly like a
hide.

Interaction: double-click -> inline field seeded with the current name, Enter or blur
commits, **Esc cancels**. Plasticity has no cancel at all — its only key handler is
`e.code === "Enter"` and blur commits (`OutlinerItems.tsx:153-167`), so clicking away
saves an edit you were abandoning. That is a gap in theirs, not a rule to copy: every
other text field in this editor cancels on Esc.

**Worldspawn brushes are NOT renameable, and that is a storage fact.** See the decision
entry below and KNOWN_ISSUES.

### 54.9 Parking a gesture freezes it

USER DIRECTIVE: *"Sometimes when extruding and releasing the mouse, it snaps back to
the starting value for no reason. Seems related to snapping."*

It was. `KiwiCmd_MouseButton`'s pause arm used to run a full `KiwiCmd_MouseMove` — a
fresh pick AND a fresh snap query — on the very event that parks the gesture, and then
pause. A command with no grab gate (the region extrude is the plain case: its
`Recompute` maps the cursor unconditionally) therefore took one more snap answer AFTER
the user had stopped aiming. The snap ranking is a closest-candidate-in-pixels contest
and the source geometry of an extrusion is always on screen right where the gesture
started, so the extra query was the one most likely to land back on the source — i.e.
"it snaps back to the starting value".

Parking is not an edit. The move is now made only by the `WantsClicks` branch, which
genuinely needs it (the click PLACES A POINT). §54.4's self-snap band removes the
attractor as well; this removes the extra question.

### 54.10 Decision log — round X

- **D-X1 — the finite plane pins at the quad edge; Plasticity drops the candidate.**
  Their point-picker can produce no point at all and simply leaves the last one where
  it was; KIWI's drawing tools have no such state and a click with no plane point has
  nowhere to land. Pinning is stateless and is what the user saw last before their quad
  ran out. (§54.2)
- **D-X2 — `KCON_PLANE_REACH` is 16384, not Plasticity's 5000.** Their unit is roughly
  a metre; CoD's is an inch. 16384 is one eighth of the engine's own world bound and
  comfortably outside any CoD4 playable area, so it can only bite on a hit that was
  already absurd.
- **D-X3 — the self-snap rule is a PLANE band, not an exclusion set.** An exclusion
  set would have to be built per gesture, kept in step with the store generation, and
  extended to construction geometry that `PICKF_EXCLUDE_SELECTED` cannot address at
  all. One scalar test against the start plane is the same rule in one line and cannot
  go stale. (§54.4)
- **D-X4 — the z-order fix goes in the DETECTION predicate, not at the source.**
  Reseeding the current-material template with `$default3d` would fix new geometry and
  change `.map` output; fixing the gate fixes every existing map and changes nothing on
  disk. (§54.6)
- **D-X5 — worldspawn brushes are not renameable, and an ordinal-keyed name is
  refused.** A brush is not an entity (no epair), is not in the sidecar (no editor-side
  record), and the `.map` format has no per-brush id. The obvious workaround — key names
  by the brush's position in the worldspawn chain — is refused because that ordinal is
  reordered by CSG, clone, delete, undo and by the load order of the `.map` itself, so
  the name would silently move to a DIFFERENT brush. A name that lies is worse than no
  name. The row says so in the console and points at the group, which *can* be named.
  (§54.8)
- **D-X6 — Esc cancels a rename, deviating from Plasticity.** Theirs commits on blur
  and has no Escape handler at all, so an abandoned edit is saved. Every other text
  field in KIWI cancels on Esc and this one is not going to be the exception. (§54.8)
- **D-X7 — the marquee's visual threshold reuses `KBOX_CLICK_PIXELS` (8) rather than
  adopting Plasticity's 4.** One number already decides the outcome at release; a
  second would be a second answer to one question, and 8 is the more forgiving of the
  two. (§54.5)

# Part X — Round Y (2026-08-10): the material that could not write depth

## 55. Round Y — eight reports, and the third go at the z-order

Eight user reports on top of round X's build (commit 1a667ad). Everything below
is implemented; the limits, the accepted costs and the one deliberate grammar
change are in `RADIANT_KNOWN_ISSUES.md` under **Round Y**.

### 55.1 THE Z-ORDER BUG — the template material was never `$default`

USER REPORTS, verbatim: *"z-order bug is still here.  All I did was split a cube
in half.  All extrusions get this on top drawing bug too. (Maybe material
related??)"*, *"Z order bug here is the biggest priority. It's awful"*, and —
mid-round, which closed it — *"yeah it looks fine when the material is changed to
something else."*

**Round X's premise was false, and it was false about the BOOT.**  Its comment
reads: *"the editor's own current-material template is `$default` …
`Radiant_SeedCurrentTexdefs` seeds `SetMaterial("$default", &rts[0].mtl)` at boot
and nothing changes it until the user clicks a thumbnail."*  Something does change
it, three steps later in the same boot:

```
Radiant_ApplyStartupTextureScale   (mainfrm.cpp:763, the OnCreate tail)
  -> Radiant_CheckTextureScale     (mainfrm.cpp:2914)
     -> Texture_ResetPosition      (texwnd.cpp:2017)
        -> TexWnd_ApplyMaterialAtIndex( TexWnd_HitTest( 9, 9 ) )   (texwnd.cpp:2028-2033)
```

`Texture_ResetPosition`'s own header comment says what it does — *"making the
first visible material the current brush texture"* (texwnd.cpp:2006-2008) — and it
is IDA-faithful.  The browser lists alphabetically, so on the user's asset set the
editor booted with the template set to the first file in `main/materials`:
**`aa_default`**.

**THE ASSET DECODE** (`main/materials/aa_default`, `MaterialRaw`, r_material.h:589):

| field | value | meaning |
|---|---|---|
| `+0x00` nameOffset | `"aa_default"` | |
| `+0x09` sortKey | `0x2B` (43) | the translucent/tool sort — byte-identical to clip, trigger, origin, white_tools |
| `+0x28` refStateBits[0] | `0x08128965` | srcBlend SrcAlpha, dstBlend InvSrcAlpha, blendOp **Add**, cull BACK |
| `+0x2C` refStateBits[1] | `0x0000000C` | `GFXS1_DEPTHTEST_LESSEQUAL`, `GFXS1_DEPTHWRITE` **clear** |
| `+0x34` techSetNameOffset | `"tools"` | **not** `"2d"` |

`main/techsets/tools.techset` maps `"unlit"` to `vertcol_shaded_tools`;
`main/techniques/vertcol_shaded_tools.tech` declares `stateMap "default"`; and
`main/statemaps/default.sm` reads

```
depthWrite { mtlBlendOp == Disable: Enable;  default: Disable; }
```

`aa_default`'s blendOp is Add, so the bound state is **depth test ON, depth write
OFF**.  That is precisely the direction-dependent signature: the brush drawn FIRST
stamps no depth, so the brush drawn SECOND passes the test wherever they overlap
and wins — correct-looking from the side where the newer brush really is in front,
wrong from the other.  A split makes two brushes and an extrusion tail-inserts
one, which is why both reproduce it every time, and why re-texturing the same
brushes fixes them.

**Why it slipped round X's gate.**  Round X widened `Cam_MaterialIsMissing` from a
techset POINTER compare to a techset BASE-NAME compare against
`rgp.defaultMaterial`'s, which is `"2d"`.  `aa_default`'s base name is `"tools"`.
The gate was widened along the wrong axis: it generalised over the
vertex-declaration prefix (`wc_2d` vs `2d`) when the thing that varies is the
techset itself.

**THE PREDICATE, moved onto an axis that generalises.**  A face is substituted when

> the technique the camera binds does not write depth, **AND** either
> (A) the material's techset base name is an EDITOR/HUD techset (`2d` or `tools`),
> **or** (B) the material declares itself fully opaque (`sortKey <= 4`).

Both arms are measured in the shipped assets, not assumed.  A census of all 4355
files in `main/materials`:

* techsets `2d` (862) and `tools` (98) are used by HUD art and editor tool
  materials **only**; every world surface class is on `l_sm_*` / `unlit*` /
  `effect*` / `sky` / `water` / `ambient_*` / `distortion_*`.  Arm A therefore
  cannot reach a legitimately translucent world material.
* arm B's threshold is **4, not `SORTKEY_DECAL` (24)**.  The obvious reading —
  "opaque sorts below decal" — was checked and is WRONG: the shipped world DECALS
  sit at sortKeys 9..12, 434 of them, alpha-blended and correctly depth-write-free
  (`ch_decal_mural` sk 12, `ch_cliff02_decal` sk 9, every `*_dec` / `*_decal`), so
  a `< 24` gate would substitute a checkerboard for every decal in the game.  The
  real opaque world sort is the single value 4, which the engine itself asserts by
  special-casing exactly `material->info.sortKey == 4 && R_IsWorldMaterialType(...)`
  (r_material_load_obj.cpp:5458) and which the census confirms (2131 of the
  non-editor materials).  Arm B's blast radius on this asset set is **13**
  materials — particle_cloud, distortion_scale_zfeather, glow/grain overlays — all
  FX, none a brush-face material anyone applies from the browser.

`g_qeglobals.d_white` is excluded by pointer at the top: `white_tools` is itself a
depth-write-free `tools` material and it is the editor's deliberate flat-colour
handle for the see-through tool volumes (round M's arm).  Substituting for it
would turn every clip and trigger volume into an opaque box.

**The substitute is a LADDER now, not one name.**  Round X's note conceded that
*"`$default3d` must exist in the asset set or nothing improves"* and left the
no-substitute case silently drawing the broken material.  There is no render-state
override on this path — state comes only from material x technique, and every
technique in the `tools` techset routes through the same statemap, so forcing the
depth bit is not on the table either.  What is on the table is a second shipped
material that is opaque and depth-writing.  `$default3d` (refStateBits[1] `0x0D`,
techset `default`, same checkerboard image) then **`caulk`** (blendOp Disable,
refStateBits[1] `0x0D`), each validated with the same predicate, and a console
warning naming which rung was taken.  `caulk` ships in every CoD4 asset set by
construction, so a world face now always ends up depth-writing.

**AND THE TEMPLATE IS PUT BACK.**  The draw-time gate fixes every existing map;
it does not stop new geometry acquiring a tool material and **saving it into the
`.map`**.  `Radiant_ApplyStartupTextureScale` now restores step 6a-pre's own
`SetMaterial("$default", &rts[0].mtl)` + `Init_MaterialLayer` after the scale pass
has clobbered it.  That is a RESTORATION, not a choice: the name and the sample
size are the seeded ones, so `.map` output is exactly what the un-clobbered boot
produced and **D-X4 is untouched** — nothing changes what gets written to disk
relative to the intended boot state.  `Texture_ResetPosition` itself is not
touched: it is IDA-faithful and it is the right behaviour when the user changes
the texture scale mid-session on a browser they are looking at.  Only the BOOT
call picks a material nobody asked for.

**A PERMANENT DIAGNOSTIC, so there is no fourth round of guessing.**
`KiwiMatInfo` (`KIWI_CMD_MATINFO`, palette-only, unbound) prints for the face
under the cursor AND for the current template: material name, techniqueSet name
and prefix-stripped base name, sortKey, the technique the camera actually binds,
that technique's decoded depthTest/depthWrite, the raw `loadBits` pair, and
whether the camera substitutes (and with what).  The decode lives in camwnd.cpp
(`KiwiMtl_Diagnose`) because that file owns the predicate; kiwi_material.cpp only
picks the subjects and formats.  Rule 12 says material state truth comes only from
the shipped statemaps — this is the readout that makes that checkable from a bug
report.

### 55.2 The outliner is skinnier

USER DIRECTIVE: *"Make the outliner window skinnier by default."*  The left split
goes 0.18 -> 0.125, and the paired bump every default-layout change makes goes
with it: `kiwi_dock7.ini` -> `kiwi_dock8.ini`, `KW_VERSION` 4 -> 5.  Without both,
the existing ini pins the old layout forever.

### 55.3 The split tool marks — and snaps to — the face centre

USER DIRECTIVE: *"when using the split tool, show the center dot so I can find it
easier."*

The face centroid has been the live split's starting position since round S
(`Derive()`'s `t = m_centreT` fallback) and it is the one offset a modeller asks
for by name.  Nothing drew it and nothing pulled the cursor to it.

`KiwiSnap_DrawFaceAccents` could not be the answer: it gates on
`cmd->WantsClicks()` and the live split is a DRAG tool, so it shows no accents at
all — and relaxing that gate would put forty dots on every face during every drag
gesture in the editor.  So kiwi_snap.cpp publishes `KiwiSnap_EmitSpot` (one
glyph, into the caller's already-open batch) plus the two pixel sizes, and the
split draws the dot-and-ring POINT glyph at the centroid for the whole gesture.

It also **latches**: the cursor-mapped offset snaps to the centroid within
`KSPLIT_CENTRE_SNAP_PIX` (8 px, the editor's own click slop) — measured in screen
pixels so it feels the same at every zoom, applied AFTER the geometry-snap arm so
a target the user aimed at still wins, and suppressed by Ctrl on the same
`SNAP_NONE` signal §6 already uses.  A marked target the cursor slides past is
worse than no mark.  The hint strip says so: `Dot — Face centre - snaps for an
exact half`.

### 55.4 An AXIS VIEW owns the construction plane

USER DIRECTIVE, verbatim: *"the line tool is still way out of whack.  IT doesn't
respect the camera angle.  When snapping to top or bottom it is IMPOSSIBLE for me
to represent a Z direction.  Take that into account and apply it on the other
views too (see how plasticity does it)."*

**Round T already derived a plane from the camera — as rung FOUR.**  So the
failure is not "the camera is ignored", it is "the camera loses".  Snap to FRONT,
leave the cursor over the floor or keep a floor face selected, start the line
tool, and rung 2 or rung 3 hands back XY: the one plane in which Z cannot be drawn
at all, from the one camera angle where the user is unambiguously asking for XZ.
Same trap on TOP/BOTTOM, which is what the directive names.

**PLASTICITY DOES NOT ARBITRATE THIS, IT DECIDES IT.**  Navigating to an axis view
sets the camera AND the construction plane in one act — `this.constructionPlane =
to.cplane; this.transitionToOrthoMode(...)`
(`plasticity/src/components/viewport/Viewport.tsx:558-566`) — with the six presets
DEFINED as cplane targets (`'viewport:navigate:top': () => this._navigate(
this.cplanes.constructionPlaneForOrientation(Orientation.posZ))`,
`Viewport.tsx:150-156`; orientation-to-plane table `case Orientation.posZ: return
PlaneDatabase.XY` at
`plasticity/src/components/viewport/ConstructionPlaneGenerator.ts:69-78`).  A view
cube face click enters the same funnel (`viewport.navigate(object.userData.type)`,
`plasticity/src/components/viewport/ViewportNavigator.ts:136` ->
`Viewport.tsx:551-556`).  Ortho mode then makes the plane authoritative — `else if
(this._restriction === undefined && isOrtho) return baseConstructionPlane;`
(`plasticity/src/command/point-picker/PointPickerModel.ts:61-62`) — and drops FACE
snaps from the candidate set outright
(`plasticity/src/editor/snaps/SnapPickerStrategy.ts:95-109`, the `:98` filter),
re-orienting every surviving snap into the plane's basis (`const
effectiveOrientation = isOrthoMode ? constructionPlaneOrientation : orientation;`,
`SnapPickerStrategy.ts:82`).  It ends on the first orbit that moves the camera
(`if (Math.abs(dot - 1) > 10e-6) this.transitionFromOrthoMode();`,
`Viewport.tsx:411-417`).

**KIWI gets a RUNG 0 and no latch.**  `KiwiViewCube_ViewAxis` answers "is the
camera on one of the six axis views" live, from one dot product against
`KVC_VIEWS` at `KVC_VIEW_ALIGNED` (cos 3 degrees — the file's own threshold,
already wide enough for the +/-89 pole clamp).  When it is,
`KiwiCon_AutoPlaneForTool` takes the view plane FIRST and the two face rungs never
run.  Off an axis view the ladder is round T's, in round T's order, unchanged.

Plasticity latches on entry and un-latches on the first orbit, which is
definitionally "the camera is still exactly on the axis it was navigated to" — a
fact this editor can read off the live camera whenever it is asked.  A stored flag
would be a second copy of it, with every path that moves the camera (the cube, the
swipe, the drag orbit, the keyboard, a map load, a focus) obliged to keep it in
step.  See D-Y2.

**Rung 1 survives, conditionally.**  Continuing the chain under the cursor is
Plasticity's `restrictionPlane`, which outranks even ortho mode
(`PointPickerModel.ts:68`).  But a chain in some OTHER plane cannot be continued
from this view either — that is the same "you cannot draw Z from the top" — so it
is honoured only when its plane is PARALLEL to the view plane, which is exactly
when continuing is possible.  Its offset along the axis is then kept, so a sketch
stays coplanar with itself.

**The view cube sets the plane at the click too.**  Rung 0 would reach the same
answer at the next tool start, so this is not what makes the fix work — it is what
makes it VISIBLE: the console line and the plane readout change AT THE CLICK
rather than after a tool is started.  It is also literally Plasticity's view-cube
contract (`ViewportNavigator.ts:136`).  Safe mid-gesture: a live drawing tool
captured its plane in `Begin()`, so changing the ACTIVE plane cannot move points
already placed.

**The axis guides gained the plane NORMAL.**  Round P offered world Z plus the
plane's U and V — the full basis only while the plane is XY.  On a vertical
working plane (exactly what rung 0 now hands you from a front/back/left/right
camera) the normal is the ONE direction that leaves the plane, and it was the one
direction with no guide.  Plasticity offers it both as `NormalAxisSnap`
(`plasticity/src/editor/snaps/AxisSnap.ts:129-133`, handed out by
`FaceSnap.additionalSnapsFor`, `Snaps.ts:353-358`, selectable with `n`,
`default-keymap.ts:354-360`) and, in ortho mode, as the whole world triple rotated
into the plane's basis (`const quat = viewportInfo.isOrthoMode ?
viewportInfo.constructionPlane.orientation : new THREE.Quaternion(); …
this.addAxesAt(snap.position, quat, XYZ, …)`, `PointPickerModel.ts:274-292`).
The world-vertical dedup is unchanged and now covers all three candidates.

### 55.5 Mode 2 + Ctrl selects construction lines only

USER DIRECTIVE: *"mode -2 selection needs a way to select only lines.  Maybe with
ctrl-held it does that(box select primarily).  Make it so and show it in the
bottom when using mode 2."*

Ctrl is a FILTER on the candidate set, not a new gesture: the marquee, the click,
the crossing/containment rule and the undo shape are all unchanged, and only what
is allowed to answer changes.  Box select runs `KiwiConSel_ApplyRect` alone with
the brush pass skipped; click select runs the construction arm alone.  Without
Ctrl, mode 2 is byte-for-byte what it was.

The cost is real and is stated rather than hidden: Ctrl already means SUBTRACT
everywhere in this file, and inside mode 2 it cannot mean both.  See D-Y4.

The hint strip gains a `Ctrl — Lines only` chip whenever the mode mask is EDGE, in
BOTH prompt builders that can be on screen in that mode (with a selection and
without one) — a hint that only appears once something is selected is a hint that
arrives after it was needed.

### 55.6 Clicks that were being eaten

USER REPORT: *"Clicks are still ignored sometimes.  Makes it really annoying to
work fast."*  A full end-to-end audit of the dispatch path found five real eaters.
Four are fixed; the fifth is deliberate and documented in KNOWN_ISSUES.

**(a) The sliver marquee.**  `KiwiBox_End`'s click fallback tests both axes with
`&&`, so a purely vertical hand-wobble — `dx = 0, dy = 9`, the commonest click
artifact and the one a fast worker makes most — escaped it and ran a box select
with a ZERO-WIDTH rect.  Everything downstream then stacked against it:
`crossing = (dx < 0)` is false for a rightward or zero dx, so the pass is
CONTAINMENT and no brush is ever contained in a 0x9 px rect; the centre-ray rescue
that exists for exactly this is gated on `crossing`, so it never ran; and
`ApplyAndSync` did `Sel_Clear` on the empty result.  The click did nothing AND
dropped the selection.  It is DIRECTIONAL — a wobble that happens to travel left
is crossing and IS rescued — which is why it read as "sometimes".  Fix: a marquee
that is a sliver on either axis **and found nothing** is re-run as a click at the
press pixel.  Both halves matter (D-Y5).

**(b) A focused text field swallowed the first click into the viewport.**
`ImGui::Image` submits with id 0, and plain `IsItemHovered()` cancels the hover
whenever ANY other item owns `g.ActiveId` — the only exemption is the window's own
`MoveId`.  An `InputText` holds `ActiveId` ACROSS FRAMES while focused, and the
viewport images are submitted BEFORE the panels, so on the frame where the user
clicked into the 3D view after typing anywhere (entity panel, prefs, surface
inspector, an outliner rename, the view-cube grid type-in, the palette's
auto-focused filter) `s_hovered[]` was false and the click was never dispatched at
all.  The field deactivates later in the same frame, so the SECOND click worked.
That is the whole "takes 2 tries" shape, and it explains the randomness: it
depended on whether the user had typed since the last viewport click.  Fix:
`ImGuiHoveredFlags_AllowWhenBlockedByActiveItem`, which removes exactly that
cancellation.  Window OVERLAP is a separate test (`g.HoveredWindow`) and is
untouched, so a panel or popup over the image still blocks it — which must keep
happening.

**(c) The grid type-in popup un-hovered the whole camera.**
`DrawGridEditPopup` returned true for as long as it was OPEN, and the caller ORs
that into the image's hover claim — so while the popup was up, EVERY click
anywhere in the 3D view was discarded.  It now claims only its own rect.

**(d) Double-click stole the second click's meaning.**  ImGui's double-click
detector is TIME AND DISTANCE ONLY (0.30 s, 6 px) and has no idea what is under
the cursor, so two quick clicks 6 px apart on DIFFERENT objects fired
`KiwiSelExt_CameraDoubleClick`, whose `Sel_Clear` + select-connected replaced the
second click's plain select.  Worse, it CASCADES with (a) and (b): a user whose
click was swallowed re-clicks immediately, within 300 ms and a few pixels — the
exact input the detector calls a double-click.  Fix: the brush under the second
click must already be represented in the selection.  Tested by brush rather than
by item so it holds in every mode; a genuine double-click always passes and every
other case falls straight back to the ordinary click path.

**(e) Input dispatched for a frame that never ran.**  The pump calls
`ImGuiShell_DispatchViewportInput` every tick, but `ImGuiShell_DrawOverlay` has
four early-outs and any of them skips the `NewFrame` that produces the io state
the dispatch reads.  When that happened, `io.MousePos`, `IsMouseClicked/Released`
and `s_hovered[]` were all LAST tick's and the same edges were dispatched twice —
the press is protected by `s_inputOwner`, the RELEASE is not, and re-firing it ran
the legacy up-handler for a press the viewport never saw.  Fix: `s_frameLive`, set
past every early-out and consumed by the dispatch, with the stuck-drag guard
hoisted out so it still runs on the skipped path.

### 55.7 The bevel gets the lollipop, and an out-of-range colour

USER DIRECTIVE: *"Edge bevel mode needs a gizmo/lollipop.  Hard to tell when where
it starts/beings and goes out of range."*

The B chamfer/fillet is a one-degree-of-freedom gesture measured along a bisector
— exactly the class kiwi_lollipop.h was written for ("face push/pull, region
extrude and face extrude … the gestures with ONE degree of freedom") — and it was
the only one of the four without a handle.  Nothing is reinvented: answering
`LollipopHandle` is the whole contract, and kiwi_lollipop.cpp then owns the ring,
the stem, the ball, the hit test, the grab and the gizmo stand-down.

The header's two rules, applied to a chamfer: the anchor is the DRIVING unit's
chamfer-face midpoint AT THE CURRENT DEPTH (`mid - biasedNormal * depth`, the same
point the existing bisector stub ends at), so it rides the moving face; and the
direction is `-biasedNormal`, the way the chamfer travels as the depth grows.  The
depth is clamped non-negative in `Recompute`, so unlike a push/pull there is no
sign to fold in.  `Rebase()` re-latches through the existing `ReLatchFor`, so
taking hold of the ball cannot make the chamfer jump.

**Out of range is a VISIBILITY problem, not a clamp problem.**  Every one-axis
command already knows when its value is past what the geometry allows — that is
the §19 validity gate, surfaced as `HudInvalid()` — and already tints its own
preview with it.  The HANDLE did not, so the one piece of UI the eye is on stayed
cheerfully yellow while the preview went red behind the solid.  The lollipop now
asks the COMMAND and paints the whole glyph, ring included, in the same red
`KEXT_COL_BAD` and `KPF_COL_BAD` already are — so "this is refused" is ONE colour
across the layer, and every present and future lollipop host gets it from a
predicate it already implements.

### 55.8 The cut disc is wound toward the eye

USER DIRECTIVE: *"the cut previewer I asked you to add only renders when i'm
underneath the grid.  Fix that."*

"Visible from one side only" is backface culling on a fixed winding, and the state
says so.  The fan is drawn with `g_qeglobals.d_white =
Material_RegisterHandle("white_tools")` (gfxwrapper.cpp:77), and
`main/materials/white_tools` carries refStateBits[0] `0x08128965`, whose cull field
(`& GFXS0_CULL_MASK` = `0x8000`) is `GFXS0_CULL_BACK` -> `s_cullTable_30[2] = 3 =
D3DCULL_CCW` (r_state.cpp:28, :919).  `default.sm` passes `cullFace` through, so
the state reaches the device.  There is nothing to fix in the material.

The fix is one sign.  The ring runs `+u` then `+v` with `v = n x u`, so its front
face is the `+n` side; making `n` point AT the camera makes the front face the one
being looked at, from either side of a surface.  That is also exactly what the
null-normal branch already does (`n = -vpn`), which is why the camera-facing discs
— a line hover, an edge hover — always drew and only the FACE-oriented ones went
missing.  The disc still lies IN the surface plane; only its winding changes.

### 55.9 Decision log — round Y

- **D-Y1 — the substitution predicate generalises on TECHSET CLASS and the
  OPAQUE SORT, not on `sortKey < SORTKEY_DECAL`.**  The naive reading was checked
  against the shipped assets and is wrong by 434 materials: CoD4's world decals
  sit at sortKeys 9..12, i.e. below `SORTKEY_DECAL` (24), and are correctly
  depth-write-free.  The real opaque world sort is the single value 4, which the
  engine special-cases by name (r_material_load_obj.cpp:5458).  Arm A (editor/HUD
  techsets) is what actually catches the reported material; arm B (`sortKey <= 4`)
  is the general rule, costing 13 FX materials nobody applies to a brush. (§55.1)
- **D-Y2 — the axis-view test is LIVE, not latched.**  Plasticity stores an
  `orthoState` and clears it on the first orbit, which is definitionally "the
  camera is still on the axis it was navigated to".  KIWI reads that off the
  camera with one dot product instead.  A stored flag would be a second copy of a
  derivable fact, and every path that moves the camera would have to keep it in
  step. (§55.4)
- **D-Y3 — rung 0 outranks the face rungs, but not a PARALLEL construction
  chain.**  Plasticity's ortho mode drops face snaps outright
  (`SnapPickerStrategy.ts:98`) while `restrictionPlane` still wins
  (`PointPickerModel.ts:68`).  KIWI mirrors both, with "parallel" as the test for
  whether the chain can be continued from this view at all. (§55.4)
- **D-Y4 — Ctrl in EDGE mode stops meaning subtract, and that is accepted.**  The
  directive asks for the lines-only filter on Ctrl by name.  Ctrl cannot mean both
  in one mode, so while it is held in mode 2 the grammar is Ctrl = lines-only
  replace, Ctrl+Shift = lines-only add, and subtract-in-edge-mode is unreachable
  until Ctrl is released.  Plain mode 2 is untouched, which is the other half of
  the directive. (§55.5)
- **D-Y5 — the sliver-marquee rescue requires BOTH "sliver" and "found
  nothing".**  "Sliver on either axis" alone would demote a deliberate thin
  crossing swipe (drag straight down a wall to catch a column of edges) to a
  click; "found nothing" alone would fire on an honest empty marquee, where
  clearing the selection is correct.  Together they name exactly the gesture that
  was a click. (§55.6a)
- **D-Y6 — the viewport image is hovered even while another item is active.**
  `AllowWhenBlockedByActiveItem` removes ImGui's ActiveId cancellation and nothing
  else; window overlap is a separate test and is deliberately left alone, so a
  panel or popup drawn over the image still blocks clicks.  The alternative —
  submitting the image with a real id — would make it an interactive item and put
  it in the tab order. (§55.6b)
- **D-Y7 — a double-click must land on something already selected.**  The
  alternative (compare the two clicks' picks directly) would need the click path
  to remember its last pick across frames, and would still be wrong for the
  cascade case where the FIRST click was the one that got eaten.  "Expand what is
  selected" is what the verb means anyway. (§55.6d)
- **D-Y8 — the out-of-range colour is asked of the COMMAND (`HudInvalid`), not
  plumbed per tool.**  Every lollipop host already implements that predicate for
  its own preview, so one test in kiwi_lollipop.cpp gives the red state to the
  push/pull, both extrudes and the bevel at once — and to whatever gets a lollipop
  next. (§55.7)
- **D-Y9 — the boot template restore goes in `Radiant_ApplyStartupTextureScale`,
  not in `Texture_ResetPosition`.**  `Texture_ResetPosition` is IDA-faithful and
  its behaviour is correct when the user changes the texture scale mid-session on
  a browser they are looking at.  Only the BOOT call picks a material nobody asked
  for, so only the boot call is corrected — and by RESTORING step 6a-pre's own
  seed, so `.map` output is the un-clobbered boot's and D-X4 still holds. (§55.1)

# Part XI — Round Z (2026-08-10): tool swaps, opt-in snapping, and the pivot that was counted twice

## 56. Round Z — six reports

### 56.1 A tool swap takes over a live gesture (ITEM 1)

**USER REPORT, verbatim:** *"when pasting a brush, it goes into move mode
automatically, however if I want to paste and rotate(or similar) a brush, it
requires a de-selection first. This is unacceptable, allow tool swaps."*

Paste and Clone end in `KiwiCmd_AfterPaste` (kiwi_command.cpp), which starts
`KIWI_CMD_MOVE` **PAUSED** — and `Shift+D` (round J's duplicate) lands on the same
tail (kiwi_dupe.cpp:667). So after a paste the editor is always inside a modal
command, and `KiwiUX_KeyFunnel` routes every key to `KiwiCmd_KeyDown`, whose last
rung **swallows everything it did not recognise**. `R` and `S` are not Move's keys,
so they died there, every time — the identical trap round N found for `Ctrl+R`, one
state further along.

Round N's rung (`PreemptIdle` + `PreemptVerb`) could not cover this. It is
deliberately narrow: an **unmoved, auto-entered FACE** gesture, yielding to a verb
about that face, and always by **cancelling**. A paste is an OBJECT move that the
user may already have dragged, so `PreemptIdle` answers false and there is nothing
to cancel record-free anyway.

**PLASTICITY'S RULE, which this mirrors.** `CommandExecutor.enqueue( command,
interrupt = true )` interrupts whatever is running (CommandExecutor.ts:48-56 —
`this.active?.interrupt()`), and the interrupt **forks on the active command's
state** (CancellableRegistor.ts:48-70). Their own comment states it:

> *"Normally this should cancel the current command. However, when the command is
> in the 'Awaiting' state, the command is ready to be commit (i.e., the user
> entered all necessary data) and we generally interpret that as a commit."*

Mechanically: a command still collecting input (`'None'`) is marked `'Interrupted'`,
its `finish()` becomes a no-op (CancellableRegistor.ts:39), CommandExecutor's
`if ( command.state === 'Finished' )` fails, and `originator.discardSideEffects`
rolls it back with **no history entry** (CommandExecutor.ts:98-112). One in
`'Awaiting'` finishes and `history.add( command.pretty, state )` stamps a record
(CommandExecutor.ts:105, History.ts:300-308).

KIWI has no `'Awaiting'`, and does not need one, because it can ask the gesture a
better question than "are you ready": **did you actually do anything**.

    GestureMoved() true   -> COMMIT.  The record closes on its own edit, exactly as
                             Enter would have, and the new tool starts over the
                             result.
    GestureMoved() false  -> CANCEL.  Provably record-free — no bracket was opened,
                             so KiwiCmd_UndoCancel closes nothing — and Cancel never
                             clears the selection, so the new tool inherits it.

**THE THREE PIECES.**

* `KiwiEditorCommand::CanSwapTo( int )` and `::GestureMoved()` (kiwi_command.h).
  `CanSwapTo` defaults to **false**, so every command that does not opt in keeps
  the pre-round-Z swallow byte for byte.
* `SwapVerb` (kiwi_command.cpp) — the ALLOW-LIST: `KIWI_CMD_MOVE`,
  `KIWI_CMD_ROTATE`, `KIWI_CMD_SCALE`, and nothing else. Same shape and same
  safety argument as `PreemptVerb`: a transform is a statement about the
  SELECTION, and the selection is exactly what survives both a Commit and a
  Cancel of another transform.
* the funnel rung, placed **below** round N's preempt rung and **above**
  `KiwiCmd_KeyDown`, so the face-context set keeps its narrower cancel-only
  behaviour and this is only what falls through. It resolves the chord in the
  LIVE binding table, requires the id to be runnable **now**
  (`KiwiCmd_CanExecute`, asked before anything is torn down), commits or cancels,
  and returns **false** so the chord reaches `Radiant_TryHotkey` by the ordinary
  route. One dispatch path, not two.

`G`->`R`, `R`->`S`, `S`->`G` and `G`->`G` (restart) all work, in any order. **Esc is
untouched** and still cancels the whole gesture without starting anything.

**THE ONE REFUSAL, and it is not arbitrary.** `KiwiMoveCommand::CanSwapTo` declines
a FACE push that has already moved, because round K's after-confirm deselect
clears the selection on exactly that path — committing it to run `R` would hand the
new verb an empty selection and a console complaint. An *unmoved* face gesture is
fine: it cancels, and Cancel clears nothing. The construction arm is declined
outright (its undo record is a store snapshot, and no verb in the swap list acts on
construction geometry), and so is a live `V` placement (V owns the keyboard for
those frames).

### 56.2 Extrude snapping is OPT-IN (ITEM 2)

**USER DIRECTIVE, verbatim:** *"When extruding, it should not snap by default. Make
it snap only when holding CTRL. It's just not good to use in a cluttered scene."*

The three **one-axis gestures** — the face push/pull (§20), the region extrude
(§23) and the face extrude / un-extrude — now default to **no snapping at all**:
no geometry arms, no grid quantisation, just the raw cursor-mapped distance. The
numeric field is untouched and is still the way to be exact. **Ctrl** runs the
full ranked query.

This **inverts** `kiwi_snap.h` arm 0 for those three and only those three. It is
implemented as one XOR rather than a second arm:

    const bool ctrlHeld = GetAsyncKeyState( VK_CONTROL ) & 0x8000;
    if ( ctrlHeld != KiwiCmd_SnapOptIn() )   ->  SNAP_NONE + the raw point

`KiwiCmd_SnapOptIn()` asks the ACTIVE command (`KiwiEditorCommand::SnapOptIn`,
default false), so with nothing running — and for every other command — arm 0 is
byte-for-byte what it was.

**That is Plasticity's own shape, not an invention.** Ctrl is bound to
`snaps:temporarily-disable` on keydown and `-enable` on keyup
(default-keymap.ts:353,365-366), which set `snaps.xor`; the manager then reads

    get enabled() { return this._enabled !== this.xor }   // SnapManager.ts:28-49

— a genuine **XOR**, so their modifier inverts whatever the context's default is
rather than always meaning "off" (their own case: with snaps globally off, holding
Ctrl turns them **on**). KIWI's default was "on everywhere"; for these three
gestures it is "off", and Ctrl still means *the other one*.

**Suppressed is an ANSWER, not a failure** (kiwi_snap.h's own arm-9 rule): `valid`
stays true and `position` is the raw point, so all three consumers — every one of
which tests `type != SNAP_NONE` — fall through to their own raw mapping with no new
branch.

**`KEXT_SELF_SNAP_BAND` (round X) still applies.** It sits inside the geometry
branch, which is what Ctrl now *opens*: Ctrl turns the ranked query on, it does not
turn the self-snap rule off.

The bottom-left chip strip grows a **`Ctrl` — `Snap`** chip while such a command is
live, asked of the command rather than listed by name, so a future opt-in gets the
right chip for free. Every other command's strip is unchanged.

### 56.3 A face is a PLANE, not a point (ITEM 3)

**USER REPORT, verbatim:** *"When snapping an extrusion to another face. The entire
face should give the same result. Right now it's going up or down slightly more or
less based on where the face is - which makes no sense for a flat 'roof' on a
brush."*

All three one-axis gestures resolved a geometry snap with the same hand-rolled
line, in three places:

    sd = dot( snap.position - ref, axis )

For arm 6 (`SNAP_FACE`) `snap.position` is the **ray-surface hit** — a point that
slides across the target face as the cursor moves (kiwi_snap.cpp arm 6 writes
`raw`, the `Test_Ray` point, unsnapped). Projecting a sliding point gives a sliding
answer, and the amount it slides is exactly the component of the face's extent that
lies along the push direction. A **wall** is the pathological case: the projection
is then pure noise — the height the cursor happened to graze.

A face is not a point. "Extrude up to that face" has one answer: **where the target
face's plane crosses the gesture's own axis.**

    t = ( n . ( hit - ref ) ) / ( n . axis )

One scalar for the whole face, independent of where on it the cursor is, and
identical to the old projection in the case that was already right (`n` parallel to
`axis`, where the denominator is +/-1). The **hit point** is used as the plane's
reference rather than `face.plane.dist`, because the hit is by construction on the
plane the arm reported — so this cannot disagree with what the user is pointing at
even if the cached plane is stale.

A **near-parallel** face (|n . axis| < `KSNAP_AXIS_PARALLEL`, 0.05 ~ 87 degrees) is
**refused**, which is the fix's other half: a vertical wall can no longer teleport a
vertical push to wherever the cursor grazed it. The caller leaves its distance
alone.

POINT candidates — vertices, edge points, edge and face **midpoints**, construction
anchors and intersections — are projected exactly as before. Those name one
position and projecting a position is the right question; only the AREA arm was
ever answering a different one. `SNAP_FACE_CENTER` is deliberately in the *point*
set: it is the winding centroid, a deterministic single point, and it does not
slide.

The rule lives **once**, as `KiwiSnap_AxisDepth` (kiwi_snap.h), shared by the face
push, the region extrude and the face extrude / un-extrude — so the three cannot
drift the way three copies of one `dot()` did.

### 56.4 The pivot was counted twice (ITEM 4)

**USER REPORT, verbatim:** *"I just set the pivot to the top center of this cylinder
and it does this. It's wrong. The gizmo needs to ride exactly where I placed the
pivot(V) so I can snap how I want it to snap easier. the snapping needs to respect
my mouse more in this aspect as well. I should be able to hover exactly where the
corner of another brush is, but I have to guess where an invisible offset is
currently."* — with the gizmo drawn floating **~101.5 inches above** the cylinder
and the value bubble reading **101.5 in**.

The offset **was** the gesture's own travel. That is the whole diagnosis, and the
screenshot proves it: the two numbers are the same number.

**THE INVARIANT.** The move's one rule is

    live position of the reference point  ==  m_ref + m_total

and everything reads it that way: `LiveAnchor`, the gizmo through
`KiwiXform_ActivePivot`, `DrawWorld`'s marker, `Commit`'s `PivotRide`, and the grid
arm's `snap( m_ref + total ) - m_ref`.

**THE BUG, one line.** `KiwiMoveCommand::ApplyPivot( p )` did `Copy3( p, m_ref )`.
`p` is a point picked under the cursor — a **current** world position, which
already contains `m_total`. Writing it into `m_ref` made the invariant read
`p + m_total`, so everything that draws or rides the pivot was off by exactly what
the move had applied so far. Pressing `V` **before** any travel hid it completely
(at `m_total == 0` the two are the same point), which is why it survived rounds L
and T.

**THE FIX IS AT THE SOURCE** — subtract what has already been applied, so the BASE
that satisfies the invariant is stored (`p - m_total`, or for a face push
`p - pushDir * scalar`). Nothing downstream changes and nothing is re-offset at
draw time:

* `LiveAnchor` / the gizmo -> `m_ref + m_total == p`: **drawn exactly on it**;
* the geometry-snap arm -> `total = snapPos - m_ref`, so the live anchor lands
  **exactly** on the snap target;
* `Commit`'s `PivotRide` -> `PivotStore( m_ref + m_total )` = where it actually is;
* `RecomputeFace`'s absolute grid arm reads `m_ref[axis]` as the plane's
  **baseline** position, which the face form preserves (`p` is on the pushed
  plane, so `p - pushDir*scalar` is on the baseline one).

**TWO MORE READERS WERE STALE.** `PivotAnchor()` returned `m_ref` (the base), and
`BeginPivot` seeded the placement marker from the stored SESSION pivot first —
so a `V` pressed half way through a move put the marker back where the move
*started*. `PivotAnchor()` now forwards to `LiveAnchor`, and `BeginPivot` asks the
command rather than the session store (which is also the only reading that is right
when no session pivot exists yet — the common case, since V is how one gets
placed).

**AND ROUND T-7 IS REVERSED.** `KiwiMoveCommand::SnapQueryAnchor` redirected the
snap QUERY to the pivot's projected pixel. That is what made the offset *invisible*:
the mouse was no longer pointing at the candidates, so the user had to steer a point
they could only infer, one frame behind, until it happened to graze a target. It was
aimed at the wrong defect — the pivot really was in the wrong place, but because of
the double-count above, not because the query was at the cursor. With the
double-count fixed the round-L mapping is the whole answer: **point at the corner,
the pivot goes to the corner.** The override is gone (the framework hook stays for a
command that genuinely needs it), and `m_snapAnchor` / `m_snapAnchorHave` went with
it — their only reader was that projection.

### 56.5 One bottom band (ITEM 5)

**USER REPORT (screenshot):** the command's chip row (`RMB/Enter Confirm · Esc
Cancel · Drag Adjust · Tab Field · 0-9 Exact`), the §26 texture readout
(`building_wall_wood01, shift 0,0 size 64,64 rot 0, layer 0`) and the §13 numeric
HUD (`free · drag a handle / type a value · RMB / Enter confirm …`) drawn **on top
of one another** at the same anchor.

**EACH HAD ITS OWN COPY OF THE SAME LINE**: `kiwi_numeric.cpp`'s
`y = imgMinY + imgH - boxH - 12`, `kiwi_uv.cpp`'s identical one, and
`kiwi_hints.cpp`'s `bottomY = imgMinY + imgH - KHINT_EDGE`. Their comments each
claimed a different CORNER — centre, left, left — which is true horizontally and
worth nothing vertically: a status line wide enough to reach the middle of the
image overlaps whatever corner it is nominally hugging.

**THE VERTICAL ANCHOR IS NOW ALLOCATED, ONCE.** `KiwiHud_BandBegin` /
`KiwiHud_BandTake` (kiwi_hints.h): `KiwiVP_DrawCameraOverlay` opens the band for the
frame and each bottom-anchored overlay TAKES a slot for its own height. The band
grows upward, so the first taker hugs the edge and each later one sits above the
last with a fixed gap. Nothing has to know what the others are or how tall they got,
and a fourth row later costs one call. The band is keyed on
`ImGui::GetFrameCount()` rather than a bool, so it is self-clearing and no caller
reached on a later frame by another route can be handed a stale slot.

The three calls in `KiwiVP_DrawCameraOverlay` are **reordered** to set the stacking
order: chip strip lowest (it is the persistent grammar and the row a user looks
down for), numeric HUD above it, texture readout above that. Reordering is free for
the z-order — all three are ImDrawList-only and, now that they do not overlap,
nothing is on top of anything. The value **bubble** is not a band taker: it is
pinned at the action geometry, not at the bottom edge.

**AND THE REDUNDANT ROW YIELDS.** The chip strip and the numeric HUD were also
saying the same thing. The HUD's hard-coded tail
(`RMB / Enter confirm · drag adjust · Tab field · Esc cancel`) is dropped **while
the chips are up**, and the line keeps what only it has: the command name, its
transform state and the typed value. With hints toggled off the tail comes back in
full, because then it is the only place the grammar is written down at all. The
chips are the survivor because they read their keys **live** out of
`g_radiantCommands` and cannot go stale when a binding is remapped; the tail is a
sentence.

Both strips of the hint panel share **one** band slot, sized to the taller of the
two: they sit at opposite ends of the same row band (the 0.46 width split
guarantees they cannot collide), so taking a slot each would stack the right-hand
strip above the left-hand one for no reason.

### 56.6 RMB confirm slop (ITEM 6)

**USER REPORT, verbatim:** *"There needs to be a tiny threshold where right clicking
confirms an action even if the camera is slightly moved. When doing actions fast,
this is the case."*

A threshold already existed — `KVP_RMB_CLICK_PIXELS` = 4 — and this file argued for
keeping it tight, because *"a wrong answer costs the user a view, not a
selection"*. That argument holds for the **context menu** and does not hold for the
**confirm**: a lost confirm costs the whole gesture the user was in the middle of,
and a couple of pixels of pan costs nothing. Two different questions were sharing
one number.

Worse, `s_rmbDrag` is **sticky**: once the pan engaged at 4 px there was no way
back, so a release 5 px from the press — a click, by any hand doing this at speed —
lost the confirm entirely.

So the confirm gets its own slop, re-read from the NET travel at the release edge:

    CONFIRM   net travel <= KVP_RMB_CONFIRM_PIXELS (== KBOX_CLICK_PIXELS, 8),
              even if the camera already panned by those pixels
    MENU      net travel <= 4 (!s_rmbDrag, unchanged)

8 is `KBOX_CLICK_PIXELS`, the LMB side's "was this a click", reused for exactly the
reason D-X7 gave for reusing it there: one number answers "was this a click" in this
editor. The menu keeps the tight 4 because a context menu popping after a pan the
user meant is the worse wrong answer, and nothing about the menu was reported.

**PLASTICITY MEASURES THE SAME THING AT THE SAME EDGE.** Its RMB tap is
`mouse2 -> command:finish` (default-keymap.ts:378-384) and fires only when the
release is within `consummationDistanceThreshold` (4) **and** 200 ms of the press
(KeyboardEventManager.ts:12-13, :66-85), measured as
`currentPosition.distanceTo( startPosition )` on raw `clientX/clientY` — i.e. NET
displacement from the press, which is exactly what `s_rmbTrav*` accumulates (a sum
of signed deltas telescopes to it). Their orbit controls see the same pointer
stream **independently** (OrbitControls.ts:362-367), so in Plasticity the camera
pans during those sub-threshold pixels and the confirm still fires.

**THE TINY PAN IS ACCEPTED, NOT UNDONE**, and that is the choice made here: it is
what Plasticity does per the two independent streams above; `KiwiCam_PanDrag` is
depth- and scale-dependent so "the inverse pan" is not simply the negated pixels
and an approximate un-pan would be a NEW source of drift on a gesture the user
experienced as a click; and under 8 px it is invisible, which is the whole premise
of a click slop. **No time component was added** — this layer has no press
timestamp, and the pixel test alone is what the report is about.

### 56.7 Decision log — round Z

- **D-Z1 — a tool swap COMMITS a moved gesture and CANCELS an unmoved one, and that
  is Plasticity's fork.**  Their `interrupt()` cancels a command in `'None'` and
  commits one in `'Awaiting'` (CancellableRegistor.ts:48-70,
  CommandExecutor.ts:98-112), i.e. it forks on "has this collected everything it
  needs".  KIWI forks on the stronger question it can actually answer — "has this
  applied anything" — because a modal transform has no input to finish collecting,
  only work it has or has not done.  The alternative (always cancel) throws away an
  edit the user made and never asked to lose; always commit stamps a record for a
  gesture that did nothing. (§56.1)
- **D-Z2 — Ctrl is an XOR against a per-command default, not a global "suppress".**
  The inversion is deliberately asymmetric and deliberately narrow: it applies to
  the three ONE-AXIS gestures and nothing else, because those are the ones whose
  whole output is a single scalar that a stray candidate can throw by tens of
  units, and they are what the directive names.  A blanket inversion would be a
  second grammar for every drag in the editor, none of which was reported.
  Plasticity's own `enabled = _enabled !== xor` (SnapManager.ts:28-49) is exactly
  this: the modifier inverts the context's default rather than meaning one fixed
  thing. (§56.2)
- **D-Z3 — a SURFACE snap is resolved against the target's PLANE; a POINT snap is
  projected.**  The alternative — projecting the hit and then quantising, or
  averaging over the face — either keeps the sliding answer or invents a point the
  user is not pointing at.  A near-parallel face is REFUSED rather than clamped:
  clamping would answer a question that has no answer, and a hair of cursor
  movement would move it by hundreds of units. (§56.3)
- **D-Z4 — the pivot fix goes in `ApplyPivot`, not in the drawing.**  Re-offsetting
  the gizmo at draw time would have hidden the same defect from the snap arm, the
  commit ride and the grid arm, all of which read the same invariant.  One
  subtraction at the one place the invariant was broken fixes all five readers, and
  the two that were separately stale (`PivotAnchor`, `BeginPivot`) are corrected by
  pointing them at the one definition of "now" rather than by giving them a
  third. (§56.4)
- **D-Z5 — round T-7's snap-query redirect is REVERSED, and that is a reversal, not
  a refinement.**  It was a correct implementation of a wrong diagnosis: the pivot
  was in the wrong place because of D-Z4's double-count, and moving the QUESTION to
  the broken answer made the offset unsteerable.  The hook stays in
  `KiwiEditorCommand` — it is a reasonable thing for some future command to want —
  but no command uses it. (§56.4)
- **D-Z6 — the bottom band allocates a VERTICAL slot per row and ignores the
  horizontal.**  Stacking a bottom-CENTRE box above a bottom-LEFT one wastes a row
  of screen where they would not have collided.  The alternative — tracking each
  row's x-extent and packing — is a layout engine for three boxes whose widths
  depend on a texture name and a live binding table, and the failure it prevents
  (two boxes at the same y that happen not to overlap) is invisible.  One column of
  slots is one rule and cannot produce the reported bug. (§56.5)
- **D-Z7 — the numeric HUD yields its grammar tail to the chips, rather than the
  chips being suppressed.**  The chips read every key LIVE from `g_radiantCommands`
  and render them as keycaps; the tail is a hard-coded sentence that would lie the
  moment anything is rebound.  When the chips are toggled OFF the tail returns, so
  the grammar is never written down in zero places. (§56.5)
- **D-Z8 — the RMB confirm slop is 8 (`KBOX_CLICK_PIXELS`), the menu's stays 4.**
  Plasticity uses 4 for both (KeyboardEventManager.ts:12-13), but it pairs the
  distance with a 200 ms time test this layer cannot make, and its report is not
  the one that was filed.  8 is the number this editor already uses to answer "was
  this a click" (D-X7), and splitting the two decisions is what lets the confirm be
  forgiving without making the context menu fire after a deliberate pan. (§56.6)

## 57. Round AA — nine reports

### 57.1 The texture browser regression (ITEM 1)

**USER REPORT, verbatim:** *"you broke the textures window! It only shows 4
materials now. Revert whatever you did."*

Round Y appended a block to the tail of `Radiant_ApplyStartupTextureScale`
(mainfrm.cpp) to put the `$default` template back after `Texture_ResetPosition`'s
first-visible-thumbnail clobber. It was written as a **verbatim excerpt of
`Radiant_SeedCurrentTexdefs`' layer-0 arm** — `SetMaterial( "$default", … )` +
`Init_MaterialLayer( …, 0.25f )` — on the reasoning that replaying the seed's own
calls could not choose anything the seed had not already chosen.

**That reasoning was about the VALUE, and the calls are not only about the value.**
`SetMaterial` (materialdef.cpp:101) dispatches into `Texture_GetHandle`
(texwnd.cpp:313), which is a **registering** call against the texture browser's own
state: it walks `texWndGlob_textureOffset.qtextures`, sets `is_in_use`, and on a
miss runs `Register_WorldMaterial` + `Editor_AddRadiantMaterial`
(texwnd.cpp:273-306) — which prepends to `->qtextures` and appends to
`sorted_materials[ materialCount++ ]`. Step 6a-pre runs those identical calls
**before `Load_Materials()`**, which is exactly why the seed never had this
problem and why the excerpt looked safe. The copy runs at the **other** end of the
boot: after the browser is populated, after `Texture_ShowAll()`, and after the
whole `Radiant_CheckTextureScale` -> `Texture_ResetPosition` -> `TexWnd_HitTest` /
`TexWnd_ApplyMaterialAtIndex` -> `Texture_SetTexture` -> `Brush_SetTexture`
subtree, with nothing after it to re-establish the listing.

**THE FIX IS THAT THE RESTORE NEEDS NO CALLS AT ALL.** Everything it wanted is
already sitting in `random_texture_stuff[0]` when the seed finishes — the resolved
handles and the `mat_texDef` `Init_MaterialLayer` has just written. The scale pass
merely overwrites it. So the seed **snapshots** those bytes
(`Radiant_Kiwi_SeedSnapshotLayer0`) and the tail **copies them back**:

    rts[0].mtl        = s_kiwiSeedLayer0Mtl;
    rts[0].sampleSize = s_kiwiSeedLayer0SampleSize;

Byte-identical to what step 6a-pre produced, so D-X4 ("do not change what is
written to disk") is untouched and round Y's actual fix — new brushes wear
`$default` and not the tool material `aa_default` — is fully kept. And it cannot
have a side effect of any kind, because it makes no call.

**AND THE BOOT NO LONGER DEPENDS ON THE ORDER.** `Texture_ShowAll()` was asked
*before* the scale pass (radiant_main.cpp, mirroring mainfrm.cpp:1633). The base
accept predicate in `TexWnd_FilterAccept` is `tex->is_in_use` (texwnd.cpp:819), so
anything in that subtree that leaves flags clear leaves the browser showing a
handful of materials and nothing re-opens it. The call is now **also** made after
the scale pass — the same ported call, idempotent (one flag write per qtexture
plus a `W_TEXTURE` dirty bit, texwnd.cpp:574), asked after the last thing that can
narrow the listing. Show-In-Use and a map load still narrow the browser on demand
(map.cpp:546); only the BOOT state stops being order-dependent.

### 57.2 Every fill in the kiwi layer was one-sided (ITEM 2)

**USER REPORT, verbatim:** *"the light blue construction lineface isn't rendering
unless you're facing the other way (see pics). Fix this."*

Round Y found this once, on the cut disc, and fixed it in place. It was never one
bug. The kiwi layer has **six** `R_AddRenderCmdDrawTris` call sites, all on
`g_qeglobals.d_white` = `Material_RegisterHandle( "white_tools" )`
(gfxwrapper.cpp:77), whose `refStateBits[0] = 0x08128965` decodes
(`& GFXS0_CULL_MASK` -> `0x8000`) to `GFXS0_CULL_BACK` -> `s_cullTable_30[2] = 3 =
D3DCULL_CCW` (r_state.cpp:28, :919), with `default.sm` passing cullFace through.
**Five of the six emitted a fixed winding**, and two of them carried comments
asserting the opposite:

* kiwi_extrude.cpp — *"Backface culling is not available on this path (the state
  comes from d_white's material), so both are drawn once and the far side simply
  shows through."* It is available, and it comes from precisely that material.
* kiwi_region.cpp — *"the fill is visible from both sides"*, used to justify
  nudging along the view normal rather than the region normal. (The nudge argument
  is unaffected and stands; only the tail of the sentence was false.)

**ONE RULE, IN ONE PLACE.** `KiwiTris_OrientToEye` (kiwi_lines.h TRAP 3 /
kiwi_lines.cpp) rewrites an index buffer in place: a triangle whose geometric
normal points away from the eye has its last two indices swapped. **Per triangle,
not per polygon**, which is what makes one function cover every case — a flat
polygon flips as a unit because all its triangles agree; a closed volume drawn in
one batch resolves each face independently, so the near half fills *and* the far
half fills instead of one of them vanishing; a non-coplanar fan is handled without
anyone having to notice it was one. A degenerate triangle has no normal and is
left exactly as it was.

The five sites: `kiwi_region.cpp` `KiwiRegion_DrawFills` (the reported one),
`kiwi_hover.cpp` `EmitFaceFill`, `kiwi_boolean.cpp` `FillBrush`,
`kiwi_extrude.cpp` `EmitFilledTris` (the one choke point both the prism cap and
the prism sides pass through, so its `idx` became mutable), and `kiwi_split.cpp`
`DrawQuad` — whose `idx` was `static const`, and whose corners are built from a
camera vector **frozen at the click**, making it the closest analogue to the disc
bug: orbit past the click-time plane and the sweep preview disappeared entirely.

### 57.3 The chain latches its plane (ITEMS 3b + 6)

**USER REPORT, verbatim:** *"Also it's not registering as a square when floating
it off the brush."* and *"line plane recognition could use some more work. Seems
iffy."*

Shakeout H made the free-3D tools take `snap.position` **verbatim**, which was the
right fix for its bug (aim at a corner 64 units up and the point used to land at
that corner's shadow on the plane). But "verbatim" has no plane in it: as the
cursor crosses from a brush face to the ground, successive points come off
DIFFERENT surfaces, so a ring drawn part-on and part-off a brush is not one
polygon — it is four points that do not share a plane. Some rungs land within a
hair of coplanar and some do not, which **is** the "seems iffy" report: it worked
when the surfaces happened to agree and silently did not when they did not.
`kiwi_region.cpp` will not accept a non-planar ring, and it is right not to — a
skew quadrilateral is not a face.

    point 1   free.  It may come from anywhere, and PushPoint re-seats the working
              plane's ORIGIN through it.
    point 2+  ON THAT PLANE.  The snap still chooses WHERE in the plane; it just
              cannot leave it.

A closed ring is coplanar **by construction**, so the acceptance test stays strict.
The Z lock is the escape and deliberately the only one — `ApplyZLock` runs after
the projection and outranks it, because it is a constraint the user turned on.

**PushPoint's re-seat is now the first point's only.** Every later point is
already on the plane, so re-seating through it is a no-op by construction; writing
it anyway would only give float drift a chance to walk the plane one epsilon per
point along a long chain — the same class of defect the latch just fixed.

**THIS IS PLASTICITY'S SHAPE.** Every fixed-shape command there calls
`pointPicker.restrictToPlaneThroughPoint( p1, snap )` the instant the first point
lands — RectangleCommand.ts:65, BoxCommand.ts:82, CircleCommand.ts:32,
PolygonCommand.ts:35, CylinderCommand.ts:30, SphereCommand.ts:18,
EllipseCommand.ts:17, SpiralCommand.ts:29 — and `FaceSnap.restrictionFor` returns
a hard `PlaneSnap` on the face's normal (Snaps.ts:347-351).

**AND THERE IS NO HYSTERESIS TO ADD.** `KiwiCon_AutoPlaneForTool` is called from
exactly two places — `KiwiDrawTool::Begin` and kiwi_primitive.cpp's `Begin` — i.e.
ONCE, when the tool starts; the tool then works off its own `m_plane` copy. The
rungs cannot flip mid-hover and a per-frame band would damp something that does
not oscillate. **Plasticity has no hysteresis anywhere either**, and its reason is
worth recording: its construction plane never auto-derives from the surface under
the cursor at all — the plane changes only through an explicit keybinding or
navigation (Viewport.tsx:449-460, :553-566), so there is nothing to damp. Its
nearest auto-derive, the point picker's face PREFERENCE, is latched to the first
picked point (PointPickerModel.ts:89-101) and **released the instant the cursor
leaves that face** (SnapPickerStrategy.ts:50-56, :78-79) — the opposite of
hysteresis. KIWI keeps the auto-derive, because §16's "kills most explicit
management" is the whole value of it, and confines it to the one instant the tool
starts: not "never derive", but "derive once, then never again for this gesture".

**THE RUNG TABLE, AS IT NOW STANDS** (unchanged from round Y in content; what
changed is that it is now evaluated exactly once and can no longer be
re-litigated inside a gesture):

    0.  an AXIS-SNAPPED camera                -> the view's own plane   (ROUND Y)
        (takes the whole decision; the face rungs do not run)
    1.  a construction object under the cursor -> its plane             (shakeout F)
    2.  exactly ONE brush face SELECTED       -> its plane              (ROUND U)
    3.  a brush FACE under the cursor         -> its plane              (§16)
    4.  the VIEW-DOMINANT world axis          -> XY / XZ / YZ           (ROUND T)
    5.  nothing                               -> the active plane stands
    --- and then, for the whole gesture, NOTHING re-runs this. ---
    chain latch (ROUND AA): point 1 seats the origin; points 2+ are projected.

**AND THE PLANE IS NOW VISIBLE WHILE A TOOL IS LIVE.** The other half of "iffy"
was that the plane was only ever announced in the CONSOLE — a line that scrolls
away, in a pane nobody is looking at while drawing. `KiwiDrawTool::DrawWorkingPlane`
draws a bounded square outline plus a small cross at the anchor, sized in PIXELS
(`KCON_PLANE_HALF_PIXELS`) so it reads the same at any zoom, in the active-chain
amber. This is **not** round M's deleted cplane patch coming back: that was a
permanent fixture that fought §17's ground grid and drew for commands that were
not placing anything; this is bounded, and alive only while a drawing tool runs.
Plasticity's equivalent is a two-tier LINE grid, never a fill, positioned at
`constructionPlane.p` and oriented by `constructionPlane.orientation`
(GridHelper.ts:19-36, sizes at :6-15, the second tier at FloorHelper.ts:90) —
and, the part worth copying, its opacity is the SQUARED grazing angle,
`const dot = grid.dot( eye ); material.opacity = dot * dot;`
(FloorHelper.ts:124-135), so it fades to nothing as you look along it instead of
becoming a wall of aliased lines. That fade is reproduced here as a COLOUR ramp
rather than an alpha, because kiwi_lines.h TRAP 2 is explicit that alpha on this
path means "blend less of my colour in", not "be transparent". Like theirs it is
centred on the plane's own anchor and never on the cursor.

### 57.4 The loop-closing point is a snap (ITEM 3a)

**USER REPORT, verbatim:** *"We need a light snapping point here for this line
connect."*

The one target a closing gesture aims at was the one point in the scene the snap
query could not see: the in-progress chain lives in the tool's `m_pts` and is not
committed to the construction store until Finish, so arm 1 (which scans
`KiwiCon_At()`) walked right past it. The tool knew when the cursor was near it —
`NearFirstPoint` / `KCON_JOIN_PIXELS` is what closes the loop on a click — but
"the click will close" and "the point will land exactly on the first point" were
two different tests and only the first one existed. So a loop closed **visually**
and left a hairline gap, and a ring with a gap is not a region.

`KiwiCon_ToolLoopStart` (kiwi_construct.h) exposes it; **arm 0b** of kiwi_snap.cpp
consumes it at `PICK_VERT_PIXELS`, reported as `SNAP_ENDPOINT` so it inherits the
accent language that already exists for a construction endpoint and counts as a
geometry snap.

**PLASTICITY DOES THIS, AND THE THRESHOLD IS THEIRS.** `addSnaps` re-registers the
curve's points on every iteration of the point loop and names the start one
(CurveCommand.ts:73-81):

    if ( makeCurve.canBeClosed ) {
        for ( const point of makeCurve.otherPoints )
            pointPicker.addSnap( new PointSnap( undefined, point ) );
        pointPicker.addSnap( new PointSnap( "Closed", makeCurve.startPoint ) );
    }

gated on `get canBeClosed() { return this.underlying.points.length >= 3; }`
(CurveFactory.ts:185-187) — which is where `KiwiCon_ToolLoopStart`'s `>= 3` comes
from rather than from a guess.

**THE DEVIATION IS THE PRIORITY, AND IT IS DELIBERATE.** Their "Closed" snap is a
plain `PointSnap`, priority **1** — the same as every vertex and endpoint in the
scene (SnapPicker.ts:136-161; only `FaceCenterPointSnap` and `TanTanSnap` outrank
a point, at 0.99), with ties falling to array order, so a model vertex under the
cursor can beat it. KIWI puts it **above every other arm**, because (1) they do
not need the snap to win — their close test is a coincidence check on whatever
point came back, `p.manhattanDistanceTo( this.startPoint ) < 10e-6`
(CurveFactory.ts:114-116), so any snap landing exactly on the start point closes
the curve, whereas KIWI's close test is a pixel box on the CURSOR that closes the
loop without moving the point; and (2) losing that race costs the whole shape, in
a way far too small to see.

### 57.5 Planar consistency, swept (ITEM 5)

**USER DIRECTIVE, verbatim:** *"the split tool is still vulnerable to the variable
flat face non-planar behavior that was fixed with extrusions. Fix this and in
other spots too. Make basically all operations like the new extrusion, it works
good."*

Round Z's `KiwiSnap_AxisDepth` (§56.3) was applied to three one-axis gestures. The
tree was swept for every other site that reduces a snap result to a scalar or a
position along a constrained axis or plane. **The sweep list, in full:**

| site | verdict |
|---|---|
| kiwi_transform.cpp `RecomputeFace` | round Z, converted |
| kiwi_extrude.cpp region-extrude `Recompute` | round Z, converted |
| kiwi_extrude.cpp face extrude / un-extrude `Recompute` | round Z, converted |
| **kiwi_patchfillet.cpp `Recompute`** (the bevel / chamfer drag) | **CONVERTED THIS ROUND** |
| **kiwi_transform.cpp `Recompute`, `CON_AXIS` arm** (X/Y/Z-locked move) | **CONVERTED THIS ROUND** |
| kiwi_transform.cpp `Recompute`, `CON_PLANE` / `CON_FREE` arms | not applicable — 2 and 3 DOF; `AxisDepth` returns one scalar and cannot answer either, and "drag the reference point onto that vertex" is the right question there |
| kiwi_split.cpp `MouseMove` (the live Ctrl+R cut position) | **already correct, and converting it would BREAK it** — see below |
| kiwi_construct.cpp `ApplyZLock` (the drawing tools' Z lock) | SNAP_FACE cannot reach it: arm 6 is suppressed while a plane-placement tool runs. Flagged for the day that changes |
| kiwi_primitive.cpp `MouseMove` (the base stages) | reduces onto a PLANE's (u,v), not an axis; same arm-6 suppression |
| kiwi_primitive.cpp height stage, kiwi_bevel.cpp `Inset`, kiwi_transform.cpp `KiwiScaleCommand` | one-axis gestures with **no snap arm at all** — an adjacent gap, not a mis-projection. Recorded, not filled |
| kiwi_transform.cpp rotate, kiwi_uv.cpp shift / rotate, kiwi_dupe.cpp array | consume the snap as a BOOLEAN gate only (quantise / do not) — nothing to project |
| kiwi_offset.cpp, kiwi_fillet.cpp, kiwi_trim.cpp, kiwi_matchface.cpp, kiwi_boolean.cpp | `(void)snap;` — discard it entirely |
| kiwi_gizmo.cpp, kiwi_region.cpp, kiwi_arrange.cpp, kiwi_lines.cpp | never touch a `snap_result_t` |

**THE SPLIT TOOL IS THE ONE THE DIRECTIVE NAMES AND IT IS ALSO THE ONE THAT MUST
NOT BE CONVERTED.** `m_cursorT = Dot3( world, OffDir() )` reduces along the
**in-face slide axis**, so any SNAP_FACE candidate on the face being split is
near-parallel to that axis *by construction* and `KiwiSnap_AxisDepth` would refuse
every one of them. The tool already knows this: it excludes SNAP_FACE explicitly
before the reduction, and tests plane membership with
`fabsf( Dot3( n, snap.position ) - d ) <= KSPLIT_FACE_EPS` — a point-on-plane
test, not an axis depth. What it takes are vertices and edge points, and
projecting a position is the right question for those. The directive's premise
about this tool was mistaken; the finding is recorded rather than acted on,
because "make it like the extrusion" would have made it worse.

The patch-fillet conversion has two traps worth naming: the file's convention is
**inward-positive**, so the negation moves outside `AxisDepth` rather than
disappearing; and the snap branch used to overwrite `s` unconditionally, so a
near-parallel REFUSAL has to fall back to the value the drag produced rather than
to zero — the assignment moved inside the `if`.

### 57.6 The two exclusive pick modes (ITEM 7)

**USER REPORT, verbatim:** *"Make pick mode 4 a brush-only pick mode. Also for
some reason lines are clickable in mode 3. It should only be faces and
construction faces."*

Shakeout F's answer to "lines aren't selectable with ANY mode" was to make every
mode pick them: `KindForMode` (kiwi_conselect.cpp) folded Face and Object into the
whole-object arm, and its comment argued that answering *"…except mode 3"* would
be answering half the report. That was right at the time and is wrong now — the
report has been made from the other side. **The modes are a FILTER, and a filter
that never excludes anything is not one.**

    mode 3 (mask == SEL_MASK_FACE  ) — brush faces and construction REGION faces.
            A construction LINE is not a face; that half of shakeout F's sentence
            was always true, it was just being used to argue the opposite.
    mode 4 (mask == SEL_MASK_OBJECT) — brush / entity objects.  Nothing else,
            which is the whole content of "brush-only".  Region faces go too.

**EXACT EQUALITY, NOT A BIT TEST**, and this is the load-bearing detail: mode 5
(Everything) has the OBJECT and FACE bits set as well, and mode 5's entire job is
not filtering. A bit test would silence construction in mode 5 as collateral. That
is also why this is not folded into `KindForMode`'s priority chain — that chain
answers *which granularity* and this answers *whether at all*; two questions, two
predicates (`ModeAllowsConstructionLines`), gating the two entry points
`KiwiConSel_PickAt` and `KiwiConSel_ApplyRect`. The region half is a matching gate
on ClickSelect's region arm (kiwi_boxselect.cpp), which had no mode gate at all.

**AND THIS RETIRES THE 14-PIXEL LINE-BEATS-FACE RULE IN THOSE MODES BY
CONSTRUCTION.** `KCON_CLICK_PIXELS` (kiwi_construct.h:233) is only reachable
through `PickAt`, and ClickSelect's arbitration (`brushPointish` — an area hit
always loses to a construction line) only runs when `PickAt` answered. With
`PickAt` refusing outright in modes 3 and 4, a line can no longer beat the face it
is drawn on top of there, and the rule keeps working unchanged everywhere it was
wanted. **No constant changed.**

`ApplyRect`'s refusal returns **before** its own `s_sel.clear()`: a face- or
object-mode marquee is a statement about brushes and has no opinion about the
construction selection, so it must not silently drop one the user made in another
mode. The mode chip tooltips now enumerate what each mode picks, because a mode
that silently refuses a click is a mode the user has to guess at.

### 57.7 The array takes a line, and it takes the pivot (ITEM 8)

**USER REPORT, verbatim:** *"The linear array tool needs to accept a line to go
across. Doing it by hand is really hard and pivot (V) support needs to be there as
well."*

**PLASTICITY HAS AN ARRAY, AND IT ANSWERS THE DESIGN QUESTION.**
`src/commands/array/` — `ArrayFactory.ts`, `RectangularArrayCommand.ts`,
`RadialArrayCommand.ts`. Their rectangular array prompts **"Select endpoint 1"**
and turns the picked point into the SPAN:

    step1.copy( point ).sub( centroid );
    array.step1 = step1.length() / ( array.num1 - 1 );      // :39-40

`distance1` is the stored concept and spacing is derived from it —
`step1 = distance1 / ( num1 - 1 )` (ArrayFactory.ts:143-146) — and
`RectangularArrayFactory.mode` **defaults to `'extent'`** (:120), in which changing
the count re-derives the step to PRESERVE the span (:130-134). Their `'spacing'`
mode is the non-default. So: **the count decides the spacing and the copies span
the picked extent**, which is also the natural reading of "a line to go *across*"
and the only reading the tool's single existing editable field (`count`) already
supports. The derived spacing is surfaced rather than hidden — a read-only
`spacing` field in the bubble, and the HUD.

`RectangularArrayCommand.ts:31-32` also confirms their array measures from the
**centroid** (`bbox.getCenter`), which is precisely the thing the pivot replaces.
`Begin()` now asks `KiwiXform_PivotOverride( m_ref )` first and falls back to
`Select_GetMid`. There is no new `V` gesture and there must not be: the pivot is a
SESSION pivot whose reset rule is the selection signature
(`PivotActive`, kiwi_transform.cpp), so placing it with `V` inside Move and then
running the array already carries it across — and the placement machinery is
file-local to kiwi_transform.cpp, so a second copy would be a second pivot.

For the RADIAL array the pivot IS the rotation centre, which makes "revolve these
around *that* corner" expressible for the first time. For the LINEAR array it is
the origin the step is measured from, and with a line picked it decides which END
the array runs away from — a construction segment has no inherent direction and the
store's point order is an artifact of how it was drawn, so the endpoint nearer the
reference point is taken as the origin rather than reversing the array half the
time.

**THE PRESS IS TAKEN WITH `PressIntercept`, NOT `WantsClicks`.** The brief said
`WantsClicks`; the framework says otherwise, and this is worth recording because it
looks like the obvious answer. `KiwiCmd_MouseButton` takes the multi-click branch
**above** the HOT→PAUSED arm, so opting in would have silently deleted the linear
array's park / resume grammar — i.e. it would have broken "keep the existing
hand-drag mode exactly as it is" while implementing it. `PressIntercept` is the
first rung, above the resume arm, with the press pixel already latched, and
kiwi_transform.cpp already wrote the argument out for the movable pivot. The veto
is CONDITIONAL: the press is consumed only when a construction segment is actually
under it, so a press on empty space falls through unchanged and a click can never
confirm the array.

**AND DRAGGING RELEASES THE LINE THROUGH A DEADZONE, NOT ON THE NEXT MOUSE-MOVE.**
The linear array has no grab gate — `Recompute()` maps the cursor on every
`MouseMove`, pressed or not — so "dragging" and "moving the mouse" are the same
event here and a raw release would make line mode last until the next twitch. The
release is measured from the PICK pixel (`KARR_LINE_BREAK_PIXELS`), it prints, and
on release the drag start is REBASED rather than re-latched so the step does not
collapse to zero on the release frame.

### 57.8 The difference tool takes many tools (ITEM 9)

**USER REPORT, verbatim:** *"the difference command needs to accept multiple
shift-clicked brushes. For example, if I'm making a sidewalk and I want to boolean
each crack, I have to do it 1 by 1 current, that's awful. Also support shift box
select."*

`m_tool` became `std::vector<selbrush_t*> m_tools`. Plain click replaces the set,
**Shift+click appends**, Shift+click on a brush already in the set removes it, and
the stage drops back to the pick stage when the set empties. `WantsClicks()` is
now true in BOTH stages, so the preview is live from the first tool while more can
still be added. Esc pops the LAST tool while more than one remains, so the final
Esc lands in exactly the state the old single Esc did.

**THE CARVE IS A CASCADE, NOT A UNION.** `CarveTarget` splits into
`CarveByOneTool` (the old body, verbatim, except that "everything ended up inside
the tool" now returns a new `KBOOL_CONSUMED` instead of a refusal) and an outer
loop: carve the target by tool 0, feed every SURVIVING piece through tool 1, and so
on. The tools' union is never computed — computing it would be a second CSG
problem with its own failure modes, and the cascade gets the same answer out of the
N-plane subtract that already exists. The ownership invariant is stated in the code
(`!owned ⟺ cur == { target }`): the borrowed target is never freed, every clone is.

**ALL-OR-NOTHING PER TARGET IS PRESERVED, AND THE REFUSAL POINT MOVED BY ONE
LEVEL.** Any `REFUSED` at any stage frees that target's whole working set and
leaves the target untouched; a `MISS` leaves that piece alone. **A piece swallowed
by a LATER tool is not a refusal** — only a target ending with ZERO pieces is,
which for a single tool is byte-for-byte the pre-round behaviour, message included.
Strict per-stage refusal (the literal reading of the brief) would reject legitimate
sidewalk cases where two adjacent cracks leave a thin fragment.

**ONE UNDO BRACKET, AND EVERY TOOL IS COVERED INSIDE IT.** The tools are not on
`selected_brushes` (they are picked with `PICKF_EXCLUDE_SELECTED`), so the
bracket's head never cloned them; `Undo_AddEntity` / `Undo_AddBrush` now run for
each. All tools are `Brush_Free`d **after** the piece-landing loop — round N's
ordering rationale, and it matters more with several tools, because with several
tools sharing an entity with several targets, freeing any tool early is precisely
the transient the old rule forbade.

**SHIFT+BOX-SELECT IS A NEW FRAMEWORK RUNG, and it is deliberately not
`KiwiBox_End`.** A marquee is unreachable while a command is live — the viewport
claims LMB and returns above the marquee arm. So: `KiwiEditorCommand::WantsMarquee`
/ `::Marquee` (kiwi_command.h), a `KG_CMD_MARQUEE` gesture in kiwi_viewport.cpp
that is **Shift-gated at the dispatch site** (a bare press keeps meaning exactly
what it meant, for opted-in and opted-out commands alike), and
`KiwiBox_CollectBrushes` exported from kiwi_boxselect.h — the same collector the
marquee uses, pinned to `SEL_OBJECT`, touching no selection. It uses the same
`KiwiBox_*` rect so the §12 overlay draws unchanged, but it never reaches
`KiwiBox_End`, which would clear `KiwiSel()`, re-run the click grammar and auto-
enter push/pull.

**THE CLICK RECOVERY IS LOAD-BEARING.** Claiming the press at the viewport takes
the Shift+CLICK away from `Click()`, so under `KBOX_CLICK_PIXELS` of travel the
release feeds the press straight back to `KiwiCmd_MouseButton( 0, x, y, true )`.
Without it, opting into the marquee would have cost the command its shift-click —
i.e. the other half of the same report.

**THE SHIFT FLAG IS A LATCH, NOT A WIDENED `Click()`.** `KiwiCmd_LastShift()` sits
beside the existing cursor latch. Widening the `Click()` virtual would have made
five existing overriders (matchface, construct, primitive, trim, split) silently
stop overriding it, which is the worst possible failure mode for a signature
change — silent, and in five unrelated tools.

### 57.9 Match Face on a chamfer, and Remove Face (ITEM 4)

**USER REPORT, verbatim:** *"(see pic) match face should support this operation.
You still can't delete a chamfer that was made on a brush."* — with an orange
skewed quad (a chamfer face) and the neighbouring target face outlined.

**(a) THE REFUSAL WAS NOT IN kiwi_matchface.cpp, AND IT WAS NOT ABOUT SKEW.**
There is no axiality assumption anywhere in that file: `BuildPlanePts` builds an
arbitrary right-handed in-plane basis and reproduces the target's normal and `dist`
to float precision, and the reject path is a pure RESULT gate. Taking the
neighbour's plane verbatim worked exactly as designed — and that is the problem.
It makes the two faces **coplanar**, and the trial's result gate rejected the
brush at kiwi_validity.cpp V5, *"duplicate plane"*:

    if ( d > KVALID_PLANE_DOT && fabsf( pi.dist - pj.dist ) < KVALID_PLANE_DIST )
    { *outWhy = "duplicate plane"; return false; }

**V5 IS NOT WEAKENED, AND MUST NOT BE.** A brush written to disk carrying the same
half-space twice is genuinely ambiguous to the compiler. The bug is that the trial
gated the INTERMEDIATE brush rather than the one the user asked for. A duplicated
half-space clips nothing its twin did not already clip, so the requested result is
that same geometry **with the now-redundant face dropped** — an ordinary solid, and
"delete the chamfer" arriving from the other direction, which is literally the
second sentence of the report. So Match Face detects the twin, gates the brush
MINUS the source face, and on success removes it (`Brush_RemoveFace`), rebuilds,
and drops the stale face selection. The non-coplanar path is byte-for-byte
unchanged, §20 texture lock included.

**(b) REMOVE FACE WAS UNREACHABLE IN THE ONE STATE IT NEEDS.** Round T-4 put its
rung on the key funnel **below** `if ( g_activeCommand ) return KiwiCmd_KeyDown(…)`.
In Face mode a face selection **is** a live PAUSED push/pull — kiwi_boxselect.cpp
auto-enters `KIWI_CMD_MOVE` and pauses it on the very click that makes the
selection — so `g_activeCommand` is always set in precisely the one-face state
Remove Face requires, DELETE went to `KiwiCmd_KeyDown`, and died on its catch-all
*"Everything else is SWALLOWED"*. Round T's rung could only fire in the states that
do NOT auto-enter (mode 5, an additive click, or after an Esc). **That is the
"narrow state", and it is the same trap round N found for `Ctrl+R` and round U for
`Ctrl+1..4`, one state further along.** It could not be fixed through
`PreemptVerb`: DELETE resolves through the binding table to 33003 (Delete
Selection) and never to `KIWI_CMD_REMOVE_FACE`, which is registered unbound on
purpose. The new rung sits immediately ABOVE the swallow, gated on `PreemptIdle()`
(an unmoved auto-entered gesture, so the cancel is provably record-free and the
selection survives) and on `!KiwiConSel_OwnsDelete()` so the construction selection
still owns DELETE first.

**"DOES THE SOLID STILL CLOSE" NEEDED A NEW TEST, AND HERE IS WHY V1..V7 COULD NOT
ANSWER IT.** They are all read off what `Brush_BuildWindings` produced, and
**`Brush_BuildWindings` has no world box**: `CM_ForEachBrushPlaneIntersection`
collects triple-plane intersection points lying inside every other half-space, and
`def->[mins,maxs]` is the box of *those points*. An open cell still has vertices —
a box with its lid removed still has its four floor corners — so its bounds are
finite and V7 passes on a solid that runs away to infinity. (V3 catches most opens
by accident: the side faces of a lidless box keep only two intersection points
each, so their windings come back NULL. "Most" is not a decision procedure, and
these two verbs' whole job is to remove a bounding plane.)

`KiwiValid_BrushCloses` (V8) gives the def a **probe box** — its own bounds grown
by `KVALID_CLOSE_MARGIN` — and rebuilds each face with the ported
`Brush_MakeFaceWinding` (brush.cpp:4684), which is `Winding_BaseForPlane` over the
bounds clipped behind every other face plane, i.e. **the world-box clip the port
already has**. A closed brush's faces are its real faces and clear the probe box by
a whole margin; an open cell's faces are clipped BY the box, so a winding point
lands on it within `KVALID_CLOSE_TOUCH` — **and that point is the leak.** Bounds
are saved and restored on every exit path, and the windings are caller-owned copies
freed with `Winding_Free`.

It is a **separate entry point rather than a V8 inside `KiwiValid_CheckBrush`**:
every per-frame §20/§21/§22 drag calls `CheckBrush`, none of them can remove a
half-space, and none should pay faceCount² winding clips for a question they cannot
raise. `KiwiValid_CheckBrushIgnoringFace` (V1..V6 over the face set minus one, with
V1's four-half-space floor counted AFTER the exclusion) keeps V1..V7 in exactly one
copy — `KiwiValid_CheckBrush` is now a one-line forwarder.

**MATERIAL AND TEXTURE LOCK ON THE SURVIVORS NEEDED NOTHING, AND THAT IS NOW
WRITTEN DOWN AT BOTH SITES.** `Brush_RemoveFace` (brush.cpp:332) memmoves whole
232-byte `face_t` records, so every surviving plane keeps its planepts, all four
`MaterialDef` layers, its contents and its toolflags. No surviving PLANE moves, so
there is no texdef to re-project; the neighbours only get larger WINDINGS, and a
texdef is anchored to the plane.

### 57.10 Decision log — round AA

- **D-AA1 — the `$default` restore is a byte COPY, not a replay of the seed's
  calls.** The alternative — keeping `SetMaterial` and trying to undo its browser
  side effects afterwards — is a second thing to keep in step with a ported
  function whose reach is the whole texture-window subsystem. A snapshot has no
  reach at all: it makes no call, so there is no side effect to reason about, and
  it is *more* faithful to "restore what step 6a-pre produced" than re-deriving it
  ever was. (§57.1)
- **D-AA2 — `Texture_ShowAll` is asked AGAIN after the scale pass, rather than
  moved.** Moving it would shift where the ported boot sits relative to
  mainfrm.cpp:1633 for no benefit; asking twice is idempotent and makes the boot
  state unconditional instead of order-dependent. The narrowing paths a USER
  invokes — Show In Use, a map load — are untouched. (§57.1)
- **D-AA3 — the eye-orient is PER TRIANGLE, not per polygon.** Per-polygon is
  cheaper and is enough for a flat fill, but it cannot express a closed volume: a
  prism preview or a boolean operand needs its near faces and its far faces wound
  oppositely *in the same batch*. One rule that covers both is worth more than a
  faster rule that needs a second rule beside it — and per-triangle additionally
  makes the "far side simply shows through" claim in kiwi_extrude.cpp true for the
  first time. (§57.2)
- **D-AA4 — the chain latch is a PROJECTION at the point of use, not a looser
  region test.** Widening `KVALID_PLANE_DOT` would take a genuinely skew ring and
  pretend it was flat; everything downstream (the extrude prism, the brush the
  region becomes) would then be built off a plane that does not contain its own
  outline. Tolerant acceptance was the stated fallback; it was not needed. (§57.3)
- **D-AA5 — the Z lock stays the ONLY escape from the latch.** A modifier that
  temporarily released the plane would be a second grammar for a gesture that
  already has one, and the Z lock is a better answer to the same need (it names an
  axis instead of removing a constraint). The cost — a free 3D polyline now needs
  Z per rising segment — is real and is recorded in KNOWN_ISSUES. (§57.3)
- **D-AA6 — no hysteresis was added, and that is a finding, not an omission.** The
  rung chain runs once per gesture, so nothing oscillates; Plasticity has none
  either and does not need one, because it never auto-derives from hover at all.
  Adding a damping band would have been machinery around a non-event, and it would
  have hidden the real instability, which was one layer down in the POINTS and not
  in the plane. (§57.3)
- **D-AA7 — the loop-closing snap outranks everything, deviating from Plasticity's
  flat priority 1.** Their close test is geometric and forgiving
  (CurveFactory.ts:114-116), so losing the snap race costs them nothing; KIWI's is
  a pixel box on the cursor, so losing it stores a point that is not the first one
  and silently costs the user the region. Different close tests, different right
  answers. (§57.4)
- **D-AA8 — the split tool is NOT converted to `KiwiSnap_AxisDepth`, against the
  directive's explicit request.** Its reduction axis lies IN the face being split,
  so every SNAP_FACE candidate there is near-parallel by construction and would be
  refused; the tool already excludes SNAP_FACE and already tests plane membership
  the right way. Doing what was asked would have replaced a correct answer with no
  answer. Reported rather than done. (§57.5)
- **D-AA9 — the mode gates test EXACT mask equality.** A bit test is the obvious
  spelling and is wrong: mode 5 carries the OBJECT and FACE bits and must keep
  picking everything, so a bit test would silence construction geometry in the one
  mode whose job is not to filter. (§57.6)
- **D-AA10 — the 14 px line-beats-face rule is retired in modes 3 and 4 by making
  the PICK refuse, not by changing the rule.** The rule is correct where it was
  designed to apply (a line drawn on a face must be reachable); it only ever
  misbehaved in modes that should not have been offering lines at all. Gating the
  entry point fixes the reported bug and leaves `KCON_CLICK_PIXELS` alone. (§57.6)
- **D-AA11 — the array's count decides the spacing, and the copies SPAN the picked
  line.** This is Plasticity's default `'extent'` mode (ArrayFactory.ts:119-146,
  RectangularArrayCommand.ts:35-47), it is the only reading the tool's single
  existing editable field supports, and it is what "a line to go across" says. The
  derived spacing is surfaced as a read-only field rather than hidden, so the
  alternative question ("what spacing did that give me?") is still answered. (§57.7)
- **D-AA12 — the array takes the press with `PressIntercept`, not `WantsClicks`.**
  `WantsClicks` is taken ABOVE the HOT→PAUSED arm, so opting in would have deleted
  the array's park/resume grammar — it would have broken "keep the hand-drag mode
  exactly as it is" while implementing it. The intercept is conditional (only when
  a construction segment is actually under the press), so a press on empty space
  still falls through unchanged and a click can never confirm the array. (§57.7)
- **D-AA13 — the multi-tool boolean CASCADES rather than unioning its tools.**
  Computing the union of N tool brushes first would be a second CSG problem with
  its own refusal modes, sitting in front of the one that already works. The
  cascade reuses the N-plane subtract unchanged and reaches the same answer. Its
  one visible consequence — a piece swallowed by a LATER tool is not a refusal,
  only a target ending with zero pieces is — is what makes the reported sidewalk
  case work at all, and for a single tool it is byte-for-byte the old behaviour.
  (§57.8)
- **D-AA14 — the command marquee is a new gesture that never reaches
  `KiwiBox_End`, and it is Shift-gated at the DISPATCH site.** Reaching
  `KiwiBox_End` would clear `KiwiSel()`, re-run the click grammar and auto-enter
  push/pull — a marquee lent to a command is not a selection act. Gating on Shift
  at the dispatch site rather than inside the command means a bare press keeps
  meaning exactly what it meant for every command in the editor, opted-in or not,
  and the under-threshold click is fed BACK to `KiwiCmd_MouseButton` so opting in
  does not cost the command its shift-click. (§57.8)
- **D-AA15 — V5 ("duplicate plane") is not weakened; the TRIAL asks about the right
  brush.** A brush on disk carrying the same half-space twice is genuinely
  ambiguous to the compiler, so the check stays exactly as strict. What changed is
  that Match Face now recognises "the user asked for a coplanar twin" as meaning
  "…and therefore for the redundant face to go", and gates the brush MINUS that
  face. The alternative — accepting the duplicate — would have written the
  ambiguity to disk. (§57.9)
- **D-AA16 — "does it still close" is a SEPARATE validity entry point, not a V8
  inside `KiwiValid_CheckBrush`.** Every per-frame §20/§21/§22 drag calls
  `CheckBrush`, none of them can remove a half-space, and none should pay
  faceCount² winding clips for a question they cannot raise. The two verbs that CAN
  raise it are one-shot clicks and can afford it. (§57.9)
- **D-AA17 — the closure test borrows the port's OWN world-box clip rather than
  inventing a containment proof.** `Brush_MakeFaceWinding` over the def's grown
  bounds is exactly what the editor already uses to turn planes into faces; if a
  face's winding TOUCHES that box, the solid leaks there, and the touching point
  names where. Writing a fresh convex-closure proof would be new geometry code
  answering a question the ported code already answers as a side effect. (§57.9)

## 58. Round AB — the monitor-sleep crash, the cancelled drag, Auto Bool

### 58.1 The device came back and the editor did not (ITEM 1)

**USER REPORT, verbatim:** *"Hit this crash after my monitor went to sleep and woke
back up. Try to fix it."*

    RB_EndSurfacePrologue()              rb_shade.cpp:202
    RB_EndTessSurface()                  rb_shade.cpp:189
    RB_SetMaterialColorCmd( … )          rb_backend.cpp:1464
    RB_ExecuteRenderCommandsLoop( … )    rb_backend.cpp:2736
    RB_CallExecuteRenderCommands()       rb_backend.cpp:2820
    R_IssueRenderCommands( … )           r_rendercmds.cpp:298
    CamWnd_RenderToRT( w, h )            camwnd.cpp:4422
    ImGuiShell_RenderViewportsToRT()     imgui_shell.cpp:757
    Radiant_RunMessageLoop()             radiant_main.cpp:801

**WHAT IS NULL, EXACTLY.** `rb_shade.cpp:202` is
`g_primStats->dynamicIndexCount += tess.indexCount;`. There are two operands and
both are file-scope globals. `tess` is a `materialCommands_t` object
(rb_backend.cpp:78), never a pointer. `g_primStats` is only ever `0` or
`&g_viewStats->primStats[target]` — a static address (rb_stats.cpp:69-75). **So the
fault can only be `g_primStats == NULL`**, and the state that produces it is
`tess.indexCount != 0` while `g_primStats == 0`. The `iassert(g_primStats)` one line
above DID fire — `iassert` is non-fatal in this build (assertive.cpp logs to
`%TEMP%\radiant_firstlight.log` and falls through), so it wrote a log line and then
let the store run. An access violation is not a `Com_Error`, so
`Kiwi_FatalRescue` (engine_stubs.cpp:275) never ran and the map was not rescued
either.

**EXACTLY ONE PATH PRODUCES THAT PAIR.** The editor's own surf-cache handler
`RB_DrawEditorSkinnedCached_Sub` (r_ed_scene.cpp:704) drives `tess` itself and
**never calls `R_TrackPrims`**, so `g_primStats` is 0 for its whole duration — that
is fine, because it always flushes its own batch before returning. Always, except:
the final flush is `if ( haveBatch && boundVb )` (r_ed_scene.cpp:805). An
`ED_SURF_MESH` surf whose vertex buffer resolves to **NULL** takes the model arm
(`VERTDECL_PACKED`, `boundVb = 0`) while its indices are still copied into `tess`
(:787-789) — so the flush is skipped and **`tess.indexCount` escapes the command
handler**. The next `RC_SET_MATERIAL_COLOR` — and camwnd.cpp emits one per brush,
per face, per entity — hits `if ( tess.indexCount ) RB_EndTessSurface();`
(rb_backend.cpp:1463) and dies.

**AND A NULL VERTEX BUFFER AFTER A WAKE IS GUARANTEED.** The wake sequence, in
order:

    1.  monitor sleeps.  The D3D9 device goes lost.  Nothing paints, nothing notices.
    2.  monitor wakes.  Frames resume; the compositing WM_PAINT's
        R_SetupRendertarget_CheckDevice -> R_TestDevice observes the loss.
    3.  ROUND V's machinery does its job: R_RecoverLostDevice -> R_ResetDevice ->
        R_ReleaseForShutdownOrReset.  Its tail calls Editor_VB_ReleaseForReset
        (r_ed_vertbuf.cpp:125), which Releases and NULLs every editorGlobals.vb[],
        sets vbCount = 0 and frees every pool.  It has to: they are D3DPOOL_DEFAULT
        and Reset() fails otherwise.
    4.  Reset() SUCCEEDS.  The round-V tail (r_init.cpp:4487) rebuilds the per-window
        swap chains.  The device is healthy.  Everything is fine — except:
    5.  NOTHING invalidated the per-face and per-patch vertHandles cached in the
        editor surf cache (brush.cpp:2676, pmesh.cpp:9768).  The faceVis rebuild
        triggers only on `b->version != b->def->version` (brush.cpp:200), and a
        device reset does not touch a brush version.
    6.  the first healthy frame re-emits those stale handles.  Editor_VB_ForBuffer
        (r_ed_vertbuf.cpp:75) was an unchecked `return vb[buffer-1];` -> NULL.
    7.  -> step "EXACTLY ONE PATH" above.  Crash.

The comment at r_init.cpp:4356 asserting *"faceVis visCount stays 0 in all build
modes, so nothing dangles"* was true when the surf cache was device-gated off.
`Radiant_FaceVisGpuReady()` is `dx.device != nullptr` (camwnd.cpp:1130). It has not
been true for a long time.

**THE FIX IS THAT THE CACHE DIES WITH THE POOL, IN THE SAME BREATH.**
`KiwiDevice_InvalidateEditorSurfCache` (kiwi_devicereset.cpp) walks the three
display-list sentinels and drops every instance's cached state, immediately before
`Editor_VB_ReleaseForReset`. It must NOT hand the handles back: `Visuals_VisArray`
(brush.cpp:1793) would call `R_Ed_FreeVertices` on each one, manufacturing
free-slots inside pools for D3D9 buffers that are about to stop existing, and
`Editor_VB_GetHandle` would later hand one of them out. **The binary already has the
drop-don't-return primitive** — `Brush_InvalidateVis` (brush.cpp:1478, 0x478340)
frees `b->faces` outright, routes a patch through `PMESH_22_Indices` (pmesh.cpp:9826,
the deliberate no-`R_Ed_FreeVertices` twin) and sets `version = def->version - 1`,
arming the rebuild. The only thing added around it is a loop freeing the per-face
`visArray` blocks, which 0x478340 leaks.

**AND THE REBUILD MUST NOT RUN WHILE THE DEVICE IS STILL DOWN.** With every brush
armed, the first draw after a loss would walk into `Editor_VB_Upload` ->
`Editor_CreateAdditionalVertexBuffer`, whose `CreateVertexBuffer` failure is a
`FatalError` — and `CreateVertexBuffer` on a lost device fails by definition. So
`Radiant_FaceVisGpuReady` also refuses while `dx.deviceLost`. That created its own
trap, and it is the interesting one: `Brush_MakeFaceVisuals` syncs
`b->version = b->def->version` **unconditionally** at its tail, so a rebuild skipped
for want of a device would mark the brush "cached" with an empty cache and nothing
would ever rebuild it — one monitor sleep and the map renders untextured forever.
The two reasons `Radiant_FaceVisGpuReady` says no are not the same reason, and now
they are told apart (`Radiant_FaceVisDeviceLost`, camwnd.cpp): NO DEVICE is a
headless gate and syncs faithfully; DEVICE LOST is transient and leaves the version
armed.

**THREE MORE GUARDS, EACH BECAUSE THE CHAIN SHOULD NOT DEPEND ON ONE FIX.**

* `Editor_VB_ForBuffer` bounds-checks (`buffer == 0 || buffer > vbCount -> NULL`).
  A zero handle used to read `vb[-1]`.
* `RB_DrawEditorSkinnedCached_Sub` **skips** a MESH surf with no VB rather than
  falling through it. A mesh with no vertex buffer has nothing to draw, so skipping
  is both the safe and the correct answer — and it is what makes the tess leak
  structurally impossible regardless of cause.
* `RB_EndSurfacePrologue` returns early on a NULL `g_primStats`. The stats are a
  diagnostic; the draw is not. Losing a frame's prim counters beats losing the map.

**THE FRAME LOOP ALSO STOPS RENDERING WHILE THE DEVICE IS LOST**, which round V
did not do. `RTT_DeviceHealthy()` (radiant_rtt.cpp) hoists `RTT_Begin`'s own
three-part test so `ImGuiShell_RenderViewportsToRT` can skip the WHOLE viewport pass
as one decision. Two reasons it is worth hoisting: one `TestCooperativeLevel` per
frame instead of four, and a PARTIALLY rendered pass is worse than none (the camera
succeeds, the XY fails, and three stale RT images composite against one fresh one).
The white-flicker guard `ImGuiShell_FrameAuthorized` already tolerates a tick with no
scene render, so skipping costs nothing. **This composes with round V rather than
replacing it**: recovery still happens on the compositing WM_PAINT's
`R_SetupRendertarget_CheckDevice` (radiant_main.cpp:197), whose active render target
is the window backbuffer — the path that already works — and the recreate side stays
exactly where round V put it (r_init.cpp:4487 and :4803). What this stops is
`R_IssueRenderCommands`' own `R_CheckLostDevice` (r_rendercmds.cpp:278) running the
full reset cascade from INSIDE the RTT bracket, i.e. with an app-created
D3DPOOL_DEFAULT surface bound and a half-built command list referencing a pool that
`R_ReleaseForShutdownOrReset` is in the middle of destroying.

The RTT textures themselves needed nothing: `RTT_ReleaseForReset` NULLs `tex`, and
`EnsureSlot` (radiant_rtt.cpp:34) re-creates lazily on `tex == NULL`. That
recreate-on-demand was already correct and is the model the surf cache now follows.

### 58.2 The drag that was cancelled for finishing (ITEM 2)

**USER REPORT, verbatim:** *"sometimes when using a tool and then dragging your mouse
off, it just resets the operation. it's super annoying. it just says 'move selection
undone.' or something similar to in console and snaps back to where it was."*

That console line is `Sys_Printf( "%s undone.\n", … )` (undo.cpp:984), i.e. a REAL
`Undo_Undo`, and `"move selection"` is the literal `OpenUndo` passes at
kiwi_transform.cpp:2547. **The chain that reaches it without a keystroke is exactly
one:**

    ImGuiShell_ForceReleaseIfStuck      imgui_shell.cpp:434   (round Z)
      -> ImGuiShell_AbortViewportInput  imgui_shell.cpp:405
         -> KiwiVP_CameraAbort          kiwi_viewport.cpp:893
            -> KiwiGizmo_Abort          kiwi_gizmo.cpp:685
               -> KiwiCmd_Cancel        kiwi_command.cpp:1262
                  -> KiwiCmd_UndoCancel kiwi_command.cpp:2039
                     -> Undo_Undo       "move selection undone."

**THE TRIGGER WAS RIGHT AND THE ACTION WAS WRONG.** The guard fires when "this
viewport owns a drag AND no physical mouse button is down" (`GetAsyncKeyState`,
which reads the true state regardless of focus). That is not an ambiguous state:
**the user has let go.** The release EDGE simply never reached ImGui, because the
`WM_*BUTTONUP` went somewhere else — the drag left the client area and capture was
dropped (`WM_CANCELMODE`, another process taking capture, an alt-tab), or the app
lost the foreground while the cursor was outside. Round Z wrote the guard for the
gestures that have no result to keep (the XY RMB pan whose `m_nButtonstate` wedges,
the camera free-look that leaves the cursor hidden), where "tear it down" IS the
clean answer. For a MODAL EDIT it throws away a drag the user finished — and prints
an undo line for it.

The brief's other suspects were audited and are innocent:

| suspect | verdict |
|---|---|
| hover loss (`s_hovered[]` false) | INNOCENT. `ImGuiShell_ViewportInput`'s `owns` branch (imgui_shell.cpp:371) never reads `hovered`; a drag that leaves the image cell keeps being dispatched. That is round-one software capture and it works. |
| `ImGuiShell_ForceReleaseIfStuck` during a drag | INNOCENT AS PREDICTED **while the button is down** — it requires `!anyDown`. It is guilty only at the instant the button comes up, which is the whole finding. |
| `s_frameLive` (round Y) | CONTRIBUTING, not causal. The `!s_frameLive` early-out (imgui_shell.cpp:470) calls the guard *before* any dispatch, so a skipped frame on the release tick reaches the wrong action one tick sooner. With the action fixed it now commits instead. |
| legacy `SetCapture`/`ReleaseCapture` | NOT IN THIS PATH. The RTT input path is software-captured (imgui_shell.cpp:157-159); the OS capture the ported handlers used is neutered, and `ImGuiShell_AbortViewportInput` already routes the camera around `VP_Up` because `CamWnd_OnRButtonUp` pops a context menu. |
| a synthesized Esc on focus/hover loss | DOES NOT EXIST. `ImGuiShell_AbortViewportInput` has exactly one caller. |

**SO THE GUARD NOW SYNTHESIZES THE RELEASE.** `ImGuiShell_ReleaseViewportInput`
dispatches the same up-handlers a release inside the image would have run
(`KiwiVP_CameraButtonUp` for the camera, `VP_Up` for XY/Z/texture), at the **real
cursor position**, and feeds `io.AddMouseButtonEvent( b, false )` so ImGui's own
`MouseDown[]` — stuck true because the up message never arrived — cannot swallow the
next genuine press. Abort survives as the fallback for the one case that is
genuinely not a release: a live drag with no button left to release.

**AND THE CURSOR IS NEVER LOST WHILE A DRAG IS LIVE.** `WM_MOUSELEAVE` makes the
backend post `io.AddMousePosEvent( -FLT_MAX, -FLT_MAX )` (imgui_impl_win32.cpp:789),
and its `GetCursorPos` fallback (:389-398) only re-supplies a position while the app
is FOREGROUND. Drag off, let something else take the foreground, and `io.MousePos`
stays at `-FLT_MAX`: the image-space subtraction then feeds the gesture garbage.
While a viewport OWNS the drag and `ImGui::IsMousePosValid()` is false, the position
now comes from `GetCursorPos` + `ScreenToClient`, **unclamped** — negative and
overflowing coordinates are correct, because a drag that has left the cell is still
pointing somewhere and every consumer is a ray/plane projection over the whole
plane, not an array index. It is deliberately *only* the fallback: inside the window
`io.MousePos` is the authoritative event-synced value, and the XY RMB pan
re-centres the cursor every move, so preferring the physical position
unconditionally would fight it. (Multi-viewport is off in this shell, so ImGui
screen space IS the backend window's client space and the mapping is one
`ScreenToClient`.)

**THIS IS PLASTICITY'S RULE, AND IT IS EXPLICIT IN TWO PLACES.**

`ViewportControl.onPointerDown` (plasticity/src/components/viewport/ViewportControl.ts:95)
does not listen on the canvas for the duration of a drag — **it listens on
`document`**:

    document.addEventListener( 'pointerup',   this.onPointerUp   );   // :111
    document.addEventListener( 'pointermove', this.onPointerMove );   // :112

and only unregisters them through the gesture's own `Disposable` (:113-114). So once
a drag starts, every move and the release are heard document-wide and the cursor
leaving the viewport element changes nothing. `onPointerMove`'s `'dragging'` arm
(:167-169) is a bare `continueDrag( moveEvent, … )` with **no bounds test, no hover
test and no timeout**, and `onPointerUp`'s `'dragging'` arm (:210-216) is a bare
`endDrag(…)` — the gesture's ordinary, committing tail. There is no cancel anywhere
in that state machine; cancellation is reserved for an explicit `escape`
(KeyboardEventManager) or the command's own `cancel()`.

`OrbitControls` — the navigation half — uses the browser's real pointer capture for
the same reason: `domElement.setPointerCapture( event.pointerId )` on the first
pointer down (OrbitControls.ts:291) and `releasePointerCapture` when the last one
goes up (:329).

KIWI reaches the same place without the browser's help: software capture for the
moves (already there, imgui_shell.cpp:157-159), the OS cursor for the position when
ImGui's is meaningless, and a synthesized release for the edge Windows delivered
elsewhere.

### 58.3 Auto Bool (ITEM 3)

**USER REQUEST, verbatim:** *"You should add an 'auto bool' that attempts to
consolidate a large group of brushes all at once to reduce brush-count. Might be a
bad idea, but we can try it."*

**NO GEOMETRY WAS WRITTEN.** `Brush_MergeList` (csg.cpp:409, 0x47D600) is the ported
core and it is the only thing in the feature that looks at planes: classify each
face INNER (another brush in the set carries a flipped-equal plane) or OUTER, reject
the set if any pair of OUTER faces is `Winding_PlanesConcave` (csg.cpp:478-487,
winding.cpp:246), build one brush from the outer planes and skip duplicates, return
`nullptr` when the union would not be convex. Materials come from
`Face_Alloc( newBrush, face1 )` per surviving outer face (csg.cpp:541) — i.e.
exactly what a manual Selection -> CSG -> Merge already gives. None of it is
reimplemented, second-guessed or tuned.

**IT CALLS THE CORE, NOT THE COMMAND, AND THE REASON IS UNDO.** `CSG_Merge`
(csg.cpp:572) takes no arguments and merges *everything* on `selected_brushes` —
which is why kiwi_join.cpp and kiwi_boolean.cpp drive it by rewriting the selection
and dispatching the classic id 32927. Auto Bool cannot: that id's handler
(mainfrm.cpp:2408) opens and closes its OWN bracket with its own `Undo_ClearRedo`,
so a greedy run through it would be N undo records for one gesture and would destroy
redo N times. **One gesture, one undo record** is not negotiable, so the pair
surgery around the core — validate, unlink both into a NULL-terminated merge list
threaded through their own `.next`, call the core, then either `Brush_Free` both and
`Brush_AddToList2` the result or `Brush_AddToList2` both back — is transcribed from
`CSG_Merge`'s body (csg.cpp:605-648) with the loop bounded to two. Including the
**prepend direction**, so the merge list's head is the LATER brush exactly as it is
there. The second benefit is silence: `CSG_Merge` prints five possible lines per
attempt and a run makes hundreds of attempts.

**THE BRACKET NEEDS NO MANUAL COVERS, AND THAT IS WORTH WRITING DOWN** because
kiwi_boolean.cpp's carve DOES need them (kiwi_boolean.cpp:970-999).
`KiwiCmd_UndoBegin`'s `Undo_AddBrushList( &selected_brushes )` deep-clones every
original, which is exactly the set this run consumes — nothing here is freed while
OFF the list the bracket head walked. An INTERMEDIATE (merged in pass *k*, consumed
in pass *k+1*) is created and destroyed inside the record and did not exist before
it, so there is nothing to restore. A FINAL merged brush is on `selected_brushes` at
commit, so `Undo_EndBrushList` stamps it with the record id and the undo removes it
(undo.cpp:576-593).

**THE PREFILTER IS THE TOUCHING TEST, NOT AN OVERLAP TEST.** The pairs the core
accepts are the ones that share a face — they ABUT — and a strict bounds-overlap
test rejects every one of them. So the gate is the ported `Select_Touching_R`
epsilon form (select.cpp:1678-1679, `KSELX_TOUCH_EPS`), read off `def->[mins,maxs]`
which `Brush_BuildWindings` keeps current.

**THE LOOP IS ONE O(n^2) SWEEP PER PASS, REPEATED WHILE ANYTHING CHANGED.** A
successful merge does not restart the sweep — `a` becomes the merged brush and the
inner loop carries on — so a run of collinear boxes collapses inside a single pass
instead of costing one pass each; the outer repeat exists only because a merge can
make an EARLIER pair viable that was refused before it. `Sel_BrushLive` (rule 11) is
swept once per pass into a bitmap rather than called per compare: it is a linear walk
of the whole map's display lists and calling it inside the inner loop would make the
run O(n^2 * mapsize).

**IT IS STRICTLY OPT-IN AND UNBOUND.** Registered with `vk 0` like
`KIWI_CMD_MATINFO` and `KIWI_CMD_FILLET_EDGE`; reachable by name from the palette
and by the "Auto Bool" button in the CSG panel, and by nothing else. The user's own
framing was *"might be a bad idea"*: a verb that rewrites a whole selection's
topology does not get a key it can be hit by accident, and it is never chained onto
any other command.

**THE LIMITS ARE REAL AND ARE IN KNOWN_ISSUES.** Convex pair union only (not a
simplifier — no face is deleted, no plane is moved, so the volume cannot change);
greedy, therefore order-dependent; no cross-entity merging, no patches, no
fixed-size entity brushes (`CSG_Merge`'s own refusals, reproduced rather than
relaxed); the merged brush takes `g_activeLayer_string` and the head's owner
(csg.cpp:548-555, inside the core).

**THE CORE'S PRECONDITIONS DID NOT FORCE A SCOPE-DOWN.** It was worth checking, and
the answer is: `Brush_MergeList` has no axis-alignment restriction and no
same-material restriction — the test is purely plane/winding convexity, so a cascade
over arbitrary convex solids is safe. The one precondition that DOES shape the design
is that a face is classified INNER whenever any *other brush in the set* carries a
flipped-equal plane, without checking that the two faces geometrically touch. Over a
large set that is a way to get a result nobody drew; over a PAIR it is the intended
meaning of "shared face". Merging two at a time is therefore not only the simple
choice, it is the safe one.

### 58.4 Decision log — round AB

- **D-AB1 — the surf cache is invalidated at RELEASE time, not lazily on first
  use.** A lazy "is this handle still valid" test would need an epoch, and the handle
  is a packed `(buffer << 16) | firstIndex` with no room and two ported readers
  (`Editor_GetVertexBufferAndIndex`, `Editor_VB_Upload`) that would both have to
  change. Dropping the cache in the same breath as the pool means no cached handle
  can ever outlive the pool it indexes, which is a property rather than a check.
  (§58.1)
- **D-AB2 — the drop uses `Brush_InvalidateVis`, NOT `Visuals_VisArray`.** The
  difference is the whole point: `Visuals_VisArray` RETURNS each handle to the
  per-material pool, which on this path would push free-slots into pools for D3D9
  buffers that are about to stop existing. `Brush_InvalidateVis` (0x478340) is the
  binary's own drop-don't-return primitive and it already arms the rebuild and
  already handles the patch case. (§58.1)
- **D-AB3 — "no device" and "device lost" are told apart.** They were one predicate
  and they mean opposite things: the first is permanent and must sync the version,
  the second is transient and must not. Collapsing them would have traded a crash
  for a map that renders untextured after one monitor sleep — a worse bug, because
  it looks like data loss. (§58.1)
- **D-AB4 — a MESH surf with no VB is SKIPPED, not drawn-through.** Drawing through
  it is what leaks `tess.indexCount` past the handler. Skipping is not a
  workaround: a mesh with no vertex buffer has nothing to draw, so the surf is
  meaningless, and skipping makes the leak structurally impossible for any future
  cause as well. (§58.1)
- **D-AB5 — the device gate sits at `ImGuiShell_RenderViewportsToRT`, above all four
  viewports, and does NOT replace `RTT_Begin`'s.** Above, because a partially
  rendered pass composites three stale images against one fresh one and because one
  `TestCooperativeLevel` beats four. Not instead, because the device can be lost
  between the pass gate and any one viewport's Begin, and that inner check is the
  one that keeps a reset from running with an app-created default-pool surface
  bound. (§58.1)
- **D-AB6 — the stuck-drag guard RELEASES instead of ABORTING.** Its trigger —
  "we own a drag and no physical button is down" — has exactly one meaning, and it
  is "the user let go". Round Z's abort was written for gestures with no result to
  keep and was never re-examined when modal edits started using the same ownership.
  Abort is kept only for the case with no button left to release. (§58.2)
- **D-AB7 — the physical cursor is a FALLBACK, not the source.** Preferring
  `GetCursorPos` unconditionally would fight the XY RMB pan, which re-centres the
  cursor every move and therefore depends on `io.MousePos` being the event-synced
  value. The fallback is taken on exactly the condition that makes `io.MousePos`
  meaningless (`!ImGui::IsMousePosValid()`), which is the same condition that
  produced the garbage. (§58.2)
- **D-AB8 — image coordinates are left UNCLAMPED outside the cell.** Clamping to
  the image would make a drag "stick" at the border and silently change what the
  user is pointing at. Every consumer projects a ray or a plane over the whole
  plane; negative and overflowing values are ordinary inputs there. (§58.2)
- **D-AB9 — Auto Bool calls `Brush_MergeList`, not `Radiant_ExecCommand(32927)`.**
  Every other kiwi caller of the merge dispatches the classic id precisely so there
  is ONE handler and ONE undo record. Here that reasoning inverts: the classic
  handler opens its own bracket, so dispatching it N times gives N records for one
  gesture and clears redo N times. Calling the core under one bracket is what keeps
  the rule the id was being used to keep. (§58.3)
- **D-AB10 — it merges PAIRS, never a set.** The core's INNER-face classification
  does not check that a flipped-equal plane pair actually touches, so over a large
  set it can accept a union the user did not draw. Over a pair it is exactly "these
  two share a face". Pairwise is also what makes every intermediate of the greedy
  loop a real, valid, inspectable brush. (§58.3)
- **D-AB11 — greedy, with no search and no backtracking.** A better consolidation
  exists for some inputs (merge order matters), and finding it is a search problem
  with a combinatorial cost and a result the user cannot predict. A first version
  whose every step is a merge the user could have done by hand is worth more than a
  cleverer one that surprises them. The order-dependence is documented rather than
  hidden. (§58.3)
- **D-AB12 — no hotkey, and no auto-trigger anywhere.** The request came with
  *"might be a bad idea"* attached. Palette and panel button only, so the feature
  can be evaluated without any chance of it firing when it was not asked for.
  (§58.3)

## 59. Round AD — the map load that never finished drawing

**USER REPORT, verbatim:** *"I had it hang while loading a map, we need to fix this"*
— and, after Break All in the debugger, *"screen all black, never loads. all other
threads are just nvidia."*, with the main thread parked here:

    MsgWaitForMultipleObjects( 0, nullptr, FALSE, waitMs, QS_ALLINPUT )
    Radiant_RunMessageLoop()              radiant_main.cpp:843

### 59.1 IT WAS NOT HUNG, AND THAT IS THE WHOLE DIAGNOSIS

`radiant_main.cpp:761-845` is a deterministic tick: drain the queue, drain the update
bits, and — every 1/60 s — render the viewports, authorize one ImGui frame, force one
synchronous `WM_PAINT`, then park in `MsgWaitForMultipleObjects` for **at most 16 ms**
(`:842` clamps the wait). A thread sitting in that call is not stuck; it is *between
frames*. So the editor was ticking at 60 Hz and every one of those ticks drew nothing.

Follow one tick to the end and there is exactly one gate that can produce that:

    ImGuiShell_RenderViewportsToRT()   imgui_shell.cpp:906 — returns early while
                                       !RTT_DeviceHealthy() (:920, round AB)
    ImGuiShell_BeginFrame()            authorizes this paint
    InvalidateRect + UpdateWindow      -> the frame WM_PAINT, radiant_main.cpp:163
      if ( drawScene && dx.device && R_SetupRendertarget_CheckDevice( hwnd ) )   :197

`R_SetupRendertarget_CheckDevice` (r_init.cpp:4793) was answering FALSE on every tick,
forever, and printing NOTHING. That is a silent infinite state, and the round exists
as much to make it impossible again as to fix the instance.

**EVERY WAY THAT GATE ANSWERS FALSE, AND WHETHER IT CAN LATCH FOREVER.** In source
order (r_init.cpp:4799-4836), plus the paint's own two pre-conditions:

| # | Condition | Latches? | Verdict |
|---|-----------|----------|---------|
| 0 | `!drawScene` (`ImGuiShell_FrameAuthorized`) | no | the pump authorizes every tick; only STRAY paints see false, by design (round U) |
| 1 | `!dx.device` | yes | pre-boot / post-shutdown only; not this |
| 2 | `dx.targetWindowIndex >= 0` | **YES** | a `R_Setup*` with no matching `R_CheckTargetWindow`/`RTT_End` bricks every later frame. All six brackets were read end to end and each pairs unconditionally; only a non-local exit (a `Com_Error` `longjmp`) between them could do it. NAMED in the new log rather than auto-cleared — see D-AD5 |
| 3 | `g_disableRendering` | yes, but | it is a one-way counter, and **every one of its ~90 increment sites is immediately followed by `Com_Error(ERR_FATAL)` or `FatalError`, both of which `ExitProcess`** (engine_stubs.cpp:353/747). It cannot be observed set in a living process. RULED OUT for this report |
| 4 | `!R_TestDevice()` | **YES** | the actual failure — see below |
| 5 | invalid hwnd | no | `MyAssertHandler` logs it, and it is a fixed property of the window |
| 6 | degenerate size | no | resolves on the next `WM_SIZE` |
| 7 | swap chain still NULL after `R_Hwnd_Resize` | no | only reachable while the device is healthy, and round V's deferred recreation retries it every paint |

Case 4 decomposes further, and this is where "silent forever" lives:
`R_TestDevice` (:4787) returns 0 when `R_RecoverLostDevice` returns 0, which happens
for (a) `R_CanRecoverLostDevice` false — still `D3DERR_DEVICELOST`, the correct wait;
(b) the re-entrancy guard; (c) **`R_ResetDevice` returned false**. None of the three
printed anything the operator could see: (c)'s own line was throttled to every 300th
attempt on `Com_Printf`, i.e. into a console inside a window that is black.

### 59.2 THE JACKPOT: A D3DPOOL_DEFAULT CLASS NOBODY WAS RELEASING

`Reset()` fails `D3DERR_INVALIDCALL` while ANY app-created `D3DPOOL_DEFAULT` resource
is alive or bound. Round V unbound everything and round AB completed the release list
— for every class anyone had enumerated. Here is the full audit, and the one hole:

| default-pool class | released before `Reset()` by | ok |
|---|---|---|
| per-window ADDITIONAL swap chains | `R_ReleaseForShutdownOrReset` :4285-4306 | yes |
| `gfxRenderTargets[]` colour + depth-stencil surfaces (incl. FLOAT_Z, the frame-buffer backbuffer ref) | `R_ShutdownRenderTargets` (r_rendertarget.cpp:561) | yes |
| `gfxRenderTargets[].image` (render-target GfxImages) | same, via `Image_Release` | yes |
| model-lighting images | `R_ShutdownModelLightingImage` | yes |
| static-model cache VB | `R_ShutdownStaticModelCache` | yes |
| dynamic + pre-tess vertex/index buffers | `R_DestroyDynamicBuffers` | yes |
| particle-cloud VB/IB | `R_DestroyParticleCloudBuffer` | yes |
| back-end dynamic meshes, smodel cache indices | `R_ShutdownRenderBuffers` (`g_allocateMinimalResources` is never assigned in this build, so the guard never skips it) | yes |
| event/fence/occlusion queries | :4315-4351 | yes |
| ImGui DX9 vertex/index buffers | `ImGuiShell_InvalidateDeviceObjects` | yes |
| RTT viewport textures + surfaces | `RTT_ReleaseForReset` | yes |
| editor immediate-mode VB pool | `Editor_VB_ReleaseForReset` | yes |
| zone (fastfile) geometry buffers | `DB_BeginRecoverLostDevice` — **stubbed**, but the editor loads no zones (`DB_LoadXAssets` is a stub, db_registry.cpp is not in radiant_files.cmake), so none exist | n/a |
| world VB | `R_ReleaseWorld` — the editor never loads a `GfxWorld` | n/a |
| **UNMANAGED `GfxImage`s (`category >= IMG_CATEGORY_FIRST_UNMANAGED`)** | `R_ReleaseLostImages` — **which is `DB_EnumXAssets(ASSET_TYPE_IMAGE, R_FreeLostImage, ...)`, and `DB_EnumXAssets` is a NO-OP STUB in the editor** (engine_stubs.cpp:537) | **NO** |

**AND THE EDITOR REALLY DOES CREATE THEM.** `Image_Create2DTexture_PC`
(r_image.cpp:1355-1363) passes the pool as `(_D3DPOOL)(usage == 0)` — so
`usage != 0` means **`D3DPOOL_DEFAULT`**, not managed. `Image_GetUsage`
(r_image.h:239) returns non-zero for imageFlags `0x10000` (dynamic) and `0x20000`
(render target). A **water** image is `IMG_CATEGORY_WATER` (5) and is built by
`Image_BuildWaterMap` (r_image_load_obj.cpp:309) with flags `0x10001` ->
`D3DUSAGE_DYNAMIC` -> default pool. So:

> **one water material anywhere in the map being loaded is enough to make every
> recovery `Reset()` return `D3DERR_INVALIDCALL` for the rest of the session.**

Which is exactly the report's shape: it happened DURING A MAP LOAD (heavy texture
registration is when a driver TDR is most likely, and "all other threads are just
nvidia" is what a TDR looks like from the outside), it never came back, and with no
map loaded the same alt-tab/sleep recovery had always worked — because with no map
there are no unmanaged images.

The editor's images are not in the DB at all: they live in the open-addressed
`imageGlobals.imageHashTable` (r_image.h:111 — 32768 slots, the editor size), which
`Image_Alloc` fills and `Image_FindExisting_LoadObj` (r_image_load_obj.cpp:240)
probes. That table is the enumeration the editor HAS, so
`KiwiDevice_ReleaseUnmanagedImages` / `KiwiDevice_RebuildUnmanagedImages`
(kiwi_devicereset.cpp) walk it and reproduce the engine's own per-image rules —
`R_FreeLostImage` (r_image.cpp:1024) and `R_RebuildLostImage`'s unmanaged arm
(r_image.cpp:1108-1125) — verbatim.

### 59.3 AND THE RETRY COULD NEVER HAVE FIXED IT EITHER

Round V's reset-retry latch (`s_releasedForReset`, r_init.cpp:4395) exists for a good
reason: the release cascade is destructive and NOT idempotent, so a retry "skips
straight to `Reset()`". That is right for `D3DERR_DEVICELOST` — the device is simply
not ready yet and nothing the editor does changes that.

It is exactly wrong for `D3DERR_INVALIDCALL`, which means *the device state is not
acceptable*. A retry that releases nothing and unbinds nothing gets the same answer
every tick until the process dies. Worse, the unbind block was under the same latch,
so a resource bound after the first attempt was never unbound either — and a map load
that is STILL RUNNING while the device is down keeps registering images.

So `R_ResetDevice` now remembers the failure kind (`s_lastResetHr`) and an
`INVALIDCALL` retry re-runs (a) the whole unbind block — pure `SetX(nullptr)`, safe on
any device state — and (b) a **second-chance release** limited to the classes that are
idempotent AND can have come back since: ImGui's buffers, the RTT textures, the
unmanaged images, the editor VB pool, the render targets, the swap chains. It is
deliberately NOT `R_ReleaseForShutdownOrReset` again: that function's middle
(`R_ShutdownRenderBuffers` -> `R_ShutdownDynamicMesh` -> `R_FreeGlobalVariable`) would
double-free, and those subsystems provably cannot have come back, because nothing
recreates them until `R_CreateForInitOrReset` — which only runs after a `Reset()` that
SUCCEEDED.

### 59.4 THE CHAIN IS NOT ALLOWED TO BE SILENT AGAIN

Three hooks, one line:

* `R_TestDevice` (:4791) now KEEPS the `TestCooperativeLevel` HRESULT the verbatim
  port discarded (`KiwiDevice_NoteCoopLevel`). The test itself is unchanged.
* `R_ResetDevice` hands every attempt's HRESULT and *which release pass ran* to
  `KiwiDevice_NoteResetResult`; its own `% 300` `Com_Printf` is gone.
* the frame `WM_PAINT` calls `KiwiDevice_FrameHealthWatch` — the one place that knows
  a tick rendered nothing — which prints ONE throttled (~2 s) line naming the reason
  from the table in 59.1, both HRESULTs, the attempt count and the release pass.

It goes to `OutputDebugStringA` **always**, and to the console as well. That is not
belt-and-braces: the console is inside the window that is black, and this build's
`Com_Printf` is a bare `vprintf` that can stall the main thread on an undrained stdout
(the hazard `R_ResetDevice` documents at :4460 — and which was observed live, the main
thread parked in `Com_Printf`). The debugger's output window is the only channel that
survives the exact situation the line exists for.

### 59.5 THE ESCAPE HATCH

Fifteen seconds of unbroken black is not a transient loss. At that point
`KiwiDevice_FrameHealthWatch` writes the rescue save (`Kiwi_RescueSave`, map.cpp:836)
and pops ONE message box naming the two HRESULTs and the rescue path, and telling the
operator to restart. Retries continue — the device may still come back — and the box
is one-shot.

Three things make that safe, and all three are load-bearing:
* it runs **after `::EndPaint`**. The box runs a nested message loop; inside
  `BeginPaint`/`EndPaint` the region is not yet validated and the loop would
  re-deliver `WM_PAINT` forever.
* nothing is inside a scene bracket and no render target is bound at that point —
  `R_CheckTargetWindow` has run, or the paint never started one.
* `Kiwi_RescueSave` was built for dying contexts (one-shot, SEH-wrapped, pure
  `fopen`/`fprintf`, posts no window messages, touches no editor state,
  map.cpp:790-802), so a *live* context is strictly easier for it.

### 59.6 Decision log — round AD

- **D-AD1 — the editor gets its OWN unmanaged-image release, rather than un-stubbing
  `DB_EnumXAssets`.** Making the DB enumeration real would drag the whole zone/asset
  registry into a tool that deliberately loads loose files, to serve one caller.
  `imageGlobals.imageHashTable` is where the editor's images actually are, and walking
  it is 32768 pointer tests. The per-image *rules* are still the engine's, copied from
  `R_FreeLostImage`/`R_RebuildLostImage` rather than invented.
- **D-AD2 — the rebuild pass reproduces ONLY `R_RebuildLostImage`'s unmanaged arm.**
  Its `category < 5` arm ends in `Com_Error(ERR_DROP, "Couldn't load image ... to
  recover from a lost device")`, and in this editor an image with no texture is an
  ordinary missing asset; a managed image keeps its texture across a `Reset` anyway.
  Killing the process over a missing asset would be a worse bug than the one being
  fixed. Progs are excluded for the engine's own reason — they are render targets and
  `R_CreateForInitOrReset` rebuilds those.
- **D-AD3 — the second-chance release is a hand-picked list, not "call the cascade
  again".** Idempotency was verified per entry (see 59.3); `R_ReleaseForShutdownOrReset`
  as a whole is NOT idempotent and re-running it would trade a black screen for a
  double free. The editor surf cache is deliberately not re-dropped: it cannot
  repopulate while `dx.deviceLost` (`Radiant_FaceVisGpuReady`, camwnd.cpp:1130), so a
  second drop would only reprint its console line every retry.
- **D-AD4 — the retry arm is keyed on `D3DERR_INVALIDCALL`, not on "any failure".**
  `DEVICELOST` genuinely wants a bare retry, and re-running releases against a device
  that is merely not ready yet is churn on a path that is already correct. The two
  failures mean opposite things and now get opposite treatment.
- **D-AD5 — the `targetWindowIndex` latch is NAMED, not auto-cleared.** It is a real
  silent-forever path (row 2 of 59.1), but clearing it from the health watch would
  paper over a genuinely broken render bracket somewhere else and make the next such
  bug invisible instead of loud. Every bracket in the tree pairs unconditionally
  today; if this ever appears in a log, the fix belongs at the unpaired bracket.
- **D-AD6 — `g_disableRendering` is left exactly as it is.** It is a one-way counter
  with no decrement, which looks like a latent forever-black — but every increment
  site is followed by a fatal that exits the process, so it cannot be observed set in
  a living editor. Adding a reset would be inventing a state transition the binary
  does not have, to fix something that cannot happen. It IS reported by the health
  watch, so if that reasoning is ever wrong the log says so in one line.
- **D-AD7 — unauthorized paints are ignored by the health watch entirely.** Since
  round U a paint the pump did not ask for draws nothing BY DESIGN, and those arrive
  constantly (OS repaints, and the nested loop of every menu and modal dialog).
  Counting them would let a file dialog left open for fifteen seconds pop a "device
  lost" box. They are neither evidence of health nor of failure, so they neither start
  nor clear the clock.
- **D-AD8 — the escape-hatch box is owned by the frame**, unlike `Kiwi_FatalRescue`'s
  NULL owner (engine_stubs.cpp:309). That one runs while the process is dying, where
  re-entering our own WndProc as an owner is a risk with no upside; here the editor
  keeps running and the box belongs over its window, and the round-U guard means a
  paint from its nested loop draws nothing.
- **D-AD9 — the mid-load image-creation failure is documented, not patched.** If the
  device dies mid-load, `Image_Create2DTexture_PC`'s `CreateTexture` fails and raises
  `Com_Error(ERR_DROP, "Create2DTexture(...) failed")`, which is survivable inside
  `Brush_RealizeFaceMaterialsGuarded` (engine_stubs.cpp:230) and fatal outside it.
  Making that non-fatal means letting `Image_LoadFromFileWithReader` continue to
  `LockRect` on a NULL texture — trading a clean exit for an access violation. It did
  not happen in this report (the process was alive), the round-AD rebuild pass covers
  the unmanaged half, and the honest fix is a load-time device gate, which is its own
  round. See RADIANT_KNOWN_ISSUES "Round AD".

# Part XII — Round AF (2026-08-12): the LOFT, the fence cut, and the cave

## 60. Round AF — nine reports

Nine items, of which two are bug reports, one is the round's centrepiece (Loft) and
six are scoped enhancements. Every one is stated below with the user's own words,
the mechanism, and what it cannot do.

---

### 60.1 ITEM 3 — the boolean preview that died at twelve tools

**USER REPORT, verbatim:** *"When adding a bunch of objects to a boolean (windows on
a building), After about 10-12, the red previewer stopped working on newly selected
diff tools. I still clicked all of them and the operation worked as expected, but the
previewer always broke."*

**THE ARITHMETIC IS THE DIAGNOSIS, and the user found it to the brush.**
`KiwiCmd_DrawWorld` (kiwi_command.cpp) opened ONE 288-segment `KiwiLines_Begin` batch
for the whole gesture. A box brush costs 6 faces × 4 edges = **24 segments**.
288 / 24 = **twelve**. Past that, `OutlineBrush` returned false, the boolean's
`DrawWorld` `return`ed, and every tool after the twelfth drew nothing.

There were in fact TWO cliffs and both were spent in PICK ORDER, which is what made
the *newest* operand the one that vanished:

| budget | value | bites at | what disappeared |
|---|---|---|---|
| `KBOOL_MAX_FILL` | 8 | the 9th tool | the translucent red tint |
| the shared line batch | 288 segs | the 13th box | the outline **and the hover** |

The hover loss is the worse half: past twelve tools the editor stopped saying which
solid the next Shift+click would even take.

**THE FIX IS THREE CHANGES, and none is "raise it and hope".**

1. **THE BATCH SCALES.** `KiwiEditorCommand::LineBudget()` is a new virtual (default
   0 = "the framework's number"). `KiwiCmd_DrawWorld` takes
   `max( KCMD_LINE_BUDGET, cmd->LineBudget() )` clamped to `KCMD_LINE_BUDGET_MAX`
   (1536). The boolean answers with `OutlineCost()` **measured** over the actual
   windings — not a "24 per solid" guess, which would reintroduce the same cliff one
   shape further along (a cylinder tool is not 24 segments).
2. **NEWEST FIRST**, both budgets. Whatever the ceiling eventually drops is then the
   OLDEST operand, not the one being looked at.
3. **IT SAYS SO.** A degrade prints ONE console line per gesture naming the counts,
   and the hover outline is RESERVED out of the budget so it can never be the thing
   that falls off the end.

**WHY 1536.** It is of the same order as `KCON_DRAW_SEGMENTS` (1600), which the
construction pass has drawn every frame since shakeout H without ever being the thing
that overflows the render command buffer — that is the empirical justification, not a
guess. Same-colour segments merge into one `RC_DRAW_LINES` per 256-segment staging
flush (`Ed_EmitLineBatch` + `KLINES_VERTS`), so 1536 segments in a handful of colour
runs is ~6 draw commands, not 1536.

`KBOOL_MAX_FILL` went 8 → 32: punching a facade full of windows is a ~20-30 operand
gesture, and 32 boxes is ~192 `R_AddRenderCmdDrawTris` in a frame, next to a camera
pass that already emits one line batch per brush for hundreds of brushes.

---

### 60.2 ITEM 9 — the q3 curves that would not hide, and would not select

**USER REPORT, verbatim:** *"When hiding selected objects, q3 curves don't hide.
actually they aren't selectable at all. Fix this."*

**THE HIDE HALF IS ONE MISSING LINE, and it is a port defect rather than a design
question.** `FilterBrush` (filters.cpp:724) folds the HIDDEN bit into the same answer
as every filter entry — `return (a1->brushFlags & 5) != 0;` — and it gates:

* the camera's convex world pass (camwnd.cpp:2477, *"FilterBrush is load-bearing"*),
* BOTH of xywnd.cpp's brush loops (:1085 active, :1163 selected),
* all three pick entries (select.cpp:672, kiwi_pick.cpp:167, kiwi_boxselect.cpp).

The camera's **patch pass** asked nothing but `!b->def || !b->patch`. So `Select_Hide`
set `brushFlags |= 4` on the patch node correctly, the 2D views dropped it, every pick
path dropped it — **and the 3D camera kept drawing it.** "Hiding does nothing" and "it
cannot be clicked" were ONE defect seen from two windows.

It is not a deviation to fix: the binary has no separate patch pass at all —
`DrawGeneralWorld_` (0x407af0) runs ONE loop over the world brushes gated on
`!CullCubic && !FilterBrush` and lets `DrawBrush` (0x47B018) dispatch patches out of
it. The port split that loop in two so the patch surfs could be self-bracketed, and
the split dropped the gate on one side. Both sub-passes are now gated, matching
xywnd.cpp's pair.

**THE SELECTABILITY HALF HAS FOUR GATES, every one legitimate and every one silent:**

| # | gate | where |
|---|---|---|
| 1 | `g_PrefsDlg->m_bSelectCurves == 0` (the ported "Don't select curves", cmd 32852) | select.cpp:698 |
| 2 | a `Misc curve` filter entry with `isShown == 0` — and the round-Q fillet stamps `PATCH_BEVEL`, i.e. `type != 64`, squarely inside it | filters.cpp:509 |
| 3 | `Misc terrain` likewise, for a `type == 64` patch | filters.cpp:507 |
| 4 | selection **mode 3** (Face): a patch has no brush face to resolve to, so the pick answers "no hit" BY DESIGN | kiwi_pick.cpp:584-599 |

None of them is a bug, and any one alone produces "I click the curve and nothing
happens". So `KiwiVis_ReportPatchGates` prints ONE line naming every gate that is
currently ON — from the H key (once per session, and only when the map actually
contains a patch) and from Ctrl+H (always, including the ALL-CLEAR line, whose value
is that it rules the four out and names what is left: the patch's own `curveDef`,
which `PMESH_51` needs, pmesh.cpp:4257).

Box selection also gained the patch **bbox** windings as extra crossing witnesses.
The control-point-only test was honest about its limit, and for a fillet patch —
three or four control points strung along one edge — "strictly between two control
points" is most of the patch. Added on the CROSSING path only: `Contained()` demands
every witness be inside, and a diagonal patch's bbox corners stick out past its own
control points, so feeding them to an ENCLOSING marquee would make patches *harder*
to box-select.

---

### 60.3 ITEM 6 — LOFT (L), the round's centrepiece

**USER DIRECTIVE, verbatim:** *"You select 1 face then press L and select the other
face. (Another way is to select 2 faces and then press L and the operation starts) It
creates a new bridge between the 2 faces like a loft in plasticity. It can be used on
faces from other solids(Brushes) as well. Make it robust and try to solve most issues
including curvature. It might need a density setting before a final confirm.
G-continuity options like in plasticity would also be incredible."*

#### 60.3.1 WHAT PLASTICITY ACTUALLY HAS, and it moved the design twice

The research came back with two facts that had to change the plan rather than
decorate it:

* **its loft cannot take faces at all.** `LoftCommand.ts:12` reads its sections from
  the SELECTION — `const curves = [...this.editor.selection.selected.curves];` — and
  the only thing it PICKS is an optional SPINE (`LoftCommand.ts:23-32`, an
  `ObjectPicker` with min 1 / max 1 / `SelectionMode.Curve`).
* **it has neither of the two options the directive asks for.** `LoftParams`
  (`LoftFactory.ts:6-11`) is `thickness1, thickness2, thickness, closed` — no
  density, no degree, **no G-continuity control**. `closed` is not one either: it is
  set from the spine (`LoftFactory.ts:26`). The surface is one kernel call,
  `c3d.ActionSolid.LoftedSolid( placements, contours, spine, params, [], names, ns )`
  (`LoftFactory.ts:76`), so the continuity and the tessellation are C3D's, not the
  application's.

There is therefore **no Plasticity code to port here**; it has a NURBS kernel where
KIWI has half-spaces. Two things ARE taken across unchanged and are cited as such:

* `"l": "command:loft"` — `default-keymap.ts:270`, bare L. Adopted.
* the planar-section rule: each section is planarised with `curve3d2curve2d`
  (`LoftFactory.ts:40-41`) and one that will not planarise raises
  `ValidationError("Curve cannot be converted to planar")`. Adopted in spirit —
  **planar profiles only** — which brush face windings satisfy for free.
* its live-update loop (`LoftCommand.ts:19-21`, re-running the factory on every
  parameter change) is adopted in SHAPE: every field edit re-tessellates and the
  preview IS the result, not a schematic.

Everything else below is KIWI's own design.

#### 60.3.2 THE GRAMMAR

```
  2+ faces selected  ->  L starts LIVE on the first two, immediately.
  1  face  selected  ->  L is HOT and prompts for the second: click ANY brush face,
                         on any solid, including another face of the same solid.
  0  faces selected  ->  L refuses and names both states it accepts.

  D          RULED <-> TANGENT        Tab   density <-> tension
  digits     the focused field        RMB/Enter apply     Esc back a stage / cancel
```

The two-field table (`density` KNUM_COUNT, `tension` KNUM_FACTOR) is deliberate:
with two fields the numeric layer owns Tab and cycles them, which is what makes the
density typeable rather than nudgeable. Neither field is a LENGTH, so both undo
`Units_FromDisplay` — kiwi_primitive.cpp:232-252's rule, verbatim.

#### 60.3.3 THE EIGHT-STEP PIPELINE, and the one honest approximation

A classic brush is an INTERSECTION OF HALF-SPACES. A loft between two arbitrary
planar profiles is, in general, a surface with **non-planar quads**, and a non-planar
quad is not something a brush face can be. That is the whole difficulty and it does
not go away by being ignored.

1. **RINGS.** The two source faces' WINDINGS, in world space. A brush face winding is
   convex by construction (it is itself a half-space intersection), so both profiles
   are convex for free — which is why this command never needs the region layer's ear
   clip.
2. **SENSE.** Both rings rewound CCW about the loft axis `d = normalize(cB - cA)`, so
   a side quad's winding rule is one expression instead of two mirrored ones.
3. **COUNTS.** The SHORTER ring's longest edges are split until the counts match.
   Splitting the longest edge (rather than resampling both rings at uniform arc
   length) keeps every original corner of both profiles: the points it adds are
   strictly collinear with an existing edge, so the profile's SHAPE is bit-for-bit
   unchanged and only its parameterisation grows.
4. **CORRESPONDENCE.** The cyclic offset `k` minimising
   `Σ | perp_d(A[i] - cA) - perp_d(B[(i+k) mod n] - cB) |²` wins — nearest match after
   both rings are centred and the axial component projected out. Centring removes the
   translation the loft is supposed to have; projecting out the axis removes the
   length of the bridge. What is left is exactly "which rotation of B lines its
   corners up with A's", i.e. **no twist**, made measurable. O(n²) over n ≤ 64.
   A closed-form angular match would be O(n log n) and would be *wrong* for a ring
   whose vertices are not angularly uniform — which, after step 3 has split the long
   edges of a rectangle, is every ring here.
5. **STATIONS.** N+1 rings along the path. RULED interpolates linearly; TANGENT is a
   cubic Hermite per corresponding vertex — `h00 = 2t³-3t²+1`, `h10 = t³-2t²+t`,
   `h01 = -2t³+3t²`, `h11 = t³-t²` — leaving A along A's outward normal and arriving
   at B along B's, with tangent magnitude `tension × |cB - cA|` (default 0.5, i.e.
   "about half the span"). `M0`/`M1` are the same vector for every `i`, so a station
   is an affine blend of the two profiles plus a common offset: the cross-section
   morphs A into B while the path bends. `t == 0` and `t == 1` reproduce A and B
   **exactly**.
6. **PLANARISE — THE APPROXIMATION.** Each *intermediate* station ring is projected
   onto its own Newell best-fit plane. This is what makes station `i`'s top cap and
   station `i+1`'s bottom cap **the same plane**, which is what makes the segments
   mate with no gap and no overlap. The projection is exactly zero at both ends (the
   source windings are already planar) and shrinks with density, because a shorter
   span twists less. The two END stations take their own plane rather than a refit,
   so the loft's end caps are exactly coplanar with the faces it was asked to bridge.
7. **SEGMENTS.** Segment `i` is a brush over ring[i] and ring[i+1]:
   * two cap planes — the two stations' own fit planes, oriented outward;
   * one side plane per corresponding edge pair, whose direction is the **Newell
     normal of the (possibly non-planar) quad**, then PUSHED OUT until all four of
     its points are inside.
   Pushing out rather than through means the brush CONTAINS the hull of its 2n ring
   points, so **no vertex of either ring is ever clipped off**. The inflation is
   bounded by the quad's non-planarity — which is a function of how much the profile
   twists across ONE segment, i.e. exactly what the density field buys down. Newell
   is the right tool twice over: stable for a nearly-flat ring where three-point
   cross products are not, and its magnitude is twice the projected area, so a
   degenerate ring reports itself by collapsing.
8. **GATE.** Every segment goes through `KiwiValid_Rebuild` + `KiwiValid_CheckBrush`
   (§19 V1..V7) while it is still UNLINKED, so a rejection frees defs and touches
   nothing — kiwi_extrude.h's "REJECTION IS FREE".

**THE ONE FALLBACK.** A TANGENT loft can twist a segment into something §19 will not
accept where the RULED one between the same two profiles is perfectly ordinary. The
whole loft is retried RULED — **not per-span** — and says so on the console. A bridge
that is curved for six segments and straight for two is a shape nobody asked for and
would be harder to understand than either honest answer.

**MATERIALS.** Nearest end wins per segment, and the midpoint rounds toward face A
(the face chosen FIRST, which the gesture is anchored on). The chosen SOURCE FACE is
tried directly; a tool material on it (a mapper lofting between two caulk faces) falls
through to R2/R3 over the source BRUSH, ending at the classic caulk synthesis rather
than inventing a visible skin nobody asked for.

**UNDO.** `Select_Deselect(1)` → `KiwiCmd_UndoBegin("loft")` → land. The bracket opens
over an EMPTY selection and the new brushes land selected, so
`KiwiCmd_UndoCommit`'s `Undo_EndBrushList` stamps precisely them and one Ctrl+Z
removes the whole bridge. The two SOURCE brushes are never in the bracket because
they are never touched.

**PREVIEW.** Translucent sleeve fills (one draw per SEGMENT — 2n verts, 2n triangles
— because per quad would be up to 32 × 64 = 2048 draw commands a frame), station ring
outlines, and one RAIL per corresponding vertex, which is what makes a twist visible
before the user commits to it. BAD colour when the state is invalid; `LineBudget()`
scales with the station count through item 3's new hook.

#### 60.3.4 THE LIMITS, stated rather than discovered

* **PLANAR PROFILES ONLY.** Brush face windings always are. (Plasticity's own rule.)
* **EVERY SEGMENT IS CONVEX**, because a brush is. Two profiles whose correspondence
  would require a concave bridge are refused by §19, not silently mangled.
* **G0 (RULED) IS EXACT. G1 (TANGENT) is exact AT THE TWO ENDS** — the first and last
  station ARE the source windings, and the first/last path tangents ARE the source
  normals — and piecewise-linear in between, which is what a faceted medium can
  offer. Raising the density is what buys smoothness.
* **G2 IS NOT ATTAINABLE IN BRUSHES AT ALL** and is not attempted. A curvature-
  continuous bridge needs a curved medium; in this editor that medium is the q3
  PATCH (pmesh.cpp), which kiwi_patchfillet.cpp already lays into a chamfer. A patch
  loft is the honest future work.
* **THE SOURCE BRUSHES ARE NOT TOUCHED.** A loft BRIDGES; it does not weld. The two
  interior end caps stay where they are.

---

### 60.4 ITEM 1 — cut with a chain or a loop

**USER DIRECTIVE, verbatim:** *"When cutting, a lot of times I want to cut with an
entire circle or half-circle. Allow shift-clicking of lines during cut operation setup
to enable this."*

**A CLICK NOW NAMES THE WHOLE OBJECT**, not the segment under the cursor. For a
straight line that is exactly what it always was; for a circle or a polyline it is the
loop the directive asks for, and it needed no new picking —
`KiwiCon_SegmentCount` / `KiwiCon_SegmentWorld` have always been able to walk an
object. **SHIFT** adds another object; shift-clicking one already in takes it back
out — the boolean's additive grammar, unchanged.

**STAGE 2 IS A CLICK TOOL NOW**, and it had to be: a second line can only be named by
a second click, and the FIRST click is what enters stage 2, so with stage 2 refusing
clicks there was no reachable state in which a fence could be grown at all. This is
round AA's boolean change with the same cost accounted for — what is given up is
park-on-click, and the cut has NO DRAG to park (round L locks the plane at the click).
The hover therefore tracks in both stages, and **nothing about the locked plane
moves**: `m_p`, `m_planeN/D`, `m_lineA/B` and `m_away` are written only by
`AimFromCamera` / `AimFromFace`, both called only from `Click()`.

**THE SWEEP DIRECTION.** For a single segment it is unchanged and deliberately so:
round L's `AimFromCamera`, which strips the along-the-line component out of the view
direction. That derivation is about ONE line and does not generalise — ask it about a
circle and it answers differently for every segment, producing a fence of planes that
**fan** instead of forming a tube. So a MULTI-SEGMENT fence uses the objects' own
PLANE NORMAL (`KiwiCon_ObjectPlane`). A construction loop is planar by construction —
it is what the region layer accepts — and extruding it along its normal is precisely
the cookie cutter: a circle on the floor becomes a vertical tube, and the brush inside
the tube separates from the brush outside it. With no member carrying a plane (two
lines that determine none between them) the camera derivation from the FIRST segment
is the fallback, and the console says which was used.

**THE CASCADE.** Borrowed in shape from the Q boolean's, with exactly one structural
difference: **the boolean discards the intersection and a cut discards nothing at
all.** Per target, per plane, per surviving piece: `KiwiSplit_DefByPlane`; both halves
survive; a plane that misses a piece returns one re-clone which is dropped and the
piece carried. Ownership is the boolean's invariant verbatim —
`!owned ⟺ cur == { the map's own def }`, borrowed and never freed, the transition
one-way at the first real division. All-or-nothing per target: a refusal frees the
whole working set and leaves that brush on the map untouched.

**CONVEXITY, STATED PLAINLY.** Every piece a cut produces is CONVEX BY CONSTRUCTION —
each split is a single plane through a convex solid, and both halves of that are
convex. What a **concave fence** produces is not a concave piece, it is **MANY
pieces**: an L-shaped fence cutting a slab gives four solids, not two, because the two
planes of the L each cut the whole slab. That is correct, it is what a plane-defined
editor can do, and it is said in the HUD ("FENCE N planes … nothing is discarded"),
here, and in KNOWN_ISSUES so it is never mistaken for a bug. `KCUT_MAX_FENCE` is 64 —
the same 64 `KCON_SEGS_MAX` caps a circle at, so a maximally tessellated ring fits
exactly and nothing the construction layer can draw is refused for being too fine.

**MATERIALS AND UNDO ARE UNCHANGED.** Every plane goes through
`KiwiSplit_DefByPlane`, which seeds its template face from kiwi_material.h R2/R3
against the def BEING SPLIT — so a piece cut off a piece inherits from its own parent.
ONE `KiwiCmd_UndoBegin` covers the whole cascade, and the targets are on
`selected_brushes`, so the bracket head cloned all of them and nothing needs a hand
cover.

---

### 60.5 ITEM 2 — push in and you get a cave

> ### ⚠ SUPERSEDED BY USER DIRECTIVE — ROUND AP, ITEM 1 (see §68)
>
> *"when extruding, dont automatically bool diff the solid. It can be done by the
> user with a boolean after."*
>
> Everything described in this section — the `DragsIntoSolid()` point probe, the
> amber CARVE preview, the `CARVE (cavity)` HUD rung and the `Carve()` commit that
> turned the prism pieces into `KiwiBool_DifferenceByDef` tools — was **removed in
> round AP**. The region extrude now always lands its prism, in both directions,
> and carving is the user's own Q afterwards. `kiwi_boolean.h`'s cascade is
> untouched and keeps every entry point. Round Q's FACE un-extrude
> (`KEXTF_CARVE` / `KEXTF_DESTROY`) is a different feature and **survives** — §68
> item 1 states the three tests it passes. This section is kept as the record of
> what was built and why.

**USER DIRECTIVE, verbatim:** *"While doing an extrusion operation, if you push
inwards to make an indentation/cave, I want you to make that work. It might require a
brush extrusion/difference double combo operation to accomplish it."*

**WHAT EXISTED, honestly, because the round-Q note reads as though this were already
covered.** Round Q's negative-E semantics (grow / carve / destroy) are on the **FACE**
command only — `KEXTF_GROW` / `KEXTF_CARVE` / `KEXTF_DESTROY`, and its carve is
`KiwiXform_PushFaceOnce`, i.e. it **moves one face plane of one brush**. That can
hollow a brush only when the dent spans a whole face; it cannot make a window in a
wall. The **REGION** command had none of it: `MakeBuild` puts a negative distance on
the other side of the plane and lands an ordinary new brush there, and **no path in
kiwi_extrude.cpp had ever performed a boolean or even looked at a brush**. So "draw a
rectangle on a wall and push it in" produced a second brush interpenetrating the wall
— which is exactly the report.

**THE TRIGGER IS NOT THE SIGN OF THE DISTANCE.** A negative extrude in OPEN SPACE is a
legitimate, shipped gesture ("grow it downward instead") and must not change. What
separates the two cases is not which way the number points, it is WHAT IS THERE: the
cave is the gesture whose prism grows INTO a solid. So the test is a point test — one
`KEXT_MIN_DIST` along the drag direction from the region's own centroid, against every
CSG-usable brush (`KiwiBool_PointInSolid`). The centroid rather than a vertex because
a corner of the profile can legitimately sit exactly on a brush edge, where "inside"
is a coin toss; the centroid of a region drawn on a face is squarely on that face. It
is re-asked every frame with no latching, exactly as round Q's three-way is, so
dragging back and forth across the wall crosses freely between "new body" and
"cavity".

**WHAT HAPPENS THEN.** The prism defs are built by the SAME `BuildPieceDef` the grow
path uses — one per convex piece, so a **concave profile cuts a concave cavity as
several tools in cascade** — and each is handed to the Q boolean's own difference,
newly exported as `KiwiBool_DifferenceByDef`. Nothing about the carve is re-derived:
the N-plane subtract, the §19 all-or-nothing policy per target, kiwi_material.h R4
(the hole wears the TARGET's material, never the tool's) and the land-then-free
ordering are all that function's. This is the "brush extrusion / difference double
combo" the directive predicted, and it multi-brush cascades for free because the
boolean's does.

**THE PRISM IS CONSUMED.** It is scaffolding: built unlanded, used as a cutter, freed.
It is never linked, so freeing it is `Brush_Free_R` with its precondition satisfied by
construction and the undo stack never hears of it.

**A DRY RUN COMES FIRST** (`KiwiBool_WouldCarve`), and it is what keeps
kiwi_command.h's "a command that mutates nothing must NOT open a bracket" true: a cave
aimed a hair off the wall would otherwise leave an empty undo record to step over.

**THE PREVIEW SAYS WHICH.** Amber (`KEXT_COL_CARVE`, round Q's own carve colour, so
there is ONE language: amber = material is being taken away, blue = added, red =
refused) and the HUD reads `CARVE (cavity)` every frame before the confirm.

**UNDO COVER, the subtle part.** The bracket head is
`Undo_AddBrushList( &selected_brushes )`, and a brush the prism happens to penetrate
is by definition NOT on that list — nothing cloned it. `KiwiBool_DifferenceByDef`
covers every target it is about to free with `Undo_AddBrush` itself, and **all** the
covers run before any landing or freeing, so the record's brush section is contiguous
(undo.cpp:539 warns when brushes are added after an entity, and interleaving
cover/land/free per target would produce exactly that).

---

### 60.6 ITEM 4 — the trim that would not touch a circle

**USER DIRECTIVE, verbatim:** *"Allow the trim tool to chop into completed lines (like
a circle to make a half circle by eating half the links)."*

**THE SPAN MACHINERY ALREADY DID THIS.** Round T built the closed-chain arm in full:
`BuildChain` appends the wrap point so the parameterisation covers the whole ring,
`GatherCuts` excludes the wrap-adjacent segment pair, `hover_t::wrap` expresses the
interval that crosses the seam, `SpanAt` has a dedicated closed branch, and
`PrepareTrim`'s closed arm walks the complement all the way round. **A closed POLYLINE
has trimmed correctly since that round.**

What a CIRCLE could never get past is the TYPE GATE — `Trimmable()` — twice over,
since a parametric object also has an empty `pts`. Shakeout H argued that refusal
("trimming one means converting it to a polyline, which silently destroys the thing
the user drew"), and the directive overrules exactly that: **half a circle IS a
polyline, there is nothing else for it to be, and refusing to make one does not
preserve the circle — it preserves the inability to cut it.**

The gate now asks the only question that matters — "can this be walked as a chain of
segments" — which `KiwiCon_VertCount` / `KiwiCon_VertWorld` answer for every type.
The conversion is no longer silent: the HUD reads `· becomes a polyline` BEFORE the
click, and the tessellation it is frozen at is the one the object was carrying, which
item 7 turned into a number the user chooses. That is why the two items shipped
together.

**ONE CROSSING IS STILL A REFUSAL, and now it says so.** Plasticity reaches the same
answer by arithmetic rather than by rule: `PlanarCurveDatabase.ts:121-123` gives a
closed curve no synthetic end points (an OPEN one gets both of its own injected at
:110-119) and only appends `crosses.push(crosses[0])` to close the seam. With ONE
crossing that array is `[c0, c0]`, so the emission loop's
`if (Math.abs(start - stop) > 10e-6)` (:135) never fires and the curve ends up with
**zero fragments** — nothing to pick, i.e. untrimmable. Two crossings give exactly two
fragments, one of which wraps the seam. KIWI has a console and can explain itself, so
it does, throttled to one line per object.

---

### 60.7 ITEM 5 — the loop that would not close

**USER REPORT, verbatim:** *"Curve closed loop detection still needs a bit more work.
I find myself having to re-trace the line points myself to get it to detect a
construction face."*

Round AA fixed the SAME-CHAIN case at the source, and its note is still right —
*"loosening the region tolerance would take a genuinely skew ring and pretend it was
flat"*. What that fix cannot reach is a loop assembled from **several objects** drawn
at different times, on different working planes, possibly before the latch existed.
Those meet the acceptance path with endpoints that nearly coincide and points that
nearly share a plane, and "nearly" was measured against a **hard 0.5 world units
everywhere** — a number chosen when the editor worked at grid 1 and 2.

**(a) THE ARGUMENT FOR SCALING, and it is what keeps this from being "loosen it and
hope".** Construction points SNAP TO THE GRID (§17, absolute snapping since round P).
Two endpoints the user meant to be DIFFERENT therefore land on different grid nodes,
i.e. **at least one full grid step apart**. Two endpoints meant to be the SAME land on
the same node. So any weld tolerance strictly below HALF a grid step is *incapable* of
fusing two points the grid put in different places, and a QUARTER of a step is that
with a factor of two in hand:

```
  weld = clamp( grid * 0.25, KREG_JOIN_DIST      (0.5), KREG_TOL_MAX (16) )
  band = clamp( grid * 0.25, KCON_PLANE_FIT_DIST (0.5), KREG_TOL_MAX (16) )
```

The FLOOR is the old 0.5, so nothing gets tighter than it has ever been. The CEILING
is 16 units, because the argument assumes snapped points and a mapper at grid 256 with
snapping off would otherwise get a 64-unit weld, which is a real distance in a real
map. Both are functions, not constants, so every call site re-reads the grid — and
`kiwi_arrange.cpp`'s `KARR_WELD` asks the same accessor, because a loop that welds for
the chain walker must weld for the arrangement.

**(b) NEAR-COPLANAR SETS: the projection was already there.** The acceptance path
already projects every member's points onto the group's best-fit plane
(`AppendObjectPoints` → `KiwiCon_WorldToPlane`), and the plane itself already comes
from Newell's method (`KiwiCon_FitPlane`). All that changed is how far out a point may
be and still be admitted to that projection — the BAND above.

**(c) IT SAYS WHY.** Every rejection inside `BuildRegions` is a silent `continue`,
which is why "it will not detect my face" has never had an answer the editor itself
could give — and why re-tracing the points was the user's best move.
`KiwiRegion_ReportGaps` welds every visible OPEN object's two ends at exactly the
distance the chain walker uses and reports the nearest DANGLING pair:

```
Construction: loop gap 0.80 at (128 64 0) — nearly closed — 2 open end(s) are not
joined.  The weld is 2.00 (a quarter of the grid); raise the grid or move that point
closer.
```

It hangs off the Extrude refusal (*"Extrude: no closed construction region."*), which
was the only user-visible sign that detection had failed and named no reason.

---

### 60.8 ITEM 7 — segment density for the round shapes

**USER DIRECTIVE, verbatim:** *"I want another option for side-density on the round
shapes. Sometimes you want more sides on the circles/cylinders. Add this to the [tab]
typing menu for the tools that use segments like that."*

The SOLID primitives (cylinder / sphere / cone) already had a `sides` field on Tab
since shakeout E. What did not exist:

* **the construction CIRCLE, 2-POINT CIRCLE and ARC had no control at all** —
  tessellation was `CircleSegs(radius)`, radius-driven and not settable;
* **the n-gon had a count but only on `[` / `]`**, not in the Tab menu;
* the caps were low.

`kconObject_t` gains `int segs` (**0 = automatic**, the radius-driven rule, so every
circle drawn before this round and every one the user does not touch the field on
tessellates byte for byte as it did). It is stored PER OBJECT rather than as a tool
setting because the count is a property of the shape: two circles of the same radius
may legitimately want 8 sides and 48, and a later region / extrude reading the store
has to get the one the user chose for THAT circle. ONE choke point,
`CircleSegsFor(o)`, so every reader agrees without any of them knowing the override
exists. ARC honours it pro-rata over its sweep, as the automatic rule does.

The n-gon's `[` / `]` keys and its Tab field are now **two views of one number**, and
the last-used count is remembered per session in `kiwi_radiant.ini` under
`[KiwiUX] RoundToolSides` — ONE setting rather than one per tool, because "I work at
32 sides" is a statement about the map being built, not about which of the four round
tools drew a given ring.

Sidecar: written as `segs N` and **omitted when 0**, so a store that never touched the
field writes exactly the bytes it wrote before this round. The loader's
unknown-keyword arm means an older build reads a newer file and falls back to AUTO,
which is the correct degradation.

Caps: `KPRIM_CYL_SIDES_MAX` 32 → **64** (matching `KCON_SEGS_MAX` and
`KEXT_MAX_PROFILE`; the binary's own ceiling refuses only at 1020,
brush.cpp:3383-3391); `KCON_POLY_SIDES_MAX` 32 → **64**; a new floor
`KCON_SIDES_MIN` of **3** for a *typed* count, because the automatic rule is choosing
a smoothness and 8 is right for that, but a user typing "3" into a circle is asking
for a triangle and means it.

`KPRIM_SPH_SIDES_MAX` went 12 → **16 and deliberately not 64**: a sphere is
`sides × sides` faces (`Brush_MakeSidedSphere`, 0x47BE90 — bands × segments), so the
cylinder's 64 would be a **4096-face brush**. 16 is 256 faces. The asymmetry is the
geometry's, not a policy.

---

### 60.9 ITEM 8 — the centre box

**USER DIRECTIVE, verbatim:** *"Add an option to the Box command that allows it to be
a 'Center' box instead of the current 'Corner box' behavior."*

Plasticity ships the two as **separate commands**, not as a mode inside one:
`CornerBoxCommand` (`BoxCommand.ts:60`) and `CenterBoxCommand` (`BoxCommand.ts:155`),
bound `"shift-c": "command:corner-box"` and `"shift-v": "command:center-box"`
(`default-keymap.ts:288-289`). KIWI now follows that exactly.

**SHIFT+V WAS ALREADY RESERVED FOR IT.** Shakeout F put the centre RECT there as a
placeholder and said why in as many words: *"KIWI HAS NO CENTRE-BOX primitive (§16b.3
lists it as LATER)… Binding Rectangle (CENTRE) here instead keeps the corner/centre
PAIRING the neighbouring keys teach… and means the chord will not have to move when
the centre box does ship."* It has shipped; the chord is claimed as promised.

The centre rect **moves to Alt+V**, and nothing else is disturbed. vk 0x56's
occupancy, re-audited in full: mods 0 DragVertices 33005 (mainfrm.cpp:1045) · mods 1
the centre box · mods 3 VehicleGroup 33221 (displaced there by shakeout F) · mods 4
Paste 33040 (:1118) · mods 5 ToggleView 33071 (:1070). **mods 2 (Alt) is FREE**, and
it is not in res/radiant.rc's accelerator table either (:508-515), which matters
because `TranslateAccelerator` runs BEFORE the hotkey table.

Implementation is a fifth `primKind_t` over the same command class, not a fifth class:
a centre box differs from a corner box in ONE function (`BoxLoopUV` — the rect is
`p0 ± |c1 - p0|` per axis) and every other `m_kind == KPRIM_BOX` test in the file was
widened to `IsBox()` in the same change. Field 0 relabels to `half-size`, and the HUD
reports the base as **twice** the drag, because that is what will be built.

---

## Decision log — round AF

- **D-AF1 — `LineBudget()` is a virtual on the command, not a global raise.** Every
  other overlay in the layer is sized correctly for what it draws; only an overlay
  whose cost is a function of a set the user is still growing needs to negotiate. A
  global 1536 would give the same headroom to a gesture that has never needed more
  than 24 segments, and would make the render-command pressure of a busy frame
  unpredictable. The default return of 0 means every pre-round-AF command is
  byte-identical.
- **D-AF2 — the boolean spends its budgets NEWEST-FIRST rather than merely raising
  them.** A ceiling can always be reached; what must never happen again is that the
  thing it drops is the thing the user just clicked. Raising the numbers makes the
  common case never reach the degrade; the ordering makes the degrade survivable.
- **D-AF3 — the patch hide fix goes in camwnd.cpp, not in the KIWI layer.** It is a
  port defect against the binary's own single gated loop, and papering it over from
  the outside (say, by having the hide command also set the layer bit) would leave
  the 2D/3D disagreement in place for every other filter entry.
- **D-AF4 — the four patch-selection gates are REPORTED, not removed.** Every one of
  them is a real feature with a real user behind it: "Don't select curves" is a
  toolbar toggle in the shipping binary, the Misc filters are the filter system
  working, and mode 3 having no face on a patch is a fact about patches. What was
  wrong was that all four were silent. This is round AD's doctrine ("the chain is not
  allowed to be silent again") applied to selection.
- **D-AF5 — the loft PLANARISES its intermediate stations, and that is the whole
  approximation.** The alternative — letting station rings be non-planar and giving
  each segment its own best-fit caps — produces segments that do not mate, i.e. a
  bridge with hairline gaps between its own solids. A gap is worse than a bounded
  error that is zero at both ends and shrinks with density.
- **D-AF6 — the loft's side planes are pushed OUT to contain all four quad points,
  never fitted through three of them.** Fitting through three clips the fourth, which
  means a vertex of a source winding is silently outside the solid the loft claims to
  bridge to. Containing is the conservative direction and its error is measurable.
- **D-AF7 — a failed TANGENT loft retries RULED for the WHOLE bridge, not per span.**
  A bridge that is curved for six segments and straight for two is a shape nobody
  asked for. Two honest answers beat one incoherent one, and the console names which
  was given.
- **D-AF8 — G2 is not attempted.** Brushes are planar-faced; curvature continuity
  needs a curved medium. The editor HAS one — q3 patches — and kiwi_patchfillet.cpp
  already proves the machinery. Shipping a "G2" that was really a denser G1 would be
  a lie in a field label.
- **D-AF9 — the fence cut's sweep direction is the construction PLANE NORMAL, not the
  camera.** Round L's camera derivation is correct and stays for a single line, but it
  is a statement about one line: applied per segment of a ring it fans the planes and
  the "tube" never closes. The plane normal is the only direction that is the same for
  every segment of a planar loop, which is what a cookie cutter needs.
- **D-AF10 — the fence KEEPS every piece, and a concave fence making many pieces is
  documented rather than mitigated.** Merging them back would need a non-convex brush,
  which this editor does not have; picking "the" inside piece would need a rule about
  which side the user meant, which the gesture does not carry. N pieces is the true
  answer and the HUD says the plane count in advance.
- **D-AF11 — the cave triggers on "the drag goes into solid", not on the sign of the
  distance.** The sign already means something (round Q's grow-the-other-way), and
  redefining it would break a shipped gesture to serve a new one. The point test is
  about the world, which is what the user is actually reasoning about.
- **D-AF12 — the region carve DRY-RUNS before opening its bracket.** kiwi_command.h
  forbids an empty undo record, and "the prism missed everything" is a real outcome of
  a gesture aimed a hair off a wall.
- **D-AF13 — trimming a circle CONVERTS it to a polyline, and the HUD says so before
  the click.** Shakeout H's refusal protected a parametric object at the cost of the
  operation the user wants. The conversion is inherent — half a circle has no
  parametric spelling in this store — so the honest move is to do it and to be loud
  about it, with item 7 giving the user control of the tessellation it freezes at.
- **D-AF14 — the region tolerances scale with the GRID, at a quarter step, with the
  old value as a floor.** The snapping invariant makes a quarter step provably unable
  to fuse two points the user meant to keep apart, which is the property that
  separates this from "loosen it". The 16-unit ceiling exists because the invariant
  assumes snapped points.
- **D-AF15 — the loop-gap report names the SMALLEST real gap.** The near miss is the
  one the user meant to close; a line end genuinely on its own across the map is not
  news, and listing every dangling end would bury the one that matters.
- **D-AF16 — the segment count lives on the OBJECT, and the "last used" lives in the
  ini as ONE number.** Per-object because two circles of the same radius may want
  different densities and every downstream reader must get the right one; one remembered
  number because four per-tool settings is four places for the answer to be stale.
- **D-AF17 — the centre box is a `primKind_t`, not a command class; but it IS a
  separate COMMAND ID.** The geometry differs in one function, so a fifth class would
  be duplication. The id is separate because Plasticity's is, because a chord has to
  reach it directly, and because an in-command toggle would be a hidden mode with no
  visible state — the thing the HUD rule exists to prevent.
- **D-AF18 — the centre RECT is displaced rather than deleted.** Shakeout F chose
  Shift+V for it precisely to hold the seat, and said the chord would move when the
  box shipped. Alt+V is free by the same audit that freed Shift+Alt+V, and the rect is
  still in the palette and the Shift+A add menu by name.

---

## 61. Round AG — eleven reports

Eleven items: four are bug reports that turn out to have real root causes rather
than tolerances (2, 10, 7, 5a), five are scoped enhancements (1, 3, 6, 9, 11),
one is a keybind the user simply wants a different way (4), and one is a design
question the user asked out loud and the coordinator answered yes to (8).

---

### 61.1 ITEM 2 — the light-blue fill that only appeared sometimes

**USER REPORT, verbatim:** *"Construction faces are lacking their light blue color.
It should show light blue whenever I can extrude from a closed off set of
construction lines - but it only does it sometimes."* The screenshot is an ARCH
profile — straight lines plus a tessellated arc — with several enclosed cells
unfilled.

**THIS IS THREE DEFECTS, AND THE FIRST IS A ROUND-AF REGRESSION.**

**(a) A WELD WAS APPLIED TO GEOMETRY IT COULD SWALLOW.** Round AF item 5 scaled the
weld tolerance to the grid (`grid * 0.25`, floored at 0.5, ceilinged at 16) and its
argument is sound *as far as it goes*: construction points SNAP, so two points the
user meant to keep apart are at least one grid step apart, and a quarter step cannot
fuse them. **That argument is about USER-PLACED ENDPOINTS.** It is false for
TESSELLATED vertices, and three consumers eat tessellated vertices:

    a circle or arc of radius r at the KCON_SEGS_MAX cap has an edge length of
    2*pi*r/64 = 0.098*r, which has NOTHING to do with the grid.
    A radius-32 arc has 3.1-unit edges.  At grid 16 the weld is 4.0.

So at grid 16 and coarser:

| site | what it did |
|---|---|
| `kiwi_arrange.cpp` `NodeFor` | welded each arc vertex onto its predecessor, so every one of that arc's fragments came back with `n0 == n1` and was `continue`d — **the arc left the arrangement entirely** |
| the same file's fragment floor (`Len2(prev,cur) > weld`) | dropped those fragments a second time |
| `AcceptLoop`'s `DedupLoop` | chewed the arc's vertices out of any loop that did survive, collapsing it below three points |

A rectangle drawn at grid 16 has 16-unit edges and sails through all three. **That
is "it only does it sometimes": it is a function of the grid and of how finely the
round objects in the loop are tessellated, and of nothing else.**

THE RULE, and it is one idea: **a weld may never exceed `KREG_WELD_EDGE_FRAC`
(0.4) of the FINEST edge it is being applied to.** Below that fraction it cannot
collapse the finest edge present, whatever the grid says; above the finest edge the
grid's number still wins, so round AF's fix is intact everywhere it was actually
about — a sketch of straight lines drawn at grid 64 still welds at 16, because its
finest edge is 64. `KiwiRegion_WeldFor( finestEdge )` is the one place it lives;
the arrangement computes its bound once per call from its own input
(`ArrangeWeld`), and `AcceptLoop` computes it from the loop's own edges.

**(b) THE LOOP CAP WAS THE TESSELLATION CAP, AND THEY ARE DIFFERENT QUANTITIES.**
`KREG_MAX_LOOP` read `KCON_SEGS_MAX` (64). But:

* `KCON_SEGS_MAX` is how many segments ONE round object is tessellated to;
* `KREG_MAX_LOOP` is how many vertices a DERIVED loop may carry — and a derived
  loop is bounded by PARTS OF SEVERAL OBJECTS. A full 64-segment circle with one
  chord across it produces cells of 64+2 and 2+2 vertices. The first was over the
  cap and was dropped **silently, in two places** (`kiwi_arrange.cpp`'s cell
  emitter and `AcceptLoop`). **An arch profile built on a tessellated arc is
  exactly that shape.**

`KREG_MAX_LOOP` is 128 now (2 × `KCON_SEGS_MAX` — a cell bounded by two fully
tessellated round objects), `KEXT_MAX_PROFILE` is defined AS `KREG_MAX_LOOP` so it
can never fall behind again, `KEXT_MAX_PIECES` follows to 128 (Hertel-Mehlhorn on a
128-gon can need 126 pieces), and **both drops now print**.

**(c) PASS 3 NEVER RAN ON A PLANE WITH NO OPEN OBJECT ON IT.** The arrangement pass
lives INSIDE the PASS-2 open-object group loop, so a plane carrying only CLOSED
objects never reached it: two overlapping rectangles, or a circle inside a rect,
enclose real cells and produced NO fill — while the same picture with one stray
line across it produced all of them, because the line seeded a group. PASS 3b is a
second sweep over closed objects whose plane no group already arranged. It costs
nothing when the store has open geometry on every plane, and it cannot double-emit
(`DuplicateRegion` is the same guard PASS 3 uses).

**THE INVARIANT, STATED:** extrudable ⇔ filled. It holds because both sides read
the same `KiwiRegion_All()` and because the profile cap now *is* the loop cap. Where
it cannot hold — a cell past 128 corners — the editor says so on the console instead
of dropping it silently.

---

### 61.2 ITEM 10 — the grid that ends in mid-air

**USER REPORT, verbatim:** *"the grid still shrinks and scales inappropriately at
narrow camera angles. Needs to be fixed, dont understand why this is happening."*

**NOTHING IS SHRINKING, AND THAT IS THE WHOLE DIAGNOSIS.** Grid v3 (round M) is
height-only LOD with a world-anchored lattice window of `R = spacing *
KGRID_HALF_CELLS`. At 192 units up on a 10-unit grid that is R = 1200, so the
window is 2400 units across and the grid **stops 1740 units in front of the eye**.
Look straight down and that window covers the screen. Tip toward the horizon and
the VISIBLE ground plane runs to tens of thousands of units while the window does
not move — so the user sees the window's own edge and reads it, reasonably, as the
grid shrinking with the angle.

v3 made DENSITY independent of pitch, which was the directive, and left REACH
independent of pitch too — which was never the directive. **Reach and density are
different questions.**

**WHY THE FIX IS A COARSER RING AND NOT A BIGGER WINDOW.** Covering distance D at
spacing S costs 2D/S lines per axis; that is arithmetic. And `R = 120 * spacing` is
already very nearly the screen-space limit: a cell of size S at ground distance d
subtends about `(S/d) * f` pixels where `f = (height/2) / (tan(fov/2)*0.75)` is
CameraCalcRayDir's own scale (camwnd.cpp:3090) — at 65° and 900 px that is f ≈ 940,
so a 4-pixel floor puts d_max ≈ 235·S. The fine tier is drawn to about where it
stops being legible. The honest extension is a SECOND, COARSER LATTICE:

    FAR RING   spacing = near spacing << 3   (8x)
               reach   = near reach   << 3   (8x)
               cells   = the SAME KGRID_HALF_CELLS, so the SAME line count

**Eight times the distance for the same 482 segments, and the near field is
COMPLETELY UNCHANGED.** It is emitted BEFORE the near pass so the brighter near
lines land on top of the coincident far ones (8·spacing is a multiple of spacing,
and $line is depthTest LESSEQUAL).

**THE FRUSTUM IS BACK, AND ONLY ONE THING IS TRUSTED TO IT.** v2 died because the
frustum footprint fed the SPACING (`while (span/spacing > 200) spacing *= 2`), so a
shallow angle coarsened the grid under the viewer's feet. Here the frustum answers
ONE boolean — "is there ground past R" — and the ring it turns on has a FIXED
spacing and a FIXED reach. A pitch change can add or remove the far ring; it can
never change a single line of the near lattice. The probe is four
`Ed_CameraCalcRayDir` rays (the picker's own forwarder, so ortho is free): three
along the top edge plus one at the bottom centre, resolved as
off / horizon-visible / `d = h·|horiz|/|dz|`, clamped to 8R and passed through a
1.30 / 1.05 Schmitt latch.

Budget: near 482 + axes 6 + far 482 = **970 ≤ KGRID_MAX_SEGMENTS (1024)**. The far
ring gets no halo — it is dim distant context and doubling its cost to soften it
would be the wrong trade.

---

### 61.3 ITEM 7 — hide is on the timeline now

**USER DIRECTIVE, verbatim:** *"making something hidden should be an un-doable
action."*

Round J's ruling ("UNDO: NONE, AND THAT IS FAITHFUL") had one true half and one
false half. **TRUE:** a hide must not go into a LEGACY record. **FALSE:** therefore
it is not undoable at all — the CONSTRUCTION store's hide has been journalled since
round U by exactly the right mechanism, and the asymmetry was visible to the user as
"Ctrl+Z undoes hiding a LINE but not hiding a BRUSH".

**WHY IT COULD NOT BE A LEGACY RECORD, and this is a fact about the data:** the
hidden state is `selbrush_t::brushFlags & 4` plus the depth counter
`selbrush_t::xx5`, and both live on the **INSTANCE**. The legacy undo snapshots
**DEFS** — `Undo_AddBrush` clones through `Brush_FullClone_sub475E80`
(undo.cpp:510 → brush.cpp:7401), which allocates a 0x58 `brush_t` with no
`brushFlags` member at all. Worse, `Undo_Undo`'s re-create phase goes through
`Brush_AddToList` (brush.cpp:667), whose `memset( b, 0, 0x38u )` **ZEROES**
`brushFlags` and `xx5`. A legacy record does not merely fail to carry the hidden
bit — it destroys it.

So hide gets **KUNDO_VISIBILITY**, a third domain modelled one-for-one on
`KiwiCon_UndoPush / UndoPop / RedoPop / ClearRedo`: a whole-state snapshot of
`{ brush_t *def, hidden, depth }` per live instance, keyed on the **DEF** pointer
(the instance does not survive an unrelated legacy undo and the def does).

* **It restores ONLY bit 2 and `xx5`.** Never the whole word: bit 0 is
  FilterBrush's cache, bits 1 and 5 are the LAYER system's, bit 7 is SELECTED,
  bits 3/4 are the filter-list accumulators. Writing the word back would make
  Ctrl+Z on a hide silently re-select brushes and stomp layer visibility.
* **One record per gesture**, at the head of `H`, `Shift+H`, `Alt+H`,
  `Shift+Ctrl+H`, `Ctrl+H` and each outliner eye click — never per brush. The push
  is a no-op when the snapshot equals the last one, so a hide that changes nothing
  does not cost a Ctrl+Z.
* `kiwi_outliner.cpp`'s private `SetBrushHidden` copy became a forwarder to
  `KiwiVis_SetHidden`: a second spelling of "hidden" is a second thing the snapshot
  store would have to be kept in step with.

---

### 61.4 ITEM 5 — the circle's side count, and the cardinals

**USER REPORT, verbatim:** *"when making a circle, i still cant specify the number
of segments (only for cylinders?). When making a circle, there need to be segments
on each 90 degree point so I can make more advanced shapes like ovals."*

**(a) THE FIELD WAS ATTACHED, TAB REACHED IT, AND IT HAD NOTHING TO SHOW.** Round
AF really did put `sides` on the circle (`WantsSidesField`,
`KCON_FIELDS_LEN_SIDES`) and the typed value really did flow. What was missing is
that in this HUD **a field with no value does not exist**:
`kiwi_numeric.cpp`'s `FieldDisplay` (:104-127) returns false when the command's
`NumericFieldValue` says no, and the bubble `continue`s past the row (:460-461).
The base's `NumericFieldValue` refuses the sides field when `ToolSides()` is ≤ 0
(:1785), and the base `ToolSides()` returns `m_sidesOverride` — **0 until something
has been typed**. So: no row until you type, and no way to see that typing would
work. Tab moved a caret onto an invisible box.

*"only for cylinders?"* is the diagnosis to the letter — `kiwi_primitive`'s
`m_sides` is seeded to `KPRIM_CYL_SIDES_DEF` and is therefore never 0, its row
always draws, and that tool has always been usable.

The fix: the circle, the 2-point circle and the arc override `ToolSides()` to report
**the count the shape WILL HAVE, never 0**. AUTO is an answer, not an absence. The
number in the box is now the number on the geometry at every instant, including
before the first click.

**(b) CARDINAL ANCHORING.** `KiwiCon_VertWorld` already phases the ring at ang = 0,
so the 0° cardinal has always been hit. The other three are hit **if and only if the
segment count is a multiple of four** (vertex k is at 2πk/n; 90/180/270 are
k = n/4, n/2, 3n/4). The automatic rule produces 8, 9, 10, 11 … so three counts in
four miss all three.

It is not cosmetic: scale a ring anisotropically and you get an ellipse whose SHAPE
is right at any phase — but its EXTREMES, the four points a mapper aligns to, snaps
to and extrudes from, only exist as vertices when the cardinals are vertices. Off
phase the major axis ends in a flat chord, the bounding box is smaller than the
nominal radii, and the quadrant symmetry the user is asking for is simply absent.

So `CardinalSegs` rounds the count **UP** to the next multiple of four (up, not to
nearest: a count is a smoothness request and rounding down would silently give less
than was asked for), for both the automatic rule and a typed override. The ceiling
64 is already a multiple of four. The ARC deliberately does not get it — `ArcSegs`
takes the count pro rata over an arbitrary sweep, so there are no cardinals to hit.
The HUD says `(N segs, x4 for the quadrants)`.

---

### 61.5 ITEM 1 — one gesture, many sources

**USER DIRECTIVE, verbatim:** *"you fixed shift-clicking face extending (good job).
But I want it on all extrusions. I should be able to shift click construction faces.
and shift click extrude from solid faces."*

**THE COMMON RULE: each source keeps its own plane and grows along ITS OWN NORMAL
by the SAME distance.** That is §20's "active drives, the rest follow" for the
multi-face push/pull, and it is the only reading that survives two sources on two
different walls — a shared world direction would push one of them sideways through
its own solid.

**REGIONS.** `KiwiRegion_Select` held ONE world centroid; it holds a LIST now
(`KiwiRegion_ToggleSelect`, `SelectedCount`, `SelectedAt`, `IsSelected`). Each
stored centroid resolves independently, so a region that stops existing drops out on
its own. The single-selection API is unchanged in meaning — `Select` replaces the
set, `SelectedIndex` answers the PRIMARY — so nothing that predates this round has
to know the set exists. Shift+click on a region toggles it; the fill tints every
member; `E` latches the whole set at Begin and previews, builds and lands all of it
in ONE bracket.

**FACES.** The typed selection has been a multi-face set since round U; until this
round `E` read the ACTIVE face out of it and ignored the rest — which is exactly why
shift-clicking faces "worked" for the push/pull and did nothing for the extrude.
`BuildProfile` was split into `BuildProfileInto`, every other live non-patch
`SEL_FACE` item is profiled at Begin, and `Grow()` builds and gates them all before
anything is linked.

**ALL-OR-NOTHING, deliberately.** One rejected source refuses the whole gesture
rather than landing a partial edit that one Ctrl+Z cannot describe. The one
exception is an EXTRA face that will not profile at all (a >128-vertex winding, a
degenerate plane, a non-convex winding): it is dropped from the extras and reported
by count, because the drive face is the one the user aimed at and losing a whole
gesture to a bad neighbour would be worse.

**WHAT IS NOT MULTI-SOURCE, and why:**

* **The FACE CARVE (negative E).** It is round Q's un-extrude —
  `KiwiXform_PushFaceOnce` pushes ONE face plane of ONE brush, with its own destroy
  threshold measured from that brush's thickness. "Push five unrelated face planes
  in by the same number" is not one act: each has its own destroy depth, some would
  delete their brush and some would not, and one Ctrl+Z would undo a mixture the
  user never described. It refuses out loud and names the count it is skipping.
* **MIXED regions + faces.** They are two different COMMANDS (`KIWI_CMD_EXTRUDE_REGION`
  vs the face arm of `E`); the dispatcher picks one. It does not fall out naturally
  and it is not forced — documented in KNOWN_ISSUES.

Both previews scale their line batch through round AF's `LineBudget`, so the second
prism cannot vanish at the cliff round AF removed for the boolean.

---

### 61.6 ITEM 3 — the outliner's hover is a viewport hover

**USER DIRECTIVE, verbatim:** *"While mousing over the brushes in the outliner, it
should highlight them in 3D so I can find them easier."*

**PLASTICITY DOES EXACTLY THIS, and it is one line of its own outliner.**
`plasticity/src/components/outliner/Outliner.tsx` binds `onPointerEnter` /
`onPointerLeave` on every row to `this.editor.selection.hovered.add(item)` /
`.remove(item)` — the row hover writes into the SAME hover collection the viewport
raycast writes into, so the highlight the 3D view already draws for a mouse-over is
what the list gets, free. (The same file's `select` handler goes through
`editor.selection.selected`, which is why hover and selection read as one language
there and not two.)

KIWI reproduces the SHAPE and not the plumbing: `pick_result_t` is a single raycast
result and is not a set, so instead of pushing rows into it the outliner publishes a
small separate HOVER TARGET that `KiwiHover_DrawWorld` consumes in the same pass,
with the same fill colour, right after the raycast hover.

THE CONTRACT, which is what keeps it from leaking stale pointers:

* `KiwiHover_OutlinerClear()` runs at the TOP of every outliner draw (before the
  early-out, so closing the panel with a row hovered cannot burn a highlight in), so
  a target survives exactly one frame unless the row re-publishes it;
* every brush is `Sel_BrushLive`-guarded at DRAW time, not at publish time — a row
  can be hovered and the brush deleted by a hotkey in the same frame;
* the brush list is bounded (`KHOVER_OUT_MAX` 96) and shares the existing
  per-frame fill cap, so a hovered 500-brush func_group degrades against the same
  ceiling every other fill answers to. Past the cap it says so, once.

A CURVE is not a brush and is not drawn by `kiwi_hover.cpp`, so its half is one more
colour run in `KiwiCon_DrawWorld`, after both selection passes, in the same hover
cyan.

---

### 61.7 ITEM 6 — the marquee shows its work

**USER DIRECTIVE, verbatim:** *"While box selecting, it should highlight the items
in realtime as the box goes over them (quality of life)."*

Plasticity's `BoxSelection` re-runs its box intersection on every pointer move and
pushes the result into `selection.hovered` — what the user sees mid-drag is the
ordinary hover highlight, on a set. KIWI does the same two things: re-run **the same
`Collect()` the release will run**, and draw the result in the §18 hover cyan.
Reusing Collect is the whole point — a preview computed by a second, cheaper test
would be a preview that lies, and a preview that lies about a selection is worse
than none.

What holds the cost:

* **It only re-collects when the rect MOVED** (`KBOX_PREVIEW_EPS` 2 px on any edge,
  compared against the last COLLECTED rect so sub-threshold motion accumulates
  rather than being lost). A held-still mouse costs nothing, and `KiwiBox_Update`
  only requests a repaint when the cursor actually moved.
* **The centre-ray rescue is skipped.** `Collect()`'s tail fires one full `Pick()`;
  that is worth it once at the release and not once per drag frame. The honest
  consequence: a crossing rect entirely inside one big face previews nothing and
  then selects that face on release.
* **It draws OUTLINES, not fills.** The fill emitter is one `RC_DRAW_TRIS` command
  per face — right for one hovered item, wrong for a marquee that can name a
  hundred.
* One hard segment budget (900). Past it the preview is partial; the SELECTION is
  not, and cannot be — this pass reads state and emits lines.

EDGE and VERTEX previews are deliberately absent: at those granularities the marquee
names dozens of handles the ported vertex pass is already drawing.

---

### 61.8 ITEM 9 — box-selecting faces

**USER DIRECTIVE, verbatim:** *"Allow box selecting of faces on a solid. This is
needed for complex shapes with lots of small brushes."*

**THE ARM ALREADY EXISTED** and has since round U: mode 3 sets `SEL_MASK_FACE`,
`RectKind` answers `SEL_FACE`, and the marquee walks every winding. What made it
unusable is that it had **no facing test**, so a box over one wall took that wall's
face AND the face on the far side of the same brush AND both faces of everything
behind it. On the "lots of small brushes" geometry the directive is about, one drag
produced a selection several times larger than what the user was pointing at — which
is indistinguishable from "it does not work".

**WHICH WITNESS:** the EXISTING shapeTest over every winding point, unchanged. The
brief offered "centroid, or any vertex — pick one and justify"; the justification
for picking NEITHER is that the brush marquee's crossing/containment semantics are
already defined by this test, and a face marquee answering a DIFFERENT question from
the object marquee in the same rect would be a second grammar. A centroid test would
also silently refuse a big face the rect sits inside — precisely what the centre-ray
rescue exists to stop.

**IT IS A FACING TEST, NOT AN OCCLUSION TEST.** A front-facing face behind a wall is
still collected, exactly as the object marquee still collects the brush behind the
wall. "Match the brush marquee's semantics" is the rule, and occluding would need a
ray per face per frame.

---

### 61.9 ITEM 4 — Shift+C is the circle

**USER DIRECTIVE, verbatim:** *"Shift-c should be circle, change the keybinds so it
is so."*

**THIS OVERRIDES PLASTICITY, KNOWINGLY.** Their table is `shift-c` → corner-box,
`shift-v` → center-box (default-keymap.ts:288-289), and shakeout F reproduced it key
for key. The user's own hand wins over an upstream convention; §16b's job was to
give KIWI a coherent creation table, not to make it byte-identical to another
editor's.

**IT IS A STRAIGHT SWAP, NOT A DISPLACEMENT:** the circle vacates Shift+W and the
corner box takes it, so no third command has to move and no chord is dropped. What
the swap costs is the corner-box / centre-box adjacency (C/V) — the corner box is
now one key from its own Shift+Q corner rect, which is arguably the better grouping
anyway (the two CORNER draws together).

**THE FINAL CREATION-CHORD TABLE (modern profile):**

| chord | command | id |
|---|---|---|
| `Shift+A` | Construct: Line (chained curve) | 34034 |
| `Shift+S` | Construct: Spline | 34050 |
| `Shift+Q` | Construct: Rectangle (corner) | 34036 |
| **`Shift+C`** | **Construct: Circle (centre)** | **34037** |
| `Alt+V` | Construct: Rectangle (centre) | 34047 |
| **`Shift+W`** | **Box (corner)** | **34051** |
| `Shift+V` | Box (centre) | 34064 |
| `Shift+X` | Cylinder | 34052 |
| `Shift+Z` | Sphere | 34053 |
| (unbound) | Polyline · 2-pt Circle · Arc · N-gon · Cone · Extrude Region · Add Menu | — |

Both keys were re-audited in full. vk 0x57 (W): mods 0 ConnectSelection 33021 ·
**1 the corner box** · 2 SplaySelection 33157 · 3 TogglePatchWireframes 32857 ·
5 MakeWeaponClip 196. vk 0x43 (C): mods 0 KIWI_CMD_CUT · **1 the circle** ·
2 AutoCaulk 33220 · 3 CapCurrentCurve 32885 · 4 Copy 33039 · 5 ToggleCamera 33069 ·
6 CameraDown 33056. Neither key is in `res/radiant.rc`'s accelerator table
(:506-516 is Ctrl+O/S/L/P/K/M and Ctrl+Delete / Ctrl+Insert and nothing else), which
matters because `TranslateAccelerator` runs BEFORE the hotkey table
(radiant_main.cpp:651 then :675).

The add menu, the palette, the hint chips and the menu-bar annotations all read the
LIVE table and follow automatically. The one user-visible string in the build that
hardcoded these chords is the Keymap-profile tooltip (kiwi_keymap.cpp) — which was
already stale on Shift+V from round AF — and it is rewritten from the live table.

---

### 61.10 ITEM 11 — light grid snapping, and the partial walk-back of D-Z2

**USER DIRECTIVE, verbatim:** *"There should be light snapping to the global grid
(disabled with ctrl)."*

Round Z item 2 (D-Z2) made the three ONE-AXIS gestures raw by default on the user's
own report (*"When extruding, it should not snap by default […] It's just not good
to use in a cluttered scene"*). **Both directives are right, and they are not in
conflict once you separate the two things "snapping" meant:**

* **THE GEOMETRY ARMS** were what made a cluttered scene unusable — the query is
  RANKED, it reaches across the whole viewport, and it drags the gesture onto
  whichever of a hundred nearby edges/verts/faces won. Those stay OFF by default.
  **D-Z2 is not walked back.**
* **THE GRID** is a fixed, predictable, global lattice that is exactly where a
  mapper wants to land, and it was collateral damage: turning off the ranked query
  also turned off the grid quantisation, so a plain drag could not land on a round
  number without Ctrl — which brings the clutter back with it.

LIGHT means two things, both load-bearing: **grid only** (no geometry candidates,
ever), and **a capture band, not a quantiser** — the value is nudged only when it is
already within `KSNAP_LIGHT_BAND_PIX` (5) screen pixels of a lattice value, and
outside the band it passes through completely untouched. So free dragging still
feels continuous; the lattice reads as a magnet rather than as a ratchet. That
difference is the whole directive: full quantisation IS what Ctrl already does. The
band is in screen pixels (converted at the gesture's own depth) so the stickiness
feels identical at any zoom, and it is capped at `KSNAP_LIGHT_MAX_FRAC` (0.22) of a
cell so zooming out cannot widen it into full snapping.

Which coordinate goes on the lattice is round P's rule, reused verbatim rather than
re-decided: when the axis IS a world axis the ABSOLUTE world coordinate is snapped
(an off-grid start does not drag its offset along — "the global grid" means the
global grid); otherwise there is no world coordinate to be on the grid of and the
DISTANCE is snapped.

Applied at the three raw-by-default gestures and nowhere else: the region extrude,
the face extrude / un-extrude, and the face push/pull. The numeric field still
outranks everything, Ctrl still runs the full ranked query, and
`KEXT_SELF_SNAP_BAND` is untouched (it lives on the geometry arm).

---

### 61.11 ITEM 8 — the EXPERIMENTAL patch mode

**USER REPORT + PROPOSAL, verbatim:** a boolean'd cylinder arch comes out as *"a fan
of sliver faces"* and *"texturing becomes hell"*; *"In normal cod4, they use patches
for curves. Maybe you could add an experimental patch hybrid option for the
circle/cylinder (any round) tools? What do you think?"*

**YES, AND IT IS THE AUTHENTIC ANSWER RATHER THAN A NEW IDEA.** Stock Radiant's own
Curve > Cylinder is exactly this — mainfrm.cpp:3831 `Cmd_OnCurvePatchtube` calls
`Patch_BrushToMesh( 0, 0, 0, 0 )` (pmesh.cpp:1281), which throws the box away and
leaves a 9x3 `PATCH_CYLINDER`. A CoD4 mapper's round geometry IS patches; the
faceted brush cylinder is what this editor had, not what the format wants.

**WHY IT IS NOT `Patch_BrushToMesh`,** three hard reasons — and
`kiwi_patchfillet.cpp` reached the same conclusion for its own quarter-cylinders
(:1145-1155):

1. it gates on `QE_SingleBrush()` — exactly one brush SELECTED — so it cannot be
   driven from a tool that has not landed anything;
2. it derives the ring from `def->mins/maxs` only, i.e. from an axis-aligned box, so
   the radius and the working plane the user just drew would be discarded;
3. it ends with `Select_Delete()`, destroying the source.

So the mode reproduces the SEQUENCE — `MakeNewPatch` -> fill `ctrl` -> materials ->
`KiwiMtl_RealizePatch` -> `Patch_KiwiFinishNew` -> `AddBrushForPatch` ->
`Brush_AddToList` -> `Brush_AddToList2` — and supplies its own control net.

**THE OVERSHOOT.** A quadratic bezier column pair per quarter arc: width =
`spans*2 + 1`, EVEN columns on the circle at radius r, ODD (handle) columns pushed
out to `r / cos(alpha/2)`. That is what makes the quadratic interpolate a true
circular arc, it is `kiwi_patchfillet.cpp`'s own `ArcPoint` rule (:206-220), and at
the 4-span 90-degree case it evaluates to `r*sqrt(2)` — precisely what
`Patch_BrushToMesh`'s corner construction produces, so the two agree.

**SPANS ARE FIXED AT 4, AND THAT IS NOT A LIMITATION.** A patch's smoothness comes
from its TESSELLATION, not from how many control points it has — that is the entire
point of using one. Four quarter-arc spans describe a circle EXACTLY. More columns
would not make the curve rounder; they would only make it harder to edit. So the
`sides` count and the `[` `]` keys are simply not the knob in patch mode, and the
HUD says so. (The format's own ceiling is 15 usable columns —
`Patch_GenericMesh` refuses a width outside 3..15, pmesh.cpp:1550 — i.e. 7 spans, so
there is room for a user-facing span count later.)

**NO CAPS AND NO COLLISION, per stock CoD4 practice.** A patch is a render surface;
the mapper puts a caulk brush inside it. The creation line says so every time, the
HUD says so at every stage, and the mode is marked EXPERIMENTAL in both.

`P` toggles it mid-gesture and the choice is REMEMBERED across sessions
(`RoundToolPatch` in the KiwiUX profile section), exactly as round AF's side count
is. **Only the cylinder offers it**: the cone and the sphere have no stock patch
spelling this could be faithful to (`Patch_BrushToMesh`'s cone arm collapses a ring
to a point and its hemisphere type has no creation path at all), and the box is not
round.

**THE ARCH WORKFLOW is documented rather than given a dedicated tool**, as the brief
allowed: draw a patch cylinder through the wall, delete the half you do not want (or
build the wall around it), and put a caulk brush inside for collision. The
CIRCLE-when-extruded patch arm is NOT in this round — see KNOWN_ISSUES.

---

## Decision log — round AG

- **D-AG1 — a weld may never exceed 40% of the finest edge it is applied to.** The
  round-AF grid scaling is right for USER-PLACED endpoints (which snap) and wrong for
  TESSELLATED ones (whose spacing is 0.098*r and has nothing to do with the grid).
  Bounding it by the input is the one rule that keeps both: the grid still wins
  wherever the geometry is coarse enough to allow it.
- **D-AG2 — `KREG_MAX_LOOP` is DIVORCED from `KCON_SEGS_MAX`, and
  `KEXT_MAX_PROFILE` is DEFINED AS `KREG_MAX_LOOP`.** They answer different
  questions (how finely one object is tessellated vs how many corners a derived cell
  may have), and a profile cap below the loop cap breaks "extrudable <=> filled" by
  construction. Defining one as the other makes the invariant structural rather than
  a thing to remember.
- **D-AG3 — a cap that bites PRINTS.** Both the arrangement's cell drop and
  `AcceptLoop`'s were silent `continue`s, and that is what made the unfilled arch
  unanswerable from inside the editor. Throttled by store generation.
- **D-AG4 — PASS 3b is a second sweep, not a restructure of the group loop.** The
  group loop is delicate (the seed-plane fix, the touch-first partner rule) and the
  gap is narrow: planes with no open object on them. A guarded second pass costs
  nothing where the store already has open geometry and cannot double-emit.
- **D-AG5 — the grid gains REACH, never DENSITY, from the frustum.** v2 died letting
  the frustum set the spacing. Here it answers one boolean and the ring it enables
  has a fixed spacing and a fixed reach, so a pitch change can never alter a line of
  the near lattice. Latched, and capped at 8R.
- **D-AG6 — the far ring gets no halo.** It is dim distant context; doubling its cost
  to soften it would spend the budget on the least informative half of the picture.
- **D-AG7 — hide gets its OWN undo domain, not a legacy record.** Not a preference:
  the hidden bit lives on the INSTANCE and the legacy snapshot is of DEFS, whose
  re-create path memsets the instance to zero. A legacy record would destroy the
  state it was meant to save.
- **D-AG8 — the hide snapshot restores ONLY bit 2 and `xx5`.** The rest of
  `brushFlags` belongs to the filter cache, the layer system and the selection;
  restoring the word would make Ctrl+Z on a hide re-select brushes and stomp layer
  visibility. Round J's fear was right about scope even though its ruling was wrong
  about undo.
- **D-AG9 — `ToolSides()` reports AUTO rather than 0.** In this HUD a field with no
  value does not exist, so "0 means automatic" was indistinguishable from "there is
  no such field". AUTO is an answer.
- **D-AG10 — the circle's segment count rounds UP to a multiple of four.** Rounding
  to NEAREST would silently give less smoothness than was asked for. The cardinals
  are what make a scaled circle a usable oval, so they are not optional and the
  rounding is announced in the HUD rather than discovered from the geometry.
- **D-AG11 — the ARC does not get cardinal rounding.** Its count is taken pro rata
  over an arbitrary sweep, so there are no cardinals to hit and forcing a multiple of
  four would only coarsen it.
- **D-AG12 — every source of a multi-source extrude grows along ITS OWN normal.**
  Section 20's own rule for the multi-face push/pull. A shared world direction would
  push a 90-degree-away source sideways through its own solid.
- **D-AG13 — a multi-source extrude is ALL-OR-NOTHING.** One gesture, one undo
  record; a record has to be describable by one Ctrl+Z. The single exception is an
  extra face that will not profile at all, which is dropped and counted — the drive
  face is the one the user aimed at.
- **D-AG14 — the face CARVE stays single-source.** Each face would have its own
  destroy depth; some would delete their brush and some would not, and one Ctrl+Z
  would undo a mixture the user never described. Refused out loud.
- **D-AG15 — mixed regions + faces is REFUSED, not forced.** They are two different
  commands. It did not fall out naturally, and inventing a third dispatch to fuse
  them would be a grammar nobody asked for.
- **D-AG16 — the outliner hover is a ONE-FRAME LATCH, cleared before the panel
  draws.** No subscription, no lifetime to manage, and a brush deleted between the
  publish and the draw is caught by the draw-time `Sel_BrushLive` guard rather than
  by bookkeeping.
- **D-AG17 — the live marquee preview reuses `Collect()`.** A preview computed by a
  cheaper test would be a preview that lies about a selection, which is worse than
  no preview. The cost is paid down by throttling on rect CHANGE and by dropping the
  centre-ray rescue, not by changing the answer.
- **D-AG18 — the face marquee gains a FACING test and not an OCCLUSION test.** The
  directive is "match the brush marquee's semantics"; the object marquee still takes
  the brush behind the wall, so the face marquee still takes the face behind it. The
  facing half is what removes the back faces that made the gesture unusable.
- **D-AG19 — Shift+C / Shift+W is a straight SWAP.** The user's directive outranks
  Plasticity's table; a swap displaces nobody and drops no chord, which is cheaper
  than the house two-step it would otherwise cost.
- **D-AG20 — D-Z2 IS REFINED, NOT REVERSED: default = LIGHT GRID snap, Ctrl = the
  full ranked query.** The user's round-Z complaint was about the GEOMETRY arms in a
  cluttered scene; the grid was collateral damage. A capture band restores the
  lattice without restoring the clutter, and a band is categorically not a
  quantiser — full quantisation is what Ctrl already does.
- **D-AG21 — patch mode is a MODE on the cylinder, not a fifth `primKind_t` and not
  a new command id.** It produces the same shape from the same gesture with the same
  fields; only the medium changes, and the modal id block has four ids left
  (34066-34069) that a medium toggle has no claim on. It has visible state (the HUD
  owns the whole strip in patch mode), which is what D-AF17 required of any
  in-command toggle.
- **D-AG22 — the patch cylinder's control grid is FIXED AT 4 SPANS.** A patch's
  smoothness is its tessellation, not its control-point count; four quarter arcs
  describe a circle exactly. More columns would make it harder to edit and no
  rounder. `sides` is therefore not the knob, and the HUD says so instead of
  offering a number that does nothing.
- **D-AG23 — the patch cylinder has no caps and no collision, and says so every
  time.** That is stock CoD4 practice rather than an omission, and a silent one
  would be a trap. It is also why the mode is EXPERIMENTAL and opt-in.

## 62. Round AI — six reports

Three bugs with real root causes (1, 2, 5), one strength calibration that turned
out to be a missing channel rather than a missing number (4), one UI addition that
was worth generalising (3), and one legacy feature revived by routing rather than
by resurrection (6).

---

### 62.1 ITEM 1 — the thumbnails that turn solid red

**USER REPORT, verbatim:** *"bug in textures view where when selecting a texture,
it sometimes turn the other textures in the viewport solid red"* — with two
screenshots of the SAME material, once as a solid red quad and once drawn
correctly with a thin red border.

**THE BORDER IS A STATE-DESTRUCTIVE DRAW, AND IT SAYS SO AT ITS OWN DEFINITION.**
`R_AddCmd_Line2D` (r_rendercmds.cpp:2028-2035) is a pure forwarder in the binary;
under `KISAK_RADIANT` it routes through `Ed_EmitLineBatch` (r_rendercmds.cpp:1976),
which **pushes the run's first vertex colour as `MATERIAL_COLOR`** (:2004) and
never puts it back. That adaptation exists for a real reason — kisak's editor
`$line` technique colours from `CONST_SRC_CODE_MATERIAL_COLOR`, not from the
per-vertex colour the binary relies on — and r_rendercmds.cpp:1920 states the
consequence outright: *"MATERIAL_COLOR persists in backend state until the next
set."*

The selected thumbnail's frame is drawn in `d_savedinfo.colors[10]` =
**{1, 0, 0, 1}** (win_qe3.cpp:423). So after `R_DrawOutlineRect` the backend's
`MATERIAL_COLOR` is **red with w == 1** — and `w` is exactly the flat-override
weight the thumbnail shader lerps by:

    rgb = w * (matColor.rgb - vColor*colorMap) + colorMap*vColor

which at `w == 1` is `matColor.rgb`, i.e. **flat red, whatever the texture is**.
Every `R_AddCmdDraw2DImage` emitted after the border in the same frame collapses.
That derivation is not new — it is written out on `TexWnd_Paint`'s own
`MATERIAL_COLOR` seed (texwnd.cpp:1052-1065), which sets `w == 0` for precisely
this reason and notes that the earlier `{1,1,1,1}` "FULLY replaced every thumbnail
with flat white".

**WHY "SOMETIMES", AND IT IS NOT RANDOM — IT IS POSITIONAL.** The frame openers
(`TexWnd_Paint`:1066 and `TexWnd_RenderToRT`:1110) re-neutralise `MATERIAL_COLOR`
at the top of every paint, so the damage never survives a frame. Within a frame it
reaches **exactly the thumbnails emitted after the selected one**. Scroll the
selection off the top — the vertical cull `continue` at texwnd.cpp:1000 skips its
border entirely — or land it on the last visible cell, and the grid is perfect.
That is why the same material appears correct-with-a-border in one screenshot and
solid red in the next: in the second it is not the selected one, it is merely
downstream of it.

**THE FIX IS THE BRACKET DISCIPLINE EVERY OTHER PASS IN THIS EDITOR ALREADY
FOLLOWS.** Set, draw, put it back — xywnd.cpp:1148/1190 (whose restore carries the
literal comment "restore for subsequent passes/frames"), camwnd.cpp:1437/1439,
kiwi_hover.cpp:497/521, kiwi_region.cpp:1290/1409. **texwnd.cpp was the one drawing
path in the repo that set a non-neutral colour and never restored it.** The restore
goes INSIDE `R_DrawOutlineRect` rather than at its two call sites, because the
layered sub-view's per-row frame has the identical hazard and is worse: its frame
colour for a non-active row is `colors[8]` = black with `w == 1`, which would tint
the next entry's layer thumbnails **solid black** by the same mechanism.

`CLayermatWnd_OnPaint` also gains the `{1,1,1,0}` seed every other window already
has. It was the one thumbnail-drawing paint in the editor that seeded nothing, so
it inherited whatever the previously painted window parked. With the texwnd fix
that is much less likely to be hostile; the seed makes it an invariant.

---

### 62.2 ITEM 2 — the height that was already -140 yd

**USER REPORT, verbatim:** *"when creating a box (or other shape) with the camera
perfectly aligned to TOP, when it's time to do the Height(Z), i move the camera and
the height is already set to a huge negative number. This needs to be fixed and it
should be zero until I move my mouse in a way that's able to portray Z movement.
It's impossible to portray Z movement while at top/bottom camera lock."*

**THE VALUE IS NOT STALE — IT IS A REAL SOLVE OF A DEGENERATE SYSTEM.** Every
one-axis gesture in this editor maps the cursor to a scalar through the same five
lines of algebra, `RayAxis( ray, pt, axis )` — the closest point on the world line
`(pt, axis)` to the cursor ray. Its denominator is

    den = 1 - dot(axis, ray.dir)^2 = sin^2(theta)

and the solve is amplified by `1/den`. A one-pixel cursor move at ground distance
`d` moves the answer by roughly `d / (f * sin^2 theta)`, where `f ~ 940` is
CameraCalcRayDir's own pixel scale (camwnd.cpp:3090). At theta = 5 degrees that is
**70 world units per pixel at d = 500**. And the local guards in all six copies of
`RayAxis` read `den < 1.0e-4`, which only refuses **theta < 0.6 degrees** — so
everything between 0.6 and ~10 degrees SOLVED, loudly and wrongly, was quantised to
the grid, stored in the command's scalar and sat there. Orbiting away did not
create the -140 yd; it made an already-latched value visible.

**TWO GATES, ANSWERING DIFFERENT QUESTIONS.**

**THE VIEW GATE** — `KiwiCam_AxisPortrayable( axis )`, kiwi_camera.h. Reads the
CAMERA's `vpn`, not the cursor ray, so it is **one stable boolean per frame**: it
does not flicker as the cursor crosses the screen, and the HUD can name the remedy.
A gesture that fails it **HOLDS** its scalar (not resets — a value set from a
workable angle survives an orbit through the top) and **REBASES** when the gate
re-opens. Ortho is not special-cased: an ortho top view has the same `vpn` and a
worse degeneracy (every ray is exactly parallel to Z), so the same test refuses it.

**THE SAMPLE GATE** — `KCAM_RAYAXIS_MIN_DEN`, in all six `RayAxis` copies. The view
gate cannot be the whole answer: with a 65-degree FOV a ray at the edge of the image
is up to 32 degrees off `vpn`, so even at a legal `vpn` SOME pixel on screen still
looks straight down the axis. Each copy now refuses its own sample. The cost of a
refusal is that the scalar does not move for those pixels — a small dead disc around
wherever the axis is end-on on screen — which is strictly better than a jump.

**THE THRESHOLD IS 0.97 (14.1 degrees), and it is the same angle both times**
(`KCAM_RAYAXIS_MIN_DEN` is `sin^2` of it, so they cannot drift). At 14 degrees the
amplification is 17x — about 9 world units per pixel at d = 500, which the grid
quantise absorbs. Tighter (0.99 / 8 degrees) is 51x and unusable; looser refuses a
legitimately steep but workable view.

**REBASE IS NOW INSIDE THE LATCH, AND THAT IS A REAL CONSOLIDATION.** Both extrude
commands used to carry the correction in `Rebase()` (`const float keep = m_dist;
LatchStart(); m_start -= keep;`). It lives in `LatchStart` / `LatchHeightStart`
itself now — `m_start = dot(...) - m_dist` — because the view gate needs the same
preservation and two copies of it would have been a double-subtract waiting to
happen. `Rebase()` is a bare `LatchStart()` call in both.

**WHAT EACH GESTURE DOES NOW.** The box / cylinder / cone height stage holds at 0
and says *"ORBIT to set height — this view looks straight along Z"*; a click while
blocked refuses with a console line naming the camera instead of the geometry.
Region extrude and face extrude hold their distance and say the same thing about
their own axis. The Move command's face push/pull and axis-constrained drags get
the sample gate plus **one console sentence per refusal run** — a silent refusal on
the single most-used gesture in the editor (push a wall face while looking at the
wall) would be worse than the bug it replaces.

**Numeric typing works at every angle, in every one of them.** It never went
through the cursor mapping.

---

### 62.3 ITEM 3 — the loft panel, and why it is not a loft panel

**USER DIRECTIVE, verbatim:** *"The lofting is pretty cool, but make the options
clickable buttons like in plasticity. Open a temp lofting panel (still allows
enter/rightclick completion)."*

**THE GENERIC VERSION WAS THE CHEAPER ONE.** A command's UI surface was already
mostly declarative — `NumericFields` declares its scalars, `HudPrompts` declares its
keycaps — and the ONLY thing a running command could not describe to a UI was a
BOOLEAN or an ENUM. Loft's Ruled/Tangent existed solely as the `D` key and a
substring of `HudStatus()`. Three virtuals close that for every command at once:

    virtual int  CommandOptions( const kiwiOption_t **out ) const;
    virtual int  OptionValue  ( int opt ) const;
    virtual void OptionChanged( int opt, int value );

with four row kinds — `KOPT_ENUM` (a segmented button group), `KOPT_TOGGLE`,
`KOPT_INT` (a `- N +` stepper) and `KOPT_NUMFIELD` (a slider that reads and writes
one of the command's existing NUMERIC FIELDS, so there is no second store for a
number). A row may be gated on another row's value (`enabledBy`), which is how
`tension` greys out in RULED mode.

**PLASTICITY'S SHAPE, AND THREE FACTS COPIED FROM IT.** A command constructs a
dialog over its factory's params and hands it a re-run callback
(`plasticity/src/commands/loft/LoftCommand.ts:17-21`); one generic `onChange`
writes `params[name] = value` and fires that callback
(`plasticity/src/command/AbstractDialog.ts:48-55`). An enum is a row of hidden
radios sharing one `name` with styled labels
(`plasticity/src/commands/fillet/FilletDialog.tsx:78-81`); a disabled row is
DRAWN, not hidden (`FilletDialog.tsx:73`); a scalar is a
`<plasticity-number-scrubber>` (`LoftDialog.tsx:27-28`). Then:

1. **IT IS SCOPED TO THE VIEWPORT, NOT THE APP WINDOW.** `Dialog.tsx:32` renders
   `absolute bottom-2 left-2 w-96` and `index.html:31` mounts the host INSIDE
   `<plasticity-viewport view="3d">`, so "absolute" resolves against the 3D view's
   rect. KIWI anchors to the camera IMAGE rect, which is the same rectangle —
   `ImGuiShell_CameraImageRect` publishes what the terrain-paint bridge already
   recorded.
2. **IT NEVER TAKES KEYBOARD FOCUS.** `Dialog.tsx:45-47` puts `tabIndex={-1}` on
   both footer buttons. KIWI's equivalent is `NoNavFocus | NoFocusOnAppearing` plus
   a widget set with no text field, so `ImGuiShell_WantsKeyboard` stays false and
   Enter / Esc / Tab keep reaching the command.
3. **THE FOOTER IS Cancel + OK AND THEY CALL WHAT THE KEYBOARD CALLS**
   (`Dialog.tsx:44-47`). KIWI's two call `KiwiCmd_Cancel` and `KiwiCmd_Confirm` —
   i.e. Esc and Enter, and `KiwiCmd_Confirm` **is** what the RMB release calls
   (kiwi_command.cpp:1351 feeds VK_RETURN). "Still allows enter/rightclick
   completion" is therefore not a special case: nothing about either changed.

**ONE DEPARTURE.** Plasticity's dialog sits bottom-left, which in this editor is
where kiwi_hints.cpp already puts the prompt chips (`KHINT_PROMPTS_LEFT`). The
panel takes the viewport's LEFT edge at upper-third height instead — clear of the
chips (bottom), the numeric HUD (bottom-centre) and the view cube (top-right), and
never under the cursor, which mid-gesture is in the middle of the image.

**LOFT DECLARES THREE ROWS AND OWNS NO NEW STATE.** The mode button calls
`KeyDown( 'D' )` — literally the key's own call, so the "a mode flip re-seeds the
density unless it was typed" rule (`m_segsTyped`) cannot be forgotten by one path;
the density stepper goes through `NumericFieldChanged( 0 )`, where the clamp and
that latch live; the tension slider is a `KOPT_NUMFIELD` on field 1. The panel is
offered at stage `KLOFT_LIVE` only — at the pick stage there is no second face, no
stations, and `Rebuild` has never run.

---

### 62.4 ITEM 4 — the hover was missing a channel, not an alpha

**USER DIRECTIVE, verbatim:** *"The hover highlighting is too weak. Should be the
same as when a brush is selected. Fix it."*

**WHAT "SELECTED" ACTUALLY MEASURES.** A selected BRUSH in the camera is **two
channels** (camwnd.cpp:2755-2790): a `MATERIAL_COLOR` tint of `colors[11]` =
{1, .25, .25, .25} over the whole TEXTURED surface — `vertcol_shaded` lerps, so the
result is `0.75*texture + 0.25*red` — **and a full white wireframe on top of it**.
The ported selected-FACE fill is the same 0.25 as a straight alpha (`colors[16]`,
win_qe3.cpp:429) drawn through the identical `d_white` / `TECHNIQUE_UNLIT` route
`EmitFaceFill` uses, so the alphas in kiwi_hover.cpp and that 0.25 are directly
comparable quantities rather than two different scales.

**THE HOVER WAS BELOW IT ON BOTH.** 0.18 of wash against 0.25 — and, the bigger
half, **the outliner hover (round AG) drew a fill and no outline at all**, so it
was missing an entire one of the two channels a selection has. No alpha could have
closed that; the alpha was only the visible symptom. The marquee preview had the
opposite gap: an outline and no fill, at width 1.

**NOTHING ELSE WAS DIMMING IT**, and that was checked before touching a number:
`Byte4PackPixelColor` is a straight float-to-byte pack, `s_color[i]` is the same
bit-cast packed word the ported batcher uses, the draw is `d_white` +
`TECHNIQUE_UNLIT` with no second colour term, and the bracket around the pass is
the NEUTRAL {0,0,0,0} that hands the draw to the per-vertex colour. There is no
fade, no multiplier and no second alpha anywhere in it.

So: the raycast face hover goes 0.18 to **0.26** (the ported selection tint's own
weight); the outliner hover goes 0.26 to **0.34** and **gains a cyan wireframe at
width 2**, the "grab me" weight `DrawSelectedAccents` already spends on the same
argument; the marquee preview's outline goes to **width 2** for the same reason —
it has one channel and has to carry the weight of two, because a per-brush fill
over a marquee's worth of geometry is not affordable (the fill emitter is one draw
command per face, capped at 64 a frame, and a marquee routinely names more).

---

### 62.5 ITEM 5 — the face marquee gets the gate round AG left out

**USER DIRECTIVE, verbatim:** *"when box selecting the faces, it shouldn't
penetrate through any brushes, just the visible faces. also faces that are only
visible by 1 pixel (90 degrees facing left->right of the camera) shouldn't be
picked up either (So when I'm at a perfect south orientation, I can box select
every face on that south side without worrying about east/west, etc.)"*

Round AG's facing gate said in as many words what it did not do — *"it is a FACING
test, not an OCCLUSION test. A front-facing face BEHIND a wall is still
collected"* — and logged it. Two gates close it, and they answer different
questions.

**EDGE-ON IS ABOUT PROJECTED AREA.** `|n . d|`, with `d` the eye-to-centroid
direction, **is** the foreshortening factor: a face at 0.12 shows 12% of its true
area, which at a perfect south view is exactly the east/west walls the directive
names. `KBOX_FACE_EDGEON_DOT = 0.12`. It is a dot product, so it runs in the live
preview as well as the release and costs nothing.

**OCCLUSION IS ABOUT WHAT IS IN FRONT.** One ray from the eye to a witness point;
if the first thing it hits is a DIFFERENT brush, the face is behind something.
Three decisions inside that:

* **THE QUERY ASKS FOR `SEL_MASK_OBJECT` AND COMPARES THE BRUSH**, not the face. An
  object query always resolves — kiwi_pick.cpp's face granularity requires
  FACE-without-OBJECT and returns invalid on a patch or model hit — so **a patch or
  a prefab in front of the face occludes it too**, which a face-granular query would
  have silently let through. Comparing the brush is sufficient because the facing
  gate has already dropped this brush's own back faces and a brush is convex by
  construction. `Pick` does the round-X `selUnmask` internally, so a SELECTED brush
  in front still occludes.
* **IT FAILS OPEN.** When the ray hits nothing the face is ADMITTED. `Test_Ray` runs
  under `Pick_CameraContents`, so a filtered content type — a tool brush, a trigger
  — can be un-hittable while being perfectly visible and perfectly selectable, and
  rejecting on "no hit" would make those faces unmarqueeable. The gate only ever
  rejects when it positively identifies something else in front.
* **FOUR WITNESSES, TRIED LAZILY.** The centroid first; only if that is occluded,
  three more points halfway from the centroid to spread winding corners. A face
  whose middle is behind a pillar but whose body is plainly visible would otherwise
  be refused — and the cost profile is right, because a VISIBLE face (the common
  case, and the one being dragged over) still costs exactly one ray.

**THE PREVIEW IS A SUPERSET, NOT A SUBSET.** The occlusion gate is the only part
that costs a ray, and the preview re-runs on every 2-pixel rect change, so it gets a
budget of 96 rays against the release's 8192. Past the budget the gate is SKIPPED
(admit), so the user may see one face highlighted that the release then drops, but
can never have something taken that was never shown. Both walks share
`CollectFromBrush`, so preview and release agree by construction wherever the budget
holds.

The ray is spent LAST, after the shape test has already decided the rect wants the
face — so it is never paid for a face the marquee was not going to take anyway.

---

### 62.6 ITEM 6 — patch vertex mode, revived by routing

**USER DIRECTIVE, verbatim:** *"Vertex Mode (V) (Legacy Feature). I want you to
enable use of the old vertex mode that allows you to move points. This is used for
q3 curve manipulation. Right now pressing V opens it but it doesn't allow dragging.
Fix it so it works and give it the modern gizmos that make it easier to handle."*

**THE V BINDING — THE FINAL RULE, and it changes no chord:**

| when | V means |
|---|---|
| a modal command is running | **the movable pivot** (section 29) — command-local, unchanged |
| nothing running, selection HAS a patch | **patch vertex mode** (toggle) |
| nothing running, no patch | **the classic 33005 Drag Vertices**, byte for byte |

V was never globally bound to the pivot and kiwi_keymap.cpp:705-715 says so at
length: the key funnel offers every key to the live command before the hotkey table
is consulted, so vk 0x56's global occupancy was always undisturbed. Bare V is
33005 in BOTH keymap profiles. Rule 2 is a REFINEMENT of what 33005 already did —
its own body routes a pure-patch selection to `Patch_EditPatch`
(mainfrm.cpp:2223-2228) — it just had no working drag on the other side.

**"IT OPENS BUT DOESN'T ALLOW DRAGGING" IS THREE INDEPENDENT KILLS**, any one
fatal:

1. **THE CAMERA NEVER REACHES `Drag_Begin`.** Its only camera route is
   `CamWnd_OnLButtonDown` (camwnd.cpp:3358), and `KiwiVP_CameraButtonDown` returns
   true for a bare LMB unconditionally (kiwi_viewport.cpp:639-677), so `VP_Down` is
   skipped (imgui_shell.cpp:386-391). Move and up likewise. kiwi_viewport.cpp:394-404
   already records the legacy 3D marquee as "UNREACHABLE from any modern flow" for
   exactly this reason.
2. **THE MODE IS RESET ON EVERY MODERN CLICK.** `Sel_SyncToLegacy` opens with
   `Select_Deselect(1)` (kiwi_selection.cpp:314), which zeroes `d_num_move_points`
   (select.cpp:1446) and calls `ResetSelectMode()` (select.cpp:244-252), writing
   `d_select_mode = sel_brush`.
3. **THE HANDLE LISTS ARE EMPTY.** Shakeout D removed the SEL_VERTEX to
   `selected_brushes` promotion, and every legacy handle builder
   (`SetupVertexSelection`, `Patch_EditPatch`) walks `selected_brushes`.

**SO IT IS NOT RESURRECTED — IT IS ROUTED, and the destination already existed.**
Punching a hole through the modern dispatch for one mode would fix (1) and leave
(2) and (3). The modern layer already owns patch control points completely:
`kiwi_pick.cpp:336-359` picks one by screen distance (8 px) on any pickable patch,
selected or not; `kiwi_boxselect.cpp`'s SEL_VERTEX arm marquees them;
`kiwi_transform.cpp` MOVES them — `m_verts[i].patchPoint` (:1912-1931), the
baseline restore so a cancelled drag is exact, the apply (:2735-2760) and **one
`Patch_Rebuild` per patch however many of its points moved** (:2765-2781). The move
gizmo, the grid snap and the undo bracket ride on that for free.

**WHAT WAS ACTUALLY MISSING WAS TWO THINGS**, and `kiwi_patchverts.cpp` is exactly
those two:

* **THE PICK MASK.** Entering the mode sets `SEL_MASK_VERTEX` — a pick-time mask,
  as section 1 requires — and clears the selection so the first click selects a
  point rather than adding one to a whole-object selection.
* **SEEING THE POINTS.** The ported `Patch_DrawControlPoints` (brush.cpp:6230) is
  gated on `d_select_mode == sel_curvepoint` AND reached only from the SELECTED-patch
  wireframe pass — so the instant the user clicks a control point the modern
  selection replaces the object item with a vertex item, the patch leaves
  `selected_brushes`, and the points **vanish exactly when they are needed most**.
  The mode draws the control lattice and its markers from its own latched patch
  list, which has no such dependency. Same decision shakeout D made for brush
  vertices, same reason.

**ONE UNDO RECORD PER DRAG**, and it is the Move command's existing bracket —
`KiwiCmd_UndoBegin` + a cover per touched brush before the first mutation,
`KiwiCmd_UndoCommit` at the end. A patch def is cloned through
`Brush_FullClone_sub475E80`, whose FIRST branch is the patch branch
(brush.cpp:7386-7398, `Patch_Duplicate`, the whole 20 556-byte struct including the
16x16 grid), so the control grid is inside the record. Entering and leaving the MODE
is not journalled: it changes no geometry.

Leaving hands the patches back as whole objects rather than leaving an empty
selection, and restores the pick mask the user was working in. A mode chip leads
both idle prompt strips while it is live, naming the way out — a mode with no live
command has nowhere else to announce itself, and a mode you cannot see you are in
is how you get "V opens something and then nothing works".

---

## Decision log — round AI

- **D-AI1 — a 2D line emit inside a stretch-pic window must restore
  `MATERIAL_COLOR`, and the restore lives in the HELPER.** `R_AddCmd_Line2D` is
  state-destructive by design and by documentation; the fix is not to change that
  (3D line batches depend on the parked colour and restoring per batch would double
  the command count the dedup exists to avoid) but to make the one helper that draws
  lines in a thumbnail window self-restoring. Both its callers had the identical
  hazard and the second one's colour is BLACK.
- **D-AI2 — every paint that draws thumbnails seeds `MATERIAL_COLOR`.**
  `CLayermatWnd_OnPaint` was the only one that did not, and backend colour state
  survives across windows. Cheap, and it turns "unlikely to be hostile" into an
  invariant.
- **D-AI3 — a one-axis gesture is gated on the VIEW, not on the sample, for the
  behaviour the user sees.** `vpn` gives one stable boolean per frame; the cursor ray
  would flicker across the screen and make the HUD unreadable. The sample gate exists
  too, but only to stop a single bad pixel producing a jump.
- **D-AI4 — a blocked one-axis gesture HOLDS, it does not RESET.** A value set from
  a workable angle must survive an orbit through the degenerate one. Zero at stage
  entry is what the report asked for; zero on every pass through the top would be a
  new bug.
- **D-AI5 — the rebase correction moved INTO the latch.** It was in `Rebase()` in two
  places and the view gate needs the same preservation; two copies of a `-= m_dist`
  is a double-subtract waiting to happen. `Rebase()` is now a bare latch call.
- **D-AI6 — 0.97 / 14 degrees, and the two gates express the SAME angle.**
  `KCAM_RAYAXIS_MIN_DEN` is `sin^2` of `KCAM_AXIS_PORTRAY_DOT` so they cannot drift.
  The number is chosen from the amplification (17x, ~9 units per pixel at d = 500),
  not from taste.
- **D-AI7 — the Move command's refusal SPEAKS.** Push/pull a face while looking at
  it head-on is the most-used gesture in the editor; turning a wild drag into a
  silent dead one would be a worse bug than the one being fixed. One console line
  per refusal run, and only when the axis really is end-on (the cursor being off the
  image fails the same test and must stay silent).
- **D-AI8 — the command options panel is GENERIC.** The scalars and the keycaps were
  already declarative; only a bool/enum was missing. Three virtuals serve every
  command, and a loft-only panel would have been the same code with none of the
  reuse. It is Plasticity's own shape — a dialog over the command's params with a
  generic write-back.
- **D-AI9 — the panel CALLS the keyboard's handlers, never a parallel setter.** The
  mode button is literally `KeyDown('D')` and the density stepper is
  `NumericFieldChanged(0)`. Anything else would drift from the key on the first
  change to either.
- **D-AI10 — the panel is a real ImGui window, not an ImDrawList overlay.**
  kiwi_hints.h forbids ImGui items inside the camera image (the `End()`
  cursor-extent assert class), and the panel is interactive by definition. It is
  drawn at top-level window scope beside the outliner, anchored to the camera image
  rect, and never takes keyboard focus.
- **D-AI11 — a disabled option row is DRAWN, not hidden.** Plasticity's rule
  (`class="disabled"`), and a control that vanishes teaches nothing about why it is
  unavailable.
- **D-AI12 — hover strength is measured against BOTH of a selection's channels.**
  The tint AND the wireframe. The outliner hover had only the tint, which is why no
  alpha could have made it read as strong as a selection; it gains an outline.
- **D-AI13 — the marquee preview gets WIDTH, not a fill.** One channel has to carry
  the weight of two, and a per-brush fill over a marquee's worth of geometry blows
  the 64-face-per-frame fill budget. Width is per batch, so it costs nothing.
- **D-AI14 — the face marquee's occlusion query is OBJECT-granular.** A face-granular
  query returns invalid on a patch or a model hit and would let those through as
  non-occluders. Comparing the brush is sound because the facing gate has already
  removed this brush's own back faces and brushes are convex.
- **D-AI15 — the occlusion gate FAILS OPEN.** It rejects only when it positively
  identifies something else in front. `Test_Ray` runs under a contents mask, and a
  filtered-but-visible brush must stay selectable.
- **D-AI16 — the marquee preview may over-report, never under-report.** The ray
  budget makes the preview a superset of the release. Showing one extra face is a
  much smaller lie than taking one that was never shown.
- **D-AI17 — the occlusion ray is spent LAST.** After the shape test has already
  decided the rect wants the face, so it is never paid for a face that was not going
  to be taken.
- **D-AI18 — patch vertex mode is ROUTED, not resurrected.** Three independent
  things kill the legacy drag and un-killing one leaves two. The modern layer already
  picks, marquees, moves, snaps, gizmos and journals patch control points; the mode
  only had to set the pick mask and draw the points.
- **D-AI19 — the mode draws its own control lattice.** The ported draw is gated on
  the patch still being on `selected_brushes`, which stops being true the moment a
  control point is clicked. Drawing from the mode's own latched list is the same
  decision shakeout D made for brush vertices.
- **D-AI20 — V is a REFINEMENT of 33005, not a new binding.** No chord moves, both
  profiles behave the same, and with no patch in the selection the classic
  brush-vertex toggle is byte for byte what it was.
- **D-AI21 — the draw-time exit is QUIET.** `KiwiPatchVerts_DrawWorld` runs inside
  `Cam_Draw`, which is walking the brush sentinel lists; the full exit ends in
  `Sel_SyncToLegacy` then `Select_Deselect`, which RELINKS brushes between them.
  Restoring the mask and dropping the latch is all the draw-time path may do.

---

## 63. Round AJ — six reports

One mode that outlived its own subject (1), one tool that ran before it was asked
to (6), one surface with a hole in it and a texture at the wrong density (2), one
camera number that was right far away and wrong up close (3), one checkbox and one
deliberate retreat (5), and one pure repaint (4).

Items 1, 6 and 2 are one cluster: all three are about a gesture saying what it is
doing at the moment it does it.

---

### 63.1 ITEM 1 — the lattice that would not go away, and the second point

**USER REPORT, verbatim:** *"The curve vertex mode needs to hide the points when
the curve is no longer selected.  It needs to function more natively, it's also
impossible to edit more than 1."*

**(a) THE LATCH WAS LOAD-BEARING AND THAT IS WHY IT WAS IMMORTAL.** Round AI
latched the patch list at entry for a reason it wrote down: entering the mode
CLEARS the selection so the first click picks a point, and clicking a point
replaces the object item with a vertex item — so *"is the patch still selected"* is
false within one click and cannot be the test. The list therefore had exactly one
way to end (the draw hook noticing that no latched patch was live any more), and
everything else — deselect, click empty, select something else — left the lattice
drawn over geometry the user had moved on from.

The fix is not a different test, it is a set of **named exit triggers**, checked
once per frame from `KiwiVP_CameraTick`:

| trigger | mask restored? | patches reselected? |
|---|---|---|
| V again / Esc / a delete / a map load | yes | yes |
| the mode mask moved off `SEL_MASK_VERTEX` | **no** — the user just chose one | no |
| the selection names something that is not a latched patch | yes | **no** — something else owns it |
| no latched patch is live | yes | no |

**AN EMPTY SELECTION IS NOT A TRIGGER**, and that is the whole of *"click empty =
drop the point selection, stay in the mode"*: the patch is still the thing being
edited; only the points have been let go of.

**IT IS TICKED, NOT DRAWN.** D-AI21 established that the draw hook may not run the
full exit — it is inside `Cam_Draw`, which is walking the brush sentinel lists, and
the exit's `Sel_SyncToLegacy` then `Select_Deselect` relinks brushes between them.
`KiwiVP_CameraTick` (imgui_shell.cpp:636) is once per frame and outside every list
walk, which is the only place a real exit is safe. The selection walk is gated on
`Sel_Generation()`, so an idle frame costs one integer compare, and the tick sits
ABOVE the modern-input gate so flipping the master toggle off cannot strand the
editor inside a mode whose only exit is the modern layer.

**AND THE LATCH GREW AN ADOPTION RULE**, which retires round AI's "scoped at entry"
limit rather than documenting it again. The pick mask is GLOBAL, so a control point
on a patch the mode did not latch was always clickable — the lattice just did not
draw for it, which reads as *"that patch's points do not work"*. A control point of
any live patch is by definition a statement that the user is still doing patch
vertex work, so the mode widens instead of standing down. Anything else in the
selection — an object, a face, a brush vertex — is a statement that they are not.

**(b) THE HOVER WAS THE MISSING AFFORDANCE.** `kiwi_hover.cpp` does pick a control
point (its `SEL_VERTEX` arm draws a 5 px marker) but the mode draws its own markers
at 4 px in its own palette, so the hover accent was a one-pixel difference in a
colour the user had no reason to read as "hover". The mode's marker pass gains a
THIRD colour run — plain, selected, then the single HOVERED one — drawn LAST so it
wins the overlapping pixels and LARGER (6.5 px) so it is a second channel and not
just a tint. It reads the SHARED hover pick, so the accent and the click can never
disagree about which point is being aimed at.

**(c) "IMPOSSIBLE TO EDIT MORE THAN 1" IS NOT A BROKEN PATH — IT IS AN INVISIBLE
ONE, and that took proving.** Every layer was read end to end first:

* `kiwi_boxselect.cpp`'s `ClickSelect` does `Sel_Add` under Shift and `Sel_Toggle`
  under Ctrl, for a `SEL_VERTEX` hit exactly as for any other kind;
* its rect arm has collected patch control points since it was written — the
  `SEL_VERTEX` branch of `CollectFromBrush` walks `pm->ctrl[col][row]` and emits
  `Sel_MakePatchPoint`, ignoring the crossing/containment distinction because a
  point is a point;
* `kiwi_transform.cpp`'s `BeginVerts` builds `m_verts` from EVERY `SEL_VERTEX` item
  in the selection, `ApplyVerts` adds the same `m_total` to every one of them from
  its own baseline, and the rebuild is **one `Patch_Rebuild` per patch however many
  of its points moved** — one undo record, by construction.

So the machinery was whole. **What was missing was any way to SEE that it was.**
Two things, and the second is a real defect:

1. **NOTHING SAID SO.** The mode's console line and status named a single click and
   the gizmo. Both now name Shift-add and the dragged box explicitly, and the idle
   chip strip carries `Shift+LMB — Add point` and `Drag box — Take several` for as
   long as the mode is live.
2. **THE MARQUEE PREVIEW DREW NOTHING AT VERTEX GRANULARITY.** `KiwiBox_DrawPreview`
   skipped `SEL_EDGE` and `SEL_VERTEX` with a justification — *"the ported vertex
   pass is already drawing"* those handles — **and that justification is false in
   this mode**: the ported handle builders walk `selected_brushes`, and shakeout D
   removed the promotion that put anything there (kiwi_patchverts.h KILL 3). So a
   user dragging a box over a control lattice watched an empty rect sweep across the
   points and highlight nothing, from which the only available conclusion is that
   box select does not work here and one point is the ceiling. The preview now emits
   a 5 px marker per `SEL_VERTEX` item. EDGES stay absent for the reason that was
   always true of them: a marquee at edge granularity names dozens of brush edges
   the ported wireframe genuinely is drawing.

---

### 63.2 ITEM 6 — the bevel that had already cut before you asked

**USER DIRECTIVE, verbatim:** *"in edge bevel mode, dont do operations with the
mouse automatically.  Add a lollipop that controls the amount of deformation that
must be grabbed."*

**THE LOLLIPOP ALREADY EXISTED** — round Y, item 7 gave the merged bevel/fillet
command a `LollipopHandle` anchored on the chamfer face's live midpoint. What did
not exist is a GATE. `Begin()` latched the cursor and returned, the command was HOT
by definition (`KiwiCmd_Start` sets `s_hot = true` before calling it), and the very
first `MouseMove` ran `Recompute`, mapped the cursor through `RayAxis` and appended
a chamfer face at whatever depth that pixel happened to express. The ball was drawn
on geometry that had already deformed.

**TWO HALVES, AND BOTH ARE NECESSARY.**

* **THE COMMAND PARKS AT ENTRY.** `KiwiCmd_Pause()` from inside `Begin()` is legal
  and deliberate: `g_activeCommand` is assigned BEFORE `Begin` is called
  (kiwi_command.cpp:1260, whose own comment says *"set BEFORE Begin: Begin may query
  Active()"*), so the pause takes. A PAUSED command is not fed `MouseMove` at all.
* **AND THE COMMAND KEEPS ITS OWN GRAB GATE.** Parking alone is not enough, because
  shakeout E's resume rule is deliberately the BROADEST one — any LMB press inside
  the camera image resumes — so one stray click would hand the gesture back to the
  cursor. With `m_grabbed`, a resumed-but-ungrabbed command still holds its scalar:
  the only things that can move it are the ball and a typed number. That is exactly
  the shape `kiwi_transform.cpp`'s Move has used since round L, raised by the same
  shared arm.

**THE GATE COVERS THE SNAP ARM TOO.** The geometry snap is cursor-driven — it
answers *"what is under the pointer"* — so "chamfer out to that wall" is something
you do WHILE dragging, not something a parked tool does on its own. Numeric entry is
deliberately OUTSIDE the gate: it never went through the cursor.

**`KiwiCmd_HandleGrab` DOES THE REST FOR FREE**, and none of it is new code: it
resumes, re-latches at the press pixel through `Rebase` then `ReLatchFor` (so the
chamfer does not jump by however far the cursor wandered since B), raises
`HandleGrab`, re-latches again and feeds frame one with a delta of exactly zero.
`KiwiLollipop_Release` then PAUSES the gesture, so letting go parks it. RMB / Enter
confirm and Esc cancel are untouched, and so are D, Tab, DEL and the options.

**A PARKED TOOL IS NOT AN INVALID ONE.** `Apply()`'s depth-too-small branch used to
raise `m_invalid` unconditionally, which under the new entry state would paint the
HUD red and shout *"depth too small"* at a tool that has not been asked to do
anything yet. It now asks `Parked()` first, and the HUD gets its own rest line —
*"parked at 0 · grab the ball and drag (or type a depth)"* — with a `Ball` chip
leading the prompt strip in both modes.

---

### 63.3 ITEM 2 — the fillet's gap, derived rather than eyeballed

**USER DIRECTIVE, verbatim:** *"When using the Bevel mode on a solid's edge, in
fillet mode (with curve), the curve should automatically use the thicken operation
(built-in) to fill up the gap between the underlying brush chamfer.  Also the
Texture should be fixed so it looks natural with the parent solid edge area."*

**(a) WHERE THE GAP IS, AND WHERE IT IS NOT.** The arc's centre is
`A = mid - n*(r/k)` and its tangent points are `T1 = A + r*n1` and `T2 = A + r*n2`.
The chamfer plane is `n.(x - mid) = -d` with `d = r(1-k^2)/k`. Substituting:

    n.(T1 - mid) = -r/k + r*(n.n1) = -r/k + r*k = -r(1-k^2)/k = -d

so **both tangent points lie exactly ON the chamfer plane** — and, being the arc's
endpoints, they are also on the two original faces, i.e. on the two long edges of
the chamfer face's own winding. The arc therefore meets the solid exactly along
both of its long rails and bulges outward between them (its middle sits at
`r(1-k)/k`, nearer the edge than `d`). The enclosed volume is a lens whose top is
the arc, whose bottom is the chamfer FACE — a real solid surface, already there,
already wearing the same material — and **which is open only at its two ends.**

**SO THE THICKEN IS TWO PATCHES AND NOT FIVE.** Stock `Patch_Thicken`
(pmesh.cpp:7411) makes an offset copy plus up to four seam strips. Here the offset
copy IS the brush's own chamfer face, and two of the four seams ARE those tangent
rails; the only seams that are not already solid are seam C and seam D, the
`width x 3` end strips. `LandCaps` is that pair, built the way pmesh.cpp:7529-7581
builds them — source profile on row 0, offset profile on row 2, MIDPOINT on row 1 —
with the offset being the projection of the arc onto the chamfer plane along `n`.

**THE CAP IS EXACTLY PLANAR, and that makes its winding test exact.** Every arc
control point is translated along the edge direction `u` to reach the end, and the
offset is a projection along `n` — and `n` is perpendicular to `u` by construction
(`KiwiBevel_MakeFrame` re-orthogonalises `u` against `n`) — so all of a cap's
control points lie in one plane perpendicular to `u`. The cross product of any two
independent in-plane directions is therefore parallel to +/-u, and comparing it
against the outward direction decides the row order with no tolerance at all. Stock
thicken has to call `patchInvert2` on exactly one of its two end seams for the same
reason; that function is file-static in pmesh.cpp, and choosing the row ORDER up
front is the same act without reaching for it.

**ONE TRAP INSIDE THAT TEST, and it is the tangency again:** `dRow` must be sampled
at the MIDDLE column. At columns 0 and w-1 the arc point and its projection are the
SAME POINT (that is what tangency means), so the difference there is zero and the
cross product would be too.

**(b) THE TEXTURE WAS A SCALE, NOT A PROJECTION.** `Patch_Naturalize2`
(pmesh.cpp:1219) lays S from the cumulative WIDTH DISTANCE in world units divided by
`texWidth * a3` — so `texWidth * a3` **is** the number of world units one full
texture repeat covers. A brush FACE expresses that same quantity directly as its
texdef `size[0]`; brush.cpp:3327 and pmesh.cpp:855 both seed it as `texWidth * 0.25`,
i.e. exactly this product at the default sample size. `Patch_KiwiFinishNew`
hardcoded `random_texture_stuff[0].sampleSize` instead, so a patch grown out of a
rescaled wall — which is every wall a mapper has actually worked on — came out at
the EDITOR DEFAULT density while the face it grows from was at some other one, and
the join read as a texture change. That is *"doesn't look natural with the parent
solid edge area"*.

`Patch_KiwiFinishNewLike( p, srcTex )` takes `a3 = srcTex->size[0] / texWidth`.
Both numbers are read from the SAME material (the patch has already inherited the
face's, round T), so the widths cancel and the two surfaces agree by construction
rather than by luck. `Patch_KiwiFinishNew` is now a one-line forwarder passing a
null texdef, which takes the identical default path — **no ported caller changes.**
The caps get the same material and the same scale as the arc they cap: it is the
same surface turning a corner.

---

### 63.4 ITEM 3 — squirrely is a DIRECTION problem, not only a speed one

**USER REPORT, verbatim:** *"When zooming in, the camera gets squirrely.  The
sensitivity is too high.  Need to try and remedy that slightly."*

**PLASTICITY, MEASURED** (its own fork of three.js `OrbitControls`, and every number
here is read off it):

* the wheel is MULTIPLICATIVE at `Math.pow(0.95, zoomSpeed)` with `zoomSpeed = 1`
  (OrbitControls.ts:648, default-settings.js:9-13) — **five** percent a notch,
  half of this editor's ten;
* `Math.sign(deltaY)` throws the wheel delta's MAGNITUDE away
  (OrbitControls.ts:436), so a fast flick is not a bigger step;
* there is **no soft minimum and no easing**: `spherical.radius` is clamped hard
  against `minDistance = 0.1` (OrbitControls.ts:42, :222-225). The only damping is
  the geometric series itself — the world step is `radius * 0.05`, so it shrinks as
  the camera closes in;
* and, the one that matters most, **there is no zoom-to-cursor at all.**
  `onMouseWheel` (OrbitControls.ts:423-443) never reads `clientX/Y`, and `dolly()`
  (:650-657) only scales the radius about `this.target`. The eye moves along the
  view axis, always.

**SO "SQUIRRELY" HAS TWO SOURCES HERE AND ONLY ONE IS THE STEP SIZE.**
`KiwiCam_Dolly` moves the eye along the CURSOR RAY — the "zoom to mouse" this editor
deliberately has and Plasticity does not. Decompose it: the along-view part is the
zoom, the perpendicular part is a **sideways slide** of `move * sin(angle between
the cursor ray and vpn)`. Far out, `move` is a small fraction of what is framed and
the slide is invisible. Close in, `move` is a large fraction of the framed extent,
so every notch also shoves the view sideways by a visible slice of the screen. **No
smaller step alone fixes that.**

**THE REMEDY IS TWO EASES ON ONE PARAMETER**, `t = clamp( reference / 48, 0, 1 )`:

1. the STEP eases `0.90 -> 0.96` as `t -> 0`. At the bottom that is a hair gentler
   than Plasticity's flat 0.95, which is the right end to land on for detail work;
   at `t == 1` it is byte-for-byte the old number.
2. the AIM eases from the full cursor ray toward the VIEW AXIS as `t -> 0` — never
   all the way (a third of the cursor aim survives), so zoom-to-mouse still works up
   close, it just stops dominating.

48 in is half the default standoff (`KCAM_DEF_DIST` 96), i.e. "closer than arm's
length to the thing you are working on"; beyond it **nothing changes at all**, which
is the *"slightly"* the report asked for.

**ORBIT AND PAN WERE CHECKED AND LEFT ALONE**, because they already match
Plasticity: pan is per-pixel world-proportional through `KiwiCam_WorldPerPixel` with
a floor at `KCAM_MIN_DIST` (the same shape as its
`2 * targetDistance * tan(fov/2) / clientHeight`, OrbitControls.ts:263-282), and
orbit is a flat `KCAM_DEG_PER_PX` with NO distance term, exactly as
`2*PI*dx/clientHeight` has none (OrbitControls.ts:672-677).

---

### 63.5 ITEM 5 — one checkbox, and one deliberate retreat

**USER DIRECTIVE, verbatim:** *"Add a checkbox by the grid density setting to
enable/disable snapping to grid.  Disable drawing the grid in 'P' Camera mode
because the bugs are even worse."*

**(a) ONE BOOLEAN, ENFORCED AT TWO PLACES AND NOT SIX.** The lattice can reach a
number through exactly two functions — `KiwiGrid_Snap` (the full quantiser:
SNAP_GRID, the Move quantise, the split, the clone) and `KiwiSnap_LightGridAxis`
(the round-AG magnet BAND: both extrudes and push/pull). Both already had a
documented "leave the value alone" answer for a degenerate spacing, so switching off
reuses a path that was always there. Gating the six CALL SITES instead is how a
toggle acquires an exception.

**IT IS NOT WHAT CTRL DOES.** Ctrl is the per-gesture, hold-to-suppress override
(section 6, deliberately inverted vs other apps) and still suppresses everything
including geometry snaps; this is a persistent preference about the GRID alone, and
geometry / face / construction snapping is untouched by it. Persisted under
`KiwiUX/GridSnap`, default ON.

**THE CONTROL IS LITERALLY "BY" THE DENSITY SETTING**: it shares the grid pill's row
and baseline and sits against its left edge, so the two read as one control ("this
much grid, and do / do not stick to it"). Pure `ImDrawList` like everything else in
that cluster (kiwi_viewcube.h's no-ImGui-item contract), so it cannot take the camera
image's hover away from a marquee that starts elsewhere. The glyph is a 2x2 LATTICE,
struck through when off — a tick would say "on" without saying what is on, and there
is no room for a label and no ImGui item to hang a tooltip from. The state is
announced on the console, mirrored in the settings block, and while snapping is OFF
the in-gesture chip strip carries `Grid — Snap OFF` (shown only in the non-default
state, so the strip does not grow for everyone).

**(b) THE PERSPECTIVE GRID IS SUPPRESSED, AND THIS IS A WORKAROUND THE USER CHOSE.**
Written down as one here and in RADIANT_KNOWN_ISSUES. Perspective is where every one
of the lattice's open problems is at its worst — the horizon-plane aliasing the
round-S edge-on ramp only softens, the band seams the round-AG far ring only pushes
further out, the near-plane crawl a screen-space window cannot fix — and none of them
exist in ortho, where the lattice is a fixed-pitch pattern at a fixed scale.
Suppressing the draw is not a fix for any of them; it removes the surface they show
up on until they ARE fixed. **The axes stay** (three lines, they do not alias, and
they are the one thing that still says which way is which — the same argument the
edge-on ramp already makes for keeping them outside its own fade), and **snapping is
untouched**, so the lattice you cannot see is still the lattice you land on.

---

### 63.6 ITEM 4 — the repaint

**USER DIRECTIVE, verbatim:** *"I want the gizmos to look less ugly.  Make it look
like plasticity's gizmos."*

**PLASTICITY'S GEOMETRY, MEASURED.** Everything is built at unit scale and multiplied
by a per-sub-gizmo `relativeScale`, so it is quoted below as a fraction of the SHAFT
LENGTH and then multiplied by this file's `KGZ_AXIS_PIX` (70 px):

| element | Plasticity | here |
|---|---|---|
| shaft | `Line2`, unit length, linewidth **2 px** / 3 hovered (MiniGizmos.ts:348-349, GizmoMaterials.ts:33,37) | unchanged 70 px, width 2 |
| head | cone, base radius **0.1**, height **0.2** (`CylinderGeometry(0, 0.1, 0.2, 12)`, MiniGizmos.ts:346-347) | 14 px long, 7 px half-width, **closed at the base** so it reads as a cone and not a V |
| plane handle | filled **square 0.2 x 0.2 centred at (0.5, 0.5)** (MiniGizmos.ts:382-398) | a 14 px square outline centred 35 px out |
| origin | **white billboarded circle**, radius 1 at relativeScale 0.25 vs the axes' 0.8 (MoveGizmo.ts:40-41, MiniGizmos.ts:62-63, :106-116) | a 10 px screen-space ring |
| rotate | three axis circles at 0.7 + a **white one at 0.8**, the white billboarded and the three not (RotateGizmo.ts:40-41, :129) | 55 px rings + a 63 px white view ring |
| back halves | hidden by a 100000-square camera-facing **depth-only** plane at `renderOrder = -1` (RotateGizmo.ts:13, :132-159) | one dot product per segment |

**THE PALETTE IS ITS 600 RAMP** — X `#CF1124`, Y `#199473`, Z `#2563EB`
(GizmoMaterials.ts:111-123, default-theme.js) — with the **400 ramp as the HOVER**
(`#EF4E4E` / `#3EBD93` / `#60A5FA`, MiniGizmos.ts:220-228) and `#FAFAFA` for white.
They are noticeably more muted than the near-primary triple that was here, which is
the "less ugly" half of the directive: at 2 px a fully saturated primary vibrates
against a mid-grey viewport.

**THREE STATES, NOT TWO, and that is a deliberate departure.** Plasticity has rest
and hover; this file keeps its yellow for HELD. Unlike Plasticity — where the pointer
is captured and the rest of the scene stops responding — a grab here coexists with
live camera navigation (section 4), so "I am pointing at it" and "I am holding it"
have to be told apart at a glance.

**THE ORIGIN CIRCLE IS A RESTYLE, NOT A NEW HANDLE.** `KGZ_CENTER` has always been
the free move on the latched view-normal plane, i.e. the view-plane translate;
Plasticity draws exactly that handle as a small billboarded white circle. Nothing was
wired.

**TWO HIT ZONES MOVED WITH THEIR VISUALS, AND ONE GREW.**

* **THE CENTRE GREW, 8 px to 12 px.** The ring is drawn at 10 px, so it is now inside
  its own target instead of 6 px outside it. A strict superset, and it can steal from
  nothing: the axis shafts refuse hits below `KGZ_AXIS_MIN_T` (19.6 px) and the plane
  squares sit at 35 px. Plasticity's ring is proportionally larger (0.31 of the
  shaft, 22 px here) and can afford it because its axis PICKER is a small sphere at
  the arrow TIP only (MoveGizmo.ts:139) rather than the whole shaft.
* **THE PLANE ZONE RELOCATED**, from the two arms of an L cornered 22 px out to the
  perimeter plus both diagonals of the square centred 35 px out. A hit zone has to be
  where the handle is drawn — leaving it at the old corner would have made the new
  square decorative and the grab invisible. The tolerance and the resolution order
  (CENTRE, then PLANE, then AXIS) are unchanged, and the covered area is larger.

**THE BACK-HALF CULL IS ONE DOT PRODUCT.** This layer emits LINES through a batcher,
not meshes through a depth buffer, so Plasticity's depth-only occluder plane becomes
"is this segment's midpoint on the far side of the plane through the pivot facing the
eye". Exact for a ring centred on the pivot, which all four are. **The hit test is
deliberately NOT culled** — hiding a handle's back half is a legibility decision, and
quietly shrinking the target with it would be a second, unasked change.

**THE WHITE OUTER RING IS DRAWN AND IS NOT GRABBABLE, and that is an honest gap.**
In Plasticity it is the VIEW-AXIS rotation. This editor has no such rotation to wire
it to: the ported core is `Select_RotateAxis( axis, deg, ... )` (select.cpp:2337),
which takes an axis INDEX 0/1/2, and R's whole state is one `m_axis` int. An
arbitrary-axis rotate is a new core, not a visual pass — so the ring is drawn as the
silhouette it also is (with the back halves culled, it is what turns three arcs into
a sphere) and the three axis rings keep every grab they had.

**HOVER WIDTH COSTS A SECOND BATCH.** `KiwiLines_Begin` takes the line width per
BATCH, not per segment, so Plasticity's 2 to 3 px hover is done by re-emitting
exactly the ONE element under the cursor in a small second batch. At most a handful
of segments, and only while something is hovered.

---

## Decision log — round AJ

- **D-AJ1 — a mode gets NAMED EXIT TRIGGERS, never a liveness heuristic.** Patch
  vertex mode clears the selection at entry and replaces the object item with a
  vertex item on the first click, so "is the patch still selected" is false almost
  immediately and cannot be the test. Four explicit triggers, each with its own
  answer to "restore the mask?" and "reselect the patches?".
- **D-AJ2 — an EMPTY selection is not an exit.** Clicking empty space drops the
  POINT selection and stays in the mode; the patch is still the thing being edited.
  That is the only reading of "click empty = deselect" that does not also mean
  "click empty = leave".
- **D-AJ3 — the lifecycle is TICKED, not DRAWN.** D-AI21's rule stands: the draw
  hook runs inside a sentinel-list walk and may not relink brushes.
  `KiwiVP_CameraTick` is the one per-frame place outside every walk, and it sits
  above the modern-input gate so the toggle cannot strand the mode.
- **D-AJ4 — the mode ADOPTS an unlatched patch's control point instead of exiting
  on it.** The pick mask is global, so those points were always clickable; the
  lattice just did not draw for them. This retires round AI's "scoped at entry"
  limit rather than restating it.
- **D-AJ5 — a marquee preview that draws nothing IS the bug report.** The
  `SEL_VERTEX` preview was skipped on the grounds that the ported vertex pass draws
  those handles — untrue since shakeout D removed the promotion that fills its
  lists. "Impossible to edit more than 1" was a feedback failure, and the accumulate
  paths were proved whole before a single one was touched.
- **D-AJ6 — a mode's grammar goes on the CHIP STRIP, not only in a console line.**
  A console line is read once, at entry, and the strip is what is on screen while
  the user is trying the gesture.
- **D-AJ7 — a tool that deforms geometry PARKS at entry and needs a GRAB.**
  `KiwiCmd_Pause()` from inside `Begin()` plus the command's own `m_grabbed` gate.
  The pause alone is not enough (shakeout E resumes on ANY LMB press inside the
  image); the gate alone would leave the preview chasing the cursor.
- **D-AJ8 — the grab gate covers the SNAP arm, and not numeric entry.** The
  geometry snap asks "what is under the pointer" and is therefore cursor-driven;
  a typed number never went through the cursor.
- **D-AJ9 — a PARKED tool is not an INVALID one.** Zero depth before the first grab
  is the rest state, not a rejection, and painting the HUD red on entry would be a
  false alarm every single time.
- **D-AJ10 — the fillet's thicken is TWO patches because tangency already sealed
  the other three sides.** The tangent points are proved to lie on the chamfer
  plane, so the arc meets the solid along both long rails and only the ENDS are
  open. Reproducing stock thicken's five-piece shape would add three surfaces that
  are already there.
- **D-AJ11 — the cap's row order is chosen, not inverted.** The cap is exactly
  planar and perpendicular to the edge, so the winding test is exact; `patchInvert2`
  is file-static in pmesh.cpp and choosing the order up front is the same act.
- **D-AJ12 — `dRow` is sampled at the MIDDLE column.** At the ends the arc and its
  projection are the same point — that is what tangency means — and the cross
  product there would be zero.
- **D-AJ13 — the patch's naturalize SCALE comes from the parent face's texdef.**
  `texWidth * a3` and the face's `size[0]` are the same physical quantity (world
  units per repeat), read from the same material, so the two surfaces match by
  construction. `Patch_KiwiFinishNew` keeps its exact old behaviour as a forwarder.
- **D-AJ14 — "squirrely" is the LATERAL component of zoom-to-cursor, not just the
  step.** Plasticity has no zoom-to-cursor at all; the remedy eases the AIM toward
  the view axis as well as easing the step, and both ease on the same parameter so
  they cannot drift.
- **D-AJ15 — the dolly ease is bounded by distance and does NOTHING beyond it.**
  Half the default standoff. The report said "slightly", and a change that altered
  the feel at every distance would not be it.
- **D-AJ16 — orbit and pan were checked and deliberately not changed.** They already
  match Plasticity's formulas: pan distance-proportional with a floor, orbit flatly
  angular.
- **D-AJ17 — the grid-snap switch is enforced at the two FUNCTIONS, not the six call
  sites.** `KiwiGrid_Snap` and `KiwiSnap_LightGridAxis` are the only two ways the
  lattice reaches a number, and both already had a "leave it alone" answer.
- **D-AJ18 — it is not Ctrl.** Ctrl is per-gesture and suppresses geometry snapping
  too; this is persistent and grid-only.
- **D-AJ19 — the perspective grid is SUPPRESSED as a workaround the user chose, and
  it is logged as one.** It removes the surface the open aliasing shows up on; it
  fixes none of it. Axes stay, snapping is untouched.
- **D-AJ20 — the gizmo diverges from section 18's shared axis palette, locally.**
  The world axes and the constraint accent keep their brighter triple (1 px lines
  against the whole scene); the gizmo takes Plasticity's muted 600 ramp. The HUE is
  the same in all three, which is the part of section 18 that carries meaning.
- **D-AJ21 — three gizmo states, not Plasticity's two.** Rest, hover (its 400 ramp),
  HELD (this editor's yellow). A grab here coexists with live camera navigation, so
  pointing and holding must be distinguishable.
- **D-AJ22 — a hit zone follows its visual.** The plane handle's zone relocated with
  its square and the centre's grew to contain its ring. A zone left behind its
  handle is a grab the user cannot see and a picture they cannot click.
- **D-AJ23 — the back-half cull is per SEGMENT, and the hit test is not culled.**
  A line batcher's equivalent of a depth-only occluder is one dot product; shrinking
  the grab target along with the drawing would be a second, unasked change.
- **D-AJ24 — the white view ring is drawn and not wired.** `Select_RotateAxis` takes
  an axis INDEX, so a view-axis rotate is a new core rather than a repaint. Drawn as
  the sphere silhouette it also is, and logged.

---

## 64. Round AK — four reports, two of them recurring sores

Two items the user has now reported across several rounds and asked to have
settled ("after like 10 attempts"), one selectability gap a previous round only
diagnosed and never closed, and one repaint.

Items 1 and 2 have the same *shape*, and it is worth naming before either one:
**a subsystem was reading a quantity that was a good proxy in the case it was
written for and no proxy at all in the case the user is actually in.** The region
fill locked its shading normal to its own plane; the grid locked its LOD to camera
altitude. Both are defensible under the assumption each was written under, and
neither assumption survives contact with the editor as it now ships.

---

### 64.1 ITEM 1 — the light-blue fill: what round AA's flip could never reach

**USER REPORT, verbatim:** *"construction faces that can be extruded from still
lack their light blue background on the face. It worked a few days ago sometimes,
but now never shows - not even from the backside."*

**THE CRITICAL DATUM IS IN THE REPORT: THEY STILL EXTRUDE.** Both sides read the
same `KiwiRegion_All()` (round AG made that the invariant), so the derivation chain
— round AG's weld bound, its 128-vertex loop cap and its PASS 3b — is *working*.
This is a DRAW defect, and the whole draw was audited end to end before anything
was changed.

**WHAT WAS RULED OUT, EACH WITH A REASON RATHER THAN A SHRUG:**

| suspect | verdict |
|---|---|
| `KiwiRegion_Triangulate` failing | **Impossible on a region that extrudes.** A CONVEX region does bypass it (kiwi_extrude.cpp:934-939 takes the whole-polygon branch) but cannot *fail* it either: `AcceptLoop` already ran the identical `KiwiRegion_SelfIntersects` on the identical points, and a convex CCW loop always has an ear on the first scan. A CONCAVE region reaches it through `KiwiRegion_ConvexPieces` (kiwi_region.cpp:1607) and has therefore already proven it succeeds. |
| `SegCross` over-reporting on touching / collinear / duplicate points | All four determinant tests are strict `> 0.0f`, so a zero determinant lands on the same side of both comparisons and returns **false**. It can only under-report. The wrap pair is correctly skipped (`i == j2`). |
| winding | `AcceptLoop` order is Dedup, DropCollinear, the n-gates, **then** `RewindCCW`, then area, then self-intersect. RewindCCW is the last mutation. `SignedArea` / `Cross2` / Triangulate's reflex test all agree on sign. |
| re-entrancy through `ResolveCentroid` -> `KiwiRegion_All` -> `EnsureBuilt` | `EnsureBuilt` commits `s_builtFor` and `s_dirty` **before** returning, so the nested call short-circuits. And the `selFlags` writes are bound-checked — worst case a wrong colour, never a skipped draw. |
| `KiwiCon_Generation()` moving per frame | `++s_generation` happens only inside `Touch()`; all 15 call sites are store mutators or load. No draw path touches it. |
| a pass leaving MATERIAL_COLOR dirty | `KiwiHover_DrawWorld`'s brackets are balanced, round AI's new wireframe batch opens after the bracket closed, and this pass re-seeds its own bracket regardless. |
| a new gate / early return | `KiwiRegion_DrawFills` has exactly one call site, and `KiwiCon_DrawWorld`'s two early returns are pre-existing — either would take the construction LINES with it, and the user still sees those. |
| the `KREG_MAX_LOOP` 64 -> 128 raise | Every consumer's static buffers track the macro; a 128-vertex command is 5892 bytes, well inside what `R_GetCommandBuffer` accepts. |
| `KREG_FILL_NUDGE` too small for the ortho depth buffer | 0.5 world units against a 262144-unit linear range on 24 bits = 1/64 unit per step, i.e. **32 steps**. Not marginal. |

**SO THE GEOMETRY WAS REACHING `R_AddRenderCmdDrawTris` AND NOT APPEARING — AND
THERE IS EXACTLY ONE THING THIS FILL DOES THAT NO OTHER FILL IN THE KIWI LAYER
DOES.**

    kiwi_hover.cpp:206-209     s_normal[i] = -c->vpn        (works)
    kiwi_extrude.cpp:213-215   nrm[i]      = -cam->vpn      (works)
    kiwi_region.cpp            s_normal[i] = reg.plane.normal   <- THE ODD ONE OUT

`RB_DrawTriangles_Internal` packs that normal per vertex through
`R_SetVertex4dWithNormal` (rb_backend.cpp:186-217), and the editor's tools
techniques are the **vertcol_SHADED** family — `r_rendercmds.cpp:1944-1947` names
the resolved technique explicitly as `vertcol_shaded_tools`. A fill whose normal is
locked to its own plane therefore presents the **same fixed normal from both
sides**, and — this is the part that matters — **round AA's `KiwiTris_OrientToEye`
cannot reach it.** That call reorders INDICES. It fixes the back-face CULL and it
never touches the normal array. So the two halves of the report line up exactly:

* *"only sometimes"* — the result was a function of which way the sketch's plane
  happened to face, which is why a ground-plane sketch and a wall sketch behaved
  differently and why the behaviour looked random from the user's chair;
* *"not even from the backside"* — orbiting flips the winding (AA's fix works) but
  cannot flip a normal that is nailed to `reg.plane`.

**THE FIX IS THE ONE LINE THE OTHER TWO FILLS ALREADY AGREED ON**, and
kiwi_hover.cpp wrote the rule down when it faced the same question: *"the fan is a
screen-facing decal over the face, TECHNIQUE_UNLIT ignores it, and reading
face->plane would need the def here for no gain."* The region fill is now the third
file to say it. The NUDGE keeps reading `c->vpn` exactly as round R specified —
that argument was always about depth and is untouched.

**AND THE PASS NOW SAYS WHEN IT DREW NOTHING.** Round AG established the rule for
this subsystem — *"both drops now print"* — because a silent `continue` is what made
the unfilled arch unanswerable from inside the editor. The DRAW had no such account,
so *"the fill never shows"* could not be told apart from *"the region never derived"*
without a rebuild. `KiwiRegion_DrawFills` now counts its four outcomes and prints
**only the anomaly** — regions exist and *none* of them reached the renderer —
throttled on the store generation exactly as the loop-cap report is. A healthy
editor never prints; a sick one names which of the three skips ate them, and says
out loud that a region which still extrudes is a draw problem and not a derivation
one. If this round's diagnosis is wrong, the next one starts from a fact instead of
from a re-audit.


---

### 64.2 ITEM 2 — THE GRID, DEFINITIVELY: the LOD was reading a number the ortho image does not contain

**USER REPORT, verbatim:** *"There is still (after like 10 attempts) bugs in the
grid where it snaps and morphs and transforms into an ugly mess. You really gotta
fix this."* The screenshots are TOP/BOT-ish views with the viewcube pill reading
**"P"** — which is the *switch to perspective* affordance, so **every one of them
is an ORTHOGRAPHIC view.** They show banded density messes, dense near-black far
fields, and the lattice re-scaling as the camera merely orbits.

**THE DIAGNOSIS, AND IT IS ONE SENTENCE: THE LOD IS DRIVEN BY CAMERA ALTITUDE, AND
UNDER A PARALLEL PROJECTION ALTITUDE IS NOT A PROXY FOR ANYTHING ON SCREEN.**

Grid v3 (round M) picks its tier from `PickLod( base, fabsf(c->origin[2]) )`. In
PERSPECTIVE that is defensible: the eye height *is* the distance to the ground
under your feet, so it approximates "how big is a cell on screen". In ORTHO it is
unrelated to the image by construction:

| the quantity | what actually sets it |
|---|---|
| ortho image scale | `KiwiCam_OrthoHalfHeight()` = `s_dist * tan(fov/2) * 0.75` |
| camera altitude | `lookAt[2] - forward[2] * s_dist` (kiwi_camera.cpp:812) |

Three consequences, and the user reported all three:

* **ZOOM DOES NOT MOVE THE TIER unless you happen to be in a TOP view.** Only in a
  top view is `forward[2] = -1`, making altitude track `s_dist`. Everywhere else
  altitude is dominated by `lookAt[2]`, the pivot's height — which zooming does not
  touch. Focus on something 2000 units up and zoom in: the tier stays coarse and
  the cells swell to fill the screen. Zoom out: the tier is *still* the same and
  241 lines per axis compress into a few hundred pixels. **That is the dense black
  field, and it is why the same grid looks fine from directly above.**
* **ORBITING DOES MOVE THE TIER.** `origin[2]` swings by up to `s_dist` as the
  camera tips, with nothing on screen changing scale. Cross a Schmitt threshold
  mid-orbit and the whole lattice doubles or halves. **That is "it morphs and
  transforms" — literally, the LOD stepping on an input the user cannot see.**
* **THE FAR RING'S PROBE IS ALSO ALTITUDE MATH, AND IT IS MEANINGLESS IN ORTHO.**
  `FarGroundDistance` shoots `Ed_CameraCalcRayDir` rays and returns
  `h * horiz / dz`. Under a parallel projection *all four rays are the same
  direction*, so the three top-edge samples return one number, and that number is
  "where the eye's own ray meets the ground" — not "how much ground is in the view
  rect". A high pivot at a shallow angle latched the ring ON over a tightly zoomed
  image: an 8x lattice at `band2 * 0.62 * fade` grey. The dark field again, from a
  second direction.

**THE FIX: ORTHO IS DERIVED FROM WORLD-PER-PIXEL AND FROM NOTHING ELSE.**
`KiwiCam_WorldPerPixel` (kiwi_camera.cpp:662) returns `2*H/height` on its ortho arm
and **never reads its `world` argument** — a parallel projection has ONE scale for
the whole image, which is exactly the property that makes this exact rather than
approximate.

    SPACING   the smallest tier base*2^k whose projected cell is >= KGRID_PX_MIN
              (9 px), Schmitt-latched against KGRID_PX_REFINE (22 px).
              NO pitch term.  NO altitude term.  NO pivot term.
              Orbiting cannot move it; panning cannot move it; only ZOOM can.
    REACH     the EXACT XY footprint of the view rect projected onto Z = 0,
              PER AXIS:
                  halfW = wpp*width/2,   halfH = wpp*height/2
                  screen-right on the ground = c->vright  (horizontal, unit)
                  screen-up   on the ground = g = vup - vpn*(vup[2]/vpn[2])
                  need[a] = halfW*|vright[a]| + halfH*|g[a]|
    CENTRE    the SCREEN-CENTRE ground hit, not the eye's ground projection —
              in ortho those differ by the whole pitch offset.

**THIS IS A STRONGER FORM OF ROUND M'S DIRECTIVE, NOT A RETREAT FROM IT.** Round M
was told "density must not change with angle" and answered by making density a
function of altitude. Altitude *is* an angle-coupled quantity in ortho (see the
orbit row above). Pinning the tier to `wpp` makes density a function of ZOOM ALONE,
which is the only camera parameter that changes what a cell looks like.

**THE 1/sin QUESTION, ANSWERED RATHER THAN WAVED AT.** `g` carries a factor of
`vup[2]/vpn[2]`, i.e. 1/sin(pitch), so `need` grows without bound as the view goes
edge-on while the spacing — now pinned to a pixel size — does not grow with it.
Both available answers were weighed:

* **(a) coarsen the spacing by the same 1/sin factor for the far span. REJECTED.**
  That is v2's `while (span/spacing > 200) spacing *= 2` wearing a different hat.
  It puts pitch back into the density under the viewer's feet, which is the one
  thing round M exists to prevent, and it is the exact defect the round-M header
  spends forty lines dissecting.
* **(b) CLAMP THE REACH at `KGRID_HALF_CELLS` cells per axis and let round S's edge
  fade cover the rest. TAKEN.** `vpn[2]` is floored at `KGRID_ORTHO_MIN_SIN`, which
  is `#define`d **as** `KGRID_EDGE_FULL` (0.08, ~4.6 deg) rather than as its value,
  so the two can never drift apart. Below that angle the edge ramp is already
  dropping minors and dimming toward nothing, because 241 lines compressed into a
  few pixel rows cannot resolve at any brightness. Clamping a reach the viewer
  cannot resolve costs nothing real; coarsening the near field to buy it costs the
  directive.

**THE FAR RING SURVIVES IN ORTHO AND NEEDS NO FRUSTUM PROBE THERE.**
`need[a] > reach[a]` *is* the exact statement "there is visible ground the near
window does not cover" — the same two numbers the clamp was computed from. Same
1.30 / 1.05 Schmitt latch, same `s_farOn` (so a P-toggle cannot leave two latches
disagreeing), same majors-only rule round AH established for the same reason.

**DISTANCE BANDS ARE OFF IN ORTHO, AND THAT IS THE "BANDED" HALF OF THE REPORT.**
The three brightness bands model PERSPECTIVE attenuation. A parallel projection has
none: every line is at the same image scale. Banding by world distance from the
camera's ground point therefore paints a literal bullseye centred on the viewer.
Ortho draws ONE flat tier (band 0, the brightest), majors then minors — which also
takes the colour runs from six to two and the `RC_DRAW_LINES` count with them.

**THE ORTHO BUDGET, DECLARED (kiwi_grid.h):**

| | |
|---|---|
| spacing floor | `px >= KGRID_PX_MIN`, so a W x H viewport shows at most W/9 x H/9 cells |
| reach clamp | `reach[a] <= KGRID_HALF_CELLS * spacing` -> `<= 241` indices per axis |
| near window, both axes | 482 |
| axes overlay | 6 |
| FAR ring, majors only | 50 |
| **MAIN BATCH worst case** | **538 <= KGRID_MAX_SEGMENTS (1024)** |
| halo: near majors only | 50 <= KGRID_HALO_SEGMENTS (56) |

Unchanged from round AH **by construction**: the clamp is the same clamp, reached
from the other side.

**PERSPECTIVE IS UNTOUCHED, AND THE AJ WORKAROUND STAYS.** `KiwiGrid_Draw` still
gates the lattice on `KiwiCam_Ortho()` (round AJ item 5b, the user's own choice) and
the altitude ladder, the frustum probe and the three bands are all still there,
byte-for-byte, behind that gate. Changing a path nobody can currently see would be
a change nobody can test. The LOD latch gains a REGIME field (`s_lodMode`) so a
P-toggle cannot hand the new ladder a tier the other ladder chose.

**AND THE SNAP IS NOT THE DISPLAY TIER — CHECKED, NOT ASSUMED.** This mattered more
this round than in any previous one, because the display spacing now changes with
ZOOM, which it never did before. `KiwiGrid_Snap` quantises with
`KiwiUnits_GridSpacingWorld()` and `KiwiSnap_LightGridAxis` with the same call
(kiwi_snap.cpp:1111). **Neither reads `s_lod`**, which is a file-static inside
kiwi_grid.cpp's anonymous namespace and is exported nowhere. So zooming out to a
coarse display tier cannot move a vertex: you land on the spacing the units pill
says, always. No change was needed and none was made.

---


---

### 64.3 ITEM 3 — the fillet you could not click, and the texture that stretched when you did

**USER REPORT, verbatim:** *"(see pic) bevel is good, but I can't select the curve
parts to retexture them. See how it's messed up?"* The screenshot is a chamfered
corner whose fillet patch carries a stretched texture.

**THIS IS TWO INDEPENDENT DEFECTS, AND THE SECOND IS INVISIBLE UNTIL THE FIRST IS
FIXED** — `Brush_SetTexture` returns before touching anything when the selection is
empty (select.cpp:1792), so the retexture bug could not even be observed.

**(a) NONE OF ROUND AF'S FOUR GATES WAS THE CAUSE. ALL FOUR ALREADY DEFAULT
PERMISSIVE**, and that was re-verified rather than assumed:

| gate | default | where |
|---|---|---|
| `g_PrefsDlg->m_bSelectCurves` | **1** (select curves) | prefs.cpp:82 ctor, prefs.cpp:191 load |
| `Misc curve` / `Misc terrain` filter `isShown` | **true** (shown) | filters.cpp:982; registry default 1 at filters.cpp:1091 |
| selection mode | **`SEL_MASK_EVERYTHING`** | kiwi_selection.cpp:55 |

So round AF diagnosed four doors and all four were already open. There was nothing
to flip, and flipping something anyway would have been a change that fixed nothing
while quietly breaking the `Curve` filter for every hand-made bevel in the map.

**THE FIFTH CAUSE, AND IT IS ONE CHAIN END TO END:**

1. Mode 5 (Everything, the default) carries **both** the VERTEX bit and the OBJECT
   bit.
2. `Pick()` runs the screen-space vertex/edge pass **before** the `Test_Ray` area
   pass, and a screen-space hit **returns immediately** — the area pass never runs.
3. `ScanList`'s patch branch accepts **any control point within `PICK_VERT_PIXELS`
   (8 px)**.
4. A fillet arc is `spans*2+1` x 3 control points packed into one chamfer
   (kiwi_patchfillet.cpp:1272-1273). **At working zoom the 8 px discs TILE the whole
   visible patch.**
5. So every click on a fillet resolved to a `SEL_VERTEX` item…
6. …and `Sel_SyncToLegacy` pushes **only** `SEL_OBJECT` onto `selected_brushes`
   (kiwi_selection.cpp:352 — shakeout D removed the vertex-to-brush promotion, for
   reasons that are still right),
7. so `Brush_SetTexture` early-returned and **the Textures-panel click was a silent
   no-op.**

**THAT ONE CHAIN EXPLAINS BOTH HALVES OF THE SENTENCE THE USER WROTE**, and it
explains the shape of the complaint precisely: it bites FILLETS and not large
patches, because the only variable is *control-point density per screen pixel*. It
also explains why pressing `4` (Object) would have worked around it — mode 4 has no
VERTEX bit, so the screen-space pass is skipped entirely.

**THE FIX IS THE RULE THIS FILE ALREADY APPLIES TO FACES.** `Pick()`'s
`faceGranularity` reads *"the FACE bit is set AND the OBJECT bit is not"*. Patch
control points now answer the same test. Under it:

* **mode 5 / mode 4** pick the PATCH — it selects, marquees, and takes a texture;
* **patch vertex mode (V)** sets `SEL_MASK_VERTEX` **alone** (kiwi_patchverts.cpp:370,
  and :286 asserts that ownership as the mode's defining state), so its control
  points pick exactly as they did — the mode round AI built is untouched.

**SCOPED TO PATCHES ON PURPOSE.** Brush vertices in mode 5 are a handful of corner
handles, not a tiling field, and they are a gesture the editor has always had at
that mask. Nothing outside the `if ( b->patch )` branch changes.

**(b) THE STRETCH IS A REAL, SEPARATE DEFECT, AND THE PORTED FUNCTION IS NOT THE
PLACE TO FIX IT.** `Texture_SetTexture` reaches a patch through `sub_476ED0`
(brush.cpp:2852-2871) with `a5 == 1`, whose patch branch copies the two material
POINTERS and bumps `p->version`. It never re-lays `ctrl[][].texCoord` and never
naturalizes — **faithful to 0x476ED0**. So the per-control-point S/T stay as
`Patch_Naturalize2` laid them against the OLD material's dimensions, and the
world-units-per-repeat silently rescales by `newWidth/oldWidth`. Swap a 512 px wall
material for a 128 px one and the surface comes out four times denser. On a fillet —
which round AJ deliberately born at its *parent face's* density — that is exactly
the reported stretch.

**THE COMPOSITION RULE, AND NOTHING HAS TO BE CAPTURED BEFOREHAND.**
`Patch_Naturalize2` lays a LINEAR map, `st[0]_i = widthDist(i) / (w * a3)`, and
`w * a3` **is** the world units one repeat covers (that identity is round AJ's own,
and it is what `Patch_KiwiFinishNewLike` is built on). Because the apply does not
touch `ctrl[][]`, the OLD product survives the swap *inside the coordinates
themselves*:

    WUPR_s = widthDist ( width  - 1 ) /  ctrl[width -1][0].texCoord.st[0]
    WUPR_t = heightDist( height - 1 ) / -ctrl[0][height-1].texCoord.st[1]

(the T sign is `Patch_Naturalize2`'s negated `tScale`). Feed those back as a
synthesized `texdef_sub_t.size[]` and `Patch_KiwiFinishNewLike` computes
`a3 = WUPR_s / newWidth`, so the re-lay lands at `1 / (newWidth * WUPR_s / newWidth)`
= `1 / WUPR_s` — **the density the patch already had, against the material it now
has.** The old width cancels; no before-state is needed.

**SO ROUND AJ'S INHERITANCE AND A MANUAL RETEXTURE COMPOSE**, which is what this
item was asked to guarantee: the fillet is born at its parent face's density and
re-texturing KEEPS that density instead of snapping back to the editor default. A
patch whose coordinates are degenerate on an axis (zero span, or an S that never
advanced) takes the editor default **on that axis only** — `Patch_KiwiFinishNewLike`
already gates each axis independently on `size[n] > 0` — which is what stock
Patch->Naturalize does and is always better than leaving the stretch in.

**IT LIVES IN pmesh.cpp AND IS CALLED FROM ONE FENCE.** `Patch_KiwiReNaturalize`
sits beside `Patch_KiwiFinishNewLike` because it needs that file's static
`Patch_WidthDistanceTo` / `Patch_HeightDistanceTo` — reimplementing an arc-length
accumulator whose float-vs-double behaviour is deliberately pinned to the binary
would have been the wrong kind of copy. The call is a single fenced line at the end
of `TexWnd_ApplyMaterialAtIndex`, the one funnel a thumbnail click goes through, and
it walks the SAME `selected_brushes` list the apply walked, so it re-lays exactly
what the apply reached and nothing else. It also rebuilds `curveDef` on the way out
(`Patch_KiwiFinishNewLike`'s tail), which is what `PMESH_51`'s pick needs — so it is
self-healing on that axis too.

**TWO GATES ROUND AF'S DIAGNOSTIC STILL DOES NOT REPORT**, found while proving the
above and logged rather than fixed, because neither is implicated in this report and
both are one console line away from being answerable: the **Reverse Filter**
checkbox (filters.cpp:704 inverts the entire filter verdict for every brush) and
**edit layer 1** (filters.cpp:691 filters non-fixedsize brushes outright). Also
worth knowing: `CFilterWnd_SaveCheck` (filters.cpp:1096) **has no callers**, so an
ImGui filter un-tick is never persisted — which means a stale
`HKCU\...\Filters\Curve = 0` left by a real CoD4Radiant install cannot be repaired
from the panel across a restart. All three are in RADIANT_KNOWN_ISSUES.


---

### 64.4 ITEM 4 — the gizmo, filled and one step brighter

**USER DIRECTIVE, verbatim:** *"The new gizmo's are better, but I want them slightly
brighter colors and filled in (no hollow shapes)."*

**THE OUTLINES WERE AN ADMISSION, AND ROUND AJ WROTE IT DOWN ITSELF.** Its head
comment reads *"two chevron segments PLUS the base line closing them, so it reads as
the small solid cone Plasticity uses ... instead of as a bare V"* — an approximation
declared as one, because this file only had a line batcher. The directive is to stop
approximating.

| element | round AJ | now |
|---|---|---|
| arrowhead | 2 chevron lines + a base line | a **closed cone**: `KGZ_CONE_SEGS` (10) side triangles + a 10-triangle base cap, opaque |
| plane handle | a 14 px square OUTLINE | the outline **plus two filled triangles** over the same four `PlaneSquare` corners |
| origin | a 10 px RING | the ring **plus a 24-triangle billboarded disc** over the same `CentreRingPoint` rim |
| rotate rings | lines | unchanged — they are circles, and a filled circle is not a ring |

**THE FILL PATH IS THE ONE EVERY OTHER KIWI FILL USES**, deliberately: one
`R_AddRenderCmdDrawTris` on `g_qeglobals.d_white` with `TECHNIQUE_UNLIT`, inside a
neutral `MATERIAL_COLOR` bracket so the per-vertex colour drives the draw, with
`KiwiTris_OrientToEye` per element so nothing is lost to the white_tools back-face
cull (kiwi_lines.h TRAP 3). Identical in shape to kiwi_region.cpp's region fills and
kiwi_split.cpp's cut quad — five files already carry the same extern block.

**THE LINES STAY, AND THAT IS NOT LAZINESS.** Every filled element keeps its outline
on top. The shafts are lines by definition; and a 2 px outline over a fill is what
makes a 14 px arrowhead read as a crisp silhouette rather than a smear. It also
means the hover overdraw (pass 2, the 3 px re-emit) is completely untouched, so
**grab semantics, hit zones and the resolution order are exactly what round AJ
shipped.** This round adds pixels and nothing else.

**OPACITY IS PER ELEMENT AND EACH ONE HAS A REASON.**

* **HEADS opaque.** A closed cone drawn translucent shows its own far wall through
  its near one — the fill would look like a bug.
* **PLANES translucent (0.42).** The square sits over the scene at 35 px out and the
  user has to see past it; Plasticity's is translucent for the same reason. Hover
  and held raise it to 0.72, because on a FILL a lighter tint alone is a weak
  signal — opacity is the second channel, the same argument round AI made for the
  hover wireframe.
* **ORIGIN nearly opaque (0.85, 1.0 emphasised).** It is 10 px across and marks the
  pivot, which is the one place the user wants an unambiguous answer.

**"SLIGHTLY BRIGHTER" IS LITERALLY ONE RAMP STEP.** The REST row moves from the 600
ramp to the **500** ramp of the same three scales — X `#E12D39`, Y `#27AB83`,
Z `#3B82F6`. The HOVER row stays on the 400 ramp and HELD stays yellow, so the three
states are still one step apart each and none collapsed into another. Round AJ's
argument for leaving the near-primary triple behind (a saturated primary vibrates
against a mid-grey viewport at 2 px) stands — this is a step, not a walk-back.

**THE FILL BUDGET IS SEPARATE FROM THE SEGMENT BUDGET** and is declared in
kiwi_gizmo.h: triangles do not consume `KGIZMO_MAX_SEGMENTS`, so the line budget is
untouched. At most **8 `R_AddRenderCmdDrawTris` and 110 triangles** per frame
(3 axis cones + 1 face-normal cone + 3 plane quads + 1 disc), one bracket around the
lot, and staging arrays sized by the **largest single element** (the disc, 25 verts)
because every element flushes before the next one starts.

---

## Decision log — round AK

- **D-AK1 — a fill's NORMAL is the VIEW normal, in every file, without exception.**
  `KiwiTris_OrientToEye` reorders indices and cannot reach the normal array, so a
  plane-locked normal is invisible to the one mechanism that was supposed to make
  fills two-sided. kiwi_hover.cpp and kiwi_extrude.cpp already said this; the region
  fill was the last holdout and is now the third file to agree.
- **D-AK2 — "it draws nothing" must be distinguishable from "it derived nothing",
  from inside the editor.** Round AG made the DERIVATION drops print. This round
  makes the DRAW account for itself, reporting only the anomaly and only once per
  store generation. A round should never again have to re-audit an entire pass to
  learn which of four `continue`s ate the frame.
- **D-AK3 — in ORTHO the grid LOD comes from WORLD-PER-PIXEL and from nothing
  else.** Altitude is a good perspective proxy and no ortho proxy at all: it is
  dominated by the pivot's height, it does not move with zoom outside a top view,
  and it *does* move with orbit. `KiwiCam_WorldPerPixel` is exact and constant
  across a parallel projection, which is what makes this a derivation rather than a
  heuristic.
- **D-AK4 — this is a STRONGER form of round M's directive, not a walk-back.** M was
  told density must not change with angle and answered with altitude; altitude is
  itself angle-coupled in ortho. Pinning the tier to `wpp` makes density a function
  of ZOOM ALONE — the only camera parameter that changes what a cell looks like.
- **D-AK5 — the 1/sin blow-up is answered by CLAMPING REACH, never by coarsening
  SPACING.** Coarsening for the far span is v2's dead auto-coarsen loop in a new
  hat: it puts pitch back into the density under the viewer's feet. The clamp bites
  only below `KGRID_ORTHO_MIN_SIN`, which is `#define`d **as** `KGRID_EDGE_FULL` so
  the two can never drift — i.e. only where round S's ramp already owns the frame.
- **D-AK6 — in ortho the far ring needs no frustum probe.** `need > reach` is the
  exact statement of "there is visible ground the near window does not cover", from
  the same two numbers the clamp used. The ray probe is meaningless under a parallel
  projection (every ray is the same direction) and is left to the perspective arm.
- **D-AK7 — distance BANDS are a perspective idea and are off in ortho.** A parallel
  projection has no distance attenuation to model, so banding by world distance from
  the camera paints a bullseye centred on the viewer. One flat tier.
- **D-AK8 — the LOD latch carries its REGIME.** Two ladders measuring different
  quantities against different thresholds may not hand each other a tier; a P-toggle
  resets `k`.
- **D-AK9 — the perspective grid is not touched, because it is not drawn.** Round
  AJ's suppression (the user's own choice) stays, and the altitude ladder, the ray
  probe and the three bands stay intact behind it. Changing a path nobody can see is
  a change nobody can test.
- **D-AK10 — the SNAP is never the display tier, and that was checked this round
  because it finally mattered.** Display spacing now moves with zoom; `KiwiGrid_Snap`
  and `KiwiSnap_LightGridAxis` both quantise with `KiwiUnits_GridSpacingWorld()` and
  neither can see `s_lod`, which is not exported. No change was needed and none was
  made.
- **D-AK11 — granularity means "the bit is set AND the OBJECT bit is not", for
  PATCH CONTROL POINTS as it already did for faces.** The screen-space pass returns
  before the area pass ever runs, so at mode 5 a dense control lattice is a wall the
  object behind it can never be picked through.
- **D-AK12 — scoped to patches, not to all vertices.** Brush vertices at mode 5 are
  a few corner handles and a long-standing gesture; patch control points on a fillet
  are a tiling field. The defect is density, so the fix is where the density is.
- **D-AK13 — four open doors are not a cause.** Round AF's gates were re-verified
  and every one already defaults permissive. Flipping a default that is already
  permissive would have shipped a no-op and broken the `Curve` filter for every
  hand-made bevel. The report was answered by finding the fifth cause, not by
  loosening the four.
- **D-AK14 — the ported apply is not the place to naturalize.** `sub_476ED0` is
  IDA-pinned and its patch branch is correct as transcribed. The composition happens
  in the KIWI fence at the click funnel, over the same list the apply walked.
- **D-AK15 — the old density is recovered FROM THE COORDINATES, so nothing needs
  capturing before the apply.** `Patch_Naturalize2`'s map is linear and the apply
  does not touch `ctrl[][]`, so world-units-per-repeat is readable after the fact and
  the old material's width cancels out of the answer.
- **D-AK16 — gizmo fills go on the shared triangle path, and the OUTLINES STAY.**
  Round AJ's shapes were declared approximations in their own comments; they are
  meshes now. Keeping the outlines means the hover overdraw, the hit zones and the
  resolution order are byte-for-byte what AJ shipped — the round adds pixels and
  nothing else.
- **D-AK17 — opacity is a per-element decision with a per-element reason.** Opaque
  heads (a translucent closed cone shows its own far wall), translucent plane squares
  (the user must see past them), a near-opaque origin disc (it marks the pivot).
  Hover and held raise the plane's opacity, because on a fill a lighter tint alone is
  a weak signal — the same "second channel" argument round AI made for the hover
  wireframe.
- **D-AK18 — "slightly brighter" is one ramp step, exactly.** Rest moves 600 -> 500;
  hover stays on 400 and HELD stays yellow, so all three states remain one step apart
  and none collapsed into another. D-AJ20 and D-AJ21 both still stand.

## 65. Round AL — three follow-ups, all of them on round AK's own fixes

Three reports, all recurrences. The design rationale for each is at the code as
well; this section carries what does not fit in a comment — the diff table that
killed the coordinator's premise on item 1, the always-on-top precedent for item
2, and the foreshortening derivation for item 3. Decision log D-AL1..D-AL12.

### Item 1 — the region fill is angle-dependent, and the *normal* is why (again)

USER REPORT, verbatim: *"the blue face only shows up at steep angles. Fix this!"*
Screenshots: one sketch, the fill present from a high/steep view and absent from a
shallower one, **including where the region hangs off the wall over open air** —
so it is not depth-fighting the coplanar face.

**THE PROPOSED FIX WAS ALREADY IN THE TREE, and that is the finding.** The brief
was "make the region fill's draw byte-identical to the hover fill". It already
is. Field by field, `KiwiRegion_DrawFills` (kiwi_region.cpp:1270) against
`EmitFaceFill` (kiwi_hover.cpp:176), as they stood at c5457db:

| field | region fill | hover fill | same? |
|---|---|---|---|
| material | `g_qeglobals.d_white` | `g_qeglobals.d_white` | yes |
| technique | `TECHNIQUE_UNLIT` (4) | `TECHNIQUE_UNLIT` (4) | yes |
| MATERIAL_COLOR bracket | neutral `{0,0,0,0}` then white | neutral `{0,0,0,0}` then white | yes |
| bracket scope | once per pass | once per pass (caller owns it) | yes |
| colour channel | per-vertex, `Byte4PackPixelColor` bit-cast | per-vertex, `Byte4PackPixelColor` bit-cast | yes |
| `xyzw[3]` | `1.0f` | `1.0f` | yes |
| `st` | `{0,0}` | `{0,0}` | yes |
| normal | `-c->vpn` | `-c->vpn` | **yes (AK made them equal)** |
| eye-orient | `KiwiTris_OrientToEye`, stride 4 | `KiwiTris_OrientToEye`, stride 4 | yes |
| index list | ear-clip triangulation | fan from vertex 0 | equivalent |
| draw call | `R_AddRenderCmdDrawTris` | `R_AddRenderCmdDrawTris` | yes |
| pass position | camwnd.cpp:3022 (`KiwiCon_DrawWorld`) | camwnd.cpp:3013 (`KiwiHover_DrawWorld`) | adjacent, same block |
| eye-ward nudge | `KREG_FILL_NUDGE` 0.50 | `KHOVER_FILL_NUDGE` | *differs (by design)* |
| colour values | `KREG_FILL` etc. | `KFILL_HOVER` etc. | *differs (by design)* |

Everything except the nudge constant and the palette was already identical.
Identity was therefore **not** the fix, and the hover fill is not a control: it
carries the same defect and nobody had noticed because a face tint is transient.

**WHAT IT ACTUALLY IS.** `d_white` is `white_tools`, techset `tools`, whose
`unlit` entry is **`vertcol_shaded_tools`** (section 55's asset decode). Its pixel
shader is measured — `rgb = lerp(sample(colorMap)*vertexColour, matColor.rgb,
matColor.w)` (r_rendercmds.cpp:1927-1946) — and its VERTEX stage is the "shaded"
half of the name: r_shade.cpp:366-372 records, from a live DEVPROBE, that a
zeroed def constant *"NaNs the fakelight vertex colour"*, i.e. the vertex colour
the pixel shader receives has already been modulated by a term the vertex shader
computes **from the normal**. The shader is not in this tree; its *direction* was
derived from two rounds of user evidence instead:

* round AK, normal = the region's own PLANE normal: a sketch on a **wall**
  (horizontal normal) never showed **from either side**, a sketch on the ground
  did. So the term is about zero for a horizontal normal and large for a vertical
  one, and it is **not** view-relative — a wall seen face-on was still dark, which
  excludes `dot(N,V)`, and being dark from both sides excludes `abs(dot(N,.))`.
* round AL, normal = `-vpn`: bright looking down, gone as the view levels. So
  `normal.z` is what drives it, again.

One model fits both: **a fixed-direction, roughly world-UP fake light.** So the
fix is neither of the two normals tried: it is a **constant**, and the constant
that maximises an up-lit term is world **+Z**. That is also what the BINARY does
— `Face_AddWindingToTriBatch` (brush.cpp:5915-5917, 0x47b86a), the batcher behind
the ported selected-face fill the brief nominated as the angle-independent
reference, writes a fixed WORLD normal per vertex and never a camera vector. The
kiwi layer's `-vpn` was this port's invention. The rule now lives once, as
`KiwiTris_FillNormal` (kiwi_lines.h **TRAP 4**), and six emitters use it:
kiwi_region, kiwi_hover, kiwi_extrude, kiwi_split (x2), kiwi_loft, kiwi_gizmo.

**WHY NOT THE OTHER IMMUNITY.** `MATERIAL_COLOR.w = 1` lerps the whole vertex
term away, and it is exactly why the editor's LINES never showed any of this —
`Ed_EmitLineBatch` pushes a per-colour-run MATERIAL_COLOR with `w == 1`
(r_rendercmds.cpp:1940-1975). It is refused because `.w` is the lerp weight and
these fills need per-vertex ALPHA: whether the shader's alpha output is
`sample.a * vcol.a` (fill survives) or `matColor.a` (fill becomes an **opaque
slab**) cannot be read from this tree, and round AH already paid for one
opaque-slab regression. Documented as the next probe, with its risk named.

**THE ROUND-AK DIAGNOSTIC WOULD NOT HAVE FIRED, and that is a flaw in the
instrument.** `nDrawn` is incremented unconditionally after
`R_AddRenderCmdDrawTris` returns, and that function returns `void` and drops
silently (missing technique, buffer overflow). Regions derive and geometry is
submitted, so `nDrawn > 0` and the pass prints **nothing** — which, by round AK's
own decision table, reads as *"the geometry is submitted and the emitter or its
blend state is at fault"*. That is the correct verdict. The counter is left as it
is: it still catches the derivation-side failures it was written for.

### Item 2 — the gizmo is on top of everything now, unconditionally

USER REPORT, verbatim: *"the gizmos are only solid at a high zoom level. Wtf? Fix
this."* At a wide zoom the filled cones/squares/disc go, the outlines stay.

**FILL-VS-OUTLINE IS THE SIGNATURE, and item 1 explains it.** The outlines are
immune to the vertex shading term (flat `MATERIAL_COLOR.w == 1` per colour run);
the fills are not (neutral `{0,0,0,0}`, so the vertex term drives the draw). The
gizmo fills carried `-c->vpn` with a comment asserting *"TECHNIQUE_UNLIT does not
shade with it"* — the same false claim kiwi_hover.cpp carried. They now use
`KiwiTris_FillNormal`.

**AND THE DIRECTIVE IS IMPLEMENTED ON ITS OWN TERMS: a manipulator renders on top
at every distance, full stop.** A gizmo is a HUD object with world coordinates —
screen-constant in size, so its WORLD extent grows without bound as the view
zooms out (`KGZ_AXIS_PIX * wpp` is about 0.12 * `s_dist` in ortho, hundreds of
units at a wide zoom) and it is anchored at the selection pivot, normally inside
the geometry being moved. Every depth question about it therefore changes with
zoom. A bigger eye-ward nudge is the wrong shape of answer — one more number that
is right at one distance.

**THE MECHANISM, AND ITS PRECEDENT.** Not a material trick: `R_AddCmdClearScreen(
6 /* depth|stencil */, ...)`, which is the editor's own always-on-top device. The
binary opens its selected-brush white outline with exactly that call *"so the
selected wireframe passes the depth test against the coplanar geometry and shows
THROUGH"* (camwnd.cpp:2883-2889, 0x4084d2), and the port already re-issues it a
second time when a later pass has dirtied the buffer under an overlay
(camwnd.cpp:2996-3001, the terrain-paint ring). This is the third use and the
same sentence. It buys two things the existing prelude clear did not:

* the KIWI overlay block runs a long way after that prelude and **every line pass
  in it writes depth** — `$line` is depthTest LESSEQUAL / depthWrite ON
  (`refStateBits[1] = 0x0d`, decoded at camwnd.cpp:2422-2432). Hover outlines,
  construction lines, the marquee, the patch lattice, the snap accents and the
  live command overlay all stamp the cleared buffer before the gizmo draws;
* the prelude clear is gated on `!dontDrawSelectedOutlines` (mainfrm.cpp:5121),
  so "the gizmo is on top" was conditional on a preference it has nothing to do
  with.

Rejected alternatives, each for a measured reason. `$default` / techset `2d` to
`vertcol_simple2d` to `default2d.sm` really is depthTest **and** depthWrite
Disable (section 55/57 decode) — the right depth state — but its
`refStateBits[0]` is `0x08124812`, **blendOp Disable = OPAQUE**, which would turn
the 0.42-alpha plane squares into solid slabs. `$line_nodepth` is depthTest OFF
but `default.sm` forces its depth WRITE back ON (camwnd.cpp:2439-2450 already
worked this out for the grid), so it paints over everything *and* stamps depth.
The clear costs one `RC_CLEAR_SCREEN` per frame, only on frames where a gizmo
exists, and nothing after it in the block reads depth except the lollipop — which
is mutually exclusive with the gizmo by `GizmoUsable()`.

**A THIRD, INDEPENDENT DEFECT FOUND WHILE PROVING THIS.** `KiwiTris_OrientToEye`
tested the camera **position** in a view that has no eye point. Under a parallel
projection the rasteriser's winding sign is `n . vpn`; the position does not enter
it. KIWI's ortho pseudo-eye sits only `s_dist` back with a half-height of
0.75 * `s_dist`, so a vertex at the screen edge subtends about 37 degrees off the
axis — every triangle within that cone of edge-on was flipped the wrong way and
culled (the silhouette walls of the gizmo's cones, the rim of a large fan). The
helper now tests the DIRECTION in ortho and the point in perspective. This is
angle-coupled rather than zoom-coupled, so it is a contributor and not the whole
report.

**HONEST LIMIT.** The exact monotonic coupling to zoom could not be pinned to a
single line from source alone. The always-on-top clear subsumes every
depth-shaped cause including ones not identified, and the normal fix removes the
one mechanism that is fill-only by construction. Two suspects were **ruled out**
with argument rather than left open: `KiwiTris_OrientToEye`'s degeneracy test
(`n.n <= 1e-12`) fails on SMALL triangles, so if it ever bit it would bite at
HIGH zoom — the opposite end of the report; and the render-command buffer
(48 MB, r_rendercmds.cpp:149) drops *everything* after it fills, which would take
the outlines with the fills.

### Item 3 — the ortho ladder measures the wrong axis of the cell

USER REPORT, verbatim: *"Grid ugliness still there. not fixed."* Screenshot: a
TOP-ish but **tilted** ortho view (the cube shows its top corner with -X), 6 in
spacing, anisotropic dense line bands in the middle distance.

**THE DERIVATION.** Under a parallel projection a world displacement `d` lands on
screen at `(d.vright, d.vup)/wpp` — no divide, so this is exact everywhere in the
image. `CamWnd_BuildMatrix` builds the basis with no roll, so at pitch phi, yaw
psi:

```
vpn    = ( cos phi cos psi,  cos phi sin psi, -sin phi )
vright = (         -sin psi,          cos psi,       0 )
vup    = ( sin phi cos psi,  sin phi sin psi,  cos phi )
```

Split a GROUND displacement `d` (`d_z = 0`) as `d = a*vright + b*h` with
`h = (cos psi, sin psi, 0)` the ground-forward direction. Then

```
d . vright = a                     (vright is horizontal - this axis is UNTOUCHED)
d . vup    = b * sin phi           (h . vup = sin phi - this axis is COMPRESSED)
```

The ground plane is compressed by **exactly `|sin phi| = |vpn.Z|`** along the
screen's vertical axis and not at all along `vright`, uniformly over the view.
The two line families' screen pitches are therefore

```
lines parallel to h        :  spacing / wpp                 (full)
lines parallel to vright   :  spacing * |vpn.Z| / wpp       (compressed)
```

Round AK's ladder held the **first** above `KGRID_PX_MIN` = 9 px. A 9 px floor on
the first is a `9*|vpn.Z|` px floor on the second: **4.5 px at 30 degrees, 2.3 px
at 15 degrees** — under the resolvable limit, on one axis only. That is the
report: dense bands, anisotropic. The ladder now takes
`wppEff = wpp / |vpn.Z|`, which makes its test `px = spacing*|vpn.Z|/wpp` — the
foreshortened extent, exactly. At a straight top-down view `|vpn.Z| = 1` and the
behaviour is bit-for-bit round AK's.

**WHY THIS IS NOT THE HISTORIC PITCH-COUPLING DISEASE.** That disease was
SPATIAL: a per-line or per-row factor under a perspective divide, so density
varied across one image and crawled within one frame. `|vpn.Z|` in ortho is one
scalar per frame, identical at every pixel by construction, and it feeds the SAME
Schmitt-latched integer `s_lod` that zoom already feeds. Orbiting can now move
the tier — but unlike round AK's altitude ladder, which moved it while nothing on
screen changed scale, the cells genuinely **are** compressing when it does.
`KGRID_PX_REFINE / KGRID_PX_MIN = 22/9 = 2.44 > 2`, so the latch cannot
oscillate.

**FADE AND LADDER HAND OFF AT ONE CONSTANT, rather than fighting.** `|vpn.Z|` is
clamped at `KGRID_ORTHO_MIN_SIN`, which is *defined* as `KGRID_EDGE_FULL` (0.08,
about 4.6 degrees) — the exact angle below which `EdgeFade` starts ramping the
lattice out and `OrthoFootprint` already clamps. So the ladder stops coarsening
precisely where the fade takes over. Bounded: 1/0.08 = 12.5x, under four tiers,
and `KGRID_LOD_MAX` still caps it.

**THE FAR RING AND THE MAJORS NEED NO SEPARATE GATE — checked, and the brief's
worry is the wrong way round.** The far ring's spacing is the near spacing
`<< KGRID_FAR_STEP` (**8x**), and the majors are a coarser multiple again, so
both are 8x or more **above** whatever floor the near lattice just satisfied. A
coarser tier can only be safer; the direction of the inequality is the whole
argument. Nothing else in the ortho arm reads a pixel size, so there is no second
independent contributor to name — and the budget only moves the safe way, because
the ladder can now only coarsen relative to round AK, never refine.

## Decision log — round AL

- **D-AL1 — D-AK1 IS REVERSED. A fill's normal is a CONSTANT, never a camera
  quantity.** AK was right that the normal is the channel and wrong about the
  replacement: `-vpn` trades "depends on the sketch plane" for "depends on where the
  camera points", which is worse, because a user orbits far more often than they
  re-plane. The binary's own fill batcher writes a fixed world normal
  (brush.cpp:5915) and has all along.
- **D-AL2 — the rule lives in ONE function, `KiwiTris_FillNormal`.** Six emitters
  had six copies of a three-line camera read. Round AA put the winding rule in one
  place for exactly this reason; the normal rule now sits beside it as TRAP 4.
- **D-AL3 — a shader that is not in the tree is characterised from USER EVIDENCE,
  and the model must explain every round, not the latest one.** Two failure reports
  under two different normals uniquely select a fixed, up-ish light: view-relative
  and two-sided terms are both excluded by round AK's wall-seen-face-on case. That
  is a derivation, and it is written down so the next round can falsify it rather
  than re-guess.
- **D-AL4 — the `MATERIAL_COLOR.w = 1` immunity is REFUSED while the alpha channel
  is unknown.** It is strictly more robust against the shading term and carries an
  opaque-slab risk that cannot be settled from this tree. Named as the next probe
  with its failure mode spelled out, not taken silently.
- **D-AL5 — "byte-identical to a fill that works" is only a fix if the two differ.**
  They did not. The premise was checked field by field before any code moved, and
  the table is in this section so the next round does not spend a pass re-checking
  it.
- **D-AL6 — an instrument that counts SUBMISSIONS cannot detect a drop after
  submission.** `nDrawn` increments after a `void` call that fails silently. Left in
  place (it still catches derivation-side drops) but its blind spot is now written
  down next to it.
- **D-AL7 — a manipulator is always on top, and that is a PASS-ORDER property, not
  a nudge.** Any answer expressed in world units is right at one distance. The depth
  clear is the editor's own device, used twice already in this file, and it makes the
  guarantee unconditional in one line.
- **D-AL8 — a depth-test-disabled MATERIAL is refused for overlay FILLS.** The `2d`
  family's state is right and its blend state is not (opaque), and `$line_nodepth`
  has its depth write forced back on by `default.sm`. The state we want is not
  available on any shipped material; the clear does not need one.
- **D-AL9 — the gizmo's always-on-top must not depend on an unrelated preference.**
  It was riding on `!dontDrawSelectedOutlines`. A pref that hides selection outlines
  has no business deciding whether the move handles are reachable.
- **D-AL10 — in ORTHO, "which way does this triangle face" is a question about a
  DIRECTION, not about a point.** The rasteriser has no eye; neither should the
  helper that predicts it. The perspective arm is untouched and still exact there.
- **D-AL11 — a screen-space floor must be applied to the SMALLEST projected extent
  of the cell, not to a convenient one.** The un-foreshortened cell size is the axis
  that never collapses, so testing it guarantees nothing. One `|vpn.Z|` divide moves
  the test onto the axis that actually aliases.
- **D-AL12 — a FRAME-LEVEL scalar is not the pitch coupling round M banned; a
  PER-LINE one is.** The ban exists because density varying across one image reads as
  crawl and moire. A parallel projection's foreshortening is constant over the whole
  image, so honouring it removes moire instead of creating it. The distinction is now
  written into the header so it is not re-litigated.

## 66. Round AM — seven reports, and three of them were one bug

Seven items. The finding that shapes the whole round is that **items 1, 2 and 4
are the same defect on three different shapes**, which is why item 1 had survived
four rounds of single-suspect fixes: every one of them looked at the region fill
alone. Decision log D-AM1..D-AM15.

### Item 1 — the region fill: the complete gate table, with verdicts

The brief asked for the whole chain from `Cam_Draw` to the D3D submission, every
gate, and a verdict for **this exact scenario**: sketch on a WALL plane, NO
construction tool active, mode 3, perspective camera.

| # | Gate | Where | Verdict for this scenario |
|---|---|---|---|
| 1 | `Cam_Draw` reaches its KIWI overlay block | camwnd.cpp:3005-3022, a BARE block — no `if` between the selected-outline pass and it | **OPEN** |
| 2 | `KiwiHover_DrawWorld` then `KiwiCon_DrawWorld` | camwnd.cpp:3013 / 3022 | **OPEN** — adjacent, unconditional |
| 3 | `KiwiCon_ShowConstruction()` | kiwi_construct.cpp:4089, profile default 1 | **OPEN** — *and proven open by the user's own screenshot: the rose construction lines are drawn by the very next statements in the same function* |
| 4 | `c->width/height >= 1` | kiwi_construct.cpp:4092-4094 | **OPEN** |
| 5 | **Is `KiwiRegion_DrawFills` called with no tool live?** | kiwi_construct.cpp:4099 — the FIRST statement after gate 4, before `KiwiLines_Begin` | **YES. UNCONDITIONALLY.** Every caller was grepped: there is exactly one, and no `s_activeTool` test exists anywhere on the path. Nothing re-gated it after round AA |
| 6 | `EnsureBuilt()` re-entrancy | kiwi_region.cpp:937-955 | **OPEN** — `s_builtFor`/`s_dirty` are committed before return, so the nested call from `ResolveCentroid` short-circuits |
| 7 | `regions.empty()` early return | kiwi_region.cpp:1276 (as it stood) | **THE ONE GATE WITH ZERO INSTRUMENTATION.** It printed nothing, ever. Now it prints, and runs `KiwiRegion_ReportGaps` |
| 8 | per-region corner count / `KREG_MAX_LOOP` | the loop | open unless the sketch is degenerate; counted, and now named |
| 9 | `KiwiRegion_Triangulate` | `AcceptLoop` has already run `RewindCCW` and the self-intersect test | **OPEN**; counted, and now named |
| 10 | `KiwiTris_OrientToEye` | kiwi_lines.cpp:79-131 | **OPEN** — reorders indices only, cannot drop a triangle; the perspective arm tests `n.(eye-a)`, the convention the ported face fill confirms |
| 11 | `Material_GetTechnique(d_white, UNLIT)` — **silent `return`** | r_rendercmds.cpp:2153 | **PROBED NOW** (`stateBitsEntry[tech] == 0xFF`, the read camwnd.cpp:589 already uses) |
| 12 | `R_GetCommandBuffer` overflow — **silent `return`** | r_rendercmds.cpp:454-505 | **PROBED NOW** via `R_Ed_CmdBufferHeadroom` |
| 13 | `RB_DrawTriangles_Internal` | rb_backend.cpp:1407-1444 | no drop path other than tess overflow, which a 128-vertex fan cannot reach |
| 14 | blend / shader | `white_tools` unlit -> `vertcol_shaded_tools` | **THE ONLY GATE LEFT — and this is the answer** |

**SO THE CALL CHAIN IS PROVABLY OPEN, and gate 14 is where it dies.** The proof
that it is gate 14 and not gate 7 does not come from the region fill at all — it
comes from **item 2**. A FACE has exactly one visual channel in this layer:
`EmitItem` carried an explicit empty `case SEL_FACE:` (shakeout G, so that a face
accent cannot look like an edge one), and the tint comes from `DrawFaceFills`.
`KiwiHover_Update` picks with the **live** mode mask (kiwi_hover.cpp:707), so in
mode 3 the hover result *is* a `SEL_FACE`, `hoverIsFace` accepts it, and
`EmitFaceFill` emits for it — **the wiring item 2 asks for already exists and has
since shakeout G.** "No hover highlight on a face in mode 3" therefore says the
same thing item 1 says: *the fill is emitted and does not appear.* Item 4 says it
a third time — the gizmo heads have had real triangles since round AK and what is
on screen is round AJ's outline chevron.

Three shapes, three independent reports, one call: `R_AddRenderCmdDrawTris` on
`d_white` inside a **neutral** `MATERIAL_COLOR` bracket. And in the same frame,
same layer, same material family, every LINE renders at full strength.

**THE MEASUREMENT WAS ALREADY IN THE TREE.** r_rendercmds.cpp's own
RADIANT_LINEVCOL experiment (:1913-1930) records that with `MATERIAL_COLOR`
neutral — the state that makes the draw `sample(colorMap) * vertexColour` — *"the
XY brush wireframes came back at ~0.32x, so the neutral-matColor route needs the
colorMap binding verified first"*. That is the whole story. The editor draws
outside a full scene render, so under a neutral bracket the tools shaders return
about a **third** of the colour asked for. A line at 0.32x is dim but visible. A
0.22-alpha fill whose colour is also multiplied by 0.32 is nothing.

**AND THE FIX IS THE ROUTE THE LINES TOOK.** `Ed_EmitLineBatch` pushes a
per-colour-run `MATERIAL_COLOR` with `.w == 1` — a flat colour override that
lerps the sampled term entirely away — which is *exactly* why the outlines in
this layer never showed any of this and the fills did. `KiwiTris_FillFlatColor`
(kiwi_lines.h **TRAP 5**) is that push, spelled once.

**THE RISK IS REAL AND IS SCOPED RATHER THAN WAVED AWAY.** `.w` is the lerp
weight; whether the alpha output is `sample.a * vcol.a` or `matColor.a` cannot be
read from this tree. Round AL refused the probe on that ground and cited "the
round-AH failure mode" — **but round AH's regression was the grid's far ring
drawing minors** (sections 61/63, "the black-slab correction"), a line-density bug
with no alpha in it. There is no evidence in this tree that a flat override turns
a fill opaque, only an absence of evidence that it does not. So it is taken where
the downside is zero or bounded: **gizmo heads (alpha 1.00) and the origin disc
(0.85), which are opaque by design, and the REGION fill as the one translucent
probe** — which is what RADIANT_KNOWN_ISSUES round AL asked for in as many words
("Try it on ONE fill first"). Hover face fills, plane squares and
extrude/split/loft stay on the neutral bracket, so the layer cannot go uniformly
opaque on one unproven assumption.

**AND THE INSTRUMENT IS SHIPPED REGARDLESS,** because the brief is right that a
fourth failed guess is worse than a measurement. The old counter incremented
`nDrawn` *after* a void call (D-AL6). The new one counts **triangles submitted,
computed before the submit**, threads a **gate name** through every early-out, and
probes both in-renderer drops before they can happen. Its silence is now
informative: it means the geometry reached the backend with a technique and room
to hold it, and the loss is blend state or shading.

### Item 3 — the grid strays are the FAR RING, and the fade's ceiling is the hole

Identified rather than guessed, on four properties: they are **dark** (the ring
paints `KGRID_BANDS[2][1]`, the dimmest tier, times `KGRID_FAR_MUL` 0.62 — nothing
else in the ortho arm draws that dim), **long and stopping in mid-air** (its reach
is `ClampAxis`-clamped exactly as the near window's is), **diagonal**
(world-axis-aligned under yaw), and **a handful** (majors only since round AH, at
8x the near spacing).

**Why the existing fade does not catch them, exactly.** `EdgeFade` returns 1.0 for
every `|vpn.Z| >= KGRID_EDGE_FULL` (0.08), so the ring's `KGRID_FAR_MUL * fade` is
at full strength above about 4.6 degrees. Between there and roughly 9 degrees the
near lattice is reach-clamped into a small patch and the ring is the only thing
drawing across the rest of the view. The fade was doing its job; the band it
protects simply ends below where this starts. Two gates, on the ring only, ortho
only: `KGRID_FAR_MIN_SIN` = 2x `KGRID_EDGE_FULL` (stacked, so no angle
half-applies both) and `KGRID_FAR_MIN_LINES` = 6. The axes stay exempt from every
fade, by design.

### Item 5 — the fillet seam is not bezier sag, and the sign proves it

Both candidates were worked out on paper first.

**Sag is the wrong sign and the wrong place.** A patch column triple is a
QUADRATIC bezier and `ArcPoint` puts the odd handle at the tangent intersection
`r/cos(a/2)` — stock Radiant's own cylinder construction. Projecting the resulting
mid-span point back onto the mid-angle direction gives

```
|B(0.5)| = r (1 + cos^2(a/2)) / (2 cos(a/2))   >  r      (equality only at a = 0)
```

so the curve **bulges outward**, never inward, and it does so at MID-SPAN, not at
the sides. Widening by a sagitta would have made every span's middle worse to fix
an error that is not there.

**The rails are exact in the ideal, and that is why three rounds found nothing.**
Column 0 sits at `A + r*n1` with `A = mid - n*(r/k)`, so its offset from the edge
is `r(n1 - n/k)`, whose dot with `n1` is `-r(k/k) + r = 0` (**on face 0's plane**)
and whose dot with `n` is `-r/k + r*k = -r(1-k^2)/k = -ChamferDepth` (**on the
chamfer plane**). The seam is closed by construction.

**What is not ideal is the BRUSH.** `KiwiBevel_AppendFace` lays three planepts
down and the rebuild re-derives the plane from them (and snaps them when the
preference says to), so the face that actually exists is a few thousandths from
the plane the radius was solved against — a hairline, on **both** rails. `RowEnds`
had already conceded the principle for the other axis, reading the real winding to
decide how far along the edge the patch runs while the cross-section kept trusting
the ideal. `RefitRails` applies the same rule to the cross-section: each rail is
the point on the intersection LINE of the **actual** chamfer plane and the
**actual** adjacent plane nearest the ideal one (a 2x2 Gram solve), and the radius
is re-solved from it through

```
|rail - mid| = r * |n1 - n/k| = r * sqrt(1 - k^2) / k
```

(the middle term expands to `1 - 2(n1.n)/k + 1/k^2 = 1/k^2 - 1`). The two sides
are solved independently and averaged; the two rail COLUMNS are then written
exactly, so the seam closes to float precision even when the sides disagree. A
refit more than 5% from the dragged radius is refused — that is not a rebuild
artefact.

**And the caps follow the same profile.** `CrossSection` is now the single rule
both the arc patch and the two end caps read, and `LandCaps` projects onto the
refitted plane. Refitting one and not the other would have closed the rail seam
and opened an end seam in its place, which is the same bug moved.

### The mid-round directive — "lmap" on every part of a fillet

USER DIRECTIVE, verbatim: *"Please set the texture alignment to 'lmap' when doing
a fillet curve (for all parts). It's the setting that makes it lineup perfect."*

The primitive the directive names is `Patch_Lightmap_Texturing_Sub` (0x4397B0),
file-static in pmesh.cpp. Its PUBLIC spelling — `Patch_Lightmap_Texturing`
(0x448110), what the Surface Inspector's "Lmap" button runs — walks
`selected_brushes` and opens its own `Undo_GeneralStart`/`Undo_End`, which inside
a command's commit would be a second undo record in one gesture. So it takes the
round-Q forwarder shape: `Patch_KiwiLmapAlign( patchMesh_t* )`, one call to the
ported sub on one patch, with the CALLER keeping the bracket. Nothing is
reimplemented, and a fillet patch and a patch the user presses "Lmap" on go
through identical code. Applied to all three parts — the arc patch and both end
caps — and LAST, after `Patch_KiwiFinishNewLike`, which is the same order a user
gets pressing Natural and then Lmap, with Lmap winning as the directive says.

### Item 7b — the Alt chord, and the latch

Windows delivers a key pressed with Alt as `WM_SYSKEYDOWN`. **Every** hotkey path
in this shell filtered on `WM_KEYDOWN` — `Radiant_PreTranslateMessage` at both its
tests and the frame WndProc's `ON_WM_KEYDOWN` arm — so an Alt chord reached
neither `Radiant_TryHotkey` nor `KiwiUX_KeyFunnel`, fell through to
`DefWindowProc`, and was consumed by the menu-bar mnemonic handler with the window
left in menu-activation mode. `WM_SYSKEYDOWN` is now routed through the same
`Radiant_TryHotkey`, whose modifier mask already comes from `GetKeyState`, so an
**unbound** Alt chord returns false and the native menu behaviour is untouched.
Consuming a matched chord means the pump calls neither `TranslateMessage` nor
`DispatchMessage`. **Consuming the chord is not sufficient on its own**: the Alt
key-UP is what `DefWindowProc` turns into "activate the menu bar", so the trailing
bare-Alt release is consumed with it — and only when a chord was actually taken,
so a bare Alt tap still opens the menu.

## Decision log — round AM

- **D-AM1 — WHEN THREE REPORTS SHARE ONE CALL, THEY ARE ONE BUG.** Four rounds
  looked at the region fill in isolation. Reading item 2's report as evidence
  rather than as a feature request is what turned an unfalsifiable shader guess
  into a measurement that was already written down in this tree.
- **D-AM2 — THE FLAT-COLOUR OVERRIDE IS THE LINES' OWN IMMUNITY, NOT A NEW IDEA.**
  `Ed_EmitLineBatch` has pushed `.w == 1` per colour run since the port's bring-up.
  The fills asked for a shading path the editor never sets up.
- **D-AM3 — D-AL4'S RISK CITATION WAS WRONG, AND CORRECTING IT IS PART OF THE
  DECISION.** Round AH's regression was a grid line-density bug, not an alpha one.
  The opaque-slab risk is still real and still unproven — it is simply not
  supported by the precedent that was cited for it.
- **D-AM4 — THE PROBE IS SCOPED BY WHAT THE ELEMENT HAS TO LOSE.** Opaque-by-design
  fills take the override unconditionally; exactly ONE translucent fill takes it as
  the experiment. A layer-wide flip on an unsettled question is how round AH
  happened.
- **D-AM5 — AN INSTRUMENT MUST MEASURE BEFORE THE THING IT IS MEASURING.**
  Triangles are counted from the submitted index list and both in-renderer drops
  are probed BEFORE the call. `R_Ed_CmdBufferHeadroom` exists because the buffer
  gate was the one that could not be read from the editor at all.
- **D-AM6 — THE STATE WITH NO INSTRUMENTATION IS THE STATE THAT HIDES.**
  `regions.empty()` returned silently for four rounds. Every other drop in this
  subsystem had a counter.
- **D-AM7 — A FACE'S SECOND CHANNEL IS ROUND AI'S RULING, NOT SHAKEOUT G'S
  REVERSAL.** Shakeout G banned a face reading like an EDGE; an edge accent is one
  segment at width 2, a face border is a closed loop at width 1, in a different
  batch. Round AI already established that a tint alone reads weaker than a
  selection no matter what the alpha says.
- **D-AM8 — A PATCH HAS NO FACES, SO IN FACE MODE THE PATCH IS THE SURFACE.** Not a
  fallback: it is the finest thing there is to name, and it is round AK's mode-5
  ruling applied to the one mode it did not reach. Prefab/model hits still refuse.
- **D-AM9 — A MODE THAT PICKS SOMETHING NEW SAYS SO.** The mode-3 tooltip
  enumerates its targets by the rule round AA set; adding a target without adding
  the line would leave the enumeration lying.
- **D-AM10 — AN ALT CHORD IS INTERCEPTED ONLY WHEN IT IS BOUND.** Unbound chords,
  bare modifiers, F10 and the system chords (Alt+F4/Space/Tab/Enter/Esc) all fall
  through by name. Round U's law is reproduced at the new site rather than assumed
  to still hold.
- **D-AM11 — CONSUMING A CHORD WITHOUT CONSUMING ITS ALT RELEASE FIXES HALF THE
  BUG.** The latch is on the key-UP. It is eaten only when a chord was taken, so
  accessibility survives.
- **D-AM12 — THE FRAME WNDPROC IS DELIBERATELY NOT GIVEN A WM_SYSKEYDOWN ARM.** The
  pump already covers every message the frame sees; the only messages that reach
  the WndProc without it are those in NESTED MODAL LOOPS, where a native menu owns
  the keyboard and stealing its Alt chords would be a new bug.
- **D-AM13 — GEOMETRY DERIVED FROM AN IDEAL MUST BE RE-MEASURED AGAINST WHAT WAS
  ACTUALLY BUILT.** `RowEnds` did this for the along-edge axis two rounds ago; the
  cross-section was the half nobody had converted. The refusal band keeps it a
  correction and not a silent reshape.
- **D-AM14 — ONE CROSS-SECTION RULE, OR THE SEAM JUST MOVES.** The arc and the caps
  were two copies of the same `ArcPoint` call. Refitting one would have closed the
  rails and opened the ends.
- **D-AM15 — "lmap" IS CALLED, NOT REIMPLEMENTED, AND THE CALLER KEEPS THE
  BRACKET.** The public `Patch_Lightmap_Texturing` walks `selected_brushes` and
  opens its own `Undo_GeneralStart`, which would put a second undo record inside
  one gesture. `Patch_KiwiLmapAlign` is the round-Q forwarder shape onto the ported
  per-patch primitive.

## 67. Round AO — nine items, and one of them was not a regression at all

Five new reports plus the four deferred out of round AN. The finding that shapes
the round is **item 3**: the boolean did not regress. Not one line of
`kiwi_boolean.cpp`'s carve has changed since round AA — round AF touched only its
two PREVIEW budgets, and rounds AL/AM/AN did not touch the file at all (the single
AL edit to `kiwi_split.cpp` was TRAP 4's fill normal). What the report found is an
older defect that a PYRAMID is simply the worst possible shape for. Decision log
D-AO1..D-AO16.

### Item 3 — the boolean: a sliver is not a refusal

**THE CHAIN, WALKED RATHER THAN GUESSED.** Every candidate the brief named was
checked against the actual diffs:

| Suspect | Verdict |
|---|---|
| AL's `OrientToEye` ortho arm + fill normals | **PREVIEW ONLY.** `kiwi_split.cpp`'s two hunks replace `-cam->vpn` with `KiwiTris_FillNormal` inside the two fill emitters. No plane, no winding, no epsilon. |
| AM's `kiwi_pick` granularity gate | **CANNOT REACH THE TOOL PICK.** The new arm is an `else if ( hb->patch )` on the tail of `Pick`, reached only when the mask has neither the FACE nor the OBJECT bit resolved. The boolean re-casts its own ray with `SEL_MASK_OBJECT` explicitly (kiwi_boolean.cpp:604), so the OBJECT branch wins and the new arm is unreachable from here. |
| AN's `kiwi_transform` snap arms | **NOT ON ANY BOOLEAN PATH.** The boolean never calls the transform layer. |
| `KiwiSplit_DefByPlane` / `KiwiBool_DifferenceByDef` (round AF) | the machinery is right, but see below |
| all-or-nothing across the gesture | **ALREADY PER-TARGET.** `DoDifference` refuses one target, counts it, and carries on to the next (kiwi_boolean.cpp's `++refused` arm). The design the brief asked to confirm is the design that is there. |

**SO WHAT IS ACTUALLY WRONG.** `KiwiSplit_DefByPlane` is *all-or-nothing*: a §19
failure on EITHER half frees BOTH and returns `false`, and `CarveByOneTool` turned
that single indivisible `false` into `KBOOL_REFUSED` for the **whole target**. But
the two halves of a subtract do not have the same standing:

* the **FRONT** half is a piece that would be LANDED. A sliver front — V3 "face
  collapsed", V4 "zero-area face" under `KVALID_MIN_FACE_AREA` = 0.1 square units,
  V6 "planes crossed" under `KVALID_MIN_THICKNESS` = 0.01 — is a fragment of
  essentially zero volume that no mapper asked for and none could select
  afterwards. The right answer is to **drop it**: the volume forfeited is smaller
  than the gate that rejected it, by definition.
* the **BACK** half is the REMAINDER, `target ∩ (inside planes 0..f)`, and the
  subtract DISCARDS it at the end by construction. The true intersection is a
  SUBSET of it, so a sliver remainder proves that what this tool would remove is
  itself below the gate — the two solids effectively **miss**. Leave the target
  alone, and do not consume the tool over a graze.

**AND A SQUARE PYRAMID IS THE WORST CASE FOR THE OLD RULE.** It has an apex where
four sloped planes meet at a point and four sloped edges where they meet in pairs,
so the outside slices an arch's many planes cut off it are wedges that taper to
nothing. ONE of them under 0.1 square units anywhere in the cascade threw away the
entire carve and printed *"Boolean: one brush left untouched — zero-area face"*.
Against a BOX target the same tool never produces a taper and the same boolean
"works" — which is exactly the shape-dependence the report describes, and exactly
why it reads as "it got worse" rather than as "this one shape".

**THE FIX IS A REPORTING CHANGE, NOT A LOOSENING.** `KiwiSplit_DefByPlaneParts`
reports the two halves separately (`KSPLIT_HALF_NONE` / `_OK` / `_SLIVER`), and
`KiwiSplit_DefByPlane` becomes a thin wrapper that reproduces its old contract
EXACTLY — including "on false NOTHING is allocated" and including which half's §19
reason wins — so the cut and split verbs are untouched. **§19 itself is not
relaxed: nothing that fails the gate is ever landed.** What changed is only what a
failure MEANS for the rest of the gesture. Drops are counted and reported.

**THE PATCH HALF OF THE REPORT IS REAL BUT SMALLER.** Round AM's pick change does
have a boolean consequence, just not the one suspected: a patch now ENTERS
`selected_brushes` on a face-mode click where before the click bounced, so a mapper
who rubber-bands "the arch" — and round AG's own patch-cylinder mode exists
*because* "a boolean'd cylinder arch comes out as a fan of sliver faces" — can be
holding patches. `Usable` drops every one of them (csg.cpp:572's ported validation
refuses patches: a patch is a render surface with no volume) and did so in complete
silence. `Begin` now counts and names them, and the tool-pick click says *"that is
a curve/patch"* instead of the generic *"not a usable solid"*, which was true of a
patch, a fixed-size entity and empty space alike.

### Item 1 — the loft: the correspondence was measured in the wrong plane

**THE REFUSAL FOR PERPENDICULAR FACES, NAMED.** Step 4 compared the two rings
after "the axial component is projected out", i.e. both rings flattened along ONE
direction, the straight axis `d = normalize(cB - cA)`. That is the right
comparison when the faces are roughly PARALLEL and facing each other. **It is the
wrong comparison the moment they are not.** Ring A lies in a plane at angle α to
`d` and ring B in a plane at α on the OTHER side, so projecting both along `d`
squashes each ring by cos α — *along two different in-plane directions, 2α apart.*
At the pictured 90°, A's square end face projects to a rectangle squashed one way
and B's projects to a rectangle squashed the ORTHOGONAL way, and the cyclic offset
minimising the sum of squares is being chosen between two shapes whose aspect
ratios are inverted. It is routinely ONE VERTEX OUT — a 90° twist — and a
90°-twisted square bridge has side quads that cross, which §19 rejects as "planes
crossed" / "face collapsed".

**THE TWIST IS DECIDED BEFORE ANY STATION MATH RUNS.** That is precisely why the
report says it "will not work no matter what": neither RULED nor TANGENT nor any
density can rescue a wrong correspondence.

**AND THE SECOND HALF OF THE REPORT FALLS OUT OF THE SAME PLACE.** Even with the
right correspondence, blending corresponding vertices in WORLD space between two
non-parallel planes shrinks the section: for the pictured wall, A's section
spanning ±w along X and B's spanning ±w along Y, the midpoint ring spans ±w/2 along
each — a width of w·√2/2 ≈ 0.71 w. That is "the curve needs to be the same
thickness", exactly.

**THE CURE FOR BOTH IS ONE CHANGE: A ROTATION-MINIMISING FRAME.** Stop comparing
and blending in world space; do both in a frame carried from A to B with no twist
about the section normal.

```
frame at A:   (nA, uA, vA),  uA deterministic,  vA = nA × uA
transport:    R = minimal rotation nA → nB, i.e. Rodrigues about
              w = normalize(nA × nB) through θ = acos(nA·nB):
                  R x = x cosθ + (w × x) sinθ + w (w·x)(1 − cosθ)
              uB = R uA,  vB = nB × uB
```

A minimal rotation has **no component about the normal itself**, which is what
makes the transported frame twist-free — that IS the rotation-minimising frame's
defining property, evaluated in closed form here because this path has exactly two
ends rather than a sampled spine.

* **CORRESPONDENCE** is then the cyclic offset minimising the squared distance
  between A's (x,y) in (uA,vA) and B's in (uB,vB). No squash, no aspect inversion.
* **STATIONS** are built the same way: at parameter t the frame is R evaluated at
  t·θ, the origin is the path point (straight for RULED, the cubic Hermite of the
  CENTROIDS for TANGENT/CURVE), and the ring is the 2D blend of A's and B's
  coordinates mapped THROUGH that frame. Two consequences, both of them the
  report's asks: **congruent profiles stay congruent at every station** (the wall
  keeps its thickness), and **every station is planar by construction**, so step
  6's projection is exact rather than an approximation and cannot introduce a
  twist of its own.
* **THE PARALLEL CASE IS ALGEBRAICALLY UNCHANGED.** With nA = nB the rotation is
  the identity, the frames coincide, and
  `c(t) + lerp(xa,xb)·u + lerp(ya,yb)·v  =  lerp(a, b)` — the old world-space lerp,
  term for term. Every loft that already worked builds the same brushes.

**THE ANTIPODAL CASE IS REFUSED IN WORDS**, not guessed: two sections exactly back
to back after the sense rewind have no defined bridge direction, and any answer
would be an arbitrary twist.

**THE PREVIEW NOW RUNS THE COMMIT.** *"dont make the preview blue until it will
actually work (misleading! and a time waster!)"*. Steps 7 and 8 — build every
segment, gate it — ran only inside `Apply`, so the preview was blue whenever
STATIONS existed. `Rebuild` now performs the whole build as a DRY RUN (the
"rejection is free" property kiwi_extrude.h states and `KiwiBool_WouldCarve`
already relies on), frees every def, and colours the bridge BAD-red unless the
commit would succeed. **And it previews what will ACTUALLY be built**: Apply's
TANGENT→RULED retry is run in the dry pass too, so when the retry is what will
happen the stations left behind are the RULED ones, the preview shows the straight
bridge, and the HUD says `RULED (tangent refused)` BEFORE the click rather than the
console saying it after. `Apply` no longer re-derives anything — it refuses with
the same words the HUD has been showing, which is the only way the two can stay in
step.

**THE TWIST STEPPER** (`[` / `]`, and a panel row) steps the correspondence by
whole vertices on top of the computed offset. The automatic answer is now measured
in the right frame, but a profile with rotational symmetry has several offsets of
near-equal cost, and when the refusal IS a correspondence this rescues it in one
keypress.

**THE THIRD BRIDGE MODE: CURVE.** RULED and TANGENT build BRUSHES and are
untouched. CURVE builds q3 BEZIER PATCHES over the same stations — every
corresponding profile EDGE sweeps to one patch strip, so a rectangular wall
section becomes four strips (two large sides plus top and bottom): the tube, with
both ends left open because the source faces are already there.

* **CONTROL GRID.** `width = 2·spans + 1` columns along the path, `height = 3` rows
  across the edge. EVEN columns sit on the stations; ODD columns are the **arc
  handle** for that span — the meeting point of the two stations' path tangents,
  which is kiwi_patchfillet.cpp's own `ArcPoint` rule (`r/cos(a/2)` at a known
  circle) generalised to a path whose curvature is not known in closed form. The
  tangent at station s for rail i is the central difference of the neighbouring
  stations' rail points; the meeting point is the closest approach of the two
  tangent LINES, a 2×2 least squares. Parallel tangents (vanishing determinant),
  a meeting point BEHIND the span (negative parameter), or a handle further than
  `KLOFT_HANDLE_MAX`·chord from the midpoint all fall back to the chord midpoint —
  which is exactly what a straight span wants. The three ROWS are the edge's two
  rails and their exact midpoint, which is exact for a straight edge.
* **THICKNESS IS FREE.** Both sheets of the wall are the same profile swept from
  the same stations, so they stay parallel by construction. There is no separate
  offset surface to keep in step, which is the whole reason the patch answer is
  better than a brush answer for this shape.
* **TEXTURES.** Each strip inherits from the face of brush A ADJACENT to the
  profile edge it continues — the wall's own side, top or bottom — resolved
  geometrically (the face whose plane contains both of the edge's endpoints).
  Alignment is `Patch_KiwiCapAlign`, round AN's directive for the fillet's SWEPT
  surface, because a loft strip is the fillet arc's shape and not a flat end cap;
  the whole landing sequence is kiwi_patchfillet.cpp:1441-1559 in order, not a
  second spelling of it.
* **SPAN CAP.** `2·spans + 1` must stay inside the format's control grid
  (`Patch_GenericMesh` refuses a width outside 3..15, pmesh.cpp:1550), so CURVE
  clamps the density to 7 and the HUD says so. `MaxSegs()` is mode-dependent; the
  brush modes keep round AN's 128.
* **NO COLLISION**, and the standard caulk hint prints on every creation.

### Item 2 — the fillets follow the surface

**THERE IS NO STORED LINK AND THERE IS NOT GOING TO BE ONE.** A fillet lands three
INDEPENDENT patch entities (the arc and two end caps) and records nothing about the
brush it was cut into — `filletUnit_t` is a gesture-lifetime struct that dies with
the command. Two designs were possible:

* a **PERSISTED id** (a sidecar line, or a spare field on the patch) is only as
  good as the round trip. A .map is a text file that other tools and other Radiants
  write; a brush ordinal or a minted id survives neither an edit outside KIWI nor a
  re-order, and a STALE id is worse than none because it names the wrong brush with
  full confidence.
* the **GEOMETRIC** association is re-derived from the geometry actually in front
  of the editor, every time, so it works identically on a map just loaded, one
  hand-edited outside KIWI, and one built five minutes ago. **A fillet's own
  construction is what makes it decidable**: round AM's `RefitRails` put the two
  rail columns exactly on the chamfer plane and on the two adjacent planes, so "does
  this patch sit on the face that just moved" is a plane test plus a
  point-in-winding test, not a guess.

Geometric wins, and the load-a-saved-map case is the reason it wins rather than a
caveat on it.

**THE ONE RULE.** A control point that lies ON the moved face's plane AND INSIDE
that face's winding moves with the plane; any interior row between two end rows is
then re-interpolated so `KPF_PATCH_ROWS`' "both ends and the exact midpoint" grid
stays true. That single rule covers both gestures the report is about:

* pushing the wall's END face lengthens the filleted edge — the arc patch's far
  ROW is on that plane, so it translates and **the arc stretches**; the end CAP
  patch lies wholly on that plane, so it translates whole and stays sealed.
* an interior row that did not move while an end row did would leave the patch
  straight but non-uniformly parameterised — a texture stretch on the very surface
  the round-AM/AN alignment work exists to make line up. Hence the re-interpolation.

**AND IT REFUSES, LOUDLY, THE CASE IT CANNOT DO HONESTLY.** When the on-plane
control points do NOT form complete rows, the moved plane is cutting the patch's
CROSS-SECTION rather than its length — that is the chamfer-plane push, and carrying
it correctly means re-solving the RADIUS against the new chamfer plane (round AM's
`RefitRails`), which cannot be done from a landed patch without reconstructing the
gesture's `filletUnit_t`. Moving the rails without re-solving the radius would shear
the arc and **re-open the very seam round AM closed**. So the fillet is left alone
and the console says to re-run the bevel.

**THE UNDO IS THE CALLER'S**, and each patch is covered with `Undo_AddBrush` inside
the already-open record — kiwi_boolean.cpp's tool-cover rule at a different site,
for the identical reason (the bracket head cloned `selected_brushes`, and a patch
this gesture touches is not on that list, so nothing cloned it). The post-mutation
bookkeeping is `Patch_Rebuild( p, 1 )`, the ported tail
`Patch_UpdateSelected_0` (0x43D800) runs and the one kiwi_transform.cpp:2823
already calls — **not** `bDirty`, which means "this patch carries an explicit sample
size" and gates `size` in `Patch_Write` (pmesh.cpp:1146/:1418).

### Item 4 — the Y paint: the marquee arm ate every left button

Established by reading the dispatch. The shell offers every camera press to
`KiwiVP_CameraButtonDown` FIRST (imgui_shell.cpp:407) and runs the legacy handler
only when it returns false (:412). **The `KiwiBox_Begin` arm at the bottom of that
block claimed every LMB press as `KG_MARQUEE`, unconditionally, with no test of Alt
at all**, so `CamWnd_OnLButtonDown` was never called and with it the whole ported
paint chain: `CamWnd_OnLButtonDown` → `CamWnd_DropModelsToPlane` (camwnd.cpp:3334)
→ `Drag_Begin` (drag.cpp:571), whose `LABEL_34` arm at drag.cpp:650 IS the
terrain-paint start (Alt held, buttons 1 or 2, mode sel_brush/sel_addpoint,
`sub_401D50()`) → `Patch_Paint_Start` (pmesh.cpp:5994). **The ring the user sees is
drawn from the SAME `sub_401D50` gate** (camwnd.cpp:2988) — which is exactly why it
appeared while the stroke did not: the drawing path was reachable and the input path
was not.

Everything else was checked and cleared by name. Round AM's `WM_SYSKEYDOWN` routing
only ever looks at `WM_SYSKEYDOWN`/`WM_SYSKEYUP`, and **there is no `WM_SYS*BUTTON*`
message in Win32** — an Alt+click is a plain `WM_LBUTTONDOWN` — so a mouse gesture
cannot enter that path at all, the same reason its own header already gives for
Alt+MMB. `io.WantCaptureMouse` gates the legacy hidden child windows, not this
dispatch. Alt+RMB is mouselook, latched in the RMB arm, and this arm is LMB-only.

**THE ROUTING IS A DECLINE, NOT A NEW GESTURE.** Returning false is the whole fix:
the shell then runs the ported `CamWnd_OnLButtonDown`, and because no modern gesture
started, the REST of the stroke follows by itself (`KiwiVP_CameraMouseMove` and
`KiwiVP_CameraButtonUp` both return false while `s_gesture == KG_NONE`). No
synthetic press is fabricated anywhere, and **the undo bracket is the ported one** —
`Patch_Paint_Start` opens `Undo_GeneralStart( "patch painting" )` and
`Drag_MouseUp`'s `sel_addpoint` arm closes it exactly once.

The RMB "lower" half is **not** bound, and the panel now says so instead of
promising it: mode 0's amount is `grid_sizes[gridsize] * sign * 0.5` and `sign`
comes ONLY from the button, so no panel control can invert it. Smooth is the way
down. A literal `Alt+LMB — Paint terrain` chip appears only while the tool is armed.

### Item 5 — Auto Bool tries harder, and volume is the guard

Three phases now, each reported separately: the existing pairwise fixed point, then
a **CLUSTER** phase, then a **REVERSE** pairwise sweep. The cluster phase builds the
touching graph of what is left (union-find), and hands each connected component of
three or more to `Brush_MergeList` **whole**.

**THE VOLUME CHECK IS NOT OPTIONAL AND IT IS WHY THE PHASE IS SAFE.**
`Brush_MergeList` misclassifies INNER faces across non-touching flipped-equal
planes, i.e. it will happily return a merged brush that swallows empty space. The
guard is total-volume conservation computed from the windings by the divergence
theorem — Σ over faces of (winding centroid · face normal) × winding area / 3,
about a common reference point — before and after, accepted only within a relative
epsilon. A rejected component is restored and the run carries on.

**AND THE REVERSE SWEEP IS THERE BECAUSE `MergeList`'S PAIRWISE RESULT IS
ORDER-DEPENDENT.** Two brushes the forward sweep refused can merge when they are
offered in the other order, so the second sweep is a cheap rescue rather than a
duplicate of the first.

### Item 6 — Match Face accepts a patch as the source

*"the cap needs to be adjustable, I would use the (Z) match face command on the
curve itself."* Match Face's source stage demanded exactly ONE brush FACE; it now
also accepts a PATCH. The patch's own plane is fitted over its control grid, and
the control points are projected onto the target face's plane **along that
normal** — which for a planar cap is exact, not an approximation. Non-planar
patches are REFUSED with a message naming the reason, as are a degenerate patch
normal and a patch whose normal is near-perpendicular to the target plane (where
the projection is unbounded). One undo record, the patch-edit precedent's bracket,
and the preview draws the projected control net before the click.

### Item 7 — hidden solids persist in the sidecar

*"Hidden solids are not respected on save and unhide on load."* The hidden bit
lives on the `selbrush_t` INSTANCE (brushFlags bit 2 plus a hide depth), so it can
never be a .map field — the .map has no instances. It goes in the KIWI2 sidecar as
optional `hiddenbrush N` lines, N being the ordinal in **Map_SaveFile's own brush
walk**, matched to that walk exactly rather than to a plausible re-derivation of it.
No version bump: a sidecar with no such lines behaves exactly as before, and an
older reader skips them.

**THE APPLY MUST HAPPEN AT THE TAIL OF THE MAP LOAD, NOT IN THE SIDECAR READER**,
because the instances the bit lives on only exist once the load has finished
building them. Ordinals out of range are ignored rather than erroring.
**THE FRAGILITY IS REAL AND IS STATED**: an ordinal association breaks if the map
is edited outside KIWI or if the save and load orders ever diverge. A wrong hide is
harmless and Unhide All fixes it, which is the whole reason an ordinal is an
acceptable key here and would not be for, say, item 2's fillet association.

## Decision log — round AO

- **D-AO1 — "IT GOT WORSE" IS A HYPOTHESIS, NOT A FINDING.** Every suspect the
  brief named was checked against the actual diffs before any code was written, and
  all four were cleared. The boolean's carve has not changed since round AA. Saying
  so plainly is worth more than shipping a fix aimed at a regression that is not
  there — and it is what turned the search toward the shape (a pyramid) instead of
  toward the calendar.
- **D-AO2 — THE TWO HALVES OF A SUBTRACT DO NOT HAVE THE SAME STANDING.** The front
  half is a piece that will be landed; the back half is discarded by construction.
  One all-or-nothing `false` for both is what made a sliver anywhere in a cascade
  destroy the whole carve.
- **D-AO3 — §19 IS NOT LOOSENED, ONLY RE-READ.** Nothing that fails the gate is
  ever landed. What changed is what a failure MEANS for the rest of the gesture, and
  the volume forfeited by dropping a sliver is smaller than the gate that rejected
  it, by definition.
- **D-AO4 — THE OLD ENTRY POINT KEEPS ITS EXACT CONTRACT.** `KiwiSplit_DefByPlane`
  is now a wrapper that reproduces all-or-nothing including which half's reason
  wins, so cut and split are untouched. A second splitter with "nearly the same"
  semantics is the duplicate-function drift this codebase keeps paying for.
- **D-AO5 — A SKIPPED OPERAND THAT SAYS NOTHING IS INDISTINGUISHABLE FROM A BROKEN
  COMMAND.** Round AM made patches selectable in face mode; `Usable` dropped them in
  silence. Counting and naming them is most of the "more robust" the report asked
  for, without changing one line of geometry.
- **D-AO6 — A CORRESPONDENCE MEASURED IN THE WRONG PLANE CANNOT BE RESCUED
  DOWNSTREAM.** The loft's twist was decided before any station math ran, which is
  the whole content of "this type of loft will not work no matter what". Fixing the
  station math would have fixed nothing.
- **D-AO7 — THE FRAME IS THE FIX FOR BOTH HALVES OF ITEM 1's REPORT.** The same
  rotation-minimising transport that un-squashes the comparison also makes the swept
  section congruent, which is "the same thickness". Two symptoms, one cause, one
  change — and the parallel case reduces to the old code algebraically, so nothing
  that worked can break.
- **D-AO8 — AN UNDEFINED ANSWER IS REFUSED IN WORDS.** Antipodal sections have no
  bridge direction. Picking one arbitrarily is how a tool earns "it doesn't work in
  a lot of cases".
- **D-AO9 — A PREVIEW THAT DOES NOT RUN THE COMMIT IS A GUESS.** The dry run costs
  nothing to reject, which is a property this tree already relies on twice
  (kiwi_extrude.h, `KiwiBool_WouldCarve`). And the FALLBACK is previewed too:
  showing a curve and building a straight bridge is the same lie one step later.
- **D-AO10 — `Apply` NO LONGER DECIDES ANYTHING.** It refuses with the words the
  HUD has been showing. A commit that can reach a verdict the preview did not show
  is the bug item 1(a) is about, reintroduced.
- **D-AO11 — G2 NEEDS A CURVED MEDIUM, AND THE EDITOR HAS ONE.** Round AF recorded
  a patch loft as the honest future work rather than half-shipping it. CURVE is that
  work, built on the fillet's own patch construction and landing sequence rather
  than a second one.
- **D-AO12 — THE THICKNESS COMES FROM THE SWEEP, NOT FROM AN OFFSET SURFACE.** Both
  sheets of the wall are the same profile at the same stations. An offset-surface
  design would have needed to keep two surfaces in step and would have drifted at
  exactly the bend the user asked for.
- **D-AO13 — THE FILLET ASSOCIATION IS GEOMETRIC BECAUSE OF THE RELOAD, NOT IN
  SPITE OF IT.** A stored id is only as good as the round trip through a text file
  other tools write, and a stale id names the wrong brush confidently. The fillet's
  own construction puts its rails on the brush's planes, so the geometry is the
  index.
- **D-AO14 — THE CHAMFER-PLANE PUSH IS REFUSED RATHER THAN APPROXIMATED.** Moving
  the rails without re-solving the radius would shear the arc and re-open the seam
  round AM closed. A loud refusal that names the fix is worth more than a silent
  wrong shape.
- **D-AO15 — ROUTING AN INPUT MEANS DECLINING IT, NOT SYNTHESISING IT.** Item 4's
  whole fix is one gate returning false so the ported chain runs — press, drag and
  release included, with the ported undo bracket. Fabricating a press would have
  been a second input model to keep in step.
- **D-AO16 — AN ORDINAL IS AN ACCEPTABLE KEY ONLY WHERE BEING WRONG IS HARMLESS.**
  Item 7 uses one and says why (a wrong hide is undone by Unhide All); item 2
  refuses one for the same question, because a wrongly-associated fillet silently
  deforms geometry. The same mechanism, two verdicts, on the cost of being wrong.

## 68. Round AP — two reports, and one of them is a withdrawal

The user built through round AO (e765821) and sent two items. They are unrelated to
each other and unrelated to AO; one asks for a shipped feature to be **taken out**,
and one is a live-drag oscillation that turned out to be the last self-snap in the
editor.

### Item 1 — the extrude does not get to run a boolean on its own any more

USER DIRECTIVE, verbatim: *"when extruding, dont automatically bool diff the solid.
It can be done by the user with a boolean after."*

**WHAT WAS THERE.** Round AF, item 2 gave the REGION extrude a second verb.
`KiwiExtrudeRegionCommand::Recompute` ran `DragsIntoSolid()` every frame — a
`KiwiBool_PointInSolid` probe one `KEXT_MIN_DIST` along the drag direction from the
profile centroid — and when it answered yes, `Commit()` forked to `Carve()`: the
same convex prism pieces the grow path builds, dry-run through
`KiwiBool_WouldCarve`, then handed one at a time to `KiwiBool_DifferenceByDef`
inside one `"carve region"` bracket, and freed. The preview turned amber and the HUD
said `CARVE (cavity)` for the whole drag, so it was announced — but announced is not
asked for.

**WHY THE TRIGGER WAS THE PROBLEM, in the user's hands.** AF chose a POINT PROBE
rather than the sign of the distance, deliberately, so that a negative extrude in
open space would still grow a body downward. The cost of that choice is that the
SAME gesture — same key, same drag, same direction — means "make a brush" or
"subtract from every brush I touch" depending on geometry the user is not looking at
while they drag. A verb that changes identity on a hidden test is a verb the user
cannot predict, and the report is what that feels like after using it once.

**WHAT SHIPS.** The probe, the fork, the amber preview colour, the `CARVE (cavity)`
HUD rung, the `(drag INTO a solid to hollow a cavity instead)` sentence in the entry
message, `DragsIntoSolid()`, `Carve()` and the `kiwi_boolean.h` include are all gone
from `kiwi_extrude.cpp`. An extrude LANDS ITS PRISM, in both directions, and is
allowed to interpenetrate whatever is there — overlapping brushes are legal in a
`.map`. Carving is Q: one keypress, explicit, previewed, with its own undo record,
and it reaches the same `CarveTarget` subtract — so the geometry a user gets by
extruding and then pressing Q is the geometry the auto-carve produced.

**WHAT IS DELIBERATELY NOT TOUCHED — and this was checked, not assumed.** Round Q's
negative-E semantics on the FACE command (`KEXTF_GROW` / `KEXTF_CARVE` /
`KEXTF_DESTROY`) share the word "carve" and nothing else. That arm calls
`KiwiXform_PushFaceOnce`: it moves ONE face plane of ONE brush — the identical edit
the lollipop performs by hand — it never touches a bystander, and its trigger is the
SIGN OF THE DRAG, which the user is steering directly and can read in the HUD before
the confirm. It fails none of the three things the report is about. It stays, and
the reasoning is written into `kiwi_extrude.h` next to the round-Q table so the next
round does not have to re-derive it.

`kiwi_boolean.h` keeps `KiwiBool_DifferenceByDef`, `KiwiBool_WouldCarve` and
`KiwiBool_PointInSolid`. The honest statement, recorded there, is that all three now
have no in-tree caller — Q's interactive command carves with a brush the user
CLICKED and goes through `CarveTarget` directly, so the by-def trio existed for the
extrude and only for it. They are kept as the file's stated public surface (and two
sibling subsystems cite them by name as the definition of the dry-run and of the
on-plane tolerance), not resurrected as a hook.

### Item 2 — the line set that teleports between the pivot and the arrow

USER REPORT, verbatim: *"when dragging a line set using the move gizmo, it teleports
back and forth from the pivot point to the arrow. Fix this so the pivot point takes
control in all cases and doesn't try to fight and teleport."*

**THE MECHANISM, named precisely: THE CONSTRUCTION MOVE WAS SNAPPING TO ITSELF.**

A transform must never snap the geometry it is dragging to itself. `kiwi_transform.cpp`
says exactly that, in one line, and enforces it with
`PickFlags() -> PICKF_EXCLUDE_SELECTED`. That flag is forwarded by `KiwiCmd_MouseMove`
into `KiwiSnap_Query`, which forwards it to every `Pick()` call it makes — and
`Pick()` is how the BRUSH candidates are gathered. **Construction candidates are not
gathered through `Pick()`**: `kiwi_construct.h` scope ruling 1 deliberately keeps
construction geometry out of the pick set, so `kiwi_snap.cpp` walks the store itself
in five places (anchors, segment closest points, segment midpoints, segment-segment
intersections, the relative-angle chain). None of those five walks looked at
`pickFlags` — they could not; the flag says nothing about construction. So a
CONSTRUCTION move was the one gesture in the editor whose own geometry was still a
candidate for its own snap.

The geometry-snap arm is ABSOLUTE by design (rounds L and P): `total = snapPos - m_ref`,
i.e. "drag the reference point onto that target". Feed it a point of the very object
set it is moving and it solves

```
snapPos    = base + total_old                  (the anchor, as already drawn)
total_new  = snapPos - m_ref
           = total_old + ( base - m_ref )
```

— it adds the anchor's fixed offset from the reference point **every frame**. The set
jumps that far, which takes the anchor out of the 8 px query radius; the next frame
the arm misses and `total` falls back to the cursor mapping
(`m_lockBase + Constrain(d)`), which puts the set back under the arrow; the anchor
re-enters the radius; the jump happens again. That two-state limit cycle at frame
rate IS the report, and its two states are exactly the two the user named: "the
reference point is under the cursor" and "some anchor is under the cursor" — the
pivot, and the arrow.

**Everything else the brief asked to audit was checked and was already correct**,
which is worth recording because it is what narrowed the search:

- (a) `m_ref` for a construction-only selection is **latched**, once, in `Begin` from
  `KiwiConSel_MoveBegin`'s anchor centroid, and never written again. It is not
  re-derived per frame from the live selection.
- (b) the construction apply path is already **baseline-absolute**:
  `KiwiConSel_MoveApply( m_total )` writes `base + delta` over the `s_moveBase`
  snapshot taken at Begin, for every world point and for the plane origin. It never
  compounds, so it composes correctly with the absolute snap arms and with round AN's
  carried-base rule — the off-axis components sitting in `total` are carried travel,
  and applying the FULL total from the baseline is exactly what they need.
- (c) round AN did not create this. AN's change was *which axes a snap may write*;
  the self-snap loop is older than AN and fires identically under AN's predecessor
  (which wrote all three axes from the same absolute answer).

**THE FIX: an exclusion, not a tolerance.** `kiwi_extrude.cpp` solves the same class
of problem with `KEXT_SELF_SNAP_BAND` — refuse an answer within a band of scalar zero
— and that works there because the extrude's SOURCE cannot move. Here the whole
object set is travelling and its offsets from the reference point are unbounded, so a
band is not expressible. The correct rule is the one the brush arms already have:
while a move owns them, these objects are not candidates at all.

`KiwiConSel_SnapMuted( objectIndex )` (`kiwi_conselect.h`/`.cpp`) answers "is a live
move gesture dragging this store object", straight off `s_moveBase` — the same list
`MoveApply` writes through, so there is no second definition of what the gesture owns
to drift from. `kiwi_snap.cpp`'s five construction walks now share ONE gate,
`ConCandidateUsable( o, i )`, which folds the round-U hidden test and the round-AP
mute together; five copies of a two-part predicate becoming one copy of a three-part
predicate is the point of factoring it. The mute is strictly per-gesture:
construction geometry is a first-class snap target again the instant the drag ends.

**One exception, and it is the same exception `PickFlags` already makes.** While the
pivot is being PLACED, `KiwiXformBase::PickFlags` returns `PICKF_NONE` — "the points
they will reach for FIRST are corners of the very solid the gesture is about"
(shakeout G). `ConCandidateUsable` mirrors it through `KiwiXform_PivotPlacing()`. It
is safe as well as symmetric: a live placement is consumed by `TrackPivot` and never
reaches `Recompute`, so no geometry moves and there is no loop to close.

**AND THE PIVOT TAKES CONTROL — the other half of the directive.** The construction
arm of `KiwiMoveCommand::Begin` returns before the shakeout-G line
`m_pivotOverridden = PivotActive( m_ref )`, so it was the one Move arm that ignored a
placed pivot outright: `m_ref` stayed the anchor centroid, the gizmo drew on the
centroid, and the absolute snap arm dragged the CENTROID onto the target. V only did
anything if pressed mid-gesture (`ApplyPivot` does work on this arm). The arm now
asks the same question the brush arms ask, at the same point in the sequence — after
the natural anchor is written, before `LatchMapStart` — so the pivot IS the latched
reference for the whole gesture and the natural centroid is still what a pivot-less
move uses. `PivotActive` self-expires on a selection change and its signature already
counted construction items, so nothing new had to be hooked.

The construction `Commit` branch also returns before round L's `PivotRide`, so the
ride is duplicated into it, written the same way and in the same order (before
`Reset`, which forgets `m_total`). Without it a pivot placed for a construction move
stayed at the position the lines used to occupy and the next G started with the
anchor detached from the geometry — the same complaint, one gesture later.

## Decision log — round AP

- **D-AP1 — A SHIPPED FEATURE CAN BE WITHDRAWN, AND THE RECORD SAYS SO.** Round AF's
  cave was built to a verbatim directive and worked. It is removed to a later
  verbatim directive. The AF material is marked SUPERSEDED rather than deleted, in
  this document and in `kiwi_boolean.h` and `kiwi_extrude.cpp`, because "why is there
  no cave" is a question the next round will ask.
- **D-AP2 — THE COMPLAINT IS ABOUT THE TRIGGER, NOT ABOUT THE BOOLEAN.** What the
  user objects to is a gesture that silently changes verb on a hidden probe. That is
  why round Q's face un-extrude survives untouched: same word, different mechanism,
  and its trigger is the sign of the drag the user is steering.
- **D-AP3 — "VERIFY WHETHER THE COMPLAINT REACHES IT" IS A REAL QUESTION WITH A REAL
  ANSWER.** Three tests were applied to the face path — is it a boolean, does it
  touch bystanders, is its trigger visible — and it fails none of them. The answer is
  written next to the round-Q table so it is not re-litigated.
- **D-AP4 — DEAD PUBLIC SURFACE IS KEPT AND LABELLED, NOT SILENTLY LEFT.** All three
  by-def boolean entry points now have no caller. Saying that in the header is worth
  more than either deleting them (they are the documented way for a future tool to
  cut with scaffolding it built) or leaving the stale "ROUND AF uses it" sentence
  that would send the next reader hunting for a caller that is gone.
- **D-AP5 — A FEEDBACK LOOP IS FOUND BY WRITING THE RECURRENCE DOWN.**
  `total_new = total_old + (base - m_ref)` is the whole bug. Everything the brief
  suspected — a per-frame `m_ref`, an incremental apply, a pivot fighting the arrow —
  was checked against the code and cleared, and the cleared list is recorded because
  it is what left only one candidate.
- **D-AP6 — AN EXCLUSION, NOT A TOLERANCE.** The extrude's self-snap band works
  because its source is stationary. A moving set has no bound on its own offsets, so
  the only correct answer is "not a candidate while the gesture owns it" — which is
  what the brush arms have had since the beginning.
- **D-AP7 — ONE GATE FOR FIVE COPIES.** The construction walks had the same two-part
  predicate written out five times. Round AP needed a third part in all five.
  Factoring first and adding second is the only version of that change that cannot be
  applied to four places out of five.
- **D-AP8 — THE EXCEPTION IS COPIED, NOT INVENTED.** Pivot placement lifts the
  self-snap exclusion because `PickFlags` already lifts it, for a reason that is
  written down. Two exclusions with different exception rules is how a user learns
  that snapping "sometimes" works.
- **D-AP9 — "IN ALL CASES" MEANS THE ARM ASKS THE SAME QUESTION, NOT A SECOND
  MECHANISM.** The construction arm gets `PivotActive( m_ref )` and `PivotRide` — the
  exact lines the brush arms run, at the exact points they run them. A
  construction-specific pivot path would be a second answer to a question that
  already has one.

## 69. Round AQ — eight reports, a crash, and the end of a four-round theory

The user built through 52027e0 (round AP) and sent eight items. One is a hard crash
with a stack, one is the fifth round of "the light blue face does not show", and the
rest are a mix of long-standing ergonomics gaps and one boolean robustness report.

### Item 1 — the crash: `PMESH_19_Radius`, and what the 1280 was

USER STACK, verbatim: `PMESH_19_Radius(...) Line 5046` <- `DrawAdvancedTerrainEditCircle
Line 2133` <- `CamWnd_Draw` -> `CamWnd_RenderToRT` -> `ImGuiShell_RenderViewportsToRT`.
Exception: read access violation, **`turnRow` was `0x60CF436`**. The user was making a
**patch cylinder** (round AG's `P` mode) with the advanced terrain-edit `Y` ring live.
The user's own note: *"hardcoded 1280 in this function needs to go."*

**WHAT THE 1280 WAS.** It is `sizeof(drawVert_t[16])` — **one row of the 16x16 CONTROL
grid** `patchMesh_t::ctrl[16][16]` at `patchMesh_t+0x38`. `drawVert_t` is 80 bytes;
`turned_edge` sits at `+0x4C` inside it, so `0x38 + 0x4C = 132` is `ctrl[0][0].turned_edge`
and the binary's flag walk is `*(patch + 132 + 80*row + 1280*col)`. Both strides were
faithful. **The bug is not the constant, it is what the loop feeds it.**

`PMESH_19_Radius` iterates the **TESSELLATED** mesh — `cdef->width`/`cdef->height` of
the `curvePatchDef_t` — and then indexes the **CONTROL** grid with those same indices.
That is only in bounds while the tessellated dims are <= 16, which is true for a terrain
SHEET (stored unsubdivided, which is why the editor's own terrain paint never hit this)
and for nothing else. A patch cylinder tessellates to far more than 16 columns, so at
`col >= 16` the walk left `ctrl` entirely — `1280*col` is past the end of the whole
20 556-byte `patchMesh_t` — and read whatever was there. `turnRow` was a garbage pointer
because it was pointing at unmapped heap.

**THE FIX, in two places.** `pmesh.cpp` indexes the grid as the typed array it is
(`patch->ctrl[col][row].turned_edge`) and consults it **only for a terrain patch whose
control indices are in range**; anything else takes the untorn diagonal. That is not an
invention — `Patch_Fill_BuildFrontIndices` (0x43FB70) applies exactly this rule to the
same flag, so the ring now clips against the same triangulation the patch is drawn with.
`camwnd.cpp` then adds the honest half: the terrain-paint overlay is **refused outright
on non-terrain patches**, with one console line per run, because a cylinder is not a
terrain sheet and the paint modes cannot edit it anyway.

### Item 2 — the region fill: the decisive round

USER REPORT, verbatim: *"You seriously need to fix the light blue construction plane
visibility!"* Fifth round. No console diagnostic has ever come back, so this round stops
waiting for one.

**THE CONTROL EXPERIMENT WAS IN THE TREE ALL ALONG.** The **boolean's red operand
preview** (`kiwi_boolean.cpp` `FillBrush`, :449) is a translucent triangle fill that the
user has confirmed visible across many sessions. Round AM claimed the two emitters were
byte-identical; re-diffed against the CURRENT tree, they were not — AM's own probe had
introduced a difference. The full table:

| aspect | boolean fill (VISIBLE) | region fill (before AQ) | after AQ |
|---|---|---|---|
| material / technique | `d_white`, `TECHNIQUE_UNLIT` | same | same |
| MATERIAL_COLOR | **neutral {0,0,0,0}**, white after | **flat {r,g,b,1} per region** (AM's TRAP 5 probe) | **neutral** — copied |
| vertex colour | packed rgba, alpha **0.22** | packed rgba, alpha **0.22** | same |
| fill normal | face plane normal | constant +Z (`KiwiTris_FillNormal`) | constant +Z — kept, see D-AQ3 |
| eye orient | `KiwiTris_OrientToEye` | same | same |
| depth nudge | none | `KREG_FILL_NUDGE` 0.5 toward eye | kept (coplanarity, unrelated) |
| **draw-pass location** | **`KiwiCmd_DrawWorld`** (the command-overlay slot) | `KiwiCon_DrawWorld` | **relocated to the command-overlay slot** |

**WHAT THE AM PROBE ACTUALLY RETURNED.** TRAP 5 predicted two outcomes for the flat
override: the fill appears (model confirmed) or it becomes an opaque slab (model
confirmed the other way). It returned a **third**: nothing at all. That falsifies the
"~0.32x neutral bracket" model outright — and the boolean carries a visible fill at the
same 0.22 alpha **on the neutral bracket**, which is the same conclusion from the other
side. So the flat override is withdrawn and the pass-level neutral bracket (the ported
selected-face fill's own) now covers every region.

**THE RELOCATION.** `KiwiRegion_DrawFills( -1 )` no longer runs from
`KiwiCon_DrawWorld`. It runs from `camwnd.cpp`'s Cam_Draw tail, immediately before
`KiwiCmd_DrawWorld` — the slot the boolean's preview draws from — inside its own
`KiwiLines_Begin`/`KiwiLines_Flush` pair so that even the "submitted while a line batch
is open" condition matches. Same relative order to the selected-outline depth clear and
its `R_SortMaterials`; same bracket discipline. The gate is unchanged: the new site
re-checks `KiwiCon_ShowConstruction()`. Round AM's diagnostic is kept intact.

**THIS IS A FALSIFIABLE SUBMISSION.** If the fill appears, pass location was the bug and
that is a fact worth having about this renderer. If it does not, there are now **zero**
differences between a fill that is visible and one that is not, the whole
submission-side model is dead, and the next round starts at the renderer — at
`RB_DrawTriangles_Internal`'s blend state — instead of at this layer.

### Item 3 — the terrain-paint ring was offset from the cursor

The ring built its own ray: `CameraCalcRayDir( cph - cpy, dir, cpx )` plus
`camera.origin`. `CameraCalcRayDir` is deliberately byte-identical to the binary and is
therefore **perspective-only**, while KIWI's camera is **orthographic by default**
(`kiwi_camera.cpp` `KCAM_ORTHO_ENTRY`, "default ON"; `CamWnd_SetupScene` really does swap
the projection matrix). Under a parallel projection the ray ORIGIN moves per pixel and
the direction is constant `vpn`, so an eye-diverging ray lands displaced by the full
parallax — **zero at the image centre, growing toward the edges**, which is the reported
symptom exactly. The flip was also one pixel short (`cph - cpy` where every other site in
the tree uses `height - y - 1`).

Both halves are already solved once, in the canonical modern picker every other
subsystem uses, and it reads the same `ImGuiShell_CameraPaintCursor` mousespace — so the
site now calls `Pick_RayFromImagePos( cpx, cpy, &ray )` and hands `ray.dir` / `ray.origin`
to `sub_43DD50`. `RADIANT_KNOWN_ISSUES` listed this under *"Ortho: two CLASSIC-profile
paths still build perspective rays"*; the terrain-ring half is now closed. The 3D marquee
quad and `Camera_GetRectSelection3D` are the other half and are untouched.

### Item 4 — math and units in numeric entry

USER DIRECTIVE, verbatim: *"allow basic math operations when typing units. Also allow
y/yd/i/in/f/ft for unit specifier. So I can do ex: 12 * 12 (default inch). OR 12ft * 10
(120ft) OR 10ft6in ALT 10f6i OR 120 + 120 (240 inches). Also allow fractions like 1/8."*

The entry layer's parser was one `atof` (`kiwi_numeric.cpp`). It is now a recursive
descent evaluator over the same buffer:

```
expr     := term { ('+' | '-') term }
term     := factor { ('*' | '/') factor }
factor   := ['-' | '+'] factor | '(' expr ')' | compound
compound := number [unit] { number [unit] }
unit     := y|yd|yds|yard|yards | f|ft|feet|foot | i|in|ins|inch|inches
```

**Precedence is STANDARD, not left-to-right** (D-AQ6). Parentheses are supported because
they were nearly free once the grammar existed. **Fractions need no special case**: `1/8`
is one divided by eight = 0.125 in, and `10ft/2` is 120 divided by 2 = 60 in = 5 ft — one
operator, both readings, no mode. Everything resolves to INCHES and the single conversion
to world units stays exactly where it was, in `KiwiNum_ValueWorldField`'s
`Units_FromDisplay`, so the "kind is a display fact" rule is untouched.

**THE KEY ALPHABET.** Numpad `+ - * /` and OEM `/`, `-`, `=`->`+` are unshifted; `*`, `(`
and `)` come from a narrow Shift+`8`/`9`/`0` arm added ABOVE the "any modifier is not
mine" gate. Unit letters `y d i n f t` are taken **only when the field is a LENGTH and
already has text**, which is what keeps every tool hotkey alive: with an empty field
those letters still fall straight through to the command and the binding table.

**INVALID INPUT IS NEVER GARBAGE.** A half-typed expression simply does not evaluate, and
every caller already treats "no value" as "use the cursor" — so a preview never sees a
partial number. The bubble marks it `(incomplete)`, and a complete expression echoes its
result (`10ft6in = 10 ft 6 in`). The evaluator is published as `KiwiNum_EvalDisplay` and
the tree's only other typed-number box — the viewcube's grid-spacing popup, which had the
tree's second `atof` — now goes through it, so "it is one parser" is true rather than
aspirational.

### Item 5 — trim deletes stray whole lines

USER REPORT, verbatim: *"Allow the trim tool to delete whole lines (some become stray
lines and it's easy to just mash click with T on)."*

Round T removed exactly this, and it was right to: its zero-crossing fallback turned a
**missed** crossing into a delete, which is the worst possible answer to "I could not
find the boundary". The distinction round T needed is preserved and is what makes this
safe — an **incomplete** scan (the `KTRIM_MAX_PAIRS` cap) still refuses before the
zero-crossing case is ever reached, so a missed crossing still cannot become a delete.
What is left after that refusal is a chain that genuinely has no crossings: a stray. It
now highlights **whole** and a click deletes it, one `KiwiCon_UndoPush` per click so
mashing over a field of leftovers is undoable one leftover at a time. A closed loop that
nothing crosses is a stray too; a loop with **exactly one** crossing is not, and keeps
round AF's "a loop needs two crossings" explanation.

### Item 6 — circle sides inaccurate until Tab

USER REPORT, verbatim: *"when drawing a circle, the number of sides is inaccurate until
it's tab-navigated to."* Round AG fixed the FIELD and round AF seeded `m_sidesOverride`
at `Begin()` from the remembered `RoundToolSides` — both correct, and neither reached the
**preview**, which builds a throwaway `kconObject_t` and never assigned its `segs`. It
therefore kept the struct default `0`, which `CircleSegsFor` reads as AUTO (radius-driven).
The field, the HUD and the object placed on click all already said `m_sidesOverride`; only
the rubber band disagreed, and it snapped to the stated count at commit — *"inaccurate
until it's tab-navigated to"*, exactly. Fixed in the circle, the 2-point circle and the
arc. Two adjacent instances of the same class were fixed with it: `KiwiArcTool::ToolSides`
was missing the `CardinalSegs` rounding its own geometry applies (field said 30, mesh used
32), and the **n-gon** never re-derived `m_sides` from a typed count until the tool was
restarted, so its Tab field was inert for the whole gesture.

### Item 7 — boolean: arch into box

USER REPORT, verbatim: *"I'm having trouble dragging arches into boxes. This needs to
work. The boolean tool needs to be more robust. (see pic)."*

**AN EXTRUDED ARCH IS NOT ONE TOOL.** A concave region extrudes as a convex decomposition,
so an arch arrives at the carve as a fan of many thin wedges whose tops are near-tangent
to each other and to the box face. Under the old rule, **any one** of those wedges hitting
a hard refusal — a degenerate cut plane, or the ported core producing neither half —
abandoned the ENTIRE difference and left the box whole. That is the report: the arch
"does nothing", and it does nothing more often the more segments the arch has.

Round AO made this argument for **slivers** and deliberately kept hard refusals
all-or-nothing. Round AQ extends it to the hard refusal, and it is safe for the same
reason: a refusal means `CarveByOneTool` **changed nothing and freed everything it made**,
so keeping the piece is identical to that tool having MISSED it. Section 19 is not
relaxed — nothing invalid is landed; the hole is incomplete where a plane could not be
applied, and the console says which tool, which piece and which gate.

**THE PER-REFUSAL REPORTING** is the other half of the deliverable, and it is what the
next report will be diagnosed from:

- `CarveByOneTool` now names which of the two MISS verdicts fired and at which plane —
  *"no shared volume — tool plane 7 leaves the target entirely outside it"* versus *"the
  shared volume collapsed below the validity gate at tool plane 7 (a near-tangent cut:
  zero-area face)"*. Those look identical from outside and have opposite fixes.
- The bounds early-out names itself.
- Every skipped cut prints tool index, piece index and gate, capped at 8 lines per
  operation with a tally line after.
- `KBOOL_MISS` at target level now prints its reason instead of the old bare *"did not
  meet the tool(s)"*, on both the interactive and the by-def carve paths.
- The dry run (`KiwiBool_WouldCarve`, asked every hovered frame) is silenced.

### Item 8 — Enter advances a stage, it does not early-commit one

USER REPORT, verbatim: *"When creating an object (like a cylinder, but keep ALL in mind),
I tab, type in 120, then press enter. This should confirm step1 of the creation
operation, not submit it early with 0 z height (thus invalidating it)."*

Enter reached `KiwiCmd_KeyDown`'s rung 5 and called `KiwiCmd_Commit` unconditionally, and
a staged creation tool's `Commit()` refuses outright below its final stage. So typing a
radius and pressing Enter **destroyed** the gesture — the most natural way to say "yes,
that number" guaranteed nothing would be built. Only the mouse could advance a stage.

The rule is now enforced in exactly one place, rung 5:

- Enter with a **pending typed value** calls `KiwiEditorCommand::AdvanceStage()`; a
  command that has a stage left folds the value in, advances, rebases the next stage's
  mapping and stays live.
- Enter with **no typed value** means confirm, byte for byte as before.
- Enter with typed text that **does not evaluate** (item 4 makes that a real state) is
  swallowed with a console line — it advances nothing and commits nothing, so a
  half-typed number can never early-commit either.
- A second Enter always confirms, because the first cleared the field on its way through.

Per-tool audit of every multi-stage machine:

| tool | stages | Enter before AQ | Enter after AQ |
|---|---|---|---|
| Box / Centre Box | 3 (corner, size, height) | commit -> *"not enough points"* | stage 0->1, 1->2 advance; height stage commits |
| Cylinder / Cone | 3 (centre, radius, height) | same | same |
| Sphere | 2 (`FinalStage()==1`) | commit -> refused at stage 0 | stage 0->1 advances; stage 1 commits |
| Construct Arc | 3 (centre, radius+a0, sweep) | `Finish()` at zero sweep | stage 0->1, 1->2 advance; sweep commits |
| Rect / Centre Rect / Circle / Circle-2pt / N-gon | 2 | commit — **already correct** (the typed value is applied by `Recompute` before `Finish`) | unchanged |
| Polyline / Spline | N | intercepted at rung 3 (`WantsEnterFinish`) | unchanged — rung 3 still wins |
| Cut / Loft / Boolean | staged | intercepted at rung 3 with a veto message | unchanged |
| Everything single-stage | 1 | commit | unchanged (`AdvanceStage` defaults to false) |

Because `KiwiCmd_Confirm` deliberately routes RMB through `KiwiCmd_KeyDown(0x0D)`, the
options panel's Confirm button and the RMB confirm inherit the rule for free.

## Decision log — round AQ

- **D-AQ1 — A FAITHFUL STRIDE IS NOT A CORRECT INDEX.** The 1280 in `PMESH_19_Radius` was
  right; the loop bounds handed to it were the tessellated dims where the flag lives on
  the control grid. Deleting the constant would have hidden the defect. The fix is to
  index the typed array and to gate on the patch KIND, which is the rule the binary's own
  `Patch_Fill_BuildFrontIndices` already applies to the same flag.
- **D-AQ2 — A TOOL THAT CANNOT ACT ON A THING SHOULD SAY SO, NOT DRAW ON IT.** The
  terrain ring is refused on non-terrain patches with one console line rather than made
  merely crash-safe. Crash-safety was necessary; silence would have left "why does the Y
  circle ignore my cylinder" as the next report.
- **D-AQ3 — COPY THE CONTROL, DO NOT PROBE AGAIN.** Four rounds of single-suspect fixes
  on the region fill have missed. The boolean's fill is confirmed visible and every
  difference between them is now removed, including the pass slot. The one deliberate
  remaining difference is the constant +Z fill normal, which is kept because it is the
  layer's stated law and because a constant normal cannot be *worse* than the arbitrary
  per-face one the visible fill uses.
- **D-AQ4 — A PROBE'S THIRD OUTCOME IS DATA.** TRAP 5 predicted "visible" or "opaque
  slab". It returned "nothing", which falsifies the 0.32x model rather than leaving it
  unproven. The AM material is marked SUPERSEDED in place rather than deleted, because
  "why is the region fill not on the flat override like the gizmo heads" is the next
  reader's question.
- **D-AQ5 — A PORTED HELPER STAYS PORTED; THE KIWI CALLER CHANGES.** The terrain ring's
  ortho fix does not touch `CameraCalcRayDir`. It moves the call site onto
  `Pick_RayFromImagePos`, which is where round M put the ortho pair, so there is still
  exactly one copy of that math.
- **D-AQ6 — STANDARD PRECEDENCE, AND SAY WHY.** `120 + 120 * 2` is 360. A mapper who
  wants the other reading has parentheses; a mapper who assumes left-to-right has no way
  to ask for precedence at all. Every calculator, spreadsheet and CAD entry field the
  user has used behaves this way.
- **D-AQ7 — A LETTER IS ONLY A UNIT WHEN IT CAN CONTINUE A NUMBER.** The numeric layer
  sits above the command in the key ladder, so an unconditional letter grab would shadow
  tool hotkeys. Gating on "the field is a LENGTH and already has text" makes the two
  meanings unambiguous without a mode.
- **D-AQ8 — ROUND T'S RULE IS PRESERVED, NOT REVERSED.** Trim deletes a stray, and still
  refuses when the crossing scan was TRUNCATED. "No crossings were found" and "no
  crossings exist" are different facts and the code already distinguished them; item 5
  is only about acting differently on the second.
- **D-AQ9 — DISPLAYED MUST EQUAL TESSELLATED AT EVERY MOMENT.** Not at commit, not after
  a Tab. Three separate side-count divergences are fixed under one rule rather than one
  report's worth.
- **D-AQ10 — ROBUSTNESS IS PARTIAL SUCCESS PLUS A LOUD ACCOUNT.** A boolean that carves
  nine of ten planes and says which one it skipped is strictly better than one that
  discards all ten silently. The ownership argument that makes it safe is the same one
  round AO used for slivers: a refusal mutates nothing.
- **D-AQ11 — ONE RUNG OWNS THE ENTER RULE.** `AdvanceStage` is consulted in exactly one
  place, so RMB confirm and the options-panel button inherit it, and no tool can grow a
  second, subtly different answer to "what does Enter mean here".

## 70. Round AR — two items: the construction clipboard, and the join differential

The user built through 96e593b (round AQ) and sent two items. One is a feature gap
(construction geometry has no Copy/Paste); the other is the sharpest boolean report
of the whole overhaul — **the same outline, one JOIN apart, carves or does not** —
and it turned out to name a defect that had nothing to do with the region layer.

### Item 1 — Copy / Paste for construction geometry, and the Move that follows

USER DIRECTIVE, verbatim: *"allow copy pasting of construction lines! after a paste,
it should automatically go into G(move) mode."*

**WHAT EXISTED.** The brush clipboard is entirely ported — Ctrl+C is classic id 33039
(`mainfrm.cpp:1191`'s binding -> `Cmd_OnEditCopybrush` -> `XYWnd_CopyClip`), Ctrl+V is
33040 (-> `Cmd_OnEditPastebrush` -> `XYWnd_PasteClip` -> `RadiantClipboard_Paste` ->
`Map_ImportBuffer`) — and shakeout G already hung `KiwiCmd_AfterPaste` on the paste id
so pasted solids land selected and in a **paused** Move. Construction geometry reached
none of it: the store is KIWI's own and never enters `selected_brushes`, so Ctrl+C over
a set of lines copied nothing and Ctrl+V pasted nothing.

**THE STORAGE, and why not the OS clipboard.** The brush path *does* use the OS
clipboard, so precedent would permit it. It is still the wrong home, for a reason that
is not taste: **a mixed copy needs both payloads at once.** The two selections are
PARALLEL (`kiwi_conselect.h`) and both may be non-empty, so one OS clipboard slot means
the second writer stomps the first and a mixed Ctrl+C silently loses one half. So
`kiwi_conclip.cpp` keeps a process-local `std::vector<kconObject_t>` of **values**, not
store indices — indices shift on every `KiwiCon_RemoveAt`, so an index clipboard would
paste the wrong lines after a delete. Accepted cost, stated: construction geometry does
not travel between two running copies of the editor. Brushes still do.

**THE TWO CLIPBOARDS ARE KEPT HONEST.** Separate stores can disagree: copy lines, then
copy a brush, and Ctrl+V would paste the brush *and* the older lines. The rule is the
obvious one — *a Copy that took solids and no lines means the clipboard holds solids
only* — so the construction half is cleared when the ported Copy had something to take
(`selected_brushes` non-empty, which is exactly what `XYWnd_CopyClip` serialises). A
Copy with **nothing** selected still leaves both alone, which is `KiwiCmd_ClipCut`'s
own rule for the same reason.

**WHAT A PASTED OBJECT IS.** A value copy, in place (no offset — the brush paste does
not offset either, and the Move is how you place it), with exactly two fields reset:
`hidden` -> false (a paste is new scaffolding, and scaffolding you cannot see is a paste
that appears to have failed) and `group` -> -1 (round W's group id is a handle into the
store's own table; copying it would file a paste into a folder the user was not looking
at). Type, points, `closed`, the parametric block, `segs` and `name` carry verbatim, and
`KiwiCon_Add` then normalises and refits exactly as it does for a drawn object — so a
pasted object is indistinguishable from one drawn there, and persists in the sidecar
with no special casing. **One `KiwiCon_UndoPush` for the whole paste**, before the first
Add.

**THE MOVE NEEDED NO NEW MACHINERY.** `KiwiXform_CanMove` already answers true for a
pure construction selection (`kiwi_transform.cpp:3565` -> `KiwiConSel_CanMove`) and the
Move command already has its construction arm (`kiwi_transform.cpp:1146`), so the
existing `KiwiCmd_AfterPaste` hook starts the same paused Move for either kind. The only
thing that function had to learn is that "nothing landed" now has a second half.

**THE MIXED CASE IS REFUSED, LOUDLY — and it is a framework fact, not a preference.**
`KiwiConSel_CanMove()` requires the brush-side selection to be EMPTY and Move reaches
its construction arm only when `DominantKind` fails, so ONE gesture structurally cannot
carry both; starting it would move the solids and leave the lines at the paste position.
Both halves are still pasted and both are still selected. Only the auto-enter is
withheld, with a line saying which two counts landed and that G is waiting.

**THE CLONE TRAP, and the one-shot latch.** `KiwiCmd_AfterPaste` is also the tail on
CLONE (33001), which never touches this clipboard. Asking "is anything selected
construction-side" would make a **stale** line selection turn every Clone into the mixed
refusal. The question actually being asked is "did THIS paste land lines", and only the
paste knows it — hence `KiwiConClip_TakeJustPasted`, read once and cleared.

### Item 2 — THE JOINED-POLYLINE BOOLEAN DIFFERENTIAL

USER REPORT, verbatim: *"why does this bool diff fail? see the 2 pics. The 2nd pic fails
while the 1st pic works. Why? The only different is I joined the polyline on the 2nd
one. When i bool it into the pyramid it fails!"* Three screenshots: an arch profile
(rectangle plus a pointed arch of ~12 arc segments) as separate lines and arcs, which
extrudes and diffs into a pyramid correctly; the same outline after JOIN, which does
not; and the white extruded arch sitting inside an uncarved pyramid.

#### (a) The region derivation was NOT the difference — and here is the proof

The obvious hypothesis is that a joined closed polyline takes PASS 1 (the closed-object
fast path) while an unjoined outline takes PASS 2 (the chain) or PASS 3 (the
arrangement), and that PASS 1 hands the extruder a dirtier ring. **Read out, it does
not.** All three passes funnel through `AcceptLoop` (`kiwi_region.cpp` PASS 1's
`if ( AcceptLoop( r ) )`, PASS 2's, PASS 3's and PASS 3b's), and `AcceptLoop` was
already running the dedupe **and** `DropCollinear` on every one of them since round R.
The closing vertex is likewise removed on both routes: `DedupLoop`'s wrap check on the
region side, `NormalizePoints`' seam rule (`kiwi_construct.cpp`, the `o.closed && m >= 2`
block) on the store side. So a "the joined path skips the cleaning" story is simply
false, and this round does not tell it.

What that convention lacked was any **enforcement** — three passes that happen to call
one function, with the cleaning written out inline inside it where a fourth pass could
acquire one step and not the other. It is a function now: `KiwiRegion_SanitizeRing`
(exported, `kiwi_region.h`), and it gains a step that neither pass had —

    1. dedupe at the weld, bounded by the ring's finest edge (round AG);
    2. DropCollinear at section 19's own KVALID_PLANE_DOT (round R);
    3. dedupe AGAIN, because step 2 can bring two survivors within the weld.

Step 3 is exactly what a near-tangent arc/line junction leaves behind, and it was
reachable from every pass. Winding stays out of it: `RewindCCW` is an acceptance
decision and belongs with the gates.

#### (b) The defect JOIN really did carry: a weld that disagreed with the walk

`KiwiConSel_Join` merges the chain that `KiwiRegion_ChainWalk` found. The walker welds
two ends into ONE NODE at `KiwiRegion_WeldDist()` — grid-scaled since round AF,
`clamp( grid * 0.25, 0.5, 16 )` (`kiwi_region.cpp`'s `NodeFor` call sites, and the
self-closing test just above them). **The concatenation that follows used the bare
`KREG_JOIN_DIST` floor of 0.5**, and so did the closing-vertex drop under it
(`kiwi_conselect.cpp`, both sites). At any grid of 4 or coarser — i.e. every grid anyone
maps at — the walker therefore declares two ends IDENTICAL and the join then stores
**both of them**:

* a **stub edge** at every junction, up to 16 world units long, and
* a **duplicate closing vertex**, because the ring's last point is within the weld of
  its first but further than 0.5 from it.

Neither is a shape the user drew. Both are precisely the input section 23's prism
builder turns into two nearly-identical side planes (section 19's V5, "duplicate
plane") and the boolean then turns into a near-tangent cut plane. **The unjoined
outline never acquires them** — nothing bakes the walker's weld into the store, and the
loop the region layer stitches is cleaned by `AcceptLoop` and that is the end of it.
That is a join changing geometry, which it must never do, and it is the one ring-level
asymmetry the two pictures could carry. Join now welds at
`KiwiRegion_WeldFor( finest edge )` — bit for bit what `AcceptLoop`'s own dedupe uses on
the same points.

**The store's `NormalizePoints` is deliberately NOT changed with it.** It runs on every
`KiwiCon_Add`, including a polyline the user drew point by point, and its hard 0.5 is
right there: it must never eat a point the user placed. The difference in Join is that
the walker has already ruled, on the record, that those two points are one.

#### (c) ...and the reason a DIFF can fail COMPLETELY: section 19 on a thing never landed

Rounds AO and AQ both walked past this. `KiwiSplit_DefByPlaneParts` applies the **full**
section-19 gate to BOTH halves of a split, and `CarveByOneTool` then reads a refused
BACK half as `KBOOL_MISS` — "the two solids do not intersect" — and abandons the whole
difference. But the back half of a subtract step is the **running intersection**,
`target INTERSECT (inside tool planes 0..f)`, and `kiwi_boolean.cpp`'s own contract says
it is DISCARDED at the end of the loop and never landed for even one frame.

Section 19 exists to keep invalid brushes out of the **map**. Half of its checks are
statements about a brush's presentation rather than its volume: V3 rejects a face whose
winding was clipped away, V4 a face under 0.1 square units, V5 two planes that became
coincident. A running intersection against a **many-planed** tool collects all three as
a matter of course, *with a completely healthy volume* — and an extruded arch is a
fifteen-plus-plane prism whose side planes are near-tangent neighbours. So: cut a
pyramid with one, and at some plane the intermediate trips a cosmetic gate; with a
single tool and a single target that verdict leaves `owned` false, `CarveTarget` returns
MISS, and the whole difference is a **silent no-op** — the pyramid uncarved with the
white tool sitting in it, which is picture three exactly.

`KiwiSplit_DefByPlaneCarve` is the subtract's own entry point. It differs from the Parts
entry in one place: a back half that fails section 19 is **handed back** instead of
freed, when and only when

* every bounds extent is at least `KSPLIT_CARRY_EXTENT` (1.0 world unit) — anything
  thinner is round AO's genuine graze and still stops the cascade; and
* its bounds sit **inside the input's**, one unit of slack. A half-space cut of a solid
  is a SUBSET of it, so this containment invariant is free — and it is what refuses the
  two verdicts that mean "this is not a bounded solid at all", V7 (span past the map
  bound) and V8 (the half-spaces no longer enclose a finite cell). Only presentation
  verdicts on a real subset can ever be carried.

**WHAT IS NOT RELAXED: section 19 on anything landed.** The FRONT halves — the pieces
that become brushes — are gated exactly as before, and a front sliver is still dropped.
The carry is counted and printed per plane (capped at 8 per operation) and tallied in
both commit paths, so a partial hole is never silent.

#### What this round can and cannot claim

The console has still never spoken from the failing case. What is claimed is: (b) is a
provable ring-level asymmetry between joined and unjoined outlines at any real grid, and
(c) is a provable mechanism by which a single many-planed tool produces a **total**
no-op while a decomposed one does not. Both are fixed at the source. Whether (b) or (c)
is what the two pictures show is decided by the next paste of the console — AQ's
per-refusal lines plus this round's carry lines now name the plane and the gate.

## Decision log — round AR

- **D-AR1 — TWO CLIPBOARDS, BECAUSE A MIXED COPY NEEDS TWO PAYLOADS.** The OS clipboard
  has exactly one slot for this editor's format, and the brush and construction
  selections are parallel and can both be non-empty. One store would silently lose a
  half. The cost — construction geometry does not travel between editor instances — is
  smaller than the cost of a Ctrl+C that drops what you selected.
- **D-AR2 — A COPY DEFINES THE WHOLE CLIPBOARD, NOT ITS OWN HALF.** Copying solids
  clears the line half. Without that rule the two stores drift apart and a later paste
  produces a mixture the user never assembled. A Copy with *nothing* selected still
  changes nothing, which is the existing Cut rule.
- **D-AR3 — A PASTE IS NEW SCAFFOLDING.** Pasted construction objects are visible and
  ungrouped. Carrying `hidden` would make a paste look like a failure; carrying `group`
  would file it somewhere the user was not looking.
- **D-AR4 — REFUSE THE MIXED MOVE, DO NOT HALF-DO IT.** One Move gesture structurally
  carries one kind (`KiwiConSel_CanMove` requires an empty brush selection). Starting it
  on the solids would leave the lines behind at the paste position, which is a worse
  answer than a console line telling the user to press G on one kind.
- **D-AR5 — ASK "DID THIS PASTE LAND LINES", NOT "ARE LINES SELECTED".** `AfterPaste` is
  the tail on Clone as well as Paste, and a stale construction selection must not turn
  every Clone into the mixed refusal. A one-shot latch is the only spelling of the
  question that is actually being asked.
- **D-AR6 — THE INVARIANT IS A FUNCTION, NOT A CONVENTION.** Joined and unjoined
  outlines derived the same ring only because all three passes happened to call
  `AcceptLoop`. `KiwiRegion_SanitizeRing` makes it a thing the code states, and the
  second dedupe pass it gains is a shape no pass removed before.
- **D-AR7 — A JOIN MUST WELD AT THE TOLERANCE THAT AUTHORISED IT.** The walker matched
  the ends at the grid-scaled weld; the merge dropped duplicates at the 0.5 floor. Any
  gap between those two numbers is stored geometry the user never drew. This is the
  round's one honest "join changed the shape" finding.
- **D-AR8 — SECTION 19 IS A GATE ON WHAT GETS LANDED.** The running intersection of a
  subtract is never a brush. Holding it to the map's landing gate refused an
  intermediate for how it would look if it were one, and a single such verdict turned a
  whole difference into a no-op. The front halves — the pieces that ARE landed — are
  gated exactly as before.
- **D-AR9 — CARRY ONLY A PROVABLE SUBSET.** The containment test (the back's bounds
  inside the input's) is not belt-and-braces: it is what distinguishes a presentation
  verdict on a real subset from V7/V8, which mean the thing is not a bounded solid.
  Without it the carry could propagate an unbounded intermediate through the cascade.
- **D-AR10 — A PARTIAL ANSWER IS ANNOUNCED, ALWAYS.** Round AQ's rule, applied to the
  new case: every carried intermediate prints its plane and its gate, capped, with a
  tally on both commit paths.

## 71. Round AT — three items: a gate that refused everything, a plane with an invisible height, and a curve that was a polyline

The user built through ff04342 (round AR) and sent three items. One is a
regression from round AQ's own crash fix; one is the drawing plane, reported from
two directions at once; and one is the loft's CURVE mode, which shipped in round
AO with the right control grid and the wrong tangents.

### Item 1 — the cyan terrain ring stopped rendering

USER REPORT, verbatim: *"the cyan circle for the advanced patch editor no longer
renders."*

**WHAT ROUND AQ ADDED, AND WHAT IT ACTUALLY ASKED.** AQ item 1 was a hard crash:
`PMESH_19_Radius` walked the tessellated mesh and indexed the 16x16 CONTROL grid
with those indices, so a patch cylinder read a garbage `turned_edge` pointer. The
fix had two halves. The first — a typed, index-checked `ctrl[col][row]` access
that falls back to the untorn diagonal whenever the tessellated index leaves the
control grid (`pmesh.cpp:5068-5075`) — is the one that stops the crash, and it is
untouched. The second was a predicate in `camwnd.cpp`:

```
if ( (int)def->type == PATCH_TERRAIN )   // 0x40
    return true;
… otherwise skip this patch, with one console line.
```

**`PATCH_TERRAIN` IS NOT "THIS IS A TERRAIN SHEET".** It is a bit set by exactly
three creation paths: `Patch_ParseMesh`'s `"mesh"` / `patchTerrainDef3` branch
(`pmesh.cpp:1002`), `Create_Terrain`, i.e. the Terrain dialog (`pmesh.cpp:1810`),
and the curve-to-terrain conversion (`pmesh.cpp:9311`). **Everything else has type
0**: `Patch_GenericMesh`'s "simple patch mesh" sets `p->type = 0` outright
(`pmesh.cpp:1702`), and so does every patch KIWI's own verbs build — the loft's
CURVE strips, the patch fillet, anything cloned or pasted from those. So the gate
answered *false* for a perfectly ordinary flat patch sheet, which is what a mapper
paints on, and the ring stopped rendering for everyone. (`mapinfo.cpp:78` tests
the same field bitwise, `type & 0x40`; even that spelling refuses a type-0 sheet,
so this was not a `==` versus `&` slip.)

**AND THE GATE WAS NARROWER THAN THE VERB IT DRAWS.** The paint stroke itself —
`sub_43E4B0` -> `PMESH_16` (`pmesh.cpp:5404` / `:5351`) — tests only
`if ( i->patch )` and edits the control points of ANY patch. A cursor ring that
refuses what the brush will paint is a lie about the tool's reach, in the other
direction.

**THE GATE IS NOW ABOUT MEANING, BECAUSE THE MEMORY SAFETY LIVES ELSEWHERE.**
Three conditions, each tied to a real read the overlay performs:

* the control grid must be walkable — `PMESH_20_Radius_2` (`pmesh.cpp:5158`)
  iterates `ctrl[0..width)[0..height)` directly, so `width`/`height` must be in
  [2,16];
* there must be a tessellated mesh to clip the rings against (`curveDef` +
  `verts`, at least 2x2);
* **closed / wrapping families are refused** — cylinder, cone, hemisphere
  (`Patch_BrushToMesh`, `pmesh.cpp:1293-1436`). A cylinder wraps a full 360
  degrees and a cone or hemisphere closes to a point, so there is no single
  sheet-like surface for a flat XY cursor ring to sit on and the tessellated
  columns wrap far past the control grid. That is the case round AQ was written
  for — its crash was a patch CYLINDER — and it keeps its one console line per
  editor run.

**BEVEL and ENDCAP are deliberately NOT in that list**, though AQ's console line
named them. A bevel is a quarter turn and an endcap a half turn: both are OPEN
sheets that a ring sits on perfectly well. And decisively, KIWI's own swept
surfaces stamp `PATCH_BEVEL` on themselves — the loft's CURVE strips
(`kiwi_loft.cpp`) and the patch fillet's arc (`kiwi_patchfillet.cpp`) — so
refusing that bit would refuse the geometry the user had just built with this
editor's own verbs.

Sheets pass whatever their `type`: 0, `PATCH_TERRAIN`, `PATCH_TRIANGLE`,
`PATCH_BEVEL`, `PATCH_ENDCAP`.

### Item 2 — the drawing plane: tilted in open space, and floating on TOP

USER REPORT, verbatim: *"when clicking like this (see pic). The lines should just
hit the nearest major plane? Why dont they? Fix it. The construction plane here
should be flat. Not only that, when I lock the camera to TOP, it doesn't draw them
on the major plane either. IT draws them slightly above it?? (pic2)"*

Two pictures, two defects, and they are not the same defect.

#### (a) The tilted plane: both face rungs answer a question nobody asked

The ladder is round Y's, five rungs, and rung 4 — the VIEW-DOMINANT major plane,
axis-aligned by construction — is the one that produces the flat plane the user is
asking for. It is below the two face rungs, and both of those fire on facts that
are not about where the user is pointing:

* **rung 2 (round U) fires on SELECTION STATE ALONE.** A face selection is sticky;
  it survives the tool that made it and the one after that. The rung asks "is
  exactly one face selected", never "is the cursor anywhere near it", so a face
  picked minutes ago captures a sketch drawn thousands of units away in open sky
  and hands back its own tilted plane, positioned at that face's winding centroid.
* **rung 3 (§16) fires on any face the ray reaches**, with no distance bound and
  no angle test. From an oblique camera, a ray aimed at empty space below the
  geometry still crosses a distant wall or roof — at a grazing angle — and that
  face wins.

Rung 4 is then never reached, which is why "the lines should just hit the nearest
major plane" reads as a question about a missing feature when it is actually a
report about rung ordering.

**TWO TESTS, AND A FACE HAS TO PASS BOTH.**

*FACING* (both rungs). `|n . raydir|` must clear `KCON_PLANE_FACING_MIN`, cos(85
degrees). A plane you are looking along the edge of is not a plane you can place
points on: as it turns edge-on the pixels-to-world scale runs away, one pixel of
cursor travel slides the point by tens of units, and past 90 degrees the point is
behind the eye. This is the whole of rung 3's defect and half of rung 2's.

*REACH* (rung 2 only). The cursor ray must meet the selected face's plane inside
the face's own bounds grown by `KCON_FACE_PLANE_SLACK` (1.0) of its largest
extent, floored at 16 units so a tiny face still has reach. Round U's flow — select
a face, draw a window on it — puts the cursor ON the face, and its documented
tolerance ("the cursor drifted onto the floor behind it") is a drift of a
face-width, not of a map. Rung 3 needs no reach test: its face is under the cursor
by construction. With NO cursor ray at all (the tool started from a menu) the
question cannot be asked and round U's answer stands unchanged.

A refusal is a FALL-THROUGH, not an error: the ladder carries on, reaches rung 4,
and the user gets the flat major plane.

#### (b) "Slightly above": the working height was never a height

Three rungs synthesise an axis plane — the Z cycle (`KiwiCon_SetPlaneAxis`), rung
4 and rung 0 — and all three built it as `origin = the previous plane's origin,
verbatim`. That origin is not a height anyone chose. It is whatever the last
plane-setter left behind:

* rung 3's `pick.point` — the RAW ray hit on a face, an arbitrary point under the
  cursor, off-grid by construction (`kiwi_construct.h`'s own round-R note says so
  in those words);
* rung 2's winding CENTROID;
* rung 1's construction-object origin;
* `PushPoint`'s placed point, which may have come off a geometry snap.

Nor is it ever quantised: `KiwiCon_SnapUV` snaps the two IN-PLANE coordinates and
deliberately never touches the normal component. **Round R fixed exactly this
poisoning for the LATTICE, by anchoring it at the world origin; the plane's own
OFFSET was left alone, and that is the other half of the same bug.** Lock to TOP
after drawing on anything at all and the XY plane arrives at that thing's z — a
fractional number, a few units up. "Slightly above it."

**TWO RULES.**

1. **AN OFFSET IS ONLY INHERITED FROM A PARALLEL PLANE.** If the plane being
   replaced is parallel to the one being built, its offset along that axis IS the
   height being worked at, and keeping it is round T/Y's documented intent ("XY
   after a placement at z = 128 means the plane THROUGH that placement"). If it is
   not parallel, `origin[axis]` is the arbitrary coordinate of a point that
   happened to lie on some other plane — there is no height to inherit, and the
   major plane is the world's own: **0**.
2. **THE OFFSET IS GRID-QUANTISED.** A working height is a number the user could
   type; 64.3271 is not. With round AJ's grid-snap master switch off,
   `KiwiGrid_Snap` copies through, which is the same "leave it alone" answer every
   other caller already handles.

The two off-axis components carry through untouched — they do not affect the plane
at all, and they keep the working-plane overlay square where the work is.

**AND THE PROBE THAT LOST STOPPED LEAVING A TRACE.** Rung 0 tries rung 1 first and
takes it only when the object's plane is PARALLEL to the view plane — but
`KiwiCon_PlaneFromCursorConstruction` WRITES the active plane before that test can
reject it, so a non-parallel object used to leave its own plane installed and the
fall-through then read ITS origin as "where the work is". The plane is snapshotted
and restored when the parallel test fails.

**THE HEIGHT IS NOW VISIBLE.** It was invisible state: announced once in the
console, by a line that scrolls away in a pane nobody is looking at while drawing.
A `Plane` chip joins the command strip while a plane-placing tool is live
(`KiwiCon_PlanePlacement`, the same question `kiwi_snap.cpp`'s CPLANE arm asks),
showing `XY 128` in §17 display units — or `tilted`. Shown ONLY in the
non-default state, exactly as the "Grid / Snap OFF" chip beside it is: on the
world's own major plane there is nothing to warn about.

### Item 3 — LOFT / CURVE: it did not curve, and it only had one side

USER REPORT, verbatim: *"For the lofting curve option - it does not curve. It only
acts like a brush. You need to make it a perfect curve and Add G0/G1/G2 like in
plasticity for start and end. Also when doing a curve like this, have an option for
2-sided faces (Texture on BOTH sides). Currently in some scenarios it only shows 1
side of the walls texture at a time."*

#### (a) What round AO actually emitted

The control grid was right — one patch per profile edge, `width = 2*spans + 1`,
even columns on the stations, odd columns the arc handle, `height = 3` — and the
handle solve (the 2x2 closest approach of the two tangent lines) was right. **The
TANGENTS fed to it were not.**

`SpanTangent` took the central difference of the neighbouring stations' rail points
and clamped to a ONE-SIDED difference at the two end stations. At station 0 that is
`normalize(P1 - P0)`: **the span's own chord**. `ArcHandle` then intersects a line
that LIES ON the chord with the other tangent line, so the meeting point is on the
chord, the quadratic degenerates to a straight segment, and the "arc" is the chord.
The same at the last span from the other side.

**AT THE DEFAULT DENSITY THAT IS EVERY SPAN.** `KLOFT_DEF_SEGS_CURVE` is 2, so
spans 0 and 1 ARE the first and the last: both flat, meeting at an angle at the
middle station. A polyline of flat quads — "it only acts like a brush", and the
picture is exactly that: planar facets meeting at angles. At density 3+ the
interior spans did bend, which is why this survived a round.

**THE CORRECT END TANGENTS WERE ALREADY COMPUTED AND WERE THROWN AWAY.**
`BuildStations` derives `outA` / `outB` from the two source faces' own plane
normals — the directions the Hermite path leaves A and arrives at B — as LOCALS.
They are now stored on the command (both pointing forward along the path) and are
what the end stations' tangents come from. That is the definition of G1 and it is
what the mode was documented to do.

**AND THE PREVIEW DREW STRAIGHT RAILS**, which is an honest picture of a brush
bridge and a lie about a patch one — and is precisely what hid this: the preview
looked identical whether the handle curved or collapsed onto the chord. In CURVE
mode each rail span is now evaluated as the quadratic Bezier it will become, using
the SAME `SpanTangent` / `ArcHandle` the commit uses.

#### (b) G0 / G1 / G2, per end, and exactly what each one means

A Continuity row per END (START = face A, END = face B), CURVE mode only:

* **G0** — the end span leaves along its own CHORD. This is the old behaviour,
  kept and NAMED rather than deleted: a crease where the sweep meets the source
  face is sometimes what a mapper wants, and it is exact for a straight run.
* **G1** — the end span leaves along the SOURCE FACE'S NORMAL. Tangent-continuous
  with the wall it grows out of, no crease. **The default**, and it is exact: the
  first two control columns are colinear with the face normal, so the surface's
  start direction IS the face's normal.
* **G2** — "arc-fit", and the name is the honest one. A quadratic Bezier span (the
  patch format's three columns) has ONE interior control point, so its second
  derivative is a single constant vector: once the tangent is matched there is no
  freedom left to match a curvature. What G2 does instead is force the end span to
  be a CIRCULAR ARC — the handle goes where a circle would put its tangent
  intersection, `chord / (2*cos(angle between the end tangent and the chord))`
  along the end tangent. That is `kiwi_patchfillet.cpp`'s `ArcPoint` rule
  (`r / cos(alpha/2)`, `kiwi_patchfillet.cpp:217-234`) written for a path whose
  radius is not known in closed form; for a quarter turn of radius r both
  expressions give the same point. Constant curvature across the span is what
  makes two equal spans agree on curvature at the station between them.
  **It is NOT a curvature match against the source face.** That face is a PLANE,
  curvature zero, and matching it honestly would mean the sweep leaves dead
  straight — G0's shape with G1's direction, which is not what anyone means by
  asking for G2. Stated here and in RADIANT_KNOWN_ISSUES rather than papered over.

An arc fit that cannot exist (the end tangent does not lean toward the far end by
`KLOFT_ARC_MIN_COS`) falls through to the tangent intersection, and that to the
chord midpoint — so a G2 end whose geometry cannot support an arc behaves as G1
rather than producing a shape nobody asked for. With ONE span and BOTH ends set to
G2 the span cannot be two different circles, so the START wins, stated rather than
left to reading order.

#### (c) Two-sided

A q3 patch is a SHEET with one facing: `Curve_ComputeNormals` (`pmesh.cpp:502`)
takes `cross(dCol, dRow)`, so it is visible from one side and invisible from the
other — the report's "only shows 1 side of the walls texture at a time". The
editor's own answer is the double-sided patch idiom: a second patch over the same
control points with the row order REVERSED, which is what `patchInvert2`
(`pmesh.cpp:2232`, the Curve-to-Negative primitive) does to a selection.

That primitive is `static` and takes the SELECTION, so it cannot be called from
here; the in-tree precedent for orienting a patch being BUILT is
`kiwi_patchfillet.cpp`'s `rowFlip` (`:870`, honoured at `:1437-1443`) — swap the
row ends as the control points are written. Two-sided therefore writes the SAME
strip twice through one helper, the second with its rows mirrored, both inside the
one undo bracket, both carrying the same material and the same CAP alignment.
**Default OFF** (it doubles the patch count), and the caulk/no-collision hint
prints on every creation as it already did.

All three settings PERSIST in the profile ini (`[KiwiLoft]`), because `Reset()`
runs on every `Begin()` and a preference that has to be re-set per gesture is not
a preference. Read once and lazily, validated, written at the moment of the change
— `kiwi_camera.cpp:606`'s pattern for the ortho toggle.

## Decision log — round AT

- **D-AT1 — A GATE ON A DRAWN OVERLAY IS ABOUT MEANING; THE MEMORY SAFETY BELONGS
  AT THE READ.** Round AQ used the eligibility predicate as a second line of
  defence against an out-of-bounds walk and paid for it by refusing every patch a
  mapper actually makes. The index-checked `ctrl[col][row]` access IS the defence;
  with that in place the predicate can ask the question it is for — "does a flat
  cursor ring mean anything on this surface".
- **D-AT2 — `PATCH_TERRAIN` NAMES A CREATION PATH, NOT A SHAPE.** Three functions
  set it; every other patch in the editor has type 0. Any test that means "is this
  a sheet" must be written as one (control dims in range, a tessellated mesh, and
  not one of the closed families), never as `type == PATCH_TERRAIN`.
- **D-AT3 — AN OVERLAY MUST NOT BE NARROWER THAN THE VERB IT DRAWS.** The paint
  stroke edits any patch it is given. A ring drawn on strictly fewer patches than
  the brush paints is a lie in the opposite direction from the crash, and it is the
  one the user reported.
- **D-AT4 — A RUNG MAY ONLY ANSWER FROM SOMETHING IT IS ACTUALLY POINTING AT.**
  Selection state is sticky and a ray reaches to infinity; neither is evidence
  about where the user is drawing. Rung 2 gains reach, both face rungs gain facing,
  and a rung that declines simply falls through to the next one.
- **D-AT5 — AN EDGE-ON PLANE IS NOT A PLANE YOU CAN DRAW ON.** cos(85 degrees) is
  not a robustness nicety: past it, a pixel of cursor travel is tens of world
  units, and the "plane" a click lands on is numerically meaningless.
- **D-AT6 — A MAJOR PLANE'S OFFSET IS A WORKING HEIGHT OR IT IS NOTHING.**
  Inherited only from a PARALLEL plane, grid-quantised always. An offset carried
  over from a tilted face plane is not a height the user chose, and carrying it was
  the whole of "slightly above".
- **D-AT7 — A PROBE THAT LOSES LEAVES NO TRACE.** Rung 0's conditional rung-1 probe
  now restores the plane when the parallel test rejects it. A candidate that has
  been ruled irrelevant must not still be setting state.
- **D-AT8 — INVISIBLE STATE GETS A READOUT, NOT ONLY A BETTER DEFAULT.** The height
  rule fixes the default; the chip is what makes the next surprise reportable as a
  number instead of "slightly above".
- **D-AT9 — THE END TANGENT OF A SWEEP IS THE FACE'S NORMAL, NOT THE CHORD.** A
  one-sided difference at an end station is the chord by definition, and an arc
  solved from it is the chord. The two normals were already computed for the
  Hermite path; using them for the handles is the same statement made twice.
- **D-AT10 — G2 IS SHIPPED AS "ARC-FIT" AND SAYS SO.** A quadratic span has one
  interior control point and therefore one curvature; true curvature matching needs
  a higher degree than the patch format has. Naming the approximation is the
  difference between a documented limit and a lie in a dropdown.
- **D-AT11 — THE PREVIEW EVALUATES THE SAME MATH AS THE COMMIT.** Round AO's rule
  ("the preview runs the commit"), applied to the CURVE rails. A straight rail over
  a curved strip is how a flat "curve" survived a whole round unnoticed.
- **D-AT12 — TWO-SIDED IS TWO PATCHES, BECAUSE THAT IS WHAT THE FORMAT HAS.** A q3
  patch has one facing. The editor's own Negative primitive is the row reverse, and
  the fillet already flips rows at build time; both copies go through one write
  helper so they can never drift apart.
- **D-AT13 — A CURVE-ONLY OPTION ROW ARRIVES WITH THE MEDIUM.** `enabledBy` can
  express only "that option is non-zero", which cannot say "the Bridge row is
  CURVE". Given the choice between rows that are live in TANGENT where they do
  nothing and rows that appear with the medium, the live-but-dead control is the
  worse lie. Widening the framework's gate would touch every command's table.
