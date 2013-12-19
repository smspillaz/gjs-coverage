#include <wchar.h>
#include <jsapi.h>
#include <jsprvtd.h>
#include <string.h>
#include <gio/gio.h>
#include <gjs/gjs.h>

static JSTrapStatus
interrupt_hook (JSContext *context,
                JSScript *script,
                jsbytecode *pc,
                jsval *rval,
                void *closure)
{
  const gchar *filename = (const gchar *) JS_GetScriptFilename (context,
                                                                script);
  GArray *statistics = g_hash_table_lookup ((GHashTable *) closure,
                                            filename);
  /* This shouldn't really happen, but if it does just return early */
  if (!statistics)
    return JSTRAP_CONTINUE;

  guint lineNo = JS_PCToLineNumber (context, script, pc);

  g_assert (lineNo <= statistics->len);

  /* If this happens it is not a huge problem - we only try to
   * filter out lines which we think are not executable so
   * that they don't cause execess noise in coverage reports */
  if (g_array_index (statistics, gint, lineNo) == -1)
      g_array_index (statistics, gint, lineNo) = 0;

  ++(g_array_index (statistics, gint, lineNo));
  return JSTRAP_CONTINUE;
}

static void
set_interrupts (JSContext *context,
                JSScript *script,
                JSInterruptHook hook,
                gpointer user_data)
{
  if (hook)
    JS_SetSingleStepMode (context, script, TRUE);

  JS_SetInterrupt (JS_GetRuntime (context), hook, user_data);
}

JSCrossCompartmentCall * JS_EnterCrossCompartmentCallScript (JSContext *context,
                                                             JSScript  *script);

static void
disable_interrupts_for_script (JSContext *context,
                               JSScript  *script)
{
  JSCrossCompartmentCall *call = JS_EnterCrossCompartmentCallScript (context, script);
  JS_BeginRequest (context);
  set_interrupts (context, script, NULL, NULL);
  JS_EndRequest (context);
  JS_LeaveCrossCompartmentCall (call);
}

static void
enable_interrupts_for_script (JSContext *context,
                              JSScript *script,
                              JSInterruptHook hook,
                              gpointer user_data)
{
  JSCrossCompartmentCall *call = JS_EnterCrossCompartmentCallScript (context, script);
  JS_BeginRequest (context);
  set_interrupts (context, script, hook, user_data);
  JS_EndRequest (context);
  JS_LeaveCrossCompartmentCall (call);
}

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

typedef struct _ExecutableDeterminationData
{
  guint currentLine;
  const gchar *filename;
  GArray *statistics;
} ExecutableDeterminationData;

static gboolean
is_nonexecutable_character (gchar character)
{
  return character == ' ' ||
         character == ';' ||
         character == ']' ||
         character == '}' ||
         character == ')';
}

const gchar *
advance_past_leading_nonexecutable_characters (const gchar *str)
{
  while (is_nonexecutable_character (str[0]))
    ++str;

  return str;
}

static gboolean
is_only_newline (const gchar *str)
{
  if (str[0] == '\n')
    {
      return TRUE;
    }

  return FALSE;
}

static gboolean
is_single_line_comment (const gchar *str)
{
  if (strncmp (str, "//", 2) == 0)
    return TRUE;

  return FALSE;
}

static const gchar *
search_backwards_for_substr (const gchar *haystack,
                             const gchar *needle,
                             const gchar *position)
{
  guint needle_length = strlen (needle);

  /* Search backwards for the first character, then try and
   * strcmp if there's a match. Removes needless jumping around
   * into strcmp */
  while (position != haystack)
    {
      if (position[0] == needle[0] &&
          strncmp (position, needle, needle_length) == 0)
        return position;

      --position;
    }

  return NULL;
}

static gboolean
is_within_comment_block (const gchar *str, const gchar *begin)
{
  static const gchar *previousCommentBeginIdentifier = "/*";
  static const gchar *previousCommentEndIdentifier = "*/";
  const gchar *previousCommentBeginToken = search_backwards_for_substr (begin,
                                                                        previousCommentBeginIdentifier,
                                                                        str);
  const gchar *previousCommentEndToken = search_backwards_for_substr (begin,
                                                                      previousCommentEndIdentifier,
                                                                      str);

  /* We are in a comment block if previousCommentBegin > previousCommentEnd or
   * if there is no previous comment end */
  const gboolean withinCommentBlock =
      previousCommentBeginToken > previousCommentEndToken ||
      previousCommentBeginToken && !previousCommentEndToken;

  return withinCommentBlock;
}

static gboolean
is_nonexecutable_line (const gchar *data,
                       guint       lineNumber)
{
  const gchar *str = data;
  guint lineNo = lineNumber;
  while (--lineNumber)
    {
      str = strstr (str, "\n");
      g_assert (str);
      str += 1;
    }

  str = advance_past_leading_nonexecutable_characters (str);

  return is_only_newline (str) ||
         is_single_line_comment (str) ||
         is_within_comment_block (str, data);
}

static void
determine_executable_lines (JSContext *context,
                            JSScript  *script,
                            guint     begin,
                            GArray    *statistics,
                            const gchar *data)
{
  jsbytecode **program_counters;
  unsigned int *lines;
  unsigned int count;

  if (!JS_GetLinePCs (context, script, begin, UINT_MAX,
                      &count,
                      &lines,
                      &program_counters))
    {
      exit (1);
    }

  unsigned int i = 0;
  for (; i < count; ++i)
    {
      if (g_array_index (statistics, gint, lines[i]) == -1 &&
          !is_nonexecutable_line (data, lines[i]))
          g_array_index (statistics, gint, lines[i]) = 0;
    }

  JS_free (context, lines);
  JS_free (context, program_counters);
}

static GArray *
lookup_or_create_statistics (GHashTable *table,
                             const gchar *filename,
                             const gchar *data)
{
  GArray *statistics = g_hash_table_lookup (table, filename);
  if (statistics)
    return statistics;

  guint lineCount = count_lines_in_string (data);

  statistics = g_array_new (TRUE, FALSE, sizeof (gint));
  g_array_set_size (statistics, lineCount);
  memset (statistics->data, -1, sizeof (gint) * statistics->len);
  g_hash_table_insert (table, g_strdup (filename), statistics);
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

static void
new_script_hook (JSContext *context,
                 const char *filename,
                 unsigned lineno,
                 JSScript *script,
                 JSFunction *function,
                 void *caller_data)
{
  disable_interrupts_for_script (context, script);

  GjsCoverageData *coverage_data = (GjsCoverageData *) caller_data;

  /* We don't want coverage data on installed scripts */
  if (str_contained_in_env (filename, "XDG_DATA_DIRS"))
    return;
  if (coverage_data->exclude_paths &&
      strv_element_contained_in_string (filename,
                                        coverage_data->exclude_paths,
                                        g_strv_length ((gchar **) coverage_data->exclude_paths)))
    return;

  /* Count the number of lines in the file */
  GFile *file = g_file_new_for_path (filename);
  g_assert (file);

  GFileInputStream *stream = g_file_read (file, NULL, NULL);

  /* It isn't a file, so we can't do coverage reports on it */
  if (!stream)
    {
      g_object_unref (file);
      return;
    }

  g_seekable_seek ((GSeekable *) stream, 0, G_SEEK_END, NULL, NULL);
  goffset data_count = g_seekable_tell ((GSeekable *) stream);
  g_object_unref (stream);

  stream = g_file_read (file, NULL, NULL);

  char data[data_count];
  gsize bytes_read;
  g_input_stream_read_all ((GInputStream *) stream, (void *) data, data_count, &bytes_read, NULL, NULL);

  g_assert (bytes_read == data_count);

  /* Add information about any executable lines to this script */
  GArray *statistics = lookup_or_create_statistics (coverage_data->statistics, filename, data);
  determine_executable_lines (context, script, lineno, statistics, data);

  enable_interrupts_for_script (context, script, interrupt_hook, coverage_data->statistics);

  g_object_unref (stream);
  g_object_unref (file);
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
  g_print("created tracefile %s\n", tracefile_name_buffer);

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

  JSContext *js_context = gjs_context_get_native_context (context);
  JSRuntime *js_runtime = JS_GetRuntime (js_context);
  JS_BeginRequest (js_context);
  JS_SetOptions (js_context, JS_GetOptions (js_context) | JSOPTION_METHODJIT);
  JS_SetDebugMode (js_context, TRUE);
  JS_SetNewScriptHookProc (js_runtime, new_script_hook, &coverage_data);
  JS_EndRequest (js_context);

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
