#define _POSIX_C_SOURCE 200809L

#include "safe_exec.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static const char *const DEVNULL_PATH = "/dev/null";

/* Common child setup shared by the capture and quiet variants:
 * redirect the given descriptors to /dev/null and exec argv[0].
 * Never returns on success. */
static void exec_child(const char *const argv[], int stdout_target, int stderr_target)
{
	if (stdout_target != STDOUT_FILENO) {
		if (dup2(stdout_target, STDOUT_FILENO) < 0)
			_exit(127);
	}
	if (stderr_target != STDERR_FILENO) {
		if (dup2(stderr_target, STDERR_FILENO) < 0)
			_exit(127);
	}
	if (stdout_target > STDERR_FILENO)
		close(stdout_target);
	if (stderr_target > STDERR_FILENO && stderr_target != stdout_target)
		close(stderr_target);
	execv(argv[0], (char *const *)argv);
	_exit(127);
}

/* Wait for the child started by fork() and map its exit status to 0/-1.
 * Returns -2 when waitpid itself failed so callers can log it. */
static int wait_child(pid_t child)
{
	int status = 0;
	while (waitpid(child, &status, 0) < 0) {
		if (errno != EINTR)
			return -2;
	}
	if (WIFEXITED(status) && WEXITSTATUS(status) == 0)
		return 0;
	return -1;
}

int safe_exec_quiet(const char *const argv[])
{
	if (!argv || !argv[0])
		return -1;
	int devnull = open(DEVNULL_PATH, O_WRONLY);
	if (devnull < 0)
		return -1;
	pid_t child = fork();
	if (child < 0) {
		close(devnull);
		return -1;
	}
	if (child == 0)
		exec_child(argv, devnull, devnull);
	close(devnull);
	int result = wait_child(child);
	return result == -2 ? -1 : result;
}

int safe_exec_line(const char *const argv[], char *buffer, unsigned int size)
{
	if (!argv || !argv[0] || !buffer || size < 2)
		return -1;
	buffer[0] = '\0';
	int devnull = open(DEVNULL_PATH, O_WRONLY);
	int pipes[2];
	if (devnull < 0 || pipe(pipes) != 0) {
		if (devnull >= 0)
			close(devnull);
		return -1;
	}
	pid_t child = fork();
	if (child < 0) {
		close(devnull);
		close(pipes[0]);
		close(pipes[1]);
		return -1;
	}
	if (child == 0) {
		close(pipes[0]);
		exec_child(argv, pipes[1], devnull);
	}
	close(pipes[1]);
	close(devnull);
	FILE *stream = fdopen(pipes[0], "r");
	if (!stream) {
		close(pipes[0]);
		if (wait_child(child) == -2)
			return -1;
		return -1;
	}
	char *result = fgets(buffer, (int)size, stream);
	int read_error = ferror(stream);
	fclose(stream);
	int status = wait_child(child);
	if (!result || read_error || status != 0) {
		buffer[0] = '\0';
		return -1;
	}
	/* Trim trailing newline and whitespace, same contract as run_line(). */
	size_t length = strlen(buffer);
	while (length && (buffer[length - 1] == '\n' || buffer[length - 1] == '\r' ||
			  buffer[length - 1] == ' ' || buffer[length - 1] == '\t'))
		buffer[--length] = '\0';
	return 0;
}
