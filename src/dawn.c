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
#include <assert.h>
#include <getopt.h>
#include <math.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* JSON */
#include <json-c/json.h>

/* CURL */
#include <curl/curl.h>

/* Mosquitto */
#include <mosquitto.h>

/* Speech to Text */
#include "asr/asr_interface.h"

/* Local */
#include "asr/vad_silero.h"
#include "audio/audio_capture_thread.h"
#include "audio_utils.h"
#include "dawn.h"
#include "llm/llm_command_parser.h"
#include "llm/llm_interface.h"
#include "logging.h"
#include "mosquitto_comms.h"
#include "network/dawn_network_audio.h"
#include "network/dawn_server.h"
#include "network/dawn_wav_utils.h"
#include "text_to_command_nuevo.h"
#include "tts/text_to_speech.h"
#include "version.h"

// Whisper chunking manager
#include "asr/chunking_manager.h"

// Define the default sample rate for audio capture.
#define DEFAULT_RATE 16000

// Define the default number of audio channels (1 for mono).
#define DEFAULT_CHANNELS 1

// Define the default duration of audio capture in seconds.
#define DEFAULT_CAPTURE_SECONDS 0.05f  // 50ms for responsive VAD (optimized for latency)

// Define the default command timeout in terms of iterations of DEFAULT_CAPTURE_SECONDS.
#define DEFAULT_COMMAND_TIMEOUT 24  // 24 * 0.05s = 1.2 seconds of silence before timeout

// VAD configuration
#define VAD_SAMPLE_SIZE 512               // Silero VAD requires 512 samples (32ms at 16kHz)
#define VAD_SPEECH_THRESHOLD 0.5f         // Probability threshold for speech detection
#define VAD_SILENCE_THRESHOLD 0.3f        // Probability threshold for silence detection
#define VAD_END_OF_SPEECH_DURATION 1.2f   // Seconds of silence to consider speech ended (optimized)
#define VAD_MAX_RECORDING_DURATION 30.0f  // Maximum recording duration (semantic timeout)

// Chunking configuration for Whisper
#define VAD_CHUNK_PAUSE_DURATION \
   0.3f  // Seconds of silence to detect natural pause for chunking (optimized)
#define VAD_MIN_CHUNK_DURATION 1.0f   // Minimum speech duration before allowing chunk
#define VAD_MAX_CHUNK_DURATION 10.0f  // Maximum chunk duration before forcing finalization

// Define the duration for each background audio capture sample in seconds.
#define BACKGROUND_CAPTURE_SECONDS 2
// Number of samples to take (will use minimum RMS to get true silence baseline)
#define BACKGROUND_CAPTURE_SAMPLES 3

// Check if ALSA_DEVICE is defined to include ALSA-specific headers and define ALSA-specific macros.
#ifdef ALSA_DEVICE
#include <alsa/asoundlib.h>

// Define the default ALSA PCM access type (read/write interleaved).
#define DEFAULT_ACCESS SND_PCM_ACCESS_RW_INTERLEAVED

// Define the default ALSA PCM format (16-bit signed little-endian).
#define DEFAULT_FORMAT SND_PCM_FORMAT_S16_LE

// Define the default number of frames per ALSA PCM period.
#define DEFAULT_FRAMES 64
#else
// Include PulseAudio simple API and error handling headers for non-ALSA configurations.
#include <pulse/error.h>
#include <pulse/simple.h>

// Define the default PulseAudio sample format (16-bit signed little-endian).
#define DEFAULT_PULSE_FORMAT PA_SAMPLE_S16LE
#endif

// Define the threshold offset for detecting talking in the audio stream.
// RMS detection removed - using VAD only

static char pcm_capture_device[MAX_WORD_LENGTH + 1] = "";
static char pcm_playback_device[MAX_WORD_LENGTH + 1] = "";

/* Parsed audio devices. */
static audioDevices captureDevices[MAX_AUDIO_DEVICES]; /**< Audio capture devices. */
static int numAudioCaptureDevices = 0;                 /**< How many capture devices. */

static audioDevices playbackDevices[MAX_AUDIO_DEVICES]; /**< Audio playback devices. */
static int numAudioPlaybackDevices = 0;                 /**< How many playback devices. */

/**
 * @struct audioControl
 * @brief Manages audio capture settings and state for either ALSA or PulseAudio systems.
 *
 * This structure abstracts the specific audio system being used, allowing the rest of the
 * code to interact with audio hardware in a more uniform way. It must be initialized with
 * the appropriate settings for the target audio system before use.
 *
 * @var audioControl::handle
 * Pointer to the ALSA PCM device handle.
 *
 * @var audioControl::frames
 * Number of frames for ALSA to capture in each read operation.
 *
 * @var audioControl::pa_handle
 * Pointer to the PulseAudio simple API handle.
 *
 * @var audioControl::pa_framesize
 * Size of the buffer (in bytes) for PulseAudio to use for each read operation.
 *
 * @var audioControl::full_buff_size
 * Size of the buffer to be filled in each read operation, common to both ALSA and PulseAudio.
 */
typedef struct {
#ifdef ALSA_DEVICE
   snd_pcm_t *handle;
   snd_pcm_uframes_t frames;
#else
   pa_simple *pa_handle;
   size_t pa_framesize;
#endif

   uint32_t full_buff_size;
} audioControl;

#ifndef ALSA_DEVICE
static const pa_sample_spec sample_spec = { .format = DEFAULT_PULSE_FORMAT,
                                            .rate = DEFAULT_RATE,
                                            .channels = DEFAULT_CHANNELS };
#endif

// Audio capture thread context (dedicated thread for continuous audio capture)
static audio_capture_context_t *audio_capture_ctx = NULL;

// Silero VAD context for voice activity detection
static silero_vad_context_t *vad_ctx = NULL;

// backgroundRMS removed - using VAD only
static char *wakeWords[] = { "hello " AI_NAME,    "okay " AI_NAME,         "alright " AI_NAME,
                             "hey " AI_NAME,      "hi " AI_NAME,           "good evening " AI_NAME,
                             "good day " AI_NAME, "good morning " AI_NAME, "yeah " AI_NAME,
                             "k " AI_NAME };

// Array of words/phrases used to signal the end of an interaction with the AI.
static char *goodbyeWords[] = { "good bye", "goodbye", "good night", "bye", "quit", "exit" };

// Array of predefined responses the AI can use upon recognizing a wake word/phrase.
const char *wakeResponses[] = { "Hello Sir.", "At your service Sir.", "Yes Sir?",
                                "How may I assist you Sir?", "Listening Sir." };

// Array of words/phrases that the AI should explicitly ignore during interaction.
// This includes common filler words or phrases signaling to disregard the prior input.
static char *ignoreWords[] = { "", "the", "cancel", "never mind", "nevermind", "ignore" };

// Array of words/phrases that we accept as a way to tell the AI to cancel its current
// text to speech instead of requiring another command.
static char *cancelWords[] = { "stop",      "stop it",        "cancel",        "hold on",
                               "wait",      "never mind",     "abort",         "pause",
                               "enough",    "disregard",      "no thanks",     "forget it",
                               "leave it",  "drop it",        "stand by",      "cease",
                               "interrupt", "say no more",    "shut up",       "silence",
                               "zip it",    "enough already", "that's enough", "stop right there" };

// Standard greeting messages based on the time of day.
const char *morning_greeting = "Good morning boss.";
const char *day_greeting = "Good day Sir.";
const char *evening_greeting = "Good evening Sir.";

/**
 * @enum listeningState
 * Enum representing the possible states of Dawn's listening process.
 *
 * @var SILENCE
 * The AI is not actively listening or processing commands.
 * It's waiting for a noise threshold to be exceeded.
 *
 * @var WAKEWORD_LISTEN
 * The AI is listening for a wake word to initiate interaction.
 *
 * @var COMMAND_RECORDING
 * The AI is recording a command after recognizing a wake word.
 *
 * @var PROCESS_COMMAND
 * The AI is processing a recorded command.
 *
 * @var VISION_AI_READY
 * Indicates that the vision AI component is ready for processing.
 */
typedef enum {
   SILENCE,
   WAKEWORD_LISTEN,
   COMMAND_RECORDING,
   PROCESS_COMMAND,
   VISION_AI_READY,
   NETWORK_PROCESSING,
   INVALID_STATE
} listeningState;

// Define the shared variables for tts state
pthread_cond_t tts_cond = PTHREAD_COND_INITIALIZER;
pthread_mutex_t tts_mutex = PTHREAD_MUTEX_INITIALIZER;
// Note: tts_playback_state is defined in text_to_speech.cpp (mutex-protected, not atomic)

// Define the shared variables for LLM thread state
pthread_mutex_t llm_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_t llm_thread;
volatile int llm_processing = 0;  // Flag: 1 if LLM thread is running, 0 otherwise

/**
 * @var static char *vision_ai_image
 * Pointer to a buffer containing the latest image captured for vision AI processing.
 * Initially set to NULL and should be allocated when an image is captured.
 */
static char *vision_ai_image = NULL;

/**
 * @var static int vision_ai_image_size
 * Size of the buffer pointed to by vision_ai_image, representing the image size in bytes.
 * Initially set to 0 and updated upon capturing an image.
 */
static int vision_ai_image_size = 0;

/**
 * @var static int vision_ai_ready
 * Flag indicating whether the vision AI component is ready for image processing.
 * Set to 1 when ready, 0 otherwise.
 */
static int vision_ai_ready = 0;

/**
 * @var volatile sig_atomic_t quit
 * @brief Global flag indicating the application should quit.
 *
 * This flag is set to 1 when a SIGINT signal is received, signaling the
 * main loop to terminate and allow for a graceful exit. The use of
 * `volatile sig_atomic_t` ensures that the variable is updated atomically
 * and prevents the compiler from applying unwanted optimizations, considering
 * it may be altered asynchronously by signal handling.
 */
volatile sig_atomic_t quit = 0;

/* MQTT */
static struct mosquitto *mosq;

// Is remote DAWN enabled
static int enable_network_audio = 0;
pthread_mutex_t processing_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t processing_done = PTHREAD_COND_INITIALIZER;
uint8_t *processing_result_data = NULL;
size_t processing_result_size = 0;
int processing_complete = 0;

// Network processing state
static listeningState previous_state_before_network = SILENCE;
static uint8_t *network_pcm_buffer = NULL;
static size_t network_pcm_size = 0;
static pthread_mutex_t network_processing_mutex = PTHREAD_MUTEX_INITIALIZER;

// PCM Data Structure for network processing
typedef struct {
   uint8_t *pcm_data;
   size_t pcm_size;
   uint32_t sample_rate;
   uint16_t num_channels;
   uint16_t bits_per_sample;
   int is_valid;
} NetworkPCMData;

// Global variable for command processing mode
command_processing_mode_t command_processing_mode = CMD_MODE_DIRECT_ONLY;
struct json_object *conversation_history = NULL;

// Shared buffers for LLM thread communication (protected by llm_mutex)
static char *llm_request_text = NULL;     // Input: command text for LLM
static char *llm_response_text = NULL;    // Output: LLM response
static char *llm_vision_image = NULL;     // Input: vision image data
static size_t llm_vision_image_size = 0;  // Input: vision image size

/**
 * @brief Callback for streaming LLM responses with TTS
 *
 * This callback is called for each complete sentence from the streaming LLM.
 * It cleans the sentence and sends it directly to TTS for immediate playback.
 */
static void dawn_tts_sentence_callback(const char *sentence, void *userdata) {
   if (!sentence || strlen(sentence) == 0) {
      return;
   }

   // Make a copy for cleaning
   char *cleaned = strdup(sentence);
   if (!cleaned) {
      return;
   }

   // Remove command tags (they'll be processed from the full response later)
   char *cmd_start, *cmd_end;
   while ((cmd_start = strstr(cleaned, "<command>")) != NULL) {
      cmd_end = strstr(cmd_start, "</command>");
      if (cmd_end) {
         cmd_end += strlen("</command>");
         memmove(cmd_start, cmd_end, strlen(cmd_end) + 1);
      } else {
         // Incomplete command tag - strip from <command> to end of string
         // This handles cases where the stream breaks between tags
         *cmd_start = '\0';
         break;
      }
   }

   // Remove <end_of_turn> tags (local AI models)
   char *match = NULL;
   if ((match = strstr(cleaned, "<end_of_turn>")) != NULL) {
      *match = '\0';
   }

   // Remove special characters that cause problems
   remove_chars(cleaned, "*");
   remove_emojis(cleaned);

   // Trim whitespace
   size_t len = strlen(cleaned);
   while (len > 0 && (cleaned[len - 1] == ' ' || cleaned[len - 1] == '\t' ||
                      cleaned[len - 1] == '\n' || cleaned[len - 1] == '\r')) {
      cleaned[--len] = '\0';
   }

   // Only send to TTS if there's actual content
   if (len > 0) {
      text_to_speech(cleaned);

      // Add natural pause between sentences (300ms)
      usleep(300000);
   }

   free(cleaned);
}

sig_atomic_t get_quit(void) {
   return quit;
}

#if 0
// Define the function to draw the waveform using SDL
void drawWaveform(const int16_t *audioBuffer, size_t numSamples) {
    // Clear the screen
    SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
    SDL_RenderClear(ren);

    // Define waveform colors (e.g., green lines)
    SDL_SetRenderDrawColor(ren, 0, 255, 0, 255);

    // Determine the dimensions and position of the waveform within the window
    int waveformPosX = 0; // Adjust as needed
    int waveformPosY = 50; // Adjust as needed
    int waveformWidth = 0; // Adjust as needed
    int waveformHeight = 0; // Adjust as needed
    SDL_GetWindowSize(win, &waveformWidth, &waveformHeight);
    waveformPosY = waveformPosY + (waveformHeight - waveformPosY*2) / 2;

    // Calculate the step size to sample audio data and draw the waveform
    int stepSize = numSamples / waveformWidth;
    if (stepSize <= 0) {
        stepSize = 1;
    }

    // Scale factor for the waveform's height
    float scaleFactor = (float)waveformHeight / INT16_MAX;

    // Draw the waveform based on the audio buffer data
    int prevX = waveformPosX;
    int prevY = waveformPosY + (int)(audioBuffer[0] * scaleFactor);
    for (size_t i = 1; i < numSamples; i += stepSize) {
        int x = waveformPosX + waveformWidth * i / numSamples;
        int y = waveformPosY + (int)(audioBuffer[i] * scaleFactor);

        SDL_RenderDrawLine(ren, prevX, prevY, x, y);

        prevX = x;
        prevY = y;
    }

    // Update the screen
    SDL_RenderPresent(ren);
}
#endif

/**
 * @fn void signal_handler(int signal)
 * @brief Signal handler for SIGINT.
 *
 * This function is designed to handle the SIGINT signal (typically generated
 * by pressing Ctrl+C). When the signal is received, it sets the global
 * `quit` flag to 1, indicating to the application that it should exit.
 *
 * @param signal The signal number received, expected to be SIGINT.
 */
void signal_handler(int signal) {
   if (signal == SIGINT) {
      // Request LLM interrupt (safe to call from signal handler - uses sig_atomic_t)
      llm_request_interrupt();
      quit = 1;
   }
}

char *textToSpeechCallback(const char *actionName, char *value, int *should_respond) {
   LOG_INFO("Received text to speech command: \"%s\"\n", value);

   // TTS commands always execute directly, regardless of mode
   if (should_respond != NULL) {
      *should_respond = 0;  // No need to respond about TTS
   }
   text_to_speech(value);
   return NULL;
}

const char *getPcmPlaybackDevice(void) {
   return (const char *)pcm_playback_device;
}

const char *getPcmCaptureDevice(void) {
   return (const char *)pcm_capture_device;
}

char *findAudioPlaybackDevice(char *name) {
   int i = 0;
   char speech[MAX_COMMAND_LENGTH];

   for (i = 0; i < numAudioPlaybackDevices; i++) {
      if (strcmp(playbackDevices[i].name, name) == 0) {
         return playbackDevices[i].device;
      }
   }

   return NULL;
}

char *setPcmPlaybackDevice(const char *actionName, char *value, int *should_respond) {
   int i = 0;
   char speech[MAX_COMMAND_LENGTH];
   static char return_buffer[MAX_COMMAND_LENGTH];

   *should_respond = 1;  // We always respond for device changes

   for (i = 0; i < numAudioPlaybackDevices; i++) {
      if (strcmp(playbackDevices[i].name, value) == 0) {
         LOG_INFO("Setting audio playback device to \"%s\".\n", playbackDevices[i].device);
         strncpy(pcm_playback_device, playbackDevices[i].device, MAX_WORD_LENGTH);
         pcm_playback_device[MAX_WORD_LENGTH] = '\0';  // Ensure null termination

         if (command_processing_mode == CMD_MODE_DIRECT_ONLY) {
            snprintf(speech, MAX_COMMAND_LENGTH, "Switching playback device to %s.", value);
            text_to_speech(speech);
            return NULL;  // Already handled
         } else {
            // AI modes: return the result for AI to process
            snprintf(return_buffer, MAX_COMMAND_LENGTH, "Audio playback device switched to %s",
                     value);
            return return_buffer;
         }
      }
   }

   if (i >= numAudioPlaybackDevices) {
      LOG_ERROR("Requested audio playback device not found.\n");

      if (command_processing_mode == CMD_MODE_DIRECT_ONLY) {
         snprintf(speech, MAX_COMMAND_LENGTH,
                  "Sorry sir. A playback device called %s was not found.", value);
         text_to_speech(speech);
         return NULL;
      } else {
         snprintf(return_buffer, MAX_COMMAND_LENGTH, "Audio playback device '%s' not found", value);
         return return_buffer;
      }
   }

   return NULL;  // Should never reach here
}

char *setPcmCaptureDevice(const char *actionName, char *value, int *should_respond) {
   int i = 0;
   char speech[MAX_COMMAND_LENGTH];
   static char return_buffer[MAX_COMMAND_LENGTH];

   *should_respond = 1;

   for (i = 0; i < numAudioCaptureDevices; i++) {
      if (strcmp(captureDevices[i].name, value) == 0) {
         LOG_INFO("Setting audio capture device to \"%s\".\n", captureDevices[i].device);
         strncpy(pcm_capture_device, captureDevices[i].device, MAX_WORD_LENGTH);
         pcm_capture_device[MAX_WORD_LENGTH] = '\0';  // Ensure null termination

         if (command_processing_mode == CMD_MODE_DIRECT_ONLY) {
            snprintf(speech, MAX_COMMAND_LENGTH, "Switching capture device to %s.", value);
            text_to_speech(speech);
            return NULL;
         } else {
            snprintf(return_buffer, MAX_COMMAND_LENGTH, "Audio capture device switched to %s",
                     value);
            return return_buffer;
         }
      }
   }

   if (i >= numAudioCaptureDevices) {
      LOG_ERROR("Requested audio capture device not found.\n");

      if (command_processing_mode == CMD_MODE_DIRECT_ONLY) {
         snprintf(speech, MAX_COMMAND_LENGTH,
                  "Sorry sir. A capture device called %s was not found.", value);
         text_to_speech(speech);
         return NULL;
      } else {
         snprintf(return_buffer, MAX_COMMAND_LENGTH, "Audio capture device '%s' not found", value);
         return return_buffer;
      }
   }

   return NULL;
}

/**
 * Measures the RMS value of background audio for a predefined duration.
 * This function supports both ALSA and PulseAudio backends, determined at compile time.
 * It captures audio into a buffer, computes the RMS value, and stores it in a global variable.
 *
 * Note: This function is designed to be run in a separate thread, not required, taking a pointer
 * to an audioControl structure as its argument. This structure must be properly initialized
 * before calling this function.
 *
 * @param audHandle A void pointer to an audioControl structure containing audio capture settings.
 * @return NULL always, indicating the thread's work is complete.
 */
void *measureBackgroundAudio(void *audHandle) {
   (void)audHandle;  // Unused - kept for API compatibility

   // Use the dedicated capture thread instead of direct device access
   if (!audio_capture_ctx) {
      LOG_ERROR("Audio capture thread not initialized for background measurement\n");
      return NULL;
   }

   // Calculate buffer size for background audio measurement
   uint32_t max_buff_size = DEFAULT_RATE * DEFAULT_CHANNELS * sizeof(int16_t) *
                            BACKGROUND_CAPTURE_SECONDS;

   char *max_buff = (char *)malloc(max_buff_size);
   if (max_buff == NULL) {
      LOG_ERROR("malloc() failed on max_buff.\n");
      return NULL;
   }

   // Background RMS measurement removed - VAD handles all voice detection now
   // This eliminates the 6-second startup delay
   LOG_INFO("Using Silero VAD for voice activity detection (no RMS calibration needed)");

   free(max_buff);
   return NULL;
}

/**
 * Normalize text for wake word/command matching
 * Converts to lowercase and removes punctuation/special characters
 *
 * @param input Original text
 * @return Normalized text (caller must free), or NULL on error
 */
static char *normalize_for_matching(const char *input) {
   if (!input) {
      return NULL;
   }

   size_t len = strlen(input);
   char *normalized = (char *)malloc(len + 1);
   if (!normalized) {
      return NULL;
   }

   size_t j = 0;
   for (size_t i = 0; i < len; i++) {
      char c = input[i];
      // Convert to lowercase
      if (c >= 'A' && c <= 'Z') {
         normalized[j++] = c + 32;
      }
      // Keep lowercase letters, digits, and spaces
      else if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == ' ') {
         normalized[j++] = c;
      }
      // Skip punctuation and other characters
   }
   normalized[j] = '\0';

   return normalized;
}

/**
 * Parses a JSON string to extract the value of the "text" field.
 *
 * @param input A JSON string expected to contain a "text" field.
 * @return A dynamically allocated string containing the value of the "text" field.
 *         The caller is responsible for freeing this string.
 *         Returns NULL on error, including JSON parsing errors, missing "text" field,
 *         or memory allocation failures.
 */
char *getTextResponse(const char *input) {
   struct json_object *parsed_json;
   struct json_object *text_object;
   char *return_text = NULL;

   // Parse the JSON data
   parsed_json = json_tokener_parse(input);
   if (parsed_json == NULL) {
      LOG_ERROR("Error: Unable to process text response.\n");
      return NULL;
   }

   // Get the "text" object from the JSON
   if (json_object_object_get_ex(parsed_json, "text", &text_object)) {
      const char *input_text = json_object_get_string(text_object);
      if (input_text == NULL) {
         LOG_ERROR("Error: Unable to get string from input text.\n");
         json_object_put(parsed_json);
         return NULL;
      }

      return_text = malloc((strlen(input_text) + 1) * sizeof(char));
      if (return_text == NULL) {
         LOG_ERROR("malloc() failed in getTextResponse().\n");
         json_object_put(parsed_json);
         return NULL;
      }

      // Directly copy the input text into the return buffer
      strcpy(return_text, input_text);

      // Debugging: Print the extracted text
      LOG_INFO("Input Text: %s\n", return_text);
   } else {
      LOG_ERROR("Error: 'text' field not found in JSON.\n");
   }

   // Cleanup and return
   json_object_put(parsed_json);
   return return_text;
}

#ifdef ALSA_DEVICE
/**
 * Opens an ALSA PCM capture device and configures it with default hardware parameters.
 *
 * This function initializes an ALSA PCM capture device using specified settings for audio capture.
 * It sets parameters such as the audio format, rate, channels, and period size to defaults defined
 * elsewhere.
 *
 * @param handle Pointer to a snd_pcm_t pointer where the opened PCM device handle will be stored.
 * @param pcm_device String name of the PCM device to open (e.g., "default" or a specific hardware
 * device).
 * @param frames Pointer to a snd_pcm_uframes_t variable where the period size in frames will be
 * stored.
 *
 * @return 0 on success, 1 on error, with an error message printed to stderr.
 */
int openAlsaPcmCaptureDevice(snd_pcm_t **handle, char *pcm_device, snd_pcm_uframes_t *frames) {
   snd_pcm_hw_params_t *params = NULL;
   unsigned int rate = DEFAULT_RATE;
   int dir = 0;
   *frames = DEFAULT_FRAMES;
   int rc = 0;

   LOG_INFO("ALSA CAPTURE DRIVER\n");

   /* Open PCM device for playback. */
   rc = snd_pcm_open(handle, pcm_device, SND_PCM_STREAM_CAPTURE, 0);
   if (rc < 0) {
      LOG_ERROR("Unable to open pcm device for capture (%s): %s\n", pcm_device, snd_strerror(rc));
      return 1;
   }

   snd_pcm_hw_params_alloca(&params);

   rc = snd_pcm_hw_params_any(*handle, params);
   if (rc < 0) {
      LOG_ERROR("Unable to get hardware parameter structure: %s\n", snd_strerror(rc));
      snd_pcm_close(*handle);
      *handle = NULL;
      return 1;
   }

   rc = snd_pcm_hw_params_set_access(*handle, params, DEFAULT_ACCESS);
   if (rc < 0) {
      LOG_ERROR("Unable to set access type: %s\n", snd_strerror(rc));
      snd_pcm_close(*handle);
      *handle = NULL;
      return 1;
   }

   rc = snd_pcm_hw_params_set_format(*handle, params, DEFAULT_FORMAT);
   if (rc < 0) {
      LOG_ERROR("Unable to set sample format: %s\n", snd_strerror(rc));
      snd_pcm_close(*handle);
      *handle = NULL;
      return 1;
   }

   rc = snd_pcm_hw_params_set_channels(*handle, params, DEFAULT_CHANNELS);
   if (rc < 0) {
      LOG_ERROR("Unable to set channel count: %s\n", snd_strerror(rc));
      snd_pcm_close(*handle);
      *handle = NULL;
      return 1;
   }

   rc = snd_pcm_hw_params_set_rate_near(*handle, params, &rate, &dir);
   if (rc < 0) {
      LOG_ERROR("Unable to set sample rate: %s\n", snd_strerror(rc));
      snd_pcm_close(*handle);
      *handle = NULL;
      return 1;
   }
   LOG_INFO("Capture rate set to %u\n", rate);

   rc = snd_pcm_hw_params_set_period_size_near(*handle, params, frames, &dir);
   if (rc < 0) {
      LOG_ERROR("Unable to set period size: %s\n", snd_strerror(rc));
      snd_pcm_close(*handle);
      *handle = NULL;
      return 1;
   }
   LOG_INFO("Frames set to %lu\n", *frames);

   rc = snd_pcm_hw_params(*handle, params);
   if (rc < 0) {
      LOG_ERROR("Unable to set hw parameters: %s\n", snd_strerror(rc));
      snd_pcm_close(*handle);
      *handle = NULL;
      return 1;
   }

   return 0;
}
#else
/**
 * Opens a PulseAudio capture stream for a given PCM device.
 *
 * This function initializes a PulseAudio capture stream, using the PulseAudio Simple API,
 * for audio recording. It requires specifying the PCM device and uses predefined sample
 * specifications and application name defined elsewhere in the code.
 *
 * @param pcm_capture_device String name of the PCM capture device or NULL for the default device.
 *
 * @return A pointer to the initialized pa_simple structure representing the capture stream, or NULL
 * on error, with an error message printed to stderr.
 */
pa_simple *openPulseaudioCaptureDevice(char *pcm_capture_device) {
   pa_simple *pa_handle = NULL;
   int rc = 0;

   LOG_INFO("PULSEAUDIO CAPTURE DRIVER: %s\n", pcm_capture_device);

   /* Create a new capture stream */
   if (!(pa_handle = pa_simple_new(NULL, APPLICATION_NAME, PA_STREAM_RECORD, pcm_capture_device,
                                   "record", &sample_spec, NULL, NULL, &rc))) {
      LOG_ERROR("Error opening PulseAudio record: %s\n", pa_strerror(rc));
      return NULL;
   }

   LOG_INFO("Capture opened successfully.\n");

   return pa_handle;
}
#endif

/**
 * Generates a greeting message based on the current time of day.
 *
 * This function checks the system's local time and selects an appropriate greeting
 * for morning, day, or evening. It uses predefined global strings for each time-specific
 * greeting, ensuring that the greeting is contextually relevant.
 *
 * @return A pointer to a constant character string containing the selected greeting.
 *         The return value points to a global variable and should not be modified or freed.
 */
const char *timeOfDayGreeting(void) {
   time_t t = time(NULL);
   struct tm *local_time = localtime(&t);  // Obtain local time from the system clock.

   int hour = local_time->tm_hour;  // Extract the hour component of the current time.

   // Select the appropriate greeting based on the hour of the day.
   if (hour >= 3 && hour < 12) {
      return morning_greeting;  // Morning greeting from 3 AM to before 12 PM.
   } else if (hour >= 12 && hour < 18) {
      return day_greeting;  // Day greeting from 12 PM to before 6 PM.
   } else {
      return evening_greeting;  // Evening greeting for 6 PM onwards.
   }
}

/**
 * Selects a random acknowledgment response to a wake word detection.
 *
 * This function is designed to provide variability in the AI's response to
 * being activated by a wake word. It randomly selects one of the predefined
 * responses from the global `wakeResponses` array each time it's called.
 *
 * @return A pointer to a constant character string containing the selected wake word
 * acknowledgment. The return value points to an element within the global `wakeResponses` array and
 *         should not be modified or freed.
 */
const char *wakeWordAcknowledgment() {
   int numWakeResponses = sizeof(wakeResponses) /
                          sizeof(wakeResponses[0]);  // Calculate the number of available responses.
   int choice;

   srand(time(NULL));                   // Seed the random number generator.
   choice = rand() % numWakeResponses;  // Generate a random index to select a response.

   return wakeResponses[choice];  // Return the randomly selected wake word acknowledgment.
}

/**
 * Captures audio into a buffer until it is full or an error occurs, managing its own local buffer.
 *
 * This function abstracts the audio capture process to work with either ALSA or PulseAudio,
 * depending on compilation flags. It allocates a local buffer for audio data capture,
 * copies captured data into a larger buffer provided by the caller, and ensures
 * that the larger buffer does not overflow. The local buffer is dynamically allocated
 * and managed within the function.
 *
 * @param myAudioControls A pointer to an audioControl structure containing audio capture settings
 * and state.
 * @param max_buff A pointer to the buffer where captured audio data will be accumulated.
 * @param max_buff_size The maximum size of max_buff, indicating how much data can be stored.
 * @param ret_buff_size A pointer to an integer where the function will store the total size of
 * captured audio data stored in max_buff upon successful completion.
 * @return Returns 0 on successful capture of audio data without filling the buffer.
 *         Returns 1 if a memory allocation failure occurs or an error occurred during audio
 * capture, such as a failure to read from the audio device.
 *
 * @note The function updates *ret_buff_size with the total size of captured audio data.
 *       It's crucial to ensure that max_buff is large enough to hold the expected amount of data.
 */
/**
 * @brief Capture audio from the ring buffer filled by the capture thread
 *
 * This function reads audio data from the ring buffer that is continuously
 * filled by the dedicated audio capture thread. This replaces the old
 * direct device reading approach with a non-blocking ring buffer read.
 *
 * @param myAudioControls Audio control structure (unused, kept for API compatibility)
 * @param max_buff Destination buffer for audio data
 * @param max_buff_size Maximum size of destination buffer
 * @param ret_buff_size Pointer to store actual bytes read
 * @return 0 on success, 1 on error
 */
int capture_buffer(audioControl *myAudioControls,
                   char *max_buff,
                   uint32_t max_buff_size,
                   int *ret_buff_size) {
   (void)myAudioControls;  // Unused parameter (kept for API compatibility)

   *ret_buff_size = 0;

   if (!audio_capture_ctx) {
      LOG_ERROR("Audio capture thread not initialized");
      return 1;
   }

   // Check if capture thread is still running
   if (!audio_capture_is_running(audio_capture_ctx)) {
      LOG_ERROR("Audio capture thread has stopped unexpectedly");
      return 1;
   }

   // Wait for enough data to be available in ring buffer (at least max_buff_size)
   // Timeout of 2 seconds to prevent indefinite blocking
   size_t available = audio_capture_wait_for_data(audio_capture_ctx, max_buff_size, 2000);
   if (available < max_buff_size) {
      LOG_WARNING("Timeout waiting for audio data (got %zu bytes, needed %u)", available,
                  max_buff_size);
      // Read whatever is available
      if (available > 0) {
         *ret_buff_size = audio_capture_read(audio_capture_ctx, max_buff, available);
      }
      return 0;  // Not a fatal error, return partial data
   }

   // Read the requested amount of data from ring buffer
   *ret_buff_size = audio_capture_read(audio_capture_ctx, max_buff, max_buff_size);

   return 0;  // Success
}

void process_vision_ai(const char *base64_image, size_t image_size) {
   if (vision_ai_image != NULL) {
      free(vision_ai_image);
      vision_ai_image = NULL;
   }

   vision_ai_image = malloc(image_size);
   if (!vision_ai_image) {
      LOG_ERROR("Error: Memory allocation failed.\n");
      return;
   }
   memcpy(vision_ai_image, base64_image, image_size);

   vision_ai_image_size = image_size;
   vision_ai_ready = 1;
}

listeningState currentState = INVALID_STATE;

/**
 * @brief Saves the conversation history JSON to a timestamped file.
 *
 * This function creates a filename with the current date and time, then writes
 * the entire conversation history JSON object to that file for later analysis
 * or debugging purposes.
 *
 * @param conversation_history The JSON object containing the chat conversation
 * @return 0 on success, -1 on failure
 */
int save_conversation_history(struct json_object *conversation_history) {
   FILE *chat_file = NULL;
   time_t current_time;
   struct tm *time_info;
   char filename[256];
   const char *json_string = NULL;

   if (conversation_history == NULL) {
      LOG_ERROR("Cannot save NULL conversation history");
      return -1;
   }

   // Get current time for filename
   time(&current_time);
   time_info = localtime(&current_time);

   // Create timestamped filename: chat_history_YYYYMMDD_HHMMSS.json
   strftime(filename, sizeof(filename), "chat_history_%Y%m%d_%H%M%S.json", time_info);

   // Open file for writing
   chat_file = fopen(filename, "w");
   if (chat_file == NULL) {
      LOG_ERROR("Failed to open chat history file: %s", filename);
      return -1;
   }

   // Convert JSON object to pretty-printed string
   json_string = json_object_to_json_string_ext(
       conversation_history, JSON_C_TO_STRING_PRETTY | JSON_C_TO_STRING_NOSLASHESCAPE);
   if (json_string == NULL) {
      LOG_ERROR("Failed to convert conversation history to JSON string");
      fclose(chat_file);
      return -1;
   }

   // Write JSON string to file
   if (fprintf(chat_file, "%s\n", json_string) < 0) {
      LOG_ERROR("Failed to write conversation history to file: %s", filename);
      fclose(chat_file);
      return -1;
   }

   fclose(chat_file);
   LOG_INFO("Conversation history saved to: %s", filename);

   return 0;
}

/**
 * @brief Reset all subsystems for a new utterance
 *
 * Resets VAD, ASR, chunking manager, duration tracking, and pre-roll buffer.
 * Call this when transitioning back to WAKEWORD_LISTEN or SILENCE state
 * to ensure clean state for the next interaction.
 *
 * @param vad_ctx Silero VAD context to reset (can be NULL)
 * @param asr_ctx ASR context to reset (must not be NULL)
 * @param chunk_mgr Chunking manager to reset (can be NULL if Vosk mode)
 * @param silence_duration Pointer to silence duration to reset
 * @param speech_duration Pointer to speech duration to reset
 * @param recording_duration Pointer to recording duration to reset
 * @param preroll_write_pos Pointer to pre-roll write position to reset
 * @param preroll_valid_bytes Pointer to pre-roll valid bytes to reset
 */
static void reset_for_new_utterance(silero_vad_context_t *vad_ctx,
                                    asr_context_t *asr_ctx,
                                    chunking_manager_t *chunk_mgr,
                                    float *silence_duration,
                                    float *speech_duration,
                                    float *recording_duration,
                                    size_t *preroll_write_pos,
                                    size_t *preroll_valid_bytes) {
   // Reset VAD state for new interaction boundary
   if (vad_ctx) {
      vad_silero_reset(vad_ctx);
   }

   // Reset chunking manager (Whisper only)
   if (chunk_mgr) {
      chunking_manager_reset(chunk_mgr);
   }

   // Reset ASR for next utterance
   asr_reset(asr_ctx);

   // Reset duration tracking
   *silence_duration = 0.0f;
   *speech_duration = 0.0f;
   *recording_duration = 0.0f;

   // Reset pre-roll buffer
   *preroll_write_pos = 0;
   *preroll_valid_bytes = 0;
}

/* Publish AI State. Only send state if it's changed.
 *
 * FIXME: Build this JSON correctly.
 *        Also pick a better topic for general purpose use.
 */
int publish_ai_state(listeningState newState) {
   const char stateTemplate[] = "{\"device\": \"ai\", \"name\":\"%s\", \"state\":\"%s\"}";
   char state[20] = "";
   char *aiState = NULL;
   int aiStateLength = 0;
   int rc = 0;

   if (newState == currentState || newState == INVALID_STATE) {
      return 0;
   }

   switch (newState) {
      case SILENCE:
         strcpy(state, "SILENCE");
         break;
      case WAKEWORD_LISTEN:
         strcpy(state, "WAKEWORD_LISTEN");
         break;
      case COMMAND_RECORDING:
         strcpy(state, "COMMAND_RECORDING");
         break;
      case PROCESS_COMMAND:
         strcpy(state, "PROCESS_COMMAND");
         break;
      case VISION_AI_READY:
         strcpy(state, "VISION_AI_READY");
         break;
      case NETWORK_PROCESSING:
         strcpy(state, "NETWORK_PROCESSING");
         break;
      default:
         LOG_ERROR("Unknown state: %d", newState);
         return 1;
   }

   /* "- 4" is from substracting the two %s but adding the term char */
   aiStateLength = strlen(stateTemplate) + strlen(AI_NAME) + strlen(state) - 4 + 1;
   aiState = malloc(aiStateLength);
   if (aiState == NULL) {
      LOG_ERROR("Error allocating memory for AI state.");
      return 1;
   }

   rc = snprintf(aiState, aiStateLength, stateTemplate, AI_NAME, state);
   if (rc < 0 || rc >= aiStateLength) {
      LOG_ERROR("Error creating AI state message.");
      free(aiState);
      return 1;
   }

   rc = mosquitto_publish(mosq, NULL, "hud", strlen(aiState), aiState, 0, false);
   if (rc != MOSQ_ERR_SUCCESS) {
      LOG_ERROR("Error publishing: %s\n", mosquitto_strerror(rc));
      free(aiState);
      return 1;
   }

   free(aiState);

   currentState = newState;  // Update the state after successful publish

   return 0;
}

/**
 * @brief Extract PCM audio data from a WAV file received over the network
 *
 * This function parses a WAV file buffer, validates the header format, and extracts
 * the raw PCM audio data for processing by Vosk speech recognition. The function
 * performs basic validation to ensure the WAV format is compatible with the DAWN
 * audio pipeline (16-bit mono PCM).
 *
 * @param wav_data Pointer to buffer containing complete WAV file data
 * @param wav_size Total size of WAV data buffer in bytes
 *
 * @return NetworkPCMData* Pointer to allocated structure containing extracted PCM data
 *                         and format information, or NULL on error
 * @retval NULL if:
 *         - wav_size is smaller than WAV header (44 bytes)
 *         - RIFF/WAVE header validation fails
 *         - Memory allocation fails
 *
 * @note Caller is responsible for freeing returned structure using free_network_pcm_data()
 * @note The function uses le32toh/le16toh for endian conversion (assumes little-endian WAV)
 * @note is_valid flag is set only if format is mono 16-bit (compatible with pipeline)
 *
 * @warning Does not validate data_bytes field against actual buffer size - potential
 *          buffer overflow vulnerability
 *
 * @see free_network_pcm_data() for proper cleanup
 * @see WAVHeader structure definition in dawn_tts_wrapper.h
 */
NetworkPCMData *extract_pcm_from_network_wav(const uint8_t *wav_data, size_t wav_size) {
   // Validate parameters
   if (!wav_data || wav_size == 0) {
      LOG_ERROR("Invalid parameters: wav_data=%p, wav_size=%zu", (void *)wav_data, wav_size);
      return NULL;
   }

   // Validate minimum size
   if (wav_size < sizeof(WAVHeader)) {
      LOG_ERROR("WAV data too small for header: %zu bytes (need %zu)", wav_size, sizeof(WAVHeader));
      return NULL;
   }

   const WAVHeader *header = (const WAVHeader *)wav_data;

   // Validate RIFF/WAVE headers
   if (strncmp(header->riff_header, "RIFF", 4) != 0 ||
       strncmp(header->wave_header, "WAVE", 4) != 0) {
      LOG_ERROR("Invalid WAV header format");
      return NULL;
   }

   // Extract format information (little-endian)
   uint32_t sample_rate = le32toh(header->sample_rate);
   uint16_t num_channels = le16toh(header->num_channels);
   uint16_t bits_per_sample = le16toh(header->bits_per_sample);
   uint16_t audio_format = le16toh(header->audio_format);
   uint32_t data_bytes = le32toh(header->data_bytes);

   // Validate audio format (must be PCM)
   if (audio_format != 1) {
      LOG_ERROR("Not PCM format: %u", audio_format);
      return NULL;
   }

   // Validate data_bytes against actual buffer size
   size_t expected_total_size = sizeof(WAVHeader) + data_bytes;
   if (expected_total_size > wav_size) {
      LOG_WARNING("WAV header claims %u data bytes, but only %zu available", data_bytes,
                  wav_size - sizeof(WAVHeader));
      data_bytes = wav_size - sizeof(WAVHeader);
   }

   // Sanity check for unreasonably large data
   if (data_bytes > ESP32_MAX_RESPONSE_BYTES) {
      LOG_ERROR("WAV data size unreasonably large: %u bytes (max: %ld)", data_bytes,
                (long)ESP32_MAX_RESPONSE_BYTES);
      return NULL;
   }

   LOG_INFO("WAV format: %uHz, %u channels, %u-bit, %u data bytes", sample_rate, num_channels,
            bits_per_sample, data_bytes);

   // Allocate PCM structure
   NetworkPCMData *pcm = malloc(sizeof(NetworkPCMData));
   if (!pcm) {
      LOG_ERROR("Failed to allocate NetworkPCMData structure");
      return NULL;
   }

   // Allocate PCM data buffer
   pcm->pcm_data = malloc(data_bytes);
   if (!pcm->pcm_data) {
      LOG_ERROR("Failed to allocate %u bytes for PCM data", data_bytes);
      free(pcm);
      return NULL;
   }

   // Copy PCM data (skip WAV header)
   memcpy(pcm->pcm_data, wav_data + sizeof(WAVHeader), data_bytes);

   // Populate structure
   pcm->pcm_size = data_bytes;
   pcm->sample_rate = sample_rate;
   pcm->num_channels = num_channels;
   pcm->bits_per_sample = bits_per_sample;
   pcm->is_valid = (num_channels == 1 && bits_per_sample == 16);

   if (!pcm->is_valid) {
      LOG_WARNING("WAV format not pipeline-compatible (need mono 16-bit)");
   }

   return pcm;
}

/**
 * @brief Free memory allocated for NetworkPCMData structure
 *
 * This function safely deallocates a NetworkPCMData structure and its associated
 * PCM data buffer that was allocated by extract_pcm_from_network_wav(). The function
 * performs NULL checks before freeing to prevent crashes.
 *
 * @param pcm Pointer to NetworkPCMData structure to free (may be NULL)
 *
 * @note This function is safe to call with NULL pointer (no-op)
 * @note The pcm pointer becomes invalid after this call - do not use after freeing
 * @note This function does NOT modify any global state or mutexes
 *
 * @warning Calling this function twice on the same pointer causes undefined behavior
 *          (double-free). Caller must set pointer to NULL after freeing.
 *
 * @see extract_pcm_from_network_wav() for structure allocation
 *
 * @par Example Usage:
 * @code
 *   NetworkPCMData *pcm = extract_pcm_from_network_wav(wav_data, wav_size);
 *   if (pcm) {
 *      // Use pcm...
 *      free_network_pcm_data(pcm);
 *      pcm = NULL;  // Good practice to prevent double-free
 *   }
 * @endcode
 */
void free_network_pcm_data(NetworkPCMData *pcm) {
   if (!pcm)
      return;
   if (pcm->pcm_data)
      free(pcm->pcm_data);
   free(pcm);
}

/**
 * Displays help information for the program, outlining the usage and available command-line
 * options. The function dynamically adjusts the usage message based on whether the program name is
 * available from the command-line arguments.
 *
 * @param argc The number of command-line arguments passed to the program.
 * @param argv The array of command-line arguments. argv[0] is expected to contain the program name.
 */
void display_help(int argc, char *argv[]) {
   if (argc > 0) {
      printf("Usage: %s [options]\n\n", argv[0]);
   } else {
      printf("Usage: [options]\n\n");
   }

   // Print the list of available command-line options.
   printf("Options:\n");
   printf("  -c, --capture DEVICE   Specify the PCM capture device.\n");
   printf("  -d, --playback DEVICE  Specify the PCM playback device.\n");
   printf("  -l, --logfile LOGFILE  Specify the log filename instead of stdout/stderr.\n");
   printf("  -N, --network-audio    Enable network audio processing server\n");
   printf("  -h, --help             Display this help message and exit.\n");
   printf("  -m, --llm TYPE         Set default LLM type (cloud or local).\n");
   printf("  -P, --cloud-provider PROVIDER    Set cloud provider (openai or claude).\n");
   printf(
       "  -A, --asr-engine ENGINE          Set ASR engine (vosk or whisper, default: whisper).\n");
   printf("  -W, --whisper-model MODEL        Whisper model size (tiny, base, small, default: "
          "base).\n");
   printf("  -w, --whisper-path PATH          Path to Whisper models directory (default: "
          "../whisper.cpp/models).\n");
   printf("  -M, --music-dir PATH             Absolute path to music directory (default: "
          "~/Music).\n");
   printf("Command Processing Modes:\n");
   printf("  -D, --commands-only    Direct command processing only (default).\n");
   printf("  -C, --llm-commands     Try direct commands first, then LLM if no match.\n");
   printf("  -L, --llm-only         LLM handles all commands, skip direct processing.\n");
}

/**
 * @brief LLM worker thread function
 *
 * This function runs in a separate thread to process LLM requests without blocking
 * the main audio processing loop. It reads the request data from shared buffers,
 * calls the LLM API, stores the response, and signals completion.
 *
 * @param arg Unused (required for pthread compatibility)
 * @return NULL
 */
void *llm_worker_thread(void *arg) {
   (void)arg;  // Unused

   // Lock mutex to access shared data
   pthread_mutex_lock(&llm_mutex);

   // Copy request data to local variables (minimize time holding mutex)
   char *request_text = llm_request_text ? strdup(llm_request_text) : NULL;
   char *vision_image = llm_vision_image ? malloc(llm_vision_image_size) : NULL;
   size_t vision_image_size = llm_vision_image_size;

   if (vision_image && llm_vision_image) {
      memcpy(vision_image, llm_vision_image, llm_vision_image_size);
   }

   pthread_mutex_unlock(&llm_mutex);

   // Clear interrupt flag before calling LLM
   llm_clear_interrupt();

   // Call LLM (this can take 10+ seconds and blocks this thread, not main thread)
   // Note: conversation_history is a global, but only main thread modifies it during setup
   // and we only read it here, so no mutex needed for conversation_history itself
   char *response = llm_chat_completion_streaming_tts(conversation_history, request_text,
                                                      vision_image, vision_image_size,
                                                      dawn_tts_sentence_callback, NULL);

   // Free local copies
   if (request_text) {
      free(request_text);
   }
   if (vision_image) {
      free(vision_image);
   }

   // Lock mutex to store response
   pthread_mutex_lock(&llm_mutex);

   // Store response (or NULL if interrupted/failed)
   if (llm_response_text) {
      free(llm_response_text);
   }
   llm_response_text = response;  // Transfer ownership

   // Mark processing complete
   llm_processing = 0;


   pthread_mutex_unlock(&llm_mutex);

   LOG_INFO("LLM worker thread finished");
   return NULL;
}

int main(int argc, char *argv[]) {
   char *input_text = NULL;
   char *command_text = NULL;
   char *response_text = NULL;
   asr_result_t *asr_result = NULL;
   size_t prev_text_length = 0;
   int text_nochange = 0;
   struct json_object *system_message = NULL;
   int rc = 0;
   int opt = 0;
   const char *log_filename = NULL;

#ifndef ALSA_DEVICE
   // Define the Pulse parameters
   int error = 0;
#endif

   // Audio Buffer
   uint32_t buff_size = 0;
   float temp_buff_size = DEFAULT_RATE * DEFAULT_CHANNELS * sizeof(int16_t) *
                          DEFAULT_CAPTURE_SECONDS;
   uint32_t max_buff_size = (uint32_t)ceil(temp_buff_size);
   char *max_buff = NULL;

   // Command Configuration
   FILE *configFile = NULL;
   char buffer[10 * 1024];
   int bytes_read = 0;

   // Command Parsing
   actionType actions[MAX_ACTIONS]; /**< All of the available actions read from the JSON. */
   int numActions = 0;              /**< Total actions in the actions array. */

   commandSearchElement commands[MAX_COMMANDS];
   int numCommands = 0;

   char *next_char_ptr = NULL;

   audioControl myAudioControls;
   pthread_t backgroundAudioDetect;

   int commandTimeout = 0;

   /* Array Counts */
   int numGoodbyeWords = sizeof(goodbyeWords) / sizeof(goodbyeWords[0]);
   int numWakeWords = sizeof(wakeWords) / sizeof(wakeWords[0]);
   int numIgnoreWords = sizeof(ignoreWords) / sizeof(ignoreWords[0]);
   int numCancelWords = sizeof(cancelWords) / sizeof(cancelWords[0]);

   int i = 0;

   listeningState recState = SILENCE;
   listeningState silenceNextState = WAKEWORD_LISTEN;

   // VAD state tracking
   float vad_speech_prob = 0.0f;
   int silence_count = 0;
   float silence_duration = 0.0f;
   float speech_duration = 0.0f;     // For pause detection / chunking
   float recording_duration = 0.0f;  // Total recording time (prevents buffer overflow)

// Pre-roll buffer: captures audio before VAD trigger to avoid missing first words
// 500ms at 16kHz mono, 16-bit = 8000 samples * 2 bytes = 16000 bytes
#define VAD_PREROLL_MS 500
#define VAD_PREROLL_BYTES 16000  // (16000 Hz * 1 channel * 2 bytes/sample * 500ms / 1000)
   static unsigned char preroll_buffer[VAD_PREROLL_BYTES];
   size_t preroll_write_pos = 0;
   size_t preroll_valid_bytes = 0;

   static struct option long_options[] = {
      { "capture", required_argument, NULL, 'c' },
      { "logfile", required_argument, NULL, 'l' },
      { "playback", required_argument, NULL, 'd' },
      { "help", no_argument, NULL, 'h' },
      { "llm-only", no_argument, NULL, 'L' },              // LLM handles everything
      { "llm-commands", no_argument, NULL, 'C' },          // Commands first, then LLM
      { "commands-only", no_argument, NULL, 'D' },         // Direct commands only (explicit)
      { "network-audio", no_argument, NULL, 'N' },         // Start server for remote DAWN access
      { "llm", required_argument, NULL, 'm' },             // Set default LLM type (local/cloud)
      { "cloud-provider", required_argument, NULL, 'P' },  // Cloud provider (openai/claude)
      { "asr-engine", required_argument, NULL, 'A' },      // ASR engine (vosk/whisper)
      { "whisper-model", required_argument, NULL, 'W' },   // Whisper model (tiny/base/small)
      { "whisper-path", required_argument, NULL, 'w' },    // Whisper models directory
      { "music-dir", required_argument, NULL, 'M' },       // Music directory (absolute path)
      { 0, 0, 0, 0 }
   };
   int option_index = 0;
   const char *cloud_provider_override = NULL;
   asr_engine_type_t asr_engine =
       ASR_ENGINE_WHISPER;              // Default: Whisper base (best performance + accuracy)
   const char *asr_model_path = NULL;   // Will be set based on engine
   const char *whisper_model = "base";  // Default Whisper model (tiny/base/small)
   const char *whisper_path =
       "../whisper.cpp/models";  // Default Whisper models directory (relative to build/)
   char whisper_full_path[512];  // Buffer to construct full model path

   // Construct default Whisper base model path
   snprintf(whisper_full_path, sizeof(whisper_full_path), "%s/ggml-%s.bin", whisper_path,
            whisper_model);
   asr_model_path = whisper_full_path;

   LOG_INFO("%s Version %s: %s\n", APP_NAME, VERSION_NUMBER, GIT_SHA);

   // TODO: I'm adding this here but it will need better error clean-ups.
   curl_global_init(CURL_GLOBAL_DEFAULT);

   while ((opt = getopt_long(argc, argv, "c:d:hl:LCDNm:P:A:W:w:M:", long_options, &option_index)) !=
          -1) {
      switch (opt) {
         case 'c':
            strncpy(pcm_capture_device, optarg, sizeof(pcm_capture_device));
            pcm_capture_device[sizeof(pcm_capture_device) - 1] = '\0';
            break;
         case 'd':
            strncpy(pcm_playback_device, optarg, sizeof(pcm_playback_device));
            pcm_playback_device[sizeof(pcm_playback_device) - 1] = '\0';
            break;
         case 'h':
            display_help(argc, argv);
            exit(EXIT_SUCCESS);
         case 'l':
            log_filename = optarg;
            break;
         case 'L':
            command_processing_mode = CMD_MODE_LLM_ONLY;
            LOG_INFO("LLM-only command processing enabled");
            break;
         case 'C':
            command_processing_mode = CMD_MODE_DIRECT_FIRST;
            LOG_INFO("Commands-first with LLM fallback enabled");
            break;
         case 'D':
            command_processing_mode = CMD_MODE_DIRECT_ONLY;
            LOG_INFO("Direct commands only mode enabled");
            break;
         case 'N':
            enable_network_audio = 1;
            LOG_INFO("Network audio enabled");
            break;
         case 'm':
            if (strcasecmp(optarg, "cloud") == 0) {
               llm_set_type(LLM_CLOUD);
               LOG_INFO("Using cloud LLM by default");
            } else if (strcasecmp(optarg, "local") == 0) {
               llm_set_type(LLM_LOCAL);
               LOG_INFO("Using local LLM by default");
            } else {
               LOG_ERROR("Unknown LLM type: %s. Using auto-detection.", optarg);
            }
            break;
         case 'P':
            cloud_provider_override = optarg;
            LOG_INFO("Cloud provider override: %s", cloud_provider_override);
            break;
         case 'W':
            whisper_model = optarg;
            LOG_INFO("Whisper model set to: %s", whisper_model);
            break;
         case 'w':
            whisper_path = optarg;
            LOG_INFO("Whisper models path set to: %s", whisper_path);
            break;
         case 'A':
#ifdef ENABLE_VOSK
            if (strcmp(optarg, "vosk") == 0) {
               asr_engine = ASR_ENGINE_VOSK;
               asr_model_path = "../models/vosk-model";  // Relative to build/
               LOG_INFO("Using Vosk ASR engine");
            } else
#endif
                if (strcmp(optarg, "whisper") == 0) {
               asr_engine = ASR_ENGINE_WHISPER;
               // Construct full path: whisper_path/ggml-MODEL.bin
               snprintf(whisper_full_path, sizeof(whisper_full_path), "%s/ggml-%s.bin",
                        whisper_path, whisper_model);
               asr_model_path = whisper_full_path;
               LOG_INFO("Using Whisper ASR engine with model: %s", asr_model_path);
            } else {
#ifdef ENABLE_VOSK
               LOG_ERROR("Unknown ASR engine: %s. Using Whisper (default).", optarg);
#else
               LOG_ERROR("Unknown ASR engine: %s (Vosk support not compiled in). Using Whisper.",
                         optarg);
#endif
            }
            break;
         case 'M':
            set_music_directory(optarg);
            LOG_INFO("Music directory set to: %s", optarg);
            break;
         case '?':
            display_help(argc, argv);
            exit(EXIT_FAILURE);
         default:
            display_help(argc, argv);
            exit(EXIT_FAILURE);
      }
   }

   // Initialize logging
   if (log_filename) {
      if (init_logging(log_filename, LOG_TO_FILE) != 0) {
         fprintf(stderr, "Failed to initialize logging to file: %s\n", log_filename);
         return 1;
      }
   } else {
      if (init_logging(NULL, LOG_TO_CONSOLE) != 0) {
         fprintf(stderr, "Failed to initialize logging to console\n");
         return 1;
      }
   }

   if (strcmp(pcm_capture_device, "") == 0) {
      strncpy(pcm_capture_device, DEFAULT_PCM_CAPTURE_DEVICE, sizeof(pcm_capture_device));
      pcm_capture_device[sizeof(pcm_capture_device) - 1] = '\0';
   }

   if (strcmp(pcm_playback_device, "") == 0) {
      strncpy(pcm_playback_device, DEFAULT_PCM_PLAYBACK_DEVICE, sizeof(pcm_playback_device));
      pcm_playback_device[sizeof(pcm_playback_device) - 1] = '\0';
   }

   // Command Processing
   initActions(actions);

   LOG_INFO("Reading json file...");
   configFile = fopen(CONFIG_FILE, "r");
   if (configFile == NULL) {
      LOG_ERROR("Unable to open config file: %s\n", CONFIG_FILE);
      return 1;
   }

   if ((bytes_read = fread(buffer, 1, sizeof(buffer), configFile)) > 0) {
      buffer[bytes_read] = '\0';
   } else {
      LOG_ERROR("Failed to read config file (%s): %s\n", CONFIG_FILE, strerror(bytes_read));
      fclose(configFile);
      return 1;
   }

   fclose(configFile);
   LOG_INFO("Done.\n");

   if (parseCommandConfig(buffer, actions, &numActions, captureDevices, &numAudioCaptureDevices,
                          playbackDevices, &numAudioPlaybackDevices)) {
      LOG_ERROR("Error parsing json.\n");
      return 1;
   }

   LOG_INFO("\n");
   //printParsedData(actions, numActions);
   convertActionsToCommands(actions, &numActions, commands, &numCommands);
   LOG_INFO("Processed %d commands.", numCommands);
   //printCommands(commands, numCommands);

   // JSON setup for OpenAI
   conversation_history = json_object_new_array();
   system_message = json_object_new_object();

   json_object_object_add(system_message, "role", json_object_new_string("system"));

   // Set the appropriate system message content based on processing mode
   if (command_processing_mode == CMD_MODE_LLM_ONLY ||
       command_processing_mode == CMD_MODE_DIRECT_FIRST) {
      // LLM modes get the enhanced prompt with command information
      json_object_object_add(system_message, "content",
                             json_object_new_string(get_command_prompt()));
      LOG_INFO("Using enhanced system prompt for LLM command processing");
   } else {
      // Direct-only mode gets the original AI description
      json_object_object_add(system_message, "content", json_object_new_string(AI_DESCRIPTION));
      LOG_INFO("Using standard system prompt for direct command processing");
   }

   json_object_array_add(conversation_history, system_message);

   // Start dedicated audio capture thread with ring buffer
   // Ring buffer size: 262144 bytes = ~8 seconds of audio at 16kHz mono 16-bit
   // Increased to prevent audio loss during Vosk processing which can take 100-500ms per iteration
   // Realtime priority: enabled (requires cap_sys_nice capability or root)
   LOG_INFO("Starting audio capture thread...");
   audio_capture_ctx = audio_capture_start(pcm_capture_device, 262144, 1);
   if (!audio_capture_ctx) {
      LOG_ERROR("Failed to start audio capture thread");
      return 1;
   }

   // Legacy audioControl structure kept for compatibility
   // (no longer used for actual capture, but needed by measureBackgroundAudio)
#ifdef ALSA_DEVICE
   myAudioControls.frames = DEFAULT_FRAMES;
   myAudioControls.full_buff_size = myAudioControls.frames * DEFAULT_CHANNELS * 2;
#else
   myAudioControls.pa_framesize = pa_frame_size(&sample_spec);
   myAudioControls.full_buff_size = myAudioControls.pa_framesize;
#endif

   LOG_INFO("max_buff_size: %u, full_buff_size: %u\n", max_buff_size,
            myAudioControls.full_buff_size);

   max_buff = (char *)malloc(max_buff_size);
   if (max_buff == NULL) {
      LOG_ERROR("malloc() failed on max_buff.\n");

      if (audio_capture_ctx) {
         audio_capture_stop(audio_capture_ctx);
         audio_capture_ctx = NULL;
      }

      return 1;
   }

   // Test background audio level
#if 0
   if (pthread_create(&backgroundAudioDetect, NULL, measureBackgroundAudio, (void *) &myAudioControls) != 0) {
      LOG_ERROR("Error creating background audio detection thread.\n");
   }
#else
   measureBackgroundAudio((void *)&myAudioControls);
#endif

   LOG_INFO("Init ASR: %s", asr_engine_name(asr_engine));
   // Initialize ASR engine (Vosk or Whisper)
   asr_context_t *asr_ctx = asr_init(asr_engine, asr_model_path, DEFAULT_RATE);
   if (asr_ctx == NULL) {
      LOG_ERROR("Error initializing ASR engine: %s\n", asr_engine_name(asr_engine));

      free(max_buff);

      if (audio_capture_ctx) {
         audio_capture_stop(audio_capture_ctx);
         audio_capture_ctx = NULL;
      }

      return 1;
   }

   // Initialize chunking manager (Whisper only, persistent lifecycle)
   chunking_manager_t *chunk_mgr = NULL;
   if (asr_engine == ASR_ENGINE_WHISPER) {
      chunk_mgr = chunking_manager_init(asr_ctx);
      if (!chunk_mgr) {
         LOG_ERROR("Failed to initialize chunking manager");
         asr_cleanup(asr_ctx);
         free(max_buff);
         if (audio_capture_ctx) {
            audio_capture_stop(audio_capture_ctx);
            audio_capture_ctx = NULL;
         }
         return 1;
      }
   }

   LOG_INFO("Init mosquitto.");
   /* MQTT Setup */
   mosquitto_lib_init();

   mosq = mosquitto_new(NULL, true, NULL);
   if (mosq == NULL) {
      LOG_ERROR("Error: Out of memory.\n");
      return 1;
   }

   /* Configure callbacks. This should be done before connecting ideally. */
   mosquitto_connect_callback_set(mosq, on_connect);
   mosquitto_subscribe_callback_set(mosq, on_subscribe);
   mosquitto_message_callback_set(mosq, on_message);

   /* Set reconnect parameters (min delay, max delay, exponential backoff) */
   mosquitto_reconnect_delay_set(mosq, 2, 30, true);

   /* Connect to local MQTT server. */
   rc = mosquitto_connect(mosq, MQTT_IP, MQTT_PORT, 60);
   if (rc != MOSQ_ERR_SUCCESS) {
      mosquitto_destroy(mosq);
      LOG_ERROR("Error on mosquitto_connect(): %s\n", mosquitto_strerror(rc));
      return 1;
   } else {
      LOG_INFO("Connected to local MQTT server.\n");
   }

   /* Start processing MQTT events. */
   mosquitto_loop_start(mosq);

   LOG_INFO("Init text to speech.");
   /* Initialize text to speech processing. */
   initialize_text_to_speech(pcm_playback_device);

   // Initialize Silero VAD
   LOG_INFO("Init Silero VAD for voice activity detection.");
   const char *home_dir = getenv("HOME");
   char vad_model_path[512];
   if (home_dir) {
      snprintf(vad_model_path, sizeof(vad_model_path),
               "%s/code/The-OASIS-Project/dawn/models/silero_vad_16k_op15.onnx", home_dir);
      vad_ctx = vad_silero_init(vad_model_path, NULL);  // Option B: separate OrtEnv
      if (!vad_ctx) {
         LOG_WARNING("Failed to initialize Silero VAD - proceeding without VAD");
      } else {
         LOG_INFO("Silero VAD initialized successfully (opset15 model, 0.311ms inference)");
      }
   } else {
      LOG_WARNING("HOME environment variable not set - VAD initialization skipped");
   }

   text_to_speech((char *)timeOfDayGreeting());

   // Register the signal handler for SIGINT.
   if (signal(SIGINT, signal_handler) == SIG_ERR) {
      LOG_ERROR("Error: Unable to register signal handler.\n");
      exit(EXIT_FAILURE);
   }

   // Initialize LLM system
   llm_init(cloud_provider_override);

   // Auto-detect local vs cloud if not set via command-line
   if (llm_get_type() == LLM_UNDEFINED) {
      if (llm_check_connection("https://api.openai.com", 4)) {
         llm_set_type(LLM_CLOUD);
         text_to_speech("Setting AI to cloud LLM.");
      } else {
         llm_set_type(LLM_LOCAL);
         text_to_speech("Setting AI to local LLM.");
      }
   }

   if (enable_network_audio) {
      LOG_INFO("Initializing network audio system...");
      if (dawn_network_audio_init() != 0) {
         LOG_ERROR("Failed to initialize network audio system");
         enable_network_audio = 0;
      } else {
         LOG_INFO("Starting DAWN network server...");
         if (dawn_server_start() != DAWN_SUCCESS) {
            LOG_ERROR("Failed to start DAWN server - network audio disabled");
            dawn_network_audio_cleanup();
            enable_network_audio = 0;
         } else {
            LOG_INFO("DAWN network server started successfully on port 5000");
            LOG_INFO("Network TTS will use existing Piper instance");
         }
      }
   }

   // Main loop
   LOG_INFO("Listening...\n");
   while (!quit) {
      if (vision_ai_ready) {
         recState = VISION_AI_READY;
         // Reset VAD state at interaction boundary (vision AI entry)
         if (vad_ctx) {
            vad_silero_reset(vad_ctx);
         }
      }

      if (enable_network_audio) {
         uint8_t *network_audio = NULL;
         size_t network_audio_size = 0;
         char client_info[64];

         if (dawn_get_network_audio(&network_audio, &network_audio_size, client_info)) {
            LOG_INFO("Network audio received from %s (%zu bytes)", client_info, network_audio_size);

            // Validate returned data
            if (!network_audio || network_audio_size == 0) {
               LOG_ERROR("dawn_get_network_audio returned invalid data");
               dawn_clear_network_audio();
               continue;
            }

            // State transition safety check
            if (recState == PROCESS_COMMAND || recState == VISION_AI_READY) {
               LOG_WARNING("Network audio received during %s - deferring",
                           recState == PROCESS_COMMAND ? "command processing" : "vision AI");

               // Send busy message TTS
               size_t busy_wav_size = 0;
               uint8_t *busy_wav = error_to_wav("I'm currently busy. Please try again in a moment.",
                                                &busy_wav_size);
               if (busy_wav) {
                  pthread_mutex_lock(&processing_mutex);
                  processing_result_data = busy_wav;
                  processing_result_size = busy_wav_size;
                  processing_complete = 1;
                  pthread_cond_signal(&processing_done);
                  pthread_mutex_unlock(&processing_mutex);

                  LOG_INFO("Sent busy message to %s", client_info);
               } else {
                  // Fallback: signal with no data (triggers echo fallback in server)
                  LOG_ERROR("Failed to generate busy TTS - client will timeout");
                  pthread_mutex_lock(&processing_mutex);
                  processing_result_data = NULL;
                  processing_result_size = 0;
                  processing_complete = 1;
                  pthread_cond_signal(&processing_done);
                  pthread_mutex_unlock(&processing_mutex);
               }

               dawn_clear_network_audio();
               continue;  // Skip to next loop iteration
            }

            // Safe to process - save current state and transition
            LOG_INFO("Interrupting %s state for network processing",
                     recState == SILENCE             ? "SILENCE"
                     : recState == WAKEWORD_LISTEN   ? "WAKEWORD_LISTEN"
                     : recState == COMMAND_RECORDING ? "COMMAND_RECORDING"
                                                     : "OTHER");

            previous_state_before_network = recState;

            // Extract PCM from WAV
            NetworkPCMData *pcm = extract_pcm_from_network_wav(network_audio, network_audio_size);
            if (pcm && pcm->is_valid) {
               // Store PCM data for processing
               pthread_mutex_lock(&network_processing_mutex);

               if (network_pcm_buffer) {
                  free(network_pcm_buffer);
               }

               network_pcm_buffer = malloc(pcm->pcm_size);
               if (network_pcm_buffer) {
                  memcpy(network_pcm_buffer, pcm->pcm_data, pcm->pcm_size);
                  network_pcm_size = pcm->pcm_size;
                  recState = NETWORK_PROCESSING;
                  LOG_INFO("Transitioned to NETWORK_PROCESSING state");
               } else {
                  LOG_ERROR("Failed to allocate network PCM buffer");

                  // Send error response to client
                  size_t error_wav_size = 0;
                  uint8_t *error_wav = error_to_wav("Sorry, I ran out of memory. Please try again.",
                                                    &error_wav_size);
                  if (error_wav) {
                     pthread_mutex_lock(&processing_mutex);
                     processing_result_data = error_wav;
                     processing_result_size = error_wav_size;
                     processing_complete = 1;
                     pthread_cond_signal(&processing_done);
                     pthread_mutex_unlock(&processing_mutex);
                  } else {
                     // Fallback: signal with no data (triggers echo fallback in server)
                     LOG_ERROR("Failed to generate busy TTS - client will timeout");
                     pthread_mutex_lock(&processing_mutex);
                     processing_result_data = NULL;
                     processing_result_size = 0;
                     processing_complete = 1;
                     pthread_cond_signal(&processing_done);
                     pthread_mutex_unlock(&processing_mutex);
                  }
               }

               pthread_mutex_unlock(&network_processing_mutex);
               free_network_pcm_data(pcm);
            } else {
               LOG_ERROR("Invalid WAV format from network client");

               // Send error TTS
               size_t error_wav_size = 0;
               uint8_t *error_wav = error_to_wav(ERROR_MSG_WAV_INVALID, &error_wav_size);
               if (error_wav) {
                  pthread_mutex_lock(&processing_mutex);
                  processing_result_data = error_wav;
                  processing_result_size = error_wav_size;
                  processing_complete = 1;
                  pthread_cond_signal(&processing_done);
                  pthread_mutex_unlock(&processing_mutex);
               } else {
                  // Fallback: signal with no data (triggers echo fallback in server)
                  LOG_ERROR("Failed to generate busy TTS - client will timeout");
                  pthread_mutex_lock(&processing_mutex);
                  processing_result_data = NULL;
                  processing_result_size = 0;
                  processing_complete = 1;
                  pthread_cond_signal(&processing_done);
                  pthread_mutex_unlock(&processing_mutex);
               }

               if (pcm)
                  free_network_pcm_data(pcm);
            }

            dawn_clear_network_audio();
         }
      }

      // Check if LLM thread has completed (non-blocking check)
      static int prev_llm_processing = 0;  // Track previous state
      if (prev_llm_processing == 1 && llm_processing == 0) {
         // LLM just completed - process the response
         LOG_INFO("LLM thread completed - processing response");

         pthread_mutex_lock(&llm_mutex);
         char *response_text = llm_response_text;
         llm_response_text = NULL;  // Transfer ownership
         pthread_mutex_unlock(&llm_mutex);

         // Join the thread to clean up resources
         pthread_join(llm_thread, NULL);

         // Check if response was interrupted
         if (response_text == NULL && llm_is_interrupt_requested()) {
            LOG_INFO("LLM was interrupted - discarding partial response");
            llm_clear_interrupt();

            // Remove the user message from conversation history (last item)
            int history_len = json_object_array_length(conversation_history);
            if (history_len > 0) {
               json_object_array_del_idx(conversation_history, history_len - 1, 1);
            }
         } else if (response_text != NULL) {
            // Process successful response
            LOG_WARNING("AI: %s\n", response_text);

            // Create cleaned version for TTS (keep original for conversation history)
            char *tts_response = strdup(response_text);

            // Process any commands in the LLM response
            if (command_processing_mode == CMD_MODE_LLM_ONLY ||
                command_processing_mode == CMD_MODE_DIRECT_FIRST) {
               int cmds_processed = parse_llm_response_for_commands(response_text, mosq);
               if (cmds_processed > 0) {
                  LOG_INFO("Processed %d commands from LLM response", cmds_processed);
               }
               if (tts_response) {
                  // Remove command tags
                  char *cmd_start, *cmd_end;
                  while ((cmd_start = strstr(tts_response, "<command>")) != NULL) {
                     cmd_end = strstr(cmd_start, "</command>");
                     if (cmd_end) {
                        cmd_end += strlen("</command>");
                        memmove(cmd_start, cmd_end, strlen(cmd_end) + 1);
                     } else {
                        break;
                     }
                  }

                  // Remove <end_of_turn> tags (local AI models)
                  char *match = NULL;
                  if ((match = strstr(tts_response, "<end_of_turn>")) != NULL) {
                     *match = '\0';
                  }

                  // Remove special characters that cause problems
                  remove_chars(tts_response, "*");
                  remove_emojis(tts_response);

                  // Trim trailing whitespace
                  size_t len = strlen(tts_response);
                  while (len > 0 &&
                         (tts_response[len - 1] == ' ' || tts_response[len - 1] == '\t' ||
                          tts_response[len - 1] == '\n' || tts_response[len - 1] == '\r')) {
                     tts_response[--len] = '\0';
                  }
               }
            }

            // TTS already handled by streaming callback - no need to call text_to_speech here
            // Note: We don't touch TTS state here - let the state machine handle it
            // If user interrupts with wake word, state machine will discard TTS
            // If user interrupts without wake word, state machine will resume TTS

            // Save original response (with command tags) to conversation history
            // but trim trailing whitespace for Claude API compatibility
            char *history_response = strdup(response_text);
            if (history_response) {
               size_t len = strlen(history_response);
               while (len > 0 &&
                      (history_response[len - 1] == ' ' || history_response[len - 1] == '\t' ||
                       history_response[len - 1] == '\n' || history_response[len - 1] == '\r')) {
                  history_response[--len] = '\0';
               }
            }

            struct json_object *ai_message = json_object_new_object();
            json_object_object_add(ai_message, "role", json_object_new_string("assistant"));
            json_object_object_add(ai_message, "content",
                                   json_object_new_string(history_response ? history_response
                                                                           : response_text));
            json_object_array_add(conversation_history, ai_message);

            free(response_text);
            free(tts_response);
            free(history_response);
         } else {
            // Response is NULL but not interrupted - error case
            pthread_mutex_lock(&tts_mutex);
            if (tts_playback_state == TTS_PLAYBACK_PAUSE) {
               tts_playback_state = TTS_PLAYBACK_DISCARD;
               pthread_cond_signal(&tts_cond);
            }
            pthread_mutex_unlock(&tts_mutex);

            LOG_ERROR("LLM error - no response");
            text_to_speech("I'm sorry but I'm currently unavailable boss.");
         }
      }
      prev_llm_processing = llm_processing;  // Update previous state

      publish_ai_state(recState);
      switch (recState) {
         case SILENCE:
            pthread_mutex_lock(&tts_mutex);
            if (tts_playback_state == TTS_PLAYBACK_PAUSE) {
               tts_playback_state = TTS_PLAYBACK_PLAY;
               pthread_cond_signal(&tts_cond);
            }
            pthread_mutex_unlock(&tts_mutex);

            capture_buffer(&myAudioControls, max_buff, max_buff_size, &buff_size);

            // Store audio in pre-roll buffer (circular buffer for speech padding)
            if (buff_size > 0) {
               size_t bytes_to_copy = buff_size;
               // Wrap around if necessary
               if (preroll_write_pos + bytes_to_copy > VAD_PREROLL_BYTES) {
                  size_t first_chunk = VAD_PREROLL_BYTES - preroll_write_pos;
                  size_t second_chunk = bytes_to_copy - first_chunk;
                  memcpy(preroll_buffer + preroll_write_pos, max_buff, first_chunk);
                  memcpy(preroll_buffer, max_buff + first_chunk, second_chunk);
                  preroll_write_pos = second_chunk;
               } else {
                  memcpy(preroll_buffer + preroll_write_pos, max_buff, bytes_to_copy);
                  preroll_write_pos += bytes_to_copy;
               }
               // Track valid bytes (up to buffer capacity)
               preroll_valid_bytes = (preroll_valid_bytes + bytes_to_copy > VAD_PREROLL_BYTES)
                                         ? VAD_PREROLL_BYTES
                                         : preroll_valid_bytes + bytes_to_copy;
            }

            // VAD-based wake word detection
            int speech_detected = 0;
            static int vad_debug_counter = 0;
            if (vad_ctx && buff_size >= VAD_SAMPLE_SIZE * sizeof(int16_t)) {
               // Process through VAD (requires 512 samples = 32ms at 16kHz)
               const int16_t *samples = (const int16_t *)max_buff;
               vad_speech_prob = vad_silero_process(vad_ctx, samples, VAD_SAMPLE_SIZE);

               // Debug logging every 50 iterations (~5 seconds)
               if (vad_debug_counter++ % 50 == 0) {
                  LOG_INFO("SILENCE: VAD=%.3f", vad_speech_prob);
               }

               if (vad_speech_prob < 0.0f) {
                  LOG_ERROR("SILENCE: VAD processing failed - assuming silence");
                  speech_detected = 0;  // Assume silence on error
               } else {
                  speech_detected = (vad_speech_prob >= VAD_SPEECH_THRESHOLD);
               }
            } else {
               // VAD not available - log error
               if (vad_debug_counter++ % 50 == 0) {
                  LOG_ERROR("SILENCE: VAD unavailable - vad_ctx=%p, buff_size=%u, need=%zu",
                            vad_ctx, buff_size, VAD_SAMPLE_SIZE * sizeof(int16_t));
               }
               speech_detected = 0;  // Assume silence if VAD unavailable
            }

            if (speech_detected) {
               LOG_INFO("SILENCE: Speech detected (VAD: %.3f), transitioning to %s (pre-roll: %zu "
                        "bytes)\n",
                        vad_speech_prob,
                        silenceNextState == WAKEWORD_LISTEN ? "WAKEWORD_LISTEN"
                                                            : "COMMAND_RECORDING",
                        preroll_valid_bytes);
               recState = silenceNextState;

               // Reset VAD state for new interaction (critical for preventing state accumulation)
               if (vad_ctx) {
                  vad_silero_reset(vad_ctx);
               }
               silence_count = 0;
               silence_duration = 0.0f;
               speech_duration = 0.0f;
               recording_duration = 0.0f;

               // Feed pre-roll buffer to ASR first (audio before VAD trigger)
               // Note: Pre-roll buffer will be reset AFTER feeding to ASR (line 2011-2013)
               // Pre-roll buffer is circular, so read from correct position
               if (preroll_valid_bytes > 0) {
                  // Defensive assertions for buffer invariants
                  assert(preroll_write_pos < VAD_PREROLL_BYTES);
                  assert(preroll_valid_bytes <= VAD_PREROLL_BYTES);

                  if (preroll_valid_bytes < VAD_PREROLL_BYTES) {
                     // Buffer not yet full, read from beginning
                     asr_result = asr_process_partial(asr_ctx, (const int16_t *)preroll_buffer,
                                                      preroll_valid_bytes / sizeof(int16_t));
                  } else {
                     // Buffer full, read from write_pos to end, then beginning to write_pos
                     size_t first_chunk_bytes = VAD_PREROLL_BYTES - preroll_write_pos;
                     asr_result = asr_process_partial(
                         asr_ctx, (const int16_t *)(preroll_buffer + preroll_write_pos),
                         first_chunk_bytes / sizeof(int16_t));
                     if (asr_result != NULL) {
                        asr_result_free(asr_result);
                        asr_result = NULL;
                     }
                     asr_result = asr_process_partial(asr_ctx, (const int16_t *)preroll_buffer,
                                                      preroll_write_pos / sizeof(int16_t));
                  }
                  if (asr_result != NULL) {
                     asr_result_free(asr_result);
                     asr_result = NULL;
                  }
               }

               // Then feed current chunk
               asr_result = asr_process_partial(asr_ctx, (const int16_t *)max_buff,
                                                buff_size / sizeof(int16_t));
               if (asr_result == NULL) {
                  LOG_ERROR("asr_process_partial() returned NULL!\n");
               } else {
                  asr_result_free(asr_result);
                  asr_result = NULL;
               }

               // Reset pre-roll buffer for next detection
               preroll_write_pos = 0;
               preroll_valid_bytes = 0;
            }
            break;
         case WAKEWORD_LISTEN:
            pthread_mutex_lock(&tts_mutex);
            if (tts_playback_state == TTS_PLAYBACK_PLAY) {
               tts_playback_state = TTS_PLAYBACK_PAUSE;
            }
            pthread_mutex_unlock(&tts_mutex);

            capture_buffer(&myAudioControls, max_buff, max_buff_size, &buff_size);

            // VAD-based speech end detection
            int is_silence = 0;
            static int ww_vad_debug_counter = 0;
            if (vad_ctx && buff_size >= VAD_SAMPLE_SIZE * sizeof(int16_t)) {
               // Process through VAD
               vad_speech_prob = vad_silero_process(vad_ctx, (const int16_t *)max_buff,
                                                    VAD_SAMPLE_SIZE);

               if (ww_vad_debug_counter++ % 50 == 0) {
                  LOG_INFO("WAKEWORD_LISTEN: VAD=%.3f", vad_speech_prob);
               }

               if (vad_speech_prob < 0.0f) {
                  LOG_ERROR("WAKEWORD_LISTEN: VAD processing failed - assuming silence");
                  is_silence = 1;  // Assume silence on error
               } else {
                  is_silence = (vad_speech_prob < VAD_SILENCE_THRESHOLD);

                  // ENGINE-AWARE AUDIO ROUTING (chunk for Whisper, stream for Vosk)
                  if (!is_silence) {
                     if (asr_engine == ASR_ENGINE_WHISPER && chunk_mgr) {
                        // Whisper: Feed to chunking manager
                        int result = chunking_manager_add_audio(chunk_mgr,
                                                                (const int16_t *)max_buff,
                                                                buff_size / sizeof(int16_t));
                        if (result != 0) {
                           LOG_ERROR("WAKEWORD_LISTEN: chunking_manager_add_audio() failed");
                        }
                     } else if (asr_engine == ASR_ENGINE_VOSK) {
                        // Vosk: Direct streaming path
                        asr_result = asr_process_partial(asr_ctx, (const int16_t *)max_buff,
                                                         buff_size / sizeof(int16_t));
                        if (asr_result == NULL) {
                           LOG_ERROR("asr_process_partial() returned NULL!\n");
                        } else {
                           asr_result_free(asr_result);
                           asr_result = NULL;
                        }
                     }
                  }
               }
            } else {
               // VAD not available - log error
               if (ww_vad_debug_counter++ % 50 == 0) {
                  LOG_ERROR("WAKEWORD_LISTEN: VAD unavailable - vad_ctx=%p, buff_size=%u, need=%zu",
                            vad_ctx, buff_size, VAD_SAMPLE_SIZE * sizeof(int16_t));
               }
               is_silence = 1;  // Assume silence if VAD unavailable
            }

            // Track silence and speech duration for speech end detection and chunking
            if (is_silence) {
               silence_duration += DEFAULT_CAPTURE_SECONDS;
            } else {
               speech_duration += DEFAULT_CAPTURE_SECONDS;
               silence_duration = 0.0f;  // Reset on speech
            }

            // Pause detection for chunking (Whisper only, similar to COMMAND_RECORDING)
            if (asr_engine == ASR_ENGINE_WHISPER && chunk_mgr) {
               int should_finalize_chunk = 0;

               // Detect natural pauses (silence after sufficient speech)
               if (is_silence && speech_duration >= VAD_MIN_CHUNK_DURATION &&
                   silence_duration >= VAD_CHUNK_PAUSE_DURATION) {
                  LOG_INFO("WAKEWORD_LISTEN: Pause detected (%.1fs) after %.1fs speech - "
                           "finalizing chunk",
                           silence_duration, speech_duration);
                  should_finalize_chunk = 1;
               }

               // Force chunk on max duration
               if (speech_duration >= VAD_MAX_CHUNK_DURATION) {
                  LOG_INFO("WAKEWORD_LISTEN: Max chunk duration reached (%.1fs) - forcing chunk",
                           speech_duration);
                  should_finalize_chunk = 1;
               }

               // Finalize chunk if triggered
               if (should_finalize_chunk && !chunking_manager_is_finalizing(chunk_mgr)) {
                  char *chunk_text = NULL;
                  int result = chunking_manager_finalize_chunk(chunk_mgr, &chunk_text);

                  if (result == 0) {
                     if (chunk_text) {
                        LOG_INFO("WAKEWORD_LISTEN: Chunk %zu finalized: \"%s\"",
                                 chunking_manager_get_num_chunks(chunk_mgr) - 1, chunk_text);
                        free(chunk_text);
                     }
                     // Reset speech duration after successful chunk
                     speech_duration = 0.0f;
                  } else {
                     LOG_ERROR("WAKEWORD_LISTEN: Chunk finalization failed");
                  }
               }
            }

            // Track total recording duration (prevents buffer overflow from background
            // conversation)
            recording_duration += DEFAULT_CAPTURE_SECONDS;

            // Flag to trigger wake word processing after finalization
            int should_check_wake_word = 0;

            // Check for maximum recording duration (50s safety limit for 60s buffer)
            if (recording_duration >= VAD_MAX_RECORDING_DURATION) {
               LOG_WARNING("WAKEWORD_LISTEN: Max recording duration reached (%.1fs), forcing "
                           "finalization to prevent buffer overflow.\n",
                           recording_duration);
               recording_duration = 0.0f;
               silence_duration = 0.0f;
               speech_duration = 0.0f;
               // Reset pre-roll buffer
               preroll_write_pos = 0;
               preroll_valid_bytes = 0;

               // ENGINE-AWARE FINALIZATION
               if (asr_engine == ASR_ENGINE_WHISPER && chunk_mgr) {
                  // Whisper: Get concatenated text from chunks
                  size_t num_chunks = chunking_manager_get_num_chunks(chunk_mgr);
                  input_text = chunking_manager_get_full_text(chunk_mgr);
                  if (input_text) {
                     LOG_WARNING("Input (from %zu chunks): %s\n", num_chunks, input_text);
                  } else {
                     LOG_WARNING("Input: (no chunks at max duration timeout)\n");
                  }
                  should_check_wake_word =
                      1;  // Set flag even with NULL input to trigger state transition
               } else {
                  // Vosk: Direct finalization
                  asr_result = asr_finalize(asr_ctx);
                  if (asr_result && asr_result->text) {
                     input_text = strdup(asr_result->text);
                     LOG_WARNING("Input: %s\n", input_text);
                  } else {
                     LOG_WARNING("Input: (Vosk finalize returned NULL or empty at max duration)\n");
                  }
                  if (asr_result) {
                     asr_result_free(asr_result);
                     asr_result = NULL;
                  }
                  should_check_wake_word =
                      1;  // Set flag even with NULL input to trigger state transition
               }
            }
            // Check if speech has ended (1.5s of silence)
            else if (silence_duration >= VAD_END_OF_SPEECH_DURATION) {
               silence_duration = 0.0f;
               speech_duration = 0.0f;
               recording_duration = 0.0f;
               // Reset pre-roll buffer
               preroll_write_pos = 0;
               preroll_valid_bytes = 0;
               LOG_WARNING(
                   "WAKEWORD_LISTEN: Speech ended (%.1fs silence), checking for wake word.\n",
                   VAD_END_OF_SPEECH_DURATION);

               // ENGINE-AWARE FINALIZATION
               if (asr_engine == ASR_ENGINE_WHISPER && chunk_mgr) {
                  // Whisper: Get concatenated text from chunks
                  size_t num_chunks = chunking_manager_get_num_chunks(chunk_mgr);
                  input_text = chunking_manager_get_full_text(chunk_mgr);
                  if (input_text) {
                     LOG_WARNING("Input (from %zu chunks): %s\n", num_chunks, input_text);
                  } else {
                     LOG_WARNING("Input: (no chunks finalized)\n");
                  }
                  should_check_wake_word =
                      1;  // Set flag even with NULL input to trigger state transition
               } else {
                  // Vosk: Direct finalization
                  asr_result = asr_finalize(asr_ctx);
                  if (asr_result == NULL) {
                     LOG_ERROR("asr_finalize() returned NULL!\n");
                  } else {
                     LOG_WARNING("Input: %s\n", asr_result->text ? asr_result->text : "");
                     if (asr_result->text) {
                        input_text = strdup(asr_result->text);
                     } else {
                        input_text = NULL;
                     }
                     asr_result_free(asr_result);
                     asr_result = NULL;
                  }
                  should_check_wake_word =
                      1;  // Set flag even with NULL input to trigger state transition
               }
            }

            // WAKE WORD PROCESSING (shared by max duration and speech end paths)
            if (should_check_wake_word) {
               if (input_text) {
                  // Normalize for wake word/command matching
                  char *normalized_text = normalize_for_matching(input_text);

                  for (i = 0; i < numGoodbyeWords; i++) {
                     if (normalized_text && strcmp(normalized_text, goodbyeWords[i]) == 0) {
                        pthread_mutex_lock(&tts_mutex);
                        if (tts_playback_state == TTS_PLAYBACK_PAUSE) {
                           tts_playback_state = TTS_PLAYBACK_DISCARD;
                           pthread_cond_signal(&tts_cond);
                        }
                        pthread_mutex_unlock(&tts_mutex);

                        text_to_speech("Goodbye sir.");

                        quit = 1;
                     }
                  }

                  pthread_mutex_lock(&tts_mutex);
                  if (tts_playback_state == TTS_PLAYBACK_PAUSE) {
                     for (i = 0; i < numCancelWords; i++) {
                        if (normalized_text && strcmp(normalized_text, cancelWords[i]) == 0) {
                           LOG_WARNING("Cancel word detected.\n");

                           tts_playback_state = TTS_PLAYBACK_DISCARD;
                           pthread_cond_signal(&tts_cond);

                           silenceNextState = WAKEWORD_LISTEN;
                           recState = SILENCE;
                           // Reset all subsystems for new utterance
                           reset_for_new_utterance(vad_ctx, asr_ctx, chunk_mgr, &silence_duration,
                                                   &speech_duration, &recording_duration,
                                                   &preroll_write_pos, &preroll_valid_bytes);
                        }
                     }
                  }
                  pthread_mutex_unlock(&tts_mutex);

                  // Search for wake word in normalized text
                  for (i = 0; i < numWakeWords; i++) {
                     char *found_ptr = normalized_text ? strstr(normalized_text, wakeWords[i])
                                                       : NULL;
                     if (found_ptr != NULL) {
                        LOG_WARNING("Wake word detected.\n");

                        // Check if LLM is currently processing - if so, interrupt it
                        if (llm_processing) {
                           LOG_INFO(
                               "Wake word detected during LLM processing - requesting interrupt");
                           llm_request_interrupt();
                        }

                        // Calculate offset in normalized text
                        size_t offset = found_ptr - normalized_text;
                        size_t wakeWordLength = strlen(wakeWords[i]);

                        // Find corresponding position in original text
                        // (accounting for removed punctuation)
                        size_t orig_offset = 0;
                        size_t norm_offset = 0;
                        while (norm_offset < offset && input_text[orig_offset] != '\0') {
                           char c = input_text[orig_offset];
                           if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                               (c >= '0' && c <= '9') || c == ' ') {
                              norm_offset++;
                           }
                           orig_offset++;
                        }

                        // Advance past wake word in original text
                        while (norm_offset < offset + wakeWordLength &&
                               input_text[orig_offset] != '\0') {
                           char c = input_text[orig_offset];
                           if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                               (c >= '0' && c <= '9') || c == ' ') {
                              norm_offset++;
                           }
                           orig_offset++;
                        }

                        next_char_ptr = input_text + orig_offset;
                        break;
                     }
                  }

                  if (i < numWakeWords) {
                     pthread_mutex_lock(&tts_mutex);
                     if (tts_playback_state == TTS_PLAYBACK_PAUSE) {
                        tts_playback_state = TTS_PLAYBACK_DISCARD;
                        pthread_cond_signal(&tts_cond);
                     }
                     pthread_mutex_unlock(&tts_mutex);

                     // Check if there's any meaningful text after wake word
                     // Skip whitespace and punctuation to see if there's actual command text
                     const char *check_ptr = next_char_ptr;
                     while (*check_ptr != '\0' &&
                            (*check_ptr == ' ' || *check_ptr == '.' || *check_ptr == ',' ||
                             *check_ptr == '!' || *check_ptr == '?')) {
                        check_ptr++;
                     }

                     if (*check_ptr == '\0') {
                        // No command after wake word - transition to COMMAND_RECORDING
                        LOG_WARNING("Wake word detected with no command, transitioning to "
                                    "COMMAND_RECORDING.\n");
                        text_to_speech("Hello sir.");

                        commandTimeout = 0;
                        silenceNextState = COMMAND_RECORDING;
                        recState = SILENCE;

                        // Reset all subsystems for new utterance
                        reset_for_new_utterance(vad_ctx, asr_ctx, chunk_mgr, &silence_duration,
                                                &speech_duration, &recording_duration,
                                                &preroll_write_pos, &preroll_valid_bytes);
                     } else {
                        // Skip leading whitespace and punctuation to get actual command text
                        const char *cmd_start = next_char_ptr;
                        while (*cmd_start != '\0' &&
                               (*cmd_start == ' ' || *cmd_start == '.' || *cmd_start == ',' ||
                                *cmd_start == '!' || *cmd_start == '?')) {
                           cmd_start++;
                        }
                        command_text = strdup(cmd_start);
                        free(input_text);
                        input_text = NULL;

                        recState = PROCESS_COMMAND;

                        // Reset all subsystems for new utterance
                        reset_for_new_utterance(vad_ctx, asr_ctx, chunk_mgr, &silence_duration,
                                                &speech_duration, &recording_duration,
                                                &preroll_write_pos, &preroll_valid_bytes);
                     }
                  } else {
                     pthread_mutex_lock(&tts_mutex);
                     if (tts_playback_state == TTS_PLAYBACK_PAUSE) {
                        tts_playback_state = TTS_PLAYBACK_PLAY;
                        pthread_cond_signal(&tts_cond);
                     }
                     pthread_mutex_unlock(&tts_mutex);

                     silenceNextState = WAKEWORD_LISTEN;
                     recState = SILENCE;

                     // Reset all subsystems for new utterance (no wake word detected)
                     reset_for_new_utterance(vad_ctx, asr_ctx, chunk_mgr, &silence_duration,
                                             &speech_duration, &recording_duration,
                                             &preroll_write_pos, &preroll_valid_bytes);
                  }

                  // Cleanup normalized text
                  if (normalized_text) {
                     free(normalized_text);
                  }

                  // Cleanup input text
                  if (input_text) {
                     free(input_text);
                     input_text = NULL;
                  }
               } else {
                  // No content to process, transition back to SILENCE to avoid infinite loop
                  LOG_INFO("WAKEWORD_LISTEN: No content to process, returning to SILENCE.\n");
                  silenceNextState = WAKEWORD_LISTEN;
                  recState = SILENCE;
                  // Reset all subsystems for new utterance
                  reset_for_new_utterance(vad_ctx, asr_ctx, chunk_mgr, &silence_duration,
                                          &speech_duration, &recording_duration, &preroll_write_pos,
                                          &preroll_valid_bytes);
               }
            }
            buff_size = 0;
            break;
         case COMMAND_RECORDING:
            pthread_mutex_lock(&tts_mutex);
            if (tts_playback_state == TTS_PLAYBACK_PAUSE) {
               tts_playback_state = TTS_PLAYBACK_DISCARD;
               pthread_cond_signal(&tts_cond);
            }
            pthread_mutex_unlock(&tts_mutex);

            capture_buffer(&myAudioControls, max_buff, max_buff_size, &buff_size);

            // Use VAD for speech end detection in command recording
            int cmd_speech_detected = 0;
            static int cmd_vad_debug_counter = 0;
            if (vad_ctx && buff_size >= VAD_SAMPLE_SIZE * sizeof(int16_t)) {
               vad_speech_prob = vad_silero_process(vad_ctx, (const int16_t *)max_buff,
                                                    VAD_SAMPLE_SIZE);

               if (cmd_vad_debug_counter++ % 50 == 0) {
                  LOG_INFO("COMMAND_RECORDING: VAD=%.3f", vad_speech_prob);
               }

               if (vad_speech_prob < 0.0f) {
                  LOG_ERROR("COMMAND_RECORDING: VAD processing failed - assuming silence");
                  cmd_speech_detected = 0;
               } else {
                  cmd_speech_detected = (vad_speech_prob >= VAD_SPEECH_THRESHOLD);
               }
            } else {
               if (cmd_vad_debug_counter++ % 50 == 0) {
                  LOG_ERROR(
                      "COMMAND_RECORDING: VAD unavailable - vad_ctx=%p, buff_size=%u, need=%zu",
                      vad_ctx, buff_size, VAD_SAMPLE_SIZE * sizeof(int16_t));
               }
               cmd_speech_detected = 0;
            }

            // ENGINE-AWARE AUDIO ROUTING (Architecture Review Issues #1, #2, #3)
            if (asr_engine == ASR_ENGINE_WHISPER && chunk_mgr) {
               // Whisper mode: Feed audio to chunking manager (which mediates ASR)
               int result = chunking_manager_add_audio(chunk_mgr, (const int16_t *)max_buff,
                                                       buff_size / sizeof(int16_t));
               if (result != 0) {
                  LOG_ERROR("COMMAND_RECORDING: chunking_manager_add_audio() failed");
               }
               // NO direct ASR calls in Whisper mode - chunking_manager owns ASR interactions
            } else if (asr_engine == ASR_ENGINE_VOSK) {
               // Vosk mode: Direct ASR path for streaming partials (unchanged)
               if (cmd_speech_detected) {
                  // Reduced logging to avoid noise with 100ms polling
                  /* For an additional layer of "silence," check if ASR output is changing */
                  asr_result = asr_process_partial(asr_ctx, (const int16_t *)max_buff,
                                                   buff_size / sizeof(int16_t));
                  if (asr_result == NULL) {
                     LOG_ERROR("asr_process_partial() returned NULL!\n");
                  } else {
                     size_t current_length = asr_result->text ? strlen(asr_result->text) : 0;
                     if (current_length == prev_text_length) {
                        text_nochange = 1;
                     }
                     prev_text_length = current_length;
                     asr_result_free(asr_result);
                     asr_result = NULL;
                  }
               }
            }

            // Track speech and silence duration for pause detection
            if (cmd_speech_detected) {
               speech_duration += DEFAULT_CAPTURE_SECONDS;
               silence_duration = 0.0f;  // Reset silence when speech detected
            } else {
               silence_duration += DEFAULT_CAPTURE_SECONDS;  // Track silence duration
            }

            // Pause detection for chunking (Whisper only - Week 3 implementation)
            if (asr_engine == ASR_ENGINE_WHISPER && chunk_mgr) {
               int should_finalize_chunk = 0;

               // Detect natural pauses for chunking (silence after sufficient speech)
               if (!cmd_speech_detected && speech_duration >= VAD_MIN_CHUNK_DURATION &&
                   silence_duration >= VAD_CHUNK_PAUSE_DURATION) {
                  LOG_INFO("COMMAND_RECORDING: Pause detected (%.1fs) after %.1fs speech - "
                           "finalizing chunk",
                           silence_duration, speech_duration);
                  should_finalize_chunk = 1;
               }

               // Force chunk on max duration (even if speech continues)
               if (speech_duration >= VAD_MAX_CHUNK_DURATION) {
                  LOG_INFO("COMMAND_RECORDING: Max chunk duration reached (%.1fs) - forcing chunk",
                           speech_duration);
                  should_finalize_chunk = 1;
               }

               // Finalize chunk if triggered (with re-entrance check)
               if (should_finalize_chunk && !chunking_manager_is_finalizing(chunk_mgr)) {
                  char *chunk_text = NULL;
                  int result = chunking_manager_finalize_chunk(chunk_mgr, &chunk_text);

                  if (result == 0) {
                     if (chunk_text) {
                        LOG_INFO("COMMAND_RECORDING: Chunk %zu finalized: \"%s\"",
                                 chunking_manager_get_num_chunks(chunk_mgr) - 1, chunk_text);
                        free(chunk_text);
                     }
                     // Reset speech duration after successful chunk finalization
                     speech_duration = 0.0f;
                  } else {
                     LOG_ERROR("COMMAND_RECORDING: Chunk finalization failed");
                  }
               }
            }

            // Only use text_nochange for Vosk (which has meaningful partials).
            // Whisper always returns empty partials, so rely solely on VAD for silence detection.
            if (!cmd_speech_detected || (text_nochange && asr_engine == ASR_ENGINE_VOSK)) {
               commandTimeout++;
               text_nochange = 0;
            } else {
               commandTimeout = 0;
               text_nochange = 0;
            }

            if (commandTimeout >= DEFAULT_COMMAND_TIMEOUT) {
               commandTimeout = 0;
               LOG_WARNING("COMMAND_RECORDING: Command processing.\n");

               // ENGINE-AWARE FINALIZATION (Architecture Review Issue #3)
               if (asr_engine == ASR_ENGINE_WHISPER && chunk_mgr) {
                  // Whisper mode: Get concatenated text from all chunks
                  size_t num_chunks = chunking_manager_get_num_chunks(chunk_mgr);
                  command_text = chunking_manager_get_full_text(chunk_mgr);

                  if (command_text) {
                     LOG_WARNING("Input (from %zu chunks): %s\n", num_chunks, command_text);
                  } else {
                     LOG_WARNING("Input: (no chunks finalized)\n");
                  }

                  // chunking_manager_get_full_text() internally calls reset(), so we're ready
                  // for next utterance. ASR context is still owned by chunking_manager.
                  recState = PROCESS_COMMAND;
               } else {
                  // Vosk mode: Direct ASR finalization (unchanged)
                  asr_result = asr_finalize(asr_ctx);
                  if (asr_result == NULL) {
                     LOG_ERROR("asr_finalize() returned NULL!\n");
                  } else {
                     LOG_WARNING("Input: %s\n", asr_result->text ? asr_result->text : "");

                     if (asr_result->text) {
                        command_text = strdup(asr_result->text);
                     } else {
                        command_text = NULL;
                     }
                     asr_result_free(asr_result);
                     asr_result = NULL;

                     recState = PROCESS_COMMAND;

                     // Reset ASR and chunking manager (durations will be reset when returning to
                     // WAKEWORD_LISTEN)
                     if (chunk_mgr) {
                        chunking_manager_reset(chunk_mgr);
                     }
                     asr_reset(asr_ctx);
                  }
               }
            }
            buff_size = 0;
            break;
         case PROCESS_COMMAND:
            // Skip processing if command is empty, whitespace, or [BLANK_AUDIO]
            if (!command_text || strlen(command_text) == 0 ||
                strspn(command_text, " \t\n\r") == strlen(command_text) ||
                strstr(command_text, "[BLANK_AUDIO]") != NULL) {
               LOG_INFO("Ignoring empty or invalid command\n");
               if (command_text) {
                  free(command_text);
                  command_text = NULL;
               }
               silenceNextState = WAKEWORD_LISTEN;
               recState = SILENCE;
               // Reset all subsystems for new utterance
               reset_for_new_utterance(vad_ctx, asr_ctx, chunk_mgr, &silence_duration,
                                       &speech_duration, &recording_duration, &preroll_write_pos,
                                       &preroll_valid_bytes);
               break;
            }

            int direct_command_found = 0;

            // Add user message to conversation history first (needed for vision context)
            // We'll add it regardless of whether it's a direct command or LLM processing
            if (command_processing_mode != CMD_MODE_DIRECT_ONLY) {
               struct json_object *user_message_early = json_object_new_object();
               json_object_object_add(user_message_early, "role", json_object_new_string("user"));
               json_object_object_add(user_message_early, "content",
                                      json_object_new_string(command_text));
               json_object_array_add(conversation_history, user_message_early);
            }

            /* Process Commands before AI if LLM command processing is disabled */
            if (command_processing_mode != CMD_MODE_LLM_ONLY) {
               /* Process Commands before AI. */
               for (i = 0; i < numCommands; i++) {
                  if (searchString(commands[i].actionWordsWildcard, command_text) == 1) {
                     char thisValue[1024];  // FIXME: These are abnormally large.
                                            // I'm in a hurry and don't want overflows.
                     char thisCommand[2048];
                     char thisSubstring[2048];
                     int strLength = 0;

                     pthread_mutex_lock(&tts_mutex);
                     if (tts_playback_state == TTS_PLAYBACK_PAUSE) {
                        tts_playback_state = TTS_PLAYBACK_DISCARD;
                        pthread_cond_signal(&tts_cond);
                     }
                     pthread_mutex_unlock(&tts_mutex);

                     memset(thisValue, '\0', sizeof(thisValue));
                     LOG_WARNING("Found command \"%s\".\n\tLooking for value in \"%s\".\n",
                                 commands[i].actionWordsWildcard, commands[i].actionWordsRegex);

                     strLength = strlen(commands[i].actionWordsRegex);
                     if ((strLength >= 2) && (commands[i].actionWordsRegex[strLength - 2] == '%') &&
                         (commands[i].actionWordsRegex[strLength - 1] == 's')) {
                        strncpy(thisSubstring, commands[i].actionWordsRegex, strLength - 2);
                        thisSubstring[strLength - 2] = '\0';
                        strcpy(thisValue,
                               extract_remaining_after_substring(command_text, thisSubstring));
                     } else {
                        int retSs = sscanf(command_text, commands[i].actionWordsRegex, thisValue);
                     }
                     snprintf(thisCommand, sizeof(thisCommand), commands[i].actionCommand,
                              thisValue);
                     LOG_WARNING("Sending: \"%s\"\n", thisCommand);

                     rc = mosquitto_publish(mosq, NULL, commands[i].topic, strlen(thisCommand),
                                            thisCommand, 0, false);
                     if (rc != MOSQ_ERR_SUCCESS) {
                        LOG_ERROR("Error publishing: %s\n", mosquitto_strerror(rc));
                     }

                     direct_command_found = 1;
                     break;
                  }
               }
            }

            /* Try LLM processing if:
             * - We're in LLM-only mode, OR
             * - We're in direct-first mode but no direct command was found, OR
             * - We're in direct-only mode but no direct command was found (for ignore word
             * processing)
             */
            if (command_processing_mode == CMD_MODE_LLM_ONLY ||
                (command_processing_mode == CMD_MODE_DIRECT_FIRST && !direct_command_found) ||
                (command_processing_mode == CMD_MODE_DIRECT_ONLY && !direct_command_found)) {
               LOG_WARNING("Processing with LLM (mode: %d, direct found: %d).\n",
                           command_processing_mode, direct_command_found);

               int ignoreCount = 0;

               // Check ignore words (only if we're in direct-only mode and no command found)
               if (command_processing_mode == CMD_MODE_DIRECT_ONLY && !direct_command_found) {
                  for (ignoreCount = 0; ignoreCount < numIgnoreWords; ignoreCount++) {
                     if (strcmp(command_text, ignoreWords[ignoreCount]) == 0) {
                        LOG_WARNING("Ignore word detected.\n");

                        pthread_mutex_lock(&tts_mutex);
                        if (tts_playback_state == TTS_PLAYBACK_PAUSE) {
                           tts_playback_state = TTS_PLAYBACK_PLAY;
                           pthread_cond_signal(&tts_cond);
                        }
                        pthread_mutex_unlock(&tts_mutex);

                        break;
                     }
                  }
               }

               if (ignoreCount < numIgnoreWords &&
                   command_processing_mode == CMD_MODE_DIRECT_ONLY) {
                  LOG_WARNING("Input ignored. Found in ignore list.\n");
                  silenceNextState = WAKEWORD_LISTEN;
                  recState = SILENCE;

                  // Reset all subsystems for new utterance (ignored empty command)
                  reset_for_new_utterance(vad_ctx, asr_ctx, chunk_mgr, &silence_duration,
                                          &speech_duration, &recording_duration, &preroll_write_pos,
                                          &preroll_valid_bytes);
               } else {
                  // User message already added at top of PROCESS_COMMAND

                  // Check if an LLM thread is already running (from a previous request or
                  // interrupt)
                  if (llm_processing) {
                     LOG_WARNING(
                         "LLM thread already running - ignoring new request (say command again "
                         "after response completes)");

                     // Remove the user message we just added (last item in conversation history)
                     int history_len = json_object_array_length(conversation_history);
                     if (history_len > 0) {
                        json_object_array_del_idx(conversation_history, history_len - 1, 1);
                     }

                     // Free command text
                     if (command_text) {
                        free(command_text);
                        command_text = NULL;
                     }

                     // Return to listening state
                     silenceNextState = WAKEWORD_LISTEN;
                     recState = SILENCE;

                     // Reset all subsystems for new utterance
                     reset_for_new_utterance(vad_ctx, asr_ctx, chunk_mgr, &silence_duration,
                                             &speech_duration, &recording_duration,
                                             &preroll_write_pos, &preroll_valid_bytes);
                     break;  // Exit PROCESS_COMMAND case
                  }

                  // Spawn LLM thread to process request (non-blocking)
                  pthread_mutex_lock(&llm_mutex);

                  // Free any previous request/response data
                  if (llm_request_text) {
                     free(llm_request_text);
                  }
                  if (llm_response_text) {
                     free(llm_response_text);
                     llm_response_text = NULL;
                  }

                  // Set up request data (transfer ownership of command_text to thread)
                  llm_request_text = command_text;
                  command_text = NULL;  // Thread will free this

                  // Pass vision data if available
                  if (vision_ai_ready && vision_ai_image != NULL) {
                     llm_vision_image = vision_ai_image;
                     llm_vision_image_size = vision_ai_image_size;
                  } else {
                     llm_vision_image = NULL;
                     llm_vision_image_size = 0;
                  }

                  // Mark LLM as processing
                  llm_processing = 1;

                  pthread_mutex_unlock(&llm_mutex);

                  // Spawn worker thread
                  int thread_result = pthread_create(&llm_thread, NULL, llm_worker_thread, NULL);
                  if (thread_result != 0) {
                     LOG_ERROR("Failed to create LLM thread: %d", thread_result);
                     pthread_mutex_lock(&llm_mutex);
                     llm_processing = 0;
                     if (llm_request_text) {
                        free(llm_request_text);
                        llm_request_text = NULL;
                     }
                     pthread_mutex_unlock(&llm_mutex);

                     text_to_speech("I'm sorry but I'm currently unavailable boss.");
                  } else {
                     LOG_INFO("LLM thread spawned - continuing audio processing");
                  }

                  // Return to listening state - audio processing continues while LLM works
                  silenceNextState = WAKEWORD_LISTEN;
                  recState = SILENCE;

                  // Reset all subsystems for new utterance
                  reset_for_new_utterance(vad_ctx, asr_ctx, chunk_mgr, &silence_duration,
                                          &speech_duration, &recording_duration, &preroll_write_pos,
                                          &preroll_valid_bytes);
                  break;  // Exit PROCESS_COMMAND case - LLM will complete asynchronously
               }
            }

            break;  // PROCESS_COMMAND case end
         case VISION_AI_READY:
            pthread_mutex_lock(&tts_mutex);
            if (tts_playback_state == TTS_PLAYBACK_PAUSE) {
               tts_playback_state = TTS_PLAYBACK_PLAY;
               pthread_cond_signal(&tts_cond);
            }
            pthread_mutex_unlock(&tts_mutex);

            // Remove assistant's viewing command and replace user's trigger phrase
            // (otherwise LLM sees "what am I looking at?" and Rule #3 triggers again)
            size_t history_len = json_object_array_length(conversation_history);

            // Remove assistant's command response (should be last message)
            if (history_len > 0) {
               struct json_object *last_msg = json_object_array_get_idx(conversation_history,
                                                                        history_len - 1);
               struct json_object *role_obj;
               if (json_object_object_get_ex(last_msg, "role", &role_obj)) {
                  const char *role = json_object_get_string(role_obj);
                  if (strcmp(role, "assistant") == 0) {
                     json_object_array_del_idx(conversation_history, history_len - 1, 1);
                     history_len--;  // Update length after deletion
                  }
               }
            }

            // Replace user's vision request with neutral prompt (avoids Rule #3 trigger)
            if (history_len > 0) {
               struct json_object *last_msg = json_object_array_get_idx(conversation_history,
                                                                        history_len - 1);
               struct json_object *role_obj;
               if (json_object_object_get_ex(last_msg, "role", &role_obj)) {
                  const char *role = json_object_get_string(role_obj);
                  if (strcmp(role, "user") == 0) {
                     // Replace the content with a neutral vision description request
                     json_object_object_del(last_msg, "content");
                     json_object_object_add(last_msg, "content",
                                            json_object_new_string(
                                                "Please describe this captured image."));
                  }
               }
            }

            // Get the AI response using the image recognition.
            // The user message was already added in PROCESS_COMMAND state
            // The vision image will be added by llm_claude.c/llm_openai.c to the last user message
            // Use streaming with TTS sentence buffering for immediate response
            response_text = llm_chat_completion_streaming_tts(
                conversation_history,
                "Describe this image in detail. Ignore any overlay graphics unless specifically "
                "asked.",
                vision_ai_image, vision_ai_image_size, dawn_tts_sentence_callback, NULL);
            if (response_text != NULL) {
               // AI returned successfully
               LOG_WARNING("AI: %s\n", response_text);
               // TTS already handled by streaming callback

               // Add the successful AI response to the conversation.
               struct json_object *ai_message = json_object_new_object();
               json_object_object_add(ai_message, "role", json_object_new_string("assistant"));
               json_object_object_add(ai_message, "content", json_object_new_string(response_text));
               json_object_array_add(conversation_history, ai_message);

               free(response_text);
            } else {
               // Error on AI response
               LOG_ERROR("GPT error.\n");
               text_to_speech("I'm sorry but I'm currently unavailable boss.");
            }

            // Cleanup the image
            if (vision_ai_image != NULL) {
               free(vision_ai_image);
               vision_ai_image = NULL;
            }
            vision_ai_image_size = 0;
            vision_ai_ready = 0;

            // Set the next listening state
            silenceNextState = WAKEWORD_LISTEN;
            recState = SILENCE;

            // Reset all subsystems for new utterance (vision AI complete)
            reset_for_new_utterance(vad_ctx, asr_ctx, chunk_mgr, &silence_duration,
                                    &speech_duration, &recording_duration, &preroll_write_pos,
                                    &preroll_valid_bytes);

            break;
         case NETWORK_PROCESSING:
            LOG_INFO("Processing network audio from client");

            pthread_mutex_lock(&network_processing_mutex);

            if (network_pcm_buffer && network_pcm_size > 0) {
               // Reset ASR and chunking manager for new network session
               if (chunk_mgr) {
                  chunking_manager_reset(chunk_mgr);
               }
               asr_reset(asr_ctx);

               // Process PCM data with ASR
               asr_result = asr_process_partial(asr_ctx, (const int16_t *)network_pcm_buffer,
                                                network_pcm_size / sizeof(int16_t));
               if (asr_result) {
                  asr_result_free(asr_result);
                  asr_result = NULL;
               }

               asr_result = asr_finalize(asr_ctx);
               if (asr_result && asr_result->text) {
                  LOG_INFO("Network transcription result: %s", asr_result->text);

                  // Use transcription text directly
                  input_text = strdup(asr_result->text);

                  /* Process Commands before AI if LLM command processing is disabled */
                  if (command_processing_mode != CMD_MODE_LLM_ONLY) {
                     /* Process Commands before AI. */
                     direct_command_found = 0;
                     for (i = 0; i < numCommands; i++) {
                        if (searchString(commands[i].actionWordsWildcard, input_text) == 1) {
                           char thisValue[1024];  // FIXME: These are abnormally large.
                                                  // I'm in a hurry and don't want overflows.
                           char thisCommand[2048];
                           char thisSubstring[2048];
                           int strLength = 0;

                           pthread_mutex_lock(&tts_mutex);
                           if (tts_playback_state == TTS_PLAYBACK_PAUSE) {
                              tts_playback_state = TTS_PLAYBACK_DISCARD;
                              pthread_cond_signal(&tts_cond);
                           }
                           pthread_mutex_unlock(&tts_mutex);

                           memset(thisValue, '\0', sizeof(thisValue));
                           LOG_WARNING("Found command \"%s\".\n\tLooking for value in \"%s\".\n",
                                       commands[i].actionWordsWildcard,
                                       commands[i].actionWordsRegex);

                           strLength = strnlen(commands[i].actionWordsRegex, MAX_COMMAND_LENGTH);
                           if ((strLength >= 2) &&
                               (commands[i].actionWordsRegex[strLength - 2] == '%') &&
                               (commands[i].actionWordsRegex[strLength - 1] == 's')) {
                              strncpy(thisSubstring, commands[i].actionWordsRegex, strLength - 2);
                              thisSubstring[strLength - 2] = '\0';
                              strcpy(thisValue,
                                     extract_remaining_after_substring(input_text, thisSubstring));
                           } else {
                              int retSs = sscanf(input_text, commands[i].actionWordsRegex,
                                                 thisValue);
                           }
                           snprintf(thisCommand, sizeof(thisCommand), commands[i].actionCommand,
                                    thisValue);
                           LOG_WARNING("Sending: \"%s\"\n", thisCommand);

                           rc = mosquitto_publish(mosq, NULL, commands[i].topic,
                                                  strlen(thisCommand), thisCommand, 0, false);
                           if (rc != MOSQ_ERR_SUCCESS) {
                              LOG_ERROR("Error publishing: %s\n", mosquitto_strerror(rc));
                           }

                           direct_command_found = 1;
                           break;
                        }
                     }
                  }

                  if (input_text && strlen(input_text) > 0 && !direct_command_found) {
                     LOG_INFO("Network speech recognized: \"%s\"", input_text);

                     // Add user message to conversation history
                     struct json_object *user_message = json_object_new_object();
                     json_object_object_add(user_message, "role", json_object_new_string("user"));
                     json_object_object_add(user_message, "content",
                                            json_object_new_string(input_text));
                     json_object_array_add(conversation_history, user_message);

                     // Get LLM response using streaming (no TTS callback - we generate WAV below)
                     response_text = llm_chat_completion_streaming(conversation_history, input_text,
                                                                   NULL, 0, NULL, NULL);

                     if (response_text && strlen(response_text) > 0) {
                        // Now be sure to filter out special characters that give us problems.
                        remove_chars(response_text, "*");
                        remove_emojis(response_text);

                        LOG_INFO("Network LLM response: \"%s\"", response_text);

                        // Generate TTS WAV for network transmission with ESP32 size limits
                        size_t response_wav_size = 0;
                        uint8_t *response_wav = NULL;

                        if (text_to_speech_to_wav(response_text, &response_wav,
                                                  &response_wav_size) == 0 &&
                            response_wav) {
                           LOG_INFO("Network TTS generated: %zu bytes", response_wav_size);

                           // Check ESP32 buffer limits
                           if (check_response_size_limit(response_wav_size)) {
                              // Fits within ESP32 limits - send as-is
                              pthread_mutex_lock(&processing_mutex);
                              processing_result_data = response_wav;
                              processing_result_size = response_wav_size;
                              processing_complete = 1;
                              pthread_cond_signal(&processing_done);
                              pthread_mutex_unlock(&processing_mutex);

                              LOG_INFO("Network TTS response ready (%zu bytes)", response_wav_size);
                           } else {
                              // Too large for ESP32 - truncate
                              LOG_WARNING("TTS response too large for ESP32, truncating...");

                              uint8_t *truncated_wav = NULL;
                              size_t truncated_size = 0;

                              if (truncate_wav_response(response_wav, response_wav_size,
                                                        &truncated_wav, &truncated_size) == 0) {
                                 // Truncation successful
                                 free(response_wav);  // Free original

                                 pthread_mutex_lock(&processing_mutex);
                                 processing_result_data = truncated_wav;
                                 processing_result_size = truncated_size;
                                 processing_complete = 1;
                                 pthread_cond_signal(&processing_done);
                                 pthread_mutex_unlock(&processing_mutex);

                                 LOG_INFO("Network TTS truncated and ready (%zu bytes)",
                                          truncated_size);
                              } else {
                                 // Truncation failed - send error TTS
                                 free(response_wav);
                                 LOG_ERROR("Failed to truncate TTS response");

                                 size_t error_wav_size = 0;
                                 uint8_t *error_wav = error_to_wav(
                                     "Response too long. Please ask for a shorter answer.",
                                     &error_wav_size);
                                 if (error_wav) {
                                    pthread_mutex_lock(&processing_mutex);
                                    processing_result_data = error_wav;
                                    processing_result_size = error_wav_size;
                                    processing_complete = 1;
                                    pthread_cond_signal(&processing_done);
                                    pthread_mutex_unlock(&processing_mutex);

                                    LOG_INFO("Sent 'too long' error TTS (%zu bytes)",
                                             error_wav_size);
                                 }
                              }
                           }
                        } else {
                           // Send TTS error
                           size_t error_wav_size = 0;
                           uint8_t *error_wav = error_to_wav(ERROR_MSG_TTS_FAILED, &error_wav_size);
                           if (error_wav) {
                              pthread_mutex_lock(&processing_mutex);
                              processing_result_data = error_wav;
                              processing_result_size = error_wav_size;
                              processing_complete = 1;
                              pthread_cond_signal(&processing_done);
                              pthread_mutex_unlock(&processing_mutex);
                           }
                        }

                        free(response_text);
                     } else {
                        LOG_WARNING("Network LLM processing failed");

                        // Send LLM timeout error
                        size_t error_wav_size = 0;
                        uint8_t *error_wav = error_to_wav(ERROR_MSG_LLM_TIMEOUT, &error_wav_size);
                        if (error_wav) {
                           pthread_mutex_lock(&processing_mutex);
                           processing_result_data = error_wav;
                           processing_result_size = error_wav_size;
                           processing_complete = 1;
                           pthread_cond_signal(&processing_done);
                           pthread_mutex_unlock(&processing_mutex);
                        }
                     }

                     free(input_text);
                  } else {
                     // Send speech error
                     size_t error_wav_size = 0;
                     uint8_t *error_wav = NULL;

                     if (direct_command_found) {
                        LOG_WARNING("Direct command found.");
                        error_wav = error_to_wav("Direct command found and acted upon.",
                                                 &error_wav_size);
                     } else {
                        LOG_WARNING("Network speech recognition failed");
                        error_wav = error_to_wav(ERROR_MSG_SPEECH_FAILED, &error_wav_size);
                     }
                     if (error_wav) {
                        pthread_mutex_lock(&processing_mutex);
                        processing_result_data = error_wav;
                        processing_result_size = error_wav_size;
                        processing_complete = 1;
                        pthread_cond_signal(&processing_done);
                        pthread_mutex_unlock(&processing_mutex);
                     }
                  }
               } else {
                  LOG_WARNING("ASR processing returned no output");

                  // Send speech error
                  size_t error_wav_size = 0;
                  uint8_t *error_wav = error_to_wav(ERROR_MSG_SPEECH_FAILED, &error_wav_size);
                  if (error_wav) {
                     pthread_mutex_lock(&processing_mutex);
                     processing_result_data = error_wav;
                     processing_result_size = error_wav_size;
                     processing_complete = 1;
                     pthread_cond_signal(&processing_done);
                     pthread_mutex_unlock(&processing_mutex);
                  }
               }

               // Cleanup ASR result
               if (asr_result) {
                  asr_result_free(asr_result);
                  asr_result = NULL;
               }

               // Cleanup network buffers
               if (network_pcm_buffer) {
                  free(network_pcm_buffer);
                  network_pcm_buffer = NULL;
                  network_pcm_size = 0;
               }
            }

            pthread_mutex_unlock(&network_processing_mutex);

            // Return to previous state
            recState = previous_state_before_network;
            LOG_INFO("Network processing complete, returned to %s",
                     recState == SILENCE ? "SILENCE" : "previous state");
            break;
         default:
            LOG_ERROR("I really shouldn't be here.\n");
      }
   }

   LOG_INFO("Quit.\n");

   if (enable_network_audio) {
      LOG_INFO("Stopping network audio system...");
      dawn_server_stop();
      dawn_network_audio_cleanup();

      // Cleanup IPC resources
      pthread_mutex_lock(&processing_mutex);
      if (processing_result_data) {
         free(processing_result_data);
         processing_result_data = NULL;
         processing_complete = 0;
      }
      pthread_mutex_unlock(&processing_mutex);

      pthread_mutex_lock(&network_processing_mutex);
      if (network_pcm_buffer) {
         free(network_pcm_buffer);
         network_pcm_buffer = NULL;
         network_pcm_size = 0;
      }
      pthread_mutex_unlock(&network_processing_mutex);

      LOG_INFO("Network audio cleanup complete");
   }

   cleanup_text_to_speech();

   // Cleanup Silero VAD
   if (vad_ctx) {
      LOG_INFO("Cleaning up Silero VAD");
      vad_silero_cleanup(vad_ctx);
      vad_ctx = NULL;
   }

   mosquitto_disconnect(mosq);
   mosquitto_loop_stop(mosq, false);
   mosquitto_lib_cleanup();

   // Save conversation history before cleanup
   if (conversation_history != NULL) {
      save_conversation_history(conversation_history);
   }

   json_object_put(conversation_history);

   // Cleanup chunking manager (if initialized)
   if (chunk_mgr) {
      chunking_manager_cleanup(chunk_mgr);
   }

   // Cleanup ASR
   asr_cleanup(asr_ctx);

   // Stop audio capture thread and clean up resources
   if (audio_capture_ctx) {
      audio_capture_stop(audio_capture_ctx);
      audio_capture_ctx = NULL;
   }

   free(max_buff);

   curl_global_cleanup();

   // Close the log file properly
   close_logging();

   return 0;
}
