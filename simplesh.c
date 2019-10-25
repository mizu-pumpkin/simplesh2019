/*
 * Shell `simplesh` (basado en el shell de xv6)
 *
 * Ampliación de Sistemas Operativos
 * Departamento de Ingeniería y Tecnología de Computadores
 * Facultad de Informática de la Universidad de Murcia
 *
 * Alumnos: MARTÍNEZ MARTÍNEZ, EDUARDO (G2.1)
 *          ZUCCA, MARISOL ISABEL (G2.1)
 *
 * Convocatoria: FEBRERO
 */


/*
 * Ficheros de cabecera
 */


#define _POSIX_C_SOURCE 200809L /* IEEE 1003.1-2008 (véase /usr/include/features.h) */
//#define NDEBUG                /* Traduce asertos y DMACROS a 'no ops' */

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <libgen.h>     //para basename()
#include <limits.h>     //para PATH_MAX
#include <pwd.h>        //para getpwuid()
#include <signal.h>     //para señales
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

// Biblioteca readline
#include <readline/readline.h>
#include <readline/history.h>


/******************************************************************************
 * Constantes, macros y variables globales
 ******************************************************************************/


#define NUMERO_INTERNOS 4
#define DEFAULT_BSIZE 1024
#define DEFAULT_PROCS 1
#define MAX_BSIZE 1048576


static const char* VERSION = "0.19";

// Niveles de depuración
#define DBG_CMD   (1 << 0)
#define DBG_TRACE (1 << 1)
// . . .
static int g_dbg_level = 0;

#ifndef NDEBUG
#define DPRINTF(dbg_level, fmt, ...)                            \
    do {                                                        \
        if (dbg_level & g_dbg_level)                            \
            fprintf(stderr, "%s:%d:%s(): " fmt,                 \
                    __FILE__, __LINE__, __func__, ##__VA_ARGS__);       \
    } while ( 0 )

#define DBLOCK(dbg_level, block)                                \
    do {                                                        \
        if (dbg_level & g_dbg_level)                            \
            block;                                              \
    } while( 0 );
#else
#define DPRINTF(dbg_level, fmt, ...)
#define DBLOCK(dbg_level, block)
#endif

#define TRY(x)                                                  \
    do {                                                        \
        int __rc = (x);                                         \
        if( __rc < 0 ) {                                        \
            fprintf(stderr, "%s:%d:%s: TRY(%s) failed\n",       \
                    __FILE__, __LINE__, __func__, #x);          \
            fprintf(stderr, "ERROR: rc=%d errno=%d (%s)\n",     \
                    __rc, errno, strerror(errno));              \
            exit(EXIT_FAILURE);                                 \
        }                                                       \
    } while( 0 )


// Número máximo de argumentos de un comando
#define MAX_ARGS 16


// Delimitadores
static const char WHITESPACE[] = " \t\r\n\v";
// Caracteres especiales
static const char SYMBOLS[] = "<|>&;()";


// Comandos internos
#define N_INTERNALCMD 5
char * comandos_internos[N_INTERNALCMD] = {"cwd", "exit", "cd", "psplit", "bjobs"};

// Array de procesos en background
#define MAX_BPROC 8
pid_t bpids[MAX_BPROC];
int bpids_i;

// Código de retorno de exit
#define EXIT 1


/******************************************************************************
 * Funciones auxiliares
 ******************************************************************************/

// Añade el pid al array de pids de procesos en background activos
void add_to_bpids(pid_t pid)
{
    if (bpids_i < MAX_BPROC) {
        bpids[bpids_i] = pid;
        bpids_i++;
    }
}

// Elimina el pid del array de pids de procesos en background activos
void remove_from_bpids(pid_t pid)
{
    int i=0;
    while (i<bpids_i && bpids[i]!=pid) i++;
    while ((i+1)<bpids_i) {
        bpids[i] = bpids[i+1];
        i++;
    }
    if (i < MAX_BPROC) {
        bpids[i] = -1;
        bpids_i--;
    }
}

// Bloquea o desbloquea la señal SIGCHLD
// Valores para how: SIG_BLOCK o SIG_UNBLOCK
void sigchld_inhibitor(int how)
{    
    sigset_t sigchldset;
    
    TRY (sigemptyset(&sigchldset));
    TRY (sigaddset(&sigchldset, SIGCHLD));
    TRY (sigprocmask(how, &sigchldset, NULL));
}


int min(int a, int b)
{
    if (a < b) return a;
    else return b;
}


// Imprime el mensaje
void info(const char *fmt, ...)
{
    va_list arg;

    fprintf(stdout, "%s: ", __FILE__);
    va_start(arg, fmt);
    vfprintf(stdout, fmt, arg);
    va_end(arg);
}


// Imprime el mensaje de error
void error(const char *fmt, ...)
{
    va_list arg;

    fprintf(stderr, "%s: ", __FILE__);
    va_start(arg, fmt);
    vfprintf(stderr, fmt, arg);
    va_end(arg);
}


// Imprime el mensaje de error y aborta la ejecución
void panic(const char *fmt, ...)
{
    va_list arg;

    fprintf(stderr, "%s: ", __FILE__);
    va_start(arg, fmt);
    vfprintf(stderr, fmt, arg);
    va_end(arg);

    exit(EXIT_FAILURE);
}


// `fork()` que muestra un mensaje de error si no se puede crear el hijo
int fork_or_panic(const char* s)
{
    int pid;

    pid = fork();
    if(pid == -1)
        panic("%s failed: errno %d (%s)", s, errno, strerror(errno));
    return pid;
}


/******************************************************************************
 * Estructuras de datos `cmd`
 ******************************************************************************/


// Las estructuras `cmd` se utilizan para almacenar información que servirá a
// simplesh para ejecutar líneas de órdenes con redirecciones, tuberías, listas
// de comandos y tareas en segundo plano. El formato es el siguiente:

//     |----------+--------------+--------------|
//     | (1 byte) | ...          | ...          |
//     |----------+--------------+--------------|
//     | type     | otros campos | otros campos |
//     |----------+--------------+--------------|

// Nótese cómo las estructuras `cmd` comparten el primer campo `type` para
// identificar su tipo. A partir de él se obtiene un tipo derivado a través de
// *casting* forzado de tipo. Se consigue así polimorfismo básico en C.

// Valores del campo `type` de las estructuras de datos `cmd`
enum cmd_type { EXEC=1, REDR=2, PIPE=3, LIST=4, BACK=5, SUBS=6, INV=7 };

struct cmd { enum cmd_type type; };

// Comando con sus parámetros
struct execcmd {
    enum cmd_type type;
    char* argv[MAX_ARGS];
    char* eargv[MAX_ARGS];
    int argc;
};

// Comando con redirección
struct redrcmd {
    enum cmd_type type;
    struct cmd* cmd;
    char* file;
    char* efile;
    int flags;
    mode_t mode;
    int fd;
};

// Comandos con tubería
struct pipecmd {
    enum cmd_type type;
    struct cmd* left;
    struct cmd* right;
};

// Lista de órdenes
struct listcmd {
    enum cmd_type type;
    struct cmd* left;
    struct cmd* right;
};

// Tarea en segundo plano (background) con `&`
struct backcmd {
    enum cmd_type type;
    struct cmd* cmd;
};

// Subshell
struct subscmd {
    enum cmd_type type;
    struct cmd* cmd;
};


/******************************************************************************
 * Funciones para construir las estructuras de datos `cmd`
 ******************************************************************************/


// Construye una estructura `cmd` de tipo `EXEC`
struct cmd* execcmd(void)
{
    struct execcmd* cmd;

    if ((cmd = malloc(sizeof(*cmd))) == NULL)
    {
        perror("execcmd: malloc");
        exit(EXIT_FAILURE);
    }
    memset(cmd, 0, sizeof(*cmd));
    cmd->type = EXEC;

    return (struct cmd*) cmd;
}

// Construye una estructura `cmd` de tipo `REDR`
struct cmd* redrcmd(struct cmd* subcmd,
        char* file, char* efile,
        int flags, mode_t mode, int fd)
{
    struct redrcmd* cmd;

    if ((cmd = malloc(sizeof(*cmd))) == NULL)
    {
        perror("redrcmd: malloc");
        exit(EXIT_FAILURE);
    }
    memset(cmd, 0, sizeof(*cmd));
    cmd->type = REDR;
    cmd->cmd = subcmd;
    cmd->file = file;
    cmd->efile = efile;
    cmd->flags = flags;
    cmd->mode = mode;
    cmd->fd = fd;

    return (struct cmd*) cmd;
}

// Construye una estructura `cmd` de tipo `PIPE`
struct cmd* pipecmd(struct cmd* left, struct cmd* right)
{
    struct pipecmd* cmd;

    if ((cmd = malloc(sizeof(*cmd))) == NULL)
    {
        perror("pipecmd: malloc");
        exit(EXIT_FAILURE);
    }
    memset(cmd, 0, sizeof(*cmd));
    cmd->type = PIPE;
    cmd->left = left;
    cmd->right = right;

    return (struct cmd*) cmd;
}

// Construye una estructura `cmd` de tipo `LIST`
struct cmd* listcmd(struct cmd* left, struct cmd* right)
{
    struct listcmd* cmd;

    if ((cmd = malloc(sizeof(*cmd))) == NULL)
    {
        perror("listcmd: malloc");
        exit(EXIT_FAILURE);
    }
    memset(cmd, 0, sizeof(*cmd));
    cmd->type = LIST;
    cmd->left = left;
    cmd->right = right;

    return (struct cmd*)cmd;
}

// Construye una estructura `cmd` de tipo `BACK`
struct cmd* backcmd(struct cmd* subcmd)
{
    struct backcmd* cmd;

    if ((cmd = malloc(sizeof(*cmd))) == NULL)
    {
        perror("backcmd: malloc");
        exit(EXIT_FAILURE);
    }
    memset(cmd, 0, sizeof(*cmd));
    cmd->type = BACK;
    cmd->cmd = subcmd;

    return (struct cmd*)cmd;
}

// Construye una estructura `cmd` de tipo `SUB`
struct cmd* subscmd(struct cmd* subcmd)
{
    struct subscmd* cmd;

    if ((cmd = malloc(sizeof(*cmd))) == NULL)
    {
        perror("subscmd: malloc");
        exit(EXIT_FAILURE);
    }
    memset(cmd, 0, sizeof(*cmd));
    cmd->type = SUBS;
    cmd->cmd = subcmd;

    return (struct cmd*) cmd;
}


/******************************************************************************
 * Funciones para realizar el análisis sintáctico de la línea de órdenes
 ******************************************************************************/


// `get_token` recibe un puntero al principio de una cadena (`start_of_str`),
// otro puntero al final de esa cadena (`end_of_str`) y, opcionalmente, dos
// punteros para guardar el principio y el final del token, respectivamente.
//
// `get_token` devuelve un *token* de la cadena de entrada.

int get_token(char** start_of_str, char const* end_of_str,
        char** start_of_token, char** end_of_token)
{
    char* s;
    int ret;

    // Salta los espacios en blanco
    s = *start_of_str;
    while (s < end_of_str && strchr(WHITESPACE, *s))
        s++;

    // `start_of_token` apunta al principio del argumento (si no es NULL)
    if (start_of_token)
        *start_of_token = s;

    ret = *s;
    switch (*s)
    {
        case 0:
            break;
        case '|':
        case '(':
        case ')':
        case ';':
        case '&':
        case '<':
            s++;
            break;
        case '>':
            s++;
            if (*s == '>')
            {
                ret = '+';
                s++;
            }
            break;

        default:

            // El caso por defecto (cuando no hay caracteres especiales) es el
            // de un argumento de un comando. `get_token` devuelve el valor
            // `'a'`, `start_of_token` apunta al argumento (si no es `NULL`),
            // `end_of_token` apunta al final del argumento (si no es `NULL`) y
            // `start_of_str` avanza hasta que salta todos los espacios
            // *después* del argumento. Por ejemplo:
            //
            //     |-----------+---+---+---+---+---+---+---+---+---+-----------|
            //     | (espacio) | a | r | g | u | m | e | n | t | o | (espacio)
            //     |
            //     |-----------+---+---+---+---+---+---+---+---+---+-----------|
            //                   ^                                   ^
            //            start_o|f_token                       end_o|f_token

            ret = 'a';
            while (s < end_of_str &&
                    !strchr(WHITESPACE, *s) &&
                    !strchr(SYMBOLS, *s))
                s++;
            break;
    }

    // `end_of_token` apunta al final del argumento (si no es `NULL`)
    if (end_of_token)
        *end_of_token = s;

    // Salta los espacios en blanco
    while (s < end_of_str && strchr(WHITESPACE, *s))
        s++;

    // Actualiza `start_of_str`
    *start_of_str = s;

    return ret;
}


// `peek` recibe un puntero al principio de una cadena (`start_of_str`), otro
// puntero al final de esa cadena (`end_of_str`) y un conjunto de caracteres
// (`delimiter`).
//
// El primer puntero pasado como parámero (`start_of_str`) avanza hasta el
// primer carácter que no está en el conjunto de caracteres `WHITESPACE`.
//
// `peek` devuelve un valor distinto de `NULL` si encuentra alguno de los
// caracteres en `delimiter` justo después de los caracteres en `WHITESPACE`.

int peek(char** start_of_str, char const* end_of_str, char* delimiter)
{
    char* s;

    s = *start_of_str;
    while (s < end_of_str && strchr(WHITESPACE, *s))
        s++;
    *start_of_str = s;

    return *s && strchr(delimiter, *s);
}


// Definiciones adelantadas de funciones
struct cmd* parse_line(char**, char*);
struct cmd* parse_pipe(char**, char*);
struct cmd* parse_exec(char**, char*);
struct cmd* parse_subs(char**, char*);
struct cmd* parse_redr(struct cmd*, char**, char*);
struct cmd* null_terminate(struct cmd*);

int check_internal(struct cmd * cmd);
void exec_internal(struct cmd * cmd, int command);
void register_handler(void);


// `parse_cmd` realiza el *análisis sintáctico* de la línea de órdenes
// introducida por el usuario.
//
// `parse_cmd` utiliza `parse_line` para obtener una estructura `cmd`.

struct cmd* parse_cmd(char* start_of_str)
{
    char* end_of_str;
    struct cmd* cmd;

    DPRINTF(DBG_TRACE, "STR\n");

    end_of_str = start_of_str + strlen(start_of_str);

    cmd = parse_line(&start_of_str, end_of_str);

    // Comprueba que se ha alcanzado el final de la línea de órdenes
    peek(&start_of_str, end_of_str, "");
    if (start_of_str != end_of_str)
        error("%s: error sintáctico: %s\n", __func__);

    DPRINTF(DBG_TRACE, "END\n");

    return cmd;
}


// `parse_line` realiza el análisis sintáctico de la línea de órdenes
// introducida por el usuario.
//
// `parse_line` comprueba en primer lugar si la línea contiene alguna tubería.
// Para ello `parse_line` llama a `parse_pipe` que a su vez verifica si hay
// bloques de órdenes y/o redirecciones.  A continuación, `parse_line`
// comprueba si la ejecución de la línea se realiza en segundo plano (con `&`)
// o si la línea de órdenes contiene una lista de órdenes (con `;`).

struct cmd* parse_line(char** start_of_str, char* end_of_str)
{
    struct cmd* cmd;
    int delimiter;

    cmd = parse_pipe(start_of_str, end_of_str);

    while (peek(start_of_str, end_of_str, "&"))
    {
        // Consume el delimitador de tarea en segundo plano
        delimiter = get_token(start_of_str, end_of_str, 0, 0);
        assert(delimiter == '&');

        // Construye el `cmd` para la tarea en segundo plano
        cmd = backcmd(cmd);
    }

    if (peek(start_of_str, end_of_str, ";"))
    {
        if (cmd->type == EXEC && ((struct execcmd*) cmd)->argv[0] == 0)
            error("%s: error sintáctico: no se encontró comando\n", __func__);

        // Consume el delimitador de lista de órdenes
        delimiter = get_token(start_of_str, end_of_str, 0, 0);
        assert(delimiter == ';');

        // Construye el `cmd` para la lista
        cmd = listcmd(cmd, parse_line(start_of_str, end_of_str));
    }

    return cmd;
}


// `parse_pipe` realiza el análisis sintáctico de una tubería de manera
// recursiva si encuentra el delimitador de tuberías '|'.
//
// `parse_pipe` llama a `parse_exec` y `parse_pipe` de manera recursiva para
// realizar el análisis sintáctico de todos los componentes de la tubería.

struct cmd* parse_pipe(char** start_of_str, char* end_of_str)
{
    struct cmd* cmd;
    int delimiter;

    cmd = parse_exec(start_of_str, end_of_str);

    if (peek(start_of_str, end_of_str, "|"))
    {
        if (cmd->type == EXEC && ((struct execcmd*) cmd)->argv[0] == 0)
            error("%s: error sintáctico: no se encontró comando\n", __func__);

        // Consume el delimitador de tubería
        delimiter = get_token(start_of_str, end_of_str, 0, 0);
        assert(delimiter == '|');

        // Construye el `cmd` para la tubería
        cmd = pipecmd(cmd, parse_pipe(start_of_str, end_of_str));
    }

    return cmd;
}


// `parse_exec` realiza el análisis sintáctico de un comando a no ser que la
// expresión comience por un paréntesis, en cuyo caso se llama a `parse_subs`.
//
// `parse_exec` reconoce las redirecciones antes y después del comando.

struct cmd* parse_exec(char** start_of_str, char* end_of_str)
{
    char* start_of_token;
    char* end_of_token;
    int token, argc;
    struct execcmd* cmd;
    struct cmd* ret;

    // ¿Inicio de un bloque?
    if (peek(start_of_str, end_of_str, "("))
        return parse_subs(start_of_str, end_of_str);

    // Si no, lo primero que hay en una línea de órdenes es un comando

    // Construye el `cmd` para el comando
    ret = execcmd();
    cmd = (struct execcmd*) ret;

    // ¿Redirecciones antes del comando?
    ret = parse_redr(ret, start_of_str, end_of_str);

    // Bucle para separar los argumentos de las posibles redirecciones
    argc = 0;
    while (!peek(start_of_str, end_of_str, "|)&;"))
    {
        if ((token = get_token(start_of_str, end_of_str,
                        &start_of_token, &end_of_token)) == 0)
            break;

        // El siguiente token debe ser un argumento porque el bucle
        // para en los delimitadores
        if (token != 'a')
            error("%s: error sintáctico: se esperaba un argumento\n", __func__);

        // Almacena el siguiente argumento reconocido. El primero es
        // el comando
        cmd->argv[argc] = start_of_token;
        cmd->eargv[argc] = end_of_token;
        cmd->argc = ++argc;
        if (argc >= MAX_ARGS)
            panic("%s: demasiados argumentos\n", __func__);

        // ¿Redirecciones después del comando?
        ret = parse_redr(ret, start_of_str, end_of_str);
    }

    // El comando no tiene más parámetros
    cmd->argv[argc] = 0;
    cmd->eargv[argc] = 0;

    return ret;
}


// `parse_subs` realiza el análisis sintáctico de un bloque de órdenes
// delimitadas por paréntesis o `subshell` llamando a `parse_line`.
//
// `parse_subs` reconoce las redirecciones después del bloque de órdenes.

struct cmd* parse_subs(char** start_of_str, char* end_of_str)
{
    int delimiter;
    struct cmd* cmd;
    struct cmd* scmd;

    // Consume el paréntesis de apertura
    if (!peek(start_of_str, end_of_str, "("))
        error("%s: error sintáctico: se esperaba '('", __func__);
    delimiter = get_token(start_of_str, end_of_str, 0, 0);
    assert(delimiter == '(');

    // Realiza el análisis sintáctico hasta el paréntesis de cierre
    scmd = parse_line(start_of_str, end_of_str);

    // Construye el `cmd` para el bloque de órdenes
    cmd = subscmd(scmd);

    // Consume el paréntesis de cierre
    if (!peek(start_of_str, end_of_str, ")"))
        error("%s: error sintáctico: se esperaba ')'", __func__);
    delimiter = get_token(start_of_str, end_of_str, 0, 0);
    assert(delimiter == ')');

    // ¿Redirecciones después del bloque de órdenes?
    cmd = parse_redr(cmd, start_of_str, end_of_str);

    return cmd;
}


// `parse_redr` realiza el análisis sintáctico de órdenes con
// redirecciones si encuentra alguno de los delimitadores de
// redirección ('<' o '>').

struct cmd* parse_redr(struct cmd* cmd, char** start_of_str, char* end_of_str)
{
    int delimiter;
    char* start_of_token;
    char* end_of_token;

    // Si lo siguiente que hay a continuación es delimitador de
    // redirección...
    while (peek(start_of_str, end_of_str, "<>"))
    {
        // Consume el delimitador de redirección
        delimiter = get_token(start_of_str, end_of_str, 0, 0);
        assert(delimiter == '<' || delimiter == '>' || delimiter == '+');

        // El siguiente token tiene que ser el nombre del fichero de la
        // redirección entre `start_of_token` y `end_of_token`.
        if ('a' != get_token(start_of_str, end_of_str, &start_of_token, &end_of_token))
            error("%s: error sintáctico: se esperaba un fichero", __func__);

        // Construye el `cmd` para la redirección
        switch(delimiter)
        {
            case '<':
                cmd = redrcmd(cmd, start_of_token, end_of_token, O_RDONLY, S_IRWXU, STDIN_FILENO);
                break;
            case '>':
                cmd = redrcmd(cmd, start_of_token, end_of_token, O_WRONLY|O_CREAT|O_TRUNC, S_IRWXU, STDOUT_FILENO);
                break;
            case '+': // >>
                cmd = redrcmd(cmd, start_of_token, end_of_token, O_WRONLY|O_CREAT|O_APPEND, S_IRWXU, STDOUT_FILENO);
                break;
        }
    }

    return cmd;
}


// Termina en NULL todas las cadenas de las estructuras `cmd`
struct cmd* null_terminate(struct cmd* cmd)
{
    struct execcmd* ecmd;
    struct redrcmd* rcmd;
    struct pipecmd* pcmd;
    struct listcmd* lcmd;
    struct backcmd* bcmd;
    struct subscmd* scmd;
    int i;

    if(cmd == 0)
        return 0;

    switch(cmd->type)
    {
        case EXEC:
            ecmd = (struct execcmd*) cmd;
            for(i = 0; ecmd->argv[i]; i++)
                *ecmd->eargv[i] = 0;
            break;

        case REDR:
            rcmd = (struct redrcmd*) cmd;
            null_terminate(rcmd->cmd);
            *rcmd->efile = 0;
            break;

        case PIPE:
            pcmd = (struct pipecmd*) cmd;
            null_terminate(pcmd->left);
            null_terminate(pcmd->right);
            break;

        case LIST:
            lcmd = (struct listcmd*) cmd;
            null_terminate(lcmd->left);
            null_terminate(lcmd->right);
            break;

        case BACK:
            bcmd = (struct backcmd*) cmd;
            null_terminate(bcmd->cmd);
            break;

        case SUBS:
            scmd = (struct subscmd*) cmd;
            null_terminate(scmd->cmd);
            break;

        case INV:
        default:
            panic("%s: estructura `cmd` desconocida\n", __func__);
    }

    return cmd;
}


/******************************************************************************
 * Funciones para la ejecución de la línea de órdenes
 ******************************************************************************/


void exec_cmd(struct execcmd* ecmd)
{
    assert(ecmd->type == EXEC);

    if (ecmd->argv[0] == NULL) exit(EXIT_SUCCESS);

    execvp(ecmd->argv[0], ecmd->argv);

    panic("no se encontró el comando '%s'\n", ecmd->argv[0]);
}


void run_cmd(struct cmd* cmd)
{
    struct execcmd* ecmd;
    struct redrcmd* rcmd;
    struct listcmd* lcmd;
    struct pipecmd* pcmd;
    struct backcmd* bcmd;
    struct subscmd* scmd;
    int p[2];
    int fd, old_fd;
    int interno = -1;
    pid_t pidhijo, pidhijo2;

    DPRINTF(DBG_TRACE, "STR\n");

    if(cmd == 0) return;

    switch(cmd->type)
    {
        case EXEC:
            ecmd = (struct execcmd*) cmd;
            interno = check_internal(cmd);
            if (interno != -1)
                exec_internal(cmd, interno);
            else
            {
                sigchld_inhibitor(SIG_BLOCK);
                if ((pidhijo = fork_or_panic("fork EXEC")) == 0)
                    exec_cmd(ecmd);
                TRY( waitpid(pidhijo,0,0) );
                sigchld_inhibitor(SIG_UNBLOCK);
            }
            break;

        case REDR:
            rcmd = (struct redrcmd*) cmd;
            interno = check_internal(rcmd->cmd);
            if (interno != -1)
            {
                TRY ( old_fd = dup(rcmd->fd) );
                TRY( close(rcmd->fd) );
                if ((fd = open(rcmd->file, rcmd->flags, rcmd->mode)) < 0)
                {
                    perror("run_cmd: open");
                    exit(EXIT_FAILURE);
                }
                exec_internal(rcmd->cmd, interno);
                TRY( dup2(old_fd, fd) );
                TRY( close(old_fd) );
            }
            else
            {
                sigchld_inhibitor(SIG_BLOCK);
                if ((pidhijo = fork_or_panic("fork REDR")) == 0)
                {
                    TRY( close(rcmd->fd) );
                    if ((fd = open(rcmd->file, rcmd->flags, rcmd->mode)) < 0)
                    {
                        perror("run_cmd: open");
                        exit(EXIT_FAILURE);
                    }
                    if (rcmd->cmd->type == EXEC)
                        exec_cmd((struct execcmd*) rcmd->cmd);
                    else
                        run_cmd(rcmd->cmd);
                    exit(EXIT_SUCCESS);
                }
                TRY( waitpid(pidhijo,0,0) );
                sigchld_inhibitor(SIG_UNBLOCK);
            }
            break;

        case LIST:
            lcmd = (struct listcmd*) cmd;
            //TODO
            run_cmd(lcmd->left);
            run_cmd(lcmd->right);
            break;

        case PIPE:
            pcmd = (struct pipecmd*)cmd;
            if (pipe(p) < 0)
            {
                perror("run_cmd: pipe");
                exit(EXIT_FAILURE);
            }

            sigchld_inhibitor(SIG_BLOCK);
            // Ejecución del hijo de la izquierda
            if ((pidhijo = fork_or_panic("fork PIPE left")) == 0)
            {
                TRY( close(STDOUT_FILENO) );
                TRY( dup(p[1]) );
                TRY( close(p[0]) );
                TRY( close(p[1]) );
                if (pcmd->left->type == EXEC) {
                    interno = check_internal(pcmd->left);
                    if (interno != -1)
                        exec_internal(pcmd->left, interno);
                    else
                        exec_cmd((struct execcmd*) pcmd->left);
                } else
                    run_cmd(pcmd->left);
                exit(EXIT_SUCCESS);
            }

            // Ejecución del hijo de la derecha
            if ((pidhijo2 = fork_or_panic("fork PIPE right")) == 0)
            {
                TRY( close(STDIN_FILENO) );
                TRY( dup(p[0]) );
                TRY( close(p[0]) );
                TRY( close(p[1]) );
                if (pcmd->right->type == EXEC) {
                    interno = check_internal(pcmd->right);
                    if (interno != -1)
                        exec_internal(pcmd->right, interno);
                    else
                        exec_cmd((struct execcmd*) pcmd->right);
                } else
                    run_cmd(pcmd->right);
                exit(EXIT_SUCCESS);
            }
            TRY( close(p[0]) );
            TRY( close(p[1]) );

            // Esperar a ambos hijos
            TRY( waitpid(pidhijo,0,0) );
            TRY( waitpid(pidhijo2,0,0) );
            sigchld_inhibitor(SIG_UNBLOCK);
            break;

        case BACK:
            bcmd = (struct backcmd*)cmd;
            if ((pidhijo = fork_or_panic("fork BACK")) == 0)
            {
                if (bcmd->cmd->type == EXEC) {
                    interno = check_internal(bcmd->cmd);
                    if (interno != -1)
                        exec_internal(bcmd->cmd, interno);
                    else {
                        exec_cmd((struct execcmd*) bcmd->cmd);
                    }
                } else
                    run_cmd(bcmd->cmd);
                exit(EXIT_SUCCESS);
            }
            // Envía [PID] a stdout y lo añade a los pid de procesos en segundo plano activos
            printf("[%d]\n",pidhijo);
            sigchld_inhibitor(SIG_BLOCK);
            add_to_bpids(pidhijo);
            sigchld_inhibitor(SIG_UNBLOCK);
            break;

        case SUBS:
            scmd = (struct subscmd*) cmd;
            sigchld_inhibitor(SIG_BLOCK);
            if ((pidhijo = fork_or_panic("fork SUBS")) == 0)
            {
                run_cmd(scmd->cmd);
                exit(EXIT_SUCCESS);
            }
            TRY( waitpid(pidhijo,0,0) );
            sigchld_inhibitor(SIG_UNBLOCK);
            break;

        case INV:
        default:
            panic("%s: estructura `cmd` desconocida\n", __func__);
    }

    DPRINTF(DBG_TRACE, "END\n");
}


void print_cmd(struct cmd* cmd)
{
    struct execcmd* ecmd;
    struct redrcmd* rcmd;
    struct listcmd* lcmd;
    struct pipecmd* pcmd;
    struct backcmd* bcmd;
    struct subscmd* scmd;

    if(cmd == 0) return;

    switch(cmd->type)
    {
        case EXEC:
            ecmd = (struct execcmd*) cmd;
            if (ecmd->argv[0] != 0)
                printf("fork( exec( %s ) )", ecmd->argv[0]);
            break;

        case REDR:
            rcmd = (struct redrcmd*) cmd;
            printf("fork( ");
            if (rcmd->cmd->type == EXEC)
                printf("exec ( %s )", ((struct execcmd*) rcmd->cmd)->argv[0]);
            else
                print_cmd(rcmd->cmd);
            printf(" )");
            break;

        case LIST:
            lcmd = (struct listcmd*) cmd;
            print_cmd(lcmd->left);
            printf(" ; ");
            print_cmd(lcmd->right);
            break;

        case PIPE:
            pcmd = (struct pipecmd*) cmd;
            printf("fork( ");
            if (pcmd->left->type == EXEC)
                printf("exec ( %s )", ((struct execcmd*) pcmd->left)->argv[0]);
            else
                print_cmd(pcmd->left);
            printf(" ) => fork( ");
            if (pcmd->right->type == EXEC)
                printf("exec ( %s )", ((struct execcmd*) pcmd->right)->argv[0]);
            else
                print_cmd(pcmd->right);
            printf(" )");
            break;

        case BACK:
            bcmd = (struct backcmd*) cmd;
            printf("fork( ");
            if (bcmd->cmd->type == EXEC)
                printf("exec ( %s )", ((struct execcmd*) bcmd->cmd)->argv[0]);
            else
                print_cmd(bcmd->cmd);
            printf(" )");
            break;

        case SUBS:
            scmd = (struct subscmd*) cmd;
            printf("fork( ");
            print_cmd(scmd->cmd);
            printf(" )");
            break;

        case INV:
        default:
            panic("%s: estructura `cmd` desconocida\n", __func__);
    }
}


void free_cmd(struct cmd* cmd)
{
    struct execcmd* ecmd;
    struct redrcmd* rcmd;
    struct listcmd* lcmd;
    struct pipecmd* pcmd;
    struct backcmd* bcmd;
    struct subscmd* scmd;

    if(cmd == 0) return;

    switch(cmd->type)
    {
        case EXEC:
            break;

        case REDR:
            rcmd = (struct redrcmd*) cmd;
            free_cmd(rcmd->cmd);

            free(rcmd->cmd);
            break;

        case LIST:
            lcmd = (struct listcmd*) cmd;

            free_cmd(lcmd->left);
            free_cmd(lcmd->right);

            free(lcmd->right);
            free(lcmd->left);
            break;

        case PIPE:
            pcmd = (struct pipecmd*) cmd;

            free_cmd(pcmd->left);
            free_cmd(pcmd->right);

            free(pcmd->right);
            free(pcmd->left);
            break;

        case BACK:
            bcmd = (struct backcmd*) cmd;

            free_cmd(bcmd->cmd);

            free(bcmd->cmd);
            break;

        case SUBS:
            scmd = (struct subscmd*) cmd;

            free_cmd(scmd->cmd);

            free(scmd->cmd);
            break;

        case INV:
        default:
            panic("%s: estructura `cmd` desconocida\n", __func__);
    }
}


/******************************************************************************
 * Lectura de la línea de órdenes con la biblioteca libreadline
 ******************************************************************************/


// `get_cmd` muestra un *prompt* y lee lo que el usuario escribe usando la
// biblioteca readline. Ésta permite mantener el historial, utilizar las flechas
// para acceder a las órdenes previas del historial, búsquedas de órdenes, etc.

char* get_cmd()
{
    uid_t uid = getuid(); // Obtiene el uid
    struct passwd * passwd = getpwuid(uid); // Obtiene una entrada del fichero de cuentas del usuario

    // Con respecto a los errores es necesario tratar los mostrados en la sección "errores" de los manuales de las funciones
    if (!passwd) {
        perror("get_cmd: getpwuid");
        exit(EXIT_FAILURE);
    }

    char * user = passwd->pw_name; // Nombre del usuario sacado de la entrada

    char path[PATH_MAX];        // Ruta absoluta en el que se encuentra el usuario

    if (!getcwd(path, PATH_MAX)) {
        perror("get_cmd: getcwd");
        exit(EXIT_FAILURE);
    }

    char * dir = basename(path); // Directorio en el que estamos sin la ruta absoluta

    char prompt[strlen(user) + strlen(dir) + 4]; // Reservar el espacio para el prompt
    // + 4 por el arroba, el mayor que, el espacio y el valor nulo

    sprintf(prompt, "%s@%s> ", user, dir); // Almacena en el buffer la cadena del prompt

    char* buf;

    // Lee la orden tecleada por el usuario
    buf = readline(prompt);

    // Si el usuario ha escrito una orden, almacenarla en la historia.
    if(buf)
        add_history(buf);

    return buf;
}


/*****************************************************************************
 * Comandos internos
 * ***************************************************************************/


int check_internal(struct cmd * cmd)
{
    if (!cmd || cmd->type != EXEC)
        return -1;
    
    struct execcmd * ecmd = (struct execcmd*) cmd;
    
    if (!ecmd->argv[0])
        return -1;
    
    for (int i=0; i<N_INTERNALCMD; i++)
        if (!strcmp(ecmd->argv[0], comandos_internos[i]))
            return i;
    return -1;
}


void run_cwd(void)
{
    char path[PATH_MAX];

    if (!getcwd(path, PATH_MAX)) {
        perror("run_cwd: getcwd");
        exit(EXIT_FAILURE);
    }

    printf("cwd: %s\n", path);
}


void run_exit(struct cmd * cmd)
{
    free_cmd(cmd);
    free(cmd);
    exit(EXIT_SUCCESS);
}


void run_cd(struct execcmd * ecmd)
{
    char * path;
    
    switch(ecmd->argc)
    {
        case 1:
            if (!(path = getenv("HOME"))) {
                perror("run_cd: getenv");
                exit(EXIT_FAILURE);
            }
            break;
        case 2:
            if (!strcmp(ecmd->argv[1], "-"))
            {
                if (!(path = getenv("OLDPWD"))) {
                    printf("run_cd: Variable OLDPWD no definida\n");
                    return;
                }
            }
            else
                path = ecmd->argv[1];
            break;
        default:
            printf("run_cd: Demasiados argumentos\n");
            return;
    }
    
    char old_path[PATH_MAX];
    if (!getcwd(old_path, PATH_MAX)) {
        perror("run_cd: getcwd");
        exit(EXIT_FAILURE);
    }
    if (setenv("OLDPWD", old_path, 1)==-1) {
        perror("run_cd: setenv");
        exit(EXIT_FAILURE);
    }
    
    if (chdir(path)==-1) {//FIXME TRY
        if (errno==ENOENT)
            printf("run_cd: No existe el directorio '%s'\n", path);
        else {
            perror("run_cd: chdir");
            exit(EXIT_FAILURE);
        }
    }
}


void help_psplit(void)
{
    printf("Uso: psplit [-l NLINES] [-b NBYTES] [-s BSIZE] [-p PROCS] [FILE1] [FILE2]...\n\
            Opciones:\n\
            -l NLINES Número máximo de líneas por fichero.\n\
            -b NBYTES Número máximo de bytes por fichero.\n\
            -s BSIZE  Tamaño en bytes de los bloques leídos de [FILEn] o stdin.\n\
            -p PROCS  Número máximo de procesos simultáneos.\n\
            -h        Ayuda\n");
}


void run_psplit(char * argv[], int argc)
{//TODO 
    int modo = 0;                  // Modo escogido para el procesado:
                                   //     modo = 0   Sin definir
                                   //     modo = 1   Líneas
                                   //     modo = 2   Bytes

    int nlines = 1;                // Nº de líneas a escribir por fichero.
    int nbytes = 1;                // Nº de bytes a escribir por fichero.
    size_t bsize = DEFAULT_BSIZE;  // Tamaño de bloque leído.
    int procs = DEFAULT_PROCS;     // Nº de procesos simultáneos.
    int fd = 0;                    // Variable para guardar temporalmente el descriptor del fichero.
    int * files = NULL;            // Vector de descriptores de fichero.
    int files_size = 0;            // Tamaño del vector anterior.
    char * buf = NULL;             // Buffer en el que insertar temporalmente los datos.
    ssize_t size_readed = 0;       // Tamaño leído por un read()
                                   //     size_readed <= bsize
    int n;                         // Nº de fichero copiado.
    char str[256];                 // Nombre del fichero copiado.
    int size_written;              // Tamaño escrito por el momento en el fichero.
    int size_extracted;            // Tamaño extraído por el momento del buffer.
    int size_chosen;

    int lines_written;
    int lines_found;

    int opt = 0;
    optind = 1;

    while ((opt = getopt(argc, argv, "hl:b:s:p:")) != -1) {
        switch (opt) {
        case 'l':
        if (modo == 0) {    
            nlines = atoi(optarg);
            modo = 1;
        }
        else {
            printf("psplit: Opciones incompatibles\n");
            return;
        }
            break;
        case 'b':
        if (modo == 0) {
            nbytes = atoi(optarg);
            modo = 2;
        }
        else {
            printf("psplit: Opciones incompatibles\n");
            return;
        }
        break;
        case 's':
        bsize = atoi(optarg);
        if ((bsize < 1) || (bsize > MAX_BSIZE)) {
            printf("psplit: El tamaño de bloque debe estar comprendido entre 1 B y 1 MB\n");
            return;
        }
        break;
        case 'p':
                procs = atoi(optarg);
        if (procs <= 0) {
            printf("psplit: El número de procesos debe ser superior a cero\n");
            return;
        }
        break;
        case 'h':
        help_psplit();
        return;
        break;
        default:
        return;
        break;
    }
    }

    // Comprobamos que haya un criterio.
    if (modo == 0) {
        printf("psplit: Es necesario especificar un criterio de escritura, ya sea por líneas o por bytes.\n");
    return;
    }

    // Reservamos memoria para el buffer.
    buf = malloc(bsize * sizeof(char));
    // Inicializamos las cuenta para las escrituras.
    size_written = 0;
    size_extracted = 0;
   
    // No hay ningún fichero como parámetro.
    if (optind == argc) {
    files_size = 1;
        files = malloc(files_size * sizeof(int));
    files[0] = 0;
    }
    // Sí hay ficheros como parámetros.
    else {
    files_size = argc - optind;
    files = malloc(files_size * sizeof(int));
        for (int i = 0; i < files_size; i++) {
        if ((fd = open(argv[optind + i], O_RDONLY, S_IRWXU)) == -1) {
            perror("open");
        exit(EXIT_FAILURE);
        }
        files[i] = fd;
    }
    }

    // Criterio por líneas.
    if (modo == 1) {
        for (int i = 0; i < files_size; i++) {
        n = 0;
        fd = -1;
        lines_written = 0;
        size_extracted = 0;
        lines_found = 0;
        while (1) {
            buf -= size_extracted;
        size_extracted = 0;
        TRY( size_readed = read(files[i], buf, bsize) );
        if (size_readed == 0) {
            if (fd != -1) {
                TRY( fsync(fd) );
            TRY( close(fd) );
            }
            break;
        }
                if (fd == -1) {
                    sprintf(str, "%s%d", argv[optind + i], n);
                    n++;
                    TRY( fd = open(str, O_WRONLY|O_CREAT|O_TRUNC, S_IRWXU) );
                }
                while (size_extracted < size_readed) {
            if (lines_written == nlines) {
                TRY( fsync(fd) );
            TRY( close(fd) );
            lines_written = 0;
            sprintf(str, "%s%d", argv[optind + i], n);
                        n++;
                        TRY( fd = open(str, O_WRONLY|O_CREAT|O_TRUNC, S_IRWXU) );
            }
            else {
                while ((size_extracted != size_readed) && ((nlines - (lines_written + lines_found)) != 0)) {
                            if (buf[size_extracted] == '\n')
                    lines_found++;
                size_extracted++;
            }
            size_extracted--;
                        TRY( write(fd, buf, size_extracted) );
            lines_written += lines_found;
            buf += size_extracted;
            }
        }
        }
    }
    }
    // Criterio por bytes.
    else if (modo == 2) {
    // Bucle que itera cada fichero.
        for (int i = 0; i < files_size; i++) {
            n = 0;
        fd = -1;
        size_written = 0;
        size_extracted = 0;
        while (1) {
        buf -= size_extracted;
        size_extracted = 0;
                TRY( size_readed = read(files[i], buf, bsize) );
        if (size_readed == 0) {
            if (fd != -1) {
                TRY( fsync(fd) );
            TRY( close(fd) );
            }
            break;
        }
        if (fd == -1) {
            sprintf(str, "%s%d", argv[optind + i], n);
            n++;
            TRY( fd = open(str, O_WRONLY|O_CREAT|O_TRUNC, S_IRWXU) );
        }
        while (size_extracted < size_readed) {
            if (size_written == nbytes) {
                TRY( fsync(fd) );
            TRY( close(fd) );
            size_written = 0;
            sprintf(str, "%s%d", argv[optind + i], n);
            n++;
            TRY( fd = open(str, O_WRONLY|O_CREAT|O_TRUNC, S_IRWXU) );
            }
            else {
            size_chosen = min(nbytes - size_written, size_readed - size_extracted); 
                    TRY( write(fd, buf, size_chosen) );
                        size_written += size_chosen;
            size_extracted += size_chosen;
            buf += size_chosen;
            }
        }
        }
    }
    }

    // Cerramos los ficheros abiertos.
    if (optind != argc)
        for (int i = 0; i < files_size; i++)
            TRY( close(files[i]) );
    
    // Liberamos la memoria reservada.
    free(files);
    free(buf);
}


void help_bjobs(void)
{
    printf("Uso: bjobs [-k] [-h]\n\
    Opciones:\n\
    -k Mata todos los procesos en segundo plano.\n\
    -h Ayuda\n\n");
}


void parse_bjobs(struct execcmd * ecmd)
{
    int option;
    optind = 0;
    
    while((option = getopt(ecmd->argc, ecmd->argv, "kh")) != -1)
    {
        switch(option) {
            case 'k':
                sigchld_inhibitor(SIG_BLOCK);
                for (int i=0; i < bpids_i; i++)
                    TRY ( kill(bpids[i], SIGKILL) );    //FIXME SIGTERM?
                sigchld_inhibitor(SIG_UNBLOCK);
                break;
            case 'h':
            default:
                help_bjobs();
                return;
        }
    }
}


void run_bjobs(struct execcmd * ecmd)
{
    if (ecmd->argc==1) {
        sigchld_inhibitor(SIG_BLOCK);
        for (int i=0; i < bpids_i; i++)
            printf("[%d]\n",bpids[i]);
        sigchld_inhibitor(SIG_UNBLOCK);
    }
    else
        parse_bjobs(ecmd);
}


void exec_internal(struct cmd * cmd, int command)
{
    struct execcmd* ecmd = (struct execcmd*) cmd;
    
    switch(command) {
        case 0: run_cwd(); break;
        case 1: run_exit(cmd); break;
        case 2: run_cd(ecmd); break;
        case 3: run_psplit(ecmd->argv, ecmd->argc); break;
        case 4: run_bjobs(ecmd); break;
        default: panic("no se encontró el comando '%s'\n", ecmd->argv[0]); break;
    }
}


/*****************************************************************************
 * Señales
 * ***************************************************************************/


void treat_signals(void)
{
    sigset_t blockedsigset;
    //simplesh debe bloquear la señal SIGINT (CTRL+C)
    TRY ( sigemptyset(&blockedsigset) );
    TRY ( sigaddset(&blockedsigset, SIGINT) );
    TRY ( sigprocmask(SIG_BLOCK, &blockedsigset, NULL) );
    //simplesh debe ignorar la señal SIGQUIT (CTRL+\)
    struct sigaction sigact;
    sigact.sa_handler = SIG_IGN;
    sigemptyset(&sigact.sa_mask);
    sigact.sa_flags = 0;
    TRY ( sigaction(SIGQUIT, &sigact, NULL) );
}


// Manejador de señales para la señal SIGCHLD
void handle_sigchld(int sig)
{
    int saved_errno = errno;
    pid_t pid;
    // Evita que los procesos creados para comandos en segundo plano se conviertan en procesos zombies al terminar
    while ((pid = waitpid((pid_t)(-1), 0, WNOHANG)) > 0) {
        // Envia [PID] a stdout cuando termina la ejecución de un proceso creado para un comando en segundo plano
        remove_from_bpids(pid);
        printf("[%d]",pid);//FIXME usa write(STDOUT_FILENO,etc)
    }
    errno = saved_errno;
}


// You should do this before any child processes terminate, which in practice means registering before any are spawned
void register_handler(void)
{
    struct sigaction sa;
    sa.sa_handler = &handle_sigchld;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    TRY (sigaction(SIGCHLD, &sa, 0) );
}


/******************************************************************************
 * Bucle principal de `simplesh`
 ******************************************************************************/


void help(char **argv)
{
    info("Usage: %s [-d N] [-h]\n\
         shell simplesh v%s\n\
         Options: \n\
         -d set debug level to N\n\
         -h help\n\n",
         argv[0], VERSION);
}


void parse_args(int argc, char** argv)
{
    int option;

    // Bucle de procesamiento de parámetros
    while((option = getopt(argc, argv, "d:h")) != -1) {
        switch(option) {
            case 'd':
                g_dbg_level = atoi(optarg);
                break;
            case 'h':
            default:
                help(argv);
                exit(EXIT_SUCCESS);
                break;
        }
    }
}


int main(int argc, char** argv)
{
    char* buf;
    struct cmd* cmd;
    memset(bpids, -1, MAX_BPROC*sizeof(pid_t));
    bpids_i = 0;
    
    //Bloquea la señal SIGINT y ignora SIGQUIT
    treat_signals();
    register_handler();

    parse_args(argc, argv);

    DPRINTF(DBG_TRACE, "STR\n");
    
    //Elimina la variable de entorno OLDPWD
    if (unsetenv("OLDPWD") == -1) {
        perror("main: unsetenv");
        exit(EXIT_FAILURE);
    }

    // Bucle de lectura y ejecución de órdenes
    while ((buf = get_cmd()) != NULL)
    {
        // Realiza el análisis sintáctico de la línea de órdenes
        cmd = parse_cmd(buf);

        // Termina en `NULL` todas las cadenas de las estructuras `cmd`
        null_terminate(cmd);

        DBLOCK(DBG_CMD, {
            info("%s:%d:%s: print_cmd: ",
                 __FILE__, __LINE__, __func__);
            print_cmd(cmd); printf("\n"); fflush(NULL); } );

        // Ejecuta la línea de órdenes
        run_cmd(cmd);

        // Libera la memoria de las estructuras `cmd`
        free_cmd(cmd);
        free(cmd);

        // Libera la memoria de la línea de órdenes
        free(buf);
    }

    DPRINTF(DBG_TRACE, "END\n");

    return 0;
}
