#ifndef compiler_h
#define compiler_h

#include "object.h"

ObjFunction* compile(const char* src);
void mark_compiler_roots();

#endif