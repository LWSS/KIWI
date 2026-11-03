#pragma once
#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// KIWI (2026-09-28, user: "I want the pipe tool copied from plasticity. It allows me to select
// a construction line/spline and create a brush/curve"): Pipe sweeps a regular section along
// the selected construction objects (Plasticity commands/evolution/PipeCommand.ts +
// PipeFactory.ts: vertex count 0 = round, 2 becomes 3, section size, angle).
//   straight spine (line / polyline / rectangle): one convex brush prism per segment; a round
//                  section (vertex count 0) makes a patch cylinder per segment instead;
//   curved spine   (spline spans / circle / arc): q3 curve patches only, no brushes - round,
//                  or faceted with straight patch edges (at most 7 sides).
// Sections are carried along the spine in a rotation-minimising frame, so they do not twist.
// The command starts paused; the radius follows only a drag of its lollipop handle or a typed
// value.  Enter / RMB / Confirm writes one undo record; the spine stays.
//
// KIWI (2026-11-01, user: "add a sweep command that is basically the pipe tool, but for a
// custom construction face ... on Shift-P"): Sweep takes a construction face as the section.
// The face is turned by the least rotation that lays it across the path's start tangent, about
// its ANCHOR point, which then runs exactly along the path.  The anchor defaults to the path's
// start when that lies on the face (the sweep stays as drawn), else the face's centroid; a click
// on a marked corner / centroid picks another, [ ] steps through them.  Straight runs make one
// brush prism per convex piece of the face; elbows and curved paths a tube of curve patches per
// face loop, or (option) mitered brush segments along the curve.  With only the face or only
// the path selected, the command asks for a click on the other.

class KiwiEditorCommand;

#define KPIPE_MAX_SIDES          16     // brush sections
#define KPIPE_MAX_PATCH_SIDES     7     // a patch row holds at most 2 * 7 + 1 columns
#define KPIPE_SPANS_PER_PATCH     7     // rows along the spine per patch: 2 * 7 + 1
#define KPIPE_MAX_PATCHES       128
#define KPIPE_MIN_RADIUS       0.5f

void KiwiPipe_RegisterCommands();
KiwiEditorCommand *KiwiPipe_CommandForId( int commandId );
// A pipeable construction object (line, polyline, rectangle, circle, arc, spline) is selected.
bool KiwiPipe_CanExecute();
// A construction face or a pipeable object is selected (Sweep picks the other by clicking).
bool KiwiSweep_CanExecute();
