"""User-space UDP link emulator between one viewer client and the server.

The viewer connects to --listen instead of the server. Traffic toward the
viewer (server -> viewer) crosses a bottleneck: each datagram occupies the link
for len*8/rate seconds behind a drop-tail queue of --buffer-kb, then takes
--delay-ms of propagation. Traffic from the viewer only takes the propagation
delay. No driver or elevation is needed, and QUIC sees a real bottleneck: its
own congestion control, retransmissions and ACK clocking all react to it.

Every 100 ms it appends one JSONL line to --log, stamped like the client verify
logs (QueryPerformanceCounter in 100 ns units), so it joins their timeline.
"""
import argparse
import heapq
import json
import socket
import threading
import time


def now_100ns() -> int:
    # perf_counter is QueryPerformanceCounter on Windows: the same clock the
    # clients and the WGC probe stamp their events with.
    return time.perf_counter_ns() // 100


def parse_addr(text: str):
    host, port = text.rsplit(":", 1)
    return host, int(port)


class Link:
    def __init__(self, args):
        self.rate_bps = args.down_mbps * 1e6
        self.delay_s = args.delay_ms / 1000.0
        self.buffer_bytes = int(args.buffer_kb * 1024)
        self.upstream = parse_addr(args.upstream)
        self.listen = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.tune(self.listen)
        self.listen.bind(parse_addr(args.listen))
        self.clients = {}            # viewer address -> socket toward the server
        self.heap = []               # (deliver_at, order, socket, address, payload)
        self.order = 0
        self.link_free_at = 0.0      # when the bottleneck finishes its queued bytes
        self.cv = threading.Condition()
        self.stats = dict(down_in=0, down_out=0, down_drop_pkts=0, down_drop_bytes=0, up=0)
        self.peak_backlog = 0
        self.log = open(args.log, "a", buffering=1)
        self.stop = threading.Event()

    @staticmethod
    def tune(sock):
        for opt in (socket.SO_RCVBUF, socket.SO_SNDBUF):
            sock.setsockopt(socket.SOL_SOCKET, opt, 8 * 1024 * 1024)

    def schedule(self, at, sock, addr, payload):
        heapq.heappush(self.heap, (at, self.order, sock, addr, payload))
        self.order += 1
        self.cv.notify()

    def backlog_bytes(self, now):
        return max(0.0, self.link_free_at - now) * self.rate_bps / 8

    def from_viewer(self):
        while not self.stop.is_set():
            try:
                payload, addr = self.listen.recvfrom(65536)
            except (ConnectionResetError, OSError):
                continue
            sock = self.clients.get(addr)
            if sock is None:
                sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
                self.tune(sock)
                sock.bind(("127.0.0.1", 0))
                self.clients[addr] = sock
                threading.Thread(target=self.from_server, args=(sock, addr), daemon=True).start()
            with self.cv:
                self.stats["up"] += len(payload)
                self.schedule(time.perf_counter() + self.delay_s, sock, self.upstream, payload)

    def from_server(self, sock, viewer):
        while not self.stop.is_set():
            try:
                payload, _ = sock.recvfrom(65536)
            except (ConnectionResetError, OSError):
                continue
            now = time.perf_counter()
            size = len(payload)
            with self.cv:
                self.stats["down_in"] += size
                backlog = self.backlog_bytes(now)
                if backlog + size > self.buffer_bytes:
                    self.stats["down_drop_pkts"] += 1
                    self.stats["down_drop_bytes"] += size
                    continue
                start = max(now, self.link_free_at)
                self.link_free_at = start + size * 8 / self.rate_bps
                self.peak_backlog = max(self.peak_backlog, int(backlog + size))
                self.schedule(self.link_free_at + self.delay_s, self.listen, viewer, payload)

    def deliver(self):
        while not self.stop.is_set():
            with self.cv:
                while not self.heap:
                    self.cv.wait(0.05)
                    if self.stop.is_set():
                        return
                at = self.heap[0][0]
                wait = at - time.perf_counter()
                if wait > 0:
                    self.cv.wait(min(wait, 0.002))
                    continue
                _, _, sock, addr, payload = heapq.heappop(self.heap)
                if sock is self.listen:
                    self.stats["down_out"] += len(payload)
            try:
                sock.sendto(payload, addr)
            except OSError:
                pass

    def report(self):
        last = dict(self.stats)
        while not self.stop.wait(0.1):
            with self.cv:
                cur = dict(self.stats)
                now = time.perf_counter()
                backlog = self.backlog_bytes(now)
                peak, self.peak_backlog = self.peak_backlog, int(backlog)
            delta = {k: cur[k] - last[k] for k in cur}
            last = cur
            self.log.write(json.dumps({
                "ev": "link", "t": now_100ns(),
                "in_bytes": delta["down_in"], "out_bytes": delta["down_out"],
                "drop_pkts": delta["down_drop_pkts"], "drop_bytes": delta["down_drop_bytes"],
                "up_bytes": delta["up"],
                "backlog_bytes": int(backlog), "peak_backlog_bytes": peak,
                "backlog_ms": round(backlog * 8 / self.rate_bps * 1000, 2),
            }) + "\n")

    def run(self):
        self.log.write(json.dumps({
            "ev": "link_start", "t": now_100ns(), "down_mbps": self.rate_bps / 1e6,
            "delay_ms": self.delay_s * 1000, "buffer_bytes": self.buffer_bytes,
            "listen": "%s:%d" % self.listen.getsockname(), "upstream": "%s:%d" % self.upstream,
        }) + "\n")
        for target in (self.from_viewer, self.deliver, self.report):
            threading.Thread(target=target, daemon=True).start()
        try:
            while True:
                time.sleep(1)
        except KeyboardInterrupt:
            self.stop.set()


def main():
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("--listen", required=True, help="host:port the viewer connects to")
    p.add_argument("--upstream", required=True, help="server host:port")
    p.add_argument("--down-mbps", type=float, required=True, help="bottleneck rate toward the viewer")
    p.add_argument("--delay-ms", type=float, default=10.0, help="one-way propagation delay")
    p.add_argument("--buffer-kb", type=float, default=0, help="drop-tail queue; 0 = 100 ms at the rate")
    p.add_argument("--log", required=True)
    args = p.parse_args()
    if args.buffer_kb <= 0:
        args.buffer_kb = max(64.0, args.down_mbps * 1e6 / 8 * 0.1 / 1024)
    Link(args).run()


if __name__ == "__main__":
    main()
