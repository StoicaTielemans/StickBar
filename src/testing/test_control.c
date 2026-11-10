#include "testing/test_control.h"

#include <cJSON.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input-event-codes.h> // BTN_*
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "audio/device_popup.h"
#include "audio/mixer_popup.h"
#include "audio/pulse_client.h"
#include "bar/popup_surface.h"
#include "eq/eq_state.h"
#include "modules/eq/eq_popup.h"
#include "modules/media/media_popup.h"
#include "modules/powermenu/power_menu.h"
#include "modules/privacy/privacy_info_popup.h"
#include "modules/tray/tray.h"
#include "modules/tray/tray_context_menu.h"
#include "mpris/mpris_client.h"
#include "network/network_client.h"
#include "render/render.h"
#include "sysmon/sysmon_client.h"
#include "visualizer/visualizer.h"
#include "wayland/pointer.h"

static AppContext *g_app = NULL;
static int g_fd = -1;
static char g_buf[4096];
static size_t g_len = 0;

static const char *MODULES[] = {"arch", "workspaces", "cpu",  "gpu",    "mem", "privacy", "tray",
                                "clock", "network",   "eq",   "volume", "mic", "media"};

// ---- Replies -------------------------------------------------------------------

static void reply(cJSON *obj) {
    char *text = cJSON_PrintUnformatted(obj);
    if (text) {
        fputs(text, stdout);
        fputc('\n', stdout);
        fflush(stdout);
        free(text);
    }
    cJSON_Delete(obj);
}

static cJSON *ok_obj(void) {
    cJSON *obj = cJSON_CreateObject();
    cJSON_AddBoolToObject(obj, "ok", true);
    return obj;
}

static void reply_error(const char *error) {
    cJSON *obj = cJSON_CreateObject();
    cJSON_AddBoolToObject(obj, "ok", false);
    cJSON_AddStringToObject(obj, "error", error);
    reply(obj);
}

// ---- Injected input ------------------------------------------------------------

static Output *find_output(const char *name) {
    for (Output *o = g_app->outputs; o; o = o->next) {
        if (strcmp(o->name, name) == 0 || (name[0] >= '0' && name[0] <= '9' && o->index == atoi(name))) {
            return o;
        }
    }
    return NULL;
}

static uint32_t parse_button(const char *name) {
    if (strcmp(name, "right") == 0) return BTN_RIGHT;
    if (strcmp(name, "middle") == 0) return BTN_MIDDLE;
    return BTN_LEFT;
}

// Points the routing state at `surface` (and `output`, for bar surfaces),
// runs `fn`, then puts the real pointer state back -- so an injected event
// never leaves pointer.c believing the cursor is somewhere it isn't.
typedef struct {
    Output *over;
    struct wl_surface *surface;
    double x, y;
} PointerState;

static PointerState aim(struct wl_surface *surface, Output *output, double x, double y) {
    PointerState saved = {g_app->pointer_over, g_app->pointer_over_surface, g_app->pointer_x, g_app->pointer_y};
    g_app->pointer_over_surface = surface;
    g_app->pointer_over = output;
    g_app->pointer_x = x;
    g_app->pointer_y = y;
    return saved;
}

static void restore(PointerState saved) {
    g_app->pointer_over = saved.over;
    g_app->pointer_over_surface = saved.surface;
    g_app->pointer_x = saved.x;
    g_app->pointer_y = saved.y;
}

static struct wl_surface *menu_surface(void) {
    PopupSurface *menu = popup_surface_latest_menu();
    return menu ? popup_surface_wl_surface(menu) : NULL;
}

// ---- Dumps ---------------------------------------------------------------------

static cJSON *dump_mem(void) {
    cJSON *o = ok_obj();
    cJSON_AddNumberToObject(o, "percent", sysmon_mem_percent());
    cJSON_AddNumberToObject(o, "total_gb", sysmon_mem_total_gb());
    cJSON_AddNumberToObject(o, "apps_gb", sysmon_mem_used_gb());
    cJSON_AddNumberToObject(o, "avail_gb", sysmon_mem_avail_gb());
    cJSON_AddNumberToObject(o, "free_gb", sysmon_mem_free_gb());
    cJSON_AddNumberToObject(o, "cache_gb", sysmon_mem_cache_gb());
    cJSON_AddNumberToObject(o, "shared_gb", sysmon_mem_shared_gb());
    cJSON_AddNumberToObject(o, "dirty_mb", sysmon_mem_dirty_mb());
    cJSON_AddNumberToObject(o, "swap_used_gb", sysmon_swap_used_gb());
    cJSON_AddNumberToObject(o, "swap_total_gb", sysmon_swap_total_gb());
    cJSON_AddNumberToObject(o, "swap_percent", sysmon_swap_percent());
    cJSON_AddNumberToObject(o, "history_len", sysmon_mem_history_len());
    cJSON_AddBoolToObject(o, "psi", sysmon_mem_psi_available());
    cJSON *psi = cJSON_AddArrayToObject(o, "psi_some");
    for (int i = 0; i < 3; i++) {
        cJSON_AddItemToArray(psi, cJSON_CreateNumber(sysmon_mem_psi_some(i)));
    }
    cJSON_AddBoolToObject(o, "zram", sysmon_swap_is_zram());
    cJSON_AddStringToObject(o, "zram_algorithm", sysmon_zram_algorithm());
    cJSON_AddNumberToObject(o, "zram_orig_mb", sysmon_zram_orig_mb());
    cJSON_AddNumberToObject(o, "zram_compr_mb", sysmon_zram_compr_mb());
    cJSON_AddNumberToObject(o, "zram_used_mb", sysmon_zram_used_mb());
    cJSON *procs = cJSON_AddArrayToObject(o, "procs");
    for (int i = 0; i < sysmon_mem_process_count(); i++) {
        cJSON *p = cJSON_CreateObject();
        cJSON_AddStringToObject(p, "name", sysmon_mem_process_name(i));
        cJSON_AddNumberToObject(p, "gb", sysmon_mem_process_gb(i));
        cJSON_AddItemToArray(procs, p);
    }
    return o;
}

static cJSON *dump_cpu(void) {
    cJSON *o = ok_obj();
    cJSON_AddStringToObject(o, "model", sysmon_cpu_model());
    cJSON_AddNumberToObject(o, "physical_cores", sysmon_cpu_physical_cores());
    cJSON_AddNumberToObject(o, "threads", sysmon_cpu_core_count());
    cJSON_AddBoolToObject(o, "hybrid", sysmon_cpu_hybrid());
    cJSON_AddNumberToObject(o, "max_ghz", sysmon_cpu_max_ghz());
    cJSON_AddNumberToObject(o, "percent", sysmon_cpu_percent());
    cJSON_AddNumberToObject(o, "temp_c", sysmon_cpu_temp_c());
    cJSON_AddStringToObject(o, "load", sysmon_cpu_load_avg());
    cJSON_AddNumberToObject(o, "tasks_running", sysmon_cpu_tasks_running());
    cJSON_AddNumberToObject(o, "tasks_total", sysmon_cpu_tasks_total());
    cJSON_AddNumberToObject(o, "uptime_s", sysmon_cpu_uptime_s());
    cJSON_AddNumberToObject(o, "history_len", sysmon_cpu_history_len());
    cJSON *eff = cJSON_AddArrayToObject(o, "efficient");
    cJSON *ghz = cJSON_AddArrayToObject(o, "core_ghz");
    for (int i = 0; i < sysmon_cpu_core_count(); i++) {
        cJSON_AddItemToArray(eff, cJSON_CreateBool(sysmon_cpu_core_is_efficient(i)));
        cJSON_AddItemToArray(ghz, cJSON_CreateNumber(sysmon_cpu_core_ghz(i)));
    }
    cJSON_AddBoolToObject(o, "procs_ready", sysmon_cpu_process_ready());
    cJSON *procs = cJSON_AddArrayToObject(o, "procs");
    for (int i = 0; i < sysmon_cpu_process_count(); i++) {
        cJSON *p = cJSON_CreateObject();
        cJSON_AddStringToObject(p, "name", sysmon_cpu_process_name(i));
        cJSON_AddNumberToObject(p, "percent", sysmon_cpu_process_percent(i));
        cJSON_AddItemToArray(procs, p);
    }
    return o;
}

static cJSON *dump_gpu(void) {
    cJSON *o = ok_obj();
    cJSON_AddBoolToObject(o, "available", sysmon_gpu_available());
    cJSON_AddStringToObject(o, "name", sysmon_gpu_name());
    cJSON_AddStringToObject(o, "driver", sysmon_gpu_driver());
    cJSON_AddNumberToObject(o, "percent", sysmon_gpu_percent());
    cJSON_AddNumberToObject(o, "temp_c", sysmon_gpu_temp_c());
    cJSON_AddNumberToObject(o, "mem_used_mb", sysmon_gpu_mem_used_mb());
    cJSON_AddNumberToObject(o, "mem_total_mb", sysmon_gpu_mem_total_mb());
    cJSON_AddNumberToObject(o, "power_w", sysmon_gpu_power_w());
    cJSON_AddNumberToObject(o, "power_limit_w", sysmon_gpu_power_limit_w());
    cJSON_AddStringToObject(o, "pstate", sysmon_gpu_pstate());
    cJSON_AddNumberToObject(o, "history_len", sysmon_gpu_history_len());
    cJSON *procs = cJSON_AddArrayToObject(o, "procs");
    for (int i = 0; i < sysmon_gpu_process_count(); i++) {
        cJSON *p = cJSON_CreateObject();
        cJSON_AddStringToObject(p, "name", sysmon_gpu_process_name(i));
        cJSON_AddNumberToObject(p, "mib", sysmon_gpu_process_mib(i));
        cJSON_AddItemToArray(procs, p);
    }
    return o;
}

static cJSON *dump_net(void) {
    cJSON *o = ok_obj();
    cJSON_AddStringToObject(o, "iface", network_interface_name());
    cJSON_AddBoolToObject(o, "connected", network_connected());
    cJSON_AddBoolToObject(o, "wifi", network_is_wifi());
    cJSON_AddStringToObject(o, "ip", network_ip_address());
    cJSON_AddStringToObject(o, "gateway", network_gateway());
    cJSON_AddStringToObject(o, "mac", network_mac_address());
    cJSON_AddStringToObject(o, "mtu", network_mtu());
    cJSON_AddStringToObject(o, "speed", network_link_speed());
    cJSON_AddNumberToObject(o, "rx_rate", network_rx_rate_bytes_per_sec());
    cJSON_AddNumberToObject(o, "tx_rate", network_tx_rate_bytes_per_sec());
    cJSON_AddNumberToObject(o, "rx_total", network_rx_total_bytes());
    cJSON_AddNumberToObject(o, "tx_total", network_tx_total_bytes());
    cJSON_AddNumberToObject(o, "history_len", network_history_len());
    cJSON *dns = cJSON_AddArrayToObject(o, "dns");
    for (int i = 0; i < network_dns_count(); i++) {
        cJSON_AddItemToArray(dns, cJSON_CreateString(network_dns_get(i)));
    }
    return o;
}

static cJSON *node_json(PulseNode *node) {
    cJSON *n = cJSON_CreateObject();
    cJSON_AddStringToObject(n, "description", pulse_node_description(node));
    cJSON_AddNumberToObject(n, "volume", pulse_node_volume_percent(node));
    cJSON_AddBoolToObject(n, "muted", pulse_node_muted(node));
    cJSON_AddBoolToObject(n, "default", pulse_node_is_default(node));
    cJSON_AddStringToObject(n, "state", pulse_node_state(node));
    cJSON_AddStringToObject(n, "bus", pulse_node_bus(node));
    cJSON_AddNumberToObject(n, "rate", pulse_node_rate(node));
    cJSON_AddNumberToObject(n, "channels", pulse_node_channels(node));
    cJSON_AddBoolToObject(n, "monitor", pulse_node_is_monitor(node));
    return n;
}

static cJSON *dump_audio(void) {
    cJSON *o = ok_obj();
    cJSON *sinks = cJSON_AddArrayToObject(o, "sinks");
    for (int i = 0; i < pulse_sink_count(); i++) {
        PulseNode *node = pulse_sink_get(i);
        if (node) cJSON_AddItemToArray(sinks, node_json(node));
    }
    cJSON *sources = cJSON_AddArrayToObject(o, "sources");
    for (int i = 0; i < pulse_source_count(); i++) {
        PulseNode *node = pulse_source_get(i);
        if (node) cJSON_AddItemToArray(sources, node_json(node));
    }
    cJSON *streams = cJSON_AddArrayToObject(o, "streams");
    for (int i = 0; i < pulse_stream_count(); i++) {
        PulseStream *s = pulse_stream_get(i);
        if (!s) continue;
        cJSON *j = cJSON_CreateObject();
        cJSON_AddStringToObject(j, "name", pulse_stream_name(s));
        cJSON_AddNumberToObject(j, "volume", pulse_stream_volume_percent(s));
        cJSON_AddBoolToObject(j, "muted", pulse_stream_muted(s));
        cJSON_AddItemToArray(streams, j);
    }
    return o;
}

static cJSON *dump_media(void) {
    cJSON *o = ok_obj();
    cJSON *players = cJSON_AddArrayToObject(o, "players");
    for (int i = 0; i < mpris_player_count(); i++) {
        MprisPlayer *p = mpris_player_get(i);
        char app[256];
        mpris_player_app_name(p, app, sizeof(app));
        cJSON *j = cJSON_CreateObject();
        cJSON_AddStringToObject(j, "app", app);
        cJSON_AddStringToObject(j, "title", mpris_player_title(p));
        cJSON_AddStringToObject(j, "artist", mpris_player_artist(p));
        cJSON_AddStringToObject(j, "album", mpris_player_album(p));
        cJSON_AddStringToObject(j, "status", mpris_player_status(p));
        cJSON_AddNumberToObject(j, "length_us", (double)mpris_player_length_us(p));
        cJSON_AddItemToArray(players, j);
    }
    return o;
}

static cJSON *dump_eq(void) {
    cJSON *o = ok_obj();
    cJSON_AddBoolToObject(o, "available", eq_state_available());
    cJSON_AddStringToObject(o, "profile", eq_state_profile());
    cJSON_AddStringToObject(o, "gain", eq_state_gain());
    cJSON_AddNumberToObject(o, "preamp", eq_state_preamp());
    cJSON_AddBoolToObject(o, "enabled", eq_state_enabled());
    cJSON_AddStringToObject(o, "pending", eq_state_pending_profile());
    cJSON_AddNumberToObject(o, "curve_points", eq_state_curve_count());
    return o;
}

static cJSON *levels_array(const float *levels) {
    cJSON *a = cJSON_CreateArray();
    for (int i = 0; i < SPECTRUM_BAR_COUNT; i++) {
        cJSON_AddItemToArray(a, cJSON_CreateNumber(levels[i]));
    }
    return a;
}

// Per media-dropdown card with a graph: its player, slot, captured stream,
// geometry, and the slot's current levels.
static cJSON *dump_visualizer(void) {
    cJSON *o = ok_obj();
    cJSON_AddBoolToObject(o, "available", visualizer_available());
    cJSON *graphs = cJSON_AddArrayToObject(o, "graphs");
    MediaPopupTestGraph g;
    for (int card = 0; media_popup_test_card_count() > card; card++) {
        if (!media_popup_test_graph(card, &g)) {
            continue;
        }
        cJSON *j = cJSON_CreateObject();
        cJSON_AddStringToObject(j, "app", g.app);
        cJSON_AddNumberToObject(j, "slot", g.slot);
        cJSON_AddNumberToObject(j, "target", g.target);
        cJSON_AddNumberToObject(j, "panel_x", g.panel_x);
        cJSON_AddNumberToObject(j, "panel_y", g.panel_y);
        cJSON_AddNumberToObject(j, "panel_w", g.panel_w);
        cJSON_AddNumberToObject(j, "panel_h", g.panel_h);
        cJSON_AddNumberToObject(j, "graph_x", g.graph_x);
        cJSON_AddNumberToObject(j, "top_y", g.top_y);
        cJSON_AddNumberToObject(j, "bottom_y", g.bottom_y);
        cJSON_AddNumberToObject(j, "half_h", g.half_h);
        cJSON_AddNumberToObject(j, "rows", g.rows);
        cJSON_AddItemToObject(j, "levels_l", levels_array(visualizer_levels(g.slot, VISUALIZER_LEFT)));
        cJSON_AddItemToObject(j, "levels_r", levels_array(visualizer_levels(g.slot, VISUALIZER_RIGHT)));
        cJSON_AddItemToArray(graphs, j);
    }
    // Every slot, shown or not -- a closed dropdown must leave them all at 0.
    cJSON *slots = cJSON_AddArrayToObject(o, "slots");
    for (int slot = 0; slot < VISUALIZER_MAX_SOURCES; slot++) {
        cJSON *j = cJSON_CreateObject();
        cJSON_AddItemToObject(j, "levels_l", levels_array(visualizer_levels(slot, VISUALIZER_LEFT)));
        cJSON_AddItemToObject(j, "levels_r", levels_array(visualizer_levels(slot, VISUALIZER_RIGHT)));
        cJSON_AddItemToArray(slots, j);
    }
    return o;
}

// ---- Commands -----------------------------------------------------------------
// Coordinates are surface-local pixels: bar-surface for `click`, popup-local
// for the menu-* commands (what `target` returns).

static void cmd_rects(const char *output_name) {
    Output *output = find_output(output_name);
    if (!output) {
        reply_error("no such output");
        return;
    }
    cJSON *o = ok_obj();
    cJSON *rects = cJSON_AddObjectToObject(o, "rects");
    for (size_t i = 0; i < sizeof(MODULES) / sizeof(MODULES[0]); i++) {
        double x, y, w, h;
        if (render_module_rect(output, MODULES[i], &x, &y, &w, &h)) {
            cJSON *r = cJSON_CreateArray();
            cJSON_AddItemToArray(r, cJSON_CreateNumber(x));
            cJSON_AddItemToArray(r, cJSON_CreateNumber(y));
            cJSON_AddItemToArray(r, cJSON_CreateNumber(w));
            cJSON_AddItemToArray(r, cJSON_CreateNumber(h));
            cJSON_AddItemToObject(rects, MODULES[i], r);
        }
    }
    reply(o);
}

static void cmd_outputs(void) {
    cJSON *o = ok_obj();
    cJSON *list = cJSON_AddArrayToObject(o, "outputs");
    for (Output *out = g_app->outputs; out; out = out->next) {
        cJSON *j = cJSON_CreateObject();
        cJSON_AddStringToObject(j, "name", out->name);
        cJSON_AddNumberToObject(j, "index", out->index);
        cJSON_AddNumberToObject(j, "width", out->width);
        cJSON_AddNumberToObject(j, "height", out->height);
        cJSON_AddBoolToObject(j, "has_bar", out->bar_surface != NULL);
        cJSON_AddItemToArray(list, j);
    }
    reply(o);
}

static void cmd_menu(void) {
    cJSON *o = ok_obj();
    PopupSurface *menu = popup_surface_latest_menu();
    cJSON_AddBoolToObject(o, "open", menu != NULL);
    if (menu) {
        int w, h;
        popup_surface_size(menu, &w, &h);
        cJSON_AddNumberToObject(o, "width", w);
        cJSON_AddNumberToObject(o, "height", h);
    }
    cJSON_AddNumberToObject(o, "catchers", popup_surface_catcher_count());
    cJSON_AddNumberToObject(o, "device_cards", device_popup_test_card_count());
    cJSON_AddNumberToObject(o, "mixer_rows", mixer_popup_test_row_count());
    cJSON_AddNumberToObject(o, "media_cards", media_popup_test_card_count());
    cJSON_AddNumberToObject(o, "eq_cards", eq_popup_test_card_count());
    cJSON_AddStringToObject(o, "power_last_action", power_menu_test_last_action());
    int privacy_entries = privacy_info_popup_test_entry_count();
    if (privacy_entries >= 0) {
        cJSON *list = cJSON_AddArrayToObject(o, "privacy_entries");
        for (int i = 0; i < privacy_entries; i++) {
            cJSON *j = cJSON_CreateObject();
            cJSON_AddStringToObject(j, "name", privacy_info_popup_test_entry_name(i));
            cJSON_AddStringToObject(j, "category", privacy_info_popup_test_entry_category(i));
            cJSON_AddItemToArray(list, j);
        }
    }
    int tray_entries = tray_context_menu_test_entry_count();
    if (tray_entries >= 0) {
        cJSON *list = cJSON_AddArrayToObject(o, "tray_entries");
        for (int i = 0; i < tray_entries; i++) {
            const DbusMenuEntry *e = tray_context_menu_test_entry(i);
            cJSON *j = cJSON_CreateObject();
            cJSON_AddStringToObject(j, "label", e->label);
            cJSON_AddBoolToObject(j, "separator", e->is_separator);
            cJSON_AddBoolToObject(j, "enabled", e->enabled);
            cJSON_AddStringToObject(j, "toggle", e->toggle == DBUSMENU_TOGGLE_CHECKMARK ? "checkmark"
                                                 : e->toggle == DBUSMENU_TOGGLE_RADIO   ? "radio"
                                                                                        : "");
            cJSON_AddBoolToObject(j, "toggled", e->toggled);
            cJSON_AddBoolToObject(j, "submenu", e->has_submenu);
            cJSON_AddItemToArray(list, j);
        }
    }
    reply(o);
}

static void cmd_target(char *args) {
    // target device <part> <frac> <description...>
    // target mixer <part> <frac> <stream name...>
    // target media <prev|next|play|timeline:FRAC> <app name...>
    // target eq <profile>
    // target power <logout|restart|shutdown|header|corner>
    // target traymenu <label...>
    // target trayicon <output> <title...> (bar-surface coordinates)
    char *kind = strtok(args, " ");
    double x = 0, y = 0;
    bool found = false;
    if (kind && strcmp(kind, "device") == 0) {
        char *part = strtok(NULL, " ");
        char *frac = strtok(NULL, " ");
        char *name = strtok(NULL, "");
        found = part && frac && name && device_popup_test_target(name, part, atof(frac), &x, &y);
    } else if (kind && strcmp(kind, "mixer") == 0) {
        char *part = strtok(NULL, " ");
        char *frac = strtok(NULL, " ");
        char *name = strtok(NULL, "");
        found = part && frac && name && mixer_popup_test_target(name, part, atof(frac), &x, &y);
    } else if (kind && strcmp(kind, "media") == 0) {
        char *button = strtok(NULL, " ");
        char *name = strtok(NULL, "");
        found = button && name && media_popup_test_target(name, button, &x, &y);
    } else if (kind && strcmp(kind, "eq") == 0) {
        char *profile = strtok(NULL, "");
        found = profile && eq_popup_test_target(profile, &x, &y);
    } else if (kind && strcmp(kind, "power") == 0) {
        char *part = strtok(NULL, "");
        found = part && power_menu_test_target(part, &x, &y);
    } else if (kind && strcmp(kind, "traymenu") == 0) {
        char *label = strtok(NULL, "");
        found = label && tray_context_menu_test_target(label, &x, &y);
    } else if (kind && strcmp(kind, "trayicon") == 0) {
        char *output_name = strtok(NULL, " ");
        char *title = strtok(NULL, "");
        Output *output = output_name ? find_output(output_name) : NULL;
        double w = 0, h = 0;
        found = output && title && tray_test_icon_rect(output, title, &x, &y, &w, &h);
        x += w / 2.0;
        y += h / 2.0;
    }
    if (!found) {
        reply_error("target not found");
        return;
    }
    cJSON *o = ok_obj();
    cJSON_AddNumberToObject(o, "x", x);
    cJSON_AddNumberToObject(o, "y", y);
    reply(o);
}

static void dispatch(char *line) {
    char *cmd = strtok(line, " ");
    char *rest = strtok(NULL, "");
    if (!cmd) {
        reply_error("empty command");
        return;
    }
    if (strcmp(cmd, "ping") == 0) {
        reply(ok_obj());
    } else if (strcmp(cmd, "outputs") == 0) {
        cmd_outputs();
    } else if (strcmp(cmd, "rects") == 0 && rest) {
        cmd_rects(rest);
    } else if (strcmp(cmd, "click") == 0 && rest) {
        // click <output> <x> <y> <left|right|middle> -- on that output's bar.
        char name[32], button[16];
        double x, y;
        Output *output = NULL;
        if (sscanf(rest, "%31s %lf %lf %15s", name, &x, &y, button) != 4 || !(output = find_output(name)) ||
            !output->wl_surface) {
            reply_error("usage: click <output> <x> <y> <button>");
            return;
        }
        PointerState saved = aim(output->wl_surface, output, x, y);
        pointer_dispatch_button(g_app, parse_button(button), true);
        pointer_dispatch_button(g_app, parse_button(button), false);
        restore(saved);
        reply(ok_obj());
    } else if (strcmp(cmd, "menu") == 0) {
        cmd_menu();
    } else if ((strcmp(cmd, "menu-press") == 0 || strcmp(cmd, "menu-click") == 0 ||
                strcmp(cmd, "menu-motion") == 0 || strcmp(cmd, "menu-scroll") == 0) && rest) {
        // menu-press|menu-click <x> <y> [button] / menu-motion <x> <y> /
        // menu-scroll <x> <y> <+1|-1>, popup-local coordinates.
        struct wl_surface *surface = menu_surface();
        double x, y;
        char extra[16] = "left";
        if (!surface || sscanf(rest, "%lf %lf %15s", &x, &y, extra) < 2) {
            reply_error(surface ? "usage: menu-* <x> <y> [arg]" : "no open menu");
            return;
        }
        PointerState saved = aim(surface, NULL, x, y);
        if (strcmp(cmd, "menu-motion") == 0) {
            pointer_dispatch_motion(g_app);
        } else if (strcmp(cmd, "menu-scroll") == 0) {
            pointer_dispatch_scroll(g_app, atoi(extra) > 0 ? 1 : -1);
        } else {
            pointer_dispatch_button(g_app, parse_button(extra), true);
            if (strcmp(cmd, "menu-click") == 0) {
                pointer_dispatch_button(g_app, parse_button(extra), false);
            }
        }
        restore(saved);
        reply(ok_obj());
    } else if (strcmp(cmd, "menu-release") == 0) {
        pointer_dispatch_button(g_app, BTN_LEFT, false);
        reply(ok_obj());
    } else if (strcmp(cmd, "catcher-click") == 0) {
        PopupSurface *catcher = popup_surface_first_catcher();
        if (!catcher) {
            reply_error("no catcher");
            return;
        }
        PointerState saved = aim(popup_surface_wl_surface(catcher), NULL, 5.0, 5.0);
        pointer_dispatch_button(g_app, BTN_LEFT, true);
        pointer_dispatch_button(g_app, BTN_LEFT, false);
        restore(saved);
        reply(ok_obj());
    } else if (strcmp(cmd, "snapshot") == 0 && rest) {
        PopupSurface *menu = popup_surface_latest_menu();
        if (!menu || !popup_surface_snapshot_png(menu, rest)) {
            reply_error("no open menu to snapshot");
            return;
        }
        reply(ok_obj());
    } else if (strcmp(cmd, "target") == 0 && rest) {
        cmd_target(rest);
    } else if (strcmp(cmd, "vis-tone") == 0 && rest) {
        // vis-tone <slot> <channel 0=L 1=R> <hz>: synthetic audio for one
        // visualizer slot (only in STICKBAR_TEST_VISUALIZER_SYNTH mode).
        int slot = -1, channel = -1;
        double hz = -1.0;
        if (sscanf(rest, "%d %d %lf", &slot, &channel, &hz) == 3 && hz >= 0.0) {
            visualizer_test_set_tone(slot, channel, hz);
            reply(ok_obj());
        } else {
            reply_error("usage: vis-tone <slot> <channel> <hz>");
        }
    } else if (strcmp(cmd, "refresh") == 0 && rest) {
        // refresh <mem|cpu|cpu-reset|gpu|poll>: run a details refresh (what an
        // open dropdown does) or a full 5s-style poll, without waiting for it.
        if (strcmp(rest, "mem") == 0) {
            sysmon_mem_refresh_details();
        } else if (strcmp(rest, "cpu-reset") == 0) {
            sysmon_cpu_reset_process_sample();
            sysmon_cpu_refresh_details();
        } else if (strcmp(rest, "cpu") == 0) {
            sysmon_cpu_refresh_details();
        } else if (strcmp(rest, "gpu") == 0) {
            sysmon_gpu_refresh_details();
        } else if (strcmp(rest, "poll") == 0) {
            sysmon_client_poll();
            network_client_poll();
            network_client_refresh_details(); // what an open dropdown would read
        } else {
            reply_error("unknown refresh target");
            return;
        }
        reply(ok_obj());
    } else if (strcmp(cmd, "dump") == 0 && rest) {
        if (strcmp(rest, "mem") == 0) reply(dump_mem());
        else if (strcmp(rest, "cpu") == 0) reply(dump_cpu());
        else if (strcmp(rest, "gpu") == 0) reply(dump_gpu());
        else if (strcmp(rest, "net") == 0) reply(dump_net());
        else if (strcmp(rest, "audio") == 0) reply(dump_audio());
        else if (strcmp(rest, "media") == 0) reply(dump_media());
        else if (strcmp(rest, "eq") == 0) reply(dump_eq());
        else if (strcmp(rest, "visualizer") == 0) reply(dump_visualizer());
        else reply_error("unknown dump section");
    } else {
        reply_error("unknown command");
    }
}

bool test_control_enabled(void) {
    const char *enabled = getenv("STICKBAR_TEST_CONTROL");
    return enabled && strcmp(enabled, "1") == 0;
}

void test_control_isolate_surface(AppContext *app, struct wl_surface *surface) {
    if (!test_control_enabled()) {
        return;
    }
    // An empty region: no part of the surface accepts pointer input. Takes
    // effect with the surface's next commit (its first, in every caller).
    struct wl_region *region = wl_compositor_create_region(app->compositor);
    wl_surface_set_input_region(surface, region);
    wl_region_destroy(region);
}

int test_control_init(AppContext *app) {
    if (!test_control_enabled()) {
        return -1;
    }
    g_app = app;
    g_fd = STDIN_FILENO;
    int flags = fcntl(g_fd, F_GETFL);
    fcntl(g_fd, F_SETFL, flags | O_NONBLOCK);
    fprintf(stderr, "stickBar: test-control channel enabled on stdin\n");
    return g_fd;
}

bool test_control_pump(void) {
    if (g_fd < 0) {
        return false;
    }
    for (;;) {
        ssize_t n = read(g_fd, g_buf + g_len, sizeof(g_buf) - 1 - g_len);
        if (n > 0) {
            g_len += (size_t)n;
        } else if (n == 0) {
            // The test closed its end -- stop watching (the main loop keeps
            // seeing EOF otherwise). Pending lines below still run.
            g_fd = -1;
        } else if (errno != EAGAIN && errno != EINTR) {
            g_fd = -1;
        }
        if (n <= 0 || g_len >= sizeof(g_buf) - 1) {
            break;
        }
    }
    g_buf[g_len] = '\0';
    char *start = g_buf;
    char *nl;
    while ((nl = strchr(start, '\n')) != NULL) {
        *nl = '\0';
        if (*start) {
            dispatch(start);
        }
        start = nl + 1;
    }
    size_t rest = strlen(start);
    memmove(g_buf, start, rest);
    g_len = rest;
    return g_fd >= 0;
}
