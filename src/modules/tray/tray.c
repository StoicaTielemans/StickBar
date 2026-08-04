#include "modules/tray/tray.h"

#include <linux/input-event-codes.h> // BTN_LEFT

#include "config/config.h"
#include "dbus/sni_registry.h"
#include "modules/tray/tray_context_menu.h"
#include "render/module_box.h"

// How many different Output pointers we keep click-hit-rect state for -- sized
// generously (this machine has 2 monitors), matching modules/workspaces.c's
// MAX_TRACKED_OUTPUTS convention.
#define MAX_TRACKED_OUTPUTS 8

// Mirrors dbus/sni_registry.h's MAX_TRAY_ITEMS, but that constant isn't part of
// sni_registry.h's public surface (the registry's own storage bound shouldn't
// leak into every consumer) -- redeclared locally at the same value, since a
// rendered row can never have more rects than the registry has items regardless.
#define MAX_TRAY_ITEMS_HINT 32

typedef struct {
    double x, y, w, h;
    SniItem *item;
} IconRect;

typedef struct {
    Output *output; // NULL if this slot is unused
    IconRect rects[MAX_TRAY_ITEMS_HINT];
    int rect_count;
} OutputTrayState;

static OutputTrayState g_tray_states[MAX_TRACKED_OUTPUTS];

static OutputTrayState *state_for_output(Output *output) {
    OutputTrayState *free_slot = NULL;
    for (int i = 0; i < MAX_TRACKED_OUTPUTS; i++) {
        if (g_tray_states[i].output == output) {
            return &g_tray_states[i];
        }
        if (!free_slot && g_tray_states[i].output == NULL) {
            free_slot = &g_tray_states[i];
        }
    }
    if (free_slot) {
        free_slot->output = output;
        free_slot->rect_count = 0;
    }
    return free_slot;
}

double tray_render(cairo_t *cr, Output *output, double right_edge_x, double box_top, double box_height) {
    OutputTrayState *state = state_for_output(output);
    if (state) {
        state->rect_count = 0;
    }

    int count = sni_registry_count();
    if (count == 0) {
        return right_edge_x; // zero width when empty, matches Tray.qml's `visible: count > 0`
    }

    double icon_size = (double)BAR.height;
    double total_width = (double)count * icon_size + (double)(count - 1) * BAR.item_spacing;
    double start_x = right_edge_x - total_width;

    double cursor_x = start_x;
    for (int i = 0; i < count; i++) {
        SniItem *item = sni_registry_get(i);
        double icon_y = box_top + (box_height - icon_size) / 2.0;

        cairo_surface_t *icon = sni_item_icon_surface(item);
        if (icon) {
            // Already pre-scaled to BAR.height x BAR.height at conversion time
            // (see sni_icon_convert()), so this is a plain blit -- no further
            // scaling needed here.
            cairo_save(cr);
            cairo_set_source_surface(cr, icon, cursor_x, icon_y);
            cairo_paint(cr);
            cairo_restore(cr);
        } else {
            // No IconPixmap was advertised (IconName-only items aren't
            // supported in this pass -- see the project's known limitations).
            // The reference QML's IconImage has no equivalent "missing icon"
            // fallback to mirror here, so this is an intentional, minimal
            // addition: a muted placeholder square rather than a blank gap.
            module_box_draw_rect(cr, cursor_x, icon_y, icon_size, icon_size,
                                  COLORS.overlay0, (Color){ 0, 0, 0, 0 }, 0.0);
        }

        if (state && state->rect_count < MAX_TRAY_ITEMS_HINT) {
            state->rects[state->rect_count++] = (IconRect){
                .x = cursor_x, .y = icon_y, .w = icon_size, .h = icon_size, .item = item,
            };
        }

        cursor_x += icon_size + BAR.item_spacing;
    }

    // One item-spacing gap before whatever right-region widget sits further
    // left (the clock) -- omitted entirely in the count==0 early return above,
    // since there's nothing to space away from when the tray draws nothing.
    return start_x - BAR.item_spacing;
}

void tray_handle_click(Output *output, double x, double y, uint32_t button) {
    OutputTrayState *state = NULL;
    for (int i = 0; i < MAX_TRACKED_OUTPUTS; i++) {
        if (g_tray_states[i].output == output) {
            state = &g_tray_states[i];
            break;
        }
    }
    if (!state) {
        return;
    }

    for (int i = 0; i < state->rect_count; i++) {
        IconRect *r = &state->rects[i];
        if (x < r->x || x >= r->x + r->w || y < r->y || y >= r->y + r->h) {
            continue;
        }

        // Re-reading the reference Tray.qml's actual onClicked handler matters
        // here: if the item has a menu, *any* click (left or right) opens it --
        // Activate() only ever fires when the item has *no* menu at all, and
        // only then on left-click. Menu presence overrides button entirely;
        // this is not "left activates, right opens the menu" as one might
        // otherwise assume by convention.
        if (sni_item_has_menu(r->item)) {
            tray_context_menu_open(output, r->item, r->x, r->y, r->w, r->h);
        } else if (button == BTN_LEFT) {
            sni_item_activate(r->item);
        }
        return;
    }
}
