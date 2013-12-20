#include <wchar.h>
#include <jsapi.h>
#include <jsprvtd.h>
#include <string.h>
#include <gio/gio.h>
#include <gjs/gjs.h>

#include "gjs-debug-interrupt-register.h"
#include "gjs-interrupt-register.h"

typedef void (*LineForeachFunc) (const gchar *str,
                                 gpointer    user_data);

static void
for_each_line_in_string (const gchar *data,
                         gpointer    user_data,
                         LineForeachFunc func)
{
  const gchar *str = data;

  while (str)
    {
      (*func) (str + 1, user_data);
      str = (gchar *) (strstr (str + 1, "\n"));
    }
}

static void
increment_line_counter (const gchar *str,
                        gpointer    user_data)
{
  guint *lineCount = (guint *) user_data;
  ++(*lineCount);
}

static guint
count_lines_in_string (const gchar *data)
{
  guint lineCount = 0;

  for_each_line_in_string (data, &lineCount, increment_line_counter);

  return lineCount;
}

static void
mark_executable_lines (GArray *lines,
                       guint  *executable_lines,
                       guint  n_executable_lines)
{
  guint i = 0;
  for (; i < n_executable_lines; ++i)
      g_array_index (lines, gint, executable_lines[i]) = 0;
}

static GArray *
lookup_or_create_statistics (GHashTable *table,
                             const gchar *filename)
{
  GArray *statistics = g_hash_table_lookup (table, filename);
  if (statistics)
    return statistics;

  gchar *lines = NULL;
  gsize length = 0;

  if (!g_file_get_contents (filename,
                            &lines,
                            &length,
                            NULL))
    return NULL;

  guint lineCount = count_lines_in_string (lines);

  statistics = g_array_new (TRUE, FALSE, sizeof (gint));
  g_array_set_size (statistics, lineCount);
  memset (statistics->data, -1, sizeof (gint) * statistics->len);
  g_hash_table_insert (table, g_strdup (filename), statistics);

  g_free (lines);

  return statistics;
}

static gboolean
strv_element_contained_in_string (const gchar *haystack,
                                  const gchar **strv,
                                  guint       haystack_length)
{
  size_t iterator = 0;
  for (; iterator < haystack_length; ++iterator)
    {
      if (strstr (haystack, strv[iterator]))
        {
          return TRUE;
        }
    }

  return FALSE;
}

static gboolean
str_contained_in_env(const gchar *haystack,
                     const gchar *key)
{
  const gchar *value = g_getenv (key);
  gchar **value_tokens = g_strsplit(value, ":", -1);
  gboolean return_value = strv_element_contained_in_string (haystack,
                                                            (const gchar **) value_tokens,
                                                            g_strv_length (value_tokens));

  g_strfreev (value_tokens);
  return return_value;
}

typedef struct _GjsCoverageData
{
  const gchar **exclude_paths;
  GHashTable  *statistics;
} GjsCoverageData;

static gboolean
should_skip_this_script (const gchar     *filename,
                         GjsCoverageData *coverage_data)
{
  /* We don't want coverage data on installed scripts */
  if (str_contained_in_env (filename, "XDG_DATA_DIRS"))
    return TRUE;
  if (coverage_data->exclude_paths &&
      strv_element_contained_in_string (filename,
                                        coverage_data->exclude_paths,
                                        g_strv_length ((gchar **) coverage_data->exclude_paths)))
    return TRUE;

  return FALSE;
}

void write_to_stream (GOutputStream *ostream,
                       const gchar   *msg)
{
  g_output_stream_write (ostream, msg, strlen (msg), NULL, NULL);
}

typedef struct _GjsCoverageTracefile
{
  const gchar *potential_path;
  GFile       *open_handle;
} GjsCoverageTracefile;

GFile *
delete_file_at_path_and_open_anew (const gchar *path)
{
  GFile *file = g_file_new_for_path (path);
  g_file_delete (file, NULL, NULL);
  g_file_create (file, G_FILE_CREATE_REPLACE_DESTINATION, NULL, NULL);
  return file;
}

GFile *
create_tracefile_for_script_name (const gchar *script_name)
{
  gsize tracefile_name_buffer_size = strlen ((const gchar *) script_name) + 8;
  gchar tracefile_name_buffer[tracefile_name_buffer_size];
  snprintf (tracefile_name_buffer, tracefile_name_buffer_size, "%s.info", (const gchar *) script_name);

  return delete_file_at_path_and_open_anew (tracefile_name_buffer);
}

GFile *
open_tracefile (GjsCoverageTracefile *tracefile_info,
                const gchar          *script_name)
{
  if (tracefile_info->open_handle)
    return g_object_ref (tracefile_info->open_handle);
  if (tracefile_info->potential_path)
    {
      tracefile_info->open_handle = delete_file_at_path_and_open_anew (tracefile_info->potential_path);
      /* Create an extra reference on this file so that we don't have
       * to constantly open and close it */
      g_object_ref (tracefile_info->open_handle);
      return tracefile_info->open_handle;
    }

  return create_tracefile_for_script_name (script_name);
}

GFileIOStream *
get_io_stream_at_end_position_for_tracefile (GFile *file)
{
  GFileIOStream *iostream = g_file_open_readwrite (file, NULL, NULL);
  GError *error = NULL;

  if (!g_seekable_seek (G_SEEKABLE (iostream), 0, SEEK_END, NULL, &error))
    g_error ("Error occurred in seeking output stream: %s", error->message);

  return iostream;
}

void print_statistics_for_files (gpointer key,
                                 gpointer value,
                                 gpointer user_data)
{
  GFile *tracefile = open_tracefile ((GjsCoverageTracefile *) user_data, (const gchar *) key);
  GFileIOStream *iostream = get_io_stream_at_end_position_for_tracefile (tracefile);
  GOutputStream *ostream = g_io_stream_get_output_stream (G_IO_STREAM (iostream));

  write_to_stream (ostream, "SF:");
  write_to_stream (ostream, (const gchar *) key);
  write_to_stream (ostream, "\n");
  write_to_stream (ostream, "FNF:0\n");
  write_to_stream (ostream, "FNH:0\n");
  write_to_stream (ostream, "BRF:0\n");
  write_to_stream (ostream, "BRH:0\n");

  GArray *stats = value;

  guint i = 0;
  guint lines_hit_count = 0;
  guint executable_lines_count = 0;
  for (i = 0; i < stats->len; ++i)
    {
      gchar hit_count_buffer[64];
      gint hit_count_for_line = g_array_index (stats, gint, i);

      if (hit_count_for_line == -1)
          continue;

      write_to_stream (ostream, "DA:");

      snprintf (hit_count_buffer, 64, "%i,%i\n", i, g_array_index (stats, gint, i));
      write_to_stream (ostream, hit_count_buffer);

      if (g_array_index (stats, guint, i))
        ++lines_hit_count;

      ++executable_lines_count;
    }

  gchar lines_hit_buffer[64];
  write_to_stream (ostream, "LH:");
  snprintf (lines_hit_buffer, 64, "%i\n", lines_hit_count);
  write_to_stream (ostream, lines_hit_buffer);
  write_to_stream (ostream, "LF:");
  snprintf (lines_hit_buffer, 64, "%i\n", executable_lines_count);
  write_to_stream (ostream, lines_hit_buffer);
  write_to_stream (ostream, "end_of_record\n");

  g_object_unref (iostream);
  g_object_unref (tracefile);
}

static void
interrupt_callback_for_register (GjsInterruptRegister *reg,
                                 GjsContext           *context,
                                 GjsInterruptInfo     *info,
                                 gpointer             user_data)
{
  const gchar *filename = info->filename;
  GArray *statistics = g_hash_table_lookup ((GHashTable *) user_data,
                                            filename);
  /* This shouldn't really happen, but if it does just return early */
  if (!statistics)
    return;

  guint lineNo = info->line;

  g_assert (lineNo <= statistics->len);

  /* If this happens it is not a huge problem - we only try to
   * filter out lines which we think are not executable so
   * that they don't cause execess noise in coverage reports */
  if (g_array_index (statistics, gint, lineNo) == -1)
      g_array_index (statistics, gint, lineNo) = 0;

  ++(g_array_index (statistics, gint, lineNo));
}

static void
new_script_callback_for_register (GjsInterruptRegister *reg,
                                  GjsContext           *context,
                                  GjsDebugScriptInfo   *info,
                                  gpointer             user_data)
{
  GjsCoverageData *coverage_data = (GjsCoverageData *) user_data;

  /* We don't want coverage data on installed scripts */
  if (should_skip_this_script (info->filename, coverage_data))
    return;

  GArray *statistics = lookup_or_create_statistics (coverage_data->statistics,
                                                    info->filename);
  mark_executable_lines (statistics,
                         info->executable_lines,
                         info->n_executable_lines);
}

int main (int argc, char **argv)
{
  if (argc < 2)
    {
      g_print ("usage: gjs-coverage [FILE]\n");
      return 1;
    }

  static gchar **include_path = NULL;
  static gchar **exclude_from_coverage_path = NULL;
  static gchar *js_version = NULL;
  static gchar *tracefile_output_path = NULL;

  static GOptionEntry entries[] = {
      { "include-path", 'I', 0, G_OPTION_ARG_STRING_ARRAY, &include_path, "Add the directory DIR to the list of directories to search for js files.", "DIR" },
      { "js-version", 0, 0, G_OPTION_ARG_STRING, &js_version, "JavaScript version (e.g. \"default\", \"1.8\"", "JSVERSION" },
      { "exlcude-from-coverage", 'E', 0, G_OPTION_ARG_STRING_ARRAY, &exclude_from_coverage_path, "Exclude the directory DIR from the directories containing files where coverage reports will be generated." "DIR" },
      { "tracefile-output", 'o', 0, G_OPTION_ARG_STRING, &tracefile_output_path, "Write all trace data to a single file FILE.", "FILE" },
      { NULL }
  };

  GOptionContext *option_context = g_option_context_new (NULL);
  GError *error = NULL;

  g_option_context_set_ignore_unknown_options (option_context, TRUE);
  g_option_context_add_main_entries (option_context, entries, NULL);
  if (!g_option_context_parse(option_context, &argc, &argv, &error))
    {
      g_error("failed to parse options: %s", error->message);
      return 1;
    }

  g_option_context_free (option_context);

  GHashTable *statistics =
    g_hash_table_new_full (g_str_hash,
                           g_str_equal,
                           g_free,
                           (GDestroyNotify) g_array_unref);

  const gchar *js_version_to_pass = NULL;

  if (js_version)
    js_version_to_pass = gjs_context_scan_buffer_for_js_version(js_version, 1024);

  GjsContext *context = NULL;

  if (js_version_to_pass)
    {
      context = g_object_new(GJS_TYPE_CONTEXT,
                             "search-path", include_path,
                             "js-version", js_version_to_pass,
                             NULL);
    }
  else
    {
      context = g_object_new(GJS_TYPE_CONTEXT,
                             "search-path", include_path,
                             NULL);
    }

  GjsCoverageData coverage_data =
  {
    (const gchar **) exclude_from_coverage_path,
    statistics
  };

  GjsDebugInterruptRegister *debug_register =
      gjs_debug_interrupt_register_new (context);
  GjsDebugConnection *single_step_connection =
      gjs_interrupt_register_start_singlestep (GJS_INTERRUPT_REGISTER_INTERFACE (debug_register),
                                               interrupt_callback_for_register,
                                               statistics);
  GjsDebugConnection *new_script_hook_connection =
      gjs_interrupt_register_connect_to_script_load (GJS_INTERRUPT_REGISTER_INTERFACE (debug_register),
                                                     new_script_callback_for_register,
                                                     &coverage_data);

  if (!gjs_context_define_string_array (context,
                                        "ARGV",
                                        argc - 2,
                                        (const char **) argv + 2,
                                        NULL))
    {
      g_print ("Failed to define ARGV");
      g_object_unref (context);
      g_hash_table_unref (statistics);
      return 1;
    }

  int status;
  error = NULL;
  if (!gjs_context_eval_file (context, argv[1], &status, &error))
    {
      g_printerr ("Error in evaluating js : %s\n", error->message);
    }

  g_object_unref (single_step_connection);
  g_object_unref (new_script_hook_connection);
  g_object_unref (debug_register);

  GjsCoverageTracefile tracefile_data =
  {
    tracefile_output_path,
    NULL
  };

  g_hash_table_foreach (statistics, print_statistics_for_files, &tracefile_data);

  /* print_statistics_for_files might try to open the tracefile, so
   * we need to check for this and unref it if so */
  if (tracefile_data.open_handle)
    g_object_unref (tracefile_data.open_handle);

  g_hash_table_unref (statistics);
  g_object_unref (context);

  if (include_path)
    g_strfreev(include_path);

  if (exclude_from_coverage_path)
    g_strfreev(exclude_from_coverage_path);

  if (js_version)
    g_free(js_version);

  if (tracefile_output_path)
    g_free(tracefile_output_path);

  return 0;
}
