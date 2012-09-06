// Copyright (C)2008-2012 Laurence Tratt http://tratt.net/laurie/
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to
// deal in the Software without restriction, including without limitation the
// rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
// sell copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
// FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
// IN THE SOFTWARE.


#include "Config.h"

#include <assert.h>
#include <err.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "multitime.h"
#include "format.h"



#define BUFFER_SIZE (64 * 1024)


extern char* __progname;

void usage(int, char *);
void execute_cmd(Conf *, Cmd *, int);
FILE *read_input(Conf *, Cmd *, int);
bool fcopy(FILE *, FILE *);
char *replace(Conf *, Cmd *, const char *, int);
char escape_char(char);

#ifdef MT_HAVE_RANDOM
#define RANDN(n) (random() % n)
#else
#define RANDN(n) (rand() % n)
#endif



////////////////////////////////////////////////////////////////////////////////
// Running commands
//

#include <fcntl.h>

void execute_cmd(Conf *conf, Cmd *cmd, int runi)
{
    if (conf->verbosity > 0) {
        fprintf(stderr, "===> Executing ");
        pp_cmd(conf, cmd);
        fprintf(stderr, "\n");
    }

    FILE *tmpf = NULL;
    if (cmd->input_cmd)
        tmpf = read_input(conf, cmd, runi);

    FILE *outtmpf = NULL;
    char *output_cmd = replace(conf, cmd, cmd->output_cmd, runi);
    if (output_cmd) {
        outtmpf = tmpfile();
        if (!outtmpf)
            goto cmd_err;
    }

    struct rusage *ru = cmd->rusages[runi] =
      malloc(sizeof(struct rusage));

    // Note: we want to do as little stuff in either parent or child between the
    // two gettimeofday calls, otherwise we might interfere with the timings.

    struct timeval startt;
    gettimeofday(&startt, NULL);
    pid_t pid = fork();
    if (pid == 0) {
        // Child
        if (tmpf && dup2(fileno(tmpf), STDIN_FILENO) == -1)
            goto cmd_err;

        if (cmd->quiet && freopen("/dev/null", "w", stdout) == NULL)
            goto cmd_err;
        else if (output_cmd && dup2(fileno(outtmpf), STDOUT_FILENO) == -1)
            goto cmd_err;
        execvp(cmd->argv[0], cmd->argv);
        goto cmd_err;
    }

    // Parent
    
    int status;
    wait4(pid, &status, 0, ru);
    struct timeval endt;
    gettimeofday(&endt, NULL);

    if (tmpf)
        fclose(tmpf);

    struct timeval *tv = cmd->timevals[runi] = malloc(sizeof(struct timeval));
    timersub(&endt, &startt, tv);

    // If an output command is specified, pipe the temporary output to it, and
    // check its return code.

    if (output_cmd) {
        fflush(outtmpf);
        fseek(outtmpf, 0, SEEK_SET);
        FILE *cmdf = popen(output_cmd, "w");
        if (cmdf == NULL || !fcopy(outtmpf, cmdf))
            goto output_cmd_err;
        int cmdr = pclose(cmdf);
        if (cmdr != 0)
            errx(1, "Exiting because '%s' failed.", output_cmd);
        fclose(outtmpf);
        free(output_cmd);
    }

    return;

cmd_err:
    err(1, "Error when attempting to run %s", cmd->argv[0]);

output_cmd_err:
    err(1, "Error when attempting to run %s", output_cmd);
}



//
// Read in the input from cmd->input_cmd for runi and return an open file set
// to read from the beginning which contains its output.
//

FILE *read_input(Conf *conf, Cmd *cmd, int runi)
{
    assert(cmd->input_cmd);

    char *input_cmd = replace(conf, cmd, cmd->input_cmd, runi);
    FILE *cmdf = popen(input_cmd, "r");
    FILE *tmpf = tmpfile();
    if (!cmdf || !tmpf)
        goto cmd_err;
    
    fcopy(cmdf, tmpf);
    if (pclose(cmdf) != 0)
        goto cmd_err;
    free(input_cmd);
    fseek(tmpf, 0, SEEK_SET);
    
    return tmpf;

cmd_err:
    errx(1, "Error when attempting to run %s.", cmd->input_cmd);
}



//
// Copy all data from rf to wf. Returns true if successful, false if not.
//

bool fcopy(FILE *rf, FILE *wf)
{
    char *buf = malloc(BUFFER_SIZE);
    while (1) {
        size_t r = fread(buf, 1, BUFFER_SIZE, rf);
        if (r < BUFFER_SIZE && ferror(rf)) {
            free(buf);
            return false;
        }
        size_t w = fwrite(buf, 1, r, wf);
        if (w < r && ferror(wf)) {
            free(buf);
            return false;
        }
        if (feof(rf))
            break;
    }
    free(buf);

    return true;
}



//
// Take in string 's' and replace all instances of cmd->replace_str with
// str(runi + 1). Always returns a malloc'd string (even if cmd->replace_str is
// not in s) which must be manually freed *except* if s is NULL, whereupon NULL
// is returned.
//

char *replace(Conf *conf, Cmd *cmd, const char *s, int runi)
{
    if (s == NULL)
        return NULL;

    char *rtn;
    if (!cmd->replace_str) {
        rtn = malloc(strlen(s) + 1);
        memmove(rtn, s, strlen(s));
        rtn[strlen(s)] = 0;
    }
    else {
        int replacen = 0;
        const char *f = s;
        while (true) {
            f = strstr(f, cmd->replace_str);
            if (f == NULL)
                break;
            replacen++;
            f += strlen(cmd->replace_str);
        }
        int nch = snprintf(NULL, 0, "%d", runi + 1);
        char buf1[nch + 1];
        snprintf(buf1, nch + 1, "%d", runi + 1);
        rtn = malloc(strlen(s) + replacen * (nch - strlen(cmd->replace_str)) + 1);
        f = s;
        char *r = rtn;
        while (true) {
            char *fn = strstr(f, cmd->replace_str);
            if (fn == NULL) {
                memmove(r, f, strlen(f));
                r[strlen(f)] = 0;
                break;
            }
            memmove(r, f, fn - f);
            r += fn - f;
            memmove(r, buf1, strlen(buf1));
            r += strlen(buf1);
            f = fn + strlen(cmd->replace_str);
        }
    }
    
    return rtn;
}



////////////////////////////////////////////////////////////////////////////////
// Start-up routines
//

//
// Parse a batch file and update conf accordingly. This is fairly simplistic
// at the moment, not allowing e.g. breaking of lines with '\'.
//

void parse_batch(Conf *conf, char *path)
{
    FILE *bf = fopen(path, "r");
    if (bf == NULL)
        err(1, "Error when trying to open '%s'", path);
    struct stat sb;
    if (fstat(fileno(bf), &sb) == -1)
        err(1, "Error when trying to fstat '%s'", path);
    size_t bfsz = sb.st_size;
    char *bd = malloc(bfsz);
    if (fread(bd, 1, bfsz, bf) < sb.st_size)
        err(1, "Error when trying to read from '%s'", path);
    fclose(bf);

    int num_cmds = 0;
    Cmd **cmds = malloc(sizeof(Cmd **));
    off_t i = 0;
    int lineno = 0;
    char *msg = NULL; // Set to an error message before doing "goto parse_err".
    while (i < bfsz) {
        // Skip space at beginning of line.
        while (i < bfsz && (bd[i] == ' ' || bd[i] == '\t'))
            i += 1;
        if (i == bfsz)
            break;
        if (bd[i] == '\n' || bd[i] == '\r') {
            if (bd[i] == '\n')
                lineno += 1;
            i += 1;
            continue;
        }
        // Skip comment lines.
        if (bd[i] == '#') {
            if (bd[i] == '\n')
                lineno += 1;
            i += 1;
            while (i < bfsz && (bd[i] != '\n' || bd[i] != '\r'))
                i += 1;
            if (i == bfsz)
                break;
            continue;
        }

        int argc = 0;
        char **argv = malloc(sizeof(char *));
        while (i < bfsz && bd[i] != '\n' && bd[i] != '\r') {
            int j = i;
            // Skip whitespace at the beginning of lines, as well as complete blank lines
            while (j < bfsz && (bd[j] == ' ' || bd[j] == '\t' || bd[j] == '\r'))
                j += 1;
            if (j > i) {
                i = j;
                continue;
            }

            char *arg;
            char qc = 0;
            if (bd[i] == '"' || bd[i] == '\'') {
                qc = bd[i];
                i += 1;
            }
            // Work out the length of the arg. Note that two-character
            // pairs '\x' are length 2 in the input, but only 1 character
            // in the arg.
            j = i;
            size_t argsz = 0;
            while (j < bfsz) {
                if (qc && bd[j] == qc)
                    break;
                else if (bd[j] == '\n' || bd[j] == '\r') {
                    if (qc) {
                        msg = "Unterminated string";
                        goto parse_err;
                    }
                    else
                        break;
                }
                else if (!qc && bd[j] == ' ') {
                    break;
                }
                else if (bd[j] == '\\') {
                    if (j + 1 == bfsz) {
                        msg = "Escape char not specified";
                        goto parse_err;
                    }
                    argsz += 1;
                    j += 2;
                }
                else {
                    argsz += 1;
                    j += 1;
                }
            }
            if (qc && j == bfsz) {
                msg = "Unterminated string";
                goto parse_err;
            }
            arg = malloc(argsz + 1);
            if (arg == NULL)
                errx(1, "Out of memory.");
            // Copy the arg.
            j = 0;
            while (i < bfsz) {
                if (qc && bd[i] == qc) {
                    i += 1;
                    break;
                }
                else if (bd[i] == '\n' || bd[i] == '\r') {
                    assert(!qc);
                    break;
                }
                else if (!qc && bd[i] == ' ')
                    break;
                else if (bd[i] == '\\') {
                    assert(i + 1 < bfsz);
                    arg[j] = escape_char(bd[i + 1]);
                    i += 2;
                    j += 1;
                }
                else {
                    arg[j] = bd[i];
                    i += 1;
                    j += 1;
                }
            }
            arg[j] = 0;

            argc += 1;
            argv = realloc(argv, argc * sizeof(char *));
            if (argv == NULL)
                errx(1, "Out of memory.");
            argv[argc - 1] = arg;
        }

        Cmd *cmd = malloc(sizeof(Cmd));
        cmd->input_cmd = cmd->output_cmd = cmd->replace_str = NULL;
        cmd->quiet = false;
        cmd->rusages = malloc(sizeof(struct rusage *) * conf->num_runs);
        cmd->timevals = malloc(sizeof(struct timeval *) * conf->num_runs);
        memset(cmd->rusages, 0, sizeof(struct rusage *) * conf->num_runs);
        memset(cmd->timevals, 0, sizeof(struct rusage *) * conf->num_runs);
        int j = 0;
        while (j < argc) {
            if (strcmp(argv[j], "-I") == 0) {
                if (j + 1 == argc)
                    errx(1, "option requires an argument -- I");
                cmd->replace_str = argv[j + 1];
                free(argv[j]);
                j += 2;
            }
            else if (strcmp(argv[j], "-i") == 0) {
                if (j + 1 == argc)
                    errx(1, "option requires an argument -- i");
                cmd->input_cmd = argv[j + 1];
                free(argv[j]);
                j += 2;
            }
            else if (strcmp(argv[j], "-o") == 0) {
                if (j + 1 == argc)
                    errx(1, "option requires an argument -- o");
                cmd->output_cmd = argv[j + 1];
                free(argv[j]);
                j += 2;
            }
            else if (strcmp(argv[j], "-q") == 0) {
                cmd->quiet = true;
                j += 1;
            }
            else if (strlen(argv[j]) > 0 && argv[j][0] == '-')
                errx(1, "unknown option -- %c", argv[j][0]);
            else
                break;
        }
        char **new_argv = malloc((argc - j + 1) * sizeof(char *));
        memmove(new_argv, argv + j, (argc - j) * sizeof(char *));
        free(argv);
        argc -= j;
        new_argv[argc] = NULL;
        cmd->argv = new_argv;
        num_cmds += 1;
        cmds = realloc(cmds, num_cmds * sizeof(Cmd *));
        if (cmds == NULL)
            errx(1, "Out of memory.");
        cmds[num_cmds - 1] = cmd;
    }
    
    free(bd);
    conf->cmds = cmds;
    conf->num_cmds = num_cmds;
    
    return;

parse_err:
    errx(1, "%s at line %d.\n", msg, lineno);
}



//
// Given a char c, assuming it was prefixed by '\' (e.g. '\r'), return the
// escaped code.
//

char escape_char(char c)
{
    switch (c) {
        case '0':
            return '\0';
        case 'n':
            return '\n';
        case 'r':
            return '\r';
        case 't':
            return '\r';
        default:
            return c;
    }
}



void usage(int rtn_code, char *msg)
{
    if (msg)
        fprintf(stderr, "%s\n", msg);
    fprintf(stderr, "Usage:\n  %s [-f <liketime|rusage>] [-I <replstr>] "
      "[-i <stdincmd>]\n    [-n <numruns> [-o <stdoutcmd>] [-q] [-s <sleep>] "
      "<command>\n    [<arg 1> ... <arg n>]\n"
      "  %s -b <file> [-f <rusage>] [-q] [-s <sleep>] "
      "[-n <numruns>]\n", __progname, __progname);
    exit(rtn_code);
}



int main(int argc, char** argv)
{
    Conf *conf = malloc(sizeof(Conf));
    conf->num_runs = 1;
    conf->format_style = FORMAT_NORMAL;
    conf->sleep = 3;
    conf->verbosity = 0;
    
    bool quiet = false;
    char *batch_file = NULL;
    char *input_cmd = NULL, *output_cmd = NULL, *replace_str = NULL;
    int ch;
    while ((ch = getopt(argc, argv, "b:f:hi:n:I:o:qs:v")) != -1) {
        switch (ch) {
            case 'b':
                batch_file = optarg;
                break;
            case 'f':
                if (strcmp(optarg, "liketime") == 0)
                    conf->format_style = FORMAT_LIKE_TIME;
                else if (strcmp(optarg, "rusage") == 0)
                    conf->format_style = FORMAT_RUSAGE;
                else
                    usage(1, "Unknown format style.");
                break;
            case 'h':
                usage(0, NULL);
                break;
            case 'I':
                replace_str = optarg;
                break;
            case 'i':
                input_cmd = optarg;
                break;
            case 'n':
                errno = 0;
                char *ep = optarg + strlen(optarg);
                long lval = strtoimax(optarg, &ep, 10);
                if (optarg[0] == 0 || *ep != 0)
                    usage(1, "'num runs' not a valid number.");
                if ((errno == ERANGE && (lval == INTMAX_MIN || lval == INTMAX_MAX))
                  || lval <= 0 || lval >= UINT_MAX)
                    usage(1, "'num runs' out of range.");
                conf->num_runs = (int) lval;
                break;
            case 'o':
                output_cmd = optarg;
                break;
            case 'q':
                quiet = true;
                break;
            case 's': {
                char *ep = optarg + strlen(optarg);
                long lval = strtoimax(optarg, &ep, 10);
                if (optarg[0] == '\0' || *ep != '\0')
                    usage(1, "'sleep' not a valid number.");
                if ((errno == ERANGE && (lval == INTMAX_MIN || lval == INTMAX_MAX))
                  || lval < 0)
                    usage(1, "'sleep' out of range.");
                conf->sleep = (int) lval;
                break;
            }
            case 'v':
                conf->verbosity += 1;
                break;
            default:
                usage(1, NULL);
                break;
        }
    }
    argc -= optind;
    argv += optind;

    if (batch_file && conf->format_style == FORMAT_LIKE_TIME)
        usage(1, "Can't use batch file mode with -f liketime.");
    if (batch_file && (input_cmd || output_cmd || replace_str || quiet))
        usage(1, "In batch file mode, -I/-i/-o/-q must be specified per-command in the batch file.");
    if (quiet && output_cmd)
        usage(1, "-q and -o are mutually exclusive.");
    
    // Process the command(s).

    if (batch_file) {
        // Batch file mode.

        parse_batch(conf, batch_file);
    }
    else {
        // Simple mode: one command specified on the command-line.

        if (argc == 0)
            usage(1, "Missing command.");

        Cmd *cmd;
        if ((conf->cmds = malloc(sizeof(Cmd *))) == NULL
          || (cmd = malloc(sizeof(Cmd))) == NULL)
            errx(1, "Out of memory.");
        conf->num_cmds = 1;
        conf->cmds[0] = cmd;
        cmd->argv = argv;
        cmd->input_cmd = input_cmd;
        cmd->output_cmd = output_cmd;
        cmd->replace_str = replace_str;
        cmd->quiet = quiet;
        cmd->rusages = malloc(sizeof(struct rusage *) * conf->num_runs);
        cmd->timevals = malloc(sizeof(struct timeval *) * conf->num_runs);
        memset(cmd->rusages, 0, sizeof(struct rusage *) * conf->num_runs);
        memset(cmd->timevals, 0, sizeof(struct rusage *) * conf->num_runs);
    }

    // Seed the random number generator.

#	if defined(MT_HAVE_RANDOM) && defined(MT_HAVE_SRANDOMDEV)
	srandomdev();
#	elif defined(MT_HAVE_RANDOM)
	struct timeval tv;

	gettimeofday(&tv, NULL);
	srandom(tv.tv_sec ^ tv.tv_usec);
#	else
	struct timeval tv;

	gettimeofday(&tv, NULL);
	srand(tv.tv_sec ^ tv.tv_usec);
#	endif

    for (int i = 0; i < (conf->num_cmds * conf->num_runs); i += 1) {
        // Find a command which has not yet had all its runs executed.
        Cmd *cmd;
        while (true) {
            cmd = conf->cmds[RANDN(conf->num_cmds)];
            int j;
            for (j = 0; j < conf->num_runs; j += 1) {
                if (cmd->rusages[j] == NULL)
                    break;
            }
            if (j < conf->num_runs)
                break;
        }

        // Find a run of cmd which has not yet been executed.
        int runi;
        while (true) {
            runi = RANDN(conf->num_runs);
            if (cmd->rusages[runi] == NULL)
                break;
        }

        // Execute the command and, if there are more commands yet to be run,
        // sleep.
        execute_cmd(conf, cmd, runi);
        if (i + 1 < conf->num_runs && conf->sleep > 0)
	        usleep(RANDN(conf->sleep * 1000000));
    }
    
    switch (conf->format_style) {
        case FORMAT_LIKE_TIME:
            format_like_time(conf);
            break;
        case FORMAT_NORMAL:
        case FORMAT_RUSAGE:
            format_other(conf);
            break;
    }

    free(conf);
}
