#pragma once
struct cbrush_t;
struct cbrushside_t;
struct CollisionPartition;
void DB64_LoadBrushSide(cbrushside_t *side, bool atStreamStart);
void DB64_LoadCollisionPartition(CollisionPartition *partition, bool atStreamStart);
void DB64_LoadCollisionBrush(cbrush_t *brush, bool atStreamStart);
