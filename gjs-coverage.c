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

static void
new_script_hook (JSContext *context,
                 const char *filename,
                 unsigned lineno,
                 JSScript *script,
                 JSFunction *function,
                 void *caller_data)
{
  static const gchar *ignore_patterns[] =
  {
    /* Ignore /share/gjs-1.0 as this is where we keep things like
     * overrids and lang, and we don't care about code coverage there */
    "/share/gjs-1.0/"
  };

  disable_interrupts_for_script (context, script);

  const size_t ignore_patterns_length = sizeof (ignore_patterns) / sizeof (ignore_patterns[0]);
  size_t ignore_patterns_iterator = 0;
  for (; ignore_patterns_iterator < ignore_patterns_length; ++ignore_patterns_iterator)
    {
      if (strstr (filename, ignore_patterns[ignore_patterns_iterator]))
        {
          return;
        }
    }

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
  GArray *statistics = lookup_or_create_statistics (caller_data, filename, data);
  determine_executable_lines (context, script, lineno, statistics, data);

  enable_interrupts_for_script (context, script, interrupt_hook, caller_data);

  g_object_unref (stream);
  g_object_unref (file);
}

void write_to_stream (GOutputStream *ostream,
                       const gchar   *msg)
{
  g_output_stream_write (ostream, msg, strlen (msg), NULL, NULL);
}

void print_statistics_for_files (gpointer key,
                                 gpointer value,
                                 gpointer user_data)
{
  gsize tracefile_name_buffer_size = strlen ((const gchar *) key) + 8;
  gchar tracefile_name_buffer[tracefile_name_buffer_size];
  snprintf (tracefile_name_buffer, tracefile_name_buffer_size, "%s.info", (const gchar *) key);
  GFile *tracefile = g_file_new_for_path (tracefile_name_buffer);
  g_file_delete (tracefile, NULL, NULL);
  g_file_create (tracefile, G_FILE_CREATE_REPLACE_DESTINATION, NULL, NULL);
  GFileIOStream *iostream = g_file_open_readwrite (tracefile, NULL, NULL);
  GOutputStream *ostream = g_io_stream_get_output_stream ((GIOStream *) iostream);
  gchar *current_directory = g_get_current_dir();
  gchar *absolute_path_to_source_file = g_strconcat (current_directory, "/", (const gchar *) key, NULL);
  
  g_free (current_directory);

  write_to_stream (ostream, "SF:");
  write_to_stream (ostream, absolute_path_to_source_file);
  write_to_stream (ostream, "\n");
  write_to_stream (ostream, "FNF:0\n");
  write_to_stream (ostream, "FNH:0\n");
  write_to_stream (ostream, "BRF:0\n");
  write_to_stream (ostream, "BRH:0\n");
  
  g_free (absolute_path_to_source_file);
  
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
}

int main (int argc, char **argv)
{
  if (argc < 2)
    {
      g_print ("usage: gjs-coverage [FILE]\n");
      return 1;
    }

  GHashTable *statistics =
    g_hash_table_new_full (g_str_hash,
                           g_str_equal,
                           g_free,
                           (GDestroyNotify) g_array_unref);

  GjsContext *context = gjs_context_new ();
  JSContext *js_context = gjs_context_get_native_context (context);
  JSRuntime *js_runtime = JS_GetRuntime (js_context);
  JS_BeginRequest (js_context);
  JS_SetOptions (js_context, JSOPTION_METHODJIT);
  JS_SetDebugMode (js_context, TRUE);
  JS_SetNewScriptHookProc (js_runtime, new_script_hook, statistics);
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
  GError *error = NULL;
  if (!gjs_context_eval_file (context, argv[1], &status, &error))
    {
      g_printerr ("Error in evaluating js : %s\n", error->message);
    }

  g_hash_table_foreach (statistics, print_statistics_for_files, NULL);

  g_hash_table_unref (statistics);
  g_object_unref (context);

  return 0;
}
