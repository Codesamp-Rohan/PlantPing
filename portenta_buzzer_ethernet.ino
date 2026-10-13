// Portenta H7 + Breakout - Call station (Mitsubishi GS2107 over Ethernet)
// - Web page (plant network), password protected
// - Pick who is calling (sender dropdown), type the location and a message, press Ring
// - HMI shows RING + sender + location + message, and beeps until OK/CLEAR is pressed
// - Optional: relay (PWM0 = pin 6) switches the 24 V buzzer
// - Mitsubishi HMI reads everything over Modbus TCP (port 502)
//
// Libraries (Library Manager): ArduinoModbus, ArduinoRS485
//
// Modbus map (Portenta = Modbus TCP server)              ->  GT Designer3 (MODBUS) address
//   reg 0       : 1 = call active, 0 = idle (HMI OK/CLEAR writes 0) -> 400001
//   reg 1       : total call count                                -> 400002
//   reg 2       : sender number (1..N)                            -> 400003
//   reg 10..29  : message text, 40 ASCII chars                    -> 400011 (40 chars / 20 words)
//   reg 60..69  : location text, 20 ASCII chars                   -> 400061 (20 chars / 10 words)
//   reg 80..89  : sender text, 20 ASCII chars                     -> 400081 (20 chars / 10 words)
//   reg 100     : System Signal 1-1 for the HMI (bit 14 = buzzer)  -> 400101
//   reg 150..   : area the HMI writes its own status to           -> 400151
//
// GT Designer3: Common -> GOT Environmental Setting -> System Information
//   [x] Use System Information
//   Read Device  (Controller -> GOT) first device: 400101
//   Write Device (GOT -> Controller) first device: 400151

#include <PortentaEthernet.h>
#include <Ethernet.h>
#include <ArduinoRS485.h>
#include <ArduinoModbus.h>

// ---------------- Settings you may change ----------------

// Password for the web page
const char* PASSWORD = "project1234567890";

// Who can send a call (shown in the "From" dropdown, max 20 characters each)
// Who can send a call (shown in the "From" dropdown, max 20 characters each)
const char* SENDERS[] = {
  "Rahul",
  "Navneet",
  "Harshil",
  "Prarthana",
  "Mayank",
  "Niranjan",
  "Arya",
  "Rohan",
  "Samanth",
  "Tushar",
  "Kanchan"
};
const int SENDER_COUNT = sizeof(SENDERS) / sizeof(SENDERS[0]);

// The call ends automatically after CALL_TIMEOUT_MS (or earlier if OK/CLEAR is pressed)
const unsigned long BUZZER_MS  = 0;       // 0 = beep for the whole call
const unsigned long DISPLAY_MS = 10000;   // call cuts after 10 seconds

// Which buzzers sound on a call
const bool USE_HMI_BEEP     = true;       // HMI built-in buzzer
const bool USE_RELAY_BUZZER = true;       // 24 V buzzer on the relay

// Sound pattern: true = "ring-ring" pattern, false = continuous
const bool HMI_BEEP_PATTERN    = false;
const bool RELAY_RING_PATTERN  = false;

// Text byte order for the HMI.
// Mitsubishi GOT normally reads the FIRST character from the LOW byte -> false.
// If the HMI shows letters swapped in pairs ("ELLO" as "EHLL"), change to true.
const bool TEXT_HIGH_BYTE_FIRST = false;

// Network (plant LAN)
const bool USE_DHCP = false;
byte mac[] = { 0x02, 0x00, 0x00, 0x10, 0x00, 0x02 };
IPAddress staticIp(172, 16, 41, 72);
IPAddress dns(172, 16, 40, 1);
IPAddress gateway(172, 16, 40, 1);
IPAddress subnet(255, 255, 248, 0);

// Relay (5 V NPN module: ON when LOW)
const int RELAY_PIN = 6;                  // Breakout PWM0
const int RELAY_ON  = LOW;
const int RELAY_OFF = HIGH;

// ---------------- Internals ----------------

const int MSG_MAX = 40;
const int LOC_MAX = 20;
const int SND_MAX = 20;

const int REG_ACTIVE = 0;    // RING            -> 400001
const int REG_COUNT  = 1;    // call count      -> 400002
const int REG_SNDNUM = 2;    // sender number   -> 400003
const int REG_SYS    = 4;    // HMI beep signal -> 400005 (System Information read device)
const int REG_MSG    = 10;   // message  (40 chars) -> 400011
const int REG_LOC    = 30;   // location (20 chars) -> 400031
const int REG_SND    = 40;   // sender   (20 chars) -> 400041
const int REG_GOT    = 50;   // HMI status write area -> 400051
const int REG_TOTAL  = 128;

const uint16_t HMI_BUZZER_BIT = (1 << 14);   // System Signal 1-1.b14 = Buzzer Output

EthernetServer server(80);         // web page
EthernetServer modbusServer(502);  // Modbus TCP for the HMI
EthernetClient modbusClient;
ModbusTCPServer modbusTCP;
bool networkUp = false;

String sessionToken;               // new random token each time the Portenta starts

bool buzzerActive = false;
bool callActive = false;
unsigned long callStart = 0;
uint16_t callCount = 0;

struct Call { String msg; String loc; String snd; unsigned long at; };
const int RECENT_MAX = 6;
Call recent[RECENT_MAX];
int recentCount = 0;

void ledGreen(bool on) { digitalWrite(LEDG, on ? LOW : HIGH); }
void ledRed(bool on)   { digitalWrite(LEDR, on ? LOW : HIGH); }
void ledBlue(bool on)  { digitalWrite(LEDB, on ? LOW : HIGH); }

// ---------------- Buzzers ----------------

// "ring-ring" pattern, repeats every 2 s: ON 400 ms, OFF 200 ms, ON 400 ms, OFF 1000 ms
bool ringPatternOn(unsigned long t) {
  unsigned long p = t % 2000;
  return (p < 400) || (p >= 600 && p < 1000);
}

void hmiBuzzer(bool on) {
  uint16_t v  = modbusTCP.holdingRegisterRead(REG_SYS);
  uint16_t nv = on ? (v | HMI_BUZZER_BIT) : (v & ~HMI_BUZZER_BIT);
  if (nv != v) modbusTCP.holdingRegisterWrite(REG_SYS, nv);
}

void relayBuzzer(bool on) {
  digitalWrite(RELAY_PIN, on ? RELAY_ON : RELAY_OFF);
}

void allBuzzersOff() {
  hmiBuzzer(false);
  relayBuzzer(false);
}

// ---------------- Web pages ----------------

const char CSS[] = R"rawliteral(
:root{--bg:#fafafa;--card:#fff;--fg:#09090b;--muted:#71717a;--border:#e4e4e7;--ring:#a1a1aa;--primary:#18181b;--pfg:#fafafa;--danger:#dc2626;--dbg:#fef2f2;--ok:#16a34a;--obg:#f0fdf4}
@media (prefers-color-scheme:dark){:root{--bg:#09090b;--card:#111113;--fg:#fafafa;--muted:#a1a1aa;--border:#27272a;--ring:#52525b;--primary:#fafafa;--pfg:#18181b;--dbg:#2a0a0a;--obg:#052e16}}
*{box-sizing:border-box}
body{margin:0;background:var(--bg);color:var(--fg);font-family:ui-sans-serif,system-ui,-apple-system,"Segoe UI",Roboto,Arial,sans-serif;-webkit-font-smoothing:antialiased}
.wrap{max-width:540px;margin:0 auto;padding:32px 16px}
.narrow{max-width:400px;padding-top:12vh}
header{display:flex;align-items:flex-start;justify-content:space-between;gap:12px;margin-bottom:20px}
h1{font-size:22px;font-weight:600;margin:0;letter-spacing:-.02em}
.sub{color:var(--muted);font-size:14px;margin:4px 0 0}
.card{background:var(--card);border:1px solid var(--border);border-radius:12px;padding:20px;box-shadow:0 1px 2px rgba(0,0,0,.04)}
.card+.card{margin-top:16px}
label{display:block;font-size:14px;font-weight:500;margin-bottom:6px}
.field{margin-bottom:16px}
input,select{width:100%;height:40px;padding:0 12px;border:1px solid var(--border);border-radius:8px;background:transparent;color:var(--fg);font:inherit;font-size:14px;outline:none;transition:border-color .15s,box-shadow .15s}
select{appearance:none;-webkit-appearance:none;padding-right:36px;background-image:url("data:image/svg+xml,%3Csvg xmlns='http://www.w3.org/2000/svg' width='16' height='16' viewBox='0 0 24 24' fill='none' stroke='%2371717a' stroke-width='2' stroke-linecap='round' stroke-linejoin='round'%3E%3Cpath d='m6 9 6 6 6-6'/%3E%3C/svg%3E");background-repeat:no-repeat;background-position:right 12px center}
option{background:var(--card);color:var(--fg)}
input::placeholder{color:var(--muted)}
input:focus,select:focus{border-color:var(--ring);box-shadow:0 0 0 3px rgba(161,161,170,.25)}
.hint{display:flex;justify-content:space-between;font-size:12px;color:var(--muted);margin-top:6px}
.btn{width:100%;height:44px;border:0;border-radius:8px;background:var(--primary);color:var(--pfg);font:inherit;font-size:15px;font-weight:500;cursor:pointer;display:flex;align-items:center;justify-content:center;gap:8px;transition:opacity .15s}
.btn:hover{opacity:.9}.btn:disabled{opacity:.5;cursor:not-allowed}
.btn svg{width:18px;height:18px}
.badge{display:inline-flex;align-items:center;gap:6px;font-size:12px;font-weight:500;padding:4px 10px;border-radius:999px;border:1px solid var(--border);color:var(--muted);white-space:nowrap}
.dot{width:8px;height:8px;border-radius:50%;background:var(--muted)}
.badge.ok{color:var(--ok)}.badge.ok .dot{background:var(--ok)}
.badge.on{color:var(--danger);border-color:var(--danger);background:var(--dbg)}.badge.on .dot{background:var(--danger);animation:p 1s infinite}
@keyframes p{50%{opacity:.25}}
.toast{display:none;margin-top:12px;font-size:14px;padding:10px 12px;border-radius:8px}
.toast.show{display:block}.toast.ok{background:var(--obg);color:var(--ok)}.toast.err{background:var(--dbg);color:var(--danger)}
.err{margin:0 0 16px;font-size:14px;padding:10px 12px;border-radius:8px;background:var(--dbg);color:var(--danger)}
.rh{display:flex;justify-content:space-between;align-items:center;margin-bottom:4px}
h2{font-size:15px;font-weight:600;margin:0}
.stat{font-size:12px;color:var(--muted)}
ul{list-style:none;margin:0;padding:0}
li{display:flex;justify-content:space-between;align-items:flex-start;gap:12px;padding:12px 0;border-top:1px solid var(--border);font-size:14px}
li:first-child{border-top:0}
.m{font-weight:500;word-break:break-word}.l{color:var(--muted);font-size:13px;margin-top:2px}.t{color:var(--muted);font-size:12px;white-space:nowrap}
.empty{color:var(--muted);justify-content:center}
.clr{background:none;border:1px solid var(--border);color:var(--fg);border-radius:8px;height:30px;padding:0 10px;font:inherit;font-size:12px;cursor:pointer;display:none}
.clr.show{display:inline-block}
footer{display:flex;justify-content:center;gap:12px;color:var(--muted);font-size:12px;margin-top:20px}
footer a{color:var(--muted)}
)rawliteral";

const char LOGIN_BODY_TOP[] = R"rawliteral(<div class="wrap narrow"><div class="card">
<h1>Call station</h1><p class="sub" style="margin-bottom:20px">Enter the password to continue.</p>
<form method="POST" action="/login">
)rawliteral";

const char LOGIN_BODY_BOTTOM[] = R"rawliteral(<div class="field"><label for="p">Password</label>
<input id="p" name="password" type="password" autocomplete="current-password" autofocus required></div>
<button class="btn" type="submit">Sign in</button>
</form></div></div></body></html>
)rawliteral";

const char MAIN_BODY_TOP[] = R"rawliteral(<div class="wrap">
<header><div><h1>Call station</h1><p class="sub">Send a call to the floor buzzer and display.</p></div>
<span id="badge" class="badge"><span class="dot"></span><span id="bt">Connecting</span></span></header>
<form class="card" id="f" autocomplete="off">
<div class="field"><label for="snd">From</label>
<select id="snd"><option value="" disabled selected>Select who is calling</option>
)rawliteral";

const char MAIN_BODY_BOTTOM[] = R"rawliteral(</select></div>
<div class="field"><label for="loc">Location</label>
<input id="loc" maxlength="20" placeholder="e.g. Paint Shop">
<div class="hint"><span>Where the call should go.</span><span id="lc">0/20</span></div></div>
<div class="field"><label for="msg">Message</label>
<input id="msg" maxlength="40" placeholder="Please come to the control room">
<div class="hint"><span>Shown on the HMI. English letters only.</span><span id="cc">0/40</span></div></div>
<button class="btn" id="b" type="submit"><svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M6 8a6 6 0 0 1 12 0c0 7 3 9 3 9H3s3-2 3-9"/><path d="M10.3 21a1.94 1.94 0 0 0 3.4 0"/></svg><span id="bl">Ring</span></button>
<div class="toast" id="toast"></div>
</form>
<div class="card"><div class="rh"><h2>Recent calls</h2><div><span class="stat" id="count"></span> <button class="clr" id="clr" type="button">Clear active call</button></div></div>
<ul id="recent"><li class="empty">No calls yet</li></ul></div>
<footer><span>Portenta H7 on the plant network</span><a href="/logout">Sign out</a></footer>
</div>
<script>
const $=i=>document.getElementById(i);
const msg=$('msg'),loc=$('loc'),snd=$('snd'),b=$('b'),toast=$('toast');let tt;
msg.addEventListener('input',()=>{$('cc').textContent=msg.value.length+'/40'});
loc.addEventListener('input',()=>{$('lc').textContent=loc.value.length+'/20'});
function show(t,ok){toast.textContent=t;toast.className='toast show '+(ok?'ok':'err');clearTimeout(tt);tt=setTimeout(()=>toast.className='toast',5000)}
function ago(s){if(s<60)return 'Just now';if(s<3600)return Math.floor(s/60)+' min ago';return Math.floor(s/3600)+' h ago'}
function esc(s){const d=document.createElement('div');d.textContent=s;return d.innerHTML}
async function api(u){const r=await fetch(u,{credentials:'same-origin'});if(r.status===401){window.location.href='/login';throw new Error('auth')}return r.json()}
$('f').addEventListener('submit',async e=>{e.preventDefault();
if(snd.value===''){show('Select who is calling.',false);snd.focus();return}
if(!loc.value.trim()){show('Enter a location.',false);loc.focus();return}
b.disabled=true;$('bl').textContent='Sending';
try{const j=await api('/ring?snd='+snd.value+'&loc='+encodeURIComponent(loc.value)+'&msg='+encodeURIComponent(msg.value));
if(j.ok){show('Call sent to '+loc.value.trim()+'.',true);msg.value='';$('cc').textContent='0/40';poll()}else show(j.error||'Call failed. Try again.',false)}
catch(err){if(err.message!=='auth')show("Couldn't reach the device. Check the network.",false)}
b.disabled=false;$('bl').textContent='Ring'});
$('clr').addEventListener('click',async()=>{try{await api('/clear');poll()}catch(e){}});
async function poll(){try{const j=await api('/status');
$('badge').className='badge '+(j.active?'on':'ok');$('bt').textContent=j.active?'Call active':'Online';
$('clr').className='clr'+(j.active?' show':'');
$('count').textContent=j.count+' total';
const ul=$('recent');ul.innerHTML=j.recent.length?j.recent.map(c=>'<li><div><div class="m">'+esc(c.msg)+'</div><div class="l">'+esc(c.loc)+' &middot; from '+esc(c.snd)+'</div></div><span class="t">'+ago(c.age)+'</span></li>').join(''):'<li class="empty">No calls yet</li>'}
catch(e){if(e.message!=='auth'){$('badge').className='badge';$('bt').textContent='Offline'}}}
poll();setInterval(poll,2000);
</script></body></html>
)rawliteral";

// ---------------- Helpers ----------------

String urlDecode(const String& s) {
  String o;
  o.reserve(s.length());
  for (unsigned i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c == '+') o += ' ';
    else if (c == '%' && i + 2 < s.length()) {
      char h[3] = { s[i + 1], s[i + 2], 0 };
      o += (char)strtol(h, nullptr, 16);
      i += 2;
    } else o += c;
  }
  return o;
}

String getParam(const String& query, const char* key) {
  String k = String(key) + "=";
  int start = 0;
  while (start < (int)query.length()) {
    int end = query.indexOf('&', start);
    if (end < 0) end = query.length();
    if (query.substring(start, start + k.length()) == k)
      return urlDecode(query.substring(start + k.length(), end));
    start = end + 1;
  }
  return "";
}

// Keep plain printable ASCII only (what the HMI can show), limit length
String cleanText(const String& in, int maxLen) {
  String o;
  for (unsigned i = 0; i < in.length() && (int)o.length() < maxLen; i++) {
    char c = in[i];
    if (c >= 32 && c <= 126) o += c;
  }
  o.trim();
  return o;
}

String jsonEsc(const String& s) {
  String o;
  for (unsigned i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c == '"' || c == '\\') o += '\\';
    o += c;
  }
  return o;
}

// 2 characters per register, padded with spaces
void writeText(int startReg, int numRegs, const String& s) {
  for (int i = 0; i < numRegs; i++) {
    uint8_t first  = (2 * i < (int)s.length())     ? (uint8_t)s[2 * i]     : ' ';
    uint8_t second = (2 * i + 1 < (int)s.length()) ? (uint8_t)s[2 * i + 1] : ' ';
    uint16_t value = TEXT_HIGH_BYTE_FIRST ? ((first << 8) | second) : ((second << 8) | first);
    modbusTCP.holdingRegisterWrite(startReg + i, value);
  }
}

void addRecent(const String& msg, const String& loc, const String& snd) {
  int last = (recentCount < RECENT_MAX) ? recentCount : RECENT_MAX - 1;
  for (int i = last; i > 0; i--) recent[i] = recent[i - 1];
  recent[0].msg = msg;
  recent[0].loc = loc;
  recent[0].snd = snd;
  recent[0].at = millis();
  if (recentCount < RECENT_MAX) recentCount++;
}

void makeSessionToken() {
  randomSeed(micros() ^ ((uint32_t)analogRead(A0) << 16) ^ analogRead(A1));
  sessionToken = "";
  for (int i = 0; i < 4; i++) sessionToken += String((uint32_t)random(0x7FFFFFFF), HEX);
}

// ---------------- Call logic ----------------

void startCall(const String& msg, const String& loc, int sndIndex) {
  String snd = SENDERS[sndIndex];
  callCount++;

  writeText(REG_MSG, MSG_MAX / 2, msg);
  writeText(REG_LOC, LOC_MAX / 2, loc);
  writeText(REG_SND, SND_MAX / 2, snd);
  modbusTCP.holdingRegisterWrite(REG_SNDNUM, sndIndex + 1);
  modbusTCP.holdingRegisterWrite(REG_COUNT, callCount);
  modbusTCP.holdingRegisterWrite(REG_ACTIVE, 1);

  buzzerActive = true;
  callActive = true;
  callStart = millis();

  if (USE_HMI_BEEP)     hmiBuzzer(true);
  if (USE_RELAY_BUZZER) relayBuzzer(true);
  ledGreen(true);

  addRecent(msg, loc, snd);
  Serial.print("CALL  from ");
  Serial.print(snd);
  Serial.print("  to ");
  Serial.print(loc);
  Serial.print("  |  ");
  Serial.println(msg);
}

void stopCall() {
  allBuzzersOff();
  buzzerActive = false;
  callActive = false;
  ledGreen(false);
  modbusTCP.holdingRegisterWrite(REG_ACTIVE, 0);
}

void updateCall() {
  if (!callActive) return;

  // OK/CLEAR pressed on the HMI (wrote 0 to 400001) -> stop everything
  if (modbusTCP.holdingRegisterRead(REG_ACTIVE) == 0) {
    stopCall();
    Serial.println("Call cleared from HMI");
    return;
  }

  unsigned long elapsed = millis() - callStart;

  // Optional safety limit for the whole call (0 = no limit)
  if (DISPLAY_MS > 0 && elapsed >= DISPLAY_MS) {
    stopCall();
    return;
  }

  if (buzzerActive) {
    if (BUZZER_MS > 0 && elapsed >= BUZZER_MS) {
      allBuzzersOff();
      buzzerActive = false;
      ledGreen(false);
    } else {
      bool pattern = ringPatternOn(elapsed);
      if (USE_HMI_BEEP)     hmiBuzzer(HMI_BEEP_PATTERN ? pattern : true);
      if (USE_RELAY_BUZZER) relayBuzzer(RELAY_RING_PATTERN ? pattern : true);
      ledGreen(pattern);
    }
  }
}

// ---------------- Modbus TCP (non-blocking, keeps the HMI connection open) ----------------

void handleModbus() {
  EthernetClient newClient = modbusServer.available();
  if (newClient) {
    bool sameConnection = modbusClient &&
                          modbusClient.remoteIP() == newClient.remoteIP() &&
                          modbusClient.remotePort() == newClient.remotePort();
    if (!sameConnection) {
      if (modbusClient) modbusClient.stop();
      modbusClient = newClient;
      modbusTCP.accept(modbusClient);
      Serial.print("HMI connected from ");
      Serial.println(modbusClient.remoteIP());
    }
  }
  if (modbusClient && modbusClient.connected()) {
    modbusTCP.poll();
  }
}

// ---------------- HTTP ----------------

struct Request {
  String method;
  String path;
  String query;
  String cookie;
  String body;
};

bool readRequest(EthernetClient& c, Request& r) {
  c.setTimeout(300);
  String line = c.readStringUntil('\n');   // e.g. "GET /ring?loc=..&msg=.. HTTP/1.1"
  line.trim();
  int sp1 = line.indexOf(' ');
  int sp2 = line.indexOf(' ', sp1 + 1);
  if (sp1 <= 0 || sp2 <= sp1) return false;

  r.method = line.substring(0, sp1);
  String target = line.substring(sp1 + 1, sp2);
  int q = target.indexOf('?');
  r.path  = (q >= 0) ? target.substring(0, q) : target;
  r.query = (q >= 0) ? target.substring(q + 1) : "";

  // Headers
  int contentLength = 0;
  while (true) {
    String h = c.readStringUntil('\n');
    h.trim();
    if (h.length() == 0) break;
    String hl = h;
    hl.toLowerCase();
    if (hl.startsWith("cookie:")) r.cookie = h.substring(7);
    else if (hl.startsWith("content-length:")) contentLength = h.substring(15).toInt();
  }

  // Body (only small form posts, e.g. the login password)
  if (contentLength > 0 && contentLength <= 512) {
    char buf[513];
    size_t n = c.readBytes(buf, contentLength);
    buf[n] = 0;
    r.body = String(buf);
  }
  return true;
}

bool isLoggedIn(const Request& r) {
  return r.cookie.indexOf("cs_session=" + sessionToken) >= 0;
}

void sendHeader(EthernetClient& c, int code, const char* type) {
  c.print("HTTP/1.1 ");
  c.print(code);
  if (code == 200) c.println(" OK");
  else if (code == 400) c.println(" Bad Request");
  else if (code == 401) c.println(" Unauthorized");
  else c.println(" Not Found");
  c.print("Content-Type: ");
  c.println(type);
  c.println("Cache-Control: no-store");
  c.println("Connection: close");
  c.println();
}

void sendRedirect(EthernetClient& c, const char* location, const String& setCookie) {
  c.println("HTTP/1.1 302 Found");
  c.print("Location: ");
  c.println(location);
  if (setCookie.length()) {
    c.print("Set-Cookie: ");
    c.println(setCookie);
  }
  c.println("Cache-Control: no-store");
  c.println("Connection: close");
  c.println();
}

void sendRaw(EthernetClient& c, const char* p) {
  size_t n = strlen(p);
  while (n > 0) {
    size_t k = n > 512 ? 512 : n;
    c.write((const uint8_t*)p, k);
    p += k;
    n -= k;
  }
}

void sendPageStart(EthernetClient& c, const char* title) {
  sendHeader(c, 200, "text/html; charset=utf-8");
  sendRaw(c, "<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\"><meta name=\"viewport\" content=\"width=device-width,initial-scale=1\"><title>");
  sendRaw(c, title);
  sendRaw(c, "</title><style>");
  sendRaw(c, CSS);
  sendRaw(c, "</style></head><body>");
}

void sendLoginPage(EthernetClient& c, bool wrongPassword) {
  sendPageStart(c, "Sign in - Call station");
  sendRaw(c, LOGIN_BODY_TOP);
  if (wrongPassword) sendRaw(c, "<p class=\"err\">Wrong password. Try again.</p>");
  sendRaw(c, LOGIN_BODY_BOTTOM);
}

void sendMainPage(EthernetClient& c) {
  sendPageStart(c, "Call station");
  sendRaw(c, MAIN_BODY_TOP);
  for (int i = 0; i < SENDER_COUNT; i++) {
    c.print("<option value=\"");
    c.print(i);
    c.print("\">");
    c.print(SENDERS[i]);
    c.println("</option>");
  }
  sendRaw(c, MAIN_BODY_BOTTOM);
}

void sendStatus(EthernetClient& c) {
  String j = "{\"active\":";
  j += callActive ? "true" : "false";
  j += ",\"count\":";
  j += callCount;
  j += ",\"recent\":[";
  for (int i = 0; i < recentCount; i++) {
    if (i) j += ",";
    j += "{\"msg\":\"" + jsonEsc(recent[i].msg) +
         "\",\"loc\":\"" + jsonEsc(recent[i].loc) +
         "\",\"snd\":\"" + jsonEsc(recent[i].snd) + "\",\"age\":";
    j += (millis() - recent[i].at) / 1000;
    j += "}";
  }
  j += "]}";
  sendHeader(c, 200, "application/json");
  c.print(j);
}

void handleWeb() {
  EthernetClient client = server.available();
  if (!client) return;

  Request r;
  if (!readRequest(client, r)) {
    client.stop();
    return;
  }

  bool loggedIn = isLoggedIn(r);

  // ----- Pages that work without logging in -----
  if (r.path == "/login") {
    if (r.method == "POST") {
      String pw = getParam(r.body, "password");
      if (pw == PASSWORD) {
        sendRedirect(client, "/", "cs_session=" + sessionToken + "; Path=/; HttpOnly; SameSite=Strict");
        Serial.print("Login OK from ");
        Serial.println(client.remoteIP());
      } else {
        sendLoginPage(client, true);
        Serial.print("Wrong password from ");
        Serial.println(client.remoteIP());
      }
    } else if (loggedIn) {
      sendRedirect(client, "/", "");
    } else {
      sendLoginPage(client, false);
    }
  } else if (r.path == "/logout") {
    sendRedirect(client, "/login", "cs_session=; Path=/; Max-Age=0");
  } else if (r.path == "/favicon.ico") {
    sendHeader(client, 404, "text/plain");

  // ----- Everything else needs a login -----
  } else if (!loggedIn) {
    if (r.path == "/") {
      sendRedirect(client, "/login", "");
    } else {
      sendHeader(client, 401, "application/json");
      client.print("{\"ok\":false,\"error\":\"Please sign in again.\"}");
    }

  } else if (r.path == "/") {
    sendMainPage(client);

  } else if (r.path == "/ring") {
    String sndStr = getParam(r.query, "snd");
    int sndIndex = sndStr.length() ? sndStr.toInt() : -1;
    String loc = cleanText(getParam(r.query, "loc"), LOC_MAX);
    String msg = cleanText(getParam(r.query, "msg"), MSG_MAX);

    if (sndIndex < 0 || sndIndex >= SENDER_COUNT) {
      sendHeader(client, 400, "application/json");
      client.print("{\"ok\":false,\"error\":\"Select who is calling.\"}");
    } else if (loc.length() == 0) {
      sendHeader(client, 400, "application/json");
      client.print("{\"ok\":false,\"error\":\"Enter a location (English letters only).\"}");
    } else {
      if (msg.length() == 0) msg = "Please respond";
      startCall(msg, loc, sndIndex);
      sendHeader(client, 200, "application/json");
      client.print("{\"ok\":true}");
    }

  } else if (r.path == "/clear") {
    stopCall();
    sendHeader(client, 200, "application/json");
    client.print("{\"ok\":true}");

  } else if (r.path == "/status") {
    sendStatus(client);

  } else {
    sendHeader(client, 404, "text/plain");
    client.print("Not found");
  }

  delay(2);
  client.stop();
}

// ---------------- Setup / loop ----------------

void setup() {
  digitalWrite(RELAY_PIN, RELAY_OFF);   // relay OFF before enabling the output
  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, RELAY_OFF);

  Serial.begin(115200);
  pinMode(LEDG, OUTPUT);
  pinMode(LEDR, OUTPUT);
  pinMode(LEDB, OUTPUT);
  ledGreen(false);
  ledRed(false);
  ledBlue(true);

  bool ok;
  if (USE_DHCP) {
    ok = Ethernet.begin(mac, 30000) != 0;
  } else {
    Ethernet.begin(mac, staticIp, dns, gateway, subnet);
    ok = true;
  }
  if (Ethernet.hardwareStatus() == EthernetNoHardware) ok = false;
  networkUp = ok;

  makeSessionToken();

  // Modbus TCP server (unit ID 1)
  if (!modbusTCP.begin(1)) Serial.println("Failed to start Modbus TCP server");
  modbusTCP.configureHoldingRegisters(0, REG_TOTAL);
  for (int i = 0; i < REG_TOTAL; i++) modbusTCP.holdingRegisterWrite(i, 0);
  writeText(REG_MSG, MSG_MAX / 2, "");
  writeText(REG_LOC, LOC_MAX / 2, "");
  writeText(REG_SND, SND_MAX / 2, "");

  ledBlue(false);

  if (networkUp) {
    server.begin();
    modbusServer.begin();
    Serial.print("Ready. Open http://");
    Serial.print(Ethernet.localIP());
    Serial.println("/   (HMI: Modbus TCP port 502)");
  } else {
    Serial.println("No network. USB test mode.");
  }
  Serial.println("Type 'r' in the Serial Monitor for a test call.");
}

void loop() {
  if (networkUp) handleModbus();
  updateCall();

  ledRed(!networkUp || Ethernet.linkStatus() != LinkON);

  if (Serial.available()) {
    char c = Serial.read();
    if (c == 'r' || c == 'R') startCall("Test call", "Test location", 0);
  }

  static unsigned long lastPrint = 0;
  if (millis() - lastPrint > 5000) {
    lastPrint = millis();
    if (networkUp) { Serial.print("IP address: "); Serial.println(Ethernet.localIP()); }
    else Serial.println("USB test mode (no network) - type 'r' for a test call");
  }

  if (networkUp) handleWeb();
}
