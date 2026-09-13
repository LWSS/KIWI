#include <universal/q_shared.h>
#include <gfx_d3d/r_material.h>
#include <gfx_d3d/r_image.h>
#include "native_material.h"
#include "native_water.h"
#include "native_image.h"
#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

void Linker_FreeWater(water_t *water)
{
    if (water)
    {
        free(water->H0);
        free(water->wTerm);
        Linker_FreeImage(water->image);
        free(water);
    }
}

static double WaterRandom(uint32_t *state)
{
    *state ^= *state << 13;
    *state ^= *state >> 17;
    *state ^= *state << 5;
    return ((double)*state + 0.5) / 4294967296.0;
}

bool Linker_CompileWater(const LinkerMaterialTexture *source, const char *imageName, water_t **water, char *error,
                         size_t errorSize)
{
    if (!water || (!error && errorSize))
    {
        return false;
    }
    *water = NULL;
    bool valid = source && imageName && imageName[0] && source->semantic == TS_WATER_MAP && source->waterWidth >= 4 &&
                 source->waterWidth <= 64 && !(source->waterWidth & (source->waterWidth - 1));
    for (int i = 0; valid && i < 6; ++i)
    {
        valid = isfinite(source->waterParameters[i]) && (i >= 4 || source->waterParameters[i] > 0);
    }
    valid = valid && (source->waterParameters[4] != 0 || source->waterParameters[5] != 0);
    water_t *result = valid ? (water_t *)calloc(1, sizeof(water_t)) : NULL;
    valid = valid && result;
    if (valid)
    {
        result->M = result->N = source->waterWidth;
        result->Lx = source->waterParameters[0];
        result->Lz = source->waterParameters[1];
        result->amplitude = source->waterParameters[2];
        result->windvel = source->waterParameters[3];
        result->winddir[0] = source->waterParameters[4];
        result->winddir[1] = source->waterParameters[5];
        result->gravity = 800;
        const size_t count = (size_t)result->M * result->N;
        result->H0 = (complex_s *)calloc(count, sizeof(complex_s));
        result->wTerm = (float *)calloc(count, sizeof(float));
        result->image = (GfxImage *)calloc(1, sizeof(GfxImage));
        valid = result->H0 && result->wTerm && result->image;
    }
    if (valid)
    {
        GfxImage *image = result->image;
        image->name = _strdup(imageName);
        image->texture.loadDef = (GfxImageLoadDef *)calloc(1, sizeof(GfxImageLoadDef));
        valid = image->name && image->texture.loadDef;
        if (valid)
        {
            image->category = IMG_CATEGORY_WATER;
            image->semantic = TS_WATER_MAP;
            image->track = IMAGE_TRACK_WORLD;
            image->mapType = MAPTYPE_2D;
            image->noPicmip = true;
            image->width = (uint16_t)result->M;
            image->height = (uint16_t)result->N;
            image->depth = 1;
            GfxImageLoadDef *definition = image->texture.loadDef;
            definition->dimensions[0] = image->width;
            definition->dimensions[1] = image->height;
            definition->dimensions[2] = 1;
            definition->flags = IMG_FLAG_NOPICMIP | IMG_FLAG_DYNAMIC;
            definition->format = D3DFMT_L8;
        }
    }
    if (valid)
    {
        // Same spectrum as R_PickWaterFrequencies; local RNG makes compiler output reproducible.
        const double tau = 6.283185482025146;
        const double nScale = tau / ((double)result->N * result->Lx);
        const double mScale = tau / ((double)result->M * result->Lz);
        const double windSquared = (double)result->windvel * result->windvel;
        const double lengthSquared = windSquared * windSquared / result->gravity;
        uint32_t randomState = 0x6D2B79F5;
        unsigned int i = 0;
        for (int n = -result->N / 2; valid && n < result->N / 2; ++n)
        {
            const double kx = n * nScale;
            for (int m = -result->M / 2; valid && m < result->M / 2; ++m, ++i)
            {
                const double kz = m * mScale;
                const double kSquared = kx * kx + kz * kz;
                const double windFactor = result->winddir[0] * kx + result->winddir[1] * kz;
                const double gaussianRadius = sqrt(-2 * log(WaterRandom(&randomState)));
                const double angle = tau * WaterRandom(&randomState);
                if (windFactor > 0)
                {
                    const double spectrum = result->amplitude * exp(-1 / (lengthSquared * kSquared)) * windFactor *
                                            windFactor / (kSquared * kSquared * kSquared);
                    const double scale = sqrt(spectrum * 0.5) * result->amplitude;
                    result->H0[i].real = (float)(gaussianRadius * cos(angle) * scale);
                    result->H0[i].imag = (float)(gaussianRadius * sin(angle) * scale);
                    result->wTerm[i] = (float)sqrt(result->gravity * sqrt(kSquared));
                    valid = isfinite(result->H0[i].real) && isfinite(result->H0[i].imag) && isfinite(result->wTerm[i]);
                }
            }
        }
    }
    if (!valid)
    {
        Linker_FreeWater(result);
        if (errorSize)
        {
            snprintf(error, errorSize,
                     "Invalid water setup, nonfinite spectrum, or allocation failure (grid must be 4..64)");
        }
        return false;
    }
    *water = result;
    return true;
}
