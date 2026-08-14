/*
 * com_shared.c — shared wildcard filter routines.
 *
 * Source: ..\src\universal\com_shared.cpp
 */

#include "cod2rad64.h"
#include <string.h>
#include <ctype.h>

/*
================
Com_Filter

Wildcard match. Returns 1 if 'name' matches 'filter' (supports '*',
'?', and '[abc]'/'[a-z]' character classes), else 0.

After a '*', the binary scans 'name' for the literal-prefix chunk
that follows the star and uses a full-string compare (not a prefix
compare), so it only matches the chunk as a suffix of 'name'. This
is faithful to the compiled behaviour — not a Quake3-style substring
search.
================
*/
int Com_Filter(const char *filter, const char *name, int casesensitive)
{
    char buf[1024];
    int (*cmp)(const char *, const char *);
    const char *s;
    int i;
    int buflen;
    int maxOffset;
    int found;

    while (*filter)
    {
        if (*filter == '*')
        {
            filter++;
            i = 0;
            while (*filter && *filter != '*' && *filter != '?')
            {
                buf[i++] = *filter;
                filter++;
            }
            buf[i] = 0;

            buflen = (int)strlen(buf);
            if (buflen == 0)
                continue;

            maxOffset = (int)strlen(name) - buflen;
            if (maxOffset < 0)
                return 0;

            cmp = casesensitive ? I_strcmp : I_stricmp;

            s = name;
            i = 0;
            while (cmp(s, buf) != 0)
            {
                i++;
                s++;
                if (i > maxOffset)
                    return 0;
            }

            if (s == NULL)
                return 0;
            name = s + strlen(buf);
        }
        else if (*filter == '?')
        {
            filter++;
            name++;
        }
        else if (*filter == '[')
        {
            filter++;
            if (*filter == '[')
            {
                /* [[ = escaped literal '[' — consume one and let the
                   outer loop re-enter the '[' branch */
                continue;
            }
            if (*filter == 0)
                return 0;

            found = 0;
            while (*filter && !found)
            {
                if (*filter == ']' && filter[1] != ']')
                    break;

                if (filter[1] == '-' && filter[2] != 0 &&
                    (filter[2] != ']' || filter[3] == ']'))
                {
                    if (casesensitive)
                    {
                        if ((unsigned char)*name >= (unsigned char)filter[0] &&
                            (unsigned char)*name <= (unsigned char)filter[2])
                        {
                            found = 1;
                        }
                    }
                    else
                    {
                        int lo = toupper((unsigned char)filter[0]);
                        int hi = toupper((unsigned char)filter[2]);
                        int nc = toupper((unsigned char)*name);
                        if (nc >= lo && nc <= hi)
                            found = 1;
                    }
                    filter += 3;
                }
                else
                {
                    if (casesensitive)
                    {
                        if (*filter == *name)
                            found = 1;
                    }
                    else
                    {
                        if (toupper((unsigned char)*filter) == toupper((unsigned char)*name))
                            found = 1;
                    }
                    filter++;
                }
            }

            if (!found)
                return 0;

            while (*filter)
            {
                if (*filter == ']' && filter[1] != ']')
                    break;
                filter++;
            }

            filter++;
            name++;
        }
        else
        {
            if (casesensitive)
            {
                if (*filter != *name)
                    return 0;
            }
            else
            {
                if (toupper((unsigned char)*filter) != toupper((unsigned char)*name))
                    return 0;
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

Copies filter and name into fixed 64-byte stack buffers, converting
'\\' and ':' to '/' as it goes, then calls Com_Filter. The binary
caps each copy at 63 source chars and nul-terminates at offset equal
to the char count; it does not bounds-check the input beyond the cap.
================
*/
int Com_FilterPath(const char *filter, const char *name, int casesensitive)
{
    char filterBuf[MAX_QPATH];
    char nameBuf[MAX_QPATH];
    int i;

    for (i = 0; i < MAX_QPATH - 1 && filter[i]; i++)
    {
        if (filter[i] == '\\' || filter[i] == ':')
            filterBuf[i] = '/';
        else
            filterBuf[i] = filter[i];
    }
    filterBuf[i] = 0;

    for (i = 0; i < MAX_QPATH - 1 && name[i]; i++)
    {
        if (name[i] == '\\' || name[i] == ':')
            nameBuf[i] = '/';
        else
            nameBuf[i] = name[i];
    }
    nameBuf[i] = 0;

    return Com_Filter(filterBuf, nameBuf, casesensitive);
}
