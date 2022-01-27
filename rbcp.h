#ifndef _RBCP
#define _RBCP

#include <sys/wait.h>
#include <sys/stat.h>    /* for S_xxx file mode constants */
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/uio.h>     /* for iovec{} and readv/writev */
#include <sys/ioctl.h>

#include <arpa/inet.h>   /* inet(3) functions */
#include <netinet/in.h>  /* sockaddr_in{} and other Internet defns */
#include <netinet/tcp.h> /* for TCP_NODELAY */

#include <err.h>
#include <errno.h>
#include <fcntl.h>       /* for nonblocking */
#include <netdb.h>       /* for getaddrinfo() */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#define RBCP_PORT 4660

struct sitcp_rbcp_header {
    unsigned char ver_type;
    unsigned char cmd_flag;
    unsigned char id;
    unsigned char length;
    unsigned int  address;
};

extern int open_rbcp(char *remote_ip);
extern int   get_reg_byte_stream(char *remote_ip, unsigned int address, unsigned char *buf, int len);
extern char  get_reg_byte(char *remote_ip, unsigned int address);
extern short get_reg_short(char *remote_ip, unsigned int address);
extern int   get_reg_int(char *remote_ip, unsigned int address);
extern int   set_reg_byte_stream(char *remote_ip, unsigned int address, unsigned char *buf, int len);
extern int   set_reg_byte(char *remote_ip, unsigned int address, unsigned char data);
extern int   set_reg_short(char *remote_ip, unsigned int address, unsigned short data);
extern int   set_reg_int(char *remote_ip, unsigned int address, unsigned int data);

#endif
