import sys
import re
import serial
import math
from PyQt5 import QtWidgets, QtCore, QtGui
from PyQt5.QtCore import Qt, QThread, pyqtSignal

# --- CONFIGURATION ---
#SERIAL_PORT = "/dev/tty.usbmodem0010502017941"  # DON'T FORGET TO CHANGE THIS TO YOUR OWN PORT
SERIAL_PORT = "/dev/ttyACM0"
BAUD_RATE = 115200


class SerialWorker(QThread):
    packet_completed = pyqtSignal(dict)
    routing_updated = pyqtSignal(dict)
    event_update = pyqtSignal(dict)
    queue_updated = pyqtSignal(list)
    # NEW SIGNAL: safely sends active packet data to the Main Thread
    active_packet_updated = pyqtSignal(str, str) 

    def __init__(self, port, baud_rate):
        super().__init__()
        self.port = port
        self.baud_rate = baud_rate
        self.running = True

    def run(self):
        try:
            ser = serial.Serial(self.port, self.baud_rate, timeout=1)
            print(f"✅ SUCCESS: Connected to port {self.port}...")
            self.event_update.emit({'type': 'SYS', 'msg': "🟢 System connected, waiting for data..."})
        except Exception as e:
            print(f"❌ CRITICAL ERROR: {e}")
            return

        in_routing_table = False
        current_routing = {}

        # --- TRACKING VARIABLES ---
        active_packets = {}
        current_inbound_id = None
        last_report_id = None
        packet_queue = []

        while self.running:
            try:
                raw_line = ser.readline()
                if not raw_line: continue
                line = raw_line.decode('utf-8', errors='ignore').strip()
                if not line: continue

                # --- 1. ROUTING TABLE PARSING ---
                if "========== OPERATOR ROUTING TABLE ==========" in line:
                    in_routing_table = True
                    current_routing = {}
                    continue
                elif "============================================" in line and in_routing_table:
                    in_routing_table = False
                    self.routing_updated.emit(current_routing)
                    continue

                if in_routing_table:
                    try:
                        if "(Self)" in line:
                            match = re.search(r"Node\s+(\d+)", line)
                            if match: current_routing[int(match.group(1))] = {'reachable': True,
                                                                              'next_hop': int(match.group(1))}
                        elif "UNREACHABLE" in line:
                            match = re.search(r"Node\s+(\d+)", line)
                            if match: current_routing[int(match.group(1))] = {'reachable': False}
                        elif "Route via Node" in line:
                            match = re.search(r"Node\s+(\d+):\s*Route via Node\s+(\d+)", line)
                            if match: current_routing[int(match.group(1))] = {'reachable': True,
                                                                              'next_hop': int(match.group(2))}
                    except Exception:
                        pass
                    continue

                # --- 2. PACKET PARSING & EVENT NOTIFICATIONS ---
                if "Cargo #" in line and "Arrived!" in line:
                    match = re.search(r"Cargo #(\d+)", line)
                    if match:
                        cid = match.group(1)
                        current_inbound_id = cid
                        active_packets[cid] = {'id': cid}
                        self.event_update.emit({'type': 'INFO', 'msg': f"📦 Cargo #{cid} arrived from Factory."})

                elif "Sensor Force:" in line:
                    match = re.search(r"Sensor Force: (\d+) \| True Label: ([A-D])", line)
                    if match and current_inbound_id in active_packets:
                        active_packets[current_inbound_id]['force'] = match.group(1)
                        active_packets[current_inbound_id]['true_label'] = match.group(2)

                        self.event_update.emit({
                            'type': 'WAITING',
                            'id': current_inbound_id,
                            'label': active_packets[current_inbound_id]['true_label'],
                            'msg': f"⏳ Sensor Force: {match.group(1)}. Target: {match.group(2)}. Waiting for Joystick..."
                        })

                elif "Selected Label:" in line:
                    match = re.search(r"Selected Label: ([A-D])", line)
                    if match and current_inbound_id in active_packets:
                        active_packets[current_inbound_id]['selected_label'] = match.group(1)
                        self.event_update.emit({'type': 'JOYSTICK_PUSHED',
                                                'msg': f"🕹️ Joystick selected target '{match.group(1)}'. Dispatching..."})


                # --- ATOMIC RESULT PARSING ---
                elif "[ATOMIC_RESULT]" in line:
                    # Expects: [ATOMIC_RESULT] ID:42 | Force:120 | True:A | Sel:B | Route:1-6-2 | Result:FAIL
                    match = re.search(
                        r"ID:(\d+) \| Force:(\d+) \| True:([A-D]) \| Sel:([A-D]) \| Route:([\d\-]+) \| Result:(PASS|FAIL)",
                        line)

                    if match:
                        pkt = {
                            'id': match.group(1),
                            'force': match.group(2),
                            'true_label': match.group(3),
                            'selected_label': match.group(4),
                            'route': [int(n) for n in match.group(5).split('-')],
                            'success': match.group(6) == "PASS"
                        }

                        # Emit to UI
                        self.event_update.emit({'type': 'INFO', 'msg': f"✅ Packet Delivered. Route: {pkt['route']}"})
                        self.packet_completed.emit(pkt)

                        # Clean up the live tracking dictionary
                        if pkt['id'] in active_packets:
                            del active_packets[pkt['id']]
                        if current_inbound_id == pkt['id']:
                            current_inbound_id = None

                # --- 3. ARQ PARSING ---
                elif "[ARQ]" in line:
                    match_id = re.search(r"Cargo #(\d+)", line)
                    if match_id:
                        cid = match_id.group(1)
                        if "Re-inserted" in line or "Triggering" in line:
                            self.event_update.emit({'type': 'INFO',
                                                    'msg': f"🔄 ARQ: Cargo #{cid} delivery failed. Re-inserted into queue."})
                        elif "permanently dropped" in line:
                            self.event_update.emit({'type': 'TIMEOUT',
                                                    'msg': f"❌ ARQ: Cargo #{cid} failed. Queue FULL! Permanently dropped."})

                elif "[WARN] Timeout!" in line:
                    match = re.search(r"Cargo #(\d+)", line)
                    if match:
                        lost_id = match.group(1)

                        if lost_id in active_packets:
                            del active_packets[lost_id]
                        if current_inbound_id == lost_id:
                            current_inbound_id = None

                        self.event_update.emit({
                            'type': 'TIMEOUT',
                            'msg': f"⚠️ TIMEOUT: Cargo #{lost_id} lost in network. Terminal unlocked."
                        })
                
                elif "[FAIL]" in line and "completely dead" in line:
                    self.event_update.emit({
                        'type': 'TIMEOUT', 
                        'msg': f"❌ ERROR: {line}"
                    })

                # --- 4. QUEUE PARSING ---
                elif "[SNAPSHOT_QUEUE]" in line:
                    try:
                        # Split the line in case multiple serial messages got glued together
                        snapshots = line.split("[SNAPSHOT_QUEUE]")
                        
                        # Search backwards to find the most recent valid snapshot in this line
                        valid_snapshot = None
                        for snap in reversed(snapshots):
                            if " | Act:" in snap and " | Q:" in snap:
                                valid_snapshot = snap
                                break
                        
                        if not valid_snapshot:
                            continue # Silently ignore if we couldn't find a fully formed snapshot

                        # Safely extract the Active and Queue sections
                        act_part = valid_snapshot.split(" | Act:")[1].split(" | Q:")[0].strip()
                        q_part = valid_snapshot.split(" | Q:")[1].strip()

                        # 1. Update Active Table via SIGNAL
                        if act_part == "NONE":
                            self.active_packet_updated.emit("", "")
                        else:
                            if "," in act_part:
                                act_id, act_lbl = act_part.split(",")
                                self.active_packet_updated.emit(act_id.strip(), act_lbl.strip())

                        # 2. Update Queue Table via SIGNAL
                        queue_list = []
                        if q_part: # If there is text in the queue section
                            q_items = q_part.split(";")
                            for item in q_items:
                                if "," in item: # Ensure it's a valid ID,Label pair
                                    q_id, q_lbl = item.split(",")
                                    queue_list.append((q_id.strip(), q_lbl.strip()))

                        self.queue_updated.emit(queue_list)

                    except Exception as e:
                        print(f"⚠️ Queue parsing failed on line: {line}\nError: {e}")
                


            except Exception as e:
                print(f"🔥 RUNTIME ERROR (Parsing crashed): {e}")

    def stop(self):
        self.running = False
        self.wait()


class WsnGui(QtWidgets.QMainWindow):
    def __init__(self):
        super().__init__()
        self.setWindowTitle("Live Mesh Network Sorter")
        self.resize(1200, 800)

        self.network_state = {}
        self.active_route = []
        self.wait_icon_items = []
        self.hud_items = []

        self.setup_ui()

        self.serial_thread = SerialWorker(SERIAL_PORT, BAUD_RATE)
        self.serial_thread.packet_completed.connect(self.update_gui_from_packet)
        self.serial_thread.routing_updated.connect(self.update_live_topology)
        self.serial_thread.event_update.connect(self.handle_system_event)
        self.serial_thread.queue_updated.connect(self.update_queue_table)
        
        # Connect the NEW Active Packet Signal
        self.serial_thread.active_packet_updated.connect(self.handle_active_packet_update)
        
        self.serial_thread.start()

    def setup_ui(self):
        self.setStyleSheet("""
            QMainWindow { background-color: #1e1e1e; }
            QTableWidget { background-color: #252526; color: #d4d4d4; gridline-color: #3e3e42; font-size: 14px; }
            QHeaderView::section { background-color: #333333; color: white; font-weight: bold; padding: 4px; }
            QListWidget { background-color: #252526; color: #00aaff; font-size: 14px; border: 1px solid #333; padding: 5px; }
        """)

        central_widget = QtWidgets.QWidget()
        self.setCentralWidget(central_widget)

        main_layout = QtWidgets.QGridLayout(central_widget)
        main_layout.setColumnStretch(0, 1)
        main_layout.setColumnStretch(1, 1)
        main_layout.setRowStretch(0, 1)
        main_layout.setRowStretch(1, 1)

        # 1. TOP LEFT: Operator Target Map
        self.sort_scene = QtWidgets.QGraphicsScene()
        self.sort_scene.setSceneRect(-300, -250, 600, 500)
        self.sort_scene.setBackgroundBrush(QtGui.QBrush(QtGui.QColor("#1e1e1e")))
        self.sort_view = QtWidgets.QGraphicsView(self.sort_scene)
        self.sort_view.setRenderHint(QtGui.QPainter.Antialiasing)
        main_layout.addWidget(self.sort_view, 0, 0)
        self.init_sort_graphics()

        # 2. TOP RIGHT: Past Packet Table
        self.log_table = QtWidgets.QTableWidget(0, 5)
        self.log_table.setHorizontalHeaderLabels(["ID", "Force", "True", "Selected", "Status"])
        self.log_table.horizontalHeader().setSectionResizeMode(QtWidgets.QHeaderView.Stretch)
        main_layout.addWidget(self.log_table, 0, 1)

        # 3. BOTTOM LEFT: 2D Live Topology Map
        self.topo_scene = QtWidgets.QGraphicsScene()
        self.topo_scene.setSceneRect(-400, -250, 800, 500)
        self.topo_scene.setBackgroundBrush(QtGui.QBrush(QtGui.QColor("#1e1e1e")))
        self.topo_view = QtWidgets.QGraphicsView(self.topo_scene)
        self.topo_view.setRenderHint(QtGui.QPainter.Antialiasing)
        main_layout.addWidget(self.topo_view, 1, 0)

        # 4. BOTTOM RIGHT: System Messages + Active Packet + Queue
        br_widget = QtWidgets.QWidget()
        br_layout = QtWidgets.QVBoxLayout(br_widget)
        br_layout.setContentsMargins(0, 0, 0, 0)

        self.sys_msg_list = QtWidgets.QListWidget()
        br_layout.addWidget(self.sys_msg_list, stretch=1)

        # --- NEW: ACTIVE PACKET TABLE ---
        self.active_table = QtWidgets.QTableWidget(1, 2)
        self.active_table.setHorizontalHeaderLabels(["Active ID", "True Label"])
        self.active_table.horizontalHeader().setSectionResizeMode(QtWidgets.QHeaderView.Stretch)
        self.active_table.verticalHeader().setVisible(False)
        self.active_table.setFixedHeight(60)
        self.active_table.setEditTriggers(QtWidgets.QAbstractItemView.NoEditTriggers)
        self.active_table.setSelectionMode(QtWidgets.QAbstractItemView.NoSelection)
        self.active_table.setStyleSheet("QTableWidget { background-color: #252526; border: 2px solid #00aaff; }")
        br_layout.addWidget(self.active_table)
        # --------------------------------

        self.queue_table = QtWidgets.QTableWidget(5, 2)
        self.queue_table.setHorizontalHeaderLabels(["Queue ID", "True Label"])
        self.queue_table.horizontalHeader().setSectionResizeMode(QtWidgets.QHeaderView.Stretch)
        self.queue_table.verticalHeader().setVisible(False)
        self.queue_table.setFixedHeight(170)
        self.queue_table.setEditTriggers(QtWidgets.QAbstractItemView.NoEditTriggers)
        self.queue_table.setSelectionMode(QtWidgets.QAbstractItemView.NoSelection)
        br_layout.addWidget(self.queue_table)

        main_layout.addWidget(br_widget, 1, 1)
        
    # --- NEW THREAD-SAFE UI HANDLER ---
    def handle_active_packet_update(self, p_id, p_label):
        if p_id == "" and p_label == "":
            self.clear_active_packet_table()
        else:
            self.set_active_packet_table(p_id, p_label)

    def set_active_packet_table(self, p_id, p_label):
        id_item = QtWidgets.QTableWidgetItem(str(p_id))
        lbl_item = QtWidgets.QTableWidgetItem(str(p_label))
        id_item.setTextAlignment(Qt.AlignCenter)
        lbl_item.setTextAlignment(Qt.AlignCenter)

        id_item.setForeground(QtGui.QBrush(QtGui.QColor("#00aaff")))
        lbl_item.setForeground(QtGui.QBrush(QtGui.QColor("#00aaff")))

        self.active_table.setItem(0, 0, id_item)
        self.active_table.setItem(0, 1, lbl_item)

    def clear_active_packet_table(self):
        self.active_table.setItem(0, 0, QtWidgets.QTableWidgetItem(""))
        self.active_table.setItem(0, 1, QtWidgets.QTableWidgetItem(""))

    def update_queue_table(self, queue_list):
        for row in range(5):
            if row < len(queue_list):
                q_id, q_lbl = queue_list[row]
                id_item = QtWidgets.QTableWidgetItem(str(q_id))
                lbl_item = QtWidgets.QTableWidgetItem(str(q_lbl))
                id_item.setTextAlignment(Qt.AlignCenter)
                lbl_item.setTextAlignment(Qt.AlignCenter)
                id_item.setForeground(QtGui.QBrush(Qt.yellow))
                lbl_item.setForeground(QtGui.QBrush(Qt.yellow))
                self.queue_table.setItem(row, 0, id_item)
                self.queue_table.setItem(row, 1, lbl_item)
            else:
                self.queue_table.setItem(row, 0, QtWidgets.QTableWidgetItem(""))
                self.queue_table.setItem(row, 1, QtWidgets.QTableWidgetItem(""))

    def init_sort_graphics(self):
        self.target_coords = {
            'A': (0, -120),
            'B': (-120, 0),
            'C': (120, 0),
            'D': (0, 120)
        }

        self.sort_scene.addRect(-30, -30, 60, 60, QtGui.QPen(Qt.cyan, 2), QtGui.QBrush(QtGui.QColor("#2d2d30")))
        op_text = self.sort_scene.addText("OP")
        op_text.setDefaultTextColor(Qt.white)
        op_text.setPos(-12, -12)

        for label, (x, y) in self.target_coords.items():
            self.sort_scene.addRect(x - 25, y - 25, 50, 50, QtGui.QPen(Qt.white, 2),
                                    QtGui.QBrush(QtGui.QColor("#252526")))
            text = self.sort_scene.addText(label)
            text.setDefaultTextColor(Qt.white)
            font = text.font()
            font.setPointSize(16)
            font.setBold(True)
            text.setFont(font)
            text.setPos(x - 8, y - 15)

        self.arrow_items = []
        self.draw_customer_map_hud()

    def handle_system_event(self, event):
        if 'msg' in event:
            item = QtWidgets.QListWidgetItem(event['msg'])
            if event['type'] == 'TIMEOUT':
                item.setForeground(QtGui.QBrush(QtGui.QColor("#ff4444")))
            self.sys_msg_list.addItem(item)

            self.sys_msg_list.scrollToBottom()
            if self.sys_msg_list.count() > 10:
                self.sys_msg_list.takeItem(0)

        if event['type'] == 'WAITING':
            self.clear_wait_icon()

            # Populate the active 1-row table via internal GUI logic (since the packet details come from an event)
            self.set_active_packet_table(event['id'], event['label'])

            rect = self.sort_scene.addRect(35, -50, 50, 40, QtGui.QPen(Qt.yellow, 2),
                                           QtGui.QBrush(QtGui.QColor(74, 74, 0)))
            text = self.sort_scene.addText(f"#{event['id']}\nL: {event['label']}")
            text.setDefaultTextColor(Qt.yellow)
            font = text.font()
            font.setPointSize(10)
            text.setFont(font)
            text.setPos(40, -45)
            self.wait_icon_items.extend([rect, text])

        elif event['type'] == 'JOYSTICK_PUSHED':
            self.clear_wait_icon()
        elif event['type'] == 'TIMEOUT':
            self.clear_wait_icon()

    def clear_wait_icon(self):
        for item in self.wait_icon_items:
            self.sort_scene.removeItem(item)
        self.wait_icon_items = []

        self.clear_active_packet_table()

    def calculate_customer_map(self):
        mapping = {'A': 2, 'B': 3, 'C': 4, 'D': 5}
        if not self.network_state: return mapping

        r6 = False;
        r7 = False;
        r8 = False
        for target in ['A', 'B', 'C', 'D']:
            base = mapping[target]
            if not self.network_state.get(base, {}).get('reachable', True):
                if self.network_state.get(6, {}).get('reachable', False) and not r6:
                    mapping[target] = 6
                    r6 = True
                elif self.network_state.get(7, {}).get('reachable', False) and not r7:
                    mapping[target] = 7
                    r7 = True
                elif self.network_state.get(8, {}).get('reachable', False) and not r8:
                    mapping[target] = 8
                    r8 = True
        return mapping

    def draw_customer_map_hud(self):
        for item in self.hud_items: self.sort_scene.removeItem(item)
        self.hud_items = []

        mapping = self.calculate_customer_map()
        start_x, start_y = -295, -245

        bg = self.sort_scene.addRect(start_x, start_y, 200, 60, QtGui.QPen(Qt.white),
                                     QtGui.QBrush(QtGui.QColor(37, 37, 38, 200)))
        self.hud_items.append(bg)

        labels = ['A', 'B', 'C', 'D']
        for i, lbl in enumerate(labels):
            x = start_x + (i * 50)

            t1 = self.sort_scene.addText(lbl)
            t1.setDefaultTextColor(Qt.cyan)
            t1.setPos(x + 15, start_y + 5)

            t2 = self.sort_scene.addText(f"N{mapping[lbl]}")
            t2.setDefaultTextColor(Qt.white)
            t2.setPos(x + 10, start_y + 30)

            l1 = self.sort_scene.addLine(x, start_y, x, start_y + 60, QtGui.QPen(Qt.gray))
            self.hud_items.extend([t1, t2, l1])

        l_horiz = self.sort_scene.addLine(start_x, start_y + 30, start_x + 200, start_y + 30, QtGui.QPen(Qt.gray))
        self.hud_items.append(l_horiz)

    def update_live_topology(self, routing_data):
        self.network_state = routing_data
        self.draw_customer_map_hud()
        self.render_topology()

    def update_gui_from_packet(self, packet):
        self.update_sort_visuals(packet)
        self.update_table(packet)
        self.active_route = packet.get('route', [])
        self.render_topology()
        self.clear_wait_icon()

    def update_sort_visuals(self, packet):
        for item in self.arrow_items:
            self.sort_scene.removeItem(item)
        self.arrow_items = []

        selected = packet.get('selected_label')
        success = packet.get('success')

        if selected in self.target_coords:
            tx, ty = self.target_coords[selected]
            color = Qt.green if success else Qt.red

            pen = QtGui.QPen(color, 5, Qt.SolidLine, Qt.RoundCap, Qt.RoundJoin)
            end_x, end_y = tx * 0.6, ty * 0.6
            line = self.sort_scene.addLine(0, 0, end_x, end_y, pen)
            self.arrow_items.append(line)

            angle = math.atan2(end_y, end_x)
            arrow_size = 18
            p1 = QtCore.QPointF(end_x - arrow_size * math.cos(angle - math.pi / 6),
                                end_y - arrow_size * math.sin(angle - math.pi / 6))
            p2 = QtCore.QPointF(end_x - arrow_size * math.cos(angle + math.pi / 6),
                                end_y - arrow_size * math.sin(angle + math.pi / 6))
            p3 = QtCore.QPointF(end_x, end_y)

            polygon = QtGui.QPolygonF([p1, p2, p3])
            arrow_head = self.sort_scene.addPolygon(polygon, QtGui.QPen(color, 2), QtGui.QBrush(color))
            self.arrow_items.append(arrow_head)

    def update_table(self, packet):
        row_pos = 0
        self.log_table.insertRow(row_pos)

        items = [
            QtWidgets.QTableWidgetItem(str(packet.get('id', ''))),
            QtWidgets.QTableWidgetItem(str(packet.get('force', ''))),
            QtWidgets.QTableWidgetItem(str(packet.get('true_label', ''))),
            QtWidgets.QTableWidgetItem(str(packet.get('selected_label', ''))),
            QtWidgets.QTableWidgetItem("Pass" if packet.get('success') else "Fail")
        ]

        bg_color = QtGui.QColor("#1e4620") if packet.get('success') else QtGui.QColor("#5c1e1e")
        text_color = QtGui.QBrush(Qt.white)

        for col, item in enumerate(items):
            item.setBackground(bg_color)
            item.setForeground(text_color)
            item.setTextAlignment(Qt.AlignCenter)
            font = item.font()
            font.setBold(True)
            item.setFont(font)
            self.log_table.setItem(row_pos, col, item)

        if self.log_table.rowCount() > 10:
            self.log_table.removeRow(10)

    def render_topology(self):
        try:
            self.topo_scene.clear()

            # ---------------------------------------------------------
            # 1. TEXT HUD (Static - Always shows all tracked nodes)
            # ---------------------------------------------------------
            hud_x, hud_y = -395, -245
            self.topo_scene.addRect(hud_x, hud_y, 160, 240, QtGui.QPen(Qt.white),
                                    QtGui.QBrush(QtGui.QColor(37, 37, 38, 200)))
            title = self.topo_scene.addText("ROUTING TABLE")
            title.setDefaultTextColor(Qt.cyan)
            title.setPos(hud_x + 10, hud_y + 5)

            y_off = hud_y + 30
            
            # Nodes we ALWAYS want to see in the text list (2 through 7)
            display_nodes = [2, 3, 4, 5, 6, 7]
            
            for nid in display_nodes:
                data = self.network_state.get(nid, {'reachable': False})
                if data.get('reachable'):
                    txt = self.topo_scene.addText(f"N{nid} -> via N{data.get('next_hop', '?')}")
                    txt.setDefaultTextColor(Qt.white)
                else:
                    txt = self.topo_scene.addText(f"N{nid} -> DEAD")
                    txt.setDefaultTextColor(QtGui.QColor("#ff4444"))
                txt.setPos(hud_x + 10, y_off)
                y_off += 20

            # ---------------------------------------------------------
            # 2. 2D MAP (Dynamic - Only shows reachable nodes)
            # ---------------------------------------------------------
            active_nodes = []
            
            # Check all nodes 2 through 8 (include 8 if it's acting as a proxy)
            for nid in range(2, 9):
                is_reachable = self.network_state.get(nid, {}).get('reachable', False)
                is_in_route = nid in self.active_route
                
                # ONLY add to the map if it's alive OR if a packet is actively trying to use it
                if is_reachable or is_in_route:
                    if nid not in active_nodes:
                        active_nodes.append(nid)

            # Safety catch: add any other nodes that might be in the active route (except Node 1)
            for n in self.active_route:
                if n != 1 and n not in active_nodes:
                    active_nodes.append(n)
                    
            active_nodes.sort()

            node_positions = {1: (0, -120)} # Node 1 is fixed at the center bottom
            R = 220
            N = len(active_nodes)

            # Dynamically calculate equal spacing for ONLY the visible nodes
            if N == 1:
                node_positions[active_nodes[0]] = (0, -120 + R)
            elif N > 1:
                start_angle = math.pi * 0.85
                end_angle = math.pi * 0.15
                for i, n_id in enumerate(active_nodes):
                    # This math perfectly spaces them based on the new, dynamic N
                    angle = start_angle - (i * ((start_angle - end_angle) / (N - 1)))
                    x = R * math.cos(angle)
                    y = -120 + R * math.sin(angle)
                    node_positions[n_id] = (x, y)

            # ---------------------------------------------------------
            # 3. DRAW LINES (Route and Connections)
            # ---------------------------------------------------------
            if self.active_route and len(self.active_route) > 1:
                passed_edges = {}
                idx_op = self.active_route.index(1) if 1 in self.active_route else -1

                for i in range(len(self.active_route) - 1):
                    n_current = self.active_route[i]
                    n_next = self.active_route[i + 1]

                    if n_current in node_positions and n_next in node_positions:
                        x1, y1 = node_positions[n_current]
                        x2, y2 = node_positions[n_next]

                        if idx_op != -1 and i < idx_op:
                            route_color = QtGui.QColor("#ff4444")
                        else:
                            route_color = QtGui.QColor("#00aaff")

                        edge_tuple = tuple(sorted((n_current, n_next)))

                        if edge_tuple in passed_edges:
                            pen = QtGui.QPen(route_color, 4, Qt.DotLine)
                            offset = 12
                            self.topo_scene.addLine(x1 + offset, y1 + offset, x2 + offset, y2 + offset, pen)
                        else:
                            pen = QtGui.QPen(route_color, 4, Qt.DashLine)
                            self.topo_scene.addLine(x1, y1, x2, y2, pen)
                            passed_edges[edge_tuple] = True

            # ---------------------------------------------------------
            # 4. DRAW NODES (Circles)
            # ---------------------------------------------------------
            nodes_to_draw = active_nodes + [1]
            for n_id in nodes_to_draw:
                if n_id not in node_positions: continue
                x, y = node_positions[n_id]
                
                is_in_packet_route = n_id in self.active_route
                is_dead = False if n_id == 1 else not self.network_state.get(n_id, {}).get('reachable', False)

                # Color coding (Dead nodes will now ONLY appear if they are in the active_route, 
                # so the red color remains useful to show a packet died AT that specific node).
                if is_in_packet_route:
                    fill_color = QtGui.QColor("#007acc") # Bright Blue
                    border_color = Qt.white
                    border_width = 3
                    text_color = Qt.white
                elif is_dead:
                    fill_color = QtGui.QColor("#3a1515") # Dark Red
                    border_color = QtGui.QColor("#ff4444")
                    border_width = 2
                    text_color = QtGui.QColor("#ff4444")
                else:
                    fill_color = QtGui.QColor("#2d2d30") # Standard Grey
                    border_color = QtGui.QColor("#888888")
                    border_width = 2
                    text_color = QtGui.QColor("#aaaaaa")

                self.topo_scene.addEllipse(x - 25, y - 25, 50, 50, QtGui.QPen(border_color, border_width),
                                           QtGui.QBrush(fill_color))

                text = self.topo_scene.addText(f"N{n_id}")
                text.setDefaultTextColor(text_color)
                font = text.font()
                font.setPointSize(12)
                font.setBold(is_in_packet_route)
                text.setFont(font)
                text.setPos(x - 14, y - 12)

        except Exception as e:
            print(f"⚠️ UI Drawing Error Prevented: {e}")

    """
    def render_topology(self):
        try:
            self.topo_scene.clear()

            hud_x, hud_y = -395, -245
            self.topo_scene.addRect(hud_x, hud_y, 160, 240, QtGui.QPen(Qt.white),
                                    QtGui.QBrush(QtGui.QColor(37, 37, 38, 200)))
            title = self.topo_scene.addText("ROUTING TABLE")
            title.setDefaultTextColor(Qt.cyan)
            title.setPos(hud_x + 10, hud_y + 5)

            y_off = hud_y + 30
            
            # --- FIX 1: ALWAYS ANCHOR NODES 2 THROUGH 7 IN THE TEXT HUD ---
            display_nodes = [2, 3, 4, 5, 6, 7]
            
            for nid in display_nodes:
                # If the node is missing from serial data, safely default to DEAD
                data = self.network_state.get(nid, {'reachable': False})
                
                if data.get('reachable'):
                    txt = self.topo_scene.addText(f"N{nid} -> via N{data.get('next_hop', '?')}")
                    txt.setDefaultTextColor(Qt.white)
                else:
                    txt = self.topo_scene.addText(f"N{nid} -> DEAD")
                    txt.setDefaultTextColor(QtGui.QColor("#ff4444"))
                txt.setPos(hud_x + 10, y_off)
                y_off += 20

            # --- FIX 2: ALWAYS ANCHOR NODES 2 THROUGH 7 ON THE 2D MAP ---
            active_nodes = display_nodes[:]
            
            # Add any extra relay nodes (like 8) ONLY if they are actively routing a packet
            for n in self.active_route:
                if n not in active_nodes and n not in [1, 9, 10]:
                    active_nodes.append(n)
            active_nodes.sort()

            node_positions = {1: (0, -120)}
            R = 220
            N = len(active_nodes)

            if N == 1:
                node_positions[active_nodes[0]] = (0, -120 + R)
            elif N > 1:
                start_angle = math.pi * 0.85
                end_angle = math.pi * 0.15
                for i, n_id in enumerate(active_nodes):
                    angle = start_angle - (i * ((start_angle - end_angle) / (N - 1)))
                    x = R * math.cos(angle)
                    y = -120 + R * math.sin(angle)
                    node_positions[n_id] = (x, y)

            if self.active_route and len(self.active_route) > 1:
                passed_edges = {}
                idx_op = self.active_route.index(1) if 1 in self.active_route else -1

                for i in range(len(self.active_route) - 1):
                    n_current = self.active_route[i]
                    n_next = self.active_route[i + 1]

                    if n_current in node_positions and n_next in node_positions:
                        x1, y1 = node_positions[n_current]
                        x2, y2 = node_positions[n_next]

                        if idx_op != -1 and i < idx_op:
                            route_color = QtGui.QColor("#ff4444")
                        else:
                            route_color = QtGui.QColor("#00aaff")

                        edge_tuple = tuple(sorted((n_current, n_next)))

                        if edge_tuple in passed_edges:
                            pen = QtGui.QPen(route_color, 4, Qt.DotLine)
                            offset = 12
                            self.topo_scene.addLine(x1 + offset, y1 + offset, x2 + offset, y2 + offset, pen)
                        else:
                            pen = QtGui.QPen(route_color, 4, Qt.DashLine)
                            self.topo_scene.addLine(x1, y1, x2, y2, pen)
                            passed_edges[edge_tuple] = True

            nodes_to_draw = active_nodes + [1]
            for n_id in nodes_to_draw:
                if n_id not in node_positions: continue
                x, y = node_positions[n_id]
                
                is_in_packet_route = n_id in self.active_route
                # Safely check if a node is dead
                is_dead = False if n_id == 1 else not self.network_state.get(n_id, {}).get('reachable', False)

                # --- FIX 3: COLOR CODE DEAD NODES ON THE MAP ---
                if is_in_packet_route:
                    fill_color = QtGui.QColor("#007acc") # Bright Blue
                    border_color = Qt.white
                    border_width = 3
                    text_color = Qt.white
                elif is_dead:
                    fill_color = QtGui.QColor("#3a1515") # Dark Red
                    border_color = QtGui.QColor("#ff4444")
                    border_width = 2
                    text_color = QtGui.QColor("#ff4444")
                else:
                    fill_color = QtGui.QColor("#2d2d30") # Standard Grey
                    border_color = QtGui.QColor("#888888")
                    border_width = 2
                    text_color = QtGui.QColor("#aaaaaa")

                self.topo_scene.addEllipse(x - 25, y - 25, 50, 50, QtGui.QPen(border_color, border_width),
                                           QtGui.QBrush(fill_color))

                text = self.topo_scene.addText(f"N{n_id}")
                text.setDefaultTextColor(text_color)
                font = text.font()
                font.setPointSize(12)
                font.setBold(is_in_packet_route)
                text.setFont(font)
                text.setPos(x - 14, y - 12)

        except Exception as e:
            print(f"⚠️ UI Drawing Error Prevented: {e}")        
    """
    """
    def render_topology(self):
        try:
            self.topo_scene.clear()

            hud_x, hud_y = -395, -245
            self.topo_scene.addRect(hud_x, hud_y, 160, 240, QtGui.QPen(Qt.white),
                                    QtGui.QBrush(QtGui.QColor(37, 37, 38, 200)))
            title = self.topo_scene.addText("ROUTING TABLE")
            title.setDefaultTextColor(Qt.cyan)
            title.setPos(hud_x + 10, hud_y + 5)

            y_off = hud_y + 30
            for nid in sorted(self.network_state.keys()):
                # Exclude Node 1 (Self) AND Nodes 8, 9, 10
                if nid in [1, 8, 9, 10]: continue

                data = self.network_state[nid]
                if data.get('reachable'):
                    txt = self.topo_scene.addText(f"N{nid} -> via N{data['next_hop']}")
                    txt.setDefaultTextColor(Qt.white)
                else:
                    txt = self.topo_scene.addText(f"N{nid} -> DEAD")
                    txt.setDefaultTextColor(QtGui.QColor("#ff4444"))
                txt.setPos(hud_x + 10, y_off)
                y_off += 20

            # Filter active nodes for 2D Map (exclude 8, 9, 10)
            active_nodes = [nid for nid, d in self.network_state.items() if
                            d.get('reachable', False) and nid not in [1, 8, 9, 10]]
            for n in self.active_route:
                if n not in active_nodes and n not in [1, 8, 9, 10]:
                    active_nodes.append(n)
            active_nodes.sort()

            node_positions = {1: (0, -120)}
            R = 220
            N = len(active_nodes)

            if N == 1:
                node_positions[active_nodes[0]] = (0, -120 + R)
            elif N > 1:
                start_angle = math.pi * 0.85
                end_angle = math.pi * 0.15
                for i, n_id in enumerate(active_nodes):
                    angle = start_angle - (i * ((start_angle - end_angle) / (N - 1)))
                    x = R * math.cos(angle)
                    y = -120 + R * math.sin(angle)
                    node_positions[n_id] = (x, y)

            if self.active_route and len(self.active_route) > 1:
                passed_edges = {}
                idx_op = self.active_route.index(1) if 1 in self.active_route else -1

                for i in range(len(self.active_route) - 1):
                    n_current = self.active_route[i]
                    n_next = self.active_route[i + 1]

                    if n_current in node_positions and n_next in node_positions:
                        x1, y1 = node_positions[n_current]
                        x2, y2 = node_positions[n_next]

                        if idx_op != -1 and i < idx_op:
                            route_color = QtGui.QColor("#ff4444")
                        else:
                            route_color = QtGui.QColor("#00aaff")

                        edge_tuple = tuple(sorted((n_current, n_next)))

                        if edge_tuple in passed_edges:
                            pen = QtGui.QPen(route_color, 4, Qt.DotLine)
                            offset = 12
                            self.topo_scene.addLine(x1 + offset, y1 + offset, x2 + offset, y2 + offset, pen)
                        else:
                            pen = QtGui.QPen(route_color, 4, Qt.DashLine)
                            self.topo_scene.addLine(x1, y1, x2, y2, pen)
                            passed_edges[edge_tuple] = True

            nodes_to_draw = active_nodes + [1]
            for n_id in nodes_to_draw:
                if n_id not in node_positions: continue
                x, y = node_positions[n_id]
                is_in_packet_route = n_id in self.active_route

                fill_color = QtGui.QColor("#007acc") if is_in_packet_route else QtGui.QColor("#2d2d30")
                border_color = Qt.white if is_in_packet_route else QtGui.QColor("#888888")
                border_width = 3 if is_in_packet_route else 2

                self.topo_scene.addEllipse(x - 25, y - 25, 50, 50, QtGui.QPen(border_color, border_width),
                                           QtGui.QBrush(fill_color))

                text = self.topo_scene.addText(f"N{n_id}")
                text.setDefaultTextColor(Qt.white if is_in_packet_route else QtGui.QColor("#aaaaaa"))
                font = text.font()
                font.setPointSize(12)
                font.setBold(is_in_packet_route)
                text.setFont(font)
                text.setPos(x - 14, y - 12)

        except Exception as e:
            print(f"⚠️ UI Drawing Error Prevented: {e}")
    """

    def closeEvent(self, event):
        self.serial_thread.stop()
        event.accept()


if __name__ == "__main__":
    app = QtWidgets.QApplication(sys.argv)
    window = WsnGui()
    window.show()
    sys.exit(app.exec_())