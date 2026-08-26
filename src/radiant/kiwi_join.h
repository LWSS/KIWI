#pragma once
#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// Context-sensitive Join merges the owners of exactly two coplanar faces on
// different brushes, joins construction lines, or prints guidance.
// Plasticity's J joins curves only; KIWI's face arm maps the requested behavior
// onto the existing convex-solid CSG_Merge core (csg.cpp:572, 0x47DA40).
//
// Coplanarity uses the validity tolerances:
//   |dot(n1,n2)| > KVALID_PLANE_DOT and
//   |d1 - sign(dot) * d2| < KVALID_PLANE_DIST.
// Plane distances use world units and n·p <= dist. Touching faces can have
// opposed normals, so the absolute dot and sign-adjusted distance are deliberate.
// Command 32927 owns CSG undo, refusal recovery, and reporting; this adapter only
// establishes the two-brush selection. The face-arm check deliberately runs first.

void KiwiJoin_RegisterCommands();
bool KiwiJoin_DispatchInstant( unsigned int commandId );

// Palette predicate: one of the two real arms is available.
bool KiwiJoin_CanJoin();
