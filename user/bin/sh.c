/*
 * ITEM-628: the native HFOC shell language.
 *
 * This is a small Unix-like language over the published native ABI, not a
 * Bash or POSIX implementation.  It supports quoting, $VAR/$?, ;, &&, ||,
 * pipelines, <, >, >>, 2> and 2>&1, PATH lookup, script files and the
 * cd/export/unset/exit builtins.  Commands run as real ELF64 children through
 * SPAWN; nothing is dispatched to a kernel command table.
 */
#include "hfoc.h"

#define SH_LINE_MAX 4096U
#define SH_TOKEN_STORAGE 16384U
#define SH_TOKEN_MAX 1024U
#define SH_ARGS_MAX 64U
#define SH_REDIRS_MAX 8U
#define SH_STAGES_MAX 8U
#define SH_ANDOR_MAX 4U
#define SH_VARS_MAX 12U
#define SH_NAME_MAX 64U
#define SH_VALUE_MAX 256U
#define SH_PATH_MAX 1024U
#define SH_SAVED_STDOUT 20
#define SH_SAVED_STDERR 21
/* ITEM-659: sh_spawn_stage uses a negative PID to report a name it could not
   resolve, so the caller can publish the shell's not-found status 127. */
#define SH_SPAWN_NOT_FOUND (-127)
/* ITEM-632: bounded script/AST limits.  Nothing here grows from input. */
#define SH_SCRIPT_MAX 10240U
#define SH_STRUCT_TOKEN_MAX 1024U
#define SH_NODE_MAX 128U
#define SH_SEQ_CHILD_MAX 512U
#define SH_POS_MAX 10U
#define SH_FUNC_MAX 32U
#define SH_CALL_MAX 6U
#define SH_LOOP_MAX 10000U
#define SH_CAPTURE_MAX 4096U
#define SH_FOR_WORDS_MAX 32U
#define SH_SUBSHELL_MAX 2U
/* ITEM-633: bounded job table. */
#define SH_JOBS_MAX 8U
#define SH_JOB_PIDS_MAX 8U
#define SH_JOB_TEXT_MAX 128U
#define SH_ASSIGN_MAX 8U
#define SH_SEQ_MAX 64U
#define SH_HISTORY_MAX 8U

/* Compact source offsets leave room for history under the user page limit. */
_Static_assert(SH_SCRIPT_MAX <= UINT16_MAX, "shell source offsets must fit");

enum sh_token_type {
    SH_T_EOF = 0,
    SH_T_WORD,
    SH_T_PIPE,
    SH_T_AND,
    SH_T_OR,
    SH_T_SEMI,
    SH_T_IN,
    SH_T_OUT,
    SH_T_APPEND,
    SH_T_ERR,
    SH_T_ERR_TO_OUT,
    SH_T_BAD
};

enum sh_redir_type {
    SH_R_IN = 0,
    SH_R_OUT,
    SH_R_APPEND,
    SH_R_ERR,
    SH_R_ERR_TO_OUT
};

struct sh_token {
    enum sh_token_type type;
    char *text;
};

struct sh_redir {
    enum sh_redir_type type;
    char *path;
};

struct sh_command {
    char *argv[SH_ARGS_MAX + 1U];
    uint32_t argc;
    struct sh_redir redirs[SH_REDIRS_MAX];
    uint32_t redir_count;
};

struct sh_pipeline {
    struct sh_command stages[SH_STAGES_MAX];
    uint32_t count;
};

struct sh_var {
    char name[SH_NAME_MAX];
    char value[SH_VALUE_MAX];
};

struct sh_andor {
    struct sh_pipeline pipes[SH_ANDOR_MAX];
    uint32_t ops[SH_ANDOR_MAX];
    uint32_t count;
};

/*
 * ITEM-632: the structural layer parses a whole script into a bounded static
 * AST before anything runs, so a syntax error never executes half a compound
 * command.  Simple command lists keep their source slice and are handed to the
 * ITEM-628 lexer/executor unchanged; the AST only owns compound structure.
 */
enum sh_stok_type {
    SH_ST_WORD = 0,
    SH_ST_PIPE,
    SH_ST_AND,
    SH_ST_OR,
    SH_ST_SEMI,
    SH_ST_NEWLINE,
    SH_ST_IN,
    SH_ST_OUT,
    SH_ST_APPEND,
    SH_ST_ERR,
    SH_ST_ERR_TO_OUT,
    SH_ST_LPAREN,
    SH_ST_RPAREN,
    SH_ST_LBRACE,
    SH_ST_RBRACE,
    SH_ST_AMP,
    SH_ST_EOF,
    SH_ST_BAD
};

struct sh_stok {
    uint8_t type;
    uint8_t literal; /* no quoting/expansion: safe for reserved words */
    uint16_t start;
    uint16_t end;
};

enum sh_node_kind {
    SH_N_EMPTY = 0,
    SH_N_SIMPLE,
    SH_N_SEQ,
    SH_N_IF,
    SH_N_FOR,
    SH_N_WHILE,
    SH_N_FUNC,
    SH_N_SUBSHELL,
    SH_N_GROUP
};

struct sh_node {
    uint8_t kind;
    uint8_t negate;      /* until */
    uint8_t background;  /* trailing & */
    uint8_t positional;  /* for name; do */
    uint16_t first;      /* sequence child pool start */
    uint16_t count;      /* sequence children; for words token span */
    uint16_t a;          /* if condition / while condition / word start */
    uint16_t b;          /* if then / for words end */
    uint16_t c;          /* if else */
    uint16_t body;
    uint16_t raw_start;
    uint16_t raw_end;
    char name[SH_NAME_MAX];
};

struct sh_function {
    char name[SH_NAME_MAX];
    uint16_t body;
};

struct sh_call_frame {
    char pos[SH_POS_MAX][SH_VALUE_MAX];
    uint32_t count;
};

enum sh_job_state {
    SH_JOB_FREE = 0,
    SH_JOB_RUNNING,
    SH_JOB_STOPPED,
    SH_JOB_DONE
};

struct sh_job {
    uint8_t used;
    uint8_t state;
    uint32_t pgid;
    uint32_t pids[SH_JOB_PIDS_MAX];
    uint32_t pid_count;
    int status;
    uint8_t reported;
    char text[SH_JOB_TEXT_MAX];
};

struct sh_state_frame {
    struct sh_var vars[SH_VARS_MAX];
    uint32_t var_count;
    char cwd[SH_PATH_MAX];
    char pos[SH_POS_MAX][SH_VALUE_MAX];
    uint32_t pos_count;
    int last_status;
};

enum { SH_OP_NONE = 0, SH_OP_AND, SH_OP_OR };

struct sh_editor {
    char *line;
    const char *prompt;
    size_t prompt_length;
    size_t length;
    size_t cursor;
    size_t view_start;
    size_t view_width;
    size_t history_offset;
    size_t draft_length;
    size_t draft_cursor;
};

/* Packed oldest to newest. The byte budget can hold any one accepted line. */
static struct {
    char bytes[SH_LINE_MAX];
    uint16_t lengths[SH_HISTORY_MAX];
    size_t count;
    size_t used;
} sh_history;

static char sh_line[SH_LINE_MAX];
static size_t sh_tty_columns = 80U;
static char sh_tokens[SH_TOKEN_STORAGE];
static size_t sh_token_used;
static size_t sh_line_pos;
static struct sh_token sh_token;
static struct sh_token sh_pending_tokens[SH_POS_MAX];
static uint32_t sh_pending_count;
static uint32_t sh_pending_index;
static int sh_token_ready;
static int sh_last_status;
static const char *sh_program_name = "sh";
static struct sh_var sh_vars[SH_VARS_MAX];
static uint32_t sh_var_count;
static char sh_env_storage[SH_VARS_MAX][SH_NAME_MAX + SH_VALUE_MAX + 2U];
static char *sh_envp[SH_VARS_MAX + 2U];
static struct sh_andor sh_andor;
/* ITEM-632 positional frame: $0 is separate from $1..$9. */
static char sh_pos0[SH_PATH_MAX];
static char sh_pos[SH_POS_MAX][SH_VALUE_MAX];
static uint32_t sh_pos_count;
static struct sh_call_frame sh_call_frames[SH_CALL_MAX];
static uint32_t sh_call_depth;
static struct sh_function sh_functions[SH_FUNC_MAX];
static uint32_t sh_function_count;
static struct sh_state_frame sh_subshell_frames[SH_SUBSHELL_MAX];
static uint32_t sh_subshell_depth;
static char sh_script[SH_SCRIPT_MAX];
static size_t sh_script_len;
static struct sh_stok sh_stoks[SH_STRUCT_TOKEN_MAX];
static uint32_t sh_stok_count;
static uint32_t sh_stok_pos;
static struct sh_node sh_nodes[SH_NODE_MAX];
static uint32_t sh_node_count;
static uint16_t sh_script_root;
static uint16_t sh_seq_children[SH_SEQ_CHILD_MAX];
static uint32_t sh_seq_used;
/* One list buffer per parse depth: a user stack page is only 4096 bytes. */
static uint16_t sh_parse_children[SH_CALL_MAX + 1U][SH_SEQ_MAX];
static int sh_parse_failed;
static int sh_parse_incomplete;
static int sh_parse_message_shown;
static int sh_exit_requested;
static int sh_exit_status;
static int sh_return_requested;
static int sh_return_status;
static int sh_break_requested;
static int sh_continue_requested;
static uint32_t sh_loop_depth;
static int sh_trap_set;
static char sh_trap_exit[2048];
static int sh_trap_running;
static struct sh_job sh_jobs[SH_JOBS_MAX];
static int sh_job_last;
static int sh_job_control;
static const char *sh_current_text = "";

static void sh_error(const char *text)
{
    hfoc_eputs(sh_program_name);
    hfoc_eputs(": ");
    hfoc_eputs(text);
    hfoc_eputs("\n");
}

static int sh_streq(const char *left, const char *right)
{
    return hfoc_strcmp(left, right) == 0;
}

static int sh_is_space(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

static int sh_is_meta(char c)
{
    return sh_is_space(c) || c == '|' || c == '&' || c == ';' ||
           c == '<' || c == '>';
}

static char *sh_var_find(const char *name)
{
    uint32_t index;

    for (index = 0U; index < sh_var_count; ++index) {
        if (sh_streq(sh_vars[index].name, name)) {
            return sh_vars[index].value;
        }
    }
    return NULL;
}

static void sh_var_set(const char *name, const char *value)
{
    uint32_t index;

    if (name == NULL || name[0] == '\0') {
        return;
    }
    if (hfoc_strlen(name) >= SH_NAME_MAX) {
        sh_error("variable name too long");
        return;
    }
    if (hfoc_strlen(value) >= SH_VALUE_MAX) {
        sh_error("variable value too long");
        return;
    }
    for (index = 0U; index < sh_var_count; ++index) {
        if (sh_streq(sh_vars[index].name, name)) {
            hfoc_strncpy(sh_vars[index].value, value, SH_VALUE_MAX - 1U);
            sh_vars[index].value[SH_VALUE_MAX - 1U] = '\0';
            return;
        }
    }
    if (sh_var_count >= SH_VARS_MAX) {
        sh_error("too many variables");
        return;
    }
    hfoc_strncpy(sh_vars[sh_var_count].name, name, SH_NAME_MAX - 1U);
    hfoc_strncpy(sh_vars[sh_var_count].value, value, SH_VALUE_MAX - 1U);
    ++sh_var_count;
}

static void sh_var_unset(const char *name)
{
    uint32_t index;

    for (index = 0U; index < sh_var_count; ++index) {
        if (sh_streq(sh_vars[index].name, name)) {
            uint32_t move;

            for (move = index; move + 1U < sh_var_count; ++move) {
                sh_vars[move] = sh_vars[move + 1U];
            }
            --sh_var_count;
            return;
        }
    }
}

static void sh_env_build(void)
{
    uint32_t index;

    for (index = 0U; index < sh_var_count; ++index) {
        hfoc_snprintf(sh_env_storage[index], sizeof(sh_env_storage[index]),
                      "%s=%s", sh_vars[index].name, sh_vars[index].value);
        sh_envp[index] = sh_env_storage[index];
    }
    sh_envp[sh_var_count] = NULL;
}

static void sh_vars_from_env(void)
{
    char **entry;

    for (entry = hfoc_envp; entry != NULL && *entry != NULL; ++entry) {
        char *equals = hfoc_strchr(*entry, '=');

        if (equals == NULL) {
            continue;
        }
        if (hfoc_strncmp(*entry, "HFOC_TTY_COLUMNS=",
                         sizeof("HFOC_TTY_COLUMNS=") - 1U) == 0) {
            /* Spawn metadata, outside the shell's environment table. */
            size_t columns = 0U;

            for (const char *digit = equals + 1;
                 *digit != '\0' && columns <= SH_LINE_MAX; ++digit) {
                if (*digit < '0' || *digit > '9') {
                    columns = 0U;
                    break;
                }
                columns = columns * 10U + (size_t)(*digit - '0');
            }
            if (columns != 0U && columns <= SH_LINE_MAX) {
                sh_tty_columns = columns;
            }
            continue;
        }
        if ((size_t)(equals - *entry) >= SH_NAME_MAX) {
            continue;
        }
        {
            char name[SH_NAME_MAX];

            hfoc_strncpy(name, *entry, (size_t)(equals - *entry));
            name[equals - *entry] = '\0';
            sh_var_set(name, equals + 1);
        }
    }
}

static int sh_append_char(char *buffer, size_t capacity, size_t *used,
                          char value)
{
    if (*used + 1U >= capacity) {
        return 0;
    }
    buffer[*used] = value;
    ++*used;
    buffer[*used] = '\0';
    return 1;
}

static int sh_append_text(char *buffer, size_t capacity, size_t *used,
                          const char *text)
{
    while (*text != '\0') {
        if (!sh_append_char(buffer, capacity, used, *text++)) {
            return 0;
        }
    }
    return 1;
}

static int sh_append_u64(char *buffer, size_t capacity, size_t *used,
                         uint64_t value)
{
    char digits[24];
    size_t count = 0U;

    if (value == 0U) {
        return sh_append_char(buffer, capacity, used, '0');
    }
    while (value != 0U && count < sizeof(digits)) {
        digits[count++] = (char)('0' + (value % 10U));
        value /= 10U;
    }
    while (count != 0U) {
        if (!sh_append_char(buffer, capacity, used, digits[--count])) {
            return 0;
        }
    }
    return 1;
}

/* Expand $?, $0..$9, $#, $@/$*, $NAME, ${NAME} and $(list). */
static int sh_capture_shell(const char *body, char *out, size_t capacity,
                            size_t *used_out)
{
    int fds[2];
    static struct hfoc_fd_action actions[3];
    char *argv[4];
    long pid;
    size_t used = 0U;
    int overflow = 0;
    int child_status = 0;

    out[0] = '\0';
    *used_out = 0U;
    if (hfoc_pipe(fds) < 0) {
        sh_error("command substitution: cannot create a pipe");
        return 0;
    }
    argv[0] = "sh";
    argv[1] = "-c";
    argv[2] = (char *)body;
    argv[3] = NULL;
    hfoc_memset(actions, 0, sizeof(actions));
    actions[0].destination = 0U;
    actions[0].operation = HFOC_SPAWN_ADOPT;
    actions[0].source = 0U;
    actions[1].destination = 1U;
    actions[1].operation = HFOC_SPAWN_ADOPT;
    actions[1].source = (uint32_t)fds[1];
    actions[2].destination = 2U;
    actions[2].operation = HFOC_SPAWN_ADOPT;
    actions[2].source = 2U;
    sh_env_build();
    pid = hfoc_spawn("/bin/sh", argv, sh_envp, actions, 3U);
    (void)hfoc_close(fds[1]);
    if (pid < 0L) {
        (void)hfoc_close(fds[0]);
        sh_error("command substitution: cannot start /bin/sh");
        return 0;
    }
    for (;;) {
        char chunk[128];
        const long count = hfoc_read(fds[0], chunk, sizeof(chunk));

        if (count < 0L) {
            (void)hfoc_close(fds[0]);
            (void)hfoc_wait(&child_status);
            sh_error("command substitution: read failed");
            return 0;
        }
        if (count == 0L) {
            break;
        }
        if (used + (size_t)count + 1U > capacity) {
            overflow = 1;
            continue;
        }
        hfoc_memcpy(out + used, chunk, (size_t)count);
        used += (size_t)count;
        out[used] = '\0';
    }
    (void)hfoc_close(fds[0]);
    for (;;) {
        int status = 0;
        const long reaped = hfoc_wait(&status);

        if (reaped < 0L) {
            break;
        }
        if (reaped == pid) {
            child_status = status;
            break;
        }
    }
    if (overflow) {
        sh_error("command substitution output exceeds 4096 bytes");
        return 0;
    }
    while (used != 0U && out[used - 1U] == '\n') {
        --used;
    }
    out[used] = '\0';
    *used_out = used;
    /* A nonzero substitution command still yields its captured text; the
       shell records the status for a following $?. */
    sh_last_status = child_status & 0xff;
    return 1;
}

static int sh_expand(char *buffer, size_t capacity, size_t *used,
                     size_t *position)
{
    const char *line = sh_line;
    size_t pos = *position;

    if (line[pos] != '$') {
        return 0;
    }
    ++pos;
    if (line[pos] == '?') {
        *position = pos + 1U;
        return sh_append_u64(buffer, capacity, used,
                             (uint64_t)(uint32_t)sh_last_status);
    }
    if (line[pos] == '(') {
        size_t depth = 1U;
        size_t scan = pos + 1U;
        size_t body_end;
        size_t captured = 0U;

        while (line[scan] != '\0') {
            const char c = line[scan];

            if (c == '\'') {
                ++scan;
                while (line[scan] != '\0' && line[scan] != '\'') {
                    ++scan;
                }
                if (line[scan] == '\0') {
                    return 0;
                }
                ++scan;
                continue;
            }
            if (c == '"') {
                ++scan;
                while (line[scan] != '\0' && line[scan] != '"') {
                    if (line[scan] == '\\' && line[scan + 1U] != '\0') {
                        scan += 2U;
                        continue;
                    }
                    ++scan;
                }
                if (line[scan] == '\0') {
                    return 0;
                }
                ++scan;
                continue;
            }
            if (c == '\\' && line[scan + 1U] != '\0') {
                scan += 2U;
                continue;
            }
            if (c == '(') {
                ++depth;
            } else if (c == ')') {
                --depth;
                if (depth == 0U) {
                    break;
                }
            }
            ++scan;
        }
        if (line[scan] != ')' || depth != 0U) {
            return 0;
        }
        body_end = scan;
        {
            static char capture_body[SH_LINE_MAX];
            static char capture_out[SH_CAPTURE_MAX + 1U];
            const size_t body_len = body_end - (pos + 1U);

            if (body_len >= sizeof(capture_body)) {
                return 0;
            }
            hfoc_memcpy(capture_body, line + pos + 1U, body_len);
            capture_body[body_len] = '\0';
            if (!sh_capture_shell(capture_body, capture_out,
                                  sizeof(capture_out), &captured)) {
                return 0;
            }
            *position = body_end + 1U;
            return sh_append_text(buffer, capacity, used, capture_out);
        }
    }
    if (line[pos] >= '0' && line[pos] <= '9') {
        const uint32_t index = (uint32_t)(line[pos] - '0');

        *position = pos + 1U;
        if (index == 0U) {
            return sh_append_text(buffer, capacity, used, sh_pos0);
        }
        if (index <= sh_pos_count) {
            return sh_append_text(buffer, capacity, used,
                                  sh_pos[index - 1U]);
        }
        return 1;
    }
    if (line[pos] == '#') {
        *position = pos + 1U;
        return sh_append_u64(buffer, capacity, used,
                             (uint64_t)sh_pos_count);
    }
    if (line[pos] == '@' || line[pos] == '*') {
        uint32_t index;

        *position = pos + 1U;
        for (index = 0U; index < sh_pos_count; ++index) {
            if (index != 0U && !sh_append_char(buffer, capacity, used, ' ')) {
                return 0;
            }
            if (!sh_append_text(buffer, capacity, used, sh_pos[index])) {
                return 0;
            }
        }
        return 1;
    }
    if (line[pos] == '{') {
        char name[SH_NAME_MAX];
        size_t length = 0U;
        const char *value;
        uint32_t index;

        ++pos;
        while (line[pos] != '\0' && line[pos] != '}' &&
               length + 1U < sizeof(name)) {
            name[length++] = line[pos++];
        }
        if (line[pos] != '}') {
            return 0;
        }
        name[length] = '\0';
        *position = pos + 1U;
        if (length != 0U) {
            int digits = 1;

            for (index = 0U; index < length; ++index) {
                if (name[index] < '0' || name[index] > '9') {
                    digits = 0;
                    break;
                }
            }
            if (digits && length == 1U) {
                index = (uint32_t)(name[0] - '0');
                if (index == 0U) {
                    return sh_append_text(buffer, capacity, used, sh_pos0);
                }
                if (index <= sh_pos_count) {
                    return sh_append_text(buffer, capacity, used,
                                          sh_pos[index - 1U]);
                }
                return 1;
            }
            if (digits && length == 2U && name[0] == '1' && name[1] == '0') {
                return 1; /* ${10}: named bound is $1..$9 */
            }
        }
        value = sh_var_find(name);
        return sh_append_text(buffer, capacity, used,
                              value != NULL ? value : "");
    }
    if ((line[pos] >= 'A' && line[pos] <= 'Z') ||
        (line[pos] >= 'a' && line[pos] <= 'z') || line[pos] == '_') {
        char name[SH_NAME_MAX];
        size_t length = 0U;
        const char *value;

        while (((line[pos] >= 'A' && line[pos] <= 'Z') ||
                (line[pos] >= 'a' && line[pos] <= 'z') ||
                (line[pos] >= '0' && line[pos] <= '9') ||
                line[pos] == '_') && length + 1U < sizeof(name)) {
            name[length++] = line[pos++];
        }
        name[length] = '\0';
        *position = pos;
        value = sh_var_find(name);
        return sh_append_text(buffer, capacity, used,
                              value != NULL ? value : "");
    }
    *position = pos;
    return sh_append_char(buffer, capacity, used, '$');
}

static void sh_skip_space(void)
{
    while (sh_line[sh_line_pos] != '\0' &&
           sh_is_space(sh_line[sh_line_pos])) {
        ++sh_line_pos;
    }
}

static int sh_word_start(void)
{
    char c = sh_line[sh_line_pos];

    return c != '\0' && !sh_is_meta(c);
}

static enum sh_token_type sh_next_token(void)
{
    char c;

    if (sh_pending_index < sh_pending_count) {
        sh_token = sh_pending_tokens[sh_pending_index++];
        return sh_token.type;
    }
    sh_skip_space();
    sh_token.text = NULL;
    c = sh_line[sh_line_pos];
    if (c == '\0') {
        sh_token.type = SH_T_EOF;
        return SH_T_EOF;
    }
    /*
     * ITEM-632: an exact "$@" (or unquoted $@) yields one word per positional
     * parameter, including empty parameters, and no words when there are none.
     */
    if ((c == '"' && sh_line[sh_line_pos + 1U] == '$' &&
         sh_line[sh_line_pos + 2U] == '@' &&
         sh_line[sh_line_pos + 3U] == '"' &&
         (sh_line[sh_line_pos + 4U] == '\0' ||
          sh_is_meta(sh_line[sh_line_pos + 4U]))) ||
        (c == '$' && sh_line[sh_line_pos + 1U] == '@' &&
         (sh_line[sh_line_pos + 2U] == '\0' ||
          sh_is_meta(sh_line[sh_line_pos + 2U])))) {
        uint32_t index;

        sh_line_pos += c == '"' ? 4U : 2U;
        sh_pending_count = 0U;
        sh_pending_index = 0U;
        for (index = 0U; index < sh_pos_count; ++index) {
            const size_t start = sh_token_used;
            const size_t length = hfoc_strlen(sh_pos[index]);

            if (sh_token_used + length + 1U >= SH_TOKEN_STORAGE) {
                sh_token.type = SH_T_BAD;
                return SH_T_BAD;
            }
            hfoc_memcpy(sh_tokens + sh_token_used, sh_pos[index], length + 1U);
            sh_token_used += length + 1U;
            sh_pending_tokens[sh_pending_count].type = SH_T_WORD;
            sh_pending_tokens[sh_pending_count].text = &sh_tokens[start];
            ++sh_pending_count;
        }
        if (sh_pending_count == 0U) {
            return sh_next_token();
        }
        sh_token = sh_pending_tokens[0];
        sh_pending_index = 1U;
        return sh_token.type;
    }
    if (c == '|' && sh_line[sh_line_pos + 1U] == '|') {
        sh_line_pos += 2U;
        sh_token.type = SH_T_OR;
        return SH_T_OR;
    }
    if (c == '&' && sh_line[sh_line_pos + 1U] == '&') {
        sh_line_pos += 2U;
        sh_token.type = SH_T_AND;
        return SH_T_AND;
    }
    if (c == '|') {
        ++sh_line_pos;
        sh_token.type = SH_T_PIPE;
        return SH_T_PIPE;
    }
    if (c == ';') {
        ++sh_line_pos;
        sh_token.type = SH_T_SEMI;
        return SH_T_SEMI;
    }
    if (c == '<') {
        ++sh_line_pos;
        sh_token.type = SH_T_IN;
        return SH_T_IN;
    }
    if (c == '>' && sh_line[sh_line_pos + 1U] == '>') {
        sh_line_pos += 2U;
        sh_token.type = SH_T_APPEND;
        return SH_T_APPEND;
    }
    if (c == '>') {
        ++sh_line_pos;
        sh_token.type = SH_T_OUT;
        return SH_T_OUT;
    }
    if (c == '2' && sh_line[sh_line_pos + 1U] == '>') {
        if (sh_line[sh_line_pos + 2U] == '&' &&
            sh_line[sh_line_pos + 3U] == '1') {
            sh_line_pos += 4U;
            sh_token.type = SH_T_ERR_TO_OUT;
            return SH_T_ERR_TO_OUT;
        }
        sh_line_pos += 2U;
        sh_token.type = SH_T_ERR;
        return SH_T_ERR;
    }
    if (c == '&') {
        ++sh_line_pos;
        sh_token.type = SH_T_BAD;
        return SH_T_BAD;
    }
    {
        const size_t start = sh_token_used;
        int wrote = 0;

        while (sh_word_start()) {
            c = sh_line[sh_line_pos];
            if (c == '\'') {
                ++sh_line_pos;
                while (sh_line[sh_line_pos] != '\0' &&
                       sh_line[sh_line_pos] != '\'') {
                    if (!sh_append_char(sh_tokens, SH_TOKEN_STORAGE,
                                        &sh_token_used,
                                        sh_line[sh_line_pos++])) {
                        sh_token.type = SH_T_BAD;
                        return SH_T_BAD;
                    }
                    wrote = 1;
                }
                if (sh_line[sh_line_pos] != '\'') {
                    sh_token.type = SH_T_BAD;
                    return SH_T_BAD;
                }
                ++sh_line_pos;
                wrote = 1;
            } else if (c == '"') {
                ++sh_line_pos;
                while (sh_line[sh_line_pos] != '\0' &&
                       sh_line[sh_line_pos] != '"') {
                    if (sh_line[sh_line_pos] == '\\') {
                        const char next = sh_line[sh_line_pos + 1U];

                        if (next == '"' || next == '\\' || next == '$') {
                            sh_line_pos += 2U;
                            if (!sh_append_char(sh_tokens, SH_TOKEN_STORAGE,
                                                &sh_token_used, next)) {
                                sh_token.type = SH_T_BAD;
                                return SH_T_BAD;
                            }
                            wrote = 1;
                            continue;
                        }
                    }
                    if (sh_line[sh_line_pos] == '$') {
                        if (!sh_expand(sh_tokens, SH_TOKEN_STORAGE,
                                       &sh_token_used, &sh_line_pos)) {
                            sh_token.type = SH_T_BAD;
                            return SH_T_BAD;
                        }
                        wrote = 1;
                        continue;
                    }
                    if (!sh_append_char(sh_tokens, SH_TOKEN_STORAGE,
                                        &sh_token_used,
                                        sh_line[sh_line_pos++])) {
                        sh_token.type = SH_T_BAD;
                        return SH_T_BAD;
                    }
                    wrote = 1;
                }
                if (sh_line[sh_line_pos] != '"') {
                    sh_token.type = SH_T_BAD;
                    return SH_T_BAD;
                }
                ++sh_line_pos;
                wrote = 1;
            } else if (c == '\\') {
                ++sh_line_pos;
                if (sh_line[sh_line_pos] == '\0') {
                    sh_token.type = SH_T_BAD;
                    return SH_T_BAD;
                }
                if (!sh_append_char(sh_tokens, SH_TOKEN_STORAGE,
                                    &sh_token_used,
                                    sh_line[sh_line_pos++])) {
                    sh_token.type = SH_T_BAD;
                    return SH_T_BAD;
                }
                wrote = 1;
            } else if (c == '$') {
                if (!sh_expand(sh_tokens, SH_TOKEN_STORAGE, &sh_token_used,
                               &sh_line_pos)) {
                    sh_token.type = SH_T_BAD;
                    return SH_T_BAD;
                }
                wrote = 1;
            } else {
                if (!sh_append_char(sh_tokens, SH_TOKEN_STORAGE,
                                    &sh_token_used,
                                    sh_line[sh_line_pos++])) {
                    sh_token.type = SH_T_BAD;
                    return SH_T_BAD;
                }
                wrote = 1;
            }
        }
        if (sh_token_used >= SH_TOKEN_STORAGE) {
            sh_token.type = SH_T_BAD;
            return SH_T_BAD;
        }
        sh_tokens[sh_token_used++] = '\0';
        if (!wrote) {
            /* An empty quoted word is still an argument. */
            if (sh_token_used < SH_TOKEN_STORAGE) {
                sh_tokens[sh_token_used++] = '\0';
            }
        }
        sh_token.text = &sh_tokens[start];
        sh_token.type = SH_T_WORD;
        return SH_T_WORD;
    }
}

static void sh_advance(void)
{
    (void)sh_next_token();
    sh_token_ready = 1;
}

static int sh_accept(enum sh_token_type type)
{
    if (!sh_token_ready) {
        sh_advance();
    }
    if (sh_token.type != type) {
        return 0;
    }
    sh_token_ready = 0;
    return 1;
}

static int sh_command_add_word(struct sh_command *command, char *word)
{
    if (command->argc >= SH_ARGS_MAX) {
        sh_error("too many arguments");
        return 0;
    }
    command->argv[command->argc++] = word;
    command->argv[command->argc] = NULL;
    return 1;
}

static int sh_add_redir(struct sh_command *command, enum sh_redir_type type,
                        char *path)
{
    if (command->redir_count >= SH_REDIRS_MAX) {
        sh_error("too many redirections");
        return 0;
    }
    command->redirs[command->redir_count].type = type;
    command->redirs[command->redir_count].path = path;
    ++command->redir_count;
    return 1;
}

static int sh_reserved_word(const char *word)
{
    static const char *const reserved[] = {
        "if", "then", "else", "elif", "fi", "for", "while", "until", "do",
        "done", "case", "esac", "function", "select", "time"
    };
    uint32_t index;

    for (index = 0U; index < sizeof(reserved) / sizeof(reserved[0]);
         ++index) {
        if (sh_streq(word, reserved[index])) {
            return 1;
        }
    }
    return 0;
}

static int sh_parse_command(struct sh_command *command)
{
    command->argc = 0U;
    command->redir_count = 0U;
    command->argv[0] = NULL;
    for (;;) {
        if (!sh_token_ready) {
            sh_advance();
        }
        if (sh_token.type == SH_T_WORD) {
            if (command->argc == 0U && sh_reserved_word(sh_token.text)) {
                sh_error("unsupported syntax in this shell");
                return 0;
            }
            if (!sh_command_add_word(command, sh_token.text)) {
                return 0;
            }
            sh_token_ready = 0;
            continue;
        }
        if (sh_token.type == SH_T_IN || sh_token.type == SH_T_OUT ||
            sh_token.type == SH_T_APPEND || sh_token.type == SH_T_ERR) {
            enum sh_redir_type type;

            if (sh_token.type == SH_T_IN) {
                type = SH_R_IN;
            } else if (sh_token.type == SH_T_OUT) {
                type = SH_R_OUT;
            } else if (sh_token.type == SH_T_APPEND) {
                type = SH_R_APPEND;
            } else {
                type = SH_R_ERR;
            }
            sh_token_ready = 0;
            sh_advance();
            if (sh_token.type != SH_T_WORD) {
                sh_error("redirection needs a path");
                return 0;
            }
            if (!sh_add_redir(command, type, sh_token.text)) {
                return 0;
            }
            sh_token_ready = 0;
            continue;
        }
        if (sh_token.type == SH_T_ERR_TO_OUT) {
            sh_token_ready = 0;
            if (!sh_add_redir(command, SH_R_ERR_TO_OUT, NULL)) {
                return 0;
            }
            continue;
        }
        break;
    }
    return 1;
}

static int sh_parse_pipeline(struct sh_pipeline *pipeline)
{
    pipeline->count = 0U;
    for (;;) {
        if (pipeline->count >= SH_STAGES_MAX) {
            sh_error("pipeline has more than 8 stages");
            return 0;
        }
        if (!sh_parse_command(&pipeline->stages[pipeline->count])) {
            return 0;
        }
        if (pipeline->stages[pipeline->count].argc == 0U) {
            sh_error("empty command");
            return 0;
        }
        ++pipeline->count;
        if (!sh_accept(SH_T_PIPE)) {
            break;
        }
    }
    return 1;
}

static int sh_parse_andor(struct sh_andor *andor)
{
    andor->count = 0U;
    andor->ops[0] = SH_OP_NONE;
    for (;;) {
        if (andor->count >= SH_ANDOR_MAX) {
            sh_error("too many command lists");
            return 0;
        }
        if (!sh_parse_pipeline(&andor->pipes[andor->count])) {
            return 0;
        }
        ++andor->count;
        if (!sh_token_ready) {
            sh_advance();
        }
        if (sh_token.type == SH_T_AND || sh_token.type == SH_T_OR) {
            if (andor->count >= SH_ANDOR_MAX) {
                sh_error("too many command lists");
                return 0;
            }
            andor->ops[andor->count] =
                sh_token.type == SH_T_AND ? SH_OP_AND : SH_OP_OR;
            sh_token_ready = 0;
            continue;
        }
        break;
    }
    return 1;
}

/*
 * ITEM-632 structural layer: lex the whole input into word/operator tokens,
 * parse compound commands into a bounded static AST, then execute.  Simple
 * command lists are kept as source slices and run by the ITEM-628 executor.
 */
#define SH_STOP_THEN  0x01U
#define SH_STOP_DO    0x02U
#define SH_STOP_FI    0x04U
#define SH_STOP_ELIF  0x08U
#define SH_STOP_ELSE  0x10U
#define SH_STOP_DONE  0x20U

static int sh_execute_text(const char *text);
static int sh_interactive;

static void sh_syntax_error(const char *message)
{
    if (!sh_parse_failed && !(sh_interactive && sh_parse_incomplete)) {
        sh_error(message);
    }
    sh_parse_failed = 1;
}

static int sh_st_add(enum sh_stok_type type, uint32_t start, uint32_t end,
                     int literal)
{
    struct sh_stok *token;

    if (sh_stok_count >= SH_STRUCT_TOKEN_MAX) {
        sh_syntax_error("script has too many tokens");
        return 0;
    }
    token = &sh_stoks[sh_stok_count++];
    token->type = (uint8_t)type;
    token->literal = (uint8_t)(literal != 0);
    token->start = (uint16_t)start;
    token->end = (uint16_t)end;
    return 1;
}

static int sh_st_brace_standalone(size_t p, size_t length, char brace)
{
    const char next = p + 1U < length ? sh_script[p + 1U] : '\0';

    if (brace == '{') {
        return next == '\0' || sh_is_space(next) || next == ';' ||
               next == '}';
    }
    return next == '\0' || sh_is_space(next) || next == ';' ||
           next == '}';
}

static size_t sh_st_skip_paren(size_t p)
{
    size_t depth = 1U;

    while (p < sh_script_len) {
        const char c = sh_script[p];

        if (c == '\'') {
            ++p;
            while (p < sh_script_len && sh_script[p] != '\'') {
                ++p;
            }
            if (p < sh_script_len) {
                ++p;
            }
            continue;
        }
        if (c == '"') {
            ++p;
            while (p < sh_script_len && sh_script[p] != '"') {
                if (sh_script[p] == '\\' && p + 1U < sh_script_len) {
                    p += 2U;
                    continue;
                }
                ++p;
            }
            if (p < sh_script_len) {
                ++p;
            }
            continue;
        }
        if (c == '\\' && p + 1U < sh_script_len) {
            p += 2U;
            continue;
        }
        if (c == '(') {
            ++depth;
        } else if (c == ')') {
            --depth;
            if (depth == 0U) {
                return p + 1U;
            }
        }
        ++p;
    }
    return p;
}

static int sh_lex_script(void)
{
    size_t p = 0U;

    sh_stok_count = 0U;
    while (p < sh_script_len) {
        const char c = sh_script[p];

        if (c == ' ' || c == '\t' || c == '\r') {
            ++p;
            continue;
        }
        if (c == '\n') {
            if (!sh_st_add(SH_ST_NEWLINE, (uint32_t)p, (uint32_t)p + 1U, 0)) {
                return 0;
            }
            ++p;
            continue;
        }
        if (c == '#') {
            while (p < sh_script_len && sh_script[p] != '\n') {
                ++p;
            }
            continue;
        }
        if (c == '&' && p + 1U < sh_script_len && sh_script[p + 1U] == '&') {
            if (!sh_st_add(SH_ST_AND, (uint32_t)p, (uint32_t)p + 2U, 0)) {
                return 0;
            }
            p += 2U;
            continue;
        }
        if (c == '|' && p + 1U < sh_script_len && sh_script[p + 1U] == '|') {
            if (!sh_st_add(SH_ST_OR, (uint32_t)p, (uint32_t)p + 2U, 0)) {
                return 0;
            }
            p += 2U;
            continue;
        }
        if (c == ';') {
            if (!sh_st_add(SH_ST_SEMI, (uint32_t)p, (uint32_t)p + 1U, 0)) {
                return 0;
            }
            ++p;
            continue;
        }
        if (c == '|') {
            if (!sh_st_add(SH_ST_PIPE, (uint32_t)p, (uint32_t)p + 1U, 0)) {
                return 0;
            }
            ++p;
            continue;
        }
        if (c == '<') {
            if (!sh_st_add(SH_ST_IN, (uint32_t)p, (uint32_t)p + 1U, 0)) {
                return 0;
            }
            ++p;
            continue;
        }
        if (c == '>' && p + 1U < sh_script_len && sh_script[p + 1U] == '>') {
            if (!sh_st_add(SH_ST_APPEND, (uint32_t)p, (uint32_t)p + 2U, 0)) {
                return 0;
            }
            p += 2U;
            continue;
        }
        if (c == '>') {
            if (!sh_st_add(SH_ST_OUT, (uint32_t)p, (uint32_t)p + 1U, 0)) {
                return 0;
            }
            ++p;
            continue;
        }
        if (c == '2' && p + 1U < sh_script_len && sh_script[p + 1U] == '>') {
            if (p + 3U < sh_script_len && sh_script[p + 2U] == '&' &&
                sh_script[p + 3U] == '1') {
                if (!sh_st_add(SH_ST_ERR_TO_OUT, (uint32_t)p,
                               (uint32_t)p + 4U, 0)) {
                    return 0;
                }
                p += 4U;
                continue;
            }
            if (!sh_st_add(SH_ST_ERR, (uint32_t)p, (uint32_t)p + 2U, 0)) {
                return 0;
            }
            p += 2U;
            continue;
        }
        if (c == '&') {
            if (!sh_st_add(SH_ST_AMP, (uint32_t)p, (uint32_t)p + 1U, 0)) {
                return 0;
            }
            ++p;
            continue;
        }
        if (c == '(') {
            if (!sh_st_add(SH_ST_LPAREN, (uint32_t)p, (uint32_t)p + 1U, 0)) {
                return 0;
            }
            ++p;
            continue;
        }
        if (c == ')') {
            if (!sh_st_add(SH_ST_RPAREN, (uint32_t)p, (uint32_t)p + 1U, 0)) {
                return 0;
            }
            ++p;
            continue;
        }
        if ((c == '{' || c == '}') && sh_st_brace_standalone(p,
                                                            sh_script_len, c)) {
            if (!sh_st_add(c == '{' ? SH_ST_LBRACE : SH_ST_RBRACE,
                           (uint32_t)p, (uint32_t)p + 1U, 0)) {
                return 0;
            }
            ++p;
            continue;
        }
        {
            const size_t start = p;
            int literal = 1;

            while (p < sh_script_len) {
                const char wc = sh_script[p];

                if (sh_is_space(wc) || wc == '|' || wc == '&' ||
                    wc == ';' || wc == '<' || wc == '>' || wc == '(' ||
                    wc == ')') {
                    break;
                }
                if (wc == '\'') {
                    literal = 0;
                    ++p;
                    while (p < sh_script_len && sh_script[p] != '\'') {
                        ++p;
                    }
                    if (p >= sh_script_len) {
                        sh_syntax_error("syntax error: unterminated quote");
                        return 0;
                    }
                    ++p;
                    continue;
                }
                if (wc == '"') {
                    literal = 0;
                    ++p;
                    while (p < sh_script_len && sh_script[p] != '"') {
                        if (sh_script[p] == '\\' &&
                            p + 1U < sh_script_len) {
                            p += 2U;
                            continue;
                        }
                        if (sh_script[p] == '$' && p + 1U < sh_script_len &&
                            sh_script[p + 1U] == '(') {
                            p = sh_st_skip_paren(p + 2U);
                            continue;
                        }
                        ++p;
                    }
                    if (p >= sh_script_len) {
                        sh_syntax_error("syntax error: unterminated quote");
                        return 0;
                    }
                    ++p;
                    continue;
                }
                if (wc == '\\') {
                    literal = 0;
                    if (p + 1U >= sh_script_len) {
                        sh_syntax_error("syntax error: trailing backslash");
                        return 0;
                    }
                    p += 2U;
                    continue;
                }
                if (wc == '$') {
                    literal = 0;
                    if (p + 1U < sh_script_len &&
                        sh_script[p + 1U] == '(') {
                        p = sh_st_skip_paren(p + 2U);
                        if (p >= sh_script_len &&
                            (p == 0U || sh_script[p - 1U] != ')')) {
                            sh_syntax_error(
                                "syntax error: unterminated substitution");
                            return 0;
                        }
                        continue;
                    }
                    if (p + 1U < sh_script_len &&
                        sh_script[p + 1U] == '{') {
                        p += 2U;
                        while (p < sh_script_len && sh_script[p] != '}') {
                            ++p;
                        }
                        if (p >= sh_script_len) {
                            sh_syntax_error(
                                "syntax error: unterminated ${...}");
                            return 0;
                        }
                        ++p;
                        continue;
                    }
                    ++p;
                    continue;
                }
                if (wc == '`') {
                    literal = 0;
                    ++p;
                    while (p < sh_script_len && sh_script[p] != '`') {
                        if (sh_script[p] == '\\' &&
                            p + 1U < sh_script_len) {
                            p += 2U;
                            continue;
                        }
                        ++p;
                    }
                    if (p >= sh_script_len) {
                        sh_syntax_error(
                            "syntax error: unterminated command substitution");
                        return 0;
                    }
                    ++p;
                    continue;
                }
                ++p;
            }
            if (p == start) {
                sh_syntax_error("unexpected character in script");
                return 0;
            }
            if (!sh_st_add(SH_ST_WORD, (uint32_t)start, (uint32_t)p,
                           literal)) {
                return 0;
            }
        }
    }
    if (!sh_st_add(SH_ST_EOF, (uint32_t)sh_script_len,
                   (uint32_t)sh_script_len, 0)) {
        return 0;
    }
    sh_stok_pos = 0U;
    return 1;
}

static struct sh_stok *sh_st_peek(void)
{
    return &sh_stoks[sh_stok_pos < sh_stok_count ? sh_stok_pos
                                                 : sh_stok_count - 1U];
}

static int sh_st_word_equals(uint32_t index, const char *word)
{
    const struct sh_stok *token;
    const size_t length = hfoc_strlen(word);

    if (index >= sh_stok_count) {
        return 0;
    }
    token = &sh_stoks[index];
    if (token->type != SH_ST_WORD || !token->literal) {
        return 0;
    }
    return token->end - token->start == length &&
           hfoc_memcmp(sh_script + token->start, word, length) == 0;
}

static int sh_st_word_is(uint32_t index, const char *word)
{
    return sh_st_word_equals(index, word);
}

static int sh_st_current_is(const char *word)
{
    return sh_st_word_is(sh_stok_pos, word);
}

static int sh_st_stop(uint32_t mask)
{
    if (sh_stok_pos >= sh_stok_count) {
        return 0;
    }
    if ((mask & SH_STOP_THEN) && sh_st_current_is("then")) {
        return 1;
    }
    if ((mask & SH_STOP_DO) && sh_st_current_is("do")) {
        return 1;
    }
    if ((mask & SH_STOP_FI) && sh_st_current_is("fi")) {
        return 1;
    }
    if ((mask & SH_STOP_ELIF) && sh_st_current_is("elif")) {
        return 1;
    }
    if ((mask & SH_STOP_ELSE) && sh_st_current_is("else")) {
        return 1;
    }
    if ((mask & SH_STOP_DONE) && sh_st_current_is("done")) {
        return 1;
    }
    return 0;
}

static void sh_skip_separators(void)
{
    while (sh_stok_pos < sh_stok_count) {
        const uint8_t type = sh_st_peek()->type;

        if (type != SH_ST_SEMI && type != SH_ST_NEWLINE) {
            break;
        }
        ++sh_stok_pos;
    }
}

static uint16_t sh_node_new(enum sh_node_kind kind)
{
    struct sh_node *node;

    if (sh_node_count >= SH_NODE_MAX) {
        sh_syntax_error("script has too many statements");
        return 0xFFFFU;
    }
    node = &sh_nodes[sh_node_count];
    hfoc_memset(node, 0, sizeof(*node));
    node->kind = (uint8_t)kind;
    node->a = 0xFFFFU;
    node->b = 0xFFFFU;
    node->c = 0xFFFFU;
    node->body = 0xFFFFU;
    return (uint16_t)sh_node_count++;
}

static uint16_t sh_seq_publish(uint16_t *children, uint32_t count)
{
    uint16_t node_index = sh_node_new(SH_N_SEQ);
    struct sh_node *node;
    uint32_t index;

    if (node_index == 0xFFFFU) {
        return 0xFFFFU;
    }
    if (count != 0U && sh_seq_used + count > SH_SEQ_CHILD_MAX) {
        sh_syntax_error("script list is too large");
        return 0xFFFFU;
    }
    node = &sh_nodes[node_index];
    node->first = (uint16_t)sh_seq_used;
    node->count = (uint16_t)count;
    for (index = 0U; index < count; ++index) {
        sh_seq_children[sh_seq_used++] = children[index];
    }
    return node_index;
}

static uint16_t sh_parse_list(uint32_t stop_mask, uint32_t depth,
                              uint16_t *out);
static uint16_t sh_parse_statement(uint32_t depth);

static int sh_parse_expect_word(const char *word)
{
    sh_skip_separators();
    if (sh_st_peek()->type == SH_ST_EOF) {
        sh_parse_incomplete = 1;
        sh_syntax_error("syntax error: unexpected end of input");
        return 0;
    }
    if (!sh_st_current_is(word)) {
        sh_syntax_error("syntax error: expected reserved word");
        return 0;
    }
    ++sh_stok_pos;
    return 1;
}

static uint16_t sh_parse_if_chain(uint32_t depth, int consume_elif)
{
    uint16_t node_index = sh_node_new(SH_N_IF);
    struct sh_node *node;
    uint16_t cond;
    uint16_t then_body;
    uint16_t else_body = 0xFFFFU;

    if (sh_parse_failed || node_index == 0xFFFFU) {
        return 0xFFFFU;
    }
    (void)consume_elif;
    ++sh_stok_pos; /* consume "if" or "elif" */
    cond = sh_parse_list(SH_STOP_THEN, depth + 1U, NULL);
    if (sh_parse_failed || !sh_parse_expect_word("then")) {
        return 0xFFFFU;
    }
    then_body = sh_parse_list(SH_STOP_ELIF | SH_STOP_ELSE | SH_STOP_FI,
                              depth + 1U, NULL);
    if (sh_parse_failed) {
        return 0xFFFFU;
    }
    sh_skip_separators();
    if (sh_st_current_is("elif")) {
        else_body = sh_parse_if_chain(depth, 1);
    } else if (sh_st_current_is("else")) {
        ++sh_stok_pos;
        else_body = sh_parse_list(SH_STOP_FI, depth + 1U, NULL);
        if (!sh_parse_failed) {
            (void)sh_parse_expect_word("fi");
        }
    } else {
        (void)sh_parse_expect_word("fi");
    }
    if (sh_parse_failed) {
        return 0xFFFFU;
    }
    node = &sh_nodes[node_index];
    node->a = cond;
    node->b = then_body;
    node->c = else_body;
    return node_index;
}

static uint16_t sh_parse_for(uint32_t depth)
{
    uint16_t node_index = sh_node_new(SH_N_FOR);
    struct sh_node *node;
    const struct sh_stok *name;
    size_t length;

    if (sh_parse_failed || node_index == 0xFFFFU) {
        return 0xFFFFU;
    }
    ++sh_stok_pos;
    if (sh_st_peek()->type != SH_ST_WORD) {
        sh_syntax_error("for: expected a variable name");
        return 0xFFFFU;
    }
    name = sh_st_peek();
    length = name->end - name->start;
    if (length == 0U || length >= SH_NAME_MAX) {
        sh_syntax_error("for: variable name is too long");
        return 0xFFFFU;
    }
    node = &sh_nodes[node_index];
    hfoc_memcpy(node->name, sh_script + name->start, length);
    node->name[length] = '\0';
    ++sh_stok_pos;
    node->first = (uint16_t)sh_stok_pos;
    node->count = 0U;
    if (sh_st_current_is("in")) {
        ++sh_stok_pos;
        node->a = (uint16_t)sh_stok_pos;
        while (sh_st_peek()->type == SH_ST_WORD) {
            ++sh_stok_pos;
        }
        node->b = (uint16_t)sh_stok_pos;
        node->positional = 0U;
    } else {
        node->positional = 1U;
    }
    if (!sh_parse_expect_word("do")) {
        return 0xFFFFU;
    }
    node->body = sh_parse_list(SH_STOP_DONE, depth + 1U, NULL);
    if (!sh_parse_failed) {
        (void)sh_parse_expect_word("done");
    }
    if (sh_parse_failed) {
        return 0xFFFFU;
    }
    return node_index;
}

static uint16_t sh_parse_while(uint32_t depth, int until)
{
    uint16_t node_index = sh_node_new(SH_N_WHILE);
    struct sh_node *node;
    uint16_t cond;
    uint16_t body;

    if (sh_parse_failed || node_index == 0xFFFFU) {
        return 0xFFFFU;
    }
    ++sh_stok_pos;
    cond = sh_parse_list(SH_STOP_DO, depth + 1U, NULL);
    if (sh_parse_failed || !sh_parse_expect_word("do")) {
        return 0xFFFFU;
    }
    body = sh_parse_list(SH_STOP_DONE, depth + 1U, NULL);
    if (!sh_parse_failed) {
        (void)sh_parse_expect_word("done");
    }
    if (sh_parse_failed) {
        return 0xFFFFU;
    }
    node = &sh_nodes[node_index];
    node->a = cond;
    node->body = body;
    node->negate = (uint8_t)(until != 0);
    return node_index;
}

static uint16_t sh_parse_function(uint32_t depth, int keyword_form)
{
    uint16_t node_index = sh_node_new(SH_N_FUNC);
    struct sh_node *node;
    const struct sh_stok *name;
    size_t length;
    uint16_t body;

    if (sh_parse_failed || node_index == 0xFFFFU) {
        return 0xFFFFU;
    }
    if (keyword_form) {
        ++sh_stok_pos;
    }
    if (sh_st_peek()->type != SH_ST_WORD) {
        sh_syntax_error("function: expected a name");
        return 0xFFFFU;
    }
    name = sh_st_peek();
    length = name->end - name->start;
    if (length == 0U || length >= SH_NAME_MAX) {
        sh_syntax_error("function: name is too long");
        return 0xFFFFU;
    }
    ++sh_stok_pos;
    if (sh_st_peek()->type == SH_ST_LPAREN) {
        ++sh_stok_pos;
        if (sh_st_peek()->type != SH_ST_RPAREN) {
            sh_syntax_error("function: expected ')'");
            return 0xFFFFU;
        }
        ++sh_stok_pos;
    } else if (!keyword_form) {
        sh_syntax_error("function: expected '()'");
        return 0xFFFFU;
    }
    sh_skip_separators();
    if (sh_st_peek()->type != SH_ST_LBRACE) {
        sh_syntax_error("function: expected '{'");
        return 0xFFFFU;
    }
    ++sh_stok_pos;
    body = sh_parse_list(0U, depth + 1U, NULL);
    sh_skip_separators();
    if (sh_st_peek()->type == SH_ST_EOF) {
        sh_parse_incomplete = 1;
        sh_syntax_error("syntax error: unexpected end of input");
        return 0xFFFFU;
    }
    if (sh_st_peek()->type != SH_ST_RBRACE) {
        sh_syntax_error("function: expected '}'");
        return 0xFFFFU;
    }
    ++sh_stok_pos;
    if (sh_parse_failed) {
        return 0xFFFFU;
    }
    node = &sh_nodes[node_index];
    hfoc_memcpy(node->name, sh_script + name->start, length);
    node->name[length] = '\0';
    node->body = body;
    return node_index;
}

static uint16_t sh_parse_group(uint32_t depth, int subshell)
{
    uint16_t node_index = sh_node_new(subshell ? SH_N_SUBSHELL : SH_N_GROUP);
    uint16_t body;

    if (sh_parse_failed || node_index == 0xFFFFU) {
        return 0xFFFFU;
    }
    ++sh_stok_pos;
    body = sh_parse_list(0U, depth + 1U, NULL);
    sh_skip_separators();
    if (sh_st_peek()->type == SH_ST_EOF) {
        sh_parse_incomplete = 1;
        sh_syntax_error("syntax error: unexpected end of input");
        return 0xFFFFU;
    }
    if (subshell) {
        if (sh_st_peek()->type != SH_ST_RPAREN) {
            sh_syntax_error("subshell: expected ')'");
            return 0xFFFFU;
        }
    } else if (sh_st_peek()->type != SH_ST_RBRACE) {
        sh_syntax_error("group: expected '}'");
        return 0xFFFFU;
    }
    ++sh_stok_pos;
    if (sh_parse_failed) {
        return 0xFFFFU;
    }
    sh_nodes[node_index].body = body;
    return node_index;
}

static uint16_t sh_parse_simple(void)
{
    uint16_t node_index;
    const uint32_t start_token = sh_stok_pos;
    uint16_t last_end = sh_st_peek()->end;

    if (sh_parse_failed) {
        return 0xFFFFU;
    }
    while (sh_stok_pos < sh_stok_count) {
        const uint8_t type = sh_st_peek()->type;

        if (type == SH_ST_SEMI || type == SH_ST_NEWLINE ||
            type == SH_ST_EOF || type == SH_ST_RPAREN ||
            type == SH_ST_RBRACE || type == SH_ST_AMP) {
            break;
        }
        if (type == SH_ST_BAD) {
            sh_syntax_error("unexpected character");
            return 0xFFFFU;
        }
        last_end = sh_st_peek()->end;
        ++sh_stok_pos;
    }
    if (sh_stok_pos == start_token) {
        sh_syntax_error("syntax error: empty command");
        return 0xFFFFU;
    }
    node_index = sh_node_new(SH_N_SIMPLE);
    if (node_index == 0xFFFFU) {
        return 0xFFFFU;
    }
    sh_nodes[node_index].raw_start = sh_stoks[start_token].start;
    sh_nodes[node_index].raw_end = last_end;
    if (sh_st_peek()->type == SH_ST_AMP) {
        ++sh_stok_pos;
        sh_nodes[node_index].background = 1U;
    }
    return node_index;
}

static uint16_t sh_parse_statement(uint32_t depth)
{
    const struct sh_stok *token = sh_st_peek();

    if (token->type == SH_ST_LPAREN) {
        return sh_parse_group(depth, 1);
    }
    if (token->type == SH_ST_LBRACE) {
        return sh_parse_group(depth, 0);
    }
    if (token->type == SH_ST_WORD && token->literal) {
        if (sh_st_current_is("if")) {
            return sh_parse_if_chain(depth, 0);
        }
        if (sh_st_current_is("for")) {
            return sh_parse_for(depth);
        }
        if (sh_st_current_is("while")) {
            return sh_parse_while(depth, 0);
        }
        if (sh_st_current_is("until")) {
            return sh_parse_while(depth, 1);
        }
        if (sh_st_current_is("function")) {
            return sh_parse_function(depth, 1);
        }
        if (sh_stok_pos + 2U < sh_stok_count &&
            sh_stoks[sh_stok_pos + 1U].type == SH_ST_LPAREN &&
            sh_stoks[sh_stok_pos + 2U].type == SH_ST_RPAREN) {
            return sh_parse_function(depth, 0);
        }
        if (sh_st_current_is("case") || sh_st_current_is("select") ||
            sh_st_current_is("time")) {
            sh_syntax_error("unsupported compound command");
            return 0xFFFFU;
        }
        if (sh_st_current_is("then") || sh_st_current_is("else") ||
            sh_st_current_is("elif") || sh_st_current_is("fi") ||
            sh_st_current_is("do") || sh_st_current_is("done") ||
            sh_st_current_is("esac")) {
            sh_syntax_error("syntax error: unexpected reserved word");
            return 0xFFFFU;
        }
    }
    return sh_parse_simple();
}

static uint16_t sh_parse_list(uint32_t stop_mask, uint32_t depth,
                              uint16_t *out)
{
    uint16_t *children;
    uint32_t count = 0U;
    uint16_t result;

    if (depth > SH_CALL_MAX) {
        sh_syntax_error("maximum nesting depth exceeded");
        return 0xFFFFU;
    }
    children = sh_parse_children[depth];
    for (;;) {
        uint16_t statement;
        uint8_t type;

        sh_skip_separators();
        if (sh_parse_failed || sh_stok_pos >= sh_stok_count) {
            break;
        }
        type = sh_st_peek()->type;
        if (type == SH_ST_EOF || type == SH_ST_RPAREN ||
            type == SH_ST_RBRACE) {
            break;
        }
        if (sh_st_stop(stop_mask)) {
            break;
        }
        if (count >= SH_SEQ_MAX) {
            sh_syntax_error("list has too many statements");
            break;
        }
        statement = sh_parse_statement(depth);
        if (statement == 0xFFFFU) {
            break;
        }
        children[count++] = statement;
        sh_skip_separators();
    }
    if (sh_parse_failed) {
        return 0xFFFFU;
    }
    result = sh_seq_publish(children, count);
    if (out != NULL) {
        *out = result;
    }
    return result;
}

static int sh_identifier(const char *name)
{
    const char *cursor = name;

    if (cursor == NULL || !((*cursor >= 'A' && *cursor <= 'Z') ||
                            (*cursor >= 'a' && *cursor <= 'z') ||
                            *cursor == '_')) {
        return 0;
    }
    ++cursor;
    while (*cursor != '\0') {
        if (!((*cursor >= 'A' && *cursor <= 'Z') ||
              (*cursor >= 'a' && *cursor <= 'z') ||
              (*cursor >= '0' && *cursor <= '9') || *cursor == '_')) {
            return 0;
        }
        ++cursor;
    }
    return 1;
}

static int sh_parse_script(void)
{
    sh_parse_failed = 0;
    sh_parse_incomplete = 0;
    sh_parse_message_shown = 0;
    sh_node_count = 0U;
    sh_seq_used = 0U;
    if (!sh_lex_script()) {
        return 0;
    }
    {
        uint16_t root = sh_parse_list(0U, 0U, NULL);

        if (root == 0xFFFFU) {
            if (!sh_parse_failed && !sh_parse_incomplete) {
                sh_syntax_error("syntax error");
            }
            return 0;
        }
        /* Keep the root outside the arena so a later definition cannot
           overwrite it. */
        sh_script_root = root;
    }
    if (!sh_parse_failed && sh_stok_pos < sh_stok_count &&
        sh_st_peek()->type != SH_ST_EOF) {
        sh_syntax_error("syntax error: unexpected closing token");
        return 0;
    }
    return !sh_parse_failed;
}

static int sh_execute_slice(uint32_t start, uint32_t end)
{
    static char text[SH_LINE_MAX];
    const size_t length = (size_t)(end - start);

    if (length + 1U >= sizeof(text)) {
        sh_error("command line is too long");
        return 2;
    }
    hfoc_memcpy(text, sh_script + start, length);
    text[length] = '\0';
    return sh_execute_text(text);
}

static void sh_positional_save(struct sh_state_frame *frame)
{
    uint32_t index;

    frame->pos_count = sh_pos_count;
    for (index = 0U; index < SH_POS_MAX; ++index) {
        hfoc_strncpy(frame->pos[index], sh_pos[index], SH_VALUE_MAX - 1U);
        frame->pos[index][SH_VALUE_MAX - 1U] = '\0';
    }
}

static void sh_positional_restore(const struct sh_state_frame *frame)
{
    uint32_t index;

    sh_pos_count = frame->pos_count;
    for (index = 0U; index < SH_POS_MAX; ++index) {
        hfoc_strncpy(sh_pos[index], frame->pos[index], SH_VALUE_MAX - 1U);
        sh_pos[index][SH_VALUE_MAX - 1U] = '\0';
    }
}

static void sh_vars_restore(const struct sh_state_frame *frame)
{
    uint32_t index;

    for (index = 0U; index < frame->var_count; ++index) {
        sh_vars[index] = frame->vars[index];
    }
    sh_var_count = frame->var_count;
}

static int sh_register_function(const char *name, uint16_t body)
{
    uint32_t index;

    for (index = 0U; index < sh_function_count; ++index) {
        if (sh_streq(sh_functions[index].name, name)) {
            sh_functions[index].body = body;
            return 1;
        }
    }
    if (sh_function_count >= SH_FUNC_MAX) {
        sh_error("too many functions");
        return 0;
    }
    hfoc_strncpy(sh_functions[sh_function_count].name, name,
                 SH_NAME_MAX - 1U);
    sh_functions[sh_function_count].name[SH_NAME_MAX - 1U] = '\0';
    sh_functions[sh_function_count].body = body;
    ++sh_function_count;
    return 1;
}

/* ITEM-633: bounded job table and wait decoding. */
#define SH_WIFEXITED(s) (((s) & 0x7fU) == 0U)
#define SH_WEXITSTATUS(s) (((s) >> 8U) & 0xffU)
#define SH_WIFSIGNALED(s) (((s) & 0x7fU) != 0U && ((s) & 0x7fU) != 0x7fU)
#define SH_WTERMSIG(s) ((s) & 0x7fU)
#define SH_WIFSTOPPED(s) (((s) & 0xffU) == 0x7fU)
#define SH_WSTOPSIG(s) (((s) >> 8U) & 0xffU)
#define SH_WIFCONTINUED(s) ((s) == 0xffffU)

static const char *sh_job_state_name(const struct sh_job *job)
{
    if (job->state == SH_JOB_RUNNING) {
        return "Running";
    }
    if (job->state == SH_JOB_STOPPED) {
        return "Stopped";
    }
    return "Done";
}

static void sh_job_clear(struct sh_job *job)
{
    hfoc_memset(job, 0, sizeof(*job));
}

static int sh_job_index(const struct sh_job *job)
{
    return (int)(job - sh_jobs) + 1;
}

static struct sh_job *sh_job_alloc(const char *text)
{
    uint32_t index;
    struct sh_job *fallback = NULL;

    for (index = 0U; index < SH_JOBS_MAX; ++index) {
        if (!sh_jobs[index].used) {
            fallback = &sh_jobs[index];
            break;
        }
    }
    if (fallback == NULL) {
        for (index = 0U; index < SH_JOBS_MAX; ++index) {
            if (sh_jobs[index].state == SH_JOB_DONE) {
                fallback = &sh_jobs[index];
                break;
            }
        }
    }
    if (fallback == NULL) {
        return NULL;
    }
    sh_job_clear(fallback);
    fallback->used = 1U;
    fallback->state = SH_JOB_RUNNING;
    hfoc_strncpy(fallback->text, text != NULL ? text : "",
                 SH_JOB_TEXT_MAX - 1U);
    fallback->text[SH_JOB_TEXT_MAX - 1U] = '\0';
    sh_job_last = sh_job_index(fallback);
    return fallback;
}

static void sh_job_add_pid(struct sh_job *job, uint32_t pid)
{
    if (job->pid_count < SH_JOB_PIDS_MAX) {
        job->pids[job->pid_count++] = pid;
    }
}

static void sh_job_remove_pid(struct sh_job *job, uint32_t pid)
{
    uint32_t index;

    for (index = 0U; index < job->pid_count; ++index) {
        if (job->pids[index] == pid) {
            uint32_t move;

            for (move = index; move + 1U < job->pid_count; ++move) {
                job->pids[move] = job->pids[move + 1U];
            }
            --job->pid_count;
            return;
        }
    }
}

static struct sh_job *sh_job_by_pid(uint32_t pid)
{
    uint32_t index;
    uint32_t slot;

    for (index = 0U; index < SH_JOBS_MAX; ++index) {
        if (!sh_jobs[index].used) {
            continue;
        }
        for (slot = 0U; slot < sh_jobs[index].pid_count; ++slot) {
            if (sh_jobs[index].pids[slot] == pid) {
                return &sh_jobs[index];
            }
        }
    }
    return NULL;
}

static void sh_job_report(struct sh_job *job, int force)
{
    if (!force &&
        (job->state != SH_JOB_DONE || job->reported)) {
        return;
    }
    if (job->state == SH_JOB_DONE) {
        job->reported = 1U;
    }
    hfoc_printf("[%d] %s %s\n", sh_job_index(job),
                sh_job_state_name(job), job->text);
}

/* Reap every finished or stopped child without blocking. */
static void sh_job_reap_nohang(void)
{
    for (;;) {
        uint32_t result[2] = { 0U, 0U };
        const long status = hfoc_wait2(HFOC_WAIT_NOHANG | HFOC_WAIT_UNTRACED |
                                           HFOC_WAIT_CONTINUED,
                                       result);
        struct sh_job *job;

        if (status != 0L) {
            break;
        }
        job = sh_job_by_pid(result[0]);
        if (job == NULL) {
            continue;
        }
        if (SH_WIFSTOPPED(result[1])) {
            job->state = SH_JOB_STOPPED;
            job->status = 128 + (int)SH_WSTOPSIG(result[1]);
        } else if (SH_WIFCONTINUED(result[1])) {
            job->state = SH_JOB_RUNNING;
        } else {
            sh_job_remove_pid(job, result[0]);
            if (SH_WIFEXITED(result[1])) {
                job->status = (int)SH_WEXITSTATUS(result[1]);
            } else {
                job->status = 128 + (int)SH_WTERMSIG(result[1]);
            }
        }
        if (job->pid_count == 0U) {
            job->state = SH_JOB_DONE;
            sh_job_report(job, 0);
        }
    }
}

static int sh_job_find_index(const char *argument)
{
    int number;

    if (argument == NULL || argument[0] == '\0') {
        return -1;
    }
    if (argument[0] == '%') {
        ++argument;
    }
    {
        int ok = 0;
        const long parsed = hfoc_parse_int(argument, &ok);

        if (!ok || parsed <= 0 || parsed > (long)SH_JOBS_MAX) {
            return -1;
        }
        number = (int)parsed;
    }
    return sh_jobs[number - 1].used ? number : -1;
}

static int sh_job_pick(int wanted)
{
    uint32_t index;

    if (wanted > 0) {
        return sh_jobs[wanted - 1].used ? wanted : -1;
    }
    for (index = SH_JOBS_MAX; index != 0U; --index) {
        if (sh_jobs[index - 1U].used &&
            (sh_jobs[index - 1U].state == SH_JOB_STOPPED ||
             sh_jobs[index - 1U].state == SH_JOB_RUNNING)) {
            return (int)index;
        }
    }
    return -1;
}

/* Wait for one job, updating every child it reaps.  1 means stopped. */
static int sh_job_wait(struct sh_job *job, int *status_out)
{
    while (job->pid_count != 0U) {
        uint32_t result[2] = { 0U, 0U };
        const long wait_status = hfoc_wait2(
            HFOC_WAIT_UNTRACED | HFOC_WAIT_CONTINUED, result);
        struct sh_job *owner;

        if (wait_status != 0L) {
            break;
        }
        owner = sh_job_by_pid(result[0]);
        if (owner == NULL) {
            continue;
        }
        if (SH_WIFSTOPPED(result[1])) {
            owner->state = SH_JOB_STOPPED;
            owner->status = 128 + (int)SH_WSTOPSIG(result[1]);
            if (owner == job) {
                *status_out = owner->status;
                return 1;
            }
            continue;
        }
        if (SH_WIFCONTINUED(result[1])) {
            owner->state = SH_JOB_RUNNING;
            continue;
        }
        sh_job_remove_pid(owner, result[0]);
        owner->status = SH_WIFEXITED(result[1])
                            ? (int)SH_WEXITSTATUS(result[1])
                            : 128 + (int)SH_WTERMSIG(result[1]);
        if (owner->pid_count == 0U) {
            owner->state = SH_JOB_DONE;
        }
    }
    *status_out = job->status;
    return 0;
}

static void sh_jobs_cleanup(void)
{
    uint32_t index;
    uint32_t spins;

    for (index = 0U; index < SH_JOBS_MAX; ++index) {
        if (sh_jobs[index].used && sh_jobs[index].pgid != 0U &&
            sh_jobs[index].state != SH_JOB_DONE) {
            (void)hfoc_kill(sh_jobs[index].pgid, HFOC_SIG_HUP, 1);
            (void)hfoc_kill(sh_jobs[index].pgid, HFOC_SIG_CONT, 1);
        }
    }
    for (spins = 0U; spins < 1000U; ++spins) {
        uint32_t alive = 0U;

        sh_job_reap_nohang();
        for (index = 0U; index < SH_JOBS_MAX; ++index) {
            if (sh_jobs[index].used &&
                sh_jobs[index].state != SH_JOB_DONE) {
                ++alive;
            }
        }
        if (alive == 0U) {
            break;
        }
        (void)hfoc_yield();
    }
}

static int sh_spawn_background(const char *text)
{
    /* Five 1 KiB action records would not fit the 16 KiB user stack. */
    static struct hfoc_fd_action actions[5];
    char *argv[4];
    struct sh_job *job;
    long pid;

    if (text == NULL || text[0] == '\0') {
        sh_error("empty background command");
        return 2;
    }
    job = sh_job_alloc(text);
    if (job == NULL) {
        sh_error("too many jobs");
        return 1;
    }
    argv[0] = "sh";
    argv[1] = "-c";
    argv[2] = (char *)text;
    argv[3] = NULL;
    hfoc_memset(actions, 0, sizeof(actions));
    actions[0].destination = 0U;
    actions[0].operation = HFOC_SPAWN_ADOPT;
    actions[0].source = 0U;
    actions[1].destination = 1U;
    actions[1].operation = HFOC_SPAWN_ADOPT;
    actions[1].source = 1U;
    actions[2].destination = 2U;
    actions[2].operation = HFOC_SPAWN_ADOPT;
    actions[2].source = 2U;
    actions[3].destination = 0U;
    actions[3].operation = HFOC_SPAWN_PGID;
    actions[3].source = 0U;
    sh_env_build();
    /* The background wrapper must not take the terminal foreground: only the
       owner shell that is the session leader drives job control. */
    if (sh_var_count + 1U < SH_VARS_MAX + 2U) {
        sh_envp[sh_var_count] = "HFOC_JOB_CONTROL=0";
        sh_envp[sh_var_count + 1U] = NULL;
    }
    pid = hfoc_spawn("/bin/sh", argv, sh_envp, actions, 4U);
    sh_envp[sh_var_count] = NULL;
    if (pid < 0L) {
        sh_job_clear(job);
        hfoc_eprintf("%s: cannot start background command: %s\n",
                     sh_program_name, hfoc_strerror(pid));
        return 1;
    }
    job->pgid = (uint32_t)pid;
    sh_job_add_pid(job, (uint32_t)pid);
    hfoc_printf("[%d] %ld\n", sh_job_index(job), pid);
    return 0;
}

/* Expand the word tokens of a `for ... in` header into bounded strings. */
static char sh_for_words[SH_FOR_WORDS_MAX][SH_VALUE_MAX];

static int sh_expand_word_range(uint32_t first, uint32_t last,
                                uint32_t *count_out)
{
    size_t used = 0U;
    uint32_t token;
    uint32_t count = 0U;

    for (token = first; token < last; ++token) {
        const struct sh_stok *word = &sh_stoks[token];
        const size_t length = (size_t)(word->end - word->start);

        if (used != 0U) {
            if (used + 1U >= sizeof(sh_line)) {
                sh_error("for: word list is too long");
                return 0;
            }
            sh_line[used++] = ' ';
        }
        if (used + length + 1U >= sizeof(sh_line)) {
            sh_error("for: word list is too long");
            return 0;
        }
        hfoc_memcpy(sh_line + used, sh_script + word->start, length);
        used += length;
    }
    sh_line[used] = '\0';
    sh_line_pos = 0U;
    sh_token_used = 0U;
    sh_token_ready = 0;
    sh_pending_count = 0U;
    sh_pending_index = 0U;
    for (;;) {
        const enum sh_token_type type = sh_next_token();

        if (type == SH_T_EOF) {
            break;
        }
        if (type != SH_T_WORD) {
            sh_error("for: bad word list");
            return 0;
        }
        if (count >= SH_FOR_WORDS_MAX) {
            sh_error("for: too many words");
            return 0;
        }
        if (hfoc_strlen(sh_token.text) >= SH_VALUE_MAX) {
            sh_error("for: word is too long");
            return 0;
        }
        hfoc_strcpy(sh_for_words[count], sh_token.text);
        ++count;
        sh_token_ready = 0;
    }
    *count_out = count;
    return 1;
}

static int sh_exec_node(uint16_t index, uint32_t depth);

static int sh_exec_seq(uint16_t index, uint32_t depth)
{
    const struct sh_node *node = &sh_nodes[index];
    uint32_t offset;
    int status = 0;

    for (offset = 0U; offset < node->count; ++offset) {
        status = sh_exec_node(sh_seq_children[node->first + offset], depth);
        if (sh_exit_requested || sh_return_requested || sh_break_requested ||
            sh_continue_requested) {
            break;
        }
    }
    return status;
}

static int sh_exec_node(uint16_t index, uint32_t depth)
{
    struct sh_node *node;
    int status = 0;

    if (index == 0xFFFFU) {
        return 0;
    }
    if (depth > SH_CALL_MAX) {
        sh_error("maximum call depth exceeded");
        return 2;
    }
    node = &sh_nodes[index];
    switch (node->kind) {
    case SH_N_EMPTY:
        return 0;
    case SH_N_SIMPLE:
        if (node->background) {
            static char background_text[SH_LINE_MAX];
            const size_t length = (size_t)(node->raw_end - node->raw_start);

            if (length + 1U >= sizeof(background_text)) {
                sh_error("background command is too long");
                return 2;
            }
            hfoc_memcpy(background_text, sh_script + node->raw_start, length);
            background_text[length] = '\0';
            return sh_spawn_background(background_text);
        }
        return sh_execute_slice(node->raw_start, node->raw_end);
    case SH_N_SEQ:
        return sh_exec_seq(index, depth);
    case SH_N_IF:
        status = sh_exec_node(node->a, depth + 1U);
        sh_last_status = status;
        if (sh_exit_requested || sh_return_requested) {
            return status;
        }
        if (status == 0) {
            return sh_exec_node(node->b, depth + 1U);
        }
        if (node->c != 0xFFFFU) {
            return sh_exec_node(node->c, depth + 1U);
        }
        return 0;
    case SH_N_FOR: {
        uint32_t count = 0U;
        uint32_t iteration;

        if (node->positional) {
            count = sh_pos_count;
            for (iteration = 0U; iteration < count; ++iteration) {
                hfoc_strcpy(sh_for_words[iteration], sh_pos[iteration]);
            }
        } else if (!sh_expand_word_range(node->a, node->b, &count)) {
            return 2;
        }
        status = 0;
        for (iteration = 0U; iteration < count; ++iteration) {
            if (iteration >= SH_LOOP_MAX) {
                sh_error("for: iteration limit exceeded");
                return 2;
            }
            sh_var_set(node->name, sh_for_words[iteration]);
            ++sh_loop_depth;
            status = sh_exec_node(node->body, depth + 1U);
            --sh_loop_depth;
            sh_last_status = status;
            if (sh_exit_requested || sh_return_requested) {
                break;
            }
            if (sh_break_requested) {
                sh_break_requested = 0;
                break;
            }
            if (sh_continue_requested) {
                sh_continue_requested = 0;
                continue;
            }
        }
        return status;
    }
    case SH_N_WHILE: {
        uint32_t iteration = 0U;

        status = 0;
        for (;;) {
            int condition = sh_exec_node(node->a, depth + 1U);

            sh_last_status = condition;
            if (sh_exit_requested || sh_return_requested) {
                break;
            }
            if (node->negate) {
                condition = condition == 0 ? 1 : 0;
            }
            if (condition != 0) {
                break;
            }
            if (++iteration > SH_LOOP_MAX) {
                sh_error("while: iteration limit exceeded");
                return 2;
            }
            ++sh_loop_depth;
            status = sh_exec_node(node->body, depth + 1U);
            --sh_loop_depth;
            sh_last_status = status;
            if (sh_exit_requested || sh_return_requested) {
                break;
            }
            if (sh_break_requested) {
                sh_break_requested = 0;
                break;
            }
            if (sh_continue_requested) {
                sh_continue_requested = 0;
            }
        }
        return status;
    }
    case SH_N_FUNC:
        if (!sh_register_function(node->name, node->body)) {
            return 2;
        }
        sh_last_status = 0;
        return 0;
    case SH_N_SUBSHELL: {
        struct sh_state_frame *frame;
        int saved_exit;
        int saved_exit_status;
        int saved_return;
        int saved_return_status;
        uint32_t saved_functions;

        if (sh_subshell_depth >= SH_SUBSHELL_MAX) {
            sh_error("maximum subshell depth exceeded");
            return 2;
        }
        frame = &sh_subshell_frames[sh_subshell_depth++];
        frame->var_count = sh_var_count;
        {
            uint32_t variable;

            for (variable = 0U; variable < sh_var_count; ++variable) {
                frame->vars[variable] = sh_vars[variable];
            }
        }
        frame->cwd[0] = '\0';
        (void)hfoc_getcwd(frame->cwd, sizeof(frame->cwd));
        sh_positional_save(frame);
        frame->last_status = sh_last_status;
        saved_exit = sh_exit_requested;
        saved_exit_status = sh_exit_status;
        saved_return = sh_return_requested;
        saved_return_status = sh_return_status;
        saved_functions = sh_function_count;
        sh_exit_requested = 0;
        sh_return_requested = 0;
        sh_exit_status = 0;
        sh_return_status = 0;
        status = sh_exec_node(node->body, depth + 1U);
        sh_exit_requested = saved_exit;
        sh_exit_status = saved_exit_status;
        sh_return_requested = saved_return;
        sh_return_status = saved_return_status;
        sh_function_count = saved_functions;
        sh_vars_restore(frame);
        sh_positional_restore(frame);
        sh_last_status = frame->last_status;
        if (frame->cwd[0] != '\0') {
            (void)hfoc_chdir(frame->cwd);
        }
        --sh_subshell_depth;
        return status;
    }
    case SH_N_GROUP:
        return sh_exec_node(node->body, depth + 1U);
    default:
        sh_error("internal shell error");
        return 2;
    }
}

static int sh_is_builtin(const char *name)
{
    return sh_streq(name, "cd") || sh_streq(name, "export") ||
           sh_streq(name, "unset") || sh_streq(name, "exit") ||
           sh_streq(name, "test") || sh_streq(name, "[") ||
           sh_streq(name, "trap") || sh_streq(name, "return") ||
           sh_streq(name, "break") || sh_streq(name, "continue") ||
           sh_streq(name, "shift") || sh_streq(name, "jobs") ||
           sh_streq(name, "fg") || sh_streq(name, "bg") ||
           sh_streq(name, "wait");
}

static int sh_test_number(const char *text, long *out)
{
    int ok = 0;
    const long value = hfoc_parse_int(text, &ok);

    if (!ok) {
        return 0;
    }
    *out = value;
    return 1;
}

static int sh_test_command(struct sh_command *command)
{
    char **argv = command->argv + 1;
    uint32_t argc = command->argc - 1U;
    struct hfoc_stat stat;

    if (sh_streq(command->argv[0], "[")) {
        if (argc == 0U || !sh_streq(argv[argc - 1U], "]")) {
            sh_error("[: missing ']'");
            return 2;
        }
        --argc;
    }
    if (argc == 0U) {
        return 1;
    }
    if (argc == 1U) {
        return argv[0][0] == '\0' ? 1 : 0;
    }
    if (argc == 2U) {
        if (sh_streq(argv[0], "!")) {
            return argv[1][0] == '\0' ? 0 : 1;
        }
        if (sh_streq(argv[0], "-n")) {
            return argv[1][0] == '\0' ? 1 : 0;
        }
        if (sh_streq(argv[0], "-z")) {
            return argv[1][0] == '\0' ? 0 : 1;
        }
        if (sh_streq(argv[0], "-e") || sh_streq(argv[0], "-f") ||
            sh_streq(argv[0], "-d") || sh_streq(argv[0], "-r") ||
            sh_streq(argv[0], "-w")) {
            if (hfoc_stat(argv[1], &stat) != 0) {
                return 1;
            }
            if (sh_streq(argv[0], "-d")) {
                return stat.type == HFOC_STAT_DIR ? 0 : 1;
            }
            if (sh_streq(argv[0], "-f")) {
                return stat.type == HFOC_STAT_FILE ? 0 : 1;
            }
            if (sh_streq(argv[0], "-w")) {
                return (stat.flags & HFOC_STAT_RDONLY) != 0U ? 1 : 0;
            }
            return 0;
        }
        sh_error("test: unsupported expression");
        return 2;
    }
    if (argc == 3U) {
        long left;
        long right;

        if (sh_streq(argv[1], "=")) {
            return sh_streq(argv[0], argv[2]) ? 0 : 1;
        }
        if (sh_streq(argv[1], "!=")) {
            return sh_streq(argv[0], argv[2]) ? 1 : 0;
        }
        if (!sh_test_number(argv[0], &left) ||
            !sh_test_number(argv[2], &right)) {
            sh_error("test: numeric operand is not an integer");
            return 2;
        }
        if (sh_streq(argv[1], "-eq")) {
            return left == right ? 0 : 1;
        }
        if (sh_streq(argv[1], "-ne")) {
            return left != right ? 0 : 1;
        }
        if (sh_streq(argv[1], "-lt")) {
            return left < right ? 0 : 1;
        }
        if (sh_streq(argv[1], "-le")) {
            return left <= right ? 0 : 1;
        }
        if (sh_streq(argv[1], "-gt")) {
            return left > right ? 0 : 1;
        }
        if (sh_streq(argv[1], "-ge")) {
            return left >= right ? 0 : 1;
        }
        sh_error("test: unsupported expression");
        return 2;
    }
    sh_error("test: unsupported expression");
    return 2;
}


static int sh_resolve(const char *name, char *path_out)
{
    const char *path;
    const char *cursor;

    if (hfoc_strchr(name, '/') != NULL) {
        if (hfoc_strlen(name) >= SH_PATH_MAX) {
            return 0;
        }
        hfoc_strcpy(path_out, name);
        return 1;
    }
    path = sh_var_find("PATH");
    if (path == NULL || path[0] == '\0') {
        path = "/bin";
    }
    cursor = path;
    while (*cursor != '\0') {
        const char *end = hfoc_strchr(cursor, ':');
        const size_t length = end != NULL ? (size_t)(end - cursor)
                                         : hfoc_strlen(cursor);
        struct hfoc_stat stat;

        if (length != 0U && length + 1U + hfoc_strlen(name) < SH_PATH_MAX) {
            hfoc_memcpy(path_out, cursor, length);
            path_out[length] = '/';
            hfoc_strcpy(path_out + length + 1U, name);
            if (hfoc_stat(path_out, &stat) == 0 &&
                stat.type == HFOC_STAT_FILE) {
                return 1;
            }
        }
        if (end == NULL) {
            break;
        }
        cursor = end + 1;
    }
    return 0;
}

static int sh_builtin(struct sh_command *command)
{
    const char *name = command->argv[0];

    if (sh_streq(name, "cd")) {
        const char *target = command->argc > 1U ? command->argv[1]
                                                : sh_var_find("HOME");
        char cwd[SH_PATH_MAX];

        if (target == NULL || target[0] == '\0') {
            target = "/";
        }
        if (hfoc_chdir(target) != 0) {
            sh_error("cd: cannot change directory");
            return 1;
        }
        if (hfoc_getcwd(cwd, sizeof(cwd)) > 0) {
            sh_var_set("PWD", cwd);
        }
        return 0;
    }
    if (sh_streq(name, "export")) {
        uint32_t index;

        for (index = 1U; index < command->argc; ++index) {
            char *equals = hfoc_strchr(command->argv[index], '=');

            if (equals != NULL) {
                char variable[SH_NAME_MAX];
                const size_t length = (size_t)(equals - command->argv[index]);

                if (length >= sizeof(variable) || length == 0U) {
                    sh_error("export: bad name");
                    return 1;
                }
                hfoc_strncpy(variable, command->argv[index], length);
                variable[length] = '\0';
                sh_var_set(variable, equals + 1);
            }
        }
        return 0;
    }
    if (sh_streq(name, "unset")) {
        uint32_t index;

        for (index = 1U; index < command->argc; ++index) {
            sh_var_unset(command->argv[index]);
        }
        return 0;
    }
    if (sh_streq(name, "exit")) {
        int status = sh_last_status;

        if (command->argc > 1U) {
            int ok = 0;
            const long parsed = hfoc_parse_int(command->argv[1], &ok);

            if (ok) {
                status = (int)parsed;
            }
        }
        sh_exit_requested = 1;
        sh_exit_status = status & 0xff;
        return sh_exit_status;
    }
    if (sh_streq(name, "test") || sh_streq(name, "[")) {
        return sh_test_command(command);
    }
    if (sh_streq(name, "trap")) {
        if (command->argc == 1U) {
            return 0;
        }
        if (command->argc != 3U || !sh_streq(command->argv[2], "EXIT")) {
            sh_error("trap: only 'trap COMMAND EXIT' is supported");
            return 2;
        }
        if (sh_streq(command->argv[1], "-")) {
            sh_trap_set = 0;
            sh_trap_exit[0] = '\0';
            return 0;
        }
        if (hfoc_strlen(command->argv[1]) >= sizeof(sh_trap_exit)) {
            sh_error("trap: command is too long");
            return 2;
        }
        hfoc_strcpy(sh_trap_exit, command->argv[1]);
        sh_trap_set = 1;
        return 0;
    }
    if (sh_streq(name, "return")) {
        int status = sh_last_status;

        if (sh_call_depth == 0U) {
            sh_error("return: not in a function");
            return 2;
        }
        if (command->argc > 1U) {
            int ok = 0;
            const long parsed = hfoc_parse_int(command->argv[1], &ok);

            if (ok) {
                status = (int)parsed;
            }
        }
        sh_return_requested = 1;
        sh_return_status = status & 0xff;
        return sh_return_status;
    }
    if (sh_streq(name, "break") || sh_streq(name, "continue")) {
        const int is_break = sh_streq(name, "break");

        if (sh_loop_depth == 0U) {
            sh_error(is_break ? "break: not in a loop"
                              : "continue: not in a loop");
            return 2;
        }
        if (command->argc > 1U && !sh_streq(command->argv[1], "1")) {
            sh_error("break/continue levels other than 1 are not supported");
            return 2;
        }
        if (is_break) {
            sh_break_requested = 1;
        } else {
            sh_continue_requested = 1;
        }
        return 0;
    }
    if (sh_streq(name, "jobs")) {
        uint32_t index;

        for (index = 0U; index < SH_JOBS_MAX; ++index) {
            if (sh_jobs[index].used) {
                sh_job_report(&sh_jobs[index], 1);
            }
        }
        return 0;
    }
    if (sh_streq(name, "bg") || sh_streq(name, "fg")) {
        const int foreground = sh_streq(name, "fg");
        const int wanted = sh_job_find_index(
            command->argc > 1U ? command->argv[1] : NULL);
        const int picked = sh_job_pick(wanted);
        struct sh_job *job;
        int status = 1;

        if (picked < 0) {
            sh_error(foreground ? "fg: no such job" : "bg: no such job");
            return 1;
        }
        job = &sh_jobs[picked - 1];
        if (job->state == SH_JOB_DONE) {
            return job->status;
        }
        if (job->state == SH_JOB_STOPPED) {
            (void)hfoc_kill(job->pgid, HFOC_SIG_CONT, 1);
            job->state = SH_JOB_RUNNING;
        }
        if (!foreground) {
            hfoc_printf("[%d]+ %s &\n", picked, job->text);
            return 0;
        }
        (void)hfoc_tty_foreground(job->pgid);
        {
            const int stopped = sh_job_wait(job, &status);

            (void)hfoc_tty_foreground(0U);
            if (stopped) {
                hfoc_printf("[%d]+ Stopped %s\n", picked, job->text);
            } else {
                job->state = SH_JOB_DONE;
                hfoc_printf("[%d]+ Done %s\n", picked, job->text);
            }
        }
        return status;
    }
    if (sh_streq(name, "wait")) {
        int status = 0;
        uint32_t index;

        if (command->argc > 1U) {
            const int picked = sh_job_find_index(command->argv[1]);

            if (picked < 0) {
                sh_error("wait: no such job");
                return 1;
            }
            (void)sh_job_wait(&sh_jobs[picked - 1], &status);
            return status;
        }
        for (index = 0U; index < SH_JOBS_MAX; ++index) {
            if (sh_jobs[index].used &&
                sh_jobs[index].state != SH_JOB_DONE) {
                (void)sh_job_wait(&sh_jobs[index], &status);
            }
        }
        return status;
    }
    if (sh_streq(name, "shift")) {
        uint32_t count = 1U;
        uint32_t index;

        if (command->argc > 1U) {
            int ok = 0;
            const long parsed = hfoc_parse_int(command->argv[1], &ok);

            if (!ok || parsed < 0) {
                sh_error("shift: bad count");
                return 2;
            }
            count = (uint32_t)parsed;
        }
        if (count > sh_pos_count) {
            return 1;
        }
        for (index = 0U; index + count < sh_pos_count; ++index) {
            hfoc_strcpy(sh_pos[index], sh_pos[index + count]);
        }
        sh_pos_count -= count;
        for (index = sh_pos_count; index < SH_POS_MAX; ++index) {
            sh_pos[index][0] = '\0';
        }
        return 0;
    }
    return 1;
}

static int sh_redirect_builtin(struct sh_command *command)
{
    int saved_out = -1;
    int saved_err = -1;
    uint32_t index;

    if (command->redir_count == 0U) {
        return 1;
    }
    saved_out = (int)hfoc_dup2(1, SH_SAVED_STDOUT);
    saved_err = (int)hfoc_dup2(2, SH_SAVED_STDERR);
    if (saved_out < 0 || saved_err < 0) {
        sh_error("cannot save standard streams");
        return 0;
    }
    for (index = 0U; index < command->redir_count; ++index) {
        const struct sh_redir *redir = &command->redirs[index];

        if (redir->type == SH_R_ERR_TO_OUT) {
            if (hfoc_dup2(1, 2) < 0) {
                sh_error("cannot redirect stderr");
                return 0;
            }
            continue;
        }
        if (redir->type == SH_R_IN) {
            const long fd = hfoc_open(redir->path, HFOC_OPEN_READ);

            if (fd < 0 || hfoc_dup2((int)fd, 0) < 0) {
                sh_error("cannot open input redirection");
                return 0;
            }
            (void)hfoc_close((int)fd);
            continue;
        }
        {
            const uint32_t flags = HFOC_OPEN_WRITE | HFOC_OPEN_CREATE |
                (redir->type == SH_R_APPEND ? 0U : HFOC_OPEN_TRUNC);
            const long fd = hfoc_open(redir->path, flags);
            const int target = redir->type == SH_R_ERR ? 2 : 1;

            if (fd < 0 || hfoc_dup2((int)fd, target) < 0) {
                sh_error("cannot open output redirection");
                return 0;
            }
            (void)hfoc_close((int)fd);
        }
    }
    return 1;
}

static void sh_restore_builtin(int had_redirs)
{
    if (!had_redirs) {
        return;
    }
    (void)hfoc_dup2(SH_SAVED_STDOUT, 1);
    (void)hfoc_dup2(SH_SAVED_STDERR, 2);
    (void)hfoc_close(SH_SAVED_STDOUT);
    (void)hfoc_close(SH_SAVED_STDERR);
}

/*
 * ITEM-659: a name that does not resolve is still a command with its own
 * redirections.  Open them first, so `nosuch 2>/tmp/e` creates `/tmp/e` and
 * writes the diagnostic there, then report status 127 like the rest of the
 * shell's missing-name paths.
 */
static int sh_not_found(struct sh_command *command)
{
    const int had_redirs = command->redir_count != 0U;

    (void)sh_redirect_builtin(command);
    hfoc_eprintf("%s: %s: command not found\n", sh_program_name,
                 command->argv[0]);
    sh_restore_builtin(had_redirs);
    return SH_SPAWN_NOT_FOUND;
}

static void sh_actions_add(struct hfoc_fd_action *actions,
                           uint32_t *count, uint32_t destination,
                           uint32_t operation, uint32_t source,
                           uint32_t flags, const char *path)
{
    struct hfoc_fd_action *action;

    if (*count >= HFOC_SPAWN_ACTION_MAX) {
        sh_error("too many fd actions");
        return;
    }
    action = &actions[*count];
    hfoc_memset(action, 0, sizeof(*action));
    action->destination = destination;
    action->operation = operation;
    action->source = source;
    action->flags = flags;
    if (path != NULL) {
        hfoc_strncpy(action->path, path, HFOC_PATH_MAX - 1U);
        action->path[HFOC_PATH_MAX - 1U] = '\0';
    }
    ++*count;
}

static int sh_spawn_stage(struct sh_command *command, int stdin_fd,
                          int stdout_fd, int stderr_file, int stderr_to_out,
                          int set_pgid, uint32_t pgid)
{
    /* The action record carries a path per entry; keep the 8 KiB array off
       the 16 KiB user stack.  The shell is single-threaded. */
    static struct hfoc_fd_action actions[HFOC_SPAWN_ACTION_MAX];
    static char path[SH_PATH_MAX];
    uint32_t action_count = 0U;
    uint32_t index;
    int had_action_failure = 0;
    int append_fd = -1;
    int redirected[3] = { 0, 0, 0 };

    if (!sh_resolve(command->argv[0], path)) {
        return sh_not_found(command);
    }
    if (set_pgid) {
        sh_actions_add(actions, &action_count, 0U, HFOC_SPAWN_PGID, pgid, 0U,
                       NULL);
    }
    if (stdin_fd >= 0) {
        sh_actions_add(actions, &action_count, 0U, HFOC_SPAWN_ADOPT,
                       (uint32_t)stdin_fd, 0U, NULL);
        redirected[0] = 1;
    }
    if (stdout_fd >= 0) {
        sh_actions_add(actions, &action_count, 1U, HFOC_SPAWN_ADOPT,
                       (uint32_t)stdout_fd, 0U, NULL);
        redirected[1] = 1;
    }
    if (stderr_to_out) {
        sh_actions_add(actions, &action_count, 2U, HFOC_SPAWN_DUP, 1U, 0U,
                       NULL);
        redirected[2] = 1;
    } else if (stderr_file >= 0) {
        sh_actions_add(actions, &action_count, 2U, HFOC_SPAWN_ADOPT,
                       (uint32_t)stderr_file, 0U, NULL);
        redirected[2] = 1;
    }
    for (index = 0U; index < command->redir_count; ++index) {
        const struct sh_redir *redir = &command->redirs[index];
        uint32_t flags;

        if (redir->type == SH_R_IN) {
            sh_actions_add(actions, &action_count, 0U, HFOC_SPAWN_OPEN, 0U,
                           HFOC_OPEN_READ, redir->path);
            redirected[0] = 1;
        } else if (redir->type == SH_R_APPEND) {
            /* The VFS has no O_APPEND flag; open in the shell, seek to the
               end once, and let the child adopt that open description. */
            const long fd = hfoc_open(redir->path,
                                      HFOC_OPEN_WRITE | HFOC_OPEN_CREATE);

            if (fd < 0 || hfoc_lseek((int)fd, 0, HFOC_SEEK_END) < 0) {
                if (fd >= 0) {
                    (void)hfoc_close((int)fd);
                }
                hfoc_eprintf("%s: cannot append to %s\n", sh_program_name,
                             redir->path);
                return -1;
            }
            if (append_fd >= 0) {
                (void)hfoc_close(append_fd);
            }
            append_fd = (int)fd;
            sh_actions_add(actions, &action_count, 1U, HFOC_SPAWN_ADOPT,
                           (uint32_t)fd, 0U, NULL);
            redirected[1] = 1;
        } else if (redir->type == SH_R_OUT) {
            flags = HFOC_OPEN_WRITE | HFOC_OPEN_CREATE | HFOC_OPEN_TRUNC;
            sh_actions_add(actions, &action_count, 1U, HFOC_SPAWN_OPEN, 0U,
                           flags, redir->path);
            redirected[1] = 1;
        } else if (redir->type == SH_R_ERR) {
            flags = HFOC_OPEN_WRITE | HFOC_OPEN_CREATE | HFOC_OPEN_TRUNC;
            sh_actions_add(actions, &action_count, 2U, HFOC_SPAWN_OPEN, 0U,
                           flags, redir->path);
            redirected[2] = 1;
        } else {
            sh_actions_add(actions, &action_count, 2U, HFOC_SPAWN_DUP, 1U,
                           0U, NULL);
            redirected[2] = 1;
        }
    }
    /*
     * ITEM-629: a command with no redirection inherits the shell's own three
     * streams.  Without these actions SPAWN would bind the kernel console,
     * which is not the owner's SSH session.
     */
    for (index = 0U; index < 3U; ++index) {
        if (!redirected[index]) {
            sh_actions_add(actions, &action_count, index, HFOC_SPAWN_ADOPT,
                           index, 0U, NULL);
        }
    }
    sh_env_build();
    {
        const long pid = hfoc_spawn(path, command->argv, sh_envp, actions,
                                    action_count);

        if (append_fd >= 0) {
            (void)hfoc_close(append_fd);
        }
        if (pid < 0) {
            hfoc_eprintf("%s: cannot start %s: %s\n", sh_program_name, path,
                         hfoc_strerror(pid));
            had_action_failure = 1;
        }
        return had_action_failure ? -1 : (int)pid;
    }
}

static void sh_wait_children(uint32_t count, int last_pid, int *status_out)
{
    uint32_t reaped = 0U;
    int last_status = 1;

    while (reaped < count) {
        int status = 0;
        const long pid = hfoc_wait(&status);

        if (pid < 0) {
            break;
        }
        ++reaped;
        if ((int)pid == last_pid) {
            last_status = status & 0xff;
        }
    }
    *status_out = last_status;
}

static struct sh_function *sh_function_find(const char *name)
{
    uint32_t index;

    for (index = 0U; index < sh_function_count; ++index) {
        if (sh_streq(sh_functions[index].name, name)) {
            return &sh_functions[index];
        }
    }
    return NULL;
}

static int sh_call_function(struct sh_command *command)
{
    struct sh_function *function = sh_function_find(command->argv[0]);
    struct sh_call_frame *frame;
    uint32_t index;
    int status;

    if (function == NULL) {
        sh_error("function not found");
        return 127;
    }
    if (sh_call_depth >= SH_CALL_MAX) {
        sh_error("maximum call depth exceeded");
        return 2;
    }
    frame = &sh_call_frames[sh_call_depth++];
    frame->count = sh_pos_count;
    for (index = 0U; index < SH_POS_MAX; ++index) {
        hfoc_strcpy(frame->pos[index], sh_pos[index]);
    }
    sh_pos_count = command->argc > 1U ? command->argc - 1U : 0U;
    if (sh_pos_count > SH_POS_MAX) {
        sh_pos_count = SH_POS_MAX;
    }
    for (index = 0U; index < sh_pos_count; ++index) {
        hfoc_strncpy(sh_pos[index], command->argv[index + 1U],
                     SH_VALUE_MAX - 1U);
        sh_pos[index][SH_VALUE_MAX - 1U] = '\0';
    }
    {
        const int had_redirs = command->redir_count != 0U;

        if (!sh_redirect_builtin(command)) {
            sh_restore_builtin(had_redirs);
            status = 1;
        } else {
            status = sh_exec_node(function->body, 0U);
            sh_restore_builtin(had_redirs);
        }
    }
    for (index = 0U; index < SH_POS_MAX; ++index) {
        hfoc_strcpy(sh_pos[index], frame->pos[index]);
    }
    sh_pos_count = frame->count;
    --sh_call_depth;
    if (sh_return_requested) {
        status = sh_return_status;
        sh_return_requested = 0;
    }
    return status;
}

struct sh_assign_save {
    char name[SH_NAME_MAX];
    char value[SH_VALUE_MAX];
    int existed;
};

static void sh_restore_assignments(struct sh_assign_save *saved,
                                   uint32_t count)
{
    while (count != 0U) {
        const struct sh_assign_save *entry = &saved[--count];

        if (entry->existed) {
            sh_var_set(entry->name, entry->value);
        } else {
            sh_var_unset(entry->name);
        }
    }
}

/* ITEM-633: wait for a foreground pipeline, reporting a stop instead of
   consuming it.  alives[] marks the PIDs that still belong to the job. */
static int sh_wait_foreground(int *pids, uint32_t count, int *status_out,
                              int *alive)
{
    uint32_t remaining = 0U;
    uint32_t index;

    for (index = 0U; index < count; ++index) {
        alive[index] = pids[index] >= 0;
        if (alive[index]) {
            ++remaining;
        }
    }
    while (remaining != 0U) {
        uint32_t result[2] = { 0U, 0U };
        const long wait_status = hfoc_wait2(HFOC_WAIT_UNTRACED, result);

        if (wait_status != 0L) {
            if (wait_status == HFOC_EAGAIN) {
                (void)hfoc_yield();
                continue;
            }
            break;
        }
        for (index = 0U; index < count; ++index) {
            if (alive[index] && (uint32_t)pids[index] == result[0]) {
                if (SH_WIFSTOPPED(result[1])) {
                    *status_out = 128 + (int)SH_WSTOPSIG(result[1]);
                    return 1;
                }
                alive[index] = 0;
                --remaining;
                if (SH_WIFEXITED(result[1])) {
                    *status_out = (int)SH_WEXITSTATUS(result[1]);
                } else {
                    *status_out = 128 + (int)SH_WTERMSIG(result[1]);
                }
                break;
            }
        }
    }
    return 0;
}

static int sh_run_pipeline(struct sh_pipeline *pipeline)
{
    int pipe_fds[SH_STAGES_MAX][2];
    int pids[SH_STAGES_MAX];
    uint32_t index;
    int status = 1;
    static struct sh_assign_save saved[SH_ASSIGN_MAX];
    uint32_t saved_count = 0U;

    if (pipeline->count == 1U) {
        struct sh_command *command = &pipeline->stages[0];
        uint32_t first = 0U;

        while (first < command->argc) {
            char *equals = hfoc_strchr(command->argv[first], '=');
            char name[SH_NAME_MAX];
            size_t length;
            const char *old;

            if (equals == NULL || equals == command->argv[first]) {
                break;
            }
            length = (size_t)(equals - command->argv[first]);
            if (length >= sizeof(name)) {
                break;
            }
            hfoc_memcpy(name, command->argv[first], length);
            name[length] = '\0';
            if (!sh_identifier(name)) {
                break;
            }
            if (saved_count >= SH_ASSIGN_MAX) {
                sh_error("too many assignment words");
                return 2;
            }
            hfoc_strcpy(saved[saved_count].name, name);
            old = sh_var_find(name);
            saved[saved_count].existed = old != NULL;
            hfoc_strcpy(saved[saved_count].value, old != NULL ? old : "");
            ++saved_count;
            sh_var_set(name, equals + 1);
            ++first;
        }
        if (first != 0U) {
            if (first == command->argc) {
                return 0; /* a standalone assignment persists */
            }
            for (index = first; index < command->argc; ++index) {
                command->argv[index - first] = command->argv[index];
            }
            command->argc -= first;
            command->argv[command->argc] = NULL;
        }
        if (sh_function_find(command->argv[0]) != NULL) {
            status = sh_call_function(command);
            sh_restore_assignments(saved, saved_count);
            return status;
        }
        if (sh_is_builtin(command->argv[0])) {
            const int had_redirs = command->redir_count != 0U;

            if (!sh_redirect_builtin(command)) {
                sh_restore_builtin(had_redirs);
                sh_restore_assignments(saved, saved_count);
                return 1;
            }
            status = sh_builtin(command);
            sh_restore_builtin(had_redirs);
            sh_restore_assignments(saved, saved_count);
            return status;
        }
    }
    for (index = 0U; index < pipeline->count; ++index) {
        if (sh_is_builtin(pipeline->stages[index].argv[0]) ||
            sh_function_find(pipeline->stages[index].argv[0]) != NULL) {
            sh_error("builtin or function in a pipeline is not supported");
            sh_restore_assignments(saved, saved_count);
            return 1;
        }
        pids[index] = -1;
    }
    for (index = 0U; index + 1U < pipeline->count; ++index) {
        int fds[2];

        if (hfoc_pipe(fds) < 0) {
            sh_error("cannot create a pipe");
            sh_restore_assignments(saved, saved_count);
            return 1;
        }
        pipe_fds[index][0] = fds[0];
        pipe_fds[index][1] = fds[1];
    }
    {
        const int job_control = sh_job_control;
        uint32_t leader = 0U;
        int alive[SH_STAGES_MAX];

        for (index = 0U; index < pipeline->count; ++index) {
            const int stdin_fd = index > 0U ? pipe_fds[index - 1U][0] : -1;
            const int stdout_fd =
                index + 1U < pipeline->count ? pipe_fds[index][1] : -1;
            const uint32_t pgid = index == 0U ? 0U : leader;
            const int pid = sh_spawn_stage(
                &pipeline->stages[index], stdin_fd, stdout_fd, -1, 0,
                job_control, pgid);

            if (pid < 0) {
                uint32_t close_index;

                for (close_index = 0U; close_index + 1U < pipeline->count;
                     ++close_index) {
                    (void)hfoc_close(pipe_fds[close_index][0]);
                    (void)hfoc_close(pipe_fds[close_index][1]);
                }
                if (job_control) {
                    (void)hfoc_tty_foreground(0U);
                }
                sh_wait_children(index, -1, &status);
                sh_restore_assignments(saved, saved_count);
                return pid == SH_SPAWN_NOT_FOUND ? 127 : 1;
            }
            pids[index] = pid;
            if (index == 0U && job_control) {
                leader = (uint32_t)pid;
                (void)hfoc_tty_foreground(leader);
            }
        }
        for (index = 0U; index + 1U < pipeline->count; ++index) {
            (void)hfoc_close(pipe_fds[index][0]);
            (void)hfoc_close(pipe_fds[index][1]);
        }
        if (job_control) {
            const int stopped = sh_wait_foreground(
                pids, pipeline->count, &status, alive);

            (void)hfoc_tty_foreground(0U);
            if (stopped) {
                struct sh_job *job = sh_job_alloc(sh_current_text);

                if (job == NULL) {
                    sh_error("too many stopped jobs");
                    (void)hfoc_kill(leader, HFOC_SIG_TERM, 1);
                    (void)hfoc_kill(leader, HFOC_SIG_CONT, 1);
                } else {
                    job->pgid = leader;
                    job->state = SH_JOB_STOPPED;
                    job->status = status;
                    for (index = 0U; index < pipeline->count; ++index) {
                        if (alive[index]) {
                            sh_job_add_pid(job, (uint32_t)pids[index]);
                        }
                    }
                    hfoc_printf("[%d]+ Stopped %s\n", sh_job_index(job),
                                job->text);
                }
            }
        } else {
            sh_wait_children(pipeline->count, pids[pipeline->count - 1U],
                             &status);
        }
    }
    sh_restore_assignments(saved, saved_count);
    return status;
}

static int sh_execute_andor(struct sh_andor *andor)
{
    uint32_t index;
    int status = 0;

    for (index = 0U; index < andor->count; ++index) {
        if (index != 0U) {
            if (andor->ops[index] == SH_OP_AND && status != 0) {
                continue;
            }
            if (andor->ops[index] == SH_OP_OR && status == 0) {
                continue;
            }
        }
        status = sh_run_pipeline(&andor->pipes[index]);
        sh_last_status = status;
        if (sh_exit_requested || sh_return_requested || sh_break_requested ||
            sh_continue_requested) {
            break;
        }
    }
    return status;
}

static int sh_execute_text(const char *text)
{
    size_t length = hfoc_strlen(text);

    while (length != 0U && sh_is_space(text[length - 1U])) {
        --length;
    }
    if (length == 0U) {
        return 0;
    }
    if (length >= SH_LINE_MAX) {
        sh_error("line too long");
        return 2;
    }
    sh_current_text = text;
    hfoc_memcpy(sh_line, text, length);
    sh_line[length] = '\0';
    sh_line_pos = 0U;
    sh_token_used = 0U;
    sh_token_ready = 0;
    sh_pending_count = 0U;
    sh_pending_index = 0U;
    for (;;) {
        if (!sh_parse_andor(&sh_andor)) {
            return 2;
        }
        (void)sh_execute_andor(&sh_andor);
        if (sh_exit_requested || sh_return_requested || sh_break_requested ||
            sh_continue_requested) {
            return sh_last_status;
        }
        if (!sh_token_ready) {
            sh_advance();
        }
        if (sh_token.type == SH_T_SEMI) {
            sh_token_ready = 0;
            continue;
        }
        if (sh_token.type == SH_T_EOF) {
            return sh_last_status;
        }
        sh_error("unexpected token");
        return 2;
    }
}

static void sh_run_exit_trap(void)
{
    char body[SH_LINE_MAX];

    if (!sh_trap_set || sh_trap_running) {
        return;
    }
    hfoc_strncpy(body, sh_trap_exit, sizeof(body) - 1U);
    body[sizeof(body) - 1U] = '\0';
    sh_trap_running = 1;
    sh_trap_set = 0;
    (void)sh_execute_text(body);
    sh_trap_running = 0;
}

/* Returns -2 for an incomplete interactive input, otherwise a real status. */
static int sh_run_buffer(int run_exit_trap)
{
    int status;

    if (!sh_parse_script()) {
        if (sh_parse_incomplete) {
            return -2;
        }
        if (run_exit_trap) {
            sh_run_exit_trap();
        }
        return 2;
    }
    sh_exit_requested = 0;
    sh_exit_status = 0;
    sh_return_requested = 0;
    sh_return_status = 0;
    sh_break_requested = 0;
    sh_continue_requested = 0;
    sh_loop_depth = 0U;
    sh_call_depth = 0U;
    sh_subshell_depth = 0U;
    if (!run_exit_trap) {
        /* Interactive buffers are parsed one at a time; a function body is a
           node index into that buffer.  Keep functions within one buffer. */
        sh_function_count = 0U;
    }
    status = sh_exec_node(sh_script_root, 0U);
    if (sh_exit_requested) {
        status = sh_exit_status;
    }
    sh_last_status = status;
    if (run_exit_trap) {
        sh_run_exit_trap();
    }
    return status;
}

static void sh_set_positional(char **args, uint32_t count)
{
    uint32_t index;

    sh_pos_count = count > SH_POS_MAX ? SH_POS_MAX : count;
    for (index = 0U; index < sh_pos_count; ++index) {
        hfoc_strncpy(sh_pos[index], args[index], SH_VALUE_MAX - 1U);
        sh_pos[index][SH_VALUE_MAX - 1U] = '\0';
    }
    for (index = sh_pos_count; index < SH_POS_MAX; ++index) {
        sh_pos[index][0] = '\0';
    }
}

static int sh_run_script(const char *path)
{
    const long fd = hfoc_open(path, HFOC_OPEN_READ);
    size_t used = 0U;
    int status;

    if (fd < 0) {
        sh_error("cannot open script");
        return 127;
    }
    for (;;) {
        char chunk[256];
        const long count = hfoc_read((int)fd, chunk, sizeof(chunk));

        if (count < 0L) {
            sh_error("cannot read script");
            (void)hfoc_close((int)fd);
            return 126;
        }
        if (count == 0L) {
            break;
        }
        if (used + (size_t)count + 1U > sizeof(sh_script)) {
            sh_error("script exceeds 10240 bytes");
            (void)hfoc_close((int)fd);
            return 2;
        }
        hfoc_memcpy(sh_script + used, chunk, (size_t)count);
        used += (size_t)count;
    }
    (void)hfoc_close((int)fd);
    sh_script_len = used;
    sh_interactive = 0;
    if (used == 0U) {
        return 0;
    }
    status = sh_run_buffer(1);
    return status == -2 ? 2 : status;
}

static int sh_read_input_byte(void)
{
    unsigned char byte;

    return hfoc_read(0, &byte, 1U) > 0L ? (int)byte : -1;
}

/* Consume CSI and SS3, preserving a control byte that interrupts either. */
static int sh_read_escape(int *pending)
{
    int byte = sh_read_input_byte();
    int first;

    if (byte != '[' && byte != 'O') {
        *pending = byte;
        return 0;
    }
    first = byte = sh_read_input_byte();
    while (byte >= 0x20 && byte < 0x40) {
        byte = sh_read_input_byte();
    }
    if (byte < 0x40 || byte > 0x7e) {
        *pending = byte;
        return 0;
    }
    return byte == first ? byte : 0;
}

static size_t sh_utf8_width(unsigned char byte)
{
    if (byte >= 0xc2U && byte <= 0xdfU) {
        return 2U;
    }
    if (byte >= 0xe0U && byte <= 0xefU) {
        return 3U;
    }
    if (byte >= 0xf0U && byte <= 0xf4U) {
        return 4U;
    }
    return 1U;
}

static int sh_utf8_tail(int byte)
{
    return byte >= 0x80 && byte <= 0xbf;
}

static size_t sh_utf8_span(const char *text, size_t remaining)
{
    const size_t width = sh_utf8_width((unsigned char)text[0]);

    if (width > remaining) {
        return 1U;
    }
    for (size_t index = 1U; index < width; ++index) {
        if (!sh_utf8_tail((unsigned char)text[index])) {
            return 1U;
        }
    }
    return width;
}

static size_t sh_utf8_previous(const char *text, size_t cursor)
{
    size_t start = cursor - 1U;

    while (start != 0U && cursor - start < 4U &&
           sh_utf8_tail((unsigned char)text[start])) {
        --start;
    }
    if (sh_utf8_span(text + start, cursor - start) == cursor - start) {
        return start;
    }
    return cursor - 1U;
}

static void sh_editor_start_row(size_t columns)
{
    static const char spaces[] = "                                ";
    const size_t chunk = sizeof(spaces) - 1U;

    /* At column one the CR cancels pending wrap. After partial command
       output the padding wraps first, retaining the previous row. */
    while (columns != 0U) {
        const size_t count = columns < chunk ? columns : chunk;

        (void)hfoc_write_all(1, spaces, count);
        columns -= count;
    }
    (void)hfoc_write_all(1, "\r", 1U);
}

/* UTF-8 byte count bounds the display width of the visible slice. Keep one
   spare column so the TTY stays on the same row, even at its bottom edge. */
static void sh_editor_redraw(struct sh_editor *edit)
{
    size_t end = edit->length;

    if (edit->cursor < edit->view_start) {
        edit->view_start = edit->cursor;
    } else if (edit->cursor - edit->view_start > edit->view_width) {
        edit->view_start = edit->cursor - edit->view_width / 2U;
        while (edit->view_start < edit->cursor &&
               sh_utf8_tail((unsigned char)edit->line[edit->view_start])) {
            ++edit->view_start;
        }
    }
    if (end - edit->view_start > edit->view_width) {
        end = edit->view_start + edit->view_width;
        while (end > edit->cursor &&
               sh_utf8_tail((unsigned char)edit->line[end])) {
            --end;
        }
    }
    (void)hfoc_write_all(1, "\r", 1U);
    (void)hfoc_write_all(1, edit->prompt, edit->prompt_length);
    (void)hfoc_write_all(1, edit->line + edit->view_start,
                         edit->cursor - edit->view_start);
    if (end > edit->cursor) {
        /* Write the suffix after the prefix so the terminal can combine them. */
        (void)hfoc_write_all(1, "\0337", 2U);
        (void)hfoc_write_all(1, edit->line + edit->cursor, end - edit->cursor);
    }
    (void)hfoc_write_all(1, "\033[K", 3U);
    if (end > edit->cursor) {
        (void)hfoc_write_all(1, "\0338", 2U);
    }
}

static void sh_history_add(const char *line, size_t length)
{
    size_t removed = 0U;
    size_t skip = 0U;

    if (length == 0U) {
        return;
    }
    while (sh_history.count - removed == SH_HISTORY_MAX ||
           sh_history.used - skip + length > sizeof(sh_history.bytes)) {
        skip += sh_history.lengths[removed++];
    }
    if (removed != 0U) {
        sh_history.count -= removed;
        sh_history.used -= skip;
        hfoc_memmove(sh_history.bytes, sh_history.bytes + skip,
                     sh_history.used);
        hfoc_memmove(sh_history.lengths, sh_history.lengths + removed,
                     sh_history.count * sizeof(sh_history.lengths[0]));
    }
    hfoc_memcpy(sh_history.bytes + sh_history.used, line, length);
    sh_history.used += length;
    sh_history.lengths[sh_history.count++] = (uint16_t)length;
}

static void sh_history_move(struct sh_editor *edit, int arrow)
{
    if (arrow == 'A') {
        if (edit->history_offset == sh_history.count) {
            return;
        }
        if (edit->history_offset == 0U) {
            /* The lexer uses sh_line only after the editor returns. */
            hfoc_memcpy(sh_line, edit->line, edit->length);
            edit->draft_length = edit->length;
            edit->draft_cursor = edit->cursor;
        }
        ++edit->history_offset;
    } else {
        if (edit->history_offset == 0U) {
            return;
        }
        --edit->history_offset;
    }

    if (edit->history_offset == 0U) {
        edit->length = edit->draft_length;
        edit->cursor = edit->draft_cursor;
        hfoc_memcpy(edit->line, sh_line, edit->length);
    } else {
        size_t start = sh_history.used;
        const size_t index = sh_history.count - edit->history_offset;

        for (size_t i = sh_history.count; i > index; --i) {
            start -= sh_history.lengths[i - 1U];
        }
        edit->length = sh_history.lengths[index];
        edit->cursor = edit->length;
        hfoc_memcpy(edit->line, sh_history.bytes + start, edit->length);
    }
    edit->view_start = 0U;
    sh_editor_redraw(edit);
}

/* Returns the length, -1 on EOF, -2 on Ctrl-C, -3 on an overlong line. */
static int sh_read_line(char *line, size_t capacity, const char *prompt)
{
    struct sh_editor edit = {
        .line = line, .prompt = prompt, .prompt_length = hfoc_strlen(prompt)
    };
    const int no_pending = -2;
    int pending = no_pending;
    int overflow = 0;

    if (sh_job_control) {
        if (sh_tty_columns <= edit.prompt_length + 1U) {
            edit.prompt_length = 0U;
        }
        edit.view_width = sh_tty_columns - edit.prompt_length - 1U;
        sh_editor_start_row(sh_tty_columns);
    }
    (void)hfoc_write_all(1, edit.prompt, edit.prompt_length);
    for (;;) {
        const int byte = pending == no_pending ? sh_read_input_byte() : pending;

        pending = no_pending;
        if (byte < 0 || byte == '\r' || byte == '\n' ||
            (byte == 0x04 && edit.length == 0U) || byte == 0x03) {
            if (byte == 0x03) {
                (void)hfoc_write_all(1, "^C\r\n", 4U);
                return -2;
            }
            (void)hfoc_write_all(1, "\r\n", 2U);
            if (byte < 0 || byte == 0x04) {
                return -1;
            }
            if (overflow) {
                return -3;
            }
            line[edit.length] = '\0';
            if (sh_job_control) {
                sh_history_add(line, edit.length);
            }
            return (int)edit.length;
        }
        if (byte == 0x1b && sh_job_control) {
            const int arrow = sh_read_escape(&pending);

            if (arrow == 'A' || arrow == 'B') {
                sh_history_move(&edit, arrow);
            } else if (arrow == 'D' && edit.cursor != 0U) {
                edit.cursor = sh_utf8_previous(line, edit.cursor);
                sh_editor_redraw(&edit);
            } else if (arrow == 'C' && edit.cursor < edit.length) {
                edit.cursor += sh_utf8_span(line + edit.cursor,
                                           edit.length - edit.cursor);
                sh_editor_redraw(&edit);
            }
            continue;
        }
        if (byte == 0x7f || byte == 0x08) {
            if (edit.cursor != 0U) {
                const size_t start = sh_utf8_previous(line, edit.cursor);

                hfoc_memmove(line + start, line + edit.cursor,
                             edit.length - edit.cursor);
                edit.length -= edit.cursor - start;
                edit.cursor = start;
                if (sh_job_control) {
                    sh_editor_redraw(&edit);
                } else {
                    (void)hfoc_write_all(1, "\b \b", 3U);
                }
            }
            continue;
        }
        if (byte < 0x20) {
            continue;
        }
        {
            unsigned char text[4] = { (unsigned char)byte };
            const size_t width = sh_utf8_width(text[0]);
            size_t used = 1U;

            /* Don't interleave a redraw with a partial UTF-8 character. */
            while (used < width) {
                const int next = sh_read_input_byte();

                if (!sh_utf8_tail(next)) {
                    pending = next;
                    break;
                }
                text[used++] = (unsigned char)next;
            }
            if (used >= capacity - edit.length) {
                overflow = 1;
                (void)hfoc_write_all(1, "\a", 1U);
                continue;
            }
            hfoc_memmove(line + edit.cursor + used, line + edit.cursor,
                         edit.length - edit.cursor);
            hfoc_memcpy(line + edit.cursor, text, used);
            edit.cursor += used;
            edit.length += used;
            if (!sh_job_control || (edit.cursor == edit.length &&
                                    edit.length - edit.view_start <= edit.view_width)) {
                (void)hfoc_write_all(1, text, used);
            } else {
                sh_editor_redraw(&edit);
            }
        }
    }
}

static int sh_run_interactive(void)
{
    char line[SH_LINE_MAX];
    int status = 0;

    sh_interactive = 1;
    sh_script_len = 0U;
    for (;;) {
        const char *prompt = sh_script_len == 0U ? "hfoc$ " : "> ";

        sh_job_reap_nohang();
        const int read_status = sh_read_line(line, sizeof(line), prompt);

        if (read_status == -1) {
            break;
        }
        if (read_status == -2) {
            sh_script_len = 0U;
            status = 130;
            sh_last_status = status;
            continue;
        }
        if (read_status == -3) {
            sh_error("line exceeds 4096 bytes");
            sh_script_len = 0U;
            status = 2;
            sh_last_status = status;
            continue;
        }
        {
            const size_t length = (size_t)read_status;

            if (sh_script_len + length + 2U > sizeof(sh_script)) {
                sh_error("input exceeds 10240 bytes");
                sh_script_len = 0U;
                status = 2;
                sh_last_status = status;
                continue;
            }
            hfoc_memcpy(sh_script + sh_script_len, line, length);
            sh_script_len += length;
            sh_script[sh_script_len++] = '\n';
        }
        status = sh_run_buffer(0);
        if (status == -2) {
            continue; /* keep reading the same compound command */
        }
        sh_script_len = 0U;
        sh_last_status = status;
        if (sh_exit_requested) {
            status = sh_exit_status;
            break;
        }
    }
    sh_jobs_cleanup();
    sh_run_exit_trap();
    return status;
}

static int sh_run_string(const char *text)
{
    const size_t length = hfoc_strlen(text);
    int status;

    if (length + 1U > sizeof(sh_script)) {
        sh_error("command exceeds 10240 bytes");
        return 2;
    }
    hfoc_memcpy(sh_script, text, length);
    sh_script_len = length;
    sh_interactive = 0;
    status = sh_run_buffer(1);
    sh_jobs_cleanup();
    return status == -2 ? 2 : status;
}

int main(int argc, char **argv)
{
    char cwd[SH_PATH_MAX];

    (void)argc;
    (void)argv;
    sh_program_name = hfoc_argc > 0 ? hfoc_argv[0] : "sh";
    /* A real TTY enables job control; the console path stays as before. */
    hfoc_strncpy(sh_pos0, sh_program_name, sizeof(sh_pos0) - 1U);
    sh_pos0[sizeof(sh_pos0) - 1U] = '\0';
    sh_vars_from_env();
    sh_job_control = hfoc_tty_foreground(0U) == 0L;
    if (sh_var_find("HFOC_JOB_CONTROL") != NULL &&
        sh_streq(sh_var_find("HFOC_JOB_CONTROL"), "0")) {
        sh_job_control = 0;
    }
    if (hfoc_getcwd(cwd, sizeof(cwd)) > 0) {
        sh_var_set("PWD", cwd);
    }
    if (hfoc_argc < 2) {
        return sh_run_interactive();
    }
    if (sh_streq(hfoc_argv[1], "-c")) {
        if (hfoc_argc < 3) {
            sh_error("-c needs a command string");
            return 2;
        }
        if (hfoc_argc > 3) {
            hfoc_strncpy(sh_pos0, hfoc_argv[3], sizeof(sh_pos0) - 1U);
            sh_pos0[sizeof(sh_pos0) - 1U] = '\0';
            sh_set_positional(&hfoc_argv[4],
                              (uint32_t)(hfoc_argc - 4));
        }
        return sh_run_string(hfoc_argv[2]);
    }
    hfoc_strncpy(sh_pos0, hfoc_argv[1], sizeof(sh_pos0) - 1U);
    sh_pos0[sizeof(sh_pos0) - 1U] = '\0';
    if (hfoc_argc > 2) {
        sh_set_positional(&hfoc_argv[2], (uint32_t)(hfoc_argc - 2));
    }
    return sh_run_script(hfoc_argv[1]);
}
