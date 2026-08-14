/*
 * uv_repack.c - Per-island UV atlas repacking (Frostbite-style bitmap packer).
 *
 * Mirrors the proven standalone packer (uv_packer_test.c). One chart per
 * pixel-flood-fill island (not per surface), so charts are tight and packing
 * fits ~all real lightmap content on 1 page at high efficiency.
 *
 * Pipeline:
 *   1. Rasterize triangle UVs to a 512x512 grid.
 *   2. 4-connected flood-fill → island labels.
 *   3. Triangle-driven merge: any component a triangle's rasterized pixels
 *      span (other than the dominant one) gets unioned into the dominant.
 *   4. Per-island bitmask + 1px dilation padding.
 *   5. Multi-page first-fit pack with 8 orientations (4 rot x 2 mirror).
 *      Hard cap at 31 pages (engine max).
 *   6. Per-vertex UV rewrite: each vertex's atlas pixel determines its island,
 *      and the vertex shifts by THAT island's offset (not its surface's), so
 *      multi-island surfaces are handled correctly. Requires no vertex be
 *      shared across islands - true on cod2 maps (verified).
 *   7. Per-surface lightmapIndex set from the dominant island of its tris.
 *      Patch g_triangles[].lightmapIdx to match.
 */

#include "cod2rad64.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define LM_SIZE 512
#define LM_MAX_PAGES 31

typedef struct {
    unsigned char *mask;
    int w, h;
    int offX, offY, pixels, placed;
    int newPage, newX, newY, orient;
    unsigned char *masks[8];
    int dims[8][2];
} Chart_t;

static void RasterizeTriangle(unsigned char *grid, int gridW, int gridH,
                              float x0, float y0, float x1, float y1, float x2, float y2,
                              unsigned char val)
{
    int minY = (int)floorf(y0<y1?(y0<y2?y0:y2):(y1<y2?y1:y2));
    int maxY = (int)ceilf(y0>y1?(y0>y2?y0:y2):(y1>y2?y1:y2));
    int scanY;
    if (minY < 0) minY = 0;
    if (maxY >= gridH) maxY = gridH - 1;

    for (scanY = minY; scanY <= maxY; scanY++)
    {
        float fy = (float)scanY + 0.5f;
        float xI[6]; int nX = 0;
        float ex[3][2] = {{x0,y0},{x1,y1},{x2,y2}};
        int e;
        for (e = 0; e < 3; e++)
        {
            float ay = ex[e][1], by = ex[(e+1)%3][1];
            if ((ay <= fy && by > fy) || (by <= fy && ay > fy))
            {
                float t = (fy - ay) / (by - ay);
                xI[nX++] = ex[e][0] + t * (ex[(e+1)%3][0] - ex[e][0]);
            }
        }
        if (nX >= 2)
        {
            int xMin, xMax, fx;
            if (xI[0] > xI[1]) { float tmp=xI[0]; xI[0]=xI[1]; xI[1]=tmp; }
            xMin = (int)floorf(xI[0]);
            xMax = (int)ceilf(xI[1]);
            if (xMin < 0) xMin = 0;
            if (xMax >= gridW) xMax = gridW - 1;
            for (fx = xMin; fx <= xMax; fx++)
                grid[scanY * gridW + fx] = val;
        }
    }
}

void UVRepack(void)
{
    extern Triangle_t g_triangles[];
    extern int g_triCount;

    DrawVert_t *allVerts = (DrawVert_t *)bspDrawVerts;
    unsigned char *pixGrid;
    int *labels, *stack;
    int numIslands = 0;
    Chart_t *charts = NULL;
    int s, t, i, j, v;
    int x, y;

    Com_Printf("UVRepack: starting (%d surfaces, %d vertices)\n",
               numBSPTriSoups, numBSPDrawVerts);

    /* === Step 1: rasterize all lit triangles' UVs to 512x512 grid === */
    pixGrid = (unsigned char *)calloc(LM_SIZE * LM_SIZE, 1);
    for (s = 0; s < numBSPTriSoups; s++)
    {
        BspTriSoup_t *ts = &bspTriangles[s];
        int numTris;
        if (ts->lightmapIndex == 0x1F) continue;
        numTris = ts->indexCount / 3;
        for (t = 0; t < numTris; t++)
        {
            int idx = ts->firstIndex + t * 3;
            int base = ts->firstVertex;
            int va = base + bspDrawIndexes[idx+0];
            int vb = base + bspDrawIndexes[idx+1];
            int vc = base + bspDrawIndexes[idx+2];
            float x0, y0, x1, y1, x2, y2;
            if (va >= numBSPDrawVerts || vb >= numBSPDrawVerts || vc >= numBSPDrawVerts) continue;
            x0 = allVerts[va].lmCoord[0] * 511.0f; y0 = allVerts[va].lmCoord[1] * 511.0f;
            x1 = allVerts[vb].lmCoord[0] * 511.0f; y1 = allVerts[vb].lmCoord[1] * 511.0f;
            x2 = allVerts[vc].lmCoord[0] * 511.0f; y2 = allVerts[vc].lmCoord[1] * 511.0f;
            RasterizeTriangle(pixGrid, LM_SIZE, LM_SIZE, x0, y0, x1, y1, x2, y2, 1);
        }
    }

    /* === Step 2: 4-connected flood-fill to find islands === */
    labels = (int *)malloc(LM_SIZE * LM_SIZE * sizeof(int));
    stack  = (int *)malloc(LM_SIZE * LM_SIZE * sizeof(int));
    for (i = 0; i < LM_SIZE*LM_SIZE; i++) labels[i] = -1;

    for (y = 0; y < LM_SIZE; y++)
    {
        for (x = 0; x < LM_SIZE; x++)
        {
            int top;
            if (!pixGrid[y*LM_SIZE+x] || labels[y*LM_SIZE+x] >= 0) continue;
            top = 0;
            stack[top++] = y*LM_SIZE+x;
            labels[y*LM_SIZE+x] = numIslands;
            while (top > 0)
            {
                int idx = stack[--top];
                int cy = idx / LM_SIZE, cx = idx % LM_SIZE;
                int dirs[4][2] = {{0,-1},{0,1},{-1,0},{1,0}};
                int d;
                for (d = 0; d < 4; d++)
                {
                    int nx = cx+dirs[d][0], ny = cy+dirs[d][1];
                    if (nx >= 0 && nx < LM_SIZE && ny >= 0 && ny < LM_SIZE)
                    {
                        int ni = ny*LM_SIZE+nx;
                        if (pixGrid[ni] && labels[ni] == -1)
                        {
                            labels[ni] = numIslands;
                            stack[top++] = ni;
                        }
                    }
                }
            }
            numIslands++;
        }
    }
    free(stack);
    Com_Printf("UVRepack: %d flood-fill islands\n", numIslands);

    /* === Step 3: triangle-driven merge — eliminate orphan components a
       triangle's rasterized pixels span beyond the dominant one. */
    {
        int *uf = (int *)malloc(numIslands * sizeof(int));
        unsigned char *trMask = (unsigned char *)malloc(LM_SIZE * LM_SIZE);
        int oldCount = numIslands;
        int mergeCount = 0;

        for (i = 0; i < numIslands; i++) uf[i] = i;

        for (s = 0; s < numBSPTriSoups; s++)
        {
            BspTriSoup_t *ts = &bspTriangles[s];
            int numTris;
            if (ts->lightmapIndex == 0x1F) continue;
            numTris = ts->indexCount / 3;
            for (t = 0; t < numTris; t++)
            {
                int idx = ts->firstIndex + t * 3;
                int base = ts->firstVertex;
                int va = base + bspDrawIndexes[idx+0];
                int vb = base + bspDrawIndexes[idx+1];
                int vc = base + bspDrawIndexes[idx+2];
                float xv[3], yv[3];
                int xMin, xMax, yMin, yMax, rx, ry;
                int hLbl[8]; int hCnt[8]; int hN = 0;
                int domLbl = -1, domCnt = 0;
                int k;

                if (va >= numBSPDrawVerts || vb >= numBSPDrawVerts || vc >= numBSPDrawVerts) continue;

                xv[0] = allVerts[va].lmCoord[0]*511.0f; yv[0] = allVerts[va].lmCoord[1]*511.0f;
                xv[1] = allVerts[vb].lmCoord[0]*511.0f; yv[1] = allVerts[vb].lmCoord[1]*511.0f;
                xv[2] = allVerts[vc].lmCoord[0]*511.0f; yv[2] = allVerts[vc].lmCoord[1]*511.0f;
                xMin = (int)floorf(xv[0]<xv[1]?(xv[0]<xv[2]?xv[0]:xv[2]):(xv[1]<xv[2]?xv[1]:xv[2]));
                xMax = (int)ceilf (xv[0]>xv[1]?(xv[0]>xv[2]?xv[0]:xv[2]):(xv[1]>xv[2]?xv[1]:xv[2]));
                yMin = (int)floorf(yv[0]<yv[1]?(yv[0]<yv[2]?yv[0]:yv[2]):(yv[1]<yv[2]?yv[1]:yv[2]));
                yMax = (int)ceilf (yv[0]>yv[1]?(yv[0]>yv[2]?yv[0]:yv[2]):(yv[1]>yv[2]?yv[1]:yv[2]));
                if (xMin < 0) xMin = 0; if (xMax > 511) xMax = 511;
                if (yMin < 0) yMin = 0; if (yMax > 511) yMax = 511;
                for (ry = yMin; ry <= yMax; ry++)
                    for (rx = xMin; rx <= xMax; rx++)
                        trMask[ry*LM_SIZE+rx] = 0;
                RasterizeTriangle(trMask, LM_SIZE, LM_SIZE, xv[0], yv[0], xv[1], yv[1], xv[2], yv[2], 1);

                for (ry = yMin; ry <= yMax; ry++)
                    for (rx = xMin; rx <= xMax; rx++)
                        if (trMask[ry*LM_SIZE+rx] && labels[ry*LM_SIZE+rx] >= 0)
                        {
                            int lbl = labels[ry*LM_SIZE+rx];
                            int kk;
                            for (kk = 0; kk < hN; kk++) if (hLbl[kk] == lbl) break;
                            if (kk < hN) hCnt[kk]++;
                            else if (hN < 8) { hLbl[hN] = lbl; hCnt[hN] = 1; hN++; }
                        }
                if (hN < 2) continue;

                for (k = 0; k < hN; k++)
                    if (hCnt[k] > domCnt) { domCnt = hCnt[k]; domLbl = hLbl[k]; }

                for (k = 0; k < hN; k++)
                {
                    int ra, rb;
                    if (hLbl[k] == domLbl) continue;
                    ra = hLbl[k]; rb = domLbl;
                    while (uf[ra] != ra) ra = uf[ra];
                    while (uf[rb] != rb) rb = uf[rb];
                    if (ra != rb) { uf[ra] = rb; mergeCount++; }
                }
            }
        }

        for (i = 0; i < numIslands; i++)
        {
            int r = i;
            while (uf[r] != r) r = uf[r];
            uf[i] = r;
        }
        {
            int *remap = (int *)malloc(numIslands * sizeof(int));
            int newCount = 0;
            for (i = 0; i < numIslands; i++) remap[i] = -1;
            for (i = 0; i < numIslands; i++)
            {
                int r = uf[i];
                if (remap[r] < 0) remap[r] = newCount++;
            }
            for (i = 0; i < LM_SIZE*LM_SIZE; i++)
                if (labels[i] >= 0) labels[i] = remap[uf[labels[i]]];
            Com_Printf("UVRepack: triangle merge %d→%d islands (%d merges)\n",
                       oldCount, newCount, mergeCount);
            numIslands = newCount;
            free(remap);
        }
        free(uf);
        free(trMask);
    }

    /* === Step 4: extract per-island bbox + bitmask, 1px dilation padding === */
    charts = (Chart_t *)calloc(numIslands, sizeof(Chart_t));
    for (j = 0; j < numIslands; j++)
    {
        charts[j].offX = LM_SIZE; charts[j].offY = LM_SIZE;
        charts[j].newPage = -1;
    }
    for (i = 0; i < LM_SIZE*LM_SIZE; i++)
    {
        if (labels[i] >= 0)
        {
            int lbl = labels[i];
            int px = i % LM_SIZE, py = i / LM_SIZE;
            charts[lbl].pixels++;
            if (px < charts[lbl].offX) charts[lbl].offX = px;
            if (py < charts[lbl].offY) charts[lbl].offY = py;
            if (px - charts[lbl].offX + 1 > charts[lbl].w) charts[lbl].w = px - charts[lbl].offX + 1;
            if (py - charts[lbl].offY + 1 > charts[lbl].h) charts[lbl].h = py - charts[lbl].offY + 1;
        }
    }
    /* compact mask first (before dilation) */
    for (j = 0; j < numIslands; j++)
    {
        if (charts[j].pixels == 0) continue;
        charts[j].masks[0] = (unsigned char *)calloc(charts[j].w * charts[j].h, 1);
    }
    for (i = 0; i < LM_SIZE*LM_SIZE; i++)
    {
        if (labels[i] >= 0)
        {
            int lbl = labels[i];
            int lx = i % LM_SIZE - charts[lbl].offX;
            int ly = i / LM_SIZE - charts[lbl].offY;
            if (charts[lbl].masks[0])
                charts[lbl].masks[0][ly * charts[lbl].w + lx] = 1;
        }
    }
    /* dilate: w,h grow by 2; original pixel (lx,ly) becomes (lx+1,ly+1) and
       a 3x3 stamp fills the surroundings */
    for (j = 0; j < numIslands; j++)
    {
        int dw, dh, mx, my, dx, dy;
        unsigned char *src, *dst;
        if (!charts[j].masks[0]) continue;
        dw = charts[j].w + 2;
        dh = charts[j].h + 2;
        dst = (unsigned char *)calloc(dw * dh, 1);
        src = charts[j].masks[0];
        for (my = 0; my < charts[j].h; my++)
            for (mx = 0; mx < charts[j].w; mx++)
                if (src[my * charts[j].w + mx])
                    for (dy = 0; dy <= 2; dy++)
                        for (dx = 0; dx <= 2; dx++)
                            dst[(my+dy)*dw + (mx+dx)] = 1;
        free(src);
        charts[j].masks[0] = dst;
        charts[j].w = dw;
        charts[j].h = dh;
        charts[j].dims[0][0] = dw;
        charts[j].dims[0][1] = dh;
    }
    /* generate orientations 1..7 */
    for (j = 0; j < numIslands; j++)
    {
        int ow, oh, o, lx, ly;
        unsigned char *src;
        if (!charts[j].masks[0]) continue;
        ow = charts[j].w;
        oh = charts[j].h;
        src = charts[j].masks[0];
        for (o = 1; o < 8; o++)
        {
            int tw = (o & 1) ? oh : ow;
            int th = (o & 1) ? ow : oh;
            charts[j].masks[o] = (unsigned char *)calloc(tw * th, 1);
            charts[j].dims[o][0] = tw;
            charts[j].dims[o][1] = th;
            for (ly = 0; ly < oh; ly++)
                for (lx = 0; lx < ow; lx++)
                    if (src[ly*ow + lx])
                    {
                        int tx, ty;
                        switch (o)
                        {
                            case 1: tx = oh-1-ly; ty = lx; break;
                            case 2: tx = ow-1-lx; ty = oh-1-ly; break;
                            case 3: tx = ly;      ty = ow-1-lx; break;
                            case 4: tx = ow-1-lx; ty = ly; break;
                            case 5: tx = oh-1-ly; ty = ow-1-lx; break;
                            case 6: tx = lx;      ty = oh-1-ly; break;
                            default: tx = ly;     ty = lx; break;
                        }
                        charts[j].masks[o][ty*tw + tx] = 1;
                    }
        }
    }

    /* === Step 5: multi-page first-fit with 8 orientations === */
    {
        int *sortOrder = (int *)malloc(numIslands * sizeof(int));
        unsigned char *atlasPages[LM_MAX_PAGES] = {0};
        int activePages = 0, placed = 0, unplaced = 0;
        int orientCount[8] = {0};

        for (j = 0; j < numIslands; j++) sortOrder[j] = j;
        /* sort by content area, biggest first */
        for (i = 0; i < numIslands - 1; i++)
            for (j = i + 1; j < numIslands; j++)
                if (charts[sortOrder[j]].pixels > charts[sortOrder[i]].pixels)
                { int tmp = sortOrder[i]; sortOrder[i] = sortOrder[j]; sortOrder[j] = tmp; }

        for (i = 0; i < numIslands; i++)
        {
            int idx = sortOrder[i];
            Chart_t *c = &charts[idx];
            int p, pickedPage = -1, pickedX = -1, pickedY = -1, pickedOrient = -1;

            if (!c->masks[0] || c->pixels == 0) continue;

            for (p = 0; p < activePages && pickedPage < 0; p++)
            {
                unsigned char *atlas = atlasPages[p];
                int bestY = 999999, bestX = 999999, bestOrient = -1;
                int o;
                for (o = 0; o < 8; o++)
                {
                    int oW = c->dims[o][0], oH = c->dims[o][1];
                    unsigned char *oMask = c->masks[o];
                    int foundX = -1, foundY = -1;
                    int ay, ax;
                    if (oW > LM_SIZE || oH > LM_SIZE) continue;
                    for (ay = 0; ay <= LM_SIZE - oH && foundX < 0; ay++)
                    {
                        if (ay > bestY) break;
                        for (ax = 0; ax <= LM_SIZE - oW; ax++)
                        {
                            int fits = 1, my, mx;
                            if (ay == bestY && ax >= bestX) break;
                            for (my = 0; my < oH && fits; my++)
                                for (mx = 0; mx < oW && fits; mx++)
                                    if (oMask[my*oW + mx] && atlas[(ay+my)*LM_SIZE + (ax+mx)])
                                        fits = 0;
                            if (fits) { foundX = ax; foundY = ay; break; }
                        }
                    }
                    if (foundX >= 0 && (foundY < bestY || (foundY == bestY && foundX < bestX)))
                    {
                        bestY = foundY; bestX = foundX; bestOrient = o;
                        if (bestY == 0 && bestX == 0) break;
                    }
                }
                if (bestOrient >= 0)
                { pickedPage = p; pickedX = bestX; pickedY = bestY; pickedOrient = bestOrient; }
            }

            if (pickedPage < 0 && activePages < LM_MAX_PAGES)
            {
                int o, bestO = -1;
                atlasPages[activePages] = (unsigned char *)calloc(LM_SIZE * LM_SIZE, 1);
                activePages++;
                pickedPage = activePages - 1;
                for (o = 0; o < 8; o++)
                    if (c->dims[o][0] <= LM_SIZE && c->dims[o][1] <= LM_SIZE) { bestO = o; break; }
                if (bestO < 0) { pickedPage = -1; }
                else { pickedX = 0; pickedY = 0; pickedOrient = bestO; }
            }

            if (pickedPage >= 0)
            {
                int my, mx;
                int oW = c->dims[pickedOrient][0], oH = c->dims[pickedOrient][1];
                unsigned char *oMask = c->masks[pickedOrient];
                unsigned char *atlas = atlasPages[pickedPage];
                for (my = 0; my < oH; my++)
                    for (mx = 0; mx < oW; mx++)
                        if (oMask[my*oW + mx])
                            atlas[(pickedY+my)*LM_SIZE + (pickedX+mx)] = 1;
                c->placed = 1;
                c->newPage = pickedPage;
                c->newX = pickedX;
                c->newY = pickedY;
                c->orient = pickedOrient;
                c->mask = oMask;
                c->w = oW;
                c->h = oH;
                placed++;
                orientCount[pickedOrient]++;
            }
            else
            {
                unplaced++;
            }
        }

        Com_Printf("UVRepack: packed %d/%d islands on %d pages (unplaced %d)\n",
                   placed, numIslands, activePages, unplaced);
        Com_Printf("UVRepack: orient o0=%d o1=%d o2=%d o3=%d o4=%d o5=%d o6=%d o7=%d\n",
                   orientCount[0], orientCount[1], orientCount[2], orientCount[3],
                   orientCount[4], orientCount[5], orientCount[6], orientCount[7]);
        for (i = 0; i < activePages; i++)
        {
            int used = 0, k;
            for (k = 0; k < LM_SIZE*LM_SIZE; k++) if (atlasPages[i][k]) used++;
            Com_Printf("UVRepack: page %d %.1f%% (%d/%d)\n",
                       i, 100.0 * used / (LM_SIZE*LM_SIZE), used, LM_SIZE*LM_SIZE);
        }

        for (i = 0; i < activePages; i++) free(atlasPages[i]);
        free(sortOrder);

        /* update global page count for downstream alloc */
        if (activePages > g_lightmapSize) g_lightmapSize = activePages;
        if (g_lightmapSize < 1) g_lightmapSize = 1;
    }

    /* === Step 6: per-vertex UV rewrite using vertex's OWN island === */
    {
        int *islandOfVert = (int *)malloc(numBSPDrawVerts * sizeof(int));
        char *vertDone = (char *)calloc(numBSPDrawVerts, 1);
        int rewritten = 0, vertNoIsland = 0, vertNotPlaced = 0;
        int origW, origH, dx, dy;

        for (i = 0; i < numBSPDrawVerts; i++) islandOfVert[i] = -1;
        /* assign island per vertex via pixel lookup */
        for (s = 0; s < numBSPTriSoups; s++)
        {
            BspTriSoup_t *ts = &bspTriangles[s];
            if (ts->lightmapIndex == 0x1F) continue;
            for (v = 0; v < ts->vertexCount; v++)
            {
                int vi = ts->firstVertex + v;
                int px, py, lbl;
                if (vi >= numBSPDrawVerts || islandOfVert[vi] >= 0) continue;
                px = (int)(allVerts[vi].lmCoord[0] * 511.0f);
                py = (int)(allVerts[vi].lmCoord[1] * 511.0f);
                if (px < 0) px = 0; if (px > 511) px = 511;
                if (py < 0) py = 0; if (py > 511) py = 511;
                lbl = labels[py*LM_SIZE+px];
                /* 3x3 fallback - vertex pixel may land just off the rasterized region */
                for (dy = -1; dy <= 1 && lbl < 0; dy++)
                    for (dx = -1; dx <= 1 && lbl < 0; dx++)
                    {
                        int nx2 = px+dx, ny2 = py+dy;
                        if (nx2 >= 0 && nx2 < LM_SIZE && ny2 >= 0 && ny2 < LM_SIZE && labels[ny2*LM_SIZE+nx2] >= 0)
                            lbl = labels[ny2*LM_SIZE+nx2];
                    }
                islandOfVert[vi] = lbl;
            }
        }

        /* apply transform per vertex: shift + rotate using ITS island */
        for (s = 0; s < numBSPTriSoups; s++)
        {
            BspTriSoup_t *ts = &bspTriangles[s];
            if (ts->lightmapIndex == 0x1F) continue;
            for (v = 0; v < ts->vertexCount; v++)
            {
                int vi = ts->firstVertex + v;
                int isle;
                Chart_t *c;
                double lx, ly, tx, ty, newU, newV;

                if (vi >= numBSPDrawVerts || vertDone[vi]) continue;
                vertDone[vi] = 1;

                isle = islandOfVert[vi];
                if (isle < 0) { vertNoIsland++; continue; }
                c = &charts[isle];
                if (!c->placed) { vertNotPlaced++; continue; }

                origW = c->dims[0][0];
                origH = c->dims[0][1];

                /* original pixel relative to island origin (+1 = dilation pad) */
                lx = (double)allVerts[vi].lmCoord[0] * 511.0 - (double)c->offX + 1.0;
                ly = (double)allVerts[vi].lmCoord[1] * 511.0 - (double)c->offY + 1.0;
                if (lx < 0.0) lx = 0.0; else if (lx > (double)(origW - 1)) lx = (double)(origW - 1);
                if (ly < 0.0) ly = 0.0; else if (ly > (double)(origH - 1)) ly = (double)(origH - 1);

                switch (c->orient)
                {
                    case 0: tx = lx; ty = ly; break;
                    case 1: tx = (double)(origH - 1) - ly; ty = lx; break;
                    case 2: tx = (double)(origW - 1) - lx; ty = (double)(origH - 1) - ly; break;
                    case 3: tx = ly; ty = (double)(origW - 1) - lx; break;
                    case 4: tx = (double)(origW - 1) - lx; ty = ly; break;
                    case 5: tx = (double)(origH - 1) - ly; ty = (double)(origW - 1) - lx; break;
                    case 6: tx = lx; ty = (double)(origH - 1) - ly; break;
                    default: tx = ly; ty = lx; break;
                }
                newU = ((double)c->newX + tx) / 511.0;
                newV = ((double)c->newY + ty) / 511.0;
                if (newU < 0.0) newU = 0.0; else if (newU > 1.0) newU = 1.0;
                if (newV < 0.0) newV = 0.0; else if (newV > 1.0) newV = 1.0;
                allVerts[vi].lmCoord[0] = (float)newU;
                allVerts[vi].lmCoord[1] = (float)newV;
                rewritten++;
            }
        }
        Com_Printf("UVRepack: vertex rewrite ok=%d noIsland=%d notPlaced=%d\n",
                   rewritten, vertNoIsland, vertNotPlaced);

        /* === Step 7: per-surface lightmapIndex from dominant triangle island === */
        {
            int *triIslandHist = (int *)calloc(numIslands, sizeof(int));
            int multiPageSurfs = 0;
            for (s = 0; s < numBSPTriSoups; s++)
            {
                BspTriSoup_t *ts = &bspTriangles[s];
                int numTris, k, maxCnt = 0, maxLbl = -1;
                int lastPage = -1, surfMultiPage = 0;
                if (ts->lightmapIndex == 0x1F) continue;
                for (k = 0; k < numIslands; k++) triIslandHist[k] = 0;
                numTris = ts->indexCount / 3;
                for (t = 0; t < numTris; t++)
                {
                    int idx = ts->firstIndex + t * 3;
                    int base = ts->firstVertex;
                    int va = base + bspDrawIndexes[idx+0];
                    int isle;
                    if (va >= numBSPDrawVerts) continue;
                    isle = islandOfVert[va];
                    if (isle < 0) continue;
                    if (charts[isle].placed)
                    {
                        triIslandHist[isle]++;
                        if (lastPage < 0) lastPage = charts[isle].newPage;
                        else if (charts[isle].newPage != lastPage) surfMultiPage = 1;
                    }
                }
                for (k = 0; k < numIslands; k++)
                    if (triIslandHist[k] > maxCnt) { maxCnt = triIslandHist[k]; maxLbl = k; }
                if (maxLbl >= 0 && charts[maxLbl].placed)
                    ts->lightmapIndex = (unsigned short)charts[maxLbl].newPage;
                if (surfMultiPage) multiPageSurfs++;
            }
            free(triIslandHist);
            if (multiPageSurfs > 0)
                Com_Printf("UVRepack: WARNING %d surfaces span multiple pages "
                           "(per-surface lightmapIndex is per-engine model)\n",
                           multiPageSurfs);
        }

        /* patch g_triangles[].lightmapIdx to match the new bspTriangles */
        for (i = 0; i < g_triCount; i++)
        {
            Triangle_t *tri = &g_triangles[i];
            if (tri->lightmapIdx == 0x1F) continue;
            /* find this triangle's BSP surface and pull its post-repack page */
            {
                int firstV0 = tri->vertIndex[0];
                /* search bspTriangles for the surface containing this vertex
                   range; cheap because g_triangles preserve per-surface order */
                /* fallback: use island of vert[0] */
                int isle = (firstV0 < numBSPDrawVerts) ? islandOfVert[firstV0] : -1;
                if (isle >= 0 && charts[isle].placed)
                    tri->lightmapIdx = (unsigned short)charts[isle].newPage;
            }
        }

        free(islandOfVert);
        free(vertDone);
    }

    /* cleanup */
    for (j = 0; j < numIslands; j++)
    {
        int o;
        for (o = 0; o < 8; o++)
            if (charts[j].masks[o]) free(charts[j].masks[o]);
    }
    free(charts);
    free(labels);
    free(pixGrid);

    Com_Printf("UVRepack: done\n");
}
