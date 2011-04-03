/*
 * "syncsh" - See associated README for documentation.
 * This is written to the POSIX API and should work
 * on just about any Unix-like system.
 * I know of no reason it couldn't be ported to Windows.
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
#define BAR1	"------------------------------------------------------\n"
#define BAR2	"======================================================\n"

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
    fprintf(stderr, "Usage: %s -<flags> <command>\n", prog);
    fprintf(stderr, "  " "where <flags> will typically be -c\n");
    fprintf(stderr, "Environment variables:\n");
    fprintf(stderr, "  " PFX "INTERACTIVE: debugging mode\n");
    fprintf(stderr, "  " PFX "LOCKFILE: full path to a writable lock file\n");
    fprintf(stderr, "  " PFX "SHELL: path of the shell to hand off to\n");
    fprintf(stderr, "  " PFX "TEE: file to which output will be appended\n");
    fprintf(stderr, "  " PFX "VERBOSE: nonzero int for extra verbosity\n");
    exit(1);
}

int
main(int argc, char *argv[])
{
    int status = 0;
    int teefd = -1;
    int lockfd = -1;
    int verbose = 0;
    pid_t child;
    FILE *tempfp;
    char buffer[8192];
    ssize_t bytesRead;
    char *sh;
    char *recipe;
    char *tee;
    char *lockfile;
    char *shargv[4];

    prog = basename(argv[0]);

    if (argc <= 1 || !strcmp(argv[1], "-h") || strstr(argv[1], "help")) {
	usage();
    }

    if (!(sh = getenv(PFX "SHELL")))
	sh = "/bin/sh";

    /*
     * Here it looks like the shell is being used in a non-standard way
     * by the makefile itself. E.g. the makefile may contain a literal
     * "$(SHELL) script ..." in a recipe or in some function.
     * In this case we don't have to worry about synchronization
     * because we're not in charge of a recipe.
     */
    if (argc != 3 || argv[1][0] != '-' || !strchr(argv[1], 'c')
	|| argv[2][0] == '-') {
	argv[0] = sh;
	execvp(argv[0], argv);
	syserr(2, argv[0]);
    }

    shargv[0] = sh;
    shargv[1] = argv[1];
    shargv[2] = recipe = argv[2];
    shargv[3] = NULL;

    if ((tempfp = tmpfile()) == NULL) {
	syserr(2, "tmpfile");
    }

    if (getenv(PFX "INTERACTIVE")) {
	write(STDOUT_FILENO, "++ ", 3);
	write(STDOUT_FILENO, recipe, strlen(recipe));
	write(STDOUT_FILENO, "\n", 1);

	child = fork();
	if (child == (pid_t) 0) {
	    putenv("PS1=>> ");
	    execlp(shargv[0], shargv[0], "-i", (char *)0);
	    perror(shargv[0]);
	    exit(2);
	} else if (child == (pid_t) - 1) {
	    syserr(2, "fork");
	}

	waitpid(child, &status, 0);
	if (status)
	    return status >> 8;
    }

    child = fork();
    if (child == (pid_t) 0) {
	close(1);
	dup2(fileno(tempfp), 1);
	close(2);
	dup2(fileno(tempfp), 2);
	execvp(shargv[0], shargv);
	perror(shargv[0]);
	exit(2);
    } else if (child == (pid_t) - 1) {
	syserr(2, "fork");
    }

    waitpid(child, &status, 0);

    fseek(tempfp, 0, SEEK_SET);

    lockfile = getenv(PFX "LOCKFILE");

    if ((tee = getenv(PFX "TEE"))) {
	if (!is_absolute(tee)) {
	    fprintf(stderr, "%s: Error: '%s' not an absolute path\n",
		    prog, tee);
	    return 2;
	}
	teefd = open(tee, O_APPEND | O_WRONLY | O_CREAT, 0644);
	if (!lockfile) {
	    lockfile = tee;
	    lockfd = teefd;
	}
    } else if (!lockfile) {
	char *makelist, *t1, *t2, *lf;

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
	    lockfile = lf;
	    free(makelist);
	} else {
	    fprintf(stderr, "%s: Error: no lockfile\n", prog);
	    return 2;
	}
    }

    if (!is_absolute(lockfile)) {
	fprintf(stderr, "%s: Error: '%s' not an absolute path\n",
		prog, lockfile);
	return 2;
    }

    verbose = getenv(PFX "VERBOSE") ? atoi(getenv(PFX "VERBOSE")) : 0;

    /*
     * Note that we NEVER write to the lockfile but must open
     * it for write in order for lockf() to acquire the lock.
     */
    if (lockfd == -1 && (lockfd = open(lockfile, O_WRONLY)) == -1)
	syserr(2, lockfile);

    /*
     * Lockf() is preferred because it works over NFS but we can
     * fall back to flock() if need be. As of some date Cygwin did
     * not have lockf().
     */
#ifdef F_LOCK
    if (lockf(lockfd, F_LOCK, 0) == 0) {
#else
    if (flock(lockfd, LOCK_EX) == 0) {
#endif
	/*
	 * This is the "critical section" during which the lock is held.
	 * We want to keep it as short as possible.
	 */
	if (verbose) {
	    write(STDOUT_FILENO, recipe, strlen(recipe));
	    write(STDOUT_FILENO, "\n", 1);
	}
	if (teefd > 0) {
	    lseek(teefd, 0, SEEK_END);
	    if (verbose) {
		write(teefd, BAR1, sizeof(BAR1) - 1);
		write(teefd, recipe, strlen(recipe));
		write(teefd, "\n", 1);
		write(teefd, BAR2, sizeof(BAR2) - 1);
	    }
	}
	while ((bytesRead = fread(buffer, 1, sizeof(buffer), tempfp)) > 0) {
	    write(STDOUT_FILENO, buffer, bytesRead);
	    if (teefd > 0)
		write(teefd, buffer, bytesRead);
	}
	fclose(tempfp);
    } else {
	syserr(2, lockfile);
    }

    return status >> 8;
}
