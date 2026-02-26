#include "deferred_loader.h"
#include "utils.h"
#include <stdlib.h>

/* lv_async_call lets us schedule work to run after the current event/draw
 * cycle completes.  This is required when creating or deleting LVGL objects
 * from inside a VALUE_CHANGED handler that fires mid-scroll-animation,
 * otherwise LVGL's in-progress draw pass crashes. */
#include "misc/lv_async.h"

/* ---------------------------------------------------------------------------
 * Internal data structures
 * ---------------------------------------------------------------------------
 * One DeferredParentReg per scroll-container (tabview / tileview / …).
 * Each owns a singly-linked list of DeferredChildReg entries, one per
 * deferred child panel, in the order they were registered.
 * --------------------------------------------------------------------------- */

typedef struct DeferredChildReg {
    lv_obj_t*             child;
    deferred_create_fn_t  create_fn;
    bool                  populated;
    struct DeferredChildReg* next;
} DeferredChildReg;

typedef struct DeferredParentReg {
    lv_obj_t*           parent;
    DeferredChildReg*   first_child;
    struct DeferredParentReg* prev;
    struct DeferredParentReg* next;
} DeferredParentReg;

/* Global doubly-linked list of all registered parents. */
static DeferredParentReg* s_parents = NULL;

/* Context passed to lv_async_call so the deferred work knows which parent
 * and which active child to switch to. */
typedef struct {
    DeferredParentReg* preg;
    lv_obj_t*          active_child;
} ApplyAsyncCtx;

/* ---------------------------------------------------------------------------
 * Internal helpers
 * --------------------------------------------------------------------------- */

static DeferredParentReg* find_parent_reg(lv_obj_t* parent)
{
    for (DeferredParentReg* p = s_parents; p; p = p->next) {
        if (p->parent == parent) return p;
    }
    return NULL;
}

static void free_parent_reg(DeferredParentReg* entry)
{
    /* Free child list */
    DeferredChildReg* c = entry->first_child;
    while (c) {
        DeferredChildReg* next = c->next;
        free(c);
        c = next;
    }
    /* Unlink from global list */
    if (entry->prev) entry->prev->next = entry->next;
    else             s_parents = entry->next;
    if (entry->next) entry->next->prev = entry->prev;
    free(entry);
}

/**
 * Return the child lv_obj_t* that is currently the active / visible page.
 * Returns NULL if the widget class is not (yet) supported.
 */
static lv_obj_t* get_active_child(lv_obj_t* parent)
{
#if LV_USE_TABVIEW
    if (lv_obj_check_type(parent, &lv_tabview_class)) {
        lv_obj_t* content = lv_tabview_get_content(parent);
        uint32_t  idx     = lv_tabview_get_tab_active(parent);
        return lv_obj_get_child(content, (int32_t)idx);
    }
#endif

#if LV_USE_TILEVIEW
    if (lv_obj_check_type(parent, &lv_tileview_class)) {
        return lv_tileview_get_tile_active(parent);
    }
#endif

    _dprintf(stderr, "[deferred_loader] WARNING: Unsupported parent widget class; "
             "cannot determine active child. Pointer=%p\n", (void*)parent);
    return NULL;
}

/**
 * Core create/destroy logic shared by deferred_loader_init and the event CB.
 * - active_child: the child that should be populated.
 * - All other registered children that are populated will be cleaned.
 */
static void apply_active_child(DeferredParentReg* preg, lv_obj_t* active_child)
{
    for (DeferredChildReg* c = preg->first_child; c; c = c->next) {
        if (c->child == active_child) {
            if (!c->populated) {
                _dprintf(stderr, "[deferred_loader] Creating children for %p\n",
                         (void*)c->child);
                c->create_fn(c->child);
                c->populated = true;
            }
        } else {
            if (c->populated) {
                _dprintf(stderr, "[deferred_loader] Cleaning children for %p\n",
                         (void*)c->child);
                lv_obj_clean(c->child);
                c->populated = false;
            }
        }
    }
}

/**
 * lv_async_call callback: runs apply_active_child outside the event/draw cycle.
 * This avoids crashes that occur when creating LVGL objects mid-scroll-animation.
 */
static void apply_active_child_async(void* user_data)
{
    ApplyAsyncCtx* ctx = (ApplyAsyncCtx*)user_data;
    /* Guard: parent may have been destroyed between schedule and execution. */
    if (find_parent_reg(ctx->preg->parent)) {
        apply_active_child(ctx->preg, ctx->active_child);
    }
    free(ctx);
}

/**
 * LVGL event handler installed on the scroll-container parent.
 * Handles LV_EVENT_VALUE_CHANGED (page switch) and LV_EVENT_DELETE (cleanup).
 *
 * Widget/style creation is NOT done directly here — it is scheduled via
 * lv_async_call so it executes after the current event and draw pass finish.
 * This prevents crashes caused by modifying the object tree while LVGL is
 * mid-way through a scroll animation or draw cycle.
 */
static void deferred_loader_event_cb(lv_event_t* e)
{
    lv_obj_t*        parent = lv_event_get_target_obj(e);
    lv_event_code_t  code   = lv_event_get_code(e);

    DeferredParentReg* preg = find_parent_reg(parent);
    if (!preg) return;

    if (code == LV_EVENT_VALUE_CHANGED) {
        lv_obj_t* active_child = get_active_child(parent);
        if (!active_child) return;

        ApplyAsyncCtx* ctx = malloc(sizeof(ApplyAsyncCtx));
        if (!ctx) {
            /* Fallback: run synchronously if allocation fails. */
            apply_active_child(preg, active_child);
            return;
        }
        ctx->preg         = preg;
        ctx->active_child = active_child;
        lv_async_call(apply_active_child_async, ctx);

    } else if (code == LV_EVENT_DELETE) {
        /* The parent widget is being destroyed; free our bookkeeping. */
        free_parent_reg(preg);
    }
}

/* ---------------------------------------------------------------------------
 * Public API
 * --------------------------------------------------------------------------- */

void deferred_loader_register(lv_obj_t*            scroll_parent,
                               lv_obj_t*            child,
                               deferred_create_fn_t create_fn)
{
    DeferredParentReg* preg = find_parent_reg(scroll_parent);
    if (!preg) {
        preg = malloc(sizeof(DeferredParentReg));
        if (!preg) return;
        preg->parent      = scroll_parent;
        preg->first_child = NULL;
        preg->prev        = NULL;
        preg->next        = s_parents;
        if (s_parents) s_parents->prev = preg;
        s_parents = preg;
    }

    DeferredChildReg* creg = malloc(sizeof(DeferredChildReg));
    if (!creg) return;
    creg->child      = child;
    creg->create_fn  = create_fn;
    creg->populated  = false;
    creg->next       = NULL;

    /* Append to tail of child list to preserve registration order. */
    if (!preg->first_child) {
        preg->first_child = creg;
    } else {
        DeferredChildReg* tail = preg->first_child;
        while (tail->next) tail = tail->next;
        tail->next = creg;
    }
}

void deferred_loader_init(lv_obj_t* scroll_parent)
{
    DeferredParentReg* preg = find_parent_reg(scroll_parent);
    if (!preg) return;

    /* Install event callbacks. LVGL allows multiple callbacks per object; we
     * register separately for the two codes we care about. */
    lv_obj_add_event_cb(scroll_parent, deferred_loader_event_cb,
                        LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(scroll_parent, deferred_loader_event_cb,
                        LV_EVENT_DELETE, NULL);

    /* Populate active child immediately so the initial view is not blank.
     *
     * Some widgets (e.g. lv_tileview) do not report a valid active child
     * until the user navigates for the first time.  In that case we fall back
     * to the first registered child so the initial page is never blank. */
    lv_obj_t* active_child = get_active_child(scroll_parent);
    if (!active_child && preg->first_child) {
        _dprintf(stderr, "[deferred_loader] get_active_child returned NULL at init; "
                 "falling back to first registered child.\n");
        active_child = preg->first_child->child;
    }
    if (active_child) {
        apply_active_child(preg, active_child);
    }
}
