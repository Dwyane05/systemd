/***
  This file is part of systemd.

  Copyright 2010 Lennart Poettering

  systemd is free software; you can redistribute it and/or modify it
  under the terms of the GNU Lesser General Public License as published by
  the Free Software Foundation; either version 2.1 of the License, or
  (at your option) any later version.

  systemd is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public License
  along with systemd; If not, see <http://www.gnu.org/licenses/>.
***/

#include <dirent.h>
#include <errno.h>
#include <sys/prctl.h>
#include <sys/types.h>
#include <unistd.h>
#include <stdio.h>

#include "alloc-util.h"
#include "conf-files.h"
#include "env-util.h"
#include "exec-util.h"
#include "fd-util.h"
#include "fileio.h"
#include "hashmap.h"
#include "macro.h"
#include "process-util.h"
#include "set.h"
#include "signal-util.h"
#include "stat-util.h"
#include "string-util.h"
#include "strv.h"
#include "terminal-util.h"
#include "util.h"

/* Put this test here for a lack of better place */
assert_cc(EAGAIN == EWOULDBLOCK);

static int do_spawn(const char *path, char *argv[], int stdout_fd, pid_t *pid) {

        pid_t _pid;

        if (null_or_empty_path(path)) {
                log_debug("%s is empty (a mask).", path);
                return 0;
        }

        _pid = fork();
        if (_pid < 0)
                return log_error_errno(errno, "Failed to fork: %m");
        if (_pid == 0) {
                char *_argv[2];

                assert_se(prctl(PR_SET_PDEATHSIG, SIGTERM) == 0);

                if (stdout_fd >= 0) {
                        /* If the fd happens to be in the right place, go along with that */
                        if (stdout_fd != STDOUT_FILENO &&
                            dup2(stdout_fd, STDOUT_FILENO) < 0)
                                return -errno;

                        fd_cloexec(STDOUT_FILENO, false);
                }

                if (!argv) {
                        _argv[0] = (char*) path;
                        _argv[1] = NULL;
                        argv = _argv;
                } else
                        argv[0] = (char*) path;

                execv(path, argv);
                log_error_errno(errno, "Failed to execute %s: %m", path);
                _exit(EXIT_FAILURE);
        }

        log_debug("Spawned %s as " PID_FMT ".", path, _pid);
        *pid = _pid;
        return 1;
}

static int do_execute(
                char **directories,
                usec_t timeout,
                gather_stdout_callback_t const callbacks[_STDOUT_CONSUME_MAX],
                void* const callback_args[_STDOUT_CONSUME_MAX],
                int output_fd,
                char *argv[]) {

        _cleanup_hashmap_free_free_ Hashmap *pids = NULL;
        _cleanup_strv_free_ char **paths = NULL;
        char **path;
        int r;

        /* We fork this all off from a child process so that we can somewhat cleanly make
         * use of SIGALRM to set a time limit.
         *
         * If callbacks is nonnull, execution is serial. Otherwise, we default to parallel.
         */

        (void) reset_all_signal_handlers();
        (void) reset_signal_mask();

        assert_se(prctl(PR_SET_PDEATHSIG, SIGTERM) == 0);

        r = conf_files_list_strv(&paths, NULL, NULL, CONF_FILES_EXECUTABLE, (const char* const*) directories);
        if (r < 0)
                return r;

        if (!callbacks) {
                pids = hashmap_new(NULL);
                if (!pids)
                        return log_oom();
        }

        /* Abort execution of this process after the timout. We simply rely on SIGALRM as
         * default action terminating the process, and turn on alarm(). */

        if (timeout != USEC_INFINITY)
                alarm((timeout + USEC_PER_SEC - 1) / USEC_PER_SEC);

        STRV_FOREACH(path, paths) {
                _cleanup_free_ char *t = NULL;
                _cleanup_close_ int fd = -1;
                pid_t pid;

                t = strdup(*path);
                if (!t)
                        return log_oom();

                if (callbacks) {
                        fd = open_serialization_fd(basename(*path));
                        if (fd < 0)
                                return log_error_errno(fd, "Failed to open serialization file: %m");
                }

                r = do_spawn(t, argv, fd, &pid);
                if (r <= 0)
                        continue;

                if (pids) {
                        r = hashmap_put(pids, PID_TO_PTR(pid), t);
                        if (r < 0)
                                return log_oom();
                        t = NULL;
                } else {
                        r = wait_for_terminate_and_warn(t, pid, true);
                        if (r < 0)
                                continue;

                        if (lseek(fd, 0, SEEK_SET) < 0)
                                return log_error_errno(errno, "Failed to seek on serialization fd: %m");

                        r = callbacks[STDOUT_GENERATE](fd, callback_args[STDOUT_GENERATE]);
                        fd = -1;
                        if (r < 0)
                                return log_error_errno(r, "Failed to process output from %s: %m", *path);
                }
        }

        if (callbacks) {
                r = callbacks[STDOUT_COLLECT](output_fd, callback_args[STDOUT_COLLECT]);
                if (r < 0)
                        return log_error_errno(r, "Callback two failed: %m");
        }

        while (!hashmap_isempty(pids)) {
                _cleanup_free_ char *t = NULL;
                pid_t pid;

                pid = PTR_TO_PID(hashmap_first_key(pids));
                assert(pid > 0);

                t = hashmap_remove(pids, PID_TO_PTR(pid));
                assert(t);

                wait_for_terminate_and_warn(t, pid, true);
        }

        return 0;
}

int execute_directories(
                const char* const* directories,
                usec_t timeout,
                gather_stdout_callback_t const callbacks[_STDOUT_CONSUME_MAX],
                void* const callback_args[_STDOUT_CONSUME_MAX],
                char *argv[]) {

        pid_t executor_pid;
        char *name;
        char **dirs = (char**) directories;
        _cleanup_close_ int fd = -1;
        int r;

        assert(!strv_isempty(dirs));

        name = basename(dirs[0]);
        assert(!isempty(name));

        if (callbacks) {
                assert(callback_args);
                assert(callbacks[STDOUT_GENERATE]);
                assert(callbacks[STDOUT_COLLECT]);
                assert(callbacks[STDOUT_CONSUME]);

                fd = open_serialization_fd(name);
                if (fd < 0)
                        return log_error_errno(fd, "Failed to open serialization file: %m");
        }

        /* Executes all binaries in the directories serially or in parallel and waits for
         * them to finish. Optionally a timeout is applied. If a file with the same name
         * exists in more than one directory, the earliest one wins. */

        executor_pid = fork();
        if (executor_pid < 0)
                return log_error_errno(errno, "Failed to fork: %m");

        if (executor_pid == 0) {
                r = do_execute(dirs, timeout, callbacks, callback_args, fd, argv);
                _exit(r < 0 ? EXIT_FAILURE : EXIT_SUCCESS);
        }

        r = wait_for_terminate_and_warn(name, executor_pid, true);
        if (r < 0)
                return log_error_errno(r, "Execution failed: %m");
        if (r > 0) {
                /* non-zero return code from child */
                log_error("Forker process failed.");
                return -EREMOTEIO;
        }

        if (!callbacks)
                return 0;

        if (lseek(fd, 0, SEEK_SET) < 0)
                return log_error_errno(errno, "Failed to rewind serialization fd: %m");

        r = callbacks[STDOUT_CONSUME](fd, callback_args[STDOUT_CONSUME]);
        fd = -1;
        if (r < 0)
                return log_error_errno(r, "Failed to parse returned data: %m");
        return 0;
}

static int gather_environment_generate(int fd, void *arg) {
        char ***env = arg, **x, **y;
        _cleanup_fclose_ FILE *f = NULL;
        _cleanup_strv_free_ char **new;
        int r;

        /* Read a series of VAR=value assignments from fd, use them to update the list of
         * variables in env. Also update the exported environment.
         *
         * fd is always consumed, even on error.
         */

        assert(env);

        f = fdopen(fd, "r");
        if (!f) {
                safe_close(fd);
                return -errno;
        }

        r = load_env_file_pairs(f, NULL, NULL, &new);
        if (r < 0)
                return r;

        STRV_FOREACH_PAIR(x, y, new) {
                char *p;

                if (!env_name_is_valid(*x)) {
                        log_warning("Invalid variable assignment \"%s=...\", ignoring.", *x);
                        continue;
                }

                p = strjoin(*x, "=", *y);
                if (!p)
                        return -ENOMEM;

                r = strv_env_replace(env, p);
                if (r < 0)
                        return r;

                if (setenv(*x, *y, true) < 0)
                        return -errno;
        }

        return r;
}

static int gather_environment_collect(int fd, void *arg) {
        char ***env = arg;
        _cleanup_fclose_ FILE *f = NULL;
        int r;

        /* Write out a series of env=cescape(VAR=value) assignments to fd. */

        assert(env);

        f = fdopen(fd, "w");
        if (!f) {
                safe_close(fd);
                return -errno;
        }

        r = serialize_environment(f, *env);
        if (r < 0)
                return r;

        if (ferror(f))
                return errno > 0 ? -errno : -EIO;

        return 0;
}

static int gather_environment_consume(int fd, void *arg) {
        char ***env = arg;
        _cleanup_fclose_ FILE *f = NULL;
        char line[LINE_MAX];
        int r = 0, k;

        /* Read a series of env=cescape(VAR=value) assignments from fd into env. */

        assert(env);

        f = fdopen(fd, "r");
        if (!f) {
                safe_close(fd);
                return -errno;
        }

        FOREACH_LINE(line, f, return -EIO) {
                truncate_nl(line);

                k = deserialize_environment(env, line);
                if (k < 0)
                        log_error_errno(k, "Invalid line \"%s\": %m", line);
                if (k < 0 && r == 0)
                        r = k;
        }

        return r;
}

const gather_stdout_callback_t gather_environment[] = {
        gather_environment_generate,
        gather_environment_collect,
        gather_environment_consume,
};
