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
 * Socket client implementation for dawn-admin CLI.
 */

#include "socket_client.h"

#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

int admin_client_connect(void) {
   int fd = socket(AF_UNIX, SOCK_STREAM, 0);
   if (fd < 0) {
      fprintf(stderr, "Error: Failed to create socket: %s\n", strerror(errno));
      return -1;
   }

   struct sockaddr_un addr;
   memset(&addr, 0, sizeof(addr));
   addr.sun_family = AF_UNIX;

   /* Use abstract socket namespace (Linux-specific) */
   addr.sun_path[0] = '\0';
   strncpy(addr.sun_path + 1, ADMIN_SOCKET_ABSTRACT_NAME, sizeof(addr.sun_path) - 2);

   socklen_t addr_len = offsetof(struct sockaddr_un, sun_path) + 1 +
                        strlen(ADMIN_SOCKET_ABSTRACT_NAME);

   if (connect(fd, (struct sockaddr *)&addr, addr_len) != 0) {
      if (errno == ECONNREFUSED) {
         fprintf(stderr, "Error: Dawn daemon is not running or admin socket not available\n");
      } else if (errno == ENOENT) {
         fprintf(stderr, "Error: Dawn admin socket not found - is the daemon running?\n");
      } else {
         fprintf(stderr, "Error: Failed to connect to daemon: %s\n", strerror(errno));
      }
      close(fd);
      return -1;
   }

   return fd;
}

void admin_client_disconnect(int fd) {
   if (fd >= 0) {
      close(fd);
   }
}

static int send_message(int fd, admin_msg_type_t type, const void *payload, uint16_t payload_len) {
   admin_msg_header_t header = { 0 };
   header.version = ADMIN_PROTOCOL_VERSION;
   header.msg_type = (uint8_t)type;
   header.payload_len = payload_len;

   /* Send header */
   ssize_t sent = write(fd, &header, sizeof(header));
   if (sent != sizeof(header)) {
      fprintf(stderr, "Error: Failed to send message header\n");
      return -1;
   }

   /* Send payload if present */
   if (payload_len > 0 && payload != NULL) {
      sent = write(fd, payload, payload_len);
      if (sent != payload_len) {
         fprintf(stderr, "Error: Failed to send message payload\n");
         return -1;
      }
   }

   return 0;
}

static int recv_response(int fd, admin_msg_response_t *resp) {
   ssize_t n = read(fd, resp, sizeof(*resp));
   if (n != sizeof(*resp)) {
      if (n == 0) {
         fprintf(stderr, "Error: Daemon closed connection\n");
      } else if (n < 0) {
         fprintf(stderr, "Error: Failed to read response: %s\n", strerror(errno));
      } else {
         fprintf(stderr, "Error: Incomplete response (got %zd bytes)\n", n);
      }
      return -1;
   }

   /* Validate protocol version */
   if (resp->version != ADMIN_PROTOCOL_VERSION) {
      fprintf(stderr, "Error: Protocol version mismatch (got 0x%02x, expected 0x%02x)\n",
              resp->version, ADMIN_PROTOCOL_VERSION);
      return -1;
   }

   return 0;
}

int admin_client_ping(int fd) {
   if (send_message(fd, ADMIN_MSG_PING, NULL, 0) != 0) {
      return -1;
   }

   admin_msg_response_t resp;
   if (recv_response(fd, &resp) != 0) {
      return -1;
   }

   return (resp.response_code == ADMIN_RESP_SUCCESS) ? 0 : -1;
}

admin_resp_code_t admin_client_validate_token(int fd, const char *token) {
   if (!token) {
      return ADMIN_RESP_FAILURE;
   }

   size_t token_len = strlen(token);
   if (token_len > ADMIN_MSG_MAX_PAYLOAD) {
      return ADMIN_RESP_FAILURE;
   }

   if (send_message(fd, ADMIN_MSG_VALIDATE_SETUP_TOKEN, token, (uint16_t)token_len) != 0) {
      return ADMIN_RESP_SERVICE_ERROR;
   }

   admin_msg_response_t resp;
   if (recv_response(fd, &resp) != 0) {
      return ADMIN_RESP_SERVICE_ERROR;
   }

   return (admin_resp_code_t)resp.response_code;
}

admin_resp_code_t admin_client_create_user(int fd,
                                           const char *token,
                                           const char *username,
                                           const char *password,
                                           bool is_admin) {
   if (!token || !username || !password) {
      return ADMIN_RESP_FAILURE;
   }

   size_t token_len = strlen(token);
   size_t username_len = strlen(username);
   size_t password_len = strlen(password);

   /* Validate lengths */
   if (token_len != SETUP_TOKEN_LENGTH - 1) {
      fprintf(stderr, "Error: Invalid token format\n");
      return ADMIN_RESP_FAILURE;
   }

   if (username_len == 0 || username_len > ADMIN_USERNAME_MAX_LEN) {
      fprintf(stderr, "Error: Username must be 1-%d characters\n", ADMIN_USERNAME_MAX_LEN);
      return ADMIN_RESP_FAILURE;
   }

   if (password_len < ADMIN_PASSWORD_MIN_LEN || password_len > ADMIN_PASSWORD_MAX_LEN) {
      fprintf(stderr, "Error: Password must be %d-%d characters\n", ADMIN_PASSWORD_MIN_LEN,
              ADMIN_PASSWORD_MAX_LEN);
      return ADMIN_RESP_FAILURE;
   }

   /* Build payload:
    * - setup_token (24 bytes, no null terminator)
    * - username_len (1 byte)
    * - password_len (1 byte)
    * - is_admin (1 byte)
    * - username (username_len bytes)
    * - password (password_len bytes)
    */
   size_t payload_size = sizeof(admin_create_user_payload_t) + username_len + password_len;
   if (payload_size > ADMIN_MSG_MAX_PAYLOAD) {
      fprintf(stderr, "Error: Payload too large\n");
      return ADMIN_RESP_FAILURE;
   }

   char payload[ADMIN_MSG_MAX_PAYLOAD];
   memset(payload, 0, sizeof(payload));

   admin_create_user_payload_t *hdr = (admin_create_user_payload_t *)payload;
   memcpy(hdr->setup_token, token, SETUP_TOKEN_LENGTH - 1);
   hdr->username_len = (uint8_t)username_len;
   hdr->password_len = (uint8_t)password_len;
   hdr->is_admin = is_admin ? 1 : 0;

   /* Copy username and password after header */
   char *data_ptr = payload + sizeof(admin_create_user_payload_t);
   memcpy(data_ptr, username, username_len);
   memcpy(data_ptr + username_len, password, password_len);

   if (send_message(fd, ADMIN_MSG_CREATE_USER, payload, (uint16_t)payload_size) != 0) {
      return ADMIN_RESP_SERVICE_ERROR;
   }

   admin_msg_response_t resp;
   if (recv_response(fd, &resp) != 0) {
      return ADMIN_RESP_SERVICE_ERROR;
   }

   return (admin_resp_code_t)resp.response_code;
}

const char *admin_resp_strerror(admin_resp_code_t code) {
   switch (code) {
      case ADMIN_RESP_SUCCESS:
         return "Success";
      case ADMIN_RESP_FAILURE:
         return "Invalid or expired token";
      case ADMIN_RESP_RATE_LIMITED:
         return "Too many failed attempts - please wait and try again";
      case ADMIN_RESP_SERVICE_ERROR:
         return "Internal service error";
      case ADMIN_RESP_VERSION_MISMATCH:
         return "Protocol version mismatch - update dawn-admin";
      case ADMIN_RESP_UNAUTHORIZED:
         return "Unauthorized - must run as root or dawn user";
      default:
         return "Unknown error";
   }
}
