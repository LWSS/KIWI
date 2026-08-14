/*
 * q_parse.c — Token parsing for script/config files.
 */

#include "cod2rad64.h"

#define MAX_PARSE_INFO 16
#define MAX_TOKEN_LEN 1024
#define PARSE_SESSION_SIZE 1136

/* parse session state stored in global array */
static char g_parseSessions[MAX_PARSE_INFO + 1][PARSE_SESSION_SIZE];
static int g_parseDepth;
static char *g_prevParsePtr;
static char *g_savedParsePtr;

/* offsets within a parse session (1136 bytes):
 *   0..1023   = token buffer (1024 bytes)
 *   1024      = line count (int at offset 0x400)
 *   1028      = ungot flag (byte at offset 0x404)
 *   1029      = crossline allowed (byte at offset 0x405)
 *   1030-1032 = flags
 *   1040      = saved data_p (qword at offset 0x410, index 130)
 *   1048      = saved data_p2 (qword at offset 0x418, index 131)
 *   1056      = saved line (int at offset 0x420, index 264)
 *   1064      = saved ptr (qword at offset 0x428, index 133)
 *   1072..1135 = session name (64 bytes at offset 0x430, index 134)
 */

/*
 * ParseSession — parser state, ~1100 bytes.
 * Offsets verified from LST macro access patterns.
 */
typedef struct ParseSession
{
    char token[1024];           /* +0x000: current token buffer */
    int line;                   /* +0x400: current line number */
    char ungot;                 /* +0x404: token unget flag */
    char crossline;             /* +0x405: cross-line flag */
    char keepStringQuotes;      /* +0x406 */
    char csv;                   /* +0x407 */
    char negativeNumbers;       /* +0x408 */
    unsigned char pad409[7];    /* +0x409: padding */
    const char *errorPrefix;    /* +0x410 */
    const char *warningPrefix;  /* +0x418 */
    int savedLine;              /* +0x420: saved line number */
    unsigned char pad424[4];    /* +0x424: padding */
    char *savedPtr;             /* +0x428: saved parse pointer */
    char name[1];               /* +0x430: session name (variable length) */
} ParseSession;

#define PS_TOKEN(ps)            (((ParseSession *)(ps))->token)
#define PS_LINE(ps)             (((ParseSession *)(ps))->line)
#define PS_UNGOT(ps)            (((ParseSession *)(ps))->ungot)
#define PS_CROSSLINE(ps)        (((ParseSession *)(ps))->crossline)
#define PS_KEEPSTRINGQUOTES(ps) (((ParseSession *)(ps))->keepStringQuotes)
#define PS_CSV(ps)              (((ParseSession *)(ps))->csv)
#define PS_NEGATIVENUMBERS(ps)  (((ParseSession *)(ps))->negativeNumbers)
#define PS_ERRORPREFIX(ps)      (((ParseSession *)(ps))->errorPrefix)
#define PS_WARNINGPREFIX(ps)    (((ParseSession *)(ps))->warningPrefix)
#define PS_SAVEDPTR(ps)         (((ParseSession *)(ps))->savedPtr)
#define PS_SAVEDLINE(ps)        (((ParseSession *)(ps))->savedLine)
#define PS_NAME(ps)             (((ParseSession *)(ps))->name)

static char *g_currentSession(void)
{
    return g_parseSessions[g_parseDepth];
}

/*
================
Com_BeginParseSession

Begin a new parsing session. Max 15 nested.
================
*/
void Com_BeginParseSession(const char *name)
{
    int idx;
    char *ps;

    idx = g_parseDepth;
    if (g_parseDepth == MAX_PARSE_INFO - 1)
    {
        int i;
        Com_Printf("Already parsing:\n");
        for (i = 0; i < g_parseDepth; i++)
            Com_Printf("%i. %s\n", i, PS_NAME(g_parseSessions[i]));
        Com_Error(0, "Com_BeginParseSession: session overflow trying to parse %s\n", name);
    }
    g_parseDepth++;
    ps = g_parseSessions[idx + 1];
    PS_ERRORPREFIX(ps) = "";
    PS_WARNINGPREFIX(ps) = "";
    PS_LINE(ps) = 1;
    PS_UNGOT(ps) = 0;
    PS_CROSSLINE(ps) = 1;
    PS_KEEPSTRINGQUOTES(ps) = 0;
    PS_CSV(ps) = 0;
    PS_NEGATIVENUMBERS(ps) = 0;
    PS_SAVEDLINE(ps) = 0;
    PS_SAVEDPTR(ps) = NULL;
    I_strncpyz(PS_NAME(ps), name, 64);
}

/*
================
Com_ParseInternal

Main token parser. Handles whitespace, comments (// and block), quoted strings
with escape sequences, numbers with exponents, identifiers, and punctuation.
Uses a table of multi-character punctuation tokens (s_punctuationTable).
================
*/
char *Com_ParseInternal(char **data_p, int crossline)
{
    char *ps = g_currentSession();
    char *data;
    int len;
    char c;
    int newlines;

    static char s_assertDisable_Com_ParseInternal;
    Assert(data_p, s_assertDisable_Com_ParseInternal);

    data = *data_p;
    len = 0;
    PS_TOKEN(ps)[0] = 0;

    if (!data)
    {
        *data_p = NULL;
        return PS_TOKEN(ps);
    }

    PS_SAVEDLINE(ps) = PS_LINE(ps);
    PS_SAVEDPTR(ps) = *data_p;

    if (PS_CSV(ps))
        return Com_ParseCSV(data_p, crossline);

    newlines = 0;

skipwhite:
    /* skip whitespace */
    c = *data;
    while (c <= ' ')
    {
        if (!c)
        {
            *data_p = NULL;
            return PS_TOKEN(ps);
        }
        if (c == '\n')
        {
            PS_LINE(ps)++;
            newlines = 1;
        }
        c = *++data;
    }

    /* if crossed a newline and not allowed, return empty */
    if (newlines && !crossline)
        return PS_TOKEN(ps);

    c = *data;

    /* skip // comments */
    if (c == '/' && data[1] == '/')
    {
        while (*data && *data != '\n')
            data++;
        goto skipwhite;
    }

    /* skip /* comments */
    if (c == '/' && data[1] == '*')
    {
        while (*data != '*' || data[1] != '/')
        {
            if (*data == '\n')
                PS_LINE(ps)++;
            if (!*++data)
                goto skipwhite;
        }
        if (*data)
            data += 2;
        goto skipwhite;
    }

    /* save parse position */
    g_savedParsePtr = g_prevParsePtr;
    g_prevParsePtr = data;

    /* quoted string */
    if (c == '"')
    {
        if (PS_KEEPSTRINGQUOTES(ps))
        {
            PS_TOKEN(ps)[0] = '"';
            len = 1;
        }
        data++;
        while (1)
        {
            c = *data++;
            if (c == '\\')
            {
                if (*data == '"' || *data == '\\')
                {
                    c = *data++;
                    goto store_quoted;
                }
            }
            else if (c == '"' || !c)
            {
                if (PS_KEEPSTRINGQUOTES(ps))
                    PS_TOKEN(ps)[len++] = '"';
                PS_TOKEN(ps)[len] = 0;
                *data_p = data;
                return PS_TOKEN(ps);
            }
            if (*data == '\n')
                PS_LINE(ps)++;
store_quoted:
            if (len < MAX_TOKEN_LEN - 1)
                PS_TOKEN(ps)[len++] = c;
        }
    }

    /* if crossline parse mode (byte 1029), parse everything until whitespace */
    if (PS_CROSSLINE(ps))
    {
        while (c > ' ')
        {
            if (len < MAX_TOKEN_LEN - 1)
                PS_TOKEN(ps)[len++] = c;
            c = *++data;
        }
        goto done;
    }

    /* number: starts with digit, or '-' followed by digit, or '.' followed by digit */
    if (c >= '0' && c <= '9')
        goto parse_number;
    if (PS_NEGATIVENUMBERS(ps) && c == '-')
    {
        char nc = data[1];
        if (nc >= '0' && nc <= '9')
            goto parse_number;
        goto parse_punctuation;
    }
    if (c == '.')
    {
        char nc = data[1];
        if (nc >= '0' && nc <= '9')
        {
parse_number:
            do
            {
                if (len < MAX_TOKEN_LEN - 1)
                    PS_TOKEN(ps)[len++] = c;
                c = *++data;
            } while ((c >= '0' && c <= '9') || c == '.');

            /* exponent */
            if (c == 'e' || c == 'E')
            {
                if (len < MAX_TOKEN_LEN - 1)
                    PS_TOKEN(ps)[len++] = c;
                c = *++data;
                if (c == '-' || c == '+')
                {
                    if (len < MAX_TOKEN_LEN - 1)
                        PS_TOKEN(ps)[len++] = c;
                    c = *++data;
                }
                do
                {
                    if (len < MAX_TOKEN_LEN - 1)
                        PS_TOKEN(ps)[len++] = c;
                    c = *++data;
                } while (c >= '0' && c <= '9');
            }
            goto done;
        }
        goto parse_punctuation;
    }

    /* identifier: starts with alpha or _ or / or \ */
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == '/' || c == '\\')
    {
        do
        {
            if (len < MAX_TOKEN_LEN - 1)
                PS_TOKEN(ps)[len++] = c;
            c = *++data;
        } while ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || (c >= '0' && c <= '9'));
        goto done;
    }

parse_punctuation:
    {
        /* check multi-char punctuation table */
        static const char *s_punctuationTable[] = { "+=", "-=", "*=", "/=", "&=", "|=", "++", "--", "&&", "||", "<=", ">=", "==", "!=", NULL };
        const char **table = (const char **)s_punctuationTable;
        const char *punct;
        int plen, matched;

        for (; (punct = *table) != NULL; table++)
        {
            plen = (int)strlen(punct);
            matched = 0;
            while (matched < plen && data[matched] == punct[matched])
                matched++;
            if (matched == plen)
            {
                memmove(PS_TOKEN(ps), punct, plen);
                PS_TOKEN(ps)[plen] = 0;
                *data_p = data + plen;
                return PS_TOKEN(ps);
            }
        }

        /* single character token */
        PS_TOKEN(ps)[0] = *data;
        PS_TOKEN(ps)[1] = 0;
        *data_p = data + 1;
        return PS_TOKEN(ps);
    }

done:
    if (len == 1024)
        len = 0;
    PS_TOKEN(ps)[len] = 0;
    *data_p = data;
    return PS_TOKEN(ps);
}

/*
================
Com_Parse

Parse next token, allowing cross-line. Handles unget.
================
*/
char *Com_Parse(char **data_p)
{
    char *ps = g_currentSession();

    if (PS_UNGOT(ps))
    {
        char *saved = PS_SAVEDPTR(ps);
        PS_UNGOT(ps) = 0;
        *data_p = saved;
        PS_LINE(ps) = PS_SAVEDLINE(ps);
    }
    return Com_ParseInternal(data_p, 1);
}

/*
================
Com_ParseOnLine

Parse next token without crossing line boundaries. Handles unget.
================
*/
char *Com_ParseOnLine(char **data_p)
{
    char *ps = g_currentSession();

    if (PS_UNGOT(ps))
    {
        int wasCrossline = PS_CROSSLINE(ps);
        PS_UNGOT(ps) = 0;
        if (!wasCrossline)
            return PS_TOKEN(ps);
        *data_p = PS_SAVEDPTR(ps);
        PS_LINE(ps) = PS_SAVEDLINE(ps);
    }
    return Com_ParseInternal(data_p, 0);
}

/*
================
Com_ParseCSV

Parse a CSV-style token delimited by comma, newline, or quoted strings.
================
*/
char *Com_ParseCSV(char **data_p, int crossline)
{
    char *data;
    int len;
    char *token;
    char c;

    data = *data_p;
    len = 0;
    token = PS_TOKEN(g_currentSession());
    *token = 0;

    if (crossline)
    {
        while (*data == '\r' || *data == '\n')
            data++;
    }
    else
    {
        if (*data == '\r' || *data == '\n')
            return token;
    }

    g_savedParsePtr = g_prevParsePtr;
    g_prevParsePtr = data;

    c = *data;
    if (!c)
        goto done_null;

    while (c != ',' && c != '\n')
    {
        if (c != '\r')
        {
            if (c == '"')
            {
                for (;;)
                {
                    data++;
                    while (*data == '"')
                    {
                        if (data[1] != '"')
                            goto advance;
                        if (len < MAX_TOKEN_LEN - 1)
                            token[len++] = '"';
                        data += 2;
                    }
                    if (len < MAX_TOKEN_LEN - 1)
                        token[len++] = *data;
                }
            }
            if (len < MAX_TOKEN_LEN - 1)
                token[len++] = c;
        }
advance:
        c = *++data;
        if (!c)
            goto done_null;
    }

    if (!*data)
    {
done_null:
        *data_p = NULL;
        token[len] = 0;
        return token;
    }
    if (*data != '\n')
        data++;
    *data_p = data;
    token[len] = 0;
    return token;
}
