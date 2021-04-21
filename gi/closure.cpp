/* -*- mode: C++; c-basic-offset: 4; indent-tabs-mode: nil; -*- */
// SPDX-License-Identifier: MIT OR LGPL-2.0-or-later
// SPDX-FileCopyrightText: 2008 litl, LLC

#include <config.h>

#include <glib.h>

#include <new>

#include <js/RootingAPI.h>
#include <js/TypeDecls.h>
#include <js/ValueArray.h>
#include <jsapi.h>  // for JS_IsExceptionPending, Call, JS_Get...

#include "gi/closure.h"
#include "gjs/context-private.h"
#include "gjs/jsapi-util-root.h"
#include "gjs/jsapi-util.h"
#include "gjs/mem-private.h"
#include "util/log.h"

struct Closure {
    JSContext *context;
    GjsMaybeOwned<JSFunction*> func;
};

struct GjsClosure {
    GClosure base;

    /* We need a separate object to be able to call
       the C++ constructor without stomping on the base */
    Closure priv;
};

/*
 * Memory management of closures is "interesting" because we're keeping around
 * a JSContext* and then trying to use it spontaneously from the main loop.
 * I don't think that's really quite kosher, and perhaps the problem is that
 * (in xulrunner) we just need to save a different context.
 *
 * Or maybe the right fix is to create our own context just for this?
 *
 * But for the moment, we save the context that was used to create the closure.
 *
 * Here's the problem: this context can be destroyed. AFTER the
 * context is destroyed, or at least potentially after, the objects in
 * the context's global object may be garbage collected. Remember that
 * JSObject* belong to a runtime, not a context.
 *
 * There is apparently no robust way to track context destruction in
 * SpiderMonkey, because the context can be destroyed without running
 * the garbage collector, and xulrunner takes over the JS_SetContextCallback()
 * callback. So there's no callback for us.
 *
 * So, when we go to use our context, we iterate the contexts in the runtime
 * and see if ours is still in the valid list, and decide to invalidate
 * the closure if it isn't.
 *
 * The closure can thus be destroyed in several cases:
 * - invalidation by unref, e.g. when a signal is disconnected, closure is unref'd
 * - invalidation because we were invoked while the context was dead
 * - invalidation through finalization (we were garbage collected)
 *
 * These don't have to happen in the same order; garbage collection can
 * be either before, or after, context destruction.
 *
 */

static void
invalidate_js_pointers(GjsClosure *gc)
{
    Closure *c;

    c = &gc->priv;

    if (!c->func)
        return;

    c->func.reset();
    c->context = nullptr;

    /* Notify any closure reference holders they
     * may want to drop references.
     */
    g_closure_invalidate(&gc->base);
}

static void global_context_finalized(JS::HandleFunction func [[maybe_unused]],
                                     void* data) {
    GjsClosure *gc = (GjsClosure*) data;
    Closure *c = &gc->priv;

    gjs_debug_closure(
        "Context global object destroy notifier on closure %p which calls "
        "object %p",
        c, c->func.debug_addr());

    if (c->func) {
        g_assert(c->func == func.get());

        invalidate_js_pointers(gc);
    }
}

/* Invalidation is like "dispose" - it is guaranteed to happen at
 * finalize, but may happen before finalize. Normally, g_closure_invalidate()
 * is called when the "target" of the closure becomes invalid, so that the
 * source (the signal connection, say can be removed.) The usage above
 * in invalidate_js_pointers() is typical. Since the target of the closure
 * is under our control, it's unlikely that g_closure_invalidate() will ever
 * be called by anyone else, but in case it ever does, it's slightly better
 * to remove the "keep alive" here rather than in the finalize notifier.
 *
 * Unlike "dispose" invalidation only happens once.
 */
static void closure_invalidated(void*, GClosure* closure) {
    Closure *c;

    c = &((GjsClosure*) closure)->priv;

    GJS_DEC_COUNTER(closure);
    gjs_debug_closure("Invalidating closure %p which calls function %p",
                      closure, c->func.debug_addr());

    if (!c->func) {
        gjs_debug_closure("   (closure %p already dead, nothing to do)",
                          closure);
        return;
    }

    /* The context still exists, remove our destroy notifier. Otherwise we
     * would call the destroy notifier on an already-freed closure.
     *
     * This happens in the normal case, when the closure is
     * invalidated for some reason other than destruction of the
     * JSContext.
     */
    gjs_debug_closure("   (closure %p's context was alive, "
                      "removing our destroy notifier on global object)",
                      closure);

    c->func.reset();
    c->context = nullptr;
}

static void closure_set_invalid(void*, GClosure* closure) {
    Closure *self = &((GjsClosure*) closure)->priv;

    gjs_debug_closure("Invalidating signal closure %p which calls function %p",
                      closure, self->func.debug_addr());

    self->func.prevent_collection();
    self->func.reset();
    self->context = nullptr;

    GJS_DEC_COUNTER(closure);
}

static void closure_finalize(void*, GClosure* closure) {
    Closure *self = &((GjsClosure*) closure)->priv;

    self->~Closure();
}

bool gjs_closure_invoke(GClosure* closure, JS::HandleObject this_obj,
                        const JS::HandleValueArray& args,
                        JS::MutableHandleValue retval) {
    Closure *c;
    JSContext *context;

    c = &((GjsClosure*) closure)->priv;

    if (!c->func) {
        /* We were destroyed; become a no-op */
        c->context = nullptr;
        return false;
    }

    context = c->context;
    JSAutoRealm ar(context, JS_GetFunctionObject(c->func));

    if (gjs_log_exception(context)) {
        gjs_debug_closure("Exception was pending before invoking callback??? "
                          "Not expected - closure %p", closure);
    }

    JS::RootedFunction func(context, c->func);
    if (!JS::Call(context, this_obj, func, args, retval)) {
        /* Exception thrown... */
        gjs_debug_closure(
            "Closure invocation failed (exception should have been thrown) "
            "closure %p function %p",
            closure, c->func.debug_addr());
        return false;
    }

    if (gjs_log_exception_uncaught(context)) {
        gjs_debug_closure("Closure invocation succeeded but an exception was set"
                          " - closure %p", closure);
    }

    GjsContextPrivate* gjs = GjsContextPrivate::from_cx(context);
    gjs->schedule_gc_if_needed();
    return true;
}

bool
gjs_closure_is_valid(GClosure *closure)
{
    Closure *c;

    c = &((GjsClosure*) closure)->priv;

    return !!c->context;
}

JSContext*
gjs_closure_get_context(GClosure *closure)
{
    Closure *c;

    c = &((GjsClosure*) closure)->priv;

    return c->context;
}

JSFunction* gjs_closure_get_callable(GClosure* closure) {
    Closure *c;

    c = &((GjsClosure*) closure)->priv;

    return c->func;
}

void
gjs_closure_trace(GClosure *closure,
                  JSTracer *tracer)
{
    Closure *c;

    c = &((GjsClosure*) closure)->priv;

    if (!c->func)
        return;

    c->func.trace(tracer, "signal connection");
}

GClosure* gjs_closure_new(JSContext* context, JSFunction* callable,
                          const char* description GJS_USED_VERBOSE_GCLOSURE,
                          bool root_function) {
    Closure *c;

    auto* gc = reinterpret_cast<GjsClosure*>(
        g_closure_new_simple(sizeof(GjsClosure), nullptr));
    c = new (&gc->priv) Closure();

    /* The saved context is used for lifetime management, so that the closure will
     * be torn down with the context that created it. The context could be attached to
     * the default context of the runtime using if we wanted the closure to survive
     * the context that created it.
     */
    c->context = context;

    GJS_INC_COUNTER(closure);

    if (root_function) {
        /* Fully manage closure lifetime if so asked */
        c->func.root(context, callable, global_context_finalized, gc);

        g_closure_add_invalidate_notifier(&gc->base, nullptr,
                                          closure_invalidated);
    } else {
        c->func = callable;
        /* Only mark the closure as invalid if memory is managed
           outside (i.e. by object.c for signals) */
        g_closure_add_invalidate_notifier(&gc->base, nullptr,
                                          closure_set_invalid);
    }

    g_closure_add_finalize_notifier(&gc->base, nullptr, closure_finalize);

    gjs_debug_closure("Create closure %p which calls function %p '%s'", gc,
                      c->func.debug_addr(), description);

    return &gc->base;
}
