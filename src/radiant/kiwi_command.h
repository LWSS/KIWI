#pragma once
#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif

// KIWI command metadata and modal-command framework. g_radiantCommands remains
// the binding registry and Radiant_ExecCommand remains the dispatcher; this file
// adds palette metadata and one active modal lifecycle.
//
// A command starts HOT. LMB parks it; any bare viewport LMB resumes and rebases;
// RMB click or Enter commits; Esc clears numeric input before cancelling.
// Multi-click tools place points and never pause. Selection keeps its normal
// grammar: Shift adds and Ctrl removes.
//
// The active command owns LMB and preview motion before selection, but MMB, the
// wheel, and RMB drag remain camera navigation. Only a no-drag RMB click confirms.
//
// Undo protocol: begin = ClearRedo/GeneralStart/AddBrushList; commit =
// EndBrushList/End; cancel adds Undo/clear-redo. Cover extra brushes before
// mutation, add entities before brushes, and use static storage for operation
// because undo stores the pointer. Non-mutating commands open no bracket.
#include "kiwi_numeric.h"                // kiwiNumField_t
#include "kiwi_pick.h"
#include "kiwi_snap.h"

// Reserved ids: 34001..34029 are instant, 34030..34069 modal, and
// 34100..34199 the second instant block. Next free: instant 34142, modal 34066.
// Every id in the modal block routes through KiwiCmd_Start before instant dispatch.
#define KIWI_CMD_FIRST              34000
#define KIWI_CMD_LAST               34199
#define KIWI_CMD_MODAL_FIRST        34030
#define KIWI_CMD_MODAL_LAST         34069

#define KIWI_CMD_SELMODE_POINT      34001   // §11 selection-mode chips, modern keys 1..5
#define KIWI_CMD_SELMODE_EDGE       34002
#define KIWI_CMD_SELMODE_FACE       34003
#define KIWI_CMD_SELMODE_OBJECT     34004
#define KIWI_CMD_SELMODE_ALL        34005
#define KIWI_CMD_PALETTE            34010   // §15 command palette (modern F)
#define KIWI_CMD_GRID_HALVE         34020   // §17 modern grid spacing, modern [
#define KIWI_CMD_GRID_DOUBLE        34021   // §17 modern grid spacing, modern ]
#define KIWI_CMD_SNAP_TOGGLE        34022   // §6  snap markers on/off
#define KIWI_CMD_CPLANE_XY          34011   // §16 construction planes (kiwi_construct)
#define KIWI_CMD_CPLANE_XZ          34012
#define KIWI_CMD_CPLANE_YZ          34013
#define KIWI_CMD_CPLANE_FACE        34014
#define KIWI_CMD_CPLANE_VIEW        34015
#define KIWI_CMD_CONSTRUCT_CLEAR    34016   // §7  drop every construction object
#define KIWI_CMD_CONSTRUCT_UNDO     34017   // §7  pop the construction store's own undo
#define KIWI_CMD_SELECT_COPLANAR    34023   // §25 selection expansion (kiwi_selext)
#define KIWI_CMD_SELECT_TOUCHING    34024
#define KIWI_CMD_SELECT_MATERIAL    34025
#define KIWI_CMD_SELECT_CONNECTED   34026   // …also the camera double-click action
#define KIWI_CMD_PICK_TEXTURE       34006   // §26 pick the material under the 3D cursor (kiwi_uv)
#define KIWI_CMD_WINDOW_XY          34007   // "2D View"
#define KIWI_CMD_WINDOW_Z           34008   // "Z"
#define KIWI_CMD_WINDOW_TEXTURE     34009   // "Textures"
#define KIWI_CMD_WINDOW_CONSOLE     34018   // "Console"
#define KIWI_CMD_WINDOW_SHELL       34019   // "KIWI ImGui shell"
#define KIWI_CMD_ADD_MENU           34027   // §16b creation palette at the cursor (kiwi_addmenu)
#define KIWI_CMD_VIEW_SHOW_GRID     34028   // §17 grid on/off, mirrored in the native View menu
#define KIWI_CMD_VIEW_SHOW_AXES     34029   // §17 axes on/off, likewise
#define KIWI_CMD_SELFTEST           34030   // MODAL: "UX: Modal Self-Test" (palette-only)
#define KIWI_CMD_MOVE               34031   // MODAL: §13 G — context-aware move (kiwi_transform)
#define KIWI_CMD_ROTATE             34032   // MODAL: §13 R — whole-selection rotate
#define KIWI_CMD_SCALE              34033   // MODAL: §13 S — whole-selection scale
#define KIWI_CMD_DRAW_LINE          34034   // MODAL: §7 drawing tools (kiwi_construct)
#define KIWI_CMD_DRAW_POLYLINE      34035
#define KIWI_CMD_DRAW_RECT          34036
#define KIWI_CMD_DRAW_CIRCLE        34037
#define KIWI_CMD_DRAW_ARC           34038
#define KIWI_CMD_EXTRUDE_REGION     34039   // MODAL: §23 region → brushes (kiwi_extrude)
#define KIWI_CMD_BEVEL_EDGE         34040   // MODAL: §25 bevel/chamfer (kiwi_bevel)
#define KIWI_CMD_INSET_FACE         34041   // MODAL: §25 inset, the clone compound (kiwi_bevel)
#define KIWI_CMD_ARRAY_LINEAR       34042   // MODAL: §25 arrays (kiwi_dupe)
#define KIWI_CMD_ARRAY_RADIAL       34043
#define KIWI_CMD_TEX_SHIFT          34044   // MODAL: §26 texture shift  (kiwi_uv)
#define KIWI_CMD_TEX_ROTATE         34045   // MODAL: §26 texture rotate (kiwi_uv)
#define KIWI_CMD_TEX_SCALE          34046   // MODAL: §26 texture scale  (kiwi_uv)
#define KIWI_CMD_DRAW_RECT_CENTER   34047   // MODAL: centre + corner   (kiwi_construct)
#define KIWI_CMD_DRAW_CIRCLE_2PT    34048   // MODAL: diameter endpoints
#define KIWI_CMD_DRAW_POLYGON       34049   // MODAL: centre + radius, 3..32 sides
#define KIWI_CMD_DRAW_SPLINE        34050   // MODAL: Catmull-Rom through clicked points
#define KIWI_CMD_PRIM_BOX           34051   // MODAL: §16b solids (kiwi_primitive)
#define KIWI_CMD_PRIM_CYLINDER      34052
#define KIWI_CMD_PRIM_SPHERE        34053
#define KIWI_CMD_PRIM_CONE          34054
#define KIWI_CMD_CUT                34055   // MODAL: C      solid + construction line -> 2 solids
#define KIWI_CMD_MATCH_FACE         34056   // MODAL: Z      make the source face coplanar with a picked one
#define KIWI_CMD_SPLIT_FACE         34057   // MODAL: Ctrl+R split the owning brush across a face
#define KIWI_CMD_EXTRUDE_FACE       34058   // MODAL: E      face winding -> a NEW brush, original untouched
#define KIWI_CMD_TRIM               34059   // MODAL: T      remove a line's span between crossings
#define KIWI_CMD_SELCONV_POINT      34100   // Ctrl+1  selection -> its points
#define KIWI_CMD_SELCONV_EDGE       34101   // Ctrl+2  selection -> its edges
#define KIWI_CMD_SELCONV_FACE       34102   // Ctrl+3  selection -> its faces
#define KIWI_CMD_SELCONV_OBJECT     34103   // Ctrl+4  selection -> its owning objects
#define KIWI_CMD_CONSTRUCT_JOIN     34104   // Ctrl+J  chain the selected lines into one polyline
#define KIWI_CMD_CONSTRUCT_DELETE   34105   // (Delete, via the key funnel — see below)
#define KIWI_CMD_JOIN               34106   // J  faces -> CSG_Merge, lines -> JoinLines
#define KIWI_CMD_OFFSET_CURVE       34060   // MODAL: O   parallel copy of a construction chain
#define KIWI_CMD_FILLET_CURVE       34061   // MODAL: B   round a chain's corners into arcs
#define KIWI_CMD_BOOLEAN            34062   // MODAL: Q   difference / union of solids
// Bare B redirects here for brush-edge selections; keep the palette row unbound.
#define KIWI_CMD_FILLET_EDGE        34063   // MODAL: B (brush edges) fillet -> patch
#define KIWI_CMD_DUPLICATE          34107   // Shift+D  clone the selection, then Move PAUSED
#define KIWI_CMD_FOCUS_SELECTION    34108   // /        frame the selection (kiwi_focus.h)
#define KIWI_CMD_HIDE_INVERT        34109   // Ctrl+H   swap hidden and visible (kiwi_visibility.h)
#define KIWI_CMD_REPEAT_LAST        34110   // Shift+R  re-run the last KIWI command
#define KIWI_CMD_VIEW_ORTHO         34111   // toggle orthographic / perspective 3D camera
#define KIWI_CMD_CLIP_CUT           34112   // Ctrl+X   copy the selection, then delete it
#define KIWI_CMD_REMOVE_FACE        34113   // Delete   one selected FACE -> gone, edge back
#define KIWI_CMD_CONSTRUCT_HIDE     34114   // (H, via the key funnel)
#define KIWI_CMD_CONSTRUCT_UNHIDE   34115   // "Unhide All (construction)"
#define KIWI_CMD_WINDOW_OUTLINER    34116   // "Outliner"
#define KIWI_CMD_GROUP_CREATE       34117   // selection -> a func_group / construction group
#define KIWI_CMD_GROUP_UNGROUP      34118   // dissolve the selection's groups
#define KIWI_CMD_MATINFO            34119   // "Material info (under cursor)"
#define KIWI_CMD_AUTO_BOOL          34120   // "Auto Bool (consolidate brushes)"
#define KIWI_CMD_PRIM_BOX_CENTER    34064   // MODAL: Shift+V  centre + half-extents
#define KIWI_CMD_LOFT               34065   // MODAL: L        face + face -> a bridge
// ENT_DROP, MODEL_DROP, and IMPORT_DROPPED are internal payload consumers;
// never register them.
#define KIWI_CMD_WINDOW_ENTITIES    34121   // "Entities" (the browser dock window)
#define KIWI_CMD_ENT_DROP           34122   // internal: place the dragged eclass
#define KIWI_CMD_MODELINFO          34123   // "Model info (next frame's models)"
#define KIWI_CMD_WINDOW_SKY         34124   // "Sky" (the sky-material dock window)
#define KIWI_CMD_SKY_APPLY          34125   // "Apply sky material to selection"
#define KIWI_CMD_SKY_SHELL          34126   // "Create skybox shell"
#define KIWI_CMD_WINDOW_UVEDITOR    34127   // "UV editor" (the TrenchBroom-style UV window)
#define KIWI_CMD_IMPORT_DROPPED     34128   // internal: run the wizard on the dropped-file queue
#define KIWI_CMD_IMPORT_BROWSE      34129   // "Import Textures..." (the no-drag entry point)
#define KIWI_CMD_BUILD_RUN          34130   // "Build & Run..." (cod4map / cod4rad / the game)

#define KIWI_CMD_CAULK_FACES        34131   // "Caulk Selection" (End, modern profile)

#define KIWI_CMD_VIEW_FACE          34132   // "View Face Head-on" (Space, modern profile)
// Section is persistent view state; making it modal would monopolize the gesture slot.
#define KIWI_CMD_SECTION_TOGGLE     34133   // "Section Analysis" (view-cube button)
#define KIWI_CMD_INSTBATCH          34134   // "Instance batching (toggle)"
#define KIWI_CMD_PLACE_SUN          34135   // "Place Sun" (Add menu / palette)
#define KIWI_CMD_WINDOW_SUN         34136   // "Sun" (the sun-helper dock window)
#define KIWI_CMD_WINDOW_LIGHT       34137   // "Light" (the selected-light helper)
#define KIWI_CMD_WINDOW_INSPECTOR   34138   // "Inspector" (per-type entity properties)
#define KIWI_CMD_WINDOW_MODELS      34139   // "Models" (the static-xmodel browser)
#define KIWI_CMD_MODEL_DROP         34140   // internal: place the dragged xmodel
#define KIWI_CMD_PLASTICITY_PUSH    34141   // "Send Selection to Plasticity"

// Shared with mainfrm's dispatch gate so range changes have one definition.
bool KiwiCmd_IsKiwiId ( int id );
inline bool KiwiCmd_IsModalId( int id ) { return id >= KIWI_CMD_MODAL_FIRST && id <= KIWI_CMD_MODAL_LAST; }

// Optional palette metadata keyed by existing command id.
struct kiwiCommandInfo_t
{
    const char *displayName;    // "Extrude Region" — palette label
    const char *category;       // "Modeling" / "Selection" / "KIWI" / "Classic"
    sel_mask_t  selKindMask;    // the selection kinds this command is meaningful for (0 = any)
    bool      (*canExecute)();  // NULL = always available.  Greys the palette row.
};

// Returns metadata or NULL; callers fall back to the registry name and "Classic".
const kiwiCommandInfo_t *KiwiCmd_Info( int commandId );

// Runs the metadata predicate; missing predicates mean available.
bool KiwiCmd_CanExecute( int commandId );

// Appends the complete KIWI command set to the shared registry. Classic-profile
// bindings are registered here; kiwi_keymap applies the modern profile.
void KiwiCmd_RegisterCommands();

// Dispatches instant ids directly and modal ids through KiwiCmd_Start.
bool KiwiCmd_Dispatch( unsigned int cmdId );

// Classic Paste/Clone post-tail: enter Move PAUSED over newly selected output.
void KiwiCmd_AfterPaste();

// Steps to the nearest nice-number ladder rung on the requested side.
void KiwiCmd_StepGrid( bool doubleIt );

// Applies any positive typed spacing within the shared clamp; never ladder-snaps it.
bool KiwiCmd_SetGridSpacing( float inches );

bool KiwiCmd_HasRepeatable();
const char *KiwiCmd_RepeatLabel();       // display name of the recorded id, or NULL
bool KiwiCmd_RepeatLast();               // false = nothing recorded / it refused

// Ported Copy followed by ported Delete Selection; empty selection preserves clipboard.
void KiwiCmd_ClipCut();

// One static-lifetime keycap/label pair for the modal HUD.
struct kiwiPrompt_t
{
    const char *key;
    const char *label;
};

// Generic command-panel rows. NUMFIELD shares NumericFieldValue/Changed state;
// enabledBy is an option index that must be nonzero, or -1 for always enabled.
enum kiwiOptKind_t
{
    KOPT_ENUM = 0,
    KOPT_TOGGLE,
    KOPT_INT,
    KOPT_NUMFIELD
};

struct kiwiOption_t
{
    const char        *label;
    kiwiOptKind_t      kind;
    const char *const *choices;      // KOPT_ENUM only
    int                choiceCount;  // KOPT_ENUM only
    int                field;        // KOPT_NUMFIELD: the NumericFields index
    float              lo, hi;       // KOPT_INT / KOPT_NUMFIELD clamps
    int                enabledBy;    // option index gating this row, or -1
};

// Modal command contract. Pointer-returning text/tables must outlive the frame.
class KiwiEditorCommand
{
public:
    virtual ~KiwiEditorCommand() {}

    // Static-lifetime HUD label.
    virtual const char *Name() const = 0;                 // HUD label; static storage
    // Preflight gate; false refuses entry.
    virtual bool CanExecute()                    { return true; }

    // Latches initial state; false means no mutation and no open undo bracket.
    virtual bool Begin()                         { return true; }

    // Receives the current pick and snap once per HOT cursor update.
    virtual void MouseMove( const pick_result_t &pick, const snap_result_t &snap ) { (void)pick; (void)snap; }

    // Handles command keys. Numeric input runs first; Esc/Enter framework actions run after.
    virtual bool KeyDown( int vk, unsigned int mods ) { (void)vk; (void)mods; return false; }

    // Single-field update for field 0; world is already in raw world units.
    virtual void NumericChanged( bool has, float world ) { (void)has; (void)world; }

    // Finalizes the preview; the framework closes its undo bracket afterwards.
    virtual void Commit()                        {}
    // Restores the exact pre-gesture state.
    virtual void Cancel()                        {}

    // Emits the live world overlay into the framework-owned kiwi_lines batch.
    virtual void DrawWorld()                     {}

    // Requests current overlay segments; 0 uses the default. The framework clamps,
    // so requests above the ceiling must degrade visibly rather than vanish.
    virtual int LineBudget() const               { return 0; }

    // Flags used for both framework pick and snap queries.
    virtual unsigned PickFlags() const           { return PICKF_NONE; }

    // Optional static/owned status fragment appended to the numeric HUD.
    virtual const char *HudStatus() const        { return 0; }

    // True marks the HUD invalid; Commit should then behave as Cancel.
    virtual bool HudInvalid() const              { return false; }

    // True changes LMB from pause/commit grammar into point-placement events.
    virtual bool WantsClicks() const             { return false; }

    // Called after MouseMove for a click tool; true keeps running, false commits.
    virtual bool Click()                         { return true; }

    // Consumes a pending valid typed value by advancing one stage; never commits.
    virtual bool AdvanceStage()                  { return false; }

    // Opts into Shift+LMB marquee while live; bare LMB retains pause/resume meaning.
    virtual bool WantsMarquee() const            { return false; }

    // Camera-image coordinates use a top-left origin and remain unnormalized;
    // crossing means right-to-left. Shift is additive.
    virtual void Marquee( int x0, int y0, int x1, int y1, bool crossing, bool shift )
    { (void)x0; (void)y0; (void)x1; (void)y1; (void)crossing; (void)shift; }

    // Field-aware numeric update; all values have passed through Units_FromDisplay.
    // Keep this separate: widening NumericChanged would stop existing overrides.
    virtual void NumericFieldChanged( int field, bool has, float world )
    {
        if ( field == 0 )
            NumericChanged( has, world );
    }

    // Returns static field definitions; 0 keeps one unnamed editable length field.
    virtual int NumericFields( const kiwiNumField_t **out ) const
    { (void)out; return 0; }

    // Returns a live natural-unit value: world units, degrees, or a bare scalar.
    virtual bool NumericFieldValue( int field, float *out ) const
    { (void)field; (void)out; return false; }

    // Returns the value-bubble world anchor; false falls back to the last snap.
    virtual bool BubbleAnchor( float *out3 ) const { (void)out3; return false; }

    // May consume one LMB before pause/resume; LastCursor is already current.
    virtual bool PressIntercept( int imgX, int imgY ) { (void)imgX; (void)imgY; return false; }

    // Optionally relocates only the snap query in top-left camera-image space.
    virtual bool SnapQueryAnchor( int *outX, int *outY ) const
    { (void)outX; (void)outY; return false; }

    // Returns static command-specific key prompts; framework prompts are implicit.
    virtual int HudPrompts( const struct kiwiPrompt_t **out ) const
    { (void)out; return 0; }

    // Returns static generic panel options; 0 means no panel.
    virtual int CommandOptions( const struct kiwiOption_t **out ) const
    { (void)out; return 0; }

    // Returns the current toggle, enum index, or integer option value.
    virtual int OptionValue( int opt ) const { (void)opt; return 0; }

    // Applies an option through the same state transition used by its keyboard path.
    virtual void OptionChanged( int opt, int value ) { (void)opt; (void)value; }

    // Supplies the current anchor and signed outward direction, replacing the gizmo.
    virtual bool LollipopHandle( float outAnchor[3], float outDir[3] ) const
    { (void)outAnchor; (void)outDir; return false; }

    // Opens/closes the command's handle gate after the framework rebases the mapping.
    virtual void HandleGrab( bool held ) { (void)held; }

    // May reclaim an idle selection press before resume. Shift adds, Ctrl removal
    // stays in selection handling, and an unclaimed bare press resumes.
    virtual bool IdlePressReselect( int imgX, int imgY, bool shift )
    { (void)imgX; (void)imgY; (void)shift; return false; }

    // Allows an unmoved auto-entered gesture to yield to an allow-listed context verb.
    virtual bool PreemptIdle() const { return false; }

    // Allows an explicit transform tool swap; false keeps arbitrary hotkeys swallowed.
    virtual bool CanSwapTo( int commandId ) const { (void)commandId; return false; }

    // On an allowed swap, true commits the old gesture and false cancels it.
    virtual bool GestureMoved() const { return true; }

    // Ctrl inverts the context default. Deliberate Plasticity divergence: transforms
    // are raw unless Ctrl is held; construction matches its snapped/Ctrl-free rule.
    enum kiwiSnapCtx_t
    {
        KSNAPCTX_TRANSFORM = 0,   // move / push-pull / extrude / gizmo: RAW, Ctrl snaps
        KSNAPCTX_CONSTRUCT,       // draw / place / construct: SNAPS, Ctrl frees
        KSNAPCTX_ALWAYS           // the pivot placement: snapping is the whole point
    };

    // Classifies point-placement tools as construction and other gestures as transforms.
    virtual kiwiSnapCtx_t SnapContext() const
    {
        return WantsClicks() ? KSNAPCTX_CONSTRUCT : KSNAPCTX_TRANSFORM;
    }

    // Relatches delta origins on PAUSED->HOT so resume cannot teleport geometry.
    virtual void Rebase()                        {}
};

// Single active-command owner and terminal actions.
KiwiEditorCommand *KiwiCmd_Active();
bool KiwiCmd_Start( int commandId );          // false = unknown id / CanExecute said no
void KiwiCmd_Commit();
void KiwiCmd_Cancel();

// Starts only after Commit, undo close, and numeric reset; Cancel drops the request.
void KiwiCmd_StartDeferred( int commandId, bool paused );

// Defers deselection until after Undo_EndBrushList has captured the selected tail.
void KiwiCmd_DeselectAfterCommit();

// Parks preview motion; click tools do not pause.
void KiwiCmd_Pause();

// Returns to HOT and rebases at the latched cursor.
void KiwiCmd_Resume();

// Unified grab ordering: feed press, resume/rebase, open gate, rebase, feed zero delta.
bool KiwiCmd_HandleGrab( int imgX, int imgY );

// Closes the command's handle gate; the caller decides pause, commit, or cancel.
void KiwiCmd_HandleRelease();

// Sends RMB confirmation through the Enter ladder so command vetoes stay identical.
void KiwiCmd_Confirm();

// True only during KiwiCmd_Confirm dispatch, for commands whose Enter/RMB tails differ.
bool KiwiCmd_ConfirmIsRmb();

// Numeric input -> command key -> Esc/Enter; true means the hotkey table must not run.
bool KiwiCmd_KeyDown( int vk, unsigned int mods );

// Active-command mouse input. Only LMB is accepted; camera buttons never enter here.
bool KiwiCmd_MouseMove  ( int imgX, int imgY );
// Shift reaches idle reselect/click grammar; bare paused LMB resumes.
bool KiwiCmd_MouseButton( int btn, int imgX, int imgY, bool shift = false );

// Last snap supplied to the active command.
const snap_result_t &KiwiCmd_LastSnap();

// Last top-left camera-image cursor, including paused motion; false before any sample.
bool KiwiCmd_LastCursor( int *imgX, int *imgY );

// Shift state latched with the last LMB for PressIntercept/reselect/Click.
bool KiwiCmd_LastShift();

// Live-command Shift-marquee query and resolved-rectangle delivery.
bool KiwiCmd_WantsMarquee();
void KiwiCmd_Marquee( int x0, int y0, int x1, int y1, bool crossing, bool shift );

// Draws the active overlay and snap marker in one bounded batch.
void KiwiCmd_DrawWorld();

// Default covers the largest ordinary preview plus its marker; the ceiling limits
// render-command pressure while allowing set-sized overlays.
#define KCMD_LINE_BUDGET      288
#define KCMD_LINE_BUDGET_MAX  1536

// Polls Ctrl and applies the active inversion; KSNAPCTX_ALWAYS ignores Ctrl.
bool KiwiCmd_SnapEngaged();

// Shell key funnel after ImGui text input and before accelerators/hotkeys.
bool KiwiUX_KeyFunnel( unsigned int vk );

// One undo bracket per gesture; operation must have static storage.
void KiwiCmd_UndoBegin ( const char *operation );
void KiwiCmd_UndoCommit();
void KiwiCmd_UndoCancel();

// Covers a brush omitted from selected_brushes before mutation. Fixed-size owners
// are added entity-first; Undo_AddBrush self-deduplicates. Mirrors undo.cpp 0x45E7C0.
void KiwiCmd_UndoCoverBrush( selbrush_t *node );
