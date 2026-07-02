#include "contiki.h"
#include "net/nullnet/nullnet.h"
#include "net/netstack.h"
#include "net/packetbuf.h" 
#include "dev/leds.h"
#include "dev/button-hal.h"      
#include "common/saadc-sensor.h" 
#include <stdio.h>
#include <string.h>
#include "routing.h"

#define BEACON_INTERVAL (CLOCK_SECOND * 3)
#define WATCHDOG_TIMEOUT (CLOCK_SECOND * 10)
#define SENSOR_POLL_RATE (CLOCK_SECOND / 4) 

#define FORCE_MAX_LIMIT 4000
#define FORCE_LEVEL_D 3000      
#define FORCE_LEVEL_C 2000      
#define FORCE_LEVEL_B 1000      

static uint8_t my_node_id = 7; 
static route_entry_t routing_table[MAX_NODES];
static net_packet_t tx_beacon_buf;
static net_packet_t tx_cargo_buf;
static uint16_t cargo_id_counter = 5000; 

PROCESS(factory_process, "Factory Process");
PROCESS(watchdog_process, "Watchdog Process");
AUTOSTART_PROCESSES(&factory_process, &watchdog_process);

static void set_linkaddr_from_id(linkaddr_t *addr, uint8_t id) {
    memset(addr, 0, LINKADDR_SIZE);
    addr->u8[1] = id;
}

uint8_t calculate_rssi_penalty(int16_t rssi) {
    if (rssi >= -55) return 1;    
    if (rssi >= -70) return 4;    
    if (rssi >= -85) return 15;   
    return 50;                    
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
            if (rx->payload.beacon.costs[i] != INFINITY_COST) {
                uint16_t r_cost = rx->payload.beacon.costs[i] + link_penalty;
                uint8_t p_cost = (r_cost > 254) ? INFINITY_COST : (uint8_t)r_cost;
                if (p_cost < routing_table[i].cost || routing_table[i].next_hop == neighbor_id) {
                    routing_table[i].cost = p_cost;
                    routing_table[i].next_hop = neighbor_id;
                    routing_table[i].last_seen = clock_seconds(); 
                }
            }
        }
    }
    else if (rx->type == PACKET_TYPE_REPORT) {
        uint8_t return_hop = routing_table[0].next_hop; 
        if(return_hop != INFINITY_COST) {
            linkaddr_t ret_mac;
            set_linkaddr_from_id(&ret_mac, return_hop);
            memcpy(&tx_cargo_buf, rx, sizeof(net_packet_t));
            nullnet_buf = (uint8_t *)&tx_cargo_buf;
            nullnet_len = sizeof(net_packet_t);
            NETSTACK_NETWORK.output(&ret_mac);
        }
    }
    else if (rx->type == PACKET_TYPE_CARGO) {
        uint8_t target_dest = rx->payload.cargo.final_dest_id;
        
        if (target_dest == my_node_id) {
            if(rx->payload.cargo.hop_count < MAX_HOPS) {
                rx->payload.cargo.route_history[rx->payload.cargo.hop_count++] = my_node_id;
            }
            
            char true_lbl = rx->payload.cargo.true_label;
            char sel_lbl = rx->payload.cargo.selected_label;
            uint8_t did_match = (true_lbl == sel_lbl) ? 1 : 0;
            
            printf("Delivered! Match: %d\n", did_match);
            leds_toggle(LED_2); 
            
            tx_cargo_buf.type = PACKET_TYPE_REPORT;
            tx_cargo_buf.src_id = my_node_id;
            tx_cargo_buf.payload.report.cargo_id = rx->payload.cargo.packet_id;
            tx_cargo_buf.payload.report.action = 1; 
            tx_cargo_buf.payload.report.hop_count = rx->payload.cargo.hop_count;
            tx_cargo_buf.payload.report.match_success = did_match; 
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
                tx_cargo_buf.type = PACKET_TYPE_REPORT;
                tx_cargo_buf.src_id = my_node_id;
                tx_cargo_buf.payload.report.cargo_id = rx->payload.cargo.packet_id;
                tx_cargo_buf.payload.report.action = 2; 
                
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

PROCESS_THREAD(factory_process, ev, data) {
    static struct etimer beacon_timer;
    static struct etimer sensor_timer;
    static struct etimer pause_timer;          
    static uint8_t is_paused = 0; 
    
    PROCESS_BEGIN();
    
    static int8_t tx_power = -25; 
    NETSTACK_RADIO.set_value(RADIO_PARAM_TXPOWER, tx_power); 
    
    linkaddr_t new_mac;
    memset(&new_mac, 0, LINKADDR_SIZE);
    new_mac.u8[1] = my_node_id;
    linkaddr_set_node_addr(&new_mac);

    for (int i = 0; i < MAX_NODES; i++) routing_table[i].cost = INFINITY_COST;
    routing_table[my_node_id - 1].cost = 0;
    routing_table[my_node_id - 1].next_hop = my_node_id;

    nullnet_set_input_callback(input_callback);
    etimer_set(&beacon_timer, BEACON_INTERVAL);
    etimer_set(&sensor_timer, SENSOR_POLL_RATE);

    printf("Booted.\n");

    while(1) {
        PROCESS_WAIT_EVENT();

        // 1. CONTINUOUS SENSOR PRINTING 
        if (ev == PROCESS_EVENT_TIMER && data == &sensor_timer) {
            if (!is_paused) {
                uint16_t force_val = saadc_sensor.value(P0_31); 
                if (force_val < 5000) {
                    printf("%u\n", force_val); 
                }
            }
            etimer_set(&sensor_timer, SENSOR_POLL_RATE);
        }

        // 2. UNPAUSE 
        if (ev == PROCESS_EVENT_TIMER && data == &pause_timer) {
            is_paused = 0;
        }

        // 3. MINIMAL BUTTON PRESS LOGIC
        if (ev == button_hal_press_event) {
            // --- RESTORED LOCK ---
            if (!is_paused) {
                uint16_t locked_val = saadc_sensor.value(P0_31); 
                
                if (locked_val > 5000) {
                    printf("Loose\n"); 
                }
                else if (locked_val > FORCE_MAX_LIMIT) {
                    printf("Reject\n"); 
                    leds_toggle(LED_3); 
                } else {
                    char true_lbl = 'A';
                    if (locked_val >= FORCE_LEVEL_D) true_lbl = 'D';
                    else if (locked_val >= FORCE_LEVEL_C) true_lbl = 'C';
                    else if (locked_val >= FORCE_LEVEL_B) true_lbl = 'B';
                    
                    if (routing_table[0].cost != INFINITY_COST) {
                        printf("%c\n", true_lbl);
                        
                        tx_cargo_buf.type = PACKET_TYPE_CARGO;
                        tx_cargo_buf.src_id = my_node_id;
                        tx_cargo_buf.payload.cargo.packet_id = cargo_id_counter++;
                        tx_cargo_buf.payload.cargo.final_dest_id = 1; 
                        tx_cargo_buf.payload.cargo.force_value = locked_val;
                        tx_cargo_buf.payload.cargo.true_label = true_lbl;
                        tx_cargo_buf.payload.cargo.selected_label = '-'; 
                        tx_cargo_buf.payload.cargo.hop_count = 0;
                        tx_cargo_buf.payload.cargo.route_history[tx_cargo_buf.payload.cargo.hop_count++] = my_node_id; 
                        
                        linkaddr_t next;
                        set_linkaddr_from_id(&next, routing_table[0].next_hop);
                        nullnet_buf = (uint8_t *)&tx_cargo_buf;
                        nullnet_len = sizeof(net_packet_t);
                        NETSTACK_NETWORK.output(&next);
                        leds_toggle(LED_2); 
                    } else {
                        printf("Dead\n"); 
                    }
                }
                
                // Suspend numbers for 1 second so you can read the word
                is_paused = 1;
                etimer_set(&pause_timer, CLOCK_SECOND); 
            }
        }

        // 4. ROUTING BEACONS
        if (ev == PROCESS_EVENT_TIMER && data == &beacon_timer) {
            tx_beacon_buf.type = PACKET_TYPE_BEACON;
            tx_beacon_buf.src_id = my_node_id;
            for (int i = 0; i < MAX_NODES; i++) tx_beacon_buf.payload.beacon.costs[i] = routing_table[i].cost;
            nullnet_buf = (uint8_t *)&tx_beacon_buf;
            nullnet_len = sizeof(net_packet_t);
            NETSTACK_NETWORK.output(NULL); 
            etimer_set(&beacon_timer, BEACON_INTERVAL);
        }
    }
    PROCESS_END();
}

PROCESS_THREAD(watchdog_process, ev, data) {
    static struct etimer w;
    PROCESS_BEGIN();
    etimer_set(&w, CLOCK_SECOND * 2);
    while(1) {
        PROCESS_WAIT_EVENT_UNTIL(etimer_expired(&w));
        for (int i = 0; i < MAX_NODES; i++) {
            if (i + 1 == my_node_id) continue;
            if (routing_table[i].cost != INFINITY_COST) {
                if (clock_seconds() - routing_table[i].last_seen > (WATCHDOG_TIMEOUT / CLOCK_SECOND)) {
                    routing_table[i].cost = INFINITY_COST;
                }
            }
        }
        etimer_reset(&w);
    }
    PROCESS_END();
}