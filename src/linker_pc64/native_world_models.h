#pragma once
#include <stddef.h>
struct LinkerBspStaticModel;
struct XModel;
struct GfxStaticModelInst;
struct GfxStaticModelDrawInst;
struct cStaticModel_s;
bool Linker_BuildStaticModelCollision(const GfxStaticModelDrawInst *instances, unsigned int count,
                                      cStaticModel_s **collision, char *error, size_t errorSize);
// Owns the two returned arrays; models are borrowed. Lighting/probe selection and cell assignment follow this step.
bool Linker_BuildStaticModelInstances(const LinkerBspStaticModel *placements, XModel *const *models, unsigned int count,
                                      GfxStaticModelDrawInst **drawInstances, GfxStaticModelInst **instances,
                                      char *error, size_t errorSize);
