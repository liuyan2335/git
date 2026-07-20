/**
 * tcp_client.c - Embedded TCP Client for RK3568
 *
 * Lightweight TCP client implementation with protocol framing,
 * heartbeat keep-alive, and reconnection logic.
 * Designed for resource-constrained embedded Linux devices.
 *
 * Compile: aarch64-linux-gcc -o tcp_client tcp_client.c -static
 * Usage:   ./tcp_client [server_ip] [server_port]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <signal.h>
#include <time.h>
#include <poll.h>

#include "tcp_client.h"

/* ================================================================
 *  Checksum
 * ================================================================ */

uint16_t tcp_checksum(const uint8_t *data, uint32_t len)
{
    uint16_t checksum = 0;
    for (uint32_t i = 0; i < len; i++) {
        checksum ^= (uint16_t)data[i] << ((i % 2) * 8);
    }
    return checksum;
}

/* ================================================================
 *  Initialization & Configuration
 * ================================================================ */

void tcp_client_init(tcp_client_t *client)
{
    if (!client) return;
    memset(client, 0, sizeof(tcp_client_t));
    client->sockfd = -1;
    client->state = TCP_STATE_DISCONNECTED;
    strncpy(client->server_ip, TCP_DEFAULT_IP, sizeof(client->server_ip) - 1);
    client->server_port = TCP_DEFAULT_PORT;
    client->recv_buf_len = 0;
}

void tcp_client_set_server(tcp_client_t *client, const char *ip, int port)
{
    if (!client) return;
    if (ip) {
        strncpy(client->server_ip, ip, sizeof(client->server_ip) - 1);
    }
    if (port > 0 && port < 65536) {
        client->server_port = port;
    }
}

/* ================================================================
 *  Connection Management
 * ================================================================ */

int tcp_client_connect(tcp_client_t *client)
{
    if (!client) return -1;

    client->state = TCP_STATE_CONNECTING;

    /* Create socket */
    client->sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (client->sockfd < 0) {
        fprintf(stderr, "[tcp_client] ERROR: socket() failed: %s\n", strerror(errno));
        client->state = TCP_STATE_ERROR;
        return -1;
    }

    /* Set TCP_NODELAY for lower latency (disable Nagle) */
    int optval = 1;
    setsockopt(client->sockfd, IPPROTO_TCP, TCP_NODELAY,
               (const void *)&optval, sizeof(optval));

    /* Set keep-alive */
    optval = 1;
    setsockopt(client->sockfd, SOL_SOCKET, SO_KEEPALIVE,
               (const void *)&optval, sizeof(optval));

    /* Build server address */
    struct sockaddr_in serv_addr;
    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(client->server_port);

    if (inet_pton(AF_INET, client->server_ip, &serv_addr.sin_addr) <= 0) {
        /* Try hostname resolution */
        struct hostent *he = gethostbyname(client->server_ip);
        if (!he) {
            fprintf(stderr, "[tcp_client] ERROR: Invalid IP/hostname: %s\n",
                    client->server_ip);
            close(client->sockfd);
            client->sockfd = -1;
            client->state = TCP_STATE_ERROR;
            return -1;
        }
        memcpy(&serv_addr.sin_addr, he->h_addr_list[0], he->h_length);
    }

    printf("[tcp_client] Connecting to %s:%d...\n",
           client->server_ip, client->server_port);

    /* Connect with timeout using non-blocking socket */
    int flags = fcntl(client->sockfd, F_GETFL, 0);
    fcntl(client->sockfd, F_SETFL, flags | O_NONBLOCK);

    int ret = connect(client->sockfd, (struct sockaddr *)&serv_addr, sizeof(serv_addr));

    if (ret < 0 && errno != EINPROGRESS) {
        fprintf(stderr, "[tcp_client] ERROR: connect() failed: %s\n", strerror(errno));
        close(client->sockfd);
        client->sockfd = -1;
        client->state = TCP_STATE_ERROR;
        return -1;
    }

    /* Wait for connection with 5-second timeout */
    if (errno == EINPROGRESS) {
        struct pollfd pfd;
        pfd.fd = client->sockfd;
        pfd.events = POLLOUT;

        ret = poll(&pfd, 1, 5000);
        if (ret <= 0) {
            fprintf(stderr, "[tcp_client] ERROR: Connection timeout.\n");
            close(client->sockfd);
            client->sockfd = -1;
            client->state = TCP_STATE_ERROR;
            return -1;
        }

        /* Check if connection succeeded */
        int so_error = 0;
        socklen_t len = sizeof(so_error);
        getsockopt(client->sockfd, SOL_SOCKET, SO_ERROR, &so_error, &len);
        if (so_error != 0) {
            fprintf(stderr, "[tcp_client] ERROR: Connection failed: %s\n",
                    strerror(so_error));
            close(client->sockfd);
            client->sockfd = -1;
            client->state = TCP_STATE_ERROR;
            return -1;
        }
    }

    /* Restore blocking mode */
    fcntl(client->sockfd, F_SETFL, flags);

    client->state = TCP_STATE_CONNECTED;
    client->seq_num = 0;
    client->reconnect_count = 0;

    printf("[tcp_client] Connected to %s:%d (fd=%d)\n",
           client->server_ip, client->server_port, client->sockfd);
    return 0;
}

void tcp_client_disconnect(tcp_client_t *client)
{
    if (!client) return;
    if (client->sockfd >= 0) {
        shutdown(client->sockfd, SHUT_RDWR);
        close(client->sockfd);
        client->sockfd = -1;
    }
    client->state = TCP_STATE_DISCONNECTED;
    printf("[tcp_client] Disconnected.\n");
}

int tcp_client_is_connected(tcp_client_t *client)
{
    return (client && client->state == TCP_STATE_CONNECTED);
}

const char *tcp_client_state_str(tcp_client_t *client)
{
    if (!client) return "NULL";
    switch (client->state) {
    case TCP_STATE_DISCONNECTED: return "DISCONNECTED";
    case TCP_STATE_CONNECTING:   return "CONNECTING";
    case TCP_STATE_CONNECTED:    return "CONNECTED";
    case TCP_STATE_ERROR:        return "ERROR";
    default:                     return "UNKNOWN";
    }
}

/* ================================================================
 *  Data Sending
 * ================================================================ */

static int send_all(int sockfd, const uint8_t *buf, int len)
{
    int total = 0;
    int remaining = len;

    while (total < len) {
        int n = send(sockfd, buf + total, remaining, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) break;
        total += n;
        remaining -= n;
    }
    return total;
}

int tcp_client_send(tcp_client_t *client, uint8_t msg_type,
                     const uint8_t *data, uint32_t len)
{
    if (!client || client->state != TCP_STATE_CONNECTED) return -1;

    /* Build packet header */
    tcp_packet_header_t hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.msg_type = msg_type;
    hdr.payload_len = htonl(len);
    hdr.seq_num = htonl(client->seq_num++);
    hdr.checksum = data ? tcp_checksum(data, len) : 0;

    /* Send header */
    int ret = send_all(client->sockfd, (const uint8_t *)&hdr, sizeof(hdr));
    if (ret < 0) {
        fprintf(stderr, "[tcp_client] ERROR: send header failed: %s\n",
                strerror(errno));
        client->state = TCP_STATE_ERROR;
        return -1;
    }

    /* Send payload if any */
    if (data && len > 0) {
        ret = send_all(client->sockfd, data, len);
        if (ret < 0) {
            fprintf(stderr, "[tcp_client] ERROR: send payload failed: %s\n",
                    strerror(errno));
            client->state = TCP_STATE_ERROR;
            return -1;
        }
    }

    client->bytes_sent += sizeof(hdr) + len;
    client->packets_sent++;

    return sizeof(hdr) + len;
}

int tcp_client_send_text(tcp_client_t *client, const char *text)
{
    if (!text) return -1;
    return tcp_client_send(client, MSG_TYPE_TEXT,
                           (const uint8_t *)text, strlen(text));
}

int tcp_client_heartbeat(tcp_client_t *client)
{
    uint8_t marker = 0xFF;
    return tcp_client_send(client, MSG_TYPE_HEARTBEAT, &marker, 1);
}

/* ================================================================
 *  Data Receiving
 * ================================================================ */

static int recv_exact(int sockfd, uint8_t *buf, int len, int timeout_ms)
{
    struct pollfd pfd;
    pfd.fd = sockfd;
    pfd.events = POLLIN;

    int total = 0;
    while (total < len) {
        int ret = poll(&pfd, 1, timeout_ms);
        if (ret < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (ret == 0) return total;  /* timeout with partial data */

        int n = recv(sockfd, buf + total, len - total, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) return -1;  /* connection closed */
        total += n;
    }
    return total;
}

int tcp_client_poll(tcp_client_t *client, tcp_recv_callback_t callback,
                     void *user_data)
{
    if (!client || client->state != TCP_STATE_CONNECTED) return -1;

    int messages_processed = 0;

    /* Check if data is available (non-blocking) */
    struct pollfd pfd;
    pfd.fd = client->sockfd;
    pfd.events = POLLIN;

    while (1) {
        int ret = poll(&pfd, 1, 10);  /* 10ms timeout */
        if (ret < 0) {
            if (errno == EINTR) continue;
            fprintf(stderr, "[tcp_client] ERROR: poll() failed: %s\n",
                    strerror(errno));
            return -1;
        }
        if (ret == 0) break;  /* no data available */
        if (!(pfd.revents & POLLIN)) break;

        /* Read header first */
        tcp_packet_header_t hdr;
        int n = recv_exact(client->sockfd, (uint8_t *)&hdr, sizeof(hdr), 2000);
        if (n < 0) {
            fprintf(stderr, "[tcp_client] ERROR: Connection lost.\n");
            client->state = TCP_STATE_ERROR;
            return -1;
        }
        if (n < (int)sizeof(hdr)) break;  /* partial header, wait for more */

        uint32_t payload_len = ntohl(hdr.payload_len);

        /* Sanity check */
        if (payload_len > TCP_RECV_BUFFER_SIZE - sizeof(hdr)) {
            fprintf(stderr, "[tcp_client] ERROR: Oversized payload (%u bytes), "
                    "closing connection.\n", payload_len);
            client->state = TCP_STATE_ERROR;
            return -1;
        }

        /* Read payload */
        uint8_t payload[TCP_RECV_BUFFER_SIZE];
        if (payload_len > 0) {
            n = recv_exact(client->sockfd, payload, payload_len, 2000);
            if (n < 0) {
                fprintf(stderr, "[tcp_client] ERROR: Connection lost during payload.\n");
                client->state = TCP_STATE_ERROR;
                return -1;
            }
            if (n < (int)payload_len) {
                client->recv_buf_len = 0;
                break;  /* not enough data yet */
            }
        }

        /* Verify checksum */
        uint16_t calc_checksum = tcp_checksum(payload, payload_len);
        if (calc_checksum != hdr.checksum && payload_len > 0) {
            fprintf(stderr, "[tcp_client] WARNING: Checksum mismatch "
                    "(expected 0x%04X, got 0x%04X)\n",
                    hdr.checksum, calc_checksum);
            /* Continue anyway — best-effort delivery */
        }

        client->bytes_recv += sizeof(hdr) + payload_len;
        client->packets_recv++;
        messages_processed++;

        /* Dispatch to callback */
        if (callback) {
            callback(hdr.msg_type, payload_len > 0 ? payload : NULL,
                     payload_len, user_data);
        }
    }

    return messages_processed;
}

/* ================================================================
 *  Reconnection Logic
 * ================================================================ */

int tcp_client_reconnect(tcp_client_t *client, int max_attempts)
{
    if (!client) return -1;

    tcp_client_disconnect(client);

    for (int attempt = 1; attempt <= max_attempts; attempt++) {
        int delay = attempt * 2;  /* exponential-ish backoff: 2, 4, 6, ... */
        if (delay > 30) delay = 30;

        printf("[tcp_client] Reconnect attempt %d/%d (waiting %ds)...\n",
               attempt, max_attempts, delay);
        sleep(delay);

        if (tcp_client_connect(client) == 0) {
            client->reconnect_count++;
            printf("[tcp_client] Reconnected successfully after %d attempt(s).\n",
                   attempt);
            return 0;
        }
    }

    fprintf(stderr, "[tcp_client] ERROR: All %d reconnect attempts failed.\n",
            max_attempts);
    return -1;
}

/* ================================================================
 *  Standalone Demo: Interactive TCP client
 * ================================================================ */

#ifdef TCP_CLIENT_STANDALONE

static void on_receive(uint8_t msg_type, const uint8_t *payload,
                       uint32_t len, void *user_data)
{
    (void)user_data;

    switch (msg_type) {
    case MSG_TYPE_AI_RESPONSE:
        printf("[server] AI Response: %.*s\n", (int)len, payload);
        break;
    case MSG_TYPE_STATUS:
        printf("[server] Status: %.*s\n", (int)len, payload);
        break;
    case MSG_TYPE_ERROR:
        printf("[server] Error: %.*s\n", (int)len, payload);
        break;
    case MSG_TYPE_HEARTBEAT:
        printf("[server] Heartbeat ACK\n");
        break;
    default:
        printf("[server] Unknown msg type 0x%02X, len=%u\n", msg_type, len);
        break;
    }
}

int main(int argc, char *argv[])
{
    tcp_client_t client;
    const char *ip = (argc > 1) ? argv[1] : TCP_DEFAULT_IP;
    int port = (argc > 2) ? atoi(argv[2]) : TCP_DEFAULT_PORT;

    printf("=== RK3568 TCP Client Demo ===\n");

    tcp_client_init(&client);
    tcp_client_set_server(&client, ip, port);

    if (tcp_client_connect(&client) < 0) {
        fprintf(stderr, "Failed to connect. Exiting.\n");
        return 1;
    }

    printf("Connected. Type messages (Ctrl+C to exit):\n");

    /* Send a greeting */
    tcp_client_send_text(&client, "HELLO from RK3568");

    while (1) {
        /* Poll for incoming messages */
        tcp_client_poll(&client, on_receive, NULL);

        /* Check for user input (non-blocking) */
        struct pollfd pfd;
        pfd.fd = STDIN_FILENO;
        pfd.events = POLLIN;

        if (poll(&pfd, 1, 100) > 0 && (pfd.revents & POLLIN)) {
            char line[1024];
            if (fgets(line, sizeof(line), stdin)) {
                /* Remove trailing newline */
                size_t len = strlen(line);
                if (len > 0 && line[len - 1] == '\n') {
                    line[len - 1] = '\0';
                }
                if (strlen(line) > 0) {
                    if (strcmp(line, "/quit") == 0 || strcmp(line, "/q") == 0) {
                        break;
                    }
                    if (strcmp(line, "/ping") == 0) {
                        tcp_client_heartbeat(&client);
                    } else {
                        tcp_client_send_text(&client, line);
                    }
                }
            }
        }

        /* Heartbeat every 10 seconds */
        static time_t last_hb = 0;
        time_t now = time(NULL);
        if (now - last_hb >= 10) {
            tcp_client_heartbeat(&client);
            last_hb = now;
        }
    }

    printf("\nSent: %llu packets, %llu bytes | Recv: %llu packets, %llu bytes\n",
           client.packets_sent, client.bytes_sent,
           client.packets_recv, client.bytes_recv);

    tcp_client_disconnect(&client);
    return 0;
}

#endif /* TCP_CLIENT_STANDALONE */
