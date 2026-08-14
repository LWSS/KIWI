/*
 * dvar.c — Dynamic variable (console variable) system.
 */

#include "cod2rad64.h"
#include <ctype.h>

#define DVAR_MAX 1280
#define DVAR_HASH_SIZE 256

static dvar_t g_dvarPool[DVAR_MAX];
static int g_dvarCount;
static dvar_t *g_dvarHashTable[DVAR_HASH_SIZE];
static dvar_t *g_dvarLinkedList;
static dvar_t *g_svCheats;
static unsigned int g_dvarModifiedFlags;
static char isDvarSystemActive;
static const char *g_emptyString = "";
static const char *g_offString = "off";
static const char *g_onString = "on";
static const char g_digitStrings[20] = {'0',0,'1',0,'2',0,'3',0,'4',0,'5',0,'6',0,'7',0,'8',0,'9',0};

static int g_dvarSuppressTypeAssert;

static char s_assertDisable_Dvar_AssignResetStringValue;
static char s_assertDisable_Dvar_ClampValueToDomain;
static char s_assertDisable_Dvar_ClearModified;
static char s_assertDisable_Dvar_CopyString;
static char s_assertDisable_Dvar_DomainToString;
static char s_assertDisable_Dvar_DomainToString_default;
static char s_assertDisable_Dvar_MakeExplicitType;
static char s_assertDisable_Dvar_ReRegister;
static char s_assertDisable_Dvar_Register_internal_keep;
static char s_assertDisable_Dvar_Register_internal_name;
static char s_assertDisable_Dvar_Register_internal_value;
static char s_assertDisable_Dvar_RegisterString;
static char s_assertDisable_Dvar_RegisterVariant_flags;
static char s_assertDisable_Dvar_RegisterVariant_keep;
static char s_assertDisable_Dvar_Reregister;
static char s_assertDisable_Dvar_Reregister_final;
static char s_assertDisable_Dvar_Reregister_name;
static char s_assertDisable_Dvar_Reregister_reset;
static char s_assertDisable_Dvar_Reregister_string;
static char s_assertDisable_Dvar_Reregister_type;
static char s_assertDisable_Dvar_SetResetValue;
static char s_assertDisable_Dvar_SetVariant;
static char s_assertDisable_Dvar_SetVariant_name;
static char s_assertDisable_Dvar_SetVariant_reset;
static char s_assertDisable_Dvar_SetVariant_string;
static char s_assertDisable_Dvar_SetVariant_stringval;
static char s_assertDisable_Dvar_StringToBool;
static char s_assertDisable_Dvar_StringToEnum;
static char s_assertDisable_Dvar_StringToInt;
static char s_assertDisable_Dvar_StringToFloat;
static char s_assertDisable_Dvar_StringToValue;
static char s_assertDisable_Dvar_StringToVec2;
static char s_assertDisable_Dvar_StringToVec3;
static char s_assertDisable_Dvar_StringToVec4;
static char s_assertDisable_Dvar_UpdateReRegister;
static char s_assertDisable_Dvar_ValueInDomain;
static char s_assertDisable_Dvar_ValuesEqual;
static char s_assertDisable_Dvar_ValueToString;

/*
================
Dvar_CopyString

Interns common strings (empty, "off", "on", single digits).
Otherwise allocates a copy via CopyStringInternal.
================
*/
char *Dvar_CopyString(const char *string)
{
    char c;
    int len;

    Assert(string, s_assertDisable_Dvar_CopyString);

    c = *string;
    if (!c)
        return (char *)g_emptyString;

    len = (int)strlen(string) + 1;

    if (string[1])
    {
        if (c == 'o')
        {
            if (len == 4 && string[1] == 'f' && string[2] == 'f' && !string[3])
                return (char *)g_offString;
            if (len == 3 && string[1] == 'n' && !string[2])
                return (char *)g_onString;
        }
    }
    else if (c >= '0' && c <= '9')
    {
        return (char *)&g_digitStrings[(c - '0') * 2];
    }

    return CopyStringInternal(string);
}

/*
================
Dvar_FreeCurrentStringValue

Frees the current string value if it's not shared with latched/reset
and not an interned string.
================
*/
void Dvar_FreeCurrentStringValue(dvar_t *dvar)
{
    const char *s = dvar->current.string;
    if (s != dvar->latched.string && s != dvar->reset.string)
    {
        if (*s && (s[1] || *s < '0' || *s > '9') && s != g_offString && s != g_onString)
            Z_FreeInternal((void *)s);
    }
    dvar->current.string = NULL;
}

/*
================
Dvar_FreeLatchedStringValue

Frees the latched string value if it's not shared with current/reset
and not an interned string.
================
*/
void Dvar_FreeLatchedStringValue(dvar_t *dvar)
{
    const char *s = dvar->latched.string;
    if (s != dvar->current.string && s != dvar->reset.string)
    {
        if (*s && (s[1] || *s < '0' || *s > '9') && s != g_offString && s != g_onString)
            Z_FreeInternal((void *)s);
    }
    dvar->latched.string = NULL;
}

/*
================
Dvar_AssignCurrentStringValue

Sets current string, reusing latched or reset pointer if matching.
================
*/
void Dvar_AssignCurrentStringValue(dvar_t *dvar, const char *string)
{
    const char *latched = dvar->latched.string;
    if (latched && (string == latched || !strcmp(string, latched)))
    {
        dvar->current.string = latched;
    }
    else
    {
        const char *reset = dvar->reset.string;
        if (reset && (string == reset || !strcmp(string, reset)))
            dvar->current.string = reset;
        else
            dvar->current.string = Dvar_CopyString(string);
    }
}

/*
================
Dvar_AssignResetStringValue

Sets reset string, reusing current or latched pointer if matching.
================
*/
void Dvar_AssignResetStringValue(dvar_t *dvar, const char *string)
{
    const char *match;

    Assert(string, s_assertDisable_Dvar_AssignResetStringValue);
    match = dvar->current.string;
    if (match && (string == match || !strcmp(string, match))
        || (match = dvar->latched.string) != NULL && (string == match || !strcmp(string, match)))
    {
        dvar->reset.string = match;
    }
    else
    {
        dvar->reset.string = Dvar_CopyString(string);
    }
}

/*
================
Dvar_ValueToString

Converts a dvar value to a displayable string based on type.
================
*/
const char *Dvar_ValueToString(dvar_t *dvar, DvarValue_t value)
{
    switch (dvar->type)
    {
    case 0: /* bool */
        return value.enabled ? "1" : "0";
    case 1: /* float */
        return va("%g", value.value);
    case 2: /* vec2 */
        return va("%g %g", value.vector[0], value.vector[1]);
    case 3: /* vec3 */
        return va("%g %g %g", value.vector[0], value.vector[1], value.vector[2]);
    case 4: /* vec4 */
        return va("%g %g %g %g", value.vector[0], value.vector[1], value.vector[2], value.vector[3]);
    case 5: /* int */
        return va("%i", value.integer);
    case 6: /* enum */
        Assert(value.integer >= 0 && value.integer < dvar->domain.enumeration.stringCount || value.integer == 0,
               s_assertDisable_Dvar_ValueToString);
        if (!dvar->domain.enumeration.stringCount)
            return g_emptyString;
        return dvar->domain.enumeration.strings[value.integer];
    case 7: /* string */
        Assert(value.string, s_assertDisable_Dvar_ValueToString);
        return va("%s", value.string);
    case 8: /* color */
        return va("%g %g %g %g",
                  (float)(value.color[0] * 0.0039215689f),
                  (float)(value.color[1] * 0.0039215689f),
                  (float)(value.color[2] * 0.0039215689f),
                  (float)(value.color[3] * 0.0039215689f));
    default:
        if (!g_dvarSuppressTypeAssert)
            Assert(0, s_assertDisable_Dvar_ValueToString);
        return g_emptyString;
    }
}

/*
================
Dvar_StringToBool

Converts string to bool via atol.
================
*/
int Dvar_StringToBool(const char *string)
{
    Assert(string, s_assertDisable_Dvar_StringToBool);
    return atol(string) != 0;
}

/*
================
Dvar_StringToInt

Converts string to int via atol.
================
*/
int Dvar_StringToInt(const char *string)
{
    Assert(string, s_assertDisable_Dvar_StringToInt);
    return atol(string);
}

/*
================
Dvar_StringToFloat

Converts string to float via atof.
================
*/
float Dvar_StringToFloat(const char *string)
{
    Assert(string, s_assertDisable_Dvar_StringToFloat);
    return (float)atof(string);
}

#define DVAR_PARSE_BUF_SLOTS 12
static float dvarParseBuffer[DVAR_PARSE_BUF_SLOTS];
static int g_dvarStringBufIdx;

/*
================
Dvar_StringToVec2

Parses string into vec2 using rotating buffer.
================
*/
float *Dvar_StringToVec2(const char *string)
{
    int idx;
    float *result;

    Assert(string, s_assertDisable_Dvar_StringToVec2);
    idx = g_dvarStringBufIdx;
    if ((unsigned int)(g_dvarStringBufIdx + 2) > DVAR_PARSE_BUF_SLOTS)
        idx = 0;
    result = &dvarParseBuffer[idx];
    g_dvarStringBufIdx = idx + 2;
    result[0] = 0.0f;
    result[1] = 0.0f;
    sscanf(string, "%g %g", &result[0], &result[1]);
    return result;
}

/*
================
Dvar_StringToVec3

Parses string into vec3 using rotating buffer.
================
*/
float *Dvar_StringToVec3(const char *string)
{
    int idx;
    float *result;

    Assert(string, s_assertDisable_Dvar_StringToVec3);
    idx = g_dvarStringBufIdx;
    if ((unsigned int)(g_dvarStringBufIdx + 3) > DVAR_PARSE_BUF_SLOTS)
        idx = 0;
    result = &dvarParseBuffer[idx];
    g_dvarStringBufIdx = idx + 3;
    result[0] = 0.0f;
    result[1] = 0.0f;
    result[2] = 0.0f;
    sscanf(string, "%g %g %g", &result[0], &result[1], &result[2]);
    return result;
}

/*
================
Dvar_StringToVec4

Parses string into vec4 using rotating buffer.
================
*/
float *Dvar_StringToVec4(const char *string)
{
    int idx;
    float *result;

    Assert(string, s_assertDisable_Dvar_StringToVec4);
    idx = g_dvarStringBufIdx;
    if ((unsigned int)(g_dvarStringBufIdx + 4) > DVAR_PARSE_BUF_SLOTS)
        idx = 0;
    result = &dvarParseBuffer[idx];
    g_dvarStringBufIdx = idx + 4;
    result[0] = 0.0f;
    result[1] = 0.0f;
    result[2] = 0.0f;
    result[3] = 0.0f;
    sscanf(string, "%g %g %g %g", &result[0], &result[1], &result[2], &result[3]);
    return result;
}

/*
================
Dvar_StringToEnum

Converts string to enum index by exact match, numeric parse, or prefix match.
================
*/
int Dvar_StringToEnum(DvarLimits_t *domain, const char *string)
{
    int i, numericValue;
    int len;

    Assert(domain, s_assertDisable_Dvar_StringToEnum);
    Assert(string, s_assertDisable_Dvar_StringToEnum);

    for (i = 0; i < domain->enumeration.stringCount; i++)
    {
        if (!I_stricmp(string, domain->enumeration.strings[i]))
            return i;
    }

    /* inline digit-only parser — rejects if any non-digit */
    {
        const char *p = string;
        numericValue = 0;
        if (*p)
        {
            while (*p)
            {
                if (*p < '0' || *p > '9')
                    return -1337;
                numericValue = numericValue * 10 + (*p - '0');
                p++;
            }
        }
        if (numericValue >= 0 && numericValue < domain->enumeration.stringCount)
            return numericValue;
    }

    /* prefix match — only reached for all-digit strings out of range */
    len = (int)strlen(string);
    if (domain->enumeration.stringCount > 0)
    {
        for (i = 0; i < domain->enumeration.stringCount; i++)
        {
            if (!I_strnicmp(string, domain->enumeration.strings[i], len))
                return i;
        }
    }

    return -1337;
}

/*
================
Dvar_StringToColor

Parses string into packed RGBA color bytes, clamping [0,1].
================
*/
void Dvar_StringToColor(const char *string, unsigned char *color)
{
    float v[4];
    int i;

    v[0] = 0.0f; v[1] = 0.0f; v[2] = 0.0f; v[3] = 0.0f;
    sscanf(string, "%g %g %g %g", &v[0], &v[1], &v[2], &v[3]);
    for (i = 0; i < 4; i++)
    {
        float c = v[i];
        if (c > 1.0f) c = 1.0f;
        if (c < 0.0f) c = 0.0f;
        color[i] = (unsigned char)(int)floorf(c * 255.0f + 0.5f);
    }
}

/*
================
Dvar_StringToValue

Converts a string to a DvarValue_t based on type.
================
*/
DvarValue_t Dvar_StringToValue(unsigned char type, DvarLimits_t *domain, const char *string)
{
    DvarValue_t result;

    Assert(string, s_assertDisable_Dvar_StringToValue);

    memset(&result, 0, sizeof(result));
    switch (type)
    {
    case 0: result.enabled = Dvar_StringToBool(string); break;
    case 1: result.value = Dvar_StringToFloat(string); break;
    case 2: result.vector = Dvar_StringToVec2(string); break;
    case 3: result.vector = Dvar_StringToVec3(string); break;
    case 4: result.vector = Dvar_StringToVec4(string); break;
    case 5: result.integer = Dvar_StringToInt(string); break;
    case 6: result.integer = Dvar_StringToEnum(domain, string); break;
    case 7: result.string = string; break;
    case 8: Dvar_StringToColor(string, result.color); break;
    default:
        if (!g_dvarSuppressTypeAssert)
            Assert(0, s_assertDisable_Dvar_StringToValue);
        memset(&result, 0, sizeof(result));
        break;
    }
    return result;
}

/*
================
Dvar_ClampValueToDomain

Clamps a dvar value to its domain limits based on type.
================
*/
DvarValue_t Dvar_ClampValueToDomain(unsigned char type, DvarValue_t value, DvarValue_t resetValue, DvarLimits_t domain)
{
    int i;

    switch (type)
    {
    case 0:
        value.enabled = value.enabled != 0;
        break;
    case 1:
        if (domain.decimal.min > value.value)
            value.value = domain.decimal.min;
        else if (value.value > domain.decimal.max)
            value.value = domain.decimal.max;
        break;
    case 2:
        for (i = 0; i < 2; i++)
        {
            if (value.vector[i] < domain.decimal.min)
                value.vector[i] = domain.decimal.min;
            else if (value.vector[i] > domain.decimal.max)
                value.vector[i] = domain.decimal.max;
        }
        break;
    case 3:
        for (i = 0; i < 3; i++)
        {
            if (value.vector[i] < domain.decimal.min)
                value.vector[i] = domain.decimal.min;
            else if (value.vector[i] > domain.decimal.max)
                value.vector[i] = domain.decimal.max;
        }
        break;
    case 4:
        for (i = 0; i < 4; i++)
        {
            if (value.vector[i] < domain.decimal.min)
                value.vector[i] = domain.decimal.min;
            else if (value.vector[i] > domain.decimal.max)
                value.vector[i] = domain.decimal.max;
        }
        break;
    case 5:
        Assert(domain.integer.min <= domain.integer.max, s_assertDisable_Dvar_ClampValueToDomain);
        if (value.integer < domain.integer.min)
            value.integer = domain.integer.min;
        else if (value.integer > domain.integer.max)
            value.integer = domain.integer.max;
        break;
    case 6:
        if (value.integer < 0 || value.integer >= domain.enumeration.stringCount)
        {
            value.integer = resetValue.integer;
            Assert(value.integer >= 0 && value.integer < domain.enumeration.stringCount || value.integer == 0,
                   s_assertDisable_Dvar_ClampValueToDomain);
        }
        break;
    case 7:
    case 8:
        break;
    default:
        if (!g_dvarSuppressTypeAssert)
            Assert(0, s_assertDisable_Dvar_ClampValueToDomain);
        break;
    }
    return value;
}

/*
================
Dvar_ValueInDomain

Returns true if a value is within the domain limits.
================
*/
int Dvar_ValueInDomain(unsigned char type, DvarValue_t value, DvarLimits_t domain)
{
    int i;

    switch (type)
    {
    case 0:
        Assert(value.enabled == 0 || value.enabled == 1, s_assertDisable_Dvar_ValueInDomain);
        return 1;
    case 1:
        return domain.decimal.min <= value.value && value.value <= domain.decimal.max;
    case 2:
        for (i = 0; i < 2; i++)
            if (value.vector[i] < domain.decimal.min || value.vector[i] > domain.decimal.max) return 0;
        return 1;
    case 3:
        for (i = 0; i < 3; i++)
            if (value.vector[i] < domain.decimal.min || value.vector[i] > domain.decimal.max) return 0;
        return 1;
    case 4:
        for (i = 0; i < 4; i++)
            if (value.vector[i] < domain.decimal.min || value.vector[i] > domain.decimal.max) return 0;
        return 1;
    case 5:
        Assert(domain.integer.min <= domain.integer.max, s_assertDisable_Dvar_ValueInDomain);
        return value.integer >= domain.integer.min && value.integer <= domain.integer.max;
    case 6:
        return value.integer >= 0 && value.integer < domain.enumeration.stringCount || !value.integer;
    case 7:
    case 8:
        return 1;
    default:
        if (!g_dvarSuppressTypeAssert)
            Assert(0, s_assertDisable_Dvar_ValueInDomain);
        return 0;
    }
}

/*
================
Dvar_ValuesEqual

Returns true if two dvar values are equal for the given type.
================
*/
int Dvar_ValuesEqual(unsigned char type, DvarValue_t val0, DvarValue_t val1)
{
    switch (type)
    {
    case 0:
        return val0.enabled == val1.enabled;
    case 1:
        return val0.value == val1.value;
    case 2:
        return val0.vector[0] == val1.vector[0] && val0.vector[1] == val1.vector[1];
    case 3:
        return val0.vector[0] == val1.vector[0] && val0.vector[1] == val1.vector[1] && val0.vector[2] == val1.vector[2];
    case 4:
        return val0.vector[0] == val1.vector[0] && val0.vector[1] == val1.vector[1]
            && val0.vector[2] == val1.vector[2] && val0.vector[3] == val1.vector[3];
    case 5:
    case 6:
    case 8:
        return val0.integer == val1.integer;
    case 7:
        Assert(val0.string, s_assertDisable_Dvar_ValuesEqual);
        Assert(val1.string, s_assertDisable_Dvar_ValuesEqual);
        return !strcmp(val0.string, val1.string);
    default:
        if (!g_dvarSuppressTypeAssert)
            Assert(0, s_assertDisable_Dvar_ValuesEqual);
        return 0;
    }
}

/*
================
Dvar_ClearModified

Clears the modified flag on a dvar.
================
*/
void Dvar_ClearModified(dvar_t *dvar)
{
    Assert(dvar, s_assertDisable_Dvar_ClearModified);
    dvar->modified = 0;
}

/*
================
Dvar_GenerateHashValue

Hash of dvar name for hash table lookup.
================
*/
static int Dvar_GenerateHashValue(const char *name)
{
    int hash = 0;
    int mul = 119;
    while (*name)
    {
        char c = *name++;
        hash += tolower((int)c) * mul;
        mul++;
    }
    return hash & (DVAR_HASH_SIZE - 1);
}

/*
================
Dvar_FindVar

Looks up a dvar by name using the hash table.
================
*/
dvar_t *Dvar_FindVar(const char *name)
{
    dvar_t *dvar;

    dvar = g_dvarHashTable[Dvar_GenerateHashValue(name)];
    while (dvar)
    {
        if (!I_stricmp(name, dvar->name))
            return dvar;
        dvar = dvar->hashNext;
    }
    return NULL;
}

/*
================
Dvar_RegisterBool

Registers a boolean dvar.
================
*/
dvar_t *Dvar_RegisterBool(const char *name, int value, unsigned short flags)
{
    DvarValue_t val;
    DvarLimits_t dom;

    memset(&val, 0, sizeof(val));
    memset(&dom, 0, sizeof(dom));
    val.enabled = value;
    return Dvar_RegisterVariant(name, DVAR_TYPE_BOOL, flags, val, &dom);
}

/*
================
Dvar_RegisterInt

Registers an integer dvar with min/max domain.
================
*/
dvar_t *Dvar_RegisterInt(const char *name, int value, int min, int max, unsigned short flags)
{
    DvarValue_t val;
    DvarLimits_t dom;

    memset(&val, 0, sizeof(val));
    memset(&dom, 0, sizeof(dom));
    val.integer = value;
    dom.integer.min = min;
    dom.integer.max = max;
    return Dvar_RegisterVariant(name, DVAR_TYPE_INT, flags, val, &dom);
}

/*
================
Dvar_RegisterString

Registration wrapper for string dvars.
================
*/
dvar_t *Dvar_RegisterString(const char *name, const char *value, unsigned short flags)
{
    DvarValue_t val;
    DvarLimits_t dom;

    Assert(name, s_assertDisable_Dvar_RegisterString);
    Assert(value, s_assertDisable_Dvar_RegisterString);

    memset(&val, 0, sizeof(val));
    memset(&dom, 0, sizeof(dom));
    val.string = value;
    return Dvar_RegisterVariant(name, DVAR_TYPE_STRING, flags, val, &dom);
}

/*
================
Dvar_SetStringValue

Sets a dvar's value from a raw string.
Takes dvar_t* and sets its current value via Dvar_SetVariant,
handling both STRING (type 7) and ENUM (type 6).
XLSX mislabels this as Dvar_RegisterString.
================
*/
static char s_assertDisable_Dvar_SetStringValue_dvar;
static char s_assertDisable_Dvar_SetStringValue_name;
static char s_assertDisable_Dvar_SetStringValue_type;
static char s_assertDisable_Dvar_SetStringValue_string;
static char s_assertDisable_Dvar_SetStringValue_enum;

void Dvar_SetStringValue(dvar_t *dvar, const char *string, int source)
{
    char buffer[1024];
    DvarValue_t newValue;

    Assert(dvar, s_assertDisable_Dvar_SetStringValue_dvar);
    Assert(dvar->name, s_assertDisable_Dvar_SetStringValue_name);
    Assert(dvar->type == DVAR_TYPE_STRING || dvar->type == DVAR_TYPE_ENUM,
           s_assertDisable_Dvar_SetStringValue_type);
    Assert(string, s_assertDisable_Dvar_SetStringValue_string);

    if (dvar->type == DVAR_TYPE_STRING)
    {
        I_strncpyz(buffer, string, 1024);
        newValue.string = buffer;
    }
    else /* DVAR_TYPE_ENUM */
    {
        newValue.integer = Dvar_StringToEnum(&dvar->domain, string);
        if (newValue.integer == -1337) /* DVAR_INVALID_ENUM_INDEX */
        {
            Assert(0, s_assertDisable_Dvar_SetStringValue_enum);
        }
    }

    Dvar_SetVariant(dvar, newValue, source);
}

/*
================
Dvar_SetStringByName

Sets a string dvar by name, creating it if it doesn't exist.
================
*/
void Dvar_SetStringByName(const char *name, const char *value)
{
    dvar_t *dvar;

    if (!name)
        Com_Error(1, "null name in generateHashValue");

    dvar = Dvar_FindVar(name);
    if (dvar)
        Dvar_SetStringValue(dvar, value, 0);
    else
        Dvar_Register_internal(name, value, DVAR_SYS_EXTERNAL);
}

/*
================
Dvar_Init

Initializes the dvar system, registers sv_cheats.
================
*/
void Dvar_Init(void)
{
    DvarValue_t val;
    DvarLimits_t dom;

    memset(&val, 0, sizeof(val));
    memset(&dom, 0, sizeof(dom));
    isDvarSystemActive = 1;
    g_svCheats = Dvar_RegisterVariant("sv_cheats", DVAR_TYPE_BOOL, 0x1018, val, &dom);
}

/*
================
Dvar_MakeExplicitType

Converts dvar to explicit string type. Interns name if not already,
converts latched and reset values to string representation,
frees old vector allocation if applicable.
================
*/
void Dvar_MakeExplicitType(dvar_t *dvar)
{
    void *oldVector;
    const char *str;
    char *copied;
    DvarValue_t resetVal;
    const char *resetStr;

    Assert(dvar, s_assertDisable_Dvar_MakeExplicitType);

    if (!(dvar->flags & DVAR_SYS_EXTERNAL))
    {
        dvar->flags |= DVAR_SYS_EXTERNAL;
        dvar->name = CopyStringInternal(dvar->name);
    }

    if (dvar->type != DVAR_TYPE_STRING)
    {
        if (dvar->type == DVAR_TYPE_VEC2 || dvar->type == DVAR_TYPE_VEC3 || dvar->type == DVAR_TYPE_VEC4)
            oldVector = (void *)dvar->current.vector;
        else
            oldVector = NULL;

        str = Dvar_ValueToString(dvar, dvar->latched);
        copied = Dvar_CopyString(str);
        resetVal = dvar->reset;
        dvar->current.string = copied;
        dvar->latched.string = copied;
        resetStr = Dvar_ValueToString(dvar, resetVal);
        Dvar_AssignResetStringValue(dvar, resetStr);
        dvar->type = DVAR_TYPE_STRING;

        if (oldVector)
            Z_FreeInternal(oldVector);
    }
}

/*
================
Dvar_DomainToString_Vector

Formats vector domain description into buffer.
================
*/
int Dvar_DomainToString_Vector(int components, float *domain, char *outBuffer, int outBufferLen)
{
    float min = domain[0];
    float max = domain[1];

    if (min == -3.4028235e38f)
    {
        if (max == 3.4028235e38f)
            return Com_sprintf(outBuffer, outBufferLen, "Domain is any %iD vector", components);
        else
            return Com_sprintf(outBuffer, outBufferLen, "Domain is any %iD vector with components %g or smaller", components, max);
    }
    else
    {
        if (max == 3.4028235e38f)
            return Com_sprintf(outBuffer, outBufferLen, "Domain is any %iD vector with components %g or bigger", components, min);
        else
            return Com_sprintf(outBuffer, outBufferLen, "Domain is any %iD vector with components from %g to %g", components, min, max);
    }
}

/*
================
Dvar_DomainToString

Formats domain description into buffer based on type.
================
*/
char *Dvar_DomainToString(unsigned char type, DvarLimits_t domain, char *outBuffer, int outBufferLen, int *outLineCount)
{
    char *end;
    int written;

    Assert(outBufferLen > 0, s_assertDisable_Dvar_DomainToString);

    end = outBuffer + outBufferLen;
    if (outLineCount)
        *outLineCount = 0;

    switch (type)
    {
    case 0:
        Com_sprintf(outBuffer, outBufferLen, "Domain is 0 or 1");
        break;
    case 1:
        if (domain.decimal.min == -3.4028235e38f)
        {
            if (domain.decimal.max == 3.4028235e38f)
                Com_sprintf(outBuffer, outBufferLen, "Domain is any number");
            else
                Com_sprintf(outBuffer, outBufferLen, "Domain is any number %g or smaller", domain.decimal.max);
        }
        else
        {
            if (domain.decimal.max == 3.4028235e38f)
                Com_sprintf(outBuffer, outBufferLen, "Domain is any number %g or bigger", domain.decimal.min);
            else
                Com_sprintf(outBuffer, outBufferLen, "Domain is any number from %g to %g", domain.decimal.min, domain.decimal.max);
        }
        break;
    case 2:
        Dvar_DomainToString_Vector(2, (float *)&domain, outBuffer, outBufferLen);
        break;
    case 3:
        Dvar_DomainToString_Vector(3, (float *)&domain, outBuffer, outBufferLen);
        break;
    case 4:
        Dvar_DomainToString_Vector(4, (float *)&domain, outBuffer, outBufferLen);
        break;
    case 5:
        if (domain.integer.min == (int)0x80000000)
        {
            if (domain.integer.max == 0x7FFFFFFF)
                Com_sprintf(outBuffer, outBufferLen, "Domain is any integer");
            else
                Com_sprintf(outBuffer, outBufferLen, "Domain is any integer %i or smaller", domain.integer.max);
        }
        else if (domain.integer.max == 0x7FFFFFFF)
        {
            Com_sprintf(outBuffer, outBufferLen, "Domain is any integer %i or bigger", domain.integer.min);
        }
        else
        {
            Com_sprintf(outBuffer, outBufferLen, "Domain is any integer from %i to %i", domain.integer.min, domain.integer.max);
        }
        break;
    case 6:
    {
        int i;
        written = Com_sprintf(outBuffer, outBufferLen, "Domain is one of the following:");
        if (written >= 0)
        {
            outBuffer += written;
            for (i = 0; i < domain.enumeration.stringCount; i++)
            {
                written = Com_sprintf(outBuffer, (int)(end - outBuffer), "\n  %2i: %s", i, domain.enumeration.strings[i]);
                if (written < 0)
                    break;
                if (outLineCount)
                    ++*outLineCount;
                outBuffer += written;
            }
        }
        break;
    }
    case 7:
        Com_sprintf(outBuffer, outBufferLen, "Domain is any text");
        break;
    case 8:
        Com_sprintf(outBuffer, outBufferLen, "Domain is any 4-component color, in RGBA format");
        break;
    default:
        if (!g_dvarSuppressTypeAssert)
            Assert(0, s_assertDisable_Dvar_DomainToString_default);
        *outBuffer = 0;
        break;
    }

    *(end - 1) = 0;
    return outBuffer;
}

/*
================
Dvar_SetResetValue

Sets the reset value of a dvar. For vector types, copies through
the existing allocated vector pointer. For strings, frees old
and assigns new. For scalars, stores directly.
================
*/
void Dvar_SetResetValue(dvar_t *dvar, DvarValue_t value)
{
    Assert(dvar, s_assertDisable_Dvar_SetResetValue);

    switch (dvar->type)
    {
    case DVAR_TYPE_VEC2:
        dvar->reset.vector[0] = value.vector[0];
        dvar->reset.vector[1] = value.vector[1];
        break;
    case DVAR_TYPE_VEC3:
        dvar->reset.vector[0] = value.vector[0];
        dvar->reset.vector[1] = value.vector[1];
        dvar->reset.vector[2] = value.vector[2];
        break;
    case DVAR_TYPE_VEC4:
        dvar->reset.vector[0] = value.vector[0];
        dvar->reset.vector[1] = value.vector[1];
        dvar->reset.vector[2] = value.vector[2];
        dvar->reset.vector[3] = value.vector[3];
        break;
    case DVAR_TYPE_STRING:
    {
        const char *old = dvar->reset.string;
        if (old != dvar->current.string && old != dvar->latched.string)
        {
            if (*old && (old[1] || *old < '0' || *old > '9') && old != g_offString && old != g_onString)
                Z_FreeInternal((void *)old);
        }
        dvar->reset.string = NULL;
        Dvar_AssignResetStringValue(dvar, value.string);
        break;
    }
    default:
        dvar->reset = value;
        break;
    }
}

/*
================
Dvar_SetLatchedVariant

Sets the latched value of a dvar. For vector types, copies through
the allocated pointer. For strings, frees old and assigns with
reuse of current or reset pointer if matching.
================
*/
void Dvar_SetLatchedVariant(dvar_t *dvar, DvarValue_t value)
{
    switch (dvar->type)
    {
    case DVAR_TYPE_VEC2:
        dvar->latched.vector[0] = value.vector[0];
        dvar->latched.vector[1] = value.vector[1];
        break;
    case DVAR_TYPE_VEC3:
        dvar->latched.vector[0] = value.vector[0];
        dvar->latched.vector[1] = value.vector[1];
        dvar->latched.vector[2] = value.vector[2];
        break;
    case DVAR_TYPE_VEC4:
        dvar->latched.vector[0] = value.vector[0];
        dvar->latched.vector[1] = value.vector[1];
        dvar->latched.vector[2] = value.vector[2];
        dvar->latched.vector[3] = value.vector[3];
        break;
    case DVAR_TYPE_STRING:
    {
        const char *old = dvar->latched.string;
        const char *current;
        const char *reset;

        if (old != dvar->current.string && old != dvar->reset.string)
        {
            if (*old && (old[1] || *old < '0' || *old > '9') && old != g_offString && old != g_onString)
                Z_FreeInternal((void *)old);
        }
        current = dvar->current.string;
        dvar->latched.string = NULL;
        if (current && (value.string == current || !strcmp(value.string, current)))
        {
            dvar->latched.string = current;
        }
        else
        {
            reset = dvar->reset.string;
            if (reset && (value.string == reset || !strcmp(value.string, reset)))
                dvar->latched.string = reset;
            else
                dvar->latched.string = Dvar_CopyString(value.string);
        }
        break;
    }
    default:
        dvar->latched = value;
        break;
    }
}

/*
================
Dvar_SetCurrentVariant

Sets current and latched values of a dvar. For vector types, copies
through allocated pointers. For strings, frees old current and assigns.
================
*/
void Dvar_SetCurrentVariant(dvar_t *dvar, DvarValue_t value)
{
    switch (dvar->type)
    {
    case DVAR_TYPE_VEC2:
        dvar->current.vector[0] = value.vector[0];
        dvar->current.vector[1] = value.vector[1];
        dvar->latched.vector[0] = value.vector[0];
        dvar->latched.vector[1] = value.vector[1];
        break;
    case DVAR_TYPE_VEC3:
        dvar->current.vector[0] = value.vector[0];
        dvar->current.vector[1] = value.vector[1];
        dvar->current.vector[2] = value.vector[2];
        dvar->latched.vector[0] = value.vector[0];
        dvar->latched.vector[1] = value.vector[1];
        dvar->latched.vector[2] = value.vector[2];
        break;
    case DVAR_TYPE_VEC4:
        dvar->current.vector[0] = value.vector[0];
        dvar->current.vector[1] = value.vector[1];
        dvar->current.vector[2] = value.vector[2];
        dvar->current.vector[3] = value.vector[3];
        dvar->latched.vector[0] = value.vector[0];
        dvar->latched.vector[1] = value.vector[1];
        dvar->latched.vector[2] = value.vector[2];
        dvar->latched.vector[3] = value.vector[3];
        break;
    case DVAR_TYPE_STRING:
    {
        const char *old = dvar->current.string;
        if (value.string != old)
        {
            if (old != dvar->latched.string && old != dvar->reset.string)
            {
                if (*old && (old[1] || *old < '0' || *old > '9') && old != g_offString && old != g_onString)
                    Z_FreeInternal((void *)old);
            }
            dvar->current.string = NULL;
            Dvar_AssignCurrentStringValue(dvar, value.string);
        }
        dvar->latched.string = value.string;
        break;
    }
    default:
        dvar->current = value;
        dvar->latched = value;
        break;
    }
}

/*
================
Dvar_SetVariant

Main dvar value setter. Validates domain, checks access flags,
handles latching for restart-required dvars.
================
*/
void Dvar_SetVariant(dvar_t *dvar, DvarValue_t value, int source)
{
    DvarLimits_t domain;
    char outBuffer[1024];

    Assert(dvar, s_assertDisable_Dvar_SetVariant);
    Assert(dvar->name, s_assertDisable_Dvar_SetVariant_name);

    while (1)
    {
        domain = dvar->domain;
        if (Dvar_ValueInDomain(dvar->type, value, domain))
            break;

        Com_Printf("'%s' is not a valid value for dvar '%s'\n",
                 Dvar_ValueToString(dvar, value), dvar->name);
        domain = dvar->domain;
        Com_Printf("  %s\n", Dvar_DomainToString(dvar->type, domain, outBuffer, 1024, NULL));

        if (dvar->type != 6)
            return;

        domain = dvar->domain;
        Assert(Dvar_ValueInDomain(6, dvar->reset, domain), s_assertDisable_Dvar_SetVariant_reset);
        value = dvar->reset;
    }

    if (source == 1 || source == 2)
    {
        if (dvar->flags & 0x40)
        {
            Com_Printf("%s is read only.\n", dvar->name);
            return;
        }
        if (dvar->flags & 0x10)
        {
            Com_Printf("%s is write protected.\n", dvar->name);
            return;
        }
        if (source == 1 && (dvar->flags & 0x80) && !g_svCheats->current.enabled)
        {
            Com_Printf("%s is cheat protected.\n", dvar->name);
            return;
        }
        if (dvar->flags & 0x20)
        {
            Dvar_SetLatchedVariant(dvar, value);
            if (!Dvar_ValuesEqual(dvar->type, dvar->latched, dvar->current))
                Com_Printf("%s will be changed upon restarting.\n", dvar->name);
            return;
        }
    }

    if (Dvar_ValuesEqual(dvar->type, dvar->current, value))
    {
        Dvar_SetLatchedVariant(dvar, dvar->current);
        return;
    }

    g_dvarModifiedFlags |= dvar->flags;

    switch (dvar->type)
    {
    case 2:
        dvar->current.vector[0] = value.vector[0];
        dvar->current.vector[1] = value.vector[1];
        dvar->latched.vector[0] = value.vector[0];
        dvar->latched.vector[1] = value.vector[1];
        break;
    case 3:
        dvar->current.vector[0] = value.vector[0];
        dvar->current.vector[1] = value.vector[1];
        dvar->current.vector[2] = value.vector[2];
        dvar->latched.vector[0] = value.vector[0];
        dvar->latched.vector[1] = value.vector[1];
        dvar->latched.vector[2] = value.vector[2];
        break;
    case 4:
        dvar->current.vector[0] = value.vector[0];
        dvar->current.vector[1] = value.vector[1];
        dvar->current.vector[2] = value.vector[2];
        dvar->current.vector[3] = value.vector[3];
        dvar->latched.vector[0] = value.vector[0];
        dvar->latched.vector[1] = value.vector[1];
        dvar->latched.vector[2] = value.vector[2];
        dvar->latched.vector[3] = value.vector[3];
        break;
    case 7:
        Assert(dvar->name, s_assertDisable_Dvar_SetVariant_string);
        Assert(value.string != dvar->current.string
            || value.string == dvar->latched.string
            || value.string == dvar->reset.string,
               s_assertDisable_Dvar_SetVariant_stringval);
        Dvar_FreeCurrentStringValue(dvar);
        Dvar_AssignCurrentStringValue(dvar, value.string);
        Dvar_FreeLatchedStringValue(dvar);
        dvar->latched.string = dvar->current.string;
        break;
    default:
        dvar->current = value;
        dvar->latched = value;
        break;
    }
    dvar->modified = 1;
}

/*
================
Dvar_ReRegister

Re-registers an existing dvar with a new type. Converts from
string type back to the requested type, reallocating vector
storage if needed.
================
*/
void Dvar_ReRegister(dvar_t *dvar, const char *name, unsigned char type, unsigned short flags, DvarValue_t resetValue, DvarLimits_t *domain)
{
    DvarValue_t newValue;
    DvarLimits_t dom;

    Assert(dvar->type == 7, s_assertDisable_Dvar_ReRegister);

    dvar->type = type;
    dvar->domain = *domain;

    if ((flags & 0x40) || ((flags & 0x80) && g_svCheats && !g_svCheats->current.enabled))
    {
        newValue = resetValue;
    }
    else
    {
        dom = *domain;
        newValue = Dvar_StringToValue(type, &dom, dvar->current.string);
        dom = *domain;
        newValue = Dvar_ClampValueToDomain(type, newValue, resetValue, dom);
    }

    if (dvar->type != 7)
    {
        Dvar_FreeCurrentStringValue(dvar);
    }
    Dvar_FreeLatchedStringValue(dvar);

    {
        const char *old = dvar->reset.string;
        if (old != dvar->current.string)
        {
            if (old)
            {
                if (*old && (old[1] || *old < '0' || *old > '9') && old != g_offString && old != g_onString)
                    Z_FreeInternal((void *)old);
            }
        }
    }

    dvar->reset.string = NULL;
    if (dvar->type == 2 || dvar->type == 3 || dvar->type == 4)
    {
        float *buf = (float *)Z_Malloc(12 * dvar->type);
        dvar->current.vector = buf;
        dvar->latched.vector = buf + dvar->type;
        dvar->reset.vector = buf + dvar->type * 2;
    }

    Dvar_SetResetValue(dvar, resetValue);
    Dvar_SetCurrentVariant(dvar, newValue);
    g_dvarModifiedFlags |= flags;
}

/*
================
Dvar_UpdateReRegister

Handles re-registration when system flags change. Converts to
explicit string type first, then re-registers with new params.
================
*/
void Dvar_UpdateReRegister(dvar_t *dvar, const char *name, unsigned char type, unsigned short flags, DvarValue_t resetValue, DvarLimits_t *domain)
{
    DvarLimits_t dom;

    if ((dvar->flags & 0x4000) && !(flags & 0x4000))
    {
        Assert((dvar->flags & 0x7000) == 0x4000, s_assertDisable_Dvar_UpdateReRegister);

        if ((short)dvar->flags < 0 && (flags & 0x400) && !(dvar->flags & 0xD0))
        {
            dom = *domain;
            resetValue = Dvar_StringToValue(type, &dom, dvar->reset.string);
        }

        Dvar_MakeExplicitType(dvar);
        Z_FreeInternal((void *)dvar->name);
        dvar->flags &= ~0x4000u;
        dvar->name = name;
        Dvar_ReRegister(dvar, name, type, flags, resetValue, domain);
    }
}

/*
================
Dvar_Reregister

Processes re-registration of an existing dvar. Checks type
compatibility, handles external flag conversion, verifies reset values.
================
*/
void Dvar_Reregister(dvar_t *dvar, const char *name, unsigned char type, unsigned short flags, DvarValue_t resetValue, DvarLimits_t *domain)
{
    Assert(dvar, s_assertDisable_Dvar_Reregister);
    Assert(name, s_assertDisable_Dvar_Reregister_name);
    Assert(dvar->type == type || (dvar->flags & 0x4000), s_assertDisable_Dvar_Reregister_type);

    if (((dvar->flags ^ flags) & 0x7000) != 0)
    {
        Dvar_UpdateReRegister(dvar, name, type, flags, resetValue, domain);

        if (!(flags & 0x4000) && (flags & 0x1000) && !(dvar->flags & 0x1000))
        {
            dvar->name = name;
            if (dvar->type == 6)
                dvar->domain = *domain;
        }
    }

    if (dvar->flags & 0x4000)
    {
        if (dvar->type != type)
        {
            Assert(dvar->type == 7, s_assertDisable_Dvar_Reregister_string);
            Dvar_ReRegister(dvar, name, type, flags, resetValue, domain);
        }
    }

    Assert(dvar->type == type, s_assertDisable_Dvar_Reregister_final);
    Assert((dvar->flags & 0x8600) || Dvar_ValuesEqual(type, dvar->reset, resetValue), s_assertDisable_Dvar_Reregister_reset);

    dvar->flags |= flags;

    if ((dvar->flags & 0x80) && g_svCheats && !g_svCheats->current.enabled)
    {
        Dvar_SetVariant(dvar, dvar->reset, 0);
        Dvar_SetLatchedVariant(dvar, dvar->reset);
    }

    if (dvar->flags & 0x20)
        Dvar_SetVariant(dvar, dvar->latched, 0);
}

/*
================
Dvar_RegisterNew

Allocates a new dvar in the pool, initializes all fields,
inserts into hash table and sorted linked list.
================
*/
dvar_t *Dvar_RegisterNew(const char *name, unsigned char type, unsigned short flags, DvarValue_t value, DvarLimits_t *domain)
{
    int index;
    dvar_t *dvar;
    int hashIdx;

    if (g_dvarCount >= DVAR_MAX)
        Com_Error(0, "Can't create dvar '%s': %i dvars already exist", name, DVAR_MAX);

    index = g_dvarCount++;
    dvar = &g_dvarPool[index];

    dvar->type = type;

    if (flags & 0x4000)
        dvar->name = CopyStringInternal(name);
    else
        dvar->name = name;

    switch (type)
    {
    case 2:
    {
        float *buf = (float *)Z_Malloc(12 * type);
        dvar->current.vector = buf;
        dvar->latched.vector = buf + type;
        dvar->reset.vector = buf + type * 2;
        dvar->current.vector[0] = value.vector[0];
        dvar->current.vector[1] = value.vector[1];
        dvar->latched.vector[0] = value.vector[0];
        dvar->latched.vector[1] = value.vector[1];
        dvar->reset.vector[0] = value.vector[0];
        dvar->reset.vector[1] = value.vector[1];
        break;
    }
    case 3:
    {
        float *buf = (float *)Z_Malloc(12 * type);
        dvar->current.vector = buf;
        dvar->latched.vector = buf + type;
        dvar->reset.vector = buf + type * 2;
        dvar->current.vector[0] = value.vector[0];
        dvar->current.vector[1] = value.vector[1];
        dvar->current.vector[2] = value.vector[2];
        dvar->latched.vector[0] = value.vector[0];
        dvar->latched.vector[1] = value.vector[1];
        dvar->latched.vector[2] = value.vector[2];
        dvar->reset.vector[0] = value.vector[0];
        dvar->reset.vector[1] = value.vector[1];
        dvar->reset.vector[2] = value.vector[2];
        break;
    }
    case 4:
    {
        float *buf = (float *)Z_Malloc(12 * type);
        dvar->current.vector = buf;
        dvar->latched.vector = buf + type;
        dvar->reset.vector = buf + type * 2;
        dvar->current.vector[0] = value.vector[0];
        dvar->current.vector[1] = value.vector[1];
        dvar->current.vector[2] = value.vector[2];
        dvar->current.vector[3] = value.vector[3];
        dvar->latched.vector[0] = value.vector[0];
        dvar->latched.vector[1] = value.vector[1];
        dvar->latched.vector[2] = value.vector[2];
        dvar->latched.vector[3] = value.vector[3];
        dvar->reset.vector[0] = value.vector[0];
        dvar->reset.vector[1] = value.vector[1];
        dvar->reset.vector[2] = value.vector[2];
        dvar->reset.vector[3] = value.vector[3];
        break;
    }
    case 7:
    {
        char *s = Dvar_CopyString(value.string);
        dvar->current.string = s;
        dvar->latched.string = s;
        dvar->reset.string = s;
        break;
    }
    default:
        dvar->current = value;
        dvar->latched = value;
        dvar->reset = value;
        break;
    }

    dvar->modified = 0;
    dvar->domain = *domain;

    /* insert into sorted linked list */
    {
        dvar_t **link = &g_dvarLinkedList;
        while (*link)
        {
            if (I_stricmp(dvar->name, (*link)->name) < 0)
                break;
            link = &(*link)->next;
        }
        dvar->next = *link;
        *link = dvar;
    }

    dvar->flags = flags;

    /* insert into hash table */
    if (!name)
        Com_Error(1, "null name in generateHashValue");
    hashIdx = Dvar_GenerateHashValue(name);
    dvar->hashNext = g_dvarHashTable[hashIdx];
    g_dvarHashTable[hashIdx] = dvar;

    return dvar;
}

/*
================
Dvar_RegisterVariant

Main dvar registration entry point. Looks up existing dvar by name
in hash table — if found, re-registers it; if not, allocates new.
================
*/
dvar_t *Dvar_RegisterVariant(const char *name, unsigned char type, unsigned short flags, DvarValue_t value, DvarLimits_t *domain)
{
    int hashIdx;
    dvar_t *dvar;

    Assert((flags & 0x7000) != 0, s_assertDisable_Dvar_RegisterVariant_flags);
    Assert((flags & 0x4000) || CanKeepStringPointer(name), s_assertDisable_Dvar_RegisterVariant_keep);

    if (!name)
        Com_Error(1, "null name in generateHashValue");

    hashIdx = Dvar_GenerateHashValue(name);
    dvar = g_dvarHashTable[hashIdx];
    while (dvar)
    {
        if (!I_stricmp(name, dvar->name))
        {
            Dvar_Reregister(dvar, name, type, flags, value, domain);
            return dvar;
        }
        dvar = dvar->hashNext;
    }

    return Dvar_RegisterNew(name, type, flags, value, domain);
}

/*
================
Dvar_Register_internal

Registers a string dvar with name, value, and flags.
Validates parameters, checks CanKeepStringValue if not external,
then calls the common registration path with type=7 (string).
================
*/
dvar_t *Dvar_Register_internal(const char *dvarName, const char *value, unsigned short flags)
{
    DvarLimits_t domain;
    DvarValue_t val;

    Assert(dvarName, s_assertDisable_Dvar_Register_internal_name);
    Assert(value, s_assertDisable_Dvar_Register_internal_value);

    if (!(flags & 0x4000)) /* DVAR_SYS_EXTERNAL */
    {
        Assert(CanKeepStringPointer(value),
               s_assertDisable_Dvar_Register_internal_keep);
    }

    memset(&val, 0, sizeof(val));
    memset(&domain, 0, sizeof(domain));
    val.string = value;

    return Dvar_RegisterVariant(dvarName, 7, flags, val, &domain);
}
