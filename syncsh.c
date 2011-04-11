/*
 * "syncsh" - See associated README for documentation.
 * This is written to the POSIX API and should work
 * on just about any Unix-like system.
 * I know of no reason it couldn't be ported to Windows;
 * it only needs to spawn a subprocess, directing its
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
#include <stdarg.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>

#define PFX	"SYNCSH_"

#define is_absolute(path)		(*path == '/')

static char *prog = "???";

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
    const char *fmt = "  %-16s %s\n";

    fprintf(stderr, "Usage: %s -<flags> <command>\n", prog);
    fprintf(stderr, "  " "where <flags> will typically be -c\n");
    fprintf(stderr, "Environment variables:\n");
    fprintf(stderr, fmt, PFX "HEADLINE:", "string to print before output");
    fprintf(stderr, fmt, PFX "SHELL:", "path of shell to hand off to");
    fprintf(stderr, fmt, PFX "SYNCFILE:", "full path to a writable lock file");
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

int
main(int argc, char *argv[])
{
    int status = 0;
    int teefd = -1;
    int syncfd = -1;
    pid_t child;
    FILE *tempout;
    FILE *temperr;
    char buffer[8192];
    char *sh;
    char *recipe;
    char *tee;
    char *syncfile;
    char *verbose = NULL;
    char *shargv[4];
    ssize_t nread;
    char *headline;
    struct flock fl;

    prog = basename(argv[0]);

    if (argc <= 1 || !strcmp(argv[1], "-h") || strstr(argv[1], "help")) {
	usage();
    }

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
    shargv[2] = recipe = argv[2];
    shargv[3] = NULL;

    if (!(tempout = tmpfile()) || !(temperr = tmpfile())) {
	syserr(2, "tmpfile");
    }

    if (verbose)
	vb(fileno(temperr), verbose, shargv + 2);

    /* GNU make uses vfork so we do too */
    child = vfork();
    if (child == (pid_t) 0) {
	int outfd = fileno(stdout);
	int errfd = fileno(stderr);

	if ((close(outfd) == -1)
	    || (dup2(fileno(tempout), outfd) == -1)
	    || (close(errfd) == -1)
	    || (dup2(fileno(temperr), errfd) == -1))
	    syserr(2, "dup2");
	execvp(shargv[0], shargv);
	perror(shargv[0]);
	exit(EXIT_FAILURE);
    } else if (child == (pid_t) - 1) {
	syserr(2, "fork");
    }

    waitpid(child, &status, 0);

    if (lseek(fileno(tempout), 0, SEEK_SET) == -1
	|| lseek(fileno(temperr), 0, SEEK_SET) == -1)
	syserr(2, "lseek");

    syncfile = getenv(PFX "SYNCFILE");

    if ((tee = getenv(PFX "TEE"))) {
	if (!is_absolute(tee)) {
	    fprintf(stderr, "%s: Error: '%s' not an absolute path\n",
		    prog, tee);
	    return 2;
	}
	teefd = open(tee, O_APPEND | O_WRONLY | O_CREAT, 0644);
	if (!syncfile) {
	    syncfile = tee;
	    syncfd = teefd;
	}
    } else if (!syncfile) {
	char *makelist, *t1, *t2, *lf;

	/*
	 * If MAKEFILE_LIST was exported we can get the makefile
	 * name from it.
	 */
	if ((makelist = getenv("MAKEFILE_LIST"))
	    && (t1 = strdup(makelist))
	    && (lf = malloc(PATH_MAX + 1))) {

	    makelist = t1;
	    while (isspace((int)*t1))
		t1++;
	    if ((t2 = strchr(t1, ' ')))
		*t2 = '\0';
	    if (!realpath(t1, lf))
		syserr(2, t1);
	    syncfile = lf;
	    free(makelist);
	} else {
	    fprintf(stderr, "%s: Error: no syncfile\n", prog);
	    return 2;
	}
    }

    if (!is_absolute(syncfile)) {
	fprintf(stderr, "%s: Error: '%s' not an absolute path\n",
		prog, syncfile);
	return 2;
    }

    /*
     * Note that we NEVER write to the syncfile but must open
     * it for write in order for fcntl() to acquire the lock.
     */
    if (syncfd == -1 && (syncfd = open(syncfile, O_WRONLY | O_APPEND)) == -1)
	syserr(0, syncfile);

    /*
     * fcntl locks are preferred but we can fall back to flock()
     * if need be.  An alternative would be to synchronize on
     * a semaphore rather than a file but (a) file locking is older
     * and more portable and (b) file locks go away on program exit
     * whereas POSIX semaphores need to be released which causes
     * fragility. A Windows port might prefer semaphores though.
     */


#ifdef F_LOCK
    memset(&fl, 0, sizeof(fl));
    fl.l_type = F_WRLCK;
    fl.l_whence = SEEK_SET;
    fl.l_pid = getpid();
    if (fcntl(syncfd, F_SETLKW, &fl) == -1)
	syserr(0, syncfile);
#else
    if (flock(syncfd, LOCK_EX))
	syserr(0, syncfile);
#endif

    /*
     * Now in the "critical section" during which the lock is held.
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

    while ((nread = fread(buffer, 1, sizeof(buffer), tempout)) > 0) {
	write(fileno(stdout), buffer, nread);
	if (teefd > 0)
	    write(teefd, buffer, nread);
    }

    while ((nread = fread(buffer, 1, sizeof(buffer), temperr)) > 0) {
	write(fileno(stderr), buffer, nread);
	if (teefd > 0)
	    write(teefd, buffer, nread);
    }

    /* Exit the critical section */
#ifdef F_LOCK
    fl.l_type = F_UNLCK;
    if (fcntl(syncfd, F_SETLKW, &fl) == -1)
	syserr(0, syncfile);
#else
    if (flock(syncfd, LOCK_UN))
	syserr(0, syncfile);
#endif
    close(syncfd);

    (void)fclose(tempout);
    (void)fclose(temperr);

    return status >> 8;
}
