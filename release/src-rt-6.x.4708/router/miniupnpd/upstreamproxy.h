/* Upstream port-mapping proxy (PCP / NAT-PMP / UPnP IGD client)
 * FreshTomato-specific: used to proxy LAN-side mappings to an upstream router.
 */
#ifndef UPSTREAMPROXY_H_INCLUDED
#define UPSTREAMPROXY_H_INCLUDED

#include <stdint.h>

/* Return codes:
 *  0  success
 * -1  hard failure (timeout, protocol error)
 * -9  retryable (cannot provide requested external port)
 * -10 denied (not authorized / refused)
 */

int upstreamproxy_add(int proto, unsigned short eport, unsigned int lifetime);
int upstreamproxy_renew(int proto, unsigned short eport, unsigned int lifetime);
int upstreamproxy_del(int proto, unsigned short eport);

#endif /* UPSTREAMPROXY_H_INCLUDED */
