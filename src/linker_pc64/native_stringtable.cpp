#include <universal/q_shared.h>
#include "native_stringtable.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

bool Linker_ImportStringTable(const void *data, size_t size, const char *name, StringTable **result,
                              char *error, size_t errorSize)
{
    if (!result || (!error && errorSize))
    {
        return false;
    }
    *result = NULL;
    if (!data || size > 16 * 1024 * 1024 || !name || !name[0] || strlen(name) >= 256 || memchr(data, 0, size))
    {
        if (errorSize)
        {
            snprintf(error, errorSize, "Invalid string-table source");
        }
        return false;
    }
    char *text = (char *)malloc(size + 2);
    size_t *cells = (size_t *)malloc(1048576 * sizeof(size_t));
    unsigned int *widths = (unsigned int *)malloc(65536 * sizeof(unsigned int));
    bool valid = text && cells && widths;
    size_t cursor = 0, output = 0, cellCount = 0;
    unsigned int rows = 0, columns = 0;
    const char *source = (const char *)data;
    while (valid && cursor < size)
    {
        if (rows == 65536)
        {
            valid = false;
            break;
        }
        unsigned int width = 0;
        bool another = true;
        while (valid && another)
        {
            if (cellCount == 1048576 || width == 4096)
            {
                valid = false;
                break;
            }
            cells[cellCount++] = output;
            ++width;
            const bool quoted = cursor < size && source[cursor] == '"';
            if (quoted)
            {
                ++cursor;
                bool closed = false;
                while (cursor < size)
                {
                    const char c = source[cursor++];
                    if (c == '"')
                    {
                        if (cursor == size || source[cursor] != '"')
                        {
                            closed = true;
                            break;
                        }
                        ++cursor;
                    }
                    text[output++] = c;
                }
                valid = closed;
            }
            else
            {
                while (cursor < size && source[cursor] != ',' && source[cursor] != '\r' && source[cursor] != '\n')
                {
                    text[output++] = source[cursor++];
                }
            }
            text[output++] = 0;
            another = cursor < size && source[cursor] == ',';
            if (another)
            {
                ++cursor;
            }
            else if (cursor < size)
            {
                const char c = source[cursor++];
                valid = valid && (c == '\r' || c == '\n');
                if (c == '\r' && cursor < size && source[cursor] == '\n')
                {
                    ++cursor;
                }
            }
        }
        widths[rows++] = width;
        if (columns < width)
        {
            columns = width;
        }
    }
    const size_t count = (size_t)rows * columns;
    valid = valid && count <= 1048576;
    StringTable *table = NULL;
    if (valid)
    {
        table = (StringTable *)calloc(1, sizeof(StringTable) + count * sizeof(const char *) + strlen(name) + 1 + output + 1);
        valid = table != NULL;
    }
    if (valid)
    {
        table->rowCount = rows;
        table->columnCount = columns;
        table->values = (const char **)(table + 1);
        char *ownedName = (char *)(table->values + count);
        strcpy(ownedName, name);
        table->name = ownedName;
        char *ownedText = ownedName + strlen(name) + 1;
        memcpy(ownedText, text, output);
        size_t cell = 0;
        for (unsigned int row = 0; row < rows; ++row)
        {
            for (unsigned int column = 0; column < columns; ++column)
            {
                table->values[(size_t)row * columns + column] = column < widths[row] ?
                    ownedText + cells[cell++] : ownedText + output;
            }
        }
        *result = table;
    }
    free(widths);
    free(cells);
    free(text);
    if (!valid && errorSize)
    {
        snprintf(error, errorSize, "Malformed CSV or string-table size limit exceeded at byte %zu", cursor);
    }
    return valid;
}
