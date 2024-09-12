#ifndef vm_h
#define vm_h

#include "object.h"
#include "table.h"
#include "value.h"
#include <stdint.h>
#include <stdio.h>

#define FRAMES_MAX 64
#define STACK_MAX (FRAMES_MAX * UINT8_COUNT)

typedef struct {
    ObjClosure* closure;
    // IP to return to after the call.
    uint8_t* ip;
    // Points into stack where the function's locals start.
    Value* slots;
} CallFrame;

typedef struct {

    // Stack of function call frames.
    CallFrame frames[FRAMES_MAX];
    // Number of ongoing function calls.
    int frame_count;

    Value stack[STACK_MAX];
    Value* stack_top;
    
    size_t bytes_allocated;
    size_t next_gc;

    // Points to head of linked list of objects.
    Obj* objects;
    ObjUpvalue* open_upvalues;
    ObjString* init_string;

    int grey_count;
    int grey_capacity;
    Obj** grey_stack;

    Table globals;
    Table strings;

} VirtualMachine;

typedef enum {
    INTERPRET_OK,
    INTERPRET_COMPILE_ERROR,
    INTERPRET_RUNTIME_ERROR,
} InterpretResult;

extern VirtualMachine vm;

void init_vm();
void free_vm();
InterpretResult interpret(const char* src);
void push(Value value);
Value pop();

#endif