/* 
 * The MIT License (MIT)
 *
 * Based on tinyUSB example that is: Copyright (c) 2020 Peter Lawrence
 * Modified for Pico by Floris Bos
 *
 * influenced by lrndis https://github.com/fetisov/lrndis
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 *
 */
#include "tusb_lwip_glue.h"
#include "pico/cyw43_arch.h"
#include "wifi_code.h"
/* lwip context */
static struct netif netif_data;

/* shared between tud_network_recv_cb() and service_traffic() */
struct pbuf *received_frame;

int packet_stat_rx =0;
int packet_stat_tx=0;
int eth_frame_send_success=0;
uint8_t pattern[] = {0xc0, 0xa8, 0x07, 0x01};
/* this is used by this code, ./class/net/net_driver.c, and usb_descriptors.c */
/* ideally speaking, this should be generated from the hardware's unique ID (if available) */
/* it is suggested that the first byte is 0x02 to indicate a link-local address */
uint8_t tud_network_mac_address[6] = {0x02,0x02,0x84,0x6A,0x96,0x00};
/* network parameters of this MCU */
static const ip_addr_t ipaddr  = IPADDR4_INIT_BYTES(192, 168, 7, 1);
static const ip_addr_t netmask = IPADDR4_INIT_BYTES(255, 255, 255, 0);
static const ip_addr_t gateway = IPADDR4_INIT_BYTES(0, 0, 0, 0);

/* database IP addresses that can be offered to the host; this must be in RAM to store assigned MAC addresses */
static dhcp_entry_t entries[] =
{
    /* mac ip address                          lease time */
    { {0}, IPADDR4_INIT_BYTES(192, 168, 7, 2), 24 * 60 * 60 },
    { {0}, IPADDR4_INIT_BYTES(192, 168, 7, 3), 24 * 60 * 60 },
    { {0}, IPADDR4_INIT_BYTES(192, 168, 7, 4), 24 * 60 * 60 },
};

static const dhcp_config_t dhcp_config =
{
    .router = IPADDR4_INIT_BYTES(0, 0, 0, 0),  /* router address (if any) */
    .port = 67,                                /* listen port */
    .dns = IPADDR4_INIT_BYTES(0, 0, 0, 0),     /* dns server (if any) */
    "",                                        /* dns suffix */
    TU_ARRAY_SIZE(entries),                    /* num entry */
    entries                                    /* entries */
};

static err_t linkoutput_fn(struct netif *netif, struct pbuf *p)
{
    (void)netif;
    
    for (;;)
    {
      /* if TinyUSB isn't ready, we must signal back to lwip that there is nothing we can do */
      if (!tud_ready())
        return ERR_USE;
    
      /* if the network driver can accept another packet, we make it happen */
      if (tud_network_can_xmit(p->tot_len))
      {
        tud_network_xmit(p, 0 /* unused for this example */);
        return ERR_OK;
      }
    
      /* transfer execution to TinyUSB in the hopes that it will finish transmitting the prior packet */
      tud_task();
    }
}

static err_t output_fn(struct netif *netif, struct pbuf *p, const ip_addr_t *addr)
{
    return etharp_output(netif, p, addr);
}

static err_t netif_init_cb(struct netif *netif)
{
    LWIP_ASSERT("netif != NULL", (netif != NULL));
    netif->mtu = 1500;//CFG_TUD_NET_MTU;
    netif->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_LINK_UP | NETIF_FLAG_UP;
    netif->state = NULL;
    netif->name[0] = 'E';
    netif->name[1] = 'X';
    netif->linkoutput = linkoutput_fn;
    netif->output = output_fn;
    return ERR_OK;
}

void init_lwip(void)
{
    struct netif *netif = &netif_data;
	mutex_enter_blocking(&wifi_ready);
    tud_network_mac_address[0]=macaddr[0];
	tud_network_mac_address[1]=macaddr[1];
	tud_network_mac_address[2]=macaddr[2];
	tud_network_mac_address[3]=macaddr[3];
	tud_network_mac_address[4]=macaddr[4];
	tud_network_mac_address[5]=macaddr[5];
	mutex_exit(&wifi_ready);
    /* Fixup MAC address based on flash serial */
    //pico_unique_board_id_t id;
    //pico_get_unique_board_id(&id);
    //memcpy( (tud_network_mac_address)+1, id.id, 5);
    // Fixing up does not work because tud_network_mac_address is const
    
    /* Initialize tinyUSB */
    tusb_init();
    
    /* Initialize lwip */
    lwip_init();
    /* the lwip virtual MAC address must be different from the host's; to ensure this, we toggle the LSbit */
	//tud_network_mac_address[5]=0x00;
    netif->hwaddr_len = sizeof(tud_network_mac_address);
    memcpy(netif->hwaddr, tud_network_mac_address, sizeof(tud_network_mac_address));
    //netif->hwaddr[5] ^= 0x01;
    
    netif = netif_add(netif, &ipaddr, &netmask, &gateway, NULL, netif_init_cb, ip_input);
    netif_set_default(netif);
}

void tud_network_init_cb(void)
{
    /* if the network is re-initializing and we have a leftover packet, we must do a cleanup */
    if (received_frame)
    {
      pbuf_free(received_frame);
      received_frame = NULL;
    }
}

bool tud_network_recv_cb(const uint8_t *src, uint16_t size)
{
    /* this shouldn't happen, but if we get another packet before 
    parsing the previous, we must signal our inability to accept it */
    if (received_frame) return false;
	if (size)
    {
        struct pbuf *p = pbuf_alloc(PBUF_RAW, size, PBUF_POOL);

        if (p)
        {
            /* pbuf_alloc() has already initialized struct; all we need to do is copy the data */
            //memcpy(p->payload, src, size);
			pbuf_take(p, src, size);
			//if(size!=in_pkt.len){
			//}
            /* store away the pointer for service_traffic() to later handle */
            received_frame = p;
        }
    }

    return true;
}

uint16_t tud_network_xmit_cb(uint8_t *dst, void *ref, uint16_t arg)
{
    
  struct pbuf *p = (struct pbuf *)ref;

  (void)arg; /* unused for this example */

  return pbuf_copy_partial(p, dst, p->tot_len, 0);
}

void service_traffic(void)
{
    /* handle any packet received by tud_network_recv_cb() */
    if (received_frame)
    {
	  eth_frame_send_success=cyw43_send_ethernet(&cyw43_state, CYW43_ITF_STA, received_frame->tot_len, (void*)received_frame, true);
	  if(eth_frame_send_success==0){ // packet send pass
	  }
      pbuf_free(received_frame);
      received_frame = NULL;
	  tud_network_recv_renew();
    }
    
    //sys_check_timeouts();
}

void dhcpd_init()
{
    while (dhserv_init(&dhcp_config) != ERR_OK);    
}

void wait_for_netif_is_up()
{
    while (!netif_is_up(&netif_data));    
}

sys_prot_t sys_arch_protect(void) {
    return 0;
}

void sys_arch_unprotect(__unused sys_prot_t pval) {
}

/* lwip needs a millisecond time source, and the TinyUSB board support code has one available */
uint32_t sys_now(void) {
    return board_millis();
}