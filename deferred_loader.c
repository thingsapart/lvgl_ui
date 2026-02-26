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
    bool                async_pending;  /* true → an async apply is already queued */
    struct DeferredParentReg* prev;
    struct DeferredParentReg* next;
} DeferredParentReg;

/* Global doubly-linked list of all registered parents. */
static DeferredParentReg* s_parents = NULL;

/* Context passed to lv_async_call.
 * Stores only the raw parent pointer so the callback can look up the preg
 * safely.  If the parent is deleted before the async fires, free_parent_reg
 * removes it from s_parents and the callback gets NULL from find_parent_reg.
 * Never store the preg* itself here: it may be freed before the async runs. */
typedef struct {
    lv_obj_t* parent;
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
 * This avoids crashes caused by creating LVGL objects mid-scroll-animation.
 *
 * Safety guarantees:
 *  - ctx->parent is used only as a key into s_parents.  If the parent was
 *    deleted while the async was pending, free_parent_reg has already removed
 *    the entry from the list, so find_parent_reg returns NULL and we bail.
 *  - async_pending is cleared before doing any work so a VALUE_CHANGED that
 *    fires during create_fn will schedule a fresh async rather than being
 *    silently dropped.
 *  - active_child is re-queried here (not at schedule time) so we always act
 *    on the tile/tab that is actually active when LVGL is quiescent, even if
 *    several VALUE_CHANGED events fired during the scroll animation.
 */
static void apply_active_child_async(void* user_data)
{
    ApplyAsyncCtx*     ctx  = (ApplyAsyncCtx*)user_data;
    DeferredParentReg* preg = find_parent_reg(ctx->parent);
    free(ctx);

    if (!preg) return; /* parent was deleted while async was pending — safe to skip */

    /* Clear the flag BEFORE applying so any VALUE_CHANGED that fires during
     * create_fn can schedule a new async. */
    preg->async_pending = false;

    lv_obj_t* active_child = get_active_child(preg->parent);
    if (!active_child && preg->first_child) {
        active_child = preg->first_child->child; /* tileview init fallback */
    }
    if (active_child) {
        apply_active_child(preg, active_child);
    }
}

/* Schedule an async apply for parent, unless one is already in flight. */
static void schedule_async(DeferredParentReg* preg)
{
    if (preg->async_pending) return; /* coalesce: one async is enough */
    ApplyAsyncCtx* ctx = malloc(sizeof(ApplyAsyncCtx));
    if (!ctx) {
        /* OOM fallback: run synchronously — caller must be outside draw cycle. */
        lv_obj_t* active_child = get_active_child(preg->parent);
        if (!active_child && preg->first_child) active_child = preg->first_child->child;
        if (active_child) apply_active_child(preg, active_child);
        return;
    }
    ctx->parent = preg->parent;
    preg->async_pending = true;
    lv_async_call(apply_active_child_async, ctx);
}

/**
 * LVGL event handler installed on the scroll-container parent.
 */
static void deferred_loader_event_cb(lv_event_t* e)
{
    lv_obj_t*        parent = lv_event_get_target_obj(e);
    lv_event_code_t  code   = lv_event_get_code(e);

    DeferredParentReg* preg = find_parent_reg(parent);
    if (!preg) return;

    if (code == LV_EVENT_VALUE_CHANGED) {
        schedule_async(preg);
    } else if (code == LV_EVENT_DELETE) {
        /* free_parent_reg removes the entry from s_parents before freeing.
         * Any pending async will fire, find_parent_reg returns NULL, and bail. */
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
        preg->async_pending = false;
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

    /* Schedule initial population via lv_async_call so that it executes after
     * create_ui (and the full lv_obj tree construction) has returned. */
    schedule_async(preg);
}
