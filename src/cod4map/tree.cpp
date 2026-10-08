/*
tree.c — BSP tree utilities

Reconstructed from cod2map.exe by Rose.
*/

#include "cod4map.h"


/*
================
FreeTreePortals_r

Recursively frees all portals in the BSP tree
================
*/
void FreeTreePortals_r(Node_t *node)
{
  Portal_t *p, *next;
  int s;

  if ( node->planenum != PLANENUM_LEAF )
  {
    FreeTreePortals_r(node->children[0]);
    FreeTreePortals_r(node->children[1]);
  }

  for ( p = node->portals; p; p = next )
  {
    s = (p->nodes[1] == node);
    next = p->next[s];
    RemovePortalFromNode(p, p->nodes[!s]);
    FreePortal(p);
  }
  node->portals = NULL;
}

/*
================
FreeTree_r

Recursively frees all BSP tree nodes
================
*/
void FreeTree_r(Node_t *node)
{
  if ( node->planenum != PLANENUM_LEAF )
  {
    FreeTree_r(node->children[0]);
    FreeTree_r(node->children[1]);
  }
  FreeBrushList(node->leafBrushes);
  if ( node->volume )
    FreeBrush(node->volume);
  if ( numthreads == 1 )
    --g_treeNodeCount;
  free(node);
}

/*
================
FreeTreeBlock

Frees entire BSP tree: portals, nodes, then tree struct

================
*/
void FreeTreeBlock(Tree_t *tree)
{
  FreeTreePortals_r(tree->headnode);
  FreeTree_r(tree->headnode);
  free(tree);
}

