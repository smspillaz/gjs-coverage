#include <wchar.h>
#include <jsapi.h>
#include <jsprvtd.h>
#include <string.h>
#include <gio/gio.h>
#include <gjs/gjs.h>

#include "gjs-debug-interrupt-register.h"
#include "gjs-interrupt-register.h"
#include "gjs-debug-coverage.h"

int main (int argc, char **argv)
{
  if (argc < 2)
    {
      g_print ("usage: gjs-coverage [FILE]\n");
      return 1;
    }

  static gchar **include_path = NULL;
  static gchar **coverage_path = NULL;
  static gchar *js_version = NULL;
  static gchar *tracefile_output_path = NULL;

  static GOptionEntry entries[] = {
      { "include-path", 'I', 0, G_OPTION_ARG_STRING_ARRAY, &include_path, "Add the directory DIR to the list of directories to search for js files.", "DIR" },
      { "js-version", 0, 0, G_OPTION_ARG_STRING, &js_version, "JavaScript version (e.g. \"default\", \"1.8\"", "JSVERSION" },
      { "coverage-paths", 'C', 0, G_OPTION_ARG_STRING_ARRAY, &coverage_path, "Include the directory DIR and all children in files where we will generate a coverage report." "DIR" },
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

  const gchar *js_version_to_pass = NULL;

  if (js_version)
    js_version_to_pass = gjs_context_scan_buffer_for_js_version (js_version, 1024);

  GjsContext *context = NULL;

  if (js_version_to_pass)
    {
      context = g_object_new (GJS_TYPE_CONTEXT,
                              "search-path", include_path,
                              "js-version", js_version_to_pass,
                              NULL);
    }
  else
    {
      context = g_object_new (GJS_TYPE_CONTEXT,
                              "search-path", include_path,
                              NULL);
    }

  if (!gjs_context_define_string_array (context,
                                        "ARGV",
                                        argc - 2,
                                        (const char **) argv + 2,
                                        NULL))
    {
      g_print ("Failed to define ARGV");
      g_object_unref (context);
      return 1;
    }

  GjsDebugInterruptRegister *debug_register =
      gjs_debug_interrupt_register_new (context);
  GjsDebugCoverage *coverage = gjs_debug_coverage_new (GJS_INTERRUPT_REGISTER_INTERFACE (debug_register),
                                                       context,
                                                       coverage_path);

  int status;
  error = NULL;
  if (!gjs_context_eval_file (context, argv[1], &status, &error))
    {
      g_printerr ("Error in evaluating js : %s\n", error->message);
    }

  GFile *output_file = NULL;

  if (tracefile_output_path)
    {
      output_file = g_file_new_for_path (tracefile_output_path);
      g_file_delete (output_file, NULL, NULL);
      g_file_create (output_file, G_FILE_CREATE_REPLACE_DESTINATION, NULL, NULL);
    }

  gjs_debug_coverage_write_statistics (coverage, output_file);

  if (output_file)
    g_object_unref (output_file);

  g_object_unref (coverage);
  g_object_unref (debug_register);

  if (include_path)
    g_strfreev (include_path);

  if (coverage_path)
    g_strfreev (coverage_path);

  if (js_version)
    g_free(js_version);

  if (tracefile_output_path)
    g_free(tracefile_output_path);

  return 0;
}
