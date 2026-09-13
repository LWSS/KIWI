#pragma once
#include <stdarg.h>

// Called before Com_Error enters its global error state. Only traps an active native load.
void DB64_CaptureLoadError(const char *format, va_list arguments);
void DB_CloseNativeFile();
