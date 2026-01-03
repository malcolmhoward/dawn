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
 *
 * dawn-admin: Administrative CLI for Dawn daemon management.
 *
 * Phase 1 implements:
 *   dawn-admin user create <username> --admin
 *
 * This validates the setup token, prompts for password, and creates
 * the user in the database atomically.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "password_prompt.h"
#include "socket_client.h"

#define VERSION "1.0.0"

static void print_usage(const char *prog) {
   fprintf(stderr, "Dawn Admin CLI v%s\n\n", VERSION);
   fprintf(stderr, "Usage: %s <command> [options]\n\n", prog);
   fprintf(stderr, "Commands:\n");
   fprintf(stderr, "  user create <username> --admin    Create an admin user (first-run setup)\n");
   fprintf(stderr, "  ping                              Test connection to daemon\n");
   fprintf(stderr, "  help                              Show this help message\n");
   fprintf(stderr, "\nEnvironment:\n");
   fprintf(stderr, "  DAWN_SETUP_TOKEN    Setup token (skips interactive prompt)\n");
   fprintf(stderr, "\nExamples:\n");
   fprintf(stderr, "  %s user create admin --admin\n", prog);
   fprintf(stderr, "  DAWN_SETUP_TOKEN=DAWN-XXXX-XXXX-XXXX-XXXX %s user create admin --admin\n",
           prog);
}

static int cmd_ping(void) {
   int fd = admin_client_connect();
   if (fd < 0) {
      return 1;
   }

   int result = admin_client_ping(fd);
   admin_client_disconnect(fd);

   if (result == 0) {
      printf("Dawn daemon is running and responsive.\n");
      return 0;
   } else {
      fprintf(stderr, "Failed to ping daemon.\n");
      return 1;
   }
}

static int cmd_user_create(const char *username, int is_admin) {
   if (!username || strlen(username) == 0) {
      fprintf(stderr, "Error: Username is required\n");
      return 1;
   }

   if (!is_admin) {
      fprintf(stderr, "Error: --admin flag is required for initial setup\n");
      fprintf(stderr, "Hint: Non-admin user creation will be available in Phase 2\n");
      return 1;
   }

   printf("Creating admin user: %s\n\n", username);

   /* Prompt for setup token first (before connecting to minimize socket hold time) */
   char token[64] = { 0 };
   if (prompt_input("Enter setup token: ", token, sizeof(token)) != 0) {
      fprintf(stderr, "Error: Failed to read setup token\n");
      return 1;
   }

   /* Validate token format (basic check) */
   if (strlen(token) != SETUP_TOKEN_LENGTH - 1 || strncmp(token, "DAWN-", 5) != 0) {
      fprintf(stderr, "Error: Invalid token format (expected DAWN-XXXX-XXXX-XXXX-XXXX)\n");
      secure_clear(token, sizeof(token));
      return 1;
   }

   printf("\n");

   /* Prompt for password */
   char password[PASSWORD_MAX_LENGTH] = { 0 };
   if (prompt_password_confirm(password, sizeof(password)) != 0) {
      secure_clear(token, sizeof(token));
      return 1;
   }

   /* Connect to daemon */
   int fd = admin_client_connect();
   if (fd < 0) {
      secure_clear(token, sizeof(token));
      secure_clear(password, sizeof(password));
      return 1;
   }

   /* Create user (atomic token validation + user creation) */
   printf("\nCreating user account...\n");
   admin_resp_code_t resp = admin_client_create_user(fd, token, username, password, is_admin != 0);

   /* Clear sensitive data from memory */
   secure_clear(token, sizeof(token));
   secure_clear(password, sizeof(password));

   admin_client_disconnect(fd);

   if (resp != ADMIN_RESP_SUCCESS) {
      fprintf(stderr, "Error: %s\n", admin_resp_strerror(resp));
      return 1;
   }

   printf("\n");
   printf("========================================\n");
   printf("  User created successfully!\n");
   printf("========================================\n");
   printf("\n");
   printf("  Username: %s\n", username);
   printf("  Role:     %s\n", is_admin ? "admin" : "user");
   printf("\n");
   printf("You can now log in to the WebUI with these credentials.\n");
   printf("\n");

   return 0;
}

int main(int argc, char *argv[]) {
   if (argc < 2) {
      print_usage(argv[0]);
      return 1;
   }

   const char *cmd = argv[1];

   /* Help command */
   if (strcmp(cmd, "help") == 0 || strcmp(cmd, "--help") == 0 || strcmp(cmd, "-h") == 0) {
      print_usage(argv[0]);
      return 0;
   }

   /* Ping command */
   if (strcmp(cmd, "ping") == 0) {
      return cmd_ping();
   }

   /* User commands */
   if (strcmp(cmd, "user") == 0) {
      if (argc < 3) {
         fprintf(stderr, "Error: Missing user subcommand\n");
         fprintf(stderr, "Usage: %s user create <username> --admin\n", argv[0]);
         return 1;
      }

      const char *subcmd = argv[2];

      if (strcmp(subcmd, "create") == 0) {
         if (argc < 4) {
            fprintf(stderr, "Error: Missing username\n");
            fprintf(stderr, "Usage: %s user create <username> --admin\n", argv[0]);
            return 1;
         }

         const char *username = argv[3];
         int is_admin = 0;

         /* Check for --admin flag */
         for (int i = 4; i < argc; i++) {
            if (strcmp(argv[i], "--admin") == 0) {
               is_admin = 1;
               break;
            }
         }

         return cmd_user_create(username, is_admin);
      } else {
         fprintf(stderr, "Error: Unknown user subcommand: %s\n", subcmd);
         fprintf(stderr, "Available: create\n");
         return 1;
      }
   }

   fprintf(stderr, "Error: Unknown command: %s\n", cmd);
   print_usage(argv[0]);
   return 1;
}
