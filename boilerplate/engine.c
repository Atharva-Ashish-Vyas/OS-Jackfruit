/*
 * engine.c — Multi-Container Runtime + Supervisor
 *
 * Usage:
 *   engine supervisor <base-rootfs>
 *   engine start <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]
 *   engine run   <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]
 *   engine ps
 *   engine logs <id>
 *   engine stop <id>
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/mount.h>
#include <sys/ioctl.h>
#include <sched.h>
#include <pthread.h>
#include <stdatomic.h>
#include "monitor_ioctl.h"

/* ───────── constants ───────── */
#define MAX_CONTAINERS   32
#define LOG_DIR          "/tmp/engine-logs"
#define SOCK_PATH        "/tmp/engine.sock"
#define BUF_CAPACITY     4096   /* bounded log buffer slots */
#define MAX_LINE         4096
#define STACK_SIZE       (1 << 20)  /* 1 MiB clone stack */
#define DEFAULT_SOFT_MIB 40
#define DEFAULT_HARD_MIB 64

/* ───────── container state ───────── */
typedef enum {
    STATE_STARTING = 0,
    STATE_RUNNING,
    STATE_STOPPED,
    STATE_KILLED,
    STATE_HARD_LIMIT_KILLED,
    STATE_EXITED
} ContainerState;

static const char *state_str(ContainerState s) {
    switch (s) {
        case STATE_STARTING:          return "starting";
        case STATE_RUNNING:           return "running";
        case STATE_STOPPED:           return "stopped";
        case STATE_KILLED:            return "killed";
        case STATE_HARD_LIMIT_KILLED: return "hard_limit_killed";
        case STATE_EXITED:            return "exited";
        default:                      return "unknown";
    }
}

typedef struct {
    char          id[64];
    pid_t         host_pid;
    time_t        start_time;
    ContainerState state;
    int           soft_mib;
    int           hard_mib;
    char          log_path[256];
    int           exit_status;
    int           exit_signal;
    int           stop_requested;   /* attribution flag for Task 4 */
    int           pipe_stdout[2];   /* supervisor reads [0], container writes [1] */
    int           pipe_stderr[2];
    pthread_t     producer_tid;
    int           used;
    pthread_mutex_t meta_lock;
} Container;

/* ───────── bounded log buffer ───────── */
typedef struct {
    char   data[MAX_LINE];
    size_t len;
} LogEntry;

typedef struct {
    LogEntry       slots[BUF_CAPACITY];
    int            head, tail, count;
    pthread_mutex_t lock;
    pthread_cond_t  not_empty;
    pthread_cond_t  not_full;
    int            shutdown;
} BoundedBuffer;

static BoundedBuffer g_logbuf;

/* ───────── global state ───────── */
static Container    g_containers[MAX_CONTAINERS];
static pthread_mutex_t g_meta_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_t    g_consumer_tid;
static atomic_int   g_supervisor_running = 1;
static int          g_monitor_fd = -1;  /* /dev/container_monitor */

/* ───────────────────────────────────────
 * Bounded buffer helpers
 * ─────────────────────────────────────── */
static void bb_init(BoundedBuffer *b) {
    memset(b, 0, sizeof(*b));
    pthread_mutex_init(&b->lock, NULL);
    pthread_cond_init(&b->not_empty, NULL);
    pthread_cond_init(&b->not_full, NULL);
}

static void bb_push(BoundedBuffer *b, const char *data, size_t len) {
    pthread_mutex_lock(&b->lock);
    while (b->count == BUF_CAPACITY && !b->shutdown)
        pthread_cond_wait(&b->not_full, &b->lock);
    if (b->shutdown) { pthread_mutex_unlock(&b->lock); return; }
    LogEntry *e = &b->slots[b->tail];
    size_t copy = len < MAX_LINE - 1 ? len : MAX_LINE - 1;
    memcpy(e->data, data, copy);
    e->data[copy] = '\0';
    e->len = copy;
    b->tail = (b->tail + 1) % BUF_CAPACITY;
    b->count++;
    pthread_cond_signal(&b->not_empty);
    pthread_mutex_unlock(&b->lock);
}

/* Returns 0 if got entry, -1 if shutdown and empty */
static int bb_pop(BoundedBuffer *b, LogEntry *out) {
    pthread_mutex_lock(&b->lock);
    while (b->count == 0 && !b->shutdown)
        pthread_cond_wait(&b->not_empty, &b->lock);
    if (b->count == 0) { pthread_mutex_unlock(&b->lock); return -1; }
    *out = b->slots[b->head];
    b->head = (b->head + 1) % BUF_CAPACITY;
    b->count--;
    pthread_cond_signal(&b->not_full);
    pthread_mutex_unlock(&b->lock);
    return 0;
}

static void bb_shutdown(BoundedBuffer *b) {
    pthread_mutex_lock(&b->lock);
    b->shutdown = 1;
    pthread_cond_broadcast(&b->not_empty);
    pthread_cond_broadcast(&b->not_full);
    pthread_mutex_unlock(&b->lock);
}

/* ───────────────────────────────────────
 * Consumer thread — writes log entries to files
 * ─────────────────────────────────────── */
/*
 * Each log line is prefixed by container-id so the single consumer
 * can dispatch to the right file. Format: "<id>|<line>\n"
 */
static void *consumer_thread(void *arg) {
    (void)arg;
    LogEntry e;
    while (bb_pop(&g_logbuf, &e) == 0) {
        /* parse id prefix */
        char *sep = memchr(e.data, '|', e.len);
        if (!sep) continue;
        *sep = '\0';
        const char *id  = e.data;
        const char *msg = sep + 1;

        /* find container log path */
        pthread_mutex_lock(&g_meta_lock);
        char log_path[256] = {0};
        for (int i = 0; i < MAX_CONTAINERS; i++) {
            if (g_containers[i].used && strcmp(g_containers[i].id, id) == 0) {
                strncpy(log_path, g_containers[i].log_path, sizeof(log_path)-1);
                break;
            }
        }
        pthread_mutex_unlock(&g_meta_lock);

        if (log_path[0]) {
            FILE *f = fopen(log_path, "a");
            if (f) { fputs(msg, f); fclose(f); }
        }
    }
    return NULL;
}

/* ───────────────────────────────────────
 * Producer thread — reads one container's pipes
 * ─────────────────────────────────────── */
typedef struct { int fd; char id[64]; } ProducerArg;

static void *producer_thread(void *arg) {
    ProducerArg *pa = (ProducerArg *)arg;
    char buf[MAX_LINE];
    char prefixed[MAX_LINE + 70];
    ssize_t n;
    while ((n = read(pa->fd, buf, sizeof(buf)-1)) > 0) {
        buf[n] = '\0';
        int written = snprintf(prefixed, sizeof(prefixed), "%s|%s", pa->id, buf);
        bb_push(&g_logbuf, prefixed, written);
    }
    close(pa->fd);
    free(pa);
    return NULL;
}

/* ───────────────────────────────────────
 * Container lookup
 * ─────────────────────────────────────── */
static Container *find_container(const char *id) {
    for (int i = 0; i < MAX_CONTAINERS; i++)
        if (g_containers[i].used && strcmp(g_containers[i].id, id) == 0)
            return &g_containers[i];
    return NULL;
}

static Container *alloc_container(void) {
    for (int i = 0; i < MAX_CONTAINERS; i++)
        if (!g_containers[i].used) return &g_containers[i];
    return NULL;
}

/* ───────────────────────────────────────
 * Container init function (runs inside clone)
 * ─────────────────────────────────────── */
typedef struct {
    char  rootfs[256];
    char  command[512];
    int   pipe_stdout_write;
    int   pipe_stderr_write;
    int   nice_val;
} ContainerInitArg;

static int container_init(void *arg) {
    ContainerInitArg *a = (ContainerInitArg *)arg;

    /* redirect stdout/stderr to supervisor pipes */
    dup2(a->pipe_stdout_write, STDOUT_FILENO);
    dup2(a->pipe_stderr_write, STDERR_FILENO);
    close(a->pipe_stdout_write);
    close(a->pipe_stderr_write);

    /* set nice */
    if (a->nice_val != 0) nice(a->nice_val);

    /* chroot into container rootfs */
    if (chroot(a->rootfs) < 0) {
        perror("chroot");
        return 1;
    }
    if (chdir("/") < 0) {
        perror("chdir /");
        return 1;
    }

    /* mount /proc */
    mkdir("/proc", 0555);
    if (mount("proc", "/proc", "proc", 0, NULL) < 0) {
        /* non-fatal — /proc may already exist */
    }

    /* set UTS hostname to container id (passed via env-less arg parsing) */
    /* exec the command */
    char *argv[] = { "/bin/sh", "-c", a->command, NULL };
    execv("/bin/sh", argv);
    perror("execv");
    return 127;
}

/* ───────────────────────────────────────
 * Launch container
 * ─────────────────────────────────────── */
static int launch_container(Container *c, const char *rootfs, const char *command, int nice_val) {
    /* create pipes */
    if (pipe(c->pipe_stdout) < 0 || pipe(c->pipe_stderr) < 0) {
        perror("pipe");
        return -1;
    }

    /* prepare init arg */
    ContainerInitArg *ia = malloc(sizeof(*ia));
    strncpy(ia->rootfs, rootfs, sizeof(ia->rootfs)-1);
    strncpy(ia->command, command, sizeof(ia->command)-1);
    ia->pipe_stdout_write = c->pipe_stdout[1];
    ia->pipe_stderr_write = c->pipe_stderr[1];
    ia->nice_val = nice_val;

    /* clone stack */
    char *stack = malloc(STACK_SIZE);
    if (!stack) { free(ia); return -1; }
    char *stack_top = stack + STACK_SIZE;

    int flags = SIGCHLD | CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS;
    pid_t pid = clone(container_init, stack_top, flags, ia);
    free(stack);
    if (pid < 0) { perror("clone"); free(ia); return -1; }

    /* close write ends in supervisor */
    close(c->pipe_stdout[1]);
    close(c->pipe_stderr[1]);

    c->host_pid = pid;
    c->start_time = time(NULL);
    c->state = STATE_RUNNING;

    /* register with kernel module */
    if (g_monitor_fd >= 0) {
        struct container_reg reg;
        reg.pid      = pid;
        reg.soft_mib = c->soft_mib;
        reg.hard_mib = c->hard_mib;
        strncpy(reg.id, c->id, sizeof(reg.id)-1);
        if (ioctl(g_monitor_fd, CONTAINER_MONITOR_REGISTER, &reg) < 0)
            perror("ioctl REGISTER (non-fatal)");
    }

    /* start producer threads for stdout+stderr */
    /* stdout */
    ProducerArg *pa1 = malloc(sizeof(*pa1));
    pa1->fd = c->pipe_stdout[0];
    strncpy(pa1->id, c->id, sizeof(pa1->id)-1);
    pthread_t t1;
    pthread_create(&t1, NULL, producer_thread, pa1);
    pthread_detach(t1);

    /* stderr */
    ProducerArg *pa2 = malloc(sizeof(*pa2));
    pa2->fd = c->pipe_stderr[0];
    strncpy(pa2->id, c->id, sizeof(pa2->id)-1);
    pthread_t t2;
    pthread_create(&t2, NULL, producer_thread, pa2);
    pthread_detach(t2);

    free(ia);
    return 0;
}

/* ───────────────────────────────────────
 * SIGCHLD handler — reap children
 * ─────────────────────────────────────── */
static void sigchld_handler(int sig) {
    (void)sig;
    int status;
    pid_t pid;
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        pthread_mutex_lock(&g_meta_lock);
        for (int i = 0; i < MAX_CONTAINERS; i++) {
            Container *c = &g_containers[i];
            if (!c->used || c->host_pid != pid) continue;
            if (WIFEXITED(status)) {
                c->exit_status = WEXITSTATUS(status);
                c->exit_signal = 0;
                c->state = STATE_EXITED;
            } else if (WIFSIGNALED(status)) {
                c->exit_signal = WTERMSIG(status);
                c->exit_status = 128 + c->exit_signal;
                if (c->exit_signal == SIGKILL && !c->stop_requested)
                    c->state = STATE_HARD_LIMIT_KILLED;
                else if (c->stop_requested)
                    c->state = STATE_STOPPED;
                else
                    c->state = STATE_KILLED;
            }
            break;
        }
        pthread_mutex_unlock(&g_meta_lock);
    }
}

/* ───────────────────────────────────────
 * SIGINT/SIGTERM — orderly supervisor shutdown
 * ─────────────────────────────────────── */
static void shutdown_handler(int sig) {
    (void)sig;
    atomic_store(&g_supervisor_running, 0);
}

/* ───────────────────────────────────────
 * Command handling (called from supervisor main loop)
 * ─────────────────────────────────────── */

/* Parse command: "start <id> <rootfs> <cmd> [opts]"
 * Write response into resp buffer (MAX_LINE). */
static void handle_start(const char *line, char *resp, int do_run) {
    char id[64], rootfs[256], cmd[512];
    int soft = DEFAULT_SOFT_MIB, hard = DEFAULT_HARD_MIB, niceval = 0;

    /* tokenise */
    char copy[MAX_LINE];
    strncpy(copy, line, sizeof(copy)-1);
    char *saveptr;
    char *tok = strtok_r(copy, " \n", &saveptr); /* "start"/"run" */
    tok = strtok_r(NULL, " \n", &saveptr);
    if (!tok) { snprintf(resp, MAX_LINE, "ERR missing id\n"); return; }
    strncpy(id, tok, sizeof(id)-1);
    tok = strtok_r(NULL, " \n", &saveptr);
    if (!tok) { snprintf(resp, MAX_LINE, "ERR missing rootfs\n"); return; }
    strncpy(rootfs, tok, sizeof(rootfs)-1);
    tok = strtok_r(NULL, " \n", &saveptr);
    if (!tok) { snprintf(resp, MAX_LINE, "ERR missing command\n"); return; }
    strncpy(cmd, tok, sizeof(cmd)-1);

    /* remaining optional flags */
    while ((tok = strtok_r(NULL, " \n", &saveptr)) != NULL) {
        if (strcmp(tok, "--soft-mib") == 0) {
            tok = strtok_r(NULL, " \n", &saveptr);
            if (tok) soft = atoi(tok);
        } else if (strcmp(tok, "--hard-mib") == 0) {
            tok = strtok_r(NULL, " \n", &saveptr);
            if (tok) hard = atoi(tok);
        } else if (strcmp(tok, "--nice") == 0) {
            tok = strtok_r(NULL, " \n", &saveptr);
            if (tok) niceval = atoi(tok);
        }
    }

    pthread_mutex_lock(&g_meta_lock);

    if (find_container(id)) {
        pthread_mutex_unlock(&g_meta_lock);
        snprintf(resp, MAX_LINE, "ERR container '%s' already exists\n", id);
        return;
    }

    Container *c = alloc_container();
    if (!c) {
        pthread_mutex_unlock(&g_meta_lock);
        snprintf(resp, MAX_LINE, "ERR max containers reached\n");
        return;
    }

    memset(c, 0, sizeof(*c));
    strncpy(c->id, id, sizeof(c->id)-1);
    c->soft_mib = soft;
    c->hard_mib = hard;
    c->state    = STATE_STARTING;
    c->used     = 1;
    snprintf(c->log_path, sizeof(c->log_path), "%s/%s.log", LOG_DIR, id);
    pthread_mutex_init(&c->meta_lock, NULL);

    if (launch_container(c, rootfs, cmd, niceval) < 0) {
        c->used = 0;
        pthread_mutex_unlock(&g_meta_lock);
        snprintf(resp, MAX_LINE, "ERR failed to launch container '%s'\n", id);
        return;
    }

    pid_t pid = c->host_pid;
    pthread_mutex_unlock(&g_meta_lock);

    if (do_run) {
        /* block until container exits — signal via resp with special prefix */
        snprintf(resp, MAX_LINE, "RUN_WAIT %s %d\n", id, pid);
    } else {
        snprintf(resp, MAX_LINE, "OK started '%s' pid=%d\n", id, pid);
    }
}

static void handle_ps(char *resp) {
    int pos = 0;
    pos += snprintf(resp + pos, MAX_LINE - pos,
        "%-12s %-8s %-10s %-20s %-6s %-6s\n",
        "ID", "PID", "STATE", "STARTED", "SOFT", "HARD");
    pos += snprintf(resp + pos, MAX_LINE - pos,
        "%-12s %-8s %-10s %-20s %-6s %-6s\n",
        "------------","--------","----------","--------------------","------","------");
    pthread_mutex_lock(&g_meta_lock);
    for (int i = 0; i < MAX_CONTAINERS && pos < MAX_LINE - 128; i++) {
        Container *c = &g_containers[i];
        if (!c->used) continue;
        char timebuf[32];
        struct tm *tm = localtime(&c->start_time);
        strftime(timebuf, sizeof(timebuf), "%Y-%m-%d %H:%M:%S", tm);
        pos += snprintf(resp + pos, MAX_LINE - pos,
            "%-12s %-8d %-10s %-20s %-6d %-6d\n",
            c->id, c->host_pid, state_str(c->state),
            timebuf, c->soft_mib, c->hard_mib);
    }
    pthread_mutex_unlock(&g_meta_lock);
}

static void handle_logs(const char *line, char *resp) {
    char copy[MAX_LINE];
    strncpy(copy, line, sizeof(copy)-1);
    char *saveptr;
    strtok_r(copy, " \n", &saveptr); /* "logs" */
    char *id = strtok_r(NULL, " \n", &saveptr);
    if (!id) { snprintf(resp, MAX_LINE, "ERR missing id\n"); return; }

    pthread_mutex_lock(&g_meta_lock);
    Container *c = find_container(id);
    char log_path[256] = {0};
    if (c) strncpy(log_path, c->log_path, sizeof(log_path)-1);
    pthread_mutex_unlock(&g_meta_lock);

    if (!log_path[0]) {
        snprintf(resp, MAX_LINE, "ERR no container '%s'\n", id);
        return;
    }

    FILE *f = fopen(log_path, "r");
    if (!f) { snprintf(resp, MAX_LINE, "(log file empty or not found)\n"); return; }
    size_t n = fread(resp, 1, MAX_LINE - 1, f);
    resp[n] = '\0';
    fclose(f);
}

static void handle_stop(const char *line, char *resp) {
    char copy[MAX_LINE];
    strncpy(copy, line, sizeof(copy)-1);
    char *saveptr;
    strtok_r(copy, " \n", &saveptr); /* "stop" */
    char *id = strtok_r(NULL, " \n", &saveptr);
    if (!id) { snprintf(resp, MAX_LINE, "ERR missing id\n"); return; }

    pthread_mutex_lock(&g_meta_lock);
    Container *c = find_container(id);
    if (!c) {
        pthread_mutex_unlock(&g_meta_lock);
        snprintf(resp, MAX_LINE, "ERR no container '%s'\n", id);
        return;
    }
    if (c->state != STATE_RUNNING) {
        pid_t pid = c->host_pid;
        pthread_mutex_unlock(&g_meta_lock);
        snprintf(resp, MAX_LINE, "OK container '%s' (pid=%d) not running\n", id, pid);
        return;
    }
    c->stop_requested = 1;
    pid_t pid = c->host_pid;
    pthread_mutex_unlock(&g_meta_lock);

    /* graceful SIGTERM, then SIGKILL after 3s */
    kill(pid, SIGTERM);
    struct timespec ts = {3, 0};
    nanosleep(&ts, NULL);
    kill(pid, SIGKILL);

    snprintf(resp, MAX_LINE, "OK stop sent to '%s' (pid=%d)\n", id, pid);
}

/* ───────────────────────────────────────
 * Supervisor main loop
 * ─────────────────────────────────────── */
static void run_supervisor(const char *base_rootfs) {
    (void)base_rootfs;

    /* setup log dir */
    mkdir(LOG_DIR, 0755);

    /* init bounded buffer */
    bb_init(&g_logbuf);

    /* start consumer thread */
    pthread_create(&g_consumer_tid, NULL, consumer_thread, NULL);

    /* open kernel monitor device (optional) */
    g_monitor_fd = open("/dev/container_monitor", O_RDWR);
    if (g_monitor_fd < 0)
        fprintf(stderr, "[supervisor] kernel monitor not available (non-fatal)\n");

    /* signals */
    struct sigaction sa_chld = {0};
    sa_chld.sa_handler = sigchld_handler;
    sa_chld.sa_flags   = SA_RESTART | SA_NOCLDSTOP;
    sigaction(SIGCHLD, &sa_chld, NULL);

    struct sigaction sa_term = {0};
    sa_term.sa_handler = shutdown_handler;
    sigaction(SIGTERM, &sa_term, NULL);
    sigaction(SIGINT,  &sa_term, NULL);

    /* UNIX domain socket for CLI IPC */
    unlink(SOCK_PATH);
    int srv = socket(AF_UNIX, SOCK_STREAM, 0);
    if (srv < 0) { perror("socket"); exit(1); }

    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCK_PATH, sizeof(addr.sun_path)-1);

    if (bind(srv, (struct sockaddr *)&addr, sizeof(addr)) < 0) { perror("bind"); exit(1); }
    if (listen(srv, 8) < 0) { perror("listen"); exit(1); }

    /* make socket non-blocking for clean shutdown */
    int flags = fcntl(srv, F_GETFL, 0);
    fcntl(srv, F_SETFL, flags | O_NONBLOCK);

    fprintf(stderr, "[supervisor] ready, socket=%s\n", SOCK_PATH);

    while (atomic_load(&g_supervisor_running)) {
        int cli = accept(srv, NULL, NULL);
        if (cli < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                usleep(50000);
                continue;
            }
            if (errno == EINTR) continue;
            perror("accept");
            break;
        }

        char req[MAX_LINE] = {0};
        ssize_t n = recv(cli, req, sizeof(req)-1, 0);
        if (n <= 0) { close(cli); continue; }
        req[n] = '\0';

        char resp[MAX_LINE * 4] = {0};

        if (strncmp(req, "start ", 6) == 0) {
            handle_start(req, resp, 0);
        } else if (strncmp(req, "run ", 4) == 0) {
            handle_start(req, resp, 1);
            /* if RUN_WAIT, wait for container exit and update resp */
            if (strncmp(resp, "RUN_WAIT", 8) == 0) {
                char wid[64]; int wpid;
                sscanf(resp, "RUN_WAIT %63s %d", wid, &wpid);
                /* poll until container is done */
                while (1) {
                    usleep(200000);
                    pthread_mutex_lock(&g_meta_lock);
                    Container *c = find_container(wid);
                    int done = c && (c->state == STATE_EXITED ||
                                     c->state == STATE_STOPPED ||
                                     c->state == STATE_KILLED  ||
                                     c->state == STATE_HARD_LIMIT_KILLED);
                    int code = c ? c->exit_status : -1;
                    pthread_mutex_unlock(&g_meta_lock);
                    if (done) {
                        snprintf(resp, sizeof(resp), "OK run '%s' exit_status=%d\n", wid, code);
                        break;
                    }
                }
            }
        } else if (strncmp(req, "ps", 2) == 0) {
            handle_ps(resp);
        } else if (strncmp(req, "logs ", 5) == 0) {
            handle_logs(req, resp);
        } else if (strncmp(req, "stop ", 5) == 0) {
            handle_stop(req, resp);
        } else {
            snprintf(resp, MAX_LINE, "ERR unknown command\n");
        }

        send(cli, resp, strlen(resp), 0);
        close(cli);
    }

    /* orderly shutdown */
    fprintf(stderr, "[supervisor] shutting down\n");

    /* stop all running containers */
    pthread_mutex_lock(&g_meta_lock);
    for (int i = 0; i < MAX_CONTAINERS; i++) {
        Container *c = &g_containers[i];
        if (c->used && c->state == STATE_RUNNING) {
            c->stop_requested = 1;
            kill(c->host_pid, SIGTERM);
        }
    }
    pthread_mutex_unlock(&g_meta_lock);

    /* wait for children */
    int waited = 0;
    while (waited < 30) {
        int any = 0;
        pthread_mutex_lock(&g_meta_lock);
        for (int i = 0; i < MAX_CONTAINERS; i++) {
            if (g_containers[i].used && g_containers[i].state == STATE_RUNNING) any = 1;
        }
        pthread_mutex_unlock(&g_meta_lock);
        if (!any) break;
        usleep(100000);
        waited++;
    }

    /* force kill stragglers */
    pthread_mutex_lock(&g_meta_lock);
    for (int i = 0; i < MAX_CONTAINERS; i++) {
        Container *c = &g_containers[i];
        if (c->used && c->state == STATE_RUNNING)
            kill(c->host_pid, SIGKILL);
    }
    pthread_mutex_unlock(&g_meta_lock);

    bb_shutdown(&g_logbuf);
    pthread_join(g_consumer_tid, NULL);

    if (g_monitor_fd >= 0) close(g_monitor_fd);
    close(srv);
    unlink(SOCK_PATH);
    fprintf(stderr, "[supervisor] clean exit\n");
}

/* ───────────────────────────────────────
 * CLI client helper — send command and print response
 * ─────────────────────────────────────── */
static int cli_send(const char *cmd) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); return 1; }

    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCK_PATH, sizeof(addr.sun_path)-1);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "Cannot connect to supervisor at %s — is it running?\n", SOCK_PATH);
        close(fd);
        return 1;
    }

    send(fd, cmd, strlen(cmd), 0);

    char buf[MAX_LINE * 4];
    ssize_t n;
    n = recv(fd, buf, sizeof(buf)-1, 0);
    if (n > 0) {
        buf[n] = '\0';
        fputs(buf, stdout);
    }

    close(fd);
    return 0;
}

/* ───────────────────────────────────────
 * main
 * ─────────────────────────────────────── */
static void print_usage(void) {
    fprintf(stderr,
        "Usage:\n"
        "  engine supervisor <base-rootfs>\n"
        "  engine start <id> <rootfs> <cmd> [--soft-mib N] [--hard-mib N] [--nice N]\n"
        "  engine run   <id> <rootfs> <cmd> [--soft-mib N] [--hard-mib N] [--nice N]\n"
        "  engine ps\n"
        "  engine logs <id>\n"
        "  engine stop <id>\n");
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        print_usage();
        return 1;
    }

    if (strcmp(argv[1], "supervisor") == 0) {
        if (argc < 3) { print_usage(); return 1; }
        run_supervisor(argv[2]);
        return 0;
    }

    /* All other sub-commands are CLI clients */
    /* Reconstruct full command string */
    char cmd[MAX_LINE] = {0};
    int pos = 0;
    for (int i = 1; i < argc; i++) {
        pos += snprintf(cmd + pos, sizeof(cmd) - pos, "%s ", argv[i]);
    }
    /* trim trailing space */
    if (pos > 0 && cmd[pos-1] == ' ') cmd[pos-1] = '\0';

    if (strcmp(argv[1], "run") == 0) {
        /* run: forward SIGINT/SIGTERM to supervisor as stop */
        /* For simplicity we just call cli_send; supervisor handles wait */
        return cli_send(cmd);
    }

    return cli_send(cmd);
}
