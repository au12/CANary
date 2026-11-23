/* Minimal threaded HTTP server for the simulator: the JSON API from the core
 * plus the web UI straight from firmware/web. SPDX-License-Identifier: GPL-3.0-or-later */
#include "host_platform.h"

#include <arpa/inet.h>
#include <pthread.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <unistd.h>

typedef struct {
    cn_engine *e;
    const char *web_dir;
    int fd;
} conn_t;

static const char *mime(const char *path)
{
    const char *ext = strrchr(path, '.');
    if (!ext) return "application/octet-stream";
    if (!strcmp(ext, ".html")) return "text/html; charset=utf-8";
    if (!strcmp(ext, ".css")) return "text/css";
    if (!strcmp(ext, ".js")) return "application/javascript";
    if (!strcmp(ext, ".json") || !strcmp(ext, ".webmanifest")) return "application/json";
    if (!strcmp(ext, ".svg")) return "image/svg+xml";
    if (!strcmp(ext, ".png")) return "image/png";
    if (!strcmp(ext, ".ico")) return "image/x-icon";
    if (!strcmp(ext, ".woff2")) return "font/woff2";
    return "text/plain; charset=utf-8";
}

static void send_all(int fd, const char *p, size_t n)
{
    while (n) {
        ssize_t w = send(fd, p, n, MSG_NOSIGNAL);
        if (w <= 0) return;
        p += w;
        n -= (size_t)w;
    }
}

static void send_head(int fd, int status, const char *ctype, long len, const char *extra)
{
    char head[400];
    int n = snprintf(head, sizeof(head), "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %ld\r\nCache-Control: no-store\r\nConnection: close\r\n%s\r\n",
                     status, status == 200 ? "OK" : "Error", ctype, len, extra ? extra : "");
    send_all(fd, head, (size_t)n);
}

static void send_file(int fd, const char *path, const char *ctype, const char *download)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        const char *msg = "{\"ok\":false,\"error\":\"not found\"}";
        send_head(fd, 404, "application/json", (long)strlen(msg), NULL);
        send_all(fd, msg, strlen(msg));
        return;
    }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    char extra[160] = "";
    if (download && download[0]) snprintf(extra, sizeof(extra), "Content-Disposition: attachment; filename=\"%s\"\r\n", download);
    send_head(fd, 200, ctype, len, extra);
    char buf[8192];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) send_all(fd, buf, n);
    fclose(f);
}

static void *serve(void *arg)
{
    conn_t *c = arg;
    cn_buf in;
    cn_buf_init(&in);
    char tmp[4096];
    char *hdr_end = NULL;
    while (!hdr_end && in.len < 32768) {
        ssize_t n = recv(c->fd, tmp, sizeof(tmp), 0);
        if (n <= 0) break;
        cn_buf_put(&in, tmp, (size_t)n);
        hdr_end = strstr(in.p, "\r\n\r\n");
    }
    if (!hdr_end) goto done;
    size_t head_len = (size_t)(hdr_end + 4 - in.p);
    long clen = 0;
    const char *cl = strcasestr(in.p, "\r\nContent-Length:");
    if (cl && cl < hdr_end) clen = atol(cl + 17);
    while (in.len < head_len + (size_t)clen && clen < (64 << 20)) {
        ssize_t n = recv(c->fd, tmp, sizeof(tmp), 0);
        if (n <= 0) break;
        cn_buf_put(&in, tmp, (size_t)n);
    }
    char method[8], target[512];
    if (sscanf(in.p, "%7s %511s", method, target) != 2) goto done;
    char *query = strchr(target, '?');
    if (query) *query++ = 0;
    char *body = in.p + head_len;

    if (!strncmp(target, "/api/", 5)) {
        cn_api_req req = {.method = method, .path = target, .query = query, .body = clen ? body : NULL, .body_len = (size_t)clen};
        cn_api_resp resp;
        memset(&resp, 0, sizeof(resp));
        if (cn_api_handle(c->e, &req, &resp)) {
            if (resp.send_file[0]) {
                send_file(c->fd, resp.send_file, resp.content_type, resp.download_name);
            } else {
                send_head(c->fd, resp.status, resp.content_type, (long)resp.body.len, NULL);
                send_all(c->fd, resp.body.p ? resp.body.p : "", resp.body.len);
            }
        } else if (!strcmp(target, "/api/wifi/scan")) {
            const char *nets = "{\"networks\":[{\"ssid\":\"HomeNet\",\"rssi\":-48,\"secure\":true},{\"ssid\":\"HomeNet-Guest\",\"rssi\":-55,\"secure\":true},"
                               "{\"ssid\":\"Garage\",\"rssi\":-71,\"secure\":true},{\"ssid\":\"xfinitywifi\",\"rssi\":-83,\"secure\":false}]}";
            send_head(c->fd, 200, "application/json", (long)strlen(nets), NULL);
            send_all(c->fd, nets, strlen(nets));
        } else if (!strcmp(target, "/api/system")) {
            char sys[512];
            snprintf(sys, sizeof(sys),
                     "{\"fw\":\"%s\",\"build\":\"simulator\",\"idf\":\"host\",\"board\":\"%s\",\"device\":\"%s\",\"recovery\":false,"
                     "\"reset_reason\":\"power on\",\"partition\":\"ota_0\",\"heap_free\":182344,\"heap_min\":141200,\"psram_free\":7912448,"
                     "\"obd_chip\":\"MIC3624 v2.3.19\",\"sd_card\":\"SD32G 29.7 GB\",\"sd_mounted\":true,\"uptime_s\":%lld}",
                     CN_FW_VERSION, c->e->board, c->e->device_id, (long long)((c->e->pf.mono_ms(c->e->pf.ctx) - c->e->boot_mono) / 1000));
            send_head(c->fd, 200, "application/json", (long)strlen(sys), NULL);
            send_all(c->fd, sys, strlen(sys));
        } else if (!strcmp(target, "/api/ap") && !strcmp(method, "POST")) {
            const char *msg = "{\"ok\":true}";
            send_head(c->fd, 200, "application/json", (long)strlen(msg), NULL);
            send_all(c->fd, msg, strlen(msg));
        } else {
            const char *msg = "{\"ok\":false,\"error\":\"not available in the simulator\"}";
            send_head(c->fd, 404, "application/json", (long)strlen(msg), NULL);
            send_all(c->fd, msg, strlen(msg));
        }
        cn_buf_free(&resp.body);
    } else if (strstr(target, "..")) {
        send_head(c->fd, 400, "text/plain", 0, NULL);
    } else {
        char path[700];
        snprintf(path, sizeof(path), "%s%s", c->web_dir, !strcmp(target, "/") ? "/index.html" : target);
        send_file(c->fd, path, mime(path), NULL);
    }
done:
    cn_buf_free(&in);
    shutdown(c->fd, SHUT_RDWR);
    close(c->fd);
    free(c);
    return NULL;
}

int http_serve(cn_engine *e, const char *web_dir, int port)
{
    signal(SIGPIPE, SIG_IGN);
    int ls = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_port = htons((uint16_t)port), .sin_addr.s_addr = htonl(INADDR_ANY)};
    if (bind(ls, (struct sockaddr *)&addr, sizeof(addr)) != 0 || listen(ls, 32) != 0) {
        perror("listen");
        return -1;
    }
    fprintf(stderr, "CANary simulator: http://localhost:%d\n", port);
    for (;;) {
        int fd = accept(ls, NULL, NULL);
        if (fd < 0) continue;
        conn_t *c = malloc(sizeof(*c));
        c->e = e;
        c->web_dir = web_dir;
        c->fd = fd;
        pthread_t th;
        pthread_attr_t at;
        pthread_attr_init(&at);
        pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
        if (pthread_create(&th, &at, serve, c) != 0) {
            close(fd);
            free(c);
        }
        pthread_attr_destroy(&at);
    }
}
