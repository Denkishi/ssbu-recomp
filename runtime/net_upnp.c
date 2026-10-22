/* UPnP-IGD port mapping for online play (Windows NAT traversal COM API: IUPnPNAT -> IStaticPortMappingCollection).
 * UPnP approach after SNO2207/WindowSmash, used with the author's permission. The code is our own.
 *
 * #included by net_relay.c, so everything is static. Needs winsock2.h, windows.h, stdio, string, stdint before it
 * (it includes them itself when compiled alone).
 *
 *   upnp_map_udp(port, lan_ip, desc, ext_ip, n, why, n)   external UDP port -> lan_ip:port; 1 ok, 0 failed (why)
 *   upnp_unmap_udp(port)                                   removes that mapping (call at exit)
 *   upnp_probe(why, n)                                     READ ONLY: 1 if an IGD answers (why: existing mappings)
 *
 * Linux / macOS: the same jobs on the IGD protocol itself (SSDP + SOAP over plain sockets, below upnp_worker's Windows one).
 * Every COM call runs on a short-lived worker thread (its own CoInitializeEx) and the caller waits at most
 * UPNP_WAIT_MS, so a silent router can never freeze the game. A worker that outlives the wait is abandoned. */
#ifndef NET_UPNP_C
#define NET_UPNP_C

#ifndef _WIN32_DCOM
#define _WIN32_DCOM
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#ifdef _WIN32
#define COBJMACROS
#include <ole2.h>
#include <oleauto.h>
#include <natupnp.h>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#endif

#define UPNP_WAIT_MS 6000
#define UPNP_UNMAP_WAIT_MS 4000

#ifdef _WIN32
/* own copies of the two GUIDs, so no uuid.lib is needed */
static const GUID upnp_clsid_nat = { 0xAE1E00AA, 0x3FD5, 0x403C, { 0x8A, 0x27, 0x2B, 0xBD, 0xC3, 0x0C, 0xD0, 0xE1 } };
static const GUID upnp_iid_nat   = { 0xB171C812, 0xCC76, 0x485A, { 0x94, 0xD8, 0xB6, 0xB3, 0xA2, 0x79, 0x4E, 0x99 } };
#endif

enum { UPNP_OP_MAP = 1, UPNP_OP_UNMAP, UPNP_OP_PROBE };

typedef struct upnp_job {
    volatile LONG refs;            /* waiter + worker; the last one frees */
    HANDLE done;                   /* manual-reset event, set by the worker when finished */
    int op;
    uint16_t port;
    char lan[16];
    char desc[64];
    int ok;                        /* result */
    char ext[64];
    char why[96];
} upnp_job;

static void upnp_job_release(upnp_job *j)
{
    if (InterlockedDecrement(&j->refs) == 0) {
        if (j->done) CloseHandle(j->done);
        free(j);
    }
}

static void upnp_fail(upnp_job *j, const char *why)
{
    j->ok = 0;
    snprintf(j->why, sizeof j->why, "%s", why);
}

#ifdef _WIN32
static DWORD WINAPI upnp_worker(LPVOID arg)
{
    upnp_job *j = (upnp_job *)arg;
    HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    int com = SUCCEEDED(hr);
    IUPnPNAT *nat = NULL;
    IStaticPortMappingCollection *col = NULL;

    if (!com && hr != RPC_E_CHANGED_MODE) { upnp_fail(j, "COM could not start"); goto out; }
    hr = CoCreateInstance(&upnp_clsid_nat, NULL, CLSCTX_ALL, &upnp_iid_nat, (void **)&nat);
    if (FAILED(hr) || !nat) { upnp_fail(j, "no UPnP service on this PC"); goto out; }
    hr = IUPnPNAT_get_StaticPortMappingCollection(nat, &col);
    if (FAILED(hr) || !col) { upnp_fail(j, "no UPnP router answered"); goto out; }

    if (j->op == UPNP_OP_PROBE) {
        long n = 0;
        if (SUCCEEDED(IStaticPortMappingCollection_get_Count(col, &n))) {
            j->ok = 1;
            snprintf(j->why, sizeof j->why, "UPnP router answered, %ld existing mapping(s)", n);
        } else {
            j->ok = 1;
            snprintf(j->why, sizeof j->why, "UPnP router answered, mapping count not readable");
        }
    } else if (j->op == UPNP_OP_UNMAP) {
        BSTR proto = SysAllocString(L"UDP");
        hr = proto ? IStaticPortMappingCollection_Remove(col, (long)j->port, proto) : E_OUTOFMEMORY;
        SysFreeString(proto);
        if (SUCCEEDED(hr)) { j->ok = 1; snprintf(j->why, sizeof j->why, "mapping removed"); }
        else upnp_fail(j, "router refused to remove the mapping");
    } else {
        wchar_t wlan[16], wdesc[64];
        BSTR proto, client, desc;
        IStaticPortMapping *map = NULL;
        MultiByteToWideChar(CP_ACP, 0, j->lan, -1, wlan, 16);
        MultiByteToWideChar(CP_UTF8, 0, j->desc, -1, wdesc, 64);
        proto = SysAllocString(L"UDP");
        client = SysAllocString(wlan);
        desc = SysAllocString(wdesc);
        if (!proto || !client || !desc) hr = E_OUTOFMEMORY;
        else {
            /* a stale mapping of ours from a crashed run would make Add fail: drop it first (ignore the result) */
            IStaticPortMappingCollection_Remove(col, (long)j->port, proto);
            hr = IStaticPortMappingCollection_Add(col, (long)j->port, proto, (long)j->port, client, VARIANT_TRUE, desc, &map);
        }
        SysFreeString(proto); SysFreeString(client); SysFreeString(desc);
        if (FAILED(hr) || !map) {
            upnp_fail(j, "router refused the mapping (port in use?)");
        } else {
            BSTR ext = NULL;
            j->ok = 1;
            snprintf(j->why, sizeof j->why, "mapped");
            if (SUCCEEDED(IStaticPortMapping_get_ExternalIPAddress(map, &ext)) && ext) {
                WideCharToMultiByte(CP_ACP, 0, ext, -1, j->ext, (int)sizeof j->ext, NULL, NULL);
                j->ext[sizeof j->ext - 1] = 0;
            }
            if (ext) SysFreeString(ext);
        }
        if (map) IStaticPortMapping_Release(map);
    }
out:
    if (col) IStaticPortMappingCollection_Release(col);
    if (nat) IUPnPNAT_Release(nat);
    if (com) CoUninitialize();
    SetEvent(j->done);
    upnp_job_release(j);
    return 0;
}
#else
/* Linux / macOS: no NAT traversal API in the system, so the IGD protocol itself on plain sockets: SSDP M-SEARCH for the
 * router, its device description (HTTP GET), then SOAP calls to its WANIPConnection / WANPPPConnection service. */
#include <strings.h>
#include <sys/time.h>
#include <unistd.h>
typedef struct { char host[64]; uint16_t port; char path[256]; } upnp_url;

static const char *upnp_istr(const char *s, const char *w)                /* strcasestr without _GNU_SOURCE */
{
    size_t n = strlen(w);
    for (; *s; s++) if (!strncasecmp(s, w, n)) return s;
    return NULL;
}

static int upnp_parse_url(const char *u, upnp_url *o)
{
    const char *p = strstr(u, "://"), *slash, *colon;
    size_t n;
    p = p ? p + 3 : u;
    slash = strchr(p, '/');
    n = slash ? (size_t)(slash - p) : strlen(p);
    colon = memchr(p, ':', n);
    if ((colon ? (size_t)(colon - p) : n) >= sizeof o->host) return 0;
    snprintf(o->host, sizeof o->host, "%.*s", (int)(colon ? colon - p : (long)n), p);
    o->port = colon ? (uint16_t)atoi(colon + 1) : 80;
    snprintf(o->path, sizeof o->path, "%s", slash ? slash : "/");
    return o->host[0] && o->port;
}

/* one HTTP/1.0 exchange (no chunked answers, the router closes the connection); returns the status, the body in out */
static int upnp_http(const upnp_url *u, const char *head, const char *body, char *out, size_t n)
{
    struct sockaddr_in a;
    struct timeval to = { 3, 0 };
    char req[2048];
    size_t got = 0;
    int s, len, status = 0;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons(u->port);
    if (inet_pton(AF_INET, u->host, &a.sin_addr) != 1) return 0;          /* an IGD's description URL is an IP address */
    if ((s = socket(AF_INET, SOCK_STREAM, 0)) < 0) return 0;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &to, sizeof to);
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &to, sizeof to);              /* (also bounds connect on Linux) */
    len = snprintf(req, sizeof req, "%s %s HTTP/1.0\r\nHost: %s:%u\r\n%sContent-Length: %u\r\nConnection: close\r\n\r\n%s",
                   body ? "POST" : "GET", u->path, u->host, u->port, head ? head : "", body ? (unsigned)strlen(body) : 0u, body ? body : "");
    if (len > 0 && len < (int)sizeof req && !connect(s, (struct sockaddr *)&a, sizeof a) && send(s, req, (size_t)len, 0) == len) {
        for (;;) {
            ssize_t r = recv(s, out + got, n - 1 - got, 0);
            if (r <= 0) break;
            got += (size_t)r;
            if (got + 1 >= n) break;
        }
    }
    close(s);
    out[got] = 0;
    if (!strncmp(out, "HTTP/", 5) && strchr(out, ' ')) status = atoi(strchr(out, ' ') + 1);
    if (status) { char *b = strstr(out, "\r\n\r\n"); if (b) memmove(out, b + 4, strlen(b + 4) + 1); }
    return status;
}

/* text of the first <tag>...</tag> at or after p */
static int upnp_tag(const char *p, const char *tag, char *out, size_t n)
{
    char open[64], close_[64];
    const char *a, *b;
    snprintf(open, sizeof open, "<%s>", tag);
    snprintf(close_, sizeof close_, "</%s>", tag);
    if (!(a = strstr(p, open)) || !(b = strstr(a += strlen(open), close_)) || (size_t)(b - a) >= n) return 0;
    snprintf(out, n, "%.*s", (int)(b - a), a);
    return 1;
}

/* finds the router: control URL and service type of its WAN connection service; why on failure */
static int upnp_find_igd(upnp_url *ctl, char *svc, size_t svc_n, char *why, size_t why_n)
{
    static const char *const sts[] = { "urn:schemas-upnp-org:device:InternetGatewayDevice:1",
                                       "urn:schemas-upnp-org:service:WANIPConnection:1", "upnp:rootdevice" };
    struct sockaddr_in m;
    struct timeval to = { 0, 300000 };
    char buf[2048], loc[300] = "";
    int s = socket(AF_INET, SOCK_DGRAM, 0), tries;
    if (s < 0) { snprintf(why, why_n, "no UDP socket"); return 0; }
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &to, sizeof to);
    memset(&m, 0, sizeof m);
    m.sin_family = AF_INET;
    m.sin_port = htons(1900);
    m.sin_addr.s_addr = inet_addr("239.255.255.250");
    for (tries = 0; tries < 9 && !loc[0]; tries++) {                       /* about 3 s */
        int k = snprintf(buf, sizeof buf, "M-SEARCH * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\nMAN: \"ssdp:discover\"\r\nMX: 2\r\nST: %s\r\n\r\n", sts[tries % 3]);
        sendto(s, buf, (size_t)k, 0, (struct sockaddr *)&m, sizeof m);
        for (;;) {
            ssize_t r = recv(s, buf, sizeof buf - 1, 0);
            const char *l;
            if (r <= 0) break;
            buf[r] = 0;
            if (!upnp_istr(buf, "InternetGatewayDevice") && !upnp_istr(buf, "WANIPConnection") && !upnp_istr(buf, "WANPPPConnection")
                && tries % 3 != 2) continue;
            if ((l = upnp_istr(buf, "\nLOCATION:"))) {
                l += 10;
                while (*l == ' ') l++;
                snprintf(loc, sizeof loc, "%.*s", (int)strcspn(l, "\r\n"), l);
                break;
            }
        }
    }
    close(s);
    if (!loc[0]) { snprintf(why, why_n, "no UPnP router answered"); return 0; }

    upnp_url dev;
    char *xml = (char *)malloc(65536), *p;
    if (!xml) { snprintf(why, why_n, "out of memory"); return 0; }
    if (!upnp_parse_url(loc, &dev) || upnp_http(&dev, NULL, NULL, xml, 65536) != 200) {
        snprintf(why, why_n, "the router's UPnP description could not be read"); free(xml); return 0;
    }
    /* the service whose type names a WAN connection; its controlURL follows it inside the same <service> */
    for (p = xml; (p = strstr(p, "<serviceType>")); p++) {
        char type[128], url[256];
        if (!upnp_tag(p, "serviceType", type, sizeof type)) continue;
        if (!strstr(type, "WANIPConnection:") && !strstr(type, "WANPPPConnection:")) continue;
        if (!upnp_tag(p, "controlURL", url, sizeof url)) continue;
        snprintf(svc, svc_n, "%s", type);
        if (strstr(url, "://")) { if (!upnp_parse_url(url, ctl)) break; }
        else { *ctl = dev; snprintf(ctl->path, sizeof ctl->path, "%s%s", url[0] == '/' ? "" : "/", url); }
        free(xml);
        return 1;
    }
    free(xml);
    snprintf(why, why_n, "the UPnP device is not an internet gateway");
    return 0;
}

static int upnp_soap(const upnp_url *ctl, const char *svc, const char *action, const char *args, char *out, size_t n)
{
    char head[300], body[1536];
    snprintf(head, sizeof head, "Content-Type: text/xml; charset=\"utf-8\"\r\nSOAPAction: \"%s#%s\"\r\n", svc, action);
    snprintf(body, sizeof body, "<?xml version=\"1.0\"?>\r\n<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
             "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\"><s:Body><u:%s xmlns:u=\"%s\">%s</u:%s></s:Body></s:Envelope>\r\n",
             action, svc, args, action);
    return upnp_http(ctl, head, body, out, n);
}

static DWORD WINAPI upnp_worker(LPVOID arg)
{
    upnp_job *j = (upnp_job *)arg;
    upnp_url ctl;
    char svc[128], args[768], out[4096];

    if (!upnp_find_igd(&ctl, svc, sizeof svc, j->why, sizeof j->why)) { j->ok = 0; goto out; }
    if (j->op == UPNP_OP_PROBE) {
        j->ok = 1;
        snprintf(j->why, sizeof j->why, "UPnP router answered (%s)", strstr(svc, "PPP") ? "WANPPPConnection" : "WANIPConnection");
    } else {
        snprintf(args, sizeof args, "<NewRemoteHost></NewRemoteHost><NewExternalPort>%u</NewExternalPort><NewProtocol>UDP</NewProtocol>", j->port);
        if (j->op == UPNP_OP_UNMAP) {
            if (upnp_soap(&ctl, svc, "DeletePortMapping", args, out, sizeof out) == 200) { j->ok = 1; snprintf(j->why, sizeof j->why, "mapping removed"); }
            else upnp_fail(j, "router refused to remove the mapping");
        } else {
            /* a stale mapping of ours from a crashed run would make Add fail: drop it first (ignore the result) */
            upnp_soap(&ctl, svc, "DeletePortMapping", args, out, sizeof out);
            snprintf(args + strlen(args), sizeof args - strlen(args), "<NewInternalPort>%u</NewInternalPort><NewInternalClient>%s</NewInternalClient>"
                     "<NewEnabled>1</NewEnabled><NewPortMappingDescription>%s</NewPortMappingDescription><NewLeaseDuration>0</NewLeaseDuration>",
                     j->port, j->lan, j->desc);
            if (upnp_soap(&ctl, svc, "AddPortMapping", args, out, sizeof out) != 200) upnp_fail(j, "router refused the mapping (port in use?)");
            else {
                j->ok = 1;
                snprintf(j->why, sizeof j->why, "mapped");
                if (upnp_soap(&ctl, svc, "GetExternalIPAddress", "", out, sizeof out) == 200)
                    upnp_tag(out, "NewExternalIPAddress", j->ext, sizeof j->ext);
            }
        }
    }
out:
    SetEvent(j->done);
    upnp_job_release(j);
    return 0;
}
#endif

/* runs one operation; returns the finished job (caller releases it) or NULL on timeout / start failure */
static upnp_job *upnp_run(upnp_job *j, DWORD wait_ms)
{
    HANDLE th;
    j->refs = 2;
    j->done = CreateEventA(NULL, TRUE, FALSE, NULL);
    th = j->done ? CreateThread(NULL, 0, upnp_worker, j, 0, NULL) : NULL;
    if (!th) {
        if (j->done) CloseHandle(j->done);
        j->done = NULL;
        free(j);
        return NULL;
    }
    CloseHandle(th);
    if (WaitForSingleObject(j->done, wait_ms) == WAIT_OBJECT_0) return j;
    upnp_job_release(j);           /* abandoned: the worker frees it when (if) it ends */
    return NULL;
}

static void upnp_copy(char *dst, size_t n, const char *src)
{
    if (dst && n) snprintf(dst, n, "%s", src);
}

static int upnp_finish(upnp_job *j, char *ext_ip, size_t ext_n, char *why, size_t why_n)
{
    int ok;
    if (!j) { upnp_copy(why, why_n, "no UPnP router answered (timeout)"); return 0; }
    ok = j->ok;
    upnp_copy(why, why_n, j->why);
    if (ok && ext_ip) upnp_copy(ext_ip, ext_n, j->ext);
    upnp_job_release(j);
    return ok;
}

static int upnp_map_udp(uint16_t port, uint32_t lan_ip_host_order, const char *desc,
                        char *ext_ip, size_t ext_n, char *why, size_t why_n)
{
    upnp_job *j = (upnp_job *)calloc(1, sizeof *j);
    if (ext_ip && ext_n) ext_ip[0] = 0;
    if (!j) { upnp_copy(why, why_n, "out of memory"); return 0; }
    j->op = UPNP_OP_MAP;
    j->port = port;
    snprintf(j->lan, sizeof j->lan, "%u.%u.%u.%u", (unsigned)(lan_ip_host_order >> 24) & 255,
             (unsigned)(lan_ip_host_order >> 16) & 255, (unsigned)(lan_ip_host_order >> 8) & 255,
             (unsigned)lan_ip_host_order & 255);
    snprintf(j->desc, sizeof j->desc, "%s", desc ? desc : "SSBU online");
    return upnp_finish(upnp_run(j, UPNP_WAIT_MS), ext_ip, ext_n, why, why_n);
}

static void upnp_unmap_udp(uint16_t port)
{
    upnp_job *j = (upnp_job *)calloc(1, sizeof *j);
    if (!j) return;
    j->op = UPNP_OP_UNMAP;
    j->port = port;
    upnp_finish(upnp_run(j, UPNP_UNMAP_WAIT_MS), NULL, 0, NULL, 0);
}

static int upnp_probe(char *why, size_t why_n)
{
    upnp_job *j = (upnp_job *)calloc(1, sizeof *j);
    if (!j) { upnp_copy(why, why_n, "out of memory"); return 0; }
    j->op = UPNP_OP_PROBE;
    return upnp_finish(upnp_run(j, UPNP_WAIT_MS), NULL, 0, why, why_n);
}

#ifdef UPNP_MAIN
/* test: the probe is read only. The mapping calls are compiled but only run with the flag --really-map, which the
 * test run does NOT use (it changes the router configuration and needs the owner's permission). */
int main(int argc, char **argv)
{
    char why[128] = "", ext[64] = "";
    int ok;
    if (argc > 3 && !strcmp(argv[1], "--really-map")) {
        ok = upnp_map_udp((uint16_t)atoi(argv[2]), 0, argv[3], ext, sizeof ext, why, sizeof why);
        printf("map: %d ext=%s why=%s\n", ok, ext, why);
        upnp_unmap_udp((uint16_t)atoi(argv[2]));
        return !ok;
    }
    ok = upnp_probe(why, sizeof why);
    printf("probe: %d (%s)\n", ok, why);
    return 0;
}
#endif
#endif /* NET_UPNP_C */
