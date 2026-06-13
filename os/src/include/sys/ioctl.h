#ifndef _SYS_IOCTL_H
#define _SYS_IOCTL_H

#define TCGETATTR   0x5401
#define TCSETATTR   0x5402
#define TCGETS      TCGETATTR
#define TCSETS      TCSETATTR
#define TIOCGPGRP   0x540F
#define TIOCSPGRP   0x5410

#define NCCS 32

struct termios {
    unsigned int c_lflag;
    char         c_cc[NCCS];
};

extern int ioctl(int fd, unsigned long request, void* argp);

#endif
