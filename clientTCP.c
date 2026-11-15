#include <stdio.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdlib.h>
#include <unistd.h>
#include <netdb.h>

#include <string.h>

#define FTP_READY       220
#define FTP_NEED_PASS   331
#define FTP_LOGIN_OK    230
#define FTP_PASV_OK     227
#define FTP_DATA_OPEN   125
#define FTP_START_TRANS 150
#define FTP_TRANS_OK    226
#define FTP_QUIT        221

#define FTP_PORT 21

//strchr é tudo que vem asseguir do delimitador
//sprintf é em vez de dar print na consola, dá print na string

struct URL {
    char user[256];
    char password[256];
    char host[256];
    char path[512];
    char filename[256];
};

int parseFTPUrl(const char *url, struct URL *u) {
    if (strncmp(url, "ftp://", 6) != 0) {
        fprintf(stderr, "Invalid Url: must start with ftp://\n");
        return -1;
    }

    strcpy(u->user, "anonymous");
    strcpy(u->password, "anonymous");
    u->host[0] = '\0';
    u->path[0] = '\0';
    u->filename[0] = '\0';
    
    if (strchr(url, '@') != NULL) {
        if (sscanf(url, "ftp://%255[^:]:%255[^@]@%255[^/]/%511s", 
                   u->user, u->password, u->host, u->path) != 4) {
            fprintf(stderr, "Invalid Url with credentials\n");
            return -1;
        }
    } else {
        if (sscanf(url, "ftp://%255[^/]/%511s", 
                   u->host, u->path) != 2) {
            fprintf(stderr, "Invalid Url: ftp://<host>/<path>\n");
            return -1;
        }
    }

    char *last_slash = strrchr(u->path, '/');
    if (last_slash != NULL) {
        strcpy(u->filename, last_slash + 1);
    } else {
        strcpy(u->filename, u->path);
    }
    return 0;
}

int readFTPResponse(int sockfd, char *response, size_t max_len) {
    size_t total = 0;
    char line[1024];
    int code = 0;

    if (max_len > 0) response[0] = '\0';

    while (1) {
        size_t line_len = 0;
        char c;
        while (read(sockfd, &c, 1) > 0) {
            if (line_len < sizeof(line) - 1) {
                line[line_len++] = c;
            }
            if (c == '\n') break;
        }
        if (line_len == 0) break;
        line[line_len] = '\0';

        printf("%s\n",line); // só para debug

        if (total + line_len < max_len) {
            strcpy(response + total, line);
            total += line_len;
        }

        // Resposta FTP completa termina com 3 dígitos seguidos de um espaço ' '
        if (line_len >= 4 && 
            line[0] >= '0' && line[0] <= '9' &&
            line[1] >= '0' && line[1] <= '9' &&
            line[2] >= '0' && line[2] <= '9' &&
            line[3] == ' ') {
            
            code = atoi(line);
            break;
        }
    }

    return code;
}

int main(int argc, char **argv) {

    if (argc != 2) {
        printf("Usage: %s ftp://[<user>:<password>@]<host>/<url-path>\n", argv[0]);
        return -1;
    }
    
    struct URL url_data;
    if (parseFTPUrl(argv[1], &url_data) < 0) {
        return -1;
    }

    // só para debug
    printf("%s \n", url_data.user);
    printf("%s \n", url_data.password);
    printf("%s \n", url_data.path);
    printf("%s \n", url_data.host);
    
    int sockfd, sockfd1;
    struct sockaddr_in server_addr;
    char buf[1024];

    struct hostent *h;
    if ((h = gethostbyname(url_data.host)) == NULL) {
        herror("gethostbyname()");
        exit(-1);
    }

    bzero((char *) &server_addr, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = inet_addr(inet_ntoa(*((struct in_addr *) h->h_addr)));
    server_addr.sin_port = htons(FTP_PORT);       

    /*open a TCP socket*/
    if ((sockfd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        perror("socket()");
        exit(-1);
    }
    /*connect to the server*/
    if (connect(sockfd, (struct sockaddr *) &server_addr, sizeof(server_addr)) < 0) {
        perror("connect()");
        exit(-1);
    }
    
    enum State {START, USER, PASS, PASV, START_TRANS, RCV, QUIT, DONE};
    enum State state = START;

    char cmd[1024];
    int code;

    while (state != DONE) { 

        code = readFTPResponse(sockfd, buf, sizeof(buf));
        if (code == 0) continue;
        if (code < 0) break;

        switch (state) {
            case START:
                if (code == FTP_READY) {
                    sprintf(cmd, "USER %s\r\n", url_data.user);
                    write(sockfd, cmd, strlen(cmd));
                    state = USER;
                }
                break;

            case USER:
                if (code == FTP_NEED_PASS) {
                    sprintf(cmd, "PASS %s\r\n", url_data.password);
                    write(sockfd, cmd, strlen(cmd));
                    state = PASS;
                } else if (code == FTP_LOGIN_OK) { 
                    sprintf(cmd, "PASV\r\n");
                    write(sockfd, cmd, strlen(cmd));
                    state = PASV;
                }
                break;

            case PASS:
                if (code == FTP_LOGIN_OK) {
                    sprintf(cmd, "PASV\r\n");
                    write(sockfd, cmd, strlen(cmd));
                    state = PASV;
                }
                break;

            case PASV:
                if (code == FTP_PASV_OK) {
                    char *start = strchr(buf, '(');
                    if (start != NULL) {
                        int ip1, ip2, ip3, ip4, port1, port2;
                        if (sscanf(start, "(%d,%d,%d,%d,%d,%d)", 
                                   &ip1, &ip2, &ip3, &ip4, &port1, &port2) == 6) {
                            
                            char data_ip[64];
                            sprintf(data_ip, "%d.%d.%d.%d", ip1, ip2, ip3, ip4);

                            int data_port = port1 * 256 + port2;
            
                            struct sockaddr_in server_addr1;

                            bzero((char *) &server_addr1, sizeof(server_addr1));
                            server_addr1.sin_family = AF_INET;
                            server_addr1.sin_addr.s_addr = inet_addr(data_ip);
                            server_addr1.sin_port = htons(data_port);

                            if ((sockfd1 = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
                                perror("socket()");
                                exit(-1);
                            }
                            
                            if (connect(sockfd1, (struct sockaddr *) &server_addr1, sizeof(server_addr1)) < 0) {
                                perror("connect()");
                                exit(-1);
                            }

                            sprintf(cmd, "RETR %s\r\n", url_data.path);
                            write(sockfd, cmd, strlen(cmd));

                            state = START_TRANS;
                        }
                    }
                }
                break;
            case START_TRANS:
                if (code == FTP_START_TRANS || code == FTP_DATA_OPEN) {
                    FILE *fp = fopen(url_data.filename, "wb");
                    if (fp == NULL) {
                        perror("fopen()");
                        exit(-1);
                    }
                    
                    char file_buf[1024];
                    int bytes_file;
                    while ((bytes_file = read(sockfd1, file_buf, sizeof(file_buf))) > 0) {
                        fwrite(file_buf, 1, bytes_file, fp);
                    }
                    
                    fclose(fp);
                    close(sockfd1);
                    printf("File saved!\n");
                    
                    state = RCV; 
                }
                
                break;
            case RCV:
                if (code == FTP_TRANS_OK){
                    sprintf(cmd, "QUIT\r\n");
                    write(sockfd, cmd, strlen(cmd));
                    state = QUIT;
                }
                break;
            case QUIT:
                if (code == FTP_QUIT) state = DONE;
                break;
            case DONE:
                break;
                
        }
    }

    if (close(sockfd)<0) {
        perror("close()");
        exit(-1);
    }
    return 0;
}


