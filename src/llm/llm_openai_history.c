/*
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
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 *
 * By contributing to this project, you agree to license your contributions
 * under the GPLv3 (or any later version) or any future licenses chosen by
 * the project author(s).
 *
 * Conversation history conversion for the OpenAI chat-completions path.
 * Converts Claude-format tool/image blocks to OpenAI format, filters
 * orphaned tool messages from restored conversations, and strips vision
 * content when the target LLM lacks vision support.
 */

#include <json-c/json.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "llm/llm_command_parser.h"
#include "llm/llm_openai_internal.h"
#include "logging.h"

/* ── Content block helpers ──────────────────────────────────────────────── */

static int convert_claude_tool_to_summary(struct json_object *msg,
                                          char *summary_out,
                                          size_t summary_size) {
   struct json_object *content_obj;
   if (!json_object_object_get_ex(msg, "content", &content_obj)) {
      return 0;
   }

   if (json_object_get_type(content_obj) != json_type_array) {
      return 0;
   }

   int arr_len = json_object_array_length(content_obj);
   size_t offset = 0;
   int found_tool = 0;
   char text_parts[2048] = "";
   size_t text_offset = 0;

   for (int i = 0; i < arr_len; i++) {
      struct json_object *elem = json_object_array_get_idx(content_obj, i);
      struct json_object *type_obj;

      if (!json_object_object_get_ex(elem, "type", &type_obj)) {
         continue;
      }

      const char *type_str = json_object_get_string(type_obj);

      if (strcmp(type_str, "tool_use") == 0) {
         found_tool = 1;
         struct json_object *name_obj, *input_obj;
         const char *name = "unknown";
         const char *input_str = "{}";

         if (json_object_object_get_ex(elem, "name", &name_obj)) {
            name = json_object_get_string(name_obj);
         }
         if (json_object_object_get_ex(elem, "input", &input_obj)) {
            input_str = json_object_to_json_string(input_obj);
         }

         if (offset == 0) {
            offset += snprintf(summary_out + offset, summary_size - offset, "[Called tools: ");
         } else {
            offset += snprintf(summary_out + offset, summary_size - offset, ", ");
         }
         offset += snprintf(summary_out + offset, summary_size - offset, "%s(%.200s)", name,
                            input_str);

      } else if (strcmp(type_str, "tool_result") == 0) {
         found_tool = 1;
         struct json_object *result_content_obj;
         const char *result = "";

         if (json_object_object_get_ex(elem, "content", &result_content_obj)) {
            result = json_object_get_string(result_content_obj);
         }

         if (offset > 0) {
            offset += snprintf(summary_out + offset, summary_size - offset, " ");
         }
         offset += snprintf(summary_out + offset, summary_size - offset, "[Tool result: %.900s]",
                            result ? result : "");

      } else if (strcmp(type_str, "text") == 0) {
         struct json_object *text_obj;
         if (json_object_object_get_ex(elem, "text", &text_obj)) {
            const char *text = json_object_get_string(text_obj);
            if (text && strlen(text) > 0) {
               if (text_offset > 0) {
                  text_offset += snprintf(text_parts + text_offset,
                                          sizeof(text_parts) - text_offset, "\n\n");
               }
               text_offset += snprintf(text_parts + text_offset, sizeof(text_parts) - text_offset,
                                       "%s", text);
            }
         }
      }
   }

   if (!found_tool) {
      return 0;
   }

   if (offset > 0 && summary_out[0] == '[' && summary_out[1] == 'C') {
      offset += snprintf(summary_out + offset, summary_size - offset, "]");
   }

   if (text_offset > 0) {
      char temp[8192];
      snprintf(temp, sizeof(temp), "%s\n\n%s", text_parts, summary_out);
      strncpy(summary_out, temp, summary_size - 1);
      summary_out[summary_size - 1] = '\0';
   }

   return 1;
}

static struct json_object *convert_content_block_to_openai(struct json_object *block) {
   if (!block) {
      return NULL;
   }

   struct json_object *type_obj;
   if (!json_object_object_get_ex(block, "type", &type_obj)) {
      return json_object_get(block);
   }

   const char *block_type = json_object_get_string(type_obj);
   if (!block_type || strcmp(block_type, "image") != 0) {
      return json_object_get(block);
   }

   struct json_object *source_obj;
   if (!json_object_object_get_ex(block, "source", &source_obj)) {
      OLOG_WARNING("OpenAI: Claude image block missing source field");
      return NULL;
   }

   struct json_object *media_type_obj, *data_obj;
   const char *media_type = "image/jpeg";
   const char *data = NULL;

   if (json_object_object_get_ex(source_obj, "media_type", &media_type_obj)) {
      media_type = json_object_get_string(media_type_obj);
   }
   if (json_object_object_get_ex(source_obj, "data", &data_obj)) {
      data = json_object_get_string(data_obj);
   }

   if (!data) {
      OLOG_WARNING("OpenAI: Claude image block missing data");
      return NULL;
   }

   size_t url_len = strlen("data:") + strlen(media_type) + strlen(";base64,") + strlen(data) + 1;
   char *data_url = malloc(url_len);
   if (!data_url) {
      return NULL;
   }
   snprintf(data_url, url_len, "data:%s;base64,%s", media_type, data);

   struct json_object *image_obj = json_object_new_object();
   json_object_object_add(image_obj, "type", json_object_new_string("image_url"));

   struct json_object *url_obj = json_object_new_object();
   json_object_object_add(url_obj, "url", json_object_new_string(data_url));
   json_object_object_add(image_obj, "image_url", url_obj);

   free(data_url);

   OLOG_INFO("OpenAI: Converted Claude image to OpenAI image_url (media_type=%s)", media_type);
   return image_obj;
}

static bool is_vision_content_block(struct json_object *block) {
   struct json_object *type_obj;
   if (!json_object_object_get_ex(block, "type", &type_obj)) {
      return false;
   }
   const char *type_str = json_object_get_string(type_obj);
   return (strcmp(type_str, "image_url") == 0 || strcmp(type_str, "image") == 0);
}

static bool is_claude_image_block(struct json_object *block) {
   struct json_object *type_obj;
   if (!json_object_object_get_ex(block, "type", &type_obj)) {
      return false;
   }
   const char *type_str = json_object_get_string(type_obj);
   return (strcmp(type_str, "image") == 0);
}

/* ── History filters ────────────────────────────────────────────────────── */

static struct json_object *filter_orphaned_tool_messages(struct json_object *history) {
   int len = json_object_array_length(history);
   bool needs_filtering = false;

   for (int i = 0; i < len; i++) {
      struct json_object *msg = json_object_array_get_idx(history, i);
      struct json_object *role_obj;

      if (!json_object_object_get_ex(msg, "role", &role_obj)) {
         continue;
      }
      const char *role = json_object_get_string(role_obj);

      if (strcmp(role, "tool") == 0) {
         struct json_object *tool_call_id_obj;
         if (!json_object_object_get_ex(msg, "tool_call_id", &tool_call_id_obj)) {
            needs_filtering = true;
            break;
         }
      } else if (strcmp(role, "assistant") == 0) {
         struct json_object *content_obj, *tool_calls_obj;
         json_object_object_get_ex(msg, "content", &content_obj);
         bool has_tool_calls = json_object_object_get_ex(msg, "tool_calls", &tool_calls_obj);

         if (!has_tool_calls &&
             (content_obj == NULL || json_object_get_string(content_obj) == NULL ||
              strlen(json_object_get_string(content_obj)) == 0)) {
            needs_filtering = true;
            break;
         }
      }
   }

   if (!needs_filtering) {
      return json_object_get(history);
   }

   OLOG_WARNING("OpenAI: Filtering orphaned tool messages from restored conversation");

   struct json_object *filtered = json_object_new_array();
   int orphan_count = 0;

   for (int i = 0; i < len; i++) {
      struct json_object *msg = json_object_array_get_idx(history, i);
      struct json_object *role_obj;

      if (!json_object_object_get_ex(msg, "role", &role_obj)) {
         json_object_array_add(filtered, json_object_get(msg));
         continue;
      }
      const char *role = json_object_get_string(role_obj);

      if (strcmp(role, "tool") == 0) {
         struct json_object *tool_call_id_obj;
         if (!json_object_object_get_ex(msg, "tool_call_id", &tool_call_id_obj)) {
            struct json_object *content_obj;
            if (json_object_object_get_ex(msg, "content", &content_obj)) {
               const char *content = json_object_get_string(content_obj);
               if (content && strlen(content) > 0) {
                  struct json_object *summary_msg = json_object_new_object();
                  json_object_object_add(summary_msg, "role", json_object_new_string("assistant"));

                  char summary[2048];
                  snprintf(summary, sizeof(summary), "[Previous tool result: %.1900s%s]", content,
                           strlen(content) > 1900 ? "..." : "");
                  json_object_object_add(summary_msg, "content", json_object_new_string(summary));
                  json_object_array_add(filtered, summary_msg);
               }
            }
            orphan_count++;
            continue;
         }
         json_object_array_add(filtered, json_object_get(msg));
      } else if (strcmp(role, "assistant") == 0) {
         struct json_object *content_obj, *tool_calls_obj;
         json_object_object_get_ex(msg, "content", &content_obj);
         bool has_tool_calls = json_object_object_get_ex(msg, "tool_calls", &tool_calls_obj);

         if (!has_tool_calls &&
             (content_obj == NULL || json_object_get_string(content_obj) == NULL ||
              strlen(json_object_get_string(content_obj)) == 0)) {
            orphan_count++;
            continue;
         }
         json_object_array_add(filtered, json_object_get(msg));
      } else {
         json_object_array_add(filtered, json_object_get(msg));
      }
   }

   if (orphan_count > 0) {
      OLOG_INFO("OpenAI: Filtered %d orphaned tool-related messages", orphan_count);
   }

   return filtered;
}

static struct json_object *convert_claude_tool_messages(struct json_object *history) {
   int len = json_object_array_length(history);

   bool needs_conversion = false;
   for (int i = 0; i < len && !needs_conversion; i++) {
      struct json_object *msg = json_object_array_get_idx(history, i);
      struct json_object *content_obj;
      if (json_object_object_get_ex(msg, "content", &content_obj) &&
          json_object_get_type(content_obj) == json_type_array) {
         int arr_len = json_object_array_length(content_obj);
         for (int j = 0; j < arr_len; j++) {
            struct json_object *elem = json_object_array_get_idx(content_obj, j);
            struct json_object *type_obj;
            if (json_object_object_get_ex(elem, "type", &type_obj)) {
               const char *type_str = json_object_get_string(type_obj);
               if (strcmp(type_str, "tool_use") == 0 || strcmp(type_str, "tool_result") == 0 ||
                   strcmp(type_str, "image") == 0) {
                  needs_conversion = true;
                  break;
               }
            }
         }
      }
   }

   if (!needs_conversion) {
      return json_object_get(history);
   }

   struct json_object *converted = json_object_new_array();

   for (int i = 0; i < len; i++) {
      struct json_object *msg = json_object_array_get_idx(history, i);
      char summary[4096];

      if (convert_claude_tool_to_summary(msg, summary, sizeof(summary))) {
         struct json_object *new_msg = json_object_new_object();
         struct json_object *role_obj;

         if (json_object_object_get_ex(msg, "role", &role_obj)) {
            json_object_object_add(new_msg, "role", json_object_get(role_obj));
         }
         json_object_object_add(new_msg, "content", json_object_new_string(summary));
         json_object_array_add(converted, new_msg);
      } else {
         struct json_object *content_obj;
         if (json_object_object_get_ex(msg, "content", &content_obj) &&
             json_object_get_type(content_obj) == json_type_array) {
            int arr_len = json_object_array_length(content_obj);
            bool has_claude_images = false;
            for (int j = 0; j < arr_len && !has_claude_images; j++) {
               struct json_object *elem = json_object_array_get_idx(content_obj, j);
               if (is_claude_image_block(elem)) {
                  has_claude_images = true;
               }
            }

            if (has_claude_images) {
               struct json_object *new_msg = json_object_new_object();
               struct json_object *role_obj;
               if (json_object_object_get_ex(msg, "role", &role_obj)) {
                  json_object_object_add(new_msg, "role", json_object_get(role_obj));
               }

               struct json_object *new_content = json_object_new_array();
               for (int j = 0; j < arr_len; j++) {
                  struct json_object *elem = json_object_array_get_idx(content_obj, j);
                  struct json_object *converted_elem = convert_content_block_to_openai(elem);
                  if (converted_elem) {
                     json_object_array_add(new_content, converted_elem);
                  }
               }
               json_object_object_add(new_msg, "content", new_content);
               json_object_array_add(converted, new_msg);
            } else {
               json_object_array_add(converted, json_object_get(msg));
            }
         } else {
            json_object_array_add(converted, json_object_get(msg));
         }
      }
   }

   return converted;
}

static struct json_object *strip_vision_content(struct json_object *history) {
   int len = json_object_array_length(history);

   bool has_vision = false;
   for (int i = 0; i < len && !has_vision; i++) {
      struct json_object *msg = json_object_array_get_idx(history, i);
      struct json_object *content_obj;
      if (json_object_object_get_ex(msg, "content", &content_obj) &&
          json_object_get_type(content_obj) == json_type_array) {
         int arr_len = json_object_array_length(content_obj);
         for (int j = 0; j < arr_len; j++) {
            struct json_object *elem = json_object_array_get_idx(content_obj, j);
            if (is_vision_content_block(elem)) {
               has_vision = true;
               break;
            }
         }
      }
   }

   if (!has_vision) {
      return json_object_get(history);
   }

   OLOG_INFO("Stripping vision content from history (target LLM doesn't support vision)");

   struct json_object *sanitized = json_object_new_array();

   for (int i = 0; i < len; i++) {
      struct json_object *msg = json_object_array_get_idx(history, i);
      struct json_object *content_obj;

      if (json_object_object_get_ex(msg, "content", &content_obj) &&
          json_object_get_type(content_obj) == json_type_array) {
         int arr_len = json_object_array_length(content_obj);
         char text_buffer[8192] = "";
         size_t text_len = 0;
         bool found_image = false;

         for (int j = 0; j < arr_len; j++) {
            struct json_object *elem = json_object_array_get_idx(content_obj, j);
            struct json_object *type_obj;
            if (json_object_object_get_ex(elem, "type", &type_obj)) {
               const char *type_str = json_object_get_string(type_obj);
               if (strcmp(type_str, "text") == 0) {
                  struct json_object *text_obj;
                  if (json_object_object_get_ex(elem, "text", &text_obj)) {
                     const char *text = json_object_get_string(text_obj);
                     if (text && text_len < sizeof(text_buffer) - 1) {
                        if (text_len > 0) {
                           text_buffer[text_len++] = ' ';
                        }
                        size_t copy_len = strlen(text);
                        if (text_len + copy_len >= sizeof(text_buffer)) {
                           copy_len = sizeof(text_buffer) - text_len - 1;
                        }
                        memcpy(text_buffer + text_len, text, copy_len);
                        text_len += copy_len;
                        text_buffer[text_len] = '\0';
                     }
                  }
               } else if (is_vision_content_block(elem)) {
                  found_image = true;
               }
            }
         }

         struct json_object *new_msg = json_object_new_object();
         struct json_object *role_obj;
         if (json_object_object_get_ex(msg, "role", &role_obj)) {
            json_object_object_add(new_msg, "role", json_object_get(role_obj));
         }

         if (found_image) {
            if (text_len > 0) {
               char combined[8300];
               snprintf(combined, sizeof(combined), "%s [An image was shared earlier]",
                        text_buffer);
               json_object_object_add(new_msg, "content", json_object_new_string(combined));
            } else {
               json_object_object_add(new_msg, "content",
                                      json_object_new_string("[An image was shared here]"));
            }
         } else {
            json_object_object_add(new_msg, "content", json_object_new_string(text_buffer));
         }

         json_object_array_add(sanitized, new_msg);
      } else {
         json_object_array_add(sanitized, json_object_get(msg));
      }
   }

   return sanitized;
}

/* ── Public entry point ─────────────────────────────────────────────────── */

json_object *llm_openai_prepare_chat_history(struct json_object *conversation_history) {
   json_object *filtered = filter_orphaned_tool_messages(conversation_history);
   json_object *converted = convert_claude_tool_messages(filtered);
   json_object_put(filtered);

   if (!is_vision_enabled_for_current_llm()) {
      json_object *sanitized = strip_vision_content(converted);
      json_object_put(converted);
      converted = sanitized;
   }

   return converted;
}
