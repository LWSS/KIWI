#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// See kiwi_pack.h.  Single-threaded (editor main thread only).

#include "stdafx.h"
#include <math.h>
#include <string.h>
#include <float.h>
#include "kiwi_pack.h"

namespace
{

// Wire format (matches Vec3PackUnitVec / the vertex shader's decode):
//   byte[3] = scale s, encodeScale = 32385 / (s + 192)
//   byte[i] = (int)( n[i] * encodeScale + 127.5 ),  decoded[i] = (byte[i] - 127) / encodeScale
const int KPK_CANDIDATES = 6;          // scales tried, from the finest that fits

// Direct-mapped memo keyed on the raw input bits; a miss costs one pack.
const unsigned KPK_MEMO_SIZE = 4096;
struct kpkMemo_t
{
    unsigned      key[3];
    PackedUnitVec out;
    bool          valid;
};
kpkMemo_t s_memo[KPK_MEMO_SIZE];

unsigned KPK_Hash( const unsigned k[3] )
{
    unsigned h = 2166136261u;
    for ( int i = 0; i < 3; ++i )
    {
        h ^= k[i];
        h *= 16777619u;
    }
    h ^= h >> 15;
    return h & ( KPK_MEMO_SIZE - 1 );
}

PackedUnitVec KPK_Pack( const float *v )
{
    float n[3] = { v[0], v[1], v[2] };
    const float len2 = n[0] * n[0] + n[1] * n[1] + n[2] * n[2];
    if ( !( len2 > 1e-12f ) )
    {
        // Degenerate input (a collapsed triangle's tangent): straight up rather than the
        // all-zero encoding the shipped packer would assert on.
        n[0] = 0.0f; n[1] = 0.0f; n[2] = 1.0f;
    }
    else
    {
        const float inv = 1.0f / sqrtf( len2 );
        n[0] *= inv; n[1] *= inv; n[2] *= inv;
    }
    float maxAbs = fabsf( n[0] );
    if ( fabsf( n[1] ) > maxAbs ) maxAbs = fabsf( n[1] );
    if ( fabsf( n[2] ) > maxAbs ) maxAbs = fabsf( n[2] );

    // The finest scale whose bytes cannot wrap: |n| * encodeScale <= 127.5.
    int s0 = (int)ceilf( 254.0f * maxAbs - 192.0f );
    if ( s0 < 0 )   s0 = 0;
    if ( s0 > 255 ) s0 = 255;

    PackedUnitVec best;
    best.packed = 0;
    float bestDir = FLT_MAX, bestLen = FLT_MAX;
    for ( int k = 0; k < KPK_CANDIDATES; ++k )
    {
        const int s = s0 + k;
        if ( s > 255 )
            break;
        const float encodeScale = 32385.0f / (float)( s + 192 );
        const float decodeScale = 1.0f / encodeScale;
        unsigned char b[4];
        float d[3];
        for ( int i = 0; i < 3; ++i )
        {
            const int q = (int)( n[i] * encodeScale + 127.5f );
            b[i] = (unsigned char)q;
            d[i] = ( (float)b[i] - 127.0f ) * decodeScale;
        }
        b[3] = (unsigned char)s;
        const float dl = sqrtf( d[0] * d[0] + d[1] * d[1] + d[2] * d[2] );
        const float lenErr = fabsf( dl - 1.0f );
        if ( lenErr >= 0.001f || dl <= 0.0f )
            continue;
        const float dirErr = fabsf( ( d[0] * n[0] + d[1] * n[1] + d[2] * n[2] ) / dl - 1.0f );
        if ( dirErr < bestDir || ( dirErr == bestDir && lenErr < bestLen ) )
        {
            bestDir = dirErr;
            bestLen = lenErr;
            memcpy( &best.packed, b, 4 );
            if ( lenErr + dirErr == 0.0f )
                break;
        }
    }
    if ( best.packed == 0 )
        return Vec3PackUnitVec( n );       // no candidate fitted: the exhaustive search decides
    return best;
}

} // namespace

PackedUnitVec KiwiPack_UnitVec( const float *v )
{
    unsigned key[3];
    memcpy( key, v, sizeof( key ) );
    kpkMemo_t *m = &s_memo[KPK_Hash( key )];
    if ( m->valid && m->key[0] == key[0] && m->key[1] == key[1] && m->key[2] == key[2] )
        return m->out;
    m->key[0] = key[0]; m->key[1] = key[1]; m->key[2] = key[2];
    m->out   = KPK_Pack( v );
    m->valid = true;
    return m->out;
}
