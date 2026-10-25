#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// CSG workflow UI over the ported geometry cores; no CSG math lives here.
// Buttons dispatch command ids so the shared handlers own execution.

#include "stdafx.h"
#include "qe3.h"
#include <imgui/imgui.h>

#include "winding.h"

#include "kiwi_csg.h"
#include "kiwi_autobool.h"
#include "kiwi_command.h"                      // KIWI_CMD_AUTO_BOOL
#include "kiwi_pick.h"                         // Pick_WorldToImage (failure hint)
#include "kiwi_ported.h"

#include <math.h>
#include <vector>

namespace
{
    // Classic mainfrm.cpp dispatch ids: Hollow, Merge, and Auto Caulk.
    const int KCSG_ID_HOLLOW     = 32982;
    const int KCSG_ID_MERGE      = 32927;
    const int KCSG_ID_AUTO_CAULK = 33220;

    int  s_beforeCount = -1;          // -1 = no NoteBefore is pending

    int SelectedCount()
    {
        int n = 0;
        for ( selbrush_t *b = selected_brushes.next; b != &selected_brushes; b = b->next )
            ++n;
        return n;
    }

    // Baseline patch/fixed-owner gate shared by the CSG-family tools.
    inline bool CsgUsable( const selbrush_t *b ) { return KiwiCsg_BrushUsable( b ); }
}

bool KiwiCsg_BrushUsable( const selbrush_t *b )
{
    if ( !b || !b->def )
        return false;
    if ( b->patch )                                    // instance patch @0x20
        return false;
    const entity_s *owner = b->owner;
    if ( !owner || !owner->def )
        return false;
    const entity_s *ownerDef = owner->def;
    if ( !ownerDef->eclass || ownerDef->eclass->fixedsize )
        return false;
    return true;
}

bool KiwiCsg_SelectionMergeable()
{
    selbrush_t *first = selected_brushes.next;
    if ( first == &selected_brushes || first->next == &selected_brushes )
        return false;
    const entity_s *ownerDef0 = first->owner ? first->owner->def : 0;
    if ( !ownerDef0 )
        return false;
    for ( selbrush_t *b = first; b != &selected_brushes; b = b->next )
    {
        if ( !KiwiCsg_BrushUsable( b ) )
            return false;
        if ( b->owner->def != ownerDef0 )
            return false;
    }
    return true;
}

// Palette predicates.
bool KiwiCsg_CanHollow()
{
    selbrush_t *first = selected_brushes.next;
    if ( first == &selected_brushes )
        return false;
    if ( first->next != &selected_brushes )
        return false;
    return CsgUsable( first ) && !first->xx5;   // KIWI: the core skips xx5 (csg.cpp)
}

bool KiwiCsg_CanMerge()
{
    return KiwiCsg_SelectionMergeable();
}

bool KiwiCsg_CanAutoCaulk()
{
    return selected_brushes.next != &selected_brushes;
}

// Hollow is silent and Merge omits the selection delta, so report it here.
void KiwiCsg_NoteBefore()
{
    s_beforeCount = SelectedCount();
}

void KiwiCsg_NoteAfter( const char *label )
{
    if ( s_beforeCount < 0 )
        return;                                            // no pending observation
    const int before = s_beforeCount;
    const int after  = SelectedCount();
    s_beforeCount = -1;

    if ( before == after )
        Sys_Printf( "%s: %i brush(es) unchanged.\n", label ? label : "CSG", before );
    else
        Sys_Printf( "%s: %i brush(es) in, %i out (selection is the result).\n",
                    label ? label : "CSG", before, after );
}

// The caller is already inside ImGui::Begin, so this block must use buttons.
void KiwiCsg_MenuItems()
{
    ImGui::SeparatorText( "CSG" );

    const bool canMerge  = KiwiCsg_CanMerge();
    const bool canHollow = KiwiCsg_CanHollow();
    const bool canCaulk  = KiwiCsg_CanAutoCaulk();

    ImGui::BeginDisabled( !canMerge );
    if ( ImGui::Button( "Merge" ) )
        Radiant_ExecCommand( (unsigned int)KCSG_ID_MERGE );
    ImGui::EndDisabled();
    if ( ImGui::IsItemHovered() && !canMerge )
        ImGui::SetTooltip( "Select 2+ brushes of the SAME entity\n"
                           "(no patches, no fixed-size entities).\n"
                           "The union must stay convex or the merge is refused." );

    ImGui::SameLine();
    ImGui::BeginDisabled( !canHollow );
    if ( ImGui::Button( "Hollow" ) )
        Radiant_ExecCommand( (unsigned int)KCSG_ID_HOLLOW );
    ImGui::EndDisabled();
    if ( ImGui::IsItemHovered() && !canHollow )
        ImGui::SetTooltip( "Select exactly ONE brush (not a patch,\n"
                           "not a fixed-size entity).  Wall thickness is\n"
                           "the CLASSIC grid size, not the modern one." );

    ImGui::SameLine();
    ImGui::BeginDisabled( !canCaulk );
    if ( ImGui::Button( "Auto Caulk" ) )
        Radiant_ExecCommand( (unsigned int)KCSG_ID_AUTO_CAULK );
    ImGui::EndDisabled();

    // Auto Bool runs pair sweeps plus guarded 3+ clusters; it is intentionally unbound.
    ImGui::SameLine();
    ImGui::BeginDisabled( !KiwiAutoBool_CanExecute() );
    if ( ImGui::Button( "Auto Bool" ) )
        Radiant_ExecCommand( (unsigned int)KIWI_CMD_AUTO_BOOL );
    ImGui::EndDisabled();
    if ( ImGui::IsItemHovered() )
        ImGui::SetTooltip( "Merge selected brushes whose union is exactly ONE convex\n"
                           "brush - pairs first, then clusters of 3+ - until nothing\n"
                           "changes.  No outer plane moves, so the volume cannot\n"
                           "change.  Greedy, so the result depends on order; one\n"
                           "undo reverts the whole run." );

    // csg.cpp has no subtract core; Boolean Difference composes the splitter instead.
    ImGui::TextDisabled( "Subtract: press Q (Boolean -> difference), kiwi_boolean.h" );
}

// ─── Failure hint (kiwi_csg.h) ───────────────────────────────────────────────
namespace
{
    const double KCSG_HINT_SECONDS = 6.0;
    const double KCSG_HINT_FADE    = 1.5;       // the last seconds fade out
    const int    KCSG_HINT_MAX     = 4096;      // polygons; a runaway merge stays drawable
    const double KCSG_HINT_SEED    = 262144.0;  // half-size of the seed square per plane
    const double KCSG_HINT_LIMIT   = 131072.0;  // a face reaching this is unbounded

    std::vector< std::vector<float> > s_hint;   // world polygons, xyz triples
    double                            s_hintAt = -1.0;

    double Dot3d( const double *a, const double *b ) { return a[0]*b[0] + a[1]*b[1] + a[2]*b[2]; }

    // Face i of the convex solid: a big square on plane i clipped by every other plane
    // (csg.cpp KmFace).  Empty when the face is degenerate or unbounded.
    void SolidFace( const double *planes, int count, int i, std::vector<double> *w )
    {
        const double *p = planes + i * 4;
        int ax = 0;
        for ( int k = 1; k < 3; ++k )
            if ( fabs( p[k] ) > fabs( p[ax] ) ) ax = k;
        double up[3] = { 0.0, 0.0, 0.0 };
        up[( ax == 2 ) ? 0 : 2] = 1.0;
        const double du = Dot3d( up, p );
        double v[3], r[3];
        for ( int k = 0; k < 3; ++k ) v[k] = up[k] - p[k] * du;
        const double lv = sqrt( Dot3d( v, v ) );
        if ( !( lv > 1.0e-12 ) )
        {
            w->clear();
            return;
        }
        for ( int k = 0; k < 3; ++k ) v[k] /= lv;
        r[0] = v[1]*p[2] - v[2]*p[1];
        r[1] = v[2]*p[0] - v[0]*p[2];
        r[2] = v[0]*p[1] - v[1]*p[0];
        static const double SX[4] = { -1, 1, 1, -1 }, SY[4] = { 1, 1, -1, -1 };
        w->clear();
        for ( int c = 0; c < 4; ++c )
            for ( int k = 0; k < 3; ++k )
                w->push_back( p[k] * p[3] + ( r[k] * SX[c] + v[k] * SY[c] ) * KCSG_HINT_SEED );

        std::vector<double> out;
        for ( int j = 0; j < count && !w->empty(); ++j )
        {
            if ( j == i )
                continue;
            const double *q = planes + j * 4;
            // A coincident plane is the same face: the first copy owns it.
            if ( Dot3d( p, q ) > 0.999999 && fabs( p[3] - q[3] ) < 0.001 )
            {
                if ( j < i )
                    w->clear();
                continue;
            }
            const size_t m = w->size() / 3;
            out.clear();
            for ( size_t a = 0; a < m; ++a )
            {
                const double *pa = &(*w)[a * 3];
                const double *pb = &(*w)[( ( a + 1 ) % m ) * 3];
                const double da = Dot3d( q, pa ) - q[3];
                const double db = Dot3d( q, pb ) - q[3];
                if ( da <= 0.0 )
                    out.insert( out.end(), pa, pa + 3 );
                if ( ( da < 0.0 && db > 0.0 ) || ( da > 0.0 && db < 0.0 ) )
                {
                    const double t = da / ( da - db );
                    for ( int k = 0; k < 3; ++k )
                        out.push_back( pa[k] + ( pb[k] - pa[k] ) * t );
                }
            }
            w->swap( out );
            if ( w->size() < 9 )
                w->clear();
        }
        for ( size_t k = 0; k < w->size(); ++k )
            if ( fabs( (*w)[k] ) > KCSG_HINT_LIMIT )
            {
                w->clear();
                return;
            }
    }

    // Face_MakePlane's convention (csg.cpp KmPlaneOf), outward: n = (p0-p1) x (p2-p1).
    bool FacePlane( const face_t *f, double *out4 )
    {
        double a[3], b[3];
        for ( int k = 0; k < 3; ++k )
        {
            a[k] = (double)f->planepts[0][k] - (double)f->planepts[1][k];
            b[k] = (double)f->planepts[2][k] - (double)f->planepts[1][k];
        }
        double n[3] = { a[1]*b[2] - a[2]*b[1], a[2]*b[0] - a[0]*b[2], a[0]*b[1] - a[1]*b[0] };
        const double l = sqrt( Dot3d( n, n ) );
        if ( !( l > 1.0e-12 ) )
            return false;
        for ( int k = 0; k < 3; ++k )
            out4[k] = n[k] / l;
        const double p1[3] = { f->planepts[1][0], f->planepts[1][1], f->planepts[1][2] };
        out4[3] = Dot3d( out4, p1 );
        return true;
    }

    void AppendPlanes( const brush_t *def, std::vector<double> *planes )
    {
        if ( !def || !def->faces )
            return;
        for ( int f = 0; f < def->faceCount; ++f )
        {
            double p[4];
            if ( FacePlane( &def->faces[f], p ) )
                planes->insert( planes->end(), p, p + 4 );
        }
    }

    void HintPolygon( const double *pts, size_t n )
    {
        if ( n < 3 || (int)s_hint.size() >= KCSG_HINT_MAX )
            return;
        std::vector<float> poly( n * 3 );
        for ( size_t k = 0; k < n * 3; ++k )
            poly[k] = (float)pts[k];
        s_hint.push_back( poly );
        s_hintAt = ImGui::GetTime();
    }
}

void KiwiCsg_FailHintClear()
{
    s_hint.clear();
    s_hintAt = -1.0;
}

bool KiwiCsg_FailHintAddSolid( const double *planes, int count )
{
    if ( !planes || count < 4 )
        return false;
    bool any = false;
    std::vector<double> w;
    for ( int i = 0; i < count; ++i )
    {
        SolidFace( planes, count, i, &w );
        if ( w.size() >= 9 )
        {
            HintPolygon( &w[0], w.size() / 3 );
            any = true;
        }
    }
    return any;
}

void KiwiCsg_FailHintAddBrush( const brush_t *def )
{
    if ( !def || !def->faces )
        return;
    for ( int f = 0; f < def->faceCount; ++f )
    {
        const winding_t *w = def->faces[f].w;
        if ( !w || w->numpoints < 3 )
            continue;
        std::vector<double> pts( (size_t)w->numpoints * 3 );
        for ( int i = 0; i < w->numpoints; ++i )
            for ( int k = 0; k < 3; ++k )
                pts[i * 3 + k] = w->p[i][k];
        HintPolygon( &pts[0], (size_t)w->numpoints );
    }
}

bool KiwiCsg_FailHintAddOverlap( const brush_t *a, const brush_t *b )
{
    std::vector<double> planes;
    AppendPlanes( a, &planes );
    AppendPlanes( b, &planes );
    if ( planes.empty() )
        return false;
    return KiwiCsg_FailHintAddSolid( &planes[0], (int)( planes.size() / 4 ) );
}

void KiwiCsg_DrawFailHint( float imgMinX, float imgMinY, float imgW, float imgH )
{
    (void)imgW; (void)imgH;
    if ( s_hint.empty() || s_hintAt < 0.0 )
        return;
    const double age = ImGui::GetTime() - s_hintAt;
    if ( age > KCSG_HINT_SECONDS )
    {
        KiwiCsg_FailHintClear();
        return;
    }
    float fade = 1.0f;
    if ( age > KCSG_HINT_SECONDS - KCSG_HINT_FADE )
        fade = (float)( ( KCSG_HINT_SECONDS - age ) / KCSG_HINT_FADE );
    const ImU32 fill = IM_COL32( 255, 40, 40, (int)( 70.0f * fade ) );
    const ImU32 edge = IM_COL32( 255, 60, 50, (int)( 235.0f * fade ) );

    ImDrawList *dl = ImGui::GetWindowDrawList();
    std::vector<ImVec2> scr;
    for ( size_t i = 0; i < s_hint.size(); ++i )
    {
        const std::vector<float> &poly = s_hint[i];
        const size_t n = poly.size() / 3;
        scr.resize( n );
        bool ok = true;
        for ( size_t k = 0; k < n && ok; ++k )
        {
            float sx, sy;
            ok = Pick_WorldToImage( &poly[k * 3], &sx, &sy );   // behind the eye: skip it
            scr[k] = ImVec2( imgMinX + sx, imgMinY + sy );
        }
        if ( !ok )
            continue;
        // ImGui's anti-aliased fill wants clockwise screen winding; back faces arrive reversed.
        float area2 = 0.0f;
        for ( size_t k = 0; k < n; ++k )
        {
            const ImVec2 &u = scr[k], &v = scr[( k + 1 ) % n];
            area2 += u.x * v.y - v.x * u.y;
        }
        if ( area2 < 0.0f )
            for ( size_t k = 0; k < n / 2; ++k )
            {
                const ImVec2 t = scr[k];
                scr[k] = scr[n - 1 - k];
                scr[n - 1 - k] = t;
            }
        dl->AddConvexPolyFilled( &scr[0], (int)n, fill );
        dl->AddPolyline( &scr[0], (int)n, edge, ImDrawFlags_Closed, 2.0f );
    }
}
