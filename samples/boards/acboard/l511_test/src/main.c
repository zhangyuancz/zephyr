/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 *
 * Brings up the Lynq L511 cellular modem through its driver and waits for the
 * PPP network interface to reach connectivity, then prints the assigned IP.
 */

#include <zephyr/kernel.h>
#include <zephyr/net/net_event.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/sys/printk.h>

static K_SEM_DEFINE(net_connected, 0, 1);
static struct net_mgmt_event_callback l4_cb;

static void l4_handler(struct net_mgmt_event_callback *cb, uint64_t event, struct net_if *iface)
{
	ARG_UNUSED(cb);
	ARG_UNUSED(iface);

	switch (event) {
	case NET_EVENT_L4_CONNECTED:
		printk("network connected\n");
		k_sem_give(&net_connected);
		break;
	case NET_EVENT_L4_DISCONNECTED:
		printk("network disconnected\n");
		break;
	default:
		break;
	}
}

static void print_addresses(struct net_if *iface)
{
	char buf[NET_IPV4_ADDR_LEN];

	for (int i = 0; i < NET_IF_MAX_IPV4_ADDR; i++) {
		struct net_if_addr *unicast = &iface->config.ip.ipv4->unicast[i].ipv4;

		if (!unicast->is_used) {
			continue;
		}
		printk("IPv4: %s\n",
		       net_addr_ntop(AF_INET, &unicast->address.in_addr, buf, sizeof(buf)));
	}
}

int main(void)
{
	printk("\nACBoard Lynq L511 modem test\n");

	net_mgmt_init_event_callback(&l4_cb, l4_handler,
				     NET_EVENT_L4_CONNECTED | NET_EVENT_L4_DISCONNECTED);
	net_mgmt_add_event_callback(&l4_cb);

	printk("powering modem and dialing (up to 120s)...\n");
	if (k_sem_take(&net_connected, K_SECONDS(120)) != 0) {
		printk("no connectivity within timeout\n");
		return 0;
	}

	struct net_if *iface = net_if_get_default();

	if (iface != NULL && iface->config.ip.ipv4 != NULL) {
		print_addresses(iface);
	}
	printk("L511 PPP link up\n");
	return 0;
}
