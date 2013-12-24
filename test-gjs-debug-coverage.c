/*
 * Copyright © 2013 Endless Mobile, Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 *
 * Authored By: Sam Spilsbury <sam@endlessm.com>
 */

#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <gio/gio.h>
#include <gjs/gjs.h>
#include "gjs-debug-interrupt-register.h"
#include "gjs-interrupt-register.h"
#include "gjs-debug-coverage.h"

typedef struct _GjsDebugCoverageFixture
{
  GjsContext           *context;
  GjsInterruptRegister *interrupt_register;
  GjsDebugCoverage     *coverage;
  gchar                *temporary_js_script_directory_name;
  gchar                *temporary_js_script_filename;
  gint                 temporary_js_script_open_handle;
} GjsDebugCoverageFixture;

static void
gjs_debug_coverage_fixture_set_up (gpointer      fixture_data,
                                   gconstpointer user_data)
{
  GjsDebugCoverageFixture *fixture = fixture_data;
  const gchar                      *js_script = "function f () { return 1; }\n";

  fixture->temporary_js_script_directory_name = g_strdup ("/tmp/gjs_debug_coverage_tmp.XXXXXX");
  mkdtemp (fixture->temporary_js_script_directory_name);
  fixture->temporary_js_script_filename = g_strconcat (fixture->temporary_js_script_directory_name,
                                                       "/",
                                                       "gjs_debug_coverage_script_XXXXXX.js",
                                                       NULL);
  fixture->temporary_js_script_open_handle =
      mkstemps (fixture->temporary_js_script_filename, 3);

  /* Allocate a strv that we can pass over to gjs_debug_coverage_new */
  const gchar *coverage_paths[] =
  {
    fixture->temporary_js_script_directory_name,
    NULL
  };

  const gchar *search_paths[] =
  {
    fixture->temporary_js_script_directory_name,
    NULL
  };

  fixture->context = gjs_context_new_with_search_path ((gchar **) search_paths);
  fixture->interrupt_register = GJS_INTERRUPT_REGISTER_INTERFACE (gjs_debug_interrupt_register_new (fixture->context));
  fixture->coverage = gjs_debug_coverage_new (fixture->interrupt_register,
                                              fixture->context,
                                              (gchar **) coverage_paths);

  if (write (fixture->temporary_js_script_open_handle, js_script, strlen (js_script) * sizeof (gchar)) == -1)
    g_print ("Error writing to temporary file: %s", strerror (errno));
}

static void
gjs_debug_coverage_fixture_tear_down (gpointer      fixture_data,
                                      gconstpointer user_data)
{
  GjsDebugCoverageFixture *fixture = fixture_data;
  g_unlink (fixture->temporary_js_script_filename);
  g_free (fixture->temporary_js_script_filename);
  close (fixture->temporary_js_script_open_handle);
  rmdir (fixture->temporary_js_script_directory_name);
  g_free (fixture->temporary_js_script_directory_name);

  g_object_unref (fixture->coverage);
  g_object_unref (fixture->interrupt_register);
  g_object_unref (fixture->context);
}

static void
write_content_to_file_at_beginning (gint        handle,
                                    const gchar *content)
{
  ftruncate (handle, 0);
  lseek (handle, 0, SEEK_SET);
  write (handle, (gconstpointer) content, strlen (content) * sizeof (gchar));
}

typedef struct _GjsDebugCoverageToSingleOutputFileFixture
{
  GjsDebugCoverageFixture base_fixture;
  gchar *output_file_name;
  guint output_file_handle;
} GjsDebugCoverageToSingleOutputFileFixture;

static void
gjs_debug_coverage_to_single_output_file_fixture_set_up (gpointer      fixture_data,
                                                         gconstpointer user_data)
{
  gjs_debug_coverage_fixture_set_up (fixture_data, user_data);

  GjsDebugCoverageToSingleOutputFileFixture *fixture = fixture_data;
  fixture->output_file_name = g_strconcat (fixture->base_fixture.temporary_js_script_directory_name,
                                           "/",
                                           "gjs_debug_coverage_test.XXXXXX.info",
                                           NULL);
  fixture->output_file_handle = mkstemps (fixture->output_file_name, 5);
}

static void
gjs_debug_coverage_to_single_output_file_fixture_tear_down (gpointer      fixture_data,
                                                            gconstpointer user_data)
{
  GjsDebugCoverageToSingleOutputFileFixture *fixture = fixture_data;
  unlink (fixture->output_file_name);
  g_free (fixture->output_file_name);
  close (fixture->output_file_handle);

  gjs_debug_coverage_fixture_tear_down (fixture_data, user_data);
}

static const gchar *
line_starting_with (const gchar *data,
                    const gchar *needle)
{
  const gsize needle_length = strlen (needle);
  const gchar *iter = data;

  while (iter)
    {
      if (strncmp (iter, needle, needle_length) == 0)
        return iter;

      iter = strstr (iter, "\n");

      if (iter)
        iter += 1;
    }

  return NULL;
}

static gchar *
eval_script_and_get_coverage_data (GjsContext       *context,
                                   GjsDebugCoverage *coverage,
                                   const gchar      *filename,
                                   const gchar      *output_filename,
                                   gsize            *coverage_data_length_return)
{
  gjs_context_eval_file (context,
                         filename,
                         NULL,
                         NULL);

  GFile *output_file = g_file_new_for_path (output_filename);
  gjs_debug_coverage_write_statistics (coverage, output_file);
  g_object_unref (output_file);

  gsize coverage_data_length;
  gchar *coverage_data_contents;

  g_file_get_contents (output_filename,
                       &coverage_data_contents,
                       &coverage_data_length,
                       NULL);

  if (coverage_data_length_return)
    *coverage_data_length_return = coverage_data_length;

  return coverage_data_contents;
}

static gboolean
coverage_data_contains_value_for_key (const gchar *data,
                                      const gchar *key,
                                      const gchar *value)
{
  const gchar *sf_line = line_starting_with (data, key);

  if (!sf_line)
    return FALSE;

  return strncmp (&sf_line[strlen (key)],
                  value,
                  strlen (value)) == 0;
}

static void
test_expected_source_file_name_written_to_coverage_data (gpointer      fixture_data,
                                                         gconstpointer user_data)
{
  GjsDebugCoverageToSingleOutputFileFixture *fixture = fixture_data;

  gchar *coverage_data_contents =
      eval_script_and_get_coverage_data (fixture->base_fixture.context,
                                         fixture->base_fixture.coverage,
                                         fixture->base_fixture.temporary_js_script_filename,
                                         fixture->output_file_name,
                                         NULL);

  g_assert (coverage_data_contains_value_for_key (coverage_data_contents,
                                                  "SF:",
                                                  fixture->base_fixture.temporary_js_script_filename));

  g_free (coverage_data_contents);
}

static void
test_zero_branch_coverage_written_to_coverage_data (gpointer      fixture_data,
                                                    gconstpointer user_data)
{
  GjsDebugCoverageToSingleOutputFileFixture *fixture = fixture_data;

  gchar *coverage_data_contents =
      eval_script_and_get_coverage_data (fixture->base_fixture.context,
                                         fixture->base_fixture.coverage,
                                         fixture->base_fixture.temporary_js_script_filename,
                                         fixture->output_file_name,
                                         NULL);

  /* More than one assert per test is bad, but we are testing interlinked concepts */
  g_assert (coverage_data_contains_value_for_key (coverage_data_contents,
                                                  "BRF:",
                                                  "0"));
  g_assert (coverage_data_contains_value_for_key (coverage_data_contents,
                                                  "BRH:",
                                                  "0"));
  g_free (coverage_data_contents);
}

static void
test_zero_function_coverage_written_to_coverage_data (gpointer      fixture_data,
                                                      gconstpointer user_data)
{
  GjsDebugCoverageToSingleOutputFileFixture *fixture = fixture_data;

  gchar *coverage_data_contents =
      eval_script_and_get_coverage_data (fixture->base_fixture.context,
                                         fixture->base_fixture.coverage,
                                         fixture->base_fixture.temporary_js_script_filename,
                                         fixture->output_file_name,
                                         NULL);

  /* More than one assert per test is bad, but we are testing interlinked concepts */
  g_assert (coverage_data_contains_value_for_key (coverage_data_contents,
                                                  "FNF:",
                                                  "0"));
  g_assert (coverage_data_contains_value_for_key (coverage_data_contents,
                                                  "FNH:",
                                                  "0"));
  g_free (coverage_data_contents);
}

static void
test_single_line_hit_written_to_coverage_data (gpointer      fixture_data,
                                               gconstpointer user_data)
{
  GjsDebugCoverageToSingleOutputFileFixture *fixture = fixture_data;

  gchar *coverage_data_contents =
      eval_script_and_get_coverage_data (fixture->base_fixture.context,
                                         fixture->base_fixture.coverage,
                                         fixture->base_fixture.temporary_js_script_filename,
                                         fixture->output_file_name,
                                         NULL);

  /* This is somewhat fragile. If the line specified in the test a different
   * number of instructions in future then this test will break */
  g_assert (coverage_data_contains_value_for_key (coverage_data_contents,
                                                  "DA:",
                                                  "1,3"));
  g_free (coverage_data_contents);
}

static void
test_full_line_tally_written_to_coverage_data (gpointer      fixture_data,
                                               gconstpointer user_data)
{
  GjsDebugCoverageToSingleOutputFileFixture *fixture = fixture_data;

  gchar *coverage_data_contents =
      eval_script_and_get_coverage_data (fixture->base_fixture.context,
                                         fixture->base_fixture.coverage,
                                         fixture->base_fixture.temporary_js_script_filename,
                                         fixture->output_file_name,
                                         NULL);

  /* More than one assert per test is bad, but we are testing interlinked concepts */
  g_assert (coverage_data_contains_value_for_key (coverage_data_contents,
                                                  "LF:",
                                                  "1"));
  g_assert (coverage_data_contains_value_for_key (coverage_data_contents,
                                                  "LH:",
                                                  "1"));
  g_free (coverage_data_contents);
}

static void
test_end_of_record_section_written_to_coverage_data (gpointer      fixture_data,
                                                     gconstpointer user_data)
{
  GjsDebugCoverageToSingleOutputFileFixture *fixture = fixture_data;

  gchar *coverage_data_contents =
      eval_script_and_get_coverage_data (fixture->base_fixture.context,
                                         fixture->base_fixture.coverage,
                                         fixture->base_fixture.temporary_js_script_filename,
                                         fixture->output_file_name,
                                         NULL);

  g_assert (strstr (coverage_data_contents, "end_of_record") != NULL);
  g_free (coverage_data_contents);
}

typedef struct _GjsDebugCoverageMultipleSourceFilesToSingleOutputFileFixture
{
  GjsDebugCoverageToSingleOutputFileFixture base_fixture;
  gchar *second_js_source_file_name;
  guint second_gjs_source_file_handle;
} GjsDebugCoverageMultpleSourceFilesToSingleOutputFileFixture;

static void
gjs_debug_coverage_multiple_source_files_to_single_output_fixture_set_up (gpointer fixture_data,
                                                                          gconstpointer user_data)
{
  gjs_debug_coverage_to_single_output_file_fixture_set_up (fixture_data, user_data);

  GjsDebugCoverageMultpleSourceFilesToSingleOutputFileFixture *fixture = fixture_data;
  fixture->second_js_source_file_name = g_strconcat (fixture->base_fixture.base_fixture.temporary_js_script_directory_name,
                                                     "/",
                                                     "gjs_debug_coverage_second_source_file_XXXXXX.js",
                                                     NULL);
  fixture->second_gjs_source_file_handle = mkstemps (fixture->second_js_source_file_name, 3);

  /* Because GjsDebugCoverage searches the coverage directories at file-creation time,
   * we need to destroy the previously constructed one and construct it again */
  const gchar *coverage_paths[] =
  {
    fixture->base_fixture.base_fixture.temporary_js_script_directory_name,
    NULL
  };

  g_object_unref (fixture->base_fixture.base_fixture.coverage);
  fixture->base_fixture.base_fixture.coverage = gjs_debug_coverage_new (fixture->base_fixture.base_fixture.interrupt_register,
                                                                        fixture->base_fixture.base_fixture.context,
                                                                        (gchar **) coverage_paths);

  gchar *base_name = g_path_get_basename (fixture->base_fixture.base_fixture.temporary_js_script_filename);
  gchar *base_name_without_extension = g_strndup (base_name,
                                                  strlen (base_name) - 3);
  gchar *mock_script = g_strconcat ("const FirstScript = imports.",
                                    base_name_without_extension,
                                    ";\n",
                                    "let a = FirstScript.f;\n"
                                    "\n",
                                    NULL);

  write_content_to_file_at_beginning (fixture->second_gjs_source_file_handle, mock_script);

  g_free (mock_script);
  g_free (base_name_without_extension);
  g_free (base_name);
}

static void
gjs_debug_coverage_multiple_source_files_to_single_output_fixture_tear_down (gpointer      fixture_data,
                                                                             gconstpointer user_data)
{
  GjsDebugCoverageMultpleSourceFilesToSingleOutputFileFixture *fixture = fixture_data;
  unlink (fixture->second_js_source_file_name);
  g_free (fixture->second_js_source_file_name);
  close (fixture->second_gjs_source_file_handle);

  gjs_debug_coverage_to_single_output_file_fixture_tear_down (fixture_data, user_data);
}

static void
test_multiple_source_file_records_written_to_coverage_data (gpointer      fixture_data,
                                                            gconstpointer user_data)
{
  GjsDebugCoverageMultpleSourceFilesToSingleOutputFileFixture *fixture = fixture_data;

  gchar *coverage_data_contents =
      eval_script_and_get_coverage_data (fixture->base_fixture.base_fixture.context,
                                         fixture->base_fixture.base_fixture.coverage,
                                         fixture->second_js_source_file_name,
                                         fixture->base_fixture.output_file_name,
                                         NULL);

  const gchar *first_sf_record = line_starting_with (coverage_data_contents, "SF:");
  const gchar *second_sf_record = line_starting_with (first_sf_record + 1, "SF:");

  g_assert (first_sf_record != NULL);
  g_assert (second_sf_record != NULL);

  g_free (coverage_data_contents);
}

typedef struct _ExpectedSourceFileCoverageData
{
  const gchar *source_file_path;
  const gchar *expected_line_coverage_strings;
  const gchar expected_lines_hit_character;
  const gchar expected_lines_found_character;
} ExpectedSourceFileCoverageData;

static gboolean
check_coverage_data_for_source_file (ExpectedSourceFileCoverageData *expected,
                                     const gsize                    expected_size,
                                     const gchar                    *section_start)
{
  gsize i = 0;
  for (; i < expected_size; ++i)
    {
      if (strncmp (&section_start[3],
                   expected[i].source_file_path,
                   strlen (expected[i].source_file_path)) == 0)
        {
          const gchar *first_line_hit_record = line_starting_with (section_start, "DA:");
          const gboolean line_hits_match = strncmp (first_line_hit_record,
                                                    expected[i].expected_line_coverage_strings,
                                                    strlen (expected[i].expected_line_coverage_strings)) == 0;
          const gchar *total_hits_record = line_starting_with (section_start, "LH:");
          const gboolean total_hits_match = total_hits_record[3] == expected[i].expected_lines_hit_character;
          const gchar *total_found_record = line_starting_with (section_start, "LF:");
          const gboolean total_found_match = total_found_record[3] == expected[i].expected_lines_found_character;

          return line_hits_match &&
                 total_hits_match &&
                 total_found_match;
        }
    }

  return FALSE;
}

static void
test_correct_line_coverage_data_written_for_both_source_file_sectons (gpointer      fixture_data,
                                                                      gconstpointer user_data)
{
  GjsDebugCoverageMultpleSourceFilesToSingleOutputFileFixture *fixture = fixture_data;

  gchar *coverage_data_contents =
      eval_script_and_get_coverage_data (fixture->base_fixture.base_fixture.context,
                                         fixture->base_fixture.base_fixture.coverage,
                                         fixture->second_js_source_file_name,
                                         fixture->base_fixture.output_file_name,
                                         NULL);

  ExpectedSourceFileCoverageData expected[] =
  {
    {
      fixture->base_fixture.base_fixture.temporary_js_script_filename,
      "DA:1,3",
      '1',
      '1'
    },
    {
      fixture->second_js_source_file_name,
      "DA:1,5\n"
      "DA:2,7\n",
      '2',
      '2'
    }
  };

  const gsize expected_len = sizeof (expected) / sizeof (expected[0]);

  const gchar *first_sf_record = line_starting_with (coverage_data_contents, "SF:");
  g_assert (check_coverage_data_for_source_file (expected, expected_len, first_sf_record));

  const gchar *second_sf_record = line_starting_with (first_sf_record + 1, "SF:");
  g_assert (check_coverage_data_for_source_file (expected, expected_len, second_sf_record));

  g_free (coverage_data_contents);
}

typedef struct _FixturedTest
{
  gsize            fixture_size;
  GTestFixtureFunc set_up;
  GTestFixtureFunc tear_down;
} FixturedTest;

static void
add_test_for_fixture (const gchar      *name,
                      FixturedTest     *fixture,
                      GTestFixtureFunc test_func,
                      gconstpointer    user_data)
{
  g_test_add_vtable (name,
                     fixture->fixture_size,
                     user_data,
                     fixture->set_up,
                     test_func,
                     fixture->tear_down);
}

int main (int  argc,
          char **argv)
{
  g_test_init (&argc, &argv, NULL);

  FixturedTest debug_coverage_to_single_output_fixture =
  {
    sizeof (GjsDebugCoverageToSingleOutputFileFixture),
    gjs_debug_coverage_to_single_output_file_fixture_set_up,
    gjs_debug_coverage_to_single_output_file_fixture_tear_down
  };

  add_test_for_fixture ("/gjs/debug/coverage/expected_source_file_name_written_to_coverage_data",
                        &debug_coverage_to_single_output_fixture,
                        test_expected_source_file_name_written_to_coverage_data,
                        NULL);
  add_test_for_fixture ("/gjs/debug/coverage/zero_branch_coverage_written_to_coverage_data",
                        &debug_coverage_to_single_output_fixture,
                        test_zero_branch_coverage_written_to_coverage_data,
                        NULL);
  add_test_for_fixture ("/gjs/debug/coverage/zero_function_coverage_written_to_coverage_data",
                        &debug_coverage_to_single_output_fixture,
                        test_zero_function_coverage_written_to_coverage_data,
                        NULL);
  add_test_for_fixture ("/gjs/debug/coverage/single_line_hit_written_to_coverage_data",
                        &debug_coverage_to_single_output_fixture,
                        test_single_line_hit_written_to_coverage_data,
                        NULL);
  add_test_for_fixture ("/gjs/debug/coverage/full_line_tally_written_to_coverage_data",
                        &debug_coverage_to_single_output_fixture,
                        test_full_line_tally_written_to_coverage_data,
                        NULL);
  add_test_for_fixture ("/gjs/debug/coverage/end_of_record_section_written_to_coverage_data",
                        &debug_coverage_to_single_output_fixture,
                        test_end_of_record_section_written_to_coverage_data,
                        NULL);

  FixturedTest debug_coverage_for_multiple_files_to_single_output_fixture =
  {
    sizeof (GjsDebugCoverageMultpleSourceFilesToSingleOutputFileFixture),
    gjs_debug_coverage_multiple_source_files_to_single_output_fixture_set_up,
    gjs_debug_coverage_multiple_source_files_to_single_output_fixture_tear_down
  };

  add_test_for_fixture ("/gjs/debug/coverage/multiple_source_file_records_written_to_coverage_data",
                        &debug_coverage_for_multiple_files_to_single_output_fixture,
                        test_multiple_source_file_records_written_to_coverage_data,
                        NULL);
  add_test_for_fixture ("/gjs/debug/coverage/correct_line_coverage_data_written_for_both_source_file_sections",
                        &debug_coverage_for_multiple_files_to_single_output_fixture,
                        test_correct_line_coverage_data_written_for_both_source_file_sectons,
                        NULL);
  g_test_run();

  return 0;
}