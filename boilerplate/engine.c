/*
 * engine.c - Supervised Multi-Container Runtime (User Space)
 *
 * Full implementation:
 *   - UNIX domain socket control plane (Path B)
 *   - clone() with CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS (Task 1)
 *   - chroot + /proc mount inside each container
 *   - pipe-based logging (Path A) with bounded buffer (Task 3)
 *   - producer thread per container, single consumer/logger thread
 *   - SIGCHLD reaping, SIGINT/SIGTERM graceful shutdown (Task 2)
 *   - ioctl registration with kernel monitor (Task 4)
 *   - stop_requested flag for termination attribution
 *   - run command blocks until container exits (Task 2)
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "monitor_ioctl.h"

/* ------------------------------------------------------------------ */
/* Constants                                                           */
/* ------------------------------------------------------------------ */
#define STACK_SIZE          (1024 * 1024)
#define CONTAINER_ID_LEN    32
#define CONTROL_PATH        "/tmp/mini_runtime.sock"
#define LOG_DIR             "logs"
#define CONTROL_MESSAGE_LEN 256
#define CHILD_COMMAND_LEN   256
#define LOG_CHUNK_SIZE      4096
#define LOG_BUFFER_CAPACITY 16
#define DEFAULT_SOFT_LIMIT  (40UL << 20)
#define DEFAULT_HARD_LIMIT  (64UL << 20)
#define MONITOR_DEV         "/dev/container_monitor"

/* ------------------------------------------------------------------ */
/* Types                                                               */
/* ------------------------------------------------------------------ */
typedef enum {
    CMD_SUPERVISOR = 0,
    CMD_START,
    CMD_RUN,
    CMD_PS,
    CMD_LOGS,
    CMD_STOP
} command_kind_t;

typedef enum {
    CONTAINER_STARTING = 0,
    CONTAINER_RUNNING,
    CONTAINER_STOPPED,
    CONTAINER_KILLED,
    CONTAINER_EXITED
} container_state_t;

typedef struct container_record {
    char id[CONTAINER_ID_LEN];
    pid_t host_pid;
    time_t started_at;
    container_state_t state;
    unsigned long soft_limit_bytes;
    unsigned long hard_limit_bytes;
    int exit_code;
    int exit_signal;
    int stop_requested;        /* set before sending SIGTERM/SIGKILL via stop */
    char log_path[PATH_MAX];
    struct container_record *next;
} container_record_t;

typedef struct {
    char container_id[CONTAINER_ID_LEN];
    size_t length;
    char data[LOG_CHUNK_SIZE];
} log_item_t;

typedef struct {
    log_item_t items[LOG_BUFFER_CAPACITY];
    size_t head;
    size_t tail;
    size_t count;
    int shutting_down;
    pthread_mutex_t mutex;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;
} bounded_buffer_t;

typedef struct {
    command_kind_t kind;
    char container_id[CONTAINER_ID_LEN];
    char rootfs[PATH_MAX];
    char command[CHILD_COMMAND_LEN];
    unsigned long soft_limit_bytes;
    unsigned long hard_limit_bytes;
    int nice_value;
} control_request_t;

typedef struct {
    int status;          /* 0 = ok, -1 = error, 1 = more data, 2 = end-of-log */
    int exit_code;       /* for CMD_RUN: final container exit code */
    char message[CONTROL_MESSAGE_LEN];
} control_response_t;

typedef struct {
    char id[CONTAINER_ID_LEN];
    char rootfs[PATH_MAX];
    char command[CHILD_COMMAND_LEN];
    int nice_value;
    int log_write_fd;    /* write end of log pipe; child dup2s stdout/stderr here */
} child_config_t;

typedef struct {
    int pipe_read_fd;
    char container_id[CONTAINER_ID_LEN];
    bounded_buffer_t *buffer;
} producer_arg_t;

typedef struct {
    int server_fd;
    int monitor_fd;
    volatile int should_stop;
    pthread_t logger_thread;
    bounded_buffer_t log_buffer;
    pthread_mutex_t metadata_lock;
    container_record_t *containers;
} supervisor_ctx_t;

/* ------------------------------------------------------------------ */
/* Global supervisor context pointer (needed in signal handlers)      */
/* ------------------------------------------------------------------ */
static supervisor_ctx_t *g_ctx = NULL;

/* ------------------------------------------------------------------ */
/* Utility                                                             */
/* ------------------------------------------------------------------ */
static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage:\n"
            "  %s supervisor <base-rootfs>\n"
            "  %s start <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n"
            "  %s run   <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n"
            "  %s ps\n"
            "  %s logs <id>\n"
            "  %s stop <id>\n",
            prog, prog, prog, prog, prog, prog);
}

static const char *state_to_string(container_state_t state)
{
    switch (state) {
    case CONTAINER_STARTING: return "starting";
    case CONTAINER_RUNNING:  return "running";
    case CONTAINER_STOPPED:  return "stopped";
    case CONTAINER_KILLED:   return "killed";
    case CONTAINER_EXITED:   return "exited";
    default:                 return "unknown";
    }
}

static int parse_mib_flag(const char *flag, const char *value,
                          unsigned long *target_bytes)
{
    char *end = NULL;
    unsigned long mib;
    errno = 0;
    mib = strtoul(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0') {
        fprintf(stderr, "Invalid value for %s: %s\n", flag, value);
        return -1;
    }
    if (mib > ULONG_MAX / (1UL << 20)) {
        fprintf(stderr, "Value for %s is too large: %s\n", flag, value);
        return -1;
    }
    *target_bytes = mib * (1UL << 20);
    return 0;
}

static int parse_optional_flags(control_request_t *req, int argc,
                                char *argv[], int start_index)
{
    int i;
    for (i = start_index; i < argc; i += 2) {
        char *end = NULL;
        long nv;
        if (i + 1 >= argc) {
            fprintf(stderr, "Missing value for option: %s\n", argv[i]);
            return -1;
        }
        if (strcmp(argv[i], "--soft-mib") == 0) {
            if (parse_mib_flag("--soft-mib", argv[i+1], &req->soft_limit_bytes) != 0)
                return -1;
            continue;
        }
        if (strcmp(argv[i], "--hard-mib") == 0) {
            if (parse_mib_flag("--hard-mib", argv[i+1], &req->hard_limit_bytes) != 0)
                return -1;
            continue;
        }
        if (strcmp(argv[i], "--nice") == 0) {
            errno = 0;
            nv = strtol(argv[i+1], &end, 10);
            if (errno != 0 || end == argv[i+1] || *end != '\0' ||
                nv < -20 || nv > 19) {
                fprintf(stderr, "Invalid --nice value: %s\n", argv[i+1]);
                return -1;
            }
            req->nice_value = (int)nv;
            continue;
        }
        fprintf(stderr, "Unknown option: %s\n", argv[i]);
        return -1;
    }
    if (req->soft_limit_bytes > req->hard_limit_bytes) {
        fprintf(stderr, "soft limit cannot exceed hard limit\n");
        return -1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Bounded Buffer                                                      */
/* ------------------------------------------------------------------ */
static int bounded_buffer_init(bounded_buffer_t *b)
{
    int rc;
    memset(b, 0, sizeof(*b));
    rc = pthread_mutex_init(&b->mutex, NULL);
    if (rc) return rc;
    rc = pthread_cond_init(&b->not_empty, NULL);
    if (rc) { pthread_mutex_destroy(&b->mutex); return rc; }
    rc = pthread_cond_init(&b->not_full, NULL);
    if (rc) {
        pthread_cond_destroy(&b->not_empty);
        pthread_mutex_destroy(&b->mutex);
        return rc;
    }
    return 0;
}

static void bounded_buffer_destroy(bounded_buffer_t *b)
{
    pthread_cond_destroy(&b->not_full);
    pthread_cond_destroy(&b->not_empty);
    pthread_mutex_destroy(&b->mutex);
}

static void bounded_buffer_begin_shutdown(bounded_buffer_t *b)
{
    pthread_mutex_lock(&b->mutex);
    b->shutting_down = 1;
    pthread_cond_broadcast(&b->not_empty);
    pthread_cond_broadcast(&b->not_full);
    pthread_mutex_unlock(&b->mutex);
}

/*
 * Push a log item.
 * Blocks when full. Returns 0 on success, -1 if shutting down and full.
 */
int bounded_buffer_push(bounded_buffer_t *b, const log_item_t *item)
{
    pthread_mutex_lock(&b->mutex);
    while (b->count == LOG_BUFFER_CAPACITY && !b->shutting_down)
        pthread_cond_wait(&b->not_full, &b->mutex);

    if (b->shutting_down && b->count == LOG_BUFFER_CAPACITY) {
        pthread_mutex_unlock(&b->mutex);
        return -1;
    }

    b->items[b->tail] = *item;
    b->tail = (b->tail + 1) % LOG_BUFFER_CAPACITY;
    b->count++;
    pthread_cond_signal(&b->not_empty);
    pthread_mutex_unlock(&b->mutex);
    return 0;
}

/*
 * Pop a log item.
 * Blocks while empty and not shutting down.
 * Returns 0 on success, -1 when shut down and empty (consumer should exit).
 */
int bounded_buffer_pop(bounded_buffer_t *b, log_item_t *item)
{
    pthread_mutex_lock(&b->mutex);
    while (b->count == 0 && !b->shutting_down)
        pthread_cond_wait(&b->not_empty, &b->mutex);

    if (b->count == 0) {
        pthread_mutex_unlock(&b->mutex);
        return -1;
    }

    *item = b->items[b->head];
    b->head = (b->head + 1) % LOG_BUFFER_CAPACITY;
    b->count--;
    pthread_cond_signal(&b->not_full);
    pthread_mutex_unlock(&b->mutex);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Logger consumer thread                                              */
/* ------------------------------------------------------------------ */
void *logging_thread(void *arg)
{
    bounded_buffer_t *buf = (bounded_buffer_t *)arg;
    log_item_t item;

    while (bounded_buffer_pop(buf, &item) == 0) {
        char path[PATH_MAX];
        snprintf(path, sizeof(path), "%s/%s.log", LOG_DIR, item.container_id);

        int fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (fd >= 0) {
            ssize_t written = 0;
            while (written < (ssize_t)item.length) {
                ssize_t n = write(fd, item.data + written,
                                  item.length - (size_t)written);
                if (n <= 0) break;
                written += n;
            }
            close(fd);
        }
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Producer thread: reads one container pipe, pushes to buffer        */
/* ------------------------------------------------------------------ */
static void *producer_thread(void *arg)
{
    producer_arg_t *parg = (producer_arg_t *)arg;
    log_item_t item;

    memset(&item, 0, sizeof(item));
    strncpy(item.container_id, parg->container_id, CONTAINER_ID_LEN - 1);

    while (1) {
        ssize_t n = read(parg->pipe_read_fd, item.data, LOG_CHUNK_SIZE);
        if (n <= 0) break;
        item.length = (size_t)n;
        bounded_buffer_push(parg->buffer, &item);
    }

    close(parg->pipe_read_fd);
    free(parg);
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Container child entrypoint (executes after clone())                */
/* ------------------------------------------------------------------ */
int child_fn(void *arg)
{
    child_config_t *cfg = (child_config_t *)arg;

    /* Redirect stdout and stderr to supervisor log pipe */
    dup2(cfg->log_write_fd, STDOUT_FILENO);
    dup2(cfg->log_write_fd, STDERR_FILENO);
    close(cfg->log_write_fd);

    /* Set UTS hostname to container ID */
    sethostname(cfg->id, strlen(cfg->id));

    /* Mount /proc inside the container's rootfs */
    char proc_path[PATH_MAX];
    snprintf(proc_path, sizeof(proc_path), "%s/proc", cfg->rootfs);
    mkdir(proc_path, 0555);
    if (mount("proc", proc_path, "proc", 0, NULL) != 0)
        perror("mount proc");

    /* chroot into the container rootfs */
    if (chroot(cfg->rootfs) != 0) {
        perror("chroot");
        return 1;
    }
    if (chdir("/") != 0) {
        perror("chdir /");
        return 1;
    }

    /* Apply nice value if requested */
    if (cfg->nice_value != 0)
        nice(cfg->nice_value);

    /* Execute the requested command */
    char *args[] = { cfg->command, NULL };
    execv(cfg->command, args);
    perror("execv");
    return 1;
}

/* ------------------------------------------------------------------ */
/* Kernel monitor ioctl helpers                                        */
/* ------------------------------------------------------------------ */
int register_with_monitor(int monitor_fd, const char *container_id,
                          pid_t host_pid, unsigned long soft_limit_bytes,
                          unsigned long hard_limit_bytes)
{
    struct monitor_request req;
    memset(&req, 0, sizeof(req));
    req.pid = host_pid;
    req.soft_limit_bytes = soft_limit_bytes;
    req.hard_limit_bytes = hard_limit_bytes;
    strncpy(req.container_id, container_id, sizeof(req.container_id) - 1);
    if (ioctl(monitor_fd, MONITOR_REGISTER, &req) < 0)
        return -1;
    return 0;
}

int unregister_from_monitor(int monitor_fd, const char *container_id,
                            pid_t host_pid)
{
    struct monitor_request req;
    memset(&req, 0, sizeof(req));
    req.pid = host_pid;
    strncpy(req.container_id, container_id, sizeof(req.container_id) - 1);
    if (ioctl(monitor_fd, MONITOR_UNREGISTER, &req) < 0)
        return -1;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Metadata helpers (caller must hold metadata_lock)                  */
/* ------------------------------------------------------------------ */
static container_record_t *find_container(supervisor_ctx_t *ctx,
                                          const char *id)
{
    container_record_t *c = ctx->containers;
    while (c) {
        if (strcmp(c->id, id) == 0) return c;
        c = c->next;
    }
    return NULL;
}

static container_record_t *alloc_container(const char *id)
{
    container_record_t *c = calloc(1, sizeof(*c));
    if (!c) return NULL;
    strncpy(c->id, id, CONTAINER_ID_LEN - 1);
    c->state = CONTAINER_STARTING;
    return c;
}

/* ------------------------------------------------------------------ */
/* SIGCHLD: reap exited children and update metadata                  */
/* ------------------------------------------------------------------ */
static void sigchld_handler(int sig)
{
    (void)sig;
    int saved_errno = errno;
    int status;
    pid_t pid;

    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        if (!g_ctx) continue;

        pthread_mutex_lock(&g_ctx->metadata_lock);
        container_record_t *c = g_ctx->containers;
        while (c) {
            if (c->host_pid == pid) {
                if (WIFEXITED(status)) {
                    c->exit_code   = WEXITSTATUS(status);
                    c->exit_signal = 0;
                    c->state       = CONTAINER_EXITED;
                } else if (WIFSIGNALED(status)) {
                    c->exit_signal = WTERMSIG(status);
                    c->exit_code   = 128 + c->exit_signal;
                    /*
                     * Attribution rule:
                     *   stop_requested set → STOPPED (manual stop flow)
                     *   otherwise         → KILLED   (hard-limit or external)
                     */
                    c->state = c->stop_requested ? CONTAINER_STOPPED
                                                 : CONTAINER_KILLED;
                }
                if (g_ctx->monitor_fd >= 0)
                    unregister_from_monitor(g_ctx->monitor_fd, c->id, pid);
                break;
            }
            c = c->next;
        }
        pthread_mutex_unlock(&g_ctx->metadata_lock);
    }
    errno = saved_errno;
}

/* ------------------------------------------------------------------ */
/* SIGINT / SIGTERM: orderly supervisor shutdown                       */
/* ------------------------------------------------------------------ */
static void sigterm_handler(int sig)
{
    (void)sig;
    if (g_ctx) g_ctx->should_stop = 1;
}

/* ------------------------------------------------------------------ */
/* Handle one CLI connection                                           */
/* ------------------------------------------------------------------ */
static void handle_control_connection(supervisor_ctx_t *ctx, int client_fd)
{
    control_request_t req;
    control_response_t resp;
    ssize_t n;

    memset(&resp, 0, sizeof(resp));

    n = recv(client_fd, &req, sizeof(req), MSG_WAITALL);
    if (n != (ssize_t)sizeof(req)) {
        resp.status = -1;
        snprintf(resp.message, sizeof(resp.message), "recv error");
        send(client_fd, &resp, sizeof(resp), 0);
        return;
    }

    switch (req.kind) {

    /* ---- START / RUN --------------------------------------------- */
    case CMD_START:
    case CMD_RUN: {
        pthread_mutex_lock(&ctx->metadata_lock);
        if (find_container(ctx, req.container_id)) {
            pthread_mutex_unlock(&ctx->metadata_lock);
            resp.status = -1;
            snprintf(resp.message, sizeof(resp.message),
                     "container '%s' already exists", req.container_id);
            send(client_fd, &resp, sizeof(resp), 0);
            return;
        }

        container_record_t *rec = alloc_container(req.container_id);
        if (!rec) {
            pthread_mutex_unlock(&ctx->metadata_lock);
            resp.status = -1;
            snprintf(resp.message, sizeof(resp.message), "out of memory");
            send(client_fd, &resp, sizeof(resp), 0);
            return;
        }
        rec->soft_limit_bytes = req.soft_limit_bytes;
        rec->hard_limit_bytes = req.hard_limit_bytes;
        rec->started_at = time(NULL);
        snprintf(rec->log_path, sizeof(rec->log_path),
                 "%s/%s.log", LOG_DIR, req.container_id);
        pthread_mutex_unlock(&ctx->metadata_lock);

        /* Create log pipe */
        int pipefd[2];
        if (pipe(pipefd) != 0) {
            free(rec);
            resp.status = -1;
            snprintf(resp.message, sizeof(resp.message),
                     "pipe: %s", strerror(errno));
            send(client_fd, &resp, sizeof(resp), 0);
            return;
        }

        /* Child config (allocated on heap; child reads it before exec) */
        child_config_t *cfg = calloc(1, sizeof(*cfg));
        if (!cfg) {
            close(pipefd[0]); close(pipefd[1]);
            free(rec);
            resp.status = -1;
            snprintf(resp.message, sizeof(resp.message), "out of memory");
            send(client_fd, &resp, sizeof(resp), 0);
            return;
        }
        strncpy(cfg->id,      req.container_id, CONTAINER_ID_LEN - 1);
        strncpy(cfg->rootfs,  req.rootfs,        PATH_MAX - 1);
        strncpy(cfg->command, req.command,        CHILD_COMMAND_LEN - 1);
        cfg->nice_value   = req.nice_value;
        cfg->log_write_fd = pipefd[1];

        /* Allocate clone stack */
        char *stack = malloc(STACK_SIZE);
        if (!stack) {
            free(cfg);
            close(pipefd[0]); close(pipefd[1]);
            free(rec);
            resp.status = -1;
            snprintf(resp.message, sizeof(resp.message), "malloc stack failed");
            send(client_fd, &resp, sizeof(resp), 0);
            return;
        }

        pid_t pid = clone(child_fn, stack + STACK_SIZE,
                          CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS | SIGCHLD,
                          cfg);
        if (pid < 0) {
            free(stack); free(cfg);
            close(pipefd[0]); close(pipefd[1]);
            free(rec);
            resp.status = -1;
            snprintf(resp.message, sizeof(resp.message),
                     "clone failed: %s", strerror(errno));
            send(client_fd, &resp, sizeof(resp), 0);
            return;
        }

        /* Supervisor closes write end; child owns it */
        close(pipefd[1]);

        /* Finalise metadata and insert */
        pthread_mutex_lock(&ctx->metadata_lock);
        rec->host_pid = pid;
        rec->state    = CONTAINER_RUNNING;
        rec->next     = ctx->containers;
        ctx->containers = rec;
        pthread_mutex_unlock(&ctx->metadata_lock);

        /* Register with kernel monitor */
        if (ctx->monitor_fd >= 0)
            register_with_monitor(ctx->monitor_fd, req.container_id, pid,
                                  req.soft_limit_bytes, req.hard_limit_bytes);

        /* Spawn producer thread for this container's pipe read end */
        producer_arg_t *parg = calloc(1, sizeof(*parg));
        if (parg) {
            parg->pipe_read_fd = pipefd[0];
            strncpy(parg->container_id, req.container_id, CONTAINER_ID_LEN - 1);
            parg->buffer = &ctx->log_buffer;
            pthread_t pt;
            pthread_create(&pt, NULL, producer_thread, parg);
            pthread_detach(pt);
        } else {
            close(pipefd[0]);
        }

        if (req.kind == CMD_START) {
            resp.status = 0;
            snprintf(resp.message, sizeof(resp.message),
                     "started container '%s' pid=%d", req.container_id, pid);
            send(client_fd, &resp, sizeof(resp), 0);
        } else {
            /* CMD_RUN: ack first, then block until container exits */
            resp.status = 0;
            snprintf(resp.message, sizeof(resp.message),
                     "running container '%s' pid=%d", req.container_id, pid);
            send(client_fd, &resp, sizeof(resp), 0);

            int wstatus;
            waitpid(pid, &wstatus, 0);

            pthread_mutex_lock(&ctx->metadata_lock);
            container_record_t *r = find_container(ctx, req.container_id);
            if (r) {
                if (WIFEXITED(wstatus)) {
                    r->exit_code = WEXITSTATUS(wstatus);
                    r->state     = CONTAINER_EXITED;
                } else if (WIFSIGNALED(wstatus)) {
                    r->exit_signal = WTERMSIG(wstatus);
                    r->exit_code   = 128 + r->exit_signal;
                    r->state = r->stop_requested ? CONTAINER_STOPPED
                                                 : CONTAINER_KILLED;
                }
            }
            pthread_mutex_unlock(&ctx->metadata_lock);

            memset(&resp, 0, sizeof(resp));
            resp.status    = 0;
            resp.exit_code = WIFEXITED(wstatus) ? WEXITSTATUS(wstatus)
                                                 : 128 + WTERMSIG(wstatus);
            snprintf(resp.message, sizeof(resp.message),
                     "container '%s' exited with code %d",
                     req.container_id, resp.exit_code);
            send(client_fd, &resp, sizeof(resp), 0);
        }

        free(stack);
        free(cfg);
        break;
    }

    /* ---- PS ------------------------------------------------------ */
    case CMD_PS: {
        char buf[4096] = {0};
        int off = 0;
        off += snprintf(buf + off, sizeof(buf) - off,
                        "%-16s %-8s %-10s %-10s %-8s %-8s %-6s\n",
                        "ID", "PID", "STATE", "STARTED",
                        "SOFT_MiB", "HARD_MiB", "EXIT");

        pthread_mutex_lock(&ctx->metadata_lock);
        container_record_t *c = ctx->containers;
        while (c && off < (int)sizeof(buf) - 128) {
            char tstr[32];
            struct tm *tm_info = localtime(&c->started_at);
            strftime(tstr, sizeof(tstr), "%H:%M:%S", tm_info);
            off += snprintf(buf + off, sizeof(buf) - off,
                            "%-16s %-8d %-10s %-10s %-8lu %-8lu %-6d\n",
                            c->id, c->host_pid,
                            state_to_string(c->state), tstr,
                            c->soft_limit_bytes >> 20,
                            c->hard_limit_bytes >> 20,
                            c->exit_code);
            c = c->next;
        }
        pthread_mutex_unlock(&ctx->metadata_lock);

        resp.status = 0;
        snprintf(resp.message, sizeof(resp.message), "%s", buf);
        send(client_fd, &resp, sizeof(resp), 0);
        break;
    }

    /* ---- LOGS ---------------------------------------------------- */
    case CMD_LOGS: {
        char log_path[PATH_MAX];
        snprintf(log_path, sizeof(log_path),
                 "%s/%s.log", LOG_DIR, req.container_id);

        FILE *f = fopen(log_path, "r");
        if (!f) {
            resp.status = -1;
            snprintf(resp.message, sizeof(resp.message),
                     "No log found for '%s'", req.container_id);
            send(client_fd, &resp, sizeof(resp), 0);
            break;
        }

        /* Header */
        resp.status = 0;
        snprintf(resp.message, sizeof(resp.message),
                 "=== logs for %s ===", req.container_id);
        send(client_fd, &resp, sizeof(resp), 0);

        /* Stream lines */
        char line[CONTROL_MESSAGE_LEN];
        while (fgets(line, sizeof(line), f)) {
            control_response_t chunk;
            memset(&chunk, 0, sizeof(chunk));
            chunk.status = 1; /* more data */
            snprintf(chunk.message, sizeof(chunk.message), "%s", line);
            send(client_fd, &chunk, sizeof(chunk), 0);
        }
        fclose(f);

        /* EOF marker */
        control_response_t eof_resp;
        memset(&eof_resp, 0, sizeof(eof_resp));
        eof_resp.status = 2;
        send(client_fd, &eof_resp, sizeof(eof_resp), 0);
        break;
    }

    /* ---- STOP ---------------------------------------------------- */
    case CMD_STOP: {
        pthread_mutex_lock(&ctx->metadata_lock);
        container_record_t *c = find_container(ctx, req.container_id);
        if (!c || c->state != CONTAINER_RUNNING) {
            pthread_mutex_unlock(&ctx->metadata_lock);
            resp.status = -1;
            snprintf(resp.message, sizeof(resp.message),
                     "container '%s' not running", req.container_id);
            send(client_fd, &resp, sizeof(resp), 0);
            break;
        }
        /* Set attribution flag BEFORE signalling */
        c->stop_requested = 1;
        pid_t pid = c->host_pid;
        pthread_mutex_unlock(&ctx->metadata_lock);

        kill(pid, SIGTERM);
        usleep(500000);
        kill(pid, SIGKILL);

        resp.status = 0;
        snprintf(resp.message, sizeof(resp.message),
                 "stop sent to container '%s'", req.container_id);
        send(client_fd, &resp, sizeof(resp), 0);
        break;
    }

    default:
        resp.status = -1;
        snprintf(resp.message, sizeof(resp.message), "unknown command");
        send(client_fd, &resp, sizeof(resp), 0);
        break;
    }
}

/* ------------------------------------------------------------------ */
/* Supervisor main loop                                                */
/* ------------------------------------------------------------------ */
static int run_supervisor(const char *rootfs)
{
    supervisor_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.server_fd  = -1;
    ctx.monitor_fd = -1;
    g_ctx = &ctx;

    mkdir(LOG_DIR, 0755);

    if (pthread_mutex_init(&ctx.metadata_lock, NULL) != 0) {
        perror("pthread_mutex_init");
        return 1;
    }
    if (bounded_buffer_init(&ctx.log_buffer) != 0) {
        perror("bounded_buffer_init");
        pthread_mutex_destroy(&ctx.metadata_lock);
        return 1;
    }

    /* Open kernel monitor (non-fatal if module not loaded) */
    ctx.monitor_fd = open(MONITOR_DEV, O_RDWR);
    if (ctx.monitor_fd < 0)
        fprintf(stderr, "[supervisor] Warning: cannot open %s: %s\n",
                MONITOR_DEV, strerror(errno));

    /* Create UNIX domain socket */
    ctx.server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (ctx.server_fd < 0) { perror("socket"); return 1; }

    unlink(CONTROL_PATH);
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);
    if (bind(ctx.server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind"); close(ctx.server_fd); return 1;
    }
    if (listen(ctx.server_fd, 8) < 0) {
        perror("listen"); close(ctx.server_fd); return 1;
    }

    /* Signal handlers */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigchld_handler;
    sa.sa_flags   = SA_RESTART | SA_NOCLDSTOP;
    sigaction(SIGCHLD, &sa, NULL);

    sa.sa_handler = sigterm_handler;
    sa.sa_flags   = SA_RESTART;
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    /* Start logger consumer thread */
    if (pthread_create(&ctx.logger_thread, NULL,
                       logging_thread, &ctx.log_buffer) != 0) {
        perror("pthread_create logger");
        close(ctx.server_fd);
        return 1;
    }

    fprintf(stderr, "[supervisor] Ready. rootfs-base=%s socket=%s\n",
            rootfs, CONTROL_PATH);

    /* Event loop */
    while (!ctx.should_stop) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(ctx.server_fd, &rfds);
        struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };

        int sel = select(ctx.server_fd + 1, &rfds, NULL, NULL, &tv);
        if (sel < 0) {
            if (errno == EINTR) continue;
            perror("select");
            break;
        }
        if (sel == 0) continue;

        int client_fd = accept(ctx.server_fd, NULL, NULL);
        if (client_fd < 0) {
            if (errno == EINTR) continue;
            perror("accept");
            continue;
        }
        handle_control_connection(&ctx, client_fd);
        close(client_fd);
    }

    fprintf(stderr, "[supervisor] Shutting down...\n");

    /* Kill all running containers */
    pthread_mutex_lock(&ctx.metadata_lock);
    container_record_t *c = ctx.containers;
    while (c) {
        if (c->state == CONTAINER_RUNNING) {
            c->stop_requested = 1;
            kill(c->host_pid, SIGKILL);
        }
        c = c->next;
    }
    pthread_mutex_unlock(&ctx.metadata_lock);

    /* Reap any remaining children */
    while (waitpid(-1, NULL, WNOHANG) > 0)
        ;

    /* Drain and stop logging pipeline */
    bounded_buffer_begin_shutdown(&ctx.log_buffer);
    pthread_join(ctx.logger_thread, NULL);

    /* Free container list */
    pthread_mutex_lock(&ctx.metadata_lock);
    c = ctx.containers;
    while (c) {
        container_record_t *nx = c->next;
        free(c);
        c = nx;
    }
    ctx.containers = NULL;
    pthread_mutex_unlock(&ctx.metadata_lock);

    close(ctx.server_fd);
    unlink(CONTROL_PATH);
    if (ctx.monitor_fd >= 0) close(ctx.monitor_fd);
    bounded_buffer_destroy(&ctx.log_buffer);
    pthread_mutex_destroy(&ctx.metadata_lock);

    fprintf(stderr, "[supervisor] Clean exit.\n");
    return 0;
}

/* ------------------------------------------------------------------ */
/* CLI client                                                          */
/* ------------------------------------------------------------------ */
static int send_control_request(const control_request_t *req)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); return 1; }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "Cannot connect to supervisor at %s: %s\n",
                CONTROL_PATH, strerror(errno));
        close(fd);
        return 1;
    }

    if (send(fd, req, sizeof(*req), 0) != (ssize_t)sizeof(*req)) {
        perror("send"); close(fd); return 1;
    }

    control_response_t resp;
    int rc = 0;

    if (req->kind == CMD_LOGS) {
        if (recv(fd, &resp, sizeof(resp), MSG_WAITALL) == (ssize_t)sizeof(resp)) {
            if (resp.status < 0) {
                fprintf(stderr, "Error: %s\n", resp.message);
                rc = 1;
            } else {
                printf("%s\n", resp.message);
                while (recv(fd, &resp, sizeof(resp), MSG_WAITALL)
                       == (ssize_t)sizeof(resp)) {
                    if (resp.status == 2) break;
                    printf("%s", resp.message);
                }
            }
        }
    } else if (req->kind == CMD_RUN) {
        /* First ack */
        if (recv(fd, &resp, sizeof(resp), MSG_WAITALL) == (ssize_t)sizeof(resp)) {
            if (resp.status < 0) {
                fprintf(stderr, "Error: %s\n", resp.message);
                rc = 1;
            } else {
                printf("%s\n", resp.message);
                /* Wait for final exit-code message */
                if (recv(fd, &resp, sizeof(resp), MSG_WAITALL)
                    == (ssize_t)sizeof(resp)) {
                    printf("%s\n", resp.message);
                    rc = resp.exit_code;
                }
            }
        }
    } else {
        if (recv(fd, &resp, sizeof(resp), MSG_WAITALL) == (ssize_t)sizeof(resp)) {
            if (resp.status < 0) {
                fprintf(stderr, "Error: %s\n", resp.message);
                rc = 1;
            } else {
                printf("%s\n", resp.message);
            }
        }
    }

    close(fd);
    return rc;
}

/* ------------------------------------------------------------------ */
/* CLI sub-commands                                                    */
/* ------------------------------------------------------------------ */
static int cmd_start(int argc, char *argv[])
{
    if (argc < 5) {
        fprintf(stderr,
                "Usage: %s start <id> <container-rootfs> <command> "
                "[--soft-mib N] [--hard-mib N] [--nice N]\n", argv[0]);
        return 1;
    }
    control_request_t req;
    memset(&req, 0, sizeof(req));
    req.kind = CMD_START;
    strncpy(req.container_id, argv[2], CONTAINER_ID_LEN - 1);
    strncpy(req.rootfs,       argv[3], PATH_MAX - 1);
    strncpy(req.command,      argv[4], CHILD_COMMAND_LEN - 1);
    req.soft_limit_bytes = DEFAULT_SOFT_LIMIT;
    req.hard_limit_bytes = DEFAULT_HARD_LIMIT;
    if (parse_optional_flags(&req, argc, argv, 5) != 0) return 1;
    return send_control_request(&req);
}

static int cmd_run(int argc, char *argv[])
{
    if (argc < 5) {
        fprintf(stderr,
                "Usage: %s run <id> <container-rootfs> <command> "
                "[--soft-mib N] [--hard-mib N] [--nice N]\n", argv[0]);
        return 1;
    }
    control_request_t req;
    memset(&req, 0, sizeof(req));
    req.kind = CMD_RUN;
    strncpy(req.container_id, argv[2], CONTAINER_ID_LEN - 1);
    strncpy(req.rootfs,       argv[3], PATH_MAX - 1);
    strncpy(req.command,      argv[4], CHILD_COMMAND_LEN - 1);
    req.soft_limit_bytes = DEFAULT_SOFT_LIMIT;
    req.hard_limit_bytes = DEFAULT_HARD_LIMIT;
    if (parse_optional_flags(&req, argc, argv, 5) != 0) return 1;
    return send_control_request(&req);
}

static int cmd_ps(void)
{
    control_request_t req;
    memset(&req, 0, sizeof(req));
    req.kind = CMD_PS;
    return send_control_request(&req);
}

static int cmd_logs(int argc, char *argv[])
{
    if (argc < 3) {
        fprintf(stderr, "Usage: %s logs <id>\n", argv[0]);
        return 1;
    }
    control_request_t req;
    memset(&req, 0, sizeof(req));
    req.kind = CMD_LOGS;
    strncpy(req.container_id, argv[2], CONTAINER_ID_LEN - 1);
    return send_control_request(&req);
}

static int cmd_stop(int argc, char *argv[])
{
    if (argc < 3) {
        fprintf(stderr, "Usage: %s stop <id>\n", argv[0]);
        return 1;
    }
    control_request_t req;
    memset(&req, 0, sizeof(req));
    req.kind = CMD_STOP;
    strncpy(req.container_id, argv[2], CONTAINER_ID_LEN - 1);
    return send_control_request(&req);
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */
int main(int argc, char *argv[])
{
    if (argc < 2) { usage(argv[0]); return 1; }

    if (strcmp(argv[1], "supervisor") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Usage: %s supervisor <base-rootfs>\n", argv[0]);
            return 1;
        }
        return run_supervisor(argv[2]);
    }
    if (strcmp(argv[1], "start") == 0) return cmd_start(argc, argv);
    if (strcmp(argv[1], "run")   == 0) return cmd_run(argc, argv);
    if (strcmp(argv[1], "ps")    == 0) return cmd_ps();
    if (strcmp(argv[1], "logs")  == 0) return cmd_logs(argc, argv);
    if (strcmp(argv[1], "stop")  == 0) return cmd_stop(argc, argv);

    usage(argv[0]);
    return 1;
}
