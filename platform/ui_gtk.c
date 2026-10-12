/* GTK4/Wayland 原生界面，主线程只消费快照和提交任务，托盘通过 StatusNotifierItem 接入 KDE。 */
#include "runtime.h"
#include <gtk/gtk.h>
#include <sodium.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>

typedef struct gtk_ui {
    GtkApplication *app;
    GtkWidget *window, *settings, *document, *users, *messages, *detail, *files, *compose, *preview, *preview_label, *preview_scroll, *compose_scroll, *status, *recipient,
        *overview, *search, *find, *from, *to;
    GtkWidget *url, *login, *password, *ca, *send, *delete_button, *export_button;
    GtkWidget *notify, *attention;
    drop_runtime *runtime;
    drop_snapshot *snapshot;
    char peer[DROP_ID_CAP], selected[DROP_ID_CAP];
    char *paths[DROP_MAX_ATTACHMENTS];
    int count, row, file, page, rebuilding, quitting, hidden, send_pending;
    uint64_t pending_finished, pending_serial;
    int starts[DROP_HISTORY_PAGE], ends[DROP_HISTORY_PAGE], file_starts[DROP_HISTORY_PAGE][DROP_MAX_ATTACHMENTS], file_ends[DROP_HISTORY_PAGE][DROP_MAX_ATTACHMENTS];
    GDBusConnection *bus;
    guint tray_registration, watcher_subscription, poll;
    GDBusNodeInfo *tray_info;
    GVariant *tray_pixmap;
    int tray_available;
} gtk_ui;

static char *utf8(const WCHAR *text) {
    size_t capacity = (wcslen(text) + 1) * 4;
    char *result = malloc(capacity);
    if (result && !drop_to_utf8(text, result, capacity)) {
        free(result);
        result = NULL;
    }
    return result;
}
static GtkWidget *label(const char *text) {
    GtkWidget *widget = gtk_label_new(text);
    gtk_label_set_xalign(GTK_LABEL(widget), 0);
    gtk_label_set_ellipsize(GTK_LABEL(widget), PANGO_ELLIPSIZE_END);
    return widget;
}
static GtkWidget *scrolled(GtkWidget *child, int height) {
    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), child);
    if (height) gtk_widget_set_size_request(scroll, -1, height);
    return scroll;
}
static GtkWidget *text_view(int editable) {
    GtkWidget *view = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(view), editable);
    gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(view), editable);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(view), GTK_WRAP_WORD_CHAR);
    gtk_text_view_set_left_margin(GTK_TEXT_VIEW(view), 4);
    gtk_text_view_set_right_margin(GTK_TEXT_VIEW(view), 4);
    gtk_text_view_set_top_margin(GTK_TEXT_VIEW(view), 2);
    return view;
}
/* 失败仍由界面拥有任务，统一清理秘密和路径，不能把提交失败当作已保存。 */
static int submit(gtk_ui *ui, drop_task *task) {
    if (task && drop_runtime_submit(ui->runtime, task)) return 1;
    drop_task_free(task);
    gtk_label_set_text(GTK_LABEL(ui->status), "任务队列忙，请稍后重试。");
    return 0;
}
static drop_task *task_new(drop_task_kind kind) {
    drop_task *task = calloc(1, sizeof(*task));
    if (task) task->kind = kind;
    return task;
}
static void sync_clicked(GtkButton *button, gpointer context) {
    (void)button;
    submit(context, task_new(DROP_TASK_SYNC));
}
static void view_request(gtk_ui *ui) {
    drop_task *task = task_new(DROP_TASK_VIEW);
    if (!task) return;
    strcpy_s(task->peer, DROP_ID_CAP, ui->peer);
    task->page = ui->page;
    task->text = strdup(gtk_editable_get_text(GTK_EDITABLE(ui->search)));
    const char *dates[2] = {gtk_editable_get_text(GTK_EDITABLE(ui->from)), gtk_editable_get_text(GTK_EDITABLE(ui->to))};
    int64_t times[2] = {0, INT64_MAX};
    for (int i = 0; i < 2; ++i) {
        if (!*dates[i]) continue;
        unsigned year, month, day;
        char trailing;
        if (sscanf(dates[i], "%u-%u-%u%c", &year, &month, &day, &trailing) != 3 || year < 1970 || year > 9999 || month < 1 || month > 12 || day < 1 || day > 31) {
            gtk_label_set_text(GTK_LABEL(ui->status), "日期格式：YYYY-MM-DD。");
            drop_task_free(task);
            return;
        }
        GDateTime *date = g_date_time_new_local((int)year, (int)month, (int)day, i ? 23 : 0, i ? 59 : 0, i ? 59 : 0);
        if (!date) {
            drop_task_free(task);
            return;
        }
        times[i] = g_date_time_to_unix(date);
        g_date_time_unref(date);
    }
    task->from = times[0];
    task->to = times[1];
    submit(ui, task);
}
static void filter_clicked(GtkButton *button, gpointer context) {
    (void)button;
    gtk_ui *ui = context;
    ui->page = 0;
    view_request(ui);
}
static void previous_clicked(GtkButton *button, gpointer context) {
    (void)button;
    gtk_ui *ui = context;
    if (ui->page) --ui->page;
    view_request(ui);
}
static void next_clicked(GtkButton *button, gpointer context) {
    (void)button;
    gtk_ui *ui = context;
    if (ui->snapshot && ui->snapshot->history && ui->snapshot->history->more) {
        ++ui->page;
        view_request(ui);
    }
}
static void find_clicked(GtkButton *button, gpointer context) {
    (void)button;
    gtk_ui *ui = context;
    drop_task *task = task_new(DROP_TASK_FIND);
    if (task) task->login = strdup(gtk_editable_get_text(GTK_EDITABLE(ui->find)));
    submit(ui, task);
}
static void peer_selected(GtkListBox *box, GtkListBoxRow *row, gpointer context) {
    (void)box;
    gtk_ui *ui = context;
    const char *id = row ? g_object_get_data(G_OBJECT(row), "drop-peer") : NULL;
    if (ui->rebuilding || !id) return;
    strcpy_s(ui->peer, DROP_ID_CAP, id);
    ui->page = 0;
    gtk_widget_set_sensitive(ui->send, FALSE);
    view_request(ui);
}
/* 文件预览使用平台文件图标与名称，每个按钮仅删除草稿项，不操作用户原文件。 */
static void preview_update(gtk_ui *ui);
static void file_remove(GtkButton *button, gpointer context) {
    gtk_ui *ui = context;
    int index = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(button), "drop-index"));
    if (ui->send_pending) return;
    if (index < 0 || index >= ui->count) return;
    free(ui->paths[index]);
    for (int i = index; i + 1 < ui->count; ++i)
        ui->paths[i] = ui->paths[i + 1];
    --ui->count;
    preview_update(ui);
}
static void preview_update(gtk_ui *ui) {
    GtkWidget *child;
    char heading[80];
    g_snprintf(heading, sizeof(heading), "待发送附件（%d）", ui->count);
    if (ui->preview_label) gtk_label_set_text(GTK_LABEL(ui->preview_label), heading);
    while ((child = gtk_widget_get_first_child(ui->preview)))
        gtk_box_remove(GTK_BOX(ui->preview), child);
    for (int i = 0; i < ui->count; ++i) {
        GtkWidget *item = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 3);
        GFile *file = g_file_new_for_path(ui->paths[i]);
        GFileInfo *info = g_file_query_info(file, "standard::icon,standard::size", G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, NULL, NULL);
        GIcon *icon = info ? g_file_info_get_icon(info) : NULL;
        GtkWidget *image = icon ? gtk_image_new_from_gicon(icon) : gtk_image_new_from_icon_name("text-x-generic");
        gtk_image_set_pixel_size(GTK_IMAGE(image), 16);
        gtk_box_append(GTK_BOX(item), image);
        char *base = g_path_get_basename(ui->paths[i]);
        char *title = g_strdup_printf("%s · %llu 字节", base, (unsigned long long)(info ? g_file_info_get_size(info) : 0));
        GtkWidget *name = label(title), *remove = gtk_button_new_with_label("×");
        gtk_widget_set_hexpand(name, TRUE);
        gtk_widget_set_tooltip_text(name, title);
        gtk_label_set_max_width_chars(GTK_LABEL(name), 32);
        gtk_box_append(GTK_BOX(item), name);
        g_object_set_data(G_OBJECT(remove), "drop-index", GINT_TO_POINTER(i));
        g_signal_connect(remove, "clicked", G_CALLBACK(file_remove), ui);
        gtk_box_append(GTK_BOX(item), remove);
        gtk_box_append(GTK_BOX(ui->preview), item);
        g_free(base);
        g_free(title);
        if (info) g_object_unref(info);
        g_object_unref(file);
    }
    gtk_widget_set_visible(ui->preview, ui->count > 0);
}
static void add_path(gtk_ui *ui, GFile *file) {
    if (ui->send_pending) return;
    char *path = g_file_get_path(file);
    struct stat st;
    if (!path) return;
    if (ui->count == DROP_MAX_ATTACHMENTS || lstat(path, &st) || !S_ISREG(st.st_mode) || st.st_size < 0 || (uint64_t)st.st_size > UINT64_C(10737418240)) {
        gtk_label_set_text(GTK_LABEL(ui->status), "请选择普通文件，最多 32 个，每个不超过 10 GiB。");
        g_free(path);
        return;
    }
    for (int i = 0; i < ui->count; ++i)
        if (!strcmp(ui->paths[i], path)) {
            g_free(path);
            return;
        }
    ui->paths[ui->count++] = strdup(path);
    g_free(path);
}
static void picked(GObject *dialog, GAsyncResult *result, gpointer context) {
    gtk_ui *ui = context;
    GListModel *files = gtk_file_dialog_open_multiple_finish(GTK_FILE_DIALOG(dialog), result, NULL);
    if (files) {
        for (guint i = 0; i < g_list_model_get_n_items(files); ++i) {
            GFile *file = g_list_model_get_item(files, i);
            add_path(ui, file);
            g_object_unref(file);
        }
        g_object_unref(files);
        preview_update(ui);
    }
    g_object_unref(dialog);
}
static void add_clicked(GtkButton *button, gpointer context) {
    (void)button;
    gtk_ui *ui = context;
    GtkFileDialog *dialog = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dialog, "添加附件");
    gtk_file_dialog_open_multiple(dialog, GTK_WINDOW(ui->window), NULL, picked, ui);
}
static gboolean files_dropped(GtkDropTarget *target, const GValue *value, double x, double y, gpointer context) {
    (void)target;
    (void)x;
    (void)y;
    gtk_ui *ui = context;
    GdkFileList *list = g_value_get_boxed(value);
    for (GSList *item = gdk_file_list_get_files(list); item; item = item->next)
        add_path(ui, item->data);
    preview_update(ui);
    return TRUE;
}
static void clipboard_files(GObject *clipboard, GAsyncResult *result, gpointer context) {
    gtk_ui *ui = context;
    const GValue *value = gdk_clipboard_read_value_finish(GDK_CLIPBOARD(clipboard), result, NULL);
    if (value && G_VALUE_HOLDS(value, GDK_TYPE_FILE_LIST)) {
        for (GSList *item = gdk_file_list_get_files(g_value_get_boxed(value)); item; item = item->next)
            add_path(ui, item->data);
        preview_update(ui);
    }
}
static void send_clicked(GtkButton *button, gpointer context) {
    (void)button;
    gtk_ui *ui = context;
    if (!ui->snapshot || !ui->snapshot->ready || !drop_id_valid(ui->peer) || ui->send_pending) return;
    GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(ui->compose));
    GtkTextIter start, end;
    gtk_text_buffer_get_bounds(buffer, &start, &end);
    gchar *text = gtk_text_buffer_get_text(buffer, &start, &end, FALSE);
    if (!*text && !ui->count) {
        g_free(text);
        return;
    }
    drop_task *task = task_new(DROP_TASK_SEND);
    if (task) {
        task->text = strdup(text);
        strcpy_s(task->peer, DROP_ID_CAP, ui->peer);
        task->count = ui->count;
        if (ui->snapshot->history) strcpy_s(task->account, DROP_ID_CAP, ui->snapshot->history->self);
        for (int i = 0; i < ui->count; ++i)
            task->paths[i] = strdup(ui->paths[i]);
    }
    if (submit(ui, task)) {
        /* 草稿只有在后台确认独立副本和队列已提交后才清空，磁盘或文件失败时继续保留。 */
        ui->send_pending = 1;
        ui->pending_finished = ui->snapshot->send_finished;
        ui->pending_serial = ui->snapshot->send_serial;
        gtk_text_view_set_editable(GTK_TEXT_VIEW(ui->compose), FALSE);
        gtk_widget_set_sensitive(ui->send, FALSE);
    }
    g_free(text);
}
static gboolean compose_key(GtkEventControllerKey *controller, guint key, guint code, GdkModifierType state, gpointer context) {
    (void)controller;
    (void)code;
    gtk_ui *ui = context;
    if (!(state & GDK_CONTROL_MASK)) return FALSE;
    if (key == GDK_KEY_Return) {
        send_clicked(NULL, ui);
        return TRUE;
    }
    if (key == GDK_KEY_v || key == GDK_KEY_V) {
        GdkClipboard *clipboard = gtk_widget_get_clipboard(ui->compose);
        if (gdk_content_formats_contain_gtype(gdk_clipboard_get_formats(clipboard), GDK_TYPE_FILE_LIST)) {
            gdk_clipboard_read_value_async(clipboard, GDK_TYPE_FILE_LIST, G_PRIORITY_DEFAULT, NULL, clipboard_files, ui);
            return TRUE;
        }
    }
    return FALSE;
}
static void detail_show(gtk_ui *ui, int index, int file) {
    if (!ui->snapshot || !ui->snapshot->history || index < 0 || index >= ui->snapshot->history->count) return;
    drop_history_row *row = &ui->snapshot->history->rows[index];
    ui->row = index;
    ui->file = file;
    strcpy_s(ui->selected, DROP_ID_CAP, row->id);
    char *text = row->content ? utf8(row->content->text) : NULL;
    gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(ui->detail)), text ? text : "", -1);
    free(text);
    GtkWidget *child;
    while ((child = gtk_widget_get_first_child(ui->files)))
        gtk_list_box_remove(GTK_LIST_BOX(ui->files), child);
    if (row->content)
        for (int i = 0; i < row->content->count; ++i) {
            drop_file_view *f = &row->content->files[i];
            char *name = utf8(f->name), *line = g_strdup_printf("%s · %llu 字节 · %s", name ? name : "", (unsigned long long)f->total, f->complete ? "已缓存" : "待下载");
            GtkWidget *item = label(line);
            gtk_list_box_append(GTK_LIST_BOX(ui->files), item);
            free(name);
            g_free(line);
        }
    if (row->content && file >= 0 && file < row->content->count) gtk_list_box_select_row(GTK_LIST_BOX(ui->files), gtk_list_box_get_row_at_index(GTK_LIST_BOX(ui->files), file));
    gtk_widget_set_sensitive(ui->delete_button, row->deletable && ui->snapshot->history->deletion_supported);
    gtk_widget_set_sensitive(ui->export_button, row->content && row->content->count > 0);
    if (row->unread && !row->pending && !ui->rebuilding) {
        drop_task *task = task_new(DROP_TASK_READ);
        if (task) strcpy_s(task->message, DROP_ID_CAP, row->id);
        submit(ui, task);
    }
}
static void file_selected(GtkListBox *box, GtkListBoxRow *row, gpointer context) {
    (void)box;
    gtk_ui *ui = context;
    if (row) ui->file = gtk_list_box_row_get_index(row);
}
/* 完整正文窗口使用已经加载的文字，普通选择只更新附件，不重复占用主窗口空间。 */
static void full_text(gtk_ui *ui) {
    if (!*ui->selected) return;
    gtk_window_present(GTK_WINDOW(ui->document));
}
static void message_clicked(GtkGestureClick *gesture, int count, double x, double y, gpointer context) {
    (void)gesture;
    (void)count;
    gtk_ui *ui = context;
    GtkTextIter iter;
    int bx, by;
    gtk_text_view_window_to_buffer_coords(GTK_TEXT_VIEW(ui->messages), GTK_TEXT_WINDOW_WIDGET, (int)x, (int)y, &bx, &by);
    if (!gtk_text_view_get_iter_at_location(GTK_TEXT_VIEW(ui->messages), &iter, bx, by) || !ui->snapshot || !ui->snapshot->history) return;
    int offset = gtk_text_iter_get_offset(&iter);
    for (int i = 0; i < ui->snapshot->history->count; ++i) {
        if (offset < ui->starts[i] || offset >= ui->ends[i]) continue;
        int selected_file = -1;
        for (int j = 0; j < DROP_MAX_ATTACHMENTS; ++j)
            if (ui->file_ends[i][j] && offset >= ui->file_starts[i][j] && offset < ui->file_ends[i][j]) selected_file = j;
        detail_show(ui, i, selected_file);
        if (count == 2 && gtk_gesture_single_get_current_button(GTK_GESTURE_SINGLE(gesture)) != 3) full_text(ui);
        break;
    }
}
/* 右键先建立所属消息和附件的稳定选择，再显示与工具栏共用的命令，不能隐式导出。 */
static void message_context(GtkGestureClick *gesture, int count, double x, double y, gpointer context) {
    gtk_ui *ui = context;
    message_clicked(gesture, count, x, y, context);
    GMenu *menu = g_menu_new();
    g_menu_append(menu, "查看完整正文", "app.full-text");
    g_menu_append(menu, "导出所选附件", "app.export");
    g_menu_append(menu, "删除所选消息", "app.delete");
    GtkWidget *popover = gtk_popover_menu_new_from_model(G_MENU_MODEL(menu));
    g_object_unref(menu);
    gtk_widget_set_parent(popover, ui->messages);
    GdkRectangle point = {(int)x, (int)y, 1, 1};
    gtk_popover_set_pointing_to(GTK_POPOVER(popover), &point);
    g_signal_connect_swapped(popover, "closed", G_CALLBACK(gtk_widget_unparent), popover);
    gtk_popover_popup(GTK_POPOVER(popover));
}
/* 确认时保存稳定消息 ID，不能在异步对话结束时依赖可能已刷新的行号。 */
typedef struct confirm_context {
    gtk_ui *ui;
    drop_task *task;
} confirm_context;
static void confirmed(GObject *dialog, GAsyncResult *result, gpointer context) {
    confirm_context *confirm = context;
    int choice = gtk_alert_dialog_choose_finish(GTK_ALERT_DIALOG(dialog), result, NULL);
    if (choice == 1)
        submit(confirm->ui, confirm->task);
    else
        drop_task_free(confirm->task);
    free(confirm);
    g_object_unref(dialog);
}
static void confirm_task(gtk_ui *ui, const char *title, const char *detail, drop_task *task) {
    if (!task) return;
    confirm_context *context = malloc(sizeof(*context));
    if (!context) {
        drop_task_free(task);
        return;
    }
    context->ui = ui;
    context->task = task;
    GtkAlertDialog *dialog = gtk_alert_dialog_new("%s", title);
    const char *buttons[] = {"取消", "确认", NULL};
    gtk_alert_dialog_set_detail(dialog, detail);
    gtk_alert_dialog_set_buttons(dialog, buttons);
    gtk_alert_dialog_set_default_button(dialog, 0);
    gtk_alert_dialog_set_cancel_button(dialog, 0);
    gtk_alert_dialog_choose(dialog, GTK_WINDOW(ui->window), NULL, confirmed, context);
}
static void delete_clicked(GtkButton *button, gpointer context) {
    (void)button;
    gtk_ui *ui = context;
    if (!gtk_widget_get_sensitive(ui->delete_button) || !drop_id_valid(ui->selected)) return;
    drop_task *task = task_new(DROP_TASK_DELETE);
    if (task) strcpy_s(task->message, DROP_ID_CAP, ui->selected);
    confirm_task(ui, "删除所选消息？", "删除将同步到服务器及双方账号的所有设备。用户原文件、已导出文件和旧备份保留。", task);
}
static void review_clicked(GtkButton *button, gpointer context) {
    (void)button;
    gtk_ui *ui = context;
    confirm_task(ui, "重新提交已复核的待发送任务？", "请先检查服务器恢复后的待核对记录，确认需要重新发送。", task_new(DROP_TASK_REVIEW));
}
static gboolean message_key(GtkEventControllerKey *controller, guint key, guint code, GdkModifierType state, gpointer context) {
    (void)controller;
    (void)code;
    (void)state;
    if (key != GDK_KEY_Delete) return FALSE;
    delete_clicked(NULL, context);
    return TRUE;
}
typedef struct export_context {
    gtk_ui *ui;
    drop_task *task;
} export_context;
static void exported(GObject *dialog, GAsyncResult *result, gpointer context) {
    export_context *export = context;
    GFile *file = gtk_file_dialog_save_finish(GTK_FILE_DIALOG(dialog), result, NULL);
    if (file) {
        char *path = g_file_get_path(file);
        export->task->destination = path ? strdup(path) : NULL;
        if (export->task->destination) {
            submit(export->ui, export->task);
            export->task = NULL;
        }
        g_free(path);
        g_object_unref(file);
    }
    drop_task_free(export->task);
    free(export);
    g_object_unref(dialog);
}
static void export_clicked(GtkButton *button, gpointer context) {
    (void)button;
    gtk_ui *ui = context;
    if (!ui->snapshot || !ui->snapshot->history || ui->row < 0 || ui->row >= ui->snapshot->history->count) return;
    drop_history_row *row = &ui->snapshot->history->rows[ui->row];
    if (!row->content || ui->file < 0 || ui->file >= row->content->count || !row->content->files[ui->file].complete) {
        gtk_label_set_text(GTK_LABEL(ui->status), "请先选择已完整保存的附件。");
        return;
    }
    export_context *export = calloc(1, sizeof(*export));
    if (!export || !(export->task = task_new(DROP_TASK_EXPORT))) {
        free(export);
        return;
    }
    export->ui = ui;
    strcpy_s(export->task->message, DROP_ID_CAP, row->id);
    strcpy_s(export->task->object, DROP_ID_CAP, row->content->files[ui->file].id);
    WCHAR safe[260];
    drop_ui_safe_filename(row->content->files[ui->file].name, safe);
    char *name = utf8(safe);
    GtkFileDialog *dialog = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dialog, "导出所选附件");
    gtk_file_dialog_set_initial_name(dialog, name ? name : "attachment");
    free(name);
    gtk_file_dialog_save(dialog, GTK_WINDOW(ui->window), NULL, exported, export);
}
static GtkWidget *button(GtkWidget *box, const char *text, GCallback callback, gtk_ui *ui) {
    GtkWidget *widget = gtk_button_new_with_label(text);
    if (callback) g_signal_connect(widget, "clicked", callback, ui);
    gtk_box_append(GTK_BOX(box), widget);
    return widget;
}
static void login_clicked(GtkButton *button_widget, gpointer context) {
    (void)button_widget;
    gtk_ui *ui = context;
    drop_task *task = task_new(DROP_TASK_LOGIN);
    if (task) {
        task->url = strdup(gtk_editable_get_text(GTK_EDITABLE(ui->url)));
        task->login = strdup(gtk_editable_get_text(GTK_EDITABLE(ui->login)));
        task->password = strdup(gtk_editable_get_text(GTK_EDITABLE(ui->password)));
        task->ca = strdup(gtk_editable_get_text(GTK_EDITABLE(ui->ca)));
    }
    gtk_editable_set_text(GTK_EDITABLE(ui->password), "");
    submit(ui, task);
}
static void logout_clicked(GtkButton *button_widget, gpointer context) {
    (void)button_widget;
    gtk_ui *ui = context;
    confirm_task(ui, "注销当前账号？", "停止此账号同步并清除本机登录凭据，已有正文和附件保留。", task_new(DROP_TASK_LOGOUT));
}
static gboolean settings_close(GtkWindow *window, gpointer context) {
    (void)context;
    gtk_widget_set_visible(GTK_WIDGET(window), FALSE);
    return TRUE;
}
static void preferences_clicked(GtkButton *button_widget, gpointer context) {
    (void)button_widget;
    gtk_ui *ui = context;
    drop_task *task = task_new(DROP_TASK_PREFERENCES);
    if (task) {
        task->notify = gtk_check_button_get_active(GTK_CHECK_BUTTON(ui->notify));
        task->attention = gtk_check_button_get_active(GTK_CHECK_BUTTON(ui->attention));
    }
    submit(ui, task);
}
static void settings_clicked(GtkButton *button_widget, gpointer context) {
    (void)button_widget;
    gtk_ui *ui = context;
    if (!ui->settings) {
        ui->settings = gtk_window_new();
        gtk_window_set_title(GTK_WINDOW(ui->settings), "Drop · 配置与账号");
        gtk_window_set_transient_for(GTK_WINDOW(ui->settings), GTK_WINDOW(ui->window));
        gtk_window_set_default_size(GTK_WINDOW(ui->settings), 520, 270);
        GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
        gtk_widget_set_margin_start(box, 10);
        gtk_widget_set_margin_end(box, 10);
        gtk_widget_set_margin_top(box, 10);
        gtk_widget_set_margin_bottom(box, 10);
        gtk_window_set_child(GTK_WINDOW(ui->settings), box);
        const char *names[] = {"服务器地址（http:// 使用 PAKE；https:// 使用 TLS）", "账号", "密码（仅登录时使用）", "私有 CA 文件（可留空）"};
        GtkWidget **entries[] = {&ui->url, &ui->login, &ui->password, &ui->ca};
        for (int i = 0; i < 4; ++i) {
            gtk_box_append(GTK_BOX(box), label(names[i]));
            *entries[i] = i == 2 ? gtk_password_entry_new() : gtk_entry_new();
            gtk_box_append(GTK_BOX(box), *entries[i]);
        }
        GtkWidget *actions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
        GtkWidget *preferences = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        ui->notify = gtk_check_button_new_with_label("系统通知");
        ui->attention = gtk_check_button_new_with_label("托盘未读提示");
        gtk_box_append(GTK_BOX(preferences), ui->notify);
        gtk_box_append(GTK_BOX(preferences), ui->attention);
        gtk_box_append(GTK_BOX(box), preferences);
        button(actions, "登录并保存", G_CALLBACK(login_clicked), ui);
        button(actions, "保存提醒设置", G_CALLBACK(preferences_clicked), ui);
        button(actions, "注销", G_CALLBACK(logout_clicked), ui);
        gtk_box_append(GTK_BOX(box), actions);
        g_signal_connect(ui->settings, "close-request", G_CALLBACK(settings_close), ui);
    }
    if (ui->snapshot && !gtk_widget_get_visible(ui->settings)) {
        gtk_editable_set_text(GTK_EDITABLE(ui->url), ui->snapshot->url);
        gtk_editable_set_text(GTK_EDITABLE(ui->login), ui->snapshot->login);
        gtk_editable_set_text(GTK_EDITABLE(ui->ca), ui->snapshot->ca);
        gtk_check_button_set_active(GTK_CHECK_BUTTON(ui->notify), ui->snapshot->notify);
        gtk_check_button_set_active(GTK_CHECK_BUTTON(ui->attention), ui->snapshot->attention);
    }
    gtk_window_present(GTK_WINDOW(ui->settings));
}
static void quit_clicked(GtkButton *button_widget, gpointer context) {
    (void)button_widget;
    gtk_ui *ui = context;
    ui->quitting = 1;
    g_application_release(G_APPLICATION(ui->app));
    g_application_quit(G_APPLICATION(ui->app));
}
/* 没有托盘服务时保留可访问窗口；托盘注册成功才允许关闭窗口后后台运行。 */
static gboolean window_close(GtkWindow *window, gpointer context) {
    gtk_ui *ui = context;
    if (ui->tray_available) {
        gtk_widget_set_visible(GTK_WIDGET(window), FALSE);
        return TRUE;
    }
    quit_clicked(NULL, ui);
    return FALSE;
}
static const char tray_xml[] =
    "<node><interface name='org.kde.StatusNotifierItem'>"
    "<method name='Activate'><arg type='i' direction='in'/><arg type='i' direction='in'/></method>"
    "<method name='SecondaryActivate'><arg type='i' direction='in'/><arg type='i' direction='in'/></method>"
    "<method name='ContextMenu'><arg type='i' direction='in'/><arg type='i' direction='in'/></method>"
    "<method name='Scroll'><arg type='i' direction='in'/><arg type='s' direction='in'/></method>"
    "<property name='Category' type='s' access='read'/><property name='Id' type='s' access='read'/><property name='Title' type='s' access='read'/>"
    "<property name='Status' type='s' access='read'/><property name='WindowId' type='u' access='read'/><property name='IconName' type='s' access='read'/>"
    "<property name='IconPixmap' type='a(iiay)' access='read'/><property name='AttentionIconName' type='s' access='read'/>"
    "<property name='AttentionIconPixmap' type='a(iiay)' access='read'/><property name='OverlayIconName' type='s' access='read'/>"
    "<property name='OverlayIconPixmap' type='a(iiay)' access='read'/><property name='ToolTip' type='(sa(iiay)ss)' access='read'/>"
    "<property name='ItemIsMenu' type='b' access='read'/><property name='Menu' type='o' access='read'/>"
    "<signal name='NewIcon'/><signal name='NewStatus'><arg type='s'/></signal></interface></node>";
static void tray_method(GDBusConnection *connection, const char *sender, const char *path, const char *interface, const char *method, GVariant *parameters,
    GDBusMethodInvocation *invocation, gpointer context) {
    (void)connection;
    (void)sender;
    (void)path;
    (void)interface;
    (void)parameters;
    gtk_ui *ui = context;
    if (!strcmp(method, "Activate") || !strcmp(method, "SecondaryActivate") || !strcmp(method, "ContextMenu")) gtk_window_present(GTK_WINDOW(ui->window));
    g_dbus_method_invocation_return_value(invocation, NULL);
}
static GVariant *tray_property(GDBusConnection *connection, const char *sender, const char *path, const char *interface, const char *property, GError **error, gpointer context) {
    (void)connection;
    (void)sender;
    (void)path;
    (void)interface;
    (void)error;
    gtk_ui *ui = context;
    if (!strcmp(property, "Category")) return g_variant_new_string("Communications");
    if (!strcmp(property, "Id")) return g_variant_new_string("Drop");
    if (!strcmp(property, "Title")) return g_variant_new_string("Drop " DROP_VERSION);
    if (!strcmp(property, "Status"))
        return g_variant_new_string(ui->snapshot && ui->snapshot->attention && ui->snapshot->history && ui->snapshot->history->unread ? "NeedsAttention" : "Active");
    if (!strcmp(property, "WindowId")) return g_variant_new_uint32(0);
    if (!strcmp(property, "ItemIsMenu")) return g_variant_new_boolean(FALSE);
    if (!strcmp(property, "Menu")) return g_variant_new_object_path("/NO_DBUSMENU");
    if (!strcmp(property, "IconName") || !strcmp(property, "AttentionIconName")) return g_variant_new_string("");
    if (!strcmp(property, "OverlayIconName")) return g_variant_new_string("");
    if ((!strcmp(property, "IconPixmap") || !strcmp(property, "AttentionIconPixmap")) && ui->tray_pixmap) return g_variant_ref(ui->tray_pixmap);
    if (strstr(property, "Pixmap")) return g_variant_new_array(G_VARIANT_TYPE("(iiay)"), NULL, 0);
    if (!strcmp(property, "ToolTip")) return g_variant_new("(s@a(iiay)ss)", "mail-message-new", g_variant_new_array(G_VARIANT_TYPE("(iiay)"), NULL, 0), "Drop", "点击打开消息窗口");
    return NULL;
}
static void tray_registered(GObject *connection, GAsyncResult *result, gpointer context) {
    gtk_ui *ui = context;
    GVariant *reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(connection), result, NULL);
    if (reply) {
        g_variant_unref(reply);
        ui->tray_available = 1;
        if (ui->hidden) gtk_widget_set_visible(ui->window, FALSE);
    } else if (ui->hidden)
        gtk_window_present(GTK_WINDOW(ui->window));
}
static void tray_start(gtk_ui *ui) {
    static const GDBusInterfaceVTable table = {tray_method, tray_property, NULL, {0}};
    ui->bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, NULL);
    ui->tray_info = g_dbus_node_info_new_for_xml(tray_xml, NULL);
    if (!ui->bus || !ui->tray_info) return;
    /* 托盘像素使用协议要求的 ARGB 字节顺序，图标直接来自产品资源，不依赖系统图标主题。 */
    GdkPixbuf *icon = gdk_pixbuf_new_from_resource_at_scale("/win/jsnowly/Drop/icons/drop.png", 32, 32, TRUE, NULL);
    if (icon) {
        int width = gdk_pixbuf_get_width(icon), height = gdk_pixbuf_get_height(icon), stride = gdk_pixbuf_get_rowstride(icon), channels = gdk_pixbuf_get_n_channels(icon);
        guchar *pixels = gdk_pixbuf_get_pixels(icon), *argb = g_malloc((gsize)width * height * 4);
        for (int y = 0; y < height; ++y)
            for (int x = 0; x < width; ++x) {
                guchar *p = pixels + y * stride + x * channels, *out = argb + ((gsize)y * width + x) * 4;
                out[0] = channels == 4 ? p[3] : 255;
                out[1] = p[0];
                out[2] = p[1];
                out[3] = p[2];
            }
        GVariant *item = g_variant_new("(ii@ay)", width, height, g_variant_new_fixed_array(G_VARIANT_TYPE_BYTE, argb, (gsize)width * height * 4, 1));
        ui->tray_pixmap = g_variant_ref_sink(g_variant_new_array(G_VARIANT_TYPE("(iiay)"), &item, 1));
        g_free(argb);
        g_object_unref(icon);
    }
    ui->tray_registration = g_dbus_connection_register_object(ui->bus, "/StatusNotifierItem", ui->tray_info->interfaces[0], &table, ui, NULL, NULL);
    if (ui->tray_registration)
        g_dbus_connection_call(ui->bus, "org.kde.StatusNotifierWatcher", "/StatusNotifierWatcher", "org.kde.StatusNotifierWatcher", "RegisterStatusNotifierItem",
            g_variant_new("(s)", "/StatusNotifierItem"), NULL, G_DBUS_CALL_FLAGS_NONE, 5000, NULL, tray_registered, ui);
}
/* 连续文本流与 Windows 采用相同行结构；重绘保留所选消息 ID，并从新快照重建安全的附件范围。 */
static void render(gtk_ui *ui) {
    drop_history *history = ui->snapshot->history;
    GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(ui->messages));
    GtkTextIter end;
    ui->rebuilding = 1;
    GtkWidget *child;
    while ((child = gtk_widget_get_first_child(ui->users)))
        gtk_list_box_remove(GTK_LIST_BOX(ui->users), child);
    GtkTextIter sel_start, sel_end;
    gboolean selection = gtk_text_buffer_get_selection_bounds(buffer, &sel_start, &sel_end);
    int selection_start = gtk_text_iter_get_offset(&sel_start), selection_end = gtk_text_iter_get_offset(&sel_end);
    GtkWidget *scroll = gtk_widget_get_parent(ui->messages);
    GtkAdjustment *adjustment = GTK_IS_SCROLLED_WINDOW(scroll) ? gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(scroll)) : NULL;
    double position = adjustment ? gtk_adjustment_get_value(adjustment) : 0;
    gtk_text_buffer_set_text(buffer, "", -1);
    memset(ui->file_ends, 0, sizeof(ui->file_ends));
    ui->row = -1;
    gtk_widget_set_sensitive(ui->delete_button, FALSE);
    gtk_widget_set_sensitive(ui->export_button, FALSE);
    if (history) {
        strcpy_s(ui->peer, DROP_ID_CAP, history->peer);
        for (int i = 0; i < history->peer_count; ++i) {
            drop_peer *peer = &history->peers[i];
            char *name = utf8(peer->name), *title = g_strdup_printf("%s%s   %d", name ? name : peer->id, i ? "" : "（自己）", peer->unread);
            GtkWidget *widget = label(title), *row = gtk_list_box_row_new();
            gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), widget);
            g_object_set_data_full(G_OBJECT(row), "drop-peer", g_strdup(peer->id), g_free);
            gtk_list_box_append(GTK_LIST_BOX(ui->users), row);
            if (!strcmp(peer->id, ui->peer)) {
                gtk_list_box_select_row(GTK_LIST_BOX(ui->users), GTK_LIST_BOX_ROW(row));
                char *recipient = g_strdup_printf("发送给：%s%s", name ? name : peer->id, !strcmp(peer->id, history->self) ? " · 自己的设备" : "");
                gtk_label_set_text(GTK_LABEL(ui->recipient), recipient);
                char *overview = g_strdup_printf("%s · 第 %d 页", name ? name : peer->id, ui->page + 1);
                gtk_label_set_text(GTK_LABEL(ui->overview), overview);
                g_free(overview);
                g_free(recipient);
            }
            free(name);
            g_free(title);
        }
        for (int i = 0; i < history->count; ++i) {
            drop_history_row *row = &history->rows[i];
            char *sender = utf8(row->sender), *time = utf8(row->time), *body = utf8(row->preview);
            char *head = g_strdup_printf("%s  %s · %s\n", sender ? sender : "", time ? time : "", row->unread ? "未读" : "本机已保存");
            gtk_text_buffer_get_end_iter(buffer, &end);
            ui->starts[i] = gtk_text_iter_get_offset(&end);
            gtk_text_buffer_insert_with_tags_by_name(buffer, &end, head, -1, "header", NULL);
            if (body && *body) {
                gtk_text_buffer_insert(buffer, &end, body, -1);
                gtk_text_buffer_insert(buffer, &end, "\n", 1);
            }
            if (row->content)
                for (int j = 0; j < row->content->count; ++j) {
                    drop_file_view *file = &row->content->files[j];
                    char *name = utf8(file->name),
                         *line = g_strdup_printf("    附件：%s · %llu 字节 · %s\n", name ? name : "", (unsigned long long)file->total, file->complete ? "已缓存到本机" : "待下载");
                    ui->file_starts[i][j] = gtk_text_iter_get_offset(&end);
                    gtk_text_buffer_insert_with_tags_by_name(buffer, &end, line, -1, "attachment", NULL);
                    ui->file_ends[i][j] = gtk_text_iter_get_offset(&end);
                    free(name);
                    g_free(line);
                }
            ui->ends[i] = gtk_text_iter_get_offset(&end);
            if (!strcmp(row->id, ui->selected)) ui->row = i;
            free(sender);
            free(time);
            free(body);
            g_free(head);
        }
    }
    if (selection) {
        int total = gtk_text_buffer_get_char_count(buffer);
        gtk_text_buffer_get_iter_at_offset(buffer, &sel_start, MIN(selection_start, total));
        gtk_text_buffer_get_iter_at_offset(buffer, &sel_end, MIN(selection_end, total));
        gtk_text_buffer_select_range(buffer, &sel_start, &sel_end);
    }
    if (adjustment) gtk_adjustment_set_value(adjustment, position);
    if (ui->row >= 0)
        detail_show(ui, ui->row, ui->file);
    else {
        gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(ui->detail)), "", -1);
        while ((child = gtk_widget_get_first_child(ui->files)))
            gtk_list_box_remove(GTK_LIST_BOX(ui->files), child);
    }
    gtk_widget_set_sensitive(ui->send, ui->snapshot->ready && drop_id_valid(ui->peer) && !ui->send_pending);
    ui->rebuilding = 0;
}
static gboolean poll_snapshot(gpointer context) {
    gtk_ui *ui = context;
    if (drop_process_stop_requested()) {
        quit_clicked(NULL, ui);
        return G_SOURCE_REMOVE;
    }
    drop_snapshot *snapshot = drop_runtime_take(ui->runtime);
    if (!snapshot) return G_SOURCE_CONTINUE;
    if (ui->send_pending && snapshot->send_finished > ui->pending_finished) {
        if (snapshot->send_serial > ui->pending_serial) {
            gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(ui->compose)), "", -1);
            for (int i = 0; i < ui->count; ++i)
                free(ui->paths[i]);
            ui->count = 0;
            preview_update(ui);
        }
        ui->send_pending = 0;
        gtk_text_view_set_editable(GTK_TEXT_VIEW(ui->compose), TRUE);
    }
    drop_snapshot_free(ui->snapshot);
    ui->snapshot = snapshot;
    render(ui);
    char status[512];
    drop_history *h = snapshot->history;
    sprintf_s(status, sizeof(status), "%s · 待发送 %d · 下载 %d · 待删除 %d", snapshot->status, h ? h->pending_count : 0, h ? h->downloads : 0, h ? h->pending_deletes : 0);
    gtk_label_set_text(GTK_LABEL(ui->status), status);
    if (h && h->new_notifications && snapshot->notify && !gtk_widget_get_visible(ui->window)) {
        GNotification *notification = g_notification_new("Drop 收到新消息");
        g_notification_set_body(notification, "消息和附件正在同步到本机。");
        g_application_send_notification(G_APPLICATION(ui->app), "drop-new-message", notification);
        g_object_unref(notification);
    }
    if (ui->bus && ui->tray_registration)
        g_dbus_connection_emit_signal(ui->bus, NULL, "/StatusNotifierItem", "org.kde.StatusNotifierItem", "NewStatus",
            g_variant_new("(s)", snapshot->attention && h && h->unread ? "NeedsAttention" : "Active"), NULL);
    return G_SOURCE_CONTINUE;
}
#ifdef DROP_GUI_CHECKS
#include "../checks/gtk_probe.inc"
#endif
/* 菜单与工具栏复用同一组任务和确认流程，不能另建未校验的文件或删除入口。 */
static void menu_action(GSimpleAction *action, GVariant *parameter, gpointer context) {
    (void)parameter;
    const char *name = g_action_get_name(G_ACTION(action));
    if (!strcmp(name, "sync"))
        sync_clicked(NULL, context);
    else if (!strcmp(name, "settings"))
        settings_clicked(NULL, context);
    else if (!strcmp(name, "export"))
        export_clicked(NULL, context);
    else if (!strcmp(name, "delete"))
        delete_clicked(NULL, context);
    else if (!strcmp(name, "review"))
        review_clicked(NULL, context);
    else if (!strcmp(name, "full-text"))
        full_text(context);
    else if (!strcmp(name, "quit"))
        quit_clicked(NULL, context);
}
static GtkWidget *menu_create(gtk_ui *ui) {
    GMenu *menu = g_menu_new(), *file = g_menu_new(), *conversation = g_menu_new();
    const char *names[] = {"sync", "settings", "export", "delete", "review", "quit", "full-text"};
    for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
        GSimpleAction *action = g_simple_action_new(names[i], NULL);
        g_signal_connect(action, "activate", G_CALLBACK(menu_action), ui);
        g_action_map_add_action(G_ACTION_MAP(ui->app), G_ACTION(action));
        g_object_unref(action);
    }
    g_menu_append(file, "配置与账号", "app.settings");
    g_menu_append(file, "导出所选附件", "app.export");
    g_menu_append(file, "真正退出", "app.quit");
    g_menu_append(conversation, "同步", "app.sync");
    g_menu_append(conversation, "查看所选消息完整正文", "app.full-text");
    g_menu_append(conversation, "删除所选消息", "app.delete");
    g_menu_append(conversation, "恢复后重新提交已复核任务", "app.review");
    g_menu_append_submenu(menu, "文件", G_MENU_MODEL(file));
    g_menu_append_submenu(menu, "会话", G_MENU_MODEL(conversation));
    GtkWidget *bar = gtk_popover_menu_bar_new_from_model(G_MENU_MODEL(menu));
    g_object_unref(file);
    g_object_unref(conversation);
    g_object_unref(menu);
    return bar;
}
static void activate(GtkApplication *app, gpointer context) {
    gtk_ui *ui = context;
    if (ui->window) {
        gtk_window_present(GTK_WINDOW(ui->window));
        return;
    }
    ui->window = gtk_application_window_new(app);
    GtkCssProvider *style = gtk_css_provider_new();
    gtk_css_provider_load_from_resource(style, "/win/jsnowly/Drop/drop-style.css");
    gtk_style_context_add_provider_for_display(gtk_widget_get_display(ui->window), GTK_STYLE_PROVIDER(style), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(style);
    gtk_icon_theme_add_resource_path(gtk_icon_theme_get_for_display(gtk_widget_get_display(ui->window)), "/win/jsnowly/Drop/icons");
    gtk_window_set_icon_name(GTK_WINDOW(ui->window), "drop");
    gtk_window_set_title(GTK_WINDOW(ui->window), "Drop " DROP_VERSION);
    gtk_window_set_default_size(GTK_WINDOW(ui->window), 1080, 760);
    GtkWidget *outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 3), *toolbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_widget_set_margin_start(outer, 4);
    gtk_widget_set_margin_end(outer, 4);
    gtk_widget_set_margin_top(outer, 3);
    gtk_widget_set_margin_bottom(outer, 3);
    gtk_window_set_child(GTK_WINDOW(ui->window), outer);
    gtk_box_append(GTK_BOX(outer), menu_create(ui));
    button(toolbar, "同步", G_CALLBACK(sync_clicked), ui);
    button(toolbar, "配置与账号", G_CALLBACK(settings_clicked), ui);
    ui->export_button = button(toolbar, "导出附件", G_CALLBACK(export_clicked), ui);
    ui->delete_button = button(toolbar, "删除消息", G_CALLBACK(delete_clicked), ui);
    GtkWidget *spacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_hexpand(spacer, TRUE);
    gtk_box_append(GTK_BOX(toolbar), spacer);
    button(toolbar, "真正退出", G_CALLBACK(quit_clicked), ui);
    gtk_box_append(GTK_BOX(outer), toolbar);
    GtkWidget *paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL), *left = gtk_box_new(GTK_ORIENTATION_VERTICAL, 3), *right = gtk_box_new(GTK_ORIENTATION_VERTICAL, 3);
    gtk_paned_set_position(GTK_PANED(paned), 220);
    gtk_paned_set_resize_start_child(GTK_PANED(paned), FALSE);
    gtk_paned_set_start_child(GTK_PANED(paned), left);
    gtk_paned_set_end_child(GTK_PANED(paned), right);
    gtk_widget_set_vexpand(paned, TRUE);
    gtk_box_append(GTK_BOX(outer), paned);
    GtkWidget *lookup = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 3);
    ui->find = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(ui->find), "账号 / 查找用户");
    gtk_widget_set_hexpand(ui->find, TRUE);
    gtk_box_append(GTK_BOX(lookup), ui->find);
    button(lookup, "查找", G_CALLBACK(find_clicked), ui);
    gtk_box_append(GTK_BOX(left), lookup);
    ui->users = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(ui->users), GTK_SELECTION_SINGLE);
    GtkWidget *user_scroll = scrolled(ui->users, 0);
    gtk_widget_set_vexpand(user_scroll, TRUE);
    gtk_box_append(GTK_BOX(left), user_scroll);
    g_signal_connect(ui->users, "row-selected", G_CALLBACK(peer_selected), ui);
    gtk_box_append(GTK_BOX(left), label("自己的账号置顶 · 本机 Linux"));
    ui->overview = label("当前会话 · 请先登录");
    gtk_widget_add_css_class(ui->overview, "drop-band");
    gtk_widget_set_size_request(ui->overview, -1, 26);
    gtk_box_append(GTK_BOX(right), ui->overview);
    GtkWidget *filters = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 3);
    ui->search = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(ui->search), "当前会话正文");
    gtk_widget_set_hexpand(ui->search, TRUE);
    gtk_box_append(GTK_BOX(filters), ui->search);
    ui->from = gtk_entry_new();
    ui->to = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(ui->from), "起日 YYYY-MM-DD");
    gtk_entry_set_placeholder_text(GTK_ENTRY(ui->to), "止日 YYYY-MM-DD");
    gtk_editable_set_width_chars(GTK_EDITABLE(ui->from), 12);
    gtk_editable_set_width_chars(GTK_EDITABLE(ui->to), 12);
    gtk_box_append(GTK_BOX(filters), ui->from);
    gtk_box_append(GTK_BOX(filters), ui->to);
    button(filters, "查询", G_CALLBACK(filter_clicked), ui);
    button(filters, "‹", G_CALLBACK(previous_clicked), ui);
    button(filters, "›", G_CALLBACK(next_clicked), ui);
    gtk_box_append(GTK_BOX(right), filters);
    ui->messages = text_view(0);
    GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(ui->messages));
    gtk_text_buffer_create_tag(buffer, "header", "foreground", "#315a88", "weight", PANGO_WEIGHT_BOLD, NULL);
    gtk_text_buffer_create_tag(buffer, "attachment", "foreground", "#315a88", "underline", PANGO_UNDERLINE_SINGLE, NULL);
    GtkWidget *message_scroll = scrolled(ui->messages, 240);
    gtk_widget_set_vexpand(message_scroll, TRUE);
    gtk_box_append(GTK_BOX(right), message_scroll);
    GtkGesture *click = gtk_gesture_click_new();
    g_signal_connect(click, "released", G_CALLBACK(message_clicked), ui);
    gtk_widget_add_controller(ui->messages, GTK_EVENT_CONTROLLER(click));
    GtkGesture *context_click = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(context_click), 3);
    g_signal_connect(context_click, "pressed", G_CALLBACK(message_context), ui);
    gtk_widget_add_controller(ui->messages, GTK_EVENT_CONTROLLER(context_click));
    GtkEventController *message_controller = gtk_event_controller_key_new();
    g_signal_connect(message_controller, "key-pressed", G_CALLBACK(message_key), ui);
    gtk_widget_add_controller(ui->messages, message_controller);
    GtkWidget *details = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_widget_set_vexpand(details, FALSE);
    ui->detail = text_view(0);
    ui->document = gtk_window_new();
    gtk_window_set_application(GTK_WINDOW(ui->document), ui->app);
    gtk_window_set_title(GTK_WINDOW(ui->document), "Drop " DROP_VERSION " · 完整正文（可选择复制）");
    gtk_window_set_transient_for(GTK_WINDOW(ui->document), GTK_WINDOW(ui->window));
    gtk_window_set_destroy_with_parent(GTK_WINDOW(ui->document), TRUE);
    gtk_window_set_hide_on_close(GTK_WINDOW(ui->document), TRUE);
    gtk_window_set_default_size(GTK_WINDOW(ui->document), 760, 500);
    gtk_window_set_child(GTK_WINDOW(ui->document), scrolled(ui->detail, 0));
    ui->files = gtk_list_box_new();
    GtkWidget *draft_panel = gtk_box_new(GTK_ORIENTATION_VERTICAL, 3), *file_panel = gtk_box_new(GTK_ORIENTATION_VERTICAL, 3);
    ui->preview_label = label("待发送附件（0）");
    gtk_box_append(GTK_BOX(draft_panel), ui->preview_label);
    ui->preview = gtk_box_new(GTK_ORIENTATION_VERTICAL, 3);
    ui->preview_scroll = scrolled(ui->preview, 84);
    gtk_widget_set_vexpand(ui->preview_scroll, FALSE);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(ui->preview_scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(ui->preview_scroll), 84);
    gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(ui->preview_scroll), 84);
    gtk_box_append(GTK_BOX(draft_panel), ui->preview_scroll);
    gtk_box_append(GTK_BOX(file_panel), label("所选消息的附件"));
    GtkWidget *file_scroll = scrolled(ui->files, 84);
    gtk_widget_set_vexpand(file_scroll, FALSE);
    gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(file_scroll), 84);
    gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(file_scroll), 84);
    gtk_box_append(GTK_BOX(file_panel), file_scroll);
    gtk_paned_set_start_child(GTK_PANED(details), draft_panel);
    gtk_paned_set_end_child(GTK_PANED(details), file_panel);
    gtk_paned_set_position(GTK_PANED(details), 380);
    gtk_box_append(GTK_BOX(right), details);
    g_signal_connect(ui->files, "row-selected", G_CALLBACK(file_selected), ui);
    ui->recipient = label("发送给：请先登录");
    gtk_widget_add_css_class(ui->recipient, "drop-band");
    gtk_widget_set_size_request(ui->recipient, -1, 26);
    gtk_box_append(GTK_BOX(right), ui->recipient);
    GtkWidget *composer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    ui->compose = text_view(1);
    GtkWidget *compose_scroll = scrolled(ui->compose, 56);
    ui->compose_scroll = compose_scroll;
    gtk_widget_set_hexpand(compose_scroll, TRUE);
    gtk_box_append(GTK_BOX(composer), compose_scroll);
    GtkWidget *send_buttons = gtk_box_new(GTK_ORIENTATION_VERTICAL, 3);
    button(send_buttons, "添加附件", G_CALLBACK(add_clicked), ui);
    ui->send = button(send_buttons, "发送", G_CALLBACK(send_clicked), ui);
    gtk_widget_add_css_class(ui->send, "suggested-action");
    gtk_box_append(GTK_BOX(composer), send_buttons);
    gtk_box_append(GTK_BOX(right), composer);
    GtkDropTarget *drop = gtk_drop_target_new(GDK_TYPE_FILE_LIST, GDK_ACTION_COPY);
    g_signal_connect(drop, "drop", G_CALLBACK(files_dropped), ui);
    gtk_widget_add_controller(ui->window, GTK_EVENT_CONTROLLER(drop));
    GtkEventController *key = gtk_event_controller_key_new();
    g_signal_connect(key, "key-pressed", G_CALLBACK(compose_key), ui);
    gtk_event_controller_set_propagation_phase(key, GTK_PHASE_CAPTURE);
    gtk_widget_add_controller(ui->compose, key);
    ui->status = label("正在打开本机数据…");
    gtk_box_append(GTK_BOX(outer), ui->status);
    gtk_widget_set_sensitive(ui->send, FALSE);
    gtk_widget_set_sensitive(ui->delete_button, FALSE);
    gtk_widget_set_sensitive(ui->export_button, FALSE);
    GtkCssProvider *css = gtk_css_provider_new();
    gtk_css_provider_load_from_string(css,
        "window { background: #e9eef4; } textview { font-size: 13px; } button { min-height: 22px; padding: 2px 7px; } entry { min-height: 24px; padding: 1px 5px; } list row { "
        "padding: 2px 4px; } textview text { background: #fff; color: #243449; } ");
    gtk_style_context_add_provider_for_display(gtk_widget_get_display(ui->window), GTK_STYLE_PROVIDER(css), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(css);
    g_signal_connect(ui->window, "close-request", G_CALLBACK(window_close), ui);
    g_application_hold(G_APPLICATION(app));
    ui->poll = g_timeout_add(200, poll_snapshot, ui);
    tray_start(ui);
    gtk_window_present(GTK_WINDOW(ui->window));
#ifdef DROP_GUI_CHECKS
    g_timeout_add(200, gui_probe, ui);
#endif
}
int drop_native_ui(const WCHAR *root, int hidden) {
    gtk_ui ui = {0};
    int result;
    ui.row = -1;
    ui.file = -1;
    ui.hidden = hidden;
    g_setenv("GDK_BACKEND", "wayland", TRUE);
    if (!(ui.runtime = drop_runtime_start(root))) {
        fprintf(stderr, "Drop data directory unavailable or already running.\n");
        return 1;
    }
    ui.app = gtk_application_new("win.jsnowly.Drop", G_APPLICATION_NON_UNIQUE);
    g_signal_connect(ui.app, "activate", G_CALLBACK(activate), &ui);
    result = g_application_run(G_APPLICATION(ui.app), 0, NULL);
    if (ui.poll) g_source_remove(ui.poll);
    drop_runtime_stop(ui.runtime);
    drop_snapshot_free(ui.snapshot);
    for (int i = 0; i < ui.count; ++i)
        free(ui.paths[i]);
    if (ui.tray_registration) g_dbus_connection_unregister_object(ui.bus, ui.tray_registration);
    if (ui.tray_info) g_dbus_node_info_unref(ui.tray_info);
    if (ui.bus) g_object_unref(ui.bus);
    if (ui.tray_pixmap) g_variant_unref(ui.tray_pixmap);
    g_object_unref(ui.app);
    return result;
}
