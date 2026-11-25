import { useEffect, useState } from "react";

async function api(path, { method = "GET", body } = {}) {
  const r = await fetch(path, {
    method,
    credentials: "same-origin",
    headers: { "Content-Type": "application/json" },
    body: body ? JSON.stringify(body) : undefined,
  });
  const j = await r.json().catch(() => ({}));
  if (!r.ok) throw new Error(j.error || r.statusText);
  return j;
}

const LABELS = {
  cadence: "Cadence (steps/min)",
  stance_pct: "Stance (%)",
  rom: "Shin-tilt range (deg)",
  peak_gyro: "Peak shin speed (deg/s)",
  clearance_mm: "Foot clearance (mm)",
};
const TONE = { green: "g", amber: "a", red: "r" };
const Chip = ({ text, tone }) => <span className={`chip ${TONE[tone] || ""}`}>{text}</span>;
const when = (ts) => new Date(ts * 1000).toLocaleString();

function LineChart({ values }) {
  if (!values.length) return <div className="muted">No data yet</div>;
  const lo = Math.min(...values);
  const hi = Math.max(...values);
  const span = hi - lo || 1;
  const pts = values
    .map((v, i) => `${values.length < 2 ? 50 : (i / (values.length - 1)) * 100},${95 - ((v - lo) / span) * 90}`)
    .join(" ");
  return (
    <div>
      <div className="muted small">min {lo.toFixed(1)} · max {hi.toFixed(1)}</div>
      <svg viewBox="0 0 100 100" preserveAspectRatio="none" className="chart">
        <polyline points={pts} fill="none" stroke="currentColor" strokeWidth="1.8" vectorEffect="non-scaling-stroke" />
      </svg>
    </div>
  );
}

function useLive() {
  const [live, setLive] = useState(null);
  const [trace, setTrace] = useState([]);
  useEffect(() => {
    let ws;
    let timer;
    let closed = false;
    const open = () => {
      ws = new WebSocket(`${location.protocol === "https:" ? "wss" : "ws"}://${location.host}/ws/live`);
      ws.onmessage = (e) => {
        const d = JSON.parse(e.data);
        setLive(d);
        if (d.telemetry) setTrace((t) => [...t.slice(-299), Number(d.telemetry.angle)]);
      };
      ws.onclose = () => {
        if (!closed) timer = setTimeout(open, 2000);
      };
    };
    open();
    return () => {
      closed = true;
      clearTimeout(timer);
      ws.close();
    };
  }, []);
  return [live, trace, () => setTrace([])];
}

function Login({ onDone }) {
  const [u, setU] = useState("");
  const [p, setP] = useState("");
  const [err, setErr] = useState("");
  const submit = async (e) => {
    e.preventDefault();
    try {
      onDone(await api("/api/login", { method: "POST", body: { username: u, password: p } }));
    } catch (x) {
      setErr(x.message);
    }
  };
  return (
    <div className="login card">
      <h1 style={{ marginTop: 0, color: "var(--ac)" }}>KneeTrack</h1>
      <form onSubmit={submit} className="form">
        <label htmlFor="u">Username</label>
        <input id="u" value={u} onChange={(e) => setU(e.target.value)} autoComplete="username" required />
        <label htmlFor="p">Password</label>
        <input id="p" type="password" value={p} onChange={(e) => setP(e.target.value)} autoComplete="current-password" required />
        <div className="err">{err}</div>
        <button className="primary" style={{ width: "100%" }}>Log in</button>
      </form>
    </div>
  );
}

function Live({ live, trace, clearTrace, who, patients, setWho }) {
  const [msg, setMsg] = useState("");
  const t = live?.telemetry;
  const running = !!live?.active_session;
  const start = async () => {
    try {
      clearTrace();
      await api("/api/sessions/start", { method: "POST", body: { user_id: who } });
      setMsg("Session running");
    } catch (x) {
      setMsg(x.message);
    }
  };
  const stop = async () => {
    try {
      const r = await api("/api/sessions/stop", { method: "POST" });
      const n = r.features?.strides || 0;
      setMsg(`Saved. Strides found: ${n}${r.features?.valid ? "" : " (10 or more are needed for a result)"}`);
    } catch (x) {
      setMsg(x.message);
    }
  };
  const faults = [live?.faults?.ultrasonic && "foot sensor", live?.faults?.imu && "IMU"].filter(Boolean);
  return (
    <>
      <div className="card row">
        {patients.length > 1 && (
          <select value={who} onChange={(e) => setWho(Number(e.target.value))} aria-label="Patient">
            {patients.map((u) => <option key={u.id} value={u.id}>{u.full_name || u.username}</option>)}
          </select>
        )}
        <button className="primary" onClick={start} disabled={running}>Start session</button>
        <button onClick={stop} disabled={!running}>Stop session</button>
        <span className="muted">{msg}</span>
      </div>
      <div className="card grid">
        <div className="stat"><b>{t ? Number(t.angle).toFixed(1) : "-"}</b><span>Shin tilt (deg)</span></div>
        <div className="stat"><b>{t ? Math.round(t.clearance_mm || 0) : "-"}</b><span>Foot height (mm)</span></div>
        <div className="stat"><b>{t ? (t.stance ? "On ground" : "Swing") : "-"}</b><span>Foot phase</span></div>
        <div className="stat"><b>{t ? <Chip text={["Normal", "Mild", "Significant"][live.gait] || "?"} tone={["green", "amber", "red"][live.gait]} /> : "-"}</b><span>Gait check</span></div>
        <div className="stat"><b>{t ? (faults.length ? <Chip text={faults.join(", ")} tone="red" /> : <Chip text="None" tone="green" />) : "-"}</b><span>Sensor faults</span></div>
      </div>
      <div className="card">
        <div className="muted small">Shin tilt, last 60 seconds</div>
        <LineChart values={trace} />
        {!t && <div className="muted">Start a session to see live knee data.</div>}
      </div>
    </>
  );
}

function SessionDetail({ id }) {
  const [s, setS] = useState(null);
  const [err, setErr] = useState("");
  useEffect(() => {
    setS(null);
    setErr("");
    api(`/api/sessions/${id}/summary`).then(setS).catch((e) => setErr(e.message));
  }, [id]);
  if (err) return <div className="card err">{err}</div>;
  if (!s) return <div className="card muted">Loading…</div>;
  const f = s.features || {};
  const p = s.personal || {};
  return (
    <div className="card">
      <div className="row">
        <b>Session {id}</b>
        <span className="muted small">{f.strides || 0} strides, {f.fault_pct || 0}% of samples had a sensor fault</span>
        <a href={`/api/sessions/${id}/report.pdf`} target="_blank" rel="noreferrer"><button className="primary">PDF report</button></a>
      </div>
      {!f.valid ? <p>Not enough walking to judge this session (10 or more strides needed).</p> : (
        <>
          <p className="muted">Compared with typical healthy ranges ({s.literature.group}), provisional:</p>
          <table>
            <thead><tr><th>Measure</th><th>Value</th><th>Typical range</th><th /></tr></thead>
            <tbody>
              {Object.entries(s.literature.features).map(([k, v]) => (
                <tr key={k}>
                  <td>{LABELS[k] || k}</td><td>{v.value ?? "-"}</td>
                  <td>{v.band ? `${v.band[0]} to ${v.band[1]}` : "-"}</td>
                  <td><Chip text={v.status} tone={v.status} /></td>
                </tr>
              ))}
            </tbody>
          </table>
          <p className="muted">Compared with your own baseline: <b>{p.state}</b> ({p.baseline_sessions || 0} of {p.needed || 3} baseline sessions)</p>
          {p.state === "compared" && (
            <table>
              <thead><tr><th>Measure</th><th>Value</th><th>Your usual</th><th>z</th><th /></tr></thead>
              <tbody>
                {Object.entries(p.features).map(([k, v]) => (
                  <tr key={k}>
                    <td>{LABELS[k] || k}</td><td>{v.value}</td><td>{v.mean} (sd {v.sd})</td><td>{v.z}</td>
                    <td><Chip text={v.status} tone={v.status} /></td>
                  </tr>
                ))}
              </tbody>
            </table>
          )}
        </>
      )}
    </div>
  );
}

function Sessions({ who }) {
  const [list, setList] = useState([]);
  const [sel, setSel] = useState(null);
  useEffect(() => {
    setSel(null);
    api(`/api/sessions?user_id=${who}`).then(setList).catch(() => setList([]));
  }, [who]);
  return (
    <>
      <div className="card">
        {!list.length ? <span className="muted">No finished sessions yet.</span> : (
          <table>
            <thead><tr><th>#</th><th>Date</th><th>Duration</th><th>Strides</th><th>Valid</th></tr></thead>
            <tbody>
              {list.map((s) => (
                <tr key={s.id} className="click" onClick={() => setSel(s.id)}>
                  <td>{s.id}</td><td>{when(s.start_ts)}</td><td>{Math.round(s.duration_s)} s</td>
                  <td>{s.strides}</td><td>{s.valid ? "yes" : "no"}</td>
                </tr>
              ))}
            </tbody>
          </table>
        )}
      </div>
      {sel && <SessionDetail id={sel} />}
    </>
  );
}

function Trends({ who }) {
  const [data, setData] = useState([]);
  const [feat, setFeat] = useState("cadence");
  useEffect(() => {
    api(`/api/trend?user_id=${who}`).then(setData).catch(() => setData([]));
  }, [who]);
  return (
    <div className="card">
      <div className="row">
        <select value={feat} onChange={(e) => setFeat(e.target.value)} aria-label="Measure">
          {Object.entries(LABELS).map(([k, v]) => <option key={k} value={k}>{v}</option>)}
        </select>
        <span className="muted">{data.length ? `${data.length} valid sessions` : "No valid sessions yet."}</span>
      </div>
      <LineChart values={data.map((d) => d[feat])} />
    </div>
  );
}

const EMPTY = { username: "", password: "", role: "patient", full_name: "", age: "", sex: "female", height_cm: "", weight_kg: "" };

function Users({ users, reload }) {
  const [f, setF] = useState(EMPTY);
  const [a, setA] = useState({ physio_id: "", patient_id: "" });
  const [msg, setMsg] = useState("");
  const set = (k) => (e) => setF({ ...f, [k]: e.target.value });
  const create = async (e) => {
    e.preventDefault();
    const body = { username: f.username, password: f.password, role: f.role, full_name: f.full_name, sex: f.sex };
    ["age", "height_cm", "weight_kg"].forEach((k) => { if (f[k]) body[k] = Number(f[k]); });
    try {
      await api("/api/users", { method: "POST", body });
      setF(EMPTY);
      setMsg("User created.");
      reload();
    } catch (x) {
      setMsg(x.message);
    }
  };
  const assign = async () => {
    const ph = a.physio_id || users.find((u) => u.role === "physio")?.id;
    const pa = a.patient_id || users.find((u) => u.role === "patient")?.id;
    try {
      await api("/api/assign", { method: "POST", body: { physio_id: Number(ph), patient_id: Number(pa) } });
      setMsg("Assigned.");
    } catch (x) {
      setMsg(x.message);
    }
  };
  const opts = (role) => users.filter((u) => u.role === role).map((u) => <option key={u.id} value={u.id}>{u.username}</option>);
  return (
    <>
      <div className="card">
        <table>
          <thead><tr><th>User</th><th>Role</th><th>Name</th><th>Age</th><th>Sex</th></tr></thead>
          <tbody>{users.map((u) => <tr key={u.id}><td>{u.username}</td><td>{u.role}</td><td>{u.full_name}</td><td>{u.age}</td><td>{u.sex}</td></tr>)}</tbody>
        </table>
      </div>
      <div className="card">
        <b>Create user</b>
        <form onSubmit={create} className="form grid">
          <div><label htmlFor="n1">Username</label><input id="n1" value={f.username} onChange={set("username")} required /></div>
          <div><label htmlFor="n2">Password (8+ chars)</label><input id="n2" type="password" value={f.password} onChange={set("password")} required /></div>
          <div><label htmlFor="n3">Role</label><select id="n3" value={f.role} onChange={set("role")}><option>patient</option><option>physio</option><option>admin</option></select></div>
          <div><label htmlFor="n4">Full name</label><input id="n4" value={f.full_name} onChange={set("full_name")} /></div>
          <div><label htmlFor="n5">Age</label><input id="n5" type="number" value={f.age} onChange={set("age")} /></div>
          <div><label htmlFor="n6">Sex</label><select id="n6" value={f.sex} onChange={set("sex")}><option>female</option><option>male</option><option>other</option></select></div>
          <div><label htmlFor="n7">Height (cm)</label><input id="n7" type="number" value={f.height_cm} onChange={set("height_cm")} /></div>
          <div><label htmlFor="n8">Weight (kg)</label><input id="n8" type="number" value={f.weight_kg} onChange={set("weight_kg")} /></div>
          <div style={{ alignSelf: "end" }}><button className="primary">Create</button></div>
        </form>
      </div>
      <div className="card">
        <b>Assign patient to physio</b>
        <div className="row">
          <select value={a.physio_id} onChange={(e) => setA({ ...a, physio_id: e.target.value })} aria-label="Physio">{opts("physio")}</select>
          <select value={a.patient_id} onChange={(e) => setA({ ...a, patient_id: e.target.value })} aria-label="Patient">{opts("patient")}</select>
          <button onClick={assign}>Assign</button>
        </div>
      </div>
      <div className="muted">{msg}</div>
    </>
  );
}

function Shell({ me, onOut }) {
  const [tab, setTab] = useState("live");
  const [users, setUsers] = useState([]);
  const [who, setWho] = useState(me.id);
  const [live, trace, clearTrace] = useLive();
  const reload = () => api("/api/users").then(setUsers).catch(() => setUsers([]));
  useEffect(() => { reload(); }, []);
  const patients = users.filter((u) => u.role === "patient" || u.id === me.id);
  const tabs = [["live", "Live"], ["sessions", "Sessions"], ["trends", "Trends"], ...(me.role === "admin" ? [["users", "Users"]] : [])];
  const baseOk = live?.serial && !live?.env_stale;
  return (
    <>
      <header className="top">
        <h1>KneeTrack</h1>
        <span className={`chip ${live?.ble ? "g" : "r"}`}>Knee: {live?.ble ? "connected" : "not connected"}</span>
        <span className={`chip ${baseOk ? "g" : "a"}`}>{baseOk ? `Base: OK, gas ${live.gas_level}` : "Base: offline"}</span>
        <span className="muted">{me.username} ({me.role})</span>
        <button onClick={async () => { await api("/api/logout", { method: "POST" }).catch(() => {}); onOut(); }}>Log out</button>
      </header>
      <main>
        {live?.hazard ? <div className="hazard">GAS HAZARD detected by the base station</div> : null}
        <nav className="tabs">
          {tabs.map(([id, label]) => <button key={id} className={tab === id ? "on" : ""} onClick={() => setTab(id)}>{label}</button>)}
        </nav>
        {tab === "live" && <Live live={live} trace={trace} clearTrace={clearTrace} who={who} patients={patients} setWho={setWho} />}
        {tab === "sessions" && <Sessions who={who} />}
        {tab === "trends" && <Trends who={who} />}
        {tab === "users" && <Users users={users} reload={reload} />}
        <p className="muted small">KneeTrack is a research prototype, not a medical device and not a diagnosis. Reference ranges are provisional. Shin tilt is a proxy for knee motion, not the true knee angle.</p>
      </main>
    </>
  );
}

export default function App() {
  const [me, setMe] = useState(undefined);
  useEffect(() => { api("/api/me").then(setMe).catch(() => setMe(null)); }, []);
  if (me === undefined) return <div className="center muted">Loading…</div>;
  if (me === null) return <Login onDone={setMe} />;
  return <Shell me={me} onOut={() => setMe(null)} />;
}
