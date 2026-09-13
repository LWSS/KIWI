#pragma once
#include <stddef.h>
struct XModel;
struct XModelPieces;
typedef XModel *(*LinkerResolvePieceModel)(const char *name, void *context);
bool Linker_ImportModelPieces(const void *data, size_t size, const char *name, LinkerResolvePieceModel resolve,
                              void *context, XModelPieces **pieces, char *error, size_t errorSize);
void Linker_FreeModelPieces(XModelPieces *pieces);
