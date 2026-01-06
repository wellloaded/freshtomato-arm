/* Upstream port-mapping proxy (PCP / NAT-PMP / UPnP IGD client)
 *
 * Goal: when FreshTomato is behind another NAT (double NAT), ensure that
 * LAN-side UPnP/PCP/NAT-PMP mappings are only accepted if we can also
 * create/renew the corresponding mapping on the upstream router.
 *
 * This module is intentionally small and self-contained.
 */

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <stdlib.h>
#include <syslog.h>
#include <time.h>

#include "config.h"
#include "upstreamproxy.h"
#include "upnpglobalvars.h"
#include "getifaddr.h"

#define UPROXY_RETRYABLE   (-9)
#define UPROXY_DENIED      (-10)

/* Default timeouts are intentionally conservative: proxying should not
 * stall the LAN control plane for long.
 */
#define UPROXY_TIMEOUT_MS_DEFAULT 800

static int
get_wan_ipv4_str(char *buf, size_t buflen)
{
	if(!buf || buflen == 0)
		return -1;
	if(use_ext_ip_addr && use_ext_ip_addr[0] != '\0') {
		snprintf(buf, buflen, "%s", use_ext_ip_addr);
		return 0;
	}
	if(getifaddr(ext_if_name, buf, (int)buflen, NULL, NULL) < 0)
		return -1;
	return 0;
}

static int
udp_call(const struct sockaddr_in *dst, const void *req, size_t req_len,
         void *resp, size_t resp_len,
         unsigned int timeout_ms)
{
	int s;
	struct timeval tv;
	ssize_t n;

	s = socket(AF_INET, SOCK_DGRAM, 0);
	if(s < 0)
		return -1;

	tv.tv_sec = timeout_ms / 1000;
	tv.tv_usec = (timeout_ms % 1000) * 1000;
	(void)setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	(void)setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

	n = sendto(s, req, req_len, 0, (const struct sockaddr *)dst, sizeof(*dst));
	if(n < 0 || (size_t)n != req_len) {
		close(s);
		return -1;
	}

	n = recvfrom(s, resp, resp_len, 0, NULL, NULL);
	if(n < 0) {
		close(s);
		return -1;
	}
	close(s);
	return (int)n;
}

/* ---------------- NAT-PMP client ---------------- */

static int
natpmp_map(int proto, unsigned short eport, unsigned int lifetime)
{
	/* NAT-PMP uses version 0 and UDP port 5351 */
	unsigned char req[12];
	unsigned char resp[16];
	struct sockaddr_in dst;
	int n;
	uint16_t opcode, r_opcode, result;
	uint16_t internal_port, external_port;
	uint32_t r_lifetime;
	unsigned int timeout_ms;

	if(!proxy_upstream_enabled)
		return 0;
	if(!proxy_upstream_addr_set)
		return -1;

	timeout_ms = proxy_upstream_timeout_ms ? proxy_upstream_timeout_ms : UPROXY_TIMEOUT_MS_DEFAULT;

	memset(&dst, 0, sizeof(dst));
	dst.sin_family = AF_INET;
	dst.sin_port = htons(5351);
	dst.sin_addr = proxy_upstream_addr;

	memset(req, 0, sizeof(req));
	req[0] = 0; /* vers */
	if(proto == IPPROTO_TCP)
		opcode = 2;
	else
		opcode = 1;
	req[1] = (unsigned char)opcode;
	/* req[2..3] reserved */
	internal_port = htons(eport);
	external_port = htons(eport);
	memcpy(req + 4, &internal_port, 2);
	memcpy(req + 6, &external_port, 2);
	r_lifetime = htonl(lifetime);
	memcpy(req + 8, &r_lifetime, 4);

	n = udp_call(&dst, req, sizeof(req), resp, sizeof(resp), timeout_ms);
	if(n < 16)
		return -1;
	if(resp[0] != 0)
		return -1;
	r_opcode = resp[1];
	if(r_opcode != (unsigned char)(128 + opcode))
		return -1;
	memcpy(&result, resp + 2, 2);
	result = ntohs(result);
	memcpy(&internal_port, resp + 8, 2);
	memcpy(&external_port, resp + 10, 2);
	internal_port = ntohs(internal_port);
	external_port = ntohs(external_port);

	if(result == 0) {
		/* If upstream assigns a different external port, treat as retryable.
		 * (We currently require exact port to keep LAN-side semantics simple.)
		 */
		if(external_port != eport)
			return UPROXY_RETRYABLE;
		return 0;
	}
	if(result == 2)
		return UPROXY_DENIED;
	if(result == 4)
		return UPROXY_RETRYABLE;
	return -1;
}

static int
natpmp_unmap(int proto, unsigned short eport)
{
	return natpmp_map(proto, eport, 0);
}

/* ---------------- PCP client (MAP) ---------------- */

static uint32_t
rand32_fallback(void)
{
	uint32_t v = 0;
	int fd = open("/dev/urandom", O_RDONLY);
	if(fd >= 0) {
		if(read(fd, &v, sizeof(v)) != (ssize_t)sizeof(v))
			v = 0;
		close(fd);
	}
	if(v == 0)
		v = (uint32_t)((uintptr_t)&v ^ (uint32_t)time(NULL));
	return v;
}

static int
pcp_map(int proto, unsigned short eport, unsigned int lifetime)
{
	/* PCP MAP request (RFC 6887)
	 * We only support basic MAP without options.
	 */
	unsigned char req[60];
	unsigned char resp[110];
	struct sockaddr_in dst;
	int n;
	unsigned int timeout_ms;
	uint32_t r_lifetime;
	uint16_t internal_port, suggested_external_port, assigned_external_port;
	
	if(!proxy_upstream_enabled)
		return 0;
	if(!proxy_upstream_addr_set)
		return -1;

	timeout_ms = proxy_upstream_timeout_ms ? proxy_upstream_timeout_ms : UPROXY_TIMEOUT_MS_DEFAULT;

	memset(&dst, 0, sizeof(dst));
	dst.sin_family = AF_INET;
	dst.sin_port = htons(5351);
	dst.sin_addr = proxy_upstream_addr;

	memset(req, 0, sizeof(req));
	req[0] = 2; /* PCP version */
	req[1] = 1; /* MAP opcode */
	/* req[2..3] reserved */
	r_lifetime = htonl(lifetime);
	memcpy(req + 4, &r_lifetime, 4);

	/* Client IP Address (16 bytes) : IPv4-mapped IPv6 address */
	{
		char wan_ip[INET_ADDRSTRLEN];
		struct in_addr wan_addr;
		unsigned char *cip = req + 8;
		memset(cip, 0, 16);
		cip[10] = 0xff;
		cip[11] = 0xff;
		if(get_wan_ipv4_str(wan_ip, sizeof(wan_ip)) < 0)
			return -1;
		if(inet_aton(wan_ip, &wan_addr) == 0)
			return -1;
		memcpy(cip + 12, &wan_addr.s_addr, 4);
	}

	/* MAP-specific fields start at offset 24 */
	{
		unsigned char *p = req + 24;
		uint32_t n1 = rand32_fallback();
		uint32_t n2 = rand32_fallback();
		uint32_t n3 = rand32_fallback();
		memcpy(p + 0, &n1, 4);
		memcpy(p + 4, &n2, 4);
		memcpy(p + 8, &n3, 4);
		p[12] = (unsigned char)proto;
		/* p[13..15] reserved */
		internal_port = htons(eport);
		suggested_external_port = htons(eport);
		memcpy(p + 16, &internal_port, 2);
		memcpy(p + 18, &suggested_external_port, 2);
		/* suggested external IP address (16 bytes) = all zeros */
	}

	n = udp_call(&dst, req, sizeof(req), resp, sizeof(resp), timeout_ms);
	if(n < 60)
		return -1;
	if(resp[0] != 2)
		return -1;
	/* resp[1] is R-Opcode (128 + MAP) */
	if(resp[1] != (unsigned char)(128 + 1))
		return -1;
	/* resp[3] is Result Code */
	if(resp[3] == 0) {
		/* Assigned external port is at MAP response offset 40 */
		memcpy(&assigned_external_port, resp + 40, 2);
		assigned_external_port = ntohs(assigned_external_port);
		if(assigned_external_port != eport)
			return UPROXY_RETRYABLE;
		return 0;
	}
	if(resp[3] == 2 || resp[3] == 3)
		return UPROXY_DENIED;
	if(resp[3] == 4 || resp[3] == 8)
		return UPROXY_RETRYABLE;
	return -1;
}

static int
pcp_unmap(int proto, unsigned short eport)
{
	return pcp_map(proto, eport, 0);
}

/* ---------------- UPnP IGD client (minimal) ---------------- */

static int
tcp_connect_timeout(const char *host, unsigned short port, unsigned int timeout_ms)
{
	int s;
	struct sockaddr_in addr;
	struct timeval tv;

	s = socket(AF_INET, SOCK_STREAM, 0);
	if(s < 0)
		return -1;

	tv.tv_sec = timeout_ms / 1000;
	tv.tv_usec = (timeout_ms % 1000) * 1000;
	(void)setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	(void)setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons(port);
	if(inet_aton(host, &addr.sin_addr) == 0) {
		close(s);
		return -1;
	}
	if(connect(s, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		close(s);
		return -1;
	}
	return s;
}

static int
http_read_all(int s, char *buf, size_t buflen)
{
	size_t off = 0;
	ssize_t n;
	while(off + 1 < buflen) {
		n = recv(s, buf + off, buflen - 1 - off, 0);
		if(n <= 0)
			break;
		off += (size_t)n;
	}
	buf[off] = '\0';
	return (int)off;
}

static int
parse_location(const char *line, char *host, size_t hostlen, unsigned short *port, char *path, size_t pathlen)
{
	/* LOCATION: http://host:port/path */
	const char *p;
	const char *h;
	const char *slash;
	const char *colon;
	if(!line || !host || !port || !path)
		return -1;
	p = strstr(line, "http://");
	if(!p)
		return -1;
	p += 7;
	h = p;
	slash = strchr(p, '/');
	if(!slash)
		return -1;
	colon = memchr(p, ':', (size_t)(slash - p));
	if(colon) {
		size_t hl = (size_t)(colon - h);
		if(hl + 1 > hostlen)
			return -1;
		memcpy(host, h, hl);
		host[hl] = '\0';
		*port = (unsigned short)atoi(colon + 1);
	} else {
		size_t hl = (size_t)(slash - h);
		if(hl + 1 > hostlen)
			return -1;
		memcpy(host, h, hl);
		host[hl] = '\0';
		*port = 80;
	}
	snprintf(path, pathlen, "%s", slash);
	return 0;
}

static int
ssdp_discover_location(char *location, size_t locationlen, unsigned int timeout_ms)
{
	int s;
	struct sockaddr_in dst;
	struct sockaddr_in src;
	socklen_t srclen;
	char msg[256];
	char buf[1024];
	ssize_t n;
	struct timeval tv;

	if(!proxy_upstream_enabled)
		return -1;

	s = socket(AF_INET, SOCK_DGRAM, 0);
	if(s < 0)
		return -1;

	tv.tv_sec = timeout_ms / 1000;
	tv.tv_usec = (timeout_ms % 1000) * 1000;
	(void)setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	(void)setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

	memset(&dst, 0, sizeof(dst));
	dst.sin_family = AF_INET;
	dst.sin_port = htons(1900);
	(void)inet_aton("239.255.255.250", &dst.sin_addr);

	/* Try WANIPConnection first, then WANPPPConnection */
	snprintf(msg, sizeof(msg),
		"M-SEARCH * HTTP/1.1\r\n"
		"HOST: 239.255.255.250:1900\r\n"
		"MAN: \"ssdp:discover\"\r\n"
		"MX: 1\r\n"
		"ST: urn:schemas-upnp-org:service:WANIPConnection:1\r\n\r\n");
	(void)sendto(s, msg, strlen(msg), 0, (struct sockaddr *)&dst, sizeof(dst));

	snprintf(msg, sizeof(msg),
		"M-SEARCH * HTTP/1.1\r\n"
		"HOST: 239.255.255.250:1900\r\n"
		"MAN: \"ssdp:discover\"\r\n"
		"MX: 1\r\n"
		"ST: urn:schemas-upnp-org:service:WANPPPConnection:1\r\n\r\n");
	(void)sendto(s, msg, strlen(msg), 0, (struct sockaddr *)&dst, sizeof(dst));

	for(;;) {
		srclen = sizeof(src);
		n = recvfrom(s, buf, sizeof(buf) - 1, 0, (struct sockaddr *)&src, &srclen);
		if(n < 0)
			break;
		buf[n] = '\0';
		/* Find LOCATION header */
		{
			char *p = buf;
			while(p && *p) {
				char *eol = strstr(p, "\r\n");
				if(!eol)
					break;
				*eol = '\0';
				if(strncasecmp(p, "location:", 9) == 0) {
					const char *v = p + 9;
					while(*v == ' ' || *v == '\t') v++;
					snprintf(location, locationlen, "%s", v);
					close(s);
					return 0;
				}
				p = eol + 2;
			}
		}
	}
	close(s);
	return -1;
}

static int
igd_get_control_url(char *host, size_t hostlen, unsigned short *port, char *control_path, size_t pathlen)
{
	char location[256];
	char loc_host[64];
	char loc_path[192];
	unsigned short loc_port;
	unsigned int timeout_ms;
	int s;
	char req[512];
	char *xml;
	char *svc;
	char *ctrl;
	
	timeout_ms = proxy_upstream_timeout_ms ? proxy_upstream_timeout_ms : UPROXY_TIMEOUT_MS_DEFAULT;
	if(ssdp_discover_location(location, sizeof(location), timeout_ms) < 0)
		return -1;

	/* Build GET for the device description */
	{
		char linebuf[320];
		snprintf(linebuf, sizeof(linebuf), "LOCATION: %s", location);
		if(parse_location(linebuf, loc_host, sizeof(loc_host), &loc_port, loc_path, sizeof(loc_path)) < 0)
			return -1;
	}

	s = tcp_connect_timeout(loc_host, loc_port, timeout_ms);
	if(s < 0)
		return -1;

	snprintf(req, sizeof(req),
		"GET %s HTTP/1.1\r\n"
		"Host: %s:%hu\r\n"
		"Connection: close\r\n\r\n",
		loc_path, loc_host, loc_port);
	if(send(s, req, strlen(req), 0) < 0) {
		close(s);
		return -1;
	}

	xml = (char *)malloc(65536);
	if(!xml) {
		close(s);
		return -1;
	}
	if(http_read_all(s, xml, 65536) <= 0) {
		free(xml);
		close(s);
		return -1;
	}
	close(s);

	/* crude parsing: find a service block containing WANIPConnection or WANPPPConnection
	 * then read its <controlURL>...</controlURL>
	 */
	svc = strstr(xml, "WANIPConnection");
	if(!svc)
		svc = strstr(xml, "WANPPPConnection");
	if(!svc) {
		free(xml);
		return -1;
	}
	ctrl = strstr(svc, "<controlURL>");
	if(!ctrl) {
		free(xml);
		return -1;
	}
	ctrl += strlen("<controlURL>");
	{
		char *end = strstr(ctrl, "</controlURL>");
		size_t l;
		if(!end) {
			free(xml);
			return -1;
		}
		l = (size_t)(end - ctrl);
		if(l + 1 > pathlen) {
			free(xml);
			return -1;
		}
		memcpy(control_path, ctrl, l);
		control_path[l] = '\0';
	}

	/* controlURL can be absolute path or full URL. We support absolute path. */
	if(strncmp(control_path, "http://", 7) == 0) {
		char linebuf[320];
		snprintf(linebuf, sizeof(linebuf), "LOCATION: %s", control_path);
		free(xml);
		if(parse_location(linebuf, host, hostlen, port, control_path, pathlen) < 0)
			return -1;
		return 0;
	}

	snprintf(host, hostlen, "%s", loc_host);
	*port = loc_port;
	if(control_path[0] != '/') {
		char tmp[192];
		snprintf(tmp, sizeof(tmp), "/%s", control_path);
		snprintf(control_path, pathlen, "%s", tmp);
	}

	free(xml);
	return 0;
}

static int
igd_soap(int is_add, int proto, unsigned short eport, unsigned int lifetime)
{
	char host[64];
	unsigned short port;
	char path[192];
	unsigned int timeout_ms;
	int s;
	char wan_ip[INET_ADDRSTRLEN];
	char body[1024];
	char req[2048];
	char *resp;
	int resp_len;
	int rc = -1;

	if(!proxy_upstream_enabled)
		return -1;

	if(get_wan_ipv4_str(wan_ip, sizeof(wan_ip)) < 0)
		return -1;

	if(igd_get_control_url(host, sizeof(host), &port, path, sizeof(path)) < 0)
		return -1;

	timeout_ms = proxy_upstream_timeout_ms ? proxy_upstream_timeout_ms : UPROXY_TIMEOUT_MS_DEFAULT;
	s = tcp_connect_timeout(host, port, timeout_ms);
	if(s < 0)
		return -1;

	if(is_add) {
		snprintf(body, sizeof(body),
			"<?xml version=\"1.0\"?>"
			"<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
			"s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\">"
			"<s:Body>"
			"<u:AddPortMapping xmlns:u=\"urn:schemas-upnp-org:service:WANIPConnection:1\">"
			"<NewRemoteHost></NewRemoteHost>"
			"<NewExternalPort>%hu</NewExternalPort>"
			"<NewProtocol>%s</NewProtocol>"
			"<NewInternalPort>%hu</NewInternalPort>"
			"<NewInternalClient>%s</NewInternalClient>"
			"<NewEnabled>1</NewEnabled>"
			"<NewPortMappingDescription>FreshTomato Proxy</NewPortMappingDescription>"
			"<NewLeaseDuration>%u</NewLeaseDuration>"
			"</u:AddPortMapping>"
			"</s:Body></s:Envelope>",
			eport, (proto==IPPROTO_TCP)?"TCP":"UDP", eport, wan_ip, lifetime);
	} else {
		snprintf(body, sizeof(body),
			"<?xml version=\"1.0\"?>"
			"<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
			"s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\">"
			"<s:Body>"
			"<u:DeletePortMapping xmlns:u=\"urn:schemas-upnp-org:service:WANIPConnection:1\">"
			"<NewRemoteHost></NewRemoteHost>"
			"<NewExternalPort>%hu</NewExternalPort>"
			"<NewProtocol>%s</NewProtocol>"
			"</u:DeletePortMapping>"
			"</s:Body></s:Envelope>",
			eport, (proto==IPPROTO_TCP)?"TCP":"UDP");
	}

	snprintf(req, sizeof(req),
		"POST %s HTTP/1.1\r\n"
		"Host: %s:%hu\r\n"
		"Content-Type: text/xml; charset=\"utf-8\"\r\n"
		"SOAPAction: \"urn:schemas-upnp-org:service:WANIPConnection:1#%s\"\r\n"
		"Content-Length: %u\r\n"
		"Connection: close\r\n\r\n"
		"%s",
		path, host, port, is_add ? "AddPortMapping" : "DeletePortMapping",
		(unsigned)strlen(body), body);

	if(send(s, req, strlen(req), 0) < 0) {
		close(s);
		return -1;
	}

	resp = (char *)malloc(65536);
	if(!resp) {
		close(s);
		return -1;
	}
	resp_len = http_read_all(s, resp, 65536);
	close(s);
	if(resp_len <= 0) {
		free(resp);
		return -1;
	}

	if(strstr(resp, " 200 ") || strstr(resp, " 204 ")) {
		rc = 0;
	} else {
		/* Map common IGD faults to retryable/denied when possible */
		char *p = strstr(resp, "<errorCode>");
		if(p) {
			int code = atoi(p + strlen("<errorCode>"));
			if(code == 718 || code == 729)
				rc = UPROXY_RETRYABLE;
			else if(code == 725 || code == 606)
				rc = UPROXY_DENIED;
		}
		if(rc != UPROXY_RETRYABLE && rc != UPROXY_DENIED)
			rc = -1;
	}

	free(resp);
	return rc;
}

static int
igd_map(int proto, unsigned short eport, unsigned int lifetime)
{
	return igd_soap(1, proto, eport, lifetime);
}

static int
igd_unmap(int proto, unsigned short eport)
{
	return igd_soap(0, proto, eport, 0);
}

/* ---------------- Public API ---------------- */

static int
try_protocols_add(int proto, unsigned short eport, unsigned int lifetime)
{
	int r;
	int saw_retryable = 0;
	int saw_denied = 0;

	/* Prefer PCP, then NAT-PMP, then IGD */
	r = pcp_map(proto, eport, lifetime);
	if(r == 0) return 0;
	if(r == UPROXY_RETRYABLE) saw_retryable = 1;
	else if(r == UPROXY_DENIED) saw_denied = 1;

	r = natpmp_map(proto, eport, lifetime);
	if(r == 0) return 0;
	if(r == UPROXY_RETRYABLE) saw_retryable = 1;
	else if(r == UPROXY_DENIED) saw_denied = 1;

	r = igd_map(proto, eport, lifetime);
	if(r == 0) return 0;
	if(r == UPROXY_RETRYABLE) saw_retryable = 1;
	else if(r == UPROXY_DENIED) saw_denied = 1;

	if(saw_retryable) return UPROXY_RETRYABLE;
	if(saw_denied) return UPROXY_DENIED;
	return -1;
}

static int
try_protocols_renew(int proto, unsigned short eport, unsigned int lifetime)
{
	return try_protocols_add(proto, eport, lifetime);
}

static int
try_protocols_del(int proto, unsigned short eport)
{
	int r;
	int saw_denied = 0;
	/* Best-effort delete: try all; treat success if any succeeded */
	r = pcp_unmap(proto, eport);
	if(r == 0) return 0;
	if(r == UPROXY_DENIED) saw_denied = 1;
	r = natpmp_unmap(proto, eport);
	if(r == 0) return 0;
	if(r == UPROXY_DENIED) saw_denied = 1;
	r = igd_unmap(proto, eport);
	if(r == 0) return 0;
	if(r == UPROXY_DENIED) saw_denied = 1;
	return saw_denied ? UPROXY_DENIED : -1;
}

int
upstreamproxy_add(int proto, unsigned short eport, unsigned int lifetime)
{
	if(!proxy_upstream_enabled)
		return 0;
	return try_protocols_add(proto, eport, lifetime);
}

int
upstreamproxy_renew(int proto, unsigned short eport, unsigned int lifetime)
{
	if(!proxy_upstream_enabled)
		return 0;
	return try_protocols_renew(proto, eport, lifetime);
}

int
upstreamproxy_del(int proto, unsigned short eport)
{
	if(!proxy_upstream_enabled)
		return 0;
	return try_protocols_del(proto, eport);
}
