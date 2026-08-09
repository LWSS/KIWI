/*
q_shared.c — Shared quake utilities

Reconstructed from cod2map.exe by Rose.
*/

#include "cod4map.h"

DrawSurf_t g_drawSurfs[MAX_MAP_DRAW_SURFS];
MapDrawSurf_t g_nativeMapDrawSurfs[MAX_MAP_DRAW_SURFS];

int        (*_BigFloat)(int);
int        (*_BigLong)(int);
int        (*_BigShort)(int);
char          g_decimalPoint = '.';
const char    g_fmtVec2[] = "%g %g";
const char    g_fmtVec4[] = "%g %g %g %g";
const char    g_localizedBlankPrefix[] = "          ";
int           g_printfInit;
char          g_vaBigBuf[VA_BIG_BUF_SIZE];
int           g_vaBufferIdx;
int           g_vaBufferToggle;
char          g_vaBuffer[VA_BUFFER_SIZE];
char          g_vaCircularBuf[VA_CIRCULAR_BUF_SIZE];
char          g_vaTerminator;
void        *_LittleFloat;
void        *_LittleLong64;
void        *_LittleLong;
void        *_LittleShort;
size_t        SrcSizeInBytes = 1;
float         vec3_origin_float[3] = {0.0f, 0.0f, 0.0f};

char s_assertDisable_COM_GetExtension;
char s_assertDisable_I_stricmpWild;
char s_assertDisable_I_stricmpWild;
char s_assertDisable_I_strncat;
char s_assertDisable_I_strncpyz;
char s_assertDisable_I_strncpyz;
char s_assertDisable_I_strncpyz;
char s_assertDisable_MatrixTransformVector43;
char s_assertDisable_MatrixTransformVector43;
char s_assertDisable_MatrixTransposeTransformVector;
char s_assertDisable_MatrixVecScaleAddTransform43;
char s_assertDisable_Q_strcmp;
char s_assertDisable_Q_strcmp;
char s_assertDisable_Q_stricmp;
char s_assertDisable_Q_stricmp;
char s_assertDisable_TransformPlane;
char s_assertDisable_TransformPoint;
char s_assertDisable_TransformPoint;


/*
================
Q_strlen

String length wrapper.
================
*/
unsigned int Q_strlen(const char *str)
{
  return (unsigned int)strlen(str);
}

/*
================
VectorCompare

Returns 1 if v1 == v2 (exact float equality, all 3 components).
================
*/
int VectorCompare(float *v1, float *v2)
{
  return v1[0] == v2[0] && v1[1] == v2[1] && v1[2] == v2[2];
}

/*
================
Com_StringContains

Searches for str2 within str1 using compareFunc for comparison.
================
*/
const char *Com_StringContains(const char *str1, const char *str2, int (*compareFunc)(const char *, const char *)) {
  const char *ptr;
  int maxOffset;
  int i;

  ptr = str1;
  maxOffset = (int)(strlen(str1) - strlen(str2));
  i = 0;
  if ( maxOffset < 0 )
    return 0;
  while ( compareFunc(ptr, str2) )
  {
    ++i;
    ++ptr;
    if ( i > maxOffset )
      return 0;
  }
  return ptr;
}

/*
================
Com_Filter

Wildcard pattern matching: filter against name.
Supports '*' (any chars), '?' (any single char), and '[ranges]'.
================
*/
char Com_Filter(const char *filter, const char *name, int caseSensitive)
{
  char buf[MAX_OS_PATH];
  const char *ptr;
  int i, found;
  int (*compareFunc)(const char *, const char *);

  while ( *filter ) {
    if ( *filter == '*' ) {
      filter++;
      for ( i = 0; *filter; i++ ) {
        if ( *filter == '*' || *filter == '?' ) break;
        buf[i] = *filter;
        filter++;
      }
      buf[i] = '\0';
      if ( strlen(buf) ) {
        compareFunc = (int (*)(const char *, const char *))Q_stricmp;
        if ( !caseSensitive )
          compareFunc = (int (*)(const char *, const char *))Q_strcmp;
        ptr = Com_StringContains(name, buf, compareFunc);
        if ( !ptr ) return 0;
        name = ptr + strlen(buf);
      }
    }
    else if ( *filter == '?' ) {
      filter++;
      name++;
    }
    else if ( *filter == '[' && *(filter+1) == '[' ) {
      filter++;
    }
    else if ( *filter == '[' ) {
      filter++;
      found = 0;
      while ( *filter && !found ) {
        if ( *filter == ']' && *(filter+1) != ']' ) break;
        if ( *(filter+1) == '-' && *(filter+2) && (*(filter+2) != ']' || *(filter+3) == ']') ) {
          if ( caseSensitive ) {
            if ( *name >= *filter && *name <= *(filter+2) ) found = 1;
          }
          else {
            if ( toupper(*name) >= toupper(*filter) &&
                 toupper(*name) <= toupper(*(filter+2)) ) found = 1;
          }
          filter += 3;
        }
        else {
          if ( caseSensitive ) {
            if ( *filter == *name ) found = 1;
          }
          else {
            if ( toupper(*filter) == toupper(*name) ) found = 1;
          }
          filter++;
        }
      }
      if ( !found ) return 0;
      while ( *filter ) {
        if ( *filter == ']' && *(filter+1) != ']' ) break;
        filter++;
      }
      filter++;
      name++;
    }
    else {
      if ( caseSensitive ) {
        if ( *filter != *name ) return 0;
      }
      else {
        if ( toupper(*filter) != toupper(*name) ) return 0;
      }
      filter++;
      name++;
    }
  }
  return 1;
}

/*
================
Com_FilterPath

Normalizes paths (backslash/colon to forward slash) then does wildcard match.
================
*/
char Com_FilterPath(const char *filter, const char *name, int caseSensitive)
{
  int i;
  char new_filter[MAX_QPATH];
  char new_name[MAX_QPATH];

  for ( i = 0; i < MAX_QPATH - 1 && filter[i]; i++ ) {
    if ( filter[i] == '\\' || filter[i] == ':' )
      new_filter[i] = '/';
    else
      new_filter[i] = filter[i];
  }
  new_filter[i] = '\0';

  for ( i = 0; i < MAX_QPATH - 1 && name[i]; i++ ) {
    if ( name[i] == '\\' || name[i] == ':' )
      new_name[i] = '/';
    else
      new_name[i] = name[i];
  }
  new_name[i] = '\0';

  return Com_Filter(new_filter, new_name, caseSensitive);
}

/*
================
COM_GetExtension

Returns pointer to last '.' extension in filename, or end-of-string if none.
================
*/
char *COM_GetExtension(char *filename)
{
  char *p, *lastDot;

  Assert(filename, s_assertDisable_COM_GetExtension);
  lastDot = NULL;
  for ( p = filename; *p; p++ )
  {
    if ( *p == '.' )
      lastDot = p;
    else if ( *p == '/' || *p == '\\' )
      lastDot = NULL;
  }
  return lastDot ? lastDot : p;
}

char *COM_SkipPath(char *pathname)
{
  char *lastComponent = pathname;

  while ( *pathname )
  {
    if ( *pathname == '/' || *pathname == '\\' )
      lastComponent = pathname + 1;
    ++pathname;
  }

  return lastComponent;
}

char *COM_StripExtension(char *input, char *output)
{
  char *extension = COM_GetExtension(input);

  while ( input != extension )
    *output++ = *input++;
  *output = '\0';
  return input;
}

/*
================
BigShort / BigLong / BigFloat

Byte-swap trampolines: push ebp; mov ebp,esp; pop ebp; jmp [funcptr].
Session 133: IDA missed the argument because of the indirect jmp. LST confirms each takes 1 arg.
For PC platform, function pointers are identity (no-op swap).
================
*/
int BigShort(int l)
{
  return ((int (*)(int))_BigShort)(l);
}

int BigLong(int l)
{
  return ((int (*)(int))_BigLong)(l);
}

int BigFloat(int l)
{
  return ((int (*)(int))_BigFloat)(l);
}

/*
================
ShortSwap

Byte-swap a 16-bit value.
================
*/
int ShortSwap(short l)
{
  return ((unsigned char)l << 8) + ((unsigned char)(l >> 8));
}

/*
================
ShortNoSwap

Identity function — returns input unchanged (no byte swap).
================
*/
short ShortNoSwap(short l)
{
  return l;
}

/*
================
LongSwap

Byte-swap a 32-bit value (ntohl equivalent).
================
*/
int LongSwap(int l)
{
  return ((l >> 24) & 0xFF) | ((l >> 8) & 0xFF00) | ((l << 8) & 0xFF0000) | ((l << 24) & 0xFF000000);
}

/*
================
LongNoSwap

Identity function — returns input unchanged (no byte swap).
================
*/
int LongNoSwap(int l)
{
  return l;
}

/*
================
Long64Swap

Byte-swap a 64-bit value.
================
*/
unsigned long long Long64Swap(long long ll)
{
  return ((unsigned long long)((unsigned char)(ll >> 56)) <<  0)
       | ((unsigned long long)((unsigned char)(ll >> 48)) <<  8)
       | ((unsigned long long)((unsigned char)(ll >> 40)) << 16)
       | ((unsigned long long)((unsigned char)(ll >> 32)) << 24)
       | ((unsigned long long)((unsigned char)(ll >> 24)) << 32)
       | ((unsigned long long)((unsigned char)(ll >> 16)) << 40)
       | ((unsigned long long)((unsigned char)(ll >>  8)) << 48)
       | ((unsigned long long)((unsigned char)(ll >>  0)) << 56);
}

/*
================
Long64NoSwap

Identity function — returns input unchanged (no byte swap).
================
*/
long long Long64NoSwap(long long ll)
{
  return ll;
}

/*
================
FloatSwap

Byte-swap a float value. Takes int (raw bits), returns double.
================
*/
double FloatSwap(int f)
{
  int swapped;
  float result;

  swapped = ((f >> 24) & 0xFF) | ((f >> 8) & 0xFF00) | ((f << 8) & 0xFF0000) | ((f << 24) & 0xFF000000);
  memcpy(&result, &swapped, sizeof(result));
  return result;
}

/*
================
LongSwapUnsigned

32-bit byte-swap for unsigned int. Used as _LittleFloat function pointer.
================
*/
int LongSwapUnsigned(unsigned int l)
{
  return ((l >> 24) & 0xFF) | ((l >> 8) & 0xFF00) | ((l << 8) & 0xFF0000) | ((l << 24) & 0xFF000000);
}

/*
================
Q_islower

Returns 1 if character is lowercase ASCII letter.
================
*/
int Q_islower(int c)
{
  return c >= 'a' && c <= 'z';
}

int Q_isupper(int c)
{
  return c >= 'A' && c <= 'Z';
}

int Q_isalpha(int c)
{
  return Q_islower(c) || Q_isupper(c);
}

int Q_isnumeric(int c)
{
  return c >= '0' && c <= '9';
}

int Q_isalphanumeric(int c)
{
  return Q_isalpha(c) || Q_isnumeric(c);
}

/*
================
Q_stricmpn

Case-insensitive string comparison, up to n characters.
Returns 0 if equal, -1 if s1 < s2, 1 if s1 > s2.
================
*/
int Q_stricmpn(const char *s1, const char *s2, int n)
{
  int c1;
  int c2;

  while ( 1 )
  {
    c1 = *s1;
    c2 = *s2;
    ++s1;
    ++s2;
    if ( !n-- )
      return 0;
    if ( c1 != c2 )
    {
      if ( c1 >= 'a' && c1 <= 'z' )
        c1 -= ('a' - 'A');
      if ( c2 >= 'a' && c2 <= 'z' )
        c2 -= ('a' - 'A');
      if ( c1 != c2 )
        break;
    }
    if ( !c1 )
      return 0;
  }
  return 2 * (c1 >= c2) - 1;
}

/*
================
Q_strncmp

Case-sensitive string comparison, up to n characters.
Returns 0 if equal, -1 if s1 < s2, 1 if s1 > s2.
================
*/
int Q_strncmp(const char *s1, const char *s2, int n)
{
  int c1;
  int c2;

  while ( 1 )
  {
    c1 = *s1;
    c2 = *s2;
    ++s1;
    ++s2;
    if ( !n-- )
      return 0;
    if ( c1 != c2 )
      break;
    if ( !c1 )
      return 0;
  }
  return 2 * (c1 >= c2) - 1;
}

/*
================
Q_stricmp

Case-insensitive string comparison.
Returns 0 if equal, -1 if s0 < s1, 1 if s0 > s1.
================
*/
int Q_stricmp(const char *s0, const char *s1)
{
  Assert(s0, s_assertDisable_Q_stricmp);
  Assert(s1, s_assertDisable_Q_stricmp);
  return Q_stricmpn(s0, s1, INT_MAX);
}

/*
================
Q_strcmp

Case-sensitive string comparison.
Returns 0 if equal, -1 if s0 < s1, 1 if s0 > s1.
================
*/
int Q_strcmp(const char *s0, const char *s1)
{
  Assert(s0, s_assertDisable_Q_strcmp);
  Assert(s1, s_assertDisable_Q_strcmp);
  return Q_strncmp(s0, s1, INT_MAX);
}

/*
================
I_stricmpWild

Case-insensitive wildcard string comparison.
Supports '*' (match any) and '?' (match single char).
Returns 0 if match, non-zero otherwise.
================
*/
int I_stricmpWild(const char *wild, const char *s)
{
  const char *w, *p;
  char wc, sc;
  int diff;

  Assert(wild, s_assertDisable_I_stricmpWild);
  Assert(s, s_assertDisable_I_stricmpWild);
  w = wild;
  p = s;
  while ( 1 )
  {
    wc = *w++;
    if ( wc == '*' )
    {
      if ( !*w )
        return 0;
      if ( *p && !I_stricmpWild(w - 1, p + 1) )
        return 0;
      continue;
    }
    sc = *p++;
    if ( wc == '?' )
      continue;
    if ( !wc )
      return sc ? 1 : 0;
    diff = tolower(wc) - tolower(sc);
    if ( diff )
      return diff > 0 ? 1 : -1;
  }
}

/*
================
Q_strlwr

Convert string to lowercase in-place.
================
*/
char *Q_strlwr(char *s)
{
  char *p;

  for ( p = s; *p; p++ )
  {
    if ( *p >= 'A' && *p <= 'Z' )
      *p += ('a' - 'A');
  }
  return s;
}

/*
================
Com_sprintf

Safe sprintf with buffer size limit.
================
*/
int Com_sprintf(char *dest, size_t size, const char *format, ...)
{
  int result;
  va_list va;

  va_start(va, format);
  result = _vsnprintf(dest, size, format, va);
  dest[size - 1] = 0;
  return result;
}

/*
================
CanKeepStringPointer

Checks if a pointer is in temporary memory.
Returns 0 if pointer is on stack or in va() circular buffer (temporary, will be overwritten).
Returns 1 if pointer is safe to keep long-term.
================
*/
int CanKeepStringPointer(uintptr_t ptr)
{
  char stackLocal;
  /* check if pointer is on the current stack frame */
  if (ptr >= (uintptr_t)&stackLocal && ptr < (uintptr_t)&stackLocal + STACK_CHECK_SIZE)
    return 0;
  /* check if pointer is in the va() circular buffer */
  if (ptr >= (uintptr_t)g_vaCircularBuf && ptr < (uintptr_t)g_vaCircularBuf + sizeof(g_vaCircularBuf))
    return 0;
  return 1;
}

/*
================
fix_exponent_format

Restores MSVC6 3-digit exponent format (e.g. 1.00179e-005)
from modern UCRT 2-digit format (e.g. 1.00179e-05).
================
*/
static void fix_exponent_format(char *buf, unsigned int *plen)
{
  unsigned int len = *plen;
  unsigned int i = 0;
  while (i + 3 < len) {
    if ((buf[i] == 'e' || buf[i] == 'E') &&
        (buf[i+1] == '+' || buf[i+1] == '-') &&
        buf[i+2] >= '0' && buf[i+2] <= '9' &&
        buf[i+3] >= '0' && buf[i+3] <= '9') {
      if (i + 4 >= len || buf[i+4] < '0' || buf[i+4] > '9') {
        if (len + 1 < VA_CIRCULAR_BUF_SIZE) {
          memmove(buf + i + 3, buf + i + 2, len - (i + 2) + 1);
          buf[i+2] = '0';
          len++;
          i += 5;
        } else {
          i += 4;
        }
      } else {
        i += 5;
      }
    } else {
      i++;
    }
  }
  *plen = len;
}

/*
================
va

Variable-argument string formatter with circular buffer.
================
*/
char *va(const char *format, ...)
{
  unsigned int len;
  int index;
  char *result;
  va_list ArgList;

  va_start(ArgList, format);
  len = _vsnprintf(g_vaBuffer, VA_CIRCULAR_BUF_SIZE, format, ArgList);
  g_vaTerminator = 0;
  if ( len >= VA_CIRCULAR_BUF_SIZE )
    Com_ErrorLevel(1, "Attempted to overrun string in call to va()\n");
  index = g_vaBufferIdx;
  if ( (int)(g_vaBufferIdx + len) >= VA_BUFFER_SIZE )
  {
    index = 0;
    g_vaBufferIdx = 0;
  }
  result = (char *)&g_vaCircularBuf + index;
  memcpy(result, g_vaBuffer, len + 1);
  g_vaBufferIdx += len + 1;
  return result;
}

/*
================
Info_ValueForKey

Searches for a key in a backslash-delimited info string
and returns the associated value.
================
*/
char *Info_ValueForKey(const char *s, const char *key)
{
  const char *ptr;
  int altBuf;
  bool startsWith;
  char ch;
  char *kp;
  char *valWalk;
  char *vp;
  char *valStart;
  char keyBuf[BIG_INFO_STRING];

  ptr = s;
  if ( !s || !key )
    return "";
  /* BIG_INFO_STRING already defined in cod4map.h as 8192 (0x2000) */
  altBuf = g_vaBufferToggle ^ 1;
  startsWith = *s == '\\';
  g_vaBufferToggle ^= 1;
  if ( startsWith )
    ptr = s + 1;
  while ( 1 )
  {
    ch = *ptr;
    kp = keyBuf;
    if ( *ptr != '\\' )
    {
      while ( ch )
      {
        if ( kp - keyBuf >= BIG_INFO_STRING )
          Com_ErrorLevel(1, "Info_ValueForKey: oversize key %d", kp - keyBuf);
        *kp = ch;
        ch = ptr[1];
        ++kp;
        ++ptr;
        if ( ch == '\\' )
          break;
      }
      if ( ch != '\\' )
        return "";
    }
    ch = ptr[1];
    valWalk = (char *)ptr + 1;
    vp = (char *)&g_vaBigBuf + (VA_BIG_BUF_SIZE / 2) * altBuf;
    *kp = 0;
    for ( valStart = vp; ch != '\\'; ++valWalk )
    {
      if ( !ch )
        break;
      if ( vp - valStart >= BIG_INFO_STRING )
        Com_ErrorLevel(1, "Info_ValueForKey: oversize key %d", vp - valStart);
      *vp = ch;
      ch = valWalk[1];
      ++vp;
    }
    *vp = 0;
    if ( !Q_stricmp(key, keyBuf) )
      return valStart;
    if ( !*valWalk )
      return "";
    altBuf = g_vaBufferToggle;
    ptr = valWalk + 1;
  }
}

/*
================
Info_RemoveKey

Removes a key/value pair from a backslash-delimited info string.
================
*/
char *Info_RemoveKey(char *s, char *key)
{
  char *p;
  char *found;
  char *kp;
  char c;
  char *vp;
  char *start;
  char ch;
  char valBuf[MAX_OS_PATH];
  char keyBuf[MAX_OS_PATH];

  p = (char *)s;
  #define MAX_INFO_STRING 0x400
  if ( strlen(s) >= MAX_INFO_STRING )
    Com_ErrorLevel(1, "Info_RemoveKey: oversize infostring");
  found = strchr(key, '\\');
  if ( !found )
  {
    while ( 1 )
    {
      start = p;
      if ( *p == '\\' )
        ++p;
      ch = *p;
      kp = keyBuf;
      if ( *p != '\\' )
      {
        while ( ch )
        {
          *kp = ch;
          ch = p[1];
          ++kp;
          ++p;
          if ( ch == '\\' )
            break;
        }
        if ( ch != '\\' )
          break;
      }
      c = *++p;
      *kp = 0;
      for ( vp = valBuf; c != '\\'; ++p )
      {
        if ( !c )
          break;
        *vp = c;
        c = p[1];
        ++vp;
      }
      *vp = 0;
      found = (char *)(intptr_t)strcmp(key, keyBuf);
      if ( !found )
      {
        strcpy(start, p);
        return found;
      }
      if ( !*p )
        return found;
    }
  }
  return found;
}

/*
================
Info_RemoveKey_Big

Removes a key/value pair from a big (0x2000) info string.
================
*/
char *Info_RemoveKey_Big(char *s, char *key)
{
  char *p;
  char *found;
  char *kp;
  char c;
  char *vp;
  char *start;
  char ch;
  char valBuf[BIG_INFO_STRING];
  char keyBuf[BIG_INFO_STRING];

  p = (char *)s;
  if ( strlen(s) >= BIG_INFO_STRING )
    Com_ErrorLevel(1, "Info_RemoveKey_Big: oversize infostring");
  found = strchr(key, '\\');
  if ( !found )
  {
    while ( 1 )
    {
      start = p;
      if ( *p == '\\' )
        ++p;
      ch = *p;
      kp = keyBuf;
      if ( *p != '\\' )
      {
        while ( ch )
        {
          *kp = ch;
          ch = p[1];
          ++kp;
          ++p;
          if ( ch == '\\' )
            break;
        }
        if ( ch != '\\' )
          break;
      }
      c = *++p;
      *kp = 0;
      for ( vp = valBuf; c != '\\'; ++p )
      {
        if ( !c )
          break;
        *vp = c;
        c = p[1];
        ++vp;
      }
      *vp = 0;
      found = (char *)(intptr_t)strcmp(key, keyBuf);
      if ( !found )
      {
        strcpy(start, p);
        return found;
      }
      if ( !*p )
        return found;
    }
  }
  return found;
}

/*
================
MatrixTransformVector43

Transforms a vector by a 3x4 matrix (rotation + translation).
================
*/
float *MatrixTransformVector43(float *mat, float *in, float *out)
{
  Assert(in != out, s_assertDisable_MatrixTransformVector43);
  /* x87 80-bit precision: double intermediates match original fstp truncation */
  out[0] = (float)((double)mat[9] * (double)in[2] + (double)mat[3] * (double)in[0] + (double)mat[6] * (double)in[1] + (double)mat[0]);
  out[1] = (float)((double)mat[10] * (double)in[2] + (double)mat[4] * (double)in[0] + (double)mat[7] * (double)in[1] + (double)mat[1]);
  out[2] = (float)((double)mat[11] * (double)in[2] + (double)mat[5] * (double)in[0] + (double)mat[8] * (double)in[1] + (double)mat[2]);
  return out;
}

/*
================
MatrixTransformNormal43

Transforms a normal vector by a 3x4 matrix (rotation only, no translation).
================
*/
float *MatrixTransformNormal43(float *mat, float *in, float *out)
{
  Assert(in != out, s_assertDisable_MatrixTransformVector43);
#ifdef _WIN64
  x87_MatTransVec2(mat, in, out);
#else
  out[0] = (float)((double)mat[9] * (double)in[2] + (double)mat[3] * (double)in[0] + (double)mat[6] * (double)in[1]);
  out[1] = (float)((double)mat[10] * (double)in[2] + (double)mat[4] * (double)in[0] + (double)mat[7] * (double)in[1]);
  out[2] = (float)((double)mat[11] * (double)in[2] + (double)mat[5] * (double)in[0] + (double)mat[8] * (double)in[1]);
#endif
  return out;
}

/*
================
MatrixTransposeTransformVector

Transforms a vector by the transpose of a 3x4 matrix.
================
*/
float *MatrixTransposeTransformVector(float *mat, float *in, float *out)
{
  volatile float sum;

  Assert(in != out, s_assertDisable_MatrixTransposeTransformVector);
  sum = in[0] * mat[3];
  sum = sum + in[1] * mat[4];
  out[0] = sum + in[2] * mat[5];
  sum = in[0] * mat[6];
  sum = sum + in[1] * mat[7];
  out[1] = sum + in[2] * mat[8];
  sum = in[0] * mat[9];
  sum = sum + in[1] * mat[10];
  out[2] = sum + in[2] * mat[11];
  return out;
}

/*
================
MatrixTransposeTransformVector43

Transforms a position by the inverse of an orthonormal 3x4 transform.
================
*/
float *MatrixTransposeTransformVector43(float *mat, float *in, float *out)
{
  float translated[3];

  Assert(in != out, s_assertDisable_MatrixTransposeTransformVector);
  VectorSubtract(in, mat, translated);
  out[0] = translated[0] * mat[3] + translated[1] * mat[4] + translated[2] * mat[5];
  out[1] = translated[0] * mat[6] + translated[1] * mat[7] + translated[2] * mat[8];
  out[2] = translated[0] * mat[9] + translated[1] * mat[10] + translated[2] * mat[11];
  return out;
}

/*
================
TransformPoint

Transforms a full 3x4 matrix by another 3x4 matrix.
================
*/
float *TransformPoint(float *dst, float *src, float *out)
{
  Assert(out != dst, s_assertDisable_TransformPoint);
  Assert(out != src, s_assertDisable_TransformPoint);
  MatrixTransformNormal43(src, dst + 3, out + 3);
  MatrixTransformNormal43(src, dst + 6, out + 6);
  MatrixTransformNormal43(src, dst + 9, out + 9);
  return MatrixTransformVector43(src, dst, out);
}

/*
================
MatrixVecScaleAddTransform43

Transforms a vector by a 3x4 matrix with scale and translation.
out = (mat * in) * scale + mat_translation
================
*/
float *MatrixVecScaleAddTransform43(float *mat, float scale, float *in, float *out)
{
  Assert(in != out, s_assertDisable_MatrixVecScaleAddTransform43);
  /* x87 80-bit precision: double intermediates match original fstp truncation */
  *out = (float)(((double)mat[9] * (double)in[2] + (double)mat[3] * (double)*in + (double)mat[6] * (double)in[1]) * (double)scale + (double)*mat);
  out[1] = (float)(((double)mat[10] * (double)in[2] + (double)mat[4] * (double)*in + (double)mat[7] * (double)in[1]) * (double)scale + (double)mat[1]);
  out[2] = (float)(((double)mat[11] * (double)in[2] + (double)mat[5] * (double)*in + (double)mat[8] * (double)in[1]) * (double)scale + (double)mat[2]);
  return out;
}

/*
================
TransformPlane

Transforms a plane by a 3x4 transformation matrix and scale factor.
matrix = 12-float [tx,ty,tz, r00,r10,r20, r01,r11,r21, r02,r12,r22]
inPlane/outPlane = [nx, ny, nz, dist]
================
*/
void TransformPlane(float *matrix, float scale, float *inPlane, float *outPlane)
{
  Assert(inPlane != outPlane, s_assertDisable_TransformPlane);

  MatrixTransformNormal43(matrix, inPlane, outPlane);

  /* x87 80-bit precision: long double intermediates match original fstp truncation */
  { long double dist = (long double)matrix[2] * (long double)outPlane[2]
              + (long double)matrix[1] * (long double)outPlane[1]
              + (long double)scale * (long double)inPlane[3]
              + (long double)matrix[0] * (long double)outPlane[0];
  outPlane[3] = (float)dist; }

}

/*
================
FloatNoSwap

Identity function — returns input unchanged (no byte swap).
================
*/
double FloatNoSwap(float f)
{
  return f;
}

/*
================
Swap_Init

Initializes byte-swap function pointers.
Sets swap and no-swap variants for short/long/float types.
================
*/
void Swap_Init(void)
{
  _LittleShort = ShortNoSwap;
  _BigShort = (int (*)(int))ShortSwap;
  _LittleLong = LongNoSwap;
  _BigLong = (int (*)(int))LongSwap;
  _LittleLong64 = Long64Swap;
  _BigFloat = (int (*)(int))FloatSwap;
  _LittleFloat = LongSwapUnsigned;
}

int Swap_InitByteSwap()
{
  _LittleShort = ShortSwap;
  _BigShort = (int (*)(int))ShortNoSwap;
  _LittleLong = LongSwap;
  _BigLong = (int (*)(int))LongNoSwap;
  _LittleLong64 = Long64NoSwap;
  _BigFloat = (int (*)(int))FloatNoSwap;
  _LittleFloat = (void*)FloatNoSwap;
  return 1;
}

/*
================
I_strncpyz

Safe string copy with guaranteed null termination.
================
*/
char *I_strncpyz(char *dest, const char *src, int destsize)
{

  Assert(src, s_assertDisable_I_strncpyz);
  Assert(dest, s_assertDisable_I_strncpyz);
  Assert(destsize >= 1, s_assertDisable_I_strncpyz);
  strncpy(dest, src, destsize - 1);
  dest[destsize - 1] = 0;
  return dest;
}

/*
================
I_strncat

Safe string concatenation with size limit.
================
*/
char *I_strncat(const char *dest, int size, char *src)
{
  int len;

  Assert(size != sizeof(char *), s_assertDisable_I_strncat);
  len = (int)strlen(dest);
  if ( len >= size )
    Com_ErrorLevel(0, "I_strncat: already overflowed");
  return I_strncpyz((char *)&dest[len], src, size - len);
}
