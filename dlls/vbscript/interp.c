/*
 * Copyright 2011 Jacek Caban for CodeWeavers
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include <assert.h>

#include "vbscript.h"

#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(vbscript);


typedef struct {
    vbscode_t *code;
    instr_t *instr;
    script_ctx_t *script;
    function_t *func;

    unsigned stack_size;
    unsigned top;
    VARIANT *stack;
} exec_ctx_t;

typedef HRESULT (*instr_func_t)(exec_ctx_t*);

typedef enum {
    REF_NONE,
    REF_DISP
} ref_type_t;

typedef struct {
    ref_type_t type;
    union {
        struct {
            IDispatch *disp;
            DISPID id;
        } d;
    } u;
} ref_t;

typedef struct {
    VARIANT *v;
    VARIANT store;
    BOOL owned;
} variant_val_t;

static HRESULT lookup_identifier(exec_ctx_t *ctx, BSTR name, ref_t *ref)
{
    named_item_t *item;
    DISPID id;
    HRESULT hres;

    LIST_FOR_EACH_ENTRY(item, &ctx->script->named_items, named_item_t, entry) {
        if(item->flags & SCRIPTITEM_GLOBALMEMBERS) {
            hres = disp_get_id(item->disp, name, &id);
            if(SUCCEEDED(hres)) {
                ref->type = REF_DISP;
                ref->u.d.disp = item->disp;
                ref->u.d.id = id;
                return S_OK;
            }
        }
    }

    if(!ctx->func->code_ctx->option_explicit)
        FIXME("create an attempt to set\n");

    ref->type = REF_NONE;
    return S_OK;
}

static inline VARIANT *stack_pop(exec_ctx_t *ctx)
{
    assert(ctx->top);
    return ctx->stack + --ctx->top;
}

static HRESULT stack_push(exec_ctx_t *ctx, VARIANT *v)
{
    if(ctx->stack_size == ctx->top) {
        VARIANT *new_stack;

        new_stack = heap_realloc(ctx->stack, ctx->stack_size*2);
        if(!new_stack) {
            VariantClear(v);
            return E_OUTOFMEMORY;
        }

        ctx->stack = new_stack;
        ctx->stack_size *= 2;
    }

    ctx->stack[ctx->top++] = *v;
    return S_OK;
}

static void stack_popn(exec_ctx_t *ctx, unsigned n)
{
    while(n--)
        VariantClear(stack_pop(ctx));
}

static HRESULT stack_pop_val(exec_ctx_t *ctx, variant_val_t *v)
{
    VARIANT *var;

    var = stack_pop(ctx);

    if(V_VT(var) == (VT_BYREF|VT_VARIANT)) {
        v->owned = FALSE;
        var = V_VARIANTREF(var);
    }else {
        v->owned = TRUE;
    }

    if(V_VT(var) == VT_DISPATCH) {
        FIXME("got dispatch - get its default value\n");
        return E_NOTIMPL;
    }else {
        v->v = var;
    }

    return S_OK;
}

static inline void release_val(variant_val_t *v)
{
    if(v->owned)
        VariantClear(v->v);
}

static void vbstack_to_dp(exec_ctx_t *ctx, unsigned arg_cnt, DISPPARAMS *dp)
{
    dp->cArgs = arg_cnt;
    dp->rgdispidNamedArgs = NULL;
    dp->cNamedArgs = 0;

    if(arg_cnt) {
        VARIANT tmp;
        unsigned i;

        assert(ctx->top >= arg_cnt);

        for(i=1; i*2 <= arg_cnt; i++) {
            tmp = ctx->stack[ctx->top-i];
            ctx->stack[ctx->top-i] = ctx->stack[ctx->top-arg_cnt+i-1];
            ctx->stack[ctx->top-arg_cnt+i-1] = tmp;
        }

        dp->rgvarg = ctx->stack + ctx->top-arg_cnt;
    }else {
        dp->rgvarg = NULL;
    }
}

static HRESULT interp_icallv(exec_ctx_t *ctx)
{
    BSTR identifier = ctx->instr->arg1.bstr;
    const unsigned arg_cnt = ctx->instr->arg2.uint;
    ref_t ref = {0};
    DISPPARAMS dp;
    HRESULT hres;

    TRACE("\n");

    hres = lookup_identifier(ctx, identifier, &ref);
    if(FAILED(hres))
        return hres;

    vbstack_to_dp(ctx, arg_cnt, &dp);

    switch(ref.type) {
    case REF_DISP:
        hres = disp_call(ctx->script, ref.u.d.disp, ref.u.d.id, &dp, NULL);
        if(FAILED(hres))
            return hres;
        break;
    default:
        FIXME("%s not found\n", debugstr_w(identifier));
        return DISP_E_UNKNOWNNAME;
    }

    stack_popn(ctx, arg_cnt);
    return S_OK;
}

static HRESULT interp_ret(exec_ctx_t *ctx)
{
    TRACE("\n");

    ctx->instr = NULL;
    return S_OK;
}

static HRESULT interp_bool(exec_ctx_t *ctx)
{
    const VARIANT_BOOL arg = ctx->instr->arg1.lng;
    VARIANT v;

    TRACE("%s\n", arg ? "true" : "false");

    V_VT(&v) = VT_BOOL;
    V_BOOL(&v) = arg;
    return stack_push(ctx, &v);
}

static HRESULT interp_string(exec_ctx_t *ctx)
{
    VARIANT v;

    TRACE("\n");

    V_VT(&v) = VT_BSTR;
    V_BSTR(&v) = SysAllocString(ctx->instr->arg1.str);
    if(!V_BSTR(&v))
        return E_OUTOFMEMORY;

    return stack_push(ctx, &v);
}

static HRESULT interp_not(exec_ctx_t *ctx)
{
    variant_val_t val;
    VARIANT v;
    HRESULT hres;

    TRACE("\n");

    hres = stack_pop_val(ctx, &val);
    if(FAILED(hres))
        return hres;

    hres = VarNot(val.v, &v);
    release_val(&val);
    if(FAILED(hres))
        return hres;

    return stack_push(ctx, &v);
}

static const instr_func_t op_funcs[] = {
#define X(x,n,a,b) interp_ ## x,
OP_LIST
#undef X
};

static const unsigned op_move[] = {
#define X(x,n,a,b) n,
OP_LIST
#undef X
};

HRESULT exec_script(script_ctx_t *ctx, function_t *func)
{
    exec_ctx_t exec;
    vbsop_t op;
    HRESULT hres = S_OK;

    exec.stack_size = 16;
    exec.top = 0;
    exec.stack = heap_alloc(exec.stack_size * sizeof(VARIANT));
    if(!exec.stack)
        return E_OUTOFMEMORY;

    exec.code = func->code_ctx;
    exec.instr = exec.code->instrs + func->code_off;
    exec.script = ctx;
    exec.func = func;

    while(exec.instr) {
        op = exec.instr->op;
        hres = op_funcs[op](&exec);
        if(FAILED(hres)) {
            FIXME("Failed %08x\n", hres);
            stack_popn(&exec, exec.top);
            break;
        }

        exec.instr += op_move[op];
    }

    assert(!exec.top);
    heap_free(exec.stack);

    return hres;
}
