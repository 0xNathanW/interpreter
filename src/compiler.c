#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "compiler.h"
#include "chunk.h"
#include "common.h"
#include "debug.h"
#include "memory.h"
#include "object.h"
#include "scanner.h"

typedef struct {
    Token current;
    Token previous;
    bool had_error;
    bool panic_mode;
} Parser;

typedef enum {
    PREC_NONE,
    PREC_ASSIGNMENT,  // =
    PREC_OR,          // or
    PREC_AND,         // and
    PREC_EQUALITY,    // == !=
    PREC_COMPARISON,  // < > <= >=
    PREC_TERM,        // + -
    PREC_FACTOR,      // * /
    PREC_UNARY,       // ! -
    PREC_CALL,        // . ()
    PREC_PRIMARY
} Precendence;

typedef void (*ParseFn)(bool can_assign);

typedef struct {
    ParseFn prefix;
    ParseFn infix;
    Precendence precedence;
} ParseRule;

typedef struct {
    Token name;
    int depth; // 0 is global, 1 is local to the outermost function, etc.
    bool is_captured; // Captured by a closure.
} Local;

typedef struct {
    uint8_t idx;
    bool is_local;
} Upvalue;

typedef enum {
    TYPE_FUNCTION,
    TYPE_INITIALIZER,
    TYPE_METHOD,
    // Top level script.
    TYPE_SCRIPT,
} FunctionType;

typedef struct Compiler {
    // Each compiler points back to the compiler for the function that encloses it.
    // Linked list/stack back to root compiler for the top-level script.
    struct Compiler* enclosing;

    ObjFunction* function;
    FunctionType type;

    Local locals[UINT8_COUNT];
    int local_count;
    Upvalue upvalues[UINT8_COUNT];
    int scope_depth;

} Compiler;

typedef struct ClassCompiler {
    // Linked list from innermost to outermost class.
    struct ClassCompiler* enclosing;
    bool has_superclass;
} ClassCompiler;

Parser parser;
Compiler* current = NULL;
ClassCompiler* current_class = NULL;
Chunk* compiling_chunk;

static Chunk* current_chunk() {
    return &current->function->chunk;
}

static void error_at(const Token* token, const char* message) {
    
    if (parser.panic_mode) return;
    fprintf(stderr, "[line %d] Error", token->line);

    if (token->type == TOKEN_EOF) {
        fprintf(stderr, " at end");
    } else if (token->type == TOKEN_ERROR) {
        // Nothing.
    } else {
        fprintf(stderr, " at '%.*s'", token->length, token->start);
    }

    fprintf(stderr, ": %s\n", message);
    parser.had_error = true;
}

static void error(const char* message) {
    error_at(&parser.previous, message);
}

static void error_at_current(const char* message) {
    error_at(&parser.current, message);
}

static void advance() {
    parser.previous = parser.current;

    for (;;) {
        parser.current = scan_token();
        if (parser.current.type != TOKEN_ERROR) break;

        error_at_current(parser.current.start);
    }
}

static void consume(TokenType type, const char* message) {
    if (parser.current.type == type) {
        advance();
        return;
    }
    error_at_current(message);
}

static bool check_type(TokenType type) {
    return parser.current.type == type;
}

static bool match(TokenType type) {
    if (!check_type(type)) return false;
    advance();
    return true;
}

static void emit_byte(uint8_t byte) {
    write_chunk(current_chunk(), byte, parser.previous.line);
}

static void emit_bytes(uint8_t byte1, uint8_t byte2) {
    emit_byte(byte1);
    emit_byte(byte2);
}

static void emit_loop(int loop_start) {
    emit_byte(OP_LOOP);
    int offset = current_chunk()->count - loop_start + 2; // +2 for the loop instruction itself.
    if (offset > UINT16_MAX) {
        error("Loop body too large.");
    }
    emit_byte((offset >> 8) & 0xff);
    emit_byte(offset & 0xff);
}

static int emit_jump(uint8_t instruction) {
    emit_byte(instruction);
    // Placeholder for jump offset, 16bit is max 65,535 byte jump.
    emit_byte(0xff);
    emit_byte(0xff);
    return current_chunk()->count - 2;
}

static void emit_return() {
    if (current->type == TYPE_INITIALIZER) {
        // Return the instance.
        emit_bytes(OP_GET_LOCAL, 0);
    } else {
        emit_byte(OP_NIL);
    }
    emit_byte(OP_RETURN);
}

// Returns the index where the constant was added.
static uint8_t make_constant(Value value) {
    int constant = add_constant(current_chunk(), value);
    if (constant > UINT8_MAX) {
        error("Too many constants in one chunk.");
        return 0;
    }

    return (uint8_t)constant;
}

static void emit_constant(Value value) {
    emit_bytes(OP_CONSTANT, make_constant(value));
}

// Replaces the placeholder with the actual jump offset.
static void patch_jump(int offset) {
    // -2 to adjust for the bytecode for the jump offset itself.
    int jump = current_chunk()->count - offset - 2;
    if (jump > UINT16_MAX) {
        error("Too much code to jump over.");
    }

    current_chunk()->code[offset] = (jump >> 8) & 0xff;
    current_chunk()->code[offset + 1] = jump & 0xff;
}

static void init_compiler(Compiler *compiler, FunctionType type) {
    
    compiler->enclosing = current;
    compiler->function = NULL;
    compiler->type = type;
    compiler->local_count = 0;
    compiler->scope_depth = 0;
    compiler->function = new_function();
    current = compiler;
    if (type != TYPE_SCRIPT) {
        // Copy name because src string fill be freed after compilation.
        current->function->name = copy_string(
            parser.previous.start, 
            parser.previous.length
        );
    }

    // Claim the first slot for the implicit receiver.
    Local* local = &current->locals[current->local_count++];
    local->depth = 0;
    local->is_captured = false;
    if (type != TYPE_FUNCTION) {
        // In a method, we have "this".
        local->name.start = "this";
        local->name.length = 4;
    } else {
        local->name.start = "";
        local->name.length = 0;
    }
}

static ObjFunction* exit_compiler() {
    emit_return();
    ObjFunction* function = current->function;
    #ifdef DEBUG_PRINT_CODE
        if (!parser.had_error) {
            disassemble_chunk(
                current_chunk(),
                function->name != NULL ? function->name->chars : "<script>"
            );
        }
    #endif
    // Pop the current compiler off the stack.
    current = current->enclosing;
    return function;
}

static void begin_scope() {
    current->scope_depth++;
}

static void end_scope() {
    current->scope_depth--;
    while(
        current->local_count > 0 && 
        current->locals[current->local_count - 1].depth > current->scope_depth
    ) {
        if (current->locals[current->local_count - 1].is_captured) {
            emit_byte(OP_CLOSE_UPVALUE); // OP to move to heap.
        } else {
            emit_byte(OP_POP); // Otherwise pop off the stack.
        }
        current->local_count--; // Decrease the array length.
    }
}

static void expression();
static void statement();
static void declaration();
static void parse_precedence(Precendence precedence);
static ParseRule* get_rule(TokenType type);

// Inserts token name into the constant table and returns its index.
static uint8_t identifier_constant(const Token* name) {
    return make_constant(OBJ_VAL(copy_string(name->start, name->length)));
}

static bool identifiers_equal(const Token* a, const Token* b) {
    if (a->length != b->length) return false;
    return memcmp(a->start, b->start, a->length) == 0;
}

// Iterate local array backwards so that most recently declared variable is found first.
static int resolve_local(Compiler* compiler, Token* name) {
    for (int i = compiler->local_count - 1; i >= 0; i--) {
        Local* local = &compiler->locals[i];
        if (identifiers_equal(name, &local->name)) {
            if (local->depth == -1) {
                error("Cannot read local variable in its own initializer.");
            }
            return i;
        }
    }
    return -1;
}

static int add_upvalue(Compiler* compiler, uint8_t idx, bool is_local) {
    int upvalue_count = compiler->function->upvalue_count;

    // Check if the upvalue already exists.
    for (int i = 0; i < upvalue_count; i++) {
        Upvalue* upvalue = &compiler->upvalues[i];
        if (upvalue->idx == idx && upvalue->is_local == is_local) {
            return i;
        }
    }

    if (upvalue_count == UINT8_COUNT) {
        error("Too many closure variables in function.");
        return 0;
    }

    compiler->upvalues[upvalue_count].is_local = is_local;
    compiler->upvalues[upvalue_count].idx = idx;
    return compiler->function->upvalue_count++;
}

static int resolve_upvalue(Compiler* compiler, Token* name) {
    // Reached the top level function.
    if (compiler->enclosing == NULL) return -1;

    // Resolve the identifier as a local variable in the enclosing function.
    int local = resolve_local(compiler->enclosing, name);
    if (local != -1) {
        compiler->enclosing->locals[local].is_captured = true;
        return add_upvalue(compiler, (uint8_t)local, true);
    }

    int upvalue = resolve_upvalue(compiler->enclosing, name);
    if (upvalue != -1) {
        return add_upvalue(compiler, (uint8_t)upvalue, false);
    }

    return -1;
}

static void add_local(Token name) {
    if (current->local_count == UINT8_COUNT) {
        error("Too many local variables in function.");
        return;
    }

    Local* local = &current->locals[current->local_count++];
    local->name = name;
    local->depth = -1;
    local->is_captured = false;
}

static void declare_variable() {
    if (current->scope_depth == 0) return;
    // Only for local variables.
    Token* name = &parser.previous;
    // Start from the end of the local array for current scope.
    for (int i = current->local_count - 1; i >= 0; i--) {
        Local* local = &current->locals[i];
        // If the local is in an outer scope, stop looking.
        if (local->depth != -1 && local->depth < current->scope_depth) {
            break;
        }
        if (identifiers_equal(name, &local->name)) {
            error("Variable with this name already declared in this scope.");
        }
    }
    add_local(*name);
}

static uint8_t parse_variable(const char* error_message) {
    consume(TOKEN_IDENTIFIER, error_message);
    declare_variable();
    if (current->scope_depth > 0) return 0; // Return if in local scope.
    return identifier_constant(&parser.previous);
}

static void mark_initialized() {
    // Check for top-level function declarations.
    if (current->scope_depth == 0) return;
    current->locals[current->local_count - 1].depth = current->scope_depth;
}

// Declaring a variable is when variable is added to scope.
// Defining a variable is when the variable is available for use.
// Stores variable in vm bytecode.
static void define_variable(uint8_t global) {
    if (current->scope_depth > 0) {
        mark_initialized();
        return;
    }
    emit_bytes(OP_DEFINE_GLOBAL, global);
}

static uint8_t argument_list() {
    uint8_t arg_count = 0;
    if (!check_type(TOKEN_RIGHT_PAREN)) {
        do {
            expression();
            if (arg_count == 255) {
                error("Cannot have more than 255 arguments.");
            }
            arg_count++;
        } while (match(TOKEN_COMMA));
    }
    consume(TOKEN_RIGHT_PAREN, "Expect ')' after arguments.");
    return arg_count;
}

static void and_(bool can_assign) {
    // If the left operand (on top of the stack) is falsey, jump to the end.
    int end_jump = emit_jump(OP_JUMP_IF_FALSE);
    emit_byte(OP_POP);
    parse_precedence(PREC_AND);
    patch_jump(end_jump);
}

static void binary(bool can_assign) {
    TokenType operator_type = parser.previous.type;
    ParseRule* rule = get_rule(operator_type);
    parse_precedence((Precendence)(rule->precedence + 1));
    switch (operator_type) {
        case TOKEN_EQUAL_EQUAL: emit_byte(OP_EQUAL); break;
        // a != b is equivalent to !(a == b).
        case TOKEN_BANG_EQUAL: emit_bytes(OP_EQUAL, OP_NOT); break;
        case TOKEN_GREATER: emit_byte(OP_GREATER); break;
        // a >= b is equivalent to !(a < b).
        case TOKEN_GREATER_EQUAL: emit_bytes(OP_LESS, OP_NOT); break;
        case TOKEN_LESS: emit_byte(OP_LESS); break;
        // a <= b is equivalent to !(a > b).
        case TOKEN_LESS_EQUAL: emit_bytes(OP_GREATER, OP_NOT); break;
        case TOKEN_PLUS: emit_byte(OP_ADD); break;
        case TOKEN_MINUS: emit_byte(OP_SUBTRACT); break;
        case TOKEN_STAR: emit_byte(OP_MULTIPLY); break;
        case TOKEN_SLASH: emit_byte(OP_DIVIDE); break;
        default: return;
    }
}

static void call(bool can_assign) {
    uint8_t arg_count = argument_list();
    emit_bytes(OP_CALL, arg_count);
}

static void literal(bool can_assign) {
    switch (parser.previous.type) {
        case TOKEN_FALSE: emit_byte(OP_FALSE); break;
        case TOKEN_NIL: emit_byte(OP_NIL); break;
        case TOKEN_TRUE: emit_byte(OP_TRUE); break;
        default: return;
    }
}

static void dot(bool can_assign) {
    consume(TOKEN_IDENTIFIER, "Expect property name after '.'.");
    uint8_t name = identifier_constant(&parser.previous);

    if (can_assign && match(TOKEN_EQUAL)) {
        // Assignment.
        expression();
        emit_bytes(OP_SET_PROPERTY, name);
    } else if (match(TOKEN_LEFT_PAREN)) {
        // Method call.
        uint8_t arg_count = argument_list();
        emit_bytes(OP_INVOKE, name);
        emit_byte(arg_count);    
    } else {
        // Getter.
        emit_bytes(OP_GET_PROPERTY, name);
    }
}

static void grouping(bool can_assign) {
    expression();
    consume(TOKEN_RIGHT_PAREN, "Expect ')' after expression.");
}

static void number(bool can_assign) {
    // strtod stops at the first character that cannot be part of a number.
    double value = strtod(parser.previous.start, NULL);
    emit_constant(NUMBER_VAL(value));
}

// Bit weird, maybe change.
static void or_(bool can_assign) {
    int else_jump = emit_jump(OP_JUMP_IF_FALSE);
    int end_jump = emit_jump(OP_JUMP);
    
    patch_jump(else_jump);
    emit_byte(OP_POP);
    
    parse_precedence(PREC_OR);
    patch_jump(end_jump);
}

static void string(bool can_assign) {
    // +1, -2 to remove the quotes.
    emit_constant(OBJ_VAL(copy_string(parser.previous.start + 1, parser.previous.length - 2)));
}

static void named_variable(Token name, bool can_assign) {
    uint8_t get_op, set_op;
    int idx = resolve_local(current, &name);
    if (idx != -1) {
        // Local variable.
        get_op = OP_GET_LOCAL;
        set_op = OP_SET_LOCAL;
    } else if ((idx = resolve_upvalue(current, &name)) != -1) {
        // Variable found in surrounding functions.
        get_op = OP_GET_UPVALUE;
        set_op = OP_SET_UPVALUE;
        
    } else {
        idx = identifier_constant(&name);
        get_op = OP_GET_GLOBAL;
        set_op = OP_SET_GLOBAL;
    }
    if (can_assign && match(TOKEN_EQUAL)) {
        expression();
        emit_bytes(set_op, (uint8_t)idx);
    } else {
        emit_bytes(get_op, (uint8_t)idx);
    }
}

static void variable(bool can_assign) {
    named_variable(parser.previous, can_assign);
}

static Token synthetic_token(const char* text) {
    Token token;
    token.start = text;
    token.length = (int)strlen(text);
    return token;
}

static void super_(bool can_assign) {
    if (current_class == NULL) {
        error("Cannot use 'super' outside of a class.");
    } else if (!current_class->has_superclass) {
        error("Cannot use 'super' in a class with no superclass.");
    }

    consume(TOKEN_DOT, "Expect '.' after 'super'.");
    consume(TOKEN_IDENTIFIER, "Expect superclass method name.");
    uint8_t name = identifier_constant(&parser.previous);

    // Needs the receiver and superclass on the stack.
    named_variable(synthetic_token("this"), false);
    if (match(TOKEN_LEFT_PAREN)) {
        uint8_t arg_count = argument_list();
        named_variable(synthetic_token("super"), false);
        emit_bytes(OP_SUPER_INVOKE, name);
        emit_byte(arg_count);
    } else {
        named_variable(synthetic_token("super"), false);
        emit_bytes(OP_GET_SUPER, name);
    }
}

static void this_(bool can_assign) {
    // If 'this' is used outside of a class, throw an error.
    if (current_class == NULL) {
        error("Cannot use 'this' outside of a class.");
        return;
    }
    // Compile like a local variable.
    // False because we don't want to assign to 'this'.
    variable(false);
}

static void unary(bool can_assign) {
    TokenType operator_type = parser.previous.type;
    parse_precedence(PREC_UNARY); // Unary level to parse things like !!true.
    switch (operator_type) {
        case TOKEN_MINUS: emit_byte(OP_NEGATE); break;
        case TOKEN_BANG: emit_byte(OP_NOT); break;
        default: return;
    }
}

ParseRule rules[] = {
  [TOKEN_LEFT_PAREN]    = {grouping, call,   PREC_CALL},
  [TOKEN_RIGHT_PAREN]   = {NULL,     NULL,   PREC_NONE},
  [TOKEN_LEFT_BRACE]    = {NULL,     NULL,   PREC_NONE}, 
  [TOKEN_RIGHT_BRACE]   = {NULL,     NULL,   PREC_NONE},
  [TOKEN_COMMA]         = {NULL,     NULL,   PREC_NONE},
  [TOKEN_DOT]           = {NULL,     dot,    PREC_CALL},
  [TOKEN_MINUS]         = {unary,    binary, PREC_TERM},
  [TOKEN_PLUS]          = {NULL,     binary, PREC_TERM},
  [TOKEN_SEMICOLON]     = {NULL,     NULL,   PREC_NONE},
  [TOKEN_SLASH]         = {NULL,     binary, PREC_FACTOR},
  [TOKEN_STAR]          = {NULL,     binary, PREC_FACTOR},
  [TOKEN_BANG]          = {unary,    NULL,   PREC_NONE},
  [TOKEN_BANG_EQUAL]    = {NULL,     binary, PREC_EQUALITY},
  [TOKEN_EQUAL]         = {NULL,     NULL,   PREC_NONE},
  [TOKEN_EQUAL_EQUAL]   = {NULL,     binary, PREC_EQUALITY},
  [TOKEN_GREATER]       = {NULL,     binary, PREC_COMPARISON},
  [TOKEN_GREATER_EQUAL] = {NULL,     binary, PREC_COMPARISON},
  [TOKEN_LESS]          = {NULL,     binary, PREC_COMPARISON},
  [TOKEN_LESS_EQUAL]    = {NULL,     binary, PREC_COMPARISON},
  [TOKEN_IDENTIFIER]    = {variable, NULL,   PREC_NONE},
  [TOKEN_STRING]        = {string,   NULL,   PREC_NONE},
  [TOKEN_NUMBER]        = {number,   NULL,   PREC_NONE},
  [TOKEN_AND]           = {NULL,     and_,   PREC_AND},
  [TOKEN_CLASS]         = {NULL,     NULL,   PREC_NONE},
  [TOKEN_ELSE]          = {NULL,     NULL,   PREC_NONE},
  [TOKEN_FALSE]         = {literal,  NULL,   PREC_NONE},
  [TOKEN_FOR]           = {NULL,     NULL,   PREC_NONE},
  [TOKEN_FUN]           = {NULL,     NULL,   PREC_NONE},
  [TOKEN_IF]            = {NULL,     NULL,   PREC_NONE},
  [TOKEN_NIL]           = {literal,  NULL,   PREC_NONE},
  [TOKEN_OR]            = {NULL,     or_,    PREC_OR},
  [TOKEN_PRINT]         = {NULL,     NULL,   PREC_NONE},
  [TOKEN_RETURN]        = {NULL,     NULL,   PREC_NONE},
  [TOKEN_SUPER]         = {super_,   NULL,   PREC_NONE},
  [TOKEN_THIS]          = {this_,    NULL,   PREC_NONE},
  [TOKEN_TRUE]          = {literal,  NULL,   PREC_NONE},
  [TOKEN_VAR]           = {NULL,     NULL,   PREC_NONE},
  [TOKEN_WHILE]         = {NULL,     NULL,   PREC_NONE},
  [TOKEN_ERROR]         = {NULL,     NULL,   PREC_NONE},
  [TOKEN_EOF]           = {NULL,     NULL,   PREC_NONE},
};

static void parse_precedence(Precendence precedence) {
    advance();

    ParseFn prefix_rule = get_rule(parser.previous.type)->prefix;
    if (prefix_rule == NULL) {
        error("Expect expression.");
        return;
    }
    
    bool can_assign = precedence <= PREC_ASSIGNMENT;
    prefix_rule(can_assign);

    while (precedence <= get_rule(parser.current.type)->precedence) {
        advance();
        ParseFn infix_rule = get_rule(parser.previous.type)->infix;
        infix_rule(can_assign);
    }

    if (can_assign && match(TOKEN_EQUAL)) {
        error("Invalid assignment target.");
    }
}

static ParseRule* get_rule(TokenType type) {
    return &rules[type];
}

static void expression() {
    parse_precedence(PREC_ASSIGNMENT);
};

static void block() {
    while (!check_type(TOKEN_RIGHT_BRACE) && !check_type(TOKEN_EOF)) {
        declaration();
    }
    consume(TOKEN_RIGHT_BRACE, "Expect '}' after block.");
}

static void function(FunctionType type) {
    Compiler compiler;
    init_compiler(&compiler, type);
    begin_scope();

    consume(TOKEN_LEFT_PAREN, "Expect '(' after function name.");
    if (!check_type(TOKEN_RIGHT_PAREN)) {
        do {
            current->function->arity++;
            if (current->function->arity > 255) {
                error_at_current("Cannot have more than 255 parameters.");
            }
            uint8_t param_constant = parse_variable("Expect parameter name.");
            define_variable(param_constant);
        } while (match(TOKEN_COMMA));
    }
    consume(TOKEN_RIGHT_PAREN, "Expect ')' after parameters.");
    consume(TOKEN_LEFT_BRACE, "Expect '{' before function body.");
    block();

    ObjFunction* function = exit_compiler();
    emit_bytes(OP_CLOSURE, make_constant(OBJ_VAL(function)));
    for (int i = 0; i < function->upvalue_count; i++) {
        emit_byte(compiler.upvalues[i].is_local ? 1 : 0);
        emit_byte(compiler.upvalues[i].idx);
    }
}

static void method() {
    consume(TOKEN_IDENTIFIER, "Expect method name.");
    uint8_t constant = identifier_constant(&parser.previous);
    FunctionType type = TYPE_METHOD;
    if (parser.previous.length == 4 && memcmp(parser.previous.start, "init", 4) == 0) {
        type = TYPE_INITIALIZER;
    }
    function(type);
    emit_bytes(OP_METHOD, constant);
}

static void class_declaration() {
    consume(TOKEN_IDENTIFIER, "Expect class name.");
    Token class_name = parser.previous;
    uint8_t name_constant = identifier_constant(&parser.previous);
    declare_variable();

    emit_bytes(OP_CLASS, name_constant);
    define_variable(name_constant);

    // Push a new ClassCompiler onto implicit linked stack.
    ClassCompiler class_compiler;
    class_compiler.has_superclass = false;
    class_compiler.enclosing = current_class;
    current_class = &class_compiler;

    // Class that inherits from another class.
    if (match(TOKEN_LESS)) {
        consume(TOKEN_IDENTIFIER, "Expect superclass name.");
        // Push superclass to stack.
        variable(false);
        if (identifiers_equal(&class_name, &parser.previous)) {
            error("A class cannot inherit from itself.");
        }
        // For storing superclass incase we want to call methods using "super".
        begin_scope();
        add_local(synthetic_token("super"));
        define_variable(0);

        named_variable(class_name, false);
        emit_byte(OP_INHERIT);
        class_compiler.has_superclass = true;
    }

    // Push stack to stack.
    named_variable(class_name, false);
    consume(TOKEN_LEFT_BRACE, "Expect '{' before class body.");
    while (!check_type(TOKEN_RIGHT_BRACE) && !check_type(TOKEN_EOF)) {
        method();
    }
    consume(TOKEN_RIGHT_BRACE, "Expect '}' after class body.");
    emit_byte(OP_POP); // Pop the class name.

    if (class_compiler.has_superclass) {
        end_scope();
    }

    // Pop the ClassCompiler off the stack.
    current_class = current_class->enclosing;
}

static void fun_declaration() {
    uint8_t global = parse_variable("Expect function name.");
    // Functions can be recursive, so can be initialised immediately.
    mark_initialized();
    function(TYPE_FUNCTION);
    define_variable(global);
}

static void var_declaration() {
    uint8_t global = parse_variable("Expect variable name.");

    if (match(TOKEN_EQUAL)) {
        expression();
    } else {
        // eg var a; is equivalent to var a = nil;
        emit_byte(OP_NIL);
    }

    consume(TOKEN_SEMICOLON, "Expect ';' after variable declaration.");
    define_variable(global);
}

// An expression statement is an expression followed by a semicolon.
static void expression_statement() {
    expression();
    consume(TOKEN_SEMICOLON, "Expect ';' after expression.");
    emit_byte(OP_POP);
}

static void for_statement() {
    begin_scope();

    // Initializer.
    consume(TOKEN_LEFT_PAREN, "Expect '(' after 'for'.");
    if (match(TOKEN_SEMICOLON)) {
        // No initializer.
    } else if (match(TOKEN_VAR)) {
        var_declaration();
    } else {
        expression_statement();
    }

    int loop_start = current_chunk()->count;
    // Exit condition.
    int exit_jump = -1;
    if (!match(TOKEN_SEMICOLON)) {
        expression();
        consume(TOKEN_SEMICOLON, "Expect ';' after loop condition.");
        // Jump out of the loop if the condition is false.
        exit_jump = emit_jump(OP_JUMP_IF_FALSE);
        emit_byte(OP_POP); // Condition.
    }

    // Increment, executed after the body.
    if (!match(TOKEN_RIGHT_PAREN)) {
        int body_jump = emit_jump(OP_JUMP);
        int increment_start = current_chunk()->count;
        expression();
        emit_byte(OP_POP);
        consume(TOKEN_RIGHT_PAREN, "Expect ')' after for clauses.");

        emit_loop(loop_start);
        loop_start = increment_start;
        patch_jump(body_jump);
    }

    statement();
    emit_loop(loop_start);
    if (exit_jump != -1) {
        patch_jump(exit_jump);
        emit_byte(OP_POP); // Condition.
    }
    end_scope();
}

/*
We emit the jump instruction first with a placeholder offset operand. 
We keep track of where that half-finished instruction is. 
Next, we compile the then body. Once that’s done, we know how far to jump. 
So we go back and replace that placeholder offset with the real one now that we can calculate it.
*/
static void if_statement() {
    
    consume(TOKEN_LEFT_PAREN, "Expect '(' after 'if'.");
    expression();
    consume(TOKEN_RIGHT_PAREN, "Expect ')' after condition.");
    
    int then_jump = emit_jump(OP_JUMP_IF_FALSE);
    emit_byte(OP_POP); // Pop the condition value.
    statement();

    int else_jump = emit_jump(OP_JUMP);

    patch_jump(then_jump);
    emit_byte(OP_POP);

    if (match(TOKEN_ELSE)) statement();
    patch_jump(else_jump);
}

static void print_statement() {
    expression();
    consume(TOKEN_SEMICOLON, "Expect ';' after value.");
    emit_byte(OP_PRINT);
}

static void return_statement() {
    if (current->type == TYPE_SCRIPT) {
        error("Cannot return from top-level code.");
    }
    if (match(TOKEN_SEMICOLON)) {
        emit_return();
    } else {
        if (current->type == TYPE_INITIALIZER) {
            error("Cannot return a value from an initializer.");
        }
        expression();
        consume(TOKEN_SEMICOLON, "Expect ';' after return value.");
        emit_byte(OP_RETURN);
    }
}

static void while_statement() {
    int loop_start = current_chunk()->count;

    consume(TOKEN_LEFT_PAREN, "Expect '(' after 'while'.");
    expression();
    consume(TOKEN_RIGHT_PAREN, "Expect ')' after condition.");
 
    // Jump to the end of the loop if the condition is false.
    int exit_jump = emit_jump(OP_JUMP_IF_FALSE);
    emit_byte(OP_POP);
    statement();
    emit_loop(loop_start);

    patch_jump(exit_jump);
    emit_byte(OP_POP);
}

static void synchronize() {
    parser.panic_mode = false;

    // Skip until statement boundary.
    while (parser.current.type != TOKEN_EOF) {
        if (parser.previous.type == TOKEN_SEMICOLON) return;

        switch (parser.current.type) {
            case TOKEN_CLASS:
            case TOKEN_FUN:
            case TOKEN_VAR:
            case TOKEN_FOR:
            case TOKEN_IF:
            case TOKEN_WHILE:
            case TOKEN_PRINT:
            case TOKEN_RETURN:
                return;
            default:
                // Do nothing.
                ;
        }

        advance();
    }
}

static void declaration() {
    if (match(TOKEN_CLASS)) {
        class_declaration();
    } else if (match(TOKEN_FUN)) {
        fun_declaration();
    } else if (match(TOKEN_VAR)) {
        var_declaration();
    } else {
        statement();
    }

    if (parser.panic_mode) synchronize();
}

static void statement() {
    if (match(TOKEN_PRINT)) {
        print_statement();
    } else if (match(TOKEN_IF)) {
        if_statement();
    } else if (match(TOKEN_RETURN)) {
        return_statement();
    } else if (match(TOKEN_WHILE)) {
        while_statement();
    } else if (match(TOKEN_FOR)) {
        for_statement();
    } else if (match(TOKEN_LEFT_BRACE)) {
        // New block, new scope.
        begin_scope();
        block();
        end_scope();
    } else {
        expression_statement();
    }
}

ObjFunction* compile(const char* src) {
    init_scanner(src);
    Compiler compiler;
    init_compiler(&compiler, TYPE_SCRIPT);
    
    parser.had_error = false;
    parser.panic_mode = false;
    
    advance();
    while (!match(TOKEN_EOF)) {
        declaration();
    }

    ObjFunction* function = exit_compiler();
    return parser.had_error ? NULL : function;
}

void mark_compiler_roots() {
    Compiler* compiler = current;
    while (compiler != NULL) {
        mark_object((Obj*)compiler->function);
        compiler = compiler->enclosing;
    }
}