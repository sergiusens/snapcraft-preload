/* -*- Mode: C; indent-tabs-mode: nil; tab-width: 4 -*-
 *
 * Copyright (C) 2015-2016 Canonical, Ltd.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; version 3.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#define _GNU_SOURCE
#define __USE_GNU

#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <pwd.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/param.h>
#include <sys/socket.h>
#include <sys/statvfs.h>
#include <sys/un.h>
#include <sys/vfs.h>
#include <unistd.h>

#ifndef SNAPCRAFT_LIBNAME
#define SNAPCRAFT_LIBNAME "snapcraft-preload.so"
#endif

#define LITERAL_STRLEN(s) (sizeof (s) - 1)
#define LD_PRELOAD "LD_PRELOAD"
#define LD_PRELOAD_LEN LITERAL_STRLEN (LD_PRELOAD)
#define SNAPCRAFT_PRELOAD "SNAPCRAFT_PRELOAD"

static char **saved_ld_preloads = NULL;
static size_t num_saved_ld_preloads = 0;
static char *saved_snapcraft_preload = NULL;
static size_t saved_snapcraft_preload_len = 0;
static char *saved_varlib = NULL;
static size_t saved_varlib_len = 0;
static char *saved_snap_name = NULL;
static size_t saved_snap_name_len = 0;
static char *saved_snap_revision = NULL;
static size_t saved_snap_revision_len = 0;
static char *saved_snap_user_data = NULL;
static size_t saved_snap_user_data_len = 0;
static char *saved_snap_user_common = NULL;
static size_t saved_snap_user_common_len = 0;
static char saved_snap_devshm[NAME_MAX];

static void constructor() __attribute__((constructor));

static char *
getenvdup (const char *varname, size_t *envlen)
{
    char *envvar = secure_getenv (varname);
    if (envvar == NULL || envvar[0] == 0) { // identical for our purposes
        if (envlen) {
            *envlen = 0;
        }
        return NULL;
    }
    else {
        if (envlen) {
            *envlen = strlen (envvar);
            return strndup (envvar, *envlen);
        }

        return strdup (envvar);
    }
}

void constructor()
{
    char *ld_preload_copy, *p, *savedptr = NULL;
    size_t libnamelen;

    // We need to save LD_PRELOAD and SNAPCRAFT_PRELOAD in case we need to
    // propagate the values to an exec'd program.
    ld_preload_copy = getenvdup (LD_PRELOAD, NULL);
    if (ld_preload_copy == NULL) {
        return;
    }

    saved_snapcraft_preload = getenvdup (SNAPCRAFT_PRELOAD, &saved_snapcraft_preload_len);
    if (saved_snapcraft_preload == NULL) {
        free (ld_preload_copy);
        return;
    }

    saved_varlib = getenvdup ("SNAP_DATA", &saved_varlib_len);
    saved_snap_name = getenvdup ("SNAP_NAME", &saved_snap_name_len);
    saved_snap_revision = getenvdup ("SNAP_REVISION", &saved_snap_revision_len);
    saved_snap_user_data = getenvdup ("SNAP_USER_DATA", &saved_snap_user_data_len);
    saved_snap_user_common = getenvdup ("SNAP_USER_COMMON", &saved_snap_user_common_len);

    if (snprintf(saved_snap_devshm, sizeof saved_snap_devshm, "/dev/shm/snap.%s", saved_snap_name) < 0){
        perror("cannot construct path /dev/shm/snap.$SNAP_NAME");
    }

    // Pull out each absolute-pathed libsnapcraft-preload.so we find.  Better to
    // accidentally include some other libsnapcraft-preload than not propagate
    // ourselves.
    libnamelen = LITERAL_STRLEN (SNAPCRAFT_LIBNAME);
    for (p = strtok_r (ld_preload_copy, " :", &savedptr);
         p;
         p = strtok_r (NULL, " :", &savedptr)) {
        size_t plen = strlen (p);
        if (plen > libnamelen && p[0] == '/' && strncmp (p + plen - libnamelen - 1, "/" SNAPCRAFT_LIBNAME, libnamelen + 1) == 0) {
            ++num_saved_ld_preloads;
            saved_ld_preloads = realloc (saved_ld_preloads, (num_saved_ld_preloads + 1) * sizeof (char *));
            saved_ld_preloads[num_saved_ld_preloads - 1] = strdup (p);
            saved_ld_preloads[num_saved_ld_preloads] = NULL;
        }
    }
    free (ld_preload_copy);
}

static char *
redirect_writable_path (const char *pathname, const char *basepath)
{
    char *redirected_pathname;

    if (pathname[0] == 0) {
        return strdup (basepath);
    }

    size_t basepath_len = MIN (strlen (basepath), PATH_MAX);
    redirected_pathname = malloc (PATH_MAX);

    if (basepath[basepath_len - 1] == '/') {
        basepath_len -= 1;
    }
    strncpy (redirected_pathname, basepath, basepath_len);

    strncat (redirected_pathname, pathname, PATH_MAX - 1 - basepath_len);

    return redirected_pathname;
}

static char *
redirect_path_full (const char *pathname, int check_parent, int only_if_absolute)
{
    int (*_access) (const char *pathname, int mode);
    char *redirected_pathname;
    int ret;
    char *slash = 0;

    if (pathname == NULL) {
        return NULL;
    }

    const char *preload_dir = saved_snapcraft_preload;
    size_t preload_dir_len = MIN (PATH_MAX, saved_snapcraft_preload_len);

    if (preload_dir == NULL) {
        return strdup (pathname);
    }

    if (only_if_absolute && pathname[0] != '/') {
        return strdup (pathname);
    }

    _access = (int (*)(const char *pathname, int mode)) dlsym (RTLD_NEXT, "access");

    // And each app should have its own /var/lib writable tree.  Here, we want
    // to support reading the base system's files if they exist, else let the app
    // play in /var/lib themselves.  So we reverse the normal check: first see if
    // it exists in root, else do our redirection.
    if (strcmp (pathname, "/var/lib") == 0 || strncmp (pathname, "/var/lib/", 9) == 0) {
        if (saved_varlib && strncmp (pathname, saved_varlib, saved_varlib_len) != 0 && _access (pathname, F_OK) != 0) {
            return redirect_writable_path (pathname + 8, saved_varlib);
        } else {
            return strdup (pathname);
        }
    }

    if (strncmp (pathname, saved_snap_user_data, saved_snap_user_data_len) == 0 ||
        strncmp (pathname, saved_snap_user_common, saved_snap_user_common_len) == 0) {
        return strdup (pathname);
    }

    // Some apps want to open shared memory in random locations. Here we will confine it to the
    // snaps allowed path.
    redirected_pathname = malloc (PATH_MAX);

    if (strncmp (pathname, "/dev/shm/", 9) == 0) {
        snprintf(redirected_pathname, PATH_MAX - 1, "%s.%s",
                 saved_snap_devshm, pathname + 9);
        return redirected_pathname;
    }

    if (preload_dir[preload_dir_len - 1] == '/') {
        preload_dir_len -= 1;
    }
    strncpy (redirected_pathname, preload_dir, preload_dir_len);
    redirected_pathname[preload_dir_len] = '\0';

    if (pathname[0] != '/') {
        size_t cursize = strlen (redirected_pathname);
        if (getcwd (redirected_pathname + cursize, PATH_MAX - cursize) == NULL) {
            free (redirected_pathname);
            return strdup (pathname);
        }
        strncat (redirected_pathname, "/", PATH_MAX - 1 - cursize);
    }

    strncat (redirected_pathname, pathname, PATH_MAX - 1 - strlen (redirected_pathname));

    if (check_parent) {
        slash = strrchr (redirected_pathname, '/');
        if (slash) { // should always be true
            *slash = 0;
        }
    }

    ret = _access (redirected_pathname, F_OK);

    if (check_parent && slash) {
        *slash = '/';
    }

    if (ret == 0 || errno == ENOTDIR) { // ENOTDIR is OK because it exists at least
        return redirected_pathname;
    } else {
        free (redirected_pathname);
        return strdup (pathname);
    }
}

static char *
redirect_path (const char *pathname)
{
    return redirect_path_full (pathname, 0, 0);
}

static char *
redirect_path_target (const char *pathname)
{
    return redirect_path_full (pathname, 1, 0);
}

static char *
redirect_path_if_absolute (const char *pathname)
{
    return redirect_path_full (pathname, 0, 1);
}

#define REDIRECT_1_1(RET, NAME) \
RET \
NAME (const char *path) \
{ \
    RET (*_NAME) (const char *path); \
    char *new_path = NULL; \
    RET result; \
    _NAME = (RET (*)(const char *path)) dlsym (RTLD_NEXT, #NAME); \
    new_path = redirect_path (path); \
    result = _NAME (new_path); \
    free (new_path); \
    return result; \
}

#define REDIRECT_1_2(RET, NAME, T2) \
RET \
NAME (const char *path, T2 A2) \
{ \
    RET (*_NAME) (const char *path, T2 A2); \
    char *new_path = NULL; \
    RET result; \
    _NAME = (RET (*)(const char *path, T2 A2)) dlsym (RTLD_NEXT, #NAME); \
    new_path = redirect_path (path); \
    result = _NAME (new_path, A2); \
    free (new_path); \
    return result; \
}

#define REDIRECT_1_3(RET, NAME, T2, T3) \
RET \
NAME (const char *path, T2 A2, T3 A3) \
{ \
    RET (*_NAME) (const char *path, T2 A2, T3 A3); \
    char *new_path = NULL; \
    RET result; \
    _NAME = (RET (*)(const char *path, T2 A2, T3 A3)) dlsym (RTLD_NEXT, #NAME); \
    new_path = redirect_path (path); \
    result = _NAME (new_path, A2, A3); \
    free (new_path); \
    return result; \
}

#define REDIRECT_2_2(RET, NAME, T1) \
RET \
NAME (T1 A1, const char *path) \
{ \
    RET (*_NAME) (T1 A1, const char *path); \
    char *new_path = NULL; \
    RET result; \
    _NAME = (RET (*)(T1 A1, const char *path)) dlsym (RTLD_NEXT, #NAME); \
    new_path = redirect_path (path); \
    result = _NAME (A1, new_path); \
    free (new_path); \
    return result; \
}

#define REDIRECT_2_3(RET, NAME, T1, T3) \
RET \
NAME (T1 A1, const char *path, T3 A3) \
{ \
    RET (*_NAME) (T1 A1, const char *path, T3 A3); \
    char *new_path = NULL; \
    RET result; \
    _NAME = (RET (*)(T1 A1, const char *path, T3 A3)) dlsym (RTLD_NEXT, #NAME); \
    new_path = redirect_path (path); \
    result = _NAME (A1, new_path, A3); \
    free (new_path); \
    return result; \
}

#define REDIRECT_2_3_AT(RET, NAME, T1, T3) \
RET \
NAME (T1 A1, const char *path, T3 A3) \
{ \
    RET (*_NAME) (T1 A1, const char *path, T3 A3); \
    char *new_path = NULL; \
    RET result; \
    _NAME = (RET (*)(T1 A1, const char *path, T3 A3)) dlsym (RTLD_NEXT, #NAME); \
    new_path = redirect_path_if_absolute (path); \
    result = _NAME (A1, new_path, A3); \
    free (new_path); \
    return result; \
}

#define REDIRECT_2_4_AT(RET, NAME, T1, T3, T4) \
RET \
NAME (T1 A1, const char *path, T3 A3, T4 A4) \
{ \
    RET (*_NAME) (T1 A1, const char *path, T3 A3, T4 A4); \
    char *new_path = NULL; \
    RET result; \
    _NAME = (RET (*)(T1 A1, const char *path, T3 A3, T4 A4)) dlsym (RTLD_NEXT, #NAME); \
    new_path = redirect_path_if_absolute (path); \
    result = _NAME (A1, new_path, A3, A4); \
    free (new_path); \
    return result; \
}

#define REDIRECT_3_5(RET, NAME, T1, T2, T4, T5) \
RET \
NAME (T1 A1, T2 A2, const char *path, T4 A4, T5 A5) \
{ \
    RET (*_NAME) (T1 A1, T2 A2, const char *path, T4 A4, T5 A5); \
    char *new_path = NULL; \
    RET result; \
    _NAME = (RET (*)(T1 A1, T2 A2, const char *path, T4 A4, T5 A5)) dlsym (RTLD_NEXT, #NAME); \
    new_path = redirect_path (path); \
    result = _NAME (A1, A2, new_path, A4, A5); \
    free (new_path); \
    return result; \
}

#define REDIRECT_TARGET(RET, NAME) \
RET \
NAME (const char *path, const char *target) \
{ \
    RET (*_NAME) (const char *path, const char *target); \
    char *new_path = NULL; \
    char *new_target = NULL; \
    RET result; \
    _NAME = (RET (*)(const char *path, const char *target)) dlsym (RTLD_NEXT, #NAME); \
    new_path = redirect_path (path); \
    new_target = redirect_path_target (target); \
    result = _NAME (new_path, new_target); \
    free (new_path); \
    free (new_target); \
    return result; \
}

#define REDIRECT_OPEN(NAME) \
int \
NAME (const char *path, int flags, ...) \
{ \
    int mode = 0; \
    int (*_NAME) (const char *path, int flags, mode_t mode); \
    char *new_path = NULL; \
    int result; \
    if (flags & (O_CREAT|O_TMPFILE)) \
    { \
        va_list ap; \
        va_start (ap, flags); \
        mode = va_arg (ap, mode_t); \
        va_end (ap); \
    } \
    _NAME = (int (*)(const char *path, int flags, mode_t mode)) dlsym (RTLD_NEXT, #NAME); \
    new_path = redirect_path (path); \
    result = _NAME (new_path, flags, mode); \
    free (new_path); \
    return result; \
}

#define REDIRECT_OPEN_AT(NAME) \
int \
NAME (int dirfp, const char *path, int flags, ...) \
{ \
    int mode = 0; \
    int (*_NAME) (int dirfp, const char *path, int flags, mode_t mode); \
    char *new_path = NULL; \
    int result; \
    if (flags & (O_CREAT|O_TMPFILE)) \
    { \
        va_list ap; \
        va_start (ap, flags); \
        mode = va_arg (ap, mode_t); \
        va_end (ap); \
    } \
    _NAME = (int (*)(int dirfp, const char *path, int flags, mode_t mode)) dlsym (RTLD_NEXT, #NAME); \
    new_path = redirect_path_if_absolute (path); \
    result = _NAME (dirfp, new_path, flags, mode); \
    free (new_path); \
    return result; \
}

REDIRECT_1_2(FILE *, fopen, const char *)
REDIRECT_1_1(int, unlink)
REDIRECT_2_3_AT(int, unlinkat, int, int)
REDIRECT_1_2(int, access, int)
REDIRECT_1_2(int, eaccess, int)
REDIRECT_1_2(int, euidaccess, int)
REDIRECT_2_4_AT(int, faccessat, int, int, int)
REDIRECT_1_2(int, stat, struct stat *)
REDIRECT_1_2(int, stat64, struct stat64 *)
REDIRECT_1_2(int, lstat, struct stat *)
REDIRECT_1_2(int, lstat64, struct stat64 *)
REDIRECT_1_2(int, creat, mode_t)
REDIRECT_1_2(int, creat64, mode_t)
REDIRECT_1_2(int, truncate, off_t)
REDIRECT_2_2(char *, bindtextdomain, const char *)
REDIRECT_2_3(int, __xstat, int, struct stat *)
REDIRECT_2_3(int, __xstat64, int, struct stat64 *)
REDIRECT_2_3(int, __lxstat, int, struct stat *)
REDIRECT_2_3(int, __lxstat64, int, struct stat64 *)
REDIRECT_3_5(int, __fxstatat, int, int, struct stat *, int)
REDIRECT_3_5(int, __fxstatat64, int, int, struct stat64 *, int)
REDIRECT_1_2(int, statfs, struct statfs *)
REDIRECT_1_2(int, statfs64, struct statfs64 *)
REDIRECT_1_2(int, statvfs, struct statvfs *)
REDIRECT_1_2(int, statvfs64, struct statvfs64 *)
REDIRECT_1_2(long, pathconf, int)
REDIRECT_1_1(DIR *, opendir)
REDIRECT_1_2(int, mkdir, mode_t)
REDIRECT_1_1(int, rmdir)
REDIRECT_1_3(int, chown, uid_t, gid_t)
REDIRECT_1_3(int, lchown, uid_t, gid_t)
REDIRECT_1_2(int, chmod, mode_t)
REDIRECT_1_2(int, lchmod, mode_t)
REDIRECT_1_1(int, chdir)
REDIRECT_1_3(ssize_t, readlink, char *, size_t)
REDIRECT_1_2(char *, realpath, char *)
REDIRECT_TARGET(int, link)
REDIRECT_TARGET(int, rename)
REDIRECT_OPEN(open)
REDIRECT_OPEN(open64)
REDIRECT_OPEN_AT(openat)
REDIRECT_OPEN_AT(openat64)
REDIRECT_2_3(int, inotify_add_watch, int, uint32_t)

int
scandir (const char *dirp, struct dirent ***namelist, int (*filter)(const struct dirent *), int (*compar)(const struct dirent **, const struct dirent **))
{
    int (*_scandir) (const char *dirp, struct dirent ***namelist, int (*filter)(const struct dirent *), int (*compar)(const struct dirent **, const struct dirent **));
    char *new_path = NULL;
    int ret;

    _scandir = (int (*)(const char *dirp, struct dirent ***namelist, int (*filter)(const struct dirent *), int (*compar)(const struct dirent **, const struct dirent **))) dlsym (RTLD_NEXT, "scandir");

    new_path = redirect_path (dirp);
    ret = _scandir (new_path, namelist, filter, compar);
    free (new_path);

    return ret;
}

int
scandirat (int dirfd, const char *dirp, struct dirent ***namelist, int (*filter)(const struct dirent *), int (*compar)(const struct dirent **, const struct dirent **))
{
    int (*_scandirat) (int dirfd, const char *dirp, struct dirent ***namelist, int (*filter)(const struct dirent *), int (*compar)(const struct dirent **, const struct dirent **));
    char *new_path = NULL;
    int ret;

    _scandirat = (int (*)(int dirfd, const char *dirp, struct dirent ***namelist, int (*filter)(const struct dirent *), int (*compar)(const struct dirent **, const struct dirent **))) dlsym (RTLD_NEXT, "scandirat");

    new_path = redirect_path_if_absolute (dirp);
    ret = _scandirat (dirfd, new_path, namelist, filter, compar);
    free (new_path);

    return ret;
}

static int
socket_action (int (*action) (int sockfd, const struct sockaddr *addr, socklen_t addrlen), int sockfd, const struct sockaddr *addr, socklen_t addrlen)
{
    const struct sockaddr_un *un_addr = (const struct sockaddr_un *)addr;

    if (addr->sa_family != AF_UNIX) {
        // Non-unix sockets
        return action (sockfd, addr, addrlen);
    }

    if (un_addr->sun_path[0] == '\0') {
        // Abstract sockets
        return action (sockfd, addr, addrlen);
    }

    int result = 0;
    char *new_path = redirect_path (un_addr->sun_path);

    if (strncmp (un_addr->sun_path, new_path, PATH_MAX) == 0) {
        result = action (sockfd, addr, addrlen);
    } else {
        struct sockaddr_un new_addr = {0};
        new_addr.sun_family = AF_UNIX;
        new_addr.sun_path[0] = '\0';
        strncat (new_addr.sun_path, new_path, PATH_MAX - 1);
        result = action (sockfd, (const struct sockaddr *) &new_addr, sizeof (new_addr));
    }

    free (new_path);
    return result;
}

int
bind (int sockfd, const struct sockaddr *addr, socklen_t addrlen)
{
    int (*_bind) (int sockfd, const struct sockaddr *addr, socklen_t addrlen);

    _bind = (int (*)(int sockfd, const struct sockaddr *addr, socklen_t addrlen)) dlsym (RTLD_NEXT, "bind");
    return socket_action (_bind, sockfd, addr, addrlen);
}

int
connect (int sockfd, const struct sockaddr *addr, socklen_t addrlen)
{
    int (*_connect) (int sockfd, const struct sockaddr *addr, socklen_t addrlen);

    _connect = (int (*)(int sockfd, const struct sockaddr *addr, socklen_t addrlen)) dlsym (RTLD_NEXT, "connect");
    return socket_action (_connect, sockfd, addr, addrlen);
}

void *
dlopen (const char *path, int mode)
{
    void *(*_dlopen) (const char *path, int mode);
    char *new_path = NULL;
    void *result;

    _dlopen = (void *(*)(const char *path, int mode)) dlsym (RTLD_NEXT, "dlopen");

    if (path && path[0] == '/') {
        new_path = redirect_path (path);
        result = _dlopen (new_path, mode);
        free (new_path);
    } else {
        // non-absolute library paths aren't simply relative paths, they need
        // a whole lookup algorithm
        result = _dlopen (path, mode);
    }

    return result;
}

static char *
ensure_in_ld_preload (char *ld_preload, const char *to_be_added)
{
    if (ld_preload && ld_preload[0] != 0) {
        char *ld_preload_copy;
        char *p, *savedptr = NULL;
        int found = 0;

        // Check if we are already in LD_PRELOAD and thus can bail
        ld_preload_copy = strdup (ld_preload);
        for (p = strtok_r (ld_preload_copy + LD_PRELOAD_LEN + 1, " :", &savedptr);
             p;
             p = strtok_r (NULL, " :", &savedptr)) {
            if (strcmp (p, to_be_added) == 0) {
                found = 1;
                break;
            }
        }
        free (ld_preload_copy);

        if (!found) {
            size_t ld_preload_len = strlen (ld_preload);
            size_t to_be_added_len = strlen (to_be_added);
            size_t new_ld_preload_len = ld_preload_len + to_be_added_len + 2;
            ld_preload = realloc (ld_preload, new_ld_preload_len);
            ld_preload[ld_preload_len] = ':';
            strncpy (ld_preload + ld_preload_len + 1, to_be_added, to_be_added_len);
            ld_preload[new_ld_preload_len-1] = '\0';
        }
    } else {
        size_t to_be_added_len = strlen (to_be_added);
        free (ld_preload);
        ld_preload = malloc (to_be_added_len + LD_PRELOAD_LEN + 2);
        strncpy (ld_preload, LD_PRELOAD "=", LD_PRELOAD_LEN + 1);
        strncpy (ld_preload + LITERAL_STRLEN (LD_PRELOAD) + 1, to_be_added, to_be_added_len);
        ld_preload[to_be_added_len-1] = '\0';
    }
    return ld_preload;
}

static char **
execve_copy_envp (char *const envp[])
{
    int i, num_elements;
    char **new_envp = NULL;
    char *ld_preload = NULL;
    char *snapcraft_preload = NULL;

    for (num_elements = 0; envp && envp[num_elements]; num_elements++) {
        // this space intentionally left blank
    }

    new_envp = malloc (sizeof (char *) * (num_elements + 3));

    for (i = 0; i < num_elements; i++) {
        new_envp[i] = strdup (envp[i]);
        if (strncmp (envp[i], LD_PRELOAD "=", LD_PRELOAD_LEN + 1) == 0) {
            ld_preload = new_envp[i]; // point at last defined LD_PRELOAD
        }
    }

    if (saved_ld_preloads) {
        size_t j;
        char *ld_preload_copy;
        ld_preload_copy = ld_preload ? strdup (ld_preload) : NULL;
        for (j = 0; j < num_saved_ld_preloads; j++) {
            ld_preload_copy = ensure_in_ld_preload(ld_preload_copy, saved_ld_preloads[j]);
        }
        new_envp[i++] = ld_preload_copy;
    }

    if (saved_snapcraft_preload) {
        size_t preload_len = saved_snapcraft_preload_len + LITERAL_STRLEN (SNAPCRAFT_PRELOAD) + 2;
        snapcraft_preload = malloc (preload_len);
        strncpy (snapcraft_preload, SNAPCRAFT_PRELOAD "=", LITERAL_STRLEN (SNAPCRAFT_PRELOAD) + 1);
        strncpy (snapcraft_preload + LITERAL_STRLEN (SNAPCRAFT_PRELOAD) + 1, saved_snapcraft_preload, saved_snapcraft_preload_len);
        snapcraft_preload[preload_len-1] = '\0';
        new_envp[i++] = snapcraft_preload;
    }

    new_envp[i++] = NULL;
    return new_envp;
}

static int
execve32_wrapper (int (*_execve) (const char *path, char *const argv[], char *const envp[]), char *path, char *const argv[], char *const envp[])
{
    char *custom_loader = NULL;
    char **new_argv;
    int i, num_elements, result;

    custom_loader = redirect_path ("/lib/ld-linux.so.2");
    if (strcmp (custom_loader, "/lib/ld-linux.so.2") == 0) {
        free (custom_loader);
        return 0;
    }

    // envp is already adjusted for our needs.  But we need to shift argv
    for (num_elements = 0; argv && argv[num_elements]; num_elements++) {
        // this space intentionally left blank
    }
    new_argv = malloc (sizeof (char *) * (num_elements + 2));
    new_argv[0] = path;
    for (i = 0; i < num_elements; i++) {
        new_argv[i + 1] = argv[i];
    }
    new_argv[num_elements + 1] = 0;

    // Now actually run execve with our loader and adjusted argv
    result = _execve (custom_loader, new_argv, envp);

    // Cleanup on error
    free (new_argv);
    free (custom_loader);
    return result;
}

static int
execve_wrapper (const char *func, const char *path, char *const argv[], char *const envp[])
{
    int (*_execve) (const char *path, char *const argv[], char *const envp[]);
    char *new_path = NULL;
    char **new_envp = NULL;
    int i, result;

    _execve = (int (*)(const char *path, char *const argv[], char *const envp[])) dlsym (RTLD_NEXT, func);

    new_path = redirect_path (path);

    // Make sure we inject our original preload values, can't trust this
    // program to pass them along in envp for us.
    new_envp = execve_copy_envp (envp);

    result = _execve (new_path, argv, new_envp);

    if (result == -1 && errno == ENOENT) {
        // OK, get prepared for gross hacks here.  In order to run 32-bit ELF
        // executables -- which will hardcode /lib/ld-linux.so.2 as their ld.so
        // loader, we must redirect that check to our own version of ld-linux.so.2.
        // But that lookup is done behind the scenes by execve, so we can't
        // intercept it like normal.  Instead, we'll prefix the command by the
        // ld.so loader which will only work if the architecture matches.  So if
        // we failed to run it normally above because the loader couldn't find
        // something, try with our own 32-bit loader.
        int (*_access) (const char *pathname, int mode);
        _access = (int (*)(const char *pathname, int mode)) dlsym (RTLD_NEXT, "access");
        if (_access (new_path, F_OK) == 0) {
            // Only actually try this if the path actually did exist.  That
            // means the ENOENT must have been a missing linked library or the
            // wrong ld.so loader.  Lets assume the latter and try to run as
            // a 32-bit executable.
            result = execve32_wrapper (_execve, new_path, argv, new_envp);
        }
    }

    free (new_path);
    for (i = 0; new_envp[i]; i++) {
        free (new_envp[i]);
    }
    free (new_envp);

    return result;
}

int
execv (const char *path, char *const argv[])
{
    return execve (path, argv, environ);
}

int
execve (const char *path, char *const argv[], char *const envp[])
{
    return execve_wrapper ("execve", path, argv, envp);
}

int
__execve (const char *path, char *const argv[], char *const envp[])
{
    return execve_wrapper ("__execve", path, argv, envp);
}

struct passwd *
getpwnam (const char *name)
{
    struct passwd *ret;
    struct passwd *(*_getpwnam) (const char *);
    _getpwnam = (struct passwd *(*) (const char *)) dlsym (RTLD_NEXT, "getpwnam");
    ret = _getpwnam (name);
    ret->pw_dir = saved_snap_user_data;
    return ret;
}

struct passwd *
getpwuid (uid_t uid)
{
    struct passwd *ret;
    struct passwd *(*_getpwuid) (uid_t);
    _getpwuid = (struct passwd *(*) (uid_t)) dlsym (RTLD_NEXT, "getpwuid");
    ret = getpwuid (uid);
    ret->pw_dir = saved_snap_user_data;
    return ret;
}

int
getpwnam_r (const char *name, struct passwd *pwd, char *buf, size_t buflen, struct passwd **result)
{
    int ret;
    int (*_getpwnam_r) (const char *name, struct passwd *pwd, char *buf, size_t buflen, struct passwd **result);
    _getpwnam_r = (int (*) (const char *name, struct passwd *pwd, char *buf, size_t buflen, struct passwd **result)) dlsym (RTLD_NEXT, "getpwnam_r");
    ret = _getpwnam_r (name, pwd, buf, buflen, result);
    pwd->pw_dir = saved_snap_user_data;
    return ret;
}

int
getpwuid_r (uid_t uid, struct passwd *pwd, char *buf, size_t buflen, struct passwd **result)
{
    int ret;
    int (*_getpwuid_r) (uid_t uid, struct passwd *pwd, char *buf, size_t buflen, struct passwd **result);
    _getpwuid_r = (int (*) (uid_t uid, struct passwd *pwd, char *buf, size_t buflen, struct passwd **result)) dlsym (RTLD_NEXT, "getpwuid_r");
    ret = _getpwuid_r (uid, pwd, buf, buflen, result);
    pwd->pw_dir = saved_snap_user_data;
    return ret;
}
