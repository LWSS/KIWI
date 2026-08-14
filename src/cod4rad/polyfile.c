/*
 * polyfile.c — Polygon file reader for radiosity geometry input.
 *
 * Source: polyfile.cpp (from XLSX; LST has no source annotation)
 * Reads .poly files containing material names and vertex data,
 * generates invisible opaque triangles for radiosity computation.
 */

#include "cod2rad64.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

extern void ErrorMsg(const char *fmt, ...);

/*
================
Map_ReadPolyFile

Read polygon geometry from a .poly file.
Reads alternating blocks of: material name, vertex count, N vertex lines.
Each vertex line is "( x y z u v )". Generates triangle fans from the
polygon vertices via AddInvisibleOpaqueTriangle.
================
*/
void Map_ReadPolyFile(const char *filename)
{
    char filenameBuffer[MAX_OS_PATH];
    char materialName[1024];
    char lineBuf[1024];
    float positions[16][3];     /* max 16 verts, 3 floats each (12-byte stride) */
    float uvs[16][2];           /* max 16 verts, 2 floats each (8-byte stride) */
    FILE *fp;
    int numVerts;
    MaterialDef_t *material;
    const char *ext;
    int len;
    int i;

    /* Native normally receives a short relative map name, but absolute paths
     * are valid tool input too.  Keep the path construction bounded. */
    if (strlen(filename) + strlen(GetPolyFileExtension()) >= sizeof(filenameBuffer))
        ErrorMsg("Map path is too long: %s\n", filename);
    strcpy(filenameBuffer, filename);

    /* strip extension and append poly file extension */
    StripExtension(filenameBuffer);
    ext = GetPolyFileExtension();

    /* strcat extension onto filenameBuffer (inline in binary) */
    strcat(filenameBuffer, ext);

    /* open file for reading */
    fp = fopen(filenameBuffer, "r");
    if (!fp)
        return;

    /* read first material name line */
    if (!fgets(materialName, 1024, fp))
        goto close_file;

    /* outer loop: process each polygon block */
    do
    {
        /* strip trailing whitespace from material name */
        len = (int)strlen(materialName);
        while (len > 0 && isspace((unsigned char)materialName[len - 1]))
            len--;
        materialName[len] = '\0';

        /* read vertex count */
        if (!fgets(lineBuf, 1024, fp))
            ErrorMsg("Unexpected end of file in %s\n", filenameBuffer);

        numVerts = atoi(lineBuf);

        if (numVerts < 0 || numVerts > 16)
            ErrorMsg("File %s has a polygon with %i vertices; maximum is 16\n", filenameBuffer, numVerts);

        if (numVerts > 0)
        {
            /* read vertex data lines */
            for (i = 0; i < numVerts; i++)
            {
                if (!fgets(lineBuf, 1024, fp))
                    ErrorMsg("Unexpected end of file in %s\n", filenameBuffer);

                if (sscanf(lineBuf, "( %g %g %g %g %g )\n",
                           &positions[i][0], &positions[i][1], &positions[i][2],
                           &uvs[i][0], &uvs[i][1]) != 5)
                {
                    ErrorMsg("File %s has been corrupted\n", filenameBuffer);
                }
            }
        }

        /* load material by name */
        material = (MaterialDef_t *)LoadMaterial(materialName);

        /* generate triangles (fan from vertex 0/1) */
        if (numVerts > 2)
        {
            for (i = 0; i < numVerts - 2; i++)
            {
                AddInvisibleOpaqueTriangle(material,
                    positions[0], positions[1], positions[i + 2],
                    uvs[0], uvs[1], uvs[i + 2]);
            }
        }

    } while (fgets(materialName, 1024, fp));

close_file:
    fclose(fp);
}
