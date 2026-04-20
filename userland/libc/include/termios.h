#ifndef _MAKAOS_TERMIOS_H
#define _MAKAOS_TERMIOS_H 1

typedef unsigned int  tcflag_t;
typedef unsigned char cc_t;
typedef unsigned int  speed_t;

#define NCCS 32
struct termios {
    tcflag_t c_iflag;
    tcflag_t c_oflag;
    tcflag_t c_cflag;
    tcflag_t c_lflag;
    cc_t     c_line;
    cc_t     c_cc[NCCS];
    speed_t  c_ispeed;
    speed_t  c_ospeed;
};

// c_iflag
#define IGNBRK  0000001
#define BRKINT  0000002
#define IGNPAR  0000004
#define ICRNL   0000400
#define IXON    0002000
#define IXOFF   0010000

// c_oflag
#define OPOST   0000001
#define ONLCR   0000004

// c_cflag
#define CS8     0000060
#define CREAD   0000200
#define CLOCAL  0004000

// c_lflag
#define ISIG    0000001
#define ICANON  0000002
#define ECHO    0000010
#define ECHOE   0000020
#define ECHOK   0000040
#define ECHONL  0000100
#define NOFLSH  0000200
#define TOSTOP  0000400
#define ECHOCTL 0001000
#define ECHOPRT 0002000
#define ECHOKE  0004000
#define IEXTEN  0100000
#define EXTPROC 0200000

// c_cc indices
#define VINTR  0
#define VQUIT  1
#define VERASE 2
#define VKILL  3
#define VEOF   4
#define VTIME  5
#define VMIN   6
#define VSTART 8
#define VSTOP  9
#define VSUSP  10

// tcsetattr optional actions
#define TCSANOW   0
#define TCSADRAIN 1
#define TCSAFLUSH 2

int tcgetattr(int fd, struct termios* t);
int tcsetattr(int fd, int actions, const struct termios* t);
int tcflush(int fd, int q);
int tcdrain(int fd);

#endif
