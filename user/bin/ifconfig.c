/* ifconfig - show the network configuration and packet counters */
#include <stdio.h>
#include <unistd.h>
#include <sys/socket.h>

int main(void) {
    struct netinfo n;
    char a[16], b[16], c[16], d[16];
    if (netinfo(&n) < 0 || !n.present) { printf("ifconfig: no network interface\n"); return 1; }
    for (int i = 0; i < 50 && !n.up; i++) { usleep(100000); netinfo(&n); }   /* DHCP may still be running at boot */
    printf("eth0  e1000  HWaddr %02x:%02x:%02x:%02x:%02x:%02x\n", n.mac[0], n.mac[1], n.mac[2], n.mac[3], n.mac[4], n.mac[5]);
    if (!n.up) { printf("      no DHCP lease\n"); }
    else printf("      inet %s  mask %s  gw %s  dns %s\n", inet_ntoa_r(n.ip, a), inet_ntoa_r(n.mask, b),
                inet_ntoa_r(n.gw, c), inet_ntoa_r(n.dns, d));
    printf("      RX %u packets (%u bytes)  TX %u packets (%u bytes)\n", n.rx_pkts, n.rx_bytes, n.tx_pkts, n.tx_bytes);
    return 0;
}
