/* continuumd -- the Continuum Kernel. PID 1 on every node (ARCH-002 §04):
 * brings up networking, opens the boot-processor UART heartbeat link, and
 * supervises this node's service set as separate, fault-isolated child
 * processes.
 *
 * This reference build runs continuumd as an ordinary process (not
 * literally PID 1 under a Buildroot initramfs) so it can be developed and
 * tested on an ordinary Ubuntu machine -- see README.md's build-status
 * table. The supervision, cgroup, and heartbeat logic is identical either
 * way; only "is this process pid 1 and responsible for mounting /proc" is
 * skipped when running from a normal shell.
 */
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/signalfd.h>
#include <sys/timerfd.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "clog.h"
#include "config.h"
#include "supervisor.h"

static volatile sig_atomic_t g_shutdown_requested = 0;

/* Opens the boot-processor UART for the heartbeat link (ARCH-002 §03).
 * Returns a raw fd configured for 115200 8N1, or -1 if the device is
 * absent/unopenable -- which this reference build treats as "no boot
 * processor attached" rather than a fatal error, so it still runs on a
 * dev machine with nothing plugged in. */
static int open_uart(const char *path) {
    if (!path || path[0] == '\0') return -1;

    int fd = open(path, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) {
        clog_warn("continuumd: boot processor UART %s not available (%s) -- running without a hardware heartbeat", path, strerror(errno));
        return -1;
    }

    struct termios tio;
    if (tcgetattr(fd, &tio) == 0) {
        cfsetispeed(&tio, B115200);
        cfsetospeed(&tio, B115200);
        tio.c_cflag = (tio.c_cflag & ~CSIZE) | CS8;
        tio.c_cflag |= CLOCAL | CREAD;
        tio.c_cflag &= (unsigned)~(PARENB | CSTOPB);
        tio.c_lflag = 0;
        tio.c_iflag = 0;
        tio.c_oflag = 0;
        tcsetattr(fd, TCSANOW, &tio);
    }
    clog_info("continuumd: boot processor heartbeat link open on %s", path);
    return fd;
}

int main(int argc, char **argv) {
    clog_init("continuumd");

    const char *config_path = "/etc/continuum/continuumd.conf";
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--config") == 0 && i + 1 < argc) {
            config_path = argv[++i];
        } else if (strcmp(argv[i], "-v") == 0) {
            clog_set_level(CLOG_DEBUG);
        }
    }

    continuumd_config_t cfg;
    if (config_load(config_path, &cfg) != 0) {
        clog_error("continuumd: failed to load %s, exiting", config_path);
        return 1;
    }

    int uart_fd = open_uart(cfg.uart_device);

    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGCHLD);
    sigaddset(&mask, SIGTERM);
    sigaddset(&mask, SIGINT);
    sigprocmask(SIG_BLOCK, &mask, NULL);
    int sigfd = signalfd(-1, &mask, SFD_NONBLOCK);

    int tickfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);
    struct itimerspec tick_its = {
        .it_interval = {.tv_sec = 0, .tv_nsec = 250 * 1000000L},
        .it_value = {.tv_sec = 0, .tv_nsec = 250 * 1000000L},
    };
    timerfd_settime(tickfd, 0, &tick_its, NULL);

    int hbfd = -1;
    if (uart_fd >= 0) {
        hbfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);
        long ms = cfg.heartbeat_interval_ms > 0 ? cfg.heartbeat_interval_ms : 2000;
        struct itimerspec hb_its = {
            .it_interval = {.tv_sec = ms / 1000, .tv_nsec = (ms % 1000) * 1000000L},
            .it_value = {.tv_sec = ms / 1000, .tv_nsec = (ms % 1000) * 1000000L},
        };
        timerfd_settime(hbfd, 0, &hb_its, NULL);
    }

    int epfd = epoll_create1(0);
    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.fd = sigfd;
    epoll_ctl(epfd, EPOLL_CTL_ADD, sigfd, &ev);
    ev.data.fd = tickfd;
    epoll_ctl(epfd, EPOLL_CTL_ADD, tickfd, &ev);
    if (hbfd >= 0) {
        ev.data.fd = hbfd;
        epoll_ctl(epfd, EPOLL_CTL_ADD, hbfd, &ev);
    }

    supervisor_t sup;
    supervisor_init(&sup, &cfg);
    supervisor_start_all(&sup);

    clog_info("continuumd: %zu service(s) supervised, entering main loop", cfg.n_services);

    struct epoll_event events[8];
    while (!g_shutdown_requested) {
        int n = epoll_wait(epfd, events, 8, -1);
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }
        for (int i = 0; i < n; i++) {
            int fd = events[i].data.fd;
            if (fd == sigfd) {
                struct signalfd_siginfo si;
                while (read(sigfd, &si, sizeof(si)) == sizeof(si)) {
                    if (si.ssi_signo == SIGCHLD) {
                        int status;
                        pid_t pid;
                        while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
                            supervisor_on_child_exit(&sup, pid, status);
                        }
                    } else if (si.ssi_signo == SIGTERM || si.ssi_signo == SIGINT) {
                        clog_info("continuumd: shutdown signal received, stopping services");
                        g_shutdown_requested = 1;
                    }
                }
            } else if (fd == tickfd) {
                uint64_t exp;
                while (read(tickfd, &exp, sizeof(exp)) == sizeof(exp)) {}
                supervisor_tick(&sup, supervisor_now_ms());
            } else if (fd == hbfd) {
                uint64_t exp;
                while (read(hbfd, &exp, sizeof(exp)) == sizeof(exp)) {}
                if (uart_fd >= 0) {
                    uint8_t beat = 'H';
                    if (write(uart_fd, &beat, 1) != 1) {
                        clog_warn("continuumd: heartbeat write to boot processor failed (%s)", strerror(errno));
                    }
                }
            }
        }
    }

    supervisor_signal_all(&sup, SIGTERM);
    /* Give children a moment to exit cleanly, then move on -- this
     * reference build doesn't hard-block shutdown on a wedged child. */
    for (int waited_ms = 0; waited_ms < 3000; waited_ms += 100) {
        int status;
        pid_t pid;
        int any_alive = 0;
        while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
            supervisor_on_child_exit(&sup, pid, status);
        }
        for (size_t i = 0; i < sup.n_children; i++) {
            if (sup.children[i].pid > 0) any_alive = 1;
        }
        if (!any_alive) break;
        struct timespec ts = {.tv_sec = 0, .tv_nsec = 100 * 1000000L};
        nanosleep(&ts, NULL);
    }

    clog_info("continuumd: exiting");
    return 0;
}
