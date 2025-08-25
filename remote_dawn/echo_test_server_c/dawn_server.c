#include "dawn_server.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <arpa/inet.h>
#include <sys/select.h>
#include <time.h>
#include <signal.h>

// === Global Server State ===
static pthread_t server_thread;
static volatile int server_running = 0;
static int server_socket_fd = -1;
static pthread_mutex_t server_mutex = PTHREAD_MUTEX_INITIALIZER;

// === Forward Declarations ===
static void *dawn_server_thread(void *arg);
static int dawn_handle_client_connection(int client_fd, struct sockaddr_in *client_addr);
static int dawn_handle_handshake(dawn_client_session_t *session);
static int dawn_receive_data_chunks(dawn_client_session_t *session, uint8_t **data_out, size_t *size_out);
static int dawn_send_data_chunks(dawn_client_session_t *session, const uint8_t *data, size_t size);
static int dawn_send_ack(int socket_fd);
static int dawn_send_nack(int socket_fd);

// === Utility Functions ===

uint16_t dawn_calculate_checksum(const uint8_t *data, size_t length) {
    if (!data || length == 0) return 0;
    
    uint16_t sum1 = 0;
    uint16_t sum2 = 0;
    
    for (size_t i = 0; i < length; i++) {
        sum1 = (sum1 + data[i]) % 255;
        sum2 = (sum2 + sum1) % 255;
    }
    
    return (sum2 << 8) | sum1;
}

void dawn_build_packet_header(uint8_t *header, uint32_t data_length, 
                             uint8_t packet_type, uint16_t checksum) {
    if (!header) return;
    
    // 4 bytes: data length (big-endian)
    header[0] = (data_length >> 24) & 0xFF;
    header[1] = (data_length >> 16) & 0xFF;
    header[2] = (data_length >> 8) & 0xFF;
    header[3] = data_length & 0xFF;
    
    // 1 byte: protocol version
    header[4] = PROTOCOL_VERSION;
    
    // 1 byte: packet type
    header[5] = packet_type;
    
    // 2 bytes: checksum (big-endian)
    header[6] = (checksum >> 8) & 0xFF;
    header[7] = checksum & 0xFF;
}

int dawn_parse_packet_header(const uint8_t *header, dawn_packet_header_t *parsed) {
    if (!header || !parsed) {
        return DAWN_ERROR;
    }
    
    // Parse data length (big-endian)
    parsed->data_length = ((uint32_t)header[0] << 24) |
                         ((uint32_t)header[1] << 16) |
                         ((uint32_t)header[2] << 8) |
                         (uint32_t)header[3];
    
    // Parse protocol version
    parsed->protocol_version = header[4];
    
    // Verify protocol version
    if (parsed->protocol_version != PROTOCOL_VERSION) {
        printf("[ERROR] Invalid protocol version: 0x%02X (expected 0x%02X)\n",
               parsed->protocol_version, PROTOCOL_VERSION);
        return DAWN_ERROR_PROTOCOL;
    }
    
    // Parse packet type and checksum
    parsed->packet_type = header[5];
    parsed->checksum = ((uint16_t)header[6] << 8) | (uint16_t)header[7];
    
    return DAWN_SUCCESS;
}

int dawn_read_exact(int socket_fd, uint8_t *buffer, size_t n) {
    if (!buffer || n == 0) return DAWN_ERROR;
    
    size_t total_read = 0;
    
    while (total_read < n) {
        ssize_t bytes_read = recv(socket_fd, buffer + total_read, n - total_read, 0);
        
        if (bytes_read <= 0) {
            if (bytes_read == 0) {
                printf("[WARN] Connection closed by peer\n");
                return DAWN_ERROR;
            } else if (errno == EWOULDBLOCK || errno == EAGAIN) {
                printf("[WARN] Socket read timeout\n");
                return DAWN_ERROR_TIMEOUT;
            } else {
                printf("[ERROR] Socket read error: %s\n", strerror(errno));
                return DAWN_ERROR_SOCKET;
            }
        }
        
        total_read += bytes_read;
    }
    
    return DAWN_SUCCESS;
}

int dawn_send_exact(int socket_fd, const uint8_t *buffer, size_t n) {
    if (!buffer || n == 0) return DAWN_ERROR;
    
    size_t total_sent = 0;
    
    while (total_sent < n) {
        ssize_t bytes_sent = send(socket_fd, buffer + total_sent, n - total_sent, 0);
        
        if (bytes_sent <= 0) {
            if (errno == EWOULDBLOCK || errno == EAGAIN) {
                printf("[WARN] Socket write timeout\n");
                return DAWN_ERROR_TIMEOUT;
            } else if (errno == EPIPE) {
                printf("[WARN] Broken pipe - client disconnected\n");
                return DAWN_ERROR_SOCKET;
            } else {
                printf("[ERROR] Socket write error: %s\n", strerror(errno));
                return DAWN_ERROR_SOCKET;
            }
        }
        
        total_sent += bytes_sent;
    }
    
    return DAWN_SUCCESS;
}

// === Protocol Implementation ===

static int dawn_send_ack(int socket_fd) {
    uint8_t header[PACKET_HEADER_SIZE];
    dawn_build_packet_header(header, 0, PACKET_TYPE_ACK, 0);
    
    int result = dawn_send_exact(socket_fd, header, PACKET_HEADER_SIZE);
    if (result == DAWN_SUCCESS) {
        printf("[DEBUG] Sent ACK\n");
    }
    return result;
}

static int dawn_send_nack(int socket_fd) {
    uint8_t header[PACKET_HEADER_SIZE];
    dawn_build_packet_header(header, 0, PACKET_TYPE_NACK, 0);
    
    int result = dawn_send_exact(socket_fd, header, PACKET_HEADER_SIZE);
    if (result == DAWN_SUCCESS) {
        printf("[DEBUG] Sent NACK\n");
    }
    return result;
}

static int dawn_handle_handshake(dawn_client_session_t *session) {
    if (!session) return DAWN_ERROR;
    
    printf("[DEBUG] %s: Starting handshake\n", session->client_ip);
    
    // Read handshake header
    uint8_t header_buffer[PACKET_HEADER_SIZE];
    int result = dawn_read_exact(session->socket_fd, header_buffer, PACKET_HEADER_SIZE);
    if (result != DAWN_SUCCESS) {
        printf("[-] %s: Failed to read handshake header\n", session->client_ip);
        return result;
    }
    
    // Parse header
    dawn_packet_header_t header;
    result = dawn_parse_packet_header(header_buffer, &header);
    if (result != DAWN_SUCCESS || header.packet_type != PACKET_TYPE_HANDSHAKE) {
        printf("[-] %s: Invalid handshake header, type=0x%02X\n", 
               session->client_ip, header.packet_type);
        return DAWN_ERROR_PROTOCOL;
    }
    
    // Read handshake data
    if (header.data_length != 4) {
        printf("[-] %s: Invalid handshake data length: %u (expected 4)\n",
               session->client_ip, header.data_length);
        return DAWN_ERROR_PROTOCOL;
    }
    
    uint8_t magic_bytes[4];
    result = dawn_read_exact(session->socket_fd, magic_bytes, 4);
    if (result != DAWN_SUCCESS) {
        printf("[-] %s: Failed to read handshake data\n", session->client_ip);
        return result;
    }
    
    // Verify checksum
    uint16_t actual_checksum = dawn_calculate_checksum(magic_bytes, 4);
    if (actual_checksum != header.checksum) {
        printf("[-] %s: Handshake checksum mismatch: expected 0x%04X, got 0x%04X\n",
               session->client_ip, header.checksum, actual_checksum);
        return DAWN_ERROR_PROTOCOL;
    }
    
    // Verify magic bytes
    if (magic_bytes[0] != MAGIC_BYTE_0 || magic_bytes[1] != MAGIC_BYTE_1 ||
        magic_bytes[2] != MAGIC_BYTE_2 || magic_bytes[3] != MAGIC_BYTE_3) {
        printf("[-] %s: Invalid magic bytes: 0x%02X 0x%02X 0x%02X 0x%02X\n",
               session->client_ip, magic_bytes[0], magic_bytes[1], 
               magic_bytes[2], magic_bytes[3]);
        return DAWN_ERROR_PROTOCOL;
    }
    
    // Initialize sequence counters
    session->send_sequence = 0;
    session->receive_sequence = 0;
    
    // Brief delay for client synchronization
    usleep(50000); // 50ms
    
    // Send ACK
    result = dawn_send_ack(session->socket_fd);
    if (result != DAWN_SUCCESS) {
        printf("[-] %s: Failed to send handshake ACK\n", session->client_ip);
        return result;
    }
    
    // Brief delay after ACK
    usleep(50000); // 50ms
    
    printf("[+] %s: Handshake successful\n", session->client_ip);
    return DAWN_SUCCESS;
}

static int dawn_receive_data_chunks(dawn_client_session_t *session, uint8_t **data_out, size_t *size_out) {
    if (!session || !data_out || !size_out) return DAWN_ERROR;
    
    printf("[DEBUG] %s: Ready to receive data (max packet size: %d bytes)\n",
           session->client_ip, PACKET_MAX_SIZE);
    
    // Allocate receive buffer
    uint8_t *buffer = malloc(MAX_DATA_SIZE);
    if (!buffer) {
        printf("[-] %s: Failed to allocate receive buffer\n", session->client_ip);
        return DAWN_ERROR_MEMORY;
    }
    
    size_t total_received = 0;
    
    while (1) {
        // Read packet header
        uint8_t header_buffer[PACKET_HEADER_SIZE];
        int result = dawn_read_exact(session->socket_fd, header_buffer, PACKET_HEADER_SIZE);
        if (result != DAWN_SUCCESS) {
            printf("[-] %s: Failed to read packet header\n", session->client_ip);
            free(buffer);
            return result;
        }
        
        // Parse header
        dawn_packet_header_t header;
        result = dawn_parse_packet_header(header_buffer, &header);
        if (result != DAWN_SUCCESS) {
            printf("[-] %s: Invalid packet header\n", session->client_ip);
            dawn_send_nack(session->socket_fd);
            free(buffer);
            return result;
        }
        
        // Validate data length
        if (header.data_length > PACKET_MAX_SIZE) {
            printf("[-] %s: Packet too large (%u bytes, max: %d)\n",
                   session->client_ip, header.data_length, PACKET_MAX_SIZE);
            dawn_send_nack(session->socket_fd);
            free(buffer);
            return DAWN_ERROR_PROTOCOL;
        }
        
        if (total_received + header.data_length > MAX_DATA_SIZE) {
            printf("[-] %s: Total data exceeds maximum (%zu + %u > %d)\n",
                   session->client_ip, total_received, header.data_length, MAX_DATA_SIZE);
            dawn_send_nack(session->socket_fd);
            free(buffer);
            return DAWN_ERROR_PROTOCOL;
        }
        
        // Read sequence number (first 2 bytes)
        uint8_t seq_bytes[2];
        result = dawn_read_exact(session->socket_fd, seq_bytes, 2);
        if (result != DAWN_SUCCESS) {
            printf("[-] %s: Failed to read sequence number\n", session->client_ip);
            dawn_send_nack(session->socket_fd);
            free(buffer);
            return result;
        }
        
        uint16_t packet_sequence = ((uint16_t)seq_bytes[0] << 8) | (uint16_t)seq_bytes[1];
        
        // Verify sequence number
        if (packet_sequence != session->receive_sequence) {
            printf("[-] %s: Sequence mismatch: expected %u, got %u\n",
                   session->client_ip, session->receive_sequence, packet_sequence);
            dawn_send_nack(session->socket_fd);
            continue;
        }
        
        // Read chunk data
        uint8_t *chunk_buffer = malloc(header.data_length);
        if (!chunk_buffer) {
            printf("[-] %s: Failed to allocate chunk buffer\n", session->client_ip);
            dawn_send_nack(session->socket_fd);
            free(buffer);
            return DAWN_ERROR_MEMORY;
        }
        
        result = dawn_read_exact(session->socket_fd, chunk_buffer, header.data_length);
        if (result != DAWN_SUCCESS) {
            printf("[-] %s: Failed to read chunk data\n", session->client_ip);
            dawn_send_nack(session->socket_fd);
            free(chunk_buffer);
            free(buffer);
            return result;
        }
        
        // Verify checksum
        uint16_t actual_checksum = dawn_calculate_checksum(chunk_buffer, header.data_length);
        if (actual_checksum != header.checksum) {
            printf("[-] %s: Checksum mismatch: expected 0x%04X, got 0x%04X\n",
                   session->client_ip, header.checksum, actual_checksum);
            dawn_send_nack(session->socket_fd);
            free(chunk_buffer);
            continue;
        }
        
        // Send ACK
        result = dawn_send_ack(session->socket_fd);
        if (result != DAWN_SUCCESS) {
            printf("[-] %s: Failed to send ACK\n", session->client_ip);
            free(chunk_buffer);
            free(buffer);
            return result;
        }
        
        // Append data to buffer (sequence numbers NOT included)
        memcpy(buffer + total_received, chunk_buffer, header.data_length);
        total_received += header.data_length;
        session->receive_sequence++;
        
        // Progress report for larger packets
        if (header.data_length > 1024) {
            printf("[+] %s: Received %u bytes (total: %zu)\n",
                   session->client_ip, header.data_length, total_received);
        }
        
        free(chunk_buffer);
        
        // Check if this was the last packet
        if (header.packet_type == PACKET_TYPE_DATA_END) {
            break;
        }
    }
    
    printf("[+] %s: Successfully received %zu bytes total\n", session->client_ip, total_received);
    
    *data_out = buffer;
    *size_out = total_received;
    return DAWN_SUCCESS;
}

static int dawn_send_data_chunks(dawn_client_session_t *session, const uint8_t *data, size_t size) {
    if (!session || !data || size == 0) return DAWN_ERROR;
    
    printf("[+] %s: Sending response (%zu bytes, chunk size: %d)\n",
           session->client_ip, size, PACKET_MAX_SIZE);
    
    // Brief delay for client synchronization
    usleep(100000); // 100ms
    
    size_t total_sent = 0;
    
    while (total_sent < size) {
        size_t remaining = size - total_sent;
        size_t current_chunk_size = (remaining > PACKET_MAX_SIZE) ? PACKET_MAX_SIZE : remaining;
        int is_last_chunk = (total_sent + current_chunk_size >= size);
        
        uint8_t packet_type = is_last_chunk ? PACKET_TYPE_DATA_END : PACKET_TYPE_DATA;
        const uint8_t *chunk_data = data + total_sent;
        uint16_t checksum = dawn_calculate_checksum(chunk_data, current_chunk_size);
        
        // Build header and sequence bytes
        uint8_t header[PACKET_HEADER_SIZE];
        uint8_t sequence_bytes[2];
        
        dawn_build_packet_header(header, current_chunk_size, packet_type, checksum);
        sequence_bytes[0] = (session->send_sequence >> 8) & 0xFF;
        sequence_bytes[1] = session->send_sequence & 0xFF;
        
        // Retry logic
        int chunk_sent = 0;
        for (int retry = 0; retry < MAX_RETRIES && !chunk_sent; retry++) {
            if (retry > 0) {
                int delay_ms = 100 * (1 << retry); // Exponential backoff
                if (delay_ms > 2000) delay_ms = 2000; // Cap at 2s
                printf("[+] %s: Retry %d/%d after %dms\n",
                       session->client_ip, retry, MAX_RETRIES, delay_ms);
                usleep(delay_ms * 1000);
            }
            
            // Send header, sequence, and data
            int result = dawn_send_exact(session->socket_fd, header, PACKET_HEADER_SIZE);
            if (result != DAWN_SUCCESS) {
                printf("[-] %s: Failed to send header\n", session->client_ip);
                continue;
            }
            
            result = dawn_send_exact(session->socket_fd, sequence_bytes, 2);
            if (result != DAWN_SUCCESS) {
                printf("[-] %s: Failed to send sequence\n", session->client_ip);
                continue;
            }
            
            result = dawn_send_exact(session->socket_fd, chunk_data, current_chunk_size);
            if (result != DAWN_SUCCESS) {
                printf("[-] %s: Failed to send chunk data\n", session->client_ip);
                continue;
            }
            
            // Wait for ACK with timeout
            struct timeval timeout;
            timeout.tv_sec = 2;
            timeout.tv_usec = 0;
            setsockopt(session->socket_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
            
            uint8_t ack_header[PACKET_HEADER_SIZE];
            result = dawn_read_exact(session->socket_fd, ack_header, PACKET_HEADER_SIZE);
            
            // Reset timeout
            timeout.tv_sec = SOCKET_TIMEOUT_SEC;
            timeout.tv_usec = 0;
            setsockopt(session->socket_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
            
            if (result != DAWN_SUCCESS) {
                printf("[-] %s: No ACK received\n", session->client_ip);
                continue;
            }
            
            dawn_packet_header_t ack_info;
            result = dawn_parse_packet_header(ack_header, &ack_info);
            if (result != DAWN_SUCCESS) {
                printf("[-] %s: Invalid ACK header\n", session->client_ip);
                continue;
            }
            
            if (ack_info.packet_type == PACKET_TYPE_ACK) {
                chunk_sent = 1;
                break;
            } else if (ack_info.packet_type == PACKET_TYPE_NACK) {
                printf("[-] %s: Received NACK\n", session->client_ip);
                continue;
            }
        }
        
        if (!chunk_sent) {
            printf("[-] %s: Failed to send chunk after %d retries\n", 
                   session->client_ip, MAX_RETRIES);
            return DAWN_ERROR;
        }
        
        total_sent += current_chunk_size;
        session->send_sequence++;
        
        // Progress report
        int percent = (int)((total_sent * 100) / size);
        printf("[+] %s: Sent %zu/%zu bytes (%d%%)\n",
               session->client_ip, total_sent, size, percent);
    }
    
    printf("[+] %s: Successfully sent all data\n", session->client_ip);
    return DAWN_SUCCESS;
}

// === Client Connection Handler ===

static int dawn_handle_client_connection(int client_fd, struct sockaddr_in *client_addr) {
    dawn_client_session_t session;
    memset(&session, 0, sizeof(session));
    
    // Initialize session
    session.socket_fd = client_fd;
    session.addr = *client_addr;
    inet_ntop(AF_INET, &client_addr->sin_addr, session.client_ip, sizeof(session.client_ip));
    
    printf("[+] %s: New client connected\n", session.client_ip);
    
    // Set socket timeout
    struct timeval timeout;
    timeout.tv_sec = SOCKET_TIMEOUT_SEC;
    timeout.tv_usec = 0;
    setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(client_fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    
    int result = DAWN_SUCCESS;
    uint8_t *received_data = NULL;
    size_t received_size = 0;
    
    do {
        // Step 1: Handle handshake
        result = dawn_handle_handshake(&session);
        if (result != DAWN_SUCCESS) {
            printf("[-] %s: Handshake failed\n", session.client_ip);
            break;
        }
        
        // Step 2: Receive data
        result = dawn_receive_data_chunks(&session, &received_data, &received_size);
        if (result != DAWN_SUCCESS) {
            printf("[-] %s: Failed to receive data\n", session.client_ip);
            break;
        }
        
        printf("[INFO] %s: Received %zu bytes, preparing echo response\n",
               session.client_ip, received_size);
        
        // Step 3: Echo the data back (this is our echo server functionality)
        result = dawn_send_data_chunks(&session, received_data, received_size);
        if (result != DAWN_SUCCESS) {
            printf("[-] %s: Failed to send echo response\n", session.client_ip);
            break;
        }
        
        printf("[+] %s: Echo response sent successfully\n", session.client_ip);
        
    } while (0);
    
    // Cleanup
    if (received_data) {
        free(received_data);
    }
    
    close(client_fd);
    
    if (result == DAWN_SUCCESS) {
        printf("[+] %s: Connection completed successfully\n", session.client_ip);
    } else {
        printf("[-] %s: Connection ended with error\n", session.client_ip);
    }
    
    return result;
}

// === Server Thread ===

static void *dawn_server_thread(void *arg) {
    (void)arg; // Unused parameter
    
    printf("[INFO] DAWN Audio Protocol Echo Server starting\n");
    printf("[CONFIG] Host: %s, Port: %d\n", SERVER_HOST, SERVER_PORT);
    printf("[CONFIG] Protocol: DAP v0x%02X, Packet size: %d bytes\n", 
           PROTOCOL_VERSION, PACKET_MAX_SIZE);
    
    // Create socket
    server_socket_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_socket_fd < 0) {
        printf("[ERROR] Failed to create socket: %s\n", strerror(errno));
        server_running = 0;
        return NULL;
    }
    
    // Set socket options
    int opt = 1;
    if (setsockopt(server_socket_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        printf("[WARN] Failed to set SO_REUSEADDR: %s\n", strerror(errno));
    }
    
    // Bind socket
    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(SERVER_PORT);
    
    if (bind(server_socket_fd, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        printf("[ERROR] Failed to bind to %s:%d: %s\n", 
               SERVER_HOST, SERVER_PORT, strerror(errno));
        close(server_socket_fd);
        server_socket_fd = -1;
        server_running = 0;
        return NULL;
    }
    
    // Listen for connections
    if (listen(server_socket_fd, MAX_CLIENTS) < 0) {
        printf("[ERROR] Failed to listen: %s\n", strerror(errno));
        close(server_socket_fd);
        server_socket_fd = -1;
        server_running = 0;
        return NULL;
    }
    
    printf("[READY] Listening on %s:%d (max %d clients)\n", 
           SERVER_HOST, SERVER_PORT, MAX_CLIENTS);
    
    // Main server loop
    while (server_running) {
        struct sockaddr_in client_addr;
        socklen_t client_addr_len = sizeof(client_addr);
        
        // Accept client connection
        int client_fd = accept(server_socket_fd, (struct sockaddr*)&client_addr, &client_addr_len);
        
        if (client_fd < 0) {
            if (server_running && errno != EINTR) {
                printf("[ERROR] Accept failed: %s\n", strerror(errno));
            }
            continue;
        }
        
        // Handle client in this thread (could be improved with thread pool)
        dawn_handle_client_connection(client_fd, &client_addr);
    }
    
    // Cleanup
    if (server_socket_fd >= 0) {
        close(server_socket_fd);
        server_socket_fd = -1;
    }
    
    printf("[EXIT] Server thread stopped\n");
    return NULL;
}

// === Public API Implementation ===

int dawn_server_start(void) {
    pthread_mutex_lock(&server_mutex);
    
    if (server_running) {
        printf("[WARN] Server is already running\n");
        pthread_mutex_unlock(&server_mutex);
        return DAWN_SUCCESS;
    }
    
    server_running = 1;
    
    int result = pthread_create(&server_thread, NULL, dawn_server_thread, NULL);
    if (result != 0) {
        printf("[ERROR] Failed to create server thread: %s\n", strerror(result));
        server_running = 0;
        pthread_mutex_unlock(&server_mutex);
        return DAWN_ERROR;
    }
    
    pthread_mutex_unlock(&server_mutex);
    
    // Give the server thread a moment to initialize
    usleep(100000); // 100ms
    
    return DAWN_SUCCESS;
}

void dawn_server_stop(void) {
    pthread_mutex_lock(&server_mutex);
    
    if (!server_running) {
        pthread_mutex_unlock(&server_mutex);
        return;
    }
    
    printf("[INFO] Stopping DAWN server...\n");
    server_running = 0;
    
    // Close server socket to wake up accept()
    if (server_socket_fd >= 0) {
        close(server_socket_fd);
        server_socket_fd = -1;
    }
    
    pthread_mutex_unlock(&server_mutex);
    
    // Wait for server thread to complete
    pthread_join(server_thread, NULL);
    
    printf("[INFO] DAWN server stopped\n");
}

int dawn_server_is_running(void) {
    pthread_mutex_lock(&server_mutex);
    int running = server_running;
    pthread_mutex_unlock(&server_mutex);
    return running;
}
