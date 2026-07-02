#include "contiki.h"
#include "net/nullnet/nullnet.h"
#include "net/netstack.h"
#include "net/packetbuf.h" 
#include "dev/leds.h"
#include "dev/serial-line.h"
#include "dev/button-hal.h"       
#include "common/saadc-sensor.h" 
#include "random.h"
#include <stdio.h>
#include <string.h>
#include "routing.h"

#define BEACON_INTERVAL (CLOCK_SECOND * 3)
#define WATCHDOG_TIMEOUT (CLOCK_SECOND * 10)
#define AUTO_CARGO_TIMEOUT (CLOCK_SECOND * 5) 

// --- JOYSTICK THRESHOLDS ---
#define JOYSTICK_POLL_RATE (CLOCK_SECOND / 10) 
#define JOYSTICK_HIGH 3000
#define JOYSTICK_LOW 1000

static uint8_t my_node_id;
static route_entry_t routing_table[MAX_NODES];
static uint8_t active_target_map[4] = {2, 3, 4, 5};

// --- STATE VARIABLES ---
static uint8_t proxy_enabled = 1; 

// --- NEW PIPELINE VARIABLES ---
static net_packet_t holding_buffer;
static uint8_t is_holding_packet = 0;
static uint8_t is_waiting_for_report = 0;
static uint16_t pending_cargo_id = 0;

// NEW STATE TRACKING
static uint16_t pending_force = 0;
static char pending_true_label = '-';
static char pending_selected_label = '-';

static net_packet_t tx_beacon_buf;

// --- QUEUE VARIABLES ---
#define MAX_QUEUE_SIZE 20
static net_packet_t packet_queue[MAX_QUEUE_SIZE];
static uint8_t queue_head = 0;
static uint8_t queue_tail = 0;
static uint8_t queue_count = 0;

PROCESS(origin_routing_process, "Operator Process");
PROCESS(watchdog_process, "Routing Watchdog Process");
AUTOSTART_PROCESSES(&origin_routing_process, &watchdog_process);

static void set_linkaddr_from_id(linkaddr_t *addr, uint8_t id) {
    memset(addr, 0, LINKADDR_SIZE);
    addr->u8[1] = id;
}

uint8_t calculate_rssi_penalty(int16_t rssi) {
    if (rssi >= -55) return 1;    // Strong, close connection
    if (rssi >= -70) return 4;    // Medium connection
    if (rssi >= -85) return 15;   // Weak, distant connection
    return 50;                    // Dead end / unreliable
}

static void print_queue_snapshot(const char* message) {
    char buffer[128]; // Buffer to hold the entire string in memory
    int offset = 0;
    uint8_t has_active = (is_holding_packet || is_waiting_for_report);

    // 1. Build the Header
    offset += snprintf(buffer + offset, sizeof(buffer) - offset, "[SNAPSHOT_QUEUE] Msg:%s | Act:", message);

    // 2. Build the Active part explicitly
    if (has_active) {
        offset += snprintf(buffer + offset, sizeof(buffer) - offset, "%d,%c",
            holding_buffer.payload.cargo.packet_id,
            holding_buffer.payload.cargo.true_label);
    } else {
        offset += snprintf(buffer + offset, sizeof(buffer) - offset, "NONE");
    }

    // 3. Start the Queue part
    offset += snprintf(buffer + offset, sizeof(buffer) - offset, " | Q:");

    // 4. Build the Queue list explicitly (separated by semicolons)
    for (int i = 0; i < queue_count; i++) {
        int idx = (queue_head + i) % MAX_QUEUE_SIZE;
        offset += snprintf(buffer + offset, sizeof(buffer) - offset, "%d,%c;",
            packet_queue[idx].payload.cargo.packet_id,
            packet_queue[idx].payload.cargo.true_label);
    }

    // 5. Print it ONCE atomically. No interruptions allowed!
    printf("%s\n", buffer);
}


// --- QUEUE MANAGER ---
static void check_and_load_queue() {
    if (!is_holding_packet && !is_waiting_for_report && queue_count > 0) {
        // Dequeue the next packet
        memcpy(&holding_buffer, &packet_queue[queue_head], sizeof(net_packet_t));
        queue_head = (queue_head + 1) % MAX_QUEUE_SIZE;
        queue_count--;
        is_holding_packet = 1;

        print_queue_snapshot("Removed packet for processing");
        
        // Exact original prints for GUI compatibility
        printf("\n=========================================\n");
        printf("[FACTORY INBOUND] Cargo #%d Arrived!\n", holding_buffer.payload.cargo.packet_id);
        printf("Sensor Force: %u | True Label: %c\n", holding_buffer.payload.cargo.force_value, holding_buffer.payload.cargo.true_label);
        printf(">>> WAITING FOR JOYSTICK INPUT <<<\n");
        printf("=========================================\n");
        
        process_poll(&origin_routing_process); 
    }
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
    else if (rx->type == PACKET_TYPE_CARGO && rx->payload.cargo.final_dest_id == my_node_id) {
        if (is_holding_packet || is_waiting_for_report) {
            // Operator is busy, handle queueing
            if (queue_count < MAX_QUEUE_SIZE) {
                memcpy(&packet_queue[queue_tail], rx, sizeof(net_packet_t));
                queue_tail = (queue_tail + 1) % MAX_QUEUE_SIZE;
                queue_count++;
                print_queue_snapshot("New packet added to queue");
            } else {
                printf("[BUSY] Ignored Factory packet. Already processing Cargo #%d\n", holding_buffer.payload.cargo.packet_id);
            }
            return;
        }
        
        // Operator is idle, process immediately
        memcpy(&holding_buffer, rx, sizeof(net_packet_t));
        is_holding_packet = 1;
        print_queue_snapshot("New packet processing"); // Trigger snapshot for immediate load
        
        printf("\n=========================================\n");
        printf("[FACTORY INBOUND] Cargo #%d Arrived!\n", holding_buffer.payload.cargo.packet_id);
        printf("Sensor Force: %u | True Label: %c\n", holding_buffer.payload.cargo.force_value, holding_buffer.payload.cargo.true_label);
        printf(">>> WAITING FOR JOYSTICK INPUT <<<\n");
        printf("=========================================\n");
        
        process_poll(&origin_routing_process); 
    }
    else if (rx->type == PACKET_TYPE_REPORT) {
        uint16_t c_id = rx->payload.report.cargo_id;
        uint8_t reporting_node = rx->src_id;
        uint8_t action = rx->payload.report.action;
        
        uint8_t delivery_failed = 0; 

        if (action == ACTION_DELIVERED) {
            uint8_t success = (pending_true_label == pending_selected_label) ? 1 : 0;
            
            printf("\n[ATOMIC_RESULT] ID:%d | Force:%u | True:%c | Sel:%c | Route:", 
                   c_id, pending_force, pending_true_label, pending_selected_label);
            
            printf("%d", rx->payload.report.route_history[0]);
            for(int i = 1; i < rx->payload.report.hop_count; i++) {
                printf("-%d", rx->payload.report.route_history[i]);
            }
            printf(" | Result:%s\n", success ? "PASS" : "FAIL");

            if (!success) {
                delivery_failed = 1; 
            }
        } else if (action == ACTION_DROPPED) {
            printf("\n[CHAIN] Cargo #%d: Dropped at Node %d (Dead End)\n", c_id, reporting_node);
            delivery_failed = 1; 
        }

        if (is_waiting_for_report && c_id == pending_cargo_id) {
            
            if (delivery_failed) {
                if (queue_count < MAX_QUEUE_SIZE) {
                    holding_buffer.payload.cargo.hop_count = 0;
                    holding_buffer.payload.cargo.route_history[holding_buffer.payload.cargo.hop_count++] = my_node_id;
                    
                    memcpy(&packet_queue[queue_tail], &holding_buffer, sizeof(net_packet_t));
                    queue_tail = (queue_tail + 1) % MAX_QUEUE_SIZE;
                    queue_count++;
                } else {
                    printf("\n[ARQ] Cargo #%d failed, but queue is FULL! Packet permanently dropped.\n", c_id);
                }
            }

            is_waiting_for_report = 0; 
            print_queue_snapshot("Report resolved"); // FIX: Force instant UI clear
            check_and_load_queue(); 
        }
    }
}

PROCESS_THREAD(origin_routing_process, ev, data) {
    static struct etimer beacon_timer;
    static struct etimer joystick_timer;
    static struct etimer timeout_timer; 
    PROCESS_BEGIN();

    int8_t tx_power = -25;
    NETSTACK_RADIO.set_value(RADIO_PARAM_TXPOWER, tx_power);

    my_node_id = 1; 
    linkaddr_t new_mac;
    memset(&new_mac, 0, LINKADDR_SIZE);
    new_mac.u8[1] = my_node_id;
    linkaddr_set_node_addr(&new_mac);

    for (int i = 0; i < MAX_NODES; i++) routing_table[i].cost = INFINITY_COST;
    routing_table[0].cost = 0;
    routing_table[0].next_hop = 1;

    nullnet_set_input_callback(input_callback);
    etimer_set(&beacon_timer, BEACON_INTERVAL);
    etimer_set(&joystick_timer, JOYSTICK_POLL_RATE); 

    printf("\n--- OPERATOR TERMINAL READY ---\n");
    printf("Type 0 + Enter to disable proxy / 1 to enable proxy.\n");
    printf("Press BUTTON 1 to view Routing Table & Customer Map.\n");
    printf("Waiting for packets from the Factory...\n");

    while(1) {
        PROCESS_WAIT_EVENT();
        
        if (ev == PROCESS_EVENT_TIMER && data == &timeout_timer) {
            if (is_waiting_for_report) {
                printf("\n[WARN] Timeout! Package lost in transit. No report received for Cargo #%d.\n", pending_cargo_id);
                printf("[OPERATOR] Unlocking terminal. Ready for next package.\n");
                is_waiting_for_report = 0; 
                print_queue_snapshot("Timeout resolved"); // FIX: Force instant UI clear
                check_and_load_queue(); 
            }
        }

        if (ev == button_hal_press_event) {
            uint32_t current_time = clock_seconds();
            printf("\n========== OPERATOR ROUTING TABLE ==========\n");
            for (int i = 0; i < MAX_NODES; i++) {
                uint8_t target = i + 1;
                if (target > 7) continue; 

                if (target == my_node_id) {
                    printf("Node %d (Self): Cost 0\n", target);
                } else if (routing_table[i].cost == INFINITY_COST) {
                    printf("Node %d: UNREACHABLE (Dead/Out of Range)\n", target);
                } else {
                    uint32_t time_since_seen = current_time - routing_table[i].last_seen;
                    printf("Node %d: Route via Node %d | Cost: %u | Last Seen: %lu sec ago\n", 
                           target, 
                           routing_table[i].next_hop, 
                           routing_table[i].cost, 
                           (unsigned long)time_since_seen);
                }
            }
            printf("============================================\n");
            
            printf("========== CURRENT CUSTOMER MAP ==========\n");
            for (int i = 0; i < 4; i++) {
                char role = 'A' + i;
                uint8_t current_assigned_node = active_target_map[i];
                uint8_t primary_original_node = i + 2; 
                
                if (current_assigned_node == primary_original_node) {
                    printf(" Customer %c -> Node %d (Primary)\n", role, current_assigned_node);
                } else {
                    printf(" Customer %c -> Node %d [PROXY ACTIVE]\n", role, current_assigned_node);
                }
            }
            printf("==========================================\n\n");
        }

        if (ev == PROCESS_EVENT_TIMER && data == &beacon_timer) {
            tx_beacon_buf.type = PACKET_TYPE_BEACON;
            tx_beacon_buf.src_id = my_node_id;
            for (int i = 0; i < MAX_NODES; i++) tx_beacon_buf.payload.beacon.costs[i] = routing_table[i].cost;
            nullnet_buf = (uint8_t *)&tx_beacon_buf;
            nullnet_len = sizeof(net_packet_t);
            NETSTACK_NETWORK.output(NULL); 
            etimer_set(&beacon_timer, BEACON_INTERVAL);
        }

        if (ev == PROCESS_EVENT_TIMER && data == &joystick_timer) {
            uint32_t next_poll_delay = JOYSTICK_POLL_RATE; 

            if (is_holding_packet && !is_waiting_for_report) {
                uint16_t joy_x = saadc_sensor.value(P0_30); // Pin 30
                uint16_t joy_y = saadc_sensor.value(P0_31); // Pin 31
                
                char selected = '-';
                
                if (joy_x > JOYSTICK_HIGH) selected = 'A';     
                else if (joy_y < JOYSTICK_LOW) selected = 'B';  
                else if (joy_y > JOYSTICK_HIGH) selected = 'C'; 
                else if (joy_x < JOYSTICK_LOW) selected = 'D';  
                
                if (selected != '-') {
                    printf("[OPERATOR] Joystick pushed. Selected Label: %c. Dispatching to network...\n", selected);

                    pending_force = holding_buffer.payload.cargo.force_value;
                    pending_true_label = holding_buffer.payload.cargo.true_label;
                    pending_selected_label = selected;
                    
                    uint8_t logical_idx = selected - 'A';
                    uint8_t final_target = active_target_map[logical_idx];
                    
                    if (routing_table[final_target - 1].cost != INFINITY_COST) {
                        holding_buffer.payload.cargo.final_dest_id = final_target;
                        holding_buffer.payload.cargo.selected_label = selected;
                        
                        holding_buffer.payload.cargo.route_history[holding_buffer.payload.cargo.hop_count++] = my_node_id;
                        
                        uint8_t n_hop = routing_table[final_target - 1].next_hop;
                        linkaddr_t next_mac;
                        set_linkaddr_from_id(&next_mac, n_hop);
                        nullnet_buf = (uint8_t *)&holding_buffer;
                        nullnet_len = sizeof(net_packet_t);
                        NETSTACK_NETWORK.output(&next_mac);
                        
                        pending_cargo_id = holding_buffer.payload.cargo.packet_id;
                        is_waiting_for_report = 1; 
                        
                        etimer_set(&timeout_timer, AUTO_CARGO_TIMEOUT);
                        
                        leds_toggle(LED_2);
                        
                        is_holding_packet = 0; 
                        print_queue_snapshot("Packet Dispatched"); // Tell GUI it is flying
                    } else {
                        printf("[FAIL] Target Node %d (Role %c) is completely dead! Packet Dropped.\n", final_target, selected);
                        leds_toggle(LED_3);
                        
                        is_waiting_for_report = 0;
                        is_holding_packet = 0; 
                        print_queue_snapshot("Target dead, dropped"); // Force instant UI clear
                        check_and_load_queue();
                    }
                    
                    next_poll_delay = CLOCK_SECOND * 2; 
                }
            }
            etimer_set(&joystick_timer, next_poll_delay);
        }

        if (ev == serial_line_event_message && data != NULL) {
            char *input = (char *)data;
            char cmd = input[0]; 
            
            if (cmd == '0') {
                proxy_enabled = 0;
                printf("[CONFIG] Proxy Mode OFF (Strict Fixed Roles Only)\n");
            } else if (cmd == '1') {
                proxy_enabled = 1;
                printf("[CONFIG] Proxy Mode ON (Relays handle dead nodes)\n");
            } 
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
        uint32_t current_time = clock_seconds();
        
        for (int i = 0; i < MAX_NODES; i++) {
            if (i + 1 == my_node_id) continue;
            if (routing_table[i].cost != INFINITY_COST) {
                if (current_time - routing_table[i].last_seen > (WATCHDOG_TIMEOUT / CLOCK_SECOND)) {
                    routing_table[i].cost = INFINITY_COST;
                }
            }
        }
        
        uint8_t relay_6_in_use = 0;
        uint8_t relay_7_in_use = 0;
        uint8_t factory_8_in_use = 0; 
        
        for (int i = 0; i < 4; i++) {
            uint8_t base = i + 2; 
            if (proxy_enabled) {
                if (routing_table[base - 1].cost == INFINITY_COST) {
                    if (routing_table[5].cost != INFINITY_COST && !relay_6_in_use) {
                        active_target_map[i] = 6;
                        relay_6_in_use = 1;
                    } else if (routing_table[6].cost != INFINITY_COST && !relay_7_in_use) {
                        active_target_map[i] = 7;
                        relay_7_in_use = 1;
                    } else if (routing_table[7].cost != INFINITY_COST && !factory_8_in_use) {
                        active_target_map[i] = 8;
                        factory_8_in_use = 1;
                    }
                } else {
                    active_target_map[i] = base; 
                }
            } else {
                active_target_map[i] = base; 
            }
        }
        
        static uint8_t auto_print_counter = 0;
        if (++auto_print_counter >= 3) { 
            printf("\n========== OPERATOR ROUTING TABLE ==========\n");
            for (int i = 0; i < MAX_NODES; i++) {
                uint8_t target = i + 1;
                if (target > 7) continue; 

                if (target == my_node_id) {
                    printf("Node %d (Self): Cost 0\n", target);
                } else if (routing_table[i].cost == INFINITY_COST) {
                    printf("Node %d: UNREACHABLE (Dead/Out of Range)\n", target);
                } else {
                    printf("Node %d: Route via Node %d | Cost: %u | Last Seen: Auto\n", target, routing_table[i].next_hop, routing_table[i].cost);
                }
            }
            printf("============================================\n");
            auto_print_counter = 0;
        } else {
            // ONLY print the snapshot when the routing table is NOT printing.
            print_queue_snapshot("Watchdog Heartbeat");
        }
        etimer_reset(&w);
    }
    PROCESS_END();
}