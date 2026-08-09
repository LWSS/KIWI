/*
leakfile.c — Leak file generation

Reconstructed from cod2map.exe by Rose.
*/

#include "cod4map.h"

char s_assertDisable_LeakFile;
char s_assertDisable_LeakFile;
char s_assertDisable_PortalLeakFile;
char s_assertDisable_PortalLeakFile;


/*
================
LeakFile

Finds shortest portal chain from outside leaf to occupied leaf, writes .lin file.
================
*/
void LeakFile(Tree_t *tree)
{
  FILE *fp;
  Node_t *nd, *nextnode;
  Portal_t *pp, *nextportal;
  int side, bestOcc, count;
  vec3_t mid;
  char filename[MAX_OS_PATH];

  if ( leakFileWritten )
    return;
  if ( !tree->outside_node.occupied )
    return;

  Com_DPrintf("--- LeakFile ---\n");
  leakFileWritten = 1;

  BuildOutputPathFromLoadSource(".lin", filename, sizeof(filename));
  fp = fopen(filename, "w");
  if ( !fp )
    Com_Error("Couldn't open %s\n", filename);

  /* trace shortest portal chain from outside to occupied leaf */
  nd = &tree->outside_node;
  count = 0;
  while ( nd->occupied > 1 )
  {
    /* find portal leading to node with lowest occupied count */
    bestOcc = nd->occupied;
    nextnode = NULL;
    nextportal = NULL;
    for ( pp = nd->portals; pp; pp = pp->next[pp->nodes[0] != nd] )
    {
      side = (pp->nodes[0] == nd);
      if ( pp->nodes[side]->occupied && pp->nodes[side]->occupied < bestOcc )
      {
        nextnode = pp->nodes[side];
        nextportal = pp;
        bestOcc = nextnode->occupied;
      }
    }

    Assert(nextportal, s_assertDisable_LeakFile);
    Assert(nextnode, s_assertDisable_LeakFile);

    nd = nextnode;
    WindingCenter(nextportal->winding, mid);
    fprintf(fp, "%f %f %f\n", mid[0], mid[1], mid[2]);
    count++;
  }

  /* write final occupied entity origin */
  GetVectorForKey(nd->occupant, "origin", mid);
  fprintf(fp, "%f %f %f\n", mid[0], mid[1], mid[2]);
  fclose(fp);

  printf("\n\n================================\n\nWROTE BSP LEAKFILE: %s\n\n================================\n\n", filename);
  Com_DPrintf("%5i point linefile\n", count + 1);
}

/*
================
FloodLeakPath_r

Recursive flood fill to find leak path through portals.
================
*/
int FloodLeakPath_r(Node_t *node, Node_t *targetNode, int floodDist)
{
  Node_t *n;
  Portal_t *p;
  Node_t *other;
  int side;

  /* recurse through internal nodes */
  for ( n = node; n->planenum != -1; n = n->children[1] )
  {
    if ( FloodLeakPath_r(n->children[0], targetNode, floodDist) )
      return 1;
  }

  if ( n->occupied != floodDist )
    return 0;

  /* flood through portals */
  for ( p = n->portals; p; p = p->next[side] )
  {
    side = (p->nodes[1] == n);
    if ( p->portalFace )
      continue;

    other = p->nodes[!side];
    if ( other->opaque || other->occupied <= floodDist )
      continue;
    if ( IsTinyWinding(p->winding) )
      continue;

    other->occupied = floodDist + 1;
    if ( other == targetNode )
      return 1;
  }
  return 0;
}

/*
================
PortalLeakFile

Writes portal-based leak file using flood fill path tracing.
================
*/
void PortalLeakFile(Tree_t *tree, Portal_t *portal)
{
  FILE *fp;
  Node_t *source, *target, *nextnd;
  Portal_t *pp, *nextportal;
  int floodDist, side, count;
  vec3_t mid, point;
  char filename[MAX_OS_PATH];

  if ( leakFileWritten )
    return;

  source = portal->nodes[0];
  target = portal->nodes[1];
  source->occupied = 0;
  leakFileWritten = 1;

  /* flood fill to find path from source to target */
  for ( floodDist = 0; !FloodLeakPath_r(tree->headnode, target, floodDist); floodDist++ )
    ;

  BuildOutputPathFromLoadSource(".lin", filename, sizeof(filename));
  fp = fopen(filename, "w");
  if ( !fp )
    Com_Error("Couldn't open %s\n", filename);

  /* write starting point offset from portal */
  WindingCenter(portal->winding, mid);
  point[0] = mid[0] - portal->plane[0] * LEAK_POINT_OFFSET;
  point[1] = mid[1] - portal->plane[1] * LEAK_POINT_OFFSET;
  point[2] = mid[2] - portal->plane[2] * LEAK_POINT_OFFSET;
  fprintf(fp, "%f %f %f\n", point[0], point[1], point[2]);
  count = 1;

  /* trace back from target to source through flood path */
  while ( target != source )
  {
    nextportal = NULL;
    nextnd = NULL;
    for ( pp = target->portals; pp; pp = pp->next[pp->nodes[0] != target] )
    {
      side = (pp->nodes[0] == target);
      if ( !pp->portalFace && pp->nodes[side]->occupied == target->occupied - 1 )
      {
        nextportal = pp;
        nextnd = pp->nodes[side];
      }
    }
    Assert(nextnd, s_assertDisable_PortalLeakFile);
    Assert(nextportal, s_assertDisable_PortalLeakFile);

    target = nextnd;
    IsTinyWinding(nextportal->winding);
    WindingCenter(nextportal->winding, point);
    fprintf(fp, "%f %f %f\n", point[0], point[1], point[2]);
    count++;
  }

  /* write ending point offset from portal */
  point[0] = portal->plane[0] * LEAK_POINT_OFFSET + mid[0];
  point[1] = portal->plane[1] * LEAK_POINT_OFFSET + mid[1];
  point[2] = portal->plane[2] * LEAK_POINT_OFFSET + mid[2];
  fprintf(fp, "%f %f %f\n", point[0], point[1], point[2]);
  fclose(fp);

  printf("\n\n================================\n\nWROTE PORTAL LEAKFILE: %s\n\n================================\n\n", filename);
  Com_DPrintf("%5i point linefile\n", count + 1);
}
