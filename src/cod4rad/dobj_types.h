#ifndef DOBJ_TYPES_H
#define DOBJ_TYPES_H

#define DOBJ_MAX_PARTS      128
#define DOBJ_MAX_SUBMODELS  8
#define DOBJ_MAX_PART_BITS  4

#define IS_NAN(x) (((*(unsigned int *)&(x)) & 0x7F800000) == 0x7F800000)

#define COLL_BOUNDS_MARGIN  0.125f
#define COLL_NORMAL_EPSILON 0.001f

#define XMODEL_COLLSURFS(m)        (*(XModelCollSurf_t **)((char *)(m) + 0xA8))
#define XMODEL_SET_COLLSURFS(m, v) (*(XModelCollSurf_t **)((char *)(m) + 0xA8) = (v))

#define XMODEL_PARTS_SKEL(parts) ((DSkel_t *)&(parts)->skel_partBits)

#endif /* DOBJ_TYPES_H */
