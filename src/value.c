#include <stdio.h>
#include <string.h>

#include "value.h"
#include "memory.h"
#include "object.h"

bool value_eq(Value a, Value b) {
    #ifdef NAN_BOXING
        return a == b; // Bit rep comparison.
    #else
        if (a.type != b.type) return false;
        switch (a.type) {
            case VAL_BOOLEAN:   return AS_BOOL(a) == AS_BOOL(b);
            case VAL_NUMBER:    return AS_NUMBER(a) == AS_NUMBER(b);
            case VAL_OBJ:       return AS_OBJ(a) == AS_OBJ(b);
            case VAL_NIL:       return true;
            default:            return false; // Unreachable.
        }
    #endif
}

void print_value(Value value) {
    #ifdef NAN_BOXING
        if (IS_BOOL(value)) {
            printf(AS_BOOL(value) ? "true" : "false");
        } else if (IS_NIL(value)) {
            printf("nil");
        } else if (IS_NUMBER(value)) {
            printf("%g", AS_NUMBER(value));
        } else if (IS_OBJ(value)) {
            print_object(value);
        }
    #else
        switch (value.type) {
            case VAL_BOOLEAN:
                printf(AS_BOOL(value) ? "true" : "false");
                break;
            case VAL_NUMBER:
                printf("%g", AS_NUMBER(value));
                break;
            case VAL_OBJ:
                print_object(value);
                break;
            case VAL_NIL:
                printf("nil");
                break;
        }
    #endif
}

void init_value_array(ValueArray* array) {
    array->capacity = 0;
    array->count = 0;
    array->values = NULL;
}

void write_value_array(ValueArray* array, Value value) {
    if (array->capacity < array->count + 1) {
        int old_capacity = array->capacity;
        array->capacity = GROW_CAPACITY(old_capacity);
        array->values = GROW_ARRAY(Value, array->values, old_capacity, array->capacity);
    }

    array->values[array->count] = value;
    array->count++;
}

void free_value_array(ValueArray* array) {
    FREE_ARRAY(Value, array->values, array->capacity);
    init_value_array(array);
}