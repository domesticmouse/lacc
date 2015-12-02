#include "cli.h"
#include "eval.h"
#include "parse.h"
#include "string.h"
#include "../frontend/preprocess.h"

#include <stdio.h>
#include <assert.h>
#include <stdlib.h>
#include <string.h>

#define FIRST_type_qualifier \
    CONST: case VOLATILE

#define FIRST_type_specifier \
    VOID: case CHAR: case SHORT: case INT: case LONG: case FLOAT: case DOUBLE: \
    case SIGNED: case UNSIGNED: case STRUCT: case UNION: case ENUM

#define FIRST_type_name \
    FIRST_type_qualifier: \
    case FIRST_type_specifier

#define FIRST(s) FIRST_ ## s

#define set_break_target(old, brk) \
    old = break_target; \
    break_target = brk;

#define set_continue_target(old, cont) \
    old = continue_target; \
    continue_target = cont;

#define restore_break_target(old) \
    break_target = old;

#define restore_continue_target(old) \
    continue_target = old;

/* Store reference to top of loop, for resolving break and continue. Use call
 * stack to keep track of depth, backtracking to the old value.
 */
static struct block
    *break_target,
    *continue_target;

/* Keep track of nested switch statements and their case labels.
 */
static struct switch_context {
    struct block *default_label;
    struct block **case_label;
    struct var *case_value;
    int n;
} *switch_ctx;

static void add_switch_case(struct block *label, struct var value)
{
    struct switch_context *ctx = switch_ctx;

    ctx->n++;
    ctx->case_label =
        realloc(ctx->case_label, ctx->n * sizeof(*ctx->case_label));
    ctx->case_value =
        realloc(ctx->case_value, ctx->n * sizeof(*ctx->case_value));

    ctx->case_label[ctx->n - 1] = label;
    ctx->case_value[ctx->n - 1] = value;
}

static void free_switch_context(struct switch_context *ctx)
{
    assert(ctx);
    if (ctx->n) {
        free(ctx->case_label);
        free(ctx->case_value);
    }
    free(ctx);
}

static int is_immediate_true(struct var e)
{
    return e.kind == IMMEDIATE && is_integer(e.type) && e.imm.i;
}

static int is_immediate_false(struct var e)
{
    return e.kind == IMMEDIATE && is_integer(e.type) && !e.imm.i;
}

static struct block *expression(struct block *block);
static struct block *assignment_expression(struct block *block);
static struct block *cast_expression(struct block *block);
static struct block *statement(struct block *parent);
static struct block *block(struct block *parent);
static struct block *initializer(struct block *block, struct var target);
static struct block *declaration(struct block *parent);
static struct typetree *declaration_specifiers(int *stc);
static struct typetree *declarator(struct typetree *base, const char **symbol);

/* Parse call to builtin symbol __builtin_va_start, which is the result of
 * calling va_start(arg, s). Return type depends on second input argument.
 */
static struct block *parse__builtin_va_start(struct block *block)
{
    struct symbol *sym;
    struct token param;
    int is_invalid;

    consume('(');
    block = assignment_expression(block);
    consume(',');
    param = consume(IDENTIFIER);
    sym = sym_lookup(&ns_ident, param.strval);

    is_invalid = !sym || sym->depth != 1 || !current_cfg.fun;
    is_invalid = is_invalid || !nmembers(&current_cfg.fun->type);
    is_invalid = is_invalid || strcmp(
        get_member(&current_cfg.fun->type,
            nmembers(&current_cfg.fun->type) - 1)->name,
        param.strval);

    if (is_invalid) {
        error("Second parameter of va_start must be last function argument.");
        exit(1);
    }

    consume(')');
    block->expr = eval__builtin_va_start(block, block->expr);
    return block;
}

/* Parse call to builtin symbol __builtin_va_arg, which is the result of calling
 * va_arg(arg, T). Return type depends on second input argument.
 */
static struct block *parse__builtin_va_arg(struct block *block)
{
    struct typetree *type;

    consume('(');
    block = assignment_expression(block);
    consume(',');
    type = declaration_specifiers(NULL);
    if (peek().token != ')') {
        type = declarator(type, NULL);
    }
    consume(')');
    block->expr = eval__builtin_va_arg(block, block->expr, type);
    return block;
}

static struct block *primary_expression(struct block *block)
{
    const struct symbol *sym;
    struct token tok;

    switch ((tok = next()).token) {
    case IDENTIFIER:
        sym = sym_lookup(&ns_ident, tok.strval);
        if (!sym) {
            error("Undefined symbol '%s'.", tok.strval);
            exit(1);
        }
        /* Special handling for builtin pseudo functions. These are expected to
         * behave as macros, thus should be no problem parsing as function call
         * in primary expression. Constructs like (va_arg)(args, int) will not
         * work with this scheme. */
        if (!strcmp("__builtin_va_start", sym->name)) {
            block = parse__builtin_va_start(block);
        } else if (!strcmp("__builtin_va_arg", sym->name)) {
            block = parse__builtin_va_arg(block);
        } else {
            block->expr = var_direct(sym);
        }
        break;
    case INTEGER_CONSTANT:
        block->expr = var_int(tok.intval);
        break;
    case '(':
        block = expression(block);
        consume(')');
        break;
    case STRING:
        /* Immediate value of type char [n]. Will decay into char * immediate on
         * evaluation, and be added to string table. */
        block->expr = var_string(tok.strval);
        break;
    default:
        error("Unexpected token '%s', not a valid primary expression.",
            tok.strval);
        exit(1);
    }

    return block;
}

static struct block *postfix_expression(struct block *block)
{
    struct var root;

    block = primary_expression(block);
    root = block->expr;

    while (1) {
        const struct member *field;
        const struct typetree *type;
        struct var expr, copy, *arg;
        struct token tok;
        int i, j;

        switch ((tok = peek()).token) {
        case '[':
            do {
                /* Evaluate a[b] = *(a + b). The semantics of pointer arithmetic
                 * takes care of multiplying b with the correct width. */
                consume('[');
                block = expression(block);
                root = eval_expr(block, IR_OP_ADD, root, block->expr);
                root = eval_deref(block, root);
                consume(']');
            } while (peek().token == '[');
            break;
        case '(':
            type = root.type;
            if (is_pointer(root.type) && is_function(root.type->next))
                type = type_deref(root.type);
            else if (!is_function(root.type)) {
                error("Expression must have type pointer to function, was %t.",
                    root.type);
                exit(1);
            }
            consume('(');
            arg = calloc(nmembers(type), sizeof(*arg));
            for (i = 0; i < nmembers(type); ++i) {
                if (peek().token == ')') {
                    error("Too few arguments, expected %d but got %d.",
                        nmembers(type), i);
                    exit(1);
                }
                block = assignment_expression(block);
                arg[i] = block->expr;
                /* todo: type check here. */
                if (i < nmembers(type) - 1) {
                    consume(',');
                }
            }
            while (is_vararg(type) && peek().token != ')') {
                consume(',');
                arg = realloc(arg, (i + 1) * sizeof(*arg));
                block = assignment_expression(block);
                arg[i] = block->expr;
                i++;
            }
            consume(')');
            for (j = 0; j < i; ++j)
                param(block, arg[j]);
            free(arg);
            root = eval_call(block, root);
            break;
        case '.':
            consume('.');
            tok = consume(IDENTIFIER);
            field = find_type_member(root.type, tok.strval);
            if (!field) {
                error("Invalid field access, no member named '%s'.",
                    tok.strval);
                exit(1);
            }
            root.type = field->type;
            root.offset += field->offset;
            break;
        case ARROW:
            consume(ARROW);
            tok = consume(IDENTIFIER);
            if (is_pointer(root.type) && is_struct_or_union(root.type->next)) {
                field = find_type_member(type_deref(root.type), tok.strval);
                if (!field) {
                    error("Invalid field access, no member named '%s'.",
                        tok.strval);
                    exit(1);
                }

                /* Make it look like a pointer to the field type, then perform
                 * normal dereferencing. */
                root.type = type_init(T_POINTER, field->type);
                root = eval_deref(block, root);
                root.offset = field->offset;
            } else {
                error("Invalid field access.");
                exit(1);
            }
            break;
        case INCREMENT:
            consume(INCREMENT);
            copy = create_var(root.type);
            eval_assign(block, copy, root);
            expr = eval_expr(block, IR_OP_ADD, root, var_int(1));
            eval_assign(block, root, expr);
            root = copy;
            break;
        case DECREMENT:
            consume(DECREMENT);
            copy = create_var(root.type);
            eval_assign(block, copy, root);
            expr = eval_expr(block, IR_OP_SUB, root, var_int(1));
            eval_assign(block, root, expr);
            root = copy;
            break;
        default:
            block->expr = root;
            return block;
        }
    }
}

static struct block *unary_expression(struct block *block)
{
    struct var value;

    switch (peek().token) {
    case '&':
        consume('&');
        block = cast_expression(block);
        block->expr = eval_addr(block, block->expr);
        break;
    case '*':
        consume('*');
        block = cast_expression(block);
        block->expr = eval_deref(block, block->expr);
        break;
    case '!':
        consume('!');
        block = cast_expression(block);
        block->expr = eval_expr(block, IR_OP_EQ, var_int(0), block->expr);
        break;
    case '~':
        consume('~');
        block = cast_expression(block);
        block->expr = eval_expr(block, IR_NOT, block->expr);
        break;
    case '+':
        consume('+');
        block = cast_expression(block);
        block->expr.lvalue = 0;
        break;
    case '-':
        consume('-');
        block = cast_expression(block);
        block->expr = eval_expr(block, IR_OP_SUB, var_int(0), block->expr);
        break;
    case SIZEOF: {
        struct typetree *type;
        struct block *head = cfg_block_init(), *tail;
        consume(SIZEOF);
        if (peek().token == '(') {
            switch (peekn(2).token) {
            case FIRST(type_name):
                consume('(');
                type = declaration_specifiers(NULL);
                if (peek().token != ')') {
                    type = declarator(type, NULL);
                }
                consume(')');
                break;
            default:
                tail = unary_expression(head);
                type = (struct typetree *) tail->expr.type;
                break;
            }
        } else {
            tail = unary_expression(head);
            type = (struct typetree *) tail->expr.type;
        }
        if (is_function(type)) {
            error("Cannot apply 'sizeof' to function type.");
        }
        if (!size_of(type)) {
            error("Cannot apply 'sizeof' to incomplete type.");
        }
        block->expr = var_int(size_of(type));
        break;
    }
    case INCREMENT:
        consume(INCREMENT);
        block = unary_expression(block);
        value = block->expr;
        block->expr = eval_expr(block, IR_OP_ADD, value, var_int(1));
        block->expr = eval_assign(block, value, block->expr);
        break;
    case DECREMENT:
        consume(DECREMENT);
        block = unary_expression(block);
        value = block->expr;
        block->expr = eval_expr(block, IR_OP_SUB, value, var_int(1));
        block->expr = eval_assign(block, value, block->expr);
        break;
    default:
        block = postfix_expression(block);
        break;
    }

    return block;
}

static struct block *cast_expression(struct block *block)
{
    struct typetree *type;
    struct token tok;
    struct symbol *sym;

    /* This rule needs two lookahead; to see beyond the initial parenthesis if
     * it is actually a cast or an expression. */
    if (peek().token == '(') {
        tok = peekn(2);
        switch (tok.token) {
        case IDENTIFIER:
            sym = sym_lookup(&ns_ident, tok.strval);
            if (!sym || sym->symtype != SYM_TYPEDEF)
                break;
        case FIRST(type_name):
            consume('(');
            type = declaration_specifiers(NULL);
            if (peek().token != ')') {
                type = declarator(type, NULL);
            }
            consume(')');
            block = cast_expression(block);
            block->expr = eval_cast(block, block->expr, type);
            return block;
        default:
            break;
        }
    }

    return unary_expression(block);
}

static struct block *multiplicative_expression(struct block *block)
{
    struct var value;

    block = cast_expression(block);
    while (1) {
        value = block->expr;
        if (peek().token == '*') {
            consume('*');
            block = cast_expression(block);
            block->expr = eval_expr(block, IR_OP_MUL, value, block->expr);
        } else if (peek().token == '/') {
            consume('/');
            block = cast_expression(block);
            block->expr = eval_expr(block, IR_OP_DIV, value, block->expr);
        } else if (peek().token == '%') {
            consume('%');
            block = cast_expression(block);
            block->expr = eval_expr(block, IR_OP_MOD, value, block->expr);
        } else break;
    }

    return block;
}

static struct block *additive_expression(struct block *block)
{
    struct var value;

    block = multiplicative_expression(block);
    while (1) {
        value = block->expr;
        if (peek().token == '+') {
            consume('+');
            block = multiplicative_expression(block);
            block->expr = eval_expr(block, IR_OP_ADD, value, block->expr);
        } else if (peek().token == '-') {
            consume('-');
            block = multiplicative_expression(block);
            block->expr = eval_expr(block, IR_OP_SUB, value, block->expr);
        } else break;
    }

    return block;
}

static struct block *shift_expression(struct block *block)
{
    struct var value;

    block = additive_expression(block);
    while (1) {
        value = block->expr;
        if (peek().token == LSHIFT) {
            consume(LSHIFT);
            block = additive_expression(block);
            block->expr = eval_expr(block, IR_OP_SHL, value, block->expr);
        } else if (peek().token == RSHIFT) {
            consume(RSHIFT);
            block = additive_expression(block);
            block->expr = eval_expr(block, IR_OP_SHR, value, block->expr);
        } else break;
    }

    return block;
}

static struct block *relational_expression(struct block *block)
{
    struct var value;

    block = shift_expression(block);
    while (1) {
        value = block->expr;
        switch (peek().token) {
        case '<':
            consume('<');
            block = shift_expression(block);
            block->expr = eval_expr(block, IR_OP_GT, block->expr, value);
            break;
        case '>':
            consume('>');
            block = shift_expression(block);
            block->expr = eval_expr(block, IR_OP_GT, value, block->expr);
            break;
        case LEQ:
            consume(LEQ);
            block = shift_expression(block);
            block->expr = eval_expr(block, IR_OP_GE, block->expr, value);
            break;
        case GEQ:
            consume(GEQ);
            block = shift_expression(block);
            block->expr = eval_expr(block, IR_OP_GE, value, block->expr);
            break;
        default:
            return block;
        }
    }
}

static struct block *equality_expression(struct block *block)
{
    struct var value;

    block = relational_expression(block);
    while (1) {
        value = block->expr;
        if (peek().token == EQ) {
            consume(EQ);
            block = relational_expression(block);
            block->expr = eval_expr(block, IR_OP_EQ, value, block->expr);
        } else if (peek().token == NEQ) {
            consume(NEQ);
            block = relational_expression(block);
            block->expr = 
                eval_expr(block, IR_OP_EQ, var_int(0),
                    eval_expr(block, IR_OP_EQ, value, block->expr));
        } else break;
    }

    return block;
}

static struct block *and_expression(struct block *block)
{
    struct var value;

    block = equality_expression(block);
    while (peek().token == '&') {
        consume('&');
        value = block->expr;
        block = equality_expression(block);
        block->expr = eval_expr(block, IR_OP_AND, value, block->expr);
    }

    return block;
}

static struct block *exclusive_or_expression(struct block *block)
{
    struct var value;

    block = and_expression(block);
    while (peek().token == '^') {
        consume('^');
        value = block->expr;
        block = and_expression(block);
        block->expr = eval_expr(block, IR_OP_XOR, value, block->expr);
    }

    return block;
}

static struct block *inclusive_or_expression(struct block *block)
{
    struct var value;

    block = exclusive_or_expression(block);
    while (peek().token == '|') {
        consume('|');
        value = block->expr;
        block = exclusive_or_expression(block);
        block->expr = eval_expr(block, IR_OP_OR, value, block->expr);
    }

    return block;
}

static struct block *logical_and_expression(struct block *block)
{
    struct block *right;

    block = inclusive_or_expression(block);
    if (peek().token == LOGICAL_AND) {
        consume(LOGICAL_AND);
        right = cfg_block_init();
        block = eval_logical_and(block, right, logical_and_expression(right));
    }

    return block;
}

static struct block *logical_or_expression(struct block *block)
{
    struct block *right;

    block = logical_and_expression(block);
    if (peek().token == LOGICAL_OR) {
        consume(LOGICAL_OR);
        right = cfg_block_init();
        block = eval_logical_or(block, right, logical_or_expression(right));
    }

    return block;
}

static struct block *conditional_expression(struct block *block)
{
    block = logical_or_expression(block);
    if (peek().token == '?') {
        struct var condition = block->expr;
        struct block
            *t = cfg_block_init(),
            *f = cfg_block_init(),
            *next = cfg_block_init();

        consume('?');
        block->jump[0] = f;
        block->jump[1] = t;

        t = expression(t);
        t->jump[0] = next;

        consume(':');
        f = conditional_expression(f);
        f->jump[0] = next;

        next->expr = eval_conditional(condition, t, f);
        block = next;
    }

    return block;
}

static struct var constant_expression(void)
{
    struct block *head = cfg_block_init(),
            *tail;

    tail = conditional_expression(head);
    if (tail != head || tail->expr.kind != IMMEDIATE) {
        error("Constant expression must be computable at compile time.");
        exit(1);
    }

    return tail->expr;
}

static struct block *assignment_expression(struct block *block)
{
    struct var target;

    block = conditional_expression(block);
    target = block->expr;
    switch (peek().token) {
    case '=':
        consume('=');
        block = assignment_expression(block);
        break;
    case MUL_ASSIGN:
        consume(MUL_ASSIGN);
        block = assignment_expression(block);
        block->expr = eval_expr(block, IR_OP_MUL, target, block->expr);
        break;
    case DIV_ASSIGN:
        consume(DIV_ASSIGN);
        block = assignment_expression(block);
        block->expr = eval_expr(block, IR_OP_DIV, target, block->expr);
        break;
    case MOD_ASSIGN:
        consume(MOD_ASSIGN);
        block = assignment_expression(block);
        block->expr = eval_expr(block, IR_OP_MOD, target, block->expr);
        break;
    case PLUS_ASSIGN:
        consume(PLUS_ASSIGN);
        block = assignment_expression(block);
        block->expr = eval_expr(block, IR_OP_ADD, target, block->expr);
        break;
    case MINUS_ASSIGN:
        consume(MINUS_ASSIGN);
        block = assignment_expression(block);
        block->expr = eval_expr(block, IR_OP_SUB, target, block->expr);
        break;
    case AND_ASSIGN:
        consume(AND_ASSIGN);
        block = assignment_expression(block);
        block->expr = eval_expr(block, IR_OP_AND, target, block->expr);
        break;
    case OR_ASSIGN:
        consume(OR_ASSIGN);
        block = assignment_expression(block);
        block->expr = eval_expr(block, IR_OP_OR, target, block->expr);
        break;
    case XOR_ASSIGN:
        consume(XOR_ASSIGN);
        block = assignment_expression(block);
        block->expr = eval_expr(block, IR_OP_XOR, target, block->expr);
        break;
    default:
        return block;
    }

    block->expr = eval_assign(block, target, block->expr);
    return block;
}

static struct block *expression(struct block *block)
{
    block = assignment_expression(block);
    while (peek().token == ',') {
        consume(',');
        block = assignment_expression(block);
    }

    return block;
}

static struct block *if_statement(struct block *parent)
{
    struct block
        *right = cfg_block_init(),
        *next  = cfg_block_init();

    consume(IF);
    consume('(');
    parent = expression(parent);
    consume(')');
    if (is_immediate_true(parent->expr)) {
        parent->jump[0] = right;
    } else if (is_immediate_false(parent->expr)) {
        parent->jump[0] = next;
    } else {
        parent->jump[0] = next;
        parent->jump[1] = right;
    }

    right = statement(right);
    right->jump[0] = next;
    if (peek().token == ELSE) {
        struct block *left = cfg_block_init();
        consume(ELSE);
        parent->jump[0] = left;
        left = statement(left);
        left->jump[0] = next;
    }

    return next;
}

static struct block *do_statement(struct block *parent)
{
    struct block
        *top = cfg_block_init(),
        *body,
        *cond = cfg_block_init(),
        *tail,
        *next = cfg_block_init();

    struct block
        *old_break_target,
        *old_continue_target;

    set_break_target(old_break_target, next);
    set_continue_target(old_continue_target, cond);
    parent->jump[0] = top;

    consume(DO);
    body = statement(top);
    body->jump[0] = cond;
    consume(WHILE);
    consume('(');
    tail = expression(cond);
    consume(')');
    if (is_immediate_true(tail->expr)) {
        tail->jump[0] = top;
    } else if (is_immediate_false(tail->expr)) {
        tail->jump[0] = next;
    } else {
        tail->jump[0] = next;
        tail->jump[1] = top;
    }

    restore_break_target(old_break_target);
    restore_continue_target(old_continue_target);
    return next;
}

static struct block *while_statement(struct block *parent)
{
    struct block
        *top = cfg_block_init(),
        *cond,
        *body = cfg_block_init(),
        *next = cfg_block_init();

    struct block
        *old_break_target,
        *old_continue_target;

    set_break_target(old_break_target, next);
    set_continue_target(old_continue_target, top);
    parent->jump[0] = top;

    consume(WHILE);
    consume('(');
    cond = expression(top);
    consume(')');
    if (is_immediate_true(cond->expr)) {
        cond->jump[0] = body;
    } else if (is_immediate_false(cond->expr)) {
        cond->jump[0] = next;
    } else {
        cond->jump[0] = next;
        cond->jump[1] = body;
    }

    body = statement(body);
    body->jump[0] = top;

    restore_break_target(old_break_target);
    restore_continue_target(old_continue_target);
    return next;
}

static struct block *for_statement(struct block *parent)
{
    struct block
        *top = cfg_block_init(),
        *body = cfg_block_init(),
        *increment = cfg_block_init(),
        *next = cfg_block_init();

    struct block
        *old_break_target,
        *old_continue_target;

    set_break_target(old_break_target, next);
    set_continue_target(old_continue_target, increment);

    consume(FOR);
    consume('(');
    if (peek().token != ';') {
        parent = expression(parent);
    }

    consume(';');
    if (peek().token != ';') {
        parent->jump[0] = top;
        top = expression(top);
        if (is_immediate_true(top->expr)) {
            top->jump[0] = body;
        } else if (is_immediate_false(top->expr)) {
            top->jump[0] = next;
        } else {
            top->jump[0] = next;
            top->jump[1] = body;
        }
        top = (struct block *) parent->jump[0];
    } else {
        /* Infinite loop */
        parent->jump[0] = body;
        top = body;
    }

    consume(';');
    if (peek().token != ')') {
        expression(increment)->jump[0] = top;
    }

    consume(')');
    body = statement(body);
    body->jump[0] = increment;

    restore_break_target(old_break_target);
    restore_continue_target(old_continue_target);
    return next;
}

static struct block *switch_statement(struct block *parent)
{
    struct block
        *body = cfg_block_init(),
        *last,
        *next = cfg_block_init();

    struct switch_context *old_switch_ctx;
    struct block *old_break_target;

    set_break_target(old_break_target, next);
    old_switch_ctx = switch_ctx;
    switch_ctx = calloc(1, sizeof(*switch_ctx));

    consume(SWITCH);
    consume('(');
    parent = expression(parent);
    consume(')');
    last = statement(body);
    last->jump[0] = next;

    if (!switch_ctx->n && !switch_ctx->default_label) {
        parent->jump[0] = next;
    } else {
        int i;
        struct block *cond = parent;

        for (i = 0; i < switch_ctx->n; ++i) {
            struct block *prev_cond = cond;
            struct block *label = switch_ctx->case_label[i];
            struct var value = switch_ctx->case_value[i];

            cond = cfg_block_init();
            cond->expr = eval_expr(cond, IR_OP_EQ, value, parent->expr);
            cond->jump[1] = label;
            prev_cond->jump[0] = cond;
        }

        cond->jump[0] = (switch_ctx->default_label) ?
            switch_ctx->default_label : next;
    }

    free_switch_context(switch_ctx);
    restore_break_target(old_break_target);
    switch_ctx = old_switch_ctx;
    return next;
}

static struct block *statement(struct block *parent)
{
    const struct symbol *sym;
    struct token tok;

    switch ((tok = peek()).token) {
    case ';':
        consume(';');
        break;
    case '{':
        parent = block(parent);
        break;
    case IF:
        parent = if_statement(parent);
        break;
    case DO:
        parent = do_statement(parent);
        break;
    case WHILE:
        parent = while_statement(parent);
        break;
    case FOR:
        parent = for_statement(parent);
        break;
    case GOTO:
        consume(GOTO);
        consume(IDENTIFIER);
        /* todo */
        consume(';');
        break;
    case CONTINUE:
    case BREAK:
        next();
        parent->jump[0] =
            (tok.token == CONTINUE) ? continue_target : break_target;
        consume(';');
        /* Return orphan node, which is dead code unless there is a label and a
         * goto statement. */
        parent = cfg_block_init(); 
        break;
    case RETURN:
        consume(RETURN);
        if (!is_void(current_cfg.fun->type.next)) {
            parent = expression(parent);
            parent->expr = eval_return(parent, current_cfg.fun->type.next);
        }
        consume(';');
        parent = cfg_block_init(); /* orphan */
        break;
    case SWITCH:
        parent = switch_statement(parent);
        break;
    case CASE:
        consume(CASE);
        if (!switch_ctx) {
            error("Stray 'case' label, must be inside a switch statement.");
        } else {
            struct block *next = cfg_block_init();
            struct var expr = constant_expression();
            consume(':');
            add_switch_case(next, expr);
            parent->jump[0] = next;
            next = statement(next);
            parent = next;
        }
        break;
    case DEFAULT:
        consume(DEFAULT);
        consume(':');
        if (!switch_ctx) {
            error("Stray 'default' label, must be inside a switch statement.");
        } else if (switch_ctx->default_label) {
            error("Multiple 'default' labels inside the same switch.");
        } else {
            struct block *next = cfg_block_init();
            parent->jump[0] = next;
            switch_ctx->default_label = next;
            next = statement(next);
            parent = next;
        }
        break;
    case IDENTIFIER:
        sym = sym_lookup(&ns_ident, tok.strval);
        if (sym && sym->symtype == SYM_TYPEDEF) {
            parent = declaration(parent);
            break;
        }
        /* fallthrough */
    case INTEGER_CONSTANT:
    case STRING:
    case '*':
    case '(':
        parent = expression(parent);
        consume(';');
        break;
    default:
        parent = declaration(parent);
        break;
    }

    return parent;
}

/* Treat statements and declarations equally, allowing declarations in between
 * statements as in modern C. Called compound-statement in K&R.
 */
static struct block *block(struct block *parent)
{
    consume('{');
    push_scope(&ns_ident);
    push_scope(&ns_tag);
    while (peek().token != '}') {
        parent = statement(parent);
    }
    consume('}');
    pop_scope(&ns_tag);
    pop_scope(&ns_ident);
    return parent;
}

/* FOLLOW(parameter-list) = { ')' }, peek to return empty list; even though K&R
 * require at least specifier: (void)
 * Set parameter-type-list = parameter-list, including the , ...
 */
static struct typetree *parameter_list(const struct typetree *base)
{
    struct typetree *func = type_init(T_FUNCTION);
    func->next = base;

    while (peek().token != ')') {
        const char *name = NULL;
        struct typetree *type;

        type = declaration_specifiers(NULL);
        type = declarator(type, &name);
        if (is_void(type)) {
            if (nmembers(func)) {
                error("Incomplete type in parameter list.");
            }
            break;
        }

        type_add_member(func, name, type);
        if (peek().token != ',') {
            break;
        }

        consume(',');
        if (peek().token == ')') {
            error("Unexpected trailing comma in parameter list.");
            exit(1);
        } else if (peek().token == DOTS) {
            consume(DOTS);
            assert(!is_vararg(func));
            type_add_member(func, "...", NULL);
            assert(is_vararg(func));
            break;
        }
    }

    return func;
}

/* Parse array declarations of the form [s0][s1]..[sn], resulting in type
 * [s0] [s1] .. [sn] (base).
 *
 * Only the first dimension s0 can be unspecified, yielding an incomplete type.
 * Incomplete types are represented by having size of zero.
 */
static struct typetree *direct_declarator_array(struct typetree *base)
{
    if (peek().token == '[') {
        long length = 0;

        consume('[');
        if (peek().token != ']') {
            struct var expr = constant_expression();
            assert(expr.kind == IMMEDIATE);
            if (!is_integer(expr.type) || expr.imm.i < 1) {
                error("Array dimension must be a natural number.");
                exit(1);
            }
            length = expr.imm.i;
        }
        consume(']');

        base = direct_declarator_array(base);
        if (!size_of(base)) {
            error("Array has incomplete element type.");
            exit(1);
        }

        base = type_init(T_ARRAY, base, length);
    }

    return base;
}

/* Parse function and array declarators. Some trickery is needed to handle
 * declarations like `void (*foo)(int)`, where the inner *foo has to be 
 * traversed first, and prepended on the outer type `* (int) -> void` 
 * afterwards making it `* (int) -> void`.
 * The type returned from declarator has to be either array, function or
 * pointer, thus only need to check for type->next to find inner tail.
 */
static struct typetree *direct_declarator(
    struct typetree *base,
    const char **symbol)
{
    struct typetree *type = base;
    struct typetree *head, *tail = NULL;
    struct token ident;

    switch (peek().token) {
    case IDENTIFIER:
        ident = consume(IDENTIFIER);
        if (!symbol) {
            error("Unexpected identifier in abstract declarator.");
            exit(1);
        }
        *symbol = ident.strval;
        break;
    case '(':
        consume('(');
        type = head = tail = declarator(NULL, symbol);
        while (tail->next) {
            tail = (struct typetree *) tail->next;
        }
        consume(')');
        break;
    default:
        break;
    }

    while (peek().token == '[' || peek().token == '(') {
        switch (peek().token) {
        case '[':
            type = direct_declarator_array(base);
            break;
        case '(':
            consume('(');
            type = parameter_list(base);
            consume(')');
            break;
        default:
            assert(0);
        }
        if (tail) {
            tail->next = type;
            type = head;
        }
        base = type;
    }

    return type;
}

static struct typetree *pointer(const struct typetree *base)
{
    struct typetree *type = type_init(T_POINTER, base);

    #define set_qualifier(d) \
        if (type->qualifier & d) \
            error("Duplicate type qualifier '%s'.", peek().strval); \
        type->qualifier |= d;

    consume('*');
    while (1) {
        if (peek().token == CONST) {
            set_qualifier(Q_CONST);
        } else if (peek().token == VOLATILE) {
            set_qualifier(Q_VOLATILE);
        } else break;
        next();
    }

    #undef set_qualifier

    return type;
}

static struct typetree *declarator(struct typetree *base, const char **symbol)
{
    while (peek().token == '*') {
        base = pointer(base);
    }

    return direct_declarator(base, symbol);
}

static void member_declaration_list(struct typetree *type)
{
    struct namespace ns = {0};
    struct typetree *decl_base, *decl_type;
    const char *name;

    push_scope(&ns);

    do {
        decl_base = declaration_specifiers(NULL);

        do {
            name = NULL;
            decl_type = declarator(decl_base, &name);

            if (!name) {
                error("Missing name in member declarator.");
                exit(1);
            } else if (!size_of(decl_type)) {
                error("Field '%s' has incomplete type '%t'.", name, decl_type);
                exit(1);
            } else {
                sym_add(&ns, name, decl_type, SYM_DECLARATION, LINK_NONE);
                type_add_member(type, name, decl_type);
            }

            if (peek().token == ',') {
                consume(',');
                continue;
            }
        } while (peek().token != ';');

        consume(';');
    } while (peek().token != '}');

    pop_scope(&ns);
}

static struct typetree *struct_or_union_declaration(void)
{
    struct symbol *sym = NULL;
    struct typetree *type = NULL;
    enum type kind =
        (next().token == STRUCT) ? T_STRUCT : T_UNION;

    if (peek().token == IDENTIFIER) {
        const char *name = consume(IDENTIFIER).strval;
        sym = sym_lookup(&ns_tag, name);
        if (!sym) {
            type = type_init(kind);
            sym = sym_add(&ns_tag, name, type, SYM_TYPEDEF, LINK_NONE);
        } else if (is_integer(&sym->type)) {
            error("Tag '%s' was previously declared as enum.", sym->name);
            exit(1);
        } else if (sym->type.type != kind) {
            error("Tag '%s' was previously declared as %s.",
                sym->name, (sym->type.type == T_STRUCT) ? "struct" : "union");
            exit(1);
        }

        /* Retrieve type from existing symbol, possibly providing a complete
         * definition that will be available for later declarations. Overwrites
         * existing type information from symbol table. */
        type = &sym->type;
        if (peek().token == '{' && type->size) {
            error("Redefiniton of '%s'.", sym->name);
            exit(1);
        }
    }

    if (peek().token == '{') {
        if (!type) {
            /* Anonymous structure; allocate a new standalone type,
             * not part of any symbol. */
            type = type_init(kind);
        }

        consume('{');
        member_declaration_list(type);
        assert(type->size);
        consume('}');
    }

    /* Return to the caller a copy of the root node, which can be overwritten
     * with new type qualifiers without altering the tag registration. */
    return (sym) ? type_tagged_copy(&sym->type, sym->name) : type;
}

static void enumerator_list(void)
{
    struct var val;
    struct symbol *sym;
    int enum_value = 0;

    consume('{');
    do {
        const char *name = consume(IDENTIFIER).strval;

        if (peek().token == '=') {
            consume('=');
            val = constant_expression();
            if (!is_integer(val.type)) {
                error("Implicit conversion from non-integer type in enum.");
            }
            enum_value = val.imm.i;
        }

        sym = sym_add(
            &ns_ident,
            name,
            &basic_type__int,
            SYM_ENUM_VALUE,
            LINK_NONE);
        sym->enum_value = enum_value++;

        if (peek().token != ',')
            break;
        consume(',');
    } while (peek().token != '}');
    consume('}');
}

static struct typetree *enum_declaration(void)
{
    struct typetree *type = type_init(T_SIGNED, 4);

    consume(ENUM);
    if (peek().token == IDENTIFIER) {
        struct symbol *tag = NULL;
        const char *name = consume(IDENTIFIER).strval;

        tag = sym_lookup(&ns_tag, name);
        if (!tag || tag->depth < ns_tag.current_depth) {
            tag = sym_add(&ns_tag, name, type, SYM_TYPEDEF, LINK_NONE);
        } else if (!is_integer(&tag->type)) {
            error("Tag '%s' was previously defined as aggregate type.",
                tag->name);
            exit(1);
        }

        /* Use enum_value as a sentinel to represent definition, checked on 
         * lookup to detect duplicate definitions. */
        if (peek().token == '{') {
            if (tag->enum_value) {
                error("Redefiniton of enum '%s'.", tag->name);
            }
            enumerator_list();
            tag->enum_value = 1;
        }
    } else {
        enumerator_list();
    }

    /* Result is always integer. Do not care about the actual enum definition,
     * all enums are ints and no type checking is done. */
    return type;
}

static struct typetree get_basic_type_from_specifier(unsigned short spec)
{
    switch (spec) {
    case 0x0001: /* void */
        return basic_type__void;
    case 0x0002: /* char */
    case 0x0012: /* signed char */
        return basic_type__char;
    case 0x0022: /* unsigned char */
        return basic_type__unsigned_char;
    case 0x0004: /* short */
    case 0x0014: /* signed short */
    case 0x000C: /* short int */
    case 0x001C: /* signed short int */
        return basic_type__short;
    case 0x0024: /* unsigned short */
    case 0x002C: /* unsigned short int */
        return basic_type__unsigned_short;
    case 0x0008: /* int */
    case 0x0010: /* signed */
    case 0x0018: /* signed int */
        return basic_type__int;
    case 0x0020: /* unsigned */
    case 0x0028: /* unsigned int */
        return basic_type__unsigned_int;
    case 0x0040: /* long */
    case 0x0050: /* signed long */
    case 0x0048: /* long int */
    case 0x0058: /* signed long int */
    case 0x00C0: /* long long */
    case 0x00D0: /* signed long long */
    case 0x00D8: /* signed long long int */
        return basic_type__long;
    case 0x0060: /* unsigned long */
    case 0x0068: /* unsigned long int */
    case 0x00E0: /* unsigned long long */
    case 0x00E8: /* unsigned long long int */
        return basic_type__unsigned_long;
    case 0x0100: /* float */
        return basic_type__float;
    case 0x0200: /* double */
    case 0x0240: /* long double */
        return basic_type__double;
    default:
        error("Invalid type specification.");
        exit(1); 
    }
}

/* Parse type, qualifiers and storage class. Do not assume int by default, but
 * require at least one type specifier. Storage class is returned as token
 * value, unless the provided pointer is NULL, in which case the input is parsed
 * as specifier-qualifier-list.
 */
static struct typetree *declaration_specifiers(int *stc)
{
    struct typetree *type = NULL;
    struct token tok;
    int done = 0;

    /* Use a compact bit representation to hold state about declaration 
     * specifiers. Initialize storage class to sentinel value. */
    unsigned short spec = 0x0000;
    enum qualifier qual = Q_NONE;
    if (stc)       *stc =    '$';

    #define set_specifier(d) \
        if (spec & d) error("Duplicate type specifier '%s'.", tok.strval); \
        next(); spec |= d;

    #define set_qualifier(d) \
        if (qual & d) error("Duplicate type qualifier '%s'.", tok.strval); \
        next(); qual |= d;

    #define set_storage_class(t) \
        if (!stc) error("Unexpected storage class in qualifier list."); \
        else if (*stc != '$') error("Multiple storage class specifiers."); \
        next(); *stc = t;

    do {
        switch ((tok = peek()).token) {
        case VOID:      set_specifier(0x001); break;
        case CHAR:      set_specifier(0x002); break;
        case SHORT:     set_specifier(0x004); break;
        case INT:       set_specifier(0x008); break;
        case SIGNED:    set_specifier(0x010); break;
        case UNSIGNED:  set_specifier(0x020); break;
        case LONG:
            if (spec & 0x040) {
                set_specifier(0x080);
            } else {
                set_specifier(0x040);   
            }
            break;
        case FLOAT:     set_specifier(0x100); break;
        case DOUBLE:    set_specifier(0x200); break;
        case CONST:     set_qualifier(Q_CONST); break;
        case VOLATILE:  set_qualifier(Q_VOLATILE); break;
        case IDENTIFIER: {
            struct symbol *tag = sym_lookup(&ns_ident, tok.strval);
            if (tag && tag->symtype == SYM_TYPEDEF && !type) {
                consume(IDENTIFIER);
                type = type_init(T_STRUCT);
                *type = tag->type;
            } else {
                done = 1;
            }
            break;
        }
        case UNION:
        case STRUCT:
            if (!type) {
                type = struct_or_union_declaration();
            } else {
                done = 1;
            }
            break;
        case ENUM:
            if (!type) {
                type = enum_declaration();
            } else {
                done = 1;
            }
            break;
        case AUTO:
        case REGISTER:
        case STATIC:
        case EXTERN:
        case TYPEDEF:
            set_storage_class(tok.token);
            break;
        default:
            done = 1;
            break;
        }

        if (type && spec) {
            error("Invalid combination of declaration specifiers.");
            exit(1);
        }
    } while (!done);

    #undef set_specifier
    #undef set_qualifier
    #undef set_storage_class

    if (type) {
        if (qual & type->qualifier) {
            error("Duplicate type qualifier:%s%s.",
                (qual & Q_CONST) ? " const" : "",
                (qual & Q_VOLATILE) ? " volatile" : "");
        }
    } else if (spec) {
        type = type_init(T_STRUCT);
        *type = get_basic_type_from_specifier(spec);
    } else {
        error("Missing type specifier.");
        exit(1);
    }

    type->qualifier |= qual;
    return type;
}

/* C99: Define __func__ as static const char __func__[] = sym->name;
 */
static void define_builtin__func__(const char *name)
{
    struct var str = var_string(name);
    struct symbol *sym =
        sym_add(&ns_ident, "__func__", str.type, SYM_DEFINITION, LINK_INTERN);

    assert(ns_ident.current_depth == 1);

    /* Initialize special case, setting char[] = char[]. */
    eval_assign(current_cfg.head, var_direct(sym), str);
}

/* Set var = 0, using simple assignment on members for composite types. This
 * rule does not consume any input, but generates a series of assignments on the
 * given variable. Point is to be able to zero initialize using normal simple
 * assignment rules, although IR can become verbose for large structures.
 */
static void zero_initialize(struct block *block, struct var target)
{
    int i;
    struct var var;
    assert(target.kind == DIRECT);

    switch (target.type->type) {
    case T_STRUCT:
    case T_UNION:
        target.type = unwrapped(target.type);
        var = target;
        for (i = 0; i < nmembers(var.type); ++i) {
            target.type = get_member(var.type, i)->type;
            target.offset = var.offset + get_member(var.type, i)->offset;
            zero_initialize(block, target);
        }
        break;
    case T_ARRAY:
        assert(target.type->size);
        var = target;
        target.type = target.type->next;
        assert(is_struct(target.type) || !target.type->next);
        for (i = 0; i < var.type->size / var.type->next->size; ++i) {
            target.offset = var.offset + i * var.type->next->size;
            zero_initialize(block, target);
        }
        break;
    case T_POINTER:
        var = var_zero(8);
        var.type = type_init(T_POINTER, &basic_type__void);
        eval_assign(block, target, var);
        break;
    case T_UNSIGNED:
    case T_SIGNED:
        var = var_zero(target.type->size);
        eval_assign(block, target, var);
        break;
    default:
        error("Invalid type to zero-initialize, was '%t'.", target.type);
        exit(1);
    }
}

static struct block *object_initializer(struct block *block, struct var target)
{
    int i,
        filled = target.offset;
    const struct typetree *type = target.type;

    assert(!is_tagged(type));

    consume('{');
    target.lvalue = 1;
    switch (type->type) {
    case T_UNION:
        /* C89 states that only the first element of a union can be
         * initialized. Zero the whole thing first if there is padding. */
        if (size_of(get_member(type, 0)->type) < type->size) {
            target.type =
                (type->size % 8) ?
                    type_init(T_ARRAY, &basic_type__char, type->size) :
                    type_init(T_ARRAY, &basic_type__long, type->size / 8);
            zero_initialize(block, target);
        }
        target.type = get_member(type, 0)->type;
        block = initializer(block, target);
        if (peek().token != '}') {
            error("Excess elements in union initializer.");
            exit(1);
        }
        break;
    case T_STRUCT:
        for (i = 0; i < nmembers(type); ++i) {
            target.type = get_member(type, i)->type;
            target.offset = filled + get_member(type, i)->offset;
            block = initializer(block, target);
            if (peek().token == ',') {
                consume(',');
            } else break;
            if (peek().token == '}') {
                break;
            }
        }
        while (++i < nmembers(type)) {
            target.type = get_member(type, i)->type;
            target.offset = filled + get_member(type, i)->offset;
            zero_initialize(block, target);
        }
        break;
    case T_ARRAY:
        target.type = type->next;
        for (i = 0; !type->size || i < type->size / size_of(type->next); ++i) {
            target.offset = filled + i * size_of(type->next);
            block = initializer(block, target);
            if (peek().token == ',') {
                consume(',');
            } else break;
            if (peek().token == '}') {
                break;
            }
        }
        if (!type->size) {
            assert(!target.symbol->type.size);
            assert(is_array(&target.symbol->type));

            /* Incomplete array type can only be in the root level of target
             * type tree, overwrite type directly in symbol. */
            ((struct symbol *) target.symbol)->type.size =
                (i + 1) * size_of(type->next);
        } else {
            while (++i < type->size / size_of(type->next)) {
                target.offset = filled + i * size_of(type->next);
                zero_initialize(block, target);
            }
        }
        break;
    default:
        error("Block initializer only apply to aggregate or union type.");
        exit(1);
    }

    consume('}');
    return block;
}

/* Parse and emit initializer code for target variable in statements such as
 * int b[] = {0, 1, 2, 3}. Generate a series of assignment operations on
 * references to target variable.
 */
static struct block *initializer(struct block *block, struct var target)
{
    assert(target.kind == DIRECT);

    /* Do not care about cv-qualifiers here. */
    target.type = unwrapped(target.type);

    if (peek().token == '{') {
        block = object_initializer(block, target);
    } else {
        block = assignment_expression(block);
        if (!target.symbol->depth && block->expr.kind != IMMEDIATE) {
            error("Initializer must be computable at load time.");
            exit(1);
        }
        if (target.kind == DIRECT && !target.type->size) {
            assert(!target.offset);
            assert(block->expr.kind == IMMEDIATE);
            assert(is_array(block->expr.type) && block->expr.string);

            /* Complete type based on string literal. */
            ((struct symbol *) target.symbol)->type.size =
                block->expr.type->size;
            target.type = block->expr.type;
        }
        eval_assign(block, target, block->expr);
    }

    return block;
}

/* Cover both external declarations, functions, and local declarations (with
 * optional initialization code) inside functions.
 */
static struct block *declaration(struct block *parent)
{
    struct typetree *base;
    enum symtype symtype;
    enum linkage linkage;
    int stc = '$';

    base = declaration_specifiers(&stc);
    switch (stc) {
    case EXTERN:
        symtype = SYM_DECLARATION;
        linkage = LINK_EXTERN;
        break;
    case STATIC:
        symtype = SYM_TENTATIVE;
        linkage = LINK_INTERN;
        break;
    case TYPEDEF:
        symtype = SYM_TYPEDEF;
        linkage = LINK_NONE;
        break;
    default:
        if (!ns_ident.current_depth) {
            symtype = SYM_TENTATIVE;
            linkage = LINK_EXTERN;
        } else {
            symtype = SYM_DEFINITION;
            linkage = LINK_NONE;
        }
        break;
    }

    while (1) {
        const char *name = NULL;
        const struct typetree *type;
        struct symbol *sym;

        type = declarator(base, &name);
        if (!name) {
            consume(';');
            return parent;
        }

        sym = sym_add(&ns_ident, name, type, symtype, linkage);
        if (ns_ident.current_depth) {
            assert(ns_ident.current_depth > 1);
            cfg_register_local(sym);
        }

        switch (peek().token) {
        case ';':
            consume(';');
            return parent;
        case '=':
            if (sym->symtype == SYM_DECLARATION) {
                error("Extern symbol '%s' cannot be initialized.", sym->name);
                exit(1);
            }
            if (!sym->depth && sym->symtype == SYM_DEFINITION) {
                error("Symbol '%s' was already defined.", sym->name);
                exit(1);
            }
            consume('=');
            sym->symtype = SYM_DEFINITION;
            if (!sym->depth || sym->n) {
                current_cfg.head = initializer(current_cfg.head, var_direct(sym));
            } else {
                parent = initializer(parent, var_direct(sym));
            }
            assert(size_of(&sym->type) > 0);
            if (peek().token != ',') {
                consume(';');
                return parent;
            }
            break;
        case '{': {
            int i;
            if (!is_function(&sym->type) || sym->depth) {
                error("Invalid function definition.");
                exit(1);
            }
            sym->symtype = SYM_DEFINITION;
            current_cfg.fun = sym;

            push_scope(&ns_ident);
            define_builtin__func__(sym->name);
            for (i = 0; i < nmembers(&sym->type); ++i) {
                name = get_member(&sym->type, i)->name;
                type = get_member(&sym->type, i)->type;
                symtype = SYM_DEFINITION;
                linkage = LINK_NONE;
                if (!name) {
                    error("Missing parameter name at position %d.", i + 1);
                    exit(1);
                }
                cfg_register_param(
                    sym_add(&ns_ident, name, type, symtype, linkage));
            }
            parent = block(parent);
            pop_scope(&ns_ident);

            return parent;
        }
        default:
            break;
        }
        consume(',');
    }
}

int parse(void)
{
    if (peek().token == END)
        return 0;

    cfg_init_current();

    while (peek().token != END) {
        current_cfg.fun = NULL;
        declaration(current_cfg.body);
        if (current_cfg.head->n || current_cfg.fun) {
            return 1;
        }
    }

    return 0;
}