/* CoD4 map-source error reporting recovered from cod4map.exe. */

#include "cod4map.h"

static char s_errorFileName[MAX_OS_PATH];
static char s_errorOccurred;
static int s_errorCount;
static char s_assertDisable_ErrorMessageQuote;
static char s_assertDisable_ErrorMessageNewline;

void Error_Init(void)
{
  BuildOutputPathFromLoadSource(".errlog", s_errorFileName, sizeof(s_errorFileName));
  remove(s_errorFileName);
}

int Error_HasErrors(void)
{
  return s_errorOccurred;
}

/* CoD4 0x414CD0: attach a source diagnostic to a winding's center and plane. */
void WindingError(int errorLevel, Winding_t *winding,
                  int mapInfoIndex, int entityNum, int brushNum,
                  const char *format, ...)
{
  char message[4096];
  float origin[3];
  float plane[4];
  va_list args;

  va_start(args, format);
  _vsnprintf(message, sizeof(message), format, args);
  va_end(args);
  message[sizeof(message) - 1] = 0;

  Assert(strchr(message, '"') == NULL, s_assertDisable_ErrorMessageQuote);
  Assert(strchr(message, '\n') == NULL, s_assertDisable_ErrorMessageNewline);

  WindingCenter(winding, origin);
  if ( !WindingHasPlane(winding, plane) )
  {
    plane[0] = 1.0f;
    plane[1] = 0.0f;
    plane[2] = 0.0f;
    plane[3] = 0.0f;
  }

  Error(errorLevel, origin, plane, mapInfoIndex, entityNum, brushNum,
        "%s", message);
}

void Error(int errorLevel, const float *origin, const float *direction,
           int mapInfoIndex, int entityNum, int brushNum,
           const char *format, ...)
{
  static const float defaultDirection[3] = { 1.0f, 0.0f, 0.0f };
  char message[4096];
  const char *mapName;
  const float *errorDirection;
  float localDirection[3];
  float negatedDirection[3];
  float errorOrigin[3];
  float localOrigin[3];
  float mapTransform[12];
  float mapScale;
  FILE *stream;
  va_list args;

  if ( errorLevel == 1 )
    s_errorOccurred = 1;

  va_start(args, format);
  _vsnprintf(message, sizeof(message), format, args);
  va_end(args);
  message[sizeof(message) - 1] = 0;

  Assert(strchr(message, '"') == NULL, s_assertDisable_ErrorMessageQuote);
  Assert(strchr(message, '\n') == NULL, s_assertDisable_ErrorMessageNewline);

  mapName = MapInfo_GetName(mapInfoIndex);

  if ( entityNum < 0 || brushNum < 0 )
  {
    printf("ERROR: %s\nIn map %s at %.0f %.0f %.0f\n",
           message, mapName, origin[0], origin[1], origin[2]);
  }
  else
  {
    printf("ERROR: %s\nIn map %s on entity %i brush %i at %.0f %.0f %.0f\n",
           message, mapName, entityNum, brushNum,
           origin[0], origin[1], origin[2]);
  }

  stream = fopen(s_errorFileName, "at");
  if ( !stream )
    return;

  errorDirection = direction ? direction : defaultDirection;
  VectorMA(origin, 16.0f, errorDirection, errorOrigin);
  negatedDirection[0] = -errorDirection[0];
  negatedDirection[1] = -errorDirection[1];
  negatedDirection[2] = -errorDirection[2];
  MapInfo_GetTransform(mapInfoIndex, mapTransform, &mapScale);
  MatrixTransposeTransformVector(mapTransform, negatedDirection, localDirection);
  MatrixTransposeTransformVector43(mapTransform, errorOrigin, localOrigin);
  if ( mapScale != 0.0f )
    VectorScale(localOrigin, 1.0f / mapScale, localOrigin);
  if ( g_currentEntityIndex >= 0 && g_currentEntityIndex < num_entities )
    VectorAdd(localOrigin, g_entities[g_currentEntityIndex].origin, localOrigin);

  fprintf(stream, "\"%s\" %i %i %g %g %g %g %g %g \"%s\"\n",
          mapName, entityNum, brushNum,
          localOrigin[0], localOrigin[1], localOrigin[2],
          localDirection[0], localDirection[1], localDirection[2], message);
  fclose(stream);
  ++s_errorCount;
}
