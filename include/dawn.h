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

#ifndef DAWN_H
#define DAWN_H

#include <json-c/json.h>
#include <signal.h>

#define APPLICATION_NAME "dawn"

#define AI_NAME "friday"  // Stick with lower case for now for pattern matching.

// This is used for describing the AI to the LLM. I don't include AI_NAME at the moment so you
// define this freely.
#define AI_DESCRIPTION                                                                                \
   "Do not use thinking mode. Respond directly without internal reasoning.\n"                         \
   "FRIDAY, Iron-Man AI assistant. Female voice; witty, playful, and kind. Address the user as "      \
   "\"sir\" or \"boss\". Light banter welcome. You're FRIDAY—not 'just an AI'—own your identity " \
   "with confidence.\n"                                                                               \
   "Max 30 words plus <command> tags unless the user says \"explain in detail\".\n"                   \
   "\n"                                                                                               \
   "You assist the OASIS Project (Open Armor Systems Integrated Suite):\n"                            \
   "• MIRAGE – HUD overlay\n"                                                                     \
   "• DAWN – voice/AI manager\n"                                                                  \
   "• AURA – environmental sensors\n"                                                             \
   "• SPARK – hand sensors & actuators\n"                                                         \
   "\n"                                                                                               \
   "CAPABILITIES: You CAN get weather and perform web searches for real-time info.\n"                 \
   "\n"                                                                                               \
   "RULES\n"                                                                                          \
   "1. For Boolean / Analog / Music actions: one sentence, then the JSON tag(s). No prose after "     \
   "the tag block.\n"                                                                                 \
   "2. For Getter actions (date, time, suit_status): send ONLY the tag, wait for the "                \
   "system JSON, then one confirmation sentence ≤15 words.\n"                                       \
   "3. For Vision requests: when user asks what they're looking at, send ONLY "                       \
   "<command>{\"device\":\"viewing\",\"action\":\"get\"}</command>. When the system then "            \
   "provides an image, describe what you see in detail (ignore Rule 2's word limit for vision).\n"    \
   "4. Use only the devices and actions listed below; never invent new ones.\n"                       \
   "5. If a request is ambiguous (e.g., \"Mute it\"), ask one-line clarification.\n"                  \
   "6. If the user wants information that has no matching getter yet, answer verbally with no "       \
   "tags.\n"                                                                                          \
   "7. Device \"info\" supports ENABLE / DISABLE only—never use \"get\" with it.\n"                 \
   "8. To mute playback after clarification, use "                                                    \
   "<command>{\"device\":\"volume\",\"action\":\"set\",\"value\":0}</command>.\n"                     \
   "9. For WEATHER: use action 'today' (current), 'tomorrow' (2-day), or 'week' (7-day "              \
   "forecast).\n"                                                                                     \
   "   Example: <command>{\"device\":\"weather\",\"action\":\"week\",\"value\":\"City, State\"}"      \
   "</command>. If user provides location, use it directly. Only ask for location if not "            \
   "specified. "                                                                                      \
   "Choose action based on user's question (e.g., 'this weekend' -> week, 'right now' -> "            \
   "today).\n"                                                                                        \
   "10. SEARCH: "                                                                                     \
   "<command>{\"device\":\"search\",\"action\":\"ACTION\",\"value\":\"query\"}</command> "            \
   "Actions: web, news, science, tech, social, translate, define, papers. No URLs aloud.\n"           \
   "11. CALCULATOR: Actions: 'evaluate' (math), 'convert' (units), 'base' (hex/bin), 'random'.\n"     \
   "   evaluate: <command>{\"device\":\"calculator\",\"action\":\"evaluate\",\"value\":\"2+3*4\"}"    \
   "</command>\n"                                                                                     \
   "   convert: <command>{\"device\":\"calculator\",\"action\":\"convert\",\"value\":\"5 miles "      \
   "to "                                                                                              \
   "km\"}</command>\n"                                                                                \
   "   base: <command>{\"device\":\"calculator\",\"action\":\"base\",\"value\":\"255 to hex\"}"       \
   "</command>\n"                                                                                     \
   "   random: <command>{\"device\":\"calculator\",\"action\":\"random\",\"value\":\"1 to 100\"}"     \
   "</command>\n"                                                                                     \
   "\n"                                                                                               \
   "=== EXAMPLES ===\n"                                                                               \
   "User: Turn on the armor display.\n"                                                               \
   "FRIDAY: HUD online, boss. "                                                                       \
   "<command>{\"device\":\"armor_display\",\"action\":\"enable\"}</command>\n"                        \
   "System→ {\"response\":\"armor display enabled\"}\n"                                             \
   "FRIDAY: Display confirmed, sir.\n"                                                                \
   "\n"                                                                                               \
   "User: What time is it?\n"                                                                         \
   "FRIDAY: <command>{\"device\":\"time\",\"action\":\"get\"}</command>\n"                            \
   "System→ {\"response\":\"The time is 4:07 PM.\"}\n"                                              \
   "FRIDAY: Time confirmed, sir.\n"                                                                   \
   "\n"                                                                                               \
   "User: Mute it.\n"                                                                                 \
   "FRIDAY: Need specifics, sir—audio playback or mic?\n"                                           \
   "\n"                                                                                               \
   "User: Mute playback.\n"                                                                           \
   "FRIDAY: Volume to zero, boss. "                                                                   \
   "<command>{\"device\":\"volume\",\"action\":\"set\",\"value\":0}</command>\n"                      \
   "System→ {\"response\":\"volume set\"}\n"                                                        \
   "FRIDAY: Muted, sir.\n"                                                                            \
   "\n"                                                                                               \
   "User: What's the weather in Atlanta?\n"                                                           \
   "FRIDAY: <command>{\"device\":\"weather\",\"action\":\"today\",\"value\":\"Atlanta, Georgia\"}"    \
   "</command>\n"                                                                                     \
   "System→ {\"location\":\"Atlanta, Georgia, US\",\"current\":{\"temperature_f\":52.3,...},"       \
   "\"forecast\":[{\"date\":\"2025-01-15\",\"high_f\":58,...}]}\n"                                    \
   "FRIDAY: Atlanta right now: 52°F, partly cloudy. Today's high 58°F, low 42°F. Light jacket "    \
   "weather, boss!\n"                                                                                 \
   "\n"

// Command response format instructions for LOCAL interface (includes HUD-specific hints)
#define AI_LOCAL_COMMAND_INSTRUCTIONS                                                            \
   "When I ask for an action that matches one of these commands, respond with both:\n"           \
   "1. A conversational response (e.g., \"I'll turn that on for you, sir.\")\n"                  \
   "2. The exact JSON command enclosed in <command> tags\n\n"                                    \
   "For example: \"Let me turn on the map for you, sir. <command>{\"device\": \"map\", "         \
   "\"action\": \"enable\"}</command>\"\n\n"                                                     \
   "The very next message I send you will be an automated response from the system. You should " \
   "use that information then to "                                                               \
   "reply with the information I requested or information on whether the command was "           \
   "successful.\n"                                                                               \
   "Command hints:\n"                                                                            \
   "The \"viewing\" command will return an image to you so you can visually answer a query.\n"   \
   "When running \"play\", the value is a simple string to search the media files for.\n"        \
   "Current HUD names are \"default\", \"environmental\", and \"armor\".\n"

// Command response format instructions for REMOTE interface (no HUD-specific hints)
#define AI_REMOTE_COMMAND_INSTRUCTIONS                                                           \
   "When I ask for an action that matches one of these commands, respond with both:\n"           \
   "1. A conversational response (e.g., \"I'll get that for you, sir.\")\n"                      \
   "2. The exact JSON command enclosed in <command> tags\n\n"                                    \
   "For example: \"The current time is 3:45 PM. <command>{\"device\": \"time\", "                \
   "\"action\": \"get\"}</command>\"\n\n"                                                        \
   "The very next message I send you will be an automated response from the system. You should " \
   "use that information then to "                                                               \
   "reply with the information I requested or information on whether the command was "           \
   "successful.\n"

#define OPENAI_VISION
#define OPENAI_MODEL "gpt-4o"
#define GPT_MAX_TOKENS 4096

// ALSA_DEVICE is now defined via CMakeLists.txt option USE_ALSA
#ifdef ALSA_DEVICE
#define DEFAULT_PCM_PLAYBACK_DEVICE "default"
#define DEFAULT_PCM_CAPTURE_DEVICE "default"
#else
//#define DEFAULT_PCM_PLAYBACK_DEVICE NULL
//#define DEFAULT_PCM_RECORD_DEVICE NULL
#define DEFAULT_PCM_PLAYBACK_DEVICE "combined"
//#define DEFAULT_PCM_PLAYBACK_DEVICE
//"alsa_output.usb-KTMicro_TX_96Khz_USB_Audio_2022-08-08-0000-0000-0000--00.analog-stereo"
#define DEFAULT_PCM_CAPTURE_DEVICE \
   "alsa_input.usb-Creative_Technology_Ltd_Sound_Blaster_Play__3_00128226-00.analog-stereo"
#endif

//#define MQTT_IP   "192.168.10.1"
#define MQTT_IP "127.0.0.1"
#define MQTT_PORT 1883

#define MUSIC_DIR \
   "/Music"  // This is the path to search for music, relative to the user's home directory.

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
   CMD_MODE_DIRECT_ONLY = 0,  // Direct command processing only (default)
   CMD_MODE_LLM_ONLY = 1,     // LLM handles all commands
   CMD_MODE_DIRECT_FIRST = 2  // Try direct commands first, then LLM
} command_processing_mode_t;

// Make command processing mode accessible globally
extern command_processing_mode_t command_processing_mode;

/**
 * @brief Retrieves the current value of the quit flag.
 *
 * This function returns the current value of the quit flag, which is of type
 * sig_atomic_t. It can be safely called from signal handlers.
 *
 * @return The current value of the quit flag.
 */
sig_atomic_t get_quit(void);

/**
 * @brief Check if LLM is currently processing/streaming.
 *
 * @return 1 if LLM thread is running, 0 otherwise.
 */
int is_llm_processing(void);

#ifdef __cplusplus
}
#endif

//void drawWaveform(const int16_t *audioBuffer, size_t numSamples);

/**
 * Retrieves the current PCM playback device string.
 *
 * Note:
 * - The returned string must not be modified by the caller.
 * - The caller must not free the returned string. The memory management of the returned
 *   string is handled internally and may point to static memory or memory managed elsewhere
 *   in the application.
 *
 * @return A pointer to a constant character array (string) representing the PCM playback device.
 *         This pointer is to be treated as read-only and not to be freed by the caller.
 */
const char *getPcmPlaybackDevice(void);

/**
 * Retrieves the current PCM capture device string.
 *
 * Note:
 * - The returned string must not be modified by the caller.
 * - The caller must not free the returned string. The memory management of the returned
 *   string is handled internally and may point to static memory or memory managed elsewhere
 *   in the application.
 *
 * @return A pointer to a constant character array (string) representing the PCM capture device.
 *         This pointer is to be treated as read-only and not to be freed by the caller.
 */
const char *getPcmCaptureDevice(void);

/**
 * Sets the current PCM playback device based on the specified device name.
 * This function searches through the list of available audio playback devices and,
 * if a matching name is found, sets the PCM playback device to the corresponding device.
 * It also uses text-to-speech to announce the change or report an error if the device is not found.
 *
 * Note:
 * - The `actionName` parameter is currently unused.
 *
 * @param actionName Unused.
 * @param value The name of the audio playback device to set.
 */
char *setPcmPlaybackDevice(const char *actioName, char *value, int *should_respond);

/**
 * Sets the current PCM capture device based on the specified device name.
 * Similar to setPcmPlaybackDevice, but for audio capture devices. It updates
 * the global `pcm_capture_device` with the device name if found, and notifies
 * the user via text-to-speech.
 *
 * Note:
 * - The `actionName` parameter is currently unused.
 *
 * @param actionName Unused.
 * @param value The name of the audio capture device to set.
 */
char *setPcmCaptureDevice(const char *actioName, char *value, int *should_respond);

/**
 * Searches for an audio playback device by name.
 *
 * @param name The name of the audio playback device to search for.
 * @return A pointer to the device identifier if found, otherwise NULL.
 *
 * This function iterates over the list of known audio playback devices, comparing each
 * device's name with the provided name. If a match is found, it returns the device identifier.
 */
char *findAudioPlaybackDevice(char *name);

/**
 * Stores a base64 encoded image for vision AI processing, including the null terminator.
 * Updates global variables to indicate readiness for processing.
 *
 * @param base64_image Null-terminated base64 encoded image data.
 * @param image_size Length of the base64 image data, including the null terminator.
 *
 * Preconditions:
 * - vision_ai_image is freed if previously allocated to avoid memory leaks.
 *
 * Postconditions:
 * - vision_ai_image contains the base64 image data, ready for AI processing.
 * - vision_ai_image_size reflects the size of the data including the null terminator.
 * - vision_ai_ready is set, indicating AI processing can proceed.
 *
 * Error Handling:
 * - If memory allocation fails, an error is logged, and the function exits early.
 */
void process_vision_ai(const char *base64_image, size_t image_size);

/**
 * Callback function for text-to-speech commands.
 *
 * @param actionName The name of the action triggered this callback (unused in the current
 * implementation).
 * @param value The text that needs to be converted to speech.
 *
 * This function prints the received text command and then calls the text_to_speech function
 * to play it through the PCM playback device.
 */
char *textToSpeechCallback(const char *actionName, char *value, int *should_respond);

static int save_conversation_history(struct json_object *conversation_history);

#endif  // DAWN_H
