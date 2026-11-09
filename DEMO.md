# 5-minute demo script

Story: *"An ambulance system that dispatches on real roads, with live ambulance positions —
and a simulation that proves the dispatch rule helps."*

## Before you start (2 minutes, once)

```bash
cmake -S . -B build && cmake --build build -j
./build/ambulance_server            # run from the project root; open http://localhost:8080/map.html
```

* **Internet available:** the server uses real roads through OSRM (`provider: osrm`).
* **No internet / OSRM down:** everything still works on the stored graph (`provider: graph`,
  with a "graph fallback" note). To avoid the 4-second connection timeout on stage run
  `OSRM_URL=off ./build/ambulance_server`.
* **Phone on the same Wi-Fi:** open `http://<laptop-ip>:8080/driver.html`. Browsers allow GPS
  only on **https** or localhost, so for real GPS use an https tunnel (for example the deployed
  Render URL, or a tunnel tool). Without https use **Simulate driving** — no phone needed.
* Reset the data first if you have played with it: `curl -X POST localhost:8080/api/reset`.

## The demo

| Time | You do | You say / the audience sees |
|---|---|---|
| 0:00 | Open **map.html** on the laptop | Hyderabad: 5 ambulance markers at their stations (faded = no live GPS yet), 3 hospitals with free beds (`H 30/30`). "Dispatch decides by real travel time, not by a made-up graph." |
| 0:45 | Open **driver.html** on the phone (or a second browser tab). Pick **Unit 101**, tap **Start** (real GPS) or **Start simulation** | Unit 101's marker turns solid and starts moving: "its phone sends a GPS fix every 3 seconds. A fix older than 60 s is ignored, and the unit is treated as being at its station." |
| 1:30 | On the map choose **CRITICAL**, press **Click map to add**, click a spot close to the moving unit | A red pin appears in the *Waiting patients* list. |
| 2:00 | Press **Dispatch next** | A toast: unit, minutes to the patient, total minutes and the provider. Because it is CRITICAL, an **ADVANCED** unit is chosen. The route is drawn on real roads (red = to patient, blue dashed = to hospital) and the *Active trips* panel shows the ETA. "The unit's live position was used, not its station." |
| 2:45 | In the driver page raise the **simulation speed** to ~120 km/h (if simulating) and watch | The marker moves along the road; the trip chip changes: EN_ROUTE → AT_SCENE → TRANSPORTING → RETURNING. Hospital beds go down by one. |
| 3:15 | Press **Go off route** (driver page, simulation mode) while the unit is driving | Within a few seconds the map shows a toast: "Unit … is 380 m off its route — re-routed. ETA a → b min" and the route line redraws from the new position. "It leaves the planned route by more than 150 m, so the server recomputes the route." |
| 3:45 | Add **two or three** more patients (click the map), then press **Dispatch all (batch)** | One toast: all patients matched at once with the free units. "Greedy serves one patient at a time and can leave the next patient with a far unit. Batch uses the Hungarian algorithm to minimise the total severity-weighted response time." |
| 4:15 | Open **results/summary.md** (or `results/response_times.html`) | The simulation: 500 random emergencies × 10 scenarios, same emergencies for four strategies. Random ≈ 13.7 min average, smart rules ≈ 9.5 min; greedy sends an ADVANCED unit to HIGH/CRITICAL patients 90 % of the time vs 43 % for the naive rules; batch beat greedy in 10 of 10 paired runs (by about 0.3 min). |
| 4:45 | Say the limits (below) | Honest answers to the obvious questions. |

## If someone asks…

* **"Are those travel times real?"** With OSRM, yes (real road network, no live traffic). The
  committed `results/` were produced offline on the stored graph (straight line × 1.3); re-run
  `./build/aro_simulate` with internet to get OSRM numbers.
* **"What if the routing server is down?"** Dispatch continues on the stored graph and the response
  and the *Active trips* panel say so (`fallbackReason`). It retries OSRM after 30 seconds.
* **"Why is batch only slightly better?"** It only differs from greedy when several patients wait
  while several units are free at the same moment.
* **"Is this a real medical-grade system?"** No — it is a teaching project: no live traffic, simple
  severity weights, a single-file JSON database, and the free public OSRM demo server is for light use.

## Reset between runs

```bash
curl -X POST localhost:8080/api/reset
```
