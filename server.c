// Oświadczam, że niniejsza praca stanowiąca podstawę do uznania osiągnięcia efektów uczenia
// się z przedmiotu SOP2 została wykonana przeze mnie samodzielnie. [Jan Szablanowski] [305893]
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <signal.h>
#include <netdb.h>
#include <fcntl.h>
#include <arpa/inet.h>
#include <time.h>

#define ERR(source) (perror(source),                                 \
                     fprintf(stderr, "%s:%d\n", __FILE__, __LINE__), \
                     exit(EXIT_FAILURE))
#define BACKLOG 3
#define QUESTION_MAXLENGTH 2000
#define BUFOUT_MAX 10
#define MAX_QUESTIONS 20

typedef struct clientInfo_t
{
    int quiestion_number;
    int bytes_send;
    int sent;
    int available;
} clientInfo_t;

/////////////////////////
// SIGNALS
/////////////////////////

volatile sig_atomic_t accept_connection = 1;
volatile sig_atomic_t do_work = 1;

int sethandler(void (*f)(int), int sigNo)
{
    struct sigaction act;
    memset(&act, 0, sizeof(struct sigaction));
    act.sa_handler = f;
    if (-1 == sigaction(sigNo, &act, NULL))
        return -1;
    return 0;
}

void sigusr1_handler(int sigNo)
{
    accept_connection = 0;
}

void sigint_handler(int sigNo)
{
    do_work = 0;
}

/////////////////////////
// SOCKETS
/////////////////////////

int make_socket(int domain, int type)
{
    int sock;
    sock = socket(domain, type, 0);
    if (sock < 0)
        ERR("socket");
    return sock;
}

int bind_inet_socket(uint16_t port, char *ip_address, int type)
{
    struct sockaddr_in addr;
    int socketfd, t = 1;
    socketfd = make_socket(PF_INET, type);
    memset(&addr, 0, sizeof(struct sockaddr_in));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    //addr.sin_addr.s_addr = inet_addr(ip_address);

    if (setsockopt(socketfd, SOL_SOCKET, SO_REUSEADDR, &t, sizeof(t)))
        ERR("setsockopt");
    if (bind(socketfd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
        ERR("bind");

    // listen if we have tcp connection
    if (SOCK_STREAM == type)
        if (listen(socketfd, BACKLOG) < 0)
            ERR("listen");

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
    fprintf(stderr, "USAGE: [address] [port] [max_clients] [questions_path]\n");
    exit(EXIT_FAILURE);
}

/////////////////////////
// PROGRAM
/////////////////////////

int add_new_client(int sfd)
{
    int nfd;
    if ((nfd = TEMP_FAILURE_RETRY(accept(sfd, NULL, NULL))) < 0)
    {
        if (EAGAIN == errno || EWOULDBLOCK == errno)
            return -1;
        ERR("accept");
    }
    return nfd;
}

int write_to_clients(int cfds[], clientInfo_t clients_info[], int max_clients, int *clients_count,
                     char questions[MAX_QUESTIONS][QUESTION_MAXLENGTH], int questions_count)
{
    char out_fragment[BUFOUT_MAX];
    for (int i = 0; i < max_clients; i++)
    {
        if (cfds[i] != -1 && clients_info[i].available && clients_info[i].sent == 0)
        {
            // prepare question
            int question_number = clients_info[i].quiestion_number;
            int bytes_sent = clients_info[i].bytes_send;

            int curr_bytes = rand() % (strlen(questions[question_number]) - bytes_sent + 2) + 1;
            if (curr_bytes > BUFOUT_MAX)
                curr_bytes = BUFOUT_MAX;

            clients_info[i].bytes_send += curr_bytes;

            if (clients_info[i].bytes_send > strlen(questions[question_number]))
            {
                clients_info[i].bytes_send -= curr_bytes;
                curr_bytes = (int)strlen(questions[question_number]) + 1 - clients_info[i].bytes_send;
                clients_info[i].bytes_send += curr_bytes;
                clients_info[i].sent = 1;
            }

            for (int j = 0; j < curr_bytes; j++)
                out_fragment[j] = questions[question_number][bytes_sent + j];

            printf("Sending %d bytes to client no. %d. Total bytes sent: %d\n", curr_bytes, i, clients_info[i].bytes_send);
            fflush(stdout);
            if (bulk_write(cfds[i], out_fragment, curr_bytes) < 0)
            {
                if (errno == EPIPE) // lost connection to client
                {
                    printf("CLIENT QUITED!\n");
                    memset(&clients_info[i], 0, sizeof(clients_info[i]));
                    clients_info[i].available = 0;
                    if (TEMP_FAILURE_RETRY(close(cfds[i])))
                        ERR("close");
                    cfds[i] = -1;
                    (*clients_count)--;
                }
                else
                    ERR("write");
            }
        }
    }
    return 0; // ok
}

void doServer(int fdT, uint16_t max_clients, char questions[MAX_QUESTIONS][QUESTION_MAXLENGTH], int n)
{
    int fdmax = fdT;
    int clients_count = 0;

    fd_set rfds;
    fd_set base_rfds;
    sigset_t mask, oldmask;

    clientInfo_t *clients_info = malloc(sizeof(clientInfo_t) * max_clients);
    if (clients_info == NULL)
        ERR("malloc");
    for (int i = 0; i < max_clients; i++)
    {
        clients_info[i].bytes_send = 0;
        clients_info[i].quiestion_number = 1;
        clients_info[i].sent = 0;
    }

    int *cfds = (int *)malloc(sizeof(int) * max_clients);
    if (cfds == NULL)
        ERR("malloc");
    for (int i = 0; i < max_clients; i++)
        cfds[i] = -1;

    FD_ZERO(&base_rfds);
    FD_SET(fdT, &base_rfds);

    sigemptyset(&mask);
    sigaddset(&mask, SIGUSR1);
    sigaddset(&mask, SIGINT);
    sigprocmask(SIG_BLOCK, &mask, &oldmask);

    while (do_work)
    {

        rfds = base_rfds;
        int res;
        struct timespec ts = {.tv_nsec = 330000000, .tv_sec = 0};
        if ((res = pselect(fdmax + 1, &rfds, NULL, NULL, &ts, &oldmask)) > 0)
        {
            // Add new client
            if (FD_ISSET(fdT, &rfds))
            {
                printf("NEW CLIENT!\n");
                int cfd = add_new_client(fdT);
                if (accept_connection == 0)
                {
                    printf("We cant handle more clients!\n");
                    printf("CLIENT QUITED!\n");
                    char* mess = "NIE";
                    if (bulk_write(cfd, mess, strlen("NIE")) < 0)
                        ERR("write");
                    if (TEMP_FAILURE_RETRY(close(cfd)))
                        ERR("close");
                    continue;
                }

                if (cfd < 0)
                    continue; // select stwierdzil gotowosc deskryptora, ale
                              // polaczenie zostalo zerwane przed wywolaniem accept

                if (clients_count == max_clients)
                {
                    printf("We cant handle more clients!\n");
                    printf("CLIENT QUITED!\n");
                    char *message = "NIE";
                    if (bulk_write(cfd, message, strlen(message)) < 0)
                        ERR("write");
                    // close connection
                    if (TEMP_FAILURE_RETRY(close(cfd)) < 0)
                        ERR("close");
                    continue;
                }

                int i = 0;
                while (cfds[i] != -1)
                    i++;

                cfds[i] = cfd;
                clients_info[i].available = 1;
                clients_count++;
                FD_SET(cfd, &base_rfds);

                if (cfd > fdmax)
                    fdmax = cfd;
            }
            // Read answer from clients
            for (int i = 0; i < max_clients; i++)
            {
                if (cfds[i] != -1 && FD_ISSET(cfds[i], &rfds))
                {
                    char ans;
                    int count;
                    if ((count = bulk_read(cfds[i], &ans, sizeof(char))) < 0)
                        ERR("read");
                    if (count != 0 && ans != '0')
                        printf("\nANSWER!!!: Received answer from client %d: %c\n\n", i, ans);
                    else if (count != 0 && ans == '0')
                        printf("\nClient timed out, received: 0\n");
                    //fflush(stdout);
                    clients_info[i].bytes_send = 0;
                    clients_info[i].quiestion_number = rand() % n;
                    clients_info[i].sent = 0; // false
                }
            }
        }
        else if (res < 0)
            if (EINTR != errno)
                ERR("select");

        write_to_clients(cfds, clients_info, max_clients, &clients_count, questions, n);

        FD_ZERO(&base_rfds);
        FD_SET(fdT, &base_rfds);
        fdmax = fdT;
        for (int i = 0; i < max_clients; i++)
        {
            if (cfds[i] != -1)
            {
                FD_SET(cfds[i], &base_rfds);
                if (cfds[i] > fdmax)
                    fdmax = cfds[i];
            }
        }
    }
    

    for (int i = 0; i < max_clients; i++)
        if (cfds[i] != -1)
        {
            char* message = "Koniec";
            if (bulk_write(cfds[i], message, strlen(message)) < 0)
                ERR("write");

            if (TEMP_FAILURE_RETRY(close(cfds[i])))
                ERR("close");
            cfds[i] = -1;
        }

    sigprocmask(SIG_UNBLOCK, &mask, NULL);

    free(cfds);
    free(clients_info);
}

void read_questions(char *path, char questions[MAX_QUESTIONS][QUESTION_MAXLENGTH], int *questions_count)
{
    srand(time(NULL));

    int qfd;
    if ((qfd = open(path, O_RDONLY)) < 0)
        ERR("open");

    char n[2];
    if (bulk_read(qfd, &n[0], sizeof(char)) < 0)
        ERR("read");
    n[1] = '\0';
    int questions_number = atoi(n);

    // read new line character
    if (bulk_read(qfd, &n[0], 1) < 0)
        ERR("read");

    for (int i = 0; i < questions_number; i++)
    {
        int t = 0;
        char c;
        do
        {
            if (bulk_read(qfd, &c, sizeof(char)) < 0)
                ERR("read");

            questions[i][t++] = c;
        } while (c != ';');
        questions[i][t - 2] = '\0';
    }

    *questions_count = questions_number;
}

int main(int argc, char **argv)
{
    if (argc != 5)
        usage();

    char *address = argv[1];
    uint16_t port = atoi(argv[2]);
    uint16_t max_clients = atoi(argv[3]);
    char *file_path = argv[4];
    int fdTCP;

    int questions_count;
    char questions[MAX_QUESTIONS][QUESTION_MAXLENGTH];
    read_questions(file_path, questions, &questions_count);

    sethandler(sigusr1_handler, SIGUSR1);
    sethandler(sigint_handler, SIGINT);
    sethandler(SIG_IGN, SIGPIPE);

    fdTCP = bind_inet_socket(port, address, SOCK_STREAM);

    int new_flags = fcntl(fdTCP, F_GETFL) | O_NONBLOCK;
    fcntl(fdTCP, F_SETFL, new_flags);

    doServer(fdTCP, max_clients, questions, questions_count);

    if (TEMP_FAILURE_RETRY(close(fdTCP)) < 0)
        ERR("close");

    return EXIT_SUCCESS;
}
