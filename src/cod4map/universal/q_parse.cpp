/*
q_parse.c — Token parser

Reconstructed from cod2map.exe by Rose.
*/

#include "cod4map.h"

char        g_parseState[PARSE_STATE_SIZE];
const char *g_prevTokenPos = NULL;
const char *g_tokenPos = NULL;
char       *punctuation[15] =
{
  "+=", "-=", "*=", "/=", "&=", "|=", "++", "--",
  "&&", "||", "<=", ">=", "==", "!=", NULL
};

char s_assertDisable_Com_RestoreParserState;
char s_assertDisable_Com_RestoreParserState;
char s_assertDisable_Com_SaveParserState;
char s_assertDisable_Com_SaveParserState;
char s_assertDisable_ParseToken;


/*
================
Com_GetParseThreadInfo

Returns the global parse thread info structure
================
*/
ParseThreadInfo_t *Com_GetParseThreadInfo(void)
{
  return &g_parse;
}

/*
================
Com_BeginParseSession

Begins a new parse session, pushing the parse info stack
================
*/
void Com_BeginParseSession(const char *sessionName)
{
  ParseInfo_t *pi;
  int depth;
  int i;

  depth = g_parseDepth;
  if ( g_parseDepth == MAX_PARSE_INFO - 1 )
  {
    Sys_Printf("Already parsing:\n");
    for ( i = 0; i < g_parseDepth; i++ )
      Sys_Printf("%i. %s\n", i, g_parse.parseInfo[i].parseFile);
    Com_ErrorLevel(0, "Com_BeginParseSession: session overflow trying to parse %s\n", sessionName);
    depth = g_parseDepth;
  }
  g_parseDepth = depth + 1;
  pi = &g_parse.parseInfo[g_parseDepth];
  pi->lines = 1;
  pi->spaceDelimited = 1;
  pi->errorPrefix = "";
  pi->warningPrefix = "";
  pi->ungetToken = 0;
  pi->keepStringQuotes = 0;
  pi->csv = 0;
  pi->negativeNumbers = 0;
  pi->backup_lines = 0;
  pi->backup_text = 0;
  pi->parseFile = sessionName;
}

/*
================
Com_EndParseSession

Ends the current parse session, popping the parse info stack
================
*/
void Com_EndParseSession(void)
{
  if ( !g_parseDepth )
    Com_ErrorLevel(0, "Com_EndParseSession: session underflow");
  --g_parseDepth;
}

/*
================
Com_ResetParseSessions

Resets the parse session stack to empty
================
*/
void Com_ResetParseSessions(void)
{
  g_parseDepth = 0;
}

/*
================
Com_SetSpaceDelimited

Sets whether the parser treats spaces as token delimiters
================
*/
void Com_SetSpaceDelimited(int enabled)
{
  g_parse.parseInfo[g_parseDepth].spaceDelimited = enabled != 0;
}

/*
================
Com_SetParseNegativeNumbers

Sets whether the parser recognizes negative number tokens
================
*/
void Com_SetParseNegativeNumbers(int enabled)
{
  g_parse.parseInfo[g_parseDepth].negativeNumbers = enabled != 0;
}

/*
================
Com_GetCurrentParseLine

Returns the current line number of the active parse session
================
*/
int Com_GetCurrentParseLine(void)
{
  return g_parse.parseInfo[g_parseDepth].lines;
}

/*
================
Com_ScriptError

Prints a script error with file name and line number context
================
*/
void Com_ScriptError(const char *format, ...)
{
  ParseInfo_t *pi;
  char buffer[MAXPRINTMSG];
  va_list va;

  va_start(va, format);
  pi = &g_parse.parseInfo[g_parseDepth];
  _vsnprintf(buffer, sizeof(buffer), format, va);
  if ( g_parseDepth )
    Com_ErrorLevel(1, "%sFile %s, line %i: %s", pi->errorPrefix, pi->parseFile, pi->lines, buffer);
  else
    Com_ErrorLevel(1, "%s", buffer);
}

/*
================
Com_ScriptWarning

Prints a script warning with file name and line number context
================
*/
void Com_ScriptWarning(const char *format, ...)
{
  ParseInfo_t *pi;
  char buffer[MAXPRINTMSG];
  va_list va;

  va_start(va, format);
  pi = &g_parse.parseInfo[g_parseDepth];
  _vsnprintf(buffer, sizeof(buffer), format, va);
  if ( g_parseDepth )
    Sys_Printf("%sFile %s, line %i: %s", pi->warningPrefix, pi->parseFile, pi->lines, buffer);
  else
    Sys_Printf("%s", buffer);
}

/*
================
Com_UngetToken

Pushes back the current token so the next parse call returns it again
================
*/
void Com_UngetToken(void)
{
  ParseInfo_t *pi;

  pi = &g_parse.parseInfo[g_parseDepth];
  if ( pi->ungetToken )
    Com_ScriptError("UngetToken called twice");
  pi->ungetToken = 1;
  g_tokenPos = g_prevTokenPos;
}

/*
================
Com_SaveParserState

Saves the current parser state into a mark for later restoration
================
*/
void Com_SaveParserState(const char **text, ComParseMark_t *mark)
{
  ParseInfo_t *pi;

  pi = &g_parse.parseInfo[g_parseDepth];
  Assert(text, s_assertDisable_Com_SaveParserState);
  Assert(mark, s_assertDisable_Com_SaveParserState);
  mark->lines = pi->lines;
  mark->text = *text;
  mark->ungetToken = (unsigned char)pi->ungetToken;
  mark->backup_lines = pi->backup_lines;
  mark->backup_text = pi->backup_text;
}

/*
================
Com_RestoreParserState

Restores a previously saved parser state from a mark
================
*/
void Com_RestoreParserState(const char **text, ComParseMark_t *mark)
{
  ParseInfo_t *pi;

  pi = &g_parse.parseInfo[g_parseDepth];
  Assert(text, s_assertDisable_Com_RestoreParserState);
  Assert(mark, s_assertDisable_Com_RestoreParserState);
  pi->lines = mark->lines;
  *text = mark->text;
  pi->ungetToken = mark->ungetToken != 0;
  pi->backup_lines = mark->backup_lines;
  pi->backup_text = mark->backup_text;
}

/*
================
Com_SkipWhitespace

Advances past whitespace chars, tracking newlines
================
*/
char *Com_SkipWhitespace(char *data, int *hasNewline)
{
  int ch;
  ParseInfo_t *pi;

  ch = *data;
  pi = &g_parse.parseInfo[g_parseDepth];
  if ( ch <= ' ' )
  {
    while ( ch )
    {
      if ( ch == '\n' )
      {
        ++pi->lines;
        *hasNewline = 1;
      }
      ch = *++data;
      if ( ch > ' ' )
        return data;
    }
    return 0;
  }
  return data;
}

/*
================
Com_GetCurrentParseLine2

Returns the current line pointer (token position) rather than the line number
================
*/
intptr_t Com_GetCurrentParseLine2(void)
{
  return (intptr_t)g_tokenPos;
}

/*
================
Com_ParseCSV

Parses one CSV field from input, handling quoted strings with escaped quotes
================
*/
char *Com_ParseCSV(int crossNewline, char **parsePtr)
{
  ParseInfo_t *pi;
  unsigned int len;
  char *data;
  char ch;
  int endOfData;

  pi = &g_parse.parseInfo[g_parseDepth];
  len = 0;
  data = *parsePtr;
  pi->token[0] = 0;
  if ( crossNewline )
  {
    while ( *data == '\r' || *data == '\n' )
      ++data;
  }
  else if ( *data == '\r' || *data == '\n' )
  {
    return pi->token;
  }
  g_prevTokenPos = g_tokenPos;
  g_tokenPos = data;
  ch = *data;
  endOfData = !ch;
  while ( !endOfData && ch != ',' && ch != '\n' )
  {
    if ( ch != '\r' )
    {
      if ( ch == '"' )
      {
        while ( 1 )
        {
          ++data;
          while ( *data == '"' )
          {
            if ( data[1] != '"' )
              break;
            if ( len < MAX_TOKEN_CHARS - 1 )
              pi->token[len++] = '"';
            data += 2;
          }
          if ( *data != '"' )
          {
            if ( len < MAX_TOKEN_CHARS - 1 )
              pi->token[len++] = *data;
          }
          else
          {
            break;
          }
        }
      }
      else
      {
        if ( len < MAX_TOKEN_CHARS - 1 )
          pi->token[len++] = ch;
      }
    }
    ch = *++data;
    if ( !ch )
      endOfData = 1;
  }
  if ( !endOfData )
  {
    if ( *data != '\n' )
      ++data;
    *parsePtr = data;
  }
  else
  {
    *parsePtr = 0;
  }
  pi->token[len] = 0;
  return pi->token;
}

/*
================
Com_SkipRestOfLine

Skips to end of current line, increments line counter on newline
================
*/
void Com_SkipRestOfLine(char **parsePtr)
{
  char *data;

  data = *parsePtr;
  if ( !data )
    return;

  while ( *data && *data != '\n' )
    data++;

  if ( *data == '\n' )
  {
    g_parse.parseInfo[g_parseDepth].lines++;
    data++;
  }
  *parsePtr = data;
}

/*
================
ParseToken

Core token parsing function. Reads the next token from input.
Handles comments (line and block), quoted strings, numbers, identifiers,
and multi-character punctuation.
================
*/
char *ParseToken(char **parsePtr, int crossNewline)
{
  int tokenLen;
  ParseInfo_t *pi;
  char *data;
  char ch;
  char nextCh;
  char expCh;
  const char *specialToken;
  char **punc;
  int specialLen;
  int j;
  int hasNewline;

  tokenLen = 0;
  pi = &g_parse.parseInfo[g_parseDepth];
  hasNewline = 0;
  Assert(parsePtr, s_assertDisable_ParseToken);
  data = *parsePtr;
  pi->token[0] = 0;
  if ( *parsePtr == 0 )
  {
    *parsePtr = 0;
    return pi->token;
  }
  pi->backup_lines = pi->lines;
  ch = pi->csv;
  pi->backup_text = *parsePtr;
  if ( ch )
    return Com_ParseCSV(crossNewline, parsePtr);

  /* skip whitespace and comments */
  data = Com_SkipWhitespace(data, &hasNewline);
  if ( !data )
  {
    *parsePtr = 0;
    return pi->token;
  }
  while ( 1 )
  {
    if ( hasNewline && !crossNewline )
      return pi->token;
    ch = *data;
    if ( *data != '/' )
      break;
    nextCh = data[1];
    if ( nextCh == '/' )
    {
      /* skip line comment */
      do
      {
        if ( ch == '\n' )
          break;
        ch = *++data;
      }
      while ( ch );
    }
    else if ( nextCh == '*' )
    {
      /* skip block comment */
      while ( 1 )
      {
        if ( ch == '*' && data[1] == '/' )
        {
          if ( *data )
            data += 2;
          break;
        }
        if ( ch == '\n' )
          ++pi->lines;
        ch = *++data;
        if ( !ch )
          break;
      }
    }
    else
    {
      break;
    }
    data = Com_SkipWhitespace(data, &hasNewline);
    if ( !data )
    {
      *parsePtr = 0;
      return pi->token;
    }
  }

  g_prevTokenPos = g_tokenPos;
  g_tokenPos = data;

  /* handle quoted strings */
  if ( ch == '"' )
  {
    if ( pi->keepStringQuotes )
    {
      pi->token[0] = '"';
      tokenLen = 1;
    }
    ++data;
    while ( 1 )
    {
      ch = *data++;
      if ( ch == '\\' )
      {
        if ( *data == '"' || *data == '\\' )
        {
          ch = *data++;
        }
        else
        {
          if ( *data == '\n' )
            ++pi->lines;
        }
        if ( tokenLen < MAX_TOKEN_CHARS - 1 )
          pi->token[tokenLen++] = ch;
        continue;
      }
      if ( ch == '"' || ch == 0 )
        break;
      if ( tokenLen < MAX_TOKEN_CHARS - 1 )
        pi->token[tokenLen++] = ch;
    }
    if ( pi->keepStringQuotes )
    {
      pi->token[tokenLen] = '"';
      pi->token[tokenLen + 1] = 0;
      *parsePtr = data;
      return pi->token;
    }
    pi->token[tokenLen] = 0;
    *parsePtr = data;
    return pi->token;
  }

  /* space-delimited mode: read until whitespace */
  if ( pi->spaceDelimited )
  {
    do
    {
      if ( tokenLen < MAX_TOKEN_CHARS - 1 )
        pi->token[tokenLen++] = ch;
      ch = *++data;
    }
    while ( ch > ' ' );
    if ( tokenLen == MAX_TOKEN_CHARS )
      tokenLen = 0;
    pi->token[tokenLen] = 0;
    *parsePtr = data;
    return pi->token;
  }

  /* check for a number */
  if ( (ch >= '0' && ch <= '9')
    || (pi->negativeNumbers && ch == '-' && data[1] >= '0' && data[1] <= '9')
    || (ch == '.' && data[1] >= '0' && data[1] <= '9') )
  {
    do
    {
      if ( tokenLen < MAX_TOKEN_CHARS - 1 )
        pi->token[tokenLen++] = ch;
      ch = *++data;
    }
    while ( ch >= '0' && ch <= '9' || ch == '.' );
    if ( ch == 'e' || ch == 'E' )
    {
      if ( tokenLen < MAX_TOKEN_CHARS - 1 )
        pi->token[tokenLen++] = ch;
      expCh = *++data;
      if ( expCh == '-' || expCh == '+' )
      {
        if ( tokenLen < MAX_TOKEN_CHARS - 1 )
          pi->token[tokenLen++] = expCh;
        expCh = *++data;
      }
      do
      {
        if ( tokenLen < MAX_TOKEN_CHARS - 1 )
          pi->token[tokenLen++] = expCh;
        expCh = *++data;
      }
      while ( expCh >= '0' && expCh <= '9' );
    }
    if ( tokenLen == MAX_TOKEN_CHARS )
      tokenLen = 0;
    pi->token[tokenLen] = 0;
    *parsePtr = data;
    return pi->token;
  }

  /* check for an identifier */
  if ( ch >= 'a' && ch <= 'z' || ch >= 'A' && ch <= 'Z' || ch == '_' || ch == '/' || ch == '\\' )
  {
    do
    {
      if ( tokenLen < MAX_TOKEN_CHARS - 1 )
        pi->token[tokenLen++] = ch;
      ch = *++data;
    }
    while ( ch >= 'a' && ch <= 'z' || ch >= 'A' && ch <= 'Z' || ch == '_' || ch >= '0' && ch <= '9' );
    if ( tokenLen == MAX_TOKEN_CHARS )
      tokenLen = 0;
    pi->token[tokenLen] = 0;
    *parsePtr = data;
    return pi->token;
  }

  /* check for multi-character punctuation */
  specialToken = punctuation[0];
  for ( punc = punctuation; specialToken; ++punc )
  {
    specialLen = Q_strlen(specialToken);
    for ( j = 0; j < specialLen; ++j )
    {
      if ( data[j] != specialToken[j] )
        break;
    }
    if ( j == specialLen )
    {
      memcpy(pi->token, *punc, specialLen);
      pi->token[specialLen] = 0;
      *parsePtr = &data[specialLen];
      return pi->token;
    }
    specialToken = punc[1];
  }

  /* single character punctuation */
  pi->token[0] = *data;
  pi->token[1] = 0;
  *parsePtr = data + 1;
  return pi->token;
}

/*
================
COM_Parse

Parses next token, crossing newlines
================
*/
char *COM_Parse(char **parsePtr)
{
  ParseInfo_t *pi;

  pi = &g_parse.parseInfo[g_parseDepth];
  if ( pi->ungetToken )
  {
    pi->ungetToken = 0;
    *parsePtr = (char *)pi->backup_text;
    pi->lines = pi->backup_lines;
  }
  return ParseToken(parsePtr, 1);
}

/*
================
COM_ParseExt

Extended token parser - stops on newline
================
*/
char *COM_ParseExt(char **parsePtr)
{
  ParseInfo_t *pi;

  pi = &g_parse.parseInfo[g_parseDepth];
  if ( pi->ungetToken )
  {
    pi->ungetToken = 0;
    if ( !pi->spaceDelimited )
      return pi->token;
    *parsePtr = (char *)pi->backup_text;
    pi->lines = pi->backup_lines;
  }
  return ParseToken(parsePtr, 0);
}

/*
================
COM_MatchToken

Parses next token and verifies it matches expected string
================
*/
int COM_MatchToken(char **parsePtr, const char *match, int warnOnly)
{
  ParseInfo_t *pi;
  char *token;

  pi = &g_parse.parseInfo[g_parseDepth];
  if ( pi->ungetToken )
  {
    pi->ungetToken = 0;
    *parsePtr = (char *)pi->backup_text;
    pi->lines = pi->backup_lines;
  }
  token = ParseToken(parsePtr, 1);
  if ( !strcmp(token, match) )
    return 1;
  if ( warnOnly )
    Com_ScriptWarning("MatchToken: got '%s', expected '%s'\n", token, match);
  else
    Com_ScriptError("MatchToken: got '%s', expected '%s'\n", token, match);
  return 0;
}

/*
================
COM_ParseFloat

Parses the next token as a floating-point number
================
*/
float COM_ParseFloat(char **parsePtr)
{
  char *token;

  token = COM_ParseExt(parsePtr);
  return (float)atof(token);
}

/*
================
COM_ParseInt

Parses the next token as an integer
================
*/
int COM_ParseInt(char **parsePtr)
{
  ParseInfo_t *pi;
  char *token;

  pi = &g_parse.parseInfo[g_parseDepth];
  if ( pi->ungetToken )
  {
    pi->ungetToken = 0;
    *parsePtr = (char *)pi->backup_text;
    pi->lines = pi->backup_lines;
  }
  token = ParseToken(parsePtr, 1);
  return atol(token);
}

/*
================
COM_ParseIntExt

Parses the next token as an integer, stopping at newlines
================
*/
int COM_ParseIntExt(char **parsePtr)
{
  char *token;

  token = COM_ParseExt(parsePtr);
  return atol(token);
}

/*
================
COM_Parse1DMatrix

Parses "( val0 val1 ... valN )" into float array
================
*/
char *COM_Parse1DMatrix(char **parsePtr, int numElements, float *outArray)
{
  int i;
  ParseInfo_t *pi;
  char *token;
  char *result;

  COM_MatchToken(parsePtr, "(", 0);
  for ( i = 0; i < numElements; ++i )
  {
    pi = &g_parse.parseInfo[g_parseDepth];
    if ( pi->ungetToken )
    {
      pi->ungetToken = 0;
      *parsePtr = (char *)pi->backup_text;
      pi->lines = pi->backup_lines;
    }
    token = ParseToken(parsePtr, 1);
    outArray[i] = (float)atof(token);
  }
  pi = &g_parse.parseInfo[g_parseDepth];
  if ( pi->ungetToken )
  {
    pi->ungetToken = 0;
    *parsePtr = (char *)pi->backup_text;
    pi->lines = pi->backup_lines;
  }
  result = ParseToken(parsePtr, 1);
  if ( strcmp(result, ")") )
    Com_ScriptError("MatchToken: %s != %s\n", result, ")");
  return result;
}

/*
================
COM_Parse2DMatrix

Parses "( (row0) (row1) ... )" into 2D float array
================
*/
char *COM_Parse2DMatrix(char **parsePtr, int rows, int cols, float *outArray)
{
  int i;
  ParseInfo_t *pi;
  char *result;

  COM_MatchToken(parsePtr, "(", 0);
  for ( i = 0; i < rows; i++ )
  {
    COM_Parse1DMatrix(parsePtr, cols, outArray);
    outArray += cols;
  }
  pi = &g_parse.parseInfo[g_parseDepth];
  if ( pi->ungetToken )
  {
    pi->ungetToken = 0;
    *parsePtr = (char *)pi->backup_text;
    pi->lines = pi->backup_lines;
  }
  result = ParseToken(parsePtr, 1);
  if ( strcmp(result, ")") )
    Com_ScriptError("MatchToken: %s != %s\n", result, ")");
  return result;
}
