#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <arpa/inet.h>
#include <pcap/pcap.h>
#include <linux/if_ether.h>
#include <linux/ip.h>
#include <linux/tcp.h>

#define PROFINET_PORT 102

#define PROFINET_ISO_PROTOCOL 0x03

#define PROFINET_ISO_FUNCTION_PDU_TRANSPORT 0xf0
#define PROFINET_ISO_FUNCTION_CONNECT       0xe0

enum profinet_function_t
{
    profinet_function_open_connection  = 0xf0,
    profinet_function_read             = 0x04,
    profinet_function_write            = 0x05,
    profinet_function_download_request = 0x1a,
    profinet_function_download_block   = 0x1b,
    profinet_function_download_ended   = 0x1c,
    profinet_function_upload_start     = 0x1d,
    profinet_function_upload           = 0x1e,
    profinet_function_upload_end       = 0x1f,
    profinet_function_insert_block     = 0x28,
};

struct profinet_iso_header
{
    uint8_t prot;
    uint8_t ch1;
    uint8_t ch2;
    uint8_t len;
    uint8_t xxxx1;
    uint8_t func;
    uint8_t xxxx2;
};

struct profinet_ibh_header
{
    uint16_t channel;
    uint8_t len;
    uint8_t seq;
    uint16_t sflags;
    uint16_t rflags;
};

struct profinet_request
{
    struct profinet_iso_header iso;
    struct profinet_ibh_header ibh;

    uint16_t prefix;
    uint8_t unknown1;
    uint8_t read_size;
    uint16_t read_length;
    uint16_t db_num;
    uint8_t area_code;
    uint8_t start_addr;
    uint16_t start_addr_2;
};

void dump_profinet_iso_header(const struct profinet_iso_header *h, const int len)
{
    printf("Protocol = 0x%02x\n", h->prot);
    assert(h->prot == PROFINET_ISO_PROTOCOL);

    printf("Length = %d (packet length %d)\n", h->len, len);
    //assert(h->len == len);
    switch (h->func)
    {
        case PROFINET_ISO_FUNCTION_PDU_TRANSPORT:
            printf("Function = PDU Transport\n");
            break;
        case PROFINET_ISO_FUNCTION_CONNECT:
            printf("Function = Connect to rack\n");
            break;
        default:
            printf("Protocol = UNKNOWN (0x%02x)\n", h->func);
            assert(false);
    }
}

void dump_profinet_ibh_header(const struct profinet_ibh_header *ibh)
{
    printf("IBH channel: 0x%04x\n", ibh->channel);
    printf("IBH len: %d\n", ibh->len);
    printf("IBH seq: %d\n", ibh->seq);
    printf("IBH sflags: 0x%04x\n", ibh->sflags);
    printf("IBH rflags: 0x%04x\n", ibh->rflags);
}

void dump_profinet_request(const struct profinet_request *r, const int len)
{
    dump_profinet_iso_header(&r->iso, len);
    dump_profinet_ibh_header(&r->ibh);

    printf("Prefix = 0x%04x\n", r->prefix);
    printf("Read size = %d\n", r->read_size);
    printf("Length = %d\n", r->read_length);

    assert(r->ibh.channel == 7);
    assert(r->iso.len == len);
}

void pcap_parse_profinet_request(u_char *user, const u_char *bytes, const int len)
{
    printf("===== REQUEST ==========================\n");

    struct profinet_request *r = (struct profinet_request*)bytes;
    dump_profinet_request(r, len);
}

void pcap_parse_profinet_response(u_char *user, const u_char *bytes, const int len)
{
    printf("===== RESPONSE =========================\n");

    struct profinet_request *r = (struct profinet_request*)bytes;
    dump_profinet_request(r, len);
}

void pcap_parse_tcp(u_char *user, const u_char *bytes, const int len)
{
    assert(! user);

    if (len < sizeof(struct tcphdr))
        return;

    struct tcphdr *tcph = (struct tcphdr*)bytes;
    uint16_t dst_port = ntohs(tcph->dest);
    uint16_t src_port = ntohs(tcph->source);
    int hdr_len = tcph->doff * 4;
    assert(hdr_len >= sizeof(struct tcphdr));

    int payload_len = len - hdr_len;
    if (payload_len < sizeof(struct profinet_request))
        return;

    if (dst_port == PROFINET_PORT)
        pcap_parse_profinet_request(user, bytes + hdr_len,
                payload_len);
    else if (src_port == PROFINET_PORT)
        pcap_parse_profinet_response(user, bytes + hdr_len,
                payload_len);
    else
        printf("Unknown connection at dest port = %d, src_port = %d\n",
                dst_port, src_port);
}

void pcap_parse_ip4(u_char *user, const u_char *bytes, const int len)
{
    assert(! user);

    if (len < sizeof(struct iphdr))
        return;

    struct iphdr *iph = (struct iphdr*)bytes;
    assert(iph->version == 4);

    uint8_t ip_proto = iph->protocol;
    uint8_t header_len = iph->ihl * 4;

    switch (ip_proto)
    {
        case IPPROTO_TCP:
            pcap_parse_tcp(user, bytes + header_len, len - header_len);
            break;
        case IPPROTO_UDP:
        default:
            printf("Unknown IP protocol %d\n", ip_proto);
    }
}

void pcap_callback(u_char *user, const struct pcap_pkthdr *h,
        const u_char *bytes)
{
    assert(! user);

    if (h->caplen < sizeof(struct ethhdr))
        return;

    struct ethhdr *eh = (struct ethhdr*)bytes;
    uint16_t eth_proto = ntohs(eh->h_proto);
    switch (eth_proto)
    {
        case ETH_P_IP:
            pcap_parse_ip4(user, bytes + sizeof(struct ethhdr), h->caplen - sizeof(struct ethhdr));
            break;
        case ETH_P_ARP:
            // We don't care about arp.
            break;
        default:
            printf("Unknown ethernet protocol = %02x\n", eth_proto);
            break;
    }
}

int main(int argc, char** argv)
{
    char errbuf[PCAP_ERRBUF_SIZE];

    if (argc < 2)
    {
        printf("Usage: %s <file.pcap>\n", argv[0]);
        return 1;
    }

    pcap_t *p = pcap_open_offline(argv[1], errbuf);
    if (! p)
    {
        printf("Failed to open file %s: %s\n", argv[1], errbuf);
        return 1;
    }

    int ret = pcap_loop(p, 0, pcap_callback, NULL);
    if (ret < 0)
    {
        printf("pcap_loop error %d\n", ret);
        return 1;
    }

    return 0;
}
