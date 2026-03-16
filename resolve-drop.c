#include <gtk/gtk.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>

static void append_text(GtkTextBuffer *buffer, const char *text) {
  GtkTextIter iter;
  gtk_text_buffer_get_end_iter(buffer, &iter);
  gtk_text_buffer_insert(buffer, &iter, text, -1);
  gtk_text_buffer_insert(buffer, &iter, "\n", -1);
}

static void scroll_to_bottom(GtkTextView *view) {
  GtkTextBuffer *buf = gtk_text_view_get_buffer(view);
  GtkTextIter end;
  gtk_text_buffer_get_end_iter(buf, &end);
  GtkWidget *scrolled = gtk_widget_get_parent(GTK_WIDGET(view));
  GtkAdjustment *adj =
      gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(scrolled));
  gtk_adjustment_set_value(adj, gtk_adjustment_get_upper(adj) -
                                    gtk_adjustment_get_page_size(adj));
}

static void process_file(const char *path, GtkTextBuffer *buffer,
                         GtkTextView *view) {
  if (!g_str_has_suffix(path, ".mp4") && !g_str_has_suffix(path, ".MP4")) {
    gchar *msg =
        g_strdup_printf("Skipping (not .mp4): %s", g_path_get_basename(path));
    append_text(buffer, msg);
    g_free(msg);
    return;
  }

  gchar *out_path = g_strdup_printf("%.*s.mov", (int)(strlen(path) - 4), path);

  if (g_file_test(out_path, G_FILE_TEST_EXISTS)) {
    gchar *msg = g_strdup_printf("Already exists – skipping: %s",
                                 g_path_get_basename(out_path));
    append_text(buffer, msg);
    g_free(msg);
    g_free(out_path);
    return;
  }

  append_text(buffer, "Processing:");
  gchar *line = g_strdup_printf("  %s → %s", g_path_get_basename(path),
                                g_path_get_basename(out_path));
  append_text(buffer, line);
  g_free(line);

  gchar *cmd = g_strdup_printf(
      "ffmpeg -i \"%s\" -c:v copy -c:a pcm_s16le \"%s\" 2>&1", path, out_path);
  FILE *fp = popen(cmd, "r");

  if (fp) {
    char buf[512];
    while (fgets(buf, sizeof(buf), fp)) {
      buf[strcspn(buf, "\n")] = '\0';
      if (strlen(buf) > 0) {
        gchar *log = g_strdup_printf("    %s", buf);
        append_text(buffer, log);
        g_free(log);
      }
    }
    int status = pclose(fp);
    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
      append_text(buffer, "  ✓ Success");
    } else {
      append_text(buffer, "  ✗ ffmpeg failed");
    }
  } else {
    append_text(buffer, "  Failed to run ffmpeg");
  }
  g_free(cmd);
  g_free(out_path);
  scroll_to_bottom(view);
}

static gboolean on_accept(GtkDropTarget *target, GdkDrop *drop,
                          gpointer user_data) {
  return TRUE; // Accept MOVE action from Hyprland file managers
}

static void on_drop(GtkDropTarget *target, const GValue *value, double x,
                    double y, gpointer user_data) {
  GtkTextView *view = GTK_TEXT_VIEW(user_data);
  GtkTextBuffer *buffer = gtk_text_view_get_buffer(view);

  append_text(buffer, "=== DROP RECEIVED (Hyprland fixed) ===");

  if (!G_VALUE_HOLDS(value, GDK_TYPE_FILE_LIST)) {
    append_text(buffer, "Error: Unsupported drop format.");
    scroll_to_bottom(view);
    return;
  }

  GList *files = g_value_get_boxed(value);
  guint count = 0;

  for (GList *l = files; l != NULL; l = l->next) {
    GFile *file = l->data;
    gchar *path = g_file_get_path(file);
    if (path) {
      process_file(path, buffer, view);
      count++;
      g_free(path);
    }
  }

  gchar *summary = g_strdup_printf("Processed %u file(s).", count);
  append_text(buffer, summary);
  g_free(summary);
  append_text(buffer, "────────────────────");
  scroll_to_bottom(view);
}

static void on_open_response(GObject *source_object, GAsyncResult *result,
                             gpointer user_data) {
  GtkFileDialog *dialog = GTK_FILE_DIALOG(source_object);
  GtkTextView *view = GTK_TEXT_VIEW(user_data);
  GtkTextBuffer *buffer = gtk_text_view_get_buffer(view);

  GError *error = NULL;
  GListModel *model =
      gtk_file_dialog_open_multiple_finish(dialog, result, &error);

  if (error) {
    gchar *msg = g_strdup_printf("File dialog error: %s", error->message);
    append_text(buffer, msg);
    g_free(msg);
    g_error_free(error);
  } else if (model) {
    guint n_items = g_list_model_get_n_items(model);
    for (guint i = 0; i < n_items; i++) {
      GFile *file = G_FILE(g_list_model_get_item(model, i));
      gchar *path = g_file_get_path(file);
      if (path) {
        process_file(path, buffer, view);
        g_free(path);
      }
      g_object_unref(file);
    }
    g_object_unref(model);
  }

  scroll_to_bottom(view);
  g_object_unref(dialog);
}

static void on_select_clicked(GtkButton *button, gpointer user_data) {
  GtkTextView *view = GTK_TEXT_VIEW(user_data);

  GtkFileDialog *dialog = gtk_file_dialog_new();
  gtk_file_dialog_set_title(dialog, "Select MP4 files");
  gtk_file_dialog_set_modal(dialog, TRUE);

  // MP4 filter
  GtkFileFilter *filter = gtk_file_filter_new();
  gtk_file_filter_set_name(filter, "MP4 files");
  gtk_file_filter_add_pattern(filter, "*.mp4");
  gtk_file_filter_add_pattern(filter, "*.MP4");

  GListStore *store = g_list_store_new(GTK_TYPE_FILE_FILTER);
  g_list_store_append(store, filter);
  gtk_file_dialog_set_filters(dialog, G_LIST_MODEL(store));

  // Open async (modern GTK4 way)
  gtk_file_dialog_open_multiple(dialog, NULL, NULL, on_open_response, view);

  // Ownership transferred – no need to unref here
}

static void activate(GtkApplication *app, gpointer user_data) {
  GtkWidget *window = gtk_application_window_new(app);
  gtk_window_set_title(GTK_WINDOW(window),
                       "Resolve MP4 → MOV Dropper (Hyprland Fixed)");
  gtk_window_set_default_size(GTK_WINDOW(window), 760, 560);

  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
  gtk_widget_set_margin_start(box, 24);
  gtk_widget_set_margin_end(box, 24);
  gtk_widget_set_margin_top(box, 24);
  gtk_widget_set_margin_bottom(box, 24);
  gtk_window_set_child(GTK_WINDOW(window), box);

  GtkWidget *header = gtk_label_new(NULL);
  gtk_label_set_markup(GTK_LABEL(header),
                       "<big><b>Drag MP4 files here</b></big>\n"
                       "<small>or click the button below</small>");
  gtk_box_append(GTK_BOX(box), header);

  GtkWidget *scrolled = gtk_scrolled_window_new();
  gtk_widget_set_vexpand(scrolled, TRUE);
  gtk_box_append(GTK_BOX(box), scrolled);

  GtkWidget *textview = gtk_text_view_new();
  gtk_text_view_set_editable(GTK_TEXT_VIEW(textview), FALSE);
  gtk_text_view_set_monospace(GTK_TEXT_VIEW(textview), TRUE);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled), textview);

  GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(textview));
  append_text(buffer, "✅ Ready on Hyprland – drag files or click the button!");

  // === Drag & Drop (Hyprland fixed – accepts MOVE action) ===
  GtkDropTarget *drop = gtk_drop_target_new(GDK_TYPE_FILE_LIST,
                                            GDK_ACTION_COPY | GDK_ACTION_MOVE);
  g_signal_connect(drop, "accept", G_CALLBACK(on_accept), NULL);
  g_signal_connect(drop, "drop", G_CALLBACK(on_drop), textview);
  gtk_widget_add_controller(box, GTK_EVENT_CONTROLLER(drop));

  // === Modern file selector button (no deprecation) ===
  GtkWidget *select_btn = gtk_button_new_with_label("📂 Select MP4 Files...");
  gtk_widget_set_size_request(select_btn, -1, 48);
  g_signal_connect(select_btn, "clicked", G_CALLBACK(on_select_clicked),
                   textview);
  gtk_box_append(GTK_BOX(box), select_btn);

  gtk_window_present(GTK_WINDOW(window));
}

int main(int argc, char **argv) {
  GtkApplication *app = gtk_application_new(
      "com.resolve.dropper.hyprland.fixed", G_APPLICATION_DEFAULT_FLAGS);
  g_signal_connect(app, "activate", G_CALLBACK(activate), NULL);

  int status = g_application_run(G_APPLICATION(app), argc, argv);
  g_object_unref(app);
  return status;
}
