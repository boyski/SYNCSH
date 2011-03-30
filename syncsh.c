#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
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

int
main(int argc, char *argv[])
{
    int status = 0;
    int teefd = -1;
    int lockfd = -1;
    pid_t child;
    FILE *tempfp;
    char buffer[8192];
    ssize_t bytesRead;
    char *sh;
    char *recipe;
    char *tee;
    char *makelist;
    char *lockfile;
    char *verbose;
    char *shargv[4];

    prog = basename(argv[0]);

    if (argc != 3 || argv[1][0] != '-') {
	fprintf(stderr, "Usage: %s -<flags> <command>\n", basename(argv[0]));
	fprintf(stderr, "  " "where <flags> will typically be -c but may be any single word\n");
	fprintf(stderr, "Environment variables:\n");
	fprintf(stderr, "  " PFX "SHELL: path of the shell to hand off to\n");
	fprintf(stderr, "  " PFX "LOCKFILE: full path to a writable file for locking\n");
	fprintf(stderr, "  " PFX "TEE: file to which output will be appended\n");
	fprintf(stderr, "  " PFX "VERBOSE: non-null and non-zero for additional verbosity\n");
	return 1;
    }

    if (!(sh = getenv(PFX "SHELL")))
	sh = "/bin/sh";

    shargv[0] = sh;
    shargv[1] = argv[1];
    shargv[2] = recipe = argv[2];
    shargv[3] = NULL;

    /*
     * The $SHELL value is also used for the $(shell ...) function
     * which has no parallelism requirements. It appears that the
     * MAKEFILE_LIST variable may not be set when $(shell) assignments
     * run, so this is a shortcut opportunity.
     */
    if (!(makelist = getenv("MAKEFILE_LIST"))) {
	execvp(shargv[0], shargv);
	perror(shargv[0]);
	exit(2);
    }

    if ((tempfp = tmpfile()) == NULL) {
	syserr(2, "tmpfile");
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
	teefd = open(tee, O_APPEND | O_WRONLY | O_CREAT, 0644);
	if (!lockfile) {
	    lockfile = tee;
	    lockfd = teefd;
	}
    } else if (!lockfile) {
	if (makelist && (lockfile = strdup(makelist))) {
	    char *t;

	    while (isspace((int)*lockfile))
		lockfile++;
	    if ((t = strchr(lockfile, ' ')))
		*t = '\0';
	} else {
	    fprintf(stderr, "%s: Error: no lockfile\n", prog);
	    return 2;
	}
    }

    verbose = getenv(PFX "VERBOSE");

    /*
     * Note that we never write to the lockfile but must
     * open it for write in order to acquire the lock.
     */
    if (lockfd == -1 && (lockfd = open(lockfile, O_WRONLY)) == -1)
	syserr(2, lockfile);

    if (lockf(lockfd, F_LOCK, 0) == 0) {
	/*
	 * This is the "critical section" during which the lock is held.
	 * We want to keep it as short as possible.
	 */
	while ((bytesRead = fread(buffer, 1, sizeof(buffer), tempfp)) > 0) {
	    if (verbose) {
		write(1, recipe, strlen(recipe));
		write(1, "\n", 1);
	    }
	    write(1, buffer, bytesRead);

	    if (teefd > 0) {
		lseek(teefd, 0, SEEK_END);
		write(teefd, recipe, strlen(recipe));
		write(teefd, "\n", 1);
		write(teefd, buffer, bytesRead);
		write(teefd, "\n", 1);
	    }
	}
	fclose(tempfp);
    } else {
	syserr(2, lockfile);
    }

    return status;
}
