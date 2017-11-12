/*
 * Tyler Filla
 * CS 4760
 * Assignment 5
 */

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <sys/wait.h>
#include <unistd.h>

#include "clock.h"
#include "resmgr.h"

#define DEFAULT_LOG_FILE_PATH "oss.log"
#define MAX_PROCESSES 18

static struct
{
    /** The desired path to the log file. */
    char* log_file_path;

    /** Nonzero to indicate verbose mode, otherwise zero. */
    int verbose;

    /** The open log file. */
    FILE* log_file;

    /** The outgoing clock instance. */
    clock_s* clock;

    /** The resource manager instance. */
    resmgr_s* resmgr;

    /** The current number of child processes. */
    volatile sig_atomic_t num_child_procs;

    /** The pid of the last dead child process. */
    volatile sig_atomic_t last_child_proc_dead;

    /** Nonzero once SIGINT received. */
    volatile sig_atomic_t interrupted;
} g;

static void handle_exit()
{
    if (g.clock)
    {
        // Get stop time
        unsigned int stop_nanos = 0;
        unsigned int stop_seconds = 0;
        if (clock_lock(g.clock) == 0)
        {
            // Get stop time
            stop_nanos = clock_get_nanos(g.clock);
            stop_seconds = clock_get_seconds(g.clock);

            // Unlock the clock
            clock_unlock(g.clock);
        }

        if (g.interrupted)
        {
            fprintf(stderr, "\n--- interrupted; dumping information about last run ---\n");
            fprintf(stderr, "log file: %s\n", g.log_file_path);
            fprintf(stderr, "time now: %ds, %dns\n", stop_seconds, stop_nanos);
        }
    }

    // Clean up IPC-heavy components
    if (g.clock)
    {
        clock_delete(g.clock);
    }
    if (g.resmgr)
    {
        resmgr_delete(g.resmgr);
    }

    // Close log file
    if (g.log_file)
    {
        fclose(g.log_file);
    }
    if (g.log_file_path)
    {
        free(g.log_file_path);
    }
}

static void handle_sigchld(int sig)
{
    // Decrement number of child processes
    g.num_child_procs--;

    // printf(3) is not signal-safe
    // POSIX allows the use of write(2), but this is unformatted
    char death_notice_msg[] = "received a child process death notice\n";
    write(STDOUT_FILENO, death_notice_msg, sizeof(death_notice_msg));

    // Get and record the pid
    // Hopefully we can report it in time
    pid_t pid = wait(NULL);
    g.last_child_proc_dead = pid;
}

static void handle_sigint(int sig)
{
    // Set interrupted flag
    g.interrupted = 1;
}

static pid_t launch_child()
{
    if (g.interrupted)
        return -1;

    int child_pid = fork();
    if (child_pid == 0)
    {
        // Fork succeeded, now in child

        // Redirect child stderr and stdout to log file
        // This is a hack to allow logging from children without communicating the log fd
        //dup2(fileno(g.log_file), STDERR_FILENO);
        //dup2(fileno(g.log_file), STDOUT_FILENO);

        // Swap in the child image
        if (execv("./child", (char* []) { "./child", NULL }))
        {
            perror("launch child failed (in child): execv(2) failed");
        }

        _Exit(1);
    }
    else if (child_pid > 0)
    {
        // Increment number of child processes
        g.num_child_procs++;

        // Fork succeeded, now in parent
        return child_pid;
    }
    else
    {
        // Fork failed, still in parent
        perror("launch child failed (in parent): fork(2) failed");
        return -1;
    }
}

static void print_help(FILE* dest, const char* executable_name)
{
    fprintf(dest, "Usage: %s [option...]\n\n", executable_name);
    fprintf(dest, "Supported options:\n");
    fprintf(dest, "    -h          Display this information\n");
    fprintf(dest, "    -l <file>   Log events to <file> (default oss.log)\n");
    fprintf(dest, "    -v          Verbose mode\n");
}

static void print_usage(FILE* dest, const char* executable_name)
{
    fprintf(dest, "Usage: %s [option..]\n", executable_name);
    fprintf(dest, "Try `%s -h' for more information.\n", executable_name);
}

int main(int argc, char* argv[])
{
    atexit(&handle_exit);
    srand((unsigned int) time(NULL));

    g.log_file_path = strdup(DEFAULT_LOG_FILE_PATH);

    // Handle command-line options
    int opt;
    while ((opt = getopt(argc, argv, "hlv:")) != -1)
    {
        switch (opt)
        {
        case 'h':
            print_help(stdout, argv[0]);
            return 0;
        case 'l':
            free(g.log_file_path);
            g.log_file_path = strdup(optarg);
            if (!g.log_file_path)
            {
                perror("global.log_file_path not allocated: strdup(3) failed");
                return 1;
            }
            break;
        case 'v':
            break;
        default:
            fprintf(stderr, "invalid option: -%c\n", opt);
            print_usage(stderr, argv[0]);
            return 1;
        }
    }

    if (!g.log_file_path)
    {
        fprintf(stderr, "global.log_file_path not allocated\n");
        return 1;
    }

    // Open log file for appending
    //g.log_file = fopen(g.log_file_path, "w");
    if (!g.log_file)
    {
        perror("unable to open log file, so logging will not occur");
    }

    // Redirect stdout to the log file
    // We will communicate on the terminal using stderr
    //dup2(fileno(g.log_file), STDOUT_FILENO);

    // Register handler for SIGCHLD signal (to know when children die)
    struct sigaction sigaction_sigchld = {};
    sigaction_sigchld.sa_handler = &handle_sigchld;
    if (sigaction(SIGCHLD, &sigaction_sigchld, NULL))
    {
        perror("cannot handle SIGCHLD: sigaction(2) failed, this is a fatal error");
        return 2;
    }

    // Register handler for SIGINT signal (^C at terminal)
    struct sigaction sigaction_sigint = {};
    sigaction_sigint.sa_handler = &handle_sigint;
    if (sigaction(SIGINT, &sigaction_sigint, NULL))
    {
        perror("cannot handle SIGINT: sigaction(2) failed, so manual IPC cleanup possible");
    }

    // Create and start outgoing clock
    g.clock = clock_new(CLOCK_MODE_OUT);

    // Create server-side resource manager instance
    g.resmgr = resmgr_new(RESMGR_SIDE_SERVER);

    fprintf(stderr, "press ^C to stop the simulation\n");

    // Time of last iteration
    unsigned long last_time = 0;

    while (1)
    {
        //
        // Simulate Clock
        //

        // Lock the clock
        if (clock_lock(g.clock))
            return 1;

        // Generate a time between 1 and 1000 milliseconds
        // This duration of time passage will be simulated this iteration
        unsigned int dn = rand() % 1000000000u; // NOLINT

        // Advance the clock
        clock_advance(g.clock, dn, 0);

        // Get latest time from clock
        unsigned int now_nanos = clock_get_nanos(g.clock);
        unsigned int now_seconds = clock_get_seconds(g.clock);
        unsigned long now_time = now_seconds * 1000000000ul + now_nanos;

        // Unlock the clock
        if (clock_unlock(g.clock))
            return 1;

        //
        // Simulate OS Duties
        //

        // Report child process deaths
        if (g.last_child_proc_dead)
        {
            printf("process %d has died\n", g.last_child_proc_dead);
            g.last_child_proc_dead = 0;
        }

        // If we should try to spawn a child process
        // We do so on first iteration or on subsequent iterations spaced out by 1 to 500 milliseconds
        if (last_time == 0 || (now_time - last_time >= (rand() % 500) * 1000000u)) // NOLINT
        {
            // If there is room for another process
            if (g.num_child_procs < MAX_PROCESSES)
            {
                // Launch a child process
                pid_t child = launch_child();

                printf("spawned a new process: %d\n", child);
                printf("there are now %d processes in the system\n", g.num_child_procs);
            }
        }

        // Update last iteration time
        last_time = now_time;

        // Break loop on interrupt
        if (g.interrupted)
            break;

        usleep(100000);
    }

    return 0;
}
