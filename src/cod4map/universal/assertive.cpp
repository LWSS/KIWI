/*
assertive.c — Assert and crash handler system

Reconstructed from cod2map.exe by Rose.

Parses MSVC linker .MAP files for symbol resolution during stack walks.
Formats crash reports with stack traces and copies to clipboard.
*/

#include "cod4map.h"

AssertStringTable_t *g_assertModuleList;

const char g_assertFmtFuncFile[] = "%s\t\t...%s";
const char g_assertFmtFuncFileLine[] = "%s\t\t...%s, line %i";
const char g_assertFmtModule[] = "%s:\t";
const char g_assertFmtOOM[] = "Out of memory: filename '%s', line %d\n";
const char g_assertFmtLineNumbers[] = "%i %x:%x %i %x:%x %i %x:%x %i %x:%x\r\n";
const char g_assertFmtSymbol[] = "%x:%x %255s %llx";
const char g_assertFmtSegment[] = "%x:%x %xH %255s %255s";

char   Filename[MAX_PATH];
char   g_assertActive;
char   g_assertFmt[8] = { '%', 's', '\n', '\t', '%', 's', '\0', '\0' }; /* "%s\n\t%s" */
int  (*g_assertHandler)(int);
int    g_assertHandlerData;
char   g_assertMsgBuffer[MAXPRINTMSG];
char   g_assertPad;
char   g_assertParseFlag;
int    g_assertsDisabled;
char Str[ASSERT_LINE_BUFSIZE];

/* CoD4 0x401ED0. */
int Assertive_MapParseError(const char *message)
{
  return MessageBoxA(GetActiveWindow(), message, ".map parse error", MB_ICONERROR);
}


/*
================
SL_FreeStringList

Recursively free a linked list of AssertSymbolNodes (list A — with line info).
================
*/
void SL_FreeStringList(AssertSymbolNode_t *node)
{
  AssertSymbolNode_t *next;

  next = node->next;
  if ( next )
  {
    SL_FreeStringList(next);
    free(next);
  }
}

/*
================
SL_StringNode_ScalarDtor

Scalar destructor for AssertSymbolNode_t (list A).
Recursively destroys chain, then frees self if flags&1.
================
*/
AssertSymbolNode_t *SL_StringNode_ScalarDtor(AssertSymbolNode_t *node, char flags)
{
  if ( node->next )
    SL_StringNode_ScalarDtor(node->next, 1);
  if ( (flags & 1) != 0 )
    free(node);
  return node;
}

/*
================
SL_FreeStringListB

Recursively free a linked list of AssertSymbolNodes (list B — without line info).
================
*/
void SL_FreeStringListB(AssertSymbolNode_t *node)
{
  AssertSymbolNode_t *next;

  next = node->next;
  if ( next )
  {
    SL_FreeStringListB(next);
    free(next);
  }
}

/*
================
SL_StringNodeB_ScalarDtor

Scalar destructor for AssertSymbolNode_t (list B).
Recursively destroys chain, then frees self if flags&1.
================
*/
AssertSymbolNode_t *SL_StringNodeB_ScalarDtor(AssertSymbolNode_t *node, char flags)
{
  if ( node->next )
    SL_StringNodeB_ScalarDtor(node->next, 1);
  if ( (flags & 1) != 0 )
    free(node);
  return node;
}

/*
================
SL_FreeHashChain

Recursively free a hash chain of AssertHashNodes, including their interned strings.
================
*/
void SL_FreeHashChain(AssertHashNode_t *node)
{
  AssertHashNode_t *next;

  next = node->next;
  if ( next )
  {
    SL_FreeHashChain(next);
    free(next);
  }
  free(node->string);
}

/*
================
SL_HashNode_ScalarDtor

Scalar destructor for AssertHashNode_t.
Recursively destroys chain, frees string, then frees self if flags&1.
================
*/
AssertHashNode_t *SL_HashNode_ScalarDtor(AssertHashNode_t *node, char flags)
{
  if ( node->next )
    SL_HashNode_ScalarDtor(node->next, 1);
  free(node->string);
  if ( (flags & 1) != 0 )
    free(node);
  return node;
}

/*
================
SL_DestroyStringTable

Destroy an entire AssertStringTable_t: recursively destroy next table,
free symbol lists, clear hash buckets.
================
*/
unsigned int SL_DestroyStringTable(AssertStringTable_t *table)
{
  AssertStringTable_t *nextTable;
  AssertSymbolNode_t *symNode;
  AssertSymbolNode_t *symNext;
  AssertSymbolNode_t *symNodeA;
  AssertSymbolNode_t *symNextA;
  AssertHashNode_t *hashNode;
  AssertHashNode_t *hashNext;

  /* recursively destroy the next module in the linked list */
  nextTable = table->next;
  if ( nextTable )
  {
    SL_DestroyStringTable(nextTable);
    free(nextTable);
  }

  free(table->moduleName);

  /* free symbolsNoLines list (list B) */
  symNode = table->symbolsNoLines;
  if ( symNode )
  {
    symNext = symNode->next;
    if ( symNext )
      SL_StringNodeB_ScalarDtor(symNext, 1);
    free(symNode);
  }

  /* free symbolsWithLines list (list A) */
  symNodeA = table->symbolsWithLines;
  if ( symNodeA )
  {
    symNextA = symNodeA->next;
    if ( symNextA )
      SL_StringNode_ScalarDtor(symNextA, 1);
    free(symNodeA);
  }

  /* free all hash buckets */
  for ( table->iterator = 0; table->iterator < ASSERT_HASH_BUCKETS; table->iterator++ )
  {
    hashNode = table->hashBuckets[table->iterator];
    if ( hashNode )
    {
      hashNext = hashNode->next;
      if ( hashNext )
        SL_HashNode_ScalarDtor(hashNext, 1);
      free(hashNode->string);
      free(hashNode);
    }
  }
  return table->iterator;
}

/*
================
Assertive_GetModuleHandle

Given a module name (e.g. "foo.map"), strip extension,
try "foo.exe" then "foo.dll".
================
*/
HMODULE Assertive_GetModuleHandle(const char *moduleName)
{
  unsigned int nameLen;
  int pos;
  char ch;
  char *extPos;
  HMODULE h;
  CHAR ModuleName[MAX_PATH];

  nameLen = (unsigned int)strlen(moduleName);

  /* scan backwards for '.' extension separator (stop at path separator) */
  for ( pos = nameLen - 1; pos >= 0; --pos )
  {
    ch = moduleName[pos];
    if ( ch == '.' || ch == '/' || ch == '\\' )
      break;
  }
  if ( pos >= 0 && moduleName[pos] == '.' )
    nameLen = pos;

  /* try "name.exe" first, then "name.dll" */
  memcpy(ModuleName, moduleName, nameLen);
  extPos = &ModuleName[nameLen];
  strcpy(extPos, ".exe");
  h = GetModuleHandleA(ModuleName);
  if ( !h )
  {
    strcpy(extPos, ".dll");
    return GetModuleHandleA(ModuleName);
  }
  return h;
}

/*
================
Assertive_FormatStackFrame

Format a single stack frame into buffer. Resolves instrAddr to
module/function/file/line. Sets *hitMain=1 when WinMain/main
reached (or unknown function). Returns bytes written.
================
*/
int Assertive_FormatStackFrame(char *buffer, char *hitMain, uintptr_t instrAddr)
{
  AssertStringTable_t *module;
  AssertSymbolNode_t *bestNoLine = NULL, *bestWithLine = NULL, *iter;
  const char *moduleName = "<unknown module>";
  const char *funcName;
  char *writePos;
  char nameBuf[MAX_OS_PATH];

  *hitMain = 0;

  /* find module containing instrAddr */
  for ( module = g_assertModuleList; module; module = module->next )
    if ( instrAddr >= module->baseAddr && instrAddr < module->endAddr )
      break;

  if ( module )
  {
    moduleName = module->moduleName;

    /* find closest symbol <= instrAddr in both lists */
    for ( iter = module->symbolsNoLines; iter; iter = iter->next )
      if ( iter->address <= instrAddr && (!bestNoLine || bestNoLine->address < iter->address) )
        bestNoLine = iter;
    for ( iter = module->symbolsWithLines; iter; iter = iter->next )
      if ( iter->address <= instrAddr && (!bestWithLine || bestWithLine->address < iter->address) )
        bestWithLine = iter;
  }

  /* write "module:\t" prefix */
  writePos = buffer + sprintf(buffer, g_assertFmtModule, moduleName);

  /* resolve function name from symbol, strip MSVC decoration */
  if ( bestNoLine )
  {
    char *src = bestNoLine->file, *at;
    if ( *src == '_' || *src == '?' )
      src++;
    strcpy(nameBuf, src);
    at = strchr(nameBuf, '@');
    if ( at ) *at = 0;
    funcName = nameBuf;
    if ( !strcmp(nameBuf, "WinMain") || !strcmp(nameBuf, "main") )
      *hitMain = 1;
  }
  else
  {
    funcName = "<unknown function>";
    *hitMain = 1;
  }

  /* format: function + file/line info */
  if ( bestWithLine )
  {
    const char *base = strrchr(bestWithLine->name, '\\');
    writePos += sprintf(writePos, g_assertFmtFuncFileLine, funcName, base ? base + 1 : bestWithLine->name, bestWithLine->lineNum);
  }
  else if ( bestNoLine )
  {
    const char *base = strrchr(bestNoLine->name, '\\');
    writePos += sprintf(writePos, g_assertFmtFuncFile, funcName, base ? base + 1 : bestNoLine->name);
  }
  else
  {
    writePos += sprintf(writePos, "%s", funcName);
  }
  writePos += sprintf(writePos, "\n");
  return (int)(writePos - buffer);
}

/*
================
Assertive_CopyToClipboard

Copy the assert message buffer to the Windows clipboard (CF_TEXT).
================
*/
int Assertive_CopyToClipboard(void)
{
  HGLOBAL hMem;
  char *locked;

  if ( !OpenClipboard(GetDesktopWindow()) )
    return 0;

  EmptyClipboard();
  hMem = GlobalAlloc(GMEM_MOVEABLE, strlen(g_assertMsgBuffer) + 1);
  if ( hMem )
  {
    locked = GlobalLock(hMem);
    if ( locked )
    {
      strcpy(locked, g_assertMsgBuffer);
      GlobalUnlock(hMem);
      SetClipboardData(CF_TEXT, hMem);
    }
  }
  return CloseClipboard();
}

/*
================
Assertive_ClearFlag

Clear the "assertion in progress" flag.
================
*/
void Assertive_ClearFlag(void)
{
  g_assertActive = 0;
}

/*
================
Assertive_OutOfMemory

Fatal out-of-memory handler — prints filename/line and exits.
================
*/
void Assertive_OutOfMemory(const char *file, int line)
{
  Sys_Printf(g_assertFmtOOM, file, line);
  exit(-1);
}

/*
================
Assertive_AllocSymbol

Initialize an AssertHashNode_t: malloc+copy the name string, set hash and next pointer.
================
*/
AssertHashNode_t *Assertive_AllocSymbol(AssertHashNode_t *node, const char *name, int hashValue, AssertHashNode_t *nextNode)
{
  char *str;

  node->next = nextNode;
  str = malloc(strlen(name) + 1);
  node->string = str;
  if ( !str )
    Assertive_OutOfMemory(SRCFILE, __LINE__);
  strcpy(str, name);
  node->hashValue = hashValue;
  return node;
}

/*
================
Assertive_InternString

Intern a string in the table's hash map. Returns existing copy if found, else allocates new node.
================
*/
char *Assertive_InternString(AssertStringTable_t *table, const char *str)
{
  int hash = 0;
  int bucket;
  const char *p;
  AssertHashNode_t *node;

  #define HASH_PRIME 31337
  for ( p = str; *p; p++ )
    hash = *p + HASH_PRIME * hash;
  bucket = (unsigned char)hash;

  /* search existing entries */
  for ( node = table->hashBuckets[bucket]; node; node = node->next )
    if ( node->hashValue == hash && !strcmp(node->string, str) )
      return node->string;

  /* not found — allocate and insert */
  { AssertHashNode_t *newNode = malloc(sizeof(AssertHashNode_t));
  node = newNode ? Assertive_AllocSymbol(newNode, str, hash, table->hashBuckets[bucket]) : NULL;
  table->hashBuckets[bucket] = node;
  if ( !node )
    Assertive_OutOfMemory(SRCFILE, __LINE__);
  return node->string;
  }
  #undef HASH_PRIME
}

/*
================
Assertive_ParseMapFile

Parse an MSVC linker .MAP file, populating the AssertStringTable_t with symbols.
Sections parsed: segments, public symbols, static symbols, line numbers.
Returns 1 on success, 0 on parse error.
================
*/
char Assertive_ParseMapFile(AssertStringTable_t *table, FILE *stream, uintptr_t moduleBase)
{
  uintptr_t endAddr;
  char *lastSpace;
  AssertSymbolNode_t *newSym;
  char *openParen;
  char *closeParen;
  int fieldCount;
  AssertSymbolNode_t *lineNode;
  unsigned int sep1, sep2, sep3, sep4;
  unsigned int addrs[4];
  int lineNums[4];
  unsigned long long preferredLoadAddr;
  unsigned long long symbolAddress;
  char *internedFile;
  unsigned int segOffset;
  unsigned int segIndex;
  unsigned int symAddr;
  int i;
  char symName[MAX_OS_PATH];
  char fileName[MAX_OS_PATH];

  g_assertParseFlag = 0;

  /* find "Preferred load address" line */
  do {
    if ( !fgets(Str, ASSERT_LINE_BUFSIZE, stream) )
      goto error_unexpected_eof;
  } while ( sscanf(Str, " Preferred load address is %llx\r\n", &preferredLoadAddr) != 1 );

  table->baseAddr = moduleBase;

  /* skip 2 header lines in segments section */
  for ( i = 0; i < 2; i++ ) {
    if ( !fgets(Str, ASSERT_LINE_BUFSIZE, stream) )
      goto error_unexpected_eof;
  }

  /* parse segments -- compute endAddr from segment sizes */
  while ( fgets(Str, ASSERT_LINE_BUFSIZE, stream) ) {
    if ( !strcmp(Str, "\r\n") )
      break;
    if ( sscanf(Str, g_assertFmtSegment, &segIndex, &segOffset, &symAddr, symName, fileName) != 5 ) {
      Assertive_MapParseError("Unknown line format in the segments section");
      return 0;
    }
    if ( segIndex == 1 ) {
      endAddr = moduleBase + PE_PAGE_SIZE + segOffset + symAddr;
      if ( table->endAddr < endAddr )
        table->endAddr = endAddr;
    }
  }

  /* find "Publics by Value" section */
  while ( fgets(Str, ASSERT_LINE_BUFSIZE, stream) ) {
    if ( strstr(Str, "Publics by Value") )
      break;
  }
  if ( feof(stream) )
    goto error_unexpected_eof;

  /* skip 1 header line */
  if ( !fgets(Str, ASSERT_LINE_BUFSIZE, stream) )
    goto error_unexpected_eof;

  /* parse public symbols */
  while ( fgets(Str, ASSERT_LINE_BUFSIZE, stream) ) {
    if ( !strcmp(Str, "\r\n") )
      break;
    if ( sscanf(Str, g_assertFmtSymbol, &segIndex, &segOffset, symName, &symbolAddress) != 4 ) {
      Assertive_MapParseError("Unknown line format in the public symbols section");
      return 0;
    }
    lastSpace = strrchr(Str, ' ');
    if ( !lastSpace || sscanf(lastSpace + 1, "%255s", fileName) != 1 ) {
      Assertive_MapParseError("Couldn't parse file name in the public symbols section");
      return 0;
    }
    newSym = malloc(sizeof(AssertSymbolNode_t));
    if ( !newSym )
      Assertive_OutOfMemory(SRCFILE, __LINE__);
    newSym->next = table->symbolsNoLines;
    table->symbolsNoLines = newSym;
    newSym->name = Assertive_InternString(table, fileName);
    newSym->address = moduleBase + (uintptr_t)(symbolAddress - preferredLoadAddr);
    newSym->file = Assertive_InternString(table, symName);
  }

  /* skip 2 lines after public symbols */
  for ( i = 0; i < 2; i++ ) {
    if ( !fgets(Str, ASSERT_LINE_BUFSIZE, stream) )
      goto error_unexpected_eof;
  }

  /* read next line -- check for static symbols section */
  if ( !fgets(Str, ASSERT_LINE_BUFSIZE, stream) )
    goto error_unexpected_eof;

  if ( !strcmp(Str, " Static symbols\r\n") ) {
    /* skip 1 header line */
    if ( !fgets(Str, ASSERT_LINE_BUFSIZE, stream) )
      goto error_unexpected_eof;

    /* parse static symbols */
    while ( fgets(Str, ASSERT_LINE_BUFSIZE, stream) && Str[0] && strcmp(Str, "\r\n") ) {
      if ( sscanf(Str, g_assertFmtSymbol, &segIndex, &segOffset, symName, &symbolAddress) != 4 ) {
        Assertive_MapParseError("Unknown line format in the static symbols section");
        return 0;
      }
      lastSpace = strrchr(Str, ' ');
      if ( !lastSpace || sscanf(lastSpace + 1, "%255s", fileName) != 1 ) {
        Assertive_MapParseError("Couldn't parse file name in the static symbols section");
        return 0;
      }
      newSym = malloc(sizeof(AssertSymbolNode_t));
      if ( !newSym )
        Assertive_OutOfMemory(SRCFILE, __LINE__);
      newSym->next = table->symbolsNoLines;
      table->symbolsNoLines = newSym;
      newSym->name = Assertive_InternString(table, fileName);
      newSym->address = moduleBase + (uintptr_t)(symbolAddress - preferredLoadAddr);
      newSym->file = Assertive_InternString(table, symName);
    }
  }

  /* parse line number sections */
  for ( ;; ) {
    if ( !fgets(Str, ASSERT_LINE_BUFSIZE, stream) )
      return 1;  /* end of file = success */
    if ( strncmp(Str, "Line numbers for ", 17) ) {
      Assertive_MapParseError("Expected line number section");
      return 0;
    }

    /* extract source filename between '(' and ')' */
    openParen = strchr(Str, '(');
    if ( !openParen ) {
      Assertive_MapParseError("Couldn't find '(' in line number section");
      return 0;
    }
    closeParen = strchr(openParen, ')');
    if ( !closeParen ) {
      Assertive_MapParseError("Couldn't find ')' in line number section");
      return 0;
    }
    if ((size_t)(closeParen - openParen - 1) >= sizeof(fileName))
      return 0;
    memcpy(fileName, openParen + 1, closeParen - openParen - 1);
    fileName[closeParen - openParen - 1] = 0;
    internedFile = Assertive_InternString(table, fileName);

    /* skip 1 header line */
    if ( !fgets(Str, ASSERT_LINE_BUFSIZE, stream) )
      goto error_unexpected_eof;

    /* parse line number entries: up to 4 "linenum seg:addr" per line */
    while ( fgets(Str, ASSERT_LINE_BUFSIZE, stream) && Str[0] && strcmp(Str, "\r\n") ) {
      fieldCount = sscanf(Str, g_assertFmtLineNumbers,
        &lineNums[0], &sep1, &addrs[0],
        &lineNums[1], &sep2, &addrs[1],
        &lineNums[2], &sep3, &addrs[2],
        &lineNums[3], &sep4, &addrs[3]);
      if ( fieldCount % 3 || fieldCount / 3 <= 0 ) {
        Assertive_MapParseError("unknown line format in the line number section");
        return 0;
      }
      for ( i = 0; i < fieldCount / 3; i++ ) {
        lineNode = malloc(sizeof(AssertSymbolNode_t));
        if ( !lineNode )
          Assertive_OutOfMemory(SRCFILE, __LINE__);
        lineNode->next = table->symbolsWithLines;
        table->symbolsWithLines = lineNode;
        lineNode->name = internedFile;
        lineNode->address = moduleBase + PE_PAGE_SIZE + addrs[i];
        lineNode->lineNum = lineNums[i];
      }
    }
  }

error_unexpected_eof:
  Assertive_MapParseError("Unexpected end-of-file parsing .map file");
  return 0;
}

/*
================
Assertive_LoadMapFiles

Scan exeDir for .MAP files, parse each one, and build module symbol tables.
Skips modules already loaded (case-insensitive name check).
================
*/
void Assertive_LoadMapFiles(char *exeDir)
{
  #define ASSERT_PATH_SIZE 2048
  HANDLE findHandle;
  AssertStringTable_t *existing;
  FILE *mapFile;
  AssertStringTable_t *newTable;
  HMODULE moduleHandle;
  CHAR searchPattern[ASSERT_PATH_SIZE];
  char mapPath[ASSERT_PATH_SIZE];
  char mapName[ASSERT_PATH_SIZE];
  WIN32_FIND_DATAA FindFileData;

  if ( *exeDir )
    sprintf(searchPattern, "%s\\*.map", exeDir);
  else
    strcpy(searchPattern, "*.map");

  findHandle = FindFirstFileA(searchPattern, &FindFileData);
  if ( findHandle == INVALID_HANDLE_VALUE )
    return;

  do {
	  
    /* check if this .MAP file corresponds to a loaded module */
    moduleHandle = Assertive_GetModuleHandle(FindFileData.cFileName);
    if ( !moduleHandle )
      continue;

    strcpy(mapName, FindFileData.cFileName);

    /* check if this module is already loaded (case-insensitive) */
    for ( existing = g_assertModuleList; existing; existing = existing->next ) {
      if ( !_stricmp(existing->moduleName, mapName) )
        break;
    }
    if ( existing )
      continue;

    /* build full path to .MAP file */
    if ( *exeDir )
      sprintf(mapPath, "%s\\%s", exeDir, FindFileData.cFileName);
    else
      strcpy(mapPath, FindFileData.cFileName);

    mapFile = fopen(mapPath, "rb");
    if ( !mapFile )
      continue;

    /* allocate and zero-init a new AssertStringTable_t */
    newTable = malloc(sizeof(AssertStringTable_t));
    if ( !newTable )
      Assertive_OutOfMemory(SRCFILE, __LINE__);
    memset(newTable, 0, sizeof(AssertStringTable_t));

    /* parse the .MAP file */
    if ( Assertive_ParseMapFile(newTable, mapFile, (uintptr_t)moduleHandle) ) {
      newTable->moduleName = Assertive_InternString(newTable, mapName);
      newTable->next = g_assertModuleList;
      g_assertModuleList = newTable;
    } else {
      SL_DestroyStringTable(newTable);
      free(newTable);
    }
    fclose(mapFile);
  } while ( FindNextFileA(findHandle, &FindFileData) );

  FindClose(findHandle);
  return;
  #undef ASSERT_PATH_SIZE
}

/*
================
Assertive_WalkStack

Walk the stack via EBP chain, format each frame. Skips first skipFrames frames.
Returns total bytes written to buffer.
================
*/
int Assertive_WalkStack(char *buffer, int maxFrames, int skipFrames)
{
  void *frames[ASSERT_MAX_FRAMES];
  unsigned short frameCount;
  unsigned short i;
  char *writePos = buffer;

  if (maxFrames <= 0)
    return 0;
  if (maxFrames > ASSERT_MAX_FRAMES)
    maxFrames = ASSERT_MAX_FRAMES;
  if (skipFrames < 0)
    skipFrames = 0;
  /* Windows unwinds x64 frames from unwind metadata; there is no EBP chain. */
  frameCount = CaptureStackBackTrace((DWORD)skipFrames + 1, (DWORD)maxFrames, frames, NULL);
  for (i = 0; i < frameCount; ++i)
    writePos += sprintf(writePos, "  %p\n", frames[i]);
  return (int)(writePos - buffer);
}

/*
================
AssertMessage

Format a full assert message: expression, module, file, line + stack trace.
================
*/
int AssertMessage(const char *expr, const char *file, char *buffer, int line, int skipFrames)
{
  const char *safeFile;
  const char *safeExpr;
  int written;
  char unknown[12];

  safeFile = file;
  safeExpr = expr;
  strcpy(unknown, "<unknown>");
  if ( !file )
    safeFile = unknown;
  if ( !expr )
    safeExpr = unknown;
  if ( !GetModuleFileNameA(0, Filename, MAX_PATH) )
    strcpy(Filename, "<unknown application>");
  written = sprintf(buffer, "Expression:\n\t%s\n\nModule:\t%s\nFile:\t%s\nLine:\t%d\n\n", safeExpr, Filename, safeFile, line);
  return Assertive_WalkStack(&buffer[written], ASSERT_MAX_FRAMES, skipFrames + 1);
}

/*
================
AssertFailed

Assert handler — prints warning matching original's console format.
Original: AssertMessage formats into buffer, prints ASSERTBEGIN/message/ASSERTEND,
then shows MessageBox (OK=ExitProcess, Cancel=DebugBreak).
Recomp: print same format to console + continue (return 2 = ignore).
Can't exit because benign assertions fire during startup (dvar.cpp:1799).
a4=severity (0=assert,1=sanity,2+=internal), a5=unused.
================
*/
static char g_assertModulePath[260];
static char g_assertReentryGuard;
static int g_assertSeverity;
static void (*g_assertCallback)(const char *msg);

int AssertFailed(const char *expr, const char *file, int line, int severity, int a5)
{
  const char *caption;
  int result;

  if ( g_assertReentryGuard )
  {
    /* recursive assert */
    if ( g_assertCallback )
      g_assertCallback(g_assertMsgBuffer);

    if ( g_assertSeverity == 0 )
      caption = "ASSERTION FAILURE... (this text is on the title bar of the popup)";
    else if ( g_assertSeverity == 1 )
      caption = "SANITY CHECK FAILURE... (this text is on the title bar of the popup)";
    else
      caption = "INTERNAL ERROR";

    MessageBoxA(NULL, g_assertMsgBuffer, caption, MB_OKCANCEL | MB_ICONHAND | MB_TASKMODAL | MB_SETFOREGROUND);

    sprintf(g_assertMsgBuffer, "Expression:\n\t%s\n\nModule:\t%s\nFile:\t%s\nLine:\t%d\n\n", expr, g_assertModulePath, file, line);
    Com_Printf("ASSERTBEGIN - ( Recursive assert )-----------------------------------------------\n");
    Com_Printf("%s", g_assertMsgBuffer);
    Com_Printf("ASSERTEND - ( Recursive assert ) ------------------------------------------------\n");
    exit(-1);
  }

  g_assertSeverity = severity;
  g_assertReentryGuard = 1;

  if ( !GetModuleFileNameA(NULL, g_assertModulePath, sizeof(g_assertModulePath)) )
    strcpy(g_assertModulePath, "<unknown application>");

  sprintf(g_assertMsgBuffer, "Expression:\n\t%s\n\nModule:\t%s\nFile:\t%s\nLine:\t%d\n\n", expr, g_assertModulePath, file, line);

  Com_Printf("ASSERTBEGIN -----------------------------------------------------------------------\n");
  Com_Printf("%s", g_assertMsgBuffer);
  Com_Printf("ASSERTEND -------------------------------------------------------------------------\n");

  if ( g_assertCallback )
    g_assertCallback(g_assertMsgBuffer);

  if ( severity == 0 )
    caption = "ASSERTION FAILURE... (this text is on the title bar of the popup)";
  else if ( severity == 1 )
    caption = "SANITY CHECK FAILURE... (this text is on the title bar of the popup)";
  else
    caption = "INTERNAL ERROR";

  result = MessageBoxA(NULL, g_assertMsgBuffer, caption, MB_OKCANCEL | MB_ICONHAND | MB_TASKMODAL | MB_SETFOREGROUND);

  if ( result == IDOK )
    ExitProcess(-1);

  /* Cancel → return 0 → caller does DebugBreak() */
  g_assertReentryGuard = 0;
  return 0;
}
