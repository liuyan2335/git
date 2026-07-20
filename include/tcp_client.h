/**
 * tcp_client.h - Embedded TCP Client for RK3568
 *
 * Lightweight TCP client that connects to a remote server (Ubuntu).
 * Handles connection management, data sending/receiving,
 * heartbeats, and basic reconnection logic.
 */
#ifndef TCP_CLIENT_H
#define TCP_CLIENT_H

#include <stdint.h>
#include <sys/socket.h>

/* Default connection parameters */
#define TCP_DEFAULT_PORT        8888
#define TCP_DEFAULT_IP          "192.168.1.100"
#define TCP_RECV_BUFFER_SIZE    4096
#define TCP_SEND_BUFFER_SIZE    4096

/* Protocol message types */
#define MSG_TYPE_AUDIO          0x01    /* audio data (PCM) */
#define MSG_TYPE_TEXT            0x02    /* plain text command/message */
#define MSG_TYPE_STATUS          0x03    /* status update */
#define MSG_TYPE_HEARTBEAT       0x04    /* keep-alive */
#define MSG_TYPE_AI_RESPONSE     0x05    /* AI reply from server */
#define MSG_TYPE_ERROR           0xFF    /* error message */

/* Connection states */
typedef enum {
    TCP_STATE_DISCONNECTED = 0,
    TCP_STATE_CONNECTING,
    TCP_STATE_CONNECTED,
    TCP_STATE_ERROR,
} tcp_state_t;

/* Protocol packet header (fixed size, sent before payload) */
#pragma pack(push, 1)
typedef struct {
    uint8_t  msg_type;         /* see MSG_TYPE_* */
    uint32_t payload_len;      /* payload size in bytes (network byte order) */
    uint32_t seq_num;          /* sequence number for ordering */
    uint16_t checksum;         /* simple XOR checksum of payload */
} tcp_packet_header_t;
#pragma pack(pop)

/* TCP client context */
typedef struct {
    int sockfd;
    tcp_state_t state;
    char server_ip[32];
    int server_port;
    uint32_t seq_num;
    /* receive buffer */
    uint8_t recv_buf[TCP_RECV_BUFFER_SIZE];
    int recv_buf_len;
    /* statistics */
    uint64_t bytes_sent;
    uint64_t bytes_recv;
    uint64_t packets_sent;
    uint64_t packets_recv;
    int reconnect_count;
} tcp_client_t;

/* Callback for received messages */
typedef void (*tcp_recv_callback_t)(uint8_t msg_type, const uint8_t *payload, uint32_t len, void *user_data);

/* ----- API functions ----- */

/**
 * Initialize the TCP client context.
 */
void tcp_client_init(tcp_client_t *client);

/**
 * Set the target server address and port.
 */
void tcp_client_set_server(tcp_client_t *client, const char *ip, int port);

/**
 * Connect to the server. Blocks until connected or timeout (5s).
 * Returns 0 on success, -1 on failure.
 */
int tcp_client_connect(tcp_client_t *client);

/**
 * Disconnect from the server gracefully.
 */
void tcp_client_disconnect(tcp_client_t *client);

/**
 * Send raw data with a message type header.
 * The header is built internally and prepended.
 * Returns bytes sent (including header), or -1 on error.
 */
int tcp_client_send(tcp_client_t *client, uint8_t msg_type, const uint8_t *data, uint32_t len);

/**
 * Send a text message (convenience wrapper).
 */
int tcp_client_send_text(tcp_client_t *client, const char *text);

/**
 * Receive data (non-blocking poll, then read if available).
 * Parses packet headers, reassembles payload.
 * Calls callback for each complete message.
 * Returns number of messages processed, or -1 on error/disconnect.
 */
int tcp_client_poll(tcp_client_t *client, tcp_recv_callback_t callback, void *user_data);

/**
 * Send a heartbeat packet. Should be called periodically (e.g. every 10s).
 */
int tcp_client_heartbeat(tcp_client_t *client);

/**
 * Check if client is currently connected.
 */
int tcp_client_is_connected(tcp_client_t *client);

/**
 * Attempt reconnection with exponential backoff.
 * Returns 0 on success, -1 if all attempts failed.
 */
int tcp_client_reconnect(tcp_client_t *client, int max_attempts);

/**
 * Get a human-readable state string.
 */
const char *tcp_client_state_str(tcp_client_t *client);

/**
 * Compute simple XOR checksum over a buffer.
 */
uint16_t tcp_checksum(const uint8_t *data, uint32_t len);

#endif /* TCP_CLIENT_H */
