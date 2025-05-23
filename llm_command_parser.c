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
 * the project author(s). Contributions include any modifications,
 * enhancements, or additions to the project. These contributions become
 * part of the project and are adopted by the project author(s).
 */

/* Std C */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <json-c/json.h>

/* Local */
#include "dawn.h"
#include "logging.h"
#include "mosquitto_comms.h"
#include "text_to_command_nuevo.h"

// Static buffer for command prompt - make it static, make it large
#define PROMPT_BUFFER_SIZE 65536
static char command_prompt[PROMPT_BUFFER_SIZE];
static int prompt_initialized = 0;

/**
 * @brief Builds a simple command prompt string from the commands_config_nuevo.json file
 *
 * This function reads the config file, extracts command patterns,
 * and builds a simple string describing available commands for the LLM.
 */
static void initialize_command_prompt(void) {
   if (prompt_initialized) {
      return;
   }

   FILE *configFile = NULL;
   char buffer[10*1024];
   int bytes_read = 0;
   struct json_object *parsedJson = NULL;
   struct json_object *typesObject = NULL;
   struct json_object *devicesObject = NULL;

   // Start with a simple instruction
   int prompt_len = snprintf(command_prompt, PROMPT_BUFFER_SIZE,
      "%s\n\n"  // Include the original AI_DESCRIPTION first
      "You can also execute commands for me. These are the commands available:\n\n",
      AI_DESCRIPTION);

   LOG_INFO("Static prompt processed. Length: %d", prompt_len);

   // Read the config file
   configFile = fopen(CONFIG_FILE, "r");
   if (configFile == NULL) {
      LOG_ERROR("Unable to open config file: %s", CONFIG_FILE);
      return;
   }

   if ((bytes_read = fread(buffer, 1, sizeof(buffer), configFile)) > 0) {
      buffer[bytes_read] = '\0';
   } else {
      LOG_ERROR("Failed to read config file");
      fclose(configFile);
      return;
   }


   if (bytes_read == sizeof(buffer)) {
      LOG_ERROR("Config file buffer is too small.");
      fclose(configFile);
      return;
   }

   fclose(configFile);

   LOG_INFO("Config file read for AI prompt. Length: %d", bytes_read);

   // Parse JSON
   parsedJson = json_tokener_parse(buffer);
   if (parsedJson == NULL) {
      LOG_ERROR("Failed to parse config JSON");
      return;
   }

   // Get the "types" and "devices" objects
   if (!json_object_object_get_ex(parsedJson, "types", &typesObject) ||
       !json_object_object_get_ex(parsedJson, "devices", &devicesObject)) {
      LOG_ERROR("Required objects not found in json");
      json_object_put(parsedJson);
      return;
   }

   // Add a section for each command type
   struct json_object_iterator type_it = json_object_iter_begin(typesObject);
   struct json_object_iterator type_it_end = json_object_iter_end(typesObject);

   while (!json_object_iter_equal(&type_it, &type_it_end)) {
      const char *type_name = json_object_iter_peek_name(&type_it);
      struct json_object *type_obj;
      json_object_object_get_ex(typesObject, type_name, &type_obj);

      prompt_len += snprintf(command_prompt + prompt_len, PROMPT_BUFFER_SIZE - prompt_len,
                           "== %s Commands ==\n", type_name);

      // Get the actions for this type
      struct json_object *actions_obj;
      if (json_object_object_get_ex(type_obj, "actions", &actions_obj)) {
         struct json_object_iterator action_it = json_object_iter_begin(actions_obj);
         struct json_object_iterator action_it_end = json_object_iter_end(actions_obj);

         while (!json_object_iter_equal(&action_it, &action_it_end)) {
            const char *action_name = json_object_iter_peek_name(&action_it);
            struct json_object *action_obj;
            json_object_object_get_ex(actions_obj, action_name, &action_obj);

            struct json_object *command_obj;
            if (json_object_object_get_ex(action_obj, "action_command", &command_obj)) {
               const char *command = json_object_get_string(command_obj);

               // Add the command format
               prompt_len += snprintf(command_prompt + prompt_len, PROMPT_BUFFER_SIZE - prompt_len,
                                   "- %s: %s\n", action_name, command);
            }

            json_object_iter_next(&action_it);
         }
      }

      // Add a list of devices for this type
      prompt_len += snprintf(command_prompt + prompt_len, PROMPT_BUFFER_SIZE - prompt_len,
                           "  Valid devices: ");

      // Find all devices of this type
      int device_count = 0;
      struct json_object_iterator dev_it = json_object_iter_begin(devicesObject);
      struct json_object_iterator dev_it_end = json_object_iter_end(devicesObject);

      while (!json_object_iter_equal(&dev_it, &dev_it_end)) {
         const char *device_name = json_object_iter_peek_name(&dev_it);
         struct json_object *device_obj;
         json_object_object_get_ex(devicesObject, device_name, &device_obj);

         struct json_object *device_type_obj;
         if (json_object_object_get_ex(device_obj, "type", &device_type_obj)) {
            const char *device_type = json_object_get_string(device_type_obj);

            if (strcmp(device_type, type_name) == 0) {
               if (device_count > 0) {
                  prompt_len += snprintf(command_prompt + prompt_len, PROMPT_BUFFER_SIZE - prompt_len, ", ");
               }
               prompt_len += snprintf(command_prompt + prompt_len, PROMPT_BUFFER_SIZE - prompt_len, "%s", device_name);
               device_count++;
            }
         }

         json_object_iter_next(&dev_it);
      }

      prompt_len += snprintf(command_prompt + prompt_len, PROMPT_BUFFER_SIZE - prompt_len, "\n\n");

      json_object_iter_next(&type_it);
   }

   // Add response format instructions
   prompt_len += snprintf(command_prompt + prompt_len, PROMPT_BUFFER_SIZE - prompt_len,
                        "When I ask for an action that matches one of these commands, respond with both:\n"
                        "1. A conversational response (e.g., \"I'll turn that on for you, sir.\")\n"
                        "2. The exact JSON command enclosed in <command> tags\n\n"
                        "For example: \"Let me turn on the map for you, sir. <command>{\"device\": \"map\", \"action\": \"enable\"}</command>\"\n\n"
                        "Command hints:\n"
                        "The \"viewing\" command will return an image to you so you can visually answer a query.\n"
                        "When running \"play\", the value is a simply string to search the media files for.\n");

   json_object_put(parsedJson);
   prompt_initialized = 1;

   LOG_INFO("AI prompt initialized. Length: %d", prompt_len);
   printf("AI Prompt: \"%s\"\n", command_prompt);
}

/**
 * @brief Gets the command prompt string
 *
 * @return The command prompt string
 */
const char* get_command_prompt(void) {
   if (!prompt_initialized) {
      initialize_command_prompt();
   }
   return command_prompt;
}

/**
 * @brief Parses an LLM response for commands and executes them
 *
 * This function looks for JSON commands enclosed in <command> tags in the LLM response,
 * extracts them, and sends them through the MQTT messaging system.
 *
 * @param llm_response The text response from the LLM
 * @param mosq The MQTT client instance
 * @return The number of commands found and processed
 */
int parse_llm_response_for_commands(const char *llm_response, struct mosquitto *mosq) {
   if (!llm_response || !mosq) {
      return 0;
   }

   int commands_found = 0;
   const char *start_tag = "<command>";
   const char *end_tag = "</command>";
   size_t start_tag_len = strlen(start_tag);
   size_t end_tag_len = strlen(end_tag);

   const char *search_start = llm_response;
   const char *cmd_start, *cmd_end;

   while ((cmd_start = strstr(search_start, start_tag)) != NULL) {
      cmd_start += start_tag_len;
      cmd_end = strstr(cmd_start, end_tag);

      if (cmd_end) {
         // Extract the command
         size_t cmd_len = cmd_end - cmd_start;
         char *command = (char *)malloc(cmd_len + 1);
         if (command) {
            strncpy(command, cmd_start, cmd_len);
            command[cmd_len] = '\0';

            LOG_INFO("Found command: %s", command);

            // Parse JSON
            struct json_object *cmd_json = json_tokener_parse(command);
            if (cmd_json) {
               struct json_object *device_obj, *topic_obj;
               const char *topic = "dawn"; // Default topic

               // Find topic based on device
               if (json_object_object_get_ex(cmd_json, "device", &device_obj)) {
                  const char *device = json_object_get_string(device_obj);

                  // Read config to get topic for device
                  FILE *configFile = fopen(CONFIG_FILE, "r");
                  if (configFile) {
                     char buffer[10*1024];
                     int bytes_read = fread(buffer, 1, sizeof(buffer), configFile);
                     fclose(configFile);

                     if (bytes_read > 0) {
                        buffer[bytes_read] = '\0';
                        struct json_object *config_json = json_tokener_parse(buffer);
                        if (config_json) {
                           struct json_object *devices_obj, *device_config_obj;

                           if (json_object_object_get_ex(config_json, "devices", &devices_obj) &&
                               json_object_object_get_ex(devices_obj, device, &device_config_obj) &&
                               json_object_object_get_ex(device_config_obj, "topic", &topic_obj)) {
                              topic = json_object_get_string(topic_obj);
                           }

                           json_object_put(config_json);
                        }
                     }
                  }

                  // Publish command to MQTT
                  int rc = mosquitto_publish(mosq, NULL, topic, strlen(command), command, 0, false);
                  if (rc != MOSQ_ERR_SUCCESS) {
                     LOG_ERROR("Error publishing command: %s", mosquitto_strerror(rc));
                  } else {
                     commands_found++;
                  }
               }

               json_object_put(cmd_json);
            } else {
               LOG_ERROR("Failed to parse command JSON: %s", command);
            }

            free(command);
         }

         // Continue search after this command
         search_start = cmd_end + end_tag_len;
      } else {
         // No closing tag found, stop searching
         break;
      }
   }

   return commands_found;
}
