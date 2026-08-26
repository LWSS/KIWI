#pragma once

#include <stddef.h>
#include <string>
#include <vector>

struct eclass_t;

enum KiwiEntWidgetType
{
    KIWI_ENT_WIDGET_TEXT,
    KIWI_ENT_WIDGET_COLOR3,
    KIWI_ENT_WIDGET_VEC3,
    KIWI_ENT_WIDGET_FLOAT,
    KIWI_ENT_WIDGET_INT,
    KIWI_ENT_WIDGET_BOOL,
    KIWI_ENT_WIDGET_ANGLE,
    KIWI_ENT_WIDGET_DISTANCE,
    KIWI_ENT_WIDGET_TARGET,
    KIWI_ENT_WIDGET_MODEL,
    KIWI_ENT_WIDGET_DEF
};

struct KiwiEntParameter
{
    std::string       name;
    std::string       description;
    std::string       defaultValue;
    KiwiEntWidgetType widget;
    float             dragSpeed;
    float             dragMin;
    float             dragMax;
    bool              hasDragRange;
};

struct KiwiEntSchema
{
    std::vector<KiwiEntParameter> parameters;
};

// KIWI: schemas are parsed from the raw QUAKED comment and cached per live class.
const KiwiEntSchema &KiwiEntInspect_GetSchema( const eclass_t *eclass, size_t eclassCount );

// KIWI: keep schema and "Other keys" rows on the same widget-inference rules.
KiwiEntParameter KiwiEntInspect_InferParameter( const char *name,
                                                const char *description,
                                                const char *defaultValue );
