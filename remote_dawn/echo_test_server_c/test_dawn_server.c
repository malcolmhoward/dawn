#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "dawn_server.h"

// Global flag for clean shutdown
static volatile int keep_running = 1;

// Signal handler for graceful shutdown
void signal_handler(int sig) {
   (void)sig;  // Unused parameter
   printf("\n[INFO] Received shutdown signal\n");
   keep_running = 0;
}

int main(int argc, char *argv[]) {
   printf("DAWN Audio Protocol Echo Server Test\n");
   printf("====================================\n");

   // Set up signal handlers for graceful shutdown
   signal(SIGINT, signal_handler);   // Ctrl+C
   signal(SIGTERM, signal_handler);  // Terminate signal

   // Start the DAWN echo server
   printf("[INFO] Starting DAWN echo server...\n");
   int result = dawn_server_start();
   if (result != DAWN_SUCCESS) {
      printf("[ERROR] Failed to start DAWN server (code: %d)\n", result);
      return EXIT_FAILURE;
   }

   printf("[INFO] DAWN echo server started successfully\n");
   printf("[INFO] Server is ready to accept connections\n");
   printf("[INFO] Press Ctrl+C to stop the server\n");
   printf("\n");

   // Main application loop - simulate other DAWN functionality
   while (keep_running && dawn_server_is_running()) {
      // In a real DAWN application, this is where you would:
      // - Handle other system tasks
      // - Update state machines
      // - Process user interface
      // - Handle other communication protocols
      // etc.

      sleep(1);  // Sleep for 1 second
   }

   // Stop the server
   printf("\n[INFO] Stopping DAWN echo server...\n");
   dawn_server_stop();

   printf("[INFO] DAWN echo server stopped\n");
   printf("[INFO] Application exiting cleanly\n");

   return EXIT_SUCCESS;
}

/*
 * Example Integration into DAWN Main Application:
 *
 * int dawn_main(int argc, char *argv[]) {
 *     // Initialize DAWN system
 *     dawn_system_init();
 *
 *     // Start echo server as background service
 *     if (dawn_server_start() != DAWN_SUCCESS) {
 *         printf("Failed to start echo server\n");
 *         return -1;
 *     }
 *
 *     // Run main DAWN state machine
 *     while (dawn_system_is_running()) {
 *         dawn_state_machine_update();
 *         dawn_ui_update();
 *         dawn_process_events();
 *
 *         // Server runs in background automatically
 *         usleep(10000); // 10ms loop
 *     }
 *
 *     // Cleanup
 *     dawn_server_stop();
 *     dawn_system_cleanup();
 *
 *     return 0;
 * }
 */
