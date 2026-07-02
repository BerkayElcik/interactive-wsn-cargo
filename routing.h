#ifndef ROUTING_H
#define ROUTING_H

#include "contiki.h"
#include "net/linkaddr.h"

// Network Size & Routing Limits
#define MAX_NODES 10
#define INFINITY_COST 255
#define MAX_HOPS 10 // Maximum depth of the route history

// Core Packet Types
#define PACKET_TYPE_BEACON 0
#define PACKET_TYPE_CARGO  1
#define PACKET_TYPE_REPORT 2

// Telemetry Actions for GUI Mapping
#define ACTION_FORWARDED 0 // (Disabled in logic to prevent collisions, kept for legacy)
#define ACTION_DELIVERED 1
#define ACTION_DROPPED   2

// ---------------------------------------------------------
// Routing Table Structure (Bellman-Ford Distance Vector)
// ---------------------------------------------------------
typedef struct {
    uint8_t cost;
    uint8_t next_hop;
    uint32_t last_seen;
} route_entry_t;

// ---------------------------------------------------------
// Payload Definitions
// ---------------------------------------------------------

// 1. BEACON Payload (HELLO Messages)
typedef struct {
    uint8_t costs[MAX_NODES]; // Advertises distance to all known nodes
} beacon_payload_t;

// 2. CARGO Payload (The physical box simulation)
typedef struct {
    uint16_t packet_id;      // Unique ID for tracking
    uint8_t final_dest_id;   // Internal routing target
    
    // --- NEW FACTORY/GAME VARIABLES ---
    uint16_t force_value;    // Raw ADC value from Factory force sensor
    char true_label;         // Ground truth assigned by Factory ('A', 'B', 'C', 'D')
    char selected_label;     // Assigned by Operator Joystick ('A', 'B', 'C', 'D' or '-')
    
    // FULL ROUTE CHAIN TRACKING
    uint8_t route_history[MAX_HOPS]; 
    uint8_t hop_count; 
} cargo_payload_t;

// 3. REPORT Payload (Reverse Telemetry for PC GUI)
typedef struct {
    uint16_t cargo_id;       // Which box was handled
    uint8_t action;          // Delivered, or Dropped
    uint8_t next_hop;        // Who it was passed to (if used)
    
    // --- NEW GAME VARIABLE ---
    uint8_t match_success;   // 1 = True matches Selected, 0 = Fail
    
    // Telemetry needs to carry the history back to Operator
    uint8_t route_history[MAX_HOPS];
    uint8_t hop_count;
} report_payload_t;

// ---------------------------------------------------------
// Main Network Wrapper (Transmitted over NullNet)
// ---------------------------------------------------------
typedef struct {
    uint8_t type;            // PACKET_TYPE_BEACON, CARGO, or REPORT
    uint8_t src_id;          // The physical board transmitting this frame
    union {
        beacon_payload_t beacon;
        cargo_payload_t cargo;
        report_payload_t report;
    } payload;
} net_packet_t;

#endif /* ROUTING_H */