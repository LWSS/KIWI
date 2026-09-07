/*
vis.c — PVS visibility computation

Reconstructed from cod2map.exe by Rose.
*/

#include "cod4map.h"

Leaf_t    *faceleafs;
Leaf_t    *leafs;
Vportal_t *sorted_portals[MAX_SORTED_PORTALS];
Vportal_t *visFacePortals;
Vportal_t *visPortals;

int    fastvis;
int    leafbytes;
int    leaflongs;
int    mergevis;
int    noPassageVis;
int    nosort;
int    numfaces;
int    numportals;
size_t portalbytes;
int    portalclusters;
int    saveprt;
int    totalvis;
int    visFloodCount;
int    visPortalLongs;
char   visTmpDir[MAX_OS_PATH];

char s_assertDisable_VisMain;

/* KIWI FIX (AUDIT_cod4map finding 13): the fixed-size portal bitvectors in this file are all
   Pstack_t::mightsee-sized (cod4map.h), so that array is the real cap on portalbytes.
   LoadPortals now errors instead of silently overrunning them. */
#define MAX_VIS_PORTALBYTES  ((size_t)sizeof(((Pstack_t *)0)->mightsee))


/*
================
PComp

qsort comparator: sorts portals by nummightsee ascending
================
*/
int PComp(const void *a, const void *b)
{
  int na = (*(Vportal_t **)a)->nummightsee;
  int nb = (*(Vportal_t **)b)->nummightsee;

  if ( na < nb )
    return -1;
  if ( na > nb )
    return 1;
  return 0;
}

/*
================
SortPortals

Populates sorted_portals[] and qsorts by nummightsee ascending
================
*/
void SortPortals(void)
{
  int i;
  int numTotalPortals;

  numTotalPortals = 2 * numportals;
  for ( i = 0; i < numTotalPortals; i++ )
    sorted_portals[i] = &visPortals[i];

  if ( !nosort )
    qsort(sorted_portals, numTotalPortals, sizeof(sorted_portals[0]), PComp);
}

/*
================
LeafVectorFromPortalVector

Converts portal visibility bits to leaf visibility bits, resolving merged leaves
================
*/
int LeafVectorFromPortalVector(byte *portalbits, byte *leafbits)
{
  int i;
  int portalLeaf;
  int mergedLeaf;

  /* mark leaves visible through portals */
  for ( i = 0; i < 2 * numportals; i++ )
  {
    if ( portalbits[i >> 3] & (1 << (i & 7)) )
    {
      portalLeaf = visPortals[i].leaf;
      leafbits[portalLeaf >> 3] |= 1 << (portalLeaf & 7);
    }
  }

  /* resolve merged leaves */
  for ( i = 0; i < portalclusters; i++ )
  {
    mergedLeaf = i;
    while ( leafs[mergedLeaf].merged >= 0 )
      mergedLeaf = leafs[mergedLeaf].merged;
    if ( leafbits[mergedLeaf >> 3] & (1 << (mergedLeaf & 7)) )
      leafbits[i >> 3] |= 1 << (i & 7);
  }

  return CountBits(leafbits, portalclusters);
}

/*
================
ClusterMerge

Merges the portal visibility for a leaf into the final leaf vis data
================
*/
void ClusterMerge(int leafnum)
{
  int mergedLeafnum;
  Leaf_t *leaf;
  Vportal_t *p;
  int i, j;
  int pidx;
  int numvis;
  char portalvector[MAX_MAP_LEAFS / 8];                                  /* leaf bits  — leafbytes */
  /* KIWI FIX (AUDIT_cod4map finding 13): this buffer holds the PORTAL bitvector (memset and
     OR'd for portalbytes), so it must be sized from the portal cap, not MAX_MAP_LEAFS. */
  uintptr_t uncompressed[MAX_VIS_PORTALBYTES / sizeof(uintptr_t)];       /* portal bits — portalbytes */

  /* resolve merged leaf chain */
  mergedLeafnum = leafnum;
  while ( leafs[mergedLeafnum].merged >= 0 )
    mergedLeafnum = leafs[mergedLeafnum].merged;

  memset(uncompressed, 0, portalbytes);
  leaf = &leafs[mergedLeafnum];

  /* OR together portalvis from all portals on this leaf */
  for ( i = 0; i < leaf->numportals; i++ )
  {
    p = leaf->portals[i];
    if ( p->removed )
      continue;
    if ( p->status != stat_done )
      Com_Error("portal not done");

    for ( j = 0; j < visPortalLongs; j++ )
      uncompressed[j] |= ((uintptr_t *)p->portalvis)[j];

    /* mark this portal as visible */
    pidx = (int)(p - visPortals);
    ((byte *)uncompressed)[pidx >> 3] |= 1 << (pidx & 7);
  }

  /* convert portal vis to leaf vis */
  memset(portalvector, 0, leafbytes);
  portalvector[mergedLeafnum >> 3] |= 1 << (mergedLeafnum & 7);
  numvis = LeafVectorFromPortalVector((byte *)uncompressed, (byte *)portalvector) + 1;
  totalvis += numvis;
  Com_DPrintf("cluster %4i : %4i visible\n", leafnum, numvis);
  memcpy(((BspVisibility_t *)bspVisBytes)->data + leafnum * leafbytes, portalvector, leafbytes);
}

/*
================
CalcFastVis

Fast vis: copies portalflood to portalvis and marks all portals as done
================
*/
void CalcFastVis(void)
{
  int i;

  for ( i = 0; i < 2 * numportals; i++ )
  {
    visPortals[i].portalvis = visPortals[i].portalflood;
    visPortals[i].status = stat_done;
  }
}

/*
================
CalcVis

Main visibility computation orchestrator. Runs threaded passes then assembles leaf vis.
NOTE: Multi-threaded vis has benign data races (inherited from q3vis) — use -threads 1
for deterministic results.
================
*/
void CalcVis(void)
{
  int clusterIdx;

  /* pass 1: base portal vis — initial flood fill */
  RunThreadsOnIndividual(2 * numportals, 1, (LPTHREAD_START_ROUTINE)BasePortalVis);
  SortPortals();

  if ( fastvis )
  {
    CalcFastVis();
  }
  else if ( noPassageVis )
  {
    RunThreadsOnIndividual(2 * numportals, 1, (LPTHREAD_START_ROUTINE)PortalFlow);
  }
  else
  {
    PassageMemory();
	
    /* pass 2: create passages — clips and tightens */
    RunThreadsOnIndividual(2 * numportals, 1, (LPTHREAD_START_ROUTINE)CreatePassages);
	
    /* pass 3: passage portal flow — recursive flow using passages */
    RunThreadsOnIndividual(2 * numportals, 1, (LPTHREAD_START_ROUTINE)PassagePortalFlow);
  }
  Com_Printf("creating leaf vis...\n");
  for ( clusterIdx = 0; clusterIdx < portalclusters; clusterIdx++ )
    ClusterMerge(clusterIdx);
  Com_Printf("Average clusters visible: %i\n", totalvis / portalclusters);
}

/*
================
BuildPHS

Builds the Potentially Hearable Set from PVS data (dead code in original — never called).
For each cluster, OR's together the PVS rows of all clusters visible to it,
creating a transitive closure. Validates PVS bit indices.
================
*/
void BuildPHS(void)
{
  int i, j, k, bit, cluster, totalHearable = 0;
  unsigned int phsBuf[MAX_MAP_LEAFS / 8 / sizeof(unsigned int)];
  unsigned char *pvsRow;

  Com_Printf("Building PHS...\n");

  for ( i = 0; i < portalclusters; i++ )
  {
    pvsRow = (unsigned char *)(bspVisBytes + leafbytes * i);
    memcpy(phsBuf, pvsRow, leafbytes);

    /* for each bit set in PVS, OR in that cluster's PVS row */
    for ( j = 0; j < leafbytes; j++ )
    {
      if ( !pvsRow[j] )
        continue;
      for ( bit = 0; bit < 8; bit++ )
      {
        if ( !(pvsRow[j] & (1 << bit)) )
          continue;
        cluster = bit + j * 8;
        if ( cluster >= portalclusters )
          Com_Error("Bad bit in PVS");
        { unsigned int *srcRow = (unsigned int *)(bspVisBytes + leafbytes * cluster);
        for ( k = 0; k < leaflongs; k++ )
          phsBuf[k] |= srcRow[k];
        }
      }
    }

    /* count hearable clusters */
    for ( k = 0; k < portalclusters; k++ )
      if ( ((unsigned char *)phsBuf)[k >> 3] & (1 << (k & 7)) )
        totalHearable++;
  }
  Com_Printf("Average clusters hearable: %i\n", totalHearable / portalclusters);
}

/*
================
UpdatePortals

Fixes portal leaf references after leaf merges
================
*/
void UpdatePortals(void)
{
  int i;
  Vportal_t *vp;

  for ( i = 0; i < 2 * numportals; i++ )
  {
    vp = &visPortals[i];
    if ( vp->removed )
      continue;
    while ( leafs[vp->leaf].merged >= 0 )
      vp->leaf = leafs[vp->leaf].merged;
  }
}

/*
================
PlaneFromWindingFast

Computes plane normal and dist from a winding's first three vertices
================
*/
void PlaneFromWindingFast(Winding_t *w, float *plane)
{
  vec3_t edge1, edge2;

  VectorSubtract(w->points[0], w->points[1], edge1);
  VectorSubtract(w->points[2], w->points[1], edge2);
  CrossProduct(edge1, edge2, plane);
  VecNormalize(plane);
  plane[3] = DotProduct210(w->points[0], plane);
}

/*
================
SetPortalSphere

Computes bounding sphere (origin + radius) for a portal's winding
================
*/
void SetPortalSphere(Vportal_t *p)
{
  Winding_t *w;
  int i;
  float scale;
  vec3_t delta;
  float dist;
  
  /* volatile: force 32-bit float reload each iteration */
  volatile float originX;
  volatile float originY;
  float originZ;
  float maxRadius;

  w = p->winding;

  /* compute centroid */
  originX = 0.0f;
  originY = 0.0f;
  originZ = 0.0f;
  for ( i = 0; i < w->numpoints; i++ )
  {
    originX += w->points[i][0];
    originY += w->points[i][1];
    originZ += w->points[i][2];
  }
  scale = 1.0f / w->numpoints;
  originX *= scale;
  originY *= scale;
  originZ *= scale;

  /* find maximum radius */
  maxRadius = 0.0f;
  for ( i = 0; i < w->numpoints; i++ )
  {
    delta[0] = w->points[i][0] - originX;
    delta[1] = w->points[i][1] - originY;
    delta[2] = w->points[i][2] - originZ;
    dist = (float)sqrt(DotProduct210(delta, delta));
    if ( dist > maxRadius )
      maxRadius = dist;
  }

  p->origin[0] = originX;
  p->origin[1] = originY;
  p->origin[2] = originZ;
  p->radius = maxRadius;
}

/*
================
Winding_PlanesConcave

Tests if two windings would form a concave shape when merged
================
*/
int Winding_PlanesConcave(Winding_t *w1, Winding_t *w2, float *normal1, float *normal2, float dist1, float dist2)
{
  int i;

  if ( !w1 || !w2 )
    return 0;

  /* check if any point of w1 is in front of w2's plane */
  for ( i = 0; i < w1->numpoints; i++ )
  {
    if ( DotProduct201(w1->points[i], normal2) - dist2 > EDGE_LENGTH )
      return 1;
  }

  /* check if any point of w2 is in front of w1's plane */
  for ( i = 0; i < w2->numpoints; i++ )
  {
    if ( DotProduct021(w2->points[i], normal1) - dist1 > EDGE_LENGTH )
      return 1;
  }
  return 0;
}

/*
================
TryMergeLeaves

Attempts to merge two leaves if their portals don't form a concave shape.
Checks both leafs[] and faceleafs[] arrays (k=0: faceleafs, k=1: leafs).
================
*/
int TryMergeLeaves(int l1num, int l2num)
{
  int i, j, k, n, numportals;
  Leaf_t *l1, *l2;
  Vportal_t *p1, *p2;
  Vportal_t *portals[MAX_PORTALS_ON_LEAF];
  float plane1[4], plane2[4];

  /* phase 1: check all portal pairs for concavity */
  for ( k = 0; k < 2; k++ )
  {
    l1 = k ? &leafs[l1num] : &faceleafs[l1num];
    for ( i = 0; i < l1->numportals; i++ )
    {
      p1 = l1->portals[i];
      if ( p1->leaf == l2num )
        continue;
      for ( n = 0; n < 2; n++ )
      {
        l2 = n ? &leafs[l2num] : &faceleafs[l2num];
        for ( j = 0; j < l2->numportals; j++ )
        {
          p2 = l2->portals[j];
          if ( p2->leaf == l1num )
            continue;
          Vector4Copy(p1->plane, plane1);
          Vector4Copy(p2->plane, plane2);
          if ( Winding_PlanesConcave(p1->winding, p2->winding, plane1, plane2, plane1[3], plane2[3]) )
            return 0;
        }
      }
    }
  }

  /* phase 2: merge the leaves */
  for ( k = 0; k < 2; k++ )
  {
    l1 = k ? &leafs[l1num] : &faceleafs[l1num];
    l2 = k ? &leafs[l2num] : &faceleafs[l2num];
    numportals = 0;

    for ( i = 0; i < l1->numportals; i++ )
    {
      p1 = l1->portals[i];
      if ( p1->leaf == l2num )
        p1->removed = 1;
      else
        portals[numportals++] = p1;
    }
    for ( j = 0; j < l2->numportals; j++ )
    {
      p2 = l2->portals[j];
      if ( p2->leaf == l1num )
        p2->removed = 1;
      else
        portals[numportals++] = p2;
    }
    memcpy(l2->portals, portals, sizeof(Vportal_t *) * numportals);
    l2->numportals = numportals;
    l1->merged = l2num;
  }
  return 1;
}

/*
================
MergeLeaves

Iteratively merges leaf pairs through non-hint portals until no more merges possible
================
*/
void MergeLeaves(void)
{
  int i, j, nummerges, totalmerges;
  Leaf_t *leaf;
  Vportal_t *p;

  totalmerges = 0;
  do
  {
    nummerges = 0;
    for ( i = 0; i < portalclusters; i++ )
    {
      leaf = &leafs[i];
      if ( leaf->merged >= 0 )
        continue;
      for ( j = 0; j < leaf->numportals; j++ )
      {
        p = leaf->portals[j];
        if ( p->removed )
          continue;
        if ( TryMergeLeaves(i, p->leaf) )
        {
          UpdatePortals();
          nummerges++;
          break;
        }
      }
    }
    totalmerges += nummerges;
  } while ( nummerges );
  Com_Printf("%6d leaves merged\n", totalmerges);
}

/*
================
TryMergeWinding

Tries to merge two coplanar windings sharing a common edge into one convex winding
================
*/
/*
================
NewWinding

VIS owns a separate winding allocator from common/polylib.  Native caps the
request at 1024 points and clears every byte of the 4 + 12*n allocation; it
does not participate in AllocWinding's common-tool accounting.
================
*/
static Winding_t *NewWinding(int numPoints)
{
  int allocationSize;
  Winding_t *winding;

  if ( numPoints < 0 || numPoints > MAX_POINTS_ON_WINDING )
    Com_Error("NewWinding: %i pts", numPoints);

  allocationSize = sizeof(int) + sizeof(vec3_t) * numPoints;
  winding = (Winding_t *)malloc(allocationSize);
  if ( !winding )
    Com_Error("NewWinding: out of memory for %i points", numPoints);
  memset(winding, 0, allocationSize);
  return winding;
}

Winding_t *TryMergeWinding(Winding_t *f1, Winding_t *f2, float *planenormal)
{
  #define CONTINUOUS_EPSILON 0.005
  #define VERTEX_MATCH_EPSILON 0.1
  float *p1, *p2, *back;
  Winding_t *newf;
  int i, j, k, l;
  vec3_t normal, delta;
  double dot;
  int keep1, keep2;

  /* find a common edge */
  p1 = p2 = NULL;
  j = 0;
  for ( i = 0; i < f1->numpoints; i++ )
  {
    p1 = f1->points[i];
    p2 = f1->points[(i + 1) % f1->numpoints];
    for ( j = 0; j < f2->numpoints; j++ )
    {
      for ( k = 0; k < 3; k++ )
      {
        if ( fabs(p1[k] - f2->points[(j + 1) % f2->numpoints][k]) > VERTEX_MATCH_EPSILON )
          break;
        if ( fabs(p2[k] - f2->points[j][k]) > VERTEX_MATCH_EPSILON )
          break;
      }
      if ( k == 3 )
        break;
    }
    if ( j < f2->numpoints )
      break;
  }
  if ( i == f1->numpoints )
    return NULL;

  /* check slope of connected lines — if colinear, the point can be removed */
  back = f1->points[(i + f1->numpoints - 1) % f1->numpoints];
  VectorSubtract(p1, back, delta);
  CrossProduct(planenormal, delta, normal);
  VecNormalize(normal);

  back = f2->points[(j + 2) % f2->numpoints];
  VectorSubtract(back, p1, delta);
  dot = DotProduct210(delta, normal);
  if ( dot > CONTINUOUS_EPSILON )
    return NULL;
  keep1 = dot < -CONTINUOUS_EPSILON;

  back = f1->points[(i + 2) % f1->numpoints];
  VectorSubtract(back, p2, delta);
  CrossProduct(planenormal, delta, normal);
  VecNormalize(normal);

  back = f2->points[(j + f2->numpoints - 1) % f2->numpoints];
  VectorSubtract(back, p2, delta);
  dot = DotProduct210(delta, normal);
  if ( dot > CONTINUOUS_EPSILON )
    return NULL;
  keep2 = dot < -CONTINUOUS_EPSILON;

  /* build the new polygon */
  newf = NewWinding(f1->numpoints + f2->numpoints);

  /* copy first polygon */
  for ( k = (i + 1) % f1->numpoints; k != i; k = (k + 1) % f1->numpoints )
  {
    if ( k == (i + 1) % f1->numpoints && !keep2 )
      continue;
    VectorCopy(f1->points[k], newf->points[newf->numpoints]);
    newf->numpoints++;
  }

  /* copy second polygon */
  for ( l = (j + 1) % f2->numpoints; l != j; l = (l + 1) % f2->numpoints )
  {
    if ( l == (j + 1) % f2->numpoints && !keep1 )
      continue;
    VectorCopy(f2->points[l], newf->points[newf->numpoints]);
    newf->numpoints++;
  }

  return newf;
  #undef CONTINUOUS_EPSILON
  #undef VERTEX_MATCH_EPSILON
}

/*
================
MergeLeafPortals

Merges coplanar portal windings that lead to the same leaf within each cluster
================
*/
void MergeLeafPortals( void )
{
  int i, j, k, nummerges;
  Leaf_t *leaf;
  Vportal_t *p1, *p2;
  Winding_t *w;

  nummerges = 0;
  for ( i = 0; i < portalclusters; i++ )
  {
    leaf = &leafs[i];
    if ( leaf->merged >= 0 )
      continue;
    for ( j = 0; j < leaf->numportals; j++ )
    {
      p1 = leaf->portals[j];
      if ( p1->removed )
        continue;
      for ( k = j + 1; k < leaf->numportals; k++ )
      {
        p2 = leaf->portals[k];
        if ( p2->removed )
          continue;
        if ( p1->leaf == p2->leaf )
        {
          w = TryMergeWinding( p1->winding, p2->winding, p1->plane );
          if ( w )
          {
            FreeWinding( p1->winding );
            p1->winding = w;
            SetPortalSphere( p1 );
            p2->removed = 1;
            nummerges++;
            i--;
            break;
          }
        }
      }
      if ( k < leaf->numportals )
        break;
    }
  }
  Com_Printf( "%6d portals merged\n", nummerges );
}

/*
================
LoadPortals

Reads .d3dprt portal file, builds forward and backward portal structures and face portals
================
*/
int LoadPortals( char *portalFile )
{
  int i, j, numpoints;
  int leafnums[2];
  Vportal_t *p;
  Leaf_t *l;
  Winding_t *w;
  float plane[4];
  double v[3];
  char magic[80];
  FILE *f;

  if ( !strcmp( portalFile, "-" ) )
    f = stdin;
  else
  {
    f = fopen( portalFile, "r" );
    if ( !f )
      Com_Error( "LoadPortals: couldn't read %s\n", portalFile );
  }

  if ( fscanf( f, "%79s\n%i\n%i\n%i\n", magic, &portalclusters, &numportals, &numfaces ) != 4 )
    Com_Error( "LoadPortals: failed to read header" );
  if ( strcmp( magic, "PRT1" ) )
    Com_Error( "LoadPortals: not a portal file" );

  /* Validate before rounding counts or multiplying allocation sizes. */
  if ( portalclusters < 1 || portalclusters > MAX_MAP_LEAFS )
    Com_Error( "LoadPortals: invalid cluster count %i", portalclusters );
  if ( numportals < 0 || (size_t)numportals > MAX_VIS_PORTALBYTES * 4 )
    Com_Error( "LoadPortals: invalid portal count %i", numportals );
  if ( numfaces < 0 || numfaces > portalclusters * MAX_PORTALS_ON_LEAF )
    Com_Error( "LoadPortals: invalid face count %i", numfaces );

  Com_Printf( "%6i portalclusters\n", portalclusters );
  Com_Printf( "%6i numportals\n", numportals );
  Com_Printf( "%6i numfaces\n", numfaces );

  leafbytes = ((portalclusters + 63) & ~63) >> 3;
  leaflongs = leafbytes / sizeof(long);
  if ( (size_t)portalclusters * leafbytes + offsetof(BspVisibility_t, data) > sizeof(bspVisBytes) )
    Com_Error( "MAX_MAP_VISIBILITY (%i) exceeded\n", MAX_MAP_VISIBILITY );
  portalbytes = ((numportals * 2 + 63) & ~63) >> 3;
  visPortalLongs = (int)(portalbytes / sizeof(uint32_t));

  /* The rounded size must fit the shared Pstack_t::mightsee capacity too. */
  if ( portalbytes > MAX_VIS_PORTALBYTES )
    Com_Error( "LoadPortals: too many portals (%i, max %i)", numportals, (int)(MAX_VIS_PORTALBYTES * 4) );

  /* each file portal is split into two memory portals (forward + back) */
  visPortals = (Vportal_t *)calloc( numportals ? 2 * numportals : 1, sizeof(Vportal_t) );
  if ( !visPortals )
    Com_Error( "LoadPortals: out of memory for portals" );

  leafs = (Leaf_t *)calloc( portalclusters, sizeof(Leaf_t) );
  if ( !leafs )
    Com_Error( "LoadPortals: out of memory for clusters" );
  for ( i = 0; i < portalclusters; i++ )
    leafs[i].merged = -1;

  ((BspVisibility_t *)bspVisBytes)->numClusters = portalclusters;
  ((BspVisibility_t *)bspVisBytes)->clusterBytes = leafbytes;
  numBSPVisBytes = portalclusters * leafbytes + offsetof(BspVisibility_t, data);

  /* read portals */
  for ( i = 0, p = visPortals; i < numportals; i++ )
  {
    if ( fscanf( f, "%i %i %i ", &numpoints, &leafnums[0], &leafnums[1] ) != 3 )
      Com_Error( "LoadPortals: reading portal %i", i );
    if ( numpoints < 3 || numpoints > MAX_POINTS_ON_WINDING )
      Com_Error( "LoadPortals: portal %i has invalid point count %i", i, numpoints );
    if ( leafnums[0] < 0 || leafnums[0] >= portalclusters || leafnums[1] < 0 || leafnums[1] >= portalclusters )
      Com_Error( "LoadPortals: reading portal %i", i );

    w = NewWinding( numpoints );
    w->numpoints = numpoints;
    p->winding = w;

    /* scanf into double, then assign to float — same as q3map2 */
    for ( j = 0; j < numpoints; j++ )
    {
      if ( fscanf( f, "(%lf %lf %lf ) ", &v[0], &v[1], &v[2] ) != 3 )
        Com_Error( "LoadPortals: reading portal %i", i );
      w->points[j][0] = (float)v[0];
      w->points[j][1] = (float)v[1];
      w->points[j][2] = (float)v[2];
    }
    fscanf( f, "\n" );
    PlaneFromWindingFast( w, plane );

    /* create forward portal */
    l = &leafs[leafnums[0]];
    if ( l->numportals == MAX_PORTALS_ON_LEAF )
      Com_Error( "Leaf with too many portals" );
    l->portals[l->numportals++] = p;

    p->num = i + 1;
    p->winding = w;
    VectorNegate(plane, p->plane);
    p->plane[3] = -plane[3];
    p->leaf = leafnums[1];
    SetPortalSphere( p );
    p++;

    /* create backwards portal */
    l = &leafs[leafnums[1]];
    if ( l->numportals == MAX_PORTALS_ON_LEAF )
      Com_Error( "Leaf with too many portals" );
    l->portals[l->numportals++] = p;

    p->num = i + 1;
    p->winding = NewWinding( w->numpoints );
    p->winding->numpoints = w->numpoints;
    for ( j = 0; j < w->numpoints; j++ )
      VectorCopy( w->points[w->numpoints - 1 - j], p->winding->points[j] );

    Vector4Copy(plane, p->plane);
    p->leaf = leafnums[0];
    SetPortalSphere( p );
    p++;
  }

  /* load face portals */
  visFacePortals = (Vportal_t *)calloc( numfaces ? numfaces : 1, sizeof(Vportal_t) );
  faceleafs = (Leaf_t *)calloc( portalclusters, sizeof(Leaf_t) );
  if ( !visFacePortals || !faceleafs )
    Com_Error( "LoadPortals: out of memory for face portals" );

  for ( i = 0, p = visFacePortals; i < numfaces; i++ )
  {
    if ( fscanf( f, "%i %i ", &numpoints, &leafnums[0] ) != 2 )
      Com_Error( "LoadPortals: reading portal %i", i );
    if ( numpoints < 3 || numpoints > MAX_POINTS_ON_WINDING )
      Com_Error( "LoadPortals: face %i has invalid point count %i", i, numpoints );
    if ( leafnums[0] < 0 || leafnums[0] >= portalclusters )
      Com_Error( "LoadPortals: face %i has invalid cluster %i", i, leafnums[0] );

    w = NewWinding( numpoints );
    w->numpoints = numpoints;
    p->winding = w;

    for ( j = 0; j < numpoints; j++ )
    {
      if ( fscanf( f, "(%lf %lf %lf ) ", &v[0], &v[1], &v[2] ) != 3 )
        Com_Error( "LoadPortals: reading portal %i", i );
      w->points[j][0] = (float)v[0];
      w->points[j][1] = (float)v[1];
      w->points[j][2] = (float)v[2];
    }
    fscanf( f, "\n" );
    PlaneFromWindingFast( w, plane );

    l = &faceleafs[leafnums[0]];
    l->merged = -1;
    if ( l->numportals == MAX_PORTALS_ON_LEAF )
      Com_Error( "Leaf with too many faces" );
    l->portals[l->numportals++] = p;

    p->num = i + 1;
    p->winding = w;
    VectorNegate(plane, p->plane);
    p->plane[3] = -plane[3];
    p->leaf = -1;
    SetPortalSphere( p );
    p++;
  }

  return f == stdin ? 0 : fclose( f );
}

/*
================
VisMain

Vis stage entry point: parses args, loads BSP and portals, runs visibility computation
================
*/
int VisMain( int argc, char **argv )
{
  int i, j, activePortals;
  double startTime;
  char prtPath[MAX_OS_PATH];
  char bspPath[MAX_OS_PATH];

  Com_Printf( "---- vis ----\n" );

  /* process arguments */
  verbose = 0;
  for ( i = 1; i < argc; i++ )
  {
    if ( !strcmp( argv[i], "-threads" ) )
    {
      numthreads = atol( argv[++i] );
    }
    else if ( !strcmp( argv[i], "-fast" ) )
    {
      Com_Printf( "fastvis = true\n" );
      fastvis = 1;
    }
    else if ( !strcmp( argv[i], "-merge" ) )
    {
      Com_Printf( "merge = true\n" );
      mergevis = 1;
    }
    else if ( !strcmp( argv[i], "-nopassage" ) )
    {
      Com_Printf( "nopassage = true\n" );
      noPassageVis = 1;
    }
    else if ( !strcmp( argv[i], "-level" ) )
    {
      testlevel = atol( argv[++i] );
      Com_Printf( "testlevel = %i\n", testlevel );
    }
    else if ( !strcmp( argv[i], "-v" ) )
    {
      Com_Printf( "verbose = true\n" );
      verbose = 1;
    }
    else if ( !strcmp( argv[i], "-nosort" ) )
    {
      Com_Printf( "nosort = true\n" );
      nosort = 1;
    }
    else if ( !strcmp( argv[i], "-saveprt" ) )
    {
      Com_Printf( "saveprt = true\n" );
      saveprt = 1;
    }
    else if ( !strcmp( argv[i], "-tmpin" ) )
    {
      strcpy( visTmpDir, "/tmp" );
    }
    else if ( !_stricmp( argv[i], "-platform" ) )
    {
      SetTargetPlatformByName( argv[++i] );
    }
    else
    {
      if ( *argv[i] != '-' )
        break;
      Com_Error( "Unknown option \"%s\"", argv[i] );
    }
  }
  if ( i != argc - 1 )
    Com_Error( "usage: vis [-threads #] [-level 0-4] [-fast] [-v] bspfile" );
  if ( !ValidatePlatformSet() )
    exit( -1 );

  BSP_SetTargetPlatform();
  startTime = I_FloatTime();
  ThreadSetDefault();
  FS_Startup( argv[i] );
  FS_Startup_Simple();

  /* load the BSP */
  sprintf( bspPath, "%s%s", visTmpDir, ExpandArg( argv[i] ) );
  StripExtension( bspPath );
  strcat( bspPath, GetBSPFileExtension() );
  Com_Printf( "reading %s\n", bspPath );
  LoadBSPFile( bspPath );
  ParseEntities();

  /* load the portal file */
  sprintf( prtPath, "%s%s", visTmpDir, ExpandArg( argv[i] ) );
  StripExtension( prtPath );
  strcat( prtPath, GetPRTFileExtension() );
  Com_Printf( "reading %s\n", prtPath );
  LoadPortals( prtPath );

  if ( mergevis )
  {
    MergeLeaves();
    MergeLeafPortals();
  }

  /* count active portals */
  activePortals = 0;
  for ( j = 0; j < 2 * numportals; j++ )
  {
    if ( !visPortals[j].removed )
      activePortals++;
  }
  Com_Printf( "%6d active portals\n", activePortals );
  Com_Printf( "visdatasize:%i\n", numBSPVisBytes );
  if ( numBSPVisBytes > MAX_MAP_VISIBILITY )
    Com_Error( "MAX_MAP_VISIBILITY (%i) exceeded\n", MAX_MAP_VISIBILITY );

  CalcVis();

  /* write output */
  Com_Printf( "writing %s\n", bspPath );
  Assert( g_targetPlatform, s_assertDisable_VisMain );
  WriteBSPFile( bspPath, g_targetPlatform->bigEndian );
  Com_Printf( "%5.2f seconds elapsed\n", I_FloatTime() - startTime );
  return 0;
}

/*
================
CountBits

Counts the number of set bits in a bitvector
================
*/
int CountBits(unsigned char *bits, int numbits)
{
  int i;
  int c;

  c = 0;
  for ( i = 0; i < numbits; i++ )
  {
    if ( bits[i >> 3] & (1 << (i & 7)) )
      ++c;
  }
  return c;
}

/*
================
CheckStack

Debug validation for BSP recursion stack (dead code in original — never called).
Walks the pstack chain checking for duplicate leaf nodes which would indicate
infinite recursion in the vis flood fill.

================
*/
void CheckStack(int leafNum, VisThreadData_t *thread)
{
  Pstack_t *cur;
  Pstack_t *inner;

  for ( cur = thread->pstack_head.next; cur != NULL; cur = cur->next )
  {
    if ( (intptr_t)cur->leaf == leafNum )
      Com_Error("CheckStack: leaf recursion");

    for ( inner = thread->pstack_head.next; inner != cur; inner = inner->next )
    {
      if ( inner->leaf == cur->leaf )
        Com_Error("CheckStack: late leaf recursion");
    }
  }
}

/*
================
AllocStackWinding

Allocates a free winding from the pstack winding pool
================
*/
Winding_t *AllocStackWinding(Pstack_t *stack)
{
  int i;

  for ( i = 0; i < 3; ++i )
  {
    if ( stack->freewindings[i] )
    {
      stack->freewindings[i] = 0;
      return (Winding_t *)&stack->windings[i];
    }
  }
  Com_Error("AllocStackWinding: failed");
  return 0;
}

/*
================
FreeStackWinding

Returns a winding to the pstack winding pool
================
*/
int FreeStackWinding(Winding_t *w, Pstack_t *stack)
{
  int result;
  unsigned int i;
  ptrdiff_t offset = (char *)w - (char *)stack->windings;

  result = 0;
  i = (unsigned int)(offset / VIS_WINDING_SLOT_SIZE);
  if ( i <= 2 )
  {
    result = stack->freewindings[i];
    if ( result )
      result = (intptr_t)Com_Error("FreeStackWinding: allready free");
    stack->freewindings[i] = 1;
  }
  return result;
}

/*
================
RecursivePassageFlow

Recursive passage-based visibility flood fill
================
*/
int RecursivePassageFlow(Vportal_t *portal, VisThreadData_t *thread, Pstack_t *prevstack)
{
  Leaf_t *leaf;
  Passage_t *passage, *nextpassage;
  Vportal_t *p;
  uint32_t *cansee, *portalvis, *might, *vis;
  uint32_t more;
  int i, j, pnum;
  Pstack_t stack;

  leaf = &leafs[portal->leaf];
  stack.next = NULL;
  stack.depth = prevstack->depth + 1;
  prevstack->next = &stack;
  vis = (uint32_t *)thread->portal->portalvis;

  passage = portal->passages;
  nextpassage = passage;

  for ( i = 0; i < leaf->numportals; i++, passage = nextpassage )
  {
    p = leaf->portals[i];
    if ( p->removed )
      continue;
    nextpassage = passage->next;

    pnum = (int)(p - visPortals);
    if ( !(prevstack->mightsee[pnum >> 3] & (1 << (pnum & 7))) )
      continue;

    cansee = (uint32_t *)passage->cansee;
    might = (uint32_t *)stack.mightsee;
    memcpy(stack.mightsee, prevstack->mightsee, portalbytes);
    portalvis = (uint32_t *)(p->status == stat_done ? p->portalvis : p->portalflood);

    more = 0;
    for ( j = 0; j < visPortalLongs; j++ )
    {
      if ( *might )
      {
        *might &= *cansee++ & *portalvis++;
        more |= (*might & ~vis[j]);
      }
      else
      {
        cansee++;
        portalvis++;
      }
      might++;
    }

    if ( !more )
    {
      if ( thread->portal->portalvis[pnum >> 3] & (1 << (pnum & 7)) )
        continue;
    }

    thread->portal->portalvis[pnum >> 3] |= 1 << (pnum & 7);
    RecursivePassageFlow(p, thread, &stack);
    stack.next = NULL;
  }
  return i;
}

/*
================
PassageChopWinding

Clips a winding by a split plane into an output buffer.
Unlike VisChopWinding, uses caller-provided output rather than stack allocation.
================
*/
int *PassageChopWinding(int *in, int *out, float *split)
{
  #define MAX_PASSAGE_WINDING 128
  #define CLIP_EPSILON 0.1f
  #define MAX_PASSAGE_OUTPUT 12
  int numpoints, i, counts[3];
  float *p1, *p2, *dst;
  float dists[MAX_PASSAGE_WINDING];
  int sides[MAX_PASSAGE_WINDING];
  double dot, mid;
  float frac;
  int nextSide;

  numpoints = *in;
  if ( numpoints <= 0 )
    return in;

  /* classify each vertex */
  counts[0] = counts[1] = counts[2] = 0;
  for ( i = 0; i < numpoints; i++ )
  {
    p1 = (float *)&in[3 * i + 1];
    dot = DotProduct201(p1, split) - split[3];
    dists[i] = (float)dot;
    if ( dot > CLIP_EPSILON )
      sides[i] = SIDE_FRONT;
    else if ( dot < -CLIP_EPSILON )
      sides[i] = SIDE_BACK;
    else
      sides[i] = SIDE_ON;
    counts[sides[i]]++;
  }

  if ( !counts[SIDE_BACK] )
    return in;
  if ( !counts[SIDE_FRONT] )
    return 0;

  sides[numpoints] = sides[0];
  dists[numpoints] = dists[0];
  *out = 0;

  for ( i = 0; i < numpoints; i++ )
  {
    if ( *out == MAX_PASSAGE_OUTPUT )
      return in;

    p1 = (float *)&in[3 * i + 1];

    if ( sides[i] == SIDE_ON || sides[i] == SIDE_FRONT )
    {
      dst = (float *)&out[3 * *out + 1];
      VectorCopy(p1, dst);
      (*out)++;
    }

    if ( sides[i] == SIDE_ON )
      continue;

    nextSide = sides[i + 1];
    if ( nextSide == SIDE_ON || nextSide == sides[i] )
      continue;

    /* generate a split point */
    if ( *out == MAX_PASSAGE_OUTPUT )
      return in;

    p2 = (float *)&in[3 * ((i + 1) % numpoints) + 1];
    frac = DIVS(dists[i], dists[i], dists[i + 1]);

    dst = (float *)&out[3 * *out + 1];
    { int k;
    for ( k = 0; k < 3; k++ )
    {
      if ( split[k] == 1.0f )
        mid = split[3];
      else if ( split[k] == -1.0f )
        mid = -split[3];
      else
        mid = FMA1(p1[k], p2[k] - p1[k], frac);
      dst[k] = (float)mid;
    } }
    (*out)++;
  }
  return out;
  #undef MAX_PASSAGE_WINDING
  #undef CLIP_EPSILON
  #undef MAX_PASSAGE_OUTPUT
}

/*
================
AddSeperators

Finds separating planes between source and pass windings.
Generates candidates by taking two points from source and one from pass.
================
*/
int AddSeperators(Winding_t *source, Winding_t *pass, int flipclip, float *seperators, int maxseperators)
{
  #define SEP_EPSILON 0.1f
  int i, j, k, l, numseperators;
  vec3_t v1, v2;
  float normal[3];
  float d, length, planeDist;
  int fliptest, counts;

  numseperators = 0;

  for ( i = 0; i < source->numpoints; i++ )
  {
    l = (i + 1) % source->numpoints;
    VectorSubtract(source->points[l], source->points[i], v1);

    for ( j = 0; j < pass->numpoints; j++ )
    {
      VectorSubtract(pass->points[j], source->points[i], v2);

      CrossProduct(v1, v2, normal);
      length = DotProduct210(normal, normal);

      if ( length < SEP_EPSILON )
        continue;

      length = (float)(1.0 / sqrt(length));
      VectorScale(normal, length, normal);
      planeDist = (float)DotProduct120(pass->points[j], normal);

      /* find which side of the plane the source portal is on */
      fliptest = 0;
      for ( k = 0; k < source->numpoints; k++ )
      {
        if ( k == i || k == l )
          continue;
        d = (float)(DotProduct201(source->points[k], normal) - planeDist);
        if ( d < -SEP_EPSILON )
        {
          fliptest = 0;
          break;
        }
        if ( d > SEP_EPSILON )
        {
          fliptest = 1;
          break;
        }
      }
      if ( k == source->numpoints )
        continue;   /* planar with source portal */

      /* flip the normal if the source portal is backwards */
      if ( fliptest )
      {
        VectorNegate(normal, normal);
        planeDist = -planeDist;
      }

      /* if all of the pass portal points are on the positive side,
         this is the seperating plane */
      counts = 0;
      for ( k = 0; k < pass->numpoints; k++ )
      {
        if ( k == j )
          continue;
        d = (float)(DotProduct201(pass->points[k], normal) - planeDist);
        if ( d < -SEP_EPSILON )
          break;
        if ( d > SEP_EPSILON )
          counts++;
      }
      if ( k != pass->numpoints )
        continue;   /* points on negative side */
      if ( !counts )
        continue;   /* planar with seperating plane */

      /* flip if we want the back side */
      if ( flipclip )
      {
        VectorNegate(normal, normal);
        planeDist = -planeDist;
      }

      if ( numseperators >= maxseperators )
        Com_Error("max seperators");
      VectorCopy(normal, seperators);
      seperators[3] = planeDist;
      numseperators++;
      seperators += 4;
      break;
    }
  }
  return numseperators;
  #undef SEP_EPSILON
}

/*
================
CreatePassages

Creates passage visibility structures from one portal to all portals
in the leaf it leads to. Each passage has a cansee bitvector with all
the portals that can be seen through it.
================
*/
int *CreatePassages(LPVOID lpThreadParameter)
{
  int i, j, k, n, numseperators;
  float d;
  Vportal_t *portal, *p, *target;
  Leaf_t *leaf;
  Passage_t *passage, *lastpassage;
  float seperators[MAX_SEPERATORS * 2 * 4];
  Winding_t *w;
  int in[PASSAGE_WINDING_BUF_SIZE], out[PASSAGE_WINDING_BUF_SIZE];
  int *res;

  portal = sorted_portals[(intptr_t)lpThreadParameter];

  if ( portal->removed )
  {
    portal->status = stat_done;
    return (int *)portal;
  }

  lastpassage = NULL;
  leaf = &leafs[portal->leaf];

  for ( i = 0; i < leaf->numportals; i++ )
  {
    target = leaf->portals[i];
    if ( target->removed )
      continue;

    passage = malloc(sizeof(Passage_t) + portalbytes);
    memset(passage, 0, sizeof(Passage_t) + portalbytes);
    numseperators = AddSeperators(portal->winding, target->winding, 0, seperators, MAX_SEPERATORS * 2);
    numseperators += AddSeperators(target->winding, portal->winding, 1, &seperators[numseperators * 4], MAX_SEPERATORS * 2 - numseperators);

    passage->next = NULL;
    if ( lastpassage )
      lastpassage->next = passage;
    else
      portal->passages = passage;
    lastpassage = passage;

    /* build the passage cansee bitvector */
    for ( j = 0; j < numportals * 2; j++ )
    {
      p = &visPortals[j];
      if ( p->removed )
        continue;
      if ( !(target->portalflood[j >> 3] & (1 << (j & 7))) )
        continue;
      if ( !(portal->portalflood[j >> 3] & (1 << (j & 7))) )
        continue;

      /* check if this portal passes all separator planes */
      for ( k = 0; k < numseperators; k++ )
      {
        d = (float)(DotProduct(p->origin, &seperators[k * 4]) - seperators[k * 4 + 3]);
        if ( d < -p->radius + ON_EPSILON )
          break;
        w = p->winding;
        for ( n = 0; n < w->numpoints; n++ )
        {
          d = (float)(DotProduct(w->points[n], &seperators[k * 4]) - seperators[k * 4 + 3]);
          if ( d > ON_EPSILON )
            break;
        }
        if ( n >= w->numpoints )
          break;
      }
      if ( k < numseperators )
        continue;

      /* clip winding through all separator planes */
      /* KIWI FIX (AUDIT_cod4map finding 14): a Winding_t is allocated as exactly
         sizeof(int) + 12 * numpoints bytes (polylib.cpp AllocWinding), so the old fixed
         sizeof(in) copy read up to 148 bytes off the end of a small heap winding, and a
         winding with more than MAX_POINTS_ON_FIXED_WINDING points wrote a numpoints that
         overruns in[]/out[] downstream in PassageChopWinding.  Copy the real extent, and
         reject anything that does not fit. */
      if ( p->winding->numpoints > MAX_POINTS_ON_FIXED_WINDING )
        Com_Error( "CreatePassages: portal winding has %i points (max %i)", p->winding->numpoints, MAX_POINTS_ON_FIXED_WINDING );
      memcpy(in, p->winding, sizeof(int) + sizeof(vec3_t) * p->winding->numpoints);

      for ( k = 0; k < numseperators; k++ )
      {
        res = PassageChopWinding(in, out, &seperators[k * 4]);
        if ( res == out )
          memcpy(in, out, sizeof(in));
        if ( !res )
          break;
      }
      if ( k < numseperators )
        continue;

      passage->cansee[j >> 3] |= 1 << (j & 7);
    }
  }
  return (int *)portal;
}

/*
================
PassageMemory

Reports passage memory usage statistics
================
*/
void PassageMemory(void)
{
  int totalmem;
  int totalportals;
  int i, j;
  Vportal_t *p;
  Leaf_t *leaf;

  totalmem = 0;
  totalportals = 0;

  for ( i = 0; i < numportals; i++ )
  {
    p = sorted_portals[i];
    if ( p->removed )
      continue;

    leaf = &leafs[p->leaf];
    for ( j = 0; j < leaf->numportals; j++ )
    {
      if ( !leaf->portals[j]->removed )
      {
        totalmem += (int)(sizeof(Passage_t) + portalbytes);
        ++totalportals;
      }
    }
  }

  if ( numportals )
  {
    Com_Printf("%7i average number of passages per leaf\n", totalportals / numportals);
    Com_Printf("%7i MB required passage memory\n", totalmem >> 20);
  }
}

/*
================
SimpleFlood

Flood-fills portalflood bitvector through portal front-facing connectivity
================
*/
Leaf_t *SimpleFlood(Vportal_t *src, int leafnum)
{
  Leaf_t *leaf;
  Vportal_t *p;
  int pidx, pbit, pbyte;
  int i;

  leaf = &leafs[leafnum];

  for ( i = 0; i < leaf->numportals; i++ )
  {
    p = leaf->portals[i];
    if ( p->removed )
      continue;

    pidx = (int)(p - visPortals);
    pbit = 1 << (pidx & 7);
    pbyte = pidx >> 3;

    if ( !(src->portalfront[pbyte] & pbit) )
      continue;
    if ( src->portalflood[pbyte] & pbit )
      continue;

    src->portalflood[pbyte] |= pbit;
    SimpleFlood(src, p->leaf);
  }
  return leaf;
}

/*
================
BasePortalVis

First-order visibility approximation. Builds portalfront bitvector
by checking which portals face each other, then flood-fills portalflood.
================
*/
int BasePortalVis(LPVOID lpThreadParameter)
{
  int j, k, portalnum;
  Vportal_t *tp, *p;
  float d;
  Winding_t *w;
  vec3_t dir;
  float fogclipDist;

  portalnum = (intptr_t)lpThreadParameter;
  p = &visPortals[portalnum];

  if ( p->removed )
    return 0;

  p->portalfront = malloc(portalbytes);
  memset(p->portalfront, 0, portalbytes);
  p->portalflood = malloc(portalbytes);
  memset(p->portalflood, 0, portalbytes);
  p->portalvis = malloc(portalbytes);
  memset(p->portalvis, 0, portalbytes);

  fogclipDist = (float)FloatForKey(g_entities, "fogclip");

  for ( j = 0, tp = visPortals; j < numportals * 2; j++, tp++ )
  {
    if ( j == portalnum )
      continue;
    if ( tp->removed )
      continue;

    /* fog clip distance culling */
    if ( fogclipDist > 0.0f )
    {
      VectorSubtract(p->origin, tp->origin, dir);
      if ( VectorLength(dir) - p->radius - tp->radius > fogclipDist )
        continue;
    }

    /* check if any of tp's winding points are in front of p's plane */
    w = tp->winding;
    for ( k = 0; k < w->numpoints; k++ )
    {
      d = (float)(DotProduct021(w->points[k], p->plane) - p->plane[3]);
      if ( d > ON_EPSILON )
        break;
    }
    if ( k == w->numpoints )
      continue;   /* no points on front */

    /* check if any of p's winding points are behind tp's plane */
    w = p->winding;
    for ( k = 0; k < w->numpoints; k++ )
    {
      d = (float)(DotProduct201(w->points[k], tp->plane) - tp->plane[3]);
      if ( d < -ON_EPSILON )
        break;
    }
    if ( k == w->numpoints )
      continue;   /* no points behind */

    p->portalfront[j >> 3] |= 1 << (j & 7);
  }

  SimpleFlood(p, p->leaf);
  p->nummightsee = CountBits(p->portalflood, numportals * 2);
  visFloodCount += p->nummightsee;
  return 0;
}

/*
================
RecursiveLeafBitFlow

Second order approximation of portal visibility via recursive bit flood fill
================
*/
Leaf_t *RecursiveLeafBitFlow( int leafnum, unsigned char *mightsee, unsigned char *cansee )
{
  Leaf_t *leaf;
  Vportal_t *p;
  int i, j, pnum;
  uint32_t more;
  uint32_t newmight[sizeof(((Pstack_t *)0)->mightsee) / sizeof(uint32_t)];

  leaf = &leafs[leafnum];

  /* check all portals for flowing into other leafs */
  for ( i = 0; i < leaf->numportals; i++ )
  {
    p = leaf->portals[i];
    if ( p->removed )
      continue;
    pnum = (int)(p - visPortals);

    /* if some previous portal can't see it, skip */
    if ( !(((unsigned char *)mightsee)[pnum >> 3] & (1 << (pnum & 7))) )
      continue;

    /* if this portal can see some portals we mightsee, recurse */
    more = 0;
    for ( j = 0; j < visPortalLongs; j++ )
    {
      newmight[j] = ((uint32_t *)mightsee)[j] & ((uint32_t *)p->portalflood)[j];
      more |= newmight[j] & ~((uint32_t *)cansee)[j];
    }

    if ( !more )
      continue;

    ((unsigned char *)cansee)[pnum >> 3] |= (1 << (pnum & 7));
    RecursiveLeafBitFlow( p->leaf, (unsigned char *)newmight, cansee );
  }

  return leaf;
}

/*
================
VisChopWinding

Clips a winding by a split plane, using stack-allocated windings for temporaries.
Returns the clipped winding or NULL if fully clipped away.
================
*/
Winding_t *VisChopWinding( Winding_t *in, Pstack_t *stack, float *split )
{
  float dists[MAX_HULL_POINTS];
  int sides[MAX_HULL_POINTS];
  int counts[3];
  float dot;
  int i, j;
  float *p1, *p2;
  vec3_t mid;
  Winding_t *neww;

  counts[0] = counts[1] = counts[2] = 0;

  /* determine sides for each point */
  for ( i = 0; i < in->numpoints; i++ )
  {
    dot = DotProduct201( in->points[i], split ) - split[3];
    dists[i] = dot;
    if ( dot > ON_EPSILON )
      sides[i] = SIDE_FRONT;
    else if ( dot < -ON_EPSILON )
      sides[i] = SIDE_BACK;
    else
      sides[i] = SIDE_ON;
    counts[sides[i]]++;
  }

  if ( !counts[SIDE_BACK] )
    return in;  /* completely on front side */

  if ( !counts[SIDE_FRONT] )
  {
    FreeStackWinding( in, stack );
    return NULL;
  }

  sides[i] = sides[0];
  dists[i] = dists[0];

  neww = AllocStackWinding( stack );
  neww->numpoints = 0;

  for ( i = 0; i < in->numpoints; i++ )
  {
    p1 = in->points[i];

    if ( neww->numpoints == MAX_POINTS_ON_FIXED_WINDING )
    {
      FreeStackWinding( neww, stack );
      return in;  /* can't chop — fall back to original */
    }

    if ( sides[i] == SIDE_ON )
    {
      VectorCopy( p1, neww->points[neww->numpoints] );
      neww->numpoints++;
      continue;
    }

    if ( sides[i] == SIDE_FRONT )
    {
      VectorCopy( p1, neww->points[neww->numpoints] );
      neww->numpoints++;
    }

    if ( sides[i + 1] == SIDE_ON || sides[i + 1] == sides[i] )
      continue;

    if ( neww->numpoints == MAX_POINTS_ON_FIXED_WINDING )
    {
      FreeStackWinding( neww, stack );
      return in;
    }

    /* generate a split point — avoid round-off error when possible */
    p2 = in->points[(i + 1) % in->numpoints];
    dot = DIVS(dists[i], dists[i], dists[i + 1]);
    for ( j = 0; j < 3; j++ )
    {
      if ( split[j] == 1.0f )
        mid[j] = split[3];
      else if ( split[j] == -1.0f )
        mid[j] = -split[3];
      else
        mid[j] = FMA1(p1[j], dot, p2[j] - p1[j]);
    }

    VectorCopy( mid, neww->points[neww->numpoints] );
    neww->numpoints++;
  }

  /* free the original winding */
  FreeStackWinding( in, stack );
  return neww;
}

/*
================
ClipToSeperators

Generates separating planes between source and pass portals,
then clips the target winding by each valid separating plane.
================
*/
Winding_t *ClipToSeperators( Winding_t *source, Winding_t *pass, Winding_t *target, int flipclip, Pstack_t *stack )
{
  int i, j, k, l;
  float plane[4];
  vec3_t v1, v2;
  float d, invLength;
  long double lengthSq;
  int fliptest, frontCount;

  /* check all combinations */
  for ( i = 0; i < source->numpoints; i++ )
  {
    l = (i + 1) % source->numpoints;
    VectorSubtract( source->points[l], source->points[i], v1 );

    /* find a vertex of pass that makes a valid separating plane */
    for ( j = 0; j < pass->numpoints; j++ )
    {
      VectorSubtract( pass->points[j], source->points[i], v2 );

      CrossProduct( v1, v2, plane );

      /* if points don't make a valid plane, skip it */
      lengthSq = (long double)plane[0] * plane[0] + (long double)plane[1] * plane[1] + (long double)plane[2] * plane[2];
      if ( lengthSq < ON_EPSILON )
        continue;

      invLength = (float)(1.0 / sqrt( lengthSq ));
      VectorScale( plane, invLength, plane );
      plane[3] = DotProduct201( pass->points[j], plane );

      /* find out which side of the plane has the source portal */
      fliptest = 0;
      for ( k = 0; k < source->numpoints; k++ )
      {
        if ( k == i || k == l )
          continue;
        d = DotProduct201( source->points[k], plane ) - plane[3];
        if ( d < -ON_EPSILON )
        {
          fliptest = 0;
          break;
        }
        if ( d > ON_EPSILON )
        {
          fliptest = 1;
          break;
        }
      }
      if ( k == source->numpoints )
        continue;  /* planar with source portal */

      /* flip the normal if the source portal is backwards */
      if ( fliptest )
      {
        VectorNegate( plane, plane );
        plane[3] = -plane[3];
      }

      /* check all pass portal points are on the positive side */
      frontCount = 0;
      for ( k = 0; k < pass->numpoints; k++ )
      {
        if ( k == j )
          continue;
        d = DotProduct201( pass->points[k], plane ) - plane[3];
        if ( d < -ON_EPSILON )
          break;
        if ( d > ON_EPSILON )
          frontCount++;
      }
      if ( k != pass->numpoints )
        continue;  /* points on negative side, not a separating plane */
      if ( !frontCount )
        continue;  /* planar with separating plane */

      /* flip the normal if we want the back side */
      if ( flipclip )
      {
        VectorNegate( plane, plane );
        plane[3] = -plane[3];
      }

      /* cache the separator */
      Vector4Copy( plane, stack->seperators[flipclip][stack->numseperators[flipclip]] );
      if ( ++stack->numseperators[flipclip] >= MAX_SEPERATORS )
        Com_Error( "MAX_SEPERATORS" );

      /* fast sphere cull against target portal */
      d = DotProduct210( stack->portal->origin, plane ) - plane[3];
      if ( d < -stack->portal->radius )
        return NULL;
      if ( d > stack->portal->radius )
        break;

      /* clip target by the separating plane */
      target = VisChopWinding( target, stack, plane );
      if ( !target )
        return NULL;

      break;
    }
  }

  return target;
}

/*
================
RecursiveLeafFlow

Main recursive visibility flood fill through leaf portals.
Clips windings by separating planes to determine final PVS.
================
*/
Leaf_t *RecursiveLeafFlow( int leafnum, VisThreadData_t *thread, Pstack_t *prevstack )
{
  Pstack_t stack;
  Pstack_t *rootstack = &thread->pstack_head;
  Vportal_t *p;
  Leaf_t *leaf;
  uint32_t *test, *might, *prevmight, *vis, more;
  int i, j, n, pnum;
  float d;
  float backplane[4];

  thread->c_chains++;

  leaf = &leafs[leafnum];

  prevstack->next = &stack;
  stack.next = NULL;
  stack.leaf = leaf;
  stack.portal = NULL;
  stack.depth = prevstack->depth + 1;
  stack.numseperators[0] = 0;
  stack.numseperators[1] = 0;

  might = (uint32_t *)stack.mightsee;
  vis = (uint32_t *)thread->portal->portalvis;

  /* check all portals for flowing into other leafs */
  for ( i = 0; i < leaf->numportals; i++ )
  {
    p = leaf->portals[i];
    if ( p->removed )
      continue;
    pnum = (int)(p - visPortals);

    if ( !(prevstack->mightsee[pnum >> 3] & (1 << (pnum & 7))) )
      continue;  /* can't possibly see it */

    /* if the portal can't see anything we haven't already seen, skip */
    if ( p->status == stat_done )
      test = (uint32_t *)p->portalvis;
    else
      test = (uint32_t *)p->portalflood;

    more = 0;
    prevmight = (uint32_t *)prevstack->mightsee;
    for ( j = 0; j < visPortalLongs; j++ )
    {
      might[j] = prevmight[j] & test[j];
      more |= (might[j] & ~vis[j]);
    }

    if ( !more && (thread->portal->portalvis[pnum >> 3] & (1 << (pnum & 7))) )
      continue;  /* can't see anything new */

    /* get plane of portal, point normal into the neighbor leaf */
    Vector4Copy( p->plane, stack.portalplane );
    VectorNegate( p->plane, backplane );
    backplane[3] = -p->plane[3];

    stack.portal = p;
    stack.next = NULL;
    stack.freewindings[0] = 1;
    stack.freewindings[1] = 1;
    stack.freewindings[2] = 1;

    /* portal sphere vs source portal plane — fast cull */
    d = DotProduct210( p->origin, rootstack->portalplane ) - rootstack->portalplane[3];
    if ( d < -p->radius )
      continue;
    else if ( d > p->radius )
      stack.pass = p->winding;
    else
    {
      stack.pass = VisChopWinding( p->winding, &stack, rootstack->portalplane );
      if ( !stack.pass )
        continue;
    }

    /* source portal sphere vs target portal plane — fast cull */
    d = DotProduct210( thread->portal->origin, p->plane ) - p->plane[3];
    if ( d > thread->portal->radius )
      continue;
    else if ( d < -thread->portal->radius )
      stack.source = prevstack->source;
    else
    {
      stack.source = VisChopWinding( prevstack->source, &stack, backplane );
      if ( !stack.source )
        continue;
    }

    if ( !prevstack->pass )
    {
      /* the second leaf can only be blocked if coplanar */
      thread->portal->portalvis[pnum >> 3] |= (1 << (pnum & 7));
      RecursiveLeafFlow( p->leaf, thread, &stack );
      continue;
    }

    /* clip by cached separators or compute them */
    if ( stack.numseperators[0] )
    {
      for ( n = 0; n < stack.numseperators[0]; n++ )
      {
        stack.pass = VisChopWinding( stack.pass, &stack, stack.seperators[0][n] );
        if ( !stack.pass )
          break;
      }
      if ( n < stack.numseperators[0] )
        continue;
    }
    else
    {
      stack.pass = ClipToSeperators( prevstack->source, prevstack->pass, stack.pass, 0, &stack );
    }
    if ( !stack.pass )
      continue;

    if ( stack.numseperators[1] )
    {
      for ( n = 0; n < stack.numseperators[1]; n++ )
      {
        stack.pass = VisChopWinding( stack.pass, &stack, stack.seperators[1][n] );
        if ( !stack.pass )
          break;
      }
    }
    else
    {
      stack.pass = ClipToSeperators( prevstack->pass, prevstack->source, stack.pass, 1, &stack );
    }
    if ( !stack.pass )
      continue;

    /* mark the portal as visible and recurse */
    thread->portal->portalvis[pnum >> 3] |= (1 << (pnum & 7));
    RecursiveLeafFlow( p->leaf, thread, &stack );
    stack.next = NULL;
  }

  return leaf;
}

/*
================
PortalFlow

Full vis entry point per portal. Sets up thread data and calls RecursiveLeafFlow
to compute final portalvis bits. Called as a thread worker function.
================
*/
int PortalFlow( LPVOID lpThreadParameter )
{
  VisThreadData_t data;
  int i, c_might, c_can;
  Vportal_t *p;

  p = sorted_portals[(intptr_t)lpThreadParameter];
  if ( p->removed )
  {
    p->status = stat_done;
    return 0;
  }

  p->status = stat_working;
  c_might = CountBits( p->portalflood, 2 * numportals );

  memset( &data, 0, sizeof(data) );
  data.portal = p;
  data.pstack_head.portal = p;
  data.pstack_head.source = p->winding;
  Vector4Copy( p->plane, data.pstack_head.portalplane );
  data.pstack_head.depth = 0;
  for ( i = 0; i < visPortalLongs; i++ )
    ((uint32_t *)data.pstack_head.mightsee)[i] = ((uint32_t *)p->portalflood)[i];

  RecursiveLeafFlow( p->leaf, &data, &data.pstack_head );
  p->status = stat_done;

  c_can = CountBits( p->portalvis, 2 * numportals );
  return Com_DPrintf( "portal:%4i  mightsee:%4i  cansee:%4i (%i chains)\n",
    (int)(p - visPortals), c_might, c_can, data.c_chains );
}

/*
================
RecursivePassagePortalFlow

Recursive passage and portal combined visibility flood fill.
Uses precomputed passage data for faster separating plane tests.
================
*/
int RecursivePassagePortalFlow( Vportal_t *portal, VisThreadData_t *thread, Pstack_t *prevstack )
{
  Pstack_t stack;
  Pstack_t *rootstack = &thread->pstack_head;
  Vportal_t *srcPortal = portal;
  Vportal_t *p;
  Leaf_t *leaf;
  uint32_t *test, *might, *vis, more;
  uint32_t *cansee;
  Passage_t *passage;
  int i, j, n, pnum;
  float d;
  float backplane[4];

  leaf = &leafs[srcPortal->leaf];
  passage = srcPortal->passages;

  prevstack->next = &stack;
  stack.next = NULL;
  stack.leaf = leaf;
  stack.portal = NULL;
  stack.depth = prevstack->depth + 1;
  stack.numseperators[0] = 0;
  stack.numseperators[1] = 0;

  vis = (uint32_t *)thread->portal->portalvis;

  /* check all portals for flowing into other leafs */
  for ( i = 0; i < leaf->numportals; i++ )
  {
    p = leaf->portals[i];
    if ( p->removed )
      continue;

    /* advance passage pointer for this portal */
    cansee = (uint32_t *)passage->cansee;
    passage = passage->next;

    pnum = (int)(p - visPortals);
    if ( !(prevstack->mightsee[pnum >> 3] & (1 << (pnum & 7))) )
      continue;

    /* intersect mightsee with passage cansee and portal flood/vis */
    memcpy( stack.mightsee, prevstack->mightsee, portalbytes );
    might = (uint32_t *)stack.mightsee;
    test = (uint32_t *)(p->status == stat_done ? p->portalvis : p->portalflood);

    more = 0;
    for ( j = 0; j < visPortalLongs; j++ )
    {
      if ( might[j] )
      {
        might[j] &= cansee[j] & test[j];
        more |= (might[j] & ~vis[j]);
      }
    }

    if ( !more && (thread->portal->portalvis[pnum >> 3] & (1 << (pnum & 7))) )
      continue;

    /* get plane of portal, point normal into the neighbor leaf */
    Vector4Copy( p->plane, stack.portalplane );
    VectorNegate( p->plane, backplane );
    backplane[3] = -p->plane[3];

    stack.portal = p;
    stack.next = NULL;
    stack.freewindings[0] = 1;
    stack.freewindings[1] = 1;
    stack.freewindings[2] = 1;

    /* portal sphere vs source portal plane — fast cull */
    d = DotProduct210( p->origin, rootstack->portalplane ) - rootstack->portalplane[3];
    if ( d < -p->radius )
      continue;
    else if ( d > p->radius )
      stack.pass = p->winding;
    else
    {
      stack.pass = VisChopWinding( p->winding, &stack, rootstack->portalplane );
      if ( !stack.pass )
        continue;
    }

    /* source portal sphere vs target portal plane — fast cull */
    d = DotProduct210( thread->portal->origin, p->plane ) - p->plane[3];
    if ( d > thread->portal->radius )
      continue;
    else if ( d < -thread->portal->radius )
      stack.source = prevstack->source;
    else
    {
      stack.source = VisChopWinding( prevstack->source, &stack, backplane );
      if ( !stack.source )
        continue;
    }

    if ( !prevstack->pass )
    {
      /* the second leaf can only be blocked if coplanar */
      thread->portal->portalvis[pnum >> 3] |= (1 << (pnum & 7));
      RecursivePassagePortalFlow( p, thread, &stack );
      continue;
    }

    /* clip by cached separators or compute them */
    if ( stack.numseperators[0] )
    {
      for ( n = 0; n < stack.numseperators[0]; n++ )
      {
        stack.pass = VisChopWinding( stack.pass, &stack, stack.seperators[0][n] );
        if ( !stack.pass )
          break;
      }
      if ( n < stack.numseperators[0] )
        continue;
    }
    else
    {
      stack.pass = ClipToSeperators( prevstack->source, prevstack->pass, stack.pass, 0, &stack );
    }
    if ( !stack.pass )
      continue;

    if ( stack.numseperators[1] )
    {
      for ( n = 0; n < stack.numseperators[1]; n++ )
      {
        stack.pass = VisChopWinding( stack.pass, &stack, stack.seperators[1][n] );
        if ( !stack.pass )
          break;
      }
    }
    else
    {
      stack.pass = ClipToSeperators( prevstack->pass, prevstack->source, stack.pass, 1, &stack );
    }
    if ( !stack.pass )
      continue;

    /* mark the portal as visible and recurse */
    thread->portal->portalvis[pnum >> 3] |= (1 << (pnum & 7));
    RecursivePassagePortalFlow( p, thread, &stack );
    stack.next = NULL;
  }

  return 0;
}

/*
================
PassagePortalFlow

Passage and portal vis entry point per portal. Sets up thread data
and calls RecursivePassagePortalFlow. Called as a thread worker function.
================
*/
int PassagePortalFlow( LPVOID lpThreadParameter )
{
  VisThreadData_t data;
  int i;
  Vportal_t *p;

  p = sorted_portals[(intptr_t)lpThreadParameter];
  if ( p->removed )
  {
    p->status = stat_done;
    return 0;
  }

  p->status = stat_working;

  memset( &data, 0, sizeof(data) );
  data.portal = p;
  data.pstack_head.portal = p;
  data.pstack_head.source = p->winding;
  Vector4Copy( p->plane, data.pstack_head.portalplane );
  data.pstack_head.depth = 0;
  for ( i = 0; i < visPortalLongs; i++ )
    ((uint32_t *)data.pstack_head.mightsee)[i] = ((uint32_t *)p->portalflood)[i];

  RecursivePassagePortalFlow( p, &data, &data.pstack_head );
  p->status = stat_done;
  return 0;
}
