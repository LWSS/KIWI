#include <universal/q_shared.h>
#include <xanim/xmodel.h>
#include "db_model_assets.h"
#include <stdio.h>
#include <limits.h>

bool DB64_ValidateModelHeader(const XModel *model, char *error, size_t errorSize)
{
    const char *reason = NULL;
    if (!model || !model->name)
    {
        reason = "Native model has no name";
    }
    else if (model->numBones > 128 || model->numRootBones > model->numBones ||
             (model->numBones && !model->numRootBones) || model->numLods < 0 || model->numLods > 4 ||
             model->collLod < -1 || (model->collLod >= 0 && model->collLod >= model->numLods) ||
             model->lodRampType >= XMODEL_LOD_RAMP_COUNT || model->numCollSurfs < 0 ||
             model->numCollSurfs > INT_MAX / sizeof(XModelCollSurf_s))
    {
        reason = "Invalid native model counts";
    }
    else
    {
        const bool children = model->numBones != model->numRootBones;
        if ((!!model->boneNames != (model->numBones != 0)) || (!!model->baseMat != (model->numBones != 0)) ||
            (!!model->partClassification != (model->numBones != 0)) || (!!model->boneInfo != (model->numBones != 0)) ||
            (!!model->parentList != children) || (!!model->quats != children) || (!!model->trans != children) ||
            (!!model->surfs != (model->numsurfs != 0)) || (!!model->materialHandles != (model->numsurfs != 0)) ||
            (!!model->collSurfs != (model->numCollSurfs != 0)))
        {
            reason = "Inconsistent native model arrays";
        }
        for (int i = 0; i < model->numLods; ++i)
        {
            if ((unsigned int)model->lodInfo[i].surfIndex + model->lodInfo[i].numsurfs > model->numsurfs)
            {
                reason = "Native model LOD exceeds its surfaces";
            }
        }
    }
    if (reason && errorSize)
    {
        snprintf(error, errorSize, "%s", reason);
    }
    return !reason;
}
