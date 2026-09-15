#include <universal/q_shared.h>

#include "md4.h"

#include <cstring>

uint8_t PADDING[64] =
{
  128u,
  0u,
  0u,
  0u,
  0u,
  0u,
  0u,
  0u,
  0u,
  0u,
  0u,
  0u,
  0u,
  0u,
  0u,
  0u,
  0u,
  0u,
  0u,
  0u,
  0u,
  0u,
  0u,
  0u,
  0u,
  0u,
  0u,
  0u,
  0u,
  0u,
  0u,
  0u,
  0u,
  0u,
  0u,
  0u,
  0u,
  0u,
  0u,
  0u,
  0u,
  0u,
  0u,
  0u,
  0u,
  0u,
  0u,
  0u,
  0u,
  0u,
  0u,
  0u,
  0u,
  0u,
  0u,
  0u,
  0u,
  0u,
  0u,
  0u,
  0u,
  0u,
  0u,
  0u
}; // idb

/* Encodes input (UINT4) into output (unsigned char). Assumes len is a multiple of 4. */
void __cdecl Encode(uint8_t *output, uint *input, uint len)
{
    uint j; // [esp+0h] [ebp-8h]
    uint i; // [esp+4h] [ebp-4h]

    i = 0;
    for (j = 0; j < len; j += 4)
    {
        *(_WORD *)&output[j] = input[i];
        output[j + 2] = BYTE2(input[i]);
        output[j + 3] = HIBYTE(input[i++]);
    }
}

/* Decodes input (unsigned char) into output (UINT4). Assumes len is a multiple of 4. */
void __cdecl Decode(uint *output, uint8_t *input, uint len)
{
    uint j; // [esp+0h] [ebp-8h]
    uint i; // [esp+4h] [ebp-4h]

    i = 0;
    for (j = 0; j < len; j += 4)
        output[i++] = (input[j + 3] << 24) | (input[j + 2] << 16) | *(uint16_t *)&input[j];
}

/* MD4 initialization. Begins an MD4 operation, writing a new context. */
void __cdecl MD4Init(MD4_CTX *context)
{
    context->count[1] = 0;
    context->count[0] = 0;
    /* Load magic initialization constants.*/
    context->state[0] = 1732584193;
    context->state[1] = -271733879;
    context->state[2] = -1732584194;
    context->state[3] = 271733878;
}

/* MD4 block update operation. Continues an MD4 message-digest operation, processing another message block, and updating the context. */
void __cdecl MD4Update(MD4_CTX *context, uint8_t *input, uint inputLen)
{
    uint index; // [esp+4h] [ebp-8h]
    uint i; // [esp+8h] [ebp-4h]

    /* Compute number of bytes mod 64 */
    index = (context->count[0] >> 3) & 0x3F;
    /* Update number of bits */
    context->count[0] += 8 * inputLen;
    if (context->count[0] < 8 * inputLen)
        ++context->count[1];
    context->count[1] += inputLen >> 29;
    /* Transform as many times as possible.*/
    if (inputLen < 64 - index)
    {
        i = 0;
    }
    else
    {
        memcpy(&context->buffer[index], input, 64 - index);
        MD4Transform(context->state, context->buffer);
        for (i = 64 - index; i + 63 < inputLen; i += 64)
            MD4Transform(context->state, &input[i]);
        index = 0;
    }
    /* Buffer remaining input */
    memcpy(&context->buffer[index], &input[i], inputLen - i);
}

/* MD4 finalization. Ends an MD4 message-digest operation, writing the the message digest and zeroizing the context. */
void __cdecl MD4Final(uint8_t *digest, MD4_CTX *context)
{
    uint v2; // [esp+0h] [ebp-18h]
    uint8_t bits[8]; // [esp+8h] [ebp-10h] BYREF
    uint index; // [esp+14h] [ebp-4h]

    /* Save number of bits */
    Encode(bits, context->count, 8u);
    /* Pad out to 56 mod 64.*/
    index = (context->count[0] >> 3) & 0x3F;
    if (index >= 0x38)
        v2 = 120 - index;
    else
        v2 = 56 - index;
    MD4Update(context, PADDING, v2);
    /* Append length (before padding) */
    MD4Update(context, bits, 8u);
    /* Store state in digest */
    Encode(digest, context->state, 0x10u);
    /* Zeroize sensitive information.*/
    memset((uint8_t *)context, 0, sizeof(MD4_CTX));
}

/* MD4 basic transformation. Transforms state based on block. */
void __cdecl MD4Transform(uint *state, uint8_t *block)
{
    uint c; // [esp+0h] [ebp-50h]
    uint ca; // [esp+0h] [ebp-50h]
    uint cb; // [esp+0h] [ebp-50h]
    uint cc; // [esp+0h] [ebp-50h]
    uint cd; // [esp+0h] [ebp-50h]
    uint ce; // [esp+0h] [ebp-50h]
    uint cf; // [esp+0h] [ebp-50h]
    uint cg; // [esp+0h] [ebp-50h]
    uint ch; // [esp+0h] [ebp-50h]
    uint ci; // [esp+0h] [ebp-50h]
    uint cj; // [esp+0h] [ebp-50h]
    uint ck; // [esp+0h] [ebp-50h]
    uint cl; // [esp+0h] [ebp-50h]
    uint cm; // [esp+0h] [ebp-50h]
    uint cn; // [esp+0h] [ebp-50h]
    uint co; // [esp+0h] [ebp-50h]
    uint cp; // [esp+0h] [ebp-50h]
    uint cq; // [esp+0h] [ebp-50h]
    uint cr; // [esp+0h] [ebp-50h]
    uint cs; // [esp+0h] [ebp-50h]
    uint ct; // [esp+0h] [ebp-50h]
    uint cu; // [esp+0h] [ebp-50h]
    uint cv; // [esp+0h] [ebp-50h]
    uint cw; // [esp+0h] [ebp-50h]
    uint cx; // [esp+0h] [ebp-50h]
    uint d; // [esp+4h] [ebp-4Ch]
    uint da; // [esp+4h] [ebp-4Ch]
    uint db; // [esp+4h] [ebp-4Ch]
    uint dc; // [esp+4h] [ebp-4Ch]
    uint dd; // [esp+4h] [ebp-4Ch]
    uint de; // [esp+4h] [ebp-4Ch]
    uint df; // [esp+4h] [ebp-4Ch]
    uint dg; // [esp+4h] [ebp-4Ch]
    uint dh; // [esp+4h] [ebp-4Ch]
    uint di; // [esp+4h] [ebp-4Ch]
    uint dj; // [esp+4h] [ebp-4Ch]
    uint dk; // [esp+4h] [ebp-4Ch]
    uint dl; // [esp+4h] [ebp-4Ch]
    uint dm; // [esp+4h] [ebp-4Ch]
    uint dn; // [esp+4h] [ebp-4Ch]
    uint dp; // [esp+4h] [ebp-4Ch]
    uint dq; // [esp+4h] [ebp-4Ch]
    uint dr; // [esp+4h] [ebp-4Ch]
    uint ds; // [esp+4h] [ebp-4Ch]
    uint dt; // [esp+4h] [ebp-4Ch]
    uint du; // [esp+4h] [ebp-4Ch]
    uint dv; // [esp+4h] [ebp-4Ch]
    uint dw; // [esp+4h] [ebp-4Ch]
    uint dx; // [esp+4h] [ebp-4Ch]
    uint dy; // [esp+4h] [ebp-4Ch]
    uint b; // [esp+8h] [ebp-48h]
    uint ba; // [esp+8h] [ebp-48h]
    uint bb; // [esp+8h] [ebp-48h]
    uint bc; // [esp+8h] [ebp-48h]
    uint bd; // [esp+8h] [ebp-48h]
    uint be; // [esp+8h] [ebp-48h]
    uint bf; // [esp+8h] [ebp-48h]
    uint bg; // [esp+8h] [ebp-48h]
    uint bh; // [esp+8h] [ebp-48h]
    uint bi; // [esp+8h] [ebp-48h]
    uint bj; // [esp+8h] [ebp-48h]
    uint bk; // [esp+8h] [ebp-48h]
    uint bl; // [esp+8h] [ebp-48h]
    uint bm; // [esp+8h] [ebp-48h]
    uint bn; // [esp+8h] [ebp-48h]
    uint bo; // [esp+8h] [ebp-48h]
    uint bp; // [esp+8h] [ebp-48h]
    uint bq; // [esp+8h] [ebp-48h]
    uint br; // [esp+8h] [ebp-48h]
    uint bs; // [esp+8h] [ebp-48h]
    uint bt; // [esp+8h] [ebp-48h]
    uint bu; // [esp+8h] [ebp-48h]
    uint bv; // [esp+8h] [ebp-48h]
    uint bw; // [esp+8h] [ebp-48h]
    uint a; // [esp+Ch] [ebp-44h]
    uint aa; // [esp+Ch] [ebp-44h]
    uint ab; // [esp+Ch] [ebp-44h]
    uint ac; // [esp+Ch] [ebp-44h]
    uint ad; // [esp+Ch] [ebp-44h]
    uint ae; // [esp+Ch] [ebp-44h]
    uint af; // [esp+Ch] [ebp-44h]
    uint ag; // [esp+Ch] [ebp-44h]
    uint ah; // [esp+Ch] [ebp-44h]
    uint ai; // [esp+Ch] [ebp-44h]
    uint aj; // [esp+Ch] [ebp-44h]
    uint ak; // [esp+Ch] [ebp-44h]
    uint al; // [esp+Ch] [ebp-44h]
    uint am; // [esp+Ch] [ebp-44h]
    uint an; // [esp+Ch] [ebp-44h]
    uint ao; // [esp+Ch] [ebp-44h]
    uint ap; // [esp+Ch] [ebp-44h]
    uint aq; // [esp+Ch] [ebp-44h]
    uint ar; // [esp+Ch] [ebp-44h]
    uint as; // [esp+Ch] [ebp-44h]
    uint at; // [esp+Ch] [ebp-44h]
    uint au; // [esp+Ch] [ebp-44h]
    uint av; // [esp+Ch] [ebp-44h]
    uint aw; // [esp+Ch] [ebp-44h]
    uint ax; // [esp+Ch] [ebp-44h]
    uint x[16]; // [esp+10h] [ebp-40h] BYREF

    a = *state;
    b = state[1];
    c = state[2];
    d = state[3];
    Decode(x, block, 0x40u);
    /* Round 1 */
    aa = a + x[0] + (d & ~b | c & b);
    ab = (aa >> 29) | (8 * aa); /* 1 */
    da = d + x[1] + (c & ~ab | b & ab);
    db = (da >> 25) | (da << 7); /* 2 */
    ca = c + x[2] + (b & ~db | ab & db);
    cb = (ca >> 21) | (ca << 11); /* 3 */
    ba = b + x[3] + (ab & ~cb | db & cb);
    bb = (ba >> 13) | (ba << 19); /* 4 */
    ac = ab + x[4] + (db & ~bb | cb & bb);
    ad = (ac >> 29) | (8 * ac); /* 5 */
    dc = db + x[5] + (cb & ~ad | bb & ad);
    dd = (dc >> 25) | (dc << 7); /* 6 */
    cc = cb + x[6] + (bb & ~dd | ad & dd);
    cd = (cc >> 21) | (cc << 11); /* 7 */
    bc = bb + x[7] + (ad & ~cd | dd & cd);
    bd = (bc >> 13) | (bc << 19); /* 8 */
    ae = ad + x[8] + (dd & ~bd | cd & bd);
    af = (ae >> 29) | (8 * ae); /* 9 */
    de = dd + x[9] + (cd & ~af | bd & af);
    df = (de >> 25) | (de << 7); /* 10 */
    ce = cd + x[10] + (bd & ~df | af & df);
    cf = (ce >> 21) | (ce << 11); /* 11 */
    be = bd + x[11] + (af & ~cf | df & cf);
    bf = (be >> 13) | (be << 19); /* 12 */
    ag = af + x[12] + (df & ~bf | cf & bf);
    ah = (ag >> 29) | (8 * ag); /* 13 */
    dg = df + x[13] + (cf & ~ah | bf & ah);
    dh = (dg >> 25) | (dg << 7); /* 14 */
    cg = cf + x[14] + (bf & ~dh | ah & dh);
    ch = (cg >> 21) | (cg << 11); /* 15 */
    bg = bf + x[15] + (ah & ~ch | dh & ch);
    bh = (bg >> 13) | (bg << 19); /* 16 */
    /* Round 2 */
    ai = ah + x[0] + (dh & ch | dh & bh | ch & bh) + 1518500249;
    aj = (ai >> 29) | (8 * ai); /* 17 */
    di = dh + x[4] + (ch & bh | ch & aj | bh & aj) + 1518500249;
    dj = (di >> 27) | (32 * di); /* 18 */
    ci = ch + x[8] + (bh & aj | bh & dj | aj & dj) + 1518500249;
    cj = (ci >> 23) | (ci << 9); /* 19 */
    bi = bh + x[12] + (aj & dj | aj & cj | dj & cj) + 1518500249;
    bj = (bi >> 19) | (bi << 13); /* 20 */
    ak = aj + x[1] + (dj & cj | dj & bj | cj & bj) + 1518500249;
    al = (ak >> 29) | (8 * ak); /* 21 */
    dk = dj + x[5] + (cj & bj | cj & al | bj & al) + 1518500249;
    dl = (dk >> 27) | (32 * dk); /* 22 */
    ck = cj + x[9] + (bj & al | bj & dl | al & dl) + 1518500249;
    cl = (ck >> 23) | (ck << 9); /* 23 */
    bk = bj + x[13] + (al & dl | al & cl | dl & cl) + 1518500249;
    bl = (bk >> 19) | (bk << 13); /* 24 */
    am = al + x[2] + (dl & cl | dl & bl | cl & bl) + 1518500249;
    an = (am >> 29) | (8 * am); /* 25 */
    dm = dl + x[6] + (cl & bl | cl & an | bl & an) + 1518500249;
    dn = (dm >> 27) | (32 * dm); /* 26 */
    cm = cl + x[10] + (bl & an | bl & dn | an & dn) + 1518500249;
    cn = (cm >> 23) | (cm << 9); /* 27 */
    bm = bl + x[14] + (an & dn | an & cn | dn & cn) + 1518500249;
    bn = (bm >> 19) | (bm << 13); /* 28 */
    ao = an + x[3] + (dn & cn | dn & bn | cn & bn) + 1518500249;
    ap = (ao >> 29) | (8 * ao); /* 29 */
    dp = dn + x[7] + (cn & bn | cn & ap | bn & ap) + 1518500249;
    dq = (dp >> 27) | (32 * dp); /* 30 */
    co = cn + x[11] + (bn & ap | bn & dq | ap & dq) + 1518500249;
    cp = (co >> 23) | (co << 9); /* 31 */
    bo = bn + x[15] + (ap & dq | ap & cp | dq & cp) + 1518500249;
    bp = (bo >> 19) | (bo << 13); /* 32 */
    /* Round 3 */
    aq = ap + x[0] + (dq ^ cp ^ bp) + 1859775393;
    ar = (aq >> 29) | (8 * aq); /* 33 */
    dr = dq + x[8] + (cp ^ bp ^ ar) + 1859775393;
    ds = (dr >> 23) | (dr << 9); /* 34 */
    cq = cp + x[4] + (bp ^ ar ^ ds) + 1859775393;
    cr = (cq >> 21) | (cq << 11); /* 35 */
    bq = bp + x[12] + (ar ^ ds ^ cr) + 1859775393;
    br = (bq >> 17) | (bq << 15); /* 36 */
    as = ar + x[2] + (ds ^ cr ^ br) + 1859775393;
    at = (as >> 29) | (8 * as); /* 37 */
    dt = ds + x[10] + (cr ^ br ^ at) + 1859775393;
    du = (dt >> 23) | (dt << 9); /* 38 */
    cs = cr + x[6] + (br ^ at ^ du) + 1859775393;
    ct = (cs >> 21) | (cs << 11); /* 39 */
    bs = br + x[14] + (at ^ du ^ ct) + 1859775393;
    bt = (bs >> 17) | (bs << 15); /* 40 */
    au = at + x[1] + (du ^ ct ^ bt) + 1859775393;
    av = (au >> 29) | (8 * au); /* 41 */
    dv = du + x[9] + (ct ^ bt ^ av) + 1859775393;
    dw = (dv >> 23) | (dv << 9); /* 42 */
    cu = ct + x[5] + (bt ^ av ^ dw) + 1859775393;
    cv = (cu >> 21) | (cu << 11); /* 43 */
    bu = bt + x[13] + (av ^ dw ^ cv) + 1859775393;
    bv = (bu >> 17) | (bu << 15); /* 44 */
    aw = av + x[3] + (dw ^ cv ^ bv) + 1859775393;
    ax = (aw >> 29) | (8 * aw); /* 45 */
    dx = dw + x[11] + (cv ^ bv ^ ax) + 1859775393;
    dy = (dx >> 23) | (dx << 9); /* 46 */
    cw = cv + x[7] + (bv ^ ax ^ dy) + 1859775393;
    cx = (cw >> 21) | (cw << 11); /* 47 */
    bw = bv + x[15] + (ax ^ dy ^ cx) + 1859775393; /* 48 */
    *state += ax;
    state[1] += (bw >> 17) | (bw << 15);
    state[2] += cx;
    state[3] += dy;
    /* Zeroize sensitive information.*/
    memset((uint8_t *)x, 0, sizeof(x));
}



void __cdecl Com_BlockChecksum128Cat(
    uint8_t *buffer0,
    uint length0,
    uint8_t *buffer1,
    uint length1,
    uint8_t *outChecksum)
{
    MD4_CTX ctx; // [esp+0h] [ebp-60h] BYREF

    MD4Init(&ctx);
    MD4Update(&ctx, buffer0, length0);
    MD4Update(&ctx, buffer1, length1);
    MD4Final(outChecksum, &ctx);
}

void __cdecl Com_BlockChecksum128(uint8_t *buffer, uint length, int key, uint8_t *outChecksum)
{
    MD4_CTX ctx; // [esp+0h] [ebp-60h] BYREF

    MD4Init(&ctx);
    MD4Update(&ctx, (byte*)&key, 4u);
    MD4Update(&ctx, buffer, length);
    MD4Final(outChecksum, &ctx);
}