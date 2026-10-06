/* The flat-wire unit target never attaches a transport delivery. Reaching a
 * delivery hook would exercise a subsystem this isolated target does not link. */
#include "core/response_delivery.h"
#include <stdlib.h>
void response_delivery_wire_acquire(response_delivery_t *d)
{
	(void)d;
	abort();
}
void response_delivery_wire_release(response_delivery_t *d)
{
	(void)d;
	abort();
}
