#include "modules/workspaces/workspaces.h"

#include <cJSON.h>
#include <stdbool.h>
#include <stdio.h>

#include "config/config.h"
#include "config/glyphs.h"
#include "hyprland/dispatch.h"
#include "hyprland/ipc_request.h"
#include "render/module_box.h"

// Each output owns a fixed 5-wide workspace id range by binding order (see
// context.h's Output.index): output 0 -> ids 1-5, output 1 -> ids 6-10, etc. --
// so at most 5 pills can ever be shown per output.
#define MAX_PILLS_PER_OUTPUT 5

// Generous upper bound on distinct occupied workspace ids Hyprland might report
// across all outputs -- comfortably above what any real setup would use, so a
// legitimately large workspace count doesn't silently get truncated.
#define MAX_OCCUPIED_IDS 64

// How many different Output pointers we keep click-hit-rect state for. In
// practice this machine has 2 monitors; this is sized well above that rather than
// tied to it.
#define MAX_TRACKED_OUTPUTS 8

typedef struct {
    double x, y, w, h;
    int workspace_id;
} PillRect;

typedef struct {
    Output *output; // NULL if this slot is unused
    PillRect rects[MAX_PILLS_PER_OUTPUT];
    int rect_count;
} OutputPillState;

static OutputPillState g_pill_states[MAX_TRACKED_OUTPUTS];

// Snapshot of Hyprland's workspace state, populated by workspaces_refresh() and
// read by workspaces_render(). Deliberately just two flat arrays rather than a
// richer model -- all this module needs is "which ids have windows" and "which id
// is focused".
static int g_occupied_ids[MAX_OCCUPIED_IDS];
static int g_occupied_count = 0;
static int g_focused_id = -1;

static bool is_occupied(int id) {
    for (int i = 0; i < g_occupied_count; i++) {
        if (g_occupied_ids[i] == id) {
            return true;
        }
    }
    return false;
}

void workspaces_refresh(void) {
    g_occupied_count = 0;
    g_focused_id = -1;

    cJSON *workspaces = hyprland_ipc_request_json("workspaces");
    if (workspaces) {
        cJSON *ws;
        cJSON_ArrayForEach(ws, workspaces) {
            cJSON *id_field = cJSON_GetObjectItemCaseSensitive(ws, "id");
            cJSON *windows_field = cJSON_GetObjectItemCaseSensitive(ws, "windows");
            // Hyprland already reports each workspace's window count directly, so
            // "occupied" is just windows > 0 -- no separate query against the
            // client/window list is needed.
            if (cJSON_IsNumber(id_field) && cJSON_IsNumber(windows_field) && windows_field->valueint > 0) {
                if (g_occupied_count < MAX_OCCUPIED_IDS) {
                    g_occupied_ids[g_occupied_count++] = id_field->valueint;
                }
            }
        }
        cJSON_Delete(workspaces);
    }

    cJSON *active = hyprland_ipc_request_json("activeworkspace");
    if (active) {
        cJSON *id_field = cJSON_GetObjectItemCaseSensitive(active, "id");
        if (cJSON_IsNumber(id_field)) {
            g_focused_id = id_field->valueint;
        }
        cJSON_Delete(active);
    }
}

// Finds (or lazily allocates) this output's hit-rect tracking slot.
static OutputPillState *state_for_output(Output *output) {
    OutputPillState *free_slot = NULL;
    for (int i = 0; i < MAX_TRACKED_OUTPUTS; i++) {
        if (g_pill_states[i].output == output) {
            return &g_pill_states[i];
        }
        if (!free_slot && g_pill_states[i].output == NULL) {
            free_slot = &g_pill_states[i];
        }
    }
    if (free_slot) {
        free_slot->output = output;
        free_slot->rect_count = 0;
    }
    return free_slot; // NULL only if MAX_TRACKED_OUTPUTS is somehow exceeded
}

double workspaces_render(cairo_t *cr, Output *output, double x, double box_top, double box_height) {
    OutputPillState *state = state_for_output(output);

    int ws_start = output->index * MAX_PILLS_PER_OUTPUT + 1;
    int ws_end = ws_start + MAX_PILLS_PER_OUTPUT - 1;

    double cursor_x = x;
    int rect_count = 0;

    // ws_start..ws_end is already ascending, so filtering it in place produces the
    // sorted "occupied ∪ focused" set directly -- mirrors HyprlandWindows.qml's
    // Set-then-sort logic without needing an actual sort step here.
    for (int id = ws_start; id <= ws_end; id++) {
        bool focused = (id == g_focused_id);
        if (!focused && !is_occupied(id)) {
            continue;
        }

        uint32_t kanji_cp = kanji_for_workspace(id);
        // 16 comfortably fits either a 3-byte UTF-8 kanji glyph + nul, or the
        // numeric fallback's worst case (a negative int, up to 11 digits + sign + nul).
        char label[16];
        if (kanji_cp) {
            glyph_utf8(kanji_cp, label);
        } else {
            snprintf(label, sizeof(label), "%d", id);
        }

        double text_w, text_h;
        module_box_measure_text(label, FONT.sm, true, &text_w, &text_h);

        // Matches HyprlandWindows.qml's pill sizing (glyph size + 14px horizontal /
        // 6px vertical padding) and its own one-off 4px border width -- both
        // intentionally not shared via config.h/BAR since they're this widget's own
        // tuning, same convention the reference project uses.
        double pill_w = text_w + 14.0;
        double pill_h = text_h + 6.0;
        double pill_y = box_top + (box_height - pill_h) / 2.0;

        Color fill, border, text_color;
        if (focused) {
            // The focused pill is visually "inverted" versus an occupied-but-
            // unfocused one: transparent fill, pink border + pink text.
            fill = (Color){ .r = 0, .g = 0, .b = 0, .a = 0 };
            border = COLORS.pink;
            text_color = COLORS.pink;
        } else {
            fill = COLORS.pink;
            border = COLORS.pink;
            text_color = COLORS.base;
        }

        module_box_draw_rect(cr, cursor_x, pill_y, pill_w, pill_h, fill, border, 4.0);
        module_box_draw_text_centered(cr, cursor_x, pill_y, pill_w, pill_h, label, FONT.sm, true, text_color);

        if (state && rect_count < MAX_PILLS_PER_OUTPUT) {
            state->rects[rect_count++] = (PillRect){
                .x = cursor_x, .y = pill_y, .w = pill_w, .h = pill_h, .workspace_id = id,
            };
        }

        cursor_x += pill_w + BAR.item_spacing;
    }

    if (state) {
        state->rect_count = rect_count;
    }

    return cursor_x;
}

void workspaces_handle_click(Output *output, double x, double y) {
    OutputPillState *state = NULL;
    for (int i = 0; i < MAX_TRACKED_OUTPUTS; i++) {
        if (g_pill_states[i].output == output) {
            state = &g_pill_states[i];
            break;
        }
    }
    if (!state) {
        return;
    }

    for (int i = 0; i < state->rect_count; i++) {
        PillRect *r = &state->rects[i];
        if (x >= r->x && x < r->x + r->w && y >= r->y && y < r->y + r->h) {
            hyprland_dispatch_focus_workspace(r->workspace_id);
            return;
        }
    }
}
