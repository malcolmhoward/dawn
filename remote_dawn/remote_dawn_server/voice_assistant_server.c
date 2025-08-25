/*
 * Voice Assistant Server - Complete Audio->Text->LLM->TTS Pipeline
 * 
 * Features:
 * - Network audio reception and processing
 * - WAV header parsing and PCM extraction  
 * - Vosk speech recognition
 * - LLM integration (llama.cpp compatible)
 * - Piper TTS voice synthesis
 * - ESP32 compatibility and optimization
 */

// Enable all POSIX and GNU functions
#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "dawn_network_audio.h"
#include "dawn_server.h"
#include "dawn_tts_wrapper.h"
#include "logging.h"
#include <stdio.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <endian.h>
#include <json-c/json.h>
#include <curl/curl.h>
#include <errno.h>

// Vosk Integration
#include "vosk_api.h"

// Configuration
#define VOSK_MODEL_PATH "../../vosk-model-en-us-0.22"
#define VOSK_SAMPLE_RATE 16000
#define PIPER_MODEL_PATH "../../en_GB-alba-medium.onnx"

// LLM Integration
#define LOCALAI_URL     "http://127.0.0.1:8080"
#define OPENAI_MODEL    "gpt-4o"
#define GPT_MAX_TOKENS  4096
#define LLM_TIMEOUT_SEC 25

// PCM Data Structure
typedef struct {
    uint8_t *pcm_data;
    size_t pcm_size;
    uint32_t sample_rate;
    uint16_t num_channels;
    uint16_t bits_per_sample;
    size_t num_samples;
    double duration_seconds;
    int is_valid;
} PCMData;

// Vosk Integration Structure
typedef struct {
    VoskModel *model;
    VoskRecognizer *recognizer;
    int initialized;
    char *last_result;
    double confidence;
} VoskProcessor;

// LLM Integration Structures
struct MemoryStruct {
    char *memory;
    size_t size;
};

typedef struct {
    int initialized;
    CURL *curl_handle;
    struct curl_slist *headers;
    char *last_response;
} LLMProcessor;

// Processing context
typedef struct {
    VoskProcessor *vosk_processor;
    LLMProcessor *llm_processor;
    int processing_active;
} ProcessingContext;

static ProcessingContext processing_context = {0};
static volatile int quit = 0;

// Synchronization for server callback
pthread_mutex_t processing_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t processing_done = PTHREAD_COND_INITIALIZER;
uint8_t *processing_result_data = NULL;
size_t processing_result_size = 0;
int processing_complete = 0;

void signal_handler(int sig) {
    (void)sig;
    LOG_INFO("Shutdown signal received");
    quit = 1;
}

// TTS WAV Format Validation
int verify_tts_wav_format(const uint8_t *wav_data, size_t wav_size) {
    if (!wav_data || wav_size < sizeof(WAVHeader)) {
        LOG_ERROR("TTS WAV too small for header validation");
        return 0;
    }
    
    const WAVHeader *header = (const WAVHeader *)wav_data;
    
    // Check RIFF/WAVE headers
    if (strncmp(header->riff_header, "RIFF", 4) != 0 ||
        strncmp(header->wave_header, "WAVE", 4) != 0) {
        LOG_ERROR("TTS WAV has invalid headers");
        return 0;
    }
    
    // Extract format information (little-endian)
    uint32_t sample_rate = le32toh(header->sample_rate);
    uint16_t num_channels = le16toh(header->num_channels);
    uint16_t bits_per_sample = le16toh(header->bits_per_sample);
    uint16_t audio_format = le16toh(header->audio_format);
    
    LOG_INFO("TTS WAV format: %uHz, %u channels, %u-bit, format %u", 
             sample_rate, num_channels, bits_per_sample, audio_format);
    
    // Validate ESP32 compatibility
    int is_compatible = 1;
    
    if (audio_format != 1) {
        LOG_WARNING("Audio format not PCM: %u", audio_format);
        is_compatible = 0;
    }
    
    if (num_channels != 1) {
        LOG_WARNING("Not mono audio: %u channels", num_channels);
        is_compatible = 0;
    }
    
    if (bits_per_sample != 16) {
        LOG_WARNING("Not 16-bit audio: %u bits", bits_per_sample);
        is_compatible = 0;
    }
    
    if (is_compatible) {
        LOG_INFO("TTS WAV compatible with ESP32");
    } else {
        LOG_WARNING("TTS WAV has ESP32 compatibility issues");
    }
    
    return is_compatible;
}

// LLM Integration Functions
static size_t WriteMemoryCallback(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t realsize = size * nmemb;
    struct MemoryStruct *mem = (struct MemoryStruct *)userp;
    
    char *ptr = realloc(mem->memory, mem->size + realsize + 1);
    if (!ptr) {
        LOG_ERROR("Not enough memory for LLM response");
        return 0;
    }
    
    mem->memory = ptr;
    memcpy(&(mem->memory[mem->size]), contents, realsize);
    mem->size += realsize;
    mem->memory[mem->size] = 0;
    
    return realsize;
}

LLMProcessor* init_llm_processor(void) {
    LOG_INFO("Initializing LLM processor");
    
    LLMProcessor *processor = malloc(sizeof(LLMProcessor));
    if (!processor) {
        LOG_ERROR("Failed to allocate LLM processor");
        return NULL;
    }
    
    memset(processor, 0, sizeof(LLMProcessor));
    
    // Initialize CURL
    processor->curl_handle = curl_easy_init();
    if (!processor->curl_handle) {
        LOG_ERROR("Failed to initialize CURL");
        free(processor);
        return NULL;
    }
    
    // Setup headers
    processor->headers = curl_slist_append(processor->headers, "Content-Type: application/json");
    
    processor->initialized = 1;
    LOG_INFO("LLM processor initialized (server: %s, model: %s)", LOCALAI_URL, OPENAI_MODEL);
    
    return processor;
}

void cleanup_llm_processor(LLMProcessor *processor) {
    if (!processor) return;
    
    LOG_INFO("Cleaning up LLM processor");
    
    if (processor->last_response) {
        free(processor->last_response);
        processor->last_response = NULL;
    }
    
    if (processor->headers) {
        curl_slist_free_all(processor->headers);
        processor->headers = NULL;
    }
    
    if (processor->curl_handle) {
        curl_easy_cleanup(processor->curl_handle);
        processor->curl_handle = NULL;
    }
    
    processor->initialized = 0;
    free(processor);
}

// LLM Request Processing
char *getGptResponse(struct json_object *conversation_history, const char *input_text,
                     char *vision_ai_image, size_t vision_ai_image_size) {
    // Ignore unused parameters for now
    (void)conversation_history;
    (void)vision_ai_image; 
    (void)vision_ai_image_size;
    
    if (!input_text || strlen(input_text) == 0) {
        LOG_ERROR("No input text provided to LLM");
        return NULL;
    }
    
    LOG_INFO("Processing LLM request: \"%s\"", input_text);
    
    // Create JSON payload
    json_object *root = json_object_new_object();
    json_object *model = json_object_new_string(OPENAI_MODEL);
    json_object *max_tokens = json_object_new_int(GPT_MAX_TOKENS);
    json_object *messages = json_object_new_array();
    
    // Create user message
    json_object *user_message = json_object_new_object();
    json_object *role = json_object_new_string("user");
    json_object *content = json_object_new_string(input_text);
    
    json_object_object_add(user_message, "role", role);
    json_object_object_add(user_message, "content", content);
    json_object_array_add(messages, user_message);
    
    json_object_object_add(root, "model", model);
    json_object_object_add(root, "messages", messages);
    json_object_object_add(root, "max_tokens", max_tokens);
    
    const char *json_payload = json_object_to_json_string_ext(root, JSON_C_TO_STRING_PLAIN);
    
    // Setup CURL request
    struct MemoryStruct chunk;
    chunk.memory = malloc(1);
    chunk.size = 0;
    
    if (!chunk.memory) {
        LOG_ERROR("Failed to allocate response buffer");
        json_object_put(root);
        return NULL;
    }
    
    CURL *curl = curl_easy_init();
    if (!curl) {
        LOG_ERROR("Failed to initialize CURL");
        free(chunk.memory);
        json_object_put(root);
        return NULL;
    }
    
    // Build full URL
    char full_url[512];
    snprintf(full_url, sizeof(full_url), "%s/v1/chat/completions", LOCALAI_URL);
    
    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    
    // Configure CURL
    curl_easy_setopt(curl, CURLOPT_URL, full_url);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json_payload);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteMemoryCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&chunk);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, LLM_TIMEOUT_SEC);
    
    LOG_INFO("Waiting for LLM response (timeout: %d seconds)", LLM_TIMEOUT_SEC);
    
    // Perform request
    CURLcode res = curl_easy_perform(curl);
    
    // Cleanup CURL
    curl_easy_cleanup(curl);
    curl_slist_free_all(headers);
    json_object_put(root);
    
    if (res != CURLE_OK) {
        LOG_ERROR("CURL request failed: %s", curl_easy_strerror(res));
        free(chunk.memory);
        return NULL;
    }
    
    // Parse JSON response
    json_object *response_json = json_tokener_parse(chunk.memory);
    if (!response_json) {
        LOG_ERROR("Failed to parse JSON response");
        free(chunk.memory);
        return NULL;
    }
    
    // Extract response text
    json_object *choices = NULL;
    json_object *first_choice = NULL;
    json_object *message = NULL;
    json_object *response_content = NULL;
    
    if (!json_object_object_get_ex(response_json, "choices", &choices) ||
        json_object_get_type(choices) != json_type_array ||
        json_object_array_length(choices) < 1) {
        LOG_ERROR("Invalid response format - no choices array");
        json_object_put(response_json);
        free(chunk.memory);
        return NULL;
    }
    
    first_choice = json_object_array_get_idx(choices, 0);
    if (!first_choice) {
        LOG_ERROR("Empty choices array");
        json_object_put(response_json);
        free(chunk.memory);
        return NULL;
    }
    
    if (!json_object_object_get_ex(first_choice, "message", &message) ||
        !json_object_object_get_ex(message, "content", &response_content)) {
        LOG_ERROR("Missing message or content in response");
        json_object_put(response_json);
        free(chunk.memory);
        return NULL;
    }
    
    const char* content_str = json_object_get_string(response_content);
    if (!content_str) {
        LOG_ERROR("Empty content string");
        json_object_put(response_json);
        free(chunk.memory);
        return NULL;
    }
    
    // Duplicate response for return
    char *final_response = strdup(content_str);
    
    json_object_put(response_json);
    free(chunk.memory);
    
    if (final_response) {
        LOG_INFO("LLM response successful (%zu chars)", strlen(final_response));
    } else {
        LOG_ERROR("Failed to duplicate response string");
    }
    
    return final_response;
}

// Vosk Integration Functions
VoskProcessor* init_vosk_processor(void) {
    LOG_INFO("Initializing Vosk processor");
    
    VoskProcessor *processor = malloc(sizeof(VoskProcessor));
    if (!processor) {
        LOG_ERROR("Failed to allocate Vosk processor");
        return NULL;
    }
    
    memset(processor, 0, sizeof(VoskProcessor));
    
    // Initialize Vosk (disable logging)
    vosk_set_log_level(-1);
    
    LOG_INFO("Loading Vosk model from: %s", VOSK_MODEL_PATH);
    processor->model = vosk_model_new(VOSK_MODEL_PATH);
    if (!processor->model) {
        LOG_ERROR("Failed to load Vosk model from %s", VOSK_MODEL_PATH);
        free(processor);
        return NULL;
    }
    
    processor->recognizer = vosk_recognizer_new(processor->model, VOSK_SAMPLE_RATE);
    if (!processor->recognizer) {
        LOG_ERROR("Failed to create Vosk recognizer");
        vosk_model_free(processor->model);
        free(processor);
        return NULL;
    }
    
    processor->initialized = 1;
    LOG_INFO("Vosk processor initialized (sample rate: %d Hz)", VOSK_SAMPLE_RATE);
    
    return processor;
}

void cleanup_vosk_processor(VoskProcessor *processor) {
    if (!processor) return;
    
    LOG_INFO("Cleaning up Vosk processor");
    
    if (processor->last_result) {
        free(processor->last_result);
        processor->last_result = NULL;
    }
    
    if (processor->recognizer) {
        vosk_recognizer_free(processor->recognizer);
        processor->recognizer = NULL;
    }
    
    if (processor->model) {
        vosk_model_free(processor->model);
        processor->model = NULL;
    }
    
    processor->initialized = 0;
    free(processor);
}

char* process_audio_with_vosk(VoskProcessor *processor, const PCMData *pcm) {
    if (!processor || !processor->initialized || !pcm || !pcm->pcm_data) {
        LOG_ERROR("Invalid parameters for Vosk processing");
        return NULL;
    }
    
    LOG_INFO("Processing audio with Vosk: %zu bytes, %.2f seconds", 
             pcm->pcm_size, pcm->duration_seconds);
    
    // Verify audio format compatibility
    if (pcm->sample_rate != VOSK_SAMPLE_RATE) {
        LOG_WARNING("Sample rate mismatch - PCM: %u Hz, Vosk expects: %d Hz",
                   pcm->sample_rate, VOSK_SAMPLE_RATE);
    }
    
    if (pcm->bits_per_sample != 16) {
        LOG_WARNING("Bit depth mismatch - PCM: %u bits, Vosk expects: 16 bits",
                   pcm->bits_per_sample);
    }
    
    if (pcm->num_channels != 1) {
        LOG_WARNING("Channel mismatch - PCM: %u channels, Vosk expects: 1 (mono)",
                   pcm->num_channels);
    }
    
    // Reset recognizer for new audio
    vosk_recognizer_reset(processor->recognizer);
    
    // Feed audio data to Vosk in chunks
    const size_t chunk_size = 4000; // 4KB chunks
    size_t bytes_processed = 0;
    
    while (bytes_processed < pcm->pcm_size) {
        size_t current_chunk = (pcm->pcm_size - bytes_processed < chunk_size) ?
                              (pcm->pcm_size - bytes_processed) : chunk_size;
        
        const char *chunk_data = (const char*)(pcm->pcm_data + bytes_processed);
        
        vosk_recognizer_accept_waveform(processor->recognizer, chunk_data, current_chunk);
        bytes_processed += current_chunk;
    }
    
    // Get final result
    const char *final_result_json = vosk_recognizer_final_result(processor->recognizer);
    
    if (!final_result_json || strlen(final_result_json) < 10) {
        LOG_WARNING("No transcription result or result too short");
        return NULL;
    }
    
    // Parse JSON result to extract text
    json_object *json_result = json_tokener_parse(final_result_json);
    if (!json_result) {
        LOG_ERROR("Failed to parse Vosk JSON result");
        return NULL;
    }
    
    json_object *text_obj = NULL;
    json_object *conf_obj = NULL;
    
    if (json_object_object_get_ex(json_result, "text", &text_obj)) {
        const char *text = json_object_get_string(text_obj);
        
        if (json_object_object_get_ex(json_result, "conf", &conf_obj)) {
            processor->confidence = json_object_get_double(conf_obj);
        } else {
            processor->confidence = 0.0;
        }
        
        if (text && strlen(text) > 0) {
            // Free previous result
            if (processor->last_result) {
                free(processor->last_result);
            }
            
            // Store new result
            processor->last_result = strdup(text);
            
            LOG_INFO("Transcription successful: \"%s\" (confidence: %.2f)", 
                     text, processor->confidence);
            
            json_object_put(json_result);
            return strdup(text);
        } else {
            LOG_WARNING("Empty transcription result");
        }
    } else {
        LOG_ERROR("No 'text' field in Vosk result");
    }
    
    json_object_put(json_result);
    return NULL;
}

// WAV->PCM Conversion Functions
int validate_wav_header(const WAVHeader *header) {
    // Check RIFF header
    if (strncmp(header->riff_header, "RIFF", 4) != 0) {
        LOG_ERROR("Invalid RIFF header");
        return 0;
    }
    
    // Check WAVE header  
    if (strncmp(header->wave_header, "WAVE", 4) != 0) {
        LOG_ERROR("Invalid WAVE header");
        return 0;
    }
    
    // Check fmt header
    if (strncmp(header->fmt_header, "fmt ", 4) != 0) {
        LOG_ERROR("Invalid fmt header");
        return 0;
    }
    
    // Check data header
    if (strncmp(header->data_header, "data", 4) != 0) {
        LOG_ERROR("Invalid data header");
        return 0;
    }
    
    // Check audio format (PCM)
    if (le16toh(header->audio_format) != 1) {
        LOG_ERROR("Not PCM format (format: %u)", le16toh(header->audio_format));
        return 0;
    }
    
    return 1;
}

PCMData* extract_pcm_from_wav(const uint8_t *wav_data, size_t wav_size) {
    if (wav_size < sizeof(WAVHeader)) {
        LOG_ERROR("WAV data too small for header (%zu bytes, need %zu)", 
                 wav_size, sizeof(WAVHeader));
        return NULL;
    }
    
    // Cast to WAV header
    const WAVHeader *header = (const WAVHeader *)wav_data;
    
    // Validate WAV header
    if (!validate_wav_header(header)) {
        LOG_ERROR("WAV header validation failed");
        return NULL;
    }
    
    // Extract format information
    uint32_t sample_rate = le32toh(header->sample_rate);
    uint16_t num_channels = le16toh(header->num_channels);
    uint16_t bits_per_sample = le16toh(header->bits_per_sample);
    uint32_t data_bytes = le32toh(header->data_bytes);
    
    LOG_INFO("WAV format: %uHz, %u channels, %u-bit, %u data bytes", 
             sample_rate, num_channels, bits_per_sample, data_bytes);
    
    // Validate data size
    size_t expected_data_end = sizeof(WAVHeader) + data_bytes;
    if (expected_data_end > wav_size) {
        LOG_WARNING("Data size mismatch - header says %u bytes, but only %zu available",
                   data_bytes, wav_size - sizeof(WAVHeader));
        data_bytes = wav_size - sizeof(WAVHeader);
    }
    
    // Allocate PCM data structure
    PCMData *pcm = malloc(sizeof(PCMData));
    if (!pcm) {
        LOG_ERROR("Failed to allocate PCMData structure");
        return NULL;
    }
    
    // Allocate PCM data buffer
    pcm->pcm_data = malloc(data_bytes);
    if (!pcm->pcm_data) {
        LOG_ERROR("Failed to allocate PCM data buffer (%u bytes)", data_bytes);
        free(pcm);
        return NULL;
    }
    
    // Copy PCM data (skip WAV header)
    memcpy(pcm->pcm_data, wav_data + sizeof(WAVHeader), data_bytes);
    
    // Fill PCM structure
    pcm->pcm_size = data_bytes;
    pcm->sample_rate = sample_rate;
    pcm->num_channels = num_channels;
    pcm->bits_per_sample = bits_per_sample;
    pcm->num_samples = data_bytes / ((bits_per_sample / 8) * num_channels);
    pcm->duration_seconds = (double)pcm->num_samples / sample_rate;
    
    // Validate format for pipeline compatibility
    pcm->is_valid = (num_channels == 1 && bits_per_sample == 16);
    
    if (pcm->is_valid) {
        LOG_INFO("PCM extraction successful: %zu samples, %.2f seconds", 
                 pcm->num_samples, pcm->duration_seconds);
    } else {
        LOG_WARNING("PCM format requires conversion for pipeline compatibility");
    }
    
    return pcm;
}

void free_pcm_data(PCMData *pcm) {
    if (!pcm) return;
    
    if (pcm->pcm_data) {
        free(pcm->pcm_data);
    }
    free(pcm);
}

// Main Application
int main(int argc, char *argv[]) {
    LOG_INFO("Voice Assistant Server starting...");
    LOG_INFO("Features: Audio->Text->LLM->TTS pipeline, ESP32 compatibility");
    
    if (argc > 1 && strcmp(argv[1], "--help") == 0) {
        printf("Usage: %s [options]\n", argv[0]);
        printf("Options:\n");
        printf("  --help    Show this help message\n");
        printf("\nRequirements:\n");
        printf("  - LLM server running at %s\n", LOCALAI_URL);
        printf("  - Vosk model at %s\n", VOSK_MODEL_PATH);
        printf("  - Piper model at %s\n", PIPER_MODEL_PATH);
        return 0;
    }
    
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    // Initialize logging
    if (init_logging(NULL, LOG_TO_CONSOLE) != 0) {
        fprintf(stderr, "Failed to initialize logging\n");
        return 1;
    }
    
    // Initialize Piper TTS
    LOG_INFO("Initializing Piper TTS...");
    if (dawn_tts_init(PIPER_MODEL_PATH) != 0) {
        LOG_ERROR("Failed to initialize Piper TTS");
        LOG_ERROR("Check that Piper model exists at: %s", PIPER_MODEL_PATH);
        LOG_ERROR("Also check: %s.json", PIPER_MODEL_PATH);
        return 1;
    }
    
    // Initialize LLM Processor
    LOG_INFO("Initializing LLM processor...");
    processing_context.llm_processor = init_llm_processor();
    if (!processing_context.llm_processor) {
        LOG_ERROR("Failed to initialize LLM processor");
        LOG_ERROR("Check that llama.cpp is running at: %s", LOCALAI_URL);
        dawn_tts_cleanup();
        return 1;
    }
    
    // Initialize Vosk Processor
    LOG_INFO("Initializing Vosk processor...");
    processing_context.vosk_processor = init_vosk_processor();
    if (!processing_context.vosk_processor) {
        LOG_ERROR("Failed to initialize Vosk processor");
        LOG_ERROR("Check that Vosk model exists at: %s", VOSK_MODEL_PATH);
        cleanup_llm_processor(processing_context.llm_processor);
        dawn_tts_cleanup();
        return 1;
    }
    
    // Initialize Network Audio System
    LOG_INFO("Initializing network audio system...");
    if (dawn_network_audio_init() != 0) {
        LOG_ERROR("Failed to initialize network audio system");
        cleanup_vosk_processor(processing_context.vosk_processor);
        cleanup_llm_processor(processing_context.llm_processor);
        dawn_tts_cleanup();
        return 1;
    }
    
    // Start DAWN Server
    LOG_INFO("Starting DAWN server...");
    if (dawn_server_start() != DAWN_SUCCESS) {
        LOG_ERROR("Failed to start DAWN server");
        dawn_network_audio_cleanup();
        cleanup_vosk_processor(processing_context.vosk_processor);
        cleanup_llm_processor(processing_context.llm_processor);
        dawn_tts_cleanup();
        return 1;
    }
    
    LOG_INFO("Voice Assistant Server ready");
    LOG_INFO("Send audio from ESP32 client to begin voice processing");
    LOG_INFO("Press Ctrl+C to stop");
    
    // Main Processing Loop
    while (!quit && dawn_server_is_running()) {
        // Check for server errors
        if (!dawn_server_is_running()) {
            LOG_ERROR("DAWN server stopped unexpectedly");
            LOG_INFO("Attempting to restart server...");

            if (dawn_server_start() == DAWN_SUCCESS) {
                LOG_INFO("Server restarted successfully");
                dawn_server_set_audio_callback(dawn_process_network_audio);
            } else {
                LOG_ERROR("Failed to restart server, shutting down");
                quit = 1;
            }
        }

        // Check for network audio processing
        uint8_t *network_audio = NULL;
        size_t network_audio_size = 0;
        char client_info[64];
        char *llm_response = NULL;
        
        if (dawn_get_network_audio(&network_audio, &network_audio_size, client_info)) {
            LOG_INFO("Processing network audio from %s (%zu bytes)", client_info, network_audio_size);
            
            processing_context.processing_active = 1;
            
            // WAV->PCM Conversion
            PCMData *pcm = extract_pcm_from_wav(network_audio, network_audio_size);
            uint8_t *tts_wav_data = NULL;
            size_t tts_wav_size = 0;
            char *transcription = NULL;
            
            if (pcm) {
                // Vosk Speech Recognition  
                transcription = process_audio_with_vosk(processing_context.vosk_processor, pcm);
                if (transcription && strlen(transcription) > 0) {
                    LOG_INFO("Speech recognition successful: \"%s\"", transcription);
                    
                    // LLM Processing
                    llm_response = getGptResponse(NULL, transcription, NULL, 0);
                    if (llm_response && strlen(llm_response) > 0) {
                        LOG_INFO("LLM processing successful: \"%s\"", llm_response);
                        
                        // TTS Generation
                        int tts_result = dawn_generate_tts_wav(llm_response, &tts_wav_data, &tts_wav_size);
                        
                        if (tts_result == 0 && tts_wav_data && tts_wav_size > 0) {
                            LOG_INFO("TTS generation successful (%zu bytes)", tts_wav_size);
                            
                            if (verify_tts_wav_format(tts_wav_data, tts_wav_size)) {
                                if (check_response_size_limit(tts_wav_size)) {
                                    // Send TTS response
                                    pthread_mutex_lock(&processing_mutex);
                                    processing_result_data = tts_wav_data;
                                    processing_result_size = tts_wav_size;
                                    processing_complete = 1;
                                    pthread_cond_signal(&processing_done);
                                    pthread_mutex_unlock(&processing_mutex);
                                } else {
                                    // Truncate if needed
                                    uint8_t *truncated_data = NULL;
                                    size_t truncated_size = 0;
                                    
                                    if (truncate_wav_response(tts_wav_data, tts_wav_size, 
                                                            &truncated_data, &truncated_size) == 0) {
                                        free(tts_wav_data);
                                        
                                        pthread_mutex_lock(&processing_mutex);
                                        processing_result_data = truncated_data;
                                        processing_result_size = truncated_size;
                                        processing_complete = 1;
                                        pthread_cond_signal(&processing_done);
                                        pthread_mutex_unlock(&processing_mutex);
                                    } else {
                                    }
                                }
                            } else {
                               if (tts_wav_data) free(tts_wav_data);
                               
                               LOG_WARNING("TTS generation failed, generating error TTS");
                               uint8_t *error_tts_data = generate_error_tts(ERROR_MSG_TTS_FAILED, &tts_wav_size);
                               if (error_tts_data && tts_wav_size > 0) {
                                   pthread_mutex_lock(&processing_mutex);
                                   processing_result_data = error_tts_data;
                                   processing_result_size = tts_wav_size;
                                   processing_complete = 1;
                                   pthread_cond_signal(&processing_done);
                                   pthread_mutex_unlock(&processing_mutex);
                                   
                                   LOG_INFO("Sent TTS error TTS response: %zu bytes", tts_wav_size);
                                   goto processing_done;
                               } else {
                                   LOG_ERROR("TTS error TTS generation failed, using echo fallback");
                                   goto echo_fallback;
                               }
                            }
                        } else {
                            if (tts_wav_data) free(tts_wav_data);
                            
                            LOG_WARNING("TTS generation failed, generating error TTS");
                            uint8_t *error_tts_data = generate_error_tts(ERROR_MSG_TTS_FAILED, &tts_wav_size);
                            if (error_tts_data && tts_wav_size > 0) {
                                pthread_mutex_lock(&processing_mutex);
                                processing_result_data = error_tts_data;
                                processing_result_size = tts_wav_size;
                                processing_complete = 1;
                                pthread_cond_signal(&processing_done);
                                pthread_mutex_unlock(&processing_mutex);
                                
                                LOG_INFO("Sent TTS error TTS response: %zu bytes", tts_wav_size);
                                goto processing_done;
                            } else {
                                LOG_ERROR("TTS error TTS generation failed, using echo fallback");
                                goto echo_fallback;
                            }
                        }

                        goto processing_done;

                        echo_fallback:
                            // Echo fallback
                            uint8_t *echo_copy = malloc(network_audio_size);
                            if (echo_copy) {
                                memcpy(echo_copy, network_audio, network_audio_size);
                                
                                pthread_mutex_lock(&processing_mutex);
                                processing_result_data = echo_copy;
                                processing_result_size = network_audio_size;
                                processing_complete = 1;
                                pthread_cond_signal(&processing_done);
                                pthread_mutex_unlock(&processing_mutex);
                                
                                LOG_INFO("Using echo fallback");
                            }

                        processing_done:
                            // Cleanup
                            if (llm_response) free(llm_response);
                        
                    } else {
                       LOG_WARNING("LLM processing failed, generating error TTS");
                      
                       // Generate error TTS instead of echo
                       size_t tts_wav_size = 0;
                       uint8_t *error_tts_data = generate_error_tts(ERROR_MSG_LLM_TIMEOUT, &tts_wav_size);

                       if (error_tts_data && tts_wav_size > 0) {
                           pthread_mutex_lock(&processing_mutex);
                           processing_result_data = error_tts_data;
                           processing_result_size = tts_wav_size;
                           processing_complete = 1;
                           pthread_cond_signal(&processing_done);
                           pthread_mutex_unlock(&processing_mutex);
                          
                           LOG_INFO("Sent error TTS response: %zu bytes", tts_wav_size);
                           goto processing_done;
                       } else {
                           LOG_ERROR("Error TTS generation failed, using echo fallback");
                           goto echo_fallback;
                       }
                    }
                    
                    if (transcription) free(transcription);
                    
                } else {
                    LOG_WARNING("Speech recognition failed, generating error TTS");

                    // Generate error TTS instead of echo
                    size_t tts_wav_size = 0;
                    uint8_t *error_tts_data = generate_error_tts(ERROR_MSG_SPEECH_FAILED, &tts_wav_size);
                    if (error_tts_data && tts_wav_size > 0) {
                        pthread_mutex_lock(&processing_mutex);
                        processing_result_data = error_tts_data;
                        processing_result_size = tts_wav_size;
                        processing_complete = 1;
                        pthread_cond_signal(&processing_done);
                        pthread_mutex_unlock(&processing_mutex);
                       
                        LOG_INFO("Sent error TTS response: %zu bytes", tts_wav_size);
                        goto processing_done;
                    } else {
                        LOG_ERROR("Error TTS generation failed, using echo fallback");
                        goto echo_fallback;
                    }
                }
                
                free_pcm_data(pcm);
                
            } else {
                LOG_ERROR("WAV->PCM conversion failed, generating error TTS");
                
                uint8_t *error_tts_data = generate_error_tts(ERROR_MSG_WAV_INVALID, &tts_wav_size);
                if (error_tts_data && tts_wav_size > 0) {
                    pthread_mutex_lock(&processing_mutex);
                    processing_result_data = error_tts_data;
                    processing_result_size = tts_wav_size;
                    processing_complete = 1;
                    pthread_cond_signal(&processing_done);
                    pthread_mutex_unlock(&processing_mutex);
                    
                    LOG_INFO("Sent WAV error TTS response: %zu bytes", tts_wav_size);
                    goto processing_done;
                } else {
                    LOG_ERROR("WAV error TTS generation failed, using echo fallback");
                    goto echo_fallback;
                }
            }
            
            processing_context.processing_active = 0;
            
            // Clear the network audio
            dawn_clear_network_audio();
            
            LOG_INFO("Audio processing complete");
        }
        
        usleep(100000);  // 100ms
    }
    
    // Cleanup
    LOG_INFO("Shutting down Voice Assistant Server...");
    
    if (processing_context.vosk_processor) {
        cleanup_vosk_processor(processing_context.vosk_processor);
    }
    
    if (processing_context.llm_processor) {
        cleanup_llm_processor(processing_context.llm_processor);
    }
    
    dawn_tts_cleanup();
    dawn_server_stop();
    dawn_network_audio_cleanup();
    close_logging();
    
    LOG_INFO("Voice Assistant Server terminated successfully");
    return 0;
}
