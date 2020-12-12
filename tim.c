#define _GNU_SOURCE
#include <ctype.h>
#include <err.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pwd.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

// ANSI control sequences
#define CHA "\033[G"  // move cursor to the beginning of the line
#define EL  "\033[K"  // clear the line at the cursor

// ASCII control characters
#define ETX 3    // Ctrl + C
#define EOT 4    // Ctrl + D
#define BS  8    // Ctrl + Backspace
#define CR  13   // Enter
#define DEL 127  // Backspace

#define MAX_INPUT_BUF_SZ 801  // same as max. chat message size

#define DEFAULT_HOST "0.0.0.0"  // any IPv4 address
#define DEFAULT_PORT "7171"

#define MAX_ADDR_SZ     401
#define MAX_SOCK_MSG_SZ 1024

// Time to wait in seconds since the last message was received to send a PING
// message.
#define PING_IDLE 10
// Time to wait in seconds since the last message was received to consider the
// connection lost.
#define PING_TIMEOUT 60

#define MAX_NICK_SZ          19
#define MAX_CHAT_MSG_BODY_SZ 801

#define SCANF_NICK     "NICK %18s"  // nickname
#define SCANF_BUSY     "BUSY"
#define SCANF_CHAT_MSG "MSG %d %800[^\n]"  // chat msg id and body
#define SCANF_ACK      "ACK %d"            // chat msg id
#define SCANF_PING     "PING"
#define SCANF_PONG     "PONG"
#define SCANF_QUIT     "QUIT"

enum input_kind {
    INPUT_NONE,
    INPUT_EDIT,   // printable character or supported control character
    INPUT_QUIT,   // Ctrl + D or Ctrl + C
    INPUT_SUBMIT  // Enter
};

enum msg_kind {
    MSG_NONE,
    MSG_NICK,      // send nickname and request or accept a conversation
    MSG_BUSY,      // decline a conversation
    MSG_CHAT_MSG,  // send a chat message
    MSG_ACK,       // acknowledge a received chat message
    MSG_PING,      // test that an idle connection is still open
    MSG_PONG,      // answer PING
    MSG_QUIT,      // quit a conversation
};

struct msg {
    enum msg_kind kind;
    union {
        char nick[MAX_NICK_SZ];
        struct {
            int id;
            char body[MAX_CHAT_MSG_BODY_SZ];
        } chat_msg;
    };
};

struct chat_msg {
    int id;
    bool ack;
    char body[MAX_CHAT_MSG_BODY_SZ];
    struct chat_msg *next;
};

void parse_args(int argc, char *argv[]);
void use_default_nickname(void);
void enable_input_buf(void);
void get_term_size(void);
void disable_input_buf(void);
void hide_input_buf(void);
void show_input_buf(void);
void flush_input_buf(void);
void empty_input_buf(void);
void progn_vfprintf(FILE *fp, const char *fmt, va_list ag);
void progn_printf(const char *fmt, ...);
void printf_ib_toggle(const char *fmt, ...);
void fatalf_ib_flush(const char *fmt, ...);
enum input_kind read_input(void);
struct addrinfo *parse_addr(const char *addr);
void split_addr(char *addr, char **host, char **port);
int accept_conn(int *sockfd, struct addrinfo *info, struct sockaddr *peer_addr,
                socklen_t *peer_addr_sz);
int open_conn(struct addrinfo *info, struct sockaddr *peer_addr,
              socklen_t *peer_addr_sz);
bool peer_prompt(struct sockaddr_storage *addr, socklen_t addr_sz);
bool yes_or_no(const char *fmt, ...);
void send_msg(int sockfd, struct msg *msg);
void read_msg(int sockfd, struct msg *msg);
const char *peer_nick_or_placeholder(const char *placeholder);
void print_unack_chat_msg_count(void);
void free_chat_msg(void);
void add_chat_msg(const char *body);
void ack_chat_msg(int id);

const char help[] =
    "Tiny instant messenger\n"
    "\n"
    "Usage:\n"
    "    tim -l\n"
    "    tim -c localhost -n ferris\n"
    "    tim -L [::1]:8000 -y\n"
    "    tim -c [::1]:8000 -n ferris\n"
    "\n"
    "Options:\n"
    "    -l       Listen for a connection at any IPv4 address\n"
    "    -L ADDR  Listen for a connection at a specific address\n"
    "    -c ADDR  Open a connection to an address\n"
    "    -n NICK  Change your default nickname\n"
    "    -y       Assume yes when asked to start a conversation\n"
    "    -h       Show this message\n";

const char *opt_listen;
const char *opt_connect;
const char *opt_nick;
bool opt_assume_yes;  // assume yes to all prompts

struct winsize term_size;
struct termios term_initial_state;

char input_buf[MAX_INPUT_BUF_SZ];  // raw terminal input buffer
int input_buf_len;

char peer_nick[MAX_NICK_SZ];

struct chat_msg *sent_chat_msg;

int main(int argc, char *argv[]) {
    parse_args(argc, argv);

    if (opt_nick == NULL) {
        use_default_nickname();
    }
    if (strlen(opt_nick) > MAX_NICK_SZ - 1) {
        errx(1, "Your nickname must be at most %d characters long",
             MAX_NICK_SZ - 1);
    }

    struct addrinfo *addr_info = NULL;

    int sockfd = 0;
    int connfd = 0;

    struct sockaddr_storage peer_addr = {0};
    socklen_t peer_addr_sz = sizeof(peer_addr);

    if (opt_connect == NULL) {
        addr_info = parse_addr(opt_listen);
        connfd = accept_conn(&sockfd, addr_info, (struct sockaddr *)&peer_addr,
                             &peer_addr_sz);
    } else {
        addr_info = parse_addr(opt_connect);
        connfd = (sockfd = open_conn(addr_info, (struct sockaddr *)&peer_addr,
                                     &peer_addr_sz));
    }

    struct msg msg = {0};

    if (opt_connect == NULL) {
        read_msg(connfd, &msg);
        if (msg.kind == MSG_NICK) {
            strcpy(peer_nick, msg.nick);
        } else {
            err(1, "Read an unexpected message");
        }

        if (!opt_assume_yes) {
            if (!peer_prompt(&peer_addr, peer_addr_sz)) {
                msg.kind = MSG_BUSY;
                send_msg(connfd, &msg);
                progn_printf("You declined the conversation\n");
                exit(0);
            }
        }
    }

    msg.kind = MSG_NICK;
    strcpy(msg.nick, opt_nick);
    send_msg(connfd, &msg);

    if (opt_connect != NULL) {
        read_msg(connfd, &msg);
        if (msg.kind == MSG_NICK) {
            strcpy(peer_nick, msg.nick);
        } else if (msg.kind == MSG_BUSY) {
            err(1, "Your peer is busy");
        } else {
            err(1, "Read an unexpected message");
        }
    }

    freeaddrinfo(addr_info);

    progn_printf("You are now talking to %s\n", peer_nick);

    enable_input_buf();
    atexit(disable_input_buf);

    atexit(free_chat_msg);
    atexit(print_unack_chat_msg_count);

    struct pollfd fds[3];
    nfds_t fds_len = 2;

    fds[0].fd = STDIN_FILENO;
    fds[0].events = POLLIN;

    fds[1].fd = connfd;
    fds[1].events = POLLIN;

    if (opt_connect == NULL) {
        fds[2].fd = sockfd;
        fds[2].events = POLLIN;
        fds_len++;
    }

    time_t last_msg = time(NULL);
    bool sent_ping = false;

    while (true) {
        int ready = poll(fds, fds_len, 1000);
        if (ready == -1) {
            fatalf_ib_flush("poll: %m\n");
        }

        time_t now = time(NULL);

        if (now - last_msg > PING_TIMEOUT) {
            fatalf_ib_flush("Lost connection to %s\n", peer_nick);
        }

        if (opt_connect == NULL && now - last_msg > PING_IDLE && !sent_ping) {
            msg.kind = MSG_PING;
            send_msg(connfd, &msg);
            sent_ping = true;
        }

        if (ready > 0) {
            get_term_size();
        }

        if (fds[0].revents & POLLIN) {
            enum input_kind kind = read_input();
            if (kind == INPUT_QUIT) {
                printf_ib_toggle("You ended the conversation\n");
                msg.kind = MSG_QUIT;
                send_msg(connfd, &msg);
                break;
            } else if (kind == INPUT_EDIT) {
                hide_input_buf();
                show_input_buf();
            } else if (kind == INPUT_SUBMIT) {
                if (input_buf_len > 0) {
                    add_chat_msg(input_buf);
                    msg.kind = MSG_CHAT_MSG;
                    msg.chat_msg.id = sent_chat_msg->id;
                    strcpy(msg.chat_msg.body, sent_chat_msg->body);
                    send_msg(connfd, &msg);

                    hide_input_buf();
                    printf("%s: %s\n", opt_nick, input_buf);
                    empty_input_buf();
                }
            }
        }

        if (fds[1].revents & POLLIN) {
            read_msg(connfd, &msg);
            if (msg.kind == MSG_CHAT_MSG) {
                hide_input_buf();
                printf("%s: %s\n", peer_nick, msg.chat_msg.body);
                show_input_buf();
                msg.kind = MSG_ACK;
                send_msg(connfd, &msg);
            } else if (msg.kind == MSG_ACK) {
                ack_chat_msg(msg.chat_msg.id);
            } else if (msg.kind == MSG_QUIT) {
                flush_input_buf();
                printf_ib_toggle("%s has ended the conversation\n", peer_nick);
                exit(0);
            } else if (msg.kind == MSG_PING) {
                msg.kind = MSG_PONG;
                send_msg(connfd, &msg);
            } else if (msg.kind == MSG_PONG) {
                sent_ping = false;
            } else {
                fatalf_ib_flush("Read an unexpected message\n");
            }
            last_msg = now;
        }

        if (fds[2].revents & POLLIN) {
            int fd = accept(sockfd, NULL, NULL);
            if (fd != -1) {
                msg.kind = MSG_BUSY;
                send_msg(connfd, &msg);
            }
        }
    }
}

void parse_args(int argc, char *argv[]) {
    opterr = 0;  // disable getopt default error messages
    int opt = 0;
    while ((opt = getopt(argc, argv, ":L:c:n:lyh")) != -1) {
        switch (opt) {
        case 'l':
            opt_listen = DEFAULT_HOST;
            break;
        case 'L':
            opt_listen = optarg;
            break;
        case 'c':
            opt_connect = optarg;
            break;
        case 'n':
            opt_nick = optarg;
            break;
        case 'y':
            opt_assume_yes = true;
            break;
        case 'h':
            printf("%s", help);
            exit(0);
        case ':':
            errx(1, "Missing value for option \"-%c\"", optopt);
        case '?':
            errx(1, "Unknown option \"-%c\"", optopt);
        }
    }

    if (opt_listen == NULL && opt_connect == NULL) {
        printf("%s", help);
        exit(0);
    }
    if (opt_listen != NULL && opt_connect != NULL) {
        errx(1, "Can't listen and connect at the same time");
    }
}

void use_default_nickname() {
    errno = 0;
    struct passwd *pw = getpwuid(geteuid());
    if (pw == NULL) {
        err(1, "getpwuid");
    }
    opt_nick = pw->pw_name;
}

void enable_input_buf() {
    get_term_size();

    if (tcgetattr(STDIN_FILENO, &term_initial_state) == -1) {
        err(1, "tcgetattr");
    }
    struct termios raw_state = term_initial_state;
    cfmakeraw(&raw_state);
    raw_state.c_oflag |= (OPOST | ONLCR);
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw_state) == -1) {
        err(1, "tcsetattr");
    }

    setbuf(stdout, NULL);
}

void get_term_size() {
    if (ioctl(STDIN_FILENO, TIOCGWINSZ, &term_size) == -1) {
        fatalf_ib_flush("ioctl: %m\n");
    }
}

void disable_input_buf() {
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &term_initial_state) == -1) {
        err(1, "tcsetattr");
    }
}

void hide_input_buf() {
    printf(CHA EL);
}

void show_input_buf() {
    if (input_buf_len > 0) {
        int i = 0;
        if (input_buf_len > term_size.ws_col - 1) {
            i = input_buf_len - term_size.ws_col + 1;
        }
        printf("%s", &input_buf[i]);
    }
}

void flush_input_buf() {
    if (input_buf_len > 0) {
        // assumes input buffer is shown
        printf("\n");
        empty_input_buf();
    }
}

void empty_input_buf() {
    input_buf[0] = '\0';
    input_buf_len = 0;
}

void progn_vfprintf(FILE *fp, const char *fmt, va_list ag) {
    fprintf(
        fp, "%s: ",
        program_invocation_short_name);  // match err function printing style
    vfprintf(fp, fmt, ag);
}

void progn_printf(const char *fmt, ...) {
    va_list ag;
    va_start(ag, fmt);
    progn_vfprintf(stdout, fmt, ag);
    va_end(ag);
}

void printf_ib_toggle(const char *fmt, ...) {
    hide_input_buf();
    va_list ag;
    va_start(ag, fmt);
    progn_vfprintf(stdout, fmt, ag);
    va_end(ag);
    show_input_buf();
}

void fatalf_ib_flush(const char *fmt, ...) {
    flush_input_buf();
    va_list ag;
    va_start(ag, fmt);
    progn_vfprintf(stderr, fmt, ag);
    va_end(ag);
    exit(1);
}

enum input_kind read_input() {
    int c = getchar();
    if (c == EOF) {
        if (ferror(stdin)) {
            fatalf_ib_flush("Error reading stdin\n");
        }
        return INPUT_QUIT;
    }

    switch (c) {
    case ETX:
    case EOT:
        return INPUT_QUIT;
    case CR:
        return INPUT_SUBMIT;
    case BS:
        while (input_buf_len > 0) {
            input_buf[--input_buf_len] = '\0';
            if (input_buf_len == 0 || input_buf[input_buf_len - 1] == ' ') {
                break;
            }
        }
        return INPUT_EDIT;
    case DEL:
        if (input_buf_len > 0) {
            input_buf[--input_buf_len] = '\0';
        }
        return INPUT_EDIT;
    default:
        if (isprint(c) && input_buf_len < MAX_INPUT_BUF_SZ - 1) {
            input_buf[input_buf_len++] = c;
            input_buf[input_buf_len] = '\0';
            return INPUT_EDIT;
        }
    }

    return INPUT_NONE;
}

struct addrinfo *parse_addr(const char *addr) {
    char *host = NULL;
    char *port = NULL;

    if (addr != NULL) {
        if (strlen(addr) > MAX_ADDR_SZ - 1) {
            errx(1, "Address must be less than %d characters long",
                 MAX_ADDR_SZ - 1);
        }

        char split[MAX_ADDR_SZ];
        strcpy(split, addr);
        split_addr(split, &host, &port);
    }

    if (host == NULL) {
        host = DEFAULT_HOST;
    }
    if (port == NULL) {
        port = DEFAULT_PORT;
    }

    struct addrinfo hints = {0};
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo *info = NULL;
    int errcode = getaddrinfo(host, port, &hints, &info);
    if (errcode == EAI_SYSTEM) {
        err(1, "getaddrinfo");
    } else if (errcode != 0) {
        errx(1, "getaddrinfo: %s", gai_strerror(errcode));
    }
    return info;
}

void split_addr(char *addr, char **host, char **port) {
    if (*addr == '[') {
        *host = ++addr;
        while (*addr != ']' && *addr != '\0') {
            addr++;
        }
        if (*addr == ']') {
            *addr = '\0';
            ++addr;
        }
        if (*addr == ':') {
            *port = ++addr;
        }
    } else if (*addr != '\0') {
        if (*addr != ':') {
            *host = addr;
        }
        while (*addr != ':' && *addr != '\0') {
            addr++;
        }
        if (*addr == ':') {
            *addr = '\0';
            *port = ++addr;
        }
    }
}

int accept_conn(int *sockfd, struct addrinfo *info, struct sockaddr *peer_addr,
                socklen_t *peer_addr_sz) {
    char *cause = NULL;
    for (struct addrinfo *ai = info; ai != NULL; ai = ai->ai_next) {
        int fd = socket(ai->ai_family, SOCK_STREAM, 0);
        if (fd == -1) {
            cause = "socket";
            continue;
        }

        int opt = 1;
        socklen_t opt_sz = sizeof(opt);
        if (setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &opt, opt_sz) == -1) {
            err(1, "setsockopt");
        }

        if (bind(fd, ai->ai_addr, ai->ai_addrlen) == -1) {
            cause = "bind";
            int no = errno;
            close(fd);
            errno = no;
            continue;
        }

        if (listen(fd, 0) == -1) {
            cause = "listen";
            int no = errno;
            close(fd);
            errno = no;
            continue;
        }

        *sockfd = fd;
        break;
    }
    if (*sockfd == -1) {
        err(1, "%s", cause);
    }

    int connfd = accept(*sockfd, peer_addr, peer_addr_sz);
    if (connfd == -1) {
        err(1, "accept");
    }
    return connfd;
}

int open_conn(struct addrinfo *info, struct sockaddr *peer_addr,
              socklen_t *peer_addr_sz) {
    char *cause = NULL;
    for (struct addrinfo *ai = info; ai != NULL; ai = ai->ai_next) {
        int fd = socket(ai->ai_family, SOCK_STREAM, 0);
        if (fd == -1) {
            cause = "socket";
            continue;
        }

        if (connect(fd, ai->ai_addr, ai->ai_addrlen) == -1) {
            cause = "connect";
            int no = errno;
            close(fd);
            errno = no;
            continue;
        }

        memcpy(peer_addr, ai->ai_addr, ai->ai_addrlen);
        *peer_addr_sz = ai->ai_addrlen;
        return fd;
    }
    err(1, "%s", cause);
}

bool peer_prompt(struct sockaddr_storage *addr, socklen_t addr_sz) {
    char host[NI_MAXHOST] = {0};
    int errcode = getnameinfo((struct sockaddr *)addr, addr_sz, host,
                              NI_MAXHOST, NULL, 0, NI_NUMERICHOST);
    if (errcode == EAI_SYSTEM) {
        err(1, "getnameinfo");
    } else if (errcode != 0) {
        errx(1, "getnameinfo: %s", gai_strerror(errcode));
    }

    return yes_or_no("Talk to \"%s\" from %s? [y/N]: ", peer_nick, host);
}

bool yes_or_no(const char *fmt, ...) {
    while (true) {
        va_list ag;
        va_start(ag, fmt);
        progn_vfprintf(stdout, fmt, ag);
        va_end(ag);

        int c = 0;
        while ((c = getchar()) != '\n') {
            if (c == EOF) {
                if (ferror(stdin)) {
                    errx(1, "Error reading stdin");
                }
                exit(0);
            }

            if (tolower(c) == 'y') {
                return true;
            }
            if (tolower(c) == 'n') {
                return false;
            }
        }
    }
}

void send_msg(int sockfd, struct msg *msg) {
    char buf[MAX_SOCK_MSG_SZ] = {0};

    if (msg->kind == MSG_NICK) {
        sprintf(buf, "NICK %s", msg->nick);
    } else if (msg->kind == MSG_BUSY) {
        sprintf(buf, "BUSY");
    } else if (msg->kind == MSG_CHAT_MSG) {
        sprintf(buf, "MSG %d %s", msg->chat_msg.id, msg->chat_msg.body);
    } else if (msg->kind == MSG_ACK) {
        sprintf(buf, "ACK %d", msg->chat_msg.id);
    } else if (msg->kind == MSG_PING) {
        sprintf(buf, "PING");
    } else if (msg->kind == MSG_PONG) {
        sprintf(buf, "PONG");
    } else if (msg->kind == MSG_QUIT) {
        sprintf(buf, "QUIT");
    }

    if (write(sockfd, buf, sizeof(buf)) == -1) {
        if (msg->kind != MSG_BUSY && msg->kind != MSG_QUIT) {
            fatalf_ib_flush("write: %m\n");
        }
    }
}

void read_msg(int sockfd, struct msg *msg) {
    char buf[MAX_SOCK_MSG_SZ] = {0};
    ssize_t sz = read(sockfd, buf, MAX_SOCK_MSG_SZ);
    if (sz == -1) {
        fatalf_ib_flush("read: %m\n");
    } else if (sz == 0) {
        fatalf_ib_flush("%s has disconnected unexpectedly\n",
                        peer_nick_or_placeholder("Your peer"));
    }

    if (strncmp(buf, SCANF_BUSY, sizeof(SCANF_BUSY)) == 0) {
        msg->kind = MSG_BUSY;
    } else if (sscanf(buf, SCANF_NICK, msg->nick) == 1) {
        msg->kind = MSG_NICK;
    } else if (sscanf(buf, SCANF_CHAT_MSG, &msg->chat_msg.id,
                      msg->chat_msg.body) == 2) {
        msg->kind = MSG_CHAT_MSG;
    } else if (sscanf(buf, SCANF_ACK, &msg->chat_msg.id) == 1) {
        msg->kind = MSG_ACK;
    } else if (strncmp(buf, SCANF_PING, sizeof(SCANF_PING)) == 0) {
        msg->kind = MSG_PING;
    } else if (strncmp(buf, SCANF_PONG, sizeof(SCANF_PONG)) == 0) {
        msg->kind = MSG_PONG;
    } else if (strncmp(buf, SCANF_QUIT, sizeof(SCANF_QUIT)) == 0) {
        msg->kind = MSG_QUIT;
    } else {
        fatalf_ib_flush("Read an invalid message\n");
    }
}

const char *peer_nick_or_placeholder(const char *placeholder) {
    if (*peer_nick == '\0') {
        return placeholder;
    }
    return peer_nick;
}

void print_unack_chat_msg_count() {
    struct chat_msg *mp = sent_chat_msg;
    int n = 0;
    while (mp != NULL) {
        if (!mp->ack) {
            n++;
        }
        mp = mp->next;
    }

    if (n > 0) {
        progn_printf("Your last %d message(s) may not have been sent\n", n);
    }
}

void free_chat_msg() {
    struct chat_msg *cmsg = sent_chat_msg;
    while (cmsg != NULL) {
        struct chat_msg *mp = cmsg;
        cmsg = cmsg->next;
        free(mp);
    }
}

void add_chat_msg(const char *body) {
    struct chat_msg *mp = malloc(sizeof(struct chat_msg));
    mp->ack = false;
    strcpy(mp->body, body);
    mp->next = sent_chat_msg;

    if (sent_chat_msg == NULL) {
        mp->id = 1;
    } else {
        mp->id = sent_chat_msg->id + 1;
    }

    sent_chat_msg = mp;
}

void ack_chat_msg(int id) {
    struct chat_msg *mp = sent_chat_msg;
    while (mp != NULL) {
        if (mp->id == id) {
            mp->ack = true;
        }
        mp = mp->next;
    }
}
