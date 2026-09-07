/*
dvar.c — Dynamic variable system

Reconstructed from cod2map.exe by Rose.
*/

#include "cod4map.h"

Dvar_t   *dvar_cheats;
Dvar_t    dvarPool[MAX_DVARS];
intptr_t  dvarHashTable[DVAR_HASH_TABLE_SIZE];

char      digitStrings[10][2] = {
         "0", "1", "2", "3", "4", "5", "6", "7", "8", "9"
};
int       dvarCount;
int       dvarParseBuffer[16];
int       dvar_modifiedFlags;
char      g_dvarString0[4] = "0";
char      g_dvarString1[4] = "1";
int       g_dvarStringBufIdx;
int       g_dvarStringBufs[10];
char     *g_dvarStringOff = NULL;
char     *g_dvarStringOn = "on";
char      g_emptyString[4] = {0};
char      isDvarSystemActive;
intptr_t  sortedDvars;

char s_assertDisable_Dvar_AddFlags;
char s_assertDisable_Dvar_AddFlags;
char s_assertDisable_Dvar_AssignResetStringValue;
char s_assertDisable_Dvar_ClampValueToDomain;
char s_assertDisable_Dvar_ClampValueToDomain;
char s_assertDisable_Dvar_ClampValueToDomain;
char s_assertDisable_Dvar_ClampValueToDomain;
char s_assertDisable_Dvar_ClampValueToDomain;
char s_assertDisable_Dvar_ClearModified;
char s_assertDisable_Dvar_ClearModifiedFlags;
char s_assertDisable_Dvar_ClearModifiedFlags;
char s_assertDisable_Dvar_ClearModifiedFlags;
char s_assertDisable_Dvar_ClearModifiedFlags;
char s_assertDisable_Dvar_CopyString;
char s_assertDisable_Dvar_DisplayableLatchedValue;
char s_assertDisable_Dvar_DisplayableResetValue;
char s_assertDisable_Dvar_DomainToString_Internal;
char s_assertDisable_Dvar_DomainToString_Internal;
char s_assertDisable_Dvar_FindMalleableVar;
char s_assertDisable_Dvar_FindMalleableVar;
char s_assertDisable_Dvar_FreeResetValue;
char s_assertDisable_Dvar_FreeResetValue;
char s_assertDisable_Dvar_FreeResetValue;
char s_assertDisable_Dvar_FreeResetValue;
char s_assertDisable_Dvar_FreeResetValue;
char s_assertDisable_Dvar_GetString;
char s_assertDisable_Dvar_GetUnpackedColor;
char s_assertDisable_Dvar_GetUnpackedColor;
char s_assertDisable_Dvar_PerformUnregistration;
char s_assertDisable_Dvar_RegisterString;
char s_assertDisable_Dvar_RegisterString;
char s_assertDisable_Dvar_RegisterString;
char s_assertDisable_Dvar_RegisterVariant;
char s_assertDisable_Dvar_RegisterVariant;
char s_assertDisable_Dvar_RegisterVariant;
char s_assertDisable_Dvar_RegisterVariant;
char s_assertDisable_Dvar_RegisterVariant;
char s_assertDisable_Dvar_RegisterVariant;
char s_assertDisable_Dvar_SetBool;
char s_assertDisable_Dvar_SetBool;
char s_assertDisable_Dvar_SetBool;
char s_assertDisable_Dvar_SetColor;
char s_assertDisable_Dvar_SetColor;
char s_assertDisable_Dvar_SetColor;
char s_assertDisable_Dvar_SetFloat;
char s_assertDisable_Dvar_SetFloat;
char s_assertDisable_Dvar_SetFloat;
char s_assertDisable_Dvar_SetInt;
char s_assertDisable_Dvar_SetInt;
char s_assertDisable_Dvar_SetInt;
char s_assertDisable_Dvar_SetResetValue;
char s_assertDisable_Dvar_SetString;
char s_assertDisable_Dvar_SetString;
char s_assertDisable_Dvar_SetString;
char s_assertDisable_Dvar_SetString;
char s_assertDisable_Dvar_SetString;
char s_assertDisable_Dvar_SetVariant;
char s_assertDisable_Dvar_SetVariant;
char s_assertDisable_Dvar_SetVariant;
char s_assertDisable_Dvar_SetVariant;
char s_assertDisable_Dvar_SetVariant;
char s_assertDisable_Dvar_ReRegisterVariant;
char s_assertDisable_Dvar_ReRegisterVariant;
char s_assertDisable_Dvar_SetVec2;
char s_assertDisable_Dvar_SetVec2;
char s_assertDisable_Dvar_SetVec2;
char s_assertDisable_Dvar_SetVec3;
char s_assertDisable_Dvar_SetVec3;
char s_assertDisable_Dvar_SetVec3;
char s_assertDisable_Dvar_SetVec4;
char s_assertDisable_Dvar_SetVec4;
char s_assertDisable_Dvar_SetVec4;
char s_assertDisable_Dvar_StringToBool;
char s_assertDisable_Dvar_StringToEnum;
char s_assertDisable_Dvar_StringToEnum;
char s_assertDisable_Dvar_StringToFloat;
char s_assertDisable_Dvar_StringToInt;
char s_assertDisable_Dvar_StringToValue;
char s_assertDisable_Dvar_StringToValue;
char s_assertDisable_Dvar_StringToValue;
char s_assertDisable_Dvar_StringToValue;
char s_assertDisable_Dvar_StringToVec2;
char s_assertDisable_Dvar_StringToVec3;
char s_assertDisable_Dvar_StringToVec4;
char s_assertDisable_Dvar_ValueInDomain;
char s_assertDisable_Dvar_ValueToString;
char s_assertDisable_Dvar_ValueToString;
char s_assertDisable_Dvar_ValueToString;
char s_assertDisable_Dvar_ValuesEqual;


/*
================
Dvar_IsSystemActive

Returns whether the dvar system is active.
================
*/
bool Dvar_IsSystemActive()
{
  return isDvarSystemActive;
}

/*
================
Dvar_GenerateHashValue

Generates a hash value for dvar name lookup in the hash table.
================
*/
int Dvar_GenerateHashValue(const char *name)
{
  int i;
  unsigned char hash;

  if ( !name )
    Com_ErrorLevel(1, "null name in generateHashValue");

  hash = 0;
  for ( i = 0; name[i]; i++ )
    hash += (i + HASH_POSITION_SEED) * tolower(name[i]);
  return hash;
}

/*
================
Dvar_MakeExplicitType

Native 0x479780 changes an external string dvar into its explicit type.  It
uses the inline value carrier throughout, preserving string ownership before
current/latched/reset storage is repurposed.
================
*/
void Dvar_MakeExplicitType(
    Dvar_t *dvar,
    const char *dvarName,
    unsigned char type,
    unsigned short flags,
    DvarValue_t resetValue,
    DvarLimits_t domain)
{
  DvarValue_t value = {};
  DvarValue_t parsedValue = {};
  DvarValue_t copiedString = {};
  bool shouldCopyString;

  (void)dvarName;
  Assert(dvar->type == DVAR_TYPE_STRING, s_assertDisable_Dvar_RegisterVariant);
  dvar->type = type;
  dvar->domain = domain;

  if ((flags & DVAR_ROM) != 0 || ((flags & DVAR_CHEAT) != 0 && dvar_cheats && !dvar_cheats->current.enabled))
  {
    value = resetValue;
  }
  else
  {
    Dvar_StringToValue(&parsedValue, type, domain, dvar->current.string);
    Dvar_ClampValueToDomain(&value, type, parsedValue, resetValue, domain);
  }

  shouldCopyString = type == DVAR_TYPE_STRING && value.string != 0;
  if (shouldCopyString)
  {
    copiedString.string = Dvar_CopyString(value.string);
    value = copiedString;
  }

  if (type != DVAR_TYPE_STRING && Dvar_ShouldFreeCurrentString(dvar))
    Dvar_FreeString(&dvar->current);
  dvar->current.string = NULL;
  if (Dvar_ShouldFreeLatchedString(dvar))
    Dvar_FreeString(&dvar->latched);
  dvar->latched.string = NULL;
  if (Dvar_ShouldFreeResetString(dvar))
    Dvar_FreeString(&dvar->reset);
  dvar->reset.string = NULL;

  Dvar_UpdateResetValue(dvar, resetValue);
  Dvar_UpdateValue(dvar, value);
  dvar_modifiedFlags |= flags;

  if (shouldCopyString)
    Dvar_FreeString(&copiedString);
}

/*
================
Dvar_FreeCurrentValue

Frees the current value string of a dvar, if it's uniquely owned.
================
*/
void Dvar_FreeCurrentValue(Dvar_t *dvar)
{
  char *str;
  char firstCh;

  str = (char *)dvar->current.string;
  if ( str == (char *)dvar->latched.string || str == (char *)dvar->reset.string )
  {
    dvar->current.string = NULL;
  }
  else
  {
    firstCh = *str;
    if ( *str && (str[1] || firstCh < '0' || firstCh > '9') && str != g_dvarStringOff && str != g_dvarStringOn )
      Z_Free(str);
    dvar->current.string = NULL;
  }
}

/*
================
Dvar_FreeLatchedValue

Frees the latched value string of a dvar, if it's uniquely owned.
================
*/
void Dvar_FreeLatchedValue(Dvar_t *dvar)
{
  char *str;
  char firstCh;

  str = (char *)dvar->latched.string;
  if ( str == (char *)dvar->current.string || str == (char *)dvar->reset.string )
  {
    dvar->latched.string = NULL;
  }
  else
  {
    firstCh = *str;
    if ( *str && (str[1] || firstCh < '0' || firstCh > '9') && str != g_dvarStringOff && str != g_dvarStringOn )
      Z_Free(str);
    dvar->latched.string = NULL;
  }
}

/*
================
Dvar_FreeResetValue

Frees the reset value string of a dvar, if it's uniquely owned.
================
*/
void Dvar_FreeResetValue(Dvar_t *dvar)
{
  char *str;
  char firstCh;

  str = (char *)dvar->reset.string;
  if ( str == (char *)dvar->current.string || str == (char *)dvar->latched.string )
  {
    dvar->reset.string = NULL;
  }
  else
  {
    firstCh = *str;
    if ( *str && (str[1] || firstCh < '0' || firstCh > '9') && str != g_dvarStringOff && str != g_dvarStringOn )
      Z_Free(str);
    dvar->reset.string = NULL;
  }
}

/*
================
Dvar_EnumToString

Returns the string representation of the current enum value.
================
*/
const char *Dvar_EnumToString( Dvar_t *dvar )
{
  int idx;

  Assert( dvar, s_assertDisable_Dvar_FreeResetValue );
  Assert( dvar->name, s_assertDisable_Dvar_FreeResetValue );

  Assert(!(dvar->type != DVAR_TYPE_ENUM), s_assertDisable_Dvar_FreeResetValue);
  Assert(!(!dvar->domain.enumeration.strings), s_assertDisable_Dvar_FreeResetValue);

  idx = (int)dvar->current.integer;
  Assert(!((idx < 0 || (idx >= dvar->domain.enumeration.stringCount && idx))), s_assertDisable_Dvar_FreeResetValue);

  if ( dvar->domain.enumeration.stringCount )
    return dvar->domain.enumeration.strings[dvar->current.integer];
  return g_fsBaseGame;
}

/*
================
Dvar_ValueToString

Converts a dvar value to its string representation based on type.
================
*/
char *Dvar_ValueToString( Dvar_t *dvar, DvarValue_t value )
{
  switch ( dvar->type )
  {
    case DVAR_TYPE_BOOL:
      return value.enabled ? g_dvarString1 : g_dvarString0;
    case DVAR_TYPE_FLOAT:
      return va( "%g", value.value );
    case DVAR_TYPE_VEC2:
      return va( "%g %g", value.vector[0], value.vector[1] );
    case DVAR_TYPE_VEC3:
      return va( FMT_VECTOR3, value.vector[0], value.vector[1], value.vector[2] );
    case DVAR_TYPE_VEC4:
      return va( "%g %g %g %g", value.vector[0], value.vector[1], value.vector[2], value.vector[3] );
    case DVAR_TYPE_INT:
      return va( "%i", value.integer );

    case DVAR_TYPE_ENUM:
      Assert(!((value.integer < 0 || (value.integer >= dvar->domain.enumeration.stringCount && value.integer != 0))), s_assertDisable_Dvar_ValueToString);
      if ( !dvar->domain.enumeration.stringCount )
        return g_fsBaseGame;
      return (char *)dvar->domain.enumeration.strings[value.integer];

    case DVAR_TYPE_STRING:
      Assert(value.string, s_assertDisable_Dvar_ValueToString);
      return va( "%s", value.string );

    case DVAR_TYPE_COLOR:
      return va( "%g %g %g %g",
        value.color[0] * (1.0 / 255.0),
        value.color[1] * (1.0 / 255.0),
        value.color[2] * (1.0 / 255.0),
        value.color[3] * (1.0 / 255.0) );

    default:
      Assert(0, s_assertDisable_Dvar_ValueToString);
      return g_fsBaseGame;
  }
}

/*
================
Dvar_StringToBool

Converts a string to a boolean value.
================
*/
int Dvar_StringToBool( const char *string )
{
  Assert(string, s_assertDisable_Dvar_StringToBool);
  return atol( string ) != 0;
}

/*
================
Dvar_StringToInt

Converts a string to an integer value.
================
*/
int Dvar_StringToInt( const char *string )
{
  Assert(string, s_assertDisable_Dvar_StringToInt);
  return atol( string );
}

/*
================
Dvar_StringToFloat

Converts a string to a float value.
================
*/
double Dvar_StringToFloat( const char *string )
{
  Assert(string, s_assertDisable_Dvar_StringToFloat);
  return atof( string );
}

/*
================
Dvar_DisplayableResetValue

Returns the displayable string for a dvar's reset value.
================
*/
char *Dvar_DisplayableResetValue( Dvar_t *dvar )
{
  Assert(dvar, s_assertDisable_Dvar_DisplayableResetValue);
  return Dvar_ValueToString( dvar, dvar->reset );
}

/*
================
Dvar_DisplayableLatchedValue

Returns the displayable string for a dvar's latched value.
================
*/
char *Dvar_DisplayableLatchedValue( Dvar_t *dvar )
{
  Assert(dvar, s_assertDisable_Dvar_DisplayableLatchedValue);
  return Dvar_ValueToString( dvar, dvar->latched );
}

/*
================
Dvar_ClampValueToDomain

Clamps a dvar value to its valid domain range based on type.
================
*/
DvarValue_t *Dvar_ClampValueToDomain(
    DvarValue_t *result,
    unsigned char type,
    DvarValue_t value,
    DvarValue_t resetValue,
    DvarLimits_t domain)
{
  switch ( type )
  {
    case DVAR_TYPE_BOOL:
      value.enabled = value.color[0] != 0;
      break;
    case DVAR_TYPE_FLOAT:
      if (value.value < domain.value.min)
        value.value = domain.value.min;
      else if (value.value > domain.value.max)
        value.value = domain.value.max;
      break;
    case DVAR_TYPE_VEC2:
    case DVAR_TYPE_VEC3:
    case DVAR_TYPE_VEC4:
      Dvar_ClampVectorToDomain(value.vector, type, domain.value.min, domain.value.max);
      break;
    case DVAR_TYPE_INT:
      Assert(domain.integer.min <= domain.integer.max, s_assertDisable_Dvar_ClampValueToDomain);
      if (value.integer < domain.integer.min)
        value.integer = domain.integer.min;
      else if (value.integer > domain.integer.max)
        value.integer = domain.integer.max;
      break;
    case DVAR_TYPE_ENUM:
      if (value.integer < 0 || value.integer >= domain.enumeration.stringCount)
      {
        value.integer = resetValue.integer;
        Assert(
            value.integer >= 0 && (value.integer < domain.enumeration.stringCount || value.integer == 0),
            s_assertDisable_Dvar_ClampValueToDomain);
      }
      break;
    case DVAR_TYPE_STRING:
    case DVAR_TYPE_COLOR:
      break;
    default:
      Assert(0, s_assertDisable_Dvar_ClampValueToDomain);
      break;
  }
  *result = value;
  return result;
}

/*
================
Dvar_ClampVectorToDomain

Clamps each vector component to the inclusive dvar domain range.
================
*/
void Dvar_ClampVectorToDomain(float *vector, int components, float min, float max)
{
  int channel;

  for ( channel = 0; channel < components; ++channel )
  {
    if ( vector[channel] < min )
      vector[channel] = min;
    else if ( vector[channel] > max )
      vector[channel] = max;
  }
}

/*
================
Dvar_ValueInDomain

Returns true if the value is within the dvar's valid domain.
================
*/
bool Dvar_ValueInDomain(unsigned char type, DvarValue_t value, DvarLimits_t domain)
{
  switch ( type )
  {
    case DVAR_TYPE_BOOL:
      Assert(value.color[0] < 2, s_assertDisable_Dvar_ValueInDomain);
      return 1;
    case DVAR_TYPE_FLOAT:
      return value.value >= domain.value.min && value.value <= domain.value.max;
    case DVAR_TYPE_VEC2:
    case DVAR_TYPE_VEC3:
    case DVAR_TYPE_VEC4:
      return Dvar_VectorInDomain(value.vector, type, domain.value.min, domain.value.max);
    case DVAR_TYPE_INT:
      Assert(domain.integer.min <= domain.integer.max, s_assertDisable_Dvar_ClampValueToDomain);
      return value.integer >= domain.integer.min && value.integer <= domain.integer.max;
    case DVAR_TYPE_ENUM:
      return value.integer >= 0 && (value.integer < domain.enumeration.stringCount || value.integer == 0);
    case DVAR_TYPE_STRING:
    case DVAR_TYPE_COLOR:
      return 1;
    default:
      Assert(0, s_assertDisable_Dvar_ClampValueToDomain);
      return 0;
  }
}

/*
================
Dvar_VectorInDomain

Validates every component of a vector against a dvar domain.  The native
helper is shared by the vec2, vec3, and vec4 cases above.
================
*/
bool Dvar_VectorInDomain(const float *vector, int components, float min, float max)
{
  int channel;

  for ( channel = 0; channel < components; ++channel )
  {
    if ( vector[channel] < min || vector[channel] > max )
      return false;
  }
  return true;
}

/*
================
Dvar_ValuesEqual_Vec2

Returns true if two vec2 values are equal.
================
*/
int Dvar_ValuesEqual_Vec2(float *value0, float *value1)
{
  return *value0 == *value1 && value0[1] == value1[1];
}

/*
================
Dvar_VectorDomainToString

Formats a vector domain description string.
================
*/
void Dvar_VectorDomainToString(int components, DvarLimits_t domain, char *buf, size_t bufLen)
{
  if ( domain.value.min == -FLT_MAX )
  {
    if ( domain.value.max == FLT_MAX )
      _snprintf(buf, bufLen, "Domain is any %iD vector", components);
    else
      _snprintf(buf, bufLen, "Domain is any %iD vector with components %g or smaller", components, domain.value.max);
  }
  else if ( domain.value.max == FLT_MAX )
  {
    _snprintf(buf, bufLen, "Domain is any %iD vector with components %g or bigger", components, domain.value.min);
  }
  else
  {
    _snprintf(buf, bufLen, "Domain is any %iD vector with components from %g to %g", components, domain.value.min, domain.value.max);
  }
}

/*
================
Dvar_DomainToString_Internal

Converts a dvar domain to a human-readable description string.
================
*/
char *Dvar_DomainToString_Internal(unsigned char type, DvarLimits_t domain, char *buf, int bufLen, int *enumLinesCount)
{
  char *result;
  int written, enumIdx, lineLen;
  char *bufEnd;

  Assert(bufLen > 0, s_assertDisable_Dvar_DomainToString_Internal);
  bufEnd = &buf[bufLen];
  if ( enumLinesCount )
    *enumLinesCount = 0;
  switch ( type )
  {
    case DVAR_TYPE_BOOL:
      _snprintf(buf, bufLen, "Domain is 0 or 1");
      result = buf;
      *(bufEnd - 1) = 0;
      return result;
    case DVAR_TYPE_FLOAT:
      if ( domain.value.min == -FLT_MAX )
      {
        if ( domain.value.max == FLT_MAX )
          _snprintf(buf, bufLen, "Domain is any number");
        else
          _snprintf(buf, bufLen, "Domain is any number %g or smaller", domain.value.max);
        result = buf;
        *(bufEnd - 1) = 0;
      }
      else
      {
        if ( domain.value.max == FLT_MAX )
          _snprintf(buf, bufLen, "Domain is any number %g or bigger", domain.value.min);
        else
          _snprintf(buf, bufLen, "Domain is any number from %g to %g", domain.value.min, domain.value.max);
        result = buf;
        *(bufEnd - 1) = 0;
      }
      return result;
    case DVAR_TYPE_VEC2:
    case DVAR_TYPE_VEC3:
    case DVAR_TYPE_VEC4:
      Dvar_VectorDomainToString(type, domain, buf, bufLen);
      result = buf;
      *(bufEnd - 1) = 0;
      return result;
    case DVAR_TYPE_INT:
      { int iMin = domain.integer.min, iMax = domain.integer.max;
      if ( iMin == INT_MIN )
      {
        if ( iMax == INT_MAX )
          _snprintf(buf, bufLen, "Domain is any integer");
        else
          _snprintf(buf, bufLen, "Domain is any integer %i or smaller", iMax);
      }
      else
      {
        if ( iMax == INT_MAX )
          _snprintf(buf, bufLen, "Domain is any integer %i or bigger", iMin);
        else
          _snprintf(buf, bufLen, "Domain is any integer from %i to %i", iMin, iMax);
      } }
      result = buf;
      *(bufEnd - 1) = 0;
      return result;
    case DVAR_TYPE_ENUM:
      { int enumCount = domain.enumeration.stringCount;
      const char **enumStrings = domain.enumeration.strings;
      written = _snprintf(buf, bufLen, "Domain is one of the following:");
      if ( written >= 0 )
      {
        buf += written;
        for ( enumIdx = 0; enumIdx < enumCount; enumIdx++ )
        {
          lineLen = _snprintf(buf, bufEnd - buf, "\n  %2i: %s", enumIdx, enumStrings[enumIdx]);
          if ( lineLen < 0 )
            break;
          if ( enumLinesCount )
            ++*enumLinesCount;
          buf += lineLen;
        }
      } }
      result = buf;
      *(bufEnd - 1) = 0;
      return result;
    case DVAR_TYPE_STRING:
      _snprintf(buf, bufLen, "Domain is any text");
      result = buf;
      *(bufEnd - 1) = 0;
      return result;
    case DVAR_TYPE_COLOR:
      _snprintf(buf, bufLen, "Domain is any 4-component color, in RGBA format");
      result = buf;
      *(bufEnd - 1) = 0;
      return result;
    default:
      Assert(0, s_assertDisable_Dvar_DomainToString_Internal);
      *buf = 0;
      *(bufEnd - 1) = 0;
      return buf;
  }
}

char *Dvar_DomainToString(unsigned char type, DvarLimits_t domain, char *buf, int bufLen)
{
  return Dvar_DomainToString_Internal(type, domain, buf, bufLen, 0);
}

/*
================
Dvar_PrintDomain

Prints the domain description for a dvar type.
================
*/
int Dvar_PrintDomain(char type, DvarLimits_t domain)
{
  char *str;
  char buf[MAX_OS_PATH];

  str = Dvar_DomainToString(type, domain, buf, sizeof(buf));
  return Sys_Printf("  %s\n", str);
}

bool Dvar_HasLatchedValue(const Dvar_t *dvar)
{
  return !Dvar_ValuesEqual(dvar->type, dvar->current, dvar->latched);
}

/*
================
Dvar_FindVar

Finds a dvar by name using the hash table.
================
*/
Dvar_t *Dvar_FindMalleableVar(const char *name)
{
  Dvar_t *dvar;

  dvar = (Dvar_t *)dvarHashTable[Dvar_GenerateHashValue(name)];
  if ( !dvar )
    return NULL;
  while ( Q_stricmp(name, dvar->name) )
  {
    dvar = dvar->hashNext;
    if ( !dvar )
      return NULL;
  }
  return dvar;
}

Dvar_t *Dvar_FindVar(const char *name)
{
  return Dvar_FindMalleableVar(name);
}

/*
================
Dvar_CompareVec4

Returns true if two vec4 values are exactly equal.
================
*/
bool Dvar_CompareVec4(float *v0, float *v1)
{
  return *v0 == *v1 && v0[1] == v1[1] && v0[2] == v1[2] && v0[3] == v1[3];
}

/*
================
Dvar_ClearModified

Clears the modified flag on a dvar.
================
*/
void Dvar_ClearModified(Dvar_t *dvar)
{
  Assert(dvar, s_assertDisable_Dvar_ClearModified);
  dvar->modified = 0;
}

/*
================
Dvar_GetString

Returns the string value of a dvar by name.
================
*/
const char *Dvar_GetString( const char *dvarName )
{
  Dvar_t *dvar;

  dvar = (Dvar_t *)Dvar_FindVar( (char *)dvarName );
  if ( !dvar )
    return (const char *)g_fsBaseGame;

  Assert(!(dvar->type != DVAR_TYPE_STRING && dvar->type != DVAR_TYPE_ENUM), s_assertDisable_Dvar_GetString);

  if ( dvar->type == DVAR_TYPE_ENUM )
    return Dvar_EnumToString( dvar );
  return dvar->current.string;
}

/*
================
Dvar_Shutdown

Frees all dvar resources and resets the dvar system.
================
*/
void Dvar_FreeNameString(const char *name)
{
  Z_Free((void *)name);
}

/* CoD4 0x47A2E0. */
char *Dvar_AllocNameString(const char *name)
{
  return Dvar_CopyString(name);
}

bool Dvar_ShouldFreeCurrentString(const Dvar_t *dvar)
{
  return dvar->current.string
      && dvar->current.string != dvar->latched.string
      && dvar->current.string != dvar->reset.string;
}

bool Dvar_ShouldFreeLatchedString(const Dvar_t *dvar)
{
  return dvar->latched.string
      && dvar->latched.string != dvar->current.string
      && dvar->latched.string != dvar->reset.string;
}

bool Dvar_ShouldFreeResetString(const Dvar_t *dvar)
{
  return dvar->reset.string
      && dvar->reset.string != dvar->current.string
      && dvar->reset.string != dvar->latched.string;
}

void Dvar_FreeString(DvarValue_t *value)
{
  char *string = (char *)value->string;

  if (string && *string && (string[1] || *string < '0' || *string > '9')
      && string != g_dvarStringOff && string != g_dvarStringOn)
  {
    Z_Free(string);
  }
  value->string = NULL;
}

void Dvar_Shutdown(void)
{
  int index;

  for (index = 0; index < dvarCount; ++index)
  {
    Dvar_t *dvar = &dvarPool[index];
    if ( dvar->type == DVAR_TYPE_STRING )
    {
      if (Dvar_ShouldFreeCurrentString(dvar))
        Dvar_FreeString(&dvar->current);
      else
        dvar->current.string = NULL;
      if (Dvar_ShouldFreeResetString(dvar))
        Dvar_FreeString(&dvar->reset);
      else
        dvar->reset.string = NULL;
      if (Dvar_ShouldFreeLatchedString(dvar))
        Dvar_FreeString(&dvar->latched);
      else
        dvar->latched.string = NULL;
    }
    if ( dvar->flags & DVAR_EXTERNAL )
      Dvar_FreeNameString(dvar->name);
  }
  dvarCount = 0;
  dvar_cheats = NULL;
  dvar_modifiedFlags = 0;
  isDvarSystemActive = 0;
  memset(dvarHashTable, 0, sizeof(dvarHashTable));
  sortedDvars = 0; /* KIWI's non-native sorted-list extension. */
}

/*
================
Dvar_AddFlags

Adds flags to a dvar, asserting that protected flags are not set.
================
*/
void Dvar_AddFlags( Dvar_t *dvar, int flags )
{
  Assert( dvar, s_assertDisable_Dvar_AddFlags );

  #define DVAR_PROTECTED_FLAGS 0x70F0  /* bits 4-7, 12-14: protected flag mask */
  Assert((flags & DVAR_PROTECTED_FLAGS) == 0, s_assertDisable_Dvar_AddFlags);

  dvar->flags |= flags;
}

/*
================
Dvar_CopyString

Copies a string for dvar storage, returning cached pointers for common
values like empty, single digits, "on", and "off".
================
*/
char *Dvar_CopyString(const char *string)
{
  char firstCh;
  unsigned int len;
  char secondCh;

  Assert(string, s_assertDisable_Dvar_CopyString);
  firstCh = *string;
  if ( !*string )
    return (char *)g_fsBaseGame;
  len = (unsigned int)strlen(string);
  secondCh = string[1];
  if ( secondCh )
  {
    if ( firstCh == 'o' )
    {
      if ( len == 3 )
      {
        if ( secondCh == 'f' && string[2] == 'f' && !string[3] )
          return (char *)g_dvarStringOff;
      }
      else if ( len == 2 && secondCh == 'n' && !string[2] )
      {
        return g_dvarStringOn;
      }
    }
  }
  else if ( firstCh >= '0' && firstCh <= '9' )
  {
    return digitStrings[firstCh - '0'];
  }
  return CopyStringHunk((char *)string);
}

/*
================
Dvar_AssignCurrentStringValue

Assigns a string to the dvar's current value, reusing latched/reset pointers when possible.
================
*/
void Dvar_AssignCurrentStringValue(Dvar_t *dvar, DvarValue_t *dest, char *string)
{
  Assert(string, s_assertDisable_Dvar_StringToValue);
  if (dvar->latched.string && (string == dvar->latched.string || !strcmp(string, dvar->latched.string)))
    dest->string = dvar->latched.string;
  else if (dvar->reset.string && (string == dvar->reset.string || !strcmp(string, dvar->reset.string)))
    dest->string = dvar->reset.string;
  else
    dest->string = Dvar_CopyString(string);
}

/*
================
Dvar_AssignLatchedStringValue

Assigns a string to the dvar's latched value, reusing current/reset pointers when possible.
================
*/
void Dvar_AssignLatchedStringValue(Dvar_t *dvar, DvarValue_t *dest, char *string)
{
  Assert(string, s_assertDisable_Dvar_StringToValue);
  if (dvar->current.string && (string == dvar->current.string || !strcmp(string, dvar->current.string)))
    dest->string = dvar->current.string;
  else if (dvar->reset.string && (string == dvar->reset.string || !strcmp(string, dvar->reset.string)))
    dest->string = dvar->reset.string;
  else
    dest->string = Dvar_CopyString(string);
}

/*
================
Dvar_AssignResetStringValue

Assigns a string to the dvar's reset value, reusing current/latched pointers when possible.
================
*/
void Dvar_AssignResetStringValue(Dvar_t *dvar, DvarValue_t *dest, const char *string)
{
  Assert(string, s_assertDisable_Dvar_AssignResetStringValue);
  if (dvar->current.string && (string == dvar->current.string || !strcmp(string, dvar->current.string)))
    dest->string = dvar->current.string;
  else if (dvar->latched.string && (string == dvar->latched.string || !strcmp(string, dvar->latched.string)))
    dest->string = dvar->latched.string;
  else
    dest->string = Dvar_CopyString(string);
}

/*
================
Dvar_StringToVec2

Parses a string into a vec2, using a rotating parse buffer.
================
*/
float *Dvar_StringToVec2(const char *string)
{
  int bufIdx;
  float *result;

  Assert(string, s_assertDisable_Dvar_StringToVec2);
  bufIdx = g_dvarStringBufIdx;
  if ( (unsigned int)(g_dvarStringBufIdx + 2) > DVAR_PARSE_BUF_SLOTS )
    bufIdx = 0;
  result = (float *)&dvarParseBuffer[bufIdx];
  g_dvarStringBufIdx = bufIdx + 2;
  result[0] = 0.0f;
  result[1] = 0.0f;
  sscanf( string, g_fmtVec2, &result[0], &result[1] );
  return result;
}

/*
================
Dvar_StringToVec3

Parses a string into a vec3, using a rotating parse buffer.
================
*/
float *Dvar_StringToVec3(const char *string)
{
  int bufIdx;
  float *result;

  Assert(string, s_assertDisable_Dvar_StringToVec3);
  bufIdx = g_dvarStringBufIdx;
  if ( (unsigned int)(g_dvarStringBufIdx + 3) > DVAR_PARSE_BUF_SLOTS )
    bufIdx = 0;
  result = (float *)&dvarParseBuffer[bufIdx];
  g_dvarStringBufIdx = bufIdx + 3;
  result[0] = 0.0f;
  result[1] = 0.0f;
  result[2] = 0.0f;
  sscanf( string, FMT_VECTOR3, &result[0], &result[1], &result[2] );
  return result;
}

/*
================
Dvar_StringToVec4

Parses a string into a vec4, using a rotating parse buffer.
================
*/
float *Dvar_StringToVec4( const char *string )
{
  int bufIdx;
  float *result;

  Assert( string, s_assertDisable_Dvar_StringToVec4 );
  bufIdx = g_dvarStringBufIdx;
  if ( (unsigned int)(g_dvarStringBufIdx + 4) > DVAR_PARSE_BUF_SLOTS )
    bufIdx = 0;
  result = (float *)&dvarParseBuffer[bufIdx];
  g_dvarStringBufIdx = bufIdx + 4;
  result[0] = 0.0f;
  result[1] = 0.0f;
  result[2] = 0.0f;
  result[3] = 0.0f;
  sscanf( string, g_fmtVec4, &result[0], &result[1], &result[2], &result[3] );
  return result;
}

/*
================
Dvar_StringToEnum

Converts a string to an enum index by name match, numeric parse, or prefix match.
================
*/
int Dvar_StringToEnum(DvarLimits_t *domain, const char *string)
{
  int i, numericValue;
  size_t len;

  Assert(domain, s_assertDisable_Dvar_StringToEnum);
  Assert(string, s_assertDisable_Dvar_StringToEnum);

  /* exact case-insensitive match */
  for ( i = 0; i < domain->enumeration.stringCount; i++ )
  {
    if ( !_stricmp(string, domain->enumeration.strings[i]) )
      return i;
  }

  /* try parsing as numeric index */
  numericValue = atoi(string);
  if ( *string >= '0' && *string <= '9' && numericValue >= 0 && numericValue < domain->enumeration.stringCount )
    return numericValue;

  /* prefix match */
  len = strlen(string);
  for ( i = 0; i < domain->enumeration.stringCount; i++ )
  {
    if ( !_strnicmp(string, domain->enumeration.strings[i], len) )
      return i;
  }

  return DVAR_INVALID_ENUM_INDEX;
}

/*
================
Dvar_StringToColor

Parses a string into a packed RGBA color (4 bytes), clamping each component to [0,1].
================
*/
void Dvar_StringToColor(const char *string, unsigned char *color)
{
  float colorVec[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
  double clamped;
  int i;

  sscanf(string, g_fmtVec4, &colorVec[0], &colorVec[1], &colorVec[2], &colorVec[3]);
  for ( i = 0; i < 4; i++ )
  {
    clamped = colorVec[i];
    if ( clamped > 1.0 )
      clamped = 1.0;
    if ( clamped < 0.0 )
      clamped = 0.0;
    color[i] = (unsigned char)xs_RoundToInt((float)(clamped * 255.0) + FISTP_BIAS);
  }
}

/*
================
Dvar_StringToValue

Converts a string to a DvarValue based on the specified type.
================
*/
DvarValue_t *Dvar_StringToValue(DvarValue_t *result, unsigned char type, DvarLimits_t domain, const char *string)
{
  DvarValue_t value = {};
  Assert( string, s_assertDisable_Dvar_StringToValue );

  switch ( (unsigned char)type )
  {
    case DVAR_TYPE_BOOL:
      value.enabled = Dvar_StringToBool(string); break;
    case DVAR_TYPE_FLOAT:
      value.value = Dvar_StringToFloat(string); break;
    case DVAR_TYPE_VEC2:
      memcpy(value.vector, Dvar_StringToVec2(string), 2 * sizeof(float)); break;
    case DVAR_TYPE_VEC3:
      memcpy(value.vector, Dvar_StringToVec3(string), 3 * sizeof(float)); break;
    case DVAR_TYPE_VEC4:
      memcpy(value.vector, Dvar_StringToVec4(string), 4 * sizeof(float)); break;
    case DVAR_TYPE_INT:
      value.integer = Dvar_StringToInt(string); break;
    case DVAR_TYPE_ENUM:
      value.integer = Dvar_StringToEnum(&domain, string); break;
    case DVAR_TYPE_STRING:
      value.string = string; break;
    case DVAR_TYPE_COLOR:
      Dvar_StringToColor(string, value.color); break;
    default:
      Assert(0, s_assertDisable_Dvar_StringToValue);
      break;
  }
  *result = value;
  return result;
}

/*
================
Dvar_ValuesEqual

Returns true if two dvar values of the given type are equal.
================
*/
bool Dvar_ValuesEqual(unsigned char type, DvarValue_t val0, DvarValue_t val1)
{
  switch ( type )
  {
    case DVAR_TYPE_BOOL:
      return val0.enabled == val1.enabled;
    case DVAR_TYPE_FLOAT:
      return val0.value == val1.value;
    case DVAR_TYPE_VEC2:
      return Dvar_ValuesEqual_Vec2(val0.vector, val1.vector);
    case DVAR_TYPE_VEC3:
      return VectorCompareExact(val0.vector, val1.vector);
    case DVAR_TYPE_VEC4:
      return Dvar_CompareVec4(val0.vector, val1.vector);
    case DVAR_TYPE_INT:
    case DVAR_TYPE_ENUM:
      return val0.integer == val1.integer;
    case DVAR_TYPE_STRING:
      Assert(val0.string, s_assertDisable_Dvar_StringToValue);
      Assert(val1.string, s_assertDisable_Dvar_StringToValue);
      return strcmp(val0.string, val1.string) == 0;
    case DVAR_TYPE_COLOR:
      return val0.integer == val1.integer;
    default:
      Assert(0, s_assertDisable_Dvar_ValuesEqual);
      return 0;
  }
}

/*
================
Dvar_SetLatchedValue

Sets the latched value of a dvar, handling vector and string types specially.
================
*/
void Dvar_SetLatchedValue(Dvar_t *dvar, DvarValue_t value)
{
  switch ( dvar->type )
  {
    case DVAR_TYPE_VEC2:
      dvar->latched.value = value.value;
      dvar->latched.vector[1] = value.vector[1];
      break;
    case DVAR_TYPE_VEC3:
      dvar->latched.value = value.value;
      dvar->latched.vector[1] = value.vector[1];
      dvar->latched.vector[2] = value.vector[2];
      break;
    case DVAR_TYPE_VEC4:
      dvar->latched = value;
      break;
    case DVAR_TYPE_STRING:
      if (dvar->latched.string != value.string)
      {
        DvarValue_t oldString = {};
        const bool shouldFree = Dvar_ShouldFreeLatchedString(dvar);
        if (shouldFree)
          oldString = dvar->latched;
        DvarValue_t latchedString = {};
        Dvar_AssignLatchedStringValue(dvar, &latchedString, (char *)value.string);
        dvar->latched = latchedString;
        if (shouldFree)
          Dvar_FreeString(&oldString);
      }
      break;
    default:
      dvar->latched = value;
      break;
  }
}

/*
================
Dvar_MakeLatchedValueCurrent

Native 0x478930 forwards the complete 16-byte latched carrier with the
internal source; this must not decay a vector to a pointer.
================
*/
void Dvar_MakeLatchedValueCurrent(Dvar_t *dvar)
{
  Dvar_SetVariant(dvar, dvar->latched, DVAR_SOURCE_INTERNAL);
}

/*
================
Dvar_SetVariant

Sets a dvar's value from any source, checking domain, permissions, and latching.
================
*/
void Dvar_SetVariant(Dvar_t *dvar, DvarValue_t value, DvarSetSource_t source)
{
  DvarValue_t oldString = {};
  DvarValue_t currentString = {};
  bool shouldFreeString;

  Assert(dvar, s_assertDisable_Dvar_SetVariant);
  Assert(dvar->name, s_assertDisable_Dvar_SetVariant);

  if (!Dvar_ValueInDomain(dvar->type, value, dvar->domain))
  {
    Com_Printf("'%s' is not a valid value for dvar '%s'\n", Dvar_ValueToString(dvar, value), dvar->name);
    Dvar_PrintDomain(dvar->type, dvar->domain);
    if (dvar->type == DVAR_TYPE_ENUM)
    {
      Assert(Dvar_ValueInDomain(dvar->type, dvar->reset, dvar->domain), s_assertDisable_Dvar_SetVariant);
      Dvar_SetVariant(dvar, dvar->reset, source);
    }
    return;
  }

  if (dvar->domainFunc && !dvar->domainFunc(dvar, value))
  {
    Com_Printf("'%s' is not a valid value for dvar '%s'\n\n", Dvar_ValueToString(dvar, value), dvar->name);
    return;
  }

  if (source == DVAR_SOURCE_EXTERNAL || source == DVAR_SOURCE_SCRIPT)
  {
    if (dvar->flags & DVAR_ROM)
    {
      Com_Printf("%s is read only.\n", dvar->name);
      return;
    }
    if (dvar->flags & DVAR_INIT)
    {
      Com_Printf("%s is write protected.\n", dvar->name);
      return;
    }
    if (source == DVAR_SOURCE_EXTERNAL && (dvar->flags & DVAR_CHEAT) && !dvar_cheats->current.enabled)
    {
      Com_Printf("%s is cheat protected.\n", dvar->name);
      return;
    }
    if (dvar->flags & DVAR_LATCH)
    {
      Dvar_SetLatchedValue(dvar, value);
      if (!Dvar_ValuesEqual(dvar->type, dvar->latched, dvar->current))
        Com_Printf("%s will be changed upon restarting.\n", dvar->name);
      return;
    }
  }
  else if (source == DVAR_SOURCE_DEVGUI && (dvar->flags & 0x800))
  {
    Dvar_SetLatchedValue(dvar, value);
    return;
  }

  if (Dvar_ValuesEqual(dvar->type, dvar->current, value))
  {
    Dvar_SetLatchedValue(dvar, dvar->current);
  }
  else
  {
    dvar_modifiedFlags |= dvar->flags;
    switch (dvar->type)
    {
    case DVAR_TYPE_VEC2:
      dvar->current.value = value.value;
      dvar->current.vector[1] = value.vector[1];
      dvar->latched.value = value.value;
      dvar->latched.vector[1] = value.vector[1];
      break;
    case DVAR_TYPE_VEC3:
      dvar->current.value = value.value;
      dvar->current.vector[1] = value.vector[1];
      dvar->current.vector[2] = value.vector[2];
      dvar->latched.value = value.value;
      dvar->latched.vector[1] = value.vector[1];
      dvar->latched.vector[2] = value.vector[2];
      break;
    case DVAR_TYPE_VEC4:
      dvar->current = value;
      dvar->latched = value;
      break;
    case DVAR_TYPE_STRING:
      Assert(dvar->name, s_assertDisable_Dvar_SetVariant);
      Assert(value.string != dvar->current.string || value.string == dvar->latched.string || value.string == dvar->reset.string, s_assertDisable_Dvar_SetVariant);
      shouldFreeString = Dvar_ShouldFreeCurrentString(dvar);
      if (shouldFreeString)
        oldString = dvar->current;
      Dvar_AssignCurrentStringValue(dvar, &currentString, (char *)value.string);
      dvar->current = currentString;
      if (Dvar_ShouldFreeLatchedString(dvar))
        Dvar_FreeString(&dvar->latched);
      dvar->latched.string = NULL;
      dvar->latched.string = dvar->current.string;
      if (shouldFreeString)
        Dvar_FreeString(&oldString);
      break;
    default:
      dvar->current = value;
      dvar->latched = value;
      break;
    }
    dvar->modified = 1;
  }
}

/* Adapt the scalar setter wrappers below to the inline value carrier.
 * String pointers must retain all address bits. */
static DvarValue_t Dvar_LegacyValue(const Dvar_t *dvar, intptr_t value)
{
  DvarValue_t result = {};
  if (dvar->type == DVAR_TYPE_VEC2 || dvar->type == DVAR_TYPE_VEC3 || dvar->type == DVAR_TYPE_VEC4)
  {
    const float *vector = (const float *)value;
    for (int i = 0; i < dvar->type; ++i)
      result.vector[i] = vector[i];
  }
  else if (dvar->type == DVAR_TYPE_STRING)
  {
    result.string = (const char *)value;
  }
  else
  {
    result.integer = (int)value;
  }
  return result;
}

static void Dvar_SetVariantLegacy(Dvar_t *dvar, intptr_t value, DvarSetSource_t source)
{
  Dvar_SetVariant(dvar, Dvar_LegacyValue(dvar, value), source);
}

#define Dvar_SetVariant(dvar, value, source) Dvar_SetVariantLegacy((dvar), (intptr_t)(value), (source))

/*
================
Dvar_GetUnpackedColor

Unpacks a dvar's color value (4 packed bytes) into 4 floats in [0,1] range.
================
*/
void Dvar_GetUnpackedColor( Dvar_t *dvar, float *expandedColor )
{
  unsigned char rgba[4];

  Assert( dvar, s_assertDisable_Dvar_GetUnpackedColor );
  Assert(!(dvar->type != DVAR_TYPE_COLOR && (dvar->type != DVAR_TYPE_STRING || (dvar->flags & DVAR_EXTERNAL) == 0)), s_assertDisable_Dvar_GetUnpackedColor);

  if ( dvar->type == DVAR_TYPE_COLOR )
    memcpy(rgba, &dvar->current.integer, 4);
  else
    Dvar_StringToColor((const char *)dvar->current.string, rgba);

  expandedColor[0] = (float)(rgba[0] * (1.0 / 255.0));
  expandedColor[1] = (float)(rgba[1] * (1.0 / 255.0));
  expandedColor[2] = (float)(rgba[2] * (1.0 / 255.0));
  expandedColor[3] = (float)(rgba[3] * (1.0 / 255.0));
}

/*
================
Dvar_PerformUnregistration

Converts a dvar to STRING type and marks it as DVAR_EXTERNAL.
================
*/
void Dvar_PerformUnregistration(Dvar_t *dvar)
{
  unsigned short dvarFlags;
  char type;
  char *latchedStr;
  char *resetStr;

  Assert(dvar, s_assertDisable_Dvar_PerformUnregistration);
  dvarFlags = dvar->flags;
  if ( (dvarFlags & DVAR_EXTERNAL) == 0 )
  {
    dvar->flags = dvarFlags | DVAR_EXTERNAL;
    dvar->name = Dvar_AllocNameString(dvar->name);
  }
  type = dvar->type;
  if ( type != DVAR_TYPE_STRING )
  {
    latchedStr = Dvar_DisplayableLatchedValue(dvar);
    dvar->current.string = Dvar_CopyString(latchedStr);
    if (Dvar_ShouldFreeLatchedString(dvar))
      Dvar_FreeString(&dvar->latched);
    dvar->latched.string = NULL;
    dvar->latched.string = dvar->current.string;
    if (Dvar_ShouldFreeResetString(dvar))
      Dvar_FreeString(&dvar->reset);
    dvar->reset.string = NULL;
    resetStr = Dvar_DisplayableResetValue(dvar);
    DvarValue_t resetValue = {};
    Dvar_AssignResetStringValue(dvar, &resetValue, resetStr);
    dvar->reset = resetValue;
    dvar->type = DVAR_TYPE_STRING;
  }
}

/*
================
Dvar_ClearModifiedFlags

Clears a system flag from a dvar, unregistering it if no system flags remain.
================
*/
void Dvar_ClearModifiedFlags( Dvar_t *dvar, int sysFlag )
{
  unsigned short dvarFlags;

  Assert(dvar, s_assertDisable_Dvar_ClearModifiedFlags);
  if ( (dvar->flags & DVAR_EXTERNAL) == 0 )
  {
    #define DVAR_SYS_FLAG  0x2000  /* (1 << 13) */
    #define DVAR_SYS_MASK  0x7000  /* (1 << 12) | (1 << 13) | (1 << 14) */
    Assert(!(sysFlag != DVAR_SYS_FLAG), s_assertDisable_Dvar_ClearModifiedFlags);
    dvarFlags = dvar->flags;
    Assert(!((dvarFlags & sysFlag) == 0), s_assertDisable_Dvar_ClearModifiedFlags);
    dvarFlags = dvar->flags;
    Assert(!((dvarFlags & DVAR_EXTERNAL) != 0), s_assertDisable_Dvar_ClearModifiedFlags);
    dvar->flags &= ~(unsigned short)sysFlag;
    if ( (dvar->flags & DVAR_SYS_MASK) == 0 )
      Dvar_PerformUnregistration(dvar);
  }
}

/*
================
Dvar_UpdateResetValue

Sets the reset value of a dvar, handling vector and string types specially.
================
*/
void Dvar_UpdateResetValue(Dvar_t *dvar, DvarValue_t value)
{
  Assert(dvar, s_assertDisable_Dvar_SetResetValue);
  switch ( dvar->type )
  {
    case DVAR_TYPE_VEC2:
      dvar->reset.value = value.value;
      dvar->reset.vector[1] = value.vector[1];
      break;
    case DVAR_TYPE_VEC3:
      dvar->reset.value = value.value;
      dvar->reset.vector[1] = value.vector[1];
      dvar->reset.vector[2] = value.vector[2];
      break;
    case DVAR_TYPE_VEC4:
      dvar->reset = value;
      break;
    case DVAR_TYPE_STRING:
      if (dvar->reset.string != value.string)
      {
        DvarValue_t oldString = {};
        const bool shouldFree = Dvar_ShouldFreeResetString(dvar);
        if (shouldFree)
          oldString = dvar->reset;
        DvarValue_t resetString = {};
        Dvar_AssignResetStringValue(dvar, &resetString, value.string);
        dvar->reset = resetString;
        if (shouldFree)
          Dvar_FreeString(&oldString);
      }
      break;
    default:
      dvar->reset = value;
      break;
  }
}


/*
================
Dvar_UpdateValue

Native 0x479E90 updates the inline current/latched DvarValue carriers.  In
particular, strings retain the same ownership/reuse rules as SetVariant.
================
*/
void Dvar_UpdateValue(Dvar_t *dvar, DvarValue_t value)
{
  DvarValue_t oldString = {};
  DvarValue_t currentString = {};
  bool shouldFreeCurrentString;

  Assert(dvar, s_assertDisable_Dvar_SetResetValue);
  switch (dvar->type)
  {
    case DVAR_TYPE_VEC2:
      dvar->current.value = value.value;
      dvar->current.vector[1] = value.vector[1];
      dvar->latched.value = value.value;
      dvar->latched.vector[1] = value.vector[1];
      break;
    case DVAR_TYPE_VEC3:
      dvar->current.value = value.value;
      dvar->current.vector[1] = value.vector[1];
      dvar->current.vector[2] = value.vector[2];
      dvar->latched.value = value.value;
      dvar->latched.vector[1] = value.vector[1];
      dvar->latched.vector[2] = value.vector[2];
      break;
    case DVAR_TYPE_VEC4:
      dvar->current = value;
      dvar->latched = value;
      break;
    case DVAR_TYPE_STRING:
      if (value.string != dvar->current.string)
      {
        shouldFreeCurrentString = Dvar_ShouldFreeCurrentString(dvar);
        if (shouldFreeCurrentString)
          oldString = dvar->current;
        Dvar_AssignCurrentStringValue(dvar, &currentString, (char *)value.string);
        dvar->current = currentString;
        if (Dvar_ShouldFreeLatchedString(dvar))
          Dvar_FreeString(&dvar->latched);
        dvar->latched.string = NULL;
        dvar->latched.string = dvar->current.string;
        if (shouldFreeCurrentString)
          Dvar_FreeString(&oldString);
      }
      break;
    default:
      dvar->current = value;
      dvar->latched = value;
      break;
  }
}

/*
================
Dvar_RegisterNew

Allocates a new dvar from the pool and inserts it into the sorted list and hash table.
================
*/
Dvar_t *Dvar_RegisterNew(
    char *dvarName,
    unsigned char type,
    unsigned short flags,
    DvarValue_t value,
    DvarLimits_t domain,
    const char *description)
{
  int idx, hashIdx;
  Dvar_t *dvar;
  intptr_t prevHash;

  idx = dvarCount;
  if ( dvarCount >= MAX_DVARS )
  {
    Com_ErrorLevel(0, "Can't create dvar '%s': %i dvars already exist", dvarName, MAX_DVARS);
    idx = dvarCount;
  }
  dvarCount = idx + 1;
  dvar = &dvarPool[idx];
  dvar->type = type;
  if ( (flags & DVAR_EXTERNAL) != 0 )
    dvar->name = CopyStringHunk(dvarName);
  else
    dvar->name = dvarName;

  switch ( type )
  {
    case DVAR_TYPE_VEC2:
      dvar->current.value = value.value;
      dvar->current.vector[1] = value.vector[1];
      dvar->latched.value = value.value;
      dvar->latched.vector[1] = value.vector[1];
      dvar->reset.value = value.value;
      dvar->reset.vector[1] = value.vector[1];
      break;
    case DVAR_TYPE_VEC3:
      dvar->current.value = value.value;
      dvar->current.vector[1] = value.vector[1];
      dvar->current.vector[2] = value.vector[2];
      dvar->latched.value = value.value;
      dvar->latched.vector[1] = value.vector[1];
      dvar->latched.vector[2] = value.vector[2];
      dvar->reset.value = value.value;
      dvar->reset.vector[1] = value.vector[1];
      dvar->reset.vector[2] = value.vector[2];
      break;
    case DVAR_TYPE_VEC4:
      dvar->current = value;
      dvar->latched = value;
      dvar->reset = value;
      break;
    case DVAR_TYPE_STRING:
      dvar->current.string = Dvar_CopyString(value.string);
      dvar->latched.string = dvar->current.string;
      dvar->reset.string = dvar->current.string;
      break;
    default:
      dvar->current = value;
      dvar->latched = value;
      dvar->reset = value;
      break;
  }
  dvar->domain = domain;
  dvar->modified = 0;
  dvar->domainFunc = 0;

  /* insert into sorted linked list */
  { Dvar_t **insertPos = (Dvar_t **)&sortedDvars;
  while ( *insertPos && _stricmp(dvar->name, (*insertPos)->name) >= 0 )
    insertPos = &(*insertPos)->next;
  dvar->next = *insertPos;
  *insertPos = dvar; }
  dvar->flags = flags;
  dvar->description = description;
  hashIdx = Dvar_GenerateHashValue(dvarName);
  prevHash = dvarHashTable[hashIdx];
  dvarHashTable[hashIdx] = (intptr_t)dvar;
  dvar->hashNext = (Dvar_t *)prevHash;
  return dvar;
}

/* Native registration helpers for the external-string-to-explicit-type path. */
static DvarValue_t Dvar_GetReinterpretedResetValue(
    Dvar_t *dvar,
    DvarValue_t resetValue,
    unsigned char type,
    unsigned short flags,
    DvarLimits_t domain)
{
  if ((dvar->flags & DVAR_AUTOEXEC) != 0
      && (flags & DVAR_CHANGEABLE_RESET) != 0
      && (dvar->flags & (DVAR_INIT | DVAR_ROM | DVAR_CHEAT)) == 0)
  {
    DvarValue_t parsedValue = {};
    Dvar_StringToValue(&parsedValue, type, domain, dvar->reset.string);
    return parsedValue;
  }
  return resetValue;
}

static void Dvar_ReinterpretDvar(
    Dvar_t *dvar,
    const char *dvarName,
    unsigned char type,
    unsigned short flags,
    DvarValue_t resetValue,
    DvarLimits_t domain)
{
  if ((dvar->flags & DVAR_EXTERNAL) != 0 && (flags & DVAR_EXTERNAL) == 0)
  {
    const DvarValue_t reinterpretedReset = Dvar_GetReinterpretedResetValue(
        dvar, resetValue, type, flags, domain);
    Dvar_PerformUnregistration(dvar);
    Dvar_FreeNameString(dvar->name);
    dvar->name = dvarName;
    dvar->flags &= ~DVAR_EXTERNAL;
    Dvar_MakeExplicitType(dvar, dvarName, type, flags, reinterpretedReset, domain);
  }
}

void Dvar_Reregister(
    Dvar_t *dvar,
    const char *dvarName,
    unsigned char type,
    unsigned short flags,
    DvarValue_t resetValue,
    DvarLimits_t domain,
    const char *description)
{
  Assert(dvar, s_assertDisable_Dvar_RegisterVariant);
  Assert(dvarName, s_assertDisable_Dvar_RegisterVariant);
  Assert(dvar->type == type || (dvar->flags & DVAR_EXTERNAL), s_assertDisable_Dvar_RegisterVariant);

  if (((dvar->flags ^ flags) & DVAR_EXTERNAL) != 0)
    Dvar_ReinterpretDvar(dvar, dvarName, type, flags, resetValue, domain);

  if ((dvar->flags & DVAR_EXTERNAL) != 0 && dvar->type != type)
  {
    Assert(dvar->type == DVAR_TYPE_STRING, s_assertDisable_Dvar_RegisterVariant);
    Dvar_MakeExplicitType(dvar, dvarName, type, flags, resetValue, domain);
  }

  Assert(dvar->type == type, s_assertDisable_Dvar_RegisterVariant);
  Assert(
      (dvar->flags & 0x9200) != 0 || (Dvar_ValuesEqual)(type, dvar->reset, resetValue),
      s_assertDisable_Dvar_RegisterVariant);

  dvar->flags |= flags;
  if (description)
    dvar->description = description;
  if ((dvar->flags & DVAR_CHEAT) != 0 && dvar_cheats && !dvar_cheats->current.enabled)
  {
    (Dvar_SetVariant)(dvar, dvar->reset, DVAR_SOURCE_INTERNAL);
    (Dvar_SetLatchedValue)(dvar, dvar->reset);
  }
  if ((dvar->flags & DVAR_LATCH) != 0)
    Dvar_MakeLatchedValueCurrent(dvar);
}

Dvar_t *Dvar_RegisterVariant(
    char *dvarName,
    unsigned char type,
    unsigned short flags,
    DvarValue_t value,
    DvarLimits_t domain,
    const char *description)
{
  Dvar_t *dvar;

  Assert((flags & DVAR_EXTERNAL) != 0 || CanKeepStringPointer((uintptr_t)dvarName), s_assertDisable_Dvar_FindMalleableVar);
  dvar = Dvar_FindMalleableVar(dvarName);
  if (!dvar)
  {
    return Dvar_RegisterNew(dvarName, type, flags, value, domain, description);
  }
  Dvar_Reregister(dvar, dvarName, type, flags, value, domain, description);
  return dvar;
}

/*
================
Dvar_FindMalleableVar

Finds an existing dvar by name or registers a new one with the given parameters.
================
*/
Dvar_t *Dvar_FindOrRegisterVariant(short flags, char *dvarName, int type, char *value, int domainMin, int domainMax)
{
  union { int i; float f; } udomMax;
  DvarValue_t nativeValue = {};
  DvarLimits_t domain = {};
  char *msg;
  int ar;
  Dvar_t *dvar;

  Assert((flags & DVAR_SYS_MASK) != 0, s_assertDisable_Dvar_FindMalleableVar);
  Assert((flags & DVAR_EXTERNAL) || CanKeepStringPointer((uintptr_t)dvarName), s_assertDisable_Dvar_FindMalleableVar);
  domain.integer.min = domainMin;
  domain.integer.max = domainMax;
  if (type == DVAR_TYPE_VEC2 || type == DVAR_TYPE_VEC3 || type == DVAR_TYPE_VEC4)
  {
    const float *vector = (const float *)value;
    for (int component = 0; component < type; ++component)
      nativeValue.vector[component] = vector[component];
  }
  else if (type == DVAR_TYPE_STRING)
  {
    nativeValue.string = value;
  }
  else
  {
    nativeValue.integer = (int)(intptr_t)value;
  }
  dvar = (Dvar_t *)dvarHashTable[Dvar_GenerateHashValue(dvarName)];
  if ( !dvar ) {
    return Dvar_RegisterNew(dvarName, (unsigned char)type, flags, nativeValue, domain, 0);
  }
  while ( Q_stricmp(dvarName, dvar->name) )
  {
    dvar = dvar->hashNext;
    if ( !dvar )
      return Dvar_RegisterNew(dvarName, (unsigned char)type, flags, nativeValue, domain, 0);
  }
  udomMax.i = domainMax;
  return Dvar_RegisterVariant(dvarName, (unsigned char)type, flags, nativeValue, domain, 0);
}

/*
================
Dvar_RegisterBool

Registers a boolean dvar with the given name, default value, and flags.
================
*/
Dvar_t *Dvar_RegisterBool(const char *dvarName, char value, short flags,
                          const char *description)
{
  DvarValue_t nativeValue = {};
  DvarLimits_t domain = {};
  nativeValue.enabled = (unsigned char)value;
  return Dvar_RegisterVariant((char *)dvarName, DVAR_TYPE_BOOL, flags, nativeValue, domain,
                              description);
}

/* KIWI's existing three-argument registration façade. */
/*
================
Dvar_RegisterInt

Registers an integer dvar with the given name, default value, min/max domain, and flags.
================
*/
Dvar_t *Dvar_RegisterInt(const char *dvarName, int value, int min, int max, short flags,
                          const char *description)
{
  DvarValue_t nativeValue = {};
  DvarLimits_t domain = {};
  nativeValue.integer = value;
  domain.integer.min = min;
  domain.integer.max = max;
  return Dvar_RegisterVariant((char *)dvarName, DVAR_TYPE_INT, flags, nativeValue, domain,
                              description);
}

/*
================
Dvar_RegisterString

Registers a string dvar with the given name, default value, and flags.
================
*/
Dvar_t *Dvar_RegisterString(const char *dvarName, const char *value, short flags,
                             const char *description)
{
  DvarValue_t nativeValue = {};
  DvarLimits_t domain = {};
  Assert(dvarName, s_assertDisable_Dvar_RegisterString);
  Assert(value, s_assertDisable_Dvar_RegisterString);
  Assert((flags & DVAR_EXTERNAL) || CanKeepStringPointer((uintptr_t)value), s_assertDisable_Dvar_RegisterString);
  nativeValue.string = value;
  return Dvar_RegisterVariant((char *)dvarName, DVAR_TYPE_STRING, flags, nativeValue, domain,
                              description);
}

/*
================
Dvar_SetBool

Sets a boolean dvar's value from the specified source.
================
*/
void Dvar_SetBoolFromSource(Dvar_t *dvar, char value, DvarSetSource_t source)
{
  Assert(dvar, s_assertDisable_Dvar_SetBool);
  Assert(dvar->name, s_assertDisable_Dvar_SetBool);
  Assert(!(dvar->type && (dvar->type != DVAR_TYPE_STRING || (dvar->flags & DVAR_EXTERNAL) == 0)), s_assertDisable_Dvar_SetBool);
  if ( dvar->type )
  {
    void *boolStr = value ? g_dvarString1 : g_dvarString0;
    Dvar_SetVariant(dvar, (intptr_t)boolStr, source);
  }
  else
  {
    Dvar_SetVariant(dvar, (intptr_t)(unsigned char)value, source);
  }
}

/*
================
Dvar_SetInt

Sets an integer dvar's value from the specified source.
================
*/
void Dvar_SetIntFromSource(Dvar_t *dvar, int value, DvarSetSource_t source)
{
  intptr_t valuePtr;
  char stringBuf[32];

  Assert( dvar, s_assertDisable_Dvar_SetInt );
  Assert( dvar->name, s_assertDisable_Dvar_SetInt );
  Assert(!(dvar->type != DVAR_TYPE_INT && dvar->type != DVAR_TYPE_ENUM && (dvar->type != DVAR_TYPE_STRING || (dvar->flags & DVAR_EXTERNAL) == 0)), s_assertDisable_Dvar_SetInt);

  if ( dvar->type == DVAR_TYPE_INT || dvar->type == DVAR_TYPE_ENUM )
    valuePtr = value;
  else
  {
    Com_sprintf( stringBuf, sizeof(stringBuf), "%i", value );
    valuePtr = (intptr_t)stringBuf;
  }
  Dvar_SetVariant( dvar, (intptr_t)valuePtr, source );
}

/*
================
Dvar_SetFloat

Sets a float dvar's value from the specified source.
================
*/
void Dvar_SetFloatFromSource(Dvar_t *dvar, float value, DvarSetSource_t source)
{
  char *valuePtr;
  char stringBuf[32];

  Assert( dvar, s_assertDisable_Dvar_SetFloat );
  Assert( dvar->name, s_assertDisable_Dvar_SetFloat );
  Assert(!(dvar->type != DVAR_TYPE_FLOAT && (dvar->type != DVAR_TYPE_STRING || (dvar->flags & DVAR_EXTERNAL) == 0)), s_assertDisable_Dvar_SetFloat);

  if ( dvar->type == DVAR_TYPE_FLOAT )
  {
    union { float f; int i; } u; u.f = value;
    valuePtr = (char *)(intptr_t)u.i;  /* raw float bits as DvarValue */
  }
  else
  {
    Com_sprintf( stringBuf, sizeof(stringBuf), "%g", value );
    valuePtr = stringBuf;
  }
  Dvar_SetVariant( dvar, (intptr_t)valuePtr, source );
}

/*
================
Dvar_SetVec2

Sets a vec2 dvar's value from the specified source.
================
*/
void Dvar_SetVec2FromSource(Dvar_t *dvar, float x, float y, DvarSetSource_t source)
{
  char *valuePtr;
  float vecBuf[4];
  char stringBuf[MAX_QPATH];

  Assert( dvar, s_assertDisable_Dvar_SetVec2 );
  Assert( dvar->name, s_assertDisable_Dvar_SetVec2 );
  Assert(!(dvar->type != DVAR_TYPE_VEC2 && (dvar->type != DVAR_TYPE_STRING || (dvar->flags & DVAR_EXTERNAL) == 0)), s_assertDisable_Dvar_SetVec2);
  if ( dvar->type == DVAR_TYPE_VEC2 )
  {
    vecBuf[0] = x;
    vecBuf[1] = y;
    valuePtr = (char *)vecBuf;
  }
  else
  {
    Com_sprintf( stringBuf, sizeof(stringBuf), "%g %g", x, y );
    valuePtr = stringBuf;
  }
  Dvar_SetVariant(dvar, (intptr_t)valuePtr, source);
}

/*
================
Dvar_SetVec3

Sets a vec3 dvar's value from the specified source.
================
*/
void Dvar_SetVec3FromSource(Dvar_t *dvar, float x, float y, float z, DvarSetSource_t source)
{
  char *valuePtr;
  float vecBuf[4];
  char stringBuf[96];

  Assert( dvar, s_assertDisable_Dvar_SetVec3 );
  Assert( dvar->name, s_assertDisable_Dvar_SetVec3 );
  Assert(!(dvar->type != DVAR_TYPE_VEC3 && (dvar->type != DVAR_TYPE_STRING || (dvar->flags & DVAR_EXTERNAL) == 0)), s_assertDisable_Dvar_SetVec3);

  if ( dvar->type == DVAR_TYPE_VEC3 )
  {
    vecBuf[0] = x;
    vecBuf[1] = y;
    vecBuf[2] = z;
    valuePtr = (char *)vecBuf;
  }
  else
  {
    Com_sprintf( stringBuf, sizeof(stringBuf), FMT_VECTOR3, x, y, z );
    valuePtr = stringBuf;
  }
  Dvar_SetVariant( dvar, (intptr_t)valuePtr, source );
}

/*
================
Dvar_SetVec4

Sets a vec4 dvar's value from the specified source.
================
*/
void Dvar_SetVec4FromSource(Dvar_t *dvar, float x, float y, float z, float w, DvarSetSource_t source)
{
  char *valuePtr;
  float vecBuf[4];
  char stringBuf[128];

  Assert( dvar, s_assertDisable_Dvar_SetVec4 );
  Assert( dvar->name, s_assertDisable_Dvar_SetVec4 );
  Assert(!(dvar->type != DVAR_TYPE_VEC4 && (dvar->type != DVAR_TYPE_STRING || (dvar->flags & DVAR_EXTERNAL) == 0)), s_assertDisable_Dvar_SetVec4);

  if ( dvar->type == DVAR_TYPE_VEC4 )
  {
    vecBuf[0] = x;
    vecBuf[1] = y;
    vecBuf[2] = z;
    vecBuf[3] = w;
    valuePtr = (char *)vecBuf;
  }
  else
  {
    Com_sprintf( stringBuf, sizeof(stringBuf), "%g %g %g %g", x, y, z, w );
    valuePtr = stringBuf;
  }
  Dvar_SetVariant( dvar, (intptr_t)valuePtr, source );
}

/*
================
Dvar_SetString

Sets a string or enum dvar's value from the specified source.
================
*/
void Dvar_SetStringFromSource(Dvar_t *dvar, char *string, DvarSetSource_t source)
{
  int ar;
  char dvarType;
  char *msg;
  DvarValue_t newValue = {};
  char stringBuf[MAX_OS_PATH];

  Assert(dvar, s_assertDisable_Dvar_SetString);
  Assert(dvar->name, s_assertDisable_Dvar_SetString);
  dvarType = dvar->type;
  Assert(dvarType == DVAR_TYPE_STRING || dvarType == DVAR_TYPE_ENUM, s_assertDisable_Dvar_SetString);
  Assert(string, s_assertDisable_Dvar_SetString);
  if ( dvar->type == DVAR_TYPE_STRING )
  {
    I_strncpyz( stringBuf, string, sizeof(stringBuf) );
    newValue.string = stringBuf;
  }
  else
  {
    newValue.integer = Dvar_StringToEnum(&dvar->domain, string);
    if ( newValue.integer == DVAR_INVALID_ENUM_INDEX )
      (void)va("%s doesn't include %s", dvar->name, string);
    Assert(newValue.integer != DVAR_INVALID_ENUM_INDEX, s_assertDisable_Dvar_SetString);
  }
  (Dvar_SetVariant)(dvar, newValue, source);
}

/*
================
Dvar_SetColor

Sets a color dvar's value from RGBA floats, packing into 4 bytes.
================
*/
void Dvar_SetColorFromSource(Dvar_t *dvar, float r, float g, float b, float a, DvarSetSource_t source)
{
  char *valuePtr;
  char stringBuf[128];

  Assert(dvar, s_assertDisable_Dvar_SetColor);
  Assert(dvar->name, s_assertDisable_Dvar_SetColor);
  Assert(dvar->type == DVAR_TYPE_COLOR || (dvar->type == DVAR_TYPE_STRING && (dvar->flags & DVAR_EXTERNAL)), s_assertDisable_Dvar_SetColor);
  if ( dvar->type == DVAR_TYPE_COLOR )
  {
    float rgba[4] = { r, g, b, a };
    unsigned char bytes[4];
    int i, packed;
    double clamped;

    for ( i = 0; i < 4; i++ )
    {
      clamped = rgba[i];
      if ( clamped > 1.0 ) clamped = 1.0;
      if ( clamped < 0.0 ) clamped = 0.0;
      bytes[i] = (unsigned char)xs_RoundToInt((float)(clamped * 255.0) + FISTP_BIAS);
    }
    packed = bytes[0] | (bytes[1] << 8) | (bytes[2] << 16) | (bytes[3] << 24);
    valuePtr = (char *)(intptr_t)packed;
  }
  else
  {
    Com_sprintf( stringBuf, sizeof(stringBuf), "%g %g %g %g", r, g, b, a );
    valuePtr = stringBuf;
  }
  Dvar_SetVariant(dvar, (intptr_t)valuePtr, source);
}

void Dvar_SetBool(Dvar_t *dvar, char value)
{
  Dvar_SetBoolFromSource(dvar, value, DVAR_SOURCE_INTERNAL);
}

void Dvar_SetInt(Dvar_t *dvar, int value)
{
  Dvar_SetIntFromSource(dvar, value, DVAR_SOURCE_INTERNAL);
}

void Dvar_SetFloat(Dvar_t *dvar, float value)
{
  Dvar_SetFloatFromSource(dvar, value, DVAR_SOURCE_INTERNAL);
}

void Dvar_SetVec2(Dvar_t *dvar, float x, float y)
{
  Dvar_SetVec2FromSource(dvar, x, y, DVAR_SOURCE_INTERNAL);
}

void Dvar_SetVec3(Dvar_t *dvar, float x, float y, float z)
{
  Dvar_SetVec3FromSource(dvar, x, y, z, DVAR_SOURCE_INTERNAL);
}

void Dvar_SetVec4(Dvar_t *dvar, float x, float y, float z, float w)
{
  Dvar_SetVec4FromSource(dvar, x, y, z, w, DVAR_SOURCE_INTERNAL);
}

void Dvar_SetString(Dvar_t *dvar, char *string)
{
  Dvar_SetStringFromSource(dvar, string, DVAR_SOURCE_INTERNAL);
}

void Dvar_SetColor(Dvar_t *dvar, float r, float g, float b, float a)
{
  Dvar_SetColorFromSource(dvar, r, g, b, a, DVAR_SOURCE_INTERNAL);
}

/*
================
Dvar_SetStringInternal

Sets a string dvar's value internally (convenience wrapper).
================
*/
void Dvar_SetStringInternal(Dvar_t *dvar, char *string)
{
  Dvar_SetString(dvar, string);
}

/*
================
Dvar_SetFromString

Parses a string into the dvar's type and sets the value from the specified source.
================
*/
void Dvar_SetFromStringFromSource(Dvar_t *dvar, char *string, DvarSetSource_t source)
{
  DvarValue_t parsedValue = {};
  char stringBuf[MAX_OS_PATH];

  I_strncpyz( stringBuf, string, sizeof(stringBuf) );
  Dvar_StringToValue(&parsedValue, dvar->type, dvar->domain, stringBuf);
  if ( dvar->type == DVAR_TYPE_ENUM && parsedValue.integer == DVAR_INVALID_ENUM_INDEX )
  {
    Com_Printf("'%s' is not a valid value for dvar '%s'\n", stringBuf, dvar->name);
    Dvar_PrintDomain(dvar->type, dvar->domain);
    parsedValue = dvar->reset;
  }
  (Dvar_SetVariant)(dvar, parsedValue, source);
}

/*
================
Dvar_SetStringByName

Sets a string dvar by name, registering it as external if not found.
================
*/
Dvar_t *Dvar_SetStringByName(const char *dvarName, char *string)
{
  Dvar_t *dvar;

  dvar = (Dvar_t *)Dvar_FindVar((char *)dvarName);
  if ( dvar )
  {
    Dvar_SetString(dvar, string);
    return dvar;
  }
  return Dvar_RegisterString(dvarName, string, DVAR_EXTERNAL, "External Dvar");
}

Dvar_t *Dvar_SetFromStringByNameFromSource(const char *dvarName, char *string,
                                            DvarSetSource_t source)
{
  Dvar_t *dvar = Dvar_FindVar(dvarName);
  if (!dvar)
    return Dvar_RegisterString(dvarName, string, DVAR_EXTERNAL, "External Dvar");
  Dvar_SetFromStringFromSource(dvar, string, source);
  return dvar;
}

/* Existing internal call surface retained under its pre-audit name. */
void Dvar_SetFromString(Dvar_t *dvar, char *string, DvarSetSource_t source)
{
  Dvar_SetFromStringFromSource(dvar, string, source);
}

void Dvar_Reset(Dvar_t *dvar, DvarSetSource_t source)
{
  Assert(dvar, s_assertDisable_Dvar_SetVariant);
  (Dvar_SetVariant)(dvar, dvar->reset, source);
}

void Dvar_SetDomainFunc(Dvar_t *dvar, bool (__cdecl *domainFunc)(Dvar_t *, DvarValue_t))
{
  Assert(dvar, s_assertDisable_Dvar_SetVariant);
  dvar->domainFunc = domainFunc;
  if (domainFunc && !domainFunc(dvar, dvar->current))
  {
    Com_Printf("'%s' is not a valid value for dvar '%s'\n\n", Dvar_ValueToString(dvar, dvar->current), dvar->name);
    Dvar_Reset(dvar, DVAR_SOURCE_INTERNAL);
  }
}

/*
================
Dvar_Init

Initializes the dvar system and registers the sv_cheats dvar.
================
*/
Dvar_t *Dvar_Init(void)
{
  isDvarSystemActive = 1;
  #define DVAR_FLAGS_SV_CHEATS 4120  /* DVAR_ROM | DVAR_SYS */
  dvar_cheats = Dvar_FindOrRegisterVariant( DVAR_FLAGS_SV_CHEATS, "sv_cheats", DVAR_TYPE_BOOL, NULL, 0, 0 );
  return dvar_cheats;
}
