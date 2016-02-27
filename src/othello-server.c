/**
 * \author Alexis Giraudet
 * gcc -ansi -Wall -pedantic -o server src/othello-server.c -lpthread
 * clang -ansi -Weverything -o server src/othello-server.c -lpthread
 */

#include "othello.h"
#include "othello-server.h"

#ifdef OTHELLO_WITH_SYSLOG
#define _BSD_SOURCE
#endif

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <syslog.h>
#include <unistd.h>

struct othello_player_s {
  pthread_t thread;
  int socket;
  char name[OTHELLO_PLAYER_NAME_LENGTH];
  othello_room_t *room;
  pthread_mutex_t mutex;
  bool ready;
  enum othello_state_e state;
};

struct othello_room_s {
  othello_player_t *players[OTHELLO_ROOM_LENGTH];
  pthread_mutex_t mutex;
  char grid[OTHELLO_BOARD_LENGTH][OTHELLO_BOARD_LENGTH];
};

static othello_room_t rooms[OTHELLO_NUMBER_OF_ROOMS];
static int sock;
#ifndef OTHELLO_WITH_SYSLOG
static pthread_mutex_t log_mutex;
#endif

/**
 * \return the result of the last call to read
 */
ssize_t othello_read_all(int fd, void *buf, size_t count) {
  ssize_t bytes_read = 0;
  char *cursor = buf;

  while (count > 0 && (bytes_read = read(fd, cursor, count)) > 0) {
    count -= bytes_read;
    cursor += bytes_read;
  }

  return bytes_read;
}

/**
 * \return the result of the last call to write
 */
ssize_t othello_write_all(int fd, void *buf, size_t count) {
  ssize_t bytes_write = 0;
  char *cursor = buf;

  while (count > 0 && (bytes_write = write(fd, cursor, count)) > 0) {
    count -= bytes_write;
    cursor += bytes_write;
  }

  return bytes_write;
}

void othello_log(int priority, const char *format, ...) {
  va_list ap;

  va_start(ap, format);
#ifdef OTHELLO_WITH_SYSLOG
  vsyslog(priority, format, ap);
#else
  pthread_mutex_lock(&log_mutex);
  switch (priority) {
  case LOG_EMERG:
    fputs("EMERGENCY ", stdout);
    break;
  case LOG_ALERT:
    fputs("ALERT     ", stdout);
    break;
  case LOG_CRIT:
    fputs("CRITICAL  ", stdout);
    break;
  case LOG_ERR:
    fputs("ERROR     ", stdout);
    break;
  case LOG_WARNING:
    fputs("WARNING   ", stdout);
    break;
  case LOG_NOTICE:
    fputs("NOTICE    ", stdout);
    break;
  case LOG_INFO:
    fputs("INFO      ", stdout);
    break;
  case LOG_DEBUG:
    fputs("DEBUG     ", stdout);
    break;
  default:
    fputs("          ", stdout);
    break;
  }
  vprintf(format, ap);
  putc('\n', stdout);
  pthread_mutex_unlock(&log_mutex);
#endif
}

void othello_end(othello_player_t *player) {
  int i;

  othello_log(LOG_INFO, "%p end", player);

  /*leave room*/
  /*pthread_mutex_lock(&(player->mutex));*/

  if (player->room != NULL) {
    pthread_mutex_lock(&(player->room->mutex));
    for (i = 0; i < OTHELLO_ROOM_LENGTH; i++) {
      if (player->room->players[i] == player) {
        player->room->players[i] = NULL;
        break;
      }
    }
    pthread_mutex_unlock(&(player->room->mutex));
  }

  /*TODO: manage if player in game*/

  /*destroy mutex*/
  /*pthread_mutex_unlock(&(player->mutex));*/
  pthread_mutex_destroy(&(player->mutex));
  close(player->socket);
  /*free memory*/
  free(player);
  /*exit thread*/
  /*pthread_exit(NULL);*/
}

int othello_handle_connect(othello_player_t *player) {
  char reply[2];
  int status;

  othello_log(LOG_INFO, "%p connect #1", player);

  reply[0] = OTHELLO_QUERY_CONNECT;
  reply[1] = OTHELLO_FAILURE;

  status = OTHELLO_SUCCESS;

  if (othello_read_all(player->socket, player->name,
                       OTHELLO_PLAYER_NAME_LENGTH) <= 0) {
    status = OTHELLO_FAILURE;
  }

  if (status == OTHELLO_SUCCESS &&
      player->state == OTHELLO_STATE_NOT_CONNECTED &&
      player->name[0] != '\0' /*TODO: check protocol version*/) {
    reply[1] = OTHELLO_SUCCESS;
    player->state = OTHELLO_STATE_CONNECTED;
  }

  pthread_mutex_lock(&(player->mutex));
  if (othello_write_all(player->socket, reply, sizeof(reply)) <= 0) {
    status = OTHELLO_FAILURE;
  }
  pthread_mutex_unlock(&(player->mutex));

  othello_log(LOG_INFO, "%p connect #2", player);

  return status;
}

/*
 * | room id (1 byte) | players name (OTHELLO_PLAYER_NAME_LENGTH) *
 * OTHELLO_ROOM_LENGTH | number of players in the room (1 byte) | *
 * OTHELLO_NUMBER_OF_ROOMS
 */
int othello_handle_room_list(othello_player_t *player) {
  char reply[1 +
             (2 + OTHELLO_ROOM_LENGTH * OTHELLO_PLAYER_NAME_LENGTH) *
                 OTHELLO_NUMBER_OF_ROOMS];
  char *reply_cursor;
  char room_id;
  char room_size;
  othello_player_t **players_cursor;
  othello_room_t *room_cursor;
  int status;

  othello_log(LOG_INFO, "%p room list", player);

  status = OTHELLO_SUCCESS;

  memset(reply, 0, sizeof(reply));
  reply[0] = OTHELLO_QUERY_ROOM_LIST;

  reply_cursor = reply + 1;
  room_id = 0;
  for (room_cursor = rooms; room_cursor < rooms + OTHELLO_NUMBER_OF_ROOMS;
       room_cursor++) {
    *reply_cursor = room_id;
    reply_cursor++;
    room_size = 0;
    pthread_mutex_lock(&(room_cursor->mutex));
    for (players_cursor = room_cursor->players;
         players_cursor < room_cursor->players + OTHELLO_ROOM_LENGTH;
         players_cursor++) {
      if (*players_cursor != NULL) {
        room_size++;
        memcpy(reply_cursor, (*players_cursor)->name,
               OTHELLO_PLAYER_NAME_LENGTH);
      }
      reply_cursor += OTHELLO_PLAYER_NAME_LENGTH;
    }
    pthread_mutex_unlock(&(room_cursor->mutex));
    *reply_cursor = room_size;
    reply_cursor++;
    room_id++;
  }

  pthread_mutex_lock(&(player->mutex));
  if (othello_write_all(player->socket, reply, sizeof(reply)) <= 0) {
    status = OTHELLO_FAILURE;
  }
  pthread_mutex_unlock(&(player->mutex));

  return status;
}

int othello_handle_room_join(othello_player_t *player) {
  unsigned char room_id;
  char reply[2];
  char notif[1 + OTHELLO_PLAYER_NAME_LENGTH];
  int status;
  othello_room_t *room;
  othello_player_t **players_cursor;

  othello_log(LOG_INFO, "%p room join", player);

  reply[0] = OTHELLO_QUERY_ROOM_JOIN;
  reply[1] = OTHELLO_FAILURE;

  notif[0] = OTHELLO_NOTIF_ROOM_JOIN;

  status = OTHELLO_SUCCESS;

  othello_log(LOG_INFO, "%p room join #1", player);

  if (othello_read_all(player->socket, &room_id, sizeof(room_id)) <= 0) {
    status = OTHELLO_FAILURE;
  }

  if (status == OTHELLO_SUCCESS && player->room == NULL &&
      room_id < OTHELLO_NUMBER_OF_ROOMS &&
      player->state == OTHELLO_STATE_CONNECTED) {

    room = &(rooms[room_id]);

    pthread_mutex_lock(&(room->mutex));
    for (players_cursor = room->players;
         players_cursor < room->players + OTHELLO_ROOM_LENGTH;
         players_cursor++) {
      if (*players_cursor == NULL) {
        *players_cursor = player;
        player->room = room;
        player->state = OTHELLO_STATE_IN_ROOM;
        player->ready = false;
        reply[1] = OTHELLO_SUCCESS;
        break;
      }
    }
    if (player->room != NULL) {
      memcpy(notif + 1, player->name, OTHELLO_PLAYER_NAME_LENGTH);

      for (players_cursor = room->players;
           players_cursor < room->players + OTHELLO_ROOM_LENGTH;
           players_cursor++) {
        if (*players_cursor != NULL && *players_cursor != player) {
          pthread_mutex_lock(&((*players_cursor)->mutex));
          othello_write_all((*players_cursor)->socket, notif, sizeof(notif));
          pthread_mutex_unlock(&((*players_cursor)->mutex));
        }
      }
    }
    pthread_mutex_unlock(&(room->mutex));
  }

  pthread_mutex_lock(&(player->mutex));
  if (othello_write_all(player->socket, reply, sizeof(reply)) <= 0) {
    status = OTHELLO_FAILURE;
  }
  pthread_mutex_unlock(&(player->mutex));

  othello_log(LOG_INFO, "%p room join #2", player);

  return status;
}

int othello_handle_room_leave(othello_player_t *player) {
  char reply[2];
  char notif[1 + OTHELLO_PLAYER_NAME_LENGTH];
  int status;
  othello_player_t **players_cursor;

  othello_log(LOG_INFO, "%p room leave #1", player);

  reply[0] = OTHELLO_QUERY_ROOM_LEAVE;
  reply[1] = OTHELLO_FAILURE;

  notif[0] = OTHELLO_NOTIF_ROOM_LEAVE;

  status = OTHELLO_SUCCESS;

  if (player->room != NULL && player->state == OTHELLO_STATE_IN_ROOM) {
    memcpy(notif + 1, player->name, OTHELLO_PLAYER_NAME_LENGTH);

    pthread_mutex_lock(&(player->room->mutex));
    for (players_cursor = player->room->players;
         players_cursor < player->room->players + OTHELLO_ROOM_LENGTH;
         players_cursor++) {
      if (*players_cursor == player) {
        *players_cursor = NULL;
      } else if (*players_cursor != NULL) {
        pthread_mutex_lock(&((*players_cursor)->mutex));
        othello_write_all((*players_cursor)->socket, notif, sizeof(notif));
        pthread_mutex_unlock(&((*players_cursor)->mutex));
      }
    }
    pthread_mutex_unlock(&(player->room->mutex));

    player->room = NULL;
    player->state = OTHELLO_STATE_CONNECTED;

    reply[1] = OTHELLO_SUCCESS;
  }

  pthread_mutex_lock(&(player->mutex));
  if (othello_write_all(player->socket, reply, sizeof(reply)) <= 0) {
    status = OTHELLO_FAILURE;
  }
  pthread_mutex_unlock(&(player->mutex));

  othello_log(LOG_INFO, "%p room leave #2", player);

  return status;
}

int othello_handle_message(othello_player_t *player) {
  char reply[2];
  char notif[1 + OTHELLO_PLAYER_NAME_LENGTH + OTHELLO_MESSAGE_LENGTH];
  othello_player_t **players_cursor;
  int status;

  othello_log(LOG_INFO, "%p message #1", player);

  reply[0] = OTHELLO_QUERY_MESSAGE;
  reply[1] = OTHELLO_FAILURE;

  notif[0] = OTHELLO_NOTIF_MESSAGE;
  memcpy(notif + 1, player->name, OTHELLO_PLAYER_NAME_LENGTH);

  status = OTHELLO_SUCCESS;

  if (othello_read_all(player->socket, notif + 1 + OTHELLO_PLAYER_NAME_LENGTH,
                       OTHELLO_MESSAGE_LENGTH) <= 0) {
    status = OTHELLO_FAILURE;
  }

  if (status == OTHELLO_SUCCESS && player->room != NULL) {
    reply[1] = OTHELLO_SUCCESS;

    pthread_mutex_lock(&(player->room->mutex));
    for (players_cursor = player->room->players;
         players_cursor < player->room->players + OTHELLO_ROOM_LENGTH;
         players_cursor++) {
      if (*players_cursor != NULL && *players_cursor != player) {
        pthread_mutex_lock(&((*players_cursor)->mutex));
        othello_write_all((*players_cursor)->socket, notif, sizeof(notif));
        pthread_mutex_unlock(&((*players_cursor)->mutex));
      }
    }
    pthread_mutex_unlock(&(player->room->mutex));
  }

  pthread_mutex_lock(&(player->mutex));
  if (othello_write_all(player->socket, reply, sizeof(reply)) <= 0) {
    status = OTHELLO_FAILURE;
  }
  pthread_mutex_unlock(&(player->mutex));

  othello_log(LOG_INFO, "%p message #2", player);

  return status;
}

int othello_handle_ready(othello_player_t *player) {
  char reply[2];
  char notif_ready[2 + OTHELLO_PLAYER_NAME_LENGTH];
  char notif_start[2];
  char ready;
  int status;
  othello_player_t **players_cursor;
  bool room_ready;

  othello_log(LOG_INFO, "%p ready", player);

  reply[0] = OTHELLO_QUERY_READY;
  reply[1] = OTHELLO_FAILURE;

  notif_ready[0] = OTHELLO_NOTIF_READY;
  notif_start[0] = OTHELLO_NOTIF_GAME_START;

  status = OTHELLO_SUCCESS;

  if (othello_read_all(player->socket, &ready, sizeof(ready)) <= 0) {
    status = OTHELLO_FAILURE;
  }

  if (status == OTHELLO_SUCCESS && player->room != NULL &&
      player->state == OTHELLO_STATE_IN_ROOM) {
    reply[1] = OTHELLO_SUCCESS;
    notif_ready[1] = ready;
    memcpy(notif_ready + 2, player->name, OTHELLO_PLAYER_NAME_LENGTH);

    if (ready) {
      player->ready = true;
    } else {
      player->ready = false;
    }

    room_ready = player->ready;

    pthread_mutex_lock(&(player->room->mutex));
    for (players_cursor = player->room->players;
         players_cursor < player->room->players + OTHELLO_ROOM_LENGTH;
         players_cursor++) {
      room_ready = room_ready && (*players_cursor)->ready;
      pthread_mutex_lock(&((*players_cursor)->mutex));
      othello_write_all((*players_cursor)->socket, notif_ready,
                        sizeof(notif_ready));
      pthread_mutex_unlock(&((*players_cursor)->mutex));
    }

    if (room_ready) {
      memset(player->room->grid, 0, sizeof(player->room->grid));

      for (players_cursor = player->room->players;
           players_cursor < player->room->players + OTHELLO_ROOM_LENGTH;
           players_cursor++) {
        if (players_cursor == player->room->players) {
          notif_start[1] = true; /* first player of the room start to play */
        } else {
          notif_start[1] = false;
          (*players_cursor)->ready = false;
        }
        (*players_cursor)->state = OTHELLO_STATE_IN_GAME;
        pthread_mutex_lock(&((*players_cursor)->mutex));
        othello_write_all((*players_cursor)->socket, notif_start,
                          sizeof(notif_start));
        pthread_mutex_unlock(&((*players_cursor)->mutex));
      }
    }
    pthread_mutex_unlock(&(player->room->mutex));
  }

  pthread_mutex_lock(&(player->mutex));
  if (othello_write_all(player->socket, &reply, sizeof(reply)) <= 0) {
    status = OTHELLO_FAILURE;
  }
  pthread_mutex_unlock(&(player->mutex));

  return status;
}

int othello_handle_play(othello_player_t *player) {
  unsigned char stroke[2];
  char reply[2];
  char notif_play[3];
  char notif_end[2];
  int status;
  othello_player_t **players_cursor;
  othello_player_t **player_next;

  othello_log(LOG_INFO, "%p play", player);

  reply[0] = OTHELLO_QUERY_READY;
  reply[1] = OTHELLO_FAILURE;

  notif_play[0] = OTHELLO_NOTIF_PLAY;
  notif_end[0] = OTHELLO_NOTIF_GAME_END;

  status = OTHELLO_SUCCESS;

  if (othello_read_all(player->socket, stroke, sizeof(stroke)) <= 0) {
    status = OTHELLO_FAILURE;
  }

  /*TODO: check/valid stroke and check if game is over + notify*/
  if (status == OTHELLO_SUCCESS && player->state == OTHELLO_STATE_IN_GAME &&
      player->ready
      /*check if stroke is valid*/
      /*player->room != null*/) {
    reply[1] = OTHELLO_SUCCESS;
    player->ready = false;

    notif_play[1] = reply[0];
    notif_play[2] = reply[1];

    pthread_mutex_lock(&(player->room->mutex));
    for (players_cursor = player->room->players;
         players_cursor < player->room->players + OTHELLO_ROOM_LENGTH;
         players_cursor++) {
      if (*players_cursor == player) {
        player_next = players_cursor + 1;
        if (player_next >= player->room->players + OTHELLO_ROOM_LENGTH) {
          player_next = player->room->players;
        }
        (*player_next)->ready = true;
      } else {
        pthread_mutex_lock(&((*players_cursor)->mutex));
        othello_write_all((*players_cursor)->socket, notif_play,
                          sizeof(notif_play));
        pthread_mutex_unlock(&((*players_cursor)->mutex));
      }
    }
    pthread_mutex_unlock(&(player->room->mutex));
  }

  pthread_mutex_lock(&(player->mutex));
  if (othello_write_all(player->socket, reply, sizeof(reply)) <= 0) {
    status = OTHELLO_FAILURE;
  }
  pthread_mutex_unlock(&(player->mutex));

  return status;
}

void *othello_start(void *arg) {
  othello_player_t *player;
  char query;
  int status;

  player = (othello_player_t *)arg;

  othello_log(LOG_INFO, "%p start", player);

  while (othello_read_all(player->socket, &query, sizeof(query)) > 0) {
    switch (query) {
    case OTHELLO_QUERY_CONNECT:
      status = othello_handle_connect(player);
      break;
    case OTHELLO_QUERY_ROOM_LIST:
      status = othello_handle_room_list(player);
      break;
    case OTHELLO_QUERY_ROOM_JOIN:
      status = othello_handle_room_join(player);
      break;
    case OTHELLO_QUERY_ROOM_LEAVE:
      status = othello_handle_room_leave(player);
      break;
    case OTHELLO_QUERY_MESSAGE:
      status = othello_handle_message(player);
      break;
    case OTHELLO_QUERY_READY:
      status = othello_handle_ready(player);
      break;
    case OTHELLO_QUERY_PLAY:
      status = othello_handle_play(player);
      break;
    default:
      status = OTHELLO_FAILURE;
      break;
    }

    if (status != OTHELLO_SUCCESS) {
      break;
    }
  }

  othello_end(player);

  return NULL;
}

int othello_create_socket_stream(unsigned short port) {
  int socket_stream, status;
  struct sockaddr_in address;

  if ((socket_stream = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
    othello_log(LOG_ERR, "socket");
    return socket_stream;
  }

  memset(&address, 0, sizeof(struct sockaddr_in));
  address.sin_family = AF_INET;
  address.sin_port = htons(port);
  address.sin_addr.s_addr = htonl(INADDR_ANY);

  if ((status = bind(socket_stream, (struct sockaddr *)&address,
                     sizeof(struct sockaddr_in))) < 0) {
    close(socket_stream);
    othello_log(LOG_ERR, "bind");
    return status;
  }

  return socket_stream;
}

/*TODO: destroy all rooms/players*/
void othello_exit(void) {
  if (sock >= 0)
    close(sock);
#ifdef OTHELLO_WITH_SYSLOG
  closelog();
#else
  pthread_mutex_destroy(&log_mutex);
#endif
}

int main(int argc, char *argv[]) {
  othello_player_t *player;
  othello_room_t *room_cursor;
  unsigned short port;

  /* init global */
  /* init socket */
  port = 5000;
  sock = -1;
#ifdef OTHELLO_WITH_SYSLOG
  openlog(NULL, LOG_CONS | LOG_PID, LOG_USER);
#else
  if (pthread_mutex_init(&log_mutex, NULL)) {
    return EXIT_FAILURE;
  }
#endif

  /* init rooms */
  memset(rooms, 0, sizeof(othello_room_t) * OTHELLO_NUMBER_OF_ROOMS);
  for (room_cursor = rooms; room_cursor < rooms + OTHELLO_NUMBER_OF_ROOMS;
       room_cursor++) {
    if (pthread_mutex_init(&(room_cursor->mutex), NULL)) {
      return EXIT_FAILURE;
    }
  }

  if (atexit(othello_exit))
    return EXIT_FAILURE;

  /* open socket */
  if ((sock = othello_create_socket_stream(port)) < 0) {
    othello_log(LOG_ERR, "othello_create_socket_stream");
    return EXIT_FAILURE;
  }

  if (listen(sock, SOMAXCONN)) {
    othello_log(LOG_ERR, "listen");
    return EXIT_FAILURE;
  }

  othello_log(LOG_INFO, "server listening on port %d", port);

  for (;;) {
    if ((player = malloc(sizeof(othello_player_t))) == NULL) {
      othello_log(LOG_ERR, "malloc");
      return EXIT_FAILURE;
    }

    memset(player, 0, sizeof(othello_player_t));
    if (pthread_mutex_init(&(player->mutex), NULL)) {
      othello_log(LOG_ERR, "pthread_mutex_init");
      return EXIT_FAILURE;
    }

    if ((player->socket = accept(sock, NULL, NULL)) < 0) {
      othello_log(LOG_ERR, "accept");
      return EXIT_FAILURE;
    }

    if (pthread_mutex_init(&(player->mutex), NULL)) {
      othello_log(LOG_ERR, "pthread_mutex_init");
      return EXIT_FAILURE;
    }

    if (pthread_create(&(player->thread), NULL, othello_start, player)) {
      othello_log(LOG_ERR, "pthread_create");
      return EXIT_FAILURE;
    }

    if (pthread_detach(player->thread)) {
      othello_log(LOG_ERR, "pthread_detach");
      return EXIT_FAILURE;
    }
  }

  return EXIT_SUCCESS;
}
