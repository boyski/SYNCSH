/*
 * "syncsh" - See associated README for documentation.
 * This is written to the POSIX API and should work
 * on just about any Unix-like system.
 * I know of no reason it couldn't be ported to Windows;
 * it only needs to spawn a subprocess, direct its
 * stdout and stderr into a temp file, and synchronize
 * with other instances of itself. All these are easily
 * doable with the Win32 API. The main question would be
 * whether to rely on the Unix compatibility layer in the
 * C runtime library or to code directly to the Win32
 * layer. The former would require less coding but
 * would have more moving parts, the latter would mean
 * more conditionals here but might be more robust and
 * faster.
 */

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <libgen.h>
#include <regex.h>
#include <stdarg.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>

#define PFX			"SYNCSH_"

#define STREAM_OK(strm)		((fcntl(fileno((strm)), F_GETFD) != -1) || (errno != EBADF))

#ifdef EINTR
#define EINTR_CHECK(lhs,fcn)	while (((lhs)=fcn) == -1 && errno == EINTR)
#else
#define EINTR_CHECK(lhs,fcn)	((lhs) = (fcn))
#endif

#define is_absolute(path)	(*path == '/')

static char *prog = "??";
static char *recipe = "???";

static void
syserr_(const char *f, int l, int code, const char *ex)
{
    char *type = code ? "Error" : "Warning";
    char *msg = strerror(errno);

    if (ex) {
	fprintf(stderr, "%s:%d: %s: %s: %s: %s\n", f, l, prog, type, ex, msg);
    } else {
	fprintf(stderr, "%s:%d: %s: %s: %s\n", f, l, prog, type, msg);
    }

    if (code)
	exit(code);
}

#define syserr(code, ...)  syserr_(__FILE__, __LINE__, code, __VA_ARGS__);

#ifdef NDEBUG
#define dbg(fmt, ...)
#else
void
dbg(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    putc('\n', stderr);
    va_end(ap);
}
#endif

static void
usage(void)
{
    const char *fmt = "  %-18s %s\n";

    fprintf(stderr, "Usage: %s -<flags> <command>\n", prog);
    fprintf(stderr, "  " "where <flags> will typically be -c\n");
    fprintf(stderr, "Environment variables:\n");
    fprintf(stderr, fmt, PFX "HEADLINE:", "string to print before output");
    fprintf(stderr, fmt, PFX "SERIALIZE:", "pattern for serializable recipes");
    fprintf(stderr, fmt, PFX "SHELL:", "path of shell to hand off to");
    fprintf(stderr, fmt, PFX "TEE:", "file to which output will be appended");
    fprintf(stderr, fmt, PFX "VERBOSE:", "print recipe with this prefix");
    exit(1);
}

static void
vb(int fd, const char *prefix, char **argv)
{
    if (*prefix)
	write(fd, prefix, strlen(prefix));
    for (; *argv; argv++) {
	write(fd, *argv, strlen(*argv));
	write(fd, *(argv + 1) ? " " : "\n", 1);
    }
}

static uint16_t
str_hash(char *str, unsigned len)
{
    uint16_t hash = 0;
    uint16_t i = 0;

    for (i = 0; i < len; str++, i++) {
	hash = (*str) + (hash << 6) + (hash << 16) - hash;
    }

    return hash >> 1;
}

static void
pump_from_tmp_fd(int from_fd, int to_fd)
{
    ssize_t nleft, nwrite;
    char buffer[8192];

    if (lseek(from_fd, 0, SEEK_SET) == -1)
	perror("lseek()");

    while (1) {
	EINTR_CHECK(nleft, read(from_fd, buffer, sizeof(buffer)));
	if (nleft < 0)
	    perror("read()");
	else
	    while (nleft > 0) {
		EINTR_CHECK(nwrite, write(to_fd, buffer, nleft));
		if (nwrite < 0) {
		    perror("write()");
		    return;
		}

		nleft -= nwrite;
	    }

	if (nleft <= 0)
	    break;
    }
}

static void *
acquire_semaphore(int fd, pid_t pid, uint16_t off)
{
    static struct flock fl;

    fl.l_type = F_WRLCK;
    fl.l_whence = SEEK_SET;
    fl.l_pid = pid;
    fl.l_start = off;		/* lock just one byte */
    fl.l_len = 1;
    if (fcntl(fd, F_SETLKW, &fl) != -1) {
	//fprintf(stderr, "Locked byte %d.%u for '%s'\n", fd, off, recipe);
	return &fl;
    }
    perror("fcntl()");
    return NULL;
}

static void
release_semaphore(void *sem, int fd)
{
    struct flock *flp = (struct flock *)sem;

    flp->l_type = F_UNLCK;
    if (fcntl(fd, F_SETLKW, flp) == -1)
	perror("fcntl()");
}

int
main(int argc, char *argv[])
{
    int status = 0;
    int teefd = -1;
    int syncfd = -1;
    pid_t child;
    FILE *tempout = NULL;
    FILE *temperr = NULL;
    char *sh;
    char *tee;
    char *verbose = NULL;
    char *serialize;
    char *shargv[4];
    void *sem = NULL;
    pid_t thispid;

    prog = basename(argv[0]);

    if (argc <= 1 || !strcmp(argv[1], "-h") || strstr(argv[1], "help")) {
	usage();
    }

    thispid = getpid();

    recipe = argv[2];

    verbose = getenv(PFX "VERBOSE");

    if (!(sh = getenv(PFX "SHELL")))
	sh = "/bin/sh";

    /*
     * Here it looks like the shell is not being used to run a recipe.y
     * E.g. the makefile may contain a literal "$(SHELL) foobar.sh ..."
     * within a recipe or in some function such as $(shell foobar.sh).
     * Since we're not running a recipe we don't need to worry about
     * synchronizing with other recipes.
     * Special case of the above: if MAKELEVEL is not present at all,
     * that means we're (a) in a top-level (non-recursive) make process
     * and (b) we're not running a recipe, since MAKELEVEL is incremented
     * and exported for each recipe.
     */
    if (argc != 3 || argv[1][0] != '-' || !strchr(argv[1], 'c')
	|| argv[2][0] == '-' || !getenv("MAKELEVEL")) {
	argv[0] = sh;
	if (verbose) {
	    fflush(stderr);
	    vb(fileno(stderr), verbose, argv);
	}
	execvp(argv[0], argv);
	syserr(2, argv[0]);
    }

    shargv[0] = sh;
    shargv[1] = argv[1];
    shargv[2] = recipe;
    shargv[3] = NULL;

    if (STREAM_OK(stdout)) {
	syncfd = fileno(stdout);
    } else if (STREAM_OK(stderr)) {
	syncfd = fileno(stderr);
    } else {
	syserr(0, "stdout");
    }

    if (verbose)
	vb(fileno(temperr), verbose, shargv + 2);

    /*
     * We could be asked to serialize a certain type of recipe
     * in which case the semaphore is acquired *before* the fork
     * and we don't need to bother about tempfiles.
     */
    if ((serialize = getenv(PFX "SERIALIZE"))) {
	regex_t re;

	if (regcomp(&re, serialize, REG_EXTENDED)) {
	    /* TODO - generate a better error message */
	    fprintf(stderr, "%s: Error: bad regular expression '%s'\n", prog,
		    serialize);
	} else {
	    regmatch_t pm[1];

	    if (!regexec(&re, recipe, 1, pm, 0)) {
		uint16_t hash;

		hash = str_hash(serialize, strlen(serialize));
		sem = acquire_semaphore(syncfd, thispid, hash);
	    }
	    regfree(&re);
	}
    }

    /* Otherwise, * prepare the tempfiles. */
    if (!sem) {
	if (!(tempout = tmpfile()) || !(temperr = tmpfile())) {
	    syserr(2, "tmpfile");
	}
    }

    /* GNU make uses vfork so we do too */
    child = vfork();
    if (child == (pid_t) 0) {
	if (tempout && (close(fileno(stdout)) == -1
			|| (dup2(fileno(tempout), fileno(stdout)) == -1)))
	    syserr(2, "dup2(stdout)");

	if (temperr && (close(fileno(stderr)) == -1
			|| (dup2(fileno(temperr), fileno(stderr)) == -1)))
	    syserr(2, "dup2(stderr)");

	execvp(shargv[0], shargv);
	perror(shargv[0]);
	exit(EXIT_FAILURE);
    } else if (child == (pid_t) - 1) {
	syserr(2, "fork");
    }

    waitpid(child, &status, 0);

    if (tempout && lseek(fileno(tempout), 0, SEEK_SET) == -1)
	syserr(2, "lseek(stdout)");

    if (temperr && lseek(fileno(temperr), 0, SEEK_SET) == -1)
	syserr(2, "lseek(stderr)");

    if ((tee = getenv(PFX "TEE"))) {
	if (!is_absolute(tee)) {
	    fprintf(stderr, "%s: Error: '%s' not an absolute path\n",
		    prog, tee);
	    return 2;
	}
	teefd = open(tee, O_APPEND | O_WRONLY | O_CREAT, 0644);
    }

    if (!sem && (sem = acquire_semaphore(syncfd, thispid, 0))) {
	char *headline;

	/*
	 * We've entered the "critical section" during which a lock is held.
	 * We want to keep it as short as possible.
	 */

	if ((headline = getenv(PFX "HEADLINE"))) {
	    write(fileno(stdout), headline, strlen(headline));
	    write(fileno(stdout), "\n", 1);
	}

	if (teefd > 0) {
	    lseek(teefd, 0, SEEK_END);
	    if (headline) {
		write(teefd, headline, strlen(headline));
		write(teefd, "\n", 1);
	    }
	}

	if (tempout) {
	    pump_from_tmp_fd(fileno(tempout), fileno(stdout));
	    if (teefd > 0)
		pump_from_tmp_fd(fileno(tempout), teefd);
	    fclose(tempout);
	}
	if (temperr && temperr != tempout) {
	    pump_from_tmp_fd(fileno(temperr), fileno(stderr));
	    if (teefd > 0)
		pump_from_tmp_fd(fileno(temperr), teefd);
	    fclose(temperr);
	}
    }

    /* Exit the critical section */
    if (sem)
	release_semaphore(sem, syncfd);

    close(syncfd);

    return status >> 8;
}
