/*----------------------------------------------
Serveur à lancer avant le client
------------------------------------------------*/
#include <netinet/in.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "othello.h"

struct othello_player_s;
struct othello_room_s;

struct othello_player_s {
    pthread_t thread;
    int socket;
    char name[32];/*define macro*/
    struct othello_room_s * room;
    pthread_mutex_t mutex;/*useless ? Must think about it. (read solve the problem)*/
    bool ready;
    enum othello_state_e state;
};

struct othello_room_s {
    struct othello_player_s * players[2];/*define macro*/
    pthread_mutex_t mutex;
    char othellier[OTHELLO_BOARD_LENGTH][OTHELLO_BOARD_LENGTH];
};

typedef struct othello_player_s othello_player_t;
typedef struct othello_room_s othello_room_t;

static othello_room_t rooms[OTHELLO_NUMBER_OF_ROOMS];

/**
 * \return the result of the last call to read
 */
ssize_t othello_read_all(int fd, void * buf, size_t count) {
    ssize_t bytes_read;

    while((bytes_read = read(fd, buf, count)) > 0 && count > 0) {
        count -= bytes_read;
        buf += bytes_read;
    }

    return bytes_read;
}

/**
 * \return the result of the last call to write
 */
ssize_t othello_write_all(int fd, void * buf, size_t count) {
    ssize_t bytes_write;

    while((bytes_write = write(fd, buf, count)) > 0 && count > 0) {
        count -= bytes_write;
        buf += bytes_write;
    }

    return bytes_write;
}

int othello_connect(othello_player_t * player) {
    /*read message length*/
    unsigned char name_length;
    unsigned char reply[2];

    othello_read_all(player->socket, &name_length, 1);
    /*TODO: check length + check read*/
    /*read message*/
    othello_read_all(player->socket, player->name, name_length);
    /*check player state*/
    /*if player state and player name ok then update player state*/
    /*else send error ?*/

    reply[0] = OTHELLO_QUERY_CONNECT;
    reply[1] = OTHELLO_SUCCESS;

    pthread_mutex_lock(&(player->mutex));
    othello_write_all(player->socket, reply, 2);
    pthread_mutex_unlock(&(player->mutex));

    return 0;
}

int othello_list_room(othello_player_t * player) {
    return 0;
}

int othello_join_room(othello_player_t * player) {
    unsigned char room_id;
    char reply[2];
    int i;

    reply[0] = OTHELLO_QUERY_JOIN_ROOM;
    reply[1] = OTHELLO_ROOM_UNKNOWN_ERROR;

    othello_read_all(player->socket, &room_id, 1);

    if(room_id >= 0 && room_id < OTHELLO_NUMBER_OF_ROOMS) {
        reply[1] = OTHELLO_ROOM_FULL_ERROR;

        pthread_mutex_lock(&(rooms[room_id].mutex));
        for(i = 0; i < 2; i++) {
            if(rooms[room_id].players[i] == NULL) {
                rooms[room_id].players[i] = player;
                player->room = &(rooms[room_id]);
                reply[1] = OTHELLO_SUCCESS;
                break;
            }
        }
        pthread_mutex_unlock(&(rooms[room_id].mutex));
    }

    pthread_mutex_lock(&(player->mutex));
    othello_write_all(player->socket, reply, 2);
    pthread_mutex_unlock(&(player->mutex));

    return 0;
}

int othello_leave_room(othello_player_t * player) {
    char reply[2];
    int i;

    reply[0] = OTHELLO_QUERY_LEAVE_ROOM;
    reply[1] = OTHELLO_SUCCESS;

    /*TODO: check player/room*/

    pthread_mutex_lock(&(player->room->mutex));
    for(i = 0; i < 2; i++) {
        if(player->room->players[i] == player) {
            player->room->players[i] = NULL;
            break;
        }
    }
    pthread_mutex_unlock(&(player->room->mutex));
    player->room = NULL;

    pthread_mutex_lock(&(player->mutex));
    othello_write_all(player->socket, reply, 2);
    pthread_mutex_unlock(&(player->mutex));

    return 0;
}

int othello_send_message(othello_player_t * player) {
    return 0;
}

int othello_play_turn(othello_player_t * player) {
    return 0;
}

void * othello_start(void * player) {
    char query_code;

    while(othello_read_all(((othello_player_t*)player)->socket, &query_code, 1) > 0) {
        /*switch over query code and player state*/
        switch(query_code) {
            case OTHELLO_QUERY_CONNECT:
                othello_connect(player);
                break;
            default: break; /*error*/
        }
    }

    /*leave room if player in one*/
    close(((othello_player_t*)player)->socket);
    free(player);

    return NULL;
}

int othello_create_socket_stream(unsigned short port) {
    int sock;
    struct sockaddr_in address;

    if((sock = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        perror("socket");
        return -1;
    }

    memset(&address, 0, sizeof(struct sockaddr_in));
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_ANY);

    if(bind(sock, (struct sockaddr *) & address, sizeof(struct sockaddr_in)) < 0) {
        close(sock);
        perror("bind");
        return -1;
    }

    return sock;
}

int main(int argc, char * argv[]) {
    int sock, i;
    othello_player_t * player;

    memset(rooms, 0, sizeof(othello_room_t) * OTHELLO_NUMBER_OF_ROOMS);
    for(i = 0; i < OTHELLO_NUMBER_OF_ROOMS; i++) {
        if(pthread_mutex_init(&(rooms[i].mutex), NULL)) {
            perror("pthread_mutex_init");
            return 1;
        }
    }


    if((sock = othello_create_socket_stream(5000)) < 0) {
        perror("othello_create_socket_stream");
        return 1;
    }

    listen(sock, 5);

    for(;;) {
        if((player = malloc(sizeof(othello_player_t))) == NULL) {
            perror("malloc");
            return 1;
        }

        memset(player, 0, sizeof(othello_player_t));

        if ((player->socket = accept(sock, NULL, NULL)) < 0) {
            perror("accept");
            return 1;
        }

        if(pthread_mutex_init(&(player->mutex), NULL)) {
            perror("pthread_mutex_init");
            return 1;
        }

        if(pthread_create(&(player->thread), NULL, othello_start, player)) {
            perror("pthread_create");
            return 1;
        }
    }

    close(sock);

    return 0;
}
