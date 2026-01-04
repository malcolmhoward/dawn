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

/**
 * @brief Receive extended list response.
 */
static int recv_list_response(int fd,
                              admin_list_response_t *resp,
                              char *buffer,
                              size_t buffer_size) {
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

   if (resp->version != ADMIN_PROTOCOL_VERSION) {
      fprintf(stderr, "Error: Protocol version mismatch\n");
      return -1;
   }

   if (resp->payload_len > 0) {
      if (resp->payload_len > buffer_size) {
         fprintf(stderr, "Error: Response payload too large\n");
         return -1;
      }
      n = read(fd, buffer, resp->payload_len);
      if (n != resp->payload_len) {
         fprintf(stderr, "Error: Failed to read response data\n");
         return -1;
      }
   }

   return 0;
}

/**
 * @brief Build admin auth prefix.
 */
static size_t build_auth_prefix(char *buffer, const char *admin_user, const char *admin_password) {
   size_t ulen = strlen(admin_user);
   size_t plen = strlen(admin_password);

   admin_auth_prefix_t *prefix = (admin_auth_prefix_t *)buffer;
   prefix->admin_username_len = (uint8_t)ulen;
   prefix->admin_password_len = (uint8_t)plen;

   char *ptr = buffer + sizeof(admin_auth_prefix_t);
   memcpy(ptr, admin_user, ulen);
   memcpy(ptr + ulen, admin_password, plen);

   return sizeof(admin_auth_prefix_t) + ulen + plen;
}

admin_resp_code_t admin_client_list_users(int fd, admin_user_callback_t callback, void *ctx) {
   if (!callback) {
      return ADMIN_RESP_FAILURE;
   }

   if (send_message(fd, ADMIN_MSG_LIST_USERS, NULL, 0) != 0) {
      return ADMIN_RESP_SERVICE_ERROR;
   }

   admin_list_response_t resp;
   char buffer[4096];
   if (recv_list_response(fd, &resp, buffer, sizeof(buffer)) != 0) {
      return ADMIN_RESP_SERVICE_ERROR;
   }

   if (resp.response_code != ADMIN_RESP_SUCCESS) {
      return (admin_resp_code_t)resp.response_code;
   }

   /* Parse packed user entries */
   const char *p = buffer;
   const char *end = buffer + resp.payload_len;

   for (uint16_t i = 0; i < resp.item_count && p < end; i++) {
      /* Each entry: 4 bytes id, 1 byte uname_len, 1 byte is_admin,
       * 1 byte is_locked, 4 bytes failed_attempts, N bytes username */
      if (p + 11 > end)
         break;

      admin_user_entry_t entry = { 0 };

      uint32_t id;
      memcpy(&id, p, 4);
      entry.id = (int)id;
      p += 4;

      uint8_t uname_len = (uint8_t)*p++;
      entry.is_admin = (*p++) != 0;
      entry.is_locked = (*p++) != 0;

      uint32_t failed;
      memcpy(&failed, p, 4);
      entry.failed_attempts = (int)failed;
      p += 4;

      if (p + uname_len > end)
         break;
      size_t copy_len = (uname_len < 63) ? uname_len : 63;
      memcpy(entry.username, p, copy_len);
      p += uname_len;

      if (callback(&entry, ctx) != 0)
         break;
   }

   return ADMIN_RESP_SUCCESS;
}

admin_resp_code_t admin_client_delete_user(int fd,
                                           const char *admin_user,
                                           const char *admin_password,
                                           const char *target_user) {
   if (!admin_user || !admin_password || !target_user) {
      return ADMIN_RESP_FAILURE;
   }

   char payload[ADMIN_MSG_MAX_PAYLOAD];
   size_t auth_len = build_auth_prefix(payload, admin_user, admin_password);
   size_t target_len = strlen(target_user);

   if (auth_len + target_len > ADMIN_MSG_MAX_PAYLOAD) {
      return ADMIN_RESP_FAILURE;
   }

   memcpy(payload + auth_len, target_user, target_len);

   if (send_message(fd, ADMIN_MSG_DELETE_USER, payload, (uint16_t)(auth_len + target_len)) != 0) {
      return ADMIN_RESP_SERVICE_ERROR;
   }

   admin_msg_response_t resp;
   if (recv_response(fd, &resp) != 0) {
      return ADMIN_RESP_SERVICE_ERROR;
   }

   return (admin_resp_code_t)resp.response_code;
}

admin_resp_code_t admin_client_change_password(int fd,
                                               const char *admin_user,
                                               const char *admin_password,
                                               const char *target_user,
                                               const char *new_password) {
   if (!admin_user || !admin_password || !target_user || !new_password) {
      return ADMIN_RESP_FAILURE;
   }

   size_t target_len = strlen(target_user);
   size_t newpass_len = strlen(new_password);

   if (target_len == 0 || target_len > ADMIN_USERNAME_MAX_LEN) {
      return ADMIN_RESP_FAILURE;
   }
   if (newpass_len < ADMIN_PASSWORD_MIN_LEN || newpass_len > ADMIN_PASSWORD_MAX_LEN) {
      return ADMIN_RESP_FAILURE;
   }

   char payload[ADMIN_MSG_MAX_PAYLOAD];
   size_t auth_len = build_auth_prefix(payload, admin_user, admin_password);

   /* After auth: 1 byte target_uname_len, 1 byte new_pass_len, username, password */
   size_t remaining = 2 + target_len + newpass_len;
   if (auth_len + remaining > ADMIN_MSG_MAX_PAYLOAD) {
      return ADMIN_RESP_FAILURE;
   }

   char *ptr = payload + auth_len;
   *ptr++ = (uint8_t)target_len;
   *ptr++ = (uint8_t)newpass_len;
   memcpy(ptr, target_user, target_len);
   memcpy(ptr + target_len, new_password, newpass_len);

   if (send_message(fd, ADMIN_MSG_CHANGE_PASSWORD, payload, (uint16_t)(auth_len + remaining)) !=
       0) {
      return ADMIN_RESP_SERVICE_ERROR;
   }

   admin_msg_response_t resp;
   if (recv_response(fd, &resp) != 0) {
      return ADMIN_RESP_SERVICE_ERROR;
   }

   return (admin_resp_code_t)resp.response_code;
}

admin_resp_code_t admin_client_unlock_user(int fd,
                                           const char *admin_user,
                                           const char *admin_password,
                                           const char *target_user) {
   if (!admin_user || !admin_password || !target_user) {
      return ADMIN_RESP_FAILURE;
   }

   char payload[ADMIN_MSG_MAX_PAYLOAD];
   size_t auth_len = build_auth_prefix(payload, admin_user, admin_password);
   size_t target_len = strlen(target_user);

   if (auth_len + target_len > ADMIN_MSG_MAX_PAYLOAD) {
      return ADMIN_RESP_FAILURE;
   }

   memcpy(payload + auth_len, target_user, target_len);

   if (send_message(fd, ADMIN_MSG_UNLOCK_USER, payload, (uint16_t)(auth_len + target_len)) != 0) {
      return ADMIN_RESP_SERVICE_ERROR;
   }

   admin_msg_response_t resp;
   if (recv_response(fd, &resp) != 0) {
      return ADMIN_RESP_SERVICE_ERROR;
   }

   return (admin_resp_code_t)resp.response_code;
}

admin_resp_code_t admin_client_list_sessions(int fd, admin_session_callback_t callback, void *ctx) {
   if (!callback) {
      return ADMIN_RESP_FAILURE;
   }

   if (send_message(fd, ADMIN_MSG_LIST_SESSIONS, NULL, 0) != 0) {
      return ADMIN_RESP_SERVICE_ERROR;
   }

   admin_list_response_t resp;
   char buffer[8192];
   if (recv_list_response(fd, &resp, buffer, sizeof(buffer)) != 0) {
      return ADMIN_RESP_SERVICE_ERROR;
   }

   if (resp.response_code != ADMIN_RESP_SUCCESS) {
      return (admin_resp_code_t)resp.response_code;
   }

   /* Parse packed session entries */
   const char *p = buffer;
   const char *end = buffer + resp.payload_len;

   for (uint16_t i = 0; i < resp.item_count && p < end; i++) {
      /* Each entry: 8 bytes token_prefix, 1 byte uname_len, 8 bytes created,
       * 8 bytes last_activity, 1 byte ip_len, N bytes username, M bytes ip */
      if (p + 26 > end)
         break;

      admin_session_entry_t entry = { 0 };

      memcpy(entry.token_prefix, p, 8);
      entry.token_prefix[8] = '\0';
      p += 8;

      uint8_t uname_len = (uint8_t)*p++;

      memcpy(&entry.created_at, p, 8);
      p += 8;

      memcpy(&entry.last_activity, p, 8);
      p += 8;

      uint8_t ip_len = (uint8_t)*p++;

      if (p + uname_len + ip_len > end)
         break;

      size_t copy_len = (uname_len < 63) ? uname_len : 63;
      memcpy(entry.username, p, copy_len);
      p += uname_len;

      copy_len = (ip_len < 63) ? ip_len : 63;
      memcpy(entry.ip_address, p, copy_len);
      p += ip_len;

      if (callback(&entry, ctx) != 0)
         break;
   }

   return ADMIN_RESP_SUCCESS;
}

admin_resp_code_t admin_client_revoke_session(int fd,
                                              const char *admin_user,
                                              const char *admin_password,
                                              const char *token_prefix) {
   if (!admin_user || !admin_password || !token_prefix) {
      return ADMIN_RESP_FAILURE;
   }

   size_t prefix_len = strlen(token_prefix);
   if (prefix_len < 8) {
      return ADMIN_RESP_FAILURE;
   }

   /* Build payload: auth_prefix + token_prefix_len + token_prefix */
   char payload[ADMIN_MSG_MAX_PAYLOAD];
   size_t auth_len = build_auth_prefix(payload, admin_user, admin_password);

   /* Add token prefix length and token prefix */
   if (auth_len + 1 + 8 > sizeof(payload)) {
      return ADMIN_RESP_FAILURE;
   }

   char *ptr = payload + auth_len;
   *ptr++ = 8;
   memcpy(ptr, token_prefix, 8);

   if (send_message(fd, ADMIN_MSG_REVOKE_SESSION, payload, (uint16_t)(auth_len + 1 + 8)) != 0) {
      return ADMIN_RESP_SERVICE_ERROR;
   }

   admin_msg_response_t resp;
   if (recv_response(fd, &resp) != 0) {
      return ADMIN_RESP_SERVICE_ERROR;
   }

   return (admin_resp_code_t)resp.response_code;
}

admin_resp_code_t admin_client_revoke_user_sessions(int fd,
                                                    const char *admin_user,
                                                    const char *admin_password,
                                                    const char *target_user) {
   if (!admin_user || !admin_password || !target_user) {
      return ADMIN_RESP_FAILURE;
   }

   size_t target_len = strlen(target_user);
   if (target_len < 1 || target_len > 63) {
      return ADMIN_RESP_FAILURE;
   }

   /* Build payload: auth_prefix + username_len + username */
   char payload[ADMIN_MSG_MAX_PAYLOAD];
   size_t auth_len = build_auth_prefix(payload, admin_user, admin_password);

   /* Add target username */
   if (auth_len + 1 + target_len > sizeof(payload)) {
      return ADMIN_RESP_FAILURE;
   }

   char *ptr = payload + auth_len;
   *ptr++ = (uint8_t)target_len;
   memcpy(ptr, target_user, target_len);

   if (send_message(fd, ADMIN_MSG_REVOKE_USER_SESSIONS, payload,
                    (uint16_t)(auth_len + 1 + target_len)) != 0) {
      return ADMIN_RESP_SERVICE_ERROR;
   }

   admin_msg_response_t resp;
   if (recv_response(fd, &resp) != 0) {
      return ADMIN_RESP_SERVICE_ERROR;
   }

   return (admin_resp_code_t)resp.response_code;
}

admin_resp_code_t admin_client_get_stats(int fd, admin_db_stats_t *stats) {
   if (!stats) {
      return ADMIN_RESP_FAILURE;
   }

   if (send_message(fd, ADMIN_MSG_GET_STATS, NULL, 0) != 0) {
      return ADMIN_RESP_SERVICE_ERROR;
   }

   /* Receive extended response with stats data */
   admin_list_response_t resp;
   char buffer[256];
   if (recv_list_response(fd, &resp, buffer, sizeof(buffer)) != 0) {
      return ADMIN_RESP_SERVICE_ERROR;
   }

   if (resp.response_code != ADMIN_RESP_SUCCESS) {
      return (admin_resp_code_t)resp.response_code;
   }

   /* Copy stats from buffer */
   if (resp.payload_len >= sizeof(admin_db_stats_t)) {
      memcpy(stats, buffer, sizeof(admin_db_stats_t));
   }

   return ADMIN_RESP_SUCCESS;
}

admin_resp_code_t admin_client_db_compact(int fd,
                                          const char *admin_user,
                                          const char *admin_password) {
   if (!admin_user || !admin_password) {
      return ADMIN_RESP_FAILURE;
   }

   char payload[ADMIN_MSG_MAX_PAYLOAD];
   size_t auth_len = build_auth_prefix(payload, admin_user, admin_password);

   if (send_message(fd, ADMIN_MSG_DB_COMPACT, payload, (uint16_t)auth_len) != 0) {
      return ADMIN_RESP_SERVICE_ERROR;
   }

   admin_msg_response_t resp;
   if (recv_response(fd, &resp) != 0) {
      return ADMIN_RESP_SERVICE_ERROR;
   }

   return (admin_resp_code_t)resp.response_code;
}

admin_resp_code_t admin_client_db_backup(int fd,
                                         const char *admin_user,
                                         const char *admin_password,
                                         const char *dest_path) {
   if (!admin_user || !admin_password || !dest_path) {
      return ADMIN_RESP_FAILURE;
   }

   size_t path_len = strlen(dest_path);
   if (path_len < 1 || path_len > 255) {
      return ADMIN_RESP_FAILURE;
   }

   char payload[ADMIN_MSG_MAX_PAYLOAD];
   size_t auth_len = build_auth_prefix(payload, admin_user, admin_password);

   /* Add path: 1 byte length + path */
   if (auth_len + 1 + path_len > sizeof(payload)) {
      return ADMIN_RESP_FAILURE;
   }

   char *ptr = payload + auth_len;
   *ptr++ = (uint8_t)path_len;
   memcpy(ptr, dest_path, path_len);

   if (send_message(fd, ADMIN_MSG_DB_BACKUP, payload, (uint16_t)(auth_len + 1 + path_len)) != 0) {
      return ADMIN_RESP_SERVICE_ERROR;
   }

   admin_msg_response_t resp;
   if (recv_response(fd, &resp) != 0) {
      return ADMIN_RESP_SERVICE_ERROR;
   }

   return (admin_resp_code_t)resp.response_code;
}

admin_resp_code_t admin_client_query_log(int fd,
                                         const admin_log_filter_t *filter,
                                         admin_log_callback_t callback,
                                         void *ctx) {
   if (!callback) {
      return ADMIN_RESP_FAILURE;
   }

   /* Build filter payload */
   char payload[ADMIN_MSG_MAX_PAYLOAD];
   char *p = payload;

   /* Since and until (int64_t each) */
   int64_t since = filter ? filter->since : 0;
   int64_t until = filter ? filter->until : 0;
   memcpy(p, &since, 8);
   p += 8;
   memcpy(p, &until, 8);
   p += 8;

   /* Event and username lengths */
   size_t event_len = (filter && filter->event) ? strlen(filter->event) : 0;
   size_t user_len = (filter && filter->username) ? strlen(filter->username) : 0;
   if (event_len > 31)
      event_len = 31;
   if (user_len > 63)
      user_len = 63;

   *p++ = (uint8_t)event_len;
   *p++ = (uint8_t)user_len;

   /* Limit and offset */
   uint16_t limit_val = filter ? (uint16_t)filter->limit : 0;
   uint16_t offset_val = filter ? (uint16_t)filter->offset : 0;
   memcpy(p, &limit_val, 2);
   p += 2;
   memcpy(p, &offset_val, 2);
   p += 2;

   /* Event and username strings */
   if (event_len > 0) {
      memcpy(p, filter->event, event_len);
      p += event_len;
   }
   if (user_len > 0) {
      memcpy(p, filter->username, user_len);
      p += user_len;
   }

   size_t payload_len = p - payload;

   if (send_message(fd, ADMIN_MSG_QUERY_LOG, payload, (uint16_t)payload_len) != 0) {
      return ADMIN_RESP_SERVICE_ERROR;
   }

   /* Receive extended response */
   admin_list_response_t resp;
   char buffer[16384];
   if (recv_list_response(fd, &resp, buffer, sizeof(buffer)) != 0) {
      return ADMIN_RESP_SERVICE_ERROR;
   }

   if (resp.response_code != ADMIN_RESP_SUCCESS) {
      return (admin_resp_code_t)resp.response_code;
   }

   /* Parse log entries */
   const char *rp = buffer;
   const char *end = buffer + resp.payload_len;

   for (uint16_t i = 0; i < resp.item_count && rp < end; i++) {
      /* Each entry: 8 bytes timestamp, 4 length bytes, variable strings */
      if (rp + 12 > end)
         break;

      admin_log_entry_t entry = { 0 };

      memcpy(&entry.timestamp, rp, 8);
      rp += 8;

      uint8_t event_l = (uint8_t)*rp++;
      uint8_t user_l = (uint8_t)*rp++;
      uint8_t ip_l = (uint8_t)*rp++;
      uint8_t details_l = (uint8_t)*rp++;

      if (rp + event_l + user_l + ip_l + details_l > end)
         break;

      size_t copy_len = (event_l < 31) ? event_l : 31;
      memcpy(entry.event, rp, copy_len);
      rp += event_l;

      copy_len = (user_l < 63) ? user_l : 63;
      memcpy(entry.username, rp, copy_len);
      rp += user_l;

      copy_len = (ip_l < 63) ? ip_l : 63;
      memcpy(entry.ip_address, rp, copy_len);
      rp += ip_l;

      copy_len = (details_l < 255) ? details_l : 255;
      memcpy(entry.details, rp, copy_len);
      rp += details_l;

      if (callback(&entry, ctx) != 0)
         break;
   }

   return ADMIN_RESP_SUCCESS;
}

admin_resp_code_t admin_client_list_blocked_ips(int fd, admin_ip_callback_t callback, void *ctx) {
   if (!callback) {
      return ADMIN_RESP_FAILURE;
   }

   if (send_message(fd, ADMIN_MSG_LIST_BLOCKED_IPS, NULL, 0) != 0) {
      return ADMIN_RESP_SERVICE_ERROR;
   }

   admin_list_response_t resp;
   char buffer[4096];
   if (recv_list_response(fd, &resp, buffer, sizeof(buffer)) != 0) {
      return ADMIN_RESP_SERVICE_ERROR;
   }

   if (resp.response_code != ADMIN_RESP_SUCCESS) {
      return (admin_resp_code_t)resp.response_code;
   }

   /* Parse packed IP entries */
   const char *rp = buffer;
   const char *end = buffer + resp.payload_len;

   for (uint16_t i = 0; i < resp.item_count && rp < end; i++) {
      admin_ip_entry_t entry = { 0 };

      /* Unpack ip_len */
      uint8_t ip_len = (uint8_t)*rp++;

      /* Unpack failed_attempts (little-endian) */
      int32_t attempts;
      memcpy(&attempts, rp, 4);
      rp += 4;
      entry.failed_attempts = attempts;

      /* Unpack last_attempt (little-endian) */
      int64_t ts;
      memcpy(&ts, rp, 8);
      rp += 8;
      entry.last_attempt = ts;

      /* Unpack ip_address */
      size_t copy_len = (ip_len < 63) ? ip_len : 63;
      memcpy(entry.ip_address, rp, copy_len);
      rp += ip_len;

      if (callback(&entry, ctx) != 0)
         break;
   }

   return ADMIN_RESP_SUCCESS;
}

admin_resp_code_t admin_client_unblock_ip(int fd,
                                          const char *admin_user,
                                          const char *admin_password,
                                          const char *ip_address) {
   if (!admin_user || !admin_password || !ip_address) {
      return ADMIN_RESP_FAILURE;
   }

   char payload[ADMIN_MSG_MAX_PAYLOAD];
   size_t auth_len = build_auth_prefix(payload, admin_user, admin_password);
   size_t ip_len = strlen(ip_address);

   if (auth_len + ip_len > ADMIN_MSG_MAX_PAYLOAD) {
      return ADMIN_RESP_FAILURE;
   }

   memcpy(payload + auth_len, ip_address, ip_len);

   if (send_message(fd, ADMIN_MSG_UNBLOCK_IP, payload, (uint16_t)(auth_len + ip_len)) != 0) {
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
         return "Operation failed";
      case ADMIN_RESP_RATE_LIMITED:
         return "Too many failed attempts - please wait and try again";
      case ADMIN_RESP_SERVICE_ERROR:
         return "Internal service error";
      case ADMIN_RESP_VERSION_MISMATCH:
         return "Protocol version mismatch - update dawn-admin";
      case ADMIN_RESP_UNAUTHORIZED:
         return "Unauthorized - invalid admin credentials";
      case ADMIN_RESP_LAST_ADMIN:
         return "Cannot delete the last admin user";
      case ADMIN_RESP_NOT_FOUND:
         return "User or session not found";
      default:
         return "Unknown error";
   }
}
