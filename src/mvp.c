#define _GNU_SOURCE

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

typedef struct progress_bar {
    bool enabled;
    bool line_open;
    off_t total_bytes;
    off_t copied_bytes;
    struct timespec last_draw;
    char label[256];
} progress_bar;

static const char *program_name = "mvp";

static void progress_break_line(progress_bar *progress) {
    if (progress != NULL && progress->line_open) {
        fputc('\n', stderr);
        fflush(stderr);
        progress->line_open = false;
    }
}

static void die_oom(void) {
    fprintf(stderr, "%s: out of memory\n", program_name);
    exit(EXIT_FAILURE);
}

static void *xmalloc(size_t size) {
    void *ptr = malloc(size);
    if (ptr == NULL) {
        die_oom();
    }
    return ptr;
}

static char *xstrdup(const char *value) {
    char *copy = strdup(value);
    if (copy == NULL) {
        die_oom();
    }
    return copy;
}

static char *xstrndup(const char *value, size_t length) {
    char *copy = strndup(value, length);
    if (copy == NULL) {
        die_oom();
    }
    return copy;
}

static void report_error(progress_bar *progress, const char *format, ...) {
    va_list args;

    progress_break_line(progress);

    fprintf(stderr, "%s: ", program_name);
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);
    fputc('\n', stderr);
}

static void report_errno(progress_bar *progress, const char *action, const char *path) {
    report_error(progress, "%s '%s': %s", action, path, strerror(errno));
}

static void print_usage(FILE *stream) {
    fprintf(stream,
            "Usage: %s SOURCE DEST\n"
            "       %s SOURCE... DIRECTORY\n"
            "\n"
            "Move files like mv, with a progress bar when data must be copied.\n",
            program_name, program_name);
}

static char *path_dirname_dup(const char *path) {
    size_t len = strlen(path);

    while (len > 1 && path[len - 1] == '/') {
        len--;
    }

    if (len == 0) {
        return xstrdup(".");
    }

    while (len > 0 && path[len - 1] != '/') {
        len--;
    }

    if (len == 0) {
        return xstrdup(".");
    }

    while (len > 1 && path[len - 1] == '/') {
        len--;
    }

    return xstrndup(path, len);
}

static char *path_basename_dup(const char *path) {
    size_t len = strlen(path);
    size_t start;

    while (len > 1 && path[len - 1] == '/') {
        len--;
    }

    if (len == 0) {
        return xstrdup(".");
    }

    start = len;
    while (start > 0 && path[start - 1] != '/') {
        start--;
    }

    if (len == 1 && path[0] == '/') {
        return xstrdup("/");
    }

    return xstrndup(path + start, len - start);
}

static char *join_paths(const char *left, const char *right) {
    size_t left_len = strlen(left);
    size_t right_len = strlen(right);
    bool needs_sep = left_len > 0 && left[left_len - 1] != '/';
    size_t total = left_len + right_len + (needs_sep ? 2 : 1);
    char *joined = xmalloc(total);

    strcpy(joined, left);
    if (needs_sep) {
        joined[left_len] = '/';
        strcpy(joined + left_len + 1, right);
    } else {
        strcpy(joined + left_len, right);
    }

    return joined;
}

static bool path_is_directory(const char *path) {
    struct stat st;

    if (stat(path, &st) != 0) {
        return false;
    }

    return S_ISDIR(st.st_mode);
}

static char *make_temp_path(const char *destination) {
    char *parent = path_dirname_dup(destination);
    char *base = path_basename_dup(destination);
    pid_t pid = getpid();

    for (int attempt = 0; attempt < 1000; attempt++) {
        int needed = snprintf(NULL, 0, ".%s.mvp.%ld.%d", base, (long)pid, attempt);
        char *name = xmalloc((size_t)needed + 1);
        char *candidate;
        struct stat st;

        snprintf(name, (size_t)needed + 1, ".%s.mvp.%ld.%d", base, (long)pid, attempt);
        candidate = join_paths(parent, name);
        free(name);

        if (lstat(candidate, &st) != 0) {
            if (errno == ENOENT) {
                free(parent);
                free(base);
                return candidate;
            }

            free(candidate);
            break;
        }

        free(candidate);
    }

    free(parent);
    free(base);
    errno = EEXIST;
    return NULL;
}

static void format_bytes(off_t bytes, char *buffer, size_t size) {
    static const char *units[] = {"B", "KiB", "MiB", "GiB", "TiB", "PiB"};
    double value = (double)bytes;
    size_t unit = 0;

    while (value >= 1024.0 && unit < (sizeof(units) / sizeof(units[0])) - 1) {
        value /= 1024.0;
        unit++;
    }

    if (unit == 0) {
        snprintf(buffer, size, "%lld %s", (long long)bytes, units[unit]);
    } else {
        snprintf(buffer, size, "%.1f %s", value, units[unit]);
    }
}

static long elapsed_milliseconds(const struct timespec *start, const struct timespec *end) {
    long seconds = end->tv_sec - start->tv_sec;
    long nanoseconds = end->tv_nsec - start->tv_nsec;

    return seconds * 1000L + nanoseconds / 1000000L;
}

static void progress_set_label(progress_bar *progress, const char *path) {
    char *base;

    if (progress == NULL) {
        return;
    }

    base = path_basename_dup(path);
    snprintf(progress->label, sizeof(progress->label), "%s", base);
    free(base);
}

static void progress_draw(progress_bar *progress, bool force) {
    struct timespec now;
    struct winsize ws;
    int width = 28;
    double ratio;
    int percent;
    char copied_text[32];
    char total_text[32];
    char line[512];
    char bar[64];
    int terminal_cols = 80;

    if (progress == NULL || !progress->enabled) {
        return;
    }

    clock_gettime(CLOCK_MONOTONIC, &now);
    if (!force && elapsed_milliseconds(&progress->last_draw, &now) < 50) {
        return;
    }
    progress->last_draw = now;

    if (ioctl(STDERR_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0) {
        terminal_cols = ws.ws_col;
    }

    if (terminal_cols > 60) {
        width = terminal_cols - 42;
        if (width > 40) {
            width = 40;
        }
    }

    ratio = progress->total_bytes > 0
                ? (double)progress->copied_bytes / (double)progress->total_bytes
                : 1.0;
    if (ratio < 0.0) {
        ratio = 0.0;
    }
    if (ratio > 1.0) {
        ratio = 1.0;
    }

    percent = (int)(ratio * 100.0);
    format_bytes(progress->copied_bytes, copied_text, sizeof(copied_text));
    format_bytes(progress->total_bytes, total_text, sizeof(total_text));

    for (int i = 0; i < width; i++) {
        bar[i] = i < (int)(ratio * width) ? '#' : '-';
    }
    bar[width] = '\0';

    snprintf(line, sizeof(line), "\r%-18.18s [%s] %3d%% %s/%s",
             progress->label[0] != '\0' ? progress->label : "moving",
             bar,
             percent,
             copied_text,
             total_text);

    fprintf(stderr, "%-*s", terminal_cols - 1, line);
    fflush(stderr);
    progress->line_open = true;
}

static void progress_init(progress_bar *progress, const char *label, off_t total_bytes) {
    memset(progress, 0, sizeof(*progress));
    progress->enabled = isatty(STDERR_FILENO) && total_bytes > 0;
    progress->total_bytes = total_bytes;
    progress->copied_bytes = 0;
    progress_set_label(progress, label);
    clock_gettime(CLOCK_MONOTONIC, &progress->last_draw);
}

static void progress_finish(progress_bar *progress) {
    if (progress == NULL || !progress->enabled) {
        return;
    }

    progress->copied_bytes = progress->total_bytes;
    progress_draw(progress, true);
    progress_break_line(progress);
}

static int best_effort_chown(const char *path, uid_t uid, gid_t gid, bool follow_symlink) {
    if (follow_symlink) {
        if (chown(path, uid, gid) != 0 && errno != EPERM) {
            return -1;
        }
    } else {
        if (lchown(path, uid, gid) != 0 && errno != EPERM) {
            return -1;
        }
    }

    return 0;
}

static int best_effort_fchown(int fd, uid_t uid, gid_t gid) {
    if (fchown(fd, uid, gid) != 0 && errno != EPERM) {
        return -1;
    }

    return 0;
}

static int apply_file_times_fd(int fd, const struct stat *st) {
    struct timespec times[2];

    times[0] = st->st_atim;
    times[1] = st->st_mtim;

    return futimens(fd, times);
}

static int apply_path_times(const char *path, const struct stat *st, int flags) {
    struct timespec times[2];

    times[0] = st->st_atim;
    times[1] = st->st_mtim;

    return utimensat(AT_FDCWD, path, times, flags);
}

static int remove_tree(const char *path, progress_bar *progress);

static int measure_bytes_recursive(const char *path, off_t *total_bytes, progress_bar *progress) {
    struct stat st;

    if (lstat(path, &st) != 0) {
        report_errno(progress, "cannot stat", path);
        return -1;
    }

    if (S_ISREG(st.st_mode)) {
        *total_bytes += st.st_size;
        return 0;
    }

    if (S_ISDIR(st.st_mode)) {
        DIR *dir = opendir(path);
        struct dirent *entry;

        if (dir == NULL) {
            report_errno(progress, "cannot open directory", path);
            return -1;
        }

        while ((entry = readdir(dir)) != NULL) {
            char *child;
            int rc;

            if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
                continue;
            }

            child = join_paths(path, entry->d_name);
            rc = measure_bytes_recursive(child, total_bytes, progress);
            free(child);

            if (rc != 0) {
                closedir(dir);
                return -1;
            }
        }

        if (closedir(dir) != 0) {
            report_errno(progress, "cannot close directory", path);
            return -1;
        }
    }

    return 0;
}

static int copy_regular_file(const char *source, const char *destination, const struct stat *st,
                             progress_bar *progress, bool unlink_source) {
    char *temporary = NULL;
    int src_fd = -1;
    int dst_fd = -1;
    bool rename_complete = false;
    const size_t buffer_size = 1024 * 1024;
    unsigned char *buffer = NULL;
    off_t start_offset = progress != NULL ? progress->copied_bytes : 0;
    off_t copied = 0;

    temporary = make_temp_path(destination);
    if (temporary == NULL) {
        report_errno(progress, "cannot create temporary path near", destination);
        return -1;
    }

    src_fd = open(source, O_RDONLY);
    if (src_fd < 0) {
        report_errno(progress, "cannot open", source);
        goto fail;
    }

    dst_fd = open(temporary, O_WRONLY | O_CREAT | O_EXCL | O_TRUNC, 0600);
    if (dst_fd < 0) {
        report_errno(progress, "cannot create", temporary);
        goto fail;
    }

    buffer = xmalloc(buffer_size);
    progress_set_label(progress, source);

    while (1) {
        ssize_t bytes_read = read(src_fd, buffer, buffer_size);

        if (bytes_read == 0) {
            break;
        }
        if (bytes_read < 0) {
            report_errno(progress, "cannot read", source);
            goto fail;
        }

        ssize_t written_total = 0;
        while (written_total < bytes_read) {
            ssize_t bytes_written = write(dst_fd, buffer + written_total,
                                          (size_t)(bytes_read - written_total));
            if (bytes_written < 0) {
                report_errno(progress, "cannot write", temporary);
                goto fail;
            }

            written_total += bytes_written;
            copied += bytes_written;
            if (progress != NULL) {
                progress->copied_bytes = start_offset + copied;
                progress_draw(progress, false);
            }
        }
    }

    if (best_effort_fchown(dst_fd, st->st_uid, st->st_gid) != 0) {
        report_errno(progress, "cannot preserve ownership for", temporary);
        goto fail;
    }

    if (fchmod(dst_fd, st->st_mode & 07777) != 0) {
        report_errno(progress, "cannot preserve mode for", temporary);
        goto fail;
    }

    if (apply_file_times_fd(dst_fd, st) != 0) {
        report_errno(progress, "cannot preserve timestamps for", temporary);
        goto fail;
    }

    if (close(dst_fd) != 0) {
        dst_fd = -1;
        report_errno(progress, "cannot close", temporary);
        goto fail;
    }
    dst_fd = -1;

    if (rename(temporary, destination) != 0) {
        report_errno(progress, "cannot replace", destination);
        goto fail;
    }
    rename_complete = true;

    if (unlink_source) {
        if (unlink(source) != 0) {
            report_errno(progress, "cannot remove", source);
            goto fail;
        }
    }

    if (close(src_fd) != 0) {
        src_fd = -1;
        report_errno(progress, "cannot close", source);
        goto fail;
    }
    src_fd = -1;

    if (progress != NULL) {
        progress->copied_bytes = start_offset + st->st_size;
        progress_draw(progress, true);
    }

    free(buffer);
    free(temporary);
    return 0;

fail:
    {
        int saved_errno = errno;

        free(buffer);
        if (src_fd >= 0) {
            close(src_fd);
        }
        if (dst_fd >= 0) {
            close(dst_fd);
        }
        if (temporary != NULL && !rename_complete) {
            unlink(temporary);
        }
        free(temporary);
        errno = saved_errno;
        return -1;
    }
}

static int copy_symlink(const char *source, const char *destination, const struct stat *st,
                        progress_bar *progress, bool unlink_source) {
    char *temporary = NULL;
    char *target = NULL;
    ssize_t size = st->st_size > 0 ? st->st_size + 1 : PATH_MAX;

    temporary = make_temp_path(destination);
    if (temporary == NULL) {
        report_errno(progress, "cannot create temporary path near", destination);
        return -1;
    }

    target = xmalloc((size_t)size + 1);
    while (1) {
        ssize_t target_length = readlink(source, target, (size_t)size);
        if (target_length < 0) {
            report_errno(progress, "cannot read symlink", source);
            goto fail;
        }
        if (target_length < size) {
            target[target_length] = '\0';
            break;
        }

        size *= 2;
        char *grown = realloc(target, (size_t)size + 1);
        if (grown == NULL) {
            die_oom();
        }
        target = grown;
    }

    if (symlink(target, temporary) != 0) {
        report_errno(progress, "cannot create symlink", temporary);
        goto fail;
    }

    if (best_effort_chown(temporary, st->st_uid, st->st_gid, false) != 0) {
        report_errno(progress, "cannot preserve ownership for", temporary);
        goto fail;
    }

    if (apply_path_times(temporary, st, AT_SYMLINK_NOFOLLOW) != 0 && errno != EPERM) {
        report_errno(progress, "cannot preserve timestamps for", temporary);
        goto fail;
    }

    if (rename(temporary, destination) != 0) {
        report_errno(progress, "cannot replace", destination);
        goto fail;
    }

    if (unlink_source) {
        if (unlink(source) != 0) {
            report_errno(progress, "cannot remove", source);
            goto fail;
        }
    }

    free(target);
    free(temporary);
    return 0;

fail:
    {
        int saved_errno = errno;

        if (temporary != NULL) {
            unlink(temporary);
        }
        free(target);
        free(temporary);
        errno = saved_errno;
        return -1;
    }
}

static int copy_special_file(const char *source, const char *destination, const struct stat *st,
                             progress_bar *progress, bool unlink_source) {
    char *temporary = make_temp_path(destination);
    bool created = false;

    if (temporary == NULL) {
        report_errno(progress, "cannot create temporary path near", destination);
        return -1;
    }

    if (S_ISFIFO(st->st_mode)) {
        if (mkfifo(temporary, 0600) != 0) {
            report_errno(progress, "cannot create fifo", temporary);
            goto fail;
        }
    } else if (S_ISCHR(st->st_mode) || S_ISBLK(st->st_mode)) {
        if (mknod(temporary, st->st_mode, st->st_rdev) != 0) {
            report_errno(progress, "cannot create special file", temporary);
            goto fail;
        }
    } else {
        report_error(progress, "cannot move unsupported file type '%s'", source);
        errno = EINVAL;
        goto fail;
    }
    created = true;

    if (best_effort_chown(temporary, st->st_uid, st->st_gid, true) != 0) {
        report_errno(progress, "cannot preserve ownership for", temporary);
        goto fail;
    }

    if (chmod(temporary, st->st_mode & 07777) != 0) {
        report_errno(progress, "cannot preserve mode for", temporary);
        goto fail;
    }

    if (apply_path_times(temporary, st, 0) != 0) {
        report_errno(progress, "cannot preserve timestamps for", temporary);
        goto fail;
    }

    if (rename(temporary, destination) != 0) {
        report_errno(progress, "cannot replace", destination);
        goto fail;
    }
    created = false;

    if (unlink_source) {
        if (unlink(source) != 0) {
            report_errno(progress, "cannot remove", source);
            goto fail;
        }
    }

    free(temporary);
    return 0;

fail:
    {
        int saved_errno = errno;

        if (created) {
            unlink(temporary);
        }
        free(temporary);
        errno = saved_errno;
        return -1;
    }
}

static int copy_tree(const char *source, const char *destination, progress_bar *progress,
                     bool unlink_source) {
    struct stat st;

    if (lstat(source, &st) != 0) {
        report_errno(progress, "cannot stat", source);
        return -1;
    }

    if (S_ISDIR(st.st_mode)) {
        DIR *dir;
        struct dirent *entry;

        if (mkdir(destination, 0700) != 0) {
            report_errno(progress, "cannot create directory", destination);
            return -1;
        }

        dir = opendir(source);
        if (dir == NULL) {
            report_errno(progress, "cannot open directory", source);
            remove_tree(destination, progress);
            return -1;
        }

        while ((entry = readdir(dir)) != NULL) {
            char *child_source;
            char *child_destination;
            int rc;

            if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
                continue;
            }

            child_source = join_paths(source, entry->d_name);
            child_destination = join_paths(destination, entry->d_name);
            rc = copy_tree(child_source, child_destination, progress, unlink_source);
            free(child_source);
            free(child_destination);

            if (rc != 0) {
                closedir(dir);
                remove_tree(destination, progress);
                return -1;
            }
        }

        if (closedir(dir) != 0) {
            report_errno(progress, "cannot close directory", source);
            remove_tree(destination, progress);
            return -1;
        }

        if (best_effort_chown(destination, st.st_uid, st.st_gid, true) != 0) {
            report_errno(progress, "cannot preserve ownership for", destination);
            remove_tree(destination, progress);
            return -1;
        }

        if (chmod(destination, st.st_mode & 07777) != 0) {
            report_errno(progress, "cannot preserve mode for", destination);
            remove_tree(destination, progress);
            return -1;
        }

        if (apply_path_times(destination, &st, 0) != 0) {
            report_errno(progress, "cannot preserve timestamps for", destination);
            remove_tree(destination, progress);
            return -1;
        }

        return 0;
    }

    if (S_ISREG(st.st_mode)) {
        return copy_regular_file(source, destination, &st, progress, unlink_source);
    }

    if (S_ISLNK(st.st_mode)) {
        return copy_symlink(source, destination, &st, progress, unlink_source);
    }

    return copy_special_file(source, destination, &st, progress, unlink_source);
}

static int remove_tree(const char *path, progress_bar *progress) {
    struct stat st;

    if (lstat(path, &st) != 0) {
        report_errno(progress, "cannot stat", path);
        return -1;
    }

    if (S_ISDIR(st.st_mode)) {
        DIR *dir = opendir(path);
        struct dirent *entry;

        if (dir == NULL) {
            report_errno(progress, "cannot open directory", path);
            return -1;
        }

        while ((entry = readdir(dir)) != NULL) {
            char *child;
            int rc;

            if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
                continue;
            }

            child = join_paths(path, entry->d_name);
            rc = remove_tree(child, progress);
            free(child);

            if (rc != 0) {
                closedir(dir);
                return -1;
            }
        }

        if (closedir(dir) != 0) {
            report_errno(progress, "cannot close directory", path);
            return -1;
        }

        if (rmdir(path) != 0) {
            report_errno(progress, "cannot remove directory", path);
            return -1;
        }

        return 0;
    }

    if (unlink(path) != 0) {
        report_errno(progress, "cannot remove", path);
        return -1;
    }

    return 0;
}

static int move_across_filesystems(const char *source, const char *destination, const struct stat *st) {
    progress_bar progress;
    off_t total_bytes = 0;
    int rc;
    char *temporary = NULL;
    bool renamed_into_place = false;

    if (S_ISREG(st->st_mode)) {
        total_bytes = st->st_size;
    } else if (S_ISDIR(st->st_mode)) {
        if (measure_bytes_recursive(source, &total_bytes, NULL) != 0) {
            return -1;
        }
    }

    progress_init(&progress, source, total_bytes);
    if (S_ISDIR(st->st_mode)) {
        temporary = make_temp_path(destination);
        if (temporary == NULL) {
            report_errno(&progress, "cannot create temporary path near", destination);
            progress_break_line(&progress);
            return -1;
        }

        rc = copy_tree(source, temporary, &progress, false);
        if (rc == 0 && rename(temporary, destination) != 0) {
            report_errno(&progress, "cannot replace", destination);
            rc = -1;
        } else if (rc == 0) {
            renamed_into_place = true;
        }
        if (rc == 0 && remove_tree(source, &progress) != 0) {
            rc = -1;
        }
        if (rc != 0) {
            if (temporary != NULL && !renamed_into_place) {
                remove_tree(temporary, &progress);
            }
            progress_break_line(&progress);
            free(temporary);
            return -1;
        }
    } else {
        rc = copy_tree(source, destination, &progress, true);
        if (rc != 0) {
            progress_break_line(&progress);
            return -1;
        }
    }

    progress_finish(&progress);
    free(temporary);
    return 0;
}

static int move_path(const char *source, const char *destination) {
    struct stat source_st;
    struct stat destination_st;
    bool destination_exists = false;

    if (lstat(source, &source_st) != 0) {
        report_errno(NULL, "cannot stat", source);
        return -1;
    }

    if (lstat(destination, &destination_st) == 0) {
        destination_exists = true;
    } else if (errno != ENOENT) {
        report_errno(NULL, "cannot stat", destination);
        return -1;
    }

    if (destination_exists &&
        source_st.st_dev == destination_st.st_dev &&
        source_st.st_ino == destination_st.st_ino) {
        report_error(NULL, "'%s' and '%s' are the same file", source, destination);
        return -1;
    }

    if (S_ISDIR(source_st.st_mode) && destination_exists && !S_ISDIR(destination_st.st_mode)) {
        report_error(NULL, "cannot overwrite non-directory '%s' with directory '%s'",
                     destination, source);
        return -1;
    }

    if (!S_ISDIR(source_st.st_mode) && destination_exists && S_ISDIR(destination_st.st_mode)) {
        report_error(NULL, "cannot overwrite directory '%s' with non-directory '%s'",
                     destination, source);
        return -1;
    }

    if (rename(source, destination) == 0) {
        return 0;
    }

    if (errno != EXDEV) {
        report_error(NULL, "cannot move '%s' to '%s': %s", source, destination, strerror(errno));
        return -1;
    }

    return move_across_filesystems(source, destination, &source_st);
}

int main(int argc, char **argv) {
    const char *destination_arg;
    bool destination_is_directory;
    int sources;
    int exit_status = EXIT_SUCCESS;

    program_name = argv[0] != NULL && argv[0][0] != '\0' ? argv[0] : "mvp";

    if (argc == 2 &&
        (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0)) {
        print_usage(stdout);
        return EXIT_SUCCESS;
    }

    if (argc < 3) {
        print_usage(stderr);
        return EXIT_FAILURE;
    }

    destination_arg = argv[argc - 1];
    sources = argc - 2;
    destination_is_directory = path_is_directory(destination_arg);

    if (sources > 1 && !destination_is_directory) {
        report_error(NULL, "target '%s' is not a directory", destination_arg);
        return EXIT_FAILURE;
    }

    for (int i = 1; i <= sources; i++) {
        const char *source = argv[i];
        char *destination = NULL;
        int rc;

        if (destination_is_directory) {
            char *base = path_basename_dup(source);
            destination = join_paths(destination_arg, base);
            free(base);
        } else {
            destination = xstrdup(destination_arg);
        }

        rc = move_path(source, destination);
        if (rc != 0) {
            exit_status = EXIT_FAILURE;
        }

        free(destination);
    }

    return exit_status;
}
