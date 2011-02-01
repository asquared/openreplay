#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <unistd.h>
#include <fcntl.h>
#include <stdexcept>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/types.h>

#include "mmap_state.h"

int main( ) {
    int socket_fd;
    struct sockaddr_in listen_addr;
    struct ip_mreq mreq;

    int recvd;
    uint32_t clock;
    MmapState clock_ipc("clock_ipc");

    inet_aton("239.160.181.93", &mreq.imr_multiaddr);
    inet_aton("0.0.0.0", &mreq.imr_interface);

    memset(&listen_addr, 0, sizeof(listen_addr));
    listen_addr.sin_family = AF_INET;
    listen_addr.sin_port = htons(30003);
    /* leave address set to zeroes (bind to all interfaces) */

    socket_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (socket_fd == -1) {
        perror("socket");
        exit(1);
    }


    if (bind(socket_fd, (struct sockaddr *)&listen_addr, sizeof(listen_addr)) == -1) {
        perror("bind");
        exit(1);
    }
    
    if (setsockopt(socket_fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) != 0) {
        perror("setsockopt");
        exit(1);
    }

    for (;;) {
        recvd = recvfrom(socket_fd, &clock, sizeof(clock), 0, 0, 0);

        if (recvd < 0) {
            perror("recvfrom");
            continue;
        } else if (recvd < sizeof(clock)) {
            fprintf(stderr, "did not receive 4-byte timestamp");
            continue;
        } else {
            clock = ntohl(clock);
            clock_ipc.put(&clock, sizeof(clock));                     
        }
    }

    close(socket_fd);
}
