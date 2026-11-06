# Traffic Engine external API (version 1, draft)

The external API lets other programs read the state of the simulation and control it:

- read the workspaces, the simulation state, the statistics, the traffic lights, the vehicles and
  the congestion analysis (level of service, delay and queues of the junctions),
- send a phase to a traffic light (for example from an external traffic light controller),
- set the global traffic density,
- run, pause, change the speed or start the simulation again.

The API uses HTTP/1.1 and JSON (UTF-8). The server is in the program: it reads the simulation
between two frames.

> This is a draft. The schema of the external traffic light feed is not final. Tell us the
> necessary changes before other systems use it.

## Start the server

- Settings > API > "Start the API server". Default port: 8088. Default address: `127.0.0.1`
  (only this computer).
- "Listen on all network interfaces": other computers can connect. Then set a token.
- Token: if the token is not empty, each request must have the header
  `Authorization: Bearer <token>` or `X-Api-Token: <token>`. Else the answer is HTTP 401.
- Command line (test): `traffic_engine.exe --api-port 8099` starts the server on `127.0.0.1:8099`
  for this session only (config.xml does not change).
- The status bar shows the address of the server when it runs.

## General rules

- All paths start with `/api/v1`.
- `{w}` is a workspace: the index of the tab (`0`, `1`, ...) or `current` (the visible tab).
- A POST request must have the header `Content-Type: application/json` and a JSON object body.
  (A web page cannot send a simple cross-site request to the local API.)
- Each answer is a JSON object with `"ok": true` or `"ok": false` and `"error": "<text>"`.
- HTTP status: 200 (correct), 400 (incorrect value), 401 (token), 404 (unknown path or element),
  405 (method), 413 (request larger than 1 MB), 415 (content type).
- The server closes the connection after each answer (`Connection: close`).
- Units: meters, seconds, km/h, liters, kilograms. Positions: `x`, `y` in meters (x to the east,
  y to the north). A workspace with a geographic origin (OpenStreetMap import) also gives `lat`,
  `lon` (WGS 84).

## Read

### GET /api/v1/status

The program and all workspaces.

```json
{
  "ok": true,
  "program": { "name": "Traffic Engine", "version": "0.1.0", "build": "Release", "qt": "5.12.12", "time": "2026-10-31T11:20:00" },
  "workspaces": [
    {
      "index": 0, "current": true, "name": "Demo city", "file": "", "ground": "grass",
      "nodes": 9, "roads": 8, "road_km": 2.16, "lane_km": 6.1, "traffic_lights": 1, "signs": 9, "objects": 6,
      "origin": { "lat": 39.9208, "lon": 32.8541 },
      "simulation": { "clock": "08:00:00", "time_s": 1800.0, "speed": 1, "running": true, "editor_mode": false,
                      "traffic_density_percent": 100, "pedestrian_density_percent": 100,
                      "vehicles": 85, "pedestrians": 30, "completed_trips": 412, "gridlock_removed": 0 }
    }
  ]
}
```

`origin` is only in a workspace with a geographic origin.

### GET /api/v1/workspaces

The same list as `workspaces` in `/status`.

### GET /api/v1/workspaces/{w}

`workspace`, `simulation`, `statistics`, `traffic_lights` and `congestion` of one workspace.
`statistics` has one entry for each metric of the Charts panel, with the last value and the unit:

```json
"statistics": { "mean_speed": { "value": 23.5, "unit": "km/h" }, "density": { "value": 18.2, "unit": "veh/km" } }
```

### GET /api/v1/workspaces/{w}/lights

```json
{
  "ok": true,
  "clock": "08:00:00",
  "traffic_lights": [
    {
      "id": 18, "node": 3, "algorithm": "fixed_time", "phases": 2,
      "phase": 1, "stage": "green", "stage_remaining_s": 12.4, "external": false,
      "approaches": [
        { "road": 5, "name": "Atatürk Bulvarı", "signal": "green", "remaining_s": 12.4, "waiting": 0 },
        { "road": 7, "signal": "red", "remaining_s": 17.4, "waiting": 6 }
      ]
    }
  ]
}
```

- `algorithm`: `fixed_time`, `actuated`, `external`.
- `phase`: 1 to `phases`. `stage`: `red_yellow`, `green`, `yellow`, `all_red`.
- `signal` of an approach: `off`, `red`, `red_yellow`, `green`, `yellow`.
- `waiting`: vehicles that wait on the approach now.

### GET /api/v1/workspaces/{w}/vehicles

```json
{
  "ok": true, "count": 1,
  "vehicles": [
    { "id": 412, "type": "sedan", "x": 120.5, "y": -3.2, "lat": 39.92091, "lon": 32.85551,
      "heading_deg": 0.4, "speed_kmh": 42.1, "acceleration": 0.3, "fuel_l": 0.042, "co2_kg": 0.098,
      "passengers": 2, "stops": 1, "origin_node": 12, "destination_node": 40 }
  ]
}
```

`type`: `sedan`, `bus`, `motorcycle`.

### GET /api/v1/workspaces/{w}/congestion

The congestion analysis (View > Congestion analysis): the junctions with 3 or more arms and the
junctions with a traffic light, the worst junction first.

```json
{
  "ok": true, "clock": "08:20:00", "window_s": 300,
  "junctions": [
    {
      "node": 6, "signalized": true, "los": "D", "delay_s": 51.2, "flow_veh_h": 840,
      "queue_m": 242.0, "waiting": 31, "spillback": false,
      "main_cause": "red_light", "main_cause_share": 0.63,
      "approaches": [
        { "road": 14, "name": "Atatürk Bulvarı", "los": "E", "delay_s": 66.0, "queue_m": 242.0,
          "max_queue_m": 260.5, "queue_vehicles": 31, "length_m": 410.0, "passages": 52,
          "spillback": false, "main_cause": "red_light" }
      ]
    }
  ]
}
```

- `window_s`: the time window of the delay, the flow and the maximum queue (60, 300 or 900 s).
- `los`: level of service, `A` to `F`. The limits of the control delay are from the Highway Capacity
  Manual 2010. With lights: A <= 10, B <= 20, C <= 35, D <= 55, E <= 80 s, F above. Without lights:
  A <= 10, B <= 15, C <= 25, D <= 35, E <= 50 s, F above. A spillback gives F.
- `delay_s`: mean wait per vehicle at the junction (vehicles through the junction and vehicles in the
  queues now). `flow_veh_h`: vehicles per hour through the junction.
- `queue_m`, `waiting`: the longest queue of an approach and the vehicles in all queues now.
- `spillback`: the queue fills the approach. It blocks the junction before it.
- `main_cause`: the cause of the largest part of the waiting time: `queue`, `red_light`, `stop_sign`,
  `give_way`, `conflict`, `exit_full`, `junction_blocked`, `pedestrians`, `bus_stop`,
  `level_crossing`, `lane_blocked`, `merge_courtesy`, `no_route`, `none`.
- `blocking_node` (only with `exit_full`): the next junction. The queue comes from there.

### GET /api/v1/density

```json
{ "ok": true, "vehicles": 100, "pedestrians": 100 }
```

## Control

### POST /api/v1/workspaces/{w}/lights/{id}/phase

Body: `{"phase": 2, "duration": 30}`

- `phase`: 1 to the number of phases of the light.
- `duration`: seconds, 1 to 3600 (default 30).
- The light goes to the green of this phase immediately (no yellow time).
- Algorithm `external`: the light keeps the phase for `duration` seconds. If no new phase comes
  in this time, the light continues with the fixed times.
- Other algorithms: the phase gets `duration` seconds of green, then the normal sequence continues.

Answer: `{"ok": true, "light": 18, "phase": 2, "duration_s": 30, "algorithm": "external"}`

Set the algorithm "External (API)" in the traffic light program (Tools > Traffic light program)
for a light that an external controller drives.

### POST /api/v1/workspaces/{w}/control

Body (all fields are optional): `{"running": true, "speed": 4, "reset": false}`

- `running`: run or pause.
- `speed`: 0.1 to 64 (x real time).
- `reset`: true starts the simulation again (no vehicles, clock at the start).

Answer: `{"ok": true, "simulation": { ... }}`

### POST /api/v1/density

Body: `{"vehicles": 120, "pedestrians": 100}` (percent of the rates of the traffic sources, 0 to
300). The values go to config.xml and apply immediately to all workspaces (as the toolbar button
"Traffic").

## Examples (curl)

```bash
curl http://127.0.0.1:8088/api/v1/status
curl http://127.0.0.1:8088/api/v1/workspaces/current/lights
curl http://127.0.0.1:8088/api/v1/workspaces/current/congestion
curl -X POST -H "Content-Type: application/json" -d "{\"phase\": 2, \"duration\": 40}" http://127.0.0.1:8088/api/v1/workspaces/current/lights/18/phase
curl -X POST -H "Content-Type: application/json" -d "{\"vehicles\": 150}" http://127.0.0.1:8088/api/v1/density
```

## Security

- The default address is `127.0.0.1`: only programs on this computer can connect.
- With "Listen on all network interfaces", use a token and a firewall rule. The API has no TLS:
  use it only in a trusted network.
- The answers have no CORS header: a web page in a browser cannot read the API. Use a program or
  a local proxy for a web dashboard.
