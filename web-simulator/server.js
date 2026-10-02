// Live WTA route 190 arrivals: GTFS-Realtime -> simple JSON. Needs Node 18+.
const express = require("express"), AdmZip = require("adm-zip"), { parse } = require("csv-parse/sync");
const { transit_realtime: RT } = require("gtfs-realtime-bindings");
const STATIC = "https://github.com/whatcomtrans/publicwtadata/raw/master/GTFS/wta_gtfs_latest.zip";
const TRIPS_RT = "https://bustracker.ridewta.com/gtfsrt/trips";
const ROUTE = "190";
const stops = {}, routeIds = new Set(), trips = {}, served = new Set();
const csv = (zip, n) => parse(zip.readAsText(n), { columns: true, skip_empty_lines: true, bom: true, relax_quotes: true });

const ready = (async () => {
  const zip = new AdmZip(Buffer.from(await (await fetch(STATIC)).arrayBuffer()));
  for (const r of csv(zip, "routes.txt")) if (r.route_short_name === ROUTE || r.route_id === ROUTE) routeIds.add(r.route_id);
  for (const t of csv(zip, "trips.txt")) if (routeIds.has(t.route_id)) trips[t.trip_id] = t.trip_headsign || "";
  for (const s of csv(zip, "stops.txt")) stops[s.stop_id] = s;
  for (const st of csv(zip, "stop_times.txt")) if (st.trip_id in trips) served.add(st.stop_id);
  console.log(`Route ${ROUTE}: ${routeIds.size} route ids, ${served.size} stops loaded`);
})().catch(e => console.error("Static GTFS load failed:", e));

const app = express();
app.use(express.static("public"));

app.get("/stops", async (_, res) => {
  await ready;
  res.json([...served].map(id => ({ id, code: stops[id]?.stop_code || "", name: stops[id]?.stop_name || id })));
});

app.get("/arrivals", async (req, res) => {
  try {
    await ready;
    const want = String(req.query.stop || "");
    const buf = await (await fetch(TRIPS_RT)).arrayBuffer();
    const feed = RT.FeedMessage.decode(new Uint8Array(buf));
    const now = Date.now() / 1000, out = [];
    for (const e of feed.entity) {
      const tu = e.tripUpdate; if (!tu) continue;
      if (!(routeIds.has(tu.trip.routeId) || tu.trip.tripId in trips)) continue;
      const sts = tu.stopTimeUpdate;
      const tm = s => { const t = s.arrival?.time ?? s.departure?.time; return t == null ? null : (typeof t === "number" ? t : t.toNumber()); };
      sts.forEach((s, i) => {
        if (s.stopId !== want && stops[s.stopId]?.stop_code !== want) return;
        const t = tm(s); if (t == null) return;
        const minutes = Math.round((t - now) / 60);
        if (minutes < 0) return;
        // stops the bus still has to make before yours (0 = yours is the next stop)
        const stopsAway = sts.slice(0, i).filter(x => (tm(x) ?? 0) >= now).length;
        out.push({ minutes, stopsAway, realtime: true, headsign: trips[tu.trip.tripId] || "" });
      });
    }
    out.sort((a, b) => a.minutes - b.minutes);
    res.json({ arrivals: out.slice(0, 3), updated: Date.now() });
  } catch (err) { console.error(err); res.status(502).json({ error: String(err) }); }
});

app.listen(3000, () => console.log("Open http://localhost:3000"));
