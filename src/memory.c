#include <stdlib.h>

#include "object.h"
#include "memory.h"
#include "compiler.h"
#include "value.h"
#include "vm.h"

#ifdef DEBUG_LOG_GC
    #include <stdio.h>
    #include "debug.h"
#endif

#define GC_HEAP_GROW_FACTOR 2

void* reallocate(void* pointer, size_t old_size, size_t new_size) {
    vm.bytes_allocated += new_size - old_size;

    if (new_size > old_size) {
        #ifdef DEBUG_STRESS_GC
            collect_garbage();
        #endif
        if (vm.bytes_allocated > vm.next_gc) {
            collect_garbage();
        }
    }

    if (new_size == 0) {
        free(pointer);
        return NULL;
    }

    void* result = realloc(pointer, new_size);
    if (result == NULL) exit(1);
    return result;
}

void mark_object(Obj *object) {
    if (object == NULL) return;
    // Prevents infinite loops.
    if (object->is_marked) return;

    #ifdef DEBUG_LOG_GC
        printf("%p mark ", (void*)object);
        print_value(OBJ_VAL(object));
        printf("\n");
    #endif

    if (vm.grey_capacity < vm.grey_count + 1) {
        vm.grey_capacity = GROW_CAPACITY(vm.grey_capacity);
        vm.grey_stack = realloc(vm.grey_stack, sizeof(Obj*) * vm.grey_capacity);
        // If we run out of memory, we just exit the program.
        if (vm.grey_stack == NULL) exit(1);
    }

    object->is_marked = true;
}

void mark_value(Value value) {
    if (IS_OBJ(value)) mark_object(AS_OBJ(value));
}

static void mark_array(ValueArray* array) {
    for (int i = 0; i < array->count; i++) {
        mark_value(array->values[i]);
    }
}

// Process grey objects -> black.
static void blacken_object(Obj* object) {

    #ifdef DEBUG_LOG_GC
        printf("%p blacken ", (void*)object);
        print_value(OBJ_VAL(object));
        printf("\n");
    #endif

    switch (object->type) {
        case OBJ_BOUND_METHOD: {
            ObjBoundMethod* bound = (ObjBoundMethod*)object;
            mark_value(bound->receiver);
            mark_object((Obj*)bound->method);
            break;
        }
        case OBJ_CLASS: {
            ObjClass* klass = (ObjClass*)object;
            mark_object((Obj*)klass->name);
            mark_table(&klass->methods);
            break;
        }
        case OBJ_CLOSURE:
            ObjClosure* closure = (ObjClosure*)object;
            mark_object((Obj*)closure->function);
            for (int i = 0; i < closure->upvalue_count; i++) {
                mark_object((Obj*)closure->upvalues[i]);
            }
            break;
        case OBJ_UPVALUE:
            mark_value(((ObjUpvalue*)object)->closed);
            break;
        case OBJ_FUNCTION:
            ObjFunction* function = (ObjFunction*)object;
            mark_object((Obj*)function->name);
            // Mark the all function constants.
            mark_array(&function->chunk.constants);
            break;
        case OBJ_INSTANCE: {
            ObjInstance* instance = (ObjInstance*)object;
            mark_object((Obj*)instance->klass);
            mark_table(&instance->fields);
            break;
        }
        // Both strings and native have no outgoing references to other objects.
        case OBJ_NATIVE:
        case OBJ_STRING:
            break;
    }
}

static void free_object(Obj* object) {

    #ifdef DEBUG_LOG_GC
        printf("%p free type %d\n", (void*)object, object->type);
    #endif

    switch (object->type) {
        case OBJ_BOUND_METHOD: {
            FREE(ObjBoundMethod, object);
            break;
        }
        case OBJ_CLASS: {
            ObjClass* klass = (ObjClass*)object;
            free_table(&klass->methods);
            FREE(ObjClass, object);
            break;
        }
        case OBJ_CLOSURE: {
            // Free the closure, but not the function,
            // which is shared with other closures and still in use.
            FREE(ObjClosure, object);
            break;
        }
        case OBJ_INSTANCE: {
            ObjInstance* instance = (ObjInstance*)object;
            free_table(&instance->fields);
            FREE(ObjInstance, object);
            break;
        }
        case OBJ_FUNCTION: {
            ObjFunction* function = (ObjFunction*)object;
            free_chunk(&function->chunk);
            FREE(ObjFunction, object);
            break;
        }
        case OBJ_NATIVE: {
            FREE(ObjNative, object);
            break;
        }
        case OBJ_STRING: {
            ObjString* string = (ObjString*)object;
            FREE_ARRAY(char, string->chars, string->length + 1);
            FREE(ObjString, object);
            break;
        }
        case OBJ_UPVALUE: {
            ObjClosure* closure = (ObjClosure*)object;
            FREE_ARRAY(ObjUpvalue*, closure->upvalues, closure->upvalue_count);
            FREE(ObjUpvalue, object);
            break;
        }
    }
}

static void mark_roots() {
    
    // Mark local variables and temorparies on stack.
    for (Value* slot = vm.stack; slot < vm.stack_top; slot++) {
        mark_value(*slot);
    }

    // Mark closures referenced by callframes.
    for (int i = 0; i < vm.frame_count; i++) {
        mark_object((Obj*)vm.frames[i].closure);
    }

    // Mark open upvalues.
    for (ObjUpvalue* upvalue = vm.open_upvalues; upvalue != NULL; upvalue = upvalue->next) {
        mark_object((Obj*)upvalue);
    }

    // Mark globals.
    mark_table(&vm.globals);

    // Mark the compiler.
    mark_compiler_roots();
    mark_object((Obj*)vm.init_string);
}

static void trace_references() {
    while (vm.grey_count > 0) {
        Obj* object = vm.grey_stack[--vm.grey_count];
        blacken_object(object);
    } 
}

static void sweep() {
    // Linked list removal stuff.
    Obj* previous = NULL;
    Obj* object = vm.objects;
    while (object != NULL) {
        if (object->is_marked) {
            // Unmark the object for the next GC cycle.
            object->is_marked = false;
            previous = object;
            object = object->next;
        } else {
            Obj* unreached = object;
            object = object->next;
            if (previous != NULL) {
                previous->next = object;
            } else {
                vm.objects = object;
            }
            free_object(unreached);
        }
    }
}

// Mark and sweep garbage collection.
void collect_garbage() {
    #ifdef DEBUG_LOG_GC
        printf("-- gc begin\n");
        size_t before = vm.bytes_allocated;
    #endif

    mark_roots();
    trace_references();
    table_remove_white(&vm.strings);
    sweep();
    vm.next_gc = vm.bytes_allocated * GC_HEAP_GROW_FACTOR;

    #ifdef DEBUG_LOG_GC
        printf("-- gc end\n");
        printf(
            "   collected %ld bytes (from %ld to %ld) next at %ld\n",
            before - vm.bytes_allocated, before, vm.bytes_allocated, vm.next_gc
        );
    #endif
}

void free_objects() {
    // Free all objects in the linked list.
    // This is a simple way to free all memory at the end of the program.
    // In a real language implementation, you would use a more sophisticated
    // memory management scheme that frees memory incrementally.
    // For example, you could use reference counting, a tracing garbage collector,
    // or a combination of the two.
    // We’ll keep it simple and just free everything at the end.
    Obj* object = vm.objects;
    while (object != NULL) {
        Obj* next = object->next;
        free(object);
        object = next;
    }
    free(vm.grey_stack);
}