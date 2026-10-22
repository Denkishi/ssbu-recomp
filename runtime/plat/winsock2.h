/* Linux build: the Winsock subset nnnet.c and the online code (net_relay.c, net_minilobby.c) use, on BSD sockets. WSA error codes are the errno values (and the
 * getaddrinfo EAI_ codes for the resolver ones), so code that maps them keeps working. */
#ifndef PLAT_WINSOCK2_H
#define PLAT_WINSOCK2_H
#include "windows.h"
#include <errno.h>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>

typedef int SOCKET;
typedef unsigned long u_long;
typedef struct linger LINGER;
typedef struct pollfd WSAPOLLFD;
typedef struct { WORD wVersion, wHighVersion; char szDescription[257], szSystemStatus[129]; unsigned short iMaxSockets, iMaxUdpDg; char *lpVendorInfo; } WSADATA;
typedef struct addrinfo ADDRINFOA;
#define INVALID_SOCKET (-1)
#define SOCKET_ERROR (-1)
#define closesocket close
#define ioctlsocket ioctl
#define WSAPoll poll
#define InetPtonA inet_pton
#define InetNtopA inet_ntop
#define WSAStartup(v, d) (memset((d), 0, sizeof *(d)), 0)
#define WSACleanup() 0                 /* sockets need no library start or end on Linux */
/* SIO_UDP_CONNRESET (the only WSAIoctl the online code makes) turns off Windows' WSAECONNRESET on a UDP receive after
 * an ICMP port-unreachable; Linux reports no ICMP errors on an unconnected UDP socket (without IP_RECVERR) */
#define IOC_VENDOR 0x18000000
#define _WSAIOW(x, y) (0x80000000u | (x) | (y))
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#define WSAIoctl(s, code, in, nin, out, nout, nret, ov, fn) ((void)(s), (void)(in), (void)(nin), (void)(out), (void)(nout), (void)(nret), 0)
/* SO_RCVTIMEO / SO_SNDTIMEO: a DWORD of milliseconds on Windows, a struct timeval here (a DWORD makes setsockopt fail
 * with EINVAL, and the socket then blocks without a timeout). A 4-byte value of those two options is converted. */
static inline int plat_setsockopt(int s, int level, int name, const void *v, socklen_t n) {
    if (level == SOL_SOCKET && (name == SO_RCVTIMEO || name == SO_SNDTIMEO) && n == sizeof(DWORD)) {
        DWORD ms = *(const DWORD *)v;
        struct timeval tv = { (time_t)(ms / 1000), (suseconds_t)(ms % 1000) * 1000 };
        return setsockopt(s, level, name, &tv, sizeof tv);
    }
    return setsockopt(s, level, name, v, n);
}
#define setsockopt plat_setsockopt
#define WSAGetLastError() errno
#define WSASetLastError(e) (errno = (e))
#define SD_RECEIVE SHUT_RD
#define SD_SEND SHUT_WR
#define SD_BOTH SHUT_RDWR
#define WSAEINTR EINTR
#define WSAEBADF EBADF
#define WSAEACCES EACCES
#define WSAEFAULT EFAULT
#define WSAEINVAL EINVAL
#define WSAEMFILE EMFILE
#define WSAEWOULDBLOCK EWOULDBLOCK
#define WSAEINPROGRESS EINPROGRESS
#define WSAEALREADY EALREADY
#define WSAENOTSOCK ENOTSOCK
#define WSAEDESTADDRREQ EDESTADDRREQ
#define WSAEMSGSIZE EMSGSIZE
#define WSAEPROTOTYPE EPROTOTYPE
#define WSAENOPROTOOPT ENOPROTOOPT
#define WSAEPROTONOSUPPORT EPROTONOSUPPORT
#define WSAESOCKTNOSUPPORT ESOCKTNOSUPPORT
#define WSAEOPNOTSUPP EOPNOTSUPP
#define WSAEPFNOSUPPORT EPFNOSUPPORT
#define WSAEAFNOSUPPORT EAFNOSUPPORT
#define WSAEADDRINUSE EADDRINUSE
#define WSAEADDRNOTAVAIL EADDRNOTAVAIL
#define WSAENETDOWN ENETDOWN
#define WSAENETUNREACH ENETUNREACH
#define WSAENETRESET ENETRESET
#define WSAECONNABORTED ECONNABORTED
#define WSAECONNRESET ECONNRESET
#define WSAENOBUFS ENOBUFS
#define WSAEISCONN EISCONN
#define WSAENOTCONN ENOTCONN
#define WSAESHUTDOWN ESHUTDOWN
#define WSAETIMEDOUT ETIMEDOUT
#define WSAECONNREFUSED ECONNREFUSED
#define WSAEHOSTDOWN EHOSTDOWN
#define WSAEHOSTUNREACH EHOSTUNREACH
#define WSAHOST_NOT_FOUND EAI_NONAME
#define WSATRY_AGAIN EAI_AGAIN
#define WSANO_RECOVERY EAI_FAIL
#define WSATYPE_NOT_FOUND EAI_SERVICE
#define WSA_NOT_ENOUGH_MEMORY EAI_MEMORY
#define WSANO_DATA EAI_NODATA
#endif
