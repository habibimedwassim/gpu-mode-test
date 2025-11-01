#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <errno.h>
#include <sys/stat.h>
#include <stdarg.h>

#define SOCKET_PATH "/run/gpu-mode.sock"
#define STATE_FILE  "/var/lib/gpu-mode/state"
#define LOG_TAG     "gpu-mode"
#define MAX_CMD_LEN 128

// ---------- Logging ----------
static void log_msg(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    char msg[256];
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "logger -t %s \"%s\"", LOG_TAG, msg);
    system(cmd);
}

// ---------- Run shell command ----------
static int run_cmd(const char *cmd) {
    int ret = system(cmd);
    if (ret != 0)
        fprintf(stderr, "Command failed: %s\n", cmd);
    return ret;
}

// ---------- Clocks ----------
static int get_mem_clocks(int *clocks, int max) {
    FILE *fp = popen("nvidia-smi --query-supported-clocks=memory --format=csv,noheader,nounits", "r");
    if (!fp) return -1;
    int i = 0;
    while (i < max && fscanf(fp, "%d", &clocks[i]) == 1) i++;
    pclose(fp);
    return i;
}

static int get_current_mem_clock(void) {
    FILE *fp = popen("nvidia-smi --query-gpu=clocks.mem --format=csv,noheader,nounits", "r");
    if (!fp) return -1;
    int clk = 0;
    fscanf(fp, "%d", &clk);
    pclose(fp);
    return clk;
}

// ---------- Persistent mode ----------
static void ensure_state_dir(void) {
    mkdir("/var/lib/gpu-mode", 0755);
}

static void save_mode(const char *mode) {
    ensure_state_dir();
    FILE *f = fopen(STATE_FILE, "w");
    if (!f) return;
    fprintf(f, "%s\n", mode);
    fclose(f);
}

static void read_mode(char *buf, size_t len) {
    FILE *f = fopen(STATE_FILE, "r");
    if (!f) {
        strncpy(buf, "auto", len);
        return;
    }
    if (!fgets(buf, len, f))
        strncpy(buf, "auto", len);
    buf[strcspn(buf, "\n")] = 0;
    fclose(f);
}

// ---------- Apply mode ----------
static void apply_mode(const char *mode, int max, int mid, int low) {
    if (strcmp(mode, "performance") == 0) {
        run_cmd("nvidia-smi -pm 1");
        char cmd[128];
        snprintf(cmd, sizeof(cmd), "nvidia-smi --lock-memory-clocks=%d,%d", max, max);
        run_cmd(cmd);
    } else if (strcmp(mode, "balanced") == 0) {
        run_cmd("nvidia-smi -pm 1");
        char cmd[128];
        snprintf(cmd, sizeof(cmd), "nvidia-smi --lock-memory-clocks=%d,%d", mid, mid);
        run_cmd(cmd);
    } else if (strcmp(mode, "powersaver") == 0) {
        run_cmd("nvidia-smi -pm 1");
        char cmd[128];
        snprintf(cmd, sizeof(cmd), "nvidia-smi --lock-memory-clocks=%d,%d", low, low);
        run_cmd(cmd);
    } else { // auto/reset
        run_cmd("nvidia-smi --reset-memory-clocks");
    }
}

// ---------- Main ----------
int main(void) {
    unlink(SOCKET_PATH);
    int sockfd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sockfd < 0) { perror("socket"); exit(1); }

    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path) - 1);

    if (bind(sockfd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind");
        exit(1);
    }

    chmod(SOCKET_PATH, 0666);
    if (listen(sockfd, 5) < 0) { perror("listen"); exit(1); }

    log_msg("GPU Mode Daemon started");
    printf("gpu-mode daemon running...\n");

    // --- Initialize state and apply last mode ---
    int clocks[16];
    int count = get_mem_clocks(clocks, 16);
    int max = clocks[0];
    int mid = clocks[count / 2];
    int low = clocks[count - 1];
    char last_mode[32];
    read_mode(last_mode, sizeof(last_mode));
    apply_mode(last_mode, max, mid, low);
    log_msg("Restored last mode: %s", last_mode);

    // --- Command loop ---
    while (1) {
        int client = accept(sockfd, NULL, NULL);
        if (client < 0) { perror("accept"); continue; }

        char buf[MAX_CMD_LEN];
        int n = read(client, buf, sizeof(buf) - 1);
        if (n <= 0) { close(client); continue; }
        buf[n] = '\0';

        int cur = get_current_mem_clock();

        if (strcmp(buf, "status") == 0) {
            char mode[32];
            read_mode(mode, sizeof(mode));

            char resp[160];
            snprintf(resp, sizeof(resp),
                     "Current clock: %d MHz\nMode: %s\n", cur, mode);
            write(client, resp, strlen(resp));
        }
        else if (!strcmp(buf, "performance") ||
                 !strcmp(buf, "balanced") ||
                 !strcmp(buf, "powersaver")) {
            apply_mode(buf, max, mid, low);
            save_mode(buf);
            log_msg("Switched to %s mode", buf);
            write(client, "OK\n", 3);
        }
        else if (strcmp(buf, "reset") == 0) {
            apply_mode("auto", max, mid, low);
            save_mode("auto");
            log_msg("Reset to auto mode");
            write(client, "OK\n", 3);
        }
        else {
            write(client, "Unknown command\n", 16);
        }

        close(client);
    }

    close(sockfd);
    return 0;
}
