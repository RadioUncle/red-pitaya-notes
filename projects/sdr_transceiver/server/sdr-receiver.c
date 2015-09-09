#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <fcntl.h>
#include <math.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define TCP_PORT 1001

int interrupted = 0;

void signal_handler(int sig)
{
  interrupted = 1;
}

int main(int argc, char *argv[])
{
  int fd, sockServer, sockClient;
  int position, limit, offset;
  pid_t pid;
  void *cfg, *sts, *ram;
  char *name = "/dev/mem";
  int size = 0;
  struct sockaddr_in addr;
  uint32_t command, freq;
  uint32_t freqMin = 50000;
  uint32_t freqMax = 60000000;
  int yes = 1;

  if((fd = open(name, O_RDWR)) < 0)
  {
    perror("open");
    return 1;
  }

  cfg = mmap(NULL, sysconf(_SC_PAGESIZE), PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0x40000000);
  sts = mmap(NULL, sysconf(_SC_PAGESIZE), PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0x40001000);
  ram = mmap(NULL, sysconf(_SC_PAGESIZE), PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0x40002000);

  if((sockServer = socket(AF_INET, SOCK_STREAM, 0)) < 0)
  {
    perror("socket");
    return 1;
  }

  setsockopt(sockServer, SOL_SOCKET, SO_REUSEADDR, (void *)&yes , sizeof(yes));

  /* setup listening address */
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons(TCP_PORT);

  if(bind(sockServer, (struct sockaddr *)&addr, sizeof(addr)) < 0)
  {
    perror("bind");
    return 1;
  }

  listen(sockServer, 1024);

  while(!interrupted)
  {
    /* enter reset mode */
    *((uint32_t *)(cfg + 0)) &= ~31;
    /* set default phase increment */
    *((uint32_t *)(cfg + 4)) = (uint32_t)floor(600000/125.0e6*(1<<30)+0.5);
    /* set default sample rate */
    *((uint32_t *)(cfg + 8)) = 625;

    if((sockClient = accept(sockServer, NULL, NULL)) < 0)
    {
      perror("accept");
      return 1;
    }

    signal(SIGINT, signal_handler);

    /* enter normal operating mode */
    *((uint32_t *)(cfg + 0)) |= 7;

    limit = 256;

    while(!interrupted)
    {
      if(ioctl(sockClient, FIONREAD, &size) < 0) break;

      if(size >= 4)
      {
        if(recv(sockClient, (char *)&command, 4, MSG_WAITALL) < 0) break;
        switch(command >> 28)
        {
          case 0:
            /* set rx phase increment */
            freq = command & 0xfffffff;
            if(freq < freqMin || freq > freqMax) continue;
            *((uint32_t *)(cfg + 4)) = (uint32_t)floor(freq/125.0e6*(1<<30)+0.5);
            break;
          case 1:
            /* set sample rate */
            switch(command & 3)
            {
              case 0:
                freqMin = 25000;
                *((uint32_t *)(cfg + 0)) &= ~4;
                *((uint32_t *)(cfg + 8)) = 1250;
                *((uint32_t *)(cfg + 0)) |= 4;
                break;
              case 1:
                freqMin = 50000;
                *((uint32_t *)(cfg + 0)) &= ~4;
                *((uint32_t *)(cfg + 8)) = 625;
                *((uint32_t *)(cfg + 0)) |= 4;
                break;
              case 2:
                freqMin = 125000;
                *((uint32_t *)(cfg + 0)) &= ~4;
                *((uint32_t *)(cfg + 8)) = 250;
                *((uint32_t *)(cfg + 0)) |= 4;
                break;
              case 3:
                freqMin = 250000;
                *((uint32_t *)(cfg + 0)) &= ~4;
                *((uint32_t *)(cfg + 8)) = 125;
                *((uint32_t *)(cfg + 0)) |= 4;
                break;
            }
            break;
          case 2:
            /* set tx phase increment */
            freq = command & 0xfffffff;
            if(freq < freqMin || freq > freqMax) continue;
            *((uint32_t *)(cfg + 12)) = (uint32_t)floor(freq/125.0e6*(1<<30)+0.5);
            break;
          case 3:
            /* start tx */
            *((uint32_t *)(cfg + 0)) |= 24;
            break;
          case 4:
            /* stop tx */
            *((uint32_t *)(cfg + 0)) &= ~24;
            break;
        }
      }

      /* read ram writer position */
      position = *((uint16_t *)(sts + 0));

      /* send 2048 bytes if ready, otherwise sleep 0.1 ms */
      if((limit > 0 && position > limit) || (limit == 0 && position < 256))
      {
        offset = limit > 0 ? 0 : 2048;
        limit = limit > 0 ? 0 : 256;
        if(send(sockClient, ram + offset, 2048, MSG_NOSIGNAL) < 0) break;
      }
      else
      {
        usleep(100);
      }
    }

    signal(SIGINT, SIG_DFL);
    close(sockClient);
  }

  close(sockServer);

  /* enter reset mode */
  *((uint32_t *)(cfg + 0)) &= ~31;

  return 0;
}
