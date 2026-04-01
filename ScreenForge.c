/*
 * ScreenForge: a Linux utility for managing display settings.
 * 
 * Version: 1.1
 * 
 * Copyright (C) 2025 Maksym Nazar.
 * Created with the assistance of ChatGPT, Perplexity, and Claude.
 * 
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <https://www.gnu.org/licenses/>.
 */

#include <gtk/gtk.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>

#define CMD_BUFFER 4096
#define MAX_MODES 80
#define MAX_RATES 20
#define MAX_MONITORS 16

typedef struct {
    GtkWidget *scale;
    GtkWidget *entry;
    GtkComboBoxText *monitor_combo;
    void (*apply_func)(const char *monitor, double val);
    double default_val;
} SliderEntry;

typedef struct {
    GtkComboBoxText *monitor_combo;
    GtkComboBoxText *res_combo;
    GtkComboBoxText *rate_combo;
    GtkComboBoxText *rot_combo;
    GtkComboBoxText *onoff_combo;
    GtkComboBoxText *hdr_combo;
    char monitor[64];
    char modes[MAX_MODES][32];
    double rates[MAX_MODES][MAX_RATES];
    int rate_count[MAX_MODES];
    int mode_count;
} ResRateData;

static gboolean updating_resolution = FALSE;

typedef struct {
    char monitor[64];
    char old_mode[32];
    char old_rate[32];
    char new_mode[32];
    char new_rate[32];
    char old_rotation[16];
    char new_rotation[16];
    char old_state[8];
    char new_state[8];
    char old_hdr[16];
    char new_hdr[16];
    guint timeout_id;
    guint tick_id;
    GtkWidget *dialog;
    GtkWidget *label;
    int remaining;
    ResRateData *rr;
} SafeApplyData;

static void exec_cmd(const char *cmd, char *output, size_t size) {
    FILE *fp = popen(cmd, "r");
    if (!fp) {
        if (size > 0) output[0] = 0;
        return;
    }
    output[0] = 0;
    size_t pos = 0;
    while (fgets(output + pos, size - pos, fp)) {
        pos = strlen(output);
        if (pos >= size - 1) break;
    }
    pclose(fp);
}

static void get_monitors(char monitors[][32], int *count) {
    char output[CMD_BUFFER] = {0};
    exec_cmd("xrandr | grep ' connected' | awk '{print $1}'", output, sizeof(output));
    *count = 0;
    char *tok = strtok(output, " \n");
    while (tok && *count < MAX_MONITORS) {
        strncpy(monitors[*count], tok, 31);
        monitors[*count][31] = 0;
        (*count)++;
        tok = strtok(NULL, " \n");
    }
}

static void get_backlights(char backlights[][32], int *count) {
    char output[CMD_BUFFER] = {0};
    exec_cmd("ls /sys/class/backlight 2>/dev/null", output, sizeof(output));
    *count = 0;
    char *tok = strtok(output, " \n");
    while (tok && *count < 10) {
        strncpy(backlights[*count], tok, 31);
        backlights[*count][31] = 0;
        (*count)++;
        tok = strtok(NULL, " \n");
    }
}

static int read_backlight_value(const char *name) {
    char path[CMD_BUFFER], buf[64];
    snprintf(path, sizeof(path), "/sys/class/backlight/%s/brightness", name);
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    if (!fgets(buf, sizeof(buf), f)) { fclose(f); return -1; }
    fclose(f);
    return atoi(buf);
}

static void set_xrandr_brightness_mon(const char *monitor, double val) {
    if (!monitor) return;
    char cmd[CMD_BUFFER];
    snprintf(cmd, sizeof(cmd), "xrandr --output %s --brightness %.2f", monitor, val);
    system(cmd);
}

static void set_xrandr_gamma_mon(const char *monitor, double val) {
    if (!monitor) return;
    char cmd[CMD_BUFFER];
    snprintf(cmd, sizeof(cmd), "xrandr --output %s --gamma %.2f:%.2f:%.2f", monitor, val, val, val);
    system(cmd);
}

static void set_brightness_backlight_mon(const char *unused, double val) {
    (void)unused;
    char b[10][32]; int count = 0;
    get_backlights(b, &count);
    for (int i = 0; i < count; i++) {
        char cmd[CMD_BUFFER];
        snprintf(cmd, sizeof(cmd), "echo %d > /sys/class/backlight/%s/brightness", (int)val, b[i]);
        system(cmd);
    }
}

static void set_dpi_mon(const char *unused, double val) {
    (void)unused;
    char cmd[CMD_BUFFER];
    snprintf(cmd, sizeof(cmd), "xrandr --dpi %d", (int)val);
    system(cmd);
}

static void set_scale_mon(const char *monitor, double val) {
    if (!monitor) return;
    char cmd[CMD_BUFFER];
    snprintf(cmd, sizeof(cmd), "xrandr --output %s --scale %.2fx%.2f", monitor, val, val);
    system(cmd);
}

static void set_resolution_and_rate(const char *monitor, const char *mode, const char *rate) {
    if (!monitor || !mode || strcmp(mode, "Auto") == 0) return;
    char cmd[CMD_BUFFER];
    snprintf(cmd, sizeof(cmd), "xrandr --output %s --mode %s", monitor, mode);
    system(cmd);
    if (rate && strcmp(rate, "Auto") != 0) {
        usleep(80000);
        snprintf(cmd, sizeof(cmd), "xrandr --output %s --mode %s --rate %s", monitor, mode, rate);
        system(cmd);
    }
}

static void set_rotation(const char *monitor, const char *rotation) {
    if (!monitor || !rotation) return;
    char cmd[CMD_BUFFER];
    snprintf(cmd, sizeof(cmd), "xrandr --output %s --rotate %s", monitor, rotation);
    system(cmd);
}

static void set_monitor_state(const char *monitor, const char *state) {
    if (!monitor || !state) return;
    char cmd[CMD_BUFFER];
    if (strcmp(state, "On") == 0)
        snprintf(cmd, sizeof(cmd), "xrandr --output %s --auto", monitor);
    else
        snprintf(cmd, sizeof(cmd), "xrandr --output %s --off", monitor);
    system(cmd);
}

static void set_hdr(const char *monitor, const char *state) {
    (void)monitor; (void)state;
    g_print("HDR: %s for %s\n", state ? state : "(null)", monitor ? monitor : "(null)");
}

static void scale_changed_cb(GtkRange *range, gpointer data) {
    SliderEntry *se = (SliderEntry *)data;
    double val = gtk_range_get_value(range);
    char buf[128];
    snprintf(buf, sizeof(buf), "%.2f (default %.2f)", val, se->default_val);
    gtk_entry_set_text(GTK_ENTRY(se->entry), buf);
}

static void entry_activate_cb(GtkEntry *entry, gpointer data) {
    SliderEntry *se = (SliderEntry *)data;
    const char *txt = gtk_entry_get_text(entry);
    char *endp = NULL;
    double v = strtod(txt, &endp);
    gtk_range_set_value(GTK_RANGE(se->scale), v);
    const char *mon = gtk_combo_box_text_get_active_text(se->monitor_combo);
    if (se->apply_func) se->apply_func(mon, v);
    g_free((gchar *)mon);
}

static GtkComboBoxText *create_monitor_combo(GtkGrid *grid, int row, char monitors[][32], int mon_count, int default_idx) {
    GtkWidget *mcombo = gtk_combo_box_text_new();
    for (int i = 0; i < mon_count; i++) {
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(mcombo), monitors[i]);
    }
    if (mon_count > 0) {
        if (default_idx < 0) default_idx = 0;
        gtk_combo_box_set_active(GTK_COMBO_BOX(mcombo), default_idx);
    }
    gtk_grid_attach(GTK_GRID(grid), mcombo, 0, row, 1, 1);
    return GTK_COMBO_BOX_TEXT(mcombo);
}

static SliderEntry *create_slider_row_with_monitor(GtkGrid *grid, int row, GtkComboBoxText *monitor_combo,
                                                  const char *label_text, double min, double max, double step,
                                                  void (*func)(const char *, double), double init_val, double default_val) {
    GtkWidget *label = gtk_label_new(label_text);
    gtk_grid_attach(GTK_GRID(grid), label, 1, row, 1, 1);
    GtkWidget *scale = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, min, max, step);
    gtk_widget_set_size_request(scale, 420, -1);
    gtk_range_set_value(GTK_RANGE(scale), init_val);
    gtk_grid_attach(GTK_GRID(grid), scale, 2, row, 1, 1);
    GtkWidget *entry = gtk_entry_new();
    gtk_widget_set_tooltip_text(entry, "Press Enter to apply");
    gtk_widget_set_size_request(entry, 180, -1);
    char buf[128];
    snprintf(buf, sizeof(buf), "%.2f (default %.2f)", init_val, default_val);
    gtk_entry_set_text(GTK_ENTRY(entry), buf);
    gtk_grid_attach(GTK_GRID(grid), entry, 3, row, 1, 1);
    SliderEntry *se = g_malloc(sizeof(SliderEntry));
    se->scale = scale;
    se->entry = entry;
    se->monitor_combo = monitor_combo;
    se->apply_func = func;
    se->default_val = default_val;
    g_signal_connect(scale, "value-changed", G_CALLBACK(scale_changed_cb), se);
    g_signal_connect(entry, "activate", G_CALLBACK(entry_activate_cb), se);
    return se;
}

static GtkWidget *create_labelled_combo_with_monitor(GtkGrid *grid, int row, GtkComboBoxText *monitor_combo,
                                                     const char *label_text, const char *options[], int opt_count,
                                                     GCallback changed_cb, gpointer user_data) {
    GtkWidget *label = gtk_label_new(label_text);
    gtk_grid_attach(GTK_GRID(grid), label, 1, row, 1, 1);
    GtkWidget *combo = gtk_combo_box_text_new();
    for (int i = 0; i < opt_count; i++) {
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo), options[i]);
    }
    gtk_grid_attach(GTK_GRID(grid), combo, 3, row, 1, 1);
    gtk_combo_box_set_active(GTK_COMBO_BOX(combo), 0);
    if (changed_cb)
        g_signal_connect(combo, "changed", changed_cb, user_data);
    return combo;
}

static void get_display_modes_and_rates(const char *monitor, ResRateData *rr) {
    rr->mode_count = 0;
    if (!monitor) return;
    char output[CMD_BUFFER] = {0};
    exec_cmd("xrandr --query", output, sizeof(output));
    char *line = strtok(output, "\n");
    int found_monitor = 0;
    while (line && rr->mode_count < MAX_MODES) {
        if (!found_monitor) {
            if (strstr(line, monitor) && strstr(line, " connected")) {
                found_monitor = 1;
                line = strtok(NULL, "\n");
                continue;
            }
        } else {
            if (line[0] != ' ' && line[0] != '\t') break;
            const char *p = line;
            while (*p && isspace((unsigned char)*p)) p++;
            char mode[32] = {0};
            int i = 0;
            while (*p && !isspace((unsigned char)*p) && i < (int)sizeof(mode) - 1) mode[i++] = *p++;
            mode[i] = 0;
            if (mode[0] == 0) {
                line = strtok(NULL, "\n");
                continue;
            }
            double rates[MAX_RATES];
            int rcount = 0;
            const char *q = p;
            while (*q) {
                while (*q && !(isdigit((unsigned char)*q) || *q == '.')) q++;
                if (!*q) break;
                char numbuf[64];
                int ni = 0;
                while (*q && (isdigit((unsigned char)*q) || *q == '.')) {
                    if (ni < (int)sizeof(numbuf) - 1) numbuf[ni++] = *q;
                    q++;
                }
                numbuf[ni] = 0;
                if (ni > 0) {
                    double v = atof(numbuf);
                    if (rcount < MAX_RATES) rates[rcount++] = v;
                }
            }
            strncpy(rr->modes[rr->mode_count], mode, 31);
            rr->modes[rr->mode_count][31] = 0;
            rr->rate_count[rr->mode_count] = 0;
            if (rcount > 0) {
                for (int k = 0; k < rcount && k < MAX_RATES; k++) {
                    rr->rates[rr->mode_count][k] = rates[k];
                    rr->rate_count[rr->mode_count]++;
                }
            } else {
                rr->rates[rr->mode_count][0] = 0.0;
                rr->rate_count[rr->mode_count] = 1;
            }
            rr->mode_count++;
        }
        line = strtok(NULL, "\n");
    }
    if (rr->mode_count == 0) {
        strcpy(rr->modes[0], "Auto");
        rr->rate_count[0] = 1;
        rr->rates[0][0] = 0.0;
        rr->mode_count = 1;
    }
}

static void resolution_changed_generic(GtkComboBoxText *combo, gpointer data) {
    ResRateData *rr = (ResRateData *)data;
    int idx = gtk_combo_box_get_active(GTK_COMBO_BOX(rr->res_combo));
    if (idx < 0) return;
    gtk_combo_box_text_remove_all(rr->rate_combo);
    gtk_combo_box_text_append_text(rr->rate_combo, "Auto");
    for (int i = 0; i < rr->rate_count[idx]; i++) {
        double v = rr->rates[idx][i];
        if (v > 0.0) {
            char buf[32];
            snprintf(buf, sizeof(buf), "%.2f", v);
            gtk_combo_box_text_append_text(rr->rate_combo, buf);
        }
    }
    gtk_combo_box_set_active(GTK_COMBO_BOX(rr->rate_combo), 0);
}

static void set_current_mode_rate_from_xrandr(const char *monitor, char *out_mode, size_t om_sz, char *out_rate, size_t or_sz) {
    out_mode[0] = 0;
    out_rate[0] = 0;
    char cmd[CMD_BUFFER];
    snprintf(cmd, sizeof(cmd), "xrandr --query | sed -n '/^%s connected/,/^[^ ]/p' | grep '\\*' | head -n1 | awk '{print $1, $2}'",
             monitor);
    char out[512] = {0};
    exec_cmd(cmd, out, sizeof(out));
    if (out[0] == 0) return;
    char *tok = strtok(out, " \n");
    if (tok) strncpy(out_mode, tok, om_sz - 1);
    tok = strtok(NULL, " \n");
    if (tok) {
        char numbuf[64] = {0};
        int di = 0;
        for (size_t i = 0; i < strlen(tok); i++) {
            if ((tok[i] >= '0' && tok[i] <= '9') || tok[i] == '.')
                numbuf[di++] = tok[i];
            else
                break;
        }
        numbuf[di] = 0;
        if (di > 0) strncpy(out_rate, numbuf, or_sz - 1);
    }
}

static void rr_monitor_changed_cb(GtkComboBoxText *cmb, gpointer data) {
    ResRateData *rr = (ResRateData *)data;
    const char *mon = gtk_combo_box_text_get_active_text(cmb);
    if (mon) {
        strncpy(rr->monitor, mon, sizeof(rr->monitor) - 1);
        rr->monitor[sizeof(rr->monitor) - 1] = 0;
        
        char current_mode[32] = {0};
        char current_rate[32] = {0};
        set_current_mode_rate_from_xrandr(rr->monitor, current_mode, sizeof(current_mode), current_rate, sizeof(current_rate));
        
        get_display_modes_and_rates(rr->monitor, rr);
        
        updating_resolution = TRUE;
        
        gtk_combo_box_text_remove_all(rr->res_combo);
        int current_mode_idx = 0;
        for (int i = 0; i < rr->mode_count; i++) {
            gtk_combo_box_text_append_text(rr->res_combo, rr->modes[i]);
            if (current_mode[0] && strcmp(rr->modes[i], current_mode) == 0) {
                current_mode_idx = i;
            }
        }
        
        if (rr->mode_count > 0)
            gtk_combo_box_set_active(GTK_COMBO_BOX(rr->res_combo), current_mode_idx);
        
        resolution_changed_generic(rr->res_combo, rr);
        
        if (current_rate[0]) {
            int rate_idx = 0;
            for (int i = 0; i < rr->rate_count[current_mode_idx]; i++) {
                char buf[32];
                snprintf(buf, sizeof(buf), "%.2f", rr->rates[current_mode_idx][i]);
                if (strcmp(buf, current_rate) == 0) {
                    rate_idx = i + 1;
                    break;
                }
            }
            if (rate_idx > 0) {
                gtk_combo_box_set_active(GTK_COMBO_BOX(rr->rate_combo), rate_idx);
            }
        }
        
        updating_resolution = FALSE;
    }
    g_free((gchar *)mon);
}

static void rr_monitor_changed_cb_wrap(GtkComboBoxText *cmb, gpointer data){
    rr_monitor_changed_cb(cmb, data);
}

static void get_current_rotation(const char *monitor, char *rotation, size_t sz) {
    rotation[0] = '\0';
    char cmd[CMD_BUFFER];
    snprintf(cmd, sizeof(cmd),
             "xrandr --query | grep '^%s connected' -A 1 | grep -o 'rotation [a-z]*' | awk '{print $2}'",
             monitor);
    char out[128] = {0};
    exec_cmd(cmd, out, sizeof(out));
    if (out[0]) {
        size_t len = strlen(out);
        if (len > 0 && out[len-1] == '\n') out[len-1] = '\0';
        strncpy(rotation, out, sz - 1);
        rotation[sz - 1] = '\0';
    } else {
        strncpy(rotation, "normal", sz - 1);
        rotation[sz - 1] = '\0';
    }
}

static void get_current_state(const char *monitor, char *state, size_t sz) {
    state[0] = '\0';
    char cmd[CMD_BUFFER];
    snprintf(cmd, sizeof(cmd),
             "xrandr --query | grep '^%s ' | grep -o 'connected' | head -n1",
             monitor);
    char out[128] = {0};
    exec_cmd(cmd, out, sizeof(out));
    if (out[0] && strstr(out, "connected")) {
        char cmd2[CMD_BUFFER];
        snprintf(cmd2, sizeof(cmd2),
                 "xrandr --query | sed -n '/^%s connected/,/^[^ ]/p' | grep '\\*' | wc -l",
                 monitor);
        char out2[128] = {0};
        exec_cmd(cmd2, out2, sizeof(out2));
        int active = atoi(out2);
        if (active > 0)
            strncpy(state, "On", sz - 1);
        else
            strncpy(state, "Off", sz - 1);
        state[sz - 1] = '\0';
    }
}

static void get_current_hdr(const char *monitor, char *hdr, size_t sz) {
    strncpy(hdr, "Disabled", sz - 1);
    hdr[sz - 1] = '\0';
}

static void safe_apply_finish(SafeApplyData *sd, gboolean revert) {
    if (revert) {
        if (sd->old_mode[0] && sd->new_mode[0]) {
            set_resolution_and_rate(sd->monitor, sd->old_mode, sd->old_rate);
        }
        if (sd->old_rotation[0] && sd->new_rotation[0]) {
            set_rotation(sd->monitor, sd->old_rotation);
        }
        if (sd->old_state[0] && sd->new_state[0]) {
            set_monitor_state(sd->monitor, sd->old_state);
        }
        if (sd->old_hdr[0] && sd->new_hdr[0]) {
            set_hdr(sd->monitor, sd->old_hdr);
        }
        
        if (sd->rr) {
            updating_resolution = TRUE;
            ResRateData *rr = sd->rr;
            
            if (sd->old_mode[0]) {
                gtk_combo_box_text_remove_all(rr->res_combo);
                get_display_modes_and_rates(rr->monitor, rr);
                for (int i = 0; i < rr->mode_count; i++) {
                    gtk_combo_box_text_append_text(rr->res_combo, rr->modes[i]);
                }
                int active_idx = 0;
                for (int i = 0; i < rr->mode_count; i++) {
                    if (strcmp(rr->modes[i], sd->old_mode) == 0) {
                        active_idx = i;
                        break;
                    }
                }
                gtk_combo_box_set_active(GTK_COMBO_BOX(rr->res_combo), active_idx);
                resolution_changed_generic(rr->res_combo, rr);
                
                if (sd->old_rate[0]) {
                    int rate_idx = 0;
                    for (int i = 0; i < rr->rate_count[active_idx]; i++) {
                        char buf[32];
                        snprintf(buf, sizeof(buf), "%.2f", rr->rates[active_idx][i]);
                        if (strcmp(buf, sd->old_rate) == 0) {
                            rate_idx = i + 1;
                            break;
                        }
                    }
                    if (rate_idx > 0) {
                        gtk_combo_box_set_active(GTK_COMBO_BOX(rr->rate_combo), rate_idx);
                    }
                }
            }
            
            if (sd->old_rotation[0]) {
                const char *rot_opts[] = {"normal", "left", "right", "inverted"};
                for (int i = 0; i < 4; i++) {
                    if (strcmp(rot_opts[i], sd->old_rotation) == 0) {
                        gtk_combo_box_set_active(GTK_COMBO_BOX(rr->rot_combo), i);
                        break;
                    }
                }
            }
            
            if (sd->old_state[0]) {
                if (strcmp(sd->old_state, "On") == 0) {
                    gtk_combo_box_set_active(GTK_COMBO_BOX(rr->onoff_combo), 0);
                } else {
                    gtk_combo_box_set_active(GTK_COMBO_BOX(rr->onoff_combo), 1);
                }
            }
            
            if (sd->old_hdr[0]) {
                if (strcmp(sd->old_hdr, "Disabled") == 0) {
                    gtk_combo_box_set_active(GTK_COMBO_BOX(rr->hdr_combo), 0);
                } else {
                    gtk_combo_box_set_active(GTK_COMBO_BOX(rr->hdr_combo), 1);
                }
            }
            
            updating_resolution = FALSE;
        }
    }
    if (sd->tick_id) {
        g_source_remove(sd->tick_id);
        sd->tick_id = 0;
    }
    if (sd->timeout_id) {
        g_source_remove(sd->timeout_id);
        sd->timeout_id = 0;
    }
    if (sd->dialog) {
        gtk_widget_destroy(sd->dialog);
        sd->dialog = NULL;
    }
    g_free(sd);
}

static gboolean safe_apply_tick_cb(gpointer user_data) {
    SafeApplyData *sd = (SafeApplyData *)user_data;
    sd->remaining--;
    char buf[256];
    snprintf(buf, sizeof(buf),
             "Keep these display settings?\n"
             "Automatically revert in %d second(s).", sd->remaining);
    gtk_label_set_text(GTK_LABEL(sd->label), buf);
    if (sd->remaining <= 0) {
        safe_apply_finish(sd, TRUE);
        return FALSE;
    }
    return TRUE;
}

static gboolean safe_apply_timeout_cb(gpointer user_data) {
    SafeApplyData *sd = (SafeApplyData *)user_data;
    safe_apply_finish(sd, TRUE);
    return FALSE;
}

static void safe_apply_response_cb(GtkDialog *dialog, gint response, gpointer user_data) {
    SafeApplyData *sd = (SafeApplyData *)user_data;
    if (response == GTK_RESPONSE_OK) {
        safe_apply_finish(sd, FALSE);
    } else {
        safe_apply_finish(sd, TRUE);
    }
}

static GtkWidget *create_safe_dialog(SafeApplyData *sd) {
    GtkWidget *dlg = gtk_dialog_new_with_buttons(
        "Confirm display settings",
        NULL,
        GTK_DIALOG_MODAL,
        "OK", GTK_RESPONSE_OK,
        "Cancel", GTK_RESPONSE_CANCEL,
        NULL);
    gtk_window_set_default_size(GTK_WINDOW(dlg), 480, 160);
    GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(dlg));
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_container_set_border_width(GTK_CONTAINER(box), 14);
    gtk_container_add(GTK_CONTAINER(content), box);
    sd->label = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(sd->label), 0.0);
    gtk_label_set_line_wrap(GTK_LABEL(sd->label), TRUE);
    gtk_widget_set_size_request(sd->label, 460, -1);
    gtk_container_add(GTK_CONTAINER(box), sd->label);
    gtk_widget_show_all(dlg);
    return dlg;
}

static void safe_set_resolution_and_rate(ResRateData *rr, const char *mode_text, const char *rate_text) {
    if (updating_resolution) return;
    SafeApplyData *sd = g_malloc0(sizeof(SafeApplyData));
    strncpy(sd->monitor, rr->monitor, sizeof(sd->monitor) - 1);
    strncpy(sd->new_mode, mode_text ? mode_text : "", sizeof(sd->new_mode) - 1);
    strncpy(sd->new_rate, rate_text ? rate_text : "", sizeof(sd->new_rate) - 1);
    set_current_mode_rate_from_xrandr(sd->monitor, sd->old_mode, sizeof(sd->old_mode), sd->old_rate, sizeof(sd->old_rate));
    sd->rr = rr;
    set_resolution_and_rate(sd->monitor, sd->new_mode, sd->new_rate);
    sd->remaining = 10;
    sd->dialog = create_safe_dialog(sd);
    char buf[256];
    snprintf(buf, sizeof(buf), "Keep these display settings?\nAutomatically revert in %d second(s).", sd->remaining);
    gtk_label_set_text(GTK_LABEL(sd->label), buf);
    sd->tick_id = g_timeout_add_seconds(1, safe_apply_tick_cb, sd);
    sd->timeout_id = g_timeout_add_seconds(10, safe_apply_timeout_cb, sd);
    g_signal_connect(sd->dialog, "response", G_CALLBACK(safe_apply_response_cb), sd);
    gtk_widget_show(sd->dialog);
}

static void safe_set_rotation(ResRateData *rr, const char *rotation_text) {
    if (updating_resolution) return;
    SafeApplyData *sd = g_malloc0(sizeof(SafeApplyData));
    strncpy(sd->monitor, rr->monitor, sizeof(sd->monitor) - 1);
    strncpy(sd->new_rotation, rotation_text ? rotation_text : "", sizeof(sd->new_rotation) - 1);
    get_current_rotation(sd->monitor, sd->old_rotation, sizeof(sd->old_rotation));
    sd->rr = rr;
    set_rotation(sd->monitor, sd->new_rotation);
    sd->remaining = 10;
    sd->dialog = create_safe_dialog(sd);
    char buf[256];
    snprintf(buf, sizeof(buf), "Keep these display settings?\nAutomatically revert in %d second(s).", sd->remaining);
    gtk_label_set_text(GTK_LABEL(sd->label), buf);
    sd->tick_id = g_timeout_add_seconds(1, safe_apply_tick_cb, sd);
    sd->timeout_id = g_timeout_add_seconds(10, safe_apply_timeout_cb, sd);
    g_signal_connect(sd->dialog, "response", G_CALLBACK(safe_apply_response_cb), sd);
    gtk_widget_show(sd->dialog);
}

static void safe_set_monitor_state(ResRateData *rr, const char *state_text) {
    if (updating_resolution) return;
    SafeApplyData *sd = g_malloc0(sizeof(SafeApplyData));
    strncpy(sd->monitor, rr->monitor, sizeof(sd->monitor) - 1);
    strncpy(sd->new_state, state_text ? state_text : "", sizeof(sd->new_state) - 1);
    get_current_state(sd->monitor, sd->old_state, sizeof(sd->old_state));
    sd->rr = rr;
    set_monitor_state(sd->monitor, sd->new_state);
    sd->remaining = 10;
    sd->dialog = create_safe_dialog(sd);
    char buf[256];
    snprintf(buf, sizeof(buf), "Keep these display settings?\nAutomatically revert in %d second(s).", sd->remaining);
    gtk_label_set_text(GTK_LABEL(sd->label), buf);
    sd->tick_id = g_timeout_add_seconds(1, safe_apply_tick_cb, sd);
    sd->timeout_id = g_timeout_add_seconds(10, safe_apply_timeout_cb, sd);
    g_signal_connect(sd->dialog, "response", G_CALLBACK(safe_apply_response_cb), sd);
    gtk_widget_show(sd->dialog);
}

static void safe_set_hdr(ResRateData *rr, const char *hdr_text) {
    if (updating_resolution) return;
    SafeApplyData *sd = g_malloc0(sizeof(SafeApplyData));
    strncpy(sd->monitor, rr->monitor, sizeof(sd->monitor) - 1);
    strncpy(sd->new_hdr, hdr_text ? hdr_text : "", sizeof(sd->new_hdr) - 1);
    get_current_hdr(sd->monitor, sd->old_hdr, sizeof(sd->old_hdr));
    sd->rr = rr;
    set_hdr(sd->monitor, sd->new_hdr);
    sd->remaining = 10;
    sd->dialog = create_safe_dialog(sd);
    char buf[256];
    snprintf(buf, sizeof(buf), "Keep these display settings?\nAutomatically revert in %d second(s).", sd->remaining);
    gtk_label_set_text(GTK_LABEL(sd->label), buf);
    sd->tick_id = g_timeout_add_seconds(1, safe_apply_tick_cb, sd);
    sd->timeout_id = g_timeout_add_seconds(10, safe_apply_timeout_cb, sd);
    g_signal_connect(sd->dialog, "response", G_CALLBACK(safe_apply_response_cb), sd);
    gtk_widget_show(sd->dialog);
}

static void rotation_changed_cb(GtkComboBoxText *combo, gpointer data) {
    if (updating_resolution) return;
    ResRateData *rr = (ResRateData *)data;
    const char *rot = gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(rr->rot_combo));
    if (rot) {
        safe_set_rotation(rr, rot);
        g_free((gchar *)rot);
    }
}

static void onoff_changed_cb(GtkComboBoxText *combo, gpointer data) {
    if (updating_resolution) return;
    ResRateData *rr = (ResRateData *)data;
    const char *st = gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(rr->onoff_combo));
    if (st) {
        safe_set_monitor_state(rr, st);
        g_free((gchar *)st);
    }
}

static void hdr_changed_cb(GtkComboBoxText *combo, gpointer data) {
    if (updating_resolution) return;
    ResRateData *rr = (ResRateData *)data;
    const char *st = gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(rr->hdr_combo));
    if (st) {
        safe_set_hdr(rr, st);
        g_free((gchar *)st);
    }
}

static void rate_changed_generic_cb(GtkComboBoxText *combo, gpointer data) {
    if (updating_resolution) return;
    ResRateData *rr = (ResRateData *)data;
    int midx = gtk_combo_box_get_active(GTK_COMBO_BOX(rr->res_combo));
    int ridx = gtk_combo_box_get_active(GTK_COMBO_BOX(rr->rate_combo));
    if (midx < 0 || ridx < 0) return;
    const char *mode_text = gtk_combo_box_text_get_active_text(rr->res_combo);
    const char *rate_text = gtk_combo_box_text_get_active_text(rr->rate_combo);
    safe_set_resolution_and_rate(rr, mode_text, rate_text);
    g_free((gchar *)mode_text);
    g_free((gchar *)rate_text);
}

static void resolution_changed_cb(GtkComboBoxText *combo, gpointer data) {
    if (updating_resolution) return;
    resolution_changed_generic(combo, data);
}

static void show_text_popup(const char *title, const char *text) {
    GtkWidget *dlg = gtk_dialog_new_with_buttons(title, NULL, GTK_DIALOG_MODAL, "OK", GTK_RESPONSE_OK, NULL);
    GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(dlg));
    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_widget_set_size_request(scroll, 700, 600);
    gtk_container_add(GTK_CONTAINER(content), scroll);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    
    gtk_widget_set_margin_start(box, 14);
    gtk_widget_set_margin_end(box, 25);
    gtk_widget_set_margin_top(box, 14);
    gtk_widget_set_margin_bottom(box, 14);
    
    gtk_container_add(GTK_CONTAINER(scroll), box);
    GtkWidget *label = gtk_label_new(NULL);
    gtk_label_set_text(GTK_LABEL(label), text);
    gtk_label_set_xalign(GTK_LABEL(label), 0.0);
    gtk_label_set_line_wrap(GTK_LABEL(label), TRUE);
    gtk_container_add(GTK_CONTAINER(box), label);
    gtk_widget_show_all(dlg);
    gtk_dialog_run(GTK_DIALOG(dlg));
    gtk_widget_destroy(dlg);
}

static void about_clicked_cb(GtkButton *btn, gpointer data){
    const char *about_text =
        "ScreenForge: a Linux utility for managing display settings.\n"
        "Author: Maksym Nazar.\n"
        "Created with the assistance of ChatGPT, Perplexity, and Claude.\n\n"
        "This program allows managing brightness, gamma, resolution, refresh rate,\n"
        "DPI, scale, rotation, monitor state, HDR, and screen backlight control\n"
        "(where supported by your hardware).\n"
        "Use the monitor column to select which display each row applies to.\n"
        "For field-based changes, press Enter in the numeric field.\n\n"
        "Some changes are protected by a confirmation dialog allowing automatic\n"
        "revert within 10 seconds, helping to avoid misconfiguration.\n\n"
        "See Terms of Use and License for more info.";
    GtkWidget *dlg = gtk_dialog_new_with_buttons("About ScreenForge", NULL, GTK_DIALOG_MODAL,
                                                "OK", GTK_RESPONSE_OK, NULL);
    GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(dlg));
    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_widget_set_size_request(scroll, 550, 270);
    gtk_container_add(GTK_CONTAINER(content), scroll);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_container_set_border_width(GTK_CONTAINER(box), 14);
    gtk_container_add(GTK_CONTAINER(scroll), box);
    GtkWidget *label = gtk_label_new(NULL);
    gtk_label_set_text(GTK_LABEL(label), about_text);
    gtk_label_set_xalign(GTK_LABEL(label), 0.0);
    gtk_label_set_line_wrap(GTK_LABEL(label), TRUE);
    gtk_label_set_line_wrap_mode(GTK_LABEL(label), PANGO_WRAP_WORD_CHAR);
    gtk_container_add(GTK_CONTAINER(box), label);
    gtk_widget_show_all(dlg);
    gtk_dialog_run(GTK_DIALOG(dlg));
    gtk_widget_destroy(dlg);
}

static void terms_clicked_cb(GtkButton *btn, gpointer data) {
    show_text_popup("Terms of Use", "TERMS OF USE\n\n"
"1. Usage:\n"
"You are granted a non-exclusive, non-transferable license to use the Program under the terms of the GNU General Public License (GPL) Version 3.0. The term \"Program\" refers to the software package or product distributed under this License. You may use, copy, modify, and distribute the Program freely, provided that all copies and derivative works are licensed under the GPL and include this license notice.\n\n"
"2. License:\n"
"This Program is licensed under the GNU General Public License (GPL) Version 3.0, which ensures that users have the freedom to run, study, share, and modify the software. A copy of the GPL license is included with the Program package, or you can access it at https://www.gnu.org/licenses/gpl-3.0.html.\n\n"
"3. Source Code Availability:\n"
"As required by the GNU General Public License (GPL), the full source code of this Program is available and can be obtained from the official repository or package distribution. If you did not receive a copy of the source code, you may request it from the developer. Additionally, you have the right to access and modify the source code under the terms of this License.\n\n"
"4. Disclaimer of Warranties:\n"
"The Program is provided \"as is,\" without any warranties, express or implied, including but not limited to the implied warranties of merchantability or fitness for a particular purpose. The developers make no representations or warranties regarding the use or performance of the Program.\n\n"
"5. Limitation of Liability:\n"
"In no event shall the developers be liable for any direct, indirect, incidental, special, exemplary, or consequential damages, including but not limited to damages for loss of data or profit, arising out of or in connection with the use of or inability to use the Program, even if advised of the possibility of such damages.\n\n"
"6. Modifications to the Program:\n"
"You may modify and distribute modified versions of the Program, provided you comply with the terms of the GNU General Public License (GPL). The developers reserve the right to modify, update, or discontinue the Program at their discretion.\n\n"
"7. Compliance with Laws:\n"
"You are responsible for complying with all applicable local, state, national, and international laws in connection with your use of the Program.\n\n"
"8. Copyright:\n"
"Copyright (C) 2025 Maksym Nazar.\n"
"Created with the assistance of ChatGPT, Perplexity, and Claude.\n"
"This work is licensed under the GNU General Public License (GPL) Version 3.0.\n\n"
"9. Contact:\n"
"For inquiries, please contact us at:\n"
"Email: maximkursua@gmail.com\n");
}

static void license_clicked_cb(GtkButton *btn, gpointer data) {
    show_text_popup("License", "Version 3, 29 June 2007\n\n"
"Copyright © 2007 Free Software Foundation, Inc. <https://fsf.org/>\n\n"
"Everyone is permitted to copy and distribute verbatim copies of this license document, but changing it is not allowed.\n\n"
"Preamble\n\n"
"The GNU General Public License is a free, copyleft license for software and other kinds of works.\n\n"
"The licenses for most software and other practical works are designed to take away your freedom to share and change the works. By contrast, the GNU General Public License is intended to guarantee your freedom to share and change all versions of a program--to make sure it remains free software for all its users. We, the Free Software Foundation, use the GNU General Public License for most of our software; it applies also to any other work released this way by its authors. You can apply it to your programs, too.\n\n"
"When we speak of free software, we are referring to freedom, not price. Our General Public Licenses are designed to make sure that you have the freedom to distribute copies of free software (and charge for them if you wish), that you receive source code or can get it if you want it, that you can change the software or use pieces of it in new free programs, and that you know you can do these things.\n\n"
"To protect your rights, we need to prevent others from denying you these rights or asking you to surrender the rights. Therefore, you have certain responsibilities if you distribute copies of the software, or if you modify it: responsibilities to respect the freedom of others.\n\n"
"For example, if you distribute copies of such a program, whether gratis or for a fee, you must pass on to the recipients the same freedoms that you received. You must make sure that they, too, receive or can get the source code. And you must show them these terms so they know their rights.\n\n"
"Developers that use the GNU GPL protect your rights with two steps: (1) assert copyright on the software, and (2) offer you this License giving you legal permission to copy, distribute and/or modify it.\n\n"
"For the developers' and authors' protection, the GPL clearly explains that there is no warranty for this free software. For both users' and authors' sake, the GPL requires that modified versions be marked as changed, so that their problems will not be attributed erroneously to authors of previous versions.\n\n"
"Some devices are designed to deny users access to install or run modified versions of the software inside them, although the manufacturer can do so. This is fundamentally incompatible with the aim of protecting users' freedom to change the software. The systematic pattern of such abuse occurs in the area of products for individuals to use, which is precisely where it is most unacceptable. Therefore, we have designed this version of the GPL to prohibit the practice for those products. If such problems arise substantially in other domains, we stand ready to extend this provision to those domains in future versions of the GPL, as needed to protect the freedom of users.\n\n"
"Finally, every program is threatened constantly by software patents. States should not allow patents to restrict development and use of software on general-purpose computers, but in those that do, we wish to avoid the special danger that patents applied to a free program could make it effectively proprietary. To prevent this, the GPL assures that patents cannot be used to render the program non-free.\n\n"
"The precise terms and conditions for copying, distribution and modification follow.\n\n"
"TERMS AND CONDITIONS\n\n"
"0. Definitions.\n\n"
"This License” refers to version 3 of the GNU General Public License.\n\n"
"“Copyright” also means copyright-like laws that apply to other kinds of works, such as semiconductor masks.\n\n"
"“The Program” refers to any copyrightable work licensed under this License. Each licensee is addressed as “you”. “Licensees” and “recipients” may be individuals or organizations.\n\n"
"To “modify” a work means to copy from or adapt all or part of the work in a fashion requiring copyright permission, other than the making of an exact copy. The resulting work is called a “modified version” of the earlier work or a work “based on” the earlier work.\n\n"
"A “covered work” means either the unmodified Program or a work based on the Program.\n\n"
"To “propagate” a work means to do anything with it that, without permission, would make you directly or secondarily liable for infringement under applicable copyright law, except executing it on a computer or modifying a private copy. Propagation includes copying, distribution (with or without modification), making available to the public, and in some countries other activities as well.\n\n"
"To “convey” a work means any kind of propagation that enables other parties to make or receive copies. Mere interaction with a user through a computer network, with no transfer of a copy, is not conveying.\n\n"
"An interactive user interface displays “Appropriate Legal Notices” to the extent that it includes a convenient and prominently visible feature that (1) displays an appropriate copyright notice, and (2) tells the user that there is no warranty for the work (except to the extent that warranties are provided), that licensees may convey the work under this License, and how to view a copy of this License. If the interface presents a list of user commands or options, such as a menu, a prominent item in the list meets this criterion.\n\n"
"1. Source Code.\n\n"
"The “source code” for a work means the preferred form of the work for making modifications to it. “Object code” means any non-source form of a work.\n\n"
"A “Standard Interface” means an interface that either is an official standard defined by a recognized standards body, or, in the case of interfaces specified for a particular programming language, one that is widely used among developers working in that language.\n\n"
"The “System Libraries” of an executable work include anything, other than the work as a whole, that (a) is included in the normal form of packaging a Major Component, but which is not part of that Major Component, and (b) serves only to enable use of the work with that Major Component, or to implement a Standard Interface for which an implementation is available to the public in source code form. A “Major Component”, in this context, means a major essential component (kernel, window system, and so on) of the specific operating system (if any) on which the executable work runs, or a compiler used to produce the work, or an object code interpreter used to run it.\n\n"
"The “Corresponding Source” for a work in object code form means all the source code needed to generate, install, and (for an executable work) run the object code and to modify the work, including scripts to control those activities. However, it does not include the work's System Libraries, or general-purpose tools or generally available free programs which are used unmodified in performing those activities but which are not part of the work. For example, Corresponding Source includes interface definition files associated with source files for the work, and the source code for shared libraries and dynamically linked subprograms that the work is specifically designed to require, such as by intimate data communication or control flow between those subprograms and other parts of the work.\n\n"
"The Corresponding Source need not include anything that users can regenerate automatically from other parts of the Corresponding Source.\n\n"
"The Corresponding Source for a work in source code form is that same work.\n\n"
"2. Basic Permissions.\n\n"
"All rights granted under this License are granted for the term of copyright on the Program, and are irrevocable provided the stated conditions are met. This License explicitly affirms your unlimited permission to run the unmodified Program. The output from running a covered work is covered by this License only if the output, given its content, constitutes a covered work. This License acknowledges your rights of fair use or other equivalent, as provided by copyright law.\n\n"
"You may make, run and propagate covered works that you do not convey, without conditions so long as your license otherwise remains in force. You may convey covered works to others for the sole purpose of having them make modifications exclusively for you, or provide you with facilities for running those works, provided that you comply with the terms of this License in conveying all material for which you do not control copyright. Those thus making or running the covered works for you must do so exclusively on your behalf, under your direction and control, on terms that prohibit them from making any copies of your copyrighted material outside their relationship with you.\n\n"
"Conveying under any other circumstances is permitted solely under the conditions stated below. Sublicensing is not allowed; section 10 makes it unnecessary.\n\n"
"3. Protecting Users' Legal Rights From Anti-Circumvention Law.\n\n"
"No covered work shall be deemed part of an effective technological measure under any applicable law fulfilling obligations under article 11 of the WIPO copyright treaty adopted on 20 December 1996, or similar laws prohibiting or restricting circumvention of such measures.\n\n"
"When you convey a covered work, you waive any legal power to forbid circumvention of technological measures to the extent such circumvention is effected by exercising rights under this License with respect to the covered work, and you disclaim any intention to limit operation or modification of the work as a means of enforcing, against the work's users, your or third parties' legal rights to forbid circumvention of technological measures.\n\n"
"4. Conveying Verbatim Copies.\n\n"
"You may convey verbatim copies of the Program's source code as you receive it, in any medium, provided that you conspicuously and appropriately publish on each copy an appropriate copyright notice; keep intact all notices stating that this License and any non-permissive terms added in accord with section 7 apply to the code; keep intact all notices of the absence of any warranty; and give all recipients a copy of this License along with the Program.\n\n"
"You may charge any price or no price for each copy that you convey, and you may offer support or warranty protection for a fee.\n\n"
"5. Conveying Modified Source Versions.\n\n"
"You may convey a work based on the Program, or the modifications to produce it from the Program, in the form of source code under the terms of section 4, provided that you also meet all of these conditions:\n\n"
"    a) The work must carry prominent notices stating that you modified it, and giving a relevant date.\n\n"
"    b) The work must carry prominent notices stating that it is released under this License and any conditions added under section 7. This requirement modifies the requirement in section 4 to “keep intact all notices”.\n\n"
"    c) You must license the entire work, as a whole, under this License to anyone who comes into possession of a copy. This License will therefore apply, along with any applicable section 7 additional terms, to the whole of the work, and all its parts, regardless of how they are packaged. This License gives no permission to license the work in any other way, but it does not invalidate such permission if you have separately received it.\n\n"
"    d) If the work has interactive user interfaces, each must display Appropriate Legal Notices; however, if the Program has interactive interfaces that do not display Appropriate Legal Notices, your work need not make them do so.\n\n"
"A compilation of a covered work with other separate and independent works, which are not by their nature extensions of the covered work, and which are not combined with it such as to form a larger program, in or on a volume of a storage or distribution medium, is called an “aggregate” if the compilation and its resulting copyright are not used to limit the access or legal rights of the compilation's users beyond what the individual works permit. Inclusion of a covered work in an aggregate does not cause this License to apply to the other parts of the aggregate.\n\n"
"6. Conveying Non-Source Forms.\n\n"
"You may convey a covered work in object code form under the terms of sections 4 and 5, provided that you also convey the machine-readable Corresponding Source under the terms of this License, in one of these ways:\n\n"
"    a) Convey the object code in, or embodied in, a physical product (including a physical distribution medium), accompanied by the Corresponding Source fixed on a durable physical medium customarily used for software interchange.\n\n"
"    b) Convey the object code in, or embodied in, a physical product (including a physical distribution medium), accompanied by a written offer, valid for at least three years and valid for as long as you offer spare parts or customer support for that product model, to give anyone who possesses the object code either (1) a copy of the Corresponding Source for all the software in the product that is covered by this License, on a durable physical medium customarily used for software interchange, for a price no more than your reasonable cost of physically performing this conveying of source, or (2) access to copy the Corresponding Source from a network server at no charge.\n\n"
"    c) Convey individual copies of the object code with a copy of the written offer to provide the Corresponding Source. This alternative is allowed only occasionally and noncommercially, and only if you received the object code with such an offer, in accord with subsection 6b.\n\n"
"    d) Convey the object code by offering access from a designated place (gratis or for a charge), and offer equivalent access to the Corresponding Source in the same way through the same place at no further charge. You need not require recipients to copy the Corresponding Source along with the object code. If the place to copy the object code is a network server, the Corresponding Source may be on a different server (operated by you or a third party) that supports equivalent copying facilities, provided you maintain clear directions next to the object code saying where to find the Corresponding Source. Regardless of what server hosts the Corresponding Source, you remain obligated to ensure that it is available for as long as needed to satisfy these requirements.\n\n"
"    e) Convey the object code using peer-to-peer transmission, provided you inform other peers where the object code and Corresponding Source of the work are being offered to the general public at no charge under subsection 6d.\n\n"
"A separable portion of the object code, whose source code is excluded from the Corresponding Source as a System Library, need not be included in conveying the object code work.\n\n"
"A “User Product” is either (1) a “consumer product”, which means any tangible personal property which is normally used for personal, family, or household purposes, or (2) anything designed or sold for incorporation into a dwelling. In determining whether a product is a consumer product, doubtful cases shall be resolved in favor of coverage. For a particular product received by a particular user, “normally used” refers to a typical or common use of that class of product, regardless of the status of the particular user or of the way in which the particular user actually uses, or expects or is expected to use, the product. A product is a consumer product regardless of whether the product has substantial commercial, industrial or non-consumer uses, unless such uses represent the only significant mode of use of the product.\n\n"
"“Installation Information” for a User Product means any methods, procedures, authorization keys, or other information required to install and execute modified versions of a covered work in that User Product from a modified version of its Corresponding Source. The information must suffice to ensure that the continued functioning of the modified object code is in no case prevented or interfered with solely because modification has been made.\n\n"
"If you convey an object code work under this section in, or with, or specifically for use in, a User Product, and the conveying occurs as part of a transaction in which the right of possession and use of the User Product is transferred to the recipient in perpetuity or for a fixed term (regardless of how the transaction is characterized), the Corresponding Source conveyed under this section must be accompanied by the Installation Information. But this requirement does not apply if neither you nor any third party retains the ability to install modified object code on the User Product (for example, the work has been installed in ROM).\n\n"
"The requirement to provide Installation Information does not include a requirement to continue to provide support service, warranty, or updates for a work that has been modified or installed by the recipient, or for the User Product in which it has been modified or installed. Access to a network may be denied when the modification itself materially and adversely affects the operation of the network or violates the rules and protocols for communication across the network.\n\n"
"Corresponding Source conveyed, and Installation Information provided, in accord with this section must be in a format that is publicly documented (and with an implementation available to the public in source code form), and must require no special password or key for unpacking, reading or copying.\n\n"
"7. Additional Terms.\n\n"
"“Additional permissions” are terms that supplement the terms of this License by making exceptions from one or more of its conditions. Additional permissions that are applicable to the entire Program shall be treated as though they were included in this License, to the extent that they are valid under applicable law. If additional permissions apply only to part of the Program, that part may be used separately under those permissions, but the entire Program remains governed by this License without regard to the additional permissions.\n\n"
"When you convey a copy of a covered work, you may at your option remove any additional permissions from that copy, or from any part of it. (Additional permissions may be written to require their own removal in certain cases when you modify the work.) You may place additional permissions on material, added by you to a covered work, for which you have or can give appropriate copyright permission.\n\n"
"Notwithstanding any other provision of this License, for material you add to a covered work, you may (if authorized by the copyright holders of that material) supplement the terms of this License with terms:\n\n"
"    a) Disclaiming warranty or limiting liability differently from the terms of sections 15 and 16 of this License; or\n\n"
"    b) Requiring preservation of specified reasonable legal notices or author attributions in that material or in the Appropriate Legal Notices displayed by works containing it; or\n\n"
"    c) Prohibiting misrepresentation of the origin of that material, or requiring that modified versions of such material be marked in reasonable ways as different from the original version; or\n\n"
"    d) Limiting the use for publicity purposes of names of licensors or authors of the material; or\n\n"
"    e) Declining to grant rights under trademark law for use of some trade names, trademarks, or service marks; or\n\n"
"    f) Requiring indemnification of licensors and authors of that material by anyone who conveys the material (or modified versions of it) with contractual assumptions of liability to the recipient, for any liability that these contractual assumptions directly impose on those licensors and authors.\n\n"
"All other non-permissive additional terms are considered “further restrictions” within the meaning of section 10. If the Program as you received it, or any part of it, contains a notice stating that it is governed by this License along with a term that is a further restriction, you may remove that term. If a license document contains a further restriction but permits relicensing or conveying under this License, you may add to a covered work material governed by the terms of that license document, provided that the further restriction does not survive such relicensing or conveying.\n\n"
"If you add terms to a covered work in accord with this section, you must place, in the relevant source files, a statement of the additional terms that apply to those files, or a notice indicating where to find the applicable terms.\n\n"
"Additional terms, permissive or non-permissive, may be stated in the form of a separately written license, or stated as exceptions; the above requirements apply either way.\n\n"
"8. Termination.\n\n"
"You may not propagate or modify a covered work except as expressly provided under this License. Any attempt otherwise to propagate or modify it is void, and will automatically terminate your rights under this License (including any patent licenses granted under the third paragraph of section 11).\n\n"
"However, if you cease all violation of this License, then your license from a particular copyright holder is reinstated (a) provisionally, unless and until the copyright holder explicitly and finally terminates your license, and (b) permanently, if the copyright holder fails to notify you of the violation by some reasonable means prior to 60 days after the cessation.\n\n"
"Moreover, your license from a particular copyright holder is reinstated permanently if the copyright holder notifies you of the violation by some reasonable means, this is the first time you have received notice of violation of this License (for any work) from that copyright holder, and you cure the violation prior to 30 days after your receipt of the notice.\n\n"
"Termination of your rights under this section does not terminate the licenses of parties who have received copies or rights from you under this License. If your rights have been terminated and not permanently reinstated, you do not qualify to receive new licenses for the same material under section 10.\n\n"
"9. Acceptance Not Required for Having Copies.\n\n"
"You are not required to accept this License in order to receive or run a copy of the Program. Ancillary propagation of a covered work occurring solely as a consequence of using peer-to-peer transmission to receive a copy likewise does not require acceptance. However, nothing other than this License grants you permission to propagate or modify any covered work. These actions infringe copyright if you do not accept this License. Therefore, by modifying or propagating a covered work, you indicate your acceptance of this License to do so.\n\n"
"10. Automatic Licensing of Downstream Recipients.\n\n"
"Each time you convey a covered work, the recipient automatically receives a license from the original licensors, to run, modify and propagate that work, subject to this License. You are not responsible for enforcing compliance by third parties with this License.\n\n"
"An “entity transaction” is a transaction transferring control of an organization, or substantially all assets of one, or subdividing an organization, or merging organizations. If propagation of a covered work results from an entity transaction, each party to that transaction who receives a copy of the work also receives whatever licenses to the work the party's predecessor in interest had or could give under the previous paragraph, plus a right to possession of the Corresponding Source of the work from the predecessor in interest, if the predecessor has it or can get it with reasonable efforts.\n\n"
"You may not impose any further restrictions on the exercise of the rights granted or affirmed under this License. For example, you may not impose a license fee, royalty, or other charge for exercise of rights granted under this License, and you may not initiate litigation (including a cross-claim or counterclaim in a lawsuit) alleging that any patent claim is infringed by making, using, selling, offering for sale, or importing the Program or any portion of it.\n\n"
"11. Patents.\n\n"
"A “contributor” is a copyright holder who authorizes use under this License of the Program or a work on which the Program is based. The work thus licensed is called the contributor's “contributor version”.\n\n"
"A contributor's “essential patent claims” are all patent claims owned or controlled by the contributor, whether already acquired or hereafter acquired, that would be infringed by some manner, permitted by this License, of making, using, or selling its contributor version, but do not include claims that would be infringed only as a consequence of further modification of the contributor version. For purposes of this definition, “control” includes the right to grant patent sublicenses in a manner consistent with the requirements of this License.\n\n"
"Each contributor grants you a non-exclusive, worldwide, royalty-free patent license under the contributor's essential patent claims, to make, use, sell, offer for sale, import and otherwise run, modify and propagate the contents of its contributor version.\n\n"
"In the following three paragraphs, a “patent license” is any express agreement or commitment, however denominated, not to enforce a patent (such as an express permission to practice a patent or covenant not to sue for patent infringement). To “grant” such a patent license to a party means to make such an agreement or commitment not to enforce a patent against the party.\n\n"
"If you convey a covered work, knowingly relying on a patent license, and the Corresponding Source of the work is not available for anyone to copy, free of charge and under the terms of this License, through a publicly available network server or other readily accessible means, then you must either (1) cause the Corresponding Source to be so available, or (2) arrange to deprive yourself of the benefit of the patent license for this particular work, or (3) arrange, in a manner consistent with the requirements of this License, to extend the patent license to downstream recipients. “Knowingly relying” means you have actual knowledge that, but for the patent license, your conveying the covered work in a country, or your recipient's use of the covered work in a country, would infringe one or more identifiable patents in that country that you have reason to believe are valid.\n\n"
"If, pursuant to or in connection with a single transaction or arrangement, you convey, or propagate by procuring conveyance of, a covered work, and grant a patent license to some of the parties receiving the covered work authorizing them to use, propagate, modify or convey a specific copy of the covered work, then the patent license you grant is automatically extended to all recipients of the covered work and works based on it.\n\n"
"A patent license is “discriminatory” if it does not include within the scope of its coverage, prohibits the exercise of, or is conditioned on the non-exercise of one or more of the rights that are specifically granted under this License. You may not convey a covered work if you are a party to an arrangement with a third party that is in the business of distributing software, under which you make payment to the third party based on the extent of your activity of conveying the work, and under which the third party grants, to any of the parties who would receive the covered work from you, a discriminatory patent license (a) in connection with copies of the covered work conveyed by you (or copies made from those copies), or (b) primarily for and in connection with specific products or compilations that contain the covered work, unless you entered into that arrangement, or that patent license was granted, prior to 28 March 2007.\n\n"
"Nothing in this License shall be construed as excluding or limiting any implied license or other defenses to infringement that may otherwise be available to you under applicable patent law.\n\n"
"12. No Surrender of Others' Freedom.\n\n"
"If conditions are imposed on you (whether by court order, agreement or otherwise) that contradict the conditions of this License, they do not excuse you from the conditions of this License. If you cannot convey a covered work so as to satisfy simultaneously your obligations under this License and any other pertinent obligations, then as a consequence you may not convey it at all. For example, if you agree to terms that obligate you to collect a royalty for further conveying from those to whom you convey the Program, the only way you could satisfy both those terms and this License would be to refrain entirely from conveying the Program.\n\n"
"13. Use with the GNU Affero General Public License.\n\n"
"Notwithstanding any other provision of this License, you have permission to link or combine any covered work with a work licensed under version 3 of the GNU Affero General Public License into a single combined work, and to convey the resulting work. The terms of this License will continue to apply to the part which is the covered work, but the special requirements of the GNU Affero General Public License, section 13, concerning interaction through a network will apply to the combination as such.\n\n"
"14. Revised Versions of this License.\n\n"
"The Free Software Foundation may publish revised and/or new versions of the GNU General Public License from time to time. Such new versions will be similar in spirit to the present version, but may differ in detail to address new problems or concerns.\n\n"
"Each version is given a distinguishing version number. If the Program specifies that a certain numbered version of the GNU General Public License “or any later version” applies to it, you have the option of following the terms and conditions either of that numbered version or of any later version published by the Free Software Foundation. If the Program does not specify a version number of the GNU General Public License, you may choose any version ever published by the Free Software Foundation.\n\n"
"If the Program specifies that a proxy can decide which future versions of the GNU General Public License can be used, that proxy's public statement of acceptance of a version permanently authorizes you to choose that version for the Program.\n\n"
"Later license versions may give you additional or different permissions. However, no additional obligations are imposed on any author or copyright holder as a result of your choosing to follow a later version.\n\n"
"15. Disclaimer of Warranty.\n\n"
"THERE IS NO WARRANTY FOR THE PROGRAM, TO THE EXTENT PERMITTED BY APPLICABLE LAW. EXCEPT WHEN OTHERWISE STATED IN WRITING THE COPYRIGHT HOLDERS AND/OR OTHER PARTIES PROVIDE THE PROGRAM “AS IS” WITHOUT WARRANTY OF ANY KIND, EITHER EXPRESSED OR IMPLIED, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE. THE ENTIRE RISK AS TO THE QUALITY AND PERFORMANCE OF THE PROGRAM IS WITH YOU. SHOULD THE PROGRAM PROVE DEFECTIVE, YOU ASSUME THE COST OF ALL NECESSARY SERVICING, REPAIR OR CORRECTION.\n\n"
"16. Limitation of Liability.\n\n"
"IN NO EVENT UNLESS REQUIRED BY APPLICABLE LAW OR AGREED TO IN WRITING WILL ANY COPYRIGHT HOLDER, OR ANY OTHER PARTY WHO MODIFIES AND/OR CONVEYS THE PROGRAM AS PERMITTED ABOVE, BE LIABLE TO YOU FOR DAMAGES, INCLUDING ANY GENERAL, SPECIAL, INCIDENTAL OR CONSEQUENTIAL DAMAGES ARISING OUT OF THE USE OR INABILITY TO USE THE PROGRAM (INCLUDING BUT NOT LIMITED TO LOSS OF DATA OR DATA BEING RENDERED INACCURATE OR LOSSES SUSTAINED BY YOU OR THIRD PARTIES OR A FAILURE OF THE PROGRAM TO OPERATE WITH ANY OTHER PROGRAMS), EVEN IF SUCH HOLDER OR OTHER PARTY HAS BEEN ADVISED OF THE POSSIBILITY OF SUCH DAMAGES.\n\n"
"17. Interpretation of Sections 15 and 16.\n\n"
"If the disclaimer of warranty and limitation of liability provided above cannot be given local legal effect according to their terms, reviewing courts shall apply local law that most closely approximates an absolute waiver of all civil liability in connection with the Program, unless a warranty or assumption of liability accompanies a copy of the Program in return for a fee.\n\n"
"END OF TERMS AND CONDITIONS\n\n"
"How to Apply These Terms to Your New Programs\n\n"
"If you develop a new program, and you want it to be of the greatest possible use to the public, the best way to achieve this is to make it free software which everyone can redistribute and change under these terms.\n\n"
"To do so, attach the following notices to the program. It is safest to attach them to the start of each source file to most effectively state the exclusion of warranty; and each file should have at least the “copyright” line and a pointer to where the full notice is found.\n\n"
"ScreenForge: a Linux utility for managing display settings.\n"
"Copyright (C) 2025 Maksym Nazar.\n"
"Created with the assistance of ChatGPT, Perplexity, and Claude.\n\n"
"This program is free software: you can redistribute it and/or modify\n"
"it under the terms of the GNU General Public License as published by\n"
"the Free Software Foundation, either version 3 of the License, or\n"
"(at your option) any later version.\n\n"
"This program is distributed in the hope that it will be useful,\n"
"but WITHOUT ANY WARRANTY; without even the implied warranty of\n"
"MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the\n"
"GNU General Public License for more details.\n\n"
"You should have received a copy of the GNU General Public License\n"
"along with this program.  If not, see <https://www.gnu.org/licenses/>.\n\n"
"For inquiries, please contact us at:\n"
"Email: maximkursua@gmail.com\n");
}

static void exit_clicked_cb(GtkButton *btn, gpointer data) {
    gtk_main_quit();
}

static void set_window_icon_placeholder(GtkWindow *win) {
    static const unsigned char icon_data[] = {0};
    (void)icon_data;
    (void)win;
}

int main(int argc, char **argv) {
    gtk_init(&argc, &argv);

    GtkWidget *window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(window), "ScreenForge v1.1");
    gtk_window_set_default_size(GTK_WINDOW(window), 870, 580);
    gtk_window_set_position(GTK_WINDOW(window), GTK_WIN_POS_CENTER);
    g_signal_connect(window, "destroy", G_CALLBACK(gtk_main_quit), NULL);
    set_window_icon_placeholder(GTK_WINDOW(window));

    GtkWidget *outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_set_border_width(GTK_CONTAINER(outer), 20);
    gtk_container_add(GTK_CONTAINER(window), outer);
    gtk_widget_set_hexpand(outer, TRUE);
    gtk_widget_set_vexpand(outer, TRUE);

    GtkWidget *align = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_halign(align, GTK_ALIGN_FILL);
    gtk_widget_set_valign(align, GTK_ALIGN_FILL);
    gtk_widget_set_hexpand(align, TRUE);
    gtk_widget_set_vexpand(align, TRUE);
    gtk_container_add(GTK_CONTAINER(outer), align);

    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_widget_set_hexpand(scroll, TRUE);
    gtk_widget_set_vexpand(scroll, TRUE);
    gtk_container_add(GTK_CONTAINER(align), scroll);

    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 10);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 10);
    gtk_container_add(GTK_CONTAINER(scroll), grid);

    char monitors[MAX_MONITORS][32];
    int mon_count = 0;
    get_monitors(monitors, &mon_count);
    if (mon_count == 0) {
        strcpy(monitors[0], "eDP");
        mon_count = 1;
    }

    GtkWidget *h_mon = gtk_label_new("Monitor");
    gtk_grid_attach(GTK_GRID(grid), h_mon, 0, 0, 1, 1);
    GtkWidget *h_label = gtk_label_new("Parameter");
    gtk_grid_attach(GTK_GRID(grid), h_label, 1, 0, 1, 1);
    GtkWidget *h_control = gtk_label_new("Control");
    gtk_grid_attach(GTK_GRID(grid), h_control, 2, 0, 1, 1);
    GtkWidget *h_value = gtk_label_new("Value (Enter to apply)");
    gtk_grid_attach(GTK_GRID(grid), h_value, 3, 0, 1, 1);

    double bl_val = 128, bl_def = 255;
    double br_val = 1.0, br_def = 1.0;
    double gm_val = 1.0, gm_def = 1.0;
    double dpi_val = 90, dpi_def = 96;
    double sc_val = 1.0, sc_def = 1.0;

    char backlights[10][32]; int bl_count = 0;
    get_backlights(backlights, &bl_count);
    if (bl_count > 0) {
        int v = read_backlight_value(backlights[0]);
        if (v >= 0) bl_val = v;
    }

    int row = 1;

    GtkComboBoxText *mon_combo_brightness = create_monitor_combo(GTK_GRID(grid), row, monitors, mon_count, 0);
    create_slider_row_with_monitor(GTK_GRID(grid), row, mon_combo_brightness, "Brightness", 0.0, 2.0, 0.01, set_xrandr_brightness_mon, br_val, br_def);
    row++;

    GtkComboBoxText *mon_combo_gamma = create_monitor_combo(GTK_GRID(grid), row, monitors, mon_count, 0);
    create_slider_row_with_monitor(GTK_GRID(grid), row, mon_combo_gamma, "Gamma", 0.5, 2.0, 0.01, set_xrandr_gamma_mon, gm_val, gm_def);
    row++;

    GtkComboBoxText *mon_combo_backlight = create_monitor_combo(GTK_GRID(grid), row, monitors, mon_count, 0);
    create_slider_row_with_monitor(GTK_GRID(grid), row, mon_combo_backlight, "Backlight", 0, 255, 1, set_brightness_backlight_mon, bl_val, bl_def);
    row++;

    GtkComboBoxText *mon_combo_dpi = create_monitor_combo(GTK_GRID(grid), row, monitors, mon_count, 0);
    create_slider_row_with_monitor(GTK_GRID(grid), row, mon_combo_dpi, "DPI (global)", 50, 300, 1, set_dpi_mon, dpi_val, dpi_def);
    row++;

    GtkComboBoxText *mon_combo_scale = create_monitor_combo(GTK_GRID(grid), row, monitors, mon_count, 0);
    create_slider_row_with_monitor(GTK_GRID(grid), row, mon_combo_scale, "Scale", 0.5, 3.0, 0.01, set_scale_mon, sc_val, sc_def);
    row++;

    GtkComboBoxText *mon_combo_res = create_monitor_combo(GTK_GRID(grid), row, monitors, mon_count, 0);
    GtkWidget *lbl_res = gtk_label_new("Resolution");
    gtk_grid_attach(GTK_GRID(grid), lbl_res, 1, row, 1, 1);
    GtkWidget *res_combo = gtk_combo_box_text_new();
    gtk_grid_attach(GTK_GRID(grid), res_combo, 2, row, 1, 1);
    GtkWidget *res_entry = gtk_entry_new();
    gtk_widget_set_size_request(res_entry, 180, -1);
    gtk_entry_set_text(GTK_ENTRY(res_entry), "");
    gtk_widget_set_sensitive(res_entry, FALSE);
    gtk_grid_attach(GTK_GRID(grid), res_entry, 3, row, 1, 1);

    ResRateData *rr = g_malloc0(sizeof(ResRateData));
    rr->res_combo = GTK_COMBO_BOX_TEXT(res_combo);
    rr->monitor_combo = GTK_COMBO_BOX_TEXT(mon_combo_res);
    strncpy(rr->monitor, monitors[0], sizeof(rr->monitor) - 1);
    
    char current_mode[32] = {0};
    char current_rate[32] = {0};
    set_current_mode_rate_from_xrandr(rr->monitor, current_mode, sizeof(current_mode), current_rate, sizeof(current_rate));
    
    get_display_modes_and_rates(rr->monitor, rr);
    
    int current_mode_idx = 0;
    for (int i = 0; i < rr->mode_count; i++) {
        gtk_combo_box_text_append_text(rr->res_combo, rr->modes[i]);
        if (current_mode[0] && strcmp(rr->modes[i], current_mode) == 0) {
            current_mode_idx = i;
        }
    }
    if (rr->mode_count > 0) gtk_combo_box_set_active(GTK_COMBO_BOX(rr->res_combo), current_mode_idx);
    
    if (current_mode[0]) {
        gtk_entry_set_text(GTK_ENTRY(res_entry), current_mode);
    }
    
    row++;

    GtkComboBoxText *mon_combo_rate = create_monitor_combo(GTK_GRID(grid), row, monitors, mon_count, 0);
    GtkWidget *lbl_rate = gtk_label_new("Refresh Rate");
    gtk_grid_attach(GTK_GRID(grid), lbl_rate, 1, row, 1, 1);
    GtkWidget *rate_combo = gtk_combo_box_text_new();
    gtk_grid_attach(GTK_GRID(grid), rate_combo, 2, row, 1, 1);
    GtkWidget *rate_entry = gtk_entry_new();
    gtk_widget_set_size_request(rate_entry, 180, -1);
    gtk_entry_set_text(GTK_ENTRY(rate_entry), "");
    gtk_widget_set_sensitive(rate_entry, FALSE);
    gtk_grid_attach(GTK_GRID(grid), rate_entry, 3, row, 1, 1);

    rr->rate_combo = GTK_COMBO_BOX_TEXT(rate_combo);
    
    gtk_combo_box_text_append_text(rr->rate_combo, "Auto");
    for (int i = 0; i < rr->rate_count[current_mode_idx]; i++) {
        double v = rr->rates[current_mode_idx][i];
        if (v > 0.0) {
            char buf[32];
            snprintf(buf, sizeof(buf), "%.2f", v);
            gtk_combo_box_text_append_text(rr->rate_combo, buf);
        }
    }
    
    int current_rate_idx = 0;
    if (current_rate[0]) {
        for (int i = 0; i < rr->rate_count[current_mode_idx]; i++) {
            char buf[32];
            snprintf(buf, sizeof(buf), "%.2f", rr->rates[current_mode_idx][i]);
            if (strcmp(buf, current_rate) == 0) {
                current_rate_idx = i + 1;
                break;
            }
        }
        gtk_entry_set_text(GTK_ENTRY(rate_entry), current_rate);
    }
    gtk_combo_box_set_active(GTK_COMBO_BOX(rr->rate_combo), current_rate_idx);
    
    g_signal_connect(rr->monitor_combo, "changed", G_CALLBACK(rr_monitor_changed_cb_wrap), rr);
    g_signal_connect(mon_combo_rate, "changed", G_CALLBACK(rr_monitor_changed_cb_wrap), rr);
    g_signal_connect(rr->res_combo, "changed", G_CALLBACK(resolution_changed_cb), rr);
    g_signal_connect(rr->rate_combo, "changed", G_CALLBACK(rate_changed_generic_cb), rr);
    row++;

    GtkComboBoxText *mon_combo_rot = create_monitor_combo(GTK_GRID(grid), row, monitors, mon_count, 0);
    GtkWidget *lbl_rot = gtk_label_new("Rotation");
    gtk_grid_attach(GTK_GRID(grid), lbl_rot, 1, row, 1, 1);
    GtkWidget *rot_combo = gtk_combo_box_text_new();
    const char *rot_opts[] = {"normal", "left", "right", "inverted"};
    for (int i = 0; i < 4; i++) {
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(rot_combo), rot_opts[i]);
    }
    gtk_grid_attach(GTK_GRID(grid), rot_combo, 2, row, 1, 1);
    GtkWidget *rot_entry = gtk_entry_new();
    gtk_widget_set_size_request(rot_entry, 180, -1);
    gtk_widget_set_sensitive(rot_entry, FALSE);
    gtk_grid_attach(GTK_GRID(grid), rot_entry, 3, row, 1, 1);
    
    rr->rot_combo = GTK_COMBO_BOX_TEXT(rot_combo);
    
    char current_rotation[16] = {0};
    get_current_rotation(rr->monitor, current_rotation, sizeof(current_rotation));
    
    if (current_rotation[0] == '\0') {
        strcpy(current_rotation, "normal");
    }
    
    int rot_idx = 0;
    for (int i = 0; i < 4; i++) {
        if (strcmp(rot_opts[i], current_rotation) == 0) {
            rot_idx = i;
            break;
        }
    }
    gtk_combo_box_set_active(GTK_COMBO_BOX(rot_combo), rot_idx);
    gtk_entry_set_text(GTK_ENTRY(rot_entry), current_rotation);
    
    g_signal_connect(mon_combo_rot, "changed", G_CALLBACK(rr_monitor_changed_cb_wrap), rr);
    g_signal_connect(rot_combo, "changed", G_CALLBACK(rotation_changed_cb), rr);
    row++;

    GtkComboBoxText *mon_combo_onoff = create_monitor_combo(GTK_GRID(grid), row, monitors, mon_count, 0);
    GtkWidget *lbl_onoff = gtk_label_new("Monitor State");
    gtk_grid_attach(GTK_GRID(grid), lbl_onoff, 1, row, 1, 1);
    GtkWidget *onoff_combo = gtk_combo_box_text_new();
    const char *onoff_opts[] = {"On", "Off"};
    for (int i = 0; i < 2; i++) {
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(onoff_combo), onoff_opts[i]);
    }
    gtk_grid_attach(GTK_GRID(grid), onoff_combo, 2, row, 1, 1);
    GtkWidget *onoff_entry = gtk_entry_new();
    gtk_widget_set_size_request(onoff_entry, 180, -1);
    gtk_entry_set_text(GTK_ENTRY(onoff_entry), "");
    gtk_widget_set_sensitive(onoff_entry, FALSE);
    gtk_grid_attach(GTK_GRID(grid), onoff_entry, 3, row, 1, 1);
    
    rr->onoff_combo = GTK_COMBO_BOX_TEXT(onoff_combo);
    
    char current_state[8] = {0};
    get_current_state(rr->monitor, current_state, sizeof(current_state));
    int state_idx = 0;
    if (current_state[0]) {
        if (strcmp(current_state, "Off") == 0) {
            state_idx = 1;
        }
        gtk_entry_set_text(GTK_ENTRY(onoff_entry), current_state);
    } else {
        strcpy(current_state, "On");
        gtk_entry_set_text(GTK_ENTRY(onoff_entry), "On");
    }
    gtk_combo_box_set_active(GTK_COMBO_BOX(onoff_combo), state_idx);
    
    g_signal_connect(mon_combo_onoff, "changed", G_CALLBACK(rr_monitor_changed_cb_wrap), rr);
    g_signal_connect(onoff_combo, "changed", G_CALLBACK(onoff_changed_cb), rr);
    row++;

    GtkComboBoxText *mon_combo_hdr = create_monitor_combo(GTK_GRID(grid), row, monitors, mon_count, 0);
    GtkWidget *lbl_hdr = gtk_label_new("HDR");
    gtk_grid_attach(GTK_GRID(grid), lbl_hdr, 1, row, 1, 1);
    GtkWidget *hdr_combo = gtk_combo_box_text_new();
    const char *hdr_opts[] = {"Disabled", "Enabled"};
    for (int i = 0; i < 2; i++) {
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(hdr_combo), hdr_opts[i]);
    }
    gtk_grid_attach(GTK_GRID(grid), hdr_combo, 2, row, 1, 1);
    GtkWidget *hdr_entry = gtk_entry_new();
    gtk_widget_set_size_request(hdr_entry, 180, -1);
    gtk_entry_set_text(GTK_ENTRY(hdr_entry), "Disabled");
    gtk_widget_set_sensitive(hdr_entry, FALSE);
    gtk_grid_attach(GTK_GRID(grid), hdr_entry, 3, row, 1, 1);
    
    rr->hdr_combo = GTK_COMBO_BOX_TEXT(hdr_combo);
    gtk_combo_box_set_active(GTK_COMBO_BOX(hdr_combo), 0);
    
    g_signal_connect(mon_combo_hdr, "changed", G_CALLBACK(rr_monitor_changed_cb_wrap), rr);
    g_signal_connect(hdr_combo, "changed", G_CALLBACK(hdr_changed_cb), rr);
    row++;

    GtkWidget *sep = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_grid_attach(GTK_GRID(grid), sep, 0, row, 4, 1);
    row++;

    GtkWidget *btn_about = gtk_button_new_with_label("About");
    gtk_grid_attach(GTK_GRID(grid), btn_about, 0, row + 1, 1, 1);
    g_signal_connect(btn_about, "clicked", G_CALLBACK(about_clicked_cb), NULL);

    GtkWidget *btn_terms = gtk_button_new_with_label("Terms of Use");
    gtk_grid_attach(GTK_GRID(grid), btn_terms, 1, row + 1, 1, 1);
    g_signal_connect(btn_terms, "clicked", G_CALLBACK(terms_clicked_cb), NULL);

    GtkWidget *btn_license = gtk_button_new_with_label("License");
    gtk_grid_attach(GTK_GRID(grid), btn_license, 2, row + 1, 1, 1);
    g_signal_connect(btn_license, "clicked", G_CALLBACK(license_clicked_cb), NULL);

    GtkWidget *btn_exit = gtk_button_new_with_label("Exit");
    gtk_grid_attach(GTK_GRID(grid), btn_exit, 3, row + 1, 1, 1);
    g_signal_connect(btn_exit, "clicked", G_CALLBACK(exit_clicked_cb), NULL);

    gtk_widget_show_all(window);
    gtk_main();
    g_free(rr);
    return 0;
}
