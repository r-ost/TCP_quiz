#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <signal.h>
#include <netdb.h>
#include <sys/stat.h>
#include <fcntl.h>
#define ERR(source) (perror(source),                                 \
                     fprintf(stderr, "%s:%d\n", __FILE__, __LINE__), \
                     exit(EXIT_FAILURE))
#define BUFOUT_MAX 10
#define QUESTION_MAXLENGTH 2000
#define STDIN_BUFSIZE 50
#define MAX_SERVERS 10

/////////////////////////
// SIGNALS
/////////////////////////

volatile sig_atomic_t doWork = 1;

int sethandler(void (*f)(int), int sigNo)
{
    struct sigaction act;
    memset(&act, 0, sizeof(struct sigaction));
    act.sa_handler = f;
    if (-1 == sigaction(sigNo, &act, NULL))
        return -1;
    return 0;
}

void sigint_handler(int sigNo)
{
    doWork = 0;
}

/////////////////////////
// CONNECTION
/////////////////////////

int make_socket(void)
{
    int sock;
    sock = socket(PF_INET, SOCK_STREAM, 0);
    if (sock < 0)
        ERR("socket");
    return sock;
}

struct sockaddr_in make_address(char *address, char *port)
{
    int ret;
    struct sockaddr_in addr;
    struct addrinfo *result;
    struct addrinfo hints = {};
    hints.ai_family = AF_INET;
    if ((ret = getaddrinfo(address, port, &hints, &result)))
    {
        fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(ret));
        exit(EXIT_FAILURE);
    }
    addr = *(struct sockaddr_in *)(result->ai_addr);
    freeaddrinfo(result);
    return addr;
}

int connect_socket(char *name, char *port)
{
    struct sockaddr_in addr;
    int socketfd = make_socket();
    addr = make_address(name, port);
    if (connect(socketfd, (struct sockaddr *)&addr, sizeof(struct sockaddr_in)) < 0)
    {
        if (errno != EINTR)
            ERR("connect");
        else
        {
            /////////////////////////////////// WAZNE ///////////////////
            fd_set wfds;
            int status;
            socklen_t size = sizeof(int);
            FD_ZERO(&wfds);
            FD_SET(socketfd, &wfds);
            if (TEMP_FAILURE_RETRY(select(socketfd + 1, NULL, &wfds, NULL, NULL)) < 0)
                ERR("select");
            if (getsockopt(socketfd, SOL_SOCKET, SO_ERROR, &status, &size) < 0)
                ERR("getsockopt");
            if (0 != status)
                ERR("connect");
        }
    }
    return socketfd;
}

/////////////////////////
// UTILS
/////////////////////////

ssize_t bulk_write(int fd, char *buf, size_t count)
{
    int c;
    size_t len = 0;
    do
    {
        c = TEMP_FAILURE_RETRY(write(fd, buf, count));
        if (c < 0)
            return c;
        buf += c;
        len += c;
        count -= c;
    } while (count > 0);
    return len;
}

ssize_t bulk_read(int fd, char *buf, size_t count)
{
    int c;
    size_t len = 0;
    do
    {
        c = TEMP_FAILURE_RETRY(read(fd, buf, count));
        if (c < 0)
            return c;
        if (0 == c)
            return len;
        buf += c;
        len += c;
        count -= c;
    } while (count > 0);
    return len;
}
void usage()
{
    fprintf(stderr, "USAGE: [address1] [port1] [address2] [port2] ...\n");
    exit(EXIT_FAILURE);
}

int compare(char *s1, char *s2, int n)
{
    int same = 1;
    for (int i = 0; i < n; i++)
    {
        if (s1[i] != s2[i])
        {
            same = 0;
            break;
        }
    }

    return same;
}

/////////////////////////
// PROGRAM
/////////////////////////

void doClient(int sfds[], int servers_count)
{
    fd_set base_rfds, rfds;
    FD_ZERO(&base_rfds);
    int fdmax = 0;

    for (int i = 0; i < servers_count; i++)
    {
        FD_SET(sfds[i], &base_rfds);

        if (sfds[i] > fdmax)
            fdmax = sfds[i];
    }
    FD_SET(STDIN_FILENO, &base_rfds);

    char curr_buffers[MAX_SERVERS][BUFOUT_MAX];
    char currQuestions[MAX_SERVERS][QUESTION_MAXLENGTH];
    int currQuestionsLength[MAX_SERVERS];
    for (int i = 0; i < servers_count; i++)
        currQuestionsLength[i] = 0;

    // 0 - idle, 1 - currently reading, 2 - have already read
    int currentlyRead[MAX_SERVERS];
    for (int i = 0; i < servers_count; i++)
        currentlyRead[i] = 0;

    sigset_t mask, oldmask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGINT);
    sigprocmask(SIG_BLOCK, &mask, &oldmask);

    while (doWork)
    {
        // read questions
        rfds = base_rfds;
        char choice[STDIN_BUFSIZE];
        if (pselect(fdmax + 1, &rfds, NULL, NULL, NULL, &oldmask) > 0)
        {
            if (FD_ISSET(STDIN_FILENO, &rfds))
            {
                scanf("%s", choice);

                int not_now = 1;
                for (int i = 0; i < servers_count; i++)
                {
                    if (currentlyRead[i] == 2 && sfds[i] != -1)
                    {
                        // send to server
                        if (choice[0] >= 'A' && choice[0] <= 'D')
                            if (bulk_write(sfds[i], &choice[0], sizeof(char)) < 0)
                                if (errno != EPIPE)
                                    ERR("write");

                        not_now = 0;
                    }
                }

                if (not_now)
                    printf("nie teraz!\n");
            }

            for (int i = 0; i < servers_count; i++)
            {
                // read from i-th server fragment of question
                if (FD_ISSET(sfds[i], &rfds))
                {
                    currentlyRead[i] = 1;
                    int count;
                    if ((count = read(sfds[i], curr_buffers[i], BUFOUT_MAX)) < 0)
                        ERR("read");

                    if (compare(curr_buffers[i], "NIE", strlen("NIE")) == 1)
                    {
                        printf("\nNIE\n");
                        doWork = 0;
                    }

                    if (compare(curr_buffers[i], "Koniec", strlen("Koniec")) == 1)
                    {
                        printf("\nKoniec\n");
                        if (TEMP_FAILURE_RETRY(close(sfds[i])) < 0)
                            ERR("close");
                        FD_CLR(sfds[i], &base_rfds);
                        sfds[i] = -1;
                        int go = 0;
                        for (int i = 0; i < servers_count; i++)
                            if (sfds[i] != -1)
                                go = 1;
                        if (go == 0)
                            doWork = 0;

                        continue;
                    }

                    for (int j = 0; j < count; j++)
                    {
                        currQuestions[i][currQuestionsLength[i]] = curr_buffers[i][j];

                        if (curr_buffers[i][j] == '\0')
                        {
                            for (int k = 0; k < servers_count; k++)
                            {
                                if (k == i)
                                    continue;
                                if (currentlyRead[k] == 2) // waiting for answer
                                {
                                    currentlyRead[k] = 0;
                                    char val = '0';
                                    if (sfds[k] != -1)
                                        if (bulk_write(sfds[k], &val, 1) < 0)
                                            if (errno != EPIPE)
                                                ERR("bulk_write");
                                    printf("\nZa dlugo zwlekales!!! Kolejne pytanie.\n");
                                }
                            }

                            printf("%s\n", currQuestions[i]);

                            printf("Wybierz odpowiedz: ");
                            fflush(stdout);

                            currentlyRead[i] = 2;

                            currQuestionsLength[i] = 0;
                            break;
                        }
                        else
                            currQuestionsLength[i]++;
                    }
                }
            }
        }
        else
        {
            if (EINTR != errno)
                ERR("select");
        }
    }

    sigprocmask(SIG_UNBLOCK, &mask, NULL);
}

int main(int argc, char **argv)
{

    if (argc % 2 != 1)
        usage();

    int servers_count = (argc - 1) / 2;

    if (sethandler(SIG_IGN, SIGPIPE))
        ERR("set_handler");
    if (sethandler(sigint_handler, SIGINT))
        ERR("set_handler");

    int *sfds = malloc(sizeof(int) * servers_count);
    if (sfds == NULL)
        ERR("malloc");
    int sfds_count = 0;

    for (int i = 1; i < argc; i += 2)
    {
        sfds[sfds_count++] = connect_socket(argv[i], argv[i + 1]);

        // nonblock mode
        int new_flags = fcntl(sfds[sfds_count - 1], F_GETFL) | O_NONBLOCK;
        fcntl(sfds[sfds_count - 1], F_SETFL, new_flags);
    }

    doClient(sfds, sfds_count);

    for (int i = 0; i < servers_count; i++)
        if (sfds[i] != -1)
            if (TEMP_FAILURE_RETRY(close(sfds[i])) < 0)
                ERR("close");

    printf("\n");
    free(sfds);

    return EXIT_SUCCESS;
}
