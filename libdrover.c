#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <dlfcn.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <pthread.h>
#include <ctype.h>

#define OPTIONS_FILENAME "drover.ini"
#define PACKET_FILENAME "drover-packet.bin"

/* Embedded fallback QUIC Initial packet (1200 bytes) */
extern const unsigned char drover_packet_bin[];
extern const unsigned int drover_packet_bin_len;

typedef struct {
    int is_specified;
    int is_http;
    int is_socks5;
    int is_auth;
    char host[512];
    int port;
    char login[512];
    char password[512];
} drover_options_t;

typedef struct socket_entry {
    int fd;
    int type;
    int protocol;
    int first_send_done;
    int fake_http_proxy;
    struct socket_entry *next;
} socket_entry_t;

static drover_options_t g_options;
static socket_entry_t *g_sockets = NULL;
static pthread_mutex_t g_sock_mutex = PTHREAD_MUTEX_INITIALIZER;
static char g_config_dir[2048] = {0};
static int g_delay_ms = 50;

/* Real function pointers */
static int (*real_socket)(int domain, int type, int protocol) = NULL;
static int (*real_close)(int fd) = NULL;
static ssize_t (*real_send)(int sockfd, const void *buf, size_t len, int flags) = NULL;
static ssize_t (*real_sendto)(int sockfd, const void *buf, size_t len, int flags,
                              const struct sockaddr *dest_addr, socklen_t addrlen) = NULL;
static ssize_t (*real_sendmsg)(int sockfd, const struct msghdr *msg, int flags) = NULL;
static ssize_t (*real_recv)(int sockfd, void *buf, size_t len, int flags) = NULL;

static void trim(char *s) {
    char *p = s;
    while (isspace((unsigned char)*p)) p++;
    if (p != s) memmove(s, p, strlen(p) + 1);
    size_t l = strlen(s);
    while (l > 0 && isspace((unsigned char)s[l - 1])) {
        s[--l] = '\0';
    }
}

static void parse_proxy_url(const char *proxy_str) {
    memset(&g_options, 0, sizeof(g_options));
    if (!proxy_str || strlen(proxy_str) == 0) {
        return;
    }

    char url[512];
    snprintf(url, sizeof(url), "%s", proxy_str);
    trim(url);

    if (strlen(url) == 0) return;

    g_options.is_specified = 1;

    char *p = url;
    if (strncasecmp(p, "http://", 7) == 0) {
        g_options.is_http = 1;
        p += 7;
    } else if (strncasecmp(p, "socks5://", 9) == 0) {
        g_options.is_socks5 = 1;
        p += 9;
    } else {
        g_options.is_http = 1;
    }

    /* Check for user:pass@ */
    char *at = strchr(p, '@');
    if (at) {
        *at = '\0';
        char *colon = strchr(p, ':');
        if (colon) {
            *colon = '\0';
            snprintf(g_options.login, sizeof(g_options.login), "%s", p);
            snprintf(g_options.password, sizeof(g_options.password), "%s", colon + 1);
            g_options.is_auth = 1;
        } else {
            snprintf(g_options.login, sizeof(g_options.login), "%s", p);
            g_options.is_auth = 1;
        }
        p = at + 1;
    }

    /* host:port */
    char *colon = strchr(p, ':');
    if (colon) {
        *colon = '\0';
        snprintf(g_options.host, sizeof(g_options.host), "%s", p);
        g_options.port = atoi(colon + 1);
    } else {
        snprintf(g_options.host, sizeof(g_options.host), "%s", p);
        g_options.port = g_options.is_socks5 ? 1080 : 8080;
    }
}

static void load_options(const char *ini_path) {
    FILE *f = fopen(ini_path, "r");
    if (!f) return;

    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        trim(line);
        if (line[0] == ';' || line[0] == '#' || line[0] == '[') continue;
        char *eq = strchr(line, '=');
        if (eq) {
            *eq = '\0';
            char *key = line;
            char *val = eq + 1;
            trim(key);
            trim(val);
            if (strcasecmp(key, "proxy") == 0) {
                parse_proxy_url(val);
            } else if (strcasecmp(key, "delay") == 0) {
                int d = atoi(val);
                if (d > 0 && d <= 2000) g_delay_ms = d;
            }
        }
    }
    fclose(f);
}

static void init_drover(void) {
    static int initialized = 0;
    if (initialized) return;
    initialized = 1;

    real_socket = (int (*)(int, int, int))dlsym(RTLD_NEXT, "socket");
    real_close = (int (*)(int))dlsym(RTLD_NEXT, "close");
    real_send = (ssize_t (*)(int, const void*, size_t, int))dlsym(RTLD_NEXT, "send");
    real_sendto = (ssize_t (*)(int, const void*, size_t, int, const struct sockaddr*, socklen_t))dlsym(RTLD_NEXT, "sendto");
    real_sendmsg = (ssize_t (*)(int, const struct msghdr*, int))dlsym(RTLD_NEXT, "sendmsg");
    real_recv = (ssize_t (*)(int, void*, size_t, int))dlsym(RTLD_NEXT, "recv");

    const char *delay_env = getenv("DROVER_DELAY");
    if (delay_env) {
        int d = atoi(delay_env);
        if (d > 0 && d <= 2000) g_delay_ms = d;
    }

    const char *env_dir = getenv("DROVER_DIR");
    char path[4096];

    if (env_dir && strlen(env_dir) > 0) {
        snprintf(g_config_dir, sizeof(g_config_dir), "%s", env_dir);
        snprintf(path, sizeof(path), "%s/%s", g_config_dir, OPTIONS_FILENAME);
        if (access(path, R_OK) == 0) {
            load_options(path);
            return;
        }
    }

    const char *home = getenv("HOME");
    if (home) {
        snprintf(path, sizeof(path), "%s/.config/discord/%s", home, OPTIONS_FILENAME);
        if (access(path, R_OK) == 0) {
            snprintf(g_config_dir, sizeof(g_config_dir), "%s/.config/discord", home);
            load_options(path);
            return;
        }

        snprintf(path, sizeof(path), "%s/.config/vesktop/%s", home, OPTIONS_FILENAME);
        if (access(path, R_OK) == 0) {
            snprintf(g_config_dir, sizeof(g_config_dir), "%s/.config/vesktop", home);
            load_options(path);
            return;
        }

        snprintf(path, sizeof(path), "%s/.var/app/com.discordapp.Discord/config/discord/%s", home, OPTIONS_FILENAME);
        if (access(path, R_OK) == 0) {
            snprintf(g_config_dir, sizeof(g_config_dir), "%s/.var/app/com.discordapp.Discord/config/discord", home);
            load_options(path);
            return;
        }
    }

    if (access(OPTIONS_FILENAME, R_OK) == 0) {
        if (getcwd(g_config_dir, sizeof(g_config_dir)) != NULL) {
            load_options(OPTIONS_FILENAME);
        }
    }
}

__attribute__((constructor))
static void drover_ctor(void) {
    init_drover();
}

static socket_entry_t* get_or_create_socket(int fd, int type, int protocol) {
    pthread_mutex_lock(&g_sock_mutex);
    socket_entry_t *curr = g_sockets;
    while (curr) {
        if (curr->fd == fd) {
            pthread_mutex_unlock(&g_sock_mutex);
            return curr;
        }
        curr = curr->next;
    }
    socket_entry_t *entry = (socket_entry_t*)calloc(1, sizeof(socket_entry_t));
    if (entry) {
        entry->fd = fd;
        entry->type = type;
        entry->protocol = protocol;
        entry->next = g_sockets;
        g_sockets = entry;
    }
    pthread_mutex_unlock(&g_sock_mutex);
    return entry;
}

static socket_entry_t* find_socket(int fd) {
    pthread_mutex_lock(&g_sock_mutex);
    socket_entry_t *curr = g_sockets;
    while (curr) {
        if (curr->fd == fd) {
            pthread_mutex_unlock(&g_sock_mutex);
            return curr;
        }
        curr = curr->next;
    }
    pthread_mutex_unlock(&g_sock_mutex);
    return NULL;
}

static void remove_socket(int fd) {
    pthread_mutex_lock(&g_sock_mutex);
    socket_entry_t **curr = &g_sockets;
    while (*curr) {
        if ((*curr)->fd == fd) {
            socket_entry_t *tmp = *curr;
            *curr = (*curr)->next;
            free(tmp);
            break;
        }
        curr = &(*curr)->next;
    }
    pthread_mutex_unlock(&g_sock_mutex);
}

static socket_entry_t* ensure_udp_socket(int fd) {
    socket_entry_t *entry = find_socket(fd);
    if (!entry) {
        int type = 0;
        socklen_t optlen = sizeof(type);
        if (getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &optlen) == 0) {
            if ((type & 0xF) == SOCK_DGRAM) {
                entry = get_or_create_socket(fd, SOCK_DGRAM, 0);
            }
        }
    }
    return entry;
}

int socket(int domain, int type, int protocol) {
    init_drover();
    int fd = real_socket(domain, type, protocol);
    if (fd >= 0) {
        get_or_create_socket(fd, type & 0xF, protocol);
    }
    return fd;
}

int close(int fd) {
    init_drover();
    remove_socket(fd);
    return real_close ? real_close(fd) : -1;
}

static const char b64_table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static void base64_encode(const char *in, size_t in_len, char *out, size_t out_max) {
    size_t i, j = 0;
    for (i = 0; i < in_len; i += 3) {
        if (j + 4 >= out_max) break;
        unsigned int val = (unsigned char)in[i] << 16;
        if (i + 1 < in_len) val |= (unsigned char)in[i + 1] << 8;
        if (i + 2 < in_len) val |= (unsigned char)in[i + 2];

        out[j++] = b64_table[(val >> 18) & 0x3F];
        out[j++] = b64_table[(val >> 12) & 0x3F];
        out[j++] = (i + 1 < in_len) ? b64_table[(val >> 6) & 0x3F] : '=';
        out[j++] = (i + 2 < in_len) ? b64_table[val & 0x3F] : '=';
    }
    out[j] = '\0';
}

static int add_http_proxy_auth(void *buf, size_t len) {
    if (!g_options.is_specified || !g_options.is_http || !g_options.is_auth)
        return 0;

    char *pck = (char*)buf;
    if (memmem(pck, len, "\r\nProxy-Authorization: ", 24) != NULL)
        return 0;

    char *ua_start = (char*)memmem(pck, len, "User-Agent:", 11);
    if (!ua_start) return 0;

    char *ua_end = (char*)memmem(ua_start, len - (ua_start - pck), "\r\n", 2);
    if (!ua_end) return 0;

    size_t ua_len = ua_end - ua_start;

    char credentials[1024];
    snprintf(credentials, sizeof(credentials), "%s:%s", g_options.login, g_options.password);
    char b64[1536];
    base64_encode(credentials, strlen(credentials), b64, sizeof(b64));

    char injected[2048];
    int inj_len = snprintf(injected, sizeof(injected), "Proxy-Authorization: Basic %s", b64);
    if (inj_len < 0 || (size_t)inj_len >= sizeof(injected)) return 0;

    if (ua_len < (size_t)inj_len + 6) return 0;

    size_t filler_len = ua_len - inj_len;
    char filler[1024];
    size_t x_count = filler_len - 5;
    snprintf(filler, sizeof(filler), "\r\nX: ");
    for (size_t k = 0; k < x_count && (k + 5) < sizeof(filler) - 1; k++) {
        filler[5 + k] = 'X';
    }
    filler[filler_len] = '\0';

    snprintf(injected + inj_len, sizeof(injected) - inj_len, "%s", filler);
    if (strlen(injected) != ua_len) return 0;

    memcpy(ua_start, injected, ua_len);
    return 1;
}

static int convert_http_to_socks5(int sock, const void *buf, size_t len, int flags) {
    if (!g_options.is_specified || !g_options.is_socks5)
        return 0;

    if (len < 8) return 0;
    const char *p = (const char*)buf;
    if (strncasecmp(p, "CONNECT ", 8) != 0) return 0;

    char req[512];
    size_t copy_len = len < sizeof(req) - 1 ? len : sizeof(req) - 1;
    memcpy(req, buf, copy_len);
    req[copy_len] = '\0';

    char target_host[256] = {0};
    int target_port = 0;
    if (sscanf(req, "%*s %255[^:]:%d", target_host, &target_port) != 2) {
        return 0;
    }

    char s[64];
    s[0] = 0x05;
    s[1] = 0x01;
    s[2] = 0x00;
    if (real_send(sock, s, 3, flags) != 3) return 0;

    struct timeval tv = { .tv_sec = 10, .tv_usec = 0 };
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(sock, &fds);
    if (select(sock + 1, &fds, NULL, NULL, &tv) <= 0) return 0;

    if (real_recv(sock, s, 2, 0) != 2 || s[0] != 0x05 || s[1] != 0x00) {
        return 0;
    }

    size_t host_len = strlen(target_host);
    size_t req_len = 4 + 1 + host_len + 2;
    char connect_req[300];
    connect_req[0] = 0x05;
    connect_req[1] = 0x01;
    connect_req[2] = 0x00;
    connect_req[3] = 0x03;
    connect_req[4] = (char)host_len;
    memcpy(&connect_req[5], target_host, host_len);
    connect_req[5 + host_len] = (char)((target_port >> 8) & 0xFF);
    connect_req[5 + host_len + 1] = (char)(target_port & 0xFF);

    if (real_send(sock, connect_req, req_len, flags) != (ssize_t)req_len) {
        return 0;
    }

    socket_entry_t *entry = find_socket(sock);
    if (entry) {
        entry->fake_http_proxy = 1;
    }
    return 1;
}

static int is_discord_stun(const uint8_t *buf, size_t len) {
    if (len == 74) return 1;
    if (len >= 20 && buf) {
        /* RFC 5389 STUN Magic Cookie (0x2112A442) in big-endian */
        if (buf[4] == 0x21 && buf[5] == 0x12 && buf[6] == 0xa4 && buf[7] == 0x42) {
            /* Message type 0x0001 = Binding Request */
            if (buf[0] == 0x00 && buf[1] == 0x01) {
                return 1;
            }
        }
    }
    return 0;
}

static void send_udp_manipulation(int sockfd, const struct sockaddr *dest_addr, socklen_t addrlen) {
    char packet_path[4096];
    int packet_sent = 0;

    if (g_config_dir[0]) {
        snprintf(packet_path, sizeof(packet_path), "%s/%s", g_config_dir, PACKET_FILENAME);
    } else {
        snprintf(packet_path, sizeof(packet_path), "%s", PACKET_FILENAME);
    }

    if (access(packet_path, R_OK) == 0) {
        FILE *f = fopen(packet_path, "rb");
        if (f) {
            fseek(f, 0, SEEK_END);
            long psize = ftell(f);
            fseek(f, 0, SEEK_SET);
            if (psize > 0 && psize < 65536) {
                char *pdata = malloc(psize);
                if (pdata) {
                    if (fread(pdata, 1, psize, f) == (size_t)psize) {
                        real_sendto(sockfd, pdata, psize, 0, dest_addr, addrlen);
                        packet_sent = 1;
                    }
                    free(pdata);
                }
            }
            fclose(f);
        }
    }

    /* Fallback: Send embedded 1200-byte QUIC Initial packet if file was not read */
    if (!packet_sent && drover_packet_bin_len > 0) {
        real_sendto(sockfd, drover_packet_bin, drover_packet_bin_len, 0, dest_addr, addrlen);
    }

    /* Emulate Discord Drover exact sequence: send byte 0, byte 1, sleep 50ms */
    uint8_t payload = 0;
    real_sendto(sockfd, &payload, 1, 0, dest_addr, addrlen);
    payload = 1;
    real_sendto(sockfd, &payload, 1, 0, dest_addr, addrlen);
    usleep(g_delay_ms > 0 ? (useconds_t)g_delay_ms * 1000 : 50000);
}

ssize_t send(int sockfd, const void *buf, size_t len, int flags) {
    init_drover();
    socket_entry_t *entry = find_socket(sockfd);
    if (entry && !entry->first_send_done) {
        entry->first_send_done = 1;
        if (entry->type == SOCK_STREAM) {
            add_http_proxy_auth((void*)buf, len);
            if (convert_http_to_socks5(sockfd, buf, len, flags)) {
                return len;
            }
        }
    }
    return real_send(sockfd, buf, len, flags);
}

ssize_t sendto(int sockfd, const void *buf, size_t len, int flags,
               const struct sockaddr *dest_addr, socklen_t addrlen) {
    init_drover();
    socket_entry_t *entry = ensure_udp_socket(sockfd);

    if (entry && !entry->first_send_done) {
        entry->first_send_done = 1;
        if (entry->type == SOCK_DGRAM && is_discord_stun((const uint8_t*)buf, len)) {
            send_udp_manipulation(sockfd, dest_addr, addrlen);
        }
    }
    return real_sendto(sockfd, buf, len, flags, dest_addr, addrlen);
}

ssize_t sendmsg(int sockfd, const struct msghdr *msg, int flags) {
    init_drover();
    socket_entry_t *entry = ensure_udp_socket(sockfd);

    if (entry && !entry->first_send_done && msg && msg->msg_iov && msg->msg_iovlen > 0) {
        entry->first_send_done = 1;

        size_t total_len = 0;
        for (size_t i = 0; i < (size_t)msg->msg_iovlen; i++) {
            total_len += msg->msg_iov[i].iov_len;
        }

        uint8_t header[24] = {0};
        size_t copied = 0;
        for (size_t i = 0; i < (size_t)msg->msg_iovlen && copied < sizeof(header); i++) {
            size_t to_copy = msg->msg_iov[i].iov_len;
            if (copied + to_copy > sizeof(header)) {
                to_copy = sizeof(header) - copied;
            }
            if (msg->msg_iov[i].iov_base && to_copy > 0) {
                memcpy(header + copied, msg->msg_iov[i].iov_base, to_copy);
                copied += to_copy;
            }
        }

        if (entry->type == SOCK_DGRAM && is_discord_stun(header, total_len)) {
            const struct sockaddr *dest_addr = (const struct sockaddr *)msg->msg_name;
            socklen_t addrlen = msg->msg_namelen;

            /* If msg_name is null, check getpeername() for connected UDP sockets */
            struct sockaddr_storage peer;
            socklen_t peer_len = sizeof(peer);
            if (!dest_addr && getpeername(sockfd, (struct sockaddr *)&peer, &peer_len) == 0) {
                dest_addr = (struct sockaddr *)&peer;
                addrlen = peer_len;
            }

            if (dest_addr && addrlen > 0) {
                send_udp_manipulation(sockfd, dest_addr, addrlen);
            }
        }
    }
    return real_sendmsg(sockfd, msg, flags);
}

ssize_t recv(int sockfd, void *buf, size_t len, int flags) {
    init_drover();
    ssize_t res = real_recv(sockfd, buf, len, flags);
    if (res > 0) {
        socket_entry_t *entry = find_socket(sockfd);
        if (entry && entry->fake_http_proxy) {
            entry->fake_http_proxy = 0;
            if (res >= 10) {
                unsigned char *p = (unsigned char*)buf;
                if (p[0] == 0x05 && p[1] == 0x00 && p[2] == 0x00) {
                    const char *resp = "HTTP/1.1 200 Connection Established\r\n\r\n";
                    size_t rlen = strlen(resp);
                    if (rlen <= len) {
                        memcpy(buf, resp, rlen);
                        return rlen;
                    }
                }
            }
        }
    }
    return res;
}
