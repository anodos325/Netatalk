/*
 * Copyright (c) 2010, Frank Lahm <franklahm@googlemail.com>
 * Copyright (c) 1988, 1993, 1994
 * The Regents of the University of California.  All rights reserved.
 *
 * This code is derived from software contributed to Berkeley by
 * David Hitz of Auspex Systems Inc.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 4. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

/*
 * Cp copies source files to target files.
 *
 * The global PATH_T structure "to" always contains the path to the
 * current target file.  Since fts(3) does not change directories,
 * this path can be either absolute or dot-relative.
 *
 * The basic algorithm is to initialize "to" and use fts(3) to traverse
 * the file hierarchy rooted in the argument list.  A trivial case is the
 * case of 'cp file1 file2'.  The more interesting case is the case of
 * 'cp file1 file2 ... fileN dir' where the hierarchy is traversed and the
 * path (relative to the root of the traversal) is appended to dir (stored
 * in "to") to form the final target path.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif /* HAVE_CONFIG_H */

#include <sys/types.h>
#include <sys/stat.h>
#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <atalk/ftw.h>
#include <atalk/adouble.h>
#include <atalk/vfs.h>
#include <atalk/util.h>
#include <atalk/unix.h>
#include <atalk/volume.h>
#include <atalk/volinfo.h>
#include <atalk/bstrlib.h>
#include <atalk/bstradd.h>
#include <atalk/queue.h>
 
#include "ad.h"

#define STRIP_TRAILING_SLASH(p) {                                   \
        while ((p).p_end > (p).p_path + 1 && (p).p_end[-1] == '/')  \
            *--(p).p_end = 0;                                       \
    }

static char emptystring[] = "";

PATH_T to = { to.p_path, emptystring, "" };
enum op { FILE_TO_FILE, FILE_TO_DIR, DIR_TO_DNE };

int fflag, iflag, lflag, nflag, pflag, vflag;
mode_t mask;

cnid_t pdid, did; /* current dir CNID and parent did*/

static afpvol_t svolume, dvolume;
static enum op type;
static int Rflag;
volatile sig_atomic_t sigint;
static int badcp, rval;
static int ftw_options = FTW_MOUNT | FTW_PHYS | FTW_ACTIONRETVAL;

static char           *netatalk_dirs[] = {
    ".AppleDouble",
    ".AppleDB",
    ".AppleDesktop",
    NULL
};

/* Forward declarations */
static int copy(const char *fpath, const struct stat *sb, int tflag, struct FTW *ftwbuf);
static void siginfo(int _U_);
static int ftw_copy_file(const struct FTW *, const char *, const struct stat *, int);
static int ftw_copy_link(const struct FTW *, const char *, const struct stat *, int);
static int setfile(const struct stat *, int);
static int preserve_dir_acls(const struct stat *, char *, char *);
static int preserve_fd_acls(int, int);

/*
  Check for netatalk special folders e.g. ".AppleDB" or ".AppleDesktop"
  Returns pointer to name or NULL.
*/
static const char *check_netatalk_dirs(const char *name)
{
    int c;

    for (c=0; netatalk_dirs[c]; c++) {
        if ((strcmp(name, netatalk_dirs[c])) == 0)
            return netatalk_dirs[c];
    }
    return NULL;
}

static void upfunc(void)
{
    did = pdid;
}

static void usage_cp(void)
{
    printf(
        "Usage: ad cp [-R [-P]] [-pvf] <source_file> <target_file>\n"
        "Usage: ad cp [-R [-P]] [-pvfx] <source_file [source_file ...]> <target_directory>\n"
        );
    exit(EXIT_FAILURE);
}

int ad_cp(int argc, char *argv[])
{
    struct stat to_stat, tmp_stat;
    int r, ch, have_trailing_slash;
    char *target;
#if 0
    afpvol_t srcvol;
    afpvol_t dstvol;
#endif

    while ((ch = getopt(argc, argv, "Rafilnpvx")) != -1)
        switch (ch) {
        case 'R':
            Rflag = 1;
            break;
        case 'a':
            pflag = 1;
            Rflag = 1;
            break;
        case 'f':
            fflag = 1;
            iflag = nflag = 0;
            break;
        case 'i':
            iflag = 1;
            fflag = nflag = 0;
            break;
        case 'l':
            lflag = 1;
            break;
        case 'n':
            nflag = 1;
            fflag = iflag = 0;
            break;
        case 'p':
            pflag = 1;
            break;
        case 'v':
            vflag = 1;
            break;
        case 'x':
            ftw_options |= FTW_MOUNT;
            break;
        default:
            usage_cp();
            break;
        }
    argc -= optind;
    argv += optind;

    if (argc < 2)
        usage_cp();

    (void)signal(SIGINT, siginfo);

    cnid_init();

    /* Save the target base in "to". */
    target = argv[--argc];
    if ((strlcpy(to.p_path, target, PATH_MAX)) >= PATH_MAX)
        ERROR("%s: name too long", target);

    to.p_end = to.p_path + strlen(to.p_path);
    if (to.p_path == to.p_end) {
        *to.p_end++ = '.';
        *to.p_end = 0;
    }
    have_trailing_slash = (to.p_end[-1] == '/');
    if (have_trailing_slash)
        STRIP_TRAILING_SLASH(to);
    to.target_end = to.p_end;

    /* Set end of argument list */
    argv[argc] = NULL;

    /*
     * Cp has two distinct cases:
     *
     * cp [-R] source target
     * cp [-R] source1 ... sourceN directory
     *
     * In both cases, source can be either a file or a directory.
     *
     * In (1), the target becomes a copy of the source. That is, if the
     * source is a file, the target will be a file, and likewise for
     * directories.
     *
     * In (2), the real target is not directory, but "directory/source".
     */
    r = stat(to.p_path, &to_stat);
    if (r == -1 && errno != ENOENT)
        ERROR("%s", to.p_path);
    if (r == -1 || !S_ISDIR(to_stat.st_mode)) {
        /*
         * Case (1).  Target is not a directory.
         */
        if (argc > 1)
            ERROR("%s is not a directory", to.p_path);

        /*
         * Need to detect the case:
         *cp -R dir foo
         * Where dir is a directory and foo does not exist, where
         * we want pathname concatenations turned on but not for
         * the initial mkdir().
         */
        if (r == -1) {
            lstat(*argv, &tmp_stat);

            if (S_ISDIR(tmp_stat.st_mode) && Rflag)
                type = DIR_TO_DNE;
            else
                type = FILE_TO_FILE;
        } else
            type = FILE_TO_FILE;

        if (have_trailing_slash && type == FILE_TO_FILE) {
            if (r == -1)
                ERROR("directory %s does not exist", to.p_path);
            else
                ERROR("%s is not a directory", to.p_path);
        }
    } else
        /*
         * Case (2).  Target is a directory.
         */
        type = FILE_TO_DIR;

    /*
     * Keep an inverted copy of the umask, for use in correcting
     * permissions on created directories when not using -p.
     */
    mask = ~umask(0777);
    umask(~mask);

#if 0
    /* Inhereting perms in ad_mkdir etc requires this */
    ad_setfuid(0);
#endif

    /* Load .volinfo file for destination*/
    openvol(to.p_path, &dvolume);

    for (int i = 0; argv[i] != NULL; i++) { 
        /* Load .volinfo file for source */
        openvol(to.p_path, &svolume);

        if (nftw(argv[i], copy, upfunc, 20, ftw_options) == -1) {
            ERROR("%s: %s", argv[i], strerror(errno));
            exit(EXIT_FAILURE);
        }


    }
    return rval;
}

static int copy(const char *path,
                const struct stat *statp,
                int tflag,
                struct FTW *ftw)
{
    struct stat to_stat;
    int base = 0, dne;
    size_t nlen;
    const char *p;
    char *target_mid;

    const char *dir = strrchr(path, '/');
    if (dir == NULL)
        dir = path;
    else
        dir++;
    if (check_netatalk_dirs(dir) != NULL) {
        SLOG("Skipping Netatalk dir %s", path);
        return FTW_SKIP_SUBTREE;
    }

    /*
     * If we are in case (2) above, we need to append the
     * source name to the target name.
     */
    if (type != FILE_TO_FILE) {
        /*
         * Need to remember the roots of traversals to create
         * correct pathnames.  If there's a directory being
         * copied to a non-existent directory, e.g.
         *     cp -R a/dir noexist
         * the resulting path name should be noexist/foo, not
         * noexist/dir/foo (where foo is a file in dir), which
         * is the case where the target exists.
         *
         * Also, check for "..".  This is for correct path
         * concatenation for paths ending in "..", e.g.
         *     cp -R .. /tmp
         * Paths ending in ".." are changed to ".".  This is
         * tricky, but seems the easiest way to fix the problem.
         *
         * XXX
         * Since the first level MUST be FTS_ROOTLEVEL, base
         * is always initialized.
         */
        if (ftw->level == 0) {
            if (type != DIR_TO_DNE) {
                base = ftw->base;

                if (strcmp(&path[base], "..") == 0)
                    base += 1;
            } else
                base = strlen(path);
        }

        p = &path[base];
        nlen = strlen(path) - base;
        target_mid = to.target_end;
        if (*p != '/' && target_mid[-1] != '/')
            *target_mid++ = '/';
        *target_mid = 0;
        if (target_mid - to.p_path + nlen >= PATH_MAX) {
            SLOG("%s%s: name too long (not copied)", to.p_path, p);
            badcp = rval = 1;
            return 0;
        }
        (void)strncat(target_mid, p, nlen);
        to.p_end = target_mid + nlen;
        *to.p_end = 0;
        STRIP_TRAILING_SLASH(to);
    }

    /* Not an error but need to remember it happened */
    if (stat(to.p_path, &to_stat) == -1)
        dne = 1;
    else {
        if (to_stat.st_dev == statp->st_dev &&
            to_stat.st_ino == statp->st_ino) {
            SLOG("%s and %s are identical (not copied).",
                to.p_path, path);
            badcp = rval = 1;
            if (S_ISDIR(statp->st_mode))
                /* without using glibc extension FTW_ACTIONRETVAL cant handle this */
                return -1;
        }
        if (!S_ISDIR(statp->st_mode) &&
            S_ISDIR(to_stat.st_mode)) {
            SLOG("cannot overwrite directory %s with "
                "non-directory %s",
                to.p_path, path);
                badcp = rval = 1;
                return 0;
        }
        dne = 0;
    }

    switch (statp->st_mode & S_IFMT) {
    case S_IFLNK:
        if (ftw_copy_link(ftw, path, statp, !dne))
            badcp = rval = 1;
        break;
    case S_IFDIR:
        if (!Rflag) {
            SLOG("%s is a directory", path);
            badcp = rval = 1;
            return -1;
        }
        /*
         * If the directory doesn't exist, create the new
         * one with the from file mode plus owner RWX bits,
         * modified by the umask.  Trade-off between being
         * able to write the directory (if from directory is
         * 555) and not causing a permissions race.  If the
         * umask blocks owner writes, we fail..
         */
        if (dne) {
            if (mkdir(to.p_path, statp->st_mode | S_IRWXU) < 0)
                ERROR("%s", to.p_path);
        } else if (!S_ISDIR(to_stat.st_mode)) {
            errno = ENOTDIR;
            ERROR("%s", to.p_path);
        }

        /* Create ad dir and copy ".Parent" */
        if (svolume.volinfo.v_path && svolume.volinfo.v_adouble == AD_VERSION2 &&
            dvolume.volinfo.v_path && dvolume.volinfo.v_adouble == AD_VERSION2) {
            /* Create ".AppleDouble" dir */
            mode_t omask = umask(0);
            bstring addir = bfromcstr(to.p_path);
            bcatcstr(addir, "/.AppleDouble");
            mkdir(cfrombstr(addir), 02777);

            /* copy ".Parent" file */
            bcatcstr(addir, "/.Parent");
            bstring sdir = bfromcstr(path);
            bcatcstr(sdir, "/.AppleDouble/.Parent");
            if (copy_file(-1, cfrombstr(sdir), cfrombstr(addir), 0666) != 0) {
                SLOG("Error copying %s -> %s", cfrombstr(sdir), cfrombstr(addir));
                badcp = rval = 1;
                break;
            }

            /* Get CNID of Parent and add new childir to CNID database */
            pdid = did;
            did = cnid_for_path(&dvolume.volinfo, &dvolume.volume, to.p_path);
            SLOG("got CNID: %u for path: %s", ntohl(did), to.p_path);

            struct adouble ad;
            struct stat st;
            if (stat(to.p_path, &st) != 0) {
                badcp = rval = 1;
                break;
            }
            ad_init(&ad, dvolume.volinfo.v_adouble, dvolume.volinfo.v_ad_options);
            if (ad_open_metadata(to.p_path, ADFLAGS_DIR, O_RDWR, &ad) != 0) {
                ERROR("Error opening adouble for: %s", to.p_path);
            }
            ad_setid( &ad, st.st_dev, st.st_ino, did, pdid, dvolume.db_stamp);
            ad_flush(&ad);
            ad_close_metadata(&ad);

            bdestroy(addir);
            bdestroy(sdir);
            umask(omask);
        }

        if (pflag) {
            if (setfile(statp, -1))
                rval = 1;
#if 0
            if (preserve_dir_acls(statp, curr->fts_accpath, to.p_path) != 0)
                rval = 1;
#endif
        }
        break;

    case S_IFBLK:
    case S_IFCHR:
        SLOG("%s is a device file (not copied).", path);
        break;
    case S_IFSOCK:
        SLOG("%s is a socket (not copied).", path);
        break;
    case S_IFIFO:
        SLOG("%s is a FIFO (not copied).", path);
        break;
    default:
        if (ftw_copy_file(ftw, path, statp, dne))
            badcp = rval = 1;

        SLOG("file: %s", to.p_path);

        if (svolume.volinfo.v_path && svolume.volinfo.v_adouble == AD_VERSION2 &&
            dvolume.volinfo.v_path && dvolume.volinfo.v_adouble == AD_VERSION2) {

            SLOG("ad for file: %s", to.p_path);

            if (dvolume.volume.vfs->vfs_copyfile(&dvolume.volume, -1, path, to.p_path))
                badcp = rval = 1;
            /* Get CNID of Parent and add new childir to CNID database */
            cnid_t cnid = cnid_for_path(&dvolume.volinfo, &dvolume.volume, to.p_path);
            SLOG("got CNID: %u for path: %s", ntohl(cnid), to.p_path);

            struct adouble ad;
            ad_init(&ad, dvolume.volinfo.v_adouble, dvolume.volinfo.v_ad_options);
            if (ad_open_metadata(to.p_path, 0, O_RDWR, &ad) != 0) {
                ERROR("Error opening adouble for: %s", to.p_path);
            }
            ad_setid( &ad, statp->st_dev, statp->st_ino, cnid, did, dvolume.db_stamp);
            ad_flush(&ad);
            ad_close_metadata(&ad);
        }
        break;
    }
    if (vflag && !badcp)
        (void)printf("%s -> %s\n", path, to.p_path);

    return 0;
}

static void siginfo(int sig _U_)
{
    sigint = 1;
}

/* Memory strategy threshold, in pages: if physmem is larger then this, use a large buffer */
#define PHYSPAGES_THRESHOLD (32*1024)

/* Maximum buffer size in bytes - do not allow it to grow larger than this */
#define BUFSIZE_MAX (2*1024*1024)

/* Small (default) buffer size in bytes. It's inefficient for this to be smaller than MAXPHYS */
#define MAXPHYS (64 * 1024)
#define BUFSIZE_SMALL (MAXPHYS)

static int ftw_copy_file(const struct FTW *entp,
                         const char *spath,
                         const struct stat *sp,
                         int dne)
{
    static char *buf = NULL;
    static size_t bufsize;
    ssize_t wcount;
    size_t wresid;
    off_t wtotal;
    int ch, checkch, from_fd = 0, rcount, rval, to_fd = 0;
    char *bufp;
    char *p;

    if ((from_fd = open(spath, O_RDONLY, 0)) == -1) {
        SLOG("%s: %s", spath, strerror(errno));
        return (1);
    }

    /*
     * If the file exists and we're interactive, verify with the user.
     * If the file DNE, set the mode to be the from file, minus setuid
     * bits, modified by the umask; arguably wrong, but it makes copying
     * executables work right and it's been that way forever.  (The
     * other choice is 666 or'ed with the execute bits on the from file
     * modified by the umask.)
     */
    if (!dne) {
#define YESNO "(y/n [n]) "
        if (nflag) {
            if (vflag)
                printf("%s not overwritten\n", to.p_path);
            (void)close(from_fd);
            return (0);
        } else if (iflag) {
            (void)fprintf(stderr, "overwrite %s? %s", 
                          to.p_path, YESNO);
            checkch = ch = getchar();
            while (ch != '\n' && ch != EOF)
                ch = getchar();
            if (checkch != 'y' && checkch != 'Y') {
                (void)close(from_fd);
                (void)fprintf(stderr, "not overwritten\n");
                return (1);
            }
        }
        
        if (fflag) {
            /* remove existing destination file name, 
             * create a new file  */
            (void)unlink(to.p_path);
            (void)dvolume.volume.vfs->vfs_deletefile(&dvolume.volume, -1, to.p_path);
            if (!lflag)
                to_fd = open(to.p_path, O_WRONLY | O_TRUNC | O_CREAT,
                             sp->st_mode & ~(S_ISUID | S_ISGID));
        } else {
            if (!lflag)
                /* overwrite existing destination file name */
                to_fd = open(to.p_path, O_WRONLY | O_TRUNC, 0);
        }
    } else {
        if (!lflag)
            to_fd = open(to.p_path, O_WRONLY | O_TRUNC | O_CREAT,
                         sp->st_mode & ~(S_ISUID | S_ISGID));
    }
    
    if (to_fd == -1) {
        SLOG("%s: %s", to.p_path, strerror(errno));
        (void)close(from_fd);
        return (1);
    }

    rval = 0;

    if (!lflag) {
        /*
         * Mmap and write if less than 8M (the limit is so we don't totally
         * trash memory on big files.  This is really a minor hack, but it
         * wins some CPU back.
         * Some filesystems, such as smbnetfs, don't support mmap,
         * so this is a best-effort attempt.
         */

        if (S_ISREG(sp->st_mode) && sp->st_size > 0 &&
            sp->st_size <= 8 * 1024 * 1024 &&
            (p = mmap(NULL, (size_t)sp->st_size, PROT_READ,
                      MAP_SHARED, from_fd, (off_t)0)) != MAP_FAILED) {
            wtotal = 0;
            for (bufp = p, wresid = sp->st_size; ;
                 bufp += wcount, wresid -= (size_t)wcount) {
                wcount = write(to_fd, bufp, wresid);
                if (wcount <= 0)
                    break;
                wtotal += wcount;
                if (wcount >= (ssize_t)wresid)
                    break;
            }
            if (wcount != (ssize_t)wresid) {
                SLOG("%s: %s", to.p_path, strerror(errno));
                rval = 1;
            }
            /* Some systems don't unmap on close(2). */
            if (munmap(p, sp->st_size) < 0) {
                SLOG("%s: %s", spath, strerror(errno));
                rval = 1;
            }
        } else {
            if (buf == NULL) {
                /*
                 * Note that buf and bufsize are static. If
                 * malloc() fails, it will fail at the start
                 * and not copy only some files. 
                 */ 
                if (sysconf(_SC_PHYS_PAGES) > 
                    PHYSPAGES_THRESHOLD)
                    bufsize = MIN(BUFSIZE_MAX, MAXPHYS * 8);
                else
                    bufsize = BUFSIZE_SMALL;
                buf = malloc(bufsize);
                if (buf == NULL)
                    ERROR("Not enough memory");

            }
            wtotal = 0;
            while ((rcount = read(from_fd, buf, bufsize)) > 0) {
                for (bufp = buf, wresid = rcount; ;
                     bufp += wcount, wresid -= wcount) {
                    wcount = write(to_fd, bufp, wresid);
                    if (wcount <= 0)
                        break;
                    wtotal += wcount;
                    if (wcount >= (ssize_t)wresid)
                        break;
                }
                if (wcount != (ssize_t)wresid) {
                    SLOG("%s: %s", to.p_path, strerror(errno));
                    rval = 1;
                    break;
                }
            }
            if (rcount < 0) {
                SLOG("%s: %s", spath, strerror(errno));
                rval = 1;
            }
        }
    } else {
        if (link(spath, to.p_path)) {
            SLOG("%s", to.p_path);
            rval = 1;
        }
    }
    
    /*
     * Don't remove the target even after an error.  The target might
     * not be a regular file, or its attributes might be important,
     * or its contents might be irreplaceable.  It would only be safe
     * to remove it if we created it and its length is 0.
     */

    if (!lflag) {
        if (pflag && setfile(sp, to_fd))
            rval = 1;
        if (pflag && preserve_fd_acls(from_fd, to_fd) != 0)
            rval = 1;
        if (close(to_fd)) {
            SLOG("%s: %s", to.p_path, strerror(errno));
            rval = 1;
        }
    }

    (void)close(from_fd);

    return (rval);
}

static int ftw_copy_link(const struct FTW *p,
                         const char *spath,
                         const struct stat *sstp,
                         int exists)
{
    int len;
    char llink[PATH_MAX];

    if ((len = readlink(spath, llink, sizeof(llink) - 1)) == -1) {
        SLOG("readlink: %s: %s", spath, strerror(errno));
        return (1);
    }
    llink[len] = '\0';
    if (exists && unlink(to.p_path)) {
        SLOG("unlink: %s: %s", to.p_path, strerror(errno));
        return (1);
    }
    if (symlink(llink, to.p_path)) {
        SLOG("symlink: %s: %s", llink, strerror(errno));
        return (1);
    }
    return (pflag ? setfile(sstp, -1) : 0);
}

static int setfile(const struct stat *fs, int fd)
{
    static struct timeval tv[2];
    struct stat ts;
    int rval, gotstat, islink, fdval;
    mode_t mode;

    rval = 0;
    fdval = fd != -1;
    islink = !fdval && S_ISLNK(fs->st_mode);
    mode = fs->st_mode & (S_ISUID | S_ISGID | S_ISVTX | S_IRWXU | S_IRWXG | S_IRWXO);

    TIMESPEC_TO_TIMEVAL(&tv[0], &fs->st_atim);
    TIMESPEC_TO_TIMEVAL(&tv[1], &fs->st_mtim);
    if (islink ? lutimes(to.p_path, tv) : utimes(to.p_path, tv)) {
        SLOG("%sutimes: %s", islink ? "l" : "", to.p_path);
        rval = 1;
    }
    if (fdval ? fstat(fd, &ts) :
        (islink ? lstat(to.p_path, &ts) : stat(to.p_path, &ts)))
        gotstat = 0;
    else {
        gotstat = 1;
        ts.st_mode &= S_ISUID | S_ISGID | S_ISVTX |
            S_IRWXU | S_IRWXG | S_IRWXO;
    }
    /*
     * Changing the ownership probably won't succeed, unless we're root
     * or POSIX_CHOWN_RESTRICTED is not set.  Set uid/gid before setting
     * the mode; current BSD behavior is to remove all setuid bits on
     * chown.  If chown fails, lose setuid/setgid bits.
     */
    if (!gotstat || fs->st_uid != ts.st_uid || fs->st_gid != ts.st_gid)
        if (fdval ? fchown(fd, fs->st_uid, fs->st_gid) :
            (islink ? lchown(to.p_path, fs->st_uid, fs->st_gid) :
             chown(to.p_path, fs->st_uid, fs->st_gid))) {
            if (errno != EPERM) {
                SLOG("chown: %s: %s", to.p_path, strerror(errno));
                rval = 1;
            }
            mode &= ~(S_ISUID | S_ISGID);
        }

    if (!gotstat || mode != ts.st_mode)
        if (fdval ? fchmod(fd, mode) : chmod(to.p_path, mode)) {
            SLOG("chmod: %s: %s", to.p_path, strerror(errno));
            rval = 1;
        }

#ifdef HAVE_ST_FLAGS
    if (!gotstat || fs->st_flags != ts.st_flags)
        if (fdval ?
            fchflags(fd, fs->st_flags) :
            (islink ? lchflags(to.p_path, fs->st_flags) :
             chflags(to.p_path, fs->st_flags))) {
            SLOG("chflags: %s: %s", to.p_path, strerror(errno));
            rval = 1;
        }
#endif

    return (rval);
}

static int preserve_fd_acls(int source_fd, int dest_fd)
{
#if 0
    acl_t acl;
    acl_type_t acl_type;
    int acl_supported = 0, ret, trivial;

    ret = fpathconf(source_fd, _PC_ACL_NFS4);
    if (ret > 0 ) {
        acl_supported = 1;
        acl_type = ACL_TYPE_NFS4;
    } else if (ret < 0 && errno != EINVAL) {
        warn("fpathconf(..., _PC_ACL_NFS4) failed for %s", to.p_path);
        return (1);
    }
    if (acl_supported == 0) {
        ret = fpathconf(source_fd, _PC_ACL_EXTENDED);
        if (ret > 0 ) {
            acl_supported = 1;
            acl_type = ACL_TYPE_ACCESS;
        } else if (ret < 0 && errno != EINVAL) {
            warn("fpathconf(..., _PC_ACL_EXTENDED) failed for %s",
                 to.p_path);
            return (1);
        }
    }
    if (acl_supported == 0)
        return (0);

    acl = acl_get_fd_np(source_fd, acl_type);
    if (acl == NULL) {
        warn("failed to get acl entries while setting %s", to.p_path);
        return (1);
    }
    if (acl_is_trivial_np(acl, &trivial)) {
        warn("acl_is_trivial() failed for %s", to.p_path);
        acl_free(acl);
        return (1);
    }
    if (trivial) {
        acl_free(acl);
        return (0);
    }
    if (acl_set_fd_np(dest_fd, acl, acl_type) < 0) {
        warn("failed to set acl entries for %s", to.p_path);
        acl_free(acl);
        return (1);
    }
    acl_free(acl);
#endif
    return (0);
}

static int preserve_dir_acls(const struct stat *fs, char *source_dir, char *dest_dir)
{
#if 0
    acl_t (*aclgetf)(const char *, acl_type_t);
    int (*aclsetf)(const char *, acl_type_t, acl_t);
    struct acl *aclp;
    acl_t acl;
    acl_type_t acl_type;
    int acl_supported = 0, ret, trivial;

    ret = pathconf(source_dir, _PC_ACL_NFS4);
    if (ret > 0) {
        acl_supported = 1;
        acl_type = ACL_TYPE_NFS4;
    } else if (ret < 0 && errno != EINVAL) {
        warn("fpathconf(..., _PC_ACL_NFS4) failed for %s", source_dir);
        return (1);
    }
    if (acl_supported == 0) {
        ret = pathconf(source_dir, _PC_ACL_EXTENDED);
        if (ret > 0) {
            acl_supported = 1;
            acl_type = ACL_TYPE_ACCESS;
        } else if (ret < 0 && errno != EINVAL) {
            warn("fpathconf(..., _PC_ACL_EXTENDED) failed for %s",
                 source_dir);
            return (1);
        }
    }
    if (acl_supported == 0)
        return (0);

    /*
     * If the file is a link we will not follow it
     */
    if (S_ISLNK(fs->st_mode)) {
        aclgetf = acl_get_link_np;
        aclsetf = acl_set_link_np;
    } else {
        aclgetf = acl_get_file;
        aclsetf = acl_set_file;
    }
    if (acl_type == ACL_TYPE_ACCESS) {
        /*
         * Even if there is no ACL_TYPE_DEFAULT entry here, a zero
         * size ACL will be returned. So it is not safe to simply
         * check the pointer to see if the default ACL is present.
         */
        acl = aclgetf(source_dir, ACL_TYPE_DEFAULT);
        if (acl == NULL) {
            warn("failed to get default acl entries on %s",
                 source_dir);
            return (1);
        }
        aclp = &acl->ats_acl;
        if (aclp->acl_cnt != 0 && aclsetf(dest_dir,
                                          ACL_TYPE_DEFAULT, acl) < 0) {
            warn("failed to set default acl entries on %s",
                 dest_dir);
            acl_free(acl);
            return (1);
        }
        acl_free(acl);
    }
    acl = aclgetf(source_dir, acl_type);
    if (acl == NULL) {
        warn("failed to get acl entries on %s", source_dir);
        return (1);
    }
    if (acl_is_trivial_np(acl, &trivial)) {
        warn("acl_is_trivial() failed on %s", source_dir);
        acl_free(acl);
        return (1);
    }
    if (trivial) {
        acl_free(acl);
        return (0);
    }
    if (aclsetf(dest_dir, acl_type, acl) < 0) {
        warn("failed to set acl entries on %s", dest_dir);
        acl_free(acl);
        return (1);
    }
    acl_free(acl);
#endif
    return (0);
}