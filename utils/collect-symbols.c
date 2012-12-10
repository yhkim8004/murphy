#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <errno.h>
#define _GNU_SOURCE
#include <getopt.h>
#include <unistd.h>
#include <regex.h>

#include <sys/types.h>
#include <sys/wait.h>

#define RD 0
#define WR 1

typedef enum {
    TOKEN_ERROR = -1,
    TOKEN_BLOCK,                         /* a block enclosed in {}/()/[] */
    TOKEN_WORD,                          /* a word */
    TOKEN_DQUOTED,                       /* a double-quoted sequence */
    TOKEN_SQUOTED,                       /* a single-quoted sequence */
    TOKEN_ASSIGN,                        /* '=' */
    TOKEN_SEMICOLON,                     /* ';' */
    TOKEN_COLON,                         /* ',' */
    TOKEN_OTHER,                         /* any other token */
} token_type_t;


typedef struct {
    token_type_t  type;                  /* token type */
    char         *value;                 /* token value */
} token_t;


#define READBUF_SIZE ( 8 * 1024)
#define RINGBUF_SIZE (16 * 1024)
#define MAX_TOKEN    (512)
#define MAX_TOKENS   (64)

typedef struct {
    int  fd;                             /* file descriptor to read */
    char buf[READBUF_SIZE];              /* data buffer */
    int  len;                            /* amount of data in buffer */
    int  rd;                             /* data buffer read offset */
    int  nxt;                            /* pushed back data if non-zero */
} input_t;

typedef struct {
    char buf[RINGBUF_SIZE];              /* data buffer */
    int  wr;                             /* write offset */
} ringbuf_t;

typedef struct {
    char    *pattern;                    /* symbol pattern */
    char   **files;                      /* files to parse for symbols */
    int      nfile;                      /* number of files */
    char    *cflags;                     /* compiler flags */
    char    *output;                     /* output path */
    int      gnuld;                      /* generate GNU ld script */
    int      verbose;                    /* verbosity */
} config_t;

typedef struct {
    char **syms;
    int    nsym;
} symtab_t;


static int verbosity = 0;


static void fatal_error(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);

    exit(1);
}


static void verbose_message(int level, const char *fmt, ...)
{
    va_list ap;

    if (verbosity >= level) {
        va_start(ap, fmt);
        vfprintf(stderr, fmt, ap);
        va_end(ap);
    }
}


static void print_usage(const char *argv0, int exit_code, const char *fmt, ...)
{
    va_list ap;

    if (fmt && *fmt) {
        va_start(ap, fmt);
        vprintf(fmt, ap);
        va_end(ap);
    }

    printf("usage: %s [options]\n\n"
           "The possible options are:\n"
           "  -c, --compiler-flags <flags> flags to pass to compiler\n"
           "  -p, --pattern <pattern>      symbol regexp pattern\n"
           "  -o, --output <path>          write output to the given file\n"
           "  -g, --gnu-ld <script>        generate GNU ld linker script\n"
           "  -v, --verbose                run in verbose mode\n"
           "  -h, --help                   show this help on usage\n",
           argv0);

    if (exit_code < 0)
        return;
    else
        exit(exit_code);
}


static void set_defaults(config_t *c)
{
    memset(c, 0, sizeof(*c));
}


static void parse_cmdline(config_t *cfg, int argc, char **argv)
{
#   define OPTIONS "c:p:o:gvh"
    struct option options[] = {
        { "compiler-flags", required_argument, NULL, 'c' },
        { "pattern"       , required_argument, NULL, 'p' },
        { "output"        , required_argument, NULL, 'o' },
        { "gnu-ld"        , no_argument      , NULL, 'g' },
        { "verbose"       , no_argument      , NULL, 'v' },
        { "help"          , no_argument      , NULL, 'h' },
        { NULL, 0, NULL, 0 }
    };

    int opt;

    set_defaults(cfg);

    while ((opt = getopt_long(argc, argv, OPTIONS, options, NULL)) != -1) {
        switch (opt) {
        case 'c':
            cfg->cflags = optarg;
            break;

        case 'p':
            cfg->pattern = optarg;
            break;

        case 'o':
            cfg->output = optarg;
            break;

        case 'g':
            cfg->gnuld = 1;
            break;

        case 'v':
            verbosity++;
            break;

        case 'h':
            print_usage(argv[0], -1, "");
            exit(0);
            break;

        default:
            print_usage(argv[0], EINVAL, "invalid option '%c'", opt);
        }
    }

    cfg->files = argv + optind;
    cfg->nfile = argc - optind;
}


static int preprocess_file(const char *file, const char *cflags, pid_t *pid)
{
    char cmd[4096], *argv[32];
    int  fd[2], argc;

    /*
     * preprocess the given file
     *
     * Fork off a process for preprocessing the given file with the
     * configured compiler flags. Return the reading end of the pipe
     * the preprocessor is writing to.
     */

    if (pipe(fd) != 0)
        fatal_error("failed to create pipe (%d: %s).", errno, strerror(errno));

    *pid = fork();

    switch (*pid) {
    case -1:
        fatal_error("failed to for preprocessor (%d: %s).",
                    errno, strerror(errno));
        break;

    case 0: /* child: exec preprocessor */
        close(fd[RD]);

        argc         = 0;
        argv[argc++] = "/bin/sh";
        argv[argc++] = "-c";

        if (cflags != NULL)
            snprintf(cmd, sizeof(cmd), "gcc %s -E %s", cflags, file);
        else
            snprintf(cmd, sizeof(cmd), "gcc -E %s", file);

        argv[argc++] = cmd;
        argv[argc]   = NULL;

        if (dup2(fd[WR], fileno(stdout)) < 0)
            fatal_error("failed to redirect stdout (%d: %s)",
                        errno, strerror(errno));

        if (execv("/bin/sh", argv) != 0)
            fatal_error("failed to exec command '%s' (%d: %s)", cmd,
                        errno, strerror(errno));
        break;

    default: /* parent: return fd to read preprocessed data from */
        close(fd[WR]);
        return fd[RD];
    }

    return -1;  /* never reached */
}


static void input_init(input_t *in, int fd)
{
    memset(in, 0, sizeof(*in));

    in->fd = fd;
}


static char input_read(input_t *in)
{
    char ch;

    /*
     * read the next input character
     *
     * If there is an pushed back character deliver (and clear) than one.
     * Otherwise refill the input buffer if needed and return the next
     * character from it.
     */

    if (in->nxt != 0) {
        ch = in->nxt;
        in->nxt = 0;
    }
    else {
        if (in->len <= in->rd) {
            in->len = read(in->fd, in->buf, sizeof(in->buf));

            if (in->len > 0) {
                in->rd = 1;
                ch = in->buf[0];
            }
            else
                ch = 0;
        }
        else
            return ch = in->buf[in->rd++];
    }

    return ch;
}


static int input_pushback(input_t *in, char ch)
{
    /*
     * push back a character to the input stream
     *
     * Note that you can only push back a single character. Trying to
     * push back more than one will fail with an error.
     */

    if (in->nxt == 0) {
        in->nxt = ch;

        return 0;
    }
    else {
        errno = EBUSY;

        return -1;
    }
}


static void input_discard_whitespace(input_t *in)
{
    char ch;

    /*
     * discard consecutive whitespace (including newline)
     */

    while ((ch = input_read(in)) == ' ' || ch == '\t' || ch == '\n')
        ;

    input_pushback(in, ch);
}


static void input_discard_line(input_t *in)
{
    int ch;

    /*
     * discard input till a newline
     */

    while ((ch = input_read(in)) != '\n' && ch != 0)
        ;
}


static int input_discard_quoted(input_t *in, char quote)
{
    char ch;

    /*
     * discard a block of quoted input
     */

    while ((ch = input_read(in)) != quote && ch != 0) {
        if (ch == '\\')
            input_read(in);
    }

    if (ch != quote) {
        errno = EINVAL;
        return -1;
    }
    else
        return 0;
}


static int input_discard_block(input_t *in, char beg)
{
    char end, ch, quote;
    int  level;

    /*
     * discard a block enclosed in {}, [], or ()
     */

    switch (beg) {
    case '{': end = '}'; break;
    case '[': end = ']'; break;
    case '(': end = ')'; break;
    default:             return 0;
    }

    level = 1;
    while (level > 0) {
        switch ((ch = input_read(in))) {
        case '"':
        case '\'':
            quote = ch;
            if (input_discard_quoted(in, quote) != 0)
                return -1;
            break;

        default:
            if (ch == end)
                level--;
            else if (ch == beg)
                level++;
        }
    }

    if (level == 0)
        return 0;
    else {
        errno = EINVAL;
        return -1;
    }
}


static void ringbuf_init(ringbuf_t *rb)
{
    memset(rb->buf, 0, sizeof(rb->buf));
    rb->wr = 0;
}


static char *ringbuf_save(ringbuf_t *rb, char *token, int len)
{
    char *t, *s, *d;
    int   n, o, i;

    /*
     * save the given token in the token ring buffer
     */

    verbose_message(2, "saving '%s'...\n", token);

    if (len < 0)
        len = strlen(token);

    n = sizeof(rb->buf) - 1 - rb->wr;

    if (n < len + 1) {
        t = rb->buf;
        n = sizeof(rb->buf) - 1;
        o = 0;
    }
    else {
        t = rb->buf + rb->wr;
        o = rb->wr;
    }

    if (n >= len + 1) {
        s = token;
        d = t;

        for (i = 0; i < len; i++, o++)
            *d++ = *s++;

        *d = '\0';
        rb->wr = o + 1;

        return t;
    }
    else {
        errno = ENOSPC;
        return NULL;
    }
}



static char *input_collect_word(input_t *in, ringbuf_t *rb)
{
#define WORD_CHAR(c)                            \
    (('a' <= (c) && (c) <= 'z') ||              \
     ('A' <= (c) && (c) <= 'Z') ||              \
     ('0' <= (c) && (c) <= '9') ||              \
     ((c) == '_' || (c) == '$'))

    char buf[MAX_TOKEN], ch;
    int  n;

    /*
     * collect and save the next word (consecutive sequence) of input
     */

    for (n = 0; n < (int)sizeof(buf) - 1; n++) {
        ch = input_read(in);

        if (WORD_CHAR(ch))
            buf[n] = ch;
        else {
            buf[n] = '\0';
            input_pushback(in, ch);

            return ringbuf_save(rb, buf, n);
        }
    }

    errno = ENOSPC;
    return NULL;
}


static int collect_tokens(input_t *in, ringbuf_t *rb, token_t *tokens,
                          int ntoken)
{
    char ch, *v;
    int  n, has_paren;

    /*
     * collect a sequence of tokens that forms (or looks like) a logical unit
     */

    n = 0;
    has_paren = 0;
    while (n < ntoken) {
        switch ((ch = input_read(in))) {
            /* always treat a semicolon here as a sequence terminator */
        case ';':
            tokens[n].type  = TOKEN_SEMICOLON;
            tokens[n].value = ringbuf_save(rb, ";", 1);
            return n + 1;

            /* ignore preprocessor directives */
        case '#':
            input_discard_line(in);
            break;

            /* discard whitespace (including trailing newlines) */
        case ' ':
        case '\t':
            input_discard_whitespace(in);
            break;

            /* ignore newlines */
        case '\n':
            break;

            /* collate/collapse blocks to a block indicator token */
        case '{':
        case '(':
        case '[':
            if (input_discard_block(in, ch) != 0)
                return -1;
            else {
                /* filter out __attribute__ ((.*)) token pairs */
                if (ch == '(' && n > 0 &&
                    tokens[n-1].type == TOKEN_WORD &&
                    !strcmp(tokens[n-1].value, "__attribute__")) {
                    n--;
                    verbose_message(2, "filtered __attribute__...\n");
                    continue;
                }

                v = (ch == '{' ? "{" : (ch == '[' ? "[" : "("));
                tokens[n].type  = TOKEN_BLOCK;
                tokens[n].value = ringbuf_save(rb, v, 1);
                n++;

                if (v[0] == '(')
                    has_paren = 1;
                else {
                    /*
                     * if this sequence includes both '(...)' and '{...}'
                     * we assume this to be a function definition so we
                     * don't wait for a semicolon but terminate sequence
                     * here
                     */
                    if (v[0] == '{')
                        if (has_paren)
                            return n;
                }
            }
            break;

            /* end of file terminates the current sequence */
        case 0:
            return n;

            /* collect and save the next word */
        case 'a'...'z':
        case 'A'...'Z':
        case '_':
        case '$':
        case '0'...'9':
            input_pushback(in, ch);
            v = input_collect_word(in, rb);

            if (v != NULL) {
                tokens[n].type  = TOKEN_WORD;
                tokens[n].value = v;
                n++;
            }
            else
                return -1;
            break;

        case '=':
            tokens[n].type  = TOKEN_ASSIGN;
            tokens[n].value = ringbuf_save(rb, "=", 1);
            n++;
            break;

            /* ignore asterisks */
        case '*':
            break;

            /* the rest we print for debugging */
        default:
            printf("%c", ch);
        }
    }

    errno = EOVERFLOW;
    return -1;
}


static char *symbol_from_tokens(token_t *tokens, int ntoken)
{
#define MATCHING_TOKEN(_n, _type, _val)                         \
    (tokens[(_n)].type == TOKEN_##_type &&                      \
     (!*_val || !strcmp(_val, tokens[(_n)].value)))

    int last, has_paren, has_curly, has_bracket, has_assign;
    int i;

    /*
     * extract the symbol from a sequence of tokens
     */

    if (verbosity > 2) {
        for (i = 0; i < ntoken; i++)
            verbose_message(3, "0x%x: '%s'\n", tokens[i].type, tokens[i].value);
        verbose_message(3, "--\n");
    }

    has_paren = has_curly = has_bracket = has_assign = 0;
    for (i = 0; i < ntoken; i++) {
        if      (MATCHING_TOKEN(i, BLOCK , "(")) has_paren   = 1;
        else if (MATCHING_TOKEN(i, BLOCK , "{")) has_curly   = 1;
        else if (MATCHING_TOKEN(i, BLOCK , "[")) has_bracket = 1;
        else if (MATCHING_TOKEN(i, ASSIGN, "" )) has_assign  = 1 + i;
    }

    last = ntoken - 1;

    if (tokens[0].type != TOKEN_WORD)
        return NULL;

    /* ignore typedefs and everything static */
    if (MATCHING_TOKEN(0, WORD, "typedef") || MATCHING_TOKEN(0, WORD, "static"))
        return NULL;

    /* take care of function prototypes */
    if (last > 2) {
        if (MATCHING_TOKEN(last  , SEMICOLON, "" ) &&
            MATCHING_TOKEN(last-1, BLOCK    , "(") &&
            MATCHING_TOKEN(last-2, WORD     , "" ))
            return tokens[last-2].value;
    }

    /* take care of global variables with assignments */
    if (last > 1 && has_assign) {
        i = has_assign - 1;
        if (i > 0 && MATCHING_TOKEN(i-1, WORD, ""))
            return tokens[i-1].value;
        if (i > 1 &&
            MATCHING_TOKEN(i-1, BLOCK, "[") &&
            MATCHING_TOKEN(i-2, WORD , ""))
            return tokens[i-2].value;
    }

    /* take care of global variables */
    if (last > 1 && !has_paren && !has_curly) {
        if (MATCHING_TOKEN(last  , SEMICOLON, "") &&
            MATCHING_TOKEN(last-1, WORD     , ""))
            return tokens[last-1].value;
    }

    return NULL;
}


static void symtab_init(symtab_t *st)
{
    st->syms = NULL;
    st->nsym = 0;
}


static void symtab_add(symtab_t *st, char *sym)
{
    int i;

    for (i = 0; i < st->nsym; i++)
        if (!strcmp(st->syms[i], sym))
            return;

    st->syms = realloc(st->syms, (st->nsym + 1) * sizeof(st->syms[0]));

    if (st->syms != NULL) {
        st->syms[st->nsym] = strdup(sym);

        if (st->syms[st->nsym] != NULL) {
            st->nsym++;

            return;
        }

        fatal_error("failed to save symbol '%s'", sym);
    }

    fatal_error("failed to allocate new symbol table entry");
}


static void symtab_reset(symtab_t *st)
{
    int i;

    for (i = 0; i < st->nsym; i++)
        free(st->syms[i]);

    free(st->syms);

    st->syms = NULL;
    st->nsym = 0;
}


static void symtab_dump(symtab_t *st, int gnuld, FILE *out)
{
    int i;

    if (!gnuld) {
        for (i = 0; i < st->nsym; i++)
            fprintf(out, "%s\n", st->syms[i]);
    }
    else {
        fprintf(out, "{\n");
        if (st->nsym > 0) {
            fprintf(out, "    global:\n");
            for (i = 0; i < st->nsym; i++)
                fprintf(out, "        %s;\n", st->syms[i]);
        }
        fprintf(out, "    local:\n");
        fprintf(out, "        *;\n");
        fprintf(out, "};\n");
    }
}


static void extract_symbols(const char *path, const char *cflags,
                            symtab_t *st, regex_t *re)
{
    input_t   in;
    ringbuf_t rb;
    int       fd;
    pid_t     pp_pid;
    token_t   tokens[MAX_TOKENS];
    int       ntoken;
    char     *sym;
    int       pp_status;

    fd = preprocess_file(path, cflags, &pp_pid);

    input_init(&in, fd);
    ringbuf_init(&rb);

    while ((ntoken = collect_tokens(&in, &rb, tokens, MAX_TOKENS)) > 0) {
        sym = symbol_from_tokens(tokens, ntoken);

        if (sym != NULL) {
            if (re == NULL || regexec(re, sym, 0, NULL, 0) == 0)
                symtab_add(st, sym);
            else
                verbose_message(1, "filtered non-matching '%s'...\n", sym);
        }
    }

    close(fd);
    waitpid(pp_pid, &pp_status, 0);
}


int main(int argc, char *argv[])
{
    config_t  cfg;
    symtab_t  st;
    regex_t   rebuf, *re;
    char      regerr[1024];
    FILE     *out;
    int       err, i;

    symtab_init(&st);
    parse_cmdline(&cfg, argc, argv);

    if (cfg.pattern != NULL) {
        err = regcomp(&rebuf, cfg.pattern, REG_EXTENDED);

        if (err != 0) {
            regerror(err, &rebuf, regerr, sizeof(regerr));
            fatal_error("invalid pattern '%s' (error: %s)\n", cfg.pattern,
                        regerr);
        }

        re = &rebuf;
    }
    else
        re = NULL;

    for (i = 0; i < cfg.nfile; i++)
        extract_symbols(cfg.files[i], cfg.cflags, &st, re);

    if (cfg.output != NULL) {
        out = fopen(cfg.output, "w");

        if (out == NULL)
            fatal_error("failed to open '%s' (%d: %s)", cfg.output,
                        errno, strerror(errno));
    }
    else
        out = stdout;

    symtab_dump(&st, cfg.gnuld, out);

    if (re != NULL)
        regfree(re);

    symtab_reset(&st);

    if (out != stdout)
        fclose(out);

    return 0;
}
