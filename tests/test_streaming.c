/*
 * Test program for LLM streaming
 */

#include <stdio.h>
#include <stdlib.h>

#include "llm_interface.h"
#include "logging.h"

// Stub for text_to_speech (required by llm_interface.c)
void text_to_speech(const char *text) {
   printf("[TTS stub]: %s\n", text);
}

static int chunk_count = 0;
static int sentence_count = 0;

/**
 * @brief Callback for streaming text chunks
 */
void test_chunk_callback(const char *chunk, void *userdata) {
   chunk_count++;
   printf("[Chunk %d]: %s", chunk_count, chunk);
   fflush(stdout);
}

/**
 * @brief Callback for complete sentences
 */
void test_sentence_callback(const char *sentence, void *userdata) {
   sentence_count++;
   printf("[Sentence %d]: %s\n", sentence_count, sentence);
   fflush(stdout);
}

int main(int argc, char *argv[]) {
   const char *cloud_provider = NULL;
   const char *test_prompt = "Say hello and tell me what 2+2 equals.";

   // Allow command-line override
   if (argc > 1) {
      cloud_provider = argv[1];
   }

   printf("=== LLM Streaming Test ===\n\n");

   // Initialize LLM system
   llm_init(cloud_provider);

   printf("Testing with: %s\n", llm_get_cloud_provider_name());
   printf("Prompt: %s\n\n", test_prompt);

   // Create simple conversation with just the test prompt
   json_object *conversation = json_object_new_array();
   json_object *user_message = json_object_new_object();
   json_object_object_add(user_message, "role", json_object_new_string("user"));
   json_object_object_add(user_message, "content", json_object_new_string(test_prompt));
   json_object_array_add(conversation, user_message);

   // First test non-streaming to verify API keys work
   printf("--- Testing Non-Streaming First ---\n");
   char *non_streaming_response = llm_chat_completion(conversation, test_prompt, NULL, 0);
   if (non_streaming_response) {
      printf("Non-streaming works! Response: %s\n\n", non_streaming_response);
      free(non_streaming_response);
   } else {
      printf("ERROR: Non-streaming failed - API key issue?\n\n");
      json_object_put(conversation);
      return 1;
   }

   printf("--- Now Testing Streaming (Chunks) ---\n");

   // Make streaming request
   char *response = llm_chat_completion_streaming(conversation, test_prompt, NULL, 0,
                                                  test_chunk_callback, NULL);

   printf("\n--- End of Chunk Stream ---\n\n");

   if (response) {
      printf("Complete response:\n%s\n\n", response);
      printf("Total chunks received: %d\n\n", chunk_count);
      free(response);
   } else {
      printf("ERROR: No response received\n");
      json_object_put(conversation);
      return 1;
   }

   printf("--- Now Testing Streaming with TTS Sentence Buffering ---\n");

   // Make streaming request with sentence buffering
   response = llm_chat_completion_streaming_tts(conversation, test_prompt, NULL, 0,
                                                test_sentence_callback, NULL);

   printf("\n--- End of Sentence Stream ---\n\n");

   if (response) {
      printf("Complete response:\n%s\n\n", response);
      printf("Total sentences received: %d\n", sentence_count);
      free(response);
   } else {
      printf("ERROR: No response received from TTS streaming\n");
   }

   json_object_put(conversation);

   printf("\n=== Test Complete ===\n");
   return response ? 0 : 1;
}
