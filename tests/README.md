# Tests

## Simulation test matrix

`sim_test_matrix.ps1` runs `traffic_engine.exe --sim-test` on 5 networks with 3 seeds (15 tests,
6 at the same time) and writes a summary for each network:

- `demo`: the demo workspace of the program.
- `kizilay`, `kizilay200`: Kızılay, Ankara (lights, dual carriageways). Density 100 % and 200 %.
- `eskisehir`, `eskisehir70`: a part of Eskişehir (many junctions without lights, one-way
  roads, bridges). Density 100 % and 70 %.

```bash
powershell -ExecutionPolicy Bypass -File C:\Users\tcelik\Desktop\traffic_engine\tests\sim_test_matrix.ps1 -Name before
```

Build the Release version first. A test with 2 hours of simulated time takes some minutes.

Compare the results before and after a change of the traffic rules. The overlaps and the
standstills of 300 s or more must not increase. The reports (`<network>_s<seed>.txt`) give the
vehicles, the lanes, the junctions and the wait chain of each problem.

## Test data

`osm\kizilay.osm` and `osm\eskisehir.osm` are OpenStreetMap extracts (Overpass API, 2026-10-31).

Map data (c) OpenStreetMap contributors. The data is available under the Open Database License
(ODbL 1.0): https://www.openstreetmap.org/copyright
