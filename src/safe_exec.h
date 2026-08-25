#ifndef SCREENPLUS_SAFE_EXEC_H
#define SCREENPLUS_SAFE_EXEC_H

/* Shell-free process execution helpers.
 *
 * Every helper runs a program directly through fork + execv with a fixed
 * argument vector, so no string is ever interpreted by /bin/sh. Callers must
 * pass absolute paths: execv performs no PATH lookup, which also keeps the
 * forked child async-signal-safe until the exec.
 *
 * Return 0 on success, -1 on failure. */

/* Run argv with stdout and stderr discarded; report only the exit status. */
int safe_exec_quiet(const char *const argv[]);

/* Run argv with stderr discarded and capture the first line of stdout into
 * buffer (trailing newline/whitespace trimmed). Fails when the command exits
 * non-zero or produces no output. */
int safe_exec_line(const char *const argv[], char *buffer, unsigned int size);

#endif
