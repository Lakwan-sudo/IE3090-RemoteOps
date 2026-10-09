#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define PORT 9410
#define SID "SID:1573"
#define AUTH_TOKEN "OPS-3751"
#define BUFFER_SIZE 4096
#define BACKLOG 10

/* Send all bytes */
int send_all(int socket_fd, const char *data, size_t length)
{
    size_t total_sent = 0;

    while (total_sent < length)
    {
        ssize_t sent = send(socket_fd,
                            data + total_sent,
                            length - total_sent,
                            0);

        if (sent <= 0)
            return -1;

        total_sent += (size_t)sent;
    }

    return 0;
}

/* Receive one newline-terminated line */
int recv_line(int socket_fd, char *buffer, size_t buffer_size)
{
    size_t position = 0;

    if (buffer_size < 2)
        return -1;

    while (position < buffer_size - 1)
    {
        char character;
        ssize_t received = recv(socket_fd, &character, 1, 0);

        if (received <= 0)
            return -1;

        if (character == '\n')
        {
            buffer[position] = '\0';
            return 0;
        }

        if (character != '\r')
            buffer[position++] = character;
    }

    buffer[position] = '\0';
    return -1;
}

/* Execute only approved commands */
int handle_exec(int client_fd, const char *requested)
{
    const char *shell_command = NULL;

    if (strcmp(requested, "DATE") == 0)
        shell_command = "date";
    else if (strcmp(requested, "UPTIME") == 0)
        shell_command = "uptime";
    else if (strcmp(requested, "DISKFREE") == 0)
        shell_command = "df -h";
    else if (strcmp(requested, "HOSTNAME") == 0)
        shell_command = "hostname";
    else if (strcmp(requested, "WHOAMI") == 0)
        shell_command = "whoami";

    if (shell_command == NULL)
    {
        const char *response =
            "ERR EXEC_NOT_ALLOWED " SID "\n";

        return send_all(client_fd, response, strlen(response));
    }

    FILE *output = popen(shell_command, "r");

    if (output == NULL)
    {
        const char *response = "ERR EXEC_FAILED " SID "\n";
        return send_all(client_fd, response, strlen(response));
    }

    char response[BUFFER_SIZE];

    snprintf(response, sizeof(response),
             "OK EXEC %s %s\n", requested, SID);

    if (send_all(client_fd, response, strlen(response)) < 0)
    {
        pclose(output);
        return -1;
    }

    char line[BUFFER_SIZE];

    while (fgets(line, sizeof(line), output) != NULL)
    {
        if (send_all(client_fd, line, strlen(line)) < 0)
        {
            pclose(output);
            return -1;
        }
    }

    int result = pclose(output);

    if (result == -1)
    {
        const char *response_error = "ERR EXEC_FAILED " SID "\n";
        return send_all(client_fd, response_error,
                        strlen(response_error));
    }

    const char *end_marker = "END EXEC " SID "\n";
    return send_all(client_fd, end_marker, strlen(end_marker));
}

/* Handle controller commands */
int handle_command(int client_fd, const char *command)
{
    /* SYSINFO */
    if (strcmp(command, "SYSINFO") == 0)
    {
        char response[BUFFER_SIZE];

        snprintf(response, sizeof(response),
                 "OK SYSINFO Host:localhost %s\n", SID);

        return send_all(client_fd, response, strlen(response));
    }

    /* LISTPROC */
    else if (strcmp(command, "LISTPROC") == 0)
    {
        FILE *process_file = popen("ps -eo pid,comm", "r");

        if (process_file == NULL)
        {
            const char *response = "ERR LISTPROC_FAILED " SID "\n";
            return send_all(client_fd, response, strlen(response));
        }

        const char *header = "OK LISTPROC " SID "\n";

        if (send_all(client_fd, header, strlen(header)) < 0)
        {
            pclose(process_file);
            return -1;
        }

        char line[BUFFER_SIZE];

        while (fgets(line, sizeof(line), process_file) != NULL)
        {
            if (send_all(client_fd, line, strlen(line)) < 0)
            {
                pclose(process_file);
                return -1;
            }
        }

        pclose(process_file);

        const char *end_marker = "END LISTPROC " SID "\n";
        return send_all(client_fd, end_marker, strlen(end_marker));
    }

    /* EXEC */
    else if (strncmp(command, "EXEC ", 5) == 0)
    {
        return handle_exec(client_fd, command + 5);
    }

    /* QUIT */
    else if (strcmp(command, "QUIT") == 0)
    {
        const char *response = "OK BYE " SID "\n";
        return send_all(client_fd, response, strlen(response));
    }

    /* Unknown command */
    else
    {
        const char *response = "ERR UNKNOWN_COMMAND " SID "\n";
        return send_all(client_fd, response, strlen(response));
    }
}

/* Handle one authenticated controller connection */
void handle_client(int client_fd)
{
    char buffer[BUFFER_SIZE];

    if (recv_line(client_fd, buffer, sizeof(buffer)) < 0)
        return;

    printf("Received from controller: %s\n", buffer);

    if (strcmp(buffer, "AUTH " AUTH_TOKEN) == 0)
    {
        const char *response = "OK AUTHENTICATED " SID "\n";

        if (send_all(client_fd, response, strlen(response)) < 0)
            return;

        printf("Authentication successful\n");
    }
    else
    {
        const char *response = "ERR AUTH REQUIRED " SID "\n";

        send_all(client_fd, response, strlen(response));
        printf("Authentication failed\n");
        return;
    }

    while (1)
    {
        if (recv_line(client_fd, buffer, sizeof(buffer)) < 0)
        {
            printf("Controller disconnected\n");
            break;
        }

        printf("Command received: %s\n", buffer);

        if (strcmp(buffer, "QUIT") == 0)
        {
            handle_command(client_fd, buffer);
            printf("Controller requested disconnect\n");
            break;
        }

        if (handle_command(client_fd, buffer) < 0)
        {
            printf("Failed to send response\n");
            break;
        }
    }
}

/* Thread function for each controller */
void *client_thread(void *argument)
{
    int client_fd = *(int *)argument;
    free(argument);

    printf("Thread started for client socket %d\n", client_fd);

    handle_client(client_fd);

    close(client_fd);

    printf("Thread finished for client socket %d\n", client_fd);
    return NULL;
}

int main(void)
{
    signal(SIGPIPE, SIG_IGN);

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);

    if (server_fd < 0)
    {
        perror("socket");
        return 1;
    }

    int option = 1;

    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR,
                   &option, sizeof(option)) < 0)
    {
        perror("setsockopt");
        close(server_fd);
        return 1;
    }

    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));

    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    server_addr.sin_port = htons(PORT);

    if (bind(server_fd, (struct sockaddr *)&server_addr,
             sizeof(server_addr)) < 0)
    {
        perror("bind");
        close(server_fd);
        return 1;
    }

    if (listen(server_fd, BACKLOG) < 0)
    {
        perror("listen");
        close(server_fd);
        return 1;
    }

    printf("\n");
    printf("====================================\n");
    printf("      RemoteOps Agent Started\n");
    printf("====================================\n");
    printf("TCP Port : %d\n", PORT);
    printf("SID      : %s\n", SID);
    printf("Auth     : %s\n", AUTH_TOKEN);
    printf("Mode     : Multi-threaded\n");
    printf("====================================\n\n");

    while (1)
    {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);

        int client_fd = accept(server_fd,
                               (struct sockaddr *)&client_addr,
                               &client_len);

        if (client_fd < 0)
        {
            perror("accept");
            continue;
        }

        printf("Controller connected: %s:%d\n",
               inet_ntoa(client_addr.sin_addr),
               ntohs(client_addr.sin_port));

        int *client_socket = malloc(sizeof(int));

        if (client_socket == NULL)
        {
            perror("malloc");
            close(client_fd);
            continue;
        }

        *client_socket = client_fd;

        pthread_t thread_id;

        if (pthread_create(&thread_id, NULL,
                           client_thread, client_socket) != 0)
        {
            fprintf(stderr, "pthread_create failed\n");
            close(client_fd);
            free(client_socket);
            continue;
        }

        pthread_detach(thread_id);
        printf("New client thread created\n");
    }

    close(server_fd);
    return 0;
