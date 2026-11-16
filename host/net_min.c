/* A minimal stand-in for net_slirp.c, for builds without libslirp (Android).
 *
 * It answers only what a guest needs to bring an interface up and stop waiting:
 * ARP for the gateway/nameserver, DHCP DISCOVER/REQUEST, and ICMP echo to the
 * gateway. Everything else is dropped. That is enough to find out whether a
 * guest that stalls in launchd is stalled because its network never came up;
 * it is NOT a NAT and gives the guest no connectivity beyond those three
 * exchanges. Replace it with net_slirp.c once libslirp is available.
 *
 * Addresses match net_slirp.c, so a guest configured for either behaves the
 * same: 10.0.2.0/24, host (gateway/nameserver) 10.0.2.2, DNS 10.0.2.3,
 * guest 10.0.2.15.
 *
 * Threading: hv_virtio_net_receive runs on the CPU thread, which is also where
 * hv_net_poll runs, so one lock is enough and the queue is drained in place.
 */
#include "net_slirp.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#define MAX_FRAME 2048
#define QUEUE 32
#define ETH_HDR 14

uint64_t hv_net_tx_frames, hv_net_rx_frames;

static HvVirtioNet *net;
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;

static struct frame {
	uint16_t len;
	uint8_t data[MAX_FRAME];
} q[QUEUE];
static unsigned qhead, qtail;

static const uint8_t host_mac[6] = { 0x02, 0x48, 0x56, 0x4d, 0x00, 0x02 };
static const uint8_t bcast_mac[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
static const uint8_t host_ip[4] = { 10, 0, 2, 2 };
static const uint8_t dns_ip[4] = { 10, 0, 2, 3 };
static const uint8_t guest_ip[4] = { 10, 0, 2, 15 };
static const uint8_t netmask[4] = { 255, 255, 255, 0 };

/* ---- transmit queue ---- */

static void enqueue(const uint8_t *f, uint16_t len) {
	pthread_mutex_lock(&mu);
	if (qtail - qhead < QUEUE && len <= MAX_FRAME) {
		struct frame *e = &q[qtail % QUEUE];
		e->len = len;
		memcpy(e->data, f, len);
		qtail++;
	}
	pthread_mutex_unlock(&mu);
}

/* ---- checksums ---- */

static uint32_t sum16(const uint8_t *p, unsigned n, uint32_t sum) {
	for (unsigned i = 0; i + 1 < n; i += 2) sum += (uint32_t)(p[i] << 8 | p[i + 1]);
	if (n & 1) sum += (uint32_t)(p[n - 1] << 8);
	return sum;
}

static uint16_t fold(uint32_t sum) {
	while (sum >> 16) sum = (sum & 0xffff) + (sum >> 16);
	return (uint16_t)~sum;
}

/* ---- reply builders ---- */

/* Ethernet + IPv4 + UDP, returns the offset of the UDP payload, or 0 when the
 * frame does not fit. The caller fills the payload and sets the UDP length. */
static unsigned build_udp(uint8_t *f, const uint8_t *dstmac, const uint8_t *srcip,
                          const uint8_t *dstip, uint16_t sport, uint16_t dport,
                          unsigned payload) {
	unsigned udp = ETH_HDR + 20, total = udp + 8 + payload;
	if (total > MAX_FRAME) return 0;
	memset(f, 0, udp + 8);
	memcpy(f, dstmac, 6);
	memcpy(f + 6, host_mac, 6);
	f[12] = 0x08; f[13] = 0x00;
	f[14] = 0x45;                      /* v4, 20-byte header */
	f[16] = (uint8_t)(total >> 8); f[17] = (uint8_t)total;
	f[18] = 0x40; f[19] = 0;           /* DF */
	f[22] = 64;                        /* ttl */
	f[23] = 17;                        /* UDP */
	memcpy(f + 26, srcip, 4);
	memcpy(f + 30, dstip, 4);
	uint16_t ipck = fold(sum16(f + 14, 20, 0));
	f[24] = (uint8_t)(ipck >> 8); f[25] = (uint8_t)ipck;
	unsigned ulen = 8 + payload;
	f[udp + 0] = (uint8_t)(sport >> 8); f[udp + 1] = (uint8_t)sport;
	f[udp + 2] = (uint8_t)(dport >> 8); f[udp + 3] = (uint8_t)dport;
	f[udp + 4] = (uint8_t)(ulen >> 8); f[udp + 5] = (uint8_t)ulen;
	return udp + 8;
}

static void reply_arp(const uint8_t *in, uint16_t len) {
	if (len < ETH_HDR + 28) return;
	const uint8_t *arp = in + ETH_HDR;
	if (arp[6] != 0x08 || arp[7] != 0x00) return;   /* hardware type */
	if (arp[2] != 0x08 || arp[3] != 0x00) return;   /* protocol type */
	if (arp[5] != 1) return;                        /* request */
	if (memcmp(arp + 14, host_ip, 4) && memcmp(arp + 14, dns_ip, 4)
	    && memcmp(arp + 14, guest_ip, 4)) return;

	uint8_t f[42];
	memcpy(f, in + 6, 6);                           /* back to the sender */
	memcpy(f + 6, host_mac, 6);
	f[12] = 0x08; f[13] = 0x06;
	f[14] = 0x00; f[15] = 0x01; f[16] = 0x08; f[17] = 0x00;
	f[18] = 6; f[19] = 4; f[20] = 0x00; f[21] = 0x02;   /* reply */
	memcpy(f + 22, host_mac, 6);                    /* sender MAC */
	const uint8_t *asked = arp + 14;
	memcpy(f + 28, memcmp(asked, dns_ip, 4) == 0 ? dns_ip : host_ip, 4);
	memcpy(f + 32, in + 22, 6);                     /* target MAC */
	memcpy(f + 38, arp + 24, 4);                    /* target IP */
	enqueue(f, sizeof f);
	hv_net_rx_frames++;
}

static void reply_icmp(const uint8_t *in, uint16_t len) {
	unsigned ihl = (unsigned)(in[ETH_HDR] & 0x0f) * 4;
	unsigned total = (unsigned)(in[ETH_HDR + 2] << 8 | in[ETH_HDR + 3]);
	if (ihl < 20 || len < ETH_HDR + ihl + 8 || total > len - ETH_HDR) return;
	const uint8_t *icmp = in + ETH_HDR + ihl;
	if (icmp[0] != 8) return;                       /* echo request only */

	unsigned plen = total - ihl;
	uint8_t f[MAX_FRAME];
	unsigned off = ETH_HDR + 20 + 8;
	if (off + plen > sizeof f) return;
	memcpy(f, in + 6, 6);
	memcpy(f + 6, host_mac, 6);
	f[12] = 0x08; f[13] = 0x00;
	unsigned ipend = 20 + plen;
	f[14] = 0x45;
	f[16] = (uint8_t)(ipend >> 8); f[17] = (uint8_t)ipend;
	f[22] = 64; f[23] = 1;                          /* ICMP */
	memcpy(f + 26, in + ETH_HDR + 16, 4);           /* source becomes destination */
	memcpy(f + 30, host_ip, 4);
	uint16_t ipck = fold(sum16(f + 14, 20, 0));
	f[24] = (uint8_t)(ipck >> 8); f[25] = (uint8_t)ipck;
	memcpy(f + off, icmp, plen);
	f[off] = 0;                                     /* echo reply */
	f[off + 2] = 0; f[off + 3] = 0;
	uint16_t ck = fold(sum16(f + off, plen, 0));
	f[off + 2] = (uint8_t)(ck >> 8); f[off + 3] = (uint8_t)ck;
	enqueue(f, (uint16_t)(off + plen));
	hv_net_rx_frames++;
}

/* DHCP: BOOTP header, 236 bytes, then the options cookie and the option list. */
#define BOOTP_FIXED 236

static void reply_dhcp(const uint8_t *in, uint16_t len) {
	unsigned ihl = (unsigned)(in[ETH_HDR] & 0x0f) * 4;
	unsigned udp = ETH_HDR + ihl;
	if (len < udp + 8 + BOOTP_FIXED + 4) return;
	const uint8_t *b = in + udp + 8;
	if (b[0] != 1) return;                          /* BOOTREQUEST */
	uint32_t magic = (uint32_t)b[236] << 24 | (uint32_t)b[237] << 16
	                 | (uint32_t)b[238] << 8 | b[239];
	if (magic != 0x63825363u) return;

	/* Find the message type option (53). */
	uint8_t type = 0;
	for (unsigned i = 240; i < BOOTP_FIXED + 4 + 60 && i < len - udp - 8;) {
		uint8_t opt = b[i];
		if (opt == 0) { i++; continue; }
		if (opt == 255) break;
		if (i + 1 >= len - udp - 8) break;
		uint8_t olen = b[i + 1];
		if (opt == 53 && olen >= 1) type = b[i + 2];
		i += 2u + olen;
	}
	if (type != 1 && type != 3) return;             /* DISCOVER or REQUEST */
	uint8_t reply_type = (type == 1) ? 2 : 5;       /* OFFER or ACK */

	/* A client with no address yet cannot be unicast to: the reply goes to the
	 * broadcast address, which is also why the destination MAC is all-ones. */
	static const uint8_t all4[4] = { 255, 255, 255, 255 };
	uint8_t f[MAX_FRAME];
	unsigned payload = BOOTP_FIXED + 4 + 64;
	unsigned off = build_udp(f, bcast_mac, host_ip, all4, 67, 68, payload);
	if (!off) return;

	uint8_t *r = f + off;
	memset(r, 0, payload);
	r[0] = 2;                                       /* BOOTREPLY */
	r[1] = 1; r[2] = 6; r[3] = 0;
	memcpy(r + 4, b + 4, 4);                        /* xid */
	r[10] = b[10]; r[11] = b[11];                   /* flags */
	memcpy(r + 16, guest_ip, 4);                    /* yiaddr */
	memcpy(r + 20, host_ip, 4);                     /* siaddr */
	memcpy(r + 28, b + 28, 16);                     /* chaddr */
	r[236] = 0x63; r[237] = 0x82; r[238] = 0x53; r[239] = 0x63;
	unsigned o = 240;
	r[o++] = 53; r[o++] = 1; r[o++] = reply_type;
	r[o++] = 54; r[o++] = 4; memcpy(r + o, host_ip, 4); o += 4;
	r[o++] = 51; r[o++] = 4; r[o++] = 0x00; r[o++] = 0x00; r[o++] = 0x0e; r[o++] = 0x10;
	r[o++] = 1; r[o++] = 4; memcpy(r + o, netmask, 4); o += 4;
	r[o++] = 3; r[o++] = 4; memcpy(r + o, host_ip, 4); o += 4;
	r[o++] = 6; r[o++] = 4; memcpy(r + o, dns_ip, 4); o += 4;
	r[o++] = 255;
	uint16_t ulen = (uint16_t)(8 + o);
	f[udp + 4] = (uint8_t)(ulen >> 8); f[udp + 5] = (uint8_t)ulen;
	enqueue(f, (uint16_t)(off + o));
	hv_net_rx_frames++;
}

/* ---- the device hook ---- */

static void transmit(void *opaque, const uint8_t *frame, uint32_t len) {
	(void)opaque;
	if (!frame || len < ETH_HDR || len > MAX_FRAME) return;
	hv_net_tx_frames++;
	uint16_t type = (uint16_t)(frame[12] << 8 | frame[13]);
	if (type == 0x0806) {
		reply_arp(frame, (uint16_t)len);
	} else if (type == 0x0800 && len > ETH_HDR + 20) {
		uint8_t proto = frame[ETH_HDR + 9];
		if (proto == 1) reply_icmp(frame, (uint16_t)len);
		else if (proto == 17) {
			unsigned ihl = (unsigned)(frame[ETH_HDR] & 0x0f) * 4;
			unsigned udp = ETH_HDR + ihl;
			if (len >= udp + 8 && frame[udp] == 0 && frame[udp + 1] == 67) reply_dhcp(frame, (uint16_t)len);
		}
	}
}

int hv_net_attach(HvMachine *m, HvVirtioNet *n) {
	(void)m;
	if (!n || net) return -1;
	net = n;
	qhead = qtail = 0;
	hv_net_tx_frames = hv_net_rx_frames = 0;
	n->transmit = transmit;
	n->opaque = NULL;
	return 0;
}

void hv_net_detach(void) {
	net = NULL;
	qhead = qtail = 0;
}

void hv_net_poll(void) {
	for (;;) {
		pthread_mutex_lock(&mu);
		struct frame f;
		int have = qhead != qtail;
		if (have) f = q[qhead % QUEUE];
		pthread_mutex_unlock(&mu);
		if (!have) return;
		if (!hv_virtio_net_receive(net, f.data, f.len)) return;  /* no RX buffer yet */
		pthread_mutex_lock(&mu);
		qhead++;
		pthread_mutex_unlock(&mu);
	}
}

void hv_net_set_share_socket(const char *path) {
	(void)path;
}
