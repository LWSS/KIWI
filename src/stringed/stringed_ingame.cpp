#include <universal/q_shared.h>
#include "stringed_ingame.h"
#include <universal/assertive.h>
#include <universal/com_memory.h>
#include <database64/database.h>
#include <universal/com_files.h>

CStringEdPackage *TheStringPackage;

int giFilesFound;
char sTemp[64];

const char *__cdecl SE_GetString(const char *psPackageAndStringReference)
{
    if (IsFastFileLoad())
        return SE_GetString_FastFile(psPackageAndStringReference);
    else
        return (const char *)SE_GetString_LoadObj(psPackageAndStringReference);
}

const char *__cdecl SE_GetString_FastFile(const char *psPackageAndStringReference)
{
    LocalizeEntry *localize; // [esp+8h] [ebp-4h]

    localize = DB_FindXAssetHeader(ASSET_TYPE_LOCALIZE_ENTRY, psPackageAndStringReference).localize;
    if (localize)
        return localize->value;
    else
        return 0;
}

// filename is local here, eg:	"strings/german/obj.str"
//
// return is either NULL for good else error message to display...
//
char *__cdecl SE_Load(char *psFileName, bool forceEnglish)
{
    char *psParsePos; // [esp+14h] [ebp-4014h] BYREF
    char psDest[16388]; // [esp+20h] [ebp-4010h] BYREF
    const char *errorMsg; // [esp+4020h] [ebp-8h]
    uint8_t *psLoadedFile; // [esp+4024h] [ebp-4h]

    errorMsg = 0;
    psLoadedFile = SE_LoadFileData(psFileName);

    if (!psLoadedFile)
        return va("Unable to load \"%s\"!", psFileName);

    // now parse the data...
    //
    psParsePos = (char *)psLoadedFile;
    TheStringPackage->SetupNewFileParse(psFileName);

    while (!errorMsg && TheStringPackage->ReadLine(&psParsePos, psDest, sizeof(psDest)))
    {
        if (&psDest[strlen(psDest) + 1] != &psDest[1])
            errorMsg = TheStringPackage->ParseLine(psDest, forceEnglish);
    }

    if (errorMsg)
        errorMsg = va("%s in %s", errorMsg, psFileName);

    SE_FreeFileDataAfterLoad(psLoadedFile);

    if (!errorMsg && !TheStringPackage->m_bEndMarkerFound_ParseOnly)
        return va("Truncated file, failed to find \"%s\" at file end!", "ENDMARKER");

    return (char *)errorMsg;
}

const char *__cdecl SE_GetString_LoadObj(const char *psPackageAndStringReference)
{
    auto itEntry = TheStringPackage->m_StringEntries.find(psPackageAndStringReference);

    if (itEntry == TheStringPackage->m_StringEntries.end())
    {
        // should never get here, but fall back anyway... (except we DO use this to see if there's a debug-friendly key bind, which may not exist)
        //
        return NULL;
    }

    return itEntry->second.c_str();
}

void __cdecl SE_NewLanguage()
{
    iassert(TheStringPackage);

    TheStringPackage->Clear();
}

// these two functions aren't needed other than to make Quake-type games happy and/or stop memory managers
//	complaining about leaks if they report them before the global StringEd package object calls it's own dtor.
//
// but here they are for completeness's sake I guess...
//
void __cdecl SE_Init()
{
    iassert(!TheStringPackage);

    TheStringPackage = new CStringEdPackage();

    // LWSS: moved to operator new/delete overrides within class
    //void* v2 = Z_Malloc(120, "CStringEdPackage", 33);
    //if (v2)
    //{
    //    //CStringEdPackage::CStringEdPackage(v2);
    //    TheStringPackage = new (v2) CStringEdPackage();
    //}
    //else
    //{
    //    TheStringPackage = NULL;
    //}

    TheStringPackage->Clear();
}

void __cdecl SE_ShutDown()
{
    if (TheStringPackage)
    {
        TheStringPackage->Clear();
        // LWSS: moved to operator new/delete overrides within class
        //if (TheStringPackage)
        //{
        //    CStringEdPackage::~CStringEdPackage(TheStringPackage);
        //    Z_Free((char *)v0, 33);
        //}
        delete TheStringPackage;
        TheStringPackage = 0;
    }
}

// returns error message else NULL for ok.
//
// Any errors that result from this should probably be treated as game-fatal, since an asset file is fuxored.
//
char *__cdecl SE_LoadLanguage(bool forceEnglish)
{
    char *psErrorMessage; // [esp+30h] [ebp-28h]
    std::string strResults;
    const char *p; // [esp+54h] [ebp-4h]

    psErrorMessage = 0;
    strResults.clear();
    SE_NewLanguage();
    SE_BuildFileList("localizedstrings", &strResults);
    while (1)
    {
        p = SE_GetFoundFile(&strResults);
        if (!p || psErrorMessage)
            break;
        psErrorMessage = SE_Load((char *)p, forceEnglish);
    }
    return psErrorMessage;
}

char *__cdecl SE_GetFoundFile(std::string *strResult)
{
    _BYTE *v2; // eax

    if (!strResult->size())
    {
        return NULL;
    }

    strncpy(sTemp, strResult->data(), 63);
    sTemp[63] = 0;
    v2 = (_BYTE *)strchr(sTemp, ';');
    if (v2)
    {
        *v2 = 0;
        strResult->erase(0, (char*)v2 - sTemp + 1);
    }
    else
    {
        // no semicolon found, probably last entry? (though i think even those have them on, oh well)
        //
        strResult->erase(0, std::string::npos);
    }
    return sTemp;
}

// this just gets the binary of the file into memory, so I can parse it. Called by main SGE loader
//
//  returns either char * of loaded file, else NULL for failed-to-open...
//
uint8_t *__cdecl SE_LoadFileData(const char *psFileName)
{
    int len; // [esp+0h] [ebp-8h]
    uint8_t *pvLoadedData; // [esp+4h] [ebp-4h] BYREF

    // local filename, so prepend the base dir etc according to game and load it however (from PAK?)
    //
    len = FS_ReadFile(psFileName, (void **)&pvLoadedData);
    return len <= 0 ? 0 : pvLoadedData;
}

// called by main SGE code after loaded data has been parsedinto internal structures...
//
void __cdecl SE_FreeFileDataAfterLoad(uint8_t *psLoadedFile)
{
    iassert(psLoadedFile);
    FS_FreeFile((char *)psLoadedFile);
}

// replace this with a call to whatever your own code equivalent is.
//
// expected result is a ';'-delineated string (including last one) containing file-list search results
//
int __cdecl SE_BuildFileList(
    const char *psStartDir,
    std::string *strResults)
{
    giFilesFound = 0;
    strResults->assign("");
    SE_R_ListFiles("str", psStartDir, strResults);
    return giFilesFound;
}

// quake-style method of doing things since their file-list code doesn't have a 'recursive' flag...
//
void __cdecl SE_R_ListFiles(
    const char *psExtension,
    const char *psDir,
    std::string *strResults)
{
    char sFilename[64]; // [esp+54h] [ebp-98h] BYREF
    char sDirName[64]; // [esp+94h] [ebp-58h] BYREF
    int numdirs; // [esp+D8h] [ebp-14h] BYREF
    int numSysFiles; // [esp+DCh] [ebp-10h] BYREF
    int i; // [esp+E0h] [ebp-Ch]
    const char **sysFiles; // [esp+E4h] [ebp-8h]
    const char **dirFiles; // [esp+E8h] [ebp-4h]

    dirFiles = FS_ListFiles(psDir, "/", FS_LIST_PURE_ONLY, &numdirs);
    for (i = 0; i < numdirs; ++i)
    {
        if (*dirFiles[i])
        { // skip blanks, plus ".", ".." etc
            if (*dirFiles[i] != 46)
            {
                snprintf(sDirName, ARRAYSIZE(sDirName), "%s/%s", psDir, dirFiles[i]);
                SE_R_ListFiles(psExtension, sDirName, strResults);
            }
        }
    }
    sysFiles = FS_ListFiles(psDir, psExtension, FS_LIST_PURE_ONLY, &numSysFiles);
    for (i = 0; i < numSysFiles; ++i)
    {
        snprintf(sFilename, ARRAYSIZE(sFilename), "%s/%s", psDir, sysFiles[i]);
        strResults->append(sFilename);
        strResults->append(1, ';');
        ++giFilesFound;
    }
    FS_FreeFileList(sysFiles);
    FS_FreeFileList(dirFiles);
}

