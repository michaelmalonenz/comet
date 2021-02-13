#ifndef clox_compiler_h
#define clox_compiler_h

#include "objects.h"
#include "vm.h"

typedef struct Compiler Compiler;

ObjFunction* compile(const SourceFile* source, VM *thread);
SourceFile *readSourceFile(const char *path);
void markCompilerRoots(void);

#endif