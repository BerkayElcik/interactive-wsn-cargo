#include "contiki.h"
#include "net/nullnet/nullnet.h"
#include "net/netstack.h"
#include "net/packetbuf.h" 
#include "dev/leds.h"
#include "random.h"
#include <stdio.h>
#include <string.h>
#include "routing.h"

#define BEACON_INTERVAL (CLOCK_SECOND * 3)
#define WATCHDOG_TIMEOUT (CLOCK_SECOND * 10)

static uint8_t my_node_id;
static route_entry_t routing_table[MAX_NODES];

// Static buffers for memory safety
static net_packet_t tx_beacon_buf;
static net_packet_t tx_cargo_buf;

PROCESS(relay_beacon_process, "Relay Beacon Process");
PROCESS(watchdog_process, "Routing Watchdog Process");
AUTOSTART_PROCESSES(&relay_beacon_process, &watchdog_process);

static void set_linkaddr_from_id(linkaddr_t *addr, uint8_t id) {
    memset(addr, 0, LINKADDR_SIZE);
    addr->u8[1] = id;
}

//uint8_t calculate_rssi_penalty(int16_t rssi) {
//    if (rssi >= -50) return 1;
//    if (rssi >= -70) return 5;
//    if (rssi >= -85) return 20;
//    return 50;
//}
uint8_t calculate_rssi_penalty(int16_t rssi) {
    if (rssi >= -55) return 1;    // Strong, close connection
    if (rssi >= -70) return 4;    // Medium connection
    if (rssi >= -85) return 15;   // Weak, distant connection
    return 50;                    // Dead end / unreliable
}

void input_callback(const void *data, uint16_t len, const linkaddr_t *src, const linkaddr_t *dest) {
    if(len != sizeof(net_packet_t)) return;
    
    net_packet_t rx_packet;
    memcpy(&rx_packet, data, len);
    net_packet_t *rx = &rx_packet; 
    
    uint8_t neighbor_id = src->u8[1]; 

    if (rx->type == PACKET_TYPE_BEACON) {
        int16_t current_rssi = packetbuf_attr(PACKETBUF_ATTR_RSSI);
        uint8_t link_penalty = calculate_rssi_penalty(current_rssi);

        routing_table[neighbor_id - 1].last_seen = clock_seconds();
        routing_table[neighbor_id - 1].cost = link_penalty;
        routing_table[neighbor_id - 1].next_hop = neighbor_id;
        
        for (int i = 0; i < MAX_NODES; i++) {
            if (i + 1 == my_node_id) continue;
            uint8_t advertised_cost = rx->payload.beacon.costs[i];
            
            if (advertised_cost != INFINITY_COST) {
                uint16_t raw_cost = advertised_cost + link_penalty;
                uint8_t proposed_cost = (raw_cost > 254) ? INFINITY_COST : (uint8_t)raw_cost;
                
                if (proposed_cost < routing_table[i].cost) {
                    routing_table[i].cost = proposed_cost;
                    routing_table[i].next_hop = neighbor_id;
                    routing_table[i].last_seen = clock_seconds(); 
                } else if (routing_table[i].next_hop == neighbor_id) {
                    routing_table[i].cost = proposed_cost;
                    routing_table[i].last_seen = clock_seconds(); 
                }
            }
        }
    } 
    else if (rx->type == PACKET_TYPE_REPORT) {
        // --- FIX: WE MUST FORWARD REPORTS BACK TO NODE 1 ---
        uint8_t return_hop = routing_table[0].next_hop; 
        if(return_hop != INFINITY_COST) {
            linkaddr_t ret_mac;
            set_linkaddr_from_id(&ret_mac, return_hop);
            
            // Copy the report exactly as we received it
            memcpy(&tx_cargo_buf, rx, sizeof(net_packet_t));
            
            nullnet_buf = (uint8_t *)&tx_cargo_buf;
            nullnet_len = sizeof(net_packet_t);
            NETSTACK_NETWORK.output(&ret_mac);
            
            // Optional: Print a small debug so you know this node is passing it back
            printf("[PASS] Bouncing Report for Cargo #%d toward Node 1\n", rx->payload.report.cargo_id);
        }
    }
    else if (rx->type == PACKET_TYPE_CARGO) {
        uint8_t target_dest = rx->payload.cargo.final_dest_id;
        
        // Case A: This node IS the final destination!
        if (target_dest == my_node_id) {
            if(rx->payload.cargo.hop_count < MAX_HOPS) {
                rx->payload.cargo.route_history[rx->payload.cargo.hop_count++] = my_node_id;
            }
            
            // --- NEW: THE JUDGEMENT PHASE ---
            char true_lbl = rx->payload.cargo.true_label;
            char sel_lbl = rx->payload.cargo.selected_label;
            uint8_t did_match = (true_lbl == sel_lbl) ? 1 : 0;
            
            printf("[DELIVERY] Cargo #%d reached target! Match Status: %s (True: %c, Selected: %c)\n", 
                    rx->payload.cargo.packet_id, did_match ? "SUCCESS" : "FAIL", true_lbl, sel_lbl);
            leds_toggle(LED_2); 
            
            // Generate REPORT back to Operator
            tx_cargo_buf.type = PACKET_TYPE_REPORT;
            tx_cargo_buf.src_id = my_node_id;
            tx_cargo_buf.payload.report.cargo_id = rx->payload.cargo.packet_id;
            tx_cargo_buf.payload.report.action = ACTION_DELIVERED;
            tx_cargo_buf.payload.report.hop_count = rx->payload.cargo.hop_count;
            tx_cargo_buf.payload.report.match_success = did_match; // <--- The Grade!
            memcpy(tx_cargo_buf.payload.report.route_history, rx->payload.cargo.route_history, rx->payload.cargo.hop_count);
            
            uint8_t n_hop = routing_table[0].next_hop; 
            if(n_hop != INFINITY_COST) {
                linkaddr_t next_mac;
                set_linkaddr_from_id(&next_mac, n_hop);
                nullnet_buf = (uint8_t *)&tx_cargo_buf;
                nullnet_len = sizeof(net_packet_t);
                NETSTACK_NETWORK.output(&next_mac);
            }
        } 
        // Case B: We are an intermediate relay. Forward it!
        else {
            uint8_t n_hop = routing_table[target_dest - 1].next_hop;
            
            if (n_hop != INFINITY_COST && rx->payload.cargo.hop_count < MAX_HOPS) {
                rx->payload.cargo.route_history[rx->payload.cargo.hop_count++] = my_node_id;
                
                linkaddr_t next_mac;
                set_linkaddr_from_id(&next_mac, n_hop);
                memcpy(&tx_cargo_buf, rx, sizeof(net_packet_t));
                nullnet_buf = (uint8_t *)&tx_cargo_buf;
                nullnet_len = sizeof(net_packet_t);
                NETSTACK_NETWORK.output(&next_mac);
                
            } else {
                printf("[ROUTING] Cargo #%d dropped! Node %d has no route to %d\n", rx->payload.cargo.packet_id, my_node_id, target_dest);
                leds_toggle(LED_3); 
                
                tx_cargo_buf.type = PACKET_TYPE_REPORT;
                tx_cargo_buf.src_id = my_node_id;
                tx_cargo_buf.payload.report.cargo_id = rx->payload.cargo.packet_id;
                tx_cargo_buf.payload.report.action = ACTION_DROPPED;
                
                uint8_t return_hop = routing_table[0].next_hop;
                if(return_hop != INFINITY_COST) {
                    linkaddr_t ret_mac;
                    set_linkaddr_from_id(&ret_mac, return_hop);
                    nullnet_buf = (uint8_t *)&tx_cargo_buf;
                    nullnet_len = sizeof(net_packet_t);
                    NETSTACK_NETWORK.output(&ret_mac);
                }
            }
        }
    }
}

PROCESS_THREAD(relay_beacon_process, ev, data) {
    static struct etimer beacon_timer;
    PROCESS_BEGIN();

    int8_t tx_power = -30;
    NETSTACK_RADIO.set_value(RADIO_PARAM_TXPOWER, tx_power);

    // ⚠️ CAUTION: Change this ID manually before flashing each relay board ⚠️
    my_node_id = 4; 
    
    linkaddr_t new_mac;
    memset(&new_mac, 0, LINKADDR_SIZE);
    new_mac.u8[1] = my_node_id;
    linkaddr_set_node_addr(&new_mac);

    for (int i = 0; i < MAX_NODES; i++) routing_table[i].cost = INFINITY_COST;
    
    routing_table[my_node_id - 1].cost = 0;
    routing_table[my_node_id - 1].next_hop = my_node_id;

    nullnet_set_input_callback(input_callback);
    etimer_set(&beacon_timer, BEACON_INTERVAL);

    printf("\n--- RELAY/TARGET NODE %d READY ---\n", my_node_id);
    printf("Standing by to forward and deliver cargo.\n");

    while(1) {
        PROCESS_WAIT_EVENT_UNTIL(ev == PROCESS_EVENT_TIMER && data == &beacon_timer);
        
        tx_beacon_buf.type = PACKET_TYPE_BEACON;
        tx_beacon_buf.src_id = my_node_id;
        for (int i = 0; i < MAX_NODES; i++) tx_beacon_buf.payload.beacon.costs[i] = routing_table[i].cost;
        nullnet_buf = (uint8_t *)&tx_beacon_buf;
        nullnet_len = sizeof(net_packet_t);
        NETSTACK_NETWORK.output(NULL); 
        etimer_set(&beacon_timer, BEACON_INTERVAL);
    }
    PROCESS_END();
}

PROCESS_THREAD(watchdog_process, ev, data) {
    static struct etimer w;
    PROCESS_BEGIN();
    etimer_set(&w, CLOCK_SECOND * 2);
    while(1) {
        PROCESS_WAIT_EVENT_UNTIL(etimer_expired(&w));
        uint32_t current_time = clock_seconds();
        
        for (int i = 0; i < MAX_NODES; i++) {
            if (i + 1 == my_node_id) continue;
            if (routing_table[i].cost != INFINITY_COST) {
                if (current_time - routing_table[i].last_seen > (WATCHDOG_TIMEOUT / CLOCK_SECOND)) {
                    routing_table[i].cost = INFINITY_COST;
                }
            }
        }
        etimer_reset(&w);
    }
    PROCESS_END();
}