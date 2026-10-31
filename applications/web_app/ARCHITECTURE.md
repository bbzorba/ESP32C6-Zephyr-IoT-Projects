# Home Hub architecture

How the ESP32-C6 Home Hub, its client nodes, the MQTT broker and the Live
Lab page on [bbzorba.github.io](https://bbzorba.github.io/live) fit
together. Build, wiring and API details are in [README.md](README.md).

## The big picture

The system has two hops with two different patterns:

1. **Nodes → hub: request/response (HTTP).** Each client node POSTs its
   readings to the hub every 10 s. The hub keeps the latest report per node.
2. **Hub → browsers: publish/subscribe (MQTT).** Every 10 s the hub publishes
   one message containing its own readings, the BME688 and *all* nodes. The
   broker fans it out to every browser that has the Live Lab page open.

Only the hub talks to the broker, so nodes never need internet access, MQTT
or credentials. Everything a node reports reaches the website automatically.

```mermaid
flowchart LR
    subgraph LAN["Home network"]
        direction LR
        subgraph HUB["ESP32-C6 Home Hub (Zephyr)"]
            direction TB
            TICK["main.c<br/>10 s sampler tick"]
            SENS["sensors.c + bme688.c<br/>cached readings"]
            NODES["nodes.c<br/>latest report per node"]
            SCAN["lan_scan.c<br/>ARP sweep + reverse DNS"]
            WEB["web_api.c<br/>HTTP server :80"]
            MQTT["mqtt_pub.c<br/>MQTT client"]
            WIFI["wifi.c<br/>Wi-Fi + recovery"]
            TICK --> SENS
            TICK -->|every 60th tick| SCAN
            TICK -->|trigger| MQTT
            SENS --> WEB
            SENS --> MQTT
            NODES --> WEB
            NODES --> MQTT
            SCAN --> WEB
        end
        BME["BME688<br/>(I2C0)"] --> SENS
        IMX["i.MX 8M Plus<br/>home_hub_node"] -->|"POST /api/node"| WEB
        LANB["Browser on the LAN"] -->|"GET / and /api/*"| WEB
        ROUTER["Router<br/>DHCP + DNS"]
        SCAN -.->|PTR queries| ROUTER
        WIFI -.-> ROUTER
    end
    MQTT -->|"publish state + status<br/>(TCP 1883)"| BROKER[("MQTT broker<br/>broker.hivemq.com")]
    BROKER -->|"WebSocket (wss 8884)"| PAGE["bbzorba.github.io/live<br/>visitor's browser"]
    PAGES["GitHub Pages<br/>static HTML + JS"] -->|serves page| PAGE
```

## Inside the hub

| Thread / context | Wakes up | Does |
|---|---|---|
| `main` (sampler) | every 10 s (one `k_timer`) | reads chip temperature and BME688 together, triggers the MQTT publish, starts a LAN scan every 60th tick |
| `mqtt_tid` | on the sampler's trigger, or when the keepalive is due | connects to the broker, publishes `state`, sends pings |
| `lan_scan_tid` | when the sampler (or "Scan now") asks | ~35 s ARP sweep of the /24, then PTR lookups for names |
| HTTP server (Zephyr) | on incoming requests | dashboard, JSON API, node reports |
| Wi-Fi work item | on connect timeouts | retries every 30 s, reboots after 5 min offline |

The sampler is the only periodic timer, so sensor reads, the MQTT publish
and (every 10 minutes) the scan start all happen in the same wake-up.

## Sequences

### Every 10 seconds: from sensor to website

```mermaid
sequenceDiagram
    autonumber
    participant N as i.MX node
    participant H as Hub (ESP32-C6)
    participant B as MQTT broker
    participant W as Live Lab page (browser)

    W->>B: SUBSCRIBE bbzorba/home-hub/v1/state, /status
    B-->>W: retained state + status (latest values at once)
    loop every 10 s on the node
        N->>H: POST /api/node {id, model, sensors[]}
        H-->>N: 200 {"ok":true}
    end
    loop every 10 s on the hub (sampler tick)
        H->>H: read die temp + BME688
        H->>B: PUBLISH state {hub, env, nodes} (retain)
        B-->>W: state
        W->>W: render cards, "updated just now"
    end
```

### Hub connects, drops off, comes back

```mermaid
sequenceDiagram
    autonumber
    participant H as Hub
    participant B as MQTT broker
    participant W as Live Lab page

    H->>B: CONNECT (will: status = "offline", retain)
    B-->>H: CONNACK
    H->>B: PUBLISH status = "online" (retain)
    B-->>W: status online → green "Live"
    Note over H: power or Wi-Fi lost
    B->>B: no packet for 1.5 × keepalive (~90 s)
    B-->>W: last will: status offline → "Hub is offline", values greyed
    Note over H: Wi-Fi back (or reboot after 5 min offline)
    H->>B: CONNECT, status = "online"
    B-->>W: status online, fresh state follows
```

### A node loses the hub

```mermaid
sequenceDiagram
    autonumber
    participant N as Node
    participant H as Hub

    N-xH: POST /api/node (TCP connect fails)
    Note over N: stop sending
    loop every 60 s
        N-xH: TCP connect only
    end
    N->>H: TCP connect succeeds
    Note over N: resume reports every 10 s
    Note over H: node card shows "offline"<br/>after 30 s without a report
```

### LAN scan (every 10 minutes or "Scan now")

```mermaid
sequenceDiagram
    autonumber
    participant S as Sampler tick
    participant L as lan_scan thread
    participant D as Devices on the /24
    participant R as Router DNS

    S->>L: lan_scan_request() (60th tick)
    L->>L: clear ARP cache
    loop 16 hosts per batch, 2.1 s per batch
        L->>D: 1-byte UDP to port 9 (forces an ARP request)
        D-->>L: ARP reply (every live host answers)
        L->>L: read ARP table → mark online
    end
    loop every device seen
        L->>R: PTR x.x.x.x.in-addr.arpa
        R-->>L: hostname (if the device sent one via DHCP)
    end
```

## Adding a new node

Nothing changes on the hub or the website:

1. On the new board, every 10 s, POST JSON to `http://home-hub/api/node`:
   ```json
   {"id":"<unique id, e.g. hostname>","model":"<board name>",
    "sensors":[{"name":"Temperature","value":21.5,"unit":"°C"}]}
   ```
   Send raw UTF-8 (Zephyr's JSON parser keeps `\uXXXX` escapes as text).
2. The hub's local dashboard shows it as a new card; the next MQTT `state`
   message contains it, so the Live Lab page shows it too.

Limits: 4 nodes, 16 sensors per node, names up to 23 bytes, units up to 7
bytes (`MAX_NODES`, `MAX_SENSORS` in `src/nodes.c`). A node that stops
reporting is shown as offline after 30 s; if the table is full, the node
silent for longest is replaced.

For a node that should bypass the hub and publish to MQTT itself, the Live
Lab page would need to subscribe to its topic too; the hub-centred design
avoids that.

## Security notes

- The broker is public: anyone who knows the topic can read the values and
  could publish fake ones. Only sensor values are published; the LAN device
  list, IP/MAC addresses and Wi-Fi name stay on the local dashboard. A
  private broker (e.g. HiveMQ Cloud) with a write password for the hub and a
  read-only user for the page closes this.
- The local dashboard and `/api/node` have no authentication; they are only
  reachable from the LAN.
