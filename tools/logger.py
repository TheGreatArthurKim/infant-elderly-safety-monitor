import socket
import threading
import time

ESP32_IP = "192.168.45.80"
ESP32_PORT = 8080

LIVE_FILENAME = "pressure_log.csv"
DUMP_FILENAME = "dump.csv"

CSV_HEADER = "pc_time,millis,temp_c,pressure_mbar,dp_mbar"
RECV_TIMEOUT = 10
RECONNECT_DELAY = 3

pending = []
pending_lock = threading.Lock()


def keyboard_thread():
    """r=dump, c=clear LittleFS, s=resume live TCP."""
    while True:
        try:
            ch = input().strip().lower()
        except EOFError:
            break

        if ch in ("r", "c", "s"):
            with pending_lock:
                pending.append(ch)
            print(f"[cmd] '{ch}'")
        elif ch:
            print("[cmd] use r(dump), c(clear), s(resume)")


def connect():
    while True:
        try:
            sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            sock.settimeout(RECV_TIMEOUT)
            print(f"Connecting to {ESP32_IP}:{ESP32_PORT} ...")
            sock.connect((ESP32_IP, ESP32_PORT))
            print("Connected.")
            send_sync(sock)
            return sock
        except (socket.timeout, OSError) as e:
            print(f"[!] Connect failed ({e}). Retry in {RECONNECT_DELAY}s ...")
            time.sleep(RECONNECT_DELAY)


def send_sync(sock):
    epoch_ms = time.time_ns() // 1_000_000
    sock.sendall(f"SYNC,{epoch_ms}\n".encode("utf-8"))
    print(f"[sync] sent SYNC,{epoch_ms}")


def send_command(sock, cmd):
    sock.sendall(f"{cmd}\n".encode("utf-8"))


threading.Thread(target=keyboard_thread, daemon=True).start()

live_file = open(LIVE_FILENAME, "w", newline="", encoding="utf-8")
live_file.write(CSV_HEADER + "\n")
live_file.flush()

buffer = ""
mode = "live"
dump_file = None
sock = connect()

print(
    f"Saving live data to {LIVE_FILENAME}. "
    f"[r]=dump to {DUMP_FILENAME}  [c]=clear  [s]=resume  [Ctrl+C]=quit"
)

try:
    while True:
        with pending_lock:
            to_send = pending[:]
            pending.clear()

        for cmd in to_send:
            while True:
                try:
                    send_command(sock, cmd)
                    break
                except OSError as e:
                    print(f"[!] Command send failed ({e}). Reconnecting ...")
                    try:
                        sock.close()
                    except OSError:
                        pass
                    sock = connect()
                    buffer = ""

        try:
            data = sock.recv(1024).decode("utf-8", errors="ignore")
        except socket.timeout:
            print("\n[!] recv timeout. Reconnecting ...")
            sock.close()
            sock = connect()
            buffer = ""
            continue
        except OSError as e:
            print(f"\n[!] socket error ({e}). Reconnecting ...")
            sock.close()
            sock = connect()
            buffer = ""
            continue

        if not data:
            print("\n[!] ESP32 disconnected. Reconnecting ...")
            sock.close()
            sock = connect()
            buffer = ""
            continue

        buffer += data
        while "\n" in buffer:
            line, buffer = buffer.split("\n", 1)
            line = line.strip()
            if not line:
                continue

            if line == "<<<DUMP_START>>>":
                if dump_file:
                    dump_file.close()
                dump_file = open(DUMP_FILENAME, "w", newline="", encoding="utf-8")
                mode = "dump"
                print(f"\n[dump] start -> {DUMP_FILENAME}")
                continue

            if line == "<<<DUMP_END>>>":
                if dump_file:
                    dump_file.close()
                    dump_file = None
                mode = "live"
                print("[dump] complete. Press s to resume live TCP.\n")
                continue

            if line == "<<<CLEARED>>>":
                print("[chip] LittleFS data.csv cleared\n")
                continue

            if mode == "dump":
                if dump_file:
                    dump_file.write(line + "\n")
                    dump_file.flush()
                continue

            live_file.write(line + "\n")
            live_file.flush()
            print(line)

except KeyboardInterrupt:
    print("\nStopped by Ctrl+C.")
finally:
    try:
        sock.close()
    except OSError:
        pass
    if dump_file:
        dump_file.close()
    live_file.close()
    print("Closed. Files saved.")
