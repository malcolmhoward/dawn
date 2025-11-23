/*
 * Simple test program for SSE parser
 */

#include <stdio.h>
#include <string.h>

#include "llm/sse_parser.h"

static int event_count = 0;

void test_callback(const char *event_type, const char *event_data, void *userdata) {
   event_count++;
   printf("[Event %d]\n", event_count);
   if (event_type) {
      printf("  Type: %s\n", event_type);
   }
   printf("  Data: %s\n", event_data);
   printf("\n");
}

int main(void) {
   printf("=== SSE Parser Test ===\n\n");

   sse_parser_t *parser = sse_parser_create(test_callback, NULL);
   if (!parser) {
      fprintf(stderr, "Failed to create parser\n");
      return 1;
   }

   // Test 1: Simple event
   printf("Test 1: Simple event\n");
   const char *test1 = "data: Hello world\n\n";
   sse_parser_feed(parser, test1, strlen(test1));

   // Test 2: Event with type
   printf("Test 2: Event with type\n");
   const char *test2 = "event: message\ndata: Test message\n\n";
   sse_parser_feed(parser, test2, strlen(test2));

   // Test 3: Multi-line data
   printf("Test 3: Multi-line data\n");
   const char *test3 = "data: Line 1\ndata: Line 2\ndata: Line 3\n\n";
   sse_parser_feed(parser, test3, strlen(test3));

   // Test 4: Multiple events in one chunk
   printf("Test 4: Multiple events\n");
   const char *test4 = "data: Event 1\n\ndata: Event 2\n\ndata: Event 3\n\n";
   sse_parser_feed(parser, test4, strlen(test4));

   // Test 5: Partial event split across chunks (real-world scenario)
   printf("Test 5: Split event\n");
   const char *test5a = "data: This is ";
   const char *test5b = "a split event\n\n";
   sse_parser_feed(parser, test5a, strlen(test5a));
   sse_parser_feed(parser, test5b, strlen(test5b));

   // Test 6: Comment line (should be ignored)
   printf("Test 6: Comment (should be ignored)\n");
   const char *test6 = ": This is a comment\ndata: Real data\n\n";
   sse_parser_feed(parser, test6, strlen(test6));

   // Test 7: OpenAI-style JSON chunk
   printf("Test 7: OpenAI-style JSON\n");
   const char *test7 =
       "data: "
       "{\"id\":\"chatcmpl-123\",\"object\":\"chat.completion.chunk\",\"choices\":[{\"delta\":{"
       "\"content\":\"Hello\"}}]}\n\n";
   sse_parser_feed(parser, test7, strlen(test7));

   // Test 8: OpenAI [DONE] signal
   printf("Test 8: OpenAI [DONE]\n");
   const char *test8 = "data: [DONE]\n\n";
   sse_parser_feed(parser, test8, strlen(test8));

   // Test 9: Claude-style JSON
   printf("Test 9: Claude-style JSON\n");
   const char *test9 =
       "data: "
       "{\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":\"text_delta\","
       "\"text\":\"Hello\"}}\n\n";
   sse_parser_feed(parser, test9, strlen(test9));

   sse_parser_free(parser);

   printf("\n=== Test Complete ===\n");
   printf("Total events received: %d\n", event_count);
   printf("Expected: 11 events\n");

   return (event_count == 11) ? 0 : 1;
}
